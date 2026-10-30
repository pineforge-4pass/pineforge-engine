// O(1) resident script-state store contract.
//
// PURPOSE. Script-owned state (arrays, strings, map payloads, matrices, UDT graphs,
// drawing records) grows with bars or with objects the script creates. This header
// defines the shared store interface so that state lives in a fixed
// resident working set over a spilling backing, addressed by stable ids, with exact
// transactional versions for calc-on-fill rollback.
//
// STORE GUARANTEES:
//  - A write or resize never fails because the resident window is full; it spills.
//    Creation is bounded by Shape::max_live_objects, independent of lifetime ids.
//  - Resident bytes are bounded by the ScratchStore workspace picked at open,
//    independent of bars B and of objects created.
//  - Ids are stable and never recycled while reachable; a released id is dead, and
//    reads of a dead id are a caller error, not a silent zero.
//  - Transaction rollback restores every touched root byte-exactly; version chains
//    live in the backing, never in a growing in-heap copy.
//  - Every read and write is exact: no approximation, no dropped old element, no
//    reordering that a script can observe.
//
// IMPLEMENTATION SHAPE. This interface is a THIN
// OBJECT LAYER over pineforge::detail::ScratchStore, so the project has one page
// cache and one backing boundary. An object is a header record plus fixed-size data
// chunk records, each record keyed by the 16-byte big-endian pair (object id, chunk
// number) and ordered by a plain bytewise compare. Chunk 0's value is the 16-byte
// header (logical size, alignment) followed by that chunk's data bytes; every later
// chunk's value is its data bytes. The layer keeps no per-object, per-chunk or
// per-byte heap state: an id counter, a few scalars and fixed buffers only.
//
// DECLARATIONS ONLY. The single implementation is src/detail/o1_store.cpp;
// all store consumers use this header rather than separate implementations.

#ifndef PINEFORGE_DETAIL_O1_STORE_HPP
#define PINEFORGE_DETAIL_O1_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <pineforge/detail/scratch_backing.hpp>

namespace pineforge {
namespace detail {
namespace o1 {

using Id = std::uint64_t;
inline constexpr Id kNullId = UINT64_C(0);

// Fixed chunk-byte and resident-page-COUNT bounds, not a resident-byte cap.
// ScratchStore resident = (resident_pages + 6 work pages + metadata in pages)
// x page_bytes: at most about 1 GiB at its own 1 MiB page-size limit.
// Larger chunk_bytes/resident_pages are caller errors. These fixed limits
// bound the supported chunk size and resident page count, respectively.
inline constexpr std::uint32_t kMaxChunkBytes = 64u * 1024u;
inline constexpr std::uint32_t kMaxResidentPages = 1024u;
// Live-object default and cap: 1,048,576. Zero selects the default; larger
// values clamp to the cap, and smaller explicit values are kept.
inline constexpr std::uint64_t kMaxLiveObjectsCap = UINT64_C(1024) * 1024;

// Caller budget: outer script scope + joined collection + graph operation +
// one headroom slot. Child container operations join without another handle,
// independent of container depth. Finished handles occupy slots until destroyed;
// the one cleanup slot is separate. This is an internal bug guard, unreachable
// from a valid script, rather than a limit on objects or nesting.
inline constexpr std::size_t kMaxTxHandles = 4;

class TxPoolExhausted : public std::runtime_error {
 public:
  TxPoolExhausted()
      : std::runtime_error("o1 store: transaction handle pool exhausted") {}
};

// Refusal of work in an already poisoned transaction, distinct from the
// backing/codec failure that may have poisoned it. Thrown by the store only;
// a codec must not throw it.
class RollbackOnly : public std::logic_error {
 public:
  RollbackOnly()
      : std::logic_error("o1 store: transaction is rollback-only") {}
};

enum class CleanupSkip { rollback_only, failed };

struct Shape {
  std::uint32_t page_bytes = 4096;   // ScratchStore page size; must equal the backing's
  std::uint32_t resident_pages = 64; // ScratchStore resident cache pages; fixed at open
  // EXT: one data chunk record covers this many object bytes, so it is the backing
  // cost of one element write and the object's rounding granularity. Appended after
  // the first contract; the fields above keep their meaning and defaults.
  std::uint32_t chunk_bytes = 1024;
  // EXT: ScratchStore requires these counts >= 1 (zero is normalized to 1 here).
  // Otherwise passed through; ScratchStore's workspace sizing never reads them.
  std::uint32_t journal_slots = 8;
  std::uint32_t index_slots = 8;
  // EXT: maximum live objects in the current transaction. Zero selects the
  // default (kMaxLiveObjectsCap); larger values clamp to the cap at open,
  // and smaller explicit values are kept.
  std::uint64_t max_live_objects = kMaxLiveObjectsCap;
};

struct Stats {
  std::uint64_t bytes_live = 0;     // logical bytes held by live ids
  std::uint64_t bytes_resident = 0; // fixed ScratchStore workspace, constant per store
  std::uint64_t pages_spilled = 0;  // backing pages currently allocated
  std::uint64_t bytes_backing = 0;  // file growth, never charged as resident
  std::uint64_t max_live_handles = 0; // caller high-water, finished handles included
  // Sticky diagnostics, never rolled back. Only cleanup_failed must be zero
  // for a clean corpus run; rollback-only refusal is expected on valid scripts.
  std::uint64_t cleanup_skipped_rollback_only = 0;
  std::uint64_t cleanup_failed = 0;
  std::uint64_t objects_live = 0;     // live ids as seen by the current transaction
  std::uint64_t max_live_objects = 0; // normalized Shape limit, fixed at open
};

// Transactional root for calc-on-fill rollback. All mutations between begin() and
// commit() belong to a new version; rollback() restores the previous version of
// every object exactly.
//
// One outer transaction at a time, flattened. begin() with no active transaction
// maps to the ScratchStore transaction. begin() while one is active returns a
// JOINED scope: its commit() is a no-op, its rollback() marks the outer transaction
// rollback-only and undoes nothing by itself, and destroying it uncommitted marks
// the outer transaction rollback-only too. While rollback-only begin() and every
// mutation throw RollbackOnly (a std::logic_error); the outer commit() performs
// the real rollback and then throws the same error; the outer rollback() always
// works. There is no savepoint. A backing failure in any call, including a
// read-only one, makes the open outer transaction rollback-only.
// Destroy all Tx handles before the next
// outer begin(): destroying an unfinished stale outer handle later can roll that
// NEW transaction back.
class Tx {
 public:
  virtual ~Tx();
  virtual void commit() = 0;
  virtual void rollback() = 0;
};

class TxScope;

// Single-threaded: read() const mutates its key and chunk buffers.
class Store {
 public:
  virtual ~Store();

  // Opens a fresh object layer over one borrowed backing. The backing must be
  // empty (create_empty) and its page size must equal Shape::page_bytes.
  static std::unique_ptr<Store> open(const Shape&, ScratchBacking&);

  // Object lifecycle. `bytes` is the initial logical size; `align` is a power of two.
  // Creating beyond max_live_objects throws ScratchCapacityError (cohort) before
  // any mutation. For a direct store create(), an open Tx stays usable. Through
  // a container operation, the joined scope that saw the throw poisons the outer
  // transaction; there is no savepoint.
  virtual Id create(std::uint64_t bytes, std::uint64_t align) = 0;
  virtual void release(Id) = 0;                 // id becomes dead; never reused
  virtual bool alive(Id) const = 0;
  virtual std::uint64_t size(Id) const = 0;     // logical bytes

  // Resize preserves contents up to min(old, new) and never refuses. Growth is
  // backed by the backing, not by the resident window.
  virtual bool resize(Id, std::uint64_t bytes) = 0;

  // Exact page-granular access. Return the byte count moved; a short count means
  // the request crossed the logical end, which the caller must treat as an error.
  // A mutating call outside a transaction runs as its own one-call transaction.
  virtual std::size_t read(Id, std::uint64_t off, void* dst, std::size_t n) const = 0;
  virtual std::size_t write(Id, std::uint64_t off, const void* src, std::size_t n) = 0;

  // Successful begin() calls allocate no heap memory: caller handles reuse a
  // pool allocated at open(), with kMaxTxHandles simultaneously outstanding
  // caller handles (including finished ones). Exhaustion throws TxPoolExhausted
  // before opening or changing a transaction; there is no heap fallback.
  // This guards an internal call-shape bug, unreachable from a valid script.
  // Destructor cleanup uses begin_cleanup()'s separate reserved slot.
  // Pool access and handle destruction are NOT thread-safe, like the store.
  // Destroying a handle returns its slot. The store must outlive unfinished
  // handles; finished handles may outlive it. Other successful operations
  // reuse the open workspace.
  virtual std::unique_ptr<Tx> begin() = 0;

  static constexpr std::size_t max_caller_handles() noexcept {
    return kMaxTxHandles;
  }

  // A container operation joins an active transaction without a pool handle.
  // With no active transaction it owns one begin() handle. Commit, rollback,
  // abandonment and rollback-only errors have the same meaning as for Tx.
  // The returned stack scope must finish before its enclosing transaction.
  TxScope scope();

  // internal: for TxScope and forwarding wrappers only; calling
  // abandon_joined_tx() poisons the enclosing transaction.
  // TxScope calls these hooks on the Store it was created from. A forwarding
  // wrapper must forward BOTH hooks to its inner store: joining checks for
  // RollbackOnly and returns whether an outer transaction is active;
  // abandonment marks that transaction rollback-only without a pool handle.
  virtual bool join_active_tx() = 0;
  virtual void abandon_joined_tx() noexcept = 0;

  // Serialized destructor cleanup, with the same flattened transaction and
  // rollback-only semantics as begin(). Uses one reserved slot allocated at
  // open(), never available to begin(), and allocates no heap memory.
  // Maximum handle depth: kMaxTxHandles caller handles plus ONE cleanup handle,
  // including finished handles. Destroy the cleanup handle before another
  // cleanup begins. Map destructors do so before nested member destruction;
  // their stored-payload releases do not open cleanup handles.
  // Reserved-slot exhaustion is an internal bug and throws TxPoolExhausted before
  // changing transaction state. Backing and rollback-only failures propagate
  // as for begin(). Pool access and handle destruction are NOT thread-safe.
  // Unwinding an unfinished cleanup handle joined to an active outer transaction
  // poisons that outer transaction (rollback-only).
  // After rollback, surviving aliases/checkpoints keep the native references
  // (map Snapshot retains Storage and restore() rebinds its receiver).
  // Resurrected ids with no surviving native reference remain unreferenced, as
  // do pre-existing objects whose last native reference dies while a
  // rollback-only outer refuses cleanup. A refusal escaping a native
  // recalculation callback fails the run (NativeExecutionConsumer::invoke_recalculation).
  // If the host continues after a refused container operation, one object stays
  // unreferenced until the run ends and cleanup_skipped_rollback_only is its trace.
  virtual std::unique_ptr<Tx> begin_cleanup() = 0;

  // Every map destructor that swallows a release exception records its reason:
  // RollbackOnly is benign; every other exception is a failed cleanup. The
  // string destructor swallows a failed release without recording it.
  // Wrappers forward the reason unchanged.
  virtual void note_cleanup_skipped(CleanupSkip reason) noexcept = 0;
  virtual Stats stats() const = 0;

  // Cache invalidation token, not transactional state. Advances whenever an
  // outer transaction restores the previous version, including a rollback-only
  // commit or abandonment. Joined rollbacks only poison the outer transaction;
  // the token advances when that outer transaction actually rolls back.
  // Forwarding wrappers must return the inner store's token unchanged.
  virtual std::uint64_t rollback_epoch() const noexcept = 0;
};

// Allocation-free joined operation. An unfinished joined scope poisons its
// outer transaction even when the failure came from a codec rather than I/O.
class TxScope {
 public:
  TxScope(const TxScope&) = delete;
  TxScope& operator=(const TxScope&) = delete;
  ~TxScope();
  void commit();
  void rollback();

 private:
  friend class Store;
  explicit TxScope(Store&);
  void require_open() const;

  Store* store_;
  std::unique_ptr<Tx> owned_;
  bool finished_ = false;
};

} // namespace o1
} // namespace detail
} // namespace pineforge

#endif // PINEFORGE_DETAIL_O1_STORE_HPP
