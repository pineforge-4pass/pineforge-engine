// A reversing entry opens its own quantity after a partial or whole close.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL  [%s] %s:%d  %s\n", scenario, __FILE__, line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kT = 1700000000000LL;
constexpr int kOldEntryBar = 1;
constexpr int kSignalBar = 3;
constexpr int kEntryHeldBar = kSignalBar + 1;
constexpr int kTpBar = kEntryHeldBar + 1;
constexpr int kStopBar = kTpBar + 2;
constexpr double kTpPercent = 50.0;
constexpr double kTpDistance = 2.0;
constexpr double kStopDistance = 3.0;

struct Scenario {
    const char* name;
    double old_qty;
    double close_qty;
    double new_qty;
    bool new_is_long;

    double direction() const { return new_is_long ? 1.0 : -1.0; }
    bool old_is_long() const { return !new_is_long; }
    std::string old_id() const { return new_is_long ? "Short" : "Long"; }
    std::string new_id() const { return new_is_long ? "Long" : "Short"; }
};

Bar mk(int index, double o, double h, double l, double c) {
    Bar bar;
    bar.open = o; bar.high = h; bar.low = l; bar.close = c;
    bar.volume = 1.0;
    bar.timestamp = kT + static_cast<std::int64_t>(index) * 60000;
    return bar;
}

std::vector<Bar> feed(const Scenario& spec) {
    // Synthetic prices are binary-exact on the price and quantity grids.
    std::vector<Bar> bars = {
        mk(0,  99.0,  99.0,   99.0,  99.0),
        mk(1,  99.0,  99.0,   99.0,  99.0),
        mk(2, 100.0, 100.0,  100.0, 100.0),
        mk(3, 100.0, 100.0,  100.0, 100.0),
        mk(4, 100.0, 101.0,   99.0, 100.0),
        mk(5, 100.0, 103.0,   99.5, 102.5),
        mk(6, 102.5, 102.75, 100.0, 100.5),
        mk(7, 100.5, 100.75,  96.5,  97.5),
        mk(8,  97.5,  97.5,   97.5,  97.5),
    };
    if (!spec.new_is_long) {
        const double reflection = bars[kSignalBar].close * 2.0;
        for (auto& bar : bars) {
            const double high = bar.high;
            bar.open = reflection - bar.open;
            bar.high = reflection - bar.low;
            bar.low = reflection - high;
            bar.close = reflection - bar.close;
        }
    }
    return bars;
}

class PartialCloseProbe final : public source::PineStrategyHost {
public:
    explicit PartialCloseProbe(const Scenario& spec) : spec_(spec) {
        source::PineStrategyConfig config;
        config.process_orders_on_close = true;
        config.initial_capital = 100000.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.calc_on_order_fills = false;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.close_entries_rule_any = false;
        config.pyramiding = 0;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("qty_step", 0.25);
        margin_call_enabled_ = false;
    }

    double signed_units() const noexcept {
        return position_qty_ * (position_side_ == PositionSide::SHORT ? -1.0 : 1.0);
    }

    double units_at_signal = kNaN;
    double units_after_entry = kNaN;
    double units_after_tp = kNaN;
    double units_after_stop = kNaN;

    void on_source_bar(const Bar&) override {
        const int bar = bar_index_;
        if (bar == kOldEntryBar) {
            strategy_entry(spec_.old_id(), spec_.old_is_long(),
                           kNaN, kNaN, spec_.old_qty, "", "", 0, -1);
        }
        if (bar == kSignalBar) {
            units_at_signal = signed_units();
            strategy_close(spec_.old_id(), "", spec_.close_qty, kNaN,
                           false, 30064771091ULL);
            strategy_entry(spec_.new_id(), spec_.new_is_long,
                           kNaN, kNaN, spec_.new_qty, "", "", 0, -1);
            const double limit = current_bar_.close + spec_.direction() * kTpDistance;
            const double stop = current_bar_.close - spec_.direction() * kStopDistance;
            strategy_exit(std::string("TP"), spec_.new_id(), limit, stop,
                          kNaN, kNaN, kNaN, kTpPercent, "", kNaN, "", kNaN, kNaN);
            strategy_exit(std::string("SL"), spec_.new_id(), kNaN, stop,
                          kNaN, kNaN, kNaN, 100.0, "", kNaN, "", kNaN, kNaN);
        }
        if (bar == kEntryHeldBar) units_after_entry = signed_units();
        if (bar == kTpBar) units_after_tp = signed_units();
        if (bar == kStopBar) units_after_stop = signed_units();
    }

private:
    const Scenario& spec_;
};

struct Row {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
    int open_at_end;
};

struct ExpectedTrade {
    Row row;
    std::string entry_id;
    std::string exit_id;
};

struct Observed {
    std::vector<Row> rows;
    std::vector<Trade> closed_trades;
    std::string error;
    double units_at_signal;
    double units_after_entry;
    double units_after_tp;
    double units_after_stop;
    double final_units;
};

Observed observe(const Scenario& spec, const std::vector<Bar>& bars) {
    PartialCloseProbe probe(spec);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    Observed out;
    out.error = probe.last_error();
    out.units_at_signal = probe.units_at_signal;
    out.units_after_entry = probe.units_after_entry;
    out.units_after_tp = probe.units_after_tp;
    out.units_after_stop = probe.units_after_stop;
    out.final_units = probe.signed_units();
    for (std::size_t i = 0; i < probe.closed_trade_count(); ++i)
        out.closed_trades.push_back(probe.get_trade(static_cast<int>(i)));
    ReportC report{};
    probe.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                            t.qty, t.pnl, t.is_long, t.open_at_end});
    }
    BacktestEngine::free_report(&report);
    return out;
}

Row expected_row(const std::vector<Bar>& bars, int entry_bar, int exit_bar,
                 double exit_price, double qty, bool is_long) {
    const double entry_price = bars[entry_bar].close;
    return {bars[entry_bar].timestamp, bars[exit_bar].timestamp,
            entry_price, exit_price, qty,
            (is_long ? 1.0 : -1.0) * (exit_price - entry_price) * qty,
            is_long ? 1 : 0, 0};
}

void compare(const Row& got, const Row& expected) {
    CHECK(got.entry_time == expected.entry_time);
    CHECK(got.exit_time == expected.exit_time);
    CHECK(got.entry_price == expected.entry_price);
    CHECK(got.exit_price == expected.exit_price);
    CHECK(got.qty == expected.qty);
    CHECK(got.pnl == expected.pnl);
    CHECK(got.is_long == expected.is_long);
    CHECK(got.open_at_end == expected.open_at_end);
}

void check_exits(const Scenario& spec, const std::vector<Bar>& bars, const Observed& got) {
    CHECK(got.error.empty());
    const double close_qty = std::isfinite(spec.close_qty)
        ? std::min(spec.old_qty, spec.close_qty) : spec.old_qty;
    const double old_remainder = spec.old_qty - close_qty;
    const double tp_qty = spec.new_qty * kTpPercent / 100.0;
    const double sl_qty = spec.new_qty - tp_qty;
    const double remaining = spec.new_qty - tp_qty - sl_qty;
    CHECK(got.units_at_signal == -spec.direction() * spec.old_qty);
    CHECK(got.units_after_entry == spec.direction() * spec.new_qty);
    CHECK(got.units_after_tp == spec.direction() * (spec.new_qty - tp_qty));
    CHECK(got.units_after_stop == spec.direction() * remaining);
    CHECK(got.final_units == spec.direction() * remaining);

    std::vector<ExpectedTrade> closed;
    if (close_qty > 0.0) {
        closed.push_back({expected_row(bars, kOldEntryBar, kSignalBar,
                                      bars[kSignalBar].close, close_qty, spec.old_is_long()),
                          spec.old_id(), "__close__" + spec.old_id()});
    }
    if (old_remainder > 0.0) {
        closed.push_back({expected_row(bars, kOldEntryBar, kSignalBar,
                                      bars[kSignalBar].close, old_remainder,
                                      spec.old_is_long()),
                          spec.old_id(), spec.new_id()});
    }
    if (tp_qty > 0.0) {
        closed.push_back({expected_row(bars, kSignalBar, kTpBar,
                                      bars[kSignalBar].close + spec.direction() * kTpDistance,
                                      tp_qty, spec.new_is_long),
                          spec.new_id(), "TP"});
    }
    if (sl_qty > 0.0) {
        closed.push_back({expected_row(bars, kSignalBar, kStopBar,
                                      bars[kSignalBar].close - spec.direction() * kStopDistance,
                                      sl_qty, spec.new_is_long),
                          spec.new_id(), "SL"});
    }
    CHECK(got.closed_trades.size() == closed.size());
    for (std::size_t i = 0; i < std::min(got.closed_trades.size(), closed.size()); ++i) {
        const auto& trade = got.closed_trades[i];
        CHECK(trade.entry_id == closed[i].entry_id);
        CHECK(trade.exit_id == closed[i].exit_id);
        compare({trade.entry_time, trade.exit_time, trade.entry_price, trade.exit_price,
                 trade.qty, trade.pnl, trade.is_long, trade.open_at_end}, closed[i].row);
    }
    CHECK(got.rows.size() == closed.size());
    for (std::size_t i = 0; i < std::min(got.rows.size(), closed.size()); ++i)
        compare(got.rows[i], closed[i].row);
}

void dump(const Observed& got) {
    for (const auto& t : got.closed_trades) {
        std::printf("  OBSERVED [%s] closed entry_id='%s' entry_time=%lld "
                    "entry_price=%.4f exit_id='%s' exit_time=%lld exit_price=%.4f "
                    "qty=%.4f pnl=%.4f side=%s\n",
                    scenario, t.entry_id.c_str(), static_cast<long long>(t.entry_time),
                    t.entry_price, t.exit_id.c_str(), static_cast<long long>(t.exit_time),
                    t.exit_price, t.qty, t.pnl, t.is_long ? "long" : "short");
    }
    for (const auto& row : got.rows) {
        std::printf("  OBSERVED [%s] report entry_time=%lld exit_time=%lld "
                    "entry_price=%.4f exit_price=%.4f qty=%.4f pnl=%.4f "
                    "side=%s open_at_end=%d\n",
                    scenario, static_cast<long long>(row.entry_time),
                    static_cast<long long>(row.exit_time), row.entry_price, row.exit_price,
                    row.qty, row.pnl, row.is_long ? "long" : "short", row.open_at_end);
    }
    std::printf("  OBSERVED [%s] units at_signal=%.4f after_entry=%.4f after_tp=%.4f "
                "after_stop=%.4f final=%.4f error='%s'\n",
                scenario, got.units_at_signal, got.units_after_entry, got.units_after_tp,
                got.units_after_stop, got.final_units, got.error.c_str());
}

void run_case(const Scenario& spec) {
    scenario = spec.name;
    const auto bars = feed(spec);
    const auto got = observe(spec, bars);
    const int before = failures;
    check_exits(spec, bars, got);
    if (failures > before) dump(got);
}

}  // namespace

int main() {
    // TradingView tape sha256:c3e77e4e697fe85e5ba7d0e27bd1a14e47e569ed1c0a49f84cf29e68d045af1e
    run_case({"partial-close-to-short", 3.0, 1.0, 2.0, false});
    // By symmetry.
    run_case({"partial-close-to-long", 3.0, 1.0, 2.0, true});
    // Unchanged shape.
    run_case({"explicit-whole-close-to-short", 3.0, 3.0, 2.0, false});
    // Unchanged shape.
    run_case({"default-whole-close-to-short", 3.0, kNaN, 2.0, false});
    std::printf("test_adapter_partial_close_reversal: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
