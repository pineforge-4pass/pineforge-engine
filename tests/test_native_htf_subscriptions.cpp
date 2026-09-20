// Declared higher-timeframe series for a bare NativeStrategyHost
// (NativeRunSpec::subscriptions, on_native_timeframe_bar, native_series_bar).
//
// Witnesses, all independent of the feature under test:
//   1. a "60" series over a 15-minute input delivers exactly N/4 buckets whose
//      OHLCV equal a hand aggregation of each 4-bar group, each delivered on
//      the group's 4th input bar and BEFORE that bar's on_native_bar;
//   2. lookahead = true delivers the same buckets on each group's FIRST bar;
//   2b. two "60" series over one 15-minute input are two INSTANCES: the same
//      buckets are delivered twice, each under its own index, and
//      native_series_bar answers each independently (also "60"/"60"/"240");
//   2c. barmerge.gaps_on empties the pull accessor AND reaches the push side
//      (clear_security) on every input a gapped series delivers nothing on;
//   2d. a host that declares its series inside on_native_run_begin gets them
//      registered although the spec named none, and its own evaluator
//      registration -- which clears the state vector as generated code does --
//      no longer erases the kernel's;
//   2e. the kernel tears its previous run's registration down BEFORE the
//      host's run-begin callback, so a host that re-registers the identical
//      evaluator state itself keeps it;
//   3. authoritative_bars replace the aggregated OHLCV of completed buckets;
//   4. a series finer than the input is refused at configure with its own
//      named reason, while two series of one period are accepted unless they
//      declare different authoritative bars;
//   5. a "W" series over a daily input reproduces, bucket for bucket, what the
//      Pine request.security path produces for the same bars (the twin drives
//      source::PineStrategyHost exactly as tests/test_native_wm_buckets.cpp
//      does: register_security_eval + evaluate_security);
//   6. a spec that declares no series folds nothing new into the continuation
//      identity: its native_run_spec_digest (the consumer's spec fold, without
//      the machine-specific timezone resources a raw continuation hash carries)
//      is pinned, is unchanged when the empty series list is stated outright,
//      and moves as soon as one series is declared;
//   7. the pump is ordered against the SCRIPT interval: a script bar the input
//      after a feed hole seals lazily is calculated BEFORE that input's
//      buckets are delivered, reading the series exactly as its own inputs
//      left them (seal(k) -> deliver(i+1) -> calc(i+1)), against a series
//      whose bucket completes on that very input, a gapped series it clears,
//      a boundary it closes and a lookahead bucket it opens.

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

int checks = 0;
int failures = 0;
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

bool same(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
}

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
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

NativeRunSpec base_spec(const char* input, const char* script,
                        const char* session_key) {
    NativeRunSpec spec;
    spec.identity = {session_key, 1};
    spec.input_tf = input;
    spec.script_tf = script;
    spec.ticker = "SERIES";
    spec.tickerid = "TEST:SERIES";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native higher-timeframe subscription fixture";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// ---- hosts ----------------------------------------------------------------

struct Delivery {
    std::size_t subscription = 0;
    Bar bar{};
    NativeTimeframeBarContext context{};
    // How many script bars the host had already calculated when the bucket
    // arrived: the 0-based index of the bar it is delivered on.
    int bars_before = 0;
    // What native_series_bar answered inside the callback.
    bool accessor_matches = false;
};

class SeriesHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    std::vector<Delivery> deliveries;
    int bars_seen = 0;
    // The push side of barmerge.gaps_on: clear_security calls per sec_id.
    std::map<int, int> clears;
    // Optional per-calculation probe: what native_series_bar(i) answered at
    // each script bar, for every i < probes. Zero — the default — records
    // nothing, so every scenario that does not ask for it is unchanged.
    std::size_t probes = 0;
    std::vector<std::vector<std::optional<Bar>>> series_at_bar;

    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        log.push_back("input:" + std::to_string(context.input_index) + "@"
                      + std::to_string(bar.timestamp));
    }

    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        Delivery delivery;
        delivery.subscription = context.subscription;
        delivery.bar = bar;
        delivery.context = context;
        delivery.bars_before = bars_seen;
        const auto pulled = native_series_bar(context.subscription);
        delivery.accessor_matches = pulled.has_value()
            && pulled->timestamp == bar.timestamp && same(pulled->open, bar.open)
            && same(pulled->high, bar.high) && same(pulled->low, bar.low)
            && same(pulled->close, bar.close) && same(pulled->volume, bar.volume);
        deliveries.push_back(delivery);
        log.push_back("htf:" + std::to_string(context.subscription) + "@"
                      + std::to_string(bar.timestamp));
    }

    void clear_security(int sec_id) override { ++clears[sec_id]; }

    // How each calculated script bar was sealed: Confirmed by an input of
    // its own, LazyComplete by the first input of a later interval.
    std::vector<NativeCompletionKind> completions;

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        ++bars_seen;
        if (probes > 0) {
            std::vector<std::optional<Bar>> row;
            row.reserve(probes);
            for (std::size_t i = 0; i < probes; ++i) row.push_back(native_series_bar(i));
            series_at_bar.push_back(std::move(row));
        }
        completions.push_back(context.coordinate.completion);
        log.push_back("bar@" + std::to_string(bar.timestamp));
    }
};

// A host in the adapter's shape: its series are known only to its own
// run-begin registration, which opens by clearing the engine's evaluator
// states exactly as a generated configure_security_evaluators() does.
class DeclaringHost final : public NativeStrategyHost {
public:
    std::vector<NativeTimeframeSubscription> declare;
    bool register_own_evaluator = false;
    // What the hook answered inside on_native_run_begin, and outside it.
    bool declared_at_begin = false;
    bool declared_outside_begin = true;
    std::vector<Delivery> deliveries;
    int own_evaluations = 0;
    int bars_seen = 0;

    void on_native_run_begin() override {
        if (register_own_evaluator) {
            security_eval_states_.clear();
            register_security_eval(0, "240", "15");
        }
        declared_at_begin = declare_timeframe_subscriptions(declare);
    }

    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        Delivery delivery;
        delivery.subscription = context.subscription;
        delivery.bar = bar;
        delivery.context = context;
        delivery.bars_before = bars_seen;
        const auto pulled = native_series_bar(context.subscription);
        delivery.accessor_matches = pulled.has_value()
            && pulled->timestamp == bar.timestamp && same(pulled->close, bar.close);
        deliveries.push_back(delivery);
    }

    // The host's OWN evaluator. The kernel feeds only the states it
    // registered itself, so this stays at zero.
    void evaluate_security(int sec_id, const Bar&, bool is_complete) override {
        if (sec_id == 0 && is_complete) ++own_evaluations;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars_seen;
        declared_outside_begin = declare_timeframe_subscriptions(declare);
    }

    std::size_t evaluator_states() const { return security_eval_states_.size(); }
};

// The Pine twin of scenario 5: the same register_security_eval evaluator the
// generated code uses, recording one row per completed bucket.
class PineSeriesProbe final : public source::PineStrategyHost {
public:
    std::string requested_tf = "W";
    struct Completed {
        Bar bar{};
        int bars_before = 0;
    };
    std::vector<Completed> completed;
    int bars_seen = 0;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, requested_tf, input_tf_, false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        completed.push_back(Completed{bar, bars_seen});
    }

    void on_source_bar(const Bar&) override { ++bars_seen; }
};

// ---- fixtures -------------------------------------------------------------

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

// The oracle: a hand aggregation of one contiguous group of input bars.
Bar hand_aggregate(const std::vector<Bar>& bars, int from, int count,
                   std::int64_t label) {
    Bar out = bars[static_cast<std::size_t>(from)];
    out.timestamp = label;
    for (int i = 1; i < count; ++i) {
        const Bar& next = bars[static_cast<std::size_t>(from + i)];
        out.high = std::max(out.high, next.high);
        out.low = std::min(out.low, next.low);
        out.close = next.close;
        out.volume += next.volume;
    }
    return out;
}

void check_bucket(const Bar& got, const Bar& want, const char* tag) {
    const bool ok = got.timestamp == want.timestamp && same(got.open, want.open)
        && same(got.high, want.high) && same(got.low, want.low)
        && same(got.close, want.close) && same(got.volume, want.volume);
    if (!ok) {
        std::printf("  %s: got t %lld o %.6g h %.6g l %.6g c %.6g v %.6g; "
                    "want t %lld o %.6g h %.6g l %.6g c %.6g v %.6g\n",
                    tag, static_cast<long long>(got.timestamp), got.open, got.high,
                    got.low, got.close, got.volume,
                    static_cast<long long>(want.timestamp), want.open, want.high,
                    want.low, want.close, want.volume);
    }
    CHECK(ok);
}

// ---- 1. lookahead_off: N/4 buckets on each group's last bar ---------------

void test_hourly_over_quarter_hour() {
    scenario = "hourly over quarter-hour";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    NativeRunSpec spec = base_spec("15", "15", "native-htf-off");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    spec.subscriptions.push_back(hourly);

    SeriesHost host;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() != 4) return;
    const std::int64_t origin = bars.front().timestamp;
    for (std::size_t k = 0; k < host.deliveries.size(); ++k) {
        const Delivery& delivery = host.deliveries[k];
        const Bar want = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                        origin + static_cast<std::int64_t>(k) * kHour);
        check_bucket(delivery.bar, want, "bucket");
        CHECK(delivery.subscription == 0);
        CHECK(delivery.accessor_matches);
        // Delivered on the group's 4th input bar, before that bar's
        // calculation: exactly k script bars had been calculated.
        CHECK(delivery.bars_before == static_cast<int>(k) * 4 + 3);
        CHECK(delivery.context.delivered_at_ms
              == bars[k * 4 + 3].timestamp);
        CHECK(delivery.context.completion == NativeCompletionKind::Confirmed);
        // The bucket's own calendar span, from its first contributing bar.
        CHECK(delivery.context.interval.open_ms
              == origin + static_cast<std::int64_t>(k) * kHour);
        CHECK(delivery.context.interval.next_period_open_ms
              == origin + static_cast<std::int64_t>(k + 1) * kHour);
    }

    // The sequence itself: input, then the bucket, then the calculation.
    std::vector<std::string> want_log;
    for (int i = 0; i < 16; ++i) {
        want_log.push_back("input:" + std::to_string(i) + "@"
                           + std::to_string(bars[static_cast<std::size_t>(i)].timestamp));
        if (i % 4 == 3) {
            want_log.push_back("htf:0@" + std::to_string(
                origin + static_cast<std::int64_t>(i / 4) * kHour));
        }
        want_log.push_back("bar@"
            + std::to_string(bars[static_cast<std::size_t>(i)].timestamp));
    }
    CHECK(host.log == want_log);
    if (host.log != want_log) {
        for (std::size_t i = 0; i < std::max(host.log.size(), want_log.size()); ++i) {
            const std::string got = i < host.log.size() ? host.log[i] : std::string("-");
            const std::string exp = i < want_log.size() ? want_log[i] : std::string("-");
            if (got != exp) std::printf("  log[%zu]: got %s want %s\n", i, got.c_str(),
                                        exp.c_str());
        }
    }
}

// ---- 2. lookahead_on: the same buckets on each group's first bar ----------

void test_hourly_lookahead() {
    scenario = "hourly lookahead_on";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    NativeRunSpec spec = base_spec("15", "15", "native-htf-on");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    hourly.lookahead = true;
    spec.subscriptions.push_back(hourly);

    SeriesHost host;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() != 4) return;
    const std::int64_t origin = bars.front().timestamp;
    for (std::size_t k = 0; k < host.deliveries.size(); ++k) {
        const Delivery& delivery = host.deliveries[k];
        const Bar want = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                        origin + static_cast<std::int64_t>(k) * kHour);
        check_bucket(delivery.bar, want, "lookahead bucket");
        CHECK(delivery.accessor_matches);
        CHECK(delivery.bars_before == static_cast<int>(k) * 4);
        CHECK(delivery.context.delivered_at_ms == bars[k * 4].timestamp);
    }
    std::vector<std::string> want_log;
    for (int i = 0; i < 16; ++i) {
        want_log.push_back("input:" + std::to_string(i) + "@"
                           + std::to_string(bars[static_cast<std::size_t>(i)].timestamp));
        if (i % 4 == 0) {
            want_log.push_back("htf:0@" + std::to_string(
                origin + static_cast<std::int64_t>(i / 4) * kHour));
        }
        want_log.push_back("bar@"
            + std::to_string(bars[static_cast<std::size_t>(i)].timestamp));
    }
    CHECK(host.log == want_log);
}

// ---- 2b. several instances of one timeframe -------------------------------
//
// A subscription is a series INSTANCE: two "60" series over the same 15m
// input are two evaluators with their own bucket state, delivered under their
// own index, and the accessor answers each independently.

void test_same_timeframe_instances() {
    scenario = "two instances of one timeframe";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    NativeRunSpec spec = base_spec("15", "15", "native-htf-instances");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    spec.subscriptions.push_back(hourly);
    spec.subscriptions.push_back(hourly);

    SeriesHost host;
    host.probes = 2;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    CHECK(host.deliveries.size() == 8);
    if (host.deliveries.size() != 8) return;
    const std::int64_t origin = bars.front().timestamp;
    for (std::size_t k = 0; k < 4; ++k) {
        const Bar want = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                        origin + static_cast<std::int64_t>(k) * kHour);
        for (std::size_t instance = 0; instance < 2; ++instance) {
            const Delivery& delivery = host.deliveries[k * 2 + instance];
            check_bucket(delivery.bar, want, "instance bucket");
            CHECK(delivery.subscription == instance);
            CHECK(delivery.accessor_matches);
            CHECK(delivery.bars_before == static_cast<int>(k) * 4 + 3);
            CHECK(delivery.context.delivered_at_ms == bars[k * 4 + 3].timestamp);
            CHECK(delivery.context.completion == NativeCompletionKind::Confirmed);
            CHECK(delivery.context.interval.open_ms
                  == origin + static_cast<std::int64_t>(k) * kHour);
        }
    }
    // Both accessors answer, bar for bar, with the same series: nothing about
    // one instance's delivery reaches the other's slot.
    CHECK(host.series_at_bar.size() == 16);
    for (std::size_t i = 0; i < host.series_at_bar.size(); ++i) {
        const auto& row = host.series_at_bar[i];
        CHECK(row.size() == 2);
        if (row.size() != 2) continue;
        CHECK(row[0].has_value() == row[1].has_value());
        CHECK(row[0].has_value() == (i >= 3));
        if (!row[0] || !row[1]) continue;
        check_bucket(*row[1], *row[0], "instance accessor");
    }

    // Three instances, two of them sharing a period, the third coarser.
    NativeRunSpec mixed = base_spec("15", "15", "native-htf-instances-3");
    mixed.subscriptions.push_back(hourly);
    mixed.subscriptions.push_back(hourly);
    NativeTimeframeSubscription four_hourly;
    four_hourly.tf = "240";
    mixed.subscriptions.push_back(four_hourly);

    SeriesHost third;
    const auto mixed_setup = third.configure_native(mixed);
    CHECK(mixed_setup.status == NativeSetupStatus::Applied);
    third.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(third.last_error().empty());
    if (!third.last_error().empty()) std::printf("  error: %s\n", third.last_error().c_str());

    // 4 hourly buckets twice, and one four-hourly bucket over the same 16 bars.
    CHECK(third.deliveries.size() == 9);
    if (third.deliveries.size() != 9) return;
    std::vector<std::size_t> want_indexes{0, 1, 0, 1, 0, 1, 0, 1, 2};
    std::vector<std::size_t> got_indexes;
    for (const Delivery& delivery : third.deliveries) {
        got_indexes.push_back(delivery.subscription);
    }
    CHECK(got_indexes == want_indexes);
    const Delivery& coarse = third.deliveries.back();
    check_bucket(coarse.bar, hand_aggregate(bars, 0, 16, origin), "four-hourly bucket");
    CHECK(coarse.context.delivered_at_ms == bars[15].timestamp);
    CHECK(coarse.accessor_matches);
}

// ---- 2c. barmerge.gaps_on clears the series between deliveries ------------
//
// The FNV-1a digest of one plain hourly series — `tf` "60", lookahead off, no
// authoritative bars — as the fold produced it before `gaps` existed:
// u(1) size, u(2) + "60", u(0) lookahead, u(0) bars. A gaps_off series must
// still hash to exactly this, which is what keeps every established
// subscription digest (and the continuation identity folding it) unchanged.
constexpr std::uint64_t kHourlySeriesDigest = 13834961980321333110ull;

void test_gaps_clears_between_deliveries() {
    scenario = "gaps_on";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    NativeRunSpec spec = base_spec("15", "15", "native-htf-gaps");
    NativeTimeframeSubscription gapped;
    gapped.tf = "60";
    gapped.gaps = true;
    NativeTimeframeSubscription standing;  // the same series, gaps_off
    standing.tf = "60";
    NativeTimeframeSubscription gapped_lookahead;
    gapped_lookahead.tf = "60";
    gapped_lookahead.gaps = true;
    gapped_lookahead.lookahead = true;
    spec.subscriptions.push_back(gapped);
    spec.subscriptions.push_back(standing);
    spec.subscriptions.push_back(gapped_lookahead);

    SeriesHost host;
    host.probes = 3;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    // Gaps changes no completion and no delivery: 4 buckets per series.
    CHECK(host.deliveries.size() == 12);
    std::size_t per_index[3] = {0, 0, 0};
    for (const Delivery& delivery : host.deliveries) {
        CHECK(delivery.subscription < 3);
        if (delivery.subscription < 3) ++per_index[delivery.subscription];
        CHECK(delivery.accessor_matches);
    }
    CHECK(per_index[0] == 4);
    CHECK(per_index[1] == 4);
    CHECK(per_index[2] == 4);

    // The push side of gaps: the generated clear_security() a source host
    // implements is called on every input the gapped series delivers nothing
    // on -- 12 of the 16 for the lookahead_off series, the same 12 for the
    // lookahead_on one (delivered on the four opening bars) -- and never for
    // the gaps_off twin. A bare host's clear_security is the base no-op.
    CHECK(host.clears[0] == 12);
    CHECK(host.clears.count(1) == 0);
    CHECK(host.clears[2] == 12);

    const std::int64_t origin = bars.front().timestamp;
    CHECK(host.series_at_bar.size() == 16);
    for (std::size_t i = 0; i < host.series_at_bar.size(); ++i) {
        const auto& row = host.series_at_bar[i];
        CHECK(row.size() == 3);
        if (row.size() != 3) continue;
        // gaps_on, lookahead_off: the value exists only on the bar that
        // completes a bucket.
        CHECK(row[0].has_value() == (i % 4 == 3));
        // gaps_off: the same bucket stands until the next one replaces it.
        CHECK(row[1].has_value() == (i >= 3));
        // gaps_on, lookahead_on: only on the bar that OPENS a bucket.
        CHECK(row[2].has_value() == (i % 4 == 0));
        if (row[0]) {
            const Bar want = hand_aggregate(bars, static_cast<int>(i / 4) * 4, 4,
                                            origin + static_cast<std::int64_t>(i / 4) * kHour);
            check_bucket(*row[0], want, "gapped bucket");
            CHECK(row[1].has_value());
            if (row[1]) check_bucket(*row[1], want, "standing bucket");
        }
        if (row[2]) {
            const Bar want = hand_aggregate(bars, static_cast<int>(i / 4) * 4, 4,
                                            origin + static_cast<std::int64_t>(i / 4) * kHour);
            check_bucket(*row[2], want, "gapped lookahead bucket");
        }
    }

    // Digest neutrality: a gaps_off series hashes as it did before the field
    // existed, and setting gaps moves the digest.
    std::vector<NativeTimeframeSubscription> plain{standing};
    const std::uint64_t plain_digest = native_timeframe_subscriptions_digest(plain);
    if (plain_digest != kHourlySeriesDigest) {
        std::printf("  hourly series digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(plain_digest),
                    static_cast<unsigned long long>(kHourlySeriesDigest));
    }
    CHECK(plain_digest == kHourlySeriesDigest);
    std::vector<NativeTimeframeSubscription> gapped_only{gapped};
    CHECK(native_timeframe_subscriptions_digest(gapped_only) != kHourlySeriesDigest);
}

// ---- 2d. the begin-time declaration hook ---------------------------------

void test_begin_time_declaration() {
    scenario = "begin-time declaration";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const std::int64_t origin = bars.front().timestamp;
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";

    // Outside a run there is nothing to declare against.
    DeclaringHost unconfigured;
    CHECK(!unconfigured.declare_timeframe_subscriptions({hourly}));

    // The spec names NO series; the host names one at begin, after clearing
    // and rebuilding the engine's evaluator states as generated code does.
    NativeRunSpec spec = base_spec("15", "15", "native-htf-declare");
    DeclaringHost host;
    host.declare.push_back(hourly);
    host.register_own_evaluator = true;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    CHECK(host.declared_at_begin);
    // Legal only inside on_native_run_begin.
    CHECK(!host.declared_outside_begin);
    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() == 4) {
        for (std::size_t k = 0; k < 4; ++k) {
            const Bar want = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                            origin + static_cast<std::int64_t>(k) * kHour);
            check_bucket(host.deliveries[k].bar, want, "declared bucket");
            CHECK(host.deliveries[k].accessor_matches);
            CHECK(host.deliveries[k].bars_before == static_cast<int>(k) * 4 + 3);
        }
    }
    // The host's own registration survives, and the kernel's is appended
    // after it rather than in place of it.
    CHECK(host.evaluator_states() == 2);
    // The kernel feeds only the states it registered itself.
    CHECK(host.own_evaluations == 0);
    // The staged spec names what actually ran, so the run's continuation
    // identity folds the declared series.
    const auto view = host.native_state();
    CHECK(view.spec != nullptr);
    if (view.spec != nullptr) {
        CHECK(view.spec->subscriptions.size() == 1);
        if (view.spec->subscriptions.size() == 1) {
            CHECK(view.spec->subscriptions[0].tf == "60");
        }
    }

    // A list this run's input timeframe would refuse is refused by the hook,
    // and the staged list is left exactly as it was.
    NativeRunSpec staged = base_spec("15", "15", "native-htf-declare-refused");
    staged.subscriptions.push_back(hourly);
    DeclaringHost refused;
    NativeTimeframeSubscription finer;
    finer.tf = "5";
    refused.declare.push_back(finer);
    CHECK(refused.configure_native(staged).status == NativeSetupStatus::Applied);
    refused.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
                MagnifierDistribution::ENDPOINTS);
    CHECK(refused.last_error().empty());
    CHECK(!refused.declared_at_begin);
    CHECK(refused.deliveries.size() == 4);
    if (refused.deliveries.size() == 4) {
        check_bucket(refused.deliveries[0].bar, hand_aggregate(bars, 0, 4, origin),
                     "staged bucket after a refused declaration");
    }
}

// ---- 2e. the previous run's series are torn down before on_native_run_begin
//
// A host that declared a series in one run and registers the SAME evaluator
// state itself in the next -- the adapter switching a site from the kernel's
// drive to its own -- must keep that state. The kernel tears its previous
// registration down before the host's run-begin callback, so what the
// callback registers can never be mistaken for the kernel's own tail.
class SwitchingHost final : public NativeStrategyHost {
public:
    bool register_own = false;
    int evaluations = 0;

    void on_native_run_begin() override {
        if (!register_own) return;
        security_eval_states_.clear();
        register_security_eval(0, "60", "15");
    }
    void evaluate_security(int sec_id, const Bar&, bool is_complete) override {
        if (sec_id == 0 && is_complete) ++evaluations;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    std::size_t evaluator_states() const { return security_eval_states_.size(); }
};

void test_teardown_precedes_run_begin() {
    scenario = "teardown precedes run begin";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";

    // Run 1: the spec declares "60"; the kernel registers it as sec_id 0 and
    // its four completions reach evaluate_security.
    NativeRunSpec declared = base_spec("15", "15", "native-htf-teardown");
    declared.subscriptions.push_back(hourly);
    SwitchingHost host;
    CHECK(host.configure_native(declared).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    CHECK(host.evaluator_states() == 1);
    CHECK(host.evaluations == 4);

    // Run 2, same host: the spec declares nothing and the host registers the
    // identical state itself at begin. It is the host's, and it stays:
    // nothing pumps it (this host has no pump of its own), nothing erases it.
    NativeRunSpec own = base_spec("15", "15", "native-htf-teardown");
    own.identity.run_number = 2;
    host.register_own = true;
    host.evaluations = 0;
    CHECK(host.configure_native(own).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    CHECK(host.evaluator_states() == 1);
    CHECK(host.evaluations == 0);
    CHECK(!host.native_series_bar(0).has_value());
}

// ---- 3. authoritative bars replace the aggregate --------------------------

void test_authoritative_bars_override() {
    scenario = "authoritative bars";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const std::int64_t origin = bars.front().timestamp;

    std::vector<Bar> exchange;
    for (int k = 0; k < 4; ++k) {
        Bar bar{};
        bar.open = 1000.0 + k;
        bar.high = 1010.0 + k;
        bar.low = 990.0 + k;
        bar.close = 1005.0 + k;
        bar.volume = 7777.0 + k;
        bar.timestamp = origin + static_cast<std::int64_t>(k) * kHour;
        exchange.push_back(bar);
    }

    NativeRunSpec spec = base_spec("15", "15", "native-htf-feed");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    hourly.authoritative_bars = exchange;
    spec.subscriptions.push_back(hourly);

    SeriesHost host;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() != 4) return;
    for (std::size_t k = 0; k < host.deliveries.size(); ++k) {
        check_bucket(host.deliveries[k].bar, exchange[k], "exchange bucket");
        const Bar aggregate = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                             exchange[k].timestamp);
        CHECK(!same(host.deliveries[k].bar.close, aggregate.close));
    }
    CHECK(host.native_security_substitutions() == 4);
    CHECK(host.native_security_misses() == 0);
}

// ---- 4. a series finer than the input is refused ---------------------------

void test_finer_than_input_is_refused() {
    scenario = "finer than input";
    NativeRunSpec spec = base_spec("15", "15", "native-htf-finer");
    NativeTimeframeSubscription finer;
    finer.tf = "5";
    spec.subscriptions.push_back(finer);

    SeriesHost host;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Failed);
    CHECK(setup.validation.error == NativeRunSpecError::SubscriptionFinerThanInput);
    CHECK(setup.validation.field == NativeRunSpecField::SubscriptionTimeframe);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);

    // The neighbouring refusals of the same block, so the named reason is a
    // reason and not a catch-all.
    NativeRunSpec unparsable = base_spec("15", "15", "native-htf-bad");
    NativeTimeframeSubscription broken;
    broken.tf = "not-a-timeframe";
    unparsable.subscriptions.push_back(broken);
    SeriesHost second;
    const auto broken_setup = second.configure_native(unparsable);
    CHECK(broken_setup.status == NativeSetupStatus::Failed);
    CHECK(broken_setup.validation.error
          == NativeRunSpecError::InvalidSubscriptionTimeframe);

    // Two series of one period are two instances and are accepted; only two
    // DIFFERENT authoritative feeds for that one period are refused, because
    // the feed store is keyed by the period's duration.
    NativeRunSpec duplicated = base_spec("15", "15", "native-htf-dup");
    NativeTimeframeSubscription daily;
    daily.tf = "D";
    NativeTimeframeSubscription same_period;
    same_period.tf = "1D";
    duplicated.subscriptions.push_back(daily);
    duplicated.subscriptions.push_back(same_period);
    SeriesHost third;
    const auto duplicate_setup = third.configure_native(duplicated);
    CHECK(duplicate_setup.status == NativeSetupStatus::Applied);

    NativeRunSpec shared_feed = duplicated;
    shared_feed.identity = {"native-htf-dup-shared", 1};
    shared_feed.subscriptions[0].authoritative_bars = {Bar{1, 1, 1, 1, 1, 1000}};
    shared_feed.subscriptions[1].authoritative_bars =
        shared_feed.subscriptions[0].authoritative_bars;
    SeriesHost shared;
    CHECK(shared.configure_native(shared_feed).status == NativeSetupStatus::Applied);

    NativeRunSpec conflicting = shared_feed;
    conflicting.identity = {"native-htf-dup-conflict", 1};
    conflicting.subscriptions[1].authoritative_bars = {Bar{2, 2, 2, 2, 2, 1000}};
    SeriesHost conflicted;
    const auto conflict_setup = conflicted.configure_native(conflicting);
    CHECK(conflict_setup.status == NativeSetupStatus::Failed);
    CHECK(conflict_setup.validation.error
          == NativeRunSpecError::DuplicateSubscriptionTimeframe);
    CHECK(conflict_setup.validation.field == NativeRunSpecField::SubscriptionBars);

    NativeRunSpec unordered = base_spec("15", "15", "native-htf-unordered");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    hourly.authoritative_bars = {Bar{1, 1, 1, 1, 1, 2000}, Bar{1, 1, 1, 1, 1, 1000}};
    unordered.subscriptions.push_back(hourly);
    SeriesHost fourth;
    const auto unordered_setup = fourth.configure_native(unordered);
    CHECK(unordered_setup.status == NativeSetupStatus::Failed);
    CHECK(unordered_setup.validation.error
          == NativeRunSpecError::UnorderedSubscriptionBars);
    CHECK(unordered_setup.validation.field == NativeRunSpecField::SubscriptionBars);
}

// ---- 5. weekly over a daily input, against the Pine path -------------------

std::vector<Bar> daily_bars(int n) {
    // 2024-01-01 is a Monday, so the run opens a week exactly.
    const std::int64_t origin = utc_ms(2024, 1, 1);
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double open = 50.0 + 0.25 * i;
        Bar bar{};
        bar.open = open;
        bar.high = open + 1.5;
        bar.low = open - 0.75;
        bar.close = open + 0.5;
        bar.volume = 1.0;
        bar.timestamp = origin + static_cast<std::int64_t>(i) * kDay;
        bars.push_back(bar);
    }
    return bars;
}

void test_weekly_matches_the_pine_path() {
    scenario = "weekly twin";
    const std::vector<Bar> bars = daily_bars(40);

    NativeRunSpec spec = base_spec("D", "D", "native-htf-weekly");
    NativeTimeframeSubscription weekly;
    weekly.tf = "W";
    spec.subscriptions.push_back(weekly);

    SeriesHost native;
    const auto setup = native.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    native.run(bars.data(), static_cast<int>(bars.size()), "D", "D", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(native.last_error().empty());
    if (!native.last_error().empty()) std::printf("  error: %s\n", native.last_error().c_str());

    PineSeriesProbe pine;
    pine.requested_tf = "W";
    pine.set_syminfo_timezone("UTC");
    pine.set_syminfo_session("24x7");
    pine.set_syminfo_type("crypto");
    pine.run(bars.data(), static_cast<int>(bars.size()), "D", "D", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(pine.last_error().empty());
    if (!pine.last_error().empty()) std::printf("  pine error: %s\n", pine.last_error().c_str());

    CHECK(!pine.completed.empty());
    CHECK(native.deliveries.size() == pine.completed.size());
    if (native.deliveries.size() != pine.completed.size()) {
        std::printf("  native %zu buckets, pine %zu\n", native.deliveries.size(),
                    pine.completed.size());
        return;
    }
    for (std::size_t i = 0; i < native.deliveries.size(); ++i) {
        check_bucket(native.deliveries[i].bar, pine.completed[i].bar, "weekly bucket");
        // The same chart bar publishes it on both routes.
        CHECK(native.deliveries[i].bars_before == pine.completed[i].bars_before);
        // One unit of volume per contributing daily bar: a whole week.
        CHECK(same(native.deliveries[i].bar.volume, 7.0));
        CHECK(native.deliveries[i].context.delivered_at_ms
              == bars[static_cast<std::size_t>(pine.completed[i].bars_before)].timestamp);
    }
    // Weekly boundaries: consecutive labels are exactly seven days apart.
    for (std::size_t i = 1; i < native.deliveries.size(); ++i) {
        CHECK(native.deliveries[i].bar.timestamp
              - native.deliveries[i - 1].bar.timestamp == 7 * kDay);
    }
}

// ---- 6. a spec without subscriptions keeps its continuation identity -------

// The portable pin of that identity. A raw native_continuation_hash() constant
// is not portable — the consumer folds the resolved timezone identity, whose
// zoneinfo root and zone file paths belong to the machine that ran it, so the
// same run hashes differently here and on each CI runner.
// native_run_spec_digest() is exactly the consumer's run-spec fold and nothing
// else, so it is the same number everywhere. Observed on this tree for
// base_spec("15", "15", "native-htf-neutral"); it guards the fold's field list
// and order, while the neutrality itself is the equality below (a spec whose
// `subscriptions` vector is empty hashes as it did before the field existed,
// and declaring one series moves it).
constexpr std::uint64_t kNeutralSpecDigest = 4124988313390852746ull;

class SilentHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

std::uint64_t neutral_continuation_hash(const NativeRunSpec& spec) {
    const std::vector<Bar> bars = quarter_hour_bars(8);
    SilentHost host;
    host.configure_native(spec);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    return host.native_continuation_hash();
}

void test_hash_neutrality() {
    scenario = "hash neutrality";
    NativeRunSpec plain = base_spec("15", "15", "native-htf-neutral");
    const std::uint64_t digest = native_run_spec_digest(plain);
    if (digest != kNeutralSpecDigest) {
        std::printf("  spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kNeutralSpecDigest));
    }
    CHECK(digest == kNeutralSpecDigest);

    // Stating the field at its default — an empty series list — folds nothing.
    NativeRunSpec stated = plain;
    stated.subscriptions.clear();
    CHECK(native_run_spec_digest(stated) == kNeutralSpecDigest);

    // The run itself keeps its identity too: continuation hashes are compared
    // between two runs in this process, never against a constant, because a
    // continuation hash also folds this machine's timezone resources. Stating
    // the empty series list explicitly drives the batch to the same identity.
    const std::uint64_t hash = neutral_continuation_hash(plain);
    CHECK(neutral_continuation_hash(stated) == hash);

    // The subscriptions digest is folded only when the series exist, so the two
    // specs cannot share a spec fold — nor a continuation — once one declares
    // any.
    NativeRunSpec declared = plain;
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    declared.subscriptions.push_back(hourly);
    CHECK(native_run_spec_digest(declared) != kNeutralSpecDigest);
    const std::vector<Bar> bars = quarter_hour_bars(8);
    SeriesHost with_series;
    CHECK(with_series.configure_native(declared).status == NativeSetupStatus::Applied);
    with_series.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
                    MagnifierDistribution::ENDPOINTS);
    CHECK(with_series.last_error().empty());
    CHECK(with_series.native_continuation_hash() != hash);
}

// ---- 7. the pump is ordered against the script interval --------------------
//
// Input "15", script "60": the kernel's own aggregated chart, one calculation
// per hour. The feed has a hole from 0:45 through 1:30 -- the four bars an
// exchange outage swallows; a batch admits it -- so hour 0's script bar never
// meets an input of its own that closes it. The first input after the hole,
// 1:45, is the first input of hour 1: it seals hour 0 LazyComplete (the LAZY
// SEAL) and, being hour 1's last bar as well, seals hour 1 Confirmed once it
// has contributed to it. Four series ride on that one input:
//   0. "120": its [0:00, 2:00) bucket is {0:00, 0:15, 0:30, 1:45} and reaches
//      its real end ON 1:45 -- a bucket holding the very input that seals
//      hour 0;
//   1. "45" gaps_on: [0:00, 0:45) completed on 0:30; 1:45 opens [1:30, 2:15)
//      and delivers nothing, so 1:45 CLEARS the series;
//   2. "60": hour 0's bucket closes only at the boundary 1:45 crosses, so it
//      is delivered LazyComplete on 1:45;
//   3. "60" lookahead_on: hour 1's bucket, {1:45}, is delivered on its first
//      input, 1:45.
// Hour 0's calculation reads the series exactly as its own inputs left them
// -- 0 empty, 1 the [0:00, 0:45) bucket, 2 empty, 3 hour 0's bucket -- then
// come 1:45's deliveries, then hour 1's calculation. Before this change the
// pump ran on 1:45 BEFORE the lazy seal, so hour 0 read 0 = the [0:00, 2:00)
// bucket holding 1:45, 1 = empty, 2 = its own hour's bucket and 3 = hour 1's
// bucket: the one-input leak this scenario pins shut (it fails on 87b3b06).

// The 12 quarter hours of 0:00-2:45 without 0:45, 1:00, 1:15 and 1:30.
std::vector<Bar> holed_quarter_hours() {
    const std::vector<Bar> all = quarter_hour_bars(12);
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i >= 3 && i <= 6) continue;
        bars.push_back(all[i]);
    }
    return bars;
}

void check_series(const std::optional<Bar>& got, const std::optional<Bar>& want,
                  const char* tag) {
    CHECK(got.has_value() == want.has_value());
    if (got.has_value() != want.has_value()) {
        if (got) {
            std::printf("  %s: got t %lld o %.6g h %.6g l %.6g c %.6g v %.6g; want empty\n",
                        tag, static_cast<long long>(got->timestamp), got->open, got->high,
                        got->low, got->close, got->volume);
        } else {
            std::printf("  %s: empty, want a bucket\n", tag);
        }
        return;
    }
    if (got && want) check_bucket(*got, *want, tag);
}

void test_lazy_seal_precedes_the_pump() {
    scenario = "lazy seal precedes the pump";
    const std::vector<Bar> bars = holed_quarter_hours();
    CHECK(bars.size() == 8);
    const std::int64_t origin = bars.front().timestamp;
    CHECK(bars[3].timestamp == origin + 105 * kMinute);

    NativeRunSpec spec = base_spec("15", "60", "native-htf-lazy-seal");
    NativeTimeframeSubscription two_hourly;
    two_hourly.tf = "120";
    NativeTimeframeSubscription gapped;
    gapped.tf = "45";
    gapped.gaps = true;
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    NativeTimeframeSubscription hourly_lookahead;
    hourly_lookahead.tf = "60";
    hourly_lookahead.lookahead = true;
    spec.subscriptions.push_back(two_hourly);
    spec.subscriptions.push_back(gapped);
    spec.subscriptions.push_back(hourly);
    spec.subscriptions.push_back(hourly_lookahead);

    SeriesHost host;
    host.probes = 4;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "60", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());

    // The premise: three script bars, and hour 0's is the lazily sealed one.
    CHECK(host.bars_seen == 3);
    CHECK(host.completions.size() == 3);
    if (host.completions.size() == 3) {
        CHECK(host.completions[0] == NativeCompletionKind::LazyComplete);
        CHECK(host.completions[1] == NativeCompletionKind::Confirmed);
        CHECK(host.completions[2] == NativeCompletionKind::Confirmed);
    }

    // The buckets, by hand.
    const Bar hour0 = hand_aggregate(bars, 0, 3, origin);
    const Bar hour1 = hand_aggregate(bars, 3, 1, origin + kHour);
    const Bar hour2 = hand_aggregate(bars, 4, 4, origin + 2 * kHour);
    const Bar two_hours = hand_aggregate(bars, 0, 4, origin);
    const Bar first45 = hand_aggregate(bars, 0, 3, origin);
    const Bar second45 = hand_aggregate(bars, 3, 2, origin + 90 * kMinute);
    const Bar third45 = hand_aggregate(bars, 5, 3, origin + 135 * kMinute);

    // What each calculation read. Hour 0 reads the series as its own three
    // inputs left them: nothing of 1:45 -- not the bucket it completes, not
    // the clear it performs, not the boundary it closes, not the lookahead
    // bucket it opens.
    CHECK(host.series_at_bar.size() == 3);
    if (host.series_at_bar.size() == 3) {
        const auto& hour0_read = host.series_at_bar[0];
        CHECK(hour0_read.size() == 4);
        if (hour0_read.size() == 4) {
            check_series(hour0_read[0], std::nullopt, "hour 0 reads 120");
            check_series(hour0_read[1], first45, "hour 0 reads 45 gaps");
            check_series(hour0_read[2], std::nullopt, "hour 0 reads 60");
            check_series(hour0_read[3], hour0, "hour 0 reads 60 lookahead");
        }
        // Hour 1 -- {1:45}, sealed by its own input right after -- reads what
        // 1:45 delivered: the 120 bucket, the cleared gapped series, hour 0's
        // bucket, and its own bucket under lookahead.
        const auto& hour1_read = host.series_at_bar[1];
        CHECK(hour1_read.size() == 4);
        if (hour1_read.size() == 4) {
            check_series(hour1_read[0], two_hours, "hour 1 reads 120");
            check_series(hour1_read[1], std::nullopt, "hour 1 reads 45 gaps");
            check_series(hour1_read[2], hour0, "hour 1 reads 60");
            check_series(hour1_read[3], hour1, "hour 1 reads 60 lookahead");
        }
        const auto& hour2_read = host.series_at_bar[2];
        CHECK(hour2_read.size() == 4);
        if (hour2_read.size() == 4) {
            check_series(hour2_read[0], two_hours, "hour 2 reads 120");
            check_series(hour2_read[1], third45, "hour 2 reads 45 gaps");
            check_series(hour2_read[2], hour2, "hour 2 reads 60");
            check_series(hour2_read[3], hour2, "hour 2 reads 60 lookahead");
        }
    }

    // The deliveries, in order, each with the number of script bars already
    // calculated when it arrived: the three riding on 1:45 arrive AFTER hour
    // 0's calculation (1) and before hour 1's.
    struct Want {
        std::size_t subscription;
        Bar bar;
        std::int64_t delivered_at_ms;
        NativeCompletionKind completion;
        int bars_before;
    };
    const std::vector<Want> want_deliveries{
        {3, hour0, bars[0].timestamp, NativeCompletionKind::LazyComplete, 0},
        {1, first45, bars[2].timestamp, NativeCompletionKind::Confirmed, 0},
        {0, two_hours, bars[3].timestamp, NativeCompletionKind::Confirmed, 1},
        {2, hour0, bars[3].timestamp, NativeCompletionKind::LazyComplete, 1},
        {3, hour1, bars[3].timestamp, NativeCompletionKind::LazyComplete, 1},
        {1, second45, bars[4].timestamp, NativeCompletionKind::Confirmed, 2},
        {2, hour1, bars[4].timestamp, NativeCompletionKind::LazyComplete, 2},
        {3, hour2, bars[4].timestamp, NativeCompletionKind::Confirmed, 2},
        {1, third45, bars[7].timestamp, NativeCompletionKind::Confirmed, 2},
        {2, hour2, bars[7].timestamp, NativeCompletionKind::Confirmed, 2},
    };
    CHECK(host.deliveries.size() == want_deliveries.size());
    if (host.deliveries.size() == want_deliveries.size()) {
        for (std::size_t i = 0; i < want_deliveries.size(); ++i) {
            const Delivery& got = host.deliveries[i];
            const Want& want = want_deliveries[i];
            CHECK(got.subscription == want.subscription);
            check_bucket(got.bar, want.bar, "lazy-seal delivery");
            CHECK(got.context.delivered_at_ms == want.delivered_at_ms);
            CHECK(got.context.completion == want.completion);
            CHECK(got.bars_before == want.bars_before);
            CHECK(got.accessor_matches);
        }
    }

    // The whole sequence. On 1:45: the input, hour 0's calculation, 1:45's
    // three deliveries, hour 1's calculation. (A script bar is presented
    // under its first contributing input's stamp.)
    const auto stamp = [&bars](std::size_t i) {
        return std::to_string(bars[i].timestamp);
    };
    const auto label = [origin](std::int64_t offset_ms) {
        return std::to_string(origin + offset_ms);
    };
    const std::vector<std::string> want_log{
        "input:0@" + stamp(0), "htf:3@" + label(0),
        "input:1@" + stamp(1),
        "input:2@" + stamp(2), "htf:1@" + label(0),
        "input:3@" + stamp(3), "bar@" + stamp(0),
            "htf:0@" + label(0), "htf:2@" + label(0), "htf:3@" + label(kHour),
            "bar@" + stamp(3),
        "input:4@" + stamp(4), "htf:1@" + label(90 * kMinute),
            "htf:2@" + label(kHour), "htf:3@" + label(2 * kHour),
        "input:5@" + stamp(5),
        "input:6@" + stamp(6),
        "input:7@" + stamp(7), "htf:1@" + label(135 * kMinute),
            "htf:2@" + label(2 * kHour), "bar@" + stamp(4),
    };
    CHECK(host.log == want_log);
    if (host.log != want_log) {
        for (std::size_t i = 0; i < std::max(host.log.size(), want_log.size()); ++i) {
            const std::string got = i < host.log.size() ? host.log[i] : std::string("-");
            const std::string exp = i < want_log.size() ? want_log[i] : std::string("-");
            if (got != exp) std::printf("  log[%zu]: got %s want %s\n", i, got.c_str(),
                                        exp.c_str());
        }
    }

    // The same feed with no series declared calculates the same three bars,
    // sealed the same way: the reordering lives inside the subscription
    // branch and a run without subscriptions never enters it.
    NativeRunSpec plain = base_spec("15", "60", "native-htf-lazy-seal-plain");
    SeriesHost bare;
    CHECK(bare.configure_native(plain).status == NativeSetupStatus::Applied);
    bare.run(bars.data(), static_cast<int>(bars.size()), "15", "60", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(bare.last_error().empty());
    CHECK(bare.deliveries.empty());
    CHECK(bare.completions == host.completions);
    std::vector<std::string> bare_log;
    for (const std::string& entry : host.log) {
        if (entry.rfind("htf:", 0) != 0) bare_log.push_back(entry);
    }
    CHECK(bare.log == bare_log);
}

}  // namespace

int main() {
    test_hourly_over_quarter_hour();
    test_hourly_lookahead();
    test_same_timeframe_instances();
    test_gaps_clears_between_deliveries();
    test_begin_time_declaration();
    test_teardown_precedes_run_begin();
    test_authoritative_bars_override();
    test_finer_than_input_is_refused();
    test_weekly_matches_the_pine_path();
    test_hash_neutrality();
    test_lazy_seal_precedes_the_pump();
    std::printf("native HTF subscriptions: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
