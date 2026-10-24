// INTERIM copy of the A5 store, replaced by lane a5s (ScratchStore-based); do not edit
// CR-150 A5 — implementation of the O(1) external script-state store.
//
// Storage shape. The backing is one append-only file addressed in page_bytes
// units. Page 0 holds the SuperBlock (version root, allocator top, live
// counters). Every object is an 8-byte-aligned Meta record followed by its
// data extent; the object id is `meta_offset + 1`, so id -> location is one
// multiply and there is no resident directory at all. Ids are never reused
// because offsets are never reused. A release flips the Meta live bit; a dead
// id therefore finds a valid record with a clear live bit, while an id that
// never named an allocation falls outside the allocator top. Both throw:
// a dead id is a caller error, never a silent zero.
//
// Residency. A fixed frame array of `resident_pages` page buffers is created
// at open: frame 0 is page 0 (superblock), frame 1 is the fixed copy scratch,
// frames 2.. are the page cache (clock eviction). `bytes_resident` counts
// every one of those frames that is live, so it can never exceed
// resident_pages * page_bytes. Growth writes the backing, never a frame.
//
// Transactions. begin() flushes committed frames and appends undo records to
// the backing as each page is first modified: the record holds the page's
// committed bytes, and the records form a backwards chain (each record's
// `prev`). rollback() drops the cache's transaction-dirty frames and replays
// the chain in reverse, restoring backing pages byte-exactly; commit() flushes
// the frames, appends a VersionRecord (the version chain also lives in the
// backing) and bumps the superblock version. No step copies an object into the
// heap; the only fixed heap buffers are the resident frames themselves.
//
// A record owns two whole pages and starts on a page boundary (header page,
// then payload page). It is written at align_up(file_top), so no object extent
// covers a record's pages: a rollback restore can therefore never overwrite a
// record that the chain has still to read.
//
// Nested transactions are not supported: begin() inside a transaction throws
// std::logic_error. A Tx that is destroyed without commit() rolls back. A Tx
// must not outlive its Store. The store is not thread-safe.

#include <pineforge/detail/o1_store.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace pineforge {
namespace detail {
namespace o1 {
namespace {

constexpr std::uint64_t kMetaMagic = UINT64_C(0x4f3153544d455441);     // O1STMETA
constexpr std::uint64_t kSuperMagic = UINT64_C(0x4f31535453555052);    // O1STSUPR
constexpr std::uint64_t kVersionMagic = UINT64_C(0x4f31535456455253);  // O1STVERS
constexpr std::uint64_t kMetaBytes = 64;   // reserved stride of one Meta record
constexpr std::uint64_t kJournalHeaderBytes = 32;
// One undo record owns the header page and the payload page that follows it,
// so journal bytes never share a page with an object extent. See the layout
// note at the top of this file.
constexpr std::uint64_t kJournalPagesPerRecord = 2;
constexpr std::uint64_t kNullPage = ~UINT64_C(0);
constexpr std::uint64_t kLiveFlag = 1;
constexpr std::uint64_t kMinPageBytes = 128;
constexpr std::uint64_t kMinResidentPages = 3;  // superblock + scratch + >=1 cache

// SuperBlock is the first record of page 0. Fixed size so it fits any page.
struct SuperBlock {
  std::uint64_t magic;
  std::uint32_t page_bytes;
  std::uint32_t resident_pages;
  std::uint64_t version;       // committed version root
  std::uint64_t version_head;  // offset of the newest VersionRecord
  std::uint64_t file_top;      // next unallocated backing byte
  std::uint64_t bytes_live;
  std::uint64_t live_objects;
};
static_assert(sizeof(SuperBlock) <= 128, "superblock must fit a minimum page");

// One object descriptor. `data_off`/`allocated` only change while a resize
// moves the extent; logical keeps the caller-visible size.
struct Meta {
  std::uint64_t magic;
  std::uint64_t data_off;
  std::uint64_t logical;
  std::uint64_t allocated;
  std::uint64_t align;
  std::uint32_t flags;
  std::uint32_t reserved;
};
static_assert(sizeof(Meta) <= kMetaBytes, "meta must fit its stride");

// Undo record: link back to the previous record, the backing page it restores,
// then the page's committed bytes.
struct JournalEntry {
  std::uint64_t prev;
  std::uint64_t target;
  std::uint64_t length;
  std::uint64_t reserved;
};
static_assert(sizeof(JournalEntry) == kJournalHeaderBytes, "journal header size");

// One node of the version chain, appended by every commit.
struct VersionRecord {
  std::uint64_t magic;
  std::uint64_t version;
  std::uint64_t prev;
  std::uint64_t file_top;
  std::uint64_t bytes_live;
  std::uint64_t live_objects;
};

bool is_power_of_two(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

bool add_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return true;
  *out = a + b;
  return false;
}

bool align_up(std::uint64_t value, std::uint64_t align, std::uint64_t* out) {
  if (!is_power_of_two(align)) return false;
  if (value > std::numeric_limits<std::uint64_t>::max() - (align - 1)) return false;
  *out = (value + (align - 1)) & ~(align - 1);
  return true;
}

std::runtime_error backing_failure(const char* what) {
  return std::runtime_error(std::string("o1 store: ") + what);
}

// Anonymous positioned backing over a file descriptor (memfd or unlinked
// tmpfs/disk file). Capacity is the ftruncate size; grow() never refuses for
// namespace reasons, only an environment failure makes it return 0.
class PosixBacking final : public Backing {
 public:
  explicit PosixBacking(int fd) : fd_(fd) {}
  PosixBacking(const PosixBacking&) = delete;
  PosixBacking& operator=(const PosixBacking&) = delete;
  ~PosixBacking() override {
    if (fd_ >= 0) ::close(fd_);
  }

  bool read_at(std::uint64_t off, void* dst, std::size_t n) override {
    return transfer(off, dst, n, false);
  }
  bool write_at(std::uint64_t off, const void* src, std::size_t n) override {
    return transfer(off, const_cast<void*>(src), n, true);
  }
  std::uint64_t grow(std::uint64_t bytes) override {
    std::uint64_t target = 0;
    if (add_overflow(capacity_, bytes, &target)) return 0;
    if (target > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
      return 0;
    if (::ftruncate(fd_, static_cast<off_t>(target)) != 0) return 0;
    capacity_ = target;
    return capacity_;
  }

 private:
  bool transfer(std::uint64_t off, void* buffer, std::size_t n, bool writing) {
    if (n == 0) return true;
    if (off > capacity_ || n > capacity_ - off) return false;
    auto* bytes = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    const std::size_t max_chunk =
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
    while (done < n) {
      const std::size_t chunk = std::min(n - done, max_chunk);
      const off_t position = static_cast<off_t>(off + done);
      const ssize_t moved = writing
          ? ::pwrite(fd_, bytes + done, chunk, position)
          : ::pread(fd_, bytes + done, chunk, position);
      if (moved < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (moved == 0) return false;
      done += static_cast<std::size_t>(moved);
    }
    return true;
  }

  int fd_;
  std::uint64_t capacity_ = 0;
};

class StoreImpl final : public Store {
 public:
  StoreImpl(const Shape& shape, Backing& backing);
  ~StoreImpl() override;

  StoreImpl(const StoreImpl&) = delete;
  StoreImpl& operator=(const StoreImpl&) = delete;

  Id create(std::uint64_t bytes, std::uint64_t align) override;
  void release(Id id) override;
  bool alive(Id id) const override;
  std::uint64_t size(Id id) const override;
  bool resize(Id id, std::uint64_t bytes) override;
  std::size_t read(Id id, std::uint64_t off, void* dst, std::size_t n) const override;
  std::size_t write(Id id, std::uint64_t off, const void* src, std::size_t n) override;
  std::unique_ptr<Tx> begin() override;
  Stats stats() const override;

  void commit_tx();
  void rollback_tx();

 private:
  struct Frame {
    std::uint64_t page = kNullPage;
    bool dirty = false;
    bool journaled = false;
    std::unique_ptr<unsigned char[]> bytes;
  };

  SuperBlock* sb() const { return reinterpret_cast<SuperBlock*>(frames_[0].bytes.get()); }
  unsigned char* scratch() const { return frames_[1].bytes.get(); }
  std::uint64_t page_bytes() const { return shape_.page_bytes; }

  void ensure_capacity(std::uint64_t need) const;
  Frame& load_page(std::uint64_t page) const;
  Frame& writable_page(std::uint64_t page) const;
  void flush_frame(Frame& frame) const;
  void flush_all() const;
  void mark_super_dirty() const;
  void ensure_super_journaled() const;
  void append_journal_entry(std::uint64_t target, const unsigned char* old_bytes) const;
  void journal_page(Frame& frame) const;

  std::uint64_t meta_offset_for_top(std::uint64_t top) const;
  Meta read_meta(Id id) const;
  void write_meta(std::uint64_t meta_off, const Meta& meta) const;

  Shape shape_;
  Backing& backing_;
  mutable std::uint64_t capacity_ = 0;
  mutable std::vector<Frame> frames_;
  mutable std::size_t resident_cache_ = 0;
  mutable std::size_t clock_ = 2;
  mutable bool in_tx_ = false;
  mutable std::uint64_t tx_last_journal_ = 0;
};

class TxImpl final : public Tx {
 public:
  explicit TxImpl(StoreImpl* store) : store_(store) {}
  TxImpl(const TxImpl&) = delete;
  TxImpl& operator=(const TxImpl&) = delete;
  ~TxImpl() override {
    if (store_ != nullptr && active_) {
      // An abandoned transaction restores the previous version.
      try {
        store_->rollback_tx();
      } catch (...) {
      }
    }
  }
  void commit() override {
    require_active();
    store_->commit_tx();
    active_ = false;
  }
  void rollback() override {
    require_active();
    store_->rollback_tx();
    active_ = false;
  }

 private:
  void require_active() const {
    if (!active_)
      throw std::logic_error("o1 store: transaction already finished");
  }
  StoreImpl* store_;
  bool active_ = true;
};

StoreImpl::StoreImpl(const Shape& shape, Backing& backing)
    : shape_(shape), backing_(backing) {
  if (!is_power_of_two(shape.page_bytes) || shape.page_bytes < kMinPageBytes)
    throw std::invalid_argument(
        "o1 store: page_bytes must be a power of two of at least 128");
  if (shape.resident_pages < kMinResidentPages)
    throw std::invalid_argument(
        "o1 store: resident_pages must be at least 3 (superblock + scratch + cache)");

  const std::uint64_t page = shape.page_bytes;
  capacity_ = backing_.grow(page);
  if (capacity_ == 0) throw backing_failure("cannot size a fresh backing");

  std::vector<unsigned char> zero(static_cast<std::size_t>(page), 0);
  if (!backing_.write_at(0, zero.data(), zero.size()))
    throw backing_failure("cannot zero the superblock page");

  SuperBlock initial{};
  initial.magic = kSuperMagic;
  initial.page_bytes = shape.page_bytes;
  initial.resident_pages = shape.resident_pages;
  initial.version = 1;
  initial.version_head = 0;
  initial.file_top = page;
  initial.bytes_live = 0;
  initial.live_objects = 0;
  if (!backing_.write_at(0, &initial, sizeof initial))
    throw backing_failure("cannot write the superblock");

  frames_.resize(shape.resident_pages);
  for (Frame& frame : frames_)
    frame.bytes = std::make_unique<unsigned char[]>(static_cast<std::size_t>(page));
  if (!backing_.read_at(0, frames_[0].bytes.get(), static_cast<std::size_t>(page)))
    throw backing_failure("cannot read the superblock page");
  frames_[0].page = 0;
  frames_[0].dirty = false;
  frames_[0].journaled = false;
}

StoreImpl::~StoreImpl() {
  try {
    if (in_tx_) {
      rollback_tx();
    } else {
      flush_all();
    }
  } catch (...) {
  }
}

void StoreImpl::ensure_capacity(std::uint64_t need) const {
  if (need <= capacity_) return;
  const std::uint64_t grown = backing_.grow(need - capacity_);
  if (grown == 0) throw backing_failure("backing growth failed");
  if (grown < need) throw backing_failure("backing returned a short capacity");
  capacity_ = grown;
}

StoreImpl::Frame& StoreImpl::load_page(std::uint64_t page) const {
  if (page == 0) return frames_[0];
  const std::size_t count = frames_.size();
  for (std::size_t i = 2; i < count; ++i) {
    if (frames_[i].page == page) return frames_[i];
  }
  std::size_t victim = count;
  for (std::size_t i = 2; i < count; ++i) {
    if (frames_[i].page == kNullPage) {
      victim = i;
      break;
    }
  }
  if (victim == count) {
    victim = clock_;
    clock_ = 2 + ((clock_ - 2 + 1) % (count - 2));
    Frame& old = frames_[victim];
    if (old.dirty) flush_frame(old);
    old.page = kNullPage;
    old.journaled = false;
    --resident_cache_;
  }
  Frame& frame = frames_[victim];
  std::uint64_t off = 0;
  if (page > (std::numeric_limits<std::uint64_t>::max() - page_bytes()) / page_bytes())
    throw std::invalid_argument("o1 store: id outside the store");
  off = page * page_bytes();
  ensure_capacity(off + page_bytes());
  if (!backing_.read_at(off, frame.bytes.get(), static_cast<std::size_t>(page_bytes())))
    throw backing_failure("page read failed");
  frame.page = page;
  frame.dirty = false;
  frame.journaled = false;
  ++resident_cache_;
  return frame;
}

void StoreImpl::flush_frame(Frame& frame) const {
  if (!frame.dirty) return;
  const std::uint64_t off = frame.page * page_bytes();
  ensure_capacity(off + page_bytes());
  if (!backing_.write_at(off, frame.bytes.get(), static_cast<std::size_t>(page_bytes())))
    throw backing_failure("page write failed");
  frame.dirty = false;
}

void StoreImpl::flush_all() const {
  for (Frame& frame : frames_) {
    if (frame.page != kNullPage) flush_frame(frame);
  }
}

void StoreImpl::mark_super_dirty() const { frames_[0].dirty = true; }

void StoreImpl::ensure_super_journaled() const {
  if (!in_tx_) return;
  Frame& frame = frames_[0];
  if (frame.journaled) return;
  append_journal_entry(0, frame.bytes.get());
  frame.journaled = true;
  frame.dirty = true;
}

void StoreImpl::append_journal_entry(std::uint64_t target,
                                     const unsigned char* old_bytes) const {
  SuperBlock* super = sb();
  std::uint64_t entry_off = 0;
  if (!align_up(super->file_top, page_bytes(), &entry_off))
    throw std::overflow_error("o1 store: journal offset exhausted");
  const std::uint64_t total = kJournalPagesPerRecord * page_bytes();
  ensure_capacity(entry_off + total);
  JournalEntry entry{};
  entry.prev = tx_last_journal_;
  entry.target = target;
  entry.length = page_bytes();
  entry.reserved = 0;
  if (!backing_.write_at(entry_off, &entry, sizeof entry))
    throw backing_failure("journal header write failed");
  if (!backing_.write_at(entry_off + page_bytes(), old_bytes,
                         static_cast<std::size_t>(page_bytes())))
    throw backing_failure("journal payload write failed");
  tx_last_journal_ = entry_off;
  super->file_top = entry_off + total;
  frames_[0].dirty = true;
}

void StoreImpl::journal_page(Frame& frame) const {
  if (frame.page == 0) {
    ensure_super_journaled();
    return;
  }
  ensure_super_journaled();
  append_journal_entry(frame.page * page_bytes(), frame.bytes.get());
  frame.journaled = true;
}

StoreImpl::Frame& StoreImpl::writable_page(std::uint64_t page) const {
  Frame& frame = load_page(page);
  if (in_tx_ && !frame.journaled) journal_page(frame);
  frame.dirty = true;
  return frame;
}

std::uint64_t StoreImpl::meta_offset_for_top(std::uint64_t top) const {
  std::uint64_t off = 0;
  if (!align_up(top, 8, &off))
    throw std::overflow_error("o1 store: backing offset exhausted");
  const std::uint64_t within = off % page_bytes();
  if (within + kMetaBytes > page_bytes()) {
    if (!align_up(off, page_bytes(), &off))
      throw std::overflow_error("o1 store: backing offset exhausted");
  }
  return off;
}

Meta StoreImpl::read_meta(Id id) const {
  if (id == kNullId) throw std::invalid_argument("o1 store: null id");
  const SuperBlock* super = sb();
  const std::uint64_t off = id - 1;
  if (off > super->file_top || kMetaBytes > super->file_top - off)
    throw std::invalid_argument("o1 store: dead id");
  const std::uint64_t within = off % page_bytes();
  if (within + kMetaBytes > page_bytes())
    throw std::invalid_argument("o1 store: dead id");
  const Frame& frame = load_page(off / page_bytes());
  Meta meta{};
  std::memcpy(&meta, frame.bytes.get() + within, sizeof meta);
  if (meta.magic != kMetaMagic || (meta.flags & kLiveFlag) == 0)
    throw std::invalid_argument("o1 store: dead id");
  return meta;
}

void StoreImpl::write_meta(std::uint64_t meta_off, const Meta& meta) const {
  const std::uint64_t within = meta_off % page_bytes();
  Frame& frame = writable_page(meta_off / page_bytes());
  std::memcpy(frame.bytes.get() + within, &meta, sizeof meta);
}

Id StoreImpl::create(std::uint64_t bytes, std::uint64_t align) {
  const std::uint64_t alignment = align == 0 ? 1 : align;
  if (!is_power_of_two(alignment))
    throw std::invalid_argument("o1 store: align must be a power of two");

  ensure_super_journaled();
  SuperBlock* super = sb();
  const std::uint64_t meta_off = meta_offset_for_top(super->file_top);
  std::uint64_t data_off = 0;
  if (add_overflow(meta_off, kMetaBytes, &data_off))
    throw std::overflow_error("o1 store: backing offset exhausted");
  if (!align_up(data_off, alignment, &data_off))
    throw std::overflow_error("o1 store: backing offset exhausted");
  std::uint64_t end = 0;
  if (add_overflow(data_off, bytes, &end))
    throw std::overflow_error("o1 store: object size exhausted the backing");
  ensure_capacity(end);

  // Claim the extent before any journal entry can be appended, so an undo
  // record never lands inside the new object.
  super->file_top = end;
  mark_super_dirty();

  Meta meta{};
  meta.magic = kMetaMagic;
  meta.data_off = data_off;
  meta.logical = bytes;
  meta.allocated = bytes;
  meta.align = alignment;
  meta.flags = kLiveFlag;
  write_meta(meta_off, meta);

  super->bytes_live += bytes;
  super->live_objects += 1;
  mark_super_dirty();
  return meta_off + 1;
}

void StoreImpl::release(Id id) {
  const Meta meta = read_meta(id);
  ensure_super_journaled();
  Meta dead = meta;
  dead.flags = 0;
  write_meta(id - 1, dead);
  SuperBlock* super = sb();
  super->bytes_live -= meta.logical;
  super->live_objects -= 1;
  mark_super_dirty();
}

bool StoreImpl::alive(Id id) const {
  try {
    (void)read_meta(id);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

std::uint64_t StoreImpl::size(Id id) const { return read_meta(id).logical; }

bool StoreImpl::resize(Id id, std::uint64_t bytes) {
  const Meta old = read_meta(id);
  ensure_super_journaled();
  SuperBlock* super = sb();

  Meta next = old;
  const std::uint64_t copy_length = std::min(old.logical, bytes);
  if (bytes > old.allocated) {
    // A grown extent moves to the allocator top: later objects already own the
    // bytes that follow the old extent. The copy goes through the fixed scratch
    // page, never through a heap copy of the object.
    std::uint64_t new_off = 0;
    if (!align_up(super->file_top, old.align, &new_off))
      throw std::overflow_error("o1 store: backing offset exhausted");
    std::uint64_t end = 0;
    if (add_overflow(new_off, bytes, &end))
      throw std::overflow_error("o1 store: object size exhausted the backing");
    ensure_capacity(end);
    super->file_top = end;
    mark_super_dirty();

    std::uint64_t copied = 0;
    while (copied < copy_length) {
      const std::uint64_t remaining = copy_length - copied;
      const std::uint64_t source_pos = old.data_off + copied;
      const std::uint64_t source_page = source_pos / page_bytes();
      const std::uint64_t source_within = source_pos % page_bytes();
      const std::uint64_t dest_pos = new_off + copied;
      const std::uint64_t dest_page = dest_pos / page_bytes();
      const std::uint64_t dest_within = dest_pos % page_bytes();
      const std::uint64_t room = std::min(page_bytes() - source_within,
                                          page_bytes() - dest_within);
      const std::size_t chunk =
          static_cast<std::size_t>(std::min(remaining, room));
      const Frame& source = load_page(source_page);
      std::memcpy(scratch(), source.bytes.get() + source_within, chunk);

      Frame& dest = writable_page(dest_page);
      std::memcpy(dest.bytes.get() + dest_within, scratch(), chunk);
      copied += chunk;
    }
    next.data_off = new_off;
    next.allocated = bytes;
  }

  next.logical = bytes;
  write_meta(id - 1, next);
  super->bytes_live -= old.logical;
  super->bytes_live += bytes;
  mark_super_dirty();
  return true;
}

std::size_t StoreImpl::read(Id id, std::uint64_t off, void* dst, std::size_t n) const {
  const Meta meta = read_meta(id);
  if (n == 0 || off >= meta.logical) return 0;
  const std::uint64_t available = meta.logical - off;
  const std::size_t count =
      static_cast<std::size_t>(std::min<std::uint64_t>(n, available));
  auto* out = static_cast<unsigned char*>(dst);
  std::size_t done = 0;
  while (done < count) {
    const std::uint64_t pos = meta.data_off + off + done;
    const std::uint64_t page = pos / page_bytes();
    const std::uint64_t within = pos % page_bytes();
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(count - done, page_bytes() - within));
    const Frame& frame = load_page(page);
    std::memcpy(out + done, frame.bytes.get() + within, chunk);
    done += chunk;
  }
  return done;
}

std::size_t StoreImpl::write(Id id, std::uint64_t off, const void* src, std::size_t n) {
  const Meta meta = read_meta(id);
  if (n == 0 || off >= meta.logical) return 0;
  const std::uint64_t available = meta.logical - off;
  const std::size_t count =
      static_cast<std::size_t>(std::min<std::uint64_t>(n, available));
  const auto* in = static_cast<const unsigned char*>(src);
  std::size_t done = 0;
  while (done < count) {
    const std::uint64_t pos = meta.data_off + off + done;
    const std::uint64_t page = pos / page_bytes();
    const std::uint64_t within = pos % page_bytes();
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(count - done, page_bytes() - within));
    Frame& frame = writable_page(page);
    std::memcpy(frame.bytes.get() + within, in + done, chunk);
    done += chunk;
  }
  return done;
}

std::unique_ptr<Tx> StoreImpl::begin() {
  if (in_tx_) throw std::logic_error("o1 store: nested transactions are not supported");
  flush_all();
  for (Frame& frame : frames_) frame.journaled = false;
  auto transaction = std::make_unique<TxImpl>(this);
  in_tx_ = true;
  tx_last_journal_ = 0;
  return transaction;
}

void StoreImpl::commit_tx() {
  flush_all();
  SuperBlock* super = sb();
  const std::uint64_t record_off = super->file_top;
  ensure_capacity(record_off + sizeof(VersionRecord));
  VersionRecord record{};
  record.magic = kVersionMagic;
  record.version = super->version + 1;
  record.prev = super->version_head;
  record.file_top = record_off + sizeof record;
  record.bytes_live = super->bytes_live;
  record.live_objects = super->live_objects;
  if (!backing_.write_at(record_off, &record, sizeof record))
    throw backing_failure("version record write failed");
  super->version = record.version;
  super->version_head = record_off;
  super->file_top = record.file_top;
  frames_[0].dirty = false;
  frames_[0].journaled = false;
  if (!backing_.write_at(0, super, static_cast<std::size_t>(page_bytes())))
    throw backing_failure("superblock commit write failed");
  for (Frame& frame : frames_) frame.journaled = false;
  in_tx_ = false;
  tx_last_journal_ = 0;
}

void StoreImpl::rollback_tx() {
  // Transaction-dirty cache bytes are the new version; dropping them (without
  // write-back) leaves the backing at the last committed bytes for every page
  // that was never evicted.
  for (std::size_t i = 2; i < frames_.size(); ++i) {
    frames_[i].page = kNullPage;
    frames_[i].dirty = false;
    frames_[i].journaled = false;
  }
  resident_cache_ = 0;
  frames_[0].dirty = false;
  frames_[0].journaled = false;

  // Replay the undo chain newest-first. Every evicted transaction page has a
  // record, so the backing returns to its committed bytes exactly.
  std::uint64_t cursor = tx_last_journal_;
  while (cursor != 0) {
    JournalEntry entry{};
    if (!backing_.read_at(cursor, &entry, sizeof entry))
      throw backing_failure("journal header read failed");
    if (entry.length != page_bytes())
      throw backing_failure("journal entry has an unexpected length");
    if (entry.target > capacity_ || page_bytes() > capacity_ - entry.target)
      throw backing_failure("journal entry points outside the backing");
    if (!backing_.read_at(cursor + page_bytes(), scratch(),
                          static_cast<std::size_t>(page_bytes())))
      throw backing_failure("journal payload read failed");
    if (!backing_.write_at(entry.target, scratch(),
                           static_cast<std::size_t>(page_bytes())))
      throw backing_failure("journal restore write failed");
    cursor = entry.prev;
  }

  if (!backing_.read_at(0, frames_[0].bytes.get(), static_cast<std::size_t>(page_bytes())))
    throw backing_failure("superblock reload failed");
  frames_[0].page = 0;
  frames_[0].dirty = false;
  frames_[0].journaled = false;
  in_tx_ = false;
  tx_last_journal_ = 0;
}

Stats StoreImpl::stats() const {
  const SuperBlock* super = sb();
  Stats out;
  out.bytes_live = super->bytes_live;
  out.bytes_resident = (2 + static_cast<std::uint64_t>(resident_cache_)) * page_bytes();
  out.pages_spilled = capacity_ / page_bytes();
  out.bytes_backing = capacity_;
  return out;
}

}  // namespace

Backing::~Backing() = default;
Tx::~Tx() = default;
Store::~Store() = default;

std::unique_ptr<Store> Store::open(const Shape& shape, Backing& backing) {
  return std::unique_ptr<Store>(new StoreImpl(shape, backing));
}

std::unique_ptr<Backing> make_file_backing(const char* directory) {
  std::string base;
  if (directory != nullptr && *directory != '\0') {
    base = directory;
  } else {
    const char* temporary = std::getenv("TMPDIR");
    base = (temporary != nullptr && *temporary != '\0') ? temporary : "/tmp";
  }
  if (base.back() != '/') base += '/';
  base += "pineforge-o1-store-XXXXXX";
  std::vector<char> name(base.begin(), base.end());
  name.push_back('\0');
  const int fd = ::mkstemp(name.data());
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(),
                            "o1 store: cannot create the backing file");
  if (::unlink(name.data()) != 0) {
    const int error = errno;
    ::close(fd);
    throw std::system_error(error, std::generic_category(),
                            "o1 store: cannot unlink the backing file");
  }
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
    const int error = errno;
    ::close(fd);
    throw std::system_error(error, std::generic_category(),
                            "o1 store: cannot mark the backing close-on-exec");
  }
  return std::unique_ptr<Backing>(new PosixBacking(fd));
}

std::unique_ptr<Backing> make_memfd_backing() {
#if defined(__linux__) && defined(MFD_CLOEXEC)
  const int fd = ::memfd_create("pineforge-o1-store", MFD_CLOEXEC);
  if (fd >= 0) return std::unique_ptr<Backing>(new PosixBacking(fd));
#endif
  // No memfd on this platform: an unlinked tmpfs file is the same anonymous,
  // spillable object to the store.
  return make_file_backing(nullptr);
}

} // namespace o1
} // namespace detail
} // namespace pineforge
