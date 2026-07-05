/*
 * test_exit_id_scoped_erase.cpp — strategy.exit's replace-erase must be
 * scoped to the matching prior EXIT order, never a pending entry.
 *
 * Pine v6 contract: entry-order ids and exit-order ids live in
 * INDEPENDENT namespaces. A ``strategy.exit(id=X, from_entry=Y)`` call
 * replaces only a prior pending EXIT order with the same (id, from_entry).
 * It must NOT delete:
 *   (a) a same-bar pending ``strategy.entry(id=X)`` that reuses the id
 *       string — otherwise the position never opens (zero trades);
 *   (c) a sibling exit ``(id=X, from_entry=other)`` bound to a different
 *       entry.
 * And it MUST still:
 *   (b) replace a prior exit with the same (id, from_entry).
 *
 * Regression for the bare ``o.id == id`` erase predicate in
 * clear_existing_exit_order (engine_strategy_commands.cpp), which deleted
 * the still-pending entry when a script reused one id for both
 * strategy.entry and strategy.exit on the same bar.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

static const double NA = std::numeric_limits<double>::quiet_NaN();

namespace {

struct BarSpec { double o, h, l, c; };

static std::vector<Bar> make_bars(const std::vector<BarSpec>& specs) {
    std::vector<Bar> out;
    out.reserve(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
        Bar b;
        b.open = specs[i].o;
        b.high = specs[i].h;
        b.low = specs[i].l;
        b.close = specs[i].c;
        b.volume = 1000.0;
        b.timestamp = (int64_t)((i + 1) * 60'000);
        out.push_back(b);
    }
    return out;
}

// Common probe shell: fixed qty=1, no commission/slippage.
class ExitProbe : public BacktestEngine {
public:
    ExitProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 1;
    }

    // Count pending orders with the given id, split by whether they are
    // EXIT orders. Helpers read the protected pending_orders_ queue.
    int count_id(const std::string& id) const {
        int n = 0;
        for (const auto& o : pending_orders_) if (o.id == id) ++n;
        return n;
    }
    int count_id_type(const std::string& id, bool is_exit) const {
        int n = 0;
        for (const auto& o : pending_orders_)
            if (o.id == id && (o.type == OrderType::EXIT) == is_exit) ++n;
        return n;
    }
    int count_exit(const std::string& id, const std::string& from_entry) const {
        int n = 0;
        for (const auto& o : pending_orders_)
            if (o.type == OrderType::EXIT && o.id == id && o.from_entry == from_entry) ++n;
        return n;
    }
    double exit_limit(const std::string& id, const std::string& from_entry) const {
        for (const auto& o : pending_orders_)
            if (o.type == OrderType::EXIT && o.id == id && o.from_entry == from_entry)
                return o.limit_price;
        return NA;
    }
    // trades_ is protected on the engine; expose it for main()'s asserts.
    size_t trade_count() const { return trades_.size(); }
    const Trade& trade_at(size_t i) const { return trades_[i]; }
};

}  // namespace

// Scenario (a): same-bar strategy.entry(id X) + strategy.exit(id X,
// from_entry X). The entry must survive the exit's replace-erase, fill on
// the next bar, and the exit must become that position's bracket.
static void test_same_bar_entry_and_exit_same_id() {
    std::printf("test_same_bar_entry_and_exit_same_id\n");
    struct Probe : public ExitProbe {
        int entry_orders_at_bar0 = -1;
        int exit_orders_at_bar0 = -1;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                // Reuse the SAME id "X" for both entry and exit.
                strategy_entry("X", true, NA, NA, 1.0, "enter");
                strategy_exit("X", "X", /*limit=*/105.0, /*stop=*/NA,
                              NA, NA, NA, 100.0, "bracket");
                // Pre-fix, the bare id erase in clear_existing_exit_order
                // deleted the pending entry here, leaving only the exit.
                entry_orders_at_bar0 = count_id_type("X", /*is_exit=*/false);
                exit_orders_at_bar0 = count_id_type("X", /*is_exit=*/true);
            }
        }
    };

    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},   // bar 0: place entry + exit (same id)
        {100.0, 101.0,  99.0, 100.5},  // bar 1: market entry fills @ open 100
        {100.5, 106.0, 100.0, 105.0},  // bar 2: high 106 >= 105 → bracket fills
        {105.0, 105.5, 104.0, 105.0},  // bar 3: idle
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    // The entry order must NOT be clobbered by the same-id exit.
    CHECK(p.entry_orders_at_bar0 == 1);
    CHECK(p.exit_orders_at_bar0 == 1);
    // End-to-end: the entry fills and the exit brackets it into one trade.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const auto& tr = p.trade_at(0);
        CHECK(tr.entry_id == "X");
        CHECK(near(tr.entry_price, 100.0));
        CHECK(near(tr.exit_price, 105.0));   // exit became the bracket
    }
}

// Scenario (b): re-issuing strategy.exit(id X, from_entry X) replaces the
// prior exit order (existing behavior preserved). The old bracket must be
// gone — only the replacement fires.
static void test_reissued_exit_replaces_prior() {
    std::printf("test_reissued_exit_replaces_prior\n");
    struct Probe : public ExitProbe {
        int exits_after_replace = -1;
        double limit_after_replace = NA;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("X", true, NA, NA, 1.0, "enter");   // market long
            }
            if (bar_index_ == 1) {
                // Position is open (filled @ bar 1 open). Arm a far TP.
                strategy_exit("X", "X", /*limit=*/105.0, NA,
                              NA, NA, NA, 100.0, "tp-105");
            }
            if (bar_index_ == 2) {
                // Replace with a FARTHER TP. If the old 105 survived it
                // would fire on bar 3; only the replacement (110) should.
                strategy_exit("X", "X", /*limit=*/110.0, NA,
                              NA, NA, NA, 100.0, "tp-110");
                exits_after_replace = count_exit("X", "X");
                limit_after_replace = exit_limit("X", "X");
            }
        }
    };

    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},   // bar 0: place entry
        {100.0, 101.0,  99.0, 100.5},  // bar 1: entry fills @ 100; arm tp-105
        {100.5, 101.5, 100.0, 101.0},  // bar 2: replace with tp-110
        {101.0, 106.0, 100.5, 105.5},  // bar 3: high 106 hits OLD 105, not 110
        {105.5, 111.0, 105.0, 110.5},  // bar 4: high 111 hits replacement 110
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    // Exactly one exit pending after the replace, at the new limit.
    CHECK(p.exits_after_replace == 1);
    CHECK(near(p.limit_after_replace, 110.0));
    // Behaviorally: one trade, closed at the REPLACEMENT price (110), not
    // the erased 105 — proving the prior exit was removed.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.trade_at(0).exit_price, 110.0));
    }
}

// Scenario (c): strategy.exit(id X, from_entry EB) must NOT clobber an
// existing strategy.exit(id X, from_entry EA). Two pyramided entries each
// keep their own bracket even though the exit ids collide.
static void test_exit_same_id_distinct_from_entry_coexist() {
    std::printf("test_exit_same_id_distinct_from_entry_coexist\n");
    struct Probe : public ExitProbe {
        int exits_id_X = -1;
        int exit_EA = -1;
        int exit_EB = -1;
        Probe() { pyramiding_ = 2; }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("EA", true, NA, NA, 1.0, "enterA");
                strategy_entry("EB", true, NA, NA, 1.0, "enterB");
            }
            if (bar_index_ == 1) {
                // Both entries filled @ bar 1 open. Two brackets, SAME
                // exit id "X" but distinct from_entry.
                strategy_exit("X", "EA", /*limit=*/105.0, NA,
                              NA, NA, NA, 100.0, "tpA");
                strategy_exit("X", "EB", /*limit=*/110.0, NA,
                              NA, NA, NA, 100.0, "tpB");
                // Pre-fix, the second exit's bare id erase would delete the
                // first (both id "X"), leaving a single bracket.
                exits_id_X = count_id("X");
                exit_EA = count_exit("X", "EA");
                exit_EB = count_exit("X", "EB");
            }
        }
    };

    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},   // bar 0: place EA + EB
        {100.0, 101.0,  99.0, 100.5},  // bar 1: both fill @ 100; arm two brackets
        {100.5, 106.0, 100.0, 105.0},  // bar 2: high 106 → EA bracket (105) fills
        {105.0, 111.0, 104.5, 110.0},  // bar 3: high 111 → EB bracket (110) fills
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    // Both brackets coexist — the id collision does not erase either.
    CHECK(p.exits_id_X == 2);
    CHECK(p.exit_EA == 1);
    CHECK(p.exit_EB == 1);
    // Behaviorally: both pyramided lots close via a surviving bracket
    // (pre-fix, one bracket was erased and its lot was left stranded).
    // Per-lot bracket→price mapping is governed by the engine's FIFO exit
    // accounting, not by this fix, so we only assert both lots exit at a
    // real bracket level rather than pinning which lot took which price.
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        for (size_t i = 0; i < 2; ++i) {
            double e = p.trade_at(i).exit_price;
            CHECK(near(e, 105.0) || near(e, 110.0));
        }
    }
}

int main() {
    test_same_bar_entry_and_exit_same_id();
    test_reissued_exit_replaces_prior();
    test_exit_same_id_distinct_from_entry_coexist();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
