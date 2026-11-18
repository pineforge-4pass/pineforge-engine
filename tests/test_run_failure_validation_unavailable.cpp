// Separate process: fail real allocations only while calling production APIs.
#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>

namespace allocation_failure {
enum class Mode { bad_alloc, unexpected };
struct Unexpected {};
bool armed = false;
Mode mode = Mode::bad_alloc;
std::size_t calls = 0;
std::size_t fail_at = 0;

void arm(std::size_t index, Mode selected = Mode::bad_alloc) noexcept {
    calls = 0;
    fail_at = index;
    mode = selected;
    armed = true;
}
void before_allocation() {
    if (!armed) return;
    ++calls;
    if (fail_at != 0 && calls == fail_at) {
        if (mode == Mode::unexpected) throw Unexpected{};
        throw std::bad_alloc{};
    }
}
}  // namespace allocation_failure

void* operator new(std::size_t size) {
    allocation_failure::before_allocation();
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace pineforge;
int failures = 0;
int checks = 0;
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

void recovered(const RunFailureArgs& args, const std::string& json) {
    const auto result = validate_run_failure_name("no_data_request", args);
    CHECK(result.status == RunFailureValidationStatus::accepted);
    CHECK(result.value && result.value->code == RunFailureCode::no_data_request);
    CHECK(result.value && result.value->args && *result.value->args == json);
    const auto typed = make_run_failure(RunFailureCode::no_data_request, args);
    CHECK(typed.code == RunFailureCode::no_data_request && typed.args && *typed.args == json);
}
}  // namespace

int main() {
    // Construct inputs and expectations before arming global new. Counting a
    // successful call identifies all allocation sites, including own_json's
    // object/control/ledger path; fail each site in an otherwise identical call.
    const RunFailureArgs args{{"function", "request.financial"},
                              {"call", std::string(1031, 'c')}, {"line", 7}};
    const std::string json = "{\"call\":\"" + std::string(1031, 'c')
        + "\",\"function\":\"request.financial\",\"line\":7}";

    allocation_failure::arm(0);
    const auto baseline = validate_run_failure_name("no_data_request", args);
    allocation_failure::armed = false;
    const auto strict_allocations = allocation_failure::calls;
    CHECK(baseline.status == RunFailureValidationStatus::accepted && baseline.value);
    CHECK(strict_allocations > 0);
    for (std::size_t i = 1; i <= strict_allocations; ++i) {
        allocation_failure::arm(i);
        const auto result = validate_run_failure_name("no_data_request", args);
        allocation_failure::armed = false;
        CHECK(allocation_failure::calls >= i);
        CHECK(result.status == RunFailureValidationStatus::unavailable);
        CHECK(!result.value);
        recovered(args, json);
    }

    allocation_failure::arm(0);
    const auto typed_baseline = make_run_failure(RunFailureCode::no_data_request, args);
    allocation_failure::armed = false;
    const auto typed_allocations = allocation_failure::calls;
    CHECK(typed_baseline.code == RunFailureCode::no_data_request);
    CHECK(typed_allocations > 0);
    for (std::size_t i = 1; i <= typed_allocations; ++i) {
        allocation_failure::arm(i);
        const auto typed = make_run_failure(RunFailureCode::no_data_request, args);
        allocation_failure::armed = false;
        CHECK(allocation_failure::calls >= i);
        CHECK(typed.code == RunFailureCode::out_of_memory);
        CHECK(!typed.args && typed.args.use_count() == 0);
        recovered(args, json);
    }

    allocation_failure::arm(1, allocation_failure::Mode::unexpected);
    const auto unexpected = validate_run_failure_name("no_data_request", args);
    allocation_failure::armed = false;
    CHECK(allocation_failure::calls == 1);
    CHECK(unexpected.status == RunFailureValidationStatus::unavailable && !unexpected.value);
    allocation_failure::arm(1, allocation_failure::Mode::unexpected);
    const auto typed_unexpected = make_run_failure(RunFailureCode::no_data_request, args);
    allocation_failure::armed = false;
    CHECK(allocation_failure::calls == 1);
    CHECK(typed_unexpected.code == RunFailureCode::engine_invariant && !typed_unexpected.args);
    recovered(args, json);

    // Lookup still precedes args validation/allocation when allocation is refused.
    allocation_failure::arm(1);
    const auto unknown = validate_run_failure_name("NOT_A_CODE", args);
    allocation_failure::armed = false;
    CHECK(allocation_failure::calls == 0);
    CHECK(unknown.status == RunFailureValidationStatus::unknown_code && !unknown.value);
    allocation_failure::arm(1);
    const auto genuine_oom = validate_run_failure_name("out_of_memory", {});
    allocation_failure::armed = false;
    CHECK(allocation_failure::calls == 0);
    CHECK(genuine_oom.status == RunFailureValidationStatus::accepted);
    CHECK(genuine_oom.value && genuine_oom.value->code == RunFailureCode::out_of_memory);

    std::printf("run_failure_validation_unavailable: strict_allocations=%zu "
                "typed_allocations=%zu checks=%d failures=%d\n",
                strict_allocations, typed_allocations, checks, failures);
    return failures == 0 && strict_allocations > 0 && typed_allocations > 0 ? 0 : 1;
}
