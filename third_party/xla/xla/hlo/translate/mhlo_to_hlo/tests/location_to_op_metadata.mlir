// RUN: xla-translate -split-input-file -mlir-hlo-to-hlo-text %s | FileCheck %s --dump-input=always --check-prefixes=CHECK

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc(unknown)
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-NOT: metadata

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc("AfterAll")
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="AfterAll"}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc("name@function")
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="name"}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc("file_name":2:8)
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={source_file="file_name" source_line=2 source_end_line=2 source_column=8 source_end_column=8}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc("name(with)[]")
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="name(with)[]"}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc("name(anothername)"("file_name":2:8))
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="name(anothername)" source_file="file_name" source_line=2 source_end_line=2 source_column=8 source_end_column=8}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc(fused["fused/location/file", "source.txt":42:5])
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="fused/location/file" source_file="source.txt" source_line=42 source_end_line=42 source_column=5 source_end_column=5}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc(fused["name1", fused["nested_fusion":5:42, "name2"]])
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="name1;name2" source_file="nested_fusion" source_line=5 source_end_line=5 source_column=42 source_end_column=42}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc(fused["multiple_sources", "source1":1:2, "source2":3:4])
  func.return %0 : !mhlo.token
}

// CHECK: after-all
// CHECK-SAME: metadata={op_name="multiple_sources" source_file="source2" source_line=3 source_end_line=3 source_column=4 source_end_column=4}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: tensor<4xf32> loc("x"), %arg1: tensor<4xf32> loc("y")) -> tensor<4xf32> {
  %0 = "mhlo.add"(%arg0, %arg1) : (tensor<4xf32>, tensor<4xf32>) -> tensor<4xf32>
  func.return %0 : tensor<4xf32>
}

// CHECK: parameter(0)
// CHECK-SAME: metadata={op_name="x"}
// CHECK: parameter(1)
// CHECK-SAME: metadata={op_name="y"}

// -----

// CHECK-LABEL: %main
func.func @main(%arg0: !mhlo.token) -> !mhlo.token {
  %0 = "mhlo.after_all"(%arg0) : (!mhlo.token) -> !mhlo.token loc(#type_loc)
  func.return %0 : !mhlo.token
}
#name_loc = loc("aname")
#type_loc = loc("atype:"(#name_loc))

// CHECK: after-all
// CHECK-SAME: metadata={op_type="atype" op_name="aname"}

// -----

// CHECK-LABEL: %main
module @m {
  func.func public @main(%arg0: tensor<f64>) -> (tensor<f64>) {
    %0 = call @foo(%arg0) : (tensor<f64>) -> tensor<f64> loc("x")
    return %0 : tensor<f64>
  }
  func.func private @foo(%arg0: tensor<f64>) -> tensor<f64> {
    %0 = mhlo.cosine %arg0 : tensor<f64>
    return %0 : tensor<f64>
  }
}

// CHECK: call({{.*}}), to_apply=%foo.{{[0-9.]+}}, metadata={op_name="x"}
