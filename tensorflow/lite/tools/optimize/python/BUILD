load("//tensorflow:strict.default.bzl", "py_strict_binary", "py_strict_library", "py_strict_test")
load("//tensorflow:tensorflow.default.bzl", "pybind_extension")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = [
        "//visibility:public",
    ],
    licenses = ["notice"],
)

py_strict_binary(
    name = "modify_model_interface",
    srcs = ["modify_model_interface.py"],
    deps = [
        ":modify_model_interface_constants",
        ":modify_model_interface_lib",
        "@absl_py//absl:app",
        "@absl_py//absl/flags",
    ],
)

py_strict_library(
    name = "modify_model_interface_lib",
    srcs = ["modify_model_interface_lib.py"],
    deps = [
        ":_pywrap_modify_model_interface",
        ":modify_model_interface_constants",
        "//tensorflow/lite/python:schema_py",
    ],
)

# Use --config=disable_tf_lite_py when running this test under github.
py_strict_test(
    name = "modify_model_interface_lib_test",
    srcs = ["modify_model_interface_lib_test.py"],
    deps = [
        ":modify_model_interface_lib",
        #internal proto upb dep
        "//third_party/py/numpy",
        "//tensorflow:tensorflow_py",
        "//tensorflow/lite/python:lite",
        "//tensorflow/python/framework:test_lib",
        "//tensorflow/python/platform:client_testlib",
    ],
)

py_strict_library(
    name = "modify_model_interface_constants",
    srcs = ["modify_model_interface_constants.py"],
    deps = ["//tensorflow/python/framework:dtypes"],
)

pybind_extension(
    name = "_pywrap_modify_model_interface",
    srcs = ["modify_model_interface.cc"],
    common_lib_packages = [
        "litert/python",
        "tensorflow/lite/python",
    ],
    enable_stub_generation = True,
    pytype_srcs = [
        "_pywrap_modify_model_interface.pyi",
    ],
    wrap_py_init = True,
    deps = [
        "//tensorflow/lite/schema:schema_fbs",
        "//tensorflow/lite/tools/optimize:modify_model_interface",
        "@pybind11",
    ],
)
