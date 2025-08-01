load("@llvm-project//mlir:tblgen.bzl", "gentbl_cc_library", "td_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//tensorflow:tensorflow.bzl", "tf_cc_test")
load("//tensorflow:tensorflow.default.bzl", "get_compatible_with_portable")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_compatible_with = get_compatible_with_portable(),
    default_visibility = [
        "//tensorflow/compiler/mlir/tensorflow:__subpackages__",
        "//tensorflow/core:__subpackages__",
        "//tensorflow/tools/tfg_graph_transforms:__subpackages__",
    ],
    licenses = ["notice"],  # Apache 2.0
)

td_library(
    name = "InterfacesTdFiles",
    srcs = ["interfaces.td"],
    deps = [
        "@llvm-project//mlir:OpBaseTdFiles",
    ],
)

gentbl_cc_library(
    name = "InterfacesIncGen",
    tbl_outs = {
        "interfaces.h.inc": ["-gen-op-interface-decls"],
        "interfaces.cc.inc": ["-gen-op-interface-defs"],
    },
    tblgen = "@llvm-project//mlir:mlir-tblgen",
    td_file = "interfaces.td",
    deps = [
        ":InterfacesTdFiles",
    ],
)

# ODS (https://mlir.llvm.org/docs/OpDefinitions/) generation for op and dialect files to include.
td_library(
    name = "DialectTdFiles",
    srcs = [
        "dialect.td",
        "ops.td",
    ],
    deps = [
        "@llvm-project//mlir:CallInterfacesTdFiles",
        "@llvm-project//mlir:ControlFlowInterfacesTdFiles",
        "@llvm-project//mlir:FunctionInterfacesTdFiles",
        "@llvm-project//mlir:InferTypeOpInterfaceTdFiles",
        "@llvm-project//mlir:OpBaseTdFiles",
        "@llvm-project//mlir:SideEffectInterfacesTdFiles",
    ],
)

gentbl_cc_library(
    name = "DialectIncGen",
    tbl_outs = {
        "ops.h.inc": [
            "-gen-op-decls",
            "-dialect",
            "tfg",
        ],
        "ops.cc.inc": [
            "-gen-op-defs",
            "-dialect",
            "tfg",
        ],
        "dialect.h.inc": [
            "-gen-dialect-decls",
            "-dialect",
            "tfg",
        ],
        "dialect.cc.inc": [
            "-gen-dialect-defs",
            "-dialect",
            "tfg",
        ],
        "attributes.h.inc": [
            "-gen-attrdef-decls",
            "-attrdefs-dialect",
            "tfg",
        ],
        "attributes.cc.inc": [
            "-gen-attrdef-defs",
            "-attrdefs-dialect",
            "tfg",
        ],
    },
    tblgen = "@llvm-project//mlir:mlir-tblgen",
    td_file = "ops.td",
    deps = [
        ":DialectTdFiles",
        ":InterfacesTdFiles",
        "//tensorflow/core/ir/types:DialectTdFiles",
    ],
)

cc_library(
    name = "Dialect",
    srcs = [
        "interfaces.cc",
        "ops.cc",
        "tf_op_names.cc",
        "tf_op_names.inc",
        "tf_op_wrapper.cc",
        "utility.cc",
    ],
    hdrs = [
        "dialect.h",
        "interfaces.h",
        "ops.h",
        "tf_op_wrapper.h",
        "utility.h",
    ],
    deps = [
        ":DialectIncGen",
        ":InterfacesIncGen",
        "//tensorflow/core/ir/types:Dialect",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:CallOpInterfaces",
        "@llvm-project//mlir:ControlFlowInterfaces",
        "@llvm-project//mlir:FunctionInterfaces",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:InferTypeOpInterface",
        "@llvm-project//mlir:QuantOps",
        "@llvm-project//mlir:SideEffectInterfaces",
        "@llvm-project//mlir:Support",
    ],
)

cc_library(
    name = "tf_op_registry",
    srcs = ["tf_op_registry.cc"],
    hdrs = ["tf_op_registry.h"],
    deps = [
        ":Dialect",
        "//tensorflow/core:framework",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Support",
    ],
)

cc_library(
    name = "shape_inference_utils",
    srcs = ["utils/shape_inference_utils.cc"],
    hdrs = ["utils/shape_inference_utils.h"],
    deps = [
        ":Dialect",
        "//tensorflow/core:framework",
        "//tensorflow/core:protos_all_cc",
        "//tensorflow/core/ir/importexport:convert_tensor",
        "//tensorflow/core/ir/importexport:convert_types",
        "//tensorflow/core/ir/importexport:graphdef_export",
        "//tensorflow/core/ir/types:Dialect",
        "//tensorflow/core/platform:logging",
        "//tensorflow/core/platform:status",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:DerivedAttributeOpInterface",
        "@llvm-project//mlir:FunctionInterfaces",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:InferTypeOpInterface",
        "@llvm-project//mlir:Support",
    ],
)

tf_cc_test(
    name = "utility_test",
    size = "small",
    srcs = ["utility_test.cc"],
    deps = [
        ":Dialect",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Parser",
        "@llvm-project//mlir:Support",
    ],
)

tf_cc_test(
    name = "tf_op_wrapper_test",
    size = "small",
    srcs = ["tf_op_wrapper_test.cc"],
    deps = [
        ":Dialect",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Parser",
        "@llvm-project//mlir:Support",
    ],
)

tf_cc_test(
    name = "shape_inference_utils_test",
    size = "small",
    srcs = ["utils/shape_inference_utils_test.cc"],
    deps = [
        ":Dialect",
        ":shape_inference_utils",
        "//tensorflow/core:framework",
        "//tensorflow/core:ops",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
        "//tensorflow/core/platform:errors",
        "@com_google_absl//absl/status",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:InferTypeOpInterface",
        "@llvm-project//mlir:Parser",
        "@llvm-project//mlir:Support",
    ],
)

tf_cc_test(
    name = "ops_test",
    size = "small",
    srcs = ["ops_test.cc"],
    deps = [
        ":Dialect",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
        "@llvm-project//mlir:ControlFlowInterfaces",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Parser",
    ],
)

tf_cc_test(
    name = "interfaces_test",
    size = "small",
    srcs = ["interfaces_test.cc"],
    deps = [
        ":Dialect",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:IR",
    ],
)

tf_cc_test(
    name = "tf_op_registry_test",
    size = "small",
    srcs = ["tf_op_registry_test.cc"],
    deps = [
        ":Dialect",
        ":tf_op_registry",
        "//tensorflow/core:ops",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Parser",
        "@llvm-project//mlir:Support",
    ],
)
