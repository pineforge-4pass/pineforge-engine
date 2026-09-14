#include <pineforge/source/pine_strategy_host.hpp>

namespace pineforge::source {

PineStrategyHost::PineStrategyHost(compat::pine::CapAttachment cap)
    : BacktestEngine(cap) {}

void PineStrategyHost::on_bar(const Bar& bar) {
    on_source_bar(bar);
}

} // namespace pineforge::source
