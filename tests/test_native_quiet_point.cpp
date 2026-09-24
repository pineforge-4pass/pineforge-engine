// R5 lane D2-A (PQ1): a driver point with nothing live, matched in front of
// match_path, against match_path itself, bit for bit.
//
// NativeExecutionConsumer::match_quiet_point takes match_path's
// no-live-request branch before the call: it makes every test match_path
// makes before that branch -- a failed run, a CurrentExecution point, no spec,
// an FX-roll margin check that could act -- and hands the point to match_path
// whole if any one holds; otherwise it opens the point's epoch and, on a
// segment, applies the excursion to the open position, which is all
// match_path would have done. Three witnesses:
//
//   1. Randomized runs with the short-cut on and off
//      (NativeExecutionConsumer::set_quiet_point_match), each configuration on
//      fresh hosts, must agree on every value: the continuation hash at every
//      bar and every applied fill, the broker and stream hashes, every fill's
//      event, book, equity, lots and rows, every trade with its excursions,
//      every open lot's excursions, every event's kind and ordinal (driver
//      points included), and the hosts' own counters. K3's randomized books
//      (1 to 100 live requests; every intrabar path; calculate-on-fills, the
//      quantizing grid and every-modeled-point recalculation, whose budgets
//      read the point epochs; and PERF-L1's guards off) and PERF-L2's hosts
//      (fee forms, account FX, a stepping FX curve under the margin model,
//      stream driving, host-owned excursions).
//   2. The count: on a host that places nothing, every driver point the run
//      records is quiet and is matched by the short-cut (quiet_points() equals
//      the run's driver-point events), and by match_path with it off
//      (quiet_points() is zero); the randomized runs take it too.
//   3. The decline: a run whose staged FX curve meets a margin model is
//      offered an FX-roll check at every point, so the short-cut takes none.
//
// Fail-before: at the lane's base the consumer has no set_quiet_point_match,
// so this TU does not compile there (the lane report records the first
// diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"
#include "native_run_outcome_compare.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {
using k3_book::BookConfig;
using k3_book::Path;
using outcome_compare::Tally;

Tally tally;
#define CHECK(condition) OUTCOME_CHECK(tally, condition)

struct Switches {
    bool quiet = true;
    bool guards = true;
};

void apply(NativeStrategyHost& host, Switches switches) {
    auto& consumer = NativeExecutionConsumer::bound(host);
    consumer.set_quiet_point_match(switches.quiet);
    consumer.set_point_guards(switches.guards);
}

std::uint64_t quiet_points(const NativeStrategyHost& host) {
    return NativeExecutionConsumer::bound(host).quiet_points();
}

class SwitchedBookHost final : public k3_book::BookHost {
public:
    SwitchedBookHost(const BookConfig& config, Switches switches) : BookHost(config) {
        apply(*this, switches);
    }
};

class SwitchedFusedHost final : public l2_fused::FusedHost {
public:
    SwitchedFusedHost(const l2_fused::Config& config, Switches switches) : FusedHost(config) {
        apply(*this, switches);
    }
};

// Places nothing: nothing is ever live, so every driver point is quiet.
class IdleHost final : public NativeStrategyHost {
public:
    explicit IdleHost(Switches switches) { apply(*this, switches); }
    std::vector<std::uint64_t> trace;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        trace.push_back(native_continuation_hash());
    }
};

enum class Recalc { Default, EveryModeledPoint };

struct BookRun {
    k3_book::Outcome outcome;
    std::uint64_t excursions = 0;
    std::uint64_t quiet = 0;
};

BookRun run_book(const BookConfig& config, Recalc recalc, Switches switches) {
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    outcome_compare::walk_lower_bars(spec);
    if (recalc == Recalc::EveryModeledPoint) {
        spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
    }
    SwitchedBookHost host(config, switches);
    BookRun run;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        run.outcome.error = "configure_native refused the spec";
        return run;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    run.outcome = host.outcome;
    run.excursions = outcome_compare::excursion_digest(host, tape.bars.back().close);
    run.quiet = quiet_points(host);
    return run;
}

long book_quiet_total = 0;

void books_match_the_full_path() {
    int runs = 0;
    std::uint64_t seed = 61001;
    for (const int live : {1, 4, 12, 40, 100}) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 5; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 40 ? 25 : 60;
                config.path = path;
                config.calc_on_fills = variant == 1;
                config.quantize = variant == 2;
                const Recalc recalc = variant == 3 ? Recalc::EveryModeledPoint : Recalc::Default;
                // Variant 4 runs both computations with PERF-L1's guards off.
                const bool guards = variant != 4;
                const BookRun quick = run_book(config, recalc, {true, guards});
                const BookRun full = run_book(config, recalc, {false, guards});
                bool equal = outcome_compare::same(tally, quick.outcome, full.outcome);
                CHECK(quick.excursions == full.excursions);
                equal = equal && quick.excursions == full.excursions;
                CHECK(full.quiet == 0);
                book_quiet_total += static_cast<long>(quick.quiet);
                if (!equal) outcome_compare::describe(config, guards ? "" : "guards=off");
                ++runs;
            }
        }
    }
    std::printf("k3 books: %d configurations, %ld points matched by the short-cut\n", runs,
                book_quiet_total);
    // K3's books keep their working requests topped up, so few points are
    // quiet; the idle runs below take every point.
    CHECK(book_quiet_total > 400);
}

void fused_hosts_match_the_full_path() {
    int runs = 0;
    long quiet_total = 0;
    for (const auto& config : outcome_compare::fused_configs(62001)) {
        SwitchedFusedHost quick_host(config, {true, true});
        SwitchedFusedHost full_host(config, {false, true});
        const auto quick = outcome_compare::run_fused(quick_host, config, outcome_compare::as_is);
        const auto full = outcome_compare::run_fused(full_host, config, outcome_compare::as_is);
        if (!outcome_compare::same(tally, quick, full)) outcome_compare::describe(config, "");
        CHECK(quiet_points(full_host) == 0);
        const bool fx_roll_checks = config.fx == l2_fused::Fx::Curve && config.margin;
        if (fx_roll_checks) {
            // Witness 3: every point is offered the FX-roll check.
            CHECK(quiet_points(quick_host) == 0);
        } else {
            CHECK(quiet_points(quick_host) > 0);
        }
        quiet_total += static_cast<long>(quiet_points(quick_host));
        ++runs;
    }
    std::printf("l2 hosts: %d configurations, %ld points matched by the short-cut\n", runs,
                quiet_total);
}

void idle_runs_are_all_quiet() {
    int runs = 0;
    for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
        for (const bool every_point : {false, true}) {
            BookConfig config;
            config.seed = 63001 + static_cast<std::uint64_t>(runs);
            config.bars = 40;
            config.path = path;
            const k3_book::Tape tape = k3_book::make_tape(config);
            NativeRunSpec spec = k3_book::make_spec(config, tape);
            outcome_compare::walk_lower_bars(spec);
            if (every_point) spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
            IdleHost quick({true, true});
            IdleHost full({false, true});
            for (IdleHost* host : {&quick, &full}) {
                CHECK(host->configure_native(spec).status == NativeSetupStatus::Applied);
                host->run(tape.bars.data(), static_cast<int>(tape.bars.size()));
                CHECK(host->native_state().kind == NativeLifecycleKind::Completed);
            }
            std::uint64_t driver_points = 0;
            for (const auto& event : quick.native_events(0)) {
                if (event.kind == NativeEventKind::Driver) ++driver_points;
            }
            // Witness 2: every recorded driver point, and only with it on.
            CHECK(driver_points >= 4 * tape.bars.size());
            CHECK(quiet_points(quick) == driver_points);
            CHECK(quiet_points(full) == 0);
            CHECK(quick.trace == full.trace);
            CHECK(quick.native_continuation_hash() == full.native_continuation_hash());
            CHECK(quick.broker_state_hash() == full.broker_state_hash());
            CHECK(quick.native_events(0).size() == full.native_events(0).size());
            ++runs;
        }
    }
    std::printf("idle runs: %d configurations, every driver point quiet\n", runs);
}

}  // namespace

int main() {
    books_match_the_full_path();
    fused_hosts_match_the_full_path();
    idle_runs_are_all_quiet();
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_native_quiet_point: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_native_quiet_point: ok (%ld checks)\n", tally.checks);
    return 0;
}
