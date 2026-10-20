#pragma once

#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <stdexcept>
#include <system_error>

namespace pineforge::detail {

// Closed vocabulary: layout is a refusal resource, not a tenth byte region.
enum class ScratchResource : std::uint32_t {
    layout = 0,
    sequence_data,
    sequence_index,
    erased_data,
    erased_index,
    consumed_data,
    consumed_index,
    cohort,
    exit_rank,
    successor_roots,
};

inline const char* scratch_resource_name(ScratchResource resource) noexcept {
    switch (resource) {
    case ScratchResource::layout: return "layout";
    case ScratchResource::sequence_data: return "sequence_data";
    case ScratchResource::sequence_index: return "sequence_index";
    case ScratchResource::erased_data: return "erased_data";
    case ScratchResource::erased_index: return "erased_index";
    case ScratchResource::consumed_data: return "consumed_data";
    case ScratchResource::consumed_index: return "consumed_index";
    case ScratchResource::cohort: return "cohort";
    case ScratchResource::exit_rank: return "exit_rank";
    case ScratchResource::successor_roots: return "successor_roots";
    }
    return "layout";
}

class ScratchCapacityError final : public std::runtime_error, public RunFailureInfo {
public:
    explicit ScratchCapacityError(ScratchResource resource,
                                  const char* operation = "scratch capacity exceeded")
        : std::runtime_error(operation),
          RunFailureInfo(RunFailureCode::scratch_capacity_exceeded,
                         {{"resource", scratch_resource_name(resource)}}) {}
};

// Preserve errno and the existing system_error English; only table scratch
// failures use this code. The report sink retains report_sink_failed.
class ScratchIoError final : public std::system_error, public RunFailureInfo {
public:
    ScratchIoError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::scratch_io_failed, {}) {}
};

[[noreturn]] inline void raise_scratch_io_error(int error, const char* operation) {
    throw ScratchIoError(error, operation);
}

} // namespace pineforge::detail
