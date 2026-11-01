#pragma once

#include <pineforge/detail/scratch_backing.hpp>

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace pineforge::detail {

struct ReadThroughCacheKey {
    std::uint64_t backing_identity = 0;
    std::uint64_t generation = 0;
    std::uint64_t page_number = 0;
};
inline bool operator==(ReadThroughCacheKey a, ReadThroughCacheKey b) noexcept {
    return a.backing_identity == b.backing_identity && a.generation == b.generation
        && a.page_number == b.page_number;
}
inline bool operator!=(ReadThroughCacheKey a, ReadThroughCacheKey b) noexcept { return !(a == b); }

enum class ReadThroughCacheReplacement : std::uint32_t { direct_mapped, clock, lru };
enum class ReadThroughCacheWritePolicy : std::uint32_t { write_through, write_back };

struct ReadThroughCachePolicy {
    std::size_t capacity_bytes = 0;
    std::size_t page_size = 0;
    ReadThroughCacheReplacement replacement = ReadThroughCacheReplacement::direct_mapped;
    ReadThroughCacheWritePolicy write_policy = ReadThroughCacheWritePolicy::write_through;
};

// Completed bytes, including a prefix completed before failure. Only an
// explicit backing guarantee permits restoration of a failed put's old tag.
// A short "ok" is an I/O error, never a successful fill or an unchanged write.
struct ReadThroughCacheIoResult {
    ScratchStatus status = ScratchStatus::ok;
    std::size_t transferred_bytes = 0;
    bool unchanged_on_failure = false;
    const ReportScratchIoError* failure = nullptr;
};
struct ReadThroughCacheBacking {
    void* context = nullptr;
    ReadThroughCacheIoResult (*read)(void*, ReadThroughCacheKey, ScratchBuffer) noexcept = nullptr;
    ReadThroughCacheIoResult (*write)(void*, ReadThroughCacheKey, ScratchBytes) noexcept = nullptr;
};
struct ReadThroughCacheAllocator {
    void* context = nullptr;
    void* (*allocate)(void*, std::size_t) noexcept = nullptr;
    void (*deallocate)(void*, void*, std::size_t) noexcept = nullptr;
};

// The owner supplies journal ordering, rollback-only state and publication.
// Every write in write-back mode, including bypass, goes through this port.
// discard_allowed is affirmative only after owner rollback/abandonment.
struct ReadThroughCacheTransactionOrder {
    void* context = nullptr;
    ReadThroughCacheIoResult (*write)(void*, ReadThroughCacheKey, ScratchBytes) noexcept = nullptr;
    bool (*discard_allowed)(void*) noexcept = nullptr;
};
struct ReadThroughCacheStats {
    std::uint64_t get_calls = 0;
    std::uint64_t put_calls = 0;
    std::uint64_t pin_calls = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t bypass_reads = 0;
    std::uint64_t backing_read_bytes = 0;
    std::uint64_t backing_write_bytes = 0;
    std::uint64_t evictions = 0;
    std::uint64_t dirty_evictions = 0;
    std::uint64_t allocation_bypass_count = 0;
    std::uint64_t current_pins = 0;
    std::uint64_t pin_high_water = 0;
    std::uint64_t current_payload_bytes = 0;
    std::uint64_t peak_payload_bytes = 0;
    std::uint64_t current_metadata_bytes = 0;
    std::uint64_t peak_metadata_bytes = 0;
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
    std::uint64_t io_errors = 0;
    std::uint64_t flush_errors = 0;
};

// One serialized, exclusive owner; no callback may reenter this instance.
// Callbacks, contexts and all lease workspaces are borrowed. The cache must
// outlive its leases. Every span is a nonnull 1..page_size byte page PREFIX:
// no speculative tail read, padding, logical extent inference or negative
// cache. Owners chunk longer values and perform any intra-page slicing.
//
// Optional allocation contains ALL per-slot payload, tags, pins and policy
// metadata. The fixed control object and mandatory caller transfer workspace
// are disclosed separately. No directory or allocation grows with keys.
// Cache state, replacement state and statistics never enter logical hashes.
class ReadThroughCache {
    struct Slot {
        ReadThroughCacheKey key{};
        std::size_t valid_bytes = 0;
        std::size_t pins = 0;
        std::uint64_t age = 0;
        bool valid = false;
        bool dirty = false;
        bool recent = false;
    };
    static constexpr std::size_t no_slot = std::numeric_limits<std::size_t>::max();

public:
    class ReadLease {
    public:
        ReadLease() noexcept = default;
        ~ReadLease() noexcept { reset(); }
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&& other) noexcept { *this = std::move(other); }
        ReadLease& operator=(ReadLease&& other) noexcept;
        ScratchStatus status() const noexcept { return status_; }
        explicit operator bool() const noexcept { return status_ == ScratchStatus::ok; }
        bool detached() const noexcept { return bool(*this) && slot_ == no_slot; }
        ScratchBytes value() const & noexcept { return bytes_; }
        ScratchBytes value() const && = delete;
        void reset() noexcept;

    private:
        friend class ReadThroughCache;
        explicit ReadLease(ScratchStatus status) noexcept : status_(status) {}
        ReadLease(ReadThroughCache* owner, std::size_t slot, ScratchBytes bytes) noexcept
            : owner_(owner), slot_(slot), bytes_(bytes), status_(ScratchStatus::ok) {}
        ReadThroughCache* owner_ = nullptr;
        std::size_t slot_ = no_slot;
        ScratchBytes bytes_{};
        ScratchStatus status_ = ScratchStatus::programming_error;
    };

    ReadThroughCache(ReadThroughCachePolicy policy, ReadThroughCacheBacking backing,
                     ReadThroughCacheAllocator allocator = {},
                     ReadThroughCacheTransactionOrder transaction = {}) noexcept
        : policy_(policy), backing_(backing), allocator_(allocator), transaction_(transaction) {
        const auto line = line_bytes(policy.page_size);
        if (!line || !backing.read || bool(allocator.allocate) != bool(allocator.deallocate)
            || (policy.replacement != ReadThroughCacheReplacement::direct_mapped
                && policy.replacement != ReadThroughCacheReplacement::clock
                && policy.replacement != ReadThroughCacheReplacement::lru)
            || (policy.write_policy != ReadThroughCacheWritePolicy::write_through
                && policy.write_policy != ReadThroughCacheWritePolicy::write_back)
            || (write_back() && !transaction.write)) {
            status_ = ScratchStatus::programming_error;
            return;
        }
        slots_ = policy.capacity_bytes / line;
    }
    ~ReadThroughCache() noexcept {
        // No flush, backing operation or implicit commit, even for dirty lines.
        if (!memory_) return;
        for (std::size_t i = 0; i < slots_; ++i) slot(i).~Slot();
        if (allocator_.deallocate) allocator_.deallocate(allocator_.context, memory_, allocated_bytes_);
        else ::operator delete(memory_);
    }
    ReadThroughCache(const ReadThroughCache&) = delete;
    ReadThroughCache& operator=(const ReadThroughCache&) = delete;
    ReadThroughCache(ReadThroughCache&&) = delete;
    ReadThroughCache& operator=(ReadThroughCache&&) = delete;

    ScratchStatus status() const noexcept { return status_; }
    ReadThroughCachePolicy policy() const noexcept { return policy_; }
    ReadThroughCacheStats stats() const noexcept { return stats_; }
    const ReportScratchIoError* carried_failure() const noexcept { return failure_; }
    static std::size_t line_bytes(std::size_t page_size) noexcept {
        return page_size && page_size <= std::numeric_limits<std::size_t>::max() - sizeof(Slot)
            ? sizeof(Slot) + page_size : 0;
    }
    static std::size_t control_bytes() noexcept { return sizeof(ReadThroughCache); }
    std::size_t transfer_workspace_bytes() const noexcept { return policy_.page_size; }
    std::size_t slot_count() const noexcept { return slots_; }

    ScratchStatus get(ReadThroughCacheKey key, ScratchBuffer destination) noexcept {
        increment(stats_.get_calls);
        failure_ = nullptr;
        if (!valid_span(destination.data, destination.size)) return ScratchStatus::programming_error;
        return load(key, destination).status;
    }
    ReadLease pin(ReadThroughCacheKey key, ScratchBuffer caller_workspace) noexcept {
        increment(stats_.pin_calls);
        failure_ = nullptr;
        if (!valid_span(caller_workspace.data, caller_workspace.size))
            return ReadLease(ScratchStatus::programming_error);
        const auto loaded = load(key, caller_workspace);
        if (loaded.status != ScratchStatus::ok) return ReadLease(loaded.status);
        if (loaded.slot != no_slot) ++slot(loaded.slot).pins;
        increment(stats_.current_pins);
        if (stats_.current_pins > stats_.pin_high_water)
            stats_.pin_high_water = stats_.current_pins;
        return ReadLease(this, loaded.slot,
            {loaded.slot == no_slot ? caller_workspace.data : payload(loaded.slot),
             caller_workspace.size});
    }
    void unpin(ReadLease& lease) noexcept {
        if (lease.owner_ == this) lease.reset();
    }

    ScratchStatus put(ReadThroughCacheKey key, ScratchBytes source) noexcept {
        increment(stats_.put_calls);
        failure_ = nullptr;
        if (!valid_span(source.data, source.size) || (!write_back() && !backing_.write))
            return ScratchStatus::programming_error;
        allocate_slots();
        const auto old = find(key);
        // A shorter prefix must first preserve any dirty suffix in backing.
        if (old != no_slot && slot(old).dirty && source.size < slot(old).valid_bytes) {
            const auto result = flush_slot(old);
            if (result != ScratchStatus::ok) return result;
        }
        const auto target = old != no_slot && !slot(old).pins ? old : choose(key);
        const bool dirty_victim = target != no_slot && target != old && slot(target).dirty;
        if (dirty_victim) {
            const auto result = flush_slot(target);
            if (result != ScratchStatus::ok) return result;
        }
        const Slot old_tag = old == no_slot ? Slot{} : slot(old);
        const Slot target_tag = target == no_slot ? Slot{} : slot(target);
        if (old != no_slot) invalidate_slot(old);
        if (target != no_slot) invalidate_slot(target);

        if (write_back() && target != no_slot) {
            count_eviction(target_tag, key, dirty_victim);
            install(target, key, source, true);
            return ScratchStatus::ok;
        }

        // Tags are already invalid. Bytes are untouched until complete I/O,
        // allowing restoration only under an explicit failure guarantee.
        const auto result = write(key, source, false);
        if (result.status != ScratchStatus::ok) {
            if (old != no_slot && (old_tag.dirty || result.unchanged_on_failure))
                slot(old) = old_tag;
            if (target != no_slot && target != old) slot(target) = target_tag;
            return result.status;
        }
        if (target != no_slot) {
            count_eviction(target_tag, key, dirty_victim);
            install(target, key, source, false);
        }
        return ScratchStatus::ok;
    }

    ScratchStatus invalidate(ReadThroughCacheKey key) noexcept {
        return invalidate_if([key](const Slot& line) noexcept { return line.key == key; });
    }
    ScratchStatus invalidate_all() noexcept {
        return invalidate_if([](const Slot&) noexcept { return true; });
    }
    ScratchStatus invalidate_from(std::uint64_t identity, std::uint64_t generation,
                                  std::uint64_t byte_offset) noexcept {
        if (status_ != ScratchStatus::ok) return ScratchStatus::programming_error;
        const auto first = byte_offset / policy_.page_size;
        return invalidate_if([=](const Slot& line) noexcept {
            return line.key.backing_identity == identity && line.key.generation == generation
                && line.key.page_number >= first;
        });
    }
    // Notify BEFORE owner I/O, including I/O that might partially fail.
    ScratchStatus on_append(std::uint64_t identity, std::uint64_t generation,
                            std::uint64_t old_size_bytes) noexcept {
        return invalidate_from(identity, generation, old_size_bytes);
    }
    ScratchStatus on_truncate(std::uint64_t identity, std::uint64_t generation,
                              std::uint64_t new_size_bytes) noexcept {
        return invalidate_from(identity, generation, new_size_bytes);
    }
    ScratchStatus on_generation_change(std::uint64_t identity,
                                       std::uint64_t new_generation) noexcept {
        return invalidate_if([=](const Slot& line) noexcept {
            return line.key.backing_identity == identity && line.key.generation != new_generation;
        });
    }
    ScratchStatus rebind(ReadThroughCacheBacking backing) noexcept {
        failure_ = nullptr;
        if (!backing.read) return ScratchStatus::programming_error;
        const auto result = invalidate_all();
        if (result != ScratchStatus::ok) return result;
        backing_ = backing;
        return ScratchStatus::ok;
    }
    ScratchStatus flush() noexcept {
        failure_ = nullptr;
        if (status_ != ScratchStatus::ok) return ScratchStatus::programming_error;
        if (memory_) {
            for (std::size_t i = 0; i < slots_; ++i) {
                const auto result = flush_slot(i);
                if (result != ScratchStatus::ok) return result;
            }
        }
        return ScratchStatus::ok;
    }
    ScratchStatus discard_dirty() noexcept {
        failure_ = nullptr;
        if (status_ != ScratchStatus::ok) return ScratchStatus::programming_error;
        bool dirty = false;
        if (memory_)
            for (std::size_t i = 0; i < slots_; ++i) dirty = dirty || slot(i).dirty;
        if (!dirty) return ScratchStatus::ok;
        if (!transaction_.discard_allowed || !transaction_.discard_allowed(transaction_.context))
            return ScratchStatus::programming_error;
        for (std::size_t i = 0; i < slots_; ++i)
            if (slot(i).dirty) invalidate_slot(i);
        return ScratchStatus::ok;
    }

private:
    struct Loaded {
        ScratchStatus status;
        std::size_t slot = no_slot;
    };
    static void increment(std::uint64_t& counter, std::uint64_t count = 1) noexcept {
        const auto available = std::numeric_limits<std::uint64_t>::max() - counter;
        counter += count <= available ? count : available;
    }
    bool write_back() const noexcept {
        return policy_.write_policy == ReadThroughCacheWritePolicy::write_back;
    }
    bool valid_span(const void* data, std::size_t size) const noexcept {
        return status_ == ScratchStatus::ok && data && size && size <= policy_.page_size;
    }
    Slot& slot(std::size_t i) noexcept {
        // Each Slot has an explicit lifetime in aligned allocated storage.
        return *std::launder(reinterpret_cast<Slot*>(
            static_cast<std::byte*>(memory_) + i * sizeof(Slot)));
    }
    const Slot& slot(std::size_t i) const noexcept {
        return *std::launder(reinterpret_cast<const Slot*>(
            static_cast<const std::byte*>(memory_) + i * sizeof(Slot)));
    }
    std::byte* payload(std::size_t i) noexcept {
        return static_cast<std::byte*>(memory_) + slots_ * sizeof(Slot) + i * policy_.page_size;
    }
    void allocate_slots() noexcept {
        if (!slots_ || allocation_attempted_) return;
        allocation_attempted_ = true;
        // slots_ = capacity / line_bytes, so both products and their sum fit
        // and requested allocation is <= capacity_bytes, including metadata.
        allocated_bytes_ = slots_ * line_bytes(policy_.page_size);
        memory_ = allocator_.allocate
            ? allocator_.allocate(allocator_.context, allocated_bytes_)
            : ::operator new(allocated_bytes_, std::nothrow);
        if (!memory_) {
            slots_ = 0;
            allocated_bytes_ = 0;
            increment(stats_.allocation_bypass_count);
            return;
        }
        for (std::size_t i = 0; i < slots_; ++i)
            ::new (static_cast<void*>(static_cast<std::byte*>(memory_) + i * sizeof(Slot))) Slot{};
        stats_.current_payload_bytes = slots_ * policy_.page_size;
        stats_.current_metadata_bytes = slots_ * sizeof(Slot);
        stats_.current_bytes = allocated_bytes_;
        stats_.peak_payload_bytes = stats_.current_payload_bytes;
        stats_.peak_metadata_bytes = stats_.current_metadata_bytes;
        stats_.peak_bytes = stats_.current_bytes;
    }
    std::size_t mapped(ReadThroughCacheKey key) const noexcept {
        const auto hash = key.page_number
            ^ (key.backing_identity * 0x9e3779b97f4a7c15ULL)
            ^ (key.generation * 0xbf58476d1ce4e5b9ULL);
        return static_cast<std::size_t>(hash % slots_);
    }
    std::size_t find(ReadThroughCacheKey key) const noexcept {
        if (!memory_) return no_slot;
        if (policy_.replacement == ReadThroughCacheReplacement::direct_mapped) {
            const auto i = mapped(key);
            return slot(i).valid && slot(i).key == key ? i : no_slot;
        }
        for (std::size_t i = 0; i < slots_; ++i)
            if (slot(i).valid && slot(i).key == key) return i;
        return no_slot;
    }
    std::size_t choose(ReadThroughCacheKey key) noexcept {
        if (!memory_) return no_slot;
        if (policy_.replacement == ReadThroughCacheReplacement::direct_mapped) {
            const auto i = mapped(key);
            return slot(i).pins ? no_slot : i;
        }
        for (std::size_t i = 0; i < slots_; ++i)
            if (!slot(i).valid && !slot(i).pins) return i;
        if (policy_.replacement == ReadThroughCacheReplacement::clock) {
            // Two bounded passes suffice: the first clears reference bits.
            for (unsigned pass = 0; pass < 2; ++pass) {
                for (std::size_t count = 0; count < slots_; ++count) {
                    const auto i = clock_hand_;
                    clock_hand_ = clock_hand_ + 1 == slots_ ? 0 : clock_hand_ + 1;
                    if (slot(i).pins) continue;
                    if (!slot(i).recent) return i;
                    slot(i).recent = false;
                }
            }
            return no_slot;
        }
        std::size_t victim = no_slot;
        for (std::size_t i = 0; i < slots_; ++i)
            if (!slot(i).pins && (victim == no_slot || slot(i).age > slot(victim).age))
                victim = i;
        return victim;
    }
    void touch(std::size_t i) noexcept {
        slot(i).recent = true;
        if (policy_.replacement == ReadThroughCacheReplacement::lru) {
            for (std::size_t j = 0; j < slots_; ++j) {
                auto& line = slot(j);
                if (j != i && line.valid && line.age != std::numeric_limits<std::uint64_t>::max())
                    ++line.age;
            }
            slot(i).age = 0;
        }
    }
    void invalidate_slot(std::size_t i) noexcept {
        slot(i).valid = false;
        slot(i).dirty = false;
        // Do not change pins, payload or an existing lease's snapshot.
    }
    void install(std::size_t i, ReadThroughCacheKey key, ScratchBytes bytes, bool dirty) noexcept {
        std::memmove(payload(i), bytes.data, bytes.size);
        auto& line = slot(i);
        line.key = key;
        line.valid_bytes = bytes.size;
        line.dirty = dirty;
        line.valid = true; // Publish only complete bytes.
        touch(i);
    }
    void count_eviction(const Slot& previous, ReadThroughCacheKey key, bool dirty) noexcept {
        if (previous.valid && previous.key != key) {
            increment(stats_.evictions);
            if (dirty) increment(stats_.dirty_evictions);
        }
    }
    ReadThroughCacheIoResult normalize(ReadThroughCacheIoResult result,
                                       std::size_t requested, bool reading) noexcept {
        const auto completed = result.transferred_bytes <= requested
            ? result.transferred_bytes : requested;
        increment(reading ? stats_.backing_read_bytes : stats_.backing_write_bytes, completed);
        if (result.transferred_bytes > requested) {
            result.status = ScratchStatus::programming_error;
            result.unchanged_on_failure = false;
        } else if (result.status == ScratchStatus::ok && result.transferred_bytes != requested) {
            result.status = ScratchStatus::io_error;
            result.unchanged_on_failure = false;
        }
        failure_ = result.status == ScratchStatus::ok ? nullptr : result.failure;
        if (result.status == ScratchStatus::io_error) increment(stats_.io_errors);
        return result;
    }
    ReadThroughCacheIoResult write(ReadThroughCacheKey key, ScratchBytes bytes,
                                   bool flushing) noexcept {
        const auto result = normalize(write_back()
            ? transaction_.write(transaction_.context, key, bytes)
            : backing_.write(backing_.context, key, bytes), bytes.size, false);
        if (flushing && result.status != ScratchStatus::ok) increment(stats_.flush_errors);
        return result;
    }
    ScratchStatus flush_slot(std::size_t i) noexcept {
        auto& line = slot(i);
        if (!line.valid || !line.dirty) return ScratchStatus::ok;
        const auto result = write(line.key, {payload(i), line.valid_bytes}, true);
        if (result.status == ScratchStatus::ok) line.dirty = false;
        // On ANY failure the dirty line stays valid and authoritative.
        return result.status;
    }
    Loaded load(ReadThroughCacheKey key, ScratchBuffer destination) noexcept {
        allocate_slots();
        const auto old = find(key);
        if (old != no_slot && slot(old).valid_bytes >= destination.size) {
            increment(stats_.hits);
            touch(old);
            std::memmove(destination.data, payload(old), destination.size);
            return {ScratchStatus::ok, old};
        }
        increment(stats_.misses);
        const auto target = old != no_slot && !slot(old).pins ? old : choose(key);
        const Slot previous = target == no_slot ? Slot{} : slot(target);
        const bool dirty_prefix = old != no_slot && slot(old).dirty;
        if (target != no_slot) {
            if (!previous.dirty) {
                count_eviction(previous, key, false);
                invalidate_slot(target);
            }
        } else {
            increment(stats_.bypass_reads);
        }
        if (old != no_slot && !dirty_prefix) invalidate_slot(old);
        // EVERY miss calls the reader, even if a later dirty eviction fails.
        // Keep authoritative dirty prefixes/victims until that I/O succeeds.
        const auto result = normalize(backing_.read(backing_.context, key, destination),
                                      destination.size, true);
        if (result.status != ScratchStatus::ok) return {result.status};
        if (dirty_prefix) {
            // A read longer than a pending prefix combines backing suffix
            // bytes with that authoritative prefix, without an implicit flush.
            std::memmove(destination.data, payload(old), slot(old).valid_bytes);
        }
        if (target != no_slot) {
            if (previous.dirty && target != old) {
                const auto flushed = flush_slot(target);
                if (flushed != ScratchStatus::ok) return {flushed};
                count_eviction(previous, key, true);
            }
            if (old != no_slot) invalidate_slot(old);
            invalidate_slot(target);
            install(target, key, {destination.data, destination.size}, dirty_prefix);
        }
        return {ScratchStatus::ok, target};
    }
    template<class Predicate>
    ScratchStatus invalidate_if(Predicate matches) noexcept {
        failure_ = nullptr;
        if (status_ != ScratchStatus::ok) return ScratchStatus::programming_error;
        if (!memory_) return ScratchStatus::ok;
        // Precheck the entire selection: a refusal must not partly invalidate.
        for (std::size_t i = 0; i < slots_; ++i)
            if (slot(i).valid && slot(i).dirty && matches(slot(i)))
                return ScratchStatus::programming_error;
        for (std::size_t i = 0; i < slots_; ++i)
            if (slot(i).valid && matches(slot(i))) invalidate_slot(i);
        return ScratchStatus::ok;
    }
    void release_pin(std::size_t i) noexcept {
        if (i != no_slot) --slot(i).pins;
        if (stats_.current_pins) --stats_.current_pins;
    }

    ReadThroughCachePolicy policy_;
    ReadThroughCacheBacking backing_;
    ReadThroughCacheAllocator allocator_;
    ReadThroughCacheTransactionOrder transaction_;
    ReadThroughCacheStats stats_{};
    ScratchStatus status_ = ScratchStatus::ok;
    const ReportScratchIoError* failure_ = nullptr;
    void* memory_ = nullptr;
    std::size_t slots_ = 0;
    std::size_t allocated_bytes_ = 0;
    std::size_t clock_hand_ = 0;
    bool allocation_attempted_ = false;
};

inline ReadThroughCache::ReadLease& ReadThroughCache::ReadLease::operator=(
        ReadLease&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = other.owner_;
        slot_ = other.slot_;
        bytes_ = other.bytes_;
        status_ = other.status_;
        other.owner_ = nullptr;
        other.slot_ = no_slot;
        other.bytes_ = {};
        other.status_ = ScratchStatus::programming_error;
    }
    return *this;
}
inline void ReadThroughCache::ReadLease::reset() noexcept {
    if (owner_) owner_->release_pin(slot_);
    owner_ = nullptr;
    slot_ = no_slot;
    bytes_ = {};
    status_ = ScratchStatus::programming_error;
}

} // namespace pineforge::detail
