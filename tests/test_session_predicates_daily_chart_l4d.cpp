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

// test_session_predicates_daily_chart.cpp — session.ismarket / ispremarket /
// ispostmarket / isfirstbar / islastbar on a DAILY-OR-HIGHER chart.
//
// TradingView ground truth (Pine reference, Sessions): on "1D" and above
// session.ismarket is true on every bar and session.ispremarket /
// session.ispostmarket are false — a daily bar covers whole session days,
// not a time of day. Intraday charts keep the time-of-day test unchanged.
//
// Evidence: roi10x-shiva-lt-ls-blend on OANDA:XAUUSD @1D. The symbol's
// session is 1800-1700 America/New_York and its daily bars are stamped at
// the 17:00 ET break (minute 1020, outside the wrapped window [1080, 1020)),
// so the time-of-day test never held, every signal ANDed with
// session.ismarket stayed false, and the engine took 0 trades against
// TradingView's 57.
//
// Codegen lowers the three predicates to the UNQUALIFIED call
//   pine_session_ismarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp)
// inside the generated `class GeneratedStrategy : public BacktestEngine`
// (pineforge_codegen/codegen/visit_expr.py). The harness below makes the
// same unqualified calls from a BacktestEngine subclass, so it proves the
// class-scope members (engine.hpp) shadow the namespace-scope time-of-day
// forms for the emitted code — if that shadowing ever broke, the daily
// assertions here would fail at runtime.

#include <cstdio>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/timeframe.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

namespace {

// One dispatched chart bar as the generated strategy would observe it.
struct SeenBar {
    int64_t ts = 0;
    // The three predicates exactly as codegen emits them (unqualified).
    bool ismarket = false;
    bool ispremarket = false;
    bool ispostmarket = false;
    // The pump's own per-bar state (session.isfirstbar / islastbar lower
    // to these members directly).
    bool engine_ismarket = false;
    bool isfirstbar = false;
    bool islastbar = false;
    // The raw time-of-day forms for the same stamp (namespace-scope,
    // three arguments) — what the pump evaluated before the chart rule.
    bool raw_ismarket = false;
    bool raw_ispremarket = false;
    bool raw_ispostmarket = false;
};

class SessionProbeEngine : public pineforge::source::PineStrategyHost {
public:
    std::vector<SeenBar> seen;

    void on_source_bar(const Bar&) override {
        SeenBar s;
        s.ts = current_bar_.timestamp;
        // Byte-for-byte the expressions visit_expr.py emits for
        // session.ismarket / session.ispremarket / session.ispostmarket.
        s.ismarket = pine_session_ismarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.ispremarket = pine_session_ispremarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.ispostmarket = pine_session_ispostmarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.engine_ismarket = session_ismarket_;
        s.isfirstbar = session_isfirstbar_;
        s.islastbar = session_islastbar_;
        s.raw_ismarket = pineforge::pine_session_ismarket(
            syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.raw_ispremarket = pineforge::pine_session_ispremarket(
            syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.raw_ispostmarket = pineforge::pine_session_ispostmarket(
            syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        seen.push_back(s);
    }
};

Bar make_bar(int64_t ts_ms) {
    Bar b;
    b.open = 100.0;
    b.high = 101.0;
    b.low = 99.0;
    b.close = 100.5;
    b.volume = 1000.0;
    b.timestamp = ts_ms;
    return b;
}

constexpr int64_t kMinuteMs = 60'000LL;
constexpr int64_t kHourMs = 60 * kMinuteMs;
constexpr int64_t kDayMs = 24 * kHourMs;

// OANDA:XAUUSD: session 1800-1700 America/New_York. The week of
// 2026-04-06 (Mon) .. 2026-04-10 (Fri) is EDT (UTC-4).
const std::string kXauSession = "1800-1700";
const std::string kNyTz = "America/New_York";
// 2026-04-06 17:00 ET == 21:00 UTC — the daily-bar stamp of the tape.
constexpr int64_t kMon_1700_ET = 1775509200000LL;
// 2026-04-07 16:45 ET == 20:45 UTC — base of the 15m ladder.
constexpr int64_t kTue_1645_ET = 1775594700000LL;

// NASDAQ-style regular session.
const std::string kRthSession = "0930-1600";
// 2026-04-06 00:00 UTC (20:00 ET on 2026-04-05) — a midnight-UTC daily stamp.
constexpr int64_t kMon_0000_UTC = 1775433600000LL;
// 2026-04-07 05:00 ET — premarket by time of day.
constexpr int64_t kTue_0500_ET = 1775552400000LL;
// 2026-04-07 19:00 ET — postmarket by time of day.
constexpr int64_t kTue_1900_ET = 1775602800000LL;
// 2026-04-07 10:30 ET — inside RTH.
constexpr int64_t kTue_1030_ET = 1775577000000LL;

std::vector<Bar> daily_bars(int64_t first_ts, int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) bars.push_back(make_bar(first_ts + i * kDayMs));
    return bars;
}

void check_every_bar_is_the_session(const SessionProbeEngine& eng, size_t expect_n) {
    CHECK(eng.last_error().empty());
    if (!eng.last_error().empty())
        std::printf("    last_error: %s\n", eng.last_error().c_str());
    CHECK(eng.seen.size() == expect_n);
    for (const SeenBar& s : eng.seen) {
        CHECK(s.ismarket == true);
        CHECK(s.ispremarket == false);
        CHECK(s.ispostmarket == false);
        CHECK(s.engine_ismarket == true);
        // A D/W/M bar is its whole session: first and last bar at once.
        CHECK(s.isfirstbar == true);
        CHECK(s.islastbar == true);
    }
}

// --- 1800-1700 America/New_York on a 1D feed stamped 17:00 ET -------------

void test_xauusd_1d_feed_every_bar_is_market() {
    std::printf("test_xauusd_1d_feed_every_bar_is_market\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kXauSession);
    eng.set_syminfo_timezone(kNyTz);
    const auto bars = daily_bars(kMon_1700_ET, 5);  // Mon..Fri
    // The cloud caller's shape: input_tf auto-detected ("D"), script_tf
    // the probe's chart timeframe (run_backtest_full -> timeframe overload).
    eng.run(bars.data(), (int)bars.size(), "", "1D");

    check_every_bar_is_the_session(eng, 5);
    // The evidence itself: by time of day every 17:00 ET stamp is OUTSIDE
    // the wrapped [1080, 1020) window, which is why the old pump never
    // saw the market open on this tape.
    for (const SeenBar& s : eng.seen) {
        CHECK(s.raw_ismarket == false);
        CHECK(s.ismarket != s.raw_ismarket);
    }
}

void test_xauusd_1d_feed_explicit_1D_spelling() {
    std::printf("test_xauusd_1d_feed_explicit_1D_spelling\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kXauSession);
    eng.set_syminfo_timezone(kNyTz);
    const auto bars = daily_bars(kMon_1700_ET, 5);
    eng.run(bars.data(), (int)bars.size(), "1D", "1D");
    check_every_bar_is_the_session(eng, 5);
}

void test_xauusd_1d_feed_single_timeframe_run() {
    // The single-timeframe run(bars, n) overload (run_backtest_full takes
    // it only when the caller passes neither a timeframe nor the
    // magnifier). It detects "D" from the stamps, so the generated
    // predicates follow the daily rule here too. Its inline bar loop does
    // not maintain session_isfirstbar_ / session_islastbar_ (pre-existing,
    // untouched by the chart rule), so only the emitted expressions are
    // pinned on this path.
    std::printf("test_xauusd_1d_feed_single_timeframe_run\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kXauSession);
    eng.set_syminfo_timezone(kNyTz);
    const auto bars = daily_bars(kMon_1700_ET, 5);
    eng.run(bars.data(), (int)bars.size());         // detect_timeframe -> "D"
    CHECK(eng.last_error().empty());
    CHECK(eng.seen.size() == 5);
    for (const SeenBar& s : eng.seen) {
        CHECK(s.ismarket == true);
        CHECK(s.ispremarket == false);
        CHECK(s.ispostmarket == false);
        CHECK(s.raw_ismarket == false);
    }
}

// --- the same session on a 15m feed: intraday byte-identical ---------------

void test_xauusd_15m_feed_keeps_time_of_day_rule() {
    std::printf("test_xauusd_15m_feed_keeps_time_of_day_rule\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kXauSession);
    eng.set_syminfo_timezone(kNyTz);
    // 16:45, 17:00, 17:15, 17:30, 17:45, 18:00, 18:15, 18:30 ET (Tue).
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) bars.push_back(make_bar(kTue_1645_ET + i * 15 * kMinuteMs));
    eng.run(bars.data(), (int)bars.size(), "", "15");

    CHECK(eng.last_error().empty());
    CHECK(eng.seen.size() == 8);
    if (eng.seen.size() != 8) return;

    // Intraday: the generated-code call, the pump state and the raw
    // time-of-day form agree on every bar, for all three predicates.
    for (const SeenBar& s : eng.seen) {
        CHECK(s.ismarket == s.raw_ismarket);
        CHECK(s.engine_ismarket == s.raw_ismarket);
        CHECK(s.ispremarket == s.raw_ispremarket);
        CHECK(s.ispostmarket == s.raw_ispostmarket);
    }
    // The pinned values: 16:45 in (last bar before the break), 17:00 ..
    // 17:45 out (the 1700-1800 break), 18:00 in (first bar of the new
    // session day), 18:15 in.
    CHECK(eng.seen[0].ismarket == true);    // 16:45
    CHECK(eng.seen[0].islastbar == true);
    CHECK(eng.seen[1].ismarket == false);   // 17:00
    CHECK(eng.seen[2].ismarket == false);   // 17:15
    CHECK(eng.seen[2].isfirstbar == false);
    CHECK(eng.seen[2].islastbar == false);
    CHECK(eng.seen[3].ismarket == false);   // 17:30
    CHECK(eng.seen[4].ismarket == false);   // 17:45
    CHECK(eng.seen[5].ismarket == true);    // 18:00
    CHECK(eng.seen[5].isfirstbar == true);
    CHECK(eng.seen[5].islastbar == false);
    CHECK(eng.seen[6].ismarket == true);    // 18:15
    CHECK(eng.seen[6].isfirstbar == false);
    CHECK(eng.seen[7].ismarket == true);    // 18:30
}

// --- 0930-1600 on a 1D feed ------------------------------------------------

void test_rth_1d_feed_midnight_utc_stamp() {
    std::printf("test_rth_1d_feed_midnight_utc_stamp\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kRthSession);
    eng.set_syminfo_timezone(kNyTz);
    const auto bars = daily_bars(kMon_0000_UTC, 5);
    eng.run(bars.data(), (int)bars.size(), "D", "D");

    check_every_bar_is_the_session(eng, 5);
    // 00:00 UTC is 20:00 ET — outside RTH by time of day.
    for (const SeenBar& s : eng.seen) CHECK(s.raw_ismarket == false);
}

// --- a daily chart aggregated from an intraday feed ------------------------

void test_xauusd_daily_chart_aggregated_from_60m_feed() {
    std::printf("test_xauusd_daily_chart_aggregated_from_60m_feed\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kXauSession);
    eng.set_syminfo_timezone(kNyTz);
    // 60m bars from Mon 18:00 ET through Wed 17:00 ET (two session days).
    std::vector<Bar> bars;
    const int64_t first = kMon_1700_ET + kHourMs;  // Mon 18:00 ET
    for (int i = 0; i < 48; ++i) bars.push_back(make_bar(first + i * kHourMs));
    eng.run(bars.data(), (int)bars.size(), "60", "D");

    CHECK(eng.last_error().empty());
    CHECK(!eng.seen.empty());
    for (const SeenBar& s : eng.seen) {
        CHECK(s.ismarket == true);
        CHECK(s.ispremarket == false);
        CHECK(s.ispostmarket == false);
        CHECK(s.engine_ismarket == true);
        CHECK(s.isfirstbar == true);
        CHECK(s.islastbar == true);
    }
}

// --- the streaming pump's realtime daily bar -------------------------------

void test_xauusd_1d_stream_realtime_bar() {
    std::printf("test_xauusd_1d_stream_realtime_bar\n");
    SessionProbeEngine eng;
    eng.set_syminfo_session(kXauSession);
    eng.set_syminfo_timezone(kNyTz);
    const auto warmup = daily_bars(kMon_1700_ET, 4);  // Mon..Thu
    const bool began = eng.stream_begin(warmup.data(), (int)warmup.size(), "D", "D");
    CHECK(began);
    if (!began) {
        std::printf("    last_error: %s\n", eng.last_error().c_str());
        return;
    }
    // Friday's bar: a trade at 17:05 ET, then the clock passes Saturday
    // 17:00 ET so the bar finalizes through stream_dispatch_script_bar.
    const int64_t fri_1700 = kMon_1700_ET + 4 * kDayMs;
    CHECK(eng.stream_push_tick(TradeTick{fri_1700 + 5 * kMinuteMs, 1, 100.25, 1.0}));
    CHECK(eng.stream_advance_time(fri_1700 + kDayMs));
    CHECK(eng.stream_end(false));

    check_every_bar_is_the_session(eng, 5);
    for (const SeenBar& s : eng.seen) CHECK(s.raw_ismarket == false);
}

// --- the chart-timeframe forms directly ------------------------------------

void test_chart_tf_forms_direct() {
    std::printf("test_chart_tf_forms_direct\n");
    const char* daily_or_higher[] = {"D", "1D", "2D", "W", "1W", "M", "1M", "3M"};
    for (const char* tf : daily_or_higher) {
        CHECK(tf_is_daily_or_higher(tf));
        // Premarket / postmarket stamps by time of day: the daily rule wins.
        CHECK(pine_session_ismarket(kRthSession, kNyTz, kTue_0500_ET, tf) == true);
        CHECK(pine_session_ismarket(kRthSession, kNyTz, kTue_1900_ET, tf) == true);
        CHECK(pine_session_ismarket(kXauSession, kNyTz, kMon_1700_ET, tf) == true);
        CHECK(pine_session_ispremarket(kRthSession, kNyTz, kTue_0500_ET, tf) == false);
        CHECK(pine_session_ispostmarket(kRthSession, kNyTz, kTue_1900_ET, tf) == false);
    }
    // Controls: those stamps really are pre/post-market by time of day.
    CHECK(pine_session_ispremarket(kRthSession, kNyTz, kTue_0500_ET) == true);
    CHECK(pine_session_ispostmarket(kRthSession, kNyTz, kTue_1900_ET) == true);
    CHECK(pine_session_ismarket(kRthSession, kNyTz, kTue_0500_ET) == false);
    CHECK(pine_session_ismarket(kXauSession, kNyTz, kMon_1700_ET) == false);

    // Intraday and undetected chart timeframes defer to the time-of-day
    // forms byte for byte.
    const char* intraday[] = {"", "1", "15", "60", "240"};
    const int64_t stamps[] = {kTue_0500_ET, kTue_1030_ET, kTue_1900_ET, kMon_1700_ET, kTue_1645_ET};
    for (const char* tf : intraday) {
        CHECK(!tf_is_daily_or_higher(tf));
        for (int64_t ts : stamps) {
            CHECK(pine_session_ismarket(kRthSession, kNyTz, ts, tf)
                  == pine_session_ismarket(kRthSession, kNyTz, ts));
            CHECK(pine_session_ismarket(kXauSession, kNyTz, ts, tf)
                  == pine_session_ismarket(kXauSession, kNyTz, ts));
            CHECK(pine_session_ispremarket(kRthSession, kNyTz, ts, tf)
                  == pine_session_ispremarket(kRthSession, kNyTz, ts));
            CHECK(pine_session_ispostmarket(kRthSession, kNyTz, ts, tf)
                  == pine_session_ispostmarket(kRthSession, kNyTz, ts));
        }
    }
    CHECK(pine_session_ismarket(kRthSession, kNyTz, kTue_1030_ET, "15") == true);
    CHECK(pine_session_ismarket(kRthSession, kNyTz, kTue_1900_ET, "15") == false);
}

}  // namespace

int main() {
    test_xauusd_1d_feed_every_bar_is_market();
    test_xauusd_1d_feed_explicit_1D_spelling();
    test_xauusd_1d_feed_single_timeframe_run();
    test_xauusd_15m_feed_keeps_time_of_day_rule();
    test_rth_1d_feed_midnight_utc_stamp();
    test_xauusd_daily_chart_aggregated_from_60m_feed();
    test_xauusd_1d_stream_realtime_bar();
    test_chart_tf_forms_direct();

    std::printf("\nsession_predicates_daily_chart: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
