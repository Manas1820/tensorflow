# Description:
# JavaScript/TypeScript code generation for TensorFlow.js

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load(
    "//tensorflow:tensorflow.bzl",
    "tf_cc_test",
)

visibility = [
    "//tensorflow:internal",
]

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = visibility,
    licenses = ["notice"],
)

cc_library(
    name = "ts_op_gen",
    srcs = [
        "ops/ts_op_gen.cc",
    ],
    hdrs = [
        "ops/ts_op_gen.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//tensorflow/core:framework",
        "//tensorflow/core:lib",
        "//tensorflow/core:lib_internal",
        "//tensorflow/core:op_gen_lib",
        "//tensorflow/core:protos_all_cc",
    ],
)

tf_cc_test(
    name = "ts_op_gen_test",
    srcs = [
        "ops/ts_op_gen.cc",
        "ops/ts_op_gen.h",
        "ops/ts_op_gen_test.cc",
    ],
    deps = [
        "//tensorflow/core:framework",
        "//tensorflow/core:lib",
        "//tensorflow/core:lib_internal",
        "//tensorflow/core:op_gen_lib",
        "//tensorflow/core:protos_all_cc",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
    ],
)
