#include <pineforge/native_order.hpp>

// The price-grid arithmetic an anchored level snaps with, and the one an
// activation is re-validated on, is the run's own L8 ladder arithmetic, so a
// rounded anchor, a reached print and a quantized fill agree tick for tick.
// The header is value-only geometry; it brings no host into the core.
#include "native_matching.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pineforge::native_order {
inline namespace native_order_v6 {
namespace {

constexpr std::uint8_t kLivePush = 1;
constexpr std::uint8_t kLiveErase = 2;
constexpr std::uint8_t kLiveUpdate = 3;
constexpr std::uint8_t kLiveErasePush = 4;

template <class T>
void reserve_n(std::vector<T>& values, std::size_t n) {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    if (n == 0) return;
    if (values.size() > values.max_size() - n) {
        throw std::length_error("native working-request capacity exhausted");
    }
    const std::size_t required = values.size() + n;
    if (required <= values.capacity()) return;
    std::size_t cap = values.capacity() == 0 ? 1 : values.capacity();
    while (cap < required) {
        if (cap > values.max_size() / 2) {
            cap = values.max_size();
            break;
        }
        cap *= 2;
    }
    values.reserve(std::max(required, cap));
}

uint64_t event_ordinal(const CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

bool finite_non_negative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool finite_nonzero(double value) noexcept {
    return std::isfinite(value) && value != 0.0;
}

const Flatten* as_flatten(const OrderIntent& intent) noexcept {
    return std::get_if<Flatten>(&intent);
}
const Reduce* as_reduce(const OrderIntent& intent) noexcept {
    return std::get_if<Reduce>(&intent);
}
const Transact* as_transact(const OrderIntent& intent) noexcept {
    return std::get_if<Transact>(&intent);
}
const ReverseTo* as_reverse_to(const OrderIntent& intent) noexcept {
    return std::get_if<ReverseTo>(&intent);
}
const HostSized* as_host_sized(const OrderIntent& intent) noexcept {
    return std::get_if<HostSized>(&intent);
}
const Sized* as_sized(const OrderIntent& intent) noexcept {
    return std::get_if<Sized>(&intent);
}

const ExplicitUnits* explicit_size(const Reduce& reduce) noexcept {
    return std::get_if<ExplicitUnits>(&reduce.size);
}

bool is_owner_opened(const Reduce& reduce) noexcept {
    return std::holds_alternative<OwnerOpenedUnits>(reduce.size);
}

const ScopeFraction* fraction_size(const Reduce& reduce) noexcept {
    return std::get_if<ScopeFraction>(&reduce.size);
}

// A scope fraction is the only reduction size whose units are unknown at the
// command boundary: like a host-sized close it stays deferred until the
// matching candidate resolves the bound scope.
const ScopeFraction* deferred_reduction(const OrderIntent& intent) noexcept {
    const auto* reduce = as_reduce(intent);
    return reduce ? fraction_size(*reduce) : nullptr;
}

bool basis_fraction_ok(double fraction) noexcept {
    return std::isfinite(fraction) && fraction > 0.0 && fraction <= 1.0;
}

double working_units(const Remaining& remaining) noexcept {
    if (const auto* units = std::get_if<RemainingUnits>(&remaining)) return units->q;
    return 0.0;
}

RemainingProjection project_remaining(const Remaining& remaining) {
    if (std::holds_alternative<RemainingUnbound>(remaining)) return RemainingProjectionUnbound{};
    if (std::holds_alternative<RemainingFlattenAll>(remaining)) {
        return RemainingProjectionFlattenAll{};
    }
    if (std::holds_alternative<RemainingDeferred>(remaining)) {
        return RemainingProjectionDeferred{};
    }
    if (std::holds_alternative<NoTarget>(remaining)) {
        return RemainingProjectionNoTarget{};
    }
    return RemainingProjectionUnits{working_units(remaining)};
}

bool checked_add_positive(double a, double b, double* out) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b) || a < 0.0 || b < 0.0) return false;
    const double sum = a + b;
    if (!std::isfinite(sum)) return false;
    if (a > 0.0 && b > 0.0 && !(sum > a && sum > b)) return false;
    *out = sum;
    return true;
}

bool checked_sub_cap(double before, double deduct, double* after, bool* exhausted) noexcept {
    if (!std::isfinite(before) || !std::isfinite(deduct) || before <= 0.0 || deduct <= 0.0) {
        return false;
    }
    if (deduct == before) {
        *after = 0.0;
        *exhausted = true;
        return true;
    }
    if (deduct > before) return false;
    const double next = before - deduct;
    if (!std::isfinite(next) || !(next > 0.0 && next < before)) return false;
    *after = next;
    *exhausted = false;
    return true;
}

TriggerState trigger_state_from(const Trigger& trigger) {
    if (std::holds_alternative<Market>(trigger)) return MarketReady{};
    if (std::holds_alternative<Limit>(trigger)) return LimitReady{};
    if (std::holds_alternative<Stop>(trigger)) return StopIdle{};
    if (std::holds_alternative<StopLimit>(trigger)) return StopLimitPending{};
    return TrailWaitArm{};
}

bool fillable_state(const TriggerState& state) noexcept {
    return std::holds_alternative<MarketReady>(state) || std::holds_alternative<LimitReady>(state)
        || std::holds_alternative<StopActive>(state) || std::holds_alternative<StopLimitLive>(state)
        || std::holds_alternative<TrailActive>(state);
}

Side side_from_opened(double opened_units) noexcept {
    return opened_units < 0.0 ? Side::Short : Side::Long;
}

bool position_matches(const PositionIdentity& position, int64_t cycle, Side side) noexcept {
    const auto* nonflat = std::get_if<PositionNonflat>(&position);
    return nonflat && nonflat->cycle == cycle && nonflat->side == side && cycle > 0;
}

bool opening_alive(const OpeningObservation& observation,
                   const RequestHandle& opening,
                   int64_t cycle,
                   const Side* required_side = nullptr) noexcept {
    if (observation.queried_opening != opening || observation.queried_cycle != cycle) return false;
    if (!observation.has_live_matching_lot || cycle <= 0) return false;
    const auto* nonflat = std::get_if<PositionNonflat>(&observation.current_position);
    if (!nonflat || nonflat->cycle != cycle) return false;
    if (required_side && nonflat->side != *required_side) return false;
    return true;
}

bool book_close_alive(const TargetObservation& observation, const BookClose& close) noexcept {
    return position_matches(observation.current_position, close.cycle, close.side);
}

bool opening_close_alive(const TargetObservation& observation, const OpeningClose& close) noexcept {
    if (!observation.opening) return false;
    if (!opening_alive(*observation.opening, close.opening, close.cycle, &close.side)) return false;
    return position_matches(observation.current_position, close.cycle, close.side);
}

bool handle_less(const RequestHandle& a, const RequestHandle& b) noexcept {
    // Accepted cohorts are run-homogeneous.
    return a.incarnation < b.incarnation;
}

void canonicalize_owner(Request& request) {
    if (auto* bind = std::get_if<BindOpenings>(&request.owner)) {
        std::sort(bind->openings.begin(), bind->openings.end(), handle_less);
    }
}

bool valid_anchor_rounding(NativeAnchorRounding rounding) noexcept {
    switch (rounding) {
    case NativeAnchorRounding::Raw:
    case NativeAnchorRounding::HalfUp:
    case NativeAnchorRounding::Directional:
        return true;
    }
    return false;
}

// Writes an anchored level into the trigger it was deferred for and retires
// the anchor, so a stored definition always reads as the absolute level it
// now is and can never be resolved a second time. False means the level is
// not a representable trigger level; the request is left untouched.
bool install_anchored_level(Request& request, double level) noexcept {
    if (!std::holds_alternative<FromOwnerFill>(request.anchor)) return false;
    if (auto* limit = std::get_if<Limit>(&request.trigger)) {
        if (!finite_non_negative(level)) return false;
        limit->price = level;
    } else if (auto* stop = std::get_if<Stop>(&request.trigger)) {
        if (!finite_non_negative(level)) return false;
        stop->price = level;
    } else if (auto* trail = std::get_if<Trail>(&request.trigger)) {
        if (!finite_positive(level)) return false;
        trail->arm_price = level;
    } else {
        return false;
    }
    request.anchor = Absolute{};
    return true;
}

// The kernel level of an anchored leg at its owner's fill: fill + offset,
// snapped onto the price tick ladder when the anchor asked for a rounding.
// Directional rounds toward the region the leg needs, relative to its own
// trigger kind and side (a limit and a trail arm from the favourable side, a
// stop from the adverse one), with the same grid arithmetic the run's L8
// price grid uses. Host-free: the tick is a value the caller passes in.
// False means the level cannot be computed (nonfinite operands, a rounding
// without a ladder); representability is the installer's check.
bool anchored_kernel_level(const Request& request, double fill_price,
                           std::optional<double> price_tick, bool leg_is_buy,
                           double* level_out) noexcept {
    const auto* anchor = std::get_if<FromOwnerFill>(&request.anchor);
    if (!anchor) return false;
    if (!std::isfinite(fill_price) || !std::isfinite(anchor->offset)) return false;
    double level = fill_price + anchor->offset;
    if (anchor->rounding != NativeAnchorRounding::Raw) {
        const double tick = price_tick ? *price_tick : 0.0;
        if (!finite_positive(tick)) return false;
        if (anchor->rounding == NativeAnchorRounding::HalfUp) {
            level = native_matching::grid_round_half_up(level, tick);
        } else {
            const bool favourable_side = std::holds_alternative<Limit>(request.trigger)
                || std::holds_alternative<Trail>(request.trigger);
            level = native_matching::grid_round_directional(
                    level, tick, favourable_side ? !leg_is_buy : leg_is_buy);
        }
    }
    if (level_out) *level_out = level;
    return true;
}

bool valid_position(const PositionIdentity& position) noexcept {
    if (std::holds_alternative<PositionFlat>(position)) return true;
    const auto* nonflat = std::get_if<PositionNonflat>(&position);
    return nonflat && nonflat->cycle > 0
        && (nonflat->side == Side::Long || nonflat->side == Side::Short);
}

bool same_position(const PositionIdentity& a, const PositionIdentity& b) noexcept {
    if (std::holds_alternative<PositionFlat>(a)) {
        return std::holds_alternative<PositionFlat>(b);
    }
    const auto* left = std::get_if<PositionNonflat>(&a);
    return left && position_matches(b, left->cycle, left->side);
}

// No allocation, including during post-commit installation. The consumer's
// canonical observations take O(M log M); unordered complete observations are
// also legal and use a duplicate check without a persistent membership store.
std::optional<CoreFailure> observe_openings(
        const std::vector<RequestHandle>& cohort, int64_t cycle, Side side,
        const PositionIdentity& position, const std::vector<OpeningObservation>& observations,
        std::size_t* live_count) noexcept {
    *live_count = 0;
    if (observations.size() < cohort.size()) return CoreFailure::MissingObservation;
    if (cohort.empty() || observations.size() != cohort.size() || !valid_position(position)) {
        return CoreFailure::ObservationMismatch;
    }
    bool ordered = true;
    for (std::size_t i = 0; i < observations.size(); ++i) {
        const auto& observation = observations[i];
        if (observation.queried_cycle != cycle
            || !same_position(observation.current_position, position)) {
            return CoreFailure::ObservationMismatch;
        }
        const auto member = std::lower_bound(cohort.begin(), cohort.end(),
                                              observation.queried_opening, handle_less);
        if (member == cohort.end() || *member != observation.queried_opening) {
            return CoreFailure::ObservationMismatch;
        }
        if (i && !handle_less(observations[i - 1].queried_opening,
                              observation.queried_opening)) ordered = false;
        if (observation.has_live_matching_lot) {
            if (!position_matches(position, cycle, side)) return CoreFailure::ObservationMismatch;
            ++*live_count;
        }
    }
    if (!ordered) {
        for (std::size_t i = 0; i < observations.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (observations[i].queried_opening == observations[j].queried_opening) {
                    return CoreFailure::ObservationMismatch;
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<CoreFailure> observe_openings(const TargetObservation& observation,
                                           const OpeningsClose& close,
                                           std::size_t* live_count) noexcept {
    return observe_openings(close.openings, close.cycle, close.side,
                            observation.current_position, observation.openings, live_count);
}

bool current_shape(const LiveRequest& live) noexcept {
    const auto& request = live.request();
    if (!std::holds_alternative<Market>(request.trigger)
        || !std::holds_alternative<MarketReady>(live.trigger_state)
        || !std::holds_alternative<ImmediateRemaining>(request.capacity)) return false;
    if (std::holds_alternative<Independent>(request.owner)) return true;
    if (const auto* sized = as_host_sized(request.intent)) {
        if (sized->kind != HostSizedKind::Close) return false;
        return std::holds_alternative<BindOpening>(request.owner)
            || std::holds_alternative<BindOpenings>(request.owner);
    }
    const auto* reduce = as_reduce(request.intent);
    if (!as_flatten(request.intent)
        && !(reduce && (explicit_size(*reduce) || fraction_size(*reduce)))) return false;
    return std::holds_alternative<BindOpening>(request.owner)
        || std::holds_alternative<BindOpenings>(request.owner);
}

bool driver_class_matches_cursor(DriverEligibilityClass driver,
                                  const MatchCursor& cursor) noexcept {
    using pineforge::NativePriceProvenance;
    using pineforge::NativePathPhase;
    switch (cursor.point.provenance) {
        case NativePriceProvenance::ObservedPrint:
            return driver == DriverEligibilityClass::ObservedPrint;
        case NativePriceProvenance::CarriedOpen:
            return driver == DriverEligibilityClass::CarriedOpen;
        case NativePriceProvenance::AfterCalculationClose:
            // Completion describes the slot, not whether the input was ticks
            // or a confirmed bar. The consumer supplies that transient fact.
            return driver == DriverEligibilityClass::TickAfterCalculation
                || driver == DriverEligibilityClass::ConfirmedAfterCalculationClose;
        case NativePriceProvenance::Confirmed:
            if (cursor.point.path_phase == NativePathPhase::High
                || cursor.point.path_phase == NativePathPhase::Low
                || cursor.point.path_phase == NativePathPhase::Close) {
                return driver == DriverEligibilityClass::ConfirmedExcursion;
            }
            return driver == DriverEligibilityClass::ConfirmedOpen;
        case NativePriceProvenance::ModeledOHLCOpen:
            return driver == DriverEligibilityClass::ConfirmedOpen;
        case NativePriceProvenance::ModeledOHLCClose:
            return driver == DriverEligibilityClass::ConfirmedExcursion
                || driver == DriverEligibilityClass::ConfirmedAfterCalculationClose;
        case NativePriceProvenance::PartialFinalized:
        case NativePriceProvenance::Calculation:
            return driver == DriverEligibilityClass::ConfirmedAfterCalculationClose;
        case NativePriceProvenance::CurrentExecution:
            return driver == DriverEligibilityClass::CurrentExecution;
    }
    return false;
}

const MatchCursor& transition_cursor(const TriggerTransition& transition) noexcept {
    return std::visit([](const auto& payload) -> const MatchCursor& { return payload.cursor; },
                      transition);
}

// The activation grid as the matcher spells it. A default ActivationGrid is
// an inactive threshold, under which every helper below is the raw compare
// and the raw print, bit for bit.
native_matching::GridThreshold matcher_grid(const ActivationGrid& grid) noexcept {
    native_matching::GridThreshold out;
    if (!finite_positive(grid.price_tick)) return out;
    out.tick = grid.price_tick;
    out.half_up = grid.half_up;
    return out;
}

// A buy activation needs the print at or above its level, a sell one at or
// below. Under an activation grid (L8b) the test is the matcher's own: the
// level itself, or a print inside the tick-quantized region the matcher
// reported, so a hit it accepted is never refused here.
bool stop_price_reached(bool is_buy, double level, double reached,
                        const native_matching::GridThreshold& grid) noexcept {
    if (!std::isfinite(reached) || !finite_non_negative(level)) return false;
    return native_matching::region_reached(reached, level, /*le=*/!is_buy, grid);
}

bool same_p_continuation(const LiveRequest& live, const MatchCursor& cursor) noexcept {
    if (cursor.point.ordinal == 0) return false;
    if (cursor.point.effective_time_ms < live.birth().decision_time_lower_bound) return false;
    auto same = [&](const MatchCursor& cause) noexcept {
        return cause.point.ordinal == cursor.point.ordinal;
    };
    if (const auto* armed = std::get_if<ArmedTransaction>(&live.authority)) {
        return same(armed->cause_cursor);
    }
    if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        if (const auto* from = std::get_if<EnrollmentFromApplied>(&close->enrollment)) {
            return same(from->cursor);
        }
    }
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        return same(close->binding_cursor);
    }
    return false;
}

bool trigger_cursor_eligible(const LiveRequest& live, const MatchCursor& cursor) noexcept {
    if (point_eligible(live.birth(), cursor.point.ordinal, cursor.point.effective_time_ms)) {
        return true;
    }
    return same_p_continuation(live, cursor);
}

const ExecutionAppliedEvent* as_applied(const CommandEvent& event) noexcept {
    return std::get_if<ExecutionAppliedEvent>(&event);
}

int receipt_cmp(uint64_t oa, uint64_t ia, GroupEffect ea, uint64_t ob, uint64_t ib,
                GroupEffect eb) noexcept {
    if (oa < ob) return -1;
    if (oa > ob) return 1;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    const auto a = static_cast<std::uint8_t>(ea);
    const auto b = static_cast<std::uint8_t>(eb);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// A trail offset is a finite price distance. Zero is the "ride the best"
// spelling: the level is the best itself and only a move strictly past it
// exits. Negative and nonfinite offsets stay rejected.
bool valid_trail_offset(double offset) noexcept {
    return std::isfinite(offset) && offset >= 0.0;
}

// The anchored level is supplied by the owner's fill, so the trigger leaves
// it unwritten until then; any other written level would be silently
// overwritten at arm time. "Unwritten" is each field's own absence: the 0.0
// Limit::price and Stop::price already default to, and either the absent
// std::optional or that same 0.0 for a Trail's arm threshold.
std::optional<RequestRejectReason> validate_levels(const Trigger& trigger, bool anchored) {
    if (const auto* limit = std::get_if<Limit>(&trigger)) {
        if (!finite_non_negative(limit->price)) return RequestRejectReason::InvalidTrigger;
        if (anchored && limit->price != 0.0) return RequestRejectReason::InvalidTrigger;
        return std::nullopt;
    }
    if (const auto* stop = std::get_if<Stop>(&trigger)) {
        if (!finite_non_negative(stop->price)) return RequestRejectReason::InvalidTrigger;
        if (anchored && stop->price != 0.0) return RequestRejectReason::InvalidTrigger;
        return std::nullopt;
    }
    if (const auto* stop_limit = std::get_if<StopLimit>(&trigger)) {
        if (!finite_non_negative(stop_limit->stop) || !finite_non_negative(stop_limit->limit)) {
            return RequestRejectReason::InvalidTrigger;
        }
        // One signed distance cannot describe two levels.
        if (anchored) return RequestRejectReason::InvalidTrigger;
        return std::nullopt;
    }
    if (const auto* trail = std::get_if<Trail>(&trigger)) {
        if (trail->ticks) {
            if (!std::isfinite(trail->ticks->ticks) || trail->ticks->ticks < 0.0
                || trail->offset != 0.0) {
                return RequestRejectReason::InvalidTrigger;
            }
        } else if (!valid_trail_offset(trail->offset)) {
            return RequestRejectReason::InvalidTrigger;
        }
        // The seeded start of the running best is an absolute price level,
        // whatever the anchor does to the arm threshold, so it is checked
        // before the anchored shape returns.
        if (trail->best_seed && !finite_positive(*trail->best_seed)) {
            return RequestRejectReason::InvalidTrigger;
        }
        // An anchored trail's arm threshold comes from the owner's fill, so
        // omitting it is the spelling: install_anchored_level assigns the
        // resolved level unconditionally and a waiting request never reaches
        // the matcher, so the field is unread until the arm. A WRITTEN level
        // stays refused for the same reason a written Limit/Stop price is --
        // the arm would silently overwrite it. The placeholder remains legal
        // so every host that already spells it keeps its meaning.
        if (anchored) {
            if (trail->arm_price && *trail->arm_price != 0.0) {
                return RequestRejectReason::InvalidTrigger;
            }
            return std::nullopt;
        }
        if (trail->arm_price && !finite_positive(*trail->arm_price)) {
            return RequestRejectReason::InvalidTrigger;
        }
    }
    if (anchored && std::holds_alternative<Market>(trigger)) {
        return RequestRejectReason::InvalidTrigger;
    }
    return std::nullopt;
}

bool on_optional_grid(double value, std::optional<double> grid) {
    return !grid || quantity_on_grid(value, *grid);
}

// Prepared tokens are short-lived transactional envelopes. Reusing their
// storage keeps repeated submit/replace/evaluation traffic from returning to
// the allocator at every broker point; the contained plans still construct,
// validate and commit exactly as before. This thread-local scratch is neither
// request state nor part of the continuation hash.
template<class Tag>
struct PreparedImplStorage {
    static void* allocate(std::size_t size) {
        auto& recycled = free_blocks();
        if (!recycled.empty()) {
            void* block = recycled.back();
            recycled.pop_back();
            return block;
        }
        return ::operator new(size);
    }

    static void deallocate(void* block) noexcept {
        if (!block) return;
        try {
            free_blocks().push_back(block);
        } catch (...) {
            ::operator delete(block);
        }
    }

private:
    // The recycled blocks are released when the thread-local scratch is
    // destroyed; otherwise LeakSanitizer reports every block still parked in
    // the freelist at process exit as a direct leak (CI sanitizers lane).
    struct Recycled {
        std::vector<void*> blocks;
        Recycled() = default;
        Recycled(const Recycled&) = delete;
        Recycled& operator=(const Recycled&) = delete;
        ~Recycled() {
            for (void* block : blocks) ::operator delete(block);
        }
    };
    static std::vector<void*>& free_blocks() {
        static thread_local Recycled recycled;
        return recycled.blocks;
    }
};

struct PreparedSubmitStorageTag {};
struct PreparedReplaceStorageTag {};
struct PreparedCancelStorageTag {};
struct PreparedMutationStorageTag {};
struct PreparedExecutionStorageTag {};

void fill_applied_cursor(ExecutionAppliedEvent& event, const MatchCursor& cursor) {
    event.cursor = cursor;
}

CancelledEvent make_cancelled(uint64_t ordinal, const LiveRequest& live, CancelReason reason,
                              std::optional<EventId> cause) {
    CancelledEvent event;
    event.ordinal = ordinal;
    event.definition = live.definition;
    event.reason = reason;
    event.cause = std::move(cause);
    event.prior_authority = live.authority;
    event.unexecuted = project_remaining(live.remaining);
    event.pending = live.pending;
    return event;
}

// Whether a handle can name a request of this run at all.
inline bool names_run(const RequestHandle& handle, const RunIdentity& run) noexcept {
    return handle.incarnation != 0 && handle.run.run_number != 0
        && !handle.run.session_key.empty() && handle.run == run;
}

// Where the live row carrying `incarnation` stands, or live.size() when no
// row does. The working book is in strictly increasing incarnation order, so
// a long book is bisected down to a short run and the run is walked up to the
// first row not older than the one asked for (R5 lane PERF-K3).
// WorkingRequestCore::commit keeps that order: a push and an erase-push
// append a row born under a fresh incarnation, which usable_incarnation makes
// larger than every one the run issued before; an erase keeps the rest in
// order; an update rewrites a row in place under its own handle.
inline std::size_t live_position(const std::vector<LiveRequest>& live,
                                 uint64_t incarnation) noexcept {
    constexpr std::size_t kWalk = 16;
    std::size_t first = 0;
    std::size_t count = live.size();
    while (count > kWalk) {
        const std::size_t half = count / 2;
        if (live[first + half].handle().incarnation < incarnation) {
            first += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    for (std::size_t i = first; i < live.size(); ++i) {
        const uint64_t row = live[i].handle().incarnation;
        if (row < incarnation) continue;
        return row == incarnation ? i : live.size();
    }
    return live.size();
}

}  // namespace

struct PreparedSubmit::Impl {
    WorkingRequestCore::MutationPlan plan;
    SubmitResult result;

    static void* operator new(std::size_t size) {
        return PreparedImplStorage<PreparedSubmitStorageTag>::allocate(size);
    }
    static void operator delete(void* block) noexcept {
        PreparedImplStorage<PreparedSubmitStorageTag>::deallocate(block);
    }
    static void operator delete(void* block, std::size_t) noexcept {
        PreparedImplStorage<PreparedSubmitStorageTag>::deallocate(block);
    }
};
PreparedSubmit::PreparedSubmit() noexcept = default;
PreparedSubmit::PreparedSubmit(PreparedSubmit&&) noexcept = default;
PreparedSubmit& PreparedSubmit::operator=(PreparedSubmit&&) noexcept = default;
PreparedSubmit::PreparedSubmit(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedSubmit::~PreparedSubmit() = default;
PreparedSubmit::operator bool() const noexcept { return static_cast<bool>(impl_); }
const SubmitResult& PreparedSubmit::predicted() const {
    if (!impl_) throw std::logic_error("empty prepared submit");
    return impl_->result;
}
EventId PreparedSubmit::predicted_event_id() const {
    if (!impl_) throw std::logic_error("empty prepared submit");
    return EventId{impl_->plan.run, impl_->result.event_ordinal};
}

struct PreparedReplace::Impl {
    WorkingRequestCore::MutationPlan plan;
    ReplaceResult result;

    static void* operator new(std::size_t size) {
        return PreparedImplStorage<PreparedReplaceStorageTag>::allocate(size);
    }
    static void operator delete(void* block) noexcept {
        PreparedImplStorage<PreparedReplaceStorageTag>::deallocate(block);
    }
    static void operator delete(void* block, std::size_t) noexcept {
        PreparedImplStorage<PreparedReplaceStorageTag>::deallocate(block);
    }
};
PreparedReplace::PreparedReplace() noexcept = default;
PreparedReplace::PreparedReplace(PreparedReplace&&) noexcept = default;
PreparedReplace& PreparedReplace::operator=(PreparedReplace&&) noexcept = default;
PreparedReplace::PreparedReplace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedReplace::~PreparedReplace() = default;
PreparedReplace::operator bool() const noexcept { return static_cast<bool>(impl_); }
const ReplaceResult& PreparedReplace::predicted() const {
    if (!impl_) throw std::logic_error("empty prepared replace");
    return impl_->result;
}
EventId PreparedReplace::predicted_event_id() const {
    if (!impl_) throw std::logic_error("empty prepared replace");
    return EventId{impl_->plan.run, impl_->result.event_ordinal};
}

struct PreparedCancel::Impl {
    WorkingRequestCore::MutationPlan plan;
    CancelResult result;

    static void* operator new(std::size_t size) {
        return PreparedImplStorage<PreparedCancelStorageTag>::allocate(size);
    }
    static void operator delete(void* block) noexcept {
        PreparedImplStorage<PreparedCancelStorageTag>::deallocate(block);
    }
    static void operator delete(void* block, std::size_t) noexcept {
        PreparedImplStorage<PreparedCancelStorageTag>::deallocate(block);
    }
};
PreparedCancel::PreparedCancel() noexcept = default;
PreparedCancel::PreparedCancel(PreparedCancel&&) noexcept = default;
PreparedCancel& PreparedCancel::operator=(PreparedCancel&&) noexcept = default;
PreparedCancel::PreparedCancel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedCancel::~PreparedCancel() = default;
PreparedCancel::operator bool() const noexcept { return static_cast<bool>(impl_); }
const CancelResult& PreparedCancel::predicted() const {
    if (!impl_) throw std::logic_error("empty prepared cancel");
    return impl_->result;
}
EventId PreparedCancel::predicted_event_id() const {
    if (!impl_) throw std::logic_error("empty prepared cancel");
    return EventId{impl_->plan.run, impl_->result.event_ordinal};
}

struct PreparedMutation::Impl {
    WorkingRequestCore::MutationPlan plan;

    static void* operator new(std::size_t size) {
        return PreparedImplStorage<PreparedMutationStorageTag>::allocate(size);
    }
    static void operator delete(void* block) noexcept {
        PreparedImplStorage<PreparedMutationStorageTag>::deallocate(block);
    }
    static void operator delete(void* block, std::size_t) noexcept {
        PreparedImplStorage<PreparedMutationStorageTag>::deallocate(block);
    }
};
PreparedMutation::PreparedMutation() noexcept = default;
PreparedMutation::PreparedMutation(PreparedMutation&&) noexcept = default;
PreparedMutation& PreparedMutation::operator=(PreparedMutation&&) noexcept = default;
PreparedMutation::PreparedMutation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedMutation::~PreparedMutation() = default;
PreparedMutation::operator bool() const noexcept { return static_cast<bool>(impl_); }

struct PreparedExecution::Impl {
    WorkingRequestCore::MutationPlan terminal;
    CommandEvent retain_event{ExecutionAppliedEvent{}};
    LiveRequest retain_live{};
    bool can_retain = false;
    bool flatten = false;
    bool opening = false;
    BookClose book_close{};
    OpeningClose opening_close{};
    std::optional<OpeningsClose> openings_close;
    std::optional<CohortClose> cohort_close;
    ExecutionProposal proposal{};

    static void* operator new(std::size_t size) {
        return PreparedImplStorage<PreparedExecutionStorageTag>::allocate(size);
    }
    static void operator delete(void* block) noexcept {
        PreparedImplStorage<PreparedExecutionStorageTag>::deallocate(block);
    }
    static void operator delete(void* block, std::size_t) noexcept {
        PreparedImplStorage<PreparedExecutionStorageTag>::deallocate(block);
    }
};
PreparedExecution::PreparedExecution() noexcept = default;
PreparedExecution::PreparedExecution(PreparedExecution&&) noexcept = default;
PreparedExecution& PreparedExecution::operator=(PreparedExecution&&) noexcept = default;
PreparedExecution::PreparedExecution(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedExecution::~PreparedExecution() = default;
PreparedExecution::operator bool() const noexcept { return static_cast<bool>(impl_); }

void WorkingRequestCore::require_identity(const RunIdentity& identity) {
    if (identity.session_key.empty() || identity.run_number == 0) {
        throw std::invalid_argument(
                "native run identity requires a nonempty session key and positive run number");
    }
}

void WorkingRequestCore::require_distinct_counters(uint64_t& next_order_incarnation,
                                                   uint64_t& next_timeline_ordinal) {
    if (&next_order_incarnation == &next_timeline_ordinal) {
        throw std::invalid_argument(
                "native order incarnation and timeline counters must be distinct");
    }
}

void WorkingRequestCore::require_grid(std::optional<double> quantity_grid) {
    if (!quantity_grid) return;
    if (!std::isfinite(*quantity_grid) || *quantity_grid <= 0.0) {
        throw std::invalid_argument("quantity grid requires a finite positive step");
    }
}

uint64_t WorkingRequestCore::usable_ordinal(uint64_t next) const {
    if (next == 0) {
        throw std::invalid_argument("native timeline ordinal must be nonzero");
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native timeline ordinal exhausted");
    }
    if (next <= last_ordinal_) {
        throw std::invalid_argument("native timeline ordinal reused or regressed");
    }
    return next;
}

uint64_t WorkingRequestCore::usable_incarnation(uint64_t next) const {
    if (next == 0) {
        throw std::invalid_argument("native order incarnation must be nonzero");
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native order incarnation exhausted");
    }
    if (next <= last_incarnation_) {
        throw std::invalid_argument("native order incarnation reused or regressed");
    }
    return next;
}

bool WorkingRequestCore::bump_epoch() noexcept {
    if (epoch_ == std::numeric_limits<uint64_t>::max()) return false;
    ++epoch_;
    return true;
}

void WorkingRequestCore::require_epoch_room() const {
    if (epoch_ >= std::numeric_limits<uint64_t>::max() - 1) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
}

void WorkingRequestCore::clear_unbound() noexcept {
    identity_.session_key.clear();
    identity_.run_number = 0;
    live_.clear();
    history_.clear();
    last_ordinal_ = 0;
    last_incarnation_ = 0;
    ordinal_index_.clear();
    receipts_.clear();
    next_cohort_handle_ = 1;
    cohorts_.clear();
    cohort_receipts_.clear();
    epoch_ = 0;
    if (instance_) instance_->expired = true;
    instance_.reset();
}

void WorkingRequestCore::bind_plan(MutationPlan& plan) const {
    plan.instance = instance_;
    plan.generation = instance_ ? instance_->generation : 0;
}

WorkingRequestCore::MutationPlan WorkingRequestCore::begin_plan() const {
    require_identity(identity_);
    if (!instance_ || instance_->expired) {
        throw std::invalid_argument(
                "native run identity requires a nonempty session key and positive run number");
    }
    require_epoch_room();
    MutationPlan plan;
    plan.run = identity_;
    plan.history_size = history_.size();
    plan.last_ordinal = last_ordinal_;
    bind_plan(plan);
    plan.epoch = epoch_ + 1;
    return plan;
}

void WorkingRequestCore::seal_plan(MutationPlan& plan) {
    require_epoch_room();
    reserve_plan(plan);
    if (!bump_epoch()) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
    plan.epoch = epoch_;
    bind_plan(plan);
}

void WorkingRequestCore::reserve(std::size_t expected_events) {
    if (expected_events > history_.capacity()) history_.reserve(expected_events);
    if (expected_events > ordinal_index_.capacity()) ordinal_index_.reserve(expected_events);
    if (live_.capacity() < 8) live_.reserve(8);
}

void WorkingRequestCore::reserve_plan(const MutationPlan& plan) {
    reserve_n(history_, plan.events.size());
    reserve_n(ordinal_index_, plan.events.size());
    if (plan.live_change == kLivePush) reserve_n(live_, 1);
    if (plan.add_receipt) reserve_n(receipts_, 1);
}

PreparedMutation WorkingRequestCore::finish_mutation(MutationPlan plan) {
    seal_plan(plan);
    auto impl = std::make_unique<PreparedMutation::Impl>();
    impl->plan = std::move(plan);
    return PreparedMutation(std::move(impl));
}

WorkingRequestCore::TargetKind WorkingRequestCore::classify(
        const RequestHandle& handle, std::size_t* live_index) const {
    if (!names_run(handle, identity_)) return TargetKind::InvalidHandle;
    const std::size_t index = live_position(live_, handle.incarnation);
    if (index == live_.size()) return TargetKind::NotWorking;
    if (live_index) *live_index = index;
    return TargetKind::Live;
}

// classify's lookup without the call to classify: every lookup the consumer
// makes passes through here.
const LiveRequest* WorkingRequestCore::find_live(const RequestHandle& handle) const {
    if (!names_run(handle, identity_)) return nullptr;
    const std::size_t index = live_position(live_, handle.incarnation);
    return index == live_.size() ? nullptr : &live_[index];
}

const CommandEvent* WorkingRequestCore::event_at(const EventId& id) const {
    if (id.run != identity_ || id.ordinal == 0) return nullptr;
    const auto it = std::lower_bound(
            ordinal_index_.begin(), ordinal_index_.end(), id.ordinal,
            [](const std::pair<uint64_t, std::size_t>& row, uint64_t ordinal) {
                return row.first < ordinal;
            });
    if (it == ordinal_index_.end() || it->first != id.ordinal) return nullptr;
    if (it->second >= history_.size()) return nullptr;
    const CommandEvent* event = &history_[it->second];
    if (event_ordinal(*event) != id.ordinal) return nullptr;
    return event;
}

const RequestDefinition* WorkingRequestCore::definition_for(
        const RequestHandle& handle) const noexcept {
    if (handle.incarnation == 0 || handle.run != identity_) return nullptr;
    if (const auto* live = find_live(handle)) return live->definition.get();
    // A definition is immutable and its later lifecycle events retain the
    // same shared definition pointer. Requests receive monotonically
    // increasing incarnations, so the most recent occurrence is normally
    // close to the tail. Search backwards to avoid a whole-run forward scan
    // at every generic cohort candidate.
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        const auto& event = *it;
        if (const auto* accepted = std::get_if<AcceptedEvent>(&event)) {
            if (accepted->definition && accepted->definition->handle == handle)
                return accepted->definition.get();
        } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
            if (replaced->predecessor_definition
                && replaced->predecessor_definition->handle == handle) {
                return replaced->predecessor_definition.get();
            }
            if (replaced->successor_definition
                && replaced->successor_definition->handle == handle) {
                return replaced->successor_definition.get();
            }
        }
    }
    return nullptr;
}

std::optional<RequestHandle> WorkingRequestCore::canonical_cohort_origin(
        const RequestHandle& origin) const {
    const RequestDefinition* current = definition_for(origin);
    if (!current) return std::nullopt;
    RequestHandle root = current->handle;
    std::size_t remaining = history_.size() + live_.size() + 1;
    while (current->predecessor) {
        if (remaining-- == 0) return std::nullopt;
        current = definition_for(*current->predecessor);
        if (!current) return std::nullopt;
        root = current->handle;
    }
    return root;
}

std::size_t WorkingRequestCore::cohort_index(CohortHandle cohort) const noexcept {
    const auto it = std::lower_bound(
        cohorts_.begin(), cohorts_.end(), cohort,
        [](const CohortRoster& roster, CohortHandle value) { return roster.handle < value; });
    return it != cohorts_.end() && it->handle == cohort
        ? static_cast<std::size_t>(it - cohorts_.begin())
        : cohorts_.size();
}

CohortHandle WorkingRequestCore::cohort_open() {
    require_identity(identity_);
    require_epoch_room();
    if (next_cohort_handle_ == 0 || next_cohort_handle_ == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native cohort handle exhausted");
    }
    const CohortHandle handle{next_cohort_handle_};
    cohorts_.push_back(CohortRoster{handle, {}});
    ++next_cohort_handle_;
    if (!bump_epoch()) throw std::overflow_error("native working-request epoch exhausted");
    return handle;
}

void WorkingRequestCore::cohort_add(CohortHandle cohort, RequestHandle origin) {
    require_identity(identity_);
    require_epoch_room();
    CohortReceipt receipt;
    receipt.operation = CohortReceiptOperation::Add;
    receipt.cohort = cohort;
    receipt.origin = origin;
    const std::size_t index = cohort_index(cohort);
    if (cohort.value == 0 || index == cohorts_.size()) {
        receipt.status = CohortReceiptStatus::InvalidHandle;
    } else if (!definition_for(origin)) {
        receipt.status = CohortReceiptStatus::UnknownOrigin;
    } else if (!find_live(origin)) {
        receipt.status = CohortReceiptStatus::TerminalOrigin;
    } else if (const auto canonical = canonical_cohort_origin(origin)) {
        auto& origins = cohorts_[index].origins;
        const auto where = std::lower_bound(origins.begin(), origins.end(), *canonical, handle_less);
        if (where == origins.end() || *where != *canonical) origins.insert(where, *canonical);
        receipt.status = CohortReceiptStatus::Applied;
    } else {
        receipt.status = CohortReceiptStatus::UnknownOrigin;
    }
    cohort_receipts_.push_back(std::move(receipt));
    if (!bump_epoch()) throw std::overflow_error("native working-request epoch exhausted");
}

void WorkingRequestCore::cohort_remove(CohortHandle cohort, RequestHandle origin) {
    require_identity(identity_);
    require_epoch_room();
    CohortReceipt receipt;
    receipt.operation = CohortReceiptOperation::Remove;
    receipt.cohort = cohort;
    receipt.origin = origin;
    const std::size_t index = cohort_index(cohort);
    if (cohort.value == 0 || index == cohorts_.size()) {
        receipt.status = CohortReceiptStatus::InvalidHandle;
    } else if (!definition_for(origin)) {
        receipt.status = CohortReceiptStatus::UnknownOrigin;
    } else if (const auto canonical = canonical_cohort_origin(origin)) {
        auto& origins = cohorts_[index].origins;
        const auto where = std::lower_bound(origins.begin(), origins.end(), *canonical, handle_less);
        if (where != origins.end() && *where == *canonical) origins.erase(where);
        receipt.status = CohortReceiptStatus::Applied;
    } else {
        receipt.status = CohortReceiptStatus::UnknownOrigin;
    }
    cohort_receipts_.push_back(std::move(receipt));
    if (!bump_epoch()) throw std::overflow_error("native working-request epoch exhausted");
}

bool WorkingRequestCore::cohort_contains(
        CohortHandle cohort, const RequestHandle& opening) const {
    const std::size_t index = cohort_index(cohort);
    if (cohort.value == 0 || index == cohorts_.size()) return false;
    const RequestDefinition* current = definition_for(opening);
    std::size_t remaining = history_.size() + live_.size() + 1;
    while (current) {
        const auto& origins = cohorts_[index].origins;
        if (std::binary_search(origins.begin(), origins.end(), current->handle, handle_less)) {
            return true;
        }
        if (!current->predecessor || remaining-- == 0) break;
        current = definition_for(*current->predecessor);
    }
    return false;
}

bool WorkingRequestCore::authenticate_receipt_outcome(const CommandEvent& event,
                                                      const EventId& cause,
                                                      const RequestHandle& recipient,
                                                      GroupEffect effect) const {
    if (const auto* reduced = std::get_if<ReservationReducedEvent>(&event)) {
        return effect == GroupEffect::Reduce && reduced->cause == cause
            && reduced->recipient == recipient && reduced->effect == GroupEffect::Reduce
            && reduced->definition && reduced->definition->handle == recipient;
    }
    if (const auto* deferred = std::get_if<DeferredGroupAdjustmentEvent>(&event)) {
        return effect == GroupEffect::Reduce && deferred->cause == cause
            && deferred->recipient == recipient && deferred->effect == GroupEffect::Reduce
            && deferred->definition && deferred->definition->handle == recipient;
    }
    if (const auto* cancelled = std::get_if<CancelledEvent>(&event)) {
        const bool allowed_effect = effect == GroupEffect::Cancel
            || (effect == GroupEffect::Reduce
                && std::holds_alternative<RemainingProjectionFlattenAll>(cancelled->unexecuted)
                && cancelled->definition
                && as_flatten(cancelled->definition->request.intent));
        return allowed_effect && cancelled->reason == CancelReason::Group
            && cancelled->cause == cause && cancelled->definition
            && cancelled->definition->handle == recipient;
    }
    return false;
}

bool WorkingRequestCore::collect_pending_chain(const PendingAdjustments& pending,
                                               const RequestHandle& recipient,
                                               std::vector<EventId>* ids,
                                               double* total) const {
    ids->clear();
    *total = 0.0;
    const auto* deferred = std::get_if<PendingDeferred>(&pending);
    if (!deferred) return true;
    if (deferred->count == 0 || deferred->tail_receipt.ordinal == 0
        || deferred->tail_receipt.run != identity_) {
        return false;
    }
    EventId cursor = deferred->tail_receipt;
    for (uint64_t i = 0; i < deferred->count; ++i) {
        const CommandEvent* event = event_at(cursor);
        const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
        if (!payload || payload->recipient != recipient || payload->effect != GroupEffect::Reduce
            || !payload->definition || payload->definition->handle != recipient) {
            return false;
        }
        ids->push_back(cursor);
        if (i + 1 < deferred->count) {
            if (!payload->previous_pending_receipt
                || payload->previous_pending_receipt->ordinal >= cursor.ordinal) return false;
            cursor = *payload->previous_pending_receipt;
        } else if (payload->previous_pending_receipt) {
            return false;
        }
    }
    if (ids->size() != deferred->count) return false;
    std::reverse(ids->begin(), ids->end());
    // Reproduce the exact accumulation order used when each receipt was
    // committed. Reassociating these additions tail-first changes binary64
    // totals for valid chains such as 0.1, 0.2, 0.3.
    double sum = 0.0;
    for (std::size_t i = 0; i < ids->size(); ++i) {
        const CommandEvent* event = event_at((*ids)[i]);
        const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
        if (!payload || !finite_positive(payload->deferred_delta)) return false;
        if (i == 0) {
            if (!std::holds_alternative<PendingNone>(payload->pending_before)) return false;
        } else {
            const auto* before = std::get_if<PendingDeferred>(&payload->pending_before);
            if (!before || before->total != sum || before->count != i
                || before->tail_receipt != (*ids)[i - 1]) return false;
        }
        if (!checked_add_positive(sum, payload->deferred_delta, &sum)) return false;
        if (payload->pending_after.total != sum || payload->pending_after.count != i + 1
            || payload->pending_after.tail_receipt != (*ids)[i]) return false;
    }
    if (sum != deferred->total) return false;
    *total = sum;
    return true;
}

GroupEffectReceipt WorkingRequestCore::group_effect_receipt(std::size_t index) const {
    const ReceiptKey& key = receipts_.at(index);
    return GroupEffectReceipt{key.cause, key.recipient, key.effect, key.outcome_ordinal};
}

WorkingRequestCore::ReceiptLookup WorkingRequestCore::receipt_lookup(
        const EventId& cause, const RequestHandle& recipient, GroupEffect effect,
        uint64_t* outcome) const {
    ReceiptKey needle{cause, recipient, effect, 0};
    const auto it = std::lower_bound(
            receipts_.begin(), receipts_.end(), needle,
            [](const ReceiptKey& a, const ReceiptKey& b) {
                return receipt_cmp(a.cause.ordinal, a.recipient.incarnation, a.effect,
                                   b.cause.ordinal, b.recipient.incarnation, b.effect)
                    < 0;
            });
    if (it == receipts_.end()) return ReceiptLookup::Absent;
    if (it->cause != cause || it->recipient != recipient || it->effect != effect) {
        return ReceiptLookup::Absent;
    }
    const CommandEvent* event = event_at(EventId{identity_, it->outcome_ordinal});
    if (!event || !authenticate_receipt_outcome(*event, cause, recipient, effect)) {
        return ReceiptLookup::Conflict;
    }
    if (outcome) *outcome = it->outcome_ordinal;
    return ReceiptLookup::Present;
}

std::optional<InstallError> WorkingRequestCore::validate_plan(const MutationPlan& plan) const noexcept {
    if (plan.consumed) return InstallError::AlreadyConsumed;
    const auto locked = plan.instance.lock();
    if (!locked || locked != instance_ || !instance_ || instance_->expired
        || plan.generation != instance_->generation || plan.run != identity_) {
        return InstallError::WrongCoreOrRun;
    }
    if (plan.epoch != epoch_ || plan.history_size != history_.size()
        || plan.last_ordinal != last_ordinal_
        || epoch_ == std::numeric_limits<uint64_t>::max()) {
        return InstallError::StalePreparation;
    }
    return std::nullopt;
}

bool WorkingRequestCore::trail_level_ok(double best, double offset, bool is_buy,
                                        double* stop, double ladder_tick) const noexcept {
    if (!valid_trail_offset(offset) || !std::isfinite(best)) return false;
    // One definition of a trailing level, shared with the matcher: finite, a
    // zero offset riding the best, every positive offset landing strictly
    // beyond it, and a whole tick count from a best on the run's ladder
    // naming that ladder point (R5 lane E16). The core re-validates an
    // activation the matcher reported, so a second spelling here would refuse
    // a hit the matcher just booked.
    double level = 0.0;
    if (!native_matching::checked_trail_stop(best, offset, is_buy, &level, ladder_tick)) {
        return false;
    }
    if (stop) *stop = level;
    return true;
}

InstallResult WorkingRequestCore::commit(MutationPlan& plan) noexcept {
    if (const auto error = validate_plan(plan)) return *error;
    const std::size_t first = history_.size();
    const std::size_t count = plan.events.size();
    for (std::size_t i = 0; i < plan.events.size(); ++i) {
        auto& event = plan.events[i];
        const uint64_t ordinal = event_ordinal(event);
        history_.push_back(std::move(event));
        ordinal_index_.push_back({ordinal, history_.size() - 1});
        last_ordinal_ = ordinal;
    }
    if (plan.live_change == kLivePush) {
        live_.push_back(std::move(plan.live_row));
    } else if (plan.live_change == kLiveErase) {
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index));
    } else if (plan.live_change == kLiveUpdate) {
        live_[plan.live_index] = std::move(plan.live_row);
    } else if (plan.live_change == kLiveErasePush) {
        if (plan.live_index + 1 == live_.size()) {
            live_.back() = std::move(plan.live_row);
        } else {
            std::rotate(live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index),
                        live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index + 1),
                        live_.end());
            live_.back() = std::move(plan.live_row);
        }
    }
    if (plan.add_receipt) {
        receipts_.push_back(ReceiptKey{std::move(plan.receipt_cause), std::move(plan.receipt_recipient),
                                       plan.receipt_effect, plan.receipt_outcome});
    }
    if (plan.consume_incarnation) last_incarnation_ = plan.incarnation_used;
    plan.consumed = true;
    bump_epoch();
    return Installed{EventRange{first, count}};
}

WorkingRequestCore::WorkingRequestCore(RunIdentity identity) {
    require_identity(identity);
    identity_ = std::move(identity);
    instance_ = std::make_shared<InstanceBinding>();
}

WorkingRequestCore::WorkingRequestCore(WorkingRequestCore&& other) noexcept {
    *this = std::move(other);
}

WorkingRequestCore& WorkingRequestCore::operator=(WorkingRequestCore&& other) noexcept {
    if (this == &other) return *this;
    identity_ = std::move(other.identity_);
    live_ = std::move(other.live_);
    history_ = std::move(other.history_);
    last_ordinal_ = other.last_ordinal_;
    last_incarnation_ = other.last_incarnation_;
    epoch_ = other.epoch_;
    ordinal_index_ = std::move(other.ordinal_index_);
    receipts_ = std::move(other.receipts_);
    next_cohort_handle_ = other.next_cohort_handle_;
    cohorts_ = std::move(other.cohorts_);
    cohort_receipts_ = std::move(other.cohort_receipts_);
    instance_ = std::move(other.instance_);
    if (instance_) {
        if (instance_->generation != std::numeric_limits<uint64_t>::max()) {
            ++instance_->generation;
        } else {
            instance_->expired = true;
        }
    }
    other.identity_.session_key.clear();
    other.identity_.run_number = 0;
    other.live_.clear();
    other.history_.clear();
    other.last_ordinal_ = 0;
    other.last_incarnation_ = 0;
    other.epoch_ = 0;
    other.ordinal_index_.clear();
    other.receipts_.clear();
    other.next_cohort_handle_ = 1;
    other.cohorts_.clear();
    other.cohort_receipts_.clear();
    other.instance_.reset();
    return *this;
}

void WorkingRequestCore::reset(RunIdentity identity) {
    require_identity(identity);
    WorkingRequestCore next(std::move(identity));
    *this = std::move(next);
}

std::optional<RequestRejectReason> WorkingRequestCore::validate_request(
        const Request& request,
        const CommandContext& context,
        const std::optional<RequestHandle>& replace_target) const {
    require_grid(context.quantity_grid);
    const auto* reverse_to = as_reverse_to(request.intent);
    const auto* host_sized = as_host_sized(request.intent);
    const auto* sized = as_sized(request.intent);
    const auto* scope_fraction = deferred_reduction(request.intent);
    if (as_flatten(request.intent)) {
    } else if (const auto* reduce = as_reduce(request.intent)) {
        if (const auto* units = explicit_size(*reduce)) {
            if (!finite_positive(units->units)) return RequestRejectReason::InvalidQuantity;
            if (!on_optional_grid(units->units, context.quantity_grid)) {
                return RequestRejectReason::OffGrid;
            }
        } else if (const auto* fraction = fraction_size(*reduce)) {
            if (fraction->claim != ScopeClaim::Gross
                && fraction->claim != ScopeClaim::NetOfSiblings) {
                return RequestRejectReason::InvalidQuantity;
            }
            if (fraction->basis != ScopeBasis::AtMatch
                && fraction->basis != ScopeBasis::AtAcceptance) {
                return RequestRejectReason::InvalidQuantity;
            }
            if (!basis_fraction_ok(fraction->fraction)) {
                return RequestRejectReason::InvalidQuantityBasis;
            }
        }
    } else if (sized) {
        if ((sized->side != Side::Long && sized->side != Side::Short)
            || (sized->time != SizeTime::AtMatch && sized->time != SizeTime::AtAcceptance)
            || (sized->price != SizePrice::Resolved && sized->price != SizePrice::Signal
                && sized->price != SizePrice::SignalOnTick)
            || (sized->grid_policy != ExecutionGridPolicy::SnapToGrid
                && sized->grid_policy != ExecutionGridPolicy::ExplicitUnits)) {
            return RequestRejectReason::InvalidQuantity;
        }
        if (const auto* cash = std::get_if<CashValue>(&sized->basis)) {
            if (!finite_positive(cash->cash)) return RequestRejectReason::InvalidQuantityBasis;
        } else if (!basis_fraction_ok(std::get<EquityFraction>(sized->basis).fraction)) {
            return RequestRejectReason::InvalidQuantityBasis;
        }
        // An acceptance-time basis is an admission input at placement: the
        // producer measured the frozen quantity against the run's opening
        // caps and margin before this validation ran.
        if (!context.sizing_admissible) {
            return RequestRejectReason::PlacementAdmission;
        }
    } else if (const auto* transact = as_transact(request.intent)) {
        if (!finite_nonzero(transact->signed_units)) return RequestRejectReason::InvalidQuantity;
        if (!on_optional_grid(transact->signed_units, context.quantity_grid)) {
            return RequestRejectReason::OffGrid;
        }
    } else if (reverse_to) {
        if (!finite_nonzero(reverse_to->signed_units)) return RequestRejectReason::InvalidQuantity;
        if (!on_optional_grid(reverse_to->signed_units, context.quantity_grid)) {
            return RequestRejectReason::OffGrid;
        }
    } else if (host_sized) {
        if ((host_sized->kind != HostSizedKind::Open && host_sized->kind != HostSizedKind::Close)
            || (host_sized->kind == HostSizedKind::Open && !host_sized->side)
            || (host_sized->kind == HostSizedKind::Close && host_sized->side)
            || (host_sized->side && *host_sized->side != Side::Long
                && *host_sized->side != Side::Short)) {
            return RequestRejectReason::InvalidQuantity;
        }
    } else {
        return RequestRejectReason::InvalidQuantity;
    }

    const bool market_only = context.surface == CommandSurface::MarketOnly;
    // The market-only surface mirrors execution::Action, which cannot carry
    // any of the late-resolution request forms.
    if (market_only && (reverse_to || host_sized || sized || scope_fraction)) {
        return RequestRejectReason::InvalidQuantity;
    }
    if (market_only && !std::holds_alternative<Market>(request.trigger)) {
        return RequestRejectReason::InvalidTrigger;
    }
    const auto* anchor = std::get_if<FromOwnerFill>(&request.anchor);
    if (market_only && anchor) return RequestRejectReason::InvalidTrigger;
    if (anchor && (!std::isfinite(anchor->offset)
                   || (anchor->ticks && !context.price_tick))) {
        return RequestRejectReason::InvalidTrigger;
    }
    // A rounded anchor snaps onto the run's tick ladder at the arm, so it
    // needs a positive tick at acceptance, exactly like a tick spelling; an
    // unknown rounding is refused rather than read as Raw.
    if (anchor && !valid_anchor_rounding(anchor->rounding)) {
        return RequestRejectReason::InvalidTrigger;
    }
    if (anchor && anchor->rounding != NativeAnchorRounding::Raw
        && !(context.price_tick && finite_positive(*context.price_tick))) {
        return RequestRejectReason::InvalidTrigger;
    }
    if (const auto reason = validate_levels(request.trigger, anchor != nullptr)) return reason;
    // The anchor is delivered by the owner's fill through the arming path,
    // and WaitForApplied is the only relation that arms. A cohort or an
    // already enrolled opening has no later fill event to read a level from.
    if (anchor && !std::holds_alternative<WaitForApplied>(request.owner)) {
        return RequestRejectReason::InvalidOwner;
    }

    const bool flatten = as_flatten(request.intent) != nullptr;
    const bool owner_opened =
            as_reduce(request.intent) && is_owner_opened(*as_reduce(request.intent));
    if (market_only && !std::holds_alternative<ImmediateRemaining>(request.capacity)) {
        return RequestRejectReason::InvalidCapacity;
    }
    if (const auto* budget = std::get_if<PointBudget>(&request.capacity)) {
        if (flatten || reverse_to) return RequestRejectReason::InvalidCapacity;
        if (!finite_positive(budget->units)) return RequestRejectReason::InvalidCapacity;
        if (!on_optional_grid(budget->units, context.quantity_grid)) {
            return RequestRejectReason::OffGrid;
        }
    }

    if ((reverse_to || sized) && !std::holds_alternative<Independent>(request.owner)) {
        return RequestRejectReason::InvalidOwner;
    }
    // A HostSized close may wait for its owner only as a book close: the
    // owner-lot relation has no host-sized quantity.
    const auto* waits = std::get_if<WaitForApplied>(&request.owner);
    if (host_sized
        && ((host_sized->kind == HostSizedKind::Open
             && !std::holds_alternative<Independent>(request.owner))
            || (host_sized->kind == HostSizedKind::Close && waits
                && waits->scope != NativeArmScope::Book))) {
        return RequestRejectReason::InvalidOwner;
    }

    if (market_only && !std::holds_alternative<Independent>(request.owner)) {
        return RequestRejectReason::InvalidOwner;
    }
    if (std::holds_alternative<Independent>(request.owner)) {
    } else if (const auto* wait = std::get_if<WaitForApplied>(&request.owner)) {
        if (replace_target && wait->parent == *replace_target) {
            return RequestRejectReason::InvalidOwner;
        }
        if (!find_live(wait->parent)) return RequestRejectReason::InvalidOwner;
        // An unknown visibility is refused rather than read as Working.
        if (wait->visibility != NativeArmVisibility::Working
            && wait->visibility != NativeArmVisibility::PendingUntilArmed) {
            return RequestRejectReason::InvalidOwner;
        }
        // Likewise an unknown first-match rule is refused, never AtArmPrint.
        if (wait->first_match != NativeArmFirstMatch::AtArmPrint
            && wait->first_match != NativeArmFirstMatch::AfterArmPrint) {
            return RequestRejectReason::InvalidOwner;
        }
        // The arm scope names what a CLOSING child closes; a waiting
        // transaction closes nothing and must not claim a book.
        if (wait->scope != NativeArmScope::OwnerLot && wait->scope != NativeArmScope::Book) {
            return RequestRejectReason::InvalidOwner;
        }
        const bool closing_child = as_reduce(request.intent) || flatten
            || (host_sized && host_sized->kind == HostSizedKind::Close);
        if (wait->scope == NativeArmScope::Book && !closing_child) {
            return RequestRejectReason::InvalidOwner;
        }
    } else if (const auto* bind = std::get_if<BindOpening>(&request.owner)) {
        if (as_transact(request.intent)) return RequestRejectReason::InvalidOwner;
        if (bind->cycle <= 0 || bind->opening.incarnation == 0 || bind->opening.run != identity_) {
            return RequestRejectReason::InvalidOwner;
        }
        if (!context.opening) return RequestRejectReason::InvalidOwner;
        const auto& observation = *context.opening;
        if (!opening_alive(observation, bind->opening, bind->cycle)) {
            return RequestRejectReason::InvalidOwner;
        }
        if (std::holds_alternative<PositionFlat>(observation.current_position)) {
            return RequestRejectReason::InvalidOwner;
        }
    } else if (const auto* bind = std::get_if<BindOpenings>(&request.owner)) {
        if (as_transact(request.intent) || bind->cycle <= 0 || bind->openings.empty()) {
            return RequestRejectReason::InvalidOwner;
        }
        auto cohort = bind->openings;
        std::sort(cohort.begin(), cohort.end(), handle_less);
        for (std::size_t i = 0; i < cohort.size(); ++i) {
            if (cohort[i].incarnation == 0 || cohort[i].run != identity_
                || (i && cohort[i] == cohort[i - 1])) return RequestRejectReason::InvalidOwner;
        }
        if (context.openings.empty()) return RequestRejectReason::InvalidOwner;
        const auto& position = context.openings.front().current_position;
        const auto* nonflat = std::get_if<PositionNonflat>(&position);
        if (!nonflat || nonflat->cycle != bind->cycle) return RequestRejectReason::InvalidOwner;
        std::size_t live_count = 0;
        if (observe_openings(cohort, bind->cycle, nonflat->side, position,
                              context.openings, &live_count)
            || live_count != cohort.size()) return RequestRejectReason::InvalidOwner;
    } else if (const auto* bind = std::get_if<BindCohort>(&request.owner)) {
        if (!host_sized || host_sized->kind != HostSizedKind::Close
            || bind->cohort.value == 0 || cohort_index(bind->cohort) == cohorts_.size()) {
            return RequestRejectReason::InvalidOwner;
        }
    } else {
        return RequestRejectReason::InvalidOwner;
    }

    if (owner_opened) {
        const bool wait_reduce = std::holds_alternative<WaitForApplied>(request.owner)
                && as_reduce(request.intent);
        if (!wait_reduce) return RequestRejectReason::InvalidQuantityBasis;
    }

    if (market_only && !std::holds_alternative<NoGroup>(request.group)) {
        return RequestRejectReason::InvalidGroup;
    }
    if (reverse_to && !std::holds_alternative<NoGroup>(request.group)) {
        return RequestRejectReason::InvalidGroup;
    }
    if (const auto* member = std::get_if<Member>(&request.group)) {
        if (member->group == 0) return RequestRejectReason::InvalidGroup;
        if (member->effect != GroupEffect::Cancel && member->effect != GroupEffect::Reduce) {
            return RequestRejectReason::InvalidGroup;
        }
    }
    return std::nullopt;
}

std::optional<RequestRejectReason> WorkingRequestCore::resolve_tick_spellings(
        Request& request, const CommandContext& context) {
    auto* trail = std::get_if<Trail>(&request.trigger);
    auto* anchor = std::get_if<FromOwnerFill>(&request.anchor);
    const bool trail_ticks = trail != nullptr && trail->ticks.has_value();
    const bool anchor_ticks = anchor != nullptr && anchor->ticks;
    if (!trail_ticks && !anchor_ticks) return std::nullopt;
    const double tick = context.price_tick ? *context.price_tick : 0.0;
    if (!finite_positive(tick)) return RequestRejectReason::InvalidTrigger;
    double trail_offset = 0.0;
    double anchor_offset = 0.0;
    if (trail_ticks) {
        trail_offset = trail->ticks->ticks * tick;
        if (!valid_trail_offset(trail_offset)) return RequestRejectReason::InvalidTrigger;
    }
    if (anchor_ticks) {
        anchor_offset = anchor->offset * tick;
        if (!std::isfinite(anchor_offset)) return RequestRejectReason::InvalidTrigger;
    }
    // Every check is done: nothing below can leave a half-resolved request.
    if (trail_ticks) {
        trail->offset = trail_offset;
        trail->ticks.reset();
    }
    if (anchor_ticks) {
        anchor->offset = anchor_offset;
        anchor->ticks = false;
    }
    return std::nullopt;
}

LiveRequest WorkingRequestCore::make_live(DefinitionRef definition, const CommandContext& context,
                                          EventId accepted) const {
    LiveRequest live;
    live.definition = std::move(definition);
    const Request& request = live.request();
    live.trigger_state = trigger_state_from(request.trigger);
    live.allowance = AllowanceUnset{};
    live.pending = PendingNone{};
    // Placement-time sizing measurements are carried only for the intent that
    // asked to freeze one; every other request leaves both empty. A
    // replacement is a new command, so its successor re-freezes against its
    // own acceptance context rather than inheriting the predecessor's.
    if (const auto* reduce = as_reduce(request.intent)) {
        if (const auto* fraction = fraction_size(*reduce)) {
            if (fraction->basis == ScopeBasis::AtAcceptance) {
                live.sizing_scope = context.sizing_scope;
            }
        }
    } else if (const auto* native_sized = as_sized(request.intent)) {
        if (native_sized->time == SizeTime::AtAcceptance) {
            live.sizing_units = context.sizing_units;
        }
        if (native_sized->price != SizePrice::Resolved) {
            live.sizing_price = context.sizing_price;
        }
    }
    if (std::holds_alternative<Independent>(request.owner)) {
        if (const auto* transact = as_transact(request.intent)) {
            live.remaining = RemainingUnits{std::abs(transact->signed_units)};
            live.authority = BookTransaction{};
        } else if (const auto* reverse_to = as_reverse_to(request.intent)) {
            live.remaining = RemainingUnits{std::abs(reverse_to->signed_units)};
            live.authority = BookTransaction{};
        } else if (const auto* sized = as_host_sized(request.intent)) {
            live.remaining = RemainingDeferred{};
            live.authority = sized->kind == HostSizedKind::Open
                ? Authority{BookTransaction{}}
                : Authority{UnboundBookClose{}};
        } else if (as_sized(request.intent)) {
            // A kernel-sized opening carries a deferred quantity exactly like a
            // HostSized opening, whichever sizing point it asked for.
            // AtAcceptance has already frozen its resolution in live.sizing_units
            // above; the candidate publishes that number to the host and takes
            // the host's own units when it returns any, so both sizing points
            // reach settlement through one terms pass.
            live.remaining = RemainingDeferred{};
            live.authority = BookTransaction{};
        } else if (const auto* reduce = as_reduce(request.intent)) {
            live.remaining = fraction_size(*reduce)
                ? Remaining{RemainingDeferred{}}
                : Remaining{RemainingUnits{explicit_size(*reduce)->units}};
            live.authority = UnboundBookClose{};
        } else {
            live.remaining = RemainingFlattenAll{};
            live.authority = UnboundBookClose{};
        }
        return live;
    }
    if (const auto* wait = std::get_if<WaitForApplied>(&request.owner)) {
        live.authority = Wait{wait->parent};
        if (const auto* transact = as_transact(request.intent)) {
            live.remaining = RemainingUnits{std::abs(transact->signed_units)};
        } else if (const auto* reduce = as_reduce(request.intent)) {
            if (is_owner_opened(*reduce)) live.remaining = RemainingUnbound{};
            else if (fraction_size(*reduce)) live.remaining = RemainingDeferred{};
            else live.remaining = RemainingUnits{explicit_size(*reduce)->units};
        } else if (as_host_sized(request.intent)) {
            // Validated as a Book-scoped close: deferred to the host's terms
            // exactly like an Independent HostSized close.
            live.remaining = RemainingDeferred{};
        } else {
            live.remaining = RemainingFlattenAll{};
        }
        return live;
    }
    if (const auto* bind = std::get_if<BindOpening>(&request.owner)) {
        const auto& nonflat = std::get<PositionNonflat>(context.opening->current_position);
        live.authority = OpeningClose{bind->opening, bind->cycle, nonflat.side,
                                      EnrollmentFromCommand{accepted}};
    } else if (const auto* bind = std::get_if<BindOpenings>(&request.owner)) {
        const auto& nonflat = std::get<PositionNonflat>(context.openings.front().current_position);
        live.authority = OpeningsClose{bind->openings, bind->cycle, nonflat.side,
                                       EnrollmentFromCommand{accepted}};
    } else {
        const auto& cohort = std::get<BindCohort>(request.owner);
        live.authority = CohortClose{cohort.cohort};
        live.remaining = NoTarget{};
        return live;
    }
    if (const auto* sized = as_host_sized(request.intent)) {
        live.remaining = sized->kind == HostSizedKind::Close
            ? Remaining{RemainingDeferred{}}
            : Remaining{RemainingUnbound{}};
    } else if (const auto* reduce = as_reduce(request.intent)) {
        live.remaining = fraction_size(*reduce)
            ? Remaining{RemainingDeferred{}}
            : Remaining{RemainingUnits{explicit_size(*reduce)->units}};
    } else {
        live.remaining = RemainingFlattenAll{};
    }
    return live;
}

bool WorkingRequestCore::trigger_permits_driver(const Trigger& trigger,
                                                const TriggerState& state,
                                                DriverEligibilityClass driver_class,
                                                bool existing_matching_bit) const noexcept {
    const bool market = std::holds_alternative<Market>(trigger)
            && !std::holds_alternative<StopActive>(state)
            && !std::holds_alternative<TrailActive>(state);
    switch (driver_class) {
        case DriverEligibilityClass::ObservedPrint:
            return market ? existing_matching_bit : true;
        case DriverEligibilityClass::CarriedOpen:
            if (market) return existing_matching_bit;
            return std::holds_alternative<Limit>(trigger);
        case DriverEligibilityClass::TickAfterCalculation:
            if (market) return existing_matching_bit;
            return true;
        case DriverEligibilityClass::ConfirmedOpen:
            return market ? existing_matching_bit : true;
        case DriverEligibilityClass::ConfirmedExcursion:
            return !market;
        case DriverEligibilityClass::ConfirmedAfterCalculationClose:
            return market ? existing_matching_bit : false;
        case DriverEligibilityClass::CurrentExecution:
            // Only the consumer's guarded target command supplies this bit.
            return std::holds_alternative<Market>(trigger)
                && std::holds_alternative<MarketReady>(state) && existing_matching_bit;
    }
    return false;
}

bool WorkingRequestCore::working_is_buy(const LiveRequest& live,
                                        std::optional<Side> cohort_side) const noexcept {
    if (const auto* transact = as_transact(live.request().intent)) {
        return transact->signed_units > 0.0;
    }
    if (const auto* reverse_to = as_reverse_to(live.request().intent)) {
        return reverse_to->signed_units > 0.0;
    }
    if (const auto* sized = as_host_sized(live.request().intent)) {
        if (sized->kind == HostSizedKind::Open && sized->side) {
            return *sized->side == Side::Long;
        }
    }
    if (const auto* native_sized = as_sized(live.request().intent)) {
        return native_sized->side == Side::Long;
    }
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        return close->side == Side::Short;
    }
    if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        return close->side == Side::Short;
    }
    if (const auto* close = std::get_if<OpeningsClose>(&live.authority)) {
        return close->side == Side::Short;
    }
    if (std::holds_alternative<CohortClose>(live.authority) && cohort_side) {
        return *cohort_side == Side::Short;
    }
    return false;
}

EligibilityFacts WorkingRequestCore::eligibility_facts(
        const LiveRequest& live, const EvaluationContext& context) const noexcept {
    EligibilityFacts facts;
    facts.trigger = &live.request().trigger;
    facts.trigger_state = &live.trigger_state;
    facts.remaining = &live.remaining;
    facts.allowance = &live.allowance;
    facts.authority = &live.authority;
    facts.waiting = std::holds_alternative<Wait>(live.authority);
    facts.needs_close_bind = std::holds_alternative<UnboundBookClose>(live.authority);
    const uint64_t point = context.cursor.point.ordinal;
    const bool evaluated_at_point = [&] {
        if (const auto* units = std::get_if<AllowanceUnits>(&live.allowance)) {
            return units->point_ordinal == point;
        }
        if (const auto* all = std::get_if<AllowanceAllScope>(&live.allowance)) {
            return all->point_ordinal == point;
        }
        if (const auto* deferred = std::get_if<AllowanceDeferred>(&live.allowance)) {
            return deferred->point_ordinal == point;
        }
        return false;
    }();
    const bool pre_open_delivery = context.pre_open_birth_eligible
        && std::holds_alternative<Market>(live.request().trigger)
        && std::holds_alternative<ImmediateRemaining>(live.request().capacity)
        && context.cursor.point.path_phase == NativePathPhase::Open;
    const bool remaining_path_delivery = context.pre_open_birth_eligible
        && context.cursor.point.path_phase != NativePathPhase::None
        && context.cursor.point.path_phase != NativePathPhase::Open;
    facts.birth_ok = point_eligible(live.birth(), context.cursor.point.ordinal,
                                    context.cursor.point.effective_time_ms)
        || (evaluated_at_point
            && context.cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound)
        || ((pre_open_delivery || remaining_path_delivery)
            && context.cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound);
    if (facts.waiting) {
        facts.driver_ok = false;
        facts.ready_to_match = false;
        facts.is_buy = working_is_buy(live);
        return facts;
    }
    facts.driver_ok = trigger_permits_driver(live.request().trigger, live.trigger_state,
                                            context.driver_class, context.existing_matching_bit);
    if (context.driver_class == DriverEligibilityClass::CurrentExecution
        || context.cursor.point.provenance == NativePriceProvenance::CurrentExecution) {
        facts.driver_ok = facts.driver_ok && current_shape(live)
            && driver_class_matches_cursor(context.driver_class, context.cursor);
    }
    facts.is_buy = working_is_buy(live, context.cohort_side);
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        facts.position_side = close->side;
    } else if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        facts.position_side = close->side;
    } else if (const auto* close = std::get_if<OpeningsClose>(&live.authority)) {
        facts.position_side = close->side;
    } else if (std::holds_alternative<CohortClose>(live.authority) && context.cohort_side) {
        facts.position_side = *context.cohort_side;
    }
    facts.ready_to_match = facts.birth_ok && facts.driver_ok && !facts.waiting;
    return facts;
}

bool WorkingRequestCore::evaluation_eligible(const LiveRequest& live,
                                             const EvaluationContext& context) const noexcept {
    const EligibilityFacts facts = eligibility_facts(live, context);
    return facts.ready_to_match;
}

std::vector<RequestHandle> WorkingRequestCore::waiting_children(const RequestHandle& parent) const {
    std::vector<RequestHandle> handles;
    for (const auto& live : live_) {
        if (const auto* wait = std::get_if<Wait>(&live.authority)) {
            if (wait->parent == parent) handles.push_back(live.handle());
        }
    }
    std::sort(handles.begin(), handles.end(),
              [](const RequestHandle& a, const RequestHandle& b) {
                  return a.incarnation < b.incarnation;
              });
    return handles;
}

bool WorkingRequestCore::has_waiting_children(const RequestHandle& parent) const noexcept {
    for (const auto& live : live_) {
        if (const auto* wait = std::get_if<Wait>(&live.authority)) {
            if (wait->parent == parent) return true;
        }
    }
    return false;
}

std::vector<RequestHandle> WorkingRequestCore::bound_close_handles() const {
    std::vector<RequestHandle> handles;
    for (const auto& live : live_) {
        if (std::holds_alternative<BookClose>(live.authority)
            || std::holds_alternative<OpeningClose>(live.authority)
            || std::holds_alternative<OpeningsClose>(live.authority)) {
            handles.push_back(live.handle());
        }
    }
    std::sort(handles.begin(), handles.end(),
              [](const RequestHandle& a, const RequestHandle& b) {
                  return a.incarnation < b.incarnation;
              });
    return handles;
}

std::vector<RequestHandle> WorkingRequestCore::group_recipients(const EventId& applied) const {
    std::vector<RequestHandle> handles;
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload || !payload->definition) return handles;
    const auto* member = std::get_if<Member>(&payload->definition->request.group);
    if (!member) return handles;
    if (member->effect == GroupEffect::Cancel && !payload->terminal) return handles;
    if (member->effect == GroupEffect::Reduce && !(payload->filled_working > 0.0)) return handles;
    for (const auto& live : live_) {
        const auto* other = std::get_if<Member>(&live.request().group);
        if (!other || other->group != member->group || other->cohort == member->cohort) continue;
        if (live.handle() == payload->handle()) continue;
        if (live.birth().acceptance_ordinal >= payload->ordinal) continue;
        handles.push_back(live.handle());
    }
    std::sort(handles.begin(), handles.end(),
              [](const RequestHandle& a, const RequestHandle& b) {
                  return a.incarnation < b.incarnation;
              });
    return handles;
}

PreparedSubmit WorkingRequestCore::prepare_submit(const Request& request,
                                                  const CommandContext& context,
                                                  uint64_t& next_order_incarnation,
                                                  uint64_t& next_timeline_ordinal,
                                                  RequestOrigin origin) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    Request staged = request;
    MutationPlan plan = begin_plan();
    auto reason = validate_request(staged, context, std::nullopt);
    if (!reason) reason = resolve_tick_spellings(staged, context);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    auto impl = std::make_unique<PreparedSubmit::Impl>();
    if (reason) {
        impl->result = SubmitResult{SubmitStatus::Rejected, ordinal, std::nullopt, reason};
        plan.events.emplace_back(
                RejectedEvent{ordinal, std::move(staged), *reason, context.surface});
        seal_plan(plan);
        impl->plan = std::move(plan);
        impl->result.event_ordinal = ordinal;
        return PreparedSubmit(std::move(impl));
    }
    canonicalize_owner(staged);
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle handle{identity_, incarnation};
    Birth birth{ordinal, context.decision_time_ms};
    auto definition = std::make_shared<RequestDefinition>(
            RequestDefinition{handle, std::move(staged), birth, std::nullopt, origin});
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    AcceptedEvent accepted;
    accepted.ordinal = ordinal;
    accepted.definition = definition;
    accepted.surface = context.surface;
    plan.events.emplace_back(std::move(accepted));
    plan.live_change = kLivePush;
    plan.live_row = std::move(live);
    plan.consume_incarnation = true;
    plan.incarnation_used = incarnation;
    seal_plan(plan);
    impl->plan = std::move(plan);
    impl->result = SubmitResult{SubmitStatus::Accepted, ordinal, handle, std::nullopt};
    return PreparedSubmit(std::move(impl));
}

PreparedReplace WorkingRequestCore::prepare_replace(const RequestHandle& target,
                                                    const Request& request,
                                                    const CommandContext& context,
                                                    uint64_t& next_order_incarnation,
                                                    uint64_t& next_timeline_ordinal,
                                                    ReplaceOptions options) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    RequestHandle staged_target = target;
    Request staged = request;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    auto impl = std::make_unique<PreparedReplace::Impl>();
    if (kind != TargetKind::Live) {
        const ReplaceStatus status = kind == TargetKind::InvalidHandle
                ? ReplaceStatus::InvalidHandle
                : ReplaceStatus::NotWorking;
        impl->result = ReplaceResult{status, ordinal, std::nullopt, std::nullopt};
        if (kind == TargetKind::InvalidHandle) {
            plan.events.emplace_back(InvalidHandleEvent{ordinal, std::move(staged_target),
                                                        std::move(staged), context.surface});
        } else {
            plan.events.emplace_back(NotWorkingEvent{ordinal, std::move(staged_target),
                                                     std::move(staged), context.surface});
        }
        seal_plan(plan);
        impl->plan = std::move(plan);
        return PreparedReplace(std::move(impl));
    }
    auto reason = validate_request(staged, context, staged_target);
    if (!reason) reason = resolve_tick_spellings(staged, context);
    // A retained trigger state only describes the trigger alternative it came
    // from, and a retained trail best must still produce a representable
    // level for the successor's offset on either side.
    if (!reason && options.retain_trigger_state) {
        const LiveRequest& predecessor = live_[live_index];
        if (predecessor.request().trigger.index() != staged.trigger.index()) {
            reason = RequestRejectReason::InvalidTrigger;
        } else {
            std::optional<double> best;
            if (const auto* track = std::get_if<TrailTrack>(&predecessor.trigger_state)) {
                best = track->best;
            } else if (const auto* active = std::get_if<TrailActive>(&predecessor.trigger_state)) {
                best = active->best_at_trigger;
            }
            const auto* trail = std::get_if<Trail>(&staged.trigger);
            // Representability only, on both sides, and a command carries no
            // activation grid: the ladder spelling of a level never changes
            // this verdict, because a ladder point a whole tick from the best
            // is finite and strictly past it exactly when the subtraction is.
            if (best && (!trail || !trail_level_ok(*best, trail->offset, true, nullptr)
                         || !trail_level_ok(*best, trail->offset, false, nullptr))) {
                reason = RequestRejectReason::InvalidTrigger;
            }
        }
    }
    if (reason) {
        impl->result = ReplaceResult{ReplaceStatus::ReplaceRejected, ordinal, std::nullopt, reason};
        ReplaceRejectedEvent rejected;
        rejected.ordinal = ordinal;
        rejected.live_definition = live_[live_index].definition;
        rejected.attempted = std::move(staged);
        rejected.reason = *reason;
        rejected.surface = context.surface;
        plan.events.emplace_back(std::move(rejected));
        seal_plan(plan);
        impl->plan = std::move(plan);
        return PreparedReplace(std::move(impl));
    }
    canonicalize_owner(staged);
    const TriggerState retained = live_[live_index].trigger_state;
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle successor{identity_, incarnation};
    Birth birth{ordinal, context.decision_time_ms};
    auto definition = std::make_shared<RequestDefinition>(
            RequestDefinition{successor, std::move(staged), birth, staged_target});
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    if (options.retain_trigger_state) live.trigger_state = retained;
    ReplacedEvent replaced;
    replaced.ordinal = ordinal;
    replaced.predecessor_definition = live_[live_index].definition;
    replaced.successor_definition = definition;
    replaced.surface = context.surface;
    plan.events.emplace_back(std::move(replaced));
    plan.live_change = kLiveErasePush;
    plan.live_index = live_index;
    plan.live_row = std::move(live);
    plan.consume_incarnation = true;
    plan.incarnation_used = incarnation;
    seal_plan(plan);
    impl->plan = std::move(plan);
    impl->result = ReplaceResult{ReplaceStatus::Replaced, ordinal, successor, std::nullopt};
    return PreparedReplace(std::move(impl));
}

PreparedCancel WorkingRequestCore::prepare_cancel(const RequestHandle& target,
                                                  uint64_t& next_timeline_ordinal,
                                                  CancelReason reason) {
    require_identity(identity_);
    RequestHandle staged_target = target;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    auto impl = std::make_unique<PreparedCancel::Impl>();
    if (kind != TargetKind::Live) {
        const CancelStatus status = kind == TargetKind::InvalidHandle
                ? CancelStatus::InvalidHandle
                : CancelStatus::NotWorking;
        impl->result = CancelResult{status, ordinal};
        if (kind == TargetKind::InvalidHandle) {
            plan.events.emplace_back(
                    InvalidHandleEvent{ordinal, std::move(staged_target), std::nullopt});
        } else {
            plan.events.emplace_back(
                    NotWorkingEvent{ordinal, std::move(staged_target), std::nullopt});
        }
        seal_plan(plan);
        impl->plan = std::move(plan);
        return PreparedCancel(std::move(impl));
    }
    impl->result = CancelResult{CancelStatus::Cancelled, ordinal};
    plan.events.emplace_back(make_cancelled(ordinal, live_[live_index], reason,
                                            EventId{identity_, ordinal}));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    seal_plan(plan);
    impl->plan = std::move(plan);
    return PreparedCancel(std::move(impl));
}

InstalledCommand<SubmitResult> WorkingRequestCore::install_submit(PreparedSubmit&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto installed = commit(prepared.impl_->plan);
    if (const auto* error = std::get_if<InstallError>(&installed)) return *error;
    return CommandInstalled<SubmitResult>{std::move(prepared.impl_->result),
                                          std::get<Installed>(installed).events};
}

InstalledCommand<ReplaceResult> WorkingRequestCore::install_replace(PreparedReplace&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto installed = commit(prepared.impl_->plan);
    if (const auto* error = std::get_if<InstallError>(&installed)) return *error;
    return CommandInstalled<ReplaceResult>{std::move(prepared.impl_->result),
                                           std::get<Installed>(installed).events};
}

InstalledCommand<CancelResult> WorkingRequestCore::install_cancel(PreparedCancel&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto installed = commit(prepared.impl_->plan);
    if (const auto* error = std::get_if<InstallError>(&installed)) return *error;
    return CommandInstalled<CancelResult>{std::move(prepared.impl_->result),
                                          std::get<Installed>(installed).events};
}

SubmitResult WorkingRequestCore::submit(const Request& request,
                                        int64_t decision_time_ms,
                                        uint64_t& next_order_incarnation,
                                        uint64_t& next_timeline_ordinal,
                                        std::optional<double> quantity_grid) {
    auto prepared = prepare_submit(request, CommandContext{decision_time_ms, quantity_grid,
                                                           std::nullopt, CommandSurface::General, {}},
                                   next_order_incarnation, next_timeline_ordinal);
    auto installed = install_submit(std::move(prepared));
    if (std::holds_alternative<InstallError>(installed)) {
        throw std::logic_error("native submit install failed");
    }
    auto& ok = std::get<CommandInstalled<SubmitResult>>(installed);
    ++next_timeline_ordinal;
    if (ok.result.status == SubmitStatus::Accepted) ++next_order_incarnation;
    return std::move(ok.result);
}

ReplaceResult WorkingRequestCore::replace(const RequestHandle& target,
                                          const Request& request,
                                          int64_t decision_time_ms,
                                          uint64_t& next_order_incarnation,
                                          uint64_t& next_timeline_ordinal,
                                          std::optional<double> quantity_grid,
                                          ReplaceOptions options) {
    auto prepared = prepare_replace(target, request,
                                    CommandContext{decision_time_ms, quantity_grid, std::nullopt,
                                                    CommandSurface::General, {}},
                                    next_order_incarnation, next_timeline_ordinal, options);
    auto installed = install_replace(std::move(prepared));
    if (std::holds_alternative<InstallError>(installed)) {
        throw std::logic_error("native replace install failed");
    }
    auto& ok = std::get<CommandInstalled<ReplaceResult>>(installed);
    ++next_timeline_ordinal;
    if (ok.result.status == ReplaceStatus::Replaced) ++next_order_incarnation;
    return std::move(ok.result);
}

CancelResult WorkingRequestCore::cancel(const RequestHandle& target,
                                        uint64_t& next_timeline_ordinal) {
    auto prepared = prepare_cancel(target, next_timeline_ordinal);
    auto installed = install_cancel(std::move(prepared));
    if (std::holds_alternative<InstallError>(installed)) {
        throw std::logic_error("native cancel install failed");
    }
    auto& ok = std::get<CommandInstalled<CancelResult>>(installed);
    ++next_timeline_ordinal;
    return std::move(ok.result);
}

InstallResult WorkingRequestCore::install_mutation(PreparedMutation&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    return commit(prepared.impl_->plan);
}

namespace {

Allowance initialize_allowance(const Remaining& remaining, const Capacity& capacity, uint64_t point) {
    if (std::holds_alternative<RemainingFlattenAll>(remaining)) return AllowanceAllScope{point};
    if (std::holds_alternative<RemainingUnbound>(remaining)) return AllowanceUnset{};
    if (std::holds_alternative<RemainingDeferred>(remaining)) return AllowanceDeferred{point};
    if (std::holds_alternative<NoTarget>(remaining)) return AllowanceDeferred{point};
    const double q = working_units(remaining);
    double initial = q;
    if (const auto* budget = std::get_if<PointBudget>(&capacity)) initial = std::min(q, budget->units);
    return AllowanceUnits{point, initial, initial};
}

bool same_point_allowance(const Allowance& allowance, uint64_t point) noexcept {
    if (const auto* units = std::get_if<AllowanceUnits>(&allowance)) {
        return units->point_ordinal == point;
    }
    if (const auto* all = std::get_if<AllowanceAllScope>(&allowance)) {
        return all->point_ordinal == point;
    }
    if (const auto* deferred = std::get_if<AllowanceDeferred>(&allowance)) {
        return deferred->point_ordinal == point;
    }
    return false;
}

}  // namespace

Allowance WorkingRequestCore::evaluated_allowance(const LiveRequest& live,
                                                  uint64_t point) noexcept {
    return initialize_allowance(live.remaining, live.request().capacity, point);
}

void WorkingRequestCore::refresh_point_allowances(uint64_t point,
                                                 const PositionIdentity& position) noexcept {
    for (auto& live : live_) {
        if (same_point_allowance(live.allowance, point)) continue;
        if (const auto* close = std::get_if<BookClose>(&live.authority)) {
            const auto* nonflat = std::get_if<PositionNonflat>(&position);
            if (nonflat && nonflat->cycle == close->cycle && nonflat->side == close->side) {
                live.allowance = evaluated_allowance(live, point);
            }
        }
    }
}

bool WorkingRequestCore::refresh_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) return false;
    LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)) return false;
    const EligibilityFacts facts = eligibility_facts(live, context);
    if (!facts.birth_ok || !facts.driver_ok) return false;
    if (std::holds_alternative<CohortClose>(live.authority)) {
        if (!context.cohort_side) return false;
        bool has_live_member = false;
        for (const auto& opening : observation.openings)
            has_live_member = has_live_member || opening.has_live_matching_lot;
        if (!has_live_member) return false;
    } else if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        if (!book_close_alive(observation, *close)) return false;
    } else if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        if (!opening_close_alive(observation, *close)) return false;
    } else if (const auto* close = std::get_if<OpeningsClose>(&live.authority)) {
        std::size_t live_count = 0;
        if (observe_openings(observation, *close, &live_count) || live_count == 0) return false;
    } else if (std::holds_alternative<UnboundBookClose>(live.authority)) {
        return false;
    }
    if (same_point_allowance(live.allowance, context.cursor.point.ordinal)) {
        return false;
    }
    if (epoch_ > std::numeric_limits<uint64_t>::max() - 2U) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
    live.allowance = evaluated_allowance(live, context.cursor.point.ordinal);
    if (!bump_epoch() || !bump_epoch()) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
    return true;
}

bool WorkingRequestCore::refresh_cohort_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation) {
    return refresh_allowance(target, context, observation);
}

bool WorkingRequestCore::effective_host_units(const PendingAdjustments& pending,
                                              double resolved_units,
                                              double* deduction,
                                              double* after,
                                              bool* exhausted) noexcept {
    if (!deduction || !after || !exhausted || !std::isfinite(resolved_units)
        || resolved_units < 0.0) {
        return false;
    }
    double pending_total = 0.0;
    if (const auto* deferred = std::get_if<PendingDeferred>(&pending)) {
        pending_total = deferred->total;
        if (!std::isfinite(pending_total) || pending_total < 0.0) return false;
    }

    const double computed_deduction = std::min(pending_total, resolved_units);
    double computed_after = resolved_units;
    bool computed_exhausted = computed_deduction == resolved_units;
    if (resolved_units == 0.0 || computed_exhausted) {
        computed_after = 0.0;
    } else if (computed_deduction > 0.0) {
        if (!checked_sub_cap(resolved_units, computed_deduction, &computed_after,
                             &computed_exhausted)) {
            return false;
        }
    }

    *deduction = computed_deduction;
    *after = computed_after;
    *exhausted = computed_exhausted;
    return true;
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_evaluation(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)) {
        return NoChange{NoChangeReason::StillWaiting};
    }
    const EligibilityFacts facts = eligibility_facts(live, context);
    if (!facts.birth_ok || !facts.driver_ok) return NoChange{NoChangeReason::NotEligible};

    if (std::holds_alternative<CohortClose>(live.authority)) {
        bool has_live_member = false;
        for (const auto& opening : observation.openings) {
            has_live_member = has_live_member || opening.has_live_matching_lot;
        }
        if (!context.cohort_side || !has_live_member) {
            // No receipt and no group effect: this is the durable NoTarget
            // deferral marker, retried at the next match candidate.
            return NoChange{NoChangeReason::StillWaiting};
        }
    }

    if (std::holds_alternative<UnboundBookClose>(live.authority)) {
        if (std::holds_alternative<PositionFlat>(observation.current_position)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            NoEffectEvent none;
            none.ordinal = ordinal;
            none.definition = live.definition;
            none.remaining = project_remaining(live.remaining);
            none.authority = live.authority;
            none.cursor = context.cursor;
            plan.events.emplace_back(std::move(none));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
        const auto* nonflat = std::get_if<PositionNonflat>(&observation.current_position);
        if (!nonflat || nonflat->cycle <= 0) {
            return PreparationError{CoreFailure::MissingObservation, EventId{identity_, 0}, target};
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        MutationPlan plan = begin_plan();
        LiveRequest updated = live;
        BookClose bound;
        bound.cycle = nonflat->cycle;
        bound.side = nonflat->side;
        bound.binding_event = EventId{identity_, ordinal};
        bound.binding_cursor = context.cursor;
        CloseBoundEvent bound_event;
        bound_event.ordinal = ordinal;
        bound_event.definition = live.definition;
        bound_event.cycle = bound.cycle;
        bound_event.side = bound.side;
        bound_event.cursor = context.cursor;
        bound_event.before = live.authority;
        bound_event.after = bound;
        updated.authority = bound;
        updated.allowance = evaluated_allowance(live, context.cursor.point.ordinal);
        plan.events.emplace_back(std::move(bound_event));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    }

    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        if (!book_close_alive(observation, *close)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                    EventId{identity_, ordinal}));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
    }
    if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        if (!opening_close_alive(observation, *close)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                    EventId{identity_, ordinal}));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
    }

    if (const auto* close = std::get_if<OpeningsClose>(&live.authority)) {
        std::size_t live_count = 0;
        if (const auto error = observe_openings(observation, *close, &live_count)) {
            return PreparationError{*error, EventId{identity_, 0}, target};
        }
        if (live_count == 0) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                    EventId{identity_, ordinal}));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
    }

    if (same_point_allowance(live.allowance, context.cursor.point.ordinal)) {
        return NoChange{NoChangeReason::NoTransition};
    }
    LiveRequest updated = live;
    updated.allowance = evaluated_allowance(live, context.cursor.point.ordinal);
    MutationPlan plan = begin_plan();
    plan.live_change = kLiveUpdate;
    plan.live_index = live_index;
    plan.live_row = std::move(updated);
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_trigger(
        const RequestHandle& target,
        const TriggerTransition& transition,
        DriverEligibilityClass driver_class,
        uint64_t& next_timeline_ordinal,
        std::optional<Side> cohort_side,
        const ActivationGrid& activation_grid) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest updated = live_[live_index];
    if (std::holds_alternative<Wait>(updated.authority)
        || std::holds_alternative<UnboundBookClose>(updated.authority)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const MatchCursor& cursor = transition_cursor(transition);
    if (!trigger_cursor_eligible(updated, cursor)
        && !same_point_allowance(updated.allowance, cursor.point.ordinal)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    if (!driver_class_matches_cursor(driver_class, cursor)
        || !trigger_permits_driver(updated.request().trigger, updated.trigger_state,
                                    driver_class, false)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const bool is_buy = working_is_buy(updated, cohort_side);
    const native_matching::GridThreshold grid = matcher_grid(activation_grid);
    // The run's own tick ladder, which is a separate fact from whether prints
    // are tested on it: a trailing level spelled a whole number of ticks away
    // names a ladder point under either mode (R5 lane E16).
    const double ladder_tick = activation_grid.ladder_tick;
    MutationPlan plan = begin_plan();

    auto emit_activated = [&](ActivationKind kind, TriggerState after, const MatchCursor& cursor,
                              double price) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        ActivatedEvent activated;
        activated.ordinal = ordinal;
        activated.definition = updated.definition;
        activated.kind = kind;
        activated.before = updated.trigger_state;
        activated.after = after;
        activated.reached_price = price;
        activated.cursor = cursor;
        updated.trigger_state = after;
        plan.events.emplace_back(std::move(activated));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    };

    if (const auto* begin = std::get_if<BeginTrailTracking>(&transition)) {
        if (!std::holds_alternative<TrailWaitArm>(updated.trigger_state)) {
            if (std::holds_alternative<TrailTrack>(updated.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        if (!std::isfinite(begin->reached_price)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        const auto* trail = std::get_if<Trail>(&updated.request().trigger);
        if (!trail || !trail_level_ok(begin->reached_price, trail->offset, is_buy, nullptr,
                                      ladder_tick)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        // The arm threshold is reached from the favourable side: a sell trail
        // arms at or above it, a buy trail at or below (le == is_buy).
        if (trail->arm_price
            && !stop_price_reached(!is_buy, *trail->arm_price, begin->reached_price, grid)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        // The print the arm happened at is the arm's quantized print: the
        // arm level's ladder point on a crossing, the print's tick otherwise
        // (the print itself without a grid, already checked above).
        const double arm_print = trail->arm_price
            ? native_matching::grid_reached_print(begin->reached_price, *trail->arm_price,
                                                  /*le=*/is_buy, grid)
            : native_matching::grid_best_print(begin->reached_price, is_buy, grid);
        // The best the trail starts riding is that print, floored by the
        // host's seed when it named one: the favourable one of the two, with
        // the seed put on the ladder exactly like an observed print so the
        // running best stays on it. The activation still reports the print
        // it happened at, not the seed.
        double best = arm_print;
        if (trail->best_seed) {
            const double seed =
                native_matching::grid_best_print(*trail->best_seed, is_buy, grid);
            best = is_buy ? std::min(best, seed) : std::max(best, seed);
        }
        if (best != begin->reached_price
            && !trail_level_ok(best, trail->offset, is_buy, nullptr, ladder_tick)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::TrailArm, TrailTrack{best}, begin->cursor, arm_print);
    }
    if (const auto* extremum = std::get_if<ObserveTrailExtremum>(&transition)) {
        auto* track = std::get_if<TrailTrack>(&updated.trigger_state);
        if (!track) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        // The running best follows the quantized path: the observed print's
        // tick, nearest under HalfUp and the favourable enclosing tick under
        // Directional, so a print that reads the same tick as the best is no
        // improvement (grid_best_print is the identity without a grid).
        const double print = native_matching::grid_best_print(extremum->reached_price, is_buy, grid);
        const double next_best = is_buy ? std::min(track->best, print)
                                        : std::max(track->best, print);
        if (next_best == track->best) return NoChange{NoChangeReason::NoTransition};
        const auto* trail = std::get_if<Trail>(&updated.request().trigger);
        if (!trail || !trail_level_ok(next_best, trail->offset, is_buy, nullptr,
                                      ladder_tick)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        track->best = next_best;
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    }
    if (const auto* stop = std::get_if<ActivateStop>(&transition)) {
        if (!std::holds_alternative<StopIdle>(updated.trigger_state)) {
            if (std::holds_alternative<StopActive>(updated.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const auto* stop_px = std::get_if<Stop>(&updated.request().trigger);
        if (!stop_px || !stop_price_reached(is_buy, stop_px->price, stop->reached_price, grid)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::Stop, StopActive{}, stop->cursor,
                              native_matching::grid_reached_print(
                                  stop->reached_price, stop_px->price, /*le=*/!is_buy, grid));
    }
    if (const auto* stop_limit = std::get_if<ActivateStopLimit>(&transition)) {
        if (!std::holds_alternative<StopLimitPending>(updated.trigger_state)) {
            if (std::holds_alternative<StopLimitLive>(updated.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const auto* stop_limit_px = std::get_if<StopLimit>(&updated.request().trigger);
        if (!stop_limit_px
            || !stop_price_reached(is_buy, stop_limit_px->stop, stop_limit->reached_price, grid)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::StopLimit, StopLimitLive{}, stop_limit->cursor,
                              native_matching::grid_reached_print(
                                  stop_limit->reached_price, stop_limit_px->stop,
                                  /*le=*/!is_buy, grid));
    }
    const auto& trail_hit = std::get<ActivateTrail>(transition);
    const auto* track = std::get_if<TrailTrack>(&updated.trigger_state);
    if (!track) {
        if (std::holds_alternative<TrailActive>(updated.trigger_state)) {
            return NoChange{NoChangeReason::NoTransition};
        }
        return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0}, target};
    }
    const auto* trail = std::get_if<Trail>(&updated.request().trigger);
    double level = 0.0;
    if (!trail || !trail_level_ok(track->best, trail->offset, is_buy, &level, ladder_tick)
        || !stop_price_reached(is_buy, level, trail_hit.reached_price, grid)) {
        return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0}, target};
    }
    return emit_activated(ActivationKind::TrailTrigger, TrailActive{track->best}, trail_hit.cursor,
                          native_matching::grid_reached_print(
                              trail_hit.reached_price, level, /*le=*/!is_buy, grid));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_no_effect(
        const RequestHandle& target,
        const EvaluationContext& context,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    NoEffectEvent none;
    none.ordinal = ordinal;
    none.definition = live.definition;
    none.remaining = project_remaining(live.remaining);
    none.authority = live.authority;
    none.cursor = context.cursor;
    plan.events.emplace_back(std::move(none));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_match_rejected(
        const RequestHandle& target,
        const EvaluationContext& context,
        MatchRejectReason reason,
        uint64_t& next_timeline_ordinal) {
    return prepare_match_rejected(target, context, reason, std::nullopt,
                                  next_timeline_ordinal);
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_match_rejected(
        const RequestHandle& target,
        const EvaluationContext& context,
        MatchRejectReason reason,
        std::optional<ExecutionTerms> attempted_terms,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    MatchRejectedEvent rejected;
    rejected.ordinal = ordinal;
    rejected.reason = reason;
    rejected.definition = live.definition;
    rejected.remaining = project_remaining(live.remaining);
    rejected.authority = live.authority;
    rejected.cursor = context.cursor;
    rejected.attempted_terms = std::move(attempted_terms);
    plan.events.emplace_back(std::move(rejected));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_margin_call(
        const MarginCallEvent& event, uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    if (!event.definition || event.definition->handle.run != identity_) {
        return PreparationError{CoreFailure::InvalidProposal, event.applied,
                                event.definition ? event.definition->handle : RequestHandle{}};
    }
    if (event.definition->origin == RequestOrigin::Host) {
        return PreparationError{CoreFailure::InvalidProposal, event.applied,
                                event.definition->handle};
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    MarginCallEvent receipt = event;
    receipt.ordinal = ordinal;
    plan.events.emplace_back(std::move(receipt));
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_risk_event(
        const NativeRiskEvent& event, uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    NativeRiskEvent receipt = event;
    receipt.ordinal = ordinal;
    plan.events.emplace_back(std::move(receipt));
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_terms(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TermsResolvedInput& input,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const bool deferred = std::holds_alternative<RemainingDeferred>(live.remaining)
        || std::holds_alternative<NoTarget>(live.remaining);
    const bool has_units = input.terms.units.has_value();

    // A price-only receipt is meaningful only after a target has a concrete
    // remaining quantity. It deliberately does not authenticate an unrelated
    // pending chain or rewrite any live state.
    if (!deferred) {
        if (has_units || input.terms.shape != OpeningShape::Transact) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        MutationPlan plan = begin_plan();
        TermsResolvedEvent receipt;
        receipt.ordinal = ordinal;
        receipt.definition = live.definition;
        receipt.cursor = context.cursor;
        receipt.input = input;
        receipt.remaining_before = project_remaining(live.remaining);
        receipt.remaining_after = project_remaining(live.remaining);
        receipt.allowance_after = live.allowance;
        plan.events.emplace_back(std::move(receipt));
        return finish_mutation(std::move(plan));
    }

    if (!has_units || !std::isfinite(*input.terms.units) || *input.terms.units < 0.0) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    const auto* sized = as_host_sized(live.request().intent);
    const auto* native_sized = as_sized(live.request().intent);
    const auto* scope_fraction = deferred_reduction(live.request().intent);
    // Only an OPENING may name a nondefault physical shape. A host-sized close
    // and a scope fraction both settle through the ordinary Transact/Reduce
    // plan; a kernel-sized opening serves the same reversal shapes a
    // HostSized{Open} does, with the kernel's own units on the declared side.
    const bool opening_shapes = (sized && sized->kind == HostSizedKind::Open)
        || native_sized != nullptr;
    if ((!sized && !native_sized && !scope_fraction)
        || (input.terms.shape != OpeningShape::Transact
            && input.terms.shape != OpeningShape::ReverseTo
            && input.terms.shape != OpeningShape::CloseOpposite)
        || (!opening_shapes && input.terms.shape != OpeningShape::Transact)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    std::vector<EventId> prior_adjustment_ids;
    double pending_total = 0.0;
    if (!collect_pending_chain(live.pending, target, &prior_adjustment_ids, &pending_total)) {
        return PreparationError{CoreFailure::ConflictingReceipt, EventId{identity_, 0}, target};
    }

    double deduction = 0.0;
    double after = 0.0;
    bool exhausted = false;
    if (!effective_host_units(live.pending, *input.terms.units, &deduction, &after, &exhausted)) {
        return PreparationError{CoreFailure::UnrepresentableReservation, EventId{identity_, 0},
                                target};
    }

    const Allowance bound_allowance = initialize_allowance(
        RemainingUnits{after}, live.request().capacity, context.cursor.point.ordinal);
    if (input.terms.shape != OpeningShape::Transact) {
        const auto* allowance = std::get_if<AllowanceUnits>(&bound_allowance);
        if (!allowance || after > allowance->left) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    }

    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    LiveRequest updated = live;
    updated.remaining = RemainingUnits{after};
    updated.allowance = bound_allowance;
    updated.pending = PendingNone{};

    TermsResolvedEvent receipt;
    receipt.ordinal = ordinal;
    receipt.definition = live.definition;
    receipt.cursor = context.cursor;
    receipt.input = input;
    receipt.prior_adjustment_ids = std::move(prior_adjustment_ids);
    receipt.pending_total = pending_total;
    receipt.effective_deduction = deduction;
    receipt.remaining_before = project_remaining(live.remaining);
    receipt.remaining_after = project_remaining(updated.remaining);
    receipt.allowance_after = updated.allowance;

    MutationPlan plan = begin_plan();
    plan.events.emplace_back(std::move(receipt));
    if (*input.terms.units == 0.0) {
        const uint64_t terminal_ordinal = ordinal + 1;
        if (terminal_ordinal == 0 || terminal_ordinal == std::numeric_limits<uint64_t>::max()
            || terminal_ordinal <= last_ordinal_) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
        NoEffectEvent none;
        none.ordinal = terminal_ordinal;
        none.definition = live.definition;
        none.remaining = project_remaining(updated.remaining);
        none.authority = updated.authority;
        none.cursor = context.cursor;
        plan.events.emplace_back(std::move(none));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
        return finish_mutation(std::move(plan));
    }
    if (after == 0.0 && deduction > 0.0) {
        const uint64_t terminal_ordinal = ordinal + 1;
        if (terminal_ordinal == 0 || terminal_ordinal == std::numeric_limits<uint64_t>::max()
            || terminal_ordinal <= last_ordinal_) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
        plan.events.emplace_back(make_cancelled(terminal_ordinal, updated, CancelReason::Group,
                                                EventId{identity_, ordinal}));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
        return finish_mutation(std::move(plan));
    }

    (void) exhausted;
    plan.live_change = kLiveUpdate;
    plan.live_index = live_index;
    plan.live_row = std::move(updated);
    return finish_mutation(std::move(plan));
}

Preparation<PreparedExecution> WorkingRequestCore::prepare_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    if (std::holds_alternative<RemainingDeferred>(live.remaining)
        || std::holds_alternative<NoTarget>(live.remaining)
        || std::holds_alternative<AllowanceDeferred>(live.allowance)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    if (std::holds_alternative<Wait>(live.authority)
        || std::holds_alternative<UnboundBookClose>(live.authority)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const bool birth_ok = point_eligible(live.birth(), proposal.cursor.point.ordinal,
                                         proposal.cursor.point.effective_time_ms)
        || (same_point_allowance(live.allowance, proposal.cursor.point.ordinal)
            && proposal.cursor.point.effective_time_ms
                >= live.birth().decision_time_lower_bound)
        || (proposal.pre_open_birth_eligible
            && std::holds_alternative<Market>(live.request().trigger)
            && std::holds_alternative<ImmediateRemaining>(live.request().capacity)
            && proposal.cursor.point.path_phase == NativePathPhase::Open
            && proposal.cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound);
    if (!birth_ok) {
        return NoChange{NoChangeReason::NotEligible};
    }
    if (!fillable_state(live.trigger_state)) return NoChange{NoChangeReason::NotEligible};
    const bool current = proposal.cursor.point.provenance == NativePriceProvenance::CurrentExecution;
    if (current && !current_shape(live)) return NoChange{NoChangeReason::NotEligible};

    const uint64_t point = proposal.cursor.point.ordinal;
    double allowance_left = 0.0;
    const bool has_units_allowance = std::holds_alternative<AllowanceUnits>(live.allowance);
    const bool has_all_allowance = std::holds_alternative<AllowanceAllScope>(live.allowance);
    if (has_units_allowance) {
        const auto& units = std::get<AllowanceUnits>(live.allowance);
        if (units.point_ordinal != point) return NoChange{NoChangeReason::NotEligible};
        allowance_left = units.left;
        if (!(allowance_left > 0.0) && !std::holds_alternative<RemainingFlattenAll>(live.remaining)) {
            return NoChange{NoChangeReason::NotEligible};
        }
    } else if (has_all_allowance) {
        if (std::get<AllowanceAllScope>(live.allowance).point_ordinal != point) {
            return NoChange{NoChangeReason::NotEligible};
        }
    } else {
        return NoChange{NoChangeReason::NotEligible};
    }

    const bool flatten = as_flatten(live.request().intent) != nullptr;
    const bool reduce = as_reduce(live.request().intent) != nullptr;
    const auto* transact = as_transact(live.request().intent);
    const auto* reverse_to = as_reverse_to(live.request().intent);
    const auto* host_sized = as_host_sized(live.request().intent);
    const auto* native_sized = as_sized(live.request().intent);
    const bool plan_flatten = std::holds_alternative<execution::Flatten>(proposal.physical_action);
    const auto* plan_reduce = std::get_if<order_action::Reduce>(&proposal.physical_action);
    const auto* plan_transact = std::get_if<order_action::Transact>(&proposal.physical_action);
    const auto* plan_reverse = std::get_if<execution::ReverseTo>(&proposal.physical_action);
    double cap = flatten ? 0.0 : working_units(live.remaining);
    if (has_units_allowance) cap = std::min(cap, allowance_left);

    if (flatten) {
        if (!plan_flatten) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (reduce) {
        if (!plan_reduce || !finite_positive(plan_reduce->units) || plan_reduce->units > cap) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (transact) {
        if (!plan_transact || !finite_nonzero(plan_transact->signed_units)
            || ((plan_transact->signed_units > 0.0) != (transact->signed_units > 0.0))
            || std::abs(plan_transact->signed_units) > cap) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (reverse_to) {
        if (!plan_reverse || !finite_nonzero(plan_reverse->signed_units)
            || plan_reverse->signed_units != reverse_to->signed_units) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (host_sized) {
        if (host_sized->kind == HostSizedKind::Close) {
            if (!plan_reduce || !finite_positive(plan_reduce->units) || plan_reduce->units > cap) {
                return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
            }
        } else if (host_sized->kind == HostSizedKind::Open && host_sized->side) {
            const bool long_side = *host_sized->side == Side::Long;
            if (plan_transact) {
                if (!finite_nonzero(plan_transact->signed_units)
                    || ((plan_transact->signed_units > 0.0) != long_side)
                    || std::abs(plan_transact->signed_units) > cap) {
                    return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
                }
            } else if (plan_reverse) {
                if (!finite_nonzero(plan_reverse->signed_units)
                    || ((plan_reverse->signed_units > 0.0) != long_side)
                    || !has_units_allowance
                    || std::abs(plan_reverse->signed_units) != working_units(live.remaining)
                    || std::abs(plan_reverse->signed_units) != allowance_left) {
                    return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
                }
            } else if (plan_reduce) {
                if (!finite_positive(plan_reduce->units) || !has_units_allowance
                    || plan_reduce->units != working_units(live.remaining)
                    || plan_reduce->units != allowance_left) {
                    return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
                }
            } else if (plan_flatten) {
                if (!has_units_allowance
                    || working_units(live.remaining) != allowance_left) {
                    return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
                }
            } else {
                return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
            }
        } else {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (native_sized) {
        // A kernel-sized opening settles as a signed book transaction on its
        // declared side, or -- when the terms named one -- through the same
        // ReverseTo / CloseOpposite shapes a HostSized{Open} may name, with the
        // kernel's own units as the opening on that side.
        const bool long_side = native_sized->side == Side::Long;
        if (plan_transact) {
            if (!finite_nonzero(plan_transact->signed_units)
                || ((plan_transact->signed_units > 0.0) != long_side)
                || std::abs(plan_transact->signed_units) > cap) {
                return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
            }
        } else if (plan_reverse) {
            if (!finite_nonzero(plan_reverse->signed_units)
                || ((plan_reverse->signed_units > 0.0) != long_side)
                || !has_units_allowance
                || std::abs(plan_reverse->signed_units) != working_units(live.remaining)
                || std::abs(plan_reverse->signed_units) != allowance_left) {
                return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
            }
        } else if (plan_reduce) {
            if (!finite_positive(plan_reduce->units) || !has_units_allowance
                || plan_reduce->units != working_units(live.remaining)
                || plan_reduce->units != allowance_left) {
                return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
            }
        } else if (plan_flatten) {
            if (!has_units_allowance || working_units(live.remaining) != allowance_left) {
                return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
            }
        } else {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    const bool opening_auth = std::holds_alternative<OpeningClose>(live.authority);
    const auto* selected_auth = std::get_if<OpeningsClose>(&live.authority);
    const auto* cohort_auth = std::get_if<CohortClose>(&live.authority);
    ExecutionScope canonical_scope = proposal.scope;
    if (opening_auth) {
        const auto& close = std::get<OpeningClose>(live.authority);
        const auto* scope = std::get_if<execution::OpeningExposure>(&proposal.scope);
        if (!scope || scope->incarnation != close.opening.incarnation || scope->cycle != close.cycle) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
    } else if (selected_auth) {
        std::size_t live_count = 0;
        if (const auto error = observe_openings(proposal.pre_target, *selected_auth, &live_count)) {
            return PreparationError{*error, EventId{identity_, 0}, target};
        }
        if (!same_position(proposal.pre_fill, proposal.pre_target.current_position)) {
            return PreparationError{CoreFailure::ObservationMismatch, EventId{identity_, 0}, target};
        }
        auto* scope = std::get_if<SelectedExposure>(&canonical_scope);
        if (!scope || scope->cycle != selected_auth->cycle || live_count == 0
            || scope->incarnations.size() != live_count) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
        std::sort(scope->incarnations.begin(), scope->incarnations.end());
        if (std::adjacent_find(scope->incarnations.begin(), scope->incarnations.end())
            != scope->incarnations.end()) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
        for (const auto& observation : proposal.pre_target.openings) {
            if (observation.has_live_matching_lot
                && !std::binary_search(scope->incarnations.begin(), scope->incarnations.end(),
                                        observation.queried_opening.incarnation)) {
                return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
            }
        }
        if (proposal.inspected_opened_units != 0.0) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (cohort_auth) {
        const auto* position = std::get_if<PositionNonflat>(&proposal.pre_target.current_position);
        auto* scope = std::get_if<SelectedExposure>(&canonical_scope);
        if (!position || position->cycle <= 0 || !scope || scope->cycle != position->cycle
            || !same_position(proposal.pre_fill, proposal.pre_target.current_position)
            || scope->incarnations.empty()) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
        std::sort(scope->incarnations.begin(), scope->incarnations.end());
        if (std::adjacent_find(scope->incarnations.begin(), scope->incarnations.end())
            != scope->incarnations.end()) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
        std::size_t live_count = 0;
        for (const auto& observation : proposal.pre_target.openings) {
            if (!cohort_contains(cohort_auth->cohort, observation.queried_opening)) {
                return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
            }
            if (!same_position(observation.current_position, proposal.pre_target.current_position)) {
                return PreparationError{CoreFailure::ObservationMismatch, EventId{identity_, 0}, target};
            }
            if (observation.has_live_matching_lot) {
                ++live_count;
                if (!std::binary_search(scope->incarnations.begin(), scope->incarnations.end(),
                                        observation.queried_opening.incarnation)) {
                    return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
                }
            }
        }
        if (live_count == 0 || scope->incarnations.size() != live_count
            || proposal.inspected_opened_units != 0.0) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
    } else if (std::holds_alternative<BookTransaction>(live.authority)
               || std::holds_alternative<ArmedTransaction>(live.authority)
               || std::holds_alternative<BookClose>(live.authority)) {
        if (!std::holds_alternative<execution::Book>(proposal.scope)) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
    } else {
        return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
    }

    const bool reversal_plan = plan_reverse != nullptr;
    const bool host_open = host_sized && host_sized->kind == HostSizedKind::Open;
    const bool whole_host_flatten = host_open && plan_flatten;
    if ((reversal_plan || (host_open && !plan_transact))
        && (!std::holds_alternative<BookTransaction>(live.authority)
            || !std::holds_alternative<execution::Book>(canonical_scope))) {
        return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
    }

    if (!std::isfinite(proposal.inspected_closed_units) || proposal.inspected_closed_units < 0.0
        || !std::isfinite(proposal.inspected_opened_units)
        || !std::isfinite(proposal.resolved_price)
        || (proposal.resolved_price <= 0.0
            && !(current && proposal.inspected_opened_units == 0.0))) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    if (reversal_plan) {
        if (!has_units_allowance || !finite_nonzero(plan_reverse->signed_units)
            || ((proposal.inspected_opened_units > 0.0) != (plan_reverse->signed_units > 0.0))
            || proposal.inspected_opened_units != plan_reverse->signed_units
            || std::abs(proposal.inspected_opened_units) != working_units(live.remaining)
            || std::abs(proposal.inspected_opened_units) != allowance_left) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    }
    if (host_open && plan_reduce
        && (proposal.inspected_opened_units != 0.0
            || proposal.inspected_closed_units != plan_reduce->units)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    if (whole_host_flatten && proposal.inspected_opened_units != 0.0) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    double filled = proposal.inspected_closed_units;
    if (plan_transact) {
        if (!checked_add_positive(proposal.inspected_closed_units,
                                  std::abs(proposal.inspected_opened_units), &filled)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
    } else if (reversal_plan) {
        filled = proposal.inspected_closed_units + std::abs(proposal.inspected_opened_units);
        if (!std::isfinite(filled)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
    }
    if (!(filled > 0.0) || !std::isfinite(filled)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    const bool special_turnover = reversal_plan || whole_host_flatten;
    if (!flatten && !special_turnover && filled > working_units(live.remaining)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    if (has_units_allowance && !special_turnover && filled > allowance_left) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    RemainingProjection remaining_after = RemainingProjectionUnits{};
    if (flatten) remaining_after = RemainingProjectionFlattenAll{};
    bool units_exhausted = flatten || special_turnover;
    if (!flatten) {
        if (special_turnover) {
            remaining_after = RemainingProjectionUnits{0.0};
        } else {
            double after = 0.0;
            if (!checked_sub_cap(working_units(live.remaining),
                                 std::min(filled, working_units(live.remaining)),
                                 &after, &units_exhausted)) {
                if (filled != working_units(live.remaining)) {
                    return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                            target};
                }
                units_exhausted = true;
                after = 0.0;
            }
            remaining_after = RemainingProjectionUnits{after};
        }
    }

    Allowance allowance_after = live.allowance;
    if (has_units_allowance) {
        auto units = std::get<AllowanceUnits>(live.allowance);
        bool allow_ex = false;
        double left = 0.0;
        const double allowance_deduction = reversal_plan
            ? std::abs(proposal.inspected_opened_units)
            : (whole_host_flatten ? working_units(live.remaining) : filled);
        if (allowance_deduction == units.left) {
            units.left = 0.0;
        } else if (!checked_sub_cap(units.left, allowance_deduction, &left, &allow_ex)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        } else {
            units.left = left;
        }
        allowance_after = units;
    }

    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    ExecutionAppliedEvent base;
    base.ordinal = ordinal;
    base.definition = live.definition;
    base.raw_price = proposal.raw_price;
    base.resolved_price = proposal.resolved_price;
    base.current_ticket = proposal.inspected_current_ticket;
    base.closed_units = proposal.inspected_closed_units;
    base.opened_units = proposal.inspected_opened_units;
    base.filled_working = filled;
    base.remaining_before = project_remaining(live.remaining);
    base.allowance_before = live.allowance;
    base.allowance_after = allowance_after;
    base.scope = std::move(canonical_scope);
    fill_applied_cursor(base, proposal.cursor);

    ExecutionAppliedEvent terminal = base;
    terminal.terminal = true;
    terminal.remaining_after = flatten ? RemainingProjectionFlattenAll{}
            : (units_exhausted ? RemainingProjectionUnits{0.0}
                               : remaining_after);
    terminal.terminal_reason = flatten ? AppliedTerminalReason::Flattened
            : (units_exhausted ? AppliedTerminalReason::WorkingUnitsSatisfied
                               : AppliedTerminalReason::TargetExhausted);

    ExecutionAppliedEvent retain = base;
    retain.terminal = false;
    retain.remaining_after = remaining_after;
    retain.terminal_reason = std::nullopt;

    MutationPlan plan = begin_plan();
    plan.events.emplace_back(terminal);
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    seal_plan(plan);

    auto impl = std::make_unique<PreparedExecution::Impl>();
    impl->terminal = std::move(plan);
    impl->retain_event = CommandEvent{std::move(retain)};
    impl->can_retain = !flatten && !units_exhausted;
    if (impl->can_retain) {
        LiveRequest kept = live;
        kept.remaining = RemainingUnits{std::get<RemainingProjectionUnits>(remaining_after).q};
        kept.allowance = allowance_after;
        impl->retain_live = std::move(kept);
    }
    impl->flatten = flatten;
    impl->opening = opening_auth;
    if (opening_auth) impl->opening_close = std::get<OpeningClose>(live.authority);
    if (selected_auth) impl->openings_close = *selected_auth;
    if (cohort_auth) impl->cohort_close = *cohort_auth;
    if (std::holds_alternative<BookClose>(live.authority)) {
        impl->book_close = std::get<BookClose>(live.authority);
    }
    impl->proposal = proposal;
    return PreparedExecution(std::move(impl));
}

InstallResult WorkingRequestCore::install_execution(PreparedExecution&& prepared,
                                                    const CommittedExecutionFacts& facts) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto& impl = *prepared.impl_;
    if (const auto error = validate_plan(impl.terminal)) return *error;
    if (facts.result.status != execution::Status::Applied) return InstallError::WrongCoreOrRun;
    if (facts.result.closed_units != impl.proposal.inspected_closed_units
        || facts.result.opened_units != impl.proposal.inspected_opened_units
        || facts.result.current_ticket != impl.proposal.inspected_current_ticket) {
        return InstallError::WrongCoreOrRun;
    }

    auto stamp = [&](ExecutionAppliedEvent& event) {
        event.current_ticket = impl.proposal.inspected_current_ticket;
        event.first_trade_index = facts.result.first_trade_index;
        event.closed_trade_count = facts.result.closed_trade_count;
        event.opened_lot_incarnation = facts.result.opened_lot_incarnation;
        event.cycle_before = facts.cycle_before;
        event.cycle_after = facts.cycle_after;
    };

    bool retain = impl.can_retain;
    if (impl.openings_close) {
        std::size_t live_count = 0;
        if (observe_openings(facts.post_target, *impl.openings_close, &live_count)) {
            return InstallError::WrongCoreOrRun;
        }
        if (live_count == 0) retain = false;
    }
    if (retain && impl.opening && !opening_close_alive(facts.post_target, impl.opening_close)) {
        retain = false;
    }
    if (retain && std::holds_alternative<BookClose>(impl.retain_live.authority)
        && !book_close_alive(facts.post_target, impl.book_close)) {
        retain = false;
    }

    if (retain) {
        auto* event = std::get_if<ExecutionAppliedEvent>(&impl.retain_event);
        if (!event) return InstallError::WrongCoreOrRun;
        stamp(*event);
        impl.terminal.events.clear();
        impl.terminal.events.push_back(std::move(impl.retain_event));
        impl.terminal.live_change = kLiveUpdate;
        impl.terminal.live_row = std::move(impl.retain_live);
        return commit(impl.terminal);
    }

    auto* event = std::get_if<ExecutionAppliedEvent>(&impl.terminal.events.front());
    if (!event) return InstallError::WrongCoreOrRun;
    stamp(*event);
    if (impl.can_retain && event->terminal_reason == AppliedTerminalReason::TargetExhausted) {
        // already staged
    } else if (impl.can_retain) {
        event->terminal = true;
        event->terminal_reason = AppliedTerminalReason::TargetExhausted;
    }
    impl.terminal.live_change = kLiveErase;
    return commit(impl.terminal);
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_group_effect(
        const EventId& applied,
        const RequestHandle& recipient,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload || !payload->definition) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
    }
    const auto* member = std::get_if<Member>(&payload->definition->request.group);
    if (!member) return NoChange{NoChangeReason::NoTransition};
    if (member->effect == GroupEffect::Cancel && !payload->terminal) {
        return NoChange{NoChangeReason::NoTransition};
    }
    if (member->effect == GroupEffect::Reduce && !(payload->filled_working > 0.0)) {
        return NoChange{NoChangeReason::NoTransition};
    }
    uint64_t seen = 0;
    const ReceiptLookup lookup = receipt_lookup(applied, recipient, member->effect, &seen);
    if (lookup == ReceiptLookup::Present) return NoChange{NoChangeReason::AlreadyApplied};
    if (lookup == ReceiptLookup::Conflict) {
        return PreparationError{CoreFailure::ConflictingReceipt, applied, recipient};
    }
    if (!receipts_.empty()) {
        const auto& last = receipts_.back();
        if (receipt_cmp(applied.ordinal, recipient.incarnation, member->effect, last.cause.ordinal,
                        last.recipient.incarnation, last.effect)
            < 0) {
            return PreparationError{CoreFailure::InvalidCause, applied, recipient};
        }
    }
    std::size_t live_index = 0;
    if (classify(recipient, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const auto* other = std::get_if<Member>(&live.request().group);
    if (!other || other->group != member->group || other->cohort == member->cohort
        || live.birth().acceptance_ordinal >= payload->ordinal) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
    }

    MutationPlan plan = begin_plan();
    plan.add_receipt = true;
    plan.receipt_cause = applied;
    plan.receipt_recipient = recipient;
    plan.receipt_effect = member->effect;

    if (std::holds_alternative<RemainingUnbound>(live.remaining)
        || std::holds_alternative<RemainingDeferred>(live.remaining)
        || std::holds_alternative<NoTarget>(live.remaining)) {
        if (member->effect == GroupEffect::Cancel) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::Group, applied));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            plan.receipt_outcome = ordinal;
            return finish_mutation(std::move(plan));
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        PendingDeferred after;
        PendingAdjustments before = live.pending;
        if (const auto* pending = std::get_if<PendingDeferred>(&live.pending)) {
            if (!checked_add_positive(pending->total, payload->filled_working, &after.total)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
            }
            after.count = pending->count + 1;
            after.tail_receipt = EventId{identity_, ordinal};
        } else {
            after.total = payload->filled_working;
            after.count = 1;
            after.tail_receipt = EventId{identity_, ordinal};
            if (!std::isfinite(after.total) || !(after.total > 0.0)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
            }
        }
        DeferredGroupAdjustmentEvent deferred;
        deferred.ordinal = ordinal;
        deferred.definition = live.definition;
        deferred.cause = applied;
        deferred.recipient = recipient;
        deferred.effect = GroupEffect::Reduce;
        deferred.deferred_delta = payload->filled_working;
        deferred.pending_before = before;
        deferred.pending_after = after;
        if (const auto* pending = std::get_if<PendingDeferred>(&before)) {
            deferred.previous_pending_receipt = pending->tail_receipt;
        }
        LiveRequest updated = live;
        updated.pending = after;
        plan.events.emplace_back(std::move(deferred));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        plan.receipt_outcome = ordinal;
        return finish_mutation(std::move(plan));
    }

    if (member->effect == GroupEffect::Cancel
        || std::holds_alternative<RemainingFlattenAll>(live.remaining)) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::Group, applied));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
        plan.receipt_outcome = ordinal;
        return finish_mutation(std::move(plan));
    }

    const double before_q = working_units(live.remaining);
    const double deduct = std::min(payload->filled_working, before_q);
    double after_q = 0.0;
    bool exhausted = false;
    if (!checked_sub_cap(before_q, deduct, &after_q, &exhausted)) {
        return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    ReservationReducedEvent reduced;
    reduced.ordinal = ordinal;
    reduced.definition = live.definition;
    reduced.cause = applied;
    reduced.recipient = recipient;
    reduced.effect = GroupEffect::Reduce;
    reduced.requested_delta = payload->filled_working;
    reduced.actual_deduction = deduct;
    reduced.before = RemainingUnits{before_q};
    reduced.after = RemainingProjectionUnits{after_q};
    plan.events.emplace_back(std::move(reduced));
    plan.receipt_outcome = ordinal;
    if (exhausted) {
        uint64_t cancel_ord = ordinal + 1;
        if (cancel_ord == 0 || cancel_ord == std::numeric_limits<uint64_t>::max()
            || cancel_ord <= last_ordinal_) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
        LiveRequest snapshot = live;
        snapshot.remaining = RemainingUnits{before_q};
        plan.events.emplace_back(make_cancelled(cancel_ord, snapshot, CancelReason::Group, applied));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
    } else {
        LiveRequest updated = live;
        updated.remaining = RemainingUnits{after_q};
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
    }
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_owner_applied(
        const EventId& applied,
        const RequestHandle& child,
        const std::optional<OpeningObservation>& observation,
        uint64_t& next_timeline_ordinal,
        const ArmContext& arm) {
    require_identity(identity_);
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload) return PreparationError{CoreFailure::InvalidCause, applied, child};
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const auto* wait = std::get_if<Wait>(&live.authority);
    if (!wait || wait->parent != payload->handle()) {
        return NoChange{NoChangeReason::NoTransition};
    }
    if (payload->ordinal <= live.birth().acceptance_ordinal) {
        return NoChange{NoChangeReason::NoTransition};
    }
    const auto* host_close = as_host_sized(live.request().intent);
    const bool closing = as_reduce(live.request().intent) || as_flatten(live.request().intent)
        || (host_close && host_close->kind == HostSizedKind::Close);
    const bool opened = payload->opened_units != 0.0;
    if (closing && !opened && !payload->terminal) {
        return NoChange{NoChangeReason::StillWaiting};
    }
    if (closing && !opened && payload->terminal) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        MutationPlan plan = begin_plan();
        plan.events.emplace_back(
                make_cancelled(ordinal, live, CancelReason::UnsupportedRelation, applied));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
        return finish_mutation(std::move(plan));
    }

    MutationPlan plan = begin_plan();
    LiveRequest updated = live;
    // An anchored leg receives its level from this fill, once, at the arm
    // that binds it to the owner. Every later reader sees the absolute level
    // in the live definition and in the ArmedEvent that installed it.
    DefinitionRef armed_definition = live.definition;
    if (std::holds_alternative<FromOwnerFill>(live.request().anchor)) {
        RequestDefinition materialized{live.definition->handle, live.request(),
                                       live.definition->birth, live.definition->predecessor};
        // A closing leg trades against the lot this fill opened; a waiting
        // transaction has its own side. working_is_buy cannot answer for a
        // Wait authority, which has no bound scope yet.
        const bool leg_is_buy = closing ? !(payload->opened_units > 0.0)
                                        : working_is_buy(live);
        double level = 0.0;
        if (!anchored_kernel_level(materialized.request, payload->resolved_price, arm.price_tick,
                                   leg_is_buy, &level)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, applied, child};
        }
        // The host's one restatement, consulted here and nowhere else: the
        // installed level below is what the ArmedEvent carries.
        if (arm.resolve_level) {
            const auto& anchor = std::get<FromOwnerFill>(materialized.request.anchor);
            const auto restated = arm.resolve_level(
                    live, *payload, leg_is_buy ? Side::Long : Side::Short, anchor.offset, level);
            if (restated) level = *restated;
        }
        if (!install_anchored_level(materialized.request, level)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, applied, child};
        }
        armed_definition = std::make_shared<RequestDefinition>(std::move(materialized));
        updated.definition = armed_definition;
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    if (!closing) {
        ArmedTransaction armed;
        armed.parent = wait->parent;
        armed.cause = applied;
        armed.cause_cursor = payload->cursor;
        ArmedEvent armed_event;
        armed_event.ordinal = ordinal;
        armed_event.definition = armed_definition;
        armed_event.before = live.authority;
        armed_event.after = armed;
        armed_event.enrollment = EnrollmentFromApplied{applied, payload->cursor};
        updated.authority = armed;
        plan.events.emplace_back(std::move(armed_event));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    }

    if (!observation) {
        return PreparationError{CoreFailure::MissingObservation, applied, child};
    }
    const int64_t cycle = payload->cycle_after;
    const Side side = side_from_opened(payload->opened_units);
    if (!opening_alive(*observation, payload->handle(), cycle, &side)) {
        return PreparationError{CoreFailure::ObservationMismatch, applied, child};
    }

    Remaining remaining = live.remaining;
    std::optional<EventId> quantity_resolution;
    const bool owner_opened =
            as_reduce(live.request().intent) && is_owner_opened(*as_reduce(live.request().intent));
    if (owner_opened) {
        const double source = std::abs(payload->opened_units);
        if (!finite_positive(source)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, applied, child};
        }
        std::vector<EventId> ids;
        double pending_total = 0.0;
        if (!collect_pending_chain(live.pending, child, &ids, &pending_total)) {
            return PreparationError{CoreFailure::ConflictingReceipt, applied, child};
        }
        const double deduct = std::min(pending_total, source);
        double after = source;
        bool exhausted = deduct == source;
        if (deduct > 0.0 && !exhausted) {
            if (!checked_sub_cap(source, deduct, &after, &exhausted)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, child};
            }
        } else if (exhausted) {
            after = 0.0;
        }
        QuantityBoundEvent bound;
        bound.ordinal = ordinal;
        bound.definition = live.definition;
        bound.source = applied;
        bound.source_units = source;
        bound.prior_adjustment_ids = ids;
        bound.pending_total = pending_total;
        bound.effective_deduction = deduct;
        bound.remaining = RemainingProjectionUnits{after};
        plan.events.emplace_back(std::move(bound));
        quantity_resolution = EventId{identity_, ordinal};
        updated.pending = PendingNone{};
        if (exhausted) {
            uint64_t cancel_ord = ordinal + 1;
            if (cancel_ord <= last_ordinal_ || cancel_ord == 0) {
                throw std::invalid_argument("native timeline ordinal reused or regressed");
            }
            plan.events.emplace_back(make_cancelled(cancel_ord, live, CancelReason::Group, applied));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
        remaining = RemainingUnits{after};
        updated.remaining = remaining;
    }

    const uint64_t arm_ord = owner_opened ? ordinal + 1 : ordinal;
    if (owner_opened) {
        if (arm_ord <= last_ordinal_ || arm_ord == 0) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
    }
    const EnrollmentFromApplied enrollment{applied, payload->cursor};
    Authority armed_authority;
    const auto* waits = std::get_if<WaitForApplied>(&live.request().owner);
    if (waits && waits->scope == NativeArmScope::Book) {
        // The arm is the binding: the position this fill left, at its cursor.
        BookClose book;
        book.cycle = cycle;
        book.side = side;
        book.binding_event = EventId{identity_, arm_ord};
        book.binding_cursor = payload->cursor;
        armed_authority = book;
    } else {
        OpeningClose close;
        close.opening = payload->handle();
        close.cycle = cycle;
        close.side = side;
        close.enrollment = enrollment;
        armed_authority = close;
    }
    ArmedEvent armed_event;
    armed_event.ordinal = arm_ord;
    armed_event.definition = armed_definition;
    armed_event.before = live.authority;
    armed_event.after = armed_authority;
    armed_event.enrollment = enrollment;
    armed_event.quantity_resolution = quantity_resolution;
    updated.authority = armed_authority;
    if (!owner_opened) {
        plan.events.emplace_back(std::move(armed_event));
    } else {
        plan.events.emplace_back(std::move(armed_event));
    }
    plan.live_change = kLiveUpdate;
    plan.live_index = live_index;
    plan.live_row = std::move(updated);
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_bound_expiry(
        const EventId& physical_cause,
        const RequestHandle& child,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    bool gone = false;
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        gone = !book_close_alive(observation, *close);
    } else if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        gone = !opening_close_alive(observation, *close);
    } else if (const auto* close = std::get_if<OpeningsClose>(&live.authority)) {
        std::size_t live_count = 0;
        if (const auto error = observe_openings(observation, *close, &live_count)) {
            return PreparationError{*error, physical_cause, child};
        }
        gone = live_count == 0;
    } else {
        return NoChange{NoChangeReason::NoTransition};
    }
    if (!gone) return NoChange{NoChangeReason::NoTransition};
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone, physical_cause));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_parent_terminal(
        const EventId& terminal_or_replaced,
        const RequestHandle& child,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const CommandEvent* event = event_at(terminal_or_replaced);
    if (!event) {
        return PreparationError{CoreFailure::InvalidCause, terminal_or_replaced, child};
    }
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const auto* wait = std::get_if<Wait>(&live.authority);
    if (!wait) return NoChange{NoChangeReason::NoTransition};

    bool parent_ended = false;
    if (const auto* cancelled = std::get_if<CancelledEvent>(event)) {
        parent_ended = cancelled->handle() == wait->parent;
    } else if (const auto* replaced = std::get_if<ReplacedEvent>(event)) {
        parent_ended = replaced->predecessor() == wait->parent;
    } else if (const auto* none = std::get_if<NoEffectEvent>(event)) {
        parent_ended = none->handle() == wait->parent;
    } else if (const auto* rejected = std::get_if<MatchRejectedEvent>(event)) {
        parent_ended = rejected->handle() == wait->parent;
    } else if (const auto* applied = as_applied(*event)) {
        parent_ended = applied->handle() == wait->parent && applied->terminal;
    }
    if (!parent_ended) return NoChange{NoChangeReason::NoTransition};

    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    plan.events.emplace_back(
            make_cancelled(ordinal, live, CancelReason::OwnerGone, terminal_or_replaced));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

static_assert(std::is_nothrow_move_constructible_v<PreparedSubmit>);
static_assert(std::is_nothrow_move_constructible_v<PreparedReplace>);
static_assert(std::is_nothrow_move_constructible_v<PreparedCancel>);
static_assert(std::is_nothrow_move_constructible_v<PreparedMutation>);
static_assert(std::is_nothrow_move_constructible_v<PreparedExecution>);
// Authority/scope classification above must be reviewed when an alternative
// is introduced; an unhandled value must never acquire Book authority.
static_assert(std::variant_size_v<Owner> == 5);
static_assert(std::variant_size_v<Authority> == 8);
static_assert(std::variant_size_v<ExecutionScope> == 3);

}  // inline namespace native_order_v6
}  // namespace pineforge::native_order
