// CR-150 RAM foundation. Remote fail-before: the frozen closure has no
// pineforge/detail/scratch_slab.hpp. Record the actual first diagnostic there;
// no fail-before or green execution was performed by this edit-only lane.
#include "global_allocation_replacement.hpp"
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using detail::ScratchManifest;
using detail::ScratchResource;
using detail::ScratchSlab;
using Nine = std::array<std::uint64_t, 9>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Operation>
void capacity_failure(const char* resource, Operation operation) {
    bool caught = false;
    try {
        operation();
    } catch (const detail::ScratchCapacityError& error) {
        caught = true;
        const auto value = classify_run_failure(error);
        require(value.code == RunFailureCode::scratch_capacity_exceeded, "typed capacity code");
        require(run_failure_code_class(value.code) == RunFailureClass::resource, "resource class");
        require(!run_failure_code_retryable(value.code), "capacity is not retryable");
        require(value.args && *value.args == std::string("{\"resource\":\"") + resource + "\"}",
                "capacity args are the single closed resource, with no computed count/id");
    }
    require(caught, "expected a capacity refusal");
}

template<class Operation>
void io_failure(Operation operation) {
    bool caught = false;
    try {
        operation();
    } catch (const detail::ScratchIoError& error) {
        caught = true;
        const auto value = classify_run_failure(error);
        require(value.code == RunFailureCode::scratch_io_failed, "typed scratch I/O code");
        require(run_failure_code_class(value.code) == RunFailureClass::resource, "I/O resource class");
        require(!run_failure_code_retryable(value.code), "scratch I/O is not retryable");
        require(!value.args || *value.args == "{}", "scratch I/O has no computed arguments");
    }
    require(caught, "expected a scratch I/O refusal");
}

void layout_case(const ScratchManifest& manifest, const Nine& capacities,
                 const Nine& spans, const Nine& offsets, std::uint64_t total) {
    const auto layout = detail::compute_layout(manifest);
    require(layout.slab_size == total, "hand-derived slab size");
    const std::array<ScratchResource, 9> order{{
        ScratchResource::sequence_data, ScratchResource::sequence_index,
        ScratchResource::erased_data, ScratchResource::erased_index,
        ScratchResource::consumed_data, ScratchResource::consumed_index,
        ScratchResource::cohort, ScratchResource::exit_rank, ScratchResource::successor_roots,
    }};
    for (std::size_t i = 0; i < capacities.size(); ++i) {
        require(layout.regions[i].resource == order[i], "fixed resource directory order");
        require(layout.regions[i].capacity == capacities[i], "hand-derived exact capacity");
        require(layout.regions[i].span == spans[i], "hand-derived page ceiling");
        require(layout.regions[i].offset == offsets[i], "hand-derived prefix offset");
    }
}

void hand_derived_layouts() {
    // 1. Q(0)=0, zero U/V/R produce no regions. max(4096,0)=4096.
    layout_case({}, Nine{}, Nine{}, Nine{}, 4096);

    // 2. q:40*1+3=43; e:40*2+9=89; c:40*3+11=131.
    // All Q=64, hence I=16*64=1024. Coh=1+96*2+7=200;
    // Rank=1+60*2+5=126; Roots=16*3=48. Each nonzero region
    // rounds to one page: nine pages, prefix offsets 0 through 8*4096.
    layout_case({1, 3, 2, 9, 3, 11, 2, 7, 2, 5, 3},
        {{43, 1024, 89, 1024, 131, 1024, 200, 126, 48}},
        {{4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096}},
        {{0, 4096, 8192, 12288, 16384, 20480, 24576, 28672, 32768}}, 36864);

    // 3. Dq=1280+2816=4096; De=1320+2777=4097 -> TWO pages.
    // Q(32)=64 ->1024; Q(33)=128 ->2048. Coh=1+4032+63=4096;
    // Rank=1+1620+2475=4096; Roots=4096. c has zero length.
    // Page counts [1,1,2,1,0,0,1,1,1] sum to 8, hence 32768.
    layout_case({32, 2816, 33, 2777, 0, 0, 42, 63, 27, 2475, 256},
        {{4096, 1024, 4097, 2048, 0, 0, 4096, 4096, 4096}},
        {{4096, 4096, 8192, 4096, 0, 0, 4096, 4096, 4096}},
        {{0, 4096, 8192, 16384, 20480, 20480, 20480, 24576, 28672}}, 32768);

    // 4. Q(129)=512, Q(256)=512, Q(257)=1024 -> indexes 8192,
    // 8192,16384. D=5165,10240,10287 -> pages 2,3,3. Roots=4112
    // ->2 pages. U=V=0 ignores B_U/B_V. Page counts [2,2,3,2,3,4,0,0,2]
    // sum to 18; the independently summed prefixes end at 18*4096=73728.
    layout_case({129, 5, 256, 0, 257, 7, 0, 99, 0, 99, 257},
        {{5165, 8192, 10240, 8192, 10287, 16384, 0, 0, 4112}},
        {{8192, 8192, 12288, 8192, 12288, 16384, 0, 0, 8192}},
        {{0, 8192, 16384, 28672, 36864, 49152, 65536, 65536, 65536}}, 73728);

    // 5. K=0 keeps each Q/index at zero even with a byte allowance. D=B:
    // 4097 ->2 pages, 1 ->1 page, 4096 ->1 page; total four pages.
    layout_case({0, 4097, 0, 1, 0, 4096, 0, 1, 0, 1, 0},
        {{4097, 0, 1, 0, 4096, 0, 0, 0, 0}},
        {{8192, 0, 4096, 0, 4096, 0, 0, 0, 0}},
        {{0, 8192, 8192, 12288, 12288, 16384, 16384, 16384, 16384}}, 16384);
}

void checked_overflows() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    ScratchManifest manifest;
    manifest.K_q = maximum; // 40*K overflows.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.K_e = 1;
    manifest.B_e = maximum; // 40+B overflows.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    // 40*K still fits uint64, but Q(2^58+1)=2^60 and 16*Q=2^64.
    manifest.K_c = (std::uint64_t{1} << 58) + 1;
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.B_q = maximum; // Page rounding itself would produce 2^64.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest.B_q = std::uint64_t{1} << 63;
    manifest.B_e = std::uint64_t{1} << 63; // Two valid spans overflow their prefix sum.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.B_q = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    // The next page boundary exceeds the host's off_t (or uint64) maximum.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest.B_q = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.U = maximum / 96 + 1;
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.R = maximum / 16 + 1;
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.V = 28; // Separate fixed rank population rule.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
}

struct Snapshot {
    Nine extents{};
    std::array<std::vector<std::uint8_t>, 9> bytes;
};

Snapshot snapshot(const ScratchSlab& slab) {
    Snapshot out;
    for (std::size_t i = 0; i < out.bytes.size(); ++i) {
        const auto& region = slab.regions()[i];
        out.extents[i] = region.logical_extent;
        out.bytes[i].resize(static_cast<std::size_t>(region.logical_extent));
        slab.read_at(region, 0, out.bytes[i].data(), out.bytes[i].size());
    }
    return out;
}

void unchanged(const ScratchSlab& slab, const Snapshot& before) {
    const auto after = snapshot(slab);
    require(after.extents == before.extents, "refusal preserves every logical extent");
    require(after.bytes == before.bytes, "refusal preserves every region and its neighbors");
}

void bounds_reservations_and_reset() {
    const auto before_allocation = global_allocation::allocations;
    ScratchSlab slab({1, 3, 2, 9, 3, 11, 2, 7, 2, 5, 3});
    require(global_allocation::allocations == before_allocation + 1,
            "one aligned backing allocation for all nine regions");
    const auto& sequence = slab.region(ScratchResource::sequence_data);
    const std::array<std::uint8_t, 4> sample{{7, 11, 19, 23}};
    std::array<std::uint8_t, 4> observed{};
    require(sequence.logical_extent == 0, "new logical prefix is empty");
    io_failure([&] { slab.read_at(sequence, 0, observed.data(), 1); });
    slab.write_at(sequence, 0, sample.data(), sample.size());
    slab.read_at(sequence, 0, observed.data(), observed.size());
    require(observed == sample && sequence.logical_extent == sample.size(), "copy and extent");

    const auto short_prefix = snapshot(slab);
    const auto before_reservation = global_allocation::allocations;
    require(slab.can_extend(sequence, sequence.capacity), "exact byte capacity is reservable");
    slab.require_extend(sequence, sequence.capacity);
    require(global_allocation::allocations == before_reservation, "reservation allocates nothing");
    unchanged(slab, short_prefix); // Successful reservations are also read-only.
    io_failure([&] { slab.read_at(sequence, sample.size(), observed.data(), 1); });

    const std::array<const char*, 9> names{{"sequence_data", "sequence_index", "erased_data",
        "erased_index", "consumed_data", "consumed_index", "cohort", "exit_rank", "successor_roots"}};
    std::array<std::uint8_t*, 9> addresses{};
    const auto original_layout = slab.layout();
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        const auto& region = slab.regions()[i];
        addresses[i] = region.base; // Identity only; no payload pointer is dereferenced.
        require(reinterpret_cast<std::uintptr_t>(region.base) % 4096 == 0, "aligned directory");
        std::vector<std::uint8_t> pattern(static_cast<std::size_t>(region.capacity),
                                           static_cast<std::uint8_t>(i + 1));
        slab.write_at(region, 0, pattern.data(), pattern.size());
    }
    const auto full = snapshot(slab);
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        const auto& region = slab.regions()[i];
        require(!slab.can_extend(region, region.capacity + 1), "page padding is not admitted");
        std::uint64_t publication = 0;
        capacity_failure(names[i], [&] {
            slab.require_extend(region, region.capacity + 1);
            ++publication; // Must never be reached on a refused reservation.
            slab.write_at(region, 0, sample.data(), sample.size());
        });
        require(publication == 0, "reservation precedes its caller's first mutation");
        capacity_failure(names[i], [&] { slab.write_at(region, region.capacity, sample.data(), 1); });
        capacity_failure(names[i], [&] {
            slab.write_at(region, std::numeric_limits<std::uint64_t>::max(), sample.data(), 1);
        });
        io_failure([&] { slab.read_at(region, region.capacity, observed.data(), 1); });
        unchanged(slab, full);
    }
    io_failure([&] { slab.write_at(sequence, 0, nullptr, 1); });
    io_failure([&] { slab.read_at(sequence, 0, nullptr, 1); });
    unchanged(slab, full);
    slab.write_at(sequence, sequence.capacity, nullptr, 0);
    slab.read_at(sequence, sequence.capacity, nullptr, 0);
    unchanged(slab, full);

    ScratchSlab other({1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    capacity_failure("layout", [&] { other.require_extend(sequence, 1); });
    capacity_failure("layout", [&] { (void)slab.region(ScratchResource::layout); });

    slab.zero(ScratchResource::cohort);
    const auto cleared = snapshot(slab);
    require(cleared.extents == full.extents, "zero retains extents");
    for (std::size_t i = 0; i < cleared.bytes.size(); ++i) {
        if (i == 6)
            require(std::all_of(cleared.bytes[i].begin(), cleared.bytes[i].end(),
                                [](std::uint8_t byte) { return byte == 0; }), "zeroed cohort bytes");
        else require(cleared.bytes[i] == full.bytes[i], "per-region zero preserves neighbors");
    }
    const auto before_reset = global_allocation::allocations;
    slab.reset();
    require(global_allocation::allocations == before_reset, "reset cannot reallocate, even at the same address");
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        const auto& region = slab.regions()[i];
        require(region.base == addresses[i], "reset retains the same allocation and directory");
        require(region.capacity == original_layout.regions[i].capacity, "reset retains capacity");
        require(slab.layout().regions[i].offset == original_layout.regions[i].offset, "reset retains offsets");
        require(region.logical_extent == 0, "reset clears logical extents");
        slab.extend(region.resource, region.capacity);
    }
    const auto reset = snapshot(slab);
    for (const auto& bytes : reset.bytes)
        require(std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; }),
                "reset reinitializes every byte within the same slab");
    slab.write_at(sequence, 8, sample.data(), sample.size());
    slab.reset(ScratchResource::sequence_data);
    require(sequence.logical_extent == 0, "store-local reset clears its prefix");
    slab.extend(sequence.resource, 12);
    std::array<std::uint8_t, 12> zeros{};
    std::array<std::uint8_t, 12> after_local_reset{};
    slab.read_at(sequence, 0, after_local_reset.data(), after_local_reset.size());
    require(after_local_reset == zeros, "store-local reset removes prior bytes and holes");
}

} // namespace

int main() {
    try {
        hand_derived_layouts();
        checked_overflows();
        bounds_reservations_and_reset();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_slab_layout: %s\n", error.what());
        return 1;
    }
}
