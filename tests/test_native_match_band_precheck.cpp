// R5 lane PERF-L5: the matcher's band pre-check against the full evaluation
// it short-cuts, bit for bit, over randomized books aimed at the band's edges.
//
// match_path tests a priced trigger (a stop, a stop-limit's stop or limit, a
// limit, a trail's arm or its stop) against the rest of the driver point's
// path before it reads the rest of the row, and passes the request over when
// the path cannot reach the trigger. It does so only where the full
// evaluation would decide the row on that same geometry alone -- the
// request's allowance is already the point's, and no provenance row names it
// -- and it computes the geometry with the one function the full evaluation
// uses (the argument is written at the pre-check in
// src/native_execution_consumer.cpp). The matcher's row buffer also keeps
// its rows as an ascending run while they stay one, taking the next winner
// from the run's front instead of a heap's top. This row is the evidence
// that no book, band or edge breaks either: every configuration below runs
// on fresh hosts with the pre-check on and off (the consumer's
// set_match_band_precheck), and with K3's row reuse on and off, and every
// value the runs produce must be equal -- the continuation hash at every bar
// and every applied fill (which folds the whole command history, driver log
// and account log), the broker hash, every trade, every event's kind and
// ordinal with the cursor and prices of every fill and activation, and the
// host's own counters.
//
// Two kinds of book: native_match_band_fixture.hpp's, whose levels sit on the
// next bars' opens, highs, lows and closes and one tick either side of them
// (and an eighth of a tick under the quantizing grid), with gaps through
// them, ties at one level, every trigger kind and intent, brackets and
// requests born in the fill callback; and K3's randomized book
// (native_match_book_fixture.hpp), 1 to 100 live requests. Both run under
// every intrabar path, calculate-on-fills and the quantizing grid.
#include "../src/native_execution_consumer.hpp"
#include "native_match_band_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
using k3_book::BookConfig;
using k3_book::Outcome;
using k3_book::Path;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

struct Switches {
    bool precheck = true;
    bool reuse = true;
};

class SwitchedEdgeHost final : public l5_band::EdgeHost {
public:
    SwitchedEdgeHost(const BookConfig& config, const k3_book::Tape& tape, Switches switches)
        : EdgeHost(config, tape) {
        auto& consumer = pineforge::as_native_consumer(execution_consumer());
        consumer.set_match_band_precheck(switches.precheck);
        consumer.set_match_row_reuse(switches.reuse);
    }
};

class SwitchedBookHost final : public k3_book::BookHost {
public:
    SwitchedBookHost(const BookConfig& config, Switches switches) : BookHost(config) {
        auto& consumer = pineforge::as_native_consumer(execution_consumer());
        consumer.set_match_band_precheck(switches.precheck);
        consumer.set_match_row_reuse(switches.reuse);
    }
};

void describe(const char* book, const BookConfig& config) {
    std::fprintf(stderr, "  %s book seed=%llu live=%d bars=%d path=%s calc_on_fills=%d quantize=%d\n",
                 book, static_cast<unsigned long long>(config.seed), config.live, config.bars,
                 k3_book::path_name(config.path), config.calc_on_fills ? 1 : 0,
                 config.quantize ? 1 : 0);
}

// Every value two runs of one configuration produced must be equal.
bool same(const Outcome& a, const Outcome& b) {
    const int before = failures;
    CHECK(a.completed);
    CHECK(b.completed);
    CHECK(a.error == b.error);
    CHECK(a.trace.size() == b.trace.size());
    const std::size_t common = a.trace.size() < b.trace.size() ? a.trace.size() : b.trace.size();
    for (std::size_t index = 0; index < common; ++index) {
        if (a.trace[index] != b.trace[index]) {
            std::fprintf(stderr, "  first continuation divergence at observation %zu of %zu\n",
                         index, common);
            ++failures;
            break;
        }
    }
    CHECK(a.continuation == b.continuation);
    CHECK(a.broker == b.broker);
    CHECK(a.trades == b.trades);
    CHECK(a.trades_digest == b.trades_digest);
    CHECK(a.events == b.events);
    CHECK(a.events_digest == b.events_digest);
    CHECK(a.position == b.position);
    CHECK(a.accepted == b.accepted);
    CHECK(a.rejected == b.rejected);
    CHECK(a.replaced == b.replaced);
    CHECK(a.cancelled == b.cancelled);
    CHECK(a.applied == b.applied);
    CHECK(a.unordered == 0);
    CHECK(b.unordered == 0);
    return failures == before;
}

struct Totals {
    long runs = 0;
    long trades = 0;
    long applied = 0;
    long replaced = 0;
    long accepted = 0;
    std::size_t events = 0;
    std::size_t trace = 0;
    void add(const Outcome& outcome) {
        ++runs;
        trades += outcome.trades;
        applied += outcome.applied;
        replaced += outcome.replaced;
        accepted += outcome.accepted;
        events += outcome.events;
        trace += outcome.trace.size();
    }
};

Outcome run_edges(const BookConfig& config, const k3_book::Tape& tape, Switches switches) {
    SwitchedEdgeHost host(config, tape, switches);
    return l5_band::run_edges(host, config, tape);
}

// The edge books: 2 to 60 live requests, each under every intrabar path,
// with and without calculate-on-fills and the quantizing grid. The pre-check
// is compared on and off under the row reuse, and on and off without it.
void edge_books_match_the_full_evaluation() {
    Totals totals;
    const int sizes[] = {2, 3, 5, 8, 9, 13, 20, 32, 60};
    std::uint64_t seed = 7001;
    for (const int live : sizes) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 4; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 32 ? 30 : (live >= 13 ? 45 : 70);
                config.path = path;
                config.calc_on_fills = (variant & 1) != 0;
                config.quantize = (variant & 2) != 0;
                const k3_book::Tape tape = k3_book::make_tape(config);
                const Outcome checked = run_edges(config, tape, {true, true});
                const Outcome full = run_edges(config, tape, {false, true});
                bool equal = same(checked, full);
                // Without the row reuse, the pre-check also covers every row
                // of every full rescan.
                if (variant == 0 || live >= 8) {
                    const Outcome checked_rescan = run_edges(config, tape, {true, false});
                    const Outcome full_rescan = run_edges(config, tape, {false, false});
                    equal = same(checked_rescan, full_rescan) && equal;
                    equal = same(checked, full_rescan) && equal;
                }
                if (!equal) describe("edge", config);
                totals.add(checked);
            }
        }
    }
    std::printf("edge books: %ld configurations, %ld requests accepted, %ld replaced, "
                "%ld fills applied, %ld trades, %zu events, %zu continuation observations\n",
                totals.runs, totals.accepted, totals.replaced, totals.applied, totals.trades,
                totals.events, totals.trace);
    // The books must trade and churn, or the comparison is vacuous.
    CHECK(totals.trades > 1000);
    CHECK(totals.applied > 2000);
    CHECK(totals.replaced > 1000);
}

// K3's randomized books: every live-book size from one request to a hundred.
void randomized_books_match_the_full_evaluation() {
    Totals totals;
    const int sizes[] = {1, 4, 6, 12, 25, 50, 100};
    std::uint64_t seed = 9001;
    for (const int live : sizes) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 4; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 50 ? 20 : (live >= 12 ? 40 : 70);
                config.path = path;
                config.calc_on_fills = (variant & 1) != 0;
                config.quantize = (variant & 2) != 0;
                SwitchedBookHost checked_host(config, {true, true});
                SwitchedBookHost full_host(config, {false, true});
                const Outcome checked = k3_book::run_book(checked_host, config);
                const Outcome full = k3_book::run_book(full_host, config);
                if (!same(checked, full)) describe("randomized", config);
                totals.add(checked);
            }
        }
    }
    std::printf("randomized books: %ld configurations, %ld requests accepted, %ld replaced, "
                "%ld fills applied, %ld trades, %zu events\n",
                totals.runs, totals.accepted, totals.replaced, totals.applied, totals.trades,
                totals.events);
    CHECK(totals.trades > 1000);
    CHECK(totals.applied > 2000);
}

}  // namespace

int main() {
    edge_books_match_the_full_evaluation();
    randomized_books_match_the_full_evaluation();
    std::printf("%d checks\n", checks);
    if (failures == 0) std::printf("test_native_match_band_precheck: ok\n");
    return failures == 0 ? 0 : 1;
}
