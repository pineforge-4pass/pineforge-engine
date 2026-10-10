// The selected primary planner (include/pineforge/selected_window_plan.hpp
// holds the contract). This file states how the two passes mirror the native
// consumer.
//
// Pass 1 walks the rows once. Per row it checks order and range, resolves the
// row's input and script intervals with the native memo (one calendar, one
// pairing, no per-row calendar or engine), and replays the consumer's script
// bucket (BucketWalk) to count the groups that open before T, the window
// callbacks a batch run will execute, and whether a pre-T group is still
// pending at the end. Pass 2 runs only when some pre-roll groups are trimmed
// away while others are kept: it replays the same walk, from a fresh memo and
// so the same answers, up to the first row of the first kept group. Auxiliary
// storage is a fixed set of scalars, the parsed calendar and the memo's bounded
// tables; no row, bar or interval is copied or kept.
//
// The retained count (count_selected_retained_primary) is pass 1 alone over rows
// the caller has already retained: the same clock admission (admit_clock), the
// same boundary check and the same row walk (walk_rows), with no F boundary, no
// pre-roll request and no trimming. Its answer is the groups that opened before
// T plus the window groups the walk sealed, so for the rows a plan keeps it is
// that plan's fed_script_bars.

#include <pineforge/selected_window_plan.hpp>

#include <pineforge/native_calendar.hpp>

#include "native_calendar_memo.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pineforge {
inline namespace selected_window_plan_v1 {
namespace {

namespace nc = native_calendar;

constexpr std::int64_t kI53Max = (std::int64_t{1} << 53) - 1;
constexpr std::uint32_t kMaxPrerollBars = 5000;
// The first build's eight intraday primary tokens, exactly as a request spells
// them: no alias, no second token, no daily or weekly token.
constexpr const char* kPrimaryTokens[] = {"1", "3", "5", "15", "30", "60", "120", "240"};
constexpr const char* kBoundOptions[] = {"start_ms", "end_ms", "fed_start_ms"};

bool primary_token(const std::string& token) {
    for (const char* candidate : kPrimaryTokens) {
        if (token == candidate) return true;
    }
    return false;
}

bool in_i53(std::int64_t value) noexcept { return value >= -kI53Max && value <= kI53Max; }

// The conservative clock identity: after the empty default only the exact spellings
// UTC, GMT, Etc/UTC and Etc/GMT are one token; every other zone string is its own
// token, compared exactly (no trim, no case folding, no alias inference).
std::string_view clock_token(const std::string& timezone) noexcept {
    if (timezone.empty() || timezone == "UTC" || timezone == "GMT" || timezone == "Etc/UTC"
        || timezone == "Etc/GMT") {
        return "UTC";
    }
    return timezone;
}

SelectedPrimaryPlan refusal(SelectedPlanStatus status, const char* option, int bound = -1,
                            std::int64_t value_ms = 0, std::int64_t previous_boundary_ms = 0,
                            std::int64_t next_boundary_ms = 0) {
    SelectedPrimaryPlan plan;
    plan.status = status;
    plan.option = option;
    plan.bound = bound;
    plan.value_ms = value_ms;
    plan.previous_boundary_ms = previous_boundary_ms;
    plan.next_boundary_ms = next_boundary_ms;
    return plan;
}

// The one calendar/input/script pairing the rows are grouped under.
struct Clock {
    nc::SessionCalendar calendar;
    nc::Timeframe input_tf;
    nc::Timeframe script_tf;
    bool passthrough = false;
    bool tolerant = true;
};

// The clock identity, then the native session, timezone and timeframe grammar
// and the pairing. Both entries' requests (SelectedPlanRequest and
// SelectedRetainedRequest) carry the same six members this reads. Returns a
// refusal, or nothing with `clock` filled in. std::bad_alloc propagates; any
// other exception is contained as a named status, never a guessed value.
template <class Request>
std::optional<SelectedPrimaryPlan> admit_clock(const Request& request, Clock& clock) {
    if (clock_token(request.chart_timezone) != clock_token(request.engine_timezone)) {
        return refusal(SelectedPlanStatus::CalendarUnsupported, "timezone");
    }
    // The adapter's spec builder: a missing or empty syminfo timezone is UTC, a
    // missing or empty session is 24x7. The session string is shared by the
    // alignment and the aggregation clock.
    const std::string zone_name =
        request.engine_timezone.empty() ? std::string("UTC") : request.engine_timezone;
    const std::string session_text =
        request.session.empty() ? std::string("24x7") : request.session;
    try {
        if (!nc::timezone_accepted(zone_name)) {
            return refusal(SelectedPlanStatus::CalendarUnsupported, "engine_timezone");
        }
        std::optional<nc::SessionCalendar> calendar = nc::parse_session(session_text, zone_name);
        if (!calendar) return refusal(SelectedPlanStatus::CalendarUnsupported, "session");
        std::optional<nc::Timeframe> input_tf = nc::parse_timeframe(request.input_tf);
        if (!input_tf) return refusal(SelectedPlanStatus::RequestInvalid, "input_tf");
        std::optional<nc::Timeframe> script_tf = nc::parse_timeframe(request.script_tf);
        if (!script_tf) return refusal(SelectedPlanStatus::InternalError, "script_tf");
        const nc::TimeframeCompatibility pairing = nc::compatibility(*input_tf, *script_tf);
        const bool passthrough = pairing.pairing == nc::TimeframePairing::Passthrough;
        if (!passthrough && !nc::pairing_aggregates(pairing)) {
            return refusal(SelectedPlanStatus::CalendarUnsupported, "timeframes");
        }
        clock.calendar = std::move(*calendar);
        clock.input_tf = std::move(*input_tf);
        clock.script_tf = std::move(*script_tf);
        clock.passthrough = passthrough;
        clock.tolerant = request.feed_tolerant;
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return refusal(SelectedPlanStatus::CalendarUnsupported, "calendar");
    }
}

// The consumer's timestamp_partition: no duration is known, every boundary is
// the row's own timestamp.
nc::NativeInterval raw_partition(std::int64_t timestamp) noexcept {
    return nc::NativeInterval{timestamp, timestamp, timestamp, timestamp, timestamp};
}

// The consumer's grouping for one row: input_interval_at(timestamp), then
// script_interval_at at that input interval's open. FeedTolerant on a
// Passthrough pairing is the raw partition for both; otherwise a failed lookup
// falls back to the same raw partition under FeedTolerant and is a failure
// without it.
bool resolve_row(const Clock& clock, nc::SessionDayMemo& memo, std::int64_t timestamp,
                 nc::NativeInterval& input, nc::NativeInterval& script) {
    if (clock.tolerant && clock.passthrough) {
        input = raw_partition(timestamp);
        script = raw_partition(input.open_ms);
        return true;
    }
    std::optional<nc::NativeInterval> held =
        nc::interval_containing(clock.calendar, clock.input_tf, timestamp, memo);
    if (!held && clock.tolerant) held = raw_partition(timestamp);
    if (!held) return false;
    input = *held;
    held = nc::interval_containing(clock.calendar, clock.script_tf, input.open_ms, memo);
    if (!held && clock.tolerant) held = raw_partition(input.open_ms);
    if (!held) return false;
    script = *held;
    return true;
}

// A boundary (bound 0 = T, 1 = E, 2 = F) is the nominal open of the pure native
// script interval that holds it, independent of the rows. Returns the refusal,
// or nothing when it is aligned. Nothing snaps.
std::optional<SelectedPrimaryPlan> check_boundary(const Clock& clock, nc::SessionDayMemo& memo,
                                                  int bound, std::int64_t at) {
    const std::optional<nc::NativeInterval> held =
        nc::interval_containing(clock.calendar, clock.script_tf, at, memo);
    if (!held) {
        return refusal(SelectedPlanStatus::CalendarUnsupported, kBoundOptions[bound], bound, at);
    }
    if (held->open_ms != at) {
        return refusal(SelectedPlanStatus::BoundaryUnaligned, kBoundOptions[bound], bound, at,
                       held->open_ms, held->next_period_open_ms);
    }
    return std::nullopt;
}

// The consumer's script bucket, replayed without its OHLC aggregate: the same
// has-data / key / interval / last-accepted-input state that contribute_input,
// seal_stale_script, seal_script and pump_batch's closing rule read, so the
// groups it opens and the callbacks it seals are the ones a batch run
// delivers. A group's key (what a later row is compared with) and label (what
// the sealed bar is published under) are separate even though both are the
// script interval's open_ms here. "Before" means the group's first row lies
// before T.
class BucketWalk {
public:
    explicit BucketWalk(std::int64_t start_ms) noexcept : start_ms_(start_ms) {}

    struct Step {
        bool opened = false;     // this row opened a group
        bool straddled = false;  // one group would hold rows on both sides of T
        std::int64_t straddle_open_ms = 0;
        std::int64_t straddle_next_ms = 0;
    };

    Step feed(std::int64_t timestamp, const nc::NativeInterval& input,
              const nc::NativeInterval& script) noexcept {
        Step step;
        const std::int64_t script_key = script.open_ms;
        // seal_stale_script: the first input of a later group seals a pending
        // one (LazyComplete) before it contributes.
        if (open_ && key_ != script_key) seal();
        if (!open_) {
            // On aligned admitted clocks no script interval holds T strictly
            // inside; one that does is not silently reassigned.
            if (script.open_ms < start_ms_ && start_ms_ < script.next_period_open_ms) {
                step.straddled = true;
                step.straddle_open_ms = script.open_ms;
                step.straddle_next_ms = script.next_period_open_ms;
                return step;
            }
            open_ = true;
            before_ = timestamp < start_ms_;
            key_ = script_key;
            label_ = script.open_ms;
            eligible_open_ms_ = script.eligible_open_ms;
            last_traded_close_ms_ = script.last_traded_close_ms;
            next_period_open_ms_ = script.next_period_open_ms;
            if (before_) {
                if (opened_before_ == 0) {
                    first_key_ = key_;
                    first_label_ = label_;
                }
                ++opened_before_;
                last_before_label_ = label_;
            }
            opened_key_ = key_;
            opened_label_ = label_;
            step.opened = true;
        } else if (before_ && timestamp >= start_ms_) {
            step.straddled = true;
            step.straddle_open_ms = label_;
            step.straddle_next_ms = next_period_open_ms_;
            return step;
        }
        input_last_traded_close_ms_ = input.last_traded_close_ms;
        // contribute_input: an input that reaches the script interval's next
        // period open seals the group (Confirmed).
        if (input.next_period_open_ms >= next_period_open_ms_) seal();
        return step;
    }

    // pump_batch's closing rule (final_script_session_closed): the final input
    // reaches the pending group's last traded close. Any other pending group is
    // never sealed.
    void finish() noexcept {
        if (open_ && last_traded_close_ms_ > eligible_open_ms_
            && input_last_traded_close_ms_ >= last_traded_close_ms_) {
            seal();
        }
    }

    std::uint64_t opened_before() const noexcept { return opened_before_; }
    std::uint64_t sealed_window() const noexcept { return sealed_window_; }
    bool pending_before() const noexcept { return open_ && before_; }
    std::int64_t first_key() const noexcept { return first_key_; }
    std::int64_t first_label() const noexcept { return first_label_; }
    std::int64_t last_before_label() const noexcept { return last_before_label_; }
    std::int64_t opened_key() const noexcept { return opened_key_; }
    std::int64_t opened_label() const noexcept { return opened_label_; }

private:
    void seal() noexcept {
        if (!before_) ++sealed_window_;
        open_ = false;
    }

    std::int64_t start_ms_;
    bool open_ = false;    // the consumer's script_.has_data && !script_.sealed
    bool before_ = false;  // the pending group's first row lies before T
    std::int64_t key_ = 0;
    std::int64_t label_ = 0;
    std::int64_t eligible_open_ms_ = 0;
    std::int64_t last_traded_close_ms_ = 0;
    std::int64_t next_period_open_ms_ = 0;
    std::int64_t input_last_traded_close_ms_ = 0;  // last_accepted_input_
    std::uint64_t opened_before_ = 0;
    std::uint64_t sealed_window_ = 0;
    std::int64_t first_key_ = 0;
    std::int64_t first_label_ = 0;
    std::int64_t last_before_label_ = 0;
    std::int64_t opened_key_ = 0;
    std::int64_t opened_label_ = 0;
};

// Pass 1, the one row walk the planner and the retained count share: each row's
// order and range, its input and script intervals, and the consumer's script
// bucket (BucketWalk). `floor_ms` is the planner's F (a row below it is
// refused); the retained count has none. Returns a refusal, or nothing with the
// walk finished (pump_batch's closing rule applied) and `first_window_row` the
// index of the first row at or after T, or `count` when there is none.
std::optional<SelectedPrimaryPlan> walk_rows(const Bar* rows, std::size_t count,
                                             const Clock& clock, nc::SessionDayMemo& memo,
                                             BucketWalk& walk, std::optional<std::int64_t> floor_ms,
                                             std::int64_t start_ms, std::int64_t end_ms,
                                             std::size_t& first_window_row) {
    using Status = SelectedPlanStatus;
    first_window_row = count;  // count: no row at or after T
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t at = rows[i].timestamp;
        if (i != 0 && !(at > rows[i - 1].timestamp)) {
            return refusal(Status::RowsUnordered, "rows", -1, at, rows[i - 1].timestamp);
        }
        if (floor_ms && at < *floor_ms) {
            return refusal(Status::FeedRangeInvalid, "rows", 2, at, *floor_ms, end_ms);
        }
        if (at >= end_ms) {
            return refusal(Status::FeedRangeInvalid, "rows", 1, at, floor_ms.value_or(0), end_ms);
        }
        nc::NativeInterval input;
        nc::NativeInterval script;
        if (!resolve_row(clock, memo, at, input, script)) {
            return refusal(Status::CalendarUnsupported, "rows", -1, at);
        }
        const BucketWalk::Step step = walk.feed(at, input, script);
        if (step.straddled) {
            return refusal(Status::CalendarUnsupported, "start_ms", 0, start_ms,
                           step.straddle_open_ms, step.straddle_next_ms);
        }
        if (first_window_row == count && at >= start_ms) first_window_row = i;
    }
    walk.finish();
    return std::nullopt;
}

SelectedPrimaryPlan plan_rows(const Bar* rows, std::size_t count, const SelectedPlanRequest& request,
                              const Clock& clock) {
    using Status = SelectedPlanStatus;
    const std::int64_t start_ms = request.start_ms;
    const std::int64_t end_ms = request.end_ms;
    const std::int64_t fed_start_ms = request.fed_start_ms;

    nc::SessionDayMemo memo;

    // T, E and F are each the nominal open of the pure native script interval
    // that holds them, independent of the rows. Nothing snaps.
    const std::int64_t boundaries[3] = {start_ms, end_ms, fed_start_ms};
    for (int bound = 0; bound < 3; ++bound) {
        if (std::optional<SelectedPrimaryPlan> refused =
                check_boundary(clock, memo, bound, boundaries[bound])) {
            return std::move(*refused);
        }
    }

    // Pass 1. The memo starts fresh so pass 2 asks the same questions in the
    // same order and gets the same answers.
    memo.reset();
    BucketWalk walk(start_ms);
    std::size_t first_window_row = count;  // count: no row at or after T
    if (std::optional<SelectedPrimaryPlan> refused = walk_rows(
            rows, count, clock, memo, walk, fed_start_ms, start_ms, end_ms, first_window_row)) {
        return std::move(*refused);
    }

    const std::uint64_t available = walk.opened_before();
    const std::uint64_t used = std::min<std::uint64_t>(request.preroll_bars, available);
    const std::uint64_t trimmed = available - used;
    const std::uint64_t window_script = walk.sealed_window();

    // The first kept row: the first row of the first kept pre-T group, or with
    // no kept group the first row at or after T (trim_start is then T itself).
    std::size_t trim_index = first_window_row;
    std::int64_t trim_key = start_ms;
    std::int64_t trim_label = 0;
    if (used != 0 && trimmed == 0) {
        trim_index = 0;
        trim_key = walk.first_key();
        trim_label = walk.first_label();
    } else if (used != 0) {
        memo.reset();
        BucketWalk again(start_ms);
        bool found = false;
        for (std::size_t i = 0; i < count && !found; ++i) {
            const std::int64_t at = rows[i].timestamp;
            nc::NativeInterval input;
            nc::NativeInterval script;
            if (!resolve_row(clock, memo, at, input, script)) {
                return refusal(Status::InternalError, "rows", -1, at);
            }
            const BucketWalk::Step step = again.feed(at, input, script);
            if (step.straddled) return refusal(Status::InternalError, "rows", -1, at);
            if (step.opened && at < start_ms && again.opened_before() == trimmed + 1) {
                found = true;
                trim_index = i;
                trim_key = again.opened_key();
                trim_label = again.opened_label();
            }
        }
        if (!found) return refusal(Status::InternalError, "rows");
    }

    SelectedPrimaryPlan plan;
    plan.status = Status::Ok;
    plan.supplied_input_bars = count;
    plan.supplied_script_bars = available + window_script;
    plan.available_script_bars = available;
    plan.used_script_bars = used;
    plan.trimmed_script_bars = trimmed;
    plan.trim_index = trim_index;
    plan.fed_input_bars = count - trim_index;
    plan.fed_script_bars = used + window_script;
    plan.preroll_input_bars = first_window_row - trim_index;
    plan.window_input_bars = count - first_window_row;
    plan.window_script_bars = window_script;
    plan.trim_start_ms = used != 0 ? trim_key : start_ms;
    if (used != 0) {
        plan.preroll_first_bar_ms = trim_label;
        plan.preroll_last_bar_ms = walk.last_before_label();
    }
    if (count != 0) {
        plan.supplied_first_data_ms = rows[0].timestamp;
        plan.supplied_last_data_ms = rows[count - 1].timestamp;
    }
    if (trim_index < count) {
        plan.fed_first_data_ms = rows[trim_index].timestamp;
        plan.fed_last_data_ms = rows[count - 1].timestamp;
    }
    if (first_window_row < count) {
        plan.window_first_data_ms = rows[first_window_row].timestamp;
        plan.window_last_data_ms = rows[count - 1].timestamp;
    }
    plan.shortfall = available < request.preroll_bars;
    // A pre-T group nothing seals is completed once at T from its own retained
    // aggregate: only when it is a kept group.
    plan.complete_pending_preroll_at_horizon = used != 0 && walk.pending_before();
    return plan;
}

SelectedRetainedCount retained_refusal(SelectedPlanStatus status, const char* option) {
    SelectedRetainedCount refused;
    refused.status = status;
    refused.option = option;
    return refused;
}

// A refusal of the shared admission, boundary check or row walk, in the count's
// two-field form.
SelectedRetainedCount retained_refusal(const SelectedPrimaryPlan& refused_plan) {
    SelectedRetainedCount refused;
    refused.status = refused_plan.status;
    refused.option = refused_plan.option;
    return refused;
}

// The retained count over an admitted clock: T and E each aligned, then the one
// row walk with no F boundary. Every group that opened before T is executed (the
// last pending one by the completion at the accepted T), and so is every window
// group the walk sealed; an unsealed window tail is neither. E bounds none of
// those groups: the walk only refuses a row at or after it. With `unbounded_end`
// E is neither checked nor a bound, so the answer is the one any accepted E gives.
SelectedRetainedCount count_rows(const Bar* rows, std::size_t count,
                                 const SelectedRetainedRequest& request, const Clock& clock,
                                 bool unbounded_end) {
    nc::SessionDayMemo memo;

    const std::int64_t boundaries[2] = {request.start_ms, request.end_ms};
    for (int bound = 0; bound < (unbounded_end ? 1 : 2); ++bound) {
        if (std::optional<SelectedPrimaryPlan> refused =
                check_boundary(clock, memo, bound, boundaries[bound])) {
            return retained_refusal(*refused);
        }
    }

    memo.reset();
    BucketWalk walk(request.start_ms);
    std::size_t first_window_row = count;
    const std::int64_t end_ms =
        unbounded_end ? std::numeric_limits<std::int64_t>::max() : request.end_ms;
    if (std::optional<SelectedPrimaryPlan> refused =
            walk_rows(rows, count, clock, memo, walk, std::nullopt, request.start_ms,
                      end_ms, first_window_row)) {
        return retained_refusal(*refused);
    }

    SelectedRetainedCount counted;
    counted.status = SelectedPlanStatus::Ok;
    counted.script_bars = walk.opened_before() + walk.sealed_window();
    return counted;
}

}  // namespace

SelectedPrimaryPlan plan_selected_primary(const Bar* rows, std::size_t count,
                                         const SelectedPlanRequest& request) {
    using Status = SelectedPlanStatus;
    const std::int64_t start_ms = request.start_ms;
    const std::int64_t end_ms = request.end_ms;
    const std::int64_t fed_start_ms = request.fed_start_ms;

    if (!primary_token(request.script_tf)) return refusal(Status::RequestInvalid, "script_tf");
    if (request.input_tf.empty()) return refusal(Status::RequestInvalid, "input_tf");
    if (!in_i53(start_ms)) return refusal(Status::RequestInvalid, "start_ms", 0, start_ms);
    if (!in_i53(end_ms)) return refusal(Status::RequestInvalid, "end_ms", 1, end_ms);
    if (!in_i53(fed_start_ms)) {
        return refusal(Status::RequestInvalid, "fed_start_ms", 2, fed_start_ms);
    }
    if (!(start_ms < end_ms)) {
        return refusal(Status::RequestInvalid, "end_ms", 1, end_ms, start_ms);
    }
    if (!(fed_start_ms <= start_ms)) {
        return refusal(Status::RequestInvalid, "fed_start_ms", 2, fed_start_ms, 0, start_ms);
    }
    if (request.preroll_bars > kMaxPrerollBars) {
        return refusal(Status::RequestInvalid, "preroll_bars", -1,
                       static_cast<std::int64_t>(request.preroll_bars));
    }
    if ((rows == nullptr && count != 0)
        || static_cast<std::uint64_t>(count) > static_cast<std::uint64_t>(kI53Max)) {
        return refusal(Status::RequestInvalid, "rows");
    }

    // The clock identity is the first step of admit_clock.
    Clock clock;
    if (std::optional<SelectedPrimaryPlan> refused = admit_clock(request, clock)) {
        return std::move(*refused);
    }
    try {
        return plan_rows(rows, count, request, clock);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return refusal(Status::InternalError, "calendar");
    }
}

namespace {

// The two retained entries: the same request checks, clock admission and row walk,
// with E checked and bounding the rows unless `unbounded_end`.
SelectedRetainedCount count_retained(const Bar* rows, std::size_t count,
                                     const SelectedRetainedRequest& request, bool unbounded_end) {
    using Status = SelectedPlanStatus;

    if (!primary_token(request.script_tf)) {
        return retained_refusal(Status::RequestInvalid, "script_tf");
    }
    if (request.input_tf.empty()) return retained_refusal(Status::RequestInvalid, "input_tf");
    if (!in_i53(request.start_ms)) return retained_refusal(Status::RequestInvalid, "start_ms");
    if (!unbounded_end) {
        if (!in_i53(request.end_ms)) return retained_refusal(Status::RequestInvalid, "end_ms");
        if (!(request.start_ms < request.end_ms)) {
            return retained_refusal(Status::RequestInvalid, "end_ms");
        }
    }
    if ((rows == nullptr && count != 0)
        || static_cast<std::uint64_t>(count) > static_cast<std::uint64_t>(kI53Max)) {
        return retained_refusal(Status::RequestInvalid, "rows");
    }

    // The clock identity is the first step of admit_clock.
    Clock clock;
    if (std::optional<SelectedPrimaryPlan> refused = admit_clock(request, clock)) {
        return retained_refusal(*refused);
    }
    try {
        return count_rows(rows, count, request, clock, unbounded_end);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return retained_refusal(Status::InternalError, "calendar");
    }
}

}  // namespace

SelectedRetainedCount count_selected_retained_primary(const Bar* rows, std::size_t count,
                                                      const SelectedRetainedRequest& request) {
    return count_retained(rows, count, request, /*unbounded_end=*/false);
}

SelectedRetainedCount count_selected_executed_primary(const Bar* rows, std::size_t count,
                                                      const SelectedRetainedRequest& request) {
    return count_retained(rows, count, request, /*unbounded_end=*/true);
}

}  // inline namespace selected_window_plan_v1
}  // namespace pineforge
