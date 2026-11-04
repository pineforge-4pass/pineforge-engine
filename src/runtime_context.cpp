#include <pineforge/runtime_context.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#include <linux/magic.h>
#endif

namespace pineforge {
namespace {
std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("runtime history storage size overflow");
    return a + b;
}
[[noreturn]] void storage_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
std::uint64_t next_owner() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("runtime context owner identity exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}
void discard_pages(int file, std::uint64_t offset, std::uint64_t bytes) {
#if defined(POSIX_FADV_DONTNEED)
    static const auto page = [] {
        const long size = ::sysconf(_SC_PAGESIZE);
        if (size <= 0) throw std::runtime_error("runtime history page size unavailable");
        return static_cast<std::uint64_t>(size);
    }();
    const auto begin = offset - offset % page;
    const auto end = checked_add(checked_add(offset, bytes), page - 1) / page * page;
    const int error = ::posix_fadvise(file, static_cast<off_t>(begin),
                                    static_cast<off_t>(end - begin), POSIX_FADV_DONTNEED);
    if (error) throw std::system_error(error, std::generic_category(),
                                      "runtime history release cache");
#else
    (void)file; (void)offset; (void)bytes;
#endif
}
}

struct RuntimeBatchLease::State {
    const Bar* bars;
    std::size_t count;
    bool valid = true;
    std::size_t references = 1;
    void retain() noexcept { ++references; }
    void release() noexcept { if (--references == 0) delete this; }
};
RuntimeBatchLease::RuntimeBatchLease(const Bar* bars, std::size_t count) {
    if (!bars && count) throw std::invalid_argument("runtime batch input is null");
    state_ = new State{bars, count, true, 1};
}
RuntimeBatchLease::~RuntimeBatchLease() {
    state_->valid = false;
    state_->bars = nullptr;
    state_->count = 0;
    state_->release();
}

RuntimeHistoryBudgetExhausted::RuntimeHistoryBudgetExhausted(
        std::uint64_t configured_, std::uint64_t used_, std::uint64_t needed_)
    : std::runtime_error("runtime_history_budget_exhausted: configured="
          + std::to_string(configured_) + " used=" + std::to_string(used_)
          + " needed=" + std::to_string(needed_)),
      RunFailureInfo(RunFailureCode::runtime_history_budget_exhausted,
          {{"configured", std::to_string(configured_)},
           {"used", std::to_string(used_)},
           {"needed", std::to_string(needed_)}}),
      configured(configured_), used(used_), needed(needed_) {}

RuntimeStorageObservationIncomplete::RuntimeStorageObservationIncomplete(OwnedStorageUsage usage)
    : std::runtime_error("runtime context owned storage observation is incomplete"),
      RunFailureInfo(RunFailureCode::runtime_storage_observation_incomplete,
          {{"unsupported_flags", std::to_string(usage.unsupported)},
           {"resident_metadata_bytes_lower_bound",
            std::to_string(usage.resident_metadata_bytes)},
           {"resident_payload_bytes_lower_bound",
            std::to_string(usage.resident_payload_bytes)},
           {"backing_bytes_lower_bound", std::to_string(usage.backing_bytes)},
           {"overflow", usage.overflow}}),
      observation(usage) {}

struct RuntimeContextService::Impl {
    struct Creation {
        const RuntimeContextKey& key;
        Creation* previous;
    };
    struct Entry {
        RuntimeContextKey key;
        RuntimeContextHandle handle;
        std::unique_ptr<RuntimeContextEvaluator> evaluator;
        std::unique_ptr<Entry> next;
        std::uint64_t consumed = 0;
        std::uint64_t published_frontier = 0;
        bool advancing = false;
        bool failed = false;
        bool has_published = false;
        void observe_owned_storage(OwnedStorage& out) const noexcept {
            out.metadata(sizeof(Entry));
            pineforge::observe_owned_storage(out, key.semantic_arguments);
            evaluator->observe_owned_storage(out);
        }
    };
    RuntimeHistoryOptions options;
    const std::uint64_t owner = next_owner();
    std::uint64_t generation = 1, serial = 0, accepted = 0;
    bool enabled = false, live = false, charged = false, storage_failed = false;
    RuntimeBatchLease::State* lease = nullptr;
    std::shared_ptr<const RuntimeHistorySource> source;
    std::unique_ptr<Bar[]> resident;
    std::size_t resident_capacity = 0;
    int spool = -1;
    std::uint64_t spool_bytes = 0;
    std::unique_ptr<Entry> entries;
    Entry* active = nullptr;
    Creation* creating = nullptr;
    std::uint64_t active_frontier = 0;

    ~Impl() { clear(); }
    void clear() noexcept {
        // Iterative destruction also keeps destruction stack space bounded.
        while (entries) {
            auto retired = std::move(entries);
            entries = std::move(retired->next);
        }
        if (lease) lease->release();
        lease = nullptr;
        source.reset();
        resident.reset();
        resident_capacity = 0;
        if (spool >= 0) ::close(spool);
        spool = -1;
        spool_bytes = accepted = 0;
        active = nullptr;
        creating = nullptr;
        active_frontier = 0;
        live = charged = storage_failed = false;
    }
    OwnedStorageUsage observation(const Entry* candidate = nullptr) const noexcept {
        OwnedStorage out;
        out.metadata(sizeof(Impl));
        pineforge::observe_owned_storage(out, options.runtimeHistorySpillDirectory);
        out.payload(out.product(resident_capacity, sizeof(Bar)));
        if (lease) out.metadata(sizeof(RuntimeBatchLease::State));
        // Caller-owned leases/sources are borrowed by this service; their
        // original OHLCV is not a new service-owned allocation.
        out.backing(spool_bytes);
        for (auto* e = entries.get(); e; e = e->next.get()) e->observe_owned_storage(out);
        if (candidate) candidate->observe_owned_storage(out);
        return out.usage();
    }
    void budget(std::uint64_t added = 0, std::uint64_t released = 0,
                const Entry* candidate = nullptr, bool force = false,
                std::optional<std::uint64_t> before_consume = std::nullopt) const {
        if ((!charged && !force) || !options.runtimeHistoryBudgetBytes) return;
        const auto current = observation();
        const auto proposed_storage = candidate ? observation(candidate) : current;
        if (!current.complete()) throw RuntimeStorageObservationIncomplete(current);
        if (!proposed_storage.complete())
            throw RuntimeStorageObservationIncomplete(proposed_storage);
        const auto used = before_consume.value_or(current.storage_bytes());
        if (released > proposed_storage.storage_bytes())
            throw std::logic_error("runtime history storage release exceeds ownership");
        const auto proposed = checked_add(proposed_storage.storage_bytes() - released, added);
        const auto bound = *options.runtimeHistoryBudgetBytes;
        if (proposed > bound) {
            throw RuntimeHistoryBudgetExhausted(bound, used,
                                                proposed > used ? proposed - used : 0);
        }
    }
    Entry& entry(RuntimeContextHandle h) const {
        if (!h.serial || h.generation != generation || h.owner != owner)
            throw std::logic_error("stale runtime context handle");
        for (auto* e = entries.get(); e; e = e->next.get())
            if (e->handle == h) return *e;
        throw std::logic_error("released runtime context handle");
    }
    Bar read(std::uint64_t index) const {
        if (storage_failed) throw std::logic_error("runtime history storage previously failed");
        if (index >= accepted) throw std::out_of_range("runtime history frontier");
        if (lease) {
            if (!lease->valid) throw std::logic_error("expired runtime batch lease");
            return lease->bars[index];
        }
        if (source) return source->read(index);
        if (spool < 0) {
            if (index >= resident_capacity)
                throw std::out_of_range("runtime resident history extent");
            return resident[static_cast<std::size_t>(index)];
        }
        Bar result;
        auto* out = reinterpret_cast<unsigned char*>(&result);
        std::size_t remaining = sizeof result;
        auto offset = static_cast<off_t>(index * sizeof(Bar));
        while (remaining) {
            const auto n = ::pread(spool, out, remaining, offset);
            if (n < 0) { if (errno == EINTR) continue; storage_error("runtime history read"); }
            if (!n) throw std::runtime_error("runtime history short read");
            remaining -= static_cast<std::size_t>(n);
            out += n; offset += n;
        }
        discard_pages(spool, index * sizeof(Bar), sizeof(Bar));
        return result;
    }
    static void write_at(int file, const void* data, std::size_t count,
                         std::uint64_t offset) {
        const auto* cursor = static_cast<const unsigned char*>(data);
        while (count) {
            const auto n = ::pwrite(file, cursor, count, static_cast<off_t>(offset));
            if (n < 0) { if (errno == EINTR) continue; storage_error("runtime history write"); }
            if (!n) throw std::runtime_error("runtime history short write");
            cursor += n; count -= static_cast<std::size_t>(n); offset += n;
        }
    }
    static void flush_pages(int file, std::uint64_t offset, std::uint64_t bytes) {
        while (::fdatasync(file) != 0) {
            if (errno != EINTR) storage_error("runtime history sync");
        }
        // No whole-file mapping or resident index. Drop clean backing pages;
        // the final partial page and kernel IO working space are fixed-size.
        discard_pages(file, offset, bytes);
    }
    void append(const Bar& bar) {
        if (storage_failed) throw std::logic_error("runtime history storage previously failed");
        if (accepted >= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
                            / sizeof(Bar))
            throw std::overflow_error("runtime history seek range exhausted");
        const auto required = checked_add(accepted, 1) * sizeof(Bar);
        const auto threshold = options.runtimeHistoryResidentBytes / sizeof(Bar);
        if (spool < 0 && accepted < threshold) {
            // Reserve a fixed policy window once. Never geometrically grow
            // event or index storage with the length of the input stream.
            if (!resident) {
                budget(threshold * sizeof(Bar));
                auto window = std::unique_ptr<Bar[]>(new Bar[threshold]);
                resident = std::move(window);
                resident_capacity = threshold;
            }
            resident[static_cast<std::size_t>(accepted)] = bar;
            ++accepted;
            return;
        }
        // During the spill both old resident and new disk storage exist.
        budget(required - spool_bytes);
        budget(required, resident_capacity * sizeof(Bar) + spool_bytes);
        if (spool < 0) {
#if defined(O_TMPFILE)
            const int candidate = ::open(options.runtimeHistorySpillDirectory.c_str(),
                                          O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#else
            auto path = options.runtimeHistorySpillDirectory + "/.pineforge-runtime-XXXXXX";
            const int candidate = ::mkstemp(path.data());
#endif
            if (candidate < 0) storage_error("runtime history create spool");
            try {
#if !defined(O_TMPFILE)
                if (::unlink(path.c_str()) != 0) storage_error("runtime history unlink spool");
                if (::fcntl(candidate, F_SETFD, FD_CLOEXEC) < 0)
                    storage_error("runtime history spool close-on-exec");
#endif
#if defined(__linux__)
                struct statfs filesystem{};
                if (::fstatfs(candidate, &filesystem) != 0)
                    storage_error("runtime history spool filesystem");
                if (filesystem.f_type == TMPFS_MAGIC || filesystem.f_type == RAMFS_MAGIC)
                    throw std::runtime_error("runtime history spill directory is memory-backed");
#endif
                if (resident)
                    write_at(candidate, resident.get(), static_cast<std::size_t>(accepted) * sizeof(Bar), 0);
                write_at(candidate, &bar, sizeof bar, accepted * sizeof(Bar));
                flush_pages(candidate, 0, required);
            } catch (...) { ::close(candidate); throw; }
            spool = candidate;
            resident.reset();
            resident_capacity = 0;
        } else {
            try {
                write_at(spool, &bar, sizeof bar, accepted * sizeof(Bar));
                flush_pages(spool, accepted * sizeof(Bar), sizeof(Bar));
            } catch (...) {
                storage_failed = true;
                struct stat status{};
                if (::fstat(spool, &status) == 0 && status.st_size >= 0)
                    spool_bytes = static_cast<std::uint64_t>(status.st_size);
                throw;
            }
        }
        spool_bytes = required;
        ++accepted;
    }
};

RuntimeContextService::RuntimeContextService() = default;
RuntimeContextService::~RuntimeContextService() = default;
RuntimeContextService::Impl& RuntimeContextService::impl() {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return *impl_;
}
void RuntimeContextService::configure(RuntimeHistoryOptions options) {
    auto& p = impl();
    if (p.accepted || p.entries || p.active || p.creating)
        throw std::logic_error("runtime history options changed during a run");
    p.options = std::move(options);
}
void RuntimeContextService::enable_history() { impl().enabled = true; }
bool RuntimeContextService::history_enabled() const noexcept { return impl_ && impl_->enabled; }
void RuntimeContextService::reset() {
    if (!impl_) return;
    auto& p = *impl_;
    if (p.active || p.creating)
        throw std::logic_error("runtime context reset during replay or creation");
    if (p.generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("runtime context generation exhausted");
    p.clear();
    ++p.generation;
}
void RuntimeContextService::end_run() noexcept {
    if (!impl_) return;
    // Called only after all public-entry callbacks have unwound.
    if (impl_->active || impl_->creating) std::terminate();
    impl_->clear();
    // Serial numbers never repeat, even if the generation is exhausted.
    if (impl_->generation != std::numeric_limits<std::uint64_t>::max())
        ++impl_->generation;
}
void RuntimeContextService::bind_batch(const RuntimeBatchLease& lease) {
    auto& p = impl();
    if (p.accepted || p.entries || p.source || p.spool >= 0 || p.live || p.lease)
        throw std::logic_error("runtime history already bound");
    if (!lease.state_->valid) throw std::logic_error("expired runtime batch lease");
    p.lease = lease.state_;
    p.lease->retain();
}
void RuntimeContextService::bind_source(std::shared_ptr<const RuntimeHistorySource> source) {
    if (!source) throw std::invalid_argument("runtime history source is null");
    auto& p = impl();
    if (p.accepted || p.entries || p.lease || p.live || p.source)
        throw std::logic_error("runtime history already bound");
    p.source = std::move(source);
}
void RuntimeContextService::begin_live() {
    auto& p = impl();
    if (p.accepted || p.lease || p.source || p.entries)
        throw std::logic_error("runtime history already bound");
    p.live = true;
}
void RuntimeContextService::accept_batch(std::uint64_t frontier) {
    auto& p = impl();
    const auto count = p.lease && p.lease->valid ? p.lease->count
        : p.source ? p.source->size() : 0;
    if (p.live || (!p.lease && !p.source) || (p.lease && !p.lease->valid))
        throw std::logic_error("runtime batch lease is not live");
    if (frontier < p.accepted || frontier > count)
        throw std::out_of_range("runtime batch frontier");
    p.accepted = frontier;
}
void RuntimeContextService::append_live(const Bar& bar) {
    auto& p = impl();
    if (!p.live) throw std::logic_error("runtime live history is not bound");
    p.append(bar);
}
std::uint64_t RuntimeContextService::accepted_frontier() const noexcept {
    return impl_ ? impl_->accepted : 0;
}
std::uint64_t RuntimeContextService::callback_frontier() const noexcept {
    return impl_ && impl_->active ? impl_->active_frontier : accepted_frontier();
}
std::uint64_t RuntimeContextService::frontier_before(std::int64_t instant) const {
    if (!impl_) return 0;
    std::uint64_t first = 0, last = callback_frontier();
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (impl_->read(middle).timestamp < instant) first = middle + 1;
        else last = middle;
    }
    return first;
}
RuntimeContextHandle RuntimeContextService::active_context() const noexcept {
    return impl_ && impl_->active ? impl_->active->handle : RuntimeContextHandle{};
}
RuntimeContextHandle RuntimeContextService::get_or_create(
        const RuntimeContextKey& key, const Factory& factory) {
    auto& p = impl();
    if (key.enclosing.serial) p.entry(key.enclosing);
    for (auto* e = p.entries.get(); e; e = e->next.get())
        if (e->key.template_id == key.template_id
            && e->key.semantic_arguments == key.semantic_arguments
            && e->key.enclosing == key.enclosing) return e->handle;
    for (auto* creation = p.creating; creation; creation = creation->previous)
        if (creation->key.template_id == key.template_id
            && creation->key.semantic_arguments == key.semantic_arguments
            && creation->key.enclosing == key.enclosing)
            throw std::logic_error("runtime context creation cycle");
    if (p.serial == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("runtime context serial exhausted");
    auto candidate = std::make_unique<Impl::Entry>();
    candidate->key = key;
    Impl::Creation creation{key, p.creating};
    p.creating = &creation;
    try { candidate->evaluator = factory(); }
    catch (...) { p.creating = creation.previous; throw; }
    p.creating = creation.previous;
    if (!candidate->evaluator) throw std::invalid_argument("null runtime evaluator");
    if (key.enclosing.serial) p.entry(key.enclosing);
    p.budget(0, 0, candidate.get(), true);
    candidate->handle = {p.generation, ++p.serial, p.owner};
    const auto handle = candidate->handle;
    candidate->next = std::move(p.entries);
    p.entries = std::move(candidate);
    p.charged = true;
    return handle;
}
void RuntimeContextService::advance(RuntimeContextHandle handle, std::uint64_t frontier) {
    auto& p = impl();
    auto& e = p.entry(handle);
    if (frontier > callback_frontier() || frontier < e.consumed)
        throw std::out_of_range("runtime context accepted frontier");
    if (e.advancing) throw std::logic_error("runtime context replay cycle");
    if (e.failed) throw std::logic_error("runtime context replay previously failed");
    const auto previous = p.active;
    const auto previous_frontier = p.active_frontier;
    e.advancing = true;
    p.active = &e;
    try {
        while (e.consumed < frontier) {
            p.active_frontier = e.consumed + 1;
            const RuntimeContextEvent event{p.read(e.consumed), e.consumed,
                                             p.active_frontier};
            std::optional<std::uint64_t> used;
            if (p.options.runtimeHistoryBudgetBytes) {
                const auto before = p.observation();
                if (!before.complete()) throw RuntimeStorageObservationIncomplete(before);
                used = before.storage_bytes();
            }
            e.evaluator->consume(*this, handle, event);
            p.budget(0, 0, nullptr, false, used);
            ++e.consumed;
        }
        e.published_frontier = frontier;
        e.has_published = true;
        e.advancing = false;
        p.active = previous; p.active_frontier = previous_frontier;
    } catch (...) {
        e.failed = true; e.advancing = false;
        p.active = previous; p.active_frontier = previous_frontier;
        throw;
    }
}
RuntimeContextEvaluator& RuntimeContextService::published(RuntimeContextHandle handle) {
    auto& e = impl().entry(handle);
    if (e.advancing || e.failed || !e.has_published)
        throw std::logic_error("runtime context has no published result");
    return *e.evaluator;
}
void RuntimeContextService::release(RuntimeContextHandle handle) {
    auto& p = impl();
    auto& e = p.entry(handle);
    if (e.advancing) throw std::logic_error("release of advancing runtime context");
    for (auto* c = p.entries.get(); c; c = c->next.get())
        if (c->key.enclosing == handle)
            throw std::logic_error("runtime context still owns nested contexts");
    auto* link = &p.entries;
    while ((*link)->handle != handle) link = &(*link)->next;
    auto retired = std::move(*link);
    *link = std::move(retired->next);
}
RuntimeHistoryUsage RuntimeContextService::usage() const noexcept {
    if (!impl_) return {};
    const auto observed = impl_->observation();
    RuntimeHistoryUsage result{impl_->accepted, observed.storage_bytes(),
                                observed.resident_bytes(), impl_->spool_bytes, 0, observed};
    for (auto* e = impl_->entries.get(); e; e = e->next.get()) ++result.contexts;
    return result;
}
} // namespace pineforge
