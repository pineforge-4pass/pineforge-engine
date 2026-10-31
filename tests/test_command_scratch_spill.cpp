// Resident command scratch: real factory, real unlinked files, forwarding Linux
// syscall wrappers, and the same logical ScratchIoObserver registration as the
// source-exit scratch I/O test. Before the resident prefix existed, the
// construction overload used below was absent.
#include "../src/command_scratch_cache.hpp"
#include "command_scratch_arena_share_fixture.hpp"
#include "command_scratch_extent_probe.hpp"
#include "command_scratch_cache_run_fixture.hpp"
#include "global_allocation_replacement.hpp"
#include <pineforge/pineforge.h>
#include <pineforge/detail/script_store.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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
std::size_t allocation_fault_bytes = 0;
std::uint64_t array_allocation_attempts = 0;
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
    ++spill_test::array_allocation_attempts;
    if (spill_test::fail_next_array) {
        spill_test::fail_next_array = false; // Error construction can allocate.
        ++spill_test::allocation_faults;
        spill_test::allocation_fault_bytes = size;
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

// This row uses only the previous constructor/factory API. On the unchanged
// engine the real allocation seam reports scratch_io_failed with ENOMEM.
void allocation_failure_bypasses_cache() {
    RunCacheDiagnostics diagnostics;
    RunCacheDiagnosticsScope diagnostics_scope(diagnostics);
    require(!fail_next_array, "allocation fault starts disarmed");
    const auto before = allocation_faults;
    std::unique_ptr<ScratchBacking> backing;
    fail_next_array = true;
    try {
        backing = make_command_scratch_backing(page);
    } catch (const std::system_error& error) {
        fail_next_array = false;
        if (error.code() == std::error_code(ENOMEM, std::generic_category())
            && pineforge::classify_run_failure(error).code == pineforge::RunFailureCode::scratch_io_failed)
            throw std::runtime_error(
                "optional command cache allocation must bypass; got scratch_io_failed/ENOMEM");
        throw;
    }
    fail_next_array = false;
    require(backing && allocation_faults == before + 1,
            "bypass fixture refused exactly one real page allocation");
    const auto& cached = dynamic_cast<const CachedScratchBacking&>(*backing);
    require(cached.read_cache_stats().allocation_bypass_count == 1
            && cached.cache_slot_count() == 0 && cached.cache_storage_bytes() == 0,
            "allocation bypass is visible immediately and holds no slab");
    auto healthy = make_command_scratch_backing(page);
    auto reference = make_command_scratch_backing(page, 0);
    auto disabled = make_command_scratch_backing(page, 0, 0);
    require(dynamic_cast<const CachedScratchBacking&>(*healthy)
                .read_cache_stats().allocation_bypass_count == 0
            && dynamic_cast<const CachedScratchBacking&>(*disabled)
                .read_cache_stats().allocation_bypass_count == 0,
            "healthy and explicitly disabled caches report no allocation failure");
    const auto expected = command_cache_test::run(*healthy);
    physical = {};
    std::vector<std::byte> actual;
    { Window window; actual = command_cache_test::run(*backing); }
    require(actual == expected && actual == command_cache_test::run(*reference),
            "allocation bypass completes with identical full records, handles and continuation bytes");
    require(physical.reads != 0 && physical.writes != 0
            && physical.errors == 0 && physical.short_io == 0,
            "allocation bypass really reads and writes its inner file");
    const auto attempts = array_allocation_attempts;
    const auto reads = physical.reads;
    std::array<std::byte, page> repeated{};
    for (unsigned i = 0; i != 4; ++i) {
        Window window;
        require(backing->read_page(0, {repeated.data(), repeated.size()}) == ScratchStatus::ok,
                "repeated bypass reads continue through the backing");
    }
    const auto stats = cached.read_cache_stats();
    require(array_allocation_attempts == attempts && allocation_faults == before + 1
            && physical.reads == reads + 4 && stats.allocation_bypass_count == 1
            && stats.hits == 0 && stats.current_bytes == 0 && stats.peak_bytes == 0,
            "bypass stays off without allocation retries or duplicate diagnostics");
    require(diagnostics.allocation_bypass_count() == 1,
            "the slab's failed core borrow must not report a second production bypass");
    std::uint64_t command_bytes = 0;
    for (const auto* owner : {backing.get(), healthy.get(), reference.get(), disabled.get()})
        command_bytes += sizeof(CachedScratchBacking)
            + dynamic_cast<const CachedScratchBacking&>(*owner).cache_storage_bytes();
    require(diagnostics.resident_bytes() == command_bytes,
            "the production sink counts each command slab once across prefix and core use");
    {
        pineforge::TradeLedger ledger(std::size_t{3});
        ledger.set_scratch_directory(".");
        pineforge::Trade row{};
        row.entry_price = 100;
        ledger.push_back(row);
        const auto refused = global_allocation::refused;
        require(global_allocation::fail_after == std::numeric_limits<std::size_t>::max(),
                "ledger aggregation fault starts disarmed");
        global_allocation::fail_after = 0;
        const auto price = ledger.entry_price_at(0);
        const bool consumed = global_allocation::fail_after
            == std::numeric_limits<std::size_t>::max();
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        require(consumed && global_allocation::refused == refused + 1
                && price == 100 && ledger.cache_allocation_failed(),
                "aggregation really refuses the optional ledger directory and preserves its read");
        require(diagnostics.allocation_bypass_count() == 2
                && diagnostics.resident_bytes() == command_bytes + ledger.cache_resident_bytes(),
                "the same production sink sums command and ledger bypasses and resident owners once");
    }
    require(diagnostics.allocation_bypass_count() == 2
            && diagnostics.resident_bytes() == command_bytes,
            "released ledger storage detaches while its run's bypass event remains");
}

class AllocationHost final : public pineforge::NativeStrategyHost {
public:
    AllocationHost() {
        pineforge::NativeRunSpec spec;
        spec.identity = {"command-cache-allocation", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:CACHE";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000;
        spec.point_value = spec.account_fx = 1;
        spec.price_tick = 0.01;
        spec.close_execution = pineforge::NativeCloseExecution::AfterCalculation;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure the native cache-allocation run");
        set_broker_state_hash_recording(true);
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        namespace no = pineforge::native_order;
        const auto bar = callbacks++;
        if (bar % 2 != 0) return;
        no::Request request;
        if (bar % 4 == 0) request.intent = no::Transact{1.0};
        else request.intent = no::Flatten{};
        request.label = bar % 4 == 0 ? "entry" : "exit";
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle,
                "native cache-allocation run accepts every scheduled order");
    }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    unsigned callbacks = 0;
};

void append_double(std::vector<std::byte>& output, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof bits == sizeof value, "binary64 transcript");
    std::memcpy(&bits, &value, sizeof bits);
    command_cache_test::append_word(output, bits);
}

void append_string(std::vector<std::byte>& output, const std::string& value) {
    command_cache_test::append_word(output, value.size());
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    output.insert(output.end(), begin, begin + value.size());
}

std::vector<std::byte> native_allocation_run(bool fault) {
    AllocationHost host;
    const auto handle = static_cast<pf_strategy_t>(
        static_cast<pineforge::BacktestEngine*>(&host));
    std::uint64_t cache_bytes = 91, bypasses = 92;
    require(pf_report_cache_stats(handle, &cache_bytes, &bypasses) == 0
            && cache_bytes == 0 && bypasses == 0,
            "a configured handle has no admitted-run cache diagnostics");
    std::array<pineforge::Bar, 8> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double price = 100 + static_cast<double>(i);
        bars[i] = {price, price, price, price, 1,
                   INT64_C(1704067200000) + static_cast<std::int64_t>(i) * INT64_C(60000)};
    }
    const auto before = allocation_faults;
    fail_next_array = fault;
    try {
        host.run(bars.data(), static_cast<int>(bars.size()));
    } catch (const std::system_error& error) {
        fail_next_array = false;
        if (pineforge::classify_run_failure(error).code == pineforge::RunFailureCode::scratch_io_failed)
            throw std::runtime_error(
                "native run must complete after optional cache ENOMEM; got scratch_io_failed");
        throw;
    }
    fail_next_array = false;
    // RunCommandScratch's immutable geometry is 512 bytes. Preserve its
    // 64-KiB payload and include every shared-cache tag in the real request.
    const auto slab_bytes = CachedScratchBacking::kCacheBytes
        + (CachedScratchBacking::kCacheBytes / page)
            * (ReadThroughCache::line_bytes(page) - page);
    require(allocation_faults == before + (fault ? 1 : 0)
            && (!fault || allocation_fault_bytes == slab_bytes),
            "native allocation fixture refuses exactly the real payload-plus-metadata cache slab");
    require(pineforge::run_failure_value_of(host).code != pineforge::RunFailureCode::scratch_io_failed,
            "native run must complete after optional cache ENOMEM; got scratch_io_failed");
    require(host.last_run_status() == 0 && host.last_error().empty()
            && host.native_state().kind == pineforge::NativeLifecycleKind::Completed
            && host.callbacks == bars.size() && host.closed_trade_count() == 2
            && host.trade_count() == 2 && !host.recorded_hashes().empty(),
            "native cache-allocation run completes both actual round trips");
    pineforge::ReportC report{};
    host.fill_report(&report);
    pineforge::BacktestEngine::free_report(&report);
    require(pf_report_cache_stats(handle, &cache_bytes, &bypasses) == 0
            && cache_bytes >= pineforge::TradeLedger::resident_cache_bytes(0)
            && bypasses == static_cast<std::uint64_t>(fault),
            "the exported getter retains the real slab refusal after report finalization and release");
    const auto reported_bytes = cache_bytes;
    const auto reported_bypasses = bypasses;
    require(pf_report_cache_stats(handle, &cache_bytes, &bypasses) == 0
            && cache_bytes == reported_bytes && bypasses == reported_bypasses,
            "repeated exported queries neither count a borrowed slab nor repeat its bypass");
    std::uint64_t untouched = 93;
    require(pf_report_cache_stats(nullptr, &untouched, &bypasses) == -1
            && untouched == 93 && bypasses == reported_bypasses
            && pf_report_cache_stats(handle, nullptr, &untouched) == -1
            && untouched == 93
            && pf_report_cache_stats(handle, &untouched, nullptr) == -1
            && untouched == 93
            && pf_report_cache_stats(handle, &untouched, &untouched) == -1
            && untouched == 93,
            "invalid exported cache queries leave every supplied output unchanged");
    std::vector<std::byte> output;
    const auto word = [&](std::uint64_t value) { command_cache_test::append_word(output, value); };
    word(host.trade_count());
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        word(static_cast<std::uint64_t>(trade.entry_time));
        word(static_cast<std::uint64_t>(trade.exit_time));
        append_double(output, trade.entry_price); append_double(output, trade.exit_price);
        append_double(output, trade.qty); append_double(output, trade.pnl);
        append_double(output, trade.pnl_pct); word(trade.is_long);
        word(static_cast<std::uint64_t>(trade.entry_bar_index));
        word(static_cast<std::uint64_t>(trade.exit_bar_index));
        append_string(output, trade.entry_id); append_string(output, trade.entry_comment);
        append_string(output, trade.exit_comment); append_string(output, trade.exit_id);
        word(trade.exit_from_bracket); append_double(output, trade.max_runup);
        append_double(output, trade.max_drawdown); append_double(output, trade.commission);
        word(trade.entry_incarnation); word(trade.open_at_end);
        word(static_cast<std::uint64_t>(trade.close_cause));
    }
    word(host.native_continuation_hash());
    word(host.broker_state_hash());
    word(host.recorded_hashes().size());
    for (const auto hash : host.recorded_hashes()) word(hash);
    return output;
}

void native_allocation_failure_completes() {
    const auto healthy = native_allocation_run(false);
    require(native_allocation_run(true) == healthy,
            "native allocation bypass preserves every trade field and ordered continuation hash");
    require(native_allocation_run(false) == healthy,
            "a later healthy run reports no inherited allocation bypass");
}

void allocation_bypass_preserves_byte_admission() {
    for (const int error : {ENOSPC, EDQUOT, EIO, 0}) {
        Cap cap(3 * page);
        fail_next_array = true;
        auto backing = make_command_scratch_backing(page);
        require(!fail_next_array, "byte admission fixture really refused the cache allocation");
        const auto image = pattern(page, 3), replacement = pattern(page, 17);
        std::vector<std::byte> out(page);
        require(backing->write_page(0, bytes(image)) == ScratchStatus::ok,
                "allocation bypass writes its admitted first page");
        physical = {};
        fail_write_at = 1;
        write_error = error;
        ScratchStatus status;
        { Window window; status = backing->write_page(1, bytes(replacement)); }
        fail_write_at = 0;
        require(status == ScratchStatus::io_error && backing->page_count() == 2
                && physical.writes == 1 && physical.errors == (error != 0)
                && physical.short_io == (error == 0),
                "bypass preserves a real failed write and its already extended file extent");
        auto other = make_command_scratch_backing(page);
        require(other->write_page(0, bytes(image)) == ScratchStatus::ok,
                "a bypass failure releases no bytes belonging to the real extent");
        physical = {};
        { Window window; status = backing->write_page(2, bytes(image)); }
        require(status == ScratchStatus::io_error,
                "bypass admission charges failed-write extent against the shared cap");
        no_physical_io(physical);
        require(backing->write_page(1, bytes(replacement)) == ScratchStatus::ok
                && backing->read_page(0, buffer(out)) == ScratchStatus::ok && out == image
                && backing->read_page(1, buffer(out)) == ScratchStatus::ok && out == replacement,
                "bypass retries the failed page without losing an earlier page");
        other.reset();
        require(backing->write_page(2, bytes(image)) == ScratchStatus::ok,
                "released shared capacity admits the retried extension");
        backing.reset();
        auto reused = make_command_scratch_backing(page);
        require(reused->write_page(2, bytes(image)) == ScratchStatus::ok,
                "bypass destruction releases every charged extent byte");
    }
    physical = {};
}

void required_script_allocation_failure() {
    command_arena_test::ArenaAttachment attachment(command_arena_test::kSmallArenaBytes);
    // The optional slab is ready before arming. The first script write needs
    // the arena chunk directory, whose allocation is required for backing I/O.
    CachedScratchBacking cached(make_script_scratch_backing());
    std::array<std::byte, 4096> image{}, out{};
    image.fill(std::byte{0x5a});
    const auto claimed = ScratchArena::attached()->stats().bytes_used;
    const auto before = global_allocation::refused;
    const auto disabled = std::numeric_limits<std::size_t>::max();
    require(!global_allocation::refusing && global_allocation::fail_after == disabled,
            "required allocation fault starts disarmed");
    global_allocation::fail_after = 0;
    const auto status = cached.write_page(0, {image.data(), image.size()});
    const bool consumed = global_allocation::fail_after == disabled;
    global_allocation::fail_after = disabled;
    require(consumed && global_allocation::refused == before + 1
            && status == ScratchStatus::io_error && cached.carried_failure(),
            "exactly one required script directory allocation fails and carries its cause");
    bool caught = false;
    try {
        rethrow_carried_scratch_failure(cached.carried_failure());
    } catch (const std::system_error& error) {
        caught = error.code() == std::error_code(ENOMEM, std::generic_category())
            && pineforge::classify_run_failure(error).code == pineforge::RunFailureCode::out_of_memory;
    }
    require(caught && cached.page_count() == 0
            && ScratchArena::attached()->stats().bytes_used == claimed
            && cached.read_cache_stats().allocation_bypass_count == 0,
            "required allocation fails typed as out_of_memory with ENOMEM and publishes no page");
    require(cached.write_page(0, {image.data(), image.size()}) == ScratchStatus::ok
            && cached.read_page(0, {out.data(), out.size()}) == ScratchStatus::ok
            && out == image && !cached.carried_failure(),
            "an unarmed retry completes the required allocation and preserves all page bytes");
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

void failed_overwrite_invalidates_warm_page() {
    for (const int error : {ENOSPC, EDQUOT, EIO, 0}) {
        auto backing = make_command_scratch_backing(page, 0);
        const auto image = pattern(page, 51), replacement = pattern(page, 52);
        auto expected = image;
        std::vector<std::byte> out(page);
        require(backing->write_page(0, bytes(image)) == ScratchStatus::ok,
                "seed the write-through cache before a failed overwrite");
        physical = {};
        ScratchStatus status;
        { Window window; status = backing->read_page(0, buffer(out)); }
        require(status == ScratchStatus::ok && out == image && physical.reads == 0,
                "failed-overwrite fixture begins with a genuinely warm page");
        fail_write_at = 1;
        write_error = error;
        { Window window; status = backing->write_page(0, bytes(replacement)); }
        fail_write_at = 0;
        require(status == ScratchStatus::io_error && backing->page_count() == 1
                && physical.writes == 1 && physical.errors == (error != 0)
                && physical.short_io == (error == 0),
                "warm overwrite reaches the real failed or short backing write");
        if (error == 0)
            std::copy_n(replacement.begin(), page / 2, expected.begin());
        physical = {};
        { Window window; status = backing->read_page(0, buffer(out)); }
        require(status == ScratchStatus::ok && out == expected && physical.reads == 1
                && physical.read_bytes == page && physical.errors == 0 && physical.short_io == 0,
                "failed overwrite invalidates its tag and rereads every actual backing byte");
        physical = {};
        {
            Window window;
            status = backing->write_page(0, bytes(replacement));
            require(status == ScratchStatus::ok, "retry completes the full backing overwrite");
            status = backing->read_page(0, buffer(out));
        }
        require(status == ScratchStatus::ok && out == replacement && physical.writes == 1
                && physical.reads == 0 && physical.errors == 0 && physical.short_io == 0,
                "only a complete retry publishes the new warm page");
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

int main(int argc, char** argv) {
    try {
        spill_test::DiskDirectory directory;
        spill_test::Cap unlimited(0);
        if (argc == 2 && std::strcmp(argv[1], "--native-allocation") == 0) {
            spill_test::native_allocation_failure_completes();
            std::puts("PASS native_allocation_failure_completes");
            return 0;
        }
        spill_test::spill_boundary_matches_file(512, false);
        spill_test::spill_boundary_matches_file(1536, false);
        spill_test::spill_boundary_matches_file(4096, false);
        spill_test::spill_boundary_matches_file(4096, true);
        std::puts("PASS spill_boundary_matches_file");
        spill_test::extent_probe_covers_backing_lifetimes();
        std::puts("PASS extent_probe_covers_backing_lifetimes");
        spill_test::allocation_failure_bypasses_cache();
        std::puts("PASS allocation_failure_bypasses_cache");
        spill_test::native_allocation_failure_completes();
        std::puts("PASS native_allocation_failure_completes");
        spill_test::allocation_bypass_preserves_byte_admission();
        std::puts("PASS allocation_bypass_preserves_byte_admission");
        spill_test::required_script_allocation_failure();
        std::puts("PASS required_script_allocation_failure");
        spill_test::failed_spill_preserves_pages_and_budget();
        std::puts("PASS failed_spill_preserves_pages_and_budget");
        spill_test::failed_overwrite_invalidates_warm_page();
        std::puts("PASS failed_overwrite_invalidates_warm_page");
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
