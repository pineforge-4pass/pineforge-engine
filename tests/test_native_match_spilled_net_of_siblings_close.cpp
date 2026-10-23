// 1.7.0 review F-01 (argon cross-read of lane b1): a matched close that
// stands outside the heap is read through a held row while the kernel walks
// the rest of the book for its sibling claims.
//
// The working book keeps at most 4,096 rows resident (kBookMax in
// src/native_order.cpp); a spilled row is read back as a copy that a ring of
// the last 8 such reads keeps alive (BookSpill::kSide). A bound book close is
// refreshed where it stands at every point (refresh_point_allowances), so it
// reaches the Fill path still spilled, and consume_matched_request looked it
// up with find_live: a bare pointer into that ring. A Reduce of
// ScopeFraction{0.5, NetOfSiblings} then sizes itself through
// sibling_claimed_units, which walks the whole book and reads every spilled
// row through the same ring, so with more than 8 rows outside the heap the
// close's copy was freed before consume_matched_request read it again
// (settles_in_one_fill, its pending deduction, capacity, intent and trigger
// state): AddressSanitizer reports a heap-use-after-free.
//
// The run: bar 1 buys 10 units at market (filled at bar 2's open). Bar 2
// places R resting buy limits the tape never reaches and, 50 rows before the
// end of the book, a 2-unit sibling close the tape never reaches and the
// NetOfSiblings close at 101, so in the run of R = 4,200 both closes sit
// outside the heap, with more than a ring's width of spilled rows around
// them. Bar 3 binds both closes and reaches nothing; bar 4 opens at 101. The
// run of R = 4,000 is the same book wholly resident. Wherever the rows live,
// the close claims half of what its sibling leaves: (10 - 2) * 0.5 = 4 units,
// at 101, and nothing else fills.
//
// `--spilled-only` runs the spilled book alone: the second ctest row, the
// cheap one a sanitizer build runs.
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
// (src/native_order.cpp: kBookMax, BookSpill::kSide).
constexpr std::size_t kResidentRows = 4096;
constexpr std::size_t kRing = 8;

constexpr double kLotUnits = 10.0;
constexpr double kSiblingUnits = 2.0;
constexpr double kNetLevel = 101.0;       // bar 4 opens here
constexpr double kSiblingLevel = 150.0;   // never reached
// Half of what the sibling leaves of the 10-unit book.
constexpr double kNetUnits = (kLotUnits - kSiblingUnits) * 0.5;

struct Fill {
    std::string label;
    double price = 0.0;
    double opened = 0.0;
    double closed = 0.0;
    std::uint64_t ordinal = 0;
};

struct Outcome {
    std::size_t book = 0;      // live rows once bar 2 placed them
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

no::Request limit_close(no::ReductionSize size, double price, const char* label) {
    no::Request request{no::Reduce{size}, label, ""};
    request.trigger = no::Limit{price};
    return request;
}

class NetHost final : public NativeStrategyHost {
public:
    explicit NetHost(std::size_t resting) : resting_(resting) {}

    Outcome outcome;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == 1) {
            place(no::Request{no::Transact{kLotUnits}, "lot", ""});
            return;
        }
        if (bar_ != 2) return;
        // Fifty rows before the end of the book: outside the heap in the
        // spilled run, with spilled rows on both sides of it.
        const std::size_t close_at = resting_ - 50;
        for (std::size_t i = 0; i != resting_; ++i) {
            if (i == close_at) {
                place(limit_close(no::ExplicitUnits{kSiblingUnits}, kSiblingLevel, "sibling"));
                place(limit_close(no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings},
                                  kNetLevel, "net"));
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
        fill.closed = event.closed_units;
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
    const double rows[5][4] = {
        {100.0, 100.0, 100.0, 100.0},   // bar 1: the lot is bought at its close
        {100.0, 100.0, 100.0, 100.0},   // bar 2: the lot fills; the book is placed
        {100.0, 100.0, 100.0, 100.0},   // bar 3: the closes bind; nothing is reached
        {101.0, 101.5, 101.0, 101.25},  // bar 4: opens at the NetOfSiblings level
        {101.0, 101.0, 101.0, 101.0},
    };
    std::vector<Bar> bars;
    for (int index = 0; index != 5; ++index) {
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
    spec.identity = {"f-01-net-of-siblings-ring", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:F01";
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
    NetHost host(resting);
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
    // The lot opens once; the NetOfSiblings close takes half of what the
    // sibling leaves, once, at its level; nothing else fills.
    CHECK(outcome.fills.size() == 2);
    if (outcome.fills.size() == 2) {
        CHECK(outcome.fills[0].label == "lot");
        CHECK(outcome.fills[0].opened == kLotUnits);
        CHECK(outcome.fills[1].label == "net");
        CHECK(outcome.fills[1].closed == kNetUnits);
        CHECK(outcome.fills[1].opened == 0.0);
        CHECK(outcome.fills[1].price == kNetLevel);
        CHECK(outcome.fills[0].ordinal < outcome.fills[1].ordinal);
    }
    for (const Fill& fill : outcome.fills) {
        std::printf("%s: fill %s at %.2f opened %.4f closed %.4f\n", name, fill.label.c_str(),
                    fill.price, fill.opened, fill.closed);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool spilled_only = argc > 1 && std::strcmp(argv[1], "--spilled-only") == 0;
    try {
        constexpr std::size_t kSpilled = 4200;
        // Self-checks that the run reaches the site: the closes are placed
        // past the resident bound (rows from index kResidentRows - 1 on, but
        // the newest, stand outside the heap), and more rows stand outside
        // it than the ring keeps.
        static_assert(kSpilled - 50 >= kResidentRows - 1, "the closes must spill");
        const Outcome spilled = run(kSpilled);
        CHECK(spilled.book > kResidentRows + kRing);
        check_run(spilled, kSpilled, "spilled");
        if (!spilled_only) {
            const Outcome resident = run(4000);
            CHECK(resident.book <= kResidentRows);      // the same book, wholly resident
            check_run(resident, 4000, "resident");
            // The close answers the same units at the same price wherever
            // the rows live.
            CHECK(spilled.fills.size() == resident.fills.size());
            if (spilled.fills.size() == 2 && resident.fills.size() == 2) {
                CHECK(spilled.fills[1].label == resident.fills[1].label);
                CHECK(spilled.fills[1].closed == resident.fills[1].closed);
                CHECK(spilled.fills[1].price == resident.fills[1].price);
            }
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "spilled net-of-siblings close: %s\n", error.what());
    }
    std::printf("test_native_match_spilled_net_of_siblings_close%s: %d checks, %d failures\n",
                spilled_only ? " --spilled-only" : "", checks, failures);
    return failures == 0 ? 0 : 1;
}
