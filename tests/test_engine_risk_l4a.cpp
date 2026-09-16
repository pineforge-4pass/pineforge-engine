#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// Native-route risk twin.  The legacy test primed protected risk and broker
// fields directly.  This replacement drives each policy through the generated
// source commands and reads only public trades, native position, and receipts.

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

constexpr std::int64_t kDay = 86'400'000;
constexpr std::int64_t kStart = 1'743'379'200'000LL;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(expr) do {                                                       \
    if (expr) ++passed; else {                                                 \
        ++failed; std::printf("FAIL %d %s\n", __LINE__, #expr);               \
    }                                                                          \
} while (0)

bool near(double actual, double expected, double tolerance = 1e-8) {
    return std::abs(actual - expected) <= tolerance;
}

Bar bar(std::int64_t timestamp, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, timestamp};
}

class RiskHost : public source::PineStrategyHost {
public:
    const Trade& row(int index) const { return get_trade(index); }
    double position() const { return physical_position().signed_units; }
    double average() const { return physical_position().average_price; }
    std::size_t lots() const { return physical_position().lot_count; }
};

class DirectionHost final : public RiskHost {
public:
    explicit DirectionHost(int direction) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        process_orders_on_close_ = true;
        pyramiding_ = 4;
        set_pine_risk_direction(direction);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) {
            strategy_entry("Long", true, kNaN, kNaN, 1.0);
            strategy_entry("Short", false, kNaN, kNaN, 1.0);
        }
        if (index == 1) strategy_close_all();
    }
};

void test_direction_locks_are_source_command_gates() {
    const Bar tape[] = {
        bar(kStart, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + 60'000, 100.0, 100.0, 100.0, 100.0),
    };
    DirectionHost long_only(+1);
    long_only.run(tape, 2);
    CHECK(long_only.last_error().empty());
    CHECK(long_only.trade_count() == 1);
    CHECK(long_only.row(0).entry_id == "Long");
    CHECK(long_only.row(0).is_long);
    CHECK(near(long_only.row(0).qty, 1.0));
    CHECK(near(long_only.position(), 0.0));

    DirectionHost short_only(-1);
    short_only.run(tape, 2);
    CHECK(short_only.last_error().empty());
    CHECK(short_only.trade_count() == 1);
    CHECK(short_only.row(0).entry_id == "Short");
    CHECK(!short_only.row(0).is_long);
    CHECK(near(short_only.row(0).qty, 1.0));
    CHECK(near(short_only.position(), 0.0));
}

class DrawdownHost final : public RiskHost {
public:
    DrawdownHost() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        process_orders_on_close_ = true;
        pyramiding_ = 8;
        set_pine_risk_max_drawdown(5.0, false);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index <= 3) strategy_entry("D" + std::to_string(index), true, kNaN, kNaN, 1.0);
        if (index == 4) strategy_close_all();
    }
};

void test_drawdown_latches_and_blocks_later_commands() {
    const Bar tape[] = {
        bar(kStart, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + 60'000, 110.0, 110.0, 90.0, 90.0),
        bar(kStart + 120'000, 80.0, 80.0, 70.0, 75.0),
        bar(kStart + 180'000, 70.0, 70.0, 60.0, 65.0),
        bar(kStart + 240'000, 60.0, 60.0, 60.0, 60.0),
    };
    DrawdownHost host;
    host.run(tape, 5);
    CHECK(host.last_error().empty());
    // D0 is the only accepted opening. D1--D3 are refused after the
    // close-mark drawdown latches, but the latch gates entries only:
    // pine_risk.cpp:111-118 never refused the later close_all.
    CHECK(host.trade_count() == 1);
    CHECK(near(host.position(), 0.0));
    CHECK(host.lots() == 0);
    CHECK(near(host.average(), 0.0));
}

class IntradayLossHost final : public RiskHost {
public:
    IntradayLossHost() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        process_orders_on_close_ = true;
        pyramiding_ = 8;
        set_margin_call_enabled(false);
        set_pine_risk_max_intraday_loss(5.0, false);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) strategy_entry("first", true, kNaN, kNaN, 1.0);
        if (index == 2) strategy_entry("blocked", true, kNaN, kNaN, 1.0);
        if (index == 3) strategy_entry("tomorrow", true, kNaN, kNaN, 1.0);
        if (index == 4) strategy_close_all();
    }
};

void test_intraday_loss_closes_at_path_extreme_and_rolls_over() {
    const Bar tape[] = {
        bar(kStart, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + 60'000, 100.0, 101.0, 90.0, 92.0),
        bar(kStart + 120'000, 92.0, 94.0, 91.0, 93.0),
        bar(kStart + kDay, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + kDay + 60'000, 100.0, 100.0, 100.0, 100.0),
    };
    IntradayLossHost host;
    host.run(tape, 5);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    CHECK(host.row(0).entry_id == "first");
    CHECK(host.row(0).exit_comment == "Close Position (Max intraday Loss)");
    CHECK(near(host.row(0).entry_price, 100.0));
    CHECK(near(host.row(0).exit_price, 90.0));
    CHECK(host.row(0).exit_time == kStart + 60'000);
    CHECK(host.row(1).entry_id == "tomorrow");
    CHECK(host.row(1).exit_comment.empty());
    CHECK(near(host.row(1).qty, 1.0));
    CHECK(near(host.position(), 0.0));
}

class ConsecutiveLossHost final : public RiskHost {
public:
    ConsecutiveLossHost() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        process_orders_on_close_ = true;
        pyramiding_ = 4;
        set_margin_call_enabled(false);
        set_pine_risk_max_cons_loss_days(2);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0 || index == 2 || index == 4)
            strategy_entry("loss" + std::to_string(index), true, kNaN, kNaN, 1.0);
        if (index == 1 || index == 3 || index == 5) strategy_close_all();
    }
};

void test_consecutive_loss_latch_counts_one_row_per_chart_day() {
    const Bar tape[] = {
        bar(kStart, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + 60'000, 90.0, 90.0, 90.0, 90.0),
        bar(kStart + kDay, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + kDay + 60'000, 90.0, 90.0, 90.0, 90.0),
        bar(kStart + 2 * kDay, 100.0, 100.0, 100.0, 100.0),
        bar(kStart + 2 * kDay + 60'000, 90.0, 90.0, 90.0, 90.0),
    };
    ConsecutiveLossHost host;
    host.run(tape, 6);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    CHECK(host.row(0).entry_id == "loss0");
    CHECK(host.row(1).entry_id == "loss2");
    CHECK(host.row(0).pnl < 0.0);
    CHECK(host.row(1).pnl < 0.0);
    CHECK(near(host.position(), 0.0));
    CHECK(host.lots() == 0);
}

}  // namespace

int main() {
    test_direction_locks_are_source_command_gates();
    test_drawdown_latches_and_blocks_later_commands();
    test_intraday_loss_closes_at_path_extreme_and_rolls_over();
    test_consecutive_loss_latch_counts_one_row_per_chart_day();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
