// P38: a direct field lookup must agree with the independently collected
// diagnostic reflection, including absence and all four FieldValue kinds.
// RED against 6cfcff0696: Draft overloads of the read functions are absent.
#include <pineforge/source/market_admission.hpp>
#include "cr150_p3b_allocations.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>

namespace {
using namespace pineforge::admission;
int failures = 0;
int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (false)
void check(const Draft& draft) {
    const auto oracle = fields(draft); // explicitly non-flat diagnostic oracle
    CHECK(!oracle.empty());
    cr150_p3b_allocations::begin();
    for (const auto& field : oracle) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::uint64_t>)
                CHECK(read_unsigned(draft, field.path) == value);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                CHECK(read_integer(draft, field.path) == value);
            else if constexpr (std::is_same_v<T, double>) {
                const auto actual = read_double(draft, field.path);
                CHECK(actual == value || (std::isnan(actual) && std::isnan(value)));
            } else CHECK(read_string(draft, field.path) == value);
        }, field.value);
    }
    cr150_p3b_allocations::end();
    CHECK(cr150_p3b_allocations::live == 0);
    CHECK(cr150_p3b_allocations::peak <= 8192);
    CHECK(read_unsigned(draft, "draft.not_present") == 0);
    CHECK(read_integer(draft, "draft.not_present") == 0);
    CHECK(read_double(draft, "draft.not_present") == 0.0);
    CHECK(read_string(draft, "draft.not_present").empty());
}
}

int main() {
    check(Draft{});
    for (std::uint64_t i = 1; i <= 19; ++i) {
        Draft draft;
        auto observation = std::make_shared<CommandObservation>();
        observation->command = i;
        observation->id = "entry-" + std::to_string(i);
        observation->bar = static_cast<int>(i);
        observation->placement_cycle = -static_cast<std::int64_t>(i);
        observation->requested_quantity = i / 8.0;
        observation->buy = (i % 2) != 0;
        if (i % 3) observation->original_sizing = SizingObservation{i / 4.0, 1000, 20, 21, 1.25};
        draft.bind(observation);
        if (i % 2) draft.reviewed({i + 20, Checkpoint::ExplicitPair, static_cast<int>(i), i});
        if (i % 5) draft.sizing_revised({i + 40, i, static_cast<int>(i), i});
        check(draft);
    }
    std::printf("P38 admission field lookup: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
