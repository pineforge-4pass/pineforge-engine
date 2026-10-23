// 1.7.0 review B1-01: the matcher never reads a row of the spilled book
// through a pointer it saved before the walk moved on.
//
// The working book keeps at most 4,096 rows resident (kBookMax in
// src/native_order.cpp); its newest rows wait outside the heap, and a spilled
// row is read back as a copy that a ring of the last 8 such reads keeps alive
// (BookSpill::kSide). match_path saved a pointer to every row of the book
// before it evaluated any of them, so in a book of more than 4,096 + 8 rows
// every spilled row but the last 8 was freed before the candidate loop read
// it: a later row's copy could take its memory, so one row was evaluated
// twice and another never (AddressSanitizer: heap-use-after-free).
//
// The run: bar 1 places R resting buy limits the tape never reaches and, 50
// rows before the end of the book, two reachable buy limits, so in the run of
// R = 4,200 both sit outside the heap and outside the ring; bar 2 dips through
// both reachable levels and nowhere near a resting one. The run of R = 4,000
// is the same book wholly resident. Where the rows live changes nothing the
// matcher may answer: each reachable order fills once, in queue order, at the
// price the resident book gives it, no resting order fills, and the two
// fills' events stand the same number of ordinals apart. Under the sanitizers
// profile the freed-row read is a heap-use-after-free report as well.
//
// `--spilled-only` runs the spilled book alone (its own expectations, no
// resident comparison): the second ctest row, the cheap one a sanitizer
// build runs.
//
// Source-free, and only the public NativeStrategyHost API of the previous
// epoch, so this TU compiles against the unfixed head.
#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// The book's resident bound and the ring of recent spilled reads
// (src/native_order.cpp: kBookMax, BookSpill::kSide). A book of more rows
// than both together holds spilled rows the ring no longer keeps.
constexpr std::size_t kResidentRows = 4096;
constexpr std::size_t kRing = 8;

// The two reachable levels, below bar 1's price and above bar 2's low.
constexpr double kReachA = 99.75;
constexpr double kReachB = 99.25;

struct Fill {
    std::string label;
    double price = 0.0;
    double opened = 0.0;
    std::uint64_t ordinal = 0;
};

struct Outcome {
    std::size_t book = 0;      // live rows once bar 1 placed them
    std::size_t rejected = 0;  // placements the kernel refused
    std::vector<Fill> fills;
    bool completed = false;
    std::string error;
};

no::Request limit_buy(double price, const char* label) {
    no::Request request{no::Transact{1.0}, label, ""};
    request.trigger = no::Limit{price};
    return request;
}

class SpillHost final : public NativeStrategyHost {
public:
    explicit SpillHost(std::size_t resting) : resting_(resting) {}

    Outcome outcome;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ != 1) return;
        // Fifty rows before the end of the book: outside the heap in the
        // spilled run, and more than a ring's width from its newest row.
        const std::size_t reach_at = resting_ - 50;
        for (std::size_t i = 0; i != resting_; ++i) {
            if (i == reach_at) {
                place(limit_buy(kReachA, "reach-a"));
                place(limit_buy(kReachB, "reach-b"));
            }
            // Far below anything the tape trades; ids are unique per submit.
            place(limit_buy(50.0 - static_cast<double>(i % 100) * 0.25, "rest"));
        }
        outcome.book = native_working_requests().size();
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        Fill fill;
        fill.label = event.definition ? event.definition->request.label : std::string("?");
        fill.price = event.resolved_price;
        fill.opened = event.opened_units;
        fill.ordinal = event.ordinal;
        outcome.fills.push_back(fill);
    }

private:
    void place(const no::Request& request) {
        if (!submit(request).handle) ++outcome.rejected;
    }

    std::size_t resting_;
    int bar_ = 0;
};

std::vector<Bar> tape() {
    const std::int64_t t0 = 1736121600000LL;
    const double rows[3][4] = {
        {100.0, 100.0, 100.0, 100.0},   // bar 1: the book is placed at its close
        {100.0, 100.0, 99.0, 99.5},     // bar 2: dips through both reachable levels
        {99.5, 99.5, 99.5, 99.5},
    };
    std::vector<Bar> bars;
    for (int index = 0; index != 3; ++index) {
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
    spec.identity = {"b1-01-spilled-snapshot", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:B101";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    return spec;
}

Outcome run(std::size_t resting) {
    SpillHost host(resting);
    const auto setup = host.configure_native(spec());
    if (setup.status != NativeSetupStatus::Applied) {
        host.outcome.error = "configure_native refused the spec";
        return host.outcome;
    }
    const std::vector<Bar> bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    host.outcome.completed = host.native_state().kind == NativeLifecycleKind::Completed;
    host.outcome.error = host.last_error();
    return host.outcome;
}

// What every run must answer, wherever its rows live.
void check_run(const Outcome& outcome, std::size_t resting, const char* name) {
    if (!outcome.completed) std::fprintf(stderr, "%s: run did not complete: %s\n", name,
                                         outcome.error.c_str());
    CHECK(outcome.completed);
    CHECK(outcome.rejected == 0);
    CHECK(outcome.book == resting + 2);
    // Each reachable order fills exactly once, in queue order, for its unit;
    // no resting order fills.
    CHECK(outcome.fills.size() == 2);
    if (outcome.fills.size() == 2) {
        CHECK(outcome.fills[0].label == "reach-a");
        CHECK(outcome.fills[1].label == "reach-b");
        CHECK(outcome.fills[0].opened == 1.0);
        CHECK(outcome.fills[1].opened == 1.0);
        CHECK(outcome.fills[0].ordinal < outcome.fills[1].ordinal);
    }
    for (const Fill& fill : outcome.fills) {
        if (fill.label != "reach-a" && fill.label != "reach-b") {
            std::fprintf(stderr, "%s: unexpected fill %s at %.2f\n", name, fill.label.c_str(),
                         fill.price);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool spilled_only = argc > 1 && std::strcmp(argv[1], "--spilled-only") == 0;
    try {
        const Outcome spilled = run(4200);
        // The self-check that the walk really crosses the spill boundary: more
        // live rows than the resident part and the ring together.
        CHECK(spilled.book > kResidentRows + kRing);
        check_run(spilled, 4200, "spilled");
        if (!spilled_only) {
            const Outcome resident = run(4000);
            CHECK(resident.book <= kResidentRows);      // the same book, wholly resident
            check_run(resident, 4000, "resident");
            // The matcher answers the same fills, at the same prices, the same
            // number of events apart, wherever the rows live.
            CHECK(spilled.fills.size() == resident.fills.size());
            if (spilled.fills.size() == 2 && resident.fills.size() == 2) {
                for (std::size_t i = 0; i != 2; ++i) {
                    CHECK(spilled.fills[i].label == resident.fills[i].label);
                    CHECK(spilled.fills[i].price == resident.fills[i].price);
                    CHECK(spilled.fills[i].opened == resident.fills[i].opened);
                }
                CHECK(spilled.fills[1].ordinal - spilled.fills[0].ordinal
                      == resident.fills[1].ordinal - resident.fills[0].ordinal);
            }
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "spilled book snapshot: %s\n", error.what());
    }
    std::printf("test_native_match_spilled_book_snapshot%s: %d checks, %d failures\n",
                spilled_only ? " --spilled-only" : "", checks, failures);
    return failures == 0 ? 0 : 1;
}
