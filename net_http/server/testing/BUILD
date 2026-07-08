# Description: net_http/server/testing

package(default_visibility = ["//visibility:private"])

licenses(["notice"])

cc_binary(
    name = "evhttp_echo_server",
    srcs = ["evhttp_echo_server.cc"],
    deps = [
        "//net_http/server/public:http_server",
        "//net_http/server/public:http_server_api",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "evhttp_interceptor_server",
    srcs = ["evhttp_interceptor_server.cc"],
    testonly = 1,
    deps = [
        "//net_http/internal:fixed_thread_pool",
        "//net_http/server/public:http_server",
        "//net_http/server/public:http_server_api",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
    ],
)
