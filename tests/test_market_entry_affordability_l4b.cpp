/*
 * test_market_entry_affordability.cpp — design-market-entry-affordability:
 * TradingView's broker admission for a MARKET entry, unified over explicit-qty
 * and default FIXED / CASH sizing (default percent_of_equity keeps its own
 * pinned KI-54 / gap-reject / gross-admission family).
 *
 * Rule (pinned 2026-09-03 with `lab tv`, TV is ground truth):
 *
 *   admit iff lot_floored(resulting_position_qty)
 *               * max(tick(close(S)), tick(fill)) * pv * fx * margin/100
 *             <= placement_equity + max(1e-9, |placement_equity| * 1e-12)
 *
 * evaluated TWICE — at placement on tick(close(S)) against MARK-TO-MARKET
 * equity (initial + net_profit + open_profit at close(S)), and at fill on
 * tick(fill) against the same placement snapshot. "Resulting position" is the
 * new side's qty on a reversal (the closing leg is not counted) and held + add
 * on a same-direction add. Commission is NOT in the notional; there is no
 * max(equity, signal_notional) admission floor. A rejected reversal drops the
 * ENTRY leg only — its closing leg still executes.
 *
 * Evidence (tapes under scratchpad/r5/pins, `lab tv` exports):
 *   pin-afford-{flat,reverse} CME_MINI:NQ1! 15, fixed qty 1, margin 100:
 *     10,212 flat-entry + reversal decisions, 0 mismatches; the short reversal
 *     at 2025-05-06 14:15Z filled with realized 396,625 < cost 397,995 because
 *     MTM 398,455 >= cost.
 *   pin-afford-gapup:     capital 380,000, signal close 18,820.50 (376,410 ok)
 *                         -> fill 19,225 (384,500 > 380k) -> NOT filled.
 *   pin-afford-gapdown:   capital 345,000, signal close 17,483.25 (349,665 >
 *                         345k) -> fill 17,100 (342,000 ok) -> NOT filled.
 *   pin-afford-gapup-ctl: capital 1e6 fills at 19,225.
 *   pin-admit-allin-xau   OANDA:XAUUSD 15, qty = strategy.equity / close,
 *                         commission 0.05%, 1279/1279: 2025-04-08 13:30Z
 *                         E 1,998,000.02, close 3013.72, open 3013.745, lot
 *                         step 0.01 -> 662.968 -> 662.96 lots ADMITTED.
 *   pin-admit-allin-f     NYSE:F 15, 352/352: half-cent close 10.225 -> fill
 *                         10.23: floor(E/10.225) * 10.23 > E -> DECLINED.
 *   production: rampatel BTC 2025-05-12 07:15Z — TV closed the short remainder
 *     by "Buy" @105,600 and opened no long (equity 103,572 < 105,600);
 *     masayanfx NQ1 2025-07-30 20:15Z — pyramiding add 2 * 23,667.75 * 20 =
 *     946,710 > MTM 945,225 -> TV dropped the add.
 *
 * Every price below is on-tick for its instrument (NQ 0.25, XAUUSD/F 0.01)
 * unless the case is ABOUT a sub-tick print, so fills land on the bar prices.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

static Bar flat_bar(int64_t ts, double p) { return mk_bar(ts, p, p, p, p); }

namespace {

struct Instrument {
    double pointvalue;
    double mintick;
    double qty_step;
};
static const Instrument kNQ  = {20.0, 0.25, 1.0};
static const Instrument kXAU = {1.0,  0.01, 0.01};
static const Instrument kF   = {1.0,  0.01, 1.0};
static const Instrument kBTC = {1.0,  1.0,  0.001};

// Scripted probe. Script chars (indexed by bar_index_):
//   'L' default-sized LONG  market entry "L"      'S' default SHORT "S"
//   'A' default-sized LONG  add "L2" (pyramiding)
//   'l' explicit LONG  "L" qty = entry_qty_       's' explicit SHORT "S"
//   'a' explicit LONG  add "L2" qty = entry_qty_
//   'B' explicit LONG  "Buy" qty = entry_qty_ (the rampatel reversal id)
//   'E' explicit LONG  "L" qty = strategy.equity / close (the all-in idiom)
//   'C' strategy.close("L")                        '.' nothing
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, const Instrument& ins, QtyType qty_type,
          double qty_value, double comm_pct, double margin, int pyramiding,
          bool enable_mc) {
        initial_capital_ = capital;
        syminfo_.pointvalue = ins.pointvalue;
        syminfo_mintick_ = ins.mintick;
        qty_step_ = ins.qty_step;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = comm_pct;
        margin_long_ = margin;
        margin_short_ = margin;
        pyramiding_ = pyramiding;
        slippage_ = 0;
        process_orders_on_close_ = false;
        margin_call_enabled_ = enable_mc;
    }
    std::string script;
    double entry_qty_ = 1.0;

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("L", true); break;
            case 'S': strategy_entry("S", false); break;
            case 'A': strategy_entry("L2", true); break;
            case 'l': strategy_entry("L", true,  kNaN, kNaN, entry_qty_); break;
            case 's': strategy_entry("S", false, kNaN, kNaN, entry_qty_); break;
            case 'a': strategy_entry("L2", true, kNaN, kNaN, entry_qty_); break;
            case 'B': strategy_entry("Buy", true, kNaN, kNaN, entry_qty_); break;
            case 'E': {
                // Pine: qty = strategy.equity / close, raw (sub-lot) value.
                const double equity =
                    current_equity() + open_profit(current_bar_.close);
                strategy_entry("L", true, kNaN, kNaN,
                               equity / current_bar_.close);
                break;
            }
            case 'C': strategy_close("L"); break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    using BacktestEngine::pyramid_entries_;
    double position_size() const { return signed_position_size(); }
    double tick(double p) const { return round_to_mintick(p); }
    int trades_with_entry_id(const std::string& id) const {
        int n = 0;
        for (const auto& t : trades_) if (t.entry_id == id) ++n;
        return n;
    }
    int trades_with_exit_id(const std::string& id) const {
        int n = 0;
        for (const auto& t : trades_) if (t.exit_id == id) ++n;
        return n;
    }
    void set_process_orders_on_close(bool enabled) {
        process_orders_on_close_ = enabled;
    }
};

// ---------------------------------------------------------------------------
// NQ gap probes (default fixed qty 1, margin 100, commission 0).

// pin-afford-gapup: placement admits (376,410 <= 380,000); the fill gaps to
// 19,225 (384,500 > 380,000) -> the entry is NOT filled. Pre-fix: FIXED default
// sizing had no fill-time check at all and the engine opened it.
void test_nq_gap_up_declined_at_fill() {
    std::printf("-- NQ gap-up: admitted at placement, declined at fill --\n");
    Probe eng(380000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L....";
    std::vector<Bar> bars = {
        flat_bar(1000, 18820.50),                       // L placed: 376,410 ok
        mk_bar(2000, 19225.0, 19240.0, 19200.0, 19230.0), // fill 384,500 > 380k
        flat_bar(3000, 19230.0),
        flat_bar(4000, 19230.0),
        flat_bar(5000, 19230.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// pin-afford-gapdown: placement REJECTS (349,665 > 345,000) even though the
// gapped-down fill at 17,100 (342,000) would have been affordable -> NOT
// filled. TV uses the rounded signal close as a decline trigger, never as an
// admission floor. Pre-fix: no placement check for default sizing -> LONG.
void test_nq_gap_down_declined_at_placement() {
    std::printf("-- NQ gap-down: declined at placement --\n");
    Probe eng(345000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L....";
    std::vector<Bar> bars = {
        flat_bar(1000, 17483.25),                       // 349,665 > 345,000
        mk_bar(2000, 17100.0, 17120.0, 17080.0, 17110.0), // 342,000 would fit
        flat_bar(3000, 17110.0),
        flat_bar(4000, 17110.0),
        flat_bar(5000, 17110.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK(eng.trade_count() == 0);
}

// pin-afford-gapup-ctl: capital 1e6, same gap -> fills 1 @ 19,225.
void test_nq_gap_up_control_fills() {
    std::printf("-- NQ gap-up control (capital 1e6) fills --\n");
    Probe eng(1000000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L....";
    std::vector<Bar> bars = {
        flat_bar(1000, 18820.50),
        mk_bar(2000, 19225.0, 19240.0, 19200.0, 19230.0),
        flat_bar(3000, 19230.0),
        flat_bar(4000, 19230.0),
        flat_bar(5000, 19230.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
    CHECK(!eng.pyramid_entries_.empty());
    if (!eng.pyramid_entries_.empty()) {
        CHECK_NEAR(eng.pyramid_entries_.back().price, 19225.0, 1e-9);
    }
}

// A favorable (down) gap on a default FIXED long: the fill notional is below
// the admitted placement notional -> fills (the fill check can only add an
// adverse-gap decline, never re-decline a placement admit).
void test_nq_favorable_gap_fills() {
    std::printf("-- NQ favorable gap fills --\n");
    Probe eng(380000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L...";
    std::vector<Bar> bars = {
        flat_bar(1000, 18820.50),                       // 376,410 <= 380,000
        mk_bar(2000, 18800.0, 18810.0, 18790.0, 18805.0), // 376,000 fits
        flat_bar(3000, 18805.0),
        flat_bar(4000, 18805.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
}

// Exact tie at placement AND fill (notional == equity): admitted (the guard is
// a float guard only, and the comparison is strict).
void test_nq_exact_tie_admits() {
    std::printf("-- NQ exact tie admits --\n");
    Probe eng(376410.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L...";
    std::vector<Bar> bars = {
        flat_bar(1000, 18820.50),                       // 376,410 == equity
        flat_bar(2000, 18820.50),
        flat_bar(3000, 18820.50),
        flat_bar(4000, 18820.50),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
}

// margin_pct scales the requirement: margin 50 halves it; margin 0 disables
// the check (TV performs no margin simulation at 0).
void test_nq_margin_scaling_and_zero_inert() {
    std::printf("-- NQ margin 50 scales / margin 0 inert --\n");
    {
        Probe eng(190000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 50.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 18820.50),   // 376,410 * 0.5 = 188,205 <= 190,000
            flat_bar(2000, 18820.50), flat_bar(3000, 18820.50),
            flat_bar(4000, 18820.50),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
    }
    {
        Probe eng(180000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 50.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 18820.50),   // 188,205 > 180,000 -> declined
            flat_bar(2000, 18820.50), flat_bar(3000, 18820.50),
            flat_bar(4000, 18820.50),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
    }
    {
        Probe eng(1000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 0.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 18820.50),   // margin 0: no check at all
            mk_bar(2000, 19225.0, 19240.0, 19200.0, 19230.0),
            flat_bar(3000, 19230.0), flat_bar(4000, 19230.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
    }
}

// ---------------------------------------------------------------------------
// Reversals.

// pin-afford-reverse 2025-05-06 14:15Z shape: long 1 @ 19,808.25; the short
// signal closes at 19,899.75 (cost 397,995). Realized equity 396,625 < cost,
// but MTM = 396,625 + 91.50 * 20 = 398,455 >= cost -> TV filled the reversal.
// Only the NEW side's qty is costed (the closing leg is not part of the
// notional). Pre-fix (realized-only basis) would have declined it.
void test_reversal_uses_mtm_equity_and_new_side_only() {
    std::printf("-- reversal: MTM equity, new side only --\n");
    Probe eng(396625.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L.S..";
    std::vector<Bar> bars = {
        flat_bar(1000, 19808.25),   // L placed (396,165 <= 396,625)
        flat_bar(2000, 19808.25),   // L fills
        flat_bar(3000, 19899.75),   // S placed: MTM 398,455 >= 397,995
        flat_bar(4000, 19899.75),   // S fills: long closed, short opened
        flat_bar(5000, 19899.75),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -1.0, 1e-9);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).exit_price, 19899.75, 1e-9);
    }
}

// The reversal is admitted at placement (MTM 389,000 >= 384,000) but the fill
// gaps to 19,500 (390,000 > 389,000): the ENTRY leg is dropped, the CLOSING
// leg still executes at the fill under the entry's id.
void test_reversal_declined_at_fill_closes_only() {
    std::printf("-- reversal declined at fill: close leg only --\n");
    Probe eng(385000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L.S...";
    std::vector<Bar> bars = {
        flat_bar(1000, 19000.0),    // L placed (380,000 <= 385,000)
        flat_bar(2000, 19000.0),    // L fills
        flat_bar(3000, 19200.0),    // S placed: 384,000 <= MTM 389,000
        mk_bar(4000, 19500.0, 19520.0, 19480.0, 19500.0), // 390,000 > 389,000
        flat_bar(5000, 19500.0),
        flat_bar(6000, 19500.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: SHORT
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).exit_price, 19500.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).qty, 1.0, 1e-9);
    }
    CHECK(eng.trades_with_entry_id("S") == 0);
}

// rampatel BTC 2025-05-12 07:15Z shape: a live short, equity below the price
// of one contract, an opposite "Buy" entry. TV closed the short by "Buy"
// @105,600 and opened no long. Pre-fix the engine opened the long, then
// margin-called 4x the shortfall and cascaded (23,605 trades vs TV 1,486).
// (a) margin calls disabled: the pure broker rule.
// (b) margin calls enabled: no long is ever opened, no cascade.
void test_rampatel_reversal_close_leg_executes_no_entry() {
    std::printf("-- rampatel: reversal close leg executes, no entry --\n");
    {
        Probe eng(103572.0, kBTC, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
        eng.entry_qty_ = 1.0;
        eng.script = "S.B...";
        std::vector<Bar> bars = {
            flat_bar(1000, 100000.0),   // S placed (100,000 <= 103,572)
            flat_bar(2000, 100000.0),   // S fills
            flat_bar(3000, 105600.0),   // Buy placed: MTM 97,972 < 105,600
            flat_bar(4000, 105600.0),   // Buy: closes the short, opens nothing
            flat_bar(5000, 105600.0),
            flat_bar(6000, 105600.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
        CHECK(eng.trade_count() == 1);
        if (eng.trade_count() == 1) {
            CHECK(!eng.get_trade(0).is_long);
            CHECK(eng.get_trade(0).exit_id == "Buy");
            CHECK_NEAR(eng.get_trade(0).exit_price, 105600.0, 1e-9);
            CHECK_NEAR(eng.get_trade(0).pnl, -5600.0, 1e-6);
        }
        CHECK(eng.trades_with_entry_id("Buy") == 0);
    }
    {
        Probe eng(103572.0, kBTC, QtyType::FIXED, 1.0, 0.0, 100.0, 0, true);
        eng.entry_qty_ = 1.0;
        eng.script = "S.B.......";
        std::vector<Bar> bars = {
            flat_bar(1000, 100000.0),
            flat_bar(2000, 100000.0),
            flat_bar(3000, 105600.0),   // the short may be margin-sliced here
            flat_bar(4000, 105600.0),   // Buy closes the remainder, no long
            flat_bar(5000, 105600.0),
            flat_bar(6000, 105600.0),
            flat_bar(7000, 105600.0),
            flat_bar(8000, 105600.0),
            flat_bar(9000, 105600.0),
            flat_bar(10000, 105600.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
        CHECK(eng.trades_with_entry_id("Buy") == 0);    // pre-fix: cascade
        CHECK(eng.trades_with_exit_id("Buy") == 1);
        CHECK(eng.trade_count() <= 3);                  // no 4x cascade
        for (int i = 0; i < eng.trade_count(); ++i) {
            CHECK(!eng.get_trade(i).is_long);
        }
    }
}

// A same-bar strategy.close co-queued AFTER the declined reversal is a no-op
// (the entry's closing leg already flattened the account): exactly one exit
// row, attributed to the entry id.
void test_declined_reversal_with_coqueued_close() {
    std::printf("-- declined reversal + co-queued close: one exit row --\n");
    class P2 : public Probe {
    public:
        using Probe::Probe;
        void on_source_bar(const Bar& bar) override {
            if (bar_index_ == 2) {
                strategy_entry("S", false);
                strategy_close("L");
                return;
            }
            Probe::on_source_bar(bar);
        }
    };
    P2 eng(385000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L.....";
    std::vector<Bar> bars = {
        flat_bar(1000, 19000.0),
        flat_bar(2000, 19000.0),
        flat_bar(3000, 19200.0),
        mk_bar(4000, 19500.0, 19520.0, 19480.0, 19500.0),
        flat_bar(5000, 19500.0),
        flat_bar(6000, 19500.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).qty, 1.0, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// Pyramiding adds: the RESULTING position (held + add) is costed.

// masayanfx NQ1 2025-07-30 20:15Z: held 1, add 1 at 23,667.75 -> 2 * 23,667.75
// * 20 = 946,710 > MTM 945,225 -> the add is dropped, the position stays 1.
// Control at 950,000 admits it. Pre-fix: FIXED default adds were ungated.
void test_pyramiding_add_costed_as_resulting_position() {
    std::printf("-- pyramiding add costed held + add --\n");
    {
        // initial 943,870 + open profit 67.75 * 20 = MTM 945,225 at the add.
        Probe eng(943870.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
        eng.script = "L.A..";
        std::vector<Bar> bars = {
            flat_bar(1000, 23600.0),    // L placed (472,000 fits)
            flat_bar(2000, 23600.0),    // L fills
            flat_bar(3000, 23667.75),   // L2 placed: 946,710 > 945,225
            flat_bar(4000, 23667.75),
            flat_bar(5000, 23667.75),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 1.0, 1e-9);     // pre-fix: 2
        CHECK(eng.pyramid_entries_.size() == 1);
        CHECK(eng.trades_with_entry_id("L2") == 0);
    }
    {
        Probe eng(950000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
        eng.script = "L.A..";
        std::vector<Bar> bars = {
            flat_bar(1000, 23600.0), flat_bar(2000, 23600.0),
            flat_bar(3000, 23667.75), flat_bar(4000, 23667.75),
            flat_bar(5000, 23667.75),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 2.0, 1e-9);
        CHECK(eng.pyramid_entries_.size() == 2);
    }
}

// Explicit-qty add, same arithmetic: held 60 + add 60 at 100 = 12,000 >
// 10,000 -> dropped; held 60 + add 40 = 10,000 == equity -> admitted.
void test_explicit_add_costed_as_resulting_position() {
    std::printf("-- explicit add costed held + add --\n");
    {
        Probe eng(10000.0, kF, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
        eng.entry_qty_ = 60.0;
        eng.script = "l.a..";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.0), flat_bar(2000, 100.0), flat_bar(3000, 100.0),
            flat_bar(4000, 100.0), flat_bar(5000, 100.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK_NEAR(eng.position_size(), 60.0, 1e-9);    // pre-fix: 120
    }
    {
        class P2 : public Probe {
        public:
            using Probe::Probe;
            void on_source_bar(const Bar& bar) override {
                if (bar_index_ == 2) {
                    strategy_entry("L2", true, kNaN, kNaN, 40.0);
                    return;
                }
                Probe::on_source_bar(bar);
            }
        };
        P2 eng(10000.0, kF, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
        eng.entry_qty_ = 60.0;
        eng.script = "l....";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.0), flat_bar(2000, 100.0), flat_bar(3000, 100.0),
            flat_bar(4000, 100.0), flat_bar(5000, 100.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// Explicit qty = strategy.equity / close (the all-in idiom).

// pin-admit-allin-xau 2025-04-08 13:30Z: E 1,998,000.02, close 3013.72, next
// open 3013.745 (a sub-tick print that books at tick(3013.745)), lot step
// 0.01, commission 0.05%. qty = 662.968 -> floored 662.96; 662.96 * 3013.75 =
// 1,997,995.7 <= E -> ADMITTED. Pre-fix the engine costed the RAW 662.968 at
// the fill (1,998,016 > E) and declined.
void test_xauusd_floored_qty_admitted() {
    std::printf("-- XAUUSD 662.96 admitted (lot-floored qty) --\n");
    Probe eng(1998000.02, kXAU, QtyType::FIXED, 1.0, 0.05, 100.0, 0, false);
    eng.script = "E....";
    std::vector<Bar> bars = {
        flat_bar(1000, 3013.72),
        mk_bar(2000, 3013.745, 3015.0, 3012.0, 3014.0),
        flat_bar(3000, 3014.0),
        flat_bar(4000, 3014.0),
        flat_bar(5000, 3014.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // pre-fix: FLAT
    CHECK_NEAR(eng.position_size(), 662.96, 1e-9);
    CHECK(eng.pyramid_entries_.size() == 1);
    if (!eng.pyramid_entries_.empty()) {
        CHECK_NEAR(eng.pyramid_entries_.back().price,
                   eng.tick(3013.745), 1e-9);
    }
}

// pin-admit-allin-f: half-cent close 10.225 (E = 10,225 -> qty 1000), next
// open 10.23: 1000 * 10.23 = 10,230 > E -> DECLINED. Pre-fix the fill gate's
// max(E, qty * tick(close)) floor admitted it.
void test_f_half_cent_declined() {
    std::printf("-- F half-cent close: declined --\n");
    Probe eng(10225.0, kF, QtyType::FIXED, 1.0, 0.05, 100.0, 0, false);
    eng.script = "E....";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.225),
        mk_bar(2000, 10.23, 10.25, 10.20, 10.24),
        flat_bar(3000, 10.24),
        flat_bar(4000, 10.24),
        flat_bar(5000, 10.24),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG 1000
    CHECK(eng.trade_count() == 0);
}

// The same idiom with an on-tick close and a no-gap fill admits the floored
// quantity (positive control for the two cases above).
void test_all_in_idiom_no_gap_admits() {
    std::printf("-- all-in idiom, no gap: admits --\n");
    Probe eng(10225.0, kF, QtyType::FIXED, 1.0, 0.05, 100.0, 0, false);
    eng.script = "E...";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.23),      // qty = 999.51 -> 999 shares = 10,219.77
        flat_bar(2000, 10.23),
        flat_bar(3000, 10.23),
        flat_bar(4000, 10.23),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 999.0, 1e-9);
}

// ---------------------------------------------------------------------------
// CASH default sizing takes the same rule (no TV tape of its own; the broker
// does not know how the quantity was derived): cash 20,000 on 10,000 capital
// at margin 100 is 200 lots @100 = 20,000 > 10,000 -> declined; cash 5,000 ->
// 50 lots admitted. (Re-pins test_margin_admission_gate's former CASH
// exemption, which was a scope carve-out, not a TV observation.)
void test_cash_default_sizing_gated() {
    std::printf("-- CASH default sizing gated --\n");
    {
        Probe eng(10000.0, kF, QtyType::CASH, 20000.0, 0.0, 100.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.0), flat_bar(2000, 100.0),
            flat_bar(3000, 100.0), flat_bar(4000, 100.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
    }
    {
        Probe eng(10000.0, kF, QtyType::CASH, 5000.0, 0.0, 100.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.0), flat_bar(2000, 100.0),
            flat_bar(3000, 100.0), flat_bar(4000, 100.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 50.0, 1e-9);
    }
}

// Priced (stop) entries and process_orders_on_close are outside / a no-op for
// this gate: a POOC fill books at tick(close(S)) == the placement price, so
// the fill half can never decline what placement admitted.
void test_pooc_no_double_decline() {
    std::printf("-- POOC: fill half is a no-op --\n");
    Probe eng(376410.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.set_process_orders_on_close(true);
    eng.script = "L..";
    std::vector<Bar> bars = {
        flat_bar(1000, 18820.50),   // placed AND filled at the close, tie
        mk_bar(2000, 19225.0, 19240.0, 19200.0, 19230.0),
        flat_bar(3000, 19230.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
}

}  // namespace

int main() {
    std::printf("--- market_entry_affordability ---\n");
    test_nq_gap_up_declined_at_fill();
    test_nq_gap_down_declined_at_placement();
    test_nq_gap_up_control_fills();
    test_nq_favorable_gap_fills();
    test_nq_exact_tie_admits();
    test_nq_margin_scaling_and_zero_inert();
    test_reversal_uses_mtm_equity_and_new_side_only();
    test_reversal_declined_at_fill_closes_only();
    test_rampatel_reversal_close_leg_executes_no_entry();
    test_declined_reversal_with_coqueued_close();
    test_pyramiding_add_costed_as_resulting_position();
    test_explicit_add_costed_as_resulting_position();
    test_xauusd_floored_qty_admitted();
    test_f_half_cent_declined();
    test_all_in_idiom_no_gap_admits();
    test_cash_default_sizing_gated();
    test_pooc_no_double_decline();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
