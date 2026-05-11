/*
 * test_same_id_stop_replace.cpp — verify TradingView's same-id
 * strategy.entry replacement timing relative to the bar's
 * process-pending-orders pass.
 *
 * Pine v6 contract (verified empirically against
 * validation/62-same-id-stop-cross-before-modify):
 *
 *   bar B-1 places stop A.
 *   bar B begins:
 *     1. broker evaluates pending orders against bar B's OHLC. If A's
 *        stop is touched, A fires here at A's stop price. The fill is
 *        recorded BEFORE strategy logic runs, so ``strategy.position_size``
 *        on bar B already reflects A's fill.
 *     2. on_bar (strategy logic) executes. Any ``strategy.entry`` call
 *        with the SAME id replaces what's left in pending_orders_:
 *        - if A fired in step 1, pending_orders_ no longer has A; the
 *          new placement just adds A' for bar B+1 onward.
 *        - if A did NOT fire in step 1, A is removed and A' takes its
 *          place; A' is evaluated on bar B+1 (engine ran step 1 already).
 *     3. There is no second pass over bar B for A' — TV's reference docs
 *        describe pending-order updates as bar-boundary events, not
 *        intra-bar. The engine's bar pump enforces this by calling
 *        ``process_pending_orders`` exactly once before ``on_bar`` in
 *        the !process_orders_on_close path.
 *
 * The three scenarios below exercise each branch of the contract.
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

namespace {

// Common probe shell: pyramiding=1, fixed qty=1, no commission/slippage,
// process_orders_on_close=false (the path probe 62 exercises).
class StopReplaceProbe : public BacktestEngine {
public:
    struct TradeRow {
        std::string entry_id;
        double entry_price;
        double exit_price;
        double qty;
        int64_t entry_time;
        int64_t exit_time;
    };
    std::vector<TradeRow> closed_trades;
    int last_position_qty_seen = 0;

    StopReplaceProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 1;
        // The deferred-flip carry rule is gated on the script having a
        // ``strategy.close`` call — set it true so the carry-leak
        // regression (default position_qty_ before any fill) would
        // surface as qty=2 rather than being silently masked.
        script_has_strategy_close_ = true;
    }

    void snapshot() {
        closed_trades.clear();
        for (const auto& t : trades_) {
            closed_trades.push_back({t.entry_id, t.entry_price,
                                     t.exit_price, t.qty,
                                     t.entry_time, t.exit_time});
        }
        last_position_qty_seen = (int)position_qty_;
    }
};

// Build a minimal OHLCV array with deterministic OHLC. Each bar:
//   open = base + i*5, high = open + h_off, low = open - l_off,
//   close = open + c_off. Timestamps are 1-minute spaced for clarity.
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

}  // namespace

// Scenario 1: bar B's process_pending_orders fills the prev bar's stop A;
// strategy.entry on bar B with same id has no effect on the already-
// filled A. The new A' lives for bar B+1 onward. Since the position is
// open after step 1, the modify-branch precondition (position_size==0)
// fails and longModify is never even called — same effective outcome
// regardless of whether modify guard exists, but this scenario verifies
// the engine respects the bar-boundary order.
static void test_filled_stop_unaffected_by_same_id_replace() {
    std::printf("test_filled_stop_unaffected_by_same_id_replace\n");
    class Probe : public StopReplaceProbe {
    public:
        // Bar 0: place stop A at 100.5 (will fire on bar 1 OHLC).
        // Bar 1: position is open after step 1 — the same-id replacement
        //        block in on_bar is therefore predicated on
        //        position_size==0 and SHOULD NOT replace.
        // Bar 2: idle.
        // Bar 3: full close.
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("LE", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/100.5, 1.0, "first stop");
            }
            if (bar_index_ == 1 && position_size() == 0) {
                // Defensive: if the engine bug REVERSED the bar-boundary
                // order (replaced before evaluating prev bar's pending
                // stop), this would fire and the test would observe a
                // second pyramid entry.
                strategy_entry("LE", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/100.5 + 5.0, 1.0, "modified stop");
            }
            if (bar_index_ == 3) {
                strategy_close("LE", "close all");
            }
            if (bar_index_ == 4) snapshot();
        }
    private:
        double position_size() const { return signed_position_size(); }
    };

    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},  // bar 0: range stays below 100.5
        {100.0, 101.0, 99.0, 100.5},  // bar 1: high 101 >= stop 100.5 → fires
        {100.5, 101.5, 100.0, 101.0}, // bar 2
        {101.0, 102.0, 100.5, 101.5}, // bar 3: close call
        {101.5, 102.5, 101.0, 102.0}, // bar 4: close fills
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    // Exactly one closed trade — single pyramid entry from the first stop.
    CHECK(p.closed_trades.size() == 1);
    if (p.closed_trades.size() == 1) {
        const auto& tr = p.closed_trades[0];
        CHECK(near(tr.qty, 1.0));
        CHECK(near(tr.entry_price, 100.5));
        // Bar 1 fired the entry (no slippage), bar 4 open is the close fill.
        CHECK(near(tr.exit_price, 101.5));
    }
}

// Scenario 2: bar B's pending stop A does NOT fire on bar B's OHLC.
// strategy.entry on bar B replaces A with A' at a different price.
// On bar B+1 the new A' is evaluated against bar B+1's OHLC, NOT the
// old A.
static void test_unfilled_stop_replaced_for_next_bar() {
    std::printf("test_unfilled_stop_replaced_for_next_bar\n");
    class Probe : public StopReplaceProbe {
    public:
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("LE", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/100.5, 1.0, "first stop");
            }
            // Bar 1 OHLC won't reach 100.5; position stays FLAT.
            if (bar_index_ == 1 && signed_position_size() == 0) {
                // Replace with a much higher stop that bar 2 WILL touch.
                strategy_entry("LE", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/110.0, 1.0, "raised stop");
            }
            if (bar_index_ == 4) {
                strategy_close("LE", "close all");
            }
            if (bar_index_ == 5) snapshot();
        }
    };

    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},   // bar 0: place A=100.5
        {100.0, 100.4, 99.8, 100.2},   // bar 1: high stays below 100.5; replace
        {100.0, 110.5, 99.8, 110.2},   // bar 2: high reaches 110.5 → A'=110.0 fires
        {110.0, 111.0, 109.5, 110.5},  // bar 3
        {110.5, 111.5, 110.0, 111.0},  // bar 4: close call
        {111.0, 112.0, 110.5, 111.5},  // bar 5: close fills
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    CHECK(p.closed_trades.size() == 1);
    if (p.closed_trades.size() == 1) {
        const auto& tr = p.closed_trades[0];
        CHECK(near(tr.qty, 1.0));
        // Critical: entry price should be the REPLACED stop's price
        // (110.0) — gap-fill: bar 2's open is 100.0, low 99.8, high
        // 110.5. Long stop at 110.0 fills at 110.0 (high-touch path).
        CHECK(near(tr.entry_price, 110.0));
        CHECK(near(tr.exit_price, 111.0));
    }
}

// Scenario 3: bar B's pending stop A fires on bar B's OHLC. on_bar then
// places a NEW (different-id) entry. The new entry is for bar B+1 and
// MUST NOT fill on bar B alongside A. This guards against a regression
// where a same-bar additional process_pending_orders pass would let new
// orders placed in on_bar fire on the same bar.
static void test_new_entry_after_same_bar_fill_defers_to_next_bar() {
    std::printf("test_new_entry_after_same_bar_fill_defers_to_next_bar\n");
    class Probe : public StopReplaceProbe {
    public:
        Probe() {
            // Allow 2 entries in the same direction so the "new entry
            // after stop fill" branch isn't blocked by pyramiding.
            pyramiding_ = 2;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("LE1", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/100.5, 1.0, "first stop");
            }
            // Bar 1: A fires in step 1; we now place a NEW entry with a
            // DIFFERENT id. If the engine erroneously processes this
            // new placement on bar 1, the test will observe two trades
            // dated to bar 1. Correct behavior: the second entry fires
            // on bar 2 at bar 2's open.
            if (bar_index_ == 1 && signed_position_size() > 0) {
                strategy_entry("LE2", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/95.0, 1.0, "second stop");
            }
            if (bar_index_ == 4) {
                strategy_close("", "close all");
            }
            if (bar_index_ == 5) snapshot();
        }
    };

    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},   // bar 0: place LE1 stop=100.5
        {100.0, 101.0, 99.0, 100.5},   // bar 1: A fires at 100.5; place LE2
        {100.5, 101.5, 95.0, 99.0},    // bar 2: low 95 ≤ stop 95 → LE2 fires
        {99.0, 100.0, 98.0, 99.5},     // bar 3
        {99.5, 100.5, 98.5, 100.0},    // bar 4: close
        {100.0, 101.0, 99.5, 100.5},   // bar 5: close fills
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    CHECK(p.closed_trades.size() == 2);
    if (p.closed_trades.size() == 2) {
        const auto& a = p.closed_trades[0];
        const auto& b = p.closed_trades[1];
        std::printf("  trade[0] id=%s entry=%lld price=%.4f exit=%.4f\n",
            a.entry_id.c_str(), (long long)a.entry_time, a.entry_price, a.exit_price);
        std::printf("  trade[1] id=%s entry=%lld price=%.4f exit=%.4f\n",
            b.entry_id.c_str(), (long long)b.entry_time, b.entry_price, b.exit_price);
        CHECK(a.entry_id == "LE1");
        CHECK(b.entry_id == "LE2");
        CHECK(near(a.qty, 1.0));
        CHECK(near(b.qty, 1.0));
        CHECK(near(a.entry_price, 100.5));
        // LE2 must fire on bar 2 (not bar 1): its entry timestamp
        // matches bar 2's timestamp; if the bug leaked back in, LE2's
        // entry timestamp would equal bar 1's.
        CHECK(b.entry_time != bars[1].timestamp);
    }
}

// Scenario 4 (regression for the position_qty_ default leak): a priced
// strategy.entry placed BEFORE the first fill of any session must
// capture tv_carry_qty=0, not the engine's default
// ``position_qty_=1.0`` value. Pre-fix, the LE order's tv_carry_qty
// was 1, which combined with ``script_has_strategy_close_=true`` and
// the order firing from FLAT in the LONG direction produced
// tv_deferred_flip = (true && true && carry=1>0 && (false?:true=true))
// → qty = 1 + 1 = 2 instead of 1. Probe 62's first in-window trade
// fired qty=2 with double the expected PnL (-18.22 vs TV's -9.11),
// breaking parity even before the warmup-gate buffer fix exposed the
// preceding-bar placement.
static void test_carry_capture_on_flat_session_start() {
    std::printf("test_carry_capture_on_flat_session_start\n");
    class Probe : public StopReplaceProbe {
    public:
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                // Session has just started; no fills have happened.
                // position_side_ == FLAT, position_qty_ == default.
                // The placed stop's tv_carry_qty must be captured as 0.
                strategy_entry("LE", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/100.5, 1.0, "first ever stop");
            }
            if (bar_index_ == 3) {
                strategy_close("LE", "close all");
            }
            if (bar_index_ == 4) snapshot();
        }
    };
    auto bars = make_bars({
        {100.0, 100.4, 99.8, 100.2},   // bar 0: place LE
        {100.0, 101.0, 99.0, 100.5},   // bar 1: high 101 >= stop 100.5 → fires
        {100.5, 101.5, 100.0, 101.0},  // bar 2
        {101.0, 102.0, 100.5, 101.5},  // bar 3: close
        {101.5, 102.5, 101.0, 102.0},  // bar 4: close fills
    });
    Probe p;
    p.run(bars.data(), (int)bars.size());

    CHECK(p.closed_trades.size() == 1);
    if (p.closed_trades.size() == 1) {
        const auto& tr = p.closed_trades[0];
        // qty MUST be 1 — pre-fix bug fired qty=2 from default-leaked
        // carry; this regression test pins the contract.
        CHECK(near(tr.qty, 1.0));
        CHECK(near(tr.entry_price, 100.5));
    }
}

int main() {
    test_filled_stop_unaffected_by_same_id_replace();
    test_unfilled_stop_replaced_for_next_bar();
    test_new_entry_after_same_bar_fill_defers_to_next_bar();
    test_carry_capture_on_flat_session_start();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
