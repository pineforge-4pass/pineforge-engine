// CR-150 L19A: successor lookup survives journal retirement without a
// per-replacement heap slope. This first witness uses only the base API.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_order.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <variant>

namespace no = pineforge::native_order;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

struct Driver {
    no::WorkingRequestCore core{{"roots", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    no::CommandContext context{};

    no::RequestHandle submit() {
        const auto result = core.submit(request(), 0, incarnation, ordinal);
        require(result.status == no::SubmitStatus::Accepted, "submit accepted");
        core.retire_history(core.last_ordinal());
        return *result.handle;
    }

    no::RequestHandle replace(no::RequestHandle target, bool direct) {
        no::ReplaceResult result;
        if (direct) {
            auto installed = core.apply_replace(target, request(), context, incarnation, ordinal);
            result = std::move(installed.result);
        } else {
            auto prepared = core.prepare_replace(target, request(), context, incarnation, ordinal);
            // Installing a prepared replacement remains allocation-free,
            // including at a spill boundary.
            global_allocation::refusing = true;
            auto installed = core.install_replace(std::move(prepared));
            global_allocation::refusing = false;
            require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed),
                    "prepared replacement installed");
            result = std::move(std::get<no::CommandInstalled<no::ReplaceResult>>(installed).result);
        }
        require(result.status == no::ReplaceStatus::Replaced, "replace accepted");
        ++incarnation;
        ++ordinal;
        core.retire_history(core.last_ordinal());
        require(core.history().empty(), "replacement journal retired");
        return *result.successor;
    }
};

struct Recording {
    Recording() {
        global_allocation::live_bytes = 0;
        global_allocation::recording = true;
    }
    ~Recording() { global_allocation::recording = false; }
};

void replacement_heap_is_flat(bool direct) {
    Recording recording;
    Driver driver;
    const auto root = driver.submit();
    auto tail = root;
    no::RequestHandle old_successor;
    for (std::size_t i = 0; i < 1024; ++i) {
        tail = driver.replace(tail, direct);
        if (i == 0) old_successor = tail;
    }
    const std::size_t low = global_allocation::live_bytes;
    for (std::size_t i = 1024; i < 16384; ++i) tail = driver.replace(tail, direct);
    const std::size_t high = global_allocation::live_bytes;
    std::printf("successor roots %s: 1024=%zu 16384=%zu retained bytes\n",
                direct ? "direct" : "prepared", low, high);
    // Other core state is bounded here: one live request, one dense issued
    // range, no group/cohort receipts, and an empty journal after each step.
    // The base retains >= 16 * (16384 - 1024) bytes of successor pairs.
    require(high <= low + 4096, "successor roots retain per-replacement heap bytes");
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, root), "root still in cohort");
    require(driver.core.cohort_contains(cohort, old_successor), "old spilled successor found");
    driver.core.cohort_remove(cohort, old_successor);
    require(!driver.core.cohort_contains(cohort, tail), "old successor removes canonical root");
}

void sorted_lookup_preserves_interleaved_roots() {
    Driver driver;
    std::array<no::RequestHandle, 3> roots{driver.submit(), driver.submit(), driver.submit()};
    auto tails = roots;
    std::array<no::RequestHandle, 3> oldest{};
    for (std::size_t i = 0; i < 2049; ++i) {
        const std::size_t slot = i % tails.size();
        tails[slot] = driver.replace(tails[slot], i % 2 == 0);
        if (i < tails.size()) oldest[slot] = tails[slot];
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tails[1]);
    require(driver.core.cohort_contains(cohort, roots[1]), "second root found");
    require(driver.core.cohort_contains(cohort, oldest[1]), "interleaved old root found");
    require(!driver.core.cohort_contains(cohort, oldest[0]), "first chain stays distinct");
    require(!driver.core.cohort_contains(cohort, oldest[2]), "third chain stays distinct");
    require(!driver.core.cohort_contains(cohort, {{"other", 1}, oldest[1].incarnation}),
            "foreign-run handle refused");
    require(!driver.core.cohort_contains(cohort, {{"roots", 1}, driver.incarnation + 7}),
            "unissued handle refused");
    driver.core.cohort_remove(cohort, oldest[1]);
    require(!driver.core.cohort_contains(cohort, tails[1]), "old root removed");
    driver.core.cohort_add(cohort, tails[2]);
    no::WorkingRequestCore moved(std::move(driver.core));
    require(moved.cohort_contains(cohort, oldest[2]), "move keeps spilled roots");
    no::WorkingRequestCore destination({"discard", 1});
    destination = std::move(moved);
    require(destination.cohort_contains(cohort, oldest[2]), "move assignment keeps spilled roots");
    destination.reset({"roots", 2});
    require(!destination.cohort_contains(cohort, oldest[2]), "reset forgets previous roots");
}

}  // namespace

int main() {
    try {
        replacement_heap_is_flat(true);
        replacement_heap_is_flat(false);
        sorted_lookup_preserves_interleaved_roots();
        std::puts("successor roots file: PASS");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "successor roots file: %s\n", error.what());
        return 1;
    }
}
