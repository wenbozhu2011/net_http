/* Copyright 2018 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// libevent based server implementation

#include "net_http/server/internal/evhttp_server.h"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/memory/memory.h"
#include "event2/event.h"
#include "event2/http.h"
#include "event2/thread.h"
#include "event2/util.h"
#include "net_http/internal/net_logging.h"

namespace net_http {

namespace {

absl::once_flag libevent_init_once;

void InitLibEvent() {
  if (evthread_use_pthreads() != 0) {
    NET_LOG(FATAL, "Server requires pthread support.");
  }
  // Ignore SIGPIPE and allow errors to propagate through error codes.
  signal(SIGPIPE, SIG_IGN);
  // TODO(wenboz): windows support needed?
}

void GlobalInitialize() { absl::call_once(libevent_init_once, &InitLibEvent); }

}  // namespace

EvHTTPServer::EvHTTPServer(std::unique_ptr<ServerOptions> options)
    : server_options_(std::move(options)), accepting_requests_() {}

// May crash the server if called before WaitForTermination() returns
EvHTTPServer::~EvHTTPServer() {
  if (!is_terminating()) {
    NET_LOG(ERROR, "Server has not been terminated. Force termination now.");
    Terminate();
  }

  if (ev_http_ != nullptr) {
    // this frees the socket handlers too
    evhttp_free(ev_http_);
  }

  if (ev_base_ != nullptr) {
    event_base_free(ev_base_);
  }
}

// Checks options.
// TODO(wenboz): support multiple ports
bool EvHTTPServer::Initialize() {
  if (server_options_->executor() == nullptr) {
    NET_LOG(FATAL, "Default EventExecutor is not configured.");
    return false;
  }

  if (server_options_->ports().empty()) {
    NET_LOG(FATAL, "Server port is not specified.");
    return false;
  }

  GlobalInitialize();

  // This ev_base_ created per-server v.s. global
  ev_base_ = event_base_new();
  if (ev_base_ == nullptr) {
    NET_LOG(FATAL, "Failed to create an event_base.");
    return false;
  }

  timeval tv_zero = {0, 0};
  immediate_ = event_base_init_common_timeout(ev_base_, &tv_zero);

  ev_http_ = evhttp_new(ev_base_);
  if (ev_http_ == nullptr) {
    NET_LOG(FATAL, "Failed to create evhttp.");
    return false;
  }

  // By default libevents only allow GET, POST, HEAD, PUT, DELETE request
  // we have to manually turn OPTIONS and PATCH flag on documentation:
  // (http://www.wangafu.net/~nickm/libevent-2.0/doxygen/html/http_8h.html)
  evhttp_set_allowed_methods(
      ev_http_, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
                    EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS |
                    EVHTTP_REQ_PATCH);
  evhttp_set_gencb(ev_http_, &DispatchEvRequestFn, this);

  return true;
}

// static function pointer
void EvHTTPServer::DispatchEvRequestFn(evhttp_request* req, void* server) {
  EvHTTPServer* http_server = static_cast<EvHTTPServer*>(server);
  http_server->DispatchEvRequest(req);
}

void EvHTTPServer::DispatchEvRequest(evhttp_request* req) {
  auto parsed_request = absl::make_unique<ParsedEvRequest>(req);

  if (!parsed_request->decode()) {
    evhttp_send_error(req, HTTP_BADREQUEST, nullptr);
    return;
  }

  std::string path(parsed_request->path);

  bool dispatched = false;
  std::unique_ptr<EvHTTPRequest> ev_request(
      new EvHTTPRequest(std::move(parsed_request), this));

  if (!ev_request->Initialize()) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, nullptr);
    return;
  }

  {
    absl::MutexLock l(&request_mu_);

    // Resolve the request handler: exact URI path match first, then dispatchers.
    // exact_handler points into uri_handlers_ (stable while the lock is held);
    // dispatched_handler owns a value returned by a dispatcher.
    const RequestHandler* exact_handler = nullptr;
    RequestHandler dispatched_handler = nullptr;

    auto handler_map_it = uri_handlers_.find(path);
    if (handler_map_it != uri_handlers_.end()) {
      exact_handler = &handler_map_it->second.handler;
      ev_request->SetHandlerOptions(handler_map_it->second.options);
      dispatched = true;
    } else {
      for (const auto& dispatcher : dispatchers_) {
        auto handler = dispatcher.dispatcher(ev_request.get());
        if (handler == nullptr) {
          continue;
        }
        dispatched_handler = std::move(handler);
        ev_request->SetHandlerOptions(dispatcher.options);
        dispatched = true;
        break;
      }
    }

    if (dispatched) {
      // Resolve the interceptor chain and split it into pre-hooks (run before
      // the handler) and post-hooks (run after the response is sent, in reverse
      // order). The post-hooks are attached to the request now, on the I/O
      // thread and before any Reply(), so EvSendReply always sees the complete
      // list without a data race.
      std::vector<Interceptor> chain =
          BuildInterceptorChain(path, ev_request.get());
      std::vector<RequestInterceptor> pre_hooks;
      std::vector<ResponseInterceptor> post_hooks;
      for (auto& interceptor : chain) {
        if (interceptor.request_interceptor) {
          pre_hooks.push_back(std::move(interceptor.request_interceptor));
        }
        if (interceptor.response_interceptor) {
          post_hooks.push_back(std::move(interceptor.response_interceptor));
        }
      }
      std::reverse(post_hooks.begin(), post_hooks.end());
      ev_request->SetResponseInterceptors(std::move(post_hooks));

      IncOps();
      if (pre_hooks.empty()) {
        // No pre-hooks: keep the original fast path unchanged.
        if (exact_handler != nullptr) {
          ScheduleHandlerReference(*exact_handler, ev_request.release());
        } else {
          ScheduleHandler(std::move(dispatched_handler), ev_request.release());
        }
      } else {
        RequestHandler handler = exact_handler != nullptr
                                     ? *exact_handler
                                     : std::move(dispatched_handler);
        ScheduleInterceptedHandler(std::move(pre_hooks), std::move(handler),
                                   ev_request.release());
      }
    }
  }

  if (!dispatched) {
    evhttp_send_error(req, HTTP_NOTFOUND, nullptr);
    return;
  }
}

void EvHTTPServer::ScheduleHandlerReference(const RequestHandler& handler,
                                            EvHTTPRequest* ev_request) {
  server_options_->executor()->Schedule(
      [&handler, ev_request]() { handler(ev_request); });
}

// Exactly one copy of the handler argument
// with the lambda passed by value to Schedule()
void EvHTTPServer::ScheduleHandler(RequestHandler&& handler,
                                   EvHTTPRequest* ev_request) {
  server_options_->executor()->Schedule(
      [handler, ev_request]() { handler(ev_request); });
}

void EvHTTPServer::ScheduleInterceptedHandler(
    std::vector<RequestInterceptor> pre_hooks, RequestHandler handler,
    EvHTTPRequest* ev_request) {
  server_options_->executor()->Schedule(
      [pre_hooks, handler, ev_request]() {
        for (const auto& pre_hook : pre_hooks) {
          if (pre_hook(ev_request) == InterceptResult::kExit) {
            // The interceptor has completed the response; skip the remaining
            // pre-hooks and the handler. Post-hooks still run via EvSendReply.
            return;
          }
        }
        handler(ev_request);
      });
}

std::vector<Interceptor> EvHTTPServer::BuildInterceptorChain(
    const std::string& path, EvHTTPRequest* ev_request) {
  std::vector<Interceptor> chain;

  auto it = interceptor_handlers_.find(path);
  if (it != interceptor_handlers_.end()) {
    chain.insert(chain.end(), it->second.begin(), it->second.end());
  }

  for (const auto& dispatcher : interceptor_dispatchers_) {
    Interceptor interceptor = dispatcher(ev_request);
    if (interceptor.request_interceptor || interceptor.response_interceptor) {
      chain.push_back(std::move(interceptor));
    }
  }

  return chain;
}

namespace {

void ResolveEphemeralPort(evhttp_bound_socket* listener, int* port) {
  sockaddr_storage ss = {};
  ev_socklen_t socklen = sizeof(ss);

  evutil_socket_t fd = evhttp_bound_socket_get_fd(listener);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &socklen)) {
    NET_LOG(ERROR, "getsockname() failed");
    return;
  }

  if (ss.ss_family == AF_INET) {
    *port = ntohs((reinterpret_cast<sockaddr_in*>(&ss))->sin_port);
  } else if (ss.ss_family == AF_INET6) {
    *port = ntohs((reinterpret_cast<sockaddr_in6*>(&ss))->sin6_port);
  } else {
    NET_LOG(ERROR, "Unknown address family %d", ss.ss_family);
  }
}

}  // namespace

bool EvHTTPServer::StartAcceptingRequests() {
  if (ev_http_ == nullptr) {
    NET_LOG(FATAL, "Server has not been successfully initialized");
    return false;
  }

  const int port = server_options_->ports().front();

  // "::"  =>  in6addr_any
  ev_uint16_t ev_port = static_cast<ev_uint16_t>(port);
  ev_listener_ = evhttp_bind_socket_with_handle(ev_http_, "::", ev_port);
  if (ev_listener_ == nullptr) {
    // in case ipv6 is not supported, fallback to inaddr_any
    ev_listener_ = evhttp_bind_socket_with_handle(ev_http_, nullptr, ev_port);
    if (ev_listener_ == nullptr) {
      NET_LOG(ERROR, "Couldn't bind to port %d", port);
      return false;
    }
  }

  // Listener counts as an active operation
  IncOps();

  port_ = port;
  if (port_ == 0) {
    ResolveEphemeralPort(ev_listener_, &port_);
  }

  IncOps();
  server_options_->executor()->Schedule([this]() {
    NET_LOG(INFO, "Entering the event loop ...");
    int result = event_base_dispatch(ev_base_);
    NET_LOG(INFO, "event_base_dispatch() exits with value %d", result);

    DecOps();
  });

  accepting_requests_.Notify();

  return true;
}

int EvHTTPServer::listen_port() const { return port_; }

bool EvHTTPServer::is_accepting_requests() const {
  return accepting_requests_.HasBeenNotified();
}

void EvHTTPServer::Terminate() {
  if (!is_accepting_requests()) {
    NET_LOG(ERROR, "Server is not running ...");
    return;
  }

  if (is_terminating()) {
    NET_LOG(ERROR, "Server is already being terminated ...");
    return;
  }

  terminating_.Notify();

  // call exit-loop from the event loop
  this->EventLoopSchedule([this]() {
    // Stop the listener first, which will delete ev_listener_
    // This may cause the loop to exit, so need be scheduled from within
    evhttp_del_accept_socket(ev_http_, ev_listener_);
    DecOps();
  });

  // Current shut-down behavior:
  // - we don't proactively delete/close any HTTP connections as part of
  //   Terminate(). This is not an issue as we don't support read-streaming yet.
  // - we don't wait for all dispatched requests to run to completion
  //   before we stop the event loop.
  // - and otherwise, this is meant to be a clean shutdown
}

bool EvHTTPServer::is_terminating() const {
  return terminating_.HasBeenNotified();
}

void EvHTTPServer::IncOps() {
  absl::MutexLock l(&ops_mu_);
  num_pending_ops_++;
}

void EvHTTPServer::DecOps() {
  absl::MutexLock l(&ops_mu_);
  num_pending_ops_--;
}

void EvHTTPServer::WaitForTermination() {
  {
    absl::MutexLock l(&ops_mu_);
    ops_mu_.Await(absl::Condition(
        +[](int64_t* count) { return *count <= 1; }, &num_pending_ops_));
  }

  int result = event_base_loopexit(ev_base_, nullptr);
  NET_LOG(INFO, "event_base_loopexit() exits with value %d", result);

  {
    absl::MutexLock l(&ops_mu_);
    ops_mu_.Await(absl::Condition(
        +[](int64_t* count) { return *count == 0; }, &num_pending_ops_));
  }
}

bool EvHTTPServer::WaitForTerminationWithTimeout(absl::Duration timeout) {
  bool wait_result = true;

  {
    absl::MutexLock l(&ops_mu_);
    wait_result = ops_mu_.AwaitWithTimeout(
        absl::Condition(
            +[](int64_t* count) { return *count <= 1; }, &num_pending_ops_),
        timeout);
  }

  if (wait_result) {
    int result = event_base_loopexit(ev_base_, nullptr);
    NET_LOG(INFO, "event_base_loopexit() exits with value %d", result);

    // This should pass immediately
    {
      absl::MutexLock l(&ops_mu_);
      wait_result = ops_mu_.AwaitWithTimeout(
          absl::Condition(
              +[](int64_t* count) { return *count == 0; }, &num_pending_ops_),
          timeout);
    }
  }

  return wait_result;
}

EvHTTPServer::UriHandlerInfo::UriHandlerInfo(
    absl::string_view uri_in, RequestHandler handler_in,
    const RequestHandlerOptions& options_in)
    : uri(uri_in.data(), uri_in.size()),
      handler(std::move(handler_in)),
      options(options_in) {}

EvHTTPServer::DispatcherInfo::DispatcherInfo(
    RequestDispatcher dispatcher_in, const RequestHandlerOptions& options_in)
    : dispatcher(std::move(dispatcher_in)), options(options_in) {}

void EvHTTPServer::RegisterRequestHandler(
    absl::string_view uri, RequestHandler handler,
    const RequestHandlerOptions& options) {
  absl::MutexLock l(&request_mu_);
  auto result = uri_handlers_.emplace(
      std::piecewise_construct, std::forward_as_tuple(uri),
      std::forward_as_tuple(uri, handler, options));

  if (!result.second) {
    NET_LOG(INFO,
            "Overwrite the existing handler registered under "
            "the URI path %.*s",
            static_cast<int>(uri.size()), uri.data());

    uri_handlers_.erase(result.first);
    if (!uri_handlers_
             .emplace(std::piecewise_construct, std::forward_as_tuple(uri),
                      std::forward_as_tuple(uri, handler, options))
             .second) {
      NET_LOG(ERROR, "Failed to register an handler under the URI path %.*s",
              static_cast<int>(uri.size()), uri.data());
    }
  }
}

void EvHTTPServer::RegisterRequestDispatcher(
    RequestDispatcher dispatcher, const RequestHandlerOptions& options) {
  absl::MutexLock l(&request_mu_);
  dispatchers_.emplace_back(dispatcher, options);
}

void EvHTTPServer::RegisterRequestInterceptor(
    absl::string_view uri, RequestInterceptor request_interceptor,
    ResponseInterceptor response_interceptor) {
  absl::MutexLock l(&request_mu_);
  // Interceptors accumulate under the same uri (chain), unlike request handlers.
  interceptor_handlers_[std::string(uri)].push_back(
      Interceptor{std::move(request_interceptor),
                  std::move(response_interceptor)});
}

void EvHTTPServer::RegisterRequestInterceptorDispatcher(
    RequestInterceptorDispatcher dispatcher) {
  absl::MutexLock l(&request_mu_);
  interceptor_dispatchers_.emplace_back(std::move(dispatcher));
}

namespace {

void EvImmediateCallback(evutil_socket_t socket, int16_t flags, void* arg) {
  auto fn = static_cast<std::function<void()>*>(arg);
  (*fn)();
  delete fn;
}

}  // namespace

bool EvHTTPServer::EventLoopSchedule(std::function<void()> fn) {
  auto scheduled_fn = new std::function<void()>(std::move(fn));
  int result = event_base_once(ev_base_, -1, EV_TIMEOUT, EvImmediateCallback,
                               static_cast<void*>(scheduled_fn), immediate_);
  return result == 0;
}

void EvHTTPServer::ScheduleReply(std::function<void()> fn) {
  server_options_->executor()->Schedule(fn);
}

}  // namespace net_http
