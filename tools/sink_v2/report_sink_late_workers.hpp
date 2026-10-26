#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <thread>

namespace pineforge { namespace report_sink { namespace v2 {

inline constexpr unsigned late_worker_limit = 4; // caller counts as a worker

// Each ordinal has exactly one writer. Only inspect after joined_workers().
// The ordinal is the old serial order, independent of execution/scheduling.
template<std::size_t N> class OrderedFailures {
    std::array<std::exception_ptr, N> errors_{};
public:
    template<class Work> void capture(std::size_t ordinal, Work&& work) noexcept {
        try { work(); }
        catch (...) { errors_[ordinal] = std::current_exception(); }
    }
    void rethrow() const {
        for (const auto& error : errors_) if (error) std::rethrow_exception(error);
    }
};

// All launched threads are joined, including when launch or work throws.
// Missing lanes after a launch failure execute on the caller so the caller
// can still choose a preceding, serially ordered unit failure deterministically.
// Infrastructure errors are returned for rethrow AFTER OrderedFailures.
template<class Work> std::exception_ptr joined_workers(unsigned count, Work&& work) {
    std::array<std::thread, late_worker_limit - 1> threads;
    std::array<std::exception_ptr, late_worker_limit> errors{};
    std::exception_ptr launch_error;
    auto run = [&](unsigned worker) noexcept {
        try { work(worker); }
        catch (...) { errors[worker] = std::current_exception(); }
    };
    unsigned started = 0;
    try {
        for (unsigned worker = 1; worker < count; ++worker) {
            threads[started] = std::thread(run, worker);
            ++started;
        }
    } catch (...) { launch_error = std::current_exception(); }
    run(0);
    for (unsigned worker = started + 1; worker < count; ++worker) run(worker);
    for (auto& thread : threads) if (thread.joinable()) thread.join();
    for (const auto& error : errors) if (error) return error;
    return launch_error;
}

template<std::size_t N, class Work>
void ordered_tasks(unsigned workers, const std::array<std::size_t, N>& schedule, Work&& work) {
    OrderedFailures<N> errors;
    std::atomic<std::size_t> next{workers};
    const auto infrastructure = joined_workers(workers, [&](unsigned worker) {
        // Each lane owns its first unit, then steals remaining work. In
        // particular the caller starts the figure scan while other lanes
        // start the stored scan, page trees and first rank direction.
        for (std::size_t slot = worker; slot < N; slot = next.fetch_add(1)) {
            const auto ordinal = schedule[slot];
            errors.capture(ordinal, [&] { work(ordinal, worker); });
        }
    });
    errors.rethrow();
    if (infrastructure) std::rethrow_exception(infrastructure);
}

}}} // namespace pineforge::report_sink::v2
