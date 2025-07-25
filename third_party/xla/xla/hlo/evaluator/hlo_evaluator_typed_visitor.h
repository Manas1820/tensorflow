/* Copyright 2018 The OpenXLA Authors.

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

#ifndef XLA_HLO_EVALUATOR_HLO_EVALUATOR_TYPED_VISITOR_H_
#define XLA_HLO_EVALUATOR_HLO_EVALUATOR_TYPED_VISITOR_H_

#include <fenv.h>  // NOLINT

#include <algorithm>
#include <bitset>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "xla/array2d.h"
#include "xla/hlo/evaluator/hlo_evaluator.h"
#include "xla/hlo/ir/dfs_hlo_visitor_with_default.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/index_util.h"
#include "xla/layout.h"
#include "xla/layout_util.h"
#include "xla/literal.h"
#include "xla/primitive_util.h"
#include "xla/service/shape_inference.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status_macros.h"
#include "xla/tsl/platform/errors.h"
#include "xla/tsl/platform/status.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/types.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"

namespace xla {

template <typename T>
T Nibble0(T t) {
  if constexpr (std::is_integral_v<T>) {
    constexpr auto shift = (8 * sizeof(T)) - 4;
    return (t << shift) >> shift;
  }
  return t;
}

template <typename T>
T Nibble1(T t) {
  if constexpr (std::is_integral_v<T>) {
    return t >> 4;
  }
  return t;
}

namespace detail {
template <typename T>
using unsigned_promoted_type_t =
    std::make_unsigned_t<decltype(std::declval<T>() + std::declval<T>())>;
}

// ToArithmeticSafeType(T t):
//  - converts `t` to an unsigned integer at least as wide as `int` if T is an
//    integer, and
//  - otherwise returns `t` unchanged.
//
// It's UB in C++ to under/overflow a signed integer, so we wrap all arithmetic
// in this type to force 2's complement behavior.
template <typename T>
auto ToArithmeticSafeType(T t) {
  if constexpr (std::is_integral_v<T>) {
    return static_cast<detail::unsigned_promoted_type_t<T>>(t);
  }
  if constexpr (!std::is_integral_v<T>) {
    return t;
  }
}

// Templated DfsHloVisitor for use by HloEvaluator.
//
// Typically ReturnT here indicates the resulting literal type of each evaluated
// Handle* method of a TypedVisitor.  There are however a few exceptions to this
// rule, notably:
// - HandleCompare and HandleIsFinite: where the resulting literal type is
//   always boolean.
// - HandleImag and HandleReal: where the resulting literal type is always float
//   and the operand is always complex, or real in the case of HandleReal.
// These operations are handled outside of the parent HloEvaluator handlers
// instead of from within TypedVisitor.
//
// Type params:
//   - ReturnT: The type of input and output of each operation.
//   - ElementwiseT: The type in which internal computation are done.
//
// This is logically a private part of HloEvaluator.  It lives in this header
// file rather than in hlo_evaluator.cc because we use extern templates and a
// bunch of independent cc files to speed up compiling the many instantiations
// of this class.
//
// NOTE: Prefer putting new implementation to HloEvalator rather than
// HloEvaluatorTypedVisitor whenever possible, because this class is templated
// for all primitive types and is an order of magnitude larger in code size as
// well as compile time. Only put op handling that involves compute using native
// C++ types here, such as elementwise ops with compute, convolution, dot, etc.
template <typename ReturnT, typename ElementwiseT = ReturnT>
class HloEvaluatorTypedVisitor : public ConstDfsHloVisitorWithDefault {
 private:
  ABSL_ATTRIBUTE_NOINLINE absl::Status UnsupportedTypeError(
      const HloInstruction* instruction) {
    return InvalidArgument(
        "Unsupported type for %s: %s", HloOpcodeString(instruction->opcode()),
        PrimitiveType_Name(instruction->shape().element_type()));
  }

  // Returns `shape`, if it has a layout, or a copy of `shape` with the default
  // layout if it doesn't. Some functions require shapes to have layouts, so we
  // simply always set one.
  Shape GetShapeWithLayout(const Shape& shape) {
    CHECK(shape.IsArray());
    Shape shape_copy = shape;
    if (!shape.has_layout()) {
      LayoutUtil::SetToDefaultLayout(&shape_copy);
    }
    return shape_copy;
  }

 public:
  explicit HloEvaluatorTypedVisitor(HloEvaluator* p) : parent_(p) {}

  // Converts a UnaryOp from a unary function on ElementwiseT to a unary
  // function on ReturnT types.
  template <typename UnaryOp>
  auto ConvertUnaryFunction(const UnaryOp& unary_op) {
    return [&unary_op](ReturnT arg) {
      return static_cast<ReturnT>(unary_op(static_cast<ElementwiseT>(arg)));
    };
  }

  // Converts a BinaryOp from a binary function on ElementwiseT to a binary
  // function on ReturnT types.
  template <typename BinaryOp>
  auto ConvertBinaryFunction(const BinaryOp& binary_op) {
    return [&binary_op](ReturnT arg1, ReturnT arg2) {
      return static_cast<ReturnT>(binary_op(static_cast<ElementwiseT>(arg1),
                                            static_cast<ElementwiseT>(arg2)));
    };
  }

  // Converts a TernaryOp from a ternary function on ElementwiseT to a ternary
  // function on ReturnT types.
  template <typename TernaryOp>
  auto ConvertTernaryFunction(const TernaryOp& ternary_op) {
    return [&ternary_op](ReturnT arg1, ReturnT arg2, ReturnT arg3) {
      return static_cast<ReturnT>(ternary_op(static_cast<ElementwiseT>(arg1),
                                             static_cast<ElementwiseT>(arg2),
                                             static_cast<ElementwiseT>(arg3)));
    };
  }

  absl::Status DefaultAction(const HloInstruction* hlo_instruction) override {
    return Unimplemented("unhandled HLO ops for HloEvaluator: %s.",
                         HloOpcodeString(hlo_instruction->opcode()));
  }

  template <typename NativeT,
            typename std::enable_if_t<std::is_unsigned_v<NativeT>>* = nullptr>
  absl::Status HandleAbs(const HloInstruction* abs) {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseUnaryOp(abs, [](NativeT elem_operand) {
                          return elem_operand;
                        }));
    parent_->SetEvaluatedLiteralFor(abs, std::move(literal));
    return absl::OkStatus();
  }

  template <typename NativeT,
            typename std::enable_if_t<std::is_signed_v<NativeT>>* = nullptr>
  absl::Status HandleAbs(const HloInstruction* abs) {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseUnaryOp(abs, [](NativeT elem_operand) {
                          return std::abs(elem_operand);
                        }));
    parent_->SetEvaluatedLiteralFor(abs, std::move(literal));
    return absl::OkStatus();
  }

  template <typename NativeT,
            typename std::enable_if_t<is_complex_v<NativeT>>* = nullptr>
  absl::Status HandleAbs(const HloInstruction* abs) {
    const Literal& operand_literal =
        parent_->GetEvaluatedLiteralFor(abs->operand(0));
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        (HloEvaluator::ElementWiseUnaryOpImpl<typename NativeT::value_type,
                                              NativeT>(
            abs, [](NativeT elem_operand) { return std::abs(elem_operand); },
            operand_literal)));
    parent_->SetEvaluatedLiteralFor(abs, std::move(literal));

    return absl::OkStatus();
  }

  absl::Status HandleAbs(const HloInstruction* abs) override {
    // If the operand is of C64 type, the return type of abs will be F32.
    // However, ElementwiseT would still be the return type, F32, and thus
    // specifying the ElementwiseT explicitly as C64 is needed below.
    if (abs->operand(0)->shape().element_type() == C64) {
      return HandleAbs<complex64>(abs);
    } else if (abs->operand(0)->shape().element_type() == C128) {
      return HandleAbs<complex128>(abs);
    }
    return HandleAbs<ElementwiseT>(abs);
  }

  absl::Status HandleRound(const HloInstruction* round) override {
    if constexpr (!is_complex_v<ReturnT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(round, [](ElementwiseT elem_operand) {
            return std::round(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(round, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(round);
  }

  absl::Status HandleRoundNearestEven(const HloInstruction* round) override {
    if constexpr (!is_complex_v<ReturnT>) {
      // Verify the current rounding direction.
      TF_RET_CHECK(fegetround() == FE_TONEAREST);
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(round, [](ElementwiseT elem_operand) {
            return std::nearbyint(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(round, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(round);
  }

  absl::Status HandleCeil(const HloInstruction* ceil) override {
    if constexpr (!is_complex_v<ReturnT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(ceil, [](ElementwiseT elem_operand) {
            return std::ceil(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(ceil, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(ceil);
  }

  absl::Status HandleErf(const HloInstruction* erf) override {
    if constexpr (!is_complex_v<ReturnT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(erf, [](ElementwiseT elem_operand) {
            return std::erf(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(erf, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(erf);
  }

  absl::Status HandleExp(const HloInstruction* exp) override {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseUnaryOp(exp, [](ElementwiseT elem_operand) {
                          return std::exp(elem_operand);
                        }));
    parent_->SetEvaluatedLiteralFor(exp, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleExpm1(const HloInstruction* expm1) override {
    if constexpr (!is_complex_v<ReturnT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(expm1, [](ElementwiseT elem_operand) {
            return std::expm1(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(expm1, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(expm1);
  }

  absl::Status HandleFloor(const HloInstruction* floor) override {
    if constexpr (!is_complex_v<ReturnT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(floor, [](ElementwiseT elem_operand) {
            return std::floor(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(floor, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(floor);
  }

  absl::Status HandleLog(const HloInstruction* log) override {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseUnaryOp(log, [](ElementwiseT elem_operand) {
                          return std::log(elem_operand);
                        }));
    parent_->SetEvaluatedLiteralFor(log, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleLog1p(const HloInstruction* log1p) override {
    if constexpr (!is_complex_v<ReturnT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(log1p, [](ElementwiseT elem_operand) {
            return std::log1p(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(log1p, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(log1p);
  }

  absl::Status HandleNot(const HloInstruction* not_) override {
    if constexpr (std::is_arithmetic_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(not_, [](ElementwiseT elem_operand) {
            if constexpr (std::is_floating_point_v<ElementwiseT> ||
                          std::is_same_v<ElementwiseT, bool>) {
              return !elem_operand;
            } else {
              static_assert(std::is_integral_v<ElementwiseT>);
              return ~elem_operand;
            }
          }));
      parent_->SetEvaluatedLiteralFor(not_, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(not_);
  }

  template <
      typename NativeT,
      typename std::enable_if_t<std::is_signed_v<NativeT> &&
                                !std::is_floating_point_v<NativeT>>* = nullptr>
  absl::Status HandleNegate(const HloInstruction* negate) {
    using type = std::make_unsigned_t<NativeT>;
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseUnaryOp(negate, [](ElementwiseT elem_operand) {
          return NativeT(-type(elem_operand));
        }));
    parent_->SetEvaluatedLiteralFor(negate, std::move(literal));
    return absl::OkStatus();
  }

  template <typename NativeT, typename std::enable_if_t<
                                  !std::is_signed_v<NativeT> ||
                                  std::is_floating_point_v<NativeT>>* = nullptr>
  absl::Status HandleNegate(const HloInstruction* negate) {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseUnaryOp(
            negate, [](ElementwiseT elem_operand) { return -elem_operand; }));
    parent_->SetEvaluatedLiteralFor(negate, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleNegate(const HloInstruction* negate) override {
    return HandleNegate<ReturnT>(negate);
  }

  absl::Status HandleLogistic(const HloInstruction* logistic) override {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseUnaryOp(logistic, [](ElementwiseT elem_operand) {
          return static_cast<ElementwiseT>(1) /
                 (static_cast<ElementwiseT>(1) + std::exp(-elem_operand));
        }));
    parent_->SetEvaluatedLiteralFor(logistic, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleSign(const HloInstruction* sign) override {
    using NativeT = ElementwiseT;
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseUnaryOp(sign, [](ElementwiseT elem_operand) {
          if constexpr (std::is_integral_v<NativeT>) {
            return (ElementwiseT(0) < elem_operand) -
                   (elem_operand < ElementwiseT(0));
          }
          if constexpr (std::is_floating_point_v<ElementwiseT>) {
            return std::isnan(elem_operand)
                       ? elem_operand
                       : std::copysign(elem_operand != ElementwiseT(0),
                                       elem_operand);
          }
          if constexpr (is_complex_v<NativeT>) {
            auto abs_val = std::abs(elem_operand);
            return 0 == abs_val ? ElementwiseT(0) : elem_operand / abs_val;
          }
        }));
    parent_->SetEvaluatedLiteralFor(sign, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleAtan2(const HloInstruction* atan2) override {
    if constexpr (std::is_floating_point_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(Literal literal,
                          ElementWiseBinaryOp(atan2, [](ElementwiseT lhs_elem,
                                                        ElementwiseT rhs_elem) {
                            return std::atan2(lhs_elem, rhs_elem);
                          }));
      parent_->SetEvaluatedLiteralFor(atan2, std::move(literal));
      return absl::OkStatus();
    }
    if constexpr (is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseBinaryOp(atan2, [](ElementwiseT y, ElementwiseT x) {
            // atan2(y,x) = -i * log((x + i * y)/sqrt(x**2+y**2))
            auto i = ElementwiseT(0.0, 1.0);
            return (-i) * (std::log((x + i * y) / std::sqrt(x * x + y * y)));
          }));
      parent_->SetEvaluatedLiteralFor(atan2, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(atan2);
  }

  absl::Status HandleTanh(const HloInstruction* tanh) override {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseUnaryOp(tanh, [](ElementwiseT elem_operand) {
                          return std::tanh(elem_operand);
                        }));
    parent_->SetEvaluatedLiteralFor(tanh, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleMultiply(const HloInstruction* multiply) override {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseBinaryOp(
            multiply, [](ElementwiseT lhs_elem, ElementwiseT rhs_elem) {
              return ElementwiseT(ToArithmeticSafeType(lhs_elem) *
                                  ToArithmeticSafeType(rhs_elem));
            }));
    parent_->SetEvaluatedLiteralFor(multiply, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleSubtract(const HloInstruction* subtract) override {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseBinaryOp(
            subtract, [](ElementwiseT lhs_elem, ElementwiseT rhs_elem) {
              return ElementwiseT(ToArithmeticSafeType(lhs_elem) -
                                  ToArithmeticSafeType(rhs_elem));
            }));
    parent_->SetEvaluatedLiteralFor(subtract, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleAdd(const HloInstruction* add) override {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseBinaryOp(add, [](ElementwiseT lhs_elem,
                                                    ElementwiseT rhs_elem) {
                          return ElementwiseT(ToArithmeticSafeType(lhs_elem) +
                                              ToArithmeticSafeType(rhs_elem));
                        }));
    parent_->SetEvaluatedLiteralFor(add, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleDivide(const HloInstruction* divide) override {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseBinaryOp(
            divide,
            [](ElementwiseT lhs_elem, ElementwiseT rhs_elem) -> ElementwiseT {
              if constexpr (std::is_integral_v<ElementwiseT>) {
                if constexpr (std::is_unsigned_v<ElementwiseT>) {
                  if (rhs_elem == 0) {
                    return std::numeric_limits<ElementwiseT>::max();
                  }
                }
                if constexpr (std::is_signed_v<ElementwiseT>) {
                  if (rhs_elem == 0) {
                    return static_cast<ElementwiseT>(-1);
                  }
                  if (rhs_elem == -1 &&
                      lhs_elem == std::numeric_limits<ElementwiseT>::min()) {
                    return lhs_elem;
                  }
                }
              }
              return lhs_elem / rhs_elem;
            }));
    parent_->SetEvaluatedLiteralFor(divide, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleMaximum(const HloInstruction* maximum) override {
    if constexpr (!is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseBinaryOp(maximum, [](ElementwiseT lhs, ElementwiseT rhs) {
            if constexpr (std::numeric_limits<ElementwiseT>::has_quiet_NaN) {
              if (std::isnan(lhs)) {
                return lhs;
              }
              if (std::isnan(rhs)) {
                return rhs;
              }
            }
            return std::max(lhs, rhs);
          }));
      parent_->SetEvaluatedLiteralFor(maximum, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(maximum);
  }

  absl::Status HandleMinimum(const HloInstruction* minimum) override {
    if constexpr (!is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseBinaryOp(minimum, [](ElementwiseT lhs, ElementwiseT rhs) {
            if constexpr (std::numeric_limits<ElementwiseT>::has_quiet_NaN) {
              if (std::isnan(lhs)) {
                return lhs;
              }
              if (std::isnan(rhs)) {
                return rhs;
              }
            }
            return std::min(lhs, rhs);
          }));
      parent_->SetEvaluatedLiteralFor(minimum, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(minimum);
  }

  absl::Status HandlePower(const HloInstruction* power) override {
    TF_ASSIGN_OR_RETURN(
        Literal literal, ElementWiseBinaryOp(power, [](ElementwiseT lhs_el,
                                                       ElementwiseT rhs_el) {
          // Case 0: 1^x = 1 and x^0 = 1, regardless of X, see
          // Branch Cuts for Complex Elementary Functions or Much Ado About
          // Nothing's Sign Bit, W. Kahan, Section 10.
          if (lhs_el == ElementwiseT(1) || rhs_el == ElementwiseT(0)) {
            return static_cast<ElementwiseT>(1);
          }
          // Case 1:
          // 1. inf^(a + 0i) = inf, if a > 0.
          // 2. inf^(a + 0i) = 0, if a < 0.
          if constexpr (is_complex_v<ElementwiseT>) {
            auto is_positive_infinity = [](ElementwiseT c) {
              return c.imag() == 0 && c.real() > 0 && std::isinf(c.real());
            };
            auto is_positive_real = [](ElementwiseT c) {
              return c.real() > 0 && c.imag() == 0;
            };
            auto is_negative_real = [](ElementwiseT c) {
              return c.real() < 0 && c.imag() == 0;
            };
            if (is_positive_infinity(lhs_el) && is_positive_real(rhs_el)) {
              return static_cast<ElementwiseT>(lhs_el);
            }
            if (is_positive_infinity(lhs_el) && is_negative_real(rhs_el)) {
              return static_cast<ElementwiseT>(0);
            }
          }
          // Case 2:
          // Fallback to pow.
          if constexpr (std::is_same_v<ElementwiseT, bool>) {
            return lhs_el || !rhs_el;
          } else if constexpr (std::is_integral_v<ElementwiseT>) {
            if constexpr (std::is_signed_v<ElementwiseT>) {
              if (rhs_el < static_cast<ElementwiseT>(0)) {
                return static_cast<ElementwiseT>(
                    lhs_el == static_cast<ElementwiseT>(1) ? 1 : 0);
              }
            }
            return static_cast<ElementwiseT>(
                IPow<std::make_unsigned_t<ElementwiseT>>(lhs_el, rhs_el));
          } else {
            return static_cast<ElementwiseT>(std::pow(lhs_el, rhs_el));
          }
        }));
    parent_->SetEvaluatedLiteralFor(power, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleSqrt(const HloInstruction* sqrt) override {
    TF_ASSIGN_OR_RETURN(Literal literal,
                        ElementWiseUnaryOp(sqrt, [](ElementwiseT elem_operand) {
                          return std::sqrt(elem_operand);
                        }));
    parent_->SetEvaluatedLiteralFor(sqrt, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleCbrt(const HloInstruction* cbrt) override {
    if constexpr (!is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(cbrt, [](ElementwiseT elem_operand) {
            return std::cbrt(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(cbrt, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(cbrt);
  }

  absl::Status HandleRsqrt(const HloInstruction* rsqrt) override {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseUnaryOp(rsqrt, [](ElementwiseT elem_operand) {
          return static_cast<ElementwiseT>(1) / std::sqrt(elem_operand);
        }));
    parent_->SetEvaluatedLiteralFor(rsqrt, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleRemainder(const HloInstruction* remainder) override {
    if constexpr (!is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseBinaryOp(
              remainder,
              [](ElementwiseT lhs_el, ElementwiseT rhs_el) -> ElementwiseT {
                if constexpr (std::is_integral_v<ElementwiseT>) {
                  if (rhs_el == 0) {
                    return lhs_el;
                  }
                  if constexpr (std::is_signed_v<ElementwiseT>) {
                    if (rhs_el == -1 &&
                        lhs_el == std::numeric_limits<ElementwiseT>::min()) {
                      return 0;
                    }
                  }
                  return lhs_el % rhs_el;
                }
                if constexpr (std::is_floating_point_v<ElementwiseT>) {
                  return std::fmod(lhs_el, rhs_el);
                }
              }));
      parent_->SetEvaluatedLiteralFor(remainder, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(remainder);
  }

  absl::Status HandleAnd(const HloInstruction* and_inst) override {
    if constexpr (std::is_integral_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseBinaryOp(and_inst,
                              [](ElementwiseT lhs_el, ElementwiseT rhs_el) {
                                return lhs_el & rhs_el;
                              }));
      parent_->SetEvaluatedLiteralFor(and_inst, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(and_inst);
  }

  absl::Status HandleOr(const HloInstruction* or_inst) override {
    if constexpr (std::is_integral_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(Literal literal,
                          ElementWiseBinaryOp(or_inst, [](ElementwiseT lhs_el,
                                                          ElementwiseT rhs_el) {
                            return lhs_el | rhs_el;
                          }));
      parent_->SetEvaluatedLiteralFor(or_inst, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(or_inst);
  }

  absl::Status HandleXor(const HloInstruction* xor_inst) override {
    if constexpr (std::is_integral_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseBinaryOp(xor_inst,
                              [](ElementwiseT lhs_el, ElementwiseT rhs_el) {
                                return lhs_el ^ rhs_el;
                              }));
      parent_->SetEvaluatedLiteralFor(xor_inst, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(xor_inst);
  }

  absl::Status HandleShiftLeft(const HloInstruction* shl) override {
    if constexpr (std::is_integral_v<ElementwiseT> &&
                  !std::is_same_v<ElementwiseT, bool>) {
      TF_ASSIGN_OR_RETURN(Literal literal,
                          ElementWiseBinaryOp(shl, [](ElementwiseT lhs_elem,
                                                      ElementwiseT rhs_elem) {
                            return IsShiftOutOfBounds<ElementwiseT>(rhs_elem)
                                       ? 0
                                       : (lhs_elem << rhs_elem);
                          }));
      parent_->SetEvaluatedLiteralFor(shl, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(shl);
  }

  absl::Status HandleShiftRightArithmetic(const HloInstruction* shr) override {
    if constexpr (std::is_integral_v<ElementwiseT> &&
                  !std::is_same_v<ElementwiseT, bool>) {
      using SignedT = make_specialized_signed_t<ReturnT>;
      TF_ASSIGN_OR_RETURN(
          Literal literal, ElementWiseBinaryOp(shr, [](ElementwiseT lhs_elem,
                                                       ElementwiseT rhs_elem) {
            SignedT lhs_signed = static_cast<SignedT>(lhs_elem);
            if (IsShiftOutOfBounds<ReturnT>(rhs_elem)) {
              return lhs_signed < 0 ? static_cast<ElementwiseT>(-1) : 0;
            } else {
              return static_cast<ElementwiseT>(lhs_signed >> rhs_elem);
            }
          }));
      parent_->SetEvaluatedLiteralFor(shr, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(shr);
  }

  absl::Status HandleShiftRightLogical(const HloInstruction* shr) override {
    if constexpr (std::is_integral_v<ElementwiseT> &&
                  !std::is_same_v<ElementwiseT, bool>) {
      using UnsignedT = make_specialized_unsigned_t<ReturnT>;
      TF_ASSIGN_OR_RETURN(
          Literal literal, ElementWiseBinaryOp(shr, [](ElementwiseT lhs_elem,
                                                       ElementwiseT rhs_elem) {
            // If shift amount is greater than the number of
            // bits, then return 0.
            if (IsShiftOutOfBounds<ReturnT>(rhs_elem)) {
              return static_cast<ElementwiseT>(0);
            }
            return static_cast<ElementwiseT>(static_cast<UnsignedT>(lhs_elem) >>
                                             rhs_elem);
          }));
      parent_->SetEvaluatedLiteralFor(shr, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(shr);
  }

  absl::Status HandleClamp(const HloInstruction* clamp) override {
    if constexpr (!is_complex_v<ElementwiseT>) {
      auto clamp_op = [](ElementwiseT low, ElementwiseT value,
                         ElementwiseT high) {
        if constexpr (std::numeric_limits<ElementwiseT>::has_quiet_NaN) {
          if (std::isnan(low)) {
            return low;
          }
          if (std::isnan(value)) {
            return value;
          }
          if (std::isnan(high)) {
            return high;
          }
        }
        return std::min(high, std::max(value, low));
      };
      TF_ASSIGN_OR_RETURN(Literal literal,
                          (ElementwiseTernaryOp<ReturnT, ReturnT, ReturnT>(
                              clamp, ConvertTernaryFunction(clamp_op))));
      parent_->SetEvaluatedLiteralFor(clamp, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(clamp);
  }

  absl::Status HandleSelect(const HloInstruction* select) override {
    CHECK(!ShapeUtil::IsScalar(select->operand(0)->shape()));
    CHECK(select->shape().IsArray());
    auto select_op = [](bool pred, ReturnT on_true, ReturnT on_false) {
      return pred ? on_true : on_false;
    };
    TF_ASSIGN_OR_RETURN(Literal literal,
                        (ElementwiseTernaryOp<bool, ReturnT, ReturnT>(
                            select, std::move(select_op))));
    parent_->SetEvaluatedLiteralFor(select, std::move(literal));
    return absl::OkStatus();
  }

  absl::Status HandleConvolutionWithLiterals(const HloInstruction* conv,
                                             const Literal& lhs_literal,
                                             const Literal& rhs_literal) {
    const auto& window = conv->window();
    Shape result_shape = GetShapeWithLayout(conv->shape());
    const Shape& lhs_shape = lhs_literal.shape();
    const Shape& rhs_shape = rhs_literal.shape();

    TF_CHECK_OK(ShapeUtil::ValidateShape(lhs_shape));
    TF_CHECK_OK(ShapeUtil::ValidateShape(rhs_shape));
    CHECK(lhs_shape.IsArray());
    CHECK(rhs_shape.IsArray());
    CHECK(ShapeUtil::SameElementType(lhs_shape, rhs_shape));
    CHECK(ShapeUtil::SameElementType(lhs_shape, result_shape));

    const auto& dnums = conv->convolution_dimension_numbers();
    const int64_t num_spatial_dims = dnums.output_spatial_dimensions_size();
    CHECK_EQ(num_spatial_dims, dnums.input_spatial_dimensions_size());
    CHECK_EQ(num_spatial_dims, dnums.kernel_spatial_dimensions_size());
    CHECK_GE(num_spatial_dims, 0);
    CHECK_EQ(window.dimensions_size(), num_spatial_dims);

    std::vector<int64_t> window_dimension_sizes;
    for (auto i : dnums.kernel_spatial_dimensions()) {
      window_dimension_sizes.push_back(ShapeUtil::GetDimension(rhs_shape, i));
    }

    const Shape& window_shape =
        ShapeUtil::MakeShape(rhs_shape.element_type(), window_dimension_sizes);

    DimensionVector lhs_dim_multipliers =
        HloEvaluator::MakeDimMultipliers(lhs_shape);
    DimensionVector rhs_dim_multipliers =
        HloEvaluator::MakeDimMultipliers(rhs_shape);

    auto lhs_literal_data = lhs_literal.data<ReturnT>();
    auto rhs_literal_data = rhs_literal.data<ReturnT>();

    const int64_t feature_group_count = conv->feature_group_count();
    const int64_t batch_group_count = conv->batch_group_count();

    auto func = [&window_shape, &dnums, &lhs_shape, &rhs_shape, &window,
                 &lhs_dim_multipliers, &rhs_dim_multipliers, lhs_literal_data,
                 rhs_literal_data, feature_group_count, batch_group_count,
                 result_shape, this](const absl::Span<const int64_t> out_index,
                                     int /*thread_id*/) {
      // Dimension number applicable for input (lhs).
      const int64_t input_batch_dim = dnums.input_batch_dimension();
      const int64_t input_z_dim = dnums.input_feature_dimension();
      // Dimension number applicable for kernel (rhs).
      const int64_t kernel_input_z_dim = dnums.kernel_input_feature_dimension();
      const int64_t kernel_output_z_dim =
          dnums.kernel_output_feature_dimension();
      // Dimension number applicable for output.
      const int64_t output_batch_dim = dnums.output_batch_dimension();
      const int64_t output_z_dim = dnums.output_feature_dimension();

      const int64_t input_z_size =
          ShapeUtil::GetDimension(lhs_shape, input_z_dim);

      const int64_t input_batch_size =
          ShapeUtil::GetDimension(lhs_shape, input_batch_dim);

      const int64_t batch_group_size = input_batch_size / batch_group_count;

      // The size of an input feature group.
      const int64_t input_feature_group_size =
          input_z_size / feature_group_count;

      const int64_t output_z_size =
          ShapeUtil::GetDimension(rhs_shape, kernel_output_z_dim);
      // The output feature dimension is a concatenation of convolution results
      // from the different groups.
      const int64_t output_feature_group_size =
          output_z_size / feature_group_count;

      // Calculate the group index to which the current output index
      // belongs.
      const int64_t feature_group_index =
          out_index[output_z_dim] / output_feature_group_size;

      const int64_t depthwise_multiplier = output_z_size / batch_group_count;
      const int64_t batch_group_index =
          out_index[output_z_dim] / depthwise_multiplier;

      ElementwiseT result_val = static_cast<ElementwiseT>(0);
      DimensionVector rhs_spatial_index(dnums.kernel_spatial_dimensions_size(),
                                        0);

      // Convolve input feature with kernel.
      // The mechanism indexes into the correct LHS (input) and RHS (kernel)
      // locations and accumulates multiplications for a given output index.
      do {
        // Find corresponding spatial dimension index for input (lhs).
        int64_t lhs_linear_spatial_index = 0;
        int64_t rhs_linear_spatial_index = 0;
        for (int64_t ki = 0; ki < rhs_spatial_index.size(); ++ki) {
          // Spatial dimension number for input (lhs) and output.
          const int64_t input_spatial_dim = dnums.input_spatial_dimensions(ki);
          const int64_t output_spatial_dim =
              dnums.output_spatial_dimensions(ki);

          // Calculate lhs (input) index without taking base dilation into
          // account.
          const auto& window_dim = window.dimensions(ki);
          const int64_t undilated_index =
              out_index[output_spatial_dim] * window_dim.stride() -
              window_dim.padding_low() +
              rhs_spatial_index[ki] * window_dim.window_dilation();
          // Skip if the lhs (input) index is to be dilated.  As an
          // optimization, skip this mod if there's no dilation.
          if (window_dim.base_dilation() > 1 &&
              undilated_index % window_dim.base_dilation() != 0) {
            goto cnt;
          }

          // Calculate the actual lhs (input) index after dilation.  As an
          // optimization, skip this integer divide if there's no dilation.
          int64_t lhs_spatial_index;
          if (window_dim.base_dilation() > 1) {
            lhs_spatial_index = undilated_index / window_dim.base_dilation();
          } else {
            lhs_spatial_index = undilated_index;
          }

          // Skip if input index is not in bounds.
          if (!(lhs_spatial_index >= 0 &&
                lhs_spatial_index < lhs_shape.dimensions(input_spatial_dim))) {
            goto cnt;
          }

          lhs_linear_spatial_index +=
              lhs_spatial_index * lhs_dim_multipliers[input_spatial_dim];
          rhs_linear_spatial_index +=
              (window_dim.window_reversal()
                   ? ((window_dim.size() - 1) - rhs_spatial_index[ki])
                   : rhs_spatial_index[ki]) *
              rhs_dim_multipliers[dnums.kernel_spatial_dimensions(ki)];
        }

        for (int64_t rhs_iz = 0; rhs_iz < input_feature_group_size; ++rhs_iz) {
          const int64_t iz =
              feature_group_index * input_feature_group_size + rhs_iz;

          int64_t lhs_linear_index = lhs_linear_spatial_index;
          lhs_linear_index += out_index[output_batch_dim] *
                              lhs_dim_multipliers[input_batch_dim];

          // We are scraping only the diagonal elements in the resultant
          // convolution output when batch_group_count is greater than 1,
          // where 1 is the default. No scraping is done in that case.
          // This approach works out automatically for 'groups' in batches
          // with group_size > 1, because we already descend down the batch
          // dimension for the 'output_batch_dim' above.
          lhs_linear_index += (batch_group_index * batch_group_size) *
                              lhs_dim_multipliers[input_batch_dim];

          lhs_linear_index += iz * lhs_dim_multipliers[input_z_dim];
          int64_t rhs_linear_index = rhs_linear_spatial_index;

          rhs_linear_index += out_index[output_z_dim] *
                              rhs_dim_multipliers[kernel_output_z_dim];
          rhs_linear_index += rhs_iz * rhs_dim_multipliers[kernel_input_z_dim];
          auto lhs =
              static_cast<ElementwiseT>(lhs_literal_data[lhs_linear_index]);
          auto rhs =
              static_cast<ElementwiseT>(rhs_literal_data[rhs_linear_index]);
          result_val += ToArithmeticSafeType(lhs) * ToArithmeticSafeType(rhs);

          if (parent_->trace_mac_handler_ != nullptr) {
            const int64_t result_linear_index =
                IndexUtil::MultidimensionalIndexToLinearIndex(result_shape,
                                                              out_index);

            parent_->trace_mac_handler_(result_linear_index, lhs_linear_index,
                                        rhs_linear_index);
          }
        }
      cnt: {}
      } while (IndexUtil::BumpIndices(window_shape,
                                      absl::MakeSpan(rhs_spatial_index)));

      if constexpr (std::is_integral_v<ReturnT>) {
        auto l = static_cast<ElementwiseT>(std::numeric_limits<ReturnT>::min());
        auto h = static_cast<ElementwiseT>(std::numeric_limits<ReturnT>::max());
        result_val = std::max(l, std::min(h, result_val));
      }
      return static_cast<ReturnT>(result_val);
    };

    Literal result(result_shape);
    TF_RETURN_IF_ERROR(result.PopulateParallel<ReturnT>(func));

    parent_->SetEvaluatedLiteralFor(conv, std::move(result));
    return absl::OkStatus();
  }

  absl::Status HandleConvolution(const HloInstruction* conv) override {
    auto lhs = conv->operand(0);
    auto rhs = conv->operand(1);
    const auto& window = conv->window();
    Shape result_shape = GetShapeWithLayout(conv->shape());
    Shape lhs_shape = GetShapeWithLayout(lhs->shape());
    Shape rhs_shape = GetShapeWithLayout(rhs->shape());

    TF_CHECK_OK(ShapeUtil::ValidateShape(lhs_shape));
    TF_CHECK_OK(ShapeUtil::ValidateShape(rhs_shape));
    CHECK(lhs_shape.IsArray());
    CHECK(rhs_shape.IsArray());

    const auto& dnums = conv->convolution_dimension_numbers();
    const int64_t num_spatial_dims = dnums.output_spatial_dimensions_size();
    CHECK_EQ(num_spatial_dims, dnums.input_spatial_dimensions_size());
    CHECK_EQ(num_spatial_dims, dnums.kernel_spatial_dimensions_size());
    CHECK_GE(num_spatial_dims, 0);
    CHECK_EQ(window.dimensions_size(), num_spatial_dims);

    const auto lhs_rank = lhs_shape.dimensions().size();
    const auto rhs_rank = rhs_shape.dimensions().size();

    CHECK_EQ(num_spatial_dims + 2, lhs_rank);
    CHECK_EQ(num_spatial_dims + 2, rhs_rank);

    TF_ASSIGN_OR_RETURN(
        auto inferred_return_shape,
        ShapeInference::InferConvolveShape(
            lhs_shape, rhs_shape, conv->feature_group_count(),
            conv->batch_group_count(), window, dnums,
            /*preferred_element_type=*/conv->shape().element_type()));
    CHECK(ShapeUtil::Compatible(result_shape, inferred_return_shape))
        << "return shape set to: " << ShapeUtil::HumanString(result_shape)
        << " but is inferred to be: "
        << ShapeUtil::HumanString(inferred_return_shape);

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);
    const bool lhs_same = ShapeUtil::SameElementType(lhs_shape, result_shape);
    const bool rhs_same = ShapeUtil::SameElementType(rhs_shape, result_shape);
    if (rhs_same && lhs_same) {
      return HandleConvolutionWithLiterals(conv, lhs_literal, rhs_literal);
    }
    if (rhs_same) {
      return HandleConvolutionWithLiterals(
          conv, lhs_literal.Convert(result_shape.element_type()).value(),
          rhs_literal);
    }
    if (lhs_same) {
      return HandleConvolutionWithLiterals(
          conv, lhs_literal,
          rhs_literal.Convert(result_shape.element_type()).value());
    }
    return HandleConvolutionWithLiterals(
        conv, lhs_literal.Convert(result_shape.element_type()).value(),
        rhs_literal.Convert(result_shape.element_type()).value());
  }

  absl::Status HandleDot(const HloInstruction* dot) override {
    if (dot->dot_dimension_numbers().rhs_contracting_dimensions_size() == 1 &&
        parent_->use_fast_path_ &&
        ShapeUtil::SameElementType(dot->operand(0)->shape(), dot->shape()) &&
        ShapeUtil::SameElementType(dot->operand(1)->shape(), dot->shape())) {
      return HandleDot<ElementwiseT>(dot);
    }
    return HandleDotSlowPath(dot);
  }

  template <typename NativeT, typename std::enable_if_t<
                                  std::is_same_v<NativeT, float>>* = nullptr>
  absl::Status HandleDot(const HloInstruction* dot) {
    const HloInstruction* lhs = dot->operand(0);
    const HloInstruction* rhs = dot->operand(1);
    CHECK(dot->shape().IsArray());
    CHECK(lhs->shape().IsArray());
    CHECK(rhs->shape().IsArray());

    const auto& dnums = dot->dot_dimension_numbers();

    const int64_t lhs_rank = lhs->shape().dimensions().size();
    const int64_t rhs_rank = rhs->shape().dimensions().size();

    CHECK(ShapeUtil::SameElementType(lhs->shape(), rhs->shape()));
    CHECK(ShapeUtil::SameElementType(lhs->shape(), dot->shape()));

    // There must be 1 and only 1 Contracting dimension for lhs and rhs.
    const int64_t lhs_contracting_dimension =
        dnums.lhs_contracting_dimensions(0);
    const int64_t rhs_contracting_dimension =
        dnums.rhs_contracting_dimensions(0);
    // Contracted dimension sizes must be the same.
    CHECK_EQ(lhs->shape().dimensions(lhs_contracting_dimension),
             rhs->shape().dimensions(rhs_contracting_dimension))
        << "lhs contracted dimension: "
        << lhs->shape().dimensions(lhs_contracting_dimension)
        << " rhs contracted dimension: "
        << rhs->shape().dimensions(rhs_contracting_dimension);

    auto is_default_layout = [](const HloInstruction* op) {
      return !op->shape().has_layout() ||
             LayoutUtil::Equal(op->shape().layout(),
                               LayoutUtil::GetDefaultLayoutForR2());
    };

    // The fast path is for a simple rank 2 dot with default layout operands.
    if (lhs_rank != 2 || rhs_rank != 2 || lhs_contracting_dimension != 1 ||
        rhs_contracting_dimension != 0 || !is_default_layout(lhs) ||
        !is_default_layout(rhs) || !is_default_layout(dot)) {
      return HandleDotSlowPath(dot);
    }

    const PrimitiveType native_ty =
        primitive_util::NativeToPrimitiveType<NativeT>();
    Literal lhs_literal =
        parent_->GetEvaluatedLiteralFor(lhs).Convert(native_ty).value();
    Literal rhs_literal =
        parent_->GetEvaluatedLiteralFor(rhs).Convert(native_ty).value();
    const int64_t contracted_dimension_size =
        lhs->shape().dimensions(lhs_contracting_dimension);
    Array2D<NativeT> lhs_array(lhs->shape().dimensions(0),
                               contracted_dimension_size);
    lhs_array.SetValues(lhs_literal.data<NativeT>());
    Array2D<NativeT> rhs_array(contracted_dimension_size,
                               rhs->shape().dimensions(1));
    rhs_array.SetValues(rhs_literal.data<NativeT>());
    std::unique_ptr<Array2D<NativeT>> result_array =
        HloEvaluator::MatmulArray2D(lhs_array, rhs_array);
    Literal result(ShapeUtil::MakeShape(native_ty, dot->shape().dimensions()));
    result.PopulateR2FromArray2D(*result_array);
    parent_->SetEvaluatedLiteralFor(
        dot, std::move(result).Convert(dot->shape().element_type()).value());
    return absl::OkStatus();
  }

  template <typename NativeT, typename std::enable_if_t<
                                  !std::is_same_v<NativeT, float>>* = nullptr>
  absl::Status HandleDot(const HloInstruction* dot) {
    return HandleDotSlowPath(dot);
  }

  absl::Status HandleDotSlowPathWithLiterals(const HloInstruction* dot,
                                             const Literal& lhs_literal,
                                             const Literal& rhs_literal) {
    const auto& dnums = dot->dot_dimension_numbers();

    const auto lhs_rank = lhs_literal.shape().dimensions().size();
    const auto rhs_rank = rhs_literal.shape().dimensions().size();

    CHECK(ShapeUtil::SameElementType(lhs_literal.shape(), rhs_literal.shape()));
    CHECK(ShapeUtil::SameElementType(lhs_literal.shape(), dot->shape()));

    CHECK_EQ(dnums.lhs_batch_dimensions_size(),
             dnums.rhs_batch_dimensions_size());

    DimensionVector lhs_non_contracting_dims =
        GetNonContractingDims(lhs_rank, dnums.lhs_contracting_dimensions(),
                              dnums.lhs_batch_dimensions());
    DimensionVector rhs_non_contracting_dims =
        GetNonContractingDims(rhs_rank, dnums.rhs_contracting_dimensions(),
                              dnums.rhs_batch_dimensions());

    DimensionVector contracting_dim_sizes;
    contracting_dim_sizes.reserve(dnums.lhs_contracting_dimensions_size());
    DimensionVector lhs_contracting_dims;
    DimensionVector rhs_contracting_dims;
    for (int64_t i = 0; i < dnums.lhs_contracting_dimensions_size(); ++i) {
      const int64_t lhs_dnum = dnums.lhs_contracting_dimensions(i);
      const int64_t rhs_dnum = dnums.rhs_contracting_dimensions(i);
      lhs_contracting_dims.push_back(lhs_dnum);
      rhs_contracting_dims.push_back(rhs_dnum);
      const int64_t dim_size = lhs_literal.shape().dimensions(lhs_dnum);
      contracting_dim_sizes.push_back(dim_size);
    }
    const int64_t total_contraction_size = Product(contracting_dim_sizes);
    Shape dot_shape = GetShapeWithLayout(dot->shape());
    Literal result(dot_shape);
    TF_RETURN_IF_ERROR(result.PopulateParallel<ReturnT>(
        [&](absl::Span<const int64_t> result_index, int /*thread_id*/) {
          // Locations in LHS and RHS that we read from.
          DimensionVector lhs_index(lhs_rank);
          DimensionVector rhs_index(rhs_rank);

          // First come the batch dimensions.
          int64_t idx = 0;
          for (int64_t i = 0; i < dnums.lhs_batch_dimensions_size(); i++) {
            lhs_index[dnums.lhs_batch_dimensions(i)] = result_index[idx];
            rhs_index[dnums.rhs_batch_dimensions(i)] = result_index[idx];
            idx++;
          }

          // Next we have non-contracting dimensions, if any.
          for (int64_t i = 0; i < lhs_non_contracting_dims.size(); i++) {
            lhs_index[lhs_non_contracting_dims[i]] = result_index[idx++];
          }
          for (int64_t i = 0; i < rhs_non_contracting_dims.size(); i++) {
            rhs_index[rhs_non_contracting_dims[i]] = result_index[idx++];
          }

          // Accumulate resulting product along the contracting dimensions.
          ElementwiseT result_val = static_cast<ElementwiseT>(0);
          for (int64_t k = 0; k < total_contraction_size; k++) {
            const auto lhs =
                static_cast<ElementwiseT>(lhs_literal.Get<ReturnT>(lhs_index));
            const auto rhs =
                static_cast<ElementwiseT>(rhs_literal.Get<ReturnT>(rhs_index));
            result_val += ToArithmeticSafeType(lhs) * ToArithmeticSafeType(rhs);

            if (parent_->trace_mac_handler_ != nullptr) {
              const int64_t result_linear_index =
                  IndexUtil::MultidimensionalIndexToLinearIndex(dot_shape,
                                                                result_index);
              const int64_t lhs_linear_index =
                  IndexUtil::MultidimensionalIndexToLinearIndex(
                      lhs_literal.shape(), lhs_index);
              const int64_t rhs_linear_index =
                  IndexUtil::MultidimensionalIndexToLinearIndex(
                      rhs_literal.shape(), rhs_index);

              parent_->trace_mac_handler_(result_linear_index, lhs_linear_index,
                                          rhs_linear_index);
            }

            // If there are no contracting dimensions, do not try to count down
            // from -1 to 0; that's an infinite loop.
            if (!contracting_dim_sizes.empty()) {
              for (int64_t i = contracting_dim_sizes.size() - 1; i >= 0; --i) {
                lhs_index[lhs_contracting_dims[i]]++;
                rhs_index[rhs_contracting_dims[i]]++;
                if (lhs_index[lhs_contracting_dims[i]] !=
                    contracting_dim_sizes[i]) {
                  break;
                }
                lhs_index[lhs_contracting_dims[i]] = 0;
                rhs_index[rhs_contracting_dims[i]] = 0;
              }
            }
          }

          return static_cast<ReturnT>(result_val);
        }));

    parent_->SetEvaluatedLiteralFor(dot, std::move(result));
    return absl::OkStatus();
  }

  absl::Status HandleDotSlowPath(const HloInstruction* dot) {
    auto lhs = dot->operand(0);
    auto rhs = dot->operand(1);
    CHECK(dot->shape().IsArray());
    CHECK(lhs->shape().IsArray());
    CHECK(rhs->shape().IsArray());
    const bool lhs_same =
        ShapeUtil::SameElementType(lhs->shape(), dot->shape());
    const bool rhs_same =
        ShapeUtil::SameElementType(rhs->shape(), dot->shape());
    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);
    if (lhs_same && rhs_same) {
      return HandleDotSlowPathWithLiterals(dot, lhs_literal, rhs_literal);
    }
    if (lhs_same) {
      return HandleDotSlowPathWithLiterals(
          dot, lhs_literal,
          rhs_literal.Convert(dot->shape().element_type()).value());
    }
    if (rhs_same) {
      return HandleDotSlowPathWithLiterals(
          dot, lhs_literal.Convert(dot->shape().element_type()).value(),
          rhs_literal);
    }
    return HandleDotSlowPathWithLiterals(
        dot, lhs_literal.Convert(dot->shape().element_type()).value(),
        rhs_literal.Convert(dot->shape().element_type()).value());
  }

  absl::Status HandleRaggedDotWithLiterals(const HloInstruction* dot,
                                           const Literal& lhs_literal,
                                           const Literal& rhs_literal,
                                           const Literal& gs_literal) {
    auto ragged_dims = dot->ragged_dot_dimension_numbers();
    auto dot_dims = ragged_dims.dot_dimension_numbers();

    if (ragged_dims.rhs_group_dimensions_size() != 1) {
      return absl::UnimplementedError("Only one group dimension is supported.");
    }
    if (ragged_dims.lhs_ragged_dimensions_size() != 1) {
      return absl::UnimplementedError(
          "Only one ragged dimension is supported.");
    }
    int64_t rhs_group_dim = ragged_dims.rhs_group_dimensions(0);
    int64_t lhs_ragged_dim = ragged_dims.lhs_ragged_dimensions(0);

    int64_t lhs_rank = lhs_literal.shape().dimensions().size();
    int64_t rhs_rank = rhs_literal.shape().dimensions().size();
    int64_t gs_rank = gs_literal.shape().dimensions().size();
    int64_t num_groups = gs_literal.shape().dimensions(gs_rank - 1);

    auto lhs_contracting = dot_dims.lhs_contracting_dimensions();
    auto lhs_non_contracting = GetNonContractingDims(
        lhs_rank, lhs_contracting, dot_dims.lhs_batch_dimensions());
    if (std::find(lhs_non_contracting.begin(), lhs_non_contracting.end(),
                  lhs_ragged_dim) == lhs_non_contracting.end()) {
      return absl::UnimplementedError(
          "Ragged dimension must be a non-contracting dimension.");
    }

    auto rhs_contracting = dot_dims.rhs_contracting_dimensions();
    // Group Dimension is also a contracting dimension.
    rhs_contracting.Add(rhs_group_dim);
    auto rhs_non_contracting = GetNonContractingDims(
        rhs_rank, rhs_contracting, dot_dims.rhs_batch_dimensions());

    DimensionVector contracting_dim_sizes;
    contracting_dim_sizes.reserve(lhs_contracting.size());
    for (int64_t i = 0; i < lhs_contracting.size(); ++i) {
      int64_t dim_size = lhs_literal.shape().dimensions(lhs_contracting[i]);
      contracting_dim_sizes.push_back(dim_size);
    }
    int64_t total_contracting_size = Product(contracting_dim_sizes);

    Shape dot_shape = GetShapeWithLayout(dot->shape());
    Literal result(dot_shape);
    TF_RETURN_IF_ERROR(result.PopulateParallel<ReturnT>(
        [&](absl::Span<const int64_t> result_index, int /*thread_id*/) {
          // Locations in each operand that we read from to calculate the result
          // at result_index.
          DimensionVector lhs_index(lhs_rank);
          DimensionVector rhs_index(rhs_rank);
          DimensionVector group_index(gs_rank);

          // Batch dimensions will always be first in the final product.
          int64_t idx = 0;
          int64_t gs_idx = 0;
          for (int64_t i = 0; i < dot_dims.lhs_batch_dimensions_size(); ++i) {
            lhs_index[dot_dims.lhs_batch_dimensions(i)] = result_index[idx];
            rhs_index[dot_dims.rhs_batch_dimensions(i)] = result_index[idx];
            group_index[gs_idx++] = result_index[idx];
            idx++;
          }

          // Non-contracting dimensions - lhs, then rhs.
          for (int64_t i = 0; i < lhs_non_contracting.size(); ++i) {
            // If there is a non-contracting lhs dimension that is not ragged,
            // then there will also be a dimension for this in group_sizes.
            if (lhs_ragged_dim != lhs_non_contracting[i]) {
              group_index[gs_idx++] = result_index[idx];
            }
            lhs_index[lhs_non_contracting[i]] = result_index[idx++];
          }
          for (int64_t i = 0; i < rhs_non_contracting.size(); ++i) {
            rhs_index[rhs_non_contracting[i]] = result_index[idx++];
          }

          // Calculate which group the current lhs-row belongs to.
          int64_t lhs_ragged_index = lhs_index[lhs_ragged_dim];
          int64_t group_row_end = 0;
          for (int64_t i = 0; i < num_groups; ++i) {
            group_index[gs_idx] = i;
            group_row_end += gs_literal.Get<int64_t>(group_index);
            if (lhs_ragged_index < group_row_end) {
              break;
            }
          }
          // If lhs_ragged_index > the sum(groups), then there is no
          // corresponding rhs value, and there is no result.
          if (lhs_ragged_index >= group_row_end) {
            return static_cast<ReturnT>(0);
          }
          rhs_index[rhs_group_dim] = group_index[gs_idx];

          // Accumulate resulting product along the contracting dimensions.
          ElementwiseT result_val = static_cast<ElementwiseT>(0);
          for (int64_t i = 0; i < total_contracting_size; ++i) {
            const auto lhs =
                static_cast<ElementwiseT>(lhs_literal.Get<ReturnT>(lhs_index));
            const auto rhs =
                static_cast<ElementwiseT>(rhs_literal.Get<ReturnT>(rhs_index));
            result_val += ToArithmeticSafeType(lhs) * ToArithmeticSafeType(rhs);

            for (int64_t j = contracting_dim_sizes.size() - 1; j >= 0; --j) {
              lhs_index[lhs_contracting[j]]++;
              rhs_index[rhs_contracting[j]]++;
              if (lhs_index[lhs_contracting[j]] != contracting_dim_sizes[j]) {
                break;
              }
              lhs_index[lhs_contracting[j]] = 0;
              rhs_index[rhs_contracting[j]] = 0;
            }
          }
          return static_cast<ReturnT>(result_val);
        }));

    parent_->SetEvaluatedLiteralFor(dot, std::move(result));
    return absl::OkStatus();
  }

  // This is currently only implemented for the ragged dimension being a non-
  // contracting dimension. For other modes, this will throw an unimplemented
  // error.
  absl::Status HandleRaggedDot(const HloInstruction* dot) override {
    auto lhs = dot->operand(0);
    auto rhs = dot->operand(1);
    auto group_sizes = dot->operand(2);

    CHECK(dot->shape().IsArray());
    CHECK(lhs->shape().IsArray());
    CHECK(rhs->shape().IsArray());
    CHECK(group_sizes->shape().IsArray());

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);
    const Literal& gs_literal = parent_->GetEvaluatedLiteralFor(group_sizes);

    // LHS and RHS may have a different initial precision than our output.
    const bool lhs_same =
        ShapeUtil::SameElementType(lhs->shape(), dot->shape());
    const bool rhs_same =
        ShapeUtil::SameElementType(rhs->shape(), dot->shape());
    // Upcast group sizes to S64, since they could be e.g. S32.
    const bool gs_same =
        group_sizes->shape().element_type() == PrimitiveType::S64;

    return HandleRaggedDotWithLiterals(
        dot,
        lhs_same
            ? lhs_literal
            : static_cast<const Literal&>(
                  lhs_literal.Convert(dot->shape().element_type()).value()),
        rhs_same
            ? rhs_literal
            : static_cast<const Literal&>(
                  rhs_literal.Convert(dot->shape().element_type()).value()),
        gs_same ? gs_literal
                : static_cast<const Literal&>(
                      gs_literal.Convert(PrimitiveType::S64).value()));
  }

  absl::Status HandlePad(const HloInstruction* pad) override {
    CHECK(pad->operand(0)->shape().IsArray());
    // Padding value must be scalar.
    CHECK(ShapeUtil::IsScalar(pad->operand(1)->shape()));
    CHECK_EQ(pad->operand(0)->shape().dimensions().size(),
             pad->padding_config().dimensions_size());

    TF_ASSIGN_OR_RETURN(auto inferred_return_shape,
                        ShapeInference::InferPadShape(
                            /*operand_shape=*/pad->operand(0)->shape(),
                            /*padding_value_shape=*/pad->operand(1)->shape(),
                            /*padding_config=*/pad->padding_config()));
    // Try to convert the element type if the inferred type is not compatible.
    bool convert_element_type =
        pad->shape().element_type() != inferred_return_shape.element_type();
    if (convert_element_type) {
      inferred_return_shape.set_element_type(pad->shape().element_type());
    }
    CHECK(ShapeUtil::Compatible(pad->shape(), inferred_return_shape))
        << "return shape is set to: " << ShapeUtil::HumanString(pad->shape())
        << " but is inferred to be: "
        << ShapeUtil::HumanString(inferred_return_shape);
    ReturnT scalar;
    if (convert_element_type) {
      TF_ASSIGN_OR_RETURN(auto literal,
                          parent_->GetEvaluatedLiteralFor(pad->operand(1))
                              .Convert(inferred_return_shape.element_type()));
      scalar = literal.Get<ReturnT>({});
    } else {
      scalar =
          parent_->GetEvaluatedLiteralFor(pad->operand(1)).Get<ReturnT>({});
    }

    // Create new HLO of padded shape with padding value.
    Literal result(GetShapeWithLayout(pad->shape()));
    TF_RETURN_IF_ERROR(result.PopulateLinearParallel<ReturnT>(
        [&scalar](int64_t linear_index, int) { return scalar; }));

    const Literal& evaluated_operand =
        parent_->GetEvaluatedLiteralFor(pad->operand(0));

    std::vector<int64_t> target_index(result.shape().dimensions().size(), 0);

    // Loop through each element of the operand, assign them to the
    // corresponding index of the resulting padded literal.
    const PaddingConfig& pad_config = pad->padding_config();

    auto func = [&](absl::Span<const int64_t> input_index) {
      for (auto i = 0; i < input_index.size(); ++i) {
        // Interior padding occurs logically before edge padding, so in the case
        // of negative edge padding elements are removed from the
        // interior-padded operand.
        target_index[i] =
            pad_config.dimensions(i).edge_padding_low() +
            input_index[i] * (pad_config.dimensions(i).interior_padding() + 1);

        // Account for negative low and high padding: skip assignment if the
        // any target index is out of range.
        if (!(target_index[i] >= 0 &&
              target_index[i] < pad->shape().dimensions(i))) {
          return true;
        }
      }
      result.Set<ReturnT>(target_index,
                          evaluated_operand.Get<ReturnT>(input_index));
      return true;
    };

    std::vector<int64_t> zero_base(
        evaluated_operand.shape().dimensions().size(), 0);
    std::vector<int64_t> step(evaluated_operand.shape().dimensions().size(), 1);

    ShapeUtil::ForEachIndexNoStatus(evaluated_operand.shape(), zero_base,
                                    evaluated_operand.shape().dimensions(),
                                    step, func);

    parent_->SetEvaluatedLiteralFor(pad, std::move(result));
    return absl::OkStatus();
  }

  absl::Status HandleClz(const HloInstruction* clz) override {
    // Enable CLZ only for integer types.
    if constexpr (std::is_integral_v<ElementwiseT> &&
                  !std::is_same_v<ElementwiseT, bool>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(clz, [](ElementwiseT elem_operand) {
            int64_t unsigned_digits = std::numeric_limits<ReturnT>::digits +
                                      std::numeric_limits<ReturnT>::is_signed;
            return (unsigned_digits - 1) - Log2Floor<uint64_t>(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(clz, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(clz);
  }

  absl::Status HandlePopulationCount(const HloInstruction* popcnt) override {
    if constexpr (std::is_integral_v<ElementwiseT> &&
                  !std::is_same_v<ElementwiseT, bool>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(popcnt, [](ElementwiseT elem_operand) {
            return std::bitset<CHAR_BIT * sizeof(ReturnT)>(elem_operand)
                .count();
          }));
      parent_->SetEvaluatedLiteralFor(popcnt, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(popcnt);
  }

  absl::Status HandleSin(const HloInstruction* sin) override {
    if constexpr (std::is_floating_point_v<ElementwiseT> ||
                  is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(sin, [](ElementwiseT elem_operand) {
            return std::sin(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(sin, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(sin);
  }

  absl::Status HandleCos(const HloInstruction* cos) override {
    if constexpr (std::is_floating_point_v<ElementwiseT> ||
                  is_complex_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(cos, [](ElementwiseT elem_operand) {
            return std::cos(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(cos, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(cos);
  }

  absl::Status HandleTan(const HloInstruction* tan) override {
    if constexpr (std::is_floating_point_v<ElementwiseT>) {
      TF_ASSIGN_OR_RETURN(
          Literal literal,
          ElementWiseUnaryOp(tan, [](ElementwiseT elem_operand) {
            return std::tan(elem_operand);
          }));
      parent_->SetEvaluatedLiteralFor(tan, std::move(literal));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(tan);
  }

  template <typename NativeT, typename std::enable_if_t<
                                  std::is_floating_point_v<NativeT>>* = nullptr>
  absl::Status HandleReducePrecision(const HloInstruction* reduce_precision) {
    TF_ASSIGN_OR_RETURN(
        Literal literal,
        ElementWiseUnaryOp(reduce_precision, [&](ElementwiseT elem) {
          const uint32_t src_mantissa_bits =
              std::numeric_limits<NativeT>::digits - 1;
          const uint32_t src_exponent_bits =
              8 * sizeof(NativeT) - src_mantissa_bits - 1;
          const uint32_t dest_mantissa_bits = reduce_precision->mantissa_bits();
          const uint32_t dest_exponent_bits = reduce_precision->exponent_bits();

          using Uint = UnsignedIntegerTypeForSizeType<sizeof(NativeT)>;
          Uint value_as_int = absl::bit_cast<Uint>(elem);

          // Code is based on the CPU/GPU implementation in LLVM-emitting code.
          //
          // Bits in float32 type:
          //   mantissa : bits [0:22]
          //   exponent : bits [23:30]
          //   sign     : bits [31]
          if (dest_mantissa_bits < src_mantissa_bits) {
            const Uint last_mantissa_bit_mask =
                Uint{1} << (src_mantissa_bits - dest_mantissa_bits);

            // Compute rounding bias for round-to-nearest with ties to even.
            // This is equal to a base value of 0111... plus one bit if the last
            // remaining mantissa bit is 1.
            const Uint base_rounding_bias = (last_mantissa_bit_mask >> 1) - 1;
            const Uint x_last_mantissa_bit =
                (value_as_int & last_mantissa_bit_mask) >>
                (src_mantissa_bits - dest_mantissa_bits);
            const Uint x_rounding_bias =
                x_last_mantissa_bit + base_rounding_bias;

            // Add rounding bias, and mask out truncated bits.  Note that the
            // case where adding the rounding bias overflows into the exponent
            // bits is correct; the non-masked mantissa bits will all be zero,
            // and the exponent will be incremented by one.
            const Uint truncation_mask = ~(last_mantissa_bit_mask - 1);
            value_as_int = value_as_int + x_rounding_bias;
            value_as_int = value_as_int & truncation_mask;
          }
          if (dest_exponent_bits < src_exponent_bits) {
            // Masks for f32 values.
            const Uint sign_bit_mask = Uint{1} << 8 * sizeof(NativeT) - 1;
            const Uint exp_bits_mask = (Uint{1 << src_exponent_bits} - 1)
                                       << src_mantissa_bits;

            // An exponent of 2^(n-1)-1 -- that is, 0111... with the zero in the
            // most- significant bit -- is equal to 1.0f for all exponent sizes.
            // Adding 2^(n-1)-1 to this gives us the highest non-infinite
            // exponent for a bit- size of n, and subtracting 2^(n-1)-1 from
            // this gives us the lowest' exponent (corresponding to 0.0f).
            //
            // Thus, the f32 exponent corresponding to the highest non-infinite
            // exponent for a bit size of n is (2^7-1) + 2^(n-1)-1, and the f32
            // exponent corresponding to the lowest exponent for a bit size of n
            // is (2^7-1) - 2^(n-1)-1.
            //
            // Note that we have already checked that exponents_bits >= 1.
            const Uint exponent_bias = (Uint{1} << (src_exponent_bits - 1)) - 1;
            const Uint reduced_exponent_bias =
                (1 << (dest_exponent_bits - 1)) - 1;
            const Uint reduced_max_exponent =
                exponent_bias + reduced_exponent_bias;
            const Uint reduced_min_exponent =
                exponent_bias - reduced_exponent_bias;

            // Do we overflow or underflow?
            const Uint x_exponent = value_as_int & exp_bits_mask;
            const bool x_overflows =
                x_exponent > (reduced_max_exponent << src_mantissa_bits);
            const bool x_underflows =
                x_exponent <= (reduced_min_exponent << src_mantissa_bits);

            // Compute appropriately-signed values of zero and infinity.
            const Uint x_signed_zero = value_as_int & sign_bit_mask;
            const Uint x_signed_inf = x_signed_zero | exp_bits_mask;

            // Force to zero or infinity if overflow or underflow.  (Note that
            // this truncates all denormal values to zero, rather than rounding
            // them.)
            value_as_int = x_overflows ? x_signed_inf : value_as_int;
            value_as_int = x_underflows ? x_signed_zero : value_as_int;
          }

          NativeT reduced_result = absl::bit_cast<NativeT>(value_as_int);
          if (std::isnan(elem)) {
            reduced_result = dest_mantissa_bits > 0
                                 ? elem
                                 : std::numeric_limits<NativeT>::infinity();
          }
          return reduced_result;
        }));
    parent_->SetEvaluatedLiteralFor(reduce_precision, std::move(literal));
    return absl::OkStatus();
  }

  template <typename NativeT,
            typename std::enable_if_t<std::is_integral_v<NativeT> ||
                                      is_complex_v<NativeT>>* = nullptr>
  absl::Status HandleReducePrecision(const HloInstruction* reduce_precision) {
    return UnsupportedTypeError(reduce_precision);
  }

  absl::Status HandleReducePrecision(
      const HloInstruction* reduce_precision) override {
    return HandleReducePrecision<ElementwiseT>(reduce_precision);
  }

  absl::Status HandleIota(const HloInstruction* instruction) override {
    auto* iota = Cast<HloIotaInstruction>(instruction);
    if constexpr (std::is_integral_v<ElementwiseT> ||
                  is_complex_v<ElementwiseT> ||
                  std::is_floating_point_v<ElementwiseT>) {
      auto iota_shape = GetShapeWithLayout(iota->shape());
      Literal result(iota_shape);
      ShapeUtil::ForEachIndexNoStatus(
          iota_shape, [&](absl::Span<const int64_t> idx) {
            result.Set(idx, static_cast<ReturnT>(idx[iota->iota_dimension()]));
            return true;
          });
      parent_->SetEvaluatedLiteralFor(iota, std::move(result));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(iota);
  }

  absl::Status HandleRng(const HloInstruction* random) override {
    RandomDistribution distribution = random->random_distribution();
    Shape result_shape = GetShapeWithLayout(random->shape());
    Literal result(result_shape);

    if constexpr (std::is_floating_point_v<ElementwiseT>) {
      switch (distribution) {
        case RNG_UNIFORM: {
          const Literal& low =
              parent_->GetEvaluatedLiteralFor(random->operand(0));
          const Literal& high =
              parent_->GetEvaluatedLiteralFor(random->operand(1));

          // std::uniform_real_distribution(a, b) can sometimes return a value
          // equal to b.  Unclear if this is a spec bug or an implementation bug
          // or WAI [0] [1] [2].  Anyway for our purposes we want a half-open
          // interval, so we have to re-sample if we get `b` out.
          //
          // [0] https://gcc.gnu.org/bugzilla/show_bug.cgi?id=63176
          // [1] https://bugs.llvm.org/show_bug.cgi?id=18767
          // [2] http://open-std.org/JTC1/SC22/WG21/docs/lwg-active.html#2524
          const ReturnT low_val = low.Get<ReturnT>({});
          const ReturnT high_val = high.Get<ReturnT>({});
          std::uniform_real_distribution<ElementwiseT> generator(
              static_cast<ElementwiseT>(low_val),
              static_cast<ElementwiseT>(high_val));
          TF_RETURN_IF_ERROR(result.Populate<ReturnT>(
              [&](absl::Span<const int64_t> /*indexes*/) {
                while (true) {
                  const ReturnT v =
                      static_cast<ReturnT>(generator(parent_->engine_));
                  if (v >= low_val && v < high_val) {
                    return v;
                  }
                }
              }));
          break;
        }
        case RNG_NORMAL: {
          const Literal& mean =
              parent_->GetEvaluatedLiteralFor(random->operand(0));
          const Literal& stddev =
              parent_->GetEvaluatedLiteralFor(random->operand(1));

          std::normal_distribution<ElementwiseT> generator(
              static_cast<ElementwiseT>(mean.Get<ReturnT>({})),
              static_cast<ElementwiseT>(stddev.Get<ReturnT>({})));

          TF_RETURN_IF_ERROR(result.Populate<ReturnT>(
              [&](absl::Span<const int64_t> /*indexes*/) {
                return static_cast<ReturnT>(generator(parent_->engine_));
              }));
          break;
        }
        default:
          return UnimplementedStrCat("The distribution ",
                                     RandomDistribution_Name(distribution),
                                     " is not implemented.");
      }
      parent_->SetEvaluatedLiteralFor(random, std::move(result));
      return absl::OkStatus();
    }
    if constexpr (std::is_integral_v<ElementwiseT>) {
      switch (distribution) {
        case RNG_UNIFORM: {
          const Literal& low =
              parent_->GetEvaluatedLiteralFor(random->operand(0));
          const Literal& high =
              parent_->GetEvaluatedLiteralFor(random->operand(1));

          // Note std::uniform_int_distribution assumes interval is closed,
          // i.e., [low, high], but we want [low, high) instead. Hence high-1 is
          // used as the upper range.
          std::uniform_int_distribution<int64_t> generator(
              static_cast<int64_t>(low.Get<ReturnT>({})),
              static_cast<int64_t>(high.Get<ReturnT>({})) - 1);

          TF_RETURN_IF_ERROR(result.Populate<ReturnT>(
              [&](absl::Span<const int64_t> /*indexes*/) {
                return static_cast<ReturnT>(generator(parent_->engine_));
              }));
          break;
        }
        case RNG_NORMAL: {
          return Unimplemented(
              "Normal distribution is not supported for integral types.");
        }
        default:
          return UnimplementedStrCat("The distribution ",
                                     RandomDistribution_Name(distribution),
                                     " is not implemented.");
      }
      parent_->SetEvaluatedLiteralFor(random, std::move(result));
      return absl::OkStatus();
    }
    return UnsupportedTypeError(random);
  }

 private:
  template <typename UnaryOp>
  absl::StatusOr<Literal> ElementWiseUnaryOp(const HloInstruction* instruction,
                                             UnaryOp&& unary_op) {
    static_assert(std::is_invocable_r_v<ElementwiseT, UnaryOp, ElementwiseT>,
                  "Invalid UnaryOp signature");

    const Literal& operand_literal =
        parent_->GetEvaluatedLiteralFor(instruction->operand(0));
    TF_ASSIGN_OR_RETURN(
        auto result_literal,
        (HloEvaluator::ElementWiseUnaryOpImpl<ReturnT, ReturnT>(
            instruction, ConvertUnaryFunction(unary_op), operand_literal)));

    return result_literal;
  }

  template <typename BinaryOp>
  absl::StatusOr<Literal> ElementWiseBinaryOp(const HloInstruction* instruction,
                                              BinaryOp&& binary_op) {
    static_assert(std::is_invocable_r_v<ElementwiseT, BinaryOp, ElementwiseT,
                                        ElementwiseT>,
                  "Invalid BinaryOp signature");

    Shape shape = GetShapeWithLayout(instruction->shape());
    const auto* lhs = instruction->operand(0);
    const auto* rhs = instruction->operand(1);
    TF_RET_CHECK(ShapeUtil::SameDimensions(shape, rhs->shape()));
    TF_RET_CHECK(ShapeUtil::SameDimensions(lhs->shape(), rhs->shape()));

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);

    Literal result(shape);

    // If layout is the same, we can use linear indexing into the literals.
    const Layout& lhs_layout = lhs_literal.shape().layout();
    const Layout& rhs_layout = rhs_literal.shape().layout();
    bool same_layout = LayoutUtil::Equal(lhs_layout, rhs_layout);

    if (same_layout) {
      TF_RETURN_IF_ERROR(result.PopulateLinearParallel<ReturnT>(
          [&](int64_t linear_index, int) {
            return ConvertBinaryFunction(binary_op)(
                lhs_literal.GetLinear<ReturnT>(linear_index),
                rhs_literal.GetLinear<ReturnT>(linear_index));
          }));
    } else {
      TF_RETURN_IF_ERROR(result.PopulateParallel<ReturnT>(
          [&](absl::Span<const int64_t> multi_index, int) {
            return ConvertBinaryFunction(binary_op)(
                lhs_literal.Get<ReturnT>(multi_index),
                rhs_literal.Get<ReturnT>(multi_index));
          }));
    }

    return result;
  }

  template <typename LhsType, typename RhsType, typename EhsType,
            typename TernaryOp>
  absl::StatusOr<Literal> ElementwiseTernaryOp(
      const HloInstruction* instruction, TernaryOp&& ternary_op) {
    static_assert(
        std::is_invocable_r_v<ReturnT, TernaryOp, LhsType, RhsType, EhsType>,
        "Invalid TernaryOp signature");

    Shape shape = GetShapeWithLayout(instruction->shape());
    const auto* lhs = instruction->operand(0);
    const auto* rhs = instruction->operand(1);
    const auto* ehs = instruction->operand(2);
    TF_RET_CHECK(ShapeUtil::SameDimensions(shape, lhs->shape()));
    TF_RET_CHECK(ShapeUtil::SameDimensions(lhs->shape(), rhs->shape()));
    TF_RET_CHECK(ShapeUtil::SameDimensions(rhs->shape(), ehs->shape()));

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);
    const Literal& ehs_literal = parent_->GetEvaluatedLiteralFor(ehs);

    Literal result(shape);

    // If layout is the same, we can use linear indexing into the literals.
    const Layout& lhs_layout = lhs_literal.shape().layout();
    const Layout& rhs_layout = rhs_literal.shape().layout();
    const Layout& ehs_layout = ehs_literal.shape().layout();
    bool same_layout = LayoutUtil::Equal(lhs_layout, rhs_layout) &&
                       LayoutUtil::Equal(rhs_layout, ehs_layout);

    if (same_layout) {
      TF_RETURN_IF_ERROR(result.PopulateLinearParallel<ReturnT>(
          [&](int64_t linear_index, int) {
            return ternary_op(lhs_literal.GetLinear<LhsType>(linear_index),
                              rhs_literal.GetLinear<RhsType>(linear_index),
                              ehs_literal.GetLinear<EhsType>(linear_index));
          }));

    } else {
      TF_RETURN_IF_ERROR(result.PopulateParallel<ReturnT>(
          [&](absl::Span<const int64_t> multi_index, int) {
            return ternary_op(lhs_literal.Get<LhsType>(multi_index),
                              rhs_literal.Get<RhsType>(multi_index),
                              ehs_literal.Get<EhsType>(multi_index));
          }));
    }

    return result;
  }

  template <typename NativeT>
  static bool IsShiftOutOfBounds(ElementwiseT rhs) {
    using UnsignedT = make_specialized_unsigned_t<NativeT>;
    UnsignedT lhs_bits_unsigned =
        static_cast<UnsignedT>(std::numeric_limits<UnsignedT>::digits);
    UnsignedT rhs_unsigned = static_cast<UnsignedT>(rhs);
    return rhs_unsigned >= lhs_bits_unsigned;
  }

  HloEvaluator* parent_;
};

// These extern templates prevent users of this class from implicitly
// instantiating it.  We explicitly instantiate this class in the various
// hlo_evaluator_typed_visitor*.cc files.
extern template class HloEvaluatorTypedVisitor<bool>;
extern template class HloEvaluatorTypedVisitor<u1, uint64_t>;
extern template class HloEvaluatorTypedVisitor<u2, uint64_t>;
extern template class HloEvaluatorTypedVisitor<u4, uint64_t>;
extern template class HloEvaluatorTypedVisitor<uint8_t, uint64_t>;
extern template class HloEvaluatorTypedVisitor<uint16_t, uint64_t>;
extern template class HloEvaluatorTypedVisitor<uint32_t, uint64_t>;
extern template class HloEvaluatorTypedVisitor<uint64_t>;
extern template class HloEvaluatorTypedVisitor<s1, int64_t>;
extern template class HloEvaluatorTypedVisitor<s2, int64_t>;
extern template class HloEvaluatorTypedVisitor<s4, int64_t>;
extern template class HloEvaluatorTypedVisitor<int8_t, int64_t>;
extern template class HloEvaluatorTypedVisitor<int16_t, int64_t>;
extern template class HloEvaluatorTypedVisitor<int32_t, int64_t>;
extern template class HloEvaluatorTypedVisitor<int64_t>;
extern template class HloEvaluatorTypedVisitor<Eigen::half, float>;
extern template class HloEvaluatorTypedVisitor<float>;
extern template class HloEvaluatorTypedVisitor<double>;
extern template class HloEvaluatorTypedVisitor<complex64>;
extern template class HloEvaluatorTypedVisitor<complex128>;
extern template class HloEvaluatorTypedVisitor<bfloat16, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float4_e2m1fn, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e5m2, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e4m3, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e4m3fn, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e4m3b11fnuz, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e5m2fnuz, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e4m3fnuz, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e3m4, float>;
extern template class HloEvaluatorTypedVisitor<tsl::float8_e8m0fnu, float>;

}  // namespace xla

#endif  // XLA_HLO_EVALUATOR_HLO_EVALUATOR_TYPED_VISITOR_H_
