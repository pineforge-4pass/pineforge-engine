#pragma once

#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
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

// Locale-independent decimal grouping, shared by all count/byte refusals.
inline std::string scratch_limit_number(std::uint64_t limit) {
    char digits[26]; // 20 decimal digits and six separators fit.
    auto* end = digits + sizeof digits;
    auto* at = end;
    unsigned group = 0;
    do {
        if (group == 3) {
            *--at = ',';
            group = 0;
        }
        *--at = static_cast<char>('0' + limit % 10);
        limit /= 10;
        ++group;
    } while (limit != 0);
    return {at, end};
}

inline std::string scratch_limit_message(const char* prefix, std::uint64_t limit,
                                        const char* suffix) {
    std::string message(prefix);
    message += scratch_limit_number(limit);
    message += suffix;
    return message;
}

inline std::string scratch_count_limit_message(ScratchResource resource, std::uint64_t limit) {
    switch (resource) {
    case ScratchResource::cohort:
        return scratch_limit_message("more than ", limit,
            " distinct order ids in one run; reuse ids, e.g. build the id from a modulo of bar_index");
    case ScratchResource::erased_index:
        return scratch_limit_message("more than ", limit,
            " retired order-id keys in one run (each retired order uses 2); reuse ids, e.g. a modulo of bar_index");
    case ScratchResource::consumed_index:
        return scratch_limit_message("more than ", limit,
            " keys for partially filled exits in one run; reuse ids, e.g. a modulo of bar_index");
    case ScratchResource::exit_rank:
        return scratch_limit_message("scratch capacity exceeded: more than ", limit,
            " exit rank ids in one run; reuse ids");
    default:
        return scratch_limit_message("scratch capacity exceeded: more than ", limit,
            " keys in one run; reuse ids");
    }
}

inline std::string scratch_byte_limit_message(ScratchResource resource, std::uint64_t limit) {
    switch (resource) {
    case ScratchResource::cohort:
        return scratch_limit_message("order ids use more than ", limit,
            " bytes in one run; use shorter ids or reuse ids");
    case ScratchResource::erased_data:
        return scratch_limit_message("retired order ids use more than ", limit,
            " bytes in one run; use shorter ids or reuse ids");
    case ScratchResource::consumed_data:
        return scratch_limit_message("partially filled exits use more than ", limit,
            " bytes in one run; use shorter ids or reuse ids");
    case ScratchResource::exit_rank:
        return scratch_limit_message("scratch capacity exceeded: exit rank ids use more than ", limit,
            " bytes in one run; use shorter ids or reuse ids");
    default:
        return scratch_limit_message("scratch capacity exceeded: ids use more than ", limit,
            " bytes in one run; use shorter ids or reuse ids");
    }
}

inline std::string scratch_resident_cohort_limit_message(std::uint64_t limit) {
    return scratch_limit_message("more than ", limit,
        " order ids active at once (placed and not yet closed or cancelled); "
        "close or cancel orders sooner, or reuse ids");
}

inline std::string scratch_object_limit_message(std::uint64_t limit,
                                               std::uint64_t live,
                                               std::uint64_t cap) {
    std::string message = scratch_limit_message("more than ", limit,
        " store objects live at once; ");
    message += scratch_limit_number(limit);
    message += " is the most allowed in this store; attempted live count ";
    message += scratch_limit_number(live);
    message += ", approved maximum ";
    message += scratch_limit_number(cap);
    return message;
}

class ScratchCapacityError final : public std::runtime_error, public RunFailureInfo {
public:
    explicit ScratchCapacityError(ScratchResource resource,
                                  const char* operation = "scratch capacity exceeded")
        : std::runtime_error(operation),
          RunFailureInfo(RunFailureCode::scratch_capacity_exceeded,
                         {{"resource", scratch_resource_name(resource)}}) {}
    ScratchCapacityError(ScratchResource resource, const std::string& message)
        : std::runtime_error(message),
          RunFailureInfo(RunFailureCode::scratch_capacity_exceeded,
                         {{"resource", scratch_resource_name(resource)}}) {}
};

// Preserve errno and the existing system_error English; only table scratch
// failures use this code. The report sink retains report_sink_failed.
class AuxiliaryScratchIoError final : public std::system_error, public RunFailureInfo {
public:
    AuxiliaryScratchIoError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::scratch_io_failed, {}) {}
    explicit AuxiliaryScratchIoError(const std::system_error& error)
        : std::system_error(error),
          RunFailureInfo(RunFailureCode::scratch_io_failed, {}) {}
};

[[noreturn]] inline void raise_scratch_io_error(int error, const char* operation) {
    throw AuxiliaryScratchIoError(error, operation);
}

} // namespace pineforge::detail
