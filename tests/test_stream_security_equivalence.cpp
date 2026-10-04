#include <pineforge/pineforge.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool same(double expected, double actual) {
    if (std::isnan(expected) && std::isnan(actual)) return true;
    std::uint64_t expected_bits = 0;
    std::uint64_t actual_bits = 0;
    std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
    std::memcpy(&actual_bits, &actual, sizeof(actual_bits));
    return expected_bits == actual_bits;
}

bool same_text(const char* expected, const char* actual) {
    return expected && actual ? std::strcmp(expected, actual) == 0 : expected == actual;
}

std::uint64_t trade_fingerprint(const pf_report_t& report) {
    std::uint64_t digest = 14695981039346656037ULL;
    auto fold = [&](std::uint64_t value) {
        digest ^= value;
        digest *= 1099511628211ULL;
    };
    auto fold_double = [&](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        fold(bits);
    };
    fold(static_cast<std::uint64_t>(report.trades_len));
    fold_double(report.net_profit);
    for (int index = 0; index < report.trades_len; ++index) {
        const auto& trade = report.trades[index];
        fold(static_cast<std::uint64_t>(trade.entry_time));
        fold(static_cast<std::uint64_t>(trade.exit_time));
        fold(trade.is_long);
        fold(trade.open_at_end);
        fold(static_cast<std::uint64_t>(trade.entry_bar_index));
        fold(static_cast<std::uint64_t>(trade.exit_bar_index));
        for (double value : {trade.entry_price, trade.exit_price, trade.qty, trade.pnl,
                trade.pnl_pct, trade.commission, trade.max_runup, trade.max_drawdown})
            fold_double(value);
    }
    return digest;
}

std::vector<pf_bar_t> make_bars() {
    std::vector<pf_bar_t> bars;
    const std::string case_name = PINEFORGE_SECURITY_CASE;
    if (case_name == "all_in_reversal" || case_name == "whole_all_in_reversal"
        || case_name == "slipped_short") {
        for (int index = 0; index < 8; ++index) {
            const double close = index == 3 ? 101.15 : 100.0;
            bars.push_back({100.0, std::max(100.0, close), 100.0, close, 10.0,
                1577836800000LL + static_cast<std::int64_t>(index) * 60000});
        }
        return bars;
    }
    std::uint64_t seed = 12345;
    double price = 100.0;
    auto rounded = [](double value) { return std::round(value * 100.0) / 100.0; };
    auto next = [&seed]() {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((seed >> 33) % 2001) / 1000.0 - 1.0;
    };
    for (int index = 0; index < 4000; ++index) {
        const double open = price;
        const double close = std::max(5.0, open + next() * 0.8 + std::sin(index / 97.0) * 0.15);
        const double high = std::max(open, close) + std::fabs(next()) * 0.6;
        const double low = std::max(1.0, std::min(open, close) - std::fabs(next()) * 0.6);
        bars.push_back({rounded(open), rounded(high), rounded(low), rounded(close),
            5.0 + std::fabs(next()) * 40.0, 1577836800000LL + static_cast<std::int64_t>(index) * 60000});
        price = rounded(close);
    }
    return bars;
}

bool same_trade(const pf_trade_t& expected, const pf_trade_t& actual) {
    return expected.entry_time == actual.entry_time && expected.exit_time == actual.exit_time
        && expected.is_long == actual.is_long && expected.open_at_end == actual.open_at_end
        && expected.entry_bar_index == actual.entry_bar_index && expected.exit_bar_index == actual.exit_bar_index
        && same(expected.entry_price, actual.entry_price) && same(expected.exit_price, actual.exit_price)
        && same(expected.qty, actual.qty) && same(expected.pnl, actual.pnl)
        && same(expected.pnl_pct, actual.pnl_pct) && same(expected.commission, actual.commission)
        && same(expected.max_runup, actual.max_runup) && same(expected.max_drawdown, actual.max_drawdown);
}

int expected_batch_trades() {
    const std::string name = PINEFORGE_SECURITY_CASE;
    if (name == "htf5_close") return 1308;
    if (name == "htf15_sma_pooc") return 204;
    if (name == "htf60_close") return 109;
    if (name == "daily_close") return 51;
    if (name == "heikinashi5") return 1154;
    if (name == "htf60_ema_gaps") return 21;
    if (name == "period_previous") return 3030;
    return -1;
}

void print_trade(const char* mode, const pf_report_t& report, int index) {
    if (index >= report.trades_len) return;
    const auto& trade = report.trades[index];
    std::printf("  %s: entry_bar=%d exit_bar=%d entry=%.17g exit=%.17g qty=%.17g pnl=%.17g\n",
        mode, trade.entry_bar_index, trade.exit_bar_index, trade.entry_price,
        trade.exit_price, trade.qty, trade.pnl);
}

}

int main(int argc, char** argv) {
    const char* script_tf = argc > 1 ? argv[1] : "1";
    const int warmup = argc > 2 ? std::atoi(argv[2]) : 500;
    auto bars = make_bars();
    if (warmup < 1 || warmup > static_cast<int>(bars.size())) return 2;
    pf_strategy_t batch = strategy_create(nullptr);
    pf_strategy_t stream = strategy_create(nullptr);
    if (!batch || !stream) return 2;
    const std::string case_name = PINEFORGE_SECURITY_CASE;
    const char* input_tf = "1";
    const bool fractional_sizing = case_name == "pooc_slipped_short"
        || case_name == "slipped_short" || case_name == "all_in_reversal";
    const bool whole_sizing = case_name == "whole_all_in_reversal";
    for (pf_strategy_t strategy : {batch, stream}) {
        if (fractional_sizing || whole_sizing) {
            strategy_set_syminfo_mintick(strategy, 0.01);
            strategy_set_syminfo_pointvalue(strategy, 1.0);
            strategy_set_syminfo_metadata(strategy, "qty_step", whole_sizing ? 1.0
                : (case_name == "all_in_reversal" ? 0.25 : 0.0001));
        }
    }
    pf_report_t expected{};
    pf_report_t actual{};
    run_backtest_full(batch, bars.data(), static_cast<int>(bars.size()), input_tf, script_tf,
        0, 4, PF_MAGNIFIER_ENDPOINTS, &expected);
    if (strategy_get_last_error(batch)[0] != '\0') {
        std::printf("FAIL batch: %s\n", strategy_get_last_error(batch));
        return 1;
    }
    if (strategy_stream_begin(stream, bars.data(), warmup, input_tf, script_tf) != 0) {
        std::printf("FAIL stream begin: %s\n", strategy_get_last_error(stream));
        return 1;
    }
    for (int index = warmup; index < static_cast<int>(bars.size()); ++index) {
        if (strategy_stream_push_bar(stream, &bars[index]) != 0) {
            std::printf("FAIL stream push %d: %s\n", index, strategy_get_last_error(stream));
            return 1;
        }
        strategy_stream_order_actions_clear(stream);
    }
    strategy_stream_fill_report(stream, &actual);
    int first_trade = -1;
    for (int index = 0; index < std::min(expected.trades_len, actual.trades_len); ++index) {
        if (!same_trade(expected.trades[index], actual.trades[index])
            || !same_text(strategy_closed_trade_entry_id(batch, index),
                strategy_closed_trade_entry_id(stream, index))
            || !same_text(strategy_closed_trade_exit_id(batch, index),
                strategy_closed_trade_exit_id(stream, index))) {
            first_trade = index;
            break;
        }
    }
    if (first_trade < 0 && expected.trades_len != actual.trades_len)
        first_trade = std::min(expected.trades_len, actual.trades_len);
    bool equal = first_trade < 0 && same(expected.net_profit, actual.net_profit)
        && expected.security_feeds_total == actual.security_feeds_total
        && expected.input_bars_processed == actual.input_bars_processed
        && expected.script_bars_processed == actual.script_bars_processed;
    if (std::string(script_tf) == "1" && expected_batch_trades() >= 0) {
        equal = equal && expected.trades_len == expected_batch_trades()
            && expected.security_feeds_total == 4000;
    }
    if (expected_batch_trades() < 0) equal = equal && expected.trades_len > 0;
    std::printf("%s [%s input=%s script=%s warmup=%d] batch_trades=%d stream_trades=%d "
        "security_feeds_total=%lld/%lld first_trade=%d net_profit=%.17g/%.17g "
        "trade_fingerprint=%016llx/%016llx\n",
        equal ? "PASS" : "FAIL", PINEFORGE_SECURITY_CASE, input_tf, script_tf, warmup,
        expected.trades_len, actual.trades_len, static_cast<long long>(expected.security_feeds_total),
        static_cast<long long>(actual.security_feeds_total), first_trade, expected.net_profit, actual.net_profit,
        static_cast<unsigned long long>(trade_fingerprint(expected)),
        static_cast<unsigned long long>(trade_fingerprint(actual)));
    if (first_trade >= 0) {
        print_trade("batch", expected, first_trade);
        print_trade("stream", actual, first_trade);
    }
    report_free(&expected);
    report_free(&actual);
    strategy_free(batch);
    strategy_free(stream);
    return equal ? 0 : 1;
}
