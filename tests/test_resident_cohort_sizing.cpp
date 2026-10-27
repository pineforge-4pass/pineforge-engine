// Expected red on the fixed-floor base: the sizing function is absent and a
// table bind cannot raise the resident ceiling above 65,536.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
namespace detail = pineforge::detail;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void pure_sizing_boundaries() {
    const auto floor = detail::kMaxResidentCohorts;
    const auto cap = detail::kMaxResidentCohortsCap;

    const auto zero = detail::resident_cohort_sizing(0);
    require(zero.limit == floor && zero.need == 0 && !zero.declared_over_cap,
            "zero default need stays at the floor");
    const auto below_floor = detail::resident_cohort_sizing(floor - 1);
    require(below_floor.limit == floor && below_floor.need == floor - 1,
            "below-floor need is raised to the floor");

    const auto mid = detail::resident_cohort_sizing(200000);
    require(mid.limit == 200000 && mid.need == 200000 && !mid.declared_over_cap,
            "bar-derived mid need is honoured");

    const auto exact_cap = detail::resident_cohort_sizing(cap);
    require(exact_cap.limit == cap && exact_cap.need == cap
                && !exact_cap.declared_over_cap,
            "exact cap is admitted");

    const auto declared_over = detail::resident_cohort_sizing(
        cap + 1, 1, 0, 0, true);
    require(declared_over.limit == cap && declared_over.need == cap + 1
                && declared_over.declared_over_cap,
            "declared cap overflow remains visible in the result");
    bool refused = false;
    try {
        detail::require_resident_cohort_sizing(declared_over);
    } catch (const detail::ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                    && failure.args
                    && *failure.args == "{\"resource\":\"cohort\"}",
                "declared cap overflow is a typed cohort refusal");
        const std::string text = error.what();
        require(text.find(detail::scratch_limit_number(cap)) != std::string::npos
                    && text.find(detail::scratch_limit_number(cap + 1))
                        != std::string::npos,
                "declared cap refusal names both limit and need");
    }
    require(refused, "declared cap overflow must refuse");

    const auto default_over = detail::resident_cohort_sizing(cap + 1);
    require(default_over.limit == cap && default_over.need == cap + 1
                && !default_over.declared_over_cap,
            "default-derived over-cap need clamps to cap without refusal");

    const auto declared_terms = detail::resident_cohort_sizing(100, 2, 7, 11, true);
    require(declared_terms.limit == 218 && declared_terms.need == 218
                && !declared_terms.declared_over_cap,
            "fresh rate, literals and fixed allowance all contribute");

    const auto overflow = detail::resident_cohort_sizing(
        std::numeric_limits<std::uint64_t>::max(), 2, 1, 1, true);
    require(overflow.need == std::numeric_limits<std::uint64_t>::max()
                && overflow.limit == cap && overflow.declared_over_cap,
            "large bar demand saturates and cannot wrap to a small limit");
}

struct LiveCohort {
    struct Record {
        std::uint64_t handle;
        std::int64_t cycle;
        std::uint64_t count, digest, sides;
    };
    std::uint64_t handle = 0;
    bool live = true;
    Record retired_record() const noexcept { return {handle, 0, 0, 0, 0}; }
    static LiveCohort restore_record(const Record& row) { return {row.handle, false}; }
    bool spillable() const noexcept { return !live; }
};

void sized_table_wiring() {
    using Table = detail::DiskResidentIdTable<LiveCohort>;
    const auto limit = detail::kMaxResidentCohorts + 1;
    detail::ScratchManifest manifest{};
    manifest.U = limit + 1;
    manifest.B_U = (limit + 1) * 32;
    manifest.R = 2 * limit;
    auto slab = std::make_shared<detail::ScratchSlab>(manifest);
    Table table;
    table.bind_scratch(slab, detail::ScratchResource::cohort,
                       detail::resident_cohort_sizing(limit));

    for (std::size_t i = 0; i < limit; ++i) {
        const auto inserted = table.emplace(
            "sized-" + std::to_string(i), LiveCohort{i + 1, true});
        require(inserted.second && !inserted.first->second.spillable(),
                "sized resident limit admits every live cohort");
    }
    require(table.size() == limit && table.resident_size() == limit,
            "sized table retains the admitted population");

    bool refused = false;
    try {
        (void)table.emplace("sized-overflow", LiveCohort{limit + 1, true});
    } catch (const detail::ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                    && failure.args
                    && *failure.args == "{\"resource\":\"cohort\"}",
                "sized table refusal is typed as cohort capacity");
        require(std::string(error.what()).find(
                    "more than " + detail::scratch_limit_number(limit)
                    + " order ids active at once") != std::string::npos,
                "sized table refusal names its actual limit");
    }
    require(refused, "sized table refuses the next live cohort");
}
} // namespace

int main() {
    try {
        pure_sizing_boundaries();
        sized_table_wiring();
        std::puts("resident cohort sizing: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
