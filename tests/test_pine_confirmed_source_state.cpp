#include <pineforge/pineforge.h>
#include <pineforge/source/pine_scheduler.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)

bool same(double expected, double actual) {
    std::uint64_t left = 0;
    std::uint64_t right = 0;
    std::memcpy(&left, &expected, sizeof(left));
    std::memcpy(&right, &actual, sizeof(right));
    return left == right;
}

Bar input(int index) {
    const double price = 100.0 + static_cast<double>(index % 7);
    return {price, price + 3.0, price - 2.0, price + 1.0, 1.0,
            1743465600000LL + static_cast<std::int64_t>(index) * 60000};
}

class CloseReport final : public source::PineStrategyHost {
public:
    explicit CloseReport(bool close) : close_(close) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        process_orders_on_close_ = true;
        margin_call_enabled_ = false;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("long", true);
        if (close_ && bar_index_ == 2) strategy_close("long");
    }
    double units() const { return signed_position_size(); }
    void clear_provider() { range_end_trades_.clear(); }
    double stored_profit() const { return equity_curve_.back().open_profit; }
private:
    bool close_;
};

class ConfirmedCap final : public source::PineStrategyHost {
public:
    ConfirmedCap() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        margin_call_enabled_ = false;
        set_pine_risk_max_intraday_filled_orders(1);
    }
    void on_source_bar(const Bar&) override { strategy_entry("cap", true); }
};

void confirmed_cap_prices() {
    std::vector<Bar> bars;
    for (int index = 0; index < 4320; ++index) bars.push_back(input(index));
    ConfirmedCap batch;
    batch.run(bars.data(), static_cast<int>(bars.size()), "1", "15");
    CHECK(batch.last_error().empty());
    ReportC expected{};
    batch.fill_report(&expected);
    CHECK(expected.trades_len > 0);
    for (const int split : {1440, 2880}) {
        ConfirmedCap stream;
        CHECK(stream.stream_begin(bars.data(), split, "1", "15"));
        for (int index = split; index < static_cast<int>(bars.size()); ++index)
            CHECK(stream.stream_push_bar(bars[index]));
        CHECK(stream.stream_end(false));
        ReportC actual{};
        CHECK(strategy_stream_fill_report(&stream, reinterpret_cast<pf_report_t*>(&actual)) == 0);
        CHECK(expected.trades_len == actual.trades_len);
        CHECK(same(expected.net_profit, actual.net_profit));
        for (int index = 0; index < expected.trades_len && index < actual.trades_len; ++index) {
            const auto& left = expected.trades[index];
            const auto& right = actual.trades[index];
            CHECK(left.entry_time == right.entry_time);
            CHECK(left.exit_time == right.exit_time);
            CHECK(same(left.qty, right.qty));
            CHECK(same(left.entry_price, right.entry_price));
            CHECK(same(left.exit_price, right.exit_price));
            CHECK(same(left.pnl, right.pnl));
        }
        BacktestEngine::free_report(&actual);
    }
    BacktestEngine::free_report(&expected);
}

void terminal_reports() {
    std::vector<Bar> bars;
    for (int index = 0; index < 3; ++index) bars.push_back(input(index));
    for (const bool close : {false, true}) {
        CloseReport strategy(close);
        CHECK(strategy.stream_begin(bars.data(), 1, "1", "1"));
        CHECK(strategy.stream_push_bar(bars[1]));
        CHECK(strategy.stream_push_bar(bars[2]));
        CHECK(strategy.stream_end(false));
        strategy.clear_provider();
        const auto before = strategy.stream_state_hash();
        const double stored = strategy.stored_profit();
        ReportC first{};
        ReportC second{};
        CHECK(strategy_stream_fill_report(&strategy, reinterpret_cast<pf_report_t*>(&first)) == 0);
        CHECK(strategy_stream_fill_report(&strategy, reinterpret_cast<pf_report_t*>(&second)) == 0);
        CHECK(before == strategy.stream_state_hash());
        CHECK(same(stored, strategy.stored_profit()));
        CHECK(first.equity_curve_len == 3);
        const auto& last = first.equity_curve[first.equity_curve_len - 1];
        CHECK(same(last.open_profit, second.equity_curve[second.equity_curve_len - 1].open_profit));
        if (close) {
            CHECK(strategy.units() == 0.0);
            CHECK(stored != 0.0);
            CHECK(last.open_profit == 0.0);
            CHECK(same(last.equity, 10000.0 + first.net_profit));
        } else {
            CHECK(strategy.units() == 1.0);
            CHECK(stored != 0.0);
            CHECK(same(last.open_profit, stored));
        }
        BacktestEngine::free_report(&first);
        BacktestEngine::free_report(&second);
    }
}

void confirmed_input_window() {
    source::PineScheduler scheduler;
    NativeBeginArgs begin{};
    begin.is_stream = true;
    scheduler.capture_begin(begin);
    for (int index = 0; index < 16; ++index) scheduler.retain_confirmed_input(input(index));
    CHECK(scheduler.confirmed_input_count() == 8);
    CHECK(!scheduler.confirmed_input_bar_at(input(0).timestamp));
    CHECK(!scheduler.confirmed_input_bar_at(input(15).timestamp + 1));
    for (int index = 8; index < 16; ++index) {
        const auto observed = scheduler.confirmed_input_bar_at(input(index).timestamp);
        CHECK(observed.has_value());
        CHECK(same(observed->close, input(index).close));
    }
    CloseReport host(false);
    NativeDecisionContext point{};
    point.sub_bar_open_ms = input(15).timestamp;
    const auto broker = scheduler.broker_bar(host, point);
    CHECK(broker.has_value());
    CHECK(same(broker->high, input(15).high));
    point.sub_bar_open_ms = input(15).timestamp + 1;
    CHECK(!scheduler.broker_bar(host, point));
    BrokerStateHashSink original;
    scheduler.hash_state(original);
    auto changed = input(15);
    changed.high += 0.5;
    scheduler.retain_confirmed_input(changed);
    CHECK(scheduler.confirmed_input_count() == 8);
    BrokerStateHashSink modified;
    scheduler.hash_state(modified);
    CHECK(original.h != modified.h);
    source::PineScheduler replay;
    replay.capture_begin(begin);
    for (int index = 0; index < 16; ++index) replay.retain_confirmed_input(input(index));
    BrokerStateHashSink repeated;
    replay.hash_state(repeated);
    CHECK(original.h == repeated.h);
    scheduler.capture_begin(begin);
    CHECK(scheduler.confirmed_input_count() == 0);
    CHECK(!scheduler.confirmed_input_bar_at(input(15).timestamp));
    begin.is_stream = false;
    scheduler.capture_begin(begin);
    BrokerStateHashSink empty;
    scheduler.hash_state(empty);
    scheduler.retain_confirmed_input(input(1));
    BrokerStateHashSink batch;
    scheduler.hash_state(batch);
    CHECK(empty.h == batch.h);
}

void confirmed_period_closure() {
    NativeInputContext context{};
    context.script_interval.open_ms = 1743465600000LL;
    context.script_interval.last_traded_close_ms = 1743552000000LL;
    context.input_interval.last_traded_close_ms = 1743551940000LL;
    CHECK(!source::PineScheduler::confirmed_script_interval_complete(context));
    context.input_interval.last_traded_close_ms = 1743552000000LL;
    CHECK(source::PineScheduler::confirmed_script_interval_complete(context));
    context.input_interval.last_traded_close_ms = 1743552060000LL;
    CHECK(source::PineScheduler::confirmed_script_interval_complete(context));
    context.script_interval.last_traded_close_ms = 1743512400000LL;
    context.input_interval.last_traded_close_ms = 1743512400000LL;
    CHECK(source::PineScheduler::confirmed_script_interval_complete(context));
    context.script_interval.last_traded_close_ms = context.script_interval.open_ms;
    CHECK(!source::PineScheduler::confirmed_script_interval_complete(context));
    context.script_interval.last_traded_close_ms = 0;
    CHECK(!source::PineScheduler::confirmed_script_interval_complete(context));
}

void confirmed_interval_open_anchors() {
    source::PineScheduler scheduler;
    NativeBeginArgs begin{};
    begin.is_stream = true;
    scheduler.capture_begin(begin);
    for (int index = 0; index < 2880; ++index) {
        const auto open = input((index / 1440) * 1440).timestamp;
        scheduler.retain_confirmed_input(input(index), open);
    }
    CHECK(scheduler.confirmed_input_count() == 8);
    CHECK(scheduler.confirmed_input_bar_at(input(0).timestamp).has_value());
    CHECK(scheduler.confirmed_input_bar_at(input(1440).timestamp).has_value());
    CHECK(!scheduler.confirmed_input_bar_at(input(1).timestamp));
    BrokerStateHashSink first;
    scheduler.hash_state(first);
    auto changed = input(0);
    changed.low -= 0.5;
    scheduler.retain_confirmed_input(changed, changed.timestamp);
    BrokerStateHashSink second;
    scheduler.hash_state(second);
    CHECK(first.h != second.h);
    for (int index = 2880; index < 2896; ++index)
        scheduler.retain_confirmed_input(input(index), input(2880).timestamp);
    CHECK(!scheduler.confirmed_input_bar_at(input(0).timestamp));
    CHECK(scheduler.confirmed_input_bar_at(input(1440).timestamp).has_value());
    CHECK(scheduler.confirmed_input_bar_at(input(2880).timestamp).has_value());
    CloseReport host(false);
    NativeDecisionContext point{};
    point.sub_bar_open_ms = input(1440).timestamp;
    const auto broker = scheduler.broker_bar(host, point);
    CHECK(broker.has_value());
    if (broker) CHECK(same(broker->high, input(1440).high));
    scheduler.capture_begin(begin);
    CHECK(!scheduler.confirmed_input_bar_at(input(1440).timestamp));
    begin.is_stream = false;
    scheduler.capture_begin(begin);
    BrokerStateHashSink empty;
    scheduler.hash_state(empty);
    scheduler.retain_confirmed_input(input(0), input(0).timestamp);
    BrokerStateHashSink batch;
    scheduler.hash_state(batch);
    CHECK(empty.h == batch.h);
}

}

int main() {
    confirmed_input_window();
    confirmed_interval_open_anchors();
    confirmed_period_closure();
    confirmed_cap_prices();
    terminal_reports();
    std::printf("confirmed source state: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
