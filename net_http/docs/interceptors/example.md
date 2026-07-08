# Interceptor Example: End-to-End Setup

This walks through building and running a small net_http server that uses
interceptors, and exercising it with `curl`. It demonstrates the three core
behaviors: a **pre-hook** that adds a header, a **pre-hook that short-circuits**
(auth reject), and a **post-hook** that logs the request and final response status.

The example binary is `net_http/server/testing/evhttp_interceptor_server.cc`, added
as part of the interceptor implementation (see [`design.md`](./design.md)).

## Prerequisites

Same toolchain as the rest of the repo (see the top-level `docs/README.md`):

- Bazel (`.bazelversion` pins the version used by this repo)
- A C++17 compiler, plus the system deps to build libevent/zlib as declared in
  `WORKSPACE`

## What the example registers

The example server wires up three endpoints and interceptors:

| URL path        | Handler                         | Interceptor                                            |
|-----------------|---------------------------------|--------------------------------------------------------|
| `/echo`         | echoes method + body            | post-hook: logs `method path -> status (N us)`         |
| `/secure`       | returns `"secret"`              | pre-hook: requires `Authorization` header, else `kExit` + 401 |
| everything else | 404                             | dispatcher interceptor: post-hook access log for all   |

Sketch of the registration (illustrative):

```cpp
// Post-hook access logger, applied to all paths via an interceptor dispatcher.
server->RegisterRequestInterceptorDispatcher(
    [](ServerRequestInterface* req) -> Interceptor {
      return Interceptor{
          /*on_request=*/nullptr,
          /*on_response=*/[](ServerRequestInterface* r) {
            NET_LOG(INFO, "%s %s -> %d",
                    std::string(r->http_method()).c_str(),
                    std::string(r->uri_path()).c_str(),
                    static_cast<int>(r->response_status()));
          }};
    });

// Auth gate on /secure: short-circuit with 401 when the header is missing.
server->RegisterRequestInterceptor(
    "/secure",
    /*request_interceptor=*/[](ServerRequestInterface* req) {
      if (req->GetRequestHeader("Authorization").empty()) {
        req->WriteResponseString("unauthorized");
        req->ReplyWithStatus(HTTPStatusCode::UNAUTHORIZED);
        return InterceptResult::kExit;   // handler never runs
      }
      return InterceptResult::kContinue;
    },
    /*response_interceptor=*/nullptr);
```

## Build

```
bazel build //net_http/server/testing:evhttp_interceptor_server
```

## Run (end-to-end)

**Terminal 1 — start the server** (binds an ephemeral port or one you pass):

```
./bazel-bin/net_http/server/testing/evhttp_interceptor_server 8080
```

**Terminal 2 — drive it with `curl`:**

```
# 1) Normal request: handler replies, post-hook logs "GET /echo -> 200 (… us)"
curl -i http://127.0.0.1:8080/echo

# 2) Short-circuit: no Authorization header -> pre-hook replies 401,
#    the /secure handler never runs
curl -i http://127.0.0.1:8080/secure

# 3) Authorized: pre-hook returns kContinue, handler runs, returns 200 "secret"
curl -i -H "Authorization: Bearer test" http://127.0.0.1:8080/secure
```

## What to verify

- Request (1) returns `200` and the server log shows the post-hook access line with
  the final status from `response_status()`.
- Request (2) returns `401 unauthorized` **without** the handler executing (confirm via
  the absence of the handler's log line), demonstrating `kExit`.
- Request (3) returns `200 secret`, demonstrating `kContinue` falling through to the
  handler, and the post-hook still logs the access line.

## Notes

- Interceptors are **non-blocking**, like request handlers; offload any heavy work to an
  application-managed thread pool.
- At most one interceptor applies per request (exact-path match, else the first matching
  dispatcher), matching the server's request-dispatching semantics.
