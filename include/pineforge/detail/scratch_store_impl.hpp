#pragma once

// Scope-B. A header-only definition of CommandScratchStore, kept
// apart from the contract header so it can be replaced without touching the
// declarations. Included by scratch_store.hpp; never include it first.
//
// Design in one paragraph: fixed-size pages with a 16-byte reserved trailer; a
// copy-in copy-out page cache (no pointer into the cache survives a call); an
// in-place undo journal whose images are written through before the first
// change of a page in a transaction (a stamp in each page header says whether
// that already happened); two superblocks written alternately at commit; an
// ordered B+tree of handles compared through the caller's comparator; a radix
// table from handle to blob locations; blobs as chains of 64-byte cells or of
// whole pages; free lists for pages and cells. All growth is in the backing.
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>

namespace pineforge::detail {
namespace scratch_impl {

using Byte = unsigned char;

inline std::uint64_t ld64(const Byte* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
    return value;
}
inline void st64(Byte* p, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<Byte>((value >> (8 * i)) & 0xFF);
}
inline std::uint32_t ld32(const Byte* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void st32(Byte* p, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<Byte>((value >> (8 * i)) & 0xFF);
}

struct Fold {
    std::uint64_t value = 14695981039346656037ull;
    void bytes(const Byte* data, std::size_t length) noexcept {
        for (std::size_t i = 0; i < length; ++i) {
            value ^= data[i];
            value *= 1099511628211ull;
        }
    }
    void le64(std::uint64_t v) noexcept {
        Byte raw[8];
        st64(raw, v);
        bytes(raw, 8);
    }
};

constexpr std::uint64_t kMagic = 0x3142535243534650ull;
constexpr std::uint64_t kVersion = 1;
constexpr std::uint32_t kTypeIndexLeaf = 2;
constexpr std::uint32_t kTypeIndexNode = 3;
constexpr std::uint32_t kTypeHandleLeaf = 4;
constexpr std::uint32_t kTypeHandleNode = 5;
constexpr std::uint32_t kTypeCells = 6;
constexpr std::uint32_t kTypeBlob = 7;
constexpr std::uint32_t kTypeFree = 8;
constexpr std::size_t kHeader = 16;
constexpr std::size_t kTrailer = 16;
constexpr std::size_t kCell = 64;
constexpr std::size_t kCellPayload = 56;
constexpr std::size_t kWorkPages = 6;
constexpr std::uint64_t kLive = 1;
constexpr std::uint64_t kGhost = 2;
constexpr unsigned kMaxDepth = 48;
// Handle-table height: 60 slots per node of the smallest page reach 2^62 handles in 11 levels.
constexpr std::size_t kMaxHeight = 12;
constexpr std::size_t kSuperFields = 18;
constexpr std::uint64_t kNoSlot = UINT64_MAX;

class Core {
public:
    using Status = ScratchStatus;

    struct Super {
        std::uint64_t seq = 0;
        std::uint64_t next_handle = 1;
        std::uint64_t record_count = 0;
        std::uint64_t hw = 2;
        std::uint64_t free_head = 0;
        std::uint64_t hroot = 0;
        std::uint64_t hheight = 0;
        std::uint64_t iroot = 0;
        std::uint64_t iheight = 0;
        std::uint64_t cell_free = 0;
        std::uint64_t cell_bump = 0;
        std::uint64_t cell_next = 0;
        std::uint64_t pool_head = 0;
    };

    struct Entry {
        std::uint64_t key_loc = 0;
        std::uint64_t val_loc = 0;
        std::uint64_t key_len = 0;
        std::uint64_t val_word = 0;  // (value length << 2) | kLive or kGhost; zero is dead
    };

    struct Slot {
        std::uint64_t id;
        std::uint64_t tick;
        std::int32_t next;
        bool valid;
        bool dirty;
    };

    struct BlobPos {
        std::uint64_t index = 0;
        std::uint64_t elem = 0;
    };

    struct Path {
        std::uint64_t page[kMaxDepth];
        std::uint32_t slot[kMaxDepth];
        unsigned depth = 0;
    };

    // A stored key or value as a byte source. It resolves logical bytes through
    // the cache on every read and keeps only a chain cursor.
    class BlobSource final : public ScratchByteSource {
    public:
        BlobSource(Core* core, std::uint64_t loc, std::uint64_t len) noexcept
            : core_(core), loc_(loc), len_(len) {}
        std::uint64_t size() const noexcept override { return len_; }
        ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
            if (offset > len_ || destination.size > len_ - offset
                || (destination.size != 0 && destination.data == nullptr))
                return ScratchStatus::programming_error;
            return core_->blob_read(loc_, offset, destination.size, reinterpret_cast<Byte*>(destination.data), pos_);
        }

    private:
        Core* core_;
        std::uint64_t loc_;
        std::uint64_t len_;
        mutable BlobPos pos_{};
    };

    // ---- geometry -----------------------------------------------------------

    static bool geometry_ok(const ScratchStoreConfig& c, const ScratchBacking* b) noexcept {
        return b != nullptr && c.page_count >= scratch_min_page_count && c.page_count <= (std::size_t{1} << 24)
            && c.page_size >= scratch_min_page_size && c.page_size <= (std::size_t{1} << 20)
            && (c.page_size & (c.page_size - 1)) == 0 && c.journal_slots >= 1 && c.index_slots >= 1
            && c.order.id != 0 && c.order.compare != nullptr && c.page_size == b->page_size();
    }
    static std::size_t round_up(std::size_t n, std::size_t a) noexcept { return (n + a - 1) / a * a; }
    static std::size_t bucket_count(std::size_t slots) noexcept {
        std::size_t n = 4;
        while (n < slots * 2) n <<= 1;
        return n;
    }
    static std::size_t required_bytes(const ScratchStoreConfig& c, const ScratchBacking* b) noexcept {
        std::size_t total = round_up(sizeof(Core), 16);
        if (!geometry_ok(c, b)) return total;
        total += round_up(c.page_count * sizeof(Slot), 16);
        total += round_up(bucket_count(c.page_count) * sizeof(std::int32_t), 64);
        total += (c.page_count + kWorkPages) * c.page_size;
        return total;
    }

    Core(const ScratchStoreConfig& c, ScratchBacking* b, std::size_t total) noexcept : total_(total) {
        if (!geometry_ok(c, b)) {
            open_status_ = Status::programming_error;
            return;
        }
        backing_ = b;
        order_ = c.order;
        page_ = c.page_size;
        usable_ = page_ - kTrailer;
        slots_ = c.page_count;
        nbuckets_ = bucket_count(slots_);
        leaf_cap_ = (usable_ - 32) / 8;
        node_cap_ = (usable_ - 24) / 16;
        hleaf_cap_ = (usable_ - 16) / 32;
        hnode_cap_ = (usable_ - 16) / 8;
        cells_per_page_ = (usable_ - 16) / kCell;
        blob_payload_ = usable_ - 24;
        cell_max_ = page_ / 2;
        Byte* base = reinterpret_cast<Byte*>(this);
        std::size_t off = round_up(sizeof(Core), 16);
        slot_ = reinterpret_cast<Slot*>(base + off);
        off += round_up(slots_ * sizeof(Slot), 16);
        bucket_ = reinterpret_cast<std::int32_t*>(base + off);
        off += round_up(nbuckets_ * sizeof(std::int32_t), 64);
        cache_ = base + off;
        off += slots_ * page_;
        w0_ = base + off;
        off += page_;
        w1_ = base + off;
        off += page_;
        w2_ = base + off;
        off += page_;
        w3_ = base + off;
        off += page_;
        jbuf_ = base + off;
        off += page_;
        sbuf_ = base + off;
        std::memset(slot_, 0, slots_ * sizeof(Slot));
        for (std::size_t i = 0; i < slots_; ++i) slot_[i].next = -1;
        for (std::size_t i = 0; i < nbuckets_; ++i) bucket_[i] = -1;
        if (c.mode == ScratchOpenMode::create_empty) {
            if (b->page_count() != 0) open_status_ = Status::programming_error;
        } else if (c.mode == ScratchOpenMode::reopen) {
            open_status_ = reopen_load();
        } else {
            open_status_ = Status::programming_error;
        }
    }

    // An abandoned transaction is undone; the destructor never commits.
    void shutdown() noexcept {
        if (open_status_ == Status::ok && active_) (void)rollback_impl();
    }

    // ---- backing traffic ----------------------------------------------------

    void note_backing_failure(Status status) noexcept {
        if (status != Status::io_error) return;
        backing_io_failed_ = true;
        // Diagnostic transport, not durable logical state. Successful cleanup
        // I/O must not erase the exception for the status an owner will raise.
        // A later status-only I/O failure replaces it; a fresh begin resets it.
        carried_failure_.reset();
        if (const auto* failure = backing_->carried_failure()) carried_failure_ = *failure;
    }
    Status bread(std::uint64_t id, Byte* destination) noexcept {
        ++reads_;
        const Status st = backing_->read_page(id, ScratchBuffer{reinterpret_cast<std::byte*>(destination), page_});
        note_backing_failure(st);
        return st;
    }
    Status bwrite(std::uint64_t id, const Byte* source) noexcept {
        ++writes_;
        const Status st = backing_->write_page(id, ScratchBytes{reinterpret_cast<const std::byte*>(source), page_});
        note_backing_failure(st);
        return st;
    }
    Status bsync(ScratchDurability durability) noexcept {
        ++syncs_;
        const Status st = backing_->sync(durability);
        note_backing_failure(st);
        return st;
    }
    static Status structural(Status status) noexcept {
        return status == Status::not_found ? Status::corrupt : status;
    }

    // ---- the page cache: copy in, copy out ---------------------------------

    Byte* page_at(std::int32_t s) noexcept { return cache_ + static_cast<std::size_t>(s) * page_; }
    std::size_t bucket_of(std::uint64_t id) const noexcept {
        return static_cast<std::size_t>(((id + 1) * 0x9E3779B97F4A7C15ull) >> 40) & (nbuckets_ - 1);
    }
    std::int32_t find(std::uint64_t id) noexcept {
        std::int32_t s = bucket_[bucket_of(id)];
        while (s >= 0) {
            if (slot_[s].valid && slot_[s].id == id) return s;
            s = slot_[s].next;
        }
        return -1;
    }
    void unlink(std::int32_t s) noexcept {
        std::int32_t* link = &bucket_[bucket_of(slot_[s].id)];
        while (*link >= 0) {
            if (*link == s) {
                *link = slot_[s].next;
                return;
            }
            link = &slot_[*link].next;
        }
    }
    void link_in(std::int32_t s) noexcept {
        std::int32_t* head = &bucket_[bucket_of(slot_[s].id)];
        slot_[s].next = *head;
        *head = s;
    }
    // Free a slot for reuse: an invalid one first, else the least recently
    // used, whose dirty bytes go to the backing before it is dropped.
    Status victim(std::int32_t& out) noexcept {
        std::int32_t best = -1;
        for (std::size_t i = 0; i < slots_; ++i) {
            const std::int32_t s = static_cast<std::int32_t>(i);
            if (!slot_[s].valid) {
                best = s;
                break;
            }
            if (best < 0 || slot_[s].tick < slot_[best].tick) best = s;
        }
        if (slot_[best].valid) {
            if (slot_[best].dirty) {
                const Status st = bwrite(slot_[best].id, page_at(best));
                if (st != Status::ok) return st;
                slot_[best].dirty = false;
            }
            unlink(best);
            slot_[best].valid = false;
        }
        out = best;
        return Status::ok;
    }
    Status fetch(std::uint64_t id, bool load, std::int32_t& out) noexcept {
        std::int32_t s = find(id);
        if (s >= 0) {
            slot_[s].tick = ++tick_;
            out = s;
            return Status::ok;
        }
        Status st = victim(s);
        if (st != Status::ok) return st;
        slot_[s].id = id;
        slot_[s].valid = false;
        slot_[s].dirty = false;
        if (load) {
            st = bread(id, page_at(s));
            if (st != Status::ok) return structural(st);
        }
        slot_[s].valid = true;
        slot_[s].tick = ++tick_;
        link_in(s);
        out = s;
        return Status::ok;
    }
    Status flush() noexcept {
        for (std::size_t i = 0; i < slots_; ++i) {
            const std::int32_t s = static_cast<std::int32_t>(i);
            if (!slot_[s].valid || !slot_[s].dirty) continue;
            const Status st = bwrite(slot_[s].id, page_at(s));
            if (st != Status::ok) return st;
            slot_[s].dirty = false;
        }
        return Status::ok;
    }
    void invalidate_all() noexcept {
        for (std::size_t i = 0; i < slots_; ++i) {
            slot_[i].valid = false;
            slot_[i].dirty = false;
            slot_[i].next = -1;
        }
        for (std::size_t i = 0; i < nbuckets_; ++i) bucket_[i] = -1;
    }

    Status rd(std::uint64_t id, std::size_t off, std::size_t len, Byte* destination) noexcept {
        std::int32_t s = -1;
        const Status st = fetch(id, true, s);
        if (st != Status::ok) return st;
        std::memcpy(destination, page_at(s) + off, len);
        return Status::ok;
    }

    // Before the first change of an existing page in a transaction, write its
    // image through to a journal page and stamp the page. Pages at or above the
    // high-water mark of the transaction start are new and need no image.
    Status touch(std::uint64_t id) noexcept {
        if (!active_) return Status::programming_error;
        if (id >= hw0_) return Status::ok;
        std::int32_t s = -1;
        Status st = fetch(id, true, s);
        if (st != Status::ok) return st;
        if (ld64(page_at(s) + 8) == txn_) return Status::ok;
        std::memcpy(jbuf_, page_at(s), usable_);
        // A pool page leaves the pool before it is overwritten: if that write is
        // refused or torn the page's link may be damaged, so it must not stay in
        // the chain. A refused journal write therefore forfeits that one page.
        std::uint64_t image;
        if (cur_.pool_head != 0) {
            image = cur_.pool_head;
            st = bread(image, sbuf_);
            if (st != Status::ok) return structural(st);
            cur_.pool_head = ld64(sbuf_ + usable_ + 8);
        } else {
            image = cur_.hw++;
        }
        st64(jbuf_ + usable_, id);
        st64(jbuf_ + usable_ + 8, used_head_);
        st = bwrite(image, jbuf_);
        if (st != Status::ok) return st;
        used_head_ = image;
        if (used_tail_ == 0) used_tail_ = image;
        ++used_count_;
        st = fetch(id, true, s);
        if (st != Status::ok) return st;
        st64(page_at(s) + 8, txn_);
        slot_[s].dirty = true;
        return Status::ok;
    }
    Status wr(std::uint64_t id, std::size_t off, std::size_t len, const Byte* source) noexcept {
        Status st = touch(id);
        if (st != Status::ok) return st;
        std::int32_t s = -1;
        st = fetch(id, true, s);
        if (st != Status::ok) return st;
        std::memcpy(page_at(s) + off, source, len);
        slot_[s].dirty = true;
        return Status::ok;
    }
    Status wr64(std::uint64_t id, std::size_t off, std::uint64_t value) noexcept {
        Byte raw[8];
        st64(raw, value);
        return wr(id, off, 8, raw);
    }
    Status wr32(std::uint64_t id, std::size_t off, std::uint32_t value) noexcept {
        Byte raw[4];
        st32(raw, value);
        return wr(id, off, 4, raw);
    }
    // Replace the whole usable area of a page from a work buffer.
    Status wr_page(std::uint64_t id, Byte* buffer) noexcept {
        st64(buffer + 8, txn_);
        std::memset(buffer + usable_, 0, kTrailer);
        Status st = touch(id);
        if (st != Status::ok) return st;
        std::int32_t s = -1;
        st = fetch(id, false, s);
        if (st != Status::ok) return st;
        std::memcpy(page_at(s), buffer, page_);
        slot_[s].dirty = true;
        return Status::ok;
    }

    // ---- page allocation ------------------------------------------------------

    Status alloc_page(std::uint64_t& id) noexcept {
        if (cur_.free_head != 0) {
            id = cur_.free_head;
            Byte raw[8];
            const Status st = rd(id, 16, 8, raw);
            if (st != Status::ok) return st;
            cur_.free_head = ld64(raw);
            return Status::ok;
        }
        id = cur_.hw++;
        return Status::ok;
    }
    Status free_page(std::uint64_t id) noexcept {
        Status st = wr32(id, 0, kTypeFree);
        if (st != Status::ok) return st;
        st = wr64(id, 16, cur_.free_head);
        if (st != Status::ok) return st;
        cur_.free_head = id;
        return Status::ok;
    }

    // ---- blobs: chains of cells for small bytes, chains of pages for large ----

    std::uint64_t cell_page(std::uint64_t cell) const noexcept { return cell >> 16; }
    std::size_t cell_off(std::uint64_t cell) const noexcept {
        return kHeader + static_cast<std::size_t>(cell & 0xFFFF) * kCell;
    }
    Status cell_alloc(std::uint64_t& cell) noexcept {
        if (cur_.cell_free != 0) {
            cell = cur_.cell_free;
            Byte raw[8];
            const Status st = rd(cell_page(cell), cell_off(cell), 8, raw);
            if (st != Status::ok) return st;
            cur_.cell_free = ld64(raw);
            return Status::ok;
        }
        if (cur_.cell_bump == 0 || cur_.cell_next >= cells_per_page_) {
            std::uint64_t page = 0;
            Status st = alloc_page(page);
            if (st != Status::ok) return st;
            std::memset(w3_, 0, page_);
            st32(w3_, kTypeCells);
            st = wr_page(page, w3_);
            if (st != Status::ok) return st;
            cur_.cell_bump = page;
            cur_.cell_next = 0;
        }
        cell = (cur_.cell_bump << 16) | cur_.cell_next;
        ++cur_.cell_next;
        return Status::ok;
    }

    Status read_source(const ScratchByteSource& source, std::uint64_t offset, Byte* destination,
                       std::size_t length) noexcept {
        ++busy_;
        const Status st = source.read(offset, ScratchBuffer{reinterpret_cast<std::byte*>(destination), length});
        --busy_;
        return st;
    }

    Status blob_create(const ScratchByteSource& source, std::uint64_t length, std::uint64_t& loc) noexcept {
        loc = 0;
        if (length == 0) return Status::ok;
        Status st;
        if (length <= cell_max_) {
            std::uint64_t cell = 0;
            st = cell_alloc(cell);
            if (st != Status::ok) return st;
            loc = cell << 1;
            std::uint64_t done = 0;
            for (;;) {
                const std::size_t n = static_cast<std::size_t>(length - done < kCellPayload ? length - done : kCellPayload);
                std::uint64_t next = 0;
                if (done + n < length) {
                    st = cell_alloc(next);
                    if (st != Status::ok) return st;
                }
                Byte raw[kCell];
                st64(raw, next);
                st = read_source(source, done, raw + 8, n);
                if (st != Status::ok) return st;
                st = wr(cell_page(cell), cell_off(cell), 8 + n, raw);
                if (st != Status::ok) return st;
                done += n;
                if (done == length) break;
                cell = next;
            }
            return Status::ok;
        }
        std::uint64_t page = 0;
        st = alloc_page(page);
        if (st != Status::ok) return st;
        loc = (page << 1) | 1;
        std::uint64_t done = 0;
        for (;;) {
            const std::size_t n = static_cast<std::size_t>(length - done < blob_payload_ ? length - done : blob_payload_);
            std::uint64_t next = 0;
            if (done + n < length) {
                st = alloc_page(next);
                if (st != Status::ok) return st;
            }
            std::memset(w3_, 0, page_);
            st32(w3_, kTypeBlob);
            st64(w3_ + 16, next);
            st = read_source(source, done, w3_ + 24, n);
            if (st != Status::ok) return st;
            st = wr_page(page, w3_);
            if (st != Status::ok) return st;
            done += n;
            if (done == length) break;
            page = next;
        }
        return Status::ok;
    }

    Status blob_read(std::uint64_t loc, std::uint64_t offset, std::size_t length, Byte* destination,
                     BlobPos& pos) noexcept {
        if (length == 0) return Status::ok;
        if (loc == 0) return Status::corrupt;
        const bool paged = (loc & 1) != 0;
        const std::size_t span = paged ? blob_payload_ : kCellPayload;
        const std::uint64_t want = offset / span;
        if (pos.elem == 0 || pos.index > want) {
            pos.index = 0;
            pos.elem = loc >> 1;
        }
        Byte raw[8];
        while (pos.index < want) {
            const Status st = paged ? rd(pos.elem, 16, 8, raw) : rd(cell_page(pos.elem), cell_off(pos.elem), 8, raw);
            if (st != Status::ok) return st;
            pos.elem = ld64(raw);
            ++pos.index;
            if (pos.elem == 0) return Status::corrupt;
        }
        std::size_t inner = static_cast<std::size_t>(offset % span);
        while (length > 0) {
            const std::size_t n = length < span - inner ? length : span - inner;
            const Status st = paged ? rd(pos.elem, 24 + inner, n, destination)
                                    : rd(cell_page(pos.elem), cell_off(pos.elem) + 8 + inner, n, destination);
            if (st != Status::ok) return st;
            destination += n;
            length -= n;
            inner = 0;
            if (length > 0) {
                const Status hop = paged ? rd(pos.elem, 16, 8, raw)
                                         : rd(cell_page(pos.elem), cell_off(pos.elem), 8, raw);
                if (hop != Status::ok) return hop;
                pos.elem = ld64(raw);
                ++pos.index;
                if (pos.elem == 0) return Status::corrupt;
            }
        }
        return Status::ok;
    }

    Status blob_free(std::uint64_t loc) noexcept {
        if (loc == 0) return Status::ok;
        Byte raw[8];
        if ((loc & 1) != 0) {
            std::uint64_t page = loc >> 1;
            while (page != 0) {
                Status st = rd(page, 16, 8, raw);
                if (st != Status::ok) return st;
                const std::uint64_t next = ld64(raw);
                st = free_page(page);
                if (st != Status::ok) return st;
                page = next;
            }
            return Status::ok;
        }
        std::uint64_t cell = loc >> 1;
        while (cell != 0) {
            Status st = rd(cell_page(cell), cell_off(cell), 8, raw);
            if (st != Status::ok) return st;
            const std::uint64_t next = ld64(raw);
            st = wr64(cell_page(cell), cell_off(cell), cur_.cell_free);
            if (st != Status::ok) return st;
            cur_.cell_free = cell;
            cell = next;
        }
        return Status::ok;
    }

    // ---- the handle table: a radix tree of 32-byte entries ------------------

    static std::uint64_t mul_sat(std::uint64_t a, std::uint64_t b) noexcept {
        if (a != 0 && b > UINT64_MAX / a) return UINT64_MAX;
        return a * b;
    }
    // Leaves a node of the given level (1 = a leaf) covers.
    std::uint64_t span_of(std::uint64_t level) const noexcept {
        std::uint64_t span = 1;
        for (std::uint64_t i = 1; i < level; ++i) span = mul_sat(span, hnode_cap_);
        return span;
    }

    // The interior nodes from the root down to the parent of a leaf, and the
    // child slot taken in each. Offset 4 of an interior node counts its
    // nonzero child slots, as offset 4 of a leaf counts its present entries.
    struct HPath {
        std::uint64_t node[kMaxHeight];
        std::size_t slot[kMaxHeight];
        std::size_t depth;
    };

    Status hfind_leaf(std::uint64_t leaf_no, bool create, std::uint64_t& leaf, HPath& path) noexcept {
        leaf = 0;
        path.depth = 0;
        Status st;
        if (cur_.hroot == 0) {
            if (!create) return Status::ok;
            std::uint64_t page = 0;
            st = alloc_page(page);
            if (st != Status::ok) return st;
            std::memset(w3_, 0, page_);
            st32(w3_, kTypeHandleLeaf);
            st = wr_page(page, w3_);
            if (st != Status::ok) return st;
            cur_.hroot = page;
            cur_.hheight = 1;
        }
        while (leaf_no >= span_of(cur_.hheight)) {
            if (!create) return Status::ok;
            std::uint64_t page = 0;
            st = alloc_page(page);
            if (st != Status::ok) return st;
            std::memset(w3_, 0, page_);
            st32(w3_, kTypeHandleNode);
            st32(w3_ + 4, 1);
            st64(w3_ + 16, cur_.hroot);
            st = wr_page(page, w3_);
            if (st != Status::ok) return st;
            cur_.hroot = page;
            ++cur_.hheight;
        }
        std::uint64_t node = cur_.hroot;
        std::uint64_t rest = leaf_no;
        for (std::uint64_t level = cur_.hheight; level > 1; --level) {
            if (path.depth >= kMaxHeight) return Status::corrupt;
            const std::uint64_t span = span_of(level - 1);
            const std::size_t ci = static_cast<std::size_t>(rest / span);
            rest -= static_cast<std::uint64_t>(ci) * span;
            Byte raw[8];
            st = rd(node, 16 + 8 * ci, 8, raw);
            if (st != Status::ok) return st;
            std::uint64_t child = ld64(raw);
            if (child == 0) {
                if (!create) return Status::ok;
                st = alloc_page(child);
                if (st != Status::ok) return st;
                std::memset(w3_, 0, page_);
                st32(w3_, level == 2 ? kTypeHandleLeaf : kTypeHandleNode);
                st = wr_page(child, w3_);
                if (st != Status::ok) return st;
                st = wr64(node, 16 + 8 * ci, child);
                if (st != Status::ok) return st;
                Byte count_raw[4];
                st = rd(node, 4, 4, count_raw);
                if (st != Status::ok) return st;
                st = wr32(node, 4, ld32(count_raw) + 1);
                if (st != Status::ok) return st;
            }
            path.node[path.depth] = node;
            path.slot[path.depth] = ci;
            ++path.depth;
            node = child;
        }
        leaf = node;
        return Status::ok;
    }

    Status hget(std::uint64_t handle, Entry& entry, bool& present) noexcept {
        present = false;
        if (handle == 0 || handle >= cur_.next_handle) return Status::ok;
        const std::uint64_t index = handle - 1;
        std::uint64_t leaf = 0;
        HPath path;
        const Status st = hfind_leaf(index / hleaf_cap_, false, leaf, path);
        if (st != Status::ok || leaf == 0) return st;
        Byte raw[32];
        const Status rs = rd(leaf, kHeader + static_cast<std::size_t>(index % hleaf_cap_) * 32, 32, raw);
        if (rs != Status::ok) return rs;
        entry.key_loc = ld64(raw);
        entry.val_loc = ld64(raw + 8);
        entry.key_len = ld64(raw + 16);
        entry.val_word = ld64(raw + 24);
        present = (entry.val_word & 3) != 0;
        return Status::ok;
    }

    Status hset(std::uint64_t handle, const Entry& entry) noexcept {
        const std::uint64_t index = handle - 1;
        std::uint64_t leaf = 0;
        HPath path;
        Status st = hfind_leaf(index / hleaf_cap_, true, leaf, path);
        if (st != Status::ok) return st;
        const std::size_t off = kHeader + static_cast<std::size_t>(index % hleaf_cap_) * 32;
        Byte raw[32];
        st = rd(leaf, off, 32, raw);
        if (st != Status::ok) return st;
        const bool was = (ld64(raw + 24) & 3) != 0;
        const bool now = (entry.val_word & 3) != 0;
        st64(raw, entry.key_loc);
        st64(raw + 8, entry.val_loc);
        st64(raw + 16, entry.key_len);
        st64(raw + 24, entry.val_word);
        st = wr(leaf, off, 32, raw);
        if (st != Status::ok) return st;
        if (was == now) return Status::ok;
        Byte count_raw[4];
        st = rd(leaf, 4, 4, count_raw);
        if (st != Status::ok) return st;
        std::uint32_t count = ld32(count_raw);
        if (now) ++count;
        else --count;
        st = wr32(leaf, 4, count);
        if (st != Status::ok) return st;
        if (count != 0 || path.depth == 0) return Status::ok;
        // The leaf is empty and is not the root: release it, and each interior
        // node that this leaves without children, up to but not including the
        // root. Handle numbers are never reissued, so nothing would reuse them.
        st = free_page(leaf);
        if (st != Status::ok) return st;
        for (std::size_t k = path.depth; k-- > 0;) {
            st = wr64(path.node[k], 16 + 8 * path.slot[k], 0);
            if (st != Status::ok) return st;
            st = rd(path.node[k], 4, 4, count_raw);
            if (st != Status::ok) return st;
            const std::uint32_t children = ld32(count_raw) - 1;
            st = wr32(path.node[k], 4, children);
            if (st != Status::ok) return st;
            if (children != 0 || k == 0) break;
            st = free_page(path.node[k]);
            if (st != Status::ok) return st;
        }
        return Status::ok;
    }

    // ---- comparisons through the caller's comparator -------------------------

    Status compare_with(const ScratchByteSource& probe, std::uint64_t handle, int& order) noexcept {
        Entry entry;
        bool present = false;
        Status st = hget(handle, entry, present);
        if (st != Status::ok) return st;
        if (!present) return Status::corrupt;
        BlobSource stored(this, entry.key_loc, entry.key_len);
        ++busy_;
        st = order_.compare(order_.context, probe, stored, order);
        --busy_;
        return st;
    }

    // ---- the ordered index: a B+tree of handles -------------------------------

    Status index_count(std::uint64_t page, std::uint32_t& count) noexcept {
        Byte raw[4];
        const Status st = rd(page, 4, 4, raw);
        if (st != Status::ok) return st;
        count = ld32(raw);
        return Status::ok;
    }

    // Find the leaf position of the first key not below the probe, and whether
    // that entry equals the probe. An empty tree answers leaf 0.
    Status descend(const ScratchByteSource& probe, Path& path, bool& equal, std::uint32_t& pos,
                   std::uint64_t& leaf) noexcept {
        path.depth = 0;
        equal = false;
        pos = 0;
        leaf = 0;
        if (cur_.iroot == 0) return Status::ok;
        std::uint64_t page = cur_.iroot;
        for (std::uint64_t level = cur_.iheight; level > 1; --level) {
            if (path.depth >= kMaxDepth) return Status::corrupt;
            std::uint32_t count = 0;
            Status st = index_count(page, count);
            if (st != Status::ok) return st;
            std::uint32_t lo = 0, hi = count;
            while (lo < hi) {
                const std::uint32_t mid = lo + (hi - lo) / 2;
                Byte raw[8];
                st = rd(page, 24 + 16 * static_cast<std::size_t>(mid), 8, raw);
                if (st != Status::ok) return st;
                int order = 0;
                st = compare_with(probe, ld64(raw), order);
                if (st != Status::ok) return st;
                if (order >= 0) lo = mid + 1;
                else hi = mid;
            }
            Byte child_raw[8];
            st = rd(page, 16 + 16 * static_cast<std::size_t>(lo), 8, child_raw);
            if (st != Status::ok) return st;
            path.page[path.depth] = page;
            path.slot[path.depth] = lo;
            ++path.depth;
            page = ld64(child_raw);
            if (page == 0) return Status::corrupt;
        }
        std::uint32_t count = 0;
        Status st = index_count(page, count);
        if (st != Status::ok) return st;
        std::uint32_t lo = 0, hi = count;
        while (lo < hi) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            Byte raw[8];
            st = rd(page, 32 + 8 * static_cast<std::size_t>(mid), 8, raw);
            if (st != Status::ok) return st;
            int order = 0;
            st = compare_with(probe, ld64(raw), order);
            if (st != Status::ok) return st;
            if (order > 0) lo = mid + 1;
            else hi = mid;
        }
        pos = lo;
        leaf = page;
        if (lo < count) {
            Byte raw[8];
            st = rd(page, 32 + 8 * static_cast<std::size_t>(lo), 8, raw);
            if (st != Status::ok) return st;
            int order = 0;
            st = compare_with(probe, ld64(raw), order);
            if (st != Status::ok) return st;
            equal = order == 0;
        }
        return Status::ok;
    }

    Status leaf_entry(std::uint64_t leaf, std::uint32_t pos, std::uint64_t& handle) noexcept {
        Byte raw[8];
        const Status st = rd(leaf, 32 + 8 * static_cast<std::size_t>(pos), 8, raw);
        if (st != Status::ok) return st;
        handle = ld64(raw);
        return Status::ok;
    }

    Status leftmost_leaf(std::uint64_t& leaf) noexcept {
        leaf = 0;
        if (cur_.iroot == 0) return Status::ok;
        std::uint64_t page = cur_.iroot;
        for (std::uint64_t level = cur_.iheight; level > 1; --level) {
            Byte raw[8];
            const Status st = rd(page, 16, 8, raw);
            if (st != Status::ok) return st;
            page = ld64(raw);
            if (page == 0) return Status::corrupt;
        }
        leaf = page;
        return Status::ok;
    }

    Status new_root(std::uint64_t separator, std::uint64_t right) noexcept {
        std::uint64_t page = 0;
        Status st = alloc_page(page);
        if (st != Status::ok) return st;
        std::memset(w3_, 0, page_);
        st32(w3_, kTypeIndexNode);
        st32(w3_ + 4, 1);
        st64(w3_ + 16, cur_.iroot);
        st64(w3_ + 24, separator);
        st64(w3_ + 32, right);
        st = wr_page(page, w3_);
        if (st != Status::ok) return st;
        cur_.iroot = page;
        ++cur_.iheight;
        return Status::ok;
    }

    // Insert a separator and its right sibling above a node that split.
    Status insert_up(const Path& path, std::uint64_t separator, std::uint64_t right) noexcept {
        for (int level = static_cast<int>(path.depth) - 1;; --level) {
            if (level < 0) return new_root(separator, right);
            const std::uint64_t parent = path.page[level];
            const std::uint32_t c = path.slot[level];
            Status st = rd_full(parent, w0_);
            if (st != Status::ok) return st;
            const std::uint32_t count = ld32(w0_ + 4);
            if (count < node_cap_) {
                Byte* pairs = w0_ + 24;
                std::memmove(pairs + 16 * static_cast<std::size_t>(c + 1), pairs + 16 * static_cast<std::size_t>(c),
                             16 * static_cast<std::size_t>(count - c));
                st64(pairs + 16 * static_cast<std::size_t>(c), separator);
                st64(pairs + 16 * static_cast<std::size_t>(c) + 8, right);
                st32(w0_ + 4, count + 1);
                return wr_page(parent, w0_);
            }
            const std::uint32_t total = count + 1;
            const std::uint32_t mid = total / 2;
            // pair(i) of the conceptual node with the new pair inserted at c
            auto pair_sep = [&](std::uint32_t i) -> std::uint64_t {
                return i < c ? ld64(w0_ + 24 + 16 * static_cast<std::size_t>(i))
                             : (i == c ? separator : ld64(w0_ + 24 + 16 * static_cast<std::size_t>(i - 1)));
            };
            auto pair_child = [&](std::uint32_t i) -> std::uint64_t {
                return i < c ? ld64(w0_ + 32 + 16 * static_cast<std::size_t>(i))
                             : (i == c ? right : ld64(w0_ + 32 + 16 * static_cast<std::size_t>(i - 1)));
            };
            std::memset(w1_, 0, page_);
            st32(w1_, kTypeIndexNode);
            st32(w1_ + 4, total - mid - 1);
            st64(w1_ + 16, pair_child(mid));
            for (std::uint32_t j = 0; j + mid + 1 < total; ++j) {
                st64(w1_ + 24 + 16 * static_cast<std::size_t>(j), pair_sep(mid + 1 + j));
                st64(w1_ + 32 + 16 * static_cast<std::size_t>(j), pair_child(mid + 1 + j));
            }
            std::memset(w2_, 0, page_);
            st32(w2_, kTypeIndexNode);
            st32(w2_ + 4, mid);
            st64(w2_ + 16, ld64(w0_ + 16));
            for (std::uint32_t j = 0; j < mid; ++j) {
                st64(w2_ + 24 + 16 * static_cast<std::size_t>(j), pair_sep(j));
                st64(w2_ + 32 + 16 * static_cast<std::size_t>(j), pair_child(j));
            }
            const std::uint64_t promoted = pair_sep(mid);
            std::uint64_t new_page = 0;
            st = alloc_page(new_page);
            if (st != Status::ok) return st;
            st = wr_page(new_page, w1_);
            if (st != Status::ok) return st;
            st = wr_page(parent, w2_);
            if (st != Status::ok) return st;
            separator = promoted;
            right = new_page;
        }
    }

    Status rd_full(std::uint64_t page, Byte* buffer) noexcept { return rd(page, 0, usable_, buffer); }

    Status tree_insert(const Path& path, std::uint64_t leaf, std::uint32_t pos, std::uint64_t handle) noexcept {
        Status st;
        if (leaf == 0) {
            std::uint64_t page = 0;
            st = alloc_page(page);
            if (st != Status::ok) return st;
            std::memset(w0_, 0, page_);
            st32(w0_, kTypeIndexLeaf);
            st32(w0_ + 4, 1);
            st64(w0_ + 32, handle);
            st = wr_page(page, w0_);
            if (st != Status::ok) return st;
            cur_.iroot = page;
            cur_.iheight = 1;
            return Status::ok;
        }
        st = rd_full(leaf, w0_);
        if (st != Status::ok) return st;
        const std::uint32_t count = ld32(w0_ + 4);
        if (count < leaf_cap_) {
            Byte* entries = w0_ + 32;
            std::memmove(entries + 8 * static_cast<std::size_t>(pos + 1), entries + 8 * static_cast<std::size_t>(pos),
                         8 * static_cast<std::size_t>(count - pos));
            st64(entries + 8 * static_cast<std::size_t>(pos), handle);
            st32(w0_ + 4, count + 1);
            return wr_page(leaf, w0_);
        }
        const std::uint32_t total = count + 1;
        const std::uint32_t mid = (total + 1) / 2;
        auto at = [&](std::uint32_t i) -> std::uint64_t {
            return i < pos ? ld64(w0_ + 32 + 8 * static_cast<std::size_t>(i))
                           : (i == pos ? handle : ld64(w0_ + 32 + 8 * static_cast<std::size_t>(i - 1)));
        };
        std::uint64_t right_page = 0;
        st = alloc_page(right_page);
        if (st != Status::ok) return st;
        const std::uint64_t old_next = ld64(w0_ + 24);
        const std::uint64_t old_prev = ld64(w0_ + 16);
        std::memset(w1_, 0, page_);
        st32(w1_, kTypeIndexLeaf);
        st32(w1_ + 4, total - mid);
        st64(w1_ + 16, leaf);
        st64(w1_ + 24, old_next);
        for (std::uint32_t i = mid; i < total; ++i) st64(w1_ + 32 + 8 * static_cast<std::size_t>(i - mid), at(i));
        std::memset(w2_, 0, page_);
        st32(w2_, kTypeIndexLeaf);
        st32(w2_ + 4, mid);
        st64(w2_ + 16, old_prev);
        st64(w2_ + 24, right_page);
        for (std::uint32_t i = 0; i < mid; ++i) st64(w2_ + 32 + 8 * static_cast<std::size_t>(i), at(i));
        const std::uint64_t separator = at(mid);
        st = wr_page(right_page, w1_);
        if (st != Status::ok) return st;
        st = wr_page(leaf, w2_);
        if (st != Status::ok) return st;
        if (old_next != 0) {
            st = wr64(old_next, 16, right_page);
            if (st != Status::ok) return st;
        }
        return insert_up(path, separator, right_page);
    }

    // A ghost separator keeps its key bytes only while some index node names
    // it; release it when that name goes away.
    Status release_separator(std::uint64_t handle) noexcept {
        Entry entry;
        bool present = false;
        Status st = hget(handle, entry, present);
        if (st != Status::ok) return st;
        if (!present || (entry.val_word & 3) != kGhost) return Status::ok;
        st = blob_free(entry.key_loc);
        if (st != Status::ok) return st;
        return hset(handle, Entry{});
    }

    Status remove_child(const Path& path) noexcept {
        int level = static_cast<int>(path.depth) - 1;
        Status st;
        for (;; --level) {
            if (level < 0) {
                cur_.iroot = 0;
                cur_.iheight = 0;
                return Status::ok;
            }
            const std::uint64_t parent = path.page[level];
            const std::uint32_t c = path.slot[level];
            st = rd_full(parent, w0_);
            if (st != Status::ok) return st;
            const std::uint32_t count = ld32(w0_ + 4);
            if (count == 0) {
                st = free_page(parent);
                if (st != Status::ok) return st;
                continue;
            }
            Byte* pairs = w0_ + 24;
            std::uint64_t separator;
            if (c >= 1) {
                separator = ld64(pairs + 16 * static_cast<std::size_t>(c - 1));
                std::memmove(pairs + 16 * static_cast<std::size_t>(c - 1), pairs + 16 * static_cast<std::size_t>(c),
                             16 * static_cast<std::size_t>(count - c));
            } else {
                separator = ld64(pairs);
                st64(w0_ + 16, ld64(pairs + 8));
                std::memmove(pairs, pairs + 16, 16 * static_cast<std::size_t>(count - 1));
            }
            st32(w0_ + 4, count - 1);
            st = wr_page(parent, w0_);
            if (st != Status::ok) return st;
            st = release_separator(separator);
            if (st != Status::ok) return st;
            break;
        }
        while (cur_.iheight > 1) {
            std::uint32_t count = 0;
            st = index_count(cur_.iroot, count);
            if (st != Status::ok) return st;
            if (count != 0) break;
            Byte raw[8];
            st = rd(cur_.iroot, 16, 8, raw);
            if (st != Status::ok) return st;
            const std::uint64_t old_root = cur_.iroot;
            cur_.iroot = ld64(raw);
            --cur_.iheight;
            st = free_page(old_root);
            if (st != Status::ok) return st;
        }
        return Status::ok;
    }

    Status tree_erase(const Path& path, std::uint64_t leaf, std::uint32_t pos, std::uint64_t handle) noexcept {
        Status st = rd_full(leaf, w0_);
        if (st != Status::ok) return st;
        const std::uint32_t count = ld32(w0_ + 4);
        if (pos >= count || ld64(w0_ + 32 + 8 * static_cast<std::size_t>(pos)) != handle) return Status::corrupt;
        Byte* entries = w0_ + 32;
        std::memmove(entries + 8 * static_cast<std::size_t>(pos), entries + 8 * static_cast<std::size_t>(pos + 1),
                     8 * static_cast<std::size_t>(count - pos - 1));
        st32(w0_ + 4, count - 1);
        if (count > 1) return wr_page(leaf, w0_);
        const std::uint64_t prev = ld64(w0_ + 16);
        const std::uint64_t next = ld64(w0_ + 24);
        if (prev != 0) {
            st = wr64(prev, 24, next);
            if (st != Status::ok) return st;
        }
        if (next != 0) {
            st = wr64(next, 16, prev);
            if (st != Status::ok) return st;
        }
        st = free_page(leaf);
        if (st != Status::ok) return st;
        return remove_child(path);
    }

    // ---- the superblock -----------------------------------------------------

    void encode_super(const Super& s, Byte* buffer) const noexcept {
        std::memset(buffer, 0, page_);
        const std::uint64_t fields[kSuperFields] = {kMagic, kVersion, page_, order_.id, s.seq, s.next_handle,
                                                    s.record_count, s.hw, s.free_head, s.hroot, s.hheight, s.iroot,
                                                    s.iheight, s.cell_free, s.cell_bump, s.cell_next, s.pool_head, 0};
        Fold fold;
        for (std::size_t i = 0; i < kSuperFields; ++i) {
            st64(buffer + 8 * i, fields[i]);
            fold.le64(fields[i]);
        }
        st64(buffer + 8 * kSuperFields, fold.value);
    }
    bool decode_super(const Byte* buffer, Super& s, std::uint64_t& order_id) const noexcept {
        std::uint64_t fields[kSuperFields];
        Fold fold;
        for (std::size_t i = 0; i < kSuperFields; ++i) {
            fields[i] = ld64(buffer + 8 * i);
            fold.le64(fields[i]);
        }
        if (fields[0] != kMagic || fields[1] != kVersion || fields[2] != page_) return false;
        if (ld64(buffer + 8 * kSuperFields) != fold.value) return false;
        order_id = fields[3];
        s.seq = fields[4];
        s.next_handle = fields[5];
        s.record_count = fields[6];
        s.hw = fields[7];
        s.free_head = fields[8];
        s.hroot = fields[9];
        s.hheight = fields[10];
        s.iroot = fields[11];
        s.iheight = fields[12];
        s.cell_free = fields[13];
        s.cell_bump = fields[14];
        s.cell_next = fields[15];
        s.pool_head = fields[16];
        return true;
    }

    Status reopen_load() noexcept {
        if (backing_->page_count() == 0) return Status::not_found;
        Super best;
        std::uint64_t best_order = 0;
        bool have = false;
        for (std::uint64_t slot = 0; slot < 2; ++slot) {
            const Status st = bread(slot, sbuf_);
            if (st == Status::not_found) continue;
            if (st != Status::ok) return st;
            Super candidate;
            std::uint64_t order_id = 0;
            if (!decode_super(sbuf_, candidate, order_id)) continue;
            if (!have || candidate.seq > best.seq) {
                best = candidate;
                best_order = order_id;
                have = true;
            }
        }
        if (!have) return Status::corrupt;
        if (best_order != order_.id) return Status::programming_error;
        if (best.hw < 2) return Status::corrupt;
        cur_ = best;
        return Status::ok;
    }

    // ---- transactions -----------------------------------------------------------

    Status begin() noexcept {
        if (open_status_ != Status::ok || active_ || busy_ != 0) return Status::programming_error;
        carried_failure_.reset();
        snap_ = cur_;
        txn_ = cur_.seq + 1;
        hw0_ = cur_.hw;
        active_ = true;
        poisoned_ = false;
        used_head_ = 0;
        used_tail_ = 0;
        used_count_ = 0;
        return Status::ok;
    }

    Status commit_impl(ScratchDurability durability) noexcept {
        std::uint64_t new_pool = cur_.pool_head;
        Status st;
        if (used_count_ != 0) {
            st = bread(used_tail_, sbuf_);
            if (st != Status::ok) return structural(st);
            st64(sbuf_ + usable_ + 8, cur_.pool_head);
            st = bwrite(used_tail_, sbuf_);
            if (st != Status::ok) return st;
            new_pool = used_head_;
        }
        st = flush();
        if (st != Status::ok) return st;
        if (durability != ScratchDurability::buffered) {
            st = bsync(durability);
            if (st != Status::ok) return st;
        }
        Super next = cur_;
        next.seq = cur_.seq + 1;
        next.pool_head = new_pool;
        encode_super(next, sbuf_);
        // A refused write may still have landed this whole page (it is far
        // smaller than a page), and a refused final sync leaves it published,
        // so the slot stays marked until rollback blanks it.
        tainted_slot_ = next.seq & 1;
        st = bwrite(tainted_slot_, sbuf_);
        if (st != Status::ok) return st;
        if (durability != ScratchDurability::buffered) {
            st = bsync(durability);
            if (st != Status::ok) return st;
        }
        tainted_slot_ = kNoSlot;
        cur_ = next;
        active_ = false;
        used_head_ = 0;
        used_tail_ = 0;
        used_count_ = 0;
        return Status::ok;
    }

    Status commit(ScratchDurability durability) noexcept {
        if (open_status_ != Status::ok || !active_ || poisoned_ || busy_ != 0) return Status::programming_error;
        if (durability != ScratchDurability::buffered && durability != ScratchDurability::data
            && durability != ScratchDurability::full)
            return Status::programming_error;
        const Status st = commit_impl(durability);
        if (st != Status::ok) poisoned_ = true;
        return st;
    }

    // Every journal page is spent the moment its target is restored, so a
    // rollback that failed and is tried again resumes at the first page not yet
    // restored. A journal page that came from the pool goes back to it; one that
    // extended the backing is dropped with the high-water mark.
    Status rollback_impl() noexcept {
        invalidate_all();
        while (used_count_ != 0) {
            const std::uint64_t image = used_head_;
            Status st = bread(image, jbuf_);
            if (st != Status::ok) return structural(st);
            const std::uint64_t target = ld64(jbuf_ + usable_);
            const std::uint64_t next = ld64(jbuf_ + usable_ + 8);
            if (target < 2 || target >= hw0_) return Status::corrupt;
            std::memcpy(w3_, jbuf_, usable_);
            std::memset(w3_ + usable_, 0, kTrailer);
            st = bwrite(target, w3_);
            if (st != Status::ok) return st;
            used_head_ = next;
            --used_count_;
            if (image < hw0_) {
                st64(jbuf_ + usable_, 0);
                st64(jbuf_ + usable_ + 8, cur_.pool_head);
                st = bwrite(image, jbuf_);
                if (st != Status::ok) return st;
                cur_.pool_head = image;
            }
        }
        if (tainted_slot_ != kNoSlot) {
            std::memset(w3_, 0, page_);
            const Status st = bwrite(tainted_slot_, w3_);
            if (st != Status::ok) return st;
            tainted_slot_ = kNoSlot;
        }
        const std::uint64_t pool = cur_.pool_head;
        cur_ = snap_;
        cur_.pool_head = pool;
        active_ = false;
        poisoned_ = false;
        used_head_ = 0;
        used_tail_ = 0;
        return Status::ok;
    }

    // A rollback that fails leaves the transaction active and rollback-only.
    Status rollback() noexcept {
        if (open_status_ != Status::ok || !active_ || busy_ != 0) return Status::programming_error;
        const Status st = rollback_impl();
        if (st != Status::ok) poisoned_ = true;
        return st;
    }

    // ---- mutations ----------------------------------------------------------------

    Status put_impl(const ScratchByteSource& key, const ScratchByteSource& value, std::uint64_t& result) noexcept {
        const std::uint64_t key_len = key.size();
        const std::uint64_t val_len = value.size();
        if (val_len >= (std::uint64_t{1} << 61) || key_len >= (std::uint64_t{1} << 61)) return Status::programming_error;
        Path path;
        bool equal = false;
        std::uint32_t pos = 0;
        std::uint64_t leaf = 0;
        Status st = descend(key, path, equal, pos, leaf);
        if (st != Status::ok) return st;
        Entry entry;
        entry.key_len = key_len;
        entry.val_word = (val_len << 2) | kLive;
        if (equal) {
            std::uint64_t handle = 0;
            st = leaf_entry(leaf, pos, handle);
            if (st != Status::ok) return st;
            Entry old;
            bool present = false;
            st = hget(handle, old, present);
            if (st != Status::ok) return st;
            if (!present) return Status::corrupt;
            st = blob_create(key, key_len, entry.key_loc);
            if (st != Status::ok) return st;
            st = blob_create(value, val_len, entry.val_loc);
            if (st != Status::ok) return st;
            st = hset(handle, entry);
            if (st != Status::ok) return st;
            st = blob_free(old.key_loc);
            if (st != Status::ok) return st;
            st = blob_free(old.val_loc);
            if (st != Status::ok) return st;
            result = handle;
            return Status::ok;
        }
        if (cur_.next_handle >= (std::uint64_t{1} << 62)) return Status::programming_error;
        st = blob_create(key, key_len, entry.key_loc);
        if (st != Status::ok) return st;
        st = blob_create(value, val_len, entry.val_loc);
        if (st != Status::ok) return st;
        const std::uint64_t handle = cur_.next_handle++;
        st = hset(handle, entry);
        if (st != Status::ok) return st;
        st = tree_insert(path, leaf, pos, handle);
        if (st != Status::ok) return st;
        ++cur_.record_count;
        result = handle;
        return Status::ok;
    }

    Status put(const ScratchByteSource& key, const ScratchByteSource& value, ScratchHandle& result) noexcept {
        if (open_status_ != Status::ok || !active_ || poisoned_ || busy_ != 0) return Status::programming_error;
        backing_io_failed_ = false;
        std::uint64_t handle = 0;
        const Status st = put_impl(key, value, handle);
        if (st != Status::ok) {
            poisoned_ = true;
            return backing_io_failed_ ? Status::io_error : Status::programming_error;
        }
        result = handle;
        return Status::ok;
    }

    Status erase_impl(std::uint64_t handle, const Entry& entry) noexcept {
        BlobSource key(this, entry.key_loc, entry.key_len);
        Path path;
        bool equal = false;
        std::uint32_t pos = 0;
        std::uint64_t leaf = 0;
        Status st = descend(key, path, equal, pos, leaf);
        if (st != Status::ok) return st;
        if (!equal || leaf == 0) return Status::corrupt;
        std::uint64_t found = 0;
        st = leaf_entry(leaf, pos, found);
        if (st != Status::ok) return st;
        if (found != handle) return Status::corrupt;
        bool named = false;
        for (unsigned level = 0; level < path.depth; ++level) {
            const std::uint32_t c = path.slot[level];
            if (c == 0) continue;
            Byte raw[8];
            st = rd(path.page[level], 24 + 16 * static_cast<std::size_t>(c - 1), 8, raw);
            if (st != Status::ok) return st;
            if (ld64(raw) == handle) named = true;
        }
        if (named) {
            st = blob_free(entry.val_loc);
            if (st != Status::ok) return st;
            Entry ghost;
            ghost.key_loc = entry.key_loc;
            ghost.key_len = entry.key_len;
            ghost.val_word = kGhost;
            st = hset(handle, ghost);
        } else {
            st = blob_free(entry.key_loc);
            if (st != Status::ok) return st;
            st = blob_free(entry.val_loc);
            if (st != Status::ok) return st;
            st = hset(handle, Entry{});
        }
        if (st != Status::ok) return st;
        st = tree_erase(path, leaf, pos, handle);
        if (st != Status::ok) return st;
        --cur_.record_count;
        return Status::ok;
    }

    Status erase(ScratchHandle handle) noexcept {
        if (open_status_ != Status::ok || !active_ || poisoned_ || busy_ != 0) return Status::programming_error;
        backing_io_failed_ = false;
        Entry entry;
        bool present = false;
        Status st = hget(handle, entry, present);
        if (st != Status::ok) {
            poisoned_ = true;
            return backing_io_failed_ ? Status::io_error : Status::programming_error;
        }
        if (!present || (entry.val_word & 3) != kLive) return Status::not_found;
        st = erase_impl(handle, entry);
        if (st != Status::ok) {
            poisoned_ = true;
            return backing_io_failed_ ? Status::io_error : Status::programming_error;
        }
        return Status::ok;
    }

    // ---- reads ------------------------------------------------------------------------

    bool readable() const noexcept { return open_status_ == Status::ok && !poisoned_; }
    bool page_ok(const ScratchBuffer& page) const noexcept { return page.data != nullptr && page.size == page_; }

    Status live_entry(std::uint64_t handle, Entry& entry) noexcept {
        bool present = false;
        const Status st = hget(handle, entry, present);
        if (st != Status::ok) return st;
        if (!present || (entry.val_word & 3) != kLive) return Status::not_found;
        return Status::ok;
    }

    Status point(ScratchHandle handle, ScratchRecordInfo& info) noexcept {
        if (!readable()) return Status::programming_error;
        Entry entry;
        const Status st = live_entry(handle, entry);
        if (st != Status::ok) return st;
        info.handle = handle;
        info.key_bytes = entry.key_len;
        info.value_bytes = entry.val_word >> 2;
        return Status::ok;
    }

    Status read(ScratchHandle handle, ScratchField field, std::uint64_t offset, ScratchBuffer destination) noexcept {
        if (!readable()) return Status::programming_error;
        Entry entry;
        Status st = live_entry(handle, entry);
        if (st != Status::ok) return st;
        const bool is_key = field == ScratchField::key;
        if (!is_key && field != ScratchField::value) return Status::programming_error;
        const std::uint64_t length = is_key ? entry.key_len : entry.val_word >> 2;
        if (offset > length || destination.size > length - offset
            || (destination.size != 0 && destination.data == nullptr))
            return Status::programming_error;
        BlobSource source(this, is_key ? entry.key_loc : entry.val_loc, length);
        return source.read(offset, destination);
    }

    Status visit(ScratchHandle handle, ScratchBuffer page, ScratchChunkVisitor visitor, void* context) noexcept {
        if (!readable() || !page_ok(page) || visitor == nullptr) return Status::programming_error;
        Entry entry;
        const Status st = live_entry(handle, entry);
        if (st != Status::ok) return st;
        ScratchRecordInfo info;
        info.handle = handle;
        info.key_bytes = entry.key_len;
        info.value_bytes = entry.val_word >> 2;
        for (int f = 0; f < 2; ++f) {
            const ScratchField field = f == 0 ? ScratchField::key : ScratchField::value;
            const std::uint64_t length = f == 0 ? entry.key_len : entry.val_word >> 2;
            const std::uint64_t loc = f == 0 ? entry.key_loc : entry.val_loc;
            if (length == 0) {
                ScratchChunkView view{info, field, 0, ScratchBytes{page.data, 0}, true};
                ++busy_;
                const bool go = visitor(context, view);
                --busy_;
                if (!go) return Status::cancelled;
                continue;
            }
            BlobPos pos;
            std::uint64_t done = 0;
            while (done < length) {
                const std::size_t n = static_cast<std::size_t>(length - done < page_ ? length - done : page_);
                const Status rs = blob_read(loc, done, n, reinterpret_cast<Byte*>(page.data), pos);
                if (rs != Status::ok) return rs;
                ScratchChunkView view{info, field, done, ScratchBytes{page.data, n}, done + n == length};
                ++busy_;
                const bool go = visitor(context, view);
                --busy_;
                if (!go) return Status::cancelled;
                done += n;
            }
        }
        return Status::ok;
    }

    Status for_each(ScratchBuffer page, ScratchRecordVisitor visitor, void* context) noexcept {
        if (!readable() || !page_ok(page) || visitor == nullptr) return Status::programming_error;
        std::uint64_t leaf = 0;
        Status st = leftmost_leaf(leaf);
        if (st != Status::ok) return st;
        while (leaf != 0) {
            std::uint32_t count = 0;
            st = index_count(leaf, count);
            if (st != Status::ok) return st;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint64_t handle = 0;
                st = leaf_entry(leaf, i, handle);
                if (st != Status::ok) return st;
                Entry entry;
                st = live_entry(handle, entry);
                if (st != Status::ok) return structural(st == Status::not_found ? Status::corrupt : st);
                BlobSource key(this, entry.key_loc, entry.key_len);
                BlobSource value(this, entry.val_loc, entry.val_word >> 2);
                ScratchRecordInfo info;
                info.handle = handle;
                info.key_bytes = entry.key_len;
                info.value_bytes = entry.val_word >> 2;
                ScratchRecordView view{info, key, value};
                ++busy_;
                const bool go = visitor(context, view);
                --busy_;
                if (!go) return Status::cancelled;
            }
            Byte raw[8];
            st = rd(leaf, 24, 8, raw);
            if (st != Status::ok) return st;
            leaf = ld64(raw);
        }
        return Status::ok;
    }

    Status first(ScratchHandle& result) noexcept {
        if (!readable()) return Status::programming_error;
        std::uint64_t leaf = 0;
        Status st = leftmost_leaf(leaf);
        if (st != Status::ok) return st;
        if (leaf == 0) {
            result = scratch_no_handle;
            return Status::end;
        }
        std::uint64_t handle = 0;
        st = leaf_entry(leaf, 0, handle);
        if (st != Status::ok) return st;
        result = handle;
        return Status::ok;
    }

    Status next(ScratchHandle current, ScratchHandle& result) noexcept {
        if (!readable()) return Status::programming_error;
        Entry entry;
        Status st = live_entry(current, entry);
        if (st != Status::ok) return st;
        BlobSource key(this, entry.key_loc, entry.key_len);
        Path path;
        bool equal = false;
        std::uint32_t pos = 0;
        std::uint64_t leaf = 0;
        st = descend(key, path, equal, pos, leaf);
        if (st != Status::ok) return st;
        std::uint64_t found = 0;
        if (equal && leaf != 0) {
            st = leaf_entry(leaf, pos, found);
            if (st != Status::ok) return st;
        }
        if (found != current) return Status::corrupt;
        std::uint32_t count = 0;
        st = index_count(leaf, count);
        if (st != Status::ok) return st;
        if (pos + 1 < count) {
            std::uint64_t handle = 0;
            st = leaf_entry(leaf, pos + 1, handle);
            if (st != Status::ok) return st;
            result = handle;
            return Status::ok;
        }
        Byte raw[8];
        st = rd(leaf, 24, 8, raw);
        if (st != Status::ok) return st;
        const std::uint64_t next_leaf = ld64(raw);
        if (next_leaf == 0) {
            result = scratch_no_handle;
            return Status::end;
        }
        std::uint64_t handle = 0;
        st = leaf_entry(next_leaf, 0, handle);
        if (st != Status::ok) return st;
        result = handle;
        return Status::ok;
    }

    Status lower_bound(const ScratchByteSource& probe, ScratchHandle& result) noexcept {
        if (!readable()) return Status::programming_error;
        Path path;
        bool equal = false;
        std::uint32_t pos = 0;
        std::uint64_t leaf = 0;
        Status st = descend(probe, path, equal, pos, leaf);
        if (st != Status::ok) return st;
        if (leaf == 0) {
            result = scratch_no_handle;
            return Status::end;
        }
        std::uint32_t count = 0;
        st = index_count(leaf, count);
        if (st != Status::ok) return st;
        std::uint64_t handle = 0;
        if (pos < count) {
            st = leaf_entry(leaf, pos, handle);
            if (st != Status::ok) return st;
            result = handle;
            return Status::ok;
        }
        Byte raw[8];
        st = rd(leaf, 24, 8, raw);
        if (st != Status::ok) return st;
        const std::uint64_t next_leaf = ld64(raw);
        if (next_leaf == 0) {
            result = scratch_no_handle;
            return Status::end;
        }
        st = leaf_entry(next_leaf, 0, handle);
        if (st != Status::ok) return st;
        result = handle;
        return Status::ok;
    }

    Status fold_source(Fold& fold, std::uint64_t loc, std::uint64_t length, ScratchBuffer page) noexcept {
        BlobPos pos;
        std::uint64_t done = 0;
        while (done < length) {
            const std::size_t n = static_cast<std::size_t>(length - done < page_ ? length - done : page_);
            const Status st = blob_read(loc, done, n, reinterpret_cast<Byte*>(page.data), pos);
            if (st != Status::ok) return st;
            fold.bytes(reinterpret_cast<const Byte*>(page.data), n);
            done += n;
        }
        return Status::ok;
    }

    Status digest(bool with_state, ScratchBuffer page, std::uint64_t& result) noexcept {
        if (!readable() || !page_ok(page)) return Status::programming_error;
        if (with_state && active_) return Status::programming_error;
        Fold fold;
        if (with_state) {
            fold.le64(order_.id);
            fold.le64(cur_.next_handle);
        }
        fold.le64(cur_.record_count);
        std::uint64_t leaf = 0;
        Status st = leftmost_leaf(leaf);
        if (st != Status::ok) return st;
        while (leaf != 0) {
            std::uint32_t count = 0;
            st = index_count(leaf, count);
            if (st != Status::ok) return st;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint64_t handle = 0;
                st = leaf_entry(leaf, i, handle);
                if (st != Status::ok) return st;
                Entry entry;
                st = live_entry(handle, entry);
                if (st != Status::ok) return structural(st == Status::not_found ? Status::corrupt : st);
                if (with_state) fold.le64(handle);
                fold.le64(entry.key_len);
                st = fold_source(fold, entry.key_loc, entry.key_len, page);
                if (st != Status::ok) return st;
                fold.le64(entry.val_word >> 2);
                st = fold_source(fold, entry.val_loc, entry.val_word >> 2, page);
                if (st != Status::ok) return st;
            }
            Byte raw[8];
            st = rd(leaf, 24, 8, raw);
            if (st != Status::ok) return st;
            leaf = ld64(raw);
        }
        result = fold.value;
        return Status::ok;
    }

    ScratchStoreStats stats() const noexcept {
        ScratchStoreStats out;
        out.resident_bytes = total_;
        if (open_status_ != Status::ok && backing_ == nullptr) return out;
        out.backing_pages = backing_ != nullptr ? backing_->page_count() : 0;
        out.backing_bytes = out.backing_pages * page_;
        out.page_reads = reads_;
        out.page_writes = writes_;
        out.syncs = syncs_;
        return out;
    }

    Status advance_handles(std::uint64_t count) noexcept {
        if (open_status_ != Status::ok || poisoned_ || busy_ != 0) return Status::programming_error;
        if (count == 0) return Status::ok;
        const bool own = !active_;
        if (own) {
            const Status st = begin();
            if (st != Status::ok) return st;
        }
        const std::uint64_t limit = std::uint64_t{1} << 62; // Same domain as put_impl.
        const std::uint64_t remaining = cur_.next_handle < limit ? limit - cur_.next_handle : 0;
        cur_.next_handle += count < remaining ? count : remaining;
        if (count > remaining) {
            poisoned_ = true;
            return Status::programming_error;
        }
        return own ? commit(ScratchDurability::buffered) : Status::ok;
    }

    // ---- state ----------------------------------------------------------------------------

    Status open_status_ = Status::ok;
    ScratchBacking* backing_ = nullptr;
    std::optional<ReportScratchIoError> carried_failure_;
    ScratchKeyOrder order_{};
    std::size_t total_ = 0;
    std::size_t page_ = 0;
    std::size_t usable_ = 0;
    std::size_t slots_ = 0;
    std::size_t nbuckets_ = 0;
    std::size_t leaf_cap_ = 0;
    std::size_t node_cap_ = 0;
    std::size_t hleaf_cap_ = 0;
    std::size_t hnode_cap_ = 0;
    std::size_t cells_per_page_ = 0;
    std::size_t blob_payload_ = 0;
    std::size_t cell_max_ = 0;
    Slot* slot_ = nullptr;
    std::int32_t* bucket_ = nullptr;
    Byte* cache_ = nullptr;
    Byte* w0_ = nullptr;
    Byte* w1_ = nullptr;
    Byte* w2_ = nullptr;
    Byte* w3_ = nullptr;
    Byte* jbuf_ = nullptr;
    Byte* sbuf_ = nullptr;
    std::uint64_t tick_ = 0;
    std::uint64_t reads_ = 0;
    std::uint64_t writes_ = 0;
    std::uint64_t syncs_ = 0;
    Super cur_{};
    Super snap_{};
    bool active_ = false;
    bool poisoned_ = false;
    std::uint64_t txn_ = 0;
    std::uint64_t hw0_ = 0;
    std::uint64_t used_head_ = 0;
    std::uint64_t used_tail_ = 0;
    std::uint64_t used_count_ = 0;
    std::uint64_t tainted_slot_ = kNoSlot;
    std::uint32_t busy_ = 0;

private:
    bool backing_io_failed_ = false;
};

} // namespace scratch_impl

struct CommandScratchStore::State : scratch_impl::Core {
    State(const ScratchStoreConfig& config, ScratchBacking* backing, std::size_t total) noexcept
        : scratch_impl::Core(config, backing, total) {}
    static State* allocation_failed() noexcept {
        // Invalid geometry constructs no workspace and performs no I/O. Every
        // Core operation refuses it before mutation. Only pointer identity is
        // used as the setup carrier; no embedded object gains a data member.
        static State failed(ScratchStoreConfig{}, nullptr, 0);
        return &failed;
    }
};

inline CommandScratchStore::CommandScratchStore(ScratchStoreConfig config, ScratchBacking* backing) noexcept {
    static_assert(sizeof(State) == sizeof(scratch_impl::Core), "the workspace layout assumes no extra members");
    // Prepare the nonallocating setup marker outside the block that can fail.
    auto* const allocation_failed = State::allocation_failed();
    const std::size_t total = scratch_impl::Core::required_bytes(config, backing);
    void* block = ::operator new(total, std::nothrow);
    if (block == nullptr) {
        if (scratch_impl::Core::geometry_ok(config, backing)) state_ = allocation_failed;
        return;
    }
    state_ = new (block) State(config, backing, total);
}

inline CommandScratchStore::~CommandScratchStore() noexcept {
    if (state_ == nullptr || state_ == State::allocation_failed()) return;
    state_->shutdown();
    state_->~State();
    ::operator delete(static_cast<void*>(state_));
}

inline ScratchStatus CommandScratchStore::status() const noexcept {
    if (state_ == State::allocation_failed()) return ScratchStatus::io_error;
    return state_ == nullptr ? ScratchStatus::programming_error : state_->open_status_;
}
inline const ReportScratchIoError* CommandScratchStore::carried_failure() const noexcept {
    if (state_ == State::allocation_failed()) return &scratch_out_of_memory_failure();
    return state_ && state_->carried_failure_ ? &*state_->carried_failure_ : nullptr;
}
inline std::size_t CommandScratchStore::page_size() const noexcept { return state_ == nullptr ? 0 : state_->page_; }
inline std::uint64_t CommandScratchStore::record_count() const noexcept {
    return state_ == nullptr || state_->open_status_ != ScratchStatus::ok ? 0 : state_->cur_.record_count;
}
inline bool CommandScratchStore::transaction_active() const noexcept {
    return state_ != nullptr && state_->open_status_ == ScratchStatus::ok && state_->active_;
}
inline ScratchStatus CommandScratchStore::begin() noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->begin();
}
inline ScratchStatus CommandScratchStore::put(const ScratchByteSource& key, const ScratchByteSource& value,
                                       ScratchHandle& result) noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->put(key, value, result);
}
inline ScratchStatus CommandScratchStore::erase(ScratchHandle handle) noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->erase(handle);
}
inline ScratchStatus CommandScratchStore::commit(ScratchDurability durability) noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->commit(durability);
}
inline ScratchStatus CommandScratchStore::rollback() noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->rollback();
}
inline ScratchStatus CommandScratchStore::point(ScratchHandle handle, ScratchRecordInfo& info) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->point(handle, info);
}
inline ScratchStatus CommandScratchStore::read(ScratchHandle handle, ScratchField field, std::uint64_t offset,
                                        ScratchBuffer destination) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->read(handle, field, offset, destination);
}
inline ScratchStatus CommandScratchStore::visit(ScratchHandle handle, ScratchBuffer page, ScratchChunkVisitor visitor,
                                         void* context) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->visit(handle, page, visitor, context);
}
inline ScratchStatus CommandScratchStore::for_each(ScratchBuffer page, ScratchRecordVisitor visitor,
                                            void* context) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->for_each(page, visitor, context);
}
inline ScratchStatus CommandScratchStore::first(ScratchHandle& result) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->first(result);
}
inline ScratchStatus CommandScratchStore::next(ScratchHandle current, ScratchHandle& result) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->next(current, result);
}
inline ScratchStatus CommandScratchStore::lower_bound(const ScratchByteSource& probe, ScratchHandle& result) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->lower_bound(probe, result);
}
inline ScratchStatus CommandScratchStore::ordered_digest(ScratchBuffer page, std::uint64_t& result) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->digest(false, page, result);
}
inline ScratchStatus CommandScratchStore::state_digest(ScratchBuffer page, std::uint64_t& result) const noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->digest(true, page, result);
}
inline ScratchStoreStats CommandScratchStore::stats() const noexcept {
    return state_ == nullptr ? ScratchStoreStats{} : state_->stats();
}

inline ScratchStatus CommandScratchStore::advance_handles(std::uint64_t count) noexcept {
    return state_ == nullptr ? ScratchStatus::programming_error : state_->advance_handles(count);
}

} // namespace pineforge::detail
