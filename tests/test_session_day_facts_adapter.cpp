/*
 * test_session_day_facts_adapter.cpp — R5 lane F5.
 *
 * The Pine adapter reads session.ismarket / session.isfirstbar /
 * session.islastbar off the kernel: PineStrategyHost selects the session-day
 * facts the native decision context carries (tests/test_native_session_day_facts.cpp
 * pins them on a bare host) instead of computing a session-day rule of its
 * own. What stays in the adapter is policy about the bars the run is fed:
 * the live runner's probe protocol (set_realtime_tail) treats a batch's final
 * bar as still forming, so it reads the kernel's open-ended close instead of
 * the batch's run-end one.
 *
 * Three things are pinned here, all through the adapter as generated code
 * reads it (the session_* members, inside on_source_bar):
 *
 *   1. On the TradingView tapes lanes E25/E26 committed
 *      (tests/fixtures/session_islastbar) and on the other session shapes the
 *      E26 witness drives, every published bar carries exactly the facts the
 *      kernel presented for it, flag for flag — chart, aggregated, magnified,
 *      streamed — and they are TradingView's.
 *   2. The live-probe tail: its final bar closes its session day only where
 *      the calendar says so; a plain batch keeps the run-end convention.
 *   3. calc_on_order_fills. The adapter's own lookahead indexed its retained
 *      input by the count of source bars already published, and a fill
 *      recalculation publishes the bar it fills on before that bar's close,
 *      so on such a bar it looked TWO bars ahead: a fill on a session day's
 *      second-to-last bar flagged that bar islastbar and the day's real last
 *      bar isfirstbar. The kernel reads the bar after the bar, so both flags
 *      are TradingView's again.
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

#ifndef PINEFORGE_F5_FIXTURE_DIR
#error "PINEFORGE_F5_FIXTURE_DIR must name tests/fixtures/session_islastbar"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/session_islastbar/bars.inc"

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kDay = 1440 * kMinute;
// 2026-04-07 (Tuesday, EDT) 09:30 America/New_York.
constexpr std::int64_t kTue0930Et = 1775568600000LL;

const std::string kRth = "0930-1600";
const std::string kNewYork = "America/New_York";

// One source callback: what generated code read, and what the kernel
// presented at the same decision point.
struct Seen {
    std::int64_t ts = 0;
    bool close_calculation = false;
    bool ismarket = false;
    bool isfirstbar = false;
    bool islastbar = false;
    bool kernel = false;
    bool in_session = false;
    bool opens = false;
    bool closes = false;
    bool open_ended = false;
};

class SessionHost final : public source::PineStrategyHost {
public:
    SessionHost(const std::string& session, const std::string& timezone,
                bool coof = false) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
        fixture_configuration().calc_on_order_fills = coof;
    }

    void on_source_bar(const Bar&) override {
        Seen s;
        s.ts = current_bar_.timestamp;
        s.ismarket = session_ismarket_;
        s.isfirstbar = session_isfirstbar_;
        s.islastbar = session_islastbar_;
        if (const auto point = current_execution_point()) {
            const NativeDecisionContext& facts = point->decision;
            s.kernel = true;
            s.close_calculation =
                facts.coordinate.provenance == NativePriceProvenance::Calculation;
            s.in_session = facts.in_session;
            s.opens = facts.opens_session_day;
            s.closes = facts.closes_session_day;
            s.open_ended = facts.closes_session_day_open_ended;
        }
        seen.push_back(s);
        if (enter_at_bar >= 0 && bar_index_ == enter_at_bar && !entered) {
            entered = true;
            strategy_entry("L", true);
        }
    }

    std::vector<Seen> seen;
    int enter_at_bar = -1;
    bool entered = false;
};

struct Run {
    std::vector<Seen> seen;
    std::string error;
};

Run run_batch(const std::vector<Bar>& bars, const std::string& session,
              const std::string& timezone, const char* input_tf, const char* script_tf,
              bool magnifier) {
    SessionHost host(session, timezone);
    host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf, magnifier);
    return {host.seen, host.last_error()};
}

Bar flat_bar(std::int64_t ts) {
    Bar b{};
    b.timestamp = ts;
    b.open = 100.0;
    b.high = 101.0;
    b.low = 99.0;
    b.close = 100.5;
    b.volume = 1.0;
    return b;
}

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
        b.open = row.open;
        b.high = row.high;
        b.low = row.low;
        b.close = row.close;
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

// Every published bar carries the facts the kernel presented at the same
// point: the adapter selects, it does not compute.
bool selects_kernel_facts(const std::vector<Seen>& seen) {
    for (const Seen& s : seen) {
        if (!s.kernel || s.ismarket != s.in_session || s.isfirstbar != s.opens
            || s.islastbar != s.closes)
            return false;
    }
    return !seen.empty();
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t utc_ms(int y, int mo, int d, int h, int mi, int offset_hours = 0) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - offset_hours) * 60 + mi) * kMinute;
}

std::set<std::int64_t> tape_flags(const char* slug, std::int64_t first, std::int64_t last) {
    std::ifstream in(std::string(PINEFORGE_F5_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::set<std::int64_t> stamps;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
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

// ── 1. the tapes, flag for flag with the kernel ───────────────────────────

void test_tapes_select_the_kernel_facts() {
    std::printf("test_tapes_select_the_kernel_facts\n");
    const Run chart = run_batch(feed(kFord15), kRth, kNewYork, "15", "15", false);
    const Run agg = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", false);
    const Run agg_mag = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", true);
    for (const Run* run : {&chart, &agg, &agg_mag}) {
        CHECK(run->error.empty());
        CHECK(run->seen.size() == 66);
        CHECK(selects_kernel_facts(run->seen));
    }
    if (!chart.seen.empty()) {
        const std::int64_t first = chart.seen.front().ts;
        const std::int64_t last = chart.seen.back().ts;
        CHECK(flagged(chart.seen, &Seen::islastbar)
              == tape_flags("e25-f-islastbar", first, last));
        CHECK(flagged(chart.seen, &Seen::isfirstbar)
              == tape_flags("e25-f-isfirstbar", first, last));
    }

    const Run eth = run_batch(feed(kEth15), "24x7", "UTC", "15", "15", false);
    const Run eth_agg = run_batch(feed(kEth1m), "24x7", "UTC", "1", "15", false);
    for (const Run* run : {&eth, &eth_agg}) {
        CHECK(run->error.empty());
        CHECK(selects_kernel_facts(run->seen));
        // 23:45 is TradingView's; 00:45 is the run-end convention.
        CHECK(bits(run->seen, &Seen::islastbar) == "00010001");
        CHECK(bits(run->seen, &Seen::isfirstbar) == "10001000");
    }

    const Run forex = run_batch(ladder(kTue0930Et + 420 * kMinute, kTue0930Et + 480 * kMinute,
                                       5 * kMinute), "1700-1700", kNewYork, "5", "5", false);
    CHECK(forex.error.empty());
    CHECK(selects_kernel_facts(forex.seen));
    CHECK(bits(forex.seen, &Seen::islastbar) == "0000010000001");
    CHECK(bits(forex.seen, &Seen::isfirstbar) == "1000001000000");

    // A daily chart: every bar is its whole session day.
    const Run daily = run_batch(ladder(kTue0930Et, kTue0930Et + 4 * kDay, kDay), kRth, kNewYork,
                                "D", "D", false);
    CHECK(daily.error.empty());
    CHECK(selects_kernel_facts(daily.seen));
    CHECK(bits(daily.seen, &Seen::ismarket) == "11111");
    CHECK(bits(daily.seen, &Seen::isfirstbar) == "11111");
    CHECK(bits(daily.seen, &Seen::islastbar) == "11111");
}

// A stream's warmup and realtime bars, confirmed and printed.
void test_streams_select_the_kernel_facts() {
    std::printf("test_streams_select_the_kernel_facts\n");
    for (const char* input_tf : {"5", "1"}) {
        SessionHost host(kRth, kNewYork);
        const std::int64_t step = std::string(input_tf) == "5" ? 5 * kMinute : kMinute;
        const std::int64_t warmup_last = std::string(input_tf) == "5"
            ? kTue0930Et + 375 * kMinute : kTue0930Et + 379 * kMinute;
        const auto warmup = ladder(kTue0930Et + 360 * kMinute, warmup_last, step);
        CHECK(host.stream_begin(warmup.data(), static_cast<int>(warmup.size()), input_tf, "5"));
        std::uint64_t sequence = 0;
        for (std::int64_t ts = kTue0930Et + 380 * kMinute; ts < kTue0930Et + 390 * kMinute;
             ts += kMinute) {
            CHECK(host.stream_push_tick(TradeTick{ts + 1000, ++sequence, 100.0, 1.0}));
        }
        CHECK(host.stream_advance_time(kTue0930Et + 395 * kMinute));
        CHECK(host.stream_end(false));
        CHECK(host.last_error().empty());
        CHECK(selects_kernel_facts(host.seen));
        CHECK(bits(host.seen, &Seen::islastbar) == "000001");
    }
}

// ── 2. the live-probe tail ────────────────────────────────────────────────

// set_realtime_tail marks a batch's final bar as still forming. It then closes
// its session day only where the calendar does: 15:40 does not (15:45 is in
// session), 15:55 does (16:00 is not). Without the tail the batch's final
// bar keeps the run-end convention. Every other bar is the kernel's.
void test_live_probe_tail_reads_the_schedule() {
    std::printf("test_live_probe_tail_reads_the_schedule\n");
    for (const bool tail : {false, true}) {
        for (const std::int64_t last_bar : {kTue0930Et + 370 * kMinute,
                                            kTue0930Et + 385 * kMinute}) {
            SessionHost host(kRth, kNewYork);
            if (tail) host.set_realtime_tail(true, 1000);
            const auto bars = ladder(kTue0930Et + 360 * kMinute, last_bar, 5 * kMinute);
            host.run(bars.data(), static_cast<int>(bars.size()), "5", "5");
            CHECK(host.last_error().empty());
            CHECK(host.seen.size() == bars.size());
            if (host.seen.empty()) continue;
            const Seen& final_bar = host.seen.back();
            const bool session_end = last_bar == kTue0930Et + 385 * kMinute;
            CHECK(final_bar.kernel);
            CHECK(final_bar.open_ended == session_end);
            CHECK(final_bar.closes);  // a batch is complete input
            CHECK(final_bar.islastbar == (tail ? session_end : true));
            const std::vector<Seen> before(host.seen.begin(), host.seen.end() - 1);
            CHECK(selects_kernel_facts(before));
        }
    }
}

// ── 3. calc_on_order_fills ────────────────────────────────────────────────

// Tuesday 15:15, 15:30, 15:45 and Wednesday 09:30, 09:45 (15m, regular
// hours). The strategy enters at 15:15's close; the entry fills at 15:30's
// open and drives a fill recalculation on 15:30 before 15:30's close. At
// every CLOSE calculation the flags are TradingView's: 15:45 is Tuesday's last
// bar and 09:30 Wednesday's first, whatever filled on 15:30. (The flags a
// recalculation itself reads are lane F1's.)
void test_fill_recalculation_keeps_the_close_flags() {
    std::printf("test_fill_recalculation_keeps_the_close_flags\n");
    auto bars = ladder(kTue0930Et + 345 * kMinute, kTue0930Et + 375 * kMinute, 15 * kMinute);
    const auto wednesday = ladder(kTue0930Et + kDay, kTue0930Et + kDay + 15 * kMinute,
                                  15 * kMinute);
    bars.insert(bars.end(), wednesday.begin(), wednesday.end());
    SessionHost host(kRth, kNewYork, /*coof=*/true);
    host.enter_at_bar = 0;
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().empty());
    std::vector<Seen> closes;
    for (const Seen& s : host.seen)
        if (s.close_calculation) closes.push_back(s);
    // The fixture really drove a fill recalculation.
    CHECK(host.seen.size() > closes.size());
    CHECK(closes.size() == 5);
    CHECK(bits(closes, &Seen::ismarket) == "11111");
    CHECK(bits(closes, &Seen::isfirstbar) == "10010");
    CHECK(bits(closes, &Seen::islastbar) == "00101");
    CHECK(selects_kernel_facts(closes));
    if (bits(closes, &Seen::islastbar) != "00101" || bits(closes, &Seen::isfirstbar) != "10010") {
        std::printf("    close callbacks: isfirstbar %s islastbar %s (kernel opens %s closes %s)\n",
                    bits(closes, &Seen::isfirstbar).c_str(),
                    bits(closes, &Seen::islastbar).c_str(), bits(closes, &Seen::opens).c_str(),
                    bits(closes, &Seen::closes).c_str());
    }
}

}  // namespace

int main() {
    test_tapes_select_the_kernel_facts();
    test_streams_select_the_kernel_facts();
    test_live_probe_tail_reads_the_schedule();
    test_fill_recalculation_keeps_the_close_flags();

    std::printf("\nsession_day_facts_adapter: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
