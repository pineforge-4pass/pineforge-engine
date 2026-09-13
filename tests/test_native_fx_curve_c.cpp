// This phase-1 seed deliberately exercises only the C++ staging surface.
// The C ABI entry point is a W3 phase-2 responsibility.
#include <pineforge/native_host.hpp>

#include <cstdio>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    } \
} while (0)

class FxCurveHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

NativeRunSpec ready_spec() {
    NativeRunSpec spec;
    spec.identity = {"fx-curve-stage", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:FX";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

}  // namespace

int main() {
    FxCurveHost host;
    CHECK(host.configure_native(ready_spec()).status == NativeSetupStatus::Applied);
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);

    const auto before = host.native_continuation_hash();
    const NativeFxCurve invalid{{0, 1}, {1.0}};
    const auto result = host.configure_native_fx_curve(invalid);
    CHECK(result.status == NativeSetupStatus::Failed);
    CHECK(result.validation.error == NativeFxCurveError::LengthMismatch);
    CHECK(result.validation.index == 0);
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(host.native_continuation_hash() == before);

    const NativeFxCurve valid{{0}, {1.25}};
    const auto staged = host.configure_native_fx_curve(valid);
    CHECK(staged.status == NativeSetupStatus::Applied);
    CHECK(staged.validation.error == NativeFxCurveError::None);
    CHECK(staged.validation.index == 0);
    CHECK(host.native_continuation_hash() != before);

    const auto cleared = host.configure_native_fx_curve(NativeFxCurve{});
    CHECK(cleared.status == NativeSetupStatus::Applied);
    CHECK(cleared.validation.error == NativeFxCurveError::None);
    CHECK(host.native_continuation_hash() == before);

    std::printf("%d checks %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
