// R5 lane PERF-P7 (P7a), re-targeted by integration INT20: the request core's
// chain index against a whole-journal oracle, on consumer-driven books.
//
// P7a gave the consumer an incarnation -> definition index that answered
// WorkingRequestCore::definition_for's backward journal scan -- the lookup
// cohort_add's canonical walk and cohort_contains made once per link of a
// replace chain, which PERF0-P measured at 70 % of slot 055 (a far stop
// re-issued daily and joined to its cohort at each re-issue). Lane V19-B
// deleted definition_for: the core keeps a chain index of its own (the issued
// incarnation ranges, each replace successor's root, RequestDefinition::root)
// and answers cohort_add, cohort_remove and cohort_contains from it, without
// the journal -- which the default retention (NativeEventRetention::Window)
// now retires at every script-bar boundary. INT20 removed P7a's index, its
// thread-local publication and its switch; this row keeps P7a's two workloads
// and holds the core's answers to what the pre-window core answered,
// recomputed from a whole journal:
//   - the randomized books of test_native_match_row_reuse
//     (native_match_book_fixture.hpp: replaces, cancels, brackets, OCA groups,
//     rosters built from origins that may be working, filled, replaced or
//     cancelled, and host-sized closes bound to them) under every intrabar
//     path, fill recalculation and the price grid;
//   - a replace-chain host that re-prices resting entries every bar for the
//     whole run, joins every successor to its cohort, joins terminal and
//     unknown origins, removes some, and closes the cohort.
//
// Every configuration runs twice on fresh hosts:
//   kept      the fixture's spec as it is (NativeEventRetention::Full), so
//             the journal is whole. The oracle -- the predecessor of every
//             definition an Accepted or Replaced event carries, followed to
//             the chain's first handle, as the pre-window scan read them --
//             predicts every cohort receipt the chain host asks for and the
//             rosters it builds, and every membership answer the core gives
//             at every bar: for each open lot, the chain's members, handles
//             never issued, and a cohort that does not exist; every roster
//             member a book holds is its own oracle root.
//   windowed  the same spec with the event record read back as Window
//             (set_retention_override: the spec and its digest stay the
//             kept run's), so the core retires its journal at every
//             script-bar boundary and answers from the chain index alone.
//             It must equal the kept run on the continuation at every bar
//             and fill, the broker hash, every trade, the position, every
//             cohort receipt and roster and every membership answer, and it
//             must really have retired.
// Source-free, so the kernel-only profile registers it too.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// The core's cohort receipts and rosters, as values.
std::uint64_t cohort_digest(const no::WorkingRequestCore& core) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& receipt : core.cohort_receipts()) {
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(receipt.operation));
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(receipt.status));
        h = k3_book::fnv_u64(h, receipt.cohort.value);
        h = k3_book::fnv_u64(h, receipt.origin.incarnation);
    }
    for (const auto& roster : core.cohorts()) {
        h = k3_book::fnv_u64(h, roster.handle.value);
        for (const auto& origin : roster.origins) h = k3_book::fnv_u64(h, origin.incarnation);
    }
    return h;
}

// What the pre-window core answered, recomputed from a whole journal (the
// fold of tests/test_native_journal_window.cpp's JournalOracle, extended
// over the events appended since the last fold: a kept journal only grows).
struct JournalOracle {
    std::map<std::uint64_t, std::optional<no::RequestHandle>> predecessor;
    std::size_t folded = 0;

    void fold(const no::WorkingRequestCore& core) {
        CHECK(core.history_base() == 0);
        const auto& history = core.history();
        for (; folded < history.size(); ++folded) {
            const auto& event = history[folded];
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
    // cohort_contains before the window: the opening's chain meets the roster.
    bool contains(const no::WorkingRequestCore& core, no::CohortHandle cohort,
                  const no::RequestHandle& opening) const {
        if (cohort.value == 0) return false;
        const auto& rosters = core.cohorts();
        const auto roster = std::find_if(rosters.begin(), rosters.end(),
            [&](const no::CohortRoster& row) { return row.handle == cohort; });
        if (roster == rosters.end() || !issued(core.identity(), opening)) return false;
        return std::find(roster->origins.begin(), roster->origins.end(), root(opening))
            != roster->origins.end();
    }
};

// The membership questions a host's bar asks the core, and the answers: the
// kept run checks each against the oracle as it asks it; the windowed run
// only records them, and must record the same.
struct Membership {
    bool kept = true;
    JournalOracle oracle;
    std::vector<bool> answers;
    std::uint64_t oracle_answers = 0;

    void ask(const no::WorkingRequestCore& core, const std::vector<no::RequestHandle>& handles) {
        if (kept) oracle.fold(core);
        std::vector<no::CohortHandle> cohorts{no::CohortHandle{0}, no::CohortHandle{999}};
        for (const auto& roster : core.cohorts()) cohorts.push_back(roster.handle);
        for (const auto cohort : cohorts) {
            for (const auto& handle : handles) {
                const bool answer = core.cohort_contains(cohort, handle);
                answers.push_back(answer);
                if (!kept) continue;
                CHECK(answer == oracle.contains(core, cohort, handle));
                ++oracle_answers;
            }
        }
        if (!kept) return;
        // A roster holds chain roots only: each member is its own root.
        for (const auto& roster : core.cohorts()) {
            for (const auto& origin : roster.origins) {
                CHECK(oracle.issued(core.identity(), origin));
                CHECK(oracle.root(origin) == origin);
            }
        }
    }
};

// The handles a bar asks about: every open lot, the given ones, and two the
// core never issued.
std::vector<no::RequestHandle> questions(const NativeStrategyHost& host,
                                         const no::WorkingRequestCore& core, double mark,
                                         const std::vector<no::RequestHandle>& extra) {
    std::vector<no::RequestHandle> out;
    for (const auto& lot : host.native_open_lots(mark)) {
        out.push_back(no::RequestHandle{core.identity(), lot.entry_incarnation});
    }
    out.insert(out.end(), extra.begin(), extra.end());
    out.push_back(no::RequestHandle{core.identity(), core.last_incarnation() + 1});
    out.push_back(no::RequestHandle{core.identity(), core.last_incarnation() + 1000000});
    return out;
}

struct Summary {
    k3_book::Outcome outcome;
    std::uint64_t cohorts = 0;
    std::vector<no::CohortReceipt> receipts;
    std::vector<bool> memberships;
    std::uint64_t oracle_answers = 0;
    std::size_t retired = 0;  // journal events the core retired (history_base)
};

void collect(const NativeExecutionConsumer& consumer, const Membership& membership, Summary& out) {
    const auto& core = consumer.request_core();
    out.cohorts = cohort_digest(core);
    out.receipts = core.cohort_receipts();
    out.memberships = membership.answers;
    out.oracle_answers = membership.oracle_answers;
    out.retired = core.history_base();
}

void retain(NativeExecutionConsumer& consumer, bool kept) {
    // The windowed twin keeps its spec (and the spec's digest) and reads its
    // record back as the default retention does: the core retires its journal.
    if (!kept) consumer.set_retention_override(NativeEventRetention::Window);
}

// --- the randomized books ---------------------------------------------------

class CheckedBook final : public k3_book::BookHost {
public:
    CheckedBook(const k3_book::BookConfig& config, bool kept) : BookHost(config) {
        membership_.kept = kept;
        retain(as_native_consumer(execution_consumer()), kept);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        const auto& core = as_native_consumer(execution_consumer()).request_core();
        membership_.ask(core, questions(*this, core, bar.close, {}));
        BookHost::on_native_bar(bar, context);
    }
    Summary summary() {
        Summary out;
        out.outcome = outcome;
        collect(as_native_consumer(execution_consumer()), membership_, out);
        return out;
    }

private:
    Membership membership_;
};

Summary run_book(const k3_book::BookConfig& config, bool kept) {
    CheckedBook host(config, kept);
    k3_book::run_book(host, config);
    return host.summary();
}

// --- the replace chains -------------------------------------------------------

struct ChainConfig {
    std::uint64_t seed = 1;
    int bars = 400;
    int resting = 4;
};

class ChainHost final : public NativeStrategyHost {
public:
    ChainHost(const ChainConfig& config, bool kept)
        : config_(config), rng_(config.seed), pick_(~config.seed) {
        membership_.kept = kept;
        retain(as_native_consumer(execution_consumer()), kept);
    }

    std::vector<std::uint64_t> trace;

    void on_native_run_begin() override {
        rng_ = k3_book::Rng(config_.seed);
        pick_ = k3_book::Rng(~config_.seed);
        resting_.clear();
        retired_.clear();
        cohort_.reset();
        rosters_.clear();
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        trace.push_back(native_continuation_hash());
        {
            std::vector<no::RequestHandle> members = resting_;
            const std::size_t recent = std::min<std::size_t>(retired_.size(), 8);
            members.insert(members.end(), retired_.end() - static_cast<std::ptrdiff_t>(recent),
                           retired_.end());
            if (!retired_.empty()) {
                members.push_back(retired_[static_cast<std::size_t>(
                    pick_.below(static_cast<int>(retired_.size())))]);
            }
            membership_.ask(core(), questions(*this, core(), bar.close, members));
        }
        if (!cohort_) {
            cohort_ = cohort_open();
            rosters_[cohort_->value];
        }
        const long ref = static_cast<long>(bar.close * 4.0);
        // Keep the resting entries working, far enough below the tape that
        // most of them never fill, and re-price every one of them every bar:
        // each replace lengthens its chain, and each successor joins the
        // cohort as an origin, as the Pine adapter joins every re-issue.
        while (static_cast<int>(resting_.size()) < config_.resting) {
            no::Request entry{no::Transact{1.0}, "p7a-rest", ""};
            entry.trigger = no::Limit{k3_book::ticks(ref - rng_.between(8, 60))};
            const auto accepted = submit(entry);
            if (!accepted.handle) break;
            resting_.push_back(*accepted.handle);
            add(*cohort_, *accepted.handle);
        }
        for (auto& handle : resting_) {
            no::Request entry{no::Transact{1.0}, "p7a-rest", ""};
            const long away = rng_.percent(6) ? rng_.between(0, 2) : rng_.between(8, 60);
            entry.trigger = no::Limit{k3_book::ticks(ref - away)};
            const auto result = replace(handle, entry);
            if (result.successor) {
                retired_.push_back(handle);
                handle = *result.successor;
                add(*cohort_, handle);
            }
        }
        // Rosters built from origins in every state: a replaced (terminal)
        // origin, an unknown handle, an invalid cohort, a removal.
        if (!retired_.empty() && rng_.percent(20)) {
            add(*cohort_, retired_[static_cast<std::size_t>(
                rng_.below(static_cast<int>(retired_.size())))]);
        }
        if (rng_.percent(5)) {
            no::RequestHandle unknown = resting_.empty() ? no::RequestHandle{} : resting_.front();
            unknown.incarnation += 1000000;
            add(*cohort_, unknown);
        }
        if (rng_.percent(3) && !resting_.empty()) add(no::CohortHandle{999}, resting_.back());
        if (!retired_.empty() && rng_.percent(10)) {
            remove(*cohort_, retired_[static_cast<std::size_t>(
                rng_.below(static_cast<int>(retired_.size())))]);
        }
        // A host-sized close bound to the roster: every candidate asks the
        // core which lots belong to it.
        if (rng_.percent(12)) {
            no::Request close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "p7a-c", ""};
            close.owner = no::BindCohort{*cohort_};
            if (rng_.percent(50)) close.trigger = no::Limit{k3_book::ticks(ref + rng_.between(-2, 10))};
            (void)submit(close);
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        trace.push_back(native_continuation_hash());
        // A filled entry leaves the resting set; the next bar replaces it
        // with a fresh chain.
        for (auto it = resting_.begin(); it != resting_.end(); ++it) {
            if (*it == event.handle()) {
                retired_.push_back(*it);
                resting_.erase(it);
                break;
            }
        }
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)
            && facts.scope_exposure_units > 0.0) {
            terms.units = facts.scope_exposure_units < 2.0 ? facts.scope_exposure_units : 2.0;
        }
        return terms;
    }

    Summary summary() {
        Summary out;
        out.outcome.trace = trace;
        out.outcome.completed = native_state().kind == NativeLifecycleKind::Completed;
        out.outcome.error = last_error();
        out.outcome.continuation = native_continuation_hash();
        out.outcome.broker = broker_state_hash();
        out.outcome.trades = trade_count();
        out.outcome.position = physical_position().signed_units;
        collect(as_native_consumer(execution_consumer()), membership_, out);
        // The rosters the oracle built receipt by receipt are the core's.
        if (membership_.kept) {
            CHECK(core().cohorts().size() == rosters_.size());
            for (const auto& roster : core().cohorts()) {
                auto want = rosters_[roster.handle.value];
                std::sort(want.begin(), want.end(),
                          [](const no::RequestHandle& a, const no::RequestHandle& b) {
                              return a.incarnation < b.incarnation;
                          });
                CHECK(roster.origins == want);
            }
        }
        return out;
    }

private:
    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }

    // cohort_add / cohort_remove with the kept run's oracle prediction of the
    // receipt each one writes, and of the roster it leaves.
    void add(no::CohortHandle cohort, const no::RequestHandle& origin) {
        predict(no::CohortReceiptOperation::Add, cohort, origin);
        cohort_add(cohort, origin);
        verify();
    }
    void remove(no::CohortHandle cohort, const no::RequestHandle& origin) {
        predict(no::CohortReceiptOperation::Remove, cohort, origin);
        cohort_remove(cohort, origin);
        verify();
    }
    void predict(no::CohortReceiptOperation operation, no::CohortHandle cohort,
                 const no::RequestHandle& origin) {
        if (!membership_.kept) return;
        auto& oracle = membership_.oracle;
        oracle.fold(core());
        expected_ = no::CohortReceipt{operation, no::CohortReceiptStatus::InvalidHandle, cohort,
                                      origin};
        const bool known = cohort.value != 0 && rosters_.count(cohort.value) != 0;
        if (!known) return;
        if (!oracle.issued(core().identity(), origin)) {
            expected_.status = no::CohortReceiptStatus::UnknownOrigin;
            return;
        }
        auto& roster = rosters_[cohort.value];
        const auto root = oracle.root(origin);
        if (operation == no::CohortReceiptOperation::Add) {
            if (!core().find_live(origin)) {
                expected_.status = no::CohortReceiptStatus::TerminalOrigin;
                return;
            }
            expected_.status = no::CohortReceiptStatus::Applied;
            if (std::find(roster.begin(), roster.end(), root) == roster.end()) roster.push_back(root);
        } else {
            expected_.status = no::CohortReceiptStatus::Applied;
            roster.erase(std::remove(roster.begin(), roster.end(), root), roster.end());
        }
    }
    void verify() {
        if (!membership_.kept) return;
        const auto& receipts = core().cohort_receipts();
        CHECK(!receipts.empty());
        if (receipts.empty()) return;
        const auto& receipt = receipts.back();
        CHECK(receipt.operation == expected_.operation && receipt.status == expected_.status
              && receipt.cohort == expected_.cohort && receipt.origin == expected_.origin);
        ++membership_.oracle_answers;
    }

    ChainConfig config_;
    k3_book::Rng rng_;
    k3_book::Rng pick_;  // the questions' own draws: the workload's stream is P7a's
    Membership membership_;
    std::vector<no::RequestHandle> resting_;
    std::vector<no::RequestHandle> retired_;
    std::optional<no::CohortHandle> cohort_;
    std::map<std::uint64_t, std::vector<no::RequestHandle>> rosters_;
    no::CohortReceipt expected_{};
};

Summary run_chains(const ChainConfig& config, bool kept) {
    ChainHost host(config, kept);
    k3_book::BookConfig book;
    book.seed = config.seed;
    book.bars = config.bars;
    const k3_book::Tape tape = k3_book::make_tape(book);
    const auto setup = host.configure_native(k3_book::make_spec(book, tape));
    if (setup.status != NativeSetupStatus::Applied) return {};
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    return host.summary();
}

// --- the comparison -----------------------------------------------------------

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

// Everything but the event record, which only the kept run keeps whole.
bool same(const Summary& a, const Summary& b) {
    return a.outcome.trace == b.outcome.trace
        && a.outcome.continuation == b.outcome.continuation
        && a.outcome.broker == b.outcome.broker
        && a.outcome.trades == b.outcome.trades
        && a.outcome.trades_digest == b.outcome.trades_digest
        && a.outcome.position == b.outcome.position
        && a.outcome.accepted == b.outcome.accepted
        && a.outcome.rejected == b.outcome.rejected
        && a.outcome.replaced == b.outcome.replaced
        && a.outcome.cancelled == b.outcome.cancelled
        && a.outcome.applied == b.outcome.applied
        && a.outcome.error == b.outcome.error
        && a.outcome.completed == b.outcome.completed
        && a.cohorts == b.cohorts
        && same_receipts(a.receipts, b.receipts)
        && a.memberships == b.memberships;
}

struct Totals {
    long runs = 0;
    long trades = 0;
    std::size_t receipts = 0;
    std::uint64_t oracle_answers = 0;
    std::uint64_t memberships = 0;
    std::uint64_t retired = 0;
    std::uint64_t kept_retired = 0;
};

void tally(const Summary& kept, const Summary& windowed, Totals& totals) {
    CHECK(same(kept, windowed));
    CHECK(kept.outcome.completed);
    CHECK(kept.outcome.error.empty());
    // The windowed core answered with its journal retired; the kept one kept it.
    CHECK(windowed.retired > 0);
    ++totals.runs;
    totals.trades += kept.outcome.trades;
    totals.receipts += kept.receipts.size();
    totals.oracle_answers += kept.oracle_answers;
    totals.memberships += kept.memberships.size();
    totals.retired += windowed.retired;
    totals.kept_retired += kept.retired;
}

void randomized_books_agree(Totals& totals) {
    const k3_book::Path paths[] = {k3_book::Path::None, k3_book::Path::Synthesized,
                                   k3_book::Path::Lower};
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        k3_book::BookConfig config;
        config.seed = seed * 104729u;
        config.live = static_cast<int>(1 + (seed * 7) % 40);
        config.bars = 90;
        config.path = paths[seed % 3];
        config.calc_on_fills = seed % 4 == 1;
        config.quantize = seed % 5 == 2;
        const Summary kept = run_book(config, true);
        const Summary windowed = run_book(config, false);
        if (!same(kept, windowed)) {
            std::fprintf(stderr, "  book seed=%llu live=%d path=%s calc_on_fills=%d quantize=%d\n",
                         static_cast<unsigned long long>(config.seed), config.live,
                         k3_book::path_name(config.path), config.calc_on_fills ? 1 : 0,
                         config.quantize ? 1 : 0);
        }
        tally(kept, windowed, totals);
    }
}

void replace_chains_agree(Totals& totals) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        ChainConfig config;
        config.seed = seed * 7727u;
        config.bars = 240 + static_cast<int>(seed) * 20;
        config.resting = 1 + static_cast<int>(seed % 4);
        const Summary kept = run_chains(config, true);
        const Summary windowed = run_chains(config, false);
        if (!same(kept, windowed)) {
            std::fprintf(stderr, "  chains seed=%llu bars=%d resting=%d\n",
                         static_cast<unsigned long long>(config.seed), config.bars,
                         config.resting);
        }
        tally(kept, windowed, totals);
    }
}

} // namespace

int main() {
    Totals totals;
    randomized_books_agree(totals);
    replace_chains_agree(totals);
    // The runs are not trivial, the oracle answered, the windowed cores
    // retired while the kept ones retired nothing.
    CHECK(totals.trades > totals.runs * 5);
    CHECK(totals.receipts > static_cast<std::size_t>(totals.runs) * 50);
    CHECK(totals.oracle_answers > static_cast<std::uint64_t>(totals.runs) * 100);
    CHECK(totals.kept_retired == 0);
    std::printf("test_native_definition_index: %ld runs, %ld trades, %zu cohort receipts, "
                "%llu membership answers, %llu oracle answers, %llu journal events retired "
                "by the windowed twins; %d checks, %d failures\n",
                totals.runs, totals.trades, totals.receipts,
                static_cast<unsigned long long>(totals.memberships),
                static_cast<unsigned long long>(totals.oracle_answers),
                static_cast<unsigned long long>(totals.retired), checks, failures);
    return failures == 0 ? 0 : 1;
}
