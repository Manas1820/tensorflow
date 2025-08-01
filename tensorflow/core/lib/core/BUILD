load("//tensorflow:tensorflow.bzl", "tf_cc_test")
load("//tensorflow:tensorflow.default.bzl", "filegroup")
load("//tensorflow/core/platform:build_config.bzl", "tf_proto_library")
load(
    "//tensorflow/core/platform:rules_cc.bzl",
    "cc_library",
)

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = [
        "//tensorflow/core:__subpackages__",
        "//tensorflow/python/profiler/internal:__pkg__",
    ],
    licenses = ["notice"],
)

# Todo(bmzhao): Remaining targets to add to this BUILD file are all tests.

cc_library(
    name = "arena",
    srcs = ["arena.cc"],
    hdrs = ["arena.h"],
    deps = [
        "//tensorflow/core/lib/math:math_util",
        "//tensorflow/core/platform:logging",
        "//tensorflow/core/platform:macros",
        "//tensorflow/core/platform:platform_port",
        "//tensorflow/core/platform:types",
    ],
)

tf_cc_test(
    name = "arena_test",
    srcs = ["arena_test.cc"],
    deps = [
        ":arena",
        "//tensorflow/core:test",
        "//tensorflow/core/platform:macros",
        "@local_xla//xla/tsl/platform:test_main",
    ],
)

cc_library(
    name = "bitmap",
    hdrs = ["bitmap.h"],
    deps = [
        "@local_xla//xla/tsl/lib/core:bitmap",
    ],
    alwayslink = 1,
)

cc_library(
    name = "bits",
    hdrs = ["bits.h"],
    deps = [
        "//tensorflow/core/platform:logging",
        "//tensorflow/core/platform:types",
        "@local_xla//xla/tsl/lib/core:bits",
    ],
)

cc_library(
    name = "coding",
    hdrs = ["coding.h"],
    deps = [
        "//tensorflow/core/platform:coding",
    ],
)

tf_cc_test(
    name = "coding_test",
    srcs = ["coding_test.cc"],
    deps = [
        ":coding",
        "//tensorflow/core:test",
        "@local_xla//xla/tsl/platform:test_main",
    ],
)

cc_library(
    name = "errors",
    hdrs = ["errors.h"],
    deps = [
        "//tensorflow/core/platform:errors",
    ],
)

cc_library(
    name = "notification",
    hdrs = ["notification.h"],
    deps = [
        "//tensorflow/core/platform:notification",
    ],
)

cc_library(
    name = "raw_coding",
    hdrs = ["raw_coding.h"],
    deps = [
        "//tensorflow/core/platform:raw_coding",
    ],
)

cc_library(
    name = "refcount",
    hdrs = ["refcount.h"],
    deps = ["//tensorflow/core/platform:refcount"],
)

cc_library(
    name = "status",
    hdrs = ["status.h"],
    deps = [
        "//tensorflow/core/platform:status",
    ],
)

tf_cc_test(
    name = "status_test",
    srcs = ["status_test.cc"],
    deps = [
        ":errors",
        ":status",
        "//tensorflow/core:test",
        "@com_google_absl//absl/strings",
        "@local_xla//xla/tsl/platform:test_main",
    ],
)

cc_library(
    name = "stringpiece",
    hdrs = ["stringpiece.h"],
    deps = [
        "//tensorflow/core/platform:stringpiece",
    ],
)

cc_library(
    name = "threadpool",
    hdrs = ["threadpool.h"],
    deps = [
        "//tensorflow/core/platform:env",
    ],
)

tf_cc_test(
    name = "threadpool_test",
    srcs = ["threadpool_test.cc"],
    deps = [
        ":threadpool",
        "//tensorflow/core:framework_lite",
        "//tensorflow/core:lib",
        "//tensorflow/core:test",
        "@com_google_absl//absl/synchronization",
        "@local_xla//xla/tsl/platform:test_main",
    ],
)

cc_library(
    name = "threadpool_interface",
    hdrs = ["threadpool_interface.h"],
    deps = [
        "//tensorflow/core/platform:threadpool_interface",
    ],
)

cc_library(
    name = "threadpool_options",
    hdrs = ["threadpool_options.h"],
    deps = [
        "//tensorflow/core/platform:threadpool_options",
    ],
)

tf_proto_library(
    name = "error_codes_proto",
    srcs = ["error_codes.proto"],
    make_default_target_header_only = True,
    protodeps = [
        "@local_xla//xla/tsl/protobuf:error_codes_proto_impl",
    ],
    exports = ["@local_xla//xla/tsl/protobuf:error_codes_proto_impl"],
)

# Export source files needed for mobile builds, which do not use granular targets.
filegroup(
    name = "mobile_srcs_no_runtime",
    srcs = [
        "coding.h",
        "errors.h",
        "refcount.h",
        "status.h",
        "stringpiece.h",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

filegroup(
    name = "mobile_srcs_only_runtime",
    srcs = [
        "arena.cc",
        "arena.h",
        "bitmap.h",
        "bits.h",
        "notification.h",
        "threadpool.h",
        "threadpool_interface.h",
        "threadpool_options.h",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

filegroup(
    name = "legacy_lib_core_all_headers",
    srcs = [
        "arena.h",
        "bitmap.h",
        "bits.h",
        "coding.h",
        "errors.h",
        "notification.h",
        "raw_coding.h",
        "refcount.h",
        "status.h",
        "status_test_util.h",
        "stringpiece.h",
        "threadpool.h",
        "threadpool_interface.h",
        "threadpool_options.h",
        "@local_xla//xla/tsl/lib/core:legacy_lib_core_all_headers",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

filegroup(
    name = "legacy_lib_internal_core_headers",
    srcs = [
        "refcount.h",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

filegroup(
    name = "legacy_lib_core_status_header",
    srcs = [
        "status.h",
    ],
    visibility = ["//visibility:private"],
)

filegroup(
    name = "legacy_lib_core_status_test_util_header",
    srcs = [
        "status_test_util.h",
        "@local_xla//xla/tsl/lib/core:legacy_lib_core_status_test_util_header",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

filegroup(
    name = "legacy_lib_core_stringpiece_header",
    srcs = [
        "stringpiece.h",
    ],
    visibility = [
        "//tensorflow/core:__pkg__",
        "//tensorflow/core/lib/gif:__pkg__",
        "//tensorflow/core/lib/jpeg:__pkg__",
    ],
)

filegroup(
    name = "legacy_lib_proto_parsing_headers",
    srcs = [
        "errors.h",
        "status.h",
        "stringpiece.h",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

filegroup(
    name = "legacy_lib_core_headers",
    srcs = [
        "arena.h",
        "bitmap.h",
        "bits.h",
        "coding.h",
        "errors.h",
        "notification.h",
        "raw_coding.h",
        "refcount.h",
        "status.h",
        "stringpiece.h",
        "threadpool.h",
        "threadpool_interface.h",
        "threadpool_options.h",
    ],
    visibility = ["//tensorflow/core:__pkg__"],
)

# This is needed because of how tf_android_core_proto_sources parses proto paths.
exports_files(
    srcs = ["error_codes.proto"],
    visibility = ["//tensorflow/core:__pkg__"],
)
