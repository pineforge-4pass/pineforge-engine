// test_intraday_cap_auto_close.cpp — pin down TV's
// ``strategy.risk.max_intraday_filled_orders(N)`` semantics:
//
//   1. The Nth fill on a chart-day is allowed AND immediately followed by
//      a synthetic full close at the same fill_price tagged
//      "Close Position (Max number of filled orders in one day)".
//   2. After that auto-close, every subsequent fill on the same chart-day
//      is silently rejected (LATCH-TILL-DAY-ROLLOVER).
//   3. The first fill of the next chart-day is accepted (latch + counter
//      reset on day rollover).
//   4. The synthetic close exits at the entry's fill price -> per-pyramid
//      PnL is zero before commission (here commission=0, so trade.pnl=0).
//
// Why this is the high-signal test path: TV emits ~one cap-close per
// chart-day where the cap fires, NOT multiple per day (probe 97b: 382
// cap-closes across 13 months of data). A prior fix that recharged the
// counter after each cap-cycle over-fired cap-closes and produced 3459
// engine vs 1957 TV trades on probe 97b (43% over-count). The latch
// pinned here matches TV's true semantics.
//
// The fixture uses 15m bars so the cap counter and the chart-day rollover
// are both exercised within a small bar count. Fills are driven by raw
// ``strategy_entry`` calls inside ``on_bar`` rather than crossover signals
// so the test pins the engine's cap-state machine, not indicator math.

#include <cmath>
#include <cstdio>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

namespace {

// Bar timestamps in ms. Anchor at 2025-03-31 00:00 UTC, 15m cadence so the
// chart-day rollover (UTC 00:00 here, since we leave chart_tz unset) is
// crisp and a handful of bars covers two distinct chart-days.
//
//   2025-03-31 00:00 UTC -> 1743379200000 ms
//   add 900_000 ms (15 min) per bar
constexpr int64_t kT0_UTC = 1743379200000LL;       // 2025-03-31 00:00 UTC
constexpr int64_t k15m_ms = 900'000LL;
constexpr int64_t kNextDay_UTC = kT0_UTC + 86'400'000LL;  // 2025-04-01 00:00 UTC

// ── Test 1: cap=2 latches after first cap-close, releases on day rollover ──
//
// Layout (10 bars total):
//   bar 0..5 = Day A (Mar 31, 6 bars)
//   bar 6..9 = Day B (Apr 1, 4 bars)
//
// on_bar enqueues one strategy.entry per bar; with default
// calc_on_order_fills=false each queued entry fills at the next bar's
// open. The 97-residual fix added a PLACEMENT-time gate in
// strategy_entry: while the cap is latched, strategy.entry calls inside
// on_bar are silently dropped (matching Pine docs: "all subsequent
// orders are blocked until the start of the next trading day"). So
// L2/L3/L4 never even enter the pending queue on Day A — there is no
// pending order carried across the day boundary. Day B must place its
// own fresh entries to trigger another cap-cycle.
//
// With cap=2:
//
//   Day A:
//     bar 0: L0 placed (queued)
//     bar 1: L0 fills @ 101 (count=1); L1 placed
//     bar 2: L1 fills @ 102 (count=2 -> CAP -> synthetic close at 102,
//            LATCH SET); L2 placement BLOCKED (latched)
//     bar 3: L3 placement BLOCKED
//     bar 4: L4 placement BLOCKED
//     bar 5: (no fills — queue empty since latch); L5 placement BLOCKED
//   Day B (latch resets on rollover):
//     bar 6: L6 placed (latch reset on chart-day rollover)
//     bar 7: L6 fills @ 111 (count=1); L7 placed
//     bar 8: L7 fills @ 112 (count=2 -> CAP -> synthetic close at 112,
//            LATCH SET for Day B); L8 placement BLOCKED
//     bar 9: L9 placement BLOCKED
//
// Expected trades:
//   bar 2 close: 2 trades (L0 closes @ 102, L1 self-closes @ 102)
//   bar 8 close: 2 trades (L6 closes @ 112, L7 self-closes @ 112)
// Total = 4 trades. Position FLAT at end.
void test_cap_latches_until_day_rollover() {
    std::printf("test_cap_latches_until_day_rollover\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 10;
            max_intraday_filled_orders_ = 2;
        }
        int queued_count = 0;
        void on_bar(const Bar&) override {
            std::string id = "L" + std::to_string(queued_count);
            strategy_entry(id, true);
            ++queued_count;
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100, 101, 99,  100, 50, kT0_UTC + 0 * k15m_ms},  // bar 0: L0 placed
        {101, 102, 100, 101, 50, kT0_UTC + 1 * k15m_ms},  // bar 1: L0 fills (count=1); L1 placed
        {102, 103, 101, 102, 50, kT0_UTC + 2 * k15m_ms},  // bar 2: L1 fills (count=2 -> cap, close, latch); L2 placement BLOCKED
        {103, 104, 102, 103, 50, kT0_UTC + 3 * k15m_ms},  // bar 3: L3 placement BLOCKED
        {104, 105, 103, 104, 50, kT0_UTC + 4 * k15m_ms},  // bar 4: L4 placement BLOCKED
        {105, 106, 104, 105, 50, kT0_UTC + 5 * k15m_ms},  // bar 5: L5 placement BLOCKED
        {110, 111, 109, 110, 50, kNextDay_UTC + 0 * k15m_ms},  // bar 6: rollover; L6 placed
        {111, 112, 110, 111, 50, kNextDay_UTC + 1 * k15m_ms},  // bar 7: L6 fills (count=1); L7 placed
        {112, 113, 111, 112, 50, kNextDay_UTC + 2 * k15m_ms},  // bar 8: L7 fills (count=2 -> cap, close, latch); L8 placement BLOCKED
        {113, 114, 112, 113, 50, kNextDay_UTC + 3 * k15m_ms},  // bar 9: L9 placement BLOCKED
    };
    strat.run(bars, 10);

    // 2 cap-cycles -> 4 trades total.
    CHECK(strat.trade_count() == 4);
    CHECK(std::fabs(strat.get_signed_position_size()) < 1e-9);

    // Every emitted trade is tagged with TV's verbatim cap-close comment.
    const std::string kCapMsg = "Close Position (Max number of filled orders in one day)";
    for (int i = 0; i < strat.trade_count(); ++i) {
        const std::string& ec = strat.get_trade(i).exit_comment;
        CHECK(ec == kCapMsg);
    }

    // Cap-triggering self-close trades have entry_price == exit_price -> pnl == 0.
    // Two cap-cycles -> 2 zero-pnl trades (L1 and L7).
    int zero_pnl_count = 0;
    for (int i = 0; i < strat.trade_count(); ++i) {
        if (std::fabs(strat.get_trade(i).pnl) < 1e-9) ++zero_pnl_count;
    }
    CHECK(zero_pnl_count == 2);

    // Only L0, L1, L6, L7 should appear. L2..L5 are blocked at placement
    // (latched on Day A); L8/L9 are blocked at placement (latched on Day B).
    auto has_id = [&](const char* id) {
        for (int i = 0; i < strat.trade_count(); ++i) {
            if (strat.get_trade(i).entry_id == id) return true;
        }
        return false;
    };
    CHECK(has_id("L0"));
    CHECK(has_id("L1"));
    CHECK(!has_id("L2"));  // placement blocked (Day A latch)
    CHECK(!has_id("L3"));  // placement blocked (Day A latch)
    CHECK(!has_id("L4"));  // placement blocked (Day A latch)
    CHECK(!has_id("L5"));  // placement blocked (Day A latch)
    CHECK(has_id("L6"));   // released by day rollover
    CHECK(has_id("L7"));
    CHECK(!has_id("L8"));  // placement blocked (Day B latch)
    CHECK(!has_id("L9"));  // placement blocked (Day B latch)
}

// ── Test 2: cap=0 (unlimited) is the no-op fast path ─────────────────────
//
// Regression guard: with the cap disabled (default), no synthetic close
// must ever fire, no latch must engage, and every entry must produce one
// open trade.
void test_cap_disabled_does_not_inject_auto_close() {
    std::printf("test_cap_disabled_does_not_inject_auto_close\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 10;
            // Leave max_intraday_filled_orders_ at 0 (unlimited).
        }
        void on_bar(const Bar&) override {
            std::string id = "L" + std::to_string(bar_index_);
            strategy_entry(id, true);
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100, 101, 99,  100, 50, kT0_UTC + 0 * k15m_ms},
        {101, 102, 100, 101, 50, kT0_UTC + 1 * k15m_ms},
        {102, 103, 101, 102, 50, kT0_UTC + 2 * k15m_ms},
        {103, 104, 102, 103, 50, kT0_UTC + 3 * k15m_ms},
    };
    strat.run(bars, 4);

    // 4 on_bar calls; the last queue won't have a next bar to fill at, so
    // 3 entries fill (bars 1..3). All stay open (no auto-close).
    CHECK(strat.trade_count() == 0);
    CHECK(std::fabs(strat.get_signed_position_size() - 3.0) < 1e-9);
}

}  // namespace

int main() {
    test_cap_latches_until_day_rollover();
    test_cap_disabled_does_not_inject_auto_close();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
