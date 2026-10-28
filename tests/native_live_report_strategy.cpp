// SPDX-License-Identifier: Apache-2.0
#include <pineforge/native_module.hpp>

namespace {

// The runner's v1 configuration selects HostRecorded. Record real marked
// equity here so interval/export tests exercise finite ratios, not an empty
// curve's four null values. The memory row keeps its original module.
class NativeLiveReportExample : public pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        if (physical_position().signed_units == 0.0)
            submit_market({pineforge::order_action::Transact{1.0}, "report-open", ""});
        update_equity_extremes();
        record_equity_point(bar.timestamp);
    }
};

} // namespace

PINEFORGE_EXPORT_NATIVE_STRATEGY(NativeLiveReportExample);
