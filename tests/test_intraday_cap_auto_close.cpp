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

// A no-op same-direction MARKET attempt at pyramiding=0 is not a broker fill
// in TradingView.  It therefore must not consume max_intraday_filled_orders.
// Run the Regime source shape in both directions: a later-bar signal reissues
// the same direction while the first position remains live.  With cap=2 the
// real first fill is below the cap, while counting the later no-op as fill #2
// would spuriously flatten and latch.
void test_noop_market_attempt_does_not_consume_cap(bool is_long) {
    std::printf("test_noop_market_attempt_does_not_consume_cap(%s)\n",
                is_long ? "long" : "short");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : is_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 2;
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
        }
        bool is_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E", is_long);
            } else if (bar_index_ == 1) {
                strategy_entry("REDUNDANT", is_long);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat(is_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC},
        {101, 103, 99, 102, 50, kT0_UTC + k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);
    CHECK(std::fabs(strat.get_signed_position_size()
                    - (is_long ? 1.0 : -1.0)) < 1e-9);
}

// A POOC MARKET entry that reaches the intraday cap is accepted at the signal
// close, but TV schedules the risk-generated flatten for the next broker
// boundary.  For ordinary bars that boundary is the next bar's open.
void test_pooc_cap_close_defers_to_next_open(bool is_long) {
    std::printf("test_pooc_cap_close_defers_to_next_open(%s)\n",
                is_long ? "long" : "short");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : is_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 1;
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
        }
        bool is_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("E", is_long);
        }
    };

    Strat strat(is_long);
    Bar bars[] = {
        {100, 120, 80, is_long ? 110.0 : 90.0, 50,
         kT0_UTC + 0 * k15m_ms},
        {is_long ? 111.0 : 89.0, 115, 85, 100, 50,
         kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    const std::string kCapMsg =
        "Close Position (Max number of filled orders in one day)";
    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        const Trade& trade = strat.get_trade(0);
        CHECK(trade.entry_time == bars[0].timestamp);
        CHECK(trade.exit_time == bars[1].timestamp);
        CHECK(std::fabs(trade.exit_price - bars[1].open) < 1e-9);
        CHECK(trade.exit_comment == kCapMsg);
        CHECK(trade.exit_id.empty());
    }
}

// Fill-time role controls for factor A.  A same-tick close must make the
// following entry a real opening fill, and an opposite entry must remain a
// real reversal.  Neither may be mistaken for a same-direction no-op merely
// because the order was created while a position existed.
void test_noop_filter_preserves_same_tick_close_then_reentry(bool is_long) {
    std::printf("test_noop_filter_preserves_same_tick_close_then_reentry(%s)\n",
                is_long ? "long" : "short");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : is_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 10;
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
        }
        bool is_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E", is_long);
            } else if (bar_index_ == 1) {
                strategy_close("E", "", std::nan(""), std::nan(""),
                               /*immediately=*/true);
                strategy_entry("E2", is_long);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat(is_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.get_signed_position_size()
                    - (is_long ? 1.0 : -1.0)) < 1e-9);
}

void test_noop_filter_preserves_same_tick_reversal(bool starts_long) {
    std::printf("test_noop_filter_preserves_same_tick_reversal(%s)\n",
                starts_long ? "long-to-short" : "short-to-long");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : starts_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 10;
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
        }
        bool starts_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("FIRST", starts_long);
            } else if (bar_index_ == 1) {
                strategy_entry("REVERSE", !starts_long);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat(starts_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.get_signed_position_size()
                    - (starts_long ? -1.0 : 1.0)) < 1e-9);
}

// Removing phantom no-op fills exposes the complementary POOC close path:
// strategy.close(id) is a real broker fill even though the same-bar batch does
// not pass through apply_filled_order_to_state.  When it is fill #N it must
// latch the day and block a later entry.
void test_pooc_strategy_close_consumes_cap(bool is_long) {
    std::printf("test_pooc_strategy_close_consumes_cap(%s)\n",
                is_long ? "long" : "short");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : is_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 2;
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        bool is_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E", is_long);
            } else if (bar_index_ == 1) {
                strategy_close("E");
            } else if (bar_index_ == 2) {
                strategy_entry("LATE", is_long);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat(is_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
        {102, 104, 100, 103, 50, kT0_UTC + 2 * k15m_ms},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.get_signed_position_size()) < 1e-9);
}

// A close plus opposite MARKET entry on the same POOC tick is one TV reversal
// fill, even though the engine scheduler materializes a close before the entry.
// Counting both would latch on the scaffolding close and drop the real Nth fill.
void test_pooc_close_coqueued_with_reversal_counts_once(bool starts_long) {
    std::printf("test_pooc_close_coqueued_with_reversal_counts_once(%s)\n",
                starts_long ? "long-to-short" : "short-to-long");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : starts_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 2;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        bool starts_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("FIRST", starts_long);
            } else if (bar_index_ == 1) {
                strategy_entry("REVERSE", !starts_long);
                strategy_close("FIRST");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat(starts_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 2);
    CHECK(std::fabs(strat.get_signed_position_size()) < 1e-9);
    if (strat.trade_count() == 2) {
        CHECK(strat.get_trade(1).exit_comment ==
              "Close Position (Max number of filled orders in one day)");
    }
}

// A queued opposite MARKET is not proof that the strategy.close disappeared
// into a successful reversal. The entry can survive placement and still fail
// the fill-time direction gate after the close leaves the engine flat. The
// real close must retain its quota slot and latch cap=2, blocking LATE.
void test_pooc_close_count_survives_rejected_reversal(bool starts_long) {
    std::printf("test_pooc_close_count_survives_rejected_reversal(%s)\n",
                starts_long ? "long-held" : "short-held");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : starts_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 2;
            risk_direction_ = starts_long
                ? RiskDirection::LONG_ONLY : RiskDirection::SHORT_ONLY;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        bool starts_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("FIRST", starts_long);
            } else if (bar_index_ == 1) {
                strategy_entry("REJECTED", !starts_long);
                strategy_close("FIRST");
            } else if (bar_index_ == 2) {
                strategy_entry("LATE", starts_long);
            }
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat(starts_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
        {102, 104, 100, 103, 50, kT0_UTC + 2 * k15m_ms},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(strat.fill_count() == 2);
    CHECK(strat.cap_hit());
}

// The same close quota survives when an earlier RAW market fill OCA-cancels
// the designated opposite MARKET after flush selected its exact incarnation.
// This is the cancellation shape a mere pending-order proxy gets wrong.
void test_pooc_close_count_survives_cancelled_reversal(bool starts_long) {
    std::printf("test_pooc_close_count_survives_cancelled_reversal(%s)\n",
                starts_long ? "long-held" : "short-held");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : starts_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 4;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        bool starts_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("FIRST", starts_long);
            } else if (bar_index_ == 1) {
                strategy_order("CANCELER", !starts_long, 1.0,
                               std::nan(""), std::nan(""), "PAIR", 1);
                strategy_entry("CANCELLED", !starts_long,
                               std::nan(""), std::nan(""), std::nan(""),
                               "", "PAIR", 1);
                strategy_close("FIRST");
            }
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat(starts_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()
                    - (starts_long ? -1.0 : 1.0)) < 1e-9);
    CHECK(strat.fill_count() == 3);
    CHECK(!strat.cap_hit());
}

// The designated opposite MARKET can also become a same-direction no-op: an
// earlier RAW market order opens that side first. C counts the close at once;
// A later declines the redundant MARKET without rolling the close count back.
void test_pooc_close_count_survives_noop_reversal(bool starts_long) {
    std::printf("test_pooc_close_count_survives_noop_reversal(%s)\n",
                starts_long ? "long-to-short" : "short-to-long");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : starts_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 4;
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        bool starts_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("FIRST", starts_long);
            } else if (bar_index_ == 1) {
                strategy_order("EARLY", !starts_long, 1.0);
                strategy_entry("REDUNDANT", !starts_long);
                strategy_close("FIRST");
            }
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat(starts_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()
                    - (starts_long ? -1.0 : 1.0)) < 1e-9);
    CHECK(strat.fill_count() == 3);
    CHECK(!strat.cap_hit());
}

// Cap-boundary version of the RAW-before-MARKET sequence. EARLY is a distinct
// fill and reaches cap=3, so it expires the saved inheritance before its cap
// close/latch. INHERITOR must then obey that latch; otherwise it opens and is
// cap-closed a second time on the same day.
void test_intervening_fill_expires_pooc_close_inheritance(bool starts_long) {
    std::printf("test_intervening_fill_expires_pooc_close_inheritance(%s)\n",
                starts_long ? "long-to-short" : "short-to-long");

    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool direction) : starts_long(direction) {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 3;
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        bool starts_long;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("FIRST", starts_long);
            } else if (bar_index_ == 1) {
                strategy_order("EARLY", !starts_long, 1.0);
                strategy_entry("INHERITOR", !starts_long);
                strategy_close("FIRST");
            }
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat(starts_long);
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    int cap_closes = 0;
    bool saw_inheritor = false;
    for (int i = 0; i < strat.trade_count(); ++i) {
        const Trade& trade = strat.get_trade(i);
        if (trade.exit_comment ==
            "Close Position (Max number of filled orders in one day)") {
            ++cap_closes;
        }
        if (trade.entry_id == "INHERITOR") saw_inheritor = true;
    }
    CHECK(strat.trade_count() == 2);
    CHECK(cap_closes == 1);
    CHECK(!saw_inheritor);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(strat.fill_count() == 3);
    CHECK(strat.cap_hit());
}

// C has only an ordinary non-magnified oracle. Enabling its metadata must not
// alter a magnifier run until the lower-TF close/reversal contract is probed.
void test_pooc_close_count_candidate_excludes_magnifier() {
    std::printf("test_pooc_close_count_candidate_excludes_magnifier\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 2;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("FIRST", true);
            if (bar_index_ == 1) strategy_close("FIRST");
        }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat;
    Bar lower[] = {
        {100, 101, 99, 100, 50, kT0_UTC + 0 * 60'000LL},
        {100, 102, 99, 101, 50, kT0_UTC + 1 * 60'000LL},
        {101, 103, 100, 102, 50, kT0_UTC + 2 * 60'000LL},
        {102, 104, 101, 103, 50, kT0_UTC + 3 * 60'000LL},
    };
    strat.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
              /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(strat.last_error().empty());
    CHECK(strat.trade_count() == 1);
    CHECK(strat.fill_count() == 1);
    CHECK(!strat.cap_hit());
}

void test_pooc_deferred_cap_candidate_excludes_magnifier() {
    std::printf("test_pooc_deferred_cap_candidate_excludes_magnifier\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 1;
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("FIRST", true);
        }
        double position_size() const { return signed_position_size(); }
        bool deferred_close_pending() const {
            return intraday_cap_deferred_close_pending_;
        }
    };

    Strat strat;
    Bar lower[] = {
        {100, 101, 99, 100, 50, kT0_UTC + 0 * 60'000LL},
        {100, 102, 99, 101, 50, kT0_UTC + 1 * 60'000LL},
    };
    strat.run(lower, 2, "1", "2", /*bar_magnifier=*/true,
              /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(strat.last_error().empty());
    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(!strat.deferred_close_pending());
}

void test_pooc_deferred_cap_candidate_excludes_coof() {
    std::printf("test_pooc_deferred_cap_candidate_excludes_coof\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            calc_on_order_fills_ = true;
            max_intraday_filled_orders_ = 1;
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0 && std::fabs(signed_position_size()) < 1e-9) {
                strategy_entry("FIRST", true);
            }
        }
        double position_size() const { return signed_position_size(); }
        bool deferred_close_pending() const {
            return intraday_cap_deferred_close_pending_;
        }
    };

    Strat strat;
    Bar bars[] = {{100, 102, 98, 101, 50, kT0_UTC}};
    strat.run(bars, 1);

    CHECK(strat.last_error().empty());
    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(!strat.deferred_close_pending());
}

void test_pooc_close_count_candidate_excludes_coof() {
    std::printf("test_pooc_close_count_candidate_excludes_coof\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            calc_on_order_fills_ = true;
            max_intraday_filled_orders_ = 3;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0 && std::fabs(signed_position_size()) < 1e-9) {
                strategy_entry("FIRST", true);
            } else if (bar_index_ == 1
                       && std::fabs(signed_position_size()) > 1e-9) {
                strategy_close("FIRST");
            }
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat;
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.last_error().empty());
    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(strat.fill_count() == 1);
    CHECK(!strat.cap_hit());
}

void test_pooc_close_count_candidate_excludes_any_mode() {
    std::printf("test_pooc_close_count_candidate_excludes_any_mode\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            close_entries_rule_any_ = true;
            max_intraday_filled_orders_ = 3;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("FIRST", true);
            if (bar_index_ == 1) strategy_close("FIRST");
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat;
    Bar bars[] = {
        {100, 102, 98, 101, 50, kT0_UTC + 0 * k15m_ms},
        {101, 103, 99, 102, 50, kT0_UTC + 1 * k15m_ms},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(strat.fill_count() == 1);
    CHECK(!strat.cap_hit());
}

void test_pooc_close_count_candidate_excludes_stream_realtime() {
    std::printf("test_pooc_close_count_candidate_excludes_stream_realtime\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 3;
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("FIRST", true);
            if (bar_index_ == 1) strategy_close("FIRST");
        }
        double position_size() const { return signed_position_size(); }
        int fill_count() const { return intraday_fill_count_; }
        bool cap_hit() const { return intraday_cap_hit_; }
    };

    Strat strat;
    Bar warmup[] = {{100, 102, 98, 101, 50, kT0_UTC}};
    CHECK(strat.stream_begin(warmup, 1, "1", "1"));
    CHECK(std::fabs(strat.position_size() - 1.0) < 1e-9);
    TradeTick tick{kT0_UTC + 60'001LL, 1, 102.0, 1.0};
    CHECK(strat.stream_push_tick(tick));
    CHECK(strat.stream_advance_time(kT0_UTC + 120'000LL));

    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(strat.fill_count() == 1);
    CHECK(!strat.cap_hit());
    CHECK(strat.stream_end(false));
}

// B's next-ordinary-bar-open model cannot cross stream_begin's historical to
// realtime boundary: realtime ticks do not route through dispatch_bar(). The
// warmup therefore stays on established immediate-cap-close semantics.
void test_pooc_deferred_cap_candidate_excludes_stream_warmup() {
    std::printf("test_pooc_deferred_cap_candidate_excludes_stream_warmup\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 0;
            process_orders_on_close_ = true;
            max_intraday_filled_orders_ = 1;
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("FIRST", true);
        }
        double position_size() const { return signed_position_size(); }
        bool deferred_close_pending() const {
            return intraday_cap_deferred_close_pending_;
        }
    };

    Strat strat;
    Bar warmup[] = {{100, 102, 98, 101, 50, kT0_UTC}};
    CHECK(strat.stream_begin(warmup, 1, "1", "1"));
    CHECK(strat.trade_count() == 1);
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(!strat.deferred_close_pending());

    TradeTick next_tick{kT0_UTC + 60'001LL, 1, 102.0, 1.0};
    CHECK(strat.stream_push_tick(next_tick));
    CHECK(std::fabs(strat.position_size()) < 1e-9);
    CHECK(strat.trade_count() == 1);
    CHECK(strat.stream_end(false));
}

}  // namespace

int main() {
    test_cap_latches_until_day_rollover();
    test_cap_disabled_does_not_inject_auto_close();
    test_noop_market_attempt_does_not_consume_cap(true);
    test_noop_market_attempt_does_not_consume_cap(false);
    test_pooc_cap_close_defers_to_next_open(true);
    test_pooc_cap_close_defers_to_next_open(false);
    test_noop_filter_preserves_same_tick_close_then_reentry(true);
    test_noop_filter_preserves_same_tick_close_then_reentry(false);
    test_noop_filter_preserves_same_tick_reversal(true);
    test_noop_filter_preserves_same_tick_reversal(false);
    test_pooc_strategy_close_consumes_cap(true);
    test_pooc_strategy_close_consumes_cap(false);
    test_pooc_close_coqueued_with_reversal_counts_once(true);
    test_pooc_close_coqueued_with_reversal_counts_once(false);
    test_pooc_close_count_survives_rejected_reversal(true);
    test_pooc_close_count_survives_rejected_reversal(false);
    test_pooc_close_count_survives_cancelled_reversal(true);
    test_pooc_close_count_survives_cancelled_reversal(false);
    test_pooc_close_count_survives_noop_reversal(true);
    test_pooc_close_count_survives_noop_reversal(false);
    test_intervening_fill_expires_pooc_close_inheritance(true);
    test_intervening_fill_expires_pooc_close_inheritance(false);
    test_pooc_close_count_candidate_excludes_magnifier();
    test_pooc_deferred_cap_candidate_excludes_magnifier();
    test_pooc_deferred_cap_candidate_excludes_coof();
    test_pooc_close_count_candidate_excludes_coof();
    test_pooc_close_count_candidate_excludes_any_mode();
    test_pooc_close_count_candidate_excludes_stream_realtime();
    test_pooc_deferred_cap_candidate_excludes_stream_warmup();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
