// test_o_close_pct_day_anchor.cpp -- round 7 family O: the CME_MINI:NQ1! /
// ES1! singletons' two pinned engine rules, replayed on the registry's own
// NQ1! 15m bars (feed ed88b5530c0a) and TradingView's NQ1! 1D bars (feed
// ef0a39bf35d8, the nq1-15 lane's feeds.daily) against the family-O lab tv
// tapes (test_o_close_pct_day_anchor_data.hpp).
//
// (4) strategy.close(id, qty_percent = p) -- ledger log-20260905t123542z-
//     b46852d8, tapes scratchpad/r7/pins/o-nq-qtypct-{a,b,c} (byte-identical,
//     tv_trades sha cfed3953..., 139 trades each): TradingView closes
//     max(1 lot, floor(qty x p / 100)) on an integer-lot symbol. Three
//     contracts (percent_of_equity 100 on 1.5M, NQ 20 USD/pt, POOC), a
//     partial close two bars after the entry, another two bars later, a full
//     close two bars after that; a: 40/10 (1.2 -> 1, 0.2 -> 1), b: 60/50
//     (1.8 -> 1, 1 -> 1), c: 30/30 (0.9 -> 1, 0.6 -> 1): every one of the 46
//     cycles closes 1 + 1 + 1 and the range-end row is the 3-contract open
//     position of the last bar. The engine closed the raw fraction (0.6 then
//     0.42 of p181342x's two contracts, 0.98 carried); compute_close_target_
//     qty now routes qty_percent through apply_percent_exit_qty_step. The
//     tapes hold no strategy.exit(qty_percent=) leg: that rule (already the
//     same helper) is not re-pinned here.
// (2)+(3) the futures D period -- ledger log-20260905t123531z-7fe6b95a, tape
//     scratchpad/r7/pins/o-cme-dayanchor-full (NQ1! 15m 2025-04-01 ..
//     2026-05-01, 76590 trades, qty-encoded time("D") / time("1D") / ta.change
//     / timeframe.change("1D") / ta.vwap(hlc3) x 4 on every even bar):
//     TradingView's D on a CME 15m chart is the exchange's trade-date daily
//     bar -- the registry 1D feed's rows -- so the 17:00 CT reopen after a
//     holiday early close is NOT a new day. Memorial Day 2025: the bar
//     stamped Sun 05-25 17:00 CT runs through the Mon 05-26 12:00 pause, the
//     Mon 17:00 reopen and Tue 05-27 16:00; time("D") on the reopen reads
//     Sun 17:00, timeframe.change("1D") is false there and true on Tue 17:00,
//     ta.vwap keeps cumulating. The engine reset its session-day clock at
//     every 17:00 CT open; with the native "D" feed installed on an intraday
//     chart it now keys the chart-level D consumers on the feed's stamps
//     (timeframe.hpp NativeDayPartition), and without the feed nothing
//     changes (the control below: today's rule, bar for bar).
//
// The tape's per-bar ta.vwap x 4 agrees with the registry bars on 171 of
// the 176 even bars of the window; the five that differ by one quarter (all
// in the window's first session, Thu 05-22 17:00 .. Fri 07:30 CT, a
// registry-vs-TradingView early-session volume residual unrelated to the
// anchor) are allowed one unit of slack and only there.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "test_o_close_pct_day_anchor_data.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_NATIVE_DAY_PARTITION_V1
#error "requires the chart-level native daily partition feature probe"
#endif

namespace {

int checks = 0;
int failures = 0;

#define CHECK(cond, tag)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, (tag));    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

const double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kMinute = 60000;

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}
// America/Chicago is CDT (UTC-5) through both windows (May, August 2025).
int64_t cdt(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 5, mi); }

bool near(double a, double b, double eps = 1e-9) { return std::abs(a - b) < eps; }

template <std::size_t N>
std::vector<Bar> vec(const Bar (&arr)[N]) {
    return std::vector<Bar>(arr, arr + N);
}

// ---------------------------------------------------------------------------
// (4) strategy.close qty_percent on CME_MINI:NQ1! 15m
// ---------------------------------------------------------------------------

// The tapes' strategy() header: initial_capital 1,500,000, pyramiding 0,
// percent_of_equity 100, process_orders_on_close, no commission / slippage,
// TradingView's default 100% margin; NQ: 20 USD per point, tick 0.25, whole
// contracts (qty_step 1, what the harness injects for the lane).
class CloseProbe : public BacktestEngine {
public:
    CloseProbe() {
        initial_capital_ = 1500000.0;
        syminfo_.pointvalue = 20.0;
        syminfo_.mintick = 0.25;
        syminfo_mintick_ = 0.25;
        qty_step_ = 1.0;
        syminfo_.qty_step = 1.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = true;
        set_syminfo_timezone("America/Chicago");
        set_syminfo_session("1700-1600");
        set_syminfo_type("futures");
    }
    std::function<void(CloseProbe&, int)> script;
    void on_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }
    bool is_long() const { return position_side_ == PositionSide::LONG; }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    double qty() const { return position_qty_; }
    void no_lot_step() { qty_step_ = 0.0; syminfo_.qty_step = 0.0; }
    void fixed_qty(double q) {
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = q;
    }
    void close_pct(double pct) { strategy_close("LONG", "", kNaN, pct, false); }
    void entry_long() { strategy_entry("LONG", true); }
    void close_long() { strategy_close("LONG"); }
};

// The tapes' script: k = bar_index % 8; entry at k == 0 when flat, close
// qty_percent = P1 at k == 2, P2 at k == 4, a full close at k == 6.
void tape_script(CloseProbe& e, int bar, double p1, double p2) {
    const int k = bar % 8;
    if (k == 0 && e.flat()) e.entry_long();
    if (k == 2 && e.is_long()) e.close_pct(p1);
    if (k == 4 && e.is_long()) e.close_pct(p2);
    if (k == 6 && e.is_long()) e.close_long();
}

void check_tape(const CloseProbe& p, const char* tag) {
    const int n_tape = static_cast<int>(sizeof(o_data::kQtyPctTape) / sizeof(o_data::kQtyPctTape[0]));
    CHECK(n_tape == 139, "the tape holds 139 trades");
    CHECK(p.report_trade_count() == n_tape, tag);
    std::printf("    %s: %d closed + %d range-end rows (tape %d)\n", tag,
                p.trade_count(), p.report_trade_count() - p.trade_count(), n_tape);
    const int n = std::min(p.report_trade_count(), n_tape);
    int mismatched = 0;
    for (int i = 0; i < n; ++i) {
        const Trade& t = p.get_report_trade(i);
        const o_data::TapeTrade& w = o_data::kQtyPctTape[i];
        const bool ok = t.is_long && t.entry_time == w.entry_ms && t.exit_time == w.exit_ms
            && near(t.qty, w.qty) && near(t.entry_price, w.entry_price)
            && near(t.exit_price, w.exit_price) && near(t.pnl, w.pnl, 1e-6)
            && t.open_at_end == w.range_end;
        if (!ok && mismatched < 5) {
            std::printf("    row %d: engine entry %lld @%.2f qty %.4f exit %lld @%.2f pnl %.4f%s | tape entry %lld @%.2f qty %g exit %lld @%.2f pnl %g%s\n",
                        i, (long long)t.entry_time, t.entry_price, t.qty,
                        (long long)t.exit_time, t.exit_price, t.pnl,
                        t.open_at_end ? " (range end)" : "",
                        (long long)w.entry_ms, w.entry_price, w.qty,
                        (long long)w.exit_ms, w.exit_price, w.pnl,
                        w.range_end ? " (range end)" : "");
        }
        if (!ok) ++mismatched;
    }
    CHECK(mismatched == 0, tag);
}

void test_qty_percent_tapes() {
    std::printf("-- (4) o-nq-qtypct-{a,b,c}: 3 contracts close 1 + 1 + 1, 139 rows each --\n");
    const std::vector<Bar> bars = vec(o_data::kNq15Aug);
    CHECK(bars.size() == 369, "369 chart bars 2025-08-11 00:00Z .. 08-15 00:00Z");
    const double designs[3][2] = {{40.0, 10.0}, {60.0, 50.0}, {30.0, 30.0}};
    const char* tags[3] = {"a: 40/10 (1.2 -> 1, 0.2 -> 1)", "b: 60/50 (1.8 -> 1, 1 -> 1)",
                           "c: 30/30 (0.9 -> 1, 0.6 -> 1)"};
    for (int d = 0; d < 3; ++d) {
        CloseProbe p;
        const double p1 = designs[d][0], p2 = designs[d][1];
        p.script = [p1, p2](CloseProbe& e, int bar) { tape_script(e, bar, p1, p2); };
        p.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(p.last_error().empty(), p.last_error().c_str());
        check_tape(p, tags[d]);
        // Every cycle: qty 1, 1, 1; the last bar's entry is the range-end row.
        int ones = 0;
        for (int i = 0; i < p.trade_count(); ++i) if (near(p.get_trade(i).qty, 1.0)) ++ones;
        CHECK(ones == p.trade_count() && p.trade_count() == 138, "138 closed rows of one contract");
        CHECK(p.report_trade_count() == 139
                  && near(p.get_report_trade(138).qty, 3.0)
                  && p.get_report_trade(138).open_at_end
                  && p.get_report_trade(138).entry_time == utc_ms(2025, 8, 15, 0, 0),
              "the 3-contract entry on the last bar is the range-end row");
    }
}

// p181342x's shape: two contracts, strategy.close(qty_percent = 30) twice.
// TradingView closes 1 (0.6 -> 1) then 1 (0.3 -> 1) and is flat; the raw
// fraction closed 0.6 then 0.42 and carried 0.98. Without a lot step (the
// corpus default qty_step 0) the fraction is still what closes.
void test_two_contracts_thirty_percent() {
    std::printf("-- (4) two contracts, close 30%% twice: 1 + 1 and flat; no lot step keeps 0.6 / 0.42 --\n");
    const std::vector<Bar> bars = vec(o_data::kNq15Aug);
    {
        CloseProbe p;
        p.fixed_qty(2.0);
        p.script = [](CloseProbe& e, int bar) {
            if (bar == 0) e.entry_long();
            if (bar == 2) e.close_pct(30.0);
            if (bar == 4) e.close_pct(30.0);
            if (bar == 6) e.close_long();
        };
        p.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(p.last_error().empty(), p.last_error().c_str());
        CHECK(p.trade_count() == 2, "two closed rows");
        if (p.trade_count() >= 2) {
            CHECK(near(p.get_trade(0).qty, 1.0) && p.get_trade(0).exit_time == bars[2].timestamp,
                  "0.6 of two contracts closes one lot on bar 2");
            CHECK(near(p.get_trade(1).qty, 1.0) && p.get_trade(1).exit_time == bars[4].timestamp,
                  "0.3 of the remaining contract closes the minimum one lot on bar 4");
        }
        CHECK(p.flat() || p.report_trade_count() == 2, "flat after the second partial: bar 6 has nothing to close");
    }
    {
        CloseProbe p;
        p.fixed_qty(2.0);
        p.no_lot_step();
        p.script = [](CloseProbe& e, int bar) {
            if (bar == 0) e.entry_long();
            if (bar == 2) e.close_pct(30.0);
            if (bar == 4) e.close_pct(30.0);
            if (bar == 6) e.close_long();
        };
        p.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(p.last_error().empty(), p.last_error().c_str());
        CHECK(p.trade_count() == 3, "three closed rows without a lot step");
        if (p.trade_count() >= 3) {
            CHECK(near(p.get_trade(0).qty, 0.6), "qty_step 0: 30% of 2 closes 0.6 (today's rule)");
            CHECK(near(p.get_trade(1).qty, 0.42), "qty_step 0: 30% of 1.4 closes 0.42");
            CHECK(near(p.get_trade(2).qty, 0.98), "qty_step 0: the full close takes the 0.98 carry");
        }
    }
    // A fractional lot step floors to the step and keeps no one-lot minimum
    // (the strategy.exit dust rule): 30% of 2 on a 0.0001 grid is 0.6.
    {
        CloseProbe p;
        p.fixed_qty(2.0);
        p.no_lot_step();
        p.set_syminfo_metadata("qty_step", 0.0001);
        p.script = [](CloseProbe& e, int bar) {
            if (bar == 0) e.entry_long();
            if (bar == 2) e.close_pct(30.0);
            if (bar == 6) e.close_long();
        };
        p.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(p.trade_count() == 2 && near(p.get_trade(0).qty, 0.6),
              "qty_step 0.0001: 30% of 2 closes 0.6 (floor to the step, no lot minimum)");
    }
}

// ---------------------------------------------------------------------------
// (2)+(3) the futures D period on CME_MINI:NQ1! 15m, Memorial Day 2025
// ---------------------------------------------------------------------------

struct DayRow {
    int64_t time_d = 0;       // time("D")
    int64_t time_close_d = 0; // time_close("D")
    int64_t time_w = 0;       // time("W")
    bool chg = false;         // ta.change(time("D")) != 0
    bool tfc = false;         // timeframe.change("1D")
    double vwap = kNaN;       // ta.vwap(hlc3)
    double hlc3 = kNaN;
    int64_t other_clock_d = 0; // session_period_open_ms on another symbol's clock
};

// What the generated strategy body reads per chart bar: the symbol-clock
// pine_time / pine_time_close forms (script_tf, syminfo tz + session),
// tf_change(prev_bar_timestamp_, ...) and ta::VWAP's session anchor.
class DayProbe : public BacktestEngine {
public:
    std::map<int64_t, DayRow> rows;
    ta::VWAP vwap;
    int64_t prev_time_d = 0;

    void on_bar(const Bar& bar) override {
        DayRow r;
        const int64_t ts = current_bar_.timestamp;
        r.time_d = pine_time(ts, "D", "", "", script_tf_, syminfo_.timezone, syminfo_.session);
        r.time_close_d = pine_time_close(ts, "D", "", "", script_tf_, syminfo_.timezone, syminfo_.session);
        r.time_w = pine_time(ts, "W", "", "", script_tf_, syminfo_.timezone, syminfo_.session);
        r.tfc = tf_change(prev_bar_timestamp_, ts, "1D", syminfo_.timezone, syminfo_.session);
        r.chg = prev_time_d != 0 && r.time_d != prev_time_d;
        r.hlc3 = (bar.high + bar.low + bar.close) / 3.0;
        r.vwap = vwap.compute(r.hlc3, bar.volume, ts, syminfo_.timezone, syminfo_.session);
        r.other_clock_d = session_period_open_ms(ts, "America/New_York", "0930-1600",
                                                 CalendarPeriod::DAY);
        rows[ts] = r;
        prev_time_d = r.time_d;
    }
    const DayRow& at(int64_t ts) const {
        static const DayRow none;
        const auto it = rows.find(ts);
        return it == rows.end() ? none : it->second;
    }
};

void run_nq15(DayProbe& probe, const std::vector<Bar>& chart, bool with_daily) {
    probe.set_syminfo_timezone("America/Chicago");
    probe.set_syminfo_session("1700-1600");
    probe.set_syminfo_type("futures");
    if (with_daily) {
        const std::vector<Bar> daily = vec(o_data::kNq1DMay);
        CHECK(probe.set_native_security_feed("D", daily.data(), static_cast<int>(daily.size())),
              "native daily feed installs");
    }
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
    CHECK(active_native_day_partition() == nullptr, "the partition is cleared after run()");
}

const int64_t kSun0525 = cdt(2025, 5, 25, 17, 0);   // the merged bar's stamp
const int64_t kMon0526 = cdt(2025, 5, 26, 17, 0);   // the holiday reopen
const int64_t kTue0527 = cdt(2025, 5, 27, 17, 0);   // the next trade date's open
const int64_t kThu0522 = cdt(2025, 5, 22, 17, 0);   // a regular weekday open
const int64_t kFri0523 = cdt(2025, 5, 23, 17, 0);   // the Friday 17:00 CT open (a weekend session-day)

void test_day_anchor_against_tape() {
    std::printf("-- (2)+(3) o-cme-dayanchor-full, Memorial Day 2025 with the native daily feed --\n");
    const std::vector<Bar> chart = vec(o_data::kNq15May);
    CHECK(chart.size() == 444, "444 chart bars Wed 05-21 17:00 CT .. Wed 05-28 15:45 CT");
    DayProbe probe;
    run_nq15(probe, chart, true);
    CHECK(probe.chart_day_partition_installed(), "the chart partition is built from the D feed");
    CHECK(probe.rows.size() == chart.size(), "one row per chart bar");

    const int n_tape = static_cast<int>(sizeof(o_data::kDayAnchorTape) / sizeof(o_data::kDayAnchorTape[0]));
    CHECK(n_tape == 176, "176 even tape bars in the window");
    int bad_t = 0, bad_chg = 0, bad_tfc = 0, bad_v = 0, slack = 0;
    for (int i = 0; i < n_tape; ++i) {
        const o_data::DayAnchorRow& w = o_data::kDayAnchorTape[i];
        const auto it = probe.rows.find(w.ts);
        if (it == probe.rows.end()) { ++bad_t; continue; }
        const DayRow& r = it->second;
        if (r.time_d != w.time_d) ++bad_t;
        if (r.chg != w.chg) ++bad_chg;
        if (r.tfc != w.tfc) ++bad_tfc;
        const long got = std::lround(r.vwap * 4.0);
        if (got != w.vwap_x4) {
            // The first session's five known one-quarter residuals.
            if (w.ts < kFri0523 && std::labs(got - w.vwap_x4) == 1) ++slack;
            else ++bad_v;
        }
        if ((r.time_d != w.time_d || r.chg != w.chg || r.tfc != w.tfc) && bad_t + bad_chg + bad_tfc <= 5) {
            std::printf("    %lld: time(D) %lld vs %lld, chg %d vs %d, tfc %d vs %d\n",
                        (long long)w.ts, (long long)r.time_d, (long long)w.time_d,
                        (int)r.chg, (int)w.chg, (int)r.tfc, (int)w.tfc);
        }
    }
    std::printf("    time(D) misses %d, ta.change misses %d, timeframe.change misses %d, vwap misses %d (+%d quarter-slack in the first session)\n",
                bad_t, bad_chg, bad_tfc, bad_v, slack);
    CHECK(bad_t == 0, "time(\"D\") == the tape on every even bar");
    CHECK(bad_chg == 0, "ta.change(time(\"D\")) != 0 == the tape");
    CHECK(bad_tfc == 0, "timeframe.change(\"1D\") == the tape");
    CHECK(bad_v == 0, "round(ta.vwap(hlc3) x 4) == the tape (one quarter of slack on 5 first-session bars)");
    CHECK(slack <= 5, "at most the five known first-session residuals");

    // The named bars.
    CHECK(probe.at(kSun0525).tfc && probe.at(kSun0525).time_d == kSun0525,
          "Sun 05-25 17:00 CT opens the merged D bar");
    CHECK(!probe.at(kMon0526).tfc, "timeframe.change(\"1D\") is false on the Mon 05-26 17:00 CT reopen");
    CHECK(!probe.at(kMon0526).chg, "ta.change(time(\"D\")) is 0 on the reopen");
    CHECK(probe.at(kMon0526).time_d == kSun0525, "time(\"D\") on the reopen reads Sun 05-25 17:00 CT");
    CHECK(probe.at(kMon0526 + 15 * kMinute).time_d == kSun0525, "and on the next bar");
    CHECK(!near(probe.at(kMon0526).vwap, probe.at(kMon0526).hlc3),
          "ta.vwap is not re-anchored on the reopen (not the bar's own hlc3)");
    CHECK(probe.at(kTue0527).tfc && probe.at(kTue0527).time_d == kTue0527,
          "Tue 05-27 17:00 CT opens the next trade date's D bar");
    CHECK(near(probe.at(kTue0527).vwap, probe.at(kTue0527).hlc3),
          "ta.vwap re-anchors on Tue 17:00 CT (the first bar's hlc3)");
    // Non-holiday control inside the same run: the regular weekday opens.
    CHECK(probe.at(kThu0522).tfc && probe.at(kThu0522).time_d == kThu0522,
          "Thu 05-22 17:00 CT opens a regular D bar");
    CHECK(near(probe.at(kThu0522).vwap, probe.at(kThu0522).hlc3), "and re-anchors ta.vwap");
    CHECK(probe.at(cdt(2025, 5, 23, 9, 30)).time_d == kThu0522,
          "Fri 05-23 09:30 CT still reads Thu 17:00 CT");
    CHECK(!probe.at(cdt(2025, 5, 23, 9, 30)).tfc, "no D change inside the regular session");
    // time_close("D") (the engine's derivation, unpinned): the trade day's
    // session close -- Tue 05-27 16:00 CT for the merged bar.
    CHECK(probe.at(kMon0526).time_close_d == cdt(2025, 5, 27, 16, 0) - 1,
          "time_close(\"D\") on the reopen is Tue 05-27 16:00 CT");
    CHECK(probe.at(kThu0522).time_close_d == cdt(2025, 5, 23, 16, 0) - 1,
          "time_close(\"D\") on Thu 17:00 CT is Fri 16:00 CT");
    // time("W"): the week's first native stamp, Sun 05-25 17:00 CT, on
    // every bar of the merged day and on Tuesday's.
    CHECK(probe.at(kMon0526).time_w == kSun0525 && probe.at(kTue0527).time_w == kSun0525,
          "time(\"W\") reads the week's first stamp");
    CHECK(probe.at(kThu0522).time_w == cdt(2025, 5, 18, 17, 0), "the previous week opened Sun 05-18 17:00 CT");
    // Another symbol's clock inside the run reads the nominal calendar.
    CHECK(probe.at(kMon0526).other_clock_d
              == session_period_open_ms(kMon0526, "America/New_York", "0930-1600", CalendarPeriod::DAY),
          "a different tz/session keeps its nominal D open under the partition");
}

// The control: the same chart with no native daily feed keeps today's rule
// bar for bar -- the Mon 05-26 17:00 CT reopen is a new session-day
// (timeframe.change true, time("D") = the reopen, ta.vwap re-anchored) and
// every bar outside the merged session reads exactly what the partitioned
// run reads.
void test_no_native_feed_control() {
    std::printf("-- control: no native daily feed, the reopen is its own session-day --\n");
    const std::vector<Bar> chart = vec(o_data::kNq15May);
    DayProbe with, without;
    run_nq15(with, chart, true);
    run_nq15(without, chart, false);
    CHECK(!without.chart_day_partition_installed(), "no partition without the feed");
    CHECK(without.at(kMon0526).tfc, "control: timeframe.change(\"1D\") true on the reopen");
    CHECK(without.at(kMon0526).chg, "control: ta.change(time(\"D\")) != 0 on the reopen");
    CHECK(without.at(kMon0526).time_d == kMon0526, "control: time(\"D\") = the reopen");
    CHECK(near(without.at(kMon0526).vwap, without.at(kMon0526).hlc3), "control: ta.vwap re-anchored");
    CHECK(without.at(kMon0526).time_close_d == cdt(2025, 5, 27, 16, 0) - 1,
          "control: time_close(\"D\") on the reopen is Tue 16:00 CT (nominal session close)");
    int differing_outside = 0, differing_inside = 0;
    for (const auto& kv : with.rows) {
        const DayRow& a = kv.second;
        const DayRow& b = without.at(kv.first);
        const bool same = a.time_d == b.time_d && a.time_close_d == b.time_close_d
            && a.time_w == b.time_w && a.chg == b.chg && a.tfc == b.tfc
            && near(a.vwap, b.vwap, 1e-9);
        // The merged D bar: Sun 05-25 17:00 CT (its stamp) through Tue 16:00
        // CT -- its Sunday half already reads the merged time_close("D").
        const bool inside = kv.first >= kSun0525 && kv.first < kTue0527;
        if (!same && !inside && differing_outside < 4) {
            std::printf("    outside diff %lld: time(D) %lld/%lld close %lld/%lld W %lld/%lld chg %d/%d tfc %d/%d vwap %.6f/%.6f\n",
                        (long long)kv.first, (long long)a.time_d, (long long)b.time_d,
                        (long long)a.time_close_d, (long long)b.time_close_d,
                        (long long)a.time_w, (long long)b.time_w, (int)a.chg, (int)b.chg,
                        (int)a.tfc, (int)b.tfc, a.vwap, b.vwap);
        }
        if (!same) (inside ? differing_inside : differing_outside)++;
    }
    CHECK(differing_outside == 0, "outside the merged D bar both runs read the same");
    CHECK(differing_inside > 0, "inside it the partition differs (the reopen's D)");
    std::printf("    rows differing: %d inside the merged D bar, %d outside\n",
                differing_inside, differing_outside);
}

// The partition on its own: index, trade day, W group, the last bound, and
// the malformed installs that leave it empty.
void test_partition_unit() {
    std::printf("-- NativeDayPartition unit --\n");
    const std::vector<Bar> chart = vec(o_data::kNq15May);
    const std::vector<Bar> daily = vec(o_data::kNq1DMay);
    std::vector<int64_t> stamps;
    for (const Bar& b : daily) stamps.push_back(b.timestamp);
    NativeDayPartition p;
    CHECK(build_native_day_partition(p, "America/Chicago", "1700-1600", stamps,
                                     chart.data(), static_cast<int>(chart.size())),
          "builds");
    CHECK(p.stamps.size() == daily.size(), "one period per native bar");
    const int k_sun = native_day_partition_index(p, kSun0525);
    CHECK(k_sun >= 0 && p.stamps[(std::size_t)k_sun] == kSun0525, "Sun 17:00 CT indexes its own stamp");
    CHECK(native_day_partition_index(p, kMon0526) == k_sun, "the Mon reopen indexes the Sun stamp");
    CHECK(native_day_partition_index(p, kTue0527 - 1) == k_sun, "through Tue 16:59 CT");
    CHECK(native_day_partition_index(p, kTue0527) == k_sun + 1, "Tue 17:00 CT is the next period");
    CHECK(native_day_partition_index(p, stamps.front() - 1) == -1, "before the first stamp: nominal");
    CHECK(native_day_partition_index(p, p.last_bound) == -1, "at the last bound: nominal");
    CHECK(native_day_partition_index(p, p.last_bound - 1) == (int)stamps.size() - 1, "just before it: the last period");
    // The merged bar's trade day is Tuesday's session-day (Mon 17:00 .. Tue
    // 16:00), i.e. the nominal ordinal of its last chart bar.
    CHECK(p.trade_day[(std::size_t)k_sun] == session_day_index(kTue0527 - 1, "America/Chicago", "1700-1600"),
          "the merged bar's trade day is Tuesday's");
    CHECK(p.week_open[(std::size_t)k_sun] == kSun0525 && p.week_open[(std::size_t)k_sun + 1] == kSun0525,
          "Sun 05-25 and Tue 05-27 share the week opening Sun 05-25 17:00 CT");
    CHECK(p.month_open[(std::size_t)k_sun] == p.month_open[0], "May's month group opens on the first May stamp");
    // With nothing active every function is nominal.
    CHECK(active_native_day_partition() == nullptr, "nothing active by default");
    CHECK(session_period_open_ms(kMon0526, "America/Chicago", "1700-1600", CalendarPeriod::DAY) == kMon0526,
          "nominal: the reopen opens its own D");
    const int64_t before_first = stamps.front() - kMinute;
    const int64_t nominal_before_first =
        session_period_open_ms(before_first, "America/Chicago", "1700-1600", CalendarPeriod::DAY);
    const int64_t nominal_at_bound = session_day_index(p.last_bound, "America/Chicago", "1700-1600");
    {
        NativeDayPartitionScope scope(&p);
        CHECK(active_native_day_partition() == &p, "scope installs");
        CHECK(session_period_open_ms(kMon0526, "America/Chicago", "1700-1600", CalendarPeriod::DAY) == kSun0525,
              "installed: the reopen reads Sun 17:00 CT");
        CHECK(session_period_open_ms(kMon0526, "America/Chicago", "", CalendarPeriod::DAY) != kSun0525,
              "another session string on the same tz is not the partition's clock");
        CHECK(!crosses_boundary(kMon0526 - 15 * kMinute, kMon0526, CalendarPeriod::DAY, "America/Chicago", "1700-1600"),
              "installed: no D boundary at the reopen (the 12:00 pause bar to 17:00)");
        CHECK(crosses_boundary(kTue0527 - 15 * kMinute, kTue0527, CalendarPeriod::DAY, "America/Chicago", "1700-1600"),
              "installed: a D boundary at Tue 17:00 CT");
        CHECK(!tf_change(kMon0526 - 15 * kMinute, kMon0526, "1D", "America/Chicago", "1700-1600"),
              "installed: tf_change(\"1D\") false at the reopen");
        CHECK(session_period_close_ms(kMon0526, "America/Chicago", "1700-1600", CalendarPeriod::DAY) == cdt(2025, 5, 27, 16, 0),
              "installed: the merged D closes Tue 16:00 CT");
        CHECK(session_period_close_ms(kMon0526, "America/Chicago", "1700-1600", CalendarPeriod::WEEK) == cdt(2025, 6, 1, 17, 0),
              "installed: the week closes on the next group's first stamp, Sun 06-01 17:00 CT");
        CHECK(session_period_last_traded_close_ms(kMon0526, "America/Chicago", "1700-1600", CalendarPeriod::WEEK) == cdt(2025, 5, 30, 16, 0),
              "installed: the week's last traded close is Fri 05-30 16:00 CT");
        // Before the first stamp and after the last bound: nominal.
        CHECK(session_period_open_ms(before_first, "America/Chicago", "1700-1600", CalendarPeriod::DAY)
                  == nominal_before_first,
              "before the first stamp the nominal rule answers");
        CHECK(session_day_index(p.last_bound, "America/Chicago", "1700-1600") == nominal_at_bound,
              "at the last bound the nominal ordinal resumes");
    }
    CHECK(active_native_day_partition() == nullptr, "scope restores");
    // Malformed installs leave the partition empty.
    NativeDayPartition bad;
    CHECK(!build_native_day_partition(bad, "America/Chicago", "1700-1600", {}, chart.data(), (int)chart.size()) && bad.empty(),
          "no stamps: empty");
    std::vector<int64_t> unsorted = stamps;
    std::swap(unsorted[1], unsorted[2]);
    CHECK(!build_native_day_partition(bad, "America/Chicago", "1700-1600", unsorted, chart.data(), (int)chart.size()) && bad.empty(),
          "non-increasing stamps: empty");
    CHECK(set_active_native_day_partition(&bad) == nullptr && active_native_day_partition() == nullptr,
          "an empty partition never installs");
}

}  // namespace

int main() {
    test_qty_percent_tapes();
    test_two_contracts_thirty_percent();
    test_day_anchor_against_tape();
    test_no_native_feed_control();
    test_partition_unit();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
