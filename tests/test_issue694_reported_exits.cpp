// #694 cases 1 and 3: use the same public calls as the reported Pine shapes.
// The price-only bracket is the reporter's one-change control. No private
// fixture, codegen shim or changed engine API is needed on the main tree.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using pineforge::Bar;
using pineforge::TradeC;
constexpr double absent = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t first = 1'704'067'200'000;
constexpr std::int64_t hour = 3'600'000;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
Bar bar(int i, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, first + i * hour};
}

class IssueHost : public pineforge::source::PineStrategyHost {
public:
    IssueHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }
};

class EntryBracket : public IssueHost {
public:
    explicit EntryBracket(bool price_only) : price_only_(price_only) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("Long", true, absent, absent, 1.0);
        // profit=1000 ticks and stop=95 are independent legs. The absolute
        // stop must protect the market entry at its next-open fill of100.
        strategy_exit("Long exit", "Long", price_only_ ? 110.0 : absent, 95.0,
                      absent, absent, absent, 100.0, "", absent, "",
                      price_only_ ? absent : 1000.0, absent);
    }
private:
    bool price_only_;
};

class PyramidTicks : public IssueHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0 || pine_bar_index() == 1)
            strategy_entry("Long", true, absent, absent, 1.0);
        strategy_exit("Long exit", "Long", absent, absent, absent, absent,
                      absent, 100.0, "", absent, "", 1000.0, 1000.0);
    }
};

std::vector<TradeC> run(IssueHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    require(host.last_error().empty(), "reported-shape run returned an engine error");
    pineforge::ReportC report{};
    host.fill_report(&report);
    std::vector<TradeC> trades;
    for (int i = 0; i < report.trades_len; ++i) {
        // Only numeric fields below are used after report-owned text is freed.
        trades.push_back(report.trades[i]);
        const auto& t = report.trades[i];
        std::printf("trade %d: %lld -> %lld, %.17g -> %.17g, qty %.17g, open %d\n",
            i, static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time),
            t.entry_price, t.exit_price, t.qty, static_cast<int>(t.open_at_end));
    }
    pineforge::BacktestEngine::free_report(&report);
    return trades;
}

void check_entry_bracket(bool price_only) {
    EntryBracket host(price_only);
    const auto trades = run(host, {bar(0,100,101,99,100),
                                  bar(1,100,101,94,100),
                                  bar(2,100,101,99,100)});
    require(trades.size() == 1, "one entry must produce one observed trade");
    const auto& trade = trades.front();
    require(!trade.open_at_end, "entry crossed its absolute stop but remained open");
    require(trade.entry_time == first + hour, "market entry must fill on next open");
    require(trade.exit_time == trade.entry_time, "absolute stop must be active on entry bar");
    require(near(trade.entry_price,100.0) && near(trade.exit_price,95.0),
            "absolute stop leg must fill at95 rather than wait for relative profit");
}

void test_mixed_tick_profit_absolute_stop_arms_at_entry_fill() {
    check_entry_bracket(false);
}
void test_price_only_entry_bracket_control() {
    check_entry_bracket(true);
}
void test_pyramiding_tick_levels_use_entry_origins_with_fifo() {
    PyramidTicks host;
    const auto trades = run(host, {bar(0,100,101,99,100),bar(1,100,101,99,100),
                                  bar(2,104,105,103,104),bar(3,100,105,89,100)});
    require(trades.size() == 2, "two entries must have two separately observed exits");
    for (const auto& trade : trades) {
        require(!trade.open_at_end && trade.exit_time == first + 3*hour,
                "both entry-origin stops must fill on the crossing bar");
        require(near(trade.qty,1.0), "FIFO attribution must retain each lot quantity");
    }
    require(near(trades[0].entry_price,100.0) && near(trades[1].entry_price,104.0),
            "the two entries must keep distinct fill prices and FIFO order");
    const double tick_distance = 1000.0 * 0.01;
    // The104-origin stop at94 triggers first and settles the older lot FIFO;
    // the100-origin stop at90 then settles the remaining lot. Average92 is wrong.
    require(near(trades[0].exit_price,trades[1].entry_price-tick_distance),
            "first FIFO exit must use the second entry's actual stop level");
    require(near(trades[1].exit_price,trades[0].entry_price-tick_distance),
            "remaining exit must use the first entry's actual stop level");
}
} // namespace

int main() {
    try {
        test_price_only_entry_bracket_control();
        test_mixed_tick_profit_absolute_stop_arms_at_entry_fill();
        test_pyramiding_tick_levels_use_entry_origins_with_fifo();
        std::puts("#694 reported exit shapes passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"#694 reported exit shapes FAIL: %s\n",error.what());
        return 1;
    }
}
