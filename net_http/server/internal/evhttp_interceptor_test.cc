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

// Tests for request/response interceptor support in the libevent HTTP server.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "net_http/client/test_client/internal/evhttp_connection.h"
#include "net_http/internal/fixed_thread_pool.h"
#include "net_http/server/public/httpserver.h"
#include "net_http/server/public/httpserver_interface.h"
#include "net_http/server/public/server_request_interface.h"

namespace net_http {
namespace {

class MyExecutor final : public EventExecutor {
 public:
  explicit MyExecutor(int num_threads) : thread_pool_(num_threads) {}

  void Schedule(std::function<void()> fn) override { thread_pool_.Schedule(fn); }

 private:
  FixedThreadPool thread_pool_;
};

class InterceptorTest : public ::testing::Test {
 public:
  void SetUp() override { InitServer(); }

  void TearDown() override {
    if (!server->is_terminating()) {
      server->Terminate();
      server->WaitForTermination();
    }
  }

 protected:
  std::unique_ptr<HTTPServerInterface> server;

 private:
  void InitServer() {
    auto options = absl::make_unique<ServerOptions>();
    options->AddPort(0);
    options->SetExecutor(absl::make_unique<MyExecutor>(4));

    server = CreateEvHTTPServer(std::move(options));

    ASSERT_TRUE(server != nullptr);
  }
};

// Records events (interceptor/handler invocations) in order, thread-safely.
class EventLog {
 public:
  void Add(const std::string& event) {
    absl::MutexLock l(&mu_);
    events_.push_back(event);
  }

  std::vector<std::string> events() {
    absl::MutexLock l(&mu_);
    return events_;
  }

 private:
  absl::Mutex mu_;
  std::vector<std::string> events_ ABSL_GUARDED_BY(mu_);
};

// Case-insensitive lookup of a response header value; returns nullptr if absent.
const std::string* FindResponseHeader(const TestClientResponse& response,
                                      absl::string_view name) {
  for (const auto& header : response.headers) {
    if (absl::EqualsIgnoreCase(header.first, name)) {
      return &header.second;
    }
  }
  return nullptr;
}

// A pre-handler interceptor adds a response header, then the handler replies.
TEST_F(InterceptorTest, PreHookAddsHeader) {
  auto pre_hook = [](ServerRequestInterface* request) {
    request->AppendResponseHeader("X-Intercepted", "yes");
    return InterceptResult::kContinue;
  };
  auto handler = [](ServerRequestInterface* request) {
    request->WriteResponseString("HANDLED");
    request->Reply();
  };

  server->RegisterRequestInterceptor("/ok", std::move(pre_hook), nullptr);
  server->RegisterRequestHandler("/ok", std::move(handler),
                                 RequestHandlerOptions());
  server->StartAcceptingRequests();

  auto connection =
      TestEvHTTPConnection::Connect("localhost", server->listen_port());
  ASSERT_TRUE(connection != nullptr);

  TestClientRequest request = {"/ok", "GET", {}, ""};
  TestClientResponse response = {};

  EXPECT_TRUE(connection->BlockingSendRequest(request, &response));
  EXPECT_EQ(response.status, HTTPStatusCode::OK);
  EXPECT_EQ(response.body, "HANDLED");
  const std::string* header = FindResponseHeader(response, "X-Intercepted");
  ASSERT_NE(header, nullptr);
  EXPECT_EQ(*header, "yes");

  server->Terminate();
  server->WaitForTermination();
}

// A pre-handler interceptor short-circuits the request with kExit; the handler
// never runs.
TEST_F(InterceptorTest, ShortCircuits) {
  absl::Notification handler_ran;
  auto pre_hook = [](ServerRequestInterface* request) {
    request->WriteResponseString("denied");
    request->ReplyWithStatus(HTTPStatusCode::UNAUTHORIZED);
    return InterceptResult::kExit;
  };
  auto handler = [&handler_ran](ServerRequestInterface* request) {
    handler_ran.Notify();
    request->WriteResponseString("HANDLED");
    request->Reply();
  };

  server->RegisterRequestInterceptor("/secure", std::move(pre_hook), nullptr);
  server->RegisterRequestHandler("/secure", std::move(handler),
                                 RequestHandlerOptions());
  server->StartAcceptingRequests();

  auto connection =
      TestEvHTTPConnection::Connect("localhost", server->listen_port());
  ASSERT_TRUE(connection != nullptr);

  TestClientRequest request = {"/secure", "GET", {}, ""};
  TestClientResponse response = {};

  EXPECT_TRUE(connection->BlockingSendRequest(request, &response));
  EXPECT_EQ(response.status, HTTPStatusCode::UNAUTHORIZED);
  EXPECT_EQ(response.body, "denied");
  EXPECT_FALSE(handler_ran.HasBeenNotified());

  server->Terminate();
  server->WaitForTermination();
}

// Multiple interceptors on the same exact path form a chain: pre-hooks run in
// registration order, post-hooks run in reverse order, around the handler.
TEST_F(InterceptorTest, ChainOrder) {
  EventLog log;
  absl::Notification done;

  server->RegisterRequestInterceptor(
      "/ok",
      [&log](ServerRequestInterface* request) {
        log.Add("preA");
        return InterceptResult::kContinue;
      },
      [&log, &done](ServerRequestInterface* request) {
        // Registered first, so runs last among post-hooks (reverse order).
        log.Add("postA");
        done.Notify();
      });
  server->RegisterRequestInterceptor(
      "/ok",
      [&log](ServerRequestInterface* request) {
        log.Add("preB");
        return InterceptResult::kContinue;
      },
      [&log](ServerRequestInterface* request) { log.Add("postB"); });
  server->RegisterRequestHandler(
      "/ok",
      [&log](ServerRequestInterface* request) {
        log.Add("handler");
        request->WriteResponseString("HANDLED");
        request->Reply();
      },
      RequestHandlerOptions());
  server->StartAcceptingRequests();

  auto connection =
      TestEvHTTPConnection::Connect("localhost", server->listen_port());
  ASSERT_TRUE(connection != nullptr);

  TestClientRequest request = {"/ok", "GET", {}, ""};
  TestClientResponse response = {};

  EXPECT_TRUE(connection->BlockingSendRequest(request, &response));
  EXPECT_EQ(response.status, HTTPStatusCode::OK);
  EXPECT_EQ(response.body, "HANDLED");

  ASSERT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(5)));
  EXPECT_EQ(log.events(), (std::vector<std::string>{"preA", "preB", "handler",
                                                    "postB", "postA"}));

  server->Terminate();
  server->WaitForTermination();
}

// A post-handler interceptor can read the final response status.
TEST_F(InterceptorTest, PostReadsResponseStatus) {
  absl::Notification done;
  HTTPStatusCode observed = HTTPStatusCode::UNDEFINED;
  auto post_hook = [&observed, &done](ServerRequestInterface* request) {
    observed = request->response_status();
    done.Notify();
  };
  auto handler = [](ServerRequestInterface* request) {
    request->WriteResponseString("created");
    request->ReplyWithStatus(HTTPStatusCode::CREATED);
  };

  server->RegisterRequestInterceptor("/ok", nullptr, std::move(post_hook));
  server->RegisterRequestHandler("/ok", std::move(handler),
                                 RequestHandlerOptions());
  server->StartAcceptingRequests();

  auto connection =
      TestEvHTTPConnection::Connect("localhost", server->listen_port());
  ASSERT_TRUE(connection != nullptr);

  TestClientRequest request = {"/ok", "GET", {}, ""};
  TestClientResponse response = {};

  EXPECT_TRUE(connection->BlockingSendRequest(request, &response));
  EXPECT_EQ(response.status, HTTPStatusCode::CREATED);

  ASSERT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(5)));
  EXPECT_EQ(observed, HTTPStatusCode::CREATED);

  server->Terminate();
  server->WaitForTermination();
}

// A dispatcher-provided interceptor applies when there is no exact-path
// interceptor; a dispatcher that returns an empty Interceptor does not apply.
TEST_F(InterceptorTest, Dispatcher) {
  auto dispatcher = [](ServerRequestInterface* request) {
    if (request->uri_path() != "/ok") {
      return Interceptor{};  // does not apply
    }
    return Interceptor{
        [](ServerRequestInterface* request) {
          request->AppendResponseHeader("X-Dispatched", "yes");
          return InterceptResult::kContinue;
        },
        nullptr};
  };
  auto handler = [](ServerRequestInterface* request) {
    request->WriteResponseString("HANDLED");
    request->Reply();
  };

  server->RegisterRequestInterceptorDispatcher(std::move(dispatcher));
  server->RegisterRequestHandler("/ok", handler, RequestHandlerOptions());
  server->RegisterRequestHandler("/other", handler, RequestHandlerOptions());
  server->StartAcceptingRequests();

  auto connection =
      TestEvHTTPConnection::Connect("localhost", server->listen_port());
  ASSERT_TRUE(connection != nullptr);

  TestClientRequest request = {"/ok", "GET", {}, ""};
  TestClientResponse response = {};
  EXPECT_TRUE(connection->BlockingSendRequest(request, &response));
  EXPECT_EQ(response.status, HTTPStatusCode::OK);
  EXPECT_NE(FindResponseHeader(response, "X-Dispatched"), nullptr);

  request = {"/other", "GET", {}, ""};
  response = {};
  EXPECT_TRUE(connection->BlockingSendRequest(request, &response));
  EXPECT_EQ(response.status, HTTPStatusCode::OK);
  EXPECT_EQ(FindResponseHeader(response, "X-Dispatched"), nullptr);

  server->Terminate();
  server->WaitForTermination();
}

}  // namespace
}  // namespace net_http
