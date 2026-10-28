#pragma once

#include <pineforge/pineforge.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/resource.h>
#include <time.h>

namespace pineforge::detail {

inline constexpr std::uint64_t kRunUsageUnmeasuredCpuUser = UINT64_C(1) << 0;
inline constexpr std::uint64_t kRunUsageUnmeasuredCpuSystem = UINT64_C(1) << 1;
inline constexpr std::uint64_t kRunUsageUnmeasuredSinkBytes = UINT64_C(1) << 2;
inline constexpr std::uint64_t kRunUsageUnmeasuredSinkRows = UINT64_C(1) << 3;
inline constexpr std::uint64_t kRunUsageUnmeasuredReportBytes = UINT64_C(1) << 4;
inline constexpr std::uint64_t kRunUsageUnmeasuredScratchBytes =
    PF_RUN_USAGE_UNMEASURED_SCRATCH_BYTES;
inline constexpr std::uint64_t kRunUsageUnmeasuredScratchCount =
    PF_RUN_USAGE_UNMEASURED_SCRATCH_COUNT;

struct RunUsageClock {
    std::uint64_t user_ns = 0;
    std::uint64_t system_ns = 0;
    bool valid = false;
};

inline std::uint64_t run_usage_monotonic_ns() noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * UINT64_C(1000000000)
        + static_cast<std::uint64_t>(value.tv_nsec);
}

inline std::uint64_t run_usage_timeval_ns(const timeval& value) noexcept {
    return static_cast<std::uint64_t>(value.tv_sec) * UINT64_C(1000000000)
        + static_cast<std::uint64_t>(value.tv_usec) * UINT64_C(1000);
}

inline RunUsageClock run_usage_process_clock() noexcept {
    rusage value{};
    if (::getrusage(RUSAGE_SELF, &value) != 0) return {};
    return {run_usage_timeval_ns(value.ru_utime), run_usage_timeval_ns(value.ru_stime), true};
}

struct RunUsageState {
    std::atomic<bool> has_run{false};
    std::atomic<std::uint64_t> run_start_ns{0};
    std::atomic<std::uint64_t> baseline_user_ns{0};
    std::atomic<std::uint64_t> baseline_system_ns{0};
    std::atomic<bool> baseline_valid{false};
    std::atomic<std::uint64_t> bars_processed{0};
    std::atomic<std::uint64_t> sink_bytes_written{0};
    std::atomic<std::uint64_t> sink_rows_written{0};
    std::atomic<std::uint64_t> unmeasured_mask{kRunUsageUnmeasuredSinkBytes
                                                | kRunUsageUnmeasuredSinkRows
                                                | kRunUsageUnmeasuredReportBytes};
    std::atomic<std::uint64_t> scratch_bytes_current{0};
    std::atomic<std::uint64_t> scratch_count_current{0};
    std::atomic<std::uint64_t> scratch_bytes_peak{0};
    std::atomic<std::uint64_t> scratch_count_peak{0};
    // Sticky across run resets: an unregistered file can outlive a run.
    std::atomic<std::uint64_t> scratch_unmeasured_mask{0};
};

inline RunUsageState run_usage_state;

inline void run_usage_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    auto prior = target.load(std::memory_order_relaxed);
    while (prior < value
           && !target.compare_exchange_weak(prior, value, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {}
}

inline void run_usage_begin() noexcept {
    const auto clock = run_usage_process_clock();
    const auto current_bytes =
        run_usage_state.scratch_bytes_current.load(std::memory_order_relaxed);
    const auto current_count =
        run_usage_state.scratch_count_current.load(std::memory_order_relaxed);
    run_usage_state.run_start_ns.store(run_usage_monotonic_ns(), std::memory_order_relaxed);
    run_usage_state.baseline_user_ns.store(clock.user_ns, std::memory_order_relaxed);
    run_usage_state.baseline_system_ns.store(clock.system_ns, std::memory_order_relaxed);
    run_usage_state.baseline_valid.store(clock.valid, std::memory_order_relaxed);
    run_usage_state.bars_processed.store(0, std::memory_order_relaxed);
    run_usage_state.sink_bytes_written.store(0, std::memory_order_relaxed);
    run_usage_state.sink_rows_written.store(0, std::memory_order_relaxed);
    run_usage_state.unmeasured_mask.store(
        kRunUsageUnmeasuredSinkBytes | kRunUsageUnmeasuredSinkRows
            | kRunUsageUnmeasuredReportBytes, std::memory_order_relaxed);
    run_usage_state.scratch_bytes_peak.store(current_bytes, std::memory_order_relaxed);
    run_usage_state.scratch_count_peak.store(current_count, std::memory_order_relaxed);
    run_usage_state.has_run.store(true, std::memory_order_release);
}

inline void run_usage_script_bar() noexcept {
    run_usage_state.bars_processed.fetch_add(1, std::memory_order_relaxed);
}

inline void run_usage_scratch_peak_reset(RunUsageState& usage = run_usage_state) noexcept {
    usage.scratch_bytes_peak.store(
        usage.scratch_bytes_current.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    usage.scratch_count_peak.store(
        usage.scratch_count_current.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

inline void run_usage_sink_enabled(bool enabled) noexcept {
    if (enabled) {
        run_usage_state.unmeasured_mask.fetch_and(
            ~(kRunUsageUnmeasuredSinkBytes | kRunUsageUnmeasuredSinkRows),
            std::memory_order_relaxed);
    }
}

inline void run_usage_sink_line(std::uint64_t bytes, bool row) noexcept {
    run_usage_state.sink_bytes_written.fetch_add(bytes, std::memory_order_relaxed);
    if (row) run_usage_state.sink_rows_written.fetch_add(1, std::memory_order_relaxed);
}

inline void run_usage_scratch_open(
    std::uint64_t bytes, RunUsageState& usage = run_usage_state) noexcept {
    const auto count = usage.scratch_count_current.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto total = usage.scratch_bytes_current.fetch_add(bytes, std::memory_order_relaxed)
        + bytes;
    run_usage_max(usage.scratch_count_peak, count);
    run_usage_max(usage.scratch_bytes_peak, total);
}

inline void run_usage_scratch_resize(std::uint64_t old_bytes,
                                     std::uint64_t new_bytes,
                                     RunUsageState& usage = run_usage_state) noexcept {
    if (new_bytes > old_bytes) {
        const auto delta = new_bytes - old_bytes;
        const auto total =
            usage.scratch_bytes_current.fetch_add(delta, std::memory_order_relaxed) + delta;
        run_usage_max(usage.scratch_bytes_peak, total);
    } else {
        usage.scratch_bytes_current.fetch_sub(old_bytes - new_bytes,
                                              std::memory_order_relaxed);
    }
}

inline void run_usage_scratch_close(
    std::uint64_t bytes, RunUsageState& usage = run_usage_state) noexcept {
    usage.scratch_count_current.fetch_sub(1, std::memory_order_relaxed);
    usage.scratch_bytes_current.fetch_sub(bytes, std::memory_order_relaxed);
}

// Only descriptors opened by ScratchFile enter this table; host descriptors
// are never enumerated. Each descriptor has one thread-owned ScratchFile,
// including across moves. No allocation, mutex, or spin lock on any path.
// Reserved slots prevent unrelated opens from claiming the same entry.
class RunUsageScratchFiles {
public:
    static constexpr std::size_t kCapacity = 256;
    explicit constexpr RunUsageScratchFiles(RunUsageState& usage) noexcept : usage_(usage) {}

    void open(int fd) noexcept {
        if (fd < 0 || find(fd)) return;
        const auto start = static_cast<std::size_t>(fd) % kCapacity;
        for (std::size_t i = 0; i < kCapacity; ++i) {
            auto& entry = entries_[(start + i) % kCapacity];
            int empty = -1;
            if (!entry.fd.compare_exchange_strong(empty, -2, std::memory_order_acquire,
                                                   std::memory_order_relaxed))
                continue;
            entry.bytes.store(0, std::memory_order_relaxed);
            run_usage_scratch_open(0, usage_);
            entry.fd.store(fd, std::memory_order_release);
            return;
        }
        // Never publish a partial total as measured, even after a later run
        // reset or after entries become available. Both fields become unknown.
        usage_.scratch_unmeasured_mask.fetch_or(
            kRunUsageUnmeasuredScratchBytes | kRunUsageUnmeasuredScratchCount,
            std::memory_order_relaxed);
    }

    void resize(int fd, std::uint64_t bytes, bool grow_only = false) noexcept {
        auto* entry = find(fd);
        if (!entry) return; // Failed registration already marked the figures.
        const auto old = entry->bytes.load(std::memory_order_relaxed);
        if (grow_only && bytes <= old) return;
        entry->bytes.store(bytes, std::memory_order_relaxed);
        run_usage_scratch_resize(old, bytes, usage_);
    }

    void close(int fd) noexcept {
        auto* entry = find(fd);
        if (!entry) return; // Also makes repeated clear() harmless.
        run_usage_scratch_close(entry->bytes.load(std::memory_order_relaxed), usage_);
        entry->fd.store(-1, std::memory_order_release);
    }

private:
    struct Entry {
        std::atomic<int> fd{-1};
        std::atomic<std::uint64_t> bytes{0};
    };
    Entry* find(int fd) noexcept {
        if (fd < 0) return nullptr;
        const auto start = static_cast<std::size_t>(fd) % kCapacity;
        // Scan past holes: close() and descriptor reuse do not form chains.
        for (std::size_t i = 0; i < kCapacity; ++i) {
            auto& entry = entries_[(start + i) % kCapacity];
            if (entry.fd.load(std::memory_order_acquire) == fd) return &entry;
        }
        return nullptr;
    }
    static_assert(std::atomic<int>::is_always_lock_free,
                  "scratch receipt descriptor tracking must be lock-free");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "scratch receipt byte tracking must be lock-free");
    RunUsageState& usage_;
    std::array<Entry, kCapacity> entries_{};
};

inline RunUsageScratchFiles run_usage_scratch_files{run_usage_state};

inline void run_usage_scratch_fill(struct pf_run_usage_v1& out,
                                   const RunUsageState& usage = run_usage_state) noexcept {
    out.scratch_file_bytes_peak = usage.scratch_bytes_peak.load(std::memory_order_relaxed);
    out.scratch_file_count_peak = usage.scratch_count_peak.load(std::memory_order_relaxed);
    out.unmeasured_mask |= usage.scratch_unmeasured_mask.load(std::memory_order_relaxed);
    if (out.unmeasured_mask & kRunUsageUnmeasuredScratchBytes)
        out.scratch_file_bytes_peak = 0;
    if (out.unmeasured_mask & kRunUsageUnmeasuredScratchCount)
        out.scratch_file_count_peak = 0;
}

inline bool run_usage_fill(struct pf_run_usage_v1* out) noexcept {
    if (!out || !run_usage_state.has_run.load(std::memory_order_acquire)) return false;
    const auto clock = run_usage_process_clock();
    if (!clock.valid) return false;
    struct pf_run_usage_v1 local{};
    local.struct_size = sizeof(local);
    local.version = 1;
    const auto baseline_user =
        run_usage_state.baseline_user_ns.load(std::memory_order_relaxed);
    const auto baseline_system =
        run_usage_state.baseline_system_ns.load(std::memory_order_relaxed);
    const auto baseline_valid =
        run_usage_state.baseline_valid.load(std::memory_order_relaxed);
    local.cpu_user_ns = baseline_valid && clock.user_ns >= baseline_user
        ? clock.user_ns - baseline_user : 0;
    local.cpu_system_ns = baseline_valid && clock.system_ns >= baseline_system
        ? clock.system_ns - baseline_system : 0;
    local.unmeasured_mask = run_usage_state.unmeasured_mask.load(std::memory_order_relaxed);
    if (!baseline_valid)
        local.unmeasured_mask |= kRunUsageUnmeasuredCpuUser | kRunUsageUnmeasuredCpuSystem;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) return false;
#if defined(__APPLE__)
    local.peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    local.peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * UINT64_C(1024);
#endif
    const auto start = run_usage_state.run_start_ns.load(std::memory_order_relaxed);
    const auto now = run_usage_monotonic_ns();
    local.run_wall_ns = now >= start ? now - start : 0;
    run_usage_scratch_fill(local);
    local.sink_bytes_written =
        run_usage_state.sink_bytes_written.load(std::memory_order_relaxed);
    local.sink_rows_written =
        run_usage_state.sink_rows_written.load(std::memory_order_relaxed);
    local.report_bytes = 0;
    local.bars_processed =
        run_usage_state.bars_processed.load(std::memory_order_relaxed);
    *out = local;
    return true;
}

} // namespace pineforge::detail
