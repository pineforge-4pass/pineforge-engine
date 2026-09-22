/*
 * test_session_islastbar_aggregation.cpp — R5 lanes E25 and E26.
 *
 * Lane E20 (its report, "(f) Findings" 2): since R4 slice C (73817c1d) every
 * in-session bar of an AGGREGATED run — input timeframe finer than the
 * chart's, 1m bars under a 5m or 15m script — read session.islastbar = true.
 * scheduler_update_session_state reads "last" when the next script bar's open
 * is out of session, and a run's final bar, which has no next one, counts as
 * last. PineScheduler::bar found that next open only when the input and chart
 * timeframes are equal (the next retained bar IS the next chart bar); an
 * aggregated run passed none, so every in-session bar looked like the run's
 * final bar.
 *
 * What the flag is, three ways:
 * - The legacy engine (ab9714be src/source/pine_scheduler.cpp): the
 *   chart-timeframe loop (run_simple_bar_loop, :1714-1737) set it when the
 *   next input bar is out of session; the aggregated loop (:1865-1869) set
 *   in_session && barstate.islast, the run's final bar only; the magnified
 *   loop (:1839-1845) never set it.
 * - TradingView (tests/fixtures/session_islastbar, three `lab tv` tapes): the
 *   last chart bar of every session DAY. NYSE:F 15m flags 15:45 ET on 252
 *   days and 12:45 ET on its three half days; session.isfirstbar flags 09:30
 *   ET on all 255; BINANCE:ETHUSDT.P 15m flags 23:45 UTC on all 370 days.
 * - The chart-timeframe path today: the legacy lookahead, untouched.
 *
 * Ruling (adapter; the kernel is not involved): an aggregated chart reads the
 * flag with the chart-timeframe path's own lookahead, on its own next script
 * bar — the label of the next aggregated bucket the retained input holds. The
 * chart a script sees does not depend on the timeframe its bars were fed at,
 * so the aggregated and the chart-timeframe run of the same bars agree bar
 * for bar, plain or magnified. Where out-of-session bars separate two
 * sessions each session's last bar is flagged, as on TradingView; the legacy
 * aggregated rule flagged only the run's final bar. The chart-timeframe rows
 * are the control: they pass unchanged before and after.
 *
 * A stream's realtime bar had the same hole — no retained next bar — so it
 * too read every in-session bar as last. It reads the next script bar on the
 * calendar again, as ab9714be's stream and the batch live tail do (section 5).
 *
 * R5 lane E26 closes what E25 recorded. TradingView draws the boundary at the
 * session DAY: a day ends where the next chart bar belongs to another session
 * day, even when that bar is in session again. The registry's NYSE:F feeds
 * hold regular hours only and a 24x7 feed never leaves its session, so "is
 * the next bar out of session?" never fires between two days on them — both
 * paths used to flag the run's edges alone (1 of 255 NYSE:F last bars, 0 of
 * 255 first bars, 1 of 370 ETH last bars).
 *
 * The rule the three tapes give, bar for bar with no disagreement over their
 * full windows (exec/E26-probes/derive.txt):
 *
 *   session.islastbar  = in session && (the next chart bar is out of session
 *                                       || its session day differs)
 *   session.isfirstbar = in session && (the previous chart bar was out of
 *                                       session || its session day differs)
 *
 * The session day is the ordinal of timeframe.hpp
 * (internal::session_trading_day_index): the exchange-timezone day that rolls
 * at the symbol's day stamp — 09:30 ET on NYSE RTH, midnight on a 24x7
 * symbol, 17:00 ET on a 1700-1700 forex session — not local midnight. Section
 * 6 pins that difference on a session whose day rolls in the middle of the
 * calendar day.
 *
 * The two rules are duals, so the engine reads the second off the first: a
 * session's first bar is the bar after its predecessor's last one. Every path
 * that computes "last" therefore agrees on "first" by construction, and the
 * run's own edges keep their conventions — its first bar has no predecessor
 * in session, and a batch run's final bar has no bar after it at all.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

#ifndef PINEFORGE_E25_FIXTURE_DIR
#error "PINEFORGE_E25_FIXTURE_DIR must name tests/fixtures/session_islastbar"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/session_islastbar/bars.inc"

constexpr std::int64_t kMinute = 60'000;
// 2026-04-07 (Tuesday, EDT) 09:30 America/New_York.
constexpr std::int64_t kTue0930Et = 1775568600000LL;
constexpr std::int64_t kDay = 1440 * kMinute;

const std::string kRth = "0930-1600";
const std::string kNewYork = "America/New_York";

// One source callback, as generated code reads it.
struct Seen {
    std::int64_t ts = 0;
    bool ismarket = false;
    bool isfirstbar = false;
    bool islastbar = false;
};

class SessionHost final : public source::PineStrategyHost {
public:
    SessionHost(const std::string& session, const std::string& timezone) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
    }

    void on_source_bar(const Bar&) override {
        seen.push_back({current_bar_.timestamp, session_ismarket_,
                        session_isfirstbar_, session_islastbar_});
    }

    std::vector<Seen> seen;
};

struct Run {
    std::vector<Seen> seen;
    std::string error;
};

Run run_batch(const std::vector<Bar>& bars, const std::string& session,
              const std::string& timezone, const char* input_tf,
              const char* script_tf, bool magnifier) {
    SessionHost host(session, timezone);
    host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf, magnifier);
    return {host.seen, host.last_error()};
}

Bar flat_bar(std::int64_t ts) {
    Bar b{};
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.5;
    b.volume = 1.0;
    return b;
}

// Flat bars every `step` from `first` through `last`, both included.
std::vector<Bar> ladder(std::int64_t first, std::int64_t last, std::int64_t step) {
    std::vector<Bar> bars;
    for (std::int64_t ts = first; ts <= last; ts += step) bars.push_back(flat_bar(ts));
    return bars;
}

template <std::size_t N>
std::vector<Bar> feed(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& row : rows) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

std::string bits(const std::vector<Seen>& seen, bool Seen::*flag) {
    std::string text;
    for (const Seen& s : seen) text += (s.*flag) ? '1' : '0';
    return text;
}

std::set<std::int64_t> flagged(const std::vector<Seen>& seen, bool Seen::*flag) {
    std::set<std::int64_t> stamps;
    for (const Seen& s : seen)
        if (s.*flag) stamps.insert(s.ts);
    return stamps;
}

bool same_bars(const std::vector<Seen>& a, const std::vector<Seen>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].ts != b[i].ts || a[i].ismarket != b[i].ismarket
            || a[i].isfirstbar != b[i].isfirstbar || a[i].islastbar != b[i].islastbar)
            return false;
    }
    return true;
}

void show(const char* tag, const Run& run) {
    std::printf("    %-34s n=%zu ismarket %s\n", tag, run.seen.size(),
                bits(run.seen, &Seen::ismarket).c_str());
    std::printf("    %-34s      isfirstbar %s\n", "", bits(run.seen, &Seen::isfirstbar).c_str());
    std::printf("    %-34s      islastbar  %s\n", "", bits(run.seen, &Seen::islastbar).c_str());
    if (!run.error.empty()) std::printf("    last_error: %s\n", run.error.c_str());
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" at a fixed UTC offset in hours -> UTC milliseconds.
std::int64_t utc_ms(int y, int mo, int d, int h, int mi, int offset_hours = 0) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - offset_hours) * 60 + mi) * kMinute;
}

// The tape's entry times (UTC+8): every entry fills at the close of a bar
// TradingView flagged, and the tape dates it at that bar's open.
std::set<std::int64_t> tape_flags(const char* slug, std::int64_t first, std::int64_t last) {
    std::ifstream in(std::string(PINEFORGE_E25_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::set<std::int64_t> stamps;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 3 || cell[1].rfind("Entry", 0) != 0) continue;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
        const std::int64_t ts = utc_ms(y, mo, d, h, mi, 8);
        if (ts >= first && ts <= last) stamps.insert(ts);
    }
    return stamps;
}

std::set<std::int64_t> minus(const std::set<std::int64_t>& a, const std::set<std::int64_t>& b) {
    std::set<std::int64_t> out;
    for (std::int64_t ts : a)
        if (!b.count(ts)) out.insert(ts);
    return out;
}

// ── 1. one session close, post-market bars after it ───────────────────────

// 1m bars 15:30 .. 16:14 ET: nine 5m script bars, six in session. The flag
// belongs to 15:55 alone, the bar whose next bar (16:00) is out of session.
// Before this lane the aggregated run flagged all six in-session bars.
void test_one_session_close() {
    std::printf("test_one_session_close\n");
    const auto one_minute = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 404 * kMinute, kMinute);
    const auto five_minute = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 400 * kMinute,
                                    5 * kMinute);
    const Run chart = run_batch(five_minute, kRth, kNewYork, "5", "5", false);
    const Run chart_mag = run_batch(five_minute, kRth, kNewYork, "5", "5", true);
    const Run agg = run_batch(one_minute, kRth, kNewYork, "1", "5", false);
    const Run agg_mag = run_batch(one_minute, kRth, kNewYork, "1", "5", true);

    // The control: the chart-timeframe path, as it has always read it.
    CHECK(chart.error.empty());
    CHECK(bits(chart.seen, &Seen::ismarket) == "111111000");
    CHECK(bits(chart.seen, &Seen::islastbar) == "000001000");
    CHECK(same_bars(chart_mag.seen, chart.seen));

    CHECK(agg.error.empty());
    CHECK(agg_mag.error.empty());
    CHECK(bits(agg.seen, &Seen::islastbar) == "000001000");
    CHECK(same_bars(agg.seen, chart.seen));
    CHECK(same_bars(agg_mag.seen, chart.seen));
    if (!same_bars(agg.seen, chart.seen) || !same_bars(agg_mag.seen, chart.seen)) {
        show("chart 5 -> 5", chart);
        show("aggregated 1 -> 5", agg);
        show("aggregated 1 -> 5, magnifier", agg_mag);
    }
}

// ── 2. two sessions, out-of-session bars between them ─────────────────────

// Tuesday 15:30 .. 16:14 ET, then Wednesday 09:15 .. 09:44 ET (pre-market
// from 09:15). Each session's last bar is flagged: Tuesday 15:55, and
// Wednesday 09:40, the run's final bar. The legacy aggregated rule
// (in_session && barstate.islast) saw only 09:40 — "000000000000001".
void test_two_sessions() {
    std::printf("test_two_sessions\n");
    auto one_minute = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 404 * kMinute, kMinute);
    const auto wednesday = ladder(kTue0930Et + kDay - 15 * kMinute,
                                  kTue0930Et + kDay + 14 * kMinute, kMinute);
    one_minute.insert(one_minute.end(), wednesday.begin(), wednesday.end());
    auto five_minute = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 400 * kMinute,
                              5 * kMinute);
    const auto wednesday_5 = ladder(kTue0930Et + kDay - 15 * kMinute,
                                    kTue0930Et + kDay + 10 * kMinute, 5 * kMinute);
    five_minute.insert(five_minute.end(), wednesday_5.begin(), wednesday_5.end());

    const Run chart = run_batch(five_minute, kRth, kNewYork, "5", "5", false);
    const Run agg = run_batch(one_minute, kRth, kNewYork, "1", "5", false);
    const Run agg_mag = run_batch(one_minute, kRth, kNewYork, "1", "5", true);

    CHECK(chart.error.empty());
    CHECK(bits(chart.seen, &Seen::ismarket) == "111111000000111");
    CHECK(bits(chart.seen, &Seen::isfirstbar) == "100000000000100");
    CHECK(bits(chart.seen, &Seen::islastbar) == "000001000000001");

    CHECK(agg.error.empty());
    CHECK(bits(agg.seen, &Seen::islastbar) == "000001000000001");
    CHECK(same_bars(agg.seen, chart.seen));
    CHECK(same_bars(agg_mag.seen, chart.seen));
    if (!same_bars(agg.seen, chart.seen) || !same_bars(agg_mag.seen, chart.seen)) {
        show("chart 5 -> 5", chart);
        show("aggregated 1 -> 5", agg);
        show("aggregated 1 -> 5, magnifier", agg_mag);
    }
}

// ── 3. TradingView's NYSE:F tapes, on the registry feeds they ran on ──────

// 2025-07-02, 2025-07-03 (half day, last bar 12:45 ET) and 2025-07-07: the
// f-15 lane's 1m finer feed aggregated to 15m, and its 15m chart feed. Both
// paths flag exactly the same bars, and none that TradingView does not.
void test_tv_nyse_f() {
    std::printf("test_tv_nyse_f\n");
    const Run chart = run_batch(feed(kFord15), kRth, kNewYork, "15", "15", false);
    const Run agg = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", false);
    const Run agg_mag = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", true);
    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    CHECK(chart.seen.size() == 66);
    CHECK(same_bars(agg.seen, chart.seen));
    CHECK(same_bars(agg_mag.seen, chart.seen));
    if (chart.seen.size() != 66 || !same_bars(agg.seen, chart.seen)
        || !same_bars(agg_mag.seen, chart.seen)) {
        show("chart 15 -> 15", chart);
        show("aggregated 1 -> 15", agg);
        show("aggregated 1 -> 15, magnifier", agg_mag);
    }
    if (chart.seen.empty()) return;
    const std::int64_t first = chart.seen.front().ts;
    const std::int64_t last = chart.seen.back().ts;
    CHECK(first == utc_ms(2025, 7, 2, 13, 30));
    CHECK(last == utc_ms(2025, 7, 7, 19, 45));

    const auto tv_last = tape_flags("e25-f-islastbar", first, last);
    const auto tv_first = tape_flags("e25-f-isfirstbar", first, last);
    CHECK((tv_last == std::set<std::int64_t>{utc_ms(2025, 7, 2, 19, 45),
                                             utc_ms(2025, 7, 3, 16, 45),
                                             utc_ms(2025, 7, 7, 19, 45)}));
    CHECK((tv_first == std::set<std::int64_t>{utc_ms(2025, 7, 2, 13, 30),
                                              utc_ms(2025, 7, 3, 13, 30),
                                              utc_ms(2025, 7, 7, 13, 30)}));
    for (const Run* run : {&chart, &agg}) {
        const auto engine_last = flagged(run->seen, &Seen::islastbar);
        const auto engine_first = flagged(run->seen, &Seen::isfirstbar);
        // No bar TradingView leaves unflagged.
        CHECK(minus(engine_last, tv_last).empty());
        CHECK(minus(engine_first, tv_first).empty());
        // expectation corrected (lane E26): the residual E25 recorded here
        // was the day boundary a regular-hours feed hides — 15:45 on 07-02
        // and 12:45 on the 07-03 half day for islastbar, 09:30 on 07-03 and
        // 07-07 for isfirstbar. The session-day rule flags them, so both
        // paths now hold TradingView's set exactly, with nothing left over.
        //   expectation corrected: minus(tv_last, engine_last) ==
        //     {2025-07-02 15:45 ET, 2025-07-03 12:45 ET} -> {}, and
        //   minus(tv_first, engine_first) ==
        //     {2025-07-03 09:30 ET, 2025-07-07 09:30 ET} -> {},
        //   because a session day ends where the next bar's session day
        //   differs, not only where the next bar leaves the session.
        CHECK(minus(tv_last, engine_last).empty());
        CHECK(minus(tv_first, engine_first).empty());
        CHECK(engine_last == tv_last);
        CHECK(engine_first == tv_first);
    }
}

// ── 4. TradingView's 24x7 tape ────────────────────────────────────────────

// BINANCE:ETHUSDT.P 2025-06-10 23:00 .. 06-11 00:59 UTC: the corpus 1m feed
// aggregated to 15m, and the 15m feed derived from it. Both paths agree; a
// 24x7 feed never leaves its session, so neither sees the midnight boundary.
void test_tv_eth_24x7() {
    std::printf("test_tv_eth_24x7\n");
    const Run chart = run_batch(feed(kEth15), "24x7", "UTC", "15", "15", false);
    const Run agg = run_batch(feed(kEth1m), "24x7", "UTC", "1", "15", false);
    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    CHECK(chart.seen.size() == 8);
    CHECK(same_bars(agg.seen, chart.seen));
    if (chart.seen.size() != 8 || !same_bars(agg.seen, chart.seen)) {
        show("chart 15 -> 15", chart);
        show("aggregated 1 -> 15", agg);
    }
    if (chart.seen.empty()) return;
    const std::int64_t first = chart.seen.front().ts;
    const std::int64_t last = chart.seen.back().ts;
    const auto tv_last = tape_flags("e25-eth-islastbar", first, last);
    CHECK((tv_last == std::set<std::int64_t>{utc_ms(2025, 6, 10, 23, 45)}));
    for (const Run* run : {&chart, &agg}) {
        // expectation corrected: "00000001" -> "00010001", because a 24x7
        // symbol's session day ends at midnight and 23:45 is the last 15m bar
        // of 2025-06-10 — TradingView's own flag. 00:45 stays flagged on top
        // of it: it is this run's final bar, and no bar follows it (the
        // run-end convention both paths share, outside the tape's window).
        CHECK(bits(run->seen, &Seen::islastbar) == "00010001");
        CHECK((flagged(run->seen, &Seen::islastbar)
               == std::set<std::int64_t>{utc_ms(2025, 6, 10, 23, 45),
                                         utc_ms(2025, 6, 11, 0, 45)}));
        // Every bar TradingView flags in this window, the engine flags.
        CHECK(minus(tv_last, flagged(run->seen, &Seen::islastbar)).empty());
        // The session day also starts the run's 00:00 bar.
        CHECK((flagged(run->seen, &Seen::isfirstbar)
               == std::set<std::int64_t>{utc_ms(2025, 6, 10, 23, 0),
                                         utc_ms(2025, 6, 11, 0, 0)}));
    }
}

// ── 5. a stream's realtime bars ───────────────────────────────────────────

// A realtime bar has no retained next bar either, and it is not the run's
// final bar: the next script bar opens one bar width on, on the calendar
// (ab9714be pine_stream.cpp:458-469, the rule the batch live tail reads).
// Warmup 15:30 .. 15:45 ET, realtime ticks 15:50 .. 15:59: the realtime 15:50
// is not the session's last bar, 15:55 is. Since R4 slice C both realtime bars
// were flagged.
//
// The final WARMUP bar is not a run end either (lane E25 finding 3). A stream
// replays its warmup and then keeps going, so the bar after 15:45 is 15:50 on
// the calendar, exactly as it is for the realtime bar that follows it and for
// the batch live tail. Only a batch run that stops there reads "last" from
// having no next bar. TradingView agrees: the tapes flag the last bar of the
// session DAY (15:55 here), never the bar a chart happens to be drawn up to.
void test_stream_realtime() {
    std::printf("test_stream_realtime\n");
    struct Shape {
        const char* tag;
        const char* input_tf;
        std::int64_t step;
        std::int64_t warmup_last;
    };
    const Shape shapes[] = {
        {"stream 5 -> 5", "5", 5 * kMinute, kTue0930Et + 375 * kMinute},
        {"stream 1 -> 5", "1", kMinute, kTue0930Et + 379 * kMinute},
    };
    for (const Shape& shape : shapes) {
        SessionHost host(kRth, kNewYork);
        const auto warmup = ladder(kTue0930Et + 360 * kMinute, shape.warmup_last, shape.step);
        CHECK(host.stream_begin(warmup.data(), static_cast<int>(warmup.size()),
                                shape.input_tf, "5"));
        std::uint64_t sequence = 0;
        for (std::int64_t ts = kTue0930Et + 380 * kMinute; ts < kTue0930Et + 390 * kMinute;
             ts += kMinute) {
            CHECK(host.stream_push_tick(TradeTick{ts + 1000, ++sequence, 100.0, 1.0}));
        }
        CHECK(host.stream_advance_time(kTue0930Et + 395 * kMinute));
        CHECK(host.stream_end(false));
        const Run run{host.seen, host.last_error()};
        CHECK(run.error.empty());
        CHECK(bits(run.seen, &Seen::ismarket) == "111111");
        // expectation corrected: "000101" -> "000001", because the warmup's
        // final bar (15:45) is not the end of the run — the stream continues
        // into the realtime ticks — so it reads its next script bar on the
        // calendar like every other bar of a stream, and 15:55 is the only
        // last bar of this session.
        CHECK(bits(run.seen, &Seen::islastbar) == "000001");
        if (bits(run.seen, &Seen::islastbar) != "000001") show(shape.tag, run);
    }
}

// ── 6. the session day rolls at the symbol's day stamp, not at midnight ───

// A 1700-1700 forex session never leaves the market, and its session day
// rolls at 17:00 exchange time (timeframe.cpp session_day_stamp_offset_minutes
// — the same stamp its daily bar opens on). Tuesday 16:30 .. 17:30 ET at 5m:
// every bar is in session and no local midnight falls in the range, so a rule
// reading "next bar out of session", and equally a rule splitting at local
// midnight, flags nothing here but the run's final bar. The session-day rule
// flags 16:55, the last bar before the roll, and calls 17:00 the next session
// day's first bar.
void test_overnight_session_day_roll() {
    std::printf("test_overnight_session_day_roll\n");
    const std::string forex = "1700-1700";
    const auto five_minute = ladder(kTue0930Et + 420 * kMinute,
                                    kTue0930Et + 480 * kMinute, 5 * kMinute);
    const auto one_minute = ladder(kTue0930Et + 420 * kMinute,
                                   kTue0930Et + 484 * kMinute, kMinute);
    const Run chart = run_batch(five_minute, forex, kNewYork, "5", "5", false);
    const Run agg = run_batch(one_minute, forex, kNewYork, "1", "5", false);
    const Run agg_mag = run_batch(one_minute, forex, kNewYork, "1", "5", true);

    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    CHECK(chart.seen.size() == 13);
    //                                            16:55            17:30
    CHECK(bits(chart.seen, &Seen::ismarket)   == "1111111111111");
    CHECK(bits(chart.seen, &Seen::islastbar)  == "0000010000001");
    CHECK(bits(chart.seen, &Seen::isfirstbar) == "1000001000000");
    CHECK(same_bars(agg.seen, chart.seen));
    CHECK(same_bars(agg_mag.seen, chart.seen));
    if (bits(chart.seen, &Seen::islastbar) != "0000010000001"
        || !same_bars(agg.seen, chart.seen) || !same_bars(agg_mag.seen, chart.seen)) {
        show("chart 5 -> 5", chart);
        show("aggregated 1 -> 5", agg);
        show("aggregated 1 -> 5, magnifier", agg_mag);
    }
    if (chart.seen.size() != 13) return;
    // The roll, spelled out: 16:55 is the last bar of one session day and
    // 17:00 the first of the next, both in session, on the same local date.
    CHECK(chart.seen[5].ts == kTue0930Et + 445 * kMinute);
    CHECK(chart.seen[5].islastbar);
    CHECK(chart.seen[6].ts == kTue0930Et + 450 * kMinute);
    CHECK(chart.seen[6].isfirstbar);
    CHECK(!chart.seen[6].islastbar);
}

}  // namespace

int main() {
    test_one_session_close();
    test_two_sessions();
    test_tv_nyse_f();
    test_tv_eth_24x7();
    test_stream_realtime();
    test_overnight_session_day_roll();

    std::printf("\nsession_islastbar_aggregation: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
