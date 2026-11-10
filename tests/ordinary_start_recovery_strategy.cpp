#include "metrics_only_strategy_fixture.hpp"

#include <cstdio>
#include <stdexcept>

namespace ordinary_recovery_input {

class RecoveryInputHost : public metrics_only_fixture::TradingHost {
public:
    RecoveryInputHost() = default;

    ~RecoveryInputHost() noexcept override {
        // Observe while the native engine is still alive, using the fixture's
        // production getter pattern. This diagnostic never supplies a report.
        pineforge::metrics::MetricsExtentReceiptV1 receipt{};
        const auto status = strategy_metrics_extent_receipt_v1(
            static_cast<const pineforge::BacktestEngine*>(this), &receipt);
        std::fprintf(stderr,
            "ORDINARY_RECOVERY_NATIVE_RECEIPT={\"status\":%d,\"route\":%u,\"reason\":%u,"
            "\"mode\":%u,\"mask\":%llu,\"plain\":%u,\"failed\":%llu,"
            "\"witness\":%u,\"mismatch\":%llu,\"planned\":%u,"
            "\"count\":%lld,\"first\":%lld,\"last\":%lld,"
            "\"stop_kind\":%u,\"completed_marks\":%lld,\"stop_input_index\":%lld,"
            "\"replays\":%u,\"attempt\":%llu,\"planned_attempt\":%llu,"
            "\"authoritative_attempt\":%llu}\n",
            static_cast<int>(status), static_cast<unsigned>(receipt.route),
            static_cast<unsigned>(receipt.route_reason),
            static_cast<unsigned>(receipt.streaming_sharpe),
            static_cast<unsigned long long>(receipt.metrics_needed),
            static_cast<unsigned>(receipt.plain_subset),
            static_cast<unsigned long long>(receipt.failed_plain_inputs),
            static_cast<unsigned>(receipt.witness),
            static_cast<unsigned long long>(receipt.mismatch_fields),
            static_cast<unsigned>(receipt.extent_plan.present),
            static_cast<long long>(receipt.actual_extent.count),
            static_cast<long long>(receipt.actual_extent.first_time_ms),
            static_cast<long long>(receipt.actual_extent.last_time_ms),
            static_cast<unsigned>(receipt.stop_boundary.kind),
            static_cast<long long>(receipt.stop_boundary.completed_marks),
            static_cast<long long>(receipt.stop_boundary.input_index),
            static_cast<unsigned>(receipt.replay_attempts),
            static_cast<unsigned long long>(receipt.actual_attempt),
            static_cast<unsigned long long>(receipt.extent_plan.attempt),
            static_cast<unsigned long long>(receipt.authoritative_attempt));
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext& context) override {
        TradingHost::on_native_bar(bar, context);
        if (bar.close < bar.open)
            throw std::runtime_error("recovery input close is below open");
    }
};

} // namespace ordinary_recovery_input

namespace pineforge::native_module {
template <>
struct MetricsPlainHostV1<ordinary_recovery_input::RecoveryInputHost> {
    static std::optional<MetricsPlainDeclarationV1> declaration() {
        MetricsPlainDeclarationV1 out;
        out.builtin_presenter = out.builtin_marks = out.builtin_scheduler = 1;
        // TradingHost retains the inherited presenter and bar-close scheduler,
        // with one builtin mark per input and no anchor. The derived callback
        // calls that implementation before its data-dependent exception; it
        // adds no security, lower-timeframe, magnifier or history-tick sites.
        // The only strategy state is TradingHost's begin-reset bucket. Frozen
        // inputs/settings reproduce both trading and the exception on a fresh
        // handle; this declaration makes no claim that a run will succeed.
        return out;
    }
};
} // namespace pineforge::native_module

PINEFORGE_EXPORT_NATIVE_STRATEGY(ordinary_recovery_input::RecoveryInputHost);
