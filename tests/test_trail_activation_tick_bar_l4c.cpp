#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
/*
 * test_trail_activation_tick_bar.cpp — round 7 family K,
 * design-trail-activation-tick-bar: TradingView tests a trailing stop's
 * ACTIVATION (trail_points / trail_price) against the bar's OHLC QUANTIZED
 * to the tick — the round-6 stop / limit trigger rule (test_stop_tick_
 * rounding.cpp) extended to the trail's arming — while the trail's running
 * best (peak / trough) stays the raw print (round 5; stopround-xt-L-trail).
 *
 * Pinned with three `lab tv` tapes on NYSE:F 15m, 2025-04-10..18, fixed 100
 * shares (scratchpad/r7/pins/f15-trail-0415-{reissue,fixed760,fixed754};
 * campaign note log-20260905t084531z-57cedc55): a short entered at the
 * 04-15 14:45Z open (9.495 -> 9.49) with
 *   strategy.exit("XS", "S", trail_points = close * 0.008 / syminfo.mintick,
 *                 trail_offset = 0)            re-issued every bar, or
 *   trail_points = 7.60 / 7.54 fixed           (all ceil to 8 ticks),
 * exits on the 15:45Z bar @9.41 in EVERY tape. Activation = 9.49 - 0.08 =
 * 9.41; the 15:45Z bar (O 9.43 H 9.45 L 9.415 C 9.445) has a raw low of
 * 9.415 — above the level, the engine's no-activate, which slid the exit to
 * the 16:45Z bar (low 9.41) — and a tick-quantized low of 9.41 (9.415 is
 * 9.41499.. in binary: floor(941.499 + 0.5) = 941), which reaches it; with
 * offset 0 the trail exits one-shot at the level. Re-issuing trail_points
 * from the current close is irrelevant (the fixed tapes are identical).
 *
 * The bars are the registry feed's (lab bars NYSE:F 15, feed 80f404ae85ef),
 * UTC-stamped; tape times are UTC+8 evenings of the same day.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/engine_internal.hpp"
#include "exit_path_resolver_oracle.hpp"

using namespace pineforge;
using namespace pineforge::internal;

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

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kQuarter = 15 * 60 * 1000;
// 2025-04-15 14:30:00Z
constexpr int64_t k0415_1430Z = 1744727400000LL;

Bar mk(double o, double h, double l, double c, int64_t ts = 0) {
    Bar b{};
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 100000.0; b.timestamp = ts;
    return b;
}

// NYSE:F 15m, 2025-04-15 14:30Z .. 16:45Z (10 bars), registry feed prints.
std::vector<Bar> f15_0415() {
    std::vector<Bar> bars = {
        mk(9.51, 9.525, 9.46, 9.5),        // 14:30  signal bar
        mk(9.495, 9.5, 9.44, 9.465),       // 14:45  entry @ open 9.495 -> 9.49
        mk(9.465, 9.485, 9.42, 9.425),     // 15:00
        mk(9.425, 9.455, 9.425, 9.435),    // 15:15
        mk(9.435, 9.46, 9.42, 9.425),      // 15:30
        mk(9.43, 9.45, 9.415, 9.445),      // 15:45  low 9.415 -> tick 9.41: TV exits @9.41
        mk(9.445, 9.46, 9.43, 9.445),      // 16:00
        mk(9.445, 9.46, 9.43, 9.435),      // 16:15
        mk(9.44, 9.45, 9.42, 9.435),       // 16:30
        mk(9.435, 9.44, 9.41, 9.415),      // 16:45  low 9.41: the engine's old exit
    };
    for (std::size_t i = 0; i < bars.size(); ++i) {
        bars[i].timestamp = k0415_1430Z + static_cast<int64_t>(i) * kQuarter;
    }
    return bars;
}

// NYSE:F strategy() of the pins: 100 shares fixed, no commission / slippage,
// one position, orders processed at the next bar's open. Bar 0 enters short;
// while short, strategy.exit("X", "E", trail_points = <mode>, trail_offset =
// 0) is re-issued every bar — trail_points either fixed (fixed_points_) or
// close * 0.008 / mintick (the probe's form).
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = 10000.0;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        margin_call_enabled_ = false;
    }
    double fixed_points_ = kNaN;   // NaN -> close * 0.008 / mintick
    double trail_offset_ = 0.0;

    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("E", false);
        if (position_side_ != PositionSide::FLAT) {
            const double points = std::isnan(fixed_points_)
                ? bar.close * 0.008 / syminfo_mintick_
                : fixed_points_;
            strategy_exit("X", "E", kNaN, kNaN, points, trail_offset_);
        }
    }
    using BacktestEngine::position_side_;
};

void expect_short_exit(const Probe& eng, int exit_bar, double exit_px) {
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() != 1) return;
    const Trade& t = eng.get_trade(0);
    CHECK(t.is_long == false);
    CHECK(t.entry_time == k0415_1430Z + 1 * kQuarter);
    CHECK_NEAR(t.entry_price, 9.49, 1e-9);
    CHECK(t.exit_time == k0415_1430Z + exit_bar * kQuarter);
    CHECK_NEAR(t.exit_price, exit_px, 1e-9);
    CHECK_NEAR(t.qty, 100.0, 1e-9);
    if (t.exit_time != k0415_1430Z + exit_bar * kQuarter) {
        std::printf("        got exit bar %lld @%.5f (expected bar %d @%.5f)\n",
                    (long long)((t.exit_time - k0415_1430Z) / kQuarter),
                    t.exit_price, exit_bar, exit_px);
    }
}

// ── engine: the three tapes ───────────────────────────────────────────

void test_reissued_trail_points_exits_on_the_quantized_low() {
    std::printf("-- f15-trail-0415-reissue: trail_points = close*0.008/mintick, offset 0 -> 15:45Z @9.41 --\n");
    Probe eng;
    auto bars = f15_0415();
    eng.run(bars.data(), (int)bars.size());
    // 9.465 * 0.8 = 7.572 -> 8 ticks -> 9.41; 9.425 * 0.8 = 7.54 -> 8 -> 9.41.
    // Bar 5 (15:45Z): raw low 9.415 > 9.41, tick low 9.41 <= 9.41 -> exit.
    expect_short_exit(eng, 5, 9.41);
}

void test_fixed_760_exits_on_the_quantized_low() {
    std::printf("-- f15-trail-0415-fixed760: trail_points 7.60 -> 8 ticks -> 15:45Z @9.41 --\n");
    Probe eng;
    eng.fixed_points_ = 7.60;
    auto bars = f15_0415();
    eng.run(bars.data(), (int)bars.size());
    expect_short_exit(eng, 5, 9.41);
}

void test_fixed_754_exits_on_the_quantized_low() {
    std::printf("-- f15-trail-0415-fixed754: trail_points 7.54 -> 8 ticks -> 15:45Z @9.41 --\n");
    Probe eng;
    eng.fixed_points_ = 7.54;
    auto bars = f15_0415();
    eng.run(bars.data(), (int)bars.size());
    expect_short_exit(eng, 5, 9.41);
}

// Control: an activation one tick FURTHER (9 ticks -> 9.40) is reached by
// neither the quantized 15:45Z low (9.41) nor the 16:45Z low (9.41): the
// quantized compare does not over-fire, the position is still open at the
// end of the window.
void test_activation_below_the_quantized_low_does_not_fire() {
    std::printf("-- control: trail_points 9 -> 9.40 is below every quantized low -> no exit --\n");
    Probe eng;
    eng.fixed_points_ = 9.0;
    auto bars = f15_0415();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK(eng.trade_count() == 0);
}

// ── resolver: where the quantization lives ────────────────────────────

Bar tick_twin(const Bar& b, double mintick) {
    auto q = [&](double p) {
        const double k = std::floor(p / mintick + 0.5);
        return k / std::floor(1.0 / mintick + 0.5);
    };
    Bar t = b;
    t.open = q(b.open); t.high = q(b.high); t.low = q(b.low); t.close = q(b.close);
    return t;
}

void test_resolver_activation_is_reached_on_the_tick_path() {
    std::printf("-- resolver: the 15:45Z bar's tick twin reaches 9.41 on the O->L leg, the raw bar does not --\n");
    const Bar bar = mk(9.43, 9.45, 9.415, 9.445);
    const Bar tick = tick_twin(bar, 0.01);
    CHECK(tick.low == 9.41);
    CHECK(tick.close == 9.45);   // 9.445 -> 944.5000000000001 + 0.5 -> 945
    // Short @9.49, trail_points 8 (activation 9.41), offset 0, carried best
    // = the raw trough so far (9.42), not the entry bar.
    ExitPathFill f = resolve_exit_path_fill(
        bar, tick, PositionSide::SHORT, /*stop=*/kNaN, /*limit=*/kNaN,
        /*trail_points=*/8.0, /*trail_price=*/kNaN, /*trail_offset=*/0.0,
        /*entry=*/9.49, /*best_start=*/9.42, /*is_entry_bar=*/false,
        /*magnifier=*/false, /*mintick=*/0.01);
    CHECK(f.should_fill == true);
    CHECK(f.is_trail == true);
    CHECK(f.is_limit == false);
    CHECK(f.at_bar_open == false);
    CHECK_NEAR(f.fill_price, 9.41, 1e-12);
    // |H-O| 0.02 > |O-L| 0.015 -> low first: O -> L is segment 0 and the
    // tick leg 9.43 -> 9.41 ends exactly on the level: path position 1.0.
    CHECK_NEAR(f.path_position, 1.0, 1e-9);

    // The raw-only form (tick twin == bar) walks the raw 9.415 low: no fill.
    ExitPathFill raw = resolve_exit_path_fill(
        bar, PositionSide::SHORT, kNaN, kNaN, 8.0, kNaN, 0.0,
        9.49, 9.42, false, false, 0.01);
    CHECK(raw.should_fill == false);
}

// A trail WITH an offset arms on the quantized extreme too, then trails the
// RAW best: long @9.90, activation 10.00 (10 ticks), offset 2 ticks; bar
// O 9.98 H 9.996 L 9.97 C 9.975 (|O-L| 0.01 < |H-O| 0.016 -> low first).
// Raw high 9.996 never reaches 10.00 (dormant, no fill — the old engine);
// tick high 10.00 (999.6 + 0.5 -> 1000) arms it at the end of the L->H leg
// with best = 9.996 raw, so the H->C leg crosses 9.996 - 0.02 = 9.976 and
// fills there. (The consequence of the pinned rule for offset trails; the
// round-6 stopround-xt-L-trail tape fixes the best itself as RAW — a
// quantized best 10.00 would have printed 9.98.)
void test_resolver_offset_trail_arms_on_the_quantized_extreme() {
    std::printf("-- resolver: offset trail arms on the tick high 10.00, trails the raw best 9.996 --\n");
    const Bar bar = mk(9.98, 9.996, 9.97, 9.975);
    const Bar tick = tick_twin(bar, 0.01);
    CHECK(tick.high == 10.0);
    ExitPathFill f = resolve_exit_path_fill(
        bar, tick, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/10.0, kNaN, /*trail_offset=*/2.0,
        /*entry=*/9.90, /*best_start=*/9.90, false, false, 0.01);
    CHECK(f.should_fill == true);
    CHECK(f.is_trail == true);
    CHECK_NEAR(f.fill_price, 9.976, 1e-9);
    // Segment 2 (H -> C, 9.996 -> 9.975) is crossed 0.02 / 0.021 of the way.
    CHECK_NEAR(f.path_position, 2.0 + 0.02 / 0.021, 1e-6);
    ExitPathFill raw = resolve_exit_path_fill(
        bar, PositionSide::LONG, kNaN, kNaN, 10.0, kNaN, 2.0,
        9.90, 9.90, false, false, 0.01);
    CHECK(raw.should_fill == false);
}

// The carried best (previous bars' raw extreme) is read quantized for the
// arming test as well: best_start 9.996 with activation 10.00 arrives ARMED
// (tick 10.00), so a bar that only falls (O 9.99 H 9.99 L 9.96 C 9.97)
// crosses 9.996 - 0.02 = 9.976 on its O -> L leg; the old raw compare never
// armed it (no rising leg on this bar) and produced no fill.
void test_resolver_carried_best_arms_quantized() {
    std::printf("-- resolver: carried raw best 9.996 arms (tick 10.00) the 10.00 activation --\n");
    const Bar bar = mk(9.99, 9.99, 9.96, 9.97);
    const Bar tick = tick_twin(bar, 0.01);
    ExitPathFill f = resolve_exit_path_fill(
        bar, tick, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/10.0, kNaN, /*trail_offset=*/2.0,
        /*entry=*/9.90, /*best_start=*/9.996, false, false, 0.01);
    CHECK(f.should_fill == true);
    CHECK(f.is_trail == true);
    CHECK_NEAR(f.fill_price, 9.976, 1e-9);
    // A best that quantizes BELOW the activation (9.994 -> 9.99, and 9.995
    // -> 9.99 too: 9.995 is 9.99499.. in binary) stays dormant, as before.
    for (double best : {9.994, 9.995}) {
        ExitPathFill dormant = resolve_exit_path_fill(
            bar, tick, PositionSide::LONG, kNaN, kNaN, 10.0, kNaN, 2.0,
            9.90, best, false, false, 0.01);
        CHECK(dormant.should_fill == false);
    }
}

// The one-shot trail's activation on a segment whose RAW end already passes
// the level is unchanged: fill at the level, found on the tick path with
// the same chronology (a regression guard for the shared TRAIL slot).
void test_resolver_raw_reach_is_unchanged() {
    std::printf("-- resolver: a raw low through the activation still fills at the level --\n");
    const Bar bar = mk(9.435, 9.44, 9.405, 9.415);   // 16:45Z-like, low 9.405
    const Bar tick = tick_twin(bar, 0.01);
    ExitPathFill f = resolve_exit_path_fill(
        bar, tick, PositionSide::SHORT, kNaN, kNaN, 8.0, kNaN, 0.0,
        9.49, 9.42, false, false, 0.01);
    CHECK(f.should_fill == true);
    CHECK(f.is_trail == true);
    CHECK_NEAR(f.fill_price, 9.41, 1e-12);
    // |O-L| 0.03 > |H-O| 0.005 -> high first, so O -> H is segment 0 and
    // H -> L segment 1; the tick leg is 9.44 -> 9.40 (9.405 is 9.40499.. in
    // binary and quantizes DOWN), crossed 0.03 / 0.04 of the way -> 1.75.
    CHECK_NEAR(f.path_position, 1.75, 1e-9);
}

}  // namespace

int main() {
    test_reissued_trail_points_exits_on_the_quantized_low();
    test_fixed_760_exits_on_the_quantized_low();
    test_fixed_754_exits_on_the_quantized_low();
    test_activation_below_the_quantized_low_does_not_fire();
    test_resolver_activation_is_reached_on_the_tick_path();
    test_resolver_offset_trail_arms_on_the_quantized_extreme();
    test_resolver_carried_best_arms_quantized();
    test_resolver_raw_reach_is_unchanged();
    std::printf("trail_activation_tick_bar: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
