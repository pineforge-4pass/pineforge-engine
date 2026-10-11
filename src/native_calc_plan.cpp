#include "native_calc_plan.hpp"
#include "native_execution_consumer.hpp"

#include <pineforge/native_calendar.hpp>

#include <limits>
#include <utility>

namespace pineforge::native_calc {
namespace {

constexpr std::uint64_t kMaxOrdinal = std::numeric_limits<std::uint64_t>::max();

// FNV-1a's offset and prime, one word at a time: a bijection of the
// accumulator for a fixed word and of the word for a fixed accumulator, so two
// folds that differ in a single word never share a result.
constexpr std::uint64_t kFoldSeed = 14695981039346656037ULL;
constexpr std::uint64_t kFoldPrime = 1099511628211ULL;
constexpr std::uint64_t kGroupTag = 0x43414c43504c414eULL;

constexpr std::uint64_t fold_word(std::uint64_t accumulator, std::uint64_t word) noexcept {
    accumulator ^= word;
    accumulator *= kFoldPrime;
    accumulator ^= accumulator >> 29;
    return accumulator;
}

}  // namespace

Plan plan_schedule(const State& before, std::uint32_t cadence, const Event* events,
                   std::size_t count, const BarClock& clock, bool want_points) {
    Plan plan;
    plan.after = before;
    const auto refused = [&](Status why) {
        plan.status = why;
        plan.count = 0;
        plan.after = before;
        plan.points.clear();
        return plan;
    };
    if ((cadence & ~kCadenceMask) != 0 || (cadence & kCadenceBarClose) == 0) {
        return refused(Status::BadCadence);
    }
    if (count != 0 && events == nullptr) return refused(Status::BadArguments);

    State s = before;
    std::uint64_t planned = 0;
    std::vector<Point> rows;

    for (std::size_t index = 0; index < count; ++index) {
        const Event& event = events[index];
        if (!is_known(event.kind)) return refused(Status::UnknownEventKind);
        if (s.trace_first == kMaxOrdinal) return refused(Status::TraceExhausted);
        const std::int64_t at = event.time_ms;
        const std::uint64_t ordinal = s.trace_first;
        std::uint64_t calc = 0;
        Status failure = Status::Ok;

        // One calculation of this event, checked against the window of its
        // driving event: a Pre position lies between the previous event's time
        // (unbounded below for the first event) and this one's, a Post position
        // is this event's time.
        const auto emit = [&](Phase phase, std::int64_t position_ms, std::uint64_t key,
                              std::uint64_t in_bar, Finality finality) -> bool {
            const bool inside = phase == Phase::Post
                ? position_ms == at
                : (!s.has_prev_event || s.prev_event_ms <= position_ms) && position_ms <= at;
            if (!inside) {
                failure = Status::PositionOutsideWindow;
                return false;
            }
            if (want_points) {
                Point point;
                point.event_index = index;
                point.event = ordinal;
                point.bar_key = key;
                point.calc_ordinal = in_bar;
                point.calc = calc;
                point.at_ms = position_ms;
                point.phase = phase;
                point.finality = finality;
                rows.push_back(point);
            }
            ++planned;
            ++calc;
            return true;
        };

        // A bar lifecycle opens at the event: the next ordinal, in the interval
        // the timebase holds at the event's time, whose end must come after it.
        const auto open_bar = [&]() -> bool {
            if (s.has_last_key && s.last_bar_key == kMaxOrdinal) {
                failure = Status::BarKeyExhausted;
                return false;
            }
            Slot slot;
            if (!clock(at, slot)) {
                failure = Status::NoBarSlot;
                return false;
            }
            if (slot.end_ms <= at) {
                failure = Status::BarEndNotAfterEvent;
                return false;
            }
            s.last_bar_key = s.has_last_key ? s.last_bar_key + 1 : 0;
            s.has_last_key = true;
            s.bar_open = true;
            s.open_end_ms = slot.end_ms;
            s.cursor_ms = slot.next_open_ms;
            s.calc_in_bar = 0;
            return true;
        };

        // Time reaching the end of the open bar closes it before the event's own
        // payload is applied, and ends its lifecycle: whatever the event does
        // next belongs to a bar opened after it.
        if (s.bar_open && at >= s.open_end_ms) {
            if (!emit(Phase::Pre, s.open_end_ms, s.last_bar_key, s.calc_in_bar,
                      Finality::Confirmed)) {
                return refused(failure);
            }
            s.bar_open = false;
            s.open_end_ms = 0;
            s.calc_in_bar = 0;
        }

        // Quiet intervals the event passes over: each one that ends by the event
        // and that the timebase says yields a bar is a bar lifecycle of its own,
        // opened and closed by time alone. Closed time yields none.
        while (s.has_last_key && !s.bar_open) {
            Slot slot;
            if (!clock(s.cursor_ms, slot)) break;
            if (slot.end_ms > at || slot.next_open_ms <= s.cursor_ms) break;
            if (slot.tradable) {
                if (s.last_bar_key == kMaxOrdinal) return refused(Status::BarKeyExhausted);
                ++s.last_bar_key;
                if (!emit(Phase::Pre, slot.end_ms, s.last_bar_key, 0, Finality::Confirmed)) {
                    return refused(failure);
                }
            }
            s.cursor_ms = slot.next_open_ms;
        }

        switch (event.kind) {
        case EventKind::Tick:
            if (!s.bar_open && !open_bar()) return refused(failure);
            if ((cadence & kCadenceTick) != 0) {
                if (!emit(Phase::Post, at, s.last_bar_key, s.calc_in_bar,
                          Finality::Provisional)) {
                    return refused(failure);
                }
                ++s.calc_in_bar;
            }
            break;
        case EventKind::WarmupBar:
        case EventKind::Bar:
            if (!s.bar_open && !open_bar()) return refused(failure);
            if (!emit(Phase::Post, at, s.last_bar_key, s.calc_in_bar, Finality::Confirmed)) {
                return refused(failure);
            }
            s.bar_open = false;
            s.open_end_ms = 0;
            s.calc_in_bar = 0;
            break;
        case EventKind::AdvanceTime:
            if (s.bar_open && (cadence & kCadenceAdvanceTime) != 0) {
                if (!emit(Phase::Post, at, s.last_bar_key, s.calc_in_bar,
                          Finality::Provisional)) {
                    return refused(failure);
                }
                ++s.calc_in_bar;
            }
            break;
        }

        s.has_prev_event = true;
        s.prev_event_ms = at;
        ++s.trace_first;
    }

    plan.status = Status::Ok;
    plan.count = planned;
    plan.after = s;
    plan.points = std::move(rows);
    return plan;
}

std::uint64_t digest(const State& state) noexcept {
    std::uint64_t accumulator = kFoldSeed;
    std::apply(
        [&accumulator](const auto&... field) {
            ((accumulator = fold_word(accumulator, static_cast<std::uint64_t>(field))), ...);
        },
        state.tie());
    return accumulator;
}

std::uint64_t combine(std::uint64_t legacy, std::uint64_t group) noexcept {
    return fold_word(fold_word(legacy, kGroupTag), group);
}

}  // namespace pineforge::native_calc

namespace pineforge {
inline namespace engine_script_run_v19 {

// The consumer's schedule group. It is opt-in and per run: the group belongs to
// the run generation it was enabled in (the high-water the next begin must
// exceed), so a begin that follows leaves it disengaged without any reset in
// the begin path, and a consumer that never enables it keeps its continuation
// identity exactly.

void NativeExecutionConsumer::calc_group_enable() noexcept {
    calc_group_.emplace();
    calc_group_->run_generation = consumed_high_water_;
}

bool NativeExecutionConsumer::calc_group_engaged() const noexcept {
    return calc_group_.has_value() && calc_group_->run_generation == consumed_high_water_;
}

// The script timeframe's interval at an instant, read from the calendar the
// sealing path reads: its next period open is the exclusive end that path
// compares time against. A raw-label partition has no time boundaries, so no
// slot (its degenerate interval would end where it began).
bool NativeExecutionConsumer::calc_slot_at(std::int64_t at_ms, native_calc::Slot& slot) const {
    if (!script_tf_.valid() || uses_raw_label_partition()) return false;
    const auto interval = native_calendar::interval_containing(
        calendar_, script_tf_, day_label_origin(at_ms), calendar_memo_);
    if (!interval) return false;
    slot.end_ms = interval->next_period_open_ms;
    slot.next_open_ms = interval->next_input_open_ms;
    slot.tradable = interval->last_traded_close_ms > interval->eligible_open_ms
        && native_calendar::in_session(calendar_, interval->eligible_open_ms, calendar_memo_);
    return true;
}

native_calc::Plan NativeExecutionConsumer::calc_schedule_plan(
        std::uint32_t cadence, const native_calc::Event* events, std::size_t count,
        bool want_points) const {
    if (!calc_group_engaged()) {
        native_calc::Plan refused;
        refused.status = native_calc::Status::NotEngaged;
        return refused;
    }
    const native_calc::BarClock clock = [this](std::int64_t at_ms, native_calc::Slot& slot) {
        return calc_slot_at(at_ms, slot);
    };
    return native_calc::plan_schedule(calc_group_->state, cadence, events, count, clock,
                                      want_points);
}

native_calc::Plan NativeExecutionConsumer::calc_schedule_apply(
        std::uint32_t cadence, const native_calc::Event* events, std::size_t count) {
    native_calc::Plan plan = calc_schedule_plan(cadence, events, count, true);
    if (plan.status == native_calc::Status::Ok) calc_group_->state = plan.after;
    return plan;
}

std::uint64_t NativeExecutionConsumer::calc_continuation_hash() const noexcept {
    const std::uint64_t legacy = continuation_hash();
    if (!calc_group_engaged()) return legacy;
    return native_calc::combine(legacy, native_calc::digest(calc_group_->state));
}

}  // inline namespace engine_script_run_v19
}  // namespace pineforge
