# Design Plan: Formal Interceptor Support for the net_http Server

## Context

The `net_http` libevent-based HTTP server (`net_http/server/`) currently supports
only a request **handler** per URI: an exact-path `RequestHandler` map plus ordered
`RequestDispatcher` fallbacks (see `net_http/server/internal/evhttp_server.cc`,
`DispatchEvRequest`). There is no first-class way to run **cross-cutting logic**
(auth, logging, metrics, CORS, header injection) independently of the business-logic
handler, keyed off the request URL path.

This plan adds **formal interceptor support**: application code registers
interceptors keyed off the request URL path; the matched interceptor runs **before**
the handler (pre-hook, may short-circuit) and **after** the response is sent
(post-hook, for timing / metrics / logging). The server is atomic request/response
today, and the interceptor model is atomic too — no streaming semantics are introduced.

### Design decisions

- **Hook points:** Pre + Post.
- **Matching:** mirrors the existing request-dispatching methods — **exact path
  matching** plus an ordered **interceptor dispatcher** predicate. No prefix matching.
- **API shape:** `std::function` typedefs, consistent with `RequestHandler` /
  `RequestDispatcher`.
- **Non-blocking:** interceptors must be non-blocking, same contract as `RequestHandler`.

## Public API (`net_http/server/public/httpserver_interface.h`)

```cpp
// Controls whether the request proceeds to the handler after the pre-hook.
enum class InterceptResult {
  kContinue,  // proceed to the request handler
  kExit,      // stop; this interceptor has completed the response itself
};

// Pre-handler hook. Runs on the executor thread before the request handler.
//
// MUST be non-blocking (same contract as RequestHandler); long work must be
// offloaded to an application-managed thread pool.
//
// May inspect/modify the request, add response headers, or short-circuit by
// completing the response and returning kExit.
//
// Contract: an interceptor that returns kExit MUST complete the response
// (Reply()/ReplyWithStatus()/Abort()), exactly like a handler must.
typedef std::function<InterceptResult(ServerRequestInterface*)> RequestInterceptor;

// Post-handler hook. Runs after the response has been sent.
//
// MUST be non-blocking. The response is already flushed, so this hook is
// read-only w.r.t. the response body; use it for logging, metrics, and timing.
// The final response status is available via ServerRequestInterface::response_status().
typedef std::function<void(ServerRequestInterface*)> ResponseInterceptor;

// A pre/post interceptor pair. Either hook may be nullptr (pre-only or post-only).
struct Interceptor {
  RequestInterceptor on_request;
  ResponseInterceptor on_response;
};

// Application-provided interceptor dispatching logic, mirroring RequestDispatcher.
// Returns an Interceptor with both hooks null if it does not apply to the request.
typedef std::function<Interceptor(ServerRequestInterface*)> RequestInterceptorDispatcher;
```

New pure-virtuals on `HTTPServerInterface`, mirroring `RegisterRequestHandler` /
`RegisterRequestDispatcher`:

```cpp
// Registers an interceptor with exact URI path matching. Any existing interceptor
// under the same uri is overwritten. Either hook may be nullptr. Interceptors may
// be registered after the server has been started.
virtual void RegisterRequestInterceptor(
    absl::string_view uri,
    RequestInterceptor request_interceptor,
    ResponseInterceptor response_interceptor) = 0;

// Registers an interceptor dispatcher. For a request with no exact-path interceptor
// match, dispatchers are invoked in registration order; the first to return an
// applicable Interceptor wins.
virtual void RegisterRequestInterceptorDispatcher(
    RequestInterceptorDispatcher dispatcher) = 0;
```

### Change to `ServerRequestInterface` (`server_request_interface.h`)

Add a **method** (no data members, per the interface's contract) so post-hooks can
observe the outcome:

```cpp
// Returns the response status. Defaults to HTTPStatusCode::OK until set via
// ReplyWithStatus()/PartialReplyWithStatus(); after the response is sent this
// returns the actual status, so post-handler interceptors can record it.
virtual HTTPStatusCode response_status() const = 0;
```

All other interceptor behavior reuses existing methods (`GetRequestHeader`,
`OverwriteResponseHeader`, `Reply`, `Abort`, etc.).

## Implementation

### 1. `net_http/server/public/httpserver_interface.h`

- Add `InterceptResult`, `RequestInterceptor`, `ResponseInterceptor`, `Interceptor`,
  and `RequestInterceptorDispatcher` near the existing `RequestHandler` /
  `RequestDispatcher` typedefs.
- Add the `RegisterRequestInterceptor` and `RegisterRequestInterceptorDispatcher`
  pure virtuals to `HTTPServerInterface`.

### 2. `net_http/server/public/server_request_interface.h`

- Add the `response_status()` pure-virtual accessor.

### 3. `net_http/server/internal/evhttp_server.h`

- Add an `InterceptorInfo` struct (mirrors `UriHandlerInfo` / `DispatcherInfo`) holding
  the exact `uri` and an `Interceptor`.
- Add, guarded by `request_mu_`:
  `std::unordered_map<std::string, Interceptor> interceptor_handlers_;` and
  `std::vector<RequestInterceptorDispatcher> interceptor_dispatchers_;`
- Declare the two `Register...` overrides.
- Add a private helper `Interceptor ResolveInterceptor(EvHTTPRequest*)` (exact map,
  then dispatchers first-match) and a helper to schedule the pre-hook + handler.

### 4. `net_http/server/internal/evhttp_server.cc`

- Implement `RegisterRequestInterceptor` (overwrite semantics like
  `RegisterRequestHandler`) and `RegisterRequestInterceptorDispatcher` (append, like
  `RegisterRequestDispatcher`).
- In `DispatchEvRequest`, while holding `request_mu_` and after the handler is resolved:
  - `ResolveInterceptor(...)` for the request: exact-path interceptor for the path, else
    the first interceptor dispatcher that returns an applicable `Interceptor`.
  - Attach the post-hook to the request via a new
    `ev_request->SetResponseInterceptor(interceptor.on_response)`.
  - If there is a pre-hook, schedule a lambda that runs `on_request(request)`; on
    `kExit` it returns without calling the handler (the interceptor owns completion),
    otherwise it calls `handler(request)`. With no pre-hook, keep the current fast path
    (`ScheduleHandlerReference` / `ScheduleHandler`) unchanged.

### 5. `net_http/server/internal/evhttp_request.{h,cc}`

- Add members: `ResponseInterceptor response_interceptor_;` and
  `HTTPStatusCode response_status_ = HTTPStatusCode::OK;`
- Add `void SetResponseInterceptor(ResponseInterceptor interceptor);` and override
  `HTTPStatusCode response_status() const`.
- Record the status in `ReplyWithStatus` (and set the abort status in `Abort`).
- In `EvSendReply`: after `evhttp_send_reply(...)` and before `DecOps()` / `delete this`,
  invoke `response_interceptor_` if set.
- In `Abort`: likewise invoke the post-hook after `evhttp_send_error(...)` so
  metrics/logging observe aborted requests too.

### Execution / threading notes

- The pre-hook and the handler run in the **same** executor task (executor thread),
  preserving the non-blocking contract. Function objects are copied into the scheduled
  lambda and into the request, so there is no dependence on the interceptor registries'
  lifetime.
- The post-hook runs inside `EvSendReply` / `Abort`, which already run on the executor
  (`ReplyWithStatus` -> `server_->ScheduleReply`), while the request object is still alive.
- `IncOps` / `DecOps` accounting is unchanged: one `IncOps` at dispatch, one `DecOps` at
  reply/abort. A pre-hook that returns `kExit` must complete the response to keep this
  balanced (documented contract).

### Known v1 limitations (documented, not blocking)

- At most one interceptor applies per request (exact-path match, else first matching
  dispatcher) — mirroring request dispatching. Composing multiple interceptors into a
  chain is a possible future extension.
- No per-request context bag to pass arbitrary state from the pre- to the post-hook.
  Cross-request-safe timing would need a future request-scoped context; noted as a
  follow-up.

## BUILD

Interceptor types live in `httpserver_interface.h` and the new accessor in
`server_request_interface.h` (both already in the `http_server_api` target,
`net_http/server/public/BUILD`); implementation edits are in the existing
`evhttp_server` target sources. A new example binary is added to
`net_http/server/testing/BUILD` (see below).

## Tests (`net_http/server/internal/evhttp_server_test.cc`)

Add `TEST_F(EvHTTPServerTest, ...)` cases following existing patterns (e.g.
`ExactPathMatching`), driving requests via `TestEvHTTPConnection`:

- Pre-hook runs and mutates a response header, then the handler replies.
- Pre-hook short-circuits (returns `kExit`, replies 401) — the handler never runs.
- Post-hook runs after a normal reply and observes `response_status()` (assert via a
  flag/counter set from the hook and read under a mutex / notification).
- Exact-path matching: an interceptor on `/api/v1/x` fires only for that exact path.
- Interceptor dispatcher: a dispatcher-provided interceptor applies when there is no
  exact-path match, and dispatchers are consulted in registration order (first wins).
- Pre-only and post-only registrations (nullptr for the other hook) behave correctly.

## Example (`net_http/docs/interceptors/example.md`)

An end-to-end example — a small server binary
(`net_http/server/testing/evhttp_interceptor_server.cc`) that registers a logging /
timing interceptor plus an auth short-circuit interceptor — with build/run steps and
`curl` walkthroughs. See [`example.md`](./example.md).

## Verification

- Build: `bazel build //net_http/server/...`
- Unit tests: `bazel test //net_http/server/internal:evhttp_server_test`
- End-to-end: build and run `//net_http/server/testing:evhttp_interceptor_server` and
  follow [`example.md`](./example.md) to confirm the pre-hook, short-circuit, and
  post-hook (including `response_status()`) behave as expected.
