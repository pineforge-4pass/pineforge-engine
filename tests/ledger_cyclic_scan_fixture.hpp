#pragma once

// This witness uses only the pre-existing public ledger API so it also builds
// against the previous implementation. Its first scan bound rejects preads.
#include "p2cx_tledger_fixture.hpp"

#include <pineforge/detail/scratch_file.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace cyclic_io {
struct Counts {
    std::uint64_t reads = 0, maps = 0, unmaps = 0, bad_maps = 0;
};
bool measuring = false;
Counts counts;

struct Measure {
    Measure() {
        REQUIRE(!measuring);
        counts = {};
        measuring = true;
    }
    ~Measure() { measuring = false; }
    Counts finish() {
        measuring = false;
        return counts;
    }
};
} // namespace cyclic_io

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    if (cyclic_io::measuring) ++cyclic_io::counts.reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}

extern "C" void* mmap(void* address, std::size_t length, int protection,
                      int flags, int fd, off_t offset) noexcept {
    if (cyclic_io::measuring) {
        ++cyclic_io::counts.maps;
        if (protection != PROT_READ || !(flags & MAP_SHARED) || (flags & MAP_PRIVATE))
            ++cyclic_io::counts.bad_maps;
    }
    return reinterpret_cast<void*>(::syscall(SYS_mmap, address, length, protection,
                                              flags, fd, offset));
}

extern "C" int munmap(void* address, std::size_t length) noexcept {
    if (cyclic_io::measuring) ++cyclic_io::counts.unmaps;
    return static_cast<int>(::syscall(SYS_munmap, address, length));
}

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
// A test contract, deliberately independent of any new implementation API.
constexpr std::uint64_t kWindowBytes = 1024 * 1024;
constexpr int kPasses = 8;

void forwarding_controls() {
    detail::ScratchFile file;
    file.set_directory(".");
    file.open("ledger-cyclic-counter");
    const char expected[] = "actual positional and mapped bytes";
    char actual[sizeof expected]{};
    file.write(0, expected, sizeof expected);
    {
        cyclic_io::Measure measure;
        file.read(0, actual, sizeof actual);
        const auto io = measure.finish();
        CHECK(io.reads == 1);
        CHECK(std::memcmp(actual, expected, sizeof expected) == 0);
    }
    {
        cyclic_io::Measure measure;
        void* view = ::mmap(nullptr, sizeof expected, PROT_READ, MAP_SHARED,
                             file.descriptor(), 0);
        REQUIRE(view != MAP_FAILED);
        CHECK(std::memcmp(view, expected, sizeof expected) == 0);
        CHECK(::munmap(view, sizeof expected) == 0);
        const auto io = measure.finish();
        CHECK(io.maps == 1);
        CHECK(io.unmaps == 1);
        CHECK(io.bad_maps == 0);
    }
}

void cyclic_scan(Size population) {
    REQUIRE(population > TradeLedger::kDefaultCacheRows);
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<double> expected;
    expected.reserve(population);
    std::uint64_t generator = 0x726561646f6e6c79ULL;
    for (Size i = 0; i < population; ++i) {
        generator ^= generator << 13;
        generator ^= generator >> 7;
        generator ^= generator << 17;
        // Generate and retain the oracle before handing the row to storage.
        // Values vary independently of their keys, including signs and zeros.
        Trade value{};
        value.pnl = static_cast<double>(static_cast<std::int64_t>(
            generator % 2000001) - 1000000) / 8.0;
        value.entry_time = T + static_cast<std::int64_t>(i) * 60000;
        value.exit_time = value.entry_time + 60000;
        value.entry_id = "cyclic-entry";
        expected.push_back(value.pnl);
        ledger.push_back(value);
    }
    REQUIRE(ledger.size() == population);
    REQUIRE(ledger.resident_base() == population - TradeLedger::kResidentRows);
    const auto windows = (population * TradeLedger::kRecordBytes + kWindowBytes - 1)
        / kWindowBytes;
    // Each replacement is two syscalls: evict, then map. Include the live
    // window carried over from the preceding pass in the unmap allowance.
    const auto map_unmap_limit = 2 * windows + 2;
    for (int pass = 0; pass < kPasses; ++pass) {
        cyclic_io::Measure measure;
        for (Size i = 0; i < population; ++i)
            CHECK(same_bits(ledger.pnl_at(i), expected[i]));
        const auto io = measure.finish();
        std::printf("cyclic rows=%zu pass=%d preads=%llu maps=%llu unmaps=%llu "
                    "map_unmap_limit=%llu\n", population, pass,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.maps),
                    static_cast<unsigned long long>(io.unmaps),
                    static_cast<unsigned long long>(map_unmap_limit));
        // Check this first: the previous tree fails for actual descriptor /
        // payload preads, after the forwarding controls have succeeded.
        CHECK(io.reads == 0);
        CHECK(io.maps > 0);
        CHECK(io.maps <= windows);
        CHECK(io.unmaps <= windows + 1);
        CHECK(io.maps + io.unmaps <= map_unmap_limit);
        CHECK(io.bad_maps == 0);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
    }
}
} // namespace
