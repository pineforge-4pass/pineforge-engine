#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace {

namespace no = pineforge::native_order;

static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar),
              "pf_bar_t / pineforge::Bar size mismatch");
static_assert(offsetof(pf_bar_t, open) == offsetof(pineforge::Bar, open),
              "pf_bar_t::open offset mismatch");
static_assert(offsetof(pf_bar_t, high) == offsetof(pineforge::Bar, high),
              "pf_bar_t::high offset mismatch");
static_assert(offsetof(pf_bar_t, low) == offsetof(pineforge::Bar, low),
              "pf_bar_t::low offset mismatch");
static_assert(offsetof(pf_bar_t, close) == offsetof(pineforge::Bar, close),
              "pf_bar_t::close offset mismatch");
static_assert(offsetof(pf_bar_t, volume) == offsetof(pineforge::Bar, volume),
              "pf_bar_t::volume offset mismatch");
static_assert(offsetof(pf_bar_t, timestamp) == offsetof(pineforge::Bar, timestamp),
              "pf_bar_t::timestamp offset mismatch");
static_assert(sizeof(pf_report_t) == sizeof(pineforge::ReportC),
              "pf_report_t / pineforge::ReportC size mismatch");

class NativeSelectedExample final : public pineforge::NativeStrategyHost {
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

pineforge::BacktestEngine* as_engine(pf_strategy_t strategy) {
    return static_cast<pineforge::BacktestEngine*>(strategy);
}

NativeSelectedExample* as_example(pf_strategy_t strategy) {
    return strategy ? dynamic_cast<NativeSelectedExample*>(as_engine(strategy)) : nullptr;
}

void run_c_batch(pf_strategy_t strategy, pf_bar_t* bars, int count,
                 const char* input_tf, const char* script_tf, bool with_timeframes,
                 pf_report_t* report) {
    if (!strategy) return;
    try {
        std::vector<pineforge::Bar> copied;
        const pineforge::Bar* source = nullptr;
        if (count > 0 && bars != nullptr) {
            copied.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const auto& bar = bars[static_cast<std::size_t>(i)];
                copied.push_back({bar.open, bar.high, bar.low, bar.close, bar.volume,
                                  bar.timestamp});
            }
            source = copied.data();
        }
        auto* engine = as_engine(strategy);
        if (with_timeframes) {
            engine->run(source, count, input_tf ? input_tf : "",
                        script_tf ? script_tf : "");
        } else {
            engine->run(source, count);
        }
        if (report) engine->fill_report(reinterpret_cast<pineforge::ReportC*>(report));
    } catch (...) {
    }
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

}  // namespace

extern "C" {

PF_API pf_strategy_t strategy_create(const char*) {
    try {
        return static_cast<pf_strategy_t>(new NativeSelectedExample());
    } catch (...) {
        return nullptr;
    }
}

PF_API void strategy_free(pf_strategy_t strategy) {
    delete as_engine(strategy);
}

PF_API void strategy_set_input(pf_strategy_t, const char*, const char*) {}
PF_API void strategy_set_override(pf_strategy_t, const char*, const char*) {}
PF_API void strategy_set_magnifier_volume_weighted(pf_strategy_t, int) {}

PF_API void run_backtest(pf_strategy_t strategy, pf_bar_t* bars, int count,
                         pf_report_t* report) {
    run_c_batch(strategy, bars, count, nullptr, nullptr, false, report);
}

PF_API void run_backtest_full(pf_strategy_t strategy, pf_bar_t* bars, int count,
                              const char* input_tf, const char* script_tf,
                              int bar_magnifier, int magnifier_samples,
                              pf_magnifier_distribution_t magnifier_distribution,
                              pf_report_t* report) {
    if (bar_magnifier != 0 || magnifier_samples != 4
        || magnifier_distribution != PF_MAGNIFIER_ENDPOINTS) {
        return;
    }
    run_c_batch(strategy, bars, count, input_tf, script_tf, true, report);
}

PF_API void report_free(pf_report_t* report) {
    if (report) pineforge::BacktestEngine::free_report(
        reinterpret_cast<pineforge::ReportC*>(report));
}

// Retain the runtime C ABI object while reporting the public host epoch.
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
