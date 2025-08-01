/* Copyright 2024 The OpenXLA Authors.

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
#include "xla/codegen/emitters/computation_partitioner.h"

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "xla/hlo/analysis/indexing_analysis.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/testlib/hlo_hardware_independent_test_base.h"

namespace xla {
namespace emitters {
namespace {

using ::testing::ElementsAre;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

class ComputationPartitionerTest : public HloHardwareIndependentTestBase {
 protected:
  ComputationPartitionerTest() {
    mlir_context_.loadDialect<mlir::func::FuncDialect>();
  }

  mlir::MLIRContext mlir_context_;
};

std::string PrintAndErase(mlir::func::FuncOp func) {
  std::string out;
  llvm::raw_string_ostream os(out);
  os << func;
  // Erase the function so we don't leak memory.
  func.erase();
  return out;
}

TEST_F(ComputationPartitionerTest, PartitionDiamonds) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %param = f32[6] parameter(0)
      %slice0.1 = f32[5] slice(f32[6]{0} %param), slice={[0:5]}
      %slice0.2 = f32[5] slice(f32[6]{0} %param), slice={[1:6]}
      %add0 = f32[5] add(f32[5]{0} %slice0.1, f32[5]{0} %slice0.2)
      %slice1.1 = f32[4] slice(f32[5]{0} %add0), slice={[0:4]}
      %slice1.2 = f32[4] slice(f32[5]{0} %add0), slice={[1:5]}
      %add1 = f32[4] add(f32[4]{0} %slice1.1, f32[4]{0} %slice1.2)
      %slice2.1 = f32[3] slice(f32[4]{0} %add1), slice={[0:3]}
      %slice2.2 = f32[3] slice(f32[4]{0} %add1), slice={[1:4]}
      %add2 = f32[3] add(f32[3]{0} %slice2.1, f32[3]{0} %slice2.2)
      %slice3.1 = f32[2] slice(f32[3]{0} %add2), slice={[0:2]}
      %slice3.2 = f32[2] slice(f32[3]{0} %add2), slice={[1:3]}
      ROOT %add3 = f32[2] add(f32[2]{0} %slice3.1, f32[2]{0} %slice3.2)
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(fusion, &mlir_context_);

  constexpr auto kExpected = R"(PartitionedComputation fused_computation:
      SUBGRAPH fused_computation_add3 {
        %slice3.1 = f32[2]{0} slice(%add2), slice={[0:2]}
        %slice3.2 = f32[2]{0} slice(%add2), slice={[1:3]}
        ROOT %add3 = f32[2]{0} add(%slice3.1, %slice3.2)
      }
      SUBGRAPH fused_computation_add2 {
        %slice2.1 = f32[3]{0} slice(%add1), slice={[0:3]}
        %slice2.2 = f32[3]{0} slice(%add1), slice={[1:4]}
        ROOT %add2 = f32[3]{0} add(%slice2.1, %slice2.2)
      }
      SUBGRAPH fused_computation_add1 {
        %slice1.1 = f32[4]{0} slice(%add0), slice={[0:4]}
        %slice1.2 = f32[4]{0} slice(%add0), slice={[1:5]}
        ROOT %add1 = f32[4]{0} add(%slice1.1, %slice1.2)
      }
      SUBGRAPH fused_computation_add0 {
        %slice0.1 = f32[5]{0} slice(%param), slice={[0:5]}
        %slice0.2 = f32[5]{0} slice(%param), slice={[1:6]}
        ROOT %add0 = f32[5]{0} add(%slice0.1, %slice0.2)
      }
      SUBGRAPH fused_computation_param no_compute {
        ROOT %param = f32[6]{0} parameter(0)
      })";
  EXPECT_EQ(computation.ToString(6), kExpected);
}

TEST_F(ComputationPartitionerTest, SimpleConcatenate) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %param1 = f32[6] parameter(0)
      %param2 = f32[3] parameter(1)
      %neg = f32[6] negate(%param1)
      %exp = f32[3] exponential(%param2)
      ROOT %concat = f32[9] concatenate(%neg, %exp), dimensions={0}
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(fusion, &mlir_context_);

  EXPECT_THAT(computation.subgraphs(), SizeIs(1));
}

TEST_F(ComputationPartitionerTest, DiamondConcatenate) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %param1 = f32[6] parameter(0)
      %param2 = f32[6] parameter(1)
      %log = f32[6] log(%param1)
      %add = f32[6] add(%log, %param2)
      %neg = f32[6] negate(%log)
      %exp = f32[6] exponential(%add)
      ROOT %concat = f32[12] concatenate(%neg, %exp), dimensions={0}
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(fusion, &mlir_context_);

  constexpr auto kExpected = R"(PartitionedComputation fused_computation:
      SUBGRAPH fused_computation_concat {
        %neg = f32[6]{0} negate(%log)
        %param2 = f32[6]{0} parameter(1)
        %add = f32[6]{0} add(%log, %param2)
        %exp = f32[6]{0} exponential(%add)
        ROOT %concat = f32[12]{0} concatenate(%neg, %exp), dimensions={0}
      }
      SUBGRAPH fused_computation_log {
        %param1 = f32[6]{0} parameter(0)
        ROOT %log = f32[6]{0} log(%param1)
      })";
  EXPECT_EQ(computation.ToString(6), kExpected);
}

TEST_F(ComputationPartitionerTest, TupleRoot) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %p0 = f32[6] parameter(0)
      %p1 = f32[6] parameter(1)
      %add = f32[6] add(p0, p1)
      %sub = f32[6] subtract(p0, p1)
      ROOT %root = (f32[6], f32[6]) tuple(%add, %sub)
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(fusion, &mlir_context_);
  constexpr auto kExpected = R"(PartitionedComputation fused_computation:
      SUBGRAPH fused_computation_root {
        %p0 = f32[6]{0} parameter(0)
        %p1 = f32[6]{0} parameter(1)
        %add = f32[6]{0} add(%p0, %p1)
        %sub = f32[6]{0} subtract(%p0, %p1)
        ROOT %root = (f32[6]{0}, f32[6]{0}) tuple(%add, %sub)
      })";
  EXPECT_EQ(computation.ToString(6), kExpected);
}

TEST_F(ComputationPartitionerTest, Epilogue) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module

    add {
      p0 = f32[] parameter(0)
      p1 = f32[] parameter(1)
      ROOT add = f32[] add(p0, p1)
    }

    fused_computation {
      p0 = f32[4] parameter(0)
      c0 = f32[] constant(0)
      reduce = f32[] reduce(p0, c0), dimensions={0}, to_apply=add
      bitcast = f32[1] bitcast(reduce)
      abs = f32[1] abs(bitcast)
      log = f32[1] log(abs)
      sign = f32[1] sign(bitcast)
      ROOT tuple = (f32[1], f32[1]) tuple(log, sign)
    })")
                    .value();

  auto* fused_computation = module->GetComputationWithName("fused_computation");
  EpilogueSpecification epilogue{
      /*heroes=*/{fused_computation->GetInstructionWithName("reduce")},
      /*roots=*/
      {fused_computation->GetInstructionWithName("log"),
       fused_computation->GetInstructionWithName("sign")},
      /*index_ranges=*/{1, 42},
      {CreateIdentityMap(
          fused_computation->root_instruction()->shape().tuple_shapes(0),
          &mlir_context_)}};
  PartitionedComputations fusion(fused_computation, &mlir_context_, {epilogue});

  mlir::ImplicitLocOpBuilder builder(mlir::UnknownLoc::get(&mlir_context_),
                                     &mlir_context_);
  EXPECT_EQ(
      PrintAndErase(
          CreateSubgraphMlirFunction(fusion.epilogues().front(), builder)),
      "func.func private @fused_computation__epilogue__log_sign(tensor<4xf32>, "
      "index {xla.range = [0 : index, 0 : index]}, "
      "index {xla.range = [0 : index, 41 : index]}, "
      "f32) -> (f32, f32)");
}

TEST_F(ComputationPartitionerTest, TransposeAsRoot) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %p0 = f32[64, 32] parameter(0)
      %p1 = f32[64, 32] parameter(1)
      %add = f32[64, 32] add(p0, p1)
      %transpose = f32[32, 64] transpose(%add), dimensions={1, 0}
      %exp = f32[32, 64] exponential(%transpose)
      ROOT %root = f32[32, 64] tanh(%exp)
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(
      fusion, &mlir_context_, [](const HloInstruction* instr) {
        return instr->opcode() == HloOpcode::kTranspose;
      });
  ASSERT_THAT(computation.subgraphs(), SizeIs(2));
  EXPECT_THAT(computation.GetRootSubgraph().roots, SizeIs(1));
  EXPECT_THAT(computation.GetRootSubgraph().instructions, SizeIs(2));
}

TEST_F(ComputationPartitionerTest, TransposeReverse) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %p0 = f32[64, 32] parameter(0)
      %reverse = f32[64, 32] reverse(%p0), dimensions={0}
      %transpose = f32[32, 64] transpose(%reverse), dimensions={1, 0}
      ROOT %root = f32[32, 64] tanh(%transpose)
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(
      fusion, &mlir_context_, [](const HloInstruction* instr) {
        return instr->opcode() == HloOpcode::kTranspose;
      });
  constexpr auto kExpected = R"(PartitionedComputation fused_computation:
      SUBGRAPH fused_computation_root {
        ROOT %root = f32[32,64]{1,0} tanh(%transpose)
      }
      SUBGRAPH fused_computation_transpose no_compute {
        %p0 = f32[64,32]{1,0} parameter(0)
        %reverse = f32[64,32]{1,0} reverse(%p0), dimensions={0}
        ROOT %transpose = f32[32,64]{1,0} transpose(%reverse), dimensions={1,0}
      })";
  EXPECT_EQ(computation.ToString(6), kExpected);
}

TEST_F(ComputationPartitionerTest, PartiallyMergable) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fused_computation {
      %p0 = f32[10,10] parameter(0)
      %p1 = f32[10,10] parameter(1)
      %add = f32[10,10] add(%p0, %p1)
      %transpose = f32[10,10] transpose(%add), dimensions={1,0}
      ROOT %sub = f32[10,10] subtract(%add, %transpose)
    })")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(fusion, &mlir_context_);

  auto transpose = fusion->GetInstructionWithName("transpose");
  auto sub = fusion->GetInstructionWithName("sub");

  ASSERT_THAT(computation.subgraphs(), SizeIs(2));
  EXPECT_THAT(computation.GetRootSubgraph().instructions,
              UnorderedElementsAre(transpose, sub));
}

TEST_F(ComputationPartitionerTest, SubgraphSignatures) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module

    add {
      %p0 = f32[] parameter(0)
      %p1 = f32[] parameter(1)
      ROOT %add = f32[] add(%p0, %p1)
    }

    fusion {
      %p0 = f32[10,10]{0,1} parameter(0)
      %p1 = f32[10,10]{1,0} parameter(1)
      %c0 = f32[] constant(2)
      %bc = f32[10,10]{0,1} bitcast(%p1)
      %add = f32[10,10] add(%p0, %bc)
      ROOT %reduce = f32[10] reduce(%add, %c0), dimensions={1}, to_apply=add
    }

    ENTRY main {
      %p0 = f32[10,10] parameter(0)
      %p1 = f32[10,10] parameter(1)
      ROOT %fusion = f32[10] fusion(%p0, %p1), kind=kLoop, calls=fusion
    })")
                    .value();

  mlir::MLIRContext context;
  context.loadDialect<mlir::func::FuncDialect>();
  mlir::ImplicitLocOpBuilder builder(mlir::UnknownLoc::get(&context), &context);

  PartitionedComputation fusion(module->GetComputationWithName("fusion"),
                                &mlir_context_);
  EXPECT_EQ(
      PrintAndErase(
          CreateSubgraphMlirFunction(fusion.GetRootSubgraph(), builder)),
      "func.func private @fusion_reduce(tensor<10x10xf32, dense<[0, 1]> : "
      "tensor<2xi64>>, tensor<10x10xf32>, index {xla.range = [0 : index, 9 : "
      "index]}) -> f32");

  PartitionedComputation add(module->GetComputationWithName("add"),
                             &mlir_context_);
  EXPECT_EQ(
      PrintAndErase(CreateSubgraphMlirFunction(add.GetRootSubgraph(), builder)),
      "func.func private @add_add(f32, f32) -> f32");
}

TEST_F(ComputationPartitionerTest, ConcatWithTuple) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fusion {
      %p0 = s64[] parameter(0)
      %copy = s64[] copy(s64[] %p0)
      %reshape1 = s64[1]{0} reshape(s64[] %copy)
      %c0 = s64[] constant(0)
      %bcast = s64[4,4]{1,0} broadcast(s64[] %c0), dimensions={}
      %reshape2 = s64[16]{0} reshape(s64[4,4]{1,0} %bcast)
      %concat = s64[17]{0} concatenate(
        s64[1]{0} %reshape1, s64[16]{0} %reshape2), dimensions={0}
      %slice1 = s64[1]{0} slice(s64[17]{0} %concat), slice={[0:1]}
      %slice2 = s64[16]{0} slice(s64[17]{0} %concat), slice={[1:17]}
      ROOT %tuple = (s64[1]{0}, s64[16]{0}) tuple(s64[1]{0} %slice1,
                                                  s64[16]{0} %slice2)
    })")
                    .value();

  mlir::MLIRContext context;
  context.loadDialect<mlir::func::FuncDialect>();
  mlir::ImplicitLocOpBuilder builder(mlir::UnknownLoc::get(&context), &context);

  PartitionedComputation fusion(module->GetComputationWithName("fusion"),
                                &mlir_context_);
  EXPECT_THAT(fusion.subgraphs(), SizeIs(2));
  PrintAndErase(CreateSubgraphMlirFunction(fusion.GetRootSubgraph(), builder));
}

TEST_F(ComputationPartitionerTest, DUS) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    fusion {
      in = c64[2,3] parameter(0)
      updates = c64[2,2] parameter(1)
      i0 = s32[] parameter(2)
      i1 = s32[] parameter(3)
      updated = c64[2,3] dynamic-update-slice(in, updates, i0, i1)
      ROOT negated = c64[2,3] negate(updated)
    })")
                    .value();

  mlir::MLIRContext context;
  context.loadDialect<mlir::func::FuncDialect>();
  mlir::ImplicitLocOpBuilder builder(mlir::UnknownLoc::get(&context), &context);

  PartitionedComputation fusion(module->GetComputationWithName("fusion"),
                                &mlir_context_);
  EXPECT_THAT(fusion.subgraphs(), SizeIs(1));
  PrintAndErase(CreateSubgraphMlirFunction(fusion.GetRootSubgraph(), builder));
}

TEST_F(ComputationPartitionerTest, ScatterFusion) {
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    overwrite {
      %p0 = f16[] parameter(0)
      ROOT %p1 = f16[] parameter(1)
    }
    fusion {
      %param_0 = f16[5,10]{1,0} parameter(0)
      %iota.3.1 = s64[5,3,1] iota(), iota_dimension=0
      %param_2.4 = s64[5,3,1] parameter(2)
      %concatenate.3.1 = s64[5,3,2] concatenate(s64[5,3,1] %iota.3.1,
      s64[5,3,1] %param_2.4), dimensions={2}
      %bitcast.55.1 = s64[15,2]{1,0} bitcast(s64[5,3,2] %concatenate.3.1)
      %param_1.1 = f16[5,3,2] parameter(1)
      %bitcast.59.1 = f16[15,1,2] bitcast(f16[5,3,2] %param_1.1)
      ROOT %scatter.5.1 = f16[5,10]{1,0} scatter(
        f16[5,10]{1,0} %param_0,
        s64[15,2]{1,0} %bitcast.55.1,
        f16[15,1,2]{2,1,0} %bitcast.59.1),
        update_window_dims={1,2},
        inserted_window_dims={},
        scatter_dims_to_operand_dims={0,1},
        index_vector_dim=1,
        to_apply=%overwrite
    })")
                    .value();

  mlir::MLIRContext context;
  context.loadDialect<mlir::func::FuncDialect>();
  mlir::ImplicitLocOpBuilder builder(mlir::UnknownLoc::get(&context), &context);

  PartitionedComputation fusion(module->GetComputationWithName("fusion"),
                                &mlir_context_);
  EXPECT_THAT(fusion.subgraphs(), SizeIs(1));
  PrintAndErase(CreateSubgraphMlirFunction(fusion.GetRootSubgraph(), builder));
}

TEST_F(ComputationPartitionerTest, PartitioningIsDeterministic) {
  // This is a fusion that used to result in non-deterministic partitioning.
  auto module = ParseAndReturnVerifiedModule(R"(
    HloModule test_module
    ENTRY fused_computation {
      constant_15990_184 = s32[] constant(0)
      broadcast.59771.33 = s32[16]{0} broadcast(constant_15990_184), dimensions={}
      param_1.31375 = s32[16]{0} parameter(1)
      constant_15774_2 = s32[] constant(262143)
      broadcast.59775.7 = s32[16]{0} broadcast(constant_15774_2), dimensions={}
      clamp.6.7 = s32[16]{0} clamp(broadcast.59771.33, param_1.31375, broadcast.59775.7)
      param_2.20600 = u32[] parameter(2)
      convert.12580.11 = s32[] convert(param_2.20600)
      constant_15783_5 = s32[] constant(32768)
      multiply.10392.11 = s32[] multiply(convert.12580.11, constant_15783_5)
      broadcast.59778.9 = s32[16]{0} broadcast(multiply.10392.11), dimensions={}
      compare.12185.5 = pred[16]{0} compare(clamp.6.7, broadcast.59778.9), direction=LT
      constant_15997_1 = s32[] constant(32767)
      add.7199.4 = s32[] add(multiply.10392.11, constant_15997_1)
      broadcast.59780.6 = s32[16]{0} broadcast(add.7199.4), dimensions={}
      compare.12186.5 = pred[16]{0} compare(clamp.6.7, broadcast.59780.6), direction=GT
      or.384.3 = pred[16]{0} or(compare.12185.5, compare.12186.5)
      broadcast.59957.1 = pred[1,16,4096]{2,1,0} broadcast(or.384.3), dimensions={1}
      constant_16001_1 = bf16[] constant(0)
      broadcast.59959.1 = bf16[1,16,4096]{2,1,0} broadcast(constant_16001_1), dimensions={}
      param_0.9142 = bf16[32768,4096]{1,0} parameter(0)
      clamp.7.1 = s32[16]{0} clamp(broadcast.59778.9, clamp.6.7, broadcast.59780.6)
      subtract.4322.1 = s32[16]{0} subtract(clamp.7.1, broadcast.59778.9)
      bitcast.3.3 = s32[16,1]{1,0} bitcast(subtract.4322.1)
      gather.34.3 = bf16[16,1,4096]{2,0,1} gather(param_0.9142, bitcast.3.3), offset_dims={1,2}, collapsed_slice_dims={}, start_index_map={0}, index_vector_dim=1, slice_sizes={1,4096}
      bitcast.4104.1 = bf16[1,16,4096]{2,1,0} bitcast(gather.34.3)
      ROOT select.8499.1 = bf16[1,16,4096]{2,1,0} select(broadcast.59957.1, broadcast.59959.1, bitcast.4104.1)
    }
)")
                    .value();

  auto* fusion = module->GetComputationWithName("fused_computation");
  ASSERT_NE(fusion, nullptr);
  PartitionedComputation computation(fusion, &mlir_context_);
  EXPECT_EQ(computation.subgraphs().size(), 1);
}

}  // namespace
}  // namespace emitters
}  // namespace xla
