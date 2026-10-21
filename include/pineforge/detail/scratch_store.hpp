#pragma once

// CR150-CX L4 / CR-221 scope B. The shared substrate contract the A3/A4 lanes
// code against; no engine/source policy, D3.7 sink fields or report schema
// changes. docs/scratch-store.md states the same rules in prose and names the
// fixtures that hold them.
#include <pineforge/detail/scratch_readback.hpp>

namespace pineforge::detail {

// Supported geometry floor: a page count or page size below these, or a page
// size that is not a power of two, makes status() programming_error.
inline constexpr std::size_t scratch_min_page_count = 2;
inline constexpr std::size_t scratch_min_page_size = 512;

// Compare entire keys by bounded reads; keys need not fit a page. On ok,
// order is negative/zero/positive. The comparator must be a deterministic
// strict weak ordering; equality identifies ONE logical record (map/upsert
// semantics). It must not throw, allocate with population, mutate the store,
// retain sources, depend on addresses or consult mutable external state. A
// non-ok status aborts the calling store operation: lower_bound returns it
// unchanged, put and erase report programming_error.
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

// Fixed-size accounting for the growth gate and charged-I/O audits. Every
// field is a plain counter; nothing here grows with records or operations.
struct ScratchStoreStats {
    std::uint64_t resident_bytes = 0; // userspace bytes of the fixed workspace
    std::uint64_t backing_pages = 0;  // backing extent now (page_count)
    std::uint64_t backing_bytes = 0;  // backing_pages * page size
    std::uint64_t page_reads = 0;     // read_page calls issued since construction
    std::uint64_t page_writes = 0;    // write_page calls issued since construction
    std::uint64_t syncs = 0;          // sync calls issued since construction
};

// One exclusive run owner, one transaction at a time. Logical key/value blobs,
// handle lookup, ordered index, free-space metadata and overflow journal live
// in backing pages. Only fixed cache/journal/index slots reside in userspace;
// no directory, free list, undo vector, dirty map, traversal stack, or hidden
// key cache may grow with records, operations, bytes, or transaction length.
// Construction may reserve fixed, nonthrowing workspace sized ONLY by config;
// no subsequent allocation/growth. Recursion needs caller-supplied fixed
// buffers; the store cannot collect per-read/per-visitor state in a container.
// Space freed by erase, replacement, commit or rollback is reusable: a
// workload whose live bytes stay bounded runs in a bounded backing extent.
class ScratchStore final : public ScratchReadback {
public:
    // backing/context are borrowed through destruction; no implicit backing
    // creation or medium selection. create_empty requires an empty backing;
    // reopen requires a previously successful commit with matching page size
    // and ordering id. Cache/slot counts may differ on reopen. Supported
    // geometry is exactly page_count>=2, a power-of-two page_size>=512 equal
    // to the backing page size, journal_slots>=1 and index_slots>=1, with a
    // nonzero order id and a comparator. Failures surface through status():
    // bad geometry, null backing, create_empty on a used backing, an ordering
    // id mismatch or workspace allocation failure => programming_error; reopen
    // of an empty backing => not_found; reopen without a valid committed state
    // => corrupt; backing I/O failure => io_error. A store whose status is not
    // ok answers every other call with programming_error and changes nothing;
    // its record_count is 0, its transaction_active is false, it performs no
    // backing I/O and its destructor is harmless.
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

    // True from a successful begin until a successful commit or rollback,
    // including while the transaction is rollback-only.
    bool transaction_active() const noexcept;

    // begin snapshots logical state including handle allocation. Nested begin
    // is programming_error. Reads thereafter include successful pending edits.
    // A rollback-only transaction accepts nothing but rollback: begin, put,
    // erase and commit return programming_error with no effect. A mutation
    // attempted from inside any read, comparison or source callback returns
    // programming_error with no effect at all (it does not poison the
    // transaction).
    ScratchStatus begin() noexcept;

    // Requires begin. Upsert by comparator equality: the equal record keeps its
    // handle and takes this call's key and value bytes, even if their size or
    // page location changes. A new record gets the allocation counter's value
    // (1 for the first record of a created store), then the counter advances
    // by one; committed and erased handles are never recycled. Sources are borrowed for this
    // call only, may be larger than cache and must not read this store. result
    // changes ONLY on success. Filling any resident slot spills to backing,
    // never refuses, implicitly commits, grows RAM, or throws. ANY unserviceable
    // put returns programming_error (including exhausted backing, backing I/O
    // failure, input/comparison failure, a call outside a transaction or
    // handle domain exhaustion); no run failure is prescribed. A failure inside
    // a transaction makes it rollback-only; partial new bytes cannot damage
    // the pre-begin state. Discard outputs from aborted puts.
    ScratchStatus put(const ScratchByteSource& key, const ScratchByteSource& value,
                      ScratchHandle& result) noexcept;

    // Requires begin. An absent handle (including scratch_no_handle) => not_found
    // with no edit and no rollback-only effect. A call outside a transaction
    // or any other failure => programming_error, which in a transaction makes
    // it rollback-only.
    ScratchStatus erase(ScratchHandle) noexcept;

    // Only success publishes a committed state. buffered permits orderly
    // close/reopen; data/full additionally invoke the selected durability hook
    // in journal/data-before-publication order. Requires an active transaction
    // that is not rollback-only (else programming_error, no effect). Failed
    // mutation/commit leaves an active, rollback-only transaction and reports
    // the failing backing call's status. No implicit commit in destructor:
    // abandoning a transaction and reopening yields the last successful commit.
    ScratchStatus commit(ScratchDurability durability) noexcept;

    // Restore ALL pre-begin logical bytes, handles, count, allocation state and
    // order, even after writes/evictions/journal spill and also from a
    // rollback-only transaction. Space the aborted transaction used stays
    // reusable: repeated begin/put/rollback cycles do not grow the backing
    // without bound. Outputs/handles first obtained in the aborted transaction
    // must be discarded (its allocation numbers are issued again). rollback
    // requires an active transaction and a serviceable backing.
    ScratchStatus rollback() noexcept;

    // While the active transaction is rollback-only, every read, walk and
    // digest below returns programming_error (its pending state may be partial).
    ScratchStatus point(ScratchHandle, ScratchRecordInfo&) const noexcept override;
    ScratchStatus read(ScratchHandle, ScratchField, std::uint64_t offset,
                       ScratchBuffer destination) const noexcept override;
    ScratchStatus visit(ScratchHandle, ScratchBuffer page, ScratchChunkVisitor,
                        void* context) const noexcept override;
    ScratchStatus for_each(ScratchBuffer page, ScratchRecordVisitor,
                           void* context) const noexcept override;

    // No resident cursor/snapshot. end writes scratch_no_handle; other errors
    // leave the output unchanged. next requires a presently live handle (else
    // not_found) and finds its immediate ordered successor. lower_bound finds
    // first key >= probe under the caller comparator (inclusive of
    // equivalence). A comparator failure is returned unchanged.
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

    // Fixed-size accounting; always available, never fails. resident_bytes is
    // constant for the object's lifetime, the three counters cover the backing
    // calls this store issued, backing_pages is the backing extent right now.
    ScratchStoreStats stats() const noexcept;

private:
    struct State;
    State* state_ = nullptr; // one fixed workspace; out-of-line implementation
};

} // namespace pineforge::detail
