// WL-PTAILFIX. The oracle is the former hash_state loop, using the real
// BrokerStateHashSink and independently checked table contents.
// Expected fail-before on c15d224588: no ordered-fold template argument.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/engine.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Store = pineforge::detail::IdPairStore<std::int64_t, true>;
using Reference = std::map<std::string, std::int64_t>;
using Sink = pineforge::BrokerStateHashSink;
using IO = pineforge::detail::IdPairScratchFile::FixtureIO;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Deliberately retained enumerate / sort / relookup reference. This never
// calls the maintained fold, and never substitutes a scalar digest for bytes.
std::uint64_t old_sorted_fold(const Store& store, std::uint64_t seed) {
    std::vector<std::string> keys;
    keys.reserve(store.size());
    for (const auto& row : store) keys.push_back(row.first);
    std::sort(keys.begin(), keys.end());
    Sink f;
    f.h = seed;
    f.u(keys.size());
    for (const auto& key : keys) {
        f.s(key);
        f.i(store.at(key));
    }
    return f.h;
}

void check(const Store& store, const Reference& expected, std::mt19937_64& random,
           bool all_residues = false) {
    Reference observed;
    for (const auto& row : store)
        require(observed.emplace(row.first, row.second).second, "unique iterated key");
    require(observed == expected && store.size() == expected.size(), "table matches independent map");
    const auto compare = [&](std::uint64_t seed) {
        Sink actual;
        actual.h = seed;
        store.fold_sorted(actual);
        require(actual.h == old_sorted_fold(store, seed), "exact sorted-byte fold for incoming seed");
    };
    for (const auto seed : {UINT64_C(0), UINT64_C(1), UINT64_C(255), UINT64_C(256),
                           UINT64_C(0x8000000000000000), UINT64_MAX}) compare(seed);
    for (int i = 0; i < 4; ++i) compare(random());
    if (all_residues) {
        for (std::uint64_t low = 0; low < 256; ++low) {
            compare(low);
            compare((random() & ~UINT64_C(255)) | low);
        }
    }
}

void random_sets_and_mutations(bool memory) {
    Store store;
    if (memory) store.fixture_use_memory_reference();
    Reference expected;
    std::mt19937_64 random(0xb76321b5);
    std::vector<std::string> keys{
        "", "a", "ab", "abc", std::string("a\0", 2), std::string("a\0b", 3),
        std::string("\0", 1), std::string("\x1f", 1), std::string("a\x1f\0z", 4),
        std::string(513, 'L'), std::string(4097, 'L'), std::string(4097, '\xff'),
        std::string("\x7f", 1), std::string("\x80", 1), std::string("\xff", 1)
    };
    for (int i = 0; i < 240; ++i) {
        std::string key(static_cast<std::size_t>(random() % 40), '\0');
        for (auto& byte : key) byte = static_cast<char>(random());
        keys.push_back(std::move(key));
    }
    std::shuffle(keys.begin(), keys.end(), random);
    check(store, expected, random, true);
    for (const auto& key : keys) {
        const auto value = static_cast<std::int64_t>(random() >> 1);
        require(store.insert_or_assign(key, value) == expected.insert_or_assign(key, value).second,
                "unsorted binary-key insertion matches map");
        check(store, expected, random);
    }
    for (int step = 0; step < 1000; ++step) {
        const auto& key = keys[static_cast<std::size_t>(random() % keys.size())];
        if (step % 3 == 0) {
            require(store.erase(key) == (expected.erase(key) != 0), "erase agrees with map");
        } else {
            const auto value = step % 11 == 0 ? std::numeric_limits<std::int64_t>::min()
                : -static_cast<std::int64_t>(random() >> 1) - 1;
            require(store.insert_or_assign(key, value)
                        == expected.insert_or_assign(key, value).second,
                    "replacement with different signed value and reinsertion agree");
        }
        check(store, expected, random);
    }
    check(store, expected, random, true);
    Store moved(std::move(store));
    check(store, {}, random);
    Store assigned;
    assigned.insert_or_assign("discarded", 7);
    assigned = std::move(moved);
    check(assigned, expected, random, true);
    assigned.clear();
    require(assigned.disk_bytes() == 0, "clear releases fold, journal and table files");
    check(assigned, {}, random, true);
    assigned.insert_or_assign("new run", -19);
    check(assigned, {{"new run", -19}}, random);
    require(memory ? assigned.disk_bytes() == 0 : assigned.disk_bytes() > 0,
            "backend selection survives move and clear");
}

std::string fixed_key(std::uint64_t i) {
    // Fixed-length, general binary population, deliberately non-lexical insertion.
    std::string key(8, '\0');
    const auto value = i * UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned j = 0; j < 8; ++j) key[j] = static_cast<char>(value >> (j * 8));
    return key;
}

void bounded_work_and_heap() {
    IO io;
    Store store;
    store.fixture_count_io(&io);
    global_allocation::recording = true;
    for (std::uint64_t i = 0; i < 10; ++i) store.insert_or_assign(fixed_key(i), -17);
    const auto low = global_allocation::live_bytes;
    const auto start_reads = io.reads;
    const auto start_writes = io.writes;
    for (std::uint64_t i = 10; i < 10000; ++i) {
        store.insert_or_assign(fixed_key(i), -17);
        const auto reads = io.reads, writes = io.writes;
        global_allocation::refusing = true;
        Sink sink;
        sink.h = i * UINT64_C(0xd1342543de82ef95);
        store.fold_sorted(sink);
        global_allocation::refusing = false;
        require(io.reads == reads && io.writes == writes, "hash performs zero file IO at every population");
    }
    const auto high = global_allocation::live_bytes;
    global_allocation::recording = false;
    require(high <= low + 1024, "ordered fold heap is flat from 10 to 10000 keys");
    // 9 bits/symbol, at most 9*(8+1) radix levels; each bucket has <=16
    // records. Includes actual table probes, geometric rehashes and undo IO.
    constexpr std::uint64_t events = 9990;
    require(io.reads - start_reads < events * 4096
                && io.writes - start_writes < events * 512,
            "fixed-length mutation IO stays within a population-independent bound");
    require(store.disk_bytes() < 10000 * 700, "bucketed fold costs less than 700 disk bytes/key here");
    for (std::uint64_t i = 0; i < 10000; ++i) store.insert_or_assign(fixed_key(i), 29);
    // Visit every final path once: a split at the end of insertion may
    // introduce one deeper path than the journal has previously held.
    const auto disk = store.disk_bytes();
    for (std::uint64_t i = 0; i < 10000; ++i) store.insert_or_assign(fixed_key(i), -31);
    require(store.disk_bytes() == disk, "value updates reuse table, radix nodes and journal");
    std::printf("ordered fold: heap 10=%zu 10000=%zu; disk=%llu; reads=%llu writes=%llu\n",
                low, high, static_cast<unsigned long long>(disk),
                static_cast<unsigned long long>(io.reads - start_reads),
                static_cast<unsigned long long>(io.writes - start_writes));
}

void first_seen_iteration_and_extreme_values() {
    Store store;
    std::mt19937_64 random(1729);
    const std::vector<std::string> order{"z", std::string("x\0y", 3), "", "a", "ab"};
    Reference expected;
    for (const auto& key : order) {
        store.insert_or_assign(key, std::numeric_limits<std::int64_t>::max());
        expected[key] = std::numeric_limits<std::int64_t>::max();
    }
    store.erase(order[1]);
    expected.erase(order[1]);
    check(store, expected, random);
    store.insert_or_assign(order[1], std::numeric_limits<std::int64_t>::min());
    expected[order[1]] = std::numeric_limits<std::int64_t>::min();
    std::vector<std::string> seen;
    for (const auto& row : store) seen.push_back(row.first);
    require(seen == order, "ordered hash index leaves first-seen iteration and recurrence unchanged");
    check(store, expected, random, true);
}

void every_short_write_is_recoverable() {
    // Exercise append, split, value replacement, erase, and the deferred
    // table flush. Each trial fails a different actual pwrite boundary,
    // after writing a prefix. Hash/lookup/iteration must remain coherent.
    for (int operation = 0; operation < 6; ++operation) {
        bool saw_success = false;
        for (std::uint64_t fail = 1; !saw_success; ++fail) {
            IO io;
            Store store;
            Reference expected;
            std::mt19937_64 random(72591);
            store.fixture_count_io(&io);
            const std::uint64_t population = operation == 4 ? 0 : (operation == 5 ? 16 : 32);
            for (std::uint64_t i = 0; i < population; ++i) {
                store.insert_or_assign(fixed_key(i), static_cast<std::int64_t>(i));
                expected[fixed_key(i)] = static_cast<std::int64_t>(i);
            }
            const auto old_bytes = store.disk_bytes();
            const std::string key = operation == 0 || operation >= 4 ? fixed_key(10001)
                : operation == 1 ? std::string(700, 'Q') : fixed_key(7);
            const auto mutate = [&] {
                if (operation == 3) store.erase(key);
                else store.insert_or_assign(key, -711);
            };
            io.fail_write = io.writes + fail;
            constexpr std::array<std::size_t, 4> prefixes{7, 35, 1025, 2055};
            io.partial_write = prefixes[fail % prefixes.size()];
            try {
                mutate();
                saw_success = true;
            } catch (const pineforge::detail::IdPairStoreError& error) {
                require(error.code().value() == EIO
                            && pineforge::classify_run_failure(error).code
                                == pineforge::RunFailureCode::report_sink_failed,
                        "injected short write surfaces EIO and report_sink_failed");
            }
            io.fail_write = 0;
            if (!saw_success) {
                check(store, expected, random);
                require(store.disk_bytes() >= old_bytes, "failed write retains committed storage");
                // Move with an outstanding undo log and/or pending table write.
                Store moved(std::move(store));
                store = std::move(moved);
                // Restoration / deferred flush may itself fail again. The
                // old table and arbitrary-seed root must still be readable.
                io.fail_write = io.writes + 1;
                bool failed_again = false;
                try { mutate(); }
                catch (const pineforge::detail::IdPairStoreError& error) {
                    failed_again = error.code().value() == EIO;
                }
                require(failed_again, "second failure during retry is surfaced");
                io.fail_write = 0;
                check(store, expected, random);
                const auto reads = io.reads, writes = io.writes;
                Sink sink;
                sink.h = random();
                store.fold_sorted(sink);
                require(io.reads == reads && io.writes == writes,
                        "hash uses no IO even with an outstanding recovery");
                mutate();
            }
            if (operation == 3) expected.erase(key);
            else expected[key] = -711;
            check(store, expected, random, true);
            store.insert_or_assign("after recovery", 9);
            expected["after recovery"] = 9;
            check(store, expected, random);
        }
    }
}

void segmented_files_cover_the_old_population() {
    // Simulate a file-offset ceiling of 4095 bytes. The old
    // table can hold 64 keys: 128*16-byte slots at half load. A 2088-byte
    // branch file fits only ONE record per segment, so this exercises all
    // 64 descriptors without requiring exabyte sparse files on the host.
    using Record = std::array<std::uint64_t, 261>;
    pineforge::detail::IdPairFoldFile<Record, 4095> file;
    Record row{};
    for (std::uint64_t i = 0; i < 64; ++i) {
        for (std::size_t j = 0; j < row.size(); ++j) row[j] = i * 1009 + j;
        file.write(i, &row, {});
    }
    for (std::uint64_t i = 0; i < 64; ++i) {
        file.read(i, &row);
        for (std::size_t j = 0; j < row.size(); ++j)
            require(row[j] == i * 1009 + j, "segment boundary preserves all record bytes");
    }
    auto moved = std::move(file);
    moved.clear();
    require(!moved.opened(), "clear closes every segment");
}
} // namespace

int main() {
    try {
        random_sets_and_mutations(false);
        random_sets_and_mutations(true);
        first_seen_iteration_and_extreme_values();
        bounded_work_and_heap();
        every_short_write_is_recoverable();
        segmented_files_cover_the_old_population();
        std::puts("test_id_pair_ordered_fold: differential, bounded work/heap and short-write checks passed");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = global_allocation::recording = false;
        std::fprintf(stderr, "test_id_pair_ordered_fold: %s\n", error.what());
        return 1;
    }
}
