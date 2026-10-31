// ScratchStore-backed O(1) object-layer tests.
//
// FAIL-BEFORE. frozen2 has no pineforge/detail/o1_store.hpp, no Store target,
// and no object layer that can split a partial write into ScratchStore chunk
// records. The first diagnostic on the frozen tree is therefore the missing
// header. Every expected value below comes from the independent byte vectors or
// deterministic generators in this file.
//
// The contract rows cover lifecycle, exact chunked access, resize preservation,
// ids after delete and rollback, multi-chunk rollback, flattened transactions,
// backing I/O failure, resident-ceiling growth, the std::vector RED control, and
// no heap allocation after the store opens. The growth row executes both
// 100,000 and 1,000,000 deterministic object bars.

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_backing.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "global_allocation_replacement.hpp"

namespace {

using pineforge::detail::FixedMemoryScratchBacking;
using pineforge::detail::ScratchBacking;
using pineforge::detail::ScratchBuffer;
using pineforge::detail::ScratchBytes;
using pineforge::detail::ScratchDurability;
using pineforge::detail::ScratchPageId;
using pineforge::detail::ScratchStatus;
using pineforge::detail::o1::CleanupSkip;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::kMaxChunkBytes;
using pineforge::detail::o1::kMaxLiveObjectsCap;
using pineforge::detail::o1::kMaxResidentPages;
using pineforge::detail::o1::kMaxTxHandles;
using pineforge::detail::o1::Shape;
using pineforge::detail::o1::Stats;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::Tx;
using pineforge::detail::o1::TxPoolExhausted;

#define REQUIRE(condition)                                                        \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #condition, __FILE__, \
                   __LINE__);                                                     \
      std::abort();                                                               \
    }                                                                             \
  } while (false)

constexpr std::uint64_t kFnvOffset = UINT64_C(1469598103934665603);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

std::uint64_t fnv_bytes(const unsigned char* bytes, std::size_t length,
                        std::uint64_t hash = kFnvOffset) {
  for (std::size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    hash ^= (value >> (index * 8)) & UINT64_C(0xff);
    hash *= kFnvPrime;
  }
  return hash;
}

class MemoryBacking {
 public:
  MemoryBacking(std::size_t pages, std::size_t page_size)
      : storage_(pages * page_size),
        memory_(ScratchBuffer{storage_.data(), storage_.size()}, page_size) {}

  MemoryBacking(const MemoryBacking&) = delete;
  MemoryBacking& operator=(const MemoryBacking&) = delete;

  FixedMemoryScratchBacking& get() { return memory_; }

 private:
  std::vector<std::byte> storage_;
  FixedMemoryScratchBacking memory_;
};

class FaultBacking final : public ScratchBacking {
 public:
  explicit FaultBacking(FixedMemoryScratchBacking& inner) noexcept
      : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner) {}

  std::uint64_t page_count() const noexcept override {
    return inner_.page_count();
  }

  void fail_next_write() noexcept { fail_next_write_ = true; }
  bool failed() const noexcept { return failed_; }

  // Borrowed patterns identify data, not page ids or an observed I/O count.
  void fail_read_containing(ScratchBytes pattern,
                            ScratchBytes prior_write) noexcept {
    read_pattern_ = pattern;
    write_pattern_ = prior_write;
    read_armed_ = true;
    read_failed_ = false;
    prior_write_seen_ = false;
  }
  bool read_failed() const noexcept { return read_failed_; }
  bool prior_write_seen() const noexcept { return prior_write_seen_; }

 protected:
  ScratchStatus do_read_page(ScratchPageId id,
                             ScratchBuffer destination) const noexcept override {
    const ScratchStatus status = inner_.read_page(id, destination);
    if (status == ScratchStatus::ok && read_armed_ &&
        contains(ScratchBytes{destination.data, destination.size}, read_pattern_)) {
      read_armed_ = false;
      read_failed_ = true;
      return ScratchStatus::io_error;
    }
    return status;
  }

  ScratchStatus do_write_page(ScratchPageId id,
                              ScratchBytes source) noexcept override {
    if (fail_next_write_) {
      fail_next_write_ = false;
      failed_ = true;
      return ScratchStatus::io_error;
    }
    const ScratchStatus status = inner_.write_page(id, source);
    if (status == ScratchStatus::ok && read_armed_ &&
        contains(source, write_pattern_))
      prior_write_seen_ = true;
    return status;
  }

  ScratchStatus do_sync(ScratchDurability durability) noexcept override {
    return inner_.sync(durability);
  }

 private:
  static bool contains(ScratchBytes bytes, ScratchBytes pattern) noexcept {
    return pattern.size != 0 &&
           std::search(bytes.data, bytes.data + bytes.size,
                       pattern.data, pattern.data + pattern.size) !=
               bytes.data + bytes.size;
  }

  FixedMemoryScratchBacking& inner_;
  bool fail_next_write_ = false;
  bool failed_ = false;
  ScratchBytes read_pattern_{};
  ScratchBytes write_pattern_{};
  mutable bool read_armed_ = false;
  mutable bool read_failed_ = false;
  bool prior_write_seen_ = false;
};

Shape default_shape() {
  Shape shape;
  shape.page_bytes = 4096;
  shape.resident_pages = 8;
  shape.chunk_bytes = 1024;
  shape.journal_slots = 4;
  shape.index_slots = 4;
  return shape;
}

// For the power-of-two resident page counts used here, metadata is one 24-byte
// Slot plus two 4-byte buckets per page: 32 * resident_pages bytes. Round that
// up to pages, then add six work pages and two pages for Core/alignment overhead.
// The object layer also owns the 16-byte header + chunk_bytes buffer.
std::uint64_t ceiling_for(const Shape& shape) {
  const std::uint64_t metadata_bytes = UINT64_C(32) * shape.resident_pages;
  const std::uint64_t metadata_pages =
      (metadata_bytes + shape.page_bytes - 1) / shape.page_bytes;
  return (static_cast<std::uint64_t>(shape.resident_pages) + 6 +
          metadata_pages + 2) * shape.page_bytes + 16 + shape.chunk_bytes;
}

struct XorShift {
  explicit XorShift(std::uint64_t seed) : state(seed == 0 ? 1 : seed) {}

  std::uint64_t next() {
    std::uint64_t value = state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    state = value;
    return value;
  }

  std::uint64_t state;
};

template <typename Callable>
bool throws_invalid_argument(Callable&& callable) {
  try {
    callable();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

template <typename Callable>
bool throws_logic_error(Callable&& callable) {
  try {
    callable();
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

template <typename Callable>
bool throws_runtime_error(Callable&& callable) {
  try {
    callable();
  } catch (const std::runtime_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

void print_stats(const char* label, std::uint64_t bars, const Stats& stats,
                 std::uint64_t ceiling) {
  std::printf(
      "O1STORE %s bars=%llu bytes_live=%llu bytes_resident=%llu ceiling=%llu "
      "pages_spilled=%llu bytes_backing=%llu\n",
      label, static_cast<unsigned long long>(bars),
      static_cast<unsigned long long>(stats.bytes_live),
      static_cast<unsigned long long>(stats.bytes_resident),
      static_cast<unsigned long long>(ceiling),
      static_cast<unsigned long long>(stats.pages_spilled),
      static_cast<unsigned long long>(stats.bytes_backing));
}

void fill_pattern(Store& store, Id id, std::uint64_t seed) {
  const std::uint64_t object_size = store.size(id);
  std::array<unsigned char, 256> buffer{};
  std::uint64_t state = seed | UINT64_C(1);
  std::uint64_t offset = 0;
  while (offset < object_size) {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), object_size - offset));
    for (std::size_t index = 0; index < count; ++index) {
      state = state * UINT64_C(6364136223846793005) +
              UINT64_C(1442695040888963407);
      buffer[index] = static_cast<unsigned char>(state >> 56);
    }
    REQUIRE(store.write(id, offset, buffer.data(), count) == count);
    offset += count;
  }
}

std::vector<unsigned char> read_object(Store& store, Id id) {
  const std::uint64_t object_size = store.size(id);
  std::vector<unsigned char> bytes(static_cast<std::size_t>(object_size));
  if (!bytes.empty())
    REQUIRE(store.read(id, 0, bytes.data(), bytes.size()) == bytes.size());
  return bytes;
}

std::uint64_t object_checksum(Store& store, Id id) {
  const std::uint64_t object_size = store.size(id);
  std::array<unsigned char, 256> buffer{};
  std::uint64_t hash = mix(kFnvOffset, object_size);
  std::uint64_t offset = 0;
  while (offset < object_size) {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), object_size - offset));
    REQUIRE(store.read(id, offset, buffer.data(), count) == count);
    hash = fnv_bytes(buffer.data(), count, hash);
    offset += count;
  }
  return hash;
}

std::uint64_t store_checksum(Store& store, const std::vector<Id>& ids) {
  std::uint64_t hash = kFnvOffset;
  for (Id id : ids) {
    if (!store.alive(id)) {
      hash = mix(hash, UINT64_C(0xdead));
      continue;
    }
    hash = mix(hash, object_checksum(store, id));
  }
  return hash;
}

// ---------------------------------------------------------------- lifecycle

void check_lifecycle() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  shape.chunk_bytes = 16;
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const std::uint64_t ceiling = ceiling_for(shape);
  REQUIRE(store->stats().bytes_resident <= ceiling);
  REQUIRE(store->stats().objects_live == 0);
  REQUIRE(store->stats().max_live_objects == kMaxLiveObjectsCap);

  const Id small = store->create(10, 1);
  REQUIRE(small != pineforge::detail::o1::kNullId);
  REQUIRE(store->alive(small));
  REQUIRE(store->size(small) == 10);
  REQUIRE(store->stats().objects_live == 1);

  const std::array<unsigned char, 10> bytes{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}};
  REQUIRE(store->write(small, 0, bytes.data(), bytes.size()) == bytes.size());
  std::array<unsigned char, 16> echoed{};
  REQUIRE(store->read(small, 0, echoed.data(), bytes.size()) == bytes.size());
  REQUIRE(std::equal(bytes.begin(), bytes.end(), echoed.begin()));
  REQUIRE(store->read(small, 4, echoed.data(), echoed.size()) == 6);
  REQUIRE(echoed[0] == 4 && echoed[5] == 9);
  REQUIRE(store->write(small, 4, bytes.data(), bytes.size()) == 6);
  REQUIRE(store->read(small, 10, echoed.data(), echoed.size()) == 0);
  REQUIRE(store->write(small, 10, bytes.data(), bytes.size()) == 0);

  const Id aligned = store->create(16, 4096);
  const Id default_aligned = store->create(16, 0);
  REQUIRE(aligned != default_aligned && aligned != 0 && default_aligned != 0);
  REQUIRE(throws_invalid_argument([&] { (void)store->create(8, 3); }));
  REQUIRE(store->stats().objects_live == 3);

  const Id empty = store->create(0, 1);
  REQUIRE(store->size(empty) == 0);
  REQUIRE(store->stats().objects_live == 4);
  REQUIRE(store->read(empty, 0, echoed.data(), echoed.size()) == 0);

  const Id growing = store->create(100, 1);
  fill_pattern(*store, growing, 7);
  const std::vector<unsigned char> original = read_object(*store, growing);
  REQUIRE(store->resize(growing, 70000));
  REQUIRE(store->size(growing) == 70000);
  std::vector<unsigned char> prefix(original.size());
  REQUIRE(store->read(growing, 0, prefix.data(), prefix.size()) == prefix.size());
  REQUIRE(prefix == original);
  const std::uint64_t marker = UINT64_C(0xfeedfacecafebeef);
  REQUIRE(store->write(growing, 70000 - 8, &marker, 8) == 8);
  std::uint64_t marker_back = 0;
  REQUIRE(store->read(growing, 70000 - 8, &marker_back, 8) == 8);
  REQUIRE(marker_back == marker);
  REQUIRE(store->stats().bytes_resident <= ceiling);
  REQUIRE(store->stats().bytes_backing > ceiling);

  REQUIRE(store->resize(growing, 50));
  REQUIRE(store->size(growing) == 50);
  std::vector<unsigned char> shrunk(50);
  REQUIRE(store->read(growing, 0, shrunk.data(), shrunk.size()) == shrunk.size());
  REQUIRE(std::equal(shrunk.begin(), shrunk.end(), original.begin()));
  REQUIRE(store->resize(growing, 70000));
  std::vector<unsigned char> regrown(50);
  REQUIRE(store->read(growing, 0, regrown.data(), regrown.size()) == regrown.size());
  REQUIRE(regrown == shrunk);
  const std::vector<unsigned char> regrown_all = read_object(*store, growing);
  REQUIRE(std::all_of(regrown_all.begin() + 50, regrown_all.end(),
                      [](unsigned char byte) { return byte == 0; }));

  const std::uint64_t live_before = store->stats().bytes_live;
  const std::uint64_t objects_before = store->stats().objects_live;
  store->release(small);
  REQUIRE(!store->alive(small));
  REQUIRE(store->stats().bytes_live + 10 == live_before);
  REQUIRE(store->stats().objects_live + 1 == objects_before);
  std::uint8_t dead_byte = 0;
  REQUIRE(throws_invalid_argument(
      [&] { (void)store->read(small, 0, &dead_byte, 1); }));
  REQUIRE(throws_invalid_argument([&] { (void)store->size(small); }));
  REQUIRE(throws_invalid_argument(
      [&] { (void)store->write(small, 0, &dead_byte, 1); }));
  REQUIRE(throws_invalid_argument([&] { (void)store->resize(small, 1); }));
  REQUIRE(throws_invalid_argument([&] { store->release(small); }));
  REQUIRE(store->stats().objects_live + 1 == objects_before);
  const Id after_release = store->create(8, 1);
  REQUIRE(after_release > small);
  REQUIRE(!store->alive(small));
  REQUIRE(store->alive(after_release));

  const Id survivor = store->create(16, 1);
  const std::uint64_t survivor_hash = object_checksum(*store, survivor);
  const std::uint64_t replacement = UINT64_C(0x1112131415161718);
  {
    auto abandoned = store->begin();
    REQUIRE(store->write(survivor, 0, &replacement, 8) == 8);
  }
  REQUIRE(object_checksum(*store, survivor) == survivor_hash);

  Id aborted = 0;
  const std::uint64_t objects_before_abort = store->stats().objects_live;
  {
    auto transaction = store->begin();
    aborted = store->create(32, 1);
    REQUIRE(store->alive(aborted));
    REQUIRE(store->stats().objects_live == objects_before_abort + 1);
    transaction->rollback();
  }
  REQUIRE(aborted != 0 && !store->alive(aborted));
  REQUIRE(store->stats().objects_live == objects_before_abort);
  REQUIRE(throws_invalid_argument([&] { (void)store->size(aborted); }));
  const Id after_abort = store->create(8, 1);
  REQUIRE(after_abort > aborted);
  REQUIRE(store->alive(after_abort));
  REQUIRE(store->stats().bytes_resident <= ceiling);

  std::printf("O1STORE lifecycle backing=memory ok\n");
}

// ---------------------------------------------------------------- shape caps

void check_shape_caps() {
  {
    Shape shape = default_shape();
    shape.chunk_bytes = kMaxChunkBytes;
    MemoryBacking backing(4096, shape.page_bytes);
    std::unique_ptr<Store> store = Store::open(shape, backing.get());
    REQUIRE(store != nullptr);

    std::vector<unsigned char> expected(kMaxChunkBytes);
    for (std::size_t index = 0; index < expected.size(); ++index)
      expected[index] = static_cast<unsigned char>(index);
    const Id id = store->create(expected.size(), 1);
    REQUIRE(store->size(id) == expected.size());
    REQUIRE(store->write(id, 0, expected.data(), expected.size()) ==
            expected.size());
    std::vector<unsigned char> echoed(expected.size());
    REQUIRE(store->read(id, 0, echoed.data(), echoed.size()) == echoed.size());
    REQUIRE(echoed == expected);
  }
  {
    Shape shape = default_shape();
    shape.chunk_bytes = kMaxChunkBytes + 1;
    MemoryBacking backing(4096, shape.page_bytes);
    REQUIRE(throws_invalid_argument(
        [&] { (void)Store::open(shape, backing.get()); }));
  }
  {
    Shape shape = default_shape();
    shape.resident_pages = kMaxResidentPages;
    MemoryBacking backing(4096, shape.page_bytes);
    std::unique_ptr<Store> store = Store::open(shape, backing.get());
    REQUIRE(store != nullptr);
  }
  {
    Shape shape = default_shape();
    shape.resident_pages = kMaxResidentPages + 1;
    MemoryBacking backing(4096, shape.page_bytes);
    REQUIRE(throws_invalid_argument(
        [&] { (void)Store::open(shape, backing.get()); }));
  }
  std::printf("O1STORE shape-caps ok\n");
}

// ---------------------------------------------------------------- chunk boundaries and rollback

void check_chunk_boundaries_and_rollback() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  shape.chunk_bytes = 16;
  MemoryBacking backing(8192, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const Id id = store->create(640, 1);
  std::vector<unsigned char> expected(640, 0);

  const std::array<unsigned char, 3> at_last{{0xa1, 0xa2, 0xa3}};
  REQUIRE(store->write(id, 15, at_last.data(), at_last.size()) ==
          at_last.size());
  std::copy(at_last.begin(), at_last.end(), expected.begin() + 15);

  const std::array<unsigned char, 3> at_chunk{{0xb1, 0xb2, 0xb3}};
  REQUIRE(store->write(id, 16, at_chunk.data(), at_chunk.size()) ==
          at_chunk.size());
  std::copy(at_chunk.begin(), at_chunk.end(), expected.begin() + 16);

  const std::array<unsigned char, 5> unaligned{{0xc1, 0xc2, 0xc3, 0xc4, 0xc5}};
  REQUIRE(store->write(id, 17, unaligned.data(), unaligned.size()) ==
          unaligned.size());
  std::copy(unaligned.begin(), unaligned.end(), expected.begin() + 17);

  std::array<unsigned char, 32> echoed{};
  REQUIRE(store->read(id, 15, echoed.data(), 9) == 9);
  REQUIRE(std::equal(expected.begin() + 15, expected.begin() + 24, echoed.begin()));
  REQUIRE(store->read(id, 640, echoed.data(), 0) == 0);
  REQUIRE(store->write(id, 640, echoed.data(), 0) == 0);
  REQUIRE(store->read(id, 639, echoed.data(), echoed.size()) == 1);
  REQUIRE(echoed[0] == expected.back());
  REQUIRE(store->write(id, 639, at_last.data(), at_last.size()) == 1);
  expected.back() = at_last[0];

  const std::vector<unsigned char> baseline = read_object(*store, id);
  REQUIRE(baseline == expected);
  auto transaction = store->begin();
  for (std::size_t chunk = 0; chunk < 40; ++chunk) {
    const std::array<unsigned char, 4> value{{
        static_cast<unsigned char>(chunk + 1),
        static_cast<unsigned char>(chunk + 2),
        static_cast<unsigned char>(chunk + 3),
        static_cast<unsigned char>(chunk + 4)}};
    const std::uint64_t offset = static_cast<std::uint64_t>(chunk * 16 + 7);
    REQUIRE(store->write(id, offset, value.data(), value.size()) == value.size());
    std::array<unsigned char, 4> staged{};
    REQUIRE(store->read(id, offset, staged.data(), staged.size()) == staged.size());
    REQUIRE(staged == value);
  }
  REQUIRE(read_object(*store, id) != baseline);
  transaction->rollback();
  REQUIRE(store->size(id) == baseline.size());
  REQUIRE(read_object(*store, id) == baseline);

  auto resize_transaction = store->begin();
  REQUIRE(store->resize(id, 64000));
  REQUIRE(store->size(id) == 64000);
  std::vector<unsigned char> resized_prefix(baseline.size());
  REQUIRE(store->read(id, 0, resized_prefix.data(), resized_prefix.size()) ==
          resized_prefix.size());
  REQUIRE(resized_prefix == baseline);
  resize_transaction->rollback();
  REQUIRE(store->size(id) == baseline.size());
  REQUIRE(read_object(*store, id) == baseline);

  std::printf("O1STORE chunk-boundary and multi-chunk rollback ok\n");
}

// ---------------------------------------------------------------- transaction population

void check_transactions() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  shape.chunk_bytes = 32;
  MemoryBacking backing(16384, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());

  std::vector<Id> ids;
  constexpr std::size_t object_count = 100;
  ids.reserve(object_count);
  for (std::size_t index = 0; index < object_count; ++index) {
    const Id id = store->create(128, 16);
    fill_pattern(*store, id, 1000 + index);
    ids.push_back(id);
  }

  const std::uint64_t baseline_hash = store_checksum(*store, ids);
  const Stats baseline_stats = store->stats();
  REQUIRE(baseline_stats.objects_live == object_count);
  std::vector<std::uint64_t> baseline_sizes;
  baseline_sizes.reserve(ids.size());
  for (Id id : ids) baseline_sizes.push_back(store->size(id));

  XorShift rng(UINT64_C(0x5eed5eed));
  auto transaction = store->begin();
  for (int step = 0; step < 2000; ++step) {
    const std::size_t pick = static_cast<std::size_t>(rng.next() % ids.size());
    const std::uint64_t value = rng.next();
    const std::uint64_t offset = (rng.next() % 15) * 8;
    REQUIRE(store->write(ids[pick], offset, &value, 8) == 8);
  }
  REQUIRE(store->resize(ids[3], 512));
  REQUIRE(store->resize(ids[4], 32));

  std::vector<Id> released;
  for (std::size_t index = 0; index < 10; ++index) {
    const Id id = ids[20 + index];
    store->release(id);
    released.push_back(id);
  }
  std::vector<Id> created;
  for (std::size_t index = 0; index < 20; ++index) {
    const Id id = store->create(64, 1);
    fill_pattern(*store, id, 5000 + index);
    created.push_back(id);
  }

  std::vector<Id> mutated = ids;
  mutated.insert(mutated.end(), created.begin(), created.end());
  REQUIRE(store->stats().objects_live ==
          baseline_stats.objects_live - released.size() + created.size());
  REQUIRE(store_checksum(*store, mutated) != baseline_hash);
  transaction->rollback();

  REQUIRE(store_checksum(*store, ids) == baseline_hash);
  REQUIRE(store->stats().bytes_live == baseline_stats.bytes_live);
  REQUIRE(store->stats().objects_live == baseline_stats.objects_live);
  for (std::size_t index = 0; index < ids.size(); ++index)
    REQUIRE(store->size(ids[index]) == baseline_sizes[index]);
  for (Id id : released) REQUIRE(store->alive(id));
  for (Id id : created) REQUIRE(!store->alive(id));
  REQUIRE(store->stats().bytes_backing >= baseline_stats.bytes_backing);

  Id maximum_created = 0;
  for (Id id : created)
    if (id > maximum_created) maximum_created = id;
  const Id after_aborted_population = store->create(8, 1);
  REQUIRE(after_aborted_population > maximum_created);

  const std::size_t committed_index = 11;
  const std::uint64_t committed_value = UINT64_C(0xa5a5a5a55a5a5a5a);
  auto committed = store->begin();
  REQUIRE(store->write(ids[committed_index], 64, &committed_value, 8) == 8);
  REQUIRE(store->resize(ids[committed_index], 1024));
  const Id committed_new = store->create(64, 1);
  fill_pattern(*store, committed_new, 99);
  const std::uint64_t committed_hash = object_checksum(*store, committed_new);
  committed->commit();

  for (int index = 0; index < 100; ++index) {
    const Id churn = store->create(1024, 1);
    std::array<unsigned char, 1024> page{};
    page[0] = static_cast<unsigned char>(index);
    REQUIRE(store->write(churn, 0, page.data(), page.size()) == page.size());
    REQUIRE(store->read(churn, 0, page.data(), page.size()) == page.size());
  }
  std::uint64_t read_back = 0;
  REQUIRE(store->read(ids[committed_index], 64, &read_back, 8) == 8);
  REQUIRE(read_back == committed_value);
  REQUIRE(object_checksum(*store, committed_new) == committed_hash);

  std::printf("O1STORE transactions backing=memory rollback+commit ok\n");
}

// ---------------------------------------------------------------- backing ceiling

void check_never_refuses() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  shape.chunk_bytes = 1024;
  MemoryBacking backing(32768, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const std::uint64_t ceiling = ceiling_for(shape);

  const Id id = store->create(64, 8);
  fill_pattern(*store, id, 31337);
  const std::vector<unsigned char> prefix = read_object(*store, id);
  const std::uint64_t huge = UINT64_C(8) * 1024 * 1024;
  REQUIRE(store->resize(id, huge));
  REQUIRE(store->size(id) == huge);
  const Stats after = store->stats();
  REQUIRE(after.bytes_resident <= ceiling);
  REQUIRE(after.bytes_backing >= huge);
  std::vector<unsigned char> echoed(prefix.size());
  REQUIRE(store->read(id, 0, echoed.data(), echoed.size()) == echoed.size());
  REQUIRE(echoed == prefix);

  const std::uint64_t marker = UINT64_C(0x0123456789abcdef);
  REQUIRE(store->write(id, huge - 8, &marker, 8) == 8);
  std::uint64_t marker_back = 0;
  REQUIRE(store->read(id, huge - 8, &marker_back, 8) == 8);
  REQUIRE(marker_back == marker);

  for (int index = 0; index < 2000; ++index) {
    const Id other = store->create(24, 1);
    REQUIRE(store->write(other, 0, &marker, 8) == 8);
  }
  REQUIRE(store->stats().bytes_resident <= ceiling);

  REQUIRE(store->resize(id, 128));
  std::vector<unsigned char> small(128);
  REQUIRE(store->read(id, 0, small.data(), small.size()) == small.size());
  REQUIRE(std::equal(prefix.begin(), prefix.end(), small.begin()));
  REQUIRE(store->resize(id, huge));
  std::vector<unsigned char> regrown(prefix.size());
  REQUIRE(store->read(id, 0, regrown.data(), regrown.size()) == regrown.size());
  REQUIRE(regrown == prefix);

  std::printf("O1STORE never-refuses backing=memory 8MiB object under %llu bytes\n",
              static_cast<unsigned long long>(ceiling));
}

// ---------------------------------------------------------------- resident RED control

class HeapVectorStore final : public Store {
 public:
  ~HeapVectorStore() override = default;

  Id create(std::uint64_t bytes, std::uint64_t align) override {
    (void)align;
    Object object;
    object.data.assign(static_cast<std::size_t>(bytes), 0);
    object.live = true;
    objects_.push_back(std::move(object));
    resident_ += bytes;
    return static_cast<Id>(objects_.size());
  }

  void release(Id id) override {
    Object& object = find(id);
    object.live = false;
    resident_ -= object.data.size();
  }

  bool alive(Id id) const override {
    if (id == 0 || id > objects_.size()) return false;
    return objects_[static_cast<std::size_t>(id - 1)].live;
  }

  std::uint64_t size(Id id) const override {
    return find(id).data.size();
  }

  bool resize(Id id, std::uint64_t bytes) override {
    Object& object = find(id);
    resident_ -= object.data.size();
    object.data.resize(static_cast<std::size_t>(bytes));
    resident_ += object.data.size();
    return true;
  }

  std::size_t read(Id id, std::uint64_t offset, void* destination,
                   std::size_t length) const override {
    const Object& object = find(id);
    if (length == 0 || offset >= object.data.size()) return 0;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(length, object.data.size() - offset));
    std::memcpy(destination, object.data.data() + offset, count);
    return count;
  }

  std::size_t write(Id id, std::uint64_t offset, const void* source,
                    std::size_t length) override {
    Object& object = find(id);
    if (length == 0 || offset >= object.data.size()) return 0;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(length, object.data.size() - offset));
    std::memcpy(object.data.data() + offset, source, count);
    return count;
  }

  std::unique_ptr<Tx> begin() override {
    return std::unique_ptr<Tx>(new NullTx());
  }

  std::unique_ptr<Tx> begin_cleanup() override {
    return begin();
  }

  bool join_active_tx() override { return false; }
  void abandon_joined_tx() noexcept override {}

  // This deliberately nontransactional resident-memory negative control has
  // NullTx handles: no rollback ever restores a version.
  std::uint64_t rollback_epoch() const noexcept override { return 0; }

  void note_cleanup_skipped(CleanupSkip reason) noexcept override {
    if (reason == CleanupSkip::rollback_only) ++cleanup_skipped_rollback_only_;
    else ++cleanup_failed_;
  }

  Stats stats() const override {
    Stats stats;
    stats.bytes_live = resident_;
    stats.bytes_resident = resident_;
    stats.cleanup_skipped_rollback_only = cleanup_skipped_rollback_only_;
    stats.cleanup_failed = cleanup_failed_;
    return stats;
  }

 private:
  struct Object {
    std::vector<unsigned char> data;
    bool live = false;
  };

  class NullTx final : public Tx {
   public:
    ~NullTx() override = default;
    void commit() override {}
    void rollback() override {}
  };

  Object& find(Id id) {
    if (id == 0 || id > objects_.size())
      throw std::invalid_argument("heap control: dead id");
    Object& object = objects_[static_cast<std::size_t>(id - 1)];
    if (!object.live) throw std::invalid_argument("heap control: dead id");
    return object;
  }

  const Object& find(Id id) const {
    if (id == 0 || id > objects_.size())
      throw std::invalid_argument("heap control: dead id");
    const Object& object = objects_[static_cast<std::size_t>(id - 1)];
    if (!object.live) throw std::invalid_argument("heap control: dead id");
    return object;
  }

  std::vector<Object> objects_;
  std::uint64_t resident_ = 0;
  std::uint64_t cleanup_skipped_rollback_only_ = 0;
  std::uint64_t cleanup_failed_ = 0;
};

bool ceiling_holds(const Store& store, const Shape& shape) {
  return store.stats().bytes_resident <= ceiling_for(shape);
}

void check_control_mutant_goes_red(const Shape& shape, std::uint64_t bars,
                                   std::uint64_t object_bytes) {
  const std::uint64_t ceiling = ceiling_for(shape);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    HeapVectorStore mutant;
    for (std::uint64_t index = 0; index < bars; ++index) {
      const Id id = mutant.create(object_bytes, 8);
      if (id == 0) std::_Exit(3);
    }
    const Stats stats = mutant.stats();
    std::printf(
        "O1STORE control-mutant bars=%llu bytes_resident=%llu ceiling=%llu\n",
        static_cast<unsigned long long>(bars),
        static_cast<unsigned long long>(stats.bytes_resident),
        static_cast<unsigned long long>(ceiling));
    std::fflush(stdout);
    // This is the same predicate required of the real store. The heap control
    // must fail it, so REQUIRE aborts and the parent proves the RED path.
    REQUIRE(ceiling_holds(mutant, shape));
    std::_Exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGABRT);
  std::printf("O1STORE control-mutant resident predicate RED as required\n");
}

void check_growth_gate(std::uint64_t bars, std::size_t backing_pages) {
  Shape shape = default_shape();
  shape.resident_pages = 8;
  shape.chunk_bytes = bars == UINT64_C(100000) ? 128 : 16;
  const std::uint64_t object_bytes = shape.chunk_bytes;
  MemoryBacking backing(backing_pages, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const std::uint64_t ceiling = ceiling_for(shape);
  REQUIRE(object_bytes * bars > ceiling * 100);

  const std::uint64_t first_mark = std::max<std::uint64_t>(1000, bars / 100);
  const std::uint64_t second_mark =
      std::max<std::uint64_t>(first_mark + 1, bars / 10);
  const std::array<std::uint64_t, 3> marks{{first_mark, second_mark, bars}};
  std::array<Stats, 3> checkpoints{};
  std::vector<Id> ids;
  ids.reserve(static_cast<std::size_t>(bars));
  std::size_t next_mark = 0;
  const std::uint64_t salt = UINT64_C(0x9e3779b97f4a7c15);
  constexpr std::uint64_t batch = 10000;
  std::unique_ptr<Tx> transaction;
  // Successful begin() and mutations allocate nothing after open.
  // ids is already reserved.
  // bytes_resident only reports ScratchStore's constant workspace, so the old
  // ceiling/flatness assertions cannot see an object-layer heap leak. This
  // allocation delta catches even a growing per-object vector outside stats().
  constexpr std::size_t allocations_per_begin = 0;
  std::size_t begin_calls = 0;
  const std::size_t allocations_before = global_allocation::allocations;

  for (std::uint64_t index = 0; index < bars; ++index) {
    if (index % batch == 0) {
      transaction = store->begin();
      ++begin_calls;
    }
    const Id id = store->create(object_bytes, 8);
    REQUIRE(id != 0);
    const std::uint64_t value = index ^ salt;
    REQUIRE(store->write(id, 0, &value, sizeof value) == sizeof value);
    ids.push_back(id);
    if (index + 1 == marks[next_mark]) {
      checkpoints[next_mark] = store->stats();
      REQUIRE(checkpoints[next_mark].bytes_live ==
              (index + 1) * object_bytes);
      REQUIRE(checkpoints[next_mark].bytes_resident <= ceiling);
      REQUIRE(global_allocation::allocations - allocations_before ==
              begin_calls * allocations_per_begin);
      print_stats("growth-gate", index + 1, checkpoints[next_mark], ceiling);
      ++next_mark;
    }
    if ((index + 1) % batch == 0) {
      transaction->commit();
      transaction.reset();
    }
  }
  REQUIRE(global_allocation::allocations - allocations_before ==
          begin_calls * allocations_per_begin);
  REQUIRE(next_mark == marks.size());
  REQUIRE(transaction == nullptr);
  REQUIRE(store->alive(ids.back()));

  for (const Stats& stats : checkpoints) {
    REQUIRE(stats.bytes_resident <= ceiling);
    REQUIRE(stats.bytes_resident == checkpoints[0].bytes_resident);
  }
  REQUIRE(checkpoints[1].bytes_backing > checkpoints[0].bytes_backing);
  REQUIRE(checkpoints[2].bytes_backing > checkpoints[1].bytes_backing);
  REQUIRE(checkpoints[2].bytes_backing > ceiling * 100);

  XorShift rng(UINT64_C(0x1234567890abcdef));
  const int probes = bars == UINT64_C(100000) ? 5000 : 20000;
  for (int probe = 0; probe < probes; ++probe) {
    const std::uint64_t index = rng.next() % bars;
    const Id id = ids[static_cast<std::size_t>(index)];
    std::uint64_t value = 0;
    REQUIRE(store->read(id, 0, &value, sizeof value) == sizeof value);
    REQUIRE(value == (index ^ salt));
  }

  const Id last = ids.back();
  store->release(last);
  REQUIRE(!store->alive(last));
  const Id after_release = store->create(object_bytes, 8);
  REQUIRE(after_release > last);
  REQUIRE(!store->alive(last));

  std::printf("O1STORE growth-gate bars=%llu resident flat under 100x growth\n",
              static_cast<unsigned long long>(bars));
}

// ---------------------------------------------------------------- flattened transaction and failure paths

void check_flattened_transactions() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  shape.chunk_bytes = 32;
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const Id id = store->create(64, 8);
  fill_pattern(*store, id, 4242);
  const std::uint64_t baseline = object_checksum(*store, id);

  {
    auto outer = store->begin();
    auto joined = store->begin();
    const std::uint64_t value = UINT64_C(0x0102030405060708);
    REQUIRE(store->write(id, 0, &value, 8) == 8);
    std::uint64_t seen = 0;
    REQUIRE(store->read(id, 0, &seen, 8) == 8);
    REQUIRE(seen == value);
    joined->commit();
    outer->rollback();
  }
  REQUIRE(object_checksum(*store, id) == baseline);

  {
    auto outer = store->begin();
    auto joined = store->begin();
    const std::uint64_t value = UINT64_C(0x1111111111111111);
    REQUIRE(store->write(id, 0, &value, 8) == 8);
    joined->rollback();
    std::uint8_t byte = 0;
    REQUIRE(throws_logic_error([&] { (void)store->write(id, 0, &byte, 1); }));
    REQUIRE(throws_logic_error([&] { (void)store->resize(id, 128); }));
    REQUIRE(throws_logic_error([&] { store->release(id); }));
    REQUIRE(throws_logic_error([&] { (void)store->create(8, 1); }));
    REQUIRE(throws_logic_error([&] {
      auto nested = store->begin();
      (void)nested;
    }));
    REQUIRE(throws_logic_error([&] { outer->commit(); }));
    REQUIRE(store->alive(id));
    REQUIRE(object_checksum(*store, id) == baseline);
  }

  {
    auto outer = store->begin();
    auto joined = store->begin();
    const std::uint64_t value = UINT64_C(0x2222222222222222);
    REQUIRE(store->write(id, 0, &value, 8) == 8);
    joined->rollback();
    outer->rollback();
  }
  REQUIRE(object_checksum(*store, id) == baseline);

  {
    auto outer = store->begin();
    {
      auto joined = store->begin();
      const std::uint64_t value = UINT64_C(0x3333333333333333);
      REQUIRE(store->write(id, 0, &value, 8) == 8);
    }
    std::uint8_t byte = 0;
    REQUIRE(throws_logic_error([&] { (void)store->write(id, 0, &byte, 1); }));
    outer->rollback();
  }
  REQUIRE(object_checksum(*store, id) == baseline);

  {
    auto outer = store->begin();
    {
      auto joined = store->begin();
      const std::uint64_t value = UINT64_C(0x4444444444444444);
      REQUIRE(store->write(id, 0, &value, 8) == 8);
      joined->commit();
    }
    outer->commit();
  }
  REQUIRE(object_checksum(*store, id) != baseline);

  Shape small_shape = default_shape();
  small_shape.resident_pages = 4;
  small_shape.chunk_bytes = 1024;
  MemoryBacking small_backing(64, small_shape.page_bytes);
  std::unique_ptr<Store> small = Store::open(small_shape, small_backing.get());
  const Id victim = small->create(64, 8);
  fill_pattern(*small, victim, 99);
  const std::uint64_t victim_baseline = object_checksum(*small, victim);
  auto poisoned = small->begin();
  REQUIRE(throws_runtime_error(
      [&] { (void)small->resize(victim, UINT64_C(4) * 1024 * 1024); }));
  std::uint8_t byte = 0;
  REQUIRE(throws_logic_error([&] { (void)small->write(victim, 0, &byte, 1); }));
  REQUIRE(throws_logic_error([&] { poisoned->commit(); }));
  REQUIRE(small->alive(victim));
  REQUIRE(object_checksum(*small, victim) == victim_baseline);
  const Id usable = small->create(16, 1);
  REQUIRE(small->alive(usable));
  auto failed_allocation_tx = small->begin();
  bool allocation_failed = false;
  try {
    (void)small->create(UINT64_C(4) * 1024 * 1024, 1);
  } catch (const std::runtime_error&) {
    allocation_failed = true;
  }
  REQUIRE(allocation_failed);
  failed_allocation_tx->rollback();
  const Id after_failed_allocation = small->create(16, 1);
  REQUIRE(after_failed_allocation > usable + 1);

  std::printf("O1STORE flattened transactions and rollback-only state ok\n");
}

void check_io_failure() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  shape.chunk_bytes = 32;
  std::vector<std::byte> storage(4096 * shape.page_bytes);
  FixedMemoryScratchBacking inner(
      ScratchBuffer{storage.data(), storage.size()}, shape.page_bytes);
  FaultBacking backing(inner);
  std::unique_ptr<Store> store = Store::open(shape, backing);
  const Id id = store->create(128, 1);
  fill_pattern(*store, id, 777);
  const std::uint64_t baseline = object_checksum(*store, id);

  auto transaction = store->begin();
  const std::uint64_t value = UINT64_C(0x123456789abcdef0);
  REQUIRE(store->write(id, 0, &value, 8) == 8);
  backing.fail_next_write();
  REQUIRE(throws_runtime_error([&] { transaction->commit(); }));
  REQUIRE(backing.failed());
  transaction->rollback();
  REQUIRE(object_checksum(*store, id) == baseline);
  REQUIRE(store->alive(id));

  std::printf("O1STORE backing I/O failure injection rollback ok\n");
}

void check_read_failure_poisoning() {
  Shape shape = default_shape();
  shape.page_bytes = 512;
  shape.resident_pages = 2;
  shape.chunk_bytes = 4096;
  std::vector<std::byte> storage(4096 * shape.page_bytes);
  FixedMemoryScratchBacking inner(
      ScratchBuffer{storage.data(), storage.size()}, shape.page_bytes);
  FaultBacking backing(inner);
  std::unique_ptr<Store> store = Store::open(shape, backing);

  // Each chunk spans more pages than the cache and has a distinct byte pattern.
  // Keys/headers cannot contain the 64-byte run borrowed from chunk 1 below.
  // Thus the armed read is the value read in the SECOND load_chunk(), after
  // chunk 0's put, rather than a header lookup, key lookup, or write failure.
  std::vector<unsigned char> expected(3 * shape.chunk_bytes);
  std::vector<unsigned char> replacement(expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const std::size_t chunk = index / shape.chunk_bytes;
    expected[index] = static_cast<unsigned char>(0x41 + chunk);
    replacement[index] = static_cast<unsigned char>(0xd1 + chunk);
  }
  const Id id = store->create(expected.size(), 1);
  REQUIRE(store->write(id, 0, expected.data(), expected.size()) ==
          expected.size());
  REQUIRE(read_object(*store, id) == expected);

  auto outer = store->begin();
  backing.fail_read_containing(
      ScratchBytes{reinterpret_cast<const std::byte*>(
                       expected.data() + shape.chunk_bytes), 64},
      ScratchBytes{reinterpret_cast<const std::byte*>(replacement.data()), 64});
  bool write_failed = false;
  try {
    (void)store->write(id, 0, replacement.data(), replacement.size());
  } catch (const std::runtime_error& error) {
    write_failed = true;
    REQUIRE(std::strcmp(error.what(), "o1 store: chunk read: io_error") == 0);
  }
  REQUIRE(write_failed);
  REQUIRE(backing.read_failed());
  REQUIRE(backing.prior_write_seen());
  REQUIRE(throws_logic_error(
      [&] { (void)store->write(id, 0, replacement.data(), 1); }));
  outer->rollback();
  REQUIRE(store->size(id) == expected.size());
  REQUIRE(read_object(*store, id) == expected);

  std::printf("O1STORE second-chunk read failure poisons outer transaction ok\n");
}

struct FullCallerPool {
  explicit FullCallerPool(Store& store, bool finish_at_once = false) {
    for (;;) {
      try {
        auto next = store.begin();
        REQUIRE(count < handles.size());
        if (finish_at_once) next->commit();
        handles[count++] = std::move(next);
      } catch (const TxPoolExhausted& error) {
        REQUIRE(std::strcmp(error.what(),
                            "o1 store: transaction handle pool exhausted") == 0);
        break;
      }
    }
    REQUIRE(count == Store::max_caller_handles());
  }

  void commit_joined() {
    for (std::size_t index = 1; index < count; ++index)
      handles[index]->commit();
  }

  std::array<std::unique_ptr<Tx>, kMaxTxHandles> handles{};
  std::size_t count = 0;
};

void check_finished_begin_exhaustion() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  // Fill until the typed guard fires, retaining finished handles so that no
  // outer transaction is active when the next reservation is rejected.
  FullCallerPool retained(*store, true);
  const Stats before = store->stats();
  const std::size_t allocations_before = global_allocation::allocations;
  const std::size_t refused_before = global_allocation::refused;
  bool exhausted = false;
  global_allocation::refusing = true;
  try {
    (void)store->begin();
  } catch (const TxPoolExhausted& error) {
    exhausted = true;
    REQUIRE(std::strcmp(error.what(),
                        "o1 store: transaction handle pool exhausted") == 0);
  } catch (...) {
    global_allocation::refusing = false;
    throw;
  }
  global_allocation::refusing = false;
  REQUIRE(exhausted);
  REQUIRE(global_allocation::allocations == allocations_before);
  REQUIRE(global_allocation::refused == refused_before);
  const Stats after = store->stats();
  REQUIRE(after.bytes_live == before.bytes_live);
  REQUIRE(after.bytes_resident == before.bytes_resident);
  REQUIRE(after.bytes_backing == before.bytes_backing);
  REQUIRE(after.pages_spilled == before.pages_spilled);
  REQUIRE(after.max_live_handles == before.max_live_handles);
  REQUIRE(after.cleanup_skipped_rollback_only == 0);
  REQUIRE(after.cleanup_failed == 0);

  // A refused handle reservation must leave no hidden outer transaction:
  // this begin must own a real rollback, rather than return a joined scope.
  retained.handles.back().reset();
  auto transaction = store->begin();
  const Id id = store->create(16, 1);
  transaction->rollback();
  REQUIRE(!store->alive(id));
  REQUIRE(store->stats().bytes_live == before.bytes_live);
  std::printf("O1STORE exhausted begin pool leaves no active transaction ok\n");
}

void check_joined_begin_exhaustion() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  FullCallerPool pool(*store);
  const Id id = store->create(16, 1);
  fill_pattern(*store, id, 617);
  const std::uint64_t checksum = object_checksum(*store, id);
  pool.commit_joined();
  const Stats before = store->stats();
  const std::size_t allocations_before = global_allocation::allocations;
  const std::size_t refused_before = global_allocation::refused;
  bool exhausted = false;
  try {
    (void)store->begin();
  } catch (const TxPoolExhausted&) {
    exhausted = true;
  }
  REQUIRE(exhausted);
  REQUIRE(global_allocation::allocations == allocations_before);
  REQUIRE(global_allocation::refused == refused_before);
  REQUIRE(store->stats().bytes_live == before.bytes_live);
  REQUIRE(store->stats().bytes_backing == before.bytes_backing);
  REQUIRE(store->stats().pages_spilled == before.pages_spilled);
  REQUIRE(store->stats().max_live_handles == Store::max_caller_handles());
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  REQUIRE(object_checksum(*store, id) == checksum);

  // A rejected joined begin must neither poison nor finish the live outer
  // transaction, and deleting a finished handle must make its slot reusable.
  pool.handles.back().reset();
  auto replacement = store->begin();
  replacement->commit();
  pool.handles.front()->commit();
  REQUIRE(store->alive(id));
  REQUIRE(object_checksum(*store, id) == checksum);
  std::printf("O1STORE exhausted joined begin preserves outer transaction ok\n");
}

void check_finished_handle_lifetime() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  auto outer = store->begin();
  auto joined = store->begin();
  joined->commit();
  outer->commit();
  REQUIRE(throws_logic_error([&] { outer->commit(); }));
  REQUIRE(throws_logic_error([&] { joined->rollback(); }));

  auto moved = std::move(joined);
  REQUIRE(joined == nullptr);
  joined.reset();
  outer.reset();
  outer.reset();
  {
    auto reused = store->begin();
    reused->rollback();
  }
  store.reset();
  // The surviving handle is finished: its methods only reject reuse, and
  // deletion must return its still-live pool slot without touching the store.
  REQUIRE(throws_logic_error([&] { moved->commit(); }));
  moved.reset();
  moved.reset();
  std::printf("O1STORE moved and finished handles preserve pool lifetime ok\n");
}

void check_cleanup_begin_exhaustion() {
  Shape shape = default_shape();
  shape.chunk_bytes = 32;
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const std::uint64_t baseline_live = store->stats().bytes_live;
  const Id id = store->create(64, 8);
  fill_pattern(*store, id, 4243);
  const std::uint64_t baseline_checksum = object_checksum(*store, id);
  const auto require_caller_pool_exhausted = [&] {
    bool exhausted = false;
    try {
      (void)store->begin();
    } catch (const TxPoolExhausted&) {
      exhausted = true;
    }
    REQUIRE(exhausted);
  };

  const std::size_t allocations_before = global_allocation::allocations;
  const std::size_t refused_before = global_allocation::refused;
  global_allocation::refusing = true;
  try {
    {
      FullCallerPool pool(*store);
      require_caller_pool_exhausted();
      {
        auto cleanup = store->begin_cleanup();
        store->release(id);
        cleanup->commit();
      }
      REQUIRE(!store->alive(id));
      REQUIRE(store->stats().bytes_live == baseline_live);
      require_caller_pool_exhausted();
      pool.commit_joined();
      // A cleanup commit joins the outer transaction; its release must undo.
      pool.handles.front()->rollback();
    }
    REQUIRE(store->alive(id));
    REQUIRE(object_checksum(*store, id) == baseline_checksum);
    REQUIRE(store->stats().bytes_live == baseline_live + 64);

    {
      FullCallerPool pool(*store);
      require_caller_pool_exhausted();
      {
        auto cleanup = store->begin_cleanup();
        store->release(id);
        cleanup->commit();
      }
      pool.commit_joined();
      pool.handles.front()->commit();
      REQUIRE(!store->alive(id));
      REQUIRE(store->stats().bytes_live == baseline_live);

      // All caller handles are now finished but still occupy their slots.
      // Cleanup must also start a real outer transaction in this state.
      require_caller_pool_exhausted();
      const Id next = store->create(64, 8);
      fill_pattern(*store, next, 4243);
      {
        auto cleanup = store->begin_cleanup();
        store->release(next);
        cleanup->rollback();
      }
      REQUIRE(store->alive(next));
      REQUIRE(object_checksum(*store, next) == baseline_checksum);
      REQUIRE(store->stats().bytes_live == baseline_live + 64);
      auto cleanup = store->begin_cleanup();
      store->release(next);
      cleanup->commit();
      REQUIRE(!store->alive(next));
      REQUIRE(store->stats().bytes_live == baseline_live);
      require_caller_pool_exhausted();
      REQUIRE(store->stats().max_live_handles == Store::max_caller_handles());
      REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
      REQUIRE(store->stats().cleanup_failed == 0);
      // All finished caller and cleanup handles may outlive the store and return
      // slots without accessing the destroyed store.
      store.reset();
      cleanup.reset();
    }
  } catch (...) {
    global_allocation::refusing = false;
    throw;
  }
  global_allocation::refusing = false;
  REQUIRE(global_allocation::allocations == allocations_before);
  REQUIRE(global_allocation::refused == refused_before);
  std::printf("O1STORE cleanup releases with active and finished caller pools full ok\n");
}

void check_handle_high_water() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  auto store = Store::open(shape, backing.get());
  REQUIRE(store->stats().max_live_handles == 0);
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  {
    auto outer = store->begin();
    REQUIRE(store->stats().max_live_handles == 1);
    auto joined = store->begin();
    joined->commit();
    REQUIRE(store->stats().max_live_handles == 2);
    auto finished = store->begin();
    finished->commit();
    REQUIRE(store->stats().max_live_handles == 3);
    {
      auto cleanup = store->begin_cleanup();
      const Id id = store->create(8, 8);
      store->release(id);
      cleanup->commit();
    }
    REQUIRE(store->stats().max_live_handles == 3);
    joined.reset();
    {
      auto replacement = store->begin();
      replacement->commit();
    }
    REQUIRE(store->stats().max_live_handles == 3);
    outer->rollback();
  }
  {
    auto next = store->begin();
    next->commit();
  }
  REQUIRE(store->stats().max_live_handles == 3);
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  std::printf("O1STORE caller high-water includes retained finished handles ok\n");
}

void check_cleanup_counters() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  auto store = Store::open(shape, backing.get());
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  std::uint64_t rollback_skips = 0;
  std::uint64_t failures = 0;
  {
    auto outer = store->begin();
    store->note_cleanup_skipped(CleanupSkip::rollback_only);
    ++rollback_skips;
    REQUIRE(store->stats().cleanup_skipped_rollback_only == rollback_skips);
    REQUIRE(store->stats().cleanup_failed == failures);
    store->note_cleanup_skipped(CleanupSkip::failed);
    ++failures;
    REQUIRE(store->stats().cleanup_skipped_rollback_only == rollback_skips);
    REQUIRE(store->stats().cleanup_failed == failures);
    outer->rollback();
  }
  REQUIRE(store->stats().cleanup_skipped_rollback_only == rollback_skips);
  REQUIRE(store->stats().cleanup_failed == failures);
  store->note_cleanup_skipped(CleanupSkip::failed);
  ++failures;
  REQUIRE(store->stats().cleanup_skipped_rollback_only == rollback_skips);
  REQUIRE(store->stats().cleanup_failed == failures);
  store->note_cleanup_skipped(CleanupSkip::rollback_only);
  ++rollback_skips;
  REQUIRE(store->stats().cleanup_skipped_rollback_only == rollback_skips);
  REQUIRE(store->stats().cleanup_failed == failures);
  std::printf("O1STORE separate cleanup counters survive rollback ok\n");
}

void check_operation_scopes() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  auto store = Store::open(shape, backing.get());
  const Id id = store->create(8, 8);
  fill_pattern(*store, id, 718);
  const auto checksum = object_checksum(*store, id);
  {
    auto owned = store->scope();
    store->release(id);
    owned.rollback();
    REQUIRE(throws_logic_error([&] { owned.commit(); }));
  }
  REQUIRE(store->alive(id));
  {
    auto abandoned = store->scope();
    store->release(id);
  }
  REQUIRE(store->alive(id));
  {
    FullCallerPool pool(*store);
    const auto before = store->stats();
    const auto allocations_before = global_allocation::allocations;
    const auto refused_before = global_allocation::refused;
    global_allocation::refusing = true;
    try {
      auto joined = store->scope();
      auto child = store->scope();
      store->release(id);
      child.commit();
      joined.commit();
    } catch (...) {
      global_allocation::refusing = false;
      throw;
    }
    global_allocation::refusing = false;
    REQUIRE(global_allocation::allocations == allocations_before);
    REQUIRE(global_allocation::refused == refused_before);
    REQUIRE(store->stats().max_live_handles == before.max_live_handles);
    pool.commit_joined();
    pool.handles.front()->rollback();
  }
  REQUIRE(store->alive(id));
  REQUIRE(object_checksum(*store, id) == checksum);
  {
    auto outer = store->begin();
    auto joined = store->scope();
    store->release(id);
    joined.rollback();
    REQUIRE(!store->alive(id)); // Joined rollback has no savepoint.
    REQUIRE(throws_logic_error([&] { (void)store->scope(); }));
    REQUIRE(throws_logic_error([&] { outer->commit(); }));
  }
  REQUIRE(store->alive(id));
  {
    auto outer = store->begin();
    try {
      auto joined = store->scope();
      store->release(id);
      throw std::runtime_error("operation callback failed");
    } catch (const std::runtime_error&) {
    }
    REQUIRE(throws_logic_error([&] { outer->commit(); }));
  }
  REQUIRE(store->alive(id));
  REQUIRE(object_checksum(*store, id) == checksum);
  {
    auto owned = store->scope();
    store->release(id);
    owned.commit();
  }
  REQUIRE(!store->alive(id));
  REQUIRE(store->stats().bytes_live == 0);
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  std::printf("O1STORE operation scopes join without handles and preserve rollback ok\n");
}

void check_joined_cleanup_failure() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  auto store = Store::open(shape, backing.get());
  const Id id = store->create(8, 8);
  auto outer = store->begin();
  try {
    auto cleanup = store->begin_cleanup();
    store->release(id);
    throw std::runtime_error("cleanup callback failed");
  } catch (const std::runtime_error&) {
  }
  REQUIRE(!store->alive(id));
  REQUIRE(throws_logic_error([&] { (void)store->begin(); }));
  REQUIRE(throws_logic_error([&] { outer->commit(); }));
  REQUIRE(store->alive(id));
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  std::printf("O1STORE unfinished joined cleanup poisons its outer transaction ok\n");
}

// ---------------------------------------------------------------- no heap after open

void check_begin_no_heap_after_open() {
  Shape shape = default_shape();
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const Id id = store->create(sizeof(std::uint64_t), alignof(std::uint64_t));
  constexpr std::size_t cycles = 128;
  const std::size_t allocations_before = global_allocation::allocations;
  const std::size_t refused_before = global_allocation::refused;
  for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
    const std::uint64_t committed = cycle * 2 + 1;
    const std::uint64_t discarded = committed + 1;
    std::uint64_t echoed = 0;
    {
      auto transaction = store->begin();
      REQUIRE(store->write(id, 0, &committed, sizeof committed) ==
              sizeof committed);
      transaction->commit();
    }
    REQUIRE(store->read(id, 0, &echoed, sizeof echoed) == sizeof echoed);
    REQUIRE(echoed == committed);
    {
      auto transaction = store->begin();
      REQUIRE(store->write(id, 0, &discarded, sizeof discarded) ==
              sizeof discarded);
      transaction->rollback();
    }
    REQUIRE(store->read(id, 0, &echoed, sizeof echoed) == sizeof echoed);
    REQUIRE(echoed == committed);
    {
      auto outer = store->begin();
      auto joined = store->begin();
      REQUIRE(store->write(id, 0, &discarded, sizeof discarded) ==
              sizeof discarded);
      joined->commit();
      outer->commit();
    }
    REQUIRE(store->read(id, 0, &echoed, sizeof echoed) == sizeof echoed);
    REQUIRE(echoed == discarded);
    {
      auto outer = store->begin();
      auto joined = store->begin();
      REQUIRE(store->write(id, 0, &committed, sizeof committed) ==
              sizeof committed);
      joined->commit();
      outer->rollback();
    }
    REQUIRE(store->read(id, 0, &echoed, sizeof echoed) == sizeof echoed);
    REQUIRE(echoed == discarded);
  }
  const std::size_t allocations =
      global_allocation::allocations - allocations_before;
  std::printf("O1STORE begin-no-heap cycles=%zu begins=%zu allocations=%zu\n",
              cycles, cycles * 6, allocations);
  std::fflush(stdout);
  REQUIRE(global_allocation::refused == refused_before);
  REQUIRE(allocations == 0);
}

void check_no_heap_after_open() {
  Shape shape = default_shape();
  shape.resident_pages = 4;
  MemoryBacking backing(4096, shape.page_bytes);
  std::unique_ptr<Store> store = Store::open(shape, backing.get());
  const std::uint64_t resident_before = store->stats().bytes_resident;
  std::array<unsigned char, 64> buffer{};
  for (std::size_t index = 0; index < buffer.size(); ++index)
    buffer[index] = static_cast<unsigned char>(index);
  std::array<unsigned char, 64> echoed{};

  std::size_t allocations_before = global_allocation::allocations;
  std::size_t refused_before = global_allocation::refused;
  bool held = true;
  global_allocation::refusing = true;
  try {
    const Id id = store->create(4096, 8);
    held = held && id != 0;
    held = held && store->write(id, 0, buffer.data(), buffer.size()) ==
                       buffer.size();
    held = held && store->read(id, 0, echoed.data(), echoed.size()) ==
                       echoed.size();
    held = held && echoed == buffer;
    held = held && store->resize(id, 200000);
    held = held && store->write(id, 199999, buffer.data(), 1) == 1;
    held = held && store->resize(id, 100);
    held = held && store->size(id) == 100;
    store->release(id);
    held = held && !store->alive(id);
    held = held && store->stats().bytes_resident == resident_before;
  } catch (...) {
    held = false;
  }
  global_allocation::refusing = false;
  REQUIRE(held);
  REQUIRE(global_allocation::refused == refused_before);
  REQUIRE(global_allocation::allocations == allocations_before);

  const Id committed = store->create(32, 8);
  auto committed_tx = store->begin();
  allocations_before = global_allocation::allocations;
  refused_before = global_allocation::refused;
  held = true;
  global_allocation::refusing = true;
  try {
    held = held && store->write(committed, 0, buffer.data(), 32) == 32;
    held = held && store->resize(committed, 100000);
    committed_tx->commit();
    held = held && store->alive(committed);
    held = held && store->stats().bytes_resident == resident_before;
  } catch (const std::exception&) {
    held = false;
  } catch (...) {
    held = false;
  }
  global_allocation::refusing = false;
  REQUIRE(held);
  REQUIRE(global_allocation::refused == refused_before);
  REQUIRE(global_allocation::allocations == allocations_before);

  // Created inside the transaction, so its rollback discards the object.
  auto discarded_tx = store->begin();
  const Id discarded = store->create(48, 8);
  allocations_before = global_allocation::allocations;
  refused_before = global_allocation::refused;
  held = true;
  global_allocation::refusing = true;
  try {
    // The object is 48 bytes: a write of 64 is clipped to the object size.
    held = held && store->write(discarded, 0, buffer.data(), buffer.size()) == 48;
    discarded_tx->rollback();
    held = held && !store->alive(discarded);
    held = held && store->stats().bytes_resident == resident_before;
  } catch (...) {
    held = false;
  }
  global_allocation::refusing = false;
  REQUIRE(held);
  REQUIRE(global_allocation::refused == refused_before);
  REQUIRE(global_allocation::allocations == allocations_before);
  REQUIRE(store->stats().bytes_resident == resident_before);

  allocations_before = global_allocation::allocations;
  refused_before = global_allocation::refused;
  held = true;
  // Release the two finished handles before reserving another nested pair.
  committed_tx.reset();
  discarded_tx.reset();
  global_allocation::refusing = true;
  try {
    auto outer = store->begin();
    auto joined = store->begin();
    joined->commit();
    outer->rollback();
  } catch (...) {
    held = false;
  }
  global_allocation::refusing = false;
  REQUIRE(held);
  REQUIRE(global_allocation::refused == refused_before);
  REQUIRE(global_allocation::allocations == allocations_before);

  std::printf("O1STORE no-heap-after-open backing=memory ok\n");
}

void check_alive_failure_is_observable() {
  Shape shape = default_shape();
  shape.page_bytes = 512;
  shape.resident_pages = 2;
  shape.chunk_bytes = 16;
  MemoryBacking memory(16384, shape.page_bytes);
  FaultBacking backing(memory.get());
  auto store = Store::open(shape, backing);
  // More raw 16-byte object/chunk keys than the entire resident workspace can
  // hold: an alive() sweep must read a cold page, irrespective of cache order.
  constexpr std::uint64_t key_bytes = 2 * sizeof(std::uint64_t);
  const std::uint64_t count = ceiling_for(shape) / key_bytes + 1;
  std::vector<Id> ids;
  ids.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    const Id id = store->create(sizeof index, alignof(std::uint64_t));
    REQUIRE(store->write(id, 0, &index, sizeof index) == sizeof index);
    ids.push_back(id);
  }
  const auto baseline = store->stats().bytes_live;
  auto outer = store->begin();
  // Every stored key in this population contains zero in its high id bytes.
  // The existing backing hook fails the first cold page containing that byte.
  const std::byte zero{};
  backing.fail_read_containing(ScratchBytes{&zero, 1}, ScratchBytes{&zero, 1});
  bool rejected = false;
  for (const Id id : ids) {
    try {
      REQUIRE(store->alive(id)); // A backing error must never mean "dead".
    } catch (const std::runtime_error& error) {
      REQUIRE(std::strcmp(error.what(), "o1 store: alive lookup: io_error") == 0);
      rejected = true;
      break;
    }
  }
  REQUIRE(rejected);
  REQUIRE(backing.read_failed());
  REQUIRE(throws_logic_error([&] { (void)store->begin(); }));
  outer->rollback();
  REQUIRE(store->stats().bytes_live == baseline);
  for (std::size_t index = 0; index < ids.size(); ++index) {
    std::uint64_t value = 0;
    REQUIRE(store->alive(ids[index]));
    REQUIRE(store->read(ids[index], 0, &value, sizeof value) == sizeof value);
    REQUIRE(value == index);
  }
  REQUIRE(store->stats().cleanup_skipped_rollback_only == 0);
  REQUIRE(store->stats().cleanup_failed == 0);
  std::printf("O1STORE liveness lookup failures propagate and poison the outer ok\n");
}

}  // namespace

int main() {
  try {
    check_lifecycle();
    check_shape_caps();
    check_chunk_boundaries_and_rollback();
    check_transactions();
    check_never_refuses();
    check_begin_no_heap_after_open();
    check_growth_gate(UINT64_C(100000), 32768);
    check_growth_gate(UINT64_C(1000000), 131072);
    Shape control_shape = default_shape();
    control_shape.resident_pages = 8;
    control_shape.chunk_bytes = 16;
    check_control_mutant_goes_red(control_shape, UINT64_C(1000000), 16);
    check_flattened_transactions();
    check_io_failure();
    check_read_failure_poisoning();
    check_finished_begin_exhaustion();
    check_joined_begin_exhaustion();
    check_finished_handle_lifetime();
    check_cleanup_begin_exhaustion();
    check_handle_high_water();
    check_cleanup_counters();
    check_operation_scopes();
    check_joined_cleanup_failure();
    check_no_heap_after_open();
    check_alive_failure_is_observable();
    std::printf("test_o1_store: all groups passed\n");
    return 0;
  } catch (const std::exception& error) {
    global_allocation::refusing = false;
    std::fprintf(stderr, "test_o1_store failed: %s\n", error.what());
    return 1;
  }
}
