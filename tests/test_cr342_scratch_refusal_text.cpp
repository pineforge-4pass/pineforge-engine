// CR-342. Expected wall time: under 1 s (required budget: under 5 s).
// Edit-only lane: compile/run red on unchanged HEAD and green in the lab.
// Predicted first red: require_extend(sequence_data, 7) still says only
// "scratch capacity exceeded"; no observed fail-before or pass is claimed.
#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace pd = pineforge::detail;
namespace no = pineforge::native_order;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Operation>
void refusal(const char* resource, const char* expected, Operation operation) {
    bool caught = false;
    try {
        operation();
    } catch (const pd::ScratchCapacityError& error) {
        caught = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded,
                "refusal must preserve scratch_capacity_exceeded");
        require(failure.args
                    && *failure.args == std::string("{\"resource\":\"") + resource + "\"}",
                "refusal must preserve exactly the single resource argument");
        if (std::string(error.what()) != expected)
            throw std::runtime_error(std::string("expected: ") + expected + "; got: " + error.what());
    }
    require(caught, "operation must reach a real ScratchCapacityError throw");
}

void slab_byte_refusals() {
    // q=0; De=40*2+9=89; Dc=40*3+11=131; Q(2)=Q(3)=64,
    // so Ie=Ic=16*64=1024; Coh=1+96*2+7=200;
    // Rank=1+60*2+5=126; Roots=16*3=48. Seven pages = 28672 bytes.
    pd::ScratchSlab slab({0, 0, 2, 9, 3, 11, 2, 7, 2, 5, 3});
    struct Case {
        pd::ScratchResource resource;
        const char* name;
        std::uint64_t limit;
        std::uint64_t extent;
        std::uint64_t offset;
        std::size_t length;
        const char* extend_text;
        const char* write_text;
    };
    // Reservation needs are limit+7; writes need offset+length=limit+3.
    // For the two empty q regions the write is [0,3).
    const std::array<Case, 9> cases{{
        {pd::ScratchResource::sequence_data, "sequence_data", 0, 7, 0, 3,
         "sequence_data capacity exceeded: limit 0, needed 7",
         "sequence_data capacity exceeded: limit 0, needed 3"},
        {pd::ScratchResource::sequence_index, "sequence_index", 0, 7, 0, 3,
         "sequence_index capacity exceeded: limit 0, needed 7",
         "sequence_index capacity exceeded: limit 0, needed 3"},
        {pd::ScratchResource::erased_data, "erased_data", 89, 96, 88, 4,
         "erased_data capacity exceeded: limit 89, needed 96",
         "erased_data capacity exceeded: limit 89, needed 92"},
        {pd::ScratchResource::erased_index, "erased_index", 1024, 1031, 1023, 4,
         "erased_index capacity exceeded: limit 1,024, needed 1,031",
         "erased_index capacity exceeded: limit 1,024, needed 1,027"},
        {pd::ScratchResource::consumed_data, "consumed_data", 131, 138, 130, 4,
         "consumed_data capacity exceeded: limit 131, needed 138",
         "consumed_data capacity exceeded: limit 131, needed 134"},
        {pd::ScratchResource::consumed_index, "consumed_index", 1024, 1031, 1023, 4,
         "consumed_index capacity exceeded: limit 1,024, needed 1,031",
         "consumed_index capacity exceeded: limit 1,024, needed 1,027"},
        {pd::ScratchResource::cohort, "cohort", 200, 207, 199, 4,
         "cohort capacity exceeded: limit 200, needed 207",
         "cohort capacity exceeded: limit 200, needed 203"},
        {pd::ScratchResource::exit_rank, "exit_rank", 126, 133, 125, 4,
         "exit_rank capacity exceeded: limit 126, needed 133",
         "exit_rank capacity exceeded: limit 126, needed 129"},
        {pd::ScratchResource::successor_roots, "successor_roots", 48, 55, 47, 4,
         "successor_roots capacity exceeded: limit 48, needed 55",
         "successor_roots capacity exceeded: limit 48, needed 51"},
    }};
    const std::array<std::uint8_t, 4> bytes{{1, 2, 3, 4}};
    for (const auto& row : cases) {
        const auto& region = slab.region(row.resource);
        require(region.capacity == row.limit, "hand-derived region byte capacity");
        slab.require_extend(row.resource, row.limit);
        slab.write_at(row.resource, row.limit, nullptr, 0);
        refusal(row.name, row.extend_text, [&] { slab.require_extend(row.resource, row.extent); });
        refusal(row.name, row.extend_text, [&] { slab.require_extend(region, row.extent); });
        refusal(row.name, row.write_text, [&] {
            slab.write_at(row.resource, row.offset, bytes.data(), row.length);
        });
        refusal(row.name, row.write_text, [&] {
            slab.write_at(region, row.offset, bytes.data(), row.length);
        });
        require(region.logical_extent == 0, "reservations and refused writes publish no extent");
    }
    // First bounds disjunct, including a zero-length out-of-range write.
    refusal("erased_data", "erased_data capacity exceeded: limit 89, needed 100", [&] {
        slab.write_at(pd::ScratchResource::erased_data, 100, nullptr, 0);
    });
    refusal("erased_data", "erased_data capacity exceeded: limit 89, needed 104", [&] {
        slab.write_at(pd::ScratchResource::erased_data, 100, bytes.data(), 4);
    });
    // UINT64_MAX+1 is an exact sum in the text, never a wrapped needed 0.
    refusal("erased_data",
        "erased_data capacity exceeded: limit 89, needed 18,446,744,073,709,551,615 + 1"
        " (offset + length overflows uint64)", [&] {
            slab.write_at(pd::ScratchResource::erased_data,
                          std::numeric_limits<std::uint64_t>::max(), bytes.data(), 1);
        });
}

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

void successor_row_refusal(std::uint64_t limit, const char* expected, bool direct) {
    pd::ScratchManifest manifest;
    manifest.R = limit;
    auto slab = std::make_shared<pd::ScratchSlab>(manifest);
    no::WorkingRequestCore core{{"cr342-roots", 1}};
    core.bind_scratch_slab(slab);
    const no::CommandContext context{};
    std::uint64_t incarnation = 1;
    std::uint64_t next_ordinal = 1;
    const auto submitted = core.apply_submit(request(), context, incarnation, next_ordinal).result;
    require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
            "rootless submit must be accepted even at R=0");
    auto tail = *submitted.handle;
    ++incarnation;
    ++next_ordinal;
    core.retire_history(core.last_ordinal());
    for (std::uint64_t i = 0; i < limit; ++i) {
        const auto installed = core.apply_replace(tail, request(), context, incarnation, next_ordinal);
        require(installed.result.status == no::ReplaceStatus::Replaced && installed.result.successor,
                "replacement must really consume a root row");
        tail = *installed.result.successor;
        ++incarnation;
        ++next_ordinal;
        core.retire_history(core.last_ordinal());
    }
    const auto& region = slab->region(pd::ScratchResource::successor_roots);
    if (limit == 65)
        require(region.logical_extent == 1024, "64 flushed roots occupy exactly 1024 bytes");
    const auto ordinal = core.last_ordinal();
    refusal("successor_roots", expected, [&] {
        if (direct)
            (void)core.apply_replace(tail, request(), context, incarnation, next_ordinal);
        else
            (void)core.prepare_replace(tail, request(), context, incarnation, next_ordinal);
    });
    require(core.last_ordinal() == ordinal && core.find_live(tail),
            "refusal precedes replacement publication");
}

void successor_row_refusals() {
    for (const bool direct : {false, true}) {
        // R=0: disk=0, pending=0, adds=1 -> need 1.
        successor_row_refusal(0, "successor_roots capacity exceeded: limit 0, needed 1", direct);
        // R=2: disk=0, pending=2, adds=1 -> need 3 (not 48 bytes).
        successor_row_refusal(2, "successor_roots capacity exceeded: limit 2, needed 3", direct);
        // R=65: disk=64, pending=1, adds=1 -> need 66 (not pending+1=2).
        successor_row_refusal(65, "successor_roots capacity exceeded: limit 65, needed 66", direct);
    }
}

void layout_refusals() {
    pd::ScratchManifest manifest;
    manifest.V = 28;
    refusal("layout", "scratch layout V exceeds 27", [&] { (void)pd::compute_layout(manifest); });
    manifest = {};
    manifest.K_q = 1;
    refusal("layout", "scratch layout K_q and B_q must be zero (q region is reserved)",
            [&] { (void)pd::compute_layout(manifest); });
    manifest = {};
    manifest.B_q = 1;
    refusal("layout", "scratch layout K_q and B_q must be zero (q region is reserved)",
            [&] { (void)pd::compute_layout(manifest); });
    manifest = {};
    manifest.K_e = std::numeric_limits<std::uint64_t>::max(); // 40*K_e > UINT64_MAX.
    refusal("layout", "scratch layout K_e multiplication overflows uint64",
            [&] { (void)pd::compute_layout(manifest); });
    manifest.K_e = 1;
    manifest.B_e = std::numeric_limits<std::uint64_t>::max(); // 40+UINT64_MAX.
    refusal("layout", "scratch layout K_e/B_e addition overflows uint64",
            [&] { (void)pd::compute_layout(manifest); });
    manifest = {};
    manifest.K_c = (std::uint64_t{1} << 58) + 1; // 16*Q(K_c)=16*2^60=2^64.
    refusal("layout", "scratch layout K_c index bytes multiplication overflows uint64",
            [&] { (void)pd::compute_layout(manifest); });
    manifest = {};
    manifest.B_e = std::numeric_limits<std::uint64_t>::max(); // Round page up to 2^64.
    refusal("layout", "scratch layout erased_data multiplication overflows uint64",
            [&] { (void)pd::compute_layout(manifest); });
    manifest.B_e = std::uint64_t{1} << 63;
    manifest.B_c = std::uint64_t{1} << 63; // Prefix spans sum to 2^64.
    refusal("layout", "scratch layout slab_size addition overflows uint64",
            [&] { (void)pd::compute_layout(manifest); });
    manifest = {};
    manifest.R = (std::uint64_t{1} << 60); // 16*R=2^64.
    refusal("layout", "scratch layout R multiplication overflows uint64",
            [&] { (void)pd::compute_layout(manifest); });
    // Layout only: never construct a slab or allocate these enormous sizes.
    if constexpr (std::numeric_limits<std::size_t>::digits < 64
                  || std::numeric_limits<off_t>::digits < 64) {
        manifest = {};
        const auto host_limit = std::min(
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()),
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()));
        manifest.B_e = host_limit + 1;
        refusal("layout", "scratch layout slab_size exceeds size_t or off_t limit",
                [&] { (void)pd::compute_layout(manifest); });
    }
    pd::ScratchSlab slab(pd::ScratchManifest{});
    refusal("layout", "scratch layout region resource index 0 is outside 1..9",
            [&] { (void)slab.region(pd::ScratchResource::layout); });
    refusal("layout", "scratch layout region resource index 10 is outside 1..9",
            [&] { (void)slab.region(static_cast<pd::ScratchResource>(10)); });
    refusal("layout", "scratch layout region resource index 0 is outside 1..9",
            [&] { slab.require_extend(pd::ScratchResource::layout, 1); });
}
} // namespace

int main() {
    try {
        slab_byte_refusals();
        successor_row_refusals();
        layout_refusals();
        std::puts("CR-342 scratch refusal text: complete");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR-342 scratch refusal text: %s\n", error.what());
        return 1;
    }
}
