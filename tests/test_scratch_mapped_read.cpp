#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "p2cx_tledger_fixture.hpp"

#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <linux/memfd.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mapped_io {
std::uint64_t maps = 0, unmaps = 0, reads = 0, discards = 0;
std::uint64_t live = 0, peak = 0;
std::size_t page_bytes = 4096;
bool balanced = true;
int shrink_after_stat = -1;
int shrink_result = -1;

std::uint64_t pages(std::size_t length) {
    return ((length + page_bytes - 1) / page_bytes) * page_bytes;
}
void reset() {
    maps = unmaps = reads = discards = 0;
    peak = live;
}
}

extern "C" void* mmap(void* address, std::size_t length, int protection,
                      int flags, int fd, off_t offset) noexcept {
    ++mapped_io::maps;
    void* result = reinterpret_cast<void*>(::syscall(SYS_mmap, address, length,
                                                     protection, flags, fd, offset));
    if (result != MAP_FAILED) {
        mapped_io::live += mapped_io::pages(length);
        mapped_io::peak = std::max(mapped_io::peak, mapped_io::live);
    }
    return result;
}
extern "C" int munmap(void* address, std::size_t length) noexcept {
    ++mapped_io::unmaps;
    const int result = static_cast<int>(::syscall(SYS_munmap, address, length));
    if (result == 0) {
        const auto bytes = mapped_io::pages(length);
        if (bytes > mapped_io::live) mapped_io::balanced = false;
        else mapped_io::live -= bytes;
    }
    return result;
}
extern "C" int fstat(int fd, struct stat* status) noexcept {
    const int result = static_cast<int>(::syscall(SYS_fstat, fd, status));
    // Return a genuine successful size snapshot, then shrink the same backing
    // before the caller can dereference its live mapping. No fabricated stat.
    if (result == 0 && mapped_io::shrink_after_stat >= 0) {
        const int target = std::exchange(mapped_io::shrink_after_stat, -1);
        mapped_io::shrink_result = ::ftruncate(target, 0);
    }
    return result;
}
extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    ++mapped_io::reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}
extern "C" int posix_fadvise(int fd, off_t offset, off_t length, int advice) noexcept {
    if (advice == POSIX_FADV_DONTNEED) ++mapped_io::discards;
    const int saved_errno = errno;
    const auto result = ::syscall(SYS_fadvise64, fd, offset, length, advice);
    const int error = result < 0 ? errno : 0;
    errno = saved_errno;
    return error;
}

namespace {
using namespace p2cx_tledger;
using detail::ScratchArena;
using detail::ScratchFile;
constexpr std::size_t kWindow = ScratchFile::kReadWindowBytes;

template<class Operation>
void typed_io(Operation operation, const char* reason = "scratch_io") {
    bool caught = false;
    try { operation(); }
    catch (const detail::ReportScratchIoError& error) {
        caught = true;
        CHECK(error.code().value() == EIO);
        const auto failure = classify_run_failure(error);
        CHECK(failure.code == RunFailureCode::report_sink_failed);
        REQUIRE(failure.args);
        CHECK(*failure.args == std::string("{\"reason\":\"") + reason + "\"}");
    }
    REQUIRE(caught);
}

struct Backing {
    int fd = -1;
    explicit Backing(std::size_t length, bool sealed = false) {
        if (sealed) {
            fd = static_cast<int>(::syscall(SYS_memfd_create, "mapped-scratch",
                                            MFD_CLOEXEC | MFD_ALLOW_SEALING));
            REQUIRE(fd >= 0);
        } else {
            char name[] = "./mapped-scratch-XXXXXX";
            fd = ::mkstemp(name);
            REQUIRE(fd >= 0);
            REQUIRE(::unlink(name) == 0);
        }
        REQUIRE(::ftruncate(fd, static_cast<off_t>(length)) == 0);
        if (sealed)
            REQUIRE(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
    }
    ~Backing() { if (fd >= 0) ::close(fd); }
};

struct ArenaScope {
    bool had_cap = false;
    std::string old_cap;
    ArenaScope(int fd, std::size_t length, std::size_t cap = kWindow)
        : had_cap(std::getenv("PF_SCRATCH_WINDOW_BYTES") != nullptr),
          old_cap(had_cap ? std::getenv("PF_SCRATCH_WINDOW_BYTES") : "") {
        REQUIRE(::setenv("PF_SCRATCH_WINDOW_BYTES", std::to_string(cap).c_str(), 1) == 0);
        ScratchArena::attach(fd, length);
    }
    ~ArenaScope() {
        ScratchArena::detach();
        if (had_cap) ::setenv("PF_SCRATCH_WINDOW_BYTES", old_cap.c_str(), 1);
        else ::unsetenv("PF_SCRATCH_WINDOW_BYTES");
    }
};

std::vector<unsigned char> bytes(std::size_t size) {
    std::vector<unsigned char> result(size);
    for (std::size_t i = 0; i < size; ++i)
        result[i] = static_cast<unsigned char>((i * 17 + i / 251) & 255);
    return result;
}

void disk_bounds_and_mutations() {
    ScratchFile file;
    file.set_directory(".");
    file.bound_cache();
    file.map_index_reads();
    file.open("mapped-scratch-values");
    auto expected = bytes(3 * kWindow + 37);
    file.write(0, expected.data(), expected.size());
    std::vector<unsigned char> actual(expected.size());
    mapped_io::reset();
    file.read_mapped(0, actual.data(), actual.size());
    CHECK(actual == expected);
    CHECK(mapped_io::reads == 0);
    CHECK(mapped_io::maps == 4);
    CHECK(mapped_io::unmaps == 3);
    CHECK(mapped_io::peak <= kWindow);
    file.invalidate_mapped_reads();
    CHECK(mapped_io::live == 0);

    // Full descriptors can cross a window; scalar fields need no row copy.
    std::array<unsigned char, TradeLedger::kRecordBytes> crossing{};
    const auto start = kWindow - 81;
    file.read_mapped(start, crossing.data(), crossing.size());
    CHECK(std::equal(crossing.begin(), crossing.end(), expected.begin() + start));
    const unsigned char replacement = 197;
    file.write(start, &replacement, 1);
    CHECK(mapped_io::live == 0);
    expected[start] = replacement;
    file.read_mapped(start, crossing.data(), crossing.size());
    CHECK(std::equal(crossing.begin(), crossing.end(), expected.begin() + start));

    ScratchFile moved(std::move(file));
    CHECK(mapped_io::live == 0);
    CHECK(!file.opened());
    moved.read_mapped(start, crossing.data(), crossing.size());
    CHECK(std::equal(crossing.begin(), crossing.end(), expected.begin() + start));
    ScratchFile assigned;
    assigned = std::move(moved);
    CHECK(mapped_io::live == 0);
    assigned.read_mapped(start, crossing.data(), crossing.size());
    CHECK(std::equal(crossing.begin(), crossing.end(), expected.begin() + start));
    assigned.truncate(17);
    CHECK(mapped_io::live == 0);
    typed_io([&] { assigned.read_mapped(start, crossing.data(), crossing.size()); });
    assigned.ensure_size(64);
    std::array<unsigned char, 47> zeros{};
    std::array<unsigned char, 47> grown{};
    assigned.read_mapped(17, grown.data(), grown.size());
    CHECK(grown == zeros);
    assigned.clear();
    CHECK(mapped_io::live == 0);
    assigned.open("mapped-scratch-reopened");
    typed_io([&] { assigned.read_mapped(0, grown.data(), grown.size()); });
}

void live_disk_shrink_and_race() {
    ScratchFile file;
    file.set_directory(".");
    file.map_index_reads();
    file.open("mapped-scratch-shrink");
    const std::uint64_t expected = 0x123456780abcdef0ULL;
    const auto offset = 2 * mapped_io::page_bytes;
    std::uint64_t actual = 0;
    file.write(offset, &expected, sizeof expected);
    file.read_mapped(offset, &actual, sizeof actual);
    CHECK(actual == expected);
    // The field exists, but a truncated descriptor is still an I/O error.
    typed_io([&] {
        file.read_mapped(offset, &actual, sizeof actual, TradeLedger::kRecordBytes);
    });
    file.read_mapped(offset, &actual, sizeof actual);
    REQUIRE(actual == expected);
    REQUIRE(mapped_io::live != 0);
    REQUIRE(::ftruncate(file.descriptor(), 0) == 0);
    typed_io([&] { file.read_mapped(offset, &actual, sizeof actual); });
    CHECK(mapped_io::live == 0);
    for (int repeat = 0; repeat < 2; ++repeat) {
        file.write(offset, &expected, sizeof expected);
        file.read_mapped(offset, &actual, sizeof actual);
        REQUIRE(actual == expected);
        REQUIRE(mapped_io::live != 0);
        mapped_io::shrink_result = -1;
        mapped_io::shrink_after_stat = file.descriptor();
        typed_io([&] { file.read_mapped(offset, &actual, sizeof actual); });
        REQUIRE(mapped_io::shrink_after_stat == -1);
        REQUIRE(mapped_io::shrink_result == 0);
        CHECK(mapped_io::live == 0);
    }
    // Two faults also detect a SIGBUS mask left blocked by recovery.
    file.write(offset, &expected, sizeof expected);
    file.read_mapped(offset, &actual, sizeof actual);
    CHECK(actual == expected);
}

void discard_contract_and_forwarding_control() {
    ScratchFile file;
    file.set_directory(".");
    file.bound_cache();
    file.map_index_reads();
    file.open("mapped-scratch-discard");
    const auto expected = bytes(ScratchFile::kCacheBytes);
    std::vector<unsigned char> actual(expected.size());
    file.write(0, expected.data(), expected.size());
    // Prove this wrapper observes and forwards actual advice before testing
    // the policy, including a build in which the owner flips the switch.
    mapped_io::reset();
    REQUIRE(::posix_fadvise(file.descriptor(), 0, 0, POSIX_FADV_DONTNEED) == 0);
    CHECK(mapped_io::discards == 1);
    mapped_io::reset();
    file.read_mapped(0, actual.data(), actual.size());
    CHECK(actual == expected);
    CHECK(mapped_io::reads == 0);
    CHECK(mapped_io::discards == (ScratchFile::kKeepMappedIndexDontneed ? 1 : 0));
    // Ordinary read() still reaches the pread interposer.
    mapped_io::reset();
    file.read(0, actual.data(), 19);
    CHECK(mapped_io::reads == 1);
    CHECK(std::equal(actual.begin(), actual.begin() + 19, expected.begin()));
}

void arena_bounds_values_and_shrink() {
    constexpr auto backing_bytes = 8 * kWindow;
    Backing backing(backing_bytes);
    ArenaScope arena(backing.fd, backing_bytes);
    ScratchFile file;
    file.map_index_reads();
    file.open("mapped-arena-values");
    const auto expected = bytes(3 * kWindow + 37);
    file.write(0, expected.data(), expected.size());
    std::vector<unsigned char> actual(expected.size());
    mapped_io::reset();
    file.read_mapped(0, actual.data(), actual.size());
    CHECK(actual == expected);
    CHECK(mapped_io::reads == 0);
    CHECK(mapped_io::maps == 4);
    CHECK(mapped_io::peak <= kWindow);
    CHECK(ScratchArena::attached()->stats().windows_mapped_bytes <= kWindow);
    file.invalidate_mapped_reads();
    CHECK(ScratchArena::attached()->stats().windows_mapped_bytes == 0);
    const std::uint64_t word = 0xabcdeffedcba0123ULL;
    std::uint64_t result = 0;
    file.write(0, &word, sizeof word);
    file.read_mapped(0, &result, sizeof result);
    CHECK(result == word);
    REQUIRE(ScratchArena::attached()->stats().windows_mapped_bytes != 0);
    REQUIRE(::ftruncate(backing.fd, 0) == 0);
    typed_io([&] { file.read_mapped(0, &result, sizeof result); }, "arena_io");
    CHECK(ScratchArena::attached()->stats().windows_mapped_bytes == 0);
    REQUIRE(::ftruncate(backing.fd, backing_bytes) == 0);
    file.write(0, &word, sizeof word);
    file.read_mapped(0, &result, sizeof result);
    REQUIRE(result == word);
    mapped_io::shrink_after_stat = backing.fd;
    mapped_io::shrink_result = -1;
    typed_io([&] { file.read_mapped(0, &result, sizeof result); }, "arena_io");
    REQUIRE(mapped_io::shrink_after_stat == -1);
    REQUIRE(mapped_io::shrink_result == 0);
    CHECK(ScratchArena::attached()->stats().windows_mapped_bytes == 0);
    REQUIRE(::ftruncate(backing.fd, backing_bytes) == 0);
    file.write(0, &word, sizeof word);
    file.read_mapped(0, &result, sizeof result);
    CHECK(result == word);
    file.truncate(0);
    typed_io([&] { file.read_mapped(0, &result, sizeof result); }, "arena_io");
    file.write(0, &word, sizeof word);
    typed_io([&] {
        file.read_mapped(0, &result, sizeof result, TradeLedger::kRecordBytes);
    }, "arena_io");
}

void slab_checked_copies() {
    detail::ScratchManifest manifest;
    manifest.U = 4;
    manifest.B_U = 64;
    auto slab = std::make_shared<detail::ScratchSlab>(manifest);
    ScratchFile file;
    file.map_index_reads();
    file.bind_scratch(slab, detail::ScratchResource::cohort);
    const auto expected = bytes(73);
    std::vector<unsigned char> actual(expected.size());
    file.write(0, expected.data(), expected.size());
    mapped_io::reset();
    file.read_mapped(0, actual.data(), actual.size());
    CHECK(actual == expected);
    CHECK(mapped_io::reads == 0 && mapped_io::maps == 0 && mapped_io::unmaps == 0);
    bool caught = false;
    try { file.read_mapped(1, actual.data(), actual.size()); }
    catch (const detail::AuxiliaryScratchIoError& error) {
        caught = true;
        CHECK(error.code().value() == EIO);
        CHECK(classify_run_failure(error).code == RunFailureCode::scratch_io_failed);
    }
    REQUIRE(caught);
}

void sealed_arena_windows() {
    constexpr auto backing_bytes = 5 * kWindow;
    Backing backing(backing_bytes, true);
    const auto expected = bytes(2 * kWindow + 55);
    std::vector<unsigned char> actual(expected.size());
    for (const auto cap : {kWindow, mapped_io::page_bytes}) {
        ArenaScope arena(backing.fd, backing_bytes, cap);
        ScratchFile file;
        file.map_index_reads();
        file.open("mapped-sealed-values");
        file.write(0, expected.data(), expected.size());
        mapped_io::reset();
        file.read_mapped(0, actual.data(), actual.size());
        CHECK(actual == expected);
        CHECK(mapped_io::reads == 0);
        CHECK(mapped_io::peak <= cap);
        const auto stats = ScratchArena::attached()->stats();
        CHECK(stats.backend == detail::ScratchArenaBackend::sealed_memfd);
        CHECK(stats.windows_mapped_bytes <= cap);
        REQUIRE(::ftruncate(backing.fd, 0) == -1);
        CHECK(errno == EPERM);
    }
}

void ledger_mutations_and_all_fields() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    for (int i = 0; i < 13000; ++i) {
        expected.push_back(row(i));
        ledger.push_back(expected.back());
    }
    const auto fixed_bytes = ledger.cache_resident_bytes();
    for (std::size_t i = 0; i < expected.size(); ++i)
        same_numeric_fields(ledger, i, expected[i]);
    CHECK(ledger.cache_size() == 0);
    CHECK(ledger.cache_resident_bytes() == fixed_bytes);
    for (const std::size_t i : {std::size_t{0}, std::size_t{6241}, std::size_t{12500}}) {
        expected[i].pnl = -123.125 - static_cast<double>(i);
        expected[i].entry_comment.assign("replaced\0comment", 16);
        ledger.replace(i, expected[i]);
        same_numeric_fields(ledger, i, expected[i]);
        same_trade(ledger.read(i), expected[i]);
    }
    ledger.resize(100);
    expected.resize(100);
    for (int i = 100; i < 500; ++i) {
        expected.push_back(row(i + 17));
        ledger.push_back(expected.back());
    }
    same_numeric_fields(ledger, 5, expected[5]);
    ledger.seal(7);
    same_trade(ledger.read(5), expected[5]);
    bool refused = false;
    try { (void)ledger.pnl_at(5); }
    catch (const coded<std::logic_error>& error) {
        refused = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
    }
    REQUIRE(refused);
    TradeLedger moved(std::move(ledger));
    for (std::size_t i = moved.base(); i < expected.size(); ++i) {
        same_numeric_fields(moved, i, expected[i]);
        same_trade(moved.read(i), expected[i]);
    }
    moved.clear();
    moved.push_back(expected[17]);
    same_numeric_fields(moved, 0, expected[17]);
    same_trade(moved.read(0), expected[17]);
}
} // namespace

int main() {
    const long page = ::sysconf(_SC_PAGESIZE);
    REQUIRE(page > 0);
    mapped_io::page_bytes = static_cast<std::size_t>(page);
    test("bounded disk windows, crossing reads and mutation invalidation", disk_bounds_and_mutations);
    test("live disk truncation and two fstat-to-load races", live_disk_shrink_and_race);
    test("whole-file discard policy and forwarding controls", discard_contract_and_forwarding_control);
    test("arena shared accounting, values and concurrent shrink", arena_bounds_values_and_shrink);
    test("slab keeps checked copies without new mappings", slab_checked_copies);
    test("sealed arena reuses its one-page and one-MiB budgets", sealed_arena_windows);
    test("ledger fields, replace, rewind, publish, move and clear", ledger_mutations_and_all_fields);
    CHECK(mapped_io::balanced);
    CHECK(mapped_io::live == 0);
    std::printf("test_scratch_mapped_read: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
