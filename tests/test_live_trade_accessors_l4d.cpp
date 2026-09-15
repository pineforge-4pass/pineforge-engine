// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

/*
 * test_live_trade_accessors.cpp — ABI v4 task 9: closed-trade id / exit-
 * comment / close-cause accessors and the position-size / equity /
 * script-bars-processed scalars.
 *
 * strategy_closed_trade_entry_id / _exit_id / _exit_comment cannot be
 * exercised as same-named BacktestEngine methods the way
 * strategy_closed_trade_entry_incarnation's underlying field is: those
 * three names are already taken by the protected, narrower
 * strategy.closedtrades.* accessors (trades_-only scope, std::string
 * returns; see engine.hpp). So this test drives them through the actual
 * C ABI entry points (the real ABI v4 deliverable), passing the engine
 * instance itself as the opaque pf_strategy_t handle -- valid because
 * c_abi.cpp static_casts it straight back to BacktestEngine*, and this
 * test links against the same `pineforge` static library that c_abi.cpp is
 * part of. closed_trade_close_cause / report_trade_count / signed_position_
 * size / script_bars_processed have no such collision and are exercised
 * both directly and through the C ABI for cross-checking.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

Bar bar(double o, double h, double l, double c, int64_t ts) { return Bar{o, h, l, c, 1.0, ts}; }

// --- Case 1: a bracket exit (close_cause BRACKET=2) followed by a script
// close (close_cause SCRIPT=1). ---
class Probe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_exit("x", "L", na<double>(), 95.0);          // bracket stop
        if (bar_index_ == 4) strategy_entry("S", false);
        if (bar_index_ == 5) strategy_close("S", "done");                          // script close
    }
};

// --- Case 2: a margin-call forced liquidation (close_cause MARGIN_CALL=3).
// Shape copied from tests/test_margin_call.cpp's ShortLiqProbe (100%-equity
// short force-liquidated by a rising market): entry fills at bar0 close,
// bar1's high breaches the liquidation price and forces an exit whose
// exit_id the engine sets to the "__margin_call__" sentinel. ---
class MarginCallProbe final : public pineforge::source::PineStrategyHost {
public:
    MarginCallProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;               // 1x, default TV margin
        process_orders_on_close_ = true;     // market entry fills at bar close
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("S", false);
    }
};

// --- Case 3: an open position at the end of a flag-off run (close_cause
// RANGE_END=6). Shape copied from tests/test_live_realtime_tail.cpp's
// HoldStrategy: enter long and hold; with strategy_set_realtime_tail left
// off (the default), the final bar synthesizes a range-end row. ---
class HoldProbe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 2) strategy_entry("L", true);
    }
};

// --- Case 4: a whole-position strategy.exit bracket revived and fired at
// the margin-call event price (close_cause BRACKET=2, exit_id == the
// bracket's own id) -- revive_position_brackets_after_margin_call_partial,
// engine_fills.cpp. This is the exact TV-pinned round 7 family N mechanism
// 2 fixture "D. fast-scalper 07-21 13:30Z" from
// tests/test_aapl15_margin_brackets.cpp (NASDAQ:AAPL 15m, feed
// ae2b03d3736f), copied verbatim: a 4889-share all-in short re-issues its
// stop at 213.08 in position; a declined all-in Long reversal at the
// 07-18 19:45Z close leaves that stop dormant across the 07-21 open; the
// bar's high 214.86 both breaches the liquidation price (a 268-share
// "Margin call" slice) and the dormant stop's level, reviving and firing
// it for the 4621-share remainder AT THE SAME PRICE, same bar. TV prints
// both rows (probe TV#160/161); the engine's own row-for-row pin is
// test_aapl15_margin_brackets.cpp's check_trade(p, 1, ...) with exit_tag
// "X" (the bracket id). ---
class FastScalperReviveProbe final : public pineforge::source::PineStrategyHost {
public:
    FastScalperReviveProbe() {
        initial_capital_ = 1056333.80;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;  // all_in()
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) {
            strategy_entry("S", false, na<double>(), na<double>(), 4889.0, "");
            strategy_exit("X", "S", na<double>(), 212.83);
        }
        if (bar_index_ == 24) strategy_exit("X", "S", na<double>(), 213.08);  // re-issued in position
        if (bar_index_ == 28) strategy_entry("L", true);                     // declined at the open
    }
};

// --- Case 5: strategy.risk.max_intraday_loss forces a close (close_cause
// INTRADAY_LOSS_CAP=4). Smallest fixture reaching evaluate_max_intraday_
// loss's forced-close branch (engine_risk.cpp): a 1-share long fills at
// bar 1's open (100), and that same bar's low (90) marks a 10-currency
// open-profit loss against the day-start equity -- over the 5-currency
// absolute threshold -- so the position is closed within bar 1 itself,
// tagged the verbatim TV comment "Close Position (Max intraday Loss)"
// (exit_id stays empty, per engine_risk.cpp:165). No existing test file
// reaches this path through a full run() with a small synthetic feed --
// tests/test_engine_risk.cpp drives evaluate_max_intraday_loss directly
// (protected-method unit test, no Trade row), and
// tests/test_risk_max_intraday_loss_tv.cpp pins it against real multi-day
// registry tapes (tests/test_m45_singletons_data.hpp) -- so this is the
// smallest one that does, per the review's own fallback instruction. ---
class IntradayLossCapProbe final : public pineforge::source::PineStrategyHost {
public:
    IntradayLossCapProbe() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        // Native-route configuration is set through the public source
        // command, rather than the retired owner-private risk slots.
        set_pine_risk_max_intraday_loss(5.0, false);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
    }
};

// --- Case 6: strategy.risk.max_intraday_filled_orders forces a close
// (close_cause INTRADAY_FILL_CAP=5). Shape copied from
// tests/test_intraday_cap_auto_close.cpp's test_cap_latches_until_day_
// rollover: cap=1, so the FIRST fill on the chart-day is immediately
// followed by TV's synthetic full close at the same fill price, tagged
// "Close Position (Max number of filled orders in one day)" (exit_id
// stays empty, per engine_run.cpp / engine_fills.cpp). ---
class IntradayFillCapProbe final : public pineforge::source::PineStrategyHost {
public:
    IntradayFillCapProbe() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 10;
        adapter_.cap = 1;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
    }
};

}  // namespace

int main() {
    // ---- bracket exit + script close ----
    const std::vector<Bar> bars = {
        bar(100, 100, 100, 100, 0), bar(100, 100, 100, 100, 60'000),
        bar(100, 100, 90, 92, 120'000),      // stop 95 touched -> bracket exit
        bar(92, 92, 92, 92, 180'000), bar(92, 92, 92, 92, 240'000),
        bar(92, 92, 92, 92, 300'000), bar(92, 92, 92, 92, 360'000),
    };
    Probe s;
    s.run(bars.data(), 7);
    const pf_strategy_t h = static_cast<pf_strategy_t>(&s);

    CHECK(s.report_trade_count() == 2);

    CHECK(strategy_closed_trade_entry_id(h, 0) != nullptr);
    CHECK(std::strcmp(strategy_closed_trade_entry_id(h, 0), "L") == 0);
    CHECK(strategy_closed_trade_exit_id(h, 0) != nullptr);
    CHECK(std::strcmp(strategy_closed_trade_exit_id(h, 0), "x") == 0);
    CHECK(strategy_closed_trade_close_cause(h, 0) == 2);                 // BRACKET
    CHECK(s.closed_trade_close_cause(0) == 2);                           // same, direct engine call

    CHECK(strategy_closed_trade_exit_comment(h, 1) != nullptr);
    CHECK(std::strcmp(strategy_closed_trade_exit_comment(h, 1), "done") == 0);
    // Pin the engine's internal exit-id spelling for a deferred
    // strategy.close(id, ...): "__close__" + id (see pineforge.h
    // strategy_closed_trade_exit_id) -- distinct from a real bracket's own
    // id ("x" above) and from the "__margin_call__" sentinel below.
    CHECK(strategy_closed_trade_exit_id(h, 1) != nullptr);
    CHECK(std::strcmp(strategy_closed_trade_exit_id(h, 1), "__close__S") == 0);
    CHECK(strategy_closed_trade_close_cause(h, 1) == 1);                 // SCRIPT
    CHECK(s.closed_trade_close_cause(1) == 1);

    // Bad index: out-of-range trade_index -> NULL / -1, not a crash. Final
    // review F7: close_cause returns -1 for an out-of-range index (matching
    // every sibling indexed live accessor), not 0 -- 0 is reserved for the
    // documented "no cause" value on a VALID trade.
    CHECK(strategy_closed_trade_entry_id(h, 5) == nullptr);
    CHECK(strategy_closed_trade_exit_id(h, -1) == nullptr);
    CHECK(strategy_closed_trade_exit_comment(h, 5) == nullptr);
    CHECK(s.closed_trade_close_cause(5) == -1);
    CHECK(strategy_closed_trade_close_cause(h, 5) == -1);
    CHECK(s.closed_trade_close_cause(-1) == -1);

    // NULL handle: -1 for the int accessor, NULL for the string ones.
    CHECK(strategy_closed_trade_close_cause(nullptr, 0) == -1);
    CHECK(strategy_closed_trade_entry_id(nullptr, 0) == nullptr);
    CHECK(strategy_closed_trade_exit_id(nullptr, 0) == nullptr);
    CHECK(strategy_closed_trade_exit_comment(nullptr, 0) == nullptr);

    // Position/equity scalars: fully flat by the end of the run, equity is
    // exactly initial capital plus the sum of the two trades' own recorded
    // PnL (this cross-checks strategy_current_equity's semantics -- initial
    // capital + realized net profit -- without hand-computing fill prices).
    CHECK(std::fabs(s.live_position_size()) < 1e-12);
    CHECK(std::fabs(strategy_position_size(h)) < 1e-12);
    CHECK(std::isnan(strategy_position_size(nullptr)));

    const double expected_equity =
        1'000'000.0 + s.get_report_trade(0).pnl + s.get_report_trade(1).pnl;
    CHECK(near(strategy_current_equity(h), expected_equity));
    CHECK(std::isnan(strategy_current_equity(nullptr)));

    CHECK(s.script_bars_processed() == 7);
    CHECK(strategy_script_bars_processed(h) == 7);
    CHECK(strategy_script_bars_processed(nullptr) == -1);

    // ---- margin call ----
    {
        const std::vector<Bar> mc_bars = {
            bar(100.0, 100.0, 99.0, 100.0, 1000),   // 0: short fills @100
            bar(100.0, 105.0, 99.5, 104.0, 2000),   // 1: high 105 -> margin call
        };
        MarginCallProbe m;
        m.run(mc_bars.data(), (int)mc_bars.size());
        const pf_strategy_t mh = static_cast<pf_strategy_t>(&m);

        CHECK(m.trade_count() >= 1);
        CHECK(m.closed_trade_close_cause(0) == 3);                       // MARGIN_CALL
        CHECK(strategy_closed_trade_close_cause(mh, 0) == 3);
        CHECK(strategy_closed_trade_exit_id(mh, 0) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_exit_id(mh, 0), "__margin_call__") == 0);
    }

    // ---- range end ----
    {
        std::vector<Bar> hold_bars;
        for (int i = 0; i < 10; ++i) {
            hold_bars.push_back(bar(100.0 + i, 100.0 + i, 100.0 + i, 100.0 + i, i * 60'000LL));
        }
        HoldProbe hp;
        hp.run(hold_bars.data(), (int)hold_bars.size());
        const pf_strategy_t rh = static_cast<pf_strategy_t>(&hp);

        CHECK(hp.trade_count() == 0);            // no script/bracket/margin close
        CHECK(hp.report_trade_count() == 1);     // the range-end row lives in report space
        CHECK(hp.closed_trade_close_cause(0) == 6);                      // RANGE_END
        CHECK(strategy_closed_trade_close_cause(rh, 0) == 6);
        CHECK(strategy_closed_trade_entry_id(rh, 0) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_entry_id(rh, 0), "L") == 0);
    }

    // ---- revived bracket fired at the margin-call event price (family N
    // mechanism 2) ----
    {
        // NASDAQ:AAPL 15m, feed ae2b03d3736f, 2025-07-17 19:15Z .. 07-21
        // 14:00Z -- verbatim from test_aapl15_margin_brackets.cpp's
        // kAaplScalper.
        const std::vector<Bar> fs_bars = {
            bar(210.825, 211.06, 210.825, 210.99, 1752779700000LL),   // [0] 07-17 19:15
            bar(211, 211.05, 210.68, 210.72, 1752780600000LL),        // [1] 19:30 signal
            bar(210.71, 210.75, 209.74, 210.02, 1752781500000LL),     // [2] 19:45 entry bar
            bar(210.87, 211.01, 209.9, 210.03, 1752845400000LL),      // [3] 07-18 13:30
            bar(210.01, 210.51, 209.89, 210.32, 1752846300000LL),     // [4] 13:45
            bar(210.33, 210.62, 209.71, 210.1, 1752847200000LL),      // [5] 14:00
            bar(210.11, 210.31, 209.89, 209.95, 1752848100000LL),     // [6] 14:15
            bar(209.96, 210.51, 209.78, 210.29, 1752849000000LL),     // [7] 14:30
            bar(210.34, 211.01, 210.27, 210.49, 1752849900000LL),     // [8] 14:45
            bar(210.5, 211, 210.44, 210.77, 1752850800000LL),         // [9] 15:00
            bar(210.74, 210.9, 210.42, 210.83, 1752851700000LL),      // [10] 15:15
            bar(210.87, 211.005, 210.7, 210.97, 1752852600000LL),     // [11] 15:30
            bar(210.97, 211.1, 210.93, 211.08, 1752853500000LL),      // [12] 15:45
            bar(211.07, 211.13, 210.9, 210.94, 1752854400000LL),      // [13] 16:00
            bar(210.92, 211.105, 210.67, 211, 1752855300000LL),       // [14] 16:15
            bar(211.02, 211.76, 210.88, 211.64, 1752856200000LL),     // [15] 16:30
            bar(211.66, 211.79, 211.2, 211.32, 1752857100000LL),      // [16] 16:45
            bar(211.31, 211.4, 211.05, 211.22, 1752858000000LL),      // [17] 17:00
            bar(211.25, 211.43, 211.1, 211.19, 1752858900000LL),      // [18] 17:15
            bar(211.18, 211.53, 211.02, 211.32, 1752859800000LL),     // [19] 17:30
            bar(211.33, 211.44, 210.97, 211.095, 1752860700000LL),    // [20] 17:45
            bar(211.1, 211.26, 210.88, 210.93, 1752861600000LL),      // [21] 18:00
            bar(210.95, 210.97, 210.765, 210.94, 1752862500000LL),    // [22] 18:15
            bar(210.93, 211.06, 210.86, 211.01, 1752863400000LL),     // [23] 18:30
            bar(211.01, 211.055, 210.79, 210.97, 1752864300000LL),    // [24] 18:45 stop re-issue
            bar(210.96, 211.04, 210.88, 211.02, 1752865200000LL),     // [25] 19:00
            bar(211.02, 211.195, 210.895, 210.95, 1752866100000LL),   // [26] 19:15
            bar(210.94, 211.065, 210.84, 210.95, 1752867000000LL),    // [27] 19:30
            bar(210.96, 211.35, 210.835, 211.225, 1752867900000LL),   // [28] 19:45 reversal signal
            bar(212.06, 214.86, 211.63, 214.67, 1753104600000LL),     // [29] 07-21 13:30
            bar(214.68, 215.78, 213.96, 214.01, 1753105500000LL),     // [30] 13:45
            bar(214.05, 214.76, 214.01, 214.73, 1753106400000LL),     // [31] 14:00
        };
        FastScalperReviveProbe fs;
        fs.run(fs_bars.data(), (int)fs_bars.size());
        const pf_strategy_t fh = static_cast<pf_strategy_t>(&fs);

        // TV#160/161: a 268-share margin-call slice, then the revived 'X'
        // stop closes the 4621-share remainder, both @214.86 on bar 29.
        CHECK(fs.trade_count() == 2);
        CHECK(fs.closed_trade_close_cause(0) == 3);                     // MARGIN_CALL
        CHECK(strategy_closed_trade_close_cause(fh, 0) == 3);
        CHECK(fs.closed_trade_close_cause(1) == 2);                     // BRACKET (the fix)
        CHECK(strategy_closed_trade_close_cause(fh, 1) == 2);
        CHECK(strategy_closed_trade_exit_id(fh, 1) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_exit_id(fh, 1), "X") == 0);
    }

    // ---- strategy.risk.max_intraday_loss forced close ----
    {
        const std::vector<Bar> loss_bars = {
            bar(100, 100, 100, 100, 0),         // 0: entry "L" placed
            bar(100, 101, 90, 95, 60'000),       // 1: fills @ open=100; low=90 -> loss 10 >= 5 -> forced close
        };
        IntradayLossCapProbe lp;
        lp.run(loss_bars.data(), (int)loss_bars.size());
        const pf_strategy_t lh = static_cast<pf_strategy_t>(&lp);

        CHECK(lp.trade_count() == 1);
        CHECK(lp.closed_trade_close_cause(0) == 4);                     // INTRADAY_LOSS_CAP
        CHECK(strategy_closed_trade_close_cause(lh, 0) == 4);
        CHECK(strategy_closed_trade_exit_id(lh, 0) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_exit_id(lh, 0), "") == 0);
        CHECK(strategy_closed_trade_exit_comment(lh, 0) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_exit_comment(lh, 0),
                           "Close Position (Max intraday Loss)") == 0);
    }

    // ---- strategy.risk.max_intraday_filled_orders forced close ----
    {
        const std::vector<Bar> fill_bars = {
            bar(100, 101, 99, 100, 0),           // 0: entry "L" placed
            bar(101, 102, 100, 101, 60'000),     // 1: fills @ open=101 (count=1 -> cap -> synthetic close @101)
        };
        IntradayFillCapProbe fp;
        fp.run(fill_bars.data(), (int)fill_bars.size());
        const pf_strategy_t fph = static_cast<pf_strategy_t>(&fp);

        CHECK(fp.trade_count() == 1);
        CHECK(fp.closed_trade_close_cause(0) == 5);                     // INTRADAY_FILL_CAP
        CHECK(strategy_closed_trade_close_cause(fph, 0) == 5);
        CHECK(strategy_closed_trade_exit_id(fph, 0) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_exit_id(fph, 0), "") == 0);
        CHECK(strategy_closed_trade_exit_comment(fph, 0) != nullptr);
        CHECK(std::strcmp(strategy_closed_trade_exit_comment(fph, 0),
                           "Close Position (Max number of filled orders in one day)") == 0);
    }

    std::printf("\ntest_live_trade_accessors: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
