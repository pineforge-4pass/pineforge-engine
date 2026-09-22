/*
 * test_native_session_day_facts.cpp — R5 lane F5.
 *
 * Session-day facts are a generic native feature: every decision context the
 * kernel presents carries, for the script bar under delivery,
 *
 *   in_session                      the bar's label is in session on the run's
 *                                   own calendar (NativeRunSpec::session /
 *                                   ::timezone, native_calendar::in_session);
 *   opens_session_day               ...and the bar before it is not, or belongs
 *                                   to another session day;
 *   closes_session_day              ...and the bar after it is not, or belongs
 *                                   to another session day;
 *   closes_session_day_open_ended   closes_session_day for a run whose input
 *                                   goes on past it: a batch's final bar is
 *                                   judged by the calendar, one width on.
 *
 * The session day is native_calendar::session_day_ordinal's: the cycle that
 * rolls at the session's first window start, keyed to its trading date, so an
 * overnight session is ONE day across local midnight. The bar before / after
 * is the one the run holds when it holds one — a batch's input, a stream's
 * warmup — and otherwise the calendar's, one script width away. The run's
 * own edges keep the convention a bar with nothing held beyond it has: the
 * first bar of a run opens its session day, and the final bar of a batch
 * closes it (a batch is complete input); a stream's bars read on, because
 * the stream continues. A bar at or above the session-day grain (D, W, M)
 * holds whole session days, so it is in session and both opens and closes.
 *
 * Until this lane the kernel computed none of this. BacktestEngine carried the
 * three Pine flags, the Pine adapter alone wrote them, and every bare host read
 * false on every bar (lane AUDIT3, report section 3.1 row 9). This witness
 * replays through a bare NativeStrategyHost the TradingView tapes lanes E25 and
 * E26 committed (tests/fixtures/session_islastbar) and the audit's acid shape
 * (a Tokyo session that crosses local midnight), in batch, aggregated and
 * streamed, and pins where the facts are read on a fill recalculation.
 *
 * Kernel-only: it includes no pineforge/source header, so it also runs in the
 * kernel profile.
 */

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

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

// One decision context, as a host reads it.
struct Facts {
    std::int64_t ts = 0;
    bool in = false;
    bool opens = false;
    bool closes = false;
    bool open_ended = false;
};

Facts facts_of(const NativeDecisionContext& context) {
    return {context.script_bar_open_ms, context.in_session, context.opens_session_day,
            context.closes_session_day, context.closes_session_day_open_ended};
}

class Probe final : public NativeStrategyHost {
public:
    std::vector<Facts> bars;       // every script calculation (BarClose)
    std::vector<Facts> bar_opens;  // every bar-open decision point
    std::vector<Facts> refills;    // every OrderFill recalculation
    std::vector<Facts> applied;    // every applied execution
    std::vector<Facts> ticks;      // every realtime print
    std::vector<std::uint64_t> continuation_at_bar;
    std::function<void(Probe&, const NativeDecisionContext&)> at_bar;

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        bars.push_back(facts_of(context));
        continuation_at_bar.push_back(native_continuation_hash());
        if (at_bar) at_bar(*this, context);
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext& context) override {
        bar_opens.push_back(facts_of(context));
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& context,
                               NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        if (reason == NativeCalculationReason::OrderFill) {
            refills.push_back(facts_of(context));
            return;
        }
        NativeStrategyHost::on_native_recalculate(bar, context, reason, cause);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext& context) override {
        applied.push_back(facts_of(context));
    }
    void on_native_tick(const Bar&, const NativeTickContext& context) override {
        ticks.push_back(facts_of(context.decision));
    }
};

NativeRunSpec spec_for(const std::string& session, const std::string& timezone,
                       const char* input_tf, const char* script_tf) {
    NativeRunSpec spec;
    spec.identity = {"f5-session-day", 1};
    spec.input_tf = input_tf;
    spec.script_tf = script_tf;
    spec.tickerid = "TEST:F5";
    spec.timezone = timezone;
    spec.session = session;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

struct Run {
    std::vector<Facts> bars;
    std::vector<Facts> ticks;
    std::string error;
};

void configure(Probe& host, const NativeRunSpec& spec) {
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
}

Run batch(const std::vector<Bar>& bars, const NativeRunSpec& spec) {
    Probe host;
    configure(host, spec);
    host.run(bars.data(), static_cast<int>(bars.size()));
    return {host.bars, host.ticks, host.last_error()};
}

// A stream over the same bars: the first `warmup` bars begin it, every later
// bar is pushed as a confirmed realtime input.
Run streamed(const std::vector<Bar>& bars, std::size_t warmup, const NativeRunSpec& spec) {
    Probe host;
    configure(host, spec);
    const bool began = host.stream_begin(bars.data(), static_cast<int>(warmup),
                                         spec.input_tf, spec.script_tf);
    CHECK(began);
    for (std::size_t i = warmup; began && i < bars.size(); ++i) {
        CHECK(host.stream_push_bar(bars[i]));
    }
    CHECK(host.stream_end(false));
    return {host.bars, host.ticks, host.last_error()};
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
        b.open = row.open;
        b.high = row.high;
        b.low = row.low;
        b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

std::string bits(const std::vector<Facts>& seen, bool Facts::*flag) {
    std::string text;
    for (const Facts& facts : seen) text += (facts.*flag) ? '1' : '0';
    return text;
}

std::set<std::int64_t> flagged(const std::vector<Facts>& seen, bool Facts::*flag) {
    std::set<std::int64_t> stamps;
    for (const Facts& facts : seen)
        if (facts.*flag) stamps.insert(facts.ts);
    return stamps;
}

std::vector<int> indices(const std::vector<Facts>& seen, bool Facts::*flag) {
    std::vector<int> at;
    for (std::size_t i = 0; i < seen.size(); ++i)
        if (seen[i].*flag) at.push_back(static_cast<int>(i));
    return at;
}

bool same_facts(const std::vector<Facts>& a, const std::vector<Facts>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].ts != b[i].ts || a[i].in != b[i].in || a[i].opens != b[i].opens
            || a[i].closes != b[i].closes || a[i].open_ended != b[i].open_ended)
            return false;
    }
    return true;
}

void show(const char* tag, const Run& run) {
    std::printf("    %-28s n=%zu in        %s\n", tag, run.bars.size(),
                bits(run.bars, &Facts::in).c_str());
    std::printf("    %-28s      opens     %s\n", "", bits(run.bars, &Facts::opens).c_str());
    std::printf("    %-28s      closes    %s\n", "", bits(run.bars, &Facts::closes).c_str());
    std::printf("    %-28s      open-end  %s\n", "", bits(run.bars, &Facts::open_ended).c_str());
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

// TradingView's own flags: every tape entry fills at the close of a bar
// TradingView flagged, and the tape (times UTC+8) dates it at that bar's open.
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

// ── 1. TradingView's NYSE:F tapes, through a bare host ────────────────────

// 2025-07-02, 2025-07-03 (half day, last bar 12:45 ET) and 2025-07-07: the
// same bars the Pine adapter's witness replays. A bare host reads exactly
// TradingView's flags off the kernel, on the chart timeframe and aggregated
// from the 1m feed alike. The half day's 12:45 closes its day because the
// bar the run holds after it is the next session day's — the session string
// says 16:00, and knows nothing of the early close.
void test_tv_nyse_f() {
    std::printf("test_tv_nyse_f\n");
    const auto spec15 = spec_for(kRth, kNewYork, "15", "15");
    const auto spec1 = spec_for(kRth, kNewYork, "1", "15");
    const Run chart = batch(feed(kFord15), spec15);
    const Run agg = batch(feed(kFord1m), spec1);
    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    CHECK(chart.bars.size() == 66);
    CHECK(same_facts(agg.bars, chart.bars));
    if (chart.bars.size() != 66 || !same_facts(agg.bars, chart.bars)) {
        show("chart 15 -> 15", chart);
        show("aggregated 1 -> 15", agg);
    }
    if (chart.bars.empty()) return;
    const std::int64_t first = chart.bars.front().ts;
    const std::int64_t last = chart.bars.back().ts;
    CHECK(first == utc_ms(2025, 7, 2, 13, 30));
    CHECK(last == utc_ms(2025, 7, 7, 19, 45));
    CHECK(bits(chart.bars, &Facts::in) == std::string(66, '1'));

    const auto tv_last = tape_flags("e25-f-islastbar", first, last);
    const auto tv_first = tape_flags("e25-f-isfirstbar", first, last);
    CHECK((tv_last == std::set<std::int64_t>{utc_ms(2025, 7, 2, 19, 45),
                                             utc_ms(2025, 7, 3, 16, 45),
                                             utc_ms(2025, 7, 7, 19, 45)}));
    CHECK((tv_first == std::set<std::int64_t>{utc_ms(2025, 7, 2, 13, 30),
                                              utc_ms(2025, 7, 3, 13, 30),
                                              utc_ms(2025, 7, 7, 13, 30)}));
    for (const Run* run : {&chart, &agg}) {
        CHECK(flagged(run->bars, &Facts::closes) == tv_last);
        CHECK(flagged(run->bars, &Facts::opens) == tv_first);
        // The batch's final bar, 07-07 15:45, ends its day on the calendar as
        // well, so the open-ended reading is the same set.
        CHECK(flagged(run->bars, &Facts::open_ended) == tv_last);
    }
    // The bar-open decision point of every script bar reads the same facts
    // as its calculation.
    Probe host;
    configure(host, spec15);
    const auto bars = feed(kFord15);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(same_facts(host.bar_opens, host.bars));
}

// ── 2. TradingView's 24x7 tape ────────────────────────────────────────────

// BINANCE:ETHUSDT.P 2025-06-10 23:00 .. 06-11 00:59 UTC. A 24x7 session day
// ends at midnight UTC: 23:45 closes it, 00:00 opens the next. The run's own
// edges keep their convention: 23:00 opens (nothing before it is held) and
// 00:45 closes (a batch is complete input) — outside the tape, whose chart
// goes on past the window. Read open-ended, 00:45 is followed by 01:00 on the
// same day, and the closes are TradingView's tape exactly.
void test_tv_eth_24x7() {
    std::printf("test_tv_eth_24x7\n");
    const Run chart = batch(feed(kEth15), spec_for("24x7", "UTC", "15", "15"));
    const Run agg = batch(feed(kEth1m), spec_for("24x7", "UTC", "1", "15"));
    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    CHECK(chart.bars.size() == 8);
    CHECK(same_facts(agg.bars, chart.bars));
    if (chart.bars.size() != 8 || !same_facts(agg.bars, chart.bars)) {
        show("chart 15 -> 15", chart);
        show("aggregated 1 -> 15", agg);
    }
    if (chart.bars.empty()) return;
    const auto tv_last = tape_flags("e25-eth-islastbar", chart.bars.front().ts,
                                    chart.bars.back().ts);
    CHECK((tv_last == std::set<std::int64_t>{utc_ms(2025, 6, 10, 23, 45)}));
    CHECK(bits(chart.bars, &Facts::in) == "11111111");
    CHECK(bits(chart.bars, &Facts::closes) == "00010001");
    CHECK(bits(chart.bars, &Facts::opens) == "10001000");
    CHECK(bits(chart.bars, &Facts::open_ended) == "00010000");
    CHECK(flagged(chart.bars, &Facts::open_ended) == tv_last);
}

// ── 3. an overnight session that crosses local midnight ───────────────────

// The audit's acid shape: a Tokyo session 22:30-05:00, three session days of
// 26 fifteen-minute bars (78). The session day is the trading date of the
// 05:00 close, so Tokyo midnight is not a boundary: the days open at bars 0,
// 26, 52 and close at bars 25, 51, 77, in batch and streamed alike — the
// stream warms up on day one and pushes the other two as realtime bars.
void test_overnight_tokyo_session() {
    std::printf("test_overnight_tokyo_session\n");
    std::vector<Bar> bars;
    for (int night = 0; night < 3; ++night) {
        // 2026-07-06 22:30 JST == 13:30Z; each night's 26 bars end 04:45 JST.
        const std::int64_t open = utc_ms(2026, 7, 6, 13, 30) + night * kDay;
        const auto session = ladder(open, open + 25 * 15 * kMinute, 15 * kMinute);
        bars.insert(bars.end(), session.begin(), session.end());
    }
    const auto spec = spec_for("2230-0500", "Asia/Tokyo", "15", "15");
    const Run run = batch(bars, spec);
    const Run stream = streamed(bars, 26, spec);
    CHECK(run.error.empty());
    CHECK(stream.error.empty());
    CHECK(run.bars.size() == 78);
    CHECK(bits(run.bars, &Facts::in) == std::string(78, '1'));
    CHECK((indices(run.bars, &Facts::opens) == std::vector<int>{0, 26, 52}));
    CHECK((indices(run.bars, &Facts::closes) == std::vector<int>{25, 51, 77}));
    CHECK((indices(run.bars, &Facts::open_ended) == std::vector<int>{25, 51, 77}));
    CHECK(same_facts(stream.bars, run.bars));
    if (run.bars.size() != 78 || !same_facts(stream.bars, run.bars)) {
        show("batch 15 -> 15", run);
        show("stream 15 -> 15", stream);
    }
}

// ── 4. out-of-session bars, one and two sessions ──────────────────────────

// 5m bars 15:30 .. 16:10 ET: six in session, three after the close. 15:55
// closes its day because the bar after it is out of session. Then the same
// Tuesday with Wednesday's pre-market 09:15 .. 09:40 behind it: 09:30 opens
// Wednesday (the bar before it is out of session) and 09:40, the batch's
// final bar, closes it by the run-end convention.
void test_out_of_session_bars() {
    std::printf("test_out_of_session_bars\n");
    const auto spec5 = spec_for(kRth, kNewYork, "5", "5");
    const auto spec1 = spec_for(kRth, kNewYork, "1", "5");
    const Run one = batch(ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 400 * kMinute,
                                 5 * kMinute), spec5);
    const Run one_agg = batch(ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 404 * kMinute,
                                     kMinute), spec1);
    CHECK(one.error.empty());
    CHECK(bits(one.bars, &Facts::in) == "111111000");
    CHECK(bits(one.bars, &Facts::opens) == "100000000");
    CHECK(bits(one.bars, &Facts::closes) == "000001000");
    CHECK(bits(one.bars, &Facts::open_ended) == "000001000");
    CHECK(same_facts(one_agg.bars, one.bars));

    auto five = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 400 * kMinute, 5 * kMinute);
    const auto wednesday = ladder(kTue0930Et + kDay - 15 * kMinute,
                                  kTue0930Et + kDay + 10 * kMinute, 5 * kMinute);
    five.insert(five.end(), wednesday.begin(), wednesday.end());
    const Run two = batch(five, spec5);
    CHECK(two.error.empty());
    CHECK(bits(two.bars, &Facts::in) == "111111000000111");
    CHECK(bits(two.bars, &Facts::opens) == "100000000000100");
    CHECK(bits(two.bars, &Facts::closes) == "000001000000001");
    CHECK(bits(two.bars, &Facts::open_ended) == "000001000000000");
    if (bits(two.bars, &Facts::closes) != "000001000000001") show("two sessions 5 -> 5", two);
}

// ── 5. the session day rolls at the session's own start, not at midnight ──

// A 1700-1700 America/New_York session never leaves the market; its day rolls
// at 17:00. 16:55 closes one day and 17:00 opens the next, on the same local
// date, on the chart timeframe and aggregated from 1m alike.
void test_overnight_session_day_roll() {
    std::printf("test_overnight_session_day_roll\n");
    const Run chart = batch(ladder(kTue0930Et + 420 * kMinute, kTue0930Et + 480 * kMinute,
                                   5 * kMinute), spec_for("1700-1700", kNewYork, "5", "5"));
    const Run agg = batch(ladder(kTue0930Et + 420 * kMinute, kTue0930Et + 484 * kMinute,
                                 kMinute), spec_for("1700-1700", kNewYork, "1", "5"));
    CHECK(chart.error.empty());
    CHECK(chart.bars.size() == 13);
    CHECK(bits(chart.bars, &Facts::in) == "1111111111111");
    CHECK(bits(chart.bars, &Facts::closes) == "0000010000001");
    CHECK(bits(chart.bars, &Facts::opens) == "1000001000000");
    CHECK(bits(chart.bars, &Facts::open_ended) == "0000010000000");
    CHECK(same_facts(agg.bars, chart.bars));
}

// ── 6. a stream's warmup and realtime prints ──────────────────────────────

// Warmup 15:30 .. 15:45 ET (5m), realtime prints 15:50 .. 15:59, the clock
// then advanced past 16:00. The warmup's final bar does not close the day:
// the stream goes on, and one width on is 15:50, in session. 15:55 does. A
// print's decision context carries the facts of the bar it is forming.
void test_stream_realtime_prints() {
    std::printf("test_stream_realtime_prints\n");
    for (const char* input_tf : {"5", "1"}) {
        Probe host;
        configure(host, spec_for(kRth, kNewYork, input_tf, "5"));
        const std::int64_t step = std::string(input_tf) == "5" ? 5 * kMinute : kMinute;
        const std::int64_t warmup_last = std::string(input_tf) == "5"
            ? kTue0930Et + 375 * kMinute : kTue0930Et + 379 * kMinute;
        const auto warmup = ladder(kTue0930Et + 360 * kMinute, warmup_last, step);
        CHECK(host.stream_begin(warmup.data(), static_cast<int>(warmup.size()),
                                input_tf, "5"));
        std::uint64_t sequence = 0;
        for (std::int64_t ts = kTue0930Et + 380 * kMinute; ts < kTue0930Et + 390 * kMinute;
             ts += kMinute) {
            CHECK(host.stream_push_tick(TradeTick{ts + 1000, ++sequence, 100.0, 1.0}));
        }
        CHECK(host.stream_advance_time(kTue0930Et + 395 * kMinute));
        CHECK(host.stream_end(false));
        const Run run{host.bars, host.ticks, host.last_error()};
        CHECK(run.error.empty());
        CHECK(bits(run.bars, &Facts::in) == "111111");
        CHECK(bits(run.bars, &Facts::opens) == "100000");
        CHECK(bits(run.bars, &Facts::closes) == "000001");
        CHECK(bits(run.bars, &Facts::open_ended) == "000001");
        if (bits(run.bars, &Facts::closes) != "000001") show(input_tf, run);
        CHECK(host.ticks.size() == 10);
        CHECK(bits(host.ticks, &Facts::in) == "1111111111");
        CHECK(bits(host.ticks, &Facts::closes) == "0000011111");
    }
}

// ── 6b. the run's first bar, sealed after its warmup ──────────────────────

// A 1m -> 5m stream whose two warmup bars (04:45, 04:46 UTC) end inside the
// 04:45 script bar: the realtime inputs 04:47 .. 04:49 complete and seal it.
// It is still the run's first bar, so it opens its session day — as it does
// when a batch holds the same inputs — while 04:50, a later bar of the same
// 24x7 day, does not.
void test_stream_first_bar_sealed_in_realtime() {
    std::printf("test_stream_first_bar_sealed_in_realtime\n");
    const std::int64_t first = utc_ms(2025, 4, 10, 4, 45);
    const auto bars = ladder(first, first + 13 * kMinute, kMinute);
    const auto spec = spec_for("24x7", "UTC", "1", "5");
    const Run stream = streamed(bars, 2, spec);
    const Run run = batch(bars, spec);
    CHECK(stream.error.empty());
    CHECK(stream.bars.size() == 2);
    CHECK(bits(stream.bars, &Facts::in) == "11");
    CHECK(bits(stream.bars, &Facts::opens) == "10");
    CHECK(bits(stream.bars, &Facts::closes) == "00");
    CHECK(run.bars.size() == 2);
    CHECK(bits(run.bars, &Facts::opens) == "10");
}

// ── 7. the realtime early close a calendar cannot see (E26-6 ruling) ──────

// The 07-03 half day streamed: warmup 07-02, then 07-03's fourteen bars as
// realtime inputs. Nothing after 12:45 is held, and the calendar says 13:00
// is in session, so the stream does not close the day at 12:45 — the batch
// run of the same bars does (section 1), and TradingView's tape does. That
// is the ruled residue: a bar with no held successor reads the declared
// schedule, which carries no early closes.
void test_stream_cannot_see_an_undeclared_early_close() {
    std::printf("test_stream_cannot_see_an_undeclared_early_close\n");
    const auto all = feed(kFord15);
    const std::vector<Bar> two_days(all.begin(), all.begin() + 40);
    const auto spec = spec_for(kRth, kNewYork, "15", "15");
    const Run stream = streamed(two_days, 26, spec);
    const Run run = batch(two_days, spec);
    CHECK(stream.error.empty());
    CHECK(stream.bars.size() == 40);
    if (stream.bars.size() != 40) return;
    const std::int64_t half_day_close = utc_ms(2025, 7, 3, 16, 45);
    CHECK(stream.bars.back().ts == half_day_close);
    // The warmup's final bar still closes 07-02: one width on is 16:00.
    CHECK(stream.bars[25].closes);
    CHECK(stream.bars[26].opens);
    // The ruled residue.
    CHECK(!stream.bars.back().closes);
    CHECK(!stream.bars.back().open_ended);
    // The batch run of the same forty bars closes it by the run-end
    // convention, as the full batch in section 1 does by the next day's bar.
    CHECK(run.bars.back().closes);
    CHECK(!run.bars.back().open_ended);
}

// ── 8. daily-or-higher bars ───────────────────────────────────────────────

// A D bar holds its whole session day: in session, opening and closing it,
// whatever time of day its label reads. Fed as daily bars, and aggregated
// from 15m inputs (the first day seals when the second day's first input
// arrives).
void test_daily_bars_hold_whole_days() {
    std::printf("test_daily_bars_hold_whole_days\n");
    const Run daily = batch(ladder(kTue0930Et, kTue0930Et + 4 * kDay, kDay),
                            spec_for(kRth, kNewYork, "D", "D"));
    CHECK(daily.error.empty());
    CHECK(daily.bars.size() == 5);
    CHECK(bits(daily.bars, &Facts::in) == "11111");
    CHECK(bits(daily.bars, &Facts::opens) == "11111");
    CHECK(bits(daily.bars, &Facts::closes) == "11111");
    CHECK(bits(daily.bars, &Facts::open_ended) == "11111");

    auto intraday = ladder(kTue0930Et, kTue0930Et + 375 * kMinute, 15 * kMinute);
    const auto wednesday = ladder(kTue0930Et + kDay, kTue0930Et + kDay + 375 * kMinute,
                                  15 * kMinute);
    intraday.insert(intraday.end(), wednesday.begin(), wednesday.end());
    const Run aggregated = batch(intraday, spec_for(kRth, kNewYork, "15", "D"));
    CHECK(aggregated.error.empty());
    CHECK(!aggregated.bars.empty());
    for (const Facts& facts : aggregated.bars) {
        CHECK(facts.in && facts.opens && facts.closes && facts.open_ended);
    }
}

// ── 9. a fill recalculation reads the bar it happens on ───────────────────

// calc-on-fills: a market buy placed at Tuesday 15:45's calculation fills at
// Wednesday 09:30's open, and the recalculation that fill drives is made on
// Wednesday 09:30's bar. Its facts are that bar's — it opens the session day
// — never the bar the order was placed on, which closed the day before.
void test_fill_recalculation_reads_its_own_bar() {
    std::printf("test_fill_recalculation_reads_its_own_bar\n");
    auto bars = ladder(kTue0930Et + 370 * kMinute, kTue0930Et + 375 * kMinute, 5 * kMinute);
    const auto wednesday = ladder(kTue0930Et + kDay, kTue0930Et + kDay + 5 * kMinute,
                                  5 * kMinute);
    bars.insert(bars.end(), wednesday.begin(), wednesday.end());
    auto spec = spec_for(kRth, kNewYork, "5", "5");
    spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    Probe host;
    configure(host, spec);
    host.at_bar = [](Probe& self, const NativeDecisionContext& context) {
        if (context.script_bar_open_ms == kTue0930Et + 375 * kMinute) {
            const auto accepted = self.submit(no::Request{no::Transact{1.0}, "f5-buy", ""});
            CHECK(accepted.handle.has_value());
        }
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(bits(host.bars, &Facts::closes) == "0101");
    CHECK(bits(host.bars, &Facts::opens) == "1010");
    CHECK(host.applied.size() == 1);
    CHECK(host.refills.size() == 1);
    for (const auto* seen : {&host.applied, &host.refills}) {
        if (seen->size() != 1) continue;
        const Facts& facts = seen->front();
        CHECK(facts.ts == kTue0930Et + kDay);
        CHECK(facts.in);
        CHECK(facts.opens);
        CHECK(!facts.closes);
        CHECK(!facts.open_ended);
    }
}

// ── 10. the run's edges, and what is presentation ─────────────────────────

// A batch that stops at 15:40 mid-session: its final bar closes the day by
// the run-end convention (a batch is complete input), while read open-ended
// the session goes on. The stream of the same bars does not close it — the
// stream continues past its warmup. The facts are read off the run's input
// and calendar at the point; they are not state, so the same bars with one
// more bar after 15:40 present a different close at 15:40 over the SAME
// continuation.
void test_run_edges_and_presentation() {
    std::printf("test_run_edges_and_presentation\n");
    const auto spec = spec_for(kRth, kNewYork, "5", "5");
    const auto short_bars = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 370 * kMinute,
                                   5 * kMinute);
    const auto long_bars = ladder(kTue0930Et + 360 * kMinute, kTue0930Et + 375 * kMinute,
                                  5 * kMinute);
    Probe shorter;
    configure(shorter, spec);
    shorter.run(short_bars.data(), static_cast<int>(short_bars.size()));
    Probe longer;
    configure(longer, spec);
    longer.run(long_bars.data(), static_cast<int>(long_bars.size()));
    const Run stream = streamed(short_bars, short_bars.size(), spec);
    CHECK(shorter.last_error().empty());
    CHECK(longer.last_error().empty());
    CHECK(bits(shorter.bars, &Facts::closes) == "001");
    CHECK(bits(shorter.bars, &Facts::open_ended) == "000");
    CHECK(bits(longer.bars, &Facts::closes) == "0001");
    CHECK(bits(stream.bars, &Facts::closes) == "000");
    CHECK(bits(stream.bars, &Facts::opens) == "100");
    CHECK(shorter.continuation_at_bar.size() == 3);
    CHECK(longer.continuation_at_bar.size() == 4);
    if (shorter.continuation_at_bar.size() == 3 && longer.continuation_at_bar.size() == 4) {
        CHECK(shorter.continuation_at_bar[2] == longer.continuation_at_bar[2]);
    }
}

// ── 11. a masked overnight session's day belongs to its trading date ──────

// CME-style 1700-1600 America/Chicago, Monday-Friday (:23456). Sunday 17:00 CT
// opens MONDAY's session day: the calendar applies the day mask to the
// session day's trading date, not to the weekday of each instant.
void test_masked_overnight_session() {
    std::printf("test_masked_overnight_session\n");
    // 2026-04-05 (Sunday) 17:00 CDT == 22:00Z.
    const std::int64_t sunday_1700_ct = utc_ms(2026, 4, 5, 22, 0);
    const Run run = batch(ladder(sunday_1700_ct, sunday_1700_ct + 2 * 60 * kMinute,
                                 60 * kMinute),
                          spec_for("1700-1600:23456", "America/Chicago", "60", "60"));
    CHECK(run.error.empty());
    CHECK(bits(run.bars, &Facts::in) == "111");
    CHECK(bits(run.bars, &Facts::opens) == "100");
    CHECK(bits(run.bars, &Facts::open_ended) == "000");
    if (bits(run.bars, &Facts::in) != "111") show("1700-1600:23456 60 -> 60", run);
}

}  // namespace

int main() {
    test_tv_nyse_f();
    test_tv_eth_24x7();
    test_overnight_tokyo_session();
    test_out_of_session_bars();
    test_overnight_session_day_roll();
    test_stream_realtime_prints();
    test_stream_first_bar_sealed_in_realtime();
    test_stream_cannot_see_an_undeclared_early_close();
    test_daily_bars_hold_whole_days();
    test_fill_recalculation_reads_its_own_bar();
    test_run_edges_and_presentation();
    test_masked_overnight_session();

    std::printf("\nnative_session_day_facts: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
