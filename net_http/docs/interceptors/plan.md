# Design Plan: Formal Interceptor Support for the net_http Server

## Context

The `net_http` libevent-based HTTP server (`net_http/server/`) currently supports
only a request **handler** per URI: an exact-path `RequestHandler` map plus ordered
`RequestDispatcher` fallbacks (see `net_http/server/internal/evhttp_server.cc`,
`DispatchEvRequest`). There is no first-class way to run **cross-cutting logic**
(auth, logging, metrics, CORS, header injection) independently of the business-logic
handler, keyed off the request URL path.

This plan adds **formal interceptor support**: application code registers
interceptors against a URL **path prefix**; matched interceptors run **before** the
handler (pre-hook, may short-circuit) and **after** the response is sent (post-hook,
for timing / metrics / logging). The server is atomic request/response today, and the
interceptor model is atomic too — no streaming semantics are introduced.

### Design decisions

- **Hook points:** Pre + Post.
- **Matching:** URL path **prefix** (segment-aware); interceptors run in registration order.
- **API shape:** `std::function` typedefs, consistent with `RequestHandler` / `RequestDispatcher`.

## Public API (`net_http/server/public/httpserver_interface.h`)

```cpp
// Controls whether the interceptor chain (and then the handler) continues.
enum class InterceptResult {
  kContinue,  // proceed to the next interceptor, then the request handler
  kHalt,      // stop; this interceptor has completed the response itself
};

// Pre-handler hook. Matched by URL path prefix and run on the executor thread
// before the request handler. May inspect/modify the request, add response
// headers, or short-circuit by completing the response and returning kHalt.
//
// Contract: an interceptor that returns kHalt MUST complete the response
// (Reply()/ReplyWithStatus()/Abort()), exactly like a handler must.
typedef std::function<InterceptResult(ServerRequestInterface*)> RequestInterceptor;

// Post-handler hook. Run after the response has been sent, in reverse
// registration order (onion model). The response is already flushed, so this
// hook is read-only w.r.t. the response body/status; use it for logging,
// metrics, and timing.
typedef std::function<void(ServerRequestInterface*)> ResponseInterceptor;
```

New pure-virtual on `HTTPServerInterface`:

```cpp
// Registers an interceptor for all requests whose URI path is under
// uri_path_prefix (segment-aware prefix; "/" matches all). Either hook may be
// nullptr to register a pre-only or post-only interceptor. Interceptors may be
// registered after the server has been started.
virtual void RegisterRequestInterceptor(
    absl::string_view uri_path_prefix,
    RequestInterceptor request_interceptor,
    ResponseInterceptor response_interceptor) = 0;
```

No changes to `ServerRequestInterface` (honors its "do not add data members" note);
interceptors use existing methods (`GetRequestHeader`, `OverwriteResponseHeader`,
`Reply`, `Abort`, etc.).

## Implementation

### 1. `net_http/server/public/httpserver_interface.h`

- Add `InterceptResult`, `RequestInterceptor`, `ResponseInterceptor` typedefs near the
  existing `RequestHandler` / `RequestDispatcher` typedefs.
- Add the `RegisterRequestInterceptor` pure virtual to `HTTPServerInterface`.

### 2. `net_http/server/internal/evhttp_server.h`

- Add an `InterceptorInfo` struct (mirrors `UriHandlerInfo` / `DispatcherInfo`):
  `std::string uri_prefix; RequestInterceptor request_interceptor; ResponseInterceptor response_interceptor;`
- Add `std::vector<InterceptorInfo> interceptors_ ABSL_GUARDED_BY(request_mu_);`
- Declare `RegisterRequestInterceptor(...) override;`
- Add a private helper to schedule a handler wrapped by a pre-interceptor chain, e.g.
  `void ScheduleInterceptedHandler(std::vector<RequestInterceptor> pre, RequestHandler handler, EvHTTPRequest*)`.
- Add a private static segment-aware prefix-match helper.

### 3. `net_http/server/internal/evhttp_server.cc`

- Implement `RegisterRequestInterceptor` (lock `request_mu_`, `emplace_back`), matching
  the existing `RegisterRequestDispatcher` style.
- In `DispatchEvRequest`, while holding `request_mu_` and after the handler is resolved
  (exact map or dispatcher):
  - Walk `interceptors_` in order, collecting matching `pre` (`RequestInterceptor`) and
    `post` (`ResponseInterceptor`) function copies via the prefix-match helper.
  - `std::reverse(post)` for onion ordering; attach to the request via a new
    `ev_request->SetResponseInterceptors(std::move(post))`.
  - If `pre` is empty, keep the current fast path (`ScheduleHandlerReference` /
    `ScheduleHandler`) unchanged. If non-empty, use `ScheduleInterceptedHandler`, whose
    scheduled lambda runs each `pre(request)`; on `kHalt` it returns without calling the
    handler (the interceptor owns completion), otherwise it calls `handler(request)`.
- Prefix-match helper: match if `path == prefix`, or `prefix == "/"`, or `path` starts
  with `prefix` at a segment boundary (next char is `/`, or `prefix` ends in `/`). This
  prevents `/api` from matching `/apixyz`.

### 4. `net_http/server/internal/evhttp_request.{h,cc}`

- Add `std::vector<ResponseInterceptor> response_interceptors_;` plus
  `void SetResponseInterceptors(std::vector<ResponseInterceptor> interceptors);` and a
  private `void RunResponseInterceptors();`.
- In `EvSendReply`: after `evhttp_send_reply(...)` and before `DecOps()` / `delete this`,
  call `RunResponseInterceptors()`.
- In `Abort`: likewise run post-interceptors after `evhttp_send_error(...)` so
  metrics/logging observe aborted requests too.
- `response_interceptors_` type comes from `httpserver_interface.h`, already included by
  `evhttp_request.h`.

### Execution / threading notes

- Pre-interceptors and the handler run in the **same** executor task (executor thread),
  preserving the non-blocking contract. Function objects are copied into the scheduled
  lambda and into the request, so there is no dependence on `interceptors_` lifetime.
- Post-interceptors run inside `EvSendReply` / `Abort`, which already run on the executor
  (`ReplyWithStatus` -> `server_->ScheduleReply`), while the request object is still alive.
- `IncOps` / `DecOps` accounting is unchanged: one `IncOps` at dispatch, one `DecOps` at
  reply/abort. A halting pre-interceptor must complete the response to keep this balanced
  (documented contract).

### Known v1 limitations (documented, not blocking)

- Post-interceptors cannot read the final response status code via the public interface
  (no accessor exists). Candidate follow-up: expose response status or pass it to the hook.
- No per-request context bag to pass state from the pre- to the post-hook. Cross-request-safe
  timing would need a future request-scoped context; noted as a follow-up.

## BUILD

Interceptor types live in `httpserver_interface.h` (already in the `http_server_api`
target, `net_http/server/public/BUILD`); implementation edits are in the existing
`evhttp_server` target sources. No `BUILD` changes expected for the server targets.

## Tests (`net_http/server/internal/evhttp_server_test.cc`)

Add `TEST_F(EvHTTPServerTest, ...)` cases following existing patterns (e.g.
`ExactPathMatching`), driving requests via `TestEvHTTPConnection`:

- Pre-interceptor runs and mutates a response header, then the handler replies.
- Pre-interceptor short-circuits (returns `kHalt`, replies 401) — handler never runs.
- Post-interceptor runs after a normal reply (assert a side effect, e.g. a flag/counter
  set from the hook and observed via a mutex / notification).
- Prefix matching: an `/api` interceptor fires for `/api/v1/x` but not `/apix` or `/other`.
- Ordering: two interceptors on overlapping prefixes run in registration order (pre) and
  reverse order (post).
- Pre-only and post-only registrations (nullptr for the other hook) behave correctly.

## Verification

- Build: `bazel build //net_http/server/...`
- Unit tests: `bazel test //net_http/server/internal:evhttp_server_test`
- Optional manual smoke: extend/run `//net_http/server/testing:evhttp_echo_server` with a
  logging interceptor and `curl` a path to confirm pre/post fire and short-circuit works.
