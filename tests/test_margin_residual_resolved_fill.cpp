#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace pineforge;

namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #value); } } while (0)
bool near(double actual, double expected, double tolerance = 1e-8) {
    return std::abs(actual - expected) < tolerance;
}
}

class CentFloorHost final : public pineforge::source::PineStrategyHost {
public:
    explicit CentFloorHost(double capital) {
        pineforge::source::PineStrategyConfig configuration;
        configuration.initial_capital = capital;
        configuration.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        configuration.default_qty_value = 100.0;
        configuration.margin_long = configuration.margin_short = 100.0;
        configure_pine_strategy(configuration);
        set_syminfo_mintick(0.00001);
        set_syminfo_metadata("qty_step", 0.01);
    }
    void on_source_bar(const Bar& bar) override {
        const double missing = std::numeric_limits<double>::quiet_NaN();
        if (bar.timestamp == 1743623100000LL)
            strategy_entry("test", false, missing, missing, missing);
        if (bar.timestamp == 1743624900000LL) strategy_close_all();
    }
};

static void test_adapter_raw_cent_floor() {
    const Bar bars[] = {
        {1.08521, 1.08562, 1.08469, 1.085, 1516, 1743623100000LL},
        {1.08496, 1.08989, 1.08329, 1.0886, 5183, 1743624000000LL},
        {1.0886, 1.09244, 1.08152, 1.08537, 18230, 1743624900000LL},
        {1.08536, 1.08579, 1.08107, 1.08184, 10323, 1743625800000LL},
    };
    const double capitals[] = {996097.5955029, 996097.5954, 996097.5956,
                               996097.5957, 996097.596};
    const double expected_units[] = {918062.29, 918062.29, 0.0, 0.0, 918062.30};
    const double expected_calls[] = {33087.2, 33087.2, 0.0, 0.0, 33087.24};
    for (std::size_t index = 0; index < 5; ++index) {
        CentFloorHost host(capitals[index]);
        host.run(bars, 4, "15", "15");
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == (expected_units[index] > 0.0 ? 2 : 0));
        double opened_units = 0.0;
        for (int trade_index = 0; trade_index < host.trade_count(); ++trade_index) {
            const auto trade = host.get_trade(trade_index);
            opened_units += trade.qty;
            CHECK(near(trade.entry_price, 1.08496, 1e-10));
            CHECK(trade.entry_time == 1743624000000LL);
            if (trade_index == 0) {
                CHECK(trade.exit_id == "__margin_call__");
                CHECK(near(trade.qty, expected_calls[index], 1e-7));
                CHECK(near(trade.exit_price, 1.08989, 1e-10));
                CHECK(trade.exit_time == 1743624000000LL);
            } else {
                CHECK(near(trade.exit_price, 1.08536, 1e-10));
                CHECK(trade.exit_time == 1743625800000LL);
            }
        }
        CHECK(near(opened_units, expected_units[index], 1e-7));
    }
}

class RepeatedSlippedShort final : public pineforge::source::PineStrategyHost {
public:
    explicit RepeatedSlippedShort(double capital, int slippage = 2) {
        pineforge::source::PineStrategyConfig configuration;
        configuration.initial_capital = capital;
        configuration.margin_long = configuration.margin_short = 100.0;
        configuration.commission_type = static_cast<int>(CommissionType::PERCENT);
        configuration.commission_value = 0.05;
        configuration.slippage = slippage;
        configuration.process_orders_on_close = true;
        configure_pine_strategy(configuration);
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", 0.0001);
    }
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == 1744119900000LL)
            strategy_entry("test", false, qnan, qnan, 6.7883);
        if (bar.timestamp == 1744122600000LL) strategy_close_all();
    }
};

void test_repeated_call_reuses_resolved_fill() {
    const Bar tape[] = {
        {1566.28, 1573.87, 1557.15, 1567.65, 169548.287, 1744119900000LL},
        {1567.67, 1582.74, 1563.87, 1574.00, 138808.738, 1744120800000LL},
        {1574.00, 1578.58, 1556.15, 1556.70, 139374.183, 1744121700000LL},
        {1556.71, 1561.78, 1544.56, 1546.35, 186983.945, 1744122600000LL},
    };
    const double capitals[] = {10646.935793, 10646.9358, 10647.1065643645,
                               10645.2, 10648.2};
    const double calls[][2] = {{0.0004, 1.0}, {0.0004, 1.0}, {1.0, 0.0},
                               {0.0048, 0.5032}, {0.5148, 0.0}};
    for (std::size_t index = 0; index < 5; ++index) {
        RepeatedSlippedShort host(capitals[index]);
        host.run(tape, 4, "15", "15");
        CHECK(host.last_error().empty());
        const int call_count = calls[index][1] > 0.0 ? 2 : 1;
        CHECK(host.trade_count() == call_count + 1);
        if (host.trade_count() != call_count + 1) continue;
        for (int call_index = 0; call_index < call_count; ++call_index) {
            const auto trade = host.get_trade(call_index);
            const double fill = index == 4 || (index == 3 && call_index == 1)
                ? 1582.76 : 1567.69;
            CHECK(trade.exit_id == "__margin_call__");
            CHECK(near(trade.qty, calls[index][call_index]));
            CHECK(near(trade.entry_price, 1567.63));
            CHECK(near(trade.exit_price, fill));
            CHECK(trade.entry_time == 1744119900000LL);
            CHECK(trade.exit_time == 1744120800000LL);
            const double pnl = trade.qty * (1567.63 - fill)
                - trade.qty * (1567.63 + fill) * 0.0005;
            CHECK(near(trade.pnl, pnl));
        }
        const auto survivor = host.get_trade(call_count);
        CHECK(near(survivor.qty, 6.7883 - calls[index][0] - calls[index][1]));
        CHECK(near(survivor.exit_price, 1546.37));
        CHECK(survivor.exit_time == 1744122600000LL);
    }
}

void test_repeated_call_slippage_controls() {
    const Bar tape[] = {
        {1566.28, 1573.87, 1557.15, 1567.65, 169548.287, 1744119900000LL},
        {1567.67, 1582.74, 1563.87, 1574.00, 138808.738, 1744120800000LL},
        {1574.00, 1578.58, 1556.15, 1556.70, 139374.183, 1744121700000LL},
        {1556.71, 1561.78, 1544.56, 1546.35, 186983.945, 1744122600000LL},
    };
    const int slips[] = {0, 1, 3};
    const double calls[][3] = {{0.0004, 0.5164, 0.0}, {0.0004, 0.5164, 0.0},
                               {0.0004, 0.0004, 0.5148}};
    for (std::size_t index = 0; index < 3; ++index) {
        const int slip = slips[index];
        const double capital = 10646.935793 + 6.7883 * (slip - 2) * 0.01 * 0.9995;
        RepeatedSlippedShort host(capital, slip);
        host.run(tape, 4, "15", "15");
        CHECK(host.last_error().empty());
        const int call_count = index == 2 ? 3 : 2;
        CHECK(host.trade_count() == call_count + 1);
        if (host.trade_count() != call_count + 1) continue;
        double called_units = 0.0;
        for (int call_index = 0; call_index < call_count; ++call_index) {
            const auto trade = host.get_trade(call_index);
            const double mark = call_index == call_count - 1 ? 1582.74 : 1567.67;
            CHECK(trade.exit_id == "__margin_call__");
            CHECK(near(trade.qty, calls[index][call_index]));
            CHECK(near(trade.entry_price, 1567.65 - slip * 0.01));
            CHECK(near(trade.exit_price, mark + slip * 0.01));
            CHECK(trade.exit_time == 1744120800000LL);
            called_units += trade.qty;
        }
        const auto survivor = host.get_trade(call_count);
        CHECK(near(survivor.qty, 6.7883 - called_units));
        CHECK(near(survivor.exit_price, 1546.35 + slip * 0.01));
    }
}

int main() {
    test_adapter_raw_cent_floor();
    test_repeated_call_reuses_resolved_fill();
    test_repeated_call_slippage_controls();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
