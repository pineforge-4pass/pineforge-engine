// WL-PTAILFIX. The oracle is the former hash_state loop, using the real
// BrokerStateHashSink and independently checked table contents.
// Original WL-PTAILFIX fail-before target c15d224588 lacked the fold template argument.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/id_pair_slab_fold.hpp>
#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/engine.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Store = pineforge::detail::IdPairStore<std::int64_t, true>;
using Reference = std::map<std::string, std::int64_t>;
using Sink = pineforge::BrokerStateHashSink;
using IO = pineforge::detail::IdPairScratchFile::FixtureIO;
using Work = Store::FixtureFoldWork;

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

void bounded_slab_keeps_the_fold_without_files() {
    using namespace pineforge::detail;
    const std::vector<std::string> keys{
        "z", "", "a", "ab", std::string("a\0", 2), std::string("a\0b", 3),
        std::string("\0", 1), std::string("\x1f", 1), std::string("\x80", 1),
        std::string("\xff", 1), std::string(513, 'L'), std::string(4097, 'L')
    };
    ScratchManifest manifest;
    manifest.K_c = keys.size();
    for (const auto& key : keys) manifest.B_c += key.size();
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Store store;
    IO io;
    store.fixture_count_io(&io);
    store.set_directory("/dev/null/pineforge-id-pair-fold"); // Any file open must fail.
    store.bind_scratch_tables(slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    store.fixture_force_hash_collisions(7);
    Reference expected;
    std::mt19937_64 random(0x150f01d);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        global_allocation::refusing = true;
        const bool inserted = store.insert_or_assign(keys[i], static_cast<std::int64_t>(i));
        global_allocation::refusing = false;
        require(inserted, "folded slab admits each distinct binary key without allocation");
        expected[keys[i]] = static_cast<std::int64_t>(i);
        check(store, expected, random);
    }
    require(store.size() == keys.size() && store.disk_bytes() == 0,
            "folded slab population occupies only its admitted regions");
    const auto extent = slab->region(ScratchResource::consumed_data).logical_extent;
    std::vector<unsigned char> data(static_cast<std::size_t>(extent));
    slab->read_at(ScratchResource::consumed_data, 0, data.data(), data.size());
    bool refused = false;
    try { store.insert_or_assign("one key too many", 1); }
    catch (const ScratchCapacityError& error) {
        refused = pineforge::classify_run_failure(error).code
            == pineforge::RunFailureCode::scratch_capacity_exceeded;
    }
    std::vector<unsigned char> after(data.size());
    slab->read_at(ScratchResource::consumed_data, 0, after.data(), after.size());
    require(refused && data == after
                && slab->region(ScratchResource::consumed_data).logical_extent == extent,
            "refusal leaves the folded slab's rows and extent unchanged");
    check(store, expected, random, true);
    for (int step = 0; step < 48; ++step) {
        const auto& key = keys[static_cast<std::size_t>(step) % keys.size()];
        if (step < 24) {
            require(store.erase(key) == (expected.erase(key) != 0), "folded slab erasure");
        } else {
            const auto value = std::numeric_limits<std::int64_t>::min() + step;
            require(store.insert_or_assign(key, value)
                        == expected.insert_or_assign(key, value).second,
                    "folded slab resurrection and replacement");
        }
        check(store, expected, random);
    }
    std::vector<std::string> seen;
    for (const auto& row : store) seen.push_back(row.first);
    require(seen == keys, "folded slab retains first-seen iteration under full hash collisions");
    Store moved(std::move(store));
    check(store, {}, random, true);
    check(moved, expected, random, true);
    moved.clear();
    check(moved, {}, random, true);
    moved.insert_or_assign("new run", -19);
    check(moved, {{"new run", -19}}, random, true);
    global_allocation::refusing = true;
    Sink sink;
    moved.fold_sorted(sink);
    global_allocation::refusing = false;
    require(moved.disk_bytes() == 0 && io.reads == 0 && io.writes == 0,
            "folded slab reset, mutation and arbitrary-seed hashing perform no file IO");
}

struct SlabImage {
    std::array<std::vector<unsigned char>, pineforge::detail::kScratchRegionCount> bytes;
    std::array<std::uint64_t, pineforge::detail::kScratchRegionCount> extents{};
    explicit SlabImage(const pineforge::detail::ScratchSlab& slab) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto& region = slab.regions()[i];
            extents[i] = region.logical_extent;
            bytes[i].resize(static_cast<std::size_t>(region.logical_extent));
            slab.read_at(region.resource, 0, bytes[i].data(), bytes[i].size());
        }
    }
    bool operator==(const SlabImage& other) const { return bytes == other.bytes && extents == other.extents; }
};

void slab_work_counters_observe_the_old_byte_loop() {
    using namespace pineforge::detail;
    ScratchManifest manifest;
    manifest.K_c = 2;
    manifest.B_c = 2;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Store store;
    Work work;
    store.fixture_count_fold_work(&work);
    store.bind_scratch_tables(slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    store.insert_or_assign("x", 1);
    require(work.byte_steps == 257 * (8 + 1 + 8),
            "fold counter observes all real sink byte steps for one row");
    const auto before = work;
    store.insert_or_assign("y", -2);
    require(work.row_comparisons > before.row_comparisons
                && work.byte_steps - before.byte_steps == 2 * 257 * (8 + 1 + 8),
            "comparison and byte counters include every row in a two-row bucket");
}

// Expected RED on ba5bfdcfb2 with ONLY the FixtureFoldWork observation
// hunks backported: "4096-key slab fold exceeds 13 row comparisons/write".
// The unchanged selection loop is observed by compare_rows and append too;
// none of the counters depend on entering the new slab-fold implementation.
void slab_work_bound_and_oracle_4096(bool collide) {
    using namespace pineforge::detail;
    constexpr std::size_t capacity = 4096;
    constexpr std::uint64_t byte_bound = 257 * 257 * (8 + 16);
    auto manifest = default_scratch_manifest(0);
    require(manifest.K_c == capacity, "work fixture uses the production consumed-key admission");
    // Keep production c capacities, with the other independent stores empty.
    manifest.K_e = manifest.B_e = manifest.U = manifest.B_U = manifest.V = manifest.B_V = 0;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    const auto layout = slab->layout();
    const auto address = slab->region(ScratchResource::consumed_index).base;
    Store store, oracle;
    oracle.fixture_use_memory_reference();
    IO io;
    Work work, peak;
    store.fixture_count_io(&io);
    store.fixture_count_fold_work(&work);
    store.set_directory("/dev/null/pineforge-id-pair-fold"); // A file open cannot succeed.
    store.bind_scratch_tables(slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    if (collide) store.fixture_force_hash_collisions(7);
    Reference expected;
    std::mt19937_64 random(0x15051abf01d);
    std::vector<std::string> keys{
        "", "a", "ab", std::string("a\0", 2), std::string("a\0b", 3),
        std::string("\0", 1), std::string("\x1f", 1),
        std::string("\x80", 1), std::string("\xff", 1)
    };
    for (std::uint64_t i = 0; keys.size() < capacity; ++i) keys.push_back(fixed_key(i));
    std::shuffle(keys.begin(), keys.end(), random);
    const auto observe = [&](Work before) {
        peak.row_comparisons = std::max(peak.row_comparisons, work.row_comparisons - before.row_comparisons);
        peak.byte_steps = std::max(peak.byte_steps, work.byte_steps - before.byte_steps);
        peak.compositions = std::max(peak.compositions, work.compositions - before.compositions);
    };
    const auto bound = [&] {
        require(peak.row_comparisons <= 13, "4096-key slab fold exceeds 13 row comparisons/write");
        require(peak.byte_steps <= byte_bound, "4096-key slab fold exceeds 257-row byte-step bound/write");
        require(peak.compositions <= 32, "4096-key slab fold exceeds 32 cached map compositions/write");
    };
    const auto put = [&](const std::string& key, std::int64_t value) {
        const auto before = work;
        const auto allocations = global_allocation::allocations;
        global_allocation::refusing = true;
        const bool inserted = store.insert_or_assign(key, value);
        global_allocation::refusing = false;
        require(global_allocation::allocations == allocations, "4096-key slab write allocates nothing");
        observe(before);
        require(inserted == expected.insert_or_assign(key, value).second
                    && inserted == oracle.insert_or_assign(key, value),
                "4096-key slab insertion/replacement/resurrection agrees with independent map");
        require(store.lookup(key) == std::optional<std::int64_t>{value}, "changed slab row is readable");
    };
    const auto erase = [&](const std::string& key) {
        const auto before = work;
        const auto allocations = global_allocation::allocations;
        global_allocation::refusing = true;
        const bool removed = store.erase(key);
        global_allocation::refusing = false;
        require(global_allocation::allocations == allocations, "4096-key slab erase allocates nothing");
        observe(before);
        require(removed == (expected.erase(key) != 0) && removed == oracle.erase(key),
                "4096-key slab erase agrees with independent map");
        require(!store.lookup(key), "erased slab row is absent");
    };
    const auto compare = [&](bool relookup_collision_table) {
        Reference observed;
        for (const auto& row : store)
            require(observed.emplace(row.first, row.second).second, "4096-key iteration is unique");
        require(observed == expected && store.size() == expected.size(),
                "4096-key population and values match independent map");
        // Reuse the old enumerate/sort/relookup loop on an independently
        // mutated memory fixture for each seed. This avoids repeating the
        // intentionally quadratic hash probing of the collision table for
        // every seed at every checkpoint. Also run it on that table at each
        // complete mutation phase, including its real lookup path.
        const std::array<std::uint64_t, 8> seeds{{
            0, 1, 255, 256, UINT64_C(0x8000000000000000), UINT64_MAX, random(), random()
        }};
        for (const auto seed : seeds) {
            const auto before = work;
            const auto allocations = global_allocation::allocations;
            Sink actual;
            actual.h = seed;
            global_allocation::refusing = true;
            store.fold_sorted(actual);
            global_allocation::refusing = false;
            require(global_allocation::allocations == allocations
                        && work.row_comparisons == before.row_comparisons
                        && work.byte_steps == before.byte_steps && work.compositions == before.compositions,
                    "4096-key hash only applies the resident map without allocation or fold work");
            require(actual.h == old_sorted_fold(oracle, seed),
                    "4096-key slab digest equals independent old sorted fold at every seed");
            if (relookup_collision_table && seed == seeds.back())
                require(actual.h == old_sorted_fold(store, seed),
                        "4096-key slab digest equals old sort/relookup on the actual collision table");
        }
        require(store.disk_bytes() == 0 && io.reads == 0 && io.writes == 0,
                "4096-key slab mutation and hashing perform no file IO");
        require(slab->region(ScratchResource::consumed_index).base == address
                    && slab->layout().slab_size == layout.slab_size,
                "4096-key slab retains the original fixed allocation");
    };
    for (const auto& key : keys) put(key, static_cast<std::int64_t>(random() >> 1));
    require(store.size() == capacity && expected.size() == capacity && oracle.size() == capacity,
            "work bound is exercised at 4096 distinct live binary keys");
    require(work.row_comparisons > 0 && work.byte_steps > 0, "work counters observe actual mutation work");
    bound();
    compare(true);

    // Check ALL bytes and extents, including the new index-resident maps.
    // Publish unused bytes for inspection before the refusal boundary.
    for (const auto& region : slab->regions()) slab->extend(region.resource, region.capacity);
    const SlabImage image(*slab);
    bool refused = false;
    try { store.insert_or_assign("beyond the admitted population", 17); }
    catch (const ScratchCapacityError& error) {
        refused = pineforge::classify_run_failure(error).code
            == pineforge::RunFailureCode::scratch_capacity_exceeded;
    }
    require(refused && SlabImage(*slab) == image && store.size() == capacity,
            "4096-key refusal preserves every slab byte, extent, fold and row");
    compare(false);

    std::shuffle(keys.begin(), keys.end(), random);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        put(keys[i], -static_cast<std::int64_t>(random() >> 1) - 1);
        bound();
        if ((i + 1) % 256 == 0) compare(i + 1 == keys.size());
    }
    std::shuffle(keys.begin(), keys.end(), random);
    std::vector<std::string> erased(keys.begin(), keys.begin() + capacity / 4);
    for (std::size_t i = 0; i < erased.size(); ++i) {
        erase(erased[i]);
        bound();
        if ((i + 1) % 256 == 0) compare(i + 1 == erased.size());
    }
    require(store.size() == 3072, "one quarter of the full table is really erased");
    std::shuffle(erased.begin(), erased.end(), random);
    for (std::size_t i = 0; i < erased.size(); ++i) {
        put(erased[i], std::numeric_limits<std::int64_t>::min() + static_cast<std::int64_t>(i));
        bound();
        if ((i + 1) % 256 == 0) compare(i + 1 == erased.size());
    }
    require(store.size() == capacity, "all 4096 ever-keys are live after random resurrection");
    for (int step = 0; step < 2048; ++step) {
        const auto& key = keys[static_cast<std::size_t>(random() % keys.size())];
        if (random() % 3 == 0) erase(key);
        else put(key, static_cast<std::int64_t>(random() >> 1));
        bound();
        if ((step + 1) % 256 == 0) compare(step + 1 == 2048);
    }
    Store moved(std::move(store));
    check(store, {}, random);
    store = std::move(moved);
    compare(false);
    global_allocation::refusing = true;
    store.clear();
    global_allocation::refusing = false;
    expected.clear();
    oracle.clear();
    compare(false);
    for (std::size_t i = 0; i < 33; ++i) {
        put(keys[i], static_cast<std::int64_t>(i));
        bound();
    }
    compare(false);
    std::printf("slab fold 4096 collisions=%d: max comparisons=%llu byte_steps=%llu compositions=%llu\n",
                collide ? 1 : 0, static_cast<unsigned long long>(peak.row_comparisons),
                static_cast<unsigned long long>(peak.byte_steps),
                static_cast<unsigned long long>(peak.compositions));
}

void slab_fold_layout_fits_every_index_tier() {
    using namespace pineforge::detail;
    // No allocation: exercise each power-of-two boundary, including the
    // eight-byte offset layout, without allocating a multi-GB slab.
    for (unsigned bit = 0; bit <= 56; ++bit) {
        const auto power = UINT64_C(1) << bit;
        for (const auto keys : {power - 1, power, power + 1}) {
            ScratchManifest manifest;
            manifest.K_c = keys;
            const auto layout = compute_layout(manifest);
            IdPairSlabFold fold;
            fold.configure(layout.regions[4].capacity, keys, scratch_layout_detail::slots(keys));
            require(fold.storage_bytes() <= layout.regions[5].capacity,
                    "all admitted key counts retain their old fixed index budget");
            const bool narrow = layout.regions[4].capacity <= UINT32_MAX;
            require(fold.offset_width() == (narrow ? 4 : 8)
                        && fold.bucket_capacity() == (narrow ? 256 : 1024),
                    "offset and bucket widths cover the complete admitted data span");
        }
    }
    // Exercise the actual wide slot codec with high bits set, using a small
    // backing region. Only the codec is under test here, not data admission.
    ScratchManifest manifest;
    manifest.K_c = 1;
    ScratchSlab slab(manifest);
    slab.extend(ScratchResource::consumed_index, slab.region(ScratchResource::consumed_index).capacity);
    IdPairSlabFold wide;
    wide.configure(UINT64_C(1) << 32, 1, 64);
    std::mt19937_64 random(0x15081de);
    for (std::uint64_t at = 0; at < 64; ++at) {
        const auto value = random() | (UINT64_C(1) << 63);
        wide.write_slot(slab, ScratchResource::consumed_index, at, value);
        require(wide.read_slot(slab, ScratchResource::consumed_index, at) == value,
                "wide slab offsets retain every high bit");
    }
}

void slab_data_refusal_preserves_fold_and_index() {
    using namespace pineforge::detail;
    ScratchManifest manifest;
    manifest.K_c = 2; // Data exhausts first, with one ever-key slot available.
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Store store;
    store.bind_scratch_tables(slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    const std::string key(40, 'L'); // 40-byte header + 40-byte key exactly fills D=80.
    store.insert_or_assign(key, -19);
    for (const auto& region : slab->regions()) slab->extend(region.resource, region.capacity);
    const SlabImage before(*slab);
    bool refused = false;
    try { store.insert_or_assign("", 1); }
    catch (const ScratchCapacityError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        refused = failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
            && failure.args && *failure.args == "{\"resource\":\"consumed_data\"}";
    }
    require(refused && SlabImage(*slab) == before, "data refusal preserves all fold/index bytes and extents");
    std::mt19937_64 random(0x150da7a);
    check(store, {{key, -19}}, random, true);
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
                                == pineforge::RunFailureCode::scratch_io_failed,
                        "injected short write surfaces EIO and scratch_io_failed");
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
        bounded_slab_keeps_the_fold_without_files();
        slab_work_counters_observe_the_old_byte_loop();
        slab_work_bound_and_oracle_4096(false);
        slab_work_bound_and_oracle_4096(true);
        slab_fold_layout_fits_every_index_tier();
        slab_data_refusal_preserves_fold_and_index();
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
