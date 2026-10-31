// Count real roster dereferences in the adapter's origin-release pass and
// compare its ordered commands with the original roster-search algorithm.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
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

#if defined(PINEFORGE_ROSTER_PASS_OBSERVER) \
    && __has_include("../src/source/roster_pass_work.hpp")
#include "../src/source/roster_pass_work.hpp"
namespace cohort_origin_fixture {
using RosterWork = pineforge::source::roster_pass_work::Counts;
using RosterScope = pineforge::source::roster_pass_work::Scope;
constexpr bool roster_observer_available = true;
}
#else
// The test overlay must compile against the previous implementation too.
// Missing instrumentation is a named runtime failure, including the quiet row.
namespace cohort_origin_fixture {
struct RosterWork {
    std::uint64_t roster_cohort_visits = 0;
    std::uint64_t due_passes = 0;
};
struct RosterScope {
    explicit RosterScope(RosterWork&) noexcept {}
};
constexpr bool roster_observer_available = false;
}
#endif

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
int rows_run = 0;
const char* selected_row = nullptr;
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

void expect_work(const char* name, const RosterWork& actual, const RosterWork& expected) {
    ++checks;
    if (!roster_observer_available
        || actual.roster_cohort_visits != expected.roster_cohort_visits
        || actual.due_passes != expected.due_passes) {
        ++failures;
        std::fprintf(stderr,
            "FAIL %s: due_passes=%llu expected=%llu roster_cohort_visits=%llu expected=%llu%s\n",
            name, static_cast<unsigned long long>(actual.due_passes),
            static_cast<unsigned long long>(expected.due_passes),
            static_cast<unsigned long long>(actual.roster_cohort_visits),
            static_cast<unsigned long long>(expected.roster_cohort_visits),
            roster_observer_available ? "" : " (roster observer unavailable)");
    }
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

enum class CounterPath { Empty, Direct, General, FailedDirect, NotDue };

class CounterHost final : public Host {
public:
    CounterHost(std::size_t handles, CounterPath path)
        : Host(1), handles_(handles), path_(path) {}
    RosterWork work, expected;
    std::size_t cohort_rows = 0, nonempty_rows = 0, memberships = 0, live_requests = 0;
    bool measured = false;

    void on_source_bar(const Bar&) override {
        require(!measured && handles_ != 0, "counter population was not constructed once");
        no::CohortHandle last{};
        for (std::size_t i = 0; i < handles_; ++i) last = cohort_open();
        // One rostered root, zero lots. General-path work must exceed the
        // source's direct-scan product bound: M*(L+Q) = 1*(0+(64+1)).
        constexpr std::size_t direct_scan_limit = 64;
        const std::size_t requests = path_ == CounterPath::Empty ? 0
            : path_ == CounterPath::General ? direct_scan_limit + 1 : 1;
        no::RequestHandle member{};
        for (std::size_t i = 0; i < requests; ++i) {
            no::Request request{no::Transact{1.0}, "pending-" + std::to_string(i), ""};
            request.trigger = no::Limit{50.0};
            const auto result = submit(request);
            require(result.status == no::SubmitStatus::Accepted && result.handle,
                    "counter request was not accepted");
            if (i == 0) {
                member = *result.handle;
                cohort_add(last, member);
            }
        }
        if (path_ == CounterPath::FailedDirect) {
            require(cancel(member).status == no::CancelStatus::Cancelled,
                    "counter member was not canceled");
        }
        require(lots().empty(), "counter pending requests unexpectedly filled");
        live_requests = core().live().size();
        require(live_requests == requests - (path_ == CounterPath::FailedDirect ? 1 : 0),
                "counter working population differs from its construction");
        const bool due = path_ != CounterPath::NotDue;
        expected.due_passes = due ? 1 : 0;
        // Derive N from the constructed rosters, independently of the pass.
        // Empty handles cost no visits: general=3N, direct=2N,
        // failed-direct=4N (the sole member is last), all-empty=0.
        for (const auto& roster : core().cohorts()) {
            ++cohort_rows;
            nonempty_rows += !roster.origins.empty();
            memberships += roster.origins.size();
            if (!due || roster.origins.empty()) continue;
            ++expected.roster_cohort_visits; // Member-count walk.
            if (path_ == CounterPath::Direct || path_ == CounterPath::FailedDirect)
                ++expected.roster_cohort_visits; // Direct walk.
            if (path_ == CounterPath::General || path_ == CounterPath::FailedDirect) {
                ++expected.roster_cohort_visits; // Union walk.
                ++expected.roster_cohort_visits; // Release walk.
            }
        }
        require(cohort_rows == handles_, "counter issued an unintended cohort");
        require(memberships == (path_ == CounterPath::Empty ? 0U : 1U),
                "counter membership population differs from its construction");
        require(nonempty_rows == memberships, "counter root is not in exactly one roster");
        const auto epoch = consumer().dormant_core_epoch();
        const auto digest = NativeExecutionConsumerProbe::cohort_digest(consumer());
        const auto continuation = native_continuation_hash();
        const auto receipt_begin = core().cohort_receipts().size();
        scratch().roster_pass_due = due;
        {
            RosterScope scope(work);
            (adapter_.*access(ReleaseOrigins{}))();
        }
        CHECK(!scratch().roster_pass_due);
        if (path_ == CounterPath::FailedDirect) {
            CHECK(!core().cohort_contains(last, member));
            CHECK(core().cohort_receipts().size() == receipt_begin + memberships);
            const auto& receipt = core().cohort_receipts().back();
            CHECK(receipt.operation == no::CohortReceiptOperation::Remove);
            CHECK(receipt.status == no::CohortReceiptStatus::Applied);
            CHECK(receipt.cohort == last && receipt.origin == member);
            CHECK(consumer().dormant_core_epoch() == epoch + memberships);
        } else {
            CHECK(core().cohort_receipts().size() == receipt_begin);
            CHECK(consumer().dormant_core_epoch() == epoch);
            CHECK(NativeExecutionConsumerProbe::cohort_digest(consumer()) == digest);
            CHECK(native_continuation_hash() == continuation);
            if (memberships != 0) CHECK(core().cohort_contains(last, member));
        }
        measured = true;
    }

private:
    std::size_t handles_;
    CounterPath path_;
};

RosterWork counter_case(const char* name, std::size_t handles, CounterPath path,
                        RosterWork* expected_out = nullptr) {
    CounterHost host(handles, path);
    const auto bars = tape(1);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.measured, "counter row did not reach the adapter");
    expect_work(name, host.work, host.expected);
    if (expected_out) *expected_out = host.expected;
    std::printf("%s: C=%zu R=%zu M=%zu L=0 Q=%zu visits=%llu expected=%llu\n",
                name, host.cohort_rows, host.nonempty_rows, host.memberships, host.live_requests,
                static_cast<unsigned long long>(host.work.roster_cohort_visits),
                static_cast<unsigned long long>(host.expected.roster_cohort_visits));
    return host.work;
}

void empty_heavy_work() {
    // Increase only historical handles. R=M=1, L=0 and Q=64+1 stay fixed.
    // With N unchanged, delta(3N)=0 despite the added historical handles.
    constexpr std::size_t first_handles = 8;
    constexpr std::size_t added_empty_handles = first_handles * first_handles;
    RosterWork first_expected, second_expected;
    const auto first = counter_case("roster empty heavy first", first_handles,
                                    CounterPath::General, &first_expected);
    const auto second = counter_case("roster empty heavy second",
                                    first_handles + added_empty_handles,
                                    CounterPath::General, &second_expected);
    RosterWork delta, expected_delta;
    require(second.roster_cohort_visits >= first.roster_cohort_visits,
            "adding empty handles reduced filtered work");
    delta.roster_cohort_visits = second.roster_cohort_visits - first.roster_cohort_visits;
    // Accumulated from the two constructed populations: 3*(N_second-N_first).
    expected_delta.roster_cohort_visits =
        second_expected.roster_cohort_visits - first_expected.roster_cohort_visits;
    expect_work("roster empty heavy slope", delta, expected_delta);
}

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

struct DensePlan {
    ReleasePlan decisions;
    RosterWork dense, filtered;
    std::size_t nonempty = 0;
    bool general = false;
};

// Independent reference for the complete dense pass, including its direct
// shortcut. It never calls the adapter pass or its nonempty enumeration.
// Charge dense work before looking at origins; charge expected filtered work
// only for populated rows of this independently traversed logical sequence.
DensePlan dense_release(const no::WorkingRequestCore& core,
                        const std::vector<PyramidEntry>& entries) {
    DensePlan out;
    out.dense.due_passes = out.filtered.due_passes = 1;
    const auto visit = [&](const no::CohortRoster& row) {
        ++out.dense.roster_cohort_visits;
        if (!row.origins.empty()) ++out.filtered.roster_cohort_visits;
    };
    std::size_t members = 0;
    for (const auto& row : core.cohorts()) {
        visit(row);
        members += row.origins.size();
        out.nonempty += !row.origins.empty();
    }
    if (members == 0) return out;
    const auto root = [](const no::LiveRequest& row) {
        return row.definition->root ? row.definition->root->incarnation
                                    : row.handle().incarnation;
    };
    constexpr std::size_t direct_limit = 64;
    if (members * (entries.size() + core.live().size()) <= direct_limit) {
        bool bound = true;
        for (const auto& row : core.cohorts()) {
            visit(row);
            for (const auto& origin : row.origins) {
                if (std::any_of(entries.begin(), entries.end(), [&](const PyramidEntry& lot) {
                        return lot.entry_incarnation == origin.incarnation;
                    }) || std::any_of(core.live().begin(), core.live().end(),
                        [&](const no::LiveRequest& live) {
                            return root(live) == origin.incarnation;
                        })) continue;
                bound = false;
                break;
            }
            if (!bound) break;
        }
        if (bound) return out;
    }
    out.general = true;
    std::vector<std::uint64_t> working, lots, roster_roots;
    for (const auto& live : core.live()) working.push_back(root(live));
    std::sort(working.begin(), working.end());
    for (const auto& lot : entries) lots.push_back(lot.entry_incarnation);
    std::sort(lots.begin(), lots.end());
    lots.erase(std::unique(lots.begin(), lots.end()), lots.end());
    for (const auto& row : core.cohorts()) {
        visit(row);
        for (const auto& origin : row.origins) roster_roots.push_back(origin.incarnation);
    }
    std::sort(roster_roots.begin(), roster_roots.end());
    roster_roots.erase(std::unique(roster_roots.begin(), roster_roots.end()),
                       roster_roots.end());
    auto& plan = out.decisions;
    for (const auto opening : lots)
        if (opening != 0 && !std::binary_search(roster_roots.begin(), roster_roots.end(), opening))
            plan.successor_lots.push_back(opening);
    for (const auto& row : core.cohorts()) {
        visit(row);
        if (row.origins.empty()) continue;
        plan.probe.run = row.origins.front().run;
        std::uint64_t kept_below = 0;
        for (const auto opening : plan.successor_lots) {
            visit(row); // The dense algorithm also probes only nonempty rows.
            plan.probe.incarnation = opening;
            if (core.cohort_contains(row.handle, plan.probe)) kept_below = opening;
        }
        for (const auto& origin : row.origins) {
            if (origin.incarnation < kept_below
                || std::binary_search(working.begin(), working.end(), origin.incarnation)
                || std::binary_search(lots.begin(), lots.end(), origin.incarnation)) continue;
            plan.released.emplace_back(row.handle, origin.incarnation);
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
    RosterWork roster_work, expected_work;
};

struct SourceCohort { friend auto access(SourceCohort); };
template struct Access<SourceCohort, &Adapter::cohort_for>;

class IdentityHost : public Host {
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

protected:
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
        stage.expected_work.due_passes = 1;
        // This population starts with an unbound canceled origin in the
        // first roster, so the optional direct scan stops after ONE row.
        // Count, union and release each walk N nonempty rows; each also
        // probes every successor lot: 3N + 1 + N*S.
        const auto first = core().cohorts().begin();
        require(first != core().cohorts().end() && !first->origins.empty(),
                "successor counter lacks its first unbound member");
        const auto unbound = first->origins.front().incarnation;
        require(std::none_of(lots().begin(), lots().end(),
                    [&](const PyramidEntry& lot) { return lot.entry_incarnation == unbound; })
                && std::none_of(core().live().begin(), core().live().end(),
                    [&](const no::LiveRequest& live) {
                        return (live.definition->root ? live.definition->root->incarnation
                                                      : live.handle().incarnation) == unbound;
                    }), "successor counter direct scan will not stop at its first row");
        ++stage.expected_work.roster_cohort_visits; // Failed direct prefix.
        for (const auto& roster : core().cohorts()) {
            if (roster.origins.empty()) continue;
            ++stage.expected_work.roster_cohort_visits; // Count.
            ++stage.expected_work.roster_cohort_visits; // Union.
            ++stage.expected_work.roster_cohort_visits; // Release.
            for (const auto opening : reference.successor_lots) {
                (void)opening;
                ++stage.expected_work.roster_cohort_visits; // Successor probe.
            }
        }
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
                RosterScope roster_scope(stage.roster_work);
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
private:
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
    const std::string identity_reference_error = reference.last_error();
    const std::string identity_actual_error = actual.last_error();
    require(identity_reference_error.empty(),
            ("identity reference run: " + identity_reference_error).c_str());
    require(identity_actual_error.empty(),
            ("identity current run: " + identity_actual_error).c_str());
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
    const auto identity_actual_continuation = actual.native_continuation_hash();
    const auto identity_reference_continuation = reference.native_continuation_hash();
    CHECK(identity_actual_continuation == identity_reference_continuation);
    const auto identity_actual_broker = actual.broker_state_hash();
    const auto identity_reference_broker = reference.broker_state_hash();
    CHECK(identity_actual_broker == identity_reference_broker);
}

void successor_work() {
    IdentityHost host(false);
    const auto bars = tape(1);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(!host.stages.empty(), "successor counter did not reach a release stage");
    for (const auto& stage : host.stages)
        expect_work("roster successor probes", stage.roster_work, stage.expected_work);
}

struct DifferentialStage {
    Stage state;
    std::vector<std::pair<no::CohortHandle, no::RequestHandle>> remaining;
    std::uint64_t before_broker = 0, after_broker = 0;
    std::size_t handles = 0, nonempty = 0;
};

class RandomHost final : public IdentityHost {
public:
    RandomHost(bool original, std::uint64_t seed, std::size_t rounds,
               std::size_t empty_handles)
        : IdentityHost(original), reference_(original), random_(seed),
          rounds_(rounds), empty_handles_(empty_handles) {}
    std::vector<DifferentialStage> samples;
    std::size_t successor_fills = 0, direct_fills = 0, closed_lots = 0;
    std::size_t empty_passes = 0, ordered_pairs = 0;

    void on_source_bar(const Bar&) override {
        require(samples.empty(), "random population constructed twice");
        // Four reusable named rosters bound N independently of historical C.
        // Empty handles are never stored in an auxiliary C-sized container.
        for (std::size_t i = 0; i < empty_handles_; ++i) cohort_open();
        for (std::size_t i = 0; i < rosters_.size(); ++i) {
            const auto gap = next() % rosters_.size();
            for (std::size_t j = 0; j < gap; ++j) cohort_open();
            rosters_[i] = (adapter_.*access(SourceCohort{}))(labels_[i]);
        }
        sample(); // All-empty due pass, before the first origin exists.
        for (std::size_t round = 0; round < rounds_; ++round) {
            const auto gap = next() % rosters_.size();
            for (std::size_t j = 0; j < gap; ++j) cohort_open();
            const auto slot = next() % rosters_.size();
            close(slot);
            // Cover direct and successor openings once each, then draw the
            // kind as well as the cohort and close choices from the seed.
            constexpr std::size_t opening_kinds = 2;
            const auto kind = round < opening_kinds ? round : next() % opening_kinds;
            const bool successor = kind != 0;
            auto opening = successor ? put_order(labels_[slot], 1.0, 50.0)
                                     : put_order(labels_[slot], 1.0);
            const auto shared = (slot + 1 + next() % (rosters_.size() - 1)) % rosters_.size();
            cohort_add(rosters_[shared], opening);
            if (successor) {
                const auto root = opening;
                opening = put_order(labels_[slot], 1.0);
                require(opening != root, "random replacement kept its handle");
                const auto* live = core().find_live(opening);
                require(live && live->definition->root && *live->definition->root == root,
                        "random replacement lost its root");
                ++successor_fills;
            } else {
                ++direct_fills;
            }
            fill(opening, 1.0, 0.0);
            held_[slot] = opening;
            // These origins are newer than every lot, so cancellation must
            // release both memberships in ascending cohort order.
            const auto canceled = put_resting("canceled-origin");
            const auto first = next() % rosters_.size();
            const auto second = (first + 1 + next() % (rosters_.size() - 1)) % rosters_.size();
            cohort_add(rosters_[first], canceled);
            cohort_add(rosters_[second], canceled);
            withdraw(canceled);
            const auto pending = put_resting("pending-origin");
            const auto pending_slot = next() % rosters_.size();
            cohort_add(rosters_[pending_slot], pending);
            sample();
            CHECK(core().cohort_contains(rosters_[pending_slot], pending));
            CHECK(!core().cohort_contains(rosters_[first], canceled));
            CHECK(!core().cohort_contains(rosters_[second], canceled));
            withdraw(pending);
            if (next() & 1) close(next() % rosters_.size());
            sample();
        }
        for (std::size_t i = 0; i < held_.size(); ++i) close(i);
        sample(); // Release the last filled roots.
        sample(); // Roster nodes erased by real closes; N=0, C unchanged.
        CHECK(lots().empty());
        CHECK(core().live().empty());
        for (const auto& row : core().cohorts()) CHECK(row.origins.empty());
    }

private:
    std::uint64_t next() {
        // A specified unsigned generator keeps sequences portable.
        random_ ^= random_ << 13;
        random_ ^= random_ >> 7;
        random_ ^= random_ << 17;
        return random_;
    }
    void close(std::size_t slot) {
        if (held_[slot].incarnation == 0) return;
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "selected-close", ""};
        request.owner = no::BindOpening{held_[slot], position_cycle_seq_};
        fill(put(request), 0.0, 1.0);
        held_[slot] = {};
        ++closed_lots;
    }
    void sample() {
        const auto reference = dense_release(core(), lots());
        // A second, pre-union reference also checks the decision algorithm,
        // independently of the complete dense reference's union construction.
        const auto searched = original_release(core(), lots());
        CHECK(reference.decisions.released == searched.released);
        DifferentialStage sample;
        sample.handles = core().cohorts().size();
        sample.nonempty = reference.nonempty;
        CHECK(sample.nonempty <= rosters_.size());
        if (sample.nonempty == 0) ++empty_passes;
        if (reference.decisions.released.size() >= 2) ++ordered_pairs;
        auto& state = sample.state;
        state.expected_work = reference.filtered;
        state.before_digest = NativeExecutionConsumerProbe::cohort_digest(consumer());
        state.before_epoch = consumer().dormant_core_epoch();
        state.before_continuation = native_continuation_hash();
        sample.before_broker = broker_state_hash();
        const auto receipt_begin = core().cohort_receipts().size();
        if (reference_) {
            for (const auto& [cohort, incarnation] : reference.decisions.released) {
                auto origin = reference.decisions.probe;
                origin.incarnation = incarnation;
                cohort_remove(cohort, origin);
            }
            scratch().roster_pass_due = false;
        } else {
            RosterScope scope(state.roster_work);
            release();
            expect_work("roster randomized filtered work", state.roster_work, reference.filtered);
            // Four outer walks at most, plus one membership probe per
            // nonempty row per successor: visits <= (4+S)*N, no C term.
            const std::size_t outer_walks = 4;
            const auto bound = (outer_walks + reference.decisions.successor_lots.size())
                * sample.nonempty;
            CHECK(state.roster_work.roster_cohort_visits <= bound);
            CHECK(reference.dense.roster_cohort_visits > reference.filtered.roster_cohort_visits);
            if (reference.general) {
                CHECK(scratch().released_origins == reference.decisions.released);
                CHECK(scratch().successor_lots == reference.decisions.successor_lots);
            }
        }
        const auto& receipts = core().cohort_receipts();
        state.receipts.assign(receipts.begin() + static_cast<std::ptrdiff_t>(receipt_begin),
                              receipts.end());
        CHECK(state.receipts.size() == reference.decisions.released.size());
        for (std::size_t i = 0; i < state.receipts.size(); ++i) {
            const auto& receipt = state.receipts[i];
            CHECK(receipt.operation == no::CohortReceiptOperation::Remove);
            CHECK(receipt.status == no::CohortReceiptStatus::Applied);
            if (i < reference.decisions.released.size()) {
                auto origin = reference.decisions.probe;
                origin.incarnation = reference.decisions.released[i].second;
                CHECK(receipt.cohort == reference.decisions.released[i].first);
                CHECK(receipt.origin == origin);
            }
            state.removals.emplace_back(receipt.cohort.value, receipt.origin.incarnation);
        }
        state.successors = reference.decisions.successor_lots;
        state.after_digest = NativeExecutionConsumerProbe::cohort_digest(consumer());
        state.after_epoch = consumer().dormant_core_epoch();
        state.after_continuation = native_continuation_hash();
        sample.after_broker = broker_state_hash();
        CHECK(state.after_epoch - state.before_epoch == state.receipts.size());
        CHECK(state.after_digest.first - state.before_digest.first == state.receipts.size());
        // Store memberships only, never the C synthesized empty rows.
        for (const auto& row : core().cohorts())
            for (const auto& origin : row.origins)
                sample.remaining.emplace_back(row.handle, origin);
        samples.push_back(std::move(sample));
    }
    const std::array<const char*, 4> labels_{{"alpha", "beta", "gamma", "delta"}};
    std::array<no::CohortHandle, 4> rosters_{};
    std::array<no::RequestHandle, 4> held_{};
    bool reference_;
    std::uint64_t random_;
    std::size_t rounds_, empty_handles_;
};

void randomized_case(std::uint64_t seed, std::size_t rounds, std::size_t empty_handles) {
    const int failures_before = failures;
    RandomHost reference(true, seed, rounds, empty_handles);
    RandomHost actual(false, seed, rounds, empty_handles);
    const auto bars = tape(1);
    reference.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    actual.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const std::string random_reference_error = reference.last_error();
    const std::string random_actual_error = actual.last_error();
    require(random_reference_error.empty(),
            ("random reference: " + random_reference_error).c_str());
    require(random_actual_error.empty(), ("random actual: " + random_actual_error).c_str());
    // Two passes per round, an initial empty pass, final release and empty pass.
    const auto expected_samples = rounds * 2 + 3;
    CHECK(reference.samples.size() == expected_samples);
    CHECK(actual.samples.size() == expected_samples);
    CHECK(actual.empty_passes >= 2); // The initial and final empty populations.
    CHECK(actual.ordered_pairs >= rounds); // Two canceled memberships each round.
    CHECK(actual.closed_lots == rounds); // Every round opens exactly one lot.
    CHECK(actual.successor_fills + actual.direct_fills == rounds);
    CHECK(actual.successor_fills != 0 && actual.direct_fills != 0);
    for (std::size_t i = 0; i < std::min(actual.samples.size(), reference.samples.size()); ++i) {
        const auto& a = actual.samples[i];
        const auto& r = reference.samples[i];
        CHECK(a.handles == r.handles && a.nonempty == r.nonempty);
        CHECK(a.handles >= empty_handles);
        CHECK(a.remaining == r.remaining);
        CHECK(a.before_broker == r.before_broker && a.after_broker == r.after_broker);
        CHECK(a.state.removals == r.state.removals);
        CHECK(a.state.successors == r.state.successors);
        CHECK(a.state.before_digest == r.state.before_digest);
        CHECK(a.state.after_digest == r.state.after_digest);
        CHECK(a.state.before_epoch == r.state.before_epoch);
        CHECK(a.state.after_epoch == r.state.after_epoch);
        CHECK(a.state.before_continuation == r.state.before_continuation);
        CHECK(a.state.after_continuation == r.state.after_continuation);
        CHECK(a.state.receipts.size() == r.state.receipts.size());
        for (std::size_t j = 0; j < std::min(a.state.receipts.size(), r.state.receipts.size()); ++j)
            CHECK(same_receipt(a.state.receipts[j], r.state.receipts[j]));
    }
    const auto random_actual_continuation = actual.native_continuation_hash();
    const auto random_reference_continuation = reference.native_continuation_hash();
    CHECK(random_actual_continuation == random_reference_continuation);
    const auto random_actual_broker = actual.broker_state_hash();
    const auto random_reference_broker = reference.broker_state_hash();
    CHECK(random_actual_broker == random_reference_broker);
    const auto random_actual_trades = actual.trade_count();
    const auto random_reference_trades = reference.trade_count();
    CHECK(random_actual_trades == random_reference_trades);
    if (failures != failures_before)
        std::fprintf(stderr, "random population seed=%llu rounds=%zu initial_empty=%zu\n",
                     static_cast<unsigned long long>(seed), rounds, empty_handles);
}

void randomized_release() {
    constexpr std::size_t sequences = 64;
    for (std::size_t i = 0; i < sequences; ++i) {
        // Odd, distinct seeds; vary rounds and historical gaps independently.
        const std::uint64_t seed = (static_cast<std::uint64_t>(i) + 1) * 2 + 1;
        randomized_case(seed, 12 + i % 13, 8 + i * 3);
    }
}

void sparse_long_release() {
    constexpr std::size_t slots = 4;
    // N <= slots, C starts at slots^6, and 2*slots^4+3 due passes exercise
    // repeated opens, closes and replacement chains without a C-sized index.
    constexpr std::size_t empty_handles = slots * slots * slots * slots * slots * slots;
    constexpr std::size_t rounds = slots * slots * slots * slots;
    randomized_case(empty_handles + rounds + 1, rounds, empty_handles);
}

template<class Function> void row(const char* name, Function function) {
    if (selected_row && std::strcmp(selected_row, name) != 0) return;
    ++rows_run;
    try {
        function();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
    }
}
} // namespace cohort_origin_fixture

int main(int argc, char** argv) {
    using namespace cohort_origin_fixture;
    if (argc == 2) selected_row = argv[1];
    else if (argc != 1) {
        std::fprintf(stderr, "FAIL row selection: expected at most one row name\n");
        return 1;
    }
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
    // C is test geometry; every expected visit is accumulated over its rows.
    constexpr std::size_t handles = 8;
    row("roster general", [] { counter_case("roster general", handles, CounterPath::General); });
    row("roster direct", [] { counter_case("roster direct", handles, CounterPath::Direct); });
    row("roster empty", [] { counter_case("roster empty", handles, CounterPath::Empty); });
    row("roster not due", [] { counter_case("roster not due", handles, CounterPath::NotDue); });
    row("roster failed direct", [] {
        counter_case("roster failed direct", handles, CounterPath::FailedDirect);
    });
    row("roster empty heavy", [] { empty_heavy_work(); });
    row("roster successor probes", [] { successor_work(); });
    row("roster randomized release", [] { randomized_release(); });
    row("roster sparse long release", [] { sparse_long_release(); });
    if (rows_run == 0) {
        ++failures;
        std::fprintf(stderr, "FAIL row selection: no matching row\n");
    }
    std::printf("test_adapter_cohort_origin_release: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
