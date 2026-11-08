// T6: unbounded real Store, real disk backing, and a persistent page quota.
// Provisional until the first remote run supplies the literal exception-class,
// code and args pin. The observations below are diagnostics, not acceptance.
#include "a5u_test_store.hpp"

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <typeinfo>
#include <vector>

namespace {
using namespace pineforge::detail;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::Stats;
using pineforge::detail::o1::Store;
using a5u_test::require;

class QuotaBacking final : public ScratchBacking {
 public:
  static constexpr std::uint64_t kPageQuota = 256;

  explicit QuotaBacking(a5u_test::FileBacking& file)
      : ScratchBacking(file.kind(), file.page_size()), file_(file) {}
  std::uint64_t page_count() const noexcept override { return file_.page_count(); }
  std::uint64_t refusals() const noexcept { return refusals_; }

 private:
  ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer buffer) const noexcept override {
    return file_.read_page(id, buffer);
  }
  ScratchStatus do_write_page(ScratchPageId id, ScratchBytes bytes) noexcept override {
    if (id >= kPageQuota) {
      ++refusals_;
      return ScratchStatus::io_error;
    }
    return file_.write_page(id, bytes);
  }
  ScratchStatus do_sync(ScratchDurability durability) noexcept override {
    return file_.sync(durability);
  }
  a5u_test::FileBacking& file_;
  std::uint64_t refusals_ = 0;
};

void check_backing_failure() {
  a5u_test::FileBacking file;
  QuotaBacking backing(file);
  auto shape = a5u_test::shape(4);
  require(shape.max_live_objects == 0, "T6 needs the unbounded default");
  auto store = Store::open(shape, backing);
  require(store->stats().max_live_objects == 0, "Store changed the unbounded policy");

  // A committed nonzero marker plus zero-filled object payloads give independent
  // data oracles. Only create() runs inside the admission loop.
  constexpr std::uint64_t marker = UINT64_C(0x92e6c7a45138bd0f);
  const Id sentinel = store->create(sizeof marker, alignof(std::uint64_t));
  require(store->write(sentinel, 0, &marker, sizeof marker) == sizeof marker,
          "could not commit the retained marker");
  std::vector<Id> ids{sentinel};
  Stats before{};
  std::exception_ptr refusal;
  // Each object requires at least its 16-byte header in the finite file.
  const auto maximum_headers = QuotaBacking::kPageQuota * backing.page_size() / 16;
  for (std::uint64_t count = 0; count <= maximum_headers; ++count) {
    before = store->stats();
    Id admitted = o1::kNullId;
    try {
      admitted = store->create(sizeof marker, alignof(std::uint64_t));
    } catch (const std::exception&) {
      refusal = std::current_exception();
      break;
    }
    require(admitted != o1::kNullId && admitted > ids.back(),
            "admission did not return a fresh id");
    ids.push_back(admitted);
    require(store->stats().objects_live == ids.size(), "admission count differs from retained ids");
  }
  require(refusal != nullptr, "physical quota never refused growth");
  require(backing.refusals() != 0, "failure did not come from the backing quota");
  require(backing.page_count() <= QuotaBacking::kPageQuota, "backing exceeded its quota");

  bool typed = false;
  try {
    std::rethrow_exception(refusal);
  } catch (const std::exception& error) {
    const auto failure = pineforge::classify_run_failure(error);
    typed = dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr;
    std::cerr << "T6 observed exception=" << typeid(error).name()
              << " code=" << pineforge::run_failure_code_name(failure.code)
              << " args=" << (failure.args ? *failure.args : "{}")
              << " what=" << error.what() << '\n';
    require(dynamic_cast<const ScratchCapacityError*>(&error) == nullptr,
            "backing quota was mislabeled as a policy capacity refusal");
    require(failure.code != pineforge::RunFailureCode::scratch_capacity_exceeded,
            "physical failure was classified as a policy refusal");
  }

  const auto after = store->stats();
  require(after.max_live_objects == 0, "physical refusal changed policy");
  require(after.objects_live == before.objects_live && after.objects_live == ids.size(),
          "failed create changed live-object count");
  require(after.bytes_live == before.bytes_live, "failed create changed logical bytes");
  require(after.bytes_resident == before.bytes_resident, "physical refusal grew resident workspace");
  const Id failed_id = ids.back() + 1;
  require(!store->alive(failed_id), "failed create left a phantom object");
  for (const Id id : ids) {
    require(store->alive(id) && store->size(id) == sizeof marker, "retained id changed");
    std::uint64_t actual = UINT64_MAX;
    require(store->read(id, 0, &actual, sizeof actual) == sizeof actual,
            "retained object became unreadable");
    require(actual == (id == sentinel ? marker : 0), "retained object data changed");
  }

  const Id released = ids.back();
  store->release(released);
  require(!store->alive(released), "release left the object alive");
  require(store->stats().objects_live == before.objects_live - 1, "release changed wrong count");
  // Quota remains enabled: freeing one object must allow a real new create.
  const Id replacement = store->create(sizeof marker, alignof(std::uint64_t));
  require(replacement > released && replacement != failed_id,
          "recovery reused a released or failed-attempt id");
  require(store->alive(replacement), "replacement has no real object");
  require(store->stats().objects_live == before.objects_live, "recovery changed live count");
  require(store->stats().bytes_live == before.bytes_live, "recovery changed logical bytes");
  std::uint64_t actual = UINT64_MAX;
  require(store->read(replacement, 0, &actual, sizeof actual) == sizeof actual && actual == 0,
          "replacement payload is not a fresh zero-filled object");
  require(backing.page_count() <= QuotaBacking::kPageQuota, "recovery bypassed physical quota");
  require(typed, "physical refusal has no carried typed run-failure code; report to supervisor");
}
} // namespace

int main() {
  try {
    check_backing_failure();
  } catch (const std::exception& error) {
    std::cerr << "T6: " << error.what() << '\n';
    return 1;
  }
}
