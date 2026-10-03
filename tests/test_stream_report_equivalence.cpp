// Complete-prefix stream reports equal batch from the first realtime script bar.
// An open position at zero realtime bars retains the warmup mark; mid-bar reads
// are host presentation of a forming bar, not a pinned batch-comparable boundary.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "oracle_fixture_config_shim.hpp"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(expression) \
    do { \
        ++checks; \
        if (!(expression)) { \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expression); \
            ++failures; \
        } \
    } while (false)

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

class ReportStrategy final : public pineforge::source::PineStrategyHost {
public:
    ReportStrategy() {
        initial_capital_ = 10000.0;
        syminfo_mintick_ = 0.01;
        default_qty_type_ = pineforge::QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 15.0;
        commission_type_ = pineforge::CommissionType::PERCENT;
        commission_value_ = 0.075;
        pyramiding_ = 2;
        margin_call_enabled_ = false;
        set_pine_risk_max_drawdown(95.0, true);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 4 || bar_index_ == 16)
            strategy_entry("long", true);
        if (bar_index_ == 7)
            strategy_close("long", {}, std::numeric_limits<double>::quiet_NaN(), 40.0);
        if (bar_index_ == 10) strategy_entry("short", false);
        if (bar_index_ == 14) strategy_close_all();
    }

    double stored_open_profit() const {
        return equity_curve_.empty() ? 0.0 : equity_curve_.back().open_profit;
    }

    std::size_t real_trade_count() const { return trades_.size(); }
};

void compare_reports(const pineforge::ReportC& batch, const pineforge::ReportC& stream) {
    CHECK(batch.input_bars_processed == stream.input_bars_processed);
    CHECK(batch.script_bars_processed == stream.script_bars_processed);
    CHECK(batch.total_trades == stream.total_trades);
    CHECK(bits(batch.net_profit) == bits(stream.net_profit));
    CHECK(batch.trades_len == stream.trades_len);
    for (int index = 0; index < batch.trades_len && index < stream.trades_len; ++index) {
        const auto& expected = batch.trades[index];
        const auto& actual = stream.trades[index];
        CHECK(expected.entry_time == actual.entry_time);
        CHECK(expected.exit_time == actual.exit_time);
        CHECK(expected.is_long == actual.is_long);
        CHECK(expected.open_at_end == actual.open_at_end);
        CHECK(bits(expected.qty) == bits(actual.qty));
        CHECK(bits(expected.entry_price) == bits(actual.entry_price));
        CHECK(bits(expected.exit_price) == bits(actual.exit_price));
        CHECK(bits(expected.pnl) == bits(actual.pnl));
        CHECK(bits(expected.commission) == bits(actual.commission));
    }
    CHECK(batch.equity_curve_len == stream.equity_curve_len);
    bool diagnosed = false;
    for (std::int64_t index = 0;
         index < batch.equity_curve_len && index < stream.equity_curve_len; ++index) {
        const auto& expected = batch.equity_curve[index];
        const auto& actual = stream.equity_curve[index];
        if (!diagnosed && (bits(expected.equity) != bits(actual.equity)
            || bits(expected.open_profit) != bits(actual.open_profit))) {
            std::fprintf(stderr, "curve prefix=%lld index=%lld equity=%.17g/%.17g open=%.17g/%.17g\n",
                static_cast<long long>(batch.script_bars_processed),
                static_cast<long long>(index), expected.equity, actual.equity,
                expected.open_profit, actual.open_profit);
            diagnosed = true;
        }
        CHECK(expected.time_ms == actual.time_ms);
        CHECK(bits(expected.equity) == bits(actual.equity));
        CHECK(bits(expected.open_profit) == bits(actual.open_profit));
    }
    CHECK(bits(batch.metrics.equity.max_equity_drawdown)
        == bits(stream.metrics.equity.max_equity_drawdown));
    CHECK(bits(batch.metrics.equity.max_equity_runup)
        == bits(stream.metrics.equity.max_equity_runup));
}

void cumulative_reports_do_not_change_continuation(int split) {
    std::vector<pineforge::Bar> bars;
    for (int index = 0; index < 300; ++index) {
        const double price = 100.003 + (index % 31) * 0.137;
        bars.push_back({price, price + 0.42, price - 0.31, price + 0.093,
                        1.0, 1743465600000LL + index * 60000LL});
    }
    ReportStrategy stream;
    ReportStrategy unobserved;
    CHECK(stream.stream_begin(bars.data(), split, "1", "15"));
    CHECK(unobserved.stream_begin(bars.data(), split, "1", "15"));
    bool saw_unrealized_profit = false;
    for (int index = split; index < static_cast<int>(bars.size()); ++index) {
        CHECK(stream.stream_push_bar(bars[index]));
        CHECK(unobserved.stream_push_bar(bars[index]));
        const auto hash = stream.stream_state_hash();
        const double ordinary_profit = stream.stored_open_profit();
        pineforge::ReportC observed{};
        CHECK(strategy_stream_fill_report(&stream,
            reinterpret_cast<pf_report_t*>(&observed)) == 0);
        CHECK(hash == stream.stream_state_hash());
        CHECK(stream.stream_state_hash() == unobserved.stream_state_hash());
        CHECK(bits(ordinary_profit) == bits(stream.stored_open_profit()));
        if (ordinary_profit != 0.0) saw_unrealized_profit = true;
        if ((index + 1) % 15 == 0) {
            ReportStrategy batch;
            batch.run(bars.data(), index + 1, "1", "15");
            CHECK(batch.last_error().empty());
            pineforge::ReportC expected{};
            batch.fill_report(&expected);
            compare_reports(expected, observed);
            pineforge::BacktestEngine::free_report(&expected);
        }
        pineforge::BacktestEngine::free_report(&observed);
    }
    CHECK(saw_unrealized_profit);
    CHECK(stream.real_trade_count() >= 3);
    CHECK(stream.stream_end(false));
    pineforge::ReportC ended{};
    CHECK(strategy_stream_fill_report(&stream,
        reinterpret_cast<pf_report_t*>(&ended)) == 0);
    ReportStrategy batch;
    batch.run(bars.data(), static_cast<int>(bars.size()), "1", "15");
    pineforge::ReportC expected{};
    batch.fill_report(&expected);
    compare_reports(expected, ended);
    pineforge::BacktestEngine::free_report(&expected);
    pineforge::BacktestEngine::free_report(&ended);
}

}

int main() {
    for (int split : {15, 22, 29}) cumulative_reports_do_not_change_continuation(split);
    std::printf("stream report equivalence: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
