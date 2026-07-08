# Design Plan: Formal Interceptor Support for the net_http Server

## Context

The `net_http` libevent-based HTTP server (`net_http/server/`) currently supports
only a request **handler** per URI: an exact-path `RequestHandler` map plus ordered
`RequestDispatcher` fallbacks (see `net_http/server/internal/evhttp_server.cc`,
`DispatchEvRequest`). There is no first-class way to run **cross-cutting logic**
(auth, logging, metrics, CORS, header injection) independently of the business-logic
handler, keyed off the request URL path.

This plan adds **formal interceptor support**: application code registers
interceptors keyed off the request URL path. For a given request, **all** matching
interceptors form a **chain** — each interceptor's pre-hook runs **before** the
handler (in registration order, and may short-circuit), and each post-hook runs
**after** the response is sent (in reverse order, an onion model), for
timing / metrics / logging. The server is atomic request/response today, and the
interceptor model is atomic too — no streaming semantics are introduced.

### Design decisions

- **Hook points:** Pre + Post.
- **Matching:** mirrors the existing request-dispatching methods — **exact path
  matching** plus an ordered **interceptor dispatcher** predicate. No prefix matching.
- **Chaining:** multiple interceptors may apply to a request; they compose as a chain
  (pre in registration order, post in reverse order).
- **API shape:** `std::function` typedefs, consistent with `RequestHandler` /
  `RequestDispatcher`.
- **Non-blocking:** interceptors must be non-blocking, same contract as `RequestHandler`.

## Public API (`net_http/server/public/httpserver_interface.h`)

```cpp
// Controls whether the interceptor chain (and then the handler) continues.
enum class InterceptResult {
  kContinue,  // proceed to the next interceptor in the chain, then the handler
  kExit,      // stop the chain and skip the handler; this interceptor has
              // completed the response itself
};

// Pre-handler hook. Runs on the executor thread before the request handler,
// in interceptor registration order.
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

// Post-handler hook. Runs after the response has been sent, in reverse
// registration order (onion model).
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
// Registers an interceptor with exact URI path matching. Unlike request handlers,
// interceptors registered under the same uri are NOT overwritten — they accumulate
// and run as a chain in registration order. Either hook may be nullptr. Interceptors
// may be registered after the server has been started.
virtual void RegisterRequestInterceptor(
    absl::string_view uri,
    RequestInterceptor request_interceptor,
    ResponseInterceptor response_interceptor) = 0;

// Registers an interceptor dispatcher. For each request, every dispatcher is invoked
// in registration order; each that returns an applicable Interceptor contributes one
// link to the request's interceptor chain (after any exact-path interceptors).
virtual void RegisterRequestInterceptorDispatcher(
    RequestInterceptorDispatcher dispatcher) = 0;
```

### Chain semantics

For a request with path `p`, the interceptor **chain** is, in order:

1. all interceptors registered via `RegisterRequestInterceptor(p, ...)`, in
   registration order, then
2. for each registered interceptor dispatcher (in registration order), the
   `Interceptor` it returns for the request, if any.

Execution:

- **Pre-hooks** run in chain order. If any returns `kExit`, the remaining pre-hooks
  and the request handler are skipped (that interceptor has completed the response).
- **Post-hooks** run in **reverse** chain order after the response is sent, for every
  interceptor in the matched chain — including when the chain short-circuited, so an
  access-log / metrics post-hook still records short-circuited requests.

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

- Add, guarded by `request_mu_`:
  `std::unordered_map<std::string, std::vector<Interceptor>> interceptor_handlers_;`
  (exact path -> ordered chain segment) and
  `std::vector<RequestInterceptorDispatcher> interceptor_dispatchers_;`
- Declare the two `Register...` overrides.
- Add a private helper `std::vector<Interceptor> BuildInterceptorChain(EvHTTPRequest*)`
  that concatenates the exact-path chain and the applicable dispatcher interceptors.

### 4. `net_http/server/internal/evhttp_server.cc`

- Implement `RegisterRequestInterceptor` (append to the per-uri vector — chain, not
  overwrite) and `RegisterRequestInterceptorDispatcher` (append, like
  `RegisterRequestDispatcher`).
- In `DispatchEvRequest`, while holding `request_mu_` and after the handler is resolved:
  - `BuildInterceptorChain(...)` for the request.
  - Collect the chain's non-null post-hooks, `std::reverse` them, and attach to the
    request via a new `ev_request->SetResponseInterceptors(std::move(posts))`. This is
    set on the I/O thread before the executor task runs (and before any `Reply`), so
    `EvSendReply` sees the complete list — no race.
  - Collect the chain's non-null pre-hooks. If empty, keep the current fast path
    (`ScheduleHandlerReference` / `ScheduleHandler`) unchanged. If non-empty, schedule a
    lambda that runs each pre-hook in order; on `kExit` it returns without invoking the
    handler, otherwise it calls `handler(request)`.

### 5. `net_http/server/internal/evhttp_request.{h,cc}`

- Add members: `std::vector<ResponseInterceptor> response_interceptors_;` (already in
  reverse order) and `HTTPStatusCode response_status_ = HTTPStatusCode::OK;`
- Add `void SetResponseInterceptors(std::vector<ResponseInterceptor> interceptors);`,
  a private `void RunResponseInterceptors();`, and override
  `HTTPStatusCode response_status() const`.
- Record the status in `ReplyWithStatus` (and set the abort status in `Abort`).
- In `EvSendReply`: after `evhttp_send_reply(...)` and before `DecOps()` / `delete this`,
  call `RunResponseInterceptors()`.
- In `Abort`: likewise after `evhttp_send_error(...)` so metrics/logging observe
  aborted requests too.
- `ResponseInterceptor` comes from `httpserver_interface.h`, already included by
  `evhttp_request.h`.

### Execution / threading notes

- Pre-hooks and the handler run in the **same** executor task (executor thread),
  preserving the non-blocking contract. Function objects are copied into the scheduled
  lambda and into the request, so there is no dependence on the interceptor registries'
  lifetime.
- Post-hooks run inside `EvSendReply` / `Abort`, which already run on the executor
  (`ReplyWithStatus` -> `server_->ScheduleReply`), while the request object is still alive.
- `IncOps` / `DecOps` accounting is unchanged: one `IncOps` at dispatch, one `DecOps` at
  reply/abort. A pre-hook that returns `kExit` must complete the response to keep this
  balanced (documented contract).

### Known v1 limitations (documented, not blocking)

- No per-request context bag to pass arbitrary state from a pre-hook to its post-hook.
  Cross-request-safe timing would need a future request-scoped context; noted as a
  follow-up. (Interceptors that need shared state can carry it via captured, thread-safe
  application objects in the meantime.)

## BUILD

Interceptor types live in `httpserver_interface.h` and the new accessor in
`server_request_interface.h` (both already in the `http_server_api` target,
`net_http/server/public/BUILD`); implementation edits are in the existing
`evhttp_server` target sources. A new example binary is added to
`net_http/server/testing/BUILD` (see the example doc).

## Tests (`net_http/server/internal/evhttp_server_test.cc`)

Add `TEST_F(EvHTTPServerTest, ...)` cases following existing patterns (e.g.
`ExactPathMatching`), driving requests via `TestEvHTTPConnection`:

- Single pre-hook mutates a response header, then the handler replies.
- Chain of two exact-path interceptors on the same URI: pre-hooks run in registration
  order, post-hooks run in reverse order (assert observed ordering).
- Pre-hook short-circuits (returns `kExit`, replies 401) — the handler and later
  pre-hooks never run, but earlier/other post-hooks still run.
- Post-hook observes `response_status()` (assert via a flag/counter set from the hook and
  read under a mutex / notification).
- Exact-path + dispatcher composition: a dispatcher-provided interceptor runs after the
  exact-path chain; multiple dispatchers contribute in registration order.
- Pre-only and post-only registrations (nullptr for the other hook) behave correctly.

## Example (`net_http/docs/interceptors/example.md`)

An end-to-end example — a small server binary
(`net_http/server/testing/evhttp_interceptor_server.cc`) that registers a chain: an
access-log / timing post-hook applied to all paths (via a dispatcher) plus an auth
short-circuit pre-hook on `/secure` — with build/run steps and `curl` walkthroughs.
See [`example.md`](./example.md).

## Verification

- Build: `bazel build //net_http/server/...`
- Unit tests: `bazel test //net_http/server/internal:evhttp_server_test`
- End-to-end: build and run `//net_http/server/testing:evhttp_interceptor_server` and
  follow [`example.md`](./example.md) to confirm the chain — pre-hooks, short-circuit,
  and reverse-order post-hooks (including `response_status()`) — behaves as expected.
