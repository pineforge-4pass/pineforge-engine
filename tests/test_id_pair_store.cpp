// CR-150 L19l-C. Reference maps, real anonymous files, and a retained-byte
// witness. Fail-before against b3c39238e: id_pair_store.hpp is absent.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/id_pair_store.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using Store = detail::IdPairStore<std::uint64_t>;
using Key = std::pair<std::string, std::optional<std::string>>;
using Reference = std::map<Key, std::uint64_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ScratchDirectory {
    std::string path = "/tmp/pineforge-id-pair-test-XXXXXX";
    ScratchDirectory() {
        require(::mkdtemp(path.data()) != nullptr, "create test scratch directory");
    }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }
};

bool put(Store& store, const Key& key, std::uint64_t value) {
    return key.second ? store.insert_or_assign(key.first, *key.second, value)
                      : store.insert_or_assign(key.first, value);
}
std::optional<std::uint64_t> get(const Store& store, const Key& key) {
    return key.second ? store.lookup(key.first, *key.second) : store.lookup(key.first);
}
bool erase(Store& store, const Key& key) {
    return key.second ? store.erase(key.first, *key.second) : store.erase(key.first);
}
void check_rows(const Store& store, const Reference& reference) {
    require(store.size() == reference.size(), "reference/live row counts agree");
    Reference observed;
    for (const auto& row : store) {
        require(observed.emplace(Key{row.first, row.from_entry}, row.second).second,
                "iteration yields each key once");
    }
    require(observed == reference, "iteration exactly matches reference keys and values");
    for (const auto& row : reference)
        require(get(store, row.first) == row.second, "old key remains findable after rehash");
}

void reference_insert_find_erase_recur(bool memory_reference) {
    ScratchDirectory scratch;
    Store store;
    store.set_directory(scratch.path);
    if (memory_reference) store.fixture_use_memory_reference();
    Reference reference;
    require(store.begin() == store.end() && !store.lookup("absent"), "new table is empty");
    const std::vector<Key> adversarial{
        {"", std::nullopt}, {"", std::string{}}, {"x", std::nullopt},
        {"x", std::string{}}, {"x\x1f", std::string("y")},
        {"x", std::string("\x1fy")}, {std::string("a\0b", 3), std::string("c\0d", 3)},
        {std::string(4097, 'L'), std::string(3073, 'R')}
    };
    std::uint64_t next = 0;
    for (const auto& key : adversarial) {
        require(put(store, key, next) == reference.insert_or_assign(key, next).second,
                "single/pair domains, binary ids and long strings insert exactly");
        ++next;
    }
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const Key key{"entry-" + std::to_string(i), "from-" + std::to_string(i % 71)};
        require(put(store, key, i) == reference.insert_or_assign(key, i).second,
                "many distinct pairs insert exactly");
        const Key shared{"shared-exit", "entry-" + std::to_string(i)};
        require(put(store, shared, i + 1) == reference.insert_or_assign(shared, i + 1).second,
                "one id with many from_entry keys inserts exactly");
    }
    check_rows(store, reference);
    const auto bytes = store.disk_bytes();
    for (std::uint64_t i = 0; i < 10000; i += 3) {
        const Key key{"entry-" + std::to_string(i), "from-" + std::to_string(i % 71)};
        require(erase(store, key) == (reference.erase(key) != 0), "erase matches reference");
        require(!get(store, key) && !erase(store, key), "erased key is absent");
    }
    check_rows(store, reference);
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const Key key{"entry-" + std::to_string(i), "from-" + std::to_string(i % 71)};
        require(put(store, key, i + 10000) == reference.insert_or_assign(key, i + 10000).second,
                "recur/update matches reference");
    }
    require(store.disk_bytes() == bytes, "recur and updates reuse old disk records");
    check_rows(store, reference);
    Store moved(std::move(store));
    require(store.empty() && store.disk_bytes() == 0, "moved-from table is reset");
    Store assigned;
    assigned.insert_or_assign("discarded", 99);
    assigned = std::move(moved);
    check_rows(assigned, reference);
    assigned.clear();
    require(assigned.empty() && assigned.begin() == assigned.end() && assigned.disk_bytes() == 0,
            "clear releases all files and old keys");
    require(assigned.insert_or_assign("fresh", 17), "clear permits a new run");
    require(!assigned.lookup("shared-exit", "entry-0"), "previous run keys do not leak");
    require(memory_reference ? assigned.disk_bytes() == 0 : assigned.disk_bytes() > 0,
            "fixture backend choice survives clear and move");
}

void minima_and_signed_hash_surface() {
    Store minima;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> reference;
    std::unordered_map<std::string, std::uint64_t> any;
    const auto fold = [&](const std::string& id, const std::string& from, std::uint64_t value) {
        const auto whole = any.emplace(id, value);
        if (!whole.second) whole.first->second = std::min(whole.first->second, value);
        const auto pair = reference[id].emplace(from, value);
        if (!pair.second) pair.first->second = std::min(pair.first->second, value);
        minima.min_assign(id, value);
        minima.min_assign(id, from, value);
    };
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto id = "id-" + std::to_string(i % 31);
        const auto from = "entry-" + std::to_string(i % 113);
        fold(id, from, 10000 - i);
        fold(id, from, i);
    }
    fold("sentinel", "", std::numeric_limits<std::uint64_t>::max());
    for (const auto& row : any) {
        require(minima.lookup(row.first) == row.second, "any-entry minimum matches nested reference");
        for (const auto& pair : reference.at(row.first))
            require(minima.lookup(row.first, pair.first) == pair.second,
                    "pair minimum matches nested reference");
    }
    detail::IdPairStore<std::int64_t> cycles;
    std::unordered_map<std::string, std::int64_t> old_cycles;
    for (std::int64_t i = 0; i < 600; ++i) {
        const auto key = "exit-" + std::to_string(i) + "\x1f" + "entry-" + std::to_string(i % 13);
        const auto value = i % 2 == 0 ? -i : i;
        cycles.insert_or_assign(key, value);
        old_cycles[key] = value;
    }
    cycles.insert_or_assign("minimum", std::numeric_limits<std::int64_t>::min());
    old_cycles["minimum"] = std::numeric_limits<std::int64_t>::min();
    std::vector<std::string> disk_keys, old_keys;
    disk_keys.reserve(cycles.size());
    old_keys.reserve(old_cycles.size());
    for (const auto& row : cycles) disk_keys.push_back(row.first);
    for (const auto& row : old_cycles) old_keys.push_back(row.first);
    std::sort(disk_keys.begin(), disk_keys.end());
    std::sort(old_keys.begin(), old_keys.end());
    require(disk_keys == old_keys, "unchanged hash fold observes identical sorted key bytes");
    for (const auto& key : old_keys)
        require(cycles.at(key) == old_cycles.at(key), "unchanged hash fold observes signed values");
    bool missing = false;
    try { (void)cycles.at("missing"); }
    catch (const std::out_of_range&) { missing = true; }
    require(missing, "at of absent key fails explicitly");
}

struct Recording {
    Recording() { global_allocation::recording = true; }
    ~Recording() { global_allocation::recording = false; }
};
struct Refusing {
    Refusing() { global_allocation::refusing = true; }
    ~Refusing() { global_allocation::refusing = false; }
};

void heap_flat_in_distinct_pairs() {
    ScratchDirectory scratch;
    Recording recording;
    Store store;
    store.set_directory(scratch.path);
    const auto insert = [&](std::uint64_t i) {
        const auto id = "long-entry-for-heap-witness-" + std::to_string(i);
        const auto from = "long-from-entry-for-heap-witness-" + std::to_string(i);
        store.insert_or_assign(id, from, i);
    };
    for (std::uint64_t i = 0; i < 10; ++i) insert(i);
    const auto low = global_allocation::live_bytes;
    for (std::uint64_t i = 10; i < 10000; ++i) insert(i);
    const auto high = global_allocation::live_bytes;
    require(high <= low + 1024, "auxiliary RAM is flat from 10 to 10000 distinct pairs");
    require(store.size() == 10000 && store.disk_bytes() > 10000 * 40,
            "heap witness exercised disk storage of every pair");
    const std::string id = "long-entry-for-heap-witness-0";
    const std::string from = "long-from-entry-for-heap-witness-0";
    {
        Refusing refusing;
        require(store.lookup(id, from) == 0, "old pair lookup needs no heap allocation");
        require(!store.lookup(id, "different"), "missing pair lookup needs no heap allocation");
    }
    std::printf("id-pair heap: 10=%zu 10000=%zu bytes; disk=%llu\n", low, high,
                static_cast<unsigned long long>(store.disk_bytes()));
}

int data_descriptor(off_t size, std::uint64_t value) {
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 65536L) : 1024;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        std::array<std::uint64_t, 5> header{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 0
            && status.st_size == size
            && ::pread(fd, header.data(), sizeof(header), 0) == static_cast<ssize_t>(sizeof(header))
            && header[3] == value) {
            return fd;
        }
    }
    throw std::runtime_error("find this table's anonymous data file");
}

template<class Operation>
void expect_io_failure(Operation&& operation, int expected_errno) {
    bool caught = false;
    try { operation(); }
    catch (const std::system_error& error) {
        caught = error.code().value() == expected_errno
            && classify_run_failure(error).code == RunFailureCode::scratch_io_failed
            && !classify_run_failure(error).args;
    }
    require(caught, "IO failure carries errno and scratch_io_failed");
}

struct FileSizeLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler previous_handler = SIG_DFL;
    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        previous_handler = std::signal(SIGXFSZ, SIG_IGN);
        require(previous_handler != SIG_ERR, "ignore file-size signal for fixture");
        auto limit = previous;
        limit.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::signal(SIGXFSZ, previous_handler);
            throw std::runtime_error("set fixture file-size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, previous_handler);
    }
};

void file_lifetime_and_failures() {
    ScratchDirectory scratch;
    Store missing;
    missing.set_directory(scratch.path + "/missing");
    expect_io_failure([&] { missing.insert_or_assign("absent", 1); }, ENOENT);
    require(missing.empty(), "failed create publishes no logical row");
    missing.set_directory(scratch.path);
    require(missing.insert_or_assign("retry", 2), "failed create can be retried");

    Store store;
    store.set_directory(scratch.path);
    const std::string id = "descriptor-fixture";
    store.insert_or_assign(id, "from", 919);
    const int fd = data_descriptor(static_cast<off_t>(40 + id.size() + 4), 919);
    require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "scratch file closes on exec");
    require(::lseek(fd, 7, SEEK_SET) == 7, "set scratch file cursor");
    require(store.lookup(id, "from") == 919 && ::lseek(fd, 0, SEEK_CUR) == 7,
            "pread lookup does not move scratch file cursor");
    bool changed = false;
    try { store.set_directory(scratch.path + "/different"); }
    catch (const std::logic_error&) { changed = true; }
    require(changed, "directory cannot silently change after first write");
    require(::ftruncate(fd, 12) == 0, "truncate real scratch data for read fault");
    expect_io_failure([&] { (void)store.lookup(id, "from"); }, EIO);
    Store moved(std::move(store));
    moved.clear();
    errno = 0;
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "clear closes moved scratch file");

    Store full;
    full.set_directory(scratch.path);
    full.insert_or_assign("first", 3);
    {
        FileSizeLimit limit(96);
        expect_io_failure([&] { full.insert_or_assign(std::string(256, 'W'), 4); }, EFBIG);
    }
    require(full.size() == 1 && full.lookup("first") == 3, "failed append leaves old rows intact");
    require(full.insert_or_assign(std::string(256, 'W'), 4), "retry overwrites unpublished append");
    require(full.lookup(std::string(256, 'W')) == 4, "retried append is complete");

    Store resizing;
    resizing.set_directory(scratch.path);
    for (std::uint64_t i = 0; i < 32; ++i)
        resizing.insert_or_assign("resize-" + std::to_string(i), i);
    {
        FileSizeLimit limit(1024); // old 64-slot index fits, replacement does not
        expect_io_failure([&] { resizing.insert_or_assign("resize-new", 33); }, EFBIG);
    }
    require(resizing.size() == 32, "failed rehash publishes no logical row");
    for (std::uint64_t i = 0; i < 32; ++i)
        require(resizing.lookup("resize-" + std::to_string(i)) == i,
                "failed rehash keeps every old index slot");
    require(resizing.insert_or_assign("resize-new", 33), "rehash can be retried");
    require(resizing.lookup("resize-new") == 33, "retried rehash keeps new key");
}
} // namespace

int main() {
    try {
        reference_insert_find_erase_recur(false);
        reference_insert_find_erase_recur(true);
        minima_and_signed_hash_surface();
        heap_flat_in_distinct_pairs();
        file_lifetime_and_failures();
        std::puts("test_id_pair_store: all reference, heap and IO checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_id_pair_store: %s\n", error.what());
        return 1;
    }
}
