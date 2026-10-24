// 1.7.0 review B1-05: the matcher finds its winners' rows without walking the
// book. That is the whole claim: the bound the code gives (round 4, below) is
// O(1) for a winner its scan found outside the heap, one keyed search at most
// per point, and an uncounted O(log N) refresh path per spilled Evaluate
// winner per point, which is not O(1).
//
// Past its resident bound (kBookMax = 4,096 rows, src/native_order.cpp) the
// working book keeps its newest rows in an external ordered table, and
// WorkingRequestCore::live_at(rank) walks `rank` queue steps from the front,
// one table step for every spilled row it passes. match_path looked every
// winner up that way (its scanned_request), so a point with K fills paid K
// walks of the whole spilled part. The lookup a winner needs must walk no
// queue: the walk of the scan holds the row it picked, and a row kept from an
// earlier scan is found without a walk from the front.
//
// The run: bar 1 places N resting buy limits the tape never reaches, then
// K = 50 reachable buy limits (the newest rows, so outside the heap); bar 2
// dips through their level and all 50 fill at one point. N = 8,192 and
// N = 32,768. The core counts the table steps its lookups walk
// (WorkingRequestCore::book_lookup_reads; a walk of live() in queue order is
// not a lookup and is not counted), and the steps per winner must be the same
// for both books: a lookup that walks no queue does not depend on N.
//
// The counter is new with the fix, so against the unfixed head this TU does
// not compile (missing book_lookup_reads); the mutation control
// mutations/m3-B1-05.patch is the behavioural red: it restores the per-winner
// rank lookup and the steps per winner then differ between the two books.
//
// Round 2 (B1-05, O(1) per winner) adds the TOTAL lookup reads -- rows read
// back plus table steps -- and the refresh-reuse case. Every resting row's
// allowance belongs to the point it was last refreshed at, so at each point of
// bar 2 each row is first an Evaluate winner, oldest first: the matcher
// refreshes it and, the cursor unmoved, reuses the rows its last whole scan
// built (match_path, R5 lane PERF-K3). So every resting row past the resident
// bound is a winner reused after an allowance refresh in a spilled book, and
// the reads its reuse costs are counted once per spilled row per point. The
// O(1) reuse reads such a row at the table entry the scan found it at, which
// is no lookup; the O(log N) reuse looked it up by handle, one row read back
// per reuse, so its total grows with the book. Two windows are compared, both
// opening once bar 1 has placed the book: up to the first fill (the
// refresh-reuse case alone) and up to the end of the run (with the fills).
// Against b569c139e (O(log N) reuse) and under mutations/m7-B1-05-o1.patch,
// which looks the reused winner up by handle again, every reused spilled
// winner costs a lookup: at least one per spilled row per point.
//
// Round 4 (argon F2/F3 on c7e3cc256): the real bound, stated as the code
// gives it, and asserted only as far as the code gives it.
//  * O(1) in the book: the matcher's read of a winner its scan found outside
//    the heap (hold_scanned: one table entry and one row; no lookup).
//  * One keyed search, O(log N) table steps, counted as one row read back: a
//    winner its scan found resident that the refresh chain has spilled since.
//    Each row a refresh brings into the heap spills the heap's back row, so
//    that is the row standing at the back at the scan, at most one per point
//    -- none when a rebuild of the spilled table (BookSpill::compact) has made
//    the matcher rescan the book first. When a rebuild falls depends on how
//    much garbage the book's rewrites leave, so on N: the two books' totals
//    are NOT equal (argon traced 5 against 3 up to the first fill; over bar 2
//    the box reads 20,879 against 20,877, not argon's 54 against 52: see
//    round 6), and this row no longer asserts that they are.
//  * O(log N) per spilled Evaluate winner per point, not counted here: the
//    refresh the matcher then asks of the core (refresh_allowance: classify,
//    promote_book_row finds the row in the spilled table, make_book_room
//    spills the heap's back row into it). B1-05 makes the matcher's lookup
//    O(1); it does not make the refresh path O(1).
// What the counter counts (src/native_order.cpp, book_lookup_reads): a row
// read back per keyed search (spilled_held) or rank read (live_at,
// live_back), and a step per spilled row a rank walk passes (live_at,
// live_back) -- nothing else. A keyed search's own O(log N) DiskIdTable steps
// are not steps here, and the refresh path (promote_book_row, make_book_room)
// adds nothing to either. A walk of the book in queue order (live(),
// visit_book) reads its spilled rows without counting them.
//
// Round 6 (the box on da5271886: steps 0 and 0; refresh-reuse window 5 and 3
// rows; bar 2 20,879 and 20,877 rows). Bar 2's rows are not walks of the
// book: each is a keyed search by handle (hold_live -> spilled_held), and four
// readers make one here (src/native_execution_consumer.cpp):
//  * the provenance re-check at the head of every whole scan
//    (provenance_still_matches): one search per provenance row of a request
//    standing outside the heap. A whole scan adds a row for each Fill
//    candidate it finds and keeps the old ones; the list is cleared at the
//    next point. After fill m of K, each of the K - m requests still to fill
//    has m rows, so the K - 1 scans after the fills search
//    sum m(K - m) = K(K*K - 1)/6 = 20,825 rows;
//  * each fill's own read of its request (consume_matched_request): K = 50;
//  * the first Fill winner, kept from its winner-only rescan (book_at 0) and
//    spilled since by the next refresh (scanned_request): 1;
//  * the heap's back row at a point's first scan, spilled by the refresh
//    chain before its own reuse (scanned_request): at most one per point,
//    none where a table rebuild rescanned the book first -- 3 in the small
//    book (points 1-3), 1 in the large one (rebuilds at points 2 and 3).
// So bar 2 reads 20,876 rows in K alone plus the back rows (20,879 and
// 20,877), and the refresh-reuse window the back rows plus 2 (5 and 3). The
// count depends on K, not on N. Each is still a keyed search of O(log N)
// table steps the counter does not see, and the re-checks grow as K^3: a
// large constant for a point with K fills, not O(1).
// The real bound, then: O(1) lookup steps per winner (0 table steps in both
// books); bar 2's row reads are the fill point's keyed searches, a count in
// K, plus the back rows, and do not depend on N; the refresh path is O(log N)
// per spilled Evaluate winner per point and uncounted (round 4).
// What is asserted: no rank walk in either book (the table steps, equal: both
// none); in the refresh-reuse window, fewer keyed lookups than the spilled
// rows a single point reuses (book - kResidentRows), which a lookup per
// reused spilled winner cannot give; and over bar 2, totals within
// kBarPoints of each other both ways. Only the back rows depend on the book,
// at most one per matched point of bar 2 (its open and its three path
// segments). That needs no table rebuild among the K reachable rows'
// refreshes at the fill point: a rebuild's whole rescan there would add a
// provenance row for every reachable row refreshed so far, searched at every
// later head. Rebuilds fall about alive + 2 MiB / row bytes promotions apart
// (BookSpill::worth_compacting), more than one point's chain: here the large
// book rebuilds at promotions 15,070 of point 2 and 26,530 of point 3, the
// small one at none of points 1-3 (argon's trace). No equality of the
// totals, and nothing about the refresh path's cost, is asserted.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

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

constexpr std::size_t kResidentRows = 4096;   // kBookMax, src/native_order.cpp
constexpr std::size_t kWinners = 50;          // fills at one point
constexpr double kReach = 99.75;              // bar 2's low is below it
// Bar 2's matched points: its open and its three path segments
// (deliver_confirmed_script; close_execution is NextEligiblePoint).
constexpr std::uint64_t kBarPoints = 4;

no::Request limit_buy(double price, const char* label) {
    no::Request request{no::Transact{1.0}, label, ""};
    request.trigger = no::Limit{price};
    return request;
}

// The core's lookup reads at one moment of the run.
struct Reads {
    std::uint64_t rows = 0;     // spilled rows the core's lookups read back
    std::uint64_t steps = 0;    // table steps the core's lookups walked
};

// rows + steps read from `from` to `to`.
std::uint64_t total_between(const Reads& from, const Reads& to) {
    return (to.rows - from.rows) + (to.steps - from.steps);
}

struct Cost {
    std::size_t book = 0;
    std::size_t fills = 0;
    std::size_t rejected = 0;
    std::uint64_t steps = 0;    // table steps the core's lookups walked
    std::uint64_t rows = 0;     // spilled rows the core's lookups read back
    bool spilled_when_placed = false;
    Reads placed;               // once bar 1 has placed the book
    Reads first_fill;           // at the first fill of bar 2
    Reads end;                  // once the run is over
    bool completed = false;
    std::string error;
};

class CostHost final : public NativeStrategyHost {
public:
    explicit CostHost(std::size_t resting) : resting_(resting) {}

    Cost cost;

    // execution_consumer() is protected on the engine: read it from inside the host.
    auto lookup_reads() {
        return as_native_consumer(execution_consumer()).request_core().book_lookup_reads();
    }
    Reads reads_now() {
        const auto reads = lookup_reads();
        Reads now;
        now.rows = reads.rows;
        now.steps = reads.steps;
        return now;
    }
    bool book_spilled() {
        return as_native_consumer(execution_consumer()).request_core().book_spilled();
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ != 1) return;
        for (std::size_t i = 0; i != resting_; ++i) {
            place(limit_buy(50.0 - static_cast<double>(i % 100) * 0.25, "rest"));
        }
        for (std::size_t i = 0; i != kWinners; ++i) place(limit_buy(kReach, "reach"));
        cost.book = native_working_requests().size();
        cost.spilled_when_placed = book_spilled();
        cost.placed = reads_now();
    }

    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        if (++cost.fills == 1) cost.first_fill = reads_now();
    }

private:
    void place(const no::Request& request) {
        if (!submit(request).handle) ++cost.rejected;
    }

    std::size_t resting_;
    int bar_ = 0;
};

std::vector<Bar> tape() {
    const std::int64_t t0 = 1736121600000LL;
    const double rows[2][4] = {
        {100.0, 100.0, 100.0, 100.0},   // bar 1: the book is placed at its close
        {100.0, 100.0, 99.0, 99.5},     // bar 2: every reachable order fills
    };
    std::vector<Bar> bars;
    for (int index = 0; index != 2; ++index) {
        Bar bar{};
        bar.open = rows[index][0];
        bar.high = rows[index][1];
        bar.low = rows[index][2];
        bar.close = rows[index][3];
        bar.volume = 1.0;
        bar.timestamp = t0 + static_cast<std::int64_t>(index) * 300000;
        bars.push_back(bar);
    }
    return bars;
}

NativeRunSpec spec() {
    NativeRunSpec spec;
    spec.identity = {"b1-05-winner-cost", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:B105";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    return spec;
}

Cost run(std::size_t resting) {
    CostHost host(resting);
    const auto setup = host.configure_native(spec());
    if (setup.status != NativeSetupStatus::Applied) {
        host.cost.error = "configure_native refused the spec";
        return host.cost;
    }
    const std::vector<Bar> bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    host.cost.completed = host.native_state().kind == NativeLifecycleKind::Completed;
    host.cost.error = host.last_error();
    const auto reads = host.lookup_reads();
    host.cost.steps = reads.steps;
    host.cost.rows = reads.rows;
    host.cost.end = host.reads_now();
    return host.cost;
}

void check_run(const Cost& cost, std::size_t resting) {
    if (!cost.completed) std::fprintf(stderr, "N=%zu: %s\n", resting, cost.error.c_str());
    CHECK(cost.completed);
    CHECK(cost.rejected == 0);
    CHECK(cost.book == resting + kWinners);
    CHECK(cost.book > kResidentRows);          // the winners stand outside the heap
    CHECK(cost.spilled_when_placed);           // bar 2 reuses rows of a spilled book
    CHECK(cost.fills == kWinners);
}

void print_window(const char* name, const Cost& small, const Cost& large, const Reads& small_to,
                  const Reads& large_to) {
    std::printf("%s: rows %llu -> %llu, steps %llu -> %llu, total %llu -> %llu\n", name,
                static_cast<unsigned long long>(small_to.rows - small.placed.rows),
                static_cast<unsigned long long>(large_to.rows - large.placed.rows),
                static_cast<unsigned long long>(small_to.steps - small.placed.steps),
                static_cast<unsigned long long>(large_to.steps - large.placed.steps),
                static_cast<unsigned long long>(total_between(small.placed, small_to)),
                static_cast<unsigned long long>(total_between(large.placed, large_to)));
}

}  // namespace

int main() {
    try {
        const Cost small = run(8192);
        const Cost large = run(32768);
        check_run(small, 8192);
        check_run(large, 32768);
        std::printf("lookup steps %llu -> %llu, rows %llu -> %llu, for %zu winners\n",
                    static_cast<unsigned long long>(small.steps),
                    static_cast<unsigned long long>(large.steps),
                    static_cast<unsigned long long>(small.rows),
                    static_cast<unsigned long long>(large.rows), kWinners);
        // The table steps per winner do not depend on the book: the same for
        // a book four times the size.
        if (small.fills == kWinners && large.fills == kWinners) {
            CHECK(small.steps / kWinners == large.steps / kWinners);
            CHECK(small.steps % kWinners == large.steps % kWinners);
            // The reused spilled winners are not looked up: in the
            // refresh-reuse window (every spilled resting row a winner reused
            // after its refresh, up to the first fill) each book's keyed
            // lookups stay below the spilled rows one point reuses. Not equal
            // between the books: the row a refresh chain spills off the heap's
            // back is looked up once per point unless a table rebuild
            // rescanned first, and rebuilds fall by N.
            print_window("refresh-reuse window", small, large, small.first_fill, large.first_fill);
            print_window("bar 2", small, large, small.end, large.end);
            const std::uint64_t small_spilled = small.book - kResidentRows;
            const std::uint64_t large_spilled = large.book - kResidentRows;
            const std::uint64_t small_reuse = total_between(small.placed, small.first_fill);
            const std::uint64_t large_reuse = total_between(large.placed, large.first_fill);
            CHECK(small_reuse < small_spilled);
            CHECK(large_reuse < large_spilled);
            // Over the whole of bar 2 the reads are the fill point's keyed
            // searches -- K(K*K - 1)/6 provenance re-checks, the K fills' own
            // reads and the first Fill winner's, a count in K alone -- plus
            // the back rows (header, round 6). That is not below the small
            // book's spilled rows (K^3/6 is not), but it does not grow with
            // the book: only the back rows differ, at most one per matched
            // point, so a book four times the size reads at most kBarPoints
            // rows more, or fewer.
            const std::uint64_t small_bar = total_between(small.placed, small.end);
            const std::uint64_t large_bar = total_between(large.placed, large.end);
            const std::uint64_t bar_in_k = kWinners * (kWinners * kWinners - 1) / 6 + kWinners + 1;
            std::printf("bar 2 in K alone: %llu rows; back rows %lld -> %lld\n",
                        static_cast<unsigned long long>(bar_in_k),
                        static_cast<long long>(small_bar) - static_cast<long long>(bar_in_k),
                        static_cast<long long>(large_bar) - static_cast<long long>(bar_in_k));
            CHECK(large_bar <= small_bar + kBarPoints);
            CHECK(small_bar <= large_bar + kBarPoints);
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "winner cost: %s\n", error.what());
    }
    std::printf("test_native_match_winner_cost_flat: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
