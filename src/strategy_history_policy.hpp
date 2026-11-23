#pragma once

#include "strategy_module_symbol.hpp"

// Generated-module capability, deliberately not a runtime export. A weak
// reference also permits a statically linked strategy executable to advertise
// the same capability without exporting its entire symbol table.
extern "C" int strategy_reads_trade_history() PF_WEAK_STRATEGY_EXPORT;

namespace pineforge::detail {

static bool strategy_reads_history_by_index(const BacktestEngine& engine) noexcept {
    try {
        const auto query = strategy_module_symbol(engine, "strategy_reads_trade_history",
                                                  strategy_reads_trade_history);
        return !query || query() != 0;
    } catch (...) {
        return true;
    }
}

} // namespace pineforge::detail
