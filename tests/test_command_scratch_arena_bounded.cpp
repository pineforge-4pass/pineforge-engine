// Set B: the new bounded-region and carried-failure API. Static read only in
// the edit lane; the supervisor compiles/runs this on the lab hosts.
#include "command_scratch_arena_share_fixture.hpp"

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <cerrno>
#include <dirent.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <vector>

namespace {
using namespace command_arena_test;
using pineforge::RunFailureCode;

void integer_arg(const ReportScratchIoError& error, const char* name, std::uint64_t expected) {
    require(error.run_failure().args != nullptr, "carried failure has typed arguments");
    const auto& json = *error.run_failure().args;
    const auto needle = std::string("\"") + name + "\":" + std::to_string(expected);
    const auto at = json.find(needle);
    require(at != std::string::npos && at + needle.size() < json.size()
            && (json[at + needle.size()] == ',' || json[at + needle.size()] == '}'),
            "carried counter equals the arena snapshot, with exact integer spelling");
}

void exhaustion_args(const ReportScratchIoError& error, const ScratchArenaStats& stats,
                     std::uint64_t needed) {
    require(error.run_failure_code() == RunFailureCode::report_sink_failed,
            "arena exception retains report_sink_failed");
    require(error.run_failure().args != nullptr, "typed exhaustion has an argument object");
    const auto& json = *error.run_failure().args;
    require(json.find("\"reason\":\"arena_exhausted\"") != std::string::npos,
            "arena exception retains its arena_exhausted reason");
    require(json.find("\"store\":\"command\"") != std::string::npos,
            "the failed physical region identifies the command store");
    require(std::string(error.what()).find("scratch arena exhausted") != std::string::npos,
            "the original arena exception English survives");
    integer_arg(error, "high_water_bytes", stats.high_water);
    integer_arg(error, "arena_bytes", stats.bytes_supplied);
    integer_arg(error, "claimed_bytes", stats.bytes_used);
    integer_arg(error, "needed_bytes_lower_bound", needed);
}

void carried_failure_and_budget_rollback() {
    ArenaAttachment attachment(arena_for_store_chunks(3));
    auto* arena = ScratchArena::attached();
    const auto empty = arena->stats();
    const auto chunk = empty.bytes_total / empty.chunks_total;
    ScratchFile filler;
    filler.open("command-carried-filler");
    filler.ensure_size(empty.bytes_total - chunk);
    require(arena->stats().bytes_used == empty.bytes_total - chunk,
            "carried-failure filler claims all but one chunk");
    auto previous = set_command_scratch_byte_cap(2 * chunk);
    {
        auto backing = make_command_scratch_backing(kPageBytes);
        std::array<std::byte, kPageBytes> image{};
        require(backing->write_page(0, {image.data(), image.size()}) == ScratchStatus::ok,
                "first command page claims the last available chunk");
        const auto before = arena->stats();
        const auto first_new_chunk_page = chunk / kPageBytes;
        require((first_new_chunk_page + 1) * kPageBytes <= share_capacity(empty.bytes_supplied),
                "arena exhaustion occurs below the named region bound");
        require(backing->write_page(first_new_chunk_page, {image.data(), image.size()})
                    == ScratchStatus::io_error,
                "arena exhaustion is io_error at the backing boundary");
        const auto failed = arena->stats();
        require(failed.bytes_used == before.bytes_used && failed.chunks_free == 0,
                "failed grow claims no additional chunks");
        const auto* carried = backing->carried_failure();
        require(carried != nullptr, "io_error carries the arena exception");
        const auto needed = required_arena((first_new_chunk_page + 1) * kPageBytes);
        exhaustion_args(*carried, failed, needed);
        const auto original_args = carried->run_failure().args;
        bool caught = false;
        try {
            rethrow_carried_scratch_failure(carried);
        } catch (const ReportScratchIoError& error) {
            caught = true;
            require(error.run_failure().args == original_args,
                    "rethrow preserves the original validated argument object");
            exhaustion_args(error, failed, needed);
        }
        require(caught, "the owner helper really rethrows the original exception");
        require(backing->page_count() == 1, "failed grow preserves logical extent");
        filler.clear();
        // A failed ensure_size must release its command byte-budget reserve.
        require(backing->write_page(first_new_chunk_page, {image.data(), image.size()})
                    == ScratchStatus::ok,
                "released filler permits retry and failed growth did not leak byte budget");
        std::array<std::byte, kPageBytes> read{};
        require(backing->read_page(first_new_chunk_page, {read.data(), read.size()})
                    == ScratchStatus::ok && read == image,
                "retry reads back the written page");
        require(backing->carried_failure() == nullptr,
                "a subsequent successful operation does not retain a stale failure");
    }
    set_command_scratch_byte_cap(std::move(previous));
    require(arena->stats().bytes_used == 0, "carried-failure regions are released");
}

void share_refuses_before_claim_and_releases_budget(std::size_t page_bytes) {
    // A fractional tail checks the exact supplied-size failure counter.
    // Independently derive K from that same stats field, including its tail.
    ArenaAttachment attachment(arena_for_store_chunks(2) + ScratchArena::kChunkBytes / 3);
    auto* arena = ScratchArena::attached();
    const auto capacity = share_capacity(arena->stats().bytes_supplied);
    require(command_scratch_arena_capacity_bytes(arena->stats().bytes_supplied) == capacity,
            "production K equals an independent derivation from supplied stats");
    auto previous = set_command_scratch_byte_cap(capacity + page_bytes);
    {
        auto a = make_command_scratch_backing(page_bytes);
        require(a->arena_capacity_bytes() == capacity, "first store fixes its independent share at open");
        std::vector<std::byte> image(page_bytes, std::byte{0x4e});
        const auto pages = capacity / page_bytes;
        require(pages > 0 && capacity % page_bytes == 0,
                "derived share is positive and page aligned");
        require(a->write_page(pages - 1, {image.data(), image.size()}) == ScratchStatus::ok,
                "the exact K boundary is writable");
        const auto before = arena->stats();
        require(before.bytes_used == capacity,
                "the exact K extent claims K bytes");
        auto b = make_command_scratch_backing(page_bytes);
        require(b->arena_capacity_bytes() == capacity && a->arena_capacity_bytes() == capacity
                && arena->stats().bytes_used == before.bytes_used,
                "another store opens with the same fixed share despite existing claims");
        require(a->write_page(pages, {image.data(), image.size()}) == ScratchStatus::io_error,
                "crossing the share returns io_error at the status boundary");
        const auto after = arena->stats();
        require(after.bytes_used == before.bytes_used && after.chunks_free == before.chunks_free
                && after.high_water == before.high_water && a->page_count() == pages,
                "share refusal precedes every arena claim and extent change");
        require(after.chunks_free > 0 && a->carried_failure(),
                "share exhaustion is typed even while the arena has free chunks");
        exhaustion_args(*a->carried_failure(), after,
                        required_arena((pages + 1) * page_bytes));
        require(b->write_page(0, {image.data(), image.size()}) == ScratchStatus::ok,
                "share refusal releases the byte-budget reservation");
        a.reset();
        require(b->write_page(pages - 1, {image.data(), image.size()}) == ScratchStatus::ok,
                "destroying a bounded backing releases its entire command byte budget");
    }
    set_command_scratch_byte_cap(std::move(previous));

    previous = set_command_scratch_byte_cap(capacity);
    {
        auto a = make_command_scratch_backing(page_bytes);
        auto b = make_command_scratch_backing(page_bytes);
        std::vector<std::byte> image(page_bytes, std::byte{0x72});
        const auto pages = capacity / page_bytes;
        require(a->write_page(pages - 1, {image.data(), image.size()}) == ScratchStatus::ok,
                "fill both the share and the independent byte budget");
        const auto before = arena->stats();
        require(a->write_page(pages, {image.data(), image.size()}) == ScratchStatus::io_error
                && a->carried_failure(),
                "simultaneous byte-budget/share refusal still carries arena exhaustion");
        exhaustion_args(*a->carried_failure(), arena->stats(),
                        required_arena((pages + 1) * page_bytes));
        require(arena->stats().bytes_used == before.bytes_used,
                "simultaneous refusal claims no chunks");
        require(b->write_page(0, {image.data(), image.size()}) == ScratchStatus::io_error
                && !b->carried_failure(),
                "byte-budget-only refusal keeps its separate status-only behavior");
        a.reset();
        require(b->write_page(0, {image.data(), image.size()}) == ScratchStatus::ok,
                "neither kind of refused reservation leaks shared budget");
    }
    set_command_scratch_byte_cap(std::move(previous));
}

void real_store_crosses_share_and_replacement_fits() {
    const auto initial = arena_for_store_chunks(1);
    std::uint64_t accepted = 0, replacement = 0;
    std::array<std::byte, 4 * kPageBytes> image{};
    image.fill(std::byte{0x39});
    {
        ArenaAttachment attachment(initial);
        auto* arena = ScratchArena::attached();
        const auto capacity = share_capacity(arena->stats().bytes_supplied);
        FailedWrites failures;
        ObserverRegistration observation(failures);
        auto backing = make_command_scratch_backing(kPageBytes);
        CommandScratchStore store(config(), backing.get());
        require(store.status() == ScratchStatus::ok && store.begin() == ScratchStatus::ok,
                "open the share-crossing real store");
        const auto max_puts = capacity / image.size() + 1;
        ScratchStatus status = ScratchStatus::ok;
        for (; accepted < max_puts; ++accepted) {
            ScratchHandle handle = scratch_no_handle;
            status = put(store, accepted, image, handle);
            if (status != ScratchStatus::ok) break;
        }
        require(status == ScratchStatus::io_error && accepted > 0 && accepted < max_puts,
                "real store puts cross their derived share within a bounded workload");
        const auto failed = arena->stats(); // Independent, AFTER the refusal.
        require(failures.io_errors == 1 && store.carried_failure(),
                "the real store retains exactly one typed backing refusal");
        require(failures.requested_extent > backing->page_count() * kPageBytes
                && chunk_ceil(failures.requested_extent) > capacity,
                "failed page requires growth beyond the store's share");
        require(failed.bytes_used == failures.before.bytes_used
                && failed.high_water == failures.before.high_water && failed.chunks_free > 0,
                "share refusal changes no claim and precedes physical arena exhaustion");
        const auto needed = required_arena(failures.requested_extent);
        exhaustion_args(*store.carried_failure(), failed, needed);
        // Use the REPORTED integer, after checking it independently, to form
        // precisely COMPUTE's next arena. This is a replay, not an inequality.
        const auto& args = *store.carried_failure()->run_failure().args;
        const std::string field = "\"needed_bytes_lower_bound\":";
        const auto at = args.find(field);
        require(at != std::string::npos, "placement reads the reported needed field");
        const auto reported = std::stoull(args.substr(at + field.size()));
        replacement = compute_placement(initial, failed.bytes_used, reported);
    }
    {
        ArenaAttachment attachment(replacement);
        auto backing = make_command_scratch_backing(kPageBytes);
        CommandScratchStore store(config(), backing.get());
        require(store.status() == ScratchStatus::ok && store.begin() == ScratchStatus::ok,
                "open the real store on COMPUTE's reported placement");
        for (std::uint64_t key = 0; key <= accepted; ++key) {
            ScratchHandle handle = scratch_no_handle;
            require(put(store, key, image, handle) == ScratchStatus::ok,
                    "the identical input, including the refused put, fits its placement");
        }
        require(store.commit(ScratchDurability::buffered) == ScratchStatus::ok
                && store.record_count() == accepted + 1 && !store.carried_failure(),
                "replacement commits every input row without exhaustion");
        for (std::uint64_t key = 0; key <= accepted; ++key) {
            ScratchSpanSource probe({reinterpret_cast<const std::byte*>(&key), sizeof key});
            ScratchHandle handle = scratch_no_handle;
            std::array<std::byte, 4 * kPageBytes> actual{};
            require(store.lower_bound(probe, handle) == ScratchStatus::ok
                    && store.read(handle, ScratchField::value, 0, {actual.data(), actual.size()})
                        == ScratchStatus::ok && actual == image,
                    "replacement reads each exact input payload");
        }
    }
}

struct SparseReplay {
    ScratchStatus status = ScratchStatus::ok;
    ScratchArenaStats snapshot;
    std::optional<ReportScratchIoError> failure;
    std::uint64_t reported_needed = 0;
    std::uint64_t written_extent = 0;
};

SparseReplay sparse_input(std::uint64_t arena_bytes, std::uint64_t target_extent) {
    ArenaAttachment attachment(arena_bytes);
    auto backing = make_command_scratch_backing(kPageBytes);
    std::array<std::byte, kPageBytes> image{}, read{};
    image.fill(std::byte{0x67});
    require(target_extent % kPageBytes == 0
            && backing->write_page(0, {image.data(), image.size()}) == ScratchStatus::ok,
            "sparse input writes its first page");
    SparseReplay result;
    result.written_extent = backing->page_count() * kPageBytes;
    const auto before = ScratchArena::attached()->stats();
    result.status = backing->write_page(target_extent / kPageBytes - 1,
                                       {image.data(), image.size()});
    result.snapshot = ScratchArena::attached()->stats();
    if (result.status == ScratchStatus::io_error) {
        require(backing->carried_failure(), "sparse refusal carries typed exhaustion");
        result.failure = *backing->carried_failure();
        exhaustion_args(*result.failure, result.snapshot, required_arena(target_extent));
        require(result.snapshot.bytes_used == before.bytes_used,
                "sparse share refusal does not claim chunks");
        const auto& args = *result.failure->run_failure().args;
        const std::string field = "\"needed_bytes_lower_bound\":";
        const auto at = args.find(field);
        require(at != std::string::npos, "sparse placement reads the reported demand");
        result.reported_needed = std::stoull(args.substr(at + field.size()));
    } else {
        require(result.status == ScratchStatus::ok
                && backing->page_count() * kPageBytes == target_extent,
                "sparse replay completes its entire requested extent");
        require(backing->read_page(target_extent / kPageBytes - 1, {read.data(), read.size()})
                    == ScratchStatus::ok && read == image,
                "sparse replay reads back its distant input page");
    }
    require(backing->read_page(0, {read.data(), read.size()}) == ScratchStatus::ok && read == image,
            "sparse replay preserves the first page in either outcome");
    return result;
}

void reported_placement_and_shortfall_negative_witness() {
    const auto initial = arena_for_store_chunks(1);
    const auto target = 5 * share_capacity(initial) + 2 * ScratchArena::kChunkBytes;
    const auto failed = sparse_input(initial, target);
    require(failed.status == ScratchStatus::io_error, "sparse input crosses the initial share");
    const auto correct = compute_placement(initial, failed.snapshot.bytes_used, failed.reported_needed);
    const auto fitted = sparse_input(correct, target); // The SAME two input writes.
    require(fitted.status == ScratchStatus::ok && !fitted.failure,
            "the reported needed fits the identical sparse input after re-placement");

    auto previous_arena = initial;
    auto previous_run = failed;
    for (unsigned placement = 0; placement < 2; ++placement) {
        const auto own_shortfall = chunk_ceil(target) - chunk_ceil(previous_run.written_extent);
        const auto next = compute_placement(previous_arena, previous_run.snapshot.bytes_used,
                                            own_shortfall);
        previous_run = sparse_input(next, target); // Really re-run on EACH wrong placement.
        require(previous_run.status == ScratchStatus::io_error && previous_run.failure,
                "substituting the store's own shortfall still fails within two re-placements");
        previous_arena = next;
    }
}

void zero_share_and_saturating_arithmetic() {
    for (const auto extent : {std::uint64_t{1}, ScratchArena::kChunkBytes - 1,
                              ScratchArena::kChunkBytes, ScratchArena::kChunkBytes + 1,
                              4 * ScratchArena::kChunkBytes})
        require(command_scratch_arena_needed_bytes(extent) == required_arena(extent),
                "integer demand matches independent chunk and ratio ceilings");
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    require(command_scratch_arena_needed_bytes(maximum) == maximum
            && command_scratch_arena_needed_bytes(std::numeric_limits<std::uint64_t>::max()) == maximum,
            "overflow and out-of-transport requirements saturate to INT64_MAX");
    ArenaAttachment attachment(ScratchArena::kChunkBytes);
    auto* arena = ScratchArena::attached();
    require(share_capacity(arena->stats().bytes_supplied) == 0,
            "one arena chunk gives a strictly fractional command share");
    auto backing = make_command_scratch_backing(kPageBytes);
    std::array<std::byte, kPageBytes> image{};
    const auto before = arena->stats();
    require(backing->write_page(0, {image.data(), image.size()}) == ScratchStatus::io_error
            && backing->carried_failure(), "a zero-share region refuses its first page with a typed error");
    const auto failed = arena->stats();
    require(failed.bytes_used == before.bytes_used && failed.high_water == before.high_water,
            "zero-share refusal claims nothing");
    exhaustion_args(*backing->carried_failure(), failed, required_arena(image.size()));

    // This page extent is representable in off_t, but its chunk ceiling is
    // beyond INT64_MAX. Dividing by f <= 1 cannot reduce that requirement, so
    // the independently expected transported demand is the saturation limit.
    const auto last_page = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
        / kPageBytes - 1;
    require(backing->write_page(last_page, {image.data(), image.size()}) == ScratchStatus::io_error
            && backing->carried_failure(), "a representable huge write still has typed exhaustion");
    const auto huge_failed = arena->stats();
    require(huge_failed.bytes_used == failed.bytes_used && backing->page_count() == 0,
            "saturated-demand refusal claims neither chunks nor logical pages");
    exhaustion_args(*backing->carried_failure(), huge_failed, maximum);
}

void uncertified_default_sized_arena(std::size_t os_page) {
    // The owner brief specifies 1 GiB. COMPUTE owns the production default;
    // this engine tree has no default arena byte constant to import.
    constexpr std::uint64_t kHostDefaultBytesFromBrief = std::uint64_t{1} << 30;
    ArenaAttachment attachment(kHostDefaultBytesFromBrief);
    auto* arena = ScratchArena::attached();
    const auto supplied = arena->stats();
    const auto capacity = share_capacity(supplied.bytes_supplied);
    auto a = make_command_scratch_backing(kPageBytes);
    auto b = make_command_scratch_backing(kPageBytes);
    auto c = make_command_scratch_backing(os_page);
    require(a->arena_capacity_bytes() == capacity && b->arena_capacity_bytes() == capacity
            && c->arena_capacity_bytes() == capacity,
            "default-sized run gives all three stores f times the independently read arena size");
    require(arena->stats().bytes_used == supplied.bytes_used,
            "opening the three directories claims no arena bytes");
    CommandScratchStore first(config(), a.get()), second(config(), b.get());
    auto source_config = config();
    source_config.page_size = os_page;
    CommandScratchStore source(source_config, c.get());
    std::array<std::byte, 4 * kPageBytes> image{};
    image.fill(std::byte{0x2b});
    for (auto* store : {&first, &second, &source}) {
        require(store->status() == ScratchStatus::ok && store->begin() == ScratchStatus::ok,
                "default-sized arena opens each real command store");
        ScratchHandle handle = scratch_no_handle;
        require(put(*store, 19, image, handle) == ScratchStatus::ok
                && store->commit(ScratchDurability::buffered) == ScratchStatus::ok,
                "small run completes on every default-sized command store");
        std::array<std::byte, 4 * kPageBytes> read{};
        require(store->read(handle, ScratchField::value, 0, {read.data(), read.size()})
                    == ScratchStatus::ok && read == image,
                "small default-sized run reads its exact data");
    }
    require(a->page_count() * kPageBytes <= capacity && b->page_count() * kPageBytes <= capacity
            && c->page_count() * os_page <= capacity,
            "small run stays inside each derived share");
}

void store_retains_typed_failure_across_rollback() {
    ArenaAttachment attachment(arena_for_store_chunks(3));
    auto* arena = ScratchArena::attached();
    const auto empty = arena->stats();
    const auto chunk = empty.bytes_total / empty.chunks_total;
    ScratchFile filler;
    filler.open("command-rollback-filler");
    filler.ensure_size(empty.bytes_total - chunk);
    FailedWrites failures;
    ObserverRegistration observation(failures);
    auto backing = make_command_scratch_backing(kPageBytes);
    CommandScratchStore store(config(), backing.get());
    require(store.status() == ScratchStatus::ok && store.begin() == ScratchStatus::ok,
            "open rollback diagnostic witness");
    std::array<std::byte, 4 * kPageBytes> image{};
    const auto max_puts = chunk / image.size() + 1;
    ScratchStatus status = ScratchStatus::ok;
    for (std::uint64_t key = 0; key < max_puts && status == ScratchStatus::ok; ++key) {
        ScratchHandle handle = scratch_no_handle;
        status = put(store, key, image, handle);
    }
    require(status == ScratchStatus::io_error && store.carried_failure(),
            "real store puts return io_error and retain the backing exception");
    const auto failed = arena->stats();
    const auto needed = required_arena(failures.requested_extent);
    exhaustion_args(*store.carried_failure(), failed, needed);
    const auto original_args = store.carried_failure()->run_failure().args;
    require(store.rollback() == ScratchStatus::ok, "rollback the failed empty-store transaction");
    // Exercise successful backing I/O as cleanup does, independently of
    // whether this particular rollback needed a journal read.
    std::array<std::byte, kPageBytes> page{};
    require(backing->read_page(0, {page.data(), page.size()}) == ScratchStatus::ok
            && backing->carried_failure() == nullptr,
            "successful backing cleanup replaces its borrowed last-operation diagnostic");
    require(store.carried_failure()
            && store.carried_failure()->run_failure().args == original_args,
            "store owns the exact typed failure beyond backing cleanup");
    exhaustion_args(*store.carried_failure(), failed, needed);
    require(store.begin() == ScratchStatus::ok && store.carried_failure() == nullptr,
            "a new transaction clears the old diagnostic");
    require(store.rollback() == ScratchStatus::ok, "finish diagnostic-reset control");
}

std::size_t recorded_bytes = 0;
void record_bytes(std::size_t bytes) { recorded_bytes += bytes; }

struct AllocationWindow {
    const std::size_t before = global_allocation::allocations;
    const bool previous_recording = global_allocation::recording;
    void (*const previous_callback)(std::size_t) = global_allocation::on_recorded;
    AllocationWindow() {
        recorded_bytes = 0;
        global_allocation::on_recorded = record_bytes;
        global_allocation::recording = true;
    }
    ~AllocationWindow() {
        global_allocation::recording = previous_recording;
        global_allocation::on_recorded = previous_callback;
    }
};

void directory_is_reserved_once_before_any_write(std::size_t page_bytes) {
    ArenaAttachment attachment(arena_for_store_chunks(4));
    const auto capacity = share_capacity(ScratchArena::attached()->stats().bytes_supplied);
    auto backing = make_command_scratch_backing(page_bytes);
    std::vector<std::byte> image(page_bytes);
    const auto pages = capacity / page_bytes;
    ScratchStatus status = ScratchStatus::ok;
    std::size_t allocations = 0, bytes = 0;
    {
        // Includes the FIRST write, every chunk boundary, mapping, zero fill,
        // and the final page at K. The bound is ZERO allocations/bytes for all
        // userspace work during these writes, not merely the directory.
        AllocationWindow window;
        for (ScratchPageId page = 0; page < pages; ++page) {
            image[0] = static_cast<std::byte>(page % 251);
            status = backing->write_page(page, {image.data(), image.size()});
            if (status != ScratchStatus::ok) break;
        }
        allocations = global_allocation::allocations - window.before;
        bytes = recorded_bytes;
    }
    require(status == ScratchStatus::ok && backing->page_count() == pages,
            "allocation proof writes every page through K");
    require(allocations == 0 && bytes == 0,
            "bounded directory never allocates or reallocates during writes");
    std::vector<std::byte> read(page_bytes);
    for (ScratchPageId page = 0; page < pages; ++page) {
        require(backing->read_page(page, {read.data(), read.size()}) == ScratchStatus::ok
                && read[0] == static_cast<std::byte>(page % 251),
                "allocation witness really wrote distinct positional pages");
    }
    backing.reset();

    // Truncation to zero must retain the construction-time directory reserve.
    ScratchFile bounded;
    bounded.open_bounded("command-bounded-truncate", capacity);
    bounded.ensure_size(capacity);
    bounded.truncate(0);
    {
        AllocationWindow window;
        bounded.ensure_size(capacity);
        allocations = global_allocation::allocations - window.before;
        bytes = recorded_bytes;
    }
    require(allocations == 0 && bytes == 0,
            "bounded truncate/reuse retains its fixed directory");
}

std::size_t open_descriptors() {
    DIR* directory = ::opendir("/dev/fd");
    require(directory != nullptr, "enumerate child descriptors");
    std::size_t count = 0;
    while (const auto* entry = ::readdir(directory))
        if (entry->d_name[0] != '.') ++count;
    require(::closedir(directory) == 0, "close descriptor enumeration");
    return count;
}

struct IoLog {
    std::array<ScratchIoCharge, 16> charges{};
    std::size_t count = 0;
    static void charge(void* context, const ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<IoLog*>(context);
        if (self.count < self.charges.size()) self.charges[self.count] = io;
        ++self.count;
    }
    void expect(std::size_t index, ScratchIoOperation operation, ScratchPageId page,
                std::uint64_t bytes, ScratchStatus status) const {
        require(index < count && count <= charges.size(), "observer records every attempted operation");
        const auto& io = charges[index];
        const auto backend = ScratchArena::attached()->stats().backend;
        require(io.kind == (backend == ScratchArenaBackend::sealed_memfd
                                ? ScratchBackingKind::memfd : ScratchBackingKind::disk)
                && io.operation == operation && io.page == page
                && io.bytes == bytes && io.status == status,
                "arena observer retains exact kind, operation, position, byte count and status");
    }
};

void zero_fsize_child(std::size_t page_bytes) {
    ArenaAttachment attachment(arena_for_store_chunks(3));
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    ::sigemptyset(&ignored.sa_mask);
    require(::sigaction(SIGXFSZ, &ignored, nullptr) == 0, "child ignores SIGXFSZ");
    struct rlimit limit{};
    require(::getrlimit(RLIMIT_FSIZE, &limit) == 0, "read child's file-size limit");
    limit.rlim_cur = 0;
    require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "child enforces RLIMIT_FSIZE=0");
    const auto descriptors = open_descriptors();
    IoLog charges;
    ObserverRegistration observation(ScratchIoObserver{&charges, IoLog::charge});
    auto previous = set_command_scratch_byte_cap(4 * page_bytes);
    {
        auto a = make_command_scratch_backing(page_bytes);
        auto b = make_command_scratch_backing(page_bytes);
        std::vector<std::byte> first(page_bytes, std::byte{31}), second(page_bytes, std::byte{47});
        std::vector<std::byte> read(page_bytes);
        require(a->write_page(2, {first.data(), first.size()}) == ScratchStatus::ok,
                "attached command backing grows under zero fsize without ftruncate/pwrite");
        require(a->read_page(0, {read.data(), read.size()}) == ScratchStatus::ok
                && std::all_of(read.begin(), read.end(), [](std::byte b) { return b == std::byte{0}; }),
                "sparse arena pages are zero-filled");
        require(b->write_page(0, {second.data(), second.size()}) == ScratchStatus::ok,
                "second backing has an exclusive arena region");
        require(a->read_page(2, {read.data(), read.size()}) == ScratchStatus::ok && read == first,
                "second region does not overwrite first region");
        require(b->read_page(0, {read.data(), read.size()}) == ScratchStatus::ok && read == second,
                "second region preserves exact bytes");
        require(a->write_page(3, {first.data(), first.size()}) == ScratchStatus::io_error
                && a->page_count() == 3 && a->carried_failure() == nullptr,
                "arena mode preserves shared byte-cap refusal and logical extent");
        a.reset();
        require(b->write_page(2, {second.data(), second.size()}) == ScratchStatus::ok,
                "destroying the first backing releases its shared budget");
        require(b->sync(ScratchDurability::buffered) == ScratchStatus::ok,
                "arena supports buffered sync");
        require(b->sync(ScratchDurability::data) == ScratchStatus::programming_error
                && b->sync(ScratchDurability::full) == ScratchStatus::programming_error,
                "arena refuses unsupported durability promises");
        require(open_descriptors() == descriptors, "arena command stores open no private files");
    }
    require(charges.count == 10, "each successful or refused arena operation charges exactly once");
    charges.expect(0, ScratchIoOperation::write_page, 2, page_bytes, ScratchStatus::ok);
    charges.expect(1, ScratchIoOperation::read_page, 0, page_bytes, ScratchStatus::ok);
    charges.expect(2, ScratchIoOperation::write_page, 0, page_bytes, ScratchStatus::ok);
    charges.expect(3, ScratchIoOperation::read_page, 2, page_bytes, ScratchStatus::ok);
    charges.expect(4, ScratchIoOperation::read_page, 0, page_bytes, ScratchStatus::ok);
    charges.expect(5, ScratchIoOperation::write_page, 3, page_bytes, ScratchStatus::io_error);
    charges.expect(6, ScratchIoOperation::write_page, 2, page_bytes, ScratchStatus::ok);
    charges.expect(7, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::ok);
    charges.expect(8, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::programming_error);
    charges.expect(9, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::programming_error);
    require(ScratchArena::attached()->stats().bytes_used == 0,
            "zero-fsize destruction releases both regions");
    set_command_scratch_byte_cap(std::uint64_t{0});
    {
        auto backing = make_command_scratch_backing(page_bytes);
        const auto stats = ScratchArena::attached()->stats();
        std::vector<std::byte> image(page_bytes);
        const auto page = stats.bytes_total / page_bytes;
        require(backing->write_page(page, {image.data(), image.size()})
                    == ScratchStatus::io_error && backing->carried_failure(),
                "zero-fsize exhaustion remains a typed io_error");
        const auto failed = ScratchArena::attached()->stats();
        exhaustion_args(*backing->carried_failure(), failed,
                        required_arena((page + 1) * page_bytes));
        require(backing->page_count() == 0 && failed.bytes_used == 0,
                "refused sparse growth claims neither pages nor chunks");
        require(charges.count == 11, "typed exhaustion also charges exactly once");
        charges.expect(10, ScratchIoOperation::write_page, page, page_bytes, ScratchStatus::io_error);
    }
    set_command_scratch_byte_cap(std::move(previous));
    struct stat status{};
    require(::fstat(attachment.fd, &status) == 0
            && static_cast<std::uint64_t>(status.st_size)
                == ScratchArena::attached()->stats().bytes_supplied,
            "command backings never resize the inherited arena fd");
}

void attached_arena_with_zero_fsize(std::size_t page_bytes) {
    const pid_t child = ::fork();
    require(child >= 0, "fork zero-fsize fixture");
    if (child == 0) {
        try {
            zero_fsize_child(page_bytes);
            ::_exit(0);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL zero-fsize child: %s\n", error.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "attached arena succeeds in the isolated zero-fsize child");
}
} // namespace

int main() {
    try {
        carried_failure_and_budget_rollback();
        const long os_page = ::sysconf(_SC_PAGESIZE);
        require(os_page > 0 && ScratchArena::kChunkBytes % os_page == 0,
                "OS page size divides an arena chunk");
        share_refuses_before_claim_and_releases_budget(kPageBytes);
        share_refuses_before_claim_and_releases_budget(static_cast<std::size_t>(os_page));
        real_store_crosses_share_and_replacement_fits();
        reported_placement_and_shortfall_negative_witness();
        zero_share_and_saturating_arithmetic();
        uncertified_default_sized_arena(static_cast<std::size_t>(os_page));
        store_retains_typed_failure_across_rollback();
        directory_is_reserved_once_before_any_write(kPageBytes);
        directory_is_reserved_once_before_any_write(static_cast<std::size_t>(os_page));
        attached_arena_with_zero_fsize(kPageBytes);
        attached_arena_with_zero_fsize(static_cast<std::size_t>(os_page));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL bounded command arena: %s\n", error.what());
        return 1;
    }
}
