#pragma once

// CR150-CX L4 / CR-221 scope B: contracts, not an OS backing factory.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pineforge::detail {

// No capacity/refusal status and no RunFailureCode conversion in this API.
// end: an ordered seek found no record. cancelled: a visitor returned false.
// not_found: an absent handle, a page past the extent, or nothing to reopen.
// programming_error: contract misuse or a request the host did not provision
// for, including backing exhaustion. io_error: the medium failed or a backing
// refused growth past its per-run extent-growth byte cap.
// corrupt: stored bytes failed validation.
enum class ScratchStatus : std::uint32_t {
    ok = 0,
    end,
    cancelled,
    not_found,
    programming_error,
    io_error,
    corrupt,
};

using ScratchPageId = std::uint64_t;
inline constexpr ScratchPageId scratch_no_page =
    std::numeric_limits<ScratchPageId>::max();

struct ScratchBytes {
    const std::byte* data = nullptr;
    std::size_t size = 0;
};

struct ScratchBuffer {
    std::byte* data = nullptr;
    std::size_t size = 0;
};

// The creator must select and disclose the physical medium. There is no
// default TMPDIR, memfd, tmpfs, mmap, or fallback to another medium here.
enum class ScratchBackingKind : std::uint32_t {
    disk,
    memfd,
    tmpfs,
    test_memory,
};

enum class ScratchDurability : std::uint32_t {
    buffered, // complete positional writes; no power-loss guarantee
    data,     // data and the metadata needed to retrieve it (fdatasync-like)
    full,     // data and all backing metadata (fsync-like)
};

enum class ScratchIoOperation : std::uint32_t { read_page, write_page, sync };

// One charge per attempted public operation, including errors, journal I/O,
// rollback I/O and memory-backed I/O. bytes is REQUESTED bytes, not a claim
// about physical device traffic. sync uses scratch_no_page and zero bytes.
struct ScratchIoCharge {
    ScratchBackingKind kind;
    ScratchIoOperation operation;
    ScratchPageId page;
    std::uint64_t bytes;
    ScratchStatus status;
};

// Runs synchronously inside store/backing calls; it must not call any member
// of the store or of the backing.
struct ScratchIoObserver {
    void* context = nullptr;
    void (*charge)(void*, const ScratchIoCharge&) noexcept = nullptr;
};

// Exclusive, borrowed, positional backing for one store at a time. It owns
// neither the store nor observer context. Pages are exactly page_size bytes;
// ids are [0,scratch_no_page). Reads at/after page_count return not_found.
// Successful writes extend page_count to max(old,id+1), zero-filling holes.
// A failed write MAY have partially modified its target: transactions must
// protect previously committed pages. No operation throws or allocates an
// O(page_count) userspace directory. Production backing must extend/spill;
// backing exhaustion is a programming_error, never a capacity run failure.
// An optional per-run byte cap measures extent growth from the run's initial
// extent (page_count * page_size), including zero-filled gaps, not write traffic.
// Before a growth write changes anything, refuse it with io_error if its
// proposed extent minus the initial extent exceeds the cap. Writes below the
// current extent, including rollback writes, never fail because of this cap.
// Nothing written outlives the backing object: disk/tmpfs files are unlinked
// no later than its destruction (created unlinked where the OS allows), and a
// memfd closes with it. No other object or process reopens the file. Reopen
// means a new ScratchStore on the same live backing; it does not replay the
// journal and promises no persistence or crash recovery. If a destroyed store's
// rollback is refused, contents are unspecified and the host discards the
// backing. Per-run commits use buffered.
class ScratchBacking {
public:
    virtual ~ScratchBacking() noexcept = default;
    ScratchBacking(const ScratchBacking&) = delete;
    ScratchBacking& operator=(const ScratchBacking&) = delete;
    ScratchBacking(ScratchBacking&&) = delete;
    ScratchBacking& operator=(ScratchBacking&&) = delete;

    ScratchBackingKind kind() const noexcept { return kind_; }
    std::size_t page_size() const noexcept { return page_size_; }
    virtual std::uint64_t page_count() const noexcept = 0;

    ScratchStatus read_page(ScratchPageId id, ScratchBuffer destination) const noexcept {
        const auto status = valid_page(id, destination.data, destination.size)
            ? do_read_page(id, destination) : ScratchStatus::programming_error;
        charge(ScratchIoOperation::read_page, id, destination.size, status);
        return status;
    }

    ScratchStatus write_page(ScratchPageId id, ScratchBytes source) noexcept {
        const auto status = valid_page(id, source.data, source.size)
            ? do_write_page(id, source) : ScratchStatus::programming_error;
        charge(ScratchIoOperation::write_page, id, source.size, status);
        return status;
    }

    ScratchStatus sync(ScratchDurability durability) noexcept {
        const bool valid = durability == ScratchDurability::buffered
            || durability == ScratchDurability::data || durability == ScratchDurability::full;
        const auto status = valid ? do_sync(durability) : ScratchStatus::programming_error;
        charge(ScratchIoOperation::sync, scratch_no_page, 0, status);
        return status;
    }

protected:
    ScratchBacking(ScratchBackingKind kind, std::size_t page_size,
                   ScratchIoObserver observer = {}) noexcept
        : kind_(kind), page_size_(page_size), observer_(observer) {}

    virtual ScratchStatus do_read_page(ScratchPageId, ScratchBuffer) const noexcept = 0;
    virtual ScratchStatus do_write_page(ScratchPageId, ScratchBytes) noexcept = 0;
    virtual ScratchStatus do_sync(ScratchDurability) noexcept = 0;

private:
    bool valid_page(ScratchPageId id, const void* bytes, std::size_t size) const noexcept {
        return id != scratch_no_page && page_size_ != 0 && bytes && size == page_size_;
    }
    void charge(ScratchIoOperation operation, ScratchPageId id, std::size_t bytes,
                ScratchStatus status) const noexcept {
        if (observer_.charge)
            observer_.charge(observer_.context,
                {kind_, operation, id, static_cast<std::uint64_t>(bytes), status});
    }

    const ScratchBackingKind kind_;
    const std::size_t page_size_;
    const ScratchIoObserver observer_;
};

// Test fixture only: fixed caller-owned bytes, no allocator, file, mapping or
// capacity growth. A test must provision enough BACKING pages for its entire
// workload, separately from the store's tiny resident cache. Exhausting this
// fixture is a programming error in the fixture, not a store refusal policy.
// Reopen a store against the SAME live backing object (extent is retained).
// data/full sync are unsupported: memory is not falsely advertised durable.
class FixedMemoryScratchBacking final : public ScratchBacking {
public:
    FixedMemoryScratchBacking(ScratchBuffer storage, std::size_t page_size,
                             ScratchIoObserver observer = {}) noexcept
        : ScratchBacking(ScratchBackingKind::test_memory, page_size, observer),
          storage_(storage) {}

    ScratchStatus status() const noexcept {
        return page_size() && storage_.data && storage_.size
                && storage_.size % page_size() == 0
            ? ScratchStatus::ok : ScratchStatus::programming_error;
    }
    std::uint64_t capacity_pages() const noexcept {
        return page_size() ? storage_.size / page_size() : 0;
    }
    std::uint64_t page_count() const noexcept override { return extent_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (status() != ScratchStatus::ok) return ScratchStatus::programming_error;
        if (id >= extent_) return ScratchStatus::not_found;
        const auto offset = static_cast<std::size_t>(id) * page_size();
        std::memmove(destination.data, storage_.data + offset, page_size());
        return ScratchStatus::ok;
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        if (status() != ScratchStatus::ok || id >= capacity_pages())
            return ScratchStatus::programming_error;
        const auto offset = static_cast<std::size_t>(id) * page_size();
        if (id > extent_) {
            const auto old_end = static_cast<std::size_t>(extent_) * page_size();
            std::memset(storage_.data + old_end, 0, offset - old_end);
        }
        std::memmove(storage_.data + offset, source.data, page_size());
        if (id >= extent_) extent_ = id + 1;
        return ScratchStatus::ok;
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return status() == ScratchStatus::ok && durability == ScratchDurability::buffered
            ? ScratchStatus::ok : ScratchStatus::programming_error;
    }

private:
    const ScratchBuffer storage_;
    std::uint64_t extent_ = 0;
};

} // namespace pineforge::detail
