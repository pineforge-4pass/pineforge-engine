// 1.7.0 review F-NEW-01 (argon read of the lane b1 series head 5454eba7be): a
// resident winner the matcher's full scan holds is still that request's row
// after the trail observation that runs before it is used.
//
// The working book keeps at most 4,096 rows resident (kBookMax in
// src/native_order.cpp); a push past the bound spills the newest resident row
// (make_book_room), and a command that names a spilled row brings it back
// (classify -> promote_book_row), which first spills the heap's back row
// (live_.pop_back) and then inserts the promoted row at its priority place. A
// full scan of match_path holds its winner with book_row.hold(), which for a
// resident row is an unowning alias of live_[cursor_.r]. When the winner lies
// past the cursor on a continuous segment, observe_trails runs before that
// alias is read, and a spilled trail whose running best improves at the
// winner's price is promoted: the slot the alias names is popped and refilled
// with the trail, so the matcher fills (or refuses) the wrong request; here it
// throws "native nonfillable state reached matched coordinator".
//
// The run (argon's scenario). Bar 1 (flat at 100) places, in this order:
// request 1, a buy StopLimit{stop 100.25, limit 90}; R resting buy limits at
// 50 that the tape never reaches; the trail, a sell Trail{offset 5, armed,
// best_seed 100.25}; the limit, a sell Limit{100.5}. With R = 4,094 the book
// is 4,097 rows, one past the bound: the trail is the row the last push
// spilled and the limit stands at live_[4095]. Bar 2 rises 100 -> 101. Every
// row is refreshed at the point's start in (t, priority) order (refresh_allowance
// classifies, so the trail is promoted and the limit spilled, then the limit is
// promoted and the trail, already tracking at best 100.25, spilled again). At
// t = 0.25 request 1 activates (100.25 does not improve the trail's best); the
// full scan after it picks the limit at t = 0.5 and holds live_[4095]; the
// observation at 100.5 improves the spilled trail and promotes it into that
// slot. The run of R = 4,000 is the same scenario with the book wholly
// resident: the observation moves no row there.
//
// What the contract says, wherever the rows live: the run completes; the
// limit fills exactly once, at 100.5, on bar 2's path (after bar 1's close
// callback, before bar 2's); nothing else fills; the trail armed on bar 2 and
// its running best reached bar 2's high, 101 (so the observation that
// promotes it ran). The fill and the trail's activation take the same
// ordinals after the last bar-1 submission as in the resident run: the rows
// between the two runs differ only in resting buy limits, whose refreshes
// take no ordinal (refresh_allowance, src/native_order.cpp), so every
// expected value is the resident control run's own, read inside this test.
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
#include <optional>
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

// The book's resident bound (src/native_order.cpp: kBookMax).
constexpr std::size_t kResidentRows = 4096;

// Requests other than the resting limits: request 1, the trail, the limit.
constexpr std::size_t kNamedRows = 3;
// One row past the bound: the trail is spilled and the limit is the heap's
// back row (live_[kResidentRows - 1]).
constexpr std::size_t kSpilled = kResidentRows + 1 - kNamedRows;
// The same scenario wholly resident.
constexpr std::size_t kResident = 4000;

constexpr double kStop = 100.25;       // request 1's stop and the trail's seed
constexpr double kStopLimit = 90.0;    // request 1's limit once active: never reached
constexpr double kRestLevel = 50.0;    // the resting buys: never reached
constexpr double kTrailOffset = 5.0;   // the trail's stop rides far below the tape
constexpr double kSellLimit = 100.5;   // reached at t = 0.5 of bar 2's rise
constexpr double kBar2High = 101.0;

struct Fill {
    std::string label;
    double price = 0.0;
    double opened = 0.0;
    double closed = 0.0;
    std::uint64_t ordinal = 0;
    int closes_seen = 0;  // bar-close callbacks before the fill
};

struct Outcome {
    std::size_t book = 0;              // live rows once bar 1 placed them
    std::size_t rejected = 0;          // placements the kernel refused
    std::uint64_t last_submit = 0;     // the limit's submission ordinal (bar 1)
    std::vector<Fill> fills;
    bool trail_read = false;           // bar 2's close callback found the trail live
    bool trail_activated = false;
    double trail_best = 0.0;
    std::uint64_t trail_activation = 0;
    bool completed = false;
    std::string error;
};

class AliasHost final : public NativeStrategyHost {
public:
    explicit AliasHost(std::size_t resting) : resting_(resting) {}

    Outcome outcome;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++closes_;
        if (closes_ == 1) {
            no::StopLimit stop_limit;
            stop_limit.stop = kStop;
            stop_limit.limit = kStopLimit;
            no::Request activate{no::Transact{1.0}, "activate", ""};
            activate.trigger = stop_limit;
            place(activate);
            for (std::size_t i = 0; i != resting_; ++i) {
                no::Request rest{no::Transact{1.0}, "rest", ""};
                rest.trigger = no::Limit{kRestLevel};
                place(rest);
            }
            no::Trail ride;
            ride.offset = kTrailOffset;
            ride.best_seed = kStop;  // armed (no arm_price): tracks from bar 2's open
            no::Request trail{no::Transact{-1.0}, "trail", ""};
            trail.trigger = ride;
            trail_ = place(trail).handle;
            no::Request limit{no::Transact{-1.0}, "limit", ""};
            limit.trigger = no::Limit{kSellLimit};
            outcome.last_submit = place(limit).event_ordinal;
            outcome.book = native_working_requests().size();
            return;
        }
        if (closes_ == 2 && trail_) {
            // Bar 2's whole path has run: the trail is still live (its stop
            // rides 5 below the best) and its best is where the path took it.
            if (const auto state = trail_state(*trail_)) {
                outcome.trail_read = true;
                outcome.trail_activated = state->activated;
                outcome.trail_best = state->best_price;
                outcome.trail_activation = state->activation_ordinal;
            }
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        Fill fill;
        fill.label = event.definition ? event.definition->request.label : std::string("?");
        fill.price = event.resolved_price;
        fill.opened = event.opened_units;
        fill.closed = event.closed_units;
        fill.ordinal = event.ordinal;
        fill.closes_seen = closes_;
        outcome.fills.push_back(fill);
    }

private:
    no::SubmitResult place(const no::Request& request) {
        no::SubmitResult result = submit(request);
        if (!result.handle) ++outcome.rejected;
        return result;
    }

    std::size_t resting_;
    int closes_ = 0;
    std::optional<no::RequestHandle> trail_;
};

std::vector<Bar> tape() {
    const std::int64_t t0 = 1736121600000LL;
    const double rows[4][4] = {
        {100.0, 100.0, 100.0, 100.0},  // bar 1: the book is placed at its close
        {100.0, 101.0, 100.0, 100.5},  // bar 2: 100 -> 101 crosses 100.25 then 100.5
        {100.5, 100.5, 100.5, 100.5},  // bar 3: nothing is reached
        {100.5, 100.5, 100.5, 100.5},
    };
    std::vector<Bar> bars;
    for (int index = 0; index != 4; ++index) {
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
    spec.identity = {"f-new-01-winner-alias-trail-promotion", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:FNEW01";
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
    AliasHost host(resting);
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
    CHECK(outcome.error.empty());
    CHECK(outcome.rejected == 0);
    CHECK(outcome.book == resting + kNamedRows);
    // The limit fills once, at its level, on bar 2's path; nothing else fills.
    CHECK(outcome.fills.size() == 1);
    if (outcome.fills.size() == 1) {
        const Fill& fill = outcome.fills[0];
        CHECK(fill.label == "limit");
        CHECK(fill.price == kSellLimit);
        CHECK(fill.opened != 0.0);
        CHECK(fill.closed == 0.0);
        CHECK(fill.closes_seen == 1);
        CHECK(fill.ordinal > outcome.last_submit);
    }
    // The trail armed on bar 2 and its best rode to bar 2's high: the
    // observation at the limit's price (the one that promotes a spilled
    // trail) and the one after it both ran.
    CHECK(outcome.trail_read);
    CHECK(outcome.trail_activated);
    CHECK(outcome.trail_best == kBar2High);
    CHECK(outcome.trail_activation > outcome.last_submit);
    if (outcome.fills.size() == 1) CHECK(outcome.trail_activation < outcome.fills[0].ordinal);
    for (const Fill& fill : outcome.fills) {
        std::printf("%s: fill %s at %.2f opened %.4f closed %.4f ordinal +%llu\n", name,
                    fill.label.c_str(), fill.price, fill.opened, fill.closed,
                    static_cast<unsigned long long>(fill.ordinal - outcome.last_submit));
    }
    std::printf("%s: trail best %.2f activated at ordinal +%llu\n", name, outcome.trail_best,
                static_cast<unsigned long long>(outcome.trail_activation - outcome.last_submit));
}

}  // namespace

int main(int argc, char** argv) {
    // The twin row _spilled_only is its own executable, built from this body
    // with PINEFORGE_B1_TWIN_FLAG (tests/cr150.d/s4e-fix-b1-alias.cmake).
#ifdef PINEFORGE_B1_TWIN_FLAG
    const char* const flag = PINEFORGE_B1_TWIN_FLAG;
    (void) argc;
    (void) argv;
#else
    const char* const flag = argc > 1 ? argv[1] : "";
#endif
    const bool spilled_only = std::strcmp(flag, "--spilled-only") == 0;
    try {
        // Self-checks that the run reaches the site: the spilled book is one
        // row past the bound (the trail is the row its last push spilled, the
        // limit the heap's back row), and the control is wholly resident.
        static_assert(kSpilled + kNamedRows == kResidentRows + 1, "one row past the bound");
        static_assert(kResident + kNamedRows <= kResidentRows, "the control stays resident");
        const Outcome spilled = run(kSpilled);
        CHECK(spilled.book > kResidentRows);
        check_run(spilled, kSpilled, "spilled");
        if (!spilled_only) {
            const Outcome resident = run(kResident);
            CHECK(resident.book <= kResidentRows);
            check_run(resident, kResident, "resident");
            // The same fills, prices and ordinals (after each run's last
            // submission) wherever the rows live.
            CHECK(spilled.fills.size() == resident.fills.size());
            if (spilled.fills.size() == 1 && resident.fills.size() == 1) {
                const Fill& a = spilled.fills[0];
                const Fill& b = resident.fills[0];
                CHECK(a.label == b.label);
                CHECK(a.price == b.price);
                CHECK(a.opened == b.opened);
                CHECK(a.closed == b.closed);
                CHECK(a.closes_seen == b.closes_seen);
                CHECK(a.ordinal - spilled.last_submit == b.ordinal - resident.last_submit);
            }
            CHECK(spilled.trail_best == resident.trail_best);
            CHECK(spilled.trail_activated == resident.trail_activated);
            CHECK(spilled.trail_activation - spilled.last_submit
                  == resident.trail_activation - resident.last_submit);
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "winner alias after trail promotion: %s\n", error.what());
    }
    std::printf("test_native_match_winner_alias_after_trail_promotion%s: %d checks, %d failures\n",
                spilled_only ? " --spilled-only" : "", checks, failures);
    return failures == 0 ? 0 : 1;
}
