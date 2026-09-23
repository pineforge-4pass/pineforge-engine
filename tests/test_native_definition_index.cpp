// R5 lane PERF-P7 (P7a): the consumer's incarnation -> definition index
// against the backward history scan it replaces, bit for bit.
//
// WorkingRequestCore::definition_for answers a request that is no longer
// working by scanning the command history backwards for the newest event
// that names it -- an accepted request's definition, or a replace's
// predecessor or successor (the predecessor first). cohort_add canonicalizes
// every origin through its whole replace chain, one scan per link, and a
// cohort-bound close asks cohort_contains the same for every lot at every
// candidate, so a request re-priced for a long time made each cohort_add walk
// ever further back (PERF0-P measured slot 055, a far stop re-issued daily
// and joined to its cohort at each re-issue, at 70 % in that scan). The
// consumer now keeps an index from incarnation to the event the scan would
// stop at, folded over the history in the scan's own precedence (the newest
// event wins; inside a replace the predecessor wins), verified against the
// history on every lookup, and published to the core only for the calls it
// makes (the core gains no member: native_order_v6's layout is frozen).
//
// This row is the evidence: every configuration runs twice on fresh hosts,
// once with the index (the default) and once with the consumer's reference
// switch off, and the continuation hash at every bar and every fill, the
// broker hash, every trade, the event census, and every cohort receipt and
// roster must be equal. Two workloads: the randomized books of
// test_native_match_row_reuse (native_match_book_fixture.hpp: replaces,
// cancels, brackets, OCA groups, rosters built from origins that may be
// working, filled, replaced or cancelled, and host-sized closes bound to
// them) under every intrabar path, fill recalculation and the price grid;
// and a replace-chain host that re-prices resting entries every bar for the
// whole run, joins every successor to its cohort, removes some, joins
// terminal and unknown origins, and closes the cohort. The indexed runs must
// have answered from the index and the reference runs never. Source-free, so
// the kernel-only profile registers it too.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"

#include <cstdint>
#include <cstdio>
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

struct Summary {
    k3_book::Outcome outcome;
    std::uint64_t cohorts = 0;
    std::size_t receipts = 0;
    std::uint64_t answered = 0;
};

// --- the randomized books ---------------------------------------------------

class SwitchedBook final : public k3_book::BookHost {
public:
    SwitchedBook(const k3_book::BookConfig& config, bool indexed) : BookHost(config) {
        as_native_consumer(execution_consumer()).set_definition_index(indexed);
    }
    Summary summary() {
        Summary out;
        out.outcome = outcome;
        const auto& consumer = as_native_consumer(execution_consumer());
        out.cohorts = cohort_digest(consumer.request_core());
        out.receipts = consumer.request_core().cohort_receipts().size();
        out.answered = consumer.definition_index_answers();
        return out;
    }
};

Summary run_book(const k3_book::BookConfig& config, bool indexed) {
    SwitchedBook host(config, indexed);
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
    ChainHost(const ChainConfig& config, bool indexed) : config_(config), rng_(config.seed) {
        as_native_consumer(execution_consumer()).set_definition_index(indexed);
    }

    std::vector<std::uint64_t> trace;

    void on_native_run_begin() override {
        rng_ = k3_book::Rng(config_.seed);
        resting_.clear();
        retired_.clear();
        cohort_.reset();
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        trace.push_back(native_continuation_hash());
        if (!cohort_) cohort_ = cohort_open();
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
            cohort_add(*cohort_, *accepted.handle);
        }
        for (auto& handle : resting_) {
            no::Request entry{no::Transact{1.0}, "p7a-rest", ""};
            const long away = rng_.percent(6) ? rng_.between(0, 2) : rng_.between(8, 60);
            entry.trigger = no::Limit{k3_book::ticks(ref - away)};
            const auto result = replace(handle, entry);
            if (result.successor) {
                retired_.push_back(handle);
                handle = *result.successor;
                cohort_add(*cohort_, handle);
            }
        }
        // Rosters built from origins in every state: a replaced (terminal)
        // origin, an unknown handle, an invalid cohort, a removal.
        if (!retired_.empty() && rng_.percent(20)) {
            cohort_add(*cohort_, retired_[static_cast<std::size_t>(
                rng_.below(static_cast<int>(retired_.size())))]);
        }
        if (rng_.percent(5)) {
            no::RequestHandle unknown = resting_.empty() ? no::RequestHandle{} : resting_.front();
            unknown.incarnation += 1000000;
            cohort_add(*cohort_, unknown);
        }
        if (rng_.percent(3) && !resting_.empty()) cohort_add(no::CohortHandle{999}, resting_.back());
        if (!retired_.empty() && rng_.percent(10)) {
            cohort_remove(*cohort_, retired_[static_cast<std::size_t>(
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
        out.outcome.events = native_events(0).size();
        const auto& consumer = as_native_consumer(execution_consumer());
        out.cohorts = cohort_digest(consumer.request_core());
        out.receipts = consumer.request_core().cohort_receipts().size();
        out.answered = consumer.definition_index_answers();
        return out;
    }

private:
    ChainConfig config_;
    k3_book::Rng rng_;
    std::vector<no::RequestHandle> resting_;
    std::vector<no::RequestHandle> retired_;
    std::optional<no::CohortHandle> cohort_;
};

Summary run_chains(const ChainConfig& config, bool indexed) {
    ChainHost host(config, indexed);
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

bool same(const Summary& a, const Summary& b) {
    return a.outcome.trace == b.outcome.trace
        && a.outcome.continuation == b.outcome.continuation
        && a.outcome.broker == b.outcome.broker
        && a.outcome.trades == b.outcome.trades
        && a.outcome.trades_digest == b.outcome.trades_digest
        && a.outcome.events == b.outcome.events
        && a.outcome.events_digest == b.outcome.events_digest
        && a.outcome.position == b.outcome.position
        && a.outcome.error == b.outcome.error
        && a.outcome.completed == b.outcome.completed
        && a.cohorts == b.cohorts
        && a.receipts == b.receipts;
}

struct Totals {
    long runs = 0;
    long trades = 0;
    std::size_t receipts = 0;
    std::uint64_t answered = 0;
    std::uint64_t reference_answered = 0;
};

void tally(const Summary& indexed, const Summary& scanned, Totals& totals) {
    CHECK(same(indexed, scanned));
    CHECK(indexed.outcome.completed);
    CHECK(indexed.outcome.error.empty());
    ++totals.runs;
    totals.trades += indexed.outcome.trades;
    totals.receipts += indexed.receipts;
    totals.answered += indexed.answered;
    totals.reference_answered += scanned.answered;
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
        const Summary indexed = run_book(config, true);
        const Summary scanned = run_book(config, false);
        if (!same(indexed, scanned)) {
            std::fprintf(stderr, "  book seed=%llu live=%d path=%s calc_on_fills=%d quantize=%d\n",
                         static_cast<unsigned long long>(config.seed), config.live,
                         k3_book::path_name(config.path), config.calc_on_fills ? 1 : 0,
                         config.quantize ? 1 : 0);
        }
        tally(indexed, scanned, totals);
    }
}

void replace_chains_agree(Totals& totals) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        ChainConfig config;
        config.seed = seed * 7727u;
        config.bars = 240 + static_cast<int>(seed) * 20;
        config.resting = 1 + static_cast<int>(seed % 4);
        const Summary indexed = run_chains(config, true);
        const Summary scanned = run_chains(config, false);
        if (!same(indexed, scanned)) {
            std::fprintf(stderr, "  chains seed=%llu bars=%d resting=%d\n",
                         static_cast<unsigned long long>(config.seed), config.bars,
                         config.resting);
        }
        tally(indexed, scanned, totals);
    }
}

} // namespace

int main() {
    Totals totals;
    randomized_books_agree(totals);
    replace_chains_agree(totals);
    // The runs are not trivial, the index answered, the reference scanned.
    CHECK(totals.trades > totals.runs * 5);
    CHECK(totals.receipts > static_cast<std::size_t>(totals.runs) * 50);
    CHECK(totals.answered > static_cast<std::uint64_t>(totals.runs) * 100);
    CHECK(totals.reference_answered == 0);
    std::printf("test_native_definition_index: %ld runs, %ld trades, %zu cohort receipts, "
                "%llu indexed answers; %d checks, %d failures\n",
                totals.runs, totals.trades, totals.receipts,
                static_cast<unsigned long long>(totals.answered), checks, failures);
    return failures == 0 ? 0 : 1;
}
