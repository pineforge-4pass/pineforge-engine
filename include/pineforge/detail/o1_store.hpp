// INTERIM: verbatim a5store dependency below; supervisor replaces with the frozen2 provider.
// CR-150 A5 — O(1) resident script-state store contract.
//
// PURPOSE. Script-owned state (arrays, strings, map payloads, matrices, UDT graphs,
// drawing records) grows with bars or with objects the script creates. This header
// fixes the one interface every A5 lane consumes so that state lives in a fixed
// resident working set over a spilling backing, addressed by stable ids, with exact
// transactional versions for calc-on-fill rollback.
//
// RULES THIS CONTRACT ENCODES (owner-settled, 1.7.0):
//  - No capacity refusal. A write or resize never fails because the resident window
//    is full; it spills. Callers must not add a cap the old semantics did not have.
//  - Resident bytes are bounded by the ScratchStore workspace picked at open,
//    independent of bars B and of objects created.
//  - Ids are stable and never recycled while reachable; a released id is dead, and
//    reads of a dead id are a caller error, not a silent zero.
//  - Transaction rollback restores every touched root byte-exactly; version chains
//    live in the backing, never in a growing in-heap copy.
//  - Every read and write is exact: no approximation, no dropped old element, no
//    reordering that a script can observe.
//
// IMPLEMENTATION SHAPE (CORE-TOP ruling L6, 2026-10-21). This interface is a THIN
// OBJECT LAYER over pineforge::detail::ScratchStore, so the project has one page
// cache and one backing boundary. An object is a header record plus fixed-size data
// chunk records, each record keyed by the 16-byte big-endian pair (object id, chunk
// number) and ordered by a plain bytewise compare. Chunk 0's value is the 16-byte
// header (logical size, alignment) followed by that chunk's data bytes; every later
// chunk's value is its data bytes. The layer keeps no per-object, per-chunk or
// per-byte heap state: an id counter, a few scalars and fixed buffers only.
//
// DECLARATIONS ONLY. The single implementation is src/detail/o1_store.cpp on the
// lane that owns this file; other lanes bind to this header and must not fork it.

#ifndef PINEFORGE_DETAIL_O1_STORE_HPP
#define PINEFORGE_DETAIL_O1_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

#include <pineforge/detail/scratch_backing.hpp>

namespace pineforge {
namespace detail {
namespace o1 {

using Id = std::uint64_t;
inline constexpr Id kNullId = UINT64_C(0);

struct Shape {
  std::uint32_t page_bytes = 4096;   // ScratchStore page size; must equal the backing's
  std::uint32_t resident_pages = 64; // ScratchStore resident cache pages; fixed at open
  // EXT: one data chunk record covers this many object bytes, so it is the backing
  // cost of one element write and the object's rounding granularity. Appended after
  // the first contract; the fields above keep their meaning and defaults.
  std::uint32_t chunk_bytes = 1024;
  // EXT: fixed staging-slot floors handed to the ScratchStore configuration. They
  // reserve userspace slots, not a bound on transaction length.
  std::uint32_t journal_slots = 8;
  std::uint32_t index_slots = 8;
};

struct Stats {
  std::uint64_t bytes_live = 0;     // logical bytes held by live ids
  std::uint64_t bytes_resident = 0; // fixed ScratchStore workspace, constant per store
  std::uint64_t pages_spilled = 0;  // backing pages currently allocated
  std::uint64_t bytes_backing = 0;  // file growth, never charged as resident
};

// Transactional root for calc-on-fill rollback. All mutations between begin() and
// commit() belong to a new version; rollback() restores the previous version of
// every object exactly.
//
// One outer transaction at a time, flattened. begin() with no active transaction
// maps to the ScratchStore transaction. begin() while one is active returns a
// JOINED scope: its commit() is a no-op, its rollback() marks the outer transaction
// rollback-only and undoes nothing by itself, and destroying it uncommitted marks
// the outer transaction rollback-only too. While rollback-only every mutation
// throws std::logic_error; the outer commit() performs the real rollback and then
// throws the same error; the outer rollback() always works. There is no savepoint.
class Tx {
 public:
  virtual ~Tx();
  virtual void commit() = 0;
  virtual void rollback() = 0;
};

class Store {
 public:
  virtual ~Store();

  // Opens a fresh object layer over one borrowed backing. The backing must be
  // empty (create_empty) and its page size must equal Shape::page_bytes.
  static std::unique_ptr<Store> open(const Shape&, ScratchBacking&);

  // Object lifecycle. `bytes` is the initial logical size; `align` is a power of two.
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

  virtual std::unique_ptr<Tx> begin() = 0;

  virtual Stats stats() const = 0;
};

} // namespace o1
} // namespace detail
} // namespace pineforge

#endif // PINEFORGE_DETAIL_O1_STORE_HPP
