// test_ta_tie_cell_eurusd -- the EURUSD tie cell: on OANDA:EURUSD 15 the frozen
// "MACD & Stoch RSI Confluence S/R" strategy's K and D are both exactly 100 on
// the bar of 2025-04-08 02:00 UTC in TradingView, and no K/D cross follows on
// the 02:15 bar. A K that differs from D by one unit in the last place there
// crosses under D on the next bar and enters a short TradingView never took.
//
// The fixture (fixtures/ta_tie_cells/eurusd_15.inc) is generated from a
// TradingView capture of 2026-11-17 (its evidence is kept privately): the chart
// feed's closes from the first bar of the backtest range (2025-04-01 00:00
// UTC) to the last logged bar, and TradingView's exact macd, signal, rsi, K,
// D and K - D on the logged bars 2025-04-08 02:00..03:15 and 10:00..11:15 UTC.
// The body of the test, and what each group of checks separates, is in
// ta_tie_cell_chain.hpp.
//
// The EMA seeding is the one the engine's own winning replay of this chart
// used against TradingView's trade list (first value, no na warm-up).

#include "ta_tie_cell_chain.hpp"

namespace {
#include "fixtures/ta_tie_cells/eurusd_15.inc"
}  // namespace

int main() {
    return tie_cell::run_cell(
        "test_ta_tie_cell_eurusd", pineforge::ta::EmaSeeding::FirstValue, kRangeStartMs,
        kFeed, sizeof(kFeed) / sizeof(kFeed[0]),
        kLogged, sizeof(kLogged) / sizeof(kLogged[0]),
        kTargets, sizeof(kTargets) / sizeof(kTargets[0]));
}
