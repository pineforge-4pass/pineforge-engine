// Complete-prefix stream reports equal batch from the first realtime script bar.
// An open position at zero realtime bars retains the warmup mark; mid-bar reads
// are host presentation of a forming bar, not a pinned batch-comparable boundary.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
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
        CHECK(bits(expected.max_drawdown) == bits(actual.max_drawdown));
        CHECK(bits(expected.max_runup) == bits(actual.max_runup));
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

class BracketStrategy final : public pineforge::source::PineStrategyHost {
public:
    BracketStrategy(int entry_kind, bool same_calculation, bool is_long,
                    bool limit_exit, double offset, bool prearm)
        : entry_kind_(entry_kind), same_calculation_(same_calculation),
          is_long_(is_long), limit_exit_(limit_exit), offset_(offset), prearm_(prearm) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config{};
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_string("tickerid", "TEST:BRACKET");
        set_syminfo_string("ticker", "BRACKET");
        set_syminfo_string("type", "stock");
        set_syminfo_metadata("qty_step", 1.0);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_chart_timezone("UTC");
        fixture_retain_all_events();
    }

    void on_source_bar(const pineforge::Bar&) override {
        const double absent = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == 1)
            strategy_entry("ENTRY", is_long_, entry_kind_ == 2 ? 100.0 + offset_ : absent,
                           entry_kind_ == 1 ? 100.0 + offset_ : absent, 2.0);
        if (pine_bar_index() == (same_calculation_ ? 1 : 2)
            || (prearm_ && same_calculation_ && pine_bar_index() == 0)) {
            const double level = (is_long_ == limit_exit_ ? 105.0 : 95.0) + offset_;
            strategy_exit("EXIT", "ENTRY", limit_exit_ ? level : absent,
                          limit_exit_ ? absent : level);
        }
    }

private:
    int entry_kind_;
    bool same_calculation_;
    bool is_long_;
    bool limit_exit_;
    double offset_;
    bool prearm_;
};

void same_calculation_bracket_reports(int timeframe, int entry_kind,
                                     bool same_calculation, bool is_long,
                                     bool limit_exit, double offset, bool prearm) {
    std::vector<pineforge::Bar> bars;
    const int exit_bar = same_calculation ? 2 : 3;
    const double sign = is_long == limit_exit ? 1.0 : -1.0;
    for (int index = 0; index < 5 * timeframe; ++index) {
        double open = 100.0;
        double close = 100.0;
        double high = 100.1;
        double low = 99.9;
        if (index / timeframe == exit_bar && index % timeframe >= std::min(3, timeframe - 1)) {
            close += sign * 5.0;
            if (timeframe != 1) open = close;
            high = std::max(high, 100.0 + sign * 10.0);
            low = std::min(low, 100.0 + sign * 10.0);
        }
        bars.push_back({open + offset, high + offset, low + offset, close + offset,
                        10.0, 1743465600000LL + index * 60000LL});
    }
    BracketStrategy batch(entry_kind, same_calculation, is_long, limit_exit, offset, prearm);
    BracketStrategy stream(entry_kind, same_calculation, is_long, limit_exit, offset, prearm);
    const auto script_tf = std::to_string(timeframe);
    batch.run(bars.data(), static_cast<int>(bars.size()), "1", script_tf);
    CHECK(batch.last_error().empty());
    CHECK(stream.stream_begin(bars.data(), timeframe, "1", script_tf));
    for (int index = timeframe; index < static_cast<int>(bars.size()); ++index) {
        CHECK(stream.stream_push_bar(bars[index]));
        pineforge::ReportC interim{};
        CHECK(strategy_stream_fill_report(&stream, reinterpret_cast<pf_report_t*>(&interim)) == 0);
        pineforge::BacktestEngine::free_report(&interim);
    }
    CHECK(stream.stream_end(false));
    pineforge::ReportC expected{};
    pineforge::ReportC observed{};
    batch.fill_report(&expected);
    CHECK(strategy_stream_fill_report(&stream, reinterpret_cast<pf_report_t*>(&observed)) == 0);
    CHECK(expected.trades_len == 1);
    if (expected.trades_len == 1) CHECK(!expected.trades[0].open_at_end);
    const int prior_failures = failures;
    if (timeframe == 15 && entry_kind == 0 && same_calculation && is_long
        && !limit_exit && offset == 0.0 && prearm && expected.trades_len == 1)
        std::fprintf(stderr, "minimal bracket entry=%.17g exit=%.17g drawdown=%.17g/%.17g\n",
                     expected.trades[0].entry_price, expected.trades[0].exit_price,
                     expected.trades[0].max_drawdown, observed.trades[0].max_drawdown);
    compare_reports(expected, observed);
    if (prior_failures != failures)
        std::fprintf(stderr, "bracket tf=%d entry=%d same=%d long=%d limit=%d offset=%.3f prearm=%d\n",
                     timeframe, entry_kind, same_calculation, is_long, limit_exit, offset, prearm);
    pineforge::BacktestEngine::free_report(&expected);
    pineforge::BacktestEngine::free_report(&observed);
}

}

int main() {
    for (int split : {15, 22, 29}) cumulative_reports_do_not_change_continuation(split);
    for (int timeframe : {1, 5, 15, 60})
        for (int entry_kind : {0, 1, 2})
            for (bool same_calculation : {false, true})
                for (bool is_long : {false, true})
                    for (bool limit_exit : {false, true})
                        for (double offset : {0.0, 0.003})
                            for (bool prearm : {false, true})
                                same_calculation_bracket_reports(timeframe, entry_kind,
                                    same_calculation, is_long, limit_exit, offset, prearm);
    std::printf("stream report equivalence: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
