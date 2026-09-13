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

class FxActivationHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

}  // namespace

int main() {
    FxActivationHost host;
    const NativeFxCurve curve{{0}, {1.25}};
    const auto before = host.native_continuation_hash();
    const auto result = host.configure_native_fx_curve(curve);

    CHECK(result.status == NativeSetupStatus::Failed);
    CHECK(result.validation.error == NativeFxCurveError::WrongPhase);
    CHECK(result.validation.index == 0);
    CHECK(host.native_state().kind == NativeLifecycleKind::Unconfigured);
    CHECK(host.native_continuation_hash() == before);

    std::printf("%d checks %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
