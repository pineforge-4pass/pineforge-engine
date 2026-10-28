// Expected wall time by hand: <5 s per row in Release, <30 s instrumented.
// Before the slab became an anonymous mapping:
// --rss grows by ~964.516 MiB (1,011,367,936 bytes), failing the 64 MiB bound.
// Example before/after RSS: eager ~8/~972.516 MiB; lazy ~8/~8 MiB.
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#endif

namespace {
namespace detail = pineforge::detail;
using Word = std::uint64_t;
using Sample = std::array<std::uint8_t, 8>;
constexpr Word kMiB = 1024 * 1024;
constexpr Word kCapSlabBytes = 1011367936;
constexpr Word kMaxRssGrowth = 64 * kMiB; // Eager head touches ~964.5 MiB.
constexpr Sample kFirst{{0x01, 0xff, 0x80, 0x00, 0x7f, 0x31, 0x55, 0xab}};
constexpr Sample kLast{{0xfe, 0x00, 0x13, 0x80, 0x29, 0x44, 0x99, 0x71}};
constexpr Sample kZero{};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

detail::ScratchManifest cap_manifest() {
    const auto manifest = detail::default_scratch_manifest(1048576);
    require(manifest.U == 1048576 && manifest.K_e == 4194304
                && manifest.K_c == 524288 && manifest.R == 2097152,
            "test must construct the actual 1,048,576-row cap manifest");
    return manifest;
}

void check_cap(const detail::ScratchSlab& slab) {
    // 544 + 128 + 84.5 + 16 + 160 + 32 MiB, plus 16 KiB page ceilings.
    require(slab.layout().slab_size == kCapSlabBytes, "cap slab exact layout size");
    for (const auto& region : slab.regions())
        require(reinterpret_cast<std::uintptr_t>(region.base) % detail::kScratchPageSize == 0,
                "every region base retains 4096-byte alignment");
}

void expect_bytes(const detail::ScratchSlab& slab, const detail::Region& region,
                  Word offset, const Sample& expected) {
    Sample observed{};
    slab.read_at(region, offset, observed.data(), observed.size());
    require(observed == expected, "slab byte pattern differs");
}

void fresh_regions_are_zero(detail::ScratchSlab& slab) {
    // Inspect EVERY admitted byte in ALL regions, including their last bytes.
    // This is a correctness scan, deliberately separate from the RSS row.
    std::array<std::uint8_t, 64 * 1024> bytes{};
    for (const auto& region : slab.regions()) {
        require(region.logical_extent == 0, "fresh logical extents are empty");
        slab.extend(region.resource, region.capacity);
        if (region.capacity == 0)
            slab.read_at(region, 0, nullptr, 0); // Reserved q regions are empty.
        for (Word offset = 0; offset < region.capacity;) {
            const auto count = static_cast<std::size_t>(
                std::min<Word>(bytes.size(), region.capacity - offset));
            bytes.fill(0xa5);
            slab.read_at(region, offset, bytes.data(), count);
            require(std::all_of(bytes.begin(), bytes.begin() + count,
                                [](std::uint8_t value) { return value == 0; }),
                    "fresh region contains a nonzero byte");
            offset += count;
        }
    }
}

void zero_and_reset() {
    detail::ScratchSlab slab(cap_manifest());
    check_cap(slab);
    const auto layout = slab.layout();
    std::array<std::uint8_t*, detail::kScratchRegionCount> bases{};
    for (std::size_t i = 0; i < bases.size(); ++i) bases[i] = slab.regions()[i].base;
    fresh_regions_are_zero(slab);

    // q is reserved/empty: erased_data is the FIRST nonempty region.
    const auto& first = slab.region(detail::ScratchResource::erased_data);
    const auto& last = slab.region(detail::ScratchResource::successor_roots);
    require(&last == &slab.regions().back(), "root store is the last region");
    const Word last_at = last.capacity - kLast.size();
    slab.write_at(first, 0, kFirst.data(), kFirst.size());
    slab.write_at(last.resource, last_at, kLast.data(), kLast.size());
    expect_bytes(slab, first, 0, kFirst);
    expect_bytes(slab, last, last_at, kLast);
    slab.zero();
    expect_bytes(slab, first, 0, kZero);
    expect_bytes(slab, last, last_at, kZero);
    for (const auto& region : slab.regions())
        require(region.logical_extent == region.capacity, "zero preserves readable extents");
    slab.zero(); // Clearing an already clean slab remains correct.

    slab.write_at(first, 0, kFirst.data(), kFirst.size());
    slab.write_at(last, last_at, kLast.data(), kLast.size());
    slab.reset();
    for (std::size_t i = 0; i < bases.size(); ++i) {
        const auto& region = slab.regions()[i];
        const auto& before = layout.regions[i];
        const auto& after = slab.layout().regions[i];
        require(region.base == bases[i] && region.capacity == before.capacity
                    && after.resource == before.resource && after.offset == before.offset
                    && after.capacity == before.capacity && after.span == before.span
                    && slab.layout().slab_size == layout.slab_size,
                "reset preserves bases, region directory and layout");
        require(region.logical_extent == 0, "reset clears every logical extent");
        slab.extend(region.resource, region.capacity);
    }
    expect_bytes(slab, first, 0, kZero);
    expect_bytes(slab, last, last_at, kZero);

    const auto& far = slab.region(detail::ScratchResource::cohort);
    // Cross a page boundary ~8 MiB into the region, then write an earlier
    // offset so a last-write-length tracker cannot masquerade as high water.
    constexpr Word far_at = 8 * kMiB - 2;
    slab.write_at(far, far_at, kFirst.data(), kFirst.size());
    slab.write_at(far, 17, kLast.data(), kLast.size());
    expect_bytes(slab, far, far_at, kFirst);
    slab.zero();
    expect_bytes(slab, far, far_at, kZero);
    expect_bytes(slab, far, 17, kZero);
    expect_bytes(slab, far, far_at - kZero.size(), kZero); // Unwritten hole.

    // Store-local clearing must clear its high water without touching peers.
    slab.write_at(first, 0, kFirst.data(), kFirst.size());
    slab.write_at(last, last_at, kLast.data(), kLast.size());
    slab.write_at(far, far_at, kFirst.data(), kFirst.size());
    slab.zero(far.resource);
    require(far.logical_extent == far.capacity, "local zero preserves extent");
    expect_bytes(slab, far, far_at, kZero);
    expect_bytes(slab, first, 0, kFirst);
    expect_bytes(slab, last, last_at, kLast);
    slab.write_at(far, far_at, kLast.data(), kLast.size());
    slab.reset(far.resource);
    require(far.logical_extent == 0, "local reset clears extent");
    slab.extend(far.resource, far_at + kLast.size());
    expect_bytes(slab, far, far_at, kZero);
    expect_bytes(slab, first, 0, kFirst);
    expect_bytes(slab, last, last_at, kLast);
}

void slabs_do_not_alias() {
    detail::ScratchSlab first(cap_manifest());
    check_cap(first);
    const auto resource = detail::ScratchResource::erased_data;
    const auto& region = first.region(resource);
    first.write_at(resource, 4093, kFirst.data(), kFirst.size());
    {
        detail::ScratchSlab second(cap_manifest());
        check_cap(second);
        require(region.base != second.region(resource).base, "two live mappings have distinct bases");
        second.extend(resource, 4093 + kLast.size());
        expect_bytes(second, second.region(resource), 4093, kZero);
        second.write_at(resource, 4093, kLast.data(), kLast.size());
        expect_bytes(first, region, 4093, kFirst);
        expect_bytes(second, second.region(resource), 4093, kLast);
        second.zero();
        expect_bytes(first, region, 4093, kFirst);
        expect_bytes(second, second.region(resource), 4093, kZero);
        second.write_at(resource, 4093, kLast.data(), kLast.size());
        first.reset();
        expect_bytes(second, second.region(resource), 4093, kLast);
        first.extend(resource, 4093 + kFirst.size());
        expect_bytes(first, region, 4093, kZero);
        first.write_at(resource, 4093, kFirst.data(), kFirst.size());
    }
    expect_bytes(first, region, 4093, kFirst); // Destroying a peer must not unmap this slab.
}

#if defined(__linux__) || defined(__APPLE__)
Word resident_bytes() {
#if defined(__linux__)
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    require(file != nullptr, "open /proc/self/statm for RSS");
    unsigned long long virtual_pages = 0, resident_pages = 0;
    const int parsed = std::fscanf(file, "%llu %llu", &virtual_pages, &resident_pages);
    std::fclose(file);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    require(parsed == 2 && page_size > 0, "parse resident field and real OS page size");
    const auto page = static_cast<Word>(page_size);
    require(resident_pages <= std::numeric_limits<Word>::max() / page, "RSS bytes fit uint64");
    return static_cast<Word>(resident_pages) * page;
#else
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const auto status = ::task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                    reinterpret_cast<task_info_t>(&info), &count);
    require(status == KERN_SUCCESS && count == MACH_TASK_BASIC_INFO_COUNT,
            "task_info must provide current resident size");
    return static_cast<Word>(info.resident_size);
#endif
}
#endif

int rss_row() {
#if defined(__linux__) || defined(__APPLE__)
    const auto manifest = cap_manifest();
    (void)resident_bytes(); // Warm the measurement API before its baseline.
    const Word before = resident_bytes();
    detail::ScratchSlab slab(manifest); // Actual cap slab, alive through both measurements.
    check_cap(slab);
    const Word after = resident_bytes();
    // Publishing an untouched readable prefix must not make zero/reset eager.
    for (const auto& region : slab.regions()) slab.extend(region.resource, region.capacity);
    slab.zero();
    slab.reset();
    const Word after_reset = resident_bytes();
    const Word growth = after > before ? after - before : 0;
    const Word reset_growth = after_reset > before ? after_reset - before : 0;
    std::printf("cap slab RSS bytes: before=%llu after=%llu after_reset=%llu "
                "growth=%llu reset_growth=%llu bound=%llu slab=%llu\n",
                static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
                static_cast<unsigned long long>(after_reset), static_cast<unsigned long long>(growth),
                static_cast<unsigned long long>(reset_growth),
                static_cast<unsigned long long>(kMaxRssGrowth),
                static_cast<unsigned long long>(slab.layout().slab_size));
    require(growth < kMaxRssGrowth, "cap slab construction RSS growth must be below 64 MiB");
    require(reset_growth < kMaxRssGrowth, "untouched zero/reset RSS growth must be below 64 MiB");
    // Keep cleared payload observable rather than relying on an otherwise
    // unused allocation. This full read happens AFTER all RSS samples/checks.
    fresh_regions_are_zero(slab);
    return 0;
#else
    std::puts("SKIP lazy slab RSS: resident-size measurement is implemented only on Linux and macOS");
    return 77;
#endif
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--rss") == 0) return rss_row();
        require(argc == 1, "usage: test_cr_lazy_slab [--rss]");
        zero_and_reset();
        slabs_do_not_alias();
        std::puts("lazy scratch slab: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "lazy scratch slab: %s\n", error.what());
        return 1;
    }
}
