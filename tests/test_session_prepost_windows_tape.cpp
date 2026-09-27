/*
 * test_session_prepost_windows_tape.cpp -- R5 lane K-SESSION-WINDOWS, item F6.
 *
 * session.ispremarket / session.ispostmarket on a session with more than one
 * window, and on an overnight session. Generated code reads them through
 * PineStrategyHost's class-scope wrappers, which call the chart-bar forms
 * pineforge::session_in_premarket / session_in_postmarket (session_time.hpp)
 * with the chart timeframe; inside a request.security payload that call is
 * the whole reading, and on the chart it is gated by the calendar's in-market
 * answer.
 *
 * TradingView (tests/fixtures/session_windows, `lab tv` every-flag tapes):
 *   - TSE:7203 (09:00-11:30, 12:30-15:30 Asia/Tokyo), HKEX:700 (09:30-12:00,
 *     13:00-16:00 Asia/Hong_Kong) and CBOT:ZC1! (19:00-07:45, 08:30-13:20
 *     America/Chicago) at 15, 60 and 240 minutes: every bar is neither pre-
 *     nor post-market -- the 60-minute bars that open inside a break (TSE
 *     12:00, ZC 08:00) included;
 *   - CME_MINI:ES1! (1700-1600), OANDA:XAUUSD (1800-1700), OANDA:EURUSD
 *     (1700-1700) and BINANCE:ETHUSDT.P (24x7): every bar neither;
 *   - NASDAQ:AAPL, the one-window control that must not move: with extended
 *     hours the bars opening 04:00-09:15 ET are pre-market and 16:00-19:45
 *     post-market, without them none is.
 * So pre-market runs from 04:00 to the session day's FIRST open and
 * post-market from its LAST close to 20:00, over every window; a break
 * between windows is neither; a session day that opens on the previous date
 * (an overnight session) or never closes (24 hours) has neither.
 *
 * Before the fix both predicates read only the first "HHMM-HHMM" window with
 * fixed 04:00 / 20:00 bounds (lane CG-SESSION-2 finding F6): the afternoon of
 * HKEX:700 and TSE:7203 read post-market, the windows written the other way
 * round made the morning pre-market, and an overnight first window made the
 * same instant both. The synthetic day below holds the off-market bars no
 * TradingView chart above has (the lunch break, the evening), pinned from the
 * rule.
 */

#include "session_flag_tape_fixture.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/session_time.hpp>

#include <cstdint>
#include <cstdio>
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

#ifndef PINEFORGE_KSW_FIXTURE_DIR
#error "PINEFORGE_KSW_FIXTURE_DIR must name tests/fixtures/session_windows"
#endif

namespace {

struct Case {
    const char* slug;
    const char* session;
    const char* timezone;
    const char* chart_tf;
    std::size_t bars;  // the tape's chart bars
};

// Every bar's pre- and post-market reading against TradingView's.
void replay(const Case& c) {
    bool ok = true;
    const auto tape = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR, c.slug, ok);
    CHECK(ok);
    CHECK(tape.size() == c.bars);
    int pre_misses = 0, post_misses = 0, both = 0, pre_bars = 0, post_bars = 0;
    for (const auto& bar : tape) {
        const bool pre = session_in_premarket(c.session, c.timezone, bar.ts, c.chart_tf);
        const bool post = session_in_postmarket(c.session, c.timezone, bar.ts, c.chart_tf);
        if (pre != bar.tv.premarket) ++pre_misses;
        if (post != bar.tv.postmarket) ++post_misses;
        if (pre && post) ++both;
        pre_bars += bar.tv.premarket;
        post_bars += bar.tv.postmarket;
    }
    std::printf("  %-28s %-22s %-4s %4zu bars: TradingView pre %d post %d; misses pre %d post %d, both %d\n",
                c.slug, c.session, c.chart_tf, tape.size(), pre_bars, post_bars, pre_misses,
                post_misses, both);
    CHECK(pre_misses == 0);
    CHECK(post_misses == 0);
    CHECK(both == 0);
}

// Every 15-minute open of Wednesday 2025-03-05 in `timezone`: "P", "Q" or ""
// per open, in order.
std::vector<std::string> synthetic_day(const char* session, const char* timezone) {
    std::vector<std::string> out;
    const auto midnight = native_calendar::resolve_civil(timezone, 2025, 3, 5, 0, 0);
    CHECK(midnight.has_value());
    if (!midnight) return out;
    for (int q = 0; q < 96; ++q) {
        const std::int64_t ts = midnight->epoch_ms + q * 15 * session_tape::kMinute;
        const bool pre = session_in_premarket(session, timezone, ts, "15");
        const bool post = session_in_postmarket(session, timezone, ts, "15");
        out.push_back(std::string(pre ? "P" : "") + (post ? "Q" : ""));
    }
    return out;
}

// The quarter-hour indices [first, last] of a day that carry `flag`, or
// (-1, -1) when none does; `count` is how many do.
struct Span { int first = -1; int last = -1; int count = 0; };
Span span_of(const std::vector<std::string>& day, const std::string& flag) {
    Span s;
    for (int q = 0; q < static_cast<int>(day.size()); ++q) {
        if (day[q] != flag) continue;
        if (s.first < 0) s.first = q;
        s.last = q;
        ++s.count;
    }
    return s;
}

constexpr int quarter(int h, int m) { return h * 4 + m / 15; }

}  // namespace

int main() {
    std::printf("TradingView's pre- and post-market bars, replayed\n");
    const Case cases[] = {
        // Two windows, 15 minutes (lane CG-SESSION-2's tapes).
        {"cgs2-flags-hkex700-15", "0930-1200,1300-1600", "Asia/Hong_Kong", "15", 220},
        {"cgs2-flags-hkex700-15", "1300-1600,0930-1200", "Asia/Hong_Kong", "15", 220},
        // TradingView holds TSE:7203's bar opening at 15:30 in session, which
        // the published 15:30 end leaves out; 15:45 keeps it (lane
        // CG-SESSION-2's open finding), so the replay reads the windows alone.
        {"cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "15", 230},
        {"cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "15", 710},
        {"cgs2-flags-zc1-15", "0830-1320,1900-0745", "America/Chicago", "15", 710},
        // 60 and 240 minutes: TSE 12:00 and ZC 08:00 open inside a break.
        {"ksw-flags-hkex700-60", "0930-1200,1300-1600", "Asia/Hong_Kong", "60", 60},
        {"ksw-flags-hkex700-240", "0930-1200,1300-1600", "Asia/Hong_Kong", "240", 20},
        {"ksw-flags-tse7203-60", "0900-1130,1230-1545", "Asia/Tokyo", "60", 70},
        {"ksw-flags-tse7203-240", "0900-1130,1230-1545", "Asia/Tokyo", "240", 20},
        {"ksw-flags-zc1-60", "1900-0745,0830-1320", "America/Chicago", "60", 190},
        {"ksw-flags-zc1-240", "1900-0745,0830-1320", "America/Chicago", "240", 50},
        // Overnight and 24-hour sessions (lane CG-ISMARKET's tapes).
        {"cgim-flags-es1-60-dst-mar", "1700-1600", "America/Chicago", "60", 210},
        {"cgim-flags-xauusd-60-dst-mar", "1800-1700", "America/New_York", "60", 210},
        {"cgim-flags-eurusd-60-dst-mar", "1700-1700", "America/New_York", "60", 220},
        {"cgim-flags-eth-60-24x7", "24x7", "UTC", "60", 97},
        // One window, the control: extended hours and regular hours.
        {"cgim-flags-aapl-60-ext", "0930-1600", "America/New_York", "60", 160},
        {"cgim-flags-aapl-60-reg", "0930-1600", "America/New_York", "60", 70},
        {"cgs2-hist-aapl-15-ext", "0930-1600", "America/New_York", "15", 639},
        {"cgs2-hist-aapl-15-reg", "0930-1600", "America/New_York", "15", 260},
    };
    for (const Case& c : cases) replay(c);

    std::printf("A synthetic day's off-market bars\n");
    // HKEX: the lunch break is neither, whichever window is written first.
    const auto hk = synthetic_day("0930-1200,1300-1600", "Asia/Hong_Kong");
    const auto hk_reversed = synthetic_day("1300-1600,0930-1200", "Asia/Hong_Kong");
    CHECK(hk == hk_reversed);
    const Span hk_pre = span_of(hk, "P"), hk_post = span_of(hk, "Q");
    CHECK(hk_pre.first == quarter(4, 0) && hk_pre.last == quarter(9, 15) && hk_pre.count == 22);
    CHECK(hk_post.first == quarter(16, 0) && hk_post.last == quarter(19, 45) && hk_post.count == 16);
    for (int q = quarter(12, 0); q <= quarter(12, 45); ++q) CHECK(hk[q].empty());
    CHECK(span_of(hk, "PQ").count == 0);
    // CBOT corn: an overnight session day has neither, in either order.
    for (const char* corn : {"1900-0745,0830-1320", "0830-1320,1900-0745"}) {
        const auto day = synthetic_day(corn, "America/Chicago");
        int flagged = 0;
        for (const auto& f : day) flagged += !f.empty();
        CHECK(flagged == 0);
    }
    // The single window is the control: 04:00-09:15 pre, 16:00-19:45 post.
    const auto ny = synthetic_day("0930-1600", "America/New_York");
    const Span ny_pre = span_of(ny, "P"), ny_post = span_of(ny, "Q");
    CHECK(ny_pre.first == quarter(4, 0) && ny_pre.last == quarter(9, 15) && ny_pre.count == 22);
    CHECK(ny_post.first == quarter(16, 0) && ny_post.last == quarter(19, 45) && ny_post.count == 16);
    // The extended windows written as the session: its day opens at 04:00 and
    // closes at 20:00, so nothing is outside it, and 11:00 is its break.
    const auto ext = synthetic_day("0400-0930,1600-2000:23456", "America/New_York");
    int ext_flagged = 0;
    for (const auto& f : ext) ext_flagged += !f.empty();
    CHECK(ext_flagged == 0);
    std::printf("  HKEX lunch 12:00-12:45 neither (either order); pre %d, post %d quarters; "
                "overnight corn none\n", hk_pre.count, hk_post.count);

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
