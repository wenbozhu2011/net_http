/* Copyright 2026 Google Inc. All Rights Reserved.

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

// A server demonstrating interceptor support, backed by a fixed thread pool:
//  - /echo    echoes the request method, URI and body (text/plain)
//  - /secure  returns "secret", guarded by an auth pre-hook interceptor
//  - all paths are logged by an access-log post-hook interceptor (dispatcher)
//
// See net_http/docs/interceptors/example.md for the end-to-end walkthrough.

#include <cstddef>
#include <cstdint>

#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "absl/memory/memory.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

#include "net_http/internal/fixed_thread_pool.h"
#include "net_http/server/public/httpserver.h"
#include "net_http/server/public/httpserver_interface.h"
#include "net_http/server/public/server_request_interface.h"

namespace {

using net_http::EventExecutor;
using net_http::FixedThreadPool;
using net_http::HTTPServerInterface;
using net_http::HTTPStatusCode;
using net_http::InterceptResult;
using net_http::Interceptor;
using net_http::RequestHandlerOptions;
using net_http::ServerOptions;
using net_http::ServerRequestInterface;
using net_http::SetContentTypeTEXT;

void EchoHandler(ServerRequestInterface* req) {
  std::string body;
  absl::StrAppend(&body, req->http_method(), " ", req->uri_path(), "\n");

  int64_t num_bytes;
  auto chunk = req->ReadRequestBytes(&num_bytes);
  while (chunk != nullptr) {
    absl::StrAppend(&body, absl::string_view(chunk.get(),
                                             static_cast<size_t>(num_bytes)));
    chunk = req->ReadRequestBytes(&num_bytes);
  }

  SetContentTypeTEXT(req);
  req->WriteResponseString(body);
  req->Reply();
}

void SecureHandler(ServerRequestInterface* req) {
  SetContentTypeTEXT(req);
  req->WriteResponseString("secret\n");
  req->Reply();
}

// Pre-hook: require an Authorization header, else short-circuit with 401 and
// skip the handler.
InterceptResult AuthInterceptor(ServerRequestInterface* req) {
  if (req->GetRequestHeader("Authorization").empty()) {
    SetContentTypeTEXT(req);
    req->WriteResponseString("unauthorized\n");
    req->ReplyWithStatus(HTTPStatusCode::UNAUTHORIZED);
    return InterceptResult::kExit;
  }
  return InterceptResult::kContinue;
}

// Post-hook: access log applied to every request via a dispatcher. Runs after
// the response is sent, so the final status is available.
void AccessLog(ServerRequestInterface* req) {
  std::cerr << req->http_method() << " " << req->uri_path() << " -> "
            << static_cast<int>(req->response_status()) << std::endl;
}

// An executor backed by a fixed-size thread pool.
class ThreadPoolExecutor final : public EventExecutor {
 public:
  explicit ThreadPoolExecutor(int num_threads) : thread_pool_(num_threads) {}

  void Schedule(std::function<void()> fn) override { thread_pool_.Schedule(fn); }

 private:
  FixedThreadPool thread_pool_;
};

// Returns the server if success, or nullptr if there is any error.
std::unique_ptr<HTTPServerInterface> StartServer(int port) {
  auto options = absl::make_unique<ServerOptions>();
  options->AddPort(port);
  options->SetExecutor(absl::make_unique<ThreadPoolExecutor>(4));

  auto server = CreateEvHTTPServer(std::move(options));
  if (server == nullptr) {
    return nullptr;
  }

  RequestHandlerOptions handler_options;
  server->RegisterRequestHandler("/echo", EchoHandler, handler_options);
  server->RegisterRequestHandler("/secure", SecureHandler, handler_options);

  // Auth gate on /secure (pre-hook only).
  server->RegisterRequestInterceptor("/secure", AuthInterceptor, nullptr);

  // Access log for every request (post-hook only, via a dispatcher).
  server->RegisterRequestInterceptorDispatcher(
      [](ServerRequestInterface* req) {
        return Interceptor{nullptr, AccessLog};
      });

  if (!server->StartAcceptingRequests()) {
    return nullptr;
  }
  return server;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: evhttp_interceptor_server <port:8080>" << std::endl;
    return 1;
  }

  int port = 8080;
  if (!absl::SimpleAtoi(argv[1], &port)) {
    std::cerr << "Invalid port: " << argv[1] << std::endl;
    return 1;
  }

  auto server = StartServer(port);
  if (server != nullptr) {
    server->WaitForTermination();
    return 0;
  } else {
    std::cerr << "Failed to start the server." << std::endl;
    return 1;
  }
}
