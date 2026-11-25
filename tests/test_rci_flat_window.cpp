// The expected values follow the definition of the rank correlation index:
// Spearman's rank correlation between the window's values and their bar
// order, times 100. Tied values take the average of the ranks they span, as
// Spearman's coefficient prescribes for ties; a window whose values are all
// equal has zero price-rank variance, so the correlation is undefined and
// the result is na. Agreement with TradingView's RCI values is unverified.

#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace pineforge;

namespace {

int failures = 0;

bool check(int length, const char* row, bool ok) {
    std::printf("%s length=%d %s\n", ok ? "PASS" : "FAIL", length, row);
    failures += !ok;
    return ok;
}

void expect(int length, const char* row, double got, double want) {
    const bool ok = is_na(want) ? is_na(got) : std::abs(got - want) < 1e-12;
    if (!check(length, row, ok)) {
        std::printf("  got %.17g, want %.17g\n", got, want);
    }
}

void check_windows(int length) {
    const double missing = na<double>();
    // With one high outlier, the centered price ranks are -1/2 except
    // at the outlier, where the rank is (length - 1)/2. At the newest
    // sample this gives a correlation of sqrt(3 / (length + 1)); at the
    // oldest it gives the opposite sign. A low outlier reverses the signs.
    const double edge = 100.0 * std::sqrt(3.0 / (length + 1));
    ta::RCI rci(length);

    bool warmup = true;
    for (int i = 1; i < length; ++i) warmup &= is_na(rci.compute(1.0));
    check(length, "warmup", warmup);
    expect(length, "flat from start", rci.compute(1.0), missing);
    expect(length, "move after flat", rci.compute(2.0), edge);
    for (int i = 1; i < length - 1; ++i) rci.compute(1.0);
    expect(length, "missing input", rci.compute(missing), missing);
    expect(length, "oldest outlier after missing input", rci.compute(1.0), -edge);
    expect(length, "flat after move", rci.compute(1.0), missing);

    expect(length, "move before recompute", rci.compute(0.0), -edge);
    expect(length, "recompute into flat", rci.recompute(1.0), missing);
    expect(length, "recompute out of flat", rci.recompute(2.0), edge);
    for (int i = 1; i < length - 1; ++i) rci.compute(1.0);
    expect(length, "oldest outlier after recompute", rci.compute(1.0), -edge);
}

constexpr int host_length = 3;
constexpr std::array<double, 8> closes{3, 2, 1, 1, 1, 2, 3, 4};

class RankStrategy final : public source::PineStrategyHost {
public:
    RankStrategy() {
        attach_pine_execution_adapter();
        configure_pine_strategy(source::PineStrategyConfig{});
    }

    void on_source_bar(const Bar& bar) override {
        const double value = rci_.compute(bar.close);
        const bool crossed = crossover_.compute(value, 0.0);
        if (seen < closes.size()) {
            values[seen] = value;
            crosses[seen] = crossed;
        }
        if (!is_na(value) && crossed) strategy_entry("cross", true);
        if (seen == 6) strategy_close("cross");
        ++seen;
    }

    std::size_t seen = 0;
    std::array<double, closes.size()> values{};
    std::array<bool, closes.size()> crosses{};

private:
    ta::RCI rci_{host_length};
    ta::Crossover crossover_;
};

void check_strategy() {
    std::array<Bar, closes.size()> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double price = closes[i];
        bars[i] = {price, price, price, price, 1, static_cast<std::int64_t>(i + 1) * 60000};
    }
    RankStrategy host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!check(host_length, "host consumed all bars",
               host.last_run_status() == 0 && host.seen == bars.size())) {
        std::printf("  status=%d bars=%zu\n", host.last_run_status(), host.seen);
        std::puts("NOT COMPARED length=3 host signals and fills: incomplete run");
        return;
    }

    // The negative ranks before bar 4 carry across the missing result.
    // The first positive value is on bar 5, so the entry fills at open 6.
    expect(host_length, "host strictly falling ranks", host.values[2], -100.0);
    expect(host_length, "host flat value", host.values[4], na<double>());
    check(host_length, "host no crossover on flat", !host.crosses[4]);
    check(host_length, "host crossover after flat", host.crosses[5]);

    if (!check(host_length, "host one closed trade", host.trade_count() == 1)) {
        std::printf("  closed trades=%d\n", host.trade_count());
        std::puts("NOT COMPARED length=3 host entry and exit: no unique closed trade");
        return;
    }
    const auto trade = host.get_trade(0);
    if (!check(host_length, "host entry after flat",
               trade.entry_time == bars[6].timestamp && trade.entry_price == bars[6].open)) {
        std::printf("  entry time=%lld price=%.17g\n",
                    static_cast<long long>(trade.entry_time), trade.entry_price);
    }
    check(host_length, "host scheduled exit",
          trade.is_long && trade.qty == 1.0 &&
          trade.exit_time == bars[7].timestamp && trade.exit_price == bars[7].open);
}

} // namespace

int main() {
    for (int length : std::array<int, 3>{2, 3, 14}) check_windows(length);
    check_strategy();
    return failures != 0;
}
