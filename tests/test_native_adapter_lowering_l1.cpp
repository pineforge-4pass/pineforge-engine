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

#ifndef PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19
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
    bool saw_inputs = false;
    bool saw_syminfo = false;
    bool saw_overrides = false;
    std::string last_tickerid;
    std::string callback_tickerid;
    std::vector<double> active_fx;
    bool synthesize_intrabar = false;
    bool legacy_tolerant_intrabar = false;
    IntrabarPath::SampleEligibility sample_eligibility =
        IntrabarPath::SampleEligibility::ContinuousSegments;

    void prepare_native_begin(const NativeBeginArgs& args) override {
        ++prepares;
        last_n = args.n;
        last_warmup_n = args.warmup_n;
        last_had_bars = args.bars != nullptr;
        last_input_tf = args.input_tf;
        last_script_tf = args.script_tf;
        saw_stream = args.is_stream;
        saw_inputs = args.inputs != nullptr;
        saw_syminfo = args.syminfo != nullptr;
        saw_overrides = args.overrides_opaque != nullptr;
        const std::string input = args.input_tf.empty() ? "1" : args.input_tf;
        const std::string script = args.script_tf.empty() ? input : args.script_tf;
        NativeRunSpec configured = spec_for(
            "provider", native_consumed_high_water() + 1, input.c_str(), script.c_str());
        if (args.syminfo) {
            last_tickerid = args.syminfo->tickerid;
            // The provider owns the retained value.  These assignments model
            // the source rich-run projection without retaining the borrowed
            // pointer after prepare_native_begin returns.
            configured.ticker = args.syminfo->ticker;
            configured.tickerid = args.syminfo->tickerid;
            configured.type = args.syminfo->type;
            configured.currency = args.syminfo->currency;
            configured.basecurrency = args.syminfo->basecurrency;
            configured.description = args.syminfo->description;
            configured.volumetype = args.syminfo->volumetype;
            configured.timezone = args.syminfo->timezone;
            configured.session = args.syminfo->session;
            configured.point_value = args.syminfo->pointvalue;
            configured.price_tick = args.syminfo->mintick;
        }
        if (args.n < 2 && args.input_tf.empty() && args.script_tf.empty()) {
            configured.input_tf.clear();
            configured.script_tf.clear();
            configured.timeframe_undetected = true;
        }
        if (legacy_tolerant_intrabar) {
            configured.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        }
        if (copy_intrabar && args.bar_magnifier) {
            if (synthesize_intrabar) {
                IntrabarPath::synthesized synthesized;
                synthesized.samples = args.magnifier_samples;
                synthesized.distribution = args.magnifier_distribution;
                synthesized.volume_weighted = args.magnifier_volume_weighted;
                synthesized.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
                synthesized.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
                configured.intrabar.value = std::move(synthesized);
            } else {
                IntrabarPath::lower_tf lower;
                lower.tf = input;
                lower.samples = args.magnifier_samples;
                lower.distribution = args.magnifier_distribution;
                lower.volume_weighted = args.magnifier_volume_weighted;
                lower.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
                lower.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
                lower.sample_eligibility = sample_eligibility;
                if (args.bars && args.n > 0) lower.bars.assign(args.bars, args.bars + args.n);
                configured.intrabar.value = std::move(lower);
            }
        }
        CHECK(configure_native(configured).status == NativeSetupStatus::Applied);
    }
    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        callback_tickerid = syminfo_.tickerid;
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

class DistributionHost final : public ProviderHost {
public:
    std::optional<no::ExecutionAppliedEvent> applied;

    void on_native_run_begin() override {
        no::Request limit = market(1.0, "distribution-coarse-stop");
        limit.trigger = no::Limit{95.0};
        const auto result = submit(limit);
        CHECK(result.status == no::SubmitStatus::Accepted);
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        if (event.request().label == "distribution-coarse-stop") applied = event;
    }
};

class IntrabarFloorHost final : public ProviderHost {
public:
    std::optional<no::ExecutionAppliedEvent> opening;
    std::optional<no::ExecutionAppliedEvent> stop;

    void on_native_run_begin() override {
        no::Request entry = market(1.0, "floor-entry-at-sub-bar");
        entry.trigger = no::Limit{99.0};
        const auto result = submit(entry);
        CHECK(result.status == no::SubmitStatus::Accepted);
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        if (event.request().label == "floor-entry-at-sub-bar") {
            opening = event;
            CHECK(context.sub_bar_open_ms == kT + 120000);
            no::Request exit;
            exit.intent = no::Reduce{no::ExplicitUnits{1.0}};
            exit.trigger = no::Stop{99.0};
            exit.label = "floor-stop-after-sub-bar";
            const auto result = submit(exit);
            CHECK(result.status == no::SubmitStatus::Accepted);
        } else if (event.request().label == "floor-stop-after-sub-bar") {
            stop = event;
        }
    }
};

class AbortRestageHost final : public ProviderHost {
public:
    bool abort_once = true;

    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        ProviderHost::on_native_bar(value, context);
        if (abort_once) {
            abort_once = false;
            request_abort();
        }
    }
};

class DuringRunStagingHost final : public ProviderHost {
public:
    bool attempted = false;
    bool refused = false;
    std::string refusal;

    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        ProviderHost::on_native_bar(value, context);
        if (attempted) return;
        attempted = true;
        const std::int64_t times[] = {kT};
        const double rates[] = {2.0};
        try {
            (void)set_account_currency_fx_series(times, rates, 1);
        } catch (const std::runtime_error& error) {
            refused = true;
            refusal = error.what();
        }
    }
};

class UndetectedTimeframeHost final : public ProviderHost {
public:
    bool ready_after_prepare = false;
    int bar_calls = 0;
    std::optional<no::RequestHandle> opening;
    std::optional<no::RequestHandle> pending_reduction;

    void prepare_native_begin(const NativeBeginArgs& args) override {
        ProviderHost::prepare_native_begin(args);
        ready_after_prepare = native_state().kind == NativeLifecycleKind::Ready;
    }
    void on_native_run_begin() override {}
    void on_native_bar(const Bar& value, const NativeDecisionContext& context) override {
        TermsHost::on_native_bar(value, context);
        if (++bar_calls != 1) return;
        const auto placed = submit(market(2.0, "undetected-opening"));
        CHECK(placed.status == no::SubmitStatus::Accepted);
        CHECK(placed.handle.has_value());
        if (!placed.handle) return;
        opening = *placed.handle;

        no::Request reduction;
        reduction.intent = no::Reduce{no::ExplicitUnits{2.0}};
        reduction.owner = no::WaitForApplied{*opening};
        reduction.label = "undetected-pending-reduction";
        const auto waiting = submit(reduction);
        CHECK(waiting.status == no::SubmitStatus::Accepted);
        CHECK(waiting.handle.has_value());
        if (waiting.handle) pending_reduction = *waiting.handle;
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

bool coarse_stop_fills_with(
        IntrabarPath::SampleEligibility eligibility, MagnifierDistribution distribution,
        NativeDecisionContext* final_context) {
    DistributionHost host;
    host.copy_intrabar = true;
    host.sample_eligibility = eligibility;
    const Bar bars[] = {
        bar(kT, 100.0, 101.0, 99.0, 100.0),
        bar(kT + 60000, 100.0, 100.5, 94.5, 96.0),
    };
    host.run(bars, 2, "1", "1", true, 4, distribution);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.contexts.size() == 2);
    if (!host.contexts.empty() && final_context) *final_context = host.contexts.back();
    return host.applied.has_value();
}

void distribution_samples_witness() {
    NativeDecisionContext continuous{};
    CHECK(coarse_stop_fills_with(
        IntrabarPath::SampleEligibility::ContinuousSegments,
        MagnifierDistribution::UNIFORM, &continuous));

    struct Expected {
        MagnifierDistribution distribution;
        bool fills;
    };
    const Expected expected[] = {
        {MagnifierDistribution::UNIFORM, false},
        {MagnifierDistribution::COSINE, true},
        {MagnifierDistribution::TRIANGLE, false},
        {MagnifierDistribution::ENDPOINTS, true},
        {MagnifierDistribution::FRONT_LOADED, false},
        {MagnifierDistribution::BACK_LOADED, false},
    };
    for (const auto& row : expected) {
        NativeDecisionContext context{};
        CHECK(coarse_stop_fills_with(
            IntrabarPath::SampleEligibility::DistributionSamples,
            row.distribution, &context) == row.fills);
        CHECK(context.driver_statistics.intrabar_path_enabled);
        CHECK(context.driver_statistics.sub_bars_per_script_bar == 1);
        CHECK(context.driver_statistics.samples_per_sub_bar == 4);
        CHECK(context.driver_statistics.sub_bars_processed == 2);
        CHECK(context.driver_statistics.sample_ticks_processed == 8);
    }

    IntrabarPath::lower_tf lower;
    lower.bars.push_back(bar(kT, 100.0, 100.5, 94.5, 96.0));
    lower.tf = "1";
    lower.samples = 4;
    lower.distribution = MagnifierDistribution::UNIFORM;
    const auto continuous_digest = native_intrabar_path_digest(
        IntrabarPath{IntrabarPath::value_type{lower}});
    lower.sample_eligibility = IntrabarPath::SampleEligibility::DistributionSamples;
    const auto sampled_digest = native_intrabar_path_digest(
        IntrabarPath{IntrabarPath::value_type{lower}});
    CHECK(continuous_digest != sampled_digest);
}

bool coarse_stop_fills_synthesized(MagnifierDistribution distribution,
                                   NativeDecisionContext* final_context) {
    DistributionHost host;
    host.copy_intrabar = true;
    host.synthesize_intrabar = true;
    host.legacy_tolerant_intrabar = true;
    const Bar bars[] = {
        bar(kT, 100.0, 101.0, 99.0, 100.0),
        bar(kT + 60000, 100.0, 100.5, 94.5, 96.0),
    };
    host.run(bars, 2, "1", "1", true, 4, distribution);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.contexts.size() == 2);
    if (!host.contexts.empty() && final_context) *final_context = host.contexts.back();
    return host.applied.has_value();
}

void synthesized_distribution_samples_witness() {
    struct Expected {
        MagnifierDistribution distribution;
        bool fills;
    };
    const Expected expected[] = {
        {MagnifierDistribution::UNIFORM, false},
        {MagnifierDistribution::COSINE, true},
        {MagnifierDistribution::TRIANGLE, false},
        {MagnifierDistribution::ENDPOINTS, true},
        {MagnifierDistribution::FRONT_LOADED, false},
        {MagnifierDistribution::BACK_LOADED, false},
    };
    for (const auto& row : expected) {
        NativeDecisionContext context{};
        CHECK(coarse_stop_fills_synthesized(row.distribution, &context) == row.fills);
        CHECK(context.driver_statistics.intrabar_path_enabled);
        CHECK(context.driver_statistics.sub_bars_per_script_bar == 1);
        CHECK(context.driver_statistics.samples_per_sub_bar == 4);
        CHECK(context.driver_statistics.sub_bars_processed == 2);
        CHECK(context.driver_statistics.sample_ticks_processed == 8);
        CHECK(context.sub_index == 0);
        CHECK(context.sub_count == 1);
        CHECK(context.is_terminal_sub_bar);
        CHECK(context.sub_bar_open_ms == kT + 60000);
        CHECK(context.script_bar_open_ms == kT + 60000);
    }

    // The retained lower-timeframe path remains the distinct input-feed mode.
    NativeDecisionContext lower_context{};
    CHECK(!coarse_stop_fills_with(
        IntrabarPath::SampleEligibility::DistributionSamples,
        MagnifierDistribution::UNIFORM, &lower_context));
    CHECK(lower_context.driver_statistics.sub_bars_processed == 2);
    CHECK(lower_context.driver_statistics.sample_ticks_processed == 8);

    IntrabarPath::synthesized synthesized;
    synthesized.samples = 4;
    synthesized.distribution = MagnifierDistribution::UNIFORM;
    const auto first_digest = native_intrabar_path_digest(
        IntrabarPath{IntrabarPath::value_type{synthesized}});
    synthesized.samples = 5;
    const auto second_digest = native_intrabar_path_digest(
        IntrabarPath{IntrabarPath::value_type{synthesized}});
    CHECK(first_digest != second_digest);
}

void intrabar_decision_floor_witness() {
    IntrabarFloorHost host;
    host.copy_intrabar = true;
    const Bar bars[] = {
        bar(kT, 100.0, 101.0, 100.0, 100.0),
        bar(kT + 60000, 100.0, 101.0, 100.0, 100.0),
        bar(kT + 120000, 100.0, 101.0, 99.0, 100.0),
        bar(kT + 180000, 100.0, 101.0, 98.0, 100.0),
    };
    host.run(bars, 4, "1", "4", true, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.opening.has_value());
    CHECK(host.stop.has_value());
    if (host.opening) {
        CHECK(host.opening->effective_time_ms() == kT + 120000);
        near(host.opening->resolved_price, 99.0);
    }
    if (host.stop) {
        CHECK(host.stop->birth().decision_time_lower_bound == kT + 120000);
        CHECK(host.stop->effective_time_ms() == kT + 120000);
        near(host.stop->resolved_price, 99.0);
    }
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
    const auto before_restage = host.native_continuation_hash();
    const std::int64_t replacement_times[] = {kT - 60000, kT};
    const double replacement_rates[] = {1.0, 3.0};
    CHECK(strategy_set_account_currency_fx_series(
              reinterpret_cast<pf_strategy_t>(&host), replacement_times, replacement_rates, 2)
          == 0);
    const auto after_restage = host.native_continuation_hash();
    CHECK(after_restage != before_restage);
    host.run(fx_bars, 1, "1", "1");
    CHECK(host.prepares == 2);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.active_fx.size() == 2);
    if (host.active_fx.size() == 2) near(host.active_fx.back(), 3.0);

    const auto before_clear = host.native_continuation_hash();
    CHECK(strategy_set_account_currency_fx_series(
              reinterpret_cast<pf_strategy_t>(&host), nullptr, nullptr, 0)
          == 0);
    const auto after_clear = host.native_continuation_hash();
    CHECK(after_clear != before_clear);
    host.run(fx_bars, 1, "1", "1");
    CHECK(host.prepares == 3);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.active_fx.size() == 3);
    if (host.active_fx.size() == 3) near(host.active_fx.back(), 1.0);

    ProviderHost run_host;
    const Bar bars[] = {bar(kT)};
    run_host.run(bars, 1, "1", "1");
    CHECK(run_host.prepares == 1);
    CHECK(run_host.last_input_tf == "1");
    CHECK(run_host.last_script_tf == "1");
    CHECK(run_host.native_state().kind == NativeLifecycleKind::Completed);
}

void aborted_run_fx_restage_witness() {
    AbortRestageHost host;
    const std::int64_t initial_times[] = {kT};
    const double initial_rates[] = {1.0};
    const Bar bars[] = {bar(kT)};
    CHECK(strategy_set_account_currency_fx_series(
              reinterpret_cast<pf_strategy_t>(&host), initial_times, initial_rates, 1)
          == 0);
    host.run(bars, 1, "1", "1");
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
    const auto before_restage = host.native_continuation_hash();

    const double replacement_rates[] = {4.0};
    CHECK(strategy_set_account_currency_fx_series(
              reinterpret_cast<pf_strategy_t>(&host), initial_times, replacement_rates, 1)
          == 0);
    CHECK(host.native_continuation_hash() != before_restage);
    host.run(bars, 1, "1", "1");
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.prepares == 2);
    CHECK(host.active_fx.size() == 2);
    if (host.active_fx.size() == 2) near(host.active_fx.back(), 4.0);
}

void during_run_fx_staging_refusal_witness() {
    DuringRunStagingHost host;
    const Bar bars[] = {bar(kT)};
    host.run(bars, 1, "1", "1");
    CHECK(host.attempted);
    CHECK(host.refused);
    CHECK(host.refusal == "native host refuses source mutation: "
                          "set_account_currency_fx_series");
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::UnsupportedSource);
    CHECK(host.native_state().failure.operation == NativeFailureOperation::Mutation);
}

void rich_syminfo_begin_witness() {
    const Bar bars[] = {bar(kT), bar(kT + 60000)};
    const InputsMap inputs{{"rich_input", "kept"}};
    SymInfo info;
    info.ticker = "RICH";
    info.tickerid = "RICH:SYMINF0";
    info.currency = "EUR";
    info.basecurrency = "USD";
    info.type = "forex";
    info.timezone = "UTC";
    info.session = "24x7";
    info.volumetype = "base";
    info.description = "rich native fixture";
    info.mintick = 0.0001;
    info.pointvalue = 10.0;

    ProviderHost rich;
    rich.run(bars, 2, "1", "1", inputs, info, nullptr);
    CHECK(rich.prepares == 1);
    CHECK(rich.saw_inputs);
    CHECK(rich.saw_syminfo);
    CHECK(!rich.saw_overrides);
    CHECK(rich.last_tickerid == "RICH:SYMINF0");
    CHECK(rich.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(rich.native_state().spec != nullptr);
    if (rich.native_state().spec) {
        CHECK(rich.native_state().spec->tickerid == "RICH:SYMINF0");
        CHECK(rich.native_state().spec->ticker == "RICH");
        CHECK(rich.native_state().spec->currency == "EUR");
        near(rich.native_state().spec->price_tick, 0.0001);
        near(rich.native_state().spec->point_value, 10.0);
    }
    CHECK(rich.callback_tickerid == "RICH:SYMINF0");

    // The pointer is only a begin-call borrow; the projected spec remains
    // value-owned when the caller changes its SymInfo after return.
    info.tickerid = "RICH:CHANGED";
    CHECK(rich.native_state().spec != nullptr);
    if (rich.native_state().spec)
        CHECK(rich.native_state().spec->tickerid == "RICH:SYMINF0");

    ProviderHost changed;
    SymInfo changed_info = info;
    changed_info.tickerid = "RICH:SYMINF1";
    changed.run(bars, 2, "1", "1", inputs, changed_info, nullptr);
    CHECK(changed.saw_syminfo);
    CHECK(changed.last_tickerid == "RICH:SYMINF1");
    CHECK(changed.native_continuation_hash() != rich.native_continuation_hash());

    // Every non-rich public begin carries a null SymInfo pointer.  The simple
    // and TF-aware paths are exercised here; the stream path above also
    // records the null case.
    ProviderHost simple;
    simple.run(bars, 2);
    CHECK(!simple.saw_syminfo);
    ProviderHost tf;
    tf.run(bars, 2, "1", "1");
    CHECK(!tf.saw_syminfo);
}

NativeRunSpec undetected_spec(const char* key) {
    auto configured = spec_for(key);
    configured.input_tf.clear();
    configured.script_tf.clear();
    configured.timeframe_undetected = true;
    return configured;
}

void undetected_timeframe_witness() {
    UndetectedTimeframeHost host;
    const auto timestamp = kT + 12345;
    const Bar bars[] = {bar(timestamp, 100.0, 103.0, 99.0, 101.0)};
    host.run(bars, 1);
    CHECK(host.prepares == 1);
    CHECK(host.last_n == 1);
    CHECK(host.last_input_tf.empty());
    CHECK(host.last_script_tf.empty());
    CHECK(host.ready_after_prepare);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.bar_calls == 1);
    CHECK(host.opening.has_value());
    CHECK(host.pending_reduction.has_value());
    CHECK(host.physical_position().lot_count == 0);
    CHECK(!applied_with_label(host, "undetected-opening").has_value());
    CHECK(!applied_with_label(host, "undetected-pending-reduction").has_value());

    int accepted = 0;
    int driver_points = 0;
    int open_points = 0;
    int close_points = 0;
    int middle_points = 0;
    for (const auto& row : host.native_events(0)) {
        if (row.command && std::visit([](const auto& event) {
                using T = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<T, no::AcceptedEvent>) {
                    return event.request().label == "undetected-opening"
                        || event.request().label == "undetected-pending-reduction";
                }
                return false;
            }, *row.command)) {
            ++accepted;
        }
        if (!row.driver) continue;
        ++driver_points;
        CHECK(row.driver->coordinate.open_ms == timestamp);
        CHECK(row.driver->coordinate.eligible_open_ms == timestamp);
        CHECK(row.driver->coordinate.last_traded_close_ms == timestamp);
        CHECK(row.driver->coordinate.next_period_open_ms == timestamp);
        CHECK(row.driver->coordinate.next_input_open_ms == timestamp);
        CHECK(row.driver->coordinate.effective_time_ms == timestamp);
        if (row.driver->coordinate.path_phase == NativePathPhase::Open) ++open_points;
        else if (row.driver->coordinate.path_phase == NativePathPhase::Close) ++close_points;
        else ++middle_points;
    }
    CHECK(accepted == 2);
    CHECK(driver_points == 4);
    CHECK(open_points == 1);
    CHECK(close_points == 1);
    CHECK(middle_points == 2);
    CHECK(host.contexts.size() == 1);
    if (!host.contexts.empty()) {
        const auto& context = host.contexts.front();
        CHECK(context.input_interval.open_ms == timestamp);
        CHECK(context.input_interval.next_period_open_ms == timestamp);
        CHECK(context.script_interval.open_ms == timestamp);
        CHECK(context.script_interval.next_period_open_ms == timestamp);
        CHECK(context.sub_index == 0);
        CHECK(context.sub_count == 1);
        CHECK(context.is_terminal_sub_bar);
        CHECK(context.sub_bar_open_ms == timestamp);
        CHECK(context.script_bar_open_ms == timestamp);
    }
    CHECK(host.native_continuation_hash() != 0);
}

void undetected_timeframe_rejection_witness() {
    auto nonempty = spec_for("undetected-nonempty");
    nonempty.timeframe_undetected = true;
    const auto nonempty_result = validate_native_run_spec(nonempty);
    CHECK(nonempty_result.error == NativeRunSpecError::InvalidUndetectedTimeframe);
    CHECK(nonempty_result.field == NativeRunSpecField::TimeframeUndetected);

    auto with_path = undetected_spec("undetected-path");
    IntrabarPath::lower_tf lower;
    lower.tf = "1";
    with_path.intrabar.value = std::move(lower);
    const auto path_result = validate_native_run_spec(with_path);
    CHECK(path_result.error == NativeRunSpecError::InvalidUndetectedTimeframe);
    CHECK(path_result.field == NativeRunSpecField::TimeframeUndetected);

    TermsHost too_many;
    CHECK(too_many.configure_native(undetected_spec("undetected-two-bars")).status
          == NativeSetupStatus::Applied);
    const auto before = too_many.native_continuation_hash();
    const Bar bars[] = {bar(kT), bar(kT + 1)};
    too_many.run(bars, 2);
    CHECK(too_many.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(too_many.last_run_status() != 0);
    CHECK(too_many.native_consumed_high_water() == 0);
    CHECK(too_many.native_continuation_hash() == before);

    TermsHost zero;
    CHECK(zero.configure_native(undetected_spec("undetected-zero-bars")).status
          == NativeSetupStatus::Applied);
    zero.run(nullptr, 0);
    CHECK(zero.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(zero.native_consumed_high_water() == 1);

    TermsHost stream;
    CHECK(stream.configure_native(undetected_spec("undetected-stream")).status
          == NativeSetupStatus::Applied);
    const Bar warmup[] = {bar(kT)};
    CHECK(!stream.stream_begin(warmup, 1, "", ""));
    CHECK(stream.native_state().kind == NativeLifecycleKind::Ready);
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
    distribution_samples_witness();
    synthesized_distribution_samples_witness();
    intrabar_decision_floor_witness();
    provider_and_staged_fx_witness();
    aborted_run_fx_restage_witness();
    during_run_fx_staging_refusal_witness();
    rich_syminfo_begin_witness();
    undetected_timeframe_witness();
    undetected_timeframe_rejection_witness();
    precommit_overflow_witness();
    std::printf("R4-D L1 native lowering: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
