// RUN: flatbuffer_translate -mlir-to-tflite-flatbuffer --serialize-debug-metadata=true %s -o - | flatbuffer_translate --tflite-flatbuffer-to-mlir --mlir-print-debuginfo -o - | FileCheck %s
// This test verifies that debug locations are round-trippable.

#loc = loc("<stdin>":0:0)
#loc5 = loc("main"(#loc))
#loc8 = loc("cond_true"(#loc))
#loc10 = loc("cond_false"(#loc))
module @jit_relu attributes {jax.uses_shape_polymorphism = false, mhlo.num_partitions = 1 : i32, mhlo.num_replicas = 1 : i32, tfl._legalize_tfl_variables = true} {
  func.func @main(%arg0: tensor<1xf32>, %arg1: tensor<1xf32>) -> tensor<1xf32> {
    %0 = tfl.less(%arg0, %arg1) : (tensor<1xf32>, tensor<1xf32>) -> tensor<1xi1> loc(#loc6)
    // CHECK-DAG: {{.*}} = tfl.less(%arg0, %arg1) : (tensor<1xf32>, tensor<1xf32>) -> tensor<1xi1> loc(#loc6)
    %1 = "tf.If"(%0, %arg0, %arg1) <{else_branch = @cond_false, is_stateless = false, then_branch = @cond_true}> : (tensor<1xi1>, tensor<1xf32>, tensor<1xf32>) -> tensor<1xf32> loc(#loc7)
    // CHECK-DAG: {{.*}} = "tf.If"(%0, %arg0, %arg1) {{.*}} -> tensor<1xf32> loc(#loc7)
    func.return %1 : tensor<1xf32> loc(#loc)
  } loc(#loc5)
  func.func @cond_true(%arg0: tensor<*xf32>, %arg1: tensor<*xf32>) -> tensor<*xf32> {
    %0 = tfl.add %arg0, %arg1 {fused_activation_function = "NONE"} : tensor<*xf32> loc(#loc16)
    // CHECK-DAG: {{.*}} = tfl.add %arg0, %arg1 {fused_activation_function = "NONE"} : tensor<*xf32> loc(#loc16)
    func.return %0 : tensor<*xf32> loc(#loc)
  } loc(#loc8)
  func.func @cond_false(%arg0: tensor<*xf32>, %arg1: tensor<*xf32>) -> tensor<*xf32> {
    %0 = tfl.mul %arg0, %arg1 {fused_activation_function = "NONE"} : tensor<*xf32> loc(#loc15)
    // CHECK-DAG: {{.*}} = tfl.mul %arg0, %arg1 {fused_activation_function = "NONE"} : tensor<*xf32> loc(#loc15)
    func.return %0 : tensor<*xf32> loc(#loc)
  } loc(#loc10)
} loc(#loc)
#loc1 = loc("tfl.less")
// CHECK-DAG: #loc1 = loc("tfl.less")
#loc2 = loc("tf.If")
// CHECK-DAG: #loc2 = loc("tf.If")
#loc3 = loc("<ipython-input-7-340b9abeb7a8>":1:4)
// CHECK-DAG: #loc3 = loc("<ipython-input-7-340b9abeb7a8>":1:4)
#loc4 = loc("third_party/py/IPython/v3_2_3/core/interactiveshell.py":3066:16)
// CHECK-DAG: #loc4 = loc("third_party/py/IPython/v3_2_3/core/interactiveshell.py":3066:16)
#loc6 = loc(fused<"tflite.importer_wrapper">[#loc1])
// CHECK-DAG: #loc6 = loc(fused<"tflite.importer_wrapper">[#loc1])
#loc7 = loc(fused<"tflite.importer_wrapper">[#loc2])
// CHECK-DAG: #loc7 = loc(fused<"tflite.importer_wrapper">[#loc2])
#loc9 = loc(callsite(#loc3 at #loc4))
// CHECK-DAG: #loc9 = loc(callsite(#loc3 at #loc4))
#loc11 = loc(fused<"">[#loc3, #loc4])
// CHECK-DAG: #loc11 = loc(fused<"">[#loc3, #loc4])
#loc12 = loc("jit(relu)/jit(main)/max"(#loc9))
// CHECK-DAG: #loc12 = loc("jit(relu)/jit(main)/max"(#loc9))
#loc13 = loc("tfl.mul"(#loc11))
// CHECK-DAG: #loc13 = loc("tfl.mul"(#loc11))
#loc14 = loc("jit(relu)/jit(main)/max"(#loc12))
// CHECK-DAG: #loc14 = loc("jit(relu)/jit(main)/max"(#loc12))
#loc15 = loc(fused<"tflite.importer_wrapper">[#loc13])
// CHECK-DAG: #loc15 = loc(fused<"tflite.importer_wrapper">[#loc13])
#loc16 = loc(fused<"tflite.importer_wrapper">[#loc14])
// CHECK-DAG: #loc16 = loc(fused<"tflite.importer_wrapper">[#loc14])
