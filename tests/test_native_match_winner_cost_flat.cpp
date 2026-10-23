// 1.7.0 review B1-05: what the matcher reads to find its winner's row does
// not grow with the book.
//
// Past its resident bound (kBookMax = 4,096 rows, src/native_order.cpp) the
// working book keeps its newest rows in an external ordered table, and
// WorkingRequestCore::live_at(rank) walks `rank` queue steps from the front,
// one table step for every spilled row it passes. match_path looked every
// winner up that way (its scanned_request), so a point with K fills paid K
// walks of the whole spilled part. The lookup a winner needs must be O(1) in
// the book: the walk of the scan holds the row it picked, and a row kept
// from an earlier scan is found without a walk from the front.
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

no::Request limit_buy(double price, const char* label) {
    no::Request request{no::Transact{1.0}, label, ""};
    request.trigger = no::Limit{price};
    return request;
}

struct Cost {
    std::size_t book = 0;
    std::size_t fills = 0;
    std::size_t rejected = 0;
    std::uint64_t steps = 0;    // table steps the core's lookups walked
    std::uint64_t rows = 0;     // spilled rows the core's lookups read back
    bool completed = false;
    std::string error;
};

class CostHost final : public NativeStrategyHost {
public:
    explicit CostHost(std::size_t resting) : resting_(resting) {}

    Cost cost;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ != 1) return;
        for (std::size_t i = 0; i != resting_; ++i) {
            place(limit_buy(50.0 - static_cast<double>(i % 100) * 0.25, "rest"));
        }
        for (std::size_t i = 0; i != kWinners; ++i) place(limit_buy(kReach, "reach"));
        cost.book = native_working_requests().size();
    }

    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        ++cost.fills;
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
    const auto reads =
            as_native_consumer(host.execution_consumer()).request_core().book_lookup_reads();
    host.cost.steps = reads.steps;
    host.cost.rows = reads.rows;
    return host.cost;
}

void check_run(const Cost& cost, std::size_t resting) {
    if (!cost.completed) std::fprintf(stderr, "N=%zu: %s\n", resting, cost.error.c_str());
    CHECK(cost.completed);
    CHECK(cost.rejected == 0);
    CHECK(cost.book == resting + kWinners);
    CHECK(cost.book > kResidentRows);          // the winners stand outside the heap
    CHECK(cost.fills == kWinners);
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
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "winner cost: %s\n", error.what());
    }
    std::printf("test_native_match_winner_cost_flat: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
