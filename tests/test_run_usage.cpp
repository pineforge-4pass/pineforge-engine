// RED-FIRST CR-403 usage receipt coverage.
//
// The expected values below come from the callback, the independently read
// sink file and the existing arena query. None are copied from the receipt.
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/detail/scratch_file.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

// Lab platform's pre-receipt footprint. Debug STL has a different baseline.
#if defined(__linux__) && defined(__x86_64__) && defined(__GLIBCXX__) \
    && _GLIBCXX_USE_CXX11_ABI && !defined(_GLIBCXX_DEBUG)
static_assert(sizeof(pineforge::detail::ScratchFile) == 144,
              "usage tracking must preserve the original ScratchFile footprint");
#endif

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

pineforge::NativeRunSpec usage_spec(const char* key) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

class UsageHost final : public pineforge::NativeStrategyHost {
public:
    explicit UsageHost(bool fail) : fail_(fail) {}

private:
    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (fail_ && bars_ == 2) throw std::runtime_error("receipt failure row");
    }

    bool fail_;
    int bars_ = 0;
};

std::vector<pineforge::Bar> bars(int count) {
    std::vector<pineforge::Bar> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double close = 100.0 + i;
        result.push_back({close, close + 1.0, close - 1.0, close, 10.0,
                          INT64_C(1700000000000) + static_cast<std::int64_t>(i) * 60000});
    }
    return result;
}

struct Receipt {
    struct pf_run_usage_v1 value{};

    Receipt() {
        value.struct_size = sizeof value;
        value.version = 1;
        require(pf_run_usage_v1(&value) == 0, "usage receipt query failed");
    }
};

std::string temporary_path() {
    char pattern[] = "/tmp/pineforge-usage-XXXXXX";
    const int fd = ::mkstemp(pattern);
    require(fd >= 0, "mkstemp failed");
    ::close(fd);
    require(::unlink(pattern) == 0, "unlink failed");
    return pattern;
}

std::uint64_t file_bytes(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.good(), "sink file could not be read");
    return static_cast<std::uint64_t>(stream.tellg());
}

void query_is_transactional_before_first_run() {
    struct pf_run_usage_v1 value{};
    value.struct_size = 0;
    value.version = 1;
    value.cpu_user_ns = UINT64_C(0xdeadbeef);
    const struct pf_run_usage_v1 before = value;
    require(pf_run_usage_v1(&value) == -1, "pre-run usage query was accepted");
    require(std::memcmp(&value, &before, sizeof value) == 0,
            "failed usage query changed its output");
}

void successful_run_has_measured_usage() {
    constexpr int count = 2;
    UsageHost host(false);
    require(host.configure_native(usage_spec("usage-success")).status
                == pineforge::NativeSetupStatus::Applied,
            "usage success spec refused");
    const auto input = bars(count);
    host.run(input.data(), static_cast<int>(input.size()));
    require(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
            "usage success run failed");

    pf_report_t report{};
    host.fill_report(reinterpret_cast<pineforge::ReportC*>(&report));
    const Receipt receipt;
    require(receipt.value.bars_processed == static_cast<std::uint64_t>(count),
            "bars_processed did not equal the known script-bar count");
    require(receipt.value.cpu_user_ns + receipt.value.cpu_system_ns > 0,
            "CPU usage was not measured");
    require(receipt.value.run_wall_ns >= UINT64_C(40000000),
            "wall usage did not include the two 20 ms callback sleeps");
    require(receipt.value.peak_rss_bytes > 0, "RSS usage was not measured");
    const Receipt repeated;
    require(repeated.value.peak_rss_bytes >= receipt.value.peak_rss_bytes,
            "RSS high water was not monotone across reads");

    std::uint64_t high_water = 0;
    std::uint64_t capacity = 0;
    require(pf_report_scratch_arena_stats(&high_water, &capacity) == 0,
            "existing arena stats query failed");
    require(receipt.value.arena_high_water_bytes == high_water
                && receipt.value.arena_capacity_bytes == capacity,
            "usage arena values differ from the existing stats query");
    require((receipt.value.unmeasured_mask
             & PF_RUN_USAGE_UNMEASURED_REPORT_BYTES) != 0,
            "report-size unmeasured flag was omitted");
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(&report));
}

void sink_usage_is_independently_counted() {
    const std::string path = temporary_path();
    UsageHost host(false);
    require(host.configure_native(usage_spec("usage-sink")).status
                == pineforge::NativeSetupStatus::Applied,
            "usage sink spec refused");
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "usage", "/tmp"};
    require(strategy_set_report_sink_v1(&host, &sink) == 0, "usage sink setter refused");
    const auto input = bars(2);
    host.run(input.data(), static_cast<int>(input.size()));
    require(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
            "usage sink run failed");
    pf_report_t report{};
    host.fill_report(reinterpret_cast<pineforge::ReportC*>(&report));
    const Receipt receipt;
    const std::uint64_t expected_rows =
        static_cast<std::uint64_t>(report.equity_curve_len + report.trades_len);
    require(receipt.value.sink_rows_written == expected_rows,
            "sink row count was not independently counted");
    require(receipt.value.sink_bytes_written == file_bytes(path),
            "sink byte count differs from the independently read file");
    require(receipt.value.scratch_file_count_peak > 0,
            "ScratchFile count peak was not measured");
    std::remove(path.c_str());
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(&report));
}

void failed_run_keeps_receipt() {
    UsageHost host(true);
    require(host.configure_native(usage_spec("usage-failure")).status
                == pineforge::NativeSetupStatus::Applied,
            "usage failure spec refused");
    const auto input = bars(3);
    host.run(input.data(), static_cast<int>(input.size()));
    require(host.native_state().kind == pineforge::NativeLifecycleKind::Failed,
            "expected the callback failure");
    const Receipt receipt;
    require(receipt.value.bars_processed == 1,
            "failed receipt did not retain bars before the failure");
    require(receipt.value.run_wall_ns >= UINT64_C(20000000),
            "failed receipt did not retain wall time");
}

void registry_close_and_overflow_are_explicit() {
    using namespace pineforge::detail;
    // Isolated registry unit fixture: these keys never enter engine counters
    // and need no OS descriptors or an increased process fd limit.
    RunUsageState usage;
    RunUsageScratchFiles files(usage);
    files.open(0);
    files.open(0);
    files.resize(0, 11);
    files.resize(0, 3, true);
    require(usage.scratch_count_current.load() == 1
                && usage.scratch_bytes_current.load() == 11,
            "duplicate open or overwrite changed registry totals");
    files.resize(0, 4);
    files.close(0);
    files.close(0);
    require(usage.scratch_count_current.load() == 0
                && usage.scratch_bytes_current.load() == 0,
            "close did not lower live totals, or repeated close underflowed");
    require(usage.scratch_count_peak.load() == 1
                && usage.scratch_bytes_peak.load() == 11,
            "close lowered the recorded peaks");

    // Keys deliberately collide at the initial probe slot.
    const int capacity = static_cast<int>(RunUsageScratchFiles::kCapacity);
    for (int i = 0; i < capacity; ++i) files.open(i * capacity);
    require(usage.scratch_count_current.load() == RunUsageScratchFiles::kCapacity,
            "registry could not use its complete fixed capacity");
    require(usage.scratch_unmeasured_mask.load() == 0,
            "registry reported overflow before reaching capacity");
    files.open(capacity * capacity);
    const auto unknown = PF_RUN_USAGE_UNMEASURED_SCRATCH_BYTES
        | PF_RUN_USAGE_UNMEASURED_SCRATCH_COUNT;
    require(usage.scratch_unmeasured_mask.load() == unknown,
            "registry overflow did not mark both scratch figures unmeasured");
    struct pf_run_usage_v1 snapshot{};
    snapshot.unmeasured_mask = PF_RUN_USAGE_UNMEASURED_REPORT_BYTES;
    snapshot.cpu_user_ns = 42;
    run_usage_scratch_fill(snapshot, usage);
    require(snapshot.unmeasured_mask == (unknown | PF_RUN_USAGE_UNMEASURED_REPORT_BYTES)
                && snapshot.scratch_file_bytes_peak == 0
                && snapshot.scratch_file_count_peak == 0
                && snapshot.cpu_user_ns == 42,
            "receipt scratch fill exposed partial peaks or changed another field");
    files.close(0); // A hole must not hide a later colliding descriptor.
    files.resize(capacity, 7);
    files.close(capacity);
    require(usage.scratch_bytes_current.load() == 0
                && usage.scratch_count_current.load()
                    == RunUsageScratchFiles::kCapacity - 2,
            "registry lookup lost a descriptor after a colliding close");
    files.open(0);
    files.resize(0, 2);
    require(usage.scratch_bytes_current.load() == 2,
            "reused descriptor retained its previous extent");
    for (int i = 0; i < capacity; ++i) files.close(i * capacity);
    files.close(capacity * capacity); // Unregistered overflow key is harmless.
    require(usage.scratch_count_current.load() == 0
                && usage.scratch_bytes_current.load() == 0,
            "overflow cleanup underflowed registry totals");
    require(usage.scratch_count_peak.load() == RunUsageScratchFiles::kCapacity
                && usage.scratch_bytes_peak.load() == 11,
            "registry cleanup changed the high water");
    require(usage.scratch_unmeasured_mask.load() == unknown,
            "registry reuse or cleanup cleared the overflow warning");
    run_usage_scratch_peak_reset(usage);
    snapshot = {};
    run_usage_scratch_fill(snapshot, usage);
    require(usage.scratch_unmeasured_mask.load() == unknown
                && (snapshot.unmeasured_mask & unknown) == unknown,
            "peak reset cleared the sticky overflow mask");
}

void ordinary_scratch_lifecycle_is_measured() {
    using namespace pineforge::detail;
    require(!ScratchArena::attached(), "ordinary-file fixture needs no attached arena");
    const auto bytes = run_usage_state.scratch_bytes_current.load();
    const auto count = run_usage_state.scratch_count_current.load();
    run_usage_scratch_peak_reset();
    (void)temporary_path();
    require(run_usage_state.scratch_count_current.load() == count,
            "host temporary descriptor was counted as engine scratch");
    ScratchFile first;
    require(run_usage_state.scratch_count_current.load() == count,
            "unopened ScratchFile was counted");
    first.set_directory("/tmp");
    first.open("pineforge-usage");
    first.open("pineforge-usage");
    require(run_usage_state.scratch_count_current.load() == count + 1,
            "ordinary open did not count exactly once");
    const char data[] = {'a', 'b', 'c'};
    first.write(5, data, sizeof data);
    require(run_usage_state.scratch_bytes_current.load() == bytes + 8,
            "ordinary write missed its sparse logical extent");
    require(first.write_some(100, nullptr, 0) == 0,
            "zero-length ordinary write failed");
    require(run_usage_state.scratch_bytes_current.load() == bytes + 8,
            "zero-length ordinary write extended the measured extent");
    require(first.write_some(12, data, 2) == 2, "usage short-transfer fixture failed");
    first.write(0, data, 1);
    require(run_usage_state.scratch_bytes_current.load() == bytes + 14,
            "write_some missed growth or overwrite shrank the extent");
    first.ensure_size(32);
    first.ensure_size(16);
    first.truncate(4);
    require(run_usage_state.scratch_bytes_current.load() == bytes + 4,
            "ordinary truncate did not lower live bytes");
    ScratchFile second;
    second.set_directory("/tmp");
    second.open("pineforge-usage");
    second.write(0, data, sizeof data);
    const int descriptor = first.descriptor();
    second = std::move(first);
    first.clear();
    require(second.descriptor() == descriptor
                && run_usage_state.scratch_bytes_current.load() == bytes + 4
                && run_usage_state.scratch_count_current.load() == count + 1,
            "ordinary move lost registration or retained its overwritten destination");
    second.clear();
    second.clear();
    second.open("pineforge-usage");
    require(run_usage_state.scratch_bytes_current.load() == bytes,
            "ordinary reopen retained a closed file's bytes");
    second.write(0, data, 1);
    second.clear();
    require(run_usage_state.scratch_bytes_current.load() == bytes
                && run_usage_state.scratch_count_current.load() == count,
            "ordinary clear did not restore live totals");
    const Receipt receipt;
    require(receipt.value.scratch_file_bytes_peak == bytes + 32
                && receipt.value.scratch_file_count_peak == count + 2,
            "ordinary receipt did not retain independent byte/count peaks after close");
    require((receipt.value.unmeasured_mask
             & (PF_RUN_USAGE_UNMEASURED_SCRATCH_BYTES
                | PF_RUN_USAGE_UNMEASURED_SCRATCH_COUNT)) == 0,
            "ordinary fixture unexpectedly became unmeasured");
}

void slab_regions_are_counted_once_and_released() {
    using namespace pineforge::detail;
    const auto bytes = run_usage_state.scratch_bytes_current.load();
    const auto count = run_usage_state.scratch_count_current.load();
    run_usage_scratch_peak_reset();
    ScratchManifest manifest;
    manifest.R = 4; // Four 16-byte roots, exactly 64 logical bytes admitted.
    manifest.K_e = 1;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    require(run_usage_state.scratch_count_current.load() == count,
            "unused slab resources were counted");
    ScratchFile first;
    first.bind_scratch(slab, ScratchResource::successor_roots);
    ScratchFile alias;
    alias.bind_scratch(slab, ScratchResource::successor_roots);
    require(run_usage_state.scratch_count_current.load() == count + 1,
            "aliased slab region was counted more than once");
    first.ensure_size(32);
    const char data[] = {'x', 'y', 'z'};
    alias.write(0, data, sizeof data);
    ScratchFile moved(std::move(alias));
    alias.clear();
    require(run_usage_state.scratch_bytes_current.load() == bytes + 32
                && run_usage_state.scratch_count_current.load() == count + 1,
            "slab overwrite or move changed the shared region's measurement");
    bool refused = false;
    try { moved.ensure_size(65); }
    catch (const ScratchCapacityError&) { refused = true; }
    require(refused && run_usage_state.scratch_bytes_current.load() == bytes + 32,
            "refused slab growth changed live usage");
    first.clear(); // Shared reset clears the extent visible to both owners.
    first.clear();
    require(run_usage_state.scratch_bytes_current.load() == bytes
                && run_usage_state.scratch_count_current.load() == count,
            "shared slab clear failed to release its live measurement");
    moved.write(7, data, sizeof data);
    require(run_usage_state.scratch_bytes_current.load() == bytes + 10
                && run_usage_state.scratch_count_current.load() == count + 1,
            "retained slab binding did not reactivate after reset");
    slab->zero();
    require(run_usage_state.scratch_bytes_current.load() == bytes + 10,
            "slab zero changed a preserved logical extent");
    moved.clear();
    slab->extend(ScratchResource::erased_data, 16);
    slab->reset();
    require(run_usage_state.scratch_bytes_current.load() == bytes
                && run_usage_state.scratch_count_current.load() == count,
            "whole-slab reset did not release active regions");
    const Receipt receipt;
    require(receipt.value.scratch_file_bytes_peak == bytes + 32
                && receipt.value.scratch_file_count_peak == count + 1,
            "slab receipt lost its peak after reset");
    {
        ScratchSlab direct(manifest);
        direct.extend(ScratchResource::successor_roots, 8);
    }
    require(run_usage_state.scratch_bytes_current.load() == bytes
                && run_usage_state.scratch_count_current.load() == count,
            "slab destruction retained an active resource");
}

class UsageArena {
public:
    UsageArena() {
        char pattern[] = "/tmp/pineforge-usage-arena-XXXXXX";
        fd_ = ::mkstemp(pattern);
        require(fd_ >= 0, "usage arena mkstemp failed");
        try {
            require(::unlink(pattern) == 0, "usage arena unlink failed");
            require(::ftruncate(fd_, 2 * pineforge::detail::ScratchArena::kChunkBytes) == 0,
                    "usage arena sizing failed");
            pineforge::detail::ScratchArena::attach(
                fd_, 2 * pineforge::detail::ScratchArena::kChunkBytes);
        } catch (...) {
            ::close(fd_);
            throw;
        }
    }
    ~UsageArena() {
        pineforge::detail::ScratchArena::detach();
        ::close(fd_);
    }
    UsageArena(const UsageArena&) = delete;
    UsageArena& operator=(const UsageArena&) = delete;
private:
    int fd_ = -1;
};

void arena_regions_use_their_existing_extents() {
    using namespace pineforge::detail;
    const auto bytes = run_usage_state.scratch_bytes_current.load();
    const auto count = run_usage_state.scratch_count_current.load();
    run_usage_scratch_peak_reset();
    UsageArena arena;
    require(run_usage_state.scratch_count_current.load() == count,
            "arena backing descriptor was counted as an opened region");
    {
        ScratchFile first;
        first.open("pineforge-usage");
        first.ensure_size(32);
        first.truncate(4);
        const char data[] = {'a', 'b', 'c'};
        first.write(10, data, sizeof data);
        ScratchFile moved(std::move(first));
        first.clear();
        require(run_usage_state.scratch_bytes_current.load() == bytes + 13
                    && run_usage_state.scratch_count_current.load() == count + 1,
                "arena growth, shrink or move lost its live region");
        ScratchFile bounded;
        bounded.open_bounded("pineforge-usage", 1);
        bool refused = false;
        try { bounded.ensure_size(2); }
        catch (const ReportScratchIoError&) { refused = true; }
        require(refused && run_usage_state.scratch_bytes_current.load() == bytes + 13,
                "refused arena growth changed its measured extent");
        bounded.clear();
        moved.clear();
        moved.clear();
    }
    require(run_usage_state.scratch_bytes_current.load() == bytes
                && run_usage_state.scratch_count_current.load() == count,
            "arena close did not restore live usage");
    const Receipt receipt;
    require(receipt.value.scratch_file_bytes_peak == bytes + 32
                && receipt.value.scratch_file_count_peak == count + 2,
            "arena receipt lost its high water on close");
    std::uint64_t high_water = 0;
    std::uint64_t capacity = 0;
    require(pf_report_scratch_arena_stats(&high_water, &capacity) == 0
                && capacity == 2 * ScratchArena::kChunkBytes
                && high_water > 0
                && receipt.value.arena_high_water_bytes == high_water
                && receipt.value.arena_capacity_bytes == capacity,
            "usage arena receipt differs from the attached arena stats");
}

} // namespace

int main() {
    try {
        query_is_transactional_before_first_run();
        successful_run_has_measured_usage();
        sink_usage_is_independently_counted();
        failed_run_keeps_receipt();
        registry_close_and_overflow_are_explicit();
        ordinary_scratch_lifecycle_is_measured();
        slab_regions_are_counted_once_and_released();
        arena_regions_use_their_existing_extents();
        std::puts("test_run_usage: all rows passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_run_usage: %s\n", error.what());
        return 1;
    }
}
