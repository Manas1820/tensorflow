/* Copyright 2017 The OpenXLA Authors.

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

#include "xla/hlo/transforms/simplifiers/hlo_constant_folding.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "xla/hlo/evaluator/hlo_evaluator.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/literal.h"
#include "xla/service/slow_operation_alarm.h"
#include "xla/shape_util.h"
#include "tsl/platform/errors.h"

namespace xla {

// Checks whether instr is or transitively contains an instruction that we
// shouldn't fold.
//
// Specifically, we don't fold kRng or kAfterAll instructions:
//
//  - kRng is already marked as side-effecting and so is skipped elsewhere, but
//    we check for it here.  Even kRng weren't side-effecting and took an
//    explicit seed, we *still* wouldn't want to constant-fold it, because the
//    evaluator's handling of rng is not guaranteed to be identical to any
//    particular backend's rng.
//
//  - kAfterAll needs to be skipped because a kAfterAll op with no args can
//    currently materialize a token "out of thin air".  TODO(b/110532604):
//    Remove this check once AfterAll requires at least one operand, in which
//    case constant folding will be impossible.
static bool IsOrContainsIllegalInstr(const HloInstruction* instr) {
  if (instr->opcode() == HloOpcode::kAfterAll ||
      instr->opcode() == HloOpcode::kRng) {
    return true;
  }
  for (const HloComputation* c : instr->called_computations()) {
    if (absl::c_any_of(c->instructions(), IsOrContainsIllegalInstr)) {
      return true;
    }
  }
  return false;
}

// Checks if  any of the operands of the instruction are constants.
// Any tuples are recursively checked.
bool AnyOperandsConstant(const HloInstruction* instr) {
  for (const HloInstruction* operand : instr->operands()) {
    HloOpcode opcode = operand->opcode();

    if (opcode == HloOpcode::kTuple) {
      if (AnyOperandsConstant(operand)) {
        return true;
      }
    } else if (opcode == HloOpcode::kConstant) {
      return true;
    }
  }

  return false;
}

// Checks if all of the operands of the instruction are constants or broadcasts
// of constants (iota is a broadcast of a constant from the standpoint of
// constant folding).
// Any tuples are recursively checked.
bool AllOperandsConstantOrBroadcastConstant(const HloInstruction* instr) {
  for (const HloInstruction* operand : instr->operands()) {
    HloOpcode opcode = operand->opcode();

    if (opcode == HloOpcode::kTuple) {
      if (!AllOperandsConstantOrBroadcastConstant(operand)) {
        return false;
      }
    } else if (opcode != HloOpcode::kConstant &&
               !(opcode == HloOpcode::kBroadcast &&
                 operand->operand(0)->opcode() == HloOpcode::kConstant) &&
               opcode != HloOpcode::kIota) {
      return false;
    }
  }

  return true;
}

/*static*/ std::atomic<int64_t> HloConstantFolding::slow_op_counter_{0};

absl::Status RecursivelyRemoveDeadInstructionAndDeadOperands(
    HloComputation& computation, HloInstruction* instruction) {
  absl::flat_hash_set<HloInstruction*> already_removed;
  std::vector<HloInstruction*> dead_instructions = {instruction};
  while (!dead_instructions.empty()) {
    auto dead_instruction = dead_instructions.back();
    dead_instructions.pop_back();
    if (already_removed.insert(dead_instruction).second == false) {
      continue;
    }

    // Save the operands before calling RemoveInstruction which clears them.
    auto operands = dead_instruction->operands();

    // First remove the instruction itself.
    TF_RETURN_IF_ERROR(computation.RemoveInstruction(dead_instruction));

    // Now check if some of its operands are dead as a result of the removal.
    for (auto operand : operands) {
      if (operand->IsDead()) {
        dead_instructions.push_back(operand);
      }
    }
  }
  return absl::OkStatus();
}

namespace {

bool IsFoldable(
    const HloInstruction* instruction, HloConstantFolding::Level level,
    absl::flat_hash_map<HloComputation*, bool>& is_foldable_computation) {
  // Broadcasts dramatically increase the size of constants, which is often
  // detrimental to performance and memory capacity, so do not fold
  // broadcasts.
  if (instruction->opcode() == HloOpcode::kBroadcast ||
      instruction->opcode() == HloOpcode::kIota) {
    return false;
  }

  // Do not fold FFT. Evaluating it may significantly increase compile time.
  if (instruction->opcode() == HloOpcode::kFft) {
    return false;
  }

  // Skip while loops as they can significantly increase compile times.
  if (level == HloConstantFolding::Level::kDefault &&
      instruction->opcode() == HloOpcode::kWhile) {
    return false;
  }

  // Don't fold across async execution thread if it's not supposed to be
  // changed by this pass.
  if (instruction->IsAsynchronous() &&
      instruction->async_execution_thread() !=
          instruction->parent()->execution_thread()) {
    return false;
  }

  // Don't fold if any of the subcomputations are not foldable. Note that this
  // will recurse into deeper called computations.
  for (HloComputation* subcomputation : instruction->called_computations()) {
    auto iter = is_foldable_computation.find(subcomputation);
    if (iter == is_foldable_computation.end()) {
      for (auto* sub_instruction : subcomputation->MakeInstructionPostOrder()) {
        if (!IsFoldable(sub_instruction, level, is_foldable_computation)) {
          is_foldable_computation[subcomputation] = false;
          return false;
        }
      }
      is_foldable_computation[subcomputation] = true;
    } else if (!iter->second) {
      return false;
    }
  }

  // Check for instructions that we can't fold even if they appear inside of
  // a subcomputation (e.g. a kCall).
  if (IsOrContainsIllegalInstr(instruction)) {
    return false;
  }

  // Don't constant-fold side-effecting instructions or instructions which
  // contain side-effecting instructions.
  if (instruction->HasSideEffect()) {
    return false;
  }

  // Skip constant folding for instructions that have control dependencies.
  if (instruction->HasControlDependencies()) {
    return false;
  }

  // Reduce the compile time by skipping the constant folding of pad
  // instruction with broadcast operand. With 45m shape limit the compile
  // time could be more than 30 seconds. According to the current
  // benchmarks it does not affect the performance.
  if (instruction->opcode() == HloOpcode::kPad &&
      instruction->operand(0)->opcode() == HloOpcode::kBroadcast &&
      instruction->operand(1)->opcode() == HloOpcode::kConstant) {
    return false;
  }

  // Don't constant fold unless output and operand sizes are small.
  if (level == HloConstantFolding::Level::kDefault &&
      instruction->shape().IsArray()) {
    int64_t elements_in_operands = 0;
    for (HloInstruction* operand : instruction->operands()) {
      if (operand->shape().IsArray()) {
        elements_in_operands += ShapeUtil::ElementsIn(operand->shape());
      }
    }
    int64_t elements_in_constant = ShapeUtil::ElementsIn(instruction->shape());

    static const int64_t kMaximumConstantSizeElements = 45 * 1000 * 1000;
    if (std::max(elements_in_constant, elements_in_operands) >
        kMaximumConstantSizeElements) {
      VLOG(2) << "Ignore constant folding: result shape size is "
              << elements_in_constant << " total size of arguments is "
              << elements_in_operands;
      return false;
    }
  }
  return true;
}
}  // namespace

absl::StatusOr<bool> HloConstantFolding::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  // Limit the constant folding to 0 iterations to skip folding loops in the
  // default case. This retains the behavior from before while loop support in
  // HloEvaluator and may be revised.
  auto evaluator = std::make_unique<HloEvaluator>(
      /*max_loop_iterations=*/level_ == Level::kAggressive ? -1 : 0);
  // fast-path lets us e.g. use Eigen for matmuls.
  evaluator->set_use_fast_path(true);

  bool changed = false;

  // For each computation, cache whether we can fold all the instructions in it.
  absl::flat_hash_map<HloComputation*, bool> is_foldable_computation;

  for (auto* computation :
       module->MakeNonfusionComputations(execution_threads)) {
    for (auto* instruction : computation->MakeInstructionPostOrder()) {
      // Skip dead code.
      if (instruction->IsDead()) {
        continue;
      }

      // We only handle instructions where
      //
      //  - at least one operand is a constant, and
      //  - all other operands are either constants or broadcast(constant).
      //
      // Why this particular set of rules around broadcasts?
      //
      //  - We don't want to fold broadcast(constant) on its own, because in
      //    general it's "simpler" to remember that it's a broadcast.  Also,
      //    algsimp will fold an all-one-value constant into a broadcast, so
      //    we'd just end up fighting with it.
      //
      //  - We don't want to fold an op where all operands are broadcasts of
      //    constants, because algsimp will transform op(broadcast(constant) =>
      //    broadcast(op(constant)).  Then we can constant-fold the smaller op.
      //
      //  - So the only remaining case is where some but not all operands are
      //    broadcasts of constants, e.g. op(constant, broadcast(constant)).
      //
      // TODO(b/260601110): We may want to specialize calls and propagate
      // constants into them even if only some operands are constants.
      if (level_ == HloConstantFolding::Level::kDefault &&
          !AnyOperandsConstant(instruction)) {
        continue;
      }
      if (!AllOperandsConstantOrBroadcastConstant(instruction)) {
        continue;
      }

      // Don't fold Constant, Parameter, and Tuple instructions.  Tuple
      // constants are not directly supported by any backends, hence folding
      // Tuple is not useful and would in fact be expanded back into kTuple by
      // Algebraic Simplifier.
      //
      // (We do allow folding subcomputations that contain these instructions.)
      if (instruction->opcode() == HloOpcode::kParameter ||
          instruction->opcode() == HloOpcode::kConstant ||
          instruction->opcode() == HloOpcode::kTuple) {
        continue;
      }

      if (!IsFoldable(instruction, level_, is_foldable_computation)) {
        continue;
      }
      VLOG(5) << "Constant folding: " << instruction->ToString();

      absl::Duration slow_timeout =
          absl::Seconds(uint64_t{1} << slow_op_counter_.load());
      SlowOperationAlarm slow_alarm(slow_timeout, [instruction, slow_timeout] {
#if NDEBUG
        absl::string_view explanation_msg =
            "This isn't necessarily a bug; constant-folding is "
            "inherently a trade-off between compilation time and speed "
            "at runtime. XLA has some guards that attempt to keep "
            "constant folding from taking too long, but fundamentally "
            "you'll always be able to come up with an input program that "
            "takes a long time.\n\n"
            "If you'd like to file a bug, run with envvar "
            "XLA_FLAGS=--xla_dump_to=/tmp/foo and attach the results.";
#else
        absl::string_view explanation_msg =
            "XLA was built without compiler optimizations, which can be "
            "slow. Try rebuilding with -c opt.";
#endif
        return absl::StrFormat(
            "Constant folding an instruction is taking > %s:\n\n"
            "  %s\n\n"  // instruction->name() or instruction->ToString()
            "%s",       // explanation_msg
            absl::FormatDuration(slow_timeout), instruction->ToString(),
            explanation_msg);
      });

      // Currently we skip unimplemented operations.
      Literal result;
      if (!evaluator->TryEvaluate(
              instruction, &result,
              /*recursively_evaluate_nonconstant_operands=*/true)) {
        VLOG(2) << "Constant folding failed for instruction: "
                << instruction->ToString();
        continue;
      }

      slow_alarm.cancel();
      if (slow_alarm.fired()) {
        slow_op_counter_++;
      }

      VLOG(4) << "Constant folded: " << instruction->ToString();
      changed = true;
      HloInstruction* new_constant = instruction->AddInstruction(
          HloInstruction::CreateConstant(std::move(result)));
      if (new_constant->shape().has_layout()) {
        // Update element_size_in_bits on the new instruction's layout. Literals
        // always have element_size_in_bits set to 0, and CreateConstant copies
        // the shape/layout from the Literal, so we need to set
        // element_size_in_bits here.
        new_constant->mutable_shape()
            ->mutable_layout()
            ->set_element_size_in_bits(
                instruction->shape().layout().element_size_in_bits());
      }
      TF_RETURN_IF_ERROR(instruction->ReplaceAllUsesWith(new_constant));
      TF_RETURN_IF_ERROR(RecursivelyRemoveDeadInstructionAndDeadOperands(
          *computation, instruction));
    }
  }
  return changed;
}

}  // namespace xla
