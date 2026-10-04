#include <pineforge/pineforge.h>
#include "../runner/capabilities.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<pf_bar_t> bars;
    for (int index = 0; index < 700; ++index) {
        const double open = 100.0 + std::sin(index * 0.13);
        const double close = 100.0 + std::sin((index + 1) * 0.13);
        bars.push_back({open, std::max(open, close) + 1.0, std::min(open, close) - 1.0,
                       close, 50.0, 1577836800000LL + index * 60000LL});
    }
    auto batch = strategy_create(nullptr);
    assert(batch);
    std::size_t required = 0;
    assert(strategy_capabilities_receipt(batch, nullptr, 0, &required, nullptr, 0) == PF_SETTINGS_BUFFER_TOO_SMALL);
    std::vector<char> receipt(required);
    assert(strategy_capabilities_receipt(batch, receipt.data(), receipt.size(), &required, nullptr, 0) == PF_SETTINGS_OK);
    pineforge::live::require_close_only_capabilities(receipt.data());
    pf_report_t expected{};
    run_backtest_full(batch, bars.data(), static_cast<int>(bars.size()), "1", "1", 0, 4, PF_MAGNIFIER_COSINE, &expected);
    assert(expected.trades_len > 100);
    for (int warmup : {3, 30, 500}) {
        auto stream = strategy_create(nullptr);
        assert(strategy_stream_begin(stream, bars.data(), warmup, "1", "1") == 0);
        for (int index = warmup; index < static_cast<int>(bars.size()); ++index) {
            assert(strategy_stream_push_bar(stream, &bars[index]) == 0);
            strategy_stream_order_actions_clear(stream);
        }
        pf_report_t actual{};
        assert(strategy_stream_fill_report(stream, &actual) == 0);
        assert(expected.trades_len == actual.trades_len);
        assert(expected.net_profit == actual.net_profit);
        for (int index = 0; index < expected.trades_len; ++index) {
            const auto& expected_trade = expected.trades[index];
            const auto& actual_trade = actual.trades[index];
            assert(expected_trade.entry_time == actual_trade.entry_time);
            assert(expected_trade.exit_time == actual_trade.exit_time);
            assert(expected_trade.entry_price == actual_trade.entry_price);
            assert(expected_trade.exit_price == actual_trade.exit_price);
            assert(expected_trade.qty == actual_trade.qty);
            assert(expected_trade.is_long == actual_trade.is_long);
            assert(expected_trade.pnl == actual_trade.pnl);
            assert(expected_trade.open_at_end == actual_trade.open_at_end);
        }
        report_free(&actual);
        strategy_free(stream);
        std::cout << "batch/stream market equivalence: warmup=" << warmup
                  << " trades=" << expected.trades_len << " IDENTICAL\n";
    }
    report_free(&expected);
    strategy_free(batch);
}
