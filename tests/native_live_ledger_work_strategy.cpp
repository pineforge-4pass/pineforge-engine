// SPDX-License-Identifier: Apache-2.0
#include <pineforge/native_module.hpp>

namespace {
// Real trades and one marked equity observation per bar, with one open lot.
// Different order IDs also exercise journal-only actions and three targets.
class LedgerWorkStrategy : public pineforge::NativeStrategyHost {
    std::uint64_t bars_ = 0;
    void on_native_run_begin() override { bars_ = 0; }
    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        const auto phase = bars_++ % 4;
        if (physical_position().signed_units == 0.0) {
            const char* name = phase == 0 ? "main-open" : "side-open";
            submit_market({pineforge::order_action::Transact{1.0}, name, ""});
        } else {
            const char* name = phase == 1 ? "journal-close" : "third-close";
            submit_market({pineforge::execution::Flatten{}, name, ""});
        }
        update_equity_extremes();
        record_equity_point(bar.timestamp);
    }
};
}
PINEFORGE_EXPORT_NATIVE_STRATEGY(LedgerWorkStrategy);
