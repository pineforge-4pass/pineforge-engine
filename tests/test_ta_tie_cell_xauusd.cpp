// test_ta_tie_cell_xauusd -- the XAUUSD tie cell: on OANDA:XAUUSD 15 the frozen
// "MACD & Stoch RSI Confluence S/R" strategy's K and D are both exactly 0 on
// the bar of 2025-04-03 05:30 UTC in TradingView. A residue of the order of
// 1e-15 left in K or D there gives K - D a sign TradingView's does not have.
//
// The fixture (fixtures/ta_tie_cells/xauusd_15.inc) is generated from a
// TradingView capture of 2026-11-17 (its evidence is kept privately): the chart
// feed's closes from the first bar of the backtest range (2025-04-01 00:00
// UTC) to the last logged bar, and TradingView's exact macd, signal, rsi, K,
// D and K - D on the logged bars 2025-04-03 04:45..06:30 UTC. The body of the
// test, and what each group of checks separates, is in ta_tie_cell_chain.hpp.
//
// The EMA seeding is the one the engine's own winning replay of this chart
// used against TradingView's trade list (na until `length` values, then
// their mean).

#include "ta_tie_cell_chain.hpp"

namespace {
#include "fixtures/ta_tie_cells/xauusd_15.inc"
}  // namespace

int main() {
    return tie_cell::run_cell(
        "test_ta_tie_cell_xauusd", pineforge::ta::EmaSeeding::SimpleAverage, kRangeStartMs,
        kFeed, sizeof(kFeed) / sizeof(kFeed[0]),
        kLogged, sizeof(kLogged) / sizeof(kLogged[0]),
        kTargets, sizeof(kTargets) / sizeof(kTargets[0]));
}
