// Expected red on f66318a3: row 4,097 is refused while filling live cohorts.
// Keep this test on the base API so the lab can prove that runtime failure.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
namespace detail = pineforge::detail;
constexpr std::size_t kExpectedLimit = 65536;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

// Same fixed record width as the source cohort codec; live rows cannot spill.
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
using Table = detail::DiskResidentIdTable<LiveCohort>;

template<class Admit>
void expect_resident_refusal(Admit&& admit) {
    bool refused = false;
    try {
        admit();
    } catch (const detail::ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                    && failure.args && *failure.args == "{\"resource\":\"cohort\"}",
                "resident refusal must carry resource cohort");
        require(std::string(error.what()).find("more than 65,536 order ids active at once")
                    != std::string::npos,
                "resident refusal must name the new active-id limit");
    }
    require(refused, "65,537th live cohort must be refused");
}

void bounded_live_cohorts() {
    detail::ScratchManifest manifest{};
    // One spare historical slot isolates the resident guard at the boundary.
    manifest.U = kExpectedLimit + 1;
    manifest.B_U = (kExpectedLimit + 1) * 32;
    auto slab = std::make_shared<detail::ScratchSlab>(manifest);
    Table table;
    table.bind_scratch(slab, detail::ScratchResource::cohort);
    for (std::size_t i = 0; i < kExpectedLimit; ++i) {
        try {
            const auto inserted = table.emplace("live-" + std::to_string(i), LiveCohort{i + 1, true});
            require(inserted.second && !inserted.first->second.spillable(),
                    "each distinct live cohort must be admitted without making it spillable");
        } catch (const detail::ScratchCapacityError& error) {
            throw std::runtime_error("unexpected refusal at live cohort "
                + std::to_string(i + 1) + ": " + error.what());
        }
    }
    table.spill();
    require(table.size() == kExpectedLimit && table.resident_size() == kExpectedLimit
                && table.work_counters().rows_spilled == 0,
            "all 65,536 non-spillable cohorts must remain resident");

    const auto extent = slab->region(detail::ScratchResource::cohort).logical_extent;
    const std::string extra = "live-overflow";
    expect_resident_refusal([&] { table.require_insert(extra); });
    expect_resident_refusal([&] {
        table.require_inserts([&](auto&& visit) { visit(extra); });
    });
    expect_resident_refusal([&] {
        (void)table.emplace(extra, LiveCohort{kExpectedLimit + 1, true});
    });
    require(table.size() == kExpectedLimit && table.resident_size() == kExpectedLimit
                && table.find_resident(extra) == table.end()
                && slab->region(detail::ScratchResource::cohort).logical_extent == extent
                && table.work_counters().rows_spilled == 0,
            "refusal must preserve rows, index extent and non-spillable residency");

    // Releasing one live row must restore admission without relaxing the cap.
    {
        const auto retired = table.find_resident("live-0");
        require(retired != table.end(), "first live cohort must still be resident");
        retired->second.live = false;
    }
    table.require_insert(extra);
    require(table.emplace(extra, LiveCohort{kExpectedLimit + 1, true}).second,
            "a released cohort must free one resident admission");
    require(table.size() == kExpectedLimit + 1 && table.resident_size() == kExpectedLimit
                && table.work_counters().rows_spilled == 1,
            "only the released cohort may spill to admit its replacement");
}
} // namespace

int main() {
    try {
        bounded_live_cohorts();
        std::puts("resident cohort ceiling: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
