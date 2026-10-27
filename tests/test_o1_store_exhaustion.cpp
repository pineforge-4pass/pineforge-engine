// CR-417 fail-before on ff813d6dd: StoreImpl::throw_status discards both the
// carried arena verdict and the scratch_io_failed classification.
#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::detail;

#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #condition, __FILE__, \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

class ExhaustionBacking final : public ScratchBacking {
 public:
  ExhaustionBacking()
      : ScratchBacking(ScratchBackingKind::test_memory, 4096),
        storage_(4096 * 1024),
        inner_(ScratchBuffer{storage_.data(), storage_.size()}, 4096),
        exhausted_(ENOSPC, "fixture arena exhausted",
            {{"reason", "arena_exhausted"}, {"store", "store_objects"},
             {"arena_bytes", INT64_C(1048576)}, {"claimed_bytes", INT64_C(1048576)},
             {"high_water_bytes", INT64_C(1048576)},
             {"needed_bytes_lower_bound", INT64_C(1048576)}}) {}

  std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
  const ReportScratchIoError* carried_failure() const noexcept override {
    return carried_ ? &exhausted_ : nullptr;
  }
  void fail_next_write(bool carry_arena_verdict) noexcept {
    armed_ = true;
    carry_arena_verdict_ = carry_arena_verdict;
  }
  const ReportScratchIoError& exhausted() const noexcept { return exhausted_; }

 protected:
  ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer output) const noexcept override {
    carried_ = false; // Cleanup I/O may erase the backing's borrowed verdict.
    return inner_.read_page(id, output);
  }
  ScratchStatus do_write_page(ScratchPageId id, ScratchBytes input) noexcept override {
    carried_ = false;
    if (armed_) {
      armed_ = false;
      carried_ = carry_arena_verdict_;
      return ScratchStatus::io_error;
    }
    return inner_.write_page(id, input);
  }
  ScratchStatus do_sync(ScratchDurability durability) noexcept override {
    carried_ = false;
    return inner_.sync(durability);
  }

 private:
  std::vector<std::byte> storage_;
  FixedMemoryScratchBacking inner_;
  ReportScratchIoError exhausted_;
  bool armed_ = false;
  bool carry_arena_verdict_ = false;
  mutable bool carried_ = false;
};

void check_exhaustion(bool carry_arena_verdict) {
  ExhaustionBacking backing;
  auto store = o1::Store::open(o1::Shape{}, backing);
  auto tx = store->begin();
  const auto id = store->create(0, 1);
  REQUIRE(id == 1);
  backing.fail_next_write(carry_arena_verdict);
  bool refused = false;
  try {
    tx->commit();
  } catch (const ReportScratchIoError& error) {
    REQUIRE(carry_arena_verdict);
    refused = true;
    REQUIRE(error.code().value() == ENOSPC);
    REQUIRE(std::strcmp(error.what(), backing.exhausted().what()) == 0);
    const auto failure = classify_run_failure(error);
    REQUIRE(failure.code == RunFailureCode::report_sink_failed);
    REQUIRE(failure.args != nullptr);
    REQUIRE(*failure.args ==
        "{\"arena_bytes\":1048576,\"claimed_bytes\":1048576,"
        "\"high_water_bytes\":1048576,\"needed_bytes_lower_bound\":1048576,"
        "\"reason\":\"arena_exhausted\",\"store\":\"store_objects\"}");
  } catch (const std::runtime_error& error) {
    REQUIRE(!carry_arena_verdict);
    refused = true;
    const auto failure = classify_run_failure(error);
    REQUIRE(failure.code == RunFailureCode::scratch_io_failed);
    REQUIRE(failure.args == nullptr || *failure.args == "{}");
    REQUIRE(std::string(error.what()) == "o1 store: commit: io_error");
  }
  REQUIRE(refused);
  tx->rollback();
  REQUIRE(store->stats().objects_live == 0);
  REQUIRE(!store->alive(id));
}
} // namespace

int main() {
  check_exhaustion(false);
  check_exhaustion(true);
  std::puts("O1STORE exhaustion preserves scratch I/O code and arena counters");
}
