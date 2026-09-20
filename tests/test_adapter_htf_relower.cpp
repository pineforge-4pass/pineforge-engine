// R5 lane R3b -- the Pine adapter's request.security sites on the kernel's
// declared higher-timeframe subscriptions (NativeRunSpec::subscriptions,
// declared at the L6c begin-time hook, stepped by the kernel's pump).
//
// One library, two measurements per case, on the generated shape: a
// source::PineStrategyHost whose configure_security_evaluators() registers
// its sites exactly as codegen spells them (corpus/validation/*/generated.cpp)
// and whose evaluate_security / clear_security write the generated members.
//
//   1. The values the script body reads. Every chart bar's reads, every
//      delivery and clear, the completed buckets and the trades a body driven
//      by the site places are folded and compared with the literal harvested
//      from the adapter BEFORE this lane: this file's --harvest mode run
//      against the wave-5 integration tip 577315a (Release, arm64 macOS,
//      2026-09-21). The re-lowering may not move one value, whichever drive a
//      case runs on.
//
//   2. Which drive ran. A run whose sites the adapter declared to the kernel
//      names them in the running spec (native_state().spec->subscriptions)
//      and native_series_bar(0) answers from the first completed bucket on;
//      a run the adapter keeps on its own drive names none and the accessor
//      stays empty for the whole run. Before this lane every case ran on the
//      adapter's drive, so the kernel-route rows below are the ones that
//      fail-before.
//
// The cases follow the corpus census (23 probes: 21 plain sites, one
// barmerge.gaps_on, two request.security_lower_tf; five probes with several
// sites on one timeframe; every probe input_tf == script_tf, batch, no
// auxiliary feed, no Heikin-Ashi, no lookahead_on) and add the shapes the
// adapter keeps for itself: Heikin-Ashi, lookahead_on, lower-TF, an
// aggregated chart, the bar magnifier, the forex intraday daily request
// (KI-55 OTC pins), the range-start warmup flag, a stream, and a host reused
// from a kernel-routed batch run for a stream run.

#include <pineforge/na.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

int checks = 0;
int failures = 0;
bool harvest = false;
const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expression)) {                                                   \
            ++failures;                                                        \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,         \
                        #expression);                                          \
        }                                                                      \
    } while (false)

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
std::int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kQuarter = 15 * kMinute;

// A 24x7 quarter-hour feed: four input bars per "60" bucket, no session edge.
std::vector<Bar> quarter_hour_bars(int n) {
    const std::int64_t origin = utc_ms(2024, 1, 1);
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double open = 100.0 + i;
        Bar bar{};
        bar.open = open;
        bar.high = open + 2.0;
        bar.low = open - 1.0;
        bar.close = open + 0.5;
        bar.volume = static_cast<double>(i + 1);
        bar.timestamp = origin + static_cast<std::int64_t>(i) * kQuarter;
        bars.push_back(bar);
    }
    return bars;
}

// The same grid, priced on a wave, so a body comparing the chart close with
// its hourly request crosses both ways and places trades.
std::vector<Bar> wave_bars(int n) {
    const std::int64_t origin = utc_ms(2024, 1, 1);
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double open = 100.0 + 8.0 * std::sin(i / 9.0);
        const double close = 100.0 + 8.0 * std::sin((i + 1) / 9.0);
        Bar bar{};
        bar.open = open;
        bar.close = close;
        bar.high = std::max(open, close) + 1.0;
        bar.low = std::min(open, close) - 1.0;
        bar.volume = 10.0 + (i % 7);
        bar.timestamp = origin + static_cast<std::int64_t>(i) * kQuarter;
        bars.push_back(bar);
    }
    return bars;
}

// FNV-1a over 64-bit words; a NaN read folds as one canonical pattern.
std::uint64_t fold(std::uint64_t h, std::uint64_t word) {
    for (int i = 0; i < 8; ++i) {
        h ^= (word >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}
std::uint64_t bits(double value) {
    if (std::isnan(value)) return 0x7ff8000000000000ull;
    std::uint64_t word = 0;
    std::memcpy(&word, &value, sizeof word);
    return word;
}
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;

// One request.security site, as codegen spells it.
struct Site {
    std::string tf;
    bool lookahead = false;
    bool gaps = false;
    bool heikinashi = false;
    bool lower_tf = false;
};

// What the generated shape records: the generated members, and what the
// script body read from them on each chart bar.
struct Recording {
    std::vector<double> value;         // _req_sec_N
    std::vector<int> evaluations;      // evaluate_security calls, per site
    std::vector<int> completions;      // ... with is_complete
    std::vector<int> clears;           // clear_security calls, per site
    std::vector<int> na_bars;          // chart bars that read na, per site
    std::uint64_t visible_fold = kFnvOffset;    // every chart bar's reads
    std::uint64_t completed_fold = kFnvOffset;  // site 0's completed buckets
    int bars_seen = 0;

    void reset(std::size_t sites) {
        value.assign(sites, na<double>());
        evaluations.assign(sites, 0);
        completions.assign(sites, 0);
        clears.assign(sites, 0);
        na_bars.assign(sites, 0);
        visible_fold = kFnvOffset;
        completed_fold = kFnvOffset;
        bars_seen = 0;
    }
    void evaluated(int sec_id, const Bar& bar, bool is_complete) {
        const auto i = static_cast<std::size_t>(sec_id);
        if (i >= value.size()) return;
        value[i] = bar.close;
        ++evaluations[i];
        if (is_complete) {
            ++completions[i];
            if (sec_id == 0) {
                completed_fold = fold(completed_fold, static_cast<std::uint64_t>(bar.timestamp));
                completed_fold = fold(completed_fold, bits(bar.close));
            }
        }
    }
    void cleared(int sec_id) {
        const auto i = static_cast<std::size_t>(sec_id);
        if (i >= value.size()) return;
        value[i] = na<double>();
        ++clears[i];
    }
    void chart_bar() {
        for (std::size_t i = 0; i < value.size(); ++i) {
            visible_fold = fold(visible_fold, bits(value[i]));
            if (std::isnan(value[i])) ++na_bars[i];
        }
        ++bars_seen;
    }
};

// The adapter, in the generated shape.
class GeneratedShape final : public source::PineStrategyHost {
public:
    std::vector<Site> sites;
    Recording rec;
    // The body trades on the site: long while the chart close is above the
    // hourly request, flat while below.
    bool trade = false;
    // Did native_series_bar(0) ever answer during a chart bar? Only the
    // kernel's pump fills that accessor.
    bool series_seen = false;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        rec.reset(sites.size());
        series_seen = false;
        for (std::size_t i = 0; i < sites.size(); ++i) {
            const Site& site = sites[i];
            if (site.lower_tf) {
                register_security_lower_tf_eval(static_cast<int>(i), site.tf, input_tf_);
            } else {
                register_security_eval(static_cast<int>(i), site.tf, input_tf_,
                                       site.lookahead, site.gaps, site.heikinashi);
            }
        }
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        rec.evaluated(sec_id, bar, is_complete);
    }
    void clear_security(int sec_id) override { rec.cleared(sec_id); }
    void on_source_bar(const Bar& bar) override {
        rec.chart_bar();
        if (native_series_bar(0).has_value()) series_seen = true;
        if (!trade || rec.value.empty()) return;
        const double request = rec.value[0];
        if (std::isnan(request)) return;
        if (bar.close > request) strategy_entry("L", true);
        else if (bar.close < request) strategy_close("L");
    }

    // The running (or completed) spec names the series the kernel stepped.
    bool kernel_routed() const {
        const auto view = native_state();
        return view.spec != nullptr && !view.spec->subscriptions.empty();
    }
    std::size_t evaluator_states() const { return security_eval_states_.size(); }
    std::uint64_t trades_fold() const {
        std::uint64_t h = kFnvOffset;
        for (const Trade& trade_row : trades_) {
            h = fold(h, static_cast<std::uint64_t>(trade_row.entry_time));
            h = fold(h, static_cast<std::uint64_t>(trade_row.exit_time));
            h = fold(h, bits(trade_row.entry_price));
            h = fold(h, bits(trade_row.exit_price));
            h = fold(h, bits(trade_row.qty));
        }
        return h;
    }
    std::size_t trade_rows() const { return trades_.size(); }
};

void prime(GeneratedShape& host, const char* type = "crypto") {
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    host.set_syminfo_type(type);
}

void run_batch(GeneratedShape& host, const std::vector<Bar>& bars,
               const char* input_tf, const char* script_tf, bool magnifier = false) {
    host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf, magnifier, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty())
        std::printf("  error: %s\n", host.last_error().c_str());
}

// The literal harvested from the pre-lane adapter, one per case.
struct Expected {
    int bars;
    std::vector<int> evaluations;
    std::vector<int> completions;
    std::vector<int> clears;
    std::vector<int> na_bars;
    std::uint64_t visible_fold;
    std::uint64_t completed_fold;
    std::uint64_t trades_fold;
    std::size_t trades;
};

void print_vector(const char* name, const std::vector<int>& values) {
    std::printf("%s{", name);
    for (std::size_t i = 0; i < values.size(); ++i)
        std::printf("%s%d", i ? ", " : "", values[i]);
    std::printf("}");
}

// Compares the recording with the harvested literal (or prints it).
void expect(const GeneratedShape& host, const Expected& want) {
    const Recording& rec = host.rec;
    if (harvest) {
        std::printf("    // %s\n    Expected{%d, ", scenario, rec.bars_seen);
        print_vector("", rec.evaluations); std::printf(", ");
        print_vector("", rec.completions); std::printf(", ");
        print_vector("", rec.clears); std::printf(", ");
        print_vector("", rec.na_bars);
        std::printf(",\n             0x%016llxull, 0x%016llxull, 0x%016llxull, %zu},\n",
                    static_cast<unsigned long long>(rec.visible_fold),
                    static_cast<unsigned long long>(rec.completed_fold),
                    static_cast<unsigned long long>(host.trades_fold()),
                    host.trade_rows());
        return;
    }
    CHECK(rec.bars_seen == want.bars);
    CHECK(rec.evaluations == want.evaluations);
    CHECK(rec.completions == want.completions);
    CHECK(rec.clears == want.clears);
    CHECK(rec.na_bars == want.na_bars);
    CHECK(rec.visible_fold == want.visible_fold);
    CHECK(rec.completed_fold == want.completed_fold);
    CHECK(host.trades_fold() == want.trades_fold);
    CHECK(host.trade_rows() == want.trades);
    if (rec.bars_seen != want.bars || rec.visible_fold != want.visible_fold) {
        std::printf("  bars %d (want %d), visible fold %016llx (want %016llx)\n",
                    rec.bars_seen, want.bars,
                    static_cast<unsigned long long>(rec.visible_fold),
                    static_cast<unsigned long long>(want.visible_fold));
    }
}

// The route the adapter chose, and its two witnesses.
void expect_route(const GeneratedShape& host, bool kernel, std::size_t sites) {
    if (harvest) return;
    CHECK(host.kernel_routed() == kernel);
    CHECK(host.series_seen == kernel);
    CHECK(host.evaluator_states() == sites);
    if (kernel) {
        const auto view = host.native_state();
        CHECK(view.spec != nullptr && view.spec->subscriptions.size() == sites);
        if (view.spec != nullptr && view.spec->subscriptions.size() == sites) {
            for (std::size_t i = 0; i < sites; ++i) {
                CHECK(view.spec->subscriptions[i].tf == host.sites[i].tf);
                CHECK(view.spec->subscriptions[i].gaps == host.sites[i].gaps);
                CHECK(!view.spec->subscriptions[i].lookahead);
                CHECK(view.spec->subscriptions[i].authoritative_bars.empty());
            }
        }
    }
}

// ---- harvested literals ----------------------------------------------------
//
// Produced by `test_adapter_htf_relower --harvest` linked against the
// wave-5 integration tip 577315a (the adapter before this lane, every case on
// its own drive). Release build, Apple clang, arm64 macOS, 2026-09-21.
const std::vector<Expected> kHarvest = {
    // HARVEST_BEGIN
    // plain hourly
    Expected{64, {16}, {16}, {0}, {3},
             0xd3ca1a8617ac3664ull, 0x18304b67090dd080ull, 0xcbf29ce484222325ull, 0},
    // three instances: 60, 60, 240
    Expected{64, {16, 16, 4}, {16, 16, 4}, {0, 0, 0}, {3, 3, 15},
             0xeb8dd41eeb95a674ull, 0x18304b67090dd080ull, 0xcbf29ce484222325ull, 0},
    // calendar: D, W, M over three weeks
    Expected{2016, {21, 3, 0}, {21, 3, 0}, {0, 0, 0}, {95, 671, 2016},
             0xad4c9971b6cf2155ull, 0xf3fcdeefd63a1521ull, 0xcbf29ce484222325ull, 0},
    // barmerge.gaps_on
    Expected{64, {16}, {16}, {48}, {48},
             0x8a3996447cb4d5d1ull, 0x18304b67090dd080ull, 0xcbf29ce484222325ull, 0},
    // trading body on the hourly request
    Expected{256, {64}, {64}, {0}, {3},
             0x365507efabc26b66ull, 0xf332f5cfc4cbe953ull, 0x8ed5b846fa2117e0ull, 5},
    // trading body placed 5 trade rows
    // ticker.heikinashi
    Expected{64, {16}, {16}, {0}, {3},
             0xf9edc70c0bfa4460ull, 0xee6bd3cc3c7ecf4cull, 0xcbf29ce484222325ull, 0},
    // barmerge.lookahead_on
    Expected{64, {64}, {16}, {0}, {0},
             0xd562b54226bbcda5ull, 0x18304b67090dd080ull, 0xcbf29ce484222325ull, 0},
    // request.security_lower_tf
    Expected{64, {960}, {960}, {0}, {0},
             0xd562b54226bbcda5ull, 0xab59448fa2064cf1ull, 0xcbf29ce484222325ull, 0},
    // aggregated chart: 15m input, 60m script
    Expected{32, {8}, {8}, {0}, {3},
             0x2004b0e571ab24acull, 0xa7e0cf501ffcb128ull, 0xcbf29ce484222325ull, 0},
    // bar magnifier: 15m input, 60m script
    Expected{32, {8}, {8}, {0}, {3},
             0x2004b0e571ab24acull, 0xa7e0cf501ffcb128ull, 0xcbf29ce484222325ull, 0},
    // forex intraday chart, daily request (OTC pins)
    Expected{480, {5}, {5}, {0}, {95},
             0xc1adcfca6bae4576ull, 0xb825bdbe99c1dd7bull, 0xcbf29ce484222325ull, 0},
    // security_range_start_na_warmup
    Expected{64, {16}, {16}, {0}, {3},
             0xd3ca1a8617ac3664ull, 0x18304b67090dd080ull, 0xcbf29ce484222325ull, 0},
    // stream: warmup then a tick
    Expected{20, {5}, {5}, {0}, {3},
             0x110dc95ff603fb95ull, 0x8eea13890864a42dull, 0xcbf29ce484222325ull, 0},
    // host reused: kernel-routed batch, then a stream
    Expected{20, {5}, {5}, {0}, {3},
             0x110dc95ff603fb95ull, 0x8eea13890864a42dull, 0xcbf29ce484222325ull, 0},
    // HARVEST_END
};

enum Case {
    kPlainHourly = 0,
    kThreeInstances,
    kCalendar,
    kGaps,
    kTradingBody,
    kHeikinAshi,
    kLookahead,
    kLowerTimeframe,
    kAggregatedChart,
    kMagnifier,
    kForexDaily,
    kRangeStartWarmup,
    kStream,
    kReuseForStream,
    kCaseCount
};

const Expected& want(Case which) {
    static const Expected empty{0, {}, {}, {}, {}, 0, 0, 0, 0};
    const std::size_t index = static_cast<std::size_t>(which);
    return index < kHarvest.size() ? kHarvest[index] : empty;
}

// ---- kernel-routed cases: plain sites, batch, input_tf == script_tf ---------

void test_plain_hourly() {
    scenario = "plain hourly";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60"}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kPlainHourly));
    expect_route(host, true, 1);
    // The (i) core in the open: 16 buckets, three na bars before the first.
    CHECK(harvest || host.rec.completions[0] == 16);
    CHECK(harvest || host.rec.na_bars[0] == 3);
}

void test_three_instances() {
    scenario = "three instances: 60, 60, 240";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60"}, Site{"60"}, Site{"240"}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kThreeInstances));
    expect_route(host, true, 3);
    // Two sites on one timeframe are two series instances, each evaluated.
    CHECK(harvest || host.rec.completions[0] == 16);
    CHECK(harvest || host.rec.completions[1] == 16);
    CHECK(harvest || host.rec.completions[2] == 4);
}

void test_calendar() {
    scenario = "calendar: D, W, M over three weeks";
    const std::vector<Bar> bars = quarter_hour_bars(4 * 24 * 21);
    GeneratedShape host;
    host.sites = {Site{"D"}, Site{"W"}, Site{"M"}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kCalendar));
    expect_route(host, true, 3);
}

void test_gaps() {
    scenario = "barmerge.gaps_on";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60", false, true}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kGaps));
    expect_route(host, true, 1);
    // na on every chart bar that completed no bucket: 48 of 64.
    CHECK(harvest || host.rec.clears[0] == 48);
    CHECK(harvest || host.rec.na_bars[0] == 48);
}

void test_trading_body() {
    scenario = "trading body on the hourly request";
    const std::vector<Bar> bars = wave_bars(256);
    GeneratedShape host;
    host.sites = {Site{"60"}};
    host.trade = true;
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kTradingBody));
    expect_route(host, true, 1);
    CHECK(harvest || host.trade_rows() > 0);
    if (harvest) std::printf("    // trading body placed %zu trade rows\n", host.trade_rows());
}

// ---- adapter-routed cases: a Pine-only rule keeps the adapter's drive ------

void test_heikin_ashi() {
    scenario = "ticker.heikinashi";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60", false, false, true}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kHeikinAshi));
    expect_route(host, false, 1);
}

void test_lookahead() {
    scenario = "barmerge.lookahead_on";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60", true}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kLookahead));
    expect_route(host, false, 1);
    // The peeks: more evaluations than completions, no na bar.
    CHECK(harvest || host.rec.evaluations[0] > host.rec.completions[0]);
}

void test_lower_timeframe() {
    scenario = "request.security_lower_tf";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"1", false, false, false, true}};
    prime(host);
    run_batch(host, bars, "15", "15");
    expect(host, want(kLowerTimeframe));
    expect_route(host, false, 1);
    CHECK(harvest || host.rec.evaluations[0] == 15 * 64);
}

void test_aggregated_chart() {
    scenario = "aggregated chart: 15m input, 60m script";
    const std::vector<Bar> bars = quarter_hour_bars(128);
    GeneratedShape host;
    host.sites = {Site{"240"}};
    prime(host);
    run_batch(host, bars, "15", "60");
    expect(host, want(kAggregatedChart));
    expect_route(host, false, 1);
}

void test_magnifier() {
    scenario = "bar magnifier: 15m input, 60m script";
    const std::vector<Bar> bars = quarter_hour_bars(128);
    GeneratedShape host;
    host.sites = {Site{"240"}};
    prime(host);
    run_batch(host, bars, "15", "60", true);
    expect(host, want(kMagnifier));
    expect_route(host, false, 1);
}

void test_forex_daily() {
    scenario = "forex intraday chart, daily request (OTC pins)";
    const std::vector<Bar> bars = quarter_hour_bars(4 * 24 * 5);
    GeneratedShape host;
    host.sites = {Site{"D"}};
    prime(host, "forex");
    run_batch(host, bars, "15", "15");
    expect(host, want(kForexDaily));
    expect_route(host, false, 1);
}

void test_range_start_warmup() {
    scenario = "security_range_start_na_warmup";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60"}};
    prime(host);
    host.set_syminfo_metadata("security_range_start_na_warmup",
                              static_cast<double>(bars.front().timestamp));
    run_batch(host, bars, "15", "15");
    expect(host, want(kRangeStartWarmup));
    expect_route(host, false, 1);
}

// The stream drive: warmup through the input path, then a realtime tick that
// completes the fifth bucket.
int run_stream(GeneratedShape& host, const std::vector<Bar>& bars, int warmup) {
    CHECK(host.stream_begin(bars.data(), warmup, "15", "15"));
    const int completed_by_warmup = host.rec.completions.empty() ? -1 : host.rec.completions[0];
    TradeTick tick{};
    tick.timestamp = bars[static_cast<std::size_t>(warmup)].timestamp;
    tick.sequence = 1;
    tick.price = bars[static_cast<std::size_t>(warmup)].close;
    tick.quantity = 1.0;
    CHECK(host.stream_push_tick(tick));
    CHECK(host.last_error().empty());
    if (!host.last_error().empty())
        std::printf("  stream error: %s\n", host.last_error().c_str());
    CHECK(host.stream_end(true));
    return completed_by_warmup;
}

void test_stream() {
    scenario = "stream: warmup then a tick";
    const std::vector<Bar> bars = quarter_hour_bars(32);
    GeneratedShape host;
    host.sites = {Site{"60"}};
    prime(host);
    const int by_warmup = run_stream(host, bars, 19);
    expect(host, want(kStream));
    expect_route(host, false, 1);
    CHECK(harvest || by_warmup == 4);
    CHECK(harvest || host.rec.completions[0] == 5);
}

void test_reuse_for_stream() {
    scenario = "host reused: kernel-routed batch, then a stream";
    const std::vector<Bar> bars = quarter_hour_bars(64);
    GeneratedShape host;
    host.sites = {Site{"60"}};
    prime(host);
    run_batch(host, bars, "15", "15");
    if (!harvest) CHECK(host.kernel_routed());
    // The stream run registers the same site itself; the kernel's previous
    // registration must be gone before that, or the site would be mistaken
    // for the kernel's own tail and erased.
    const int by_warmup = run_stream(host, bars, 19);
    expect(host, want(kReuseForStream));
    expect_route(host, false, 1);
    CHECK(harvest || by_warmup == 4);
    CHECK(harvest || host.rec.completions[0] == 5);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--harvest") == 0) harvest = true;
    }
    if (harvest) std::printf("    // HARVEST_BEGIN\n");
    test_plain_hourly();
    test_three_instances();
    test_calendar();
    test_gaps();
    test_trading_body();
    test_heikin_ashi();
    test_lookahead();
    test_lower_timeframe();
    test_aggregated_chart();
    test_magnifier();
    test_forex_daily();
    test_range_start_warmup();
    test_stream();
    test_reuse_for_stream();
    if (harvest) {
        std::printf("    // HARVEST_END\n");
        return failures == 0 ? 0 : 1;
    }
    std::printf("adapter HTF re-lowering: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
