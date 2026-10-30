// Count real roster dereferences in the adapter's origin-release pass and
// compare its ordered commands with the original roster-search algorithm.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cohort_origin_fixture {
struct Work {
    std::uint64_t passes = 0;
    std::uint64_t roster_visits = 0;
};
Work* counting = nullptr;

void observe(const char* site, const void*, std::uint64_t, std::uint64_t count) noexcept {
    if (!counting) return;
    if (std::strcmp(site, "cohort_origin_pass") == 0) counting->passes += count;
    if (std::strcmp(site, "cohort_origin_roster") == 0) counting->roster_visits += count;
}

struct CountScope {
    Work* previous = counting;
    explicit CountScope(Work& work) { counting = &work; }
    ~CountScope() { counting = previous; }
};
} // namespace cohort_origin_fixture

// The existing compile-time observer seam keeps the normal library inert.
// These definitions satisfy the adapter symbols before the archive is read,
// as in the adapter's position-tail and storage tests.
#define PINEFORGE_LOOKUP_INDEX_WORK(site, owner, run, count) \
    ::cohort_origin_fixture::observe(site, owner, run, count)
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_LOOKUP_INDEX_WORK

namespace pineforge {
inline namespace engine_script_run_v19 {
struct NativeExecutionConsumerProbe {
    static std::pair<std::uint64_t, std::uint64_t> cohort_digest(
            const NativeExecutionConsumer& consumer) {
        return {consumer.cohort_receipts_.count, consumer.cohort_receipts_.h};
    }
};
} // inline namespace engine_script_run_v19
} // namespace pineforge

namespace cohort_origin_fixture {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using Removal = std::pair<std::uint64_t, std::uint64_t>;
using Digest = std::pair<std::uint64_t, std::uint64_t>;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        ++failures;                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                         \
} while (false)

void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

// Explicit member instantiation is the adapter storage fixtures' access
// convention; it changes neither the class nor its access specifiers.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct ReleaseOrigins { friend auto access(ReleaseOrigins); };
struct RetiredScratch { friend auto access(RetiredScratch); };
template struct Access<ReleaseOrigins, &Adapter::release_closed_cohort_origins>;
template struct Access<RetiredScratch, &Adapter::retired_row_scratch_>;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        1736121600000LL + i * 60000LL});
    return bars;
}

class Host : public source::PineStrategyHost {
public:
    explicit Host(int horizon) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 100000;
        config.close_entries_rule_any = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        require(set_realtime_tail(true, horizon), "realtime tail configuration refused");
        fixture_retain_all_events();
    }
    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    const no::WorkingRequestCore& core() { return consumer().request_core(); }
    auto& scratch() { return adapter_.*access(RetiredScratch{}); }
    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
    void release() {
        // Isolate one due pass at the callback's fully settled population.
        // Current-execution notifications may still be queued at this point.
        scratch().roster_pass_due = true;
        (adapter_.*access(ReleaseOrigins{}))();
    }
};

class RampHost final : public Host {
public:
    explicit RampHost(int openings) : Host(openings + 1), openings_(openings) {}
    Work work;
    bool measured = false;
    int opening_fills = 0;
    int closing_fills = 0;

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index < openings_) {
            strategy_entry("root-" + std::to_string(index), true);
            return;
        }
        require(!measured, "ramp measured more than one terminal population");
        require(lots().size() == static_cast<std::size_t>(openings_),
                "ramp did not fill every distinct opening");
        require(core().cohorts().size() == lots().size() && core().live().empty(),
                "ramp did not produce one held roster per terminal opening");
        for (const auto& roster : core().cohorts())
            require(roster.origins.size() == 1, "ramp roster does not have one root");
        const auto epoch = consumer().dormant_core_epoch();
        const auto digest = NativeExecutionConsumerProbe::cohort_digest(consumer());
        const auto continuation = native_continuation_hash();
        {
            CountScope scope(work);
            release();
        }
        CHECK(scratch().successor_lots.empty());
        CHECK(scratch().released_origins.empty());
        CHECK(consumer().dormant_core_epoch() == epoch);
        CHECK(NativeExecutionConsumerProbe::cohort_digest(consumer()) == digest);
        CHECK(native_continuation_hash() == continuation);
        measured = true;
    }

protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.opened_units > 0.0) ++opening_fills;
        if (event.closed_units > 0.0) ++closing_fills;
    }

private:
    int openings_;
};

void ramp(int openings, std::uint64_t bound) {
    RampHost host(openings);
    // One admission bar, followed by N next-open fills, one per bar, no exits.
    const auto bars = tape(openings + 1);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    CHECK(host.measured);
    CHECK(host.opening_fills == openings);
    CHECK(host.closing_fills == 0); // No close is submitted and the tail stays live.
    CHECK(host.trade_count() == 0); // Every opening is still held.
    CHECK(host.lots().size() == static_cast<std::size_t>(openings));
    CHECK(host.work.passes == 1); // Exactly one explicitly isolated release pass.
    CHECK(host.work.roster_visits > 0); // A missing observer must fail independently.
    CHECK(host.work.roster_visits <= bound);
    std::printf("distinct roots N=%d: passes=%llu roster visits=%llu bound=%llu\n",
                openings, static_cast<unsigned long long>(host.work.passes),
                static_cast<unsigned long long>(host.work.roster_visits),
                static_cast<unsigned long long>(bound));
}

struct ReleasePlan {
    std::vector<std::uint64_t> successor_lots;
    std::vector<std::pair<no::CohortHandle, std::uint64_t>> released;
    no::RequestHandle probe{};
};

// Frozen original algorithm, from working-set construction through ordered
// removal construction. Inputs are the same core and physical lots the
// adapter reads. The small direct scan can only certify an empty result;
// every identity population below has an unbound first member, so it falls
// through that scan to this block even when its product is at most 64.
ReleasePlan original_release(const no::WorkingRequestCore& core,
                             const std::vector<PyramidEntry>& entries) {
    ReleasePlan out;
    std::vector<std::uint64_t> working, lots;
    auto& successor_lots = out.successor_lots;
    auto& released = out.released;
    const auto root_of = [](const no::LiveRequest& row) {
        return row.definition->root ? row.definition->root->incarnation
                                    : row.handle().incarnation;
    };
    for (const auto& live : core.live()) working.push_back(root_of(live));
    std::sort(working.begin(), working.end());
    for (const auto& lot : entries) lots.push_back(lot.entry_incarnation);
    std::sort(lots.begin(), lots.end());
    lots.erase(std::unique(lots.begin(), lots.end()), lots.end());
    const auto member = [](const std::vector<no::RequestHandle>& origins,
                           std::uint64_t incarnation) {
        const auto at = std::lower_bound(origins.begin(), origins.end(), incarnation,
            [](const no::RequestHandle& origin, std::uint64_t value) {
                return origin.incarnation < value;
            });
        return at != origins.end() && at->incarnation == incarnation;
    };
    for (const auto opening : lots) {
        if (opening == 0) continue;
        bool root = false;
        for (const auto& roster : core.cohorts()) {
            if (member(roster.origins, opening)) {
                root = true;
                break;
            }
        }
        if (!root) successor_lots.push_back(opening);
    }
    auto& probe = out.probe;
    for (const auto& roster : core.cohorts()) {
        const auto& origins = roster.origins;
        if (origins.empty()) continue;
        if (probe.run != origins.front().run) probe.run = origins.front().run;
        std::uint64_t kept_below = 0;
        for (const auto opening : successor_lots) {
            probe.incarnation = opening;
            if (core.cohort_contains(roster.handle, probe)) kept_below = opening;
        }
        for (const auto& origin : origins) {
            const std::uint64_t incarnation = origin.incarnation;
            if (incarnation < kept_below
                || std::binary_search(working.begin(), working.end(), incarnation)
                || std::binary_search(lots.begin(), lots.end(), incarnation)) {
                continue;
            }
            released.emplace_back(roster.handle, incarnation);
        }
    }
    return out;
}

struct Stage {
    std::vector<no::CohortReceipt> receipts;
    std::vector<Removal> removals;
    std::vector<std::uint64_t> successors;
    Digest before_digest{}, after_digest{};
    std::uint64_t before_epoch = 0, after_epoch = 0;
    std::uint64_t before_continuation = 0, after_continuation = 0;
};

struct SourceCohort { friend auto access(SourceCohort); };
template struct Access<SourceCohort, &Adapter::cohort_for>;

class IdentityHost final : public Host {
public:
    explicit IdentityHost(bool original) : Host(1), original_(original) {}
    std::vector<Stage> stages;

    void on_source_bar(const Bar&) override {
        require(stages.empty(), "identity on_source_bar constructed its population twice");
        // Source orders need placement records for opening admission. Register
        // their named cohorts first so they use these same three rosters.
        const auto a = (adapter_.*access(SourceCohort{}))("replace-root");
        const auto b = (adapter_.*access(SourceCohort{}))("direct-root");
        const auto c = (adapter_.*access(SourceCohort{}))("closing-root");
        const auto empty = cohort_open();
        // Cohort creation issues handles 1 through 4 in order.
        CHECK(a.value == 1 && b.value == 2 && c.value == 3 && empty.value == 4);
        // Cohort opens issue no request incarnations; the next request is 1.
        const auto old_pending = put_resting("older-pending");
        CHECK(old_pending.incarnation == 1); // First submitted request.
        cohort_add(a, old_pending);
        withdraw(old_pending);
        const auto root = put_order("replace-root", 1.0, 50.0);
        CHECK(root.incarnation == 2); // The second submit follows the first cancel.
        cohort_add(b, root); // A shared root exercises duplicate union members.
        const auto successor = put_order("replace-root", 2.0);
        require(successor != root && !core().find_live(root),
                "identity strategy_order replace-root did not issue a successor");
        const auto* successor_row = core().find_live(successor);
        require(successor_row && successor_row->definition->root
                    && *successor_row->definition->root == root,
                "identity strategy_order replace-root successor lost its chain root");
        CHECK(successor.incarnation == 3); // A fresh-handle replace issues one successor.
        fill(successor, 2.0, 0.0); // The long transaction on flat opens 2 and closes 0.
        const auto pending_a = put_resting("later-pending-a");
        CHECK(pending_a.incarnation == 4); // Next submit after the replace successor.
        cohort_add(a, pending_a);
        withdraw(pending_a);
        const auto direct = put_order("direct-root", 3.0);
        CHECK(direct.incarnation == 5); // The next submit follows canceled origin 4.
        fill(direct, 3.0, 0.0); // A same-side transaction opens 3 and closes 0.
        const auto pending_b = put_resting("later-pending-b");
        CHECK(pending_b.incarnation == 6); // The next submit follows filled origin 5.
        cohort_add(b, pending_b);
        withdraw(pending_b);
        const auto closed = put_order("closing-root", 4.0);
        CHECK(closed.incarnation == 7); // The next submit follows canceled origin 6.
        fill(closed, 4.0, 0.0); // A same-side transaction opens 4 and closes 0.
        require(core().live().empty(),
                "identity after closing-root fill left an unexpected working request");
        CHECK(lots().size() == 3); // Successor 3 and direct openings 5 and 7.
        CHECK(physical_position().signed_units == 9.0); // 2 + 3 + 4 long units.
        CHECK(trade_count() == 0); // The first release follows cancels alone.
        stages.push_back(release_stage());
        CHECK(core().cohort_contains(a, successor));
        CHECK(core().cohort_contains(b, successor));
        CHECK(!core().cohort_contains(a, pending_a));
        CHECK(!core().cohort_contains(b, pending_b));

        no::Request close_direct{no::Reduce{no::ExplicitUnits{4.0}}, "close-direct", ""};
        close_direct.owner = no::BindOpening{closed, position_cycle_seq_};
        const auto first_close = put(close_direct);
        CHECK(first_close.incarnation == 8); // Closing root 7 issues the next request.
        fill(first_close, 0.0, 4.0); // The bound Reduce opens 0 and closes lot 7's 4.
        CHECK(lots().size() == 2); // Only openings 3 and 5 remain after closing 7.
        stages.push_back(release_stage());

        no::Request close_successor{no::Reduce{no::ExplicitUnits{2.0}}, "close-successor", ""};
        close_successor.owner = no::BindOpening{successor, position_cycle_seq_};
        const auto second_close = put(close_successor);
        CHECK(second_close.incarnation == 9); // Closing successor 3 issues the next request.
        fill(second_close, 0.0, 2.0); // The bound Reduce opens 0 and closes lot 3's 2.
        CHECK(lots().size() == 1); // Opening 5 survives both selected closes.
        CHECK(physical_position().signed_units == 3.0); // 9 - 4 - 2 long units.
        stages.push_back(release_stage());
        CHECK(core().cohort_contains(b, direct));
        CHECK(!core().cohort_contains(a, root));
    }

private:
    no::RequestHandle put_order(const char* label, double units,
                               double limit = std::numeric_limits<double>::quiet_NaN()) {
        strategy_order(label, true, units, limit);
        const auto live = core().live();
        const auto found = std::find_if(live.begin(), live.end(),
            [&](const no::LiveRequest& row) { return row.request().label == label; });
        require(found != live.end(),
                ("identity strategy_order " + std::string(label)
                    + " did not leave an accepted working request").c_str());
        return found->handle();
    }
    no::RequestHandle put(const no::Request& request) {
        const auto result = submit(request);
        std::string reason = "identity submit " + request.label + " SubmitStatus="
            + std::to_string(static_cast<unsigned>(result.status));
        if (result.reason)
            reason += " RequestRejectReason="
                + std::to_string(static_cast<unsigned>(*result.reason));
        require(result.status == no::SubmitStatus::Accepted && result.handle,
                reason.c_str());
        return *result.handle;
    }
    no::RequestHandle put_resting(const char* label) {
        no::Request request{no::Transact{1.0}, label, ""};
        request.trigger = no::Limit{50.0};
        return put(request);
    }
    void withdraw(const no::RequestHandle& handle) {
        const auto result = cancel(handle);
        require(result.status == no::CancelStatus::Cancelled,
                ("identity cancel incarnation " + std::to_string(handle.incarnation)
                    + " CancelStatus="
                    + std::to_string(static_cast<unsigned>(result.status))).c_str());
    }
    void fill(const no::RequestHandle& handle, double opened, double closed) {
        const auto result = execute_current({handle, NativeCurrentPriceRule::AsPresented});
        std::string outcome = "ExecutionAppliedEvent";
        if (const auto* refusal = std::get_if<NativeCurrentRefusal>(&result)) {
            outcome = "NativeCurrentRefusal=" + std::to_string(static_cast<unsigned>(*refusal));
        } else if (std::holds_alternative<no::NoEffectEvent>(result)) {
            outcome = "NoEffectEvent";
        } else if (const auto* rejected = std::get_if<no::MatchRejectedEvent>(&result)) {
            outcome = "MatchRejectedEvent MatchRejectReason="
                + std::to_string(static_cast<unsigned>(rejected->reason));
        } else if (const auto* cancelled = std::get_if<no::CancelledEvent>(&result)) {
            outcome = "CancelledEvent CancelReason="
                + std::to_string(static_cast<unsigned>(cancelled->reason));
        }
        require(std::holds_alternative<no::ExecutionAppliedEvent>(result),
                ("identity execute_current incarnation " + std::to_string(handle.incarnation)
                    + " returned " + outcome).c_str());
        const auto& event = std::get<no::ExecutionAppliedEvent>(result);
        CHECK(event.opened_units == opened);
        CHECK(event.closed_units == closed);
    }
    Stage release_stage() {
        const auto reference = original_release(core(), lots());
        const auto receipt_begin = core().cohort_receipts().size();
        Stage stage;
        stage.before_digest = NativeExecutionConsumerProbe::cohort_digest(consumer());
        stage.before_epoch = consumer().dormant_core_epoch();
        stage.before_continuation = native_continuation_hash();
        if (original_) {
            for (const auto& [cohort, incarnation] : reference.released) {
                no::RequestHandle origin = reference.probe;
                origin.incarnation = incarnation;
                cohort_remove(cohort, std::move(origin));
            }
            scratch().roster_pass_due = false;
        } else {
            Work work;
            {
                CountScope scope(work);
                release();
            }
            CHECK(work.passes == 1); // Each stage falls through the direct-scan test.
            CHECK(work.roster_visits > 0); // Even the identity row must reach the loop.
            CHECK(scratch().successor_lots == reference.successor_lots);
            CHECK(scratch().released_origins == reference.released);
        }
        const auto& receipts = core().cohort_receipts();
        stage.receipts.assign(receipts.begin() + static_cast<std::ptrdiff_t>(receipt_begin),
                              receipts.end());
        stage.successors = reference.successor_lots;
        CHECK(stage.receipts.size() == reference.released.size());
        for (std::size_t i = 0; i < stage.receipts.size(); ++i) {
            const auto& receipt = stage.receipts[i];
            CHECK(receipt.operation == no::CohortReceiptOperation::Remove);
            CHECK(receipt.status == no::CohortReceiptStatus::Applied);
            stage.removals.emplace_back(receipt.cohort.value, receipt.origin.incarnation);
            if (i < reference.released.size()) {
                no::RequestHandle origin = reference.probe;
                origin.incarnation = reference.released[i].second;
                CHECK(receipt.cohort == reference.released[i].first);
                CHECK(receipt.origin == origin);
            }
        }
        stage.after_digest = NativeExecutionConsumerProbe::cohort_digest(consumer());
        stage.after_epoch = consumer().dormant_core_epoch();
        stage.after_continuation = native_continuation_hash();
        return stage;
    }
    bool original_;
};

bool same_receipt(const no::CohortReceipt& a, const no::CohortReceipt& b) {
    return a.operation == b.operation && a.status == b.status
        && a.cohort == b.cohort && a.origin == b.origin;
}

void identity() {
    const auto bars = tape(1);
    IdentityHost reference(true);
    IdentityHost actual(false);
    reference.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    actual.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(reference.last_error().empty(),
            ("identity reference run: " + reference.last_error()).c_str());
    require(actual.last_error().empty(),
            ("identity current run: " + actual.last_error()).c_str());
    // One cancellation-only pass and one pass after each of two closes.
    CHECK(actual.stages.size() == 3 && reference.stages.size() == 3);
    // Before any close, successor 3 keeps roots below 3 in both rosters;
    // canceled roots 4 and 6 are newer and have no working request or lot.
    const std::vector<Removal> canceled{{1, 4}, {2, 6}};
    // Closing opening 7 releases its sole root in cohort 3.
    const std::vector<Removal> direct_close{{3, 7}};
    // Closing successor 3 releases cohort 1's older roots and cohort 2's shared root.
    const std::vector<Removal> successor_close{{1, 1}, {1, 2}, {2, 2}};
    const std::vector<std::vector<Removal>> expected{canceled, direct_close, successor_close};
    // One Applied cohort_remove increments the core epoch and receipt count once.
    const std::uint64_t epoch_deltas[]{2, 1, 3};
    // Only opening 3 lacks a roster root until its selected close.
    const std::vector<std::vector<std::uint64_t>> successors{{3}, {3}, {}};
    const auto stages = std::min(actual.stages.size(), reference.stages.size());
    for (std::size_t i = 0; i < stages && i < expected.size(); ++i) {
        const auto& a = actual.stages[i];
        const auto& r = reference.stages[i];
        CHECK(a.removals == expected[i] && r.removals == expected[i]);
        CHECK(a.successors == successors[i] && r.successors == successors[i]);
        CHECK(a.before_digest == r.before_digest);
        CHECK(a.after_digest == r.after_digest);
        CHECK(a.before_epoch == r.before_epoch);
        CHECK(a.after_epoch == r.after_epoch);
        CHECK(a.after_epoch - a.before_epoch == epoch_deltas[i]);
        CHECK(a.after_digest.first - a.before_digest.first == epoch_deltas[i]);
        CHECK(a.before_continuation == r.before_continuation);
        CHECK(a.after_continuation == r.after_continuation);
        CHECK(a.receipts.size() == r.receipts.size());
        for (std::size_t j = 0; j < std::min(a.receipts.size(), r.receipts.size()); ++j)
            CHECK(same_receipt(a.receipts[j], r.receipts[j]));
    }
    // Two selected closes book one trade each; opening 5 remains live.
    CHECK(actual.trade_count() == 2 && reference.trade_count() == 2);
    CHECK(actual.lots().size() == 1 && reference.lots().size() == 1);
    CHECK(actual.native_continuation_hash() == reference.native_continuation_hash());
    CHECK(actual.broker_state_hash() == reference.broker_state_hash());
}

template<class Function> void row(const char* name, Function function) {
    try {
        function();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
    }
}
} // namespace cohort_origin_fixture

int main() {
    using namespace cohort_origin_fixture;
    // A union pass dereferences each of the N singleton rosters exactly once,
    // then sorts N roots and binary-searches N lots. Allow 2*N roster visits:
    // 256 and 512. The old scan visits 1+...+N rosters, hence 8256 and 32896.
    // Both N^2 products exceed kDirectScan=64.
    row("distinct roots 128", [] { ramp(128, 256); });
    row("distinct roots 256", [] { ramp(256, 512); });
    // 65 > 64, 65^2 > 64; bound 2*65=130, old visits 65*66/2=2145.
    // The positive counter assertion rejects both an early return and a
    // counter removed along with the old loop.
    row("nonzero roster witness", [] { ramp(65, 130); });
    row("ordered origin release", [] { identity(); });
    std::printf("test_adapter_cohort_origin_release: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
