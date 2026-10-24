#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace pineforge::detail {

enum class ResidentKind : std::uint8_t {
    ledger_rows, idtable_entries, placement_snapshots, journal_pages
};

struct ResidentBudgetDescriptor {
    const char* name;
    const char* env_name;
    std::uint64_t default_entries;
    std::uint64_t max_entries;
    std::uint64_t bytes_per_entry;
    std::uint64_t fixed_base_bytes;
    bool measured;
};

// Foreign defaults are the current TradeLedger::kResidentRows (64),
// PlacementTable::kWindowReserve (256), and JournalStore::kMaxResident (2048).
// Their maxima and byte accounting are set by the owning lane; until then
// max == current default. These rows do not wire those caches to the knob.
inline constexpr std::array<ResidentBudgetDescriptor, 4> kResidentBudgets{{
    {"ledger_rows", "PF_RESIDENT_LEDGER_ROWS_ENTRIES", 64, 64,
     0, 0, false}, // bytes_per_entry/fixed_base_bytes: set by the owning lane
    // The unit is one unpinned spillable cohort row, not a live origin.
    // A few thousand rows already exceed every measured script's needs;
    // cap at 4096 (8x the default) to refuse accidental unbounded caching.
    {"idtable_entries", "PF_RESIDENT_IDTABLE_ENTRIES", 512, 4096,
     0, 0, false}, // lab sizeof/allocation measurements still owed; see below
    {"placement_snapshots", "PF_RESIDENT_PLACEMENT_SNAPSHOTS_ENTRIES", 256, 256,
     0, 0, false}, // bytes_per_entry/fixed_base_bytes: set by the owning lane
    {"journal_pages", "PF_RESIDENT_JOURNAL_PAGES_ENTRIES", 2048, 2048,
     0, 0, false}, // bytes_per_entry/fixed_base_bytes: set by the owning lane
}};

constexpr const ResidentBudgetDescriptor& resident_budget(ResidentKind kind) noexcept {
    return kResidentBudgets[static_cast<std::uint8_t>(kind)];
}

constexpr bool resident_budget_descriptors_valid() noexcept {
    for (const auto& row : kResidentBudgets) {
        if (row.default_entries == 0 || row.default_entries > row.max_entries
            || (row.measured && row.bytes_per_entry == 0)) return false;
    }
    return true;
}
static_assert(resident_budget_descriptors_valid(),
              "measured resident budgets must have a nonzero entry width");

// Id-table resident allocation formula (not a measured byte constant):
//   std::map node holding pair<const Offset, shared_ptr<Row>>
// + shared allocation/control block holding
//       Row = pair<const std::string, CohortFacts>
// + std::unordered_map node holding pair<const std::string, map::iterator>
// + both keys' non-SSO heap allocations + amortized hash-bucket storage
// + allocator overhead and any retained empty CohortFacts container capacity.
// DiskIdTable has no fixed per-key length bound: std::string::max_size(),
// scratch extent, and (when slab-bound) the manifest's aggregate B_U apply.
// Thus key heap bytes must remain an explicit variable in a complete bound.
// resident_layout() prints the standard type sizes; actual node/control-block
// overhead, bucket capacity and retained allocations must be measured on lab.
//
// Checked constexpr arithmetic: nullopt means uint64_t overflow. Unmeasured
// rows currently contribute zero byte placeholders; callers MUST label them
// "not measured", and must not present the resulting sum as a full RAM bound.
constexpr std::optional<std::uint64_t> resident_bytes(
        const ResidentBudgetDescriptor& row, std::uint64_t entries) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (row.bytes_per_entry != 0
        && entries > (maximum - row.fixed_base_bytes) / row.bytes_per_entry)
        return std::nullopt;
    return row.fixed_base_bytes + entries * row.bytes_per_entry;
}

constexpr std::optional<std::uint64_t> resident_bytes(
        ResidentKind kind, std::uint64_t entries) noexcept {
    return resident_bytes(resident_budget(kind), entries);
}

// Resolve at run setup and retain the result in the owning cache. No global
// memoization: a later run on the same host must observe its new environment.
// Only positive decimal uint64_t values <= the descriptor maximum are accepted.
std::uint64_t resident_entries(ResidentKind kind);

// Sum at effective (environment-resolved) budgets, checking products and sum.
// See measured above: zero placeholders are not a complete resident RAM plan.
std::uint64_t planned_resident_bytes();

} // namespace pineforge::detail
