// R5 lane D2-A (PC): a host that declares no precommit hook against a host
// whose precommit hook admits every view, bit for bit.
//
// NativeStrategyHost::declare_native_precommit_hook(false) tells the kernel
// that validate_execution_precommit is the default admission. The consumer
// then does not consult it and does not build the settlement preview it would
// have been shown (NativeExecutionConsumer::consume_matched_request) -- except
// for a host that owns lot excursions, whose closed_lot_excursion every closing
// row of that preview consults: that host is asked exactly as often as before.
// The abort check that follows the hook stays. Four witnesses:
//
//   1. Randomized runs on two fresh hosts per configuration, one declaring no
//      hook and one whose hook counts its calls and admits, must agree on
//      every value: the continuation hash at every bar and applied fill, the
//      final continuation, broker and stream hashes, every fill's event, book,
//      equity, lots and rows, every excursion consultation's facts in order,
//      every trade, every event's kind and ordinal, and the hosts' own
//      counters. PERF-L2's hosts (every fill shape the one-lot settlement
//      takes and the staged chain's, fee forms, account FX and a stepping
//      curve, the margin model and its liquidations, streams, excursion
//      owners) and K3's randomized books, whose host also executes a market
//      request at its own calculation point (execute_current).
//   2. The counts (internal::count_settlement_paths): the admitting host's
//      runs build a preview for its fills, the declaring host's build none --
//      unless it owns lot excursions, when it builds exactly as many as the
//      admitting host. The declaring host's hook is never called.
//   3. C hosts: a pf_native_callbacks_v1 without on_precommit against one
//      whose on_precommit answers PF_NATIVE_ANSWER_DEFAULT, with and without
//      an on_lot_excursion whose answer depends on how often it was asked.
//      The same values and consultations; the first host's consumer records
//      the declaration its table made.
//   4. A declaration stands across runs and is withdrawn by the next one.
//
// Fail-before: at the lane's base NativeStrategyHost has no
// declare_native_precommit_hook, so this TU does not compile there (the lane
// report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "native_c_table_fixture.hpp"
#include "native_run_outcome_compare.hpp"

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

std::uint64_t preview_count() {
    const auto counts = internal::settlement_path_counts();
    const int preview = static_cast<int>(internal::SettlementEntry::Preview);
    return counts.fused[preview] + counts.staged[preview];
}

class PrecommitFusedHost final : public l2_fused::FusedHost {
public:
    PrecommitFusedHost(const l2_fused::Config& config, bool declared) : FusedHost(config) {
        if (declared) declare_native_precommit_hook(false);
    }
    mutable long precommit_calls = 0;
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView&) const override {
        ++precommit_calls;
        return NativePrecommitVerdict::Admit;
    }
};

class PrecommitBookHost final : public k3_book::BookHost {
public:
    PrecommitBookHost(const BookConfig& config, bool declared)
        : BookHost(config), seed_(config.seed ^ 0x165667B19E3779F9ull), current_rng_(seed_) {
        if (declared) declare_native_precommit_hook(false);
    }
    mutable long precommit_calls = 0;
    long current_applied = 0;
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView&) const override {
        ++precommit_calls;
        return NativePrecommitVerdict::Admit;
    }
    void on_native_run_begin() override {
        BookHost::on_native_run_begin();
        current_rng_ = k3_book::Rng(seed_);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        BookHost::on_native_bar(bar, context);
        // A market request consumed at this very calculation point.
        if (current_rng_.percent(25)) {
            no::Request market{no::Transact{current_rng_.percent(50) ? 1.0 : -1.0}, "d2a-now", ""};
            const auto submitted = submit(market);
            if (submitted.handle) {
                const auto result = execute_current(NativeCurrentExecution{*submitted.handle});
                if (std::holds_alternative<no::ExecutionAppliedEvent>(result)) ++current_applied;
            }
        }
    }

private:
    std::uint64_t seed_;
    k3_book::Rng current_rng_;
};

long total_previews = 0;

void fused_hosts_match_an_admitting_hook() {
    int runs = 0;
    for (const auto& config : outcome_compare::fused_configs(81001)) {
        PrecommitFusedHost declared_host(config, true);
        PrecommitFusedHost admitting_host(config, false);
        internal::count_settlement_paths(true);
        const auto declared =
            outcome_compare::run_fused(declared_host, config, outcome_compare::as_is);
        const std::uint64_t declared_previews = preview_count();
        internal::count_settlement_paths(true);
        const auto admitting =
            outcome_compare::run_fused(admitting_host, config, outcome_compare::as_is);
        const std::uint64_t admitting_previews = preview_count();
        internal::count_settlement_paths(false);
        if (!outcome_compare::same(tally, declared, admitting)) {
            outcome_compare::describe(config, "");
        }
        CHECK(admitting.applied > 0);
        CHECK(admitting_previews > 0);
        CHECK(admitting_host.precommit_calls > 0);
        CHECK(declared_host.precommit_calls == 0);
        if (config.owns_excursions) {
            // The preview's closing rows consult the owner: it stays.
            CHECK(declared_previews == admitting_previews);
            CHECK(!declared.excursions.empty());
        } else {
            CHECK(declared_previews == 0);
        }
        total_previews += static_cast<long>(admitting_previews);
        ++runs;
    }
    std::printf("l2 hosts: %d configurations, %ld previews built for the admitting host\n",
                runs, total_previews);
}

void books_match_an_admitting_hook() {
    int runs = 0;
    long current = 0;
    std::uint64_t seed = 82001;
    for (const int live : {1, 6, 25, 80}) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 3; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 25 ? 30 : 60;
                config.path = path;
                config.calc_on_fills = variant == 1;
                config.quantize = variant == 2;
                PrecommitBookHost declared_host(config, true);
                PrecommitBookHost admitting_host(config, false);
                internal::count_settlement_paths(true);
                const auto declared =
                    outcome_compare::run_book(declared_host, config, outcome_compare::as_is);
                const std::uint64_t declared_previews = preview_count();
                internal::count_settlement_paths(true);
                const auto admitting =
                    outcome_compare::run_book(admitting_host, config, outcome_compare::as_is);
                const std::uint64_t admitting_previews = preview_count();
                internal::count_settlement_paths(false);
                bool equal = outcome_compare::same(tally, declared, admitting);
                CHECK(declared_host.current_applied == admitting_host.current_applied);
                equal = equal && declared_host.current_applied == admitting_host.current_applied;
                if (!equal) outcome_compare::describe(config, "");
                CHECK(declared_previews == 0);
                CHECK(admitting_previews > 0);
                CHECK(declared_host.precommit_calls == 0);
                CHECK(admitting_host.precommit_calls > 0);
                current += admitting_host.current_applied;
                ++runs;
            }
        }
    }
    std::printf("k3 books: %d configurations, %ld current executions applied\n", runs, current);
    CHECK(current > 100);
}

void c_tables_match_a_default_answer() {
    int runs = 0;
    for (std::uint64_t seed = 83001; seed < 83011; ++seed) {
        for (const bool excursions : {false, true}) {
            BookConfig config;
            config.seed = seed;
            config.bars = 120;
            const k3_book::Tape tape = k3_book::make_tape(config);
            c_table::Hooks without;
            without.lot_excursion = excursions;
            c_table::Hooks with = without;
            with.precommit = true;
            const auto absent = c_table::run(tape, seed, without);
            const auto admitting = c_table::run(tape, seed, with);
            CHECK(absent.rc == PF_NATIVE_OK);
            CHECK(admitting.rc == PF_NATIVE_OK);
            CHECK(absent.completed);
            CHECK(admitting.completed);
            CHECK(absent.trace == admitting.trace);
            CHECK(absent.continuation == admitting.continuation);
            CHECK(absent.broker == admitting.broker);
            CHECK(absent.trades == admitting.trades);
            CHECK(absent.trades_digest == admitting.trades_digest);
            CHECK(absent.events == admitting.events);
            CHECK(absent.events_digest == admitting.events_digest);
            CHECK(absent.applied == admitting.applied);
            CHECK(absent.excursions == admitting.excursions);
            CHECK(!absent.declared_precommit);
            CHECK(admitting.declared_precommit);
            CHECK(absent.precommits == 0);
            CHECK(admitting.precommits > 0);
            CHECK(admitting.previews > 0);
            if (excursions) {
                CHECK(absent.previews == admitting.previews);
                CHECK(absent.excursions > 0);
            } else {
                CHECK(absent.previews == 0);
            }
            ++runs;
        }
    }
    std::printf("c tables: %d configurations\n", runs);
}

void a_declaration_stands_until_the_next() {
    l2_fused::Config config;
    config.seed = 84001;
    config.bars = 60;
    PrecommitFusedHost host(config, true);
    l2_fused::run(host, config);
    CHECK(host.precommit_calls == 0);
    CHECK(host.outcome.applied > 0);
    host.declare_native_precommit_hook(true);
    // A second run on the same host: the next run number, the same session.
    const l2_fused::Tape tape = l2_fused::make_tape(config);
    NativeRunSpec spec = l2_fused::make_spec(config, tape);
    spec.identity.run_number = 2;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    CHECK(host.precommit_calls > 0);
}

}  // namespace

int main() {
    fused_hosts_match_an_admitting_hook();
    books_match_an_admitting_hook();
    c_tables_match_a_default_answer();
    a_declaration_stands_until_the_next();
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_native_precommit_hook: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_native_precommit_hook: ok (%ld checks)\n", tally.checks);
    return 0;
}
