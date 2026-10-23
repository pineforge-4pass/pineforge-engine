// D-04 runtime red on 57e662382d: find/insert call read_key at every
// visited node, allocating that node's 32-byte string before comparing it.
#include "global_allocation_replacement.hpp"
#include "p2_id_table_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <map>
#include <utility>

namespace {
namespace probe = p2_id_table;
using probe::require;
using Table = pineforge::detail::DiskIdTable<probe::Record>;

void lookups_allocate_nothing(std::size_t count, bool disk) {
    const auto keys = probe::ids(count + 1);
    const std::array<std::string, 3> missing{{"", std::string(32, 'a'), std::string(32, 'z')}};
    Table table;
    if (disk) table.set_directory(".");
    else table.bind_scratch(probe::slab(count + 1, 32 * (count + 1)),
                            pineforge::detail::ScratchResource::cohort);
    for (std::size_t i = 0; i < count; ++i)
        require(table.insert(keys[i], probe::record(i)).second, "seed a distinct long key");

    // AVL height <= 2*ceil(log2(N+1)); a 32-byte key needs one links read
    // and at most one key read per level. This follows from AVL balance,
    // independently of the insertion shape or an observed run.
    const auto read_bound = 4 * probe::ceil_log2(count + 1);
    const auto lookup = [&](const std::string& key, bool present) {
        const auto allocations = global_allocation::allocations;
        const auto before = probe::io;
        const auto found = table.find(key);
        const auto allocated = global_allocation::allocations - allocations;
        const auto reads = probe::io.reads - before.reads;
        const auto writes = probe::io.writes - before.writes;
        require(found.has_value() == present, "lookup presence must match the seeded keys");
        require(allocated == 0, "D-04: lookup must allocate zero times at every population");
        require(reads > 0 && reads <= read_bound, "lookup follows a bounded AVL path");
        require(writes == 0, "lookup must perform zero store writes");
        return found;
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto found = lookup(keys[i], true);
        require(table.get(*found) == probe::record(i), "lookup preserves the key/value association");
    }
    for (const auto& key : missing) (void)lookup(key, false);

    const auto allocations = global_allocation::allocations;
    const auto before = probe::io;
    const auto duplicate = table.insert(keys[count - 1], probe::record(count + 1));
    require(global_allocation::allocations == allocations, "duplicate insert allocates zero times");
    require(!duplicate.second && probe::io.writes == before.writes,
            "duplicate insertion is read-only and retains the original value");
    require(table.get(duplicate.first) == probe::record(count - 1), "duplicate leaves value unchanged");

    const auto new_allocations = global_allocation::allocations;
    require(table.insert(keys[count], probe::record(count)).second, "insert another long key");
    require(global_allocation::allocations == new_allocations,
            "new insertion compares existing nodes without allocating");
}

void chunk_boundaries_preserve_byte_order() {
    const std::array<std::size_t, 9> lengths{{0, 1, 255, 256, 257, 511, 512, 513, 4097}};
    std::map<std::string, probe::Record> reference;
    for (const auto length : lengths) {
        std::string key(length, 'm');
        if (!key.empty()) key[length / 2] = '\0';
        reference.emplace(key, probe::record(reference.size()));
        if (!key.empty()) {
            key.back() = static_cast<char>(0xff);
            reference.emplace(key, probe::record(reference.size()));
        }
    }
    std::size_t bytes = 0;
    for (const auto& row : reference) bytes += row.first.size();
    Table table;
    table.bind_scratch(probe::slab(reference.size(), bytes),
                       pineforge::detail::ScratchResource::cohort);
    for (auto at = reference.rbegin(); at != reference.rend(); ++at)
        require(table.insert(at->first, at->second).second, "insert binary boundary key");
    auto expected = reference.begin();
    table.for_each_sorted([&](const auto& key, const auto& value) {
        require(expected != reference.end() && key == expected->first && value == expected->second,
                "chunked comparisons retain std::string byte order");
        ++expected;
    });
    require(expected == reference.end(), "sorted walk includes every boundary key");
    for (const auto& row : reference) {
        const auto allocations = global_allocation::allocations;
        const auto found = table.find(row.first);
        require(global_allocation::allocations == allocations, "long boundary lookup allocates nothing");
        require(found && table.get(*found) == row.second, "find compares the entire binary key");
        auto longer = row.first + '\0';
        require(!table.find(longer) == (reference.find(longer) == reference.end()),
                "proper prefixes and embedded NULs remain length based");
    }
}
} // namespace

int main() {
    try {
        for (const auto count : {std::size_t{17}, std::size_t{257}, std::size_t{4096}})
            for (const bool disk : {false, true}) lookups_allocate_nothing(count, disk);
        chunk_boundaries_preserve_byte_order();
        std::puts("disk_id_table_find_is_allocation_free: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "disk_id_table_find_is_allocation_free: %s\n", error.what());
        return 1;
    }
}
