// Live-object limits count the current transaction's objects, not lifetime ids.
// Rows cover exact typed refusals, rollback, pool preservation and id reuse.
#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using pineforge::RunFailureCode;
using pineforge::classify_run_failure;
using pineforge::detail::FixedMemoryScratchBacking;
using pineforge::detail::ScratchBuffer;
using pineforge::detail::ScratchCapacityError;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::kMaxLiveObjectsCap;
using pineforge::detail::o1::kMaxTxHandles;
using pineforge::detail::o1::kNullId;
using pineforge::detail::o1::Shape;
using pineforge::detail::o1::Stats;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::Tx;
using pineforge::detail::scratch_object_limit_message;

#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #condition, __FILE__, \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

Shape small_shape() {
  Shape shape;
  shape.resident_pages = 4;
  shape.chunk_bytes = 16;
  shape.max_live_objects = 8;
  return shape;
}

struct Fixture {
  explicit Fixture(const Shape& shape)
      : storage(static_cast<std::size_t>(shape.page_bytes) * 1024),
        backing(ScratchBuffer{storage.data(), storage.size()}, shape.page_bytes),
        store(Store::open(shape, backing)) {}

  std::vector<std::byte> storage;
  FixedMemoryScratchBacking backing;
  std::unique_ptr<Store> store;
};

// Independent oracle: insert separators into a complete decimal string rather
// than use the production formatter or message builder to predict their output.
std::string grouped(std::uint64_t value) {
  std::string text = std::to_string(value);
  for (std::size_t end = text.size(); end > 3; end -= 3)
    text.insert(end - 3, 1, ',');
  return text;
}

std::string expected_message(std::uint64_t limit) {
  return "more than " + grouped(limit) + " store objects live at once; " + grouped(limit) +
      " is the most allowed in this store; attempted live count " + grouped(limit + 1) +
      ", approved maximum " + grouped(kMaxLiveObjectsCap);
}

Id create_admitted(Store& store, std::uint64_t bytes) {
  Id admitted = kNullId;
  try {
    admitted = store.create(bytes, 1);
  } catch (const ScratchCapacityError&) {
    // An early refusal must fail an admission assertion, not count as success.
  }
  REQUIRE(admitted != kNullId);
  return admitted;
}

void require_refusal(Store& store, std::uint64_t limit, std::uint64_t bytes) {
  bool refused = false;
  try {
    (void)store.create(bytes, 1);
  } catch (const ScratchCapacityError& error) {
    refused = true;
    const auto failure = classify_run_failure(error);
    REQUIRE(failure.code == RunFailureCode::scratch_capacity_exceeded);
    REQUIRE(std::strcmp(pineforge::run_failure_code_name(failure.code),
                        "scratch_capacity_exceeded") == 0);
    REQUIRE(failure.args != nullptr);
    REQUIRE(*failure.args == "{\"resource\":\"cohort\"}");
    REQUIRE(std::string(error.what()) == expected_message(limit));
  }
  REQUIRE(refused);
}

void require_unchanged(const Stats& before, const Stats& after) {
  REQUIRE(after.bytes_live == before.bytes_live);
  REQUIRE(after.objects_live == before.objects_live);
  REQUIRE(after.bytes_resident == before.bytes_resident);
  REQUIRE(after.pages_spilled == before.pages_spilled);
  REQUIRE(after.bytes_backing == before.bytes_backing);
  REQUIRE(after.max_live_handles == before.max_live_handles);
  REQUIRE(after.cleanup_skipped_rollback_only == before.cleanup_skipped_rollback_only);
  REQUIRE(after.cleanup_failed == before.cleanup_failed);
  REQUIRE(after.max_live_objects == before.max_live_objects);
}

void require_invalid_release(Store& store, Id id) {
  bool invalid = false;
  try {
    store.release(id);
  } catch (const std::invalid_argument&) {
    invalid = true;
  }
  REQUIRE(invalid);
}

void check_cap_and_next_id() {
  const Shape shape = small_shape();
  const std::uint64_t limit = shape.max_live_objects;
  Fixture fixture(shape);
  Store& store = *fixture.store;
  REQUIRE(store.stats().max_live_objects == limit);
  REQUIRE(store.stats().objects_live == 0);

  std::vector<Id> ids;
  std::uint64_t bytes_live = 0;
  for (std::uint64_t index = 0; index < limit; ++index) {
    // Include a zero-byte object: headers alone still count towards the limit.
    const Id id = create_admitted(store, index);
    REQUIRE(store.alive(id));
    REQUIRE(store.size(id) == index);
    ids.push_back(id);
    bytes_live += index;
    REQUIRE(store.stats().objects_live == index + 1);
    REQUIRE(store.stats().bytes_live == bytes_live);
  }
  const unsigned char marker = 0xa5;
  REQUIRE(store.write(ids.back(), 0, &marker, 1) == 1);
  const Stats before = store.stats();
  REQUIRE(before.objects_live == limit);
  const Id next = ids.back() + 1;
  require_refusal(store, limit, shape.chunk_bytes * 3 + 1);
  require_unchanged(before, store.stats());
  require_refusal(store, limit, 0);
  require_unchanged(before, store.stats());
  REQUIRE(!store.alive(next)); // No half-created header.
  for (std::size_t index = 0; index < ids.size(); ++index)
    REQUIRE(store.size(ids[index]) == index);
  unsigned char echoed = 0;
  REQUIRE(store.read(ids.back(), 0, &echoed, 1) == 1);
  REQUIRE(echoed == marker);

  store.release(ids.front());
  REQUIRE(store.stats().objects_live == limit - 1);
  REQUIRE(store.stats().bytes_live == before.bytes_live);
  const Id replacement = create_admitted(store, 1);
  REQUIRE(replacement == next); // Neither refusal consumed an id.
  REQUIRE(store.stats().objects_live == limit);
  REQUIRE(store.stats().bytes_live == before.bytes_live + 1);
  std::printf("O1STORE idcap CAP/CAP+1 and next-id preservation ok\n");
}

void check_default_and_normalization() {
  const Shape defaults;
  REQUIRE(defaults.max_live_objects == kMaxLiveObjectsCap);
  {
    Fixture fixture(defaults);
    REQUIRE(fixture.store->stats().max_live_objects == kMaxLiveObjectsCap);
    REQUIRE(fixture.store->stats().objects_live == 0);
  }
  const std::array<std::uint64_t, 3> inputs{{
      0, kMaxLiveObjectsCap + 1, std::numeric_limits<std::uint64_t>::max()}};
  for (const auto input : inputs) {
    Shape shape = small_shape();
    shape.max_live_objects = input;
    Fixture fixture(shape);
    REQUIRE(fixture.store->stats().max_live_objects == kMaxLiveObjectsCap);
    REQUIRE(fixture.store->stats().objects_live == 0);
    (void)create_admitted(*fixture.store, 0);
    REQUIRE(fixture.store->stats().objects_live == 1);
  }
  std::printf("O1STORE idcap default, zero and above-cap normalization ok\n");
}

void check_documented_text() {
  REQUIRE(scratch_object_limit_message(UINT64_C(65536),
                                      UINT64_C(65537),
                                      kMaxLiveObjectsCap) ==
      "more than 65,536 store objects live at once; 65,536 is the most allowed in this store; "
      "attempted live count 65,537, approved maximum 1,048,576");
  // The default cap, as a user sees it.
  REQUIRE(scratch_object_limit_message(kMaxLiveObjectsCap,
                                      kMaxLiveObjectsCap + 1,
                                      kMaxLiveObjectsCap) ==
      "more than 1,048,576 store objects live at once; 1,048,576 is the most allowed in this store; "
      "attempted live count 1,048,577, approved maximum 1,048,576");
  // An explicit smaller limit: the sentence must not claim the cap as its own limit.
  REQUIRE(scratch_object_limit_message(UINT64_C(1000), UINT64_C(1001), kMaxLiveObjectsCap) ==
      "more than 1,000 store objects live at once; 1,000 is the most allowed in this store; "
      "attempted live count 1,001, approved maximum 1,048,576");
  std::printf("O1STORE idcap exact documented text ok\n");
}

void check_rollback() {
  const Shape shape = small_shape();
  const std::uint64_t limit = shape.max_live_objects;
  Fixture fixture(shape);
  Store& store = *fixture.store;
  const Stats baseline = store.stats();
  std::vector<Id> aborted;
  {
    auto outer = store.begin();
    auto joined = store.begin();
    for (std::uint64_t index = 0; index < limit; ++index)
      aborted.push_back(create_admitted(store, index + 1));
    REQUIRE(store.stats().objects_live == limit);
    require_refusal(store, limit, 1);
    joined->commit();
    REQUIRE(store.stats().objects_live == limit);
    outer->rollback();
  }
  REQUIRE(store.stats().objects_live == baseline.objects_live);
  REQUIRE(store.stats().bytes_live == baseline.bytes_live);
  for (const Id id : aborted) REQUIRE(!store.alive(id));

  std::vector<Id> ids;
  for (std::uint64_t index = 0; index < limit; ++index)
    ids.push_back(create_admitted(store, index + 1));
  REQUIRE(ids.front() == aborted.back() + 1); // Rollback still burns issued ids.
  REQUIRE(store.stats().objects_live == limit);
  const Stats full = store.stats();
  {
    auto outer = store.begin();
    auto joined = store.begin();
    store.release(ids.front());
    REQUIRE(store.stats().objects_live == limit - 1);
    REQUIRE(!store.alive(ids.front()));
    joined->commit();
    REQUIRE(store.stats().objects_live == limit - 1);
    outer->rollback();
  }
  REQUIRE(store.stats().objects_live == full.objects_live);
  REQUIRE(store.stats().bytes_live == full.bytes_live);
  REQUIRE(store.alive(ids.front()));
  require_refusal(store, limit, 1);
  {
    auto outer = store.begin();
    store.release(ids.front());
    outer->commit();
  }
  REQUIRE(store.stats().objects_live == limit - 1);
  REQUIRE(!store.alive(ids.front()));
  const Id replacement = create_admitted(store, 1);
  REQUIRE(replacement == ids.back() + 1);
  REQUIRE(store.stats().objects_live == limit);
  std::printf("O1STORE idcap create/destroy rollback and committed destroy ok\n");
}

void check_flattened_and_abandoned_rollback() {
  const Shape shape = small_shape();
  Fixture fixture(shape);
  Store& store = *fixture.store;
  const Id original = create_admitted(store, 7);
  const Stats baseline = store.stats();
  Id first = kNullId;
  Id second = kNullId;
  {
    auto outer = store.begin();
    auto joined = store.begin();
    store.release(original);
    first = create_admitted(store, 11);
    second = create_admitted(store, 13);
    const Stats staged = store.stats();
    REQUIRE(staged.objects_live == baseline.objects_live + 1);
    joined->rollback();
    // Joined rollback has no savepoint; only the outer end restores the count.
    REQUIRE(store.stats().objects_live == staged.objects_live);
    bool rollback_only = false;
    try {
      outer->commit();
    } catch (const std::logic_error&) {
      rollback_only = true;
    }
    REQUIRE(rollback_only);
  }
  REQUIRE(store.stats().objects_live == baseline.objects_live);
  REQUIRE(store.stats().bytes_live == baseline.bytes_live);
  REQUIRE(store.alive(original));
  REQUIRE(!store.alive(first));
  REQUIRE(!store.alive(second));
  {
    auto abandoned = store.begin();
    store.release(original);
    REQUIRE(store.stats().objects_live == baseline.objects_live - 1);
  }
  REQUIRE(store.stats().objects_live == baseline.objects_live);
  REQUIRE(store.stats().bytes_live == baseline.bytes_live);
  REQUIRE(store.alive(original));
  std::printf("O1STORE idcap flattened and abandoned rollback ok\n");
}

void check_joined_refusal_preserves_pool() {
  const Shape shape = small_shape();
  const std::uint64_t limit = shape.max_live_objects;
  Fixture fixture(shape);
  Store& store = *fixture.store;
  Id first = kNullId;
  Id last = kNullId;
  {
    auto outer = store.begin();
    auto joined = store.begin();
    for (std::uint64_t index = 0; index < limit; ++index) {
      last = create_admitted(store, 1);
      if (index == 0) first = last;
    }
    const Stats before = store.stats();
    REQUIRE(before.max_live_handles == 2);
    require_refusal(store, limit, shape.chunk_bytes * 3 + 1);
    require_unchanged(before, store.stats());
    REQUIRE(!store.alive(last + 1));

    // Every remaining caller slot and the reserved cleanup slot still work.
    std::array<std::unique_ptr<Tx>, kMaxTxHandles - 2> remaining;
    for (auto& handle : remaining) handle = store.begin();
    auto cleanup = store.begin_cleanup();
    cleanup->commit();
    cleanup.reset();
    store.release(first);
    REQUIRE(store.stats().objects_live == limit - 1);
    REQUIRE(create_admitted(store, 1) == last + 1);
    REQUIRE(store.stats().objects_live == limit);
    for (auto& handle : remaining) handle->commit();
    joined->commit();
    outer->commit(); // A capacity refusal must not poison the outer.
  }
  REQUIRE(store.stats().objects_live == limit);
  REQUIRE(store.stats().bytes_live == limit);
  REQUIRE(store.stats().max_live_handles == kMaxTxHandles);
  std::printf("O1STORE idcap joined refusal leaves pool and transaction usable ok\n");
}

void check_destroy_unknown() {
  const Shape shape = small_shape();
  Fixture fixture(shape);
  Store& store = *fixture.store;
  const Id original = create_admitted(store, 7);
  const Id unknown = original + shape.max_live_objects + 1;
  REQUIRE(!store.alive(unknown));
  const Stats before = store.stats();
  require_invalid_release(store, unknown);
  REQUIRE(store.stats().objects_live == before.objects_live);
  require_unchanged(before, store.stats());
  require_invalid_release(store, kNullId);
  require_unchanged(before, store.stats());
  store.release(original);
  REQUIRE(store.stats().objects_live == before.objects_live - 1);
  const Stats destroyed = store.stats();
  require_invalid_release(store, original);
  REQUIRE(store.stats().objects_live == destroyed.objects_live);
  require_unchanged(destroyed, store.stats());
  std::printf("O1STORE idcap unknown, null and double destroy preserve count ok\n");
}

void check_reuse() {
  const Shape shape = small_shape();
  const std::uint64_t limit = shape.max_live_objects;
  Fixture fixture(shape);
  Store& store = *fixture.store;
  Id previous = kNullId;
  for (std::uint64_t step = 0; step < 10 * limit; ++step) {
    const Id current = create_admitted(store, step % (shape.chunk_bytes + 1));
    REQUIRE(current > previous);
    REQUIRE(store.stats().objects_live == 1);
    store.release(current);
    REQUIRE(store.stats().objects_live == 0);
    REQUIRE(store.stats().bytes_live == 0);
    previous = current;
  }
  REQUIRE(previous > limit); // Lifetime ids exceeded the live-object limit.
  std::printf("O1STORE idcap ten-times-limit create/destroy reuse ok\n");
}

} // namespace

int main() {
  try {
    check_cap_and_next_id();
    check_default_and_normalization();
    check_documented_text();
    check_rollback();
    check_flattened_and_abandoned_rollback();
    check_joined_refusal_preserves_pool();
    check_destroy_unknown();
    check_reuse();
    std::printf("test_o1_store_idcap: all groups passed\n");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "test_o1_store_idcap failed: %s\n", error.what());
    return 1;
  }
}
