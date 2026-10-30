#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/pine_cohort_exposure.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

namespace {
using pineforge::source::OriginRoster;
namespace detail = pineforge::source::detail;
using Units = std::unordered_map<std::uint64_t, double>;

// Frozen position-selection algorithm. Keep independent of the seam.
double old_fold(const OriginRoster& origins, const Units& live_units_by_origin) noexcept {
    double total = 0.0;
    bool have_previous = false;
    std::size_t previous = 0;
    for (;;) {
        bool have_next = false;
        std::size_t next = 0;
        double units = 0.0;
        for (const auto& unit : live_units_by_origin) {
            const auto* positions = origins.positions_of(unit.first);
            if (!positions) continue;
            for (const auto position : *positions) {
                if (have_previous && position <= previous) continue;
                if (!have_next || position < next) {
                    have_next = true;
                    next = position;
                    units = unit.second;
                }
                break;
            }
        }
        if (!have_next) break;
        if (std::isfinite(units) && units > 0.0) total += units;
        have_previous = true;
        previous = next;
    }
    return std::isfinite(total) && total > 0.0 ? total : 0.0;
}

void push(OriginRoster& origins, std::uint64_t incarnation) {
    origins.push_back({{"cohort-fold", 1}, incarnation});
}

std::uint64_t comparisons = 0;
std::array<std::uint64_t, 4> path_hits{};
detail::CohortExposureFold check(const OriginRoster& origins, const Units& units) {
    const auto actual = detail::fold_cohort_exposure(origins, units);
    const double expected = old_fold(origins, units);
    assert(std::memcmp(&actual.total, &expected, sizeof expected) == 0);
    assert(actual.path < path_hits.size());
    ++path_hits[actual.path];
    ++comparisons;
    return actual;
}

void special_values_and_multiplicity() {
    OriginRoster origins;
    Units units;
    assert(check(origins, units).path == 0);
    units.emplace(19, 1.0);
    assert(check(origins, units).path == 0); // Unit row with no roster entry.
    push(origins, 71);
    check(origins, units); // Roster entry with no unit row.
    units.clear();
    assert(check(origins, units).path == 0);
    push(origins, 19);
    push(origins, 3);
    push(origins, 19);
    push(origins, 41);
    units = {{71, 1e16}, {19, 1.0}, {3, 1.0}, {41, 1.0}, {9001, 2.0}};
    // Sequential additions lose the four small terms; regrouping changes bits.
    assert(check(origins, units).total == 1e16);
    origins.retire_before(2); // Cut through the repeated incarnation.
    assert(check(origins, units).total == 3.0);
    push(origins, 19);
    assert(check(origins, units).total == 4.0);
    units.erase(19);
    assert(check(origins, units).total == 2.0);
    units.emplace(19, std::numeric_limits<double>::denorm_min());
    check(origins, units);

    const std::array<double, 10> values{{
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -0.0, 0.0, -1.0, std::numeric_limits<double>::denorm_min(),
        1.0, 1e16, std::numeric_limits<double>::max()
    }};
    for (const auto value : values) {
        for (auto& row : units) row.second = value;
        check(origins, units);
    }
    for (auto& row : units) row.second = std::numeric_limits<double>::max();
    assert(check(origins, units).total == 0.0); // Overflow is clamped.
    origins.retire_before(origins.size());
    assert(origins.size() > 0 && origins.members().empty());
    assert(check(origins, units).total == 0.0);
    push(origins, 19); // Global positions continue after full retirement.
    units[19] = 7.0;
    assert(check(origins, units).total == 7.0);
}

detail::CohortExposureFold shape(std::size_t history, std::size_t rows,
                                 std::size_t positions) {
    assert(rows > 0 && positions <= history);
    OriginRoster origins;
    Units units;
    for (std::size_t i = 0; i < rows; ++i)
        units.emplace(i + 1, i % 3 == 0 ? 1e16 : 1.0);
    for (std::size_t i = 0; i < history; ++i)
        push(origins, i < positions ? i % rows + 1 : rows + i + 1);
    return check(origins, units);
}

void boundary_values() {
    assert(shape(4 * 3 + 16, 3, 6).path == 1);
    assert(shape(4 * 3 + 17, 3, 6).path == 2);
    const auto no_positions = shape(4 * 3 + 17, 3, 0);
    assert(no_positions.path == 2 && no_positions.total == 0.0);
    assert(no_positions.steps == 3); // Every missing unit row is still examined.
    const auto k = detail::kCohortFoldStack;
    assert(shape(600, k, k).path == 2);
    assert(shape(600, k + 1, k + 1).path == 3);
    // Multiplicity alone reaches the stack boundary.
    assert(shape(200, 1, k).path == 2);
    assert(shape(200, 1, k + 1).path == 3);
}

detail::CohortExposureFold check_stack_sort(const OriginRoster& origins,
                                          const Units& units) {
    assert(origins.members().size() > 4 * units.size() + 16);
    std::size_t positions = 0;
    for (const auto& row : units) {
        const auto* mapped = origins.positions_of(row.first);
        if (mapped) positions += mapped->size();
    }
    assert(positions > 0 && positions <= 128);
    const auto hits_before = path_hits[2];
    const auto actual = check(origins, units);
    assert(actual.path == 2 && actual.steps > 0);
    assert(path_hits[2] > hits_before);
    return actual;
}

void stack_sort_special_values() {
    OriginRoster origins;
    for (const auto id : {7U, 19U, 43U, 7U, 43U, 19U}) push(origins, id);
    for (std::size_t i = origins.size(); i < 64; ++i) push(origins, 1000 + i);
    const double tiny = std::numeric_limits<double>::denorm_min();
    Units units{{7, 0.0}, {19, tiny}, {43, 2.0 * tiny}};
    const std::array<double, 7> values{{
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -0.0, 0.0, -1.0, tiny
    }};
    for (const auto value : values) {
        units[7] = value;
        const auto actual = check_stack_sort(origins, units);
        // Each row occurs twice; the denormal contribution stays observable.
        const double expected = (value == tiny ? 8.0 : 6.0) * tiny;
        assert(std::memcmp(&actual.total, &expected, sizeof expected) == 0);
    }
    for (auto& row : units) row.second = std::numeric_limits<double>::max();
    const auto overflow = check_stack_sort(origins, units);
    const double expected = 0.0; // Multiple maximum values overflow to +inf.
    assert(std::memcmp(&overflow.total, &expected, sizeof expected) == 0);
}

void stack_sort_interleavings() {
    const std::array<std::uint64_t, 3> small_ids{{19, 43, 71}};
    const std::array<std::size_t, 3> repeats{{8, 12, 16}};
    for (std::size_t shape = 0; shape < repeats.size(); ++shape) {
        OriginRoster origins;
        const Units units{{7, 1e16}, {19, 1.0}, {43, 1.0}, {71, 1.0}};
        for (std::size_t i = 0; i < small_ids.size() * repeats[shape]; ++i) {
            if (i == 4) push(origins, 7);
            push(origins, small_ids[(i + shape) % small_ids.size()]);
        }
        for (std::size_t i = origins.size(); i < 64; ++i) push(origins, 1000 + i);
        const auto actual = check_stack_sort(origins, units);
        // Four ones precede 1e16: the exact sum is 10000000000000004.
        // The ULP is 2 and this sum has an even significand, so later ones
        // round away. Grouping rows puts k complete small rows before 1e16,
        // yielding 1e16 + repeats[shape] * k (k = 0..3), never this literal.
        const double expected = 10000000000000004.0;
        assert(std::memcmp(&actual.total, &expected, sizeof expected) == 0);
    }
}

void randomized_mutations() {
    std::mt19937_64 random(0x461bea77d5ULL);
    const std::array<double, 11> values{{
        1e16, 1.0, 0.25, 3.0, -1.0, -0.0,
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()
    }};
    bool saw_bucket_order_differ = false;
    for (int trial = 0; trial < 48; ++trial) {
        OriginRoster origins;
        Units units;
        std::vector<std::uint64_t> ids;
        for (int i = 0; i < 24; ++i) ids.push_back(random());
        units.reserve(64);
        for (const auto id : ids) {
            push(origins, id);
            units.emplace(id, values[random() % values.size()]);
        }
        auto row = units.begin();
        for (const auto& handle : origins) {
            assert(row != units.end());
            if (row->first != handle.incarnation) saw_bucket_order_differ = true;
            ++row;
        }
        check(origins, units);
        for (int mutation = 0; mutation < 240; ++mutation) {
            const auto id = ids[random() % ids.size()];
            switch (random() % 5) {
            case 0:
                push(origins, id);
                break;
            case 1:
                units[id] = values[random() % values.size()];
                break;
            case 2:
                units.erase(id);
                break;
            case 3: {
                const auto retained = origins.members().size();
                origins.retire_before(origins.base() + random() % (retained + 1));
                break;
            }
            default:
                // Long histories with few mapped positions exercise gathering.
                for (int i = 0; i < 5; ++i) push(origins, random());
                break;
            }
            check(origins, units);
        }
    }
    assert(saw_bucket_order_differ);
}
} // namespace

int main() {
    special_values_and_multiplicity();
    boundary_values();
    stack_sort_special_values();
    stack_sort_interleavings();
    randomized_mutations();
    for (const auto hits : path_hits) assert(hits > 0);
    std::printf("cohort exposure bit comparisons: %llu\n",
                static_cast<unsigned long long>(comparisons));
    std::printf("cohort exposure paths: %llu %llu %llu %llu\n",
                static_cast<unsigned long long>(path_hits[0]),
                static_cast<unsigned long long>(path_hits[1]),
                static_cast<unsigned long long>(path_hits[2]),
                static_cast<unsigned long long>(path_hits[3]));
}
