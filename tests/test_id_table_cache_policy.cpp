// Frozen-header compile must fail: IdTableReadCache has no Policy type.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "id_table_cache_workload.hpp"

#include <algorithm>
#include <csignal>
#include <limits>
#include <sys/mman.h>

namespace {
using namespace id_table_cache_test;
using Cache = IdTableReadCache;
using Policy = Cache::Policy;

struct GeometryCapacity {
    Policy tree, pair;
    template<class Table> Table make(bool paired) const { return Table(paired ? pair : tree); }
};
struct Path {
    ScratchFile file{ScratchFile::IoRaiser::auxiliary};
    Cache cache;
    std::uint64_t end = 0;
    explicit Path(Policy policy) : cache(policy) {
        file.set_directory(".");
        file.open("pineforge-ids");
    }
    void read(std::uint64_t at, void* bytes, std::size_t length, bool disk = true,
              unsigned tag = 0) const {
        cache.read(file, disk, end, tag, at, bytes, length);
    }
    void write(std::uint64_t at, const void* bytes, std::size_t length) {
        cache.before_write(0, end, at, length);
        file.write(at, bytes, length);
        end = std::max(end, at + length);
    }
};
std::vector<unsigned char> input_bytes(std::size_t size) {
    std::vector<unsigned char> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<unsigned char>((i * 31 + i / 13) % 251);
    return bytes;
}
void geometry_and_writes(Policy policy) {
    Path path(policy);
    auto expected = input_bytes((policy.lines + 2) * policy.line_bytes + 7);
    path.write(0, expected.data(), expected.size());
    unsigned char byte = 0;
    {
        sc412::Measure cold;
        path.read(0, &byte, 1);
        const auto io = cold.finish()[sc412::ids];
        require(byte == expected[0] && io.reads == 1 && io.read_requests == policy.line_bytes,
                "configured line size determines the physical cold fill");
    }
    {
        sc412::Measure hot;
        path.read(0, &byte, 1);
        require(hot.finish()[sc412::ids].reads == 0, "configured cache actually hits");
    }
    // Direct mapping: page == lines evicts page zero, independent of key values.
    path.read(policy.lines * policy.line_bytes, &byte, 1);
    {
        sc412::Measure evicted;
        path.read(0, &byte, 1);
        const auto io = evicted.finish()[sc412::ids];
        require(byte == expected[0] && io.reads == 1 && io.read_requests == policy.line_bytes,
                "a colliding valid page is read from backing after eviction");
    }
    const std::array<unsigned char, 11> replacement{{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 17}};
    const auto at = policy.line_bytes - 3;
    path.write(at, replacement.data(), replacement.size());
    std::copy(replacement.begin(), replacement.end(), expected.begin() + at);
    require(sc412::image(path.file.descriptor()) == expected,
            "cross-line write reaches backing before any subsequent read");
    std::vector<unsigned char> observed(expected.size());
    path.read(0, observed.data(), observed.size());
    require(observed == expected, "overlapping cached lines are invalidated before writes");
    const auto old_end = expected.size();
    const auto extra = input_bytes(policy.line_bytes + 3);
    path.write(old_end, extra.data(), extra.size());
    expected.insert(expected.end(), extra.begin(), extra.end());
    observed.resize(expected.size());
    path.read(0, observed.data(), observed.size());
    require(observed == expected, "append refills the old clipped tail for every geometry");
    path.cache.invalidate_from(0, 0);
    require(::ftruncate(path.file.descriptor(), policy.line_bytes / 2) == 0,
            "inject a failed partial line fill");
    bool failed = false;
    try { path.read(0, &byte, 1); }
    catch (const AuxiliaryScratchIoError& error) { failed = error.code().value() == EIO; }
    require(failed, "real short backing read remains EIO");
    expected[0] ^= 0xff;
    path.file.write(0, expected.data(), expected.size());
    path.read(0, &byte, 1);
    require(byte == expected[0], "failed fill never publishes a stale successful tag");
    // Moving an already warmed cache must discard the shared cache's tags.
    expected[0] ^= 0x71;
    path.file.write(0, expected.data(), 1);
    Cache moved(std::move(path.cache));
    {
        sc412::Measure reads;
        moved.read(path.file, true, path.end, 0, 0, &byte, 1);
        require(byte == expected[0] && reads.finish()[sc412::ids].reads == 1,
                "move invalidates all shared cache tags");
    }
    moved.set_capacity(Policy{policy.line_bytes, 0});
    {
        sc412::Measure reads;
        moved.read(path.file, true, path.end, 0, 0, &byte, 1);
        const auto io = reads.finish()[sc412::ids];
        require(byte == expected[0] && io.read_requests == 1,
                "changing to zero lines uses the exact direct read");
    }
}
void paired_file_tags(std::size_t lines) {
    Path data({257, lines}), index({257, 0});
    auto a = input_bytes(2 * 257 + 19), b = a;
    for (auto& byte : b) byte ^= 0xff;
    data.write(0, a.data(), a.size()); index.write(0, b.data(), b.size());
    unsigned char byte = 0;
    for (int repeat = 0; repeat < 3; ++repeat) {
        data.cache.read(data.file, true, data.end, 0, 11, &byte, 1);
        require(byte == a[11], "data page has its own file tag");
        data.cache.read(index.file, true, index.end, 1, 11, &byte, 1);
        require(byte == b[11], "paired index page cannot alias the data page");
    }
    // file*7 maps the same page of both files to one slot at 1 and 7 lines.
    {
        sc412::Measure reads;
        data.cache.read(data.file, true, data.end, 0, 11, &byte, 1);
        const auto io = reads.finish()[sc412::ids];
        require(io.reads == 1 && byte == a[11], "paired collision really refills the evicted file");
    }
    data.cache.read(index.file, true, index.end, 1, 11, &byte, 1);
    b[11] ^= 0x39;
    data.cache.before_write(1, index.end, 11, 1);
    index.write(11, &b[11], 1);
    data.cache.read(index.file, true, index.end, 1, 11, &byte, 1);
    require(byte == b[11], "paired index write invalidates its translated shared key");

    // Truncate and regrow within the same partial page: a stale longer prefix
    // could satisfy the new shorter read unless the file-one tail is dropped.
    data.cache.read(data.file, true, data.end, 0, 11, &byte, 1);
    data.cache.read(index.file, true, index.end, 1, index.end - 1, &byte, 1);
    const auto cut = index.end - 2;
    data.cache.invalidate_from(1, cut);
    require(::ftruncate(index.file.descriptor(), cut) == 0, "truncate paired index tail");
    index.end = cut;
    const unsigned char replacement = static_cast<unsigned char>(b[cut] ^ 0xa7);
    data.cache.before_write(1, index.end, cut, 1);
    index.write(cut, &replacement, 1);
    {
        sc412::Measure reads;
        data.cache.read(index.file, true, index.end, 1, cut, &byte, 1);
        require(byte == replacement && reads.finish()[sc412::ids].reads == 1,
                "paired index tail notification physically refills after truncate and regrowth");
    }
    if (lines == 7) {
        sc412::Measure reads;
        data.cache.read(data.file, true, data.end, 0, 11, &byte, 1);
        require(byte == a[11] && reads.finish()[sc412::ids].reads == 0,
                "index tail invalidation preserves an unrelated resident data page");
    }
}
void allocation_bypass(Policy policy, std::size_t allowed_allocations) {
    Path path(policy);
    const auto expected = input_bytes(3 * policy.line_bytes + 37);
    path.write(0, expected.data(), expected.size());
    std::array<unsigned char, 23> actual{};
    const auto before_refusals = global_allocation::refused;
    const auto before_bytes = global_allocation::live_bytes;
    sc412::Measure reads;
    global_allocation::recording = true;
    global_allocation::fail_after = allowed_allocations;
    path.read(7, actual.data(), actual.size());
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    const auto after_bytes = global_allocation::live_bytes;
    global_allocation::recording = false;
    const auto io = reads.finish()[sc412::ids];
    require(global_allocation::refused == before_refusals + 1
            && after_bytes == before_bytes && io.reads == 1
            && io.read_requests == actual.size() && io.read_bytes == actual.size()
            && std::equal(actual.begin(), actual.end(), expected.begin() + 7),
            "optional cache/workspace allocation failure releases storage and reads directly");
    path.read(7, actual.data(), actual.size());
    sc412::Measure hot;
    path.read(7, actual.data(), actual.size());
    require(hot.finish()[sc412::ids].reads == 0, "cache can engage after allocation bypass");
}
void bypass_ignores_tags() {
    Path path({257, 32});
    unsigned char expected = 19, actual = 0;
    path.write(0, &expected, 1);
    path.read(0, &actual, 1); // Warm a deliberately different disposable copy.
    expected = 71;
    path.file.write(0, &expected, 1);
    const auto allocations = global_allocation::allocations;
    sc412::Measure reads;
    path.read(0, &actual, 1, false);
    require(actual == expected && reads.finish()[sc412::ids].read_requests == 1
            && global_allocation::allocations == allocations,
            "arena/slab bypass neither probes existing tags nor allocates a cache");
}
void backend_reads(Tree& tree, Minima& minima, Cycles& cycles) {
    const auto at = tree.insert("id", 17).first;
    minima.insert_or_assign("id", "from", 23);
    cycles.insert_or_assign("exit\x1f" "id", -31);
    const auto allocations = global_allocation::allocations;
    for (int n = 0; n < 32; ++n)
        require(tree.get(at) == 17 && minima.lookup("id", "from") == 23
                && cycles.lookup("exit\x1f" "id") == -31,
                "slab/arena key read returns all original bytes");
    require(global_allocation::allocations == allocations,
            "slab/arena reads allocate neither shared cache nor transfer storage");
}
void slab_bypass() {
    ScratchManifest manifest;
    manifest.U = manifest.K_e = manifest.K_c = 4;
    manifest.B_U = manifest.B_e = manifest.B_c = 64;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Tree tree(Policy{257, 32});
    Minima minima(Policy{257, 32});
    Cycles cycles(Policy{257, 32});
    tree.bind_scratch(slab, ScratchResource::cohort);
    minima.bind_scratch_tables(slab, ScratchResource::erased_data, ScratchResource::erased_index);
    cycles.bind_scratch_tables(slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    backend_reads(tree, minima, cycles);
    require(tree.bounded_cohorts() && descriptor(tree) == -1, "bounded slab remains its own backend");
}
struct Arena {
    int fd = -1;
    struct sigaction previous{};
    explicit Arena(bool sealed) {
        require(ScratchArena::attached() == nullptr, "start with no attached arena");
        require(::sigaction(SIGXFSZ, nullptr, &previous) == 0, "save signal disposition");
        constexpr auto bytes = 8 * ScratchArena::kChunkBytes;
        if (sealed) {
            fd = ::memfd_create("id-read-cache", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        } else {
            char path[] = "./id-read-cache-arena-XXXXXX";
            fd = ::mkstemp(path);
            require(fd >= 0 && ::unlink(path) == 0, "create regular file arena");
        }
        require(fd >= 0 && ::ftruncate(fd, bytes) == 0, "size arena backing");
        if (sealed)
            require(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                    "seal inherited memory backing");
        ScratchArena::attach(fd, bytes);
    }
    ~Arena() {
        ScratchArena::detach();
        ::close(fd);
        ::sigaction(SIGXFSZ, &previous, nullptr);
    }
};
void arena_bypass(bool sealed) {
    Arena arena(sealed);
    Tree tree(Policy{257, 32});
    Minima minima(Policy{257, 32});
    Cycles cycles(Policy{257, 32});
    backend_reads(tree, minima, cycles);
    require(tree.arena_backed() && descriptor(tree) == -1,
            "regular/sealed arena bypass is selected from the actual backend");
}
void capacity_sweep() {
    const auto direct = workload(ByteCapacity{0, 0});
    identical(direct, workload(GeometryCapacity{{4096, 0}, {4096, 0}}));
    identical(direct, workload(GeometryCapacity{{4096, 1}, {4096, 1}}));
    identical(direct, workload(GeometryCapacity{{4096, 16}, {4096, 8}}));
    identical(direct, workload(GeometryCapacity{{512, 2}, {512, 2}}));
    identical(direct, workload(GeometryCapacity{{17, 3}, {17, 3}}));
    identical(direct, workload(GeometryCapacity{{4096, 32}, {4096, 32}}));
}

std::string ordered_key(std::uint64_t value) {
    // The native book uses byte-ordered numeric keys. Keep embedded NULs and
    // exercise the whole key, rather than relying on a decimal id prefix.
    std::string key(8, '\0');
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<char>(value >> (56 - 8 * i));
    return key;
}

void ordered_insert_growth(bool descending, bool textual = false) {
    sc412::reset_files();
    Tree tree; // Exercise the native book's actual default, uncached backend.
    tree.set_directory(".");
    constexpr std::array<std::size_t, 3> counts{{512, 2048, 8192}};
    std::array<sc412::Traffic, counts.size()> traffic{};
    std::map<std::string, std::uint64_t> expected;
    std::vector<std::string> insertion_order;
    std::size_t inserted = 0;
    const auto make_key = [&](std::uint64_t ordinal) {
        return textual ? std::string("label\0/", 7) + std::to_string(ordinal)
                       : ordered_key(ordinal);
    };
    for (std::size_t leg = 0; leg < counts.size(); ++leg) {
        sc412::Measure measure;
        while (inserted < counts[leg]) {
            const auto ordinal = descending ? counts.back() - inserted : inserted;
            const auto key = make_key(ordinal);
            const auto value = 7 * ordinal + 11;
            // Admission can ask several independent read-only questions
            // before the same key is inserted. Decimal carries cross old
            // prefix keys, so the latest insertion's empty gap alone cannot
            // keep these searches local as the tree population grows.
            if (textual)
                for (int check = 0; check < 16; ++check)
                    require(!tree.find(key), "prospective textual key is absent before insertion");
            require(tree.insert(key, value).second, "ordered append inserts every distinct key");
            expected.emplace(key, value);
            insertion_order.push_back(key);
            ++inserted;
        }
        traffic[leg] = measure.finish()[sc412::ids];
        const auto rows = counts[leg] - (leg ? counts[leg - 1] : 0);
        std::printf("ID_TABLE_APPEND descending=%d N=%zu rows=%zu reads=%llu "
                    "reads_per_row=%.3f read_bytes=%llu writes=%llu textual=%d\n",
                    descending, counts[leg], rows,
                    static_cast<unsigned long long>(traffic[leg].reads),
                    static_cast<double>(traffic[leg].reads) / rows,
                    static_cast<unsigned long long>(traffic[leg].read_bytes),
                    static_cast<unsigned long long>(traffic[leg].writes), textual);
        require(traffic[leg].reads > rows && traffic[leg].writes > rows
                && traffic[leg].read_bytes == traffic[leg].read_requests
                && traffic[leg].past_end == 0,
                "append observer sees actual, complete backing reads and writes");
    }

    // Independent map and insertion transcript cover every row, not just the
    // newest key or a sampled count. Duplicate/update paths must also survive.
    auto sorted = expected.begin();
    tree.for_each_sorted([&](const auto& key, auto value) {
        require(sorted != expected.end() && sorted->first == key && sorted->second == value,
                "ordered append preserves the complete sorted key/value witness");
        ++sorted;
    });
    require(sorted == expected.end(), "ordered append sorted witness is complete");
    std::size_t ordinal = 0;
    tree.for_each_inserted([&](const auto& key, auto value) {
        require(ordinal < insertion_order.size() && key == insertion_order[ordinal++]
                && value == expected.at(key), "ordered append preserves first-insertion order");
    });
    require(ordinal == counts.back(), "ordered append insertion witness is complete");
    for (const auto& key : {insertion_order.front(), insertion_order[counts[0]],
                            insertion_order.back()}) {
        const auto at = tree.find(key);
        require(at && !tree.insert(key, 0).second && tree.get(*at) == expected.at(key),
                "duplicate ordered key preserves its original value");
        tree.set(*at, 99);
        require(tree.get(*at) == 99, "ordered key remains updateable");
    }
    std::fflush(stdout);
    // Compare equal units of real insertion work at increasing populations.
    // As in the standing-order cohort budget, allow 4/3 overhead, never time.
    // Root-to-leaf work repeated for every append violates this growth check.
    for (std::size_t leg = 1; leg < counts.size(); ++leg) {
        const auto rows = counts[leg] - counts[leg - 1];
        require(static_cast<long double>(traffic[leg].reads) * counts[0] * 3
                    <= static_cast<long double>(traffic[0].reads) * rows * 4,
                "ordered append backing reads per row grow with table population");
    }
}

struct LiveFacts {
    using Record = std::uint64_t;
    std::uint64_t value = 0;
    bool live = true;
    Record retired_record() const noexcept { return value; }
    static LiveFacts restore_record(Record record) { return {record, false}; }
    bool spillable() const noexcept { return !live; }
};
using LiveCohorts = DiskResidentIdTable<LiveFacts>;

void resident_lookup_growth() {
    // Pending origins retain their facts. The existing resident key directory
    // is sufficient even when reads jump far from the last inserted disk key.
    // Zero cache lines ensure a disk search cannot masquerade as a RAM lookup.
    sc412::reset_files();
    LiveCohorts table;
    table.set_directory(".");
    constexpr std::array<std::size_t, 3> counts{{512, 2048, 8192}};
    std::size_t inserted = 0;
    for (const auto count : counts) {
        while (inserted < count) {
            const auto placed = table.emplace(ordered_key(inserted), LiveFacts{inserted + 17});
            require(placed.second, "every live cohort has a distinct key");
            // Deliberately dirty the resident value without changing backing.
            placed.first->second.value += 100003;
            ++inserted;
        }
        const auto probes = table.work_counters().find_resident_probes;
        const auto allocations = global_allocation::allocations;
        sc412::Measure measure;
        const auto& read_only = table;
        for (std::size_t i = 0; i < count; ++i) {
            // Odd multiplier permutes every power-of-two population.
            const auto n = (i * 197) % count;
            const auto key = ordered_key(n);
            const auto found = table.find(key);
            const auto constant = read_only.find(key);
            require(found != table.end() && constant != read_only.end()
                    && read_only.contains(key)
                    && found->second.value == n + 100020
                    && constant->second.value == n + 100020,
                    "const/nonconst membership returns every dirty resident value");
        }
        const auto io = measure.finish()[sc412::ids];
        std::printf("ID_TABLE_RESIDENT N=%zu reads=%llu bytes=%llu probes=%llu\n",
                    count, static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.read_bytes),
                    static_cast<unsigned long long>(
                        table.work_counters().find_resident_probes - probes));
        std::fflush(stdout);
        require(io.reads == 0 && io.writes == 0
                && global_allocation::allocations == allocations,
                "resident membership must not search backing as population grows");
        require(table.work_counters().find_resident_probes > probes,
                "resident lookup observes actual full-key directory comparisons");
    }

    const auto prefix = ordered_key(197);
    require(!table.contains(prefix + '\0') && table.find(prefix + '\0') == table.end(),
            "a proper binary-key extension cannot borrow a resident prefix match");
    LiveCohorts moved(std::move(table));
    require(table.size() == 0 && moved.size() == counts.back(),
            "move transfers the complete resident directory");
    require(table.emplace("reused-source", LiveFacts{31}).second
            && !table.contains(prefix), "moved-from directory is independent");
    for (auto& row : moved) row.second.live = false;
    moved.spill();
    require(moved.resident_size() == 0, "retired controls really leave residency");
    {
        sc412::Measure measure;
        for (std::size_t n = 0; n < counts.back(); ++n) {
            const auto key = ordered_key(n);
            const auto found = std::as_const(moved).find(key);
            require(found != std::as_const(moved).end()
                    && found->second.value == n + 100020 && !found->second.live,
                    "every spilled dirty value restores from its original offset");
        }
        const auto io = measure.finish()[sc412::ids];
        require(io.reads > counts.back() && io.past_end == 0
                && io.read_requests == io.read_bytes,
                "retired control still performs complete physical backing reads");
    }
    moved.spill();
    // File zero belongs to the original table; move keeps that file identity.
    require(sc412::file_count >= 1, "observer captured the actual cohort index");
    require(::ftruncate(sc412::files[0].fd, 0) == 0, "inject retired index EOF");
    bool failed = false;
    {
        sc412::Measure measure;
        try { (void)moved.find(prefix); }
        catch (const AuxiliaryScratchIoError& error) {
            failed = error.code().value() == EIO
                && pineforge::classify_run_failure(error).code
                    == pineforge::RunFailureCode::scratch_io_failed;
        }
        require(failed && measure.finish()[sc412::ids].reads > 0,
                "a retired miss cannot hide backing EIO as resident success or absence");
    }
    moved.clear();
    require(!moved.contains(prefix) && moved.emplace(prefix, LiveFacts{73}).second
            && moved.find(prefix)->second.value == 73,
            "clear discards the old borrowed directory before reusing its keys");
}

void mixed_insertion_controls() {
    std::vector<unsigned char> direct_image;
    for (const auto policy : {Policy{4096, 0}, Policy{17, 3}, Policy{4096, 16}}) {
        Tree tree(policy);
        tree.set_directory(".");
        std::map<std::string, std::uint64_t> expected;
        std::vector<std::string> insertion_order;
        const std::string prefix = std::string(257, 'p') + std::string("\0gap/", 5);
        const auto insert = [&](const std::string& key) {
            const auto reference = expected.emplace(key, 7 * expected.size() + 11);
            const auto placed = tree.insert(key, 7 * insertion_order.size() + 11);
            require(placed.second == reference.second
                    && tree.get(placed.first) == reference.first->second,
                    "mixed insertion and duplicates match the independent map");
            if (reference.second) insertion_order.push_back(key);
            // A proper extension can fall between existing keys. Membership
            // must use every byte and cannot infer it from a nearby row.
            const auto extension = key + '\0';
            const auto wanted = expected.find(extension);
            const auto found = tree.find(extension);
            require((wanted == expected.end()) == !found.has_value(),
                    "mixed binary-key membership matches the independent map");
            if (found) require(tree.get(*found) == wanted->second, "extension value is preserved");
        };
        const auto verify = [&] {
            auto next = expected.begin();
            tree.for_each_sorted([&](const auto& key, auto value) {
                require(next != expected.end() && key == next->first && value == next->second,
                        "mixed insertion preserves every sorted key and value");
                ++next;
            });
            require(next == expected.end() && tree.size() == expected.size(),
                    "mixed sorted witness covers the complete population");
            std::size_t at = 0;
            tree.for_each_inserted([&](const auto& key, auto value) {
                require(at < insertion_order.size() && key == insertion_order[at++]
                        && value == expected.at(key), "mixed insertion order is complete");
            });
            require(at == insertion_order.size(), "mixed insertion walk has no missing rows");
            for (const auto& row : expected) {
                const auto found = tree.find(row.first);
                require(found && tree.get(*found) == row.second, "every mixed key remains findable");
            }
        };

        // Seed both sides, then grow inside occupied intervals in each
        // direction. Unrelated jumps and prefix keys force general searches;
        // duplicates must preserve the original value and insertion order.
        for (const auto n : {0, 8192, 4096, 2048, 6144, 1024, 7168})
            insert(prefix + ordered_key(n));
        for (std::uint64_t n = 1; n <= 64; ++n) insert(prefix + ordered_key(2048 + n));
        verify();
        Tree moved(std::move(tree));
        require(tree.size() == 0, "move construction empties the old mixed table");
        require(tree.insert("moved-from", 19).second, "moved-from table can be reused independently");
        tree = std::move(moved); // Replace an already populated destination.
        require(moved.size() == 0, "move assignment empties the old mixed table");
        for (std::uint64_t n = 1; n <= 64; ++n) insert(prefix + ordered_key(6144 - n));
        for (std::uint64_t n = 0; n < 257; ++n)
            insert(prefix + ordered_key(128 + ((n * 197) % 257) * 24));
        for (const auto& key : {std::string{}, std::string("\0", 1), prefix,
                                prefix + '\0', std::string(513, '\xff')})
            insert(key);
        for (const auto& key : {insertion_order.front(), insertion_order[3], insertion_order.back()})
            insert(key);
        verify();
        const auto image = sc412::image(descriptor(tree));
        if (direct_image.empty()) direct_image = image;
        else require(image == direct_image, "mixed table bytes do not depend on cache geometry");
        std::printf("ID_TABLE_MIXED lines=%zu rows=%zu\n", policy.lines, expected.size());

        tree.clear();
        expected.clear();
        insertion_order.clear();
        for (std::uint64_t n = 0; n < 32; ++n)
            insert(ordered_key(n % 2 ? 64 - n : n));
        verify();
    }
}

void malformed_policy() {
    for (const auto policy : {Policy{0, 1},
                              Policy{2, std::numeric_limits<std::size_t>::max()},
                              Policy{1, std::numeric_limits<std::size_t>::max()}}) {
        bool rejected = false;
        try { Cache cache(policy); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "only malformed/unrepresentable cache geometry is rejected");
    }
}
} // namespace

int main() {
    try {
        for (const auto policy : {Policy{17, 1}, Policy{512, 3}, Policy{4096, 16},
                                  Policy{8192, 32}})
            geometry_and_writes(policy);
        paired_file_tags(1); paired_file_tags(7);
        allocation_bypass({512, 1}, 0); // Optional shared block fails.
        allocation_bypass({512, 32}, 0); // Larger shared block fails.
        allocation_bypass({8192, 32}, 1); // Shared block succeeds; wide workspace fails.
        bypass_ignores_tags();
        slab_bypass(); arena_bypass(false); arena_bypass(true);
        capacity_sweep();
        malformed_policy();
        ordered_insert_growth(false);
        ordered_insert_growth(true);
        ordered_insert_growth(false, true);
        ordered_insert_growth(true, true);
        resident_lookup_growth();
        mixed_insertion_controls();
        std::puts("id table cache geometry, bypass, and complete-output sweep: OK");
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
