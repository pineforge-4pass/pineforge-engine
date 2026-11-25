/*
 * test_session_break_open_bar_tape.cpp -- R5 lane K-SESSION-WINDOWS, item F7.
 *
 * An aggregated chart bar whose nominal open falls inside a session's break.
 * On TSE:7203 (0900-1130,1230-1530 Asia/Tokyo) the 60-minute bar opening at
 * 12:00 holds the 12:30 reopen; on CBOT:ZC1! (1900-0745,0830-1320
 * America/Chicago) the one opening at 08:00 holds the 08:30 reopen.
 *
 * TradingView (tests/fixtures/session_windows, recorded reference runs of
 * this lane): both bars are in market, neither pre- nor post-market, and
 * neither is a session day's first or last bar; its 60- and 240-minute bars
 * open at the same instants as the engine's aggregation of TradingView's own
 * 15-minute bars of the same window.
 *
 * The engine agrees where it answers per bar: the kernel reads such a bar at
 * its interval's first eligible instant, the reopen (lane F1), and the Pine
 * host's session_ismarket_ is that fact; the session-day flags and the
 * pre- / post-market predicates are TradingView's on every bar. So nothing
 * changes on those sessions. The clipped split-session charts exercise both
 * direct chart input and aggregation against the same chart-open oracle.
 *
 * What does not agree is a reading at the bar's open instant: the
 * time-of-day predicate pine_session_ismarket and the session calendar asked
 * at the open -- the reading the transpiler emits for a chart's
 * session.ismarket (codegen session_market.py) -- put both bars out of
 * market. Pinned per tape (RESIDUAL): 10 bars on each 60-minute tape, none at
 * 240 minutes, whose bars open in session. Lowering session.ismarket to
 * session_ismarket_ is the transpiler's change to make.
 */

#include "session_day_tape_join.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/session_time.hpp>
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

void replay(const session_tape::ChartCase& c) {
    std::vector<session_tape::TapeBar> tape;
    std::vector<Bar> bars;
    const bool ready = session_tape::inputs(c, PINEFORGE_KSW_FIXTURE_DIR, tape, bars);
    CHECK(ready);
    if (!ready) return;
    FlagsHost host(c.session, c.zone);
    host.run(bars.data(), static_cast<int>(bars.size()), c.input_tf, c.chart_tf);
    const auto state = host.native_state();
    const auto calendar = native_calendar::parse_session(c.session, c.zone);
    const bool ran = calendar && host.last_error().empty() && host.last_run_status() == 0
        && state.kind == NativeLifecycleKind::Completed && state.completion == NativeCompletion::BatchComplete;
    CHECK(ran);
    if (!ran) {
        std::printf("  NOT COMPARED %s: calendar=%d incomplete Pine batch engine=%zu tape=%zu error=%s\n",
                    session_tape::arm(c).c_str(), calendar.has_value(), host.seen.size(),
                    tape.size(), host.last_error().c_str());
        return;
    }
    int market = 0, pre = 0, post = 0, first = 0, last = 0, first_r = 0, last_r = 0;
    int predicate_out = 0, calendar_out = 0;
    std::size_t nonfinal = 0;
    bool resolved = true;
    const bool aligned = session_tape::join(c, host.seen, tape,
        [&](const Seen& got, const session_tape::TapeBar& row, bool final) {
            const auto& tv = row.tv;
            const auto miss = [&](const char* name, bool actual, bool expected) {
                return session_tape::miss(c, row.ts, name, actual, expected);
            };
            market += miss("ismarket", got.market, tv.market);
            pre += miss("ispremarket", got.premarket, tv.premarket);
            post += miss("ispostmarket", got.postmarket, tv.postmarket);
            first += miss("isfirstbar", got.first, tv.first);
            first_r += miss("isfirstbar_regular", got.first_regular, tv.first_regular);
            if (!final) {
                ++nonfinal;
                last += miss("islastbar", got.last, tv.last);
                last_r += miss("islastbar_regular", got.last_regular, tv.last_regular);
            } else {
                std::printf("  EXCLUDED %s open_ms=%lld final islastbar/islastbar_regular"
                            " engine=%d/%d tv=%d/%d\n", session_tape::arm(c).c_str(),
                            static_cast<long long>(row.ts), got.last, got.last_regular, tv.last, tv.last_regular);
            }
            predicate_out += tv.market && !got.at_open_predicate;
            const auto day = native_calendar::session_day_at(*calendar, row.ts);
            if (day) calendar_out += tv.market && !day->in_session_at(row.ts);
            else {
                resolved = false;
                std::printf("  NOT COMPARED %s calendar open_ms=%lld\n", session_tape::arm(c).c_str(),
                            static_cast<long long>(row.ts));
            }
        });
    CHECK(aligned);
    CHECK(resolved);
    if (!aligned || !resolved) return;
    CHECK(nonfinal > 0 && nonfinal + 1 == tape.size());
    std::printf("  %s matched misses market=%d pre=%d post=%d first=%d first_regular=%d"
                " last=%d last_regular=%d on %zu nonfinal; at-open predicate=%d calendar=%d\n",
                session_tape::arm(c).c_str(), market, pre, post, first, first_r, last, last_r,
                nonfinal, predicate_out, calendar_out);
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
    for (const auto& c : session_tape::split_charts) replay(c);
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
