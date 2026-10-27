// The reference retains the pre-change vector of every
// roster. The heap witness uses only the base kernel API, without retaining
// its own per-id data. Execute the red/green pair on the lab, not this lane.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "native_order_full_fold.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace no = pineforge::native_order;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

no::Request resting_request() {
    no::Request request{no::Transact{1.0}, "roster", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

template<class Rosters>
std::uint64_t roster_fold(const Rosters& rosters) {
    full_fold::Fold fold;
    fold.u(rosters.size());
    for (const no::CohortRoster& roster : rosters) {
        full_fold::fold(fold, roster.handle);
        full_fold::fold_vector(fold, roster.origins);
    }
    return fold.h;
}

// The original vector storage and receipt rules, with an independent walk
// through predecessor links instead of the production successor-root index.
struct Reference {
    no::RunIdentity run{"kernel-rosters", 1};
    std::vector<no::CohortRoster> rosters;
    std::map<std::uint64_t, std::optional<no::RequestHandle>> predecessors;
    std::set<std::uint64_t> working;

    no::CohortHandle open() {
        const no::CohortHandle handle{static_cast<std::uint64_t>(rosters.size()) + 1};
        rosters.push_back(no::CohortRoster{handle, {}});
        return handle;
    }

    void issue(no::RequestHandle handle, std::optional<no::RequestHandle> predecessor = {}) {
        predecessors.emplace(handle.incarnation, predecessor);
        working.insert(handle.incarnation);
        if (predecessor) working.erase(predecessor->incarnation);
    }

    std::optional<no::RequestHandle> root(no::RequestHandle handle) const {
        if (handle.run != run || predecessors.find(handle.incarnation) == predecessors.end()) {
            return std::nullopt;
        }
        for (;;) {
            const auto& previous = predecessors.at(handle.incarnation);
            if (!previous) return handle;
            handle = *previous;
        }
    }

    no::CohortRoster* find(no::CohortHandle handle) {
        const auto at = std::lower_bound(rosters.begin(), rosters.end(), handle,
            [](const no::CohortRoster& row, no::CohortHandle value) {
                return row.handle < value;
            });
        return at != rosters.end() && at->handle == handle ? &*at : nullptr;
    }

    no::CohortReceipt edit(no::CohortReceiptOperation operation,
                         no::CohortHandle handle, no::RequestHandle origin) {
        no::CohortReceipt receipt{operation, no::CohortReceiptStatus::InvalidHandle, handle, origin};
        auto* roster = find(handle);
        if (!roster) return receipt;
        const auto canonical = root(origin);
        if (!canonical) {
            receipt.status = no::CohortReceiptStatus::UnknownOrigin;
        } else if (operation == no::CohortReceiptOperation::Add
                   && working.count(origin.incarnation) == 0) {
            receipt.status = no::CohortReceiptStatus::TerminalOrigin;
        } else {
            auto& members = roster->origins;
            const auto at = std::lower_bound(members.begin(), members.end(), *canonical,
                [](const no::RequestHandle& left, const no::RequestHandle& right) {
                    return left.incarnation < right.incarnation;
                });
            if (operation == no::CohortReceiptOperation::Add) {
                if (at == members.end() || *at != *canonical) members.insert(at, *canonical);
            } else if (at != members.end() && *at == *canonical) {
                members.erase(at);
            }
            receipt.status = no::CohortReceiptStatus::Applied;
        }
        return receipt;
    }

    bool contains(no::CohortHandle handle, no::RequestHandle origin) {
        const auto* roster = find(handle);
        const auto canonical = root(origin);
        return roster && canonical
            && std::find(roster->origins.begin(), roster->origins.end(), *canonical)
                != roster->origins.end();
    }
};

struct Driver {
    no::WorkingRequestCore core{{"kernel-rosters", 1}};
    Reference reference;
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;

    no::RequestHandle submit() {
        const auto result = core.submit(resting_request(), 0, incarnation, ordinal);
        require(result.status == no::SubmitStatus::Accepted && result.handle, "opening accepted");
        reference.issue(*result.handle);
        core.retire_history(core.last_ordinal());
        return *result.handle;
    }

    no::RequestHandle replace(no::RequestHandle handle) {
        const auto result = core.replace(handle, resting_request(), 0, incarnation, ordinal);
        require(result.status == no::ReplaceStatus::Replaced && result.successor, "replacement accepted");
        reference.issue(*result.successor, handle);
        core.retire_history(core.last_ordinal());
        return *result.successor;
    }

    no::CohortHandle open() {
        const auto expected = reference.open();
        const auto actual = core.cohort_open();
        require(actual == expected, "cohort numbering matches the retained-vector reference");
        return actual;
    }

    void edit(bool add, no::CohortHandle cohort, no::RequestHandle origin) {
        const auto operation = add ? no::CohortReceiptOperation::Add : no::CohortReceiptOperation::Remove;
        const auto expected = reference.edit(operation, cohort, origin);
        if (add) core.cohort_add(cohort, origin);
        else core.cohort_remove(cohort, origin);
        const auto& actual = core.cohort_receipts().back();
        require(actual.operation == expected.operation && actual.status == expected.status
                && actual.cohort == expected.cohort && actual.origin == expected.origin,
                "roster receipt matches the original implementation");
        core.retire_cohort_receipts(core.cohort_receipt_count());
    }

    void contains(no::CohortHandle cohort, no::RequestHandle origin) {
        require(core.cohort_contains(cohort, origin) == reference.contains(cohort, origin),
                "membership matches the original implementation");
    }

    void verify() {
        require(core.cohorts().size() == reference.rosters.size(), "logical roster count preserved");
        require(roster_fold(core.cohorts()) == roster_fold(reference.rosters),
                "every handle and origin folds in the original order");
        const auto rosters = core.cohorts();
        std::size_t index = 0;
        for (const no::CohortRoster& row : rosters) {
            const auto& expected = reference.rosters[index++];
            require(row.handle == expected.handle && row.origins == expected.origins,
                    "logical roster rows equal the retained-vector reference");
        }
        require(index == reference.rosters.size(), "iteration visits every empty and live roster");
    }
};

void many_distinct_and_recurring_handles() {
    Driver driver;
    std::array<no::RequestHandle, 3> roots{driver.submit(), driver.submit(), driver.submit()};
    auto tail = roots[0];
    no::RequestHandle old_successor;
    for (int i = 0; i < 193; ++i) {
        tail = driver.replace(tail);
        if (i == 0) old_successor = tail;
    }
    constexpr std::size_t count = 4096;
    for (std::size_t i = 0; i < count; ++i) {
        const auto cohort = driver.open();
        driver.edit(true, cohort, tail);
        driver.edit(true, cohort, tail); // Duplicate insertion.
        driver.edit(true, cohort, roots[2]);
        driver.contains(cohort, old_successor); // A root read from disk.
        driver.edit(false, cohort, old_successor);
        if (i % 17 != 0) driver.edit(false, cohort, roots[2]);
        if (i % 127 == 0) driver.verify();
    }
    driver.verify();

    // Visit every old id in a permutation, with empty rosters on both sides
    // of rosters that have stayed live throughout. No prefix can be retired.
    for (std::size_t i = 0; i < count; ++i) {
        const no::CohortHandle cohort{static_cast<std::uint64_t>((i * 129) % count) + 1};
        driver.edit(true, cohort, roots[1]);
        driver.contains(cohort, roots[1]);
        driver.edit(true, cohort, old_successor); // TerminalOrigin, not a new member.
        driver.edit(false, cohort, tail); // Applied even when absent.
        driver.edit(false, cohort, roots[1]);
        driver.contains(cohort, roots[1]);
        if (i % 127 == 0) driver.verify();
    }
    driver.edit(true, no::CohortHandle{}, roots[1]);
    driver.edit(false, no::CohortHandle{count + 1}, roots[1]);
    driver.edit(true, no::CohortHandle{1}, no::RequestHandle{driver.core.identity(), 999999});
    driver.edit(false, no::CohortHandle{1}, no::RequestHandle{{"foreign", 1}, roots[1].incarnation});
    require(driver.core.cancel(roots[1], driver.ordinal).status == no::CancelStatus::Cancelled,
            "origin cancelled");
    driver.reference.working.erase(roots[1].incarnation);
    driver.core.retire_history(driver.core.last_ordinal());
    driver.edit(true, no::CohortHandle{1}, roots[1]);
    driver.edit(false, no::CohortHandle{1}, roots[1]);
    driver.verify();

    // Existing readers use typed predicates and iterator arrow access.
    const auto rows = driver.core.cohorts();
    const auto found = std::find_if(rows.begin(), rows.end(), [](const no::CohortRoster& row) {
        return row.handle == no::CohortHandle{2};
    });
    require(found != rows.end() && found->origins.empty(), "empty roster remains findable by iteration");
    const auto& indexed = driver.core.cohorts()[17];
    require(!indexed.origins.empty()
            && indexed.handle == driver.reference.rosters[17].handle
            && indexed.origins == driver.reference.rosters[17].origins,
            "indexed live roster preserves its members");

    const auto expected = roster_fold(driver.reference.rosters);
    global_allocation::refusing = true;
    const auto actual = roster_fold(driver.core.cohorts());
    global_allocation::refusing = false;
    require(actual == expected, "logical hash walk needs no heap allocation or file I/O");
    no::WorkingRequestCore moved(std::move(driver.core));
    require(roster_fold(moved.cohorts()) == expected, "move retains live and empty roster identities");
    require(driver.core.cohorts().empty(), "moved-from roster view is empty");
    no::WorkingRequestCore assigned({"discard", 1});
    assigned = std::move(moved);
    require(roster_fold(assigned.cohorts()) == expected, "move assignment transfers nonempty rosters");
    assigned.reset({"kernel-rosters", 2});
    require(assigned.cohorts().empty(), "reset discards old roster identities");
    require(!assigned.cohort_contains(no::CohortHandle{1}, roots[0]), "old-run membership is absent");
    require(assigned.cohort_open() == no::CohortHandle{1}, "new run restarts handle numbering");
}

struct Recording {
    Recording() { global_allocation::recording = true; }
    ~Recording() { global_allocation::recording = false; }
};

void distinct_id_heap_is_flat() {
    Recording recording;
    no::WorkingRequestCore core{{"roster-heap", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    const auto submitted = core.submit(resting_request(), 0, incarnation, ordinal);
    require(submitted.status == no::SubmitStatus::Accepted && submitted.handle, "heap opening accepted");
    const auto origin = *submitted.handle;
    core.retire_history(core.last_ordinal());
    const auto keeper = core.cohort_open();
    core.cohort_add(keeper, origin);
    core.retire_cohort_receipts(core.cohort_receipt_count());
    const auto round_trip = [&] {
        const auto cohort = core.cohort_open();
        core.cohort_add(cohort, origin);
        core.cohort_remove(cohort, origin);
        core.retire_cohort_receipts(core.cohort_receipt_count());
    };
    for (int i = 0; i < 10; ++i) round_trip();
    const auto low = global_allocation::live_bytes;
    for (int i = 10; i < 10000; ++i) round_trip();
    const auto high = global_allocation::live_bytes;
    require(high <= low + 4096, "empty rosters or member capacity grow per distinct id");
    require(core.cohort_contains(keeper, origin), "live roster survives empty-roster eviction");
    core.cohort_add(no::CohortHandle{2}, origin);
    require(core.cohort_contains(no::CohortHandle{2}, origin), "old empty handle recurs");
    core.cohort_remove(no::CohortHandle{2}, origin);
    core.retire_cohort_receipts(core.cohort_receipt_count());

    // A historical peak in live rosters must not leave a vector's capacity
    // behind once all those rosters are empty.
    const std::uint64_t first = static_cast<std::uint64_t>(core.cohorts().size()) + 1;
    for (int i = 0; i < 2048; ++i) {
        core.cohort_add(core.cohort_open(), origin);
        core.retire_cohort_receipts(core.cohort_receipt_count());
    }
    for (std::uint64_t i = 0; i < 2048; ++i) {
        core.cohort_remove(no::CohortHandle{first + i}, origin);
        core.retire_cohort_receipts(core.cohort_receipt_count());
    }
    const auto after_peak = global_allocation::live_bytes;
    require(after_peak <= high + 4096, "empty rosters retain capacity from a historical live-set peak");
    for (int i = 0; i < 2048; ++i) round_trip();
    require(global_allocation::live_bytes <= after_peak + 4096,
            "evicted rosters do not accumulate after a live-set peak");
    std::printf("kernel cohort rosters: 10=%zu 10000=%zu retained bytes\n", low, high);
}

void empty_roster_is_a_valid_owner() {
    no::WorkingRequestCore core{{"roster-owner", 1}};
    const auto first = core.cohort_open();
    for (int i = 0; i < 128; ++i) (void)core.cohort_open();
    no::Request close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "close", ""};
    close.owner = no::BindCohort{first};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    require(core.submit(close, 0, incarnation, ordinal).status == no::SubmitStatus::Accepted,
            "an allocated empty cohort is still a valid owner");
    close.owner = no::BindCohort{no::CohortHandle{130}};
    const auto rejected = core.submit(close, 0, incarnation, ordinal);
    require(rejected.status == no::SubmitStatus::Rejected
            && rejected.reason == no::RequestRejectReason::InvalidOwner,
            "an unallocated cohort remains an invalid owner");
}

void empty_handles_need_no_allocation() {
    Reference reference;
    for (int i = 0; i < 10000; ++i) (void)reference.open();
    no::WorkingRequestCore core{reference.run};
    // The old vector must allocate as handles are issued. The current core
    // represents empty rosters by its dense sequence alone; the population
    // comes from the independent vector, without a private spill threshold.
    global_allocation::refusing = true;
    try {
        for (const auto& row : reference.rosters)
            require(core.cohort_open() == row.handle, "empty handles preserve vector numbering");
    } catch (...) {
        global_allocation::refusing = false;
        throw;
    }
    global_allocation::refusing = false;
    require(roster_fold(core.cohorts()) == roster_fold(reference.rosters),
            "allocation-free empty rosters retain the independent vector fold");
}

} // namespace

int main() {
    try {
        many_distinct_and_recurring_handles();
        distinct_id_heap_is_flat();
        empty_roster_is_a_valid_owner();
        empty_handles_need_no_allocation();
        std::puts("kernel cohort rosters: PASS");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "kernel cohort rosters: %s\n", error.what());
        return 1;
    }
}
