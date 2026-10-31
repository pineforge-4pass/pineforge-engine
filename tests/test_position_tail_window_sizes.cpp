#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/source/pine_position_tail.hpp"
#include <pineforge/detail/read_through_cache.hpp>

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using pineforge::Trade;
using pineforge::source::detail::PositionTailSpill;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Directory {
    char path[64] = "/tmp/pineforge-tail-windows-XXXXXX";
    Directory() { require(::mkdtemp(path) != nullptr, "create scratch directory"); }
    ~Directory() { ::rmdir(path); }
};

std::vector<Trade> rows() {
    std::vector<Trade> result;
    for (std::size_t i = 0; i != 137; ++i) {
        Trade row{};
        // One id exceeds even the default window; other rows split footers
        // and ids across the deliberately non-page-aligned tiny window.
        row.entry_id.resize(i == 3 ? PositionTailSpill::kWindowBytes + 17 : i % 73);
        for (std::size_t j = 0; j != row.entry_id.size(); ++j)
            row.entry_id[j] = static_cast<char>((j * 37 + i * 11) % 256);
        row.exit_time = static_cast<std::int64_t>((i * 71) % 101) - 50;
        const std::array<double, 5> quantities{{0x1p54, 0.25, 0x1p-30, -0.0, 3.75}};
        row.qty = quantities[i % quantities.size()];
        row.entry_incarnation = i * 7919 + 1;
        row.is_long = i % 3 != 0;
        row.exit_id = i % 2 ? pineforge::source::kMarginCallLabel : "ordinary";
        result.push_back(std::move(row));
    }
    return result;
}

void check_row(const PositionTailSpill& spill, const PositionTailSpill::Row& actual,
               const Trade& expected) {
    require(actual.exit_time == expected.exit_time
                && std::memcmp(&actual.qty, &expected.qty, sizeof(double)) == 0
                && actual.entry_incarnation == expected.entry_incarnation
                && actual.is_long == expected.is_long
                && actual.margin_call == (expected.exit_id == pineforge::source::kMarginCallLabel)
                && actual.entry_id_size == expected.entry_id.size()
                && spill.entry_id_equals(actual, expected.entry_id),
            "every tail field and complete id survives window eviction");
    auto different = expected.entry_id;
    if (different.empty()) different.push_back('x');
    else different[different.size() / 2] ^= 1;
    require(!spill.entry_id_equals(actual, different), "tail compares every identifier byte");
}

void sweep(const char* directory, const std::vector<Trade>& reference, std::size_t capacity) {
    PositionTailSpill spill(directory, capacity);
    require(spill.cache_capacity_bytes() == capacity, "position tail honors its instance capacity");
    std::uint64_t expected_bytes = 0;
    for (const auto& row : reference) {
        spill.append(row);
        expected_bytes += row.entry_id.size() + PositionTailSpill::kFooterBytes;
    }
    require(spill.size_bytes() == expected_bytes && spill.row_count() == reference.size(),
            "cache capacity changes neither logical size nor retained rows");
    auto census_cursor = spill.size_bytes();
    PositionTailSpill::Row census_row;
    const auto allocations = global_allocation::allocations;
    global_allocation::live_bytes = 0;
    global_allocation::recording = true;
    const auto census_read = spill.previous(census_cursor, census_row);
    global_allocation::recording = false;
    require(census_read && global_allocation::live_bytes == spill.cache_stats().current_bytes
                && global_allocation::allocations - allocations == (capacity ? 1 : 0),
            "global allocation census matches every shared tail payload and metadata byte");
    for (int pass = 0; pass != 2; ++pass) {
        auto cursor = spill.size_bytes();
        for (auto it = reference.rbegin(); it != reference.rend(); ++it) {
            PositionTailSpill::Row row;
            require(spill.previous(cursor, row), "complete newest-first traversal");
            check_row(spill, row, *it);
        }
        PositionTailSpill::Row end;
        require(cursor == 0 && !spill.previous(cursor, end), "walk ends at the actual beginning");
    }
    // The previous walk ended at byte zero. The newest footer is separated
    // from it by more than every tested capacity. Count the inner file read,
    // independently of any cache hit/miss accounting.
    const auto before = spill.backing_read_count();
    auto cursor = spill.size_bytes();
    PositionTailSpill::Row newest;
    require(spill.previous(cursor, newest), "cold newest row remains readable");
    require(spill.backing_read_count() > before, "cold tail read reaches its backing");
    check_row(spill, newest, reference.back());
    const auto saved = newest;
    cursor = reference.front().entry_id.size() + PositionTailSpill::kFooterBytes;
    PositionTailSpill::Row oldest;
    require(spill.previous(cursor, oldest), "oldest row remains retained after eviction");
    check_row(spill, saved, reference.back()); // copy-out survives intervening refills
    const auto stats = spill.cache_stats();
    require(stats.get_calls > 0 && stats.misses > 0,
            "position reads use the shared cache, including bypass");
    require(stats.current_payload_bytes == capacity
                && stats.current_bytes == stats.current_payload_bytes + stats.current_metadata_bytes,
            "position shared cache discloses complete payload and line charges");
    spill.clear();
    spill.append(reference.front());
    cursor = spill.size_bytes();
    require(spill.previous(cursor, oldest), "tail can be reused after clear");
    check_row(spill, oldest, reference.front());
    std::printf("position_tail capacity=%zu backing_reads=%llu swept_rows=%zu\n",
                spill.cache_capacity_bytes(),
                static_cast<unsigned long long>(spill.backing_read_count()), reference.size());
}

void allocation_bypass(const char* directory, const std::vector<Trade>& reference) {
    const std::string scratch_directory(directory);
    PositionTailSpill spill(scratch_directory, PositionTailSpill::kWindowBytes);
    spill.append(reference.back());
    auto cursor = spill.size_bytes();
    PositionTailSpill::Row row;
    const auto refused = global_allocation::refused;
    // State and backing already exist. Refuse the shared cache's one lazy
    // allocation at the first actual footer read.
    global_allocation::fail_after = 0;
    require(spill.previous(cursor, row), "allocation bypass reads backing");
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    require(global_allocation::refused == refused + 1
                && spill.cache_stats().allocation_bypass_count == 1
                && spill.cache_capacity_bytes() == 0,
            "optional tail allocation failure selects permanent shared-cache bypass");
    check_row(spill, row, reference.back());
}
} // namespace

int main() {
    try {
        Directory directory;
        const auto reference = rows();
        for (const auto bytes : {std::size_t{0}, std::size_t{29}, std::size_t{30},
                                 std::size_t{4096}, PositionTailSpill::kWindowBytes})
            sweep(directory.path, reference, bytes);
        allocation_bypass(directory.path, reference);
        std::puts("position-tail capacity sweep passed");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::refusing = false;
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        std::fprintf(stderr, "position-tail capacity sweep: %s\n", error.what());
        return 1;
    }
}
