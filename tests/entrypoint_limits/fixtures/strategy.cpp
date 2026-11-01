#include <pineforge/native_module.hpp>

namespace {

class CompileFixture : public pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        if (bar.close > bar.open && physical_position().signed_units == 0.0)
            submit_market({pineforge::order_action::Transact{1.0}, "open", ""});
        update_equity_extremes();
        record_equity_point(bar.timestamp);
    }
};

} // namespace

PINEFORGE_EXPORT_NATIVE_STRATEGY(CompileFixture);

// The compile-stage loader also checks an input-dependent exported function.
extern "C" double entrypoint_fixture_signal(double opening, double closing) {
    return closing - opening;
}
