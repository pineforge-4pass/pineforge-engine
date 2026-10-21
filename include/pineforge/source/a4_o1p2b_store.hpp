#pragma once

// CR-221 scope B, A4 rows P22-P25 (lane core-cr150-o1p2b): the adapter's exact
// ordered source inventory on the shared ScratchStore contract
// (<pineforge/detail/scratch_store.hpp>, lane CX-O1C).
//
// Every population below is an EXTERNAL ordered store: fixed resident
// cache/journal/index slots are prepared once, records live in the backing,
// iteration is ordered by a caller comparator, and a full resident slot spills
// instead of refusing. The store keeps the row's existing fill order, set
// membership semantics or dictionary key order, so every hash reader in
// pine_state_hash.cpp folds the same byte stream it folded before (the readers
// are not edited by this lane).
//
// The typed view is deliberately narrow: size/empty/resident bound, ordered
// iteration materialising ONE row, point lookup by the row's own key, the
// primitive handle walk a multi-row pass needs (first/next/load/store/erase
// inside one explicit transaction), and a raw value view for the noexcept
// fee readers. No owning sequence is produced anywhere on a per-bar path.
//
// The generic substrate is lane-local on purpose: the sibling lanes'
// equivalent headers are not on this branch (see REPORT.md), so this file
// carries its own copy under namespace a4_o1p2b and nothing here depends on
// another lane's file.

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/detail/scratch_readback.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace pineforge::source {
namespace a4_o1p2b {

using pineforge::detail::ScratchBuffer;
using pineforge::detail::ScratchBytes;
using pineforge::detail::ScratchByteSource;
using pineforge::detail::ScratchDurability;
using pineforge::detail::ScratchField;
using pineforge::detail::ScratchHandle;
using pineforge::detail::ScratchRecordInfo;
using pineforge::detail::ScratchStatus;
using pineforge::detail::ScratchStore;
using pineforge::detail::ScratchStoreConfig;
using pineforge::detail::scratch_no_handle;

inline constexpr std::size_t kPageSize = 4096;
// RESIDENT cache slots, not a record limit: filling one spills to the backing.
inline constexpr std::size_t kResidentPages = 8;
inline constexpr std::size_t kJournalSlots = 8;
inline constexpr std::size_t kIndexSlots = 8;
// This layer's admitted upper bound on one fixed journal/index slot. The
// admitted resident workspace is therefore a constant independent of the
// population (the lab's growth gate measures the process itself; this reports
// the workspace the adapter admits).
inline constexpr std::size_t kSlotBytes = 64;
inline constexpr std::size_t kResidentBytes =
    kResidentPages * kPageSize + (kJournalSlots + kIndexSlots) * kSlotBytes;
// Fixed value-read workspace for the raw (no decoded row) readers, per table.
// It is a prepared row bound, not a record count: a value larger than this is
// a programming error in the codec, never a population limit.
inline constexpr std::size_t kRawRowBytes = 4096;

// Result of a raw value walk. `unavailable` means a live row could not be
// read or parsed: the caller must not treat the rows it saw as the whole
// population (a fold that silently stops would publish a different digest).
enum class RawWalk : std::uint8_t { complete, stopped, unavailable };

inline std::uint64_t load_le64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index)
        value = (value << 8) | bytes[index];
    return value;
}
inline std::uint32_t load_le32(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index)
        value = (value << 8) | bytes[index];
    return value;
}
inline void store_le64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    for (int index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8;
    }
}
inline void store_le32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (int index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8;
    }
}

// A value source that streams one record's fields straight from the caller's
// record: no materialised blob, no allocation, bytes borrowed for this put.
class Segments final : public ScratchByteSource {
public:
    void clear() noexcept {
        count_ = 0;
        size_ = 0;
        overflow_ = false;
    }
    void bytes(const void* data, std::size_t size) noexcept {
        if (size == 0) return;
        if (count_ == kMaxSegments) {
            overflow_ = true;
            return;
        }
        segments_[count_] = {static_cast<const std::uint8_t*>(data), size};
        ++count_;
        size_ += size;
    }
    void u8(std::uint8_t value) noexcept {
        if (!reserve_local()) return;
        local_[count_][0] = value;
        add_local(1);
    }
    void u32(std::uint32_t value) noexcept {
        if (!reserve_local()) return;
        store_le32(local_[count_], value);
        add_local(4);
    }
    void u64(std::uint64_t value) noexcept {
        if (!reserve_local()) return;
        store_le64(local_[count_], value);
        add_local(8);
    }
    void f64(double value) noexcept {
        if (!reserve_local()) return;
        std::memcpy(local_[count_], &value, sizeof value);
        add_local(8);
    }
    // Length-prefixed byte string: u32 length, then the exact bytes.
    void text(const std::string& value) noexcept {
        if (value.size() > 0xffffffffULL) {
            overflow_ = true;
            return;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(value.data(), value.size());
    }

    std::uint64_t size() const noexcept override { return size_; }
    bool overflowed() const noexcept { return overflow_; }

    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (overflow_ || offset > size_ || destination.size > size_ - offset)
            return ScratchStatus::programming_error;
        std::uint64_t skip = offset;
        std::size_t written = 0;
        for (std::size_t index = 0; index < count_ && written < destination.size; ++index) {
            const auto& segment = segments_[index];
            if (skip >= segment.size) {
                skip -= segment.size;
                continue;
            }
            const auto take = std::min<std::size_t>(
                segment.size - static_cast<std::size_t>(skip),
                destination.size - written);
            std::memcpy(destination.data + written, segment.data + skip, take);
            written += take;
            skip = 0;
        }
        return written == destination.size ? ScratchStatus::ok
                                           : ScratchStatus::programming_error;
    }

private:
    static constexpr std::size_t kMaxSegments = 24;
    struct Segment {
        const std::uint8_t* data;
        std::size_t size;
    };
    bool reserve_local() noexcept {
        if (count_ == kMaxSegments) {
            overflow_ = true;
            return false;
        }
        return true;
    }
    void add_local(std::size_t size) noexcept {
        if (!reserve_local()) return;
        segments_[count_] = {local_[count_], size};
        ++count_;
        size_ += size;
    }
    Segment segments_[kMaxSegments]{};
    std::uint8_t local_[kMaxSegments][8]{};
    std::size_t count_ = 0;
    std::uint64_t size_ = 0;
    bool overflow_ = false;
};

// Complete key byte streams, compared without materialising either one.
inline ScratchStatus compare_bytes(void*, const ScratchByteSource& left,
                                   const ScratchByteSource& right, int& order) noexcept {
    std::uint8_t left_chunk[256];
    std::uint8_t right_chunk[256];
    const auto limit = std::min(left.size(), right.size());
    std::uint64_t offset = 0;
    while (offset < limit) {
        const auto chunk = std::min<std::uint64_t>(sizeof left_chunk, limit - offset);
        const ScratchBuffer left_buffer{reinterpret_cast<std::byte*>(left_chunk),
                                        static_cast<std::size_t>(chunk)};
        const ScratchBuffer right_buffer{reinterpret_cast<std::byte*>(right_chunk),
                                         static_cast<std::size_t>(chunk)};
        if (left.read(offset, left_buffer) != ScratchStatus::ok
            || right.read(offset, right_buffer) != ScratchStatus::ok)
            return ScratchStatus::programming_error;
        const int difference = std::memcmp(left_chunk, right_chunk,
                                           static_cast<std::size_t>(chunk));
        if (difference != 0) {
            order = difference < 0 ? -1 : 1;
            return ScratchStatus::ok;
        }
        offset += chunk;
    }
    order = left.size() < right.size() ? -1 : left.size() > right.size() ? 1 : 0;
    return ScratchStatus::ok;
}

// Little-endian 64-bit primary keys: insertion sequence (ledger, pyramiding,
// bound exits) and the callsite token (ascending, as the std::map was).
inline ScratchStatus compare_u64(void*, const ScratchByteSource& left,
                                 const ScratchByteSource& right, int& order) noexcept {
    std::uint8_t left_bytes[8];
    std::uint8_t right_bytes[8];
    const ScratchBuffer left_buffer{reinterpret_cast<std::byte*>(left_bytes), 8};
    const ScratchBuffer right_buffer{reinterpret_cast<std::byte*>(right_bytes), 8};
    if (left.size() != 8 || right.size() != 8
        || left.read(0, left_buffer) != ScratchStatus::ok
        || right.read(0, right_buffer) != ScratchStatus::ok)
        return ScratchStatus::programming_error;
    const auto first = load_le64(left_bytes);
    const auto second = load_le64(right_bytes);
    order = first < second ? -1 : first > second ? 1 : 0;
    return ScratchStatus::ok;
}

// One adapter-owned store's disk backing. ScratchFile is the repository's
// unlinked positional scratch (run directory, close-on-exec, pread/pwrite), so
// the store spills by extending its file and never refuses capacity. The
// medium is stated here explicitly; CX-O1C's real backings replace this one
// when they land (REPORT.md).
class FileBacking final : public pineforge::detail::ScratchBacking {
public:
    FileBacking(std::string directory, std::string prefix, std::size_t page_size)
        : pineforge::detail::ScratchBacking(
              pineforge::detail::ScratchBackingKind::disk, page_size),
          file_(),
          directory_(std::move(directory)),
          prefix_(std::move(prefix)) {}

    bool open() noexcept {
        // ScratchFile::open throws on every failure path and otherwise leaves
        // an unlinked, close-on-exec positional file open.
        try {
            file_.set_directory(directory_);
            file_.open(prefix_.c_str());
        } catch (...) {
            return false;
        }
        return true;
    }

    std::uint64_t page_count() const noexcept override { return extent_; }

protected:
    ScratchStatus do_read_page(pineforge::detail::ScratchPageId id,
                               ScratchBuffer destination) const noexcept override {
        if (id >= extent_) return ScratchStatus::not_found;
        try {
            file_.read(id * page_size(), destination.data, destination.size);
        } catch (...) {
            return ScratchStatus::io_error;
        }
        return ScratchStatus::ok;
    }
    ScratchStatus do_write_page(pineforge::detail::ScratchPageId id,
                                ScratchBytes source) noexcept override {
        try {
            file_.write(id * page_size(), source.data, source.size);
        } catch (...) {
            return ScratchStatus::io_error;
        }
        if (id >= extent_) extent_ = id + 1;
        return ScratchStatus::ok;
    }
    // The store commits with `buffered`: the file is already unlinked and this
    // layer claims no power-loss durability for it.
    ScratchStatus do_sync(ScratchDurability) noexcept override {
        return ScratchStatus::ok;
    }

private:
    mutable pineforge::detail::ScratchFile file_;
    std::string directory_;
    std::string prefix_;
    std::uint64_t extent_ = 0;
};

// Shared ownership, lifecycle and primitive access for one ordered store.
class StoreOwner {
public:
    StoreOwner(const char* prefix, std::uint64_t order_id,
               ScratchStatus (*compare)(void*, const ScratchByteSource&,
                                        const ScratchByteSource&, int&) noexcept)
        : prefix_(prefix), order_id_(order_id), compare_(compare) {}
    StoreOwner(const StoreOwner&) = delete;
    StoreOwner& operator=(const StoreOwner&) = delete;
    StoreOwner(StoreOwner&&) = delete;
    StoreOwner& operator=(StoreOwner&&) = delete;
    ~StoreOwner() = default;

    void set_directory(const std::string& directory) {
        if (store_ && directory != directory_)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "a4 ordered store directory changed during run");
        directory_ = directory;
    }

    bool empty() const noexcept { return size() == 0; }
    std::size_t size() const noexcept {
        return store_ ? static_cast<std::size_t>(store_->record_count()) : 0;
    }
    std::size_t resident_bytes() const noexcept { return kResidentBytes; }
    std::size_t resident_limit() const noexcept { return kResidentBytes; }

    // Drops the store and its backing, as a new run starts.
    void reset() noexcept {
        store_.reset();
        backing_.reset();
    }

    // Opens on first use. Failure is an environment/implementation failure
    // (the store contract prescribes no run failure for it), never a capacity
    // refusal: a full resident slot spills, it does not fail.
    bool ensure_open() {
        if (store_) return true;
        auto backing = std::make_unique<FileBacking>(directory_, prefix_, kPageSize);
        if (!backing->open()) return false;
        ScratchStoreConfig config;
        config.page_count = kResidentPages;
        config.page_size = kPageSize;
        config.journal_slots = kJournalSlots;
        config.index_slots = kIndexSlots;
        config.order.id = order_id_;
        config.order.context = nullptr;
        config.order.compare = compare_;
        config.mode = pineforge::detail::ScratchOpenMode::create_empty;
        auto store = std::make_unique<ScratchStore>(config, backing.get());
        if (store->status() != ScratchStatus::ok) return false;
        backing_ = std::move(backing);
        store_ = std::move(store);
        return true;
    }

    void require_open() {
        if (!ensure_open())
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered store open");
    }

    const ScratchStore* store() const noexcept { return store_.get(); }
    ScratchStore* mutable_store() noexcept { return store_.get(); }

    bool first(ScratchHandle& handle) const noexcept {
        handle = scratch_no_handle;
        return store_ && store_->first(handle) == ScratchStatus::ok
            && handle != scratch_no_handle;
    }
    bool next(ScratchHandle current, ScratchHandle& handle) const noexcept {
        handle = scratch_no_handle;
        return store_ && store_->next(current, handle) == ScratchStatus::ok
            && handle != scratch_no_handle;
    }
    bool point(ScratchHandle handle, ScratchRecordInfo& info) const noexcept {
        return store_ && store_->point(handle, info) == ScratchStatus::ok;
    }
    bool read_key(ScratchHandle handle, std::uint8_t (&out)[8]) const noexcept {
        ScratchRecordInfo info;
        if (!point(handle, info) || info.key_bytes != 8) return false;
        ScratchBuffer buffer{reinterpret_cast<std::byte*>(out), 8};
        return store_->read(handle, ScratchField::key, 0, buffer) == ScratchStatus::ok;
    }

    // One explicit transaction per booking/update; a failed mutation leaves the
    // pre-begin state and is reported as the environment failure it is.
    void begin_batch() {
        require_open();
        if (store_->begin() != ScratchStatus::ok)
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered store begin");
    }
    void commit_batch() {
        if (store_ && store_->commit(ScratchDurability::buffered) != ScratchStatus::ok)
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered store commit");
    }
    void rollback_batch() noexcept {
        if (store_) (void)store_->rollback();
    }
    void put(const ScratchByteSource& key, const ScratchByteSource& value) {
        ScratchHandle result = scratch_no_handle;
        if (!store_ || store_->put(key, value, result) != ScratchStatus::ok)
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered store put");
    }
    void erase(ScratchHandle handle) noexcept {
        if (store_) (void)store_->erase(handle);
    }

    // Erase every row in one transaction, keeping the open store and its
    // fixed resident slots (a bar/cycle boundary reuses them).
    void clear_rows() {
        if (!store_) return;
        Batch batch(*this);
        std::optional<ScratchHandle> next_handle;
        ScratchHandle handle = scratch_no_handle;
        if (first(handle)) next_handle = handle;
        while (next_handle) {
            const auto current = *next_handle;
            ScratchHandle following = scratch_no_handle;
            const bool has_following = next(current, following);
            erase(current);
            next_handle = has_following ? std::optional<ScratchHandle>(following)
                                        : std::nullopt;
        }
        batch.commit();
    }

    // A transaction that rolls back unless committed, so an exception inside
    // a booking never leaves the store with an active transaction.
    class Batch {
    public:
        explicit Batch(StoreOwner& owner) : owner_(owner) { owner_.begin_batch(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        ~Batch() {
            if (!committed_) owner_.rollback_batch();
        }
        void commit() {
            owner_.commit_batch();
            committed_ = true;
        }

    private:
        StoreOwner& owner_;
        bool committed_ = false;
    };

private:
    std::string prefix_;
    std::uint64_t order_id_;
    ScratchStatus (*compare_)(void*, const ScratchByteSource&,
                              const ScratchByteSource&, int&) noexcept;
    std::string directory_;
    std::unique_ptr<FileBacking> backing_;
    std::unique_ptr<ScratchStore> store_;
};

// Value decoding helpers: bounded reads through the store's own page read.
inline bool read_bounded(const ScratchStore& store, ScratchHandle handle,
                         ScratchField field, std::uint64_t offset,
                         void* destination, std::size_t size) noexcept {
    if (size == 0) return true;
    const ScratchBuffer buffer{static_cast<std::byte*>(destination), size};
    return store.read(handle, field, offset, buffer) == ScratchStatus::ok;
}
inline bool read_u8(const ScratchStore& store, ScratchHandle handle,
                    std::uint64_t& offset, std::uint8_t& value) noexcept {
    return read_bounded(store, handle, ScratchField::value, offset, &value, 1)
        && ((offset += 1), true);
}
inline bool read_u32(const ScratchStore& store, ScratchHandle handle,
                     std::uint64_t& offset, std::uint32_t& value) noexcept {
    std::uint8_t bytes[4];
    if (!read_bounded(store, handle, ScratchField::value, offset, bytes, 4)) return false;
    value = load_le32(bytes);
    offset += 4;
    return true;
}
inline bool read_u64(const ScratchStore& store, ScratchHandle handle,
                     std::uint64_t& offset, std::uint64_t& value) noexcept {
    std::uint8_t bytes[8];
    if (!read_bounded(store, handle, ScratchField::value, offset, bytes, 8)) return false;
    value = load_le64(bytes);
    offset += 8;
    return true;
}
inline bool read_f64(const ScratchStore& store, ScratchHandle handle,
                     std::uint64_t& offset, double& value) noexcept {
    std::uint8_t bytes[8];
    if (!read_bounded(store, handle, ScratchField::value, offset, bytes, 8)) return false;
    std::memcpy(&value, bytes, sizeof value);
    offset += 8;
    return true;
}
// The id strings are the record's own payload; materialising one row at a time
// is the same copy the owning containers made, not a resident population.
inline bool read_text(const ScratchStore& store, ScratchHandle handle,
                      std::uint64_t& offset, std::string& value) {
    std::uint32_t length = 0;
    if (!read_u32(store, handle, offset, length)) return false;
    value.assign(length, '\0');
    std::uint8_t chunk[256];
    std::size_t done = 0;
    while (done < length) {
        const auto take = std::min<std::size_t>(sizeof chunk, length - done);
        if (!read_bounded(store, handle, ScratchField::value, offset + done, chunk, take))
            return false;
        std::memcpy(value.data() + done, chunk, take);
        done += take;
    }
    offset += length;
    return true;
}

// The full key bytes compared against a probe, without materialising the key.
inline bool key_equals(const ScratchStore& store, ScratchHandle handle,
                       const ScratchRecordInfo& info, ScratchBytes probe) noexcept {
    if (info.key_bytes != probe.size) return false;
    std::uint8_t chunk[256];
    std::uint64_t offset = 0;
    while (offset < probe.size) {
        const auto take = std::min<std::uint64_t>(sizeof chunk, probe.size - offset);
        const ScratchBuffer buffer{reinterpret_cast<std::byte*>(chunk),
                                   static_cast<std::size_t>(take)};
        if (store.read(handle, ScratchField::key, offset, buffer) != ScratchStatus::ok)
            return false;
        if (std::memcmp(chunk, probe.data + offset, static_cast<std::size_t>(take)) != 0)
            return false;
        offset += take;
    }
    return true;
}

// Read one complete key as a string (string-keyed rows fold the key itself).
inline bool read_key_text(const ScratchStore& store, ScratchHandle handle,
                          const ScratchRecordInfo& info, std::string& value) {
    value.assign(static_cast<std::size_t>(info.key_bytes), '\0');
    std::uint8_t chunk[256];
    std::uint64_t offset = 0;
    while (offset < info.key_bytes) {
        const auto take = std::min<std::uint64_t>(sizeof chunk, info.key_bytes - offset);
        const ScratchBuffer buffer{reinterpret_cast<std::byte*>(chunk),
                                   static_cast<std::size_t>(take)};
        if (store.read(handle, ScratchField::key, offset, buffer) != ScratchStatus::ok)
            return false;
        std::memcpy(value.data() + offset, chunk, static_cast<std::size_t>(take));
        offset += take;
    }
    return true;
}

// Insertion-ordered table: key = little-endian sequence, value = the record.
// Iteration is fill order; `store(handle, row)` rewrites one row in place as
// an upsert under the SAME key, so the handle and the order survive.
template<class Codec>
class SeqTable {
public:
    using Row = typename Codec::Row;
    using value_type = Row;

    SeqTable(const char* prefix, std::uint64_t order_id)
        : owner_(prefix, order_id, &compare_u64),
          raw_(kRawRowBytes, std::byte{0}) {}

    void set_directory(const std::string& directory) { owner_.set_directory(directory); }
    void reset() noexcept {
        owner_.reset();
        next_seq_ = 1;
        last_seq_ = 0;
    }
    // Emptying a queue is "no rows", exactly what the vector's clear() meant.
    // Dropping the store (rather than replaying one erase per row) keeps the
    // call noexcept, and the next push reopens an empty backing under the
    // bound directory. A never-opened queue needs no work at all.
    void clear() noexcept { reset(); }
    bool empty() const noexcept { return owner_.empty(); }
    std::size_t size() const noexcept { return owner_.size(); }
    std::size_t resident_bytes() const noexcept {
        return owner_.resident_bytes() + raw_.capacity();
    }
    std::size_t resident_limit() const noexcept {
        return owner_.resident_limit() + raw_.capacity();
    }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;

        const_iterator() = default;
        reference operator*() const { return *row_; }
        pointer operator->() const { return &*row_; }
        const_iterator& operator++() {
            advance();
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator before = *this;
            advance();
            return before;
        }
        bool operator==(const const_iterator& other) const noexcept {
            return owner_ == other.owner_ && handle_ == other.handle_;
        }
        bool operator!=(const const_iterator& other) const noexcept {
            return !(*this == other);
        }

    private:
        friend class SeqTable;
        const_iterator(const SeqTable* owner, ScratchHandle handle)
            : owner_(owner), handle_(handle) {
            load();
        }
        void load() {
            row_.reset();
            if (!owner_ || handle_ == scratch_no_handle) {
                handle_ = scratch_no_handle;
                return;
            }
            // A LIVE handle that cannot be decoded is a corrupt store, not the
            // end of the set: failing closed here keeps a hash fold from
            // silently reading fewer rows than the size it just folded.
            Row row;
            if (!owner_->load(handle_, row))
                pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered row decode");
            row_.emplace(std::move(row));
        }
        void advance() {
            if (!owner_ || handle_ == scratch_no_handle) return;
            ScratchHandle following = scratch_no_handle;
            if (!owner_->owner_.next(handle_, following)) {
                handle_ = scratch_no_handle;
                row_.reset();
                return;
            }
            handle_ = following;
            load();
        }
        const SeqTable* owner_ = nullptr;
        ScratchHandle handle_ = scratch_no_handle;
        std::optional<Row> row_;
    };

    const_iterator begin() const {
        ScratchHandle handle = scratch_no_handle;
        if (!owner_.first(handle)) return end();
        return const_iterator(this, handle);
    }
    const_iterator end() const { return const_iterator(this, scratch_no_handle); }

    // Appends one record at the next fill sequence.
    void push_back(const Row& row) {
        std::uint8_t encoded[8];
        store_le64(encoded, next_seq_);
        Segments value;
        Codec::encode(row, value);
        // The prepared row bound, not a population limit: every reader below
        // walks one row in the fixed value workspace, so a row that outgrows
        // it is a codec/programming error and is REFUSED here rather than
        // stored and silently skipped by a later pass.
        if (value.overflowed() || value.size() > raw_.size())
            pineforge::detail::raise_scratch_io_error(
                EMSGSIZE, "a4 ordered row exceeds the prepared workspace");
        const pineforge::detail::ScratchSpanSource key_source{
            ScratchBytes{reinterpret_cast<const std::byte*>(encoded), 8}};
        StoreOwner::Batch batch(owner_);
        owner_.put(key_source, value);
        last_seq_ = next_seq_;
        ++next_seq_;
        batch.commit();
    }

    // The newest live record (the ledger's `back()`), if any.
    bool back(Row& row) const {
        if (last_seq_ == 0) return false;
        return load_sequence(last_seq_, row);
    }
    // Rewrites the newest live record under its own key.
    void store_back(const Row& row) {
        ScratchHandle handle = scratch_no_handle;
        if (!find_sequence(last_seq_, handle)) return;
        StoreOwner::Batch batch(owner_);
        store_handle(handle, row);
        batch.commit();
    }

    // Primitive walk used by the bookings: capture the successor BEFORE any
    // mutation of the current handle.
    bool first(ScratchHandle& handle) const noexcept { return owner_.first(handle); }
    bool next(ScratchHandle current, ScratchHandle& handle) const noexcept {
        return owner_.next(current, handle);
    }
    bool load(ScratchHandle handle, Row& row) const {
        ScratchRecordInfo info;
        if (!owner_.point(handle, info)) return false;
        return Codec::decode(*owner_.store(), handle, info, row);
    }
    void store(ScratchHandle handle, const Row& row) { store_handle(handle, row); }
    void erase(ScratchHandle handle) noexcept { owner_.erase(handle); }
    // Erase a row the caller just located: reports an unserviceable erase so a
    // transactional pass never continues over a row it believes is gone.
    bool erase_checked(ScratchHandle handle) noexcept {
        ScratchStore* store = owner_.mutable_store();
        return store && store->erase(handle) == ScratchStatus::ok;
    }

    // Raw value view for the noexcept readers and for in-place arithmetic on
    // a fixed value buffer: no decoded Row, no string materialisation and no
    // allocation. The bytes alias raw_ and stay valid only until the next raw
    // call. Callers inside an active transaction may store_raw() them back
    // under the row's own key.
    bool load_raw(ScratchHandle handle, ScratchBytes& bytes) const noexcept {
        if (!owner_.store()) return false;
        ScratchRecordInfo info;
        if (!owner_.point(handle, info)) return false;
        if (info.value_bytes > raw_.size()) return false;
        if (info.value_bytes != 0) {
            const ScratchBuffer buffer{raw_.data(),
                                       static_cast<std::size_t>(info.value_bytes)};
            if (owner_.store()->read(handle, ScratchField::value, 0, buffer)
                != ScratchStatus::ok)
                return false;
        }
        bytes = ScratchBytes{raw_.data(), static_cast<std::size_t>(info.value_bytes)};
        return true;
    }
    bool store_raw(ScratchHandle handle, ScratchBytes bytes) noexcept {
        ScratchStore* store = owner_.mutable_store();
        if (!store) return false;
        std::uint8_t key[8];
        if (!owner_.read_key(handle, key)) return false;
        const pineforge::detail::ScratchSpanSource key_source{
            ScratchBytes{reinterpret_cast<const std::byte*>(key), 8}};
        const pineforge::detail::ScratchSpanSource value_source{bytes};
        ScratchHandle result = scratch_no_handle;
        return store->put(key_source, value_source, result) == ScratchStatus::ok
            && result == handle;
    }
    // Ordered walk over value bytes (no decode). The visitor returns false to
    // stop. noexcept so the fee readers keep their noexcept contract, but the
    // result separates "the visitor stopped" from "a live row is unreadable",
    // so a caller never mistakes a truncated walk for the whole population.
    template<class Visit>
    RawWalk for_each_raw(Visit&& visit) const noexcept {
        if (!owner_.store()) return RawWalk::complete; // no store, no rows
        ScratchHandle handle = scratch_no_handle;
        for (bool more = owner_.first(handle); more;) {
            ScratchHandle following = scratch_no_handle;
            more = owner_.next(handle, following);
            ScratchBytes bytes;
            if (!load_raw(handle, bytes)) return RawWalk::unavailable;
            if (!visit(bytes)) return RawWalk::stopped;
            handle = following;
        }
        return RawWalk::complete;
    }
    void begin_batch() { owner_.begin_batch(); }
    void commit_batch() { owner_.commit_batch(); }
    void rollback_batch() noexcept { owner_.rollback_batch(); }

    // Transaction guard for a multi-record booking (rolls back on exception).
    class Batch {
    public:
        explicit Batch(SeqTable& table) : owner_(table.owner_) { owner_.begin_batch(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        ~Batch() {
            if (!committed_) owner_.rollback_batch();
        }
        void commit() {
            owner_.commit_batch();
            committed_ = true;
        }

    private:
        StoreOwner& owner_;
        bool committed_ = false;
    };
    Batch batch() { return Batch(*this); }

    // Removes every row the predicate selects, preserving the order of the
    // rest (the containers' erase(remove_if(...)) replacement).
    template<class Predicate>
    void erase_if(Predicate&& predicate) {
        if (!owner_.ensure_open()) return;
        Batch guard(*this);
        ScratchHandle handle = scratch_no_handle;
        for (bool more = owner_.first(handle); more;) {
            ScratchHandle following = scratch_no_handle;
            more = owner_.next(handle, following);
            Row row;
            if (load(handle, row) && predicate(row)) owner_.erase(handle);
            handle = following;
        }
        refresh_last();
        guard.commit();
    }

    // Recomputes the newest live sequence after a booking erased rows.
    void refresh_last() noexcept {
        std::uint64_t last = 0;
        ScratchHandle handle = scratch_no_handle;
        for (bool more = owner_.first(handle); more;) {
            std::uint8_t key[8];
            if (!owner_.read_key(handle, key)) break;
            last = load_le64(key);
            ScratchHandle following = scratch_no_handle;
            more = owner_.next(handle, following);
            handle = following;
        }
        last_seq_ = last;
    }

private:
    bool load_sequence(std::uint64_t sequence, Row& row) const {
        ScratchHandle handle = scratch_no_handle;
        return find_sequence(sequence, handle) && load(handle, row);
    }
    bool find_sequence(std::uint64_t sequence, ScratchHandle& handle) const {
        const ScratchStore* store = owner_.store();
        if (!store) return false;
        std::uint8_t key[8];
        store_le64(key, sequence);
        const pineforge::detail::ScratchSpanSource probe{
            ScratchBytes{reinterpret_cast<const std::byte*>(key), 8}};
        ScratchHandle found = scratch_no_handle;
        if (store->lower_bound(probe, found) != ScratchStatus::ok) return false;
        ScratchRecordInfo info;
        if (!owner_.point(found, info)) return false;
        if (!key_equals(*store, found, info, probe_bytes(key))) return false;
        handle = found;
        return true;
    }
    static ScratchBytes probe_bytes(const std::uint8_t (&key)[8]) noexcept {
        return ScratchBytes{reinterpret_cast<const std::byte*>(key), 8};
    }
    void store_handle(ScratchHandle handle, const Row& row) {
        std::uint8_t key[8];
        if (!owner_.read_key(handle, key))
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered row key read");
        Segments value;
        Codec::encode(row, value);
        const pineforge::detail::ScratchSpanSource key_source{
            ScratchBytes{reinterpret_cast<const std::byte*>(key), 8}};
        owner_.put(key_source, value);
    }

    StoreOwner owner_;
    std::uint64_t next_seq_ = 1;
    std::uint64_t last_seq_ = 0;
    // The one fixed value workspace the raw readers share (mutable so a const
    // reader can fill it; no reader retains it past its call).
    mutable std::vector<std::byte> raw_;
};

// Key-ordered table: key = the row's own key bytes (source id, tombstone key,
// callsite token), value = the record fields. Iteration is the key's ascending
// order, which is the order the owning std::map (or the hash's sorted key
// vector) produced.
template<class Codec>
class KeyedTable {
public:
    using Key = typename Codec::Key;
    using Row = typename Codec::Row;
    using value_type = Row;
    static constexpr bool key_is_u64 = std::is_same_v<Key, std::uint64_t>;

    KeyedTable(const char* prefix, std::uint64_t order_id)
        : owner_(prefix, order_id, key_is_u64 ? &compare_u64 : &compare_bytes) {}

    void set_directory(const std::string& directory) { owner_.set_directory(directory); }
    void reset() noexcept { owner_.reset(); }
    // Same "no rows" result as the owning map's clear(), without a replay and
    // without a throw, so the noexcept callers (begin_source_evaluation) keep
    // their contract.
    void clear() noexcept { owner_.reset(); }
    bool empty() const noexcept { return owner_.empty(); }
    std::size_t size() const noexcept { return owner_.size(); }
    std::size_t resident_bytes() const noexcept { return owner_.resident_bytes(); }
    std::size_t resident_limit() const noexcept { return owner_.resident_limit(); }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;

        const_iterator() = default;
        reference operator*() const { return *row_; }
        pointer operator->() const { return &*row_; }
        const_iterator& operator++() {
            advance();
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator before = *this;
            advance();
            return before;
        }
        bool operator==(const const_iterator& other) const noexcept {
            return owner_ == other.owner_ && handle_ == other.handle_;
        }
        bool operator!=(const const_iterator& other) const noexcept {
            return !(*this == other);
        }

    private:
        friend class KeyedTable;
        const_iterator(const KeyedTable* owner, ScratchHandle handle)
            : owner_(owner), handle_(handle) {
            load();
        }
        void load() {
            row_.reset();
            if (!owner_ || handle_ == scratch_no_handle) {
                handle_ = scratch_no_handle;
                return;
            }
            Row row;
            if (!owner_->load(handle_, row))
                pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered row decode");
            row_.emplace(std::move(row));
        }
        void advance() {
            if (!owner_ || handle_ == scratch_no_handle) return;
            ScratchHandle following = scratch_no_handle;
            if (!owner_->owner_.next(handle_, following)) {
                handle_ = scratch_no_handle;
                row_.reset();
                return;
            }
            handle_ = following;
            load();
        }
        const KeyedTable* owner_ = nullptr;
        ScratchHandle handle_ = scratch_no_handle;
        std::optional<Row> row_;
    };

    const_iterator begin() const {
        ScratchHandle handle = scratch_no_handle;
        if (!owner_.first(handle)) return end();
        return const_iterator(this, handle);
    }
    const_iterator end() const { return const_iterator(this, scratch_no_handle); }

    bool find(const Key& key, Row& row) const {
        ScratchHandle handle = scratch_no_handle;
        return find_handle(key, handle) && load(handle, row);
    }
    bool contains(const Key& key) const noexcept {
        ScratchHandle handle = scratch_no_handle;
        return find_handle(key, handle);
    }
    // std::map::at semantics on the value half of the row (the hash reader
    // calls at() on the std::unordered_map member it replaced): absence is a
    // caller invariant violation.
    auto at(const Key& key) const {
        Row row;
        if (!find(key, row))
            throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {},
                                           "a4 ordered store key");
        return row.second;
    }
    void upsert(const Key& key, const Row& row) {
        StoreOwner::Batch batch(owner_);
        store_key(key, row);
        batch.commit();
    }
    void erase_key(const Key& key) {
        ScratchHandle handle = scratch_no_handle;
        if (!find_handle(key, handle)) return;
        StoreOwner::Batch batch(owner_);
        owner_.erase(handle);
        batch.commit();
    }

    bool load(ScratchHandle handle, Row& row) const {
        ScratchRecordInfo info;
        if (!owner_.point(handle, info)) return false;
        return Codec::decode(*owner_.store(), handle, info, row);
    }
    // Primitive walk (capture the successor before any mutation), used by a
    // multi-row transaction and by the rollback witness.
    bool first(ScratchHandle& handle) const noexcept { return owner_.first(handle); }
    bool next(ScratchHandle current, ScratchHandle& handle) const noexcept {
        return owner_.next(current, handle);
    }
    void erase_handle(ScratchHandle handle) noexcept { owner_.erase(handle); }
    void begin_batch() { owner_.begin_batch(); }
    void commit_batch() { owner_.commit_batch(); }
    void rollback_batch() noexcept { owner_.rollback_batch(); }

    class Batch {
    public:
        explicit Batch(KeyedTable& table) : owner_(table.owner_) { owner_.begin_batch(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        ~Batch() {
            if (!committed_) owner_.rollback_batch();
        }
        void commit() {
            owner_.commit_batch();
            committed_ = true;
        }

    private:
        StoreOwner& owner_;
        bool committed_ = false;
    };
    Batch batch() { return Batch(*this); }

private:
    static pineforge::detail::ScratchSpanSource probe_source(
            const Key& key, std::uint8_t (&storage)[8]) noexcept {
        if constexpr (key_is_u64) {
            store_le64(storage, key);
            return pineforge::detail::ScratchSpanSource{
                ScratchBytes{reinterpret_cast<const std::byte*>(storage), 8}};
        } else {
            (void)storage;
            return pineforge::detail::ScratchSpanSource{
                ScratchBytes{reinterpret_cast<const std::byte*>(key.data()), key.size()}};
        }
    }
    bool find_handle(const Key& key, ScratchHandle& handle) const noexcept {
        const ScratchStore* store = owner_.store();
        if (!store) return false;
        std::uint8_t storage[8];
        const auto probe = probe_source(key, storage);
        ScratchHandle found = scratch_no_handle;
        if (store->lower_bound(probe, found) != ScratchStatus::ok) return false;
        ScratchRecordInfo info;
        if (!owner_.point(found, info)) return false;
        bool equal = false;
        if constexpr (key_is_u64) {
            equal = key_equals(*store, found, info,
                               ScratchBytes{reinterpret_cast<const std::byte*>(storage), 8});
        } else {
            equal = key_equals(*store, found, info,
                               ScratchBytes{reinterpret_cast<const std::byte*>(key.data()),
                                            key.size()});
        }
        if (!equal) return false;
        handle = found;
        return true;
    }
    void store_key(const Key& key, const Row& row) {
        std::uint8_t storage[8];
        const auto source = probe_source(key, storage);
        Segments value;
        Codec::encode(row, value);
        owner_.put(source, value);
    }

    StoreOwner owner_;
};

// Set of 64-bit keys held as an ordered store. The owning containers were
// unordered_set<std::uint64_t>; membership, insert-once and erase-count are
// the only reads, and every hash reader sorts the set before folding, so
// ascending key order yields the identical byte stream.
class U64SetTable {
public:
    using value_type = std::uint64_t;

    U64SetTable(const char* prefix, std::uint64_t order_id)
        : owner_(prefix, order_id, &compare_u64) {}

    void set_directory(const std::string& directory) { owner_.set_directory(directory); }
    void reset() noexcept { owner_.reset(); }
    // noexcept: the two callers (a bar boundary and a source evaluation
    // start) must not turn a store fault into std::terminate; the observable
    // state after either is the empty set the caller asked for.
    void clear() noexcept { owner_.reset(); }
    bool empty() const noexcept { return owner_.empty(); }
    std::size_t size() const noexcept { return owner_.size(); }
    std::size_t resident_bytes() const noexcept { return owner_.resident_bytes(); }
    std::size_t resident_limit() const noexcept { return owner_.resident_limit(); }

    // unordered_set::insert semantics: a present key leaves the set unchanged.
    void insert(std::uint64_t value) {
        if (!owner_.ensure_open())
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered set open");
        if (contains(value)) return;
        std::uint8_t key[8];
        store_le64(key, value);
        const pineforge::detail::ScratchSpanSource key_source{
            ScratchBytes{reinterpret_cast<const std::byte*>(key), 8}};
        // One marker byte: the key alone carries the value, and no reader
        // decodes it. (A non-empty value avoids depending on how a backing
        // treats a zero-length record.)
        Segments marker;
        marker.u8(1U);
        StoreOwner::Batch batch(owner_);
        owner_.put(key_source, marker);
        batch.commit();
    }

    bool contains(std::uint64_t value) const noexcept {
        ScratchHandle handle = scratch_no_handle;
        return find_handle(value, handle);
    }

    // unordered_set::erase(key) returns the number removed (0 or 1).
    std::size_t erase(std::uint64_t value) {
        ScratchHandle handle = scratch_no_handle;
        if (!find_handle(value, handle)) return 0;
        StoreOwner::Batch batch(owner_);
        if (!erase_checked(handle))
            pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered set erase");
        batch.commit();
        return 1;
    }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::uint64_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const std::uint64_t*;
        using reference = std::uint64_t;

        const_iterator() = default;
        reference operator*() const noexcept { return value_; }
        const_iterator& operator++() {
            advance();
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator before = *this;
            advance();
            return before;
        }
        bool operator==(const const_iterator& other) const noexcept {
            return owner_ == other.owner_ && handle_ == other.handle_;
        }
        bool operator!=(const const_iterator& other) const noexcept {
            return !(*this == other);
        }

    private:
        friend class U64SetTable;
        const_iterator(const U64SetTable* owner, ScratchHandle handle)
            : owner_(owner), handle_(handle) {
            load();
        }
        void load() {
            value_ = 0;
            if (!owner_ || handle_ == scratch_no_handle) {
                handle_ = scratch_no_handle;
                return;
            }
            std::uint64_t value = 0;
            if (!owner_->value_of(handle_, value))
                pineforge::detail::raise_scratch_io_error(EIO, "a4 ordered set row read");
            value_ = value;
        }
        void advance() {
            if (!owner_ || handle_ == scratch_no_handle) return;
            ScratchHandle following = scratch_no_handle;
            if (!owner_->owner_.next(handle_, following)) {
                handle_ = scratch_no_handle;
                value_ = 0;
                return;
            }
            handle_ = following;
            load();
        }
        const U64SetTable* owner_ = nullptr;
        ScratchHandle handle_ = scratch_no_handle;
        std::uint64_t value_ = 0;
    };

    const_iterator begin() const {
        ScratchHandle handle = scratch_no_handle;
        if (!owner_.first(handle)) return end();
        return const_iterator(this, handle);
    }
    const_iterator end() const { return const_iterator(this, scratch_no_handle); }

    // Primitive walks a caller may need (capture the successor first).
    bool first(ScratchHandle& handle) const noexcept { return owner_.first(handle); }
    bool next(ScratchHandle current, ScratchHandle& handle) const noexcept {
        return owner_.next(current, handle);
    }
    void erase_handle(ScratchHandle handle) noexcept { owner_.erase(handle); }
    void begin_batch() { owner_.begin_batch(); }
    void commit_batch() { owner_.commit_batch(); }
    void rollback_batch() noexcept { owner_.rollback_batch(); }

private:
    bool erase_checked(ScratchHandle handle) noexcept {
        ScratchStore* store = owner_.mutable_store();
        return store && store->erase(handle) == ScratchStatus::ok;
    }
    bool value_of(ScratchHandle handle, std::uint64_t& value) const noexcept {
        std::uint8_t key[8];
        if (!owner_.read_key(handle, key)) return false;
        value = load_le64(key);
        return true;
    }
    bool find_handle(std::uint64_t value, ScratchHandle& handle) const noexcept {
        const ScratchStore* store = owner_.store();
        if (!store) return false;
        std::uint8_t key[8];
        store_le64(key, value);
        const pineforge::detail::ScratchSpanSource probe{
            ScratchBytes{reinterpret_cast<const std::byte*>(key), 8}};
        ScratchHandle found = scratch_no_handle;
        if (store->lower_bound(probe, found) != ScratchStatus::ok) return false;
        ScratchRecordInfo info;
        if (!owner_.point(found, info)) return false;
        if (!key_equals(*store, found, info,
                        ScratchBytes{reinterpret_cast<const std::byte*>(key), 8}))
            return false;
        handle = found;
        return true;
    }

    StoreOwner owner_;
};

} // namespace a4_o1p2b
} // namespace pineforge::source
