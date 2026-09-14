#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/intraday_cap.hpp>

namespace pineforge::source {

// Intermediate source-layer host. The remaining source ownership surface is
// filled in by the R4-C L2 transfer (contract sections 3.1 and 7).
class PineStrategyHost : public BacktestEngine {
public:
    explicit PineStrategyHost(
            compat::pine::CapAttachment cap = compat::pine::CapAttachment::None);

    void on_bar(const Bar& bar) final;
    virtual void on_source_bar(const Bar& bar) = 0;
};

} // namespace pineforge::source
