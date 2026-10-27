#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Work = pineforge::source::PineExecutionAdapter::DensePendingWork;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class RetiringHost final : public pineforge::source::PineStrategyHost {
public:
    RetiringHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }
    void on_source_bar(const pineforge::Bar&) override {
        const auto bar = pine_bar_index();
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (bar != 0) strategy_cancel("churn-" + std::to_string(bar - 1));
        // Unique ids leave a growing untouched book beside one retirement
        // per bar. Neither buy limit can reach [99.5,100.5].
        strategy_entry("standing-" + std::to_string(bar), true, 1, na, 1);
        strategy_entry("churn-" + std::to_string(bar), true, 1, na, 1);
        ++evaluations;
    }
    Work work() const { return adapter_.dense_pending_work(); }
    int evaluations = 0;
};

Work run(int n) {
    std::vector<pineforge::Bar> bars(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto& bar = bars[static_cast<std::size_t>(i)];
        bar.timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
        bar.open = bar.close = 100;
        bar.high = 100.5;
        bar.low = 99.5;
        bar.volume = 1000;
    }
    RetiringHost host;
    host.run(bars.data(), n, "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.evaluations == n, "every source bar must execute");
    require(host.trade_count() == 0, "unreachable limits must never trade");
    // Hand arithmetic: two accepted per bar, one cancellation on each bar
    // after the first; 2N-(N-1)=N+1 surviving public native requests.
    require(host.native_working_requests().size() == static_cast<std::size_t>(n + 1),
            "retire witness must retain N+1 requests");
    const auto result = host.work();
    std::printf("CR391B retire N=%d retire.handles=%llu retire.source_keys=%llu\n",
                n, static_cast<unsigned long long>(result.retire_handles),
                static_cast<unsigned long long>(result.retire_keys));
    return result;
}
}

int main() {
    try {
        const auto small = run(256);
        const auto large = run(512);
        // Expected cumulative cost is proportional to N-1 actual removals:
        // 2*(2N-1) <= 5*(N-1) for N>=3. A growing-book scan is quadratic.
        require(small.retire_handles > 0 && small.retire_keys > 0,
                "retire witness must exercise both counted operations");
        require(2 * large.retire_handles <= 5 * small.retire_handles,
                "retire counter retire.handles exceeds 2.5x");
        require(2 * large.retire_keys <= 5 * small.retire_keys,
                "retire counter retire.source_keys exceeds 2.5x");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "live row retire work: %s\n", error.what());
        return 1;
    }
}
