// CR-150 CX-D-P2. Red-first source: replay the same generated keys against
// std::map and file/arena storage; no arena capacity comes from observed IO.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

namespace {
using pineforge::detail::IdPairScratchFile;
using pineforge::detail::IdPairStoreError;
using pineforge::detail::ScratchArena;
using Key = std::pair<std::string, std::optional<std::string>>;
template<class Value> using Store = pineforge::detail::IdPairStore<Value>;
template<class Value> using Reference = std::map<Key, Value>;
template<class Value> using Rows = std::vector<std::pair<Key, Value>>;
constexpr auto kChunk = ScratchArena::kChunkBytes;
constexpr std::uint64_t kHeaderBytes = 5 * sizeof(std::uint64_t);
constexpr std::uint64_t kSlotBytes = 2 * sizeof(std::uint64_t);
constexpr std::uint64_t kDistinctIds = 24000;
constexpr std::uint64_t kWindowBytes = kChunk / 16;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#if defined(__linux__)
std::size_t regular_descriptors();
#endif

std::optional<std::size_t> arena_descriptor_baseline() {
#if defined(__linux__)
    if (ScratchArena::attached()) return regular_descriptors();
#endif
    return std::nullopt;
}

void check_descriptors(std::optional<std::size_t> expected) {
#if defined(__linux__)
    if (expected)
        require(regular_descriptors() == *expected, "live arena regions open no regular-file descriptor");
#else
    (void)expected;
#endif
}

template<class Operation>
void expect_id_io(Operation&& operation, int expected_errno) {
    bool caught = false;
    try { operation(); }
    catch (const IdPairStoreError& failure) {
        caught = failure.code().value() == expected_errno
            && pineforge::classify_run_failure(failure).code
                == pineforge::RunFailureCode::report_sink_failed;
    }
    require(caught, "IdPairStoreError retains errno and report_sink_failed");
}

template<class Operation>
void expect_logic(Operation&& operation) {
    bool caught = false;
    try { operation(); }
    catch (const std::logic_error&) { caught = true; }
    require(caught, "directory changes during use are rejected");
}

struct ScratchDirectory {
    std::string path = "/tmp/pineforge-id-pair-arena-XXXXXX";
    ScratchDirectory() {
        require(::mkdtemp(path.data()) != nullptr, "create private file-mode directory");
    }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }
};

std::uint64_t rounded(std::uint64_t bytes) {
    return (bytes / kChunk + (bytes % kChunk != 0 ? 1 : 0)) * kChunk;
}

// Independent format/growth model. Used counts every distinct key ever seen,
// including erased keys, and models rehash before the new record is appended.
struct Sizing {
    std::uint64_t data = 0;
    std::uint64_t slots = 0;
    std::uint64_t used = 0;
    std::uint64_t peak = 0;

    std::uint64_t steady() const { return rounded(data) + rounded(slots * kSlotBytes); }
    std::uint64_t logical() const { return data + slots * kSlotBytes; }
    void append(const Key& key) {
        if (slots == 0 || used == slots / 2) {
            const auto next = slots == 0 ? 64 : slots * 2;
            peak = std::max(peak, rounded(data) + rounded(slots * kSlotBytes)
                                      + rounded(next * kSlotBytes));
            slots = next;
        }
        data += kHeaderBytes + key.first.size() + (key.second ? key.second->size() : 0);
        ++used;
        peak = std::max(peak, steady());
    }
};

template<class Visitor>
void generated_keys(Visitor&& visit) {
    const std::array<Key, 8> adversarial{{
        {"", std::nullopt}, {"", std::string{}},
        {"x", std::nullopt}, {"x", std::string{}},
        {"x\x1f", std::string("y")}, {"x", std::string("\x1fy")},
        {std::string("a\0b", 3), std::string("c\0d", 3)},
        {std::string(4097, 'L'), std::string(3073, 'R')}
    }};
    std::uint64_t ordinal = 0;
    for (const auto& key : adversarial) visit(key, ordinal++);
    for (std::uint64_t i = 0; i < kDistinctIds; ++i) {
        const auto id = "exit-" + std::to_string(i);
        visit(Key{id, std::nullopt}, ordinal++);
        visit(Key{id, std::string{}}, ordinal++);
        visit(Key{id, "entry-" + std::to_string(i)}, ordinal++);
    }
}

Sizing workload_size() {
    Sizing model;
    generated_keys([&](const Key& key, std::uint64_t) { model.append(key); });
    return model;
}

template<class Value>
Value value_for(std::uint64_t ordinal) {
    if constexpr (std::is_signed_v<Value>) {
        if (ordinal == 0) return std::numeric_limits<Value>::min();
        if (ordinal == 1) return std::numeric_limits<Value>::max();
        const auto value = static_cast<Value>(ordinal * 17);
        return ordinal % 2 == 0 ? value : -value;
    } else {
        return std::numeric_limits<Value>::max() - ordinal * 17;
    }
}

template<class Value>
bool put(Store<Value>& store, const Key& key, Value value) {
    return key.second ? store.insert_or_assign(key.first, *key.second, value)
                      : store.insert_or_assign(key.first, value);
}
template<class Value>
std::optional<Value> get(const Store<Value>& store, const Key& key) {
    return key.second ? store.lookup(key.first, *key.second) : store.lookup(key.first);
}
template<class Value>
bool erase(Store<Value>& store, const Key& key) {
    return key.second ? store.erase(key.first, *key.second) : store.erase(key.first);
}
template<class Value>
void minimum(Store<Value>& store, const Key& key, Value value) {
    if (key.second) store.min_assign(key.first, *key.second, value);
    else store.min_assign(key.first, value);
}

void check_storage(const Sizing& model) {
    if (auto* arena = ScratchArena::attached()) {
        const auto stats = arena->stats();
        require(stats.bytes_used == model.steady(), "two regions use independently rounded bytes");
        require(stats.high_water == model.peak, "peak includes exactly old plus new index");
        require(stats.chunks_free * kChunk + stats.bytes_used == stats.bytes_total,
                "live and free chunks partition the supplied arena");
        require(stats.windows_mapped_bytes <= kWindowBytes, "mapped windows obey the configured cap");
    }
}

template<class Value>
void check_rows(const Store<Value>& store, const Reference<Value>& reference) {
    require(store.size() == reference.size(), "live count equals independent map");
    Reference<Value> observed;
    for (const auto& row : store) {
        require(observed.emplace(Key{row.first, row.from_entry}, row.second).second,
                "streaming iteration returns each record once");
    }
    require(observed == reference, "every iterated string byte and value equals the map");
    generated_keys([&](const Key& key, std::uint64_t) {
        const auto found = reference.find(key);
        const auto expected = found == reference.end() ? std::optional<Value>{}
                                                       : std::optional<Value>{found->second};
        require(get(store, key) == expected, "every live and erased record reads back exactly");
    });
}

template<class Value>
struct Replay {
    Rows<Value> rows;
    std::uint64_t disk_bytes = 0;
};

template<class Value>
Replay<Value> replay(const std::string& directory) {
    const auto descriptors = arena_descriptor_baseline();
    Store<Value> store;
    store.set_directory(directory);
    Reference<Value> reference;
    Sizing model;
    generated_keys([&](const Key& key, std::uint64_t ordinal) {
        const auto value = value_for<Value>(ordinal);
        require(put(store, key, value) == reference.insert_or_assign(key, value).second,
                "generated insertion agrees with map");
        model.append(key);
        require(store.disk_bytes() == model.logical(), "record and slot layout stays byte exact");
        check_storage(model);
    });
    check_descriptors(descriptors);
    check_rows(store, reference);
    store.set_directory(directory);
    expect_logic([&] { store.set_directory(directory + "/changed"); });
    const auto bytes = store.disk_bytes();
    generated_keys([&](const Key& key, std::uint64_t ordinal) {
        const auto value = static_cast<Value>(ordinal % 509);
        minimum(store, key, value);
        reference.at(key) = std::min(reference.at(key), value);
    });
    check_descriptors(descriptors);
    check_rows(store, reference);
    generated_keys([&](const Key& key, std::uint64_t ordinal) {
        if (ordinal % 3 != 0) return;
        require(erase(store, key) == (reference.erase(key) != 0), "erasures agree with map");
        require(!get(store, key) && !erase(store, key), "erased keys remain absent");
    });
    check_descriptors(descriptors);
    check_rows(store, reference);
    generated_keys([&](const Key& key, std::uint64_t ordinal) {
        const auto value = value_for<Value>(ordinal + model.used);
        require(put(store, key, value) == reference.insert_or_assign(key, value).second,
                "reinsertion and replacement agree with map");
    });
    require(store.disk_bytes() == bytes, "minima, erasures and recurrence reuse original records");
    check_descriptors(descriptors);
    check_rows(store, reference);
    check_storage(model);
    Store<Value> moved(std::move(store));
    require(store.empty() && store.disk_bytes() == 0, "move clears the source store");
    Store<Value> assigned;
    assigned = std::move(moved);
    require(moved.empty() && moved.disk_bytes() == 0, "move assignment clears the source store");
    Replay<Value> result;
    result.disk_bytes = assigned.disk_bytes();
    result.rows.reserve(assigned.size());
    for (const auto& row : assigned)
        result.rows.emplace_back(Key{row.first, row.from_entry}, row.second);
    assigned.clear();
    require(assigned.empty() && assigned.disk_bytes() == 0 && assigned.begin() == assigned.end(),
            "clear resets logical state");
    if (auto* arena = ScratchArena::attached())
        require(arena->stats().bytes_used == 0 && arena->stats().windows_mapped_bytes == 0,
                "clear releases both regions and their mappings");
    require(assigned.insert_or_assign("fresh", Value{17}), "clear preserves directory for next run");
    require(!assigned.lookup("exit-0"), "reused chunks disclose no previous keys");
    assigned.clear();
    return result;
}

void scratch_contract(const std::string& directory) {
    const auto descriptors = arena_descriptor_baseline();
    IdPairScratchFile file;
    require(!file.opened(), "scratch begins closed");
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    const std::uint64_t word = 0xfedcba9876543210ULL;
    std::uint64_t readback = 0;
    expect_id_io([&] { file.resize(maximum + 1); }, EFBIG);
    expect_id_io([&] { file.resize(0); }, EBADF);
    expect_id_io([&] { file.read(maximum + 1, nullptr, 0); }, EFBIG);
    expect_id_io([&] { file.write(maximum, &word, 1); }, EFBIG);
    expect_id_io([&] { file.read(0, &readback, sizeof(readback)); }, EBADF);
    expect_id_io([&] { file.write(0, &word, sizeof(word)); }, EBADF);
    file.read(maximum, nullptr, 0);
    file.write(maximum, nullptr, 0);
    file.open(directory);
    require(file.opened(), "open reports the selected backend");
    file.open(directory + "/ignored-while-open");
    const auto offset = kChunk - sizeof(word) / 2;
    file.resize(offset + sizeof(word) + 1);
    file.write(offset, &word, sizeof(word));
    std::array<unsigned char, sizeof(word)> zero{};
    std::array<unsigned char, sizeof(word)> observed{};
    file.read(0, observed.data(), observed.size());
    require(observed == zero, "sparse growth reads zero");
    file.read(offset, &readback, sizeof(readback));
    require(readback == word, "scratch record reads across the chunk boundary");
    file.read(maximum, nullptr, 0);
    file.write(maximum, nullptr, 0);
    expect_id_io([&] { file.read(offset + sizeof(word) + 1, &readback, 1); }, EIO);
    check_descriptors(descriptors);
    IdPairScratchFile moved(std::move(file));
    require(!file.opened() && moved.opened(), "scratch move transfers ownership");
    IdPairScratchFile assigned;
    assigned.open(directory);
    assigned.resize(sizeof(word));
    assigned = std::move(moved);
    assigned.read(offset, &readback, sizeof(readback));
    require(!moved.opened() && readback == word, "scratch assignment releases its old region");
    check_descriptors(descriptors);
    assigned.reset();
    require(!assigned.opened(), "reset releases scratch ownership");
    assigned.open(directory);
    assigned.resize(sizeof(word));
    assigned.read(0, observed.data(), observed.size());
    require(observed == zero, "reset and reuse do not expose old scratch bytes");
    assigned.reset();
}

void file_directory_contract(const std::string& directory) {
    Store<std::uint64_t> store;
    store.set_directory(directory + "/missing");
    expect_id_io([&] { store.insert_or_assign("directory", 23); }, ENOENT);
    require(store.empty() && store.disk_bytes() == 0,
            "file backend uses the supplied directory and never falls back to TMPDIR");
    store.set_directory(directory);
    require(store.insert_or_assign("directory", 23) && store.at("directory") == 23,
            "a directory can be rebound after a failed open");
    store.clear();
    store.set_directory(directory);
    require(store.insert_or_assign("another-run", 29), "directory binding survives a completed run");
}

void fixture_seam() {
    Store<std::int64_t> store;
    store.set_directory("/no/pf-dp2");
    store.fixture_use_memory_reference();
    store.insert_or_assign("cycle", "entry", -7);
    store.min_assign("minimum", 11);
    store.min_assign("minimum", 3);
    require(store.lookup("cycle", "entry") == -7 && store.at("minimum") == 3,
            "signed cycle and unsigned-shaped minimum operations retain the fixture seam");
    store.clear();
    require(store.insert_or_assign("second-run", -9) && store.disk_bytes() == 0,
            "clear preserves the explicit map backend");
    if (auto* arena = ScratchArena::attached())
        require(arena->stats().bytes_used == 0, "fixture map never claims an arena chunk");
}

#if defined(__linux__)
struct Backing {
    int fd = -1;
    std::uint64_t bytes;
    explicit Backing(std::uint64_t size) : bytes(size) {
        fd = ::memfd_create("pineforge-id-pair-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        require(fd >= 0, "parent creates memfd before the jail");
        require(::ftruncate(fd, static_cast<off_t>(bytes)) == 0, "parent pre-sizes workload capacity");
        require(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                "parent seals arena growth, shrink and seal changes");
    }
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;
    ~Backing() { if (fd >= 0) ::close(fd); }
};

std::size_t regular_descriptors() {
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "inspect open descriptors");
    std::size_t result = 0;
    bool complete = true;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) {
            complete = complete && errno == 0;
            break;
        }
        char* end = nullptr;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0
            || number > std::numeric_limits<int>::max()) continue;
        struct stat status{};
        int found;
        do { found = ::fstat(static_cast<int>(number), &status); } while (found != 0 && errno == EINTR);
        if (found != 0) complete = false;
        else if (S_ISREG(status.st_mode)) ++result;
    }
    require(::closedir(directory) == 0, "close descriptor census");
    require(complete, "descriptor census reads and stats every open descriptor");
    return result;
}

void jail_limits() {
    require(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore SIGXFSZ in jail child");
    struct rlimit file_limit{0, 0};
    require(::setrlimit(RLIMIT_FSIZE, &file_limit) == 0, "child applies RLIMIT_FSIZE=0");
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    require(statm != nullptr, "read existing address-space mappings");
    unsigned long long pages = 0;
    const int fields = std::fscanf(statm, "%llu", &pages);
    std::fclose(statm);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    require(fields == 1 && page_size > 0, "parse address-space baseline");
    const auto baseline = static_cast<std::uint64_t>(pages)
        * static_cast<std::uint64_t>(page_size);
    constexpr auto headroom = 128 * kChunk;
    require(baseline <= std::numeric_limits<rlim_t>::max() - headroom,
            "bounded address-space limit is representable");
    struct rlimit address_limit{};
    require(::getrlimit(RLIMIT_AS, &address_limit) == 0, "read address-space limit");
    address_limit.rlim_cur = std::min(address_limit.rlim_cur,
                                     static_cast<rlim_t>(baseline + headroom));
    require(::setrlimit(RLIMIT_AS, &address_limit) == 0, "child applies bounded RLIMIT_AS");
    const auto cap = std::to_string(kWindowBytes);
    require(::setenv("PF_SCRATCH_WINDOW_BYTES", cap.c_str(), 1) == 0,
            "bound the process-wide mapped scratch windows");
}

template<class Operation>
void jailed_child(Operation&& operation) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork jail proof child");
    if (pid == 0) {
        try {
            jail_limits();
            operation();
            ::_exit(0);
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "id-pair arena child: %s\n", failure.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "jailed arena replay completes without SIGXFSZ, SIGBUS or file IO failure");
}

template<class Value>
void arena_replay(const Backing& backing, const Replay<Value>& file_result) {
    ScratchArena::attach(backing.fd, backing.bytes);
    const auto descriptors = regular_descriptors();
    // A deliberately unusable directory also catches an accidental file-mode
    // fallback before the first write. Arena selection must not inspect it.
    const auto result = replay<Value>("/no/pf-dp2");
    require(result.rows == file_result.rows && result.disk_bytes == file_result.disk_bytes,
            "all records, their iteration order and byte counts equal the file-mode replay");
    require(regular_descriptors() == descriptors, "arena replay creates no regular-file descriptors");
    fixture_seam();
    require(regular_descriptors() == descriptors, "map fixture creates no regular-file descriptors");
    ScratchArena::detach();
}

void rehash_exhaustion(const Backing& backing) {
    ScratchArena::attach(backing.fd, backing.bytes);
    const auto descriptors = regular_descriptors();
    Store<std::uint64_t> store;
    store.set_directory("/no/pf-dp2");
    Reference<std::uint64_t> reference;
    Sizing model;
    bool refused = false;
    generated_keys([&](const Key& key, std::uint64_t ordinal) {
        if (refused) return;
        Sizing next = model;
        next.append(key);
        if (next.peak > backing.bytes) {
            require(next.slots == model.slots * 2 && model.used == model.slots / 2,
                    "one-chunk shortage is reached at the predicted index rebuild");
            expect_id_io([&] { put(store, key, value_for<std::uint64_t>(ordinal)); }, EFBIG);
            require(store.disk_bytes() == model.logical() && store.size() == reference.size(),
                    "refused replacement publishes no record or index");
            require(!get(store, key), "refused new key remains absent");
            check_storage(model);
            check_rows(store, reference);
            expect_id_io([&] { put(store, key, value_for<std::uint64_t>(ordinal)); }, EFBIG);
            check_storage(model);
            refused = true;
            return;
        }
        const auto value = value_for<std::uint64_t>(ordinal);
        require(put(store, key, value), "prefix insertion succeeds before predicted refusal");
        reference.emplace(key, value);
        model = next;
        check_storage(model);
    });
    require(refused, "arena smaller by one chunk refuses a concrete generated insertion");
    require(regular_descriptors() == descriptors, "exhaustion never opens a fallback file");
    store.clear();
    require(ScratchArena::attached()->stats().bytes_used == 0, "failed next index releases all chunks");
    require(store.insert_or_assign("recovered", 19), "clear permits reuse after a failed rebuild");
    require(store.lookup("recovered") == 19, "recovery reads back exactly");
    store.clear();
    ScratchArena::detach();
}

Key append_boundary_key() {
    const std::uint64_t first_bytes = kHeaderBytes + std::string("first").size();
    const auto length = kChunk - first_bytes - kHeaderBytes + 1;
    return {std::string(static_cast<std::size_t>(length), 'B'), std::nullopt};
}

Sizing append_boundary_size() {
    Sizing model;
    model.append(Key{"first", std::nullopt});
    model.append(append_boundary_key());
    return model;
}

void append_exhaustion(const Backing& backing) {
    ScratchArena::attach(backing.fd, backing.bytes);
    const auto descriptors = regular_descriptors();
    Store<std::int64_t> store;
    store.set_directory("/no/pf-dp2");
    require(store.insert_or_assign("first", -11), "initial record fits the smaller arena");
    const auto before = store.disk_bytes();
    const auto key = append_boundary_key();
    // The new header fits in the current data chunk; its following string
    // write needs the missing chunk. The unpublished header must stay hidden.
    expect_id_io([&] { put(store, key, std::int64_t{-13}); }, EFBIG);
    require(store.size() == 1 && store.disk_bytes() == before && !get(store, key),
            "partial append publishes no logical row");
    require(store.at("first") == -11, "append refusal preserves old record");
    std::size_t count = 0;
    for (const auto& row : store) {
        require(row.first == "first" && !row.from_entry && row.second == -11,
                "iteration cannot see an unpublished header");
        ++count;
    }
    require(count == 1, "append refusal preserves iteration count");
    require(!store.insert_or_assign("first", -17) && store.at("first") == -17,
            "in-place updates still work in a full arena");
    require(regular_descriptors() == descriptors, "failed append never opens a fallback file");
    store.clear();
    require(put(store, key, std::int64_t{-13}) && get(store, key) == -13,
            "released chunks can hold the same large key as a fresh run");
    store.clear();
    ScratchArena::detach();
}

std::size_t heap_peak = 0;
void record_peak(std::size_t) {
    heap_peak = std::max(heap_peak, global_allocation::live_bytes);
}

struct Recording {
    Recording() {
        require(!global_allocation::recording, "allocation recorder is not nested");
        global_allocation::live_bytes = 0;
        heap_peak = 0;
        global_allocation::on_recorded = record_peak;
        global_allocation::recording = true;
    }
    ~Recording() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

void arena_heap_bound(const Backing& backing) {
    ScratchArena::attach(backing.fd, backing.bytes);
    const auto descriptors = regular_descriptors();
    {
        Recording recording;
        {
            Store<std::uint64_t> store;
            store.set_directory("/no/pf-dp2");
            generated_keys([&](const Key& key, std::uint64_t ordinal) {
                require(put(store, key, ordinal), "heap witness inserts every generated key");
                require(get(store, key) == ordinal, "heap witness reads every generated key");
            });
            const auto bytes = store.disk_bytes();
            generated_keys([&](const Key& key, std::uint64_t ordinal) {
                if (ordinal % 3 == 0) require(erase(store, key), "heap witness erases old keys");
            });
            generated_keys([&](const Key& key, std::uint64_t ordinal) {
                put(store, key, ordinal + kDistinctIds);
                require(get(store, key) == ordinal + kDistinctIds,
                        "heap witness reads recurring keys");
            });
            require(store.disk_bytes() == bytes, "heap witness reuses all historical records");
            require(regular_descriptors() == descriptors, "live heap witness opens no regular file");
        }
        // 32 KiB covers the generated long strings and constant objects.
        // Region chunk ids are four bytes each, capacity <2x live chunks;
        // 16 bytes/chunk also covers old+new capacity during vector reserve.
        const auto budget = 32 * 1024ULL + 16 * (backing.bytes / kChunk);
        require(heap_peak <= budget, "component heap stays within fixed buffers plus chunk metadata");
        require(global_allocation::live_bytes == 0, "component retains no heap after destruction");
        std::printf("id-pair arena heap: peak=%zu budget=%llu bytes\n", heap_peak,
                    static_cast<unsigned long long>(budget));
        std::fflush(stdout);
    }
    require(ScratchArena::attached()->stats().bytes_used == 0, "heap witness releases every region");
    require(regular_descriptors() == descriptors, "heap witness creates no regular-file descriptor");
    ScratchArena::detach();
}
#endif
} // namespace

int main() {
    try {
        require(ScratchArena::attached() == nullptr, "test begins without an attached arena");
        ScratchDirectory scratch;
        scratch_contract(scratch.path);
        file_directory_contract(scratch.path);
        fixture_seam();
        const auto unsigned_file = replay<std::uint64_t>(scratch.path);
        const auto signed_file = replay<std::int64_t>(scratch.path);
#if defined(__linux__)
        const auto sizing = workload_size();
        require(sizing.peak > kChunk, "generated workload needs more than one chunk");
        Backing full(sizing.peak);
        Backing short_rehash(sizing.peak - kChunk);
        Backing short_append(append_boundary_size().peak - kChunk);
        jailed_child([&] {
            arena_replay(full, unsigned_file);
            arena_replay(full, signed_file);
            ScratchArena::attach(full.fd, full.bytes);
            const auto descriptors = regular_descriptors();
            scratch_contract("/no/pf-dp2");
            require(ScratchArena::attached()->stats().bytes_used == 0,
                    "scratch contract releases all regions");
            require(regular_descriptors() == descriptors, "scratch facade creates no regular file");
            ScratchArena::detach();
            rehash_exhaustion(short_rehash);
            append_exhaustion(short_append);
            arena_heap_bound(full);
        });
#else
        (void)unsigned_file;
        (void)signed_file;
        (void)workload_size();
        std::puts("test_id_pair_store_arena: SKIP sealed-memfd jail cases (Linux only)");
#endif
        std::puts("test_id_pair_store_arena: generated map and layout checks passed");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "test_id_pair_store_arena: %s\n", failure.what());
        return 1;
    }
}
