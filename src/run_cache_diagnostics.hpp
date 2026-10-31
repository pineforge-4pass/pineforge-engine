#pragma once

#include <cstdint>
#include <limits>

namespace pineforge::detail {

class RunCacheDiagnostics;
inline thread_local RunCacheDiagnostics* active_run_cache_diagnostics = nullptr;

// One embedded registration per allocation owner; no diagnostic allocation.
// Readers disclose actual storage, so the sink never adds a second ledger
// byte counter or charges the command slab again when the core borrows it.
class RunCacheSource {
public:
    using Reader = std::uint64_t (*)(const void*) noexcept;
    RunCacheSource(const void* owner, Reader reader) noexcept;
    ~RunCacheSource() noexcept { detach(); }
    RunCacheSource(const RunCacheSource&) = delete;
    RunCacheSource& operator=(const RunCacheSource&) = delete;
    void allocation_bypassed() const noexcept;

private:
    friend class RunCacheDiagnostics;
    void detach() noexcept;
    const void* owner_;
    Reader reader_;
    RunCacheDiagnostics* sink_ = nullptr;
    RunCacheSource* next_ = nullptr;
    RunCacheSource** previous_ = nullptr;
};

// The admitted run's production accounting sink for command-cache storage
// and the ledger's fixed workspace, directory and decoded-entry pages.
// Residency is a current snapshot, not a sum of peaks or backing-file bytes.
// Failure events survive owner release and reset only at the next admission.
class RunCacheDiagnostics {
public:
    RunCacheDiagnostics() = default;
    ~RunCacheDiagnostics() noexcept { begin_run(); }
    RunCacheDiagnostics(const RunCacheDiagnostics&) = delete;
    RunCacheDiagnostics& operator=(const RunCacheDiagnostics&) = delete;

    void begin_run() noexcept {
        while (first_) first_->detach();
        allocation_bypass_count_ = 0;
    }
    std::uint64_t resident_bytes() const noexcept {
        std::uint64_t total = 0;
        for (const auto* source = first_; source; source = source->next_) {
            const auto bytes = source->reader_(source->owner_);
            const auto available = std::numeric_limits<std::uint64_t>::max() - total;
            total += bytes <= available ? bytes : available;
        }
        return total;
    }
    std::uint64_t allocation_bypass_count() const noexcept {
        return allocation_bypass_count_;
    }

private:
    friend class RunCacheSource;
    RunCacheSource* first_ = nullptr;
    std::uint64_t allocation_bypass_count_ = 0;
};

inline RunCacheSource::RunCacheSource(const void* owner, Reader reader) noexcept
    : owner_(owner), reader_(reader), sink_(active_run_cache_diagnostics) {
    if (!sink_) return;
    next_ = sink_->first_;
    previous_ = &sink_->first_;
    if (next_) next_->previous_ = &next_;
    sink_->first_ = this;
}

inline void RunCacheSource::detach() noexcept {
    if (!sink_) return;
    *previous_ = next_;
    if (next_) next_->previous_ = previous_;
    sink_ = nullptr;
    next_ = nullptr;
    previous_ = nullptr;
}

inline void RunCacheSource::allocation_bypassed() const noexcept {
    if (sink_ && sink_->allocation_bypass_count_ != std::numeric_limits<std::uint64_t>::max())
        ++sink_->allocation_bypass_count_;
}

class RunCacheDiagnosticsScope {
public:
    explicit RunCacheDiagnosticsScope(RunCacheDiagnostics& sink) noexcept
        : previous_(active_run_cache_diagnostics) {
        active_run_cache_diagnostics = &sink;
    }
    ~RunCacheDiagnosticsScope() noexcept { active_run_cache_diagnostics = previous_; }
    RunCacheDiagnosticsScope(const RunCacheDiagnosticsScope&) = delete;
    RunCacheDiagnosticsScope& operator=(const RunCacheDiagnosticsScope&) = delete;
private:
    RunCacheDiagnostics* previous_;
};

} // namespace pineforge::detail
