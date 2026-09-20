// Shared fixture of the two auxiliary-feed test units (batch and stream):
// the observing host, a one-minute feed with a fifteen-minute input built over
// it, and the hand aggregation every expected bucket is derived from. Nothing
// here names a source layer, so both units survive a kernel-only build.
#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/timeframe.hpp>

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

// 2024-01-01 00:00 UTC: on the 5-, 15- and 60-minute grids at once.
std::int64_t origin_ms() { return utc_ms(2024, 1, 1); }

NativeRunSpec base_spec(const char* session_key) {
    NativeRunSpec spec;
    spec.identity = {session_key, 1};
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "AUX";
    spec.tickerid = "TEST:AUX";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native auxiliary feed fixture";
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

// ---- bars -----------------------------------------------------------------

// `n` one-minute bars from `first_ms`, numbered from `number` so two feeds
// over different spans never repeat a value. Every field differs bar to bar,
// so an aggregate that took the wrong bar cannot equal the right one.
std::vector<Bar> minute_bars(std::int64_t first_ms, int n, int number = 0) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int k = number + i;
        const double open = 100.0 + 0.25 * k;
        Bar bar{};
        bar.open = open;
        bar.high = open + 0.5 + 0.01 * (k % 7);
        bar.low = open - 0.4 - 0.01 * (k % 5);
        bar.close = open + 0.1 * ((k % 3) - 1);
        bar.volume = 1.0 + (k % 4);
        bar.timestamp = first_ms + static_cast<std::int64_t>(i) * kMinute;
        bars.push_back(bar);
    }
    return bars;
}

// The oracle: a hand aggregation of one contiguous group of bars.
Bar hand_aggregate(const std::vector<Bar>& bars, std::size_t from, std::size_t count,
                   std::int64_t label) {
    Bar out = bars[from];
    out.timestamp = label;
    for (std::size_t i = 1; i < count; ++i) {
        const Bar& next = bars[from + i];
        out.high = std::max(out.high, next.high);
        out.low = std::min(out.low, next.low);
        out.close = next.close;
        out.volume += next.volume;
    }
    return out;
}

// The fifteen-minute input over a one-minute feed: input bar i is the hand
// aggregate of minutes [15 i, 15 i + 15). `minutes` must start on the grid.
std::vector<Bar> quarter_bars_over(const std::vector<Bar>& minutes, int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        bars.push_back(hand_aggregate(minutes, static_cast<std::size_t>(i) * 15, 15,
                                      minutes[static_cast<std::size_t>(i) * 15].timestamp));
    }
    return bars;
}

NativeAuxiliaryFeed minute_feed(std::vector<Bar> bars) {
    NativeAuxiliaryFeed feed;
    feed.tf = "1";
    feed.bars = std::move(bars);
    return feed;
}

NativeTimeframeSubscription series(const char* tf, NativeSeriesSource source,
                                   bool lookahead = false, bool gaps = false) {
    NativeTimeframeSubscription subscription;
    subscription.tf = tf;
    subscription.source = source;
    subscription.lookahead = lookahead;
    subscription.gaps = gaps;
    return subscription;
}

// ---- host -----------------------------------------------------------------

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

class FeedHost : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    std::vector<Delivery> deliveries;
    int bars_seen = 0;
    // What native_series_bar(i) answered at each script bar, for i < probes.
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

// ---- comparisons ----------------------------------------------------------

bool bars_equal(const Bar& got, const Bar& want) {
    return got.timestamp == want.timestamp && same(got.open, want.open)
        && same(got.high, want.high) && same(got.low, want.low)
        && same(got.close, want.close) && same(got.volume, want.volume);
}

void check_bucket(const Bar& got, const Bar& want, const char* tag) {
    const bool ok = bars_equal(got, want);
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

void check_logs_equal(const std::vector<std::string>& got,
                      const std::vector<std::string>& want) {
    CHECK(got == want);
    if (got == want) return;
    for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
        const std::string a = i < got.size() ? got[i] : std::string("-");
        const std::string b = i < want.size() ? want[i] : std::string("-");
        if (a != b) std::printf("  log[%zu]: got %s want %s\n", i, a.c_str(), b.c_str());
    }
}

// Every delivery field the host can observe, compared one by one.
void check_deliveries_equal(const std::vector<Delivery>& got,
                            const std::vector<Delivery>& want) {
    CHECK(got.size() == want.size());
    if (got.size() != want.size()) {
        std::printf("  %zu deliveries, want %zu\n", got.size(), want.size());
        return;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        check_bucket(got[i].bar, want[i].bar, "got vs want bucket");
        CHECK(got[i].subscription == want[i].subscription);
        CHECK(got[i].bars_before == want[i].bars_before);
        CHECK(got[i].accessor_matches == want[i].accessor_matches);
        CHECK(got[i].context.delivered_at_ms == want[i].context.delivered_at_ms);
        CHECK(got[i].context.completion == want[i].context.completion);
        CHECK(got[i].context.interval.open_ms == want[i].context.interval.open_ms);
        CHECK(got[i].context.interval.next_period_open_ms
              == want[i].context.interval.next_period_open_ms);
    }
}

bool run_batch(FeedHost& host, const NativeRunSpec& spec, const std::vector<Bar>& inputs) {
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) {
        std::printf("  configure refused: error %d field %d\n",
                    static_cast<int>(setup.validation.error),
                    static_cast<int>(setup.validation.field));
        return false;
    }
    host.run(inputs.data(), static_cast<int>(inputs.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    return host.last_error().empty();
}

}  // namespace
