// CX-D-P1: exact-sized, byte-for-byte replay of both L19l-A table shapes.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "dport_id_arena_fixture.hpp"

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/disk_resident_id_table.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace dport_id_fixture;

// Match the five scalar fields of the adapter's private retired codec.
// Live rows and iterator pins are exercised without exposing private state.
struct RetiredRecord {
    std::uint64_t handle;
    std::int64_t cycle;
    std::uint64_t count, digest, sides;
};
static_assert(sizeof(RetiredRecord) == 40, "retired cohort record shape");
struct Facts {
    using Record = RetiredRecord;
    Record record{};
    bool live = false;
    Record retired_record() const noexcept { return record; }
    static Facts restore_record(const Record& value) { return {value, false}; }
    bool spillable() const noexcept { return !live; }
};
using Cohorts = pineforge::detail::DiskResidentIdTable<Facts>;
using Ranks = pineforge::detail::DiskIdTable<std::uint32_t>;
using Record = RetiredRecord;
constexpr std::uint64_t kLinks = 7 * sizeof(std::uint64_t);
constexpr std::size_t kIds = 32768;

bool same(Record a, Record b) { return std::memcmp(&a, &b, sizeof a) == 0; }
Record record_for(std::size_t i, bool updated) {
    const auto n = static_cast<std::uint64_t>(i);
    return {n + 1, static_cast<std::int64_t>(updated ? n + kIds : n),
            n * 3 + (updated ? 2U : 1U), (n + 17) * 1099511628211ULL,
            updated ? (n + 1) % 4 : n % 4};
}
std::uint32_t rank_for(std::size_t i, bool updated) {
    return static_cast<std::uint32_t>(updated ? kIds - i - 1 : i);
}

struct Workload {
    std::vector<std::string> keys;
    std::vector<std::size_t> sorted;
    std::uint64_t cohort_bytes = 1, rank_bytes = 1;
    Workload() {
        keys.reserve(kIds);
        for (std::size_t i = 0; i < kIds; ++i) {
            // Odd multiplier permutes a power-of-two population; include
            // long and binary ids, plus the empty string as an ordinary id.
            const auto ordinal = (i * 7919) % kIds;
            keys.push_back(i == 0 ? std::string{} :
                "distinct-entry-with-a-non-SSO-key/" + std::to_string(ordinal)
                    + std::string("\0tail", 5));
            cohort_bytes += kLinks + sizeof(Record) + keys.back().size();
            rank_bytes += kLinks + sizeof(std::uint32_t) + keys.back().size();
        }
        sorted.resize(keys.size());
        std::iota(sorted.begin(), sorted.end(), std::size_t{0});
        std::sort(sorted.begin(), sorted.end(), [&](auto a, auto b) {
            return keys[a] < keys[b];
        });
    }
    std::uint64_t arena_bytes() const { return rounded(cohort_bytes) + rounded(rank_bytes); }
};

struct Result {
    std::vector<Record> records;
    std::vector<std::uint32_t> ranks;
    std::vector<std::uint64_t> sorted_handles, inserted_handles;
    std::vector<std::uint32_t> sorted_ranks, inserted_ranks;
    explicit Result(std::size_t count)
        : records(count), ranks(count), sorted_handles(count), inserted_handles(count),
          sorted_ranks(count), inserted_ranks(count) {}
};

std::size_t heap_start = 0, heap_peak = 0;
void allocated(std::size_t) {
    heap_peak = std::max(heap_peak, global_allocation::live_bytes - heap_start);
}
struct HeapWindow {
    HeapWindow() {
        heap_start = global_allocation::live_bytes;
        heap_peak = 0;
        global_allocation::recording = true;
        global_allocation::on_recorded = allocated;
    }
    ~HeapWindow() {
        global_allocation::on_recorded = nullptr;
        global_allocation::recording = false;
    }
};

template<class Census>
Result replay(const Workload& work, const std::string& directory, bool arena, Census&& census) {
    Result result(work.keys.size());
    // Result/reference storage is test-owned and allocated outside the
    // table measurement. The component retains only its bounded live rows.
    std::size_t measured_peak = 0;
    {
        HeapWindow measured;
        {
            Cohorts cohorts;
            Ranks ranks;
            cohorts.set_directory(directory);
            ranks.set_directory(directory);
            for (std::size_t i = 0; i < work.keys.size(); ++i) {
                require(cohorts.emplace(work.keys[i], Facts{record_for(i, false), false}).second,
                        "insert a distinct cohort");
                require(ranks.insert(work.keys[i], rank_for(i, false)).second,
                        "insert a distinct rank key");
                require(cohorts.resident_size() <= 1, "retired history stays off heap");
                if (i % 1024 == 0) census();
            }
            cohorts.spill();
            require(cohorts.size() == work.keys.size() && cohorts.resident_size() == 0,
                    "all cohort records are evicted");
            require(ranks.size() == work.keys.size() && ranks.file_bytes() == work.rank_bytes,
                    "rank file has the independently computed exact length");
            if (arena) {
                const auto stats = ScratchArena::attached()->stats();
                require(stats.bytes_used == work.arena_bytes()
                        && stats.high_water == work.arena_bytes() && stats.windows_mapped_bytes <= 65536,
                        "two logical files obey computed extents and the mapping cap");
                // set_directory is deliberately harmless after arena spill.
                cohorts.set_directory("/dev/null/changed-after-arena-spill");
                ranks.set_directory("/dev/null/changed-after-arena-spill");
            } else {
                bool refused = false;
                try { ranks.set_directory(directory + "/different"); }
                catch (const std::logic_error&) { refused = true; }
                require(refused, "file-mode directory change after spill is still refused");
            }
            {
                auto pin = cohorts.find(work.keys.front());
                pin->second.live = true;
                for (std::size_t i = 0; i < work.keys.size(); ++i) {
                    auto found = cohorts.find(work.keys[i]);
                    require(found != cohorts.end()
                            && same(found->second.retired_record(), record_for(i, false)),
                            "read every original cohort record");
                    found->second.record = record_for(i, true);
                    const auto rank = ranks.find(work.keys[i]);
                    require(rank && ranks.get(*rank) == rank_for(i, false),
                            "read every original rank value");
                    ranks.set(*rank, rank_for(i, true));
                    require(!cohorts.emplace(work.keys[i], Facts{}).second
                            && !ranks.insert(work.keys[i], 0).second,
                            "recurring ids keep their original record");
                    require(cohorts.resident_size() <= 2, "nested pinned lookups remain bounded");
                }
                const Cohorts& view = cohorts;
                require(&view.find(work.keys.front())->second == &pin->second,
                        "const nested lookup preserves the pinned row identity");
                pin->second.live = false;
            }
            cohorts.spill();
            require(cohorts.resident_size() == 0, "released pins are evicted");
            const Cohorts& view = cohorts;
            for (std::size_t i = 0; i < work.keys.size(); ++i) {
                const auto found = view.find(work.keys[i]);
                require(found != view.end(), "restore every updated cohort");
                result.records[i] = found->second.retired_record();
                result.ranks[i] = ranks.get(*ranks.find(work.keys[i]));
                require(same(result.records[i], record_for(i, true))
                        && result.ranks[i] == rank_for(i, true),
                        "complete updated readback matches the independent record model");
            }
            cohorts.spill();
            std::size_t at = 0;
            for (const auto& row : view.all_sorted()) {
                const auto expected = work.sorted.at(at);
                result.sorted_handles[at++] = row.second.retired_record().handle;
                require(row.first == work.keys[expected]
                        && same(row.second.retired_record(), record_for(expected, true)),
                        "sorted resident-table replay reads every record in key order");
            }
            require(at == work.keys.size(), "sorted replay visits every cohort");
            at = 0;
            ranks.for_each_sorted([&](const auto& key, auto value) {
                const auto expected = work.sorted.at(at);
                result.sorted_ranks[at++] = value;
                require(key == work.keys[expected] && value == rank_for(expected, true),
                        "sorted AVL walk matches independent key order");
            });
            require(at == work.keys.size(), "sorted replay visits every rank");
            at = 0;
            view.for_each_id([&](const auto& key) {
                require(key == work.keys.at(at), "cohort insertion order survives recurrence");
                result.inserted_handles[at++] = view.find(key)->second.retired_record().handle;
            });
            require(at == work.keys.size(), "insertion replay visits every cohort");
            at = 0;
            ranks.for_each_inserted([&](const auto& key, auto value) {
                result.inserted_ranks[at] = value;
                require(key == work.keys.at(at) && value == rank_for(at, true),
                        "rank insertion order survives updates");
                ++at;
            });
            require(at == work.keys.size() && ranks.file_bytes() == work.rank_bytes,
                    "updates and walks do not grow the index file");
            census();
            Cohorts moved(std::move(cohorts));
            Ranks moved_ranks(std::move(ranks));
            require(moved.size() == work.keys.size() && moved_ranks.size() == work.keys.size()
                    && ranks.size() == 0, "move transfers both logical files");
            moved.clear();
            moved_ranks.clear();
            require(moved.size() == 0 && moved.resident_size() == 0
                    && moved_ranks.file_bytes() == 0, "clear resets both tables");
            if (arena) require(ScratchArena::attached()->stats().bytes_used == 0,
                               "clear returns every arena chunk");
            // A fresh row reuses released, possibly dirty chunks.
            require(moved.emplace("fresh", Facts{record_for(0, false), false}).second
                    && moved_ranks.insert("fresh", 9).second, "reuse after reset");
            moved.spill();
            require(same(moved.find("fresh")->second.retired_record(), record_for(0, false))
                    && moved_ranks.get(*moved_ranks.find("fresh")) == 9,
                    "reset/reuse reads exactly the new records");
        }
        measured_peak = heap_peak;
    }
    // The foundation owns a uint32_t per claimed chunk, with capacity less
    // than twice the chunk count. Include that metadata rather than claiming
    // the arena itself has literally constant heap as the backing grows.
    const auto chunks = work.arena_bytes() / kChunk;
    const auto bound = 16ULL * 1024ULL + (arena ? 2 * chunks * sizeof(std::uint32_t) : 0);
    require(measured_peak <= bound, "table heap exceeds fixed live-row plus chunk-metadata bound");
    return result;
}

void compare(const Result& actual, const Result& expected) {
    require(actual.records.size() == expected.records.size()
            && actual.ranks == expected.ranks
            && actual.sorted_handles == expected.sorted_handles
            && actual.inserted_handles == expected.inserted_handles
            && actual.sorted_ranks == expected.sorted_ranks
            && actual.inserted_ranks == expected.inserted_ranks,
            "file/arena complete replay agrees");
    for (std::size_t i = 0; i < actual.records.size(); ++i)
        require(same(actual.records[i], expected.records[i]),
                "every retired record is byte-identical across backends");
}

#if defined(__linux__)
template<class Value> void boundary_refusal() {
    using Index = pineforge::detail::DiskIdTable<Value>;
    const auto header = kLinks + sizeof(Value);
    const std::string first(static_cast<std::size_t>(kChunk - header - 1), 'a');
    const std::string next(static_cast<std::size_t>(kChunk + 1), 'b');
    const auto complete = 1 + header + first.size() + header + next.size();
    const Backing backing(rounded(complete) - kChunk);
    child([&] {
        Attachment attached(backing);
        RegularFiles files;
        Index index;
        index.set_directory("/dev/null/no-file-fallback");
        const Value original{};
        const auto first_at = index.insert(first, original).first;
        require(index.file_bytes() == kChunk, "first row ends exactly on a chunk boundary");
        const auto before = ScratchArena::attached()->stats();
        exhausted([&] { index.insert(next, original); });
        const auto after = ScratchArena::attached()->stats();
        require(index.size() == 1 && index.file_bytes() == kChunk
                && after.bytes_used == before.bytes_used && after.high_water == before.high_water
                && after.chunks_free == before.chunks_free,
                "refused full-row reservation publishes nothing and claims no partial chunk");
        require(index.find(first) == first_at && !index.find(next),
                "refused insertion preserves the old search tree");
        const auto readback = index.get(first_at);
        require(std::memcmp(&readback, &original, sizeof original) == 0,
                "refused insertion preserves the existing record bytes");
        index.set(first_at, original);
        require(index.insert("small-after-refusal", original).second,
                "a smaller insert can use the chunk the refusal did not consume");
        files.check();
    });
}

void resident_boundary_refusal() {
    const auto header = kLinks + sizeof(Record);
    const std::string first(static_cast<std::size_t>(kChunk - header - 1), 'a');
    const std::string next(static_cast<std::size_t>(kChunk + 1), 'b');
    const auto complete = 1 + header + first.size() + header + next.size();
    const Backing backing(rounded(complete) - kChunk);
    child([&] {
        Attachment attached(backing);
        RegularFiles files;
        Cohorts cohorts;
        cohorts.set_directory("/dev/null/no-resident-file-fallback");
        const auto original = record_for(0, false);
        {
            auto pin = cohorts.emplace(first, Facts{original, true}).first;
            const auto before = ScratchArena::attached()->stats();
            exhausted([&] { cohorts.emplace(next, Facts{record_for(1, false), false}); });
            const auto after = ScratchArena::attached()->stats();
            require(cohorts.size() == 1 && cohorts.resident_size() == 1
                    && after.bytes_used == before.bytes_used
                    && after.high_water == before.high_water,
                    "refused resident insertion preserves pins, count and arena allocation");
            require(cohorts.find(next) == cohorts.end()
                    && &cohorts.find(first)->second == &pin->second
                    && same(pin->second.retired_record(), original),
                    "refusal preserves pinned identity and every original field");
            pin->second.live = false;
        }
        cohorts.spill();
        require(cohorts.resident_size() == 0
                && same(cohorts.find(first)->second.retired_record(), original),
                "existing row still evicts and restores after refusal");
        files.check();
    });
}
#endif
} // namespace

int main() {
    try {
        require(!ScratchArena::attached(), "file reference starts without an arena");
        const Workload work;
        Directory directory;
        const auto expected = replay(work, directory.path, false, [] {});
#if defined(__linux__)
        const Backing backing(work.arena_bytes());
        child([&] {
            Environment window("PF_SCRATCH_WINDOW_BYTES", "65536");
            Attachment attached(backing);
            RegularFiles files;
            compare(replay(work, "/dev/null/arena-must-ignore-directory", true,
                           [&] { files.check(); }), expected);
            files.check();
            require(ScratchArena::attached()->stats().bytes_used == 0,
                    "all replay regions are released");
        });
        boundary_refusal<Record>();
        boundary_refusal<std::uint32_t>();
        resident_boundary_refusal();
#else
        compare(replay(work, directory.path, false, [] {}), expected);
        std::puts("SKIP: sealed-memfd id-table arena/jail checks require Linux");
#endif
        std::puts("test_disk_id_tables_arena: full record, ordering, heap and refusal witnesses");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        global_allocation::recording = false;
        std::fprintf(stderr, "test_disk_id_tables_arena: %s\n", error.what());
        return 1;
    }
}
