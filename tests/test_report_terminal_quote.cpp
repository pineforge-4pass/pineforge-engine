#include "native_current_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/metrics.hpp>
#include <cstring>

using namespace r4_test;

namespace {

TradeC canonical_trade(TradeC row) {
    auto* bytes = reinterpret_cast<unsigned char*>(&row);
    std::memset(bytes + offsetof(TradeC, is_long) + sizeof(row.is_long), 0,
                offsetof(TradeC, max_runup) - offsetof(TradeC, is_long) - sizeof(row.is_long));
    std::memset(bytes + offsetof(TradeC, open_at_end) + sizeof(row.open_at_end), 0,
                sizeof(row) - offsetof(TradeC, open_at_end) - sizeof(row.open_at_end));
    return row;
}

class QuoteHost final : public pineforge::source::PineStrategyHost {
public:
    explicit QuoteHost(bool long_side, CommissionType commission = CommissionType::PERCENT)
        : long_side_(long_side) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(commission);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 1) strategy_entry("open", long_side_);
        if (bar_index_ == 2) strategy_close("open", {}, 1.0);
    }

    pf_metrics_t projected_metrics(const ReportC& report) const {
        using pineforge::metrics::TradeFilter;
        return {
            pineforge::metrics::compute_trade_stats(report.trades, report.trades_len, TradeFilter::ALL, initial_capital_),
            pineforge::metrics::compute_trade_stats(report.trades, report.trades_len, TradeFilter::LONG, initial_capital_),
            pineforge::metrics::compute_trade_stats(report.trades, report.trades_len, TradeFilter::SHORT, initial_capital_),
            pineforge::metrics::compute_equity_stats(report.equity_curve, report.equity_curve_len,
                initial_capital_, chart_timezone_, first_bar_open_, current_bar_.close,
                bars_in_market_, report.net_profit),
        };
    }

private:
    bool long_side_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int index = 0; index <= 60; ++index) {
        const double price = 100.0 + index * 0.125;
        bars.push_back({price, price + 0.25, price - 0.25, price + 0.125,
                        1.0, T + index * 60000LL});
    }
    return bars;
}

void compare(bool long_side, bool stream, bool magnifier = true,
             std::int64_t quote_offset = 0, bool metadata = false,
             CommissionType commission = CommissionType::PERCENT) {
    scenario = stream ? "ended stream" : "batch";
    QuoteHost baseline(long_side, commission), quoted(long_side, commission);
    const auto bars = feed();
    const auto quote_time = bars.back().timestamp + quote_offset;
    CHECK(quoted.set_report_terminal_quote(quote_time, 112.0));
    CHECK(!quoted.set_report_terminal_quote(quote_time, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!quoted.set_report_terminal_quote(-1, 112.0));
    if (metadata) {
        quoted.clear_report_terminal_quote();
        quoted.set_syminfo_metadata("report_terminal_quote_time_ms", static_cast<double>(quote_time));
        quoted.set_syminfo_metadata("report_terminal_quote_close", 112.0);
    }
    for (auto* host : {&baseline, &quoted}) {
        if (stream) {
            CHECK(host->stream_begin(bars.data(), 15, "1", "15"));
            for (std::size_t index = 15; index < bars.size(); ++index)
                CHECK(host->stream_push_bar(bars[index]));
            CHECK(host->stream_end(false));
        } else {
            host->run(bars.data(), static_cast<int>(bars.size()), "1", "15", magnifier);
        }
        CHECK(host->last_error().empty());
    }
    CHECK(baseline.broker_state_hash() == quoted.broker_state_hash());
    CHECK(baseline.trade_count() == quoted.trade_count());
    for (int index = 0; index < baseline.report_trade_count(); ++index) {
        const auto& left = baseline.get_report_trade(index);
        const auto& right = quoted.get_report_trade(index);
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.pnl == right.pnl);
        CHECK(left.commission == right.commission);
        CHECK(left.qty == right.qty);
    }
    ReportC before{}, after{}, again{};
    baseline.fill_report(&before);
    quoted.fill_report(&after);
    const auto hash = quoted.broker_state_hash();
    quoted.fill_report(&again);
    CHECK(hash == quoted.broker_state_hash());
    CHECK(before.trades_len == after.trades_len);
    CHECK(before.broker_state_hash_len == after.broker_state_hash_len);
    CHECK(std::memcmp(before.broker_state_hash, after.broker_state_hash,
                      before.broker_state_hash_len * sizeof(std::uint64_t)) == 0);
    double change = 0.0;
    int marks = 0;
    const bool presented = !stream && magnifier && quote_offset >= -900000 && quote_offset <= 0;
    for (int index = 0; index < before.trades_len; ++index) {
        auto normalized = canonical_trade(after.trades[index]);
        const auto original = canonical_trade(before.trades[index]);
        const auto repeated = canonical_trade(again.trades[index]);
        CHECK(std::memcmp(&normalized, &repeated, sizeof(TradeC)) == 0);
        if (original.open_at_end && presented) {
            ++marks;
            CHECK(normalized.exit_time == quote_time);
            CHECK(normalized.exit_price == 112.0);
            change += normalized.pnl - original.pnl;
            normalized.exit_time = original.exit_time;
            normalized.exit_price = original.exit_price;
            normalized.pnl = original.pnl;
            normalized.pnl_pct = original.pnl_pct;
            normalized.commission = original.commission;
        }
        CHECK(std::memcmp(&original, &normalized, sizeof(TradeC)) == 0);
    }
    CHECK(marks == (presented ? 2 : 0));
    CHECK(presented ? change != 0.0 : change == 0.0);
    CHECK(after.net_profit == before.net_profit + change);
    CHECK(before.equity_curve_len == after.equity_curve_len);
    for (std::int64_t index = 0; index < before.equity_curve_len; ++index) {
        auto normalized = after.equity_curve[index];
        if (presented && index + 1 == before.equity_curve_len) {
            CHECK(normalized.time_ms == quote_time);
            CHECK(normalized.equity == before.equity_curve[index].equity + change);
            normalized.time_ms = before.equity_curve[index].time_ms;
            normalized.equity = before.equity_curve[index].equity;
        }
        CHECK(std::memcmp(&normalized, &before.equity_curve[index], sizeof(normalized)) == 0);
    }
    auto normalized = after;
    normalized.net_profit = before.net_profit;
    normalized.trades = before.trades;
    normalized.equity_curve = before.equity_curve;
    normalized.broker_state_hash = before.broker_state_hash;
    normalized.security_diag = before.security_diag;
    normalized.trace = before.trace;
    normalized.trace_names = before.trace_names;
    const auto expected_metrics = baseline.projected_metrics(after);
    CHECK(std::memcmp(&after.metrics, &expected_metrics, sizeof(pf_metrics_t)) == 0);
    normalized.metrics = before.metrics;
    CHECK(std::memcmp(&before, &normalized, sizeof(ReportC)) == 0);
    BacktestEngine::free_report(&before);
    BacktestEngine::free_report(&after);
    BacktestEngine::free_report(&again);
}

}

int main() {
    for (bool long_side : {false, true}) {
        compare(long_side, false);
        compare(long_side, true);
        compare(long_side, false, false);
        compare(long_side, false, true, -3600000);
        compare(long_side, false, true, 3600000);
        compare(long_side, false, true, 0, true);
        compare(long_side, false, true, 0, false, CommissionType::CASH_PER_CONTRACT);
        compare(long_side, false, true, 0, false, CommissionType::CASH_PER_ORDER);
    }
    std::printf("report-only terminal quote: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
