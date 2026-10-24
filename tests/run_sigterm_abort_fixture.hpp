#pragma once

// A real native host shared by the C++ boundary test and the ctypes module.
// The hook pauses after a bar's identity, before its command and final match.
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <string>

namespace q7_fixture {

class Host : public pineforge::NativeStrategyHost {
public:
    int callbacks = 0;
    int applied = 0;
    int begins = 0;

    Host() { set_trace_enabled(true); }

    void configure(const std::string& input_tf, const std::string& script_tf) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"q7-sigterm", ++runs_};
        spec.input_tf = input_tf;
        spec.script_tf = script_tf;
        spec.ticker = "MOCK";
        spec.tickerid = "TEST:MOCK";
        spec.type = "crypto";
        spec.currency = "USD";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
        spec.fee_value = 0.0;
        spec.close_execution = pineforge::NativeCloseExecution::AfterCalculation;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        spec.abort_reporting = pineforge::NativeAbortReporting::Quiet;
        spec.report_open_position_at_end = false;
        if (configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
            throw pineforge::coded<std::logic_error>(
                pineforge::RunFailureCode::engine_invariant, {}, "Q7 fixture setup refused");
    }

    void on_native_run_begin() override { ++begins; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext& context) override {
        const int index = context.coordinate.interval_index;
        // Larger than the writer's 64 KiB buffer: abandoning a suffix would
        // leave this real X record unterminated on fd 4.
        trace(identity_, index);
        after_identity(index);
        const auto result = submit({pineforge::native_order::Transact{
            index % 2 == 0 ? 1.0 : -1.0}, "q7", ""});
        if (result.status != pineforge::native_order::SubmitStatus::Accepted || !result.handle)
            throw pineforge::coded<std::logic_error>(
                pineforge::RunFailureCode::engine_invariant, {}, "Q7 fixture command refused");
        trace("tail", index);
        ++callbacks;
    }

    void on_native_applied(const pineforge::native_order::ExecutionAppliedEvent&,
                           const pineforge::NativeDecisionContext& context) override {
        ++applied;
        trace("applied", context.coordinate.interval_index);
    }

protected:
    virtual void after_identity(int) {}

private:
    std::uint64_t runs_ = 0;
    const std::string identity_ = std::string(70000, 'i');
};

} // namespace q7_fixture
