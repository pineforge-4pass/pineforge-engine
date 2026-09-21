// Pine-free native example: the auxiliary finer feed (R5 gap lane N7).
//
// A declared series is normally built from the run's own input, so it can
// never be finer than that input: a "5" series under a "15" input has nothing
// to aggregate. NativeRunSpec::auxiliary_feed is the bars the input does not
// have — the run's OWN symbol at a strictly finer timeframe — and a
// subscription whose NativeSeriesSource is AuxiliaryFeed is built from them
// instead. Pine has no spelling for this: request.security cannot go finer
// than the chart, and request.security_lower_tf returns an intrabar array
// rather than a series. It is what a host does when its decision feed is
// coarse and its signal is not.
//
// This host runs 15-minute inputs over a 60-minute (one hour) auxiliary feed
// of 1-minute bars and declares one "5" series on it. What the example proves,
// each check independent of the feature it checks:
//   * the very same subscription is REFUSED when no feed is declared
//     (NativeRunSpecError::SubscriptionWithoutAuxiliaryFeed), so the feed is
//     what makes it legal;
//   * every delivered bucket equals the hand aggregation of its five minutes;
//   * routing is by time: the three buckets that opened inside an input's
//     period ride on that input, oldest first, BEFORE its calculation, so the
//     bar the host decides on already sees the last of them;
//   * native_series_bar(0) inside on_native_bar is that last delivered bucket.
// The strategy then trades on it: long on a five-minute bucket that closed up,
// flat on one that closed down — a decision three times finer than the bar the
// kernel calculates on.
//
// A stream learns its feed live instead: append_auxiliary_bars(bars, n) adds
// the later bars of a realtime feed to the declared one (the C spelling is
// strategy_native_append_auxiliary_bars_v1). That path is pinned by
// tests/test_native_auxiliary_feed_stream.cpp; this host is batch.
//
//   c++ -std=c++17 native_auxiliary_feed_strategy.cpp -lpineforge_kernel -o aux_feed
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <vector>

namespace {

namespace no = pineforge::native_order;

constexpr std::int64_t kMinute = 60LL * 1000LL;
constexpr int kMinutes = 60;      // one hour of feed
constexpr int kInputs = 4;        // 4 x 15 minutes
constexpr int kBuckets = 12;      // 12 x 5 minutes

bool same(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
}

// One minute of tape. The close walks up for twenty minutes, down for twenty
// and up again for twenty, so the five-minute buckets alternate direction in a
// shape the reader can check by eye: buckets 0-3 up, 4-7 down, 8-11 up.
std::vector<pineforge::Bar> minute_bars() {
    std::vector<pineforge::Bar> bars;
    bars.reserve(kMinutes);
    double close = 100.0;
    for (int k = 0; k < kMinutes; ++k) {
        const double step = (k < 20 || k >= 40) ? 0.10 : -0.10;
        const double open = close;
        close = open + step;
        pineforge::Bar bar{};
        bar.open = open;
        bar.close = close;
        bar.high = (open > close ? open : close) + 0.05;
        bar.low = (open < close ? open : close) - 0.05;
        bar.volume = 1.0 + (k % 4);
        bar.timestamp = static_cast<std::int64_t>(k) * kMinute;
        bars.push_back(bar);
    }
    return bars;
}

// The oracle every expected bucket is derived from: one contiguous group of
// feed bars, labelled by the group's first timestamp.
pineforge::Bar hand_aggregate(const std::vector<pineforge::Bar>& bars,
                              std::size_t from, std::size_t count) {
    pineforge::Bar out = bars[from];
    for (std::size_t i = 1; i < count; ++i) {
        const pineforge::Bar& next = bars[from + i];
        if (next.high > out.high) out.high = next.high;
        if (next.low < out.low) out.low = next.low;
        out.close = next.close;
        out.volume += next.volume;
    }
    return out;
}

bool bars_equal(const pineforge::Bar& got, const pineforge::Bar& want) {
    return got.timestamp == want.timestamp && same(got.open, want.open)
        && same(got.high, want.high) && same(got.low, want.low)
        && same(got.close, want.close) && same(got.volume, want.volume);
}

class AuxiliaryFeedExample : public pineforge::NativeStrategyHost {
public:
    struct Delivery {
        std::size_t subscription;
        pineforge::Bar bucket;
        int bars_calculated_before;   // 0-based index of the input it rode on
        bool accessor_matches;        // native_series_bar answered this bucket
    };
    std::vector<Delivery> deliveries;
    std::vector<std::optional<pineforge::Bar>> series_at_bar;
    int bars = 0;

private:
    std::optional<std::int64_t> acted_on_bucket_;

    void on_native_run_begin() override {
        bars = 0;
        deliveries.clear();
        series_at_bar.clear();
        acted_on_bucket_.reset();
    }

    // The push side: one call per completed five-minute bucket.
    void on_native_timeframe_bar(const pineforge::Bar& bucket,
                                 const pineforge::NativeTimeframeBarContext& context) override {
        const auto pulled = native_series_bar(context.subscription);
        deliveries.push_back({context.subscription, bucket, bars,
                              pulled.has_value() && bars_equal(*pulled, bucket)});
    }

    // The pull side: decide on the finest bucket the feed has completed.
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars;
        const auto bucket = native_series_bar(0);
        series_at_bar.push_back(bucket);
        if (!bucket || (acted_on_bucket_ && *acted_on_bucket_ == bucket->timestamp)) return;
        acted_on_bucket_ = bucket->timestamp;
        const bool flat = physical_position().signed_units == 0.0;
        if (bucket->close > bucket->open && flat) {
            submit({no::Transact{1.0}, "bucket-up", "aux"});
        } else if (bucket->close < bucket->open && !flat) {
            submit({no::Flatten{}, "bucket-down", "aux"});
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-auxiliary-feed-example";
    spec.identity.run_number = 1;
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;

    pineforge::NativeTimeframeSubscription five;
    five.tf = "5";
    five.source = pineforge::NativeSeriesSource::AuxiliaryFeed;
    spec.subscriptions.push_back(five);
    return spec;
}

}  // namespace

int main() {
    const std::vector<pineforge::Bar> minutes = minute_bars();

    // The 15-minute input is the same tape at the run's own timeframe.
    std::vector<pineforge::Bar> inputs;
    inputs.reserve(kInputs);
    for (int i = 0; i < kInputs; ++i) {
        inputs.push_back(hand_aggregate(minutes, static_cast<std::size_t>(i) * 15, 15));
    }

    // 1. Without the feed the subscription has nothing to aggregate, and the
    //    run spec says so by name rather than silently delivering nothing.
    {
        pineforge::NativeRunSpec without = make_spec();
        const auto refused = pineforge::validate_native_run_spec(without);
        if (refused.error != pineforge::NativeRunSpecError::SubscriptionWithoutAuxiliaryFeed) {
            std::cerr << "a finer series without a feed must be refused by name, got error "
                      << static_cast<int>(refused.error) << '\n';
            return 1;
        }
        std::printf("without a feed: refused, NativeRunSpecError::SubscriptionWithoutAuxiliaryFeed\n");
    }

    pineforge::NativeRunSpec spec = make_spec();
    pineforge::NativeAuxiliaryFeed feed;
    feed.tf = "1";
    feed.bars = minutes;
    spec.auxiliary_feed = feed;

    AuxiliaryFeedExample host;
    if (host.configure_native(spec).status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return 1;
    }
    host.run(inputs.data(), static_cast<int>(inputs.size()));
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run: " << host.last_error() << '\n';
        return 1;
    }

    // 2. Twelve buckets, each the hand aggregation of its own five minutes.
    if (host.deliveries.size() != static_cast<std::size_t>(kBuckets) || host.bars != kInputs) {
        std::cerr << "expected " << kBuckets << " buckets over " << kInputs
                  << " inputs, got " << host.deliveries.size() << " over " << host.bars << '\n';
        return 1;
    }
    for (int j = 0; j < kBuckets; ++j) {
        const auto& delivery = host.deliveries[static_cast<std::size_t>(j)];
        const pineforge::Bar want = hand_aggregate(minutes, static_cast<std::size_t>(j) * 5, 5);
        if (delivery.subscription != 0 || !bars_equal(delivery.bucket, want)) {
            std::cerr << "bucket " << j << " is not the aggregation of its five minutes\n";
            return 1;
        }
        // 3. Routing by time: buckets 3i, 3i+1, 3i+2 opened inside input i's
        //    period, so they ride on it, oldest first, before its calculation.
        if (delivery.bars_calculated_before != j / 3 || !delivery.accessor_matches) {
            std::cerr << "bucket " << j << " rode on input " << delivery.bars_calculated_before
                      << " (expected " << (j / 3) << ") or the pull side disagreed\n";
            return 1;
        }
    }

    std::printf("buckets: %zu over %d inputs, three per input, oldest first\n",
                host.deliveries.size(), host.bars);
    for (int j = 0; j < kBuckets; ++j) {
        const auto& d = host.deliveries[static_cast<std::size_t>(j)];
        std::printf("  %02lld:%02lld  o=%.2f h=%.2f l=%.2f c=%.2f  %s, on input %d\n",
                    static_cast<long long>(d.bucket.timestamp / 3600000),
                    static_cast<long long>(d.bucket.timestamp / kMinute % 60),
                    d.bucket.open, d.bucket.high, d.bucket.low, d.bucket.close,
                    d.bucket.close > d.bucket.open ? "up  " : "down",
                    d.bars_calculated_before);
    }

    // 4. The series the host decided on is the last bucket delivered on that
    //    input: buckets 2, 5, 8 and 11.
    if (host.series_at_bar.size() != static_cast<std::size_t>(kInputs)) {
        std::cerr << "expected one series reading per input bar\n";
        return 1;
    }
    for (int i = 0; i < kInputs; ++i) {
        const auto& seen = host.series_at_bar[static_cast<std::size_t>(i)];
        const pineforge::Bar want = hand_aggregate(minutes, static_cast<std::size_t>(i) * 15 + 10, 5);
        if (!seen || !bars_equal(*seen, want)) {
            std::cerr << "native_series_bar(0) on input " << i
                      << " is not the last bucket that rode on it\n";
            return 1;
        }
    }
    std::printf("native_series_bar(0) on each input bar: the third bucket of that input\n");

    std::cout << "closed trades: " << host.trade_count() << '\n';
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        std::cout << "  " << (trade.is_long ? "long " : "short")
                  << " qty=" << trade.qty
                  << " entry=" << trade.entry_price
                  << " exit=" << trade.exit_price
                  << " pnl=" << trade.pnl << '\n';
    }
    return host.trade_count() > 0 ? 0 : 1;
}
