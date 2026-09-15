/*
 * test_famae_declined_reversal_trail_gap.cpp — round 10 family AE
 * (stevenygabbyperez-fast-scalper-with-stops on NASDAQ:AAPL@15): what a
 * declined reversal does to the position's TRAIL leg when the decline bar
 * itself reaches the trail's activation.
 *
 * Round 9 family X pinned that the kill is leg-scoped: the stop and limit
 * legs die, the trail leg lives from the bar after the decline. Its engine
 * armed that revived leg from the position's running extreme INCLUDING the
 * decline bar, so a decline on a bar whose range crossed the activation fired
 * the trail at the next open. TradingView does not:
 *
 *   NASDAQ:AAPL 15m, long signal 2025-10-30 15:00Z (fill 15:15Z @270.90),
 *   short signal 19:45Z = an all-in reversal declined at the 10-31 13:30Z
 *   earnings-gap open 276.90 (bar H 277.32 L 269.15 C 270.68; the reversal
 *   costs 2 % more than the account holds). `lab tv` tapes (campaign note
 *   log-20260905t224809z, window 10-27..12-06):
 *
 *     famae-dr-ctrl        no reversal, the probe's exit (activation
 *                          270.90 + 2 % = 276.32): 'Exit Long' 13:30Z @276.90
 *                          — the open gaps past the activation, fill at the open
 *     famae-dr-probe       with the declined reversal: NO exit at 13:30Z or
 *                          13:45Z, none on 11-13 14:30Z when 276.32 is crossed
 *                          again (high 276.69) — the leg is DEAD; the long rides
 *                          to the range end
 *     famae-dr2-tp610-ctrl activation 277.00 (open 276.90 below it, high past
 *                          it): 'Exit Long' 13:30Z @277.00, the one-shot fill
 *     famae-dr2-tp610      with the reversal: no fill at 13:45Z (a 277.32 best
 *                          would have gap-filled the 270.72 open) — the leg
 *                          resumes UNARMED; TradingView arms it at the 11-24
 *                          20:45Z touch (high 277.00) and fills 11-25 14:30Z
 *                          @280.38 riding that bar's extreme
 *     famae-dr2-tp700      activation 277.90, never reached on the decline
 *                          bar: the leg lives and fills one-shot at the level
 *                          when 11-25 14:30Z crosses it, @277.90 (family X)
 *     famae-dr-stop2695    stop 269.50 (crossed by the decline bar's low and
 *                          14:00Z): dead after the decline (family X); ctrl
 *                          fills 13:30Z @269.50
 *
 * Rules (engine.hpp PendingOrder::dormant_trail_best /
 * dormant_trail_leg_dead): the surviving trail leg's running extreme skips the
 * decline bar (seeded with the position's best before it); a trail leg whose
 * activation the decline bar's OPEN already sits past dies with the stop and
 * limit legs.
 *
 * Documented residual, outside this family: TradingView's touch at 11-24
 * 20:45Z (high == activation 277.00) ARMS the offset-less trail without
 * filling it and the next bar fills at its extreme (280.38); the engine's
 * activation test fills the touch one-shot at the level (11-24 20:45Z
 * @277.00). The tp610 case therefore asserts only what this family owns: no
 * fill on the revival bar, a fill no earlier than the 11-24 20:45Z bar.
 *
 * Registry feed bars via `lab bars` (the bars between 10-31 18:00Z and the
 * three later windows never reach 276.32 / 277.00 / 277.90 — first touches
 * 11-13 14:30Z / 11-24 20:45Z / 11-25 14:30Z — so the reduced table keeps
 * every activation event of the tapes).
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
using pineforge::source::PendingOrder;

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

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

static const std::vector<Bar> kBars = {
    mk(1761836400000LL, 268.92, 270.93, 268.89, 270.92),   // [0] 2025-10-30 15:00Z
    mk(1761837300000LL, 270.9, 271.16, 270.55, 270.79),   // [1] 2025-10-30 15:15Z
    mk(1761838200000LL, 270.78, 270.92, 270.29, 270.76),   // [2] 2025-10-30 15:30Z
    mk(1761839100000LL, 270.74, 271.73, 270.6, 271.16),   // [3] 2025-10-30 15:45Z
    mk(1761840000000LL, 271.18, 271.97, 271.18, 271.96),   // [4] 2025-10-30 16:00Z
    mk(1761840900000LL, 271.95, 272.39, 271.82, 272.04),   // [5] 2025-10-30 16:15Z
    mk(1761841800000LL, 272.04, 272.05, 271.44, 271.81),   // [6] 2025-10-30 16:30Z
    mk(1761842700000LL, 271.81, 271.92, 271.26, 271.31),   // [7] 2025-10-30 16:45Z
    mk(1761843600000LL, 271.305, 271.44, 271.05, 271.4),   // [8] 2025-10-30 17:00Z
    mk(1761844500000LL, 271.38, 271.99, 271.19, 271.96),   // [9] 2025-10-30 17:15Z
    mk(1761845400000LL, 271.96, 272, 271.29, 271.4),   // [10] 2025-10-30 17:30Z
    mk(1761846300000LL, 271.41, 272.06, 271.41, 271.99),   // [11] 2025-10-30 17:45Z
    mk(1761847200000LL, 272.01, 272.3, 271.68, 272.17),   // [12] 2025-10-30 18:00Z
    mk(1761848100000LL, 272.16, 272.19, 271.48, 271.75),   // [13] 2025-10-30 18:15Z
    mk(1761849000000LL, 271.77, 272, 271.29, 271.74),   // [14] 2025-10-30 18:30Z
    mk(1761849900000LL, 271.76, 271.785, 271.19, 271.35),   // [15] 2025-10-30 18:45Z
    mk(1761850800000LL, 271.34, 271.63, 271.21, 271.36),   // [16] 2025-10-30 19:00Z
    mk(1761851700000LL, 271.35, 271.6, 270.99, 271.35),   // [17] 2025-10-30 19:15Z
    mk(1761852600000LL, 271.34, 271.47, 271.05, 271.36),   // [18] 2025-10-30 19:30Z
    mk(1761853500000LL, 271.39, 271.92, 271.05, 271.23),   // [19] 2025-10-30 19:45Z
    mk(1761917400000LL, 276.9, 277.32, 269.15, 270.68),   // [20] 2025-10-31 13:30Z
    mk(1761918300000LL, 270.72, 272.06, 269.77, 270.4),   // [21] 2025-10-31 13:45Z
    mk(1761919200000LL, 270.47, 270.71, 269.3, 269.4),   // [22] 2025-10-31 14:00Z
    mk(1761920100000LL, 269.41, 270.64, 269.16, 270.4),   // [23] 2025-10-31 14:15Z
    mk(1761921000000LL, 270.405, 271.54, 270.12, 270.69),   // [24] 2025-10-31 14:30Z
    mk(1761921900000LL, 270.73, 271, 270.29, 270.615),   // [25] 2025-10-31 14:45Z
    mk(1761922800000LL, 270.63, 271.225, 270.39, 270.92),   // [26] 2025-10-31 15:00Z
    mk(1761923700000LL, 270.9, 271.52, 270.74, 271.31),   // [27] 2025-10-31 15:15Z
    mk(1761924600000LL, 271.3, 271.36, 270.23, 271.03),   // [28] 2025-10-31 15:30Z
    mk(1761925500000LL, 271.02, 271.54, 270.58, 271.45),   // [29] 2025-10-31 15:45Z
    mk(1761926400000LL, 271.44, 271.76, 270.74, 271.54),   // [30] 2025-10-31 16:00Z
    mk(1761927300000LL, 271.52, 272.85, 271.42, 272.44),   // [31] 2025-10-31 16:15Z
    mk(1761928200000LL, 272.45, 273.16, 272.18, 272.63),   // [32] 2025-10-31 16:30Z
    mk(1761929100000LL, 272.61, 272.79, 271.48, 271.64),   // [33] 2025-10-31 16:45Z
    mk(1761930000000LL, 271.64, 271.78, 270.95, 271.16),   // [34] 2025-10-31 17:00Z
    mk(1761930900000LL, 271.14, 271.37, 270.16, 270.33),   // [35] 2025-10-31 17:15Z
    mk(1761931800000LL, 270.3, 271.06, 270.11, 271.06),   // [36] 2025-10-31 17:30Z
    mk(1761932700000LL, 271.03, 271.61, 270.99, 271.4),   // [37] 2025-10-31 17:45Z
    mk(1761933600000LL, 271.37, 272.04, 271.37, 271.4),   // [38] 2025-10-31 18:00Z
    mk(1761934500000LL, 271.46, 271.85, 271.24, 271.83),   // [39] 2025-10-31 18:15Z
    mk(1761935400000LL, 271.79, 272.12, 271.66, 271.86),   // [40] 2025-10-31 18:30Z
    mk(1762980300000LL, 273.9, 274.39, 272.97, 273.36),   // [41] 2025-11-12 20:45Z
    mk(1763044200000LL, 274.11, 276.69, 273.57, 276.34),   // [42] 2025-11-13 14:30Z
    mk(1763045100000LL, 276.2, 276.27, 274.33, 275.11),   // [43] 2025-11-13 14:45Z
    mk(1763046000000LL, 275.13, 275.15, 274.04, 274.17),   // [44] 2025-11-13 15:00Z
    mk(1764016200000LL, 276.47, 276.98, 276.44, 276.79),   // [45] 2025-11-24 20:30Z
    mk(1764017100000LL, 276.79, 277, 275.1, 275.97),   // [46] 2025-11-24 20:45Z
    mk(1764081000000LL, 275.38, 280.38, 275.25, 279.92),   // [47] 2025-11-25 14:30Z
    mk(1764081900000LL, 279.91, 279.91, 277.92, 279.41),   // [48] 2025-11-25 14:45Z
    mk(1764082800000LL, 279.35, 279.41, 277.66, 278.85),   // [49] 2025-11-25 15:00Z
    mk(1764083700000LL, 278.84, 279.25, 278.2, 278.9),   // [50] 2025-11-25 15:15Z
    mk(1764084600000LL, 278.89, 279.63, 278.73, 279.57),   // [51] 2025-11-25 15:30Z
};

const int kLongSig = 0;      // 10-30 15:00Z close 270.92 -> fill [1] @270.90
const int kShortSig = 19;    // 10-30 19:45Z close 271.23 -> declined at [20] open 276.90
const int kDeclineBar = 20;  // 10-31 13:30Z  O 276.90 H 277.32 L 269.15 C 270.68
const int kRevivalBar = 21;  // 10-31 13:45Z  O 270.72
const int kRecross32 = 42;   // 11-13 14:30Z  H 276.69 (>= 276.32)
const int kTouch700 = 46;    // 11-24 20:45Z  H 277.00 (== 277.00)
const int kCross790 = 47;    // 11-25 14:30Z  O 275.38 H 280.38 (>= 277.90)

struct Signal {
    int bar;
    bool is_long;
    double stop_mult;      // stop = close * mult (NaN: none)
    double stop_abs;       // stop = absolute (NaN: none)
    double trail_ticks;    // trail_points ticks (NaN: none); <0 = close * 0.02 / mintick
};

class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = 1000000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = false;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        syminfo_.pointvalue = 1.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        margin_call_enabled_ = true;
    }
    std::vector<Signal> signals;
    void on_source_bar(const Bar& bar) override {
        for (const Signal& s : signals) {
            if (s.bar != bar_index_) continue;
            const std::string id = s.is_long ? "Long" : "Short";
            strategy_entry(id, s.is_long);
            double stop = kNaN;
            if (!std::isnan(s.stop_abs)) stop = s.stop_abs;
            else if (!std::isnan(s.stop_mult)) stop = bar.close * s.stop_mult;
            double tp = kNaN;
            if (!std::isnan(s.trail_ticks)) {
                tp = s.trail_ticks < 0.0 ? bar.close * 0.02 / syminfo_mintick_ : s.trail_ticks;
            }
            if (std::isnan(stop) && std::isnan(tp)) continue;
            strategy_exit("Exit " + id, id, kNaN, stop, tp, kNaN, kNaN, 100.0, "");
        }
    }
    int trades() const { return trade_count(); }
    double x_price(int i) const { return closed_trade_exit_price(i); }
    int x_bar(int i) const { return closed_trade_exit_bar_index(i); }
    int e_bar(int i) const { return closed_trade_entry_bar_index(i); }
    double e_price(int i) const { return closed_trade_entry_price(i); }
    std::string x_comment(int i) const { return closed_trade_exit_comment(i); }
    double position() const { return signed_position_size(); }
};

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// The probe's reversal: the short signal with its own exit, declined at the gap.
Signal probe_short() { return Signal{kShortSig, false, 1.01, kNaN, -1.0}; }

void run_case(const char* name, std::vector<Signal> sigs, Probe& p) {
    p.signals = std::move(sigs);
    p.run(kBars.data(), (int)kBars.size());
    std::printf("   %-22s trades %d", name, p.trades());
    for (int i = 0; i < p.trades(); ++i) {
        std::printf("  [%d] exit bar %d @%.2f (%s)", i, p.x_bar(i), p.x_price(i), p.x_comment(i).c_str());
    }
    std::printf("  position %.0f\n", p.position());
}

void test_controls() {
    std::printf("-- controls: no reversal, the decline bar's open / path fills the trail --\n");
    {   // famae-dr-ctrl: activation 276.32 gapped at the 276.90 open
        Probe p; run_case("dr-ctrl", {Signal{kLongSig, true, 0.99, kNaN, -1.0}}, p);
        CHECK(p.trades() == 1);
        if (p.trades() == 1) {
            CHECK(p.e_bar(0) == 1 && near(p.e_price(0), 270.90));
            CHECK(p.x_bar(0) == kDeclineBar);
            CHECK(near(p.x_price(0), 276.90));
        }
    }
    {   // famae-dr2-tp610-ctrl: activation 277.00 crossed intrabar
        Probe p; run_case("dr2-tp610-ctrl", {Signal{kLongSig, true, 0.99, kNaN, 610.0}}, p);
        CHECK(p.trades() == 1);
        if (p.trades() == 1) {
            CHECK(p.x_bar(0) == kDeclineBar);
            CHECK(near(p.x_price(0), 277.00));
        }
    }
    {   // famae-dr-stop2695-ctrl: the stop fills on the decline bar
        Probe p; run_case("dr-stop2695-ctrl", {Signal{kLongSig, true, kNaN, 269.5, kNaN}}, p);
        CHECK(p.trades() == 1);
        if (p.trades() == 1) {
            CHECK(p.x_bar(0) == kDeclineBar);
            CHECK(near(p.x_price(0), 269.50));
        }
    }
}

void test_declined_reversal() {
    std::printf("-- the declined reversal at the 276.90 open --\n");
    {   // famae-dr-probe: activation 276.32 under the decline bar's open -> the leg dies
        Probe p; run_case("dr-probe", {Signal{kLongSig, true, 0.99, kNaN, -1.0}, probe_short()}, p);
        CHECK(p.trades() == 0);           // no reversal row, no trail fill, no stop fill
        CHECK(p.position() > 0.0);        // the long rides past 11-13 and 11-25
    }
    {   // famae-dr2-tp610: activation 277.00 crossed intrabar on the decline bar -> the
        // leg lives but the decline bar does not arm it: nothing at the 13:45Z open
        Probe p; run_case("dr2-tp610", {Signal{kLongSig, true, 0.99, kNaN, 610.0}, probe_short()}, p);
        CHECK(p.trades() == 1);
        if (p.trades() == 1) {
            CHECK(p.x_bar(0) != kRevivalBar);
            CHECK(p.x_bar(0) >= kTouch700);   // TV: [47] @280.38 after the [46] touch; engine [46] @277.00 (residual, see header)
            CHECK(p.x_bar(0) > kRecross32);
        }
    }
    {   // famae-dr2-tp700: activation 277.90 untouched by the decline bar -> family X, fills when crossed
        Probe p; run_case("dr2-tp700", {Signal{kLongSig, true, 0.99, kNaN, 700.0}, probe_short()}, p);
        CHECK(p.trades() == 1);
        if (p.trades() == 1) {
            CHECK(p.x_bar(0) == kCross790);
            CHECK(near(p.x_price(0), 277.90));
        }
    }
    {   // famae-dr-stop2695: the stop leg dies (family X), the long rides
        Probe p; run_case("dr-stop2695", {Signal{kLongSig, true, kNaN, 269.5, kNaN}, probe_short()}, p);
        CHECK(p.trades() == 0);
        CHECK(p.position() > 0.0);
    }
}

}  // namespace

int main() {
    test_controls();
    test_declined_reversal();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
