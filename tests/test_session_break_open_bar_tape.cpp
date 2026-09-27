/*
 * test_session_break_open_bar_tape.cpp -- R5 lane K-SESSION-WINDOWS, item F7.
 *
 * An aggregated chart bar whose nominal open falls inside a session's break.
 * On TSE:7203 (0900-1130,1230-1530 Asia/Tokyo) the 60-minute bar opening at
 * 12:00 holds the 12:30 reopen; on CBOT:ZC1! (1900-0745,0830-1320
 * America/Chicago) the one opening at 08:00 holds the 08:30 reopen.
 *
 * TradingView (tests/fixtures/session_windows, `lab tv --no-note` exports of
 * this lane): both bars are in market, neither pre- nor post-market, and
 * neither is a session day's first or last bar; its 60- and 240-minute bars
 * open at the same instants as the engine's aggregation of TradingView's own
 * 15-minute bars of the same window.
 *
 * The engine agrees where it answers per bar: the kernel reads such a bar at
 * its interval's first eligible instant, the reopen (lane F1), and the Pine
 * host's session_ismarket_ is that fact; the session-day flags and the
 * pre- / post-market predicates are TradingView's on every bar. So nothing
 * changes here; the tapes are the pin.
 *
 * What does not agree is a reading at the bar's open instant: the
 * time-of-day predicate pine_session_ismarket and the session calendar asked
 * at the open -- the reading the transpiler emits for a chart's
 * session.ismarket (codegen session_market.py) -- put both bars out of
 * market. Pinned per tape (RESIDUAL): 10 bars on each 60-minute tape, none at
 * 240 minutes, whose bars open in session. Lowering session.ismarket to
 * session_ismarket_ is the transpiler's change to make.
 */

#include "session_flag_tape_fixture.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
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
    bool market = false, premarket = false, postmarket = false;
    bool first = false, last = false, first_regular = false, last_regular = false;
    bool at_open_predicate = false;  // pine_session_ismarket at the open
};

class FlagsHost final : public source::PineStrategyHost {
public:
    FlagsHost(const std::string& session, const std::string& timezone) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
    }
    void on_source_bar(const Bar&) override {
        Seen s;
        s.ts = current_bar_.timestamp;
        s.market = session_ismarket_;
        // As generated code gates them: off market, the windows decide.
        s.premarket = !s.market
            && pine_session_ispremarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.postmarket = !s.market
            && pine_session_ispostmarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        s.first = session_isfirstbar_;
        s.last = session_islastbar_;
        s.first_regular = session_isfirstbar_regular_;
        s.last_regular = session_islastbar_regular_;
        s.at_open_predicate =
            pine_session_ismarket(syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
        seen.push_back(s);
    }
    std::vector<Seen> seen;
};

// The session calendar asked at an instant, as the transpiler's chart
// session.ismarket asks it at the bar's open.
bool calendar_at(const native_calendar::SessionCalendar& calendar, std::int64_t ms) {
    const auto day = native_calendar::session_day_at(calendar, ms);
    return day && day->in_session_at(ms);
}

struct Case {
    const char* slug;       // TradingView's aggregated chart
    const char* feed_slug;  // TradingView's 15-minute bars of the same window
    const char* session;
    const char* timezone;
    const char* script_tf;
    int at_open_misses;     // RESIDUAL: bars an at-open reading puts out of market
};

void replay(const Case& c) {
    bool ok = true;
    const auto tape = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR, c.slug, ok);
    const auto feed = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR, c.feed_slug, ok);
    CHECK(ok);
    std::vector<Bar> bars;
    for (const auto& bar : feed) bars.push_back(session_tape::flat_bar(bar.ts));
    FlagsHost host(c.session, c.timezone);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", c.script_tf);
    CHECK(host.last_error().empty());
    const auto calendar = native_calendar::parse_session(c.session, c.timezone);
    CHECK(calendar.has_value());

    const bool aligned = host.seen.size() == tape.size();
    CHECK(aligned);
    int label = 0, market = 0, pre = 0, post = 0, first = 0, last = 0, first_r = 0, last_r = 0;
    int predicate_out = 0, calendar_out = 0;
    for (std::size_t i = 0; aligned && i < tape.size(); ++i) {
        const Seen& got = host.seen[i];
        const auto& tv = tape[i].tv;
        const bool final_bar = i + 1 == tape.size();
        label += got.ts != tape[i].ts;
        market += got.market != tv.market;
        pre += got.premarket != tv.premarket;
        post += got.postmarket != tv.postmarket;
        first += got.first != tv.first;
        first_r += got.first_regular != tv.first_regular;
        last += !final_bar && got.last != tv.last;
        last_r += !final_bar && got.last_regular != tv.last_regular;
        predicate_out += tv.market && !got.at_open_predicate;
        calendar_out += tv.market && calendar && !calendar_at(*calendar, tape[i].ts);
    }
    std::printf("  %-22s 15->%-3s %3zu bars: misses label %d ismarket %d pre %d post %d isfirstbar %d"
                " islastbar %d _regular %d/%d; an at-open reading puts %d (predicate) / %d (calendar)"
                " in-market bars out\n",
                c.slug, c.script_tf, tape.size(), label, market, pre, post, first, last, first_r,
                last_r, predicate_out, calendar_out);
    CHECK(label == 0);
    CHECK(market == 0);
    CHECK(pre == 0);
    CHECK(post == 0);
    CHECK(first == 0);
    CHECK(last == 0);
    CHECK(first_r == 0);
    CHECK(last_r == 0);
    CHECK(predicate_out == c.at_open_misses);
    CHECK(calendar_out == c.at_open_misses);
}

}  // namespace

int main() {
    std::printf("Aggregated bars opening inside a break, against TradingView\n");
    const Case cases[] = {
        {"ksw-flags-tse7203-60", "cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "60", 10},
        {"ksw-flags-tse7203-240", "cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "240", 0},
        {"ksw-flags-zc1-60", "cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "60", 10},
        {"ksw-flags-zc1-240", "cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "240", 0},
    };
    for (const Case& c : cases) replay(c);
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
