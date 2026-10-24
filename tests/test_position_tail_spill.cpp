// CR-150 L18. Independent reference: the original Trade rows in a vector,
// walked backwards. No harvested outputs or hash pins.
// Fail-before must be compiled by the lab against the frozen header closure:
// this tree adds ../src/source/pine_position_tail.hpp / binding.hpp.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/source/pine_position_tail.hpp"
#include "../src/source/pine_position_tail_binding.hpp"

#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
using source::detail::PositionTailBinding;
using source::detail::PositionTailSpill;

struct Directory {
    char path[64] = "/tmp/pineforge-l18-unit-XXXXXX";
    Directory() { assert(::mkdtemp(path)); }
    ~Directory() { assert(::rmdir(path) == 0); }
};

std::size_t file_count(const char* path) {
    DIR* directory = ::opendir(path);
    assert(directory);
    std::size_t count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) ++count;
    }
    assert(::closedir(directory) == 0);
    return count;
}

Trade make_row(std::size_t index, std::size_t id_size) {
    Trade trade{};
    trade.entry_id.resize(id_size);
    for (std::size_t i = 0; i < id_size; ++i)
        trade.entry_id[i] = static_cast<char>((i * 37 + index) % 256);
    // Nonmonotone times; binary64 values of very different magnitudes make
    // an accidental reversal of the addition order observable.
    trade.exit_time = static_cast<std::int64_t>((index * 71) % 101) - 50;
    const std::array<double, 7> quantities{{0x1p54, 0.25, 1.0, 0x1p-30, -0.0, 3.75, 0.125}};
    trade.qty = quantities[index % quantities.size()];
    trade.entry_incarnation = static_cast<std::uint64_t>(index) * 7919 + 1;
    trade.is_long = index % 3 != 0;
    trade.exit_id = index % 2 ? source::kMarginCallLabel : "ordinary";
    return trade;
}

void equal_row(const PositionTailSpill& spill, const PositionTailSpill::Row& actual,
               const Trade& expected) {
    assert(actual.exit_time == expected.exit_time);
    assert(std::memcmp(&actual.qty, &expected.qty, sizeof actual.qty) == 0);
    assert(actual.entry_incarnation == expected.entry_incarnation);
    assert(actual.is_long == expected.is_long);
    assert(actual.margin_call == (expected.exit_id == source::kMarginCallLabel));
    assert(actual.entry_id_size == expected.entry_id.size());
    assert(spill.entry_id_equals(actual, expected.entry_id));
    std::string different = expected.entry_id;
    if (different.empty()) different = "x";
    else different[different.size() / 2] ^= 1;
    assert(!spill.entry_id_equals(actual, different));
}

void backwards(const char* directory) {
    PositionTailSpill spill(directory);
    std::vector<Trade> reference;
    std::uint64_t bytes = 0;
    std::uint64_t seed = 0x62b97314;
    for (std::size_t i = 0; i < 4096; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        reference.push_back(make_row(i, static_cast<std::size_t>(seed % 151)));
        spill.append(reference.back());
        bytes += reference.back().entry_id.size() + PositionTailSpill::kFooterBytes;
    }
    assert(spill.size_bytes() == bytes && spill.row_count() == reference.size());
    assert(file_count(directory) == 0); // private file was unlinked immediately
    for (int pass = 0; pass < 3; ++pass) {
        auto cursor = spill.size_bytes();
        auto second_cursor = cursor;
        for (auto it = reference.rbegin(); it != reference.rend(); ++it) {
            PositionTailSpill::Row row;
            assert(spill.previous(cursor, row));
            equal_row(spill, row, *it);
            // A second, independently restarted reader may refill the cache.
            PositionTailSpill::Row second;
            assert(spill.previous(second_cursor, second));
            equal_row(spill, second, *it);
        }
        PositionTailSpill::Row row;
        assert(cursor == 0 && !spill.previous(cursor, row));
    }
    spill.clear();
    assert(spill.size_bytes() == 0 && spill.row_count() == 0);
    const auto replacement = make_row(999, 31);
    spill.append(replacement);
    auto cursor = spill.size_bytes();
    PositionTailSpill::Row row;
    assert(spill.previous(cursor, row));
    equal_row(spill, row, replacement);
    assert(cursor == 0);
}

void file_clear_and_release(const char* directory) {
    PositionTailBinding::File file(directory);
    const pf_equity_point_t bytes{1, 2.0, 3.0};
    file.write(0, &bytes, sizeof(bytes));
    const int descriptor = file.descriptor();
    assert(descriptor >= 0);
    file.clear();
    struct stat status{};
    assert(file.descriptor() == descriptor);
    assert(::fstat(descriptor, &status) == 0 && status.st_size == 0);
    file.write(0, &bytes, sizeof(bytes));
    pf_equity_point_t actual{};
    file.read(0, &actual, sizeof(actual));
    assert(std::memcmp(&actual, &bytes, sizeof(bytes)) == 0);
    file.release();
    errno = 0;
    assert(file.descriptor() == -1);
    assert(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    file.write(0, &bytes, sizeof(bytes));
    file.read(0, &actual, sizeof(actual));
    assert(std::memcmp(&actual, &bytes, sizeof(bytes)) == 0);
}

void window_edges(const char* directory) {
    // Cut the first footer at every possible byte of a 64 KiB refill.
    for (std::size_t cut = 0; cut <= PositionTailSpill::kFooterBytes; ++cut) {
        PositionTailSpill spill(directory);
        const auto first = make_row(cut, 0);
        const auto last = make_row(cut + 1, PositionTailSpill::kWindowBytes
                                             - 2 * PositionTailSpill::kFooterBytes + cut);
        spill.append(first);
        spill.append(last);
        auto cursor = spill.size_bytes();
        PositionTailSpill::Row row;
        assert(spill.previous(cursor, row)); equal_row(spill, row, last);
        assert(spill.previous(cursor, row)); equal_row(spill, row, first);
        assert(cursor == 0);
    }
    PositionTailSpill spill(directory);
    const auto oversized = make_row(1, 3 * PositionTailSpill::kWindowBytes + 17);
    spill.append(oversized);
    auto cursor = spill.size_bytes();
    PositionTailSpill::Row row;
    assert(spill.previous(cursor, row));
    equal_row(spill, row, oversized);
}

double memory_fold(const std::vector<Trade>& reference, std::int64_t position_open) {
    double result = 0.0;
    for (auto it = reference.rbegin(); it != reference.rend(); ++it) {
        if (it->exit_time < position_open) break;
        if (it->is_long) result += it->qty;
    }
    return result;
}

double split_fold(const TradeLedger& resident, const PositionTailSpill* spill,
                  std::int64_t position_open) {
    double result = 0.0;
    for (std::size_t i = resident.size(); i > resident.base();) {
        const auto& row = resident[--i];
        if (row.exit_time < position_open) return result;
        if (row.is_long) result += row.qty;
    }
    if (spill) {
        auto cursor = spill->size_bytes();
        PositionTailSpill::Row row;
        while (spill->previous(cursor, row)) {
            if (row.exit_time < position_open) break;
            if (row.is_long) result += row.qty;
        }
    }
    return result;
}

void barriers_and_splits(const char* directory) {
    std::vector<Trade> reference;
    for (std::size_t i = 0; i < 12; ++i) reference.push_back(make_row(i, i * 7));
    // The newer rows on either side of a single old exit are reachable
    // only on their own side of that barrier.
    for (auto& row : reference) { row.exit_time = 100; row.is_long = true; }
    reference[5].exit_time = 9;
    for (bool batch : {false, true}) {
        for (std::size_t end = 0; end <= reference.size(); ++end) {
            TradeLedger resident;
            for (const auto& row : reference) resident.push_back(row);
            std::shared_ptr<PositionTailSpill> spill;
            if (batch) {
                PositionTailBinding::preserve(spill, resident, end, 10, directory);
                resident.seal(end);
            } else {
                // A barrier must work between publication calls as well as
                // within one batch.
                for (std::size_t i = 1; i <= end; ++i) {
                    PositionTailBinding::preserve(spill, resident, i, 10, directory);
                    resident.seal(i);
                }
            }
            const double want = memory_fold(reference, 10);
            const double got = split_fold(resident, spill.get(), 10);
            assert(std::memcmp(&want, &got, sizeof want) == 0);
            if (end > 5 && spill) assert(spill->row_count() == end - 6);
            PositionTailBinding::preserve(spill, resident, resident.base(),
                                         std::numeric_limits<std::int64_t>::min(), directory);
            assert(!spill);
        }
    }
}

void bounded_heap(const char* directory) {
    const auto row = make_row(1, 64);
    global_allocation::recording = true;
    const std::size_t before = global_allocation::live_bytes;
    {
        PositionTailSpill spill(directory);
        for (int i = 0; i < 1000; ++i) spill.append(row);
        const auto at_1k = global_allocation::live_bytes;
        // No allocation at all is needed after construction, including
        // reverse reads and comparisons of entry ids.
        const auto allocations = global_allocation::allocations;
        for (int i = 1000; i < 100000; ++i) spill.append(row);
        assert(spill.size_bytes() > PositionTailSpill::kWindowBytes);
        auto cursor = spill.size_bytes();
        PositionTailSpill::Row actual;
        while (spill.previous(cursor, actual))
            assert(spill.entry_id_equals(actual, row.entry_id));
        assert(spill.row_count() == 100000);
        assert(global_allocation::live_bytes == at_1k);
        assert(global_allocation::allocations == allocations);
        assert(at_1k - before <= PositionTailSpill::kWindowBytes + 4096);
    }
    assert(global_allocation::live_bytes == before);
    global_allocation::recording = false;
}

void failures(const char* directory) {
    bool refused = false;
    try { PositionTailSpill invalid(std::string(directory) + "/absent"); }
    catch (const std::system_error& error) {
        refused = true;
        assert(error.code().value() == ENOENT);
        assert(classify_run_failure(error).code == RunFailureCode::report_sink_failed);
    }
    assert(refused && file_count(directory) == 0);
    PositionTailSpill spill(directory);
    spill.append(make_row(0, 1));
    for (const auto invalid_cursor : {std::uint64_t{1}, spill.size_bytes() + 1}) {
        auto cursor = invalid_cursor;
        PositionTailSpill::Row row;
        refused = false;
        try { (void)spill.previous(cursor, row); }
        catch (const std::logic_error& error) {
            refused = true;
            assert(classify_run_failure(error).code == RunFailureCode::engine_invariant);
        }
        assert(refused && cursor == invalid_cursor);
    }
}
} // namespace

int main() {
    Directory directory;
    backwards(directory.path);
    file_clear_and_release(directory.path);
    window_edges(directory.path);
    barriers_and_splits(directory.path);
    bounded_heap(directory.path);
    failures(directory.path);
    assert(file_count(directory.path) == 0);
    std::puts("L18 position-tail: reverse rows, window edges, barriers, bounded heap and I/O");
}
