/* Copyright 2025 The OpenXLA Authors.

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

#include "xla/backends/cpu/autotuner/cpu_profiler.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "xla/backends/autotuner/profiler.h"
#include "xla/executable_run_options.h"
#include "xla/literal.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/cpu/cpu_executable.h"
#include "xla/service/executable.h"
#include "xla/service/maybe_owning_device_memory.h"
#include "xla/shape_util.h"
#include "xla/tsl/platform/errors.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/casts.h"

namespace xla::cpu {

namespace {

static absl::StatusOr<std::unique_ptr<InputBuffers>> PrepareBackedBuffers(
    absl::Span<const BufferAllocation> allocations) {
  auto backed_buffers = std::make_unique<LiteralBackedCpuBuffers>();
  backed_buffers->buffers.reserve(allocations.size());
  backed_buffers->backing_literals.reserve(allocations.size());

  for (const BufferAllocation& allocation : allocations) {
    // Allocating allocation.size() bytes.
    Shape shape = ShapeUtil::MakeShape(U8, {allocation.size()});
    Literal literal(shape, true);

    backed_buffers->backing_literals.push_back(std::move(literal));
    backed_buffers->buffers.emplace_back(stream_executor::DeviceMemoryBase(
        backed_buffers->backing_literals.back().untyped_data(),
        backed_buffers->backing_literals.back().size_bytes()));
  }
  return backed_buffers;
}

}  // namespace

absl::StatusOr<std::unique_ptr<InputBuffers>> CpuProfiler::CreateInputBuffers(
    const Executable* executable) {
  const CpuExecutable* cpu_executable =
      tsl::down_cast<const CpuExecutable*>(executable);
  return PrepareBackedBuffers(
      cpu_executable->buffer_assignment().Allocations());
}

std::unique_ptr<Profiler> CpuProfiler::Create(ProfileOptions options) {
  return absl::WrapUnique(new CpuProfiler(options));
}

absl::StatusOr<ProfileResult> CpuProfiler::Profile(
    Executable* executable, const InputBuffers& buffers) {
  const LiteralBackedCpuBuffers& literal_backed_buffers =
      tsl::down_cast<const LiteralBackedCpuBuffers&>(buffers);
  {
    // Warm up run.
    TF_RETURN_IF_ERROR(Execute(executable, literal_backed_buffers.buffers,
                               /*profile=*/nullptr));
  }

  ExecutionProfile profile;
  profile.set_warmup_run_executed(true);

  TF_RETURN_IF_ERROR(
      Execute(executable, literal_backed_buffers.buffers, &profile));

  return ProfileResult{absl::Nanoseconds(profile.compute_time_ns())};
}

absl::Status CpuProfiler::Execute(
    Executable* executable, absl::Span<const MaybeOwningDeviceMemory> buffers,
    ExecutionProfile* profile) {
  ExecutableRunOptions run_options;
  run_options.set_execution_profile(profile);
  run_options.set_device_ordinal(0);

  CpuExecutable* cpu_executable = tsl::down_cast<CpuExecutable*>(executable);

  TF_RETURN_IF_ERROR(cpu_executable->ExecuteThunks(&run_options, buffers));

  return absl::OkStatus();
}

}  // namespace xla::cpu
