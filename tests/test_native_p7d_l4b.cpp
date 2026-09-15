// A31 generic P7d witnesses.  No source host or source vocabulary appears in
// this TU: it proves pre-open birth delivery and applied-callback current
// execution directly against the native host surface.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d: %s\\n", __FILE__, __LINE__, #expr);         \
    }                                                                           \
} while (0)

constexpr std::int64_t kT = 1736121600000LL;

Bar bar(std::int64_t time, double price = 100.0) {
    return {price, price, price, price, 1.0, time};
}

NativeRunSpec spec_for(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "P7D:NATIVE";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

no::Request transact(double units, const char* label) {
    no::Request request;
    request.intent = no::Transact{units};
    request.label = label;
    return request;
}

no::Request reduce(double units, const char* label) {
    no::Request request;
    request.intent = no::Reduce{no::ExplicitUnits{units}};
    request.label = label;
    return request;
}

class PreOpenBirthHost final : public NativeStrategyHost {
public:
    int opens = 0;
    std::optional<no::RequestHandle> submitted;
    std::optional<no::ExecutionAppliedEvent> applied;

    void on_native_bar_open(const Bar&, const NativeDecisionContext& context) override {
        ++opens;
        const auto result = submit(transact(1.0, "pre-open-born"));
        CHECK(result.status == no::SubmitStatus::Accepted);
        CHECK(result.handle.has_value());
        if (result.handle) submitted = *result.handle;
        CHECK(context.coordinate.path_phase == NativePathPhase::Open);
        CHECK(context.coordinate.effective_time_ms == kT);
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (event.request().label == "pre-open-born") applied = event;
    }
};

class AppliedCurrentHost final : public NativeStrategyHost {
public:
    std::optional<no::RequestHandle> opening;
    std::optional<no::ExecutionAppliedEvent> current_reduction;
    std::optional<NativeCurrentExecutionResult> current_result;
    std::optional<std::int64_t> opening_time;
    int opening_callbacks = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (opening) return;
        const auto result = submit(transact(2.0, "opening"));
        CHECK(result.status == no::SubmitStatus::Accepted);
        CHECK(result.handle.has_value());
        if (result.handle) opening = *result.handle;
        if (result.handle) {
            const auto opening_result = execute_current(
                {*result.handle, NativeCurrentPriceRule::AsPresented});
            CHECK(std::holds_alternative<no::ExecutionAppliedEvent>(opening_result));
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        if (event.request().label != "opening") {
            if (event.request().label == "applied-current") current_reduction = event;
            return;
        }
        ++opening_callbacks;
        opening_time = event.effective_time_ms();
        CHECK(context.coordinate.effective_time_ms == *opening_time);
        const auto result = submit(reduce(1.0, "applied-current"));
        CHECK(result.status == no::SubmitStatus::Accepted);
        CHECK(result.handle.has_value());
        if (!result.handle) return;
        const auto preview = inspect_current_execution(
            {*result.handle, NativeCurrentPriceRule::AsPresented});
        CHECK(!preview.refusal.has_value());
        CHECK(preview.settlement_readiness == execution::Status::Applied);
        current_result = execute_current({*result.handle, NativeCurrentPriceRule::AsPresented});
        CHECK(std::holds_alternative<no::ExecutionAppliedEvent>(*current_result));
    }
};

void pre_open_birth_is_eligible_at_open() {
    PreOpenBirthHost host;
    CHECK(host.configure_native(spec_for("p7d-pre-open")).status == NativeSetupStatus::Applied);
    const Bar tape[] = {bar(kT)};
    host.run(tape, 1);

    CHECK(host.opens == 1);
    CHECK(host.submitted.has_value());
    CHECK(host.applied.has_value());
    if (host.applied) {
        CHECK(host.applied->handle() == *host.submitted);
        CHECK(host.applied->birth().decision_time_lower_bound == kT);
        CHECK(host.applied->effective_time_ms() == kT);
        CHECK(host.applied->cursor.point.path_phase == NativePathPhase::Open);
    }
    CHECK(std::abs(host.physical_position().signed_units - 1.0) < 1e-12);
}

void applied_callback_can_execute_at_its_coordinate() {
    AppliedCurrentHost host;
    CHECK(host.configure_native(spec_for("p7d-applied")).status == NativeSetupStatus::Applied);
    const Bar tape[] = {bar(kT)};
    host.run(tape, 1);

    CHECK(host.opening_callbacks == 1);
    CHECK(host.current_result.has_value());
    CHECK(host.current_reduction.has_value());
    if (host.current_reduction) {
        CHECK(host.opening_time.has_value());
        CHECK(host.current_reduction->effective_time_ms() == *host.opening_time);
        CHECK(host.current_reduction->cursor.point.path_phase == NativePathPhase::None);
        CHECK(host.current_reduction->closed_units == 1.0);
    }
    CHECK(std::abs(host.physical_position().signed_units - 1.0) < 1e-12);
}

}  // namespace

int main() {
    pre_open_birth_is_eligible_at_open();
    applied_callback_can_execute_at_its_coordinate();
    std::printf("A31 generic P7d: %d checks, %d failures\\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
