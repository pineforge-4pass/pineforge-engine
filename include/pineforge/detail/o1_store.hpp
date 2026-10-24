// INTERIM copy of the A5 store contract, replaced by lane a5s (ScratchStore-based); do not edit
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
//  - Resident bytes are bounded by Shape::resident_pages * page_bytes, independent
//    of bars B and of objects created. The page directory itself is fixed at open.
//  - Ids are stable and never recycled while reachable; a released id is dead, and
//    reads of a dead id are a caller error, not a silent zero.
//  - Transaction rollback restores every touched root byte-exactly; version chains
//    live in the backing, never in a growing in-heap copy.
//  - Every read and write is exact: no approximation, no dropped old element, no
//    reordering that a script can observe.
//
// DECLARATIONS ONLY. The single implementation is src/detail/o1_store.cpp on the
// lane that owns this file; other lanes bind to this header and must not fork it.
// Integration reconciles this file with the shared G contract before merge.

#ifndef PINEFORGE_DETAIL_O1_STORE_HPP
#define PINEFORGE_DETAIL_O1_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pineforge {
namespace detail {
namespace o1 {

using Id = std::uint64_t;
inline constexpr Id kNullId = UINT64_C(0);

struct Shape {
  std::uint32_t page_bytes = 4096;   // read/write granularity
  std::uint32_t resident_pages = 64; // hard resident ceiling; fixed at open
};

struct Stats {
  std::uint64_t bytes_live = 0;     // logical bytes held by live ids
  std::uint64_t bytes_resident = 0; // <= resident_pages * page_bytes, always
  std::uint64_t pages_spilled = 0;  // pages currently in the backing
  std::uint64_t bytes_backing = 0;  // file growth, never charged as resident
};

// Spilling backing (memfd / tmpfs / file). One per Store. It grows on demand and
// never refuses; it is the deployment half that K (charged I/O) measures.
class Backing {
 public:
  virtual ~Backing();
  virtual bool read_at(std::uint64_t off, void* dst, std::size_t n) = 0;
  virtual bool write_at(std::uint64_t off, const void* src, std::size_t n) = 0;
  virtual std::uint64_t grow(std::uint64_t bytes) = 0; // new capacity, 0 = failure to back
};

// Transactional root for calc-on-fill rollback. All mutations between begin() and
// commit() belong to a new version; rollback() restores the previous version of
// every object exactly. Nested transactions are not part of the contract.
class Tx {
 public:
  virtual ~Tx();
  virtual void commit() = 0;
  virtual void rollback() = 0;
};

class Store {
 public:
  virtual ~Store();

  static std::unique_ptr<Store> open(const Shape&, Backing&);

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
  virtual std::size_t read(Id, std::uint64_t off, void* dst, std::size_t n) const = 0;
  virtual std::size_t write(Id, std::uint64_t off, const void* src, std::size_t n) = 0;

  virtual std::unique_ptr<Tx> begin() = 0;

  virtual Stats stats() const = 0;
};

// EXT: A5-STORE concrete spilling backings. Appended after the contract above;
// no existing declaration is changed. Both are anonymous (no surviving
// directory entry) and grow with ftruncate, so a growth request never refuses
// for lack of namespace space; they throw std::system_error only when the
// environment itself fails (fd, tmpdir, ftruncate). `make_memfd_backing` uses
// memfd_create where the platform has it and falls back to an anonymous
// tmpfs file in TMPDIR elsewhere. `directory` may be null, meaning TMPDIR (or
// /tmp).
std::unique_ptr<Backing> make_memfd_backing();
std::unique_ptr<Backing> make_file_backing(const char* directory);

} // namespace o1
} // namespace detail
} // namespace pineforge

#endif // PINEFORGE_DETAIL_O1_STORE_HPP
