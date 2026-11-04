#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/series.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define PINEFORGE_HAS_RUNTIME_CONTEXT_SERVICE_V1 1

namespace pineforge {

struct RuntimeSubdivision {
    int ratio = 1;
    int seconds = 0;
};
std::optional<RuntimeSubdivision> runtime_subdivision(
    const std::string& input_timeframe, const std::string& requested_timeframe);
std::vector<Bar> runtime_subdivide(const Bar&, RuntimeSubdivision);

struct RuntimeContextHandle {
    std::uint64_t generation = 0;
    std::uint64_t serial = 0;
    std::uint64_t owner = 0;
    friend bool operator==(RuntimeContextHandle a, RuntimeContextHandle b) noexcept {
        return a.generation == b.generation && a.serial == b.serial && a.owner == b.owner;
    }
    friend bool operator!=(RuntimeContextHandle a, RuntimeContextHandle b) noexcept {
        return !(a == b);
    }
};

// A scope, not ownership of the array. The caller promises immutable input
// until this scope ends. Its destruction revokes every service binding before
// the caller may destroy/reuse the array. Multi-entry callers keep this scope
// alive themselves, or bind an owning RuntimeHistorySource instead.
class RuntimeBatchLease {
public:
    RuntimeBatchLease(const Bar* bars, std::size_t count);
    ~RuntimeBatchLease();
    RuntimeBatchLease(const RuntimeBatchLease&) = delete;
    RuntimeBatchLease& operator=(const RuntimeBatchLease&) = delete;
private:
    struct State;
    State* state_ = nullptr;
    friend class RuntimeContextService;
};

class RuntimeHistorySource {
public:
    virtual ~RuntimeHistorySource() = default;
    virtual std::uint64_t size() const = 0;
    virtual Bar read(std::uint64_t index) const = 0;
};

struct RuntimeHistoryOptions {
    // A spill threshold, never a refusal limit. Zero uses direct spill and
    // fixed IO buffers. Neither option is read by static request machinery.
    std::size_t runtimeHistoryResidentBytes = 64u * 1024u * 1024u;
    std::optional<std::uint64_t> runtimeHistoryBudgetBytes = std::nullopt;
    // A seekable disk filesystem. A memory-backed temp directory cannot
    // satisfy the resident-memory contract. No environment-variable lookup.
    std::string runtimeHistorySpillDirectory = ".";
};

class RuntimeHistoryBudgetExhausted : public std::runtime_error, public RunFailureInfo {
public:
    RuntimeHistoryBudgetExhausted(std::uint64_t configured,
                                  std::uint64_t used, std::uint64_t needed);
    const std::uint64_t configured, used, needed;
};

struct RuntimeHistoryUsage {
    std::uint64_t accepted = 0;
    std::uint64_t accounted_bytes = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t spool_bytes = 0;
    std::uint64_t contexts = 0;
    // Categories include evaluator ownership, not just the history provider.
    // A false complete flag makes accounted_bytes an unusable UINT64_MAX
    // sentinel. The separate categories remain observed lower bounds.
    OwnedStorageUsage owned_storage{};
};

class RuntimeStorageObservationIncomplete : public std::runtime_error, public RunFailureInfo {
public:
    explicit RuntimeStorageObservationIncomplete(OwnedStorageUsage usage);
    const OwnedStorageUsage observation;
};

struct RuntimeContextEvent {
    Bar bar{};
    std::uint64_t index = 0;
    // Exclusive accepted-input frontier of THIS callback. A nested request
    // must not read beyond it, even while its parent is catching up.
    std::uint64_t frontier = 0;
};

class RuntimeContextService;
class RuntimeContextEvaluator {
public:
    virtual ~RuntimeContextEvaluator() = default;
    virtual void consume(RuntimeContextService&, RuntimeContextHandle,
                         const RuntimeContextEvent&) = 0;
    // Includes the concrete object and actual owned allocations, including
    // unused capacity and backing. Incomplete observations return UINT64_MAX.
    virtual std::uint64_t storage_bytes() const noexcept = 0;
    // Compose all evaluators into ONE observation to count shared ownership
    // once across contexts. Legacy scalar reporters cannot establish that
    // identity contract and stay explicitly incomplete under a hard budget.
    virtual void observe_owned_storage(OwnedStorage& out) const noexcept {
        out.incomplete(OwnedStorageUnsupported::legacy_evaluator);
    }
};

struct RuntimeContextKey {
    std::uint64_t template_id = 0;
    std::string semantic_arguments;
    RuntimeContextHandle enclosing{};
};

// Provider-neutral contexts. No legacy subscription ids, Pine values or
// registry references cross this interface. Handles survive new registrations;
// reset/end_run invalidate them. One service belongs to one execution thread.
class RuntimeContextService {
public:
    using Factory = std::function<std::unique_ptr<RuntimeContextEvaluator>()>;
    RuntimeContextService();
    ~RuntimeContextService();
    RuntimeContextService(const RuntimeContextService&) = delete;
    RuntimeContextService& operator=(const RuntimeContextService&) = delete;

    void configure(RuntimeHistoryOptions);
    void enable_history();
    bool history_enabled() const noexcept;
    void reset();
    void end_run() noexcept;
    void bind_batch(const RuntimeBatchLease&);
    void bind_source(std::shared_ptr<const RuntimeHistorySource>);
    void begin_live();
    void accept_batch(std::uint64_t exclusive_frontier);
    void append_live(const Bar&);
    std::uint64_t accepted_frontier() const noexcept;
    std::uint64_t callback_frontier() const noexcept;
    std::uint64_t frontier_before(std::int64_t exclusive_time_ms) const;
    RuntimeContextHandle active_context() const noexcept;

    RuntimeContextHandle get_or_create(const RuntimeContextKey&, const Factory&);
    void advance(RuntimeContextHandle, std::uint64_t exclusive_frontier);
    RuntimeContextEvaluator& published(RuntimeContextHandle);
    void release(RuntimeContextHandle);
    RuntimeHistoryUsage usage() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Impl& impl();
};

} // namespace pineforge
