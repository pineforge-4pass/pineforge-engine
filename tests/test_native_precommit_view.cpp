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

class DefaultPrecommitHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

}  // namespace

int main() {
    DefaultPrecommitHost host;
    NativePrecommitView view{};
    view.current = true;
    CHECK(host.validate_execution_precommit(view) == NativePrecommitVerdict::Proceed);

    NativeCurrentExecutionPreview preview{};
    CHECK(!preview.refusal.has_value());
    CHECK(!preview.settlement_readiness.has_value());
    CHECK(!preview.terms_rejection.has_value());
    CHECK(!preview.terms_cancellation.has_value());

    const auto before = host.native_continuation_hash();
    const auto inspected = host.inspect_current_execution(NativeCurrentExecution{});
    CHECK(inspected.refusal == NativeCurrentRefusal::NoExecutionContext);
    CHECK(!inspected.terms_rejection.has_value());
    CHECK(!inspected.terms_cancellation.has_value());
    CHECK(host.native_continuation_hash() == before);

    std::printf("%d checks %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
