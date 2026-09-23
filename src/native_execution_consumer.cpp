#include "native_execution_consumer.hpp"
#include "engine_internal.hpp"
#include "native_matching.hpp"

#include <pineforge/execution_close_scope.hpp>
#include <pineforge/market_driver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace pineforge {
inline namespace engine_script_run_v18 {
namespace {

template<class T>
void reserve_next(std::vector<T>& values) {
    if (values.size() == values.max_size())
        throw std::length_error("native observation capacity exhausted");
    if (values.size() < values.capacity()) return;
    const auto grown = values.capacity() > values.max_size() / 2
        ? values.max_size() : std::max<std::size_t>(1, values.capacity() * 2);
    values.reserve(grown);
}

// FNV-1a's prime and its powers p^k for k in [0, 8]. A zero byte leaves the
// accumulator untouched under the XOR (h ^ 0 == h), so a run of k zero bytes
// is exactly one multiplication by p^k: the same digest, folded with fewer
// dependent multiplies.
constexpr uint64_t kFnvPrime = 1099511628211ULL;

constexpr uint64_t fnv_power(unsigned exponent) noexcept {
    uint64_t value = 1;
    for (unsigned step = 0; step < exponent; ++step) value *= kFnvPrime;
    return value;
}

constexpr uint64_t kFnvPowers[9] = {
    fnv_power(0), fnv_power(1), fnv_power(2), fnv_power(3), fnv_power(4),
    fnv_power(5), fnv_power(6), fnv_power(7), fnv_power(8),
};

// A little-endian host can read an eight-byte field's object representation
// out of the value itself, which is what lets the zero runs collapse without
// touching memory. Anywhere else every fold goes through bytes() unchanged.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) \
    && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PINEFORGE_FNV_SCALAR_FOLD 1
#endif

// A continuation view's buffer grows here, out of the fold's way, and never
// throws: a buffer that cannot grow ends the recording and the capture folds.
bool reserve_view_bytes(std::vector<unsigned char>& out, size_t n) noexcept {
    if (n > out.max_size() - out.size()) return false;
    try {
        out.reserve(std::max({out.size() + n, out.capacity() * 2, size_t{4096}}));
    } catch (...) {
        return false;
    }
    return true;
}

struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    // Native run generations (`RunIdentity::run_number`, the consumed
    // high-water) are anti-stale counters that advance on every begin of a
    // reused host.  The continuation digest folds them relative to the run
    // it describes so a reused host's Nth run and a fresh host's first run
    // share one continuation identity while a leaked prior-generation handle
    // or event still folds a non-zero distance (A41(1)).
    uint64_t run_base = 0;
    void bytes(const void* p, size_t n) noexcept {
        const auto* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= kFnvPrime; }
    }
    // The eight object bytes of a scalar field, folded as bytes() would fold
    // them: byte zero first, every zero byte a bare multiplication. The runs
    // of zero bytes the kernel's encodings are full of -- the high bytes of
    // ordinals, enum words, millisecond stamps and sizes, the low mantissa
    // bytes of a round double -- collapse into one multiplication by
    // kFnvPowers[run] each, because XOR with zero is the identity and the
    // multiplications are associative. 62 % of the 94.1 MB a 43k-bar replay
    // folds are such zero bytes (lane E24).
    void scalar(uint64_t bits) noexcept {
#ifdef PINEFORGE_FNV_SCALAR_FOLD
        if (bits == 0) { h *= kFnvPowers[8]; return; }
        unsigned folded = static_cast<unsigned>(__builtin_ctzll(bits)) / 8u;
        if (folded != 0) { h *= kFnvPowers[folded]; bits >>= 8u * folded; }
        while (bits != 0) {
            h = (h ^ static_cast<unsigned char>(bits)) * kFnvPrime;
            bits >>= 8u;
            ++folded;
        }
        if (folded != 8) h *= kFnvPowers[8 - folded];
#else
        bytes(&bits, sizeof bits);
#endif
    }
    void u(uint64_t v) noexcept { scalar(v); }
    void i(int64_t v) noexcept {
        uint64_t bits = 0; std::memcpy(&bits, &v, sizeof bits); scalar(bits);
    }
    // Exact attempted IEEE-754 bits. Does not canonicalize NaN payloads or
    // signed zero; rejected request quantities keep their original encoding.
    void d(double v) noexcept {
        uint64_t bits = 0; std::memcpy(&bits, &v, sizeof bits); scalar(bits);
    }
    void b(bool v) noexcept { h = (h ^ (v ? 1ULL : 0ULL)) * kFnvPrime; }
    void s(const std::string& v) noexcept { u(v.size()); bytes(v.data(), v.size()); }
};

// R5 lane PERF-P1: the sink a continuation view is recorded into
// (NativeExecutionConsumer::capture_continuation_view). The same primitives
// as Fnv, appending the bytes Fnv would fold -- in the order bytes() reads
// them: a scalar's eight object bytes, a bool's one -- instead of folding
// them. It is a type of its own, and the hash helpers below take either sink,
// so the eager fold every other caller runs is compiled exactly as it was. A
// buffer that cannot grow latches `failed`, and the capture folds at once.
struct FnvRecord {
    std::vector<unsigned char>& out;
    uint64_t run_base = 0;
    bool failed = false;
    void bytes(const void* p, size_t n) noexcept {
        if (failed) return;
        if (out.capacity() - out.size() < n && !reserve_view_bytes(out, n)) {
            failed = true;
            return;
        }
        const auto* c = static_cast<const unsigned char*>(p);
        out.insert(out.end(), c, c + n);
    }
    void scalar(uint64_t bits) noexcept { bytes(&bits, sizeof bits); }
    void u(uint64_t v) noexcept { scalar(v); }
    void i(int64_t v) noexcept {
        uint64_t bits = 0; std::memcpy(&bits, &v, sizeof bits); scalar(bits);
    }
    void d(double v) noexcept {
        uint64_t bits = 0; std::memcpy(&bits, &v, sizeof bits); scalar(bits);
    }
    void b(bool v) noexcept { const unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void s(const std::string& v) noexcept { u(v.size()); bytes(v.data(), v.size()); }
};

// Folds recorded bytes exactly as bytes() would, eight at a time through
// scalar(), which reads a word's object bytes in memory order and collapses
// its zero runs: a view's replay costs what the fold it recorded would have.
void fold_recorded(Fnv& f, const unsigned char* p, size_t n) noexcept {
    for (; n >= sizeof(uint64_t); p += sizeof(uint64_t), n -= sizeof(uint64_t)) {
        uint64_t word = 0;
        std::memcpy(&word, p, sizeof word);
        f.scalar(word);
    }
    f.bytes(p, n);
}

template <class F>
void hash_coordinate(F& f, const NativeCoordinate& c) noexcept;
template <class F>
void hash_birth(F& f, const native_order::Birth& birth) noexcept;
template <class F>
void hash_optional_handle(F& f, const std::optional<native_order::RequestHandle>& handle) noexcept;

// L8 price grid. Generic hosts opt in through the run spec; the kernel
// otherwise keeps price_tick a pure slippage multiplier, so a spec that
// leaves price_grid at None books exactly the prices it books today.
bool price_grid_on(const NativeRunSpec& spec) noexcept {
    return spec.price_grid != NativePriceGrid::None;
}

// The pre-slippage fill basis. Directional rounds toward the region a resting
// order needs: a limit toward its own favorable side, a stop or a market fill
// toward the adverse one.
double grid_fill_basis(const NativeRunSpec& spec, double price, bool buy,
                       bool limit_governed) noexcept {
    if (!price_grid_on(spec)) return price;
    if (spec.grid_rounding == NativeGridRounding::Directional) {
        return native_matching::grid_round_directional(
            price, spec.price_tick, limit_governed ? !buy : buy);
    }
    return native_matching::grid_round_half_up(price, spec.price_tick);
}

// Limit-or-better survives the grid: the protection cap moves to the tick on
// the order's own side, never past its level. A level that already is a
// ladder point is its own cap (grid_round_directional is a fixed point there,
// R7): the half-up basis spells that point as k * tick, one ULP past a
// decimal literal, and the cap clamps it back onto the level instead of
// booking a fill the terms check below then refuses.
double grid_limit_cap(const NativeRunSpec& spec, double level, bool buy) noexcept {
    if (!price_grid_on(spec)) return level;
    return native_matching::grid_round_directional(level, spec.price_tick, !buy);
}

// Only the second mode tests triggers against the quantized path; an
// inactive threshold is the matcher's existing raw-level arithmetic.
native_matching::GridThreshold grid_threshold(const NativeRunSpec& spec) noexcept {
    native_matching::GridThreshold grid;
    if (spec.price_grid != NativePriceGrid::QuantizeFillsAndTriggers) return grid;
    grid.tick = spec.price_tick;
    grid.half_up = spec.grid_rounding == NativeGridRounding::HalfUp;
    return grid;
}

// The same facts for the core's activation re-validation (L8b): the matcher
// tests a trigger on this ladder, so the core must judge the reached print on
// it too. Inactive (the raw rule) in every mode but the second.
native_order::ActivationGrid activation_grid(const NativeRunSpec& spec) noexcept {
    const auto threshold = grid_threshold(spec);
    native_order::ActivationGrid grid;
    grid.price_tick = threshold.tick;
    grid.half_up = threshold.half_up;
    // The declared ladder is a separate fact from the quantization mode: a
    // trailing stop spelled a whole number of ticks from a best on it names
    // that ladder point under every mode (R5 lane E16).
    grid.ladder_tick = spec.price_tick;
    return grid;
}

// L5 calculation timing. A spec that leaves both the trigger and the
// open-bar view at their defaults is exactly the pre-lane surface: no
// recalculation is driven, no bar is masked, and nothing of this block is
// folded into the continuation digest.
bool calc_timing_on(const NativeRunSpec& spec) noexcept {
    return spec.calculation != NativeCalculationTrigger::BarClose
        || spec.open_bar_view != NativeOpenBarView::Complete;
}

// `cached`, when given, carries the three bar-array digests this fold takes,
// already taken of this same spec (NativeExecutionConsumer::spec_bar_digests).
// The three local digests below answer them instead of re-walking the arrays,
// so the fold itself is written, and folds, exactly as without a cache.
template <class F>
void hash_spec(F& f, const NativeRunSpec& spec,
               const NativeExecutionConsumer::SpecBarDigests* cached = nullptr) noexcept {
    const auto native_intrabar_path_digest = [cached](const IntrabarPath& path) noexcept {
        return cached ? cached->intrabar : pineforge::native_intrabar_path_digest(path);
    };
    const auto native_timeframe_subscriptions_digest =
        [cached](const std::vector<NativeTimeframeSubscription>& series) noexcept {
            return cached ? cached->subscriptions
                          : pineforge::native_timeframe_subscriptions_digest(series);
        };
    const auto native_auxiliary_feed_digest =
        [cached](const NativeAuxiliaryFeed& feed) noexcept {
            return cached ? cached->auxiliary : pineforge::native_auxiliary_feed_digest(feed);
        };
    f.s(spec.identity.session_key); f.u(spec.identity.run_number - f.run_base);
    f.s(spec.input_tf); f.s(spec.script_tf);
    f.b(spec.timeframe_undetected);
    f.u(static_cast<uint64_t>(spec.slot_label_policy));
    f.u(static_cast<uint64_t>(spec.legacy_tolerance));
    f.u(static_cast<uint64_t>(spec.path_order));
    f.s(spec.ticker); f.s(spec.tickerid); f.s(spec.type);
    f.s(spec.currency); f.s(spec.basecurrency); f.s(spec.description); f.s(spec.volumetype);
    f.s(spec.timezone); f.s(spec.session); f.s(spec.chart_timezone);
    f.d(spec.initial_capital); f.d(spec.point_value); f.d(spec.account_fx); f.d(spec.price_tick);
    f.u(spec.slippage_ticks); f.u(static_cast<uint64_t>(spec.fee_kind)); f.d(spec.fee_value);
    f.b(spec.quantity_grid.has_value()); if (spec.quantity_grid) f.d(*spec.quantity_grid);
    f.u(static_cast<uint64_t>(spec.close_execution));
    f.u(static_cast<uint64_t>(spec.abort_reporting));
    f.b(spec.max_abs_units.has_value()); if (spec.max_abs_units) f.d(*spec.max_abs_units);
    f.b(spec.max_open_lots.has_value()); if (spec.max_open_lots) f.u(*spec.max_open_lots);
    f.u(static_cast<uint64_t>(spec.allowed_open_directions));
    f.b(spec.initial_margin_fraction.has_value());
    if (spec.initial_margin_fraction) f.d(*spec.initial_margin_fraction);
    f.u(native_intrabar_path_digest(spec.intrabar));
    // Report recording folds only when the consumer records of its own
    // initiative: a spec that leaves the kernel out of its report keeps the
    // continuation identity it had before the policy existed (same
    // conditional shape as the precommit digest below).  A host-marked report
    // (KernelRecordedAtHostMarks) is the same case: the consumer decides
    // nothing and does nothing between two points of the run, so it folds
    // nothing, exactly as HostRecorded folds nothing.
    if (spec.report_policy == NativeReportPolicy::KernelRecorded) {
        f.u(static_cast<uint64_t>(spec.report_policy));
        f.b(spec.report_open_position_at_end);
    }
    // A38/L8: the price grid is folded only where a host actually opted in.
    // A defaulted grid folds nothing, so every established continuation hash
    // survives this spec extension unchanged.
    if (price_grid_on(spec)) {
        f.u(static_cast<uint64_t>(spec.price_grid));
        f.u(static_cast<uint64_t>(spec.grid_rounding));
    }
    // Declared higher-timeframe series fold only when there are any, so a
    // spec that declares none keeps its pre-subscription continuation
    // identity byte for byte (the precommit_digest_ precedent).
    if (!spec.subscriptions.empty()) {
        f.u(native_timeframe_subscriptions_digest(spec.subscriptions));
    }
    // The auxiliary finer feed folds only where a host declared one, so a
    // spec that declares none keeps its pre-feed continuation identity.
    if (spec.auxiliary_feed) {
        f.u(native_auxiliary_feed_digest(*spec.auxiliary_feed));
    }
    // L4: the generic margin model folds only where a host declared one. An
    // absent model folds nothing, so every continuation hash established
    // before it existed survives this spec extension unchanged.
    if (spec.margin) {
        f.u(native_margin_model_digest(*spec.margin));
    }
    // L9: the generic risk limits fold only where a host declared them. An
    // absent block folds nothing, so every continuation hash established
    // before it existed survives this spec extension unchanged.
    if (spec.risk) {
        f.u(native_risk_limits_digest(*spec.risk));
    }
    // L5: the calculation cadence folds only where a host actually moved it
    // off BarClose/Complete. A defaulted cadence folds nothing, including its
    // inert recalculation bound, so every established continuation hash
    // survives this spec extension unchanged.
    if (calc_timing_on(spec)) {
        f.u(static_cast<uint64_t>(spec.calculation));
        f.u(spec.max_recalculations_per_point);
        f.u(static_cast<uint64_t>(spec.open_bar_view));
    }
}

template <class F>
void hash_handle(F& f, const native_order::RequestHandle& handle) noexcept {
    f.s(handle.run.session_key);
    f.u(handle.run.run_number - f.run_base);
    f.u(handle.incarnation);
}

template <class F>
void hash_cohort_handle(F& f, native_order::CohortHandle handle) noexcept {
    f.u(handle.value);
}

template <class F>
void hash_event_id(F& f, const native_order::EventId& id) noexcept {
    f.s(id.run.session_key);
    f.u(id.run.run_number - f.run_base);
    f.u(id.ordinal);
}

template <class F>
void hash_cursor(F& f, const native_order::MatchCursor& cursor) noexcept {
    hash_coordinate(f, cursor.point);
    f.d(cursor.t);
}

template <class F>
void hash_surface(F& f, native_order::CommandSurface surface) noexcept {
    f.u(static_cast<uint64_t>(surface));
}

template <class F>
void hash_intent(F& f, const native_order::OrderIntent& intent) noexcept {
    f.u(intent.index());
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, native_order::Flatten>) {
        } else if constexpr (std::is_same_v<T, native_order::Reduce>) {
            f.u(payload.size.index());
            if (const auto* units = std::get_if<native_order::ExplicitUnits>(&payload.size)) {
                f.d(units->units);
            } else if (const auto* fraction =
                           std::get_if<native_order::ScopeFraction>(&payload.size)) {
                f.d(fraction->fraction);
                f.u(static_cast<uint64_t>(fraction->claim));
                // L3b: the scope basis folds only where a caller moved it off
                // AtMatch, so every established fraction hash survives.
                if (fraction->basis != native_order::ScopeBasis::AtMatch) {
                    f.u(static_cast<uint64_t>(fraction->basis));
                }
            }
        } else if constexpr (std::is_same_v<T, native_order::Transact>) {
            f.d(payload.signed_units);
        } else if constexpr (std::is_same_v<T, native_order::ReverseTo>) {
            f.d(payload.signed_units);
        } else if constexpr (std::is_same_v<T, native_order::HostSized>) {
            f.u(static_cast<uint64_t>(payload.kind));
            f.b(payload.side.has_value());
            if (payload.side) f.u(static_cast<uint64_t>(*payload.side));
        } else if constexpr (std::is_same_v<T, native_order::Sized>) {
            f.u(static_cast<uint64_t>(payload.side));
            f.u(payload.basis.index());
            if (const auto* cash = std::get_if<native_order::CashValue>(&payload.basis)) {
                f.d(cash->cash);
            } else {
                f.d(std::get<native_order::EquityFraction>(payload.basis).fraction);
            }
            f.u(static_cast<uint64_t>(payload.time));
            f.u(static_cast<uint64_t>(payload.grid_policy));
            f.b(payload.reserve_percent_fee);
            // L3b: the sizing-price rule folds only where a caller moved it
            // off Resolved, so every established Sized hash survives.
            if (payload.price != native_order::SizePrice::Resolved) {
                f.u(static_cast<uint64_t>(payload.price));
            }
        } else {
            static_assert(!sizeof(T), "unhashed native order intent");
        }
    }, intent);
}

template <class F>
void hash_execution_terms(F& f, const native_order::ExecutionTerms& terms) noexcept {
    f.d(terms.resolved_price);
    f.b(terms.units.has_value());
    if (terms.units) f.d(*terms.units);
    f.u(static_cast<uint64_t>(terms.shape));
    f.u(static_cast<uint64_t>(terms.grid_policy));
}

template <class F>
void hash_optional_execution_terms(F& f,
                                   const std::optional<native_order::ExecutionTerms>& terms) noexcept {
    f.b(terms.has_value());
    if (terms) hash_execution_terms(f, *terms);
}

bool same_double_bits(double left, double right) noexcept {
    return native_matching::double_bits(left) == native_matching::double_bits(right);
}

bool identity_terms(const native_order::ExecutionTerms& terms, double default_price) noexcept {
    return same_double_bits(terms.resolved_price, default_price)
        && !terms.units.has_value()
        && terms.shape == native_order::OpeningShape::Transact
        && terms.grid_policy == native_order::ExecutionGridPolicy::SnapToGrid;
}

bool valid_execution_grid_policy(native_order::ExecutionGridPolicy policy) noexcept {
    switch (policy) {
    case native_order::ExecutionGridPolicy::SnapToGrid:
    case native_order::ExecutionGridPolicy::ExplicitUnits:
        return true;
    }
    return false;
}

bool explicit_reduction_units_representable(double units, double exposure) noexcept {
    if (!std::isfinite(units) || !std::isfinite(exposure)
        || !(units > 0.0) || !(exposure > 0.0) || units > exposure) {
        return false;
    }
    return order_action::plan(exposure, order_action::Reduce{units}).has_value();
}

bool execution_terms_grid_representable(
        const native_order::ExecutionTerms& terms,
        const native_order::HostSized* host_sized, bool unresolved,
        double scope_exposure_units, const NativeRunSpec* spec) noexcept {
    if (!valid_execution_grid_policy(terms.grid_policy)) return false;
    if (terms.grid_policy == native_order::ExecutionGridPolicy::ExplicitUnits) {
        return unresolved && host_sized
            && host_sized->kind == native_order::HostSizedKind::Close
            && terms.shape == native_order::OpeningShape::Transact
            && terms.units
            && explicit_reduction_units_representable(
                *terms.units, scope_exposure_units);
    }
    if (!terms.units || !(*terms.units > 0.0)) return true;
    return spec && (!spec->quantity_grid
        || native_order::quantity_on_grid(*terms.units, *spec->quantity_grid));
}

bool path_uses_high_first(const Bar& bar, NativePathOrder order) noexcept {
    switch (order) {
    case NativePathOrder::HighFirst:
        return true;
    case NativePathOrder::LowFirst:
        return false;
    case NativePathOrder::Auto:
        return std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
    }
    return false;
}

class NativePathOrderScope {
public:
    explicit NativePathOrderScope(NativePathOrder order)
        : prior_(internal::path_order_override()) {
        const int mode = order == NativePathOrder::HighFirst ? 1
            : (order == NativePathOrder::LowFirst ? 2 : 0);
        internal::set_path_order_override(mode);
    }
    ~NativePathOrderScope() { internal::set_path_order_override(prior_); }

    NativePathOrderScope(const NativePathOrderScope&) = delete;
    NativePathOrderScope& operator=(const NativePathOrderScope&) = delete;

private:
    int prior_ = 0;
};

bool remaining_path_coordinate(const NativeCoordinate& coordinate) noexcept {
    const bool continuous_provenance = coordinate.provenance == NativePriceProvenance::Confirmed
        || coordinate.provenance == NativePriceProvenance::ModeledOHLCClose;
    return continuous_provenance
        && coordinate.path_phase != NativePathPhase::None
        && coordinate.path_phase != NativePathPhase::Open;
}

double allowance_left_at(const native_order::Allowance& allowance, uint64_t point) noexcept {
    if (const auto* units = std::get_if<native_order::AllowanceUnits>(&allowance)) {
        return units->point_ordinal == point ? units->left : 0.0;
    }
    if (const auto* all = std::get_if<native_order::AllowanceAllScope>(&allowance)) {
        return all->point_ordinal == point ? std::numeric_limits<double>::infinity() : 0.0;
    }
    return 0.0;
}

// The live Remaining alternatives map one-to-one onto their projection
// spelling, which is what leaves the engine in a receipt or a snapshot.
native_order::RemainingProjection project_live_remaining(
        const native_order::Remaining& remaining) noexcept {
    if (std::holds_alternative<native_order::RemainingFlattenAll>(remaining)) {
        return native_order::RemainingProjectionFlattenAll{};
    }
    if (const auto* units = std::get_if<native_order::RemainingUnits>(&remaining)) {
        return native_order::RemainingProjectionUnits{units->q};
    }
    if (std::holds_alternative<native_order::RemainingDeferred>(remaining)) {
        return native_order::RemainingProjectionDeferred{};
    }
    if (std::holds_alternative<native_order::NoTarget>(remaining)) {
        return native_order::RemainingProjectionNoTarget{};
    }
    return native_order::RemainingProjectionUnbound{};
}

bool same_allowance_bits(const native_order::Allowance& left,
                         const native_order::Allowance& right) noexcept {
    if (left.index() != right.index()) return false;
    if (const auto* a = std::get_if<native_order::AllowanceUnits>(&left)) {
        const auto& b = std::get<native_order::AllowanceUnits>(right);
        return a->point_ordinal == b.point_ordinal
            && same_double_bits(a->initial, b.initial)
            && same_double_bits(a->left, b.left);
    }
    if (const auto* a = std::get_if<native_order::AllowanceAllScope>(&left)) {
        return a->point_ordinal
            == std::get<native_order::AllowanceAllScope>(right).point_ordinal;
    }
    if (const auto* a = std::get_if<native_order::AllowanceDeferred>(&left)) {
        return a->point_ordinal
            == std::get<native_order::AllowanceDeferred>(right).point_ordinal;
    }
    return true;
}

// L3 sizing bases. The quantity grid is a floor in the same kind as the money
// floor a source layer applies to its own sizing quotient
// (pine_adapter.cpp source_money_floor_lot): the LARGEST grid multiple that is
// less than or equal to the quotient, never one above it. A basis that does
// not buy one whole step is not representable.
//
// The only tolerance is the engine's existing on-grid predicate
// (native_order.hpp quantity_on_grid, ~4 ulp and strictly inside half a step):
// a quotient already on the grid keeps its own binary64 representation rather
// than being rebuilt one ulp away (engine.hpp apply_exit_qty_step). A
// proportional epsilon is deliberately NOT applied here. floor(u/step + 1e-6)
// can land ABOVE the quotient — 2.9999995 on a one-unit grid became 3 — which
// the old `floored < units` guard then turned back into the raw quotient that
// representable_units refused as off-grid, so a perfectly fundable basis
// booked nothing at all instead of the two units it can afford.
//
// n is corrected in both directions because one divide plus one multiply can
// land on either side of the exact quotient by an ulp or two.
double floor_to_quantity_grid(double units, double step) noexcept {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!std::isfinite(step) || step <= 0.0) return units;
    if (native_order::quantity_on_grid(units, step)) return units;
    double n = std::floor(units / step);
    if (!std::isfinite(n)) return 0.0;
    for (int guard = 0; guard < 4 && std::isfinite((n + 1.0) * step)
                        && (n + 1.0) * step <= units; ++guard) {
        n += 1.0;
    }
    for (int guard = 0; guard < 4 && n >= 1.0 && n * step > units; ++guard) n -= 1.0;
    if (!(n >= 1.0)) return 0.0;
    const double floored = n * step;
    if (!std::isfinite(floored) || floored <= 0.0 || floored > units) return 0.0;
    return floored;
}

std::optional<double> representable_units(double units,
                                          native_order::ExecutionGridPolicy policy,
                                          const std::optional<double>& grid) noexcept {
    if (!std::isfinite(units) || units <= 0.0) return std::nullopt;
    if (policy != native_order::ExecutionGridPolicy::SnapToGrid || !grid) return units;
    const double snapped = floor_to_quantity_grid(units, *grid);
    if (!(snapped > 0.0) || !native_order::quantity_on_grid(snapped, *grid)) return std::nullopt;
    return snapped;
}

// units = cash / (price * point_value * fx); cash is the basis value or
// fraction * marked equity at the sizing point, optionally net of a percent
// fee reserve.
//
// NativeRunSpec::fee_value is a PERCENT for NativeFeeKind::Percent
// (native_run_spec.hpp:560): the charge is fee_value / 100 of the account
// notional (engine.hpp calc_commission). The reserve is the exact inverse of
// that charge, so it divides by 1 + fee_value / 100 and a 0.1 % fee reserves
// 0.1 %, not 10 %.
std::optional<double> sized_basis_units(const native_order::Sized& sized, double price,
                                        double equity, double fx,
                                        const NativeRunSpec& spec) noexcept {
    double cash = 0.0;
    if (const auto* value = std::get_if<native_order::CashValue>(&sized.basis)) {
        cash = value->cash;
    } else {
        cash = std::get<native_order::EquityFraction>(sized.basis).fraction * equity;
    }
    if (sized.reserve_percent_fee && spec.fee_kind == NativeFeeKind::Percent) {
        const double divisor = 1.0 + spec.fee_value / 100.0;
        if (!std::isfinite(divisor) || divisor <= 0.0) return std::nullopt;
        cash /= divisor;
    }
    const double denominator = price * spec.point_value * fx;
    if (!std::isfinite(cash) || !std::isfinite(denominator) || denominator <= 0.0) {
        return std::nullopt;
    }
    return representable_units(cash / denominator, sized.grid_policy, spec.quantity_grid);
}

const native_order::Sized* sized_intent(const native_order::LiveRequest& live) noexcept {
    return std::get_if<native_order::Sized>(&live.request().intent);
}

const native_order::ScopeFraction* scope_fraction_intent(
        const native_order::LiveRequest& live) noexcept {
    const auto* reduce = std::get_if<native_order::Reduce>(&live.request().intent);
    return reduce ? std::get_if<native_order::ScopeFraction>(&reduce->size) : nullptr;
}

// Two reduces share a scope when their authorities name the same physical
// target. The keys are opaque request/cohort handles and position cycles,
// never source identifiers. An unbound book close already names the book.
bool same_reduction_scope(const native_order::Authority& left,
                          const native_order::Authority& right) noexcept {
    const auto book = [](const native_order::Authority& value) {
        return std::holds_alternative<native_order::UnboundBookClose>(value)
            || std::holds_alternative<native_order::BookClose>(value);
    };
    if (book(left) || book(right)) {
        if (!book(left) || !book(right)) return false;
        const auto* a = std::get_if<native_order::BookClose>(&left);
        const auto* b = std::get_if<native_order::BookClose>(&right);
        if (a && b) return a->cycle == b->cycle && a->side == b->side;
        return true;
    }
    if (left.index() != right.index()) return false;
    if (const auto* a = std::get_if<native_order::OpeningClose>(&left)) {
        const auto& b = std::get<native_order::OpeningClose>(right);
        return a->opening == b.opening && a->cycle == b.cycle;
    }
    if (const auto* a = std::get_if<native_order::OpeningsClose>(&left)) {
        const auto& b = std::get<native_order::OpeningsClose>(right);
        return a->cycle == b.cycle && a->openings == b.openings;
    }
    if (const auto* a = std::get_if<native_order::CohortClose>(&left)) {
        return a->cohort == std::get<native_order::CohortClose>(right).cohort;
    }
    return false;
}

const native_order::HostSized* host_sized_intent(
        const native_order::LiveRequest& live) noexcept {
    return std::get_if<native_order::HostSized>(&live.request().intent);
}

template <class F>
void hash_terms_input(F& f, const native_order::TermsResolvedInput& input) noexcept {
    f.u(static_cast<uint64_t>(input.price_kind));
    f.b(input.shared_cursor_collision);
    f.d(input.raw_price);
    f.d(input.default_resolved_price);
    hash_execution_terms(f, input.terms);
}

template <class F>
void hash_trigger(F& f, const native_order::Trigger& trigger) noexcept {
    f.u(trigger.index());
    if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
        f.d(limit->price);
        // Folded only when set, so every bounded limit keeps its prior digest.
        if (limit->fill_through) f.b(true);
    }
    if (const auto* stop = std::get_if<native_order::Stop>(&trigger)) f.d(stop->price);
    if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) {
        f.d(sl->stop);
        f.d(sl->limit);
    }
    if (const auto* trail = std::get_if<native_order::Trail>(&trigger)) {
        f.d(trail->offset);
        f.b(trail->arm_price.has_value());
        if (trail->arm_price) f.d(*trail->arm_price);
        // Folded only when the tick spelling is still present, so every
        // price-spelled trail keeps its prior digest. Acceptance resolves the
        // spelling away, so only an attempted request can carry one.
        if (trail->ticks) f.d(trail->ticks->ticks);
        // Likewise the seeded start of the running best, under its own tag so
        // it can never read as an arm price: every trail without a seed keeps
        // the digest it had before the field existed.
        if (trail->best_seed) {
            f.s("best_seed");
            f.d(*trail->best_seed);
        }
    }
}

template <class F>
void hash_capacity(F& f, const native_order::Capacity& capacity) noexcept {
    f.u(capacity.index());
    if (const auto* budget = std::get_if<native_order::PointBudget>(&capacity)) f.d(budget->units);
}

template <class F>
void hash_owner(F& f, const native_order::Owner& owner) noexcept {
    f.u(owner.index());
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, native_order::Independent>) {
        } else if constexpr (std::is_same_v<T, native_order::WaitForApplied>) {
            hash_handle(f, value.parent);
            // Visibility is request state: folded only when set, so every
            // Working child keeps the digest it had before the knob existed.
            if (value.visibility != native_order::NativeArmVisibility::Working) {
                f.u(static_cast<uint64_t>(value.visibility));
            }
            // Likewise the first-match rule, under its own tag so it can
            // never read as a visibility: every AtArmPrint child keeps the
            // digest it had before the knob existed.
            if (value.first_match != native_order::NativeArmFirstMatch::AtArmPrint) {
                f.s("first_match");
                f.u(static_cast<uint64_t>(value.first_match));
            }
            if (value.scope != native_order::NativeArmScope::OwnerLot) {
                f.s("scope");
                f.u(static_cast<uint64_t>(value.scope));
            }
        } else if constexpr (std::is_same_v<T, native_order::BindOpening>) {
            hash_handle(f, value.opening);
            f.i(value.cycle);
        } else if constexpr (std::is_same_v<T, native_order::BindOpenings>) {
            f.i(value.cycle); f.u(value.openings.size());
            for (const auto& handle : value.openings) hash_handle(f, handle);
        } else if constexpr (std::is_same_v<T, native_order::BindCohort>) {
            hash_cohort_handle(f, value.cohort);
        } else {
            static_assert(!sizeof(T), "unhashed native owner");
        }
    }, owner);
}

template <class F>
void hash_group(F& f, const native_order::Group& group) noexcept {
    f.u(group.index());
    if (const auto* member = std::get_if<native_order::Member>(&group)) {
        f.u(member->group);
        f.i(member->cohort);
        f.u(static_cast<uint64_t>(member->effect));
    }
}

template <class F>
void hash_request(F& f, const native_order::Request& request) noexcept {
    hash_intent(f, request.intent);
    f.s(request.label);
    f.s(request.comment);
    hash_trigger(f, request.trigger);
    hash_capacity(f, request.capacity);
    hash_owner(f, request.owner);
    hash_group(f, request.group);
    // Folded only when the request is anchored, so every absolute request
    // keeps its prior digest; the rounding folds only when set, so every
    // Raw anchor keeps the digest it had before the rounding existed.
    if (const auto* anchor = std::get_if<native_order::FromOwnerFill>(&request.anchor)) {
        f.d(anchor->offset);
        f.b(anchor->ticks);
        if (anchor->rounding != native_order::NativeAnchorRounding::Raw) {
            f.u(static_cast<uint64_t>(anchor->rounding));
        }
    }
}

template <class F>
void hash_remaining(F& f, const native_order::Remaining& remaining) noexcept {
    f.u(remaining.index());
    if (const auto* units = std::get_if<native_order::RemainingUnits>(&remaining)) f.d(units->q);
}

template <class F>
void hash_remaining_projection(F& f, const native_order::RemainingProjection& remaining) noexcept {
    f.u(remaining.index());
    if (const auto* units = std::get_if<native_order::RemainingProjectionUnits>(&remaining)) {
        f.d(units->q);
    }
}

template <class F>
void hash_authority(F& f, const native_order::Authority& authority) noexcept {
    f.u(authority.index());
    const auto hash_enrollment = [&](const native_order::Enrollment& enrollment) {
        f.u(enrollment.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, native_order::EnrollmentFromCommand>) {
                hash_event_id(f, value.accepted);
            } else if constexpr (std::is_same_v<T, native_order::EnrollmentFromApplied>) {
                hash_event_id(f, value.cause);
                hash_cursor(f, value.cursor);
            } else {
                static_assert(!sizeof(T), "unhashed native enrollment");
            }
        }, enrollment);
    };
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, native_order::BookTransaction>) {
        } else if constexpr (std::is_same_v<T, native_order::Wait>) {
            hash_handle(f, value.parent);
        } else if constexpr (std::is_same_v<T, native_order::ArmedTransaction>) {
            hash_handle(f, value.parent);
            hash_event_id(f, value.cause);
            hash_cursor(f, value.cause_cursor);
        } else if constexpr (std::is_same_v<T, native_order::UnboundBookClose>) {
        } else if constexpr (std::is_same_v<T, native_order::BookClose>) {
            f.i(value.cycle);
            f.u(static_cast<uint64_t>(value.side));
            hash_event_id(f, value.binding_event);
            hash_cursor(f, value.binding_cursor);
        } else if constexpr (std::is_same_v<T, native_order::OpeningClose>) {
            hash_handle(f, value.opening);
            f.i(value.cycle);
            f.u(static_cast<uint64_t>(value.side));
            hash_enrollment(value.enrollment);
        } else if constexpr (std::is_same_v<T, native_order::OpeningsClose>) {
            f.i(value.cycle); f.u(static_cast<uint64_t>(value.side));
            f.u(value.openings.size());
            for (const auto& handle : value.openings) hash_handle(f, handle);
            hash_enrollment(value.enrollment);
        } else if constexpr (std::is_same_v<T, native_order::CohortClose>) {
            hash_cohort_handle(f, value.cohort);
        } else {
            static_assert(!sizeof(T), "unhashed native authority");
        }
    }, authority);
}

template <class F>
void hash_cohorts(F& f, const native_order::WorkingRequestCore& requests) noexcept {
    const auto& cohorts = requests.cohorts();
    f.u(cohorts.size());
    for (const auto& roster : cohorts) {
        hash_cohort_handle(f, roster.handle);
        f.u(roster.origins.size());
        for (const auto& origin : roster.origins) hash_handle(f, origin);
    }
    const auto& receipts = requests.cohort_receipts();
    f.u(receipts.size());
    for (const auto& receipt : receipts) {
        f.u(static_cast<std::uint64_t>(receipt.operation));
        f.u(static_cast<std::uint64_t>(receipt.status));
        hash_cohort_handle(f, receipt.cohort);
        hash_handle(f, receipt.origin);
    }
}

template <class F>
void hash_trigger_state(F& f, const native_order::TriggerState& state) noexcept {
    f.u(state.index());
    if (const auto* track = std::get_if<native_order::TrailTrack>(&state)) f.d(track->best);
    if (const auto* active = std::get_if<native_order::TrailActive>(&state)) {
        f.d(active->best_at_trigger);
    }
}

template <class F>
void hash_allowance(F& f, const native_order::Allowance& allowance) noexcept {
    f.u(allowance.index());
    if (const auto* units = std::get_if<native_order::AllowanceUnits>(&allowance)) {
        f.u(units->point_ordinal);
        f.d(units->initial);
        f.d(units->left);
    }
    if (const auto* all = std::get_if<native_order::AllowanceAllScope>(&allowance)) {
        f.u(all->point_ordinal);
    }
    if (const auto* deferred = std::get_if<native_order::AllowanceDeferred>(&allowance)) {
        f.u(deferred->point_ordinal);
    }
}

template <class F>
void hash_pending(F& f, const native_order::PendingAdjustments& pending) noexcept {
    f.u(pending.index());
    if (const auto* deferred = std::get_if<native_order::PendingDeferred>(&pending)) {
        f.d(deferred->total);
        f.u(deferred->count);
        hash_event_id(f, deferred->tail_receipt);
    }
}

template <class F>
void hash_definition(F& f, const native_order::DefinitionRef& definition) noexcept {
    f.b(static_cast<bool>(definition));
    if (!definition) return;
    hash_handle(f, definition->handle);
    hash_request(f, definition->request);
    hash_birth(f, definition->birth);
    hash_optional_handle(f, definition->predecessor);
    // Every host request is RequestOrigin::Host, so the authorship of the
    // established population folds nothing. Only a kernel-originated request
    // moves the digest, and only a run with a margin model has one.
    if (definition->origin != native_order::RequestOrigin::Host) {
        f.u(static_cast<uint64_t>(definition->origin));
    }
}

template <class F>
void hash_scope(F& f, const native_order::ExecutionScope& scope) noexcept {
    f.u(scope.index());
    if (const auto* opening = std::get_if<execution::OpeningExposure>(&scope)) {
        f.u(opening->incarnation);
        f.i(opening->cycle);
    }
    if (const auto* selected = std::get_if<native_order::SelectedExposure>(&scope)) {
        f.i(selected->cycle); f.u(selected->incarnations.size());
        for (auto incarnation : selected->incarnations) f.u(incarnation);
    }
}

template <class F>
void hash_birth(F& f, const native_order::Birth& birth) noexcept {
    f.u(birth.acceptance_ordinal);
    f.i(birth.decision_time_lower_bound);
}

template <class F>
void hash_optional_request(F& f, const std::optional<native_order::Request>& request) noexcept {
    f.b(request.has_value());
    if (request) hash_request(f, *request);
}

template <class F>
void hash_optional_handle(F& f, const std::optional<native_order::RequestHandle>& handle) noexcept {
    f.b(handle.has_value());
    if (handle) hash_handle(f, *handle);
}

template <class F>
void hash_failure(F& f, const NativeFailure& failure) noexcept {
    f.u(static_cast<uint64_t>(failure.code));
    f.u(static_cast<uint64_t>(failure.operation));
    f.u(failure.ordinal);
    f.u(failure.discriminator);
    f.u(static_cast<uint64_t>(failure.context.kind));
    f.u(failure.context.cause.ordinal);
    f.u(failure.context.recipient.incarnation);
    hash_coordinate(f, failure.context.cursor.point);
    f.d(failure.context.cursor.t);
}

template <class F>
void hash_coordinate(F& f, const NativeCoordinate& c) noexcept {
    f.u(c.ordinal);
    f.i(c.interval_index);
    f.i(c.open_ms);
    f.i(c.eligible_open_ms);
    f.i(c.last_traded_close_ms);
    f.i(c.next_period_open_ms);
    f.i(c.next_input_open_ms);
    f.i(c.effective_time_ms);
    f.i(c.source_price_time_ms);
    f.u(static_cast<uint64_t>(c.provenance));
    f.u(static_cast<uint64_t>(c.path_phase));
    f.u(static_cast<uint64_t>(c.completion));
}

template <class F>
void hash_interval(F& f, const native_calendar::NativeInterval& interval) noexcept {
    f.i(interval.open_ms);
    f.i(interval.eligible_open_ms);
    f.i(interval.last_traded_close_ms);
    f.i(interval.next_period_open_ms);
    f.i(interval.next_input_open_ms);
}

template <class F>
void hash_driver_statistics(F& f, const NativeDriverStatistics& statistics) noexcept {
    f.b(statistics.intrabar_path_enabled);
    f.i(statistics.sub_bars_per_script_bar);
    f.i(statistics.samples_per_sub_bar);
    f.u(statistics.sub_bars_processed);
    f.u(statistics.sample_ticks_processed);
}

template <class F>
void hash_current_point(F& f, const NativeCurrentPointView& point) noexcept {
    hash_coordinate(f, point.decision.coordinate);
    f.i(point.decision.decision_floor_ms);
    hash_interval(f, point.decision.input_interval);
    hash_interval(f, point.decision.script_interval);
    f.i(point.decision.sub_index);
    f.i(point.decision.sub_count);
    f.b(point.decision.is_terminal_sub_bar);
    f.i(point.decision.sub_bar_open_ms);
    f.i(point.decision.script_bar_open_ms);
    hash_driver_statistics(f, point.decision.driver_statistics);
    f.d(point.price);
    f.u(static_cast<uint64_t>(point.quote_kind));
    f.u(point.quote_origin_ordinal);
}

template <class F>
void hash_input_context(F& f, const NativeInputContext& context) noexcept {
    hash_interval(f, context.input_interval);
    hash_interval(f, context.script_interval);
    f.i(context.input_index);
    f.b(context.completes_script_interval);
}

template <class F>
void hash_tick_context(F& f, const NativeTickContext& context) noexcept {
    hash_coordinate(f, context.decision.coordinate);
    f.i(context.decision.decision_floor_ms);
    hash_interval(f, context.decision.input_interval);
    hash_interval(f, context.decision.script_interval);
    f.i(context.decision.sub_index);
    f.i(context.decision.sub_count);
    f.b(context.decision.is_terminal_sub_bar);
    f.i(context.decision.sub_bar_open_ms);
    f.i(context.decision.script_bar_open_ms);
    hash_driver_statistics(f, context.decision.driver_statistics);
    f.u(context.sequence);
}

template <class F>
void hash_bar(F& f, const Bar& bar) noexcept {
    f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close); f.d(bar.volume);
    f.i(bar.timestamp);
}

template <class F>
void hash_command(F& f, const native_order::CommandEvent& event) noexcept {
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        f.u(payload.ordinal);
        if constexpr (std::is_same_v<T, native_order::AcceptedEvent>) {
            f.u(1);
            hash_definition(f, payload.definition);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::RejectedEvent>) {
            f.u(2);
            hash_request(f, payload.request);
            f.u(static_cast<uint64_t>(payload.reason));
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::ReplacedEvent>) {
            f.u(3);
            hash_definition(f, payload.predecessor_definition);
            hash_definition(f, payload.successor_definition);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::ReplaceRejectedEvent>) {
            f.u(4);
            hash_definition(f, payload.live_definition);
            hash_request(f, payload.attempted);
            f.u(static_cast<uint64_t>(payload.reason));
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::CancelledEvent>) {
            f.u(5);
            hash_definition(f, payload.definition);
            f.u(static_cast<uint64_t>(payload.reason));
            f.b(payload.cause.has_value());
            if (payload.cause) hash_event_id(f, *payload.cause);
            hash_authority(f, payload.prior_authority);
            hash_remaining_projection(f, payload.unexecuted);
            hash_pending(f, payload.pending);
        } else if constexpr (std::is_same_v<T, native_order::NotWorkingEvent>) {
            f.u(6);
            hash_handle(f, payload.target);
            hash_optional_request(f, payload.attempted);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::InvalidHandleEvent>) {
            f.u(7);
            hash_handle(f, payload.target);
            hash_optional_request(f, payload.attempted);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::NoEffectEvent>) {
            f.u(8);
            hash_definition(f, payload.definition);
            hash_remaining_projection(f, payload.remaining);
            hash_authority(f, payload.authority);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::MatchRejectedEvent>) {
            f.u(9);
            f.u(static_cast<uint64_t>(payload.reason));
            hash_definition(f, payload.definition);
            hash_remaining_projection(f, payload.remaining);
            hash_authority(f, payload.authority);
            hash_cursor(f, payload.cursor);
            hash_optional_execution_terms(f, payload.attempted_terms);
        } else if constexpr (std::is_same_v<T, native_order::ExecutionAppliedEvent>) {
            f.u(10);
            hash_definition(f, payload.definition);
            f.d(payload.raw_price);
            f.d(payload.resolved_price);
            f.d(payload.current_ticket);
            f.u(payload.first_trade_index);
            f.u(payload.closed_trade_count);
            f.u(payload.opened_lot_incarnation);
            f.d(payload.closed_units);
            f.d(payload.opened_units);
            f.d(payload.filled_working);
            hash_remaining_projection(f, payload.remaining_before);
            hash_remaining_projection(f, payload.remaining_after);
            hash_allowance(f, payload.allowance_before);
            hash_allowance(f, payload.allowance_after);
            f.b(payload.terminal);
            f.b(payload.terminal_reason.has_value());
            if (payload.terminal_reason) {
                f.u(static_cast<uint64_t>(*payload.terminal_reason));
            }
            f.i(payload.cycle_before);
            f.i(payload.cycle_after);
            hash_scope(f, payload.scope);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::CloseBoundEvent>) {
            f.u(11);
            hash_definition(f, payload.definition);
            f.i(payload.cycle);
            f.u(static_cast<uint64_t>(payload.side));
            hash_cursor(f, payload.cursor);
            hash_authority(f, payload.before);
            hash_authority(f, payload.after);
        } else if constexpr (std::is_same_v<T, native_order::ActivatedEvent>) {
            f.u(12);
            hash_definition(f, payload.definition);
            f.u(static_cast<uint64_t>(payload.kind));
            hash_trigger_state(f, payload.before);
            hash_trigger_state(f, payload.after);
            f.d(payload.reached_price);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::ReservationReducedEvent>) {
            f.u(13);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.cause);
            hash_handle(f, payload.recipient);
            f.u(static_cast<uint64_t>(payload.effect));
            f.d(payload.requested_delta);
            f.d(payload.actual_deduction);
            f.d(payload.before.q);
            hash_remaining_projection(f, payload.after);
        } else if constexpr (std::is_same_v<T, native_order::DeferredGroupAdjustmentEvent>) {
            f.u(14);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.cause);
            hash_handle(f, payload.recipient);
            f.u(static_cast<uint64_t>(payload.effect));
            f.d(payload.deferred_delta);
            hash_pending(f, payload.pending_before);
            f.d(payload.pending_after.total);
            f.u(payload.pending_after.count);
            hash_event_id(f, payload.pending_after.tail_receipt);
            f.b(payload.previous_pending_receipt.has_value());
            if (payload.previous_pending_receipt) {
                hash_event_id(f, *payload.previous_pending_receipt);
            }
        } else if constexpr (std::is_same_v<T, native_order::QuantityBoundEvent>) {
            f.u(15);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.source);
            f.d(payload.source_units);
            f.u(payload.prior_adjustment_ids.size());
            for (const auto& id : payload.prior_adjustment_ids) hash_event_id(f, id);
            f.d(payload.pending_total);
            f.d(payload.effective_deduction);
            hash_remaining_projection(f, payload.remaining);
        } else if constexpr (std::is_same_v<T, native_order::ArmedEvent>) {
            f.u(16);
            hash_definition(f, payload.definition);
            hash_authority(f, payload.before);
            hash_authority(f, payload.after);
            f.u(payload.enrollment.index());
            if (const auto* from_cmd = std::get_if<native_order::EnrollmentFromCommand>(&payload.enrollment)) {
                hash_event_id(f, from_cmd->accepted);
            }
            if (const auto* from_app = std::get_if<native_order::EnrollmentFromApplied>(&payload.enrollment)) {
                hash_event_id(f, from_app->cause);
                hash_cursor(f, from_app->cursor);
            }
            f.b(payload.quantity_resolution.has_value());
            if (payload.quantity_resolution) hash_event_id(f, *payload.quantity_resolution);
        } else if constexpr (std::is_same_v<T, native_order::MarginCallEvent>) {
            f.u(18);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.applied);
            hash_cursor(f, payload.cursor);
            f.u(static_cast<uint64_t>(payload.side));
            f.d(payload.mark);
            f.d(payload.equity);
            f.d(payload.required);
            f.d(payload.liquidation_price);
            f.d(payload.units);
            f.d(payload.position_before);
            f.d(payload.position_after);
        } else if constexpr (std::is_same_v<T, native_order::NativeRiskEvent>) {
            f.u(19);
            f.u(static_cast<uint64_t>(payload.kind));
            f.d(payload.limit);
            f.d(payload.observed);
            f.i(payload.day_ordinal);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::TermsResolvedEvent>) {
            f.u(17);
            hash_definition(f, payload.definition);
            hash_cursor(f, payload.cursor);
            hash_terms_input(f, payload.input);
            f.u(payload.prior_adjustment_ids.size());
            for (const auto& id : payload.prior_adjustment_ids) hash_event_id(f, id);
            f.d(payload.pending_total);
            f.d(payload.effective_deduction);
            hash_remaining_projection(f, payload.remaining_before);
            hash_remaining_projection(f, payload.remaining_after);
            hash_allowance(f, payload.allowance_after);
        } else {
            static_assert(!sizeof(T), "unhashed native command event");
        }
    }, event);
}

template <class F>
void hash_driver_point(F& f, const NativeDriverPoint& point) noexcept {
    hash_coordinate(f, point.coordinate);
    f.d(point.raw_price);
    f.b(point.sequence.has_value());
    if (point.sequence) f.u(*point.sequence);
    f.b(point.matching);
    f.b(point.excursion);
}

template <class F>
void hash_account_row(F& f, const NativeAccountObservation& row) noexcept {
    f.u(row.ordinal);
    f.i(row.effective_time_ms);
    f.d(row.marked_equity);
    f.d(row.realized_balance);
    f.d(row.signed_units);
}

// R5 lane E23: the digest names the run's INPUTS, and where a zone file lives
// on this machine is not one of them. What the run read is: the kind of source
// the zone is, the definition it resolved to, and the CONTENT of the resources
// the resolver opened (`resource_digest`, folded at identity time). The
// descriptor keeps `zoneinfo_root` and `resource_paths` as diagnostics and
// this fold ignores both, so one run digests the same value on every host
// carrying the same tzdata release -- while a tzdata update that rewrites the
// zone's rules still moves it, because then the run read different rules.
template <class F>
void hash_tz_identity(
        F& f, const std::optional<native_calendar::TimezoneIdentityDescriptor>& id) noexcept {
    f.b(id.has_value());
    if (!id) return;
    f.u(id->semantics_version);
    f.u(static_cast<uint64_t>(id->kind));
    f.s(id->input);
    f.s(id->effective_definition);
    f.u(id->resource_digest);
}

CommissionType fee_to_commission(NativeFeeKind kind) {
    switch (kind) {
    case NativeFeeKind::Percent: return CommissionType::PERCENT;
    case NativeFeeKind::CashPerUnit: return CommissionType::CASH_PER_CONTRACT;
    case NativeFeeKind::CashPerExecution: return CommissionType::CASH_PER_ORDER;
    }
    return CommissionType::PERCENT;
}

// The kernel's own liquidation is a generic request, not a source signal:
// its label and comment are engine-owned literals no host can collide with
// through the public surface, because a host request is never KernelLiquidation.
constexpr char kNativeLiquidationLabel[] = "__kernel_liquidation__";
constexpr char kNativeLiquidationComment[] = "Margin liquidation";

// L9: the risk block's own flatten, under the same engine-owned convention.
constexpr char kNativeRiskLabel[] = "__kernel_risk__";
constexpr char kNativeRiskComment[] = "Risk limit";

// R5 lane L12 (2.ii l): the settling row records why it closed. Authorship is
// the generic fact — a host request never carries a kernel cause, and a host
// that runs its own forced-close policy records its own cause afterwards.
// R5 lane F3: so is the owner relation. A host request its owner's fill armed
// (WaitForApplied, the one relation that arms: what native_toolkit's bracket
// builds and what an anchored FromOwnerFill level requires) and whose intent
// only closes is a bracket leg. An armed OPENING transaction that reverses the
// book is not one, and neither is a close with no owner relation; both leave
// the row to the generic classification (closed_trade_close_cause).
execution::CloseCause close_cause_for(const native_order::RequestDefinition& definition) {
    switch (definition.origin) {
        case native_order::RequestOrigin::KernelLiquidation:
            return execution::CloseCause::Liquidation;
        case native_order::RequestOrigin::KernelRisk:
            return execution::CloseCause::RiskLimit;
        case native_order::RequestOrigin::Host:
            break;
    }
    const auto& request = definition.request;
    if (!std::holds_alternative<native_order::WaitForApplied>(request.owner))
        return execution::CloseCause::Unspecified;
    const auto* host_sized = std::get_if<native_order::HostSized>(&request.intent);
    const bool closes_only = std::holds_alternative<native_order::Reduce>(request.intent)
        || std::holds_alternative<native_order::Flatten>(request.intent)
        || (host_sized && host_sized->kind == native_order::HostSizedKind::Close);
    return closes_only ? execution::CloseCause::Bracket : execution::CloseCause::Unspecified;
}

uint64_t command_ordinal(const native_order::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

}  // namespace

std::unique_ptr<IExecutionConsumer> make_native_execution_consumer() {
    return std::make_unique<NativeExecutionConsumer>();
}

bool NativeExecutionConsumer::failed() const noexcept {
    return std::holds_alternative<NativeFailed>(state_);
}

bool NativeExecutionConsumer::recoverable_abort() const noexcept {
    const auto* failed_state = std::get_if<NativeFailed>(&state_);
    return failed_state != nullptr && failed_state->failure.code == NativeFailureCode::Aborted;
}

void NativeExecutionConsumer::latch_failure(NativeFailure failure) noexcept {
    if (std::holds_alternative<NativeFailed>(state_)) return;
    leave_running();
    std::optional<NativeRunSpec> spec;
    if (auto* r = std::get_if<NativeReady>(&state_)) spec = std::move(r->spec);
    else if (auto* n = std::get_if<NativeRunning>(&state_)) spec = std::move(n->spec);
    else if (auto* c = std::get_if<NativeCompleted>(&state_)) spec = std::move(c->spec);
    NativeFailed failed;
    failed.spec = std::move(spec);
    failed.failure = failure;
    state_.emplace<NativeFailed>(std::move(failed));
}

void NativeExecutionConsumer::fail(BacktestEngine& engine, NativeFailure failure) noexcept {
    latch_failure(failure);
    engine.last_run_status_ = 1;
}

void NativeExecutionConsumer::render(BacktestEngine& engine, const char* text) const {
    engine.last_error_ = text ? text : "";
}

void NativeExecutionConsumer::present_refusal(BacktestEngine& engine, const char* text) {
    render(engine, text);
    engine.last_run_status_ = 1;
}

bool NativeExecutionConsumer::refuse_mixed_input_mode(BacktestEngine& engine, InputMode requested) {
    if (input_mode_ != InputMode::Unselected && input_mode_ != requested) {
        present_refusal(engine, "native stream cannot mix confirmed bars and ticks");
        return true;
    }
    return false;
}

void NativeExecutionConsumer::select_input_mode(InputMode requested) {
    if (input_mode_ == InputMode::Unselected) input_mode_ = requested;
}

bool NativeExecutionConsumer::admit_public_begin(BacktestEngine& engine, const char* not_ready_text) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    // v10 §4: begin outside Ready refuses without consuming identity/history.
    // Unconfigured/Completed stay put. A begin while Running is a contract
    // failure, including reentry from on_native_run_begin / on_native_bar.
    if (preparing_begin_ || in_callback_ || processing_input_
        || std::holds_alternative<NativeRunning>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native begin is forbidden while running");
        return false;
    }
    if (!std::holds_alternative<NativeReady>(state_)) {
        present_refusal(engine, not_ready_text);
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::apply_staged_ingress(BacktestEngine& engine) {
    // BacktestEngine owns the copied C/C++ FX arrays while a host is not yet
    // running.  Applying them here makes staging independent of whether the
    // provider configured the host before or during this begin call.
    // A provider may intentionally leave an unconfigured/completed host for
    // admit_public_begin to diagnose.  Do not turn that ordinary lifecycle
    // refusal into a mutation failure merely because old staged bytes exist.
    if (!std::holds_alternative<NativeReady>(state_)
        || (engine.account_currency_fx_timestamps_.empty()
        && engine.account_currency_fx_rates_.empty())) {
        return true;
    }
    NativeFxCurve curve;
    curve.effective_from_ms = engine.account_currency_fx_timestamps_;
    curve.account_per_quote = engine.account_currency_fx_rates_;
    const auto result = configure_fx_curve(curve);
    if (result.status == NativeSetupStatus::Applied) return true;
    fail(engine, NativeFailure{NativeFailureCode::InvalidSpecification,
                               NativeFailureOperation::Configure,
                               0, static_cast<std::uint32_t>(result.validation.error)});
    render(engine, "native staged account-currency FX was rejected");
    return false;
}

bool NativeExecutionConsumer::prepare_public_begin(
        BacktestEngine& engine, const NativeBeginArgs& args) {
    interval_cache_.clear();
    // Borrowed for the length of this begin call only: begin_ready needs the
    // caller's own input array to prepare declared higher-timeframe series.
    begin_bars_ = args.bars;
    begin_n_ = args.n;
    begin_is_stream_ = args.is_stream;
    if (preparing_begin_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native prepare_native_begin cannot reenter");
        return false;
    }
    // The run's one downcast: every callback site reads it back through
    // native_host().
    downcast_engine_ = &engine;
    downcast_host_ = dynamic_cast<NativeStrategyHost*>(&engine);
    auto* host = downcast_host_;
    if (!host) return true;
    preparing_begin_ = true;
    try {
        host->prepare_native_begin(args);
    } catch (const std::exception& e) {
        preparing_begin_ = false;
        // A provider's begin-time validation is a public-entry refusal.  It
        // has not started a native run or consumed an identity, so preserve a
        // reusable Unconfigured/Completed host just as other begin refusals
        // do. Callback exceptions after begin_ready remain terminal.
        render(engine, e.what());
        return false;
    } catch (...) {
        preparing_begin_ = false;
        render(engine, "native pre-begin provider exception");
        return false;
    }
    preparing_begin_ = false;
    if (failed() || !apply_staged_ingress(engine)) return false;
    return validate_undetected_begin(engine, args);
}

bool NativeExecutionConsumer::admit_public_stream_input(BacktestEngine& engine,
                                                        NativeFailureOperation operation) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    // Public stream/run inputs are serialized. Nested calls from a native
    // callback or an in-flight input are a Running contract failure, not a
    // second pump and not a Completed downgrade. Internal pump_batch and
    // deliver_tick are not public entry points.
    if (in_callback_ || processing_input_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation});
        render(engine, "native public input cannot reenter an active callback or input");
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::check_abort(BacktestEngine& engine,
                                          NativeFailureOperation operation,
                                          uint64_t ordinal) {
    if (failed()) return false;
    if (engine.abort_requested_.load(std::memory_order_relaxed)) {
        fail(engine, NativeFailure{NativeFailureCode::Aborted, operation, ordinal});
        const auto* spec = spec_ptr();
        if (!spec || spec->abort_reporting == NativeAbortReporting::Error) {
            render(engine, "native run aborted");
        } else {
            engine.last_error_.clear();
        }
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::check_abort_or_projection(BacktestEngine& engine,
                                                        NativeFailureOperation operation,
                                                        uint64_t ordinal) {
    if (!check_abort(engine, operation, ordinal)) return false;
    if (!projection_ok(engine)) {
        fail(engine, NativeFailure{NativeFailureCode::ProjectionMismatch, operation, ordinal});
        render(engine, "native projection mismatch");
        return false;
    }
    return true;
}

double NativeExecutionConsumer::ladder_tick() const {
    const auto* spec = spec_ptr();
    return spec ? spec->price_tick : 0.0;
}

const NativeRunSpec* NativeExecutionConsumer::spec_ptr() const {
    if (running_spec_) return running_spec_;
    if (const auto* n = std::get_if<NativeRunning>(&state_)) return &n->spec;
    if (const auto* r = std::get_if<NativeReady>(&state_)) return &r->spec;
    if (const auto* c = std::get_if<NativeCompleted>(&state_)) return &c->spec;
    if (const auto* f = std::get_if<NativeFailed>(&state_)) {
        if (f->spec) return &*f->spec;
    }
    return nullptr;
}

bool NativeExecutionConsumer::path_high_first(const Bar& bar) const {
    const auto* spec = spec_ptr();
    return path_uses_high_first(bar, spec ? spec->path_order : NativePathOrder::Auto);
}

bool NativeExecutionConsumer::commands_allowed() const {
    if (failed() || consuming_request_) return false;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running) return false;
    if (in_callback_) return true;
    if (processing_input_) return false;
    return running->phase == NativeRunPhase::Realtime;
}

NativeStateView NativeExecutionConsumer::view() const {
    NativeStateView v;
    v.consumed_high_water = consumed_high_water_;
    v.decision_floor_ms = decision_floor();
    // running_spec_ is non-null exactly while state_ holds NativeRunning (see
    // cache_running_policy), whose spec it points at: the view the probe
    // below builds for that alternative, without walking the variant first.
    if (running_spec_) {
        v.kind = NativeLifecycleKind::Running;
        v.spec = running_spec_;
        v.phase = std::get_if<NativeRunning>(&state_)->phase;
        return v;
    }
    v.spec = spec_ptr();
    if (std::holds_alternative<NativeUnconfigured>(state_)) {
        v.kind = NativeLifecycleKind::Unconfigured;
    } else if (auto* r = std::get_if<NativeReady>(&state_)) {
        v.kind = NativeLifecycleKind::Ready;
        v.spec = &r->spec;
    } else if (auto* n = std::get_if<NativeRunning>(&state_)) {
        v.kind = NativeLifecycleKind::Running;
        v.spec = &n->spec;
        v.phase = n->phase;
    } else if (auto* c = std::get_if<NativeCompleted>(&state_)) {
        v.kind = NativeLifecycleKind::Completed;
        v.spec = &c->spec;
        v.completion = c->completion;
    } else if (auto* f = std::get_if<NativeFailed>(&state_)) {
        v.kind = NativeLifecycleKind::Failed;
        v.failure = f->failure;
        if (f->spec) v.spec = &*f->spec;
    }
    return v;
}

void NativeExecutionConsumer::refuse_source_mutation(const char* operation) {
    // Source-free ingress is staging while no run is active: before a first
    // begin, between completed runs, and after a cooperative abort. The
    // existing in-run refusal stays fail-closed.
    if (recoverable_abort()
        || (!failed() && (std::holds_alternative<NativeUnconfigured>(state_)
                          || std::holds_alternative<NativeReady>(state_)
                          || std::holds_alternative<NativeCompleted>(state_)
                          || preparing_begin_
                          // The kernel's own registration of the declared
                          // higher-timeframe series, which runs after the run
                          // is Running and calls no host callback inside its
                          // scope. It is the consumer writing the engine's
                          // feed store, never a source host mutating a run.
                          || wiring_subscriptions_))) {
        return;
    }
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    latch_failure(failure);
    throw std::runtime_error(std::string("native host refuses source mutation: ") +
                             (operation ? operation : ""));
}

bool NativeExecutionConsumer::stage_account_currency_fx_series(
        const std::vector<std::int64_t>& timestamps, const std::vector<double>& rates) {
    if (timestamps.size() != rates.size()) return false;
    NativeFxCurve candidate;
    try {
        candidate.effective_from_ms = timestamps;
        candidate.account_per_quote = rates;
    } catch (...) {
        return false;
    }
    if (validate_native_fx_curve(candidate).error != NativeFxCurveError::None) return false;
    if (candidate.effective_from_ms.empty()) {
        staged_fx_curve_.reset();
        staged_ingress_fx_ = true;
        return true;
    }
    try {
        std::optional<NativeFxCurve> replacement;
        replacement.emplace(std::move(candidate));
        staged_fx_curve_.swap(replacement);
        staged_ingress_fx_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

// The spec's bar arrays are fixed from the configure that stages them to the
// next one, except where a run-begin declaration rewrites the series or the
// feed; each of those clears the cache, so the digests are always this spec's.
const NativeExecutionConsumer::SpecBarDigests& NativeExecutionConsumer::spec_bar_digests(
        const NativeRunSpec& spec) const noexcept {
    if (!spec_bar_digests_) {
        SpecBarDigests digests;
        digests.intrabar = native_intrabar_path_digest(spec.intrabar);
        if (!spec.subscriptions.empty())
            digests.subscriptions = native_timeframe_subscriptions_digest(spec.subscriptions);
        if (spec.auxiliary_feed)
            digests.auxiliary = native_auxiliary_feed_digest(*spec.auxiliary_feed);
        spec_bar_digests_ = digests;
    }
    return *spec_bar_digests_;
}

uint64_t NativeExecutionConsumer::continuation_hash() const noexcept {
    // The fold, for either sink (R5 lane PERF-P1). An Fnv folds it now. A
    // FnvRecord records the bytes an Fnv would fold into the view being
    // captured, and leaves the two lazily folded digests behind as holes at
    // their logs' lengths; the account digest is folded as each row is
    // appended, so it is recorded like any other value.
    const auto fold = [this](auto& f) noexcept {
        constexpr bool recording = std::is_same_v<std::decay_t<decltype(f)>, FnvRecord>;
        f.run_base = requests_.identity().run_number;
        f.s(kNativeConsumerSemanticVersion);
        f.s(kNativeDriverSemanticVersion);
        f.s(kNativeCalendarSemanticVersion);
        f.u(static_cast<uint64_t>(state_.index()));
        if (const auto* running = std::get_if<NativeRunning>(&state_)) {
            f.u(static_cast<uint64_t>(running->phase));
        }
        if (const auto* completed = std::get_if<NativeCompleted>(&state_)) {
            f.u(static_cast<uint64_t>(completed->completion));
        }
        if (const auto* failed_state = std::get_if<NativeFailed>(&state_)) {
            hash_failure(f, failed_state->failure);
        }
        f.s(bound_session_key_);
        f.i(decision_floor_ms_);
        f.b(has_floor_);
        f.u(next_timeline_ordinal_);
        f.b(in_callback_);
        f.u(static_cast<uint64_t>(callback_phase_));
        f.b(preparing_begin_);
        f.b(input_callback_context_.has_value());
        if (input_callback_context_) hash_input_context(f, *input_callback_context_);
        f.b(input_callback_bar_.has_value());
        if (input_callback_bar_) hash_bar(f, *input_callback_bar_);
        f.b(tick_callback_context_.has_value());
        if (tick_callback_context_) hash_tick_context(f, *tick_callback_context_);
        f.b(tick_callback_bar_.has_value());
        if (tick_callback_bar_) hash_bar(f, *tick_callback_bar_);
        hash_coordinate(f, callback_context_.coordinate);
        f.i(callback_context_.decision_floor_ms);
        hash_interval(f, callback_context_.input_interval);
        hash_interval(f, callback_context_.script_interval);
        f.i(callback_context_.sub_index);
        f.i(callback_context_.sub_count);
        f.b(callback_context_.is_terminal_sub_bar);
        f.i(callback_context_.sub_bar_open_ms);
        f.i(callback_context_.script_bar_open_ms);
        hash_driver_statistics(f, callback_context_.driver_statistics);
        // The four session-day facts are not folded, here or in the frames and
        // notifications below: each is a function of the label and calendar this
        // digest already folds and of the run's input around the bar (R5 lane F5,
        // present_session_day), so folding them would move every established
        // continuation value for nothing it does not already identify.
        f.b(consuming_request_);
        f.b(draining_notifications_);
        f.b(current_frame_.has_value());
        if (current_frame_) {
            hash_current_point(f, current_frame_->point);
            f.u(current_frame_->acceptance_cutoff);
        }
        f.u(pre_open_birth_point_ordinal_);
        f.i(pre_open_birth_time_ms_);
        f.u(pre_open_births_.size());
        for (const auto& handle : pre_open_births_) hash_handle(f, handle);
        f.u(applied_notifications_.size() - notification_head_);
        for (std::size_t i = notification_head_; i < applied_notifications_.size(); ++i) {
            const auto& notification = applied_notifications_[i];
            f.u(notification.history_index);
            f.u(notification.ordinal);
            hash_current_point(f, notification.point);
            // Conditional: only a kernel-originated fill carries a margin receipt,
            // so a host-only queue folds exactly what it folded before L4.
            if (notification.margin_call_index) {
                f.u(1);
                f.u(*notification.margin_call_index);
            }
        }
        f.b(processing_input_);
        f.u(static_cast<uint64_t>(input_mode_));
        f.i(next_interval_index_);
        // Declared higher-timeframe series carry their own delivery cursors, and
        // a stream's warmup boundary is where its live phase starts -- neither is
        // recoverable from the input count alone. Folded only for a run that
        // declares a series, so every spec without one keeps the pre-subscription
        // continuation identity (the same rule hash_spec's digest follows).
        if (!subscriptions_.empty()) {
            f.i(subscription_warmup_inputs_);
            for (const auto& subscription : subscriptions_) {
                f.u(subscription.index);
                f.b(subscription.lookahead);
                f.b(subscription.latest.has_value());
                if (subscription.latest) hash_bar(f, *subscription.latest);
                f.i(subscription.bucket_first_index);
                f.i(subscription.bucket_first_ms);
                f.u(subscription.projected_bars.size());
                f.u(subscription.projected_cursor);
                // Only a series built from the auxiliary feed has a feed cursor;
                // a series built from the input folds exactly what it did.
                if (subscription.auxiliary) f.u(subscription.auxiliary_cursor);
            }
        }
        // Bars a realtime stream appended to its declared auxiliary feed are
        // durable input no spec digest covers. Folded only for a run that
        // declares a feed.
        if (auxiliary_tf_) {
            f.u(auxiliary_appended_.size());
            f.u(auxiliary_appended_digest_);
        }
        if (const auto* spec = spec_ptr()) {
            hash_spec(f, *spec, &spec_bar_digests(*spec));
            // L5: the recalculation cadence is durable decision state only for a
            // spec that opted into it. Folding it conditionally keeps a default
            // run's continuation identity byte-identical to the pre-lane tree.
            if (calc_timing_on(*spec)) {
                f.u(recalc_epoch_);
                f.u(recalc_epoch_count_);
                f.u(recalculations_);
                f.u(recalculations_skipped_);
                f.b(partial_has_);
                if (partial_has_) {
                    hash_bar(f, partial_);
                    f.i(partial_script_open_ms_);
                }
            }
        }
        f.b(staged_ingress_fx_);
        f.b(staged_fx_curve_.has_value());
        if (staged_fx_curve_) f.u(native_fx_curve_digest(*staged_fx_curve_));
        hash_tz_identity(f, tz_identity_);
        f.s(requests_.identity().session_key);
        f.u(requests_.live().size());
        for (const auto& live : requests_.live()) {
            hash_definition(f, live.definition);
            hash_remaining(f, live.remaining);
            hash_authority(f, live.authority);
            hash_trigger_state(f, live.trigger_state);
            hash_allowance(f, live.allowance);
            hash_pending(f, live.pending);
            // L3b: a placement-time sizing measurement is durable decision state
            // only for the request that froze one. Folding it conditionally keeps
            // every pre-lane request table byte-identical.
            if (live.sizing_units) { f.u(1); f.d(*live.sizing_units); }
            if (live.sizing_scope) { f.u(2); f.d(*live.sizing_scope); }
            if (live.sizing_price) { f.u(3); f.d(*live.sizing_price); }
        }
        // The host-maintained roster is durable matching authority.  Fold it
        // immediately after the request table so a membership-only change cannot
        // share a continuation identity with an otherwise identical run.
        hash_cohorts(f, requests_);
        // Both logs are append-only for the life of a run -- begin_ready is the
        // only thing that clears them, and it resets these digests and rebinds
        // the run identity in the same breath -- and each fold chains on the one
        // before it. A digest that is behind therefore needs its TAIL folded,
        // exactly as the command history's does; re-deriving the whole log at
        // every query is the same chain at quadratic cost, which a host that
        // reads the continuation once per bar pays in full (lane E24 measured
        // 4x the bars costing 16x the CPU under broker-state-hash recording).
        if constexpr (!recording) {
            sync_history_digest(requests_.history().size());
            sync_driver_digest(driver_log_.size());
        }
        if (account_digest_.count > account_log_.size()) account_digest_.reset();
        for (std::size_t index = account_digest_.count; index < account_log_.size(); ++index) {
            fold_account_digest(account_log_[index]);
        }
        if constexpr (recording) {
            continuation_view_.history_count = requests_.history().size();
            f.u(continuation_view_.history_count);
            continuation_view_.history_hole = continuation_view_.bytes.size();
        } else {
            f.u(history_digest_.count);
            f.u(history_digest_.h);
        }
        f.b(current_input_open_.has_value());
        if (current_input_open_) f.i(*current_input_open_);
        f.b(observed_input_cursor_.has_value());
        if (observed_input_cursor_) f.i(*observed_input_cursor_);
        f.b(next_tradable_synthesis_cursor_.has_value());
        if (next_tradable_synthesis_cursor_) f.i(*next_tradable_synthesis_cursor_);
        f.b(last_accepted_input_.has_value());
        if (last_accepted_input_) hash_interval(f, *last_accepted_input_);
        f.b(last_observed_slot_open_.has_value());
        if (last_observed_slot_open_) f.i(*last_observed_slot_open_);
        f.b(last_finalized_input_.has_value());
        if (last_finalized_input_) hash_interval(f, *last_finalized_input_);
        f.b(has_tick_sequence_);
        f.u(last_tick_sequence_);
        f.i(script_.key);
        f.b(script_.has_data);
        f.b(script_.sealed);
        hash_interval(f, script_.interval);
        hash_bar(f, script_.agg);
        f.i(script_.first_open_ms);
        f.i(script_.first_source_time_ms);
        f.i(script_.latest_close_ms);
        f.i(script_.first_index);
        f.i(script_.last_index);
        f.b(script_.modeled_ohlc);
        f.b(has_forming_);
        if (has_forming_) hash_bar(f, forming_);
        f.b(has_last_price_);
        f.d(last_price_);
        f.i(last_print_time_ms_);
        hash_driver_statistics(f, driver_statistics_);
        f.u(static_cast<uint64_t>(pairing_.pairing));
        f.i(pairing_.group_factor);
        if constexpr (recording) {
            continuation_view_.driver_count = driver_log_.size();
            f.u(continuation_view_.driver_count);
            continuation_view_.driver_hole = continuation_view_.bytes.size();
        } else {
            f.u(driver_digest_.count);
            f.u(driver_digest_.h);
        }
        f.u(account_digest_.count);
        f.u(account_digest_.h);
        if (precommit_digest_.count != 0) {
            f.u(precommit_digest_.count);
            f.u(precommit_digest_.h);
        }
        // L4 durable liquidation state. It exists only under a declared margin
        // model, and folds only there, so no pre-L4 continuation identity moves.
        if (margin_model() != nullptr) {
            f.b(margin_liquidation_.has_value());
            if (margin_liquidation_) {
                hash_handle(f, margin_liquidation_->handle);
                f.d(margin_liquidation_->level);
                f.d(margin_liquidation_->units);
            }
            f.b(has_margin_path_);
            if (has_margin_path_) {
                hash_bar(f, margin_path_bar_);
                f.b(margin_path_high_first_);
            }
            f.u(margin_point_ordinal_);
            f.u(margin_point_calls_);
        }
        // L9 durable risk ledger. It exists only under declared risk limits, and
        // folds only there, so no pre-L9 continuation identity moves.
        if (risk_limits() != nullptr) {
            f.b(risk_.has_day);
            if (risk_.has_day) f.i(risk_.day_ordinal);
            f.u(risk_.fills_today);
            f.u(risk_.consecutive_loss_days);
            f.b(risk_.has_peak);
            if (risk_.has_peak) f.d(risk_.peak_equity);
            f.d(risk_.day_open_equity);
            f.d(risk_.day_open_realized);
            f.b(risk_.run_block.has_value());
            if (risk_.run_block) f.u(static_cast<uint64_t>(*risk_.run_block));
            f.b(risk_.day_block.has_value());
            if (risk_.day_block) {
                f.u(static_cast<uint64_t>(*risk_.day_block));
                f.i(risk_.day_block_day);
            }
        }
    };
    if (continuation_view_.state == ContinuationView::State::Recording) {
        FnvRecord record{continuation_view_.bytes};
        fold(record);
        continuation_view_.complete = !record.failed;
        return 0;
    }
    // A pending view names both digests at the lengths it was taken at, and
    // this fold carries them to the logs' ends, where nothing can take them
    // back: the view is folded first.
    if (continuation_view_.state == ContinuationView::State::Pending) fold_continuation_view();
    Fnv f;
    fold(f);
    return f.h;
}

// R5 lane PERF-P1. A Pine run latches the continuation at its last script
// point and nothing on the benchmark or report path ever reads it, yet that
// one fold took a fifth of the run: every driver point (four a bar) and every
// command event of the run folded into their digests at the end. A view
// copies the bytes of the live state the fold reads -- the request table, the
// cohort rosters and receipts, the frames and cursors -- and leaves the two
// logs where they lie; the first reader pays the fold that was skipped, and
// no one else does. A buffer that cannot grow falls back to folding at once,
// so the value never depends on memory.
void NativeExecutionConsumer::capture_continuation_view() noexcept {
    auto& view = continuation_view_;
    view.state = ContinuationView::State::None;
    view.bytes.clear();
    if (defer_continuation_views_) {
        view.run_number = requests_.identity().run_number;
        view.state = ContinuationView::State::Recording;
        (void)continuation_hash();
        view.state = ContinuationView::State::None;
        if (view.complete) {
#ifndef NDEBUG
            // Debug builds prove every view against the eager fold it
            // replaces. The fold leaves both digests at the view's lengths,
            // so the view stays pending and a later read still folds it.
            const uint64_t eager = continuation_hash();
            if (fold_continuation_view() != eager) std::abort();
#endif
            view.state = ContinuationView::State::Pending;
            return;
        }
        view.bytes.clear();
    }
    view.value = continuation_hash();
    view.state = ContinuationView::State::Folded;
}

void NativeExecutionConsumer::drop_continuation_view() const noexcept {
    continuation_view_.state = ContinuationView::State::None;
}

bool NativeExecutionConsumer::continuation_view_pending() const noexcept {
    return continuation_view_.state == ContinuationView::State::Pending;
}

uint64_t NativeExecutionConsumer::latched_continuation(uint64_t eager) const noexcept {
    switch (continuation_view_.state) {
    case ContinuationView::State::None:
    case ContinuationView::State::Recording: return eager;
    case ContinuationView::State::Pending: return fold_continuation_view();
    case ContinuationView::State::Folded: return continuation_view_.value;
    }
    return eager;
}

// The view's value: both digests carried to exactly the lengths it was taken
// at -- from wherever they stand, which is never past them, since
// continuation_hash() folds a pending view before it moves them -- then the
// recorded bytes with the two digests in their holes.
uint64_t NativeExecutionConsumer::fold_continuation_view() const noexcept {
    auto& view = continuation_view_;
#ifndef NDEBUG
    // Debug builds hold the invariants that make a view foldable: it is of
    // this run, its logs have only grown since, and neither digest is past it.
    if (view.run_number != requests_.identity().run_number
        || view.history_count > requests_.history().size()
        || view.driver_count > driver_log_.size()
        || history_digest_.count > view.history_count
        || driver_digest_.count > view.driver_count) {
        std::abort();
    }
#endif
    sync_history_digest(view.history_count);
    sync_driver_digest(view.driver_count);
    const unsigned char* const bytes = view.bytes.data();
    Fnv f;
    fold_recorded(f, bytes, view.history_hole);
    f.u(history_digest_.h);
    fold_recorded(f, bytes + view.history_hole, view.driver_hole - view.history_hole);
    f.u(driver_digest_.h);
    fold_recorded(f, bytes + view.driver_hole, view.bytes.size() - view.driver_hole);
    view.value = f.h;
    view.state = ContinuationView::State::Folded;
    return view.value;
}

bool NativeExecutionConsumer::timeframe_args_ok(const std::string& input_tf,
                                                const std::string& script_tf) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (spec->timeframe_undetected) return input_tf.empty() && script_tf.empty();
    if (!input_tf.empty() && input_tf != spec->input_tf) return false;
    if (!script_tf.empty() && script_tf != spec->script_tf) return false;
    return true;
}

bool NativeExecutionConsumer::has_undetected_timeframe() const noexcept {
    if (running_spec_) return running_undetected_;
    const auto* spec = spec_ptr();
    return spec && spec->timeframe_undetected;
}

bool NativeExecutionConsumer::legacy_tolerant_slot_labels() const noexcept {
    if (running_spec_) return running_tolerant_labels_;
    const auto* spec = spec_ptr();
    return spec && spec->slot_label_policy == NativeSlotLabelPolicy::FeedTolerant;
}

bool NativeExecutionConsumer::uses_raw_label_partition() const noexcept {
    if (running_spec_) return running_raw_labels_;
    return has_undetected_timeframe()
        || (legacy_tolerant_slot_labels()
            && pairing_.pairing == native_calendar::TimeframePairing::Passthrough);
}

void NativeExecutionConsumer::cache_running_policy() noexcept {
    // Derived by the probing path, with the cache still empty.
    running_spec_ = nullptr;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running) return;
    running_undetected_ = has_undetected_timeframe();
    running_tolerant_labels_ = legacy_tolerant_slot_labels();
    running_raw_labels_ = uses_raw_label_partition();
    running_spec_ = &running->spec;
}

native_calendar::NativeInterval NativeExecutionConsumer::timestamp_partition(
        std::int64_t timestamp) noexcept {
    // No duration is available in this state. Each boundary is the current
    // bar timestamp, so no inferred aggregation or clock grid is introduced.
    return {timestamp, timestamp, timestamp, timestamp, timestamp};
}

std::optional<native_calendar::NativeInterval>
NativeExecutionConsumer::input_interval_at(std::int64_t timestamp) const {
    if (interval_cache_.input_ts == timestamp) {
        return interval_cache_.input_interval;
    }
    if (uses_raw_label_partition()) return timestamp_partition(timestamp);
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, timestamp, calendar_memo_);
    if (!interval && legacy_tolerant_slot_labels()) interval = timestamp_partition(timestamp);
    interval_cache_.input_ts = timestamp;
    interval_cache_.input_interval = interval;
    return interval;
}

std::optional<native_calendar::NativeInterval>
NativeExecutionConsumer::script_interval_at(std::int64_t timestamp) const {
    if (interval_cache_.script_ts == timestamp) {
        return interval_cache_.script_interval;
    }
    if (uses_raw_label_partition()) return timestamp_partition(timestamp);
    auto interval = native_calendar::interval_containing(calendar_, script_tf_, timestamp, calendar_memo_);
    if (!interval && legacy_tolerant_slot_labels()) interval = timestamp_partition(timestamp);
    interval_cache_.script_ts = timestamp;
    interval_cache_.script_interval = interval;
    return interval;
}

bool NativeExecutionConsumer::validate_undetected_begin(
        BacktestEngine& engine, const NativeBeginArgs& args) {
    if (!has_undetected_timeframe()) return true;
    if (!args.input_tf.empty() || !args.script_tf.empty()) {
        present_refusal(engine, "native undetected timeframe requires empty timeframe arguments");
        return false;
    }
    if (args.n >= 2) {
        present_refusal(engine, "native undetected timeframe requires fewer than two bars");
        return false;
    }
    if (args.is_stream) {
        present_refusal(engine, "native stream requires a detected timeframe");
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::apply_spec(BacktestEngine& engine, const NativeRunSpec& spec) {
    engine.initial_capital_ = spec.initial_capital;
    engine.syminfo_.pointvalue = spec.point_value;
    engine.account_currency_fx_ = spec.account_fx;
    if (staged_fx_curve_) {
        engine.account_currency_fx_timestamps_ = staged_fx_curve_->effective_from_ms;
        engine.account_currency_fx_rates_ = staged_fx_curve_->account_per_quote;
    } else {
        engine.account_currency_fx_timestamps_.clear();
        engine.account_currency_fx_rates_.clear();
    }
    engine.syminfo_.mintick = spec.price_tick;
    engine.syminfo_mintick_ = spec.price_tick;
    engine.commission_type_ = fee_to_commission(spec.fee_kind);
    engine.commission_value_ = spec.fee_value;
    engine.syminfo_.ticker = spec.ticker;
    engine.syminfo_.tickerid = spec.tickerid;
    engine.syminfo_.type = spec.type;
    engine.syminfo_.currency = spec.currency;
    engine.syminfo_.basecurrency = spec.basecurrency;
    engine.syminfo_.description = spec.description;
    engine.syminfo_.volumetype = spec.volumetype;
    engine.syminfo_.timezone = spec.timezone;
    engine.syminfo_.session = spec.session;
    engine.chart_timezone_ = spec.chart_timezone;
    engine.slippage_ = 0;
    applied_ = spec;
    auto parsed_session = native_calendar::parse_session(spec.session, spec.timezone);
    if (!parsed_session) return false;
    intrabar_tf_.reset();
    if (!spec.timeframe_undetected) {
        auto parsed_input = native_calendar::parse_timeframe(spec.input_tf);
        auto parsed_script = native_calendar::parse_timeframe(spec.script_tf);
        if (!parsed_input || !parsed_script) return false;
        input_tf_ = std::move(*parsed_input);
        script_tf_ = std::move(*parsed_script);
    } else {
        input_tf_ = native_calendar::Timeframe{};
        script_tf_ = native_calendar::Timeframe{};
    }
    if (const auto* lower = spec.intrabar.lower()) {
        auto parsed_intrabar = native_calendar::parse_timeframe(lower->tf);
        if (!parsed_intrabar) return false;
        intrabar_tf_ = std::move(*parsed_intrabar);
    }
    calendar_ = std::move(*parsed_session);
    reset_calendar_memos();
    // L9: a CalendarDayInTimezone risk day keys on the plain civil date of the
    // spec's scheduling timezone, which is the trading date of an all-day
    // session there. Built once per run, and only for the basis that needs it.
    risk_day_calendar_.reset();
    if (spec.risk && spec.risk->day_basis == NativeRiskDay::CalendarDayInTimezone) {
        auto plain_day = native_calendar::parse_session("", spec.timezone);
        if (!plain_day) return false;
        risk_day_calendar_ = std::move(*plain_day);
    }
    pairing_ = spec.timeframe_undetected ? native_calendar::TimeframeCompatibility{}
                                         : native_calendar::compatibility(input_tf_, script_tf_);
    tz_identity_ = native_calendar::timezone_identity_descriptor(spec.timezone);
    return true;
}

namespace {

template <typename Word>
inline Word load_word(const char* p) noexcept {
    Word word;
    std::memcpy(&word, p, sizeof word);
    return word;
}

// std::string's own equality -- the same length and the same bytes -- with
// every byte inside one of a few word loads, the last overlapping the word
// before it: no byte loop and no library call. projection_ok runs it over ten
// short strings at every input and callback boundary.
inline bool same_text(const std::string& a, const std::string& b) noexcept {
    const std::size_t n = a.size();
    if (n != b.size()) return false;
    const char* x = a.data();
    const char* y = b.data();
    if (n >= 8) {
        for (std::size_t i = 0; i + 8 < n; i += 8) {
            if (load_word<std::uint64_t>(x + i) != load_word<std::uint64_t>(y + i)) return false;
        }
        return load_word<std::uint64_t>(x + n - 8) == load_word<std::uint64_t>(y + n - 8);
    }
    if (n >= 4) {
        return load_word<std::uint32_t>(x) == load_word<std::uint32_t>(y)
            && load_word<std::uint32_t>(x + n - 4) == load_word<std::uint32_t>(y + n - 4);
    }
    // Three bytes or fewer: the first, the middle and the last are all of them.
    return n == 0 || (x[0] == y[0] && x[n / 2] == y[n / 2] && x[n - 1] == y[n - 1]);
}

}  // namespace

bool NativeExecutionConsumer::same_bytes(const std::string& a, const std::string& b) noexcept {
    return same_text(a, b);
}

bool NativeExecutionConsumer::projection_ok(const BacktestEngine& engine) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (engine.initial_capital_ != spec->initial_capital) return false;
    if (engine.syminfo_.pointvalue != spec->point_value) return false;
    if (engine.account_currency_fx_ != spec->account_fx) return false;
    if (staged_fx_curve_) {
        if (engine.account_currency_fx_timestamps_ != staged_fx_curve_->effective_from_ms
            || engine.account_currency_fx_rates_ != staged_fx_curve_->account_per_quote) {
            return false;
        }
    } else if (!engine.account_currency_fx_timestamps_.empty()
               || !engine.account_currency_fx_rates_.empty()) {
        return false;
    }
    if (engine.syminfo_.mintick != spec->price_tick) return false;
    if (engine.commission_type_ != fee_to_commission(spec->fee_kind)) return false;
    if (engine.commission_value_ != spec->fee_value) return false;
    // The ten strings, in the order they have always been compared.
    using Text = std::pair<std::string SymInfo::*, std::string NativeRunSpec::*>;
    static constexpr Text kSymbolTexts[] = {
        {&SymInfo::ticker, &NativeRunSpec::ticker},
        {&SymInfo::tickerid, &NativeRunSpec::tickerid},
        {&SymInfo::type, &NativeRunSpec::type},
        {&SymInfo::currency, &NativeRunSpec::currency},
        {&SymInfo::basecurrency, &NativeRunSpec::basecurrency},
        {&SymInfo::description, &NativeRunSpec::description},
        {&SymInfo::volumetype, &NativeRunSpec::volumetype},
        {&SymInfo::timezone, &NativeRunSpec::timezone},
        {&SymInfo::session, &NativeRunSpec::session},
    };
    for (const Text& text : kSymbolTexts) {
        if (!same_text(engine.syminfo_.*text.first, spec->*text.second)) return false;
    }
    return same_text(engine.chart_timezone_, spec->chart_timezone);
}

NativeSetupResult NativeExecutionConsumer::configure(BacktestEngine& engine,
                                                     const NativeRunSpec& spec) {
    NativeSetupResult result;
    const NativeRunSpec* prior_spec = nullptr;
    // A refusal for the host's phase judges no field: it is WrongPhase at
    // None, the same word configure_native_fx_curve answers for its phase.
    const NativeRunSpecValidation wrong_phase{NativeRunSpecError::WrongPhase,
                                              NativeRunSpecField::None};
    if (failed() && !recoverable_abort()) {
        result.validation = wrong_phase;
        render(engine, "native host already failed");
        return result;
    }
    if (recoverable_abort()) {
        const auto* aborted = std::get_if<NativeFailed>(&state_);
        if (!aborted || !aborted->spec) {
            result.validation.error = NativeRunSpecError::CalendarFailure;
            render(engine, "native aborted host has no reusable run spec");
            return result;
        }
        prior_spec = &*aborted->spec;
    }
    if (std::holds_alternative<NativeRunning>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
        render(engine, "configure refused while running");
        result.validation = wrong_phase;
        return result;
    }
    if (std::holds_alternative<NativeReady>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
        render(engine, "configure refused while ready; use a fresh host");
        result.validation = wrong_phase;
        return result;
    }
    NativeRunSpec candidate = spec;
    const auto validation = normalize_native_run_spec(candidate);
    result.validation = validation;
    if (!validation) {
        fail(engine, NativeFailure{NativeFailureCode::InvalidSpecification,
                                   NativeFailureOperation::Configure});
        render(engine, "native run spec rejected");
        return result;
    }
    if (const auto* completed = std::get_if<NativeCompleted>(&state_)) {
        prior_spec = &completed->spec;
    }
    if (prior_spec) {
        if (candidate.identity.session_key != prior_spec->identity.session_key
            && candidate.identity.session_key != bound_session_key_) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
            render(engine, "native session key cannot change on a reused host");
            return result;
        }
        if (candidate.identity.run_number <= consumed_high_water_) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
            render(engine, "native run number must exceed consumed high-water");
            return result;
        }
    }
    auto parsed_session = native_calendar::parse_session(candidate.session, candidate.timezone);
    if (!parsed_session) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
        render(engine, "native calendar parse failed at configure");
        return result;
    }
    if (!candidate.timeframe_undetected) {
        auto parsed_input = native_calendar::parse_timeframe(candidate.input_tf);
        auto parsed_script = native_calendar::parse_timeframe(candidate.script_tf);
        if (!parsed_input || !parsed_script) {
            fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
            render(engine, "native calendar parse failed at configure");
            return result;
        }
        input_tf_ = std::move(*parsed_input);
        script_tf_ = std::move(*parsed_script);
    } else {
        input_tf_ = native_calendar::Timeframe{};
        script_tf_ = native_calendar::Timeframe{};
    }
    intrabar_tf_.reset();
    if (const auto* lower = candidate.intrabar.lower()) {
        auto parsed_intrabar = native_calendar::parse_timeframe(lower->tf);
        if (!parsed_intrabar) {
            fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
            render(engine, "native intrabar timeframe parse failed at configure");
            return result;
        }
        intrabar_tf_ = std::move(*parsed_intrabar);
    }
    calendar_ = std::move(*parsed_session);
    reset_calendar_memos();
    pairing_ = candidate.timeframe_undetected ? native_calendar::TimeframeCompatibility{}
                                              : native_calendar::compatibility(input_tf_, script_tf_);
    // Direct native FX setup is per-ready-spec as before. C/C++ staged ingress
    // persists across a completed/aborted handle and is reapplied by the next
    // provider begin, so it must survive that provider's configure call.
    if (!staged_ingress_fx_) staged_fx_curve_.reset();
    spec_bar_digests_.reset();
    leave_running();
    state_ = NativeReady{std::move(candidate)};
    result.status = NativeSetupStatus::Applied;
    engine.last_error_.clear();
    return result;
}

NativeFxCurveSetupResult NativeExecutionConsumer::configure_fx_curve(
        const NativeFxCurve& curve) {
    NativeFxCurveSetupResult result;
    if (!std::holds_alternative<NativeReady>(state_)) {
        result.validation = {NativeFxCurveError::WrongPhase, 0};
        return result;
    }

    result.validation = validate_native_fx_curve(curve);
    if (result.validation.error != NativeFxCurveError::None) return result;

    if (curve.effective_from_ms.empty()) {
        staged_fx_curve_.reset();
        result.status = NativeSetupStatus::Applied;
        return result;
    }

    try {
        std::optional<NativeFxCurve> replacement;
        replacement.emplace(curve);
        staged_fx_curve_.swap(replacement);
    } catch (...) {
        result.validation = {NativeFxCurveError::AllocationFailure, 0};
        return result;
    }
    result.status = NativeSetupStatus::Applied;
    return result;
}

bool NativeExecutionConsumer::begin_ready(BacktestEngine& engine, NativeRunPhase phase,
                                          int64_t initial_floor_ms) {
    auto* ready = std::get_if<NativeReady>(&state_);
    if (!ready) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native begin requires Ready");
        return false;
    }
    NativeRunSpec spec = ready->spec;
    if (bound_session_key_.empty()) bound_session_key_ = spec.identity.session_key;
    else if (spec.identity.session_key != bound_session_key_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native session key already bound");
        return false;
    }
    if (spec.identity.run_number <= consumed_high_water_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native run number does not exceed high-water");
        return false;
    }
    consumed_high_water_ = spec.identity.run_number;
    if (!apply_spec(engine, spec)) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Begin});
        render(engine, "native calendar apply failed");
        return false;
    }
    // R5 lane F3 (E23): the continuation names the zone by its identity, and a
    // zone whose resources cannot be read back -- one past any zone file's
    // size, or a POSIX default-DST rule with no posixrules to read -- has none
    // to name. Such a run is refused, never run under an identity-less hash
    // that a run over different zone data would share.
    if (!tz_identity_) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Begin});
        render(engine, "native timezone identity cannot be derived from its zone data");
        return false;
    }
    // The run's latch dies here and its logs below: a view of it goes first.
    drop_continuation_view();
    engine.reset_run_state();
    // RULING A48: one generic capability, wired once per run. A host that
    // declares ownership supplies the closing-row magnitudes; the kernel then
    // keeps no excursion model of its own for this run.
    if (auto* excursion_owner = native_host(engine)) {
        if (excursion_owner->owns_lot_excursions()) {
            engine.lot_excursion_hook_ =
                [excursion_owner](const ClosedLotExcursionFacts& facts) {
                    return excursion_owner->closed_lot_excursion(facts);
                };
        }
    }
    engine.diag_input_bars_processed_ = 0;
    engine.diag_script_bars_processed_ = 0;
    // The report's "was a magnifier active for this run": exactly when the
    // spec declares an intrabar path, which is when this consumer walks one
    // (driver_statistics_.intrabar_path_enabled below). Reporting only; a
    // host that keeps its own notion of the flag rewrites it in its callbacks.
    engine.bar_magnifier_enabled_ = !spec.intrabar.is_none();
    requests_.reset(spec.identity);
    clear_cohort_target_cache();
    terminal_receipt_high_water_ = 0;
    next_timeline_ordinal_ = 1;
    decision_floor_ms_ = initial_floor_ms;
    has_floor_ = true;
    input_mode_ = InputMode::Unselected;
    current_frame_.reset();
    callback_phase_ = CallbackPhase::None;
    pre_open_birth_point_ordinal_ = 0;
    pre_open_birth_time_ms_ = 0;
    pre_open_births_.clear();
    applied_notifications_.clear();
    notification_head_ = 0;
    consuming_request_ = false;
    draining_notifications_ = false;
    next_interval_index_ = 0;
    current_input_open_.reset();
    observed_input_cursor_.reset();
    next_tradable_synthesis_cursor_.reset();
    last_accepted_input_.reset();
    last_observed_slot_open_.reset();
    last_finalized_input_.reset();
    last_tick_sequence_ = 0;
    has_tick_sequence_ = false;
    script_ = ScriptBucket{};
    has_forming_ = false;
    has_last_price_ = false;
    // Stream print state is run-scoped: a reused host must not carry the
    // previous run's last print into the next run's decision coordinates
    // (source_price_time/effective_time fallbacks) or its continuation digest.
    last_price_ = 0.0;
    last_print_time_ms_ = 0;
    driver_log_.clear();
    account_log_.clear();
    history_digest_.reset();
    driver_digest_.reset();
    account_digest_.reset();
    precommit_digest_.reset();
    margin_liquidation_.reset();
    has_margin_path_ = false;
    margin_path_bar_ = Bar{};
    margin_path_high_first_ = false;
    margin_point_ordinal_ = 0;
    margin_point_calls_ = 0;
    risk_ = RiskLedger{};
    driver_statistics_ = NativeDriverStatistics{};
    driver_statistics_.intrabar_path_enabled = !spec.intrabar.is_none();
    clear_partial();
    calculating_bar_ = Bar{};
    calculating_bar_has_ = false;
    point_epoch_ = 0;
    recalc_epoch_ = 0;
    recalc_epoch_count_ = 0;
    recalculations_ = 0;
    recalculations_skipped_ = 0;
    callback_context_ = NativeDecisionContext{};
    callback_context_.driver_statistics = driver_statistics_;
    input_callback_context_.reset();
    input_callback_bar_.reset();
    tick_callback_context_.reset();
    tick_callback_bar_.reset();
    // The begin input, borrowed for the length of this call, is consumed once
    // by the series registration at the end of this function.
    const Bar* const begin_bars = begin_bars_;
    const int begin_n = begin_n_;
    const bool begin_is_stream = begin_is_stream_;
    begin_bars_ = nullptr;
    begin_n_ = 0;
    begin_is_stream_ = false;
    state_ = NativeRunning{std::move(spec), phase};
    cache_running_policy();
    if (!check_abort_or_projection(engine, NativeFailureOperation::Begin)) return false;
    // The previous run's declared series are torn down BEFORE the host's
    // run-begin callback: whatever evaluator states that callback registers
    // -- even ones identical to the kernel's last registration, as a host
    // moving a series from the kernel's drive to its own does -- are the
    // host's and are never mistaken for the kernel's tail. This run's
    // declaration is registered after the callback (below).
    clear_timeframe_subscriptions(engine);
    if (auto* host = native_host(engine)) {
        in_callback_ = true;
        in_run_begin_ = true;
        try {
            host->on_native_run_begin();
        } catch (const std::exception& e) {
            in_callback_ = false;
            in_run_begin_ = false;
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, e.what());
            return false;
        } catch (...) {
            in_callback_ = false;
            in_run_begin_ = false;
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, "native callback exception");
            return false;
        }
        in_callback_ = false;
        in_run_begin_ = false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Callback)) return false;
    }
    // Declared higher-timeframe series are wired LAST, after the host's own
    // run-begin work. A host that registers its evaluators there (a generated
    // configure_security_evaluators() opens with security_eval_states_.clear())
    // would otherwise erase the kernel's registration, and a host that names
    // its series through declare_timeframe_subscriptions() has only just named
    // them -- the staged spec the registration reads back is the list that
    // call left. Installing their authoritative bars still goes through the
    // ordinary public feed setter, whose in-run mutation refusal is inert for
    // exactly this kernel-owned wiring (refuse_source_mutation).
    if (const auto* running = spec_ptr()) {
        if (!begin_timeframe_subscriptions(engine, *running, begin_bars, begin_n,
                                           begin_is_stream)) {
            return false;
        }
    }
    return true;
}

bool NativeExecutionConsumer::preflight_bars(BacktestEngine& engine, const Bar* bars, int n,
                                             bool stream, bool preserve_status) {
    const auto* spec = spec_ptr();
    if (!spec) {
        present_refusal(engine, "native preflight requires a staged spec");
        return false;
    }
    const auto result = preflight_native_inputs(
        *spec, bars, n,
        stream ? NativeInputPolicy::StreamWarmup : NativeInputPolicy::Batch);
    if (result) return true;
    const int status_before = engine.last_run_status_;
    const auto refuse = [&](const char* text) {
        present_refusal(engine, text);
        if (preserve_status) engine.last_run_status_ = status_before;
    };
    const auto indexed = [&](const char* field, const char* detail) {
        std::string message = "bar[" + std::to_string(result.index) + "]." + field;
        if (detail) message += detail;
        refuse(message.c_str());
    };
    const auto timestamped = [&](const char* detail) {
        indexed("timestamp", detail);
    };
    switch (result.error) {
    case NativeInputPreflightError::NullArray:
        // The warmup stage names itself so an invalid warmup request still
        // carries the warmup word through the generic field/index renderer.
        refuse(stream ? "native warmup bars require a non-null array"
                      : "native bars require a non-null array");
        break;
    case NativeInputPreflightError::InvalidCount:
        refuse(stream ? "native warmup bar count is invalid"
                      : "native bar count is invalid");
        break;
    case NativeInputPreflightError::StructuralInvalid:
        if (bars != nullptr && result.index >= 0 && result.index < n) {
            const Bar& bar = bars[result.index];
            if (!std::isfinite(bar.open)) {
                indexed("open", " must be finite");
            } else if (!std::isfinite(bar.high)) {
                indexed("high", " must be finite");
            } else if (!std::isfinite(bar.low)) {
                indexed("low", " must be finite");
            } else if (!std::isfinite(bar.close)) {
                indexed("close", " must be finite");
            } else if (bar.open < 0.0 && stream) {
                indexed("open", " must be non-negative");
            } else if (bar.high < 0.0 && stream) {
                indexed("high", " must be non-negative");
            } else if (bar.low < 0.0 && stream) {
                indexed("low", " must be non-negative");
            } else if (bar.close < 0.0 && stream) {
                indexed("close", " must be non-negative");
            } else if (bar.low > std::min(bar.open, bar.close)) {
                indexed("low", " must not exceed open or close");
            } else if (bar.high < std::max(bar.open, bar.close)) {
                indexed("high", " must not be below open or close");
            } else {
                indexed("volume", " must be non-negative finite or NaN (unavailable)");
            }
        } else {
            refuse("native bar failed structural validation");
        }
        break;
    case NativeInputPreflightError::Unaligned:
        timestamped(" is not aligned to the configured calendar");
        break;
    case NativeInputPreflightError::OffGridLabel:
        if (spec->slot_label_policy == NativeSlotLabelPolicy::Canonical) {
            refuse(
                "native confirmed bar timestamp is not a canonical slot label");
        } else {
            timestamped(" is not a canonical slot label");
        }
        break;
    case NativeInputPreflightError::NotStrictlyIncreasing:
        timestamped(" must be strictly increasing");
        break;
    case NativeInputPreflightError::OverlappingSlot:
        timestamped(" overlaps the previous input slot");
        break;
    case NativeInputPreflightError::InSessionGap:
        timestamped(" follows an in-session gap");
        break;
    case NativeInputPreflightError::CalendarFailure:
        refuse("native calendar parse failed during input preflight");
        break;
    case NativeInputPreflightError::TimestampDeltaOverflow:
        timestamped(" delta exceeds int64 range");
        break;
    case NativeInputPreflightError::None:
        break;
    }
    return false;
}

bool NativeExecutionConsumer::preflight_intrabar_path(BacktestEngine& engine) {
    const auto* spec = spec_ptr();
    if (!spec || spec->intrabar.is_none() || spec->intrabar.synthesized_path()) return true;
    const auto& lower = *spec->intrabar.lower();
    if (!intrabar_tf_ || lower.bars.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        present_refusal(engine, "native intrabar path timeframe or bar count is invalid");
        return false;
    }
    NativeRunSpec path_spec = *spec;
    path_spec.input_tf = lower.tf;
    path_spec.script_tf = lower.tf;
    const auto result = preflight_native_inputs(
        path_spec, lower.bars.empty() ? nullptr : lower.bars.data(),
        static_cast<int>(lower.bars.size()), NativeInputPolicy::Batch);
    if (result) return true;
    present_refusal(engine, "native intrabar path failed validation");
    return false;
}

uint64_t NativeExecutionConsumer::take_ordinal(BacktestEngine&) {
    const uint64_t ordinal = next_timeline_ordinal_;
    if (ordinal == 0 || ordinal == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native timeline ordinal exhausted");
    }
    ++next_timeline_ordinal_;
    return ordinal;
}

void NativeExecutionConsumer::raise_floor(int64_t t) {
    if (!has_floor_ || t > decision_floor_ms_) {
        decision_floor_ms_ = t;
        has_floor_ = true;
    }
}

NativeCoordinate NativeExecutionConsumer::coordinate_from(
        const native_calendar::NativeInterval& interval,
        int index, int64_t effective,
        NativePriceProvenance provenance,
        NativePathPhase phase) const {
    NativeCoordinate c;
    c.interval_index = index;
    c.open_ms = interval.open_ms;
    c.eligible_open_ms = interval.eligible_open_ms;
    c.last_traded_close_ms = interval.last_traded_close_ms;
    c.next_period_open_ms = interval.next_period_open_ms;
    c.next_input_open_ms = interval.next_input_open_ms;
    c.effective_time_ms = effective;
    c.source_price_time_ms = effective;
    c.provenance = provenance;
    c.path_phase = phase;
    return c;
}

void NativeExecutionConsumer::sync_history_digest(std::size_t count) const noexcept {
    const auto& hist = requests_.history();
    count = std::min(count, hist.size());
    if (history_digest_.count > count) history_digest_.reset();
    if (history_digest_.count == count) return;
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.h = history_digest_.h;
    for (std::size_t i = history_digest_.count; i < count; ++i) {
        hash_command(f, hist[i]);
    }
    history_digest_.h = f.h;
    history_digest_.count = count;
}

void NativeExecutionConsumer::sync_driver_digest(std::size_t count) const noexcept {
    count = std::min(count, driver_log_.size());
    if (driver_digest_.count > count) driver_digest_.reset();
    for (std::size_t index = driver_digest_.count; index < count; ++index) {
        fold_driver_digest(driver_log_[index]);
    }
}

void NativeExecutionConsumer::fold_driver_digest(const NativeDriverPoint& point) const noexcept {
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.h = driver_digest_.h;
    hash_driver_point(f, point);
    driver_digest_.h = f.h;
    ++driver_digest_.count;
}

void NativeExecutionConsumer::fold_account_digest(const NativeAccountObservation& row) const noexcept {
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.h = account_digest_.h;
    hash_account_row(f, row);
    account_digest_.h = f.h;
    ++account_digest_.count;
}

void NativeExecutionConsumer::record_driver(const NativeDriverPoint& point) {
    driver_log_.push_back(point);
    // The driver log is an owning readback surface. Its continuation digest is
    // queried only by native-state consumers, so append the point now and
    // fold the derived digest lazily in continuation_hash() rather than at
    // every ordinary source-route waypoint.
}

void NativeExecutionConsumer::reserve_driver_log(std::size_t expected_points) {
    // The driver log alone. The command history is sized by pump_batch from
    // what the batch records; a reservation made here before begin_ready
    // never reached a run anyway, since begin_ready resets the request core.
    if (expected_points > driver_log_.max_size()) {
        throw std::length_error("native driver-log capacity exhausted");
    }
    if (expected_points > driver_log_.capacity()) driver_log_.reserve(expected_points);
}

void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
    // RULING A48: a host that declared ownership of per-lot excursion
    // accounting samples its own lots, so the kernel keeps no excursion
    // model for this run — neither at a matched trigger price nor at any
    // point of the delivered path.
    if (engine.lot_excursion_hook_) return;
    if (!std::isfinite(price)) return;
    for (auto& lot : engine.pyramid_entries_) {
        const bool is_long = engine.position_side_ == PositionSide::LONG;
        const double fav = (is_long ? (price - lot.price) : (lot.price - price)) * lot.qty;
        lot.max_runup = std::max(lot.max_runup, std::max(0.0, fav));
        lot.max_drawdown = std::max(lot.max_drawdown, std::max(0.0, -fav));
    }
}

native_order::DriverEligibilityClass NativeExecutionConsumer::classify_driver(
        const NativeDriverPoint& point, bool continuous) const noexcept {
    using Class = native_order::DriverEligibilityClass;
    switch (point.coordinate.provenance) {
    case NativePriceProvenance::CurrentExecution:
        return Class::CurrentExecution;
    case NativePriceProvenance::ObservedPrint:
        return Class::ObservedPrint;
    case NativePriceProvenance::CarriedOpen:
        return Class::CarriedOpen;
    case NativePriceProvenance::AfterCalculationClose:
        return input_mode_ == InputMode::ObservedTicks
            ? Class::TickAfterCalculation
            : Class::ConfirmedAfterCalculationClose;
    case NativePriceProvenance::ModeledOHLCOpen:
        return Class::ConfirmedOpen;
    case NativePriceProvenance::Confirmed:
        return continuous ? Class::ConfirmedExcursion : Class::ConfirmedOpen;
    case NativePriceProvenance::ModeledOHLCClose:
        return continuous ? Class::ConfirmedExcursion : Class::ConfirmedAfterCalculationClose;
    case NativePriceProvenance::PartialFinalized:
    case NativePriceProvenance::Calculation:
        return Class::ConfirmedAfterCalculationClose;
    }
    return static_cast<Class>(255); // unknown is never an observed print
}

native_order::MatchCursor NativeExecutionConsumer::make_cursor(
        const NativeDriverPoint& point, double t) const noexcept {
    native_order::MatchCursor cursor;
    cursor.point = point.coordinate;
    cursor.t = t;
    return cursor;
}

native_order::PositionIdentity NativeExecutionConsumer::read_position(
        const BacktestEngine& engine) const {
    if (engine.position_side_ == PositionSide::FLAT || engine.pyramid_entries_.empty()
        || engine.position_cycle_seq_ <= 0) {
        return native_order::PositionFlat{};
    }
    const native_order::Side side = engine.position_side_ == PositionSide::SHORT
        ? native_order::Side::Short : native_order::Side::Long;
    return native_order::PositionNonflat{engine.position_cycle_seq_, side};
}

native_order::OpeningObservation NativeExecutionConsumer::read_opening(
        const BacktestEngine& engine, const native_order::RequestHandle& opening,
        int64_t cycle) const {
    native_order::OpeningObservation out;
    out.queried_opening = opening;
    out.queried_cycle = cycle;
    out.current_position = read_position(engine);
    out.has_live_matching_lot = false;
    if (opening.run != requests_.identity()) return out;
    if (cycle != engine.position_cycle_seq_ || engine.position_cycle_seq_ <= 0) return out;
    if (engine.position_side_ == PositionSide::FLAT) return out;
    for (const auto& lot : engine.pyramid_entries_) {
        if (lot.entry_incarnation == opening.incarnation) {
            out.has_live_matching_lot = true;
            break;
        }
    }
    return out;
}

native_order::TargetObservation NativeExecutionConsumer::read_target(
        const BacktestEngine& engine, const native_order::LiveRequest* live) const {
    native_order::TargetObservation out;
    std::vector<native_order::RequestHandle> handles;
    read_target_into(engine, live, out, handles);
    return out;
}

// read_target, written over `out` (and the cohort's lot handles over
// `handles`) so a caller that keeps both reuses their capacity (R5 lane
// PERF-L5). Every field of `out` is rewritten.
void NativeExecutionConsumer::read_target_into(
        const BacktestEngine& engine, const native_order::LiveRequest* live,
        native_order::TargetObservation& out,
        std::vector<native_order::RequestHandle>& handles) const {
    out.current_position = read_position(engine);
    out.opening.reset();
    out.openings.clear();
    if (!live) return;
    if (const auto* opening = std::get_if<native_order::OpeningClose>(&live->authority)) {
        out.opening = read_opening(engine, opening->opening, opening->cycle);
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&live->authority)) {
        read_openings_into(engine, openings->openings, openings->cycle, out.openings);
    } else if (const auto* cohort = std::get_if<native_order::CohortClose>(&live->authority)) {
        const auto* position = std::get_if<native_order::PositionNonflat>(&out.current_position);
        if (!position) return;
        handles.clear();
        handles.reserve(engine.pyramid_entries_.size());
        for (const auto& lot : engine.pyramid_entries_) {
            native_order::RequestHandle handle{requests_.identity(), lot.entry_incarnation};
            if (requests_.cohort_contains(cohort->cohort, handle)) {
                handles.push_back(std::move(handle));
            }
        }
        read_openings_into(engine, handles, position->cycle, out.openings);
    } else if (const auto* bind = std::get_if<native_order::BindOpenings>(&live->request().owner)) {
        read_openings_into(engine, bind->openings, bind->cycle, out.openings);
    } else if (const auto* bind = std::get_if<native_order::BindOpening>(&live->request().owner)) {
        out.opening = read_opening(engine, bind->opening, bind->cycle);
    }
}

// cohort_side and request_is_buy for the matcher, reading the target into its
// scratch (match_target_): the same values, without a target built per read.
// match_path and observe_trails are the only callers, and neither holds the
// scratch across a call to either.
std::optional<native_order::Side> NativeExecutionConsumer::scratch_cohort_side(
        const BacktestEngine& engine, const native_order::LiveRequest& live) {
    if (!std::holds_alternative<native_order::CohortClose>(live.authority)) {
        return std::nullopt;
    }
    read_target_into(engine, &live, match_target_, match_target_handles_);
    for (const auto& opening : match_target_.openings) {
        if (!opening.has_live_matching_lot) continue;
        if (const auto* position = std::get_if<native_order::PositionNonflat>(
                &opening.current_position)) {
            return position->side;
        }
    }
    return std::nullopt;
}

bool NativeExecutionConsumer::scratch_request_is_buy(
        const BacktestEngine& engine, const native_order::LiveRequest& live) {
    if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        const auto side = scratch_cohort_side(engine, live);
        return side && *side == native_order::Side::Short;
    }
    return requests_.working_is_buy(live);
}

const native_order::TargetObservation* NativeExecutionConsumer::cached_cohort_target(
        const BacktestEngine& engine, const native_order::LiveRequest& live) {
    const auto* spec = spec_ptr();
    if (!spec || !spec->intrabar.is_none()
        || !std::holds_alternative<native_order::CohortClose>(live.authority)) {
        return nullptr;
    }
    for (std::size_t index = 0; index < cohort_target_cache_size_; ++index) {
        if (cohort_target_cache_[index].handle == live.handle())
            return &cohort_target_cache_[index].target;
    }
    if (cohort_target_cache_size_ == cohort_target_cache_.size()) return nullptr;
    auto& entry = cohort_target_cache_[cohort_target_cache_size_++];
    entry.handle = live.handle();
    entry.target = read_target(engine, &live);
    return &entry.target;
}

void NativeExecutionConsumer::clear_cohort_target_cache() noexcept {
    cohort_target_cache_size_ = 0;
}

void NativeExecutionConsumer::retarget_cohort_target_cache(
        const native_order::RequestHandle& predecessor,
        const native_order::RequestHandle& successor) noexcept {
    for (std::size_t index = 0; index < cohort_target_cache_size_; ++index) {
        if (cohort_target_cache_[index].handle == predecessor)
            cohort_target_cache_[index].handle = successor;
    }
}

std::optional<native_order::Side> NativeExecutionConsumer::cohort_side(
        const BacktestEngine& engine, const native_order::LiveRequest& live) const {
    if (!std::holds_alternative<native_order::CohortClose>(live.authority)) {
        return std::nullopt;
    }
    const auto target = read_target(engine, &live);
    for (const auto& opening : target.openings) {
        if (!opening.has_live_matching_lot) continue;
        if (const auto* position = std::get_if<native_order::PositionNonflat>(
                &opening.current_position)) {
            return position->side;
        }
    }
    return std::nullopt;
}

bool NativeExecutionConsumer::request_is_buy(
        const BacktestEngine& engine, const native_order::LiveRequest& live) const {
    if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        const auto side = cohort_side(engine, live);
        return side && *side == native_order::Side::Short;
    }
    return requests_.working_is_buy(live);
}

native_order::CommandContext NativeExecutionConsumer::make_command_context(
        const BacktestEngine& engine, const native_order::Request& request,
        native_order::CommandSurface surface) const {
    native_order::CommandContext ctx;
    // A request submitted by the generic pre-open provider is born at this
    // open rather than at the prior decision floor.  Its one-point delivery
    // authorization is carried separately and consumed by match_discrete.
    const bool applied_point_is_current = callback_phase_ == CallbackPhase::Applied
        && current_frame_
        && (current_frame_->point.decision.coordinate.effective_time_ms >= decision_floor()
            || (remaining_path_coordinate(current_frame_->point.decision.coordinate)
                && !std::holds_alternative<native_order::Market>(request.trigger)));
    ctx.decision_time_ms = (callback_phase_ == CallbackPhase::PreOpen || applied_point_is_current)
            && current_frame_
        ? current_frame_->point.decision.coordinate.effective_time_ms : decision_floor();
    if (const auto* spec = spec_ptr()) {
        ctx.quantity_grid = spec->quantity_grid;
        // A Sized request freezes its sizing price here when it asked for the
        // signal rule, and a Sized{AtAcceptance} additionally freezes its
        // units against that price, the marked equity there and the activated
        // FX. AtMatch{Resolved} and any command outside a callback frame stay
        // deferred to the candidate.
        const auto* native_sized = std::get_if<native_order::Sized>(&request.intent);
        if (native_sized) {
            if (const auto point = current_execution_point()) {
                const double price = sizing_point_price(*spec, *native_sized, point->price);
                if (native_sized->price != native_order::SizePrice::Resolved) {
                    ctx.sizing_price = price;
                }
                if (native_sized->time == native_order::SizeTime::AtAcceptance) {
                    // E21: the freeze and its placement gate are one
                    // measurement at the acceptance point, taken at that
                    // point's rate: the equity basis and the units'
                    // conversion here, the gate's notional, marked equity and
                    // percent ticket below. The rate is threaded, never the
                    // clock -- in an applied callback drained at its script
                    // bar's calculation the engine presents that bar's close,
                    // not the print the fill landed on, and nothing here
                    // reads or moves that clock. With no curve every instant
                    // has one rate.
                    const double fx = engine.account_currency_fx_at(
                        point->decision.coordinate.effective_time_ms);
                    ctx.sizing_units = sized_basis_units(
                        *native_sized, price, engine.marked_equity_at(price, fx), fx, *spec);
                    // The frozen quantity is an admission input at placement,
                    // not only at the candidate.
                    if (ctx.sizing_units) {
                        ctx.sizing_admissible = admit_placement_units(
                            engine, *native_sized, *ctx.sizing_units, price, fx);
                    }
                }
            }
        } else if (const auto* reduce = std::get_if<native_order::Reduce>(&request.intent)) {
            // A ScopeBasis::AtAcceptance fraction freezes the exposure of the
            // scope it is about to bind to, measured exactly as the candidate
            // would measure it.
            const auto* fraction = std::get_if<native_order::ScopeFraction>(&reduce->size);
            if (fraction && fraction->basis == native_order::ScopeBasis::AtAcceptance) {
                ctx.sizing_scope = placement_scope_units(engine, request);
            }
        }
        ctx.price_tick = spec->price_tick;
    }
    ctx.surface = surface;
    if (const auto* bind = std::get_if<native_order::BindOpening>(&request.owner)) {
        ctx.opening = read_opening(engine, bind->opening, bind->cycle);
    } else if (const auto* bind = std::get_if<native_order::BindOpenings>(&request.owner)) {
        ctx.openings = read_openings(engine, bind->openings, bind->cycle);
    }
    return ctx;
}

void NativeExecutionConsumer::refresh_target_scalars(
        const BacktestEngine& engine, native_order::TargetObservation& target) const noexcept {
    target.current_position = read_position(engine);
    refresh_openings(engine, target.openings);
    if (!target.opening) return;
    target.opening->current_position = target.current_position;
    target.opening->has_live_matching_lot = false;
    const auto& opening = target.opening->queried_opening;
    if (opening.run != requests_.identity()) return;
    if (target.opening->queried_cycle != engine.position_cycle_seq_
        || engine.position_cycle_seq_ <= 0) {
        return;
    }
    if (engine.position_side_ == PositionSide::FLAT) return;
    for (const auto& lot : engine.pyramid_entries_) {
        if (lot.entry_incarnation == opening.incarnation) {
            target.opening->has_live_matching_lot = true;
            break;
        }
    }
}

bool NativeExecutionConsumer::admit_opening_inspect(
        const BacktestEngine& engine, double resolved_price, double fx,
        const execution::SettlementInspection& inspect,
        bool skip_initial_margin,
        native_order::MatchRejectReason* reason) const {
    const auto* spec = spec_ptr();
    if (!spec || !inspect.would_open) return true;
    // L9: a blocking risk limit refuses every opening of the run, ahead of the
    // per-opening caps, which keep their own reasons and semantics. Reduces
    // and the kernel's own requests never reach this gate: it tests
    // would_open only.
    if (risk_blocked()) {
        if (reason) *reason = native_order::MatchRejectReason::RiskLimit;
        return false;
    }
    const auto mask = static_cast<uint32_t>(spec->allowed_open_directions);
    if (inspect.incoming_short && (mask & 2u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (!inspect.incoming_short && (mask & 1u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (spec->max_abs_units && inspect.resulting_abs_units > *spec->max_abs_units) {
        if (reason) *reason = native_order::MatchRejectReason::MaxAbsUnits;
        return false;
    }
    if (spec->max_open_lots && inspect.resulting_lot_count > *spec->max_open_lots) {
        if (reason) *reason = native_order::MatchRejectReason::MaxOpenLots;
        return false;
    }
    // L4: a declared margin model replaces the one-scalar gate for this run
    // with its own per-side initial fraction. The two spellings are mutually
    // exclusive by configure, so exactly one of these branches can apply.
    // A model's per-side fraction of ZERO is the maintenance-only spelling:
    // the side enforces no opening requirement here (the host owns opening
    // admission) and keeps only its liquidation. That is the `> 0.0` below,
    // which is also what the placement gate reads through this same function.
    const double fraction = spec->margin
        ? (inspect.incoming_short ? spec->margin->initial_short : spec->margin->initial_long)
        : (spec->initial_margin_fraction ? *spec->initial_margin_fraction : 0.0);
    // Every FX-bearing term converts at `fx`, the rate of the gate's own point
    // (E21): the inspection's notional and ticket were taken at it by the
    // caller, and the equity is marked at it here.
    if (!skip_initial_margin && fraction > 0.0) {
        const double equity = engine.marked_equity_at(resolved_price, fx) - inspect.current_ticket;
        const double required = inspect.resulting_abs_notional * fraction;
        if (!std::isfinite(equity) || !std::isfinite(required) || required > equity) {
            if (reason) *reason = native_order::MatchRejectReason::InitialMargin;
            return false;
        }
    }
    return true;
}

const NativeMarginModel* NativeExecutionConsumer::margin_model() const noexcept {
    const auto* spec = spec_ptr();
    return spec && spec->margin ? &*spec->margin : nullptr;
}

std::optional<double> NativeExecutionConsumer::maintenance_fraction(
        bool short_side) const noexcept {
    const auto* margin = margin_model();
    if (!margin) return std::nullopt;
    return short_side ? margin->maintenance_short : margin->maintenance_long;
}

// equity(P) = base + dir * (P * Q - W) * pv * fx and requirement(P) =
// Q * P * pv * fx * m are both affine in P, so the breach has exactly one
// solution unless their slopes coincide: (m - dir) == 0, which is a LONG at
// full maintenance. That case has no liquidation price at all and is reported
// as such rather than as a very large or negative one.
std::optional<double> NativeExecutionConsumer::liquidation_level(
        const BacktestEngine& engine, double fx) const {
    if (engine.position_side_ == PositionSide::FLAT || engine.pyramid_entries_.empty()) {
        return std::nullopt;
    }
    const bool short_side = engine.position_side_ == PositionSide::SHORT;
    const auto fraction = maintenance_fraction(short_side);
    if (!fraction) return std::nullopt;
    const double point_value = engine.syminfo_.pointvalue;
    if (!std::isfinite(point_value) || !(point_value > 0.0)
        || !std::isfinite(fx) || !(fx > 0.0)) {
        return std::nullopt;
    }
    double units = 0.0;
    double cost = 0.0;
    double commissions = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        if (!std::isfinite(lot.qty) || lot.qty <= 0.0 || !std::isfinite(lot.price)) {
            return std::nullopt;
        }
        units += lot.qty;
        cost += lot.price * lot.qty;
        commissions += engine.open_entry_commission(lot);
    }
    if (!(units > 0.0) || !std::isfinite(commissions)) return std::nullopt;
    const double direction = short_side ? -1.0 : 1.0;
    const double slope = *fraction - direction;
    if (!std::isfinite(slope) || std::abs(slope) < 1e-12) return std::nullopt;
    // RealizedOnly solves from closed money alone: the open entries' paid
    // commission is a cost the level does not answer for. MarkedEquity keeps
    // the intercept of marked_equity(), which is this run's default.
    const auto* margin = margin_model();
    const bool realized_only = margin
        && margin->level_base == NativeLiquidationLevelBase::RealizedOnly;
    const double base = engine.initial_capital_ + engine.net_profit_sum_
        - (realized_only ? 0.0 : commissions);
    const double level = (base - direction * cost * point_value * fx)
        / (units * point_value * fx * slope);
    if (!std::isfinite(level)) return std::nullopt;
    return level;
}

// E3: the account-currency rate ONE margin check point converts at. Every
// term of the maintenance test is FX-bearing -- the requirement
// Q * P * pv * fx * m, the marked equity it is compared with, and the level
// solved from the two -- so the rate has to be the one the declared curve has
// in force at the instant being checked. BacktestEngine::active_account_
// currency_fx() reads the PRESENTED bar clock instead, which is a later
// instant whenever the walk has moved past the point: an applied fill drained
// at its script bar's calculation is presented that bar's close coordinate
// while its cursor still stands at the opening print it filled on, and a
// requirement measured there is the rate of a bar the account has not reached.
//
// The rest of the kernel already converts at the cursor -- a Sized request
// freezes its units at account_currency_fx_at(the acceptance coordinate), an
// execution term records active_fx at its own cursor -- so this is the margin
// model joining a rule the run already keeps everywhere else. Inert by
// construction for a run that declares no timestamped curve: with no curve
// account_currency_fx_at() answers the run's scalar rate at every instant.
double NativeExecutionConsumer::margin_check_fx(
        const BacktestEngine& engine,
        const native_order::MatchCursor& cursor) const noexcept {
    return engine.account_currency_fx_at(cursor.point.effective_time_ms);
}

// The equity one maintenance test is made against. The default IS the
// account's marked equity -- BacktestEngine::marked_equity_at at the check
// point's own rate, the engine's one implementation of it -- which has already
// been reduced by the open entries' commission; MarkedEquityBeforeOpenCommission
// adds that term back, for a broker that does not charge a still-open entry's
// fee against the margin equity. Neither spelling books anything: this is one
// term of one comparison.
double NativeExecutionConsumer::margin_equity(const BacktestEngine& engine,
                                              double mark, double fx) const {
    const double equity = engine.marked_equity_at(mark, fx);
    const auto* margin = margin_model();
    if (!margin || !std::isfinite(equity)
        || margin->basis != NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission) {
        return equity;
    }
    double commissions = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        commissions += engine.open_entry_commission(lot);
    }
    if (!std::isfinite(commissions)) return std::numeric_limits<double>::quiet_NaN();
    return equity + commissions;
}

// The host's gate over one kernel check point, consulted before anything is
// evaluated there. A refused point is not a no-op check: nothing is measured,
// nothing is re-armed and nothing is withdrawn, so the margin state stays
// exactly as the last admitted point left it.
bool NativeExecutionConsumer::margin_check_admitted(
        const BacktestEngine& engine, NativeMarginCheckKind kind,
        const native_order::MatchCursor& cursor, double mark) const {
    const auto* host = native_host(engine);
    if (!host) return true;
    NativeMarginCheckPoint point;
    point.kind = kind;
    point.position = position(engine);
    point.mark = mark;
    point.cursor = cursor;
    point.liquidation_resting = margin_liquidation_.has_value()
        && requests_.find_live(margin_liquidation_->handle) != nullptr;
    return host->margin_check_allowed(point);
}

// The most adverse price the modeled script path still reaches after `phase`,
// including the cursor price itself so a point with no remaining waypoint
// still has a finite mark. A run with an intrabar path has no whole-bar
// waypoint model here: it re-evaluates at each delivered sample instead.
double NativeExecutionConsumer::margin_sizing_price(
        bool short_side, NativePathPhase phase, double fallback) const noexcept {
    if (!has_margin_path_) return fallback;
    const NativePathPhase order[4] = {
        NativePathPhase::Open,
        margin_path_high_first_ ? NativePathPhase::High : NativePathPhase::Low,
        margin_path_high_first_ ? NativePathPhase::Low : NativePathPhase::High,
        NativePathPhase::Close,
    };
    const double prices[4] = {
        margin_path_bar_.open,
        margin_path_high_first_ ? margin_path_bar_.high : margin_path_bar_.low,
        margin_path_high_first_ ? margin_path_bar_.low : margin_path_bar_.high,
        margin_path_bar_.close,
    };
    int current = -1;
    for (int index = 0; index < 4; ++index) {
        if (order[index] == phase) {
            current = index;
            break;
        }
    }
    if (current < 0) return fallback;
    double adverse = fallback;
    for (int index = current + 1; index < 4; ++index) {
        const double price = prices[index];
        if (!std::isfinite(price) || !(price > 0.0)) continue;
        if (!std::isfinite(adverse) || (short_side ? price > adverse : price < adverse)) {
            adverse = price;
        }
    }
    return adverse;
}

std::optional<double> NativeExecutionConsumer::margin_call_units(
        const BacktestEngine& engine, double mark, const native_order::MatchCursor& cursor,
        NativeMarginCheckKind kind, double* out_equity, double* out_required) const {
    const auto* margin = margin_model();
    if (!margin || engine.position_side_ == PositionSide::FLAT) return std::nullopt;
    const bool short_side = engine.position_side_ == PositionSide::SHORT;
    const auto fraction = maintenance_fraction(short_side);
    if (!fraction) return std::nullopt;
    const auto book = position(engine);
    const double held = std::abs(book.signed_units);
    const double point_value = engine.syminfo_.pointvalue;
    const double fx = margin_check_fx(engine, cursor);
    if (!(held > 0.0) || !std::isfinite(mark) || !(mark > 0.0)
        || !std::isfinite(point_value) || !(point_value > 0.0)
        || !std::isfinite(fx) || !(fx > 0.0)) {
        return std::nullopt;
    }
    const double unit_margin = mark * point_value * fx * *fraction;
    double equity = margin_equity(engine, mark, fx);
    double required = held * unit_margin;
    const auto* host = native_host(engine);
    // The host's money rule, BEFORE the breach test. The kernel still owns
    // the mechanism; the two numbers it compares are where brokers differ, so
    // a host may raise a call this kernel would not make or veto one it
    // would. Nonfinite answers are refused rather than silently ignored.
    bool forced = false;
    if (host && std::isfinite(unit_margin) && unit_margin > 0.0) {
        NativeMarginRequirementView view;
        view.kind = kind;
        view.position = book;
        view.mark = mark;
        view.equity = equity;
        view.required = required;
        view.cursor = cursor;
        if (const auto decision = host->resolve_margin_requirement(view)) {
            if (!std::isfinite(decision->required) || !std::isfinite(decision->equity)) {
                return std::nullopt;
            }
            equity = decision->equity;
            required = decision->required;
            forced = decision->force_breach;
        }
    }
    if (out_equity) *out_equity = equity;
    if (out_required) *out_required = required;
    if (!std::isfinite(unit_margin) || !(unit_margin > 0.0)
        || !std::isfinite(equity) || !std::isfinite(required)
        || !(required > equity || forced)) {
        return std::nullopt;
    }
    const double restore = (required - equity) / unit_margin;
    double units = 0.0;
    switch (margin->sizing) {
    case NativeLiquidationSizing::RestoreMinimum:
        units = restore;
        break;
    case NativeLiquidationSizing::ShortfallMultiple:
        units = restore * margin->shortfall_multiple;
        break;
    case NativeLiquidationSizing::Flatten:
        units = held;
        break;
    }
    if (!std::isfinite(units) || !(units > 0.0)) {
        // A breach the kernel's own numbers do not see has no restore of its
        // own. Only a forced one survives it, and then the units hook is the
        // whole sizing authority.
        if (!forced) return std::nullopt;
        units = 0.0;
    }
    if (units > 0.0) {
        units = std::min(units, held);
        // A restore below the broker's minimum trade is not a broker action:
        // the position is closed instead of nibbled.
        if (margin->liquidation_min_units && units < *margin->liquidation_min_units) {
            units = held;
        }
    }
    // The host sees the kernel's own facts and has the last word on the size.
    if (host) {
        NativeMarginCallView view;
        view.position = book;
        view.mark = mark;
        view.equity = equity;
        view.required = required;
        view.cursor = cursor;
        if (const auto override_units = host->resolve_margin_call_units(view)) {
            if (!std::isfinite(*override_units) || !(*override_units > 0.0)) return std::nullopt;
            units = std::min(*override_units, held);
        }
    }
    if (!std::isfinite(units) || !(units > 0.0)) return std::nullopt;
    return units;
}

void NativeExecutionConsumer::withdraw_margin_liquidation(BacktestEngine& engine) {
    if (!margin_liquidation_) return;
    const auto handle = margin_liquidation_->handle;
    margin_liquidation_.reset();
    if (!requests_.find_live(handle)) return;
    auto prepared = requests_.prepare_cancel(handle, next_timeline_ordinal_,
                                             native_order::CancelReason::Superseded);
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Settlement});
        render(engine, "native liquidation withdrawal produced no preparation");
        return;
    }
    const auto predicted = prepared.predicted_event_id();
    const auto status = prepared.predicted().status;
    auto installed = requests_.install_cancel(std::move(prepared));
    if (const auto* error = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Settlement,
                                   predicted.ordinal, static_cast<uint32_t>(*error)});
        render(engine, "native liquidation withdrawal install failed");
        return;
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::CancelResult>>(installed);
    note_terminal_events(ok.events);
    clear_cohort_target_cache();
    catch_up_timeline();
    if (status == native_order::CancelStatus::Cancelled) {
        drain_parent_terminal(engine, predicted, handle, NativeFailureOperation::Settlement);
    }
}

// A kernel-originated reduction. A finite positive `level` rests it as a
// Stop; a nonpositive one makes it a market command the caller executes at the
// current point. A slice that would take the whole book becomes a Flatten, so
// a quantity grid can never refuse the broker's own liquidation. `origin`
// names which kernel authority issued it — the margin model's liquidation or
// (L9) the risk block's flatten — and only a resting liquidation is retained
// as the run's live margin request.
bool NativeExecutionConsumer::kernel_submit_liquidation(
        BacktestEngine& engine, double level, double units,
        std::int64_t decision_time_ms, native_order::RequestHandle* out_handle,
        native_order::RequestOrigin origin, const char* label, const char* comment) {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    const bool resting = std::isfinite(level) && level > 0.0;
    const auto book = position(engine);
    const double held = std::abs(book.signed_units);
    native_order::Request request;
    const double slack = std::max(1e-12, held * 1e-12);
    if (units >= held - slack) {
        request.intent = native_order::Flatten{};
    } else {
        double sized = units;
        if (spec->quantity_grid && *spec->quantity_grid > 0.0) {
            // A quantity that is ALREADY on the broker's grid is left exactly
            // as it is. Reconstructing it as floor(q/step)*step can move it a
            // whole step down, because the quotient of two on-grid binary64
            // values need not be the integer it represents: 0.0392/0.0001 is
            // 391.99999999999994, whose floor is 391, i.e. one lot short of
            // the size the sizing policy (or the host's units hook) decided
            // on. Only an off-grid quantity is floored onto the grid.
            if (!native_order::quantity_on_grid(sized, *spec->quantity_grid)) {
                sized = std::floor(units / *spec->quantity_grid) * *spec->quantity_grid;
                if (!native_order::quantity_on_grid(sized, *spec->quantity_grid)) return false;
            }
        }
        if (!std::isfinite(sized) || !(sized > 0.0)) return false;
        request.intent = native_order::Reduce{native_order::ExplicitUnits{sized}};
    }
    // A caller that names the ticket owns it -- (L9) the risk block's flatten
    // is not a margin liquidation and carries its own id. Otherwise the broker
    // names its own liquidation ticket where it has one, and the kernel's
    // constants are the default. This is the id a reporting layer classifies
    // the closed row by, so it must be the broker's.
    const auto* ticket = margin_model();
    request.label = label != nullptr
        ? label
        : (ticket && !ticket->liquidation_label.empty()
               ? ticket->liquidation_label : kNativeLiquidationLabel);
    request.comment = comment != nullptr
        ? comment
        : (ticket && !ticket->liquidation_comment.empty()
               ? ticket->liquidation_comment : kNativeLiquidationComment);
    if (resting) request.trigger = native_order::Stop{level};
    // The kernel is born AT the point it decided on, exactly as a request
    // submitted from the pre-open callback is. It never inherits the input
    // path's already-raised future decision floor, which would make it
    // ineligible for the very bar it was armed for.
    native_order::CommandContext ctx;
    ctx.decision_time_ms = decision_time_ms;
    ctx.quantity_grid = spec->quantity_grid;
    ctx.surface = native_order::CommandSurface::General;
    native_order::PreparedSubmit prepared;
    try {
        prepared = requests_.prepare_submit(request, ctx, engine.next_order_incarnation_,
                                            next_timeline_ordinal_, origin);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Settlement});
        render(engine, e.what());
        return false;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Settlement});
        render(engine, "native liquidation submit produced no preparation");
        return false;
    }
    auto installed = requests_.install_submit(std::move(prepared));
    if (const auto* error = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Settlement,
                                   0, static_cast<uint32_t>(*error)});
        render(engine, "native liquidation submit install failed");
        return false;
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::SubmitResult>>(installed);
    note_terminal_events(ok.events);
    clear_cohort_target_cache();
    catch_up_timeline();
    if (ok.result.status != native_order::SubmitStatus::Accepted || !ok.result.handle) {
        return false;
    }
    ++engine.next_order_incarnation_;
    record_pre_open_birth(request, *ok.result.handle);
    if (resting && origin == native_order::RequestOrigin::KernelLiquidation) {
        margin_liquidation_ = MarginLiquidation{*ok.result.handle, level, units};
    }
    if (out_handle) *out_handle = *ok.result.handle;
    return true;
}

void NativeExecutionConsumer::maintain_margin_liquidation(
        BacktestEngine& engine, const native_order::MatchCursor& cursor,
        NativePathPhase phase, double fallback_price, NativeMarginCheckKind kind) {
    const auto* margin = margin_model();
    if (!margin || failed() || consuming_request_) return;
    // PathAdverseExtremeMark measures the same breach at the same mark and
    // rests AT that mark: the period-mark broker, which never solves a level.
    const bool at_mark = margin->check == NativeLiquidationCheck::PathAdverseExtremeMark;
    if (!at_mark && margin->check != NativeLiquidationCheck::PathAdverseExtreme) return;
    try {
        const bool short_side = engine.position_side_ == PositionSide::SHORT;
        const double mark = margin_sizing_price(short_side, phase, fallback_price);
        // The host's gate owns the whole point, including its withdrawal.
        if (!margin_check_admitted(engine, kind, cursor, mark)) return;
        if (failed()) return;
        if (engine.position_side_ == PositionSide::FLAT) {
            withdraw_margin_liquidation(engine);
            return;
        }
        std::optional<double> level;
        if (!at_mark) {
            // A slope that solves no level rests nothing here -- there is no
            // price to rest at. A host that needs a call where the level does
            // not exist (a LONG at full maintenance) selects the mark check,
            // whose breach test and requirement hook run either way.
            level = liquidation_level(engine, margin_check_fx(engine, cursor));
            if (!level || !std::isfinite(*level) || !(*level > 0.0)) {
                withdraw_margin_liquidation(engine);
                return;
            }
        }
        const auto units = margin_call_units(engine, mark, cursor, kind, nullptr, nullptr);
        if (!units) {
            withdraw_margin_liquidation(engine);
            return;
        }
        // Bound the re-arm chain at one driver point (see the member note).
        if (cursor.point.ordinal == margin_point_ordinal_ && margin_point_calls_ >= 8) {
            withdraw_margin_liquidation(engine);
            return;
        }
        // The resting price: the adverse mark itself under the mark check,
        // the solved level otherwise. margin_call_units has already refused a
        // nonfinite or nonpositive mark.
        const double resting = at_mark ? mark : *level;
        if (margin_liquidation_ && requests_.find_live(margin_liquidation_->handle) != nullptr
            && native_matching::double_bits(margin_liquidation_->level)
                == native_matching::double_bits(resting)
            && native_matching::double_bits(margin_liquidation_->units)
                == native_matching::double_bits(*units)) {
            return;
        }
        withdraw_margin_liquidation(engine);
        if (failed()) return;
        kernel_submit_liquidation(engine, resting, *units, cursor.point.effective_time_ms);
    } catch (const std::exception& e) {
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement,
                                       cursor.point.ordinal});
            render(engine, e.what());
        }
    } catch (...) {
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement,
                                       cursor.point.ordinal});
            render(engine, "native margin maintenance callback exception");
        }
    }
}

void NativeExecutionConsumer::calculation_margin_check(
        BacktestEngine& engine, const NativeCoordinate& calc, double mark) {
    const auto* margin = margin_model();
    if (!margin || failed() || consuming_request_) return;
    if (margin->check != NativeLiquidationCheck::CalculationOnly) return;
    if (engine.position_side_ == PositionSide::FLAT) return;
    const bool short_side = engine.position_side_ == PositionSide::SHORT;
    if (!maintenance_fraction(short_side)) return;
    native_order::MatchCursor cursor;
    cursor.point = calc;
    std::optional<double> units;
    try {
        if (!margin_check_admitted(engine, NativeMarginCheckKind::Calculation, cursor, mark)) {
            return;
        }
        if (failed()) return;
        units = margin_call_units(engine, mark, cursor, NativeMarginCheckKind::Calculation,
                                  nullptr, nullptr);
    } catch (const std::exception& e) {
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, calc.ordinal});
            render(engine, e.what());
        }
        return;
    }
    if (!units) return;
    native_order::RequestHandle target;
    if (!kernel_submit_liquidation(engine, 0.0, *units, calc.effective_time_ms, &target)) return;
    (void)execute_current(engine, {target, NativeCurrentPriceRule::AsPresented});
}

// MG9: a step of the declared FX curve is a check point of its own. The
// requirement Q * P * pv * fx * m is FX-bearing, so the account can cross its
// maintenance line with every price exactly where it was; the bar open and
// the post-fill re-arm would only see that at the NEXT point of their own.
//
// The account converts at account_currency_fx_at(effective time), so the rate
// a driver point is walked under is a function of the immutable curve and of
// that point alone. A roll is a point whose rate is not its predecessor's in
// the driver log -- both already durable, digested state, so the detection
// adds none of its own and no continuation identity moves. The point is
// offered immediately before it is matched, where the walk still stands: a
// discrete point at its own price, a segment at its origin with its
// destination among the waypoints that remain, so a level the new rate moved
// inside the segment is rested in time for that same segment to reach it.
//
// N6 took this one check under a temporarily swapped engine clock, because
// the margin model still converted at whatever bar was being presented. E3
// moved every check kind onto its own cursor's rate (margin_check_fx), so the
// roll needs no clock of its own any more: the point it is offered at IS the
// instant it is measured at, and nothing but the margin state can differ from
// a walk that was never offered the point. Inert without a staged curve and a
// declared model; a CalculationOnly model returns from
// maintain_margin_liquidation before its gate, as at every other path point.
void NativeExecutionConsumer::fx_roll_margin_check(
        BacktestEngine& engine, const NativeDriverPoint& point,
        bool continuous, double from_price) {
    if (!staged_fx_curve_ || margin_model() == nullptr || failed()) return;
    std::size_t walked = driver_log_.size();
    if (walked != 0
        && driver_log_[walked - 1].coordinate.ordinal == point.coordinate.ordinal) {
        --walked;
    }
    if (walked == 0) return;
    const double before = engine.account_currency_fx_at(
        driver_log_[walked - 1].coordinate.effective_time_ms);
    const double after = engine.account_currency_fx_at(point.coordinate.effective_time_ms);
    if (native_matching::double_bits(before) == native_matching::double_bits(after)) return;
    NativePathPhase standing = point.coordinate.path_phase;
    double price = point.raw_price;
    if (continuous) {
        price = from_price;
        const NativePathPhase order[4] = {
            NativePathPhase::Open,
            margin_path_high_first_ ? NativePathPhase::High : NativePathPhase::Low,
            margin_path_high_first_ ? NativePathPhase::Low : NativePathPhase::High,
            NativePathPhase::Close,
        };
        for (int index = 1; index < 4; ++index) {
            if (order[index] == point.coordinate.path_phase) standing = order[index - 1];
        }
    }
    maintain_margin_liquidation(engine, make_cursor(point, 0.0), standing, price,
                                NativeMarginCheckKind::FxRoll);
}

std::optional<std::size_t> NativeExecutionConsumer::record_margin_call(
        BacktestEngine& engine, const native_order::ExecutionAppliedEvent& applied,
        const native_order::DefinitionRef& definition, double position_before,
        double position_after) {
    if (!definition || definition->origin != native_order::RequestOrigin::KernelLiquidation) {
        return std::nullopt;
    }
    native_order::MarginCallEvent event;
    event.definition = definition;
    event.applied = native_order::EventId{applied.handle().run, applied.ordinal};
    event.cursor = applied.cursor;
    event.side = position_before < 0.0 ? native_order::Side::Short : native_order::Side::Long;
    event.mark = applied.resolved_price;
    // The receipt reports the equity on the model's own basis, which is the
    // number the check was made on, at the rate in force where the fill that
    // booked it landed (E3). Default basis = marked_equity().
    const double fx = margin_check_fx(engine, applied.cursor);
    event.equity = margin_equity(engine, applied.resolved_price, fx);
    const auto fraction = maintenance_fraction(position_before < 0.0);
    event.required = fraction
        ? std::abs(position_after) * applied.resolved_price * engine.syminfo_.pointvalue
              * fx * *fraction
        : 0.0;
    if (const auto level = liquidation_level(engine, fx)) event.liquidation_price = *level;
    event.units = applied.closed_units;
    event.position_before = position_before;
    event.position_after = position_after;
    if (applied.cursor.point.ordinal == margin_point_ordinal_) {
        ++margin_point_calls_;
    } else {
        margin_point_ordinal_ = applied.cursor.point.ordinal;
        margin_point_calls_ = 1;
    }
    const std::size_t index = requests_.history().size();
    auto prepared = requests_.prepare_margin_call(event, next_timeline_ordinal_);
    if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
        fail_preparation(engine, *error, NativeFailureOperation::Settlement);
        return std::nullopt;
    }
    auto* mutation = std::get_if<native_order::PreparedMutation>(&prepared);
    if (!mutation || !install_mutation(engine, std::move(*mutation),
                                       NativeFailureOperation::Settlement, applied.ordinal)) {
        return std::nullopt;
    }
    return index;
}

// ── L9 generic risk limits ───────────────────────────────────────────────
// Everything below is inert for a spec that leaves NativeRunSpec::risk unset,
// which is every source-projected spec: risk_limits() answers nullptr, the
// ledger stays at its zero, no event is appended and admission is unchanged.

const NativeRiskLimits* NativeExecutionConsumer::risk_limits() const noexcept {
    const auto* spec = spec_ptr();
    return spec && spec->risk ? &*spec->risk : nullptr;
}

bool NativeExecutionConsumer::risk_blocked() const noexcept {
    if (risk_limits() == nullptr) return false;
    if (risk_.run_block) return true;
    return risk_.day_block.has_value() && risk_.has_day
        && risk_.day_ordinal == risk_.day_block_day;
}

std::optional<std::int64_t> NativeExecutionConsumer::risk_day(
        std::int64_t timestamp_ms) const {
    const auto* risk = risk_limits();
    if (!risk) return std::nullopt;
    try {
        if (risk->day_basis == NativeRiskDay::CalendarDayInTimezone) {
            if (!risk_day_calendar_) return std::nullopt;
            return native_calendar::session_day_ordinal(*risk_day_calendar_, timestamp_ms);
        }
        return native_calendar::session_day_ordinal(calendar_, timestamp_ms);
    } catch (...) {
        // A day the calendar cannot key is not a new day: the ledger keeps
        // the one it is on rather than inventing an ordinal.
        return std::nullopt;
    }
}

void NativeExecutionConsumer::risk_roll_day(const BacktestEngine& engine, std::int64_t day,
                                            double mark) {
    if (risk_limits() == nullptr) return;
    // The closing day's own realized result decides the streak: a loss
    // extends it, a profit restarts it, and a day that realized nothing
    // leaves it exactly where it was.
    if (risk_.has_day) {
        const double realized = engine.net_profit_sum_ - risk_.day_open_realized;
        if (std::isfinite(realized) && realized < 0.0) {
            if (risk_.consecutive_loss_days < std::numeric_limits<std::uint32_t>::max()) {
                ++risk_.consecutive_loss_days;
            }
        } else if (std::isfinite(realized) && realized > 0.0) {
            risk_.consecutive_loss_days = 0;
        }
    }
    risk_.day_ordinal = day;
    risk_.has_day = true;
    risk_.fills_today = 0;
    risk_.day_open_realized = engine.net_profit_sum_;
    const double equity = engine.marked_equity(mark);
    risk_.day_open_equity = std::isfinite(equity)
        ? equity : engine.initial_capital_ + engine.net_profit_sum_;
}

void NativeExecutionConsumer::risk_note_fill(const BacktestEngine& engine,
                                             const NativeCoordinate& coordinate,
                                             double price) {
    if (risk_limits() == nullptr) return;
    if (const auto day = risk_day(coordinate.open_ms)) {
        if (!risk_.has_day || *day != risk_.day_ordinal) risk_roll_day(engine, *day, price);
    }
    if (risk_.fills_today < std::numeric_limits<std::uint64_t>::max()) ++risk_.fills_today;
    const double equity = engine.marked_equity(price);
    if (std::isfinite(equity) && (!risk_.has_peak || equity > risk_.peak_equity)) {
        risk_.peak_equity = equity;
        risk_.has_peak = true;
    }
}

void NativeExecutionConsumer::risk_evaluate(BacktestEngine& engine,
                                            const NativeCurrentPointView& point,
                                            CallbackPhase phase, bool owns_frame) {
    const auto* risk = risk_limits();
    if (!risk || failed() || consuming_request_) return;
    const double price = point.price;
    if (const auto day = risk_day(point.decision.coordinate.open_ms)) {
        if (!risk_.has_day || *day != risk_.day_ordinal) risk_roll_day(engine, *day, price);
    }
    const double equity = engine.marked_equity(price);
    if (std::isfinite(equity) && (!risk_.has_peak || equity > risk_.peak_equity)) {
        risk_.peak_equity = equity;
        risk_.has_peak = true;
    }
    // A run block is terminal: nothing further is measured or reported.
    if (risk_.run_block || !std::isfinite(equity)) return;
    const bool day_blocked = risk_.day_block.has_value() && risk_.has_day
        && risk_.day_ordinal == risk_.day_block_day;
    // Declaration order, one breach per evaluation: the first limit that is
    // reached owns this point's event.
    if (risk->max_drawdown && risk_.has_peak) {
        const double limit = risk->max_drawdown->percent
            ? risk_.peak_equity * risk->max_drawdown->value / 100.0
            : risk->max_drawdown->value;
        const double observed = risk_.peak_equity - equity;
        if (std::isfinite(limit) && limit > 0.0 && observed >= limit) {
            risk_fire(engine, native_order::RiskLimitKind::MaxDrawdown, limit, observed,
                      point, phase, owns_frame);
            return;
        }
    }
    if (risk->max_intraday_loss && risk_.has_day && !day_blocked) {
        const double limit = risk->max_intraday_loss->percent
            ? risk_.day_open_equity * risk->max_intraday_loss->value / 100.0
            : risk->max_intraday_loss->value;
        const double observed = risk_.day_open_equity - equity;
        if (std::isfinite(limit) && limit > 0.0 && observed >= limit) {
            risk_fire(engine, native_order::RiskLimitKind::MaxIntradayLoss, limit, observed,
                      point, phase, owns_frame);
            return;
        }
    }
    if (risk->max_consecutive_loss_days
        && risk_.consecutive_loss_days >= *risk->max_consecutive_loss_days) {
        risk_fire(engine, native_order::RiskLimitKind::MaxConsecutiveLossDays,
                  static_cast<double>(*risk->max_consecutive_loss_days),
                  static_cast<double>(risk_.consecutive_loss_days), point, phase, owns_frame);
        return;
    }
    if (risk->max_fills_per_day && risk_.has_day && !day_blocked
        && risk_.fills_today >= *risk->max_fills_per_day) {
        risk_fire(engine, native_order::RiskLimitKind::MaxFillsPerDay,
                  static_cast<double>(*risk->max_fills_per_day),
                  static_cast<double>(risk_.fills_today), point, phase, owns_frame);
    }
}

void NativeExecutionConsumer::risk_fire(BacktestEngine& engine,
                                        native_order::RiskLimitKind kind, double limit,
                                        double observed, const NativeCurrentPointView& point,
                                        CallbackPhase phase, bool owns_frame) {
    const auto* risk = risk_limits();
    if (!risk) return;
    // The block latches BEFORE anything else happens at this point, so the
    // kernel's own flatten cannot re-enter this breach through the fill it
    // drives, and a further evaluation at the same point sees a blocked run.
    const bool day_scope = kind == native_order::RiskLimitKind::MaxIntradayLoss
        || kind == native_order::RiskLimitKind::MaxFillsPerDay;
    if (day_scope) {
        risk_.day_block = kind;
        risk_.day_block_day = risk_.day_ordinal;
    } else {
        risk_.run_block = kind;
    }
    native_order::NativeRiskEvent event;
    event.kind = kind;
    event.limit = limit;
    event.observed = observed;
    event.day_ordinal = risk_.has_day ? risk_.day_ordinal : 0;
    event.cursor.point = point.decision.coordinate;
    auto prepared = requests_.prepare_risk_event(event, next_timeline_ordinal_);
    if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
        fail_preparation(engine, *error, NativeFailureOperation::Settlement);
        return;
    }
    auto* mutation = std::get_if<native_order::PreparedMutation>(&prepared);
    if (!mutation || !install_mutation(engine, std::move(*mutation),
                                       NativeFailureOperation::Settlement,
                                       point.decision.coordinate.ordinal)) {
        return;
    }
    if (risk->action != NativeRiskAction::FlattenAndBlock) return;
    const double held = std::abs(position(engine).signed_units);
    if (!(held > 0.0)) return;
    // One kernel-originated Flatten, executed at this very point inside the
    // breach's own frame. finish_callback closes that frame and delivers the
    // fill, exactly as the callback that could have driven it would. At the
    // close calculation the caller still holds its own frame and closes it
    // afterwards, so this borrows it rather than nesting a second one.
    if (owns_frame) enter_point_frame(engine, point, phase);
    native_order::RequestHandle target;
    if (kernel_submit_liquidation(engine, 0.0, held,
                                  point.decision.coordinate.effective_time_ms, &target,
                                  native_order::RequestOrigin::KernelRisk,
                                  kNativeRiskLabel, kNativeRiskComment)) {
        (void)execute_current(engine, {target, NativeCurrentPriceRule::AsPresented});
    }
    if (owns_frame) finish_callback(engine, point.decision.coordinate.ordinal);
}

void NativeExecutionConsumer::fail_preparation(
        BacktestEngine& engine, const native_order::PreparationError& error,
        NativeFailureOperation operation) {
    NativeFailureCode code = NativeFailureCode::Contract;
    switch (error.code) {
    case native_order::CoreFailure::NonrepresentableQuantity:
    case native_order::CoreFailure::UnrepresentableReservation:
    case native_order::CoreFailure::InvalidProposal:
    case native_order::CoreFailure::InvalidScope:
        code = NativeFailureCode::SettlementFailure;
        break;
    case native_order::CoreFailure::MissingObservation:
    case native_order::CoreFailure::ObservationMismatch:
        code = NativeFailureCode::SettlementFailure;
        break;
    default:
        code = NativeFailureCode::Contract;
        break;
    }
    NativeFailure failure{code, operation, error.cause.ordinal,
                          static_cast<uint32_t>(error.code)};
    failure.context = native_failure_context_in_run(
            requests_.identity(), &error.cause, &error.target, nullptr);
    fail(engine, failure);
    render(engine, "native working-request preparation failed");
}

void NativeExecutionConsumer::catch_up_timeline() noexcept {
    if (requests_.history().empty()) return;
    const uint64_t last = command_ordinal(requests_.history().back());
    if (last >= next_timeline_ordinal_) next_timeline_ordinal_ = last + 1;
}

bool NativeExecutionConsumer::install_mutation(
        BacktestEngine& engine, native_order::PreparedMutation&& prepared,
        NativeFailureOperation operation, uint64_t ordinal) {
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation, ordinal});
        render(engine, "native mutation install missing preparation");
        return false;
    }
    const auto result = requests_.install_mutation(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&result)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation, ordinal,
                                   static_cast<uint32_t>(*err)});
        render(engine, "native mutation install failed");
        return false;
    }
    note_terminal_events(std::get<native_order::Installed>(result).events);
    clear_cohort_target_cache();
    // The continuation digest is a readback value.  Keep its append cursor
    // lazy: a source host that projects its own broker hash must not re-fold
    // every immutable history event at each ordinary command boundary.
    // continuation_hash() synchronizes it before exposing the value.
    catch_up_timeline();
    return true;
}

bool NativeExecutionConsumer::install_execution(
        BacktestEngine& engine, native_order::PreparedExecution&& prepared,
        const native_order::CommittedExecutionFacts& facts, uint64_t ordinal) {
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Settlement, ordinal});
        render(engine, "native execution install missing preparation");
        return false;
    }
    const auto result = requests_.install_execution(std::move(prepared), facts);
    if (const auto* err = std::get_if<native_order::InstallError>(&result)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Settlement, ordinal,
                                   static_cast<uint32_t>(*err)});
        render(engine, "native execution install failed after settlement");
        return false;
    }
    note_terminal_events(std::get<native_order::Installed>(result).events);
    clear_cohort_target_cache();
    catch_up_timeline();
    return true;
}

void NativeExecutionConsumer::drain_dependency_queue(
        BacktestEngine& engine,
        std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds,
        NativeFailureOperation operation) {
    if (failed() || seeds.empty()) return;
    try {
        struct Item {
            uint64_t cause_ordinal = 0;
            native_order::EventId cause;
            native_order::RequestHandle child;
        };
        std::vector<Item> queue;
        auto less_item = [](const Item& a, const Item& b) {
            if (a.cause_ordinal != b.cause_ordinal) return a.cause_ordinal < b.cause_ordinal;
            return a.child.incarnation < b.child.incarnation;
        };
        auto enqueue_children = [&](const native_order::EventId& cause,
                                    const native_order::RequestHandle& parent,
                                    std::size_t sort_from) {
            auto children = requests_.waiting_children(parent);
            for (auto& child : children) {
                queue.push_back(Item{cause.ordinal, cause, std::move(child)});
            }
            if (sort_from < queue.size()) {
                std::sort(queue.begin() + static_cast<std::ptrdiff_t>(sort_from), queue.end(),
                          less_item);
            }
        };
        for (auto& seed : seeds) {
            enqueue_children(seed.first, seed.second, queue.size());
        }
        std::sort(queue.begin(), queue.end(), less_item);
        for (std::size_t i = 0; i < queue.size() && !failed(); ++i) {
            const native_order::EventId cause = queue[i].cause;
            const native_order::RequestHandle child = queue[i].child;
            auto prep = requests_.prepare_parent_terminal(cause, child, next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, operation);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation), operation,
                                               cause.ordinal)) {
                return;
            }
            if (requests_.find_live(child) == nullptr && !requests_.history().empty()) {
                native_order::EventId next_cause = cause;
                next_cause.ordinal = command_ordinal(requests_.history().back());
                enqueue_children(next_cause, child, i + 1);
            }
        }
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, operation,
                                   seeds.empty() ? 0 : seeds.front().first.ordinal});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::drain_parent_terminal(
        BacktestEngine& engine, const native_order::EventId& cause,
        const native_order::RequestHandle& parent,
        NativeFailureOperation operation) {
    // Most source replacements have no WaitForApplied descendants.  Avoid
    // constructing the dependency queue (and its seed allocation) for that
    // ordinary no-op while retaining the exact queue path once a child exists.
    if (!requests_.has_waiting_children(parent)) return;
    std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds;
    try {
        seeds.push_back({cause, parent});
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, operation, cause.ordinal});
        render(engine, e.what());
        return;
    }
    drain_dependency_queue(engine, std::move(seeds), operation);
}

void NativeExecutionConsumer::drain_after_applied(
        BacktestEngine& engine, const native_order::EventId& applied,
        const native_order::RequestHandle& filler) {
    try {
        std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds;
        auto note_absent = [&](const native_order::RequestHandle& parent) {
            if (requests_.find_live(parent) != nullptr || requests_.history().empty()) return;
            native_order::EventId cause = applied;
            cause.ordinal = command_ordinal(requests_.history().back());
            seeds.push_back({std::move(cause), parent});
        };

        const auto recipients = requests_.group_recipients(applied);
        for (const auto& recipient : recipients) {
            if (failed()) return;
            auto prep = requests_.prepare_group_effect(applied, recipient, next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal)) {
                return;
            }
            note_absent(recipient);
        }

        const auto children = requests_.waiting_children(filler);
        // The arm of an anchored leg reads the run's price tick (the ladder a
        // rounded anchor snaps to) exactly as acceptance does through
        // CommandContext::price_tick, and offers the host its one level
        // restatement; the core itself stays host-free and sees both as
        // values. A throwing hook latches CallbackException exactly like
        // resolve_execution_terms and the arm below is not installed.
        native_order::ArmContext arm;
        if (const auto* spec = spec_ptr()) arm.price_tick = spec->price_tick;
        auto* host = native_host(engine);
        if (host) {
            arm.resolve_level = [this, &engine, host, &applied](
                    const native_order::LiveRequest& leg,
                    const native_order::ExecutionAppliedEvent& owner_fill,
                    native_order::Side leg_side, double offset,
                    double kernel_level) -> std::optional<double> {
                NativeAnchoredLevelView view;
                view.owner = owner_fill.handle();
                view.owner_applied_ordinal = applied.ordinal;
                view.owner_lot_incarnation = owner_fill.opened_lot_incarnation;
                view.owner_fill_price = owner_fill.resolved_price;
                view.owner_cursor = owner_fill.cursor;
                view.leg = leg.handle();
                view.leg_side = leg_side;
                view.trigger = std::holds_alternative<native_order::Stop>(leg.request().trigger)
                    ? NativeAnchoredTrigger::Stop
                    : std::holds_alternative<native_order::Trail>(leg.request().trigger)
                        ? NativeAnchoredTrigger::TrailArm
                        : NativeAnchoredTrigger::Limit;
                view.offset = offset;
                view.price_tick = spec_ptr() ? spec_ptr()->price_tick : 0.0;
                view.kernel_level = kernel_level;
                try {
                    return host->resolve_anchored_level(view);
                } catch (const std::exception& e) {
                    fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal});
                    render(engine, e.what());
                } catch (...) {
                    fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal});
                    render(engine, "native anchored level callback exception");
                }
                return std::nullopt;
            };
        }
        for (const auto& child : children) {
            if (failed()) return;
            std::optional<native_order::OpeningObservation> observation;
            if (requests_.find_live(child)) {
                observation = read_opening(engine, filler, engine.position_cycle_seq_);
            }
            auto prep = requests_.prepare_owner_applied(applied, child, observation,
                                                        next_timeline_ordinal_, arm);
            if (failed()) return;
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal)) {
                return;
            }
            note_absent(child);
        }

        const auto bound = requests_.bound_close_handles();
        for (const auto& handle : bound) {
            if (failed()) return;
            const auto* live = requests_.find_live(handle);
            auto target = read_target(engine, live);
            auto prep = requests_.prepare_bound_expiry(applied, handle, target,
                                                       next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal)) {
                return;
            }
            note_absent(handle);
        }

        // The queue drains the waiting children of its seeds: with no other
        // seed and none waiting on the filler it would drain nothing, so the
        // filler's seed is not built (R5 lane PERF-L5), as drain_parent_terminal
        // already does for a single parent.
        if (seeds.empty() && !requests_.has_waiting_children(filler)) return;
        seeds.push_back({applied, filler});
        drain_dependency_queue(engine, std::move(seeds), NativeFailureOperation::Settlement);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Settlement, applied.ordinal});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::observe_trails(
        BacktestEngine& engine, const NativeDriverPoint& point,
        const native_order::MatchCursor& cursor, bool continuous, double price) {
    if (!std::isfinite(price) || failed()) return;
    const auto* spec = spec_ptr();
    if (!spec) return;
    // A path with only market/limit/stop requests has no trailing state to
    // advance.  In particular, do not snapshot every live cohort request or
    // resolve its target side at each waypoint merely to discover that fact.
    const auto& live_requests = requests_.live();
    if (std::none_of(live_requests.begin(), live_requests.end(), [](const auto& live) {
            return std::holds_alternative<native_order::TrailTrack>(live.trigger_state);
        })) {
        return;
    }
    native_order::EvaluationContext evaluation;
    evaluation.cursor = cursor;
    evaluation.driver_class = classify_driver(point, continuous);
    evaluation.existing_matching_bit = point.matching;
    // The book as it stood on entry, in the consumer's scratch: match_path is
    // the only caller, and nothing below reaches either function again.
    auto& handles = match_trail_handles_;
    handles.clear();
    handles.reserve(live_requests.size());
    for (const auto& live : live_requests) handles.push_back(live.handle());
    for (const auto& handle : handles) {
        if (failed()) return;
        const auto* live = requests_.find_live(handle);
        if (!live) continue;
        evaluation.cohort_side = scratch_cohort_side(engine, *live);
        if (std::holds_alternative<native_order::CohortClose>(live->authority)
            && !evaluation.cohort_side) {
            continue;
        }
        if (!requests_.evaluation_eligible(*live, evaluation)) continue;
        const auto* track = std::get_if<native_order::TrailTrack>(&live->trigger_state);
        if (!track) continue;
        const bool buy = scratch_request_is_buy(engine, *live);
        if (!native_matching::trail_best_improves(track->best, price, buy)) continue;
        native_order::Preparation<native_order::PreparedMutation> prep;
        try {
            prep = requests_.prepare_trigger(
                handle, native_order::ObserveTrailExtremum{cursor, price},
                evaluation.driver_class, next_timeline_ordinal_, evaluation.cohort_side,
                activation_grid(*spec));
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Settlement, cursor.point.ordinal});
            render(engine, e.what());
            return;
        }
        if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
            fail_preparation(engine, *err, NativeFailureOperation::Settlement);
            return;
        }
        if (std::holds_alternative<native_order::NoChange>(prep)) continue;
        auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
        if (!mutation || !install_mutation(engine, std::move(*mutation),
                                           NativeFailureOperation::Settlement,
                                           cursor.point.ordinal)) {
            return;
        }
    }
}

void NativeExecutionConsumer::match_point(BacktestEngine& engine, const NativeDriverPoint& point) {
    match_discrete(engine, point);
}

void NativeExecutionConsumer::match_discrete(BacktestEngine& engine, const NativeDriverPoint& point) {
    match_path(engine, point, false, point.raw_price, point.raw_price);
    if (pre_open_birth_point_ordinal_ == point.coordinate.ordinal) {
        pre_open_birth_point_ordinal_ = 0;
        pre_open_birth_time_ms_ = 0;
        pre_open_births_.clear();
    }
}

bool NativeExecutionConsumer::pre_open_birth_eligible(
        const native_order::RequestHandle& handle, const NativeDriverPoint& point) const noexcept {
    if (pre_open_birth_point_ordinal_ != point.coordinate.ordinal
        || pre_open_birth_time_ms_ != point.coordinate.effective_time_ms
        || point.coordinate.path_phase != NativePathPhase::Open) {
        return false;
    }
    return std::find(pre_open_births_.begin(), pre_open_births_.end(), handle)
        != pre_open_births_.end();
}

void NativeExecutionConsumer::record_pre_open_birth(
        const native_order::Request& request, const native_order::RequestHandle& handle) {
    if (callback_phase_ != CallbackPhase::PreOpen || !current_frame_
        || !std::holds_alternative<native_order::Market>(request.trigger)
        || !std::holds_alternative<native_order::ImmediateRemaining>(request.capacity)
        || current_frame_->point.decision.coordinate.path_phase != NativePathPhase::Open) {
        return;
    }
    pre_open_birth_point_ordinal_ = current_frame_->point.decision.coordinate.ordinal;
    pre_open_birth_time_ms_ = current_frame_->point.decision.coordinate.effective_time_ms;
    if (std::find(pre_open_births_.begin(), pre_open_births_.end(), handle)
        == pre_open_births_.end()) {
        pre_open_births_.push_back(handle);
    }
}

void NativeExecutionConsumer::match_segment(
        BacktestEngine& engine, const NativeDriverPoint& dest, double from_price) {
    match_path(engine, dest, true, from_price, dest.raw_price);
}

std::vector<native_order::OpeningObservation> NativeExecutionConsumer::read_openings(
        const BacktestEngine& engine, const std::vector<native_order::RequestHandle>& handles,
        int64_t cycle) const {
    std::vector<native_order::OpeningObservation> out;
    read_openings_into(engine, handles, cycle, out);
    return out;
}

// read_openings, written over `out` so a caller that keeps it reuses its
// capacity. The rows are ordered by incarnation, ties in handle order: a
// stable sort, whose result is unique. A short list is sorted by stable
// insertion instead of std::stable_sort, whose libstdc++ spelling allocates
// a merge buffer on every call; a long one keeps std::stable_sort.
void NativeExecutionConsumer::read_openings_into(
        const BacktestEngine& engine, const std::vector<native_order::RequestHandle>& handles,
        int64_t cycle, std::vector<native_order::OpeningObservation>& out) const {
    out.clear();
    out.reserve(handles.size());
    for (const auto& handle : handles) {
        native_order::OpeningObservation row;
        row.queried_opening = handle;
        row.queried_cycle = cycle;
        out.push_back(std::move(row));
    }
    const auto by_incarnation = [](const native_order::OpeningObservation& a,
                                   const native_order::OpeningObservation& b) {
        return a.queried_opening.incarnation < b.queried_opening.incarnation;
    };
    constexpr std::size_t kInsertionSortRows = 32;
    if (out.size() <= kInsertionSortRows) {
        for (std::size_t i = 1; i < out.size(); ++i) {
            if (!by_incarnation(out[i], out[i - 1])) continue;
            native_order::OpeningObservation row = std::move(out[i]);
            std::size_t j = i;
            for (; j > 0 && by_incarnation(row, out[j - 1]); --j) out[j] = std::move(out[j - 1]);
            out[j] = std::move(row);
        }
    } else {
        std::stable_sort(out.begin(), out.end(), by_incarnation);
    }
    refresh_openings(engine, out);
}

void NativeExecutionConsumer::refresh_openings(const BacktestEngine& engine,
        std::vector<native_order::OpeningObservation>& openings) const noexcept {
    const auto position = read_position(engine);
    for (auto& row : openings) {
        row.current_position = position;
        row.has_live_matching_lot = false;
    }
    if (engine.position_side_ == PositionSide::FLAT || engine.position_cycle_seq_ <= 0) return;
    for (const auto& lot : engine.pyramid_entries_) {
        auto it = std::lower_bound(openings.begin(), openings.end(), lot.entry_incarnation,
            [](const auto& row, uint64_t incarnation) {
                return row.queried_opening.incarnation < incarnation;
            });
        for (; it != openings.end()
               && it->queried_opening.incarnation == lot.entry_incarnation; ++it) {
            if (it->queried_opening.run == requests_.identity()
                && it->queried_cycle == engine.position_cycle_seq_)
                it->has_live_matching_lot = true;
        }
    }
}

execution::Action NativeExecutionConsumer::narrow_action(
        const native_order::ExecutionPlan& plan) {
    return std::visit([](const auto& payload) -> execution::Action {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, execution::Flatten>
                      || std::is_same_v<T, order_action::Reduce>
                      || std::is_same_v<T, order_action::Transact>) {
            return payload;
        } else {
            throw std::logic_error("native reversal plan reached an action-only settlement");
        }
    }, plan);
}

NativeExecutionTermsFacts NativeExecutionConsumer::build_terms_facts(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const native_order::EvaluationContext& evaluation,
        native_order::NativeCandidatePriceKind price_kind,
        NativeCurrentPriceRule price_rule, double raw_price,
        double default_resolved_price, bool shared_cursor_collision) const {
    NativeExecutionTermsFacts out;
    out.target = live.handle();
    out.definition = live.definition;
    out.cursor = evaluation.cursor;
    out.driver_class = evaluation.driver_class;
    out.trigger_state = live.trigger_state;
    out.remaining = live.remaining;
    out.allowance = evaluation.driver_class == native_order::DriverEligibilityClass::CurrentExecution
        ? native_order::WorkingRequestCore::evaluated_allowance(
              live, evaluation.cursor.point.ordinal)
        : live.allowance;
    out.position = position(engine);
    out.is_buy = std::holds_alternative<native_order::UnboundBookClose>(live.authority)
        ? engine.position_side_ == PositionSide::SHORT : request_is_buy(engine, live);
    out.price_kind = price_kind;
    out.shared_cursor_collision = shared_cursor_collision;
    out.raw_price = raw_price;
    out.default_resolved_price = default_resolved_price;
    out.price_rule = price_rule;
    out.quote_kind = evaluation.driver_class == native_order::DriverEligibilityClass::CurrentExecution
        && current_frame_ ? current_frame_->point.quote_kind
        : NativeCurrentQuoteKind::MarketDecision;
    out.fx_effective_time_ms = evaluation.cursor.point.effective_time_ms;
    out.active_fx = engine.account_currency_fx_at(out.fx_effective_time_ms);
    if (const auto* pending = std::get_if<native_order::PendingDeferred>(&live.pending)) {
        out.pending_group_deduction = pending->total;
    }

    double roster_units = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        roster_units += lot.qty;
        if (!std::isfinite(roster_units)) {
            throw std::logic_error("native terms roster units are not finite");
        }
    }
    const bool opposite = (out.is_buy && engine.position_side_ == PositionSide::SHORT)
        || (!out.is_buy && engine.position_side_ == PositionSide::LONG);
    out.opposite_book_units = opposite ? roster_units : 0.0;

    const auto target = read_target(engine, &live);
    if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
        const execution::OpeningExposure scope{opening->opening.incarnation, opening->cycle};
        out.scope = scope;
        for (const auto& lot : engine.pyramid_entries_) {
            if (lot.entry_incarnation == opening->opening.incarnation) {
                out.scope_exposure_units += lot.qty;
            }
        }
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&live.authority)) {
        native_order::SelectedExposure scope;
        scope.cycle = openings->cycle;
        for (const auto& row : target.openings) {
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        out.scope = scope;
        for (const auto& lot : engine.pyramid_entries_) {
            if (std::find(scope.incarnations.begin(), scope.incarnations.end(),
                          lot.entry_incarnation) != scope.incarnations.end()) {
                out.scope_exposure_units += lot.qty;
            }
        }
    } else if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        native_order::SelectedExposure scope;
        if (const auto* position = std::get_if<native_order::PositionNonflat>(
                &target.current_position)) {
            scope.cycle = position->cycle;
        }
        for (const auto& row : target.openings) {
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        out.scope = scope;
        for (const auto& lot : engine.pyramid_entries_) {
            if (std::find(scope.incarnations.begin(), scope.incarnations.end(),
                          lot.entry_incarnation) != scope.incarnations.end()) {
                out.scope_exposure_units += lot.qty;
            }
        }
    } else {
        out.scope = execution::Book{};
        out.scope_exposure_units = roster_units;
    }

    const auto& trigger = live.request().trigger;
    if (std::holds_alternative<native_order::LimitReady>(live.trigger_state)
        || std::holds_alternative<native_order::StopLimitLive>(live.trigger_state)) {
        if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
            out.trigger_level = limit->price;
        } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
            out.trigger_level = stop_limit->limit;
        } else {
            throw std::logic_error("native limit state has no limit trigger");
        }
    } else if (std::holds_alternative<native_order::StopActive>(live.trigger_state)) {
        const auto* stop = std::get_if<native_order::Stop>(&trigger);
        if (!stop) throw std::logic_error("native stop state has no stop trigger");
        out.trigger_level = stop->price;
    } else if (const auto* active = std::get_if<native_order::TrailActive>(&live.trigger_state)) {
        const auto* trail = std::get_if<native_order::Trail>(&trigger);
        double stop = 0.0;
        if (!trail || !native_matching::checked_trail_stop(
                active->best_at_trigger, trail->offset, out.is_buy, &stop, ladder_tick())) {
            throw std::logic_error("native active trail has no representable trigger");
        }
        out.trigger_level = stop;
    } else if (!std::holds_alternative<native_order::MarketReady>(live.trigger_state)
               && evaluation.driver_class != native_order::DriverEligibilityClass::CurrentExecution) {
        throw std::logic_error("native nonfillable state reached terms coordinator");
    }
    return out;
}

native_order::ExecutionPlan NativeExecutionConsumer::plan_from_terms(
        native_order::HostSizedKind kind, std::optional<native_order::Side> side,
        native_order::OpeningShape shape, double after, double allowance_left,
        double opposite_book_units) {
    if (kind == native_order::HostSizedKind::Close) {
        return order_action::Reduce{std::min(after, allowance_left)};
    }
    if (!side) throw std::logic_error("native host-sized opening has no side");
    const double signed_after = *side == native_order::Side::Long ? after : -after;
    switch (shape) {
    case native_order::OpeningShape::Transact:
        return order_action::Transact{*side == native_order::Side::Long
            ? std::min(after, allowance_left) : -std::min(after, allowance_left)};
    case native_order::OpeningShape::ReverseTo:
        return execution::ReverseTo{signed_after};
    case native_order::OpeningShape::CloseOpposite:
        if (after == opposite_book_units) return execution::Flatten{};
        return order_action::Reduce{after};
    }
    throw std::logic_error("native host-sized opening shape is unknown");
}

NativeExecutionConsumer::ResolvedCandidate NativeExecutionConsumer::inspect_candidate(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const native_order::MatchCursor& cursor, double resolved,
        const native_order::ExecutionPlan* plan_override) const {
    ResolvedCandidate candidate;
    candidate.target = read_target(engine, &live);
    if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
        if (opening->opening.run != requests_.identity())
            throw std::logic_error("native opening scope identity mismatch");
        const execution::OpeningExposure scope{opening->opening.incarnation, opening->cycle};
        candidate.scope = scope;
        candidate.financial_scope = scope;
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&live.authority)) {
        native_order::SelectedExposure scope;
        scope.cycle = openings->cycle;
        scope.incarnations.reserve(candidate.target.openings.size());
        for (const auto& row : candidate.target.openings) {
            if (row.queried_opening.run != requests_.identity())
                throw std::logic_error("native selected scope identity mismatch");
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        candidate.selected = execution::SelectedOpeningSet{scope.cycle, scope.incarnations};
        candidate.scope = std::move(scope);
    } else if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        const auto* position = std::get_if<native_order::PositionNonflat>(
            &candidate.target.current_position);
        if (!position) throw std::logic_error("native cohort selection is flat");
        native_order::SelectedExposure scope;
        scope.cycle = position->cycle;
        scope.incarnations.reserve(candidate.target.openings.size());
        for (const auto& row : candidate.target.openings) {
            if (row.queried_opening.run != requests_.identity()) {
                throw std::logic_error("native cohort scope identity mismatch");
            }
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        if (scope.incarnations.empty()) throw std::logic_error("native cohort selection is empty");
        candidate.selected = execution::SelectedOpeningSet{scope.cycle, scope.incarnations};
        candidate.scope = std::move(scope);
    } else if (!std::holds_alternative<native_order::BookTransaction>(live.authority)
               && !std::holds_alternative<native_order::ArmedTransaction>(live.authority)
               && !std::holds_alternative<native_order::BookClose>(live.authority)
               && !std::holds_alternative<native_order::UnboundBookClose>(live.authority)) {
        throw std::logic_error("native unresolved execution authority");
    }
    if (plan_override) {
        candidate.physical = *plan_override;
    } else if (!std::holds_alternative<native_order::RemainingFlattenAll>(live.remaining)
        && !std::holds_alternative<native_order::Flatten>(live.request().intent)) {
        const auto* remaining = std::get_if<native_order::RemainingUnits>(&live.remaining);
        if (!remaining) throw std::logic_error("native unresolved execution quantity");
        double qty = remaining->q;
        if (const auto* allowance = std::get_if<native_order::AllowanceUnits>(&live.allowance)) {
            if (allowance->point_ordinal == cursor.point.ordinal) qty = std::min(qty, allowance->left);
        }
        if (std::holds_alternative<native_order::Transact>(live.request().intent)
            || std::holds_alternative<native_order::Sized>(live.request().intent))
            candidate.physical = native_order::Transact{request_is_buy(engine, live) ? qty : -qty};
        else if (const auto* reverse = std::get_if<native_order::ReverseTo>(&live.request().intent))
            candidate.physical = execution::ReverseTo{reverse->signed_units};
        else
            candidate.physical = order_action::Reduce{qty};
    }
    candidate.fill = execution::Fill{resolved, live.request().label, live.request().comment,
                                    live.handle().incarnation, std::nullopt,
                                    close_cause_for(*live.definition)};
    if (const auto* reversal = std::get_if<execution::ReverseTo>(&candidate.physical)) {
        candidate.inspect = engine.inspect_native_reversal_v1(*reversal, candidate.fill);
    } else {
        const auto action = narrow_action(candidate.physical);
        candidate.inspect = candidate.selected
            ? engine.inspect_native_settlement_selected(action, candidate.fill, *candidate.selected)
            : engine.inspect_native_settlement_scoped(action, candidate.fill, candidate.financial_scope);
    }
    // Carry the inspection's binary64 ticket through every preview and the
    // real settlement commit; proportional allocation must not replace it.
    const double inspected_ticket = candidate.inspect.current_ticket;
    candidate.fill.commission_account = inspected_ticket;
    return candidate;
}

double NativeExecutionConsumer::sibling_claimed_units(
        const native_order::LiveRequest& live) const noexcept {
    double claimed = 0.0;
    for (const auto& other : requests_.live()) {
        if (other.handle() == live.handle()) continue;
        if (!same_reduction_scope(live.authority, other.authority)) continue;
        const auto* units = std::get_if<native_order::RemainingUnits>(&other.remaining);
        if (!units || !std::isfinite(units->q) || units->q <= 0.0) continue;
        claimed += units->q;
        if (!std::isfinite(claimed)) return std::numeric_limits<double>::quiet_NaN();
    }
    return claimed;
}

// The price a Sized basis converts at when the request is accepted.
// SizePrice::Resolved keeps the acceptance point's own price, which is what
// the candidate would otherwise divide by. SizePrice::Signal carries that
// decision price to the expected market fill: the run's slippage on the
// request's own side first, then the run's price grid when one is declared
// (NativePriceGrid::None leaves it alone). SizePrice::SignalOnTick measures
// the same rule on the instrument's own tick ladder instead of the run's fill
// grid, and quantizes BEFORE the slippage as well as after, because a whole
// number of ticks carries a ladder price to another ladder price. Neither
// names a source language; a nearest-tick, slippage-adjusted signal price on
// an otherwise unquantized run is {SignalOnTick, price_tick, slippage}.
double NativeExecutionConsumer::sizing_point_price(
        const NativeRunSpec& spec, const native_order::Sized& sized,
        double decision_price) const noexcept {
    if (sized.price == native_order::SizePrice::Resolved) return decision_price;
    const bool buy = sized.side == native_order::Side::Long;
    const double slip = static_cast<double>(spec.slippage_ticks) * spec.price_tick;
    if (sized.price == native_order::SizePrice::SignalOnTick) {
        const double on_tick = native_matching::grid_round_half_up(
            decision_price, spec.price_tick);
        return native_matching::grid_round_half_up(
            native_matching::apply_slippage(on_tick, slip, buy), spec.price_tick);
    }
    const double slipped = native_matching::apply_slippage(decision_price, slip, buy);
    return grid_fill_basis(spec, slipped, buy, /*limit_governed=*/false);
}

// The exposure a ScopeBasis::AtAcceptance fraction freezes. It mirrors the
// scope build_terms_facts measures at the candidate, derived from the owner
// the request declares because acceptance has not bound an authority yet.
std::optional<double> NativeExecutionConsumer::placement_scope_units(
        const BacktestEngine& engine, const native_order::Request& request) const {
    double units = 0.0;
    const auto sum_incarnations = [&](const std::vector<native_order::OpeningObservation>& rows) {
        std::vector<std::uint64_t> incarnations;
        incarnations.reserve(rows.size());
        for (const auto& row : rows) {
            if (row.has_live_matching_lot) incarnations.push_back(row.queried_opening.incarnation);
        }
        double total = 0.0;
        for (const auto& lot : engine.pyramid_entries_) {
            if (std::find(incarnations.begin(), incarnations.end(), lot.entry_incarnation)
                != incarnations.end()) {
                total += lot.qty;
            }
        }
        return total;
    };
    if (const auto* bind = std::get_if<native_order::BindOpening>(&request.owner)) {
        for (const auto& lot : engine.pyramid_entries_) {
            if (lot.entry_incarnation == bind->opening.incarnation) units += lot.qty;
        }
    } else if (const auto* bind = std::get_if<native_order::BindOpenings>(&request.owner)) {
        units = sum_incarnations(read_openings(engine, bind->openings, bind->cycle));
    } else if (const auto* bind = std::get_if<native_order::BindCohort>(&request.owner)) {
        const auto position = read_position(engine);
        const auto* nonflat = std::get_if<native_order::PositionNonflat>(&position);
        if (!nonflat) return 0.0;
        std::vector<native_order::RequestHandle> handles;
        handles.reserve(engine.pyramid_entries_.size());
        for (const auto& lot : engine.pyramid_entries_) {
            native_order::RequestHandle handle{requests_.identity(), lot.entry_incarnation};
            if (requests_.cohort_contains(bind->cohort, handle)) handles.push_back(handle);
        }
        units = sum_incarnations(read_openings(engine, handles, nonflat->cycle));
    } else {
        // Independent and WaitForApplied both bind the whole book.
        for (const auto& lot : engine.pyramid_entries_) units += lot.qty;
    }
    if (!std::isfinite(units)) return std::nullopt;
    return units;
}

// The run's opening admission, run at placement against an acceptance-resolved
// quantity instead of waiting for the candidate. It is the same gate the
// candidate applies (allowed directions, max_abs_units, max_open_lots, initial
// margin), fed by the same pure settlement inspection, at the sizing price and
// at `fx`, the acceptance point's rate make_command_context hands in. A
// host that owns its own margin rule declares no kernel margin, exactly as it
// does for the candidate gate, and only the caps apply here.
bool NativeExecutionConsumer::admit_placement_units(
        const BacktestEngine& engine, const native_order::Sized& sized,
        double units, double price, double fx) const {
    if (!spec_ptr() || !std::isfinite(units) || units <= 0.0) return true;
    if (!std::isfinite(price) || price <= 0.0) return true;
    const double signed_units = sized.side == native_order::Side::Long ? units : -units;
    execution::Fill fill;
    fill.price = price;
    const auto inspect = engine.inspect_native_settlement_scoped_at(
        order_action::Transact{signed_units}, fill, execution::Book{}, fx);
    if (inspect.status != execution::Status::Applied) return true;
    return admit_opening_inspect(engine, price, fx, inspect, /*skip_initial_margin=*/false,
                                 nullptr);
}

std::optional<double> NativeExecutionConsumer::resolve_sized_units(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const NativeExecutionTermsFacts& facts) const {
    const auto* spec = spec_ptr();
    if (!spec) return std::nullopt;
    if (const auto* native_sized = sized_intent(live)) {
        // An acceptance-time basis was resolved at the command boundary, so the
        // candidate republishes that frozen number rather than re-resolving it.
        // An acceptance the producer could not resolve never becomes resolvable.
        if (native_sized->time != native_order::SizeTime::AtMatch) {
            return live.sizing_units;
        }
        // A signal rule converts at the price frozen when the request was
        // accepted, at this and at every later candidate. A signal request
        // accepted with no decision point has no price to convert at.
        double price = facts.default_resolved_price;
        if (native_sized->price != native_order::SizePrice::Resolved) {
            if (!live.sizing_price) return std::nullopt;
            price = *live.sizing_price;
        }
        return sized_basis_units(*native_sized, price, marked(engine, price),
                                 facts.active_fx, *spec);
    }
    const auto* fraction = scope_fraction_intent(live);
    if (!fraction) return std::nullopt;
    double scope = facts.scope_exposure_units;
    if (fraction->basis == native_order::ScopeBasis::AtAcceptance) {
        // The placement-time live basis, frozen when the request was accepted.
        // A request accepted with no measurable scope never becomes resolvable.
        if (!live.sizing_scope) return std::nullopt;
        scope = *live.sizing_scope;
    }
    if (fraction->claim == native_order::ScopeClaim::NetOfSiblings) {
        scope -= sibling_claimed_units(live);
    }
    if (!std::isfinite(scope) || scope <= 0.0) return std::nullopt;
    // units = scope * fraction, one binary64 multiplication. A percent-spelled
    // caller converts percent -> fraction itself, so no second rounding step
    // enters here.
    return representable_units(scope * fraction->fraction,
                               native_order::ExecutionGridPolicy::SnapToGrid,
                               spec->quantity_grid);
}

NativeCurrentPointView NativeExecutionConsumer::execution_anchor(
        const native_order::MatchCursor& cursor, double resolved) const {
    NativeCurrentPointView out;
    out.decision.coordinate = cursor.point;
    out.decision.decision_floor_ms = std::max(decision_floor(), cursor.point.effective_time_ms);
    if (auto input = input_interval_at(cursor.point.open_ms))
        out.decision.input_interval = *input;
    if (auto script = script_interval_at(cursor.point.open_ms))
        out.decision.script_interval = *script;
    out.decision.sub_index = callback_context_.sub_index;
    out.decision.sub_count = callback_context_.sub_count;
    out.decision.is_terminal_sub_bar = callback_context_.is_terminal_sub_bar;
    out.decision.sub_bar_open_ms = callback_context_.sub_bar_open_ms;
    out.decision.script_bar_open_ms = callback_context_.script_bar_open_ms;
    out.decision.in_session = callback_context_.in_session;
    out.decision.opens_session_day = callback_context_.opens_session_day;
    out.decision.closes_session_day = callback_context_.closes_session_day;
    out.decision.closes_session_day_open_ended =
        callback_context_.closes_session_day_open_ended;
    out.price = resolved;
    out.quote_kind = NativeCurrentQuoteKind::ExecutionAnchor;
    return out;
}

std::optional<NativeCurrentExecutionResult> NativeExecutionConsumer::terminal_from_history(
        BacktestEngine& engine, const native_order::RequestHandle& cause_handle,
        NativeFailureOperation operation) {
    if (requests_.history().empty()) {
        throw std::logic_error("native terminal history is empty");
    }
    const auto event = requests_.history().back();
    NativeCurrentExecutionResult outcome;
    if (const auto* rejected = std::get_if<native_order::MatchRejectedEvent>(&event)) {
        outcome = *rejected;
    } else if (const auto* no_effect = std::get_if<native_order::NoEffectEvent>(&event)) {
        outcome = *no_effect;
    } else if (const auto* cancelled = std::get_if<native_order::CancelledEvent>(&event)) {
        outcome = *cancelled;
    } else {
        throw std::logic_error("native terminal outcome missing");
    }
    const native_order::EventId cause{cause_handle.run, command_ordinal(event)};
    drain_parent_terminal(engine, cause, cause_handle, operation);
    if (failed()) return std::nullopt;
    return outcome;
}

std::optional<NativeCurrentExecutionResult> NativeExecutionConsumer::consume_matched_request(
        BacktestEngine& engine, const native_order::RequestHandle& handle,
        const native_order::EvaluationContext& evaluation, double raw_price,
        double default_resolved_price, const NativeCurrentPointView& notification_point,
        native_order::NativeCandidatePriceKind price_kind,
        NativeCurrentPriceRule price_rule, bool shared_cursor_collision) {
    const auto P = evaluation.cursor.point.ordinal;
    const bool current = evaluation.driver_class == native_order::DriverEligibilityClass::CurrentExecution;
    try {
        auto* live = requests_.find_live(handle);
        if (!live) return std::nullopt;
        // All resolver, inspection, admission and financial reads see the
        // execution coordinate's FX activation instant.
        engine.current_bar_.timestamp = evaluation.cursor.point.effective_time_ms;
        auto terminal = [&](std::optional<native_order::MatchRejectReason> rejection,
                            std::optional<native_order::ExecutionTerms> attempted = std::nullopt)
                -> std::optional<NativeCurrentExecutionResult> {
            auto prep = rejection
                ? requests_.prepare_match_rejected(
                    handle, evaluation, *rejection, std::move(attempted), next_timeline_ordinal_)
                : requests_.prepare_no_effect(handle, evaluation, next_timeline_ordinal_);
            if (const auto* error = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *error, NativeFailureOperation::Settlement);
                return std::nullopt;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation) return std::nullopt;
            if (!install_mutation(engine, std::move(*mutation), NativeFailureOperation::Settlement, P))
                return std::nullopt;
            return terminal_from_history(engine, handle, NativeFailureOperation::Settlement);
        };

        const auto* host_sized = host_sized_intent(*live);
        const auto* native_sized = sized_intent(*live);
        const auto* scope_fraction = scope_fraction_intent(*live);
        const bool unresolved = (host_sized || native_sized || scope_fraction)
            && (std::holds_alternative<native_order::RemainingDeferred>(live->remaining)
                || std::holds_alternative<native_order::NoTarget>(live->remaining));
        // Only an OPENING may name a nondefault physical shape; a host-sized
        // close and a scope fraction settle through the ordinary
        // Transact/Reduce plan. A kernel-sized opening serves the same
        // ReverseTo / CloseOpposite shapes a HostSized{Open} does: the units
        // are the sized opening on the declared side, and the reversal
        // transaction closes the opposite book and opens them.
        const bool opening_shapes = (host_sized
            && host_sized->kind == native_order::HostSizedKind::Open)
            || native_sized != nullptr;
        const bool closing_size = (host_sized
                && host_sized->kind == native_order::HostSizedKind::Close)
            || scope_fraction != nullptr;
        const native_order::HostSizedKind terms_kind = opening_shapes || native_sized
            ? native_order::HostSizedKind::Open : native_order::HostSizedKind::Close;
        const std::optional<native_order::Side> terms_side = host_sized
            ? host_sized->side
            : (native_sized ? std::optional<native_order::Side>{native_sized->side}
                            : std::nullopt);
        // This is the lone terms pre-resolver shortcut. The ordinary queued
        // evaluation path already owns the equivalent terminal.
        if (unresolved && closing_size
            && std::holds_alternative<native_order::UnboundBookClose>(live->authority)
            && engine.position_side_ == PositionSide::FLAT) {
            return terminal(std::nullopt);
        }

        auto terms_facts = build_terms_facts(engine, *live, evaluation, price_kind,
                                             price_rule, raw_price, default_resolved_price,
                                             shared_cursor_collision);
        // The kernel owns the L3 bases. It resolves them before the host hook
        // and publishes the result as the facts' remaining units, so a host
        // override of resolve_execution_terms still has the last word.
        std::optional<double> kernel_units;
        if (unresolved && (native_sized || scope_fraction)) {
            kernel_units = resolve_sized_units(engine, *live, terms_facts);
            if (kernel_units) {
                terms_facts.remaining = native_order::RemainingUnits{*kernel_units};
            }
        }
        native_order::ExecutionTerms terms;
        try {
            auto* host = native_host(engine);
            if (!host) throw std::logic_error("native terms require a native host");
            terms = host->resolve_execution_terms(terms_facts);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, P});
            render(engine, e.what());
            return std::nullopt;
        } catch (...) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, P});
            render(engine, "native terms callback exception");
            return std::nullopt;
        }
        // A virtual can invoke a guarded public entry point and latch the run,
        // request abort, or perturb projection state without throwing.
        if (!check_abort_or_projection(engine, NativeFailureOperation::Settlement, P)) {
            return std::nullopt;
        }
        if (unresolved && (native_sized || scope_fraction) && !terms.units) {
            terms.units = kernel_units;
        }

        const bool identity = identity_terms(terms, default_resolved_price);
        const auto nonidentity_attempt = identity
            ? std::optional<native_order::ExecutionTerms>{}
            : std::optional<native_order::ExecutionTerms>{terms};
        if (!unresolved && (terms.units || terms.shape != native_order::OpeningShape::Transact)) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (unresolved && !terms.units) {
            return terminal(native_order::MatchRejectReason::TermsUnresolved, terms);
        }
        if (unresolved && !opening_shapes
            && terms.shape != native_order::OpeningShape::Transact) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (unresolved && terms.shape != native_order::OpeningShape::Transact
            && terms.shape != native_order::OpeningShape::ReverseTo
            && terms.shape != native_order::OpeningShape::CloseOpposite) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (terms.units && (!std::isfinite(*terms.units) || *terms.units < 0.0)) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (!execution_terms_grid_representable(
                terms, host_sized, unresolved, terms_facts.scope_exposure_units,
                spec_ptr())) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }

        double after = 0.0;
        double deduction = 0.0;
        bool exhausted = false;
        double binding_allowance = 0.0;
        if (unresolved) {
            if (!native_order::WorkingRequestCore::effective_host_units(
                    live->pending, *terms.units, &deduction, &after, &exhausted)) {
                native_order::TermsResolvedInput probe;
                probe.price_kind = price_kind;
                probe.shared_cursor_collision = shared_cursor_collision;
                probe.raw_price = raw_price;
                probe.default_resolved_price = default_resolved_price;
                probe.terms = terms;
                auto prepared = requests_.prepare_terms(handle, evaluation, probe,
                                                        next_timeline_ordinal_);
                if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
                    fail_preparation(engine, *error, NativeFailureOperation::Settlement);
                } else {
                    fail(engine, NativeFailure{NativeFailureCode::Contract,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, "native host-sized arithmetic probe did not fail");
                }
                return std::nullopt;
            }
            if (const auto* budget = std::get_if<native_order::PointBudget>(&live->request().capacity)) {
                binding_allowance = std::min(after, budget->units);
            } else {
                binding_allowance = after;
            }
        }

        const bool explicit_reversal = std::holds_alternative<native_order::ReverseTo>(
            live->request().intent);
        const bool shape_requires_opposite = explicit_reversal || (unresolved
            && (terms.shape == native_order::OpeningShape::ReverseTo
                || terms.shape == native_order::OpeningShape::CloseOpposite));
        if (shape_requires_opposite && terms_facts.opposite_book_units == 0.0) {
            return terminal(native_order::MatchRejectReason::NoOppositeExposure,
                            unresolved ? std::optional<native_order::ExecutionTerms>{terms}
                                       : nonidentity_attempt);
        }
        if (unresolved && terms.shape == native_order::OpeningShape::CloseOpposite
            && *terms.units > terms_facts.opposite_book_units) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (unresolved && terms.shape != native_order::OpeningShape::Transact
            && after > binding_allowance) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }

        std::optional<native_order::ExecutionPlan> plan;
        if (host_sized || native_sized || scope_fraction) {
            if (unresolved) {
                if (after > 0.0) {
                    plan = plan_from_terms(terms_kind, terms_side, terms.shape,
                                           after, binding_allowance, terms_facts.opposite_book_units);
                }
            } else if (const auto* remaining = std::get_if<native_order::RemainingUnits>(
                           &live->remaining)) {
                plan = plan_from_terms(terms_kind, terms_side,
                    native_order::OpeningShape::Transact, remaining->q,
                    allowance_left_at(terms_facts.allowance, P),
                terms_facts.opposite_book_units);
            }
        }

        const double resolved_price = terms.resolved_price;
        const bool zero_units_terminal = unresolved && *terms.units == 0.0;
        // A group-exhausting binding has no executable plan, just like the
        // zero-units terminal.  The current finite-price boundary must not
        // attempt its provisional inspection on the still-deferred row: the
        // receipt itself installs the terminal Cancelled{Group} outcome.
        const bool no_plan_terminal = zero_units_terminal
            || (unresolved && after == 0.0 && deduction > 0.0);
        // Price rejection is deliberately ahead of receipt installation. The
        // sole finite-current exception needs a stack-only plan inspection to
        // distinguish a pure close from an opening plan.
        if (!std::isfinite(resolved_price)) {
            if (current) throw std::overflow_error("native current price is not finite");
            return terminal(native_order::MatchRejectReason::NonpositivePrice, nonidentity_attempt);
        }
        if (!current && resolved_price <= 0.0 && !zero_units_terminal) {
            return terminal(native_order::MatchRejectReason::NonpositivePrice, nonidentity_attempt);
        }
        if (current && resolved_price <= 0.0 && !no_plan_terminal) {
            const auto provisional = inspect_candidate(engine, *live, evaluation.cursor, resolved_price,
                                                       plan ? &*plan : nullptr);
            if (provisional.inspect.would_open) {
                return terminal(native_order::MatchRejectReason::NonpositivePrice,
                                nonidentity_attempt);
            }
        }
        if (!no_plan_terminal
            && (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state))) {
            std::optional<double> level;
            bool fill_through = false;
            if (const auto* limit = std::get_if<native_order::Limit>(&live->request().trigger)) {
                level = limit->price;
                fill_through = limit->fill_through;
            } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(
                           &live->request().trigger)) {
                level = stop_limit->limit;
            }
            // A fill-through limit is a touch trigger: its terms may settle
            // past the level.
            if (!level || (!fill_through
                           && ((terms_facts.is_buy && resolved_price > *level)
                               || (!terms_facts.is_buy && resolved_price < *level)))) {
                return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
            }
        }

        native_order::TermsResolvedInput terms_input;
        terms_input.price_kind = price_kind;
        terms_input.shared_cursor_collision = shared_cursor_collision;
        terms_input.raw_price = raw_price;
        terms_input.default_resolved_price = default_resolved_price;
        terms_input.terms = terms;
        if (unresolved || !identity) {
            auto prepared = requests_.prepare_terms(handle, evaluation, terms_input,
                                                    next_timeline_ordinal_);
            if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
                fail_preparation(engine, *error, NativeFailureOperation::Settlement);
                return std::nullopt;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prepared);
            if (!mutation) {
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Settlement, P});
                render(engine, "native terms preparation did not produce a mutation");
                return std::nullopt;
            }
            if (!install_mutation(engine, std::move(*mutation),
                                  NativeFailureOperation::Settlement, P)) {
                return std::nullopt;
            }
            live = requests_.find_live(handle);
            if (!live) {
                return terminal_from_history(engine, handle, NativeFailureOperation::Settlement);
            }
            if (host_sized && !plan) {
                const auto* remaining = std::get_if<native_order::RemainingUnits>(&live->remaining);
                if (!remaining) {
                    fail(engine, NativeFailure{NativeFailureCode::Contract,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, "native bound terms have no remaining quantity");
                    return std::nullopt;
                }
                plan = plan_from_terms(host_sized->kind, host_sized->side,
                    native_order::OpeningShape::Transact, remaining->q,
                    allowance_left_at(live->allowance, P), terms_facts.opposite_book_units);
            }
        }

        auto candidate = inspect_candidate(engine, *live, evaluation.cursor, resolved_price,
                                           plan ? &*plan : nullptr);
        const auto& inspect = candidate.inspect;
        if (inspect.status == execution::Status::NoEffect) return terminal(std::nullopt);
        if (inspect.status != execution::Status::Applied) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                NativeFailureOperation::Settlement, P, static_cast<uint32_t>(inspect.status)});
            render(engine, "native settlement inspection failed");
            return std::nullopt;
        }
        if (inspect.would_open && resolved_price <= 0.0)
            return terminal(native_order::MatchRejectReason::NonpositivePrice, nonidentity_attempt);

        native_order::ExecutionProposal proposal;
        proposal.cursor = evaluation.cursor;
        proposal.pre_open_birth_eligible = evaluation.pre_open_birth_eligible;
        proposal.raw_price = raw_price;
        proposal.resolved_price = resolved_price;
        proposal.physical_action = candidate.physical;
        proposal.scope = candidate.scope;
        proposal.pre_fill = read_position(engine);
        proposal.pre_target = candidate.target;
        proposal.inspected_closed_units = inspect.closed_units;
        proposal.inspected_opened_units = inspect.opened_units;
        proposal.inspected_current_ticket = *candidate.fill.commission_account;
        const int64_t cycle_before = engine.position_cycle_seq_;
        const double signed_units_before = position(engine).signed_units;
        const native_order::EventId applied_id{handle.run, next_timeline_ordinal_};
        auto prepared = requests_.prepare_execution(handle, proposal, next_timeline_ordinal_);
        if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
            fail_preparation(engine, *error, NativeFailureOperation::Settlement);
            return std::nullopt;
        }
        auto* token = std::get_if<native_order::PreparedExecution>(&prepared);
        if (!token) {
            if (host_sized) {
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Settlement, P});
                render(engine, "native host-sized candidate silently skipped after binding");
            }
            return std::nullopt;
        }
        execution::PhysicalExecutionContext ctx;
        ctx.effective_time_ms = evaluation.cursor.point.effective_time_ms;
        ctx.interval_index = evaluation.cursor.point.interval_index;
        NativePrecommitView view;
        view.target = handle;
        view.definition = live->definition;
        view.cursor = evaluation.cursor;
        view.plan = candidate.physical;
        view.scope = candidate.scope;
        view.raw_price = raw_price;
        view.resolved_price = resolved_price;
        view.inspected_closed_units = inspect.closed_units;
        view.inspected_opened_units = inspect.opened_units;
        view.inspected_current_ticket = proposal.inspected_current_ticket;
        view.current = current;
        if (const auto* reversal = std::get_if<execution::ReverseTo>(&candidate.physical)) {
            view.settlement_readiness = engine.preview_native_settlement_commit(
                *reversal, candidate.fill, ctx, view.account, view.closed_row_pnl);
        } else {
            const auto action = narrow_action(candidate.physical);
            view.settlement_readiness = engine.preview_native_settlement_commit(
                action, candidate.fill, ctx, candidate.financial_scope,
                candidate.selected ? &*candidate.selected : nullptr,
                view.account, view.closed_row_pnl);
        }
        NativePrecommitVerdict verdict = NativePrecommitVerdict::Admit;
        if (view.settlement_readiness == execution::Status::Applied) {
            try {
                auto* host = native_host(engine);
                if (!host) throw std::logic_error("native precommit requires a native host");
                verdict = host->validate_execution_precommit(view);
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return std::nullopt;
            } catch (...) {
                fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                           NativeFailureOperation::Settlement, P});
                render(engine, "native precommit callback exception");
                return std::nullopt;
            }
            if (!check_abort_or_projection(engine, NativeFailureOperation::Settlement, P)) {
                return std::nullopt;
            }
            if (verdict == NativePrecommitVerdict::AdmitWithHostMargin) {
                const auto* admitted_spec = spec_ptr();
                if (admitted_spec
                    && (admitted_spec->initial_margin_fraction || admitted_spec->margin)) {
                    Fnv digest;
                    digest.run_base = requests_.identity().run_number;
                    digest.h = precommit_digest_.h;
                    digest.u(P);
                    digest.u(static_cast<std::uint64_t>(verdict));
                    hash_handle(digest, handle);
                    precommit_digest_.h = digest.h;
                    ++precommit_digest_.count;
                }
            }
            if (verdict == NativePrecommitVerdict::Refuse) {
                return terminal(native_order::MatchRejectReason::HostPrecommit,
                                nonidentity_attempt);
            }
        }
        native_order::MatchRejectReason reason{};
        // The candidate gate converts at its cursor's rate: the inspection
        // above was taken at the instant this function presents, the cursor's.
        if (inspect.would_open
            && !admit_opening_inspect(
                engine, resolved_price, margin_check_fx(engine, evaluation.cursor), inspect,
                verdict == NativePrecommitVerdict::AdmitWithHostMargin, &reason)) {
            return terminal(reason, nonidentity_attempt);
        }
        // Allocate before financial effects, with geometric growth rather than
        // recopying the complete observation/notification prefix on each fill.
        reserve_next(account_log_);
        reserve_next(applied_notifications_);
        AppliedNotification notification;
        notification.history_index = requests_.history().size();
        notification.ordinal = applied_id.ordinal;
        notification.point = notification_point;
        if (!current) {
            notification.point.price = resolved_price;
            notification.point.quote_origin_ordinal = applied_id.ordinal;
        }
        const auto settled = std::get_if<execution::ReverseTo>(&candidate.physical)
            ? engine.settle_native_reversal_at_v1(*std::get_if<execution::ReverseTo>(
                  &candidate.physical), candidate.fill, ctx)
            : candidate.selected
                ? engine.settle_native_execution_selected_at(
                    narrow_action(candidate.physical), candidate.fill, ctx, *candidate.selected)
                : engine.settle_native_execution_scoped_at(
                    narrow_action(candidate.physical), candidate.fill, ctx, candidate.financial_scope);
        if (settled.status != execution::Status::Applied) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                NativeFailureOperation::Settlement, P, static_cast<uint32_t>(settled.status)});
            render(engine, "native settlement commit failed");
            return std::nullopt;
        }
        refresh_target_scalars(engine, candidate.target);
        native_order::CommittedExecutionFacts committed;
        committed.result = settled;
        committed.cycle_before = cycle_before;
        committed.cycle_after = engine.position_cycle_seq_;
        committed.post_target = std::move(candidate.target);
        committed.committed_action = candidate.physical;
        if (!install_execution(engine, std::move(*token), committed, P)) return std::nullopt;
        const auto& applied = std::get<native_order::ExecutionAppliedEvent>(
            requests_.history().at(notification.history_index));
        if (applied.ordinal != applied_id.ordinal || (current && !applied.terminal))
            throw std::logic_error("native execution receipt mismatch");
        NativeCurrentExecutionResult outcome{applied};
        NativeAccountObservation observation;
        observation.ordinal = applied_id.ordinal;
        observation.effective_time_ms = ctx.effective_time_ms;
        observation.marked_equity = engine.marked_equity(resolved_price);
        observation.realized_balance = engine.initial_capital_ + engine.net_profit_sum_;
        for (const auto& lot : engine.pyramid_entries_) observation.signed_units += lot.qty;
        if (engine.position_side_ == PositionSide::SHORT) observation.signed_units = -observation.signed_units;
        account_log_.push_back(observation);
        fold_account_digest(observation);
        engine.bar_index_ = ctx.interval_index;
        // L9: one applied fill of its risk day. Counting only — settlement is
        // not a decision point, so no limit is evaluated here.
        risk_note_fill(engine, notification.point.decision.coordinate, resolved_price);
        // L4: the margin receipt of a kernel-issued liquidation follows its
        // own fill directly, before any dependency mutation of that fill. The
        // owning event is read from the outcome copy because recording it
        // appends to the history the borrowed reference points into.
        {
            const auto& booked = std::get<native_order::ExecutionAppliedEvent>(outcome);
            if (booked.definition
                && booked.definition->origin != native_order::RequestOrigin::Host) {
                notification.margin_call_index = record_margin_call(
                    engine, booked, booked.definition, signed_units_before,
                    observation.signed_units);
                if (failed()) return std::nullopt;
            }
        }
        drain_after_applied(engine, applied_id, handle);
        if (failed()) return std::nullopt;
        enqueue_applied_notification(std::move(notification));
        return outcome;
    } catch (const std::bad_alloc& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Settlement, P});
        render(engine, e.what());
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::SettlementFailure, NativeFailureOperation::Settlement, P});
        render(engine, e.what());
    } catch (...) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Settlement, P});
    }
    return std::nullopt;
}

// A heap on precedes keeps the row that precedes every other on top.
void NativeExecutionConsumer::MatchRows::sift_up() {
    std::push_heap(rows_.begin(), rows_.end(),
                   [](const MatchCandidate& a, const MatchCandidate& b) { return precedes(b, a); });
}

void NativeExecutionConsumer::MatchRows::keep(const MatchCandidate& row) {
    if (ascending_) {
        // The run stays ascending with the row after its last one or, once a
        // row was taken, before its front, in the slot that row left.
        if (head_ == rows_.size() || !precedes(row, rows_.back())) {
            rows_.push_back(row);
            return;
        }
        if (head_ > 0 && precedes(row, rows_[head_])) {
            rows_[--head_] = row;
            return;
        }
        // Out of order: the rows still to take become the heap. A scan that
        // has taken none arranges it at its first take, as before.
        const bool taken = head_ > 0;
        rows_.erase(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
        ascending_ = false;
        if (taken) {
            std::make_heap(rows_.begin(), rows_.end(),
                           [](const MatchCandidate& a, const MatchCandidate& b) {
                               return precedes(b, a);
                           });
            heaped_ = true;
        }
    }
    rows_.push_back(row);
    if (heaped_) sift_up();
}

std::size_t NativeExecutionConsumer::MatchRows::take_first() {
    if (ascending_) return rows_[head_++].live_index;
    const auto below = [](const MatchCandidate& a, const MatchCandidate& b) {
        return precedes(b, a);
    };
    if (!heaped_) {
        std::make_heap(rows_.begin(), rows_.end(), below);
        heaped_ = true;
    }
    const std::size_t live_index = rows_.front().live_index;
    std::pop_heap(rows_.begin(), rows_.end(), below);
    rows_.pop_back();
    return live_index;
}

void NativeExecutionConsumer::match_path(
        BacktestEngine& engine, const NativeDriverPoint& point,
        bool continuous, double from_price, double to_price) {
    if (failed()) return;
    // Every matching cursor is its own recalculation budget (L5).
    open_point_epoch();
    if (point.coordinate.provenance == NativePriceProvenance::CurrentExecution) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Settlement,
                                   point.coordinate.ordinal});
        render(engine, "current execution points require a guarded target");
        return;
    }
    const auto* spec = spec_ptr();
    if (!spec) return;
    // MG9: a step of the declared FX curve is measured before this point is
    // matched, so a liquidation it rests is live for the point itself.
    fx_roll_margin_check(engine, point, continuous, from_price);
    if (failed()) return;
    // With no live request there is no trigger, allowance, trail, receipt or
    // callback work to perform. Keep the one physical excursion effect the
    // regular segment path would have applied to an already-open position.
    if (requests_.live().empty()) {
        if (continuous) apply_excursion(engine, to_price);
        return;
    }
    const auto driver_class = classify_driver(point, continuous);
    const auto grid = grid_threshold(*spec);
    const uint64_t P = point.coordinate.ordinal;
    requests_.refresh_point_allowances(P, read_position(engine));
    double t_cursor = 0.0;
    double cursor_price = from_price;
    native_order::MatchCursor path_cursor = make_cursor(point, t_cursor);
    observe_trails(engine, point, path_cursor, continuous,
                   native_matching::price_at(from_price, to_price, t_cursor));

    using Kind = MatchKind;
    using Candidate = MatchCandidate;
    using CandidateProvenance = MatchProvenance;
    auto& candidate_provenance = match_provenance_;
    candidate_provenance.clear();

    auto same_optional_bits = [](const std::optional<double>& left,
                                 const std::optional<double>& right) {
        return left.has_value() == right.has_value()
            && (!left || native_matching::double_bits(*left)
                         == native_matching::double_bits(*right));
    };
    auto same_provenance_key = [&](const CandidateProvenance& row,
                                   const native_order::LiveRequest& live,
                                   Kind kind, bool buy,
                                   const std::optional<double>& level,
                                   double t) {
        return row.handle == live.handle()
            && row.point_ordinal == P
            && row.kind == kind
            && row.trigger_state_index == live.trigger_state.index()
            && row.is_buy == buy
            && same_optional_bits(row.trigger_level, level)
            && native_matching::double_bits(row.t) == native_matching::double_bits(t);
    };
    auto retained_origin = [&](const native_order::LiveRequest& live, Kind kind, bool buy,
                               const std::optional<double>& level, double t)
            -> const CandidateProvenance* {
        for (auto it = candidate_provenance.rbegin(); it != candidate_provenance.rend(); ++it) {
            if (same_provenance_key(*it, live, kind, buy, level, t)) return &*it;
        }
        return nullptr;
    };
    auto level_for = [&](const native_order::LiveRequest& live, Kind kind,
                         bool buy) -> std::optional<double> {
        const auto& trigger = live.request().trigger;
        const auto& state = live.trigger_state;
        if (std::holds_alternative<native_order::LimitReady>(state)
            || std::holds_alternative<native_order::StopLimitLive>(state)) {
            if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) return limit->price;
            if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
                return stop_limit->limit;
            }
        }
        if (std::holds_alternative<native_order::StopIdle>(state)
            || std::holds_alternative<native_order::StopActive>(state)) {
            if (const auto* stop = std::get_if<native_order::Stop>(&trigger)) return stop->price;
        }
        if (std::holds_alternative<native_order::StopLimitPending>(state)) {
            if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
                return stop_limit->stop;
            }
        }
        if (std::holds_alternative<native_order::TrailWaitArm>(state)) {
            if (const auto* trail = std::get_if<native_order::Trail>(&trigger)) return trail->arm_price;
        }
        if (const auto* tracking = std::get_if<native_order::TrailTrack>(&state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            double stop = 0.0;
            if (trail && native_matching::checked_trail_stop(
                    tracking->best, trail->offset, buy, &stop, ladder_tick())) return stop;
        }
        if (const auto* active = std::get_if<native_order::TrailActive>(&state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            double stop = 0.0;
            if (trail && native_matching::checked_trail_stop(
                    active->best_at_trigger, trail->offset, buy, &stop,
                    ladder_tick())) return stop;
        }
        (void)kind;
        return std::nullopt;
    };
    auto erase_provenance_for = [&](const native_order::RequestHandle& handle) {
        candidate_provenance.erase(
            std::remove_if(candidate_provenance.begin(), candidate_provenance.end(),
                [&](const CandidateProvenance& row) { return row.handle == handle; }),
            candidate_provenance.end());
    };
    auto cause_floor = [&](const native_order::LiveRequest& live) {
        double t_min = t_cursor;
        if (const auto* armed = std::get_if<native_order::ArmedTransaction>(&live.authority)) {
            if (armed->cause_cursor.point.ordinal == P) t_min = std::max(t_min, armed->cause_cursor.t);
        }
        if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
            if (const auto* from = std::get_if<native_order::EnrollmentFromApplied>(&opening->enrollment)) {
                if (from->cursor.point.ordinal == P) t_min = std::max(t_min, from->cursor.t);
            }
        }
        return t_min;
    };

    // The driver point is allocated before matching this monotonic segment.
    // Therefore an acceptance ordinal after P can only have been created by a
    // callback at the current path cursor. Admit that birth at t_cursor; the
    // ordinary geometric search then sees only the unconsumed suffix. Requests
    // accepted before this segment and discrete points retain the existing gate.
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };

    // NativeArmFirstMatch::AfterArmPrint: on the driver point whose fill armed
    // it, the request has the callback-born rule above -- the print that armed
    // it is consumed, only a later crossing on this point matches. On a
    // discrete point that is no match at all; the next point is ordinary.
    auto armed_after_print_here = [&](const native_order::LiveRequest& live) {
        const auto* wait = std::get_if<native_order::WaitForApplied>(&live.request().owner);
        if (!wait || wait->first_match != native_order::NativeArmFirstMatch::AfterArmPrint) {
            return false;
        }
        if (const auto* armed = std::get_if<native_order::ArmedTransaction>(&live.authority)) {
            return armed->cause_cursor.point.ordinal == P;
        }
        if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
            if (const auto* from = std::get_if<native_order::EnrollmentFromApplied>(
                    &opening->enrollment)) {
                return from->cursor.point.ordinal == P;
            }
        }
        // NativeArmScope::Book: the arm is the book binding.
        if (const auto* book = std::get_if<native_order::BookClose>(&live.authority)) {
            return book->binding_cursor.point.ordinal == P;
        }
        return false;
    };

    // Whether a request's allowance was already evaluated at this point. Every
    // Allowance alternative but AllowanceUnset carries the point it was
    // evaluated at.
    auto allowance_at_point = [&](const native_order::LiveRequest& live) {
        if (const auto* units = std::get_if<native_order::AllowanceUnits>(&live.allowance)) {
            return units->point_ordinal == P;
        }
        if (const auto* all = std::get_if<native_order::AllowanceAllScope>(&live.allowance)) {
            return all->point_ordinal == P;
        }
        if (const auto* deferred = std::get_if<native_order::AllowanceDeferred>(&live.allowance)) {
            return deferred->point_ordinal == P;
        }
        return false;
    };
    auto needs_evaluation = [&](const native_order::LiveRequest& live,
                                const native_order::EligibilityFacts& facts) {
        return facts.needs_close_bind || !allowance_at_point(live);
    };

    // The hit a priced trigger gets on the rest of this segment from `start`,
    // and the transition that hit would be (R5 lane PERF-L5). One computation
    // for the band pre-check and the full evaluation below, so the pre-check
    // passes a row over exactly when the full evaluation would find no hit:
    //   Absent          no hit, or a trigger that does not price the state it
    //                   is in; the full evaluation passes the row over.
    //   Hit             `hit` and `kind` hold the crossing.
    //   Unpriced        a state no level gates (a market request, an activated
    //                   stop or trail); the caller decides it.
    //   Unrepresentable a tracked trail whose offset names no stop from its
    //                   best; the full evaluation fails the run on it.
    enum class TriggerReach : std::uint8_t { Absent, Hit, Unpriced, Unrepresentable };
    auto reach_trigger = [&](const native_order::LiveRequest& live, bool buy,
                             bool include_current, const native_matching::GeometricHit& start,
                             std::optional<native_matching::GeometricHit>& hit,
                             Kind& kind) -> TriggerReach {
        const auto& trigger = live.request().trigger;
        const auto& state = live.trigger_state;
        if (std::holds_alternative<native_order::StopIdle>(state)) {
            const auto* stop = std::get_if<native_order::Stop>(&trigger);
            if (!stop) return TriggerReach::Absent;
            hit = native_matching::first_region_entry(
                from_price, to_price, start, stop->price, !buy, include_current, grid);
            kind = Kind::ActivateStop;
        } else if (std::holds_alternative<native_order::StopLimitPending>(state)) {
            const auto* sl = std::get_if<native_order::StopLimit>(&trigger);
            if (!sl) return TriggerReach::Absent;
            hit = native_matching::first_region_entry(
                from_price, to_price, start, sl->stop, !buy, include_current, grid);
            kind = Kind::ActivateStopLimit;
        } else if (std::holds_alternative<native_order::TrailWaitArm>(state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            if (!trail) return TriggerReach::Absent;
            if (!trail->arm_price) {
                hit = start;
            } else {
                hit = native_matching::first_region_entry(
                    from_price, to_price, start, *trail->arm_price, buy,
                    include_current, grid);
            }
            kind = Kind::BeginTrail;
        } else if (const auto* track = std::get_if<native_order::TrailTrack>(&state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            if (!trail) return TriggerReach::Absent;
            double stop = 0.0;
            if (!native_matching::checked_trail_stop(track->best, trail->offset, buy,
                                                    &stop, ladder_tick())) {
                return TriggerReach::Unrepresentable;
            }
            hit = native_matching::trail_stop_hit(
                from_price, to_price, start, track->best, trail->offset, buy, grid,
                ladder_tick());
            kind = Kind::ActivateTrail;
        } else if (std::holds_alternative<native_order::LimitReady>(state)
                   || std::holds_alternative<native_order::StopLimitLive>(state)) {
            double level = 0.0;
            if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
                level = limit->price;
            } else if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) {
                level = sl->limit;
            } else {
                return TriggerReach::Absent;
            }
            hit = native_matching::first_region_entry(
                from_price, to_price, start, level, buy, include_current, grid);
            kind = Kind::Fill;
        } else if (std::holds_alternative<native_order::MarketReady>(state)
                   || std::holds_alternative<native_order::StopActive>(state)
                   || std::holds_alternative<native_order::TrailActive>(state)) {
            return TriggerReach::Unpriced;
        } else {
            return TriggerReach::Absent;
        }
        return hit ? TriggerReach::Hit : TriggerReach::Absent;
    };

    auto side_from_target = [](const native_order::TargetObservation& target)
            -> std::optional<native_order::Side> {
        for (const auto& opening : target.openings) {
            if (!opening.has_live_matching_lot) continue;
            if (const auto* position = std::get_if<native_order::PositionNonflat>(
                    &opening.current_position)) {
                return position->side;
            }
        }
        return std::nullopt;
    };

    auto provenance_still_matches = [&](const CandidateProvenance& row) {
        const auto* live = requests_.find_live(row.handle);
        if (!live) return false;
        const auto* target = cached_cohort_target(engine, *live);
        const bool buy = target ? requests_.working_is_buy(*live, side_from_target(*target))
                                : scratch_request_is_buy(engine, *live);
        return row.is_buy == buy
            && row.trigger_state_index == live->trigger_state.index()
            && same_optional_bits(row.trigger_level, level_for(*live, row.kind, buy));
    };

    // The (incarnation, kind) keys passed over at the current cursor: a set,
    // kept as a sorted run in the consumer's scratch.
    auto& skipped = match_skipped_;
    skipped.clear();
    double skip_t = t_cursor;
    auto skip_key = [](uint64_t incarnation, Kind kind) {
        return std::pair<uint64_t, std::uint8_t>{incarnation, static_cast<std::uint8_t>(kind)};
    };
    auto is_skipped = [&](const std::pair<uint64_t, std::uint8_t>& key) {
        return std::binary_search(skipped.begin(), skipped.end(), key);
    };
    auto skip = [&](const std::pair<uint64_t, std::uint8_t>& key) {
        const auto at = std::lower_bound(skipped.begin(), skipped.end(), key);
        if (at == skipped.end() || *at != key) skipped.insert(at, key);
    };
    // Whether a provenance row names this request. Without one, every way the
    // full evaluation passes the request over leaves the provenance as it was.
    auto has_provenance = [&](const native_order::RequestHandle& handle) {
        for (const auto& row : candidate_provenance) {
            if (row.handle == handle) return true;
        }
        return false;
    };
    // A row the band pre-check may pass over: a priced trigger state whose
    // allowance is already this point's, so the row is not an Evaluate row.
    auto banded = [&](const native_order::LiveRequest& live) {
        if (std::holds_alternative<native_order::UnboundBookClose>(live.authority)
            || !allowance_at_point(live)) {
            return false;
        }
        const auto& state = live.trigger_state;
        return std::holds_alternative<native_order::StopIdle>(state)
            || std::holds_alternative<native_order::StopLimitPending>(state)
            || std::holds_alternative<native_order::TrailWaitArm>(state)
            || std::holds_alternative<native_order::TrailTrack>(state)
            || std::holds_alternative<native_order::LimitReady>(state)
            || std::holds_alternative<native_order::StopLimitLive>(state);
    };
    const bool band_precheck = match_band_precheck_;

    // Rows survive an allowance refresh (R5 lane PERF-K3). Every live request
    // refresh_point_allowances did not refresh in bulk is an Evaluate winner
    // of its own at each point, and each winner used to be followed by a scan
    // of the whole book: K such requests cost K scans of K rows per point.
    // When the winner is an Evaluate row whose refresh_allowance succeeds at
    // an unmoved cursor -- the same t, bit for bit, so the same cursor price
    // and path cursor -- the next scan differs from the last in that row only:
    //  - refresh_allowance writes the winner's allowance and the core's epoch
    //    and nothing else. It moves no request, trigger state, roster,
    //    position or pre-open birth and installs nothing, so the cohort
    //    target cache is not cleared either.
    //  - A row reads its own request (eligibility_facts, cause_floor,
    //    needs_evaluation, level_for, working_is_buy; none reads another
    //    request or the epoch), the unchanged cursor and engine, the cohort
    //    target cache, which the last full scan already consulted for every
    //    request, and the skip set, which is cleared only when the cursor
    //    moves.
    //  - Its provenance lookup reads its own request's rows, and rescanning
    //    it would only append a copy of the row it already left. The erase at
    //    the loop head would drop nothing: every row it tests passed at the
    //    last head or was pushed since from the same state.
    // So after such a refresh only the winner is rescanned, and the next
    // winner comes from the rows already built. Their incarnations are
    // distinct (the book holds each request once) and every row's t is
    // finite, so (t, incarnation) orders them totally and a heap on the
    // scan's own comparison yields exactly the row the full rescan would
    // pick. Any other outcome -- a fill, a trigger, a mutation, a skip, a
    // moved cursor -- scans the whole book again. Rows are kept from four
    // live requests on; below that a scan is as cheap as keeping them.
    constexpr std::size_t kKeepRowsFrom = 4;
    auto& rows = match_rows_;
    rows.clear();
    bool rescan_winner_only = false;
    // The request a row was scanned from. The book does not change between a
    // scan and its winner's first use, so the row's own index names it; the
    // core's handle lookup answers whenever the reuse is switched off.
    auto scanned_request = [&](const Candidate& row) -> const native_order::LiveRequest* {
        const auto& book = requests_.live();
        if (match_row_reuse_ && row.live_index < book.size()
            && book[row.live_index].handle() == row.handle) {
            return &book[row.live_index];
        }
        return requests_.find_live(row.handle);
    };

    while (!failed()) {
        const bool winner_only = rescan_winner_only;
        rescan_winner_only = false;
        if (!winner_only) {
            candidate_provenance.erase(
                std::remove_if(candidate_provenance.begin(), candidate_provenance.end(),
                    [&](const CandidateProvenance& row) {
                        return row.point_ordinal != P || row.t < t_cursor
                            || !provenance_still_matches(row);
                    }),
                candidate_provenance.end());
        }
        if (skip_t != t_cursor) {
            skipped.clear();
            skip_t = t_cursor;
        }
        native_order::EvaluationContext eval;
        eval.cursor = make_cursor(point, t_cursor);
        eval.driver_class = driver_class;
        eval.existing_matching_bit = point.matching;
        std::optional<Candidate> winner;
        // Candidate selection makes no request-core mutation; only the
        // selected winner can replace or retire a later request afterwards.
        // Snapshot pointers through that selection pass. The ordinary route
        // has only the two bracket siblings, so keep it on the stack.
        std::array<const native_order::LiveRequest*, 8> inline_snapshot{};
        const auto& live_requests = requests_.live();
        const native_order::LiveRequest* const* snapshot = inline_snapshot.data();
        std::size_t snapshot_size = live_requests.size();
        std::size_t winner_index = 0;
        if (winner_only) {
            // The refreshed winner's row is the first one: take it off and
            // scan that one request again.
            winner_index = rows.take_first();
            inline_snapshot[0] = &live_requests[winner_index];
            snapshot_size = 1;
        } else if (snapshot_size <= inline_snapshot.size()) {
            for (std::size_t i = 0; i < snapshot_size; ++i)
                inline_snapshot[i] = &live_requests[i];
        } else {
            auto& overflow_snapshot = match_snapshot_;
            overflow_snapshot.clear();
            overflow_snapshot.reserve(snapshot_size);
            for (const auto& live : live_requests) overflow_snapshot.push_back(&live);
            snapshot = overflow_snapshot.data();
        }
        const bool keep_rows = winner_only
            || (match_row_reuse_ && live_requests.size() >= kKeepRowsFrom);
        if (!winner_only) rows.clear();
        for (std::size_t snapshot_index = 0; snapshot_index < snapshot_size; ++snapshot_index) {
            const auto* live = snapshot[snapshot_index];
            if (!live) continue;
            const auto& handle = live->handle();
            const bool cohort_close =
                std::holds_alternative<native_order::CohortClose>(live->authority);
            const native_order::TargetObservation* candidate_target = nullptr;
            std::optional<native_order::Side> cohort_side;
            if (cohort_close) {
                // Candidate selection is read-only. Reuse its complete target
                // observation for the side and trigger calculations, then
                // rebuild at the selected mutation boundary below.
                candidate_target = cached_cohort_target(engine, *live);
                if (!candidate_target) {
                    read_target_into(engine, live, match_target_, match_target_handles_);
                    candidate_target = &match_target_;
                }
                cohort_side = side_from_target(*candidate_target);
                if (!cohort_side) {
                    erase_provenance_for(handle);
                    continue;
                }
            }
            // A callback-born priced request begins immediately after the
            // birth print. It may cross a later level on this suffix, but
            // it does not inherit an already-consumed/equal crossing from
            // the request that produced the callback.
            const bool include_current = !born_on_remaining_path(*live)
                && !armed_after_print_here(*live);
            // The band pre-check (R5 lane PERF-L5). A priced trigger whose
            // allowance is already this point's is not an Evaluate row, so
            // the full evaluation below decides it on reach_trigger alone: no
            // hit, and it passes the row over. Every other way it can pass
            // the row over -- an ineligible request, a cause floor past the
            // point -- erases the request's provenance and nothing else, and
            // a request with no provenance row has nothing to erase. For such
            // a request, a trigger the rest of the segment cannot reach is
            // therefore passed over here with exactly the effect of the full
            // evaluation, before its eligibility is read: the same start, the
            // same side and the same geometry, computed as below. A hit, an
            // unrepresentable trail or an unpriced state goes on to the full
            // evaluation, which computes them again.
            if (band_precheck && banded(*live) && !has_provenance(handle)) {
                const double t_floor = cause_floor(*live);
                if (t_floor > 1.0) continue;
                const native_matching::GeometricHit floor_start{
                    t_floor, t_floor == t_cursor
                                 ? cursor_price
                                 : native_matching::price_at(from_price, to_price, t_floor)};
                const bool floor_buy = cohort_close
                    ? requests_.working_is_buy(*live, cohort_side)
                    : request_is_buy(engine, *live);
                std::optional<native_matching::GeometricHit> reach;
                Kind reach_kind = Kind::Fill;
                if (reach_trigger(*live, floor_buy, include_current, floor_start, reach, reach_kind)
                    == TriggerReach::Absent) {
                    continue;
                }
            }
            native_order::EvaluationContext candidate_eval = eval;
            candidate_eval.pre_open_birth_eligible = pre_open_birth_eligible(handle, point)
                || born_on_remaining_path(*live);
            candidate_eval.cohort_side = cohort_side;
            const auto facts = requests_.eligibility_facts(*live, candidate_eval);
            if (!facts.birth_ok || facts.waiting || !facts.driver_ok) {
                erase_provenance_for(handle);
                continue;
            }
            const double t_min = cause_floor(*live);
            if (t_min > 1.0) {
                erase_provenance_for(handle);
                continue;
            }
            const native_matching::GeometricHit start{
                t_min, t_min == t_cursor ? cursor_price
                                        : native_matching::price_at(from_price, to_price, t_min)};
            Candidate row;
            row.handle = handle;
            row.incarnation = handle.incarnation;
            if (needs_evaluation(*live, facts)) {
                erase_provenance_for(handle);
                row.t = t_min;
                row.price = start.price;
                row.kind = Kind::Evaluate;
            } else {
                const bool buy = cohort_close
                    ? requests_.working_is_buy(*live, candidate_eval.cohort_side)
                    : request_is_buy(engine, *live);
                std::optional<native_matching::GeometricHit> hit;
                Kind kind = Kind::Fill;
                switch (reach_trigger(*live, buy, include_current, start, hit, kind)) {
                case TriggerReach::Absent:
                    continue;
                case TriggerReach::Unrepresentable:
                    fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, "native trailing offset is not representable");
                    return;
                case TriggerReach::Unpriced:
                    if (!facts.ready_to_match) {
                        erase_provenance_for(handle);
                        continue;
                    }
                    if (std::holds_alternative<native_order::MarketReady>(live->trigger_state)
                        && continuous) {
                        continue;
                    }
                    hit = start;
                    kind = Kind::Fill;
                    break;
                case TriggerReach::Hit:
                    break;
                }
                if (kind == Kind::Fill) {
                    if (const auto* units = std::get_if<native_order::AllowanceUnits>(&live->allowance)) {
                        if (units->point_ordinal == P && units->left == 0.0) {
                            erase_provenance_for(handle);
                            continue;
                        }
                    }
                    if (std::holds_alternative<native_order::RemainingUnbound>(live->remaining)) {
                        erase_provenance_for(handle);
                        continue;
                    }
                }
                row.t = hit->t;
                row.price = hit->price;
                row.kind = kind;
                row.trigger_level = level_for(*live, kind, buy);
                row.at_level = hit->at_level;
            }
            if (!std::isfinite(row.price) || row.t < t_cursor || row.t > 1.0) {
                erase_provenance_for(handle);
                continue;
            }
            if (is_skipped(skip_key(row.incarnation, row.kind))) {
                erase_provenance_for(handle);
                continue;
            }
            if (row.kind != Kind::Evaluate) {
                const bool buy = std::holds_alternative<native_order::CohortClose>(live->authority)
                    ? requests_.working_is_buy(*live, candidate_eval.cohort_side)
                    : request_is_buy(engine, *live);
                if (!row.trigger_level) row.trigger_level = level_for(*live, row.kind, buy);
                if (!row.at_level) {
                    if (const auto* retained = retained_origin(
                            *live, row.kind, buy, row.trigger_level, row.t)) {
                        row.at_level = retained->at_level;
                        row.trigger_level = retained->trigger_level;
                    }
                }
                CandidateProvenance provenance;
                provenance.handle = handle;
                provenance.point_ordinal = P;
                provenance.kind = row.kind;
                provenance.trigger_state_index = live->trigger_state.index();
                provenance.is_buy = buy;
                provenance.trigger_level = row.trigger_level;
                provenance.t = row.t;
                provenance.raw_price = row.price;
                provenance.at_level = row.at_level;
                candidate_provenance.push_back(std::move(provenance));
                if (row.at_level && row.trigger_level) {
                    row.shared_cursor_collision = native_matching::double_bits(row.price)
                        != native_matching::double_bits(*row.trigger_level);
                }
            }
            row.live_index = winner_only ? winner_index : snapshot_index;
            if (keep_rows) rows.keep(row);
            if (!winner || MatchRows::precedes(row, *winner)) winner = row;
        }
        if (winner_only) {
            // Every other request's row is the one the last scan built.
            winner.reset();
            if (const auto* first = rows.first()) winner = *first;
        }
        if (!winner) break;
        const bool cursor_unmoved =
            native_matching::double_bits(winner->t) == native_matching::double_bits(t_cursor);
        if (continuous && winner->t > t_cursor) {
            apply_excursion(engine, winner->price);
            path_cursor = make_cursor(point, winner->t);
            observe_trails(engine, point, path_cursor, continuous, winner->price);
        }
        t_cursor = winner->t;
        cursor_price = winner->price;
        path_cursor = make_cursor(point, t_cursor);
        eval.cursor = path_cursor;
        const auto* live = scanned_request(*winner);
        if (!live) continue;
        eval.pre_open_birth_eligible = pre_open_birth_eligible(winner->handle, point)
            || born_on_remaining_path(*live);
        const auto* winner_target = cached_cohort_target(engine, *live);
        eval.cohort_side = winner_target ? side_from_target(*winner_target)
                                         : scratch_cohort_side(engine, *live);
        if (std::holds_alternative<native_order::CohortClose>(live->authority)
            && !eval.cohort_side) {
            skip(skip_key(winner->incarnation, winner->kind));
            continue;
        }
        if (winner->kind == Kind::Evaluate) {
            // A refresh at this cursor leaves every other row as it is.
            const bool reuse_after_refresh = keep_rows && cursor_unmoved;
            try {
                if (winner_target) {
                    if (requests_.refresh_allowance(winner->handle, eval, *winner_target)) {
                        rescan_winner_only = reuse_after_refresh;
                        continue;
                    }
                } else if (std::holds_alternative<native_order::BookClose>(live->authority)) {
                    native_order::TargetObservation obs;
                    obs.current_position = read_position(engine);
                    if (requests_.refresh_allowance(winner->handle, eval, obs)) {
                        rescan_winner_only = reuse_after_refresh;
                        continue;
                    }
                } else {
                    read_target_into(engine, live, match_target_, match_target_handles_);
                    if (requests_.refresh_allowance(winner->handle, eval, match_target_)) {
                        rescan_winner_only = reuse_after_refresh;
                        continue;
                    }
                }
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return;
            }
            native_order::Preparation<native_order::PreparedMutation> prep;
            try {
                if (winner_target) {
                    prep = requests_.prepare_evaluation(
                        winner->handle, eval, *winner_target, next_timeline_ordinal_);
                } else {
                    read_target_into(engine, live, match_target_, match_target_handles_);
                    prep = requests_.prepare_evaluation(
                        winner->handle, eval, match_target_, next_timeline_ordinal_);
                }
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return;
            }
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) {
                skip(skip_key(winner->incarnation, winner->kind));
                continue;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement, P)) {
                return;
            }
            if (requests_.find_live(winner->handle) == nullptr && !requests_.history().empty()) {
                try {
                    drain_parent_terminal(
                            engine,
                            native_order::EventId{winner->handle.run,
                                                  command_ordinal(requests_.history().back())},
                            winner->handle, NativeFailureOperation::Settlement);
                } catch (const std::exception& e) {
                    fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, e.what());
                    return;
                }
            }
            continue;
        }
        if (winner->kind != Kind::Fill) {
            native_order::TriggerTransition transition;
            if (winner->kind == Kind::ActivateStop) {
                transition = native_order::ActivateStop{path_cursor, winner->price};
            } else if (winner->kind == Kind::ActivateStopLimit) {
                transition = native_order::ActivateStopLimit{path_cursor, winner->price};
            } else if (winner->kind == Kind::BeginTrail) {
                transition = native_order::BeginTrailTracking{path_cursor, winner->price};
            } else {
                transition = native_order::ActivateTrail{path_cursor, winner->price};
            }
            native_order::Preparation<native_order::PreparedMutation> prep;
            try {
                prep = requests_.prepare_trigger(winner->handle, transition,
                                                driver_class, next_timeline_ordinal_, eval.cohort_side,
                                                activation_grid(*spec));
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return;
            }
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) {
                skip(skip_key(winner->incarnation, winner->kind));
                continue;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement, P)) {
                return;
            }
            if (winner->kind == Kind::ActivateStop || winner->kind == Kind::ActivateTrail) {
                const auto* activated = requests_.find_live(winner->handle);
                if (activated) {
                    const bool buy = scratch_request_is_buy(engine, *activated);
                    CandidateProvenance transfer;
                    transfer.handle = winner->handle;
                    transfer.point_ordinal = P;
                    transfer.kind = Kind::Fill;
                    transfer.trigger_state_index = activated->trigger_state.index();
                    transfer.is_buy = buy;
                    transfer.trigger_level = level_for(*activated, Kind::Fill, buy);
                    transfer.t = winner->t;
                    transfer.raw_price = winner->price;
                    transfer.at_level = winner->at_level;
                    candidate_provenance.push_back(std::move(transfer));
                }
            }
            continue;
        }

        live = scanned_request(*winner);
        if (!live) continue;
        const bool buy = scratch_request_is_buy(engine, *live);
        const bool limit_governed =
            std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state);
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
        // An opted-in grid books on the tick ladder BEFORE slippage, which is
        // itself a whole number of ticks.
        const double basis = grid_fill_basis(*spec, winner->price, buy, limit_governed);
        double resolved = native_matching::apply_slippage(basis, slip, buy);
        const auto& trigger = live->request().trigger;
        // Limit protection applies only after finite slippage arithmetic.
        // min/max must not turn an overflowed price into an executable limit.
        if (std::isfinite(resolved) && limit_governed) {
            double level = 0.0;
            bool fill_through = false;
            if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
                level = limit->price;
                fill_through = limit->fill_through;
            } else if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) {
                level = sl->limit;
            }
            if (!fill_through) {
                resolved = native_matching::protect_limit(
                    resolved, grid_limit_cap(*spec, level, buy), buy);
            }
        }
        native_order::NativeCandidatePriceKind price_kind =
            native_order::NativeCandidatePriceKind::PointPrice;
        if (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state)
            || std::holds_alternative<native_order::StopActive>(live->trigger_state)
            || std::holds_alternative<native_order::TrailActive>(live->trigger_state)) {
            price_kind = winner->at_level
                ? native_order::NativeCandidatePriceKind::TriggerLevel
                : native_order::NativeCandidatePriceKind::PointPrice;
        } else if (!std::holds_alternative<native_order::MarketReady>(live->trigger_state)) {
            throw std::logic_error("native nonfillable state reached matched coordinator");
        }
        const auto anchor = execution_anchor(path_cursor, resolved);
        consuming_request_ = true;
        auto outcome = consume_matched_request(engine, winner->handle, eval, winner->price,
            resolved, anchor, price_kind, NativeCurrentPriceRule::AsPresented,
            price_kind == native_order::NativeCandidatePriceKind::TriggerLevel
                && winner->shared_cursor_collision);
        consuming_request_ = false;
        if (failed()) return;
        if (!outcome) skip(skip_key(winner->incarnation, winner->kind));
        drain_applied_notifications(engine);
    }
    if (continuous && t_cursor < 1.0 && !failed()) {
        apply_excursion(engine, to_price);
        observe_trails(engine, point, make_cursor(point, 1.0), continuous, to_price);
    }
}

std::optional<NativeCurrentPointView> NativeExecutionConsumer::current_execution_point() const {
    if (!in_callback_ || !current_frame_ || !std::holds_alternative<NativeRunning>(state_))
        return std::nullopt;
    return current_frame_->point;
}

std::optional<NativeTrailState> NativeExecutionConsumer::trail_state(
        const BacktestEngine& engine, const native_order::RequestHandle& target) const {
    const auto* live = requests_.find_live(target);
    if (!live || !std::holds_alternative<native_order::Trail>(live->request().trigger)) {
        return std::nullopt;
    }

    NativeTrailState state;
    if (std::holds_alternative<native_order::TrailWaitArm>(live->trigger_state)) {
        return state;
    }

    if (const auto* tracking = std::get_if<native_order::TrailTrack>(&live->trigger_state)) {
        state.best_price = tracking->best;
    } else if (const auto* active = std::get_if<native_order::TrailActive>(
                   &live->trigger_state)) {
        state.best_price = active->best_at_trigger;
    } else {
        return std::nullopt;
    }
    state.activated = true;

    const auto& trail = std::get<native_order::Trail>(live->request().trigger);
    if (!native_matching::checked_trail_stop(
            state.best_price, trail.offset, request_is_buy(engine, *live),
            &state.current_level, ladder_tick())) {
        return std::nullopt;
    }
    for (auto it = requests_.history().rbegin(); it != requests_.history().rend(); ++it) {
        const auto* activated = std::get_if<native_order::ActivatedEvent>(&*it);
        if (activated && activated->definition
            && activated->definition->handle == target
            && activated->kind == native_order::ActivationKind::TrailArm) {
            state.activation_ordinal = activated->ordinal;
            break;
        }
    }
    return state;
}

std::optional<NativeCurrentRefusal> NativeExecutionConsumer::validate_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const {
    using Refusal = NativeCurrentRefusal;
    if (!std::holds_alternative<NativeRunning>(state_)) return Refusal::NoExecutionContext;
    if (consuming_request_) return Refusal::Reentrant;
    if (!current_execution_point()) return Refusal::NoExecutionContext;
    if (callback_phase_ != CallbackPhase::PreOpen
        && callback_phase_ != CallbackPhase::Bar
        && callback_phase_ != CallbackPhase::Applied
        && callback_phase_ != CallbackPhase::Tick) {
        return Refusal::NoExecutionContext;
    }
    if (!projection_ok(engine)) return Refusal::ConfigurationMismatch;
    if (command.target.incarnation == 0 || command.target.run != requests_.identity())
        return Refusal::InvalidHandle;
    const auto* live = requests_.find_live(command.target);
    if (!live) return Refusal::NotWorking;
    if (live->birth().acceptance_ordinal <= current_frame_->acceptance_cutoff)
        return Refusal::NotAcceptedInCallback;
    if (command.price_rule != NativeCurrentPriceRule::AsPresented
        && command.price_rule != NativeCurrentPriceRule::NearestTick)
        return Refusal::UnsupportedRequest;
    const auto& request = live->request();
    if (!std::holds_alternative<native_order::Market>(request.trigger)
        || !std::holds_alternative<native_order::ImmediateRemaining>(request.capacity))
        return Refusal::UnsupportedRequest;
    if (std::holds_alternative<native_order::WaitForApplied>(request.owner)
        || std::holds_alternative<native_order::Wait>(live->authority)
        || std::holds_alternative<native_order::RemainingUnbound>(live->remaining))
        return Refusal::UnreadyOwner;
    if (const auto* reduce = std::get_if<native_order::Reduce>(&request.intent)) {
        if (std::holds_alternative<native_order::OwnerOpenedUnits>(reduce->size))
            return Refusal::UnsupportedRequest;
    } else if (std::holds_alternative<native_order::Sized>(request.intent)) {
        if (!std::holds_alternative<native_order::Independent>(request.owner))
            return Refusal::UnsupportedRequest;
    } else if (std::holds_alternative<native_order::Transact>(request.intent)) {
        if (!std::holds_alternative<native_order::Independent>(request.owner))
            return Refusal::UnsupportedRequest;
    } else if (std::holds_alternative<native_order::ReverseTo>(request.intent)) {
        if (!std::holds_alternative<native_order::Independent>(request.owner))
            return Refusal::UnsupportedRequest;
    } else if (const auto* sized = std::get_if<native_order::HostSized>(&request.intent)) {
        if (sized->kind == native_order::HostSizedKind::Open
            && !std::holds_alternative<native_order::Independent>(request.owner)) {
            return Refusal::UnsupportedRequest;
        }
    } else if (!std::holds_alternative<native_order::Flatten>(request.intent)) {
        return Refusal::UnsupportedRequest;
    }
    if (!std::holds_alternative<native_order::Independent>(request.owner)
        && !std::holds_alternative<native_order::BindOpening>(request.owner)
        && !std::holds_alternative<native_order::BindOpenings>(request.owner)
        && !std::holds_alternative<native_order::BindCohort>(request.owner))
        return Refusal::UnsupportedRequest;
    const auto target = read_target(engine, live);
    if (std::holds_alternative<native_order::OpeningClose>(live->authority)) {
        if (!target.opening || !target.opening->has_live_matching_lot) return Refusal::UnreadyOwner;
    } else if (std::holds_alternative<native_order::OpeningsClose>(live->authority)) {
        if (target.openings.empty()) return Refusal::InvalidSelection;
        bool any_live = false;
        for (const auto& row : target.openings) any_live |= row.has_live_matching_lot;
        if (!any_live) return Refusal::UnreadyOwner;
    } else if (std::holds_alternative<native_order::CohortClose>(live->authority)) {
        bool any_live = false;
        for (const auto& row : target.openings) any_live |= row.has_live_matching_lot;
        if (!any_live) return Refusal::UnreadyOwner;
    }
    return std::nullopt;
}

NativeCoordinate NativeExecutionConsumer::current_execution_coordinate(uint64_t ordinal) const {
    auto coordinate = current_frame_->point.decision.coordinate;
    coordinate.ordinal = ordinal;
    coordinate.provenance = NativePriceProvenance::CurrentExecution;
    // A31(b): an applied notification keeps its exact decision coordinate
    // current. A newborn command in that callback must therefore both be born
    // and execute there, even if input delivery has already raised the future
    // decision floor. Other current-execution sites retain the floor rule.
    const bool applied_point_is_current = callback_phase_ == CallbackPhase::Applied
        && current_frame_
        && current_frame_->point.decision.coordinate.effective_time_ms >= decision_floor();
    if (!applied_point_is_current) {
        coordinate.effective_time_ms = std::max(coordinate.effective_time_ms, decision_floor());
    }
    return coordinate;
}

double NativeExecutionConsumer::current_price(const BacktestEngine& engine,
        const native_order::LiveRequest& live, NativeCurrentPriceRule rule) const {
    const double basis = rule == NativeCurrentPriceRule::NearestTick
        ? engine.bar_fill_price(current_frame_->point.price) : current_frame_->point.price;
    // An Independent close has not yet been bound during read-only preview.
    const bool buy = std::holds_alternative<native_order::UnboundBookClose>(live.authority)
        ? engine.position_side_ == PositionSide::SHORT : request_is_buy(engine, live);
    const auto* spec = spec_ptr();
    // Current execution is a market fill by contract, so the grid rounds it
    // on the adverse side before the same single slippage step.
    const double on_grid = grid_fill_basis(*spec, basis, buy, /*limit_governed=*/false);
    return native_matching::apply_slippage(on_grid,
        static_cast<double>(spec->slippage_ticks) * spec->price_tick, buy);
}

NativeCurrentExecutionPreview NativeExecutionConsumer::inspect_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const {
    NativeCurrentExecutionPreview out;
    out.refusal = validate_current_execution(engine, command);
    if (out.refusal) return out;
    const auto* live = requests_.find_live(command.target);
    native_order::MatchCursor cursor;
    cursor.point = current_execution_coordinate(next_timeline_ordinal_);
    native_order::EvaluationContext evaluation;
    evaluation.cursor = cursor;
    evaluation.driver_class = native_order::DriverEligibilityClass::CurrentExecution;
    evaluation.existing_matching_bit = true;
    evaluation.cohort_side = cohort_side(engine, *live);
    const auto* host_sized = host_sized_intent(*live);
    const auto* native_sized = sized_intent(*live);
    const auto* scope_fraction = scope_fraction_intent(*live);
    const bool unresolved = (host_sized || native_sized || scope_fraction)
        && (std::holds_alternative<native_order::RemainingDeferred>(live->remaining)
            || std::holds_alternative<native_order::NoTarget>(live->remaining));
    const bool opening_shapes = (host_sized
        && host_sized->kind == native_order::HostSizedKind::Open)
        || native_sized != nullptr;
    const bool closing_size = (host_sized
            && host_sized->kind == native_order::HostSizedKind::Close)
        || scope_fraction != nullptr;
    const native_order::HostSizedKind terms_kind = opening_shapes || native_sized
        ? native_order::HostSizedKind::Open : native_order::HostSizedKind::Close;
    const std::optional<native_order::Side> terms_side = host_sized
        ? host_sized->side
        : (native_sized ? std::optional<native_order::Side>{native_sized->side}
                        : std::nullopt);
    if (unresolved && closing_size
        && std::holds_alternative<native_order::UnboundBookClose>(live->authority)
        && engine.position_side_ == PositionSide::FLAT) {
        out.settlement_readiness = execution::Status::NoEffect;
        return out;
    }
    const double raw_price = current_frame_->point.price;
    const double default_resolved = current_price(engine, *live, command.price_rule);
    auto facts = build_terms_facts(engine, *live, evaluation,
        native_order::NativeCandidatePriceKind::CurrentQuote, command.price_rule,
        raw_price, default_resolved);
    std::optional<double> kernel_units;
    if (unresolved && (native_sized || scope_fraction)) {
        kernel_units = resolve_sized_units(engine, *live, facts);
        if (kernel_units) facts.remaining = native_order::RemainingUnits{*kernel_units};
    }
    native_order::ExecutionTerms terms;
    struct PreviewSeal {
        bool& value;
        explicit PreviewSeal(bool& v) : value(v) { value = true; }
        ~PreviewSeal() { value = false; }
    };
    {
        PreviewSeal seal(consuming_request_);
        const auto* host = native_host(engine);
        if (!host) throw std::logic_error("native terms require a native host");
        terms = host->resolve_execution_terms(facts);
    }
    // A hook-owned public call may latch or perturb configuration. Preview
    // reports the refusal without adding another mutation of its own.
    if (failed() || engine.abort_requested_.load(std::memory_order_relaxed)
        || !projection_ok(engine)) {
        out.refusal = NativeCurrentRefusal::ConfigurationMismatch;
        return out;
    }
    if (unresolved && (native_sized || scope_fraction) && !terms.units) {
        terms.units = kernel_units;
    }
    if (!unresolved && (terms.units || terms.shape != native_order::OpeningShape::Transact)) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && !terms.units) {
        out.terms_rejection = native_order::MatchRejectReason::TermsUnresolved;
        return out;
    }
    if (unresolved && !opening_shapes
        && terms.shape != native_order::OpeningShape::Transact) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && terms.shape != native_order::OpeningShape::Transact
        && terms.shape != native_order::OpeningShape::ReverseTo
        && terms.shape != native_order::OpeningShape::CloseOpposite) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (terms.units && (!std::isfinite(*terms.units) || *terms.units < 0.0)) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (!execution_terms_grid_representable(
            terms, host_sized, unresolved, facts.scope_exposure_units, spec_ptr())) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    double after = 0.0;
    double deduction = 0.0;
    bool exhausted = false;
    double binding_allowance = 0.0;
    if (unresolved) {
        if (!native_order::WorkingRequestCore::effective_host_units(
                live->pending, *terms.units, &deduction, &after, &exhausted)) {
            throw std::overflow_error("native host-sized deduction is not representable");
        }
        if (const auto* budget = std::get_if<native_order::PointBudget>(&live->request().capacity)) {
            binding_allowance = std::min(after, budget->units);
        } else {
            binding_allowance = after;
        }
    }
    const bool explicit_reversal = std::holds_alternative<native_order::ReverseTo>(
        live->request().intent);
    const bool requires_opposite = explicit_reversal || (unresolved
        && (terms.shape == native_order::OpeningShape::ReverseTo
            || terms.shape == native_order::OpeningShape::CloseOpposite));
    if (requires_opposite && facts.opposite_book_units == 0.0) {
        out.terms_rejection = native_order::MatchRejectReason::NoOppositeExposure;
        return out;
    }
    if (unresolved && terms.shape == native_order::OpeningShape::CloseOpposite
        && *terms.units > facts.opposite_book_units) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && terms.shape != native_order::OpeningShape::Transact
        && after > binding_allowance) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && *terms.units == 0.0) {
        out.settlement_readiness = execution::Status::NoEffect;
        return out;
    }
    if (unresolved && after == 0.0 && deduction > 0.0) {
        out.terms_cancellation = native_order::CancelReason::Group;
        return out;
    }
    std::optional<native_order::ExecutionPlan> plan;
    if (host_sized || native_sized || scope_fraction) {
        if (unresolved) {
            plan = plan_from_terms(terms_kind, terms_side, terms.shape,
                                   after, binding_allowance, facts.opposite_book_units);
        } else if (const auto* remaining = std::get_if<native_order::RemainingUnits>(
                       &live->remaining)) {
            plan = plan_from_terms(terms_kind, terms_side,
                native_order::OpeningShape::Transact, remaining->q,
                allowance_left_at(facts.allowance, cursor.point.ordinal),
                facts.opposite_book_units);
        }
    }
    const auto candidate = inspect_candidate(engine, *live, cursor, terms.resolved_price,
                                             plan ? &*plan : nullptr);
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = cursor.point.effective_time_ms;
    context.interval_index = cursor.point.interval_index;
    if (const auto* reversal = std::get_if<execution::ReverseTo>(&candidate.physical)) {
        out.settlement_readiness = engine.preview_native_settlement_commit(
            *reversal, candidate.fill, context, out.account, out.closed_row_pnl);
    } else {
        out.settlement_readiness = engine.preview_native_settlement_commit(
            narrow_action(candidate.physical), candidate.fill, context, candidate.financial_scope,
            candidate.selected ? &*candidate.selected : nullptr, out.account, out.closed_row_pnl);
    }
    return out;
}

NativeCurrentExecutionResult NativeExecutionConsumer::execute_current(
        BacktestEngine& engine, const NativeCurrentExecution& command) {
    if (!std::holds_alternative<NativeRunning>(state_)) return NativeCurrentRefusal::NoExecutionContext;
    if (consuming_request_) return NativeCurrentRefusal::Reentrant;
    if (!current_execution_point()) return NativeCurrentRefusal::NoExecutionContext;
    // The in-callback guard must run before allocating a point or inspecting a
    // temporarily modified fee, FX, price tick, calendar or admission setting.
    if (!check_abort_or_projection(engine, NativeFailureOperation::Command))
        throw std::runtime_error("native current execution projection/abort failure");
    try {
        if (auto refusal = validate_current_execution(engine, command)) return *refusal;
        consuming_request_ = true;
        NativeDriverPoint point;
        const auto ordinal = take_ordinal(engine);
        if (failed()) throw std::runtime_error("native current point allocation failed");
        point.coordinate = current_execution_coordinate(ordinal);
        point.raw_price = current_frame_->point.price;
        point.matching = true;
        record_driver(point);
        native_order::EvaluationContext evaluation;
        evaluation.cursor = make_cursor(point, 0.0);
        evaluation.driver_class = native_order::DriverEligibilityClass::CurrentExecution;
        evaluation.existing_matching_bit = true;
        const auto* live = requests_.find_live(command.target);
        evaluation.cohort_side = live ? cohort_side(engine, *live) : std::nullopt;
        const auto history_before = requests_.history().size();
        auto prep = requests_.prepare_evaluation(command.target, evaluation,
            read_target(engine, live), next_timeline_ordinal_);
        if (const auto* error = std::get_if<native_order::PreparationError>(&prep)) {
            fail_preparation(engine, *error, NativeFailureOperation::Settlement);
            throw std::runtime_error("native current evaluation failed");
        }
        if (auto* mutation = std::get_if<native_order::PreparedMutation>(&prep)) {
            if (!install_mutation(engine, std::move(*mutation), NativeFailureOperation::Settlement,
                                  point.coordinate.ordinal))
                throw std::runtime_error("native current evaluation install failed");
        }
        live = requests_.find_live(command.target);
        if (live && !same_allowance_bits(
                native_order::WorkingRequestCore::evaluated_allowance(
                    *live, point.coordinate.ordinal), live->allowance)) {
            throw std::logic_error("native current evaluated allowance mismatch");
        }
        if (!live) {
            // Flat Independent closes terminate during the existing evaluation.
            if (requests_.history().size() <= history_before)
                throw std::logic_error("native current evaluation lost target without outcome");
            const auto* no_effect = std::get_if<native_order::NoEffectEvent>(&requests_.history().back());
            if (!no_effect) throw std::logic_error("native current evaluation has no terminal outcome");
            NativeCurrentExecutionResult outcome{*no_effect};
            const native_order::EventId cause{command.target.run, no_effect->ordinal};
            drain_parent_terminal(engine, cause, command.target, NativeFailureOperation::Settlement);
            if (failed()) throw std::runtime_error("native current terminal drain failed");
            consuming_request_ = false;
            return outcome;
        }
        const auto anchor = current_frame_->point;
        const double resolved = current_price(engine, *live, command.price_rule);
        // The execution settles at its own cursor, and consume_matched_request
        // presents that instant to everything it runs -- the terms, the
        // inspection, the admission and the settlement all convert there. The
        // clock the calling frame presents is the host's, and a host command
        // hands it back unchanged (R5 lane F3), as submit and replace do.
        const std::int64_t presented_clock = engine.current_bar_.timestamp;
        auto outcome = consume_matched_request(engine, command.target, evaluation,
            anchor.price, resolved, anchor,
            native_order::NativeCandidatePriceKind::CurrentQuote, command.price_rule);
        engine.current_bar_.timestamp = presented_clock;
        if (failed() || !outcome) throw std::runtime_error("native current execution failed");
        consuming_request_ = false;
        return std::move(*outcome);
    } catch (const std::bad_alloc& e) {
        consuming_request_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Settlement});
        render(engine, e.what());
        throw;
    } catch (const std::exception& e) {
        consuming_request_ = false;
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                                NativeFailureOperation::Settlement});
        render(engine, e.what());
        throw;
    } catch (...) {
        consuming_request_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Settlement});
        throw;
    }
}

// ---------------------------------------------------------------------------
// L5 calculation timing
//
// The chronology at one point is the established one with exactly one
// addition. Match and settle, then drain the applied notifications FIFO under
// the existing re-entrancy guard, delivering on_native_applied for each; a
// spec that asked for BarCloseAndFills (or above) then drives ONE
// recalculation at that event's own cursor, bounded by
// max_recalculations_per_point for the point. A request born in any of these
// callbacks keeps the existing birth rule: born_on_remaining_path and the
// drain order are untouched. A recalculation never records a report point —
// only the script calculation does.
// ---------------------------------------------------------------------------

NativeCalculationTrigger NativeExecutionConsumer::calculation_trigger() const noexcept {
    const auto* spec = spec_ptr();
    return spec ? spec->calculation : NativeCalculationTrigger::BarClose;
}

bool NativeExecutionConsumer::recalculates_on_fills() const noexcept {
    const auto trigger = calculation_trigger();
    return trigger == NativeCalculationTrigger::BarCloseAndFills
        || trigger == NativeCalculationTrigger::EveryModeledPoint;
}

void NativeExecutionConsumer::open_point_epoch() noexcept { ++point_epoch_; }

bool NativeExecutionConsumer::claim_recalculation() noexcept {
    const auto* spec = spec_ptr();
    const uint32_t budget = spec ? spec->max_recalculations_per_point : 0;
    if (recalc_epoch_ != point_epoch_) {
        recalc_epoch_ = point_epoch_;
        recalc_epoch_count_ = 0;
    }
    if (recalc_epoch_count_ >= budget) {
        ++recalculations_skipped_;
        return false;
    }
    ++recalc_epoch_count_;
    return true;
}

void NativeExecutionConsumer::note_partial_point(
        int64_t script_open_ms, double price, double volume_delta) {
    if (!std::isfinite(price)) return;
    if (!partial_has_ || partial_script_open_ms_ != script_open_ms) {
        partial_ = Bar{price, price, price, price, 0.0, script_open_ms};
        partial_script_open_ms_ = script_open_ms;
        partial_has_ = true;
    } else {
        if (price > partial_.high) partial_.high = price;
        if (price < partial_.low) partial_.low = price;
        partial_.close = price;
    }
    if (volume_delta > 0.0 && std::isfinite(volume_delta)) partial_.volume += volume_delta;
}

void NativeExecutionConsumer::clear_partial() noexcept {
    partial_has_ = false;
    partial_ = Bar{};
    partial_script_open_ms_ = 0;
}

std::optional<Bar> NativeExecutionConsumer::partial_bar() const {
    if (!partial_has_) return std::nullopt;
    return partial_;
}

NativeCurrentPointView NativeExecutionConsumer::point_frame_view(
        const NativeDriverPoint& point) const {
    NativeCurrentPointView view;
    // The delivery loop already owns this bar's sub-bar labels and driver
    // statistics; only the cursor's own facts are replaced here.
    view.decision = callback_context_;
    view.decision.coordinate = point.coordinate;
    view.decision.decision_floor_ms = decision_floor();
    if (auto input = input_interval_at(point.coordinate.open_ms))
        view.decision.input_interval = *input;
    if (auto script = script_interval_at(point.coordinate.open_ms))
        view.decision.script_interval = *script;
    view.price = point.raw_price;
    view.quote_kind = NativeCurrentQuoteKind::MarketDecision;
    view.quote_origin_ordinal = point.coordinate.ordinal;
    return view;
}

void NativeExecutionConsumer::enter_point_frame(
        BacktestEngine& engine, const NativeCurrentPointView& point, CallbackPhase phase) {
    current_frame_ = CurrentExecutionFrame{point, next_timeline_ordinal_ - 1};
    callback_context_ = point.decision;
    callback_context_.decision_floor_ms = decision_floor();
    current_frame_->point.decision.decision_floor_ms = decision_floor();
    engine.current_bar_.timestamp = std::max(
        point.decision.coordinate.effective_time_ms, decision_floor());
    in_callback_ = true;
    callback_phase_ = phase;
}

void NativeExecutionConsumer::invoke_recalculation(
        BacktestEngine& engine, const Bar& bar, NativeCalculationReason reason,
        const native_order::ExecutionAppliedEvent* cause) {
    auto* host = native_host(engine);
    if (!host) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        return;
    }
    const uint64_t ordinal = callback_context_.coordinate.ordinal;
    ++recalculations_;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_recalculate(bar, presented, reason, cause);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Callback, ordinal});
        render(engine, e.what());
        return;
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, ordinal});
        render(engine, e.what());
        return;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, ordinal});
        return;
    }
    finish_callback(engine, ordinal);
}

const Bar& NativeExecutionConsumer::calculating_bar(const BacktestEngine& engine) const noexcept {
    return calculating_bar_has_ ? calculating_bar_ : engine.current_bar_;
}

void NativeExecutionConsumer::recalculate_at_point(
        BacktestEngine& engine, const Bar& bar, const NativeDriverPoint& point) {
    if (failed()) return;
    if (calculation_trigger() != NativeCalculationTrigger::EveryModeledPoint) return;
    if (in_callback_ || consuming_request_ || draining_notifications_) return;
    enter_point_frame(engine, point_frame_view(point), CallbackPhase::Tick);
    invoke_recalculation(engine, bar, NativeCalculationReason::Tick, nullptr);
}

void NativeExecutionConsumer::invoke_sub_bar_callback(
        BacktestEngine& engine, const Bar& sub, const NativeDriverPoint& point) {
    if (failed()) return;
    if (in_callback_ || consuming_request_ || draining_notifications_) return;
    auto* host = native_host(engine);
    if (!host) return;
    enter_point_frame(engine, point_frame_view(point), CallbackPhase::Tick);
    const uint64_t ordinal = callback_context_.coordinate.ordinal;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_sub_bar(sub, presented);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Callback, ordinal});
        render(engine, e.what());
        return;
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, ordinal});
        render(engine, e.what());
        return;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, ordinal});
        return;
    }
    finish_callback(engine, ordinal);
}

void NativeExecutionConsumer::enqueue_applied_notification(AppliedNotification notification) {
    applied_notifications_.push_back(std::move(notification));
}

void NativeExecutionConsumer::finish_callback(BacktestEngine& engine, uint64_t ordinal) {
    in_callback_ = false;
    callback_phase_ = CallbackPhase::None;
    current_frame_.reset();
    if (check_abort_or_projection(engine, NativeFailureOperation::Callback, ordinal))
        drain_applied_notifications(engine);
}

void NativeExecutionConsumer::invoke_applied_callback(
        BacktestEngine& engine, const AppliedNotification& notification) {
    try {
        auto* host = native_host(engine);
        if (!host) throw std::logic_error("native notification requires native host");
        // Owning event and frame values survive submit/replace and further
        // synchronous executions reallocating the append-only history/queue.
        const auto applied = std::get<native_order::ExecutionAppliedEvent>(
            requests_.history().at(notification.history_index));
        if (applied.ordinal != notification.ordinal)
            throw std::logic_error("native notification identity mismatch");
        current_frame_ = CurrentExecutionFrame{notification.point, next_timeline_ordinal_ - 1};
        callback_context_ = notification.point.decision;
        callback_context_.decision_floor_ms = decision_floor();
        current_frame_->point.decision.decision_floor_ms = decision_floor();
        engine.current_bar_.timestamp = std::max(
            notification.point.decision.coordinate.effective_time_ms, decision_floor());
        in_callback_ = true;
        callback_phase_ = CallbackPhase::Applied;
        const auto presented = callback_context_;
        host->on_native_applied(applied, presented);
        // L4: the margin receipt of this same fill, after the ordinary applied
        // notification and with the same cursor.
        if (notification.margin_call_index
            && *notification.margin_call_index < requests_.history().size()) {
            const auto* margin_call = std::get_if<native_order::MarginCallEvent>(
                &requests_.history().at(*notification.margin_call_index));
            if (margin_call) {
                const auto receipt = *margin_call;
                host->on_native_margin_call(receipt);
            }
        }
        finish_callback(engine, notification.ordinal);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Callback,
                                   notification.ordinal});
        render(engine, e.what());
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, notification.ordinal});
        render(engine, e.what());
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, notification.ordinal});
    }
}

void NativeExecutionConsumer::drain_applied_notifications(BacktestEngine& engine) {
    if (in_callback_ || consuming_request_ || draining_notifications_ || failed()) return;
    draining_notifications_ = true;
    while (notification_head_ < applied_notifications_.size() && !failed()) {
        const auto notification = applied_notifications_[notification_head_++];
        raise_floor(notification.point.decision.coordinate.effective_time_ms);
        invoke_applied_callback(engine, notification);
        if (failed() || !recalculates_on_fills()) continue;
        // The event is delivered whatever the budget says; only the
        // recalculation it would drive is dropped once this point has spent
        // max_recalculations_per_point. Executions the callback drives
        // through execute_current land at the same cursor, so they spend the
        // same point's budget and the cascade is bounded.
        if (!claim_recalculation()) continue;
        const auto applied = std::get<native_order::ExecutionAppliedEvent>(
            requests_.history().at(notification.history_index));
        enter_point_frame(engine, notification.point, CallbackPhase::Applied);
        invoke_recalculation(engine, calculating_bar(engine),
                             NativeCalculationReason::OrderFill, &applied);
    }
    draining_notifications_ = false;
    if (!failed()) {
        const bool drained = !applied_notifications_.empty();
        const auto last = drained ? applied_notifications_.back() : AppliedNotification{};
        applied_notifications_.clear();
        notification_head_ = 0;
        // L4: every applied fill re-arms the margin model against the book it
        // left behind. Inert for a run that declares no margin model.
        if (drained && margin_model() != nullptr) {
            native_order::MatchCursor cursor;
            cursor.point = last.point.decision.coordinate;
            maintain_margin_liquidation(engine, cursor,
                                        last.point.decision.coordinate.path_phase,
                                        last.point.price,
                                        NativeMarginCheckKind::AfterApplied);
        }
        // L9: the second evaluation point. Every fill of this drain has
        // already been counted; the account facts are measured once, at the
        // cursor the drain ended on.
        if (drained && risk_limits() != nullptr && !failed()) {
            risk_evaluate(engine, last.point, CallbackPhase::Applied);
        }
    }
}

void NativeExecutionConsumer::invoke_bar_open_callback(
        BacktestEngine& engine, const Bar& bar, const NativeDriverPoint& point) {
    auto* host = native_host(engine);
    if (!host) return;
    callback_context_.coordinate = point.coordinate;
    callback_context_.decision_floor_ms = decision_floor();
    if (auto input = input_interval_at(point.coordinate.open_ms)) {
        callback_context_.input_interval = *input;
    }
    if (auto script = script_interval_at(point.coordinate.open_ms)) {
        callback_context_.script_interval = *script;
    }
    if (callback_context_.sub_count <= 0) callback_context_.sub_count = 1;
    if (callback_context_.sub_index < 0) callback_context_.sub_index = 0;
    if (callback_context_.sub_bar_open_ms == 0) {
        callback_context_.sub_bar_open_ms = point.coordinate.open_ms;
    }
    if (callback_context_.script_bar_open_ms == 0) {
        callback_context_.script_bar_open_ms = point.coordinate.open_ms;
    }
    NativeCurrentPointView current;
    current.decision = callback_context_;
    current.price = point.raw_price;
    current.quote_kind = NativeCurrentQuoteKind::MarketDecision;
    current.quote_origin_ordinal = point.coordinate.ordinal;
    current_frame_ = CurrentExecutionFrame{current, next_timeline_ordinal_ - 1};
    open_point_epoch();
    // L5 open-bar view. OpenOnly masks this one callback's lookahead: the
    // host sees H = L = C = open and no volume, and so does current_bar_
    // while the callback runs. Nothing else moves — the complete bar is
    // restored before the open match, so matching, fills and every later
    // callback are exactly what Complete presents.
    const auto* view_spec = spec_ptr();
    const bool open_only = view_spec
        && view_spec->open_bar_view == NativeOpenBarView::OpenOnly;
    const Bar open_view = open_only
        ? Bar{bar.open, bar.open, bar.open, bar.open, 0.0, bar.timestamp} : bar;
    engine.current_bar_ = open_view;
    engine.current_bar_.timestamp = point.coordinate.effective_time_ms;
    in_callback_ = true;
    callback_phase_ = CallbackPhase::PreOpen;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_bar_open(open_view, presented);
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       point.coordinate.ordinal});
            render(engine, e.what());
        }
        return;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       point.coordinate.ordinal});
            render(engine, "native bar-open callback exception");
        }
        return;
    }
    if (open_only) {
        const int64_t stamp = engine.current_bar_.timestamp;
        engine.current_bar_ = bar;
        engine.current_bar_.timestamp = stamp;
    }
    finish_callback(engine, point.coordinate.ordinal);
}

bool NativeExecutionConsumer::invoke_input_callback(
        BacktestEngine& engine, const Bar& bar, const NativeInputContext& context) {
    auto* host = native_host(engine);
    if (!host) return true;
    input_callback_context_ = context;
    input_callback_bar_ = bar;
    in_callback_ = true;
    try {
        host->on_native_input(bar, context);
    } catch (const std::exception& e) {
        in_callback_ = false;
        input_callback_context_.reset();
        input_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input});
            render(engine, e.what());
        }
        return false;
    } catch (...) {
        in_callback_ = false;
        input_callback_context_.reset();
        input_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input});
            render(engine, "native input callback exception");
        }
        return false;
    }
    in_callback_ = false;
    input_callback_context_.reset();
    input_callback_bar_.reset();
    return !failed();
}

bool NativeExecutionConsumer::invoke_tick_callback(
        BacktestEngine& engine, const Bar& bar, const NativeTickContext& context) {
    auto* host = native_host(engine);
    if (!host) return true;
    NativeTickContext presented = context;
    presented.decision.decision_floor_ms = decision_floor();
    tick_callback_context_ = presented;
    tick_callback_bar_ = bar;
    callback_context_ = presented.decision;
    NativeCurrentPointView current;
    current.decision = callback_context_;
    current.price = bar.close;
    current.quote_kind = NativeCurrentQuoteKind::MarketDecision;
    current.quote_origin_ordinal = callback_context_.coordinate.ordinal;
    current_frame_ = CurrentExecutionFrame{current, next_timeline_ordinal_ - 1};
    in_callback_ = true;
    callback_phase_ = CallbackPhase::Tick;
    try {
        host->on_native_tick(bar, presented);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        tick_callback_context_.reset();
        tick_callback_bar_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Input,
                                   context.decision.coordinate.ordinal});
        render(engine, e.what());
        return false;
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        tick_callback_context_.reset();
        tick_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input,
                                       context.decision.coordinate.ordinal});
            render(engine, e.what());
        }
        return false;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        tick_callback_context_.reset();
        tick_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input,
                                       context.decision.coordinate.ordinal});
            render(engine, "native tick callback exception");
        }
        return false;
    }
    finish_callback(engine, context.decision.coordinate.ordinal);
    tick_callback_context_.reset();
    tick_callback_bar_.reset();
    return !failed();
}

void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
                                              const NativeCoordinate& coordinate) {
    auto* host = native_host(engine);
    if (!host) return;
    callback_context_.coordinate = coordinate;
    callback_context_.decision_floor_ms = decision_floor_ms_;
    auto input = input_interval_at(bar.timestamp);
    auto script = script_interval_at(bar.timestamp);
    if (input) callback_context_.input_interval = *input;
    if (script) callback_context_.script_interval = *script;
    if (callback_context_.sub_count <= 0) callback_context_.sub_count = 1;
    if (callback_context_.sub_index < 0) callback_context_.sub_index = 0;
    if (callback_context_.sub_bar_open_ms == 0) callback_context_.sub_bar_open_ms = coordinate.open_ms;
    if (callback_context_.script_bar_open_ms == 0) callback_context_.script_bar_open_ms = coordinate.open_ms;
    NativeCurrentPointView point;
    point.decision = callback_context_;
    point.price = bar.close;
    current_frame_ = CurrentExecutionFrame{point, next_timeline_ordinal_ - 1};
    in_callback_ = true;
    callback_phase_ = CallbackPhase::Bar;
    open_point_epoch();
    try {
        const NativeDecisionContext presented = callback_context_;
        // L5: every calculation of the run is routed here. The default
        // on_native_recalculate forwards to on_native_bar, so a host that
        // never opted into another cadence sees exactly its own contract.
        host->on_native_recalculate(bar, presented, NativeCalculationReason::BarClose,
                                    nullptr);
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, e.what());
        }
        return;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, "native callback exception");
        }
        return;
    }
    ++engine.diag_script_bars_processed_;
    calculation_margin_check(engine, coordinate, bar.close);
    // L9: the third risk evaluation point is the script bar's own close
    // calculation, inside this still-open frame, so a breach that first
    // exists at the close blocks the openings of that same calculation
    // instead of the next bar's. Not entered at all without declared limits.
    if (risk_limits() != nullptr && !failed()) {
        NativeCurrentPointView close_point;
        close_point.decision = callback_context_;
        close_point.price = bar.close;
        risk_evaluate(engine, close_point, CallbackPhase::Bar, /*owns_frame=*/false);
    }
    finish_callback(engine, coordinate.ordinal);
}

// Session-day facts (R5 lane F5). A generic reading of the run's own
// calendar, with no platform rule in it: the in-session test and the session
// day are native_calendar's (in_session, session_day_ordinal), asked of the
// script bar's label and of its neighbours. A neighbour is the bar the run
// HOLDS on that side when it holds one -- the pumped batch input or stream
// warmup -- because the input, not the schedule, says where a trading day
// actually stopped (an early close the session string does not declare, a
// holiday). With nothing held, the neighbour is the calendar's slot one script
// width away, except at the run's own edges: its first bar opens its session
// day, and a batch's final bar closes it, because a batch is complete input.
// A stream's bars read on, because the stream continues. "Opens" reads the bar
// before exactly as "closes" reads the bar after, so every path that holds a
// bar agrees on the pair by construction.
void NativeExecutionConsumer::present_session_day(NativeDecisionContext& context,
                                                  int64_t label) const {
    context.in_session = false;
    context.opens_session_day = false;
    context.closes_session_day = false;
    context.closes_session_day_open_ended = false;
    if (script_tf_.is_calendar()) {
        // A D/W/M bar holds whole session days: it is each one's first and
        // last bar at once, whatever time of day its label reads.
        context.in_session = true;
        context.opens_session_day = true;
        context.closes_session_day = true;
        context.closes_session_day_open_ended = true;
        return;
    }
    const SessionPoint here = session_point(label);
    if (!here.in_session) return;
    context.in_session = true;
    // In session on this bar's own session day.
    const auto same_day = [&](int64_t other) {
        const SessionPoint there = session_point(other);
        return there.in_session && there.ordinal && here.ordinal
            && *there.ordinal == *here.ordinal;
    };
    const int64_t width = script_width_ms();
    std::optional<int64_t> step_before;
    std::optional<int64_t> step_after;
    if (width > 0) {
        if (label >= std::numeric_limits<int64_t>::min() + width) step_before = label - width;
        if (label <= std::numeric_limits<int64_t>::max() - width) step_after = label + width;
    }
    // The bucket this bar's inputs went into, when any have: its first and
    // last input indices place it in the run. A stream whose warmup ended
    // inside a script bar seals that bar in realtime, and it is still the
    // run's first; a print's forming bar has one once a slot has closed.
    const bool has_bucket = script_.has_data && script_.key == label;
    const bool held = has_bucket && pump_bars_ != nullptr;

    std::optional<int64_t> before;
    const bool run_start = has_bucket && script_.first_index <= 0;
    if (held && !run_start) {
        before = pumped_last_index_ == script_.first_index - 1
            ? std::optional<int64_t>(pumped_last_label_)
            : pumped_script_label(script_.first_index - 1);
    }
    if (!run_start && !before) before = step_before;
    context.opens_session_day = run_start || !before || !same_day(*before);

    std::optional<int64_t> after;
    bool run_end = false;
    if (held) {
        if (script_.last_index + 1 < pump_n_) {
            after = pumped_script_label(script_.last_index + 1);
        } else {
            const auto* running = std::get_if<NativeRunning>(&state_);
            run_end = running && running->phase == NativeRunPhase::Batch;
        }
        pumped_last_index_ = script_.last_index;
        pumped_last_label_ = label;
    }
    if (after) {
        context.closes_session_day = !same_day(*after);
        context.closes_session_day_open_ended = context.closes_session_day;
    } else {
        // Nothing held after the bar: the calendar's next slot, which is also
        // the open-ended reading of a batch's final bar.
        const bool scheduled = step_after && !same_day(*step_after);
        context.closes_session_day = run_end || scheduled;
        context.closes_session_day_open_ended = scheduled;
    }
}

// (in session, session-day ordinal) of one instant, through the memo of the
// session day last read: consecutive bars share a day, so a day is resolved
// once. A day the calendar cannot key is no session at all, exactly as
// native_calendar::in_session answers it.
NativeExecutionConsumer::SessionPoint NativeExecutionConsumer::session_point(int64_t ms) const {
    try {
        if (!session_day_memo_ || !session_day_memo_->holds(ms)) {
            auto day = native_calendar::session_day_at(calendar_, ms, calendar_memo_);
            if (!day) return {};
            if (!day->holds(ms)) return {day->in_session_at(ms), day->ordinal};
            session_day_memo_ = std::move(day);
        }
        return {session_day_memo_->in_session_at(ms), session_day_memo_->ordinal};
    } catch (...) {
        return {};
    }
}

// The script bar an input of the pumped array belongs to, labelled exactly as
// consume_confirmed_input keys it: the script interval of the input's own slot.
std::optional<int64_t> NativeExecutionConsumer::pumped_script_label(int index) const {
    if (pump_bars_ == nullptr || index < 0 || index >= pump_n_) return std::nullopt;
    const auto input = input_interval_at(pump_bars_[index].timestamp);
    if (!input) return std::nullopt;
    const auto script = script_interval_at(input->open_ms);
    if (!script) return std::nullopt;
    return script->open_ms;
}

// One script bar's width on a fixed (second / minute) timeframe, 0 otherwise.
int64_t NativeExecutionConsumer::script_width_ms() const noexcept {
    if (!script_tf_.valid() || !script_tf_.is_fixed()) return 0;
    const int64_t unit = script_tf_.unit() == native_calendar::TimeframeUnit::Second
        ? 1000 : 60'000;
    const int64_t count = script_tf_.count();
    if (count <= 0 || count > std::numeric_limits<int64_t>::max() / unit) return 0;
    return count * unit;
}

void NativeExecutionConsumer::deliver_confirmed_script(BacktestEngine& engine, const Bar& bar,
                                                       const NativeCoordinate& base) {
    const auto* spec = spec_ptr();
    calculating_bar_ = bar;
    calculating_bar_has_ = true;
    const bool high_first = path_uses_high_first(
        bar, spec ? spec->path_order : NativePathOrder::Auto);
    // L4 publishes this script bar's modeled waypoints so the margin model can
    // measure a breach against the adverse price the path still reaches.
    margin_path_bar_ = bar;
    margin_path_high_first_ = high_first;
    has_margin_path_ = margin_model() != nullptr;
    callback_context_ = NativeDecisionContext{};
    callback_context_.sub_index = 0;
    callback_context_.sub_count = 1;
    callback_context_.is_terminal_sub_bar = true;
    callback_context_.sub_bar_open_ms = base.open_ms;
    callback_context_.script_bar_open_ms = base.open_ms;
    driver_statistics_.sub_bars_per_script_bar = 1;
    driver_statistics_.samples_per_sub_bar = 0;
    callback_context_.driver_statistics = driver_statistics_;
    present_session_day(callback_context_, base.open_ms);
    auto emit_discrete = [&](double price, int64_t time, NativePriceProvenance provenance,
                             NativePathPhase phase, bool matching) {
        NativeDriverPoint point;
        point.coordinate = base;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = time;
        point.coordinate.source_price_time_ms = time;
        point.coordinate.provenance = provenance;
        point.coordinate.path_phase = phase;
        point.raw_price = price;
        point.matching = matching;
        record_driver(point);
        if (phase == NativePathPhase::Open) {
            // A discrete point IS its own cursor, so the bar so far is folded
            // before the callbacks that decide at it.
            note_partial_point(base.open_ms, price, 0.0);
            invoke_bar_open_callback(engine, bar, point);
            if (failed()) return;
            maintain_margin_liquidation(engine, make_cursor(point, 0.0), phase, price,
                                        NativeMarginCheckKind::BarOpen);
            if (failed()) return;
            // L9: the script bar's own open is one of the two risk evaluation
            // points. Not entered at all for a run that declares no limits.
            if (risk_limits() != nullptr) {
                risk_evaluate(engine, point_frame_view(point), CallbackPhase::PreOpen);
                if (failed()) return;
            }
        }
        match_discrete(engine, point);
        raise_floor(time);
        if (provenance != NativePriceProvenance::AfterCalculationClose)
            recalculate_at_point(engine, bar, point);
    };
    auto emit_segment = [&](double from, double to, int64_t time, NativePathPhase phase) {
        NativeDriverPoint point;
        point.coordinate = base;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = time;
        point.coordinate.source_price_time_ms = time;
        point.coordinate.provenance = NativePriceProvenance::Confirmed;
        point.coordinate.path_phase = phase;
        point.raw_price = to;
        point.matching = false;
        point.excursion = true;
        record_driver(point);
        match_segment(engine, point, from);
        // A segment's destination is only reached once the segment has been
        // consumed, so the bar so far never runs ahead of the cursor.
        note_partial_point(base.open_ms, to, 0.0);
        raise_floor(time);
        recalculate_at_point(engine, bar, point);
    };
    const int64_t open_time = script_.first_source_time_ms != 0
        ? script_.first_source_time_ms
        : (script_.first_open_ms != 0 ? script_.first_open_ms : bar.timestamp);
    const int64_t close_time = calculation_time(base);
    emit_discrete(bar.open, open_time, NativePriceProvenance::ModeledOHLCOpen,
                  NativePathPhase::Open, true);
    if (failed()) return;
    double prev = bar.open;
    if (high_first) {
        emit_segment(prev, bar.high, open_time, NativePathPhase::High);
        if (failed()) return;
        prev = bar.high;
        emit_segment(prev, bar.low, open_time, NativePathPhase::Low);
        prev = bar.low;
    } else {
        emit_segment(prev, bar.low, open_time, NativePathPhase::Low);
        if (failed()) return;
        prev = bar.low;
        emit_segment(prev, bar.high, open_time, NativePathPhase::High);
        prev = bar.high;
    }
    if (failed()) return;
    emit_segment(prev, bar.close, close_time, NativePathPhase::Close);
    if (failed()) return;
    // The modeled path is consumed: from the calculation on there is no
    // remaining waypoint for the margin model to measure a breach against.
    has_margin_path_ = false;
    NativeCoordinate calc = base;
    calc.ordinal = take_ordinal(engine);
    calc.effective_time_ms = close_time;
    calc.provenance = NativePriceProvenance::Calculation;
    calc.path_phase = NativePathPhase::None;
    raise_floor(close_time);
    engine.current_bar_ = bar;
    engine.bar_index_ = calc.interval_index;
    engine.current_bar_.timestamp = close_time;
    clear_partial();
    invoke_callback(engine, bar, calc);
    if (failed()) return;
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        emit_discrete(bar.close, close_time, NativePriceProvenance::AfterCalculationClose,
                      NativePathPhase::Close, true);
        if (failed()) return;
    }
    record_script_report_point(engine, base.open_ms);
}

void NativeExecutionConsumer::deliver_intrabar_script(
        BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base) {
    const auto* spec = spec_ptr();
    const auto* lower = spec ? spec->intrabar.lower() : nullptr;
    const auto* synthesized = spec ? spec->intrabar.synthesized_path() : nullptr;
    if (!lower && !synthesized) {
        deliver_confirmed_script(engine, bar, base);
        return;
    }

    // A sampled intrabar path re-evaluates the margin model at each delivered
    // sample instead of at the containing bar's remaining waypoints.
    has_margin_path_ = false;
    std::vector<const Bar*> sub_bars;
    if (lower) {
        const int64_t begin = base.open_ms;
        const int64_t end = script_.interval.next_input_open_ms;
        // preflight_intrabar_path refused this feed unless its stamps strictly
        // increase (NotStrictlyIncreasing, under either label policy), so the
        // bars of [begin, end) are one contiguous run of it, in feed order:
        // find the first by binary search and walk to the window's end. A
        // scan of the whole feed for every script bar made the run quadratic
        // in its length.
        auto candidate = std::lower_bound(
            lower->bars.begin(), lower->bars.end(), begin,
            [](const Bar& bar, int64_t time) { return bar.timestamp < time; });
        for (; candidate != lower->bars.end() && candidate->timestamp < end; ++candidate) {
            sub_bars.push_back(&*candidate);
        }
        // The generic lower-feed path follows the legacy pump's fallback: a
        // script bar with no assigned lower bars walks its own OHLC path.
        if (sub_bars.empty()) {
            deliver_confirmed_script(engine, bar, base);
            return;
        }
    } else {
        // synthesized is intentionally independent of [begin, end): raw
        // caller labels have a zero-width partition but still carry their
        // own complete OHLC path.
        sub_bars.push_back(&bar);
    }

    Bar script_bar = bar;
    script_bar.timestamp = base.open_ms;
    calculating_bar_ = script_bar;
    calculating_bar_has_ = true;
    const int sample_count = lower ? lower->samples : synthesized->samples;
    const auto distribution = lower ? lower->distribution : synthesized->distribution;
    const bool volume_weighted = lower ? lower->volume_weighted : synthesized->volume_weighted;
    const int volume_weighted_min_samples = lower
        ? lower->volume_weighted_min_samples : synthesized->volume_weighted_min_samples;
    const int volume_weighted_max_samples = lower
        ? lower->volume_weighted_max_samples : synthesized->volume_weighted_max_samples;
    double mean_volume = 0.0;
    if (volume_weighted) {
        for (const Bar* sub : sub_bars) mean_volume += sub->volume;
        mean_volume /= static_cast<double>(sub_bars.size());
    }
    std::vector<double> samples;
    callback_context_ = NativeDecisionContext{};
    callback_context_.sub_count = static_cast<int>(sub_bars.size());
    callback_context_.script_bar_open_ms = base.open_ms;
    driver_statistics_.intrabar_path_enabled = true;
    driver_statistics_.sub_bars_per_script_bar = static_cast<int>(sub_bars.size());
    driver_statistics_.samples_per_sub_bar = 0;
    callback_context_.driver_statistics = driver_statistics_;
    present_session_day(callback_context_, base.open_ms);
    const bool direct_sub_bar_corners = lower && sub_bars.size() > 1;
    const bool distribution_samples = synthesized || lower->sample_eligibility
        == IntrabarPath::SampleEligibility::DistributionSamples;

    NativeDriverPoint sub_last_point{};
    for (std::size_t sub_index = 0; sub_index < sub_bars.size(); ++sub_index) {
        const Bar& sub = *sub_bars[sub_index];
        callback_context_.sub_index = static_cast<int>(sub_index);
        callback_context_.is_terminal_sub_bar = sub_index + 1 == sub_bars.size();
        callback_context_.sub_bar_open_ms = sub.timestamp;
        // DistributionSamples and synthesized paths consume the generic
        // sampler from include/pineforge/magnifier.hpp as ordered point
        // decisions. This reproduces the read-only consumption ordering at
        // src/source/pine_scheduler.cpp:806-960 without source policy here.
        {
            // The sampler still serves the byte-identical legacy route through
            // its scoped internal order. Install this run's generic policy only
            // while materializing the native driver's point sequence.
            NativePathOrderScope path_scope(spec->path_order);
            if (!distribution_samples || direct_sub_bar_corners) {
                // A retained lower bar already supplies its four exact turning
                // points. Continuous eligibility traverses those segments directly;
                // likewise, a path containing several retained lower bars has no
                // missing intrabar detail for a synthetic sampler to recover.
                sample_price_path(sub, 4, MagnifierDistribution::ENDPOINTS, samples);
            } else if (volume_weighted) {
                sample_price_path_volume_weighted(
                    sub, sample_count, mean_volume, volume_weighted_min_samples,
                    volume_weighted_max_samples, distribution, samples);
            } else {
                sample_price_path(sub, sample_count, distribution, samples);
            }
        }
        if (samples.empty()) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Input});
            render(engine, "native intrabar path produced no samples");
            return;
        }
        ++driver_statistics_.sub_bars_processed;
        driver_statistics_.samples_per_sub_bar = static_cast<int>(samples.size());
        callback_context_.driver_statistics = driver_statistics_;
        double previous = samples.front();
        for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
            const double price = samples[sample_index];
            ++driver_statistics_.sample_ticks_processed;
            callback_context_.driver_statistics = driver_statistics_;
            NativeDriverPoint point;
            point.coordinate = base;
            point.coordinate.ordinal = take_ordinal(engine);
            point.coordinate.effective_time_ms = sub.timestamp;
            point.coordinate.source_price_time_ms = sub.timestamp;
            if (distribution_samples) {
                // A sampled value is a one-price decision bar, not an
                // interpolated leg of its containing OHLC bar. Giving every
                // one its open coordinate makes O=H=L=C=price: a reached
                // resting level fills at the level, while a gap through the
                // level keeps this sample's quote. Its driver ordinal still
                // orders it strictly after the preceding sample.
                point.coordinate.path_phase = NativePathPhase::Open;
                point.coordinate.provenance = NativePriceProvenance::ModeledOHLCOpen;
            } else {
                point.coordinate.path_phase = sample_index == 0 ? NativePathPhase::Open
                    : (sample_index + 1 == samples.size() ? NativePathPhase::Close
                       : (price == sub.high ? NativePathPhase::High
                          : (price == sub.low ? NativePathPhase::Low : NativePathPhase::None)));
                point.coordinate.provenance = sample_index == 0
                    ? NativePriceProvenance::ModeledOHLCOpen
                    : (sample_index + 1 == samples.size()
                        ? NativePriceProvenance::ModeledOHLCClose
                        : NativePriceProvenance::Confirmed);
            }
            point.raw_price = price;
            point.matching = distribution_samples || sample_index == 0;
            point.excursion = sample_index != 0;
            record_driver(point);
            // Same rule as the confirmed path: a discrete sample is its own
            // cursor and folds before its callbacks, a segment folds after it
            // has been consumed.
            if (distribution_samples || sample_index == 0) {
                note_partial_point(base.open_ms, price, 0.0);
            }
            if (sub_index == 0 && sample_index == 0) {
                invoke_bar_open_callback(engine, script_bar, point);
                if (failed()) return;
                has_margin_path_ = false;
                maintain_margin_liquidation(
                    engine, make_cursor(point, 0.0), point.coordinate.path_phase, price,
                    NativeMarginCheckKind::BarOpen);
                if (failed()) return;
                if (risk_limits() != nullptr) {
                    risk_evaluate(engine, point_frame_view(point), CallbackPhase::PreOpen);
                    if (failed()) return;
                }
            }
            if (distribution_samples || sample_index == 0) {
                match_discrete(engine, point);
                if (!failed()) apply_excursion(engine, price);
            } else {
                match_segment(engine, point, previous);
                if (!failed()) note_partial_point(base.open_ms, price, 0.0);
            }
            if (failed()) return;
            raise_floor(sub.timestamp);
            recalculate_at_point(engine, script_bar, point);
            if (failed()) return;
            sub_last_point = point;
            previous = price;
        }
        // CT8/HT3: one hook per completed lower-timeframe sub-bar, after its
        // whole path. A synthesized path has no lower bars of its own — its
        // single "sub-bar" IS the script bar — so it never fires here.
        if (lower) {
            note_partial_point(base.open_ms, previous, sub.volume);
            invoke_sub_bar_callback(engine, sub, sub_last_point);
            if (failed()) return;
        }
    }

    NativeCoordinate calculation = base;
    calculation.ordinal = take_ordinal(engine);
    calculation.effective_time_ms = calculation_time(base);
    calculation.source_price_time_ms = sub_bars.back()->timestamp;
    calculation.provenance = NativePriceProvenance::Calculation;
    calculation.path_phase = NativePathPhase::None;
    callback_context_.sub_index = static_cast<int>(sub_bars.size() - 1);
    callback_context_.is_terminal_sub_bar = true;
    callback_context_.sub_bar_open_ms = sub_bars.back()->timestamp;
    callback_context_.script_bar_open_ms = base.open_ms;
    raise_floor(calculation.effective_time_ms);
    engine.current_bar_ = script_bar;
    engine.bar_index_ = calculation.interval_index;
    engine.current_bar_.timestamp = calculation.effective_time_ms;
    clear_partial();
    invoke_callback(engine, script_bar, calculation);
    if (failed()) return;
    if (spec->close_execution == NativeCloseExecution::AfterCalculation) {
        NativeDriverPoint point;
        point.coordinate = calculation;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
        point.coordinate.path_phase = NativePathPhase::Close;
        point.raw_price = script_bar.close;
        point.matching = true;
        record_driver(point);
        match_point(engine, point);
        if (failed()) return;
    }
    record_script_report_point(engine, base.open_ms);
}

int64_t NativeExecutionConsumer::calculation_time(const NativeCoordinate& base) const noexcept {
    int64_t t = base.last_traded_close_ms;
    if (base.next_period_open_ms > t) t = base.next_period_open_ms;
    if (script_.latest_close_ms > t) t = script_.latest_close_ms;
    return t;
}

// One report point per script calculation, in the order the source scheduler
// uses (pine_strategy_host.cpp): fold the extremes at this calculation's
// close, then append the script bar's own point. The label is the script
// interval's open, not current_bar_.timestamp, which the intrabar walk
// overwrites — that keeps the curve identical with and without a path.
//
// The two halves are not scoped alike. The equity and position extremes are
// a property of the RUN — what it drew down, what it ran up, the most it
// ever held — and not of who records the curve, so the kernel folds them at
// every script calculation whatever the report policy says. Appending the
// point is the recording, and that stays the policy's: a HostRecorded host
// owns its series and gets no point from here. KernelRecordedAtHostMarks is
// the one policy that has handed the mark cadence away, and its fold travels
// with it (mark_script_report_point below) — folding here as well would
// count an instant the marking host never reports, which is a different run
// truth, not a truer one.
//
// The per-bar broker-state hash is a row of that same report, so the policy
// that hands the kernel the report hands it this array too: with the
// recording switch on (set_broker_state_hash_recording, off by default) one
// row follows each point, after the extremes the point just folded, which
// keeps broker_state_hash_len 1:1 with the curve and with
// script_bars_processed. Reporting only — the switch is not continuation
// state and the row is never read back. Under HostRecorded the report is the
// host's; under KernelRecordedAtHostMarks the marking host appends its own
// row, after the continuation snapshot only it can name.
//
// R5 lane F3: the kernel takes that snapshot for the policy it records, at
// every point and whatever the switch says (NativeStrategyHost::
// broker_state_hash_projection folds it), so the run's final broker_state_hash()
// is the last point's fingerprint -- the last row, when there are rows -- and
// not the continuation of the torn-down run. The row folds this same value.
void NativeExecutionConsumer::record_script_report_point(
        BacktestEngine& engine, int64_t script_open_ms) const {
    const auto* spec = spec_ptr();
    if (!spec) return;
    if (spec->report_policy != NativeReportPolicy::KernelRecordedAtHostMarks)
        engine.update_equity_extremes();
    if (spec->report_policy != NativeReportPolicy::KernelRecorded) return;
    engine.record_equity_point(script_open_ms);
    drop_continuation_view();
    engine.last_script_continuation_hash_ = continuation_hash();
    engine.last_script_continuation_valid_ = true;
    if (engine.broker_state_hash_recording_) {
        engine.broker_state_hashes_.push_back(engine.broker_state_hash());
    }
}

// The same fold and the same append, at an instant only the host can name
// (NativeReportPolicy::KernelRecordedAtHostMarks). A host whose report
// cadence is its own — a source adapter that publishes a script bar to its
// generated code, re-enters that script on a fill, or advances its source
// history over a bar the script never calculates — marks the point inside
// its own callback, where its broker-state hash and its report rows already
// read the curve. Recording is still the kernel's: the host names when, not
// what. Inert under every other policy, so a host that records its own
// report, or one that asked for the per-calculation cadence, is unaffected.
void NativeExecutionConsumer::mark_script_report_point(
        BacktestEngine& engine, int64_t script_bar_ts) const {
    const auto* spec = spec_ptr();
    if (!spec || spec->report_policy != NativeReportPolicy::KernelRecordedAtHostMarks) return;
    record_report_point(engine, script_bar_ts);
}

// The fold and the append together, at one instant: the shape a policy whose
// mark cadence is the host's needs, because there the two are never apart.
void NativeExecutionConsumer::record_report_point(
        BacktestEngine& engine, int64_t report_ts) const {
    engine.update_equity_extremes();
    engine.record_equity_point(report_ts);
}

// The generic mark-to-market producer both report shapes share. One row per
// open physical lot at `mark_price`, built by the same non-mutating builder a
// full close uses and appended to the engine's range-end row space; the summed
// NET row P&L comes back for a caller that re-derives a report figure from it.
//
// Nothing here is a policy: the caller decides when to mark, what to clear
// first and what (if anything) downstream of the rows it re-marks. The
// `preceding_exit_trail_peak` carry is the same one the kernel's own settling
// path folds (engine_execution.cpp), so a mark row and a close row fold the
// identical excursion facts; the member is a run-scoped NaN today, which makes
// the branch inert rather than absent.
double NativeExecutionConsumer::append_open_position_report_rows(
        BacktestEngine& engine, double mark_price, int64_t mark_time_ms,
        int interval_index) const {
    const bool was_long = engine.position_side_ == PositionSide::LONG;
    double marked = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = mark_time_ms;
        context.interval_index = interval_index;
        if (!std::isnan(engine.fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = engine.fold_exit_trail_peak_;
        Trade row = engine.build_close_trade_with_costs(
            lot, lot.qty, mark_price, was_long,
            engine.allocated_entry_commission(lot, lot.qty),
            engine.calc_commission(mark_price, lot.qty), context);
        row.open_at_end = true;
        marked += row.pnl;
        engine.range_end_trades_.push_back(std::move(row));
    }
    return marked;
}

// A position still open when the feed ends is reported as the rows a close at
// the last bar's close would record — the kernel's own use of the producer
// above, once, after the last point of the run. Reporting only: the live book,
// the realized sums, the equity curve and every hash are left exactly as the
// run left them, so enabling this cannot move a fill or a continuation.
// The mark is the raw close on the price grid (bar_fill_price) with no
// slippage: slippage models a market order's fill uncertainty, and this row is
// a mark, not an order. The exit is dated on the script bar's own label.
void NativeExecutionConsumer::record_open_position_report_rows(BacktestEngine& engine) const {
    const auto* spec = spec_ptr();
    if (!spec || spec->report_policy != NativeReportPolicy::KernelRecorded
        || !spec->report_open_position_at_end) return;
    engine.range_end_trades_.clear();
    if (engine.position_side_ == PositionSide::FLAT || engine.pyramid_entries_.empty()) return;
    if (!std::isfinite(engine.current_bar_.close)) return;
    (void)append_open_position_report_rows(
        engine, engine.bar_fill_price(engine.current_bar_.close),
        engine.equity_curve_.empty() ? engine.current_bar_.timestamp
                                     : engine.equity_curve_.back().time_ms,
        engine.bar_index_);
}

void NativeExecutionConsumer::deliver_aggregate_calculation(
        BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base) {
    calculating_bar_ = bar;
    calculating_bar_has_ = true;
    callback_context_ = NativeDecisionContext{};
    callback_context_.sub_index = 0;
    callback_context_.sub_count = 1;
    callback_context_.is_terminal_sub_bar = true;
    callback_context_.sub_bar_open_ms = base.open_ms;
    callback_context_.script_bar_open_ms = base.open_ms;
    present_session_day(callback_context_, base.open_ms);
    const int64_t close_time = calculation_time(base);
    NativeCoordinate calc = base;
    calc.ordinal = take_ordinal(engine);
    calc.effective_time_ms = close_time;
    calc.source_price_time_ms = script_.latest_close_ms;
    calc.provenance = NativePriceProvenance::Calculation;
    calc.path_phase = NativePathPhase::None;
    raise_floor(close_time);
    engine.current_bar_ = bar;
    engine.bar_index_ = calc.interval_index;
    engine.current_bar_.timestamp = close_time;
    clear_partial();
    invoke_callback(engine, bar, calc);
    if (failed()) return;
    const auto* spec = spec_ptr();
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        NativeDriverPoint point;
        point.coordinate = calc;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = close_time;
        point.coordinate.source_price_time_ms = script_.latest_close_ms;
        point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
        point.coordinate.path_phase = NativePathPhase::Close;
        point.raw_price = bar.close;
        point.matching = true;
        record_driver(point);
        match_point(engine, point);
        if (failed()) return;
    }
    record_script_report_point(engine, base.open_ms);
}

// The lazy seal. A script interval none of its own inputs sealed -- a session
// close clips it short of its nominal end (the last "60" of a 09:30-16:00
// session, every "D" over an intraday input), or the feed has a hole over its
// last slot -- is sealed by the first input of a LATER interval, as
// LazyComplete, before that input contributes anything: the calculation runs
// with the bucket exactly as its own contributing inputs left it. False only
// when that calculation failed.
bool NativeExecutionConsumer::seal_stale_script(BacktestEngine& engine,
                                                std::int64_t script_key) {
    if (!script_.has_data || script_.sealed || script_.key == script_key) return true;
    seal_script(engine, NativeCompletionKind::LazyComplete);
    if (failed()) return false;
    script_ = ScriptBucket{};
    return true;
}

void NativeExecutionConsumer::seal_script(BacktestEngine& engine, NativeCompletionKind kind) {
    if (!script_.has_data || script_.sealed) return;
    NativeCoordinate base;
    base.interval_index = script_.first_index;
    base.open_ms = script_.interval.open_ms;
    base.eligible_open_ms = script_.interval.eligible_open_ms;
    base.last_traded_close_ms = script_.interval.last_traded_close_ms;
    base.next_period_open_ms = script_.interval.next_period_open_ms;
    base.next_input_open_ms = script_.interval.next_input_open_ms;
    base.completion = kind;
    if (script_.modeled_ohlc) {
        if (const auto* spec = spec_ptr(); spec && !spec->intrabar.is_none()) {
            deliver_intrabar_script(engine, script_.agg, base);
        } else {
            deliver_confirmed_script(engine, script_.agg, base);
        }
    } else {
        deliver_aggregate_calculation(engine, script_.agg, base);
    }
    script_.sealed = true;
    script_.has_data = false;
}

bool NativeExecutionConsumer::contribute_input(
        BacktestEngine& engine, const Bar& bar,
        const native_calendar::NativeInterval& interval,
        int index, InputContribution kind) {
    auto script_interval = script_interval_at(interval.open_ms);
    if (!script_interval) {
        render(engine, "native script interval lookup failed");
        return false;
    }
    const int64_t script_key = script_interval->open_ms;
    if (!seal_stale_script(engine, script_key)) return false;
    const int64_t source_close = (kind == InputContribution::ConfirmedBar)
        ? std::max(interval.last_traded_close_ms, interval.next_period_open_ms)
        : (last_print_time_ms_ != 0 ? last_print_time_ms_ : interval.last_traded_close_ms);
    const int64_t source_open = (kind == InputContribution::ConfirmedBar)
        ? bar.timestamp
        : (kind == InputContribution::ObservedTickSlot
            ? (last_print_time_ms_ != 0 ? last_print_time_ms_ : interval.eligible_open_ms)
            : interval.eligible_open_ms);
    if (!script_.has_data) {
        script_.key = script_key;
        script_.interval = *script_interval;
        script_.agg = bar;
        script_.has_data = true;
        script_.first_open_ms = interval.open_ms;
        script_.first_source_time_ms = source_open;
        script_.latest_close_ms = source_close;
        script_.first_index = index;
        script_.last_index = index;
        script_.sealed = false;
        script_.modeled_ohlc = (kind == InputContribution::ConfirmedBar);
    } else {
        script_.agg.high = std::max(script_.agg.high, bar.high);
        script_.agg.low = std::min(script_.agg.low, bar.low);
        script_.agg.close = bar.close;
        script_.agg.volume += bar.volume;
        script_.latest_close_ms = std::max(script_.latest_close_ms, source_close);
        script_.last_index = index;
        if (kind != InputContribution::ConfirmedBar) script_.modeled_ohlc = false;
    }
    current_input_open_ = interval.open_ms;
    observed_input_cursor_ = interval.open_ms;
    last_accepted_input_ = interval;
    const auto* spec = spec_ptr();
    // A modeled intrabar path supplies its own decision points during seal.
    // Raising to the input/script completion here would fence requests born
    // at an earlier sub-bar out of its remaining points. No-path and stream
    // aggregation retain the historical script-bar floor exactly.
    const bool intrabar_points_drive_floor = kind == InputContribution::ConfirmedBar
        && spec && !spec->intrabar.is_none();
    if (!intrabar_points_drive_floor) {
        raise_floor(native_canonical_input_completion(interval));
    }
    const bool exhausted =
        interval.next_period_open_ms >= script_.interval.next_period_open_ms;
    if (exhausted) {
        seal_script(engine, NativeCompletionKind::Confirmed);
        if (failed()) return false;
        script_ = ScriptBucket{};
    }
    engine.current_bar_ = bar;
    engine.bar_index_ = index;
    next_interval_index_ = index + 1;
    if (!failed() && kind != InputContribution::QuietCarried)
        ++engine.diag_input_bars_processed_;
    return !failed();
}

// ---- declared higher-timeframe series ---------------------------------------
//
// A native subscription reuses the kernel's own higher-timeframe machinery:
// one BacktestEngine::SecurityEvalState per declared series, its aggregator
// built by register_security_eval, its authoritative bars installed through
// the public set_native_security_feed store and routed by
// prepare_native_security_feeds, its buckets stepped by
// feed_security_eval_state. None of that is Pine-shaped: the kernel evaluator
// has no publication modes at all, so the projected and live passes below are
// the same step. The modes the spec declares (lookahead, gaps) are this
// consumer's own delivery rules; TradingView's request.security semantics --
// merge latching, publication gates, Heikin-Ashi, the range-start cut,
// lower-timeframe emulation, the auxiliary chart slice -- are the source
// host's, kept beside the same generic state, and no native run reaches them
// (the spec also refuses a series finer than the input).
//
// Entanglement the host inherits with authoritative bars: the feed store is
// TradingView-calibrated. A "W"/"M" series with no feed of its own is built
// from installed DAILY bars, and an installed feed's stamps become the
// period partition, so a session with no stamp of its own folds into the next
// trade date's bar (src/engine_aux_security.cpp, docs/pages/native-engine.md).

void NativeExecutionConsumer::clear_timeframe_subscriptions(BacktestEngine& engine) {
    subscription_warmup_inputs_ = -1;
    auxiliary_tf_.reset();
    auxiliary_appended_.clear();
    auxiliary_appended_digest_ = 0;
    // A feed THIS consumer installed at an earlier begin is the kernel's to
    // remove. One installed through the engine's own public setter -- the C
    // ABI's strategy_set_native_security_feed writes the same store -- is not:
    // it is pre-run host ingress that survives every later begin unless that
    // begin declares authoritative bars of its own for the same period, which
    // simply replaces it (R3 row 3).
    for (const std::string& tf : subscription_feed_tfs_) {
        for (auto it = engine.native_security_feeds_.begin();
             it != engine.native_security_feeds_.end(); ++it) {
            if (it->tf != tf) continue;
            engine.native_security_feeds_.erase(it);
            break;
        }
    }
    subscription_feed_tfs_.clear();
    if (subscriptions_.empty()) return;
    // Erase exactly the evaluator states this consumer registered, and only
    // while they are still the vector's tail exactly as it registered them.
    // This runs at the next begin BEFORE the host's on_native_run_begin, so a
    // state that callback registers -- even one identical to the kernel's
    // last registration, as a host moving a series from the kernel's drive
    // to its own does -- is never mistaken for the kernel's tail; a host that
    // touched the vector between runs owns every state in it, and the kernel
    // takes none of them away.
    const std::size_t base = subscription_states_base_;
    if (engine.security_eval_states_.size() == base + subscriptions_.size()) {
        bool registered_by_this_consumer = true;
        for (std::size_t i = 0; i < subscriptions_.size(); ++i) {
            const auto& state = engine.security_eval_states_[base + i];
            if (state.sec_id != static_cast<int>(base + i)
                || state.tf != subscriptions_[i].tf_literal) {
                registered_by_this_consumer = false;
                break;
            }
        }
        if (registered_by_this_consumer) {
            engine.security_eval_states_.erase(
                engine.security_eval_states_.begin()
                    + static_cast<std::ptrdiff_t>(base),
                engine.security_eval_states_.end());
        }
    }
    subscription_states_base_ = 0;
    subscriptions_.clear();
    input_next_ms_.clear();
    engine.security_input_tf_.clear();
    engine.security_next_input_ms_ = 0;
}

bool NativeExecutionConsumer::begin_timeframe_subscriptions(
        BacktestEngine& engine, const NativeRunSpec& spec,
        const Bar* input_bars, int n_input, bool is_stream) {
    // This wiring runs after the run is Running, so the engine's public feed
    // setter would refuse it as an in-run source mutation. It is the kernel's
    // own registration, not a host ingress, and no host callback runs inside
    // this scope.
    struct WiringScope {
        bool& flag;
        ~WiringScope() { flag = false; }
    } wiring{wiring_subscriptions_};
    wiring_subscriptions_ = true;
    clear_timeframe_subscriptions(engine);
    if (spec.auxiliary_feed) {
        // The feed is the run's from here on, series or none: a stream may
        // append to it, and those bars are part of the continuation identity.
        try {
            auxiliary_tf_ = native_calendar::parse_timeframe(spec.auxiliary_feed->tf);
        } catch (...) {
            auxiliary_tf_.reset();
        }
        if (!auxiliary_tf_) {
            fail(engine, NativeFailure{NativeFailureCode::Calendar,
                                       NativeFailureOperation::Begin});
            render(engine, "native auxiliary feed parse failed at begin");
            return false;
        }
        auxiliary_appended_digest_ = 1469598103934665603ULL;
    }
    if (spec.subscriptions.empty()) return true;
    auto* host = native_host(engine);
    if (host == nullptr) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Begin});
        render(engine, "native timeframe subscriptions require a native strategy host");
        return false;
    }
    if (input_bars == nullptr || n_input < 0) n_input = 0;
    // A stream resolves its series over the warmup input exactly as a batch of
    // those same bars does -- including the last one, whose successor a batch
    // does not know either -- and then continues the same aggregators live.
    // This is where that phase change happens: a live input has no successor
    // and no future, so it extends the current bucket and delivers it at
    // completion under both publication modes (Pine's lookahead is a
    // historical-resolution mode; the realtime bar has nothing to look into).
    subscription_warmup_inputs_ = is_stream ? n_input : -1;
    try {
        input_next_ms_.assign(static_cast<std::size_t>(n_input), 0);
        for (int i = 0; i + 1 < n_input; ++i) {
            input_next_ms_[static_cast<std::size_t>(i)] = input_bars[i + 1].timestamp;
        }
        engine.security_input_tf_ = spec.input_tf;
        engine.security_next_input_ms_ = 0;
        // Evaluator states the host registered for itself keep their sec_ids:
        // this consumer's own states are appended after them, and the base is
        // zero for the whole population that registers none.
        subscription_states_base_ = engine.security_eval_states_.size();
        subscriptions_.reserve(spec.subscriptions.size());
        for (std::size_t i = 0; i < spec.subscriptions.size(); ++i) {
            const auto& declared = spec.subscriptions[i];
            const int sec_id = static_cast<int>(subscription_states_base_ + i);
            auto parsed = native_calendar::parse_timeframe(declared.tf);
            if (!parsed) {
                fail(engine, NativeFailure{NativeFailureCode::Calendar,
                                           NativeFailureOperation::Begin});
                render(engine, "native timeframe subscription parse failed at begin");
                return false;
            }
            TimeframeSubscription subscription;
            subscription.index = i;
            subscription.sec_id = sec_id;
            subscription.tf = std::move(*parsed);
            subscription.tf_literal = declared.tf;
            subscription.lookahead = declared.lookahead;
            subscription.gaps = declared.gaps;
            subscription.auxiliary = declared.source == NativeSeriesSource::AuxiliaryFeed;
            if (subscription.auxiliary && !spec.auxiliary_feed) {
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Begin});
                render(engine, "native timeframe subscription names an undeclared auxiliary feed");
                return false;
            }
            const bool from_feed = subscription.auxiliary;
            subscriptions_.push_back(std::move(subscription));
            // A series built from the auxiliary feed aggregates the FEED's
            // bars, so its evaluator is registered against the feed's
            // timeframe; every other series keeps the input's.
            if (from_feed) engine.security_input_tf_ = spec.auxiliary_feed->tf;
            engine.register_security_eval(sec_id, declared.tf, spec.input_tf);
            if (from_feed) engine.security_input_tf_ = spec.input_tf;
            if (declared.authoritative_bars.empty()) continue;
            if (!engine.set_native_security_feed(
                    declared.tf, declared.authoritative_bars.data(),
                    static_cast<int>(declared.authoritative_bars.size()))) {
                const std::string reason = engine.last_error_.empty()
                    ? std::string("native timeframe subscription feed was refused")
                    : engine.last_error_;
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Begin});
                render(engine, reason.c_str());
                return false;
            }
            // Remembered so the next begin removes this consumer's own feeds
            // and nothing else. Same-period instances install one feed; the
            // spec already refused two conflicting ones.
            if (std::find(subscription_feed_tfs_.begin(), subscription_feed_tfs_.end(),
                          declared.tf) == subscription_feed_tfs_.end()) {
                subscription_feed_tfs_.push_back(declared.tf);
            }
        }
        if (engine.security_eval_states_.size()
                != subscription_states_base_ + subscriptions_.size()) {
            fail(engine, NativeFailure{NativeFailureCode::Contract,
                                       NativeFailureOperation::Begin});
            render(engine, "native timeframe subscription registration is inconsistent");
            return false;
        }
        engine.prepare_native_security_feeds(input_bars, n_input);
        for (auto& subscription : subscriptions_) {
            if (!subscription.lookahead) continue;
            if (!project_timeframe_subscription(engine, subscription, input_bars, n_input)) {
                return false;
            }
        }
    } catch (const std::bad_alloc&) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Begin});
        render(engine, "native timeframe subscription allocation failed");
        return false;
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected,
                                   NativeFailureOperation::Begin});
        render(engine, e.what());
        return false;
    } catch (...) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected,
                                   NativeFailureOperation::Begin});
        render(engine, "native timeframe subscription preparation failed");
        return false;
    }
    return true;
}

// The begin-time declaration hook. A host whose series are known only to its
// own run-begin registration names them here, inside on_native_run_begin and
// nowhere else, and the list REPLACES the staged spec's own before the kernel
// registers -- so it is also what the run's continuation identity folds, and
// the staged spec keeps naming exactly what ran. A list this run's input
// timeframe would refuse is refused here, leaving the staged list untouched:
// the same validation configure_native applied, against the running spec's
// own input timeframe. The two refusals are separate answers -- a call made
// anywhere but inside on_native_run_begin judged no list at all and says so
// with WrongPhase, where a judged list carries the validation's own first
// error field.
NativeSetupResult NativeExecutionConsumer::declare_timeframe_subscriptions(
        std::vector<NativeTimeframeSubscription> declared) {
    NativeSetupResult result;
    auto* running = std::get_if<NativeRunning>(&state_);
    if (!in_run_begin_ || failed() || running == nullptr) {
        result.validation = {NativeRunSpecError::WrongPhase, NativeRunSpecField::None};
        return result;
    }
    result.validation = validate_native_timeframe_subscriptions(
        declared, running->spec.input_tf, running->spec.timeframe_undetected,
        running->spec.auxiliary_feed);
    if (!result.validation) return result;
    running->spec.subscriptions = std::move(declared);
    spec_bar_digests_.reset();
    result.status = NativeSetupStatus::Applied;
    return result;
}

// The same begin-time hook for the auxiliary feed. The feed REPLACES the
// staged spec's own (nullopt withdraws it) before the kernel registers, so it
// is what the run's continuation identity folds. It is judged with the series
// the staged spec names at that moment: a feed this run's input timeframe
// would refuse, or one that leaves a staged AuxiliaryFeed series without its
// bars or finer than them, is refused and changes nothing. A host that names
// both at begin therefore declares the feed first and the series second.
NativeSetupResult NativeExecutionConsumer::declare_auxiliary_feed(
        std::optional<NativeAuxiliaryFeed> declared) {
    NativeSetupResult result;
    auto* running = std::get_if<NativeRunning>(&state_);
    if (!in_run_begin_ || failed() || running == nullptr) {
        result.validation = {NativeRunSpecError::WrongPhase, NativeRunSpecField::None};
        return result;
    }
    result.validation = validate_native_timeframe_subscriptions(
        running->spec.subscriptions, running->spec.input_tf,
        running->spec.timeframe_undetected, declared);
    if (!result.validation) return result;
    running->spec.auxiliary_feed = std::move(declared);
    spec_bar_digests_.reset();
    result.status = NativeSetupStatus::Applied;
    return result;
}

// A realtime stream learns its finer bars as they complete. They join the
// declared feed behind every bar it already holds and are folded by the very
// routing rule a batch applies: on the next accepted input whose period they
// opened before. A bar that opened before the END of the last accepted
// input's period belonged to that input's slice, and folding it now would
// build a series no batch of the same bars could, so it is refused by name --
// as is every other malformed append -- without failing the host.
NativeAuxiliaryAppendResult NativeExecutionConsumer::append_auxiliary_bars(
        BacktestEngine& engine, const Bar* bars, std::size_t n) {
    // Each `refused` below carries the SAME name the presentation text has
    // always rendered; the text stays exactly where it was.
    const auto refused = [](NativeAuxiliaryAppendError error,
                            std::size_t index = 0) noexcept {
        NativeAuxiliaryAppendResult result;
        result.error = error;
        result.index = index;
        return result;
    };
    // The two legality refusals admit_public_stream_input folds into one bool:
    // a host that was ALREADY failed on entry, and a reentrant call, which is
    // the Contract failure this very call latches.
    const bool entered_failed = failed();
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) {
        return refused(entered_failed ? NativeAuxiliaryAppendError::HostFailed
                                      : NativeAuxiliaryAppendError::Reentrant);
    }
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running || running->phase != NativeRunPhase::Realtime) {
        present_refusal(engine, "native append_auxiliary_bars requires realtime");
        return refused(NativeAuxiliaryAppendError::NotRealtime);
    }
    if (!auxiliary_tf_ || !running->spec.auxiliary_feed) {
        present_refusal(engine, "native append_auxiliary_bars requires a declared auxiliary feed");
        return refused(NativeAuxiliaryAppendError::NoAuxiliaryFeed);
    }
    NativeAuxiliaryAppendResult applied;
    applied.status = NativeSetupStatus::Applied;
    if (n == 0) return applied;
    if (bars == nullptr) {
        present_refusal(engine, "native auxiliary bar array is invalid");
        return refused(NativeAuxiliaryAppendError::InvalidBarArray);
    }
    const std::size_t held = auxiliary_bar_count();
    for (std::size_t i = 0; i < n; ++i) {
        if (!native_bar_structurally_valid(bars[i])) {
            present_refusal(engine, "native auxiliary bar has invalid OHLCV");
            return refused(NativeAuxiliaryAppendError::InvalidBar, i);
        }
        const bool ordered = i > 0 ? bars[i].timestamp > bars[i - 1].timestamp
                                   : held == 0
                                       || bars[i].timestamp > auxiliary_bar(held - 1).timestamp;
        if (!ordered) {
            present_refusal(engine, "native auxiliary bars must be strictly increasing");
            return refused(NativeAuxiliaryAppendError::UnorderedBars, i);
        }
    }
    if (last_accepted_input_
        && bars[0].timestamp < last_accepted_input_->next_period_open_ms) {
        present_refusal(engine,
            "native auxiliary bar opened inside an input period that was already accepted");
        return refused(NativeAuxiliaryAppendError::InputPeriodAlreadyAccepted);
    }
    try {
        auxiliary_appended_.insert(auxiliary_appended_.end(), bars, bars + n);
    } catch (const std::bad_alloc&) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Input});
        render(engine, "native auxiliary feed allocation failed");
        return refused(NativeAuxiliaryAppendError::AllocationFailure);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        const auto fold = [this](const void* data, std::size_t count) noexcept {
            const auto* values = static_cast<const unsigned char*>(data);
            for (std::size_t at = 0; at < count; ++at) {
                auxiliary_appended_digest_ ^= values[at];
                auxiliary_appended_digest_ *= 1099511628211ULL;
            }
        };
        fold(&bar.open, sizeof bar.open); fold(&bar.high, sizeof bar.high);
        fold(&bar.low, sizeof bar.low); fold(&bar.close, sizeof bar.close);
        fold(&bar.volume, sizeof bar.volume); fold(&bar.timestamp, sizeof bar.timestamp);
    }
    return applied;
}

std::size_t NativeExecutionConsumer::auxiliary_bar_count() const noexcept {
    const auto* spec = spec_ptr();
    const std::size_t declared =
        spec && spec->auxiliary_feed ? spec->auxiliary_feed->bars.size() : 0;
    return declared + auxiliary_appended_.size();
}

const Bar& NativeExecutionConsumer::auxiliary_bar(std::size_t at) const noexcept {
    const auto& declared = spec_ptr()->auxiliary_feed->bars;
    return at < declared.size() ? declared[at] : auxiliary_appended_[at - declared.size()];
}

// One input's slice of the auxiliary feed for one series: every feed bar that
// opened before the input's period ended, in feed order, through the same
// evaluator step a series built from the input takes. The feed bar's own
// successor plays the part the next input's stamp plays there (a calendar
// bucket may complete on the period's actual last bar), and a stream knows it
// only for bars it has already been handed. `completed(bucket, first_index,
// first_ms, kind)` receives each completed bucket with the input index and
// the timestamp of its first contributing feed bar; the bucket bookkeeping is
// the input pump's.
template <typename Completed>
bool NativeExecutionConsumer::feed_auxiliary_slice(
        BacktestEngine& engine, TimeframeSubscription& subscription, int index,
        std::int64_t input_period_end_ms, Completed&& completed) {
    auto& state =
        engine.security_eval_states_[static_cast<std::size_t>(subscription.sec_id)];
    const std::size_t total = auxiliary_bar_count();
    while (subscription.auxiliary_cursor < total) {
        const Bar feed_bar = auxiliary_bar(subscription.auxiliary_cursor);
        if (feed_bar.timestamp >= input_period_end_ms) break;
        const std::size_t at = subscription.auxiliary_cursor++;
        if (subscription.bucket_first_index < 0) {
            subscription.bucket_first_index = index;
            subscription.bucket_first_ms = feed_bar.timestamp;
        }
        engine.security_next_input_ms_ =
            at + 1 < total ? auxiliary_bar(at + 1).timestamp : 0;
        const std::int64_t before = state.eval_complete_count;
        engine.feed_security_eval_state(state, feed_bar);
        engine.security_next_input_ms_ = 0;
        if (state.eval_complete_count <= before) continue;
        const bool boundary = state.aggregator.is_active()
            && state.aggregator.current().timestamp != state.current_bar.timestamp;
        const Bar bucket = state.current_bar;
        const int first_index = subscription.bucket_first_index;
        const std::int64_t first_ms = subscription.bucket_first_ms;
        if (boundary) {
            subscription.bucket_first_index = index;
            subscription.bucket_first_ms = feed_bar.timestamp;
        } else {
            subscription.bucket_first_index = -1;
            subscription.bucket_first_ms = 0;
        }
        if (!completed(bucket, first_index, first_ms,
                       boundary ? NativeCompletionKind::LazyComplete
                                : NativeCompletionKind::Confirmed)) {
            return false;
        }
    }
    return !failed();
}

// barmerge.lookahead_on: resolve the whole series over the historical input
// now, so each completed bucket's FINAL values can be delivered at the input
// bar that opened it. This pass IS the subscription's only feed for those
// inputs; the pump is skipped over them, so the substitution/miss diagnostics
// count each bucket exactly once. A stream's historical input is its warmup,
// and the aggregator is left holding whatever bucket the warmup left open --
// the live phase continues that very bucket through the aggregating pump.
bool NativeExecutionConsumer::project_timeframe_subscription(
        BacktestEngine& engine, TimeframeSubscription& subscription,
        const Bar* input_bars, int n_input) {
    if (subscription.auxiliary) {
        // The same projection over the auxiliary feed: each historical input
        // folds its own slice, and a bucket is keyed to the input whose slice
        // held its first contributing feed bar.
        for (int i = 0; i < n_input; ++i) {
            const auto interval = input_interval_at(input_bars[i].timestamp);
            if (!interval) {
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Begin});
                render(engine, "native auxiliary feed projection found an unaligned input");
                return false;
            }
            const auto record = [&subscription](const Bar& bucket, int first_index,
                                                std::int64_t first_ms,
                                                NativeCompletionKind completion) {
                subscription.projected_bars.push_back(bucket);
                subscription.projected_first_index.push_back(first_index);
                subscription.projected_first_ms.push_back(first_ms);
                subscription.projected_completion.push_back(completion);
                return true;
            };
            if (!feed_auxiliary_slice(engine, subscription, i,
                                      interval->next_period_open_ms, record)) {
                return false;
            }
        }
        return !failed();
    }
    auto& state = engine.security_eval_states_[static_cast<std::size_t>(subscription.sec_id)];
    int first_index = -1;
    std::int64_t first_ms = 0;
    for (int i = 0; i < n_input; ++i) {
        if (first_index < 0) {
            first_index = i;
            first_ms = input_bars[i].timestamp;
        }
        engine.security_next_input_ms_ =
            input_next_ms_[static_cast<std::size_t>(i)];
        const std::int64_t before = state.eval_complete_count;
        engine.feed_security_eval_state(state, input_bars[i]);
        if (state.eval_complete_count <= before) continue;
        // A boundary emission hands back the PREVIOUS bucket and re-seats the
        // aggregator on the one this input opened; an eager completion (count,
        // real end, session close, the period's last input) leaves the
        // completed bucket current.
        const bool boundary = state.aggregator.is_active()
            && state.aggregator.current().timestamp != state.current_bar.timestamp;
        subscription.projected_bars.push_back(state.current_bar);
        subscription.projected_first_index.push_back(first_index);
        subscription.projected_first_ms.push_back(first_ms);
        subscription.projected_completion.push_back(
            boundary ? NativeCompletionKind::LazyComplete : NativeCompletionKind::Confirmed);
        if (boundary) {
            first_index = i;
            first_ms = input_bars[i].timestamp;
        } else {
            first_index = -1;
            first_ms = 0;
        }
    }
    engine.security_next_input_ms_ = 0;
    // The bucket the projected input left open, published as the aggregating
    // pump's own cursor. Inert for a batch run, where a lookahead_on series
    // never reaches that pump. For a stream it keeps the documented anchor:
    // the delivered context's interval is the span of the bucket's FIRST
    // contributing input bar (native_host.hpp), which for a bucket the warmup
    // opened is a warmup bar, not the first live one that continues it.
    subscription.bucket_first_index = first_index;
    subscription.bucket_first_ms = first_ms;
    return !failed();
}

bool NativeExecutionConsumer::pump_timeframe_subscriptions(
        BacktestEngine& engine, const Bar& bar, int index,
        std::int64_t input_period_end_ms) {
    // barmerge.gaps_on for one series: the input delivered nothing of its
    // own, so the series has no value on it -- on the pull side
    // (native_series_bar answers nullopt) and on the push side
    // (clear_security, the counterpart of the evaluate_security the step
    // dispatches: a bare host's is the base no-op, a source host's reaches
    // the generated clear_security() of the same sec_id, exactly where the
    // Pine source host's own step clears a gaps_on site). gaps_off leaves
    // the delivered bucket standing, which is every established run.
    const auto clear_if_gapped = [&engine](TimeframeSubscription& subscription) {
        if (subscription.gaps) subscription.latest.reset();
        if (subscription.gaps) engine.clear_security(subscription.sec_id);
    };
    for (auto& subscription : subscriptions_) {
        bool delivered = false;
        if (subscription.lookahead) {
            while (subscription.projected_cursor < subscription.projected_bars.size()
                   && subscription.projected_first_index[subscription.projected_cursor]
                          == index) {
                const std::size_t at = subscription.projected_cursor++;
                if (!deliver_timeframe_bar(engine, subscription,
                                           subscription.projected_bars[at],
                                           subscription.projected_first_ms[at],
                                           bar.timestamp,
                                           subscription.projected_completion[at])) {
                    return false;
                }
                delivered = true;
            }
            // Historical inputs are served entirely by that projection. A
            // stream's live inputs are not in it and have no future to be
            // resolved over, so they fall through to the aggregating pump
            // below: the same buckets, delivered when they complete.
            if (subscription_warmup_inputs_ < 0 || index < subscription_warmup_inputs_) {
                if (!delivered) clear_if_gapped(subscription);
                continue;
            }
        }
        if (subscription.auxiliary) {
            // Built from the auxiliary feed: this input's slice of it, bar by
            // bar, each completed bucket delivered on this input. Several may
            // ride on one input (a series finer than the input always does).
            const auto deliver = [&](const Bar& bucket, int, std::int64_t first_ms,
                                     NativeCompletionKind completion) {
                delivered = true;
                return deliver_timeframe_bar(engine, subscription, bucket, first_ms,
                                             bar.timestamp, completion);
            };
            if (!feed_auxiliary_slice(engine, subscription, index, input_period_end_ms,
                                      deliver)) {
                return false;
            }
            if (!delivered) clear_if_gapped(subscription);
            continue;
        }
        auto& state =
            engine.security_eval_states_[static_cast<std::size_t>(subscription.sec_id)];
        if (subscription.bucket_first_index < 0) {
            subscription.bucket_first_index = index;
            subscription.bucket_first_ms = bar.timestamp;
        }
        engine.security_next_input_ms_ =
            (index >= 0 && static_cast<std::size_t>(index) < input_next_ms_.size())
                ? input_next_ms_[static_cast<std::size_t>(index)]
                : 0;
        const std::int64_t before = state.eval_complete_count;
        engine.feed_security_eval_state(state, bar);
        engine.security_next_input_ms_ = 0;
        if (state.eval_complete_count <= before) {
            if (!delivered) clear_if_gapped(subscription);
            continue;
        }
        const bool boundary = state.aggregator.is_active()
            && state.aggregator.current().timestamp != state.current_bar.timestamp;
        const Bar completed = state.current_bar;
        const std::int64_t first_ms = subscription.bucket_first_ms;
        if (boundary) {
            subscription.bucket_first_index = index;
            subscription.bucket_first_ms = bar.timestamp;
        } else {
            subscription.bucket_first_index = -1;
            subscription.bucket_first_ms = 0;
        }
        if (!deliver_timeframe_bar(engine, subscription, completed, first_ms, bar.timestamp,
                                   boundary ? NativeCompletionKind::LazyComplete
                                            : NativeCompletionKind::Confirmed)) {
            return false;
        }
    }
    return true;
}

// A declared series is a function of the accepted CONFIRMED input, which is
// the whole input a batch of the same bars has. The tick driver's other two
// contributions have no batch counterpart to reproduce: an observed-tick slot
// is finalized after its own matching pass, and a quiet-carried slot is a
// synthesized flat bar a batch feed would simply not contain. Rather than fold
// either into a bucket and silently answer with a series no batch could
// produce, a stream that declares a series takes confirmed bars only.
bool NativeExecutionConsumer::refuse_subscription_tick_input(BacktestEngine& engine) {
    if (subscriptions_.empty()) return false;
    present_refusal(engine,
        "native timeframe subscriptions require confirmed-bar stream input");
    return true;
}

// FP6. The account converts at the rate of the engine's presented clock.
// Confirmed input moves that clock point by point, exactly as a batch of the
// same bars does, so a confirmed-bar stream under a declared curve IS that
// batch. Tick-driven input does not: an observation hook runs before its
// print has moved the clock, and a partially finalized slot calculates at its
// own OPEN, behind prints it has already matched -- measured on this tree, a
// step at T+30s still converted at the pre-step rate at the T+40s and T+50s
// prints and at the slot's calculation. Rather than convert one run on two
// clocks, a stream that declares a curve takes confirmed bars only.
bool NativeExecutionConsumer::refuse_fx_curve_tick_input(BacktestEngine& engine) {
    if (!staged_fx_curve_) return false;
    present_refusal(engine,
        "a declared native FX curve requires confirmed-bar stream input");
    return true;
}

bool NativeExecutionConsumer::deliver_timeframe_bar(
        BacktestEngine& engine, TimeframeSubscription& subscription, const Bar& bucket,
        std::int64_t first_contributing_ms, std::int64_t delivered_at_ms,
        NativeCompletionKind completion) {
    // The pull accessor answers with this bucket for the whole callback.
    subscription.latest = bucket;
    auto* host = native_host(engine);
    if (host == nullptr) return true;
    NativeTimeframeBarContext context;
    context.subscription = subscription.index;
    // The bucket's span is read over the bars it was built from: the input's
    // timeframe, or the auxiliary feed's for a series built from that feed.
    const native_calendar::Timeframe& built_from =
        subscription.auxiliary && auxiliary_tf_ ? *auxiliary_tf_ : input_tf_;
    if (auto interval = native_calendar::interval_containing(
            calendar_, subscription.tf, built_from, first_contributing_ms, calendar_memo_)) {
        context.interval = *interval;
    }
    context.completion = completion;
    context.delivered_at_ms = delivered_at_ms;
    in_callback_ = true;
    try {
        host->on_native_timeframe_bar(bucket, context);
    } catch (const std::exception& e) {
        in_callback_ = false;
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input});
            render(engine, e.what());
        }
        return false;
    } catch (...) {
        in_callback_ = false;
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input});
            render(engine, "native timeframe callback exception");
        }
        return false;
    }
    in_callback_ = false;
    return !failed();
}

std::optional<Bar> NativeExecutionConsumer::series_bar(std::size_t subscription) const {
    if (subscription >= subscriptions_.size()) return std::nullopt;
    return subscriptions_[subscription].latest;
}

bool NativeExecutionConsumer::consume_confirmed_input(BacktestEngine& engine, const Bar& bar,
                                                      int index, bool last) {
    (void)last;
    processing_input_ = true;
    const auto* running = std::get_if<NativeRunning>(&state_);
    const bool tolerant_realtime = legacy_tolerant_slot_labels() && running
        && running->phase == NativeRunPhase::Realtime;
    if (tolerant_realtime && last_accepted_input_) {
        // FeedTolerant preserves arbitrary provider labels, but the
        // realtime confirmed-bar API still advances on the caller's raw label
        // grid (ab9714be pine_stream.cpp:167-205).  Validate that boundary
        // before any driver, digest, aggregation, or callback mutation.
        std::int64_t unit_ms = 0;
        switch (input_tf_.unit()) {
        case native_calendar::TimeframeUnit::Second: unit_ms = 1000; break;
        case native_calendar::TimeframeUnit::Minute: unit_ms = 60'000; break;
        case native_calendar::TimeframeUnit::Day: unit_ms = 86'400'000; break;
        case native_calendar::TimeframeUnit::Week: unit_ms = 604'800'000; break;
        case native_calendar::TimeframeUnit::Month: break;
        }
        const auto count = static_cast<std::int64_t>(input_tf_.count());
        if (!(unit_ms > 0) || !(count > 0)
            || count > std::numeric_limits<std::int64_t>::max() / unit_ms) {
            processing_input_ = false;
            present_refusal(engine, "native confirmed bar timeframe is not a fixed grid");
            return false;
        }
        const std::int64_t step = count * unit_ms;
        const std::int64_t previous = last_accepted_input_->open_ms;
        if (previous > std::numeric_limits<std::int64_t>::max() - step
            || bar.timestamp > std::numeric_limits<std::int64_t>::max() - step) {
            processing_input_ = false;
            present_refusal(engine, "native confirmed bar timestamp overflows the input grid");
            return false;
        }
        const std::int64_t expected = previous + step;
        if (bar.timestamp < expected || (bar.timestamp - expected) % step != 0) {
            processing_input_ = false;
            present_refusal(engine,
                "native confirmed bar timestamp is out of order or off the input grid");
            return false;
        }
        for (std::int64_t missing = expected; missing < bar.timestamp;) {
            if (native_calendar::in_session(calendar_, missing, calendar_memo_)) {
                processing_input_ = false;
                present_refusal(engine, "native stream has an in-session gap");
                return false;
            }
            if (missing > std::numeric_limits<std::int64_t>::max() - step) {
                processing_input_ = false;
                present_refusal(engine, "native confirmed bar timestamp overflows the input grid");
                return false;
            }
            missing += step;
        }
    }
    auto interval = input_interval_at(bar.timestamp);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native input is not aligned");
        return false;
    }
    const bool realtime_labels = running
        && running->phase == NativeRunPhase::Realtime;
    const bool canonical_labels = !legacy_tolerant_slot_labels()
        || realtime_labels;
    if (canonical_labels
        && !native_confirmed_bar_label_admitted(*interval, bar.timestamp)) {
        processing_input_ = false;
        present_refusal(engine, "native confirmed bar timestamp is not a canonical slot label");
        return false;
    }
    if (last_accepted_input_) {
        if (canonical_labels
            && interval->open_ms <= last_accepted_input_->open_ms) {
            processing_input_ = false;
            present_refusal(engine, "native duplicate overlapping input slot");
            return false;
        }
        if (canonical_labels && realtime_labels) {
            int64_t expected_label = last_accepted_input_->next_input_open_ms;
            if (expected_label <= last_accepted_input_->open_ms) {
                int64_t unit_ms = 0;
                switch (input_tf_.unit()) {
                case native_calendar::TimeframeUnit::Second: unit_ms = 1000; break;
                case native_calendar::TimeframeUnit::Minute: unit_ms = 60 * 1000; break;
                case native_calendar::TimeframeUnit::Day: unit_ms = 24 * 60 * 60 * 1000; break;
                case native_calendar::TimeframeUnit::Week:
                    unit_ms = 7 * 24 * 60 * 60 * 1000;
                    break;
                case native_calendar::TimeframeUnit::Month: break;
                }
                const int64_t count = input_tf_.count();
                if (unit_ms <= 0 || count <= 0
                    || unit_ms > std::numeric_limits<int64_t>::max() / count
                    || last_accepted_input_->open_ms
                        > std::numeric_limits<int64_t>::max() - unit_ms * count) {
                    processing_input_ = false;
                    present_refusal(engine, "native confirmed bar timestamp overflows");
                    return false;
                }
                const int64_t step = unit_ms * count;
                expected_label = last_accepted_input_->open_ms + step;
                // A raw label partition has no calendar successor, so its
                // grid steps over the slots the calendar declares closed, as
                // next_input_open_ms skips declared closed time: the night
                // after an RTH 15:45 is skipped to Tue 09:30, while a missing
                // in-session slot is still the gap refused below.
                while (expected_label < bar.timestamp
                       && !native_calendar::in_session(calendar_, expected_label, calendar_memo_)) {
                    if (expected_label > std::numeric_limits<int64_t>::max() - step) {
                        processing_input_ = false;
                        present_refusal(engine, "native confirmed bar timestamp overflows");
                        return false;
                    }
                    expected_label += step;
                }
            }
            if (bar.timestamp != expected_label) {
                processing_input_ = false;
                present_refusal(engine, "native stream has an in-session gap");
                return false;
            }
        }
    }
    const auto script_interval = script_interval_at(interval->open_ms);
    if (!script_interval) {
        processing_input_ = false;
        present_refusal(engine, "native script interval lookup failed");
        return false;
    }
    NativeInputContext input_context;
    input_context.input_interval = *interval;
    input_context.script_interval = *script_interval;
    input_context.input_index = index;
    input_context.completes_script_interval =
        interval->next_period_open_ms >= script_interval->next_period_open_ms;
    if (!invoke_input_callback(engine, bar, input_context)) {
        processing_input_ = false;
        return false;
    }
    if (!subscriptions_.empty()) {
        // Declared higher-timeframe series are pumped from the accepted-input
        // path, and the pump is ordered against the SCRIPT interval, not the
        // raw input. A script bar this input does not belong to and that its
        // own last input never sealed -- the lazy seal contribute_input would
        // otherwise perform below, AFTER the pump -- is calculated FIRST,
        // reading the series exactly as its own contributing inputs left
        // them. Only then does this input reach the series: the bucket it
        // completes, the boundary it closes, the gapped series it clears, the
        // lookahead bucket it opens, each ahead of this input's own
        // aggregation, matching and calculation point. A bare host therefore
        // never calculates a bar against a bucket that already holds a later
        // input. With no such script bar pending -- always so when the input
        // and script timeframes are equal, where each input seals its own
        // interval -- the seal is a no-op and this is the statement order it
        // always was; a run without subscriptions never enters the branch and
        // keeps its lazy seal inside contribute_input.
        if (!seal_stale_script(engine, script_interval->open_ms)) {
            processing_input_ = false;
            return false;
        }
        if (!pump_timeframe_subscriptions(engine, bar, index,
                                          interval->next_period_open_ms)) {
            processing_input_ = false;
            return false;
        }
    }
    last_accepted_input_ = *interval;
    last_observed_slot_open_ = interval->open_ms;
    last_finalized_input_ = *interval;
    last_price_ = bar.close;
    has_last_price_ = true;
    if (!contribute_input(engine, bar, *interval, index, InputContribution::ConfirmedBar)) {
        processing_input_ = false;
        return false;
    }
    processing_input_ = false;
    return !failed();
}

void NativeExecutionConsumer::pump_batch(BacktestEngine& engine, const Bar* bars, int n) {
    // Every script bar this pump seals reads the bars around it off this
    // array (present_session_day); the view ends with the call.
    struct PumpView {
        NativeExecutionConsumer& consumer;
        PumpView(NativeExecutionConsumer& owner, const Bar* input, int count) : consumer(owner) {
            consumer.pump_bars_ = input;
            consumer.pump_n_ = count;
            consumer.pumped_last_index_ = -1;
        }
        ~PumpView() {
            consumer.pump_bars_ = nullptr;
            consumer.pump_n_ = 0;
            consumer.pumped_last_index_ = -1;
        }
    } view(*this, bars, n);
    // A batch knows its length, so the two logs it appends to -- the driver
    // log and the command history -- are sized for it instead of regrown by
    // doubling, where every regrowth moves the whole log into a freshly
    // faulted block and unmaps the old one. Capacity is not observable: no
    // hash, readback or event reads it. A reservation the allocator refuses
    // leaves the log growing as it always did.
    //
    // The driver log is reserved exactly, up front, where its length is a
    // fact of the spec: one script bar per input (a Passthrough pairing) and
    // a fixed number of points per script bar -- the four modeled waypoints
    // or a synthesized path's samples, and the after-calculation close --
    // with a small allowance for current executions. Any other driver log (a
    // lower feed, volume-weighted samples, an aggregated script timeframe)
    // and every command history, whose length the host's commands decide, is
    // sized at doubling checkpoints from the rate this batch has shown so
    // far: to its projected end and an eighth, but never beyond
    // kPresizeFactor times what the log already holds, so a burst early in a
    // run cannot reserve address space in proportion to the rest of the
    // batch.
    constexpr std::size_t kPresizeFactor = 16;
    const std::size_t total = n > 0 ? static_cast<std::size_t>(n) : 0;
    const std::size_t driver_base = driver_log_.size();
    const std::size_t history_base = requests_.history().size();
    const auto hint = [](auto&& reserve) {
        try {
            reserve();
        } catch (const std::bad_alloc&) {
        }
    };
    if (const auto* spec = spec_ptr(); spec && total > 0
        && pairing_.pairing == native_calendar::TimeframePairing::Passthrough) {
        std::size_t per_bar = 0;
        if (spec->intrabar.is_none()) {
            per_bar = 4;
        } else if (const auto* synthesized = spec->intrabar.synthesized_path();
                   synthesized && !synthesized->volume_weighted) {
            per_bar = static_cast<std::size_t>(synthesized->samples);
        }
        if (per_bar != 0) {
            if (spec->close_execution == NativeCloseExecution::AfterCalculation) ++per_bar;
            const std::size_t points = per_bar * total;
            hint([&] {
                reserve_driver_log(std::min(driver_base + points + points / 64 + 16,
                                            driver_log_.max_size()));
            });
        }
    }
    // The capacity to give a log after `consumed` inputs, or 0 to leave it.
    const auto presize = [total](std::size_t base, std::size_t size, std::size_t capacity,
                                 std::size_t consumed, std::size_t max_size) -> std::size_t {
        if (size <= base || size > max_size / (2 * kPresizeFactor)) return 0;
        const std::size_t grown = size - base;
        if (grown > std::numeric_limits<std::size_t>::max() / total) return 0;
        const std::size_t projected = size + grown * (total - consumed) / consumed;
        if (projected <= capacity) return 0;
        const std::size_t wanted = std::min(projected + projected / 8, max_size);
        const std::size_t bound = size * kPresizeFactor;
        if (wanted <= bound) return wanted;
        // At the bound, grow only once the log could not absorb another
        // window like the one it has just recorded.
        return size + grown > capacity ? bound : 0;
    };
    std::size_t checkpoint = 64;
    for (int i = 0; i < n; ++i) {
        // The previous input's closing check is this loop's last statement,
        // and no host code runs between it and here: the projection it has
        // just compared cannot have moved, so every input after the first
        // re-reads only the abort flag, the one thing another thread may set.
        if (!(i == 0 ? check_abort_or_projection(engine, NativeFailureOperation::Input)
                     : check_abort(engine, NativeFailureOperation::Input))) return;
        if (!consume_confirmed_input(engine, bars[i], i, i + 1 == n)) {
            if (!failed()) {
                fail(engine, NativeFailure{NativeFailureCode::Preflight,
                                           NativeFailureOperation::Input});
            }
            return;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return;
        if (static_cast<std::size_t>(i) + 1 == checkpoint && checkpoint < total) {
            const std::size_t consumed = checkpoint;
            checkpoint *= 2;
            if (const std::size_t points = presize(driver_base, driver_log_.size(),
                                                   driver_log_.capacity(), consumed,
                                                   driver_log_.max_size())) {
                hint([&] { reserve_driver_log(points); });
            }
            const auto& history = requests_.history();
            if (const std::size_t events = presize(history_base, history.size(),
                                                   history.capacity(), consumed,
                                                   history.max_size())) {
                hint([&] { requests_.reserve(events); });
            }
        }
    }
}

void NativeExecutionConsumer::run_simple(BacktestEngine& engine, const Bar* bars, int n) {
    NativeBeginArgs args{bars, n, {}, {}, false, 4,
        MagnifierDistribution::ENDPOINTS, engine.magnifier_volume_weighted_, 2};
    args.simple_run = true;
    if (!prepare_public_begin(engine, args)) return;
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!preflight_bars(engine, bars, n, false) || !preflight_intrabar_path(engine)) return;
        const int64_t initial = n > 0 ? bars[0].timestamp
                                      : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, bars, n);
        if (failed()) return;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        leave_running();
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::run_tf(BacktestEngine& engine,
                                     const Bar* input_bars, int n_input,
                                     const std::string& input_tf,
                                     const std::string& script_tf,
                                     bool bar_magnifier, int magnifier_samples,
                                     MagnifierDistribution magnifier_dist) {
    const NativeBeginArgs args{input_bars, n_input, input_tf, script_tf, bar_magnifier,
        magnifier_samples, magnifier_dist, engine.magnifier_volume_weighted_, 2};
    if (!prepare_public_begin(engine, args)) return;
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        const auto* configured = spec_ptr();
        if ((bar_magnifier || magnifier_samples != 4
             || magnifier_dist != MagnifierDistribution::ENDPOINTS)
            && (!configured || configured->intrabar.is_none())) {
            present_refusal(engine, "native magnifier arguments require an intrabar path");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)
            || !preflight_intrabar_path(engine)) return;
        const int64_t initial = n_input > 0 ? input_bars[0].timestamp
                                            : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, input_bars, n_input);
        if (failed()) return;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        leave_running();
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::run_rich(BacktestEngine& engine,
                                       const Bar* input_bars, int n_input,
                                       const std::string& input_tf, const std::string& script_tf,
                                       const std::unordered_map<std::string, std::string>& inputs,
                                       const SymInfo& syminfo,
                                       const void* overrides,
                                       bool bar_magnifier, int magnifier_samples,
                                       MagnifierDistribution magnifier_dist) {
    NativeBeginArgs args{input_bars, n_input, input_tf, script_tf, bar_magnifier,
        magnifier_samples, magnifier_dist, engine.magnifier_volume_weighted_, 2};
    args.inputs = &inputs;
    args.syminfo = &syminfo;
    args.overrides_opaque = overrides;
    if (!prepare_public_begin(engine, args)) return;
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)
            || !preflight_intrabar_path(engine)) return;
        const int64_t initial = n_input > 0 ? input_bars[0].timestamp
                                            : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, input_bars, n_input);
        if (failed()) return;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        leave_running();
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

bool NativeExecutionConsumer::stream_begin(BacktestEngine& engine,
                                           const Bar* warmup_bars, int n_warmup,
                                           const std::string& input_tf,
                                           const std::string& script_tf) {
    // Preserve the live stream before asking the provider to stage/configure a
    // new run.  The legacy route diagnoses this state first; in particular,
    // no warmup copy, adapter reset, or broker/spec mutation may occur.
    if (engine.stream_phase_ == BacktestEngine::StreamPhase::REALTIME) {
        render(engine, "stream is already realtime");
        return false;
    }
    if (const auto* running = std::get_if<NativeRunning>(&state_);
        running && running->phase == NativeRunPhase::Realtime) {
        render(engine, "stream is already realtime");
        return false;
    }
    // Native hosts that already have a strict staged spec can be rejected
    // before the provider is entered.  Source providers deliberately use the
    // legacy-tolerant policy and perform their equivalent borrowed-array check
    // in prepare_native_begin, where the warmup flag is formed.
    if (!failed()) {
        const auto* staged = spec_ptr();
        if (staged && staged->slot_label_policy == NativeSlotLabelPolicy::Canonical
            && !preflight_bars(engine, warmup_bars, n_warmup, true, true)) {
            return false;
        }
    }
    NativeBeginArgs args{warmup_bars, n_warmup, input_tf, script_tf, false, 4,
        MagnifierDistribution::ENDPOINTS, engine.magnifier_volume_weighted_, 2};
    args.is_stream = true;
    args.warmup_n = n_warmup;
    if (!prepare_public_begin(engine, args)) return false;
    if (!admit_public_begin(engine, "native stream_begin requires Ready")) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return false;
        }
        // FP6: a curve the run itself declared (configure_fx_curve) is the
        // stream's FX epoch -- immutable from Ready to the end of the run and
        // named by the continuation identity, so every realtime conversion
        // reads a rate that was fixed before the first warmup bar. The series
        // of the mutable setter ingress stays refused: its owner revalues on a
        // broker clock of its own, which has no realtime route.
        if (staged_fx_curve_ && staged_ingress_fx_) {
            present_refusal(engine,
                "timestamped account-currency FX is not supported by streaming");
            return false;
        }
        const auto* spec = spec_ptr();
        auto parsed = native_calendar::parse_timeframe(spec->input_tf);
        auto script = native_calendar::parse_timeframe(spec->script_tf);
        if (!parsed || !script) {
            present_refusal(engine, "native stream timeframe parse failed");
            return false;
        }
        const auto stream_pair = native_calendar::stream_compatibility(*parsed, *script);
        if (stream_pair.pairing == native_calendar::TimeframePairing::StreamMonthlyInputRefused) {
            present_refusal(engine, "native stream refuses monthly input");
            return false;
        }
        if (n_warmup <= 0 || warmup_bars == nullptr) {
            present_refusal(engine, "native stream warmup requires at least one bar");
            return false;
        }
        if (!preflight_bars(engine, warmup_bars, n_warmup, true)
            || !preflight_intrabar_path(engine)) return false;
        const auto* preflight_spec = spec_ptr();
        if (preflight_spec && n_warmup > 0
            && (!std::isfinite(warmup_bars[n_warmup - 1].close)
                || warmup_bars[n_warmup - 1].close <= 0.0)
            && native_feed_tolerance_enabled(
                preflight_spec->legacy_tolerance,
                NativeFeedTolerance::WarmupNonNegativeOHLC)) {
            present_refusal(engine, "stream warmup final close must be finite and positive");
            return false;
        }
        if (!begin_ready(engine, NativeRunPhase::Warmup, warmup_bars[0].timestamp)) return false;
        pump_batch(engine, warmup_bars, n_warmup);
        if (failed()) return false;
        if (auto* running = std::get_if<NativeRunning>(&state_)) {
            running->phase = NativeRunPhase::Realtime;
        }
        engine.stream_phase_ = BacktestEngine::StreamPhase::REALTIME;
        engine.stream_observe_actions_ = true;
        engine.stream_order_actions_.clear();
        engine.stream_action_sequence_ = 0;
        if (!has_last_price_ && n_warmup > 0) {
            last_price_ = warmup_bars[n_warmup - 1].close;
            has_last_price_ = true;
        }
        return true;
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_bar(BacktestEngine& engine, const Bar& bar) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            present_refusal(engine, "native stream_push_bar requires realtime");
            return false;
        }
        if (refuse_mixed_input_mode(engine, InputMode::ConfirmedBars)) return false;
        if (!native_bar_structurally_valid(bar)) {
            present_refusal(engine, "native confirmed bar has invalid OHLCV");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        if (!consume_confirmed_input(engine, bar, next_interval_index_, false)) return false;
        select_input_mode(InputMode::ConfirmedBars);
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::preflight_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (n < 0 || (n > 0 && ticks == nullptr)) {
        present_refusal(engine, "native tick array is invalid");
        return false;
    }
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running || running->phase != NativeRunPhase::Realtime) {
        present_refusal(engine, "native stream_push_tick requires realtime");
        return false;
    }
    if (refuse_mixed_input_mode(engine, InputMode::ObservedTicks)) return false;
    if (refuse_subscription_tick_input(engine)) return false;
    if (refuse_fx_curve_tick_input(engine)) return false;
    if (n == 0) return true;
    uint64_t prev_sequence = last_tick_sequence_;
    bool prev_has_sequence = has_tick_sequence_;
    uint64_t ordinals = next_timeline_ordinal_;
    std::optional<int64_t> volume_slot;
    double volume = 0.0;
    if (has_forming_) {
        volume_slot = forming_.timestamp;
        volume = forming_.volume;
    }
    int64_t prev_array_ts = 0;
    bool has_array_prev = false;
    for (int i = 0; i < n; ++i) {
        const TradeTick& tick = ticks[i];
        if (!std::isfinite(tick.price) || tick.price <= 0.0) {
            present_refusal(engine, "native tick price must be finite and positive");
            return false;
        }
        if (!std::isfinite(tick.quantity) || tick.quantity < 0.0) {
            present_refusal(engine, "native tick quantity must be finite and non-negative");
            return false;
        }
        if (has_floor_ && tick.timestamp < decision_floor_ms_) {
            present_refusal(engine, "native tick timestamp is backwards or regresses the decision floor");
            return false;
        }
        if (has_array_prev && tick.timestamp < prev_array_ts) {
            present_refusal(engine, "native tick timestamp is backwards or out of order");
            return false;
        }
        prev_array_ts = tick.timestamp;
        has_array_prev = true;
        if (tick.sequence != 0) {
            if (prev_has_sequence && tick.sequence <= prev_sequence) {
                present_refusal(engine, "native tick sequence must increase");
                return false;
            }
            prev_sequence = tick.sequence;
            prev_has_sequence = true;
        }
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp, calendar_memo_);
        if (!interval) {
            present_refusal(engine, "native tick is not aligned");
            return false;
        }
        const bool in_forming = has_forming_ && forming_.timestamp == interval->open_ms;
        if (last_finalized_input_ && interval->open_ms <= last_finalized_input_->open_ms
            && !in_forming) {
            present_refusal(engine, "native tick would reopen a closed input slot");
            return false;
        }
        if (!volume_slot || *volume_slot != interval->open_ms) {
            volume_slot = interval->open_ms;
            volume = 0.0;
        }
        volume += tick.quantity;
        if (!std::isfinite(volume)) {
            present_refusal(engine, "native tick volume overflow");
            return false;
        }
        if (ordinals == 0 || ordinals == std::numeric_limits<uint64_t>::max()) {
            present_refusal(engine, "native timeline ordinal exhausted");
            return false;
        }
        ++ordinals;
    }
    return true;
}

bool NativeExecutionConsumer::emit_quiet_carried_open(
        BacktestEngine& engine, const native_calendar::NativeInterval& interval) {
    if (!has_last_price_) return true;
    if (next_tradable_synthesis_cursor_ == interval.open_ms) return true;
    NativeDriverPoint point;
    point.coordinate = coordinate_from(interval, next_interval_index_,
                                       interval.eligible_open_ms,
                                       NativePriceProvenance::CarriedOpen,
                                       NativePathPhase::Open);
    point.coordinate.ordinal = take_ordinal(engine);
    point.raw_price = last_price_;
    point.matching = true;
    record_driver(point);
    match_point(engine, point);
    next_tradable_synthesis_cursor_ = interval.open_ms;
    if (failed()) return false;
    Bar quiet{last_price_, last_price_, last_price_, last_price_, 0.0, interval.open_ms};
    if (!contribute_input(engine, quiet, interval, next_interval_index_,
                          InputContribution::QuietCarried)) {
        return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::finalize_observed_tick_slot(
        BacktestEngine& engine,
        const native_calendar::NativeInterval& interval,
        NativeCompletionKind kind) {
    if (!has_forming_ || forming_.timestamp != interval.open_ms) return true;
    if (kind == NativeCompletionKind::PartialFinalized) {
        const bool equal = pairing_.pairing == native_calendar::TimeframePairing::Passthrough;
        if (equal) {
            NativeCoordinate calc;
            calc.interval_index = next_interval_index_;
            calc.open_ms = interval.open_ms;
            calc.eligible_open_ms = interval.eligible_open_ms;
            calc.last_traded_close_ms = interval.last_traded_close_ms;
            calc.next_period_open_ms = interval.next_period_open_ms;
            calc.next_input_open_ms = interval.next_input_open_ms;
            calc.source_price_time_ms = last_print_time_ms_;
            calc.completion = kind;
            calc.effective_time_ms = std::max(decision_floor_ms_, last_print_time_ms_);
            calc.provenance = NativePriceProvenance::PartialFinalized;
            calc.ordinal = take_ordinal(engine);
            raise_floor(calc.effective_time_ms);
            engine.current_bar_ = forming_;
            calculating_bar_ = forming_;
            calculating_bar_has_ = true;
            clear_partial();
            invoke_callback(engine, forming_, calc);
            if (failed()) return false;
            const auto* spec = spec_ptr();
            if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
                NativeDriverPoint point;
                point.coordinate = calc;
                point.coordinate.ordinal = take_ordinal(engine);
                point.coordinate.effective_time_ms = calc.effective_time_ms;
                point.coordinate.source_price_time_ms = last_print_time_ms_;
                point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
                point.raw_price = forming_.close;
                point.matching = true;
                record_driver(point);
                match_point(engine, point);
            }
        }
        has_forming_ = false;
        return !failed();
    }
    const Bar formed = forming_;
    has_forming_ = false;
    if (!contribute_input(engine, formed, interval, next_interval_index_,
                          InputContribution::ObservedTickSlot)) {
        return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::finalize_elapsed_slots(BacktestEngine& engine,
                                                     int64_t exclusive_end_ms) {
    std::optional<int64_t> cursor;
    if (last_finalized_input_) cursor = last_finalized_input_->next_input_open_ms;
    else if (last_accepted_input_) cursor = last_accepted_input_->next_input_open_ms;
    else if (last_observed_slot_open_) cursor = last_observed_slot_open_;
    while (cursor) {
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, *cursor, calendar_memo_);
        if (!interval) break;
        if (interval->next_period_open_ms > exclusive_end_ms) break;
        if (last_finalized_input_ && last_finalized_input_->open_ms == interval->open_ms) {
            if (interval->next_input_open_ms <= interval->open_ms) break;
            cursor = interval->next_input_open_ms;
            continue;
        }
        const bool forming_here = has_forming_ && forming_.timestamp == interval->open_ms;
        const bool observed = forming_here
            || (last_observed_slot_open_ && *last_observed_slot_open_ == interval->open_ms)
            || (last_accepted_input_ && last_accepted_input_->open_ms == interval->open_ms);
        const bool tradable = interval->last_traded_close_ms > interval->eligible_open_ms
            && native_calendar::in_session(calendar_, interval->eligible_open_ms, calendar_memo_);
        if (forming_here) {
            if (!finalize_observed_tick_slot(engine, *interval, NativeCompletionKind::Confirmed)) {
                return false;
            }
        } else if (!observed && tradable) {
            if (!emit_quiet_carried_open(engine, *interval)) return false;
        }
        last_finalized_input_ = *interval;
        if (interval->next_input_open_ms <= interval->open_ms) break;
        cursor = interval->next_input_open_ms;
        if (failed()) return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::deliver_tick(BacktestEngine& engine, const TradeTick& tick) {
    processing_input_ = true;
    select_input_mode(InputMode::ObservedTicks);
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp, calendar_memo_);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native tick is not aligned");
        return false;
    }
    if (!finalize_elapsed_slots(engine, interval->open_ms)) {
        processing_input_ = false;
        return false;
    }
    NativeDriverPoint point;
    point.coordinate = coordinate_from(*interval, next_interval_index_, tick.timestamp,
                                       NativePriceProvenance::ObservedPrint,
                                       NativePathPhase::None);
    point.coordinate.ordinal = take_ordinal(engine);
    point.coordinate.source_price_time_ms = tick.timestamp;
    point.raw_price = tick.price;
    if (tick.sequence != 0) point.sequence = tick.sequence;
    point.matching = true;
    point.excursion = true;
    record_driver(point);
    NativeTickContext tick_context;
    tick_context.decision.coordinate = point.coordinate;
    tick_context.decision.input_interval = *interval;
    if (const auto script_interval = script_interval_at(tick.timestamp)) {
        tick_context.decision.script_interval = *script_interval;
        tick_context.decision.script_bar_open_ms = script_interval->open_ms;
    } else {
        tick_context.decision.script_bar_open_ms = point.coordinate.open_ms;
    }
    tick_context.decision.sub_index = 0;
    tick_context.decision.sub_count = 1;
    tick_context.decision.is_terminal_sub_bar = true;
    tick_context.decision.sub_bar_open_ms = tick.timestamp;
    tick_context.decision.driver_statistics = driver_statistics_;
    present_session_day(tick_context.decision, tick_context.decision.script_bar_open_ms);
    tick_context.sequence = tick.sequence;
    const Bar tick_bar{tick.price, tick.price, tick.price, tick.price,
                       tick.quantity, tick.timestamp};
    calculating_bar_ = tick_bar;
    calculating_bar_has_ = true;
    // The print is the cursor, and it is real traded activity: the bar so far
    // folds its price and its quantity before the observation hook runs.
    note_partial_point(tick_context.decision.script_bar_open_ms, tick.price, tick.quantity);
    if (!invoke_tick_callback(engine, tick_bar, tick_context)) {
        processing_input_ = false;
        return false;
    }
    match_point(engine, point);
    apply_excursion(engine, tick.price);
    raise_floor(tick.timestamp);
    recalculate_at_point(engine, tick_bar, point);
    if (failed()) {
        processing_input_ = false;
        return false;
    }
    last_price_ = tick.price;
    has_last_price_ = true;
    last_print_time_ms_ = tick.timestamp;
    if (!has_forming_) {
        forming_ = Bar{tick.price, tick.price, tick.price, tick.price, tick.quantity,
                       interval->open_ms};
        has_forming_ = true;
    } else {
        forming_.high = std::max(forming_.high, tick.price);
        forming_.low = std::min(forming_.low, tick.price);
        forming_.close = tick.price;
        forming_.volume += tick.quantity;
    }
    last_observed_slot_open_ = interval->open_ms;
    if (tick.sequence != 0) {
        last_tick_sequence_ = tick.sequence;
        has_tick_sequence_ = true;
    }
    processing_input_ = false;
    return !failed();
}

bool NativeExecutionConsumer::stream_push_tick(BacktestEngine& engine, const TradeTick& tick) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!preflight_ticks(engine, &tick, 1)) return false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        return deliver_tick(engine, tick);
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!preflight_ticks(engine, ticks, n)) return false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        for (int i = 0; i < n; ++i) {
            if (!deliver_tick(engine, ticks[i])) return false;
            if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        }
        return !failed();
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Stream)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            present_refusal(engine, "native stream_advance_time requires realtime");
            return false;
        }
        if (refuse_mixed_input_mode(engine, InputMode::ObservedTicks)) return false;
        if (refuse_subscription_tick_input(engine)) return false;
        if (refuse_fx_curve_tick_input(engine)) return false;
        if (has_floor_ && timestamp_ms < decision_floor_ms_) {
            present_refusal(engine, "native time advance regresses the decision floor");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Stream)) return false;
        select_input_mode(InputMode::ObservedTicks);
        if (has_last_price_ || has_forming_) {
            processing_input_ = true;
            const bool ok = finalize_elapsed_slots(engine, timestamp_ms);
            processing_input_ = false;
            if (!ok) return false;
        }
        if (script_.has_data && !script_.sealed
            && timestamp_ms >= script_.interval.next_period_open_ms) {
            processing_input_ = true;
            seal_script(engine, NativeCompletionKind::Confirmed);
            script_ = ScriptBucket{};
            processing_input_ = false;
            if (failed()) return false;
        }
        raise_floor(timestamp_ms);
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Stream)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!std::holds_alternative<NativeRunning>(state_)) {
            present_refusal(engine, "native stream_end requires a running host");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Stream)) return false;
        if (finalize_partial_input_bar && has_forming_) {
            auto forming_interval = native_calendar::interval_containing(
                calendar_, input_tf_, forming_.timestamp, calendar_memo_);
            if (forming_interval) {
                if (!finalize_observed_tick_slot(engine, *forming_interval,
                                                 NativeCompletionKind::PartialFinalized)) {
                    return false;
                }
            }
        }
        if (failed()) return false;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) {
            render(engine, "native stream_end lost running state");
            return false;
        }
        leave_running();
        NativeRunSpec spec = std::move(running->spec);
        state_.emplace<NativeCompleted>(NativeCompleted{std::move(spec), NativeCompletion::StreamEnded});
        engine.stream_phase_ = BacktestEngine::StreamPhase::IDLE;
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

native_order::SubmitResult NativeExecutionConsumer::submit_with_surface(
        BacktestEngine& engine, const native_order::Request& request,
        native_order::CommandSurface surface) {
    if (!commands_allowed()) {
        throw std::runtime_error("native submit refused outside allowed phase");
    }
    native_order::CommandContext ctx;
    native_order::PreparedSubmit prepared;
    try {
        ctx = make_command_context(engine, request, surface);
        prepared = requests_.prepare_submit(
            request, ctx, engine.next_order_incarnation_, next_timeline_ordinal_);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command});
        render(engine, "native submit produced no preparation");
        throw std::runtime_error("native submit produced no preparation");
    }
    auto installed = requests_.install_submit(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command,
                                   0, static_cast<uint32_t>(*err)});
        render(engine, "native submit install failed");
        throw std::runtime_error("native submit install failed");
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::SubmitResult>>(installed);
    note_terminal_events(ok.events);
    clear_cohort_target_cache();
    catch_up_timeline();
    if (ok.result.status == native_order::SubmitStatus::Accepted) {
        ++engine.next_order_incarnation_;
        if (ok.result.handle) record_pre_open_birth(request, *ok.result.handle);
    }
    return std::move(ok.result);
}

native_order::ReplaceResult NativeExecutionConsumer::replace_with_surface(
        BacktestEngine& engine, const native_order::RequestHandle& target,
        const native_order::Request& request, native_order::CommandSurface surface,
        native_order::ReplaceOptions options) {
    if (!commands_allowed()) {
        throw std::runtime_error("native replace refused outside allowed phase");
    }
    native_order::CommandContext ctx;
    native_order::PreparedReplace prepared;
    try {
        ctx = make_command_context(engine, request, surface);
        prepared = requests_.prepare_replace(
            target, request, ctx, engine.next_order_incarnation_, next_timeline_ordinal_,
            options);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command});
        render(engine, "native replace produced no preparation");
        throw std::runtime_error("native replace produced no preparation");
    }
    const native_order::EventId predicted = prepared.predicted_event_id();
    const auto predicted_status = prepared.predicted().status;
    auto installed = requests_.install_replace(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command,
                                   predicted.ordinal, static_cast<uint32_t>(*err)});
        render(engine, "native replace install failed");
        throw std::runtime_error("native replace install failed");
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::ReplaceResult>>(installed);
    note_terminal_events(ok.events);
    if (predicted_status == native_order::ReplaceStatus::Replaced && ok.result.successor) {
        const auto* successor = requests_.find_live(*ok.result.successor);
        if (successor && std::holds_alternative<native_order::CohortClose>(successor->authority)) {
            // A resting cohort replacement changes trigger/definition facts,
            // not the physical opening roster captured by this derived view.
            // Preserve it across ordinary source bracket reissues; dependency
            // mutations below still clear it through install_mutation().
            retarget_cohort_target_cache(target, *ok.result.successor);
        } else {
            clear_cohort_target_cache();
        }
    } else if (predicted_status != native_order::ReplaceStatus::NotWorking) {
        clear_cohort_target_cache();
    }
    catch_up_timeline();
    if (predicted_status == native_order::ReplaceStatus::Replaced) {
        ++engine.next_order_incarnation_;
        if (ok.result.successor) record_pre_open_birth(request, *ok.result.successor);
        try {
            drain_parent_terminal(engine, predicted, target, NativeFailureOperation::Command);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Command, predicted.ordinal});
            render(engine, e.what());
            throw;
        }
        if (failed()) {
            throw std::runtime_error("native replace dependency cleanup failed");
        }
    }
    return std::move(ok.result);
}

native_order::SubmitResult NativeExecutionConsumer::submit(BacktestEngine& engine,
                                                           const native_order::Request& request) {
    return submit_with_surface(engine, request, native_order::CommandSurface::General);
}

native_order::ReplaceResult NativeExecutionConsumer::replace(
        BacktestEngine& engine,
        const native_order::RequestHandle& target,
        const native_order::Request& request,
        native_order::ReplaceOptions options) {
    return replace_with_surface(engine, target, request, native_order::CommandSurface::General,
                                options);
}

native_order::SubmitResult NativeExecutionConsumer::submit_market(
        BacktestEngine& engine, const native_order::Request& request) {
    return submit_with_surface(engine, request, native_order::CommandSurface::MarketOnly);
}

native_order::ReplaceResult NativeExecutionConsumer::replace_market(
        BacktestEngine& engine,
        const native_order::RequestHandle& target,
        const native_order::Request& request) {
    return replace_with_surface(engine, target, request, native_order::CommandSurface::MarketOnly);
}

native_order::CancelResult NativeExecutionConsumer::cancel(
        BacktestEngine& engine, const native_order::RequestHandle& target) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cancel refused outside allowed phase");
    }
    native_order::PreparedCancel prepared;
    try {
        prepared = requests_.prepare_cancel(target, next_timeline_ordinal_);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command});
        render(engine, "native cancel produced no preparation");
        throw std::runtime_error("native cancel produced no preparation");
    }
    const auto predicted_status = prepared.predicted().status;
    const native_order::EventId predicted = prepared.predicted_event_id();
    auto installed = requests_.install_cancel(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command,
                                   predicted.ordinal, static_cast<uint32_t>(*err)});
        render(engine, "native cancel install failed");
        throw std::runtime_error("native cancel install failed");
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::CancelResult>>(installed);
    note_terminal_events(ok.events);
    clear_cohort_target_cache();
    catch_up_timeline();
    if (predicted_status == native_order::CancelStatus::Cancelled) {
        try {
            drain_parent_terminal(engine, predicted, target, NativeFailureOperation::Command);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Command, predicted.ordinal});
            render(engine, e.what());
            throw;
        }
        if (failed()) {
            throw std::runtime_error("native cancel dependency cleanup failed");
        }
    }
    return std::move(ok.result);
}

// L7b visibility. A PendingUntilArmed child is not a working order before its
// arm: it is out of the working enumeration while its authority is still
// Wait, and listed from its ArmedEvent on. Every handle-addressed reader
// (replace, cancel, trail_state), the bulk cancels, matching (a waiting leg
// never matches under either value) and the continuation identity keep
// seeing it: visibility governs enumeration, not addressing.
bool hidden_until_armed(const native_order::LiveRequest& live) noexcept {
    const auto* wait = std::get_if<native_order::WaitForApplied>(&live.request().owner);
    return wait != nullptr
        && wait->visibility == native_order::NativeArmVisibility::PendingUntilArmed
        && std::holds_alternative<native_order::Wait>(live.authority);
}

std::vector<NativeWorkingRequest> NativeExecutionConsumer::working_requests() const {
    // Owning value rows in live order. The projection spelling is deliberate:
    // an observer reads what is left to execute, never the matcher's own
    // Remaining state.
    std::vector<NativeWorkingRequest> out;
    out.reserve(requests_.live().size());
    for (const auto& live : requests_.live()) {
        if (hidden_until_armed(live)) continue;
        NativeWorkingRequest row;
        row.definition = live.definition;
        row.remaining = project_live_remaining(live.remaining);
        row.trigger_state = live.trigger_state;
        out.push_back(std::move(row));
    }
    return out;
}

std::size_t NativeExecutionConsumer::cancel_all(BacktestEngine& engine) {
    // Cancelling an owner ends its waiting children in the same command, so
    // the answer is how many requests left the working book: exactly one
    // CancelledEvent each.
    const std::size_t before = requests_.live().size();
    if (before == 0) return 0;
    std::vector<native_order::RequestHandle> handles;
    handles.reserve(before);
    for (const auto& live : requests_.live()) handles.push_back(live.handle());
    for (const auto& handle : handles) {
        if (!requests_.find_live(handle)) continue;
        cancel(engine, handle);
    }
    const std::size_t after = requests_.live().size();
    return before > after ? before - after : 0;
}

std::size_t NativeExecutionConsumer::cancel_where(BacktestEngine& engine,
                                                  std::string_view text,
                                                  NativeRequestField field) {
    // Only the requests whose selected field matches are cancelled. A
    // dependent child still ends with a cancelled owner, but it is not
    // counted unless its own field matched and it was still live when the
    // loop reached it. The walk is the live book, the same one cancel_all
    // takes: no field is indexed, so a label may be reused or replaced
    // without any second copy of the book to keep in step.
    std::vector<native_order::RequestHandle> handles;
    for (const auto& live : requests_.live()) {
        const auto& request = live.request();
        const std::string& value =
            field == NativeRequestField::Label ? request.label : request.comment;
        if (std::string_view(value) == text) handles.push_back(live.handle());
    }
    std::size_t cancelled = 0;
    for (const auto& handle : handles) {
        if (!requests_.find_live(handle)) continue;
        if (cancel(engine, handle).status == native_order::CancelStatus::Cancelled) ++cancelled;
    }
    return cancelled;
}

native_order::CohortHandle NativeExecutionConsumer::cohort_open(BacktestEngine& engine) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cohort_open refused outside allowed phase");
    }
    try {
        const auto cohort = requests_.cohort_open();
        clear_cohort_target_cache();
        return cohort;
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
}

void NativeExecutionConsumer::cohort_add(
        BacktestEngine& engine, native_order::CohortHandle cohort,
        native_order::RequestHandle origin) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cohort_add refused outside allowed phase");
    }
    try {
        requests_.cohort_add(cohort, std::move(origin));
        clear_cohort_target_cache();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
}

void NativeExecutionConsumer::cohort_remove(
        BacktestEngine& engine, native_order::CohortHandle cohort,
        native_order::RequestHandle origin) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cohort_remove refused outside allowed phase");
    }
    try {
        requests_.cohort_remove(cohort, std::move(origin));
        clear_cohort_target_cache();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
}

double NativeExecutionConsumer::marked(const BacktestEngine& engine, double price) const {
    return engine.marked_equity(price);
}

std::size_t NativeExecutionConsumer::first_command_after(uint64_t after_ordinal) const noexcept {
    const auto& history = requests_.history();
    if (history.empty() || command_ordinal(history.back()) <= after_ordinal) return history.size();
    // A reader's cursor is usually a few events behind the tail: walk back
    // from it, and bisect only a long suffix.
    auto it = history.end();
    while (it != history.begin()) {
        auto prev = std::prev(it);
        if (command_ordinal(*prev) <= after_ordinal) break;
        it = prev;
        if (std::distance(it, history.end()) > 32) {
            it = std::upper_bound(history.begin(), it, after_ordinal,
                [](uint64_t ordinal, const native_order::CommandEvent& event) {
                    return ordinal < command_ordinal(event);
                });
            break;
        }
    }
    return static_cast<std::size_t>(std::distance(history.begin(), it));
}

std::vector<NativeMarketEvent> NativeExecutionConsumer::events_after(uint64_t after_ordinal) const {
    const auto& history = requests_.history();
    const auto command_begin = history.begin()
        + static_cast<std::ptrdiff_t>(first_command_after(after_ordinal));

    auto driver_begin = driver_log_.end();
    if (!driver_log_.empty() && driver_log_.back().coordinate.ordinal > after_ordinal) {
        auto it = driver_log_.end();
        while (it != driver_log_.begin()) {
            auto prev = std::prev(it);
            if (prev->coordinate.ordinal <= after_ordinal) {
                driver_begin = it;
                break;
            }
            it = prev;
            if (std::distance(it, driver_log_.end()) > 32) {
                driver_begin = std::upper_bound(driver_log_.begin(), it, after_ordinal,
                    [](uint64_t ordinal, const NativeDriverPoint& point) {
                        return ordinal < point.coordinate.ordinal;
                    });
                break;
            }
        }
        if (it == driver_log_.begin()) driver_begin = driver_log_.begin();
    }

    auto account_begin = account_log_.end();
    if (!account_log_.empty() && account_log_.back().ordinal > after_ordinal) {
        auto it = account_log_.end();
        while (it != account_log_.begin()) {
            auto prev = std::prev(it);
            if (prev->ordinal <= after_ordinal) {
                account_begin = it;
                break;
            }
            it = prev;
            if (std::distance(it, account_log_.end()) > 32) {
                account_begin = std::upper_bound(account_log_.begin(), it, after_ordinal,
                    [](uint64_t ordinal, const NativeAccountObservation& account) {
                        return ordinal < account.ordinal;
                    });
                break;
            }
        }
        if (it == account_log_.begin()) account_begin = account_log_.begin();
    }

    const std::size_t command_count = static_cast<std::size_t>(std::distance(command_begin, history.end()));
    const std::size_t driver_count = static_cast<std::size_t>(std::distance(driver_begin, driver_log_.end()));
    const std::size_t account_count = static_cast<std::size_t>(std::distance(account_begin, account_log_.end()));

    std::vector<NativeMarketEvent> out;
    out.reserve(command_count + driver_count + account_count);

    for (auto it = command_begin; it != history.end(); ++it) {
        const auto& event = *it;
        const uint64_t ordinal = command_ordinal(event);
        out.emplace_back(NativeMarketEvent{NativeEventKind::Command, ordinal, event, std::nullopt, std::nullopt});
    }
    for (auto it = driver_begin; it != driver_log_.end(); ++it) {
        const auto& point = *it;
        out.emplace_back(NativeMarketEvent{NativeEventKind::Driver, point.coordinate.ordinal, std::nullopt, point, std::nullopt});
    }
    for (auto it = account_begin; it != account_log_.end(); ++it) {
        const auto& account = *it;
        out.emplace_back(NativeMarketEvent{NativeEventKind::Account, account.ordinal, std::nullopt, std::nullopt, account});
    }
    const auto cmp = [](const NativeMarketEvent& a, const NativeMarketEvent& b) {
        if (a.ordinal != b.ordinal) return a.ordinal < b.ordinal;
        return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    };
    if (account_count > 0) {
        std::sort(out.begin(), out.end(), cmp);
    } else if (driver_count > 0 && command_count > 0) {
        std::inplace_merge(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(command_count), out.end(), cmp);
    }
    return out;
}

uint64_t NativeExecutionConsumer::event_high_water() const noexcept {
    uint64_t high = 0;
    const auto& history = requests_.history();
    if (!history.empty()) high = std::max(high, command_ordinal(history.back()));
    if (!driver_log_.empty()) high = std::max(high, driver_log_.back().coordinate.ordinal);
    if (!account_log_.empty()) high = std::max(high, account_log_.back().ordinal);
    return high;
}

void NativeExecutionConsumer::note_terminal_events(
        const native_order::EventRange& events) noexcept {
    const auto& history = requests_.history();
    const std::size_t end = std::min(history.size(), events.first_index + events.count);
    for (std::size_t index = events.first_index; index < end; ++index) {
        const auto& event = history[index];
        const bool terminal = std::visit([](const auto& payload) {
            using Event = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Event, native_order::CancelledEvent>
                          || std::is_same_v<Event, native_order::MatchRejectedEvent>) {
                return true;
            } else if constexpr (std::is_same_v<Event, native_order::ExecutionAppliedEvent>) {
                return payload.terminal;
            }
            return false;
        }, event);
        if (terminal) {
            terminal_receipt_high_water_ = std::max(
                terminal_receipt_high_water_, command_ordinal(event));
        }
    }
}

void NativeExecutionConsumer::reject_inherited_on_bar(BacktestEngine& engine) {
    try {
        refuse_source_mutation("on_bar");
    } catch (const std::exception& e) {
        render(engine, e.what());
    }
}

NativeStrategyHost::NativeStrategyHost()
    : BacktestEngine(NativeConsumerBindTag{}) {}

NativeStrategyHost::~NativeStrategyHost() = default;

void NativeStrategyHost::on_bar(const Bar&) {
    NativeExecutionConsumer::bound(*this).reject_inherited_on_bar(*this);
}

NativeSetupResult NativeStrategyHost::configure_native(const NativeRunSpec& spec) {
    return NativeExecutionConsumer::bound(*this).configure(*this, spec);
}

NativeFxCurveSetupResult NativeStrategyHost::configure_native_fx_curve(
        const NativeFxCurve& curve) {
    return NativeExecutionConsumer::bound(*this).configure_fx_curve(curve);
}

std::optional<Bar> NativeStrategyHost::native_series_bar(std::size_t subscription) const {
    return NativeExecutionConsumer::bound(*this)
        .series_bar(subscription);
}

NativeSetupResult NativeStrategyHost::declare_timeframe_subscriptions_result(
        std::vector<NativeTimeframeSubscription> subscriptions) {
    return NativeExecutionConsumer::bound(*this)
        .declare_timeframe_subscriptions(std::move(subscriptions));
}

// The established spelling, kept exactly: same signature, same staging, and
// the same answer this call has always given.
bool NativeStrategyHost::declare_timeframe_subscriptions(
        std::vector<NativeTimeframeSubscription> subscriptions) {
    return declare_timeframe_subscriptions_result(std::move(subscriptions)).status
        == NativeSetupStatus::Applied;
}

NativeSetupResult NativeStrategyHost::declare_auxiliary_feed_result(
        std::optional<NativeAuxiliaryFeed> feed) {
    return NativeExecutionConsumer::bound(*this).declare_auxiliary_feed(std::move(feed));
}

bool NativeStrategyHost::declare_auxiliary_feed(std::optional<NativeAuxiliaryFeed> feed) {
    return declare_auxiliary_feed_result(std::move(feed)).status == NativeSetupStatus::Applied;
}

NativeAuxiliaryAppendResult NativeStrategyHost::append_auxiliary_bars_result(const Bar* bars,
                                                                             std::size_t n) {
    return NativeExecutionConsumer::bound(*this).append_auxiliary_bars(*this, bars, n);
}

// The established spelling, kept exactly.
bool NativeStrategyHost::append_auxiliary_bars(const Bar* bars, std::size_t n) {
    return append_auxiliary_bars_result(bars, n).status == NativeSetupStatus::Applied;
}

std::optional<Bar> NativeStrategyHost::current_partial_bar() const {
    return NativeExecutionConsumer::bound(*this)
        .partial_bar();
}

std::uint64_t NativeStrategyHost::native_recalculation_count() const {
    return NativeExecutionConsumer::bound(*this)
        .recalculation_count();
}

std::uint64_t NativeStrategyHost::native_recalculations_skipped() const {
    return NativeExecutionConsumer::bound(*this)
        .recalculations_skipped();
}

NativeStateView NativeStrategyHost::native_state() const {
    return NativeExecutionConsumer::bound(*this).view();
}

native_order::SubmitResult NativeStrategyHost::submit(const native_order::Request& request) {
    return NativeExecutionConsumer::bound(*this).submit(*this, request);
}

native_order::ReplaceResult NativeStrategyHost::replace(
        const native_order::RequestHandle& target, const native_order::Request& request) {
    return NativeExecutionConsumer::bound(*this).replace(*this, target, request);
}

native_order::SubmitResult NativeStrategyHost::submit_market(const native_order::Request& request) {
    return NativeExecutionConsumer::bound(*this).submit_market(*this, request);
}

native_order::ReplaceResult NativeStrategyHost::replace_market(
        const native_order::RequestHandle& target, const native_order::Request& request) {
    return NativeExecutionConsumer::bound(*this).replace_market(*this, target, request);
}

native_order::ReplaceResult NativeStrategyHost::replace(
        const native_order::RequestHandle& target, const native_order::Request& request,
        native_order::ReplaceOptions options) {
    return NativeExecutionConsumer::bound(*this).replace(*this, target, request, options);
}

native_order::CancelResult NativeStrategyHost::cancel(const native_order::RequestHandle& target) {
    return NativeExecutionConsumer::bound(*this).cancel(*this, target);
}

std::vector<NativeWorkingRequest> NativeStrategyHost::native_working_requests() const {
    return NativeExecutionConsumer::bound(*this)
        .working_requests();
}

std::size_t NativeStrategyHost::cancel_all() {
    return NativeExecutionConsumer::bound(*this).cancel_all(*this);
}

std::size_t NativeStrategyHost::cancel_where(std::string_view comment) {
    return NativeExecutionConsumer::bound(*this).cancel_where(*this, comment);
}

std::size_t NativeStrategyHost::cancel_where(std::string_view text, NativeRequestField field) {
    return NativeExecutionConsumer::bound(*this).cancel_where(*this, text, field);
}

native_order::CohortHandle NativeStrategyHost::cohort_open() {
    return NativeExecutionConsumer::bound(*this).cohort_open(*this);
}

void NativeStrategyHost::cohort_add(
        native_order::CohortHandle cohort, native_order::RequestHandle origin) {
    NativeExecutionConsumer::bound(*this).cohort_add(*this, cohort, std::move(origin));
}

void NativeStrategyHost::cohort_remove(
        native_order::CohortHandle cohort, native_order::RequestHandle origin) {
    NativeExecutionConsumer::bound(*this).cohort_remove(*this, cohort, std::move(origin));
}

std::optional<NativeCurrentPointView> NativeStrategyHost::current_execution_point() const {
    return NativeExecutionConsumer::bound(*this)
        .current_execution_point();
}

std::optional<NativeTrailState> NativeStrategyHost::trail_state(
        const native_order::RequestHandle& target) const {
    return NativeExecutionConsumer::bound(*this)
        .trail_state(*this, target);
}

NativeCurrentExecutionPreview NativeStrategyHost::inspect_current_execution(
        const NativeCurrentExecution& command) const {
    return NativeExecutionConsumer::bound(*this)
        .inspect_current_execution(*this, command);
}

NativeCurrentExecutionResult NativeStrategyHost::execute_current(const NativeCurrentExecution& command) {
    return NativeExecutionConsumer::bound(*this).execute_current(*this, command);
}

NativePhysicalPosition NativeStrategyHost::physical_position() const {
    return NativeExecutionConsumer::bound(*this).position(*this);
}

// R5 gap lane N18: the book lot by lot. A read of what the engine already
// holds — the pyramid entries, their unconsumed entry fee and the excursion
// apply_excursion has sampled — marked at the caller's price with exactly the
// arithmetic marked_equity() and build_close_trade_with_costs() use, so a row
// reconciles bit-for-bit with the marked equity now and with the closed row
// later. Nothing is cached and nothing is written: this cannot move a hash.
std::vector<NativeOpenLot> NativeExecutionConsumer::open_lots(
        const BacktestEngine& engine, double mark) const {
    std::vector<NativeOpenLot> out;
    const auto n = engine.pyramid_entries_.size();
    if (n == 0) return out;
    out.reserve(n);
    const bool is_long = engine.position_side_ != PositionSide::SHORT;
    const double point_value = engine.syminfo_.pointvalue;
    const double fx = engine.active_account_currency_fx();
    for (std::size_t i = 0; i < n; ++i) {
        const PyramidEntry& lot = engine.pyramid_entries_[i];
        NativeOpenLot row;
        row.ordinal = i;
        row.entry_incarnation = lot.entry_incarnation;
        row.cycle = engine.position_cycle_seq_;
        row.side = is_long ? native_order::Side::Long : native_order::Side::Short;
        row.entry_label = lot.entry_id;
        row.entry_comment = lot.entry_comment;
        row.entry_time_ms = lot.time;
        row.entry_bar_index = lot.entry_bar_index;
        row.entry_price = lot.price;
        row.signed_units = is_long ? lot.qty : -lot.qty;
        row.entry_commission = engine.open_entry_commission(lot);
        row.mark = mark;
        // Price-points × units, the unit the sampler keeps the lot's extremes
        // in; scaled to account currency once, below, the way the accessors
        // and the closing row scale them.
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
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<NativeOpenLot> NativeStrategyHost::native_open_lots(double mark) const {
    return NativeExecutionConsumer::bound(*this)
        .open_lots(*this, mark);
}

NativeRiskState NativeExecutionConsumer::risk_state() const {
    NativeRiskState state;
    if (risk_limits() == nullptr) return state;
    state.blocked = risk_blocked();
    state.reason = risk_.run_block ? risk_.run_block
        : (state.blocked ? risk_.day_block : std::optional<native_order::RiskLimitKind>{});
    state.has_day = risk_.has_day;
    state.day_ordinal = risk_.day_ordinal;
    state.fills_today = risk_.fills_today;
    state.consecutive_loss_days = risk_.consecutive_loss_days;
    state.peak_equity = risk_.peak_equity;
    state.day_open_equity = risk_.day_open_equity;
    return state;
}

std::optional<double> NativeExecutionConsumer::host_liquidation_price(
        const BacktestEngine& engine) const {
    // A host query, not a check point: it has no cursor of its own, so it is
    // answered at the instant the engine is presenting -- which is what a host
    // asking "where is my level now" means.
    return liquidation_level(engine, engine.active_account_currency_fx());
}

std::optional<double> NativeExecutionConsumer::sized_units_preview(
        const native_order::Sized& sized, double price, double equity, double fx) const {
    const auto* spec = spec_ptr();
    if (!spec) return std::nullopt;
    return sized_basis_units(sized, price, equity, fx, *spec);
}

double NativeStrategyHost::native_marked_equity(double mark) const {
    return NativeExecutionConsumer::bound(*this).marked(*this, mark);
}

std::optional<double> NativeStrategyHost::native_sized_units(
        const native_order::Sized& sized, double price, double equity, double fx) const {
    return NativeExecutionConsumer::bound(*this)
        .sized_units_preview(sized, price, equity, fx);
}

std::optional<double> NativeStrategyHost::native_liquidation_price() const {
    return NativeExecutionConsumer::bound(*this)
        .host_liquidation_price(*this);
}

NativeRiskState NativeStrategyHost::native_risk_state() const {
    return NativeExecutionConsumer::bound(*this)
        .risk_state();
}

std::vector<NativeMarketEvent> NativeStrategyHost::native_events(uint64_t after_ordinal) const {
    return NativeExecutionConsumer::bound(*this)
        .events_after(after_ordinal);
}

int64_t NativeStrategyHost::native_decision_floor() const {
    return NativeExecutionConsumer::bound(*this).decision_floor();
}

uint64_t NativeStrategyHost::native_consumed_high_water() const {
    return NativeExecutionConsumer::bound(*this).high_water();
}

uint64_t NativeStrategyHost::native_continuation_hash() const {
    return NativeExecutionConsumer::bound(*this)
        .continuation_hash();
}

// R5 lane F3: the latch record_script_report_point takes at every
// kernel-recorded report point. Batch teardown moves the consumer into
// Completed after the last point, which would otherwise make the final scalar
// a different fold than the last recorded row.
std::uint64_t NativeStrategyHost::broker_state_hash_projection() const {
    const std::uint64_t execution = last_script_continuation_valid_
        ? NativeExecutionConsumer::bound(*this)
              .latched_continuation(last_script_continuation_hash_)
        : NativeExecutionConsumer::bound(*this).continuation_hash();
    return broker_state_hash_from_execution_hash(execution);
}

}  // inline namespace engine_script_run_v18

// The portable run-spec digest lives here, in the one translation unit that
// owns `hash_spec` (see its definition above, beside the continuation fold),
// so the digest cannot drift from the fields the continuation identity folds
// for a spec. It is declared in native_run_spec_v3 and so must be defined
// there: a sibling inline namespace of pineforge, not a nested scope of the
// engine epoch. Nothing else belongs in this block.
inline namespace native_run_spec_v3 {

uint64_t native_run_spec_digest(const NativeRunSpec& spec) noexcept {
    Fnv f;
    f.run_base = spec.identity.run_number;
    hash_spec(f, spec);
    return f.h;
}

}  // inline namespace native_run_spec_v3
}  // namespace pineforge
