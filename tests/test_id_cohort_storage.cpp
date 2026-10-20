// Uses the previous tree's public API too: its heap bound is a runtime red.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include <pineforge/native_order.hpp>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <variant>

namespace no = pineforge::native_order;
namespace {
void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

void empty_rosters_are_flat_and_reusable() {
    global_allocation::recording = true;
    {
        no::WorkingRequestCore core({"id-storage", 1});
        const auto first = core.cohort_open();
        for (int i = 1; i < 10; ++i) (void)core.cohort_open();
        const auto low = global_allocation::live_bytes;
        no::CohortHandle last;
        for (int i = 10; i < 10000; ++i) last = core.cohort_open();
        const auto high = global_allocation::live_bytes;
        require(high <= low + 4096, "empty cohort rosters grow per distinct id");
        require(first.value == 1 && last.value == 10000, "handle numbering changed");
        require(core.cohorts().size() == 10000, "logical roster count changed");
        std::uint64_t expected = 1;
        for (const auto& roster : core.cohorts()) {
            require(roster.handle.value == expected++ && roster.origins.empty(), "empty roster view");
        }
        std::uint64_t incarnation = 1;
        std::uint64_t ordinal = 1;
        no::Request request{no::Transact{1.0}, "entry", ""};
        const auto submitted = core.submit(request, 0, incarnation, ordinal);
        require(submitted.status == no::SubmitStatus::Accepted, "submit origin");
        const auto origin = *submitted.handle;
        core.cohort_add(first, origin);
        require(core.cohort_contains(first, origin), "old empty handle can recur");
        core.cohort_remove(first, origin);
        require(!core.cohort_contains(first, origin), "empty roster dropped after remove");
        core.cohort_remove(first, origin);
        require(core.cohort_receipts().back().status == no::CohortReceiptStatus::Applied,
                "removing from dropped empty roster is still Applied");
        core.cohort_add(first, origin);
        no::WorkingRequestCore moved(std::move(core));
        require(moved.cohort_contains(first, origin), "move keeps active roster");
        moved.reset({"id-storage", 2});
        require(moved.cohorts().empty(), "reset clears logical roster count");
        moved.cohort_add(first, origin);
        require(moved.cohort_receipts().back().status == no::CohortReceiptStatus::InvalidHandle,
                "old handle invalid after reset");
    }
    global_allocation::recording = false;
}
} // namespace

int main() {
    try {
        empty_rosters_are_flat_and_reusable();
        std::puts("id cohort storage: OK");
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
