#pragma once

#include "selected_capacity_internal.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/selected_window_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <string>

namespace pineforge::detail::selected_capacity {

// These callbacks are deliberately plain function pointers. A field owner can
// provide one definition for the callback it owns. The driver invokes a
// callback only when its corresponding Bound is present; a null requested
// slot is NotAdmitted/MissingObserver before any input is read. Record
// callbacks receive every validated role and must apply their own role rule.
using RecordObserver = void (*)(State&, const Limits&, const RecordEvent&);
using BytesObserver = void (*)(State&, const Limits&, std::uint64_t actual_csv_bytes);

struct ObserverTable {
    RecordObserver source_rows = nullptr;
    RecordObserver primary_rows = nullptr;
    RecordObserver script_bars = nullptr;
    RecordObserver auxiliary_rows = nullptr;
    BytesObserver bytes = nullptr;
};

// A StreamInput is borrowed for the duration of run_driver. The caller keeps
// both the stream and this descriptor array alive and unchanged until the
// call returns. `label` is diagnostic only and is never used as source
// authentication or provider identity. Streams must deliver the original CSV
// octets (open files in binary mode). Distinct streams over the same physical
// file may supply different logical roles, each with its own byte charge.
// Reusing a stream object or streambuf is rejected before reading.
struct StreamInput {
    Role role = Role::Source;
    std::istream* stream = nullptr;
    const char* label = nullptr;

    StreamInput() noexcept = default;
    StreamInput(Role input_role, std::istream& input_stream,
                const char* input_label = nullptr) noexcept
        : role(input_role), stream(&input_stream), label(input_label) {}
    StreamInput(Role input_role, std::istream* input_stream,
                const char* input_label = nullptr) noexcept
        : role(input_role), stream(input_stream), label(input_label) {}
};

using MaterializerSink = void (*)(void* sink_context, const Bar& bar);
// The Bar reference is borrowed for this invocation; a sink that retains
// a record must copy its values after its own capacity admission.

enum class DriverStatus {
    NotAdmitted,
    Completed,
    FormatError,
    IoError,
    InputError,
    CalendarError,
};

enum class DriverReason {
    None,
    NoRequestedFields,
    MissingObserver,
    MissingSink,
    MissingInput,
    DuplicateInput,
    InvalidInput,
    NoData,
    RequestInvalid,
    CalendarUnsupported,
    BoundaryUnaligned,
    FeedRangeInvalid,
    RowsUnordered,
    CsvColumns,
    CsvValue,
    CsvUtf8,
    CsvSyntax,
    ObserverFailure,
    SinkFailure,
    InternalError,
    InputReadFailure,
};

struct DriverCoverage {
    // These are committed physical observations. A failed current record is
    // excluded; the valid prefix remains visible.
    std::uint64_t source_rows = 0;
    std::uint64_t primary_rows = 0;
    std::uint64_t auxiliary_rows = 0;
    std::uint64_t primary_script_events = 0;
    std::uint64_t source_bytes = 0;
    std::uint64_t primary_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;

    // Auxiliary inputs are aggregated here deliberately. The driver never
    // retains a per-stream coverage vector.
    std::uint64_t source_streams = 0;
    std::uint64_t primary_streams = 0;
    std::uint64_t auxiliary_streams = 0;
    bool source_complete = false;
    bool primary_complete = false;
    bool auxiliary_complete = false;
};

struct DriverResult {
    DriverStatus status = DriverStatus::NotAdmitted;
    DriverReason reason = DriverReason::InternalError;

    // Set when the reason identifies a selected-capacity field. `field` is
    // otherwise left at its zero value and has_field is false.
    bool has_field = false;
    Field field = Field::SourceRows;
    Role role = Role::Source;
    std::size_t input_index = std::numeric_limits<std::size_t>::max();
    std::uint64_t row_index = 0;

    // Native request detail, when request admission was attempted.
    bool plan_attempted = false;
    SelectedPlanStatus plan_status = SelectedPlanStatus::InternalError;
    std::string option;
    int bound = -1;
    std::int64_t value_ms = 0;
    std::int64_t previous_boundary_ms = 0;
    std::int64_t next_boundary_ms = 0;

    // Only requested observer slots update State. Coverage independently
    // records the actual validated inputs, including roles with absent bounds.
    State state{};
    DriverCoverage coverage{};
    std::string message;
};

// The stream span is intentionally a pointer plus count. It is a borrowed
// descriptor, not an owning or growable input directory.
//
// All-absent limits return NotAdmitted/NoRequestedFields without touching any
// input, sink or calendar. Otherwise a Primary input and sink are required.
// Source is required only for a present source_rows or bytes bound. Auxiliary
// may be empty; every supplied distinct Auxiliary stream is aggregated.
// Every supplied stream is validated. Source and Auxiliary never reach sink.
// Bytes includes each supplied Source and retained Primary/Auxiliary CSV,
// including its header, separators and actual LF/CRLF terminators.
// Source/Auxiliary order is per stream; Primary order spans all Primary
// descriptors in their supplied order. Labels never establish data identity.
//
// Script admission and per-Primary-row calendar work run only for a present
// script_bars bound. All supplied Primary rows are observed before any later
// trimming; an opened partial tail is an event, independent of sealing.
//
// Header and record observations use tentative State/Coverage, committed only
// after every callback for that event returns. Observer/sink exceptions escape
// unchanged, including coded failure arguments and English. The driver cannot
// rewind consumed streams or undo effects a callback makes outside its State;
// sinks must not publish a partially accepted record before throwing.
// ObserverFailure and SinkFailure are reserved and are never returned.
DriverResult run_driver(const StreamInput* inputs, std::size_t count,
                        const SelectedPlanRequest& request, const Limits& limits,
                        const ObserverTable& observers, MaterializerSink sink,
                        void* sink_context);

// Product-owned strong binding to the five declarations in the shared
// contract. No weak symbols, registration, null substitutions or test builder.
ObserverTable make_product_observer_table() noexcept;

// The production entry: callers supply actual inputs and a Primary sink,
// never an observer table or counters. Owner definitions must be linked.
DriverResult materialize_selected_capacity(
    const StreamInput* inputs, std::size_t count,
    const SelectedPlanRequest& request, const Limits& limits,
    MaterializerSink sink, void* sink_context);

}  // namespace pineforge::detail::selected_capacity
