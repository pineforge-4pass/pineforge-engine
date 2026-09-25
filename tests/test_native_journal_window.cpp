// R5 lane V19-B, the order core's journal window (native_order_v7).
//
// The core's command journal becomes a window: retire_history drops a prefix,
// and nothing the core decides afterwards may read a retired event. Four
// parts, all source-free (kernel profile too):
//
//   1. the window's mechanics -- absolute journal positions (history_end,
//      EventRange, history_at), event_at and the ordinal index after a
//      retirement, the counters a retirement does not move;
//   2. the chain index -- RequestDefinition::root and the issued ranges --
//      against a journal oracle: two cores driven by one randomized command
//      stream, one retiring its whole journal after every command, answer
//      every cohort command, roster and membership query exactly as a scan
//      of the unretired journal says the pre-window core did;
//   3. a scaling row: canonicalizing and testing the members of a long
//      replace chain costs O(log) per query, not the chain walk the journal
//      scan made quadratic (4x the chain must cost < 5x). The query loop is
//      calibrated to at least 100 ms of process CPU before the ratio is read;
//      the old sub-millisecond escape is therefore unnecessary.
//   4. a live deferred group-adjustment chain pins the window at its head,
//      and the arm that reads it back through collect_pending_chain answers
//      as it does on a core that retired nothing;
//   5. a trail's TrailArm ordinal lives in its tracking state
//      (TrailTrack / TrailActive::activation_ordinal), so it outlives the
//      retired event, and a successor that retained the ride answers 0.
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)

no::Request limit(double q, double price, const char* label = "") {
    no::Request r{no::Transact{q}, label, ""};
    r.trigger = no::Limit{price};
    return r;
}

uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

struct SplitMix {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t below(uint64_t n) { return n == 0 ? 0 : next() % n; }
};

// ── 1. mechanics ──────────────────────────────────────────────────────────
void the_window_keeps_absolute_positions() {
    const no::RunIdentity run{"journal-window", 1};
    no::WorkingRequestCore core(run);
    uint64_t inc = 1;
    uint64_t ord = 1;
    std::vector<no::RequestHandle> handles;
    for (int i = 0; i < 6; ++i) {
        const auto result = core.submit(limit(1.0, 90.0 - i), 0, inc, ord);
        CHECK(result.status == no::SubmitStatus::Accepted);
        handles.push_back(*result.handle);
    }
    CHECK(core.history_base() == 0);
    CHECK(core.history_end() == 6);
    CHECK(core.retired_through() == 0);

    // A replace chain: every successor carries the chain's first handle.
    no::RequestHandle tail = handles[2];
    for (int i = 0; i < 5; ++i) {
        const auto replaced = core.replace(tail, limit(1.0, 80.0 - i), 0, inc, ord);
        CHECK(replaced.status == no::ReplaceStatus::Replaced);
        tail = *replaced.successor;
        const auto* live = core.find_live(tail);
        CHECK(live && live->definition->root && *live->definition->root == handles[2]);
    }
    CHECK(!core.find_live(handles[0])->definition->root);

    const std::size_t end = core.history_end();
    const uint64_t last_ordinal = core.last_ordinal();
    const uint64_t last_incarnation = core.last_incarnation();
    const uint64_t third = ordinal_of(core.history()[2]);
    const uint64_t fourth = ordinal_of(core.history()[3]);

    // Retire through the third event.
    CHECK(core.retire_history(third) == 3);
    CHECK(core.history_base() == 3);
    CHECK(core.history_end() == end);
    CHECK(core.retired_through() == third);
    CHECK(core.last_ordinal() == last_ordinal);
    CHECK(core.last_incarnation() == last_incarnation);
    CHECK(core.event_at(no::EventId{run, third}) == nullptr);
    CHECK(core.event_at(no::EventId{run, fourth}) != nullptr);
    CHECK(ordinal_of(*core.event_at(no::EventId{run, fourth})) == fourth);
    bool threw = false;
    try {
        (void)core.history_at(2);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(ordinal_of(core.history_at(3)) == fourth);
    // Retiring again below the window, or at the same point, retires nothing.
    CHECK(core.retire_history(third) == 0);
    CHECK(core.retire_history(1) == 0);

    // A later install reports its range in absolute positions.
    auto prepared = core.prepare_cancel(handles[5], ord);
    auto installed = core.install_cancel(std::move(prepared));
    CHECK(std::holds_alternative<no::CommandInstalled<no::CancelResult>>(installed));
    const auto& cancelled = std::get<no::CommandInstalled<no::CancelResult>>(installed);
    ++ord;
    CHECK(cancelled.events.first_index == end);
    CHECK(cancelled.events.count == 1);
    CHECK(std::holds_alternative<no::CancelledEvent>(core.history_at(cancelled.events.first_index)));
    CHECK(core.history_end() == end + 1);

    // Retiring everything leaves an empty window over an unchanged journal
    // length, and the next command lands behind it.
    const std::size_t retained = core.history().size();
    CHECK(core.retire_history(core.last_ordinal()) == retained);
    CHECK(core.history().empty());
    CHECK(core.history_base() == end + 1);
    CHECK(core.history_end() == end + 1);
    CHECK(core.retired_through() == core.last_ordinal());
    const auto another = core.submit(limit(1.0, 70.0), 0, inc, ord);
    CHECK(another.status == no::SubmitStatus::Accepted);
    CHECK(core.history_end() == end + 2);
    CHECK(core.history().size() == 1);
    CHECK(ordinal_of(core.history_at(end + 1)) == another.event_ordinal);
    CHECK(core.event_at(no::EventId{run, another.event_ordinal}) != nullptr);
    // A reset forgets the window with the run.
    core.reset(no::RunIdentity{"journal-window", 2});
    CHECK(core.history_base() == 0 && core.history_end() == 0 && core.retired_through() == 0);
    CHECK(core.issued_incarnations().empty());
}

// ── 2. the chain index against the journal ────────────────────────────────
// What the pre-window core answered, recomputed from a whole journal: a
// handle exists when an Accepted or Replaced event names it, and its chain
// root is found by following predecessors through those definitions.
struct JournalOracle {
    std::map<uint64_t, std::optional<no::RequestHandle>> predecessor;

    void fold(const std::vector<no::CommandEvent>& history) {
        predecessor.clear();
        for (const auto& event : history) {
            if (const auto* accepted = std::get_if<no::AcceptedEvent>(&event)) {
                predecessor[accepted->definition->handle.incarnation] =
                    accepted->definition->predecessor;
            } else if (const auto* replaced = std::get_if<no::ReplacedEvent>(&event)) {
                predecessor[replaced->successor_definition->handle.incarnation] =
                    replaced->successor_definition->predecessor;
            }
        }
    }
    bool issued(const no::RunIdentity& run, const no::RequestHandle& handle) const {
        return handle.run == run && handle.incarnation != 0
            && predecessor.count(handle.incarnation) != 0;
    }
    no::RequestHandle root(const no::RequestHandle& handle) const {
        no::RequestHandle current = handle;
        for (;;) {
            const auto it = predecessor.find(current.incarnation);
            if (it == predecessor.end() || !it->second) return current;
            current = *it->second;
        }
    }
};

struct Driven {
    no::WorkingRequestCore core;
    uint64_t inc = 1;
    uint64_t ord = 1;
    bool retire_every_command = false;
    std::vector<bool> contains;
    explicit Driven(const no::RunIdentity& run, bool retire) : core(run), retire_every_command(retire) {}
    void settle() {
        if (retire_every_command) core.retire_history(core.last_ordinal());
    }
};

bool same_receipts(const std::vector<no::CohortReceipt>& a, const std::vector<no::CohortReceipt>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].operation != b[i].operation || a[i].status != b[i].status
            || a[i].cohort != b[i].cohort || a[i].origin != b[i].origin) {
            return false;
        }
    }
    return true;
}

void the_chain_index_answers_what_the_journal_did(uint64_t seed) {
    const no::RunIdentity run{"journal-oracle", 1};
    const no::RunIdentity foreign{"journal-foreign", 1};
    Driven full(run, false);
    Driven windowed(run, true);
    SplitMix rng{seed};
    std::vector<no::RequestHandle> ever;       // every handle either core issued
    std::vector<no::RequestHandle> live;
    std::vector<no::CohortHandle> cohorts;
    std::vector<no::CohortReceipt> expected;   // the journal oracle's receipts
    std::vector<bool> expected_contains;
    JournalOracle oracle;
    std::map<uint64_t, std::vector<no::RequestHandle>> rosters;
    int ops = 0;

    const auto pick_handle = [&]() -> no::RequestHandle {
        const uint64_t kind = rng.below(10);
        if (kind < 5 && !live.empty()) return live[rng.below(live.size())];
        if (kind < 8 && !ever.empty()) return ever[rng.below(ever.size())];
        if (kind == 8) return no::RequestHandle{foreign, 1 + rng.below(4)};
        return no::RequestHandle{run, full.inc + rng.below(3)};  // never issued (yet)
    };
    const auto pick_cohort = [&]() -> no::CohortHandle {
        if (cohorts.empty() || rng.below(8) == 0) return no::CohortHandle{rng.below(2) * 99};
        return cohorts[rng.below(cohorts.size())];
    };
    const auto both = [&](const std::function<void(Driven&)>& step) {
        step(full);
        step(windowed);
        full.settle();
        windowed.settle();
    };

    for (int step = 0; step < 900; ++step) {
        const uint64_t op = rng.below(100);
        if (op < 25 || live.empty()) {
            // Now and then skip incarnations, so the issued index is sparse.
            if (rng.below(10) == 0) {
                const uint64_t gap = 1 + rng.below(3);
                full.inc += gap;
                windowed.inc += gap;
            }
            no::SubmitResult a, b;
            const double price = 50.0 + static_cast<double>(rng.below(40));
            both([&](Driven& d) {
                auto& out = &d == &full ? a : b;
                out = d.core.submit(limit(1.0, price), 0, d.inc, d.ord);
            });
            CHECK(a.status == b.status && a.handle == b.handle);
            if (a.handle) {
                ever.push_back(*a.handle);
                live.push_back(*a.handle);
            }
        } else if (op < 50) {
            const std::size_t index = rng.below(live.size());
            const no::RequestHandle target = live[index];
            no::ReplaceResult a, b;
            const double price = 50.0 + static_cast<double>(rng.below(40));
            both([&](Driven& d) {
                auto& out = &d == &full ? a : b;
                out = d.core.replace(target, limit(1.0, price), 0, d.inc, d.ord);
            });
            CHECK(a.status == b.status && a.successor == b.successor);
            if (a.successor) {
                ever.push_back(*a.successor);
                live[index] = *a.successor;
            }
        } else if (op < 60) {
            const std::size_t index = rng.below(live.size());
            const no::RequestHandle target = live[index];
            both([&](Driven& d) { (void)d.core.cancel(target, d.ord); });
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (op < 64 && cohorts.size() < 4) {
            no::CohortHandle a, b;
            both([&](Driven& d) { (&d == &full ? a : b) = d.core.cohort_open(); });
            CHECK(a == b);
            cohorts.push_back(a);
            rosters[a.value];
        } else if (op < 82) {
            const no::CohortHandle cohort = pick_cohort();
            const no::RequestHandle origin = pick_handle();
            // The oracle: what the pre-window core answered for this add.
            oracle.fold(full.core.history());
            no::CohortReceipt receipt{no::CohortReceiptOperation::Add,
                                      no::CohortReceiptStatus::InvalidHandle, cohort, origin};
            const bool known_cohort = cohort.value != 0 && rosters.count(cohort.value) != 0;
            if (!known_cohort) {
                receipt.status = no::CohortReceiptStatus::InvalidHandle;
            } else if (!oracle.issued(run, origin)) {
                receipt.status = no::CohortReceiptStatus::UnknownOrigin;
            } else if (!full.core.find_live(origin)) {
                receipt.status = no::CohortReceiptStatus::TerminalOrigin;
            } else {
                receipt.status = no::CohortReceiptStatus::Applied;
                auto& roster = rosters[cohort.value];
                const auto root = oracle.root(origin);
                if (std::find(roster.begin(), roster.end(), root) == roster.end()) {
                    roster.push_back(root);
                }
            }
            expected.push_back(receipt);
            both([&](Driven& d) { d.core.cohort_add(cohort, origin); });
        } else if (op < 90) {
            const no::CohortHandle cohort = pick_cohort();
            const no::RequestHandle origin = pick_handle();
            oracle.fold(full.core.history());
            no::CohortReceipt receipt{no::CohortReceiptOperation::Remove,
                                      no::CohortReceiptStatus::InvalidHandle, cohort, origin};
            const bool known_cohort = cohort.value != 0 && rosters.count(cohort.value) != 0;
            if (!known_cohort) {
                receipt.status = no::CohortReceiptStatus::InvalidHandle;
            } else if (!oracle.issued(run, origin)) {
                receipt.status = no::CohortReceiptStatus::UnknownOrigin;
            } else {
                receipt.status = no::CohortReceiptStatus::Applied;
                auto& roster = rosters[cohort.value];
                const auto root = oracle.root(origin);
                roster.erase(std::remove(roster.begin(), roster.end(), root), roster.end());
            }
            expected.push_back(receipt);
            both([&](Driven& d) { d.core.cohort_remove(cohort, origin); });
        } else {
            const no::CohortHandle cohort = pick_cohort();
            const no::RequestHandle opening = pick_handle();
            oracle.fold(full.core.history());
            bool want = false;
            const bool known_cohort = cohort.value != 0 && rosters.count(cohort.value) != 0;
            if (known_cohort && oracle.issued(run, opening)) {
                const auto& roster = rosters[cohort.value];
                want = std::find(roster.begin(), roster.end(), oracle.root(opening)) != roster.end();
            }
            expected_contains.push_back(want);
            full.contains.push_back(full.core.cohort_contains(cohort, opening));
            windowed.contains.push_back(windowed.core.cohort_contains(cohort, opening));
        }
        ++ops;
    }
    // The windowed core really retired its journal while it ran.
    CHECK(windowed.core.history().empty());
    CHECK(windowed.core.history_base() == full.core.history().size());
    CHECK(full.core.history_base() == 0);
    // Both cores answered every cohort command as the journal oracle did.
    CHECK(same_receipts(full.core.cohort_receipts(), expected));
    CHECK(same_receipts(windowed.core.cohort_receipts(), expected));
    CHECK(full.contains == expected_contains);
    CHECK(windowed.contains == expected_contains);
    CHECK(full.core.cohorts().size() == windowed.core.cohorts().size());
    for (std::size_t i = 0; i < full.core.cohorts().size(); ++i) {
        const auto& a = full.core.cohorts()[i];
        const auto& b = windowed.core.cohorts()[i];
        CHECK(a.handle == b.handle && a.origins == b.origins);
        auto want = rosters[a.handle.value];
        std::sort(want.begin(), want.end(), [](const no::RequestHandle& x, const no::RequestHandle& y) {
            return x.incarnation < y.incarnation;
        });
        CHECK(a.origins == want);
    }
    // The live tables agree definition for definition, root included.
    CHECK(full.core.live().size() == windowed.core.live().size());
    for (std::size_t i = 0; i < full.core.live().size() && i < windowed.core.live().size(); ++i) {
        const auto& a = *full.core.live()[i].definition;
        const auto& b = *windowed.core.live()[i].definition;
        CHECK(a.handle == b.handle && a.predecessor == b.predecessor && a.root == b.root);
        oracle.fold(full.core.history());
        const auto root = oracle.root(a.handle);
        CHECK((a.root ? *a.root : a.handle) == root);
    }
    CHECK(full.core.issued_incarnations() == windowed.core.issued_incarnations());
    CHECK(full.core.last_ordinal() == windowed.core.last_ordinal());
    CHECK(full.core.last_incarnation() == windowed.core.last_incarnation());
    std::size_t statuses[4] = {0, 0, 0, 0};
    for (const auto& receipt : expected) ++statuses[static_cast<int>(receipt.status)];
    std::printf("  seed %llu: %d ops, receipts applied %zu invalid %zu unknown %zu terminal %zu, "
                "%zu membership queries, %zu issued ranges\n",
                static_cast<unsigned long long>(seed), ops, statuses[0], statuses[1], statuses[2],
                statuses[3], expected_contains.size(), full.core.issued_incarnations().size());
}

// ── 3. scaling ────────────────────────────────────────────────────────────
// A fixed number of membership queries over the members of one replace chain.
// Each query is O(log) in the chain index; the journal scan the pre-window
// core made walked the chain back link by link, each link a backward search
// of the journal, so the same queries cost the chain length squared.
double chain_queries_seconds(int chain, int queries) {
    const no::RunIdentity run{"journal-scaling", 1};
    no::WorkingRequestCore core(run);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto first = core.submit(limit(1.0, 50.0), 0, inc, ord);
    std::vector<no::RequestHandle> members{*first.handle};
    no::RequestHandle tail = *first.handle;
    for (int i = 0; i < chain; ++i) {
        const auto replaced = core.replace(tail, limit(1.0, 50.0 + (i % 7)), 0, inc, ord);
        tail = *replaced.successor;
        members.push_back(tail);
    }
    const auto cohort = core.cohort_open();
    SplitMix rng{static_cast<uint64_t>(chain)};
    const std::clock_t start = std::clock();
    core.cohort_add(cohort, tail);
    std::size_t found = 0;
    for (int query = 0; query < queries; ++query) {
        found += core.cohort_contains(cohort, members[rng.below(members.size())]) ? 1 : 0;
    }
    const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    CHECK(core.cohort_receipts().back().status == no::CohortReceiptStatus::Applied);
    CHECK(found == static_cast<std::size_t>(queries));
    return seconds;
}

void membership_is_not_a_chain_walk() {
    constexpr double kMinLegSeconds = 0.1;
    constexpr int kInitialQueries = 2048;
    constexpr int kMaxQueries = 16 * 1024 * 1024;
    int queries = kInitialQueries;
    while (chain_queries_seconds(400, queries) < kMinLegSeconds
           && queries < kMaxQueries / 2) {
        queries *= 2;
    }
    // Best of three at each size, so a scheduling hiccup does not decide it.
    double small = 1e9;
    double large = 1e9;
    for (int i = 0; i < 3; ++i) {
        small = std::min(small, chain_queries_seconds(400, queries));
        large = std::min(large, chain_queries_seconds(1600, queries));
    }
    const double ratio = large / std::max(small, 1e-9);
    std::printf("  %d chain-membership queries, 400 -> 1600 replaces: %.6f s -> %.6f s "
                "(x%.2f for 4x, bound x5)\n", queries, small, large, ratio);
    CHECK(small >= kMinLegSeconds);
    CHECK(large >= kMinLegSeconds);
    CHECK(ratio < 5.0);
}

// ── 4. a deferred group-adjustment chain pins the window ─────────────────
struct GroupBook {
    no::WorkingRequestCore core;
    uint64_t inc = 1;
    uint64_t ord = 1;
    no::RequestHandle parent{}, first{}, second{}, filler{};
    no::EventId cause{};
    explicit GroupBook(const no::RunIdentity& run) : core(run) {}
};

no::EvaluationContext evaluation_at(uint64_t point) {
    no::EvaluationContext ctx;
    ctx.cursor.point.ordinal = point;
    ctx.cursor.point.effective_time_ms = 1000;
    ctx.cursor.point.provenance = NativePriceProvenance::ObservedPrint;
    ctx.driver_class = no::DriverEligibilityClass::ObservedPrint;
    ctx.existing_matching_bit = true;
    return ctx;
}

// Fill `target` for `units` at 100, as the consumer would: evaluate, then
// execute with committed facts.
no::EventId fill(no::WorkingRequestCore& core, uint64_t& ord, const no::RunIdentity& run,
                 const no::RequestHandle& target, double units, uint64_t point,
                 const no::PositionIdentity& before, const no::PositionIdentity& after) {
    no::TargetObservation observation;
    observation.current_position = before;
    auto eval = core.prepare_evaluation(target, evaluation_at(point), observation, ord);
    if (auto* mutation = std::get_if<no::PreparedMutation>(&eval)) {
        auto installed = core.install_mutation(std::move(*mutation));
        CHECK(std::holds_alternative<no::Installed>(installed));
        if (auto* ok = std::get_if<no::Installed>(&installed)) ord += ok->events.count;
    }
    no::ExecutionProposal proposal;
    const auto* live = core.find_live(target);
    CHECK(live != nullptr);
    if (!live) return {};
    proposal.cursor.point.ordinal = point;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = no::Transact{units};
    proposal.pre_fill = before;
    proposal.pre_target.current_position = before;
    proposal.inspected_opened_units = units;
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(target, proposal, ord);
    CHECK(std::holds_alternative<no::PreparedExecution>(prep));
    if (!std::holds_alternative<no::PreparedExecution>(prep)) return {};
    no::CommittedExecutionFacts facts;
    facts.result.status = execution::Status::Applied;
    facts.result.opened_units = units;
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target.current_position = after;
    facts.committed_action = no::Transact{units};
    auto installed = core.install_execution(std::get<no::PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<no::Installed>(installed));
    if (auto* ok = std::get_if<no::Installed>(&installed)) ord += ok->events.count;
    const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
    CHECK(applied != nullptr);
    return applied ? no::EventId{run, applied->ordinal} : no::EventId{};
}

std::vector<std::string> drive_group_book(bool retire) {
    const no::RunIdentity run{"journal-chain", 1};
    GroupBook book(run);
    auto& core = book.core;
    std::vector<std::string> trace;
    auto parent_req = limit(5, 50, "parent");
    book.parent = *core.submit(parent_req, 0, book.inc, book.ord).handle;
    auto child = [&](const char* label, std::int64_t cohort) {
        no::Request r{no::Reduce{no::OwnerOpenedUnits{}}, label, ""};
        r.owner = no::WaitForApplied{book.parent};
        r.group = no::Member{7, cohort, no::GroupEffect::Reduce};
        return *core.submit(r, 0, book.inc, book.ord).handle;
    };
    book.first = child("first", 2);
    book.second = child("second", 3);
    auto filler_req = limit(2, 60, "F10");
    filler_req.group = no::Member{7, 1, no::GroupEffect::Reduce};
    book.filler = *core.submit(filler_req, 0, book.inc, book.ord).handle;
    book.cause = fill(core, book.ord, run, book.filler, 2, 20, no::PositionFlat{},
                      no::PositionNonflat{1, no::Side::Long});
    for (const auto& recipient : core.group_recipients(book.cause)) {
        auto effect = core.prepare_group_effect(book.cause, recipient, book.ord);
        CHECK(std::holds_alternative<no::PreparedMutation>(effect));
        if (auto* mutation = std::get_if<no::PreparedMutation>(&effect)) {
            auto installed = core.install_mutation(std::move(*mutation));
            if (auto* ok = std::get_if<no::Installed>(&installed)) book.ord += ok->events.count;
        }
    }
    // Both children now carry a one-receipt deferred chain.
    uint64_t head = 0;
    for (const auto& event : core.history()) {
        if (std::holds_alternative<no::DeferredGroupAdjustmentEvent>(event)) {
            head = ordinal_of(event);
            break;
        }
    }
    CHECK(head != 0);
    if (retire) {
        const std::size_t retired = core.retire_history(core.last_ordinal());
        // Everything before the first chain's head goes; the head stays.
        CHECK(retired > 0);
        CHECK(core.retired_through() < head);
        CHECK(ordinal_of(core.history().front()) == head);
        CHECK(core.event_at(no::EventId{run, head}) != nullptr);
        // The group-effect receipts stay, with the chain.
        CHECK(core.group_effect_receipt_count() == 2);
    }
    // The parent fills; arming each child reads its deferred chain back.
    const auto parent_fill = fill(core, book.ord, run, book.parent, 5, 30,
                                  no::PositionNonflat{1, no::Side::Long},
                                  no::PositionNonflat{1, no::Side::Long});
    for (const auto& child_handle : {book.first, book.second}) {
        no::OpeningObservation opening;
        opening.queried_opening = book.parent;
        opening.queried_cycle = 1;
        opening.current_position = no::PositionNonflat{1, no::Side::Long};
        opening.has_live_matching_lot = true;
        auto armed = core.prepare_owner_applied(parent_fill, child_handle, opening, book.ord);
        if (auto* error = std::get_if<no::PreparationError>(&armed)) {
            trace.push_back("error " + std::to_string(static_cast<int>(error->code)));
            continue;
        }
        if (auto* mutation = std::get_if<no::PreparedMutation>(&armed)) {
            auto installed = core.install_mutation(std::move(*mutation));
            if (auto* ok = std::get_if<no::Installed>(&installed)) {
                book.ord += ok->events.count;
                for (std::size_t i = 0; i < ok->events.count; ++i) {
                    const auto& event = core.history_at(ok->events.first_index + i);
                    std::string row = std::to_string(event.index()) + "@" + std::to_string(ordinal_of(event));
                    if (const auto* bound = std::get_if<no::QuantityBoundEvent>(&event)) {
                        row += " ids";
                        for (const auto& id : bound->prior_adjustment_ids) row += " " + std::to_string(id.ordinal);
                        row += " total " + std::to_string(bound->pending_total)
                            + " deduction " + std::to_string(bound->effective_deduction);
                    }
                    trace.push_back(row);
                }
            }
        } else {
            trace.push_back("nochange");
        }
    }
    trace.push_back("live " + std::to_string(core.live().size()));
    return trace;
}

void a_live_chain_pins_the_window() {
    const auto kept = drive_group_book(false);
    const auto retired = drive_group_book(true);
    CHECK(!kept.empty());
    CHECK(kept == retired);
    bool bound = false;
    for (const auto& row : kept) bound = bound || row.find(" ids ") != std::string::npos;
    CHECK(bound);
    for (const auto& row : retired) std::printf("  %s\n", row.c_str());
}
// ── 5. a trail's arm lives in its state ──────────────────────────────────
// NativeTrailState::activation_ordinal was answered by a backward journal
// scan for the request's TrailArm event; it is now the tracking state's own
// field, so retiring the event leaves the answer where it was. A successor
// that retained its predecessor's ride never armed, and answers 0 -- what the
// scan answered for it, since the TrailArm event names the predecessor.
void a_trail_carries_its_arm_ordinal() {
    const no::RunIdentity run{"journal-trail", 1};
    no::WorkingRequestCore core(run);
    uint64_t inc = 1;
    uint64_t ord = 1;
    no::Request request{no::Transact{-1.0}, "trail", ""};
    request.trigger = no::Trail{1.0, 105.0};
    const auto accepted = core.submit(request, 0, inc, ord);
    CHECK(accepted.status == no::SubmitStatus::Accepted);
    const no::RequestHandle handle = *accepted.handle;
    const auto cursor_at = [](uint64_t point) {
        no::MatchCursor cursor;
        cursor.point.ordinal = point;
        cursor.point.effective_time_ms = 1000;
        cursor.point.provenance = NativePriceProvenance::ObservedPrint;
        return cursor;
    };
    const auto transition = [&](const no::TriggerTransition& step) -> uint64_t {
        auto prepared = core.prepare_trigger(handle, step, no::DriverEligibilityClass::ObservedPrint, ord);
        CHECK(std::holds_alternative<no::PreparedMutation>(prepared));
        if (!std::holds_alternative<no::PreparedMutation>(prepared)) return 0;
        auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(prepared)));
        CHECK(std::holds_alternative<no::Installed>(installed));
        const auto& range = std::get<no::Installed>(installed).events;
        ord += range.count;
        return ordinal_of(core.history_at(range.first_index));
    };
    const uint64_t arm = transition(no::BeginTrailTracking{cursor_at(20), 106.0});
    const auto* live = core.find_live(handle);
    CHECK(live && std::holds_alternative<no::TrailTrack>(live->trigger_state));
    if (!live || !std::holds_alternative<no::TrailTrack>(live->trigger_state)) return;
    CHECK(std::get<no::TrailTrack>(live->trigger_state).activation_ordinal == arm);
    const auto* arm_event = std::get_if<no::ActivatedEvent>(core.event_at(no::EventId{run, arm}));
    CHECK(arm_event && arm_event->kind == no::ActivationKind::TrailArm);
    CHECK(arm_event && std::get<no::TrailTrack>(arm_event->after).activation_ordinal == arm);
    // The arm event retires; the state still names it.
    CHECK(core.retire_history(core.last_ordinal()) > 0);
    CHECK(core.event_at(no::EventId{run, arm}) == nullptr);
    live = core.find_live(handle);
    CHECK(live && std::get<no::TrailTrack>(live->trigger_state).activation_ordinal == arm);
    // A successor that retains the ride armed nothing itself.
    no::ReplaceOptions retain;
    retain.retain_trigger_state = true;
    no::Request moved = request;
    moved.trigger = no::Trail{1.5, 105.0};
    const auto replaced = core.replace(handle, moved, 0, inc, ord, std::nullopt, retain);
    CHECK(replaced.status == no::ReplaceStatus::Replaced);
    const auto* successor = core.find_live(*replaced.successor);
    CHECK(successor && std::holds_alternative<no::TrailTrack>(successor->trigger_state));
    if (successor && std::holds_alternative<no::TrailTrack>(successor->trigger_state)) {
        const auto& track = std::get<no::TrailTrack>(successor->trigger_state);
        CHECK(track.activation_ordinal == 0);
        CHECK(track.best == 106.0);
    }
    // The trigger carries the arm it tracked from (its own: here 0).
    no::Request fresh{no::Transact{-1.0}, "trail-2", ""};
    fresh.trigger = no::Trail{1.0, 105.0};
    const no::RequestHandle second = *core.submit(fresh, 0, inc, ord).handle;
    auto step2 = [&](const no::TriggerTransition& step) -> uint64_t {
        auto prepared = core.prepare_trigger(second, step, no::DriverEligibilityClass::ObservedPrint, ord);
        CHECK(std::holds_alternative<no::PreparedMutation>(prepared));
        if (!std::holds_alternative<no::PreparedMutation>(prepared)) return 0;
        auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(prepared)));
        const auto& range = std::get<no::Installed>(installed).events;
        ord += range.count;
        return ordinal_of(core.history_at(range.first_index));
    };
    const uint64_t second_arm = step2(no::BeginTrailTracking{cursor_at(40), 107.0});
    const uint64_t trigger = step2(no::ActivateTrail{cursor_at(41), 105.5});
    const auto* triggered = core.find_live(second);
    CHECK(triggered && std::holds_alternative<no::TrailActive>(triggered->trigger_state));
    if (triggered && std::holds_alternative<no::TrailActive>(triggered->trigger_state)) {
        CHECK(std::get<no::TrailActive>(triggered->trigger_state).activation_ordinal == second_arm);
    }
    const auto* trigger_event = std::get_if<no::ActivatedEvent>(core.event_at(no::EventId{run, trigger}));
    CHECK(trigger_event && trigger_event->kind == no::ActivationKind::TrailTrigger);
    CHECK(trigger_event
          && std::get<no::TrailActive>(trigger_event->after).activation_ordinal == second_arm);
    std::printf("  trail arm %llu kept in state across its retirement; retained successor 0; "
                "trigger carries arm %llu\n", static_cast<unsigned long long>(arm),
                static_cast<unsigned long long>(second_arm));
}
}  // namespace

int main() {
    the_window_keeps_absolute_positions();
    for (uint64_t seed : {11ULL, 29ULL, 47ULL, 83ULL}) the_chain_index_answers_what_the_journal_did(seed);
    membership_is_not_a_chain_walk();
    a_live_chain_pins_the_window();
    a_trail_carries_its_arm_ordinal();
    if (failures != 0) {
        std::printf("test_native_journal_window: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_native_journal_window: ok (%d checks)\n", checks);
    return 0;
}
