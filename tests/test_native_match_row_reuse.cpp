// R5 lane PERF-K3: the matcher's row reuse against the full rescan it
// replaces, bit for bit, over randomized books.
//
// After an Evaluate winner's allowance refresh at an unmoved cursor,
// match_path no longer rescans every live request: it rescans the winner
// alone and takes the next winner from the rows the last scan already built,
// ordered in a heap by (t, incarnation, kind). That is exact because such a
// refresh writes nothing but the winner's allowance and the core's epoch,
// and no other request's row reads either (the argument is written at the
// reuse in src/native_execution_consumer.cpp). This row is the evidence that
// no interleaving breaks it: every configuration below runs twice on fresh
// hosts, once with the reuse (the default) and once with the consumer's
// switch turned off, which restores the full rescan after every winner and
// the core's handle lookup for the winner, and every value the two runs
// produce must be equal -- the continuation hash at every bar and at every
// applied fill (which folds the whole command history, driver log and
// account log), the broker hash, every trade, the event census and the
// host's own counters.
//
// The books are randomized (native_match_book_fixture.hpp): 1 to 100 live
// requests of every intent and trigger kind, OCA groups under both effects,
// brackets armed by fills, cohort closes, replaces and cancels every bar,
// requests born in the fill callback, and a tape that gaps through resting
// levels so fills land at t=0 among that point's refreshes -- under every
// intrabar path, calculate-on-fills, and the quantizing price grid. The
// working book must also stay in incarnation order at every bar: the
// request core's handle lookup bisects on that order, and
// the_core_finds_exactly_the_handles_it_holds, run first, holds that lookup
// equal to a walk of the book.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
using namespace k3_book;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

class SwitchedHost final : public BookHost {
public:
    SwitchedHost(const BookConfig& config, bool reuse) : BookHost(config) {
        as_native_consumer(execution_consumer()).set_match_row_reuse(reuse);
    }
};

Outcome run(const BookConfig& config, bool reuse) {
    SwitchedHost host(config, reuse);
    return run_book(host, config);
}

void describe(const BookConfig& config) {
    std::fprintf(stderr, "  seed=%llu live=%d bars=%d path=%s calc_on_fills=%d quantize=%d\n",
                 static_cast<unsigned long long>(config.seed), config.live, config.bars,
                 path_name(config.path), config.calc_on_fills ? 1 : 0,
                 config.quantize ? 1 : 0);
}

struct Totals {
    long runs = 0;
    long trades = 0;
    long applied = 0;
    long replaced = 0;
    long accepted = 0;
    std::size_t trace = 0;
};

void compare(const BookConfig& config, Totals& totals) {
    const Outcome reused = run(config, true);
    const Outcome rescanned = run(config, false);
    const int before = failures;
    CHECK(reused.completed);
    CHECK(rescanned.completed);
    CHECK(reused.error == rescanned.error);
    CHECK(reused.trace.size() == rescanned.trace.size());
    const std::size_t common = reused.trace.size() < rescanned.trace.size()
        ? reused.trace.size() : rescanned.trace.size();
    for (std::size_t index = 0; index < common; ++index) {
        if (reused.trace[index] != rescanned.trace[index]) {
            std::fprintf(stderr, "  first continuation divergence at observation %zu of %zu\n",
                         index, common);
            ++failures;
            break;
        }
    }
    CHECK(reused.continuation == rescanned.continuation);
    CHECK(reused.broker == rescanned.broker);
    CHECK(reused.trades == rescanned.trades);
    CHECK(reused.trades_digest == rescanned.trades_digest);
    CHECK(reused.events == rescanned.events);
    CHECK(reused.events_digest == rescanned.events_digest);
    CHECK(reused.position == rescanned.position);
    CHECK(reused.accepted == rescanned.accepted);
    CHECK(reused.rejected == rescanned.rejected);
    CHECK(reused.replaced == rescanned.replaced);
    CHECK(reused.cancelled == rescanned.cancelled);
    CHECK(reused.applied == rescanned.applied);
    CHECK(reused.unordered == 0);
    CHECK(rescanned.unordered == 0);
    if (failures != before) {
        describe(config);
        if (!reused.error.empty()) std::fprintf(stderr, "  error: %s\n", reused.error.c_str());
    }
    ++totals.runs;
    totals.trades += reused.trades;
    totals.applied += reused.applied;
    totals.replaced += reused.replaced;
    totals.accepted += reused.accepted;
    totals.trace += reused.trace.size();
}

// Every live-book size from one request to a hundred, each under every
// intrabar path, with and without calculate-on-fills and the price grid.
void randomized_books_match_the_full_rescan() {
    Totals totals;
    const int sizes[] = {1, 2, 3, 4, 5, 6, 8, 12, 17, 25, 33, 50, 64, 100};
    std::uint64_t seed = 1;
    for (const int live : sizes) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 4; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = live >= 50 ? 24 : (live >= 17 ? 48 : 90);
                config.path = path;
                config.calc_on_fills = (variant & 1) != 0;
                config.quantize = (variant & 2) != 0;
                compare(config, totals);
            }
        }
    }
    std::printf("randomized books: %ld configurations, %ld requests accepted, %ld replaced, "
                "%ld fills applied, %ld trades, %zu continuation observations\n",
                totals.runs, totals.accepted, totals.replaced, totals.applied, totals.trades,
                totals.trace);
    // The books must actually trade and churn, or the comparison is vacuous.
    CHECK(totals.trades > 1000);
    CHECK(totals.applied > 2000);
    CHECK(totals.replaced > 1000);
}

// The request core's handle lookup, which bisects a long working book,
// against a walk of that book, on a bare core driven through its public
// commands: randomized submits, replaces and cancels that grow the book to
// two hundred rows and shrink it back, so both the bisection and the walk
// answer. Every replace and cancel, of a live or an already retired handle,
// must answer what the walk predicts, and after every command each live
// handle, a few handles the core retired, one of another run and a zero
// incarnation are looked up.
void the_core_finds_exactly_the_handles_it_holds() {
    no::WorkingRequestCore core({"k3-core", 1});
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    Rng rng(0xC0DEull);
    std::vector<no::RequestHandle> issued;
    auto walk = [&](const no::RequestHandle& handle) -> const no::LiveRequest* {
        if (handle.incarnation == 0 || handle.run != core.identity()) return nullptr;
        for (const auto& live : core.live()) {
            if (live.handle().incarnation == handle.incarnation) return &live;
        }
        return nullptr;
    };
    auto request = [&]() {
        no::Request limit{no::Transact{rng.percent(50) ? 1.0 : -1.0}, "k3-core", ""};
        limit.trigger = no::Limit{ticks(rng.between(200, 600))};
        return limit;
    };
    long lookups = 0;
    std::size_t widest = 0;
    for (int step = 0; step < 2400; ++step) {
        const std::size_t target = step < 1200 ? 200 : 6;
        const std::size_t size = core.live().size();
        const bool grow = size < target;
        const int op = rng.below(100);
        if (issued.empty() || (grow && op < 55)) {
            const auto accepted = core.submit(request(), 1000 + step, incarnation, ordinal);
            CHECK(accepted.handle.has_value());
            if (accepted.handle) issued.push_back(*accepted.handle);
        } else {
            // Mostly a live request, sometimes one the core already retired.
            const no::RequestHandle target_handle = size > 0 && rng.percent(70)
                ? core.live()[static_cast<std::size_t>(rng.below(static_cast<int>(size)))].handle()
                : issued[static_cast<std::size_t>(rng.below(static_cast<int>(issued.size())))];
            const bool live = walk(target_handle) != nullptr;
            if ((grow && op < 85) || (!grow && op < 30)) {
                const auto replaced = core.replace(target_handle, request(), 1000 + step,
                                                   incarnation, ordinal);
                CHECK((replaced.status == no::ReplaceStatus::Replaced) == live);
                if (replaced.successor) issued.push_back(*replaced.successor);
            } else {
                const auto cancelled = core.cancel(target_handle, ordinal);
                CHECK((cancelled.status == no::CancelStatus::Cancelled) == live);
            }
        }
        if (core.live().size() > widest) widest = core.live().size();
        for (const auto& live : core.live()) {
            ++lookups;
            CHECK(core.find_live(live.handle()) == &live);
        }
        for (int probe = 0; probe < 6; ++probe) {
            const auto& handle =
                issued[static_cast<std::size_t>(rng.below(static_cast<int>(issued.size())))];
            ++lookups;
            CHECK(core.find_live(handle) == walk(handle));
        }
        no::RequestHandle foreign{{"k3-other", 1}, issued.back().incarnation};
        no::RequestHandle later_run{{"k3-core", 2}, issued.back().incarnation};
        no::RequestHandle zero{{"k3-core", 1}, 0};
        CHECK(core.find_live(foreign) == nullptr);
        CHECK(core.find_live(later_run) == nullptr);
        CHECK(core.find_live(zero) == nullptr);
        lookups += 3;
    }
    std::printf("request core: %zu handles issued, widest book %zu, %ld lookups\n",
                issued.size(), widest, lookups);
    CHECK(widest >= 200);
}

}  // namespace

int main() {
    // The lookup first: every book below leans on it.
    the_core_finds_exactly_the_handles_it_holds();
    randomized_books_match_the_full_rescan();
    std::printf("%d checks\n", checks);
    if (failures == 0) std::printf("test_native_match_row_reuse: ok\n");
    return failures == 0 ? 0 : 1;
}
