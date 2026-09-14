// R4-D L1 native-only witnesses.  This TU deliberately exercises only the
// generic host/request/driver surface; no source host or source command route
// participates in its expected values.
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V17
#error "L1 requires the v17 native host surface"
#endif

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                         \
    ++checks;                                                                    \
    if (!(expr)) {                                                               \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);            \
    }                                                                            \
} while (0)

void near(double actual, double expected) {
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) <= 1e-10 * std::max(1.0, std::abs(expected)));
}

constexpr std::int64_t kT = 1736121600000LL;

Bar bar(std::int64_t timestamp, double open = 100.0, double high = 100.0,
        double low = 100.0, double close = 100.0) {
    return {open, high, low, close, 1.0, timestamp};
}

NativeRunSpec spec_for(const char* key, std::uint64_t run = 1,
                       const char* input_tf = "1", const char* script_tf = "1") {
    NativeRunSpec spec;
    spec.identity = {key, run};
    spec.input_tf = input_tf;
    spec.script_tf = script_tf;
    spec.tickerid = "L1:TEST";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 6.0;
    return spec;
}

no::Request market(double units, const char* label) {
    no::Request request;
    request.intent = no::Transact{units};
    request.label = label;
    return request;
}

no::Request cohort_close(no::CohortHandle cohort, const char* label,
                         std::optional<double> limit = std::nullopt) {
    no::Request request;
    request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
    request.owner = no::BindCohort{cohort};
    request.label = label;
    if (limit) request.trigger = no::Limit{*limit};
    return request;
}

std::optional<no::ExecutionAppliedEvent> applied_with_label(
        const NativeStrategyHost& host, const char* label) {
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* event = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            if (event->request().label == label) return *event;
        }
    }
    return std::nullopt;
}

int terminal_events_with_label(const NativeStrategyHost& host, const char* label) {
    int result = 0;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        std::visit([&](const auto& event) {
            if constexpr (std::is_same_v<std::decay_t<decltype(event)>, no::NoEffectEvent>
                          || std::is_same_v<std::decay_t<decltype(event)>, no::MatchRejectedEvent>
                          || std::is_same_v<std::decay_t<decltype(event)>, no::CancelledEvent>) {
                if (event.request().label == label) ++result;
            }
        }, *row.command);
    }
    return result;
}

class TermsHost : public NativeStrategyHost {
public:
    int bars = 0;
    int terms_calls = 0;
    int validators = 0;
    std::vector<NativeExecutionTermsFacts> terms;
    std::vector<NativeDecisionContext> contexts;

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        ++bars;
        contexts.push_back(context);
    }
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        auto& self = const_cast<TermsHost&>(*this);
        ++self.terms_calls;
        self.terms.push_back(facts);
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return {facts.default_resolved_price, facts.scope_exposure_units * 0.5,
                    no::OpeningShape::Transact};
        }
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView&) const override {
        ++const_cast<TermsHost&>(*this).validators;
        return NativePrecommitVerdict::Proceed;
    }
protected:
    void poison_next_cycle() { next_position_cycle_seq_ = std::numeric_limits<std::int64_t>::max(); }
};

class NoTargetHost final : public TermsHost {
public:
    no::RequestHandle exit{};
    void on_native_run_begin() override {
        const auto cohort = cohort_open();
        const auto result = submit(cohort_close(cohort, "no-target"));
        CHECK(result.status == no::SubmitStatus::Accepted);
        CHECK(result.handle.has_value());
        if (result.handle) exit = *result.handle;
    }
};

class ReplacementHost final : public TermsHost {
public:
    no::CohortHandle cohort{};
    no::RequestHandle entry{};
    bool replaced = false;
    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        if (bars == 1) {
            cohort = cohort_open();
            auto request = market(1.0, "replace-entry");
            request.trigger = no::Limit{50.0};
            const auto result = submit(request);
            CHECK(result.handle.has_value());
            if (result.handle) {
                entry = *result.handle;
                cohort_add(cohort, entry);
            }
            CHECK(submit(cohort_close(cohort, "replace-close", 110.0)).handle.has_value());
        } else if (bars == 2 && !replaced) {
            const auto result = replace(entry, market(2.0, "replace-entry"));
            CHECK(result.status == no::ReplaceStatus::Replaced);
            CHECK(result.successor.has_value());
            if (result.successor) cohort_add(cohort, *result.successor);
            replaced = true;
        }
    }
};

class CohortHost final : public TermsHost {
public:
    no::CohortHandle cohort{};
    no::RequestHandle e1{};
    no::RequestHandle e2{};
    no::RequestHandle unrelated{};

    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        if (bars == 1) {
            cohort = cohort_open();
            const auto result = submit(market(1.0, "cohort-e1"));
            CHECK(result.handle.has_value());
            if (result.handle) {
                e1 = *result.handle;
                cohort_add(cohort, e1);
            }
        } else if (bars == 2) {
            const auto u = submit(market(7.0, "unrelated"));
            const auto second = submit(market(2.0, "cohort-e2"));
            CHECK(u.handle.has_value());
            CHECK(second.handle.has_value());
            if (u.handle) unrelated = *u.handle;
            if (second.handle) {
                e2 = *second.handle;
                cohort_add(cohort, e2);
            }
            CHECK(submit(cohort_close(cohort, "cohort-close", 110.0)).handle.has_value());
        }
    }
};

class PermutationHost final : public TermsHost {
public:
    explicit PermutationHost(bool reverse) : reverse_(reverse) {}
    no::CohortHandle cohort{};
    no::RequestHandle first{};
    no::RequestHandle second{};
    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        if (bars != 1) return;
        cohort = cohort_open();
        const auto a = submit(market(1.0, "perm-a"));
        const auto b = submit(market(2.0, "perm-b"));
        CHECK(a.handle.has_value());
        CHECK(b.handle.has_value());
        if (!a.handle || !b.handle) return;
        first = *a.handle;
        second = *b.handle;
        if (reverse_) {
            cohort_add(cohort, second);
            cohort_add(cohort, first);
        } else {
            cohort_add(cohort, first);
            cohort_add(cohort, second);
        }
        CHECK(submit(cohort_close(cohort, "perm-close", 110.0)).handle.has_value());
    }
private:
    bool reverse_ = false;
};

class OpenHookHost final : public TermsHost {
public:
    int opens = 0;
    int executed = 0;

    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        if (bars != 1) return;
        no::Request stop;
        stop.intent = no::Reduce{no::ExplicitUnits{1.0}};
        stop.trigger = no::Stop{95.0};
        stop.label = "resting-stop";
        CHECK(submit(stop).handle.has_value());
    }
    void on_native_run_begin() override {
        CHECK(submit(market(1.0, "open-position")).handle.has_value());
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        ++opens;
        if (opens == 2) {
            no::Request reduce;
            reduce.intent = no::Reduce{no::ExplicitUnits{1.0}};
            reduce.label = "open-reduce";
            const auto submitted = submit(reduce);
            CHECK(submitted.handle.has_value());
            if (!submitted.handle) return;
            const auto result = execute_current({*submitted.handle, NativeCurrentPriceRule::AsPresented});
            CHECK(std::holds_alternative<no::ExecutionAppliedEvent>(result));
            if (std::holds_alternative<no::ExecutionAppliedEvent>(result)) ++executed;
        }
    }
};

class ProviderHost : public TermsHost {
public:
    int prepares = 0;
    int last_n = 0;
    int last_warmup_n = 0;
    bool last_had_bars = false;
    std::string last_input_tf;
    std::string last_script_tf;
    bool copy_intrabar = false;
    bool saw_stream = false;
    std::vector<double> active_fx;

    void prepare_native_begin(const NativeBeginArgs& args) override {
        ++prepares;
        last_n = args.n;
        last_warmup_n = args.warmup_n;
        last_had_bars = args.bars != nullptr;
        last_input_tf = args.input_tf;
        last_script_tf = args.script_tf;
        saw_stream = args.is_stream;
        const std::string input = args.input_tf.empty() ? "1" : args.input_tf;
        const std::string script = args.script_tf.empty() ? input : args.script_tf;
        NativeRunSpec configured = spec_for("provider", 1, input.c_str(), script.c_str());
        if (copy_intrabar && args.bar_magnifier) {
            IntrabarPath::lower_tf lower;
            lower.tf = input;
            lower.samples = args.magnifier_samples;
            lower.distribution = args.magnifier_distribution;
            lower.volume_weighted = args.magnifier_volume_weighted;
            lower.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
            lower.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
            if (args.bars && args.n > 0) lower.bars.assign(args.bars, args.bars + args.n);
            configured.intrabar.value = std::move(lower);
        }
        CHECK(configure_native(configured).status == NativeSetupStatus::Applied);
    }
    void on_native_run_begin() override {
        CHECK(submit(market(1.0, "provider-fx")).handle.has_value());
    }
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        auto& self = const_cast<ProviderHost&>(*this);
        self.active_fx.push_back(facts.active_fx);
        return TermsHost::resolve_execution_terms(facts);
    }
};

class PathHost final : public ProviderHost {
public:
    std::vector<NativeDecisionContext> applied_contexts;
    std::optional<no::ExecutionAppliedEvent> applied;
    void on_native_run_begin() override {
        no::Request limit = market(1.0, "intrabar-limit");
        limit.trigger = no::Limit{95.0};
        CHECK(submit(limit).handle.has_value());
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        if (event.request().label == "intrabar-limit") {
            applied = event;
            applied_contexts.push_back(context);
        }
    }
};

class OverflowHost final : public TermsHost {
public:
    bool reached = false;
    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        if (reached) return;
        poison_next_cycle();
        const auto result = submit(market(1.0, "overflow"));
        CHECK(result.handle.has_value());
        bool threw = false;
        try {
            (void)execute_current({*result.handle, NativeCurrentPriceRule::AsPresented});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(native_state().kind == NativeLifecycleKind::Failed);
        CHECK(native_state().failure.code == NativeFailureCode::SettlementFailure);
        CHECK(validators == 0);
        CHECK(physical_position().lot_count == 0);
        reached = true;
    }
};

void no_target_witness() {
    NoTargetHost host;
    CHECK(host.configure_native(spec_for("no-target")).status == NativeSetupStatus::Applied);
    const Bar bars[] = {bar(kT), bar(kT + 60000)};
    host.run(bars, 2);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.terms_calls == 0);
    CHECK(!applied_with_label(host, "no-target").has_value());
    CHECK(terminal_events_with_label(host, "no-target") == 0);
    CHECK(host.native_continuation_hash() != 0);
}

void replacement_growth_witness() {
    ReplacementHost host;
    CHECK(host.configure_native(spec_for("replacement-growth")).status == NativeSetupStatus::Applied);
    const Bar bars[] = {
        bar(kT), bar(kT + 60000), bar(kT + 120000),
        bar(kT + 180000, 100, 110, 100, 100),
    };
    host.run(bars, 4);
    const auto exit = applied_with_label(host, "replace-close");
    CHECK(exit.has_value());
    if (exit) {
        near(exit->closed_units, 1.0); // default host resolver closes 50% of the grown 2-unit entry
        CHECK(std::holds_alternative<no::SelectedExposure>(exit->scope));
    }
    CHECK(!host.terms.empty());
    if (!host.terms.empty()) near(host.terms.back().scope_exposure_units, 2.0);
}

void cohort_percent_and_fifo_witness() {
    CohortHost host;
    CHECK(host.configure_native(spec_for("cohort-percent")).status == NativeSetupStatus::Applied);
    const Bar bars[] = {
        bar(kT), bar(kT + 60000), bar(kT + 120000),
        bar(kT + 180000, 100, 110, 100, 100),
    };
    host.run(bars, 4);
    const auto exit = applied_with_label(host, "cohort-close");
    CHECK(exit.has_value());
    if (exit) {
        near(exit->closed_units, 1.5);
        near(exit->current_ticket, 6.0); // one selected close, one CashPerExecution ticket
        const auto* selected = std::get_if<no::SelectedExposure>(&exit->scope);
        CHECK(selected != nullptr);
        if (selected) {
            CHECK(selected->incarnations.size() == 2);
            CHECK(std::find(selected->incarnations.begin(), selected->incarnations.end(),
                            host.unrelated.incarnation) == selected->incarnations.end());
        }
    }
    CHECK(!host.terms.empty());
    if (!host.terms.empty()) near(host.terms.back().scope_exposure_units, 3.0);
    near(host.physical_position().signed_units, 8.5); // unrelated 7 remains untouched
}

void membership_permutation_witness() {
    const Bar bars[] = {
        bar(kT), bar(kT + 60000), bar(kT + 120000, 100, 110, 100, 100),
    };
    PermutationHost ordered(false);
    PermutationHost reversed(true);
    CHECK(ordered.configure_native(spec_for("perm-ordered")).status == NativeSetupStatus::Applied);
    CHECK(reversed.configure_native(spec_for("perm-reversed")).status == NativeSetupStatus::Applied);
    ordered.run(bars, 3);
    reversed.run(bars, 3);
    const auto left = applied_with_label(ordered, "perm-close");
    const auto right = applied_with_label(reversed, "perm-close");
    CHECK(left.has_value() && right.has_value());
    if (left && right) {
        near(left->closed_units, right->closed_units);
        const auto* a = std::get_if<no::SelectedExposure>(&left->scope);
        const auto* b = std::get_if<no::SelectedExposure>(&right->scope);
        CHECK(a != nullptr && b != nullptr);
        if (a && b) CHECK(a->incarnations == b->incarnations);
    }
}

void pre_open_witness() {
    OpenHookHost host;
    CHECK(host.configure_native(spec_for("pre-open")).status == NativeSetupStatus::Applied);
    const Bar bars[] = {bar(kT), bar(kT + 60000, 90, 90, 90, 90)};
    host.run(bars, 2);
    CHECK(host.opens == 2);
    CHECK(host.executed == 1);
    CHECK(applied_with_label(host, "open-reduce").has_value());
    CHECK(!applied_with_label(host, "resting-stop").has_value());
    CHECK(host.physical_position().lot_count == 0);
}

void intrabar_path_witness() {
    PathHost host;
    host.copy_intrabar = true;
    const Bar bars[] = {
        bar(kT, 100, 101, 99, 100),
        bar(kT + 60000, 100, 101, 94, 100),
        bar(kT + 120000, 100, 102, 98, 101),
        bar(kT + 180000, 101, 103, 100, 102),
    };
    host.run(bars, 4, "1", "4", true, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.prepares == 1);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.bars == 1);
    CHECK(host.contexts.size() == 1);
    if (!host.contexts.empty()) {
        const auto& context = host.contexts.front();
        CHECK(context.sub_index == 3);
        CHECK(context.sub_count == 4);
        CHECK(context.is_terminal_sub_bar);
        CHECK(context.script_bar_open_ms == kT);
        CHECK(context.sub_bar_open_ms == kT + 180000);
    }
    CHECK(host.applied.has_value());
    if (host.applied) {
        CHECK(host.applied->cursor.point.effective_time_ms == kT + 60000);
        near(host.applied->resolved_price, 95.0);
    }
    std::vector<std::int64_t> opens;
    for (const auto& row : host.native_events(0)) {
        if (row.driver && row.driver->coordinate.provenance == NativePriceProvenance::ModeledOHLCOpen) {
            opens.push_back(row.driver->coordinate.effective_time_ms);
        }
    }
    CHECK((opens == std::vector<std::int64_t>{kT, kT + 60000, kT + 120000, kT + 180000}));
}

void provider_and_staged_fx_witness() {
    ProviderHost stream_host;
    const pf_bar_t warmup{100, 100, 100, 100, 1, kT};
    const int stream_begin = strategy_stream_begin(
        reinterpret_cast<pf_strategy_t>(&stream_host), &warmup, 1, "1", "1");
    if (stream_begin != 0) std::printf("stream begin error: %s\n", stream_host.last_error().c_str());
    CHECK(stream_begin == 0);
    CHECK(stream_host.prepares == 1);
    CHECK(stream_host.saw_stream);
    CHECK(stream_host.last_warmup_n == 1);
    CHECK(stream_host.last_had_bars);
    CHECK(stream_host.native_state().kind == NativeLifecycleKind::Running);
    CHECK(strategy_stream_end(reinterpret_cast<pf_strategy_t>(&stream_host), 0) == 0);

    ProviderHost host;
    const std::int64_t times[] = {kT - 60000, kT};
    const double rates[] = {1.0, 2.0};
    CHECK(strategy_set_account_currency_fx_series(reinterpret_cast<pf_strategy_t>(&host), times, rates, 2)
          == 0);
    const Bar fx_bars[] = {bar(kT)};
    host.run(fx_bars, 1, "1", "1");
    CHECK(host.prepares == 1);
    CHECK(!host.saw_stream);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(!host.active_fx.empty());
    if (!host.active_fx.empty()) near(host.active_fx.front(), 2.0);
    CHECK(strategy_set_account_currency_fx_series(reinterpret_cast<pf_strategy_t>(&host), times, rates, 2)
          == -1);

    ProviderHost run_host;
    const Bar bars[] = {bar(kT)};
    run_host.run(bars, 1, "1", "1");
    CHECK(run_host.prepares == 1);
    CHECK(run_host.last_input_tf == "1");
    CHECK(run_host.last_script_tf == "1");
    CHECK(run_host.native_state().kind == NativeLifecycleKind::Completed);
}

void precommit_overflow_witness() {
    OverflowHost host;
    CHECK(host.configure_native(spec_for("precommit-overflow")).status == NativeSetupStatus::Applied);
    const Bar bars[] = {bar(kT)};
    host.run(bars, 1);
    CHECK(host.reached);
}

}  // namespace

int main() {
    no_target_witness();
    replacement_growth_witness();
    cohort_percent_and_fifo_witness();
    membership_permutation_witness();
    pre_open_witness();
    intrabar_path_witness();
    provider_and_staged_fx_witness();
    precommit_overflow_witness();
    std::printf("R4-D L1 native lowering: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
