/*
 * test_same_bar_add_exit_coverage.cpp — KI-62 keep-vs-scratch (probe-pinned).
 *
 * When a pre-queued SAME-ID MARKET pyramid add and a from_entry PRICED bracket
 * exit both fill on the same bar, TradingView's open-tick fill priority is
 *   buy-market-like (market + triggered buy stops)
 *     -> sell-market-like (market + triggered sell stops)
 *       -> gapped-through limit orders (both sides, at the open, last).
 * The exit covers the add (the add scratches dur-0) iff the add's fill
 * precedes-or-ties the exit's fill in that sequence. Intrabar exit fills
 * (open inside the bracket, level hit AFTER the open) process after every
 * open fill, so they always cover an open-filled add.
 *
 * Mapping the collision (add is always a market order; the exit closes the
 * position so it is the opposite side):
 *   add prio  = LONG add -> buy(1),  SHORT add -> sell(2)
 *   exit prio = gapped STOP -> (SHORT pos buy-stop = 1, LONG pos sell-stop = 2)
 *               gapped LIMIT -> 3 (either side)
 *               intrabar     -> 4 (after all open fills)
 *   scratch iff add_prio <= exit_prio.
 *
 *   LONG  + gap-stop : add buy(1)  <= sell-stop(2)  -> SCRATCH (SCR-OPEN)
 *   LONG  + gap-limit: add buy(1)  <= sell-limit(3) -> SCRATCH (SCR-OPEN)
 *   SHORT + gap-stop : add sell(2) vs  buy-stop(1)  -> KEEP  (exit sized pre-add)
 *   SHORT + gap-limit: add sell(2) <= buy-limit(3)  -> SCRATCH (SCR-OPEN)
 *   intrabar (either): add(open)   <  exit(4)       -> SCRATCH (SCR-INTRA, pnl!=0)
 *
 * The scratch materializes as a dur-0 trade for the add slice: entry at the
 * add's fill price, exit at the exit's fill price (== the open for gapped
 * scratches -> pnl 0; == the bracket level for intrabar -> pnl != 0).
 *
 * The current engine (b6e4e35) is uniform-KEEP: the from_entry bracket freezes
 * its reserved qty to the pre-add lot at arm time and always fills before the
 * same-dir add at the open, so every add carries (0 dur-0). The scratch cells
 * below FAIL pre-fix (position carries, 1 trade) and PASS post-fix (flat,
 * 2 trades); the KEEP / control / pyr=1 guards pass PRE and POST.
 */

#include <cmath>
#include <cstdio>
#include <limits>

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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// Common probe base: fixed 1-lot sizing, no slippage/commission, tick 0.01.
class ProbeBase : public BacktestEngine {
public:
    explicit ProbeBase(int pyr) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = pyr;
        syminfo_mintick_ = 0.01;
    }
    double pos_size() const { return signed_position_size(); }
    int n_trades() const { return (int)trades_.size(); }
    // Last emitted trade (the add scratch in a scratch cell).
    const Trade& last_trade() const { return trades_.back(); }
    bool last_is_dur0() const {
        const Trade& t = trades_.back();
        return t.entry_bar_index == t.exit_bar_index;
    }
    double last_pnl() const { return trades_.back().pnl; }
};

// One collision fixture. `is_long` picks the position side; `limit`/`stop`
// arm the from_entry bracket; the bar list carries the collision geometry.
// bar0 queues base "P"; bar1 fills it @100 and (on_bar) arms the bracket +
// queues the same-id market add; bar2 is the collision; bar3 settles.
struct Fixture {
    bool is_long;
    double limit;
    double stop;
    bool queue_add;
};

class CollisionProbe : public ProbeBase {
public:
    CollisionProbe(int pyr, Fixture f) : ProbeBase(pyr), f_(f) {}
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("P", f_.is_long);            // base lot1 (market)
        }
        if (bar_index_ == 1) {
            if (f_.queue_add) strategy_entry("P", f_.is_long, kNaN, kNaN, kNaN, "ADD");
            strategy_exit("Px", "P", /*limit=*/f_.limit, /*stop=*/f_.stop);
        }
    }
private:
    Fixture f_;
};

static void run4(CollisionProbe& p, const Bar (&bars)[4]) { p.run(bars, 4); }

// ── LONG + gap-through-STOP → SCRATCH (SCR-OPEN, pnl 0) ──────────────────
static void test_long_gap_stop_scratch() {
    std::printf("LONG gap-stop -> SCRATCH\n");
    CollisionProbe p(2, {true, /*limit=*/110.0, /*stop=*/98.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),   // bar0: queue base P
        mk(100, 100, 100, 100, 1'200'000),   // bar1: P fills @100; arm Px + queue add
        mk( 97,  97,  96,  96, 1'800'000),   // bar2: open 97 <= stop 98 (gap) → collision
        mk( 96,  96,  96,  96, 2'400'000),   // bar3: settle
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));    // pre-fix: +1 (add carried)
    CHECK(p.n_trades() == 2);          // pre-fix: 1
    CHECK(p.last_is_dur0());
    CHECK(near(p.last_pnl(), 0.0));    // gapped scratch at the open
}

// ── LONG + gap-through-LIMIT → SCRATCH (SCR-OPEN, pnl 0) ─────────────────
static void test_long_gap_limit_scratch() {
    std::printf("LONG gap-limit -> SCRATCH\n");
    CollisionProbe p(2, {true, /*limit=*/103.0, /*stop=*/90.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(104, 105, 104, 104, 1'800'000),   // bar2: open 104 >= limit 103 (gap)
        mk(104, 104, 104, 104, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));    // pre-fix: +1
    CHECK(p.n_trades() == 2);
    CHECK(p.last_is_dur0());
    CHECK(near(p.last_pnl(), 0.0));
}

// ── SHORT + gap-through-STOP → KEEP (the only keep cell) ─────────────────
// buy-stop exit (prio 1) fills before the sell add (prio 2); the exit is
// sized to the pre-add lot, so the add survives and carries. Unchanged
// pre- and post-fix — this is the cell the two uniform-scratch attempts broke.
static void test_short_gap_stop_keep() {
    std::printf("SHORT gap-stop -> KEEP (guard)\n");
    CollisionProbe p(2, {false, /*limit=*/97.0, /*stop=*/102.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(103, 104, 103, 103, 1'800'000),   // bar2: open 103 >= stop 102 (gap up)
        mk(103, 103, 103, 103, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), -1.0));   // add carried → SHORT 1 (pre + post)
    CHECK(p.n_trades() == 1);          // only lot1 closed
}

// ── SHORT + gap-through-LIMIT → SCRATCH (SCR-OPEN, pnl 0) ────────────────
static void test_short_gap_limit_scratch() {
    std::printf("SHORT gap-limit -> SCRATCH\n");
    CollisionProbe p(2, {false, /*limit=*/98.0, /*stop=*/110.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk( 97,  97,  96,  96, 1'800'000),   // bar2: open 97 <= limit 98 (gap down)
        mk( 96,  96,  96,  96, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));    // pre-fix: -1 (add carried)
    CHECK(p.n_trades() == 2);
    CHECK(p.last_is_dur0());
    CHECK(near(p.last_pnl(), 0.0));
}

// ── Intrabar LONG → SCRATCH at bracket level (SCR-INTRA, pnl != 0) ───────
static void test_intrabar_long_scratch() {
    std::printf("intrabar LONG -> SCRATCH (pnl!=0)\n");
    CollisionProbe p(2, {true, /*limit=*/110.0, /*stop=*/98.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 101,  97,  99, 1'800'000),   // bar2: open 100 inside; low 97 hits stop 98
        mk( 99,  99,  99,  99, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));    // pre-fix: +1
    CHECK(p.n_trades() == 2);
    CHECK(p.last_is_dur0());
    CHECK(!near(p.last_pnl(), 0.0));   // add entered @100, exits @98 → pnl != 0
    CHECK(near(p.last_pnl(), -2.0));   // (98 - 100) * 1
}

// ── Intrabar SHORT → SCRATCH at bracket level (SCR-INTRA, pnl != 0) ──────
static void test_intrabar_short_scratch() {
    std::printf("intrabar SHORT -> SCRATCH (pnl!=0)\n");
    CollisionProbe p(2, {false, /*limit=*/90.0, /*stop=*/102.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 103,  99, 101, 1'800'000),   // bar2: open 100 inside; high 103 hits stop 102
        mk(101, 101, 101, 101, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));    // pre-fix: -1
    CHECK(p.n_trades() == 2);
    CHECK(p.last_is_dur0());
    CHECK(near(p.last_pnl(), -2.0));   // short add @100 → exit @102 → (100-102)*1
}

// ── No-collision control (no add) — unchanged pre/post ──────────────────
static void test_no_collision_control() {
    std::printf("no-collision control (guard)\n");
    CollisionProbe p(2, {true, /*limit=*/110.0, /*stop=*/98.0, /*add=*/false});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk( 97,  97,  96,  96, 1'800'000),   // bar2: exit fires, lot1 closes
        mk( 96,  96,  96,  96, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));
    CHECK(p.n_trades() == 1);          // only lot1; no add slice
}

// ── Pyramiding cap: pyr=1 drops the over-cap add (no scratch) ────────────
// probe65 pin. The add is over cap at fill (count 1 >= pyr 1) → never opens →
// no collision, no scratch. Unchanged pre/post.
static void test_pyr1_add_dropped() {
    std::printf("pyr=1 over-cap add dropped (guard)\n");
    CollisionProbe p(1, {true, /*limit=*/110.0, /*stop=*/98.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk( 97,  97,  96,  96, 1'800'000),
        mk( 96,  96,  96,  96, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));    // add dropped → only lot1 closes → FLAT
    CHECK(p.n_trades() == 1);
}

// ── Pyramiding cap: pyr=2 fills the add → scratch (mirror of long gap-stop) ─
static void test_pyr2_add_fills_scratch() {
    std::printf("pyr=2 add fills → SCRATCH\n");
    CollisionProbe p(2, {true, /*limit=*/110.0, /*stop=*/98.0, /*add=*/true});
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk( 97,  97,  96,  96, 1'800'000),
        mk( 96,  96,  96,  96, 2'400'000),
    };
    run4(p, bars);
    CHECK(near(p.pos_size(), 0.0));
    CHECK(p.n_trades() == 2);          // pre-fix: 1 (add carried)
    CHECK(p.last_is_dur0());
}

int main() {
    test_long_gap_stop_scratch();
    test_long_gap_limit_scratch();
    test_short_gap_stop_keep();
    test_short_gap_limit_scratch();
    test_intrabar_long_scratch();
    test_intrabar_short_scratch();
    test_no_collision_control();
    test_pyr1_add_dropped();
    test_pyr2_add_fills_scratch();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
