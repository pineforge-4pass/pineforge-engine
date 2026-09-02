/*
 * test_trade_start_gap_pad.cpp — the trade-start gate admits the script bar
 * immediately PRECEDING the first in-window bar whatever calendar gap sits
 * in front of the gate (weekend, holiday, session break), found by index on
 * the feed rather than by subtracting one script TF in milliseconds.
 *
 * Evidence: orb-lite on NYSE:F 1D fires its short on the 2026-03-13 (Friday)
 * bar and TradingView fills it on 2026-03-16 (Monday), its first entry. The
 * validator opened the window one bar interval before Monday (Sunday), the
 * gate's millisecond buffer reached one script TF further (Saturday), and
 * Friday's strategy.entry sat before it — dropped, so the engine's tape
 * started elsewhere. compute_trade_start_preceding_script_bar
 * (engine_run.cpp) now resolves the bar before the first bar >= the gate
 * once per run, and trading_is_active (engine_strategy_commands.cpp) admits
 * it beside the unchanged millisecond rule.
 *
 * Pins (a market RAW_ORDER placed on ``place_bar`` fills the next bar's open
 * when the gate admits the placement; the final position tells). The rule
 * is ONE pad: the script bar immediately preceding the first in-window bar
 * is admitted, nothing earlier. When the preceding bar resolves, the old
 * millisecond buffer is not stacked on top of it (a first cut of this change
 * did stack it and, with the validator's gate one bar early as well, admitted
 * TWO bars before TradingView's first entry — Thursday for a Monday entry).
 *   A. Daily feed Thu/Fri/Mon/Tue with the gate at Monday minus one calendar
 *      day (Sunday — the pre-fix validator pad, and what --emit-window-ohlcv
 *      style windows still produce): Friday's placement is admitted and
 *      fills Monday's open; Thursday's (two bars before Monday) is refused.
 *   B. The gate the validator now sets, Monday's label itself (TV's first
 *      entry): Friday admitted, Thursday refused, Monday in-window.
 *   C. A gate on Friday's label: Thursday (the bar before it) is admitted,
 *      Wednesday is refused — the pad is one bar under any gate, where the
 *      millisecond buffer stacked on the index rule admitted Wednesday too.
 *   C2. Mid-week daily gate (Wednesday): Tuesday admitted, Monday refused.
 *   D. Aggregated 1m -> 15m feed across an overnight session gap with the
 *      gate on day 2's first 15m label (TV's entry): day 1's last 15m script
 *      bar is admitted and its order fills day 2's first script bar; day 1's
 *      second-to-last is refused. The pre-fix 1m validator pad (one input
 *      minute before the label) gives the same verdicts.
 *   E. Continuous 1m feed (test_strategy_commands_extra's gate pin, T = bar
 *      3): bar 2 admitted, bar 1 refused — intraday behaviour unchanged.
 *   E2. Continuous same-TF 15m feed with the gate on the entry bar: the bar
 *      before it admitted, two bars before refused.
 *   F. No gate: everything admitted, as before. A gate past the feed keeps
 *      the millisecond rule (nothing admitted here); a gate before the feed
 *      admits everything.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

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

static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

namespace {

// NYSE 1D bars, labelled at the 13:30 UTC session open.
constexpr int64_t kThu = 1773322200000LL;   // 2026-03-12
constexpr int64_t kFri = 1773408600000LL;   // 2026-03-13
constexpr int64_t kMon = 1773667800000LL;   // 2026-03-16
constexpr int64_t kTue = 1773754200000LL;   // 2026-03-17
constexpr int64_t kDayMs = 86'400'000LL;

std::vector<Bar> daily_feed() {
    // Wed(idx0) Thu(1) Fri(2) Mon(3) Tue(4) Wed(5): a weekend between 2 and 3.
    return {
        mk_bar(kThu - kDayMs, 100, 101, 99, 100),
        mk_bar(kThu,          100, 101, 99, 100),
        mk_bar(kFri,          100, 101, 99, 100),
        mk_bar(kMon,          100, 101, 99, 100),
        mk_bar(kTue,          100, 101, 99, 100),
        mk_bar(kTue + kDayMs, 100, 101, 99, 100),
    };
}

class GateProbe : public BacktestEngine {
public:
    int place_bar = -1;
    double final_pos = 1234.0;
    int fill_bar = -1;
    explicit GateProbe(int pb) : place_bar(pb) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == place_bar) {
            strategy_order("E", true, 2.0, /*limit=*/kNaN, /*stop=*/kNaN);
        }
        if (fill_bar < 0 && signed_position_size() != 0.0) fill_bar = bar_index_;
        final_pos = signed_position_size();
    }
    int preceding() const { return trade_start_preceding_script_bar_; }
};

// Run the daily feed with a placement on ``place_bar`` and the gate at
// ``gate_ms``; report whether the order filled and on which script bar.
struct Verdict { bool filled; int fill_bar; int preceding; };
Verdict daily_verdict(int place_bar, int64_t gate_ms) {
    GateProbe p(place_bar);
    p.set_trade_start_time(gate_ms);
    auto bars = daily_feed();
    p.run(bars.data(), (int)bars.size());
    return {p.final_pos == 2.0, p.fill_bar, p.preceding()};
}

}  // namespace

// A. Gate = Sunday (Monday - 1 calendar day).
static void test_weekend_gap_gate_on_sunday() {
    std::printf("-- A: gate Sunday: Friday admitted, Thursday refused --\n");
    Verdict fri = daily_verdict(/*place_bar=*/2, kMon - kDayMs);
    CHECK(fri.preceding == 2);                   // Friday precedes Monday
    CHECK(fri.filled);                           // pre-fix: refused
    CHECK(fri.fill_bar == 3);                    // fills Monday's open
    Verdict thu = daily_verdict(/*place_bar=*/1, kMon - kDayMs);
    CHECK(!thu.filled);
}

// B. Gate exactly on Monday's label: the validator's gate (TV's first entry).
static void test_weekend_gap_gate_on_monday() {
    std::printf("-- B: gate Monday: Friday admitted, Thursday refused --\n");
    Verdict fri = daily_verdict(2, kMon);
    CHECK(fri.preceding == 2);
    CHECK(fri.filled);                           // pre-fix: refused (Fri < Sun)
    CHECK(fri.fill_bar == 3);
    Verdict thu = daily_verdict(1, kMon);
    CHECK(!thu.filled);                          // two bars before: refused
    Verdict mon = daily_verdict(3, kMon);
    CHECK(mon.filled && mon.fill_bar == 4);      // in-window, as before
}

// C. Gate on Friday's label: one pad, Thursday only.
static void test_gate_on_friday_pads_one_bar() {
    std::printf("-- C: gate Friday: Thursday admitted, Wednesday refused --\n");
    Verdict fri = daily_verdict(2, kFri);
    CHECK(fri.preceding == 1);                   // Thursday precedes Friday
    CHECK(fri.filled && fri.fill_bar == 3);
    Verdict thu = daily_verdict(1, kFri);
    CHECK(thu.filled && thu.fill_bar == 2);
    Verdict wed = daily_verdict(0, kFri);        // first cut: admitted (Fri - 1d)
    CHECK(!wed.filled);
}

// C2. Mid-week daily gate: no calendar gap, still one pad.
static void test_midweek_gate_pads_one_bar() {
    std::printf("-- C2: gate Wednesday: Tuesday admitted, Monday refused --\n");
    const int64_t wed = kTue + kDayMs;
    Verdict tue = daily_verdict(4, wed);
    CHECK(tue.preceding == 4);
    CHECK(tue.filled && tue.fill_bar == 5);
    Verdict mon = daily_verdict(3, wed);
    CHECK(!mon.filled);
}

// D. Aggregated 1m -> 15m across an overnight gap.
static void test_aggregated_session_gap() {
    std::printf("-- D: 1m->15m across a session gap: day 1's last script bar admitted --\n");
    // Day 1: 13:30..14:29 UTC (four 15m buckets), day 2: 13:30..14:29 UTC.
    const int64_t d1 = kFri, d2 = kMon;
    std::vector<Bar> bars;
    for (int i = 0; i < 60; ++i) bars.push_back(mk_bar(d1 + i * 60'000LL, 100, 101, 99, 100));
    for (int i = 0; i < 60; ++i) bars.push_back(mk_bar(d2 + i * 60'000LL, 100, 101, 99, 100));
    // Script bars: day 1 -> 0,1,2,3; day 2 -> 4,5,6,7 (the last bucket may
    // stay open at end of feed; the placements below never depend on it).
    auto run_case = [&](int place_bar, int64_t gate) {
        GateProbe p(place_bar);
        p.set_trade_start_time(gate);
        p.run(bars.data(), (int)bars.size(), "1", "15", /*bar_magnifier=*/false, 4,
              MagnifierDistribution::ENDPOINTS);
        CHECK(p.last_error().empty());
        return Verdict{p.final_pos == 2.0, p.fill_bar, p.preceding()};
    };
    for (int64_t gate : {d2, d2 - 60'000LL}) {     // TV's entry label; the old 1m pad
        Verdict last_d1 = run_case(3, gate);
        CHECK(last_d1.preceding == 3);
        CHECK(last_d1.filled);                   // pre-fix: refused (day-1 14:15 < day-2 13:14)
        CHECK(last_d1.fill_bar == 4);            // day 2's first script bar
        Verdict prev_d1 = run_case(2, gate);
        CHECK(!prev_d1.filled);
    }
}

// E. Continuous 1m feed: unchanged.
static void test_continuous_minute_feed_unchanged() {
    std::printf("-- E: continuous 1m feed, gate bar 3: bar 2 admitted, bar 1 refused --\n");
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) bars.push_back(mk_bar((int64_t)(i + 1) * 60'000, 100, 105, 95, 100));
    {
        GateProbe gated(1);
        gated.set_trade_start_time(240'000);
        gated.run(bars.data(), (int)bars.size());
        CHECK(gated.preceding() == 2);
        CHECK(gated.final_pos == 0.0);
    }
    {
        GateProbe active(2);
        active.set_trade_start_time(240'000);
        active.run(bars.data(), (int)bars.size());
        CHECK(active.final_pos == 2.0);
        CHECK(active.fill_bar == 3);
    }
}

// E2. Continuous same-TF 15m feed, gate on the entry bar: one pad.
static void test_continuous_15m_feed_one_pad() {
    std::printf("-- E2: continuous 15m feed, gate bar 5: bar 4 admitted, bar 3 refused --\n");
    std::vector<Bar> bars;
    const int64_t base = 1'773'619'200'000LL;    // 2026-03-16 00:00 UTC
    for (int i = 0; i < 10; ++i) bars.push_back(mk_bar(base + i * 900'000LL, 100, 105, 95, 100));
    Verdict four{}, three{};
    {
        GateProbe p(4);
        p.set_trade_start_time(base + 5 * 900'000LL);
        p.run(bars.data(), (int)bars.size());
        four = {p.final_pos == 2.0, p.fill_bar, p.preceding()};
    }
    {
        GateProbe p(3);
        p.set_trade_start_time(base + 5 * 900'000LL);
        p.run(bars.data(), (int)bars.size());
        three = {p.final_pos == 2.0, p.fill_bar, p.preceding()};
    }
    CHECK(four.preceding == 4);
    CHECK(four.filled && four.fill_bar == 5);
    CHECK(!three.filled);                        // the old validator pad + buffer admitted it
}

// F. No gate; gates outside the feed.
static void test_no_gate() {
    std::printf("-- F: no gate: everything admitted; gates outside the feed --\n");
    auto bars = daily_feed();
    GateProbe p(0);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.preceding() == -1);
    CHECK(p.final_pos == 2.0);
    CHECK(p.fill_bar == 1);
    // A gate past the feed's end has no preceding bar: the millisecond rule
    // alone applies (nothing admitted here).
    GateProbe late(4);
    late.set_trade_start_time(kTue + 10 * kDayMs);
    late.run(bars.data(), (int)bars.size());
    CHECK(late.preceding() == -1);
    CHECK(late.final_pos == 0.0);
    // A gate before the feed's first bar: nothing precedes it, everything
    // is in-window.
    GateProbe early(0);
    early.set_trade_start_time(kThu - 10 * kDayMs);
    early.run(bars.data(), (int)bars.size());
    CHECK(early.preceding() == -1);
    CHECK(early.final_pos == 2.0 && early.fill_bar == 1);
}

int main() {
    test_weekend_gap_gate_on_sunday();
    test_weekend_gap_gate_on_monday();
    test_gate_on_friday_pads_one_bar();
    test_midweek_gate_pads_one_bar();
    test_aggregated_session_gap();
    test_continuous_minute_feed_unchanged();
    test_continuous_15m_feed_one_pad();
    test_no_gate();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
