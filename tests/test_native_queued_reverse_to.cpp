#include <pineforge/native_host.hpp>

#include <cstdio>
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

static_assert(std::variant_size_v<native_order::ExecutionPlan> == 4);
static_assert(std::variant_size_v<NativeCurrentExecutionResult> == 5);

}  // namespace

int main() {
    native_order::ExecutionPlan plan{execution::ReverseTo{-3.5}};
    const auto* reversal = std::get_if<execution::ReverseTo>(&plan);
    CHECK(reversal != nullptr);
    CHECK(reversal != nullptr && reversal->signed_units == -3.5);

    NativeCurrentExecutionResult result{native_order::CancelledEvent{}};
    CHECK(std::holds_alternative<native_order::CancelledEvent>(result));
    CHECK(result.index() == 4);

    std::printf("%d checks %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
