// Implementation of the O(1) external script-state store.
//
// Storage shape. This is a thin object layer over ScratchStore. An object id is a
// monotonic counter value that is never reused (not after release and not after a
// rollback of the transaction that created it). Every record belongs to exactly one
// object and is keyed by a 16-byte big-endian pair (id, chunk number); the caller
// comparator orders keys with a plain bytewise memcmp, so equal keys identify one
// record. Chunk 0 always exists and holds the 16-byte big-endian header (logical
// size, alignment) followed by its data bytes; chunk k>0 holds its data bytes. The
// data of a chunk is exactly the logical bytes that chunk covers, so growth and
// shrink only rewrite the boundary chunk and add or erase whole records.
//
// Residency. The layer owns no per-object directory: every lookup is a lower_bound
// over the ScratchStore index, and the only heap storage is the fixed ScratchStore
// workspace, one chunk buffer and a fixed Tx-handle pool, all sized at open.
// Comparison buffers are inline. Finished handles can retain the pool after the
// store is destroyed; no handle can cause it to grow.
// ScratchStore spills to the backing and never refuses for lack of a cache slot.
//
// Transactions. There is one outer transaction, mapped onto the ScratchStore
// transaction, and joined scopes that flatten into it. The layer keeps only the
// scalar rollback baselines of the logical byte and live-object counts;
// ScratchStore owns the undo journal and restores every record byte-exactly.
// A mutation made outside any transaction is wrapped in its own one-call
// transaction, because ScratchStore requires one. Nested scopes never open a
// second ScratchStore transaction and no savepoint is provided.

#include <pineforge/detail/o1_store.hpp>

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge {
namespace detail {
namespace o1 {
namespace {

constexpr std::uint64_t kHeaderBytes = 16;
constexpr std::uint64_t kKeyBytes = 16;
constexpr std::uint64_t kOrderId = UINT64_C(0x01A50101);

bool is_power_of_two(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t load_be64(const std::byte* bytes) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(bytes[index]);
  }
  return value;
}

void store_be64(std::byte* bytes, std::uint64_t value) noexcept {
  for (int index = 0; index < 8; ++index) {
    bytes[7 - index] = static_cast<std::byte>(value & UINT64_C(0xff));
    value >>= 8;
  }
}

const char* status_text(ScratchStatus status) noexcept {
  switch (status) {
    case ScratchStatus::ok: return "ok";
    case ScratchStatus::end: return "end";
    case ScratchStatus::cancelled: return "cancelled";
    case ScratchStatus::not_found: return "not_found";
    case ScratchStatus::programming_error: return "programming_error";
    case ScratchStatus::io_error: return "io_error";
    case ScratchStatus::corrupt: return "corrupt";
  }
  return "unknown";
}

// ScratchKeyOrder's context is fixed at open, so the two bounded key buffers live
// in the store object. The comparator reads whatever key length it is handed and
// never allocates, mutates the store or consults external state.
struct CompareContext {
  std::byte left[kKeyBytes] = {};
  std::byte right[kKeyBytes] = {};
};

ScratchStatus compare_keys(void* context, const ScratchByteSource& left,
                           const ScratchByteSource& right, int& order) noexcept {
  auto* buffers = static_cast<CompareContext*>(context);
  const std::uint64_t left_size = left.size();
  const std::uint64_t right_size = right.size();
  const std::uint64_t common = left_size < right_size ? left_size : right_size;
  std::uint64_t done = 0;
  while (done < common) {
    const std::uint64_t room = common - done;
    const std::size_t take = static_cast<std::size_t>(
        room < kKeyBytes ? room : kKeyBytes);
    ScratchStatus status =
        left.read(done, ScratchBuffer{buffers->left, take});
    if (status != ScratchStatus::ok) return status;
    status = right.read(done, ScratchBuffer{buffers->right, take});
    if (status != ScratchStatus::ok) return status;
    const int compared = std::memcmp(buffers->left, buffers->right, take);
    if (compared != 0) {
      order = compared < 0 ? -1 : 1;
      return ScratchStatus::ok;
    }
    done += take;
  }
  order = left_size < right_size ? -1 : (left_size > right_size ? 1 : 0);
  return ScratchStatus::ok;
}

} // namespace

class StoreImpl final : public Store {
 public:
  StoreImpl(const Shape& shape, ScratchBacking& backing);
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
  bool join_active_tx() override {
    require_mutable();
    return outer_active_;
  }
  void abandon_joined_tx() noexcept override { mark_rollback_only(); }
  std::unique_ptr<Tx> begin_cleanup() override;
  void note_cleanup_skipped(CleanupSkip reason) noexcept override {
    if (reason == CleanupSkip::rollback_only) ++cleanup_skipped_rollback_only_;
    else ++cleanup_failed_;
  }
  Stats stats() const override;

 private:
  struct Header {
    std::uint64_t size = 0;
    std::uint64_t align = 1;
  };

  class StoreTx;
  class TxPool;
  class ImplicitTx;

  std::uint64_t chunk_count(std::uint64_t bytes) const noexcept;
  std::uint64_t chunk_length(std::uint64_t chunk, std::uint64_t bytes) const noexcept;
  std::size_t value_prefix(std::uint64_t chunk) const noexcept;
  void encode_key(Id id, std::uint64_t chunk) const noexcept;
  ScratchStatus lookup(Id id, std::uint64_t chunk, ScratchHandle& handle) const;
  Header header_for(Id id) const;
  [[noreturn]] void throw_status(const char* what, ScratchStatus status) const;
  void require_mutable() const;
  void take(ScratchStatus status, const char* what);
  void load_chunk(Id id, std::uint64_t chunk, std::size_t value_length) const;
  void put_chunk(Id id, std::uint64_t chunk, const std::byte* value,
                 std::size_t length);
  void erase_chunk(Id id, std::uint64_t chunk);

  std::unique_ptr<Tx> begin_tx(bool cleanup);
  void commit_outer_tx();
  void rollback_outer_tx();
  void abandon_outer_tx() noexcept;
  void mark_rollback_only() noexcept;
  void finish_outer(bool committed) noexcept;

  std::uint64_t chunk_bytes_ = 0;
  std::size_t chunk_buffer_bytes_ = 0;
  mutable CompareContext compare_;
  std::unique_ptr<CommandScratchStore> store_;
  std::unique_ptr<std::byte[]> chunk_buffer_;
  std::unique_ptr<TxPool> tx_pool_;
  Id next_id_ = 1;
  const std::uint64_t max_live_objects_;
  std::uint64_t bytes_live_ = 0;
  std::uint64_t baseline_bytes_live_ = 0;
  std::uint64_t objects_live_ = 0;
  std::uint64_t baseline_objects_live_ = 0;
  std::uint64_t cleanup_skipped_rollback_only_ = 0;
  std::uint64_t cleanup_failed_ = 0;
  bool outer_active_ = false;
  mutable bool rollback_only_ = false;
  mutable std::byte key_buffer_[kKeyBytes] = {};
};

class StoreImpl::ImplicitTx {
 public:
  explicit ImplicitTx(StoreImpl* store)
      : store_(store), begun_(!store->outer_active_) {
    if (begun_) store_->take(store_->store_->begin(), "begin");
  }
  ImplicitTx(const ImplicitTx&) = delete;
  ImplicitTx& operator=(const ImplicitTx&) = delete;
  ~ImplicitTx() {
    if (begun_) (void)store_->store_->rollback();
  }
  void commit() {
    if (!begun_) return;
    store_->take(store_->store_->commit(ScratchDurability::buffered), "commit");
    begun_ = false;
  }

 private:
  StoreImpl* store_;
  bool begun_;
};

class StoreImpl::StoreTx final : public Tx {
 public:
  StoreTx(StoreImpl* store, bool outer) noexcept : store_(store), outer_(outer) {}
  static void* operator new(std::size_t, TxPool& pool, bool cleanup);
  static void operator delete(void* address) noexcept;
  static void operator delete(void* address, TxPool&, bool) noexcept;
  StoreTx(const StoreTx&) = delete;
  StoreTx& operator=(const StoreTx&) = delete;
  ~StoreTx() override {
    if (finished_) return;
    if (!outer_) {
      store_->mark_rollback_only();
      return;
    }
    store_->abandon_outer_tx();
  }
  void commit() override {
    require_open();
    if (!outer_) {
      finished_ = true;
      return;
    }
    store_->commit_outer_tx();
    finished_ = true;
  }
  void rollback() override {
    require_open();
    if (!outer_) {
      store_->mark_rollback_only();
      finished_ = true;
      return;
    }
    store_->rollback_outer_tx();
    finished_ = true;
  }

 private:
  void require_open() const {
    if (finished_)
      throw std::logic_error("o1 store: transaction already finished");
  }

  StoreImpl* store_;
  bool outer_;
  bool finished_ = false;
};

class StoreImpl::TxPool {
 public:
  TxPool() {
    for (Slot& slot : slots_) {
      slot.pool = this;
      slot.next = free_;
      free_ = &slot;
    }
    cleanup_slot_.pool = this;
    cleanup_free_ = &cleanup_slot_;
  }
  TxPool(const TxPool&) = delete;
  TxPool& operator=(const TxPool&) = delete;

  void* acquire(bool cleanup) {
    Slot*& available = cleanup ? cleanup_free_ : free_;
    if (available == nullptr) throw exhausted_;
    Slot* slot = available;
    available = slot->next;
    ++references_;
    if (!cleanup) {
      ++live_handles_;
      max_live_handles_ = std::max(max_live_handles_, live_handles_);
    }
    return slot->storage;
  }

  std::uint64_t max_live_handles() const noexcept { return max_live_handles_; }

  static void recycle(void* address) noexcept {
    // storage is the first member of a standard-layout Slot. Its address is
    // the slot's address; the pool/next metadata survives StoreTx destruction.
    auto* slot = static_cast<Slot*>(address);
    TxPool* pool = slot->pool;
    Slot*& available = slot == &pool->cleanup_slot_
                           ? pool->cleanup_free_ : pool->free_;
    slot->next = available;
    available = slot;
    if (slot != &pool->cleanup_slot_) --pool->live_handles_;
    pool->release();
  }

  void release() noexcept {
    if (--references_ == 0) delete this;
  }

 private:
  struct Slot {
    alignas(StoreTx) unsigned char storage[sizeof(StoreTx)];
    TxPool* pool = nullptr;
    Slot* next = nullptr;
  };
  static_assert(std::is_standard_layout<Slot>::value,
                "Tx slots must be standard-layout");
  static_assert(offsetof(Slot, storage) == 0,
                "Tx storage must start at the slot address");

  Slot slots_[kMaxTxHandles];
  Slot cleanup_slot_;
  Slot* free_ = nullptr;
  Slot* cleanup_free_ = nullptr;
  // One store reference and one per caller/cleanup handle; at most K + 2.
  std::size_t references_ = 1;
  std::uint64_t live_handles_ = 0;
  std::uint64_t max_live_handles_ = 0;
  // Construct the message at open(); copying runtime_error on exhaustion is
  // noexcept and never asks the replaceable allocator for message storage.
  const TxPoolExhausted exhausted_;
};

void* StoreImpl::StoreTx::operator new(std::size_t, TxPool& pool, bool cleanup) {
  return pool.acquire(cleanup);
}

void StoreImpl::StoreTx::operator delete(void* address) noexcept {
  // Tx's virtual destructor selects this deallocation function even through
  // std::unique_ptr<Tx>'s default deleter (C++17 [expr.delete], [class.free]).
  TxPool::recycle(address);
}

void StoreImpl::StoreTx::operator delete(void* address, TxPool&, bool) noexcept {
  TxPool::recycle(address);
}

StoreImpl::~StoreImpl() {
  // During failed construction unique_ptr owns the pool normally. On success,
  // outstanding finished handles keep it alive after the store owner goes away.
  tx_pool_.release()->release();
}

StoreImpl::StoreImpl(const Shape& shape, ScratchBacking& backing)
    : chunk_bytes_(shape.chunk_bytes),
      max_live_objects_(shape.max_live_objects == 0
                            ? kMaxLiveObjectsCap
                            : std::min(shape.max_live_objects, kMaxLiveObjectsCap)) {
  if (!is_power_of_two(shape.page_bytes) ||
      shape.page_bytes < scratch_min_page_size)
    throw std::invalid_argument(
        "o1 store: page_bytes must be a power of two of at least 512");
  if (shape.resident_pages < scratch_min_page_count)
    throw std::invalid_argument("o1 store: resident_pages must be at least 2");
  if (shape.resident_pages > kMaxResidentPages)
    throw std::invalid_argument("o1 store: resident_pages exceeds fixed maximum");
  if (shape.chunk_bytes == 0)
    throw std::invalid_argument("o1 store: chunk_bytes must be at least 1");
  if (shape.chunk_bytes > kMaxChunkBytes)
    throw std::invalid_argument("o1 store: chunk_bytes exceeds fixed maximum");
  if (shape.chunk_bytes >
      std::numeric_limits<std::size_t>::max() - kHeaderBytes)
    throw std::invalid_argument("o1 store: chunk_bytes is too large");
  if (shape.page_bytes != backing.page_size())
    throw std::invalid_argument(
        "o1 store: page_bytes must equal the backing page size");

  chunk_buffer_bytes_ =
      static_cast<std::size_t>(kHeaderBytes) + shape.chunk_bytes;
  chunk_buffer_.reset(new std::byte[chunk_buffer_bytes_]);
  tx_pool_.reset(new TxPool);

  ScratchStoreConfig config;
  config.page_count = shape.resident_pages;
  config.page_size = shape.page_bytes;
  config.journal_slots = shape.journal_slots == 0 ? 1 : shape.journal_slots;
  config.index_slots = shape.index_slots == 0 ? 1 : shape.index_slots;
  config.order.id = kOrderId;
  config.order.context = &compare_;
  config.order.compare = &compare_keys;
  config.mode = ScratchOpenMode::create_empty;
  store_.reset(new CommandScratchStore(config, &backing));
  if (store_->status() != ScratchStatus::ok)
    throw_status("open", store_->status());
}

std::uint64_t StoreImpl::chunk_count(std::uint64_t bytes) const noexcept {
  if (bytes == 0) return 1;
  return 1 + (bytes - 1) / chunk_bytes_;
}

std::uint64_t StoreImpl::chunk_length(std::uint64_t chunk,
                                      std::uint64_t bytes) const noexcept {
  if (bytes == 0) return 0;
  const std::uint64_t start = chunk * chunk_bytes_;
  if (start >= bytes) return 0;
  const std::uint64_t rest = bytes - start;
  return rest < chunk_bytes_ ? rest : chunk_bytes_;
}

std::size_t StoreImpl::value_prefix(std::uint64_t chunk) const noexcept {
  return chunk == 0 ? static_cast<std::size_t>(kHeaderBytes) : 0;
}

void StoreImpl::encode_key(Id id, std::uint64_t chunk) const noexcept {
  store_be64(key_buffer_, id);
  store_be64(key_buffer_ + 8, chunk);
}

ScratchStatus StoreImpl::lookup(Id id, std::uint64_t chunk,
                                ScratchHandle& handle) const {
  encode_key(id, chunk);
  ScratchSpanSource probe(
      ScratchBytes{key_buffer_, static_cast<std::size_t>(kKeyBytes)});
  ScratchHandle found = scratch_no_handle;
  ScratchStatus status = store_->lower_bound(probe, found);
  if (status != ScratchStatus::ok) return status;
  ScratchRecordInfo info{};
  status = store_->point(found, info);
  if (status != ScratchStatus::ok) return status;
  if (info.key_bytes != kKeyBytes) return ScratchStatus::not_found;
  std::byte stored[kKeyBytes];
  status = store_->read(found, ScratchField::key, 0,
                        ScratchBuffer{stored, static_cast<std::size_t>(kKeyBytes)});
  if (status != ScratchStatus::ok) return status;
  if (std::memcmp(stored, key_buffer_, static_cast<std::size_t>(kKeyBytes)) != 0)
    return ScratchStatus::not_found;
  handle = found;
  return ScratchStatus::ok;
}

StoreImpl::Header StoreImpl::header_for(Id id) const {
  if (id == kNullId) throw std::invalid_argument("o1 store: null id");
  ScratchHandle handle = scratch_no_handle;
  const ScratchStatus status = lookup(id, 0, handle);
  if (status == ScratchStatus::end || status == ScratchStatus::not_found)
    throw std::invalid_argument("o1 store: dead id");
  if (status != ScratchStatus::ok) throw_status("header lookup", status);
  std::byte raw[kHeaderBytes];
  const ScratchStatus read_status = store_->read(
      handle, ScratchField::value, 0,
      ScratchBuffer{raw, static_cast<std::size_t>(kHeaderBytes)});
  if (read_status != ScratchStatus::ok) throw_status("header read", read_status);
  Header header;
  header.size = load_be64(raw);
  header.align = load_be64(raw + 8);
  return header;
}

[[noreturn]] void StoreImpl::throw_status(const char* what,
                                         ScratchStatus status) const {
  // Lookup/read failures can follow successful chunk puts/erases. Poison before
  // constructing the exception too: its string allocation can itself throw.
  if (outer_active_) rollback_only_ = true;
  throw std::runtime_error(std::string("o1 store: ") + what + ": " + status_text(status));
}

void StoreImpl::require_mutable() const {
  if (outer_active_ && rollback_only_)
    throw RollbackOnly();
}

void StoreImpl::take(ScratchStatus status, const char* what) {
  if (status == ScratchStatus::ok) return;
  if (outer_active_) rollback_only_ = true;
  throw_status(what, status);
}

void StoreImpl::load_chunk(Id id, std::uint64_t chunk,
                           std::size_t value_length) const {
  ScratchHandle handle = scratch_no_handle;
  const ScratchStatus status = lookup(id, chunk, handle);
  if (status != ScratchStatus::ok) throw_status("chunk lookup", status);
  const ScratchStatus read_status = store_->read(
      handle, ScratchField::value, 0,
      ScratchBuffer{chunk_buffer_.get(), value_length});
  if (read_status != ScratchStatus::ok) throw_status("chunk read", read_status);
}

void StoreImpl::put_chunk(Id id, std::uint64_t chunk, const std::byte* value,
                          std::size_t length) {
  encode_key(id, chunk);
  ScratchSpanSource key(
      ScratchBytes{key_buffer_, static_cast<std::size_t>(kKeyBytes)});
  ScratchSpanSource value_source(ScratchBytes{value, length});
  ScratchHandle result = scratch_no_handle;
  take(store_->put(key, value_source, result), "put");
}

void StoreImpl::erase_chunk(Id id, std::uint64_t chunk) {
  ScratchHandle handle = scratch_no_handle;
  const ScratchStatus status = lookup(id, chunk, handle);
  if (status == ScratchStatus::end || status == ScratchStatus::not_found) return;
  if (status != ScratchStatus::ok) throw_status("chunk lookup", status);
  take(store_->erase(handle), "erase");
}

Id StoreImpl::create(std::uint64_t bytes, std::uint64_t align) {
  require_mutable();
  const std::uint64_t alignment = align == 0 ? 1 : align;
  if (!is_power_of_two(alignment))
    throw std::invalid_argument("o1 store: align must be a power of two");
  if (objects_live_ >= max_live_objects_)
    throw ScratchCapacityError(ScratchResource::cohort,
        scratch_object_limit_message(max_live_objects_, objects_live_ + 1,
                                     kMaxLiveObjectsCap));
  if (next_id_ == std::numeric_limits<Id>::max())
    throw std::overflow_error("o1 store: id space exhausted");
  const Id id = next_id_;
  ++next_id_;
  const std::uint64_t chunks = chunk_count(bytes);
  std::memset(chunk_buffer_.get(), 0, chunk_buffer_bytes_);
  store_be64(chunk_buffer_.get(), bytes);
  store_be64(chunk_buffer_.get() + 8, alignment);
  ImplicitTx transaction(this);
  put_chunk(id, 0, chunk_buffer_.get(),
            static_cast<std::size_t>(kHeaderBytes + chunk_length(0, bytes)));
  // Data chunks start zeroed: the buffer head held the header bytes above.
  std::memset(chunk_buffer_.get(), 0, chunk_buffer_bytes_);
  for (std::uint64_t chunk = 1; chunk < chunks; ++chunk) {
    put_chunk(id, chunk, chunk_buffer_.get(),
              static_cast<std::size_t>(chunk_length(chunk, bytes)));
  }
  transaction.commit();
  bytes_live_ += bytes;
  ++objects_live_;
  return id;
}

void StoreImpl::release(Id id) {
  require_mutable();
  const Header header = header_for(id);
  const std::uint64_t chunks = chunk_count(header.size);
  ImplicitTx transaction(this);
  for (std::uint64_t chunk = chunks; chunk-- > 1;) erase_chunk(id, chunk);
  erase_chunk(id, 0);
  transaction.commit();
  bytes_live_ -= header.size;
  --objects_live_;
}

bool StoreImpl::alive(Id id) const {
  if (id == kNullId) return false;
  ScratchHandle handle = scratch_no_handle;
  const ScratchStatus status = lookup(id, 0, handle);
  if (status == ScratchStatus::ok) return true;
  if (status == ScratchStatus::end || status == ScratchStatus::not_found)
    return false;
  throw_status("alive lookup", status);
}

std::uint64_t StoreImpl::size(Id id) const { return header_for(id).size; }

bool StoreImpl::resize(Id id, std::uint64_t bytes) {
  require_mutable();
  const Header header = header_for(id);
  if (bytes == header.size) return true;
  const std::uint64_t old_chunks = chunk_count(header.size);
  const std::uint64_t new_chunks = chunk_count(bytes);
  ImplicitTx transaction(this);
  for (std::uint64_t chunk = 0; chunk < new_chunks; ++chunk) {
    const std::uint64_t target = chunk_length(chunk, bytes);
    const bool exists = chunk < old_chunks;
    const std::uint64_t current = exists ? chunk_length(chunk, header.size) : 0;
    if (target <= current) continue;
    const std::size_t prefix = value_prefix(chunk);
    if (exists) {
      load_chunk(id, chunk, prefix + static_cast<std::size_t>(current));
    } else {
      std::memset(chunk_buffer_.get(), 0,
                  prefix + static_cast<std::size_t>(target));
    }
    std::memset(chunk_buffer_.get() + prefix + current, 0,
                static_cast<std::size_t>(target - current));
    put_chunk(id, chunk, chunk_buffer_.get(),
              prefix + static_cast<std::size_t>(target));
  }
  for (std::uint64_t chunk = 0; chunk < old_chunks; ++chunk) {
    if (chunk >= new_chunks) {
      erase_chunk(id, chunk);
      continue;
    }
    const std::uint64_t target = chunk_length(chunk, bytes);
    const std::uint64_t current = chunk_length(chunk, header.size);
    if (target >= current) continue;
    const std::size_t prefix = value_prefix(chunk);
    load_chunk(id, chunk, prefix + static_cast<std::size_t>(current));
    put_chunk(id, chunk, chunk_buffer_.get(),
              prefix + static_cast<std::size_t>(target));
  }
  const std::uint64_t first_length = chunk_length(0, bytes);
  load_chunk(id, 0, static_cast<std::size_t>(kHeaderBytes + first_length));
  store_be64(chunk_buffer_.get(), bytes);
  store_be64(chunk_buffer_.get() + 8, header.align);
  put_chunk(id, 0, chunk_buffer_.get(),
            static_cast<std::size_t>(kHeaderBytes + first_length));
  transaction.commit();
  bytes_live_ = bytes_live_ - header.size + bytes;
  return true;
}

std::size_t StoreImpl::read(Id id, std::uint64_t off, void* dst,
                            std::size_t n) const {
  const Header header = header_for(id);
  if (n == 0 || off >= header.size) return 0;
  const std::size_t count = static_cast<std::size_t>(
      std::min<std::uint64_t>(n, header.size - off));
  auto* out = static_cast<unsigned char*>(dst);
  std::size_t done = 0;
  while (done < count) {
    const std::uint64_t position = off + done;
    const std::uint64_t chunk = position / chunk_bytes_;
    const std::uint64_t within = position % chunk_bytes_;
    const std::uint64_t room = chunk_bytes_ - within;
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(count - done, room));
    ScratchHandle handle = scratch_no_handle;
    const ScratchStatus status = lookup(id, chunk, handle);
    if (status != ScratchStatus::ok) throw_status("chunk lookup", status);
    const std::size_t prefix = value_prefix(chunk);
    const ScratchStatus read_status = store_->read(
        handle, ScratchField::value,
        static_cast<std::uint64_t>(prefix) + within,
        ScratchBuffer{reinterpret_cast<std::byte*>(out + done), take});
    if (read_status != ScratchStatus::ok) throw_status("chunk read", read_status);
    done += take;
  }
  return done;
}

std::size_t StoreImpl::write(Id id, std::uint64_t off, const void* src,
                             std::size_t n) {
  require_mutable();
  const Header header = header_for(id);
  if (n == 0 || off >= header.size) return 0;
  const std::size_t count = static_cast<std::size_t>(
      std::min<std::uint64_t>(n, header.size - off));
  const auto* in = static_cast<const unsigned char*>(src);
  ImplicitTx transaction(this);
  std::size_t done = 0;
  while (done < count) {
    const std::uint64_t position = off + done;
    const std::uint64_t chunk = position / chunk_bytes_;
    const std::uint64_t within = position % chunk_bytes_;
    const std::uint64_t room = chunk_bytes_ - within;
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(count - done, room));
    const std::uint64_t length = chunk_length(chunk, header.size);
    const std::size_t prefix = value_prefix(chunk);
    load_chunk(id, chunk, prefix + static_cast<std::size_t>(length));
    std::memcpy(chunk_buffer_.get() + prefix + within, in + done, take);
    put_chunk(id, chunk, chunk_buffer_.get(),
              prefix + static_cast<std::size_t>(length));
    done += take;
  }
  transaction.commit();
  return done;
}

std::unique_ptr<Tx> StoreImpl::begin() {
  return begin_tx(false);
}

std::unique_ptr<Tx> StoreImpl::begin_cleanup() {
  return begin_tx(true);
}

std::unique_ptr<Tx> StoreImpl::begin_tx(bool cleanup) {
  require_mutable();
  // Reserve a pool slot before opening an outer transaction: exhaustion must
  // not leave an active transaction whose caller never received a handle.
  const bool outer = !outer_active_;
  std::unique_ptr<Tx> transaction(new (*tx_pool_, cleanup) StoreTx(this, outer));
  if (!outer) return transaction;
  take(store_->begin(), "begin");
  outer_active_ = true;
  rollback_only_ = false;
  baseline_bytes_live_ = bytes_live_;
  baseline_objects_live_ = objects_live_;
  return transaction;
}

Stats StoreImpl::stats() const {
  const ScratchStoreStats scratch = store_->stats();
  Stats out;
  out.bytes_live = bytes_live_;
  out.bytes_resident = scratch.resident_bytes;
  out.pages_spilled = scratch.backing_pages;
  out.bytes_backing = scratch.backing_bytes;
  out.max_live_handles = tx_pool_->max_live_handles();
  out.cleanup_skipped_rollback_only = cleanup_skipped_rollback_only_;
  out.cleanup_failed = cleanup_failed_;
  out.objects_live = objects_live_;
  out.max_live_objects = max_live_objects_;
  return out;
}

void StoreImpl::commit_outer_tx() {
  if (!outer_active_)
    throw std::logic_error("o1 store: no active transaction");
  if (rollback_only_) {
    const ScratchStatus status = store_->rollback();
    if (status != ScratchStatus::ok) throw_status("rollback", status);
    finish_outer(false);
    throw RollbackOnly();
  }
  const ScratchStatus status = store_->commit(ScratchDurability::buffered);
  if (status != ScratchStatus::ok) {
    rollback_only_ = true;
    throw_status("commit", status);
  }
  finish_outer(true);
}

void StoreImpl::rollback_outer_tx() {
  if (!outer_active_) return;
  const ScratchStatus status = store_->rollback();
  if (status != ScratchStatus::ok) {
    rollback_only_ = true;
    throw_status("rollback", status);
  }
  finish_outer(false);
}

void StoreImpl::abandon_outer_tx() noexcept {
  try {
    rollback_outer_tx();
  } catch (...) {
  }
}

void StoreImpl::mark_rollback_only() noexcept {
  if (outer_active_) rollback_only_ = true;
}

void StoreImpl::finish_outer(bool committed) noexcept {
  outer_active_ = false;
  rollback_only_ = false;
  if (!committed) {
    bytes_live_ = baseline_bytes_live_;
    objects_live_ = baseline_objects_live_;
  }
}

TxScope Store::scope() { return TxScope(*this); }

TxScope::TxScope(Store& store) : store_(&store) {
  if (!store.join_active_tx()) owned_ = store.begin();
}

TxScope::~TxScope() {
  if (!finished_ && !owned_) store_->abandon_joined_tx();
  // An unfinished owned handle performs its usual outer rollback.
}

void TxScope::require_open() const {
  if (finished_)
    throw std::logic_error("o1 store: transaction already finished");
}

void TxScope::commit() {
  require_open();
  if (owned_) owned_->commit();
  finished_ = true;
}

void TxScope::rollback() {
  require_open();
  if (owned_) owned_->rollback();
  else store_->abandon_joined_tx();
  finished_ = true;
}

Tx::~Tx() = default;
Store::~Store() = default;

std::unique_ptr<Store> Store::open(const Shape& shape, ScratchBacking& backing) {
  return std::unique_ptr<Store>(new StoreImpl(shape, backing));
}

} // namespace o1
} // namespace detail
} // namespace pineforge
