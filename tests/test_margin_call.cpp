/*
 * test_margin_call.cpp — verify TradingView forced-liquidation (margin call).
 *
 * Covers the principal behaviours of process_margin_call /
 * margin_liquidation_price:
 *
 *   A. A 100%-equity SHORT held through an adverse (rising) move is force-
 *      liquidated. At least one "Margin call" exit is produced; the first one
 *      fills at the bar's adverse extreme (HIGH) and closes the documented 4x
 *      of the margin shortfall (capped at the full position). The reported
 *      margin_liquidation_price equals the closed-form formula while open.
 *
 *   B. A LONG at the default 100% margin has no adverse-price liquidation
 *      (the formula denominator margin/100 - direction = 0). A sub-lot
 *      opening affordability overage is held even through a later crash.
 *
 *   C. A one-lot-or-larger opening affordability shortfall is trimmed on the
 *      entry bar using entry affordability and exit-side fill semantics.
 *
 *   D. A LEVERAGED long (margin_long = 20 => 5x) IS liquidated when price falls
 *      far enough; the forced exit fills at the bar's adverse extreme (LOW).
 *
 *   E. The margin-call emulator can be switched off (set_margin_call_enabled
 *      false); the underwater short is then held with no forced exit.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) < tol;
}

namespace {

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c, double v) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = v; b.timestamp = ts;
    return b;
}

// Thin base exposing the protected closed-trade accessors / liquidation price
// for the test harness (these are protected on BacktestEngine, accessible only
// from subclasses).
class MCEngine : public pineforge::source::PineStrategyHost {
public:
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    int entry_bar(int i) const { return closed_trade_entry_bar_index(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
    double liq_price() const { return margin_liquidation_price(); }
    bool opening_pending() const { return opening_obligations_.pending(); }
    bool opening_eligible() const { return opening_obligations_.actionable(); }
    bool opening_default_short_reversal() const {
        return opening_obligations_.requires_adverse_pass();
    }
    double opening_raw_base() const {
        return opening_obligations_.raw_fill_base();
    }
    int live_entry_count() const { return position_entry_count_; }

protected:
    void seed_opening_check(double raw_base,
                           broker::OpeningContinuation continuation) {
        const uint64_t incarnation = pyramid_entries_.empty()
            ? 0 : pyramid_entries_.back().entry_incarnation;
        opening_obligations_.replace(broker::OpeningReceipt::check(
            {position_cycle_seq_, broker_fill_event_seq_, incarnation,
             bar_index_, current_bar_.timestamp}, raw_base, continuation));
    }

    bool opening_owner_matches_position() const {
        const auto& receipt = opening_obligations_.peek();
        return receipt && receipt->owner().positionCycle == position_cycle_seq_;
    }
};

// ---- A: 100%-equity short force-liquidated by a rising market --------------

class ShortLiqProbe : public MCEngine {
public:
    bool disable_mc_ = false;
    explicit ShortLiqProbe(bool disable_mc = false, double qty_step = 0.0,
                           double account_fx = 1.0,
                           double initial_capital = 1000.0) {
        initial_capital_ = initial_capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;          // size the short at 100% of equity
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;               // 1x, default TV margin
        process_orders_on_close_ = true;     // market entry fills at bar close
        disable_mc_ = disable_mc;
        qty_step_ = qty_step;                // 0 = no lot quantization
        account_currency_fx_ = account_fx;
        if (disable_mc_) set_margin_call_enabled(false);
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            // Short the whole account; never exit. Fills at bar0 close = 100.
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }
    }
};

static void test_short_margin_call() {
    std::printf("test_short_margin_call\n");

    // bar0 entry @ close=100 (qty = 1000/100 = 10, notional 1000 = equity).
    //   liqPrice (short, 100% margin) = ((1000/10) + 100) / 2 = 100.
    // bar1 opens AT liq=100 (no open-point deficit — finding-430 slices a
    // gap-open breach at the open) and rises: high=105 > liq=100 -> partial
    // 4x liquidation @ high=105.
    //   equity@105 = 1000 - (105-100)*10 = 950; reqMargin@105 = 10*105 = 1050.
    //   qmin = 10 - 950/105 = 0.952381; 4x = 3.809524 (< 10) -> partial fill.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: short fills @100
        mk_bar(2000, 100.0, 105.0,  99.5, 104.0, 1.0),  // 1: high 105 -> margin call
        mk_bar(3000, 104.0, 130.0, 103.0, 128.0, 1.0),  // 2: high 130 -> further call
        mk_bar(4000, 128.0, 140.0, 127.0, 139.0, 1.0),  // 3: keep rising
    };

    ShortLiqProbe eng;
    // Margin-call price while the full 10@100 short is open.
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() >= 1);
    // Every closed trade on this no-exit strategy must be a forced liquidation.
    bool all_margin = true;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) != std::string("Margin call"))
            all_margin = false;
    }
    CHECK(all_margin);

    // First liquidation: fills at bar1's adverse extreme (high = 105) and
    // closes ~3.8095 contracts (4x the shortfall), leaving the position open.
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(near(eng.trade_size(0), 3.80952381, 1e-4));
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
}

static void test_margin_liquidation_price_formula() {
    std::printf("test_margin_liquidation_price_formula\n");

    // Re-run only the entry bar (no adverse move yet) and read the formula
    // before any liquidation: short 10 @ 100, equity 1000, margin 100% ->
    // liqPrice = ((1000/10) + 100) / 2 = 100.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: short fills @100
        mk_bar(2000, 100.0, 100.0,  99.5, 100.0, 1.0),  // 1: no breach (high == liq)
    };
    ShortLiqProbe eng;
    eng.run(bars.data(), (int)bars.size());
    // No adverse move above 100 -> no margin call, position still open.
    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.liq_price(), 100.0));
}

static void test_short_margin_call_disabled() {
    std::printf("test_short_margin_call_disabled\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 105.0,  99.5, 104.0, 1.0),
        mk_bar(3000, 104.0, 200.0, 103.0, 199.0, 1.0),  // huge adverse move
    };
    ShortLiqProbe eng(/*disable_mc=*/true);
    eng.run(bars.data(), (int)bars.size());
    // With the emulator off the underwater short is simply held: no exits.
    CHECK(eng.trade_count() == 0);
}

// ---- A': lot quantization floors each forced-liquidation lot to qty_step ----

// Returns true when |x| is an integer multiple of step (within tol).
static bool is_multiple_of(double x, double step, double tol = 1e-9) {
    if (step <= 0.0) return false;
    double n = std::round(x / step);
    return std::fabs(x - n * step) <= tol;
}

static void test_short_margin_call_qty_step() {
    std::printf("test_short_margin_call_qty_step\n");

    // Same scenario as test_short_margin_call. The shortfall (minimum restore
    // qty) is 3.80952381/4 = 0.95238095 contracts. TradingView floors the
    // restore qty to the lot step BEFORE the 4x over-liquidation (KI-31), so:
    //   floor(0.95238095 / 0.5) * 0.5 = 0.5, then * 4 = 2.0 (an exact step
    // multiple). Flooring the 4x product instead (the old bug) gave 3.5 and
    // desynced multi-nibble cascades from TV. The exit price is unchanged
    // (bar1 high = 105).
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: short fills @100
        mk_bar(2000, 100.0, 105.0,  99.5, 104.0, 1.0),  // 1: high 105 -> margin call
        mk_bar(3000, 104.0, 130.0, 103.0, 128.0, 1.0),  // 2: high 130 -> further call
        mk_bar(4000, 128.0, 140.0, 127.0, 139.0, 1.0),  // 3: keep rising
    };

    const double step = 0.5;
    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/step);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() >= 1);
    // First quantized lot: 4 * floor(shortfall/step)*step = 4 * 0.5 = 2.0,
    // an exact multiple of the 0.5 step (floor-before-4x per KI-31).
    CHECK(near(eng.trade_size(0), 2.0));
    CHECK(is_multiple_of(eng.trade_size(0), step));
    // Quantization never enlarges the lot: floored <= unquantized 3.80952381.
    CHECK(eng.trade_size(0) <= 3.80952381 + 1e-9);
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(eng.exit_comment(0) == std::string("Margin call"));

    // Negative sentinel for the 1x-long fix: the established short cascade
    // stays exactly two forced rows, including the residual close at bar2 HIGH.
    CHECK(eng.trade_count() == 2);
    CHECK(near(eng.trade_size(1), 8.0));
    CHECK(near(eng.exit_price(1), 130.0));
    CHECK(eng.exit_comment(1) == std::string("Margin call"));

    // Every partial (non-final) forced lot must be a step multiple. The final
    // exit closes whatever residual remains (the position size itself is not a
    // step multiple, so only the intermediate nibbles are checked).
    int partial_checked = 0;
    for (int i = 0; i + 1 < eng.trade_count(); ++i) {
        CHECK(is_multiple_of(eng.trade_size(i), step));
        ++partial_checked;
    }
    CHECK(partial_checked >= 1);

    // Teeth: with qty_step = 0 the same first lot is the UNQUANTIZED 3.80952381,
    // which is NOT a multiple of 0.5 — proving the assertion above can fail.
    ShortLiqProbe raw(/*disable_mc=*/false, /*qty_step=*/0.0);
    raw.run(bars.data(), (int)bars.size());
    CHECK(near(raw.trade_size(0), 3.80952381, 1e-4));
    CHECK(!is_multiple_of(raw.trade_size(0), step));
}

// A $100-scale all-in short can breach margin by less than one quantity step.
// TV still emits a Margin-call trade, but its truncated cover amount is zero;
// the broker closes the full residual instead of fabricating a one-step nibble.
class ShortZeroCoverProbe : public MCEngine {
public:
    explicit ShortZeroCoverProbe(double qty_step) {
        initial_capital_ = 100.384250;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = qty_step;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }
    }
};

static void test_short_margin_call_zero_cover_closes_full_residual() {
    std::printf("test_short_margin_call_zero_cover_closes_full_residual\n");
    std::vector<Bar> bars = {
        // Signal close freezes floor(100.38425 / 3788 / 0.0001) = 0.0265.
        mk_bar(1000, 3788.00, 3788.00, 3788.00, 3788.00, 1.0),
        // At HIGH: equity=100.37153, required=100.39472, so
        // q_min=0.000006121... < qty_step and the truncated cover is zero.
        mk_bar(2000, 3788.00, 3788.48, 3766.62, 3775.78, 1.0),
    };

    ShortZeroCoverProbe eng(/*qty_step=*/0.0001);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 3788.00));
    CHECK(near(eng.exit_price(0), 3788.48));
    CHECK(near(eng.trade_size(0), 0.0265));
    CHECK(near(eng.position_size(), 0.0));
}

// Without the opt-in metadata the generic one-contract fallback applies. The
// position here is 0.0265 contracts, far below one, so the min(1.0, qty) cap
// closes the whole residual anyway — TV's own tapes contain 1,007 such capped
// floor-zero events and match this 1,007/1,007.
static void test_short_margin_call_zero_cover_closes_sub_one_residual() {
    std::printf("test_short_margin_call_zero_cover_closes_sub_one_residual\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 3788.00, 3788.00, 3788.00, 3788.00, 1.0),
        mk_bar(2000, 3788.00, 3788.48, 3766.62, 3775.78, 1.0),
    };

    ShortZeroCoverProbe eng(/*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.0265));
    CHECK(near(eng.position_size(), 0.0));
}

static void test_short_margin_call_exact_one_step_roundoff_keeps_four_x_nibble() {
    std::printf("test_short_margin_call_exact_one_step_roundoff_keeps_four_x_nibble\n");
    constexpr double step = 0.0001;
    // An all-in short of 10 @ entry from 10*entry of equity leaves
    //   q_min = 20 * (adverse - entry) / adverse
    // at the adverse high. The short cascade marks that deficit at the
    // mintick-ROUNDED high (process_margin_call, the sizing-basis fix: the
    // broker ledger is on-tick, 32 vs 0 reproduced slices on the NYSE:F
    // tape), so the shape is built on ON-TICK prices where the rounding is
    // an identity and the pin measures the lot rule alone: one penny of
    // adverse move at 2000.00 is mathematically exactly one 0.0001 lot,
    // but the engine's equivalent arithmetic represents the quotient just
    // below 1 (0.99999999998). A bare floor would erase the lot and
    // incorrectly enter the zero-cover full-close fallback.
    //
    // This case used to sit at 10 @ 100 with a SYNTHETIC sub-tick high of
    // 2000 / (20 - step) = 100.0005..., marked raw; the on-tick mark reads
    // that high as 100.00, exactly at the liquidation price, and no slice
    // fires — which is the E1 pin of test_sizing_basis_mintick.cpp, not a
    // lot-rule question. The lot rule pinned here is unchanged.
    const double entry = 1999.99;
    const double adverse = 2000.00;
    const double capital = 10.0 * entry;
    const double equity_at_high = capital - (adverse - entry) * 10.0;
    const double q_min = 10.0 - equity_at_high / adverse;
    const double step_count = q_min / step;
    CHECK(q_min < step);
    CHECK(std::abs(step_count - std::round(step_count)) < 1e-6);

    std::vector<Bar> bars = {
        mk_bar(1000, entry, entry, entry - 1.0, entry, 1.0),
        mk_bar(2000, entry, adverse, entry - 1.0, entry, 1.0),
    };

    // Without the opt-in metadata the representation jitter is NOT rounded
    // away, so this lands on the generic floor-zero discontinuity and closes
    // one whole contract.
    ShortLiqProbe default_eng(/*disable_mc=*/false, /*qty_step=*/step,
                              /*account_fx=*/1.0, capital);
    default_eng.run(bars.data(), (int)bars.size());
    CHECK(default_eng.trade_count() == 1);
    CHECK(default_eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(default_eng.trade_size(0), 1.0, 1e-12));
    CHECK(near(default_eng.position_size(), -9.0, 1e-12));

    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/step,
                      /*account_fx=*/1.0, capital);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    // finding-446: the adverse extreme is a RAW BAR PRICE and books at the
    // nearest tick (an identity on this on-tick high), never the buy-side
    // ceil.
    CHECK(near(eng.exit_price(0), adverse, 1e-12));
    CHECK(near(eng.trade_size(0), 4.0 * step, 1e-12));
    CHECK(near(eng.position_size(), -(10.0 - 4.0 * step), 1e-12));
}

static void test_short_margin_call_just_below_step_slices_one_contract() {
    std::printf("test_short_margin_call_just_below_step_slices_one_contract\n");
    constexpr double step = 0.0001;
    // Same all-in shape as the exact-one-step case above (q_min =
    // 20 * (adverse - entry) / adverse, marked at the on-tick high), one
    // penny of adverse move from a 2000.00 entry: q_min / step =
    // 2000 / 2000.01 = 0.999995, genuinely below one step by 5e-6 — more
    // than the 1e-6 representation guard. It must quantize to zero and land
    // on the settled floor-zero discontinuity: TV closes ONE whole contract
    // and HOLDS the remainder. The full-residual opt-in no longer overrides
    // that settled slice — at an eps-scale deficit it used to liquidate the
    // whole ten-contract position here, an exit TV never prints (finding
    // 279, serhan ADX). (Formerly a synthetic sub-tick high of
    // 2000 / (20 - step * (1 - 2e-6)) over a 10 @ 100 short, marked raw;
    // the on-tick mark reads that print as 100.00 and fires nothing — see
    // the sibling above.)
    const double entry = 2000.00;
    const double adverse = 2000.01;
    const double capital = 10.0 * entry;
    const double equity_at_high = capital - (adverse - entry) * 10.0;
    const double q_min = 10.0 - equity_at_high / adverse;
    const double step_count = q_min / step;
    CHECK(q_min < step);
    CHECK(std::abs(step_count - std::round(step_count)) > 1e-6);

    std::vector<Bar> bars = {
        mk_bar(1000, entry, entry, entry - 1.0, entry, 1.0),
        mk_bar(2000, entry, adverse, entry - 1.0, entry, 1.0),
    };

    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/step,
                      /*account_fx=*/1.0, capital);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    // finding-446: the adverse extreme is a RAW BAR PRICE and books at the
    // nearest tick (an identity on this on-tick high), never the buy-side
    // ceil.
    CHECK(near(eng.exit_price(0), adverse, 1e-12));
    CHECK(near(eng.trade_size(0), 1.0, 1e-12));
    CHECK(near(eng.position_size(), -9.0, 1e-12));

    // Control: the flag-off engine takes the identical settled slice.
    ShortLiqProbe default_eng(/*disable_mc=*/false, /*qty_step=*/step,
                              /*account_fx=*/1.0, capital);
    default_eng.run(bars.data(), (int)bars.size());
    CHECK(default_eng.trade_count() == 1);
    CHECK(near(default_eng.trade_size(0), 1.0, 1e-12));
    CHECK(near(default_eng.position_size(), -9.0, 1e-12));
}

// ETH-scale eps-deficit shape (finding 279, boztilkiserhan serhan ADX
// 2025-06-08 / 2026-01-17 / 2026-03-22): an all-in multi-contract short whose
// entry bar prints an adverse extreme a tick or two past the frozen sizing
// close leaves a free-margin deficit of a few tenths of a USD. The restore
// quantity floors to zero at the 0.0001 lot step and TV closes exactly ONE
// contract at the adverse extreme, HOLDING the remainder — under the
// full-residual opt-in exactly as without it. The engine used to liquidate
// the ENTIRE position at that extreme when the opt-in was set.
class ShortEpsDeficitProbe : public MCEngine {
public:
    explicit ShortEpsDeficitProbe(bool full_residual) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;          // freeze 10000/2500 = 4.0 short
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;    // fill at next bar OPEN
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("margin_zero_cover_full_liquidation",
                             full_residual ? 1.0 : 0.0);
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }
    }
};

static void test_short_margin_call_eps_deficit_slices_one_contract_and_holds() {
    std::printf(
        "test_short_margin_call_eps_deficit_slices_one_contract_and_holds\n");
    std::vector<Bar> bars = {
        // Signal close 2500 freezes qty = 4.0 exactly.
        mk_bar(1000, 2500.00, 2500.00, 2500.00, 2500.00, 1.0),
        // Entry @ open 2499.99 (notional 9999.96 < equity, admitted). The
        // bar's HIGH 2500.01 is two ticks adverse: equity there 9999.92 vs
        // required 10000.04 -> deficit 0.12 USD, q_min = 4.8e-5 < one lot.
        mk_bar(2000, 2499.99, 2500.01, 2495.00, 2496.00, 1.0),
        // No further breach: the remaining 3.0 contracts are HELD.
        mk_bar(3000, 2496.00, 2499.00, 2490.00, 2492.00, 1.0),
    };

    for (int full_residual = 0; full_residual <= 1; ++full_residual) {
        ShortEpsDeficitProbe eng(full_residual != 0);
        eng.run(bars.data(), (int)bars.size());

        CHECK(eng.trade_count() == 1);
        CHECK(eng.exit_comment(0) == std::string("Margin call"));
        CHECK(near(eng.entry_price(0), 2499.99, 1e-9));
        CHECK(near(eng.exit_price(0), 2500.01, 1e-9));
        CHECK(near(eng.trade_size(0), 1.0, 1e-9));
        CHECK(near(eng.position_size(), -3.0, 1e-9));
    }
}

// finding-308's chronological pre-exit hook carries its own copy of the
// floor-zero arithmetic (the deficit is discovered at the adverse extreme,
// before a same-bar priced exit fills). The settled slice must win there
// too — an eps-deficit found on that route is still an eps-deficit.
//
// Tape geometry: boztilkiserhan serhan1 scalp, ETHUSDT.P 15m 2025-10-19
// 08:15 UTC (O 3886.31 / H 3960 / L 3810), carrying short 2.119 @ 3879.36.
// initial_capital 8561.92 puts equity at the adverse high 3960 at
// 8391.04384 against required margin 8391.24 — a 0.196 USD deficit, so
// q_min = 4.95e-5 and floors to zero at the 0.0001 lot.
class ShortEpsDeficitChronologyProbe : public MCEngine {
public:
    explicit ShortEpsDeficitChronologyProbe(bool full_residual) {
        initial_capital_ = 8561.92;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("margin_zero_cover_full_liquidation",
                             full_residual ? 1.0 : 0.0);
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/2.119);
        } else if (bar_index_ == 1) {
            // A resting take-profit limit on the H->L leg, i.e. strictly
            // after the adverse high on the O -> H -> L -> C path.
            strategy_exit("X", "S", /*limit=*/3821.06, /*stop=*/kNaN);
        }
    }
};

static void test_eps_deficit_chronology_slice_is_one_contract() {
    std::printf("test_eps_deficit_chronology_slice_is_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 3879.36, 3879.36, 3879.36, 3879.36, 1.0),
        mk_bar(2000, 3879.36, 3879.36, 3879.36, 3879.36, 1.0),
        mk_bar(3000, 3886.31, 3960.00, 3810.00, 3873.57, 1.0),
    };

    for (int full_residual = 0; full_residual <= 1; ++full_residual) {
        ShortEpsDeficitChronologyProbe eng(full_residual != 0);
        eng.run(bars.data(), (int)bars.size());

        // One contract at the adverse high, then the limit closes the
        // remainder. The opt-in used to liquidate the whole 2.119 here.
        CHECK(eng.trade_count() == 2);
        if (eng.trade_count() == 2) {
            CHECK(eng.exit_comment(0) == std::string("Margin call"));
            CHECK(near(eng.trade_size(0), 1.0, 1e-9));
            CHECK(near(eng.exit_price(0), 3960.0, 1e-9));
            CHECK(eng.exit_comment(1) != std::string("Margin call"));
            CHECK(near(eng.trade_size(1), 1.119, 1e-9));
            CHECK(near(eng.exit_price(1), 3821.06, 1e-9));
        }
        CHECK(near(eng.position_size(), 0.0, 1e-9));
    }
}

static void test_short_margin_call_zero_cover_without_qty_step_stays_continuous() {
    std::printf("test_short_margin_call_zero_cover_without_qty_step_stays_continuous\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 3788.00, 3788.00, 3788.00, 3788.00, 1.0),
        mk_bar(2000, 3788.00, 3788.48, 3766.62, 3775.78, 1.0),
    };

    ShortZeroCoverProbe eng(/*qty_step=*/0.0);
    eng.run(bars.data(), (int)bars.size());

    const double opened_qty = 100.384250 / 3788.00;
    const double equity_at_high = 100.384250
        - (3788.48 - 3788.00) * opened_qty;
    const double q_min = opened_qty - equity_at_high / 3788.48;
    const double expected_liquidation = 4.0 * q_min;

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), expected_liquidation, 1e-9));
    CHECK(eng.trade_size(0) < opened_qty);
    CHECK(near(eng.position_size(), -(opened_qty - expected_liquidation), 1e-9));
}

static void test_short_margin_call_nonzero_cover_keeps_four_x_nibble() {
    std::printf("test_short_margin_call_nonzero_cover_keeps_four_x_nibble\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),
        // q_min=0.15085... = 1.50 qty steps. Floor-before-4x must
        // therefore close 0.4, not the full 10-contract position.
        mk_bar(2000, 100.0, 100.76, 99.0, 100.0, 1.0),
    };

    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/0.1);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.exit_price(0), 100.76));
    CHECK(near(eng.trade_size(0), 0.4));
    CHECK(near(eng.position_size(), -9.6));
}

// Opening-affordability uses a separate, one-shot budget check. Its sub-lot
// shortfall reaches the same broker discontinuity as the finite-price cascade
// and is covered by closing one whole contract, independent of the opt-in
// zero-cover metadata.
class ShortOpeningDustProbe : public MCEngine {
public:
    bool saw_actionable_opening_event = false;

    ShortOpeningDustProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.015;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/10.0);
        } else if (bar_index_ == 1) {
            // The next-open fill precedes this callback. Prove this fixture
            // actually reaches the opening-affordability branch before its
            // one-shot event is consumed at bar end.
            saw_actionable_opening_event = opening_obligations_.pending()
                && opening_obligations_.actionable()
                && std::isfinite(opening_obligations_.raw_fill_base());
        }
    }
};

static void test_short_opening_affordability_zero_cover_closes_one_contract() {
    std::printf(
        "test_short_opening_affordability_zero_cover_closes_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 99.99, 99.99, 99.99, 99.99, 1.0),
        // Required margin is 999.90. The 0.015% opening fee leaves equity
        // 999.850015, so q_min=0.0004999... < the 1-contract step.
        mk_bar(2000, 99.99, 99.99, 99.99, 99.99, 1.0),
    };

    ShortOpeningDustProbe eng;
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.saw_actionable_opening_event);
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), -9.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

static void test_short_margin_call_account_fx() {
    std::printf("test_short_margin_call_account_fx\n");
    constexpr double account_fx = 2.0;
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 105.0,  99.5, 104.0, 1.0),
    };
    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/0.0, account_fx);
    eng.run(bars.data(), (int)bars.size());

    // FX-aware percent sizing opens qty=1000/(100*2)=5. At high=105:
    // equity=1000-(105-100)*5*2=950, margin=5*105*2=1050, so the
    // finite-price 4x restore is 4*(5-950/(105*2)).
    const double expected_qty = 4.0 * (5.0 - 950.0 / (105.0 * account_fx));
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(near(eng.trade_size(0), expected_qty));
    CHECK(near(eng.position_size(), -(5.0 - expected_qty)));
}

// ---- B: long at 100% margin is never liquidated ----------------------------

class LongNoLiqProbe : public MCEngine {
public:
    LongNoLiqProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;             // 1x -> denominator (1 - 1) = 0 -> na
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, kNaN);
    }
};

static void test_long_100pct_margin_no_call() {
    std::printf("test_long_100pct_margin_no_call\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0,  99.0, 100.0, 1.0),  // 0: long fills @100
        mk_bar(2000, 100.0, 100.0,  10.0,  20.0, 1.0),  // 1: -90% crash
        mk_bar(3000,  20.0,  21.0,   1.0,   2.0, 1.0),  // 2: keeps crashing
    };
    LongNoLiqProbe eng;
    eng.run(bars.data(), (int)bars.size());
    // A long at 100% margin can never be margin-called: position is held.
    CHECK(eng.trade_count() == 0);
    // The accessor must report na (no liquidation price exists).
    CHECK(std::isnan(eng.liq_price()));
}

// A default 100%-of-equity MARKET order placed and filled from true flat.
// A gap-up whose frozen-qty cost at the fill exceeds the sizing equity is
// REJECTED at fill and silently dropped (design-cntvxiao-gap-reject) — with
// or without a commission: the round-7 market-entry-admission pin (campaign
// notes log-20260905t071818z-e57e7235 / log-20260905t071819z-ece9b623, lab
// tv tapes scratchpad/r7/pins/macd1d-mktadmit-*, 0.1% commission, 206
// placements, 0 violations) shows TradingView tests floored_qty x tick(fill)
// <= equity with the fee EXCLUDED. A COMMISSIONED fill whose cost fits but
// whose cost + fee does not (the fee-only shortfall) keeps the KI-61
// fill-then-entry-bar affordability-trim path.
class FrozenAllInFlatLongProbe : public MCEngine {
public:
    explicit FrozenAllInFlatLongProbe(double commission_percent) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_percent;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, kNaN);
    }
};

// Zero commission + an above-lot gap-up: the frozen 10-lot notional at the
// 120 fill (1200) exceeds the 1000 sizing equity by more than one lot (one lot
// = qty_step*fill = 120), so TV REJECTS the entry at fill and it is silently
// dropped — the account stays FLAT, no trade row. (Pre-gap-reject the engine
// HELD the 10-lot fill exempt from the affordability trim; the rejection is
// design-cntvxiao-gap-reject.)
static void test_zero_cost_frozen_all_in_true_flat_gap_is_rejected() {
    std::printf("test_zero_cost_frozen_all_in_true_flat_gap_is_rejected\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    FrozenAllInFlatLongProbe eng(/*commission_percent=*/0.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 0.0));   // was 10 (held); now dropped
    CHECK(eng.position_size() == 0.0);
}

// Commission 10% + the same above-equity gap-up: signal sizing reserves the
// fee, floor(1000/1.1/100) = 9, and the 120 fill costs 9 x 120 = 1080 > 1000
// — the fee is not part of the test, so TV REJECTS the entry outright: no
// fill, no entry-bar margin call, FLAT. (Until the round-7 pin the engine
// filled 9 @ 120 and trimmed 4 on the entry bar — TV's z8830 bb-macd probes
// on NYSE:F@1D 2025-09-19 and OANDA:XAUUSD@1D 2025-07-14 show no such row.)
static void test_commissioned_frozen_all_in_true_flat_gap_is_rejected() {
    std::printf("test_commissioned_frozen_all_in_true_flat_gap_is_rejected\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    FrozenAllInFlatLongProbe eng(/*commission_percent=*/10.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 0.0));   // was 5 (9 filled, 4 trimmed)
}

// Commission 10% + a gap-up that fits WITHOUT the fee: 9 x 110 = 990 <= 1000
// admits, but 990 + the 99 opening fee is unaffordable, so the fill goes
// through and the KI-61 entry-bar affordability trim fires: restoring needs
// 9 - (1000 - 99)/110 = 0.809 lots, which floors to zero and takes the
// opening event's one-contract fallback — one lot closes on the entry bar
// at the fill, 8 are held. (The tapes' shape: NYSE:F 2025-07-29 896 @ 11.29
// vs equity 10125.50, OANDA:XAUUSD 2025-10-22 2.93 @ 4110.085 vs 12043.12.)
static void test_commissioned_frozen_all_in_true_flat_fee_only_shortfall_is_eligible() {
    std::printf("test_commissioned_frozen_all_in_true_flat_fee_only_shortfall_is_eligible\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 115.0,  80.0, 105.0, 1.0),
    };
    FrozenAllInFlatLongProbe eng(/*commission_percent=*/10.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(eng.entry_bar(0) == 1);
    CHECK(eng.exit_bar(0) == 1);
    CHECK(near(eng.entry_price(0), 110.0));
    CHECK(near(eng.exit_price(0), 110.0));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), 8.0));
}

// The short closes at zero PnL immediately before the long is placed, so both
// placement and fill observe FLAT. Direct same-on_bar close provenance (not a
// trade-count/PnL heuristic) must still identify the paired reentry; otherwise
// its next-open gap would be mistaken for the true-flat exemption.
class PairedCloseDefaultLongProbe : public MCEngine {
public:
    PairedCloseDefaultLongProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            // Close immediately first, so the reentry is placed from an
            // actually FLAT engine state. Its same-on_bar paired-close
            // provenance must still exclude it from the true-flat exemption.
            strategy_close("S", "paired close", kNaN, kNaN,
                           /*immediately=*/true);
            strategy_entry("L", true, kNaN, kNaN, kNaN);
        }
    }
};

static void test_paired_short_close_default_long_gap_remains_eligible() {
    std::printf("test_paired_short_close_default_long_gap_remains_eligible\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    PairedCloseDefaultLongProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(eng.entry_bar(1) == 2);
    CHECK(eng.exit_bar(1) == 2);
    CHECK(near(eng.entry_price(1), 120.0));
    CHECK(near(eng.exit_price(1), 120.0));
    CHECK(near(eng.trade_size(1), 4.0));
    CHECK(near(eng.position_size(), 6.0));
}

// ---- B'/C: 1x-long opening affordability is lot-floored and entry-priced ---

class LongOverAllocProbe : public MCEngine {
public:
    explicit LongOverAllocProbe(double qty_step,
                                double commission_percent = 0.0,
                                bool process_on_close = false,
                                int slippage_ticks = 0,
                                double mintick = 0.01,
                                double account_fx = 1.0,
                                double pointvalue = 1.0,
                                double qty = 10.0) : qty_(qty) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = qty;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_percent;
        margin_long_ = 100.0;             // 1x -> denominator (1 - 1) = 0 -> na
        process_orders_on_close_ = process_on_close;
        qty_step_ = qty_step;
        slippage_ = slippage_ticks;
        syminfo_mintick_ = mintick;
        account_currency_fx_ = account_fx;
        syminfo_.pointvalue = pointvalue;
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, qty_);
    }
private:
    double qty_;
};

// A commissioned, default-sized all-in long can remain margin-affordable
// before its opening fee while that fee alone creates a sub-step shortfall.
// TV's broker closes one whole contract for this exact true-flat MARKET shape
// when floor_step(q_min)==0.  These probes pin the rule, its full-position cap,
// the existing nonzero-floor 4x path, and two important scope exclusions.
class CommissionedDefaultPoeDustProbe : public MCEngine {
public:
    CommissionedDefaultPoeDustProbe(
            double initial_capital, double qty_step,
            CommissionType commission_type = CommissionType::PERCENT,
            double commission_value = 0.1,
            double default_percent = 100.0) {
        initial_capital_ = initial_capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = default_percent;
        commission_type_ = commission_type;
        commission_value_ = commission_value;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.0001;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, kNaN);
        }
    }
};

static void test_fee_created_floor_zero_closes_one_contract() {
    std::printf("test_fee_created_floor_zero_closes_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1801.33, 1801.33, 1801.33, 1801.33, 1.0),
        mk_bar(2000, 1801.34, 1801.34, 1801.34, 1801.34, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/10000.0, /*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    // Frozen qty is 5.5459. Margin alone retains positive headroom, but the
    // 0.1% opening fee creates raw q_min=0.00002307... < one step.
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 1801.34));
    CHECK(near(eng.exit_price(0), 1801.34));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), 4.5459));
}

static void test_fee_created_floor_zero_caps_sub_one_position() {
    std::printf("test_fee_created_floor_zero_caps_sub_one_position\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1500.0, 1500.0, 1500.0, 1500.0, 1.0),
        mk_bar(2000, 1500.01, 1500.01, 1500.01, 1500.01, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/1000.0, /*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.666));
    CHECK(near(eng.position_size(), 0.0));
}

static void test_fee_created_sub_half_cent_deficit_respects_fx_ledger() {
    std::printf("test_fee_created_sub_half_cent_deficit_respects_fx_ledger\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1500.0, 1500.0, 1500.0, 1500.0, 1.0),
        mk_bar(2000, 1500.005, 1500.005, 1500.005, 1500.005, 1.0),
    };
    // q=0.666 leaves enough lot-floor headroom that the adverse fill creates
    // only a $0.00233 post-fee deficit. A same-currency broker compares the
    // raw amounts and applies the one-contract fallback (capped to the full
    // sub-one position).
    CommissionedDefaultPoeDustProbe same_currency(
        /*initial_capital=*/1000.0, /*qty_step=*/0.0001);
    same_currency.run(bars.data(), (int)bars.size());
    CHECK(same_currency.trade_count() == 1);
    CHECK(near(same_currency.trade_size(0), 0.666));
    CHECK(near(same_currency.position_size(), 0.0));

    // A configured quote->account provider selects TV's converted cent ledger.
    // Both bars use rate 1 so conversion lifecycle, not rate magnitude, is the
    // sole factor. The sub-half-cent remainder stays affordable.
    CommissionedDefaultPoeDustProbe converted_currency(
        /*initial_capital=*/1000.0, /*qty_step=*/0.0001);
    const int64_t timestamps[] = {0};
    const double rates[] = {1.0};
    CHECK(converted_currency.set_account_currency_fx_series(
        timestamps, rates, 1));
    converted_currency.run(bars.data(), (int)bars.size());
    CHECK(converted_currency.trade_count() == 0);
    CHECK(near(converted_currency.position_size(), 0.666));
}

static void test_fee_created_nonzero_floor_keeps_four_x_quantity() {
    std::printf("test_fee_created_nonzero_floor_keeps_four_x_quantity\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1872.19, 1872.19, 1872.19, 1872.19, 1.0),
        mk_bar(2000, 1872.27, 1872.27, 1872.27, 1872.27, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/9949.545946, /*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    // raw q_min=0.00014707... floors to 0.0001 before the established 4x.
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 0.0004));
    CHECK(near(eng.position_size(), 5.3086));
}

static void test_fee_created_floor_zero_rejects_off_grid_one_contract() {
    std::printf("test_fee_created_floor_zero_rejects_off_grid_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1800.0, 1800.0, 1800.0, 1800.0, 1.0),
        mk_bar(2000, 1800.01, 1800.01, 1800.01, 1800.01, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/9009.02, /*qty_step=*/2.5);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 5.0));
}

// The floor-zero fallback is commission-MODEL independent: a fixed per-order
// fee that creates the same sub-step shortfall gets the same one contract.
static void test_cash_per_order_floor_zero_closes_one_contract() {
    std::printf("test_cash_per_order_floor_zero_closes_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1800.0, 1800.0, 1800.0, 1800.0, 1.0),
        mk_bar(2000, 1800.0, 1800.0, 1800.0, 1800.0, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/10000.0, /*qty_step=*/0.0001,
        CommissionType::CASH_PER_ORDER, /*commission_value=*/0.2);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), 4.5555));
}

// Repurposed from the KI-61 "sublot overage held" fixture (design-explicit-qty-
// fill-admission). This is an EXPLICIT-qty all-in true-flat MARKET entry
// (strategy.entry with qty=10 == equity/close) whose next-bar fill gaps
// ADVERSELY to 110: notional 10*110 = 1100 overshoots equity 1000. TV DECLINES
// it outright with ZERO slack — the frozen path's one-lot lot-floor slack does
// NOT apply to explicit qty — so the pre-fix "held 10 via lot-floor dust"
// outcome is dead: no fill, no rows, no margin call. Evidence:
// data/probes/pf-probe-allin-floor-comm0 (4,740 from-flat attempts; decline iff
// fill notional > equity, commission-independent, zero slack). The KI-61
// lot-floored opening-affordability trim these fixtures once exercised is still
// pinned by the frozen/default-sized path (test_commissioned_frozen_all_in_
// true_flat_fee_only_shortfall_is_eligible for the commissioned fee-only
// admit+trim; the frozen
// exemption tests for the sub-lot held case) plus test_explicit_qty_fill_
// admission's GREEN-D.
static void test_explicit_all_in_zero_comm_adverse_gap_declined() {
    std::printf("test_explicit_all_in_zero_comm_adverse_gap_declined\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: signal @ close 100
        mk_bar(2000, 110.0, 112.0,  50.0,  90.0, 1.0),  // 1: gap 110 -> 1100 > 1000 DECLINE
        mk_bar(3000,  90.0,  91.0,   1.0,   2.0, 1.0),  // 2: later crash: nothing held
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 10
    CHECK(std::isnan(eng.liq_price()));
}

// Repurposed from the KI-61 "lot trim uses entry affordability" fixture. An
// EXPLICIT-qty all-in true-flat MARKET entry (qty=10 == equity/close), commission
// 4%, fill gaps ADVERSELY to 120: the NOTIONAL 10*120 = 1200 alone overshoots
// equity 1000 (the fee is irrelevant to the predicate). Commission-scoping is
// DEAD (data/probes/pf-probe-allin-floor-comm0 is comm=0 and still declines), so
// TV DECLINES this too — the pre-fix "fill 10@120 then 4x entry-bar trim to hold
// 2" outcome is dead. No fill, no Margin-call rows. The commissioned admit+trim
// KI-61 semantics remain pinned by the FROZEN path
// (test_commissioned_frozen_all_in_true_flat_fee_only_shortfall_is_eligible).
static void test_explicit_all_in_commissioned_adverse_gap_declined() {
    std::printf("test_explicit_all_in_commissioned_adverse_gap_declined\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 122.0, 100.0, 110.0, 1.0),   // gap 120 -> 1200 > 1000 DECLINE
        mk_bar(3000, 110.0, 111.0,  10.0,  20.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0, /*commission_percent=*/4.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);            // pre-fix: 1 (fill + Margin-call trim)
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 2
    CHECK(std::isnan(eng.liq_price()));
}

// Repurposed from the KI-61 "trim without qty_step" fixture. EXPLICIT-qty all-in
// true-flat MARKET entry (qty=10 == equity/close), zero commission, qty_step=0
// (continuous mode), fill gaps ADVERSELY to 110: notional 1100 > equity 1000.
// This is exactly test_explicit_qty_fill_admission RED-1's class (zero comm,
// zero slack, adverse gap), so TV DECLINES — the pre-fix fractional entry-bar
// trim is dead. No fill, no rows.
static void test_explicit_all_in_zero_comm_no_qty_step_declined() {
    std::printf("test_explicit_all_in_zero_comm_no_qty_step_declined\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 112.0, 50.0,  90.0, 1.0),   // gap 110 -> 1100 > 1000 DECLINE
        mk_bar(3000,  90.0,  91.0,  1.0,   2.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/0.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);            // pre-fix: 1 (fill + fractional trim)
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 10 - trim
    CHECK(std::isnan(eng.liq_price()));
}

// Repurposed from the KI-61 "combines fx/pointvalue/commission" fixture. The
// explicit-qty fill-admission predicate carries the SAME pv/fx/margin factors as
// KI-61 (|qty|*slipped_fill*pv*fx*margin/100). Signal-time admission is exact
// (5*10*pv10*fx2 == 1000 == equity); the ADVERSE fill at 12 makes the notional
// 5*12*10*2 = 1200 > equity 1000, so TV DECLINES (commission 10% excluded from
// the predicate). Pins that the decline arithmetic combines pv, fx, and margin
// exactly like the KI-61 trim it replaces here. No fill, no rows.
static void test_explicit_all_in_fx_pointvalue_commission_declined() {
    std::printf("test_explicit_all_in_fx_pointvalue_commission_declined\n");
    std::vector<Bar> bars = {
        // Signal-time admission is exact: 5 * 10 * pv10 * fx2 == 1000.
        mk_bar(1000, 10.0, 10.0,  9.0, 10.0, 1.0),
        mk_bar(2000, 12.0, 13.0,  8.0, 11.0, 1.0),   // 5*12*10*2 = 1200 > 1000 DECLINE
        mk_bar(3000, 11.0, 12.0,  1.0,  2.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0, /*commission_percent=*/10.0,
                           /*process_on_close=*/false, /*slippage_ticks=*/0,
                           /*mintick=*/0.01, /*account_fx=*/2.0,
                           /*pointvalue=*/10.0, /*qty=*/5.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);            // pre-fix: 1 (fill + trim to hold 1)
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 1
    CHECK(std::isnan(eng.liq_price()));
}

static void test_long_100pct_margin_trim_process_orders_on_close() {
    std::printf("test_long_100pct_margin_trim_process_orders_on_close\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 101.0, 10.0,  20.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0, /*commission_percent=*/12.0,
                           /*process_on_close=*/true);
    eng.run(bars.data(), (int)bars.size());

    // Entry commission makes q_restore=1.2, floors to one and trims four on
    // bar0 itself. The generic "no adverse path after a close fill" rule must
    // not suppress this non-price affordability action.
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(eng.entry_bar(0) == 0);
    CHECK(eng.exit_bar(0) == 0);
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(near(eng.exit_price(0), 100.0));
    CHECK(near(eng.position_size(), 6.0));
}

class LongPricedOverAllocProbe : public MCEngine {
public:
    enum class Kind { Stop, Limit };

    explicit LongPricedOverAllocProbe(Kind kind) : kind_(kind) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
        slippage_ = 2;
        syminfo_mintick_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        if (kind_ == Kind::Stop) {
            strategy_entry("L", true, kNaN, /*stop=*/120.2, /*qty=*/10.0);
        } else {
            strategy_entry("L", true, /*limit=*/120.8, kNaN, /*qty=*/10.0);
        }
    }

private:
    Kind kind_;
};

static void test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip() {
    std::printf("test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 130.0, 90.0, 115.0, 1.0),
        mk_bar(3000, 115.0, 116.0, 10.0,  20.0, 1.0),
    };
    LongPricedOverAllocProbe eng(LongPricedOverAllocProbe::Kind::Stop);
    eng.run(bars.data(), (int)bars.size());

    // KI-62 STAGE 3 (margin fill-time admission): this buy-stop 120.2 is
    // OVER-ALLOCATED — qty 10 on a $1,000 account, and the margin gate costs it
    // at the FILL BAR'S OPEN (110): required 10*110*100% = 1100 > equity 1000 ->
    // DECLINE. The order never fills, so the old KI-61 1x-long dust-trim (which
    // used to report fill@123 then trim to size 4) does NOT fire — the
    // "declined fills must not fire the dust-trim" reconciliation.
    //
    // TV declines over-allocated stops too: cross-confirmed by
    // pf-probe-ki65-dual-entry-precedence, whose UQ=1,000,000 (>=1000x
    // over-notional) stop cells decline under the identical rule, lifting its
    // canonical TV match 93.8% -> 100.0%. The LIMIT sibling below is UNAFFECTED
    // (the gate is stop-entry-only) and still fills + trims.
    //
    // CAVEAT (register): the OVER-ALLOCATED FIXED-QTY class is UNPINNED by the
    // ki62 probe itself (which used all-in / marginal / fixed-small sizing). It
    // is a candidate future-probe cell; if any tier ever regresses tracing to a
    // strategy relying on the old admit-and-trim vs TV, this scopes back to
    // admit-then-nibble and the cell becomes a probe requirement.
    CHECK(eng.trade_count() == 0);            // declined at the fill-bar open
    CHECK(near(eng.position_size(), 0.0));    // nothing opened
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

static void test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip() {
    std::printf("test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 130.0, 131.0, 129.0, 130.0, 1.0),
        mk_bar(2000, 130.0, 134.0, 100.0, 110.0, 1.0),
        mk_bar(3000, 110.0, 111.0,  10.0,  20.0, 1.0),
    };
    LongPricedOverAllocProbe eng(LongPricedOverAllocProbe::Kind::Limit);
    eng.run(bars.data(), (int)bars.size());

    // buy limit 120.8 snaps favorably to entry 120 and receives no entry
    // slippage. The affordability trim is a broker market sell: raw matched
    // base 120.8 minus two ticks snaps down to 118.
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(near(eng.entry_price(0), 120.0));
    CHECK(near(eng.exit_price(0), 118.0));
    CHECK(eng.entry_bar(0) == 1);
    CHECK(eng.exit_bar(0) == 1);
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

class RawOpeningProbe : public MCEngine {
public:
    RawOpeningProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_order("RAW", true, /*qty=*/10.0);
    }
};

static void test_raw_order_fresh_open_captures_affordability() {
    std::printf("test_raw_order_fresh_open_captures_affordability\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    RawOpeningProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(near(eng.entry_price(0), 120.0));
    CHECK(near(eng.exit_price(0), 120.0));
    CHECK(near(eng.position_size(), 6.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

// ---- C': explicit opening-affordability lifecycle -------------------------

static int margin_call_rows(const MCEngine& eng) {
    int count = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) == std::string("Margin call")) ++count;
    }
    return count;
}

// Two explicit qty=2 market entries are each affordable on their own, but the
// accepted same-bar pyramid (qty=4) exceeds a 100%-margin account after the
// configured account-currency FX conversion and opening commissions. The
// resulting broker action is direction-symmetric: it restores margin from the
// raw matched fill, not from the short side's later adverse-price path.
class SameBarExplicitPairOpeningProbe : public MCEngine {
public:
    SameBarExplicitPairOpeningProbe(bool is_long, double account_fx,
                                    double initial_capital)
        : is_long_(is_long) {
        initial_capital_ = initial_capital;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        account_currency_fx_ = account_fx;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        // Both calls are placed from FLAT within one on_bar and fill at the
        // POOC close in sequence — the real process_orders_on_close flow.
        // (The fixture used to force a fill between the two placements; under
        // design-market-entry-affordability an ADD placed while already
        // holding the BASE is costed as held + add and declined at placement
        // — masayanfx — whereas the same-source-bar pair placed from flat is
        // admitted with "held" frozen at placement, then trimmed here.)
        strategy_entry("BASE", is_long_, kNaN, kNaN, /*qty=*/2.0);
        strategy_entry("ADD", is_long_, kNaN, kNaN, /*qty=*/2.0);
    }

private:
    bool is_long_;
};

static double expected_same_bar_pair_opening_liquidation(
        double initial_capital, double raw_fill, double account_fx) {
    constexpr double total_qty = 4.0;
    constexpr double cash_per_contract = 20.0;
    constexpr double qty_step = 0.0001;
    const double margin_per_unit = raw_fill * account_fx;
    const double opening_commission = total_qty * cash_per_contract;
    const double opening_equity = initial_capital - opening_commission;
    double q_min = total_qty - opening_equity / margin_per_unit;
    q_min = std::floor(q_min / qty_step) * qty_step;
    double qty_liq = 4.0 * q_min;
    qty_liq = std::floor(qty_liq / qty_step + 1e-6) * qty_step;
    return std::min(qty_liq, total_qty);
}

static void check_same_bar_explicit_pair_opening_trim(
        bool is_long, double account_fx, double initial_capital) {
    constexpr double raw_fill = 1741.23;
    constexpr double one_entry_qty = 2.0;
    constexpr double total_qty = 4.0;
    constexpr double entry_fee = 20.0;

    // The admission fork is cumulative, not per-order: each order fits, but
    // the accepted pair plus its account-native opening fees does not.
    CHECK(one_entry_qty * raw_fill * account_fx < initial_capital);
    CHECK(total_qty * raw_fill * account_fx + total_qty * entry_fee
          > initial_capital);

    SameBarExplicitPairOpeningProbe eng(is_long, account_fx, initial_capital);
    std::vector<Bar> bars = {
        mk_bar(1000, raw_fill, raw_fill, raw_fill, raw_fill, 1.0),
    };
    eng.run(bars.data(), (int)bars.size());

    const double expected_qty = expected_same_bar_pair_opening_liquidation(
        initial_capital, raw_fill, account_fx);
    double liquidated_qty = 0.0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        CHECK(eng.exit_comment(i) == std::string("Margin call"));
        CHECK(near(eng.entry_price(i), raw_fill));
        CHECK(near(eng.exit_price(i), raw_fill));
        liquidated_qty += eng.trade_size(i);
    }
    CHECK(margin_call_rows(eng) == 2);
    CHECK(near(liquidated_qty, expected_qty));
    CHECK(near(std::fabs(eng.position_size()), total_qty - expected_qty));
}

static void test_same_bar_explicit_pair_foreign_fx_direction_symmetry() {
    std::printf("test_same_bar_explicit_pair_foreign_fx_direction_symmetry\n");
    constexpr double account_fx = 88.0;
    constexpr double initial_capital = 500000.0;
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/true, account_fx, initial_capital);
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/false, account_fx, initial_capital);
}

static void test_same_bar_explicit_pair_fx1_direction_symmetry() {
    std::printf("test_same_bar_explicit_pair_fx1_direction_symmetry\n");
    constexpr double account_fx = 1.0;
    constexpr double initial_capital = 6000.0;
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/true, account_fx, initial_capital);
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/false, account_fx, initial_capital);
}

// The add would fill above the base short. RE-PIN (2026-09-03, design-market-
// entry-affordability): an add placed while already HOLDING the base is
// costed as the resulting position at the signal close — held 2 + add 2 =
// 4 * 110 = 440 > MTM 420 - (110-100)*2 = 400 — and DECLINED at placement
// (masayanfx NQ1 2025-07-30 20:15Z: TV drops an over-notional pyramiding
// add). The base short stands (2 * 110 = 220 <= 400) and no margin call
// fires. (This fixture used to assert admit-then-4x-trim marked at the latest
// raw fill; that shape was never TV-pinned.)
class UnequalFillShortAddProbe : public MCEngine {
public:
    UnequalFillShortAddProbe() {
        initial_capital_ = 420.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("BASE", false, kNaN, kNaN, /*qty=*/2.0);
        } else if (bar_index_ == 1) {
            strategy_entry("ADD", false, kNaN, kNaN, /*qty=*/2.0);
        }
    }
};

static void test_short_add_opening_margin_marks_latest_raw_fill() {
    std::printf("test_short_add_opening_margin_marks_latest_raw_fill\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 110.0, 110.0, 110.0, 1.0),
    };
    UnequalFillShortAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    // At the add's signal close, MTM equity is 420 - (110-100)*2 = 400 and
    // the resulting position would need 4*110 = 440: the add is declined,
    // the base short (2 * 110 = 220) stands, no margin call.
    CHECK(eng.trade_count() == 0);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.position_size(), -2.0));
}

// Literal non-POOC geometry from the Thula margin fork. The effective fixed FX
// is deliberately inside the observed interval but remains an ordinary runtime
// input; the expected broker rows are pinned directly, not computed by a copy
// of the implementation formula.
class NextOpenExplicitShortPairProbe : public MCEngine {
public:
    NextOpenExplicitShortPairProbe() {
        // Prior realized loss leaves 497641.70 before these fills; four
        // account-native 20-per-contract opening fees make the broker's
        // opening-equity basis exactly 497561.70.
        initial_capital_ = 497641.70;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        account_currency_fx_ = 85.3567;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("BASE", false, kNaN, kNaN, /*qty=*/2.0);
        strategy_entry("ADD", false, kNaN, kNaN, /*qty=*/2.0);
    }
};

static void test_thula_next_open_short_pair_exact_margin_rows() {
    std::printf("test_thula_next_open_short_pair_exact_margin_rows\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1700.0, 1700.0, 1700.0, 1700.0, 1.0),
        mk_bar(2000, 1741.23, 1741.23, 1741.23, 1741.23, 1.0),
    };
    NextOpenExplicitShortPairProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 2);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 1741.23));
    CHECK(near(eng.entry_price(1), 1741.23));
    CHECK(near(eng.exit_price(0), 1741.23));
    CHECK(near(eng.exit_price(1), 1741.23));
    CHECK(near(eng.trade_size(0), 2.0));
    CHECK(near(eng.trade_size(1), 0.6088));
    CHECK(near(eng.position_size(), -1.3912));
}

class ShortOpeningEventScopeProbe : public MCEngine {
public:
    enum class Shape { DefaultPercent, DefaultCash, Priced, Raw, MarginNot100 };
    bool widened_event = false;
    bool priced_fill_observed = false;

    explicit ShortOpeningEventScopeProbe(Shape shape) : shape_(shape) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = shape == Shape::MarginNot100 ? 80.0 : 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        if (shape == Shape::DefaultPercent) {
            default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
            default_qty_value_ = 50.0;
        } else if (shape == Shape::DefaultCash) {
            default_qty_type_ = QtyType::CASH;
            default_qty_value_ = 200.0;
        }
    }

    void on_source_bar(const Bar& /*bar*/) override {
        // The priced control must be a real fill, not merely a pending shape.
        // Arm it below the market on bar 0, then observe its short fill after
        // bar 1 gaps to the limit. dispatch_bar's step 1 applies that resting
        // order before this callback, while its event provenance is visible.
        if (shape_ == Shape::Priced) {
            if (bar_index_ == 0) {
                strategy_entry("S", false, /*limit=*/110.0, kNaN,
                               /*qty=*/2.0);
            } else if (bar_index_ == 1) {
                priced_fill_observed =
                    position_side_ == PositionSide::SHORT
                    && near(position_qty_, 2.0)
                    && !pyramid_entries_.empty()
                    && near(pyramid_entries_.back().price, 110.0);
                widened_event = opening_obligations_.pending()
                    || opening_obligations_.actionable()
                    || std::isfinite(opening_obligations_.raw_fill_base());
            }
            return;
        }

        if (bar_index_ != 0) return;
        switch (shape_) {
            case Shape::DefaultPercent:
            case Shape::DefaultCash:
                strategy_entry("S", false, kNaN, kNaN, kNaN);
                break;
            case Shape::Priced:
                break;  // handled above on two distinct bars
            case Shape::Raw:
                strategy_order("S", false, /*qty=*/2.0);
                break;
            case Shape::MarginNot100:
                strategy_entry("S", false, kNaN, kNaN, /*qty=*/2.0);
                break;
        }
        process_pending_orders(current_bar_);
        widened_event = opening_obligations_.pending()
            || opening_obligations_.actionable()
            || std::isfinite(opening_obligations_.raw_fill_base());
    }

private:
    Shape shape_;
};

static void test_short_opening_event_scope_is_explicit_market_margin100_only() {
    std::printf("test_short_opening_event_scope_is_explicit_market_margin100_only\n");
    const ShortOpeningEventScopeProbe::Shape shapes[] = {
        ShortOpeningEventScopeProbe::Shape::DefaultPercent,
        ShortOpeningEventScopeProbe::Shape::DefaultCash,
        ShortOpeningEventScopeProbe::Shape::Priced,
        ShortOpeningEventScopeProbe::Shape::Raw,
        ShortOpeningEventScopeProbe::Shape::MarginNot100,
    };
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 110.0, 110.0, 110.0, 1.0),
    };
    for (auto shape : shapes) {
        ShortOpeningEventScopeProbe eng(shape);
        eng.run(bars.data(), (int)bars.size());
        if (shape == ShortOpeningEventScopeProbe::Shape::Priced) {
            CHECK(eng.priced_fill_observed);
        }
        CHECK(!eng.widened_event);
    }
}

// Literal first reversal from a source-bound TV tape. The script closes Long
// and then emits an omitted-qty Short in the same evaluation. Paid entry fees
// reduce broker equity, and the close-then-short fill receives both the
// fill-price affordability checkpoint and one bounded adverse-high retry.
class CommissionedDefaultShortCheckpointProbe : public MCEngine {
public:
    CommissionedDefaultShortCheckpointProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
        bar_magnifier_enabled_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            strategy_close("Long");
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 2) {
            captured_short_event = opening_obligations_.pending()
                && opening_obligations_.actionable()
                && opening_obligations_.requires_adverse_pass()
                && opening_owner_matches_position()
                && near(opening_obligations_.raw_fill_base(), 1798.09);
        }
    }

    bool captured_short_event = false;
};

struct DefaultShortCheckpointResult {
    int margin_rows = 0;
    int trade_rows = 0;
    double position = 0.0;
    bool captured = false;
    std::vector<double> margin_qty;
    std::vector<double> margin_exit;
};

static DefaultShortCheckpointResult run_commissioned_default_short_checkpoint() {
    std::vector<Bar> bars = {
        mk_bar(1000, 1801.48, 1801.48, 1801.48, 1801.48, 1.0),
        mk_bar(2000, 1801.48, 1801.48, 1798.09, 1798.09, 1.0),
        mk_bar(3000, 1798.09, 1806.33, 1798.09, 1804.62, 1.0),
    };
    CommissionedDefaultShortCheckpointProbe eng;
    eng.run(bars.data(), static_cast<int>(bars.size()));

    DefaultShortCheckpointResult result;
    result.margin_rows = margin_call_rows(eng);
    result.trade_rows = eng.trade_count();
    result.position = eng.position_size();
    result.captured = eng.captured_short_event;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) != std::string("Margin call")) continue;
        result.margin_qty.push_back(eng.trade_size(i));
        result.margin_exit.push_back(eng.exit_price(i));
    }
    return result;
}

static void test_commissioned_close_then_short_exact_checkpoints() {
    std::printf(
        "test_commissioned_close_then_short_exact_checkpoints\n");
    const DefaultShortCheckpointResult result =
        run_commissioned_default_short_checkpoint();

    CHECK(result.captured);
    CHECK(result.margin_rows == 2);
    CHECK(result.margin_qty.size() == 2);
    CHECK(near(result.margin_qty[0], 0.0108, 1e-9));
    CHECK(near(result.margin_exit[0], 1798.09, 1e-9));
    CHECK(near(result.margin_qty[1], 0.1696, 1e-9));
    CHECK(near(result.margin_exit[1], 1806.33, 1e-9));
    CHECK(near(result.position, -5.3650, 1e-9));
}

// Mirror a separate close-then-entry order pair on the LONG side. A
// commissioned omitted-qty LONG created while SHORT after the close command
// retains the opening-affordability provenance needed by the broker trim.
class CommissionedCloseThenLongProbe : public MCEngine {
public:
    explicit CommissionedCloseThenLongProbe(bool explicit_qty)
        : explicit_qty_(explicit_qty) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            strategy_close("Short");
            const double qty = explicit_qty_
                ? frozen_default_market_qty(/*is_buy=*/true) : kNaN;
            strategy_entry("Long", true, kNaN, kNaN, qty);
        } else if (bar_index_ == 2) {
            captured = opening_obligations_.pending()
                && opening_obligations_.actionable()
                && opening_owner_matches_position()
                && position_side_ == PositionSide::LONG;
        }
    }

    bool captured = false;

private:
    bool explicit_qty_;
};

// Omitted and explicit quantities both create a live opening check and reach
// the same floor-zero discontinuity. No obsolete commissioned-shape tag is
// needed to distinguish their identical one-contract liquidation outcome.
static void test_commissioned_close_then_long_floor_zero_scope() {
    std::printf("test_commissioned_close_then_long_floor_zero_scope\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 2968.50, 2968.50, 2968.50, 2968.50, 1.0),
        mk_bar(2000, 2968.50, 2968.50, 2968.50, 2968.50, 1.0),
        mk_bar(3000, 2967.80, 2967.80, 2967.80, 2967.80, 1.0),
    };
    CommissionedCloseThenLongProbe omitted(/*explicit_qty=*/false);
    omitted.run(bars.data(), static_cast<int>(bars.size()));
    CommissionedCloseThenLongProbe explicit_control(/*explicit_qty=*/true);
    explicit_control.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(omitted.captured);
    CHECK(explicit_control.captured);
    CHECK(omitted.trade_count() == 2);  // short close + long margin trim
    CHECK(margin_call_rows(omitted) == 1);
    CHECK(omitted.exit_comment(1) == std::string("Margin call"));
    CHECK(near(omitted.entry_price(1), 2967.80));
    CHECK(near(omitted.exit_price(1), 2967.80));
    CHECK(near(omitted.trade_size(1), 1.0, 1e-9));
    // Same discontinuity and lot under the independently sized explicit call.
    CHECK(explicit_control.trade_count() == 2);
    CHECK(margin_call_rows(explicit_control) == 1);
    CHECK(near(explicit_control.trade_size(1), 1.0, 1e-9));
}

// After the close-then-short fill-price trim, its bounded ordinary adverse
// retry can require a positive restore quantity smaller than one configured
// lot. TV's source-bound tape closes one whole contract at that exact
// discontinuity. The former tagged/untagged arms below are retained as
// identical-input repeat controls after removal of the unused lifecycle bit.
class DefaultShortLaterFloorZeroProbe : public MCEngine {
public:
    explicit DefaultShortLaterFloorZeroProbe(bool full_residual = false) {
        initial_capital_ = 10000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata(
            "margin_zero_cover_full_liquidation",
            full_residual ? 1.0 : 0.0);

        constexpr double qty = 3.6930;
        constexpr double entry = 1799.94;
        constexpr double adverse = 1801.26;
        constexpr double raw_q_min = 0.00005;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        net_profit_sum_ =
            (qty - raw_q_min) * adverse - initial_capital_
            + (adverse - entry) * qty;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(
            2000, 1800.00, 1801.26, 1799.50, 1800.50, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }

    bool has_live_short_position() const {
        return position_side_ == PositionSide::SHORT
            && position_cycle_seq_ != 0 && position_qty_ > 0.0;
    }
};

static void test_default_short_lifecycle_floor_zero_one_contract() {
    std::printf("test_default_short_lifecycle_floor_zero_one_contract\n");
    DefaultShortLaterFloorZeroProbe top_level;
    top_level.trigger();
    DefaultShortLaterFloorZeroProbe one_contract;
    one_contract.trigger();
    DefaultShortLaterFloorZeroProbe full_residual(/*full_residual=*/true);
    full_residual.trigger();

    // Retain both former provenance arms as same-economics repeat controls.
    // The fallback is not conditioned on an entry-lifecycle label.
    CHECK(top_level.trade_count() == 1);
    CHECK(near(top_level.trade_size(0), 1.0, 1e-9));
    CHECK(near(top_level.position_size(), -2.6930, 1e-9));

    CHECK(one_contract.trade_count() == 1);
    CHECK(one_contract.exit_comment(0) == std::string("Margin call"));
    CHECK(near(one_contract.exit_price(0), 1801.26));
    CHECK(near(one_contract.trade_size(0), 1.0, 1e-9));
    CHECK(near(one_contract.position_size(), -2.6930, 1e-9));
    CHECK(one_contract.has_live_short_position());
    CHECK(!one_contract.opening_pending());

    // The opt-in whole-residual interpretation no longer overrides the
    // settled floor-zero slice: when the one-contract fallback is
    // expressible, a verifier combining both candidates gets the SAME one
    // whole contract and HOLDS the remainder (TV never prints a full
    // liquidation at these eps-scale deficits — finding 279, serhan ADX).
    CHECK(full_residual.trade_count() == 1);
    CHECK(near(full_residual.exit_price(0), 1801.26));
    CHECK(near(full_residual.trade_size(0), 1.0, 1e-9));
    CHECK(near(full_residual.position_size(), -2.6930, 1e-9));
    CHECK(full_residual.has_live_short_position());
    CHECK(!full_residual.opening_pending());
}

// A positive opening restore below one lot closes one contract at the raw
// fill base. The old lifecycle-tag variants are identical economic controls;
// preserve both executions without seeding an unused Boolean.
class DefaultShortOpeningFloorZeroProbe : public MCEngine {
public:
    DefaultShortOpeningFloorZeroProbe() {
        initial_capital_ = 1000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 10.0;
        constexpr double entry = 100.0;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        net_profit_sum_ = 0.495;  // entry fee 0.5 => q_min = 0.00005
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
        seed_opening_check(entry,
            broker::OpeningContinuation::RemainingAdversePath);
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }
};

static void test_default_short_opening_floor_zero_one_contract() {
    std::printf("test_default_short_opening_floor_zero_one_contract\n");
    DefaultShortOpeningFloorZeroProbe baseline;
    baseline.trigger();
    DefaultShortOpeningFloorZeroProbe repeated;
    repeated.trigger();

    // Fee-net equity is 1000 + .495 - .5 = 999.995, so the positive 0.00005
    // restore amount floors below one 0.0001 lot. The opening checkpoint acts
    // on it identically in the two preserved repeat controls.
    CHECK(baseline.trade_count() == 1);
    CHECK(baseline.exit_comment(0) == std::string("Margin call"));
    CHECK(near(baseline.trade_size(0), 1.0, 1e-9));
    CHECK(near(baseline.position_size(), -9.0, 1e-9));
    CHECK(repeated.trade_count() == 1);
    CHECK(repeated.exit_comment(0) == std::string("Margin call"));
    CHECK(near(repeated.entry_price(0), 100.0));
    CHECK(near(repeated.exit_price(0), 100.0));
    CHECK(near(repeated.trade_size(0), 1.0, 1e-9));
    CHECK(near(repeated.position_size(), -9.0, 1e-9));
    CHECK(!baseline.opening_pending());
    CHECK(!repeated.opening_pending());
}

// A close-then-short fill-price opening check can be affordable while the same
// bar's high is
// already adverse enough to require an ordinary margin call. The opening event
// must schedule that second checkpoint even though it emitted no trade, and it
// must be consumed before recursion so the retry occurs exactly once.
class DefaultShortAffordableOpeningAdverseProbe : public MCEngine {
public:
    DefaultShortAffordableOpeningAdverseProbe() {
        initial_capital_ = 1000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 9.99;
        constexpr double entry = 100.0;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
        seed_opening_check(entry,
            broker::OpeningContinuation::RemainingAdversePath);
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(2000, 100.0, 105.0, 99.0, 100.0, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
        event_cleared = !opening_obligations_.pending()
            && !opening_obligations_.actionable()
            && !opening_obligations_.requires_adverse_pass()
            && std::isnan(opening_obligations_.raw_fill_base());
    }

    bool event_cleared = false;
};

static void test_default_short_affordable_opening_retries_adverse_once() {
    std::printf(
        "test_default_short_affordable_opening_retries_adverse_once\n");
    DefaultShortAffordableOpeningAdverseProbe probe;
    probe.trigger();

    CHECK(probe.trade_count() == 1);
    CHECK(probe.exit_comment(0) == std::string("Margin call"));
    CHECK(near(probe.entry_price(0), 100.0));
    CHECK(near(probe.exit_price(0), 105.0));
    // TV's adverse-margin ledger debits the surviving opening commission:
    // equity = 1000 + (100 - 105) * 9.99 - .4995 = 949.5505;
    // q_min = 9.99 - 949.5505 / 105 = 0.9466619..., which floors to
    // 0.9466 before TV's 4x liquidation multiplier.
    CHECK(near(probe.trade_size(0), 3.7864, 1e-9));
    CHECK(near(probe.position_size(), -6.2036, 1e-9));
    CHECK(probe.event_cleared);
}

// True-flat default shorts queue either a check (paid commission) or an
// exemption (zero commission). Both receipts belong to the actual position,
// and neither requires the direct-reversal adverse continuation.
class DefaultFlatShortOpeningDecisionProbe : public MCEngine {
public:
    explicit DefaultFlatShortOpeningDecisionProbe(bool commissioned) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commissioned ? 0.05 : 0.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            captured_pending = position_side_ == PositionSide::SHORT
                && opening_obligations_.pending()
                && opening_owner_matches_position();
            captured = captured_pending && opening_obligations_.actionable();
            captured_adverse = opening_obligations_.requires_adverse_pass();
        }
    }

    bool captured = false;
    bool captured_pending = false;
    bool captured_adverse = false;
};

static void test_default_flat_short_opening_decision_tracks_commission() {
    std::printf("test_default_flat_short_opening_decision_tracks_commission\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    DefaultFlatShortOpeningDecisionProbe uncommissioned(
        /*commissioned=*/false);
    uncommissioned.run(bars.data(), static_cast<int>(bars.size()));
    DefaultFlatShortOpeningDecisionProbe commissioned(
        /*commissioned=*/true);
    commissioned.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(!uncommissioned.captured);
    CHECK(commissioned.captured);
    CHECK(uncommissioned.captured_pending);
    CHECK(commissioned.captured_pending);
    CHECK(!uncommissioned.captured_adverse);
    CHECK(!commissioned.captured_adverse);
    CHECK(uncommissioned.trade_count() == 0);
    CHECK(commissioned.trade_count() == 0);
    CHECK(uncommissioned.position_size() < -1e-9);
    CHECK(commissioned.position_size() < -1e-9);
}

// After a prior LONG has already been fully liquidated, the next default SHORT
// is a true-flat open rather than a close-then-open reversal. Its adverse
// restore amount is positive but below one lot; TV caps the one-contract
// fallback to the entire 0.3383-contract residual.
class DefaultFlatShortFloorZeroProbe : public MCEngine {
public:
    DefaultFlatShortFloorZeroProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 0.3383;
        constexpr double entry = 3734.88;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        net_profit_sum_ = -8735.542085;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "Short", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["Short"] = qty;
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(
            2000, 3734.88, 3735.52, 3734.00, 3735.00, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }
};

static void test_default_flat_short_floor_zero_caps_to_residual() {
    std::printf("test_default_flat_short_floor_zero_caps_to_residual\n");
    DefaultFlatShortFloorZeroProbe baseline;
    baseline.trigger();
    DefaultFlatShortFloorZeroProbe repeated;
    repeated.trigger();

    // 0.3383 contracts is below one, so the min(1.0, qty) cap closes the whole
    // residual in both former lifecycle-tag arms, now repeat controls.
    CHECK(baseline.trade_count() == 1);
    CHECK(near(baseline.exit_price(0), 3735.52));
    CHECK(near(baseline.trade_size(0), 0.3383, 1e-9));
    CHECK(near(baseline.position_size(), 0.0, 1e-9));

    CHECK(repeated.trade_count() == 1);
    CHECK(repeated.exit_comment(0) == std::string("Margin call"));
    CHECK(near(repeated.entry_price(0), 3734.88));
    CHECK(near(repeated.exit_price(0), 3735.52));
    CHECK(near(repeated.trade_size(0), 0.3383, 1e-9));
    CHECK(near(repeated.position_size(), 0.0, 1e-9));
}

// A script partial and a later margin partial preserve the physical short's
// position identity while changing its quantity. The later floor-zero rule
// does not depend on the removed commissioned-lifecycle label.
class CommissionedDefaultShortPartialOwnerProbe : public MCEngine {
public:
    CommissionedDefaultShortPartialOwnerProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            opening_after_open = opening_obligations_.actionable()
                && opening_owner_matches_position();
            original_cycle_ = position_cycle_seq_;
            original_incarnation_ = pyramid_entries_.empty()
                ? 0 : pyramid_entries_.front().entry_incarnation;
            strategy_close(
                "Short", "partial lifecycle close", /*qty=*/1.0,
                /*qty_percent=*/kNaN, /*immediately=*/true);
            qty_after_partial = position_qty_;
            owner_after_partial =
                position_side_ == PositionSide::SHORT
                && position_qty_ > 1.0
                && position_cycle_seq_ == original_cycle_
                && pyramid_entries_.size() == 1
                && pyramid_entries_.front().entry_incarnation == original_incarnation_;
        }
    }

    void trigger_later_floor_zero() {
        constexpr double adverse = 105.0;
        constexpr double raw_q_min = 0.00005;
        const double open_fee = surviving_open_percent_commission_account();
        net_profit_sum_ =
            (position_qty_ - raw_q_min) * adverse - initial_capital_
            + open_fee
            + (adverse - position_entry_price_) * position_qty_;
        current_bar_ = mk_bar(
            3000, 100.0, adverse, 99.0, 100.0, 1.0);
        bar_index_ = 2;
        process_margin_call(current_bar_);
        owner_after_margin_partial = position_side_ == PositionSide::SHORT
            && position_cycle_seq_ == original_cycle_
            && pyramid_entries_.size() == 1
            && pyramid_entries_.front().entry_incarnation == original_incarnation_;
        opening_consumed = !opening_obligations_.pending();
    }

    bool opening_after_open = false;
    bool owner_after_partial = false;
    bool owner_after_margin_partial = false;
    bool opening_consumed = false;
    double qty_after_partial = 0.0;

private:
    int64_t original_cycle_ = 0;
    uint64_t original_incarnation_ = 0;
};

static void test_short_partials_preserve_position_owner() {
    std::printf(
        "test_short_partials_preserve_position_owner\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    CommissionedDefaultShortPartialOwnerProbe probe;
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.opening_after_open);
    CHECK(probe.owner_after_partial);
    CHECK(probe.trade_count() == 1);
    CHECK(near(probe.trade_size(0), 1.0, 1e-9));
    const double qty_before_margin = probe.qty_after_partial;

    probe.trigger_later_floor_zero();
    CHECK(probe.trade_count() == 2);
    CHECK(probe.exit_comment(1) == std::string("Margin call"));
    CHECK(near(probe.exit_price(1), 105.0));
    // The unchanged lot fallback depends on the actual budget, not a label.
    CHECK(near(probe.trade_size(1), 1.0, 1e-9));
    CHECK(near(probe.position_size(), -(qty_before_margin - 1.0), 1e-9));
    CHECK(probe.owner_after_margin_partial);
    CHECK(probe.opening_consumed);
}

// A genuine add replaces the opening obligation with its own committed-fill
// receipt in the same position cycle. A full close invalidates the obligation.
class CommissionedDefaultShortOpeningMutationProbe : public MCEngine {
public:
    enum class Mutation { AcceptedAdd, FullClose };

    explicit CommissionedDefaultShortOpeningMutationProbe(Mutation mutation)
        : mutation_(mutation) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            opening_after_open = opening_obligations_.actionable()
                && opening_owner_matches_position();
            original_cycle_ = position_cycle_seq_;
            if (opening_obligations_.peek()) {
                original_fill_ = opening_obligations_.peek()->owner().producerFill;
            }
            if (mutation_ == Mutation::AcceptedAdd) {
                strategy_entry("Add", false, kNaN, kNaN, /*qty=*/1.0);
            } else {
                strategy_close(
                    "Short", "full lifecycle close", /*qty=*/kNaN,
                    /*qty_percent=*/kNaN, /*immediately=*/true);
                full_close_cleared = position_side_ == PositionSide::FLAT
                    && position_cycle_seq_ == 0
                    && !opening_obligations_.pending();
            }
        } else if (bar_index_ == 2
                   && mutation_ == Mutation::AcceptedAdd) {
            add_filled = position_side_ == PositionSide::SHORT
                && position_entry_count_ == 2;
            accepted_add_replaced = add_filled
                && position_cycle_seq_ == original_cycle_
                && opening_obligations_.actionable()
                && opening_owner_matches_position()
                && opening_obligations_.peek()->owner().producerFill > original_fill_
                && opening_obligations_.peek()->owner().orderIncarnation
                    == pyramid_entries_.back().entry_incarnation;
        }
    }

    bool opening_after_open = false;
    bool add_filled = false;
    bool accepted_add_replaced = false;
    bool full_close_cleared = false;

private:
    Mutation mutation_;
    int64_t original_cycle_ = 0;
    uint64_t original_fill_ = 0;
};

static void test_short_add_replaces_obligation_and_full_close_invalidates() {
    std::printf("test_short_add_replaces_obligation_and_full_close_invalidates\n");
    // Bar 1 closes at 90 so the explicit 1-lot add is affordable as held + add
    // (design-market-entry-affordability): the all-in short is in profit,
    // MTM ~1,099 >= (9.99 + 1) * 90. At the former close of 100 the all-in
    // position had no free equity and the add was (correctly) dropped.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0,  90.0,  90.0, 1.0),
        mk_bar(3000,  90.0,  90.0,  90.0,  90.0, 1.0),
    };
    CommissionedDefaultShortOpeningMutationProbe add(
        CommissionedDefaultShortOpeningMutationProbe::Mutation::AcceptedAdd);
    add.run(bars.data(), static_cast<int>(bars.size()));
    CommissionedDefaultShortOpeningMutationProbe close(
        CommissionedDefaultShortOpeningMutationProbe::Mutation::FullClose);
    close.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(add.opening_after_open);
    CHECK(add.add_filled);
    CHECK(add.accepted_add_replaced);
    CHECK(close.opening_after_open);
    CHECK(close.full_close_cleared);
}

// A scoped explicit MARKET short event is only provenance for that exact
// fill. If a later successful same-direction short fill in the same dispatch
// cycle has a non-scoped shape, the earlier event must not survive to the
// end-of-bar margin pass. These mutations use a later synthetic broker sample
// so BASE fills at 100 and the accepted add really fills at 110.
class ShortOpeningEventMutationProbe : public MCEngine {
public:
    enum class LaterFill { PricedEntry, RawOrder };
    bool base_event_captured = false;
    bool later_add_filled = false;
    bool stale_event_cleared = false;

    explicit ShortOpeningEventMutationProbe(LaterFill later_fill)
        : later_fill_(later_fill) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        strategy_entry("BASE", false, kNaN, kNaN, /*qty=*/2.0);
        process_pending_orders(current_bar_);
        base_event_captured = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0);

        if (later_fill_ == LaterFill::PricedEntry) {
            strategy_entry("ADD", false, /*limit=*/110.0, kNaN,
                           /*qty=*/2.0);
        } else {
            strategy_order("ADD", false, /*qty=*/2.0);
        }

        const Bar later_sample =
            mk_bar(current_bar_.timestamp, 110.0, 110.0, 110.0, 110.0, 1.0);
        process_pending_orders(later_sample);
        later_add_filled = position_side_ == PositionSide::SHORT
            && near(position_qty_, 4.0)
            && pyramid_entries_.size() == 2
            && near(pyramid_entries_.back().price, 110.0);
        stale_event_cleared = !opening_obligations_.pending()
            && !opening_obligations_.actionable()
            && std::isnan(opening_obligations_.raw_fill_base());
    }

private:
    LaterFill later_fill_;
};

static void test_priced_short_add_invalidates_scoped_opening_event() {
    std::printf("test_priced_short_add_invalidates_scoped_opening_event\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ShortOpeningEventMutationProbe eng(
        ShortOpeningEventMutationProbe::LaterFill::PricedEntry);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.base_event_captured);
    CHECK(eng.later_add_filled);
    CHECK(eng.stale_event_cleared);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.position_size(), -4.0));
}

static void test_raw_short_add_invalidates_scoped_opening_event() {
    std::printf("test_raw_short_add_invalidates_scoped_opening_event\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ShortOpeningEventMutationProbe eng(
        ShortOpeningEventMutationProbe::LaterFill::RawOrder);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.base_event_captured);
    CHECK(eng.later_add_filled);
    CHECK(eng.stale_event_cleared);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.position_size(), -4.0));
}

// A genuine accepted same-direction add is itself a post-fill affordability
// event. FIFO then drains the original lot, leaving a single surviving pyramid
// leg; the event must survive because it came from the accepted add directly,
// not from reconstructing provenance from the remaining count or leg census.
class AcceptedAddFifoProbe : public MCEngine {
public:
    bool captured_after_open = false;
    bool eligible_after_add = false;
    bool eligible_after_fifo = false;
    int count_after_fifo = -1;
    int legs_after_fifo = -1;

    AcceptedAddFifoProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        captured_after_open = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0);

        // A priced explicit entry bypasses the market-only signal admission
        // gate and is a genuine accepted append (10 -> 25), not a rejected
        // over-allocation attempt. It is immediately marketable at this close.
        strategy_entry("ADD", true, /*limit=*/100.0, kNaN, /*qty=*/15.0);
        process_pending_orders(current_bar_);
        eligible_after_add = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0);

        // FIFO removes the opening lot, leaving only ADD as a live pyramid
        // leg. This drain is a CLOSE-PATH retirement (strategy.close), so TV
        // hands the pyramid slot back and position_entry_count_ falls to one
        // (a strategy.exit bracket drain would NOT release it — finding-348).
        // The add event's liveness must not depend on either reading.
        strategy_close("OPEN", "fifo drain", /*qty=*/10.0,
                       /*qty_percent=*/kNaN, /*immediately=*/true);
        count_after_fifo = position_entry_count_;
        legs_after_fifo = (int)pyramid_entries_.size();
        eligible_after_fifo = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0);
    }
};

static void test_accepted_add_fifo_keeps_add_affordability_event() {
    std::printf("test_accepted_add_fifo_keeps_add_affordability_event\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    AcceptedAddFifoProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.captured_after_open);
    CHECK(eng.eligible_after_add);
    // The close-path drain leaves ONE live pyramid leg AND returns the pyramid
    // slot, so both readings are one. Neither is a usable provenance source
    // for the affordability event — that is what this probe pins.
    CHECK(eng.legs_after_fifo == 1);
    CHECK(eng.count_after_fifo == 1);  // cannot reconstruct from this count
    CHECK(eng.eligible_after_fifo);
    // The one-shot event is consumed at the end-of-bar margin pass.
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.trade_count() == 2);  // explicit FIFO close + margin call
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.trade_size(1), 15.0));
    CHECK(near(eng.position_size(), 0.0));
}

// A rejected same-direction attempt must not erase the fresh opening's state.
// Commission then makes the opening itself genuinely unaffordable, proving the
// preserved state remains actionable in the end-of-bar check.
class RejectedAddProbe : public MCEngine {
public:
    bool preserved_after_rejection = false;

    RejectedAddProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 1;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        strategy_entry("REJECTED_ADD", true, kNaN, kNaN, /*qty=*/1.0);
        process_pending_orders(current_bar_);  // rejected by pyramiding=1
        preserved_after_rejection = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0)
            && position_entry_count_ == 1
            && near(position_qty_, 10.0);
    }
};

static void test_rejected_add_preserves_opening_eligibility() {
    std::printf("test_rejected_add_preserves_opening_eligibility\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    RejectedAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.preserved_after_rejection);
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 8.0));
    CHECK(near(eng.position_size(), 2.0));
}

// A same-bar add whose requested quantity floors to zero has no accepted
// position effect. Its implementation currently appends a zero-qty roster
// element, so the opening-affordability lifecycle must key on positive added
// quantity rather than vector growth alone.
class ZeroQtyAddProbe : public MCEngine {
public:
    bool preserved_after_zero_add = false;

    ZeroQtyAddProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);

        // apply_qty_step(0.5) == 0 with qty_step=1: the fill kernel appends a
        // zero-qty bookkeeping lot but live position quantity stays exactly 10.
        strategy_entry("ZERO_ADD", true, kNaN, kNaN, /*qty=*/0.5);
        process_pending_orders(current_bar_);
        preserved_after_zero_add = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0)
            && near(position_qty_, 10.0);
    }
};

static void test_zero_qty_add_preserves_opening_eligibility() {
    std::printf("test_zero_qty_add_preserves_opening_eligibility\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ZeroQtyAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.preserved_after_zero_add);
    // The original opening remains actionable: its 20% entry commission gives
    // q_restore=2 lots, so the 4x rule still trims eight on the opening bar.
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 8.0));
    CHECK(near(eng.position_size(), 2.0));
}

// CASH_PER_ORDER charges once per accepted order, not once per bookkeeping
// row. A high-level add that floors to zero currently appends a zero-qty
// pyramid row; counting that row as a second fee crosses this deliberately
// chosen lot-floor boundary and manufactures a four-lot trim.
class ZeroQtyCashPerOrderAddProbe : public MCEngine {
public:
    bool preserved_after_zero_add = false;

    ZeroQtyCashPerOrderAddProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 60.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        strategy_entry("ZERO_ADD", true, kNaN, kNaN, /*qty=*/0.5);
        process_pending_orders(current_bar_);
        preserved_after_zero_add = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(position_qty_, 10.0);
    }
};

static void test_zero_qty_add_does_not_duplicate_cash_per_order_fee() {
    std::printf("test_zero_qty_add_does_not_duplicate_cash_per_order_fee\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ZeroQtyCashPerOrderAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    // One real $60 fee: q_min=(1000-(1000-60))/100=.6, floors to zero, so the
    // broker closes one whole contract. Charging the zero-qty row adds a
    // phantom second fee: q_min=1.2, floors to ONE, and the 4x rule then trims
    // FOUR contracts — that is the regression this fixture exists to catch, and
    // 1 vs 4 still discriminates it.
    CHECK(eng.preserved_after_zero_add);
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), 9.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

// A full close clears the state; a later RAW fresh opening in the same bar
// captures a new raw base and can receive its own affordability trim.
class FlatThenRawFreshProbe : public MCEngine {
public:
    bool first_captured = false;
    bool add_eligible = false;
    bool flat_cleared = false;
    bool raw_fresh_captured = false;

    FlatThenRawFreshProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        first_captured = opening_obligations_.pending()
            && opening_obligations_.actionable();

        strategy_order("ADD", true, /*qty=*/15.0);
        process_pending_orders(current_bar_);
        add_eligible = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0);

        strategy_close_all();
        flat_cleared = position_side_ == PositionSide::FLAT
            && !opening_obligations_.pending()
            && !opening_obligations_.actionable()
            && std::isnan(opening_obligations_.raw_fill_base());

        strategy_order("RAW_FRESH", true, /*qty=*/12.0);
        process_pending_orders(current_bar_);
        raw_fresh_captured = opening_obligations_.pending()
            && opening_obligations_.actionable()
            && near(opening_obligations_.raw_fill_base(), 100.0);
    }
};

static void test_flat_clears_and_raw_fresh_reuses_state() {
    std::printf("test_flat_clears_and_raw_fresh_reuses_state\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    FlatThenRawFreshProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.first_captured);
    CHECK(eng.add_eligible);
    CHECK(eng.flat_cleared);
    CHECK(eng.raw_fresh_captured);
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
    CHECK(margin_call_rows(eng) == 1);
    CHECK(near(eng.position_size(), 4.0));
}

// Reversal is a fresh position cycle. RE-PIN (2026-09-03, design-market-entry-
// affordability): the 10-lot long is admitted at placement (10 * 100 = 1,000
// == MTM 1,000) but the fill gaps to 120 (1,200 > 1,000), so TV drops the
// ENTRY leg and executes only the reversal's closing leg (pin-afford-gapup;
// rampatel BTC 2025-05-12 07:15Z). No long opens, so no opening-affordability
// nibble fires and the opening state stays clear. (This fixture used to
// assert admit-then-nibble 4 of 10 at 120; that shape was never TV-pinned.)
class ReversalOpeningProbe : public MCEngine {
public:
    ReversalOpeningProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 1;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            strategy_entry("L", true, kNaN, kNaN, /*qty=*/10.0);
        }
    }
};

static void test_reversal_captures_fresh_opening_state() {
    std::printf("test_reversal_captures_fresh_opening_state\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 120.0, 121.0,  80.0, 110.0, 1.0),
    };
    ReversalOpeningProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);  // the short, closed by "L"'s closing leg
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.exit_price(0), 120.0));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), 0.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

// A frozen 100%-equity MARKET reversal can pass the signal-time admission
// check yet become microscopically underfunded after the carried short is
// realized at the next-open fill. TV restores this positive sub-step deficit
// by closing exactly one whole contract, not by treating it as dust. The
// numbers pin a source-faithful omitted-quantity reversal event and also
// exercise the frozen-quantity no-refloor path: 5.2798 must survive placement
// and flip.
class DefaultLongReversalFloorZeroProbe : public MCEngine {
public:
    explicit DefaultLongReversalFloorZeroProbe(bool explicit_reversal)
        : explicit_reversal_(explicit_reversal) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 1;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        // Seed the already-partially-liquidated short immediately before the
        // TV-pinned reversal signal. Its open mark at 1841.70 freezes the new
        // long at 5.2798; filling at 1841.71 realizes the remaining short loss
        // and leaves a positive restore amount below the 0.0001 lot step.
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = 1821.96;
        position_entry_time_ = current_bar_.timestamp - 1000;
        position_qty_ = 5.2524;
        position_entry_count_ = 1;
        position_open_bar_ = -1;
        trail_best_price_ = position_entry_price_;
        net_profit_sum_ = -172.449012;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        pyramid_entries_.push_back(
            {position_entry_price_, position_entry_time_, position_qty_,
             "SEED", -1});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["SEED"] = position_qty_;

        if (explicit_reversal_) {
            strategy_entry("L", true, kNaN, kNaN, 5.2798000001);
        } else {
            strategy_entry("L", true);
        }
    }

private:
    bool explicit_reversal_;
};

static std::vector<Bar> default_long_reversal_floor_zero_bars() {
    return {
        mk_bar(1000, 1841.70, 1841.70, 1841.70, 1841.70, 1.0),
        mk_bar(2000, 1841.71, 1841.71, 1841.71, 1841.71, 1.0),
    };
}

static void test_default_long_reversal_floor_zero_closes_one_contract() {
    std::printf("test_default_long_reversal_floor_zero_closes_one_contract\n");
    DefaultLongReversalFloorZeroProbe eng(/*explicit_reversal=*/false);
    auto bars = default_long_reversal_floor_zero_bars();
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.trade_count() == 2);  // seed close + same-fill long MC trim
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.entry_price(1), 1841.71));
    CHECK(near(eng.exit_price(1), 1841.71));
    CHECK(near(eng.trade_size(1), 1.0));
    CHECK(near(eng.position_size(), 4.2798));
}

// The explicit-quantity twin reaches the identical broker discontinuity, and
// the lot rule does not read the entry's quantity provenance.
static void test_explicit_long_reversal_floor_zero_closes_one_contract() {
    std::printf(
        "test_explicit_long_reversal_floor_zero_closes_one_contract\n");
    DefaultLongReversalFloorZeroProbe eng(/*explicit_reversal=*/true);
    auto bars = default_long_reversal_floor_zero_bars();
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.trade_count() == 2);  // seed close + same-fill long MC trim
    CHECK(margin_call_rows(eng) == 1);
    CHECK(near(eng.trade_size(1), 1.0));
    CHECK(near(eng.position_size(), 4.2798));
}

// ── Generic floor-zero forced-liquidation lot (unconditional one contract) ──
//
// TradingView's forced-liquidation quantity rule is
//
//     q_min   = position_qty - equity(adverse) / (adverse*pv*fx*margin/100)
//     q_min   = floor_step(q_min)                    // floor BEFORE the 4x
//     qty_liq = floor_step(4 * q_min)
//     if qty_liq == 0: qty_liq = 1.0                 // ONE WHOLE CONTRACT
//     qty_liq = min(qty_liq, position_qty)
//
// and the floor-zero fallback is UNCONDITIONAL: it is not scoped to a side, a
// commission model, or an entry lifecycle. Forensic fit against every
// `Signal == "Margin call"` fragment in the campaign's TV exports (58,737
// USDT-account fragments over 89 slugs) matches 58,711 = 99.956% exactly, and
// on the 974 events where the fallback value is unconstrained TV closed exactly
// 1.0000 contracts 971 times. 950 of those lie OUTSIDE any short/commission
// lifecycle scope and 464 are LONG *and* commission-free. No alternative
// fallback value (one qty_step, 4 qty_step, the whole residual, 1% of the
// position) matched a single one of them.
//
// The fixtures below pin the two configurations the previous lifecycle gate
// could not reach by construction, plus the structural guards that survive and
// the full-position cap.

// Commission-free all-in short, sized and filled through the ordinary entry
// path, driven to a positive restore quantity smaller than one lot step.
class CommissionFreeShortFloorZeroProbe : public MCEngine {
public:
    CommissionFreeShortFloorZeroProbe(double initial_capital, double qty_step) {
        initial_capital_ = initial_capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;             // commission-free
        margin_short_ = 100.0;               // 1x, Pine default
        process_orders_on_close_ = true;     // market entry fills at bar0 close
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }
    }
};

// RED-1 class (805 TV events): a LONG at margin_long=100 has no adverse-price
// liquidation, so its only broker action is the opening affordability event.
// Commission is zero, so no fee-created provenance exists — the previous gate
// could not emit anything here at all.
class CommissionFreeLongOpeningFloorZeroProbe : public MCEngine {
public:
    CommissionFreeLongOpeningFloorZeroProbe() {
        initial_capital_ = 1000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;             // commission-free
        margin_long_ = 100.0;                // 1x -> no finite liquidation price
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 10.0;
        constexpr double entry = 100.0;
        position_side_ = PositionSide::LONG;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        // required margin 1000.0 vs equity 999.995 => raw q_min = 0.00005,
        // exactly half of one 0.0001 lot, so floor_step(q_min) == 0.
        net_profit_sum_ = -0.005;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "L", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["L"] = qty;
        seed_opening_check(entry, broker::OpeningContinuation::None);
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }
};

// RED-3 class: a directly seeded SHORT at an exact finite-price floor-zero
// discontinuity. entry / adverse / qty / raw_q_min are explicit so the guard
// cases differ in exactly one structural input.
class SeededShortFloorZeroProbe : public MCEngine {
public:
    SeededShortFloorZeroProbe(double qty_step, double qty, double raw_q_min) {
        initial_capital_ = 10000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.01;

        constexpr double entry = 1799.94;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        // Solve net_profit_sum_ so that equity(adverse) == (qty - raw_q_min) *
        // adverse, i.e. the engine's q_min is exactly raw_q_min.
        net_profit_sum_ =
            (qty - raw_q_min) * kAdverse - initial_capital_
            + (kAdverse - entry) * qty;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(
            2000, 1800.00, kAdverse, 1799.50, 1800.50, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }

    static constexpr double kAdverse = 1801.26;
};

// RED-1 — the 805-event class. LONG at margin_long=100, commission 0, positive
// restore quantity below one lot step. TV closes ONE WHOLE CONTRACT; the
// lifecycle-gated engine emitted nothing at all (the gate required a
// commissioned default-long / reversal provenance this shape cannot have).
static void test_commission_free_long_floor_zero_closes_one_contract() {
    std::printf("test_commission_free_long_floor_zero_closes_one_contract\n");
    CommissionFreeLongOpeningFloorZeroProbe eng;
    eng.trigger();

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(near(eng.exit_price(0), 100.0));
    CHECK(near(eng.trade_size(0), 1.0, 1e-9));
    CHECK(near(eng.position_size(), 9.0, 1e-9));
}

// RED-2 — the 166-event class. Commission-free SHORT on the ordinary
// finite-price cascade, no lifecycle provenance of any kind. TV closes ONE
// WHOLE CONTRACT; the engine closed one 0.0001 qty_step.
static void test_commission_free_short_floor_zero_closes_one_contract() {
    std::printf("test_commission_free_short_floor_zero_closes_one_contract\n");
    constexpr double step = 0.0001;
    // All-in short of 10 @ 3999.99 from 39999.9 of equity, one penny of
    // adverse move to an ON-TICK high of 4000.00:
    //   equity(adverse) = capital - (adverse - entry) * 10
    //   q_min           = 10 - equity(adverse) / adverse
    //                   = 20 * (adverse - entry) / adverse = 0.5 * step
    // — half a lot, floors to zero. The short cascade marks the deficit at
    // the mintick-ROUNDED high (process_margin_call, the sizing-basis fix),
    // so the shape is built on-tick where the rounding is an identity and
    // the pin measures the floor-zero rule alone. The shape used to be a
    // 10 @ 100 short against a SYNTHETIC sub-tick high of
    // 2000 / (20 - 0.5 * step) = 100.00025..., marked raw; on the on-tick
    // ledger that print is 100.00, exactly at liquidation, and fires
    // nothing (test_sizing_basis_mintick.cpp E1). The 166-event class this
    // pins is a lot-rule fact and is unchanged by the mark.
    //
    // round 8/9 family R (engine.hpp rules 2 and 5): this lot is worth 0.4
    // units of account (0.0001 x 4000), so the broker admits the all-in
    // short on ten-digit money. At capital == 10 x entry exactly, the
    // price at which the rounded equity buys 10 is 3999.99 as a decimal
    // while the tick-built 3999.99 (399999 x fl(0.01)) sits two ulp above
    // its double — TradingView drops such a tie whole (rule 5; the ETH
    // 15m sweeps famr3e-Et-* on the tick+1ulp closes 1822.86 / 1823.61 /
    // 1838.87). One thousandth of equity above the cost clears it
    // (P = 3999.9901) and leaves q_min at 0.4975 of a step: still a
    // floor-zero short margin call.
    const double entry = 3999.99;
    const double adverse = 4000.00;
    const double capital = 10.0 * entry + 0.001;
    const double equity_at_high = capital - (adverse - entry) * 10.0;
    const double q_min = 10.0 - equity_at_high / adverse;
    CHECK(q_min > 0.0);
    CHECK(q_min < step);
    CHECK(near(q_min, 0.4975 * step, 1e-9));

    std::vector<Bar> bars = {
        mk_bar(1000, entry, entry, entry - 1.0, entry, 1.0),    // short 10 fills @entry
        mk_bar(2000, entry, adverse, entry - 1.0, entry, 1.0),  // adverse high
    };

    CommissionFreeShortFloorZeroProbe eng(
        /*initial_capital=*/capital, /*qty_step=*/step);
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), entry));
    // finding-446: the adverse extreme is a RAW BAR PRICE and books at the
    // nearest tick (an identity on this on-tick high), never the buy-side
    // ceil.
    CHECK(near(eng.exit_price(0), adverse, 1e-12));
    CHECK(near(eng.trade_size(0), 1.0, 1e-9));
    CHECK(near(eng.position_size(), -9.0, 1e-9));
}

// RED-3 — the relaxation must NOT become unconditional in the wrong way. The
// structural guards that survive are exactly the ones the already-generic
// carried-rollover helper uses: the instrument lot grid must be able to express
// one whole contract. Neither case may fall back to a fabricated one-step
// nibble either — the one-qty_step default is contradicted by every decidable
// TV event, so a suppressed fallback is a no-op, not a smaller fill.
static void test_floor_zero_one_contract_respects_structural_guards() {
    std::printf("test_floor_zero_one_contract_respects_structural_guards\n");

    // (a) qty_step 0.3 divides 1.0 off-grid (floor_step(1.0) == 0.9) and the
    //     position is far above one contract, so no full-position cap applies.
    SeededShortFloorZeroProbe off_grid(
        /*qty_step=*/0.3, /*qty=*/6.0, /*raw_q_min=*/0.15);
    off_grid.trigger();
    CHECK(off_grid.trade_count() == 0);
    CHECK(near(off_grid.position_size(), -6.0, 1e-9));

    // (b) qty_step 2.5 is coarser than one whole contract, so "one contract"
    //     is not a tradeable quantity on this instrument at all.
    SeededShortFloorZeroProbe coarse_step(
        /*qty_step=*/2.5, /*qty=*/7.5, /*raw_q_min=*/0.5);
    coarse_step.trigger();
    CHECK(coarse_step.trade_count() == 0);
    CHECK(near(coarse_step.position_size(), -7.5, 1e-9));

    // Teeth: the same shape on a lot grid that CAN express one contract does
    // liquidate exactly 1.0, proving the two assertions above can fail.
    SeededShortFloorZeroProbe on_grid(
        /*qty_step=*/0.25, /*qty=*/6.0, /*raw_q_min=*/0.125);
    on_grid.trigger();
    CHECK(on_grid.trade_count() == 1);
    CHECK(near(on_grid.trade_size(0), 1.0, 1e-9));
    CHECK(near(on_grid.position_size(), -5.0, 1e-9));
}

// RED-4 — the one-contract fallback is still capped at the whole position, so a
// sub-one-contract position is closed out entirely rather than over-liquidated.
static void test_floor_zero_one_contract_caps_at_sub_one_position() {
    std::printf("test_floor_zero_one_contract_caps_at_sub_one_position\n");
    constexpr double step = 0.0001;
    constexpr double target_q_min = 0.5 * step;
    // All-in short of 0.5 @ 100 from 50 of equity:
    //   equity(adverse) = 50 - (adverse - 100) * 0.5
    //   q_min           = 0.5 - equity(adverse) / adverse = 1 - 100 / adverse
    const double adverse = 100.0 / (1.0 - target_q_min);
    const double equity_at_high = 50.0 - (adverse - 100.0) * 0.5;
    const double q_min = 0.5 - equity_at_high / adverse;
    CHECK(q_min > 0.0);
    CHECK(q_min < step);

    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),   // short 0.5 fills @100
        mk_bar(2000, 100.0, adverse, 99.0, 100.0, 1.0),
    };

    CommissionFreeShortFloorZeroProbe eng(
        /*initial_capital=*/50.0, /*qty_step=*/step);
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.5, 1e-9));   // NOT 1.0
    CHECK(near(eng.position_size(), 0.0, 1e-9));
}

// A reused engine handle must clear the per-position state before on_bar of
// the next run. Run 1 deliberately ends with an open position whose one-shot
// event was consumed; run 2 observes a clean state before opening a new RAW
// position and must equal a fresh handle executing run 2 directly.
class ReuseOpeningProbe : public MCEngine {
public:
    bool second_mode = false;
    bool saw_clean_run_start = false;

    ReuseOpeningProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        qty_step_ = 1.0;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        saw_clean_run_start = position_side_ == PositionSide::FLAT
            && !opening_obligations_.pending()
            && !opening_obligations_.actionable()
            && std::isnan(opening_obligations_.raw_fill_base());
        if (second_mode) {
            strategy_order("RAW", true, /*qty=*/10.0);
        } else {
            // 9*100 + 10% fee = 990 <= 1000: eligible but no trim.
            strategy_entry("L", true, kNaN, kNaN, /*qty=*/9.0);
        }
        process_pending_orders(current_bar_);
    }
};

static void test_run_reuse_clears_opening_state() {
    std::printf("test_run_reuse_clears_opening_state\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };

    ReuseOpeningProbe reused;
    reused.run(bars.data(), (int)bars.size());
    CHECK(!reused.opening_pending());
    CHECK(!reused.opening_eligible());
    CHECK(std::isnan(reused.opening_raw_base()));
    CHECK(near(reused.position_size(), 9.0));

    reused.second_mode = true;
    reused.run(bars.data(), (int)bars.size());
    CHECK(reused.saw_clean_run_start);
    CHECK(margin_call_rows(reused) == 1);
    CHECK(near(reused.position_size(), 6.0));

    ReuseOpeningProbe fresh;
    fresh.second_mode = true;
    fresh.run(bars.data(), (int)bars.size());
    CHECK(fresh.saw_clean_run_start);
    CHECK(fresh.trade_count() == reused.trade_count());
    CHECK(near(fresh.position_size(), reused.position_size()));
    CHECK(near(fresh.trade_size(0), reused.trade_size(0)));
    CHECK(near(fresh.entry_price(0), reused.entry_price(0)));
    CHECK(near(fresh.exit_price(0), reused.exit_price(0)));
}

// ---- D: leveraged long (2x) is liquidated by a falling market --------------

class LongLevLiqProbe : public MCEngine {
public:
    LongLevLiqProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;        // 20 @ 100 = 2000 notional = 2x equity
        commission_value_ = 0.0;
        margin_long_ = 50.0;              // 50% margin -> 2x limit; at the edge
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

static void test_long_leveraged_margin_call() {
    std::printf("test_long_leveraged_margin_call\n");
    // long 20 @ 100, equity 1000, margin 50% -> notional 2000 at the 2x limit.
    //   liqPrice = ((1000/20) - 100) / (0.5 - 1) = (50 - 100)/(-0.5) = 100.
    // A fall below 100 triggers a forced exit at the bar's LOW.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0,  99.5, 100.0, 1.0),  // 0: long fills @100
        mk_bar(2000, 100.0, 100.0,  95.0,  96.0, 1.0),  // 1: low 95 < liq 100
        mk_bar(3000,  96.0,  97.0,  80.0,  82.0, 1.0),  // 2: deeper fall
    };
    LongLevLiqProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() >= 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    // First forced exit fills at bar1's adverse extreme (low = 95).
    CHECK(near(eng.exit_price(0), 95.0));
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 4.2105263157894735));
}

} // namespace

int main() {
    test_short_margin_call();
    test_short_margin_call_qty_step();
    test_short_margin_call_zero_cover_closes_full_residual();
    test_short_margin_call_zero_cover_closes_sub_one_residual();
    test_short_margin_call_exact_one_step_roundoff_keeps_four_x_nibble();
    test_short_margin_call_just_below_step_slices_one_contract();
    test_short_margin_call_eps_deficit_slices_one_contract_and_holds();
    test_eps_deficit_chronology_slice_is_one_contract();
    test_short_margin_call_zero_cover_without_qty_step_stays_continuous();
    test_short_margin_call_nonzero_cover_keeps_four_x_nibble();
    test_short_opening_affordability_zero_cover_closes_one_contract();
    test_short_margin_call_account_fx();
    test_margin_liquidation_price_formula();
    test_short_margin_call_disabled();
    test_long_100pct_margin_no_call();
    test_zero_cost_frozen_all_in_true_flat_gap_is_rejected();
    test_commissioned_frozen_all_in_true_flat_gap_is_rejected();
    test_commissioned_frozen_all_in_true_flat_fee_only_shortfall_is_eligible();
    test_paired_short_close_default_long_gap_remains_eligible();
    test_fee_created_floor_zero_closes_one_contract();
    test_fee_created_floor_zero_caps_sub_one_position();
    test_fee_created_sub_half_cent_deficit_respects_fx_ledger();
    test_fee_created_nonzero_floor_keeps_four_x_quantity();
    test_fee_created_floor_zero_rejects_off_grid_one_contract();
    test_cash_per_order_floor_zero_closes_one_contract();
    test_explicit_all_in_zero_comm_adverse_gap_declined();
    test_explicit_all_in_commissioned_adverse_gap_declined();
    test_explicit_all_in_zero_comm_no_qty_step_declined();
    test_explicit_all_in_fx_pointvalue_commission_declined();
    test_long_100pct_margin_trim_process_orders_on_close();
    test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip();
    test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip();
    test_raw_order_fresh_open_captures_affordability();
    test_same_bar_explicit_pair_foreign_fx_direction_symmetry();
    test_same_bar_explicit_pair_fx1_direction_symmetry();
    test_short_add_opening_margin_marks_latest_raw_fill();
    test_thula_next_open_short_pair_exact_margin_rows();
    test_short_opening_event_scope_is_explicit_market_margin100_only();
    test_commissioned_close_then_short_exact_checkpoints();
    test_commissioned_close_then_long_floor_zero_scope();
    test_default_short_lifecycle_floor_zero_one_contract();
    test_default_short_opening_floor_zero_one_contract();
    test_default_short_affordable_opening_retries_adverse_once();
    test_default_flat_short_opening_decision_tracks_commission();
    test_default_flat_short_floor_zero_caps_to_residual();
    test_short_partials_preserve_position_owner();
    test_short_add_replaces_obligation_and_full_close_invalidates();
    test_priced_short_add_invalidates_scoped_opening_event();
    test_raw_short_add_invalidates_scoped_opening_event();
    test_accepted_add_fifo_keeps_add_affordability_event();
    test_rejected_add_preserves_opening_eligibility();
    test_zero_qty_add_preserves_opening_eligibility();
    test_zero_qty_add_does_not_duplicate_cash_per_order_fee();
    test_flat_clears_and_raw_fresh_reuses_state();
    test_reversal_captures_fresh_opening_state();
    test_default_long_reversal_floor_zero_closes_one_contract();
    test_explicit_long_reversal_floor_zero_closes_one_contract();
    test_commission_free_long_floor_zero_closes_one_contract();
    test_commission_free_short_floor_zero_closes_one_contract();
    test_floor_zero_one_contract_respects_structural_guards();
    test_floor_zero_one_contract_caps_at_sub_one_position();
    test_run_reuse_clears_opening_state();
    test_long_leveraged_margin_call();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
