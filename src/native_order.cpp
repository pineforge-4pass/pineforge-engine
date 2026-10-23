#include <pineforge/native_order.hpp>
#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/detail/journal_codec.hpp>
#include "native_journal_codec.hpp"
#include <pineforge/native_host.hpp>
#include "native_execution_consumer.hpp"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "native_matching.hpp" // Price grids share the matcher's value-only geometry.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

namespace pineforge::native_order {
inline namespace native_order_v7 {

namespace {
using ReadStatus = detail::ScratchStatus;

struct OpeningFile {
    detail::ScratchFile storage;
    OpeningFile() = default;
    OpeningFile(const OpeningFile&) = delete;
    OpeningFile& operator=(const OpeningFile&) = delete;
    ReadStatus open(const std::string& directory) noexcept {
        try {
            storage.set_directory(directory);
            storage.open("pineforge-openings");
            return ReadStatus::ok;
        } catch (...) {
            return ReadStatus::io_error;
        }
    }
};

ReadStatus write_opening_word(detail::ScratchFile& file, std::uint64_t index,
                             std::uint64_t value) noexcept {
    if (index > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / 8)
        return ReadStatus::programming_error;
    std::array<unsigned char, 8> bytes{};
    for (unsigned i = 0; i != 8; ++i)
        bytes[i] = static_cast<unsigned char>(value >> (i * 8));
    try {
        file.write(index * 8, bytes.data(), bytes.size());
    } catch (...) {
        return ReadStatus::io_error;
    }
    return ReadStatus::ok;
}

ReadStatus read_opening_word(const detail::ScratchFile& file, std::uint64_t index,
                            std::uint64_t& value) noexcept {
    if (index > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / 8)
        return ReadStatus::programming_error;
    std::array<unsigned char, 8> bytes{};
    try {
        file.read(index * 8, bytes.data(), bytes.size());
    } catch (...) {
        return ReadStatus::io_error;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    return ReadStatus::ok;
}

class FrozenOpenings final : public OpeningReadback {
public:
    FrozenOpenings(RunIdentity run, std::uint64_t count) noexcept
        : run_(run), count_(count) {}
    std::uint64_t count() const noexcept override { return count_; }
    ReadStatus opening_at(std::uint64_t index, RequestHandle& out) const noexcept override {
        if (index >= count_) return ReadStatus::not_found;
        std::uint64_t incarnation = 0;
        const auto status = read_opening_word(file.storage, index, incarnation);
        if (status == ReadStatus::ok) out = RequestHandle{run_, incarnation};
        return status;
    }
    OpeningFile file;
private:
    RunIdentity run_;
    std::uint64_t count_;
};
} // namespace

ReadStatus OpeningInputCursor::opening_at(
        std::uint64_t index, RequestHandle& out) const noexcept {
    if (index >= count_) return ReadStatus::not_found;
    if (!reader_) return ReadStatus::programming_error;
    RequestHandle value;
    try {
        const auto status = reader_(context_, index, value);
        if (status == ReadStatus::ok) out = value;
        return status;
    } catch (...) {
        return ReadStatus::programming_error;
    }
}

static ReadStatus freeze_openings_in_directory(
        const RunIdentity& run, const OpeningReadback& input,
        std::shared_ptr<const OpeningReadback>& out, const std::string& directory) {
    if (input.count() == 0
        || input.count() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / 8)
        return ReadStatus::programming_error;
    // The owner is fixed size; only the anonymous file grows with membership.
    auto frozen = std::make_shared<FrozenOpenings>(run, input.count());
    auto status = frozen->file.open(directory);
    if (status != ReadStatus::ok) return status;
    OpeningFile work;
    status = work.open(directory);
    if (status != ReadStatus::ok) return status;
    for (std::uint64_t i = 0; i != input.count(); ++i) {
        RequestHandle handle;
        status = input.opening_at(i, handle);
        if (status != ReadStatus::ok) return status;
        if (!handle.incarnation || handle.run != run) return ReadStatus::programming_error;
        status = write_opening_word(frozen->file.storage, i, handle.incarnation);
        if (status != ReadStatus::ok) return status;
    }
    // Bottom-up external merge sort. Two words of merge state, no population
    // sized array, recursive stack, or source callback during a write.
    const auto count = input.count();
    for (std::uint64_t width = 1; width < count;) {
        for (std::uint64_t start = 0; start < count;) {
            const auto middle = start + std::min(width, count - start);
            const auto end = middle + std::min(width, count - middle);
            auto left = start;
            auto right = middle;
            for (auto dest = start; dest != end; ++dest) {
                std::uint64_t a = 0, b = 0;
                if (left < middle) {
                    status = read_opening_word(frozen->file.storage, left, a);
                    if (status != ReadStatus::ok) return status;
                }
                if (right < end) {
                    status = read_opening_word(frozen->file.storage, right, b);
                    if (status != ReadStatus::ok) return status;
                }
                const bool take_left = right == end || (left < middle && a <= b);
                auto value = take_left ? a : b;
                if (take_left) ++left;
                else ++right;
                status = write_opening_word(work.storage, dest, value);
                if (status != ReadStatus::ok) return status;
            }
            start = end;
        }
        std::swap(work.storage, frozen->file.storage);
        width = width > count / 2 ? count : width * 2;
    }
    std::uint64_t previous = 0;
    for (std::uint64_t i = 0; i != count; ++i) {
        std::uint64_t value = 0;
        status = read_opening_word(frozen->file.storage, i, value);
        if (status != ReadStatus::ok) return status;
        if (i && value == previous) return ReadStatus::programming_error;
        previous = value;
    }
    out = std::move(frozen);
    return ReadStatus::ok;
}

ReadStatus freeze_openings(const RunIdentity& run, const OpeningReadback& input,
                           std::shared_ptr<const OpeningReadback>& out) {
    // Standalone readbacks use ScratchFile's TMPDIR policy. Core-owned
    // freezes below also honor the core's explicitly configured directory.
    return freeze_openings_in_directory(run, input, out, {});
}

std::uint64_t opening_count(const std::vector<RequestHandle>& owned,
                            const OpeningReadback* external) noexcept {
    return external ? external->count() : owned.size();
}

ReadStatus opening_at(const std::vector<RequestHandle>& owned,
                      const OpeningReadback* external, std::uint64_t index,
                      RequestHandle& out) noexcept {
    if (external) return external->opening_at(index, out);
    if (index >= owned.size()) return ReadStatus::not_found;
    out = owned[static_cast<std::size_t>(index)];
    return ReadStatus::ok;
}

std::uint64_t opening_observation_count(const TargetObservation& observation) noexcept {
    return observation.external_openings
        ? observation.external_openings->count() : observation.openings.size();
}

ReadStatus opening_observation_at(const TargetObservation& observation,
                                 std::uint64_t index, OpeningObservation& out) noexcept {
    if (observation.external_openings)
        return observation.external_openings->observation_at(index, out);
    if (index >= observation.openings.size()) return ReadStatus::not_found;
    out = observation.openings[static_cast<std::size_t>(index)];
    return ReadStatus::ok;
}
namespace {

struct ObservationRows {
    const TargetObservation& source;
    struct Iterator {
        const TargetObservation* source;
        std::uint64_t index;
        bool operator!=(const Iterator& other) const noexcept { return index != other.index; }
        Iterator& operator++() noexcept { ++index; return *this; }
        OpeningObservation operator*() const {
            OpeningObservation value;
            if (opening_observation_at(*source, index, value) != ReadStatus::ok)
                throw detail::ScratchIoError(EIO, "native opening observation read");
            return value;
        }
    };
    Iterator begin() const noexcept { return {&source, 0}; }
    Iterator end() const noexcept { return {&source, opening_observation_count(source)}; }
};

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

// The working units a Transact fill consumed. A fill that crosses the book
// closes the opposite position and opens the rest on the plan's side, and the
// settlement computes that rest in binary64 as the plan's units minus the
// closed part (the FIFO split's `requested - closed`, taken as the opening).
// Added back, closed + |opened| can round one ulp either side of the units, so
// such a fill is charged exactly the plan's units. Every other fill keeps the
// checked sum, which is exact when one part is zero.
bool transact_filled_units(double plan_units, double closed, double opened,
                           double* out) noexcept {
    const double units = std::abs(plan_units);
    if (closed > 0.0 && opened != 0.0 && closed < units
        && std::abs(opened) == units - closed) {
        *out = units;
        return true;
    }
    return checked_add_positive(closed, std::abs(opened), out);
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

// A group deduction binary64 cannot take off what it is taken from: `deduct`
// is below `before`, and fl(before - deduct) is `before` again -- at most half
// an ulp of it (R5 lane K-ULP5). The deduction is absorbed: the units stay as
// they are, which is the exact binary64 result of the subtraction, and the
// event that records it deducts nothing.
bool absorbed_deduction(double before, double deduct) noexcept {
    return std::isfinite(before) && std::isfinite(deduct) && deduct > 0.0 && deduct < before
        && before - deduct == before;
}

// A deferred recipient's pending total is the binary64 sum of the fills
// deducted into it, in commit order. A delta the total absorbs is not added
// (absorbed_pending_delta); a total below half an ulp of a later delta rounds
// into it, as the addition does; only a sum that overflows is unrepresentable
// (R5 lane K-ULP5; until then both absorbing sums stopped the run too).
bool pending_sum(double total, double delta, double* out) noexcept {
    if (!std::isfinite(total) || !std::isfinite(delta) || total < 0.0 || !(delta > 0.0)) {
        return false;
    }
    const double sum = total + delta;
    if (!std::isfinite(sum) || !(sum > total)) return false;
    *out = sum;
    return true;
}

bool absorbed_pending_delta(double total, double delta) noexcept {
    return std::isfinite(total) && std::isfinite(delta) && total > 0.0 && delta > 0.0
        && total + delta == total;
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

void canonicalize_owner(Request& request, const RunIdentity& run, const std::string& directory,
                        std::shared_ptr<const OpeningReadback> admitted) {
    if (auto* bind = std::get_if<BindOpenings>(&request.owner)) {
        if (bind->external_openings) {
            if (admitted) {
                bind->external_openings = std::move(admitted);
                return;
            }
            if (!dynamic_cast<const FrozenOpenings*>(bind->external_openings.get())) {
                std::shared_ptr<const OpeningReadback> frozen;
                const auto status = freeze_openings_in_directory(
                    run, *bind->external_openings, frozen, directory);
                if (status != ReadStatus::ok)
                    throw detail::ScratchIoError(EIO, "native opening freeze");
                bind->external_openings = std::move(frozen);
            }
            return;
        }
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
        std::size_t* live_count, const OpeningReadback* external_cohort = nullptr,
        const OpeningObservationReadback* external_observations = nullptr) noexcept {
    *live_count = 0;
    const auto members = opening_count(cohort, external_cohort);
    const auto count = external_observations ? external_observations->count() : observations.size();
    if (count < members) return CoreFailure::MissingObservation;
    if (!members || count != members || !valid_position(position)) {
        return CoreFailure::ObservationMismatch;
    }
    auto read = [&](std::uint64_t i, OpeningObservation& out) noexcept {
        if (external_observations) return external_observations->observation_at(i, out);
        out = observations[static_cast<std::size_t>(i)];
        return ReadStatus::ok;
    };
    // Accepted membership is canonical. The enrollment validator also permits
    // an unordered borrowed input, before canonicalize_owner freezes it.
    bool canonical = true;
    RequestHandle previous_member;
    for (std::uint64_t i = 0; i < members; ++i) {
        RequestHandle member;
        if (opening_at(cohort, external_cohort, i, member) != ReadStatus::ok)
            return CoreFailure::MissingObservation;
        if (i && !handle_less(previous_member, member)) canonical = false;
        previous_member = member;
    }
    bool ordered = true;
    OpeningObservation previous;
    for (std::uint64_t i = 0; i < count; ++i) {
        OpeningObservation observation;
        if (read(i, observation) != ReadStatus::ok) return CoreFailure::MissingObservation;
        if (observation.queried_cycle != cycle
            || !same_position(observation.current_position, position)) {
            return CoreFailure::ObservationMismatch;
        }
        bool found = false;
        if (canonical) {
            std::uint64_t low = 0, high = members;
            while (low < high) {
                const auto middle = low + (high - low) / 2;
                RequestHandle member;
                if (opening_at(cohort, external_cohort, middle, member) != ReadStatus::ok)
                    return CoreFailure::MissingObservation;
                if (handle_less(member, observation.queried_opening)) low = middle + 1;
                else high = middle;
            }
            RequestHandle member;
            if (low < members) {
                if (opening_at(cohort, external_cohort, low, member) != ReadStatus::ok)
                    return CoreFailure::MissingObservation;
                found = member == observation.queried_opening;
            }
        } else {
            for (std::uint64_t j = 0; j < members; ++j) {
                RequestHandle member;
                if (opening_at(cohort, external_cohort, j, member) != ReadStatus::ok)
                    return CoreFailure::MissingObservation;
                found = found || member == observation.queried_opening;
            }
        }
        if (!found) return CoreFailure::ObservationMismatch;
        if (i && !handle_less(previous.queried_opening, observation.queried_opening))
            ordered = false;
        previous = observation;
        if (observation.has_live_matching_lot) {
            if (!position_matches(position, cycle, side)) return CoreFailure::ObservationMismatch;
            ++*live_count;
        }
    }
    if (!ordered) {
        for (std::uint64_t i = 0; i < count; ++i) {
            OpeningObservation a;
            if (read(i, a) != ReadStatus::ok) return CoreFailure::MissingObservation;
            for (std::uint64_t j = 0; j < i; ++j) {
                OpeningObservation b;
                if (read(j, b) != ReadStatus::ok) return CoreFailure::MissingObservation;
                if (a.queried_opening == b.queried_opening) {
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
                            observation.current_position, observation.openings, live_count,
                            close.external_openings.get(), observation.external_openings);
}

// B1-07: admission alone may allocate scratch; post-commit observation reads
// above remain allocation-free. Sort external membership once, reuse that
// freeze for the accepted definition, and sort observation handles only when
// they are not already in the same order. Every observation must be live at
// admission, so its handle is the only field needed in the temporary sort.
struct AdmissionOpenings final : OpeningReadback {
    const CommandContext& context;
    int64_t cycle;
    PositionIdentity position;
    AdmissionOpenings(const CommandContext& value, int64_t c, PositionIdentity p)
        : context(value), cycle(c), position(std::move(p)) {}
    std::uint64_t count() const noexcept override {
        return context.external_openings ? context.external_openings->count()
                                         : context.openings.size();
    }
    ReadStatus observation_at(std::uint64_t i, OpeningObservation& out) const noexcept {
        if (context.external_openings) return context.external_openings->observation_at(i, out);
        if (i >= context.openings.size()) return ReadStatus::not_found;
        out = context.openings[static_cast<std::size_t>(i)];
        return ReadStatus::ok;
    }
    ReadStatus opening_at(std::uint64_t i, RequestHandle& out) const noexcept override {
        OpeningObservation value;
        const auto status = observation_at(i, value);
        if (status != ReadStatus::ok) return status;
        if (value.queried_cycle != cycle || !same_position(value.current_position, position)
            || !value.has_live_matching_lot) return ReadStatus::programming_error;
        out = value.queried_opening;
        return ReadStatus::ok;
    }
};

bool admit_external_openings(const RunIdentity& run, const BindOpenings& bind,
                             const CommandContext& context, const std::string& directory,
                             std::shared_ptr<const OpeningReadback>& admitted) {
    const auto count = bind.external_openings->count();
    // Preserve the borrowed-reader refusal before beginning the scratch sort.
    for (std::uint64_t i = 0; i != count; ++i) {
        RequestHandle member;
        if (bind.external_openings->opening_at(i, member) != ReadStatus::ok
            || !member.incarnation || member.run != run) return false;
    }
    OpeningObservation first;
    if (context.external_openings) {
        if (context.external_openings->count() != count
            || context.external_openings->observation_at(0, first) != ReadStatus::ok) return false;
    } else {
        if (context.openings.size() != count) return false;
        first = context.openings.front();
    }
    const auto* position = std::get_if<PositionNonflat>(&first.current_position);
    if (!position || position->cycle != bind.cycle || !valid_position(first.current_position))
        return false;
    auto canonical = bind.external_openings;
    if (!dynamic_cast<const FrozenOpenings*>(canonical.get())) {
        const auto status = freeze_openings_in_directory(
            run, *bind.external_openings, canonical, directory);
        if (status == ReadStatus::io_error)
            throw detail::ScratchIoError(EIO, "native opening freeze");
        if (status != ReadStatus::ok) return false;
    }
    AdmissionOpenings observations(context, bind.cycle, first.current_position);
    bool same_order = true;
    for (std::uint64_t i = 0; i != count; ++i) {
        RequestHandle member, observed;
        if (canonical->opening_at(i, member) != ReadStatus::ok
            || !member.incarnation || member.run != run
            || observations.opening_at(i, observed) != ReadStatus::ok) return false;
        if (member != observed) same_order = false;
    }
    if (!same_order) {
        std::shared_ptr<const OpeningReadback> sorted;
        const auto status = freeze_openings_in_directory(run, observations, sorted, directory);
        if (status == ReadStatus::io_error)
            throw detail::ScratchIoError(EIO, "native opening freeze");
        if (status != ReadStatus::ok) return false;
        for (std::uint64_t i = 0; i != count; ++i) {
            RequestHandle member, observed;
            if (canonical->opening_at(i, member) != ReadStatus::ok
                || sorted->opening_at(i, observed) != ReadStatus::ok
                || member != observed) return false;
        }
    }
    admitted = std::move(canonical);
    return true;
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

// The explicit open activation grant: a caller-scoped authorization that no
// request, plan or token retains. It satisfies only the strict ordinal half of
// the birth gate, as if the request had been born before this point, and only
// at an Open cursor; the time half stands, so the point's effective time must
// not precede the request's decision-time lower bound. Every other birth
// exception stays as it was, and this adds one to them.
bool open_grant_birth_ok(const LiveRequest& live, const MatchCursor& cursor,
                         bool open_activation_grant) noexcept {
    return open_activation_grant
        && cursor.point.path_phase == NativePathPhase::Open
        && cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound;
}

bool trigger_cursor_eligible(const LiveRequest& live, const MatchCursor& cursor,
                             bool open_activation_grant) noexcept {
    if (point_eligible(live.birth(), cursor.point.ordinal, cursor.point.effective_time_ms)) {
        return true;
    }
    return same_p_continuation(live, cursor)
        || open_grant_birth_ok(live, cursor, open_activation_grant);
}

const ExecutionAppliedEvent* as_applied(const CommandEvent& event) noexcept {
    return std::get_if<ExecutionAppliedEvent>(&event);
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
// validate and commit exactly as before. Keep at most one idle envelope of
// each type, rather than retaining the peak number of simultaneous tokens.
// The block holds no constructed request state or continuation hash bytes.
template<class Tag>
struct PreparedImplStorage {
    static void prepare(std::size_t size) {
        auto& recycled = free_block();
        if (!recycled) recycled = ::operator new(size);
    }

    static void* allocate(std::size_t size) {
        auto& recycled = free_block();
        if (recycled) {
            void* block = recycled;
            recycled = nullptr;
            return block;
        }
        return ::operator new(size);
    }

    static void deallocate(void* block) noexcept {
        if (!block) return;
        auto& recycled = free_block();
        if (recycled) ::operator delete(block);
        else recycled = block;
    }

private:
    struct Recycled {
        void* block = nullptr;
        Recycled() = default;
        Recycled(const Recycled&) = delete;
        Recycled& operator=(const Recycled&) = delete;
        ~Recycled() { ::operator delete(block); }
    };
    static void*& free_block() noexcept {
        static thread_local Recycled recycled;
        return recycled.block;
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

// Where the live row standing at `priority` is, or live.size() when no row
// does. The working book is in strictly increasing priority order
// (LiveRequest::priority), so a long book is bisected down to a short run and
// the run is walked up to the first row not older than the one asked for
// (R5 lane PERF-K3). WorkingRequestCore::commit and the direct forms keep that
// order: a push and an erase-push append a row born under a fresh number of
// the request sequence, which usable_incarnation makes larger than every one
// the run took before; a keep_handle re-price takes such a number too and
// moves its row to the back (R5 lane V19-D); an erase keeps the rest in
// order; an update rewrites a row in place under its own priority.
template <bool ByPriority>
inline std::size_t book_position(const std::vector<LiveRequest>& live, uint64_t key) noexcept {
    // A request's key: its priority, or -- while no re-priced request is
    // live, when every priority is its incarnation -- the incarnation itself,
    // read without the definition's priority field.
    const auto key_of = [](const LiveRequest& row) noexcept {
        return ByPriority ? row.priority() : row.handle().incarnation;
    };
    constexpr std::size_t kWalk = 16;
    std::size_t first = 0;
    std::size_t count = live.size();
    while (count > kWalk) {
        const std::size_t half = count / 2;
        if (key_of(live[first + half]) < key) {
            first += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    for (std::size_t i = first; i < live.size(); ++i) {
        const uint64_t row = key_of(live[i]);
        if (row < key) continue;
        return row == key ? i : live.size();
    }
    return live.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// CR-221 O1-E2. The command journal, the group-effect keys and the long
// deferred chains' id lists keep a fixed resident part and write the rest to
// run-owned scratch files: detail::ScratchFile, an unlinked close-on-exec file
// read and written by position. A store that outgrows its resident part spills;
// a failed write/read is a ScratchIoError. Spill preparation can open the journal
// files before the first spill; a run whose reservation stays resident never
// touches them. Every spill
// is made where the caller may still throw (reserve_plan, seal_direct), never
// in commit.
namespace {

class ExtFile {
public:
    ExtFile() = default;
    ExtFile(const ExtFile&) = delete;
    ExtFile& operator=(const ExtFile&) = delete;
    ExtFile(ExtFile&&) noexcept = default;
    ExtFile& operator=(ExtFile&&) noexcept = default;

    void set_directory(const std::string& directory) {
        // A reservation may have opened an otherwise empty file. It has no
        // records to preserve when the caller selects its spill directory.
        if (end_ == 0) file_.clear();
        file_.set_directory(directory);
    }
    uint64_t size() const noexcept { return end_; }
    // Physical backing only: do not publish any logical records. In arena mode
    // this also claims the first chunk/table entry before a bounded operation.
    void prepare(uint64_t bytes) {
        file_.open("pineforge-o1e2");
        file_.ensure_size(bytes);
    }
    // Positional write; the logical size moves only after the whole write.
    void write(uint64_t offset, const void* data, std::size_t length) {
        if (length == 0) return;
        file_.open("pineforge-o1e2");
        file_.write(offset, data, length);
        end_ = std::max(end_, offset + static_cast<uint64_t>(length));
    }
    uint64_t append(const void* data, std::size_t length) {
        const uint64_t at = end_;
        write(at, data, length);
        return at;
    }
    // Forgets everything written and closes the file; the next write opens a
    // fresh one (in the same directory).
    void reset() noexcept {
        file_.clear();
        end_ = 0;
    }
    // Reserves `length` bytes at the end for positional writes made later.
    uint64_t extend(uint64_t length) {
        file_.open("pineforge-o1e2");
        const uint64_t at = end_;
        end_ += length;
        return at;
    }
    void read(uint64_t offset, void* out, std::size_t length) const {
        if (length == 0) return;
        if (offset > end_ || length > end_ - offset) {
            throw detail::ScratchIoError(EIO, "native o1 store read past its end");
        }
        file_.read(offset, out, length);
    }

private:
    detail::ScratchFile file_;
    uint64_t end_ = 0;
};

// Fixed-width records addressed by absolute index. The newest records stay in
// a small resident tail; once the tail is full its older half is written to the
// file at (index - origin) * sizeof(Record), so a record retired before that
// never touches the file. Retiring a prefix moves `base` and drops the tail
// records below it; once every record is retired the file behind them is
// closed and the next record starts a fresh one (a new origin), so a log that
// is pinned, drained and pinned again leaves no dead file. Only prepare() and
// make_room() do I/O: push() cannot fail after make_room().
template <class Record, std::size_t kTail>
class TailLog {
    static_assert(std::is_trivially_copyable_v<Record>, "log records are raw bytes");
    static_assert(kTail >= 4 && kTail % 2 == 0, "the tail flushes in halves");

public:
    TailLog() { tail_.reserve(kTail); }
    void set_directory(const std::string& directory) { file_.set_directory(directory); }
    uint64_t base() const noexcept { return base_; }
    uint64_t size() const noexcept { return flushed_ + tail_.size(); }
    // The number of records not yet retired.
    uint64_t retained() const noexcept { return size() - base_; }
    // How many records the process holds in memory.
    std::size_t resident() const noexcept { return tail_.size(); }
    void prepare() {
        file_.prepare((flushed_ - origin_ + kTail / 2) * sizeof(Record));
    }
    void make_room() {
        if (tail_.size() < kTail) return;
        const std::size_t half = kTail / 2;
        file_.write((flushed_ - origin_) * sizeof(Record), tail_.data(), half * sizeof(Record));
        tail_.erase(tail_.begin(), tail_.begin() + static_cast<std::ptrdiff_t>(half));
        flushed_ += half;
    }
    void push(const Record& record) noexcept { tail_.push_back(record); }
    Record get(uint64_t index) const {
        if (index < base_ || index >= size()) {
            throw std::out_of_range("native o1 log index is not retained");
        }
        if (index >= flushed_) return tail_[static_cast<std::size_t>(index - flushed_)];
        Record record{};
        file_.read((index - origin_) * sizeof(Record), &record, sizeof record);
        return record;
    }
    void retire_to(uint64_t new_base) noexcept {
        if (new_base <= base_) return;
        base_ = new_base;
        if (base_ > flushed_) {
            const uint64_t drop = std::min<uint64_t>(base_ - flushed_, tail_.size());
            tail_.erase(tail_.begin(), tail_.begin() + static_cast<std::ptrdiff_t>(drop));
            flushed_ += drop;
        }
        if (tail_.empty() && base_ == flushed_) {
            // Every record is retired: nothing in the file is read again.
            origin_ = flushed_;
            file_.reset();
        }
    }
    // Forgets every record, retired or not, and the file behind them; the
    // next record is index 0 again.
    void reset() noexcept {
        tail_.clear();
        flushed_ = 0;
        base_ = 0;
        origin_ = 0;
        file_.reset();
    }

private:
    std::vector<Record> tail_;
    uint64_t flushed_ = 0;   // records before the tail: written to the file or retired
    uint64_t base_ = 0;      // the first record not retired
    uint64_t origin_ = 0;    // the index the file's first record has
    ExtFile file_;
};

class ByteWriter {
public:
    struct CapacityExceeded {};

    ByteWriter() = default;
    ByteWriter(unsigned char* buffer, std::size_t capacity) noexcept
        : fixed_(buffer), capacity_(capacity) {}
    void raw(const void* data, std::size_t length) {
        if (fixed_) {
            if (length > capacity_ - used_) {
                throw CapacityExceeded{};
            }
            if (length != 0) std::memcpy(fixed_ + used_, data, length);
            used_ += length;
            return;
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + length);
    }
    const unsigned char* data() const noexcept { return fixed_ ? fixed_ : buffer_.data(); }
    std::size_t size() const noexcept { return fixed_ ? used_ : buffer_.size(); }
    // Only the fixed journal writer rewinds, to the last complete event.
    void rewind(std::size_t size) noexcept { used_ = size; }

private:
    std::vector<unsigned char> buffer_;
    unsigned char* fixed_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t used_ = 0;
};

class ByteReader {
public:
    ByteReader(const unsigned char* data, std::size_t length) noexcept
        : at_(data), end_(data + length) {}
    void raw(void* out, std::size_t length) {
        if (static_cast<std::size_t>(end_ - at_) < length) {
            throw detail::ScratchIoError(EIO, "native o1 store record is truncated");
        }
        if (length != 0) std::memcpy(out, at_, length);
        at_ += length;
    }
    std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - at_); }

private:
    const unsigned char* at_;
    const unsigned char* end_;
};

// One field list per type serves both directions, so what a spill writes and
// what a reload reads cannot drift apart. SaveAr reads the object it is given
// (it is handed a const one and writes it out unchanged); LoadAr assigns it.
// Leaves are raw bytes (a handle's session key is an interned process-local
// pointer, and the scratch file never outlives the process), so every
// trivially copyable type is one raw copy; the types that own a string, a
// vector, an optional, a variant or a definition are walked field by field.
struct SaveAr {
    static constexpr bool saving = true;
    ByteWriter& out;
    void raw(const void* data, std::size_t length) { out.raw(data, length); }
    template <class T>
    void operator()(const T& item);
};

struct LoadAr {
    static constexpr bool saving = false;
    ByteReader& in;
    void raw(void* data, std::size_t length) { in.raw(data, length); }
    std::size_t remaining() const noexcept { return in.remaining(); }
    template <class T>
    void operator()(T& item);
};

template <class T> struct IsVariant : std::false_type {};
template <class... Alternatives>
struct IsVariant<std::variant<Alternatives...>> : std::true_type {};
template <class T> struct IsOptional : std::false_type {};
template <class Item> struct IsOptional<std::optional<Item>> : std::true_type {};
template <class T> struct IsVector : std::false_type {};
template <class Item, class Allocator>
struct IsVector<std::vector<Item, Allocator>> : std::true_type {};

template <class Ar, class T> void archive_value(Ar& ar, T& item);

template <class Ar> void archive_io(Ar& ar, BindOpenings& item);
template <class Ar> void archive_io(Ar& ar, OpeningsClose& item);
template <class Ar> void archive_io(Ar& ar, SelectedExposure& item);
template <class Ar> void archive_io(Ar& ar, Request& item);
template <class Ar> void archive_io(Ar& ar, RequestDefinition& item);
template <class Ar> void archive_io(Ar& ar, LiveRequest& item);
template <class Ar> void archive_io(Ar& ar, AcceptedEvent& item);
template <class Ar> void archive_io(Ar& ar, RejectedEvent& item);
template <class Ar> void archive_io(Ar& ar, ReplacedEvent& item);
template <class Ar> void archive_io(Ar& ar, ReplaceRejectedEvent& item);
template <class Ar> void archive_io(Ar& ar, CancelledEvent& item);
template <class Ar> void archive_io(Ar& ar, NotWorkingEvent& item);
template <class Ar> void archive_io(Ar& ar, InvalidHandleEvent& item);
template <class Ar> void archive_io(Ar& ar, NoEffectEvent& item);
template <class Ar> void archive_io(Ar& ar, MatchRejectedEvent& item);
template <class Ar> void archive_io(Ar& ar, CloseBoundEvent& item);
template <class Ar> void archive_io(Ar& ar, ActivatedEvent& item);
template <class Ar> void archive_io(Ar& ar, ExecutionAppliedEvent& item);
template <class Ar> void archive_io(Ar& ar, ReservationReducedEvent& item);
template <class Ar> void archive_io(Ar& ar, DeferredGroupAdjustmentEvent& item);
template <class Ar> void archive_io(Ar& ar, QuantityBoundEvent& item);
template <class Ar> void archive_io(Ar& ar, ArmedEvent& item);
template <class Ar> void archive_io(Ar& ar, TermsResolvedEvent& item);
template <class Ar> void archive_io(Ar& ar, MarginCallEvent& item);

template <class Ar>
void archive_string(Ar& ar, std::string& text) {
    uint64_t length = text.size();
    ar.raw(&length, sizeof length);
    if constexpr (Ar::saving) {
        ar.raw(text.data(), text.size());
    } else {
        if (length > ar.remaining()) {
            throw detail::ScratchIoError(EIO, "native o1 store string is truncated");
        }
        text.resize(static_cast<std::size_t>(length));
        ar.raw(text.data(), text.size());
    }
}

template <class Ar, class Item>
void archive_optional(Ar& ar, std::optional<Item>& item) {
    unsigned char present = item.has_value() ? 1 : 0;
    ar.raw(&present, 1);
    if constexpr (Ar::saving) {
        if (present) archive_value(ar, *item);
    } else {
        if (present) {
            item.emplace();
            archive_value(ar, *item);
        } else {
            item.reset();
        }
    }
}

template <class Ar, class Item>
void archive_vector(Ar& ar, std::vector<Item>& items) {
    uint64_t count = items.size();
    ar.raw(&count, sizeof count);
    if constexpr (!Ar::saving) {
        if (count > ar.remaining()) {
            throw detail::ScratchIoError(EIO, "native o1 store vector is truncated");
        }
        items.clear();
        items.resize(static_cast<std::size_t>(count));
    }
    for (auto& element : items) archive_value(ar, element);
}

template <std::size_t Index, class Ar, class Variant>
void archive_variant_load(Ar& ar, Variant& variant, std::size_t index) {
    if constexpr (Index < std::variant_size_v<Variant>) {
        if (index == Index) {
            variant.template emplace<Index>();
            archive_value(ar, std::get<Index>(variant));
        } else {
            archive_variant_load<Index + 1>(ar, variant, index);
        }
    } else {
        (void) ar;
        (void) variant;
        (void) index;
        throw detail::ScratchIoError(EIO, "native o1 store variant index is out of range");
    }
}

template <class Ar, class... Alternatives>
void archive_variant(Ar& ar, std::variant<Alternatives...>& variant) {
    uint32_t index = static_cast<uint32_t>(variant.index());
    ar.raw(&index, sizeof index);
    if constexpr (Ar::saving) {
        std::visit([&ar](auto& alternative) { archive_value(ar, alternative); }, variant);
    } else {
        archive_variant_load<0>(ar, variant, index);
    }
}

template <class Ar>
void archive_definition(Ar& ar, DefinitionRef& definition) {
    unsigned char present = definition ? 1 : 0;
    ar.raw(&present, 1);
    if constexpr (Ar::saving) {
        if (present) archive_io(ar, const_cast<RequestDefinition&>(*definition));
    } else {
        if (present) {
            auto created = std::make_shared<RequestDefinition>();
            archive_io(ar, *created);
            definition = std::move(created);
        } else {
            definition.reset();
        }
    }
}

template <class Ar, class T>
void archive_value(Ar& ar, T& item) {
    if constexpr (std::is_same_v<T, std::string>) {
        archive_string(ar, item);
    } else if constexpr (std::is_same_v<T, DefinitionRef>) {
        archive_definition(ar, item);
    } else if constexpr (IsOptional<T>::value) {
        archive_optional(ar, item);
    } else if constexpr (IsVector<T>::value) {
        archive_vector(ar, item);
    } else if constexpr (IsVariant<T>::value) {
        archive_variant(ar, item);
    } else if constexpr (std::is_trivially_copyable_v<T>) {
        ar.raw(&item, sizeof(T));
    } else {
        archive_io(ar, item);
    }
}

template <class T>
void SaveAr::operator()(const T& item) {
    archive_value(*this, const_cast<T&>(item));
}

template <class T>
void LoadAr::operator()(T& item) {
    archive_value(*this, item);
}

// B1-06: only an event that outgrows an ordinary page takes these archives.
// Reuse the page buffers while streaming one record; no record-sized byte
// vector or retained allocation is needed, and SaveAr/LoadAr stay unchanged.
struct StreamSaveAr {
    static constexpr bool saving = true;
    ExtFile& file;
    unsigned char* buffer;
    std::size_t capacity;
    std::size_t used = 0;

    void flush() {
        if (used) file.append(buffer, used);
        used = 0;
    }
    void raw(const void* data, std::size_t length) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (length) {
            const auto take = std::min(length, capacity - used);
            std::memcpy(buffer + used, bytes, take);
            bytes += take;
            length -= take;
            used += take;
            if (used == capacity) flush();
        }
    }
    template <class T>
    void operator()(const T& item) {
        archive_value(*this, const_cast<T&>(item));
    }
};

struct StreamLoadAr {
    static constexpr bool saving = false;
    const ExtFile& file;
    unsigned char* buffer;
    std::size_t capacity;
    uint64_t offset;
    uint64_t unread;
    std::size_t at = 0;
    std::size_t available = 0;

    uint64_t remaining() const noexcept { return unread + available - at; }
    void raw(void* data, std::size_t length) {
        if (length > remaining()) {
            throw detail::ScratchIoError(EIO, "native o1 store record is truncated");
        }
        auto* bytes = static_cast<unsigned char*>(data);
        while (length) {
            if (at == available) {
                available = static_cast<std::size_t>(std::min<uint64_t>(unread, capacity));
                file.read(offset, buffer, available);
                offset += available;
                unread -= available;
                at = 0;
            }
            const auto take = std::min(length, available - at);
            std::memcpy(bytes, buffer + at, take);
            bytes += take;
            length -= take;
            at += take;
        }
    }
    template <class T>
    void operator()(T& item) {
        archive_value(*this, item);
    }
};

// The external openings of a bound close (BindOpenings, OpeningsClose): how
// many, then each opening's handle in the readback's order. A reload freezes
// the list again, so a spilled row or a paged event keeps its membership
// rather than coming back with none; DefinitionMatchAr compares the same
// bytes, so a resident definition is reused only when its list is this one.
template <class Ar>
void archive_external_openings(Ar& ar, std::shared_ptr<const OpeningReadback>& external) {
    std::uint64_t count = 0;
    if constexpr (Ar::saving) {
        count = external ? external->count() : 0;
        ar.raw(&count, sizeof count);
        for (std::uint64_t index = 0; index != count; ++index) {
            RequestHandle opening;
            if (external->opening_at(index, opening) != ReadStatus::ok) {
                throw detail::ScratchIoError(EIO, "native opening readback");
            }
            ar(opening);
        }
    } else {
        ar.raw(&count, sizeof count);
        if (count == 0) {
            external.reset();
            return;
        }
        // Every handle takes at least a byte: a count past the bytes left is
        // a damaged record, never an allocation.
        if (count > ar.remaining()) {
            throw detail::ScratchIoError(EIO, "native o1 store record is truncated");
        }
        std::vector<RequestHandle> openings(static_cast<std::size_t>(count));
        for (auto& opening : openings) ar(opening);
        const OpeningInputCursor input(
                count, &openings, [](void* context, std::uint64_t index, RequestHandle& out) {
                    const auto& list = *static_cast<const std::vector<RequestHandle>*>(context);
                    if (index >= list.size()) return ReadStatus::not_found;
                    out = list[static_cast<std::size_t>(index)];
                    return ReadStatus::ok;
                });
        std::shared_ptr<const OpeningReadback> frozen;
        if (freeze_openings_in_directory(openings.front().run, input, frozen, {})
            != ReadStatus::ok) {
            throw detail::ScratchIoError(EIO, "native opening freeze");
        }
        external = std::move(frozen);
    }
}

template <class Ar> void archive_io(Ar& ar, BindOpenings& item) {
    ar(item.openings); ar(item.cycle);
    archive_external_openings(ar, item.external_openings);
}
template <class Ar> void archive_io(Ar& ar, OpeningsClose& item) {
    ar(item.openings); ar(item.cycle); ar(item.side); ar(item.enrollment);
    archive_external_openings(ar, item.external_openings);
}
template <class Ar> void archive_io(Ar& ar, SelectedExposure& item) {
    ar(item.cycle); ar(item.incarnations);
}
template <class Ar> void archive_io(Ar& ar, Request& item) {
    ar(item.intent); ar(item.label); ar(item.comment); ar(item.trigger);
    ar(item.capacity); ar(item.owner); ar(item.group); ar(item.anchor);
}
template <class Ar> void archive_io(Ar& ar, RequestDefinition& item) {
    ar(item.handle); ar(item.request); ar(item.birth); ar(item.predecessor);
    ar(item.origin); ar(item.root); ar(item.priority); ar(item.kept_binding);
}
template <class Ar> void archive_io(Ar& ar, LiveRequest& item) {
    ar(item.definition); ar(item.remaining); ar(item.authority); ar(item.trigger_state);
    ar(item.allowance); ar(item.pending);
    ar(item.sizing_units); ar(item.sizing_scope); ar(item.sizing_price);
}
template <class Ar> void archive_io(Ar& ar, AcceptedEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.surface);
}
template <class Ar> void archive_io(Ar& ar, RejectedEvent& item) {
    ar(item.ordinal); ar(item.request); ar(item.reason); ar(item.surface);
}
template <class Ar> void archive_io(Ar& ar, ReplacedEvent& item) {
    ar(item.ordinal); ar(item.predecessor_definition); ar(item.successor_definition);
    ar(item.surface);
}
template <class Ar> void archive_io(Ar& ar, ReplaceRejectedEvent& item) {
    ar(item.ordinal); ar(item.live_definition); ar(item.attempted); ar(item.reason);
    ar(item.surface);
}
template <class Ar> void archive_io(Ar& ar, CancelledEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.reason); ar(item.cause);
    ar(item.prior_authority); ar(item.unexecuted); ar(item.pending);
}
template <class Ar> void archive_io(Ar& ar, NotWorkingEvent& item) {
    ar(item.ordinal); ar(item.target); ar(item.attempted); ar(item.surface);
}
template <class Ar> void archive_io(Ar& ar, InvalidHandleEvent& item) {
    ar(item.ordinal); ar(item.target); ar(item.attempted); ar(item.surface);
}
template <class Ar> void archive_io(Ar& ar, NoEffectEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.remaining); ar(item.authority);
    ar(item.cursor);
}
template <class Ar> void archive_io(Ar& ar, MatchRejectedEvent& item) {
    ar(item.ordinal); ar(item.reason); ar(item.definition); ar(item.remaining);
    ar(item.authority); ar(item.cursor); ar(item.attempted_terms);
}
template <class Ar> void archive_io(Ar& ar, CloseBoundEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.cycle); ar(item.side);
    ar(item.cursor); ar(item.before); ar(item.after);
}
template <class Ar> void archive_io(Ar& ar, ActivatedEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.kind); ar(item.before);
    ar(item.after); ar(item.reached_price); ar(item.cursor);
}
template <class Ar> void archive_io(Ar& ar, ExecutionAppliedEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.raw_price); ar(item.resolved_price);
    ar(item.current_ticket); ar(item.first_trade_index); ar(item.closed_trade_count);
    ar(item.opened_lot_incarnation); ar(item.closed_units); ar(item.opened_units);
    ar(item.filled_working); ar(item.remaining_before); ar(item.remaining_after);
    ar(item.allowance_before); ar(item.allowance_after); ar(item.terminal);
    ar(item.terminal_reason); ar(item.cycle_before); ar(item.cycle_after);
    ar(item.scope); ar(item.cursor);
}
template <class Ar> void archive_io(Ar& ar, ReservationReducedEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.cause); ar(item.recipient);
    ar(item.effect); ar(item.requested_delta); ar(item.actual_deduction);
    ar(item.before); ar(item.after);
}
template <class Ar> void archive_io(Ar& ar, DeferredGroupAdjustmentEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.cause); ar(item.recipient);
    ar(item.effect); ar(item.deferred_delta); ar(item.pending_before);
    ar(item.pending_after); ar(item.previous_pending_receipt);
}
template <class Ar> void archive_io(Ar& ar, QuantityBoundEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.source); ar(item.source_units);
    ar(item.prior_adjustment_ids); ar(item.pending_total); ar(item.effective_deduction);
    ar(item.remaining);
}
template <class Ar> void archive_io(Ar& ar, ArmedEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.before); ar(item.after);
    ar(item.enrollment); ar(item.quantity_resolution);
}
template <class Ar> void archive_io(Ar& ar, TermsResolvedEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.cursor); ar(item.input);
    ar(item.prior_adjustment_ids); ar(item.pending_total); ar(item.effective_deduction);
    ar(item.remaining_before); ar(item.remaining_after); ar(item.allowance_after);
}
template <class Ar> void archive_io(Ar& ar, MarginCallEvent& item) {
    ar(item.ordinal); ar(item.definition); ar(item.applied); ar(item.cursor);
    ar(item.side); ar(item.mark); ar(item.equity); ar(item.required);
    ar(item.liquidation_price); ar(item.units); ar(item.position_before);
    ar(item.position_after);
}

// Compare the definition prefix of a just-decoded book record by walking the
// very same field lists as SaveAr/LoadAr. No output buffer or hash is involved:
// strings, vector lengths, tags, optional flags and every leaf must match.
// Raw leaves include padding, so a padding difference conservatively misses.
struct DefinitionMatchAr {
    static constexpr bool saving = true;
    const unsigned char* at;
    std::size_t left;
    bool matched = true;

    void raw(const void* value, std::size_t length) noexcept {
        if (!matched) return;
        if (length > left || (length != 0 && std::memcmp(at, value, length) != 0)) {
            matched = false;
            return;
        }
        at += length;
        left -= length;
    }
    template <class T>
    void operator()(const T& item) {
        if (matched) archive_value(*this, const_cast<T&>(item));
    }
};

// One page of spilled journal events: the absolute positions and ordinals it
// covers (both ascend from page to page) and where its serialized events are.
struct JournalPage {
    uint64_t first_position = 0;
    uint64_t count = 0;
    uint64_t first_ordinal = 0;
    uint64_t last_ordinal = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

// A group-effect key without its run: both handles of a receipt are the
// core's own run (event_at and classify refuse any other).
struct ReceiptRec {
    uint64_t cause = 0;
    uint64_t recipient = 0;
    uint64_t outcome = 0;
    uint64_t effect = 0;
};

// Where the id list of one long chain starts, keyed by the ordinal of the
// resolution event that spent it.
struct ChainSegmentRec {
    uint64_t event_ordinal = 0;
    uint64_t offset = 0;
    uint64_t count = 0;
};

// One spilled book row, filed under its priority: where its serialized
// LiveRequest sits in the blob file. state 0: the row is spilled; 1: it came
// back (or ended) and the entry is dead weight until the next compaction.
struct BookSlot {
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t incarnation = 0;
    uint64_t state = 0;
};

// The priority of a spilled row whose handle was re-priced, filed under its
// incarnation (a row that was never re-priced stands at its incarnation).
struct IncarnationSlot {
    uint64_t priority = 0;
    uint64_t state = 0;
};

// Table keys are big endian, so that byte order is numeric order.
std::string book_key(uint64_t value) {
    std::string key(8, '\0');
    for (std::size_t i = 0; i != 8; ++i) {
        key[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xffu);
    }
    return key;
}

uint64_t book_key_value(const std::string& key) {
    uint64_t value = 0;
    for (const char byte : key) value = (value << 8) | static_cast<unsigned char>(byte);
    return value;
}

// Rows the working book keeps in live_ before its newest ones are spilled.
constexpr std::size_t kBookMax = 4096;

}  // namespace

// The command journal: the newest events resident in fixed chunks, the older
// ones written as pages to a scratch file and read back through a small cache.
// Chunks never move, so a reference to a resident event survives appends and
// retirements of other events; a spill writes the oldest events and frees
// their chunk. Positions are absolute (the core's history_base()..
// history_end()), ordinals ascend with positions.
struct WorkingRequestCore::JournalStore {
    detail::CommandScratchStore* codec = nullptr; // borrowed, run-owned backing
    mutable std::vector<std::byte> codec_page; // allocated once from store geometry
    mutable std::array<std::optional<CommandEvent>, 4> codec_values;
    mutable std::array<uint64_t, 4> codec_positions{};
    mutable std::size_t codec_next = 0;

    void bind(detail::CommandScratchStore& store) {
        if (store.status() == ReadStatus::io_error)
            detail::rethrow_carried_scratch_failure(store.carried_failure());
        if (codec || end != 0 || store.status() != ReadStatus::ok || store.record_count() != 0
            || store.transaction_active())
            throw std::invalid_argument("journal codec requires an empty committed store");
        codec_page.resize(store.page_size());
        codec = &store;
    }
    void check(ReadStatus status) const {
        if (status == ReadStatus::ok) return;
        if (status == ReadStatus::io_error) {
            if (codec) detail::rethrow_carried_scratch_failure(codec->carried_failure());
            throw detail::ScratchIoError(EIO, "native journal codec I/O");
        }
        throw std::logic_error("native journal codec invalid storage");
    }
    const CommandEvent& codec_at(uint64_t position) const {
        for (std::size_t i = 0; i != codec_values.size(); ++i)
            if (codec_values[i] && codec_positions[i] == position) return *codec_values[i];
        detail::ScratchRecordInfo info;
        check(detail::journal_codec::find(*codec, position, info));
        const detail::journal_codec::StoreSource source(*codec, info.handle, info.value_bytes);
        CommandEvent value;
        check(decode_journal_record(source, {codec_page.data(), codec_page.size()}, value));
        const auto slot = codec_next++ % codec_values.size();
        codec_values[slot] = std::move(value);
        codec_positions[slot] = position;
        return *codec_values[slot];
    }
    // A page holds a contiguous prefix of one chunk, bounded by bytes as well
    // as kChunk events. The resident part may hold up to kMaxResident events;
    // a window that grows past it, including within one bar before retirement,
    // spills its oldest chunks to pages. A spill never takes
    // the resident part below kKeepResident events, so the newest events stay
    // in memory.
    static constexpr std::size_t kChunk = 32;
    static constexpr std::size_t kMaxResident = 2048;
    static constexpr std::size_t kKeepResident = 128;
    // Page layout: concatenated events, each starting with a uint32_t
    // alternative and a uint64_t ordinal, followed by its fields. This 32,768 B
    // buffer bounds ordinary multi-event pages. A larger single event streams
    // through the same buffer into a one-event page (B1-06). Request
    // label/comment, BindOpenings/OpeningsClose opening vectors,
    // SelectedExposure incarnations and TermsResolvedEvent/QuantityBoundEvent
    // adjustment-id vectors have no finite schema bound. A full page closes
    // before the next event; no per-event size estimate is used.
    static constexpr std::size_t kSpillPageBytes = 32768;
    static_assert(sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8,
                  "journal event tag/ordinal byte widths");
    struct Chunk {
        std::array<std::optional<CommandEvent>, kChunk> slot;
    };
    static_assert(sizeof(Chunk) == kChunk * sizeof(std::optional<CommandEvent>),
                  "resident journal chunk contains exactly kChunk event slots");
    struct Cached {
        bool valid = false;
        uint64_t page_no = 0;
        uint64_t first_position = 0;   // the absolute positions the page covers
        uint64_t count = 0;
        std::vector<CommandEvent> events;
    };
    // One lazy fixed allocation per journal. A writer never aliases a reload,
    // and each cache slot keeps its own input bytes. Owning payload decoding
    // below the event vector (definitions, strings, nested vectors) is separate.
    struct SpillStorage {
        std::array<unsigned char, kSpillPageBytes> write;
        std::array<std::array<unsigned char, kSpillPageBytes>, 4> read;
    };
    static_assert(sizeof(SpillStorage) == 5 * kSpillPageBytes,
                  "one spill buffer and four distinct reload buffers");

    std::vector<std::unique_ptr<Chunk>> chunks;
    // The most recent chunk a retirement or a spill emptied, kept for the next
    // append, so a journal that is retired as it grows cycles one chunk
    // through the heap instead of allocating and freeing one per page of events.
    std::unique_ptr<Chunk> spare;
    uint64_t chunk0_base = 0;   // absolute position held by chunks[0].slot[0]
    uint64_t first = 0;         // first resident absolute position
    uint64_t end = 0;           // one past the newest event
    TailLog<JournalPage, 16> pages;
    ExtFile data;
    mutable std::array<Cached, 4> cache{};
    mutable std::size_t cache_next = 0;
    std::unique_ptr<SpillStorage> spill;
    // The directory this core's stores spill into ("": TMPDIR, then /tmp), set
    // beside the successor roots'. The journal exists for the whole life of a
    // bound core, so it is where the core keeps it (scratch_directory): the
    // core itself holds no string for it.
    std::string where;

    JournalStore() { chunks.reserve(kMaxResident / kChunk + 4); }
    // Called by a beyond-resident reserve before the operation it provisions,
    // or lazily by the first unreserved spill. No added page/cache allocation
    // is charged to a core whose reservations and history stay resident.
    void prepare_spill() {
        if (!spill) {
            auto storage = std::make_unique<SpillStorage>();
            for (Cached& slot : cache) slot.events.reserve(kChunk);
            spill = std::move(storage);
        }
        data.prepare(data.size() + kSpillPageBytes);
        pages.prepare();
    }
    void set_directory(const std::string& directory) {
        pages.set_directory(directory);
        data.set_directory(directory);
        where = directory;
        if (spill) prepare_spill();
    }
    std::size_t resident_count() const noexcept { return static_cast<std::size_t>(end - first); }
    // Every spilled event has been retired: the page table, the file behind
    // it and the decoded pages are dropped, so a journal that is pinned,
    // spills and is released again leaves no dead pages on disk.
    void release_pages() noexcept {
        pages.reset();
        data.reset();
        for (Cached& slot : cache) {
            slot.valid = false;
            slot.events.clear();
        }
        cache_next = 0;
    }
    const CommandEvent& resident_at(uint64_t position) const {
        if (codec) return codec_at(position);
        const uint64_t relative = position - chunk0_base;
        return *chunks[static_cast<std::size_t>(relative / kChunk)]
                    ->slot[static_cast<std::size_t>(relative % kChunk)];
    }
    // Appends one event into storage make_room set aside.
    void push(CommandEvent&& event) {
        if (codec) {
            const auto source = encode_journal_record(event);
            check(detail::journal_codec::put(*codec, end, source));
            const auto slot = codec_next++ % codec_values.size();
            codec_values[slot] = std::move(event);
            codec_positions[slot] = end++;
            return;
        }
        const uint64_t relative = end - chunk0_base;
        chunks[static_cast<std::size_t>(relative / kChunk)]
            ->slot[static_cast<std::size_t>(relative % kChunk)].emplace(std::move(event));
        ++end;
    }
    // Destroys the resident events below `new_first` and frees every chunk
    // they emptied. No I/O.
    void drop_front(uint64_t new_first) {
        if (codec) {
            auto status = codec->begin();
            if (status != ReadStatus::ok) { check(status); return; }
            for (auto position = first; position != new_first; ++position) {
                detail::ScratchRecordInfo info;
                status = detail::journal_codec::find(*codec, position, info);
                if (status != ReadStatus::ok) break;
                status = codec->erase(info.handle);
                if (status != ReadStatus::ok) break;
            }
            check(detail::journal_codec::finish(*codec, status));
            first = new_first;
            for (std::size_t i = 0; i != codec_values.size(); ++i)
                if (codec_values[i] && codec_positions[i] < first) codec_values[i].reset();
            return;
        }
        while (first < new_first) {
            const uint64_t relative = first - chunk0_base;
            chunks[static_cast<std::size_t>(relative / kChunk)]
                ->slot[static_cast<std::size_t>(relative % kChunk)].reset();
            ++first;
        }
        while (!chunks.empty() && first >= chunk0_base + kChunk) {
            if (!spare) spare = std::move(chunks.front());
            chunks.erase(chunks.begin());
            chunk0_base += kChunk;
        }
        if (chunks.empty()) chunk0_base = first;
    }
    // One oversized event, streamed without a record-sized byte allocation.
    // Publish its page only after serialization and every write succeed.
    void spill_large_front_event() {
        pages.make_room();
        JournalPage page;
        page.first_position = first;
        page.count = 1;
        page.first_ordinal = page.last_ordinal = event_ordinal(resident_at(first));
        page.offset = data.size();
        StreamSaveAr ar{data, spill->write.data(), spill->write.size()};
        ar(resident_at(first));
        ar.flush();
        page.length = data.size() - page.offset;
        pages.push(page);
        drop_front(first + 1);
    }
    // Writes the oldest resident events that fit, up to their chunk's end,
    // as one page and drops exactly that prefix. Everything that can throw
    // comes before publishing the page or changing the resident prefix.
    void spill_front_chunk() {
        const uint64_t chunk_end = std::min<uint64_t>(end, chunk0_base + kChunk);
        if (chunk_end <= first) return;
        if (!spill) prepare_spill();
        ByteWriter out(spill->write.data(), spill->write.size());
        SaveAr ar{out};
        uint64_t page_end = first;
        for (; page_end < chunk_end; ++page_end) {
            const std::size_t event_start = out.size();
            try {
                ar(resident_at(page_end));
            } catch (const ByteWriter::CapacityExceeded&) {
                out.rewind(event_start);
                if (page_end == first) {
                    spill_large_front_event();
                    return;
                }
                break;
            }
        }
        JournalPage page;
        page.first_position = first;
        page.count = page_end - first;
        page.first_ordinal = event_ordinal(resident_at(first));
        page.last_ordinal = event_ordinal(resident_at(page_end - 1));
        pages.make_room();
        page.offset = data.append(out.data(), out.size());
        page.length = out.size();
        pages.push(page);
        // drop_front keeps a partly occupied chunk and its original slot base;
        // the next spill starts at page_end, even when it is not aligned.
        drop_front(page_end);
    }
    // Room for `incoming` more events: spill while the resident part would
    // pass its bound, then make sure the chunks that will hold them exist.
    void make_room(std::size_t incoming) {
        if (codec) return;
        while (resident_count() + incoming > kMaxResident && resident_count() > kKeepResident) {
            spill_front_chunk();
        }
        if (chunks.empty()) chunk0_base = end;
        const uint64_t need = end + incoming;
        uint64_t have = chunk0_base + static_cast<uint64_t>(chunks.size()) * kChunk;
        while (have < need) {
            // A recycled chunk is empty: drop_front reset every slot it held.
            std::unique_ptr<Chunk> chunk = spare ? std::move(spare) : std::make_unique<Chunk>();
            chunks.reserve(chunks.size() + 1);
            chunks.push_back(std::move(chunk));
            have += kChunk;
        }
    }
    // The page holding the spilled absolute `position`.
    bool page_for_position(uint64_t position, uint64_t* page_no, JournalPage* page) const {
        uint64_t low = pages.base();
        uint64_t high = pages.size();
        while (low < high) {
            const uint64_t middle = low + (high - low) / 2;
            if (pages.get(middle).first_position <= position) low = middle + 1;
            else high = middle;
        }
        if (low == pages.base()) return false;
        const JournalPage found = pages.get(low - 1);
        if (position >= found.first_position + found.count) return false;
        *page_no = low - 1;
        *page = found;
        return true;
    }
    const std::vector<CommandEvent>& load(uint64_t page_no, const JournalPage& page) const {
        for (const Cached& slot : cache) {
            if (slot.valid && slot.page_no == page_no) return slot.events;
        }
        if (!spill || page.count > kChunk
            || (page.length > kSpillPageBytes && page.count != 1)) {
            throw detail::ScratchIoError(EFBIG, "native o1 journal page exceeds byte bound");
        }
        Cached& slot = cache[cache_next];
        auto& bytes = spill->read[cache_next];
        // Invalidate before replacing a page: a failed read/decode must never
        // leave a partially replaced event vector under its old cache key.
        slot.valid = false;
        slot.events.clear();
        slot.events.resize(static_cast<std::size_t>(page.count));
        if (page.length > kSpillPageBytes) {
            StreamLoadAr ar{data, bytes.data(), bytes.size(), page.offset, page.length};
            ar(slot.events.front());
        } else {
            data.read(page.offset, bytes.data(), static_cast<std::size_t>(page.length));
            ByteReader in(bytes.data(), static_cast<std::size_t>(page.length));
            LoadAr ar{in};
            for (CommandEvent& event : slot.events) ar(event);
        }
        cache_next = (cache_next + 1) % cache.size();
        slot.page_no = page_no;
        slot.first_position = page.first_position;
        slot.count = page.count;
        slot.valid = true;
        return slot.events;
    }
    // The retained event at an absolute position.
    const CommandEvent& at(uint64_t position) const {
        if (codec) return codec_at(position);
        if (position >= first) return resident_at(position);
        // A page already decoded answers without a look at the page table.
        for (const Cached& slot : cache) {
            if (slot.valid && position >= slot.first_position
                && position - slot.first_position < slot.count) {
                return slot.events[static_cast<std::size_t>(position - slot.first_position)];
            }
        }
        uint64_t page_no = 0;
        JournalPage page;
        if (!page_for_position(position, &page_no, &page)) {
            throw detail::ScratchIoError(EIO, "native o1 journal page is missing");
        }
        return load(page_no, page).at(static_cast<std::size_t>(position - page.first_position));
    }
    uint64_t ordinal_at(uint64_t position) const {
        if (!codec) return event_ordinal(at(position));
        detail::ScratchRecordInfo info;
        check(detail::journal_codec::find(*codec, position, info));
        const detail::journal_codec::StoreSource source(*codec, info.handle, info.value_bytes);
        detail::journal_codec::Reader reader{source, {codec_page.data(), codec_page.size()}};
        (void)reader.word(); // event alternative
        const auto ordinal = reader.word();
        check(reader.status);
        return ordinal;
    }
    // The first absolute position in [low, end) whose ordinal is above
    // `ordinal` (end when none); `low` is the window's first position and the
    // journal's ordinals ascend with positions. The resident part is bisected
    // directly; for a target before it the page table is bisected by ordinal
    // and exactly one page is read.
    uint64_t upper_bound_position(uint64_t ordinal, uint64_t low) const {
        if (codec) {
            uint64_t high = end;
            while (low < high) {
                const auto middle = low + (high - low) / 2;
                if (ordinal_at(middle) <= ordinal) low = middle + 1;
                else high = middle;
            }
            return low;
        }
        if (first < end && ordinal >= event_ordinal(resident_at(first))) {
            uint64_t lo = first;
            uint64_t hi = end;
            while (lo < hi) {
                const uint64_t middle = lo + (hi - lo) / 2;
                if (event_ordinal(resident_at(middle)) <= ordinal) lo = middle + 1;
                else hi = middle;
            }
            return lo;
        }
        // Every resident event is above the target; only spilled ones can be
        // at or below it, and only those from `low` on are retained.
        if (low >= first) return first;
        uint64_t lo = pages.base();
        uint64_t hi = pages.size();
        while (lo < hi) {
            const uint64_t middle = lo + (hi - lo) / 2;
            if (pages.get(middle).first_ordinal <= ordinal) lo = middle + 1;
            else hi = middle;
        }
        if (lo == pages.base()) return low;
        const uint64_t page_no = lo - 1;
        const JournalPage page = pages.get(page_no);
        const std::vector<CommandEvent>& events = load(page_no, page);
        std::size_t a = 0;
        std::size_t b = events.size();
        while (a < b) {
            const std::size_t middle = a + (b - a) / 2;
            if (event_ordinal(events[middle]) <= ordinal) a = middle + 1;
            else b = middle;
        }
        return std::max<uint64_t>(low, page.first_position + a);
    }
    std::optional<uint64_t> find_ordinal(uint64_t ordinal, uint64_t low) const {
        const uint64_t above = upper_bound_position(ordinal, low);
        if (above == low || ordinal_at(above - 1) != ordinal) return std::nullopt;
        return above - 1;
    }
};

void bind_journal_codec(WorkingRequestCore& core, detail::CommandScratchStore& store) {
    if (!core.journal_) throw std::invalid_argument("journal codec requires a bound run");
    core.journal_->bind(store);
    core.prepare_codec_frames();
}

ReadStatus append_journal_record(WorkingRequestCore& core,
                                const detail::ScratchByteSource& source) noexcept {
    if (!core.journal_ || !core.journal_->codec) return ReadStatus::programming_error;
    auto& journal = *core.journal_;
    if (journal.end == std::numeric_limits<uint64_t>::max())
        return ReadStatus::programming_error;
    uint64_t ordinal = 0;
    auto status = scan_journal_record(source,
        {journal.codec_page.data(), journal.codec_page.size()}, ordinal);
    if (status != ReadStatus::ok) return status;
    if (!ordinal || ordinal <= core.last_ordinal_) return ReadStatus::corrupt;
    status = detail::journal_codec::put(*journal.codec, journal.end, source);
    if (status == ReadStatus::ok) {
        ++journal.end;
        core.last_ordinal_ = ordinal;
    }
    return status;
}

const detail::ScratchReadback& journal_readback(const WorkingRequestCore& core) {
    if (!core.journal_ || !core.journal_->codec)
        throw std::logic_error("journal codec is not bound");
    return *core.journal_->codec;
}

// The group-effect keys: a resident tail over an external fixed-record log.
struct WorkingRequestCore::ReceiptStore {
    TailLog<ReceiptRec, 64> log;
};

// The id lists of resolutions that spent a long chain, and the index that
// finds a list by the ordinal of its resolution event.
struct WorkingRequestCore::ChainSegments {
    ExtFile ids;
    TailLog<ChainSegmentRec, 16> index;
    void set_directory(const std::string& directory) {
        ids.set_directory(directory);
        index.set_directory(directory);
    }
    bool find(uint64_t event_ordinal, ChainSegmentRec* out) const {
        uint64_t low = index.base();
        uint64_t high = index.size();
        while (low < high) {
            const uint64_t middle = low + (high - low) / 2;
            if (index.get(middle).event_ordinal < event_ordinal) low = middle + 1;
            else high = middle;
        }
        if (low == index.size()) return false;
        const ChainSegmentRec found = index.get(low);
        if (found.event_ordinal != event_ordinal) return false;
        *out = found;
        return true;
    }
};

// The working book's spilled rows: an ordered table keyed by priority (the
// repository's DiskIdTable, an AVL tree in a scratch file read by position, no
// resident nodes) whose values say where each row's serialized LiveRequest
// sits in an append-only blob file, plus a second table that maps the
// incarnation of a re-priced row to the priority it stands at. Entries of rows
// that came back are marked dead and skipped; the tables and the blob file are
// rebuilt from the live entries once the dead outweigh them, so the files stay
// within a constant factor of the rows they hold. A small ring keeps copies of
// the last spilled rows a reader asked for.
struct WorkingRequestCore::BookSpill {
    static constexpr std::size_t kSide = 8;
    detail::DiskIdTable<BookSlot> rows;
    detail::DiskIdTable<IncarnationSlot> links;
    ExtFile blobs;
    std::string directory;
    uint64_t alive = 0;     // rows spilled and not yet back: the book's spilled count
    uint64_t gone = 0;      // dead entries in rows
    uint64_t garbage = 0;   // blob bytes no live entry reads
    mutable std::array<std::shared_ptr<LiveRequest>, kSide> side{};
    mutable std::size_t side_next = 0;
    // A read failure a reader that cannot throw handed over
    // (carry_book_read_failure); every later read of these rows throws it.
    std::exception_ptr failure;
    // book_lookup_reads: rows read back and table steps walked by lookups.
    mutable uint64_t lookup_rows = 0;
    mutable uint64_t lookup_steps = 0;

    // Throws the carried failure, if any: an I/O failure as the typed
    // scratch failure (scratch_io_failed), anything else as it was thrown.
    void check() const {
        if (!failure) return;
        try {
            std::rethrow_exception(failure);
        } catch (const detail::AuxiliaryScratchIoError&) {
            throw;
        } catch (const std::system_error& error) {
            throw detail::AuxiliaryScratchIoError(error);
        }
    }

    void set_directory(const std::string& value) {
        directory = value;
        rows.set_directory(value);
        links.set_directory(value);
        blobs.set_directory(value);
    }
    // Writes `row` out and files it under its priority. The caller drops the
    // resident row once this has returned.
    void spill(const LiveRequest& row) {
        ByteWriter out;
        SaveAr ar{out};
        ar(row);
        const uint64_t priority = row.priority();
        const uint64_t incarnation = row.handle().incarnation;
        BookSlot slot;
        slot.offset = blobs.append(out.data(), out.size());
        slot.length = out.size();
        slot.incarnation = incarnation;
        if (priority != incarnation) {
            IncarnationSlot link;
            link.priority = priority;
            const auto linked = links.insert(book_key(incarnation), link);
            if (!linked.second) links.set(linked.first, link);
        }
        const auto placed = rows.insert(book_key(priority), slot);
        if (!placed.second) {
            // The row was spilled before and came back: its entry is revived.
            const BookSlot old = rows.get(placed.first);
            rows.set(placed.first, slot);
            if (old.state != 0) --gone;
            else garbage += old.length;
        }
    }
    LiveRequest read(const WorkingRequestCore& core, const BookSlot& slot) const {
        std::vector<unsigned char> bytes(static_cast<std::size_t>(slot.length));
        blobs.read(slot.offset, bytes.data(), bytes.size());
        ByteReader in(bytes.data(), bytes.size());
        LoadAr ar{in};
        LiveRequest row;
        ar(row);
        if (!row.definition || !core.journal_ || core.journal_->codec) return row;
        const JournalStore& journal = *core.journal_;
        // Restrict the existing ordinal bisection to resident events: no cold
        // journal reads or new index. One O(log kMaxResident) lookup per book
        // decode, then O(definition bytes) equality, never a per-child scan.
        const auto position = journal.find_ordinal(row.birth().acceptance_ordinal, journal.first);
        if (!position) return row;
        const CommandEvent& event = journal.resident_at(*position);
        const DefinitionRef* resident = nullptr;
        if (const auto* accepted = std::get_if<AcceptedEvent>(&event)) {
            resident = &accepted->definition;
        } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
            // A kept-handle re-price has the same handle but a new birth and
            // priority: only the successor can describe this book version.
            resident = &replaced->successor_definition;
        }
        if (!resident || !*resident || (*resident)->handle != row.handle()
            || (*resident)->birth != row.birth()
            || (*resident)->priority != row.definition->priority) return row;
        // LiveRequest's first field is its definition. Compare that entire
        // serialized prefix, including request (an owner's external openings
        // among it, handle by handle), predecessor, origin, root and
        // kept_binding; the decoded owner survives every mismatch unchanged.
        DefinitionMatchAr match{bytes.data(), bytes.size()};
        match(*resident);
        if (match.matched) row.definition = *resident;
        return row;
    }
    // Replaces the serialized row of the live entry at `at`, keeping its
    // priority. Bytes of the same length (a refreshed allowance usually is)
    // overwrite the old ones in place; anything else is appended and the
    // entry points at it, the old bytes becoming garbage.
    void rewrite(uint64_t at, BookSlot slot, const LiveRequest& row) {
        ByteWriter out;
        SaveAr ar{out};
        ar(row);
        if (out.size() == slot.length) {
            blobs.write(slot.offset, out.data(), out.size());
            return;
        }
        const uint64_t old_length = slot.length;
        slot.offset = blobs.append(out.data(), out.size());
        slot.length = out.size();
        rows.set(at, slot);
        garbage += old_length;
    }
    // The spilled row filed under `priority`.
    bool find(uint64_t priority, BookSlot* slot, uint64_t* at) const {
        const auto found = rows.find(book_key(priority));
        if (!found) return false;
        const BookSlot value = rows.get(*found);
        if (value.state != 0) return false;
        *slot = value;
        *at = *found;
        return true;
    }
    uint64_t priority_of(uint64_t incarnation) const {
        const auto found = links.find(book_key(incarnation));
        if (found) {
            const IncarnationSlot link = links.get(*found);
            if (link.state == 0) return link.priority;
        }
        return incarnation;
    }
    // The entry at `at` stops being a spilled row: it came back, or it ended.
    // Two steps, so the caller can make its own change between them. The row
    // entry is the one that publishes: retire_row writes first and may throw
    // while the caller has changed nothing; retired then counts the dead entry
    // and tombstones the link of a re-priced row. A link left standing by a
    // failed write is harmless: a lookup through it names a priority whose row
    // entry is dead, and BookSpill::find answers no row.
    void retire_row(uint64_t at, BookSlot slot) {
        slot.state = 1;
        rows.set(at, slot);
    }
    void retired(const BookSlot& slot, uint64_t priority) noexcept {
        garbage += slot.length;
        ++gone;
        if (priority == slot.incarnation) return;
        try {
            const auto found = links.find(book_key(slot.incarnation));
            if (found) {
                IncarnationSlot link = links.get(*found);
                link.state = 1;
                links.set(*found, link);
            }
        } catch (...) {
        }
    }
    uint64_t alive_from(uint64_t at) const {
        while (at != 0 && rows.get(at).state != 0) at = rows.next_sorted(at);
        return at;
    }
    uint64_t first_alive() const { return alive_from(rows.first_sorted()); }
    uint64_t next_alive(uint64_t at) const { return alive_from(rows.next_sorted(at)); }
    uint64_t priority_at(uint64_t at) const { return book_key_value(rows.key(at)); }
    // Keeps a copy of a spilled row alive for a reader; the last kSide copies
    // stay valid. keep hands the copy to a reader that holds it itself (the
    // ring and the reader share it, so it lives until both let go); stash
    // answers a bare pointer.
    std::shared_ptr<LiveRequest> keep(LiveRequest row) const {
        auto kept = std::make_shared<LiveRequest>(std::move(row));
        side[side_next] = kept;
        side_next = (side_next + 1) % side.size();
        return kept;
    }
    const LiveRequest* stash(LiveRequest row) const { return keep(std::move(row)).get(); }
    bool worth_compacting() const {
        return gone > 2 * alive + 64 || garbage > blobs.size() / 2 + (uint64_t{1} << 20);
    }
    // Rebuilds both tables and the blob file from the entries that still
    // stand. Table offsets taken before are no longer valid.
    void compact() {
        detail::DiskIdTable<BookSlot> fresh_rows;
        detail::DiskIdTable<IncarnationSlot> fresh_links;
        ExtFile fresh_blobs;
        if (!directory.empty()) {
            fresh_rows.set_directory(directory);
            fresh_links.set_directory(directory);
            fresh_blobs.set_directory(directory);
        }
        for (uint64_t at = first_alive(); at != 0; at = next_alive(at)) {
            BookSlot slot = rows.get(at);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(slot.length));
            blobs.read(slot.offset, bytes.data(), bytes.size());
            slot.offset = fresh_blobs.append(bytes.data(), bytes.size());
            const std::string key = rows.key(at);
            const uint64_t priority = book_key_value(key);
            fresh_rows.insert(key, slot);
            if (priority != slot.incarnation) {
                IncarnationSlot link;
                link.priority = priority;
                fresh_links.insert(book_key(slot.incarnation), link);
            }
        }
        rows = std::move(fresh_rows);
        links = std::move(fresh_links);
        blobs = std::move(fresh_blobs);
        gone = 0;
        garbage = 0;
    }
};

// What the stores know about the core (E02--E05). The core keeps no copy of
// it: the spilled count is the book spill's, the journal's length the
// journal's, the keys' base and count the key log's.
std::size_t WorkingRequestCore::spilled_count() const noexcept {
    return book_spill_ ? static_cast<std::size_t>(book_spill_->alive) : 0;
}

std::size_t WorkingRequestCore::live_size() const noexcept {
    return live_.size() + spilled_count();
}

std::size_t WorkingRequestCore::history_end() const noexcept {
    return journal_ ? static_cast<std::size_t>(journal_->end) : 0;
}

std::size_t WorkingRequestCore::group_effect_receipt_base() const noexcept {
    return receipt_store_ ? static_cast<std::size_t>(receipt_store_->log.base()) : 0;
}

std::size_t WorkingRequestCore::group_effect_receipt_count() const noexcept {
    return receipt_store_ ? static_cast<std::size_t>(receipt_store_->log.size()) : 0;
}

// ---- the working book's tiers (E02) ---------------------------------------

void WorkingRequestCore::forget_repriced(uint64_t incarnation) noexcept {
    const auto it = std::lower_bound(
            repriced_.begin(), repriced_.end(), incarnation,
            [](const std::pair<uint64_t, uint64_t>& row, uint64_t value) {
                return row.first < value;
            });
    if (it != repriced_.end() && it->first == incarnation) repriced_.erase(it);
}

// Room for one more resident row: spills the newest resident rows, one by one,
// until live_ is below its bound. Every spill writes before it drops, and
// everything that can throw comes before the row leaves live_, so a refusal
// leaves the book as it was. A re-priced row leaves repriced_ with it; the
// priority it stood at is filed in the spilled table's incarnation links.
// Rows leaving live_ shift what a prepared token indexed, so a spill raises
// the epoch: a token prepared before it installs as stale.
void WorkingRequestCore::make_book_room() {
    if (live_.size() < kBookMax) return;
    if (book_spill_) book_spill_->check();
    if (!book_spill_) {
        auto created = std::make_shared<BookSpill>();
        const std::string& directory = scratch_directory();
        if (!directory.empty()) created->set_directory(directory);
        book_spill_ = std::move(created);
    }
    BookSpill& spill = *book_spill_;
    if (spill.worth_compacting()) spill.compact();
    while (live_.size() >= kBookMax) {
        const LiveRequest& row = live_.back();
        const uint64_t incarnation = row.handle().incarnation;
        const bool repriced = row.priority() != incarnation;
        spill.spill(row);
        if (repriced) forget_repriced(incarnation);
        live_.pop_back();
        ++spill.alive;
        bump_epoch();
    }
}

// Brings the spilled request `incarnation` back into live_, at its place in
// priority order, and answers where. False, with the book untouched, when no
// spilled row stands under that incarnation. A table that is mostly dead
// entries is rebuilt first and a full live_ is made room for; both can move
// every table offset (never a row), so the entry is looked up again after
// them. Everything that can throw -- the rebuild, the room, the read, the
// reservations, the entry's tombstone -- comes before live_ changes; the
// insert and the counters after it cannot. The epoch rises: rows move in
// live_, so a token prepared before indexes stale places.
bool WorkingRequestCore::promote_book_row(uint64_t incarnation, std::size_t* index) {
    BookSpill& spill = *book_spill_;
    spill.check();
    const uint64_t priority = spill.priority_of(incarnation);
    BookSlot slot;
    uint64_t at = 0;
    if (!spill.find(priority, &slot, &at) || slot.incarnation != incarnation) return false;
    const bool rebuild = spill.worth_compacting();
    if (rebuild || live_.size() >= kBookMax) {
        if (rebuild) spill.compact();
        make_book_room();
        if (!spill.find(priority, &slot, &at)) {
            throw std::logic_error("native live book lost a spilled row");
        }
    }
    LiveRequest row = spill.read(*this, slot);
    reserve_n(live_, 1);
    if (priority != incarnation) reserve_n(repriced_, 1);
    spill.retire_row(at, slot);
    spill.retired(slot, priority);
    --spill.alive;
    const auto place = std::lower_bound(
            live_.begin(), live_.end(), priority,
            [](const LiveRequest& resident, uint64_t value) { return resident.priority() < value; });
    const std::size_t where = static_cast<std::size_t>(place - live_.begin());
    live_.insert(place, std::move(row));
    if (priority != incarnation) note_repriced(incarnation, priority);
    bump_epoch();
    *index = where;
    return true;
}

const LiveRequest* WorkingRequestCore::spilled_row(uint64_t incarnation) const {
    return spilled_held(incarnation).get();
}

// The spilled row `incarnation`, read back and kept in the ring as
// spilled_row keeps it; the answer holds the copy too, so it outlives the
// ring for a reader that keeps it (hold_live).
std::shared_ptr<LiveRequest> WorkingRequestCore::spilled_held(uint64_t incarnation) const {
    const BookSpill& spill = *book_spill_;
    spill.check();
    const uint64_t priority = spill.priority_of(incarnation);
    BookSlot slot;
    uint64_t at = 0;
    if (!spill.find(priority, &slot, &at) || slot.incarnation != incarnation) return nullptr;
    ++spill.lookup_rows;
    return spill.keep(spill.read(*this, slot));
}

const LiveRequest* WorkingRequestCore::find_resident(const RequestHandle& handle) const noexcept {
    if (!repriced_.empty()) return find_repriced(handle.incarnation);
    const std::size_t index = book_position<false>(live_, handle.incarnation);
    return index != live_.size() ? &live_[index] : nullptr;
}

namespace {
// A resident row in the held form hold_live answers: it owns nothing and is
// valid, as a pointer find_live answers, until the next command.
std::shared_ptr<const LiveRequest> unowned_live(const LiveRequest& row) noexcept {
    return std::shared_ptr<const LiveRequest>(std::shared_ptr<const LiveRequest>(), &row);
}
}  // namespace

std::shared_ptr<const LiveRequest> WorkingRequestCore::hold_live(
        const RequestHandle& handle) const {
    if (!names_run(handle, identity_)) return nullptr;
    if (const LiveRequest* resident = find_resident(handle)) return unowned_live(*resident);
    if (spilled_count() == 0) return nullptr;
    return spilled_held(handle.incarnation);
}

bool WorkingRequestCore::carry_book_read_failure(std::exception_ptr failure) const noexcept {
    if (!book_spill_ || !failure) return false;
    if (!book_spill_->failure) book_spill_->failure = std::move(failure);
    return true;
}

void WorkingRequestCore::rethrow_book_read_failure() const {
    if (book_spill_) book_spill_->check();
}

WorkingRequestCore::BookLookupReads WorkingRequestCore::book_lookup_reads() const noexcept {
    BookLookupReads reads;
    if (book_spill_) {
        reads.rows = book_spill_->lookup_rows;
        reads.steps = book_spill_->lookup_steps;
    }
    return reads;
}

WorkingRequestCore::BookCursor WorkingRequestCore::book_begin() const {
    BookCursor cursor;
    if (spilled_count() != 0) {
        book_spill_->check();
        cursor.at = book_spill_->first_alive();
        if (cursor.at != 0) cursor.spilled_priority = book_spill_->priority_at(cursor.at);
    }
    return cursor;
}

bool WorkingRequestCore::book_done(const BookCursor& cursor) const noexcept {
    return cursor.r == live_.size() && cursor.at == 0;
}

bool WorkingRequestCore::book_next_spilled(const BookCursor& cursor) const noexcept {
    return cursor.at != 0
        && (cursor.r == live_.size() || cursor.spilled_priority < live_[cursor.r].priority());
}

void WorkingRequestCore::book_step(BookCursor& cursor) const {
    if (book_next_spilled(cursor)) {
        cursor.at = book_spill_->next_alive(cursor.at);
        cursor.spilled_priority = cursor.at != 0 ? book_spill_->priority_at(cursor.at) : 0;
    } else {
        ++cursor.r;
    }
}

template <class Visit>
void WorkingRequestCore::visit_book(Visit&& visit) const {
    if (spilled_count() == 0) {
        for (const LiveRequest& row : live_) {
            if (!visit(row)) return;
        }
        return;
    }
    BookCursor cursor = book_begin();
    while (!book_done(cursor)) {
        if (book_next_spilled(cursor)) {
            const LiveRequest row = book_spill_->read(*this, book_spill_->rows.get(cursor.at));
            if (!visit(row)) return;
        } else if (!visit(live_[cursor.r])) {
            return;
        }
        book_step(cursor);
    }
}

const LiveRequest& WorkingRequestCore::live_at(std::size_t rank) const {
    if (rank >= live_size()) throw std::out_of_range("native live book rank is not live");
    if (spilled_count() == 0) return live_[rank];
    BookCursor cursor = book_begin();
    for (std::size_t skipped = 0; skipped != rank; ++skipped) {
        if (book_next_spilled(cursor)) ++book_spill_->lookup_steps;
        book_step(cursor);
    }
    if (book_next_spilled(cursor)) {
        ++book_spill_->lookup_rows;
        return *book_spill_->stash(book_spill_->read(*this, book_spill_->rows.get(cursor.at)));
    }
    return live_[cursor.r];
}

const LiveRequest& WorkingRequestCore::live_back() const {
    if (spilled_count() == 0) {
        if (live_.empty()) throw std::out_of_range("native live book is empty");
        return live_.back();
    }
    book_spill_->check();
    uint64_t last = 0;
    for (uint64_t at = book_spill_->first_alive(); at != 0; at = book_spill_->next_alive(at)) {
        ++book_spill_->lookup_steps;
        last = at;
    }
    if (live_.empty() || live_.back().priority() < book_spill_->priority_at(last)) {
        ++book_spill_->lookup_rows;
        return *book_spill_->stash(book_spill_->read(*this, book_spill_->rows.get(last)));
    }
    return live_.back();
}

WorkingRequestCore::LiveView WorkingRequestCore::live() const noexcept {
    return LiveView(this);
}

void WorkingRequestCore::LiveView::const_iterator::settle() {
    held_.reset();
    if (core_->book_done(cursor_)) return;
    if (core_->book_next_spilled(cursor_)) {
        // Through the ring, so a reference the view handed out stays valid
        // for the next few spilled rows it reads, as the header promises.
        held_ = core_->book_spill_->keep(core_->book_spill_->read(
                *core_, core_->book_spill_->rows.get(cursor_.at)));
    }
}

const LiveRequest& WorkingRequestCore::LiveView::const_iterator::operator*() const {
    return held_ ? *held_ : core_->live_[cursor_.r];
}

std::shared_ptr<const LiveRequest> WorkingRequestCore::LiveView::const_iterator::hold() const {
    if (held_) return held_;
    return unowned_live(core_->live_[cursor_.r]);
}

WorkingRequestCore::LiveView::const_iterator&
WorkingRequestCore::LiveView::const_iterator::operator++() {
    core_->book_step(cursor_);
    settle();
    return *this;
}

WorkingRequestCore::LiveView::const_iterator WorkingRequestCore::LiveView::begin() const {
    const_iterator first(core_, core_->book_begin());
    first.settle();
    return first;
}

WorkingRequestCore::LiveView::const_iterator WorkingRequestCore::LiveView::end() const {
    BookCursor cursor;
    cursor.r = core_->live_.size();
    return const_iterator(core_, cursor);
}

WorkingRequestCore::LiveView::operator std::vector<LiveRequest>() const {
    std::vector<LiveRequest> rows;
    rows.reserve(core_->live_size());
    core_->visit_book([&rows](const LiveRequest& row) {
        rows.push_back(row);
        return true;
    });
    return rows;
}

// A request stands at its own incarnation until a re-price keeps its handle;
// those, and only those, are named in repriced_. With none named the book is
// in incarnation order, and classify / find_live search it by handles alone
// (book_position<false>); otherwise they search it here, by priority.
std::size_t WorkingRequestCore::repriced_position(uint64_t incarnation) const noexcept {
    uint64_t priority = incarnation;
    const auto it = std::lower_bound(
            repriced_.begin(), repriced_.end(), incarnation,
            [](const std::pair<uint64_t, uint64_t>& row, uint64_t value) {
                return row.first < value;
            });
    if (it != repriced_.end() && it->first == incarnation) priority = it->second;
    const std::size_t index = book_position<true>(live_, priority);
    // The row found must be the request asked for. A re-price takes a number
    // no handle is issued under, so the row at a handle's own incarnation is
    // that handle's -- except when the "handle" names such a number itself,
    // which is no request's handle and must answer NotWorking.
    if (index != live_.size() && live_[index].handle().incarnation != incarnation) {
        return live_.size();
    }
    return index;
}

void WorkingRequestCore::note_repriced(uint64_t incarnation, uint64_t priority) noexcept {
    const auto it = std::lower_bound(
            repriced_.begin(), repriced_.end(), incarnation,
            [](const std::pair<uint64_t, uint64_t>& row, uint64_t value) {
                return row.first < value;
            });
    if (it != repriced_.end() && it->first == incarnation) {
        it->second = priority;
        return;
    }
    repriced_.insert(it, {incarnation, priority});
    // Prune the entries of requests that have left the book, once they
    // outnumber the rows: amortized one lookup per re-price, and the table
    // never holds more than twice the book.
    if (repriced_.size() > 2 * live_.size() + 8) {
        repriced_.erase(std::remove_if(repriced_.begin(), repriced_.end(),
                                       [&](const std::pair<uint64_t, uint64_t>& row) {
                                           const std::size_t at = book_position<true>(live_, row.second);
                                           return at == live_.size()
                                               || live_[at].handle().incarnation != row.first;
                                       }),
                        repriced_.end());
    }
}

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
    book_spill_.reset();
    journal_.reset();
    last_ordinal_ = 0;
    last_incarnation_ = 0;
    history_base_ = 0;
    retired_through_ = 0;
    chain_floor_ = kNoChainFloor;
    chain_floor_exact_ = true;
    chain_segments_.reset();
    issued_.clear();
    successor_roots_.reset();
    repriced_.clear();
    receipt_store_.reset();
    next_cohort_handle_ = 1;
    cohorts_.clear();
    cohort_receipts_.clear();
    cohort_receipts_base_ = 0;
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
    plan.history_size = history_end();
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

// The journal is no longer one vector a caller can presize: its resident part
// has a fixed bound and the rest is external. Still reserve the requested
// retained-window capacity while it fits, so preallocated command output does
// not allocate chunks during the bounded operation. A beyond-resident
// reservation also prepares the fixed page buffers and initial scratch extents;
// it does not raise the resident event bound.
void WorkingRequestCore::reserve(std::size_t expected_events) {
    if (journal_ && !journal_->codec && expected_events > JournalStore::kMaxResident) {
        journal_->prepare_spill();
    }
    if (journal_ && expected_events > journal_->end - history_base_) {
        const auto resident = journal_->resident_count();
        if (resident < JournalStore::kMaxResident) {
            const auto available = JournalStore::kMaxResident - resident;
            const auto incoming = std::min<std::size_t>(
                expected_events - (static_cast<std::size_t>(journal_->end) - history_base_),
                available);
            journal_->make_room(incoming);
        }
    }
    if (live_.capacity() < 8) live_.reserve(8);
}

PreparedMutation WorkingRequestCore::finish_mutation(MutationPlan plan) {
    seal_plan(plan);
    auto impl = std::make_unique<PreparedMutation::Impl>();
    impl->plan = std::move(plan);
    return PreparedMutation(std::move(impl));
}

// The direct forms (R5 lane L3) do what a plan's prepare and install do, in
// the order they do it, without the plan: begin_plan's checks where the
// prepare_* makes them, then seal_plan's reservation and epoch, then commit's
// appends, row change, receipt and counters, then commit's epoch. Every event
// is built before the seal, so whatever can throw has thrown before anything
// is written, exactly as a prepare that throws leaves the core untouched.
void WorkingRequestCore::begin_direct() const {
    require_identity(identity_);
    if (!instance_ || instance_->expired) {
        throw std::invalid_argument(
                "native run identity requires a nonempty session key and positive run number");
    }
    require_epoch_room();
}

// The one place an event enters the journal. Storage was set aside by
// reserve_plan or seal_direct. A bound codec publishes through its borrowed store.
void WorkingRequestCore::append_event(CommandEvent&& event) {
    journal_->push(std::move(event));
    note_chain_birth(journal_->resident_at(journal_->end - 1));
}

void WorkingRequestCore::note_chain_birth(const CommandEvent& event) noexcept {
    // A deferred link with no predecessor and a positive delta opens a chain
    // (an absorbed delta carries no predecessor either but joins none, and
    // there is then an older head already).
    const auto* deferred = std::get_if<DeferredGroupAdjustmentEvent>(&event);
    if (!deferred || deferred->previous_pending_receipt || !(deferred->deferred_delta > 0.0)) {
        return;
    }
    if (!chain_floor_exact_) return;
    if (deferred->ordinal < chain_floor_) chain_floor_ = deferred->ordinal;
}

void WorkingRequestCore::note_chain_loss(const LiveRequest& row) noexcept {
    if (std::holds_alternative<PendingDeferred>(row.pending)) chain_floor_exact_ = false;
}

void WorkingRequestCore::erase_live(std::size_t index) noexcept {
    note_chain_loss(live_[index]);
    live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(index));
}

template <class Event>
void WorkingRequestCore::append_direct(Event&& event, bool consume_incarnation) {
    static_assert(!std::is_lvalue_reference_v<Event>, "an event is appended by move");
    const uint64_t ordinal = event.ordinal;
    append_event(CommandEvent(std::forward<Event>(event)));
    if (consume_incarnation) index_committed(journal_->resident_at(journal_->end - 1));
    last_ordinal_ = ordinal;
}

EventRange WorkingRequestCore::finish_direct(std::size_t first) noexcept {
    bump_epoch();
    return EventRange{first, history_end() - first};
}

// One event that ends a live request: sealed, appended, the row erased.
template <class Event>
Installed WorkingRequestCore::end_direct(std::size_t live_index, Event&& event) {
    seal_direct(1, false, false, false);
    const std::size_t first = history_end();
    append_direct(std::forward<Event>(event), false);
    erase_live(live_index);
    return Installed{finish_direct(first)};
}

WorkingRequestCore::TargetKind WorkingRequestCore::classify_resident(
        const RequestHandle& handle, std::size_t* live_index) const {
    if (!repriced_.empty()) return classify_repriced(handle.incarnation, live_index);
    const std::size_t index = book_position<false>(live_, handle.incarnation);
    if (index == live_.size()) return TargetKind::NotWorking;
    if (live_index) *live_index = index;
    return TargetKind::Live;
}

// A request outside the heap is brought back before the command that names it
// reads or writes its row, so prepare_*/apply_* work on live_[index] as they
// always did. The one place the book changes outside a commit: it raises the
// epoch (promote_book_row), so a token prepared before it cannot install
// against the shifted indices.
WorkingRequestCore::TargetKind WorkingRequestCore::classify(
        const RequestHandle& handle, std::size_t* live_index) {
    if (!names_run(handle, identity_)) return TargetKind::InvalidHandle;
    const TargetKind resident = classify_resident(handle, live_index);
    if (resident == TargetKind::Live || spilled_count() == 0) return resident;
    std::size_t index = 0;
    if (!promote_book_row(handle.incarnation, &index)) return TargetKind::NotWorking;
    if (live_index) *live_index = index;
    return TargetKind::Live;
}

// classify and find_live once a re-priced request is live (repriced_position).
WorkingRequestCore::TargetKind WorkingRequestCore::classify_repriced(
        uint64_t incarnation, std::size_t* live_index) const noexcept {
    const std::size_t index = repriced_position(incarnation);
    if (index == live_.size()) return TargetKind::NotWorking;
    if (live_index) *live_index = index;
    return TargetKind::Live;
}

const LiveRequest* WorkingRequestCore::find_repriced(uint64_t incarnation) const noexcept {
    const std::size_t index = repriced_position(incarnation);
    return index == live_.size() ? nullptr : &live_[index];
}

// classify's lookup without the call to classify: every lookup the consumer
// makes passes through here.
const LiveRequest* WorkingRequestCore::find_live(const RequestHandle& handle) const {
    if (!names_run(handle, identity_)) return nullptr;
    const LiveRequest* resident = nullptr;
    if (!repriced_.empty()) {
        resident = find_repriced(handle.incarnation);
    } else {
        const std::size_t index = book_position<false>(live_, handle.incarnation);
        if (index != live_.size()) resident = &live_[index];
    }
    if (resident || spilled_count() == 0) return resident;
    // A read cannot bring a row back: a copy of the spilled row is kept for
    // the caller (see BookSpill::stash).
    return spilled_row(handle.incarnation);
}

// The journal's ordinals ascend with its positions, so the lookup is a bisection
// over the retained window: its resident events directly, its older ones
// through the page table and one page read.
const CommandEvent* WorkingRequestCore::event_at(const EventId& id) const {
    if (id.run != identity_ || id.ordinal == 0 || !journal_) return nullptr;
    const auto position = journal_->find_ordinal(id.ordinal, history_base_);
    if (!position) return nullptr;
    return &journal_->at(*position);
}

const CommandEvent& WorkingRequestCore::history_at(std::size_t absolute) const {
    if (!journal_ || absolute < history_base_ || absolute >= history_end()) {
        throw std::out_of_range("native journal position is not retained");
    }
    return journal_->at(absolute);
}

WorkingRequestCore::JournalView WorkingRequestCore::history() const noexcept {
    return JournalView(this);
}

// A live deferred group-adjustment chain is re-read event by event whenever
// its request resolves terms (collect_pending_chain), so its head is the
// oldest event retire_history must keep. The receipts link back through
// previous_pending_receipt with strictly decreasing ordinals. The head is
// cached: a chain's first link raises no floor above an older head
// (note_chain_birth) and only a chain that left its row makes the cache a
// lower bound that is walked again (note_chain_loss), so a long chain is not
// re-walked at every retirement.
std::optional<uint64_t> WorkingRequestCore::pending_chain_floor() const {
    if (chain_floor_exact_) {
        if (chain_floor_ == kNoChainFloor) return std::nullopt;
        return chain_floor_;
    }
    std::optional<uint64_t> floor;
    // Every live request, a spilled one read back for its visit.
    visit_book([&](const LiveRequest& live) {
        const auto* deferred = std::get_if<PendingDeferred>(&live.pending);
        if (!deferred || deferred->count == 0) return true;
        EventId cursor = deferred->tail_receipt;
        uint64_t head = cursor.ordinal;
        for (uint64_t i = 1; i < deferred->count; ++i) {
            const CommandEvent* event = event_at(cursor);
            const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
            if (!payload || !payload->previous_pending_receipt
                || payload->previous_pending_receipt->ordinal >= cursor.ordinal) {
                break;
            }
            cursor = *payload->previous_pending_receipt;
            head = cursor.ordinal;
        }
        if (!floor || head < *floor) floor = head;
        return true;
    });
    chain_floor_ = floor ? *floor : kNoChainFloor;
    chain_floor_exact_ = true;
    return floor;
}

// Everything that reads an external page happens before the first change, so
// a failed read leaves the window as it was: the consumer calls this from a
// noexcept boundary, and a retirement that cannot read just retires nothing
// this time (the window stays longer, no evidence is dropped).
std::size_t WorkingRequestCore::retire_history(uint64_t through_ordinal) {
    if (!journal_ || history_base_ == history_end()) return 0;
    try {
        uint64_t limit = through_ordinal;
        if (const auto floor = pending_chain_floor()) {
            if (*floor == 0) return 0;
            limit = std::min(limit, *floor - 1);
        }
        const uint64_t cut = journal_->upper_bound_position(limit, history_base_);
        const std::size_t count = static_cast<std::size_t>(cut - history_base_);
        if (count == 0) return 0;
        const uint64_t newest = journal_->ordinal_at(cut - 1);
        const std::size_t receipts_base = receipts_first_above(newest);
        // Only a bound codec's erase can throw here, and one that throws has
        // changed nothing: the store rolls its transaction back and the
        // journal's first position moves only after the commit. So
        // retired_through_ and the window move after it, never ahead of it.
        if (cut > journal_->first) journal_->drop_front(cut);
        // Nothing below can throw.
        retired_through_ = newest;
        if (cut >= journal_->first && journal_->pages.size() != 0) journal_->release_pages();
        history_base_ = static_cast<std::size_t>(cut);
        apply_receipt_base(receipts_base);
        return count;
    } catch (...) {
        return 0;
    }
}

std::size_t WorkingRequestCore::receipts_first_above(uint64_t retired) const {
    uint64_t low = group_effect_receipt_base();
    uint64_t high = group_effect_receipt_count();
    while (low < high) {
        const uint64_t middle = low + (high - low) / 2;
        if (receipt_store_->log.get(middle).cause <= retired) low = middle + 1;
        else high = middle;
    }
    return static_cast<std::size_t>(low);
}

void WorkingRequestCore::apply_receipt_base(std::size_t base) noexcept {
    if (!receipt_store_ || base <= receipt_store_->log.base()) return;
    receipt_store_->log.retire_to(base);
}

void WorkingRequestCore::ensure_receipt_store() {
    if (receipt_store_) return;
    auto store = std::make_shared<ReceiptStore>();
    const std::string& directory = scratch_directory();
    if (!directory.empty()) store->log.set_directory(directory);
    receipt_store_ = std::move(store);
}

void WorkingRequestCore::prepare_codec_frames() {
    // Fixed construction frames for ordinary commands and their bounded tails.
    // Larger working books continue through the existing book spill path.
    PreparedImplStorage<PreparedSubmitStorageTag>::prepare(sizeof(PreparedSubmit::Impl));
    PreparedImplStorage<PreparedReplaceStorageTag>::prepare(sizeof(PreparedReplace::Impl));
    PreparedImplStorage<PreparedCancelStorageTag>::prepare(sizeof(PreparedCancel::Impl));
    PreparedImplStorage<PreparedMutationStorageTag>::prepare(sizeof(PreparedMutation::Impl));
    PreparedImplStorage<PreparedExecutionStorageTag>::prepare(sizeof(PreparedExecution::Impl));
    live_.reserve(4);
    issued_.reserve(1);
    ensure_receipt_store();
    ensure_chain_segments();
    reserve_successor_root();
}

void WorkingRequestCore::ensure_chain_segments() {
    if (chain_segments_) return;
    auto segments = std::make_shared<ChainSegments>();
    const std::string& directory = scratch_directory();
    if (!directory.empty()) segments->set_directory(directory);
    chain_segments_ = std::move(segments);
}

void WorkingRequestCore::note_chain_segment(uint64_t event_ordinal, uint64_t offset,
                                            uint64_t count) noexcept {
    chain_segments_->index.push(ChainSegmentRec{event_ordinal, offset, count});
}

// The chain index. Incarnations are issued in increasing order (usable_
// incarnation), so both tables only ever grow at their back.
void WorkingRequestCore::index_committed(const CommandEvent& event) noexcept {
    const RequestDefinition* definition = nullptr;
    if (const auto* accepted = std::get_if<AcceptedEvent>(&event)) {
        definition = accepted->definition.get();
    } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
        // A re-price that kept its handle issued none: the number it took is
        // its priority, and the chain it belongs to is unchanged.
        if (replaced->kept_handle()) return;
        definition = replaced->successor_definition.get();
    }
    if (!definition) return;
    const uint64_t incarnation = definition->handle.incarnation;
    if (!issued_.empty() && issued_.back().second + 1 == incarnation) {
        issued_.back().second = incarnation;
    } else {
        issued_.push_back({incarnation, incarnation});
    }
    if (definition->root) append_successor_root(incarnation, definition->root->incarnation);
}

bool WorkingRequestCore::issued(const RequestHandle& handle) const noexcept {
    if (handle.incarnation == 0 || handle.run != identity_) return false;
    const auto it = std::upper_bound(
            issued_.begin(), issued_.end(), handle.incarnation,
            [](uint64_t incarnation, const std::pair<uint64_t, uint64_t>& range) {
                return incarnation < range.first;
            });
    return it != issued_.begin() && handle.incarnation <= std::prev(it)->second;
}

// A request's chain root, without its predecessors' definitions: the live
// row carries RequestDefinition::root, and the chain index keeps the root of
// every replace successor that is no longer working. A request that replaced
// nothing is its own root. Rosters hold roots only (cohort_add inserts this
// answer), so a membership test needs nothing but it.
std::optional<RequestHandle> WorkingRequestCore::canonical_cohort_origin(
        const RequestHandle& origin) const {
    if (!issued(origin)) return std::nullopt;
    if (const auto* live = find_live(origin)) {
        const auto& root = live->definition->root;
        return root ? *root : live->handle();
    }
    // Only a terminal origin reaches this read. The bounded tail is tried
    // before the sorted file's pread search; no caller reads a file cursor.
    // The consumer hashes roots at commit, independently of their storage.
    // I/O errors propagate rather than treating a successor as its own root.
    const auto root = successor_root(origin.incarnation);
    if (!root) return origin;
    return RequestHandle{identity_, *root};
}

WorkingRequestCore::CohortView WorkingRequestCore::cohorts() const noexcept {
    return CohortView(this);
}

WorkingRequestCore::CohortView::const_iterator::reference
WorkingRequestCore::CohortView::const_iterator::operator*() const noexcept {
    assert(core_ && index_ < core_->next_cohort_handle_ - 1);
    const auto handle = static_cast<std::uint64_t>(index_) + 1;
    const auto found = core_->cohorts_.find(handle);
    if (found != core_->cohorts_.end()) return found->second;
    empty_.handle = CohortHandle{handle};
    return empty_;
}

bool WorkingRequestCore::cohort_exists(CohortHandle cohort) const noexcept {
    return cohort.value > 0 && cohort.value < next_cohort_handle_;
}

CohortHandle WorkingRequestCore::cohort_open() {
    require_identity(identity_);
    require_epoch_room();
    if (next_cohort_handle_ == 0 || next_cohort_handle_ == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native cohort handle exhausted");
    }
    const CohortHandle handle{next_cohort_handle_};
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
    if (!cohort_exists(cohort)) {
        receipt.status = CohortReceiptStatus::InvalidHandle;
    } else if (!issued(origin)) {
        receipt.status = CohortReceiptStatus::UnknownOrigin;
    } else if (!find_live(origin)) {
        receipt.status = CohortReceiptStatus::TerminalOrigin;
    } else if (const auto canonical = canonical_cohort_origin(origin)) {
        const auto found = cohorts_.find(cohort.value);
        if (found == cohorts_.end()) {
            cohorts_.emplace(cohort.value, CohortRoster{cohort, {*canonical}});
        } else {
            auto& origins = found->second.origins;
            const auto where = std::lower_bound(origins.begin(), origins.end(), *canonical, handle_less);
            if (where == origins.end() || *where != *canonical) origins.insert(where, *canonical);
        }
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
    if (!cohort_exists(cohort)) {
        receipt.status = CohortReceiptStatus::InvalidHandle;
    } else if (const auto canonical = canonical_cohort_origin(origin)) {
        const auto found = cohorts_.find(cohort.value);
        if (found != cohorts_.end()) {
            auto& origins = found->second.origins;
            const auto where = std::lower_bound(origins.begin(), origins.end(), *canonical, handle_less);
            if (where != origins.end() && *where == *canonical) origins.erase(where);
            if (origins.empty()) cohorts_.erase(found);
        }
        receipt.status = CohortReceiptStatus::Applied;
    } else {
        receipt.status = CohortReceiptStatus::UnknownOrigin;
    }
    cohort_receipts_.push_back(std::move(receipt));
    if (!bump_epoch()) throw std::overflow_error("native working-request epoch exhausted");
}

bool WorkingRequestCore::cohort_contains(
        CohortHandle cohort, const RequestHandle& opening) const {
    if (!cohort_exists(cohort)) return false;
    const auto root = canonical_cohort_origin(opening);
    if (!root) return false;
    const auto found = cohorts_.find(cohort.value);
    if (found == cohorts_.end()) return false;
    const auto& origins = found->second.origins;
    return std::binary_search(origins.begin(), origins.end(), *root, handle_less);
}

std::size_t WorkingRequestCore::retire_cohort_receipts(std::size_t through) noexcept {
    if (through <= cohort_receipts_base_) return 0;
    const std::size_t count = std::min(through - cohort_receipts_base_, cohort_receipts_.size());
    cohort_receipts_.erase(cohort_receipts_.begin(),
                           cohort_receipts_.begin() + static_cast<std::ptrdiff_t>(count));
    cohort_receipts_base_ += count;
    return count;
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

// The chain walk of a request that resolves its units (E04). Two passes and
// no vector of every id: back from the tail the receipts are checked link by
// link (their ids go inline for a short chain; for a longer one they are
// written in blocks into an external list, backward into their final places so
// it ends up in chain order), then the chain is replayed forward through
// those ids to reproduce the total's exact accumulation.
bool WorkingRequestCore::collect_pending_chain(const PendingAdjustments& pending,
                                               const RequestHandle& recipient,
                                               ChainIds* ids,
                                               double* total) {
    ids->inline_ids.clear();
    ids->count = 0;
    ids->external = false;
    ids->offset = 0;
    *total = 0.0;
    const auto* deferred = std::get_if<PendingDeferred>(&pending);
    if (!deferred) return true;
    if (deferred->count == 0 || deferred->tail_receipt.ordinal == 0
        || deferred->tail_receipt.run != identity_) {
        return false;
    }
    const uint64_t count = deferred->count;
    const bool external = count > kInlineAdjustmentIds;
    if (external && count > std::numeric_limits<uint64_t>::max() / sizeof(uint64_t)) return false;
    constexpr std::size_t kBlock = 512;
    uint64_t region = 0;
    std::array<uint64_t, kBlock> block{};
    std::size_t block_size = 0;
    std::vector<EventId> local;
    if (external) {
        ensure_chain_segments();
        region = chain_segments_->ids.extend(count * sizeof(uint64_t));
    } else {
        local.reserve(static_cast<std::size_t>(count));
    }
    uint64_t written = 0;
    // `block` holds the ids discovered since the last flush, tail first: the
    // k-th id found from the tail is the (count - 1 - k)-th of the chain.
    const auto flush = [&]() {
        if (block_size == 0) return;
        std::reverse(block.begin(), block.begin() + block_size);
        const uint64_t first_forward = count - written - block_size;
        chain_segments_->ids.write(region + first_forward * sizeof(uint64_t), block.data(),
                                   block_size * sizeof(uint64_t));
        written += block_size;
        block_size = 0;
    };
    EventId cursor = deferred->tail_receipt;
    for (uint64_t i = 0; i < count; ++i) {
        const CommandEvent* event = event_at(cursor);
        const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
        if (!payload || payload->recipient != recipient || payload->effect != GroupEffect::Reduce
            || !payload->definition || payload->definition->handle != recipient) {
            return false;
        }
        if (external) {
            block[block_size++] = cursor.ordinal;
            if (block_size == kBlock) flush();
        } else {
            local.push_back(cursor);
        }
        if (i + 1 < count) {
            if (!payload->previous_pending_receipt
                || payload->previous_pending_receipt->ordinal >= cursor.ordinal) return false;
            cursor = *payload->previous_pending_receipt;
        } else if (payload->previous_pending_receipt) {
            return false;
        }
    }
    if (external) flush();
    else std::reverse(local.begin(), local.end());
    // Reproduce the exact accumulation order used when each receipt was
    // committed. Reassociating these additions tail-first changes binary64
    // totals for valid chains such as 0.1, 0.2, 0.3. An absorbed delta joined
    // no chain, so every link here moved the total (pending_sum).
    double sum = 0.0;
    std::array<uint64_t, kBlock> window{};
    std::size_t window_size = 0;
    uint64_t window_first = 0;
    EventId previous;
    for (uint64_t i = 0; i < count; ++i) {
        EventId id;
        if (external) {
            if (i >= window_first + window_size) {
                const uint64_t n = std::min<uint64_t>(kBlock, count - i);
                window_size = static_cast<std::size_t>(n);
                chain_segments_->ids.read(region + i * sizeof(uint64_t), window.data(),
                                          window_size * sizeof(uint64_t));
                window_first = i;
            }
            id = EventId{identity_, window[static_cast<std::size_t>(i - window_first)]};
        } else {
            id = local[static_cast<std::size_t>(i)];
        }
        const CommandEvent* event = event_at(id);
        const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
        if (!payload || !finite_positive(payload->deferred_delta)) return false;
        if (i == 0) {
            if (!std::holds_alternative<PendingNone>(payload->pending_before)) return false;
        } else {
            const auto* before = std::get_if<PendingDeferred>(&payload->pending_before);
            if (!before || before->total != sum || before->count != i
                || before->tail_receipt != previous) return false;
        }
        if (!pending_sum(sum, payload->deferred_delta, &sum)) return false;
        if (payload->pending_after.total != sum || payload->pending_after.count != i + 1
            || payload->pending_after.tail_receipt != id) return false;
        previous = id;
    }
    if (sum != deferred->total) return false;
    *total = sum;
    ids->count = count;
    ids->external = external;
    ids->offset = region;
    if (!external) ids->inline_ids = std::move(local);
    return true;
}

bool WorkingRequestCore::visit_prior_adjustments(
        const EventId& resolution, const std::function<bool(const EventId&)>& visit) const {
    const CommandEvent* event = event_at(resolution);
    if (!event) return false;
    // Copied first: the event may sit in a cached page that the visitor's own
    // reads evict.
    std::vector<EventId> inline_ids;
    if (const auto* terms = std::get_if<TermsResolvedEvent>(event)) {
        inline_ids = terms->prior_adjustment_ids;
    } else if (const auto* bound = std::get_if<QuantityBoundEvent>(event)) {
        inline_ids = bound->prior_adjustment_ids;
    } else {
        return false;
    }
    if (!inline_ids.empty()) {
        for (const EventId& id : inline_ids) {
            if (!visit(id)) break;
        }
        return true;
    }
    ChainSegmentRec segment;
    if (!chain_segments_ || !chain_segments_->find(resolution.ordinal, &segment)) return true;
    constexpr std::size_t kBlock = 512;
    std::array<uint64_t, kBlock> window{};
    for (uint64_t at = 0; at < segment.count;) {
        const uint64_t n = std::min<uint64_t>(kBlock, segment.count - at);
        chain_segments_->ids.read(segment.offset + at * sizeof(uint64_t), window.data(),
                                  static_cast<std::size_t>(n) * sizeof(uint64_t));
        for (std::size_t i = 0; i != n; ++i) {
            if (!visit(EventId{identity_, window[i]})) return true;
        }
        at += n;
    }
    return true;
}

std::size_t WorkingRequestCore::prior_adjustment_count(const EventId& resolution) const {
    const CommandEvent* event = event_at(resolution);
    if (!event) return 0;
    if (const auto* terms = std::get_if<TermsResolvedEvent>(event)) {
        if (!terms->prior_adjustment_ids.empty()) return terms->prior_adjustment_ids.size();
    } else if (const auto* bound = std::get_if<QuantityBoundEvent>(event)) {
        if (!bound->prior_adjustment_ids.empty()) return bound->prior_adjustment_ids.size();
    } else {
        return 0;
    }
    ChainSegmentRec segment;
    if (!chain_segments_ || !chain_segments_->find(resolution.ordinal, &segment)) return 0;
    return static_cast<std::size_t>(segment.count);
}

GroupEffectReceipt WorkingRequestCore::group_effect_receipt(std::size_t index) const {
    const std::size_t base = group_effect_receipt_base();
    assert(index >= base && "native group receipt index is below the retained base");
    if (index < base || index >= group_effect_receipt_count()) {
        throw std::out_of_range("native group receipt index is not retained");
    }
    const ReceiptRec key = receipt_store_->log.get(index);
    return GroupEffectReceipt{EventId{identity_, key.cause},
                              RequestHandle{identity_, key.recipient},
                              static_cast<GroupEffect>(key.effect), key.outcome};
}

// Bisect by cause, then walk its recipients in drain/queue order (K-OCA-KEEP).
// The keys are a resident tail over an external log; the bisection reads the
// log by position, and both handles of a key are the core's own run.
WorkingRequestCore::ReceiptLookup WorkingRequestCore::receipt_lookup(
        const EventId& cause, const RequestHandle& recipient, GroupEffect effect,
        uint64_t* outcome) const {
    assert(cause.ordinal > retired_through_ && "native group receipt lookup is below the window");
    const uint64_t base = group_effect_receipt_base();
    const uint64_t count = group_effect_receipt_count();
    if (base == count) return ReceiptLookup::Absent;
    const auto& log = receipt_store_->log;
    uint64_t low = base;
    uint64_t high = count;
    while (low < high) {
        const uint64_t middle = low + (high - low) / 2;
        if (log.get(middle).cause < cause.ordinal) low = middle + 1;
        else high = middle;
    }
    ReceiptRec found{};
    uint64_t index = low;
    while (index < count) {
        found = log.get(index);
        if (found.cause != cause.ordinal
            || (found.recipient == recipient.incarnation
                && static_cast<GroupEffect>(found.effect) == effect)) {
            break;
        }
        ++index;
    }
    if (index >= count) return ReceiptLookup::Absent;
    if (EventId{identity_, found.cause} != cause
        || RequestHandle{identity_, found.recipient} != recipient
        || static_cast<GroupEffect>(found.effect) != effect) {
        return ReceiptLookup::Absent;
    }
    // Outcomes were authenticated at commit; retained ones are checked again.
    // A retired cause never reaches this lookup or keeps a key.
    if (found.outcome > retired_through_) {
        const CommandEvent* event = event_at(EventId{identity_, found.outcome});
        if (!event || !authenticate_receipt_outcome(*event, cause, recipient, effect)) {
            return ReceiptLookup::Conflict;
        }
    }
    if (outcome) *outcome = found.outcome;
    return ReceiptLookup::Present;
}

std::optional<InstallError> WorkingRequestCore::validate_plan(const MutationPlan& plan) const noexcept {
    if (plan.consumed) return InstallError::AlreadyConsumed;
    const auto locked = plan.instance.lock();
    if (!locked || locked != instance_ || !instance_ || instance_->expired
        || plan.generation != instance_->generation || plan.run != identity_) {
        return InstallError::WrongCoreOrRun;
    }
    if (plan.epoch != epoch_ || plan.history_size != history_end()
        || plan.last_ordinal != last_ordinal_
        || (plan.add_receipt && plan.receipt_cause.ordinal <= retired_through_)
        || epoch_ == std::numeric_limits<uint64_t>::max()) {
        return InstallError::StalePreparation;
    }
    return std::nullopt;
}

bool WorkingRequestCore::trail_level_ok(double best, double offset, bool is_buy,
                                        double* stop, double ladder_tick) const noexcept {
    if (!valid_trail_offset(offset) || !std::isfinite(best)) return false;
    // Shared with the matcher: finite, zero riding the best, positive strictly
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
    const std::size_t first = history_end();
    const std::size_t count = plan.events.size();
    for (std::size_t i = 0; i < plan.events.size(); ++i) {
        auto& event = plan.events[i];
        const uint64_t ordinal = event_ordinal(event);
        append_event(std::move(event));
        if (plan.consume_incarnation) index_committed(journal_->resident_at(journal_->end - 1));
        last_ordinal_ = ordinal;
    }
    if (plan.live_change == kLivePush) {
        live_.push_back(std::move(plan.live_row));
    } else if (plan.live_change == kLiveErase) {
        erase_live(plan.live_index);
    } else if (plan.live_change == kLiveUpdate) {
        if (!std::holds_alternative<PendingDeferred>(plan.live_row.pending)) {
            note_chain_loss(live_[plan.live_index]);
        }
        live_[plan.live_index] = std::move(plan.live_row);
    } else if (plan.live_change == kLiveErasePush) {
        note_chain_loss(live_[plan.live_index]);
        const uint64_t pushed_incarnation = plan.live_row.handle().incarnation;
        const uint64_t pushed_priority = plan.live_row.priority();
        if (plan.live_index + 1 == live_.size()) {
            live_.back() = std::move(plan.live_row);
        } else {
            std::rotate(live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index),
                        live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index + 1),
                        live_.end());
            live_.back() = std::move(plan.live_row);
        }
        if (pushed_priority != pushed_incarnation) note_repriced(pushed_incarnation, pushed_priority);
    }
    if (plan.add_receipt) {
        receipt_store_->log.push(ReceiptRec{plan.receipt_cause.ordinal,
                                            plan.receipt_recipient.incarnation,
                                            plan.receipt_outcome,
                                            static_cast<uint64_t>(plan.receipt_effect)});
    }
    if (plan.chain_segment) {
        note_chain_segment(plan.chain_event_ordinal, plan.chain_offset, plan.chain_count);
    }
    if (plan.consume_incarnation) last_incarnation_ = plan.incarnation_used;
    // A kept binding took the ordinal its CloseBoundEvent would have; the
    // plan carries it with no event (prepare_evaluation).
    if (plan.skipped_ordinal != 0) last_ordinal_ = plan.skipped_ordinal;
    plan.consumed = true;
    bump_epoch();
    return Installed{EventRange{first, count}};
}

WorkingRequestCore::WorkingRequestCore(RunIdentity identity) {
    require_identity(identity);
    identity_ = std::move(identity);
    instance_ = std::make_shared<InstanceBinding>();
    journal_ = std::make_shared<JournalStore>();
}

WorkingRequestCore::WorkingRequestCore(WorkingRequestCore&& other) noexcept {
    *this = std::move(other);
}
WorkingRequestCore& WorkingRequestCore::operator=(WorkingRequestCore&& other) noexcept {
    if (this == &other) return *this;
    identity_ = std::move(other.identity_);
    live_ = std::move(other.live_);
    book_spill_ = std::move(other.book_spill_);
    journal_ = std::move(other.journal_);
    last_ordinal_ = other.last_ordinal_;
    last_incarnation_ = other.last_incarnation_;
    epoch_ = other.epoch_;
    history_base_ = std::exchange(other.history_base_, 0);
    retired_through_ = other.retired_through_;
    chain_floor_ = other.chain_floor_;
    chain_floor_exact_ = other.chain_floor_exact_;
    chain_segments_ = std::move(other.chain_segments_);
    issued_ = std::move(other.issued_);
    successor_roots_ = std::move(other.successor_roots_);
    repriced_ = std::move(other.repriced_);
    receipt_store_ = std::move(other.receipt_store_);
    next_cohort_handle_ = other.next_cohort_handle_;
    cohorts_ = std::move(other.cohorts_);
    cohort_receipts_ = std::move(other.cohort_receipts_);
    cohort_receipts_base_ = other.cohort_receipts_base_;
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
    other.last_ordinal_ = 0;
    other.last_incarnation_ = 0;
    other.epoch_ = 0;
    other.retired_through_ = 0;
    other.chain_floor_ = kNoChainFloor;
    other.chain_floor_exact_ = true;
    other.issued_.clear();
    other.successor_roots_.reset();
    other.repriced_.clear();
    other.next_cohort_handle_ = 1;
    other.cohorts_.clear();
    other.cohort_receipts_.clear();
    other.cohort_receipts_base_ = 0;
    other.instance_.reset();
    return *this;
}

void WorkingRequestCore::reset(RunIdentity identity) {
    require_identity(identity);
    if (journal_ && journal_->codec) {
        auto instance = std::make_shared<InstanceBinding>();
        journal_->check(detail::journal_codec::clear(*journal_->codec));
        auto journal = std::move(journal_);
        for (auto& value : journal->codec_values) value.reset();
        journal->first = journal->end = 0;
        journal->codec_next = 0;
        clear_unbound();
        identity_ = std::move(identity);
        instance_ = std::move(instance);
        journal_ = std::move(journal);
        prepare_codec_frames();
        return;
    }
    WorkingRequestCore next(std::move(identity));
    *this = std::move(next);
}

std::optional<RequestRejectReason> WorkingRequestCore::validate_request(
        const Request& request,
        const CommandContext& context,
        const std::optional<RequestHandle>& replace_target,
        std::shared_ptr<const OpeningReadback>& admitted_openings) const {
    require_grid(context.quantity_grid);
    const auto* reverse_to = as_reverse_to(request.intent);
    const auto* host_sized = as_host_sized(request.intent);
    const auto* sized = as_sized(request.intent);
    const auto* scope_fraction = deferred_reduction(request.intent);
    if (as_flatten(request.intent)) {
    } else if (const auto* reduce = as_reduce(request.intent)) {
        if (const auto* units = explicit_size(*reduce)) {
            if (!finite_positive(units->units)) return RequestRejectReason::InvalidQuantity;
            if (!on_optional_grid(units->units, context.quantity_grid)
                && !context.units_are_scope_boundary) {
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
        const auto count = opening_count(bind->openings, bind->external_openings.get());
        if (as_transact(request.intent) || bind->cycle <= 0 || !count
            || (bind->external_openings && !bind->openings.empty())) {
            return RequestRejectReason::InvalidOwner;
        }
        if (bind->external_openings) {
            if (!admit_external_openings(identity_, *bind, context, scratch_directory(),
                                         admitted_openings)) {
                return RequestRejectReason::InvalidOwner;
            }
        } else {
            // Keep vector admission and its refusal behavior unchanged.
            std::vector<RequestHandle> canonical = bind->openings;
            std::sort(canonical.begin(), canonical.end(), handle_less);
            for (const auto& member : canonical) {
                if (!member.incarnation || member.run != identity_)
                    return RequestRejectReason::InvalidOwner;
            }
            OpeningObservation first;
            if (context.external_openings) {
                if (context.external_openings->observation_at(0, first) != ReadStatus::ok)
                    return RequestRejectReason::InvalidOwner;
            } else {
                if (context.openings.empty()) return RequestRejectReason::InvalidOwner;
                first = context.openings.front();
            }
            const auto& position = first.current_position;
            const auto* nonflat = std::get_if<PositionNonflat>(&position);
            if (!nonflat || nonflat->cycle != bind->cycle) return RequestRejectReason::InvalidOwner;
            std::size_t live_count = 0;
            if (observe_openings(canonical, bind->cycle, nonflat->side, position,
                                 context.openings, &live_count, nullptr, context.external_openings)
                || live_count != count) return RequestRejectReason::InvalidOwner;
        }
    } else if (const auto* bind = std::get_if<BindCohort>(&request.owner)) {
        if (!host_sized || host_sized->kind != HostSizedKind::Close
            || !cohort_exists(bind->cohort)) {
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

namespace {
// Whether a request starts as an unbound book close (make_live's Independent
// branch): the only authority a carried binding can be given to. A
// keep_binding replace of any other request carries nothing, and takes the
// plain path.
bool starts_unbound_close(const Request& request) noexcept {
    if (!std::holds_alternative<Independent>(request.owner)) return false;
    if (as_transact(request.intent) || as_reverse_to(request.intent)) return false;
    if (const auto* sized = as_host_sized(request.intent)) return sized->kind != HostSizedKind::Open;
    return as_sized(request.intent) == nullptr;
}

// The binding a keep_binding replace carries (ReplaceOptions::keep_binding):
// the predecessor's book binding, or the one it still carries itself while it
// is unbound, when the successor starts unbound -- the only authority a book
// binding can later be given to. Nothing otherwise.
std::optional<PositionNonflat> carried_binding(const LiveRequest& predecessor,
                                               const LiveRequest& successor) noexcept {
    if (!std::holds_alternative<UnboundBookClose>(successor.authority)) return std::nullopt;
    if (const auto* close = std::get_if<BookClose>(&predecessor.authority)) {
        return PositionNonflat{close->cycle, close->side};
    }
    if (std::holds_alternative<UnboundBookClose>(predecessor.authority)) {
        return predecessor.definition->kept_binding;
    }
    return std::nullopt;
}

// Whether an unbound close whose definition carries a binding
// (ReplaceOptions::keep_binding) is evaluated against a book that still has
// that cycle and side. Such a close binds with no event: to the BookClose a
// CloseBoundEvent would install -- this cycle and side, this cursor, and the
// ordinal that event would take, which the bind takes and leaves unrecorded
// (kept_book_close) -- so the book, every later ordinal and a same-point
// continuation read exactly as after the event.
bool binds_kept(const LiveRequest& live, const PositionNonflat& book) noexcept {
    const auto& kept = live.definition->kept_binding;
    return kept && kept->cycle == book.cycle && kept->side == book.side;
}

BookClose kept_book_close(const PositionNonflat& book, const EvaluationContext& context,
                          const RunIdentity& run, uint64_t ordinal) {
    BookClose bound;
    bound.cycle = book.cycle;
    bound.side = book.side;
    bound.binding_event = EventId{run, ordinal};
    bound.binding_cursor = context.cursor;
    return bound;
}
}  // namespace

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
        OpeningObservation first;
        if (context.external_openings) {
            if (context.external_openings->observation_at(0, first) != ReadStatus::ok)
                throw detail::ScratchIoError(EIO, "native opening observation");
        } else {
            first = context.openings.front();
        }
        const auto& nonflat = std::get<PositionNonflat>(first.current_position);
        live.authority = OpeningsClose{bind->openings, bind->cycle, nonflat.side,
                                       EnrollmentFromCommand{accepted}, bind->external_openings};
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
    return eligibility_facts(live, context, false);
}

EligibilityFacts WorkingRequestCore::eligibility_facts(
        const LiveRequest& live, const EvaluationContext& context,
        bool open_activation_grant) const noexcept {
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
            && context.cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound)
        || open_grant_birth_ok(live, context.cursor, open_activation_grant);
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
    return evaluation_eligible(live, context, false);
}

bool WorkingRequestCore::evaluation_eligible(const LiveRequest& live,
                                             const EvaluationContext& context,
                                             bool open_activation_grant) const noexcept {
    const EligibilityFacts facts = eligibility_facts(live, context, open_activation_grant);
    return facts.ready_to_match;
}

std::vector<RequestHandle> WorkingRequestCore::waiting_children(const RequestHandle& parent) const {
    std::vector<RequestHandle> handles;
    waiting_children(parent, handles);
    return handles;
}

void WorkingRequestCore::waiting_children(const RequestHandle& parent,
                                          std::vector<RequestHandle>& handles) const {
    handles.clear();
    visit_waiting_children(parent, &handles, [](void* context, const RequestHandle& handle) {
        static_cast<std::vector<RequestHandle>*>(context)->push_back(handle);
    });
}

void WorkingRequestCore::visit_waiting_children(
        const RequestHandle& parent, void* context, HandleVisitor visitor) const {
    if (!visitor) throw std::invalid_argument("null native handle visitor");
    // The book is in queue order (live()), so the children come out oldest
    // first by priority -- incarnation order, until a re-price keeps a handle.
    // A spilled row is read back for its visit (visit_book).
    visit_book([&](const LiveRequest& live) {
        if (const auto* wait = std::get_if<Wait>(&live.authority)) {
            if (wait->parent == parent) visitor(context, live.handle());
        }
        return true;
    });
}

bool WorkingRequestCore::has_waiting_children(const RequestHandle& parent) const {
    bool found = false;
    visit_book([&](const LiveRequest& live) {
        if (const auto* wait = std::get_if<Wait>(&live.authority)) {
            if (wait->parent == parent) {
                found = true;
                return false;
            }
        }
        return true;
    });
    return found;
}

std::vector<RequestHandle> WorkingRequestCore::bound_close_handles() const {
    std::vector<RequestHandle> handles;
    bound_close_handles(handles);
    return handles;
}

void WorkingRequestCore::bound_close_handles(std::vector<RequestHandle>& handles) const {
    handles.clear();
    visit_bound_close_handles(&handles, [](void* context, const RequestHandle& handle) {
        static_cast<std::vector<RequestHandle>*>(context)->push_back(handle);
    });
}

void WorkingRequestCore::visit_bound_close_handles(void* context, HandleVisitor visitor) const {
    if (!visitor) throw std::invalid_argument("null native handle visitor");
    // In queue order, as the book holds them (waiting_children).
    visit_book([&](const LiveRequest& live) {
        if (std::holds_alternative<BookClose>(live.authority)
            || std::holds_alternative<OpeningClose>(live.authority)
            || std::holds_alternative<OpeningsClose>(live.authority)) {
            visitor(context, live.handle());
        }
        return true;
    });
}

std::vector<RequestHandle> WorkingRequestCore::group_recipients(const EventId& applied) const {
    std::vector<RequestHandle> handles;
    group_recipients(applied, handles);
    return handles;
}

void WorkingRequestCore::group_recipients(const EventId& applied,
                                          std::vector<RequestHandle>& handles) const {
    handles.clear();
    visit_group_recipients(applied, &handles, [](void* context, const RequestHandle& handle) {
        static_cast<std::vector<RequestHandle>*>(context)->push_back(handle);
    });
}

void WorkingRequestCore::visit_group_recipients(
        const EventId& applied, void* context, HandleVisitor visitor) const {
    if (!visitor) throw std::invalid_argument("null native handle visitor");
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload || !payload->definition) return;
    const auto* member = std::get_if<Member>(&payload->definition->request.group);
    if (!member) return;
    if (member->effect == GroupEffect::Cancel && !payload->terminal) return;
    if (member->effect == GroupEffect::Reduce && !(payload->filled_working > 0.0)) return;
    // B1-08: the visitor may evict the journal page/codec slot holding both
    // pointers. Keep only the fixed-size values used by the remaining walk.
    const Member group = *member;
    const RequestHandle source = payload->handle();
    const uint64_t ordinal = payload->ordinal;
    visit_book([&](const LiveRequest& live) {
        const auto* other = std::get_if<Member>(&live.request().group);
        if (!other || other->group != group.group || other->cohort == group.cohort) return true;
        if (live.handle() == source) return true;
        if (live.birth().acceptance_ordinal >= ordinal) return true;
        // In queue order, as the book holds them (waiting_children).
        visitor(context, live.handle());
        return true;
    });
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
    std::shared_ptr<const OpeningReadback> admitted_openings;
    auto reason = validate_request(staged, context, std::nullopt, admitted_openings);
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
    canonicalize_owner(staged, identity_, scratch_directory(), std::move(admitted_openings));
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
    std::shared_ptr<const OpeningReadback> admitted_openings;
    auto reason = validate_request(staged, context, staged_target, admitted_openings);
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
    canonicalize_owner(staged, identity_, scratch_directory(), std::move(admitted_openings));
    const TriggerState retained = live_[live_index].trigger_state;
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    // A replace that keeps the handle or carries a binding (R5 lane V19-D)
    // takes its own path; a plain one is the path below, unchanged -- as is
    // a keep_binding replace of a request that cannot carry one.
    if (options.keep_handle || (options.keep_binding && starts_unbound_close(staged))) {
        return prepare_replace_kept(live_index, staged_target, std::move(staged), ordinal,
                                    incarnation, context, options, std::move(plan));
    }
    RequestHandle successor{identity_, incarnation};
    Birth birth{ordinal, context.decision_time_ms};
    auto definition = std::make_shared<RequestDefinition>(
            RequestDefinition{successor, std::move(staged), birth, staged_target});
    // The successor inherits its predecessor's chain root, so a cohort names
    // the whole chain by its first handle without walking back through it.
    {
        const RequestDefinition& predecessor = *live_[live_index].definition;
        definition->root = predecessor.root ? *predecessor.root : predecessor.handle;
    }
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    if (options.retain_trigger_state) {
        live.trigger_state = retained;
        // The retained ride is the predecessor's; the successor has armed
        // nothing of its own, which is what its TrailArm ordinal says.
        if (auto* track = std::get_if<TrailTrack>(&live.trigger_state)) track->activation_ordinal = 0;
        if (auto* active = std::get_if<TrailActive>(&live.trigger_state)) active->activation_ordinal = 0;
    }
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

// prepare_replace for a replace that keeps the handle or carries a binding
// (ReplaceOptions::keep_handle / keep_binding, R5 lane V19-D), from the point
// the checks have passed: the successor definition, its live row and the
// ReplacedEvent. A re-price that keeps the handle keeps the request's
// lineage and takes the number a successor would have been issued as its
// priority; a successor is issued the number and roots its chain at its
// predecessor's root, so a cohort names the whole chain by its first handle.
// Kept out of line, like apply_replace_kept and kept_successor: a plain
// replace compiles to the code it had before the options existed.
[[gnu::noinline]] PreparedReplace WorkingRequestCore::prepare_replace_kept(
        std::size_t live_index, const RequestHandle& staged_target, Request&& staged,
        uint64_t ordinal, uint64_t incarnation, const CommandContext& context,
        ReplaceOptions options, MutationPlan&& plan) {
    auto impl = std::make_unique<PreparedReplace::Impl>();
    LiveRequest live = kept_successor(live_index, staged_target, std::move(staged), ordinal,
                                      incarnation, context, options);
    const RequestHandle successor = live.handle();
    ReplacedEvent replaced;
    replaced.ordinal = ordinal;
    replaced.predecessor_definition = live_[live_index].definition;
    replaced.successor_definition = live.definition;
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

// The successor row of a replace that keeps the handle or carries a binding,
// shared by prepare_replace_kept and apply_replace_kept.
[[gnu::noinline]] LiveRequest WorkingRequestCore::kept_successor(
        std::size_t live_index, const RequestHandle& staged_target, Request&& staged,
        uint64_t ordinal, uint64_t incarnation, const CommandContext& context,
        ReplaceOptions options) const {
    const RequestDefinition& predecessor = *live_[live_index].definition;
    Birth birth{ordinal, context.decision_time_ms};
    std::shared_ptr<RequestDefinition> definition;
    if (options.keep_handle) {
        definition = std::make_shared<RequestDefinition>(RequestDefinition{
                predecessor.handle, std::move(staged), birth, predecessor.predecessor});
        definition->root = predecessor.root;
        definition->priority = incarnation;
    } else {
        definition = std::make_shared<RequestDefinition>(RequestDefinition{
                RequestHandle{identity_, incarnation}, std::move(staged), birth, staged_target});
        definition->root = predecessor.root ? *predecessor.root : predecessor.handle;
    }
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    if (options.retain_trigger_state) {
        live.trigger_state = live_[live_index].trigger_state;
        // The retained ride is the predecessor's; a successor has armed
        // nothing of its own, which is what its TrailArm ordinal says. A
        // re-price that kept the handle is the request that armed.
        if (!options.keep_handle) {
            if (auto* track = std::get_if<TrailTrack>(&live.trigger_state)) track->activation_ordinal = 0;
            if (auto* active = std::get_if<TrailActive>(&live.trigger_state)) active->activation_ordinal = 0;
        }
    }
    if (options.keep_binding) definition->kept_binding = carried_binding(live_[live_index], live);
    return live;
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

// The direct forms of the three commands: prepare_submit, prepare_replace and
// prepare_cancel with their installs, check for check.
CommandInstalled<SubmitResult> WorkingRequestCore::apply_submit(
        const Request& request, const CommandContext& context,
        uint64_t& next_order_incarnation, uint64_t& next_timeline_ordinal,
        RequestOrigin origin) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    Request staged = request;
    begin_direct();
    std::shared_ptr<const OpeningReadback> admitted_openings;
    auto reason = validate_request(staged, context, std::nullopt, admitted_openings);
    if (!reason) reason = resolve_tick_spellings(staged, context);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    if (reason) {
        RejectedEvent rejected{ordinal, std::move(staged), *reason, context.surface};
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        append_direct(std::move(rejected), false);
        return CommandInstalled<SubmitResult>{
                SubmitResult{SubmitStatus::Rejected, ordinal, std::nullopt, reason},
                finish_direct(first)};
    }
    canonicalize_owner(staged, identity_, scratch_directory(), std::move(admitted_openings));
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle handle{identity_, incarnation};
    Birth birth{ordinal, context.decision_time_ms};
    auto definition = std::make_shared<RequestDefinition>(
            RequestDefinition{handle, std::move(staged), birth, std::nullopt, origin});
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    AcceptedEvent accepted;
    accepted.ordinal = ordinal;
    accepted.definition = std::move(definition);
    accepted.surface = context.surface;
    seal_direct(1, true, false, true);
    const std::size_t first = history_end();
    append_direct(std::move(accepted), true);
    live_.push_back(std::move(live));
    last_incarnation_ = incarnation;
    return CommandInstalled<SubmitResult>{
            SubmitResult{SubmitStatus::Accepted, ordinal, std::move(handle), std::nullopt},
            finish_direct(first)};
}

template <bool Kept>
CommandInstalled<ReplaceResult> WorkingRequestCore::apply_replace_as(
        const RequestHandle& target, const Request& request, const CommandContext& context,
        uint64_t& next_order_incarnation, uint64_t& next_timeline_ordinal,
        ReplaceOptions options) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    RequestHandle staged_target = target;
    Request staged = request;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    begin_direct();
    if (kind != TargetKind::Live) {
        const ReplaceStatus status = kind == TargetKind::InvalidHandle
                ? ReplaceStatus::InvalidHandle
                : ReplaceStatus::NotWorking;
        // Both events are built by moves alone, so the seal may come first.
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        if (kind == TargetKind::InvalidHandle) {
            append_direct(InvalidHandleEvent{ordinal, std::move(staged_target), std::move(staged),
                                             context.surface},
                          false);
        } else {
            append_direct(NotWorkingEvent{ordinal, std::move(staged_target), std::move(staged),
                                          context.surface},
                          false);
        }
        return CommandInstalled<ReplaceResult>{
                ReplaceResult{status, ordinal, std::nullopt, std::nullopt}, finish_direct(first)};
    }
    std::shared_ptr<const OpeningReadback> admitted_openings;
    auto reason = validate_request(staged, context, staged_target, admitted_openings);
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
            if (best && (!trail || !trail_level_ok(*best, trail->offset, true, nullptr)
                         || !trail_level_ok(*best, trail->offset, false, nullptr))) {
                reason = RequestRejectReason::InvalidTrigger;
            }
        }
    }
    if (reason) {
        ReplaceRejectedEvent rejected;
        rejected.ordinal = ordinal;
        rejected.live_definition = live_[live_index].definition;
        rejected.attempted = std::move(staged);
        rejected.reason = *reason;
        rejected.surface = context.surface;
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        append_direct(std::move(rejected), false);
        return CommandInstalled<ReplaceResult>{
                ReplaceResult{ReplaceStatus::ReplaceRejected, ordinal, std::nullopt, reason},
                finish_direct(first)};
    }
    canonicalize_owner(staged, identity_, scratch_directory(), std::move(admitted_openings));
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    if constexpr (Kept) {
        // A replace that keeps the handle or carries a binding (R5 lane
        // V19-D): the successor row kept_successor makes, and a re-price that
        // keeps the handle takes a number no handle is issued under.
        LiveRequest live = kept_successor(live_index, staged_target, std::move(staged), ordinal,
                                          incarnation, context, options);
        RequestHandle successor = live.handle();
        ReplacedEvent replaced;
        replaced.ordinal = ordinal;
        replaced.predecessor_definition = live_[live_index].definition;
        replaced.successor_definition = live.definition;
        replaced.surface = context.surface;
        if (options.keep_handle) reserve_n(repriced_, 1);
        seal_direct(1, false, false, !options.keep_handle);
        const std::size_t first = history_end();
        append_direct(std::move(replaced), !options.keep_handle);
        // commit's erase-push, as below: a re-price takes a fresh number of
        // the request sequence too, so its row goes to the back.
        note_chain_loss(live_[live_index]);
        for (std::size_t index = live_index; index + 1 < live_.size(); ++index) {
            live_[index] = std::move(live_[index + 1]);
        }
        live_.back() = std::move(live);
        last_incarnation_ = incarnation;
        if (options.keep_handle) note_repriced(successor.incarnation, incarnation);
        return CommandInstalled<ReplaceResult>{
                ReplaceResult{ReplaceStatus::Replaced, ordinal, std::move(successor), std::nullopt},
                finish_direct(first)};
    } else {
        RequestHandle successor{identity_, incarnation};
        Birth birth{ordinal, context.decision_time_ms};
        auto definition = std::make_shared<RequestDefinition>(
                RequestDefinition{successor, std::move(staged), birth, staged_target});
        // The successor inherits its predecessor's chain root, so a cohort names
        // the whole chain by its first handle without walking back through it.
        {
            const RequestDefinition& predecessor = *live_[live_index].definition;
            definition->root = predecessor.root ? *predecessor.root : predecessor.handle;
        }
        LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
        if (options.retain_trigger_state) {
            live.trigger_state = live_[live_index].trigger_state;
            // The retained ride is the predecessor's; the successor has armed
            // nothing of its own, which is what its TrailArm ordinal says.
            if (auto* track = std::get_if<TrailTrack>(&live.trigger_state)) track->activation_ordinal = 0;
            if (auto* active = std::get_if<TrailActive>(&live.trigger_state)) active->activation_ordinal = 0;
        }
        ReplacedEvent replaced;
        replaced.ordinal = ordinal;
        replaced.predecessor_definition = live_[live_index].definition;
        replaced.successor_definition = std::move(definition);
        replaced.surface = context.surface;
        seal_direct(1, false, false, true);
        const std::size_t first = history_end();
        append_direct(std::move(replaced), true);
        // commit's erase-push: the predecessor's row leaves, the rows after it
        // move down one place, and the successor's is born at the back (a fresh
        // incarnation keeps the book in order). The same book commit's rotation
        // leaves, by one move per later row instead of a swap.
        note_chain_loss(live_[live_index]);
        for (std::size_t index = live_index; index + 1 < live_.size(); ++index) {
            live_[index] = std::move(live_[index + 1]);
        }
        live_.back() = std::move(live);
        last_incarnation_ = incarnation;
        return CommandInstalled<ReplaceResult>{
                ReplaceResult{ReplaceStatus::Replaced, ordinal, std::move(successor), std::nullopt},
                finish_direct(first)};
    }
}

// A plain replace compiles to apply_replace_as<false>, the code it had before
// the options existed; a replace that keeps the handle or carries a binding
// (ReplaceOptions::keep_handle / keep_binding, R5 lane V19-D) to
// apply_replace_as<true>, kept out of line so it perturbs neither.
CommandInstalled<ReplaceResult> WorkingRequestCore::apply_replace(
        const RequestHandle& target, const Request& request, const CommandContext& context,
        uint64_t& next_order_incarnation, uint64_t& next_timeline_ordinal,
        ReplaceOptions options) {
    if (options.keep_handle || (options.keep_binding && starts_unbound_close(request))) {
        return apply_replace_kept(target, request, context, next_order_incarnation,
                                  next_timeline_ordinal, options);
    }
    return apply_replace_as<false>(target, request, context, next_order_incarnation,
                                   next_timeline_ordinal, options);
}

[[gnu::noinline]] CommandInstalled<ReplaceResult> WorkingRequestCore::apply_replace_kept(
        const RequestHandle& target, const Request& request, const CommandContext& context,
        uint64_t& next_order_incarnation, uint64_t& next_timeline_ordinal,
        ReplaceOptions options) {
    return apply_replace_as<true>(target, request, context, next_order_incarnation,
                                  next_timeline_ordinal, options);
}

CommandInstalled<CancelResult> WorkingRequestCore::apply_cancel(
        const RequestHandle& target, uint64_t& next_timeline_ordinal, CancelReason reason) {
    require_identity(identity_);
    RequestHandle staged_target = target;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    begin_direct();
    if (kind != TargetKind::Live) {
        const CancelStatus status = kind == TargetKind::InvalidHandle
                ? CancelStatus::InvalidHandle
                : CancelStatus::NotWorking;
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        if (kind == TargetKind::InvalidHandle) {
            append_direct(InvalidHandleEvent{ordinal, std::move(staged_target), std::nullopt},
                          false);
        } else {
            append_direct(NotWorkingEvent{ordinal, std::move(staged_target), std::nullopt}, false);
        }
        return CommandInstalled<CancelResult>{CancelResult{status, ordinal}, finish_direct(first)};
    }
    CancelledEvent cancelled =
            make_cancelled(ordinal, live_[live_index], reason, EventId{identity_, ordinal});
    return CommandInstalled<CancelResult>{
            CancelResult{CancelStatus::Cancelled, ordinal},
            end_direct(live_index, std::move(cancelled)).events};
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
                                                 const PositionIdentity& position) {
    // Only a book close on the nonflat position's cycle and side is refreshed;
    // against a flat position no row changes.
    const auto* nonflat = std::get_if<PositionNonflat>(&position);
    if (!nonflat) return;
    const auto refresh = [&](LiveRequest& live) {
        if (same_point_allowance(live.allowance, point)) return false;
        if (const auto* close = std::get_if<BookClose>(&live.authority)) {
            if (nonflat->cycle == close->cycle && nonflat->side == close->side) {
                live.allowance = evaluated_allowance(live, point);
                return true;
            }
        }
        return false;
    };
    for (auto& live : live_) refresh(live);
    if (spilled_count() == 0) return;
    // The rows outside the heap get the same refresh: read back, refreshed,
    // and written again where it changed them. Rewriting an entry moves
    // neither its place in the table nor any other entry, so the walk goes on.
    // A refresh that stops half way (an unreadable file) leaves the rest
    // behind the point; the next call picks them up, a row already at the
    // point being skipped.
    BookSpill& spill = *book_spill_;
    spill.check();
    for (uint64_t at = spill.first_alive(); at != 0; at = spill.next_alive(at)) {
        const BookSlot slot = spill.rows.get(at);
        LiveRequest row = spill.read(*this, slot);
        if (refresh(row)) spill.rewrite(at, slot, row);
    }
    // A refresh every point may append new bytes for a row whose allowance
    // changed size; no table offset is held now, so the file can be rebuilt.
    if (spill.worth_compacting()) spill.compact();
}

bool WorkingRequestCore::refresh_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation) {
    return refresh_allowance(target, context, observation, nullptr, false);
}

bool WorkingRequestCore::refresh_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation, bool open_activation_grant) {
    return refresh_allowance(target, context, observation, nullptr, open_activation_grant);
}

bool WorkingRequestCore::refresh_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation, uint64_t& next_timeline_ordinal) {
    return refresh_allowance(target, context, observation, &next_timeline_ordinal, false);
}

bool WorkingRequestCore::refresh_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation, uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
    return refresh_allowance(target, context, observation, &next_timeline_ordinal,
                             open_activation_grant);
}

bool WorkingRequestCore::refresh_allowance(
        const RequestHandle& target, const EvaluationContext& context,
        const TargetObservation& observation, uint64_t* next_timeline_ordinal,
        bool open_activation_grant) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) return false;
    LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)) return false;
    const EligibilityFacts facts = eligibility_facts(live, context, open_activation_grant);
    if (!facts.birth_ok || !facts.driver_ok) return false;
    if (std::holds_alternative<CohortClose>(live.authority)) {
        if (!context.cohort_side) return false;
        bool has_live_member = false;
        for (const auto& opening : ObservationRows{observation})
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
        // An unbound close binds here only when it carried a binding the
        // book still has (ReplaceOptions::keep_binding) and the caller hands
        // the timeline its binding takes: with no event, exactly as
        // prepare_evaluation binds it. Every other unbound close is left to
        // the evaluation, which binds it with a CloseBoundEvent or ends it.
        if (!next_timeline_ordinal || !live.definition->kept_binding) return false;
        return refresh_kept_binding(live, context, observation, *next_timeline_ordinal);
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

// refresh_allowance's bind for an unbound close that carried a binding the
// book still has: the same steps as its allowance refresh, the kept BookClose
// installed, and the ordinal its CloseBoundEvent would take taken, unrecorded.
bool WorkingRequestCore::refresh_kept_binding(LiveRequest& live, const EvaluationContext& context,
                                              const TargetObservation& observation,
                                              uint64_t& next_timeline_ordinal) {
    const auto* book = std::get_if<PositionNonflat>(&observation.current_position);
    if (!book || book->cycle <= 0 || !binds_kept(live, *book)) return false;
    if (same_point_allowance(live.allowance, context.cursor.point.ordinal)) {
        return false;
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    if (epoch_ > std::numeric_limits<uint64_t>::max() - 2U) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
    live.allowance = evaluated_allowance(live, context.cursor.point.ordinal);
    live.authority = kept_book_close(*book, context, identity_, ordinal);
    last_ordinal_ = ordinal;
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

    double computed_deduction = std::min(pending_total, resolved_units);
    double computed_after = resolved_units;
    bool computed_exhausted = computed_deduction == resolved_units;
    if (resolved_units == 0.0 || computed_exhausted) {
        computed_after = 0.0;
    } else if (absorbed_deduction(resolved_units, computed_deduction)) {
        // Absorbed (K-ULP5): the resolved units stand and nothing is deducted.
        computed_deduction = 0.0;
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
    return prepare_evaluation(target, context, observation, next_timeline_ordinal, false);
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_evaluation(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)) {
        return NoChange{NoChangeReason::StillWaiting};
    }
    const EligibilityFacts facts = eligibility_facts(live, context, open_activation_grant);
    if (!facts.birth_ok || !facts.driver_ok) return NoChange{NoChangeReason::NotEligible};

    if (std::holds_alternative<CohortClose>(live.authority)) {
        bool has_live_member = false;
        for (const auto& opening : ObservationRows{observation}) {
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
        if (binds_kept(live, *nonflat)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            LiveRequest updated = live;
            updated.authority = kept_book_close(*nonflat, context, identity_, ordinal);
            updated.allowance = evaluated_allowance(live, context.cursor.point.ordinal);
            plan.live_change = kLiveUpdate;
            plan.live_index = live_index;
            plan.live_row = std::move(updated);
            plan.skipped_ordinal = ordinal;
            return finish_mutation(std::move(plan));
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

Preparation<Installed> WorkingRequestCore::apply_evaluation(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal) {
    return apply_evaluation(target, context, observation, next_timeline_ordinal, false);
}

Preparation<Installed> WorkingRequestCore::apply_evaluation(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)) {
        return NoChange{NoChangeReason::StillWaiting};
    }
    const EligibilityFacts facts = eligibility_facts(live, context, open_activation_grant);
    if (!facts.birth_ok || !facts.driver_ok) return NoChange{NoChangeReason::NotEligible};

    if (std::holds_alternative<CohortClose>(live.authority)) {
        bool has_live_member = false;
        for (const auto& opening : ObservationRows{observation}) {
            has_live_member = has_live_member || opening.has_live_matching_lot;
        }
        if (!context.cohort_side || !has_live_member) {
            return NoChange{NoChangeReason::StillWaiting};
        }
    }

    if (std::holds_alternative<UnboundBookClose>(live.authority)) {
        if (std::holds_alternative<PositionFlat>(observation.current_position)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            begin_direct();
            NoEffectEvent none;
            none.ordinal = ordinal;
            none.definition = live.definition;
            none.remaining = project_remaining(live.remaining);
            none.authority = live.authority;
            none.cursor = context.cursor;
            return end_direct(live_index, std::move(none));
        }
        const auto* nonflat = std::get_if<PositionNonflat>(&observation.current_position);
        if (!nonflat || nonflat->cycle <= 0) {
            return PreparationError{CoreFailure::MissingObservation, EventId{identity_, 0}, target};
        }
        // A carried binding the book still has binds to the BookClose the
        // CloseBoundEvent would install and takes its ordinal, unrecorded
        // (binds_kept); any other binds with the event.
        const bool kept = binds_kept(live, *nonflat);
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        begin_direct();
        BookClose bound = kept_book_close(*nonflat, context, identity_, ordinal);
        CloseBoundEvent bound_event;
        if (!kept) {
            bound_event.ordinal = ordinal;
            bound_event.definition = live.definition;
            bound_event.cycle = bound.cycle;
            bound_event.side = bound.side;
            bound_event.cursor = context.cursor;
            bound_event.before = live.authority;
            bound_event.after = bound;
        }
        const Allowance allowance = evaluated_allowance(live, context.cursor.point.ordinal);
        seal_direct(kept ? 0 : 1, false, false, false);
        const std::size_t first = history_end();
        if (!kept) append_direct(std::move(bound_event), false);
        else last_ordinal_ = ordinal;
        live.authority = std::move(bound);
        live.allowance = allowance;
        return Installed{finish_direct(first)};
    }

    const bool book_gone = std::holds_alternative<BookClose>(live.authority)
        && !book_close_alive(observation, std::get<BookClose>(live.authority));
    const bool opening_gone = std::holds_alternative<OpeningClose>(live.authority)
        && !opening_close_alive(observation, std::get<OpeningClose>(live.authority));
    if (book_gone || opening_gone) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        begin_direct();
        return end_direct(live_index, make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                     EventId{identity_, ordinal}));
    }

    if (const auto* close = std::get_if<OpeningsClose>(&live.authority)) {
        std::size_t live_count = 0;
        if (const auto error = observe_openings(observation, *close, &live_count)) {
            return PreparationError{*error, EventId{identity_, 0}, target};
        }
        if (live_count == 0) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            begin_direct();
            return end_direct(live_index, make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                         EventId{identity_, ordinal}));
        }
    }

    if (same_point_allowance(live.allowance, context.cursor.point.ordinal)) {
        return NoChange{NoChangeReason::NoTransition};
    }
    const Allowance allowance = evaluated_allowance(live, context.cursor.point.ordinal);
    begin_direct();
    seal_direct(0, false, false, false);
    const std::size_t first = history_end();
    live.allowance = allowance;
    return Installed{finish_direct(first)};
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_trigger(
        const RequestHandle& target,
        const TriggerTransition& transition,
        DriverEligibilityClass driver_class,
        uint64_t& next_timeline_ordinal,
        std::optional<Side> cohort_side,
        const ActivationGrid& activation_grid) {
    return prepare_trigger(target, transition, driver_class, next_timeline_ordinal,
                           cohort_side, activation_grid, false);
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_trigger(
        const RequestHandle& target,
        const TriggerTransition& transition,
        DriverEligibilityClass driver_class,
        uint64_t& next_timeline_ordinal,
        std::optional<Side> cohort_side,
        const ActivationGrid& activation_grid,
        bool open_activation_grant) {
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
    if (!trigger_cursor_eligible(updated, cursor, open_activation_grant)
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
        // A trail's arm is the event its tracking state names from then on
        // (NativeTrailState::activation_ordinal), so the state outlives the
        // journal window that holds the event.
        if (kind == ActivationKind::TrailArm) {
            if (auto* track = std::get_if<TrailTrack>(&after)) track->activation_ordinal = ordinal;
        }
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
    return emit_activated(ActivationKind::TrailTrigger,
                          TrailActive{track->best, track->activation_ordinal}, trail_hit.cursor,
                          native_matching::grid_reached_print(
                              trail_hit.reached_price, level, /*le=*/!is_buy, grid));
}

Preparation<Installed> WorkingRequestCore::apply_trigger(
        const RequestHandle& target,
        const TriggerTransition& transition,
        DriverEligibilityClass driver_class,
        uint64_t& next_timeline_ordinal,
        std::optional<Side> cohort_side,
        const ActivationGrid& activation_grid) {
    return apply_trigger(target, transition, driver_class, next_timeline_ordinal,
                         cohort_side, activation_grid, false);
}

Preparation<Installed> WorkingRequestCore::apply_trigger(
        const RequestHandle& target,
        const TriggerTransition& transition,
        DriverEligibilityClass driver_class,
        uint64_t& next_timeline_ordinal,
        std::optional<Side> cohort_side,
        const ActivationGrid& activation_grid,
        bool open_activation_grant) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)
        || std::holds_alternative<UnboundBookClose>(live.authority)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const MatchCursor& cursor = transition_cursor(transition);
    if (!trigger_cursor_eligible(live, cursor, open_activation_grant)
        && !same_point_allowance(live.allowance, cursor.point.ordinal)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    if (!driver_class_matches_cursor(driver_class, cursor)
        || !trigger_permits_driver(live.request().trigger, live.trigger_state,
                                    driver_class, false)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const bool is_buy = working_is_buy(live, cohort_side);
    const native_matching::GridThreshold grid = matcher_grid(activation_grid);
    const double ladder_tick = activation_grid.ladder_tick;
    begin_direct();

    auto emit_activated = [&](ActivationKind kind, TriggerState after, const MatchCursor& at,
                              double price) -> Preparation<Installed> {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        if (kind == ActivationKind::TrailArm) {
            if (auto* track = std::get_if<TrailTrack>(&after)) track->activation_ordinal = ordinal;
        }
        ActivatedEvent activated;
        activated.ordinal = ordinal;
        activated.definition = live.definition;
        activated.kind = kind;
        activated.before = live.trigger_state;
        activated.after = after;
        activated.reached_price = price;
        activated.cursor = at;
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        append_direct(std::move(activated), false);
        live.trigger_state = after;
        return Installed{finish_direct(first)};
    };

    if (const auto* begin = std::get_if<BeginTrailTracking>(&transition)) {
        if (!std::holds_alternative<TrailWaitArm>(live.trigger_state)) {
            if (std::holds_alternative<TrailTrack>(live.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        if (!std::isfinite(begin->reached_price)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        const auto* trail = std::get_if<Trail>(&live.request().trigger);
        if (!trail || !trail_level_ok(begin->reached_price, trail->offset, is_buy, nullptr,
                                      ladder_tick)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        if (trail->arm_price
            && !stop_price_reached(!is_buy, *trail->arm_price, begin->reached_price, grid)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const double arm_print = trail->arm_price
            ? native_matching::grid_reached_print(begin->reached_price, *trail->arm_price,
                                                  /*le=*/is_buy, grid)
            : native_matching::grid_best_print(begin->reached_price, is_buy, grid);
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
        auto* track = std::get_if<TrailTrack>(&live.trigger_state);
        if (!track) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const double print = native_matching::grid_best_print(extremum->reached_price, is_buy, grid);
        const double next_best = is_buy ? std::min(track->best, print)
                                        : std::max(track->best, print);
        if (next_best == track->best) return NoChange{NoChangeReason::NoTransition};
        const auto* trail = std::get_if<Trail>(&live.request().trigger);
        if (!trail || !trail_level_ok(next_best, trail->offset, is_buy, nullptr,
                                      ladder_tick)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        seal_direct(0, false, false, false);
        const std::size_t first = history_end();
        track->best = next_best;
        return Installed{finish_direct(first)};
    }
    if (const auto* stop = std::get_if<ActivateStop>(&transition)) {
        if (!std::holds_alternative<StopIdle>(live.trigger_state)) {
            if (std::holds_alternative<StopActive>(live.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const auto* stop_px = std::get_if<Stop>(&live.request().trigger);
        if (!stop_px || !stop_price_reached(is_buy, stop_px->price, stop->reached_price, grid)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::Stop, StopActive{}, stop->cursor,
                              native_matching::grid_reached_print(
                                  stop->reached_price, stop_px->price, /*le=*/!is_buy, grid));
    }
    if (const auto* stop_limit = std::get_if<ActivateStopLimit>(&transition)) {
        if (!std::holds_alternative<StopLimitPending>(live.trigger_state)) {
            if (std::holds_alternative<StopLimitLive>(live.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const auto* stop_limit_px = std::get_if<StopLimit>(&live.request().trigger);
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
    const auto* track = std::get_if<TrailTrack>(&live.trigger_state);
    if (!track) {
        if (std::holds_alternative<TrailActive>(live.trigger_state)) {
            return NoChange{NoChangeReason::NoTransition};
        }
        return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0}, target};
    }
    const auto* trail = std::get_if<Trail>(&live.request().trigger);
    double level = 0.0;
    if (!trail || !trail_level_ok(track->best, trail->offset, is_buy, &level, ladder_tick)
        || !stop_price_reached(is_buy, level, trail_hit.reached_price, grid)) {
        return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0}, target};
    }
    return emit_activated(ActivationKind::TrailTrigger,
                          TrailActive{track->best, track->activation_ordinal}, trail_hit.cursor,
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

Preparation<Installed> WorkingRequestCore::apply_no_effect(
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
    begin_direct();
    NoEffectEvent none;
    none.ordinal = ordinal;
    none.definition = live.definition;
    none.remaining = project_remaining(live.remaining);
    none.authority = live.authority;
    none.cursor = context.cursor;
    return end_direct(live_index, std::move(none));
}

Preparation<Installed> WorkingRequestCore::apply_match_rejected(
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
    begin_direct();
    MatchRejectedEvent rejected;
    rejected.ordinal = ordinal;
    rejected.reason = reason;
    rejected.definition = live.definition;
    rejected.remaining = project_remaining(live.remaining);
    rejected.authority = live.authority;
    rejected.cursor = context.cursor;
    rejected.attempted_terms = std::move(attempted_terms);
    return end_direct(live_index, std::move(rejected));
}

Preparation<Installed> WorkingRequestCore::apply_margin_call(
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
    begin_direct();
    MarginCallEvent receipt = event;
    receipt.ordinal = ordinal;
    seal_direct(1, false, false, false);
    const std::size_t first = history_end();
    append_direct(std::move(receipt), false);
    return Installed{finish_direct(first)};
}

Preparation<Installed> WorkingRequestCore::apply_risk_event(
        const NativeRiskEvent& event, uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    begin_direct();
    NativeRiskEvent receipt = event;
    receipt.ordinal = ordinal;
    seal_direct(1, false, false, false);
    const std::size_t first = history_end();
    append_direct(std::move(receipt), false);
    return Installed{finish_direct(first)};
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

    ChainIds chain;
    double pending_total = 0.0;
    if (!collect_pending_chain(live.pending, target, &chain, &pending_total)) {
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
    receipt.prior_adjustment_ids = std::move(chain.inline_ids);
    receipt.pending_total = pending_total;
    receipt.effective_deduction = deduction;
    receipt.remaining_before = project_remaining(live.remaining);
    receipt.remaining_after = project_remaining(updated.remaining);
    receipt.allowance_after = updated.allowance;

    MutationPlan plan = begin_plan();
    plan.events.emplace_back(std::move(receipt));
    if (chain.external) {
        plan.chain_segment = true;
        plan.chain_event_ordinal = ordinal;
        plan.chain_offset = chain.offset;
        plan.chain_count = chain.count;
    }
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

Preparation<Installed> WorkingRequestCore::apply_terms(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TermsResolvedInput& input,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest& live = live_[live_index];
    const bool deferred = std::holds_alternative<RemainingDeferred>(live.remaining)
        || std::holds_alternative<NoTarget>(live.remaining);
    const bool has_units = input.terms.units.has_value();

    if (!deferred) {
        if (has_units || input.terms.shape != OpeningShape::Transact) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        begin_direct();
        TermsResolvedEvent receipt;
        receipt.ordinal = ordinal;
        receipt.definition = live.definition;
        receipt.cursor = context.cursor;
        receipt.input = input;
        receipt.remaining_before = project_remaining(live.remaining);
        receipt.remaining_after = project_remaining(live.remaining);
        receipt.allowance_after = live.allowance;
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        append_direct(std::move(receipt), false);
        return Installed{finish_direct(first)};
    }

    if (!has_units || !std::isfinite(*input.terms.units) || *input.terms.units < 0.0) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    const auto* sized = as_host_sized(live.request().intent);
    const auto* native_sized = as_sized(live.request().intent);
    const auto* scope_fraction = deferred_reduction(live.request().intent);
    const bool opening_shapes = (sized && sized->kind == HostSizedKind::Open)
        || native_sized != nullptr;
    if ((!sized && !native_sized && !scope_fraction)
        || (input.terms.shape != OpeningShape::Transact
            && input.terms.shape != OpeningShape::ReverseTo
            && input.terms.shape != OpeningShape::CloseOpposite)
        || (!opening_shapes && input.terms.shape != OpeningShape::Transact)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    ChainIds chain;
    double pending_total = 0.0;
    if (!collect_pending_chain(live.pending, target, &chain, &pending_total)) {
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
    TermsResolvedEvent receipt;
    receipt.ordinal = ordinal;
    receipt.definition = live.definition;
    receipt.cursor = context.cursor;
    receipt.input = input;
    receipt.prior_adjustment_ids = std::move(chain.inline_ids);
    receipt.pending_total = pending_total;
    receipt.effective_deduction = deduction;
    receipt.remaining_before = project_remaining(live.remaining);
    receipt.remaining_after = project_remaining(Remaining{RemainingUnits{after}});
    receipt.allowance_after = bound_allowance;
    begin_direct();

    if (*input.terms.units == 0.0 || (after == 0.0 && deduction > 0.0)) {
        const uint64_t terminal_ordinal = ordinal + 1;
        if (terminal_ordinal == 0 || terminal_ordinal == std::numeric_limits<uint64_t>::max()
            || terminal_ordinal <= last_ordinal_) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
        // The terminal reads the row as the receipt left it: bound to `after`
        // units, the pending chain spent. Built from that copy, as the plan does.
        LiveRequest updated = live;
        updated.remaining = RemainingUnits{after};
        updated.allowance = bound_allowance;
        updated.pending = PendingNone{};
        std::optional<NoEffectEvent> none;
        std::optional<CancelledEvent> cancelled;
        if (*input.terms.units == 0.0) {
            none.emplace();
            none->ordinal = terminal_ordinal;
            none->definition = live.definition;
            none->remaining = project_remaining(updated.remaining);
            none->authority = updated.authority;
            none->cursor = context.cursor;
        } else {
            cancelled.emplace(make_cancelled(terminal_ordinal, updated, CancelReason::Group,
                                             EventId{identity_, ordinal}));
        }
        seal_direct(2, false, false, false, chain.external);
        const std::size_t first = history_end();
        append_direct(std::move(receipt), false);
        if (chain.external) note_chain_segment(ordinal, chain.offset, chain.count);
        if (none) {
            append_direct(std::move(*none), false);
        } else {
            append_direct(std::move(*cancelled), false);
        }
        erase_live(live_index);
        return Installed{finish_direct(first)};
    }

    (void) exhausted;
    seal_direct(1, false, false, false, chain.external);
    const std::size_t first = history_end();
    append_direct(std::move(receipt), false);
    if (chain.external) note_chain_segment(ordinal, chain.offset, chain.count);
    live.remaining = RemainingUnits{after};
    live.allowance = bound_allowance;
    note_chain_loss(live);
    live.pending = PendingNone{};
    return Installed{finish_direct(first)};
}

Preparation<PreparedExecution> WorkingRequestCore::prepare_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal) {
    return prepare_execution(target, proposal, next_timeline_ordinal, false);
}

Preparation<PreparedExecution> WorkingRequestCore::prepare_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
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
            && proposal.cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound)
        || open_grant_birth_ok(live, proposal.cursor, open_activation_grant);
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
        for (const auto& observation : ObservationRows{proposal.pre_target}) {
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
        for (const auto& observation : ObservationRows{proposal.pre_target}) {
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
        if (!transact_filled_units(plan_transact->signed_units,
                                   proposal.inspected_closed_units,
                                   proposal.inspected_opened_units, &filled)) {
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

// prepare_execution's checks, as values (R5 lane L3): the same tests in the
// same order, answering what the token's events and retained row are made of.
Preparation<WorkingRequestCore::ExecutionValues> WorkingRequestCore::execution_values(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
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
            && proposal.cursor.point.effective_time_ms >= live.birth().decision_time_lower_bound)
        || open_grant_birth_ok(live, proposal.cursor, open_activation_grant);
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
        for (const auto& observation : ObservationRows{proposal.pre_target}) {
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
        for (const auto& observation : ObservationRows{proposal.pre_target}) {
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
        if (!transact_filled_units(plan_transact->signed_units,
                                   proposal.inspected_closed_units,
                                   proposal.inspected_opened_units, &filled)) {
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

    ExecutionValues values;
    values.live_index = live_index;
    values.ordinal = usable_ordinal(next_timeline_ordinal);
    values.filled = filled;
    values.flatten = flatten;
    values.units_exhausted = units_exhausted;
    values.remaining_after = std::move(remaining_after);
    values.allowance_after = std::move(allowance_after);
    values.scope = std::move(canonical_scope);
    return values;
}

Preparation<std::monostate> WorkingRequestCore::check_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal) {
    return check_execution(target, proposal, next_timeline_ordinal, false);
}

Preparation<std::monostate> WorkingRequestCore::check_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
    auto values = execution_values(target, proposal, next_timeline_ordinal,
                                   open_activation_grant);
    if (const auto* none = std::get_if<NoChange>(&values)) return *none;
    if (const auto* error = std::get_if<PreparationError>(&values)) return *error;
    // prepare_execution's plan: begin_plan's checks, then seal_plan for the
    // one event the install appends and no row push.
    begin_direct();
    seal_direct(1, false, false, false);
    return std::monostate{};
}

InstallResult WorkingRequestCore::apply_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        const CommittedExecutionFacts& facts,
        uint64_t& next_timeline_ordinal) {
    return apply_execution(target, proposal, facts, next_timeline_ordinal, false);
}

InstallResult WorkingRequestCore::apply_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        const CommittedExecutionFacts& facts,
        uint64_t& next_timeline_ordinal,
        bool open_activation_grant) {
    auto computed = execution_values(target, proposal, next_timeline_ordinal,
                                     open_activation_grant);
    auto* values = std::get_if<ExecutionValues>(&computed);
    if (!values || epoch_ == std::numeric_limits<uint64_t>::max()) {
        return InstallError::StalePreparation;
    }
    // install_execution's checks, in its order.
    if (facts.result.status != execution::Status::Applied) return InstallError::WrongCoreOrRun;
    if (facts.result.closed_units != proposal.inspected_closed_units
        || facts.result.opened_units != proposal.inspected_opened_units
        || facts.result.current_ticket != proposal.inspected_current_ticket) {
        return InstallError::WrongCoreOrRun;
    }
    LiveRequest& live = live_[values->live_index];
    const bool can_retain = !values->flatten && !values->units_exhausted;
    bool retain = can_retain;
    if (const auto* selected = std::get_if<OpeningsClose>(&live.authority)) {
        std::size_t live_count = 0;
        if (observe_openings(facts.post_target, *selected, &live_count)) {
            return InstallError::WrongCoreOrRun;
        }
        if (live_count == 0) retain = false;
    }
    if (retain) {
        if (const auto* opening = std::get_if<OpeningClose>(&live.authority)) {
            if (!opening_close_alive(facts.post_target, *opening)) retain = false;
        }
    }
    if (retain) {
        if (const auto* book = std::get_if<BookClose>(&live.authority)) {
            if (!book_close_alive(facts.post_target, *book)) retain = false;
        }
    }

    // The event the token would have held, stamped with the committed facts.
    ExecutionAppliedEvent event;
    event.ordinal = values->ordinal;
    event.definition = live.definition;
    event.raw_price = proposal.raw_price;
    event.resolved_price = proposal.resolved_price;
    event.current_ticket = proposal.inspected_current_ticket;
    event.first_trade_index = facts.result.first_trade_index;
    event.closed_trade_count = facts.result.closed_trade_count;
    event.opened_lot_incarnation = facts.result.opened_lot_incarnation;
    event.closed_units = proposal.inspected_closed_units;
    event.opened_units = proposal.inspected_opened_units;
    event.filled_working = values->filled;
    event.remaining_before = project_remaining(live.remaining);
    event.allowance_before = live.allowance;
    event.allowance_after = values->allowance_after;
    event.cycle_before = facts.cycle_before;
    event.cycle_after = facts.cycle_after;
    event.scope = std::move(values->scope);
    fill_applied_cursor(event, proposal.cursor);
    if (retain) {
        event.terminal = false;
        event.remaining_after = values->remaining_after;
        event.terminal_reason = std::nullopt;
    } else {
        event.terminal = true;
        event.remaining_after = values->flatten ? RemainingProjection{RemainingProjectionFlattenAll{}}
                : (values->units_exhausted ? RemainingProjection{RemainingProjectionUnits{0.0}}
                                           : values->remaining_after);
        event.terminal_reason = values->flatten ? AppliedTerminalReason::Flattened
                : (values->units_exhausted ? AppliedTerminalReason::WorkingUnitsSatisfied
                                           : AppliedTerminalReason::TargetExhausted);
    }
    // check_execution sealed the reservation; this is commit's half.
    const std::size_t first = history_end();
    append_direct(std::move(event), false);
    if (retain) {
        live.remaining =
                RemainingUnits{std::get<RemainingProjectionUnits>(values->remaining_after).q};
        live.allowance = std::move(values->allowance_after);
    } else {
        erase_live(values->live_index);
    }
    return Installed{finish_direct(first)};
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_group_effect(
        const EventId& applied,
        const RequestHandle& recipient,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    // The cause is copied: the receipt lookup below reads journal pages and
    // may evict the cached page this event would otherwise be read from.
    std::optional<CommandEvent> cause;
    if (const CommandEvent* found = event_at(applied)) cause = *found;
    const auto* payload = cause ? as_applied(*cause) : nullptr;
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
    // Receipts commit in cause order; one cause's in the order they are
    // applied, whatever the recipients' incarnations (receipt_lookup).
    const std::size_t receipt_count = group_effect_receipt_count();
    if (receipt_count > group_effect_receipt_base()
        && applied.ordinal < receipt_store_->log.get(receipt_count - 1).cause) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
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
        // A fill the pending total absorbs (K-ULP5) joins no chain: the total,
        // its count and its tail stay, and the event defers nothing.
        bool absorbed = false;
        if (const auto* pending = std::get_if<PendingDeferred>(&live.pending)) {
            if (absorbed_pending_delta(pending->total, payload->filled_working)) {
                absorbed = true;
                after = *pending;
            } else if (!pending_sum(pending->total, payload->filled_working, &after.total)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
            } else {
                after.count = pending->count + 1;
                after.tail_receipt = EventId{identity_, ordinal};
            }
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
        deferred.deferred_delta = absorbed ? 0.0 : payload->filled_working;
        deferred.pending_before = before;
        deferred.pending_after = after;
        if (const auto* pending = std::get_if<PendingDeferred>(&before)) {
            if (!absorbed) deferred.previous_pending_receipt = pending->tail_receipt;
        }
        plan.events.emplace_back(std::move(deferred));
        if (!absorbed) {
            LiveRequest updated = live;
            updated.pending = after;
            plan.live_change = kLiveUpdate;
            plan.live_index = live_index;
            plan.live_row = std::move(updated);
        }
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
    double after_q = before_q;
    bool exhausted = false;
    // A deduction binary64 cannot take off the remaining units is absorbed
    // (K-ULP5): the units stand, the event records a deduction of 0, and the
    // row is not touched.
    const bool absorbed = absorbed_deduction(before_q, deduct);
    if (!absorbed && !checked_sub_cap(before_q, deduct, &after_q, &exhausted)) {
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
    reduced.actual_deduction = absorbed ? 0.0 : deduct;
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
    } else if (!absorbed) {
        LiveRequest updated = live;
        updated.remaining = RemainingUnits{after_q};
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
    }
    return finish_mutation(std::move(plan));
}

Preparation<Installed> WorkingRequestCore::apply_group_effect(
        const EventId& applied,
        const RequestHandle& recipient,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    // Copied for the same reason as in prepare_group_effect.
    std::optional<CommandEvent> cause;
    if (const CommandEvent* found = event_at(applied)) cause = *found;
    const auto* payload = cause ? as_applied(*cause) : nullptr;
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
    const std::size_t receipt_count = group_effect_receipt_count();
    if (receipt_count > group_effect_receipt_base()
        && applied.ordinal < receipt_store_->log.get(receipt_count - 1).cause) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
    }
    std::size_t live_index = 0;
    if (classify(recipient, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest& live = live_[live_index];
    const auto* other = std::get_if<Member>(&live.request().group);
    if (!other || other->group != member->group || other->cohort == member->cohort
        || live.birth().acceptance_ordinal >= payload->ordinal) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
    }
    begin_direct();
    const GroupEffect effect = member->effect;
    const double filled_working = payload->filled_working;
    // commit's receipt, after the events and the row change.
    auto receipt = [&](uint64_t outcome) {
        receipt_store_->log.push(ReceiptRec{applied.ordinal, recipient.incarnation, outcome,
                                            static_cast<uint64_t>(effect)});
    };
    // One event that ends the recipient, with its receipt.
    auto end_with_receipt = [&](auto&& ending, uint64_t outcome) -> Preparation<Installed> {
        seal_direct(1, false, true, false);
        const std::size_t first = history_end();
        append_direct(std::move(ending), false);
        erase_live(live_index);
        receipt(outcome);
        return Installed{finish_direct(first)};
    };

    if (std::holds_alternative<RemainingUnbound>(live.remaining)
        || std::holds_alternative<RemainingDeferred>(live.remaining)
        || std::holds_alternative<NoTarget>(live.remaining)) {
        if (effect == GroupEffect::Cancel) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            return end_with_receipt(make_cancelled(ordinal, live, CancelReason::Group, applied),
                                    ordinal);
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        PendingDeferred after;
        PendingAdjustments before = live.pending;
        // prepare_group_effect, step for step: an absorbed fill joins no chain.
        bool absorbed = false;
        if (const auto* pending = std::get_if<PendingDeferred>(&live.pending)) {
            if (absorbed_pending_delta(pending->total, filled_working)) {
                absorbed = true;
                after = *pending;
            } else if (!pending_sum(pending->total, filled_working, &after.total)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
            } else {
                after.count = pending->count + 1;
                after.tail_receipt = EventId{identity_, ordinal};
            }
        } else {
            after.total = filled_working;
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
        deferred.deferred_delta = absorbed ? 0.0 : filled_working;
        deferred.pending_before = before;
        deferred.pending_after = after;
        if (const auto* pending = std::get_if<PendingDeferred>(&before)) {
            if (!absorbed) deferred.previous_pending_receipt = pending->tail_receipt;
        }
        seal_direct(1, false, true, false);
        const std::size_t first = history_end();
        append_direct(std::move(deferred), false);
        if (!absorbed) live.pending = after;
        receipt(ordinal);
        return Installed{finish_direct(first)};
    }

    if (effect == GroupEffect::Cancel
        || std::holds_alternative<RemainingFlattenAll>(live.remaining)) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        return end_with_receipt(make_cancelled(ordinal, live, CancelReason::Group, applied),
                                ordinal);
    }

    const double before_q = working_units(live.remaining);
    const double deduct = std::min(filled_working, before_q);
    double after_q = before_q;
    bool exhausted = false;
    const bool absorbed = absorbed_deduction(before_q, deduct);
    if (!absorbed && !checked_sub_cap(before_q, deduct, &after_q, &exhausted)) {
        return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    ReservationReducedEvent reduced;
    reduced.ordinal = ordinal;
    reduced.definition = live.definition;
    reduced.cause = applied;
    reduced.recipient = recipient;
    reduced.effect = GroupEffect::Reduce;
    reduced.requested_delta = filled_working;
    reduced.actual_deduction = absorbed ? 0.0 : deduct;
    reduced.before = RemainingUnits{before_q};
    reduced.after = RemainingProjectionUnits{after_q};
    if (exhausted) {
        uint64_t cancel_ord = ordinal + 1;
        if (cancel_ord == 0 || cancel_ord == std::numeric_limits<uint64_t>::max()
            || cancel_ord <= last_ordinal_) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
        LiveRequest snapshot = live;
        snapshot.remaining = RemainingUnits{before_q};
        CancelledEvent cancelled =
                make_cancelled(cancel_ord, snapshot, CancelReason::Group, applied);
        seal_direct(2, false, true, false);
        const std::size_t first = history_end();
        append_direct(std::move(reduced), false);
        append_direct(std::move(cancelled), false);
        erase_live(live_index);
        receipt(ordinal);
        return Installed{finish_direct(first)};
    }
    seal_direct(1, false, true, false);
    const std::size_t first = history_end();
    append_direct(std::move(reduced), false);
    if (!absorbed) live.remaining = RemainingUnits{after_q};
    receipt(ordinal);
    return Installed{finish_direct(first)};
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_owner_applied(
        const EventId& applied,
        const RequestHandle& child,
        const std::optional<OpeningObservation>& observation,
        uint64_t& next_timeline_ordinal,
        const ArmContext& arm) {
    require_identity(identity_);
    // The cause is copied: the chain walk below reads many journal pages and
    // may evict the cached page this event would otherwise be read from.
    std::optional<CommandEvent> cause;
    if (const CommandEvent* found = event_at(applied)) cause = *found;
    const auto* payload = cause ? as_applied(*cause) : nullptr;
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
        // The arm installs a level, not a new chain: a leg that replaced
        // another keeps the root cohorts name it by, and its place in the
        // queue.
        materialized.root = live.definition->root;
        materialized.priority = live.definition->priority;
        materialized.kept_binding = live.definition->kept_binding;
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
        ChainIds chain;
        double pending_total = 0.0;
        if (!collect_pending_chain(live.pending, child, &chain, &pending_total)) {
            return PreparationError{CoreFailure::ConflictingReceipt, applied, child};
        }
        double deduct = std::min(pending_total, source);
        double after = source;
        bool exhausted = deduct == source;
        if (deduct > 0.0 && !exhausted) {
            if (absorbed_deduction(source, deduct)) {
                // Absorbed (K-ULP5): the owner's units stand, nothing deducted.
                deduct = 0.0;
            } else if (!checked_sub_cap(source, deduct, &after, &exhausted)) {
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
        bound.prior_adjustment_ids = std::move(chain.inline_ids);
        bound.pending_total = pending_total;
        bound.effective_deduction = deduct;
        bound.remaining = RemainingProjectionUnits{after};
        plan.events.emplace_back(std::move(bound));
        if (chain.external) {
            plan.chain_segment = true;
            plan.chain_event_ordinal = ordinal;
            plan.chain_offset = chain.offset;
            plan.chain_count = chain.count;
        }
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

Preparation<Installed> WorkingRequestCore::apply_owner_applied(
        const EventId& applied,
        const RequestHandle& child,
        const std::optional<OpeningObservation>& observation,
        uint64_t& next_timeline_ordinal,
        const ArmContext& arm) {
    require_identity(identity_);
    // Copied for the same reason as in prepare_owner_applied.
    std::optional<CommandEvent> cause;
    if (const CommandEvent* found = event_at(applied)) cause = *found;
    const auto* payload = cause ? as_applied(*cause) : nullptr;
    if (!payload) return PreparationError{CoreFailure::InvalidCause, applied, child};
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest& live = live_[live_index];
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
        begin_direct();
        return end_direct(live_index, make_cancelled(ordinal, live, CancelReason::UnsupportedRelation,
                                                     applied));
    }

    begin_direct();
    // An anchored leg receives its level from this fill, once, at the arm
    // that binds it to the owner (prepare_owner_applied, step for step,
    // the host's restatement included).
    DefinitionRef armed_definition = live.definition;
    const bool anchored = std::holds_alternative<FromOwnerFill>(live.request().anchor);
    if (anchored) {
        RequestDefinition materialized{live.definition->handle, live.request(),
                                       live.definition->birth, live.definition->predecessor};
        materialized.root = live.definition->root;
        materialized.priority = live.definition->priority;
        materialized.kept_binding = live.definition->kept_binding;
        const bool leg_is_buy = closing ? !(payload->opened_units > 0.0)
                                        : working_is_buy(live);
        double level = 0.0;
        if (!anchored_kernel_level(materialized.request, payload->resolved_price, arm.price_tick,
                                   leg_is_buy, &level)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, applied, child};
        }
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
        seal_direct(1, false, false, false);
        const std::size_t first = history_end();
        append_direct(std::move(armed_event), false);
        if (anchored) live.definition = std::move(armed_definition);
        live.authority = std::move(armed);
        return Installed{finish_direct(first)};
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
    std::optional<QuantityBoundEvent> bound;
    ChainIds chain;
    const bool owner_opened =
            as_reduce(live.request().intent) && is_owner_opened(*as_reduce(live.request().intent));
    if (owner_opened) {
        const double source = std::abs(payload->opened_units);
        if (!finite_positive(source)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, applied, child};
        }
        double pending_total = 0.0;
        if (!collect_pending_chain(live.pending, child, &chain, &pending_total)) {
            return PreparationError{CoreFailure::ConflictingReceipt, applied, child};
        }
        double deduct = std::min(pending_total, source);
        double after = source;
        bool exhausted = deduct == source;
        if (deduct > 0.0 && !exhausted) {
            if (absorbed_deduction(source, deduct)) {
                deduct = 0.0;   // absorbed, as prepare_owner_applied
            } else if (!checked_sub_cap(source, deduct, &after, &exhausted)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, child};
            }
        } else if (exhausted) {
            after = 0.0;
        }
        bound.emplace();
        bound->ordinal = ordinal;
        bound->definition = live.definition;
        bound->source = applied;
        bound->source_units = source;
        bound->prior_adjustment_ids = std::move(chain.inline_ids);
        bound->pending_total = pending_total;
        bound->effective_deduction = deduct;
        bound->remaining = RemainingProjectionUnits{after};
        quantity_resolution = EventId{identity_, ordinal};
        if (exhausted) {
            uint64_t cancel_ord = ordinal + 1;
            if (cancel_ord <= last_ordinal_ || cancel_ord == 0) {
                throw std::invalid_argument("native timeline ordinal reused or regressed");
            }
            CancelledEvent cancelled =
                    make_cancelled(cancel_ord, live, CancelReason::Group, applied);
            seal_direct(2, false, false, false, chain.external);
            const std::size_t first = history_end();
            append_direct(std::move(*bound), false);
            if (chain.external) note_chain_segment(ordinal, chain.offset, chain.count);
            append_direct(std::move(cancelled), false);
            erase_live(live_index);
            return Installed{finish_direct(first)};
        }
        remaining = RemainingUnits{after};
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
    seal_direct(owner_opened ? 2 : 1, false, false, false, chain.external);
    const std::size_t first = history_end();
    if (bound) {
        append_direct(std::move(*bound), false);
        if (chain.external) note_chain_segment(ordinal, chain.offset, chain.count);
    }
    append_direct(std::move(armed_event), false);
    if (anchored) live.definition = std::move(armed_definition);
    if (owner_opened) {
        note_chain_loss(live);
        live.pending = PendingNone{};
        live.remaining = std::move(remaining);
    }
    live.authority = std::move(armed_authority);
    return Installed{finish_direct(first)};
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
        // A re-price that kept the handle leaves the parent working.
        parent_ended = replaced->predecessor() == wait->parent && !replaced->kept_handle();
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

Preparation<Installed> WorkingRequestCore::apply_bound_expiry(
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
    begin_direct();
    return end_direct(live_index,
                      make_cancelled(ordinal, live, CancelReason::OwnerGone, physical_cause));
}

Preparation<Installed> WorkingRequestCore::apply_parent_terminal(
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
        // A re-price that kept the handle leaves the parent working.
        parent_ended = replaced->predecessor() == wait->parent && !replaced->kept_handle();
    } else if (const auto* none = std::get_if<NoEffectEvent>(event)) {
        parent_ended = none->handle() == wait->parent;
    } else if (const auto* rejected = std::get_if<MatchRejectedEvent>(event)) {
        parent_ended = rejected->handle() == wait->parent;
    } else if (const auto* applied = as_applied(*event)) {
        parent_ended = applied->handle() == wait->parent && applied->terminal;
    }
    if (!parent_ended) return NoChange{NoChangeReason::NoTransition};

    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    begin_direct();
    return end_direct(live_index,
                      make_cancelled(ordinal, live, CancelReason::OwnerGone, terminal_or_replaced));
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

// CR-150 L19A. This helper stays in the core's owning epoch and translation
// unit. Its shared_ptr permits an incomplete private type in the public
// header; WorkingRequestCore is noncopyable, so the file has one owner.
// Only physical storage changes: note_committed_events still folds every
// (successor, root) pair and its total into chain_roots_, in the same order.
struct WorkingRequestCore::SuccessorRootIndex {
    struct Row {
        uint64_t successor = 0;
        uint64_t root = 0;
    };
    static_assert(sizeof(Row) == 16, "successor root scratch rows are 16 bytes");
    static constexpr std::size_t kTailRows = 64;

    // These are storage metadata, not additional broker state: the prefix
    // and tail partition the root sequence already in chain_roots_. The slab,
    // backing, directory and partition boundary do not enter its digest.
    std::array<Row, kTailRows> tail{};
    std::size_t tail_size = 0;
    uint64_t disk_rows = 0;
    detail::ScratchFile file{detail::ScratchFile::IoRaiser::auxiliary};
    std::string directory;
    std::shared_ptr<detail::ScratchSlab> slab;
    // Transient input for one bounded-memory preparation, never a root count.
    bool next_adds_root = true;

    SuccessorRootIndex() { file.set_store(detail::ScratchStore::successor_roots); }
    explicit SuccessorRootIndex(const std::shared_ptr<detail::ScratchSlab>& owner) noexcept
        : SuccessorRootIndex() { slab = owner; }
    SuccessorRootIndex(const SuccessorRootIndex&) = delete;
    SuccessorRootIndex& operator=(const SuccessorRootIndex&) = delete;

    void set_directory(const std::string& value) {
        if (file.opened() && directory != value) {
            throw std::logic_error("native successor roots directory changed after spill");
        }
        file.set_directory(value);
        directory = value;
    }

    void open_file() {
        assert(!slab);
        try {
            file.open("pineforge-roots");
        } catch (const detail::AuxiliaryScratchIoError& error) {
            // Preserve the roots diagnostic without catching arena resource failures.
            throw detail::AuxiliaryScratchIoError(error.code().value(), "native successor roots create");
        }
    }

    void prepare_append() {
        if (slab) {
            const bool adds_root = next_adds_root;
            next_adds_root = true; // Also reset before a refused ticket throws.
            // One read-only ticket covers BOTH the inline rows and the full
            // future prefix, even before the first 64-row flush. Rootless
            // preparations can flush a full tail without consuming a row.
            const auto limit = slab->manifest().R;
            const auto pending = static_cast<uint64_t>(tail_size);
            if (disk_rows > limit || pending > limit - disk_rows
                || (adds_root && disk_rows + pending == limit)) {
                throw detail::ScratchCapacityError(detail::ScratchResource::successor_roots);
            }
            const auto rows = disk_rows + pending + (adds_root ? uint64_t{1} : uint64_t{0});
            // compute_layout already proved 16*R representable before bind.
            slab->require_extend(detail::ScratchResource::successor_roots, rows * sizeof(Row));
        }
        if (tail_size < tail.size()) return;
        const std::size_t length = tail_size * sizeof(Row);
        if (slab) {
            slab->write_at(detail::ScratchResource::successor_roots,
                           disk_rows * sizeof(Row), tail.data(), length);
        } else {
            const auto maximum_rows = static_cast<uint64_t>(std::numeric_limits<off_t>::max()) / sizeof(Row);
            if (disk_rows > maximum_rows - tail_size) {
                throw coded<std::overflow_error>(RunFailureCode::scratch_io_failed, {},
                        "native successor roots file offset exhausted");
            }
            open_file();
            try {
                file.write(disk_rows * sizeof(Row), tail.data(), length);
            } catch (const detail::AuxiliaryScratchIoError& error) {
                throw detail::AuxiliaryScratchIoError(error.code().value(), "native successor roots write");
            }
        }
        // Publish only a complete batch. A short/error write leaves the
        // authoritative tail intact; retry overwrites the same file offset.
        disk_rows += tail_size;
        tail_size = 0;
    }

    void append(uint64_t successor, uint64_t root) noexcept {
        assert(tail_size < tail.size());
        assert(root != 0 && root < successor);
        assert(tail_size == 0 || tail[tail_size - 1].successor < successor);
        tail[tail_size++] = Row{successor, root};
    }

    Row read(uint64_t index) const {
        assert((slab || file.opened()) && index < disk_rows);
        Row row;
        if (slab) {
            slab->read_at(detail::ScratchResource::successor_roots,
                          index * sizeof(Row), &row, sizeof(Row));
        } else {
            try {
                file.read(index * sizeof(Row), &row, sizeof(Row));
            } catch (const detail::AuxiliaryScratchIoError& error) {
                throw detail::AuxiliaryScratchIoError(error.code().value(), "native successor roots read");
            }
        }
        if (row.root == 0 || row.root >= row.successor) {
            throw detail::AuxiliaryScratchIoError(EIO, "native successor roots invalid row");
        }
        return row;
    }

    std::optional<uint64_t> find(uint64_t successor) const {
        if (tail_size != 0 && successor >= tail[0].successor) {
            const auto end = tail.begin() + static_cast<std::ptrdiff_t>(tail_size);
            const auto where = std::lower_bound(tail.begin(), end, successor,
                    [](const Row& row, uint64_t key) { return row.successor < key; });
            return where != end && where->successor == successor
                    ? std::optional<uint64_t>{where->root} : std::nullopt;
        }
        uint64_t first = 0;
        uint64_t last = disk_rows;
        while (first < last) {
            const uint64_t middle = first + (last - first) / 2;
            const Row row = read(middle);
            if (row.successor == successor) return row.root;
            if (row.successor < successor) first = middle + 1;
            else last = middle;
        }
        return std::nullopt;
    }
};

// These reservations need the complete private helper to stage its transient
// ticket. Their declarations and all callers keep their existing signatures.
void WorkingRequestCore::reserve_plan(const MutationPlan& plan) {
    // The journal's room first: it may write its oldest resident events to an
    // external page, which is a logical no-op, so a later refusal below leaves
    // the core as it was.
    journal_->make_room(plan.events.size());
    if (plan.add_receipt) {
        ensure_receipt_store();
        receipt_store_->log.make_room();
    }
    if (plan.chain_segment) {
        ensure_chain_segments();
        chain_segments_->index.make_room();
    }
    // index_committed's two rows: an incarnation opens at most one issued
    // range and names at most one successor root.
    if (plan.consume_incarnation) {
        reserve_n(issued_, 1);
        if (successor_roots_ && successor_roots_->slab) {
            successor_roots_->next_adds_root = plan.live_row.definition
                    && plan.live_row.definition->root
                    && plan.live_row.handle().incarnation == plan.incarnation_used;
        }
        reserve_successor_root();
    }
    // A re-price that kept its handle moves its row in the queue, which
    // repriced_ records (note_repriced).
    if (plan.live_change == kLiveErasePush && plan.live_row.definition
        && plan.live_row.priority() != plan.live_row.handle().incarnation) {
        reserve_n(repriced_, 1);
    }
    // The book's own room, last: when live_ is at its bound the newest
    // resident rows move to the external table, which shifts what a token
    // prepared earlier indexed (the epoch rises), so a refusal above leaves
    // the book and the epoch as they were.
    if (plan.live_change == kLivePush) {
        make_book_room();
        reserve_n(live_, 1);
    }
}

void WorkingRequestCore::seal_direct(std::size_t events, bool push_live, bool add_receipt,
                                     bool consume_incarnation, bool chain_segment) {
    require_epoch_room();
    journal_->make_room(events);
    if (add_receipt) {
        ensure_receipt_store();
        receipt_store_->log.make_room();
    }
    if (chain_segment) {
        ensure_chain_segments();
        chain_segments_->index.make_room();
    }
    if (consume_incarnation) {
        reserve_n(issued_, 1);
        if (successor_roots_ && successor_roots_->slab) {
            // Only replacements root a newly issued incarnation.
            successor_roots_->next_adds_root = !push_live;
        }
        reserve_successor_root();
    }
    // The book's room last (see reserve_plan).
    if (push_live) {
        make_book_room();
        reserve_n(live_, 1);
    }
    if (!bump_epoch()) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
}

// Called after reset and before the first callback. Construct the fixed tail
// here, so bounded operations neither allocate a helper nor create a file.
void WorkingRequestCore::bind_scratch_slab(
        const std::shared_ptr<::pineforge::detail::ScratchSlab>& slab) {
    if (slab) {
        successor_roots_ = std::make_shared<SuccessorRootIndex>(slab);
    } else if (successor_roots_ && successor_roots_->slab) {
        successor_roots_.reset();
    }
}

void WorkingRequestCore::set_successor_roots_directory(const std::string& directory) {
    if (!successor_roots_) successor_roots_ = std::make_shared<SuccessorRootIndex>();
    successor_roots_->set_directory(directory);
    // The journal's pages, the group-effect keys, the long chains' id lists
    // and the working book's spilled rows spill beside the successor roots;
    // the journal store keeps the name for the stores created later.
    if (journal_) journal_->set_directory(directory);
    if (receipt_store_) receipt_store_->log.set_directory(directory);
    if (chain_segments_) chain_segments_->set_directory(directory);
    if (book_spill_) book_spill_->set_directory(directory);
}

// Where a store created now spills: the directory set_successor_roots_directory
// named (empty: TMPDIR, then /tmp). The journal store keeps it, so that the
// core itself holds no string for it and binding a scratch slab (which swaps
// the successor-root index) cannot lose it.
const std::string& WorkingRequestCore::scratch_directory() const noexcept {
    static const std::string none;
    return journal_ ? journal_->where : none;
}

void WorkingRequestCore::reserve_successor_root() {
    if (!successor_roots_) successor_roots_ = std::make_shared<SuccessorRootIndex>();
    successor_roots_->prepare_append();
}

void WorkingRequestCore::append_successor_root(uint64_t incarnation, uint64_t root) noexcept {
    assert(successor_roots_);
    successor_roots_->append(incarnation, root);
}

std::optional<uint64_t> WorkingRequestCore::successor_root(uint64_t incarnation) const {
    return successor_roots_ ? successor_roots_->find(incarnation) : std::nullopt;
}

// The consumer folds at commit and pins every queued cause before retiring the
// journal. Its actual watermark also respects deferred chains, so only the
// cause-sorted prefix at or below retired_through_ is no longer askable
// (receipts_first_above / apply_receipt_base, called by retire_history). The
// base preserves absolute receipt indices and the already-folded total; RAM
// holds only the resident tail, and a pinned journal's older keys sit in the
// external log.

}  // inline namespace native_order_v7
}  // namespace pineforge::native_order

namespace pineforge::detail {
namespace {
ScratchStatus payload_word(const PayloadField& field, PayloadType type,
                           std::uint64_t& out) noexcept {
    if (field.type != type || field.bytes.size() != 8) return ScratchStatus::corrupt;
    std::array<std::byte, 8> bytes{};
    const auto status = field.bytes.read(0, {bytes.data(), bytes.size()});
    if (status == ScratchStatus::ok) out = payload_decode(bytes.data());
    return status;
}
}
ScratchStatus PayloadField::unsigned_value(std::uint64_t& out) const noexcept {
    return payload_word(*this, PayloadType::unsigned_integer, out);
}
ScratchStatus PayloadField::signed_value(std::int64_t& out) const noexcept {
    std::uint64_t bits = 0;
    const auto status = payload_word(*this, PayloadType::signed_integer, bits);
    if (status == ScratchStatus::ok) std::memcpy(&out, &bits, sizeof(out));
    return status;
}
ScratchStatus PayloadField::real_value(double& out) const noexcept {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    const auto status = payload_word(*this, PayloadType::real, bits);
    if (status == ScratchStatus::ok) std::memcpy(&out, &bits, sizeof(out));
    return status;
}
ScratchStatus PayloadField::boolean_value(bool& out) const noexcept {
    std::uint64_t value = 0;
    const auto status = payload_word(*this, PayloadType::boolean, value);
    if (status != ScratchStatus::ok) return status;
    if (value > 1) return ScratchStatus::corrupt;
    out = value != 0;
    return ScratchStatus::ok;
}

ScratchStatus PayloadReadback::root(std::array<std::uint64_t, 3>& out) const noexcept {
    if (!store_) return ScratchStatus::programming_error;
    ScratchRecordInfo info;
    auto status = store_->point(version_, info);
    if (status != ScratchStatus::ok) return status;
    if (info.key_bytes != 32 || info.value_bytes != 24) return ScratchStatus::corrupt;
    std::array<std::uint64_t, 4> key{};
    status = store_->read(version_, ScratchField::key, 0,
        {reinterpret_cast<std::byte*>(key.data()), sizeof(key)});
    if (status != ScratchStatus::ok) return status;
    if (key[1] != payload_root_kind || key[2] != 0) return ScratchStatus::corrupt;
    std::array<std::byte, 24> bytes{};
    status = store_->read(version_, ScratchField::value, 0, {bytes.data(), bytes.size()});
    if (status != ScratchStatus::ok) return status;
    std::array<std::uint64_t, 3> value{};
    for (unsigned i = 0; i != 3; ++i) value[i] = payload_decode(bytes.data() + i * 8);
    if (value[0] != static_cast<std::uint64_t>(schema_) || value[1] != key[0])
        return ScratchStatus::corrupt;
    out = value;
    return ScratchStatus::ok;
}
ScratchStatus PayloadReadback::field_count(std::uint64_t& out) const noexcept {
    std::array<std::uint64_t, 3> value{};
    const auto status = root(value);
    if (status != ScratchStatus::ok) return status;
    if (first_ > value[2]) return ScratchStatus::corrupt;
    const auto remaining = value[2] - first_;
    if (limit_ != std::numeric_limits<std::uint64_t>::max() && limit_ > remaining)
        return ScratchStatus::corrupt;
    out = std::min(remaining, limit_);
    return ScratchStatus::ok;
}
ScratchStatus PayloadReadback::field(std::uint64_t index, PayloadField& out) const noexcept {
    std::array<std::uint64_t, 3> value{};
    auto status = root(value);
    if (status != ScratchStatus::ok) return status;
    if (first_ > value[2]) return ScratchStatus::corrupt;
    const auto remaining = value[2] - first_;
    if (limit_ != std::numeric_limits<std::uint64_t>::max() && limit_ > remaining)
        return ScratchStatus::corrupt;
    if (index >= std::min(remaining, limit_)) return ScratchStatus::not_found;
    const std::array<std::uint64_t, 4> key{value[1], payload_field_kind, version_, first_ + index};
    ScratchSpanSource probe({reinterpret_cast<const std::byte*>(key.data()), sizeof(key)});
    ScratchHandle handle = 0;
    status = store_->lower_bound(probe, handle);
    if (status != ScratchStatus::ok) return status;
    ScratchRecordInfo info;
    status = store_->point(handle, info);
    if (status != ScratchStatus::ok) return status;
    if (info.key_bytes != sizeof(key) || info.value_bytes < 16) return ScratchStatus::corrupt;
    std::array<std::uint64_t, 4> actual{};
    status = store_->read(handle, ScratchField::key, 0,
        {reinterpret_cast<std::byte*>(actual.data()), sizeof(actual)});
    if (status != ScratchStatus::ok) return status;
    if (actual != key) return ScratchStatus::corrupt;
    std::array<std::byte, 16> header{};
    status = store_->read(handle, ScratchField::value, 0, {header.data(), header.size()});
    if (status != ScratchStatus::ok) return status;
    const auto type = payload_decode(header.data());
    const auto name_size = payload_decode(header.data() + 8);
    if (type > static_cast<std::uint64_t>(PayloadType::object_end)
        || name_size > info.value_bytes - 16) return ScratchStatus::corrupt;
    PayloadField field;
    field.type = static_cast<PayloadType>(type);
    field.name = PayloadBytes(*store_, handle, 16, name_size);
    field.bytes = PayloadBytes(*store_, handle, 16 + name_size,
                              info.value_bytes - 16 - name_size);
    out = field;
    return ScratchStatus::ok;
}
ScratchStatus PayloadReadback::find(std::string_view name, PayloadField& out) const noexcept {
    std::uint64_t count = 0, depth = 0;
    auto status = field_count(count);
    if (status != ScratchStatus::ok) return status;
    for (std::uint64_t i = 0; i != count; ++i) {
        PayloadField field;
        status = this->field(i, field);
        if (status != ScratchStatus::ok) return status;
        if (depth == 0 && field.name.size() == name.size()) {
            bool equal = true;
            std::array<std::byte, 64> bytes{};
            for (std::size_t offset = 0; offset < name.size();) {
                const auto n = std::min(bytes.size(), name.size() - offset);
                status = field.name.read(offset, {bytes.data(), n});
                if (status != ScratchStatus::ok) return status;
                if (std::memcmp(bytes.data(), name.data() + offset, n)) equal = false;
                offset += n;
            }
            if (equal) { out = field; return ScratchStatus::ok; }
        }
        if (field.type == PayloadType::object_begin) ++depth;
        if (field.type == PayloadType::object_end) {
            if (!depth) return ScratchStatus::corrupt;
            --depth;
        }
    }
    return depth ? ScratchStatus::corrupt : ScratchStatus::not_found;
}
ScratchStatus PayloadReadback::subobject(
        std::string_view name, PayloadReadback& out) const noexcept {
    std::uint64_t count = 0, depth = 0, first = 0;
    bool found = false;
    auto status = field_count(count);
    if (status != ScratchStatus::ok) return status;
    for (std::uint64_t i = 0; i != count; ++i) {
        PayloadField field;
        status = this->field(i, field);
        if (status != ScratchStatus::ok) return status;
        if (depth == 0 && field.type == PayloadType::object_begin && field.name.size() == name.size()) {
            bool equal = true;
            std::array<std::byte, 64> bytes{};
            for (std::size_t offset = 0; offset < name.size();) {
                const auto n = std::min(bytes.size(), name.size() - offset);
                status = field.name.read(offset, {bytes.data(), n});
                if (status != ScratchStatus::ok) return status;
                if (std::memcmp(bytes.data(), name.data() + offset, n)) equal = false;
                offset += n;
            }
            if (equal) { found = true; first = i + 1; }
        }
        if (field.type == PayloadType::object_begin) ++depth;
        if (field.type == PayloadType::object_end) {
            if (!depth) return ScratchStatus::corrupt;
            --depth;
            if (found && !depth) {
                auto view = *this;
                view.first_ += first;
                view.limit_ = i - first;
                out = view;
                return ScratchStatus::ok;
            }
        }
    }
    return depth ? ScratchStatus::corrupt : ScratchStatus::not_found;
}
ScratchStatus PayloadReadback::visit(
        void* context, bool (*visitor)(void*, const PayloadField&)) const {
    if (!visitor) return ScratchStatus::programming_error;
    std::uint64_t count = 0;
    auto status = field_count(count);
    if (status != ScratchStatus::ok) return status;
    for (std::uint64_t i = 0; i != count; ++i) {
        PayloadField field;
        status = this->field(i, field);
        if (status != ScratchStatus::ok) return status;
        if (!visitor(context, field)) return ScratchStatus::cancelled;
    }
    return ScratchStatus::ok;
}
} // namespace pineforge::detail

namespace pineforge {
inline namespace engine_script_run_v19 {
namespace {
bool reader_visible(const native_order::LiveRequest& live) noexcept {
    const auto* wait = std::get_if<native_order::WaitForApplied>(&live.request().owner);
    return !wait || wait->visibility != native_order::NativeArmVisibility::PendingUntilArmed
        || !std::holds_alternative<native_order::Wait>(live.authority);
}
NativeWorkingRequest reader_working_row(const native_order::LiveRequest& live) {
    return {live.definition, native_order::project_remaining(live.remaining), live.trigger_state};
}
}

detail::ScratchStatus NativeStrategyHost::visit_native_working_requests(
        void* context, bool (*visitor)(void*, const NativeWorkingRequest&)) const {
    if (!visitor) return detail::ScratchStatus::programming_error;
    // A fresh host has no book. Do not instantiate its lazy consumer merely
    // to answer an empty read.
    if (!execution_consumer_slot_.ptr) return detail::ScratchStatus::ok;
    const auto& core = as_native_consumer(*execution_consumer_slot_.ptr).request_core();
    for (const auto& live : core.live()) {
        if (!reader_visible(live)) continue;
        const auto row = reader_working_row(live);
        if (!visitor(context, row)) return detail::ScratchStatus::cancelled;
    }
    return detail::ScratchStatus::ok;
}

void NativeStrategyHost::native_visit_working_requests(
        void* context, void (*visitor)(void*, const NativeWorkingRequest&)) const {
    if (!visitor) throw std::invalid_argument("null native working visitor");
    struct Call { void* context; void (*visitor)(void*, const NativeWorkingRequest&); };
    Call call{context, visitor};
    visit_native_working_requests(&call, [](void* context, const NativeWorkingRequest& row) {
        const auto& call = *static_cast<Call*>(context);
        call.visitor(call.context, row);
        return true;
    });
}

std::optional<NativeWorkingRequest> NativeStrategyHost::native_working_request(
        const native_order::RequestHandle& handle) const {
    if (!execution_consumer_slot_.ptr) return std::nullopt;
    const auto* live = as_native_consumer(*execution_consumer_slot_.ptr).request_core().find_live(handle);
    if (!live || !reader_visible(*live)) return std::nullopt;
    return reader_working_row(*live);
}

detail::ScratchStatus NativeStrategyHost::visit_native_open_lots(
        double mark, void* context, bool (*visitor)(void*, const NativeOpenLotView&)) const {
    if (!visitor) return detail::ScratchStatus::programming_error;
    if (pyramid_entries_.empty()) return detail::ScratchStatus::ok;
    const bool is_long = position_side_ != PositionSide::SHORT;
    const double point_value = syminfo_.pointvalue;
    const double fx = active_account_currency_fx();
    for (std::size_t i = 0; i < pyramid_entries_.size(); ++i) {
        const auto& lot = pyramid_entries_[i];
        NativeOpenLotView row;
        row.ordinal = i;
        row.entry_incarnation = lot.entry_incarnation;
        row.cycle = position_cycle_seq_;
        row.side = is_long ? native_order::Side::Long : native_order::Side::Short;
        row.entry_label = lot.entry_id;
        row.entry_comment = lot.entry_comment;
        row.entry_time_ms = lot.time;
        row.entry_bar_index = lot.entry_bar_index;
        row.entry_price = lot.price;
        row.signed_units = is_long ? lot.qty : -lot.qty;
        row.entry_commission = open_entry_commission(lot);
        row.mark = mark;
        double favorable = lot.max_runup;
        double adverse = lot.max_drawdown;
        if (std::isfinite(mark)) {
            const double move = (is_long ? (mark - lot.price) : (lot.price - mark)) * lot.qty;
            row.unrealized_pnl = move * point_value * fx - row.entry_commission;
            favorable = std::max(favorable, move);
            adverse = std::max(adverse, -move);
        }
        row.favorable_excursion = favorable * point_value * fx;
        row.adverse_excursion = adverse * point_value * fx;
        if (!visitor(context, row)) return detail::ScratchStatus::cancelled;
    }
    return detail::ScratchStatus::ok;
}

void NativeStrategyHost::native_visit_open_lots(
        double mark, void* context, void (*visitor)(void*, const NativeOpenLotView&)) const {
    if (!visitor) throw std::invalid_argument("null native lot visitor");
    struct Call { void* context; void (*visitor)(void*, const NativeOpenLotView&); };
    Call call{context, visitor};
    visit_native_open_lots(mark, &call, [](void* context, const NativeOpenLotView& row) {
        const auto& call = *static_cast<Call*>(context);
        call.visitor(call.context, row);
        return true;
    });
}

detail::ScratchStatus NativeStrategyHost::visit_native_open_lot_values(
        void* context, bool (*visitor)(void*, std::uint64_t, double, double)) const {
    if (!visitor) return detail::ScratchStatus::programming_error;
    const bool is_long = position_side_ != PositionSide::SHORT;
    for (const auto& lot : pyramid_entries_) {
        if (!visitor(context, lot.entry_incarnation, lot.price, is_long ? lot.qty : -lot.qty))
            return detail::ScratchStatus::cancelled;
    }
    return detail::ScratchStatus::ok;
}
} // inline namespace engine_script_run_v19
} // namespace pineforge
