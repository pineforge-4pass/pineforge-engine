/*
 * test_native_session_day_bar_open.cpp -- R5 lane K-SESSION-WINDOWS, item F1.
 *
 * The kernel's session-day facts describe the script bar under delivery
 * (NativeDecisionContext::in_session / opens_session_day /
 * closes_session_day). A bar is read at its own label, or -- when that label
 * falls before the first eligible instant of its interval, a bar opening
 * inside a declared break -- at that first eligible instant, never at an
 * instant before the label.
 *
 * TradingView (tests/fixtures/session_windows, README.md there):
 *   - NASDAQ:AAPL 60 with extended hours: bars open on the hour, 04:00 to
 *     19:00 ET, off the regular session's 09:30-anchored grid. The 09:00 bar,
 *     which holds the 09:30 open, and the 16:00 bar are out of market; the
 *     regular day's first bar is 10:00 (isfirstbar_regular) and its last
 *     15:00 (islastbar_regular).
 *   - TSE:7203 and CBOT:ZC1! at 60 and 240 minutes: the bars that open inside
 *     a break (TSE 12:00 before the 12:30 reopen, ZC 08:00 before 08:30) are
 *     in market, and one first and one last bar a day.
 *
 * Before this lane the facts read a bar at its interval's first eligible
 * instant, whatever its label: the 16:00 bar's interval on the 09:30 grid is
 * [15:30, 16:30), so it read in session and closed the day in place of 15:00
 * (lane CG-ISMARKET finding F1): in_session missed 10 bars and
 * closes_session_day 20 on the extended tape. Reading at the label alone
 * would lose the break-open bars; the later of the two keeps both.
 *
 * Kernel-only: a bare NativeStrategyHost, FeedTolerant labels as the Pine
 * adapter declares them.
 */

#include "session_flag_tape_fixture.hpp"

#include <pineforge/native_host.hpp>

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

struct Facts {
    std::int64_t ts = 0;
    bool in = false, opens = false, closes = false;
};

class Probe final : public NativeStrategyHost {
public:
    std::vector<Facts> bars;
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        bars.push_back({context.script_bar_open_ms, context.in_session, context.opens_session_day,
                        context.closes_session_day});
    }
};

struct Case {
    const char* slug;       // the chart TradingView flagged
    const char* feed_slug;  // the tape whose bar times feed the run
    const char* session;
    const char* timezone;
    const char* input_tf;
    const char* script_tf;
};

void replay(const Case& c) {
    bool ok = true;
    const auto tape = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR, c.slug, ok);
    const auto feed_tape = session_tape::read_tape(PINEFORGE_KSW_FIXTURE_DIR, c.feed_slug, ok);
    CHECK(ok);
    std::vector<Bar> bars;
    for (const auto& bar : feed_tape) bars.push_back(session_tape::flat_bar(bar.ts));

    NativeRunSpec spec;
    spec.identity = {std::string("ksw-f1-") + c.slug, 1};
    spec.input_tf = c.input_tf;
    spec.script_tf = c.script_tf;
    spec.tickerid = "TEST:KSW";
    spec.timezone = c.timezone;
    spec.session = c.session;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    Probe host;
    const bool configured = host.configure_native(spec).status == NativeSetupStatus::Applied;
    CHECK(configured);
    if (configured) host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());

    // One script bar per chart bar, at TradingView's own open.
    const bool aligned = host.bars.size() == tape.size();
    CHECK(aligned);
    int label_misses = 0, in_misses = 0, open_misses = 0, close_misses = 0;
    for (std::size_t i = 0; aligned && i < tape.size(); ++i) {
        const Facts& got = host.bars[i];
        const auto& tv = tape[i].tv;
        label_misses += got.ts != tape[i].ts;
        in_misses += got.in != tv.market;
        open_misses += got.opens != tv.first_regular;
        close_misses += got.closes != tv.last_regular;
    }
    std::printf("  %-24s %-20s %s->%-3s %3zu bars: misses label %d in_session %d opens %d closes %d\n",
                c.slug, c.session, c.input_tf, c.script_tf, tape.size(), label_misses, in_misses,
                open_misses, close_misses);
    CHECK(label_misses == 0);
    CHECK(in_misses == 0);
    CHECK(open_misses == 0);
    CHECK(close_misses == 0);
}

}  // namespace

int main() {
    std::printf("Session-day facts per script bar, against TradingView\n");
    const Case cases[] = {
        // Extended hours: bars off the regular session's grid.
        {"cgim-flags-aapl-60-ext", "cgim-flags-aapl-60-ext", "0930-1600", "America/New_York", "60", "60"},
        {"cgs2-hist-aapl-15-ext", "cgs2-hist-aapl-15-ext", "0930-1600", "America/New_York", "15", "15"},
        // Regular hours, the control.
        {"cgim-flags-aapl-60-reg", "cgim-flags-aapl-60-reg", "0930-1600", "America/New_York", "60", "60"},
        {"cgs2-hist-aapl-15-reg", "cgs2-hist-aapl-15-reg", "0930-1600", "America/New_York", "15", "15"},
        // A bar opening inside a break, aggregated from TradingView's own
        // 15-minute bars of the same window: in market.
        {"ksw-flags-tse7203-60", "cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "15", "60"},
        {"ksw-flags-tse7203-240", "cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "15", "240"},
        {"ksw-flags-zc1-60", "cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "15", "60"},
        {"ksw-flags-zc1-240", "cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "15", "240"},
    };
    for (const Case& c : cases) replay(c);
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
