#pragma once

#include <pineforge/pineforge.h>

#include <atomic>
#include <cstdint>
#include <sys/resource.h>
#include <time.h>

namespace pineforge::detail {

inline constexpr std::uint64_t kRunUsageUnmeasuredCpuUser = UINT64_C(1) << 0;
inline constexpr std::uint64_t kRunUsageUnmeasuredCpuSystem = UINT64_C(1) << 1;
inline constexpr std::uint64_t kRunUsageUnmeasuredSinkBytes = UINT64_C(1) << 2;
inline constexpr std::uint64_t kRunUsageUnmeasuredSinkRows = UINT64_C(1) << 3;
inline constexpr std::uint64_t kRunUsageUnmeasuredReportBytes = UINT64_C(1) << 4;

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

inline void run_usage_scratch_peak_reset() noexcept {
    run_usage_state.scratch_bytes_peak.store(
        run_usage_state.scratch_bytes_current.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    run_usage_state.scratch_count_peak.store(
        run_usage_state.scratch_count_current.load(std::memory_order_relaxed),
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

inline void run_usage_scratch_open(std::uint64_t bytes) noexcept {
    run_usage_state.scratch_count_current.fetch_add(1, std::memory_order_relaxed);
    run_usage_state.scratch_bytes_current.fetch_add(bytes, std::memory_order_relaxed);
    run_usage_max(run_usage_state.scratch_count_peak,
                  run_usage_state.scratch_count_current.load(std::memory_order_relaxed));
    run_usage_max(run_usage_state.scratch_bytes_peak,
                  run_usage_state.scratch_bytes_current.load(std::memory_order_relaxed));
}

inline void run_usage_scratch_resize(std::uint64_t old_bytes,
                                     std::uint64_t new_bytes) noexcept {
    if (new_bytes > old_bytes)
        run_usage_state.scratch_bytes_current.fetch_add(new_bytes - old_bytes,
                                                        std::memory_order_relaxed);
    else
        run_usage_state.scratch_bytes_current.fetch_sub(old_bytes - new_bytes,
                                                        std::memory_order_relaxed);
    run_usage_max(run_usage_state.scratch_bytes_peak,
                  run_usage_state.scratch_bytes_current.load(std::memory_order_relaxed));
}

inline void run_usage_scratch_close(std::uint64_t bytes) noexcept {
    run_usage_state.scratch_count_current.fetch_sub(1, std::memory_order_relaxed);
    run_usage_state.scratch_bytes_current.fetch_sub(bytes, std::memory_order_relaxed);
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
    local.scratch_file_bytes_peak =
        run_usage_state.scratch_bytes_peak.load(std::memory_order_relaxed);
    local.scratch_file_count_peak =
        run_usage_state.scratch_count_peak.load(std::memory_order_relaxed);
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
