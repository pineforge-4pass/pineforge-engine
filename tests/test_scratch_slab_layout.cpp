// H1-11: CR-150 RAM-foundation regression, registered by core-cr150-ramfound.cmake.
// The original frozen closure lacked pineforge/detail/scratch_slab.hpp; that
// header is now part of the implementation. The original first-diagnostic
// log is not recorded here; the missing-header claim is historical.
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
    } catch (const detail::AuxiliaryScratchIoError& error) {
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

    // 2. q is reserved zero: Dq=Iq=0, with no page span. e:40*2+9=89;
    // c:40*3+11=131. Both Q=64, hence I=16*64=1024.
    // Coh=1+96*2+7=200; Rank=1+60*2+5=126; Roots=16*3=48.
    // Seven nonzero regions each take one page; prefix page counts are
    // [0,0,0,1,2,3,4,5,6], ending at 7*4096=28672.
    layout_case({0, 0, 2, 9, 3, 11, 2, 7, 2, 5, 3},
        {{0, 0, 89, 1024, 131, 1024, 200, 126, 48}},
        {{0, 0, 4096, 4096, 4096, 4096, 4096, 4096, 4096}},
        {{0, 0, 0, 4096, 8192, 12288, 16384, 20480, 24576}}, 28672);

    // 3. q and c are empty. De=40*33+2777=1320+2777=4097 -> TWO pages;
    // Q(33)=128 -> Ie=2048 -> one page. Coh=1+96*42+63=4096;
    // Rank=1+60*27+2475=4096; Roots=16*256=4096 -> one page each.
    // Page counts [0,0,2,1,0,0,1,1,1] sum to 6; prefix page counts
    // [0,0,0,2,3,3,3,4,5] end at 6*4096=24576.
    layout_case({0, 0, 33, 2777, 0, 0, 42, 63, 27, 2475, 256},
        {{0, 0, 4097, 2048, 0, 0, 4096, 4096, 4096}},
        {{0, 0, 8192, 4096, 0, 0, 4096, 4096, 4096}},
        {{0, 0, 0, 8192, 12288, 12288, 12288, 16384, 20480}}, 24576);

    // 4. q is empty. Q(256)=512, Q(257)=1024 -> indexes 8192,16384
    // -> pages 2,4. De=40*256=10240 ->3 pages; Dc=40*257+7=10287
    // ->3 pages. Roots=16*257=4112 ->2 pages; U=V=0 ignores B_U/B_V.
    // Page counts [0,0,3,2,3,4,0,0,2] sum to 14; prefix page counts
    // [0,0,0,3,5,8,12,12,12] end at 14*4096=57344.
    layout_case({0, 0, 256, 0, 257, 7, 0, 99, 0, 99, 257},
        {{0, 0, 10240, 8192, 10287, 16384, 0, 0, 4112}},
        {{0, 0, 12288, 8192, 12288, 16384, 0, 0, 8192}},
        {{0, 0, 0, 12288, 20480, 32768, 49152, 49152, 49152}}, 57344);

    // 5. q is empty; K_e=K_c=0 keeps their Q/index at zero even with
    // byte allowances. De=B_e=1, Dc=B_c=4096 -> one page each.
    // U=V=0 ignores B_U/B_V. Prefix page counts [0,0,0,1,1,2,2,2,2]
    // end at 2*4096=8192.
    layout_case({0, 0, 0, 1, 0, 4096, 0, 1, 0, 1, 0},
        {{0, 0, 1, 0, 4096, 0, 0, 0, 0}},
        {{0, 0, 4096, 0, 4096, 0, 0, 0, 0}},
        {{0, 0, 0, 4096, 4096, 8192, 8192, 8192, 8192}}, 8192);

    // 6. Preserve q's former boundary witnesses in live e/c regions:
    // De=40*129+5=5165 ->2 pages; Q(129)=512 -> Ie=8192 ->2 pages.
    // Dc=40*32+2816=4096 ->1 page; Q(32)=64 -> Ic=1024 ->1 page.
    // All other capacities are zero; page counts [0,0,2,2,1,1,0,0,0]
    // give prefixes [0,0,0,2,4,5,6,6,6] and total 6*4096=24576.
    layout_case({0, 0, 129, 5, 32, 2816, 0, 0, 0, 0, 0},
        {{0, 0, 5165, 8192, 4096, 1024, 0, 0, 0}},
        {{0, 0, 8192, 8192, 4096, 4096, 0, 0, 0}},
        {{0, 0, 0, 8192, 16384, 20480, 24576, 24576, 24576}}, 24576);

    // 7. Preserve the former q byte-only page-plus-one witness in e:
    // K_e=0 -> Ie=0; De=B_e=4097 ->2 pages. All other capacities are zero.
    // Prefix page counts [0,0,0,2,2,2,2,2,2] end at 2*4096=8192.
    layout_case({0, 0, 0, 4097, 0, 0, 0, 0, 0, 0, 0},
        {{0, 0, 4097, 0, 0, 0, 0, 0, 0}},
        {{0, 0, 8192, 0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 8192, 8192, 8192, 8192, 8192, 8192}}, 8192);
}

void checked_overflows() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    ScratchManifest manifest;
    manifest.K_q = 1; // Even one reserved q key is refused, before arithmetic.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.B_q = 1; // Independently refuse a reserved q byte allowance.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.K_e = maximum; // 40*K overflows in a live store.
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
    manifest.B_e = maximum; // K_e=0: page rounding itself would produce 2^64.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest.B_e = std::uint64_t{1} << 63;
    manifest.B_c = std::uint64_t{1} << 63; // Two valid spans overflow their prefix sum.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest = {};
    manifest.B_e = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    // The next page boundary exceeds the host's off_t (or uint64) maximum.
    capacity_failure("layout", [&] { (void)detail::compute_layout(manifest); });
    manifest.B_e = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
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
    // Worked manifest 2 above: q empty, seven pages, De=89 live bytes.
    ScratchSlab slab({0, 0, 2, 9, 3, 11, 2, 7, 2, 5, 3});
    require(global_allocation::allocations == before_allocation + 1,
            "one aligned backing allocation for all nine regions");
    const auto& erased = slab.region(ScratchResource::erased_data);
    const std::array<std::uint8_t, 4> sample{{7, 11, 19, 23}};
    std::array<std::uint8_t, 4> observed{};
    require(erased.logical_extent == 0, "new logical prefix is empty");
    io_failure([&] { slab.read_at(erased, 0, observed.data(), 1); });
    slab.write_at(erased, 0, sample.data(), sample.size());
    slab.read_at(erased, 0, observed.data(), observed.size());
    require(observed == sample && erased.logical_extent == sample.size(), "copy and extent");

    const auto short_prefix = snapshot(slab);
    const auto before_reservation = global_allocation::allocations;
    require(slab.can_extend(erased, erased.capacity), "exact byte capacity is reservable");
    slab.require_extend(erased, erased.capacity);
    require(global_allocation::allocations == before_reservation, "reservation allocates nothing");
    unchanged(slab, short_prefix); // Successful reservations are also read-only.
    io_failure([&] { slab.read_at(erased, sample.size(), observed.data(), 1); });

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
    io_failure([&] { slab.write_at(erased, 0, nullptr, 1); });
    io_failure([&] { slab.read_at(erased, 0, nullptr, 1); });
    unchanged(slab, full);
    slab.write_at(erased, erased.capacity, nullptr, 0);
    slab.read_at(erased, erased.capacity, nullptr, 0);
    unchanged(slab, full);

    // Only e is live: De=40*1+3=43 and Ie=16*64=1024 each take one
    // page. Offsets [0,0,0,4096,8192,8192,8192,8192,8192], SLAB=8192.
    ScratchSlab other({0, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0});
    capacity_failure("layout", [&] { other.require_extend(erased, 1); });
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
    slab.write_at(erased, 8, sample.data(), sample.size());
    slab.reset(ScratchResource::erased_data);
    require(erased.logical_extent == 0, "store-local reset clears its prefix");
    slab.extend(erased.resource, 12);
    std::array<std::uint8_t, 12> zeros{};
    std::array<std::uint8_t, 12> after_local_reset{};
    slab.read_at(erased, 0, after_local_reset.data(), after_local_reset.size());
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
