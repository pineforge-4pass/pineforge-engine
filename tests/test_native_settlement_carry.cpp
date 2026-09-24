// R5 lane D2-C (stage once): a fill settled from the one-lot stage its
// precommit preview took, against the same fill staged again, bit for bit.
//
// A host that is shown a fill's settlement preview (it implements
// validate_execution_precommit, or owns lot excursions) had the fill staged
// twice: once for the preview and once more for the settlement, from the same
// action, fill, scope and book. NativeExecutionConsumer::consume_matched_request
// now keeps the preview's one-lot stage
// (BacktestEngine::NativeSettlementStage::OneLot::preview_keeping) and settles
// from it (settle_kept) when no fill settled in between -- the consumer's
// generation stamp. The close row is still built at the settlement, so an
// excursion owner is asked as before. set_settlement_carry(false) stages every
// settlement again, as before the lane. Four witnesses:
//
//   1. PERF-L2's hosts over outcome_compare::fused_configs -- every one-lot
//      shape and the staged chain's (partial closes, additions, selections),
//      the three fee forms, account FX (a constant rate and a curve that
//      steps mid-run), the margin model under both liquidation sizings, calc
//      on fills, streams, excursion owners whose answer depends on how often
//      they were asked -- and six more: the price grid, and books of mostly
//      partial closes with margin and excursion owners. A run with the carry
//      and one without agree on every value outcome_compare::same reads (the
//      continuation at every bar and fill; every fill's event, book, equity,
//      lots and rows; every precommit view; every excursion consultation in
//      order; every trade and event) and on the settlement path counts, entry
//      by entry. Every one-pass settlement of the carry run was taken from its
//      preview's stage; the other run took none.
//   2. K3's randomized books, whose host also executes a market request at its
//      own calculation point (execute_current), with the magnifier off and on
//      (a synthesized path, and a lower timeframe walked with Canonical
//      labels): the same.
//   3. A host whose precommit hook rewrites, at every call, a fee input or
//      the account rate -- the fee value, the fee kind, the point value, the
//      rate -- inside the pump, where the projection check waits for the
//      pump's end: the carried stages still settle exactly as the fills
//      staged again, because the fill carries the inspection's pinned ticket
//      and the stage only splits it by quantity. The carry is taken as often
//      as without the rewrites.
//   4. A host that declares no precommit hook and owns no excursions is shown
//      no preview, so nothing is carried.
//
// Fail-before: at the lane's base NativeExecutionConsumer has no
// set_settlement_carry, so this TU does not compile there (the lane report
// records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "native_run_outcome_compare.hpp"

#include "../src/engine_internal.hpp"
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

constexpr int kSettle = static_cast<int>(internal::SettlementEntry::Settle);

// What one run's settlements took: the path counts and the carried count.
struct Paths {
    internal::SettlementPathCounts counts;
    std::uint64_t carried = 0;
};

bool same_counts(const internal::SettlementPathCounts& a, const internal::SettlementPathCounts& b) {
    for (int entry = 0; entry < internal::kSettlementEntries; ++entry) {
        if (a.fused[entry] != b.fused[entry] || a.staged[entry] != b.staged[entry]) return false;
    }
    return true;
}

// Runs `host` with the carry on or off; `drive` runs it and answers its outcome.
template <class Host, class Drive>
auto run_with(Host& host, bool carry, Paths& paths, Drive drive) {
    NativeExecutionConsumer& consumer = NativeExecutionConsumer::bound(host);
    consumer.set_settlement_carry(carry);
    internal::count_settlement_paths(true);
    auto outcome = drive(host);
    paths.counts = internal::settlement_path_counts();
    internal::count_settlement_paths(false);
    paths.carried = consumer.carried_settlements();
    return outcome;
}

// Both runs of one configuration: the same values and path counts, and every
// one-pass settlement of the carry run taken from its preview's stage.
bool carried_alike(const Paths& on, const Paths& off, bool previewed) {
    const int before = tally.failures;
    CHECK(same_counts(on.counts, off.counts));
    CHECK(off.carried == 0);
    if (previewed) {
        CHECK(on.carried == on.counts.fused[kSettle]);
    } else {
        CHECK(on.carried == 0);
    }
    return tally.failures == before;
}

long total_carried = 0;
long total_settled = 0;

void fused_hosts_carry_their_stage() {
    std::vector<l2_fused::Config> configs = outcome_compare::fused_configs(91001);
    std::uint64_t seed = 91101;
    for (const l2_fused::Path path :
         {l2_fused::Path::None, l2_fused::Path::Synthesized, l2_fused::Path::Lower}) {
        l2_fused::Config grid;
        grid.seed = seed++;
        grid.bars = 90;
        grid.path = path;
        grid.quantize = true;
        grid.calc_on_fills = true;
        configs.push_back(grid);
        l2_fused::Config partial;
        partial.seed = seed++;
        partial.bars = 90;
        partial.path = path;
        partial.single_lot_percent = 30;
        partial.margin = true;
        partial.owns_excursions = true;
        partial.fx = l2_fused::Fx::Curve;
        partial.fee_kind = NativeFeeKind::CashPerUnit;
        partial.fee_value = 0.5;
        configs.push_back(partial);
    }
    int runs = 0;
    long excursion_runs = 0;
    for (const auto& config : configs) {
        l2_fused::FusedHost carry_host(config);
        l2_fused::FusedHost again_host(config);
        const auto drive = [&config](l2_fused::FusedHost& host) {
            return outcome_compare::run_fused(host, config, outcome_compare::as_is);
        };
        Paths on;
        Paths off;
        const auto carried = run_with(carry_host, true, on, drive);
        const auto staged = run_with(again_host, false, off, drive);
        bool equal = outcome_compare::same(tally, carried, staged);
        equal = carried_alike(on, off, true) && equal;
        CHECK(carried.applied > 0);
        CHECK(on.carried > 0);
        if (config.owns_excursions) {
            CHECK(!carried.excursions.empty());
            ++excursion_runs;
        }
        if (!equal) outcome_compare::describe(config, "");
        total_carried += static_cast<long>(on.carried);
        total_settled += static_cast<long>(on.counts.fused[kSettle] + on.counts.staged[kSettle]);
        ++runs;
    }
    std::printf("l2 hosts: %d configurations (%ld owning excursions), %ld of %ld settlements "
                "carried\n", runs, excursion_runs, total_carried, total_settled);
}

class CarryBookHost final : public k3_book::BookHost {
public:
    explicit CarryBookHost(const BookConfig& config)
        : BookHost(config), seed_(config.seed ^ 0x165667B19E3779F9ull), current_rng_(seed_) {}
    long current_applied = 0;
    void on_native_run_begin() override {
        BookHost::on_native_run_begin();
        current_rng_ = k3_book::Rng(seed_);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        BookHost::on_native_bar(bar, context);
        // A market request consumed at this very calculation point.
        if (current_rng_.percent(25)) {
            no::Request market{no::Transact{current_rng_.percent(50) ? 1.0 : -1.0}, "d2c-now", ""};
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

void books_carry_their_stage() {
    int runs = 0;
    long current = 0;
    long carried_total = 0;
    std::uint64_t seed = 92001;
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
                CarryBookHost carry_host(config);
                CarryBookHost again_host(config);
                const auto drive = [&config](CarryBookHost& host) {
                    return outcome_compare::run_book(host, config, outcome_compare::as_is);
                };
                Paths on;
                Paths off;
                const auto carried = run_with(carry_host, true, on, drive);
                const auto staged = run_with(again_host, false, off, drive);
                bool equal = outcome_compare::same(tally, carried, staged);
                equal = carried_alike(on, off, true) && equal;
                CHECK(carry_host.current_applied == again_host.current_applied);
                equal = equal && carry_host.current_applied == again_host.current_applied;
                if (!equal) outcome_compare::describe(config, "");
                current += carry_host.current_applied;
                carried_total += static_cast<long>(on.carried);
                ++runs;
            }
        }
    }
    std::printf("k3 books: %d configurations, %ld settlements carried, %ld current executions "
                "applied\n", runs, carried_total, current);
    CHECK(carried_total > 100);
    CHECK(current > 100);
}

enum class Rewrite { FeeValue, FeeKind, PointValue, AccountFx };

const char* rewrite_name(Rewrite rewrite) {
    switch (rewrite) {
    case Rewrite::FeeValue: return "fee value";
    case Rewrite::FeeKind: return "fee kind";
    case Rewrite::PointValue: return "point value";
    case Rewrite::AccountFx: return "account rate";
    }
    return "?";
}

// A host whose precommit hook rewrites a fee input or the account rate at
// every call, from the pump's inside, as a host holding a writable alias of
// itself can: the projection check waits for the pump's end, so the
// settlement that follows runs under the written value.
class RewritingHost final : public l2_fused::FusedHost {
public:
    RewritingHost(const l2_fused::Config& config, Rewrite rewrite)
        : FusedHost(config), rewrite_(rewrite) {}
    mutable long rewrites = 0;
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        const NativePrecommitVerdict verdict = FusedHost::validate_execution_precommit(view);
        RewritingHost& self = const_cast<RewritingHost&>(*this);
        const bool odd = (++rewrites % 2) != 0;
        switch (rewrite_) {
        case Rewrite::FeeValue:
            self.commission_value_ += 0.125;
            break;
        case Rewrite::FeeKind:
            self.commission_type_ = odd ? CommissionType::CASH_PER_CONTRACT
                                        : CommissionType::PERCENT;
            break;
        case Rewrite::PointValue:
            self.syminfo_.pointvalue = odd ? 2.0 : 1.0;
            break;
        case Rewrite::AccountFx:
            self.account_currency_fx_ = odd ? 1.5 : 1.0;
            break;
        }
        return verdict;
    }

private:
    Rewrite rewrite_;
};

void rewritten_quotes_carry_alike() {
    int runs = 0;
    long rewrites = 0;
    long carried_total = 0;
    std::uint64_t seed = 93001;
    for (const Rewrite rewrite :
         {Rewrite::FeeValue, Rewrite::FeeKind, Rewrite::PointValue, Rewrite::AccountFx}) {
        for (const l2_fused::Path path :
             {l2_fused::Path::None, l2_fused::Path::Synthesized, l2_fused::Path::Lower}) {
            l2_fused::Config config;
            config.seed = seed++;
            config.bars = 90;
            config.path = path;
            config.fee_kind = NativeFeeKind::Percent;
            config.fee_value = 0.05;
            config.owns_excursions = path == l2_fused::Path::Synthesized;
            RewritingHost carry_host(config, rewrite);
            RewritingHost again_host(config, rewrite);
            const auto drive = [&config](RewritingHost& host) {
                return outcome_compare::run_fused(host, config, outcome_compare::as_is);
            };
            Paths on;
            Paths off;
            const auto carried = run_with(carry_host, true, on, drive);
            const auto staged = run_with(again_host, false, off, drive);
            bool equal = outcome_compare::same(tally, carried, staged);
            equal = carried_alike(on, off, true) && equal;
            CHECK(on.carried > 0);
            CHECK(carry_host.rewrites > 0);
            CHECK(carry_host.rewrites == again_host.rewrites);
            if (!equal) outcome_compare::describe(config, rewrite_name(rewrite));
            rewrites += carry_host.rewrites;
            carried_total += static_cast<long>(on.carried);
            ++runs;
        }
    }
    std::printf("rewriting hosts: %d configurations, %ld fee inputs or rates rewritten, %ld "
                "settlements carried across them\n", runs, rewrites, carried_total);
}

class UndeclaredHost final : public l2_fused::FusedHost {
public:
    explicit UndeclaredHost(const l2_fused::Config& config) : FusedHost(config) {
        declare_native_precommit_hook(false);
    }
};

void no_preview_nothing_carried() {
    int runs = 0;
    for (const auto& config : outcome_compare::fused_configs(94001)) {
        if (config.owns_excursions) continue;
        UndeclaredHost carry_host(config);
        UndeclaredHost again_host(config);
        const auto drive = [&config](UndeclaredHost& host) {
            return outcome_compare::run_fused(host, config, outcome_compare::as_is);
        };
        Paths on;
        Paths off;
        const auto carried = run_with(carry_host, true, on, drive);
        const auto staged = run_with(again_host, false, off, drive);
        bool equal = outcome_compare::same(tally, carried, staged);
        equal = carried_alike(on, off, false) && equal;
        CHECK(on.counts.fused[kSettle] > 0);
        if (!equal) outcome_compare::describe(config, "undeclared");
        ++runs;
    }
    std::printf("undeclared hosts: %d configurations, nothing previewed or carried\n", runs);
}

}  // namespace

int main() {
    fused_hosts_carry_their_stage();
    books_carry_their_stage();
    rewritten_quotes_carry_alike();
    no_preview_nothing_carried();
    CHECK(total_carried > 500);
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_native_settlement_carry: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_native_settlement_carry: ok (%ld checks)\n", tally.checks);
    return 0;
}
