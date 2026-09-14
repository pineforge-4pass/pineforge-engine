// A28 native-default witnesses.  These record the pre-transfer behavior that
// the source seams must preserve for a native-bound host.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace x = pineforge::execution;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(value) do {                                                        \
    ++checks;                                                                    \
    if (!(value)) {                                                              \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\\n", __FILE__, __LINE__, #value);          \
    }                                                                            \
} while (0)

class NativeWitness final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    x::PhysicalExecutionContext context() const {
        return {1700000000000LL, 7, {}, {}};
    }

    x::Result settle(const x::Action& action, const x::Fill& fill) {
        return settle_native_execution_at(action, fill, context());
    }

    x::Result settle_with_effects(const x::Action& action, const x::Fill& fill,
                                  const x::LifecycleEffects& effects) {
        return settle_with_context(action, fill, effects, context());
    }

    double metadata(const std::string& key) const { return get_syminfo_metadata(key); }
};

class LegacyRouteWitness final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}
    void invoke_route(const char* seam) { throw_native_only_route(seam); }
};

x::Fill fill(double price, const char* id, uint64_t incarnation) {
    return {price, id, "", incarnation, 0.0};
}

void check_native_metadata_and_aux_refusals() {
    NativeWitness host;
    bool metadata_refused = false;
    try {
        host.set_syminfo_metadata("qty_step", 0.25);
    } catch (const std::runtime_error& error) {
        metadata_refused = std::string(error.what())
            == "native host refuses source mutation: set_syminfo_metadata";
    }
    CHECK(metadata_refused);
    CHECK(std::isnan(host.metadata("qty_step")));

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    const Bar bars[] = {{100.0, 101.0, 99.0, 100.0, 1.0, 1700000000000LL}};
    bool aux_refused = false;
    try {
        (void)host.set_aux_security_feed(bars, 1, "1");
    } catch (const std::runtime_error& error) {
        aux_refused = std::string(error.what())
            == "native host refuses source mutation: set_aux_security_feed";
    }
    CHECK(aux_refused);
#else
#error "A28 witness requires the auxiliary-security feed surface"
#endif
}

void check_legacy_route_refusal_message() {
    LegacyRouteWitness host;
    bool refused = false;
    try {
        host.invoke_route("legacy_run_simple");
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what())
            == "legacy_run_simple: not available on a native-bound host";
    }
    CHECK(refused);
}

void check_native_position_and_source_empty_settlement() {
    NativeWitness host;
    const auto opened = host.settle(order_action::Transact{2.5}, fill(100.0, "open", 1));
    CHECK(opened.status == x::Status::Applied);
    CHECK(host.physical_position().signed_units == 2.5);
    // S19's future base default must expose physical signed units, not the
    // source position-view freeze projection.
    CHECK(host.live_position_size() == 2.5);
    // S18 reads the generic physical trail state on a native-bound host.
    CHECK(host.observe_trail_best_price_v1() == 100.0);

    const auto closed = host.settle(x::Flatten{}, fill(90.0, "close", 2));
    CHECK(closed.status == x::Status::Applied);
    CHECK(std::isnan(host.observe_trail_best_price_v1()));
}

NativeRunSpec native_spec() {
    NativeRunSpec spec;
    spec.identity.session_key = "r4-c-s20-s21";
    spec.identity.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native witness";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

class PlainNativeWitness : public NativeStrategyHost {
public:
    int callbacks = 0;
    std::vector<native_order::SubmitStatus> submissions;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) {
            submissions.push_back(
                submit_market(native_order::Request{
                    order_action::Transact{1.0}, "open", ""}).status);
        } else if (callbacks == 2) {
            submissions.push_back(
                submit_market(native_order::Request{
                    x::Flatten{}, "close", ""}).status);
        }
    }
};

class CountingNativeWitness final : public PlainNativeWitness {
public:
    mutable int preflight_calls = 0;
    int observed_calls = 0;

protected:
    x::Status on_source_close_preflight(
            const Trade*, size_t, std::optional<int>&) const override {
        ++preflight_calls;
        return x::Status::Applied;
    }

    void on_source_close_observed(
            const Trade*, size_t, std::optional<int>) override {
        ++observed_calls;
    }
};

struct AppliedSummary {
    uint64_t ordinal = 0;
    double raw_price = 0.0;
    double resolved_price = 0.0;
    double current_ticket = 0.0;
    size_t first_trade_index = 0;
    size_t closed_trade_count = 0;
    uint64_t opened_lot_incarnation = 0;
    double closed_units = 0.0;
    double opened_units = 0.0;
};

std::vector<AppliedSummary> applied_events(const NativeStrategyHost& host) {
    std::vector<AppliedSummary> result;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(
                    &*event.command)) {
            result.push_back({applied->ordinal, applied->raw_price,
                              applied->resolved_price, applied->current_ticket,
                              applied->first_trade_index, applied->closed_trade_count,
                              applied->opened_lot_incarnation, applied->closed_units,
                              applied->opened_units});
        }
    }
    return result;
}

void check_native_source_close_hooks_are_bypassed() {
    const Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1.0, 60000},
        {101.0, 102.0, 100.0, 101.0, 1.0, 120000},
        {102.0, 103.0, 101.0, 102.0, 1.0, 180000},
    };
    CountingNativeWitness counting;
    PlainNativeWitness plain;
    const NativeRunSpec spec = native_spec();
    CHECK(counting.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(plain.configure_native(spec).status == NativeSetupStatus::Applied);
    counting.run(bars, 3, "1", "1");
    plain.run(bars, 3, "1", "1");
    CHECK(counting.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(plain.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(counting.last_error().empty());
    CHECK(plain.last_error().empty());
    CHECK(counting.callbacks == 3);
    CHECK(plain.callbacks == 3);
    CHECK(counting.preflight_calls == 0);
    CHECK(counting.observed_calls == 0);
    CHECK(counting.submissions == plain.submissions);
    CHECK(counting.physical_position().signed_units
          == plain.physical_position().signed_units);
    CHECK(counting.broker_state_hash() == plain.broker_state_hash());

    const auto counted = applied_events(counting);
    const auto uncounted = applied_events(plain);
    CHECK(counted.size() == 2);
    CHECK(counted.size() == uncounted.size());
    for (size_t index = 0; index < counted.size() && index < uncounted.size(); ++index) {
        CHECK(counted[index].ordinal == uncounted[index].ordinal);
        CHECK(counted[index].raw_price == uncounted[index].raw_price);
        CHECK(counted[index].resolved_price == uncounted[index].resolved_price);
        CHECK(counted[index].current_ticket == uncounted[index].current_ticket);
        CHECK(counted[index].first_trade_index == uncounted[index].first_trade_index);
        CHECK(counted[index].closed_trade_count == uncounted[index].closed_trade_count);
        CHECK(counted[index].opened_lot_incarnation == uncounted[index].opened_lot_incarnation);
        CHECK(counted[index].closed_units == uncounted[index].closed_units);
        CHECK(counted[index].opened_units == uncounted[index].opened_units);
    }
}

void check_native_empty_lifecycle_and_rejection() {
    NativeWitness host;
    const auto opened = host.settle(order_action::Transact{1.0}, fill(100.0, "open", 1));
    CHECK(opened.status == x::Status::Applied);

    const x::LifecycleEffects empty{};
    const auto applied = host.settle_with_effects(x::Flatten{}, fill(90.0, "empty", 2), empty);
    // S22's source-empty lifecycle default accepts the empty value (nullopt);
    // S23/S24 consequently have no source work to apply.
    CHECK(applied.status == x::Status::Applied);

    NativeWitness invalid_host;
    const auto invalid_open = invalid_host.settle(order_action::Transact{1.0}, fill(100.0, "open", 1));
    CHECK(invalid_open.status == x::Status::Applied);
    x::LifecycleEffects nonempty;
    nonempty.removals.push_back({999, 999, {}, 0});
    const auto rejected = invalid_host.settle_with_effects(
        x::Flatten{}, fill(90.0, "invalid", 2), nonempty);
    CHECK(rejected.status == x::Status::InvalidLifecycle);
    CHECK(invalid_host.physical_position().signed_units == 1.0);
}

} // namespace

int main() {
    check_native_metadata_and_aux_refusals();
    check_legacy_route_refusal_message();
    check_native_position_and_source_empty_settlement();
    check_native_source_close_hooks_are_bypassed();
    check_native_empty_lifecycle_and_rejection();
    std::printf("checks=%d failures=%d\\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
