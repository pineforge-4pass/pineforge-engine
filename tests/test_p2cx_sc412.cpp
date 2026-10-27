// Tests written before the product cache. Frozen-base compatibility below
// executes actual uncached I/O; reduction/budget assertions remain live.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "p2cx_sc412_io.hpp"

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/id_pair_store.hpp>

#include <algorithm>
#include <csignal>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sys/mman.h>
#include <sys/resource.h>
#include <utility>

#if __has_include(<pineforge/detail/id_table_read_cache.hpp>)
#include <pineforge/detail/id_table_read_cache.hpp>
using ReadPath = pineforge::detail::IdTableReadCache;
#else
// Only the frozen base lacks the helper. All guards still reach ScratchFile
// and its real syscalls/errors. This is not a substitute cache.
struct ReadPath {
    explicit ReadPath(std::size_t) {}
    void invalidate() const noexcept {}
    void release() noexcept {}
    void before_write(unsigned, std::uint64_t, std::uint64_t, std::size_t) const noexcept {}
    template<class File> void read(const File& file, bool, std::uint64_t, unsigned,
            std::uint64_t offset, void* data, std::size_t length) const {
        file.read(offset, data, length);
    }
};
#endif

namespace {
using namespace sc412;
using namespace pineforge::detail;
constexpr std::size_t budget = 64 * 1024, page = 4096;

struct Row {
    struct Record { std::uint64_t value = 0; };
    std::uint64_t value = 0;
    Record retired_record() const { return {value}; }
    static Row restore_record(Record r) { return {r.value}; }
    bool spillable() const { return true; }
};
struct Result {
    std::uint64_t lookups = basis, image_hash = basis;
    std::array<Traffic, families> io{};
    std::vector<std::vector<unsigned char>> disk;
};
// BASE GOLDENS ONLY: supervisor must fill from frozen 6b33c47fda4e,
// lab rj-20261027t180028-c7f037 (or a fresh frozen-base build). Never use
// observations from the changed product. Empty values deliberately fail.
namespace base_goldens {
struct Table { std::uint64_t lookups, image_hash; };
inline constexpr std::optional<Table> tree = Table{0x8b7b92445630d923ULL, 0x5f9518b3d89a682cULL};
inline constexpr std::optional<Table> resident = Table{0xd00155b9ab1a8583ULL, 0xee32b160e9d134ceULL};
inline constexpr std::optional<Table> pair = Table{0x2a6f8c34c7514e03ULL, 0x7913519b58888fe5ULL};
inline constexpr std::optional<Table> consumed = Table{0xeb90d13fa4587903ULL, 0xadd26779183235b4ULL};
}
template<class Populate, class Lookup>
Result observe(Populate populate, Lookup lookup) {
    reset_files();
    Measure measurement;
    auto table = populate();
    Result result;
    for (int n = 0; n < 2000; ++n) {
        const auto value = lookup(table, n);
        result.lookups = hash(result.lookups, &value, sizeof value);
    }
    result.io = measurement.finish();
    result.disk = images();
    for (const auto& bytes : result.disk) {
        const auto length = bytes.size();
        result.image_hash = hash(result.image_hash, &length, sizeof length);
        result.image_hash = hash(result.image_hash, bytes.data(), bytes.size());
    }
    return result;
}
void golden_and_fewer(const char* name, const Result& cached, const Result& direct,
                      Family family, const std::optional<base_goldens::Table>& golden) {
    const auto& a = cached.io[family];
    const auto& b = direct.io[family];
    std::printf("sc412 %s family=%zu preads=%llu/%llu requested=%llu/%llu returned=%llu/%llu "
                "pwrites=%llu/%llu written=%llu/%llu (cache/direct)\n",
                name, static_cast<std::size_t>(family),
                static_cast<unsigned long long>(a.reads), static_cast<unsigned long long>(b.reads),
                static_cast<unsigned long long>(a.read_requests), static_cast<unsigned long long>(b.read_requests),
                static_cast<unsigned long long>(a.read_bytes), static_cast<unsigned long long>(b.read_bytes),
                static_cast<unsigned long long>(a.writes), static_cast<unsigned long long>(b.writes),
                static_cast<unsigned long long>(a.write_bytes), static_cast<unsigned long long>(b.write_bytes));
    require(direct.io[family].reads > 0, "uncached workload really preads");
    require(cached.io[family].reads < direct.io[family].reads, "strictly fewer real pread calls");
    require(cached.io[family].past_end == 0 && direct.io[family].past_end == 0,
            "no valid lookup or line fill requests bytes beyond file end");
    require(cached.io[family].writes == direct.io[family].writes
            && cached.io[family].write_bytes == direct.io[family].write_bytes,
            "read cache leaves actual writes unchanged");
    require(golden.has_value(), "BASE GOLDENS MISSING: supervisor must pin frozen-base hashes");
    require(cached.lookups == golden->lookups && !cached.disk.empty()
            && cached.image_hash == golden->image_hash, "lookup and physical-image hashes match BASE GOLDENS");
}
void repeated_lookups() {
    const auto tree = [](std::size_t bytes) {
        return observe([&] {
            auto table = table_with_cache<DiskIdTable<std::uint64_t>>(bytes);
            table.set_directory(".");
            for (int n = 0; n < 32; ++n) table.insert("id-" + std::to_string(n), n * 17);
            return table;
        }, [](auto& table, int n) {
            const auto at = table.find("id-" + std::to_string(n % 32));
            require(at.has_value(), "tree key present");
            return table.get(*at);
        });
    };
    const auto resident = [](std::size_t bytes) {
        return observe([&] {
            auto table = table_with_cache<DiskResidentIdTable<Row>>(bytes);
            table.set_directory(".");
            for (int n = 0; n < 24; ++n) table.emplace("r-" + std::to_string(n), Row{std::uint64_t(n)});
            table.spill();
            return table;
        }, [](auto& table, int n) {
            const auto at = table.find("r-" + std::to_string(n % 24));
            require(at != table.end(), "retired row restored");
            return at->second.value;
        });
    };
    const auto pair = [](std::size_t bytes) {
        return observe([&] {
            auto table = table_with_cache<IdPairStore<std::uint64_t>>(bytes);
            table.set_directory(".");
            table.fixture_force_hash_collisions(7); // Real full-key collision comparisons.
            for (int n = 0; n < 20; ++n) {
                const auto key = "p-" + std::to_string(n);
                table.insert_or_assign(key, n);
                table.insert_or_assign(key, std::string_view("from\0id", 7), n + 100);
            }
            return table;
        }, [](auto& table, int n) {
            const auto key = "p-" + std::to_string(n % 20);
            const auto a = table.lookup(key);
            const auto b = table.lookup(key, std::string_view("from\0id", 7));
            require(a && b && *a + 100 == *b && !table.lookup(key, ""), "both key domains");
            return *a ^ (*b << 32);
        });
    };
    const auto consumed = [](std::size_t bytes) {
        return observe([&] {
            auto table = table_with_cache<IdPairStore<std::int64_t, true>>(bytes);
            table.set_directory(".");
            for (int n = 0; n < 12; ++n) table.insert_or_assign("cycle-" + std::to_string(n), n);
            return table;
        }, [](auto& table, int n) {
            const auto value = table.lookup("cycle-" + std::to_string(n % 12));
            require(value.has_value(), "consumed-cycle key present");
            pineforge::BrokerStateHashSink sink;
            table.fold_sorted(sink);
            return sink.h ^ static_cast<std::uint64_t>(*value);
        });
    };
    const std::array<Result, 4> cached{
        tree(budget), resident(budget), pair(budget / 2), consumed(budget / 2)};
    const std::array<Result, 4> direct{tree(0), resident(0), pair(0), consumed(0)};
    const std::array<const char*, 4> names{"tree", "resident", "pair", "consumed"};
    const std::array<std::optional<base_goldens::Table>, 4> goldens{
        base_goldens::tree, base_goldens::resident, base_goldens::pair, base_goldens::consumed};
    // Emit every observation even when frozen-base engagement is RED.
    // These diagnostics are never read back as expected values by a test.
    for (std::size_t i = 0; i < cached.size(); ++i)
        std::printf("sc412 observed %s lookups=%016llx image=%016llx\n", names[i],
                    static_cast<unsigned long long>(cached[i].lookups),
                    static_cast<unsigned long long>(cached[i].image_hash));
    for (std::size_t i = 0; i < cached.size(); ++i)
        golden_and_fewer(names[i], cached[i], direct[i], i < 2 ? ids : pairs, goldens[i]);
}

// Isolate the exact production read helper over a real ScratchFile so reads
// at/across/beyond the logical end exercise the existing errno/text surface.
struct FilePath {
    ScratchFile file{ScratchFile::IoRaiser::auxiliary};
    ReadPath cache;
    std::uint64_t end = 0;
    explicit FilePath(std::size_t bytes) : cache(bytes) { file.set_directory("."); file.open("pineforge-ids"); }
    void read(std::uint64_t offset, void* data, std::size_t size) const {
        cache.read(file, file.descriptor() >= 0, end, 0, offset, data, size);
    }
    void write(std::uint64_t offset, const void* data, std::size_t size) {
        cache.before_write(0, end, offset, size);
        file.write(offset, data, size);
        if (size) end = std::max(end, offset + size);
    }
    void resize(std::uint64_t size) {
        cache.invalidate();
        file.truncate(size);
        end = size;
    }
};
void require_cache_hit(FilePath& path, std::uint64_t offset) {
    path.cache.invalidate();
    unsigned char first = 0, second = 0;
    Measure cold;
    path.read(offset, &first, 1);
    const auto a = cold.finish()[ids];
    Measure hot;
    path.read(offset, &second, 1);
    const auto b = hot.finish()[ids];
    require(first == second && a.reads > 0 && b.reads < a.reads,
            "cache engaged: repeated read makes strictly fewer real calls");
}
void require_disk_cache() {
    FilePath path(budget);
    const unsigned char byte = 0x39;
    path.write(0, &byte, 1);
    require_cache_hit(path, 0);
}
void append_reads() {
    FilePath path(budget);
    std::array<unsigned char, 3 * page + 37> initial{};
    initial.fill(0x3b);
    path.write(0, initial.data(), initial.size());
    require_cache_hit(path, 0);
    unsigned char byte = 0;
    for (const auto at : {std::size_t(17), page + 29, 2 * page + 41, initial.size() - 1})
        path.read(at, &byte, 1);
    std::vector<unsigned char> expected(initial.begin(), initial.end());
    std::array<unsigned char, 73> added{};
    std::uint64_t earlier_bytes = 0, tail_bytes = 0;
    for (int n = 0; n < 128; ++n) {
        added.fill(static_cast<unsigned char>(n + 1));
        const auto old_end = path.end;
        path.write(old_end, added.data(), added.size());
        expected.insert(expected.end(), added.begin(), added.end());
        Measure earlier;
        for (const auto at : {std::size_t(17), page + 29, 2 * page + 41}) {
            path.read(at, &byte, 1);
            require(byte == 0x3b, "append retains bytes in complete earlier pages");
        }
        const auto stable = earlier.finish()[ids];
        earlier_bytes += stable.read_bytes;
        require(stable.reads == 0 && stable.read_requests == 0 && stable.read_bytes == 0,
                "interleaved appends never refill complete earlier lines (calls AND bytes)");
        Measure tail;
        std::array<unsigned char, 73> actual{};
        path.read(old_end, actual.data(), actual.size());
        const auto refill = tail.finish()[ids];
        tail_bytes += refill.read_bytes;
        require(actual == added && refill.reads > 0 && refill.past_end == 0,
                "old clipped tail refills after growth, including page crossings");
    }
    require(image(path.file.descriptor()) == expected, "all appended physical bytes match independent input");
    std::printf("sc412 append earlier_read_bytes=%llu tail_read_bytes=%llu\n",
                static_cast<unsigned long long>(earlier_bytes), static_cast<unsigned long long>(tail_bytes));
}
void fold_append_reads() {
    auto table = table_with_cache<IdPairStore<std::int64_t, true>>(budget / 2);
    table.set_directory(".");
    std::map<std::string, std::int64_t> expected;
    const auto check_fold = [&] {
        pineforge::BrokerStateHashSink actual, reference;
        table.fold_sorted(actual);
        reference.u(expected.size());
        for (const auto& row : expected) { reference.s(row.first); reference.i(row.second); }
        require(actual.h == reference.h, "ordered fold matches independent input-map spelling");
    };
    std::uint64_t earlier_bytes = 0;
    for (int n = 0; n < 64; ++n) {
        const auto key = std::string(200, 'p') + std::to_string(10000 + n);
        const std::int64_t value = n * 19 - 517;
        table.insert_or_assign(key, value);
        expected[key] = value;
        // publish_fold reads older keys before data_bytes_ advances; its
        // OLD-end clipped data tail must be invalidated again on publication.
        require(table.at(key) == value, "new fold-store key readable immediately after publication");
        check_fold();
        if (n == 47) require(table.at(expected.begin()->first) == expected.begin()->second, "prime early page");
        if (n >= 48) {
            // No index replacement at 48..64 rows, and data pages 0..3 plus
            // the one index page occupy distinct slots in the 32 KiB cache.
            Measure earlier;
            require(table.at(expected.begin()->first) == expected.begin()->second, "earlier fold key unchanged");
            const auto io = earlier.finish()[pairs];
            earlier_bytes += io.read_bytes;
            require(io.reads == 0 && io.read_requests == 0 && io.read_bytes == 0,
                    "fold append publication retains complete early pages and index lines");
        }
        for (const auto& row : expected) require(table.at(row.first) == row.second, "all interleaved fold reads");
    }
    const auto key = expected.begin()->first;
    table.insert_or_assign(key, 9301);
    expected[key] = 9301;
    require(table.at(key) == 9301, "fold pending overwrite visible");
    check_fold();
    require(table.erase(key), "fold erase");
    expected.erase(key);
    require(!table.lookup(key), "fold pending erase visible");
    check_fold();
    table.insert_or_assign(key, -41);
    expected[key] = -41;
    require(table.at(key) == -41, "fold resurrection");
    check_fold();
    // Flush the pending overwrite with a different mutation, then check disk reads.
    const auto last = expected.rbegin()->first;
    table.insert_or_assign(last, 17);
    expected[last] = 17;
    require(table.at(key) == -41, "flushed fold header invalidates cached data");
    check_fold();
    std::printf("sc412 fold_append earlier_read_bytes=%llu\n", static_cast<unsigned long long>(earlier_bytes));
}
void write_through() {
    reset_files();
    FilePath path(budget);
    std::array<unsigned char, 2 * page + 113> original{}, changed{};
    original.fill(0x31); changed.fill(0xa7);
    path.write(0, original.data(), original.size());
    require_cache_hit(path, 0);
    std::array<unsigned char, 2 * page + 113> actual{};
    path.read(0, actual.data(), actual.size()); // Warm both whole and partial lines.
    require(actual == original, "warm original bytes");
    for (const std::size_t at : {std::size_t(0), page - 11, page * 2 + 91}) {
        const std::size_t count = std::min<std::size_t>(37, original.size() - at);
        path.write(at, changed.data(), count);
        std::copy_n(changed.begin(), count, original.begin() + at);
        const auto physical = image(path.file.descriptor()); // Before any cache read or flush.
        require(std::equal(physical.begin(), physical.end(), original.begin(), original.end()),
                "write reached independent descriptor immediately");
        path.read(0, actual.data(), actual.size());
        require(actual == original, "next cached read sees overlapping write, including page boundary");
    }
    auto table = table_with_cache<DiskIdTable<std::uint64_t>>(budget);
    table.set_directory(".");
    const auto at = table.insert("updated", 11).first;
    require(table.get(at) == 11, "warm table value");
    const auto fd = files[file_count - 1].fd;
    table.set(at, 0xfedcba9876543210ULL);
    const auto disk = image(fd);
    std::uint64_t value = 0;
    std::memcpy(&value, disk.data() + at + 56, sizeof value);
    require(value == 0xfedcba9876543210ULL && table.get(at) == value, "table set is write-through");
    auto pair = table_with_cache<IdPairStore<std::uint64_t>>(budget / 2);
    pair.set_directory(".");
    pair.insert_or_assign("pair", 13);
    require(pair.at("pair") == 13, "warm pair header and index");
    int data_fd = -1;
    for (std::size_t i = 0; i < file_count; ++i) {
        struct stat status{};
        if (files[i].family == pairs && ::fstat(files[i].fd, &status) == 0 && status.st_size == 44)
            data_fd = files[i].fd;
    }
    require(data_fd >= 0, "observe actual 40-byte header plus key file");
    pair.insert_or_assign("pair", 97);
    const auto pair_disk = image(data_fd);
    std::memcpy(&value, pair_disk.data() + 24, sizeof value);
    require(value == 97 && pair.at("pair") == value, "pair overwrite is immediately on disk and in reads");
    require(pair.erase("pair") && !pair.lookup("pair"), "cached live flag follows erase");
    require(pair.insert_or_assign("pair", 101) && pair.at("pair") == 101, "cached tombstone resurrects");
}
void invalidation() {
    FilePath path(budget);
    std::array<unsigned char, page + 79> bytes{};
    bytes.fill(0x6b);
    path.write(0, bytes.data(), 71);
    require_cache_hit(path, 70);
    unsigned char actual = 0;
    path.read(70, &actual, 1);
    path.write(71, bytes.data() + 71, bytes.size() - 71);
    path.read(bytes.size() - 1, &actual, 1);
    require(actual == 0x6b, "growth refreshes clipped last line");
    path.resize(9);
    path.resize(page + 79);
    path.read(70, &actual, 1);
    require(actual == 0, "truncate and regrowth discard cached tail bytes");
    path.write(page - 5, bytes.data(), bytes.size());
    path.read(page - 4, &actual, 1);
    require(actual == 0x6b, "growth straddling the old end also invalidates its overwritten prefix");
    auto table = table_with_cache<DiskIdTable<std::uint64_t>>(budget);
    auto destination = table_with_cache<DiskIdTable<std::uint64_t>>(budget);
    table.set_directory("."); destination.set_directory(".");
    const auto old = table.insert("same", 7).first;
    require(table.get(old) == 7, "warm moved source");
    const auto replaced = destination.insert("same", 91).first;
    require(destination.get(replaced) == 91, "warm replaced destination");
    destination = std::move(table);
    {
        Measure measure;
        require(destination.get(old) == 7, "move assignment uses source bytes");
        require(measure.finish()[ids].reads > 0, "move assignment discards source and destination lines");
    }
    table.set_directory(".");
    table.insert("same", 33);
    require(table.get(*table.find("same")) == 33, "moved-from table has no stale identity");
    DiskIdTable<std::uint64_t> constructed(std::move(destination));
    {
        Measure measure;
        require(constructed.get(old) == 7, "move construction uses source bytes");
        require(measure.finish()[ids].reads > 0, "move construction refills through real pread");
    }
    constructed.clear();
    const auto fresh = constructed.insert("same", 55).first;
    require(constructed.get(fresh) == 55, "clear/reopen at reused offset");
    auto pairs_a = table_with_cache<IdPairStore<std::uint64_t>>(budget / 2);
    auto pairs_b = table_with_cache<IdPairStore<std::uint64_t>>(budget / 2);
    pairs_a.set_directory("."); pairs_b.set_directory(".");
    pairs_a.insert_or_assign("same", 71);
    pairs_b.insert_or_assign("same", 72);
    require(pairs_a.at("same") == 71 && pairs_b.at("same") == 72, "warm pair files");
    pairs_b = std::move(pairs_a);
    {
        Measure measure;
        require(pairs_b.at("same") == 71, "pair move invalidates both file namespaces");
        require(measure.finish()[pairs].reads > 0, "pair move refills");
    }
    pairs_b.clear();
    pairs_b.insert_or_assign("same", 73);
    for (int n = 0; n < 300; ++n) {
        pairs_b.insert_or_assign("grow-" + std::to_string(n), n);
        require(pairs_b.at("same") == 73, "data growth and replacement hash index preserve old key");
    }
}
struct Failure {
    int error = 0;
    std::string text;
    std::array<unsigned char, 80> bytes{};
    std::uint64_t reads = 0, requests = 0, past_end = 0;
};
Failure failing_read(std::size_t capacity_bytes, std::uint64_t offset, std::size_t length) {
    FilePath path(capacity_bytes);
    std::array<unsigned char, page + 37> input{};
    input.fill(0x92);
    path.write(0, input.data(), input.size());
    unsigned char warm = 0;
    path.read(input.size() - 1, &warm, 1);
    Failure result;
    result.bytes.fill(0xcd);
    Measure measure;
    try { path.read(offset, result.bytes.data(), length); }
    catch (const std::system_error& error) { result.error = error.code().value(); result.text = error.what(); }
    const auto io = measure.finish()[ids];
    result.reads = io.reads; result.requests = io.read_requests; result.past_end = io.past_end;
    return result;
}
void eof() {
    for (const auto offset : {page + 37, page + 29, page + 38}) {
        const auto a = failing_read(budget, offset, 16), b = failing_read(0, offset, 16);
        require(a.error == EIO && a.error == b.error && a.text == b.text && a.bytes == b.bytes,
                "at/across/beyond end preserves errno, exact text and copied prefix");
        require(a.reads == b.reads && a.requests == b.requests && a.past_end == b.past_end,
                "invalid read is forwarded exactly, without speculative fill");
    }
    FilePath path(budget);
    std::array<unsigned char, page + 37> data{};
    path.write(0, data.data(), data.size());
    require_cache_hit(path, 0);
    Measure measure;
    unsigned char byte = 0;
    path.read(path.end - 1, &byte, 1);
    const auto io = measure.finish()[ids];
    require(io.reads > 0 && io.past_end == 0 && io.read_requests <= 37,
            "last partial line fill stops exactly at logical end");
}

struct FileSizeLimit {
    struct rlimit saved{};
    struct sigaction previous{};
    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &saved) == 0 && bytes <= saved.rlim_cur,
                "read and lower RLIMIT_FSIZE");
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        ::sigemptyset(&ignored.sa_mask);
        require(::sigaction(SIGXFSZ, &ignored, &previous) == 0, "ignore SIGXFSZ for real EFBIG");
        auto limit = saved;
        limit.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            ::sigaction(SIGXFSZ, &previous, nullptr);
            throw std::runtime_error("set RLIMIT_FSIZE");
        }
    }
    ~FileSizeLimit() {
        if (::setrlimit(RLIMIT_FSIZE, &saved) != 0
            || ::sigaction(SIGXFSZ, &previous, nullptr) != 0) std::abort();
    }
};
void write_failure() {
    reset_files();
    FilePath path(budget);
    std::array<unsigned char, 2 * page + 113> original{}, changed{}, actual{};
    original.fill(0x31); changed.fill(0xa7);
    path.write(0, original.data(), original.size());
    require_cache_hit(path, 0);
    constexpr std::size_t limit = page + 23;
    for (const bool partial : {false, true}) {
        path.read(0, actual.data(), actual.size()); // Warm the failed write's page.
        const auto offset = partial ? limit - 17 : limit + 9;
        bool failed = false;
        Measure writes;
        {
            FileSizeLimit restriction(limit);
            try { path.write(offset, changed.data(), 64); }
            catch (const AuxiliaryScratchIoError& error) {
                failed = error.code().value() == EFBIG;
            }
        }
        const auto io = writes.finish()[ids];
        require(failed && io.writes > 0 && io.write_bytes == (partial ? 17 : 0),
                "real RLIMIT_FSIZE rejection/partial prefix preserves typed EFBIG");
        if (partial) std::copy_n(changed.begin(), 17, original.begin() + offset);
        const auto physical = image(path.file.descriptor());
        require(std::equal(physical.begin(), physical.end(), original.begin(), original.end()),
                "failed write persisted exactly the kernel's prefix, no cached failed bytes");
        Measure reads;
        path.read(0, actual.data(), actual.size());
        const auto refill = reads.finish()[ids];
        require(actual == original && refill.reads > 0,
                "even a failed write invalidates before I/O and refills actual disk bytes");
    }
    auto table = table_with_cache<DiskIdTable<std::uint64_t>>(budget);
    table.set_directory(".");
    constexpr std::uint64_t old_value = 0x3131313131313131ULL, new_value = 0xa7a7a7a7a7a7a7a7ULL;
    const auto at = table.insert("value", old_value).first;
    require(table.get(at) == old_value, "warm actual tree before short value write");
    const int tree_fd = files[file_count - 1].fd;
    bool failed = false;
    {
        FileSizeLimit restriction(at + 56 + 3);
        try { table.set(at, new_value); }
        catch (const AuxiliaryScratchIoError& error) { failed = error.code().value() == EFBIG; }
    }
    std::uint64_t expected = old_value;
    std::memcpy(&expected, &new_value, 3);
    const auto tree_image = image(tree_fd);
    require(failed && std::memcmp(tree_image.data() + at + 56, &expected, sizeof expected) == 0
            && table.get(at) == expected, "tree failed-write path discards warmed old value");
    auto pair = table_with_cache<IdPairStore<std::uint64_t>>(budget / 2);
    pair.set_directory(".");
    pair.insert_or_assign("pair", old_value);
    require(pair.at("pair") == old_value, "warm actual pair before partial header write");
    int data_fd = -1;
    for (std::size_t i = 0; i < file_count; ++i) {
        struct stat status{};
        if (files[i].family == pairs && ::fstat(files[i].fd, &status) == 0
            && status.st_dev == files[i].device && status.st_ino == files[i].inode && status.st_size == 44)
            data_fd = files[i].fd;
    }
    require(data_fd >= 0, "actual pair data descriptor");
    failed = false;
    {
        FileSizeLimit restriction(24 + 3);
        try { pair.insert_or_assign("pair", new_value); }
        catch (const IdPairStoreError& error) { failed = error.code().value() == EFBIG; }
    }
    const auto pair_image = image(data_fd);
    require(failed && std::memcmp(pair_image.data() + 24, &expected, sizeof expected) == 0
            && pair.at("pair") == expected, "pair failed-write path discards warmed old header");
}
void thrash() {
    constexpr std::size_t pages = 3 * (budget / page) + 1; // 49 pages, only 16 slots.
    std::vector<unsigned char> input(pages * page);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<unsigned char>((i / page * 17 + i % page) % 251);
    const auto run = [&](std::size_t capacity_bytes) {
        FilePath path(capacity_bytes);
        path.write(0, input.data(), input.size());
        std::uint64_t digest = basis;
        Measure measure;
        for (int repeat = 0; repeat < 3; ++repeat) {
            for (std::size_t n = 0; n < pages; ++n) {
                const auto offset = ((n * 17) % pages) * page + 19;
                for (int twice = 0; twice < 2; ++twice) {
                    std::array<unsigned char, 64> bytes{};
                    path.read(offset, bytes.data(), bytes.size());
                    require(std::equal(bytes.begin(), bytes.end(), input.begin() + offset),
                            "evicted/reused slot returns independent input bytes");
                    digest = hash(digest, bytes.data(), bytes.size());
                }
            }
        }
        return std::make_pair(digest, measure.finish()[ids]);
    };
    const auto cached = run(budget), direct = run(0);
    require(cached.first == direct.first && cached.second.reads >= pages
            && cached.second.reads < direct.second.reads && cached.second.past_end == 0,
            "thrash is identical to uncached, really evicts, and makes strictly fewer calls");
    std::printf("sc412 thrash preads=%llu/%llu bytes=%llu/%llu\n",
                static_cast<unsigned long long>(cached.second.reads),
                static_cast<unsigned long long>(direct.second.reads),
                static_cast<unsigned long long>(cached.second.read_bytes),
                static_cast<unsigned long long>(direct.second.read_bytes));
}
struct RefuseAllocations {
    bool saved = global_allocation::refusing;
    RefuseAllocations() { global_allocation::refused = 0; global_allocation::refusing = true; }
    ~RefuseAllocations() { global_allocation::refusing = saved; }
};
void allocation_failure() {
    FilePath path(budget);
    std::array<unsigned char, page + 113> input{};
    input.fill(0x49);
    path.write(0, input.data(), input.size()); // No read allocation yet.
    std::array<unsigned char, 32> actual{};
    Measure measure;
    {
        RefuseAllocations refusal; // Executable allocator, no product seam.
        path.read(9, actual.data(), actual.size());
    }
    const auto io = measure.finish()[ids];
    require(global_allocation::refused > 0 && io.reads == 1
            && io.read_requests == actual.size() && io.read_bytes == actual.size()
            && std::equal(actual.begin(), actual.end(), input.begin() + 9),
            "failed lazy allocation falls back to the exact direct read without bad_alloc");
    require_cache_hit(path, 9); // A later successful allocation really engages.
}

// Measure allocation bytes, not sizeof(table) or a product cache counter.
inline std::size_t cache_blocks = 0, cache_bytes = 0;
void allocated(std::size_t bytes) {
    if (bytes >= 32 * 1024) { ++cache_blocks; cache_bytes += bytes; }
}
struct AllocationWindow {
    AllocationWindow() {
        cache_blocks = cache_bytes = 0;
        global_allocation::on_recorded = allocated;
        global_allocation::recording = true;
    }
    ~AllocationWindow() { global_allocation::recording = false; global_allocation::on_recorded = nullptr; }
};
void default_disabled() {
    require_disk_cache(); // Negative guards must not bless a no-op cache.
    DiskIdTable<std::uint64_t> tree;
    DiskResidentIdTable<Row> resident;
    IdPairStore<std::uint64_t> erased;
    IdPairStore<std::int64_t, true> cycles;
    tree.set_directory("."); resident.set_directory(".");
    erased.set_directory("."); cycles.set_directory(".");
    AllocationWindow allocations;
    const auto at = tree.insert("id", 11).first;
    resident.emplace("id", Row{13});
    erased.insert_or_assign("id", 17); cycles.insert_or_assign("id", 19);
    Measure measure;
    for (int n = 0; n < 64; ++n)
        require(tree.get(at) == 11 && resident.find("id")->second.value == 13
                && erased.at("id") == 17 && cycles.at("id") == 19, "all default tables read real bytes");
    const auto io = measure.finish();
    require(cache_blocks == 0 && cache_bytes == 0 && io[ids].reads >= 128 && io[pairs].reads >= 128,
            "default tables have zero capacity/no allocation; only adapter tables opt in");
}
constexpr int budget_passes = 3;
constexpr std::size_t budget_key_bytes = 192;
std::string budget_key(int n) {
    auto key = std::to_string(n);
    key.resize(budget_key_bytes, 'x');
    return key;
}
struct BudgetObservation {
    std::size_t resident_bytes = 0;
    Traffic io;
    std::vector<std::uint64_t> values;
    std::vector<std::vector<unsigned char>> disk;
};
void budget_oracle(const BudgetObservation& cached, const BudgetObservation& direct,
                   std::size_t data_pages, std::size_t slots) {
    require(cached.values == direct.values && !cached.disk.empty() && cached.disk == direct.disk,
            "budget scan values and physical images equal uncached oracle bit for bit");
    require(direct.resident_bytes == 0 && direct.io.reads > 0 && cached.io.reads < direct.io.reads,
            "budget scans make strictly fewer real reads than uncached oracle");
    require(cached.io.read_requests <= direct.io.read_requests
            && cached.io.read_bytes <= direct.io.read_bytes,
            "budget scan requested and returned bytes never exceed uncached oracle");
    require(cached.io.past_end == 0 && direct.io.past_end == 0,
            "budget scans never read beyond the logical end");
    require(data_pages > slots && cached.io.reads >= budget_passes * (data_pages - slots),
            "oversubscribed budget scans repeatedly refill evicted pages");
}
std::size_t budget_for(int entries) {
    const auto run = [&](std::size_t capacity_bytes) {
        reset_files();
        BudgetObservation result;
        result.values.reserve(budget_passes * entries); // Oracle storage is outside allocation accounting.
        auto table = table_with_cache<DiskIdTable<std::uint64_t>>(capacity_bytes);
        table.set_directory(".");
        const auto hot = budget_key(0);
        {
            AllocationWindow window;
            const auto at = table.insert(hot, 29).first;
            require(table.get(at) == 29, "allocate read cache on real disk path");
            require(cache_blocks == (capacity_bytes ? 1U : 0U) && cache_bytes == capacity_bytes,
                    "first read allocates fixed cache");
            Measure warm;
            for (int n = 0; n < 64; ++n) require(table.get(at) == 29, "budget cache engagement");
            const auto hot_io = warm.finish()[ids];
            require(capacity_bytes ? hot_io.reads < 64 : hot_io.reads == 64,
                    "budget cache makes strictly fewer calls");
            for (int n = 1; n < entries; ++n) table.insert(budget_key(n), n);
            const auto found = table.find(hot);
            require(found && table.get(*found) == 29, "budget population retains hot lookup");
            require(table.file_bytes() == 1 + entries * (56 + 8 + budget_key_bytes),
                    "tree population has the specified physical extent");
            // Scan in append order: key/value/link reads cover every node and page.
            // Only the read phase is measured; construction still counts allocations.
            Measure measure;
            for (int pass = 0; pass < budget_passes; ++pass) {
                int n = 0;
                for (auto row = table.first_inserted(); row != 0; row = table.next_inserted(row)) {
                    require(n < entries && table.key(row) == budget_key(n), "budget tree scan key");
                    const auto value = table.get(row);
                    require(value == (n == 0 ? 29U : static_cast<std::uint64_t>(n)),
                            "budget tree scan value");
                    result.values.push_back(value);
                    ++n;
                    if (n == 1) {
                        // Page zero was just loaded by the first row.  The
                        // two immediate probes reuse that one page before
                        // the remaining 32/64-page cold sweep evicts it:
                        // 2 cache-hit reads per pass, 3 * 2 = 6 deliberate
                        // tree hits, with no extra cached read bytes.
                        const auto hot_value = table.get(at);
                        const auto hot_again = table.get(at);
                        require(hot_value == 29U && hot_again == 29U,
                                "budget tree hot page cache hit");
                    }
                }
                require(n == entries, "budget tree scan covers the complete population");
                require(cache_blocks == (capacity_bytes ? 1U : 0U) && cache_bytes == capacity_bytes,
                        "exactly one fixed 64 KiB allocation");
            }
            result.io = measure.finish()[ids];
            result.resident_bytes = cache_bytes;
        }
        result.disk = images(); // Image/oracle allocations are not cache allocations.
        return result;
    };
    const auto cached = run(budget), direct = run(0);
    const auto data_pages = (1 + entries * (56 + 8 + budget_key_bytes) + page - 1) / page;
    budget_oracle(cached, direct, data_pages, budget / page);
    return cached.resident_bytes;
}
std::size_t pair_budget_for(int entries) {
    const auto run = [&](std::size_t capacity_bytes) {
        reset_files();
        BudgetObservation result;
        result.values.reserve(budget_passes * entries);
        auto table = table_with_cache<IdPairStore<std::uint64_t>>(capacity_bytes);
        table.set_directory(".");
        const auto hot = budget_key(0);
        {
            AllocationWindow window;
            table.insert_or_assign(hot, 43);
            require(table.at(hot) == 43, "warm actual pair reads");
            require(cache_blocks == (capacity_bytes ? 1U : 0U) && cache_bytes == capacity_bytes,
                    "one opted-in 32 KiB data/index allocation");
            for (int n = 0; n < 64; ++n)
                require(table.at(hot) == 43, "pair budget cache engagement");
            Measure warm;
            // A disk iterator's first row is one data page: its 40-byte
            // header and 192-byte key are both in page zero.  Thirty-two
            // probes therefore make exactly 64 direct preads while the
            // opted-in cache reuses that one page after its first fill.
            for (int n = 0; n < 32; ++n) {
                const auto row = table.begin();
                require((*row).first == hot && (*row).second == 43U,
                        "pair budget cache page engagement");
            }
            const auto hot_io = warm.finish()[pairs];
            require(capacity_bytes ? hot_io.reads < 64 : hot_io.reads == 64,
                    "pair budget cache makes strictly fewer calls");
            for (int n = 1; n < entries; ++n) table.insert_or_assign(budget_key(n), n);
            require(table.at(hot) == 43, "pair budget reads");
            // Iteration reads each 40-byte header and all 192 key bytes, in order,
            // avoiding index/data conflict amplification in the byte comparison.
            Measure measure;
            for (int pass = 0; pass < budget_passes; ++pass) {
                int n = 0;
                for (const auto& row : table) {
                    require(n < entries && row.first == budget_key(n), "budget pair scan key");
                    require(row.second == (n == 0 ? 43U : static_cast<std::uint64_t>(n)),
                            "budget pair scan value");
                    result.values.push_back(row.second);
                    ++n;
                    if (n == 1) {
                        // The first row's header and key have page zero
                        // resident.  Both immediate begin() probes reuse it
                        // before the remaining 28/57 pages evict that page
                        // from the 8 shared slots: 3 * 2 = 6 hot probes, or
                        // 3 * 2 * 2 = 12 deliberate data-page read hits
                        // because each begin() reads a header and a key; the
                        // cached run adds no fill bytes here.
                        const auto hot_row = table.begin();
                        require((*hot_row).first == hot && (*hot_row).second == 43U,
                                "budget pair hot page");
                        const auto hot_again = table.begin();
                        require((*hot_again).first == hot && (*hot_again).second == 43U,
                                "budget pair hot page cache hit");
                    }
                }
                require(n == entries, "budget pair scan covers the complete population");
                require(cache_blocks == (capacity_bytes ? 1U : 0U) && cache_bytes == capacity_bytes,
                        "rehashing never grows resident page cache");
            }
            result.io = measure.finish()[pairs];
            result.resident_bytes = cache_bytes;
        }
        result.disk = images();
        return result;
    };
    const auto cached = run(budget / 2), direct = run(0);
    const auto data_pages = (entries * (40 + budget_key_bytes) + page - 1) / page;
    budget_oracle(cached, direct, data_pages, budget / 2 / page);
    return cached.resident_bytes;
}
void fixed_budget() {
    // 512/1024 entries, three complete cold scans, 192-byte keys:
    // tree = 1 + N*(56+8+192) bytes = 33/65 pages vs 16 slots (64 KiB),
    // forcing at least 3*(33-16)=51 / 3*(65-16)=147 replacements.  The
    // final cold page is 32/64 in slot zero, so each next pass refills page
    // zero at its first row and immediately reuses it twice: 3 * 2 = 6
    // deliberate cache-hit reads before the cold sweep evicts it again.
    // Pair data = N*(40+192) bytes = 29/58 pages vs 8 shared slots (32 KiB),
    // forcing at least 3*(29-8)=63 / 3*(58-8)=150 replacements from data
    // alone.  Its first row keeps page zero resident for two immediate
    // iterator probes on every pass: 3 * 2 = 6 hot probes, or 12 cache-hit
    // data-page reads.  The final cold sweep leaves slot zero holding page
    // 24/56.  Its 1024/2048 16-byte index slots add 4/8 pages and still force
    // rehashes.
    const auto small = budget_for(512);
    const auto large = budget_for(1024);
    require(small == large, "512/1024 distinct entries and complete scans retain identical cache bytes");
    const auto pair_small = pair_budget_for(512);
    const auto pair_large = pair_budget_for(1024);
    require(pair_small == pair_large && 2 * small + 2 * pair_small == 192 * 1024,
            "two trees plus two pair-store halves retain exactly 192 KiB");
    auto tree = table_with_cache<DiskResidentIdTable<Row>>(budget);
    auto ranks = table_with_cache<DiskIdTable<std::uint32_t>>(budget);
    auto erased = table_with_cache<IdPairStore<std::uint64_t>>(budget / 2);
    auto cycles = table_with_cache<IdPairStore<std::int64_t, true>>(budget / 2);
    tree.set_directory("."); ranks.set_directory(".");
    erased.set_directory("."); cycles.set_directory(".");
    AllocationWindow window;
    tree.emplace("id", Row{3});
    const auto rank = ranks.insert("id", 4).first;
    erased.insert_or_assign("id", 5); cycles.insert_or_assign("id", 6);
    require(tree.find("id")->second.value == 3 && ranks.get(rank) == 4
            && erased.at("id") == 5 && cycles.at("id") == 6,
            "all four real table constructions are read");
    require(cache_blocks == 4 && cache_bytes == 192 * 1024,
            "complete 192 KiB cache budget per adapter, not per engine");
}
void bypass() {
    require_disk_cache(); // Establish the private-disk positive control first.
    ScratchManifest manifest;
    manifest.U = 4; manifest.B_U = 32;
    manifest.K_e = 4; manifest.B_e = 32;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    const auto slab_run = [&](std::size_t bytes) {
        auto tree = table_with_cache<DiskIdTable<std::uint64_t>>(bytes);
        tree.bind_scratch(slab, ScratchResource::cohort);
        auto pair = table_with_cache<IdPairStore<std::uint64_t>>(bytes / 2);
        pair.bind_scratch_tables(slab, ScratchResource::erased_data, ScratchResource::erased_index);
        AllocationWindow allocations;
        Measure measure;
        const auto at = tree.insert("slab", 19).first;
        pair.insert_or_assign("slab", 23);
        for (int n = 0; n < 1000; ++n)
            require(tree.get(*tree.find("slab")) == 19 && pair.at("slab") == 23, "slab bytes");
        const auto io = measure.finish();
        require(tree.bounded_cohorts() && !tree.arena_backed()
                && slab->region(ScratchResource::cohort).logical_extent == at + 56 + 8 + 4,
                "constructed shape is the slab region, not an anonymous disk file");
        require(cache_blocks == 0 && cache_bytes == 0, "slab allocates no page cache");
        return io;
    };
    const auto a = slab_run(budget), b = slab_run(0);
    require(a[ids].reads == b[ids].reads && a[pairs].reads == b[pairs].reads
            && a[ids].reads == 0 && a[pairs].reads == 0, "slab bypass has uncached syscall count");
    for (const bool sealed : {false, true}) {
        int backing = -1;
        if (sealed) backing = ::memfd_create("sc412-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        else {
            char name[] = "./sc412-arena-XXXXXX";
            backing = ::mkstemp(name);
            require(backing >= 0 && ::unlink(name) == 0, "regular inherited arena backing");
        }
        require(backing >= 0 && ::ftruncate(backing, 8 * ScratchArena::kChunkBytes) == 0, "size arena");
        if (sealed) require(::fcntl(backing, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                            "sealed memfd backing");
        // The same inherited-fd environment admission path as the host.
        if (backing == ScratchArena::kInheritedFd) {
            const int relocated = ::fcntl(backing, F_DUPFD_CLOEXEC, 10);
            require(relocated >= 0, "keep caller backing distinct from fd 5");
            ::close(backing);
            backing = relocated;
        }
        const int saved = ::fcntl(ScratchArena::kInheritedFd, F_DUPFD_CLOEXEC, 20);
        require(::dup2(backing, ScratchArena::kInheritedFd) == ScratchArena::kInheritedFd, "inherit fd 5");
        ::close(backing);
        ::setenv("PF_SCRATCH_ARENA_FD", "5", 1);
        ::setenv("PF_SCRATCH_ARENA_BYTES", "8388608", 1);
        ::setenv("PF_SCRATCH_ARENA_JAILED", sealed ? "1" : "0", 1);
        ScratchArena::attach_from_environment();
        std::array<std::uint64_t, 2> reads{};
        for (int mode = 0; mode < 2; ++mode) {
            auto tree = table_with_cache<DiskIdTable<std::uint64_t>>(mode == 0 ? budget : 0);
            auto pair = table_with_cache<IdPairStore<std::uint64_t>>(mode == 0 ? budget / 2 : 0);
            AllocationWindow allocations;
            const auto at = tree.insert("arena", 31).first;
            pair.insert_or_assign("arena", 37);
            Measure measure;
            for (int n = 0; n < 1000; ++n)
                require(tree.get(at) == 31 && pair.at("arena") == 37, "arena repeated reads");
            const auto io = measure.finish();
            reads[mode] = io[ids].reads + io[pairs].reads + io[other].reads;
            require(tree.arena_backed() && cache_bytes == 0 && cache_blocks == 0,
                    "arena shape bypasses heap cache in either jail profile");
        }
        require(reads[0] == reads[1], "arena syscalls identical to disabled construction");
        ScratchArena::detach();
        ::close(ScratchArena::kInheritedFd);
        if (saved >= 0) { require(::dup2(saved, ScratchArena::kInheritedFd) >= 0, "restore fd 5"); ::close(saved); }
        ::unsetenv("PF_SCRATCH_ARENA_FD"); ::unsetenv("PF_SCRATCH_ARENA_BYTES"); ::unsetenv("PF_SCRATCH_ARENA_JAILED");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string row = argc > 1 ? argv[1] : "io";
        if (row == "io") repeated_lookups();
        else if (row == "append") append_reads();
        else if (row == "fold_append") fold_append_reads();
        else if (row == "write") write_through();
        else if (row == "fsize") write_failure();
        else if (row == "invalidate") invalidation();
        else if (row == "eof") eof();
        else if (row == "bypass") bypass();
        else if (row == "budget") fixed_budget();
        else if (row == "default") default_disabled();
        else if (row == "thrash") thrash();
        else if (row == "alloc_failure") allocation_failure();
        else throw std::runtime_error("unknown sc412 test row");
        std::printf("test_p2cx_sc412: %s completed\n", row.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_p2cx_sc412: %s\n", error.what());
        return 1;
    }
}
