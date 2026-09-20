// Pine-free native example: the ownership and execution seams.
//
// Where native_market_strategy.cpp is the minimum host, this one exercises the
// public request vocabulary a Pine strategy would reach through
// `strategy.entry` / `strategy.exit` / `strategy.close`:
//
//   * a host-sized opening behind a limit trigger (`HostSized` +
//     `resolve_execution_terms`);
//   * a child stop bound to that opening's cycle (`BindOpening`), the native
//     spelling of a bracket leg;
//   * a close selected against exactly one opening (`BindOpenings`), executed
//     at the current point after a readiness preview;
//   * an exact reversal to a signed target (`ReverseTo`).
//
// Like its sibling it is both a loadable module and a standalone program, and
// contains no Pine command call, formula, or protected engine write.

#include <pineforge/native_module.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <variant>

namespace {

namespace no = pineforge::native_order;

// Not `final`: PINEFORGE_EXPORT_NATIVE_STRATEGY derives the module class from
// this one.
class NativeSelectedExample : public pineforge::NativeStrategyHost {
    std::optional<no::RequestHandle> opening_;
    std::optional<no::RequestHandle> reversal_;
    std::optional<no::RequestHandle> selected_;
    std::optional<std::int64_t> opening_cycle_;
    int bars_ = 0;
    bool child_submitted_ = false;

    struct AppliedMetrics {
        int host_sized_applied = 0;
        int reversal_applied = 0;
        int selected_applied = 0;
        double host_sized_opened_units = 0.0;
        double reversal_opened_units = 0.0;
        double selected_ticket = 0.0;
    };

    // The sizing seam. An unresolved HostSized request asks the host for its
    // units; returning none leaves it unresolved and fails the run.
    no::ExecutionTerms resolve_execution_terms(
            const pineforge::NativeExecutionTermsFacts& facts) const override {
        if (std::holds_alternative<no::RemainingDeferred>(facts.remaining)) {
            return {facts.default_resolved_price, 2.0, no::OpeningShape::Transact};
        }
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }

    pineforge::NativePrecommitVerdict validate_execution_precommit(
            const pineforge::NativePrecommitView&) const override {
        return pineforge::NativePrecommitVerdict::Proceed;
    }

    void on_native_run_begin() override {
        opening_.reset();
        reversal_.reset();
        selected_.reset();
        opening_cycle_.reset();
        bars_ = 0;
        child_submitted_ = false;
    }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit(no::Request{no::Transact{1.0}, "baseline-open", ""});
            no::Request open;
            open.intent = no::HostSized{no::HostSizedKind::Open, no::Side::Long};
            open.label = "sized-limit";
            open.trigger = no::Limit{101.0};
            opening_ = submit(open).handle;
            return;
        }

        if (!child_submitted_ && opening_ && opening_cycle_) {
            no::Request child;
            child.intent = no::Reduce{no::OwnerOpenedUnits{}};
            child.label = "bound-stop";
            child.trigger = no::Stop{99.0};
            child.owner = no::BindOpening{*opening_, *opening_cycle_};
            submit(child);
            child_submitted_ = true;
        }

        if (bars_ == 3 && opening_ && opening_cycle_ && !selected_) {
            no::Request selected{no::Flatten{}, "selected-flatten", ""};
            selected.owner = no::BindOpenings{{*opening_}, *opening_cycle_};
            const auto accepted = submit(selected);
            selected_ = accepted.handle;
            if (selected_) {
                const pineforge::NativeCurrentExecution current{
                    *selected_, pineforge::NativeCurrentPriceRule::NearestTick};
                const auto preview = inspect_current_execution(current);
                if (!preview.refusal
                    && preview.settlement_readiness == pineforge::execution::Status::Applied) {
                    execute_current(current);
                }
            }
        }

        if (bars_ == 4 && !reversal_) {
            no::Request reverse;
            reverse.intent = no::ReverseTo{-1.0};
            reverse.label = "exact-reverse";
            reversal_ = submit(reverse).handle;
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (opening_ && event.handle() == *opening_) {
            opening_cycle_ = event.cycle_after;
        }
    }

    AppliedMetrics applied_metrics() const {
        AppliedMetrics result;
        for (const auto& event : native_events(0)) {
            if (!event.command) continue;
            const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*event.command);
            if (!applied) continue;
            if (opening_ && applied->handle() == *opening_) {
                ++result.host_sized_applied;
                result.host_sized_opened_units = applied->opened_units;
            }
            if (reversal_ && applied->handle() == *reversal_) {
                ++result.reversal_applied;
                result.reversal_opened_units = applied->opened_units;
            }
            if (selected_ && applied->handle() == *selected_) {
                ++result.selected_applied;
                result.selected_ticket = applied->current_ticket;
            }
        }
        return result;
    }

public:
    int host_sized_applied() const { return applied_metrics().host_sized_applied; }
    int reversal_applied() const { return applied_metrics().reversal_applied; }
    int selected_applied() const { return applied_metrics().selected_applied; }
    double host_sized_opened_units() const { return applied_metrics().host_sized_opened_units; }
    double reversal_opened_units() const { return applied_metrics().reversal_opened_units; }
    double selected_ticket() const { return applied_metrics().selected_ticket; }

    int reversal_terminal_kind() const {
        if (!reversal_) return -1;
        int observed = 0;
        for (const auto& event : native_events(0)) {
            if (!event.command) continue;
            const auto* command = &*event.command;
            if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(command)) {
                if (applied->handle() == *reversal_) return 1;
            }
            if (const auto* rejected = std::get_if<no::MatchRejectedEvent>(command)) {
                if (rejected->handle() == *reversal_) {
                    return 100 + static_cast<int>(rejected->reason);
                }
            }
            if (const auto* cancelled = std::get_if<no::CancelledEvent>(command)) {
                if (cancelled->handle() == *reversal_) return 200 + static_cast<int>(cancelled->reason);
            }
            if (const auto* no_effect = std::get_if<no::NoEffectEvent>(command)) {
                if (no_effect->handle() == *reversal_) return 300;
            }
            if (const auto* accepted = std::get_if<no::AcceptedEvent>(command)) {
                if (accepted->handle() == *reversal_) observed = 400;
            }
        }
        return observed;
    }
};

NativeSelectedExample* as_example(pf_strategy_t strategy) {
    return strategy
        ? dynamic_cast<NativeSelectedExample*>(
              pineforge::native_module::as_engine(strategy))
        : nullptr;
}

int lifecycle_kind(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? static_cast<int>(example->native_state().kind) : -1;
}

int failure_code(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? static_cast<int>(example->native_state().failure.code) : -1;
}

std::uint32_t failure_discriminator(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->native_state().failure.discriminator : 0;
}

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-selected-example";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "SELECTED";
    spec.tickerid = "EXCHANGE:SELECTED";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native-selected-example";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 1.0;
    spec.close_execution = pineforge::NativeCloseExecution::NextEligiblePoint;
    spec.allowed_open_directions = pineforge::NativeOpenDirections::Both;
    return spec;
}

const pineforge::Bar kBars[] = {
    {100.0, 102.0, 100.0, 101.0, 1.0, 0},
    {102.0, 103.0, 101.0, 102.5, 1.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 1.0, 600000},
    {104.0, 105.0, 103.0, 104.5, 1.0, 900000},
    {105.0, 106.0, 104.0, 105.5, 1.0, 1200000},
};
constexpr int kBarCount = 5;

bool completed(const NativeSelectedExample& host, const char* where) {
    if (host.native_state().kind == pineforge::NativeLifecycleKind::Completed) return true;
    std::cerr << where << ": lifecycle="
              << static_cast<int>(host.native_state().kind)
              << " code=" << static_cast<int>(host.native_state().failure.code)
              << " " << host.last_error() << '\n';
    return false;
}

}  // namespace

PINEFORGE_EXPORT_NATIVE_STRATEGY(NativeSelectedExample);

extern "C" {

// Example-specific observation symbols kept outside the macro: the C ABI test
// reads the host's own counters through them.
PF_API int native_selected_example_host_epoch() {
    (void)pf_abi_version();
    return 15;
}

PF_API int native_selected_example_lifecycle(pf_strategy_t strategy) {
    return lifecycle_kind(strategy);
}

PF_API int native_selected_example_failure_code(pf_strategy_t strategy) {
    return failure_code(strategy);
}

PF_API std::uint32_t native_selected_example_failure_discriminator(pf_strategy_t strategy) {
    return failure_discriminator(strategy);
}

PF_API int native_selected_example_host_sized_applied(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->host_sized_applied() : -1;
}

PF_API double native_selected_example_host_sized_opened_units(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->host_sized_opened_units() : 0.0;
}

PF_API int native_selected_example_reversal_applied(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->reversal_applied() : -1;
}

PF_API double native_selected_example_reversal_opened_units(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->reversal_opened_units() : 0.0;
}

PF_API int native_selected_example_reversal_terminal_kind(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->reversal_terminal_kind() : -1;
}

PF_API int native_selected_example_selected_applied(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->selected_applied() : -1;
}

PF_API double native_selected_example_selected_ticket(pf_strategy_t strategy) {
    const auto* example = as_example(strategy);
    return example ? example->selected_ticket() : 0.0;
}

}  // extern "C"

int main() {
    const auto spec = make_spec();

    NativeSelectedExample batch;
    if (batch.configure_native(spec).status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << batch.last_error() << '\n';
        return 1;
    }
    batch.run(kBars, kBarCount, "5", "5");
    if (!completed(batch, "run(tf)")) return 1;

    NativeSelectedExample stream;
    if (stream.configure_native(spec).status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure(stream): " << stream.last_error() << '\n';
        return 1;
    }
    if (!stream.stream_begin(kBars, 1, "5", "5")) {
        std::cerr << "stream_begin: " << stream.last_error() << '\n';
        return 1;
    }
    for (int i = 1; i < kBarCount; ++i) {
        if (!stream.stream_push_bar(kBars[i])) {
            std::cerr << "stream_push_bar: " << stream.last_error() << '\n';
            return 1;
        }
    }
    if (!stream.stream_end()) {
        std::cerr << "stream_end: " << stream.last_error() << '\n';
        return 1;
    }
    if (!completed(stream, "stream")) return 1;

    std::cout << "host-sized applied: " << batch.host_sized_applied()
              << " units=" << batch.host_sized_opened_units()
              << "  selected-close applied: " << batch.selected_applied()
              << "  closed trades: " << batch.trade_count() << '\n';
    return 0;
}
