// CR-397 E1: real factory, real unlinked files, forwarding Linux syscall
// wrappers, and the same logical ScratchIoObserver registration as CR-257.
// No local execution/fail-before is claimed. Against 30e9f9a0 the construction
// overload used below is absent; the supervisor records the first diagnostic.
#include "../src/command_scratch_cache.hpp"
#include "command_scratch_arena_share_fixture.hpp"
#include "command_scratch_extent_probe.hpp"
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace spill_test {
struct Physical {
    std::uint64_t reads = 0, writes = 0, read_bytes = 0, write_bytes = 0;
    std::uint64_t data_syncs = 0, full_syncs = 0, errors = 0, short_io = 0;
};
Physical physical;
bool measuring = false;
std::uint64_t fail_write_at = 0;
int write_error = 0; // Zero selects a real short write instead of errno.
bool fail_next_array = false;
std::uint64_t allocation_faults = 0;
}

extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" int __real_fdatasync(int);
extern "C" int __real_fsync(int);
// Itanium C++ ABI: operator new[](size_t, const std::nothrow_t&), Linux LP64.
// Forwarding to the real allocator keeps every delete matched under ASan.
extern "C" void* __real__ZnamRKSt9nothrow_t(std::size_t, const std::nothrow_t&) noexcept;
extern "C" void* __wrap__ZnamRKSt9nothrow_t(
        std::size_t size, const std::nothrow_t& tag) noexcept {
    if (spill_test::fail_next_array) {
        spill_test::fail_next_array = false; // Error construction can allocate.
        ++spill_test::allocation_faults;
        return nullptr;
    }
    return __real__ZnamRKSt9nothrow_t(size, tag);
}
extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t size, off_t offset) {
    const auto result = __real_pread(fd, data, size, offset);
    if (spill_test::measuring) {
        auto& io = spill_test::physical;
        ++io.reads;
        io.read_bytes += size;
        io.errors += result < 0;
        io.short_io += result >= 0 && static_cast<std::size_t>(result) != size;
    }
    return result;
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    using namespace spill_test;
    ssize_t result;
    if (measuring && fail_write_at != 0 && physical.writes + 1 == fail_write_at) {
        if (write_error != 0) {
            errno = write_error;
            result = -1;
        } else {
            result = __real_pwrite(fd, data, size / 2, offset);
        }
    } else {
        result = __real_pwrite(fd, data, size, offset);
    }
    if (measuring) {
        ++physical.writes;
        physical.write_bytes += size;
        physical.errors += result < 0;
        physical.short_io += result >= 0 && static_cast<std::size_t>(result) != size;
    }
    return result;
}
extern "C" int __wrap_fdatasync(int fd) {
    const auto result = __real_fdatasync(fd);
    if (spill_test::measuring) {
        ++spill_test::physical.data_syncs;
        spill_test::physical.errors += result != 0;
    }
    return result;
}
extern "C" int __wrap_fsync(int fd) {
    const auto result = __real_fsync(fd);
    if (spill_test::measuring) {
        ++spill_test::physical.full_syncs;
        spill_test::physical.errors += result != 0;
    }
    return result;
}

namespace spill_test {
using namespace pineforge::detail;
using command_arena_test::require;
constexpr std::size_t page = 512;

struct Window {
    Window() { require(!measuring, "no nested physical measurement"); measuring = true; }
    ~Window() { measuring = false; }
};

struct Logical {
    std::uint64_t reads = 0, writes = 0, syncs = 0, failures = 0;
    command_scratch_test::CommandScratchExtentProbe extent;
    static void charge(void* context, const ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<Logical*>(context);
        self.extent.observe(io);
        self.reads += io.operation == ScratchIoOperation::read_page;
        self.writes += io.operation == ScratchIoOperation::write_page;
        self.syncs += io.operation == ScratchIoOperation::sync;
        self.failures += io.status != ScratchStatus::ok;
    }
    ScratchIoObserver observer() noexcept { return {this, &charge}; }
};

struct Cap {
    CommandScratchByteCap previous;
    explicit Cap(std::uint64_t bytes) : previous(set_command_scratch_byte_cap(bytes)) {}
    ~Cap() { set_command_scratch_byte_cap(std::move(previous)); }
};

struct DiskDirectory {
    std::string path = "./command-spill-XXXXXX";
    std::string previous;
    std::string previous_cap;
    bool had_previous = false;
    bool had_cap = false;
    DiskDirectory() {
        require(::mkdtemp(path.data()) != nullptr, "create disk scratch test directory");
        if (const auto* value = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR")) {
            had_previous = true;
            previous = value;
        }
        require(::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", path.c_str(), 1) == 0,
                "select real disk scratch directory");
        if (const auto* value = std::getenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES")) {
            had_cap = true;
            previous_cap = value;
        }
        require(::unsetenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES") == 0, "clear inherited cap default");
    }
    ~DiskDirectory() {
        if (had_previous) ::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", previous.c_str(), 1);
        else ::unsetenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        if (had_cap) ::setenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", previous_cap.c_str(), 1);
        else ::unsetenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES");
        ::rmdir(path.c_str());
    }
};

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> value(size);
    for (std::size_t i = 0; i < size; ++i)
        value[i] = static_cast<std::byte>((i * 13u + seed * 29u) & 0xffu);
    return value;
}
ScratchBytes bytes(const std::vector<std::byte>& value) { return {value.data(), value.size()}; }
ScratchBuffer buffer(std::vector<std::byte>& value) { return {value.data(), value.size()}; }

void no_physical_io(const Physical& io) {
    require(io.reads == 0 && io.writes == 0 && io.data_syncs == 0 && io.full_syncs == 0
            && io.errors == 0 && io.short_io == 0, "resident pages issue no physical page I/O or sync");
}

// Compare public reads against an all-file backing after the same sequence.
// Include a sparse gap, overwrites, the exact last resident page, first spill,
// a sparse extension after spill, cache collisions, and all durability modes.
void spill_boundary_matches_file(std::size_t geometry, bool production_cap) {
    const auto cap_bytes = production_cap ? CachedScratchBacking::kCacheBytes : 4 * geometry;
    const ScratchPageId pages = cap_bytes / geometry;
    require(pages >= 4, "boundary fixture needs a gap and four resident pages");
    Logical logical;
    std::unique_ptr<ScratchBacking> candidate;
    {
        command_arena_test::ObserverRegistration observer(logical.observer());
        candidate = production_cap ? make_command_scratch_backing(geometry)
                                   : make_command_scratch_backing(geometry, cap_bytes);
    }
    auto reference = make_command_scratch_backing(geometry, 0);
    auto first = pattern(geometry, 1), second = pattern(geometry, 2);
    std::vector<std::byte> actual(geometry), expected(geometry);
    physical = {};
    const auto put = [&](ScratchPageId id, const std::vector<std::byte>& image) {
        ScratchStatus status;
        { Window window; status = candidate->write_page(id, bytes(image)); }
        require(status == ScratchStatus::ok
                && reference->write_page(id, bytes(image)) == status, "same successful page write");
        require(candidate->page_count() == reference->page_count(), "same logical extent");
    };
    const auto read = [&](ScratchPageId id) {
        std::fill(actual.begin(), actual.end(), std::byte{0x31});
        std::fill(expected.begin(), expected.end(), std::byte{0x31});
        ScratchStatus status;
        { Window window; status = candidate->read_page(id, buffer(actual)); }
        const auto other = reference->read_page(id, buffer(expected));
        require(status == other && actual == expected, "reads match all-file bytes and status");
    };
    read(0); // Never written, not_found, and destination preserved.
    put(0, first);
    put(pages - 1, second);
    put(0, second);
    for (ScratchPageId id = 0; id <= pages; ++id) read(id);
    for (auto durability : {ScratchDurability::buffered, ScratchDurability::data,
                            ScratchDurability::full}) {
        ScratchStatus status;
        { Window window; status = candidate->sync(durability); }
        require(status == ScratchStatus::ok && reference->sync(durability) == status,
                "resident sync retains the all-file status");
    }
    no_physical_io(physical);
    require(logical.writes == 3 && logical.reads == pages + 2 && logical.syncs == 3
            && logical.failures == 2, "memory hits and absent reads all remain logical charges");
    require(logical.extent.high_water_bytes == candidate->page_count() * geometry
            && logical.extent.high_water_bytes == pages * geometry
            && logical.extent.successful_writes == logical.writes
            && logical.extent.failed_writes == 0 && !logical.extent.invalid_extent,
            "write charges recover the exact resident extent, ignoring absent reads");

    put(pages, first);
    require(physical.writes == pages + 1 && physical.write_bytes == (pages + 1) * geometry
            && physical.reads == 0 && physical.errors == 0 && physical.short_io == 0,
            "first spill writes the complete prefix and requested page exactly once");
    require(logical.writes == 4, "spill migration adds no logical writes");
    require(logical.extent.high_water_bytes == (pages + 1) * geometry
            && logical.extent.high_water_bytes == candidate->page_count() * geometry,
            "the same extent probe observes the first spilled page");
    for (ScratchPageId id = 0; id <= pages + 1; ++id) read(id);
    require(physical.reads > 0, "spilled pages really read from the file");
    // Evict every low slot with a high sparse page before reading it again.
    const auto slots = CachedScratchBacking::kCacheBytes / geometry;
    for (ScratchPageId id = 0; id <= pages; ++id) {
        put(slots * 2 + id, second);
        read(id);
    }
    for (ScratchPageId id = pages + 1; id < slots * 2; ++id) read(id);
    put(0, first);
    read(0);
    require(logical.extent.high_water_bytes == candidate->page_count() * geometry
            && logical.extent.successful_writes == logical.writes
            && logical.extent.failed_writes == 0 && !logical.extent.invalid_extent,
            "sparse growth and low overwrites retain the exact high-water extent");
    physical = {};
    for (auto durability : {ScratchDurability::buffered, ScratchDurability::data,
                            ScratchDurability::full}) {
        ScratchStatus status;
        { Window window; status = candidate->sync(durability); }
        require(status == ScratchStatus::ok && reference->sync(durability) == status,
                "spilled sync retains the all-file status");
    }
    require(physical.data_syncs == 1 && physical.full_syncs == 1
            && physical.reads == 0 && physical.writes == 0 && physical.errors == 0,
            "spilled data/full sync reach the real fdatasync/fsync");
}

void extent_probe_covers_backing_lifetimes() {
    Logical previous;
    command_arena_test::ObserverRegistration observer(previous.observer());
    const auto small_image = pattern(page, 21), large_image = pattern(4096, 22);
    const ScratchPageId high_page = CachedScratchBacking::kCacheBytes / page + 3;
    const auto high_bytes = (high_page + 1) * page;
    {
        command_scratch_test::ScopedCommandScratchExtentProbe probe;
        {
            auto small = make_command_scratch_backing(page);
            auto large = make_command_scratch_backing(4096);
            auto file = make_command_scratch_backing(page, 0);
            require(small->write_page(1, bytes(small_image)) == ScratchStatus::ok
                    && large->write_page(1, bytes(large_image)) == ScratchStatus::ok
                    && probe.extent.high_water_bytes == large->page_count() * 4096,
                    "one registration observes both production page geometries");
            require(file->write_page(high_page, bytes(small_image)) == ScratchStatus::ok
                    && probe.extent.high_water_bytes == file->page_count() * page
                    && probe.extent.high_water_bytes == high_bytes,
                    "the probe reports the largest store extent, including sparse holes");
            std::vector<std::byte> out(page);
            require(file->read_page(high_page + 1000, buffer(out)) == ScratchStatus::not_found
                    && file->write_page(scratch_no_page, bytes(small_image))
                        == ScratchStatus::programming_error,
                    "absent reads and failed writes do not manufacture successful extent");
        }
        require(probe.extent.high_water_bytes == high_bytes
                && probe.extent.successful_writes == 3 && probe.extent.failed_writes == 1
                && !probe.extent.invalid_extent
                && previous.writes == 4 && previous.reads == 1 && previous.failures == 2,
                "the peak survives teardown and every charge reaches the previous observer");
    }
    auto restored = make_command_scratch_backing(page);
    require(restored->write_page(0, bytes(small_image)) == ScratchStatus::ok
            && previous.writes == 5,
            "probe teardown restores the previous observer for later factory calls");
}

void allocation_failure_is_typed() {
    require(!fail_next_array, "allocation fault starts disarmed");
    const auto before = allocation_faults;
    bool caught = false;
    fail_next_array = true;
    try {
        auto backing = make_command_scratch_backing(page);
    } catch (const std::system_error& error) {
        caught = error.code() == std::error_code(ENOMEM, std::generic_category())
            && pineforge::classify_run_failure(error).code == pineforge::RunFailureCode::scratch_io_failed;
    }
    fail_next_array = false;
    require(caught && allocation_faults == before + 1, "real page allocation fails typed with ENOMEM");
    auto retry = make_command_scratch_backing(page);
    const auto image = pattern(page, 3);
    physical = {};
    ScratchStatus status;
    { Window window; status = retry->write_page(0, bytes(image)); }
    require(status == ScratchStatus::ok, "factory remains usable after allocation failure");
    no_physical_io(physical);
}

void failed_spill_preserves_pages_and_budget() {
    for (const int error : {ENOSPC, EIO, 0}) {
        Cap cap(3 * page);
        auto backing = make_command_scratch_backing(page, 2 * page);
        const auto image = pattern(page, 4), replacement = pattern(page, 5);
        std::vector<std::byte> out(page);
        require(backing->write_page(0, bytes(image)) == ScratchStatus::ok
                && backing->write_page(1, bytes(replacement)) == ScratchStatus::ok,
                "seed two resident pages under the shared byte cap");
        physical = {};
        fail_write_at = 2;
        write_error = error;
        ScratchStatus status;
        { Window window; status = backing->write_page(2, bytes(image)); }
        fail_write_at = 0;
        require(status == ScratchStatus::io_error && physical.writes == 2
                && physical.errors == (error != 0) && physical.short_io == (error == 0)
                && backing->page_count() == 2,
                "error or real short spill does not publish an incomplete prefix");
        require(backing->read_page(0, buffer(out)) == ScratchStatus::ok && out == image
                && backing->read_page(1, buffer(out)) == ScratchStatus::ok && out == replacement
                && backing->read_page(2, buffer(out)) == ScratchStatus::not_found,
                "failed spill retains all old resident contents and extent");
        require(backing->write_page(0, bytes(replacement)) == ScratchStatus::ok,
                "resident prefix remains writable after failed spill");
        physical = {};
        { Window window; status = backing->sync(ScratchDurability::data); }
        require(status == ScratchStatus::ok && physical.data_syncs == 1 && physical.errors == 0,
                "sync still reaches the file after a partial spill");
        auto other = make_command_scratch_backing(page);
        require(other->write_page(0, bytes(image)) == ScratchStatus::ok,
                "failed spill releases its reserved extension budget");
        physical = {};
        { Window window; status = backing->write_page(2, bytes(image)); }
        require(status == ScratchStatus::io_error, "other resident extent consumes the shared budget");
        no_physical_io(physical);
        other.reset();
        physical = {};
        { Window window; status = backing->write_page(2, bytes(image)); }
        require(status == ScratchStatus::ok && physical.writes == 3
                && physical.errors == 0 && physical.short_io == 0,
                "retry rewrites the entire prefix then the requested page");
        require(backing->read_page(0, buffer(out)) == ScratchStatus::ok && out == replacement
                && backing->read_page(1, buffer(out)) == ScratchStatus::ok && out == replacement
                && backing->read_page(2, buffer(out)) == ScratchStatus::ok && out == image,
                "retry preserves every resident edit and the new page");
        backing.reset();
        auto reused = make_command_scratch_backing(page);
        require(reused->write_page(2, bytes(image)) == ScratchStatus::ok,
                "destruction releases resident and spilled logical byte charges");
    }
}

void store_commit_rollback_reopen_matches_file() {
    auto backing = make_command_scratch_backing(page, 4 * page);
    auto reference = make_command_scratch_backing(page, 0);
    auto config = command_arena_test::config();
    std::array<ScratchHandle, 12> handles{}, expected_handles{};
    const auto payload = pattern(2 * page, 6);
    {
        CommandScratchStore store(config, backing.get()), oracle(config, reference.get());
        require(store.status() == ScratchStatus::ok && oracle.status() == ScratchStatus::ok,
                "open both command stores");
        for (std::uint64_t key = 0; key < handles.size(); ++key) {
            require(store.begin() == ScratchStatus::ok && oracle.begin() == ScratchStatus::ok,
                    "begin both store transactions");
            ScratchSpanSource k({reinterpret_cast<const std::byte*>(&key), sizeof key});
            ScratchSpanSource value(bytes(payload));
            require(store.put(k, value, handles[key]) == ScratchStatus::ok
                    && oracle.put(k, value, expected_handles[key]) == ScratchStatus::ok
                    && handles[key] == expected_handles[key], "identical stored handle allocation");
            if (key % 3 == 1) {
                require(store.rollback() == ScratchStatus::ok && oracle.rollback() == ScratchStatus::ok,
                        "rollback on both sides of the spill");
                handles[key] = expected_handles[key] = scratch_no_handle;
            } else {
                require(store.commit(ScratchDurability::buffered) == ScratchStatus::ok
                        && oracle.commit(ScratchDurability::buffered) == ScratchStatus::ok,
                        "commit both stores");
            }
        }
        require(backing->page_count() > 4 && store.record_count() == 8
                && store.record_count() == oracle.record_count(), "real store workload crossed the cap");
    }
    config.mode = ScratchOpenMode::reopen;
    CommandScratchStore store(config, backing.get()), oracle(config, reference.get());
    require(store.status() == ScratchStatus::ok && oracle.status() == ScratchStatus::ok,
            "reopen both live backings");
    std::vector<std::byte> out(payload.size()), expected(payload.size());
    for (std::size_t key = 0; key < handles.size(); ++key) {
        if (handles[key] == scratch_no_handle) continue;
        require(store.read(handles[key], ScratchField::value, 0, buffer(out)) == ScratchStatus::ok
                && oracle.read(expected_handles[key], ScratchField::value, 0, buffer(expected))
                    == ScratchStatus::ok && out == expected && out == payload,
                "reopened record bytes match the all-file store");
    }
}

void store_rolls_back_failed_spill() {
    auto backing = make_command_scratch_backing(page, 64 * page);
    auto reference = make_command_scratch_backing(page, 0);
    const auto config = command_arena_test::config();
    CommandScratchStore store(config, backing.get()), oracle(config, reference.get());
    const auto seed = pattern(32, 8), large = pattern(65 * page, 9);
    const auto put = [](CommandScratchStore& target, std::uint64_t key,
                        const std::vector<std::byte>& image, ScratchHandle& handle) {
        ScratchSpanSource k({reinterpret_cast<const std::byte*>(&key), sizeof key});
        ScratchSpanSource value(bytes(image));
        return target.put(k, value, handle);
    };
    ScratchHandle handle = scratch_no_handle, expected_handle = scratch_no_handle;
    require(store.status() == ScratchStatus::ok && oracle.status() == ScratchStatus::ok
            && store.begin() == ScratchStatus::ok && oracle.begin() == ScratchStatus::ok
            && put(store, 1, seed, handle) == ScratchStatus::ok
            && put(oracle, 1, seed, expected_handle) == ScratchStatus::ok
            && store.commit(ScratchDurability::buffered) == ScratchStatus::ok
            && oracle.commit(ScratchDurability::buffered) == ScratchStatus::ok,
            "commit the control record before the failed transaction");
    require(backing->page_count() > 0 && backing->page_count() < 64,
            "committed control really fits the resident prefix");
    require(store.begin() == ScratchStatus::ok, "begin the transaction that must spill");
    physical = {};
    fail_write_at = 2;
    write_error = EIO;
    ScratchHandle failed = scratch_no_handle;
    ScratchStatus status;
    { Window window; status = put(store, 2, large, failed); }
    fail_write_at = 0;
    require(status == ScratchStatus::io_error && physical.writes == 2 && physical.errors == 1,
            "real command-store put reached the injected spill fault");
    require(store.rollback() == ScratchStatus::ok && store.record_count() == 1,
            "command-store undo succeeds while failed-spill pages remain resident");
    std::vector<std::byte> out(seed.size());
    require(store.read(handle, ScratchField::value, 0, buffer(out)) == ScratchStatus::ok && out == seed,
            "rollback preserves the previously committed record");
    std::array<std::byte, page> digest_page{};
    std::uint64_t digest = 0, expected_digest = 0;
    require(store.state_digest({digest_page.data(), digest_page.size()}, digest) == ScratchStatus::ok
            && oracle.state_digest({digest_page.data(), digest_page.size()}, expected_digest)
                == ScratchStatus::ok && digest == expected_digest,
            "failed spill and rollback leave the same logical state as all-file");
    require(store.begin() == ScratchStatus::ok && oracle.begin() == ScratchStatus::ok
            && put(store, 2, large, handle) == ScratchStatus::ok
            && put(oracle, 2, large, expected_handle) == ScratchStatus::ok
            && store.commit(ScratchDurability::buffered) == ScratchStatus::ok
            && oracle.commit(ScratchDurability::buffered) == ScratchStatus::ok,
            "the same large transaction succeeds after the fault is removed");
    out.resize(large.size());
    std::vector<std::byte> expected(large.size());
    require(store.read(handle, ScratchField::value, 0, buffer(out)) == ScratchStatus::ok
            && oracle.read(expected_handle, ScratchField::value, 0, buffer(expected)) == ScratchStatus::ok
            && out == expected && out == large, "retry reads every large-record byte from the spilled file");
}

std::string arena_failure(bool resident, bool use_default) {
    auto inner = use_default ? make_command_scratch_backing(page)
                             : make_command_scratch_backing(page, 0);
    std::unique_ptr<ScratchBacking> backing = std::move(inner);
    if (resident)
        backing = std::make_unique<CachedScratchBacking>(
            std::move(backing), ScratchIoObserver{}, 2 * page);
    const auto capacity = backing->arena_capacity_bytes();
    require(capacity && *capacity == ScratchArena::kChunkBytes, "same fixed small-arena capacity");
    const auto image = pattern(page, 7);
    require(backing->write_page(0, bytes(image)) == ScratchStatus::ok
            && backing->write_page(1, bytes(image)) == ScratchStatus::ok, "seed bounded arena pages");
    require(backing->write_page(*capacity / page, bytes(image)) == ScratchStatus::io_error,
            "spilling beyond arena capacity returns io_error");
    const auto* error = backing->carried_failure();
    require(error && error->run_failure_code() == pineforge::RunFailureCode::report_sink_failed
            && error->run_failure().args, "spill retains the typed original arena exception");
    const auto args = *error->run_failure().args;
    require(args.find("\"reason\":\"arena_exhausted\"") != std::string::npos
            && args.find("\"store\":\"command\"") != std::string::npos
            && backing->page_count() == 2, "same exhaustion vocabulary and logical extent");
    bool caught = false;
    try {
        rethrow_carried_scratch_failure(error);
    } catch (const ReportScratchIoError& carried) {
        caught = carried.run_failure_code() == pineforge::RunFailureCode::report_sink_failed
            && carried.run_failure().args && *carried.run_failure().args == args;
    }
    require(caught, "owner rethrows the original typed arena verdict");
    std::vector<std::byte> out(page);
    require(backing->read_page(0, buffer(out)) == ScratchStatus::ok && out == image,
            "arena refusal preserves existing page bytes");
    return args;
}

void spill_arena_exhaustion_is_typed() {
    command_arena_test::ArenaAttachment attachment(command_arena_test::kSmallArenaBytes);
    const auto all_file = arena_failure(false, false);
    require(ScratchArena::attached()->stats().bytes_used == 0, "all-file arena region released");
    const auto front = arena_failure(true, false);
    require(front == all_file && ScratchArena::attached()->stats().bytes_used == 0,
            "resident-to-arena spill preserves every typed argument and releases its region");
    require(arena_failure(false, true) == all_file,
            "default arena route retains its existing direct claim and failure policy");
}
} // namespace spill_test

int main() {
    try {
        spill_test::DiskDirectory directory;
        spill_test::Cap unlimited(0);
        spill_test::spill_boundary_matches_file(512, false);
        spill_test::spill_boundary_matches_file(1536, false);
        spill_test::spill_boundary_matches_file(4096, false);
        spill_test::spill_boundary_matches_file(4096, true);
        std::puts("PASS spill_boundary_matches_file");
        spill_test::extent_probe_covers_backing_lifetimes();
        std::puts("PASS extent_probe_covers_backing_lifetimes");
        spill_test::allocation_failure_is_typed();
        std::puts("PASS allocation_failure_is_typed");
        spill_test::failed_spill_preserves_pages_and_budget();
        std::puts("PASS failed_spill_preserves_pages_and_budget");
        spill_test::store_commit_rollback_reopen_matches_file();
        std::puts("PASS store_commit_rollback_reopen_matches_file");
        spill_test::store_rolls_back_failed_spill();
        std::puts("PASS store_rolls_back_failed_spill");
        spill_test::spill_arena_exhaustion_is_typed();
        std::puts("PASS spill_arena_exhaustion_is_typed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_command_scratch_spill: %s\n", error.what());
        return 1;
    }
}
