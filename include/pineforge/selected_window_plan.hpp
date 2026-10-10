#pragma once

#include <pineforge/bar.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The selected primary planner: from the supplied primary rows and one
// selected-window request, which rows a selected run keeps and what the native
// consumer will execute over them. Pure and read-only: it constructs no
// strategy or engine, runs no host callback, hashes nothing, opens no feed or
// artifact, and reads only the rows' timestamps. The caller owns the row array;
// the result owns its values. An allocation failure (std::bad_alloc)
// propagates; any other exception of the native calendar becomes a status,
// never a guessed value.
//
// Grouping is the consumer's own (NativeExecutionConsumer for a batch run): the
// input interval at the row's timestamp, then the script interval at that input
// interval's open, both from native_calendar and its session-day memo. A
// group's key and its published label are both the script interval's open_ms
// and are held separately. A FeedTolerant request on a Passthrough pairing
// groups on each row's own timestamp; otherwise a failed interval lookup falls
// back to that raw partition under FeedTolerant and is a refusal without it. The
// groups a run executes follow the consumer's three seal rules: an input that
// reaches the script interval's next_period_open, the first input of a later
// group, and, at the end of the batch, a final input that reaches the script
// interval's last_traded_close. A final window group none of them seals is
// never executed and is not counted.
//
// Admission order: the script token (one of the first build's eight intraday
// primary tokens) and an explicit input timeframe; T/E/F as signed 53-bit
// integers with T < E and F <= T; preroll_bars <= 5000; the row array; the
// conservative clock identity (after the empty default only the exact spellings
// UTC, GMT, Etc/UTC and Etc/GMT fold together, every other zone string is
// compared exactly, and the one session string serves both clocks); the native
// session, timezone and timeframe grammar and the pairing;
// T, E and F each equal to the nominal open of the native script interval that
// holds them, independent of the rows; then the rows (strictly increasing, each
// F <= timestamp < E). Nothing snaps a boundary.
//
// On a refusal only status, option, bound, value_ms, previous_boundary_ms and
// next_boundary_ms are set; every count is zero. `option` names the faulty
// request member:
//   RequestInvalid       script_tf, input_tf, start_ms, end_ms, fed_start_ms,
//                        preroll_bars (value_ms = the count), rows. A bound
//                        carries bound 0/1/2 and value_ms; end_ms <= start_ms
//                        carries previous_boundary_ms = T, fed_start_ms > T
//                        carries next_boundary_ms = T.
//   CalendarUnsupported  timezone (the clock identities differ), engine_timezone
//                        (grammar), session (grammar), calendar (a native
//                        calendar exception), timeframes (a pairing the
//                        consumer does not group), start_ms/end_ms/fed_start_ms
//                        (no native interval holds the boundary), rows (no input
//                        or script interval holds a row, value_ms = its
//                        timestamp), and start_ms with bound 0 when one group
//                        would hold rows on both sides of T (previous_boundary_ms
//                        and next_boundary_ms = that group's open and next open).
//   BoundaryUnaligned    start_ms, end_ms or fed_start_ms with bound 0, 1 or 2,
//                        value_ms = the boundary, previous_boundary_ms and
//                        next_boundary_ms = the open and next_period_open of the
//                        native script interval that holds it.
//   FeedRangeInvalid     rows; bound 2 below F, bound 1 at or after E, value_ms =
//                        the row's timestamp, previous_boundary_ms = F,
//                        next_boundary_ms = E.
//   RowsUnordered        rows; value_ms = the row's timestamp,
//                        previous_boundary_ms = the row before it.
//   InternalError        a native exception outside the admission step, or a
//                        state the plan's own two passes disagree on.
//
// The plan is a prediction, not a count: the actual executed counts are the
// consumer's observed counters, which must later equal it. A pending PRE-T
// group that nothing seals is completed once at the accepted T from its own
// retained aggregate (the consumer integration, a separate change, must do that
// without synthetic prices, orders or fills and before capture);
// complete_pending_preroll_at_horizon says it is needed. An OFF run never
// uses this planner.

namespace pineforge {
inline namespace selected_window_plan_v1 {

enum class SelectedPlanStatus { Ok, RequestInvalid, CalendarUnsupported,
    BoundaryUnaligned, FeedRangeInvalid, RowsUnordered, InternalError };
struct SelectedPlanRequest {
    std::int64_t start_ms, end_ms, fed_start_ms;
    std::uint32_t preroll_bars;
    std::string input_tf, script_tf, chart_timezone, engine_timezone, session;
    bool feed_tolerant = true;
};
struct SelectedPrimaryPlan {
    SelectedPlanStatus status = SelectedPlanStatus::InternalError;
    std::string option;
    int bound = -1; // 0=T, 1=E, 2=F, -1=no boundary detail
    std::int64_t value_ms = 0, previous_boundary_ms = 0, next_boundary_ms = 0;
    std::uint64_t supplied_input_bars = 0, supplied_script_bars = 0;
    std::uint64_t available_script_bars = 0, used_script_bars = 0;
    std::uint64_t trimmed_script_bars = 0, trim_index = 0;
    std::uint64_t fed_input_bars = 0, fed_script_bars = 0;
    std::uint64_t preroll_input_bars = 0, window_input_bars = 0, window_script_bars = 0;
    std::int64_t trim_start_ms = 0;
    std::optional<std::int64_t> preroll_first_bar_ms, preroll_last_bar_ms;
    std::optional<std::int64_t> supplied_first_data_ms, supplied_last_data_ms;
    std::optional<std::int64_t> fed_first_data_ms, fed_last_data_ms;
    std::optional<std::int64_t> window_first_data_ms, window_last_data_ms;
    bool shortfall = false;
    bool complete_pending_preroll_at_horizon = false;
};
SelectedPrimaryPlan plan_selected_primary(const Bar* rows, std::size_t count,
                                         const SelectedPlanRequest& request);

// The retained primary count, for the source layer's tail expectations: over
// rows the caller has ALREADY retained (any pre-roll trimming happened before
// them), the number of script bars a selected batch run executes. It is
// plan_selected_primary's fed_script_bars for a feed that plan would not trim
// further, taken from the same clock admission, boundary check and consumer
// bucket walk (no second calendar), with no F boundary, no pre-roll request and
// no 5000 cap: a native caller's retained prehistory is its own. Every group
// that opens before T is counted (each is executed, a last pending one by the
// completion at the accepted T) together with every window group the consumer's
// seal rules execute; an unsealed window tail adds none. Pure and read-only like
// the planner: it constructs no strategy or consumer and reads only timestamps.
//
// Admission order: the script token (one of the eight intraday primary tokens)
// and an explicit input timeframe; T and E as signed 53-bit integers with
// T < E; the row array; the conservative clock identity, the native session,
// timezone and timeframe grammar and the pairing, exactly as the planner
// admits them; T and E each equal to the nominal open of the native script
// interval that holds them; then the rows (strictly increasing, each < E).
// Nothing snaps a boundary. A refusal sets `status` and `option` with the
// planner's names for the same fault (script_tf, input_tf, start_ms, end_ms,
// rows, timezone, engine_timezone, session, calendar, timeframes) and leaves
// script_bars zero; a group that would hold rows on both sides of T is
// CalendarUnsupported on start_ms. Allocation failure behaves as in the
// planner: std::bad_alloc propagates and any other native calendar exception
// becomes a status.
struct SelectedRetainedRequest {
    std::int64_t start_ms, end_ms;
    std::string input_tf, script_tf, chart_timezone, engine_timezone, session;
    bool feed_tolerant = true;
};
struct SelectedRetainedCount {
    SelectedPlanStatus status = SelectedPlanStatus::InternalError;
    std::string option;
    std::uint64_t script_bars = 0;
};
SelectedRetainedCount count_selected_retained_primary(const Bar* rows, std::size_t count,
                                                      const SelectedRetainedRequest& request);

// The script bars an admitted selected batch run executes over its retained rows:
// count_selected_retained_primary with E unchecked and unbounded. The selected
// window setter admits any T < E, the consumer executes the same groups whatever E
// is, and the count above refuses only on E's own alignment or a row at or after
// it, so a run reads the one answer every such E gives. T, the clock and the rows
// are admitted exactly as above.
SelectedRetainedCount count_selected_executed_primary(const Bar* rows, std::size_t count,
                                                      const SelectedRetainedRequest& request);

}  // inline namespace selected_window_plan_v1
}  // namespace pineforge
