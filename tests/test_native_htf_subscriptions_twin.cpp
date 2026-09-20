// Declared higher-timeframe series, the TWIN half (witness 5 of
// tests/test_native_htf_subscriptions.cpp): a "W" series over a daily input
// reproduces, bucket for bucket, what the Pine request.security path produces
// for the same bars. The twin drives source::PineStrategyHost exactly as
// tests/test_native_wm_buckets.cpp does (register_security_eval +
// evaluate_security), so this half binds pineforge/source and
// tests/CMakeLists.txt registers it only when PINEFORGE_BUILD_SOURCE_LAYER is
// ON; the other witnesses are source-free and run in the kernel-only profile.
#include "native_htf_subscriptions_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace l6_fixture;

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

}  // namespace

int main() {
    test_weekly_matches_the_pine_path();
    std::printf("native HTF subscriptions twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
