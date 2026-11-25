#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace pineforge;

namespace {
constexpr std::array<double, 10> closes{23, 22, 21, 22, 22, 25, 22, 21, 20, 19};

class RankHost final : public source::PineStrategyHost {
public:
    RankHost() {
        attach_pine_execution_adapter();
        configure_pine_strategy(source::PineStrategyConfig{});
    }

    void on_source_bar(const Bar& bar) override {
        const double value = rci_.compute(bar.close);
        const bool buy = up_.compute(value, 0.0);
        const bool sell = down_.compute(value, 0.0);
        if (seen < values.size()) values[seen] = value;
        if (seen == 0 || sell) strategy_entry("sell", false);
        if (buy) strategy_entry("buy", true);
        ++seen;
    }

    std::size_t seen = 0;
    std::array<double, closes.size()> values{};

private:
    ta::RCI rci_{3};
    ta::Crossover up_;
    ta::Crossunder down_;
};
}

int main() {
    std::array<Bar, closes.size()> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double open = i ? closes[i - 1] : closes[0];
        bars[i] = {open, std::max(open, closes[i]), std::min(open, closes[i]),
                   closes[i], 10, static_cast<std::int64_t>(i + 1) * 60000};
    }
    RankHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (host.last_run_status() != 0 || host.seen != bars.size() || host.trade_count() != 2) {
        std::printf("FAIL run: status=%d bars=%zu closed=%d\n",
                    host.last_run_status(), host.seen, host.trade_count());
        return 1;
    }
    const auto first = host.get_trade(0);
    const auto second = host.get_trade(1);
    // The initial sale and the exit on the falling, untied window both fill
    // at the following open, regardless of how equal ranks are treated.
    if (first.is_long || !second.is_long || first.qty != 1 || second.qty != 1 ||
        first.entry_time != bars[1].timestamp || first.entry_price != bars[1].open ||
        second.exit_time != bars[8].timestamp || second.exit_price != bars[8].open ||
        !std::isnan(host.values[0]) || !std::isnan(host.values[1]) ||
        host.values[2] != -100 || host.values[7] != -100) {
        std::puts("FAIL untied ranks, warmup or next-open control");
        return 1;
    }
    // At bar 3 the centered price ranks are (1/2,-1,1/2); their dot
    // product with (-1,0,1) is zero. At bar 4 they are (-1,1/2,1/2),
    // so rho = 1.5 / sqrt(2 * 1.5) = sqrt(3)/2. The first positive
    // signal fills at open 5. Bar 6 is the mirror of the zero at bar 3.
    if (host.values[3] != 0 || host.values[6] != 0 ||
        !(std::abs(host.values[4] - 50 * std::sqrt(3.0)) < 1e-12) ||
        first.exit_time != bars[5].timestamp || first.exit_price != bars[5].open ||
        second.entry_time != bars[5].timestamp || second.entry_price != bars[5].open) {
        std::printf("FAIL tied reversal: RCI=%g long entry=%lld expected=%lld\n",
                    host.values[3], static_cast<long long>(second.entry_time),
                    static_cast<long long>(bars[5].timestamp));
        return 1;
    }
    std::puts("PASS tied reversal: zero at bar 3, long at open 5");
    return 0;
}
