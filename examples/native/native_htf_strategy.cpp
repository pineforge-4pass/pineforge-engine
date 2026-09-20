// Pine-free native example: higher-timeframe series for a bare host (R5
// lanes L6, L6c and L6d).
//
// A host reads a coarser series of its own symbol -- Pine's
// request.security(syminfo.tickerid, "60", close) -- by declaring it. The
// kernel aggregates the accepted 15-minute input into hourly buckets and
//   * pushes each completed bucket to on_native_timeframe_bar, riding on the
//     input bar that completed it, before that input is aggregated, matched
//     or calculated;
//   * answers native_series_bar(i) with the latest delivered bucket from
//     inside every callback (the pull side, used here to trade on it).
// Two series are declared here, from on_native_run_begin
// (declare_timeframe_subscriptions, for a host whose series are known only
// at begin; NativeRunSpec::subscriptions is the declaration-time spelling):
//   * index 0, "60" with gaps = false: the delivered bucket stands until the
//     next delivery replaces it;
//   * index 1, "60" with gaps = true: the series is cleared on every input bar
//     it delivers nothing on, so native_series_bar(1) answers nullopt there
//     -- the empty that stands for na.
// The input has a hole: the second hour is missing its :45 bar, so its bucket
// cannot close on its own last input and is sealed lazily by the next hour's
// first bar (NativeTimeframeBarContext::completion == LazyComplete, delivered
// at 02:00). The other two buckets close on their own fourth bar (Confirmed).
// The trailing hour has one bar only and is never delivered: a bucket still
// open at the end of the input is not a bucket.
//
//   c++ -std=c++17 native_htf_strategy.cpp -lpineforge_kernel -o htf
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cstdio>
#include <iostream>
#include <optional>
#include <vector>

namespace {

namespace no = pineforge::native_order;

const char* completion_name(pineforge::NativeCompletionKind kind) {
    switch (kind) {
        case pineforge::NativeCompletionKind::Confirmed: return "Confirmed";
        case pineforge::NativeCompletionKind::LazyComplete: return "LazyComplete";
        case pineforge::NativeCompletionKind::SessionShortened: return "SessionShortened";
        case pineforge::NativeCompletionKind::PartialFinalized: return "PartialFinalized";
    }
    return "?";
}

class HtfExample : public pineforge::NativeStrategyHost {
public:
    struct Delivery {
        std::size_t subscription;
        pineforge::Bar bucket;
        pineforge::NativeCompletionKind completion;
        std::int64_t delivered_at_ms;
    };
    std::vector<Delivery> deliveries;
    bool declared = false;
    int bars = 0;
    int series0_present = 0;   // gaps = false: stands until replaced
    int series1_present = 0;   // gaps = true: only on its own delivery bars

private:
    std::optional<std::int64_t> acted_on_bucket_;

    // L6c: the series are declared here, before the kernel registers them.
    void on_native_run_begin() override {
        bars = 0;
        deliveries.clear();
        acted_on_bucket_.reset();
        pineforge::NativeTimeframeSubscription hourly;
        hourly.tf = "60";
        pineforge::NativeTimeframeSubscription hourly_gaps = hourly;
        hourly_gaps.gaps = true;
        declared = declare_timeframe_subscriptions({hourly, hourly_gaps});
    }

    // The push side: one call per completed bucket, per series.
    void on_native_timeframe_bar(const pineforge::Bar& bucket,
                                 const pineforge::NativeTimeframeBarContext& context) override {
        deliveries.push_back({context.subscription, bucket, context.completion,
                              context.delivered_at_ms});
    }

    // The pull side: trade on the hourly close, once per hourly bucket.
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars;
        if (native_series_bar(0)) ++series0_present;
        if (native_series_bar(1)) ++series1_present;
        const auto hour = native_series_bar(0);
        if (!hour || (acted_on_bucket_ && *acted_on_bucket_ == hour->timestamp)) return;
        acted_on_bucket_ = hour->timestamp;
        const bool flat = physical_position().signed_units == 0.0;
        if (hour->close > hour->open && flat) {
            submit({no::Transact{1.0}, "hour-up", "htf"});
        } else if (hour->close < hour->open && !flat) {
            submit({no::Flatten{}, "hour-down", "htf"});
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-htf-example";
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
    spec.price_tick = 0.25;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    // spec.subscriptions stays empty: this host declares at begin instead.
    return spec;
}

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;

// open, high, low, close, volume, timestamp (Unix milliseconds).
// Hour 0 rises (bucket closes up), hour 1 falls and is missing its 01:45 bar,
// hour 2 rises again, hour 3 has one bar and is never delivered.
const pineforge::Bar kBars[] = {
    {100.00, 100.50,  99.75, 100.25, 4.0,  0 * kQuarter},   // 00:00
    {100.25, 100.75, 100.00, 100.50, 4.0,  1 * kQuarter},   // 00:15
    {100.50, 101.00, 100.25, 100.75, 4.0,  2 * kQuarter},   // 00:30
    {100.75, 101.25, 100.50, 101.00, 4.0,  3 * kQuarter},   // 00:45 -> bucket 0 Confirmed
    {101.00, 101.25, 100.25, 100.50, 4.0,  4 * kQuarter},   // 01:00 (long fills here)
    {100.50, 100.75,  99.75, 100.00, 4.0,  5 * kQuarter},   // 01:15
    {100.00, 100.25,  99.25,  99.50, 4.0,  6 * kQuarter},   // 01:30 (01:45 is missing)
    { 99.50, 100.00,  99.00,  99.75, 4.0,  8 * kQuarter},   // 02:00 -> bucket 1 LazyComplete
    { 99.75, 100.25,  99.50, 100.00, 4.0,  9 * kQuarter},   // 02:15 (flatten fills here)
    {100.00, 100.50,  99.75, 100.25, 4.0, 10 * kQuarter},   // 02:30
    {100.25, 100.75, 100.00, 100.50, 4.0, 11 * kQuarter},   // 02:45 -> bucket 2 Confirmed
    {100.50, 101.00, 100.25, 100.75, 4.0, 12 * kQuarter},   // 03:00 (long fills here; hour 3 stays open)
};
constexpr int kBarCount = 12;

}  // namespace

int main() {
    HtfExample host;
    if (host.configure_native(make_spec()).status
        != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return 1;
    }

    host.run(kBars, kBarCount);
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run: " << host.last_error() << '\n';
        return 1;
    }
    if (!host.declared) {
        std::cerr << "declare_timeframe_subscriptions refused the list at begin\n";
        return 1;
    }

    // --- the push side: three buckets per series, in input order ------------
    std::printf("deliveries: %zu\n", host.deliveries.size());
    for (const auto& d : host.deliveries) {
        std::printf("  series %zu: hour opened %02lld:%02lld o=%.2f h=%.2f l=%.2f c=%.2f  %s, delivered on the %02lld:%02lld bar\n",
                    d.subscription,
                    static_cast<long long>(d.bucket.timestamp / 3600000),
                    static_cast<long long>(d.bucket.timestamp / 60000 % 60),
                    d.bucket.open, d.bucket.high, d.bucket.low, d.bucket.close,
                    completion_name(d.completion),
                    static_cast<long long>(d.delivered_at_ms / 3600000),
                    static_cast<long long>(d.delivered_at_ms / 60000 % 60));
    }
    std::vector<HtfExample::Delivery> series0;
    for (const auto& d : host.deliveries) if (d.subscription == 0) series0.push_back(d);
    if (host.deliveries.size() != 6 || series0.size() != 3) {
        std::cerr << "expected three hourly buckets on each of the two series\n";
        return 1;
    }
    const bool shapes_ok =
        series0[0].completion == pineforge::NativeCompletionKind::Confirmed
        && series0[0].bucket.open == 100.00 && series0[0].bucket.close == 101.00
        && series0[0].delivered_at_ms == 3 * kQuarter
        && series0[1].completion == pineforge::NativeCompletionKind::LazyComplete
        && series0[1].bucket.open == 101.00 && series0[1].bucket.close == 99.50
        && series0[1].delivered_at_ms == 8 * kQuarter
        && series0[2].completion == pineforge::NativeCompletionKind::Confirmed
        && series0[2].bucket.open == 99.50 && series0[2].bucket.close == 100.50
        && series0[2].delivered_at_ms == 11 * kQuarter;
    if (!shapes_ok) {
        std::cerr << "unexpected bucket shapes, completions or delivery bars\n";
        return 1;
    }

    // --- the pull side: gaps = false stands, gaps = true clears -------------
    std::printf("native_series_bar inside on_native_bar: series 0 (gaps off) present on %d of %d bars, "
                "series 1 (gaps on) present on %d of %d bars\n",
                host.series0_present, host.bars, host.series1_present, host.bars);
    if (host.bars != kBarCount || host.series0_present != 9 || host.series1_present != 3) {
        std::cerr << "expected the gaps-off series from its first delivery on, the gaps-on series on delivery bars only\n";
        return 1;
    }

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
