#pragma once

#include "selected_capacity_driver.hpp"

namespace pineforge::detail::selected_capacity {
using ByteSource = std::ptrdiff_t (*)(void*, char*, std::size_t);
using ByteForward = bool (*)(void*, const char*, std::size_t);

struct SourceStageInput {
    ByteSource read = nullptr;
    void* read_ctx = nullptr;
    const char* label = nullptr;
};

// Optional caller-owned diagnostics survive a resource exception. Counts
// advance only when CsvReader consumes an octet, never on chunk read-ahead.
struct SourceStageProgress {
    std::size_t input_index = std::numeric_limits<std::size_t>::max();
    std::uint64_t row_index = 0;
    std::uint64_t* bytes_read = nullptr;
    bool* complete = nullptr;
    std::size_t slots = 0;
};

struct SourceStageOptions {
    std::size_t max_record_bytes = 0;
    std::size_t chunk_bytes = 0;
    SourceStageProgress* progress = nullptr;
};

enum class SourceStageReason {
    None, NoStageFields, MissingInput, RowsUnordered, CsvColumns, CsvValue,
    CsvUtf8, CsvSyntax, InputReadFailure, RecordTooLong, ForwardFailure, InternalError
};

struct SourceStageResult {
    DriverStatus status = DriverStatus::NotAdmitted;
    SourceStageReason reason = SourceStageReason::InternalError;
    std::size_t input_index = std::numeric_limits<std::size_t>::max();
    std::uint64_t row_index = 0;
    State state{};
    DriverCoverage coverage{};
    std::uint64_t forwarded_bytes = 0;
    std::string message;
};

SourceStageResult admit_source_stage(const SourceStageInput*, std::size_t,
    const Limits&, const SourceStageOptions&, ByteForward, void*);
}  // namespace pineforge::detail::selected_capacity
