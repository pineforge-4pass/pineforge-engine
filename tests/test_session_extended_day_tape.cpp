/*
 * test_session_extended_day_tape.cpp -- R5 lane K-SESSION-WINDOWS, item F2.
 *
 * session.isfirstbar / session.islastbar against session.isfirstbar_regular /
 * session.islastbar_regular, through the Pine host as generated code reads
 * them (the session_* members, inside on_source_bar).
 *
 * TradingView (tests/fixtures/session_windows, README.md there): on
 * NASDAQ:AAPL with extended hours the plain pair marks the chart's session
 * day -- the regular day widened by the pre- and post-market bars the chart
 * holds -- and the _regular pair the regular day alone: at 60 minutes
 * isfirstbar 04:00, islastbar 19:00, isfirstbar_regular 10:00,
 * islastbar_regular 15:00 ET; at 15 minutes 04:00, 19:45, 09:30, 15:45.
 * Without extended hours, and on every chart whose session has no pre- or
 * post-market (TSE:7203, HKEX:700 and CBOT:ZC1! at 15 minutes, CME_MINI:ES1!,
 * OANDA:XAUUSD, OANDA:EURUSD, BINANCE:ETHUSDT.P), each pair is one flag.
 *
 * Before this lane the host held one pair, the kernel's regular session-day
 * facts, and generated code read it for both spellings (lane CG-ISMARKET
 * finding F2): the extended tapes missed isfirstbar and islastbar on 20
 * bars each at 60 minutes and at 15.
 *
 * Each run feeds the tape's own bar times. The last-bar flags of the tape's
 * final bar are not compared: TradingView's history goes on past the export
 * window, while a batch's final bar closes its day by the run-end convention
 * (ADR-0001, E26-6), which is not what these tapes measure.
 */

#include "session_flag_tape_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

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

struct Seen {
    std::int64_t ts = 0;
    bool first = false, last = false, first_regular = false, last_regular = false;
};

class FlagsHost final : public source::PineStrategyHost {
public:
    FlagsHost(const std::string& session, const std::string& timezone) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
    }
    void on_source_bar(const Bar&) override {
        seen.push_back({current_bar_.timestamp, session_isfirstbar_, session_islastbar_,
                        session_isfirstbar_regular_, session_islastbar_regular_});
    }
    std::vector<Seen> seen;
};

struct Case {
    const char* slug;
    const char* session;
    const char* timezone;
    const char* tf;
};

void replay(const Case& c) {
    bool ok = true;
    const auto tape = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR, c.slug, ok);
    CHECK(ok);
    std::vector<Bar> bars;
    for (const auto& bar : tape) bars.push_back(session_tape::flat_bar(bar.ts));

    FlagsHost host(c.session, c.timezone);
    host.run(bars.data(), static_cast<int>(bars.size()), c.tf, c.tf);
    CHECK(host.last_error().empty());
    const bool aligned = host.seen.size() == bars.size();
    CHECK(aligned);
    int misses[4] = {0, 0, 0, 0};
    int flagged[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; aligned && i < tape.size(); ++i) {
        const Seen& got = host.seen[i];
        const auto& tv = tape[i].tv;
        CHECK(got.ts == tape[i].ts);
        const bool final_bar = i + 1 == tape.size();
        misses[0] += got.first != tv.first;
        misses[1] += !final_bar && got.last != tv.last;
        misses[2] += got.first_regular != tv.first_regular;
        misses[3] += !final_bar && got.last_regular != tv.last_regular;
        flagged[0] += tv.first; flagged[1] += tv.last;
        flagged[2] += tv.first_regular; flagged[3] += tv.last_regular;
    }
    std::printf("  %-28s %-20s %-3s %3zu bars (TradingView F %d L %d f %d l %d): misses"
                " isfirstbar %d islastbar %d isfirstbar_regular %d islastbar_regular %d\n",
                c.slug, c.session, c.tf, tape.size(), flagged[0], flagged[1], flagged[2],
                flagged[3], misses[0], misses[1], misses[2], misses[3]);
    for (int k = 0; k < 4; ++k) CHECK(misses[k] == 0);
}

}  // namespace

int main() {
    std::printf("session.isfirstbar / islastbar and their _regular twins, against TradingView\n");
    const Case cases[] = {
        // Extended hours: the two pairs part.
        {"cgim-flags-aapl-60-ext", "0930-1600", "America/New_York", "60"},
        {"cgs2-hist-aapl-15-ext", "0930-1600", "America/New_York", "15"},
        // Regular hours: one pair.
        {"cgim-flags-aapl-60-reg", "0930-1600", "America/New_York", "60"},
        {"cgs2-hist-aapl-15-reg", "0930-1600", "America/New_York", "15"},
        // Sessions without pre- or post-market: one pair.
        {"cgs2-flags-hkex700-15", "0930-1200,1300-1600", "Asia/Hong_Kong", "15"},
        {"cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "15"},
        {"cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "15"},
        {"cgim-flags-es1-60-dst-mar", "1700-1600", "America/Chicago", "60"},
        {"cgim-flags-xauusd-60-dst-mar", "1800-1700", "America/New_York", "60"},
        {"cgim-flags-eurusd-60-dst-mar", "1700-1700", "America/New_York", "60"},
        {"cgim-flags-eth-60-24x7", "24x7", "UTC", "60"},
    };
    for (const Case& c : cases) replay(c);

    // A stream holds its warmup; its realtime bars have no bar after them.
    // There a pre- or post-market bar reads the chart timeframe's next slot
    // and a regular bar the kernel's close, and the plain pair is
    // TradingView's on every bar. The _regular pair's close on a realtime day
    // is the kernel's scheduled one, which steps the regular session's
    // 09:30-anchored grid: after the 15:00 bar it expects a 15:30 bar in
    // session, so no realtime regular day closes (pinned: 8 days).
    {
        bool ok = true;
        const auto tape = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR,
                                                  "cgim-flags-aapl-60-ext", ok);
        CHECK(ok);
        std::vector<Bar> bars;
        for (const auto& bar : tape) bars.push_back(session_tape::flat_bar(bar.ts));
        const std::size_t warmup = 32;  // two whole days
        // The weekday mask keeps the weekend out of the calendar: a stream
        // refuses a gap over in-session slots, which an unmasked session
        // declares on Saturday and Sunday.
        FlagsHost host("0930-1600:23456", "America/New_York");
        const bool began = bars.size() > warmup
            && host.stream_begin(bars.data(), static_cast<int>(warmup), "60", "60");
        CHECK(began);
        for (std::size_t i = warmup; began && i < bars.size(); ++i)
            CHECK(host.stream_push_bar(bars[i]));
        CHECK(host.stream_end(false));
        CHECK(host.last_error().empty());
        const bool aligned = host.seen.size() == tape.size();
        CHECK(aligned);
        int misses[4] = {0, 0, 0, 0};
        int realtime_regular_closes = 0;
        for (std::size_t i = 0; aligned && i + 1 < tape.size(); ++i) {
            const Seen& got = host.seen[i];
            const auto& tv = tape[i].tv;
            misses[0] += got.first != tv.first;
            misses[1] += got.last != tv.last;
            misses[2] += got.first_regular != tv.first_regular;
            misses[3] += got.last_regular != tv.last_regular;
            realtime_regular_closes += i >= warmup && tv.last_regular && !got.last_regular;
        }
        std::printf("  %-28s streamed after %zu bars: misses isfirstbar %d islastbar %d"
                    " isfirstbar_regular %d islastbar_regular %d (realtime regular closes missed %d)\n",
                    "cgim-flags-aapl-60-ext", warmup, misses[0], misses[1], misses[2], misses[3],
                    realtime_regular_closes);
        CHECK(misses[0] == 0);
        CHECK(misses[1] == 0);
        CHECK(misses[2] == 0);
        CHECK(misses[3] == realtime_regular_closes);
        CHECK(realtime_regular_closes == 8);
    }
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
