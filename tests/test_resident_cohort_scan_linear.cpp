// D-06 runtime red on 57e662382d: a missing resident key compares all R
// resident strings; the caller's resident_ids/find_resident loop is quadratic.
#include "global_allocation_replacement.hpp"
#include "p2_resident_key_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace {
namespace probe = p2_id_table;
using probe::require;

struct Facts {
    using Record = probe::Record;
    Record value{};
    bool live = true;
    Record retired_record() const noexcept { return value; }
    static Facts restore_record(const Record& value) { return {value, false}; }
    bool spillable() const noexcept { return !live; }
};
using Table = pineforge::detail::DiskResidentIdTable<Facts>;
using Key = probe::CountedKey;

template<class Owner>
void bounded_lookup(Owner& table, const Key& key, bool present, std::size_t residents) {
    const auto comparisons = probe::key_comparisons;
    const auto allocations = global_allocation::allocations;
    const auto before = probe::io;
    const auto found = table.find_resident(key);
    const auto compared = probe::key_comparisons - comparisons;
    require((found != table.end()) == present, "resident lookup has the expected membership");
    require(probe::io.reads == before.reads && probe::io.writes == before.writes,
            "resident lookup never touches the backing store");
    require(global_allocation::allocations == allocations, "resident lookup must allocate zero times");
    // The AVL minimum-node recurrence gives height <=2*ceil(log2(R+1)).
    // Exactly one whole-key comparison is required per visited index node.
    require(compared > 0 && compared <= 2 * probe::ceil_log2(residents + 1),
            "D-06: resident lookup must follow a logarithmic comparison path");
    if (present) require(found->first == key, "resident index returns the matching whole string");
}

void resident_cohort_scan_linear(std::size_t count) {
    const auto ordinary = probe::ids(count);
    std::vector<Key> keys;
    keys.reserve(count);
    for (const auto& key : ordinary) keys.emplace_back(key);
    const Key missing(std::string(32, 'z'));
    Table table;
    table.bind_scratch(probe::slab(count, count * 32), pineforge::detail::ScratchResource::cohort);
    for (std::size_t i = 0; i < count; ++i)
        require(table.emplace(keys[i], Facts{probe::record(i), true}).second, "seed a live cohort");
    require(table.resident_size() == count, "all seeded cohorts must remain live residents");

    // On the base this necessarily compares count strings, including at 17.
    bounded_lookup(table, missing, false, count);
    bounded_lookup(std::as_const(table), missing, false, count);
    for (const auto& key : keys) {
        bounded_lookup(table, key, true, count);
        bounded_lookup(std::as_const(table), key, true, count);
    }
    const auto comparisons = probe::key_comparisons;
    std::size_t visited = 0;
    for (const auto& key : table.resident_ids()) {
        const auto found = table.find_resident(key);
        require(found != table.end(), "caller's resident-id replay resolves each cohort");
        ++visited;
    }
    require(visited == count, "resident-id replay includes every live cohort");
    require(probe::key_comparisons - comparisons <= count * 2 * probe::ceil_log2(count + 1),
            "resident-id lookup replay costs R logarithmic lookups");

    Table moved(std::move(table));
    require(table.resident_size() == 0 && table.find_resident(keys[0]) == table.end(),
            "move leaves no resident index in the source");
    for (std::size_t i = 0; i < count; ++i)
        require(moved.find_resident(keys[i])->second.value == probe::record(i),
                "move preserves every resident index entry and payload");
    Table assigned;
    assigned.bind_scratch(probe::slab(1, 32), pineforge::detail::ScratchResource::cohort);
    assigned.emplace(missing, Facts{probe::record(999), true});
    assigned = std::move(moved);
    require(moved.find_resident(keys[0]) == moved.end()
                && assigned.find_resident(missing) == assigned.end(),
            "move assignment removes both old destination and source index roots");

    // Erase in reverse insertion order, checking survivors after each erase.
    // This exercises index removals independently of the disk's AVL shape.
    for (std::size_t i = count; i != 0; --i) {
        {
            auto at = assigned.find_resident(keys[i - 1]);
            require(at != assigned.end(), "cohort exists before retirement");
            at->second.live = false;
        }
        assigned.spill();
        require(assigned.resident_size() == i - 1
                    && assigned.find_resident(keys[i - 1]) == assigned.end(),
                "retirement removes the resident index entry");
        if (i > 1) bounded_lookup(assigned, keys[0], true, i - 1);
    }
    // Restoring retired rows and recycling the sorted cursor must maintain
    // the same resident index without adding historical entries.
    {
        auto restored = assigned.find(keys[count / 2]);
        require(restored != assigned.end(), "restore an old disk cohort");
        require(&assigned.find_resident(keys[count / 2])->second == &restored->second,
                "restored row is indexed with its shared pin identity");
    }
    assigned.spill();
    {
        auto range = assigned.all_sorted();
        for (auto at = range.begin(); at != range.end(); ++at) {
            const auto resident = assigned.find_resident(at->first);
            require(resident != assigned.end() && &resident->second == &at->second,
                    "sorted cursor recycling updates the resident index");
        }
    }
    assigned.spill();
    require(assigned.resident_size() == 0 && assigned.find_resident(keys[0]) == assigned.end(),
            "no resident name survives complete retirement");
    assigned.clear();
    require(assigned.emplace(keys[0], Facts{probe::record(42), true}).second
                && assigned.find_resident(keys[0])->second.value == probe::record(42),
            "clear permits the same name to recur with fresh facts");
}

void resident_index_rotation_and_removal_shapes() {
    auto ordinary = probe::ids(17);
    std::sort(ordinary.begin(), ordinary.end());
    std::vector<Key> keys;
    for (const auto& key : ordinary) keys.emplace_back(key);
    const Key missing(std::string(32, 'z'));
    // Ascending, descending, and a coprime permutation force both rotation
    // directions. Remove the middle key first, then a coprime permutation.
    for (const int shape : {0, 1, 2}) {
        Table table;
        table.bind_scratch(probe::slab(17, 17 * 32), pineforge::detail::ScratchResource::cohort);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto slot = shape == 0 ? i : shape == 1 ? 16 - i : (i * 7) % 17;
            require(table.emplace(keys[slot], Facts{probe::record(slot), true}).second,
                    "seed adversarial resident insertion order");
        }
        std::array<bool, 17> removed{};
        for (std::size_t step = 0; step < keys.size(); ++step) {
            const auto victim = (8 + step * 7) % 17;
            {
                auto at = table.find_resident(keys[victim]);
                require(at != table.end(), "victim is still indexed");
                at->second.live = false;
            }
            table.spill();
            removed[victim] = true;
            const auto left = 16 - step;
            require(table.resident_size() == left, "each removal changes only one resident");
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const auto at = table.find_resident(keys[i]);
                require((at == table.end()) == removed[i], "removal preserves all other memberships");
                if (!removed[i]) {
                    require(at->second.value == probe::record(i), "rotation preserves the complete payload");
                    bounded_lookup(table, keys[i], true, left);
                }
            }
            if (left != 0) bounded_lookup(table, missing, false, left);
        }
    }
}
} // namespace

int main() {
    try {
        for (const auto count : {std::size_t{17}, std::size_t{257}, std::size_t{4096}})
            resident_cohort_scan_linear(count);
        resident_index_rotation_and_removal_shapes();
        std::puts("resident_cohort_scan_linear: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "resident_cohort_scan_linear: %s\n", error.what());
        return 1;
    }
}
