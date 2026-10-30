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
detail::CohortExposureFold check(const OriginRoster& origins, const Units& units) {
    const auto actual = detail::fold_cohort_exposure(origins, units);
    const double expected = old_fold(origins, units);
    assert(std::memcmp(&actual.total, &expected, sizeof expected) == 0);
    ++comparisons;
    return actual;
}

void special_values_and_multiplicity() {
    OriginRoster origins;
    Units units;
    check(origins, units);
    units.emplace(19, 1.0);
    check(origins, units); // Unit row with no roster entry.
    push(origins, 71);
    check(origins, units); // Roster entry with no unit row.
    units.clear();
    check(origins, units);
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
    shape(4 * 3 + 16, 3, 6);
    shape(4 * 3 + 17, 3, 6);
    shape(4 * 3 + 17, 3, 0);
    shape(600, 128, 128);
    shape(600, 129, 129);
    // Multiplicity alone reaches the stack boundary.
    shape(200, 1, 128);
    shape(200, 1, 129);
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
    randomized_mutations();
    std::printf("cohort exposure bit comparisons: %llu\n",
                static_cast<unsigned long long>(comparisons));
}
