/*
 * test_risk_max_intraday_loss_tv.cpp — round 7 family M, mechanism 5b:
 * TradingView's strategy.risk.max_intraday_loss arithmetic (the JOAT
 * BTC@1D phantom short of 2026-02-06).
 *
 * Sources: campaign note "round 7 family M mechanism 5/7" (officialjackof-
 * alltrades aureate-market-architecture BINANCE:BTCUSDT@1D, moderate 92.3
 * count 1) and the corrected COOF pin log-20260905t121513z-50167cb8 (the
 * recalc sees the bar's finals; the 02-06 phantom is "an open trigger-
 * arithmetic question"). The run_strategy --trace-json of the trigger terms
 * (spark, engine 12f9707) showed every term TRUE on the 02-06 finals in
 * both TradingView and the engine — sensor tape scratchpad/r8/pins/
 * m45-joat-0206-d-norisk (the probe with the risk rule removed) fires the
 * short @60000 exactly like the engine, and the probe's threshold ladder
 * m45-joat-risk-{1.0,1.6,2.0} blocks 02-06 while {2.5,2.8,4.0} trade it —
 * so the divergent component is the rule itself. Pinned on the registry
 * bars by lab tv (BINANCE:BTCUSDT 1D, 2026-01-01..03-01, 100000 USDT, fee
 * 0.01%, scratchpad/r8/pins/m45-risk-*):
 *
 *   t1  short 0.11773 from the 01-31 open 84260.5, limit exit 61319.37
 *       filled intrabar on 02-06 (+2699 realized), probe longs P<day> at
 *       every calc while flat: thresholds 1.0 .. 2.45% drop EVERY 02-06
 *       order (the recalc-born and the close-calc one; P7 fills 02-08),
 *       2.46 .. 3.0% fill P6 @60000 (W1) then @71751.33 (W2) then at the
 *       02-07 open. loss = 2513.61 = the short's open profit at the 02-06
 *       open 62909.87; base = 102513.6 = day-start equity WITH the open
 *       profit (2.4520%): the closing fill's own realized P&L is not yet in
 *       the equity TradingView checks at that tick.
 *   t6  the short held through 02-06 with short adds P5/P6: at 1.0% and
 *       1.1% every lot is closed at the HIGH 71751.33 as "Close Position
 *       (Max intraday Loss)" (loss there 1208.6 = 1.18%); P6 from the
 *       close calc is dropped, P7 fills 02-08.
 *   t9  after the exit a recalc-born short R6 0.15 (fills 60000, -1763 at
 *       the high): no fire at 3.0% — the booked +2699 counts (1578 = 1.54%).
 *   t3b (calc_on_order_fills off) a long 0.11773 filled at the 02-03 open
 *       78738.6: closed at the LOW 72945.5 (-682 = 0.68%) at 0.3%; P4 placed
 *       at the 02-04 close fills at the 02-05 open 73165.84.
 *
 * The old engine rule summed REALIZED P&L per chart-tz day and latched a
 * permanent risk_halted_; it never fired on the JOAT lane and admitted the
 * 02-06 recalc-born short (EN 7 @60000 -> @71751.33) and the close-calc
 * re-entry (EN 8 02-07) where TradingView's next trade is TV 7 on 02-08.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

#include "test_m45_singletons_data.hpp"

using namespace pineforge;
using namespace m45_data;

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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kDay = 86400000LL;
constexpr int64_t kJan30 = 1769731200000LL;
constexpr int64_t kJan31 = 1769817600000LL;
constexpr int64_t kFeb01 = 1769904000000LL;
constexpr int64_t kFeb02 = 1769990400000LL;
constexpr int64_t kFeb03 = 1770076800000LL;
constexpr int64_t kFeb04 = 1770163200000LL;
constexpr int64_t kFeb05 = 1770249600000LL;
constexpr int64_t kFeb06 = 1770336000000LL;
constexpr int64_t kFeb07 = 1770422400000LL;
constexpr int64_t kFeb08 = 1770508800000LL;
const char* const kRiskComment = "Close Position (Max intraday Loss)";

template <size_t N>
std::vector<Bar> to_bars(const BarRow (&rows)[N]) {
    std::vector<Bar> out;
    out.reserve(N);
    for (const BarRow& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = r.volume;
        out.push_back(b);
    }
    return out;
}

int feb_day(int64_t ts) {
    return 1 + static_cast<int>((ts - kFeb01) / kDay);
}

// The sensor tapes' broker: 100000 USDT, 0.01% commission, 1x margin, no
// slippage, market fills at the next tick, pyramiding 10, fixed quantities.
class RiskProbe : public BacktestEngine {
public:
    RiskProbe(double loss_pct, bool coof) {
        initial_capital_ = 100000.0;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1e-5;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.01;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 10;
        slippage_ = 0;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = coof;
        risk_max_intraday_loss_ = loss_pct;
        risk_max_intraday_loss_is_pct_ = true;
        set_margin_call_enabled(true);
    }
    std::function<void(RiskProbe&, const Bar&)> script;
    void on_bar(const Bar& bar) override {
        if (script) script(*this, bar);
    }
    void entry_market(const std::string& id, bool is_long, double qty) {
        strategy_entry(id, is_long, kNaN, kNaN, qty, "");
    }
    void order_market(const std::string& id, bool is_long, double qty) {
        strategy_order(id, is_long, qty, kNaN, kNaN, "", 0);
    }
    void exit_limit(const std::string& id, const std::string& from,
                    double limit) {
        strategy_exit(id, from, limit, kNaN, kNaN, kNaN, kNaN, kNaN, "",
                      kNaN, "", kNaN, kNaN);
    }
    using BacktestEngine::strategy_close;
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool is_short() const { return position_side_ == PositionSide::SHORT; }
    bool is_long_pos() const { return position_side_ == PositionSide::LONG; }
    int closed_count() const { return (int)trades_.size(); }
    // Closed trades plus TradingView's range-end rows for the positions still
    // open after the last bar (the tapes list the probe lots closed at the
    // range end, 2026-03-01).
    std::vector<Trade> closed() const {
        std::vector<Trade> out = trades_;
        out.insert(out.end(), range_end_trades_.begin(), range_end_trades_.end());
        return out;
    }
};

std::vector<Trade> with_entry_id(const std::vector<Trade>& ts,
                                 const std::string& id) {
    std::vector<Trade> out;
    for (const Trade& t : ts) if (t.entry_id == id) out.push_back(t);
    std::sort(out.begin(), out.end(), [](const Trade& a, const Trade& b) {
        if (a.entry_time != b.entry_time) return a.entry_time < b.entry_time;
        return a.entry_price < b.entry_price;
    });
    return out;
}

int risk_closes(const std::vector<Trade>& ts) {
    int n = 0;
    for (const Trade& t : ts) if (t.exit_comment == kRiskComment) ++n;
    return n;
}

void print_trades(const char* tag, const std::vector<Trade>& ts) {
    std::printf("   %s: %zu closed trades\n", tag, ts.size());
    for (const Trade& t : ts) {
        std::printf("      %-4s %s entry %lld @ %.5f qty %.5f exit %lld @ %.5f pnl %.3f [%s|%s]\n",
                    t.entry_id.c_str(), t.is_long ? "long " : "short",
                    (long long)t.entry_time, t.entry_price, t.qty,
                    (long long)t.exit_time, t.exit_price, t.pnl,
                    t.exit_id.c_str(), t.exit_comment.c_str());
    }
}

// t1: the JOAT shape — a profitable short's limit exit on 02-06 and probe
// longs at every calc while flat.
std::vector<Trade> run_t1(double pct) {
    RiskProbe p(pct, /*coof=*/true);
    p.script = [](RiskProbe& e, const Bar& bar) {
        if (bar.timestamp == kJan30) e.entry_market("S", false, 0.11773);
        if (e.is_short()) e.exit_limit("X", "S", 61319.37);
        if (bar.timestamp >= kFeb04 && bar.timestamp <= kFeb08 && !e.is_short()) {
            const int d = feb_day(bar.timestamp);
            e.order_market("P" + std::to_string(d), true, 0.001 * d);
        }
    };
    const std::vector<Bar> bars = to_bars(kBtcDaily);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.last_error().empty());
    return p.closed();
}

void check_t1_short_row(const std::vector<Trade>& ts) {
    const std::vector<Trade> s = with_entry_id(ts, "S");
    CHECK(s.size() == 1);
    if (s.size() == 1) {
        CHECK(!s[0].is_long);
        CHECK(s[0].entry_time == kJan31);
        CHECK_NEAR(s[0].entry_price, 84260.5, 1e-6);
        CHECK_NEAR(s[0].qty, 0.11773, 1e-9);
        CHECK(s[0].exit_time == kFeb06);
        CHECK_NEAR(s[0].exit_price, 61319.37, 1e-6);
        CHECK(s[0].exit_id == "X");
        CHECK(s[0].exit_comment != kRiskComment);
        CHECK_NEAR(s[0].pnl, 2699.15, 0.05);
    }
}

void test_t1_blocked(double pct) {
    std::printf("t1 @ %.2f%%: the exit tick fires (open profit at the day start 2.452%% of E_ds), 02-06 orders dropped\n", pct);
    const std::vector<Trade> ts = run_t1(pct);
    print_trades("engine", ts);
    check_t1_short_row(ts);
    CHECK(with_entry_id(ts, "P6").empty());
    CHECK(with_entry_id(ts, "P4").empty());   // short until 02-06: no probe
    CHECK(with_entry_id(ts, "P5").empty());
    const std::vector<Trade> p7 = with_entry_id(ts, "P7");
    CHECK(p7.size() == 1);
    if (p7.size() == 1) {
        CHECK(p7[0].entry_time == kFeb08);
        CHECK_NEAR(p7[0].entry_price, 69289.37, 1e-6);
        CHECK_NEAR(p7[0].qty, 0.007, 1e-9);
    }
    CHECK(risk_closes(ts) == 0);   // nothing was open at the fire
}

void test_t1_not_blocked(double pct) {
    std::printf("t1 @ %.2f%%: below the fire, the recalc-born P6 fills at the low and the close-calc P6 at the 02-07 open\n", pct);
    const std::vector<Trade> ts = run_t1(pct);
    print_trades("engine", ts);
    check_t1_short_row(ts);
    const std::vector<Trade> p6 = with_entry_id(ts, "P6");
    CHECK(p6.size() >= 2);
    bool at_low = false, at_next_open = false;
    for (const Trade& t : p6) {
        CHECK_NEAR(t.qty, 0.006, 1e-9);
        if (t.entry_time == kFeb06 && std::fabs(t.entry_price - 60000.0) < 1e-6) at_low = true;
        if (t.entry_time == kFeb07 && std::fabs(t.entry_price - 70580.26) < 1e-6) at_next_open = true;
    }
    CHECK(at_low);
    CHECK(at_next_open);
    CHECK(risk_closes(ts) == 0);
}

// t6: the short is held through 02-06 (no exit) with short adds every calc
// from 02-05: the rule fires at the bar's high.
void test_t6(double pct) {
    std::printf("t6 @ %.2f%%: the held short + adds are closed at the 02-06 high 71751.33 as Max intraday Loss\n", pct);
    RiskProbe p(pct, /*coof=*/true);
    p.script = [](RiskProbe& e, const Bar& bar) {
        if (bar.timestamp == kJan30) e.entry_market("S", false, 0.11773);
        if (bar.timestamp >= kFeb05 && bar.timestamp <= kFeb08) {
            const int d = feb_day(bar.timestamp);
            e.order_market("P" + std::to_string(d), false, 0.001 * d);
        }
    };
    const std::vector<Bar> bars = to_bars(kBtcDaily);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.last_error().empty());
    const std::vector<Trade> ts = p.closed();
    print_trades("engine", ts);
    // Every lot open at the high is closed there with TradingView's label.
    std::vector<double> risk_qtys;
    for (const Trade& t : ts) {
        if (t.exit_comment != kRiskComment) continue;
        CHECK(t.exit_time == kFeb06);
        CHECK_NEAR(t.exit_price, 71751.33, 1e-6);
        CHECK(t.exit_id.empty());
        risk_qtys.push_back(t.qty);
    }
    std::sort(risk_qtys.begin(), risk_qtys.end());
    CHECK(risk_qtys.size() == 4);
    if (risk_qtys.size() == 4) {
        CHECK_NEAR(risk_qtys[0], 0.005, 1e-9);    // P5 (02-06 open)
        CHECK_NEAR(risk_qtys[1], 0.006, 1e-9);    // P6 @62909.87 (first-O recalc)
        CHECK_NEAR(risk_qtys[2], 0.006, 1e-9);    // P6 @60000 (W1)
        CHECK_NEAR(risk_qtys[3], 0.11773, 1e-9);  // S
    }
    const std::vector<Trade> p6 = with_entry_id(ts, "P6");
    CHECK(p6.size() == 2);
    if (p6.size() == 2) {
        CHECK(p6[0].entry_time == kFeb06 && p6[1].entry_time == kFeb06);
        CHECK_NEAR(p6[0].entry_price, 60000.0, 1e-6);
        CHECK_NEAR(p6[1].entry_price, 62909.87, 1e-6);
    }
    for (const Trade& t : p6) CHECK(t.entry_time != kFeb07);  // close-calc P6 dropped
    const std::vector<Trade> p7 = with_entry_id(ts, "P7");
    CHECK(p7.size() >= 1);
    if (!p7.empty()) {
        CHECK(p7[0].entry_time == kFeb08);
        CHECK_NEAR(p7[0].entry_price, 69289.37, 1e-6);
    }
}

// t9: the booked +2699 counts at later ticks — a recalc-born short after the
// exit loses 1763 at the high (1.54% net of the gain) and 3.0% does not fire.
void test_t9() {
    std::printf("t9 @ 3.00%%: realized P&L booked earlier in the day counts at later ticks (no fire at the high)\n");
    RiskProbe p(3.0, /*coof=*/true);
    p.script = [](RiskProbe& e, const Bar& bar) {
        if (bar.timestamp == kJan30) e.entry_market("S", false, 0.11773);
        if (e.is_short() && bar.timestamp < kFeb06) e.exit_limit("X", "S", 61319.37);
        if (bar.timestamp == kFeb06 && e.flat() && e.closed_count() == 1) {
            e.order_market("R6", false, 0.15);
        }
        if (bar.timestamp >= kFeb07 && bar.timestamp <= kFeb08) {
            const int d = feb_day(bar.timestamp);
            e.order_market("P" + std::to_string(d), true, 0.001 * d);
        }
    };
    const std::vector<Bar> bars = to_bars(kBtcDaily);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.last_error().empty());
    const std::vector<Trade> ts = p.closed();
    print_trades("engine", ts);
    check_t1_short_row(ts);
    const std::vector<Trade> r6 = with_entry_id(ts, "R6");
    CHECK(!r6.empty());
    double r6_qty = 0.0;
    for (const Trade& t : r6) {
        CHECK(t.entry_time == kFeb06);
        CHECK_NEAR(t.entry_price, 60000.0, 1e-6);
        r6_qty += t.qty;
    }
    CHECK_NEAR(r6_qty, 0.15, 1e-9);
    CHECK(risk_closes(ts) == 0);
}

// t3b: without calc_on_order_fills, a long filled at the 02-03 open is
// closed at that bar's low (the first path extreme whose mark breaches).
void test_t3b() {
    std::printf("t3b @ 0.30%% (calc_on_order_fills off): the 02-03 long is closed at the low 72945.5\n");
    RiskProbe p(0.3, /*coof=*/false);
    p.script = [](RiskProbe& e, const Bar& bar) {
        if (bar.timestamp == kFeb02) e.entry_market("L", true, 0.11773);
        if (bar.timestamp == kFeb03 && e.is_long_pos()) e.strategy_close("L");
        if (bar.timestamp >= kFeb04 && bar.timestamp <= kFeb06 && e.flat()) {
            const int d = feb_day(bar.timestamp);
            e.order_market("P" + std::to_string(d), true, 0.001 * d);
        }
    };
    const std::vector<Bar> bars = to_bars(kBtcDaily);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.last_error().empty());
    const std::vector<Trade> ts = p.closed();
    print_trades("engine", ts);
    const std::vector<Trade> l = with_entry_id(ts, "L");
    CHECK(l.size() == 1);
    if (l.size() == 1) {
        CHECK(l[0].entry_time == kFeb03);
        CHECK_NEAR(l[0].entry_price, 78738.6, 1e-6);
        CHECK(l[0].exit_time == kFeb03);
        CHECK_NEAR(l[0].exit_price, 72945.5, 1e-6);
        CHECK(l[0].exit_comment == kRiskComment);
    }
    const std::vector<Trade> p4 = with_entry_id(ts, "P4");
    CHECK(p4.size() == 1);
    if (p4.size() == 1) {
        CHECK(p4[0].entry_time == kFeb05);
        CHECK_NEAR(p4[0].entry_price, 73165.84, 1e-6);
        CHECK_NEAR(p4[0].qty, 0.004, 1e-9);
    }
    CHECK(risk_closes(ts) == 1);
}

}  // namespace

int main() {
    std::printf("strategy.risk.max_intraday_loss — TradingView's arithmetic on the registry BINANCE:BTCUSDT 1D bars\n");
    test_t1_blocked(1.5);    // the JOAT probe's threshold
    test_t1_blocked(2.45);
    test_t1_not_blocked(2.46);
    test_t1_not_blocked(3.0);
    test_t6(1.0);
    test_t6(1.1);
    test_t9();
    test_t3b();
    std::printf("%d checks passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
