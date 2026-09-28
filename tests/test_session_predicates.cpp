/*
 * test_session_predicates.cpp — unit tests for Pine v6 session.is* predicates.
 *
 * Tests cover:
 *  - NYSE RTH bar inside / outside session (session.ismarket)
 *  - Premarket bar (session.ispremarket)
 *  - Postmarket bar (session.ispostmarket)
 *  - First/last session bar transitions across session boundaries
 *  - 24x7 session (crypto) — ismarket always true, pre/post always false
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <pineforge/na.hpp>
#include <pineforge/session_time.hpp>

#include "exit_comment_tape.hpp"

#ifndef PINEFORGE_SESSION_CLOCK_FIXTURE_DIR
#error "PINEFORGE_SESSION_CLOCK_FIXTURE_DIR must name tests/fixtures/session_clock"
#endif

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

// -----------------------------------------------------------------------
// Timestamp helpers
// NYSE RTH: 0930-1600 America/New_York
// 2026-04-07 (Tuesday) chosen — well past DST spring-forward.
//
// UTC offsets for America/New_York on 2026-04-07 (EDT = UTC-4).
//
//  09:30 ET  = 13:30 UTC  => 1775573400000 ms
//  10:30 ET  = 14:30 UTC  => 1775577000000 ms  (inside RTH)
//  16:00 ET  = 20:00 UTC  => 1775592000000 ms  (session close — OUTSIDE by <end convention)
//  17:00 ET  = 21:00 UTC  => 1775595600000 ms  (post-market)
//  06:00 ET  = 10:00 UTC  => 1775559600000 ms  (pre-market)
//  03:30 ET  = 07:30 UTC  => 1775548200000 ms  (before pre-market opens 04:00)
// -----------------------------------------------------------------------
static const std::string NYSE_SESSION = "0930-1600";
static const std::string NYSE_TZ      = "America/New_York";

// 2026-04-07 10:30 ET (inside RTH)
static const int64_t T_INSIDE_RTH     = 1775577000000LL;
// 2026-04-07 20:00 UTC = 16:00 ET (session end, exclusive — outside)
static const int64_t T_RTH_CLOSE_UTC  = 1775592000000LL;
// 2026-04-07 17:00 ET (post-market)
static const int64_t T_POSTMARKET     = 1775595600000LL;
// 2026-04-07 06:00 ET (pre-market, between 04:00 and 09:30)
static const int64_t T_PREMARKET      = 1775559600000LL;
// 2026-04-07 03:30 ET (before pre-market opens at 04:00)
static const int64_t T_BEFORE_PRE     = 1775548200000LL;


static void test_ismarket_inside_rth() {
    std::printf("test_ismarket_inside_rth\n");
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH) == true);
}

static void test_ismarket_outside_rth_close() {
    std::printf("test_ismarket_outside_rth_close\n");
    // 16:00 ET is the session END — exclusive convention means 16:00 itself
    // is OUTSIDE the session window [0930, 1600).
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_RTH_CLOSE_UTC) == false);
}

static void test_ismarket_postmarket_is_false() {
    std::printf("test_ismarket_postmarket_is_false\n");
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET) == false);
}

static void test_ismarket_premarket_is_false() {
    std::printf("test_ismarket_premarket_is_false\n");
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET) == false);
}

static void test_ispremarket_true() {
    std::printf("test_ispremarket_true\n");
    // 06:00 ET is between 04:00 and 09:30 => premarket
    CHECK(pine_session_ispremarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET) == true);
}

static void test_ispremarket_before_0400_false() {
    std::printf("test_ispremarket_before_0400_false\n");
    // 03:30 ET is before 04:00 => NOT premarket
    CHECK(pine_session_ispremarket(NYSE_SESSION, NYSE_TZ, T_BEFORE_PRE) == false);
}

static void test_ispremarket_inside_rth_false() {
    std::printf("test_ispremarket_inside_rth_false\n");
    // Inside RTH => NOT premarket
    CHECK(pine_session_ispremarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH) == false);
}

static void test_ispostmarket_true() {
    std::printf("test_ispostmarket_true\n");
    // 17:00 ET is between 16:00 and 20:00 => postmarket
    CHECK(pine_session_ispostmarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET) == true);
}

static void test_ispostmarket_inside_rth_false() {
    std::printf("test_ispostmarket_inside_rth_false\n");
    // Inside RTH => NOT postmarket
    CHECK(pine_session_ispostmarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH) == false);
}

static void test_ispostmarket_premarket_false() {
    std::printf("test_ispostmarket_premarket_false\n");
    // Premarket time => NOT postmarket
    CHECK(pine_session_ispostmarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET) == false);
}

static void test_24x7_ismarket_always_true() {
    std::printf("test_24x7_ismarket_always_true\n");
    CHECK(pine_session_ismarket("24x7", "UTC", T_PREMARKET) == true);
    CHECK(pine_session_ismarket("24x7", "UTC", T_INSIDE_RTH) == true);
    CHECK(pine_session_ismarket("",     "UTC", T_POSTMARKET) == true);
}

static void test_24x7_prepost_always_false() {
    std::printf("test_24x7_prepost_always_false\n");
    CHECK(pine_session_ispremarket ("24x7", "UTC", T_PREMARKET) == false);
    CHECK(pine_session_ispostmarket("24x7", "UTC", T_POSTMARKET) == false);
    CHECK(pine_session_ispremarket ("",     "UTC", T_PREMARKET) == false);
    CHECK(pine_session_ispostmarket("",     "UTC", T_POSTMARKET) == false);
}

// Test session-boundary transitions using hhmm helpers directly
static void test_hhmm_to_minutes_basic() {
    std::printf("test_hhmm_to_minutes_basic\n");
    CHECK(hhmm_to_minutes("0930") == 9 * 60 + 30);
    CHECK(hhmm_to_minutes("1600") == 16 * 60);
    CHECK(hhmm_to_minutes("0000") == 0);
    CHECK(hhmm_to_minutes("2359") == 23 * 60 + 59);
    CHECK(hhmm_to_minutes("xx")   == -1);
    CHECK(hhmm_to_minutes("2400") == -1);
}

// Test session with weekday filter
static void test_ismarket_weekend_filter() {
    std::printf("test_ismarket_weekend_filter\n");
    // 2026-04-11 (Saturday) 14:30 UTC = 10:30 ET — session hours but filtered out by :23456
    static const int64_t bar_sat = 1775921400000LL;
    CHECK(pine_session_ismarket("0930-1600:23456", NYSE_TZ, bar_sat) == false);
    // Without day filter — should be inside
    CHECK(pine_session_ismarket("0930-1600", NYSE_TZ, bar_sat) == true);
}

// Test first/last bar transitions via passes_session_filter directly
static void test_firstlastbar_transitions() {
    std::printf("test_firstlastbar_transitions\n");
    // Simulate a sequence of bars:
    //   bar0: premarket  (outside session)
    //   bar1: RTH open   (inside session  — isfirstbar=true)
    //   bar2: inside RTH (inside session  — isfirstbar=false)
    //   bar3: post-mkt   (outside session — during prev bar: islastbar=true)
    bool prev_in = false;
    bool in_session;

    // bar0: premarket — not in session
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET);
    CHECK(in_session == false);
    CHECK((in_session && !prev_in) == false);   // not first bar
    prev_in = in_session;

    // bar1: inside RTH — first bar
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH);
    CHECK(in_session == true);
    CHECK((in_session && !prev_in) == true);    // FIRST bar of session
    prev_in = in_session;

    // bar2: still inside RTH — not first bar
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH + 300000LL);  // +5m
    CHECK(in_session == true);
    CHECK((in_session && !prev_in) == false);   // NOT first bar
    prev_in = in_session;

    // bar2 islastbar check: next bar (post-market) is outside session
    bool next_in = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET);
    CHECK(next_in == false);
    CHECK((prev_in && !next_in) == true);       // LAST bar of session (bar2 fires islastbar)

    // bar3: post-market — not in session
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET);
    CHECK(in_session == false);
    prev_in = in_session;
}

// A session window whose start equals its end ("1700-1700" — TradingView's
// spelling of OANDA forex's 24-hour session; "0000-0000") spans the WHOLE
// day. The half-open [start, end) arithmetic used to make it EMPTY, so every
// bar of a forex symbol read session.ismarket == false and time(session) /
// time_close == na (finding 455: a strategy gating its 'Session Close' exit
// on minute(time_close) never fired on EURUSD).
static void test_start_equals_end_is_full_day() {
    const std::string tz = "America/New_York";
    // 2026-04-07 (Tue, EDT = UTC-4), exact epoch ms:
    const int64_t kTs_0930_ET = 1775568600000LL;   // 13:30 UTC
    const int64_t kTs_1030_ET = 1775572200000LL;   // 14:30 UTC
    const int64_t kTs_1515_ET = 1775589300000LL;   // 19:15 UTC
    const int64_t kTs_1630_ET = 1775593800000LL;   // 20:30 UTC
    const int64_t kTs_0500_ET = 1775552400000LL;   // 09:00 UTC (pre-market hours)
    const int64_t kTs_1730_ET = 1775597400000LL;   // 21:30 UTC (post-market hours)
    const int64_t kTs_SAT_1030_ET = 1775917800000LL;   // 2026-04-11 Sat
    // Sweep a full local day at 1-minute grain: every minute is in session.
    int in_1700 = 0, in_0000 = 0, in_days = 0;
    for (int m = 0; m < 1440; ++m) {
        int64_t ts = kTs_0930_ET + static_cast<int64_t>(m) * 60000LL;
        if (pine_session_ismarket("1700-1700", tz, ts)) ++in_1700;
        if (pine_session_ismarket("0000-0000", tz, ts)) ++in_0000;
        if (pine_session_ismarket("1700-1700:1234567", tz, ts)) ++in_days;
    }
    CHECK(in_1700 == 1440);
    CHECK(in_0000 == 1440);
    CHECK(in_days == 1440);
    // time(session) / time_close(session) resolve for every bar instead of na.
    CHECK(pine_time(kTs_1030_ET, "15", "1700-1700", tz, "15") == kTs_1030_ET);
    CHECK(pine_time_close(kTs_1030_ET, "15", "1700-1700", tz, "15")
          == kTs_1030_ET + 15 * 60000LL);
    // 15:15 ET bar on a 15m chart: time_close is 15:30 ET (the exemplar's
    // minute(time_close) >= 30 session-close gate).
    CHECK(pine_time_close(kTs_1515_ET, "15", "1700-1700", tz, "15")
          == kTs_1515_ET + 15 * 60000LL);
    CHECK(pine_time(kTs_1730_ET, "15", "1700-1700", tz, "15") == kTs_1730_ET);
    // A 24-hour session has no pre-/post-market.
    CHECK(!pine_session_ispremarket("1700-1700", tz, kTs_0500_ET));
    CHECK(!pine_session_ispostmarket("1700-1700", tz, kTs_1730_ET));
    CHECK(pine_session_ispremarket("0930-1600", tz, kTs_0500_ET));   // control
    CHECK(pine_session_ispostmarket("0930-1600", tz, kTs_1730_ET));  // control
    // Day-of-week filter still applies: Saturday is out even for 1700-1700.
    CHECK(!pine_session_ismarket("1700-1700:23456", tz, kTs_SAT_1030_ET));
    CHECK(pine_session_ismarket("1700-1700:23456", tz, kTs_1030_ET));
    // Ordinary and wrapped windows are untouched.
    CHECK(pine_session_ismarket("0930-1600", tz, kTs_1030_ET));
    CHECK(!pine_session_ismarket("0930-1600", tz, kTs_1630_ET));
    CHECK(pine_session_ismarket("1700-1600", tz, kTs_1030_ET));
    CHECK(!pine_session_ismarket("1700-1600", tz, kTs_1630_ET));
    CHECK(pine_session_ismarket("1700-1600", tz, kTs_1730_ET));
}

// ---------------------------------------------------------------------------
// A session window's clocks as TradingView reads them (lane
// W11-ENG-TIME-COLOR, tests/fixtures/session_clock/README.md): HHMM is
// HH * 60 + MM minutes after a day's midnight, unchecked -- "2400" is the
// day's end, "2430" 00:30 on the next day, "0060" 01:00 -- an end at or
// before the start is on the next day, and a time of day is in the window
// when it, or it on the next day, falls in [start, end). Every reading of
// every tape is replayed through pine_time / pine_time_close as generated
// code calls them (the chart's timeframe, syminfo.timezone and
// syminfo.session trailing).
// ---------------------------------------------------------------------------

// A spelled reading: "n" for na, else minutes.
static bool reading_is_na(const std::string& text) { return text == "n"; }

// `minutes` is (reference - value) / 60000 as the probe spells it; the engine
// answer `value` (na or epoch ms) must be na exactly when TradingView's was,
// and otherwise the same minutes to a millisecond.
static bool same_reading(const std::string& text, int64_t value, int64_t bar, bool to_close) {
    if (reading_is_na(text)) return is_na(value);
    if (is_na(value)) return false;
    char* end = nullptr;
    const double tv = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    const double engine = static_cast<double>(to_close ? value - bar : bar - value) / 60000.0;
    return std::fabs(engine - tv) < 1e-6;
}

struct ChartFacts {
    const char* slug;
    const char* tf;
    const char* sym_tz;
    const char* sym_session;
};

// The w11 sess2400 probe: a,b,c,d,e,f,g,k|h,i,j (strategy.pine in each tape).
static void test_session_2400_tapes() {
    std::printf("test_session_2400_tapes\n");
    struct Field {
        const char* tf;       // nullptr: timeframe.period
        const char* session;
        const char* tz;
        bool close;           // time_close, spelled from the bar's open
    };
    static const Field fields[] = {
        {nullptr, "0000-2400", "", false},
        {nullptr, "1700-2400", "", false},
        {nullptr, "2045-2400", "Asia/Tokyo", false},
        {"D", "0000-2400", "", false},
        {nullptr, "0000-2400:23456", "", false},
        {nullptr, "1700-2400", "", true},
        {nullptr, "2300-2400,0000-0100", "", false},
        {nullptr, "2000-2400", "America/New_York", false},
        {nullptr, "0000-0000", "", false},
        {nullptr, "0000-2359", "", false},
        {nullptr, "1700-0000", "", false},
    };
    static const ChartFacts charts[] = {
        {"w11-sess2400-btc15", "15", "UTC", "24x7"},
        {"w11-sess2400-btc1d", "1D", "UTC", "24x7"},
        {"w11-sess2400-xau15", "15", "America/New_York", "1800-1700"},
    };
    for (const ChartFacts& chart : charts) {
        bool ok = true;
        const auto readings =
            exit_comment_tape::read(PINEFORGE_SESSION_CLOCK_FIXTURE_DIR, chart.slug, ok);
        CHECK(ok);
        int compared = 0;
        int wrong = 0;
        for (const auto& reading : readings) {
            std::string flat = reading.signal;
            for (char& c : flat) if (c == '|') c = ',';
            const auto spelled = exit_comment_tape::split(flat, ',');
            if (spelled.size() != sizeof(fields) / sizeof(fields[0])) { ++wrong; continue; }
            for (std::size_t k = 0; k < spelled.size(); ++k) {
                const Field& f = fields[k];
                const std::string tf = f.tf ? f.tf : chart.tf;
                const int64_t value = f.close
                    ? pine_time_close(reading.bar_ms, tf, f.session, f.tz, chart.tf,
                                      chart.sym_tz, chart.sym_session)
                    : pine_time(reading.bar_ms, tf, f.session, f.tz, chart.tf,
                                chart.sym_tz, chart.sym_session);
                // Where a D period opens under a session argument is not this
                // clock's rule: on a D chart time(timeframe.period, session,
                // tz) is the session's own day bar (lane W11-ENG-TIME-COLOR's
                // report read it as the bar's time), and there only whether
                // the bar is in the window is compared. time("D", session)
                // keys its day on syminfo.timezone, as TradingView does (lane
                // W12-ENG-TIME, test_session_period_tapes).
                const bool daily_chart = std::string(chart.tf) == "1D";
                const bool session_only = daily_chart && f.tz[0] != '\0';
                const bool same = session_only
                    ? reading_is_na(spelled[k]) == is_na(value)
                    : same_reading(spelled[k], value, reading.bar_ms, f.close);
                ++compared;
                if (!same) {
                    if (++wrong <= 5) {
                        std::printf("  %s bar %lld field %zu (%s) tv=%s engine=%lld\n", chart.slug,
                                    static_cast<long long>(reading.bar_ms), k, f.session,
                                    spelled[k].c_str(), static_cast<long long>(value));
                    }
                }
            }
        }
        std::printf("  %s: %d readings compared, %d differ\n", chart.slug, compared, wrong);
        CHECK(compared > 0);
        CHECK(wrong == 0);
    }
}

// The w11 edge probes: one window each, read on BINANCE:BTCUSDT 15 -- where
// the unchecked clock arithmetic reaches (TradingView accepted every form).
static void test_session_clock_edge_tapes() {
    std::printf("test_session_clock_edge_tapes\n");
    struct Edge { const char* slug; const char* session; };
    static const Edge edges[] = {
        {"w11-edge-24000100-btc15", "2400-0100"},
        {"w11-edge-00002401-btc15", "0000-2401"},
        {"w11-edge-24002400-btc15", "2400-2400"},
        {"w11-edge-00002430-btc15", "0000-2430"},
        {"w11-edge-00002500-btc15", "0000-2500"},
        {"w11-edge-00002360-btc15", "0000-2360"},
        {"w11-edge-24300100-btc15", "2430-0100"},
        {"w11-edge-00000060-btc15", "0000-0060"},
        {"w11-edge-00009959-btc15", "0000-9959"},
        {"w11-edge-17002500-btc15", "1700-2500"},
        {"w11-edge-23302430-btc15", "2330-2430"},
    };
    for (const Edge& edge : edges) {
        bool ok = true;
        const auto readings =
            exit_comment_tape::read(PINEFORGE_SESSION_CLOCK_FIXTURE_DIR, edge.slug, ok);
        CHECK(ok);
        int wrong = 0;
        int in_session = 0;
        for (const auto& reading : readings) {
            const int64_t value =
                pine_time(reading.bar_ms, "15", edge.session, "", "15", "UTC", "24x7");
            if (!reading_is_na(reading.signal)) ++in_session;
            if (!same_reading(reading.signal, value, reading.bar_ms, false)) {
                if (++wrong <= 3) {
                    std::printf("  %s bar %lld tv=%s engine=%lld\n", edge.slug,
                                static_cast<long long>(reading.bar_ms),
                                reading.signal.c_str(), static_cast<long long>(value));
                }
            }
        }
        std::printf("  %s: %zu bars, %d in session, %d differ\n", edge.slug, readings.size(),
                    in_session, wrong);
        CHECK(readings.size() == 191);
        CHECK(wrong == 0);
    }
}

// Which days a session argument admits, TradingView's day list: with no list
// one window admits every day and several windows Monday to Friday only; a
// list is taken as written. A list filters each window by its session day --
// the day the window starts, but the day it ends for a window that wraps past
// midnight: "1700-1700:23456" opens Monday's session on Sunday at 17:00,
// "1800-0200" closes Friday's at 02:00 on Friday, and "2330-2430" keeps
// Friday's 00:00-00:30 tail on Saturday (w11-sessmask{,2,3}-btc15, BINANCE:
// BTCUSDT 15 over a weekend).
static void test_session_day_list_tapes() {
    std::printf("test_session_day_list_tapes\n");
    struct Tape {
        const char* slug;
        std::vector<const char*> sessions;
    };
    const Tape tapes[] = {
        {"w11-sessmask-btc15",
         {"0000-0100,1200-1300", "2300-2400,0000-0100:1234567", "1200-1300",
          "0000-0100,1200-1300:17", "0000-1200,1200-2400", "0000-0100:1234567",
          "0000-0100,0000-0100"}},
        {"w11-sessmask2-btc15",
         {"0930-1130,1300-1500", "0930-1130,1300-1500", "2330-2430,1200-1300",
          "1700-2500,0900-1000", "0930-1130", "1800-0200,1200-1300"}},
        {"w11-sessmask3-btc15",
         {"0000-0000:23456", "1700-1700:23456", "1800-0200:23456", "0900-1700:23456",
          "2330-0030:23456", "2330-2430:23456"}},
    };
    for (const Tape& tape : tapes) {
        bool ok = true;
        const auto readings =
            exit_comment_tape::read(PINEFORGE_SESSION_CLOCK_FIXTURE_DIR, tape.slug, ok);
        CHECK(ok);
        int compared = 0;
        int wrong = 0;
        for (const auto& reading : readings) {
            const auto spelled = exit_comment_tape::split(reading.signal, ',');
            if (spelled.size() != tape.sessions.size()) { ++wrong; continue; }
            for (std::size_t k = 0; k < spelled.size(); ++k) {
                // w11-sessmask2's second field reads its windows in New York.
                const bool new_york = std::string(tape.slug) == "w11-sessmask2-btc15" && k == 1;
                const int64_t value = pine_time(reading.bar_ms, "15", tape.sessions[k],
                                                new_york ? "America/New_York" : "", "15",
                                                "UTC", "24x7");
                ++compared;
                if (!same_reading(spelled[k], value, reading.bar_ms, false)) {
                    if (++wrong <= 5) {
                        std::printf("  %s bar %lld (%s) tv=%s engine=%lld\n", tape.slug,
                                    static_cast<long long>(reading.bar_ms), tape.sessions[k],
                                    spelled[k].c_str(), static_cast<long long>(value));
                    }
                }
            }
        }
        std::printf("  %s: %d readings compared, %d differ\n", tape.slug, compared, wrong);
        CHECK(compared == 575 * static_cast<int>(tape.sessions.size()));
        CHECK(wrong == 0);
    }
}

// What the tapes do not reach, read as before the day-list rule: a day list
// on a 24-hour body ("24x7:23456", ":23456") admits its calendar days, and
// a trailing comma adds no second window, so no Monday-to-Friday default.
static void test_session_day_list_edges() {
    std::printf("test_session_day_list_edges\n");
    const int64_t wed_noon = 1741780800000LL;  // Wed 2025-03-12 12:00Z
    const int64_t sat_noon = 1741435200000LL;  // Sat 2025-03-08 12:00Z
    CHECK(!is_na(pine_time(wed_noon, "15", "24x7:23456", "", "15", "UTC", "24x7")));
    CHECK(is_na(pine_time(sat_noon, "15", "24x7:23456", "", "15", "UTC", "24x7")));
    CHECK(!is_na(pine_time(sat_noon, "15", "24x7:1234567", "", "15", "UTC", "24x7")));
    CHECK(pine_session_ismarket("24x7:23456", "UTC", wed_noon));
    CHECK(!pine_session_ismarket("24x7:23456", "UTC", sat_noon));
    // One window and a trailing comma: every day, as one window reads.
    CHECK(!is_na(pine_time(sat_noon, "15", "1100-1300,", "", "15", "UTC", "24x7")));
    CHECK(!is_na(pine_time(wed_noon, "15", "1100-1300,", "", "15", "UTC", "24x7")));
}

// ---------------------------------------------------------------------------
// The D / W / M period of time() and time_close() under a session argument
// (lane W12-ENG-TIME, tests/fixtures/session_period/README.md). The w12-tfd
// and w12-tfd2 probes spell, on every chart bar, the minutes from each
// reading to the bar's open (time_close: from the bar's open to the reading),
// "n" for na; every reading is replayed through pine_time / pine_time_close
// as generated code calls them.
// ---------------------------------------------------------------------------

#ifndef PINEFORGE_SESSION_PERIOD_FIXTURE_DIR
#error "PINEFORGE_SESSION_PERIOD_FIXTURE_DIR must name tests/fixtures/session_period"
#endif

struct PeriodField {
    const char* tf;       // nullptr: timeframe.period
    const char* session;
    const char* tz;
    bool close;           // time_close, spelled from the bar's open
};

// w12-tfd: a,b,c,d,e,f,g,h,i,j,k,l,o,q|p,r,s,u.
static const PeriodField kTfdFields[] = {
    {"D", "", "", false},
    {"D", "0000-2400", "", false},
    {"D", "0930-1600", "", false},
    {"D", "0930-1600", "America/New_York", false},
    {"D", "1700-2400", "America/New_York", false},
    {"D", "1800-1700", "", false},
    {"D", "0930-1130,1300-1500", "", false},
    {"D", "1300-1500,0930-1130", "", false},
    {"W", "0930-1600", "", false},
    {"M", "0930-1600", "", false},
    {nullptr, "1700-2400", "America/New_York", false},
    {nullptr, "0000-2400", "America/New_York", false},
    {"D", "0000-2400", "Asia/Tokyo", false},
    {"D", "2000-0200", "America/New_York", false},
    {"D", "0930-1600", "", true},
    {"D", "1800-1700", "", true},
    {"D", "0000-2400", "America/New_York", true},
    {"W", "0930-1600", "", true},
};

// w12-tfd2: a,b,c,d,e,f,g,h,i,j,k,l,o,q,r,s.
static const PeriodField kTfd2Fields[] = {
    {"M", "0930-1600", "", true},
    {"W", "0930-1600:23456", "", false},
    {"W", "0930-1600:23456", "", true},
    {"D", "24x7", "", false},
    {"D", "0930-1600:1", "", false},
    {"M", "1800-1700", "", false},
    {"D", "2330-2430", "", false},
    {"D", "2330-2430", "", true},
    {"D", "0000-0000", "America/New_York", false},
    {"D", "0000-0000", "America/New_York", true},
    {"W", "0000-2400", "", false},
    {"D", "1700-1700", "America/New_York", false},
    {"D", "1700-1700", "America/New_York", true},
    {"D", "0930-1130,1300-1500", "America/New_York", false},
    {"D", "0930-1130,1300-1500", "America/New_York", true},
    {"M", "1800-1700", "", true},
};

// w12-tfd3: a,b,c,d,e,f,g,h,i,j,k,l,o,q -- windows past midnight.
static const PeriodField kTfd3Fields[] = {
    {"D", "2330-2430", "", false},
    {"D", "2330-0030", "", false},
    {"D", "2330-2400", "", false},
    {"D", "0000-0030", "", false},
    {"D", "2330-2430:1234567", "", false},
    {"D", "2300-2500", "", false},
    {"D", "2200-0100", "", false},
    {"D", "0000-0100,2300-2400", "", false},
    {"D", "2330-2430", "", true},
    {"D", "2330-0030:1234567", "", false},
    {nullptr, "2330-2430", "", false},
    {nullptr, "2330-0030", "", false},
    {"D", "2200-0100:1234567", "", false},
    {"D", "1200-0100", "", false},
};

// w12-tfd4: a,b,c,d,e,f,g,h,i,j,k,l,o,q -- intraday bars on windows that do
// not open on the chart's grid.
static const PeriodField kTfd4Fields[] = {
    {nullptr, "0930-1600", "", false},
    {nullptr, "0945-1600", "", false},
    {nullptr, "0930-1130,1300-1500", "", false},
    {nullptr, "0930-1130,1245-1500", "", false},
    {"240", "0930-1600", "", false},
    {"120", "1800-1700", "", false},
    {nullptr, "1800-1700", "", false},
    {"30", "0945-1600", "", false},
    {nullptr, "0945-1600", "", true},
    {"240", "0930-1600", "", true},
    {"240", "", "", false},
    {nullptr, "0945-1600:23456", "", false},
    {nullptr, "0935-1600", "", false},
    {nullptr, "0907-1600", "", false},
};

// w12-tfd5: a,b,c,d,e,f,g,h -- a session argument's grid beside the symbol's
// 17:00-anchored one.
static const PeriodField kTfd5Fields[] = {
    {"240", "1800-1700", "", false},
    {"240", "1700-1700", "", false},
    {"60", "1800-1700", "", false},
    {"240", "", "", false},
    {"240", "1800-1700", "", true},
    {"45", "1800-1700", "", false},
    {"45", "", "", false},
    {nullptr, "1800-1700", "", false},
};

// Every reading of one tape against the engine; returns the readings compared.
static int replay_period_tape(const ChartFacts& chart, const PeriodField* fields,
                              std::size_t count, int& wrong) {
    bool ok = true;
    const auto readings =
        exit_comment_tape::read(PINEFORGE_SESSION_PERIOD_FIXTURE_DIR, chart.slug, ok);
    CHECK(ok);
    int compared = 0;
    for (const auto& reading : readings) {
        std::string flat = reading.signal;
        for (char& c : flat) if (c == '|') c = ',';
        const auto spelled = exit_comment_tape::split(flat, ',');
        if (spelled.size() != count) { ++wrong; continue; }
        for (std::size_t k = 0; k < count; ++k) {
            const PeriodField& f = fields[k];
            const std::string tf = f.tf ? f.tf : chart.tf;
            const int64_t value = f.close
                ? pine_time_close(reading.bar_ms, tf, f.session, f.tz, chart.tf,
                                  chart.sym_tz, chart.sym_session)
                : pine_time(reading.bar_ms, tf, f.session, f.tz, chart.tf,
                            chart.sym_tz, chart.sym_session);
            ++compared;
            if (!same_reading(spelled[k], value, reading.bar_ms, f.close) && ++wrong <= 8) {
                std::printf("  %s bar %lld field %zu (%s %s %s) tv=%s engine=%lld\n",
                            chart.slug, static_cast<long long>(reading.bar_ms), k,
                            f.tf ? f.tf : "period", f.session, f.tz, spelled[k].c_str(),
                            static_cast<long long>(value));
            }
        }
    }
    return compared;
}

// The session's timezone -- the explicit one, else syminfo.timezone -- keys
// the period: time("D", "0000-2400") on OANDA:XAUUSD opens at New York's
// midnight, as time("D", "0000-2400", "America/New_York") does.
static void test_session_period_tapes() {
    std::printf("test_session_period_tapes\n");
    struct Tape {
        ChartFacts chart;
        const PeriodField* fields;
        std::size_t count;
    };
    const std::size_t tfd = sizeof(kTfdFields) / sizeof(kTfdFields[0]);
    const std::size_t tfd2 = sizeof(kTfd2Fields) / sizeof(kTfd2Fields[0]);
    const std::size_t tfd3 = sizeof(kTfd3Fields) / sizeof(kTfd3Fields[0]);
    const std::size_t tfd4 = sizeof(kTfd4Fields) / sizeof(kTfd4Fields[0]);
    const std::size_t tfd5 = sizeof(kTfd5Fields) / sizeof(kTfd5Fields[0]);
    const Tape tapes[] = {
        {{"w12-tfd-btc15", "15", "UTC", "24x7"}, kTfdFields, tfd},
        {{"w12-tfd-xau15", "15", "America/New_York", "1800-1700"}, kTfdFields, tfd},
        {{"w12-tfd-btc1d", "1D", "UTC", "24x7"}, kTfdFields, tfd},
        {{"w12-tfd-xau1d", "1D", "America/New_York", "1800-1700"}, kTfdFields, tfd},
        {{"w12-tfd2-btc15", "15", "UTC", "24x7"}, kTfd2Fields, tfd2},
        {{"w12-tfd2-xau15", "15", "America/New_York", "1800-1700"}, kTfd2Fields, tfd2},
        {{"w12-tfd2-btc1d", "1D", "UTC", "24x7"}, kTfd2Fields, tfd2},
        {{"w12-tfd2-xau1d", "1D", "America/New_York", "1800-1700"}, kTfd2Fields, tfd2},
        {{"w12-tfd3-btc1d", "1D", "UTC", "24x7"}, kTfd3Fields, tfd3},
        {{"w12-tfd3-btc60", "60", "UTC", "24x7"}, kTfd3Fields, tfd3},
        {{"w12-tfd4-btc60", "60", "UTC", "24x7"}, kTfd4Fields, tfd4},
        {{"w12-tfd4-btc15", "15", "UTC", "24x7"}, kTfd4Fields, tfd4},
        {{"w12-tfd4-aapl15", "15", "America/New_York", "0930-1600"}, kTfd4Fields, tfd4},
        {{"w12-tfd5-xau15", "15", "America/New_York", "1800-1700"}, kTfd5Fields, tfd5},
    };
    for (const Tape& tape : tapes) {
        int wrong = 0;
        const int compared = replay_period_tape(tape.chart, tape.fields, tape.count, wrong);
        std::printf("  %s: %d readings compared, %d differ\n", tape.chart.slug, compared, wrong);
        CHECK(compared > 0);
        CHECK(wrong == 0);
    }
}

// ---------------------------------------------------------------------------
// The function forms time_close("D" / "W" / "M") of the symbol's own clock
// (lane W12-ENG-TIME, tests/fixtures/time_close_function/README.md): the
// exact boundary, not its last millisecond; on an intraday chart a week or
// month closes where the next one opens, on a daily chart at its last traded
// close. The w12-tclose probe spells, in milliseconds, time_close - time,
// then for D, W and M the function's close minus its own open and minus the
// bar's time.
// ---------------------------------------------------------------------------

#ifndef PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR
#error "PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR must name tests/fixtures/time_close_function"
#endif
#ifndef PINEFORGE_DST_DAY_CLOSE_FIXTURE_DIR
#error "PINEFORGE_DST_DAY_CLOSE_FIXTURE_DIR must name tests/fixtures/dst_day_close"
#endif

static void test_time_close_function_tapes() {
    std::printf("test_time_close_function_tapes\n");
    struct Tape {
        const char* dir;
        ChartFacts chart;
        int holiday_readings;  // W / M readings of a period an exchange holiday shortens
        int periods;           // how many of D, W, M the tape is read for
    };
    // CAPITALCOM:BTCUSD trades 1700-1700 every day: spelled with its day list,
    // as TradingView's symbol describes it, its weekend trading dates count
    // (a session with hours and no list trades Monday to Friday on the
    // symbol clock, session_period_last_traded_close_ms's weekday rule).
    static const Tape tapes[] = {
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR, {"w12-tclose-btc15", "15", "UTC", "24x7"}, 0, 3},
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR,
         {"w12-tclose-xau15", "15", "America/New_York", "1800-1700"}, 0, 3},
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR,
         {"w12-tclose-aapl15", "15", "America/New_York", "0930-1600"}, 0, 3},
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR,
         {"w12-tclose-eur15", "15", "America/New_York", "1700-1700"}, 0, 3},
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR, {"w12-tclose-btc1d", "1D", "UTC", "24x7"}, 0, 3},
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR,
         {"w12-tclose-xau1d", "1D", "America/New_York", "1800-1700"}, 0, 3},
        {PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR,
         {"w12-tclose-aapl1d", "1D", "America/New_York", "0930-1600"}, 39, 3},
        {PINEFORGE_DST_DAY_CLOSE_FIXTURE_DIR,
         {"w12-tclose-cap1d", "1D", "America/New_York", "1700-1700:1234567"}, 0, 3},
    };
    static const char* const kPeriod[] = {"D", "W", "M"};
    for (const Tape& tape : tapes) {
        const ChartFacts& chart = tape.chart;
        bool ok = true;
        const auto readings = exit_comment_tape::read(tape.dir, chart.slug, ok);
        CHECK(ok);
        int compared = 0;
        int wrong = 0;
        int holiday = 0;
        for (const auto& reading : readings) {
            const auto spelled = exit_comment_tape::split(reading.signal, ',');
            if (spelled.size() != 7) { ++wrong; continue; }
            for (int p = 0; p < tape.periods; ++p) {
                const std::string tf = kPeriod[p];
                const int64_t open = pine_time(reading.bar_ms, tf, "", "", chart.tf,
                                               chart.sym_tz, chart.sym_session);
                const int64_t close = pine_time_close(reading.bar_ms, tf, "", "", chart.tf,
                                                      chart.sym_tz, chart.sym_session);
                const bool read = !is_na(open) && !is_na(close);
                const long long engine[2] = {
                    read ? static_cast<long long>(close - open) : -1,
                    read ? static_cast<long long>(close - reading.bar_ms) : -1};
                for (int k = 0; k < 2; ++k) {
                    const std::string& text = spelled[static_cast<std::size_t>(1 + 2 * p + k)];
                    ++compared;
                    const long long tv = std::atoll(text.c_str());
                    if (text != "n" && read && engine[k] == tv) continue;
                    if (text == "n" || !read) {
                        if (++wrong <= 6) {
                            std::printf("  %s bar %lld %s field %d: tv %s engine %s\n", chart.slug,
                                        static_cast<long long>(reading.bar_ms), kPeriod[p], k,
                                        text.c_str(), read ? "a value" : "na");
                        }
                        continue;
                    }
                    // A period an exchange holiday shortens: TradingView closes
                    // it earlier, or opens it later, than the session's own
                    // weekdays (calendar data the run does not hold).
                    if (p > 0 && text != "n" && engine[k] > tv) { ++holiday; continue; }
                    if (++wrong <= 6) {
                        std::printf("  %s bar %lld %s field %d: tv %s engine %lld\n", chart.slug,
                                    static_cast<long long>(reading.bar_ms), kPeriod[p], k,
                                    text.c_str(), engine[k]);
                    }
                }
            }
        }
        std::printf("  %s: %d readings compared, %d on holiday periods, %d differ\n",
                    chart.slug, compared, holiday, wrong);
        CHECK(compared > 0);
        CHECK(holiday == tape.holiday_readings);
        CHECK(wrong == 0);
    }
}

// A session with hours and no day list trades Monday to Friday on the symbol
// clock, so its month opens at the first weekday session (OANDA:EURUSD's
// February 2025 at Sunday 02-02 17:00 ET); a bar on a day that rule steps over
// proves that day trades, and the month it holds opened by then -- never
// after the bar.
static void test_month_open_never_after_the_bar() {
    std::printf("test_month_open_never_after_the_bar\n");
    const std::string ny = "America/New_York";
    const int64_t fri_0131 = 1738360800000LL;  // 2025-01-31 17:00 EST, trade date Sat 02-01
    const int64_t sat_0201 = 1738447200000LL;  // 2025-02-01 17:00 EST, trade date Sun 02-02
    const int64_t sun_0202 = 1738533600000LL;  // 2025-02-02 17:00 EST, trade date Mon 02-03
    CHECK(pine_time(sun_0202, "M", "", "", "1D", ny, "1700-1700") == sun_0202);
    CHECK(pine_time(fri_0131, "M", "", "", "1D", ny, "1700-1700") == fri_0131);
    CHECK(pine_time(sat_0201, "M", "", "", "1D", ny, "1700-1700") == fri_0131);
    // Spelled with every day, the month opens on the 1st's session.
    CHECK(pine_time(sun_0202, "M", "", "", "1D", ny, "1700-1700:1234567") == fri_0131);
}

// A 24x7 session in New York, where no TradingView symbol reaches but the
// rule of tests/fixtures/dst_day_close does: each day runs from one local
// midnight to the next, 23 hours on the 2025-03-09 switch and 25 on
// 2025-11-02 (the runtime's New York midnight on those Sundays read an hour
// off, at 04:00Z and 05:00Z).
static void test_wall_clock_day_24x7_new_york() {
    std::printf("test_wall_clock_day_24x7_new_york\n");
    const std::string ny = "America/New_York";
    const std::string sess = "24x7";
    // Saturday 2025-03-08, a 24-hour day.
    CHECK(pine_time(1741435200000LL, "D", "", "", "15", ny, sess) == 1741410000000LL);
    CHECK(pine_time_close(1741435200000LL, "D", "", "", "15", ny, sess) == 1741496400000LL);
    // Sunday 2025-03-09: 00:00 EST (05:00Z) to Monday 00:00 EDT (04:00Z).
    CHECK(pine_time(1741521600000LL, "D", "", "", "15", ny, sess) == 1741496400000LL);
    CHECK(pine_time_close(1741521600000LL, "D", "", "", "15", ny, sess) == 1741579200000LL);
    // Sunday 2025-11-02: 00:00 EDT (04:00Z) to Monday 00:00 EST (05:00Z).
    CHECK(pine_time(1762084800000LL, "D", "", "", "15", ny, sess) == 1762056000000LL);
    CHECK(pine_time_close(1762084800000LL, "D", "", "", "15", ny, sess) == 1762146000000LL);
}

// session.ispremarket / session.ispostmarket read the day list the in-market
// predicate reads (lane W12-ENG-TIME; W11-ENG-TIME-COLOR's reviewer): a
// session of several windows with no list trades Monday to Friday (lab tv
// w11-sessmask-btc15 and w11-sessmask2-btc15, tests/fixtures/session_clock),
// so a Saturday is neither before its open nor after its close, as it is not
// in market. No TradingView chart of such a session holds a Saturday bar; the
// weekday readings are TSE:7203's and HKEX:700's
// (test_session_prepost_windows_tape).
static void test_prepost_market_day_list() {
    std::printf("test_prepost_market_day_list\n");
    const std::string tse = "0900-1130,1230-1530";
    const std::string tokyo = "Asia/Tokyo";
    const int64_t sat_0500 = 1741377600000LL;   // Sat 2025-03-08 05:00 JST
    const int64_t sat_1630 = 1741419000000LL;   // Sat 2025-03-08 16:30 JST
    const int64_t fri_0500 = 1741291200000LL;   // Fri 2025-03-07 05:00 JST
    const int64_t fri_1630 = 1741332600000LL;   // Fri 2025-03-07 16:30 JST
    CHECK(!pine_session_ismarket(tse, tokyo, sat_0500 + 5 * 3600000LL));  // Sat 10:00
    CHECK(!pine_session_ispremarket(tse, tokyo, sat_0500));
    CHECK(!pine_session_ispostmarket(tse, tokyo, sat_1630));
    // Weekdays keep theirs, and a list that names Saturday admits it.
    CHECK(pine_session_ispremarket(tse, tokyo, fri_0500));
    CHECK(pine_session_ispostmarket(tse, tokyo, fri_1630));
    CHECK(pine_session_ispremarket(tse + ":1234567", tokyo, sat_0500));
    CHECK(pine_session_ispostmarket(tse + ":1234567", tokyo, sat_1630));
    // One window, no list: every day, as before.
    CHECK(pine_session_ispremarket("0900-1530", tokyo, sat_0500));
    CHECK(pine_session_ispostmarket("0900-1530", tokyo, sat_1630));
}

int main() {
    test_ismarket_inside_rth();
    test_ismarket_outside_rth_close();
    test_ismarket_postmarket_is_false();
    test_ismarket_premarket_is_false();
    test_ispremarket_true();
    test_ispremarket_before_0400_false();
    test_ispremarket_inside_rth_false();
    test_ispostmarket_true();
    test_ispostmarket_inside_rth_false();
    test_ispostmarket_premarket_false();
    test_24x7_ismarket_always_true();
    test_24x7_prepost_always_false();
    test_hhmm_to_minutes_basic();
    test_ismarket_weekend_filter();
    test_firstlastbar_transitions();
    test_start_equals_end_is_full_day();
    test_session_2400_tapes();
    test_session_clock_edge_tapes();
    test_session_day_list_tapes();
    test_session_day_list_edges();
    test_session_period_tapes();
    test_time_close_function_tapes();
    test_wall_clock_day_24x7_new_york();
    test_prepost_market_day_list();
    test_month_open_never_after_the_bar();

    std::printf("\nsession_predicates: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
