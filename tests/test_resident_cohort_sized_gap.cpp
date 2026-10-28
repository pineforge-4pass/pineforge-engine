// RED-first gap ruling: a table sized to the 65,536-row floor must refuse
// admission of row 65,537 before any index bytes or table counts can change.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kRows = 65'536;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

// Match the cohort index's 40-byte fixed record, with live facts kept resident.
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
static_assert(sizeof(LiveCohort::Record) == 40, "the real cohort fixed-record width");

void sized_gap_refuses_before_mutation() {
    auto manifest = pineforge::detail::default_scratch_manifest(kRows);
    // Leave room for N+1 logical ids and their bytes so only the resident
    // admission limit, not the historical index capacity, can refuse.
    manifest.U = kRows + 1;
    manifest.B_U = static_cast<std::uint64_t>(kRows + 1) * 32;
    auto slab = std::make_shared<pineforge::detail::ScratchSlab>(manifest);
    pineforge::detail::DiskResidentIdTable<LiveCohort> table;
    // Same R/2 sizing and three-argument binder as bind_tree_scratch_tables.
    const auto sizing = pineforge::detail::resident_cohort_sizing(slab->manifest().R / 2);
    require(sizing.need == kRows && sizing.limit == kRows,
            "the descriptor must select exactly the 65,536-row resident limit");
    table.bind_scratch(slab, pineforge::detail::ScratchResource::cohort, sizing);

    for (std::size_t i = 0; i < kRows; ++i) {
        const auto inserted = table.emplace(
            "gap-" + std::to_string(i), LiveCohort{i + 1, true});
        require(inserted.second && !inserted.first->second.spillable(),
                "exactly N live ids must pass");
        require(table.size() == i + 1 && table.resident_size() == i + 1,
                "every admitted id must remain logical and resident");
    }
    require(table.size() == kRows && table.resident_size() == kRows,
            "the gap witness must hold exactly N live ids first");

    const auto& region = slab->region(pineforge::detail::ScratchResource::cohort);
    const auto extent = region.logical_extent;
    std::vector<unsigned char> before(static_cast<std::size_t>(extent));
    slab->read_at(region, 0, before.data(), before.size());
    bool refused = false;
    try {
        (void)table.emplace("gap-overflow", LiveCohort{kRows + 1, true});
    } catch (const pineforge::detail::ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                    && failure.args
                    && *failure.args == "{\"resource\":\"cohort\"}",
                "sized gap refusal must be typed as cohort capacity");
        const std::string message = error.what();
        const auto prefix = std::string("more than ")
            + pineforge::detail::scratch_limit_number(kRows)
            + " order ids active at once";
        const auto count = std::string("live count ")
            + pineforge::detail::scratch_limit_number(kRows + 1);
        const auto cap = std::string("approved maximum ")
            + pineforge::detail::scratch_limit_number(
                pineforge::detail::kMaxResidentCohortsCap);
        require(message.find(prefix) != std::string::npos
                    && message.find(count) != std::string::npos
                    && message.find(cap) != std::string::npos
                    && message.find("declare") == std::string::npos,
                "sized gap refusal must name rows, live count and the approved maximum, "
                "and offer no remedy that cannot be taken");
    }

    require(refused && table.size() == kRows && table.resident_size() == kRows,
            "N+1 must refuse without dropping or adding an id");
    std::vector<unsigned char> after(before.size());
    slab->read_at(region, 0, after.data(), after.size());
    require(before == after && region.logical_extent == extent,
            "sized gap refusal must preserve index bytes");
}
} // namespace

int main() {
    try {
        sized_gap_refuses_before_mutation();
        std::puts("resident cohort sized gap: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
