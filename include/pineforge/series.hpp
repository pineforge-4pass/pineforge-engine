#pragma once
#include <pineforge/owned_storage.hpp>
#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/run_failure.hpp>
#include <array>
#include <atomic>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include "na.hpp"

namespace pineforge {

// Optional numeric-window residency, in 1024-value pages. The default keeps
// today's owning in-memory window. A finite smaller cache uses scratch backing;
// zero bypasses the cache. Neither setting changes logical history capacity.
struct WindowCacheCapacity {
    static constexpr std::size_t kKeepResident = std::numeric_limits<std::size_t>::max();
    std::size_t pages = kKeepResident;
    // Captured with the existing evaluation context at owner construction.
    // Later scope changes never retune an existing window or its COW clone.
    ScratchIoPolicy scratch_io_policy{};
};

inline WindowCacheCapacity& window_cache_capacity() {
    static thread_local WindowCacheCapacity capacity;
    return capacity;
}

// Constructor default for call sites built inside an evaluation context.
// Objects latch it at construction; restoring the scope cannot resize them.
class WindowCacheScope {
    WindowCacheCapacity previous_;
public:
    explicit WindowCacheScope(WindowCacheCapacity capacity)
        : previous_(window_cache_capacity()) { window_cache_capacity() = capacity; }
    ~WindowCacheScope() { window_cache_capacity() = previous_; }
    WindowCacheScope(const WindowCacheScope&) = delete;
    WindowCacheScope& operator=(const WindowCacheScope&) = delete;
};

struct WindowCacheStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0, bypass_reads = 0;
    std::uint64_t backing_reads = 0, backing_read_bytes = 0;
    std::uint64_t backing_writes = 0, backing_write_bytes = 0;
    std::uint64_t backing_read_attempts = 0, backing_write_attempts = 0;
    std::uint64_t allocation_bypasses = 0, io_errors = 0;
    std::size_t resident_payload_bytes = 0, resident_metadata_bytes = 0;
    std::size_t workspace_bytes = 0;
    std::uint64_t backing_bytes = 0, logical_payload_bytes = 0;
    bool storage_complete = true;
    bool scratch_backed = false;
};

namespace detail {

// Typed window backing and copy-out access through the shared page cache.
// Non-arithmetic values keep owning typed pages: an object handle cannot be
// serialized as native bytes without its own codec/lifetime contract.
template<class T>
class WindowValues {
public:
    static constexpr std::size_t kChunk = 1024;

    explicit WindowValues(WindowCacheCapacity capacity = window_cache_capacity())
        : capacity_(capacity) {}
    WindowValues(std::size_t size, T initial,
                 WindowCacheCapacity capacity = window_cache_capacity())
        : capacity_(capacity) { reset(size, std::move(initial)); }
    WindowValues(const WindowValues& other)
        : capacity_(other.capacity_), size_(other.size_), initial_(other.initial_),
          spilled_(other.spilled_), pages_(other.pages_), backing_(other.backing_) {}
    WindowValues& operator=(const WindowValues& other) {
        if (this != &other) {
            WindowValues copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    WindowValues(WindowValues&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : capacity_(other.capacity_), size_(other.size_), initial_(std::move(other.initial_)),
          spilled_(other.spilled_), pages_(std::move(other.pages_)),
          backing_(std::move(other.backing_)), stats_(other.stats_) {
        absorb_cache_stats(other.read_cache_stats());
        other.clear_moved_from();
    }
    WindowValues& operator=(WindowValues&& other)
            noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            // ReadThroughCache is immovable and its callback names this owner.
            // Reconstruct lazily instead of retaining the old owner's context.
            cache_.reset();
            cache_failure_ = nullptr;
            capacity_ = other.capacity_;
            size_ = other.size_;
            initial_ = std::move(other.initial_);
            spilled_ = other.spilled_;
            pages_ = std::move(other.pages_);
            backing_ = std::move(other.backing_);
            stats_ = other.stats_;
            absorb_cache_stats(other.read_cache_stats());
            other.clear_moved_from();
        }
        return *this;
    }

    void reset(std::size_t size, T initial) {
        // This is representable storage geometry, never a data/cache limit.
        if (size > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw coded<std::length_error>(RunFailureCode::scratch_capacity_exceeded,
                {{"resource", "layout"}}, "window byte extent is not representable");
        size_ = size;
        initial_ = std::move(initial);
        spilled_ = std::is_arithmetic_v<T> && capacity_.pages < page_count();
        pages_.clear();
        backing_.reset();
        cache_.reset();
        cache_failure_ = nullptr;
        stats_ = {};
        if (!spilled_) pages_.resize(page_count());
    }

    T read(std::size_t index) const {
        const std::size_t page = index / kChunk, offset = index % kChunk;
        if (!spilled_) return pages_[page] ? pages_[page]->values[offset] : initial_;
        if constexpr (std::is_arithmetic_v<T>) {
            // An unopened backing is the authoritative uniform initial value.
            if (!backing_) return initial_;
            ensure_cache();
            std::array<T, kChunk> transfer;
            cache_failure_ = nullptr;
            const auto status = cache_->get(cache_key(page),
                {reinterpret_cast<std::byte*>(transfer.data()), page_size(page) * sizeof(T)});
            if (cache_failure_)
                std::rethrow_exception(std::exchange(cache_failure_, {}));
            require_cache_status(status);
            return transfer[offset];
        }
        return initial_; // Non-arithmetic storage always takes the owning path.
    }

    void write(std::size_t index, T value) {
        const std::size_t page = index / kChunk, offset = index % kChunk;
        if (!spilled_) {
            auto& stored = pages_[page];
            if (!stored) {
                OwnedAllocator<Page> allocation;
                stored = std::allocate_shared<Page>(allocation, allocation, page_size(page), initial_);
            } else if (!stored.unique()) {
                OwnedAllocator<Page> allocation;
                stored = std::allocate_shared<Page>(allocation, allocation, *stored);
            }
            stored->values[offset] = std::move(value);
            return;
        }
        if constexpr (std::is_arithmetic_v<T>) {
            // A failed write cannot leave an old cache tag answering success.
            // Scalar writes are not page prefixes: invalidate, then retain the
            // exact existing write without a speculative read/modify/write.
            if (cache_) require_cache_status(cache_->invalidate(cache_key(page)));
            make_writable_backing();
            write_backing(*backing_, index, &value, 1);
        }
    }

    // The owner has semantically retired this entire page. Copies retain
    // their own ownership; ordinary cache eviction never calls this method.
    void retire_page(std::size_t page) {
        if (!spilled_) pages_[page].reset();
        if (cache_) require_cache_status(cache_->invalidate(cache_key(page)));
    }

    std::size_t size() const { return size_; }
    WindowCacheCapacity cache_capacity() const { return capacity_; }
    ReadThroughCachePolicy read_cache_policy() const noexcept {
        const auto bytes = sizeof(T) * kChunk;
        const auto line = ReadThroughCache::line_bytes(bytes);
        const auto requested = spilled_ ? std::min(capacity_.pages, page_count()) : 0;
        // An unrepresentable optional budget reduces residency, never data.
        const auto slots = line ? std::min(requested,
            std::numeric_limits<std::size_t>::max() / line) : 0;
        return {slots * line, bytes, ReadThroughCacheReplacement::direct_mapped,
                ReadThroughCacheWritePolicy::write_through};
    }
    ReadThroughCacheStats read_cache_stats() const noexcept {
        return cache_ ? cache_->stats() : ReadThroughCacheStats{};
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        if (!out.visit(*this)) return;
        out.logical(out.product(size_, sizeof(T)));
        ::pineforge::observe_owned_storage(out, initial_);
        // The pointer directory has its own capacity, including after clear().
        if (out.claim(pages_.data()))
            out.metadata(out.product(pages_.capacity(), sizeof(std::shared_ptr<Page>)));
        for (const auto& page : pages_) {
            if (!page || !out.claim(page.get())) continue;
            page->allocation.observe_owned_storage(out); // object + shared control allocation
            ::pineforge::observe_owned_storage(out, page->values);
        }
        if (cache_ && out.claim(&*cache_)) {
            // This immovable cache has one authoritative owner. Its public
            // counters contain the actual allocated metadata/payload bytes.
            const auto shared = read_cache_stats();
            out.metadata(shared.current_metadata_bytes);
            out.payload(shared.current_payload_bytes);
        }
        if (spilled_) out.workspace(sizeof(T) * kChunk);
        if (backing_ && out.claim(backing_.get())) {
            backing_->allocation.observe_owned_storage(out);
            backing_->file.observe_owned_storage(out);
        }
        if (cache_failure_) out.incomplete(OwnedStorageUnsupported::exception_state);
    }
    WindowCacheStats cache_stats() const {
        auto result = stats_;
        const auto shared = read_cache_stats();
        add_counter(result.hits, shared.hits);
        add_counter(result.misses, shared.misses);
        add_counter(result.evictions, shared.evictions);
        add_counter(result.bypass_reads, shared.bypass_reads);
        add_counter(result.allocation_bypasses, shared.allocation_bypass_count);
        result.scratch_backed = spilled_;
        OwnedStorage observation;
        observation.metadata(sizeof(*this));
        observe_owned_storage(observation);
        const auto owned = observation.usage();
        const auto bounded_size = [](std::uint64_t bytes) {
            return static_cast<std::size_t>(std::min<std::uint64_t>(
                bytes, std::numeric_limits<std::size_t>::max()));
        };
        result.workspace_bytes = bounded_size(owned.workspace_bytes);
        result.resident_metadata_bytes = bounded_size(owned.resident_metadata_bytes);
        result.resident_payload_bytes = bounded_size(owned.resident_payload_bytes);
        result.backing_bytes = owned.backing_bytes;
        result.logical_payload_bytes = owned.logical_payload_bytes;
        result.storage_complete = owned.complete();
        return result;
    }

private:
    struct Page {
        OwnedAllocator<Page> allocation;
        std::vector<T> values;
        Page(const OwnedAllocator<Page>& owner, std::size_t size, const T& initial)
            : allocation(owner), values(size, initial) {}
        Page(const OwnedAllocator<Page>& owner, const Page& other)
            : allocation(owner), values(other.values) {}
    };
    struct Backing {
        OwnedAllocator<Backing> allocation;
        ScratchFile file{ScratchFile::IoRaiser::auxiliary};
        // Checkpoint copies can read one backing from distinct owners.
        std::mutex io_mutex;
        Backing(const OwnedAllocator<Backing>& owner, const ScratchIoPolicy& policy)
            : allocation(owner) {
            file.bound_cache();
            file.set_scratch_io_policy(policy);
            file.open("pineforge-window-");
        }
    };
    std::size_t page_count() const noexcept { return size_ / kChunk + (size_ % kChunk != 0); }
    std::size_t page_size(std::size_t page) const {
        return std::min(kChunk, size_ - page * kChunk);
    }
    void ensure_cache() const {
        if (cache_) return;
        cache_.emplace(read_cache_policy(),
            ReadThroughCacheBacking{const_cast<WindowValues*>(this), &cache_read, nullptr});
        require_cache_status(cache_->status());
    }
    static ReadThroughCacheKey cache_key(std::size_t page) noexcept {
        // A cache belongs to one owner/backing snapshot. Reset/copy/move
        // discard it; cloning preserves all bytes except the invalidated page.
        return {1, 0, static_cast<std::uint64_t>(page)};
    }
    static ReadThroughCacheIoResult cache_read(void* context, ReadThroughCacheKey key,
                                               ScratchBuffer destination) noexcept {
        auto& owner = *static_cast<WindowValues*>(context);
        if (key.backing_identity != 1 || key.generation != 0
            || key.page_number >= owner.page_count() || !destination.data
            || !destination.size || destination.size % sizeof(T) != 0
            || destination.size > owner.page_size(static_cast<std::size_t>(key.page_number)) * sizeof(T))
            return {ScratchStatus::programming_error};
        try {
            owner.read_backing(static_cast<std::size_t>(key.page_number) * kChunk,
                               destination.data, destination.size / sizeof(T));
            return {ScratchStatus::ok, destination.size};
        } catch (...) {
            // The status port is noexcept. Rethrow the original dynamic type,
            // errno and arena/resource fields immediately after get returns.
            owner.cache_failure_ = std::current_exception();
            return {ScratchStatus::io_error};
        }
    }
    static void require_cache_status(ScratchStatus status) {
        if (status != ScratchStatus::ok)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "window read cache contract failed");
    }
    static void add_counter(std::uint64_t& value, std::uint64_t count) noexcept {
        value += std::min(count, std::numeric_limits<std::uint64_t>::max() - value);
    }
    void absorb_cache_stats(const ReadThroughCacheStats& shared) noexcept {
        add_counter(stats_.hits, shared.hits);
        add_counter(stats_.misses, shared.misses);
        add_counter(stats_.evictions, shared.evictions);
        add_counter(stats_.bypass_reads, shared.bypass_reads);
        add_counter(stats_.allocation_bypasses, shared.allocation_bypass_count);
    }
    void clear_moved_from() noexcept {
        cache_.reset();
        cache_failure_ = nullptr;
        pages_.clear();
        size_ = 0;
        spilled_ = false;
        stats_ = {};
    }
    void read_backing(std::size_t index, void* values, std::size_t count) const {
        std::lock_guard<std::mutex> lock(backing_->io_mutex);
        add_counter(stats_.backing_read_attempts, 1);
        try { backing_->file.read(index * sizeof(T), values, count * sizeof(T)); }
        catch (const std::system_error&) { add_counter(stats_.io_errors, 1); throw; }
        add_counter(stats_.backing_reads, 1);
        add_counter(stats_.backing_read_bytes, count * sizeof(T));
    }
    void write_backing(Backing& backing, std::size_t index, const T* values,
                       std::size_t count) {
        std::lock_guard<std::mutex> lock(backing.io_mutex);
        add_counter(stats_.backing_write_attempts, 1);
        try { backing.file.write(index * sizeof(T), values, count * sizeof(T)); }
        catch (const std::system_error&) { add_counter(stats_.io_errors, 1); throw; }
        add_counter(stats_.backing_writes, 1);
        add_counter(stats_.backing_write_bytes, count * sizeof(T));
    }
    void make_writable_backing() {
        if (backing_ && backing_.unique()) return;
        OwnedAllocator<Backing> allocation;
        auto replacement = std::allocate_shared<Backing>(
            allocation, allocation, capacity_.scratch_io_policy);
        std::array<T, kChunk> transfer;
        if (!backing_) transfer.fill(initial_);
        for (std::size_t page = 0; page != page_count(); ++page) {
            const auto count = page_size(page);
            if (backing_) read_backing(page * kChunk, transfer.data(), count);
            write_backing(*replacement, page * kChunk, transfer.data(), count);
        }
        // Copies/checkpoints share immutable bytes until a successful clone.
        backing_ = std::move(replacement);
    }

    WindowCacheCapacity capacity_;
    std::size_t size_ = 0;
    T initial_ = na<T>();
    bool spilled_ = false;
    std::vector<std::shared_ptr<Page>> pages_;
    std::shared_ptr<Backing> backing_;
    mutable std::optional<ReadThroughCache> cache_;
    mutable std::exception_ptr cache_failure_;
    mutable WindowCacheStats stats_;
};

} // namespace detail

template<typename T>
class DynamicRingBuffer {
    detail::WindowValues<T> buffer_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

public:
    explicit DynamicRingBuffer(std::size_t capacity,
                               WindowCacheCapacity cache = window_cache_capacity())
        : buffer_(cache), capacity_(capacity) {}

    void push_front(T val) {
        if (capacity_ == 0) return;
        // Many declared Series never execute. Materialize on first write.
        if (buffer_.size() == 0) buffer_.reset(capacity_, na<T>());
        if (size_ == 0) {
            buffer_.write(0, val);
            size_ = 1;
            head_ = 0;
        } else {
            const auto next = (head_ == 0) ? capacity_ - 1 : head_ - 1;
            buffer_.write(next, val);
            head_ = next;
            if (size_ < capacity_) {
                size_++;
            }
        }
    }

    void update_front(T val) {
        if (size_ == 0) {
            push_front(val);
        } else {
            buffer_.write(head_, val);
        }
    }

    T operator[](std::size_t offset) const {
        if (offset >= size_ || capacity_ == 0) {
            return na<T>();
        }
        // offset < size_ <= capacity_ and head_ < capacity_, so
        // head_ + offset < 2 * capacity_: one conditional subtract
        // replaces the hardware divide of `% capacity_`.
        std::size_t idx = head_ + offset;
        if (idx >= capacity_) idx -= capacity_;
        return buffer_.read(idx);
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }
    WindowCacheCapacity cache_capacity() const { return buffer_.cache_capacity(); }
    WindowCacheStats cache_stats() const { return buffer_.cache_stats(); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        buffer_.observe_owned_storage(out);
    }
    // Optional read lines only; cache_stats() also counts fixed owner metadata.
    detail::ReadThroughCacheStats read_cache_stats() const noexcept {
        return buffer_.read_cache_stats();
    }

    void clear() {
        head_ = 0;
        size_ = 0;
        buffer_.reset(0, na<T>());
    }

    void resize(std::size_t new_capacity) {
        if (new_capacity == capacity_) return;
        detail::WindowValues<T> new_buffer(buffer_.cache_capacity());
        std::size_t new_size = std::min(size_, new_capacity);
        if (new_size != 0) new_buffer.reset(new_capacity, na<T>());
        for (std::size_t i = 0; i < new_size; ++i) {
            new_buffer.write(i, (*this)[i]);
        }
        buffer_ = std::move(new_buffer);
        capacity_ = new_capacity;
        head_ = 0;
        size_ = new_size;
    }
};

/*
 * PineScript history indexing for transpiled series (close, open, user vars, etc.)
 *
 * Pine uses expr[k] with k = 0 on the current bar, k >= 1 meaning k bars into the past
 * (close[1] = previous bar). Newest values live at the front of the ring; operator[](k)
 * reads k steps into the past. Codegen emits the same k for _s_close[k] and Series vars.
 *
 * push() — new bar; update() — same bar (magnifier intrabar). Negative or out-of-range
 * offsets return na (insufficient history).
 */

template<typename T>
class Series {
    DynamicRingBuffer<T> buf;

public:
    explicit Series(int max_len = 500, WindowCacheCapacity cache = window_cache_capacity())
        : buf(max_len, cache) {}

    void push(T value) {
        buf.push_front(value);
    }

    void update(T value) {
        buf.update_front(value);
    }

    T operator[](int offset) const {
        if (offset < 0) {
            return na<T>();
        }
        return buf[static_cast<std::size_t>(offset)];
    }

    T current() const {
        return (*this)[0];
    }

    int size() const {
        return static_cast<int>(buf.size());
    }
    WindowCacheCapacity cache_capacity() const { return buf.cache_capacity(); }
    WindowCacheStats cache_stats() const { return buf.cache_stats(); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        buf.observe_owned_storage(out);
    }

    void clear() {
        buf.clear();
    }
};

} // namespace pineforge
