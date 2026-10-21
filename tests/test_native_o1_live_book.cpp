// CR-221 O1-E2, row E02 of the whole-run O(1) inventory: the live request book
// and its re-pricing index.
//
// The book is a vector of every simultaneously live request (LiveRequest plus
// its shared definition) and repriced_ names every request a keep_handle
// re-price moved, so the heap grows with Q. The fixtures reach that population
// (Q live limit orders, thousands of them re-priced in place) and assert it,
// then require what the row asks for:
//   * the book keeps a fixed resident part and holds the rest outside the
//     heap: two books of very different Q retain the same bytes, within a
//     slack that is a small multiple of the resident part;
//   * nothing the book answers changes. A plain vector of handles, kept in
//     step with every command, is the old representation: the queue order
//     (matching priority and tie order), every row's full value, the handle
//     lookups, a re-price that keeps the handle, a plain replace, a cancel,
//     a fill, the children a parent waits on and the arm of a spilled
//     child must all agree with it;
//   * the per-point allowance refresh reaches the rows outside the heap, and
//     only the closes it is meant to reach;
//   * a command prepared and abandoned leaves the book exactly as it was, and
//     a spill that cannot be written fails the command that needed the room
//     before it changes anything.
// The fixtures use the core's public API of the previous epoch only, so this
// TU compiles against it. There they reach the same populations and fail on
// the resident-byte comparison, not on a missing declaration.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace no = pineforge::native_order;
namespace ff = full_fold;

namespace {
int failures = 0;
int checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
        }                                                                           \
    } while (false)
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

// Points the scratch files' directory (TMPDIR, read when a store creates its
// file) somewhere, and puts it back when it goes out of scope.
struct TempDirectory {
    std::string saved;
    bool had = false;
    explicit TempDirectory(const char* value) {
        const char* old = std::getenv("TMPDIR");
        had = old != nullptr;
        if (had) saved = old;
        ::setenv("TMPDIR", value, 1);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (had) ::setenv("TMPDIR", saved.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

no::Request limit(double units, double price, const char* label = "") {
    no::Request request{no::Transact{units}, label, ""};
    request.trigger = no::Limit{price};
    return request;
}

no::EvaluationContext evaluation_at(std::uint64_t point) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = point;
    context.cursor.point.effective_time_ms = 1000;
    context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

struct Book {
    no::RunIdentity run{"o1-e2-book", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t inc = 1;
    std::uint64_t ord = 1;

    no::RequestHandle submit(const no::Request& request) {
        const auto result = core.submit(request, 0, inc, ord);
        REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
        return *result.handle;
    }

    void install(no::Preparation<no::PreparedMutation> preparation) {
        REQUIRE(std::holds_alternative<no::PreparedMutation>(preparation));
        auto installed = core.install_mutation(
                std::get<no::PreparedMutation>(std::move(preparation)));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ord += std::get<no::Installed>(installed).events.count;
    }

    // Binds an unbound close to the book position it observes (the evaluation
    // that installs its BookClose authority), at a point past every ordinal the
    // core has committed.
    void bind(const no::RequestHandle& target, const no::PositionIdentity& position) {
        no::TargetObservation observation;
        observation.current_position = position;
        install(core.prepare_evaluation(target, evaluation_at(ord + 1), observation, ord));
    }

    // Evaluates, then executes, one request at a point past every ordinal the
    // core has committed.
    no::EventId fill(const no::RequestHandle& target, double units,
                     const no::PositionIdentity& before, const no::PositionIdentity& after) {
        const std::uint64_t at = ord + 1;
        no::TargetObservation observation;
        observation.current_position = before;
        auto evaluated = core.prepare_evaluation(target, evaluation_at(at), observation, ord);
        if (std::holds_alternative<no::PreparedMutation>(evaluated)) {
            install(std::move(evaluated));
        }
        no::ExecutionProposal proposal;
        proposal.cursor.point.ordinal = at;
        proposal.cursor.point.effective_time_ms = 1000;
        proposal.raw_price = 100;
        proposal.resolved_price = 100;
        proposal.physical_action = no::Transact{units};
        proposal.pre_fill = before;
        proposal.pre_target.current_position = before;
        proposal.inspected_opened_units = units;
        proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(target, proposal, ord);
        REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = units;
        facts.result.current_ticket = 1;
        facts.cycle_after = 1;
        facts.post_target.current_position = after;
        facts.committed_action = no::Transact{units};
        auto installed = core.install_execution(
                std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ord += std::get<no::Installed>(installed).events.count;
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
        REQUIRE(applied != nullptr);
        return no::EventId{run, applied->ordinal};
    }
};

// The book as a reader sees it: every row, in queue order.
std::vector<no::RequestHandle> queue(const no::WorkingRequestCore& core) {
    std::vector<no::RequestHandle> handles;
    handles.reserve(core.live().size());
    for (const auto& row : core.live()) handles.push_back(row.handle());
    return handles;
}

// The same book as one word: its size and every row's full value, in order.
std::uint64_t book_value(const no::WorkingRequestCore& core) {
    ff::Fold fold;
    fold.u(core.live().size());
    for (const auto& row : core.live()) fold.u(ff::value(row));
    return fold.h;
}

void erase_handle(std::vector<no::RequestHandle>& order, const no::RequestHandle& handle) {
    const auto at = std::find(order.begin(), order.end(), handle);
    if (at != order.end()) order.erase(at);
}

struct BookRun {
    std::size_t retained = 0;    // heap the core holds once the book stands
    std::size_t population = 0;
};

// Q live limit orders, then R of them re-priced in place (keep_handle): the
// population of the book and of the re-pricing index. The oracle vector is
// reserved before the heap is watched.
BookRun book_bytes(std::size_t population, std::size_t reprices) {
    BookRun result;
    std::vector<no::RequestHandle> handles;
    handles.reserve(population);
    global_allocation::recording = true;
    const std::size_t start = global_allocation::live_bytes;
    try {
        Book b;
        for (std::size_t i = 0; i != population; ++i) {
            handles.push_back(b.submit(limit(1.0, 50.0 + static_cast<double>(i % 211) * 0.25, "q")));
        }
        REQUIRE(b.core.live().size() == population);           // E02: Q
        no::ReplaceOptions keep;
        keep.keep_handle = true;
        for (std::size_t r = 0; r != reprices; ++r) {
            const std::size_t at = (r * 7) % population;         // distinct rows
            const auto replaced = b.core.replace(
                    handles[at], limit(2.0, 300.0 + static_cast<double>(r) * 0.001, "kept"), 0,
                    b.inc, b.ord, std::nullopt, keep);
            REQUIRE(replaced.status == no::ReplaceStatus::Replaced);
            REQUIRE(replaced.successor && *replaced.successor == handles[at]);
        }
        REQUIRE(b.core.live().size() == population);           // none left the book
        // The newest re-price stands at the back of the queue.
        CHECK(b.core.live().back().handle() == handles[((reprices - 1) * 7) % population]);
        // The journal holds an event per command and is E03's row, not this
        // one's: it is retired here (every command of this book is already
        // folded into the book), so what is left to measure is the book.
        (void) b.core.retire_history(b.core.last_ordinal());
        result.retained = global_allocation::live_bytes - start;
        result.population = b.core.live().size();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "o1-e2 book fixture (Q=%zu, R=%zu): %s\n", population, reprices,
                     error.what());
        ++failures;
    }
    global_allocation::recording = false;
    return result;
}

// Every command the book answers, against the plain-vector oracle, on a book
// whose middle is outside the heap.
void exercise_book(std::size_t population) {
    Book b;
    std::vector<no::RequestHandle> order;      // the old representation, in queue order
    std::vector<no::RequestHandle> handles;    // by submission index
    std::vector<std::uint64_t> values;         // each row's full value as submitted
    order.reserve(population + 16);
    handles.reserve(population + 16);
    values.reserve(population + 16);

    // A parent and children waiting on it sit in the third quarter of the
    // queue, and the commands below land on rows in its middle: past the first
    // few thousand rows (the book's resident part) and before the newest one,
    // so every one of them is a row the heap does not hold.
    const std::size_t parent_at = population * 3 / 4;
    no::RequestHandle parent{};
    std::vector<no::RequestHandle> children;
    for (std::size_t i = 0; i != population; ++i) {
        const no::RequestHandle handle =
                b.submit(limit(1.0, 50.0 + static_cast<double>(i % 211) * 0.25, "q"));
        handles.push_back(handle);
        order.push_back(handle);
        values.push_back(ff::value(*b.core.find_live(handle)));
        if (i == parent_at) {
            parent = b.submit(limit(1000.0, 40.0, "parent"));
            order.push_back(parent);
            handles.push_back(parent);
            values.push_back(ff::value(*b.core.find_live(parent)));
            for (int c = 0; c != 4; ++c) {
                no::Request child{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
                child.owner = no::WaitForApplied{parent};
                const no::RequestHandle waiting = b.submit(child);
                children.push_back(waiting);
                order.push_back(waiting);
                handles.push_back(waiting);
                values.push_back(ff::value(*b.core.find_live(waiting)));
            }
        }
    }
    const std::size_t total = order.size();
    REQUIRE(b.core.live().size() == total);

    const auto check_book = [&](const char* when) {
        const std::vector<no::RequestHandle> now = queue(b.core);
        if (now != order) std::fprintf(stderr, "queue order differs %s\n", when);
        CHECK(now == order);
        CHECK(b.core.live().size() == order.size());
        CHECK(b.core.live().empty() == order.empty());
        if (!order.empty()) {
            CHECK(b.core.live().front().handle() == order.front());
            CHECK(b.core.live().back().handle() == order.back());
        }
    };
    check_book("after submit");

    // Every row reads back to the value it had when it was submitted, by
    // handle and by walking the queue.
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i != handles.size(); i += 3) {
        const no::LiveRequest* row = b.core.find_live(handles[i]);
        if (row == nullptr || ff::value(*row) != values[i]) ++mismatches;
    }
    {
        std::size_t index = 0;
        for (const auto& row : b.core.live()) {
            if (ff::value(row) != values[index]) ++mismatches;   // submission order == queue order
            ++index;
        }
    }
    CHECK(mismatches == 0);

    // The children the parent waits on, found by the book's scan, in order.
    CHECK(b.core.waiting_children(parent) == children);
    CHECK(b.core.has_waiting_children(parent));

    // Commands that land on rows the heap does not hold. All of them sit before
    // the parent block, so a submission index is also the row's place in
    // `handles`.
    const std::size_t s1 = population * 5 / 8;      // re-priced, handle kept
    const std::size_t s2 = s1 + 40;                 // replaced
    const std::size_t s3 = s1 + 80;                 // cancelled
    const std::size_t s4 = s1 + 120;                // filled
    const std::size_t s5 = s1 + 160;                // prepared, abandoned
    REQUIRE(s5 < parent_at);
    no::ReplaceOptions keep;
    keep.keep_handle = true;
    {
        const no::RequestHandle target = handles[s1];
        const auto replaced = b.core.replace(target, limit(3.0, 77.0, "kept"), 0, b.inc, b.ord,
                                             std::nullopt, keep);
        REQUIRE(replaced.status == no::ReplaceStatus::Replaced);
        CHECK(replaced.successor && *replaced.successor == target);   // the handle survives
        erase_handle(order, target);
        order.push_back(target);                                      // and goes to the back
        const no::LiveRequest* row = b.core.find_live(target);
        REQUIRE(row != nullptr);
        CHECK(std::get<no::Limit>(row->request().trigger).price == 77.0);
        CHECK(b.core.live().back().handle() == target);
    }
    check_book("after keep-handle re-price");
    {
        const no::RequestHandle target = handles[s2];
        const auto replaced = b.core.replace(target, limit(4.0, 78.0, "plain"), 0, b.inc, b.ord);
        REQUIRE(replaced.status == no::ReplaceStatus::Replaced && replaced.successor);
        CHECK(!(*replaced.successor == target));                      // a successor is issued
        CHECK(b.core.find_live(target) == nullptr);
        erase_handle(order, target);
        order.push_back(*replaced.successor);
        const no::LiveRequest* row = b.core.find_live(*replaced.successor);
        REQUIRE(row != nullptr);
        CHECK(std::get<no::Limit>(row->request().trigger).price == 78.0);
    }
    check_book("after plain replace");
    {
        const no::RequestHandle target = handles[s3];
        CHECK(b.core.cancel(target, b.ord).status == no::CancelStatus::Cancelled);
        CHECK(b.core.find_live(target) == nullptr);
        erase_handle(order, target);
    }
    check_book("after cancel");
    {
        const no::RequestHandle target = handles[s4];
        const no::PositionIdentity flat = no::PositionFlat{};
        const no::PositionIdentity long_book = no::PositionNonflat{1, no::Side::Long};
        (void) b.fill(target, 1.0, flat, long_book);
        CHECK(b.core.find_live(target) == nullptr);                   // a terminal fill leaves
        erase_handle(order, target);
    }
    check_book("after fill");

    // A command prepared and abandoned changes nothing a reader can see.
    {
        const std::uint64_t digest = book_value(b.core);
        const std::size_t end = b.core.history_end();
        const no::RequestHandle target = handles[s5];
        {
            auto abandoned = b.core.prepare_cancel(target, b.ord);
            CHECK(static_cast<bool>(abandoned));
        }
        {
            auto abandoned = b.core.prepare_replace(target, limit(5.0, 79.0, "never"),
                                                    no::CommandContext{}, b.inc, b.ord);
            CHECK(static_cast<bool>(abandoned));
        }
        CHECK(b.core.history_end() == end);                           // no partial journal row
        CHECK(book_value(b.core) == digest);                          // no changed or lost row
        CHECK(b.core.find_live(target) != nullptr);
    }
    check_book("after abandoned commands");

    // Every row's value, read by walking the queue and by handle, agree.
    {
        ff::Fold walked;
        walked.u(b.core.live().size());
        for (const auto& row : b.core.live()) walked.u(ff::value(row));
        ff::Fold looked;
        looked.u(order.size());
        for (const auto& handle : order) {
            const no::LiveRequest* row = b.core.find_live(handle);
            REQUIRE(row != nullptr);
            looked.u(ff::value(*row));
        }
        CHECK(walked.h == looked.h);
    }

    // Ownership: the parent fills, its fill arms each waiting child -- rows the
    // heap does not hold -- and the children leave the wait.
    {
        const no::PositionIdentity flat = no::PositionFlat{};
        const no::PositionIdentity long_book = no::PositionNonflat{1, no::Side::Long};
        const no::EventId parent_fill = b.fill(parent, 1000.0, flat, long_book);
        erase_handle(order, parent);
        CHECK(b.core.find_live(parent) == nullptr);
        no::OpeningObservation opening;
        opening.queried_opening = parent;
        opening.queried_cycle = 1;
        opening.current_position = long_book;
        opening.has_live_matching_lot = true;
        for (const auto& child : children) {
            b.install(b.core.prepare_owner_applied(parent_fill, child, opening, b.ord));
            const no::LiveRequest* armed = b.core.find_live(child);
            REQUIRE(armed != nullptr);
            CHECK(std::holds_alternative<no::OpeningClose>(armed->authority));
            CHECK(std::get<no::RemainingUnits>(armed->remaining).q == 1000.0);
        }
        CHECK(b.core.waiting_children(parent).empty());
        CHECK(!b.core.has_waiting_children(parent));
    }
    check_book("after the parent's fill armed its children");
}

// The point an allowance stands at (0 for none).
std::uint64_t allowance_point(const no::Allowance& allowance) {
    if (const auto* units = std::get_if<no::AllowanceUnits>(&allowance)) return units->point_ordinal;
    if (const auto* all = std::get_if<no::AllowanceAllScope>(&allowance)) return all->point_ordinal;
    if (const auto* deferred = std::get_if<no::AllowanceDeferred>(&allowance)) {
        return deferred->point_ordinal;
    }
    return 0;
}

// The per-driver-point allowance refresh walks the whole book: it re-allows
// the closes bound to the sampled book position, whether or not the heap holds
// them, and touches nothing else. Closes of two cycles are bound, a few
// thousand of them (so many are outside the heap), and the book is refreshed
// at three points against three positions.
void refresh_reaches_spilled_rows() {
    Book b;
    constexpr std::size_t kClosers = 6000;
    const no::PositionIdentity long7 = no::PositionNonflat{7, no::Side::Long};
    const no::PositionIdentity long8 = no::PositionNonflat{8, no::Side::Long};
    const no::PositionIdentity flat = no::PositionFlat{};
    std::vector<no::RequestHandle> sevens;
    std::vector<no::RequestHandle> eights;
    for (std::size_t i = 0; i != kClosers; ++i) {
        const no::RequestHandle handle =
                b.submit(no::Request{no::Reduce{no::ExplicitUnits{1.0}}, "close", ""});
        (i % 2 == 0 ? sevens : eights).push_back(handle);
    }
    for (std::size_t i = 0; i != kClosers; ++i) {
        // Submission order interleaves the two cycles.
        b.bind(i % 2 == 0 ? sevens[i / 2] : eights[i / 2], i % 2 == 0 ? long7 : long8);
    }
    REQUIRE(b.core.live().size() == kClosers);

    // What every row stands at: its cycle (from the BookClose it was bound
    // to) and the point of its allowance.
    const auto survey = [&](std::uint64_t* at7, std::uint64_t* at8, std::size_t* units_ok) {
        std::size_t sevens_seen = 0;
        std::size_t eights_seen = 0;
        *units_ok = 0;
        std::uint64_t low7 = ~std::uint64_t{0};
        std::uint64_t high7 = 0;
        std::uint64_t low8 = ~std::uint64_t{0};
        std::uint64_t high8 = 0;
        for (const auto& row : b.core.live()) {
            const auto* close = std::get_if<no::BookClose>(&row.authority);
            REQUIRE(close != nullptr);
            const std::uint64_t point = allowance_point(row.allowance);
            const auto* units = std::get_if<no::AllowanceUnits>(&row.allowance);
            if (units != nullptr && units->initial == 1.0 && units->left == 1.0) ++*units_ok;
            if (close->cycle == 7) {
                ++sevens_seen;
                low7 = std::min(low7, point);
                high7 = std::max(high7, point);
            } else {
                REQUIRE(close->cycle == 8);
                ++eights_seen;
                low8 = std::min(low8, point);
                high8 = std::max(high8, point);
            }
        }
        REQUIRE(sevens_seen == sevens.size() && eights_seen == eights.size());
        // Within a cycle the rows are all at one point or at many: report the
        // range so the caller can state what it expects of it.
        at7[0] = low7;
        at7[1] = high7;
        at8[0] = low8;
        at8[1] = high8;
    };
    std::uint64_t seven[2] = {0, 0};
    std::uint64_t eight[2] = {0, 0};
    std::size_t units_ok = 0;
    survey(seven, eight, &units_ok);
    CHECK(units_ok == kClosers);
    const std::uint64_t bound_by = b.ord;        // every bind point is at most this
    CHECK(seven[1] <= bound_by && eight[1] <= bound_by);

    // A point past every bind: the cycle-7 position re-allows the cycle-7
    // closes -- all of them -- and leaves the cycle-8 closes where they were.
    const std::uint64_t first = b.ord + 1;
    const std::uint64_t eight_low = eight[0];
    const std::uint64_t eight_high = eight[1];
    b.core.refresh_point_allowances(first, long7);
    survey(seven, eight, &units_ok);
    CHECK(seven[0] == first && seven[1] == first);       // every cycle-7 close, in or out of the heap
    CHECK(eight[0] == eight_low && eight[1] == eight_high);
    CHECK(units_ok == kClosers);                          // and the allowance is the full one

    // A flat position re-allows no close.
    b.core.refresh_point_allowances(first + 1, flat);
    survey(seven, eight, &units_ok);
    CHECK(seven[0] == first && seven[1] == first);
    CHECK(eight[0] == eight_low && eight[1] == eight_high);

    // The cycle-8 position now re-allows the other half, at a later point.
    const std::uint64_t second = first + 2;
    b.core.refresh_point_allowances(second, long8);
    survey(seven, eight, &units_ok);
    CHECK(seven[0] == first && seven[1] == first);
    CHECK(eight[0] == second && eight[1] == second);
    // The cycle-7 position at that later point re-allows the cycle-7 closes
    // again (they stood at `first`); the cycle-8 closes, already at `second`,
    // stay.
    b.core.refresh_point_allowances(second, long7);
    survey(seven, eight, &units_ok);
    CHECK(seven[0] == second && seven[1] == second);
    CHECK(eight[0] == second && eight[1] == second);
    CHECK(units_ok == kClosers);
    // The refresh changed no row's identity or place.
    const std::vector<no::RequestHandle> now = queue(b.core);
    CHECK(now.size() == kClosers);
    std::size_t in_order = 0;
    for (std::size_t i = 0; i != kClosers; ++i) {
        const no::RequestHandle expect = i % 2 == 0 ? sevens[i / 2] : eights[i / 2];
        if (now[i] == expect) ++in_order;
    }
    CHECK(in_order == kClosers);
}

// A spill that has to be written and cannot be (the scratch directory is gone)
// fails the command that needed the room, before that command changes
// anything: the book, the journal and the counters are as they were. The same
// submit then succeeds once the directory is back, and the book reads back as
// one that never failed.
void unwritable_spill_fails_before_commit() {
    Book b;
    std::vector<no::RequestHandle> order;
    order.reserve(12000);
    const no::Request request = limit(1.0, 50.0, "q");
    bool refused = false;
    std::size_t size_before = 0;
    std::size_t end_before = 0;
    std::uint64_t ord_before = 0;
    std::uint64_t inc_before = 0;
    std::uint64_t last_before = 0;
    {
        const TempDirectory broken("/pineforge-o1e2-no-such-directory");
        for (std::size_t i = 0; i != 100000; ++i) {
            size_before = b.core.live().size();
            end_before = b.core.history_end();
            ord_before = b.ord;
            inc_before = b.inc;
            last_before = b.core.last_ordinal();
            try {
                order.push_back(b.submit(request));
            } catch (const std::system_error&) {
                refused = true;
                break;
            }
        }
    }
    REQUIRE(refused);                       // a book this long has an external part
    CHECK(b.core.live().size() == size_before);
    CHECK(b.core.history_end() == end_before);                // no partial journal row
    CHECK(b.ord == ord_before && b.inc == inc_before);
    CHECK(b.core.last_ordinal() == last_before);
    CHECK(queue(b.core) == order);                            // no row lost, none changed
    // The same submit, now that the directory is back, and enough more that the
    // book is well outside the heap.
    for (std::size_t i = 0; i != 3000; ++i) order.push_back(b.submit(request));
    CHECK(b.core.live().size() == order.size());
    CHECK(queue(b.core) == order);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < order.size(); i += 5) {
        const no::LiveRequest* row = b.core.find_live(order[i]);
        if (row == nullptr || !(row->handle() == order[i])) ++mismatches;
    }
    CHECK(mismatches == 0);
}
}  // namespace

int main() {
    try {
        // Warm the caches the first measured run would otherwise charge to itself.
        (void) book_bytes(64, 16);
        // Q well past the book's resident part (a few thousand rows), a very
        // different Q, and the same number of re-prices spread over each.
        const BookRun small = book_bytes(8192, 2048);
        const BookRun large = book_bytes(32768, 2048);
        std::printf("E02: retained %zu -> %zu bytes for %zu -> %zu live requests\n", small.retained,
                    large.retained, small.population, large.population);
        CHECK(small.population == 8192 && large.population == 32768);
        constexpr std::size_t kSlack = 2u << 20;   // 2 MiB: the resident part's width, twice over
        CHECK(large.retained <= small.retained + kSlack);
        exercise_book(12000);
        refresh_reaches_spilled_rows();
        unwritable_spill_fails_before_commit();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "o1-e2 live-book fixtures: %s\n", error.what());
    }
    std::printf("test_native_o1_live_book: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
