/*
 * test_market_entry_affordability.cpp — design-market-entry-affordability:
 * TradingView's broker admission for a MARKET entry, unified over explicit-qty
 * and default FIXED / CASH sizing (default percent_of_equity keeps its own
 * pinned KI-54 / gap-reject / gross-admission family).
 *
 * Rule pinned against TradingView behaviour (recorded tapes are kept outside
 * this repository):
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
 * Every non-crypto scenario uses invented prices and timestamps; the
 * instrument constants supply only tick, lot and point-value specifications.
 * Prices are on-tick unless the case is about a sub-tick print.
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
static const Instrument kWholeLot = {1.0, 0.01, 1.0};
static const Instrument kBTC = {1.0,  1.0,  0.001};

// Scripted probe. Script chars (indexed by bar_index_):
//   'L' default-sized LONG  market entry "L"      'S' default SHORT "S"
//   'A' default-sized LONG  add "L2" (pyramiding)
//   'l' explicit LONG  "L" qty = entry_qty_       's' explicit SHORT "S"
//   'a' explicit LONG  add "L2" qty = entry_qty_
//   'B' explicit LONG  "Buy" qty = entry_qty_ (the reversal close id)
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
// Gap probes (default fixed qty 1, margin 100, commission 0).

// Synthetic gap: placement 1*200*20=4000 <= 4200, fill 1*230*20=4600
// > 4200. Missing the fill-time admission check would open a long.
void test_nq_gap_up_declined_at_fill() {
    std::printf("-- gap-up: admitted at placement, declined at fill --\n");
    Probe eng(4200.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L....";
    std::vector<Bar> bars = {
        flat_bar(1000, 200.0),                       // 1*200*20=4000 <= 4200
        mk_bar(2000, 230.0, 235.0, 225.0, 232.0), // 1*230*20=4600 > 4200
        flat_bar(3000, 232.0),
        flat_bar(4000, 232.0),
        flat_bar(5000, 232.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// Synthetic downward gap: placement 1*240*20=4800 > 4500 rejects,
// although the later 1*210*20=4200 would fit. Placement cannot be rescued
// by a favorable fill.
void test_nq_gap_down_declined_at_placement() {
    std::printf("-- gap-down: declined at placement --\n");
    Probe eng(4500.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L....";
    std::vector<Bar> bars = {
        flat_bar(1000, 240.0),                       // 1*240*20=4800 > 4500
        mk_bar(2000, 210.0, 215.0, 205.0, 212.0), // 1*210*20=4200 would fit
        flat_bar(3000, 212.0),
        flat_bar(4000, 212.0),
        flat_bar(5000, 212.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK(eng.trade_count() == 0);
}

// Capital 1e6 exceeds both requirements: control fills one contract at 230.
void test_nq_gap_up_control_fills() {
    std::printf("-- gap-up control (capital 1e6) fills --\n");
    Probe eng(1000000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L....";
    std::vector<Bar> bars = {
        flat_bar(1000, 200.0),
        mk_bar(2000, 230.0, 235.0, 225.0, 232.0),
        flat_bar(3000, 232.0),
        flat_bar(4000, 232.0),
        flat_bar(5000, 232.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
    CHECK(!eng.pyramid_entries_.empty());
    if (!eng.pyramid_entries_.empty()) {
        CHECK_NEAR(eng.pyramid_entries_.back().price, 230.0, 1e-9);
    }
}

// A favorable (down) gap on a default FIXED long: the fill notional is below
// the admitted placement notional -> fills (the fill check can only add an
// adverse-gap decline, never re-decline a placement admit).
void test_nq_favorable_gap_fills() {
    std::printf("-- favorable gap fills --\n");
    Probe eng(4200.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L...";
    std::vector<Bar> bars = {
        flat_bar(1000, 200.0),                       // 4000 <= 4200
        mk_bar(2000, 180.0, 185.0, 175.0, 182.0), // 1*180*20=3600 fits
        flat_bar(3000, 182.0),
        flat_bar(4000, 182.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
}

// Exact tie at placement AND fill (notional == equity): admitted (the guard is
// a float guard only, and the comparison is strict).
void test_nq_exact_tie_admits() {
    std::printf("-- exact tie admits --\n");
    Probe eng(4000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L...";
    std::vector<Bar> bars = {
        flat_bar(1000, 200.0),                       // 1*200*20=4000 == equity
        flat_bar(2000, 200.0),
        flat_bar(3000, 200.0),
        flat_bar(4000, 200.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1.0, 1e-9);
}

// margin_pct scales the requirement: margin 50 halves it; margin 0 disables
// the check (TV performs no margin simulation at 0).
void test_nq_margin_scaling_and_zero_inert() {
    std::printf("-- margin 50 scales / margin 0 inert --\n");
    {
        Probe eng(2100.0, kNQ, QtyType::FIXED, 1.0, 0.0, 50.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 200.0),   // 1*200*20*0.5=2000 <= 2100
            flat_bar(2000, 200.0), flat_bar(3000, 200.0),
            flat_bar(4000, 200.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
    }
    {
        Probe eng(1900.0, kNQ, QtyType::FIXED, 1.0, 0.0, 50.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 200.0),   // 2000 > 1900 -> declined
            flat_bar(2000, 200.0), flat_bar(3000, 200.0),
            flat_bar(4000, 200.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
    }
    {
        Probe eng(1000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 0.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 200.0),   // margin 0: no check at all
            mk_bar(2000, 230.0, 235.0, 225.0, 232.0),
            flat_bar(3000, 232.0), flat_bar(4000, 232.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
    }
}

// ---------------------------------------------------------------------------
// Reversals.

// Long 1 at 300; signal 320 costs 1*320*20=6400 > realized equity 6100.
// MTM is 6100+(320-300)*20=6500 >= 6400, so the new short is admitted.
// Costing the closing leg as well would require 12800 and reject it.
void test_reversal_uses_mtm_equity_and_new_side_only() {
    std::printf("-- reversal: MTM equity, new side only --\n");
    Probe eng(6100.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L.S..";
    std::vector<Bar> bars = {
        flat_bar(1000, 300.0),   // 1*300*20=6000 <= 6100
        flat_bar(2000, 300.0),   // L fills
        flat_bar(3000, 320.0),   // MTM 6500 >= required 6400
        flat_bar(4000, 320.0),   // S fills: long closed, short opened
        flat_bar(5000, 320.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -1.0, 1e-9);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).exit_price, 320.0, 1e-9);
    }
}

// Placement MTM = 8300+(420-400)*20=8700 >= 1*420*20=8400.
// The fill gaps to 460: 1*460*20=9200 > the frozen 8700 snapshot.
// Only the entry leg is dropped; the closing leg still exits at 460.
void test_reversal_declined_at_fill_closes_only() {
    std::printf("-- reversal declined at fill: close leg only --\n");
    Probe eng(8300.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L.S...";
    std::vector<Bar> bars = {
        flat_bar(1000, 400.0),    // 1*400*20=8000 <= 8300
        flat_bar(2000, 400.0),    // L fills
        flat_bar(3000, 420.0),    // required 8400 <= MTM 8700
        mk_bar(4000, 460.0, 465.0, 455.0, 460.0), // 9200 > frozen MTM 8700
        flat_bar(5000, 460.0),
        flat_bar(6000, 460.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: SHORT
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).exit_price, 460.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).qty, 1.0, 1e-9);
    }
    CHECK(eng.trades_with_entry_id("S") == 0);
}

// A live short, equity below the price of one contract, and an opposite
// "Buy" entry: the broker closes the short and opens no long. Pre-fix the
// engine opened the long, then margin-called 4x the shortfall and cascaded.
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
    P2 eng(8300.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.script = "L.....";
    std::vector<Bar> bars = {
        flat_bar(1000, 400.0),
        flat_bar(2000, 400.0),
        flat_bar(3000, 420.0),
        mk_bar(4000, 460.0, 465.0, 455.0, 460.0),
        flat_bar(5000, 460.0),
        flat_bar(6000, 460.0),
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

// Held 1 at 500, add 1 at 520 requires 2*520*20=20800.
// MTM = 20200+(520-500)*20=20600, so the add is dropped.
// Control capital 21000 gives MTM 21400 >= 20800 and admits the add.
void test_pyramiding_add_costed_as_resulting_position() {
    std::printf("-- pyramiding add costed held + add --\n");
    {
        // initial 20200 + (520-500)*20 = MTM 20600 at the add.
        Probe eng(20200.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
        eng.script = "L.A..";
        std::vector<Bar> bars = {
            flat_bar(1000, 500.0),    // 1*500*20=10000 fits
            flat_bar(2000, 500.0),    // L fills
            flat_bar(3000, 520.0),   // 20800 > 20600
            flat_bar(4000, 520.0),
            flat_bar(5000, 520.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 1.0, 1e-9);     // pre-fix: 2
        CHECK(eng.pyramid_entries_.size() == 1);
        CHECK(eng.trades_with_entry_id("L2") == 0);
    }
    {
        Probe eng(21000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
        eng.script = "L.A..";
        std::vector<Bar> bars = {
            flat_bar(1000, 500.0), flat_bar(2000, 500.0),
            flat_bar(3000, 520.0), flat_bar(4000, 520.0),
            flat_bar(5000, 520.0),
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
        Probe eng(10000.0, kWholeLot, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
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
        P2 eng(10000.0, kWholeLot, QtyType::FIXED, 1.0, 0.0, 100.0, 2, false);
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

// Invented fractional-lot control: E=100.249, close=25, raw qty=4.00996.
// Floor to 0.01 lots gives 4.00; the half-cent open 25.005 books at 25.01.
// Floored cost 4*25.01=100.04 <= E; raw cost 4.00996*25.01=100.2890996
// > E. This distinguishes lot-flooring BEFORE admission; fee 0.05% is
// excluded from notional and margin calls are disabled.
void test_xauusd_floored_qty_admitted() {
    std::printf("-- synthetic fractional-lot quantity 4 admitted --\n");
    Probe eng(100.249, kXAU, QtyType::FIXED, 1.0, 0.05, 100.0, 0, false);
    eng.script = "E....";
    std::vector<Bar> bars = {
        flat_bar(1000, 25.0),
        mk_bar(2000, 25.005, 26.0, 24.0, 25.5),
        flat_bar(3000, 25.5),
        flat_bar(4000, 25.5),
        flat_bar(5000, 25.5),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // pre-fix: FLAT
    CHECK_NEAR(eng.position_size(), 4.0, 1e-9);
    CHECK(eng.pyramid_entries_.size() == 1);
    if (!eng.pyramid_entries_.empty()) {
        CHECK_NEAR(eng.pyramid_entries_.back().price,
                   25.01, 1e-9); // nearest-cent half tie: 25.005 -> 25.01
    }
}

// Invented half-cent close: qty = 25,025 / 125.125 = 200 whole lots.
// The cent grid rounds UP to 125.13: 200 * 125.13 = 25,026 > 25,025,
// so placement rejects it despite the raw-close notional being an exact tie.
// The next open is also 125.13: a max(E, qty * tick(close)) floor must not
// rescue the entry at fill. The no-gap on-tick case below is its control.
void test_case_half_cent_declined() {
    std::printf("-- synthetic whole-lot half-cent close: declined --\n");
    Probe eng(25025.0, kWholeLot, QtyType::FIXED, 1.0, 0.05, 100.0, 0, false);
    eng.script = "E....";
    std::vector<Bar> bars = {
        flat_bar(1000, 125.125),
        mk_bar(2000, 125.13, 125.50, 125.00, 125.25),
        flat_bar(3000, 125.25),
        flat_bar(4000, 125.25),
        flat_bar(5000, 125.25),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG 200
    CHECK(eng.trade_count() == 0);
}

// The same idiom with an on-tick close and a no-gap fill admits the floored
// quantity (positive control for the two cases above).
void test_all_in_idiom_no_gap_admits() {
    std::printf("-- all-in idiom, no gap: admits --\n");
    Probe eng(25025.0, kWholeLot, QtyType::FIXED, 1.0, 0.05, 100.0, 0, false);
    eng.script = "E...";
    std::vector<Bar> bars = {
        flat_bar(1000, 125.13),      // floor(25,025 / 125.13) = 199; cost 24,900.87 <= 25,025
        flat_bar(2000, 125.13),
        flat_bar(3000, 125.13),
        flat_bar(4000, 125.13),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 199.0, 1e-9); // 199 * 125.13 = 24,900.87
}

// ---------------------------------------------------------------------------
// CASH default sizing takes the same rule (the broker does not know how the
// quantity was derived): cash 20,000 on 10,000 capital
// at margin 100 is 200 lots @100 = 20,000 > 10,000 -> declined; cash 5,000 ->
// 50 lots admitted. (Re-pins test_margin_admission_gate's former CASH
// exemption, which was a scope carve-out, not a TV observation.)
void test_cash_default_sizing_gated() {
    std::printf("-- CASH default sizing gated --\n");
    {
        Probe eng(10000.0, kWholeLot, QtyType::CASH, 20000.0, 0.0, 100.0, 0, false);
        eng.script = "L...";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.0), flat_bar(2000, 100.0),
            flat_bar(3000, 100.0), flat_bar(4000, 100.0),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
    }
    {
        Probe eng(10000.0, kWholeLot, QtyType::CASH, 5000.0, 0.0, 100.0, 0, false);
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
    Probe eng(4000.0, kNQ, QtyType::FIXED, 1.0, 0.0, 100.0, 0, false);
    eng.set_process_orders_on_close(true);
    eng.script = "L..";
    std::vector<Bar> bars = {
        flat_bar(1000, 200.0),   // placed AND filled at the close, tie
        mk_bar(2000, 230.0, 235.0, 225.0, 232.0),
        flat_bar(3000, 232.0),
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
    test_case_half_cent_declined();
    test_all_in_idiom_no_gap_admits();
    test_cash_default_sizing_gated();
    test_pooc_no_double_decline();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
