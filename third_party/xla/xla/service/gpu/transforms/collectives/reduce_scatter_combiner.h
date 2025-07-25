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

#ifndef XLA_SERVICE_GPU_TRANSFORMS_COLLECTIVES_REDUCE_SCATTER_COMBINER_H_
#define XLA_SERVICE_GPU_TRANSFORMS_COLLECTIVES_REDUCE_SCATTER_COMBINER_H_

#include <cstdint>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/pass/hlo_pass_interface.h"
#include "xla/service/reduce_scatter_combiner.h"

namespace xla::gpu {

// Similarly to `ReduceScatterCombiner` pass, combines `ReduceScatter` ops into
// a single larger `ReduceScatter` op to maximize network bandwidth usage.
// Additionally, if no flags are set for combiner thresholds, the pass will try
// to figure out the optimal combiner threshold by itself.
class GpuReduceScatterCombiner : public ReduceScatterCombiner {
 public:
  GpuReduceScatterCombiner(const int default_combine_threshold_in_bytes,
                           const int64_t combine_threshold_in_bytes,
                           const int64_t combine_threshold_count,
                           const bool combine_by_dim)
      : ReduceScatterCombiner(combine_threshold_in_bytes,
                              combine_threshold_count, combine_by_dim),
        default_combine_threshold_in_bytes_(
            default_combine_threshold_in_bytes) {}

  absl::string_view name() const override {
    return "gpu-reduce-scatter-combiner";
  }

  using HloPassInterface::Run;
  absl::StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;

 private:
  const int default_combine_threshold_in_bytes_;
};

}  // namespace xla::gpu

#endif  // XLA_SERVICE_GPU_TRANSFORMS_COLLECTIVES_REDUCE_SCATTER_COMBINER_H_
