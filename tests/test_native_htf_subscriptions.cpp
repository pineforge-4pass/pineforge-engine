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
//      and moves as soon as one series is declared.

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bars_seen;
        if (probes > 0) {
            std::vector<std::optional<Bar>> row;
            row.reserve(probes);
            for (std::size_t i = 0; i < probes; ++i) row.push_back(native_series_bar(i));
            series_at_bar.push_back(std::move(row));
        }
        log.push_back("bar@" + std::to_string(bar.timestamp));
    }
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

}  // namespace

int main() {
    test_hourly_over_quarter_hour();
    test_hourly_lookahead();
    test_same_timeframe_instances();
    test_gaps_clears_between_deliveries();
    test_authoritative_bars_override();
    test_finer_than_input_is_refused();
    test_weekly_matches_the_pine_path();
    test_hash_neutrality();
    std::printf("native HTF subscriptions: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
