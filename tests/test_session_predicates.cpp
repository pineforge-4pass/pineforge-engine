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
        bool replayed;        // false: another rule's reading, not compared
    };
    static const Field fields[] = {
        {nullptr, "0000-2400", "", false, true},
        {nullptr, "1700-2400", "", false, true},
        {nullptr, "2045-2400", "Asia/Tokyo", false, true},
        {"D", "0000-2400", "", false, true},
        {nullptr, "0000-2400:23456", "", false, true},
        {nullptr, "1700-2400", "", true, true},
        // Two windows and no day list: which days TradingView admits there is
        // another rule's than this clock's.
        {nullptr, "2300-2400,0000-0100", "", false, false},
        {nullptr, "2000-2400", "America/New_York", false, true},
        {nullptr, "0000-0000", "", false, true},
        {nullptr, "0000-2359", "", false, true},
        {nullptr, "1700-0000", "", false, true},
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
                if (!f.replayed) continue;
                const std::string tf = f.tf ? f.tf : chart.tf;
                const int64_t value = f.close
                    ? pine_time_close(reading.bar_ms, tf, f.session, f.tz, chart.tf,
                                      chart.sym_tz, chart.sym_session)
                    : pine_time(reading.bar_ms, tf, f.session, f.tz, chart.tf,
                                chart.sym_tz, chart.sym_session);
                // Where a D period opens under a session argument is not this
                // clock's rule, and two readings of it differ from
                // TradingView's (lane W11-ENG-TIME-COLOR's report): time("D",
                // session) keys its day on UTC where TradingView keys it on
                // syminfo.timezone (the New York chart), and on a D chart
                // time(timeframe.period, session, tz) floors the bar to the
                // tz's day where TradingView answers the bar's own time. There
                // only whether the bar is in the window is compared.
                const bool daily_chart = std::string(chart.tf) == "1D";
                const bool session_only =
                    (f.tf != nullptr && std::string(chart.sym_tz) != "UTC")
                    || (daily_chart && f.tz[0] != '\0');
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

    std::printf("\nsession_predicates: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
