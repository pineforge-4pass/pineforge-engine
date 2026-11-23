#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {
using namespace pineforge;

constexpr int N = 133;
constexpr double entry_price = 100.0;
constexpr double win_price = 112.5;
constexpr double loss_price = 93.75;
constexpr double win_percent = 12.5;
constexpr double loss_percent = -6.25;

bool compared(const char* reason, double actual, double expected) {
    if (actual == expected) return true;
    std::fprintf(stderr, "NOT COMPARED: %s: actual=%.17g expected=%.17g\n",
                 reason, actual, expected);
    return false;
}

class Strategy final : public source::PineStrategyHost {
public:
    Strategy() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        configure_pine_strategy(source::PineStrategyConfig{});
    }

    void on_source_bar(const Bar&) override {
        if (!input_ok) return;
        const int bar = callbacks++;
        const int closed = bar / 2;
        input_ok = compared("unsealed ledger base", trades_.base(), 0)
            && compared("script bar index", pine_bar_index(), bar)
            && compared("closed trades at script callback", closed_trade_count(), closed);
        if (!input_ok) return;
        if (bar > 0 && bar % 2 == 0) {
            const auto row = trades_.numeric_at(static_cast<std::size_t>(closed - 1));
            const bool win = closed % 2 == 1;
            input_ok = compared("entry price", row.entry_price, entry_price)
                && compared("exit price", row.exit_price, win ? win_price : loss_price)
                && compared("closed trade percentage", row.pnl_pct,
                            win ? win_percent : loss_percent);
            if (!input_ok) return;
        }

        // The issue's Pine script: alternate a market entry and a named
        // market close, then read the average on every bar.
        if (signed_position_size() == 0.0) strategy_entry("L", true);
        else strategy_close("L");

        const auto before = trades_.read_cache_stats();
        const double all = avg_trade_percent();
        const double winning = avg_winning_trade_percent();
        const double losing = avg_losing_trade_percent();
        const auto after = trades_.read_cache_stats();
        row_reads += (after.hot_hit_count - before.hot_hit_count)
            + (after.cold_miss_count - before.cold_miss_count);
        cache_probes += after.lookup_probe_count - before.lookup_probe_count;
        descriptor_reads += after.descriptor_read_count - before.descriptor_read_count;
        if (all > 1000.0) strategy_close_all();

        const int wins = (closed + 1) / 2;
        const int losses = closed / 2;
        const double expected = closed ? (wins * win_percent + losses * loss_percent) / closed : 0.0;
        if (all != expected || winning != (wins ? win_percent : 0.0)
                            || losing != (losses ? loss_percent : 0.0)) {
            std::fprintf(stderr, "FAIL bar %d: averages %.17g, %.17g, %.17g; expected %.17g, %.17g, %.17g\n",
                         bar, all, winning, losing, expected,
                         wins ? win_percent : 0.0, losses ? loss_percent : 0.0);
            values_ok = false;
        }
    }

    int callbacks = 0;
    bool input_ok = true;
    bool values_ok = true;
    std::uint64_t row_reads = 0;
    std::uint64_t cache_probes = 0;
    std::uint64_t descriptor_reads = 0;
};

bool check(int count, std::string_view mode) {
    std::vector<Bar> bars;
    if (mode != "--empty") {
        for (int i = 0; i < count; ++i) {
            double price = entry_price;
            if (i > 0 && i % 2 == 0) price = i % 4 == 2 ? win_price : loss_price;
            if (mode == "--wrong-input" && i == 2) price = 125.0;
            bars.push_back({price, price, price, price, 1.0,
                            INT64_C(1736121600000) + INT64_C(60000) * i});
        }
    }
    if (!compared("input bar count", bars.size(), count)) return false;

    Strategy strategy;
    strategy.run(bars.data(), count, "1", "1");
    if (!compared("run status", strategy.last_run_status(), 0)) {
        std::fprintf(stderr, "NOT COMPARED: engine error: %s\n", strategy.last_error().c_str());
        return false;
    }
    if (!strategy.input_ok
        || !compared("script callback count", strategy.callbacks, count)
        || !compared("final closed trade count", strategy.closed_trade_count(), (count - 1) / 2))
        return false;

    if (strategy.row_reads != 0 || strategy.cache_probes != 0 || strategy.descriptor_reads != 0) {
        std::fprintf(stderr, "%s:%d: FAIL %d bars: average getters read %llu ledger rows "
                     "(%llu cache probes, %llu backing descriptors); expected 0 for all three\n",
                     __FILE__, __LINE__, count,
                     static_cast<unsigned long long>(strategy.row_reads),
                     static_cast<unsigned long long>(strategy.cache_probes),
                     static_cast<unsigned long long>(strategy.descriptor_reads));
        return false;
    }
    return strategy.values_ok;
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? argv[1] : "";
    if (argc != 1) {
        if (argc == 2 && (mode == "--empty" || mode == "--wrong-input"))
            return check(N, mode) ? 0 : 1;
        std::fprintf(stderr, "NOT COMPARED: invalid arguments: actual argument count=%d expected=0 or one known mode\n",
                     argc - 1);
        return 1;
    }
    const bool small = check(N, mode);
    const bool large = check(2 * N, mode);
    if (!small || !large) return 1;
    std::puts("PASS: 133 and 266 bars; all three averages matched on every bar with zero ledger reads");
    return 0;
}
