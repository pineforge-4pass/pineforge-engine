#include "selected_capacity_driver.hpp"

#include "native_calendar_memo.hpp"
#include "selected_capacity_csv_reader.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/run_failure.hpp>

#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace pineforge::detail::selected_capacity {
namespace {

namespace nc = native_calendar;

struct InputCounts {
    std::uint64_t source = 0;
    std::uint64_t primary = 0;
    std::uint64_t auxiliary = 0;
};

struct Clock {
    nc::SessionCalendar calendar;
    nc::Timeframe input;
    nc::Timeframe script;
    nc::SessionDayMemo memo;
    bool passthrough = false;
    bool tolerant = true;
};

void add(std::uint64_t& value, std::uint64_t delta) {
    if (delta > std::numeric_limits<std::uint64_t>::max() - value) {
        throw coded<std::runtime_error>(
            RunFailureCode::engine_invariant, {}, "Pine native run counter exhausted");
    }
    value += delta;
}

bool any_requested(const Limits& limits) noexcept {
    return limits.source_rows.present || limits.primary_rows.present || limits.script_bars.present
        || limits.auxiliary_rows.present || limits.bytes.present;
}

void fail(DriverResult& result, DriverStatus status, DriverReason reason,
          const char* message) {
    result.status = status;
    result.reason = reason;
    result.message = message;
}

bool require_observer(DriverResult& result, const Bound& bound, bool supplied, Field field) {
    if (!bound.present || supplied) return true;
    fail(result, DriverStatus::NotAdmitted, DriverReason::MissingObserver,
         "A requested selected-capacity observer is absent from the supplied table");
    result.has_field = true;
    result.field = field;
    return false;
}

bool admit_observers(DriverResult& result, const Limits& limits, const ObserverTable& observers) {
    return require_observer(result, limits.source_rows, observers.source_rows != nullptr,
                            Field::SourceRows)
        && require_observer(result, limits.primary_rows, observers.primary_rows != nullptr,
                            Field::PrimaryRows)
        && require_observer(result, limits.script_bars, observers.script_bars != nullptr,
                            Field::ScriptBars)
        && require_observer(result, limits.auxiliary_rows, observers.auxiliary_rows != nullptr,
                            Field::AuxiliaryRows)
        && require_observer(result, limits.bytes, observers.bytes != nullptr, Field::Bytes);
}

bool admit_inputs(DriverResult& result, const StreamInput* inputs, std::size_t count,
                  const Limits& limits, MaterializerSink sink, InputCounts& expected) {
    if (count == 0 || inputs == nullptr) {
        fail(result, DriverStatus::NotAdmitted, DriverReason::MissingInput,
             "Selected-capacity materialization requires borrowed CSV inputs");
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        result.input_index = i;
        result.role = inputs[i].role;
        if (inputs[i].stream == nullptr || inputs[i].stream->rdbuf() == nullptr) {
            fail(result, DriverStatus::NotAdmitted, DriverReason::InvalidInput,
                 "CSV input has no stream or stream buffer");
            return false;
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (inputs[i].stream == inputs[previous].stream
                || inputs[i].stream->rdbuf() == inputs[previous].stream->rdbuf()) {
                fail(result, DriverStatus::NotAdmitted, DriverReason::DuplicateInput,
                     "The same borrowed CSV stream or stream buffer was supplied twice");
                return false;
            }
        }
        switch (inputs[i].role) {
        case Role::Source: add(expected.source, 1); break;
        case Role::Primary: add(expected.primary, 1); break;
        case Role::Auxiliary: add(expected.auxiliary, 1); break;
        default:
            fail(result, DriverStatus::NotAdmitted, DriverReason::InvalidInput,
                 "CSV input role is invalid");
            return false;
        }
    }
    result.input_index = std::numeric_limits<std::size_t>::max();
    if (expected.primary == 0) {
        result.role = Role::Primary;
        fail(result, DriverStatus::NotAdmitted, DriverReason::MissingInput,
             "Selected-capacity materialization requires Primary CSV");
        return false;
    }
    if (expected.source == 0 && (limits.source_rows.present || limits.bytes.present)) {
        result.role = Role::Source;
        result.has_field = true;
        result.field = limits.source_rows.present ? Field::SourceRows : Field::Bytes;
        fail(result, DriverStatus::NotAdmitted, DriverReason::MissingInput,
             "The source-row or byte bound requires original Source CSV");
        return false;
    }
    if (sink == nullptr) {
        result.role = Role::Primary;
        fail(result, DriverStatus::NotAdmitted, DriverReason::MissingSink,
             "Primary CSV requires a materializer sink");
        return false;
    }
    return true;
}

DriverReason plan_reason(SelectedPlanStatus status) noexcept {
    switch (status) {
    case SelectedPlanStatus::Ok: return DriverReason::None;
    case SelectedPlanStatus::RequestInvalid: return DriverReason::RequestInvalid;
    case SelectedPlanStatus::CalendarUnsupported: return DriverReason::CalendarUnsupported;
    case SelectedPlanStatus::BoundaryUnaligned: return DriverReason::BoundaryUnaligned;
    case SelectedPlanStatus::FeedRangeInvalid: return DriverReason::FeedRangeInvalid;
    case SelectedPlanStatus::RowsUnordered: return DriverReason::RowsUnordered;
    case SelectedPlanStatus::InternalError: return DriverReason::InternalError;
    }
    return DriverReason::InternalError;
}

bool admit_clock(DriverResult& result, const SelectedPlanRequest& request, Clock& clock) {
    // Use canonical request/clock/boundary admission, with no row array and
    // without importing any of the planner's predicted/sealed counters.
    result.plan_attempted = true;
    const SelectedPrimaryPlan admission = plan_selected_primary(nullptr, 0, request);
    result.plan_status = admission.status;
    if (admission.status != SelectedPlanStatus::Ok) {
        fail(result, DriverStatus::NotAdmitted, plan_reason(admission.status),
             "Native selected request admission failed");
        result.option = admission.option;
        result.bound = admission.bound;
        result.value_ms = admission.value_ms;
        result.previous_boundary_ms = admission.previous_boundary_ms;
        result.next_boundary_ms = admission.next_boundary_ms;
        return false;
    }
    try {
        auto calendar = nc::parse_session(request.session.empty() ? "24x7" : request.session,
                                         request.engine_timezone.empty() ? "UTC"
                                                                         : request.engine_timezone);
        auto input = nc::parse_timeframe(request.input_tf);
        auto script = nc::parse_timeframe(request.script_tf);
        if (!calendar || !input || !script) {
            fail(result, DriverStatus::NotAdmitted, DriverReason::CalendarUnsupported,
                 "Native calendar could not be reconstructed after request admission");
            result.plan_status = SelectedPlanStatus::CalendarUnsupported;
            result.option = "calendar";
            return false;
        }
        const auto pairing = nc::compatibility(*input, *script);
        clock.calendar = std::move(*calendar);
        clock.input = std::move(*input);
        clock.script = std::move(*script);
        clock.passthrough = pairing.pairing == nc::TimeframePairing::Passthrough;
        clock.tolerant = request.feed_tolerant;
        return true;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        fail(result, DriverStatus::NotAdmitted, DriverReason::CalendarUnsupported,
             "Native calendar admission raised an exception");
        result.plan_status = SelectedPlanStatus::CalendarUnsupported;
        result.option = "calendar";
        return false;
    }
}

bool resolve_primary(DriverResult& result, const SelectedPlanRequest& request,
                     Clock& clock, const Bar& bar, RecordEvent& event) {
    if (bar.timestamp < request.fed_start_ms || bar.timestamp >= request.end_ms) {
        fail(result, DriverStatus::InputError, DriverReason::FeedRangeInvalid,
             "Primary timestamp is outside the selected feed range");
        result.plan_status = SelectedPlanStatus::FeedRangeInvalid;
        result.option = "rows";
        result.bound = bar.timestamp < request.fed_start_ms ? 2 : 1;
        result.value_ms = bar.timestamp;
        result.previous_boundary_ms = request.fed_start_ms;
        result.next_boundary_ms = request.end_ms;
        return false;
    }
    std::optional<nc::NativeInterval> input;
    std::optional<nc::NativeInterval> script;
    try {
        // Every supplied Primary row asks the native calendar, including
        // pretrim rows and the final partial bucket. No timestamp division.
        input = nc::interval_containing(clock.calendar, clock.input, bar.timestamp, clock.memo);
        const bool raw_passthrough = clock.tolerant && clock.passthrough;
        const std::int64_t input_open =
            raw_passthrough || !input ? bar.timestamp : input->open_ms;
        script = nc::interval_containing(clock.calendar, clock.script, input_open, clock.memo);
        if (raw_passthrough) {
            // The canonical FeedTolerant passthrough partition is the actual
            // row timestamp, even if it lies inside a nominal calendar slot.
            event.script_bucket_key = bar.timestamp;
        } else if (script && (input || clock.tolerant)) {
            event.script_bucket_key = script->open_ms;
            if (script->open_ms < request.start_ms
                && request.start_ms < script->next_period_open_ms) {
                fail(result, DriverStatus::CalendarError, DriverReason::CalendarUnsupported,
                     "A native script interval straddles the selected start");
                result.plan_status = SelectedPlanStatus::CalendarUnsupported;
                result.option = "start_ms";
                result.bound = 0;
                result.value_ms = request.start_ms;
                result.previous_boundary_ms = script->open_ms;
                result.next_boundary_ms = script->next_period_open_ms;
                return false;
            }
        } else if (clock.tolerant) {
            // The planner's FeedTolerant missing-interval policy partitions
            // at the actual input open, never a guessed bucket duration.
            event.script_bucket_key = input_open;
        } else {
            fail(result, DriverStatus::CalendarError, DriverReason::CalendarUnsupported,
                 "Native calendar has no input or script interval for this Primary row");
            result.plan_status = SelectedPlanStatus::CalendarUnsupported;
            result.option = "rows";
            result.value_ms = bar.timestamp;
            return false;
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        // This region calls only the calendar; coded observer and sink
        // exceptions never enter it.
        fail(result, DriverStatus::CalendarError, DriverReason::CalendarUnsupported,
             "Native Primary-row calendar resolution raised an exception");
        result.plan_status = SelectedPlanStatus::CalendarUnsupported;
        result.option = "calendar";
        result.value_ms = bar.timestamp;
        return false;
    }
    event.has_script_bucket = true;
    return true;
}

void charge_bytes(DriverCoverage& coverage, Role role, std::uint64_t bytes) {
    switch (role) {
    case Role::Source: add(coverage.source_bytes, bytes); break;
    case Role::Primary: add(coverage.primary_bytes, bytes); break;
    case Role::Auxiliary: add(coverage.auxiliary_bytes, bytes); break;
    }
}

void charge_record(DriverCoverage& coverage, const RecordEvent& event) {
    switch (event.role) {
    case Role::Source: add(coverage.source_rows, 1); break;
    case Role::Primary:
        add(coverage.primary_rows, 1);
        if (event.has_script_bucket) add(coverage.primary_script_events, 1);
        break;
    case Role::Auxiliary: add(coverage.auxiliary_rows, 1); break;
    }
}

void observe_record(State& state, const Limits& limits, const ObserverTable& observers,
                    const RecordEvent& event) {
    if (limits.source_rows.present) observers.source_rows(state, limits, event);
    if (limits.primary_rows.present) observers.primary_rows(state, limits, event);
    if (limits.script_bars.present) observers.script_bars(state, limits, event);
    if (limits.auxiliary_rows.present) observers.auxiliary_rows(state, limits, event);
}

bool read_ok(DriverResult& result, const CsvRead& read) {
    if (read.status == CsvStatus::Ok) return true;
    if (read.status == CsvStatus::IoError) {
        fail(result, DriverStatus::IoError, DriverReason::InputReadFailure, read.message);
        return false;
    }
    DriverReason reason = DriverReason::CsvSyntax;
    switch (read.reason) {
    case CsvReason::Columns: reason = DriverReason::CsvColumns; break;
    case CsvReason::Value: reason = DriverReason::CsvValue; break;
    case CsvReason::Encoding: reason = DriverReason::CsvUtf8; break;
    case CsvReason::Syntax: break;
    case CsvReason::None: reason = DriverReason::InternalError; break;
    }
    fail(result, DriverStatus::FormatError, reason, read.message);
    return false;
}

void complete_stream(DriverCoverage& coverage, Role role, const InputCounts& expected) {
    switch (role) {
    case Role::Source: add(coverage.source_streams, 1); break;
    case Role::Primary: add(coverage.primary_streams, 1); break;
    case Role::Auxiliary: add(coverage.auxiliary_streams, 1); break;
    }
    coverage.source_complete =
        expected.source != 0 && coverage.source_streams == expected.source;
    coverage.primary_complete =
        expected.primary != 0 && coverage.primary_streams == expected.primary;
    coverage.auxiliary_complete = coverage.auxiliary_streams == expected.auxiliary;
}

}  // namespace

DriverResult run_driver(const StreamInput* inputs, std::size_t count,
                        const SelectedPlanRequest& request, const Limits& limits,
                        const ObserverTable& observers, MaterializerSink sink,
                        void* sink_context) {
    DriverResult result;
    if (!any_requested(limits)) {
        fail(result, DriverStatus::NotAdmitted, DriverReason::NoRequestedFields,
             "No selected-capacity fields were requested; use the legacy route");
        return result;
    }
    if (!admit_observers(result, limits, observers)) return result;
    InputCounts expected;
    if (!admit_inputs(result, inputs, count, limits, sink, expected)) return result;
    Clock clock;
    if (limits.script_bars.present && !admit_clock(result, request, clock)) return result;
    result.coverage.auxiliary_complete = expected.auxiliary == 0;

    bool has_previous_primary = false;
    std::int64_t previous_primary = 0;
    std::int64_t previous_primary_bucket = 0;
    for (std::size_t i = 0; i < count; ++i) {
        result.input_index = i;
        result.role = inputs[i].role;
        result.row_index = 0;
        CsvReader reader(*inputs[i].stream);
        const CsvRead header = reader.read_header();
        if (!read_ok(result, header)) return result;
        {
            State tentative = result.state;
            DriverCoverage coverage = result.coverage;
            charge_bytes(coverage, result.role, header.csv_bytes);
            if (limits.bytes.present) observers.bytes(tentative, limits, header.csv_bytes);
            result.state = tentative;
            result.coverage = coverage;
        }
        bool has_previous = false;
        std::int64_t previous = 0;
        std::uint64_t rows = 0;
        for (;;) {
            const CsvRead record = reader.next();
            if (record.status == CsvStatus::End) break;
            result.row_index = rows;
            add(result.row_index, 1);  // one-based record index; header is zero
            if (!read_ok(result, record)) return result;
            const Bar& bar = record.bar;
            const bool primary = result.role == Role::Primary;
            const bool ordered = (!has_previous || bar.timestamp > previous)
                && (!primary || !has_previous_primary || bar.timestamp > previous_primary);
            if (!ordered) {
                fail(result, DriverStatus::InputError, DriverReason::RowsUnordered,
                     "CSV timestamps must increase strictly within a stream and across Primary streams");
                result.option = "rows";
                result.value_ms = bar.timestamp;
                result.previous_boundary_ms = primary && has_previous_primary
                    ? previous_primary : previous;
                if (limits.script_bars.present && primary) {
                    result.plan_status = SelectedPlanStatus::RowsUnordered;
                }
                return result;
            }
            RecordEvent event;
            event.role = result.role;
            event.timestamp_ms = bar.timestamp;
            event.bar = &bar;
            if (primary && limits.script_bars.present
                && !resolve_primary(result, request, clock, bar, event)) {
                return result;
            }
            if (event.has_script_bucket && has_previous_primary
                && event.script_bucket_key == previous_primary_bucket
                && previous_primary < request.start_ms && bar.timestamp >= request.start_ms) {
                // Also cover a FeedTolerant raw partition whose input open
                // would otherwise put supplied rows on both sides of T.
                fail(result, DriverStatus::CalendarError, DriverReason::CalendarUnsupported,
                     "A supplied Primary bucket crosses the selected start");
                result.plan_status = SelectedPlanStatus::CalendarUnsupported;
                result.option = "start_ms";
                result.bound = 0;
                result.value_ms = request.start_ms;
                result.previous_boundary_ms = event.script_bucket_key;
                result.next_boundary_ms = event.script_bucket_key;
                return result;
            }

            State tentative = result.state;
            DriverCoverage coverage = result.coverage;
            charge_bytes(coverage, event.role, record.csv_bytes);
            charge_record(coverage, event);
            if (limits.bytes.present) observers.bytes(tentative, limits, record.csv_bytes);
            observe_record(tentative, limits, observers, event);
            // The identical parsed Bar whose timestamp populated event reaches
            // the sink only after every selected reservation succeeds.
            if (primary) sink(sink_context, bar);
            result.state = tentative;
            result.coverage = coverage;
            has_previous = true;
            previous = bar.timestamp;
            if (primary) {
                has_previous_primary = true;
                previous_primary = bar.timestamp;
                if (event.has_script_bucket) previous_primary_bucket = event.script_bucket_key;
            }
            rows = result.row_index;
        }
        complete_stream(result.coverage, result.role, expected);
    }
    if (result.coverage.primary_rows == 0) {
        result.role = Role::Primary;
        result.input_index = std::numeric_limits<std::size_t>::max();
        result.row_index = 0;
        fail(result, DriverStatus::InputError, DriverReason::NoData,
             "Primary CSV contains no data records");
        return result;
    }
    result.status = DriverStatus::Completed;
    result.reason = DriverReason::None;
    result.input_index = std::numeric_limits<std::size_t>::max();
    result.row_index = 0;
    return result;
}

}  // namespace pineforge::detail::selected_capacity
