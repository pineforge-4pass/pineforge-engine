// R5 lane D2-A (PA): a host that declares no bar-open hook against a host
// whose bar-open hook is empty, bit for bit.
//
// NativeStrategyHost::declare_native_bar_open_hook(false) tells the kernel
// that on_native_bar_open does nothing. The consumer then makes no call
// (NativeExecutionConsumer::invoke_bar_open_callback) and makes what the
// call's boundary leaves behind for the kernel as the call would: the decision
// context, the point's epoch, the complete bar at the point's instant (an
// OpenOnly view restored), no frame, and the boundary's abort check and
// applied-notification drain. Three witnesses:
//
//   1. Randomized runs on two fresh hosts per configuration, one declaring no
//      hook and one whose hook is empty -- it counts its calls and touches
//      nothing the kernel reads -- must agree on every value: the continuation
//      hash at every bar and applied fill, what the host sees inside those
//      callbacks (the engine's current bar and the current execution point),
//      the broker-state hash at every calculation and the per-bar broker-hash
//      rows the kernel records, the
//      final continuation, broker and stream hashes, every fill's event,
//      book, equity, lots and rows, every trade with its excursions, every
//      event's kind and ordinal (driver points included) and the hosts' own
//      counters. K3's randomized books under every intrabar path with
//      calculate-on-fills, every-modeled-point recalculation (budgets read the
//      point epochs), the OpenOnly open-bar view and the quantizing grid; and
//      PERF-L2's hosts (the margin model's bar-open check and liquidations,
//      account FX and a stepping curve, stream driving, excursion owners).
//      The declaring host's hook is never called; the other one's is called
//      at every script bar.
//   2. A C host: a pf_native_callbacks_v1 without on_bar_open against one
//      whose on_bar_open returns 0. The same values; the first host's
//      consumer records the declaration its table made, the second's does not.
//   3. A declaration stands across runs and is withdrawn by the next one: a
//      host that declares, runs, declares the hook back and runs again is
//      called on the second run only.
//
// Fail-before: at the lane's base NativeStrategyHost has no
// declare_native_bar_open_hook, so this TU does not compile there (the lane
// report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "native_c_table_fixture.hpp"
#include "native_run_outcome_compare.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

using namespace pineforge;

namespace {
using k3_book::BookConfig;
using k3_book::Path;
using outcome_compare::Tally;

Tally tally;
#define CHECK(condition) OUTCOME_CHECK(tally, condition)

// What a host sees inside a callback besides its arguments: the engine's
// current bar and the current execution point. A fill at the open point is
// delivered right after the bar-open boundary, so this is where a boundary
// that left the bar behind would show.
std::uint64_t seen(const Bar& bar, const std::optional<NativeCurrentPointView>& point) {
    std::uint64_t h = 1469598103934665603ull;
    h = k3_book::fnv_f64(h, bar.open);
    h = k3_book::fnv_f64(h, bar.high);
    h = k3_book::fnv_f64(h, bar.low);
    h = k3_book::fnv_f64(h, bar.close);
    h = k3_book::fnv_f64(h, bar.volume);
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(bar.timestamp));
    h = k3_book::fnv_u64(h, point.has_value() ? 1u : 0u);
    if (point) {
        h = k3_book::fnv_f64(h, point->price);
        h = k3_book::fnv_u64(h, point->decision.coordinate.ordinal);
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(point->quote_kind));
        h = k3_book::fnv_u64(h, point->quote_origin_ordinal);
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(point->decision.decision_floor_ms));
    }
    return h;
}

class OpenBookHost final : public k3_book::BookHost {
public:
    OpenBookHost(const BookConfig& config, bool declared) : BookHost(config) {
        if (declared) declare_native_bar_open_hook(false);
        set_broker_state_hash_recording(true);
    }
    long bar_open_calls = 0;
    std::vector<std::uint64_t> brokers;
    std::vector<std::uint64_t> sights;
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        ++bar_open_calls;
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        sights.push_back(seen(current_bar_, current_execution_point()));
        brokers.push_back(broker_state_hash());
        BookHost::on_native_bar(bar, context);
    }
    void on_native_applied(const native_order::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        sights.push_back(seen(current_bar_, current_execution_point()));
        BookHost::on_native_applied(event, context);
    }
    const std::vector<std::uint64_t>& rows() const { return broker_state_hashes_; }
};

class OpenFusedHost final : public l2_fused::FusedHost {
public:
    OpenFusedHost(const l2_fused::Config& config, bool declared) : FusedHost(config) {
        if (declared) declare_native_bar_open_hook(false);
    }
    long bar_open_calls = 0;
    std::vector<std::uint64_t> sights;
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        ++bar_open_calls;
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        sights.push_back(seen(current_bar_, current_execution_point()));
        FusedHost::on_native_bar(bar, context);
    }
    void on_native_applied(const native_order::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        sights.push_back(seen(current_bar_, current_execution_point()));
        FusedHost::on_native_applied(event, context);
    }
};

enum class Variant { Plain, CalcOnFills, EveryPoint, OpenOnly, Quantize };

const char* variant_name(Variant variant) {
    switch (variant) {
    case Variant::Plain: return "plain";
    case Variant::CalcOnFills: return "calc-on-fills";
    case Variant::EveryPoint: return "every-modeled-point";
    case Variant::OpenOnly: return "open-only";
    case Variant::Quantize: return "quantize";
    }
    return "?";
}

struct BookRun {
    k3_book::Outcome outcome;
    std::vector<std::uint64_t> sights;
    std::vector<std::uint64_t> brokers;
    std::vector<std::uint64_t> rows;
    std::uint64_t excursions = 0;
    long calls = 0;
};

BookRun run_book(BookConfig config, Variant variant, bool declared) {
    config.calc_on_fills = variant == Variant::CalcOnFills;
    config.quantize = variant == Variant::Quantize;
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    outcome_compare::walk_lower_bars(spec);
    // The kernel records the curve, and with it one broker-hash row per bar.
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    if (variant == Variant::EveryPoint) {
        spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
    }
    if (variant == Variant::OpenOnly) spec.open_bar_view = NativeOpenBarView::OpenOnly;
    OpenBookHost host(config, declared);
    BookRun run;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        run.outcome.error = "configure_native refused the spec";
        return run;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    run.outcome = host.outcome;
    run.sights = host.sights;
    run.brokers = host.brokers;
    run.rows = host.rows();
    run.excursions = outcome_compare::excursion_digest(host, tape.bars.back().close);
    run.calls = host.bar_open_calls;
    return run;
}

void books_match_an_empty_hook() {
    int runs = 0;
    long calls = 0;
    std::uint64_t seed = 71001;
    for (const int live : {1, 6, 25, 80}) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (const Variant variant : {Variant::Plain, Variant::CalcOnFills, Variant::EveryPoint,
                                          Variant::OpenOnly, Variant::Quantize}) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 25 ? 30 : 60;
                config.path = path;
                const BookRun declared = run_book(config, variant, true);
                const BookRun empty = run_book(config, variant, false);
                bool equal = outcome_compare::same(tally, declared.outcome, empty.outcome);
                CHECK(declared.sights == empty.sights);
                CHECK(declared.brokers == empty.brokers);
                CHECK(declared.rows == empty.rows);
                CHECK(declared.excursions == empty.excursions);
                equal = equal && declared.sights == empty.sights
                    && declared.brokers == empty.brokers && declared.rows == empty.rows
                    && declared.excursions == empty.excursions;
                CHECK(declared.calls == 0);
                CHECK(empty.calls == config.bars);
                CHECK(empty.rows.size() == static_cast<std::size_t>(config.bars));
                if (!equal) outcome_compare::describe(config, variant_name(variant));
                calls += empty.calls;
                ++runs;
            }
        }
    }
    std::printf("k3 books: %d configurations, %ld bar-open calls made by one host and "
                "skipped for the other\n", runs, calls);
}

void fused_hosts_match_an_empty_hook() {
    int runs = 0;
    for (const auto& config : outcome_compare::fused_configs(72001)) {
        OpenFusedHost declared_host(config, true);
        OpenFusedHost empty_host(config, false);
        const auto declared =
            outcome_compare::run_fused(declared_host, config, outcome_compare::as_is);
        const auto empty = outcome_compare::run_fused(empty_host, config, outcome_compare::as_is);
        const bool equal = outcome_compare::same(tally, declared, empty);
        CHECK(declared_host.sights == empty_host.sights);
        if (!equal || declared_host.sights != empty_host.sights) {
            outcome_compare::describe(config, "");
        }
        CHECK(declared_host.bar_open_calls == 0);
        CHECK(empty_host.bar_open_calls > 0);
        ++runs;
    }
    std::printf("l2 hosts: %d configurations\n", runs);
}

void c_tables_match_an_empty_hook() {
    int runs = 0;
    for (std::uint64_t seed = 73001; seed < 73011; ++seed) {
        BookConfig config;
        config.seed = seed;
        config.bars = 120;
        const k3_book::Tape tape = k3_book::make_tape(config);
        c_table::Hooks without;
        c_table::Hooks with;
        with.bar_open = true;
        const auto absent = c_table::run(tape, seed, without);
        const auto empty = c_table::run(tape, seed, with);
        CHECK(absent.rc == PF_NATIVE_OK);
        CHECK(empty.rc == PF_NATIVE_OK);
        CHECK(absent.completed);
        CHECK(empty.completed);
        CHECK(absent.trace == empty.trace);
        CHECK(absent.continuation == empty.continuation);
        CHECK(absent.broker == empty.broker);
        CHECK(absent.trades == empty.trades);
        CHECK(absent.trades_digest == empty.trades_digest);
        CHECK(absent.events == empty.events);
        CHECK(absent.events_digest == empty.events_digest);
        CHECK(absent.applied == empty.applied);
        CHECK(absent.bars == empty.bars);
        // The declaration the table made, and the hook the other one keeps.
        CHECK(!absent.declared_bar_open);
        CHECK(empty.declared_bar_open);
        CHECK(absent.bar_opens == 0);
        CHECK(empty.bar_opens == static_cast<long>(tape.bars.size()));
        CHECK(empty.trades > 0);
        ++runs;
    }
    std::printf("c tables: %d configurations\n", runs);
}

void a_declaration_stands_until_the_next() {
    BookConfig config;
    config.seed = 74001;
    config.live = 6;
    config.bars = 40;
    const k3_book::Tape tape = k3_book::make_tape(config);
    OpenBookHost host(config, true);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    CHECK(host.bar_open_calls == 0);
    host.declare_native_bar_open_hook(true);
    spec.identity.run_number = 2;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    CHECK(host.bar_open_calls == config.bars);
}

}  // namespace

int main() {
    books_match_an_empty_hook();
    fused_hosts_match_an_empty_hook();
    c_tables_match_an_empty_hook();
    a_declaration_stands_until_the_next();
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_native_bar_open_hook: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_native_bar_open_hook: ok (%ld checks)\n", tally.checks);
    return 0;
}
