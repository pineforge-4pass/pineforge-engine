/*
 * test_margin_call_intrabar_chronology.cpp — finding-308: TV places the
 * forced-liquidation event chronologically on the synthesized intrabar path.
 *
 * When a priced exit of the live position fills on a bar whose adverse
 * extreme comes STRICTLY earlier on the engine's own OHLC path
 * (bar_path_uses_high_first proximity rule) than the exit's fill, and the
 * pre-fill position is already in margin deficit at that extreme, TV slices
 * FIRST (floor-before-4x nibble, filled at the extreme, "Margin call" tag)
 * and the exit then closes the reduced remainder. Previously the engine
 * checked margin once AFTER all order processing, so a same-bar full exit
 * hid the deficit (FLAT early-return) and the event was lost.
 *
 * The fixtures reproduce the rhyme17 derivation's 2025-10-11 20:45 seed
 * arithmetic exactly (short 2.5105, adverse high 3721.62, deficit ~8.15,
 * q_min 0.0021894... -> floor 0.0021 -> 4x = 0.0084):
 *
 *   A. HIGH-first bar, TP limit fills after the high -> slice 0.0084@high,
 *      TP closes the remainder 2.5021. (The confirmed gap event.)
 *   B. LOW-first bar with the SAME large deficit at the high -> the exit
 *      fills before the extreme on the path -> NO margin call. (The two
 *      tape bars 2025-06-28 08:15 / 2025-09-17 18:45 that a naive
 *      check-before-orders would false-fire.)
 *   C. TIE — the exit stop fills exactly AT the adverse extreme -> exit
 *      first, NO margin call. (Protects the 157/158 quiet SL-stop bars.)
 *   D. SL stop strictly BEFORE the extreme -> quiet (same protection).
 *   E. Partial exit variant: exactly ONE margin-call slice on the bar (the
 *      end-of-bar cascade is consumed by the chronological slice; the
 *      survivor is re-checked from the next bar on).
 *   F. Emulator off -> nothing fires.
 *   G. Handle reuse: a rerun reproduces the same rows (bar-keyed one-shot
 *      markers reset with reset_run_state).
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
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) < tol;
}

namespace {

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c, double v) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = v; b.timestamp = ts;
    return b;
}

class MCEngine : public BacktestEngine {
public:
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
};

static int margin_call_rows(const MCEngine& eng) {
    int count = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) == std::string("Margin call")) ++count;
    }
    return count;
}

// A 1x short (the rhyme17 seed's margin regime) opened with explicit qty
// 2.5105 at 3706.26 and carried into the event bar with a resting
// strategy.exit. Chosen so the deficit at the adverse high 3721.62 is
// 8.152... USDT: q_min = 2.5105 - 9334.978672/3721.62 = 0.0021894...,
// floored to 0.0021 at qty_step 0.0001, 4x = the seed's bit-exact 0.0084.
class ChronologyShortProbe : public MCEngine {
public:
    enum class ExitKind { TpLimit, SlStop };

    ChronologyShortProbe(ExitKind kind, double exit_level,
                         double exit_qty_percent = 100.0,
                         bool disable_mc = false)
        : kind_(kind), exit_level_(exit_level),
          exit_qty_percent_(exit_qty_percent) {
        initial_capital_ = 9373.54;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        if (disable_mc) set_margin_call_enabled(false);
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            // Market short queued at the signal bar; fills at bar1 open.
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/2.5105);
        } else if (bar_index_ == 1) {
            // Armed while the position is live; rests for the event bar.
            const double limit =
                kind_ == ExitKind::TpLimit ? exit_level_ : kNaN;
            const double stop =
                kind_ == ExitKind::SlStop ? exit_level_ : kNaN;
            strategy_exit("X", "S", limit, stop, kNaN, kNaN, kNaN,
                          exit_qty_percent_, "", kNaN, "");
        }
    }

private:
    ExitKind kind_;
    double exit_level_;
    double exit_qty_percent_;
};

static std::vector<Bar> seed_bars(const Bar& event_bar) {
    return {
        mk_bar(1000, 3706.26, 3706.26, 3706.26, 3706.26, 1.0),  // 0: signal
        mk_bar(2000, 3706.26, 3706.26, 3706.26, 3706.26, 1.0),  // 1: fill+arm
        event_bar,                                              // 2: event
    };
}

// HIGH-first event bar (|3721.62-3712| = 9.62 < |3712-3660| = 52): the
// path is O -> H -> L -> C, so the adverse high (path position 1.0) comes
// strictly before the TP limit 3664.69 on the H->L leg (position ~1.92).
static Bar high_first_event_bar() {
    return mk_bar(3000, 3712.0, 3721.62, 3660.0, 3665.0, 1.0);
}

// ---- A: the confirmed gap event fires the 0.0084 slice ---------------------

static void test_gap_event_slices_before_tp_exit() {
    std::printf("test_gap_event_slices_before_tp_exit\n");
    std::vector<Bar> bars = seed_bars(high_first_event_bar());

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::TpLimit,
                             /*exit_level=*/3664.69);
    eng.run(bars.data(), (int)bars.size());

    // Slice first (0.0084 @ the adverse high), then the TP closes the
    // remainder 2.5021 at the unslipped limit.
    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.0084, 1e-9));
    CHECK(near(eng.exit_price(0), 3721.62));
    CHECK(near(eng.entry_price(0), 3706.26));
    CHECK(eng.exit_bar(0) == 2);
    CHECK(eng.exit_comment(1) != std::string("Margin call"));
    CHECK(near(eng.trade_size(1), 2.5021, 1e-9));
    CHECK(near(eng.exit_price(1), 3664.69));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- B: a LOW-first bar with the same deficit must NOT fire ----------------

static void test_low_first_large_deficit_stays_quiet() {
    std::printf("test_low_first_large_deficit_stays_quiet\n");
    // LOW-first (|3721.62-3666| = 55.62 > |3666-3660| = 6): path is
    // O -> L -> H -> C. The TP fills on the O->L leg (position ~0.22),
    // BEFORE the adverse high (position 2.0), even though the deficit at
    // the high is the same 8.15. This is the naive check-before-orders
    // false-fire shape (tape bars 2025-06-28 08:15 / 2025-09-17 18:45).
    std::vector<Bar> bars = seed_bars(
        mk_bar(3000, 3666.0, 3721.62, 3660.0, 3700.0, 1.0));

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::TpLimit,
                             /*exit_level=*/3664.69);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 2.5105, 1e-9));
    CHECK(near(eng.exit_price(0), 3664.69));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- C: a fill exactly AT the extreme ties -> exit first -------------------

static void test_exit_at_extreme_ties_to_exit_first() {
    std::printf("test_exit_at_extreme_ties_to_exit_first\n");
    // Exit stop exactly at the adverse high: both first-touch positions are
    // 1.0 on the O->H leg. The tie keeps the exit first — no slice.
    std::vector<Bar> bars = seed_bars(high_first_event_bar());

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::SlStop,
                             /*exit_level=*/3721.62);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 2.5105, 1e-9));
    CHECK(near(eng.exit_price(0), 3721.62));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- D: an SL stop strictly before the extreme stays quiet -----------------

static void test_sl_stop_before_extreme_stays_quiet() {
    std::printf("test_sl_stop_before_extreme_stays_quiet\n");
    // Stop 3715 fills on the O->H leg at position ~0.31, before the high at
    // 1.0 — the 157/158 quiet SL-stop class.
    std::vector<Bar> bars = seed_bars(high_first_event_bar());

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::SlStop,
                             /*exit_level=*/3715.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 2.5105, 1e-9));
    CHECK(near(eng.exit_price(0), 3715.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- E: one forced-liquidation event per bar -------------------------------

static void test_partial_exit_single_slice_per_bar() {
    std::printf("test_partial_exit_single_slice_per_bar\n");
    // A 1% partial TP leaves a live survivor after the slice + exit. The
    // end-of-bar cascade must not book a second same-bar slice (the
    // chronological one consumed the bar's event; TV re-checks the survivor
    // on the NEXT bar — the seed's own 20:30 -> 20:45 sequence).
    std::vector<Bar> bars = seed_bars(high_first_event_bar());
    bars.push_back(mk_bar(4000, 3665.0, 3665.0, 3665.0, 3665.0, 1.0));

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::TpLimit,
                             /*exit_level=*/3664.69,
                             /*exit_qty_percent=*/1.0);
    eng.run(bars.data(), (int)bars.size());

    // On the event bar: exactly one Margin-call slice (0.0084) plus the
    // 1%-frozen partial (0.0251). The survivor stays short.
    int event_bar_mc_rows = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_bar(i) == 2
            && eng.exit_comment(i) == std::string("Margin call")) {
            ++event_bar_mc_rows;
        }
    }
    CHECK(event_bar_mc_rows == 1);
    CHECK(eng.trade_count() >= 2);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.0084, 1e-9));
    CHECK(near(eng.exit_price(0), 3721.62));
    CHECK(near(eng.trade_size(1), 0.0251, 1e-9));
    CHECK(near(eng.exit_price(1), 3664.69));
    CHECK(eng.position_size() < 0.0);  // survivor carried past the event bar
}

// ---- F: emulator off -> nothing fires --------------------------------------

static void test_disabled_emulator_stays_quiet() {
    std::printf("test_disabled_emulator_stays_quiet\n");
    std::vector<Bar> bars = seed_bars(high_first_event_bar());

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::TpLimit,
                             /*exit_level=*/3664.69,
                             /*exit_qty_percent=*/100.0,
                             /*disable_mc=*/true);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 2.5105, 1e-9));
}

// ---- G: handle reuse reproduces the same rows ------------------------------

static void test_rerun_reproduces_slice() {
    std::printf("test_rerun_reproduces_slice\n");
    std::vector<Bar> bars = seed_bars(high_first_event_bar());

    ChronologyShortProbe eng(ChronologyShortProbe::ExitKind::TpLimit,
                             /*exit_level=*/3664.69);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 1);

    // Rerun on the same handle: bar-keyed one-shot markers must reset.
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(near(eng.trade_size(0), 0.0084, 1e-9));
    CHECK(near(eng.exit_price(0), 3721.62));
    CHECK(near(eng.trade_size(1), 2.5021, 1e-9));
}

}  // namespace

int main() {
    std::printf("=== test_margin_call_intrabar_chronology ===\n");

    test_gap_event_slices_before_tp_exit();
    test_low_first_large_deficit_stays_quiet();
    test_exit_at_extreme_ties_to_exit_first();
    test_sl_stop_before_extreme_stays_quiet();
    test_partial_exit_single_slice_per_bar();
    test_disabled_emulator_stays_quiet();
    test_rerun_reproduces_slice();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
