// Replay S record: C ABI accessor and frozen ABI-layout red-first tests.
//
// The only new API used here is strategy_net_profit_sum. The fixture creates
// a real native host, submits real orders, and reads reports through public
// C entry points. No test writes engine state or derives its oracle from a
// report, equity accessor, or another engine-produced P&L.

#include "replay_fixture.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

struct Report {
    pf_report_t value{};
    ~Report() { strategy_native_report_free_v1(&value); }
};

void check_frozen_layouts() {
    static_assert(PF_ABI_VERSION == 5, "report replay leaves the current ABI version unchanged");
    static_assert(offsetof(pf_equity_stats_t, sharpe_monthly) == 48,
                  "pf_equity_stats_t::sharpe_monthly moved");
    static_assert(offsetof(pf_equity_stats_t, sortino_monthly) == 56,
                  "pf_equity_stats_t::sortino_monthly moved");
    static_assert(offsetof(pf_equity_stats_t, sharpe_bar) == 64,
                  "pf_equity_stats_t::sharpe_bar moved");
    static_assert(offsetof(pf_equity_stats_t, open_pl) == 112,
                  "pf_equity_stats_t::open_pl moved");
    static_assert(sizeof(pf_equity_stats_t) == 120,
                  "pf_equity_stats_t size changed");
    static_assert(offsetof(pf_metrics_t, equity) == 648,
                  "pf_metrics_t::equity moved");
    static_assert(sizeof(pf_metrics_t) == 768, "pf_metrics_t size changed");

    CHECK(sizeof(pf_bar_t) >= 6 * sizeof(double));
    CHECK(sizeof(pf_trade_tick_t) == 32);
    CHECK(sizeof(pf_trade_t) >= 80);
    CHECK(sizeof(pf_report_t) >=
          80 + sizeof(pf_metrics_t) + sizeof(pf_equity_point_t*) + sizeof(std::int64_t));
}

bool prepare(replay_fixture::Scenario& scenario) {
    const auto callbacks = replay_fixture::callbacks_for(scenario);
    scenario.handle = strategy_native_host_create_v1(&callbacks);
    CHECK(scenario.handle != nullptr);
    if (!scenario.handle) return false;
    const auto spec = replay_fixture::spec_for(scenario);
    const auto ext = replay_fixture::report_policy();
    const int status = strategy_configure_native_ext_v1(scenario.handle, &spec, &ext);
    CHECK(status == PF_NATIVE_OK);
    return status == PF_NATIVE_OK;
}

void test_realized_sum_matches_independent_left_fold(
        const std::array<int, replay_fixture::kBars>& prices, double capital) {
    replay_fixture::Scenario scenario;
    scenario.initial_capital = capital;
    if (!prepare(scenario)) return;

    // These are the scenario's input prices, not prices read from its report.
    // The first two closes belong to ONE entry of two lots. Every physical
    // close has quantity 1, pointvalue 1 and entry/exit commission 1 + 1.
    struct Close {
        int entry;
        int exit;
        bool is_long;
    };
    const std::array<Close, 3> closes{{
        {prices[1], prices[2], true},
        {prices[1], prices[3], true},
        {prices[4], prices[5], false},
    }};
    constexpr int quantity = 1;
    constexpr int pointvalue = 1;
    constexpr int entry_commission = 1;
    constexpr int exit_commission = 1;
    std::array<int, 3> per_close{};
    double expected = 0.0;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const auto& close = closes[i];
        const int points = close.is_long
            ? close.exit - close.entry : close.entry - close.exit;
        per_close[i] = points * quantity * pointvalue
                    - entry_commission - exit_commission;
        expected += static_cast<double>(per_close[i]);  // Plain left fold.
    }

    const auto bars = replay_fixture::bars_for(prices);
    // Deliberately defer report retrieval until AFTER the first two reads.
    const int status = strategy_native_run_v1(
        scenario.handle, bars.data(), static_cast<int>(bars.size()), nullptr);
    CHECK(status == PF_NATIVE_OK);
    if (status != PF_NATIVE_OK) return;
    CHECK(strategy_last_run_status(scenario.handle) == 0);
    CHECK(scenario.command_error == PF_NATIVE_OK);
    CHECK(scenario.calculations == static_cast<int>(bars.size()));
    CHECK(scenario.openings == 3);
    CHECK(scenario.physical_closes == 3);
    CHECK(scenario.closes_have_one_unit);
    CHECK(strategy_position_size(scenario.handle) == 1.0);

    const double first = strategy_net_profit_sum(scenario.handle);
    CHECK(first == expected);
    CHECK(strategy_net_profit_sum(scenario.handle) == first);
    const double capital_inclusive = strategy_current_equity(scenario.handle);
    CHECK(capital_inclusive == capital + expected);
    CHECK(first != capital_inclusive);

    Report report;
    CHECK(strategy_stream_fill_report(scenario.handle, &report.value) == 0);
    CHECK(strategy_net_profit_sum(scenario.handle) == expected);
    CHECK(strategy_net_profit_sum(scenario.handle) == first);
    CHECK(strategy_current_equity(scenario.handle) == capital_inclusive);
    CHECK(strategy_position_size(scenario.handle) == 1.0);
    CHECK(report.value.input_bars_processed == static_cast<std::int64_t>(bars.size()));
    CHECK(report.value.script_bars_processed == static_cast<std::int64_t>(bars.size()));
    CHECK(report.value.trades_len == 4);
    CHECK(report.value.total_trades == 4);
    CHECK(report.value.trades != nullptr);
    if (!report.value.trades || report.value.trades_len != 4) return;

    for (std::size_t i = 0; i < closes.size(); ++i) {
        const auto& trade = report.value.trades[i];
        CHECK(trade.open_at_end == 0);
        CHECK(trade.is_long == static_cast<int>(closes[i].is_long));
        CHECK(trade.entry_price == closes[i].entry);
        CHECK(trade.exit_price == closes[i].exit);
        CHECK(trade.qty == quantity);
        CHECK(trade.commission == entry_commission + exit_commission);
        CHECK(trade.pnl == per_close[i]);
    }
    // Same entry timestamp/bar and incarnation prove two slices of one lot,
    // rather than two independently entered longs.
    CHECK(report.value.trades[0].entry_time == bars[1].timestamp);
    CHECK(report.value.trades[0].entry_time == report.value.trades[1].entry_time);
    CHECK(report.value.trades[0].entry_bar_index == report.value.trades[1].entry_bar_index);
    const auto first_entry = strategy_closed_trade_entry_incarnation(scenario.handle, 0);
    CHECK(first_entry != 0);
    CHECK(strategy_closed_trade_entry_incarnation(scenario.handle, 1) == first_entry);

    const auto& terminal = report.value.trades[3];
    CHECK(terminal.open_at_end != 0);
    CHECK(terminal.is_long == 1);
    CHECK(terminal.qty == 1.0);
    CHECK(terminal.entry_price == prices[6]);
    CHECK(terminal.entry_time == bars[6].timestamp);
    CHECK(std::isfinite(terminal.pnl) && terminal.pnl != 0.0);
    // This fold is ONLY the explicitly wrong candidate, never the oracle.
    double presented_sum = 0.0;
    for (int i = 0; i < report.value.trades_len; ++i)
        presented_sum += report.value.trades[i].pnl;
    CHECK(std::isfinite(presented_sum));
    CHECK(presented_sum != expected);
    CHECK(first != presented_sum);

    // Retrieving/freeing reports must not consume or amend the accumulator.
    Report repeated;
    CHECK(strategy_stream_fill_report(scenario.handle, &repeated.value) == 0);
    CHECK(repeated.value.trades_len == report.value.trades_len);
    strategy_native_report_free_v1(&report.value);
    CHECK(strategy_net_profit_sum(scenario.handle) == expected);
    CHECK(strategy_last_run_status(scenario.handle) == 0);
}

void test_zero_trade_run_returns_positive_zero() {
    replay_fixture::Scenario scenario;
    scenario.trade = false;
    if (!prepare(scenario)) return;
    const auto bars = replay_fixture::bars_for({100, 100, 107, 109, 120, 116, 130, 140});
    const int status = strategy_native_run_v1(
        scenario.handle, bars.data(), static_cast<int>(bars.size()), nullptr);
    CHECK(status == PF_NATIVE_OK);
    if (status != PF_NATIVE_OK) return;
    CHECK(scenario.calculations == static_cast<int>(bars.size()));
    CHECK(scenario.openings == 0 && scenario.physical_closes == 0);
    CHECK(scenario.command_error == PF_NATIVE_OK);
    const double first = strategy_net_profit_sum(scenario.handle);
    CHECK(first == 0.0);
    CHECK(!std::signbit(first));
    const double second = strategy_net_profit_sum(scenario.handle);
    CHECK(second == 0.0 && !std::signbit(second));
    Report report;
    CHECK(strategy_stream_fill_report(scenario.handle, &report.value) == 0);
    CHECK(report.value.trades_len == 0 && report.value.total_trades == 0);
    CHECK(report.value.script_bars_processed == static_cast<std::int64_t>(bars.size()));
    const double after_report = strategy_net_profit_sum(scenario.handle);
    CHECK(after_report == 0.0 && !std::signbit(after_report));
    CHECK(strategy_current_equity(scenario.handle) == scenario.initial_capital);
}

}  // namespace

int main() {
    check_frozen_layouts();
    CHECK(pf_abi_version() == PF_ABI_VERSION);
    CHECK(std::isnan(strategy_net_profit_sum(nullptr)));

    // (107 - 100 - 2) + (109 - 100 - 2) + (120 - 116 - 2) = 14.
    test_realized_sum_matches_independent_left_fold(
        {100, 100, 107, 109, 120, 116, 130, 140}, 12345.0);
    // (-3 - 2) + (-1 - 2) + (-6 - 2) = -16; same orders, different input.
    test_realized_sum_matches_independent_left_fold(
        {100, 100, 97, 99, 120, 126, 130, 143}, 5000.0);
    test_zero_trade_run_returns_positive_zero();

    if (failures != 0) {
        std::printf("Replay ABI: %d checks failed\n", failures);
        return 1;
    }
    std::puts("Replay ABI: checks passed");
    return 0;
}
