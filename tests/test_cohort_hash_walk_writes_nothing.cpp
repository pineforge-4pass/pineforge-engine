// D-05 runtime red on 57e662382d: sorted ++ calls spill(), so a walk
// restores each retired row and writes it back despite making no changes.
#include "global_allocation_replacement.hpp"
#include "p2_id_table_probe.hpp"
#include <pineforge/detail/disk_resident_id_table.hpp>

#include <cstdio>
#include <exception>
#include <map>
#include <utility>

namespace {
namespace probe = p2_id_table;
using probe::require;

struct Facts {
    using Record = probe::Record;
    Record value{};
    bool live = false;
    Record retired_record() const noexcept { return value; }
    static Facts restore_record(const Record& value) { return {value, false}; }
    bool spillable() const noexcept { return !live; }
};
using Table = pineforge::detail::DiskResidentIdTable<Facts>;
using Reference = std::map<std::string, probe::Record>;

template<class Owner>
void measured_walk(Owner& table, const Reference& reference) {
    auto expected = reference.begin();
    const auto before = probe::io;
    const auto allocations = global_allocation::allocations;
    std::size_t seen = 0;
    for (const auto& row : table.all_sorted()) {
        require(expected != reference.end() && row.first == expected->first
                    && row.second.value == expected->second,
                "sorted read returns each independent expected key and full record");
        ++expected;
        ++seen;
    }
    const auto allocated = global_allocation::allocations - allocations;
    const auto reads = probe::io.reads - before.reads;
    const auto writes = probe::io.writes - before.writes;
    require(expected == reference.end() && seen == reference.size(), "walk reaches every retired row");
    require(writes == 0, "D-05: a read-only sorted walk must perform zero store writes");
    // At most two retained row slots: Range::first may pin one, and the
    // advancing cursor needs one. Each slot owns one shared allocation, one
    // long key buffer and one map node, independent of historical count.
    require(allocated <= 2 * (1 + 1 + 1), "D-05: walk allocation count must be constant");
    // In-order AVL traversal reads <=3*N links; restoring all rows reads
    // <=3*N (links, key, record); recycling checks <=N stored records.
    require(reads > 0 && reads <= 7 * reference.size(), "walk store reads are linear");
    require(table.resident_size() <= 2, "a walk cannot retain historical rows on the heap");
}

void cohort_hash_walk_writes_nothing(std::size_t count, bool disk) {
    const auto keys = probe::ids(count);
    Table table;
    if (disk) table.set_directory(".");
    else table.bind_scratch(probe::slab(count, 32 * count),
                            pineforge::detail::ScratchResource::cohort);
    Reference reference;
    for (std::size_t i = 0; i < count; ++i) {
        auto placed = table.emplace(keys[i], Facts{probe::record(i), false});
        require(placed.second, "seed retired cohort");
        // Exercise real writeback before measuring the read-only walk.
        placed.first->second.value = probe::record(i + count);
        reference.emplace(keys[i], probe::record(i + count));
    }
    table.spill();
    require(table.size() == count && table.resident_size() == 0,
            "seed must actually spill all historical cohorts");
    measured_walk(std::as_const(table), reference);
    table.spill();
    require(table.resident_size() == 0, "release the const walk's pins before the mutable-owner walk");
    measured_walk(table, reference);
}

void copied_pins_nested_lookups_and_mutations() {
    const auto keys = probe::ids(17);
    Table table;
    table.bind_scratch(probe::slab(17, 17 * 32), pineforge::detail::ScratchResource::cohort);
    for (std::size_t i = 0; i < keys.size(); ++i)
        table.emplace(keys[i], Facts{probe::record(i), false});
    table.spill();

    std::string changed;
    {
        auto range = table.all_sorted();
        auto at = range.begin();
        auto saved = at++;
        const auto saved_key = saved->first;
        const auto saved_record = saved->second.value;
        changed = at->first;
        at->second.value = probe::record(9000);
        const auto nested = table.find(changed);
        require(&nested->second == &at->second, "nested mutable lookup retains row identity");
        ++at;
        require(saved->first == saved_key && saved->second.value == saved_record,
                "advancing a copy cannot recycle the saved row");
        for (; at != range.end(); ++at) (void)at->second.value;
    }
    table.spill();
    require(table.find(changed)->second.value == probe::record(9000),
            "mutable sorted iterator still writes changed facts back");
    table.spill();

    const auto dirty_key = keys.back();
    {
        auto dirty = table.find(dirty_key);
        dirty->second.value = probe::record(12345);
    }
    const auto before = probe::io;
    {
        const auto& owner = std::as_const(table);
        for (const auto& row : owner.all_sorted())
            if (row.first == dirty_key)
                require(row.second.value == probe::record(12345), "const walk sees dirty resident facts");
    }
    require(probe::io.writes == before.writes, "const walk must leave dirty residents unwritten");
    table.spill();
    require(table.find(dirty_key)->second.value == probe::record(12345),
            "later explicit writeback preserves the dirty row");
}
} // namespace

int main() {
    try {
        for (const auto count : {std::size_t{17}, std::size_t{257}, std::size_t{4096}})
            for (const bool disk : {false, true}) cohort_hash_walk_writes_nothing(count, disk);
        copied_pins_nested_lookups_and_mutations();
        std::puts("cohort_hash_walk_writes_nothing: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cohort_hash_walk_writes_nothing: %s\n", error.what());
        return 1;
    }
}
