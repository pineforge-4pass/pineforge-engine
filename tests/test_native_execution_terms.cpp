#include <pineforge/native_host.hpp>

#include <cstdio>
#include <type_traits>
#include <variant>

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

class IdentityTermsHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

static_assert(std::variant_size_v<NativeCurrentExecutionResult> == 5);
static_assert(std::is_same_v<std::variant_alternative_t<4, NativeCurrentExecutionResult>,
                             native_order::CancelledEvent>);

}  // namespace

int main() {
    IdentityTermsHost host;
    NativeExecutionTermsFacts facts{};
    facts.default_resolved_price = 17.25;
    facts.raw_price = 17.0;

    const auto terms = host.resolve_execution_terms(facts);
    CHECK(terms.resolved_price == facts.default_resolved_price);
    CHECK(!terms.units.has_value());
    CHECK(terms.shape == native_order::OpeningShape::Transact);

    std::printf("%d checks %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
