#pragma once

// CR150-CX L4 / CR-221 scope B. Declarations for the substrate implementation
// lane; no engine/source policy, D3.7 sink fields or report schema changes.
#include <pineforge/detail/scratch_readback.hpp>

namespace pineforge::detail {

// Compare entire keys by bounded reads; keys need not fit a page. On ok,
// order is negative/zero/positive. The comparator must be a deterministic
// strict weak ordering; equality identifies ONE logical record (map/upsert
// semantics). It must not throw, allocate with population, mutate the store,
// retain sources, depend on addresses or consult mutable external state.
struct ScratchKeyOrder {
    std::uint64_t id = 0; // nonzero caller-owned schema/ordering version
    void* context = nullptr;
    ScratchStatus (*compare)(void*, const ScratchByteSource&, const ScratchByteSource&,
                             int& order) noexcept = nullptr;
};

enum class ScratchOpenMode : std::uint32_t { create_empty, reopen };

struct ScratchStoreConfig {
    std::size_t page_count = 0;    // RESIDENT cache slots, not a record limit
    std::size_t page_size = 0;     // must match backing.page_size()
    std::size_t journal_slots = 0; // fixed RESIDENT transaction staging slots
    std::size_t index_slots = 0;   // fixed RESIDENT index staging slots
    ScratchKeyOrder order{};
    ScratchOpenMode mode = ScratchOpenMode::reopen;
};

// One exclusive run owner, one transaction at a time. Logical key/value blobs,
// handle lookup, ordered index, free-space metadata and overflow journal live
// in backing pages. Only fixed cache/journal/index slots reside in userspace;
// no directory, free list, undo vector, dirty map, traversal stack, or hidden
// key cache may grow with records, operations, bytes, or transaction length.
// Construction may reserve fixed, nonthrowing workspace sized ONLY by config;
// no subsequent allocation/growth. Recursion needs caller-supplied fixed
// buffers; the store cannot collect per-read/per-visitor state in a container.
class ScratchStore final : public ScratchReadback {
public:
    // backing/context are borrowed through destruction; no implicit backing
    // creation or medium selection. create_empty requires an empty backing;
    // reopen requires a previously successful commit with matching page size
    // and ordering id. Cache/slot counts may differ on reopen. Supported
    // geometry includes page_count>=2, power-of-two page_size>=512, and
    // journal_slots/index_slots>=1. Bad geometry/open/allocation => status().
    ScratchStore(ScratchStoreConfig config, ScratchBacking* backing) noexcept;
    ~ScratchStore() noexcept override;
    ScratchStore(const ScratchStore&) = delete;
    ScratchStore& operator=(const ScratchStore&) = delete;
    ScratchStore(ScratchStore&&) = delete;
    ScratchStore& operator=(ScratchStore&&) = delete;

    // Construction/open result; no operation is legal unless this is ok.
    ScratchStatus status() const noexcept;
    std::size_t page_size() const noexcept override;
    std::uint64_t record_count() const noexcept override;
    bool transaction_active() const noexcept;

    // begin snapshots logical state including handle allocation. Nested begin
    // is programming_error. Reads thereafter include successful pending edits.
    ScratchStatus begin() noexcept;

    // Requires begin. Upsert by comparator equality; existing handle survives
    // even if value size/page location changes. New records get successive
    // nonzero uint64 handles; committed/erased handles are never recycled.
    // Sources are borrowed for this call only and may be larger than cache.
    // result changes ONLY on success. Filling any resident slot spills to
    // backing, never refuses, implicitly commits, grows RAM, or throws.
    // ANY unserviceable put returns programming_error (including exhausted
    // backing, input/comparison failure or handle domain); no run failure is
    // prescribed. The transaction becomes rollback-only; partial new bytes
    // cannot damage the pre-begin state. Discard outputs from aborted puts.
    ScratchStatus put(const ScratchByteSource& key, const ScratchByteSource& value,
                      ScratchHandle& result) noexcept;
    ScratchStatus erase(ScratchHandle) noexcept; // absent => not_found, no edit

    // Only success publishes a committed state. buffered permits orderly
    // close/reopen; data/full additionally invoke the selected durability hook
    // in journal/data-before-publication order. Failed mutation/commit leaves
    // an active, rollback-only transaction. No implicit commit in destructor:
    // abandoning a transaction and reopening yields the last successful commit.
    ScratchStatus commit(ScratchDurability durability) noexcept;

    // Restore ALL pre-begin logical bytes, handles, count, allocation state and
    // order, even after writes/evictions/journal spill. May leave unreachable
    // physical pages. Outputs/handles first obtained in the aborted transaction
    // must be discarded (its allocation numbers may be issued again).
    // rollback requires an active transaction and a serviceable backing.
    ScratchStatus rollback() noexcept;

    ScratchStatus point(ScratchHandle, ScratchRecordInfo&) const noexcept override;
    ScratchStatus read(ScratchHandle, ScratchField, std::uint64_t offset,
                       ScratchBuffer destination) const noexcept override;
    ScratchStatus visit(ScratchHandle, ScratchBuffer page, ScratchChunkVisitor,
                        void* context) const noexcept override;
    ScratchStatus for_each(ScratchBuffer page, ScratchRecordVisitor,
                           void* context) const noexcept override;

    // No resident cursor/snapshot. end writes scratch_no_handle; other errors
    // leave the output unchanged. next requires a presently live handle and
    // finds its immediate ordered successor. lower_bound finds first key >=
    // probe under the caller comparator (inclusive of equivalence).
    ScratchStatus first(ScratchHandle& result) const noexcept;
    ScratchStatus next(ScratchHandle current, ScratchHandle& result) const noexcept;
    ScratchStatus lower_bound(const ScratchByteSource& probe,
                              ScratchHandle& result) const noexcept;

    // Noncryptographic ordered CONTENT digest, independent of handles, cache,
    // chunk sizes, insertion history, transactions and physical placement.
    // FNV-1a-64: basis 14695981039346656037, prime 1099511628211, mod 2^64.
    // Fold LE64(record_count), then each ascending record's LE64(key_bytes),
    // key bytes, LE64(value_bytes), value bytes. No terminators/padding.
    // Same caller-owned exact-size page and reentrancy rules as for_each.
    // Output changes only on ok; errors never yield a sentinel digest.
    ScratchStatus ordered_digest(ScratchBuffer page, std::uint64_t& result) const noexcept;

    // Logical durable-state digest for consumer state hashing; only legal
    // outside a transaction. Same fold, prefixed with LE64(order.id),
    // LE64(next_handle), then LE64(record_count); each row additionally folds
    // LE64(handle) BEFORE its lengths/bytes. Hash every persisted logical
    // field; physical cache/journal placement is not logical durable state.
    ScratchStatus state_digest(ScratchBuffer page, std::uint64_t& result) const noexcept;

private:
    struct State;
    State* state_ = nullptr; // one fixed workspace; out-of-line implementation
};

} // namespace pineforge::detail
