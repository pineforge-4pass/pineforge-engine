// R5 lane D2-C: the consumer's in-place reads of a run's state answer what
// NativeStrategyHost's accessors answer, wherever a host can ask.
//
// The Pine layer reads the run's lifecycle, spec and phase, the callback's
// point and the book through the bound consumer, in place
// (NativeExecutionConsumer::running / state_kind / state_spec / state_phase /
// current_point / position / bound; src/source/pine_host_reads.hpp), instead
// of through native_state(), current_execution_point(), physical_position()
// and execution_consumer(), which build their answers out of line and by
// value. Hosts over K3's randomized books and PERF-L2's hosts compare the two
// at every callback of every kind -- input, bar open, walked lower-timeframe
// sub-bar (Canonical labels), calculation, fill recalculation, applied fill
// -- inside the policy hooks the matcher consults (terms, precommit, lot
// excursions), right after a current execution a callback drives, and between
// callbacks: before configure, Ready, Running between stream inputs,
// Completed and Failed. Every read must agree field for field; the counts show
// each kind of read was taken (a present point, a running spec, a hook, each
// lifecycle kind).
//
// Fail-before: at the lane's base the consumer has none of these reads, so
// this TU does not compile there (the lane report records the first
// diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "native_run_outcome_compare.hpp"

#include "../src/native_execution_consumer.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {
using k3_book::BookConfig;
using k3_book::Path;
using outcome_compare::Tally;
namespace no = pineforge::native_order;

Tally tally;
#define CHECK(condition) OUTCOME_CHECK(tally, condition)

struct Counts {
    long compared = 0;
    long with_point = 0;
    long running = 0;
    long in_hooks = 0;
    long after_current = 0;
    long kinds[5] = {};
};
Counts counts;

std::uint64_t point_digest(const NativeCurrentPointView& point) {
    std::uint64_t h = 1469598103934665603ull;
    const NativeDecisionContext& d = point.decision;
    h = k3_book::fnv_u64(h, d.coordinate.ordinal);
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.coordinate.interval_index));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.coordinate.open_ms));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.coordinate.effective_time_ms));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.coordinate.source_price_time_ms));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.coordinate.provenance));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.coordinate.path_phase));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.decision_floor_ms));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.sub_index));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.sub_count));
    h = k3_book::fnv_u64(h, d.is_terminal_sub_bar ? 1u : 0u);
    h = k3_book::fnv_u64(h, d.in_session ? 1u : 0u);
    h = k3_book::fnv_u64(h, d.opens_session_day ? 1u : 0u);
    h = k3_book::fnv_u64(h, d.closes_session_day ? 1u : 0u);
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.sub_bar_open_ms));
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(d.script_bar_open_ms));
    h = k3_book::fnv_u64(h, d.driver_statistics.sample_ticks_processed);
    h = k3_book::fnv_f64(h, point.price);
    h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(point.quote_kind));
    return k3_book::fnv_u64(h, point.quote_origin_ordinal);
}

// Every in-place read of `host`'s consumer against the accessor it stands
// for. `accessor_consumer` is what host.execution_consumer() answered.
void compare_reads(const NativeStrategyHost& host, const IExecutionConsumer& accessor_consumer) {
    const NativeExecutionConsumer& consumer = NativeExecutionConsumer::bound(host);
    CHECK(&consumer == &as_native_consumer(accessor_consumer));
    const NativeStateView view = host.native_state();
    CHECK(consumer.running() == (view.kind == NativeLifecycleKind::Running));
    CHECK(consumer.state_kind() == view.kind);
    CHECK(consumer.state_spec() == view.spec);
    CHECK(consumer.state_phase() == view.phase);
    const auto point = host.current_execution_point();
    const NativeCurrentPointView* in_place = consumer.current_point();
    CHECK(point.has_value() == (in_place != nullptr));
    if (point && in_place) CHECK(point_digest(*point) == point_digest(*in_place));
    const NativePhysicalPosition position = host.physical_position();
    const NativePhysicalPosition book = consumer.position(host);
    CHECK(outcome_compare::bits(position.signed_units) == outcome_compare::bits(book.signed_units));
    CHECK(outcome_compare::bits(position.average_price)
          == outcome_compare::bits(book.average_price));
    CHECK(position.lot_count == book.lot_count);
    ++counts.compared;
    if (in_place) ++counts.with_point;
    if (consumer.running()) ++counts.running;
    ++counts.kinds[static_cast<int>(view.kind)];
}

// A K3 book that compares at every callback and hook, and executes a market
// request at a quarter of its calculations (reading again after it).
class ReadsBookHost final : public k3_book::BookHost {
public:
    explicit ReadsBookHost(const BookConfig& config)
        : BookHost(config), seed_(config.seed ^ 0x2545F4914F6CDD1Dull), current_rng_(seed_) {}
    void check() const { compare_reads(*this, execution_consumer()); }

    void on_native_run_begin() override {
        BookHost::on_native_run_begin();
        current_rng_ = k3_book::Rng(seed_);
        check();
    }
    void on_native_input(const Bar&, const NativeInputContext&) override { check(); }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override { check(); }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { check(); }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        check();
        BookHost::on_native_bar(bar, context);
        check();
        if (current_rng_.percent(25)) {
            no::Request market{no::Transact{current_rng_.percent(50) ? 1.0 : -1.0}, "d2c-now", ""};
            const auto submitted = submit(market);
            if (submitted.handle) {
                (void)execute_current(NativeCurrentExecution{*submitted.handle});
                check();
                ++counts.after_current;
            }
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        check();
        BookHost::on_native_applied(event, context);
    }
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        check();
        ++counts.in_hooks;
        return BookHost::resolve_execution_terms(facts);
    }
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView&) const override {
        check();
        ++counts.in_hooks;
        return NativePrecommitVerdict::Admit;
    }

private:
    std::uint64_t seed_;
    k3_book::Rng current_rng_;
};

// A PERF-L2 host (fee forms, FX, margin, excursion owners, streams) that
// compares at every callback and hook.
class ReadsFusedHost final : public l2_fused::FusedHost {
public:
    explicit ReadsFusedHost(const l2_fused::Config& config) : FusedHost(config) {}
    void check() const { compare_reads(*this, execution_consumer()); }

    void on_native_run_begin() override { FusedHost::on_native_run_begin(); check(); }
    void on_native_input(const Bar&, const NativeInputContext&) override { check(); }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override { check(); }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { check(); }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        check();
        FusedHost::on_native_bar(bar, context);
        check();
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        check();
        FusedHost::on_native_applied(event, context);
        check();
    }
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        check();
        ++counts.in_hooks;
        return FusedHost::validate_execution_precommit(view);
    }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& facts) const override {
        check();
        ++counts.in_hooks;
        return FusedHost::closed_lot_excursion(facts);
    }
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        check();
        ++counts.in_hooks;
        return FusedHost::resolve_execution_terms(facts);
    }
};

void books_read_in_place() {
    int runs = 0;
    std::uint64_t seed = 97001;
    for (const int live : {1, 6, 25}) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 3; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 25 ? 30 : 60;
                config.path = path;
                config.calc_on_fills = variant == 1;
                config.quantize = variant == 2;
                ReadsBookHost host(config);
                host.check();  // Unconfigured
                const auto outcome =
                    outcome_compare::run_book(host, config, outcome_compare::as_is);
                host.check();  // Completed
                CHECK(outcome.completed);
                ++runs;
            }
        }
    }
    std::printf("k3 books: %d configurations\n", runs);
}

void fused_hosts_read_in_place() {
    int runs = 0;
    for (const auto& config : outcome_compare::fused_configs(98001)) {
        ReadsFusedHost host(config);
        host.check();  // Unconfigured
        const auto outcome = outcome_compare::run_fused(host, config, outcome_compare::as_is);
        host.check();  // Completed
        CHECK(outcome.completed);
        ++runs;
    }
    std::printf("l2 hosts: %d configurations\n", runs);
}

// Ready, Running between stream inputs, and Failed, read from outside.
void every_lifecycle_reads_in_place() {
    l2_fused::Config config;
    config.seed = 99001;
    config.bars = 60;
    const l2_fused::Tape tape = l2_fused::make_tape(config);
    {
        ReadsFusedHost host(config);
        NativeRunSpec spec = l2_fused::make_spec(config, tape);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.check();  // Ready
        CHECK(host.stream_begin(tape.bars.data(), 20, "5", "5"));
        host.check();  // Running, realtime, between inputs
        for (std::size_t i = 20; i < tape.bars.size(); ++i) {
            CHECK(host.stream_push_bar(tape.bars[i]));
            host.check();
        }
        CHECK(host.stream_end());
        host.check();  // Completed
    }
    {
        ReadsFusedHost host(config);
        NativeRunSpec spec = l2_fused::make_spec(config, tape);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        // A second configure is a contract failure: the host is Failed.
        CHECK(host.configure_native(spec).status != NativeSetupStatus::Applied);
        host.check();
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    }
}

}  // namespace

int main() {
    books_read_in_place();
    fused_hosts_read_in_place();
    every_lifecycle_reads_in_place();
    std::printf("reads: %ld compared, %ld with a point, %ld running, %ld in hooks, %ld after a "
                "current execution; kinds unconfigured %ld ready %ld running %ld completed %ld "
                "failed %ld\n",
                counts.compared, counts.with_point, counts.running, counts.in_hooks,
                counts.after_current, counts.kinds[0], counts.kinds[1], counts.kinds[2],
                counts.kinds[3], counts.kinds[4]);
    CHECK(counts.with_point > 1000);
    CHECK(counts.in_hooks > 100);
    CHECK(counts.after_current > 50);
    for (const long kind : counts.kinds) CHECK(kind > 0);
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_native_in_place_reads: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_native_in_place_reads: ok (%ld checks)\n", tally.checks);
    return 0;
}
