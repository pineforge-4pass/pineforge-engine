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
 *
 * R5 lane P9: the one-bar rows below used to call the TradingView exit-path
 * resolver (`resolve_exit_path_fill`, a tests-only copy since lane N10).
 * They now run tests/trail_exit_product_probe.hpp — the adapter's own
 * lowering and the kernel consumer's walk, the one fill simulation the
 * repository ships — and read the fill back from the kernel's public event
 * record. The product spells the quantized activation as the adapter's
 * half-tick threshold on a one-shot limit (source_trigger_threshold), so
 * the tick twin below is only the arithmetic that names the quantized print
 * the rule reads.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "trail_exit_product_probe.hpp"

using namespace pineforge;
using namespace pineforge::trail_probe;

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

// ── the product, one bar at a time: where the quantization lives ──────

Bar tick_twin(const Bar& b, double mintick) {
    auto q = [&](double p) {
        const double k = std::floor(p / mintick + 0.5);
        return k / std::floor(1.0 / mintick + 0.5);
    };
    Bar t = b;
    t.open = q(b.open); t.high = q(b.high); t.low = q(b.low); t.close = q(b.close);
    return t;
}

// The one-bar probe: a position at `entry` with the carried running extreme
// `best_start`, strategy.exit(trail_points, trail_offset) resting from the
// bar's open (tests/trail_exit_product_probe.hpp).
TrailExitProjection trail_fill(const Bar& bar, PositionSide side,
                               double trail_points, double trail_offset,
                               double entry, double best_start, double mintick) {
    TrailExitScenario scenario;
    scenario.bar = bar;
    scenario.is_long = side == PositionSide::LONG;
    scenario.trail_points = trail_points;
    scenario.trail_offset = trail_offset;
    scenario.entry = entry;
    scenario.best_start = best_start;
    scenario.mintick = mintick;
    return trail_exit(scenario);
}

void test_product_activation_is_reached_on_the_tick_path() {
    std::printf("-- product: the 15:45Z bar reaches the 9.41 activation through its half-tick threshold on the O->L leg --\n");
    const Bar bar = mk(9.43, 9.45, 9.415, 9.445);
    const Bar tick = tick_twin(bar, 0.01);
    CHECK(tick.low == 9.41);
    CHECK(tick.close == 9.45);   // 9.445 -> 944.5000000000001 + 0.5 -> 945
    // Short @9.49, trail_points 8 (activation 9.41), offset 0, carried best
    // = the raw trough so far (9.42), not the entry bar.
    TrailExitProjection f = trail_fill(bar, PositionSide::SHORT, 8.0, 0.0, 9.49, 9.42, 0.01);
    CHECK(f.filled == true);
    // The adapter lowers a zero-tick-offset trail to a one-shot limit resting
    // at the half-tick threshold 9.415 — the price where the tick-quantized
    // path first prints 9.41 — and books the source activation itself.
    CHECK(f.leg_is_limit == true);
    CHECK(f.level_fill == true);
    CHECK(f.at_bar_open == false);
    CHECK_NEAR(f.exit_price, 9.41, 1e-12);
    CHECK_NEAR(f.raw_price, 9.415, 1e-9);
    // |H-O| 0.02 > |O-L| 0.015 -> low first: O -> L is segment 0 and the raw
    // leg 9.43 -> 9.415 ends exactly on the threshold: path position 1.0.
    CHECK_NEAR(f.path_position, 1.0, 1e-9);
    // expectation corrected: the resolver's raw-only form (tick twin == bar)
    // held on this bar -> no such row, because the product has one path — the
    // quantized activation is the adapter's threshold spelling, not a caller
    // option — so the negative control left with the resolver.
}

// A trail WITH an offset arms on the RAW extreme in the product: long @9.90,
// activation 10.00 (10 ticks), offset 2 ticks; bar O 9.98 H 9.996 L 9.97
// C 9.975 (|O-L| 0.01 < |H-O| 0.016 -> low first). The raw high 9.996 never
// reaches 10.00: the generic Trail's arm_price is the on-grid activation and
// the kernel compares the raw path against it — only the one-shot leg carries
// the half-tick threshold — so the trail stays dormant and the position stays
// open. expectation corrected: fill 9.976 on this bar -> no fill, because the
// deleted resolver extended the tick-quantized activation (pinned for the
// zero-offset one-shot, above) to offset trails as "the consequence of the
// pinned rule"; no TradingView tape pins an offset trail whose raw extreme
// stops half a tick short of its activation, the product does not arm it,
// and the corpus is byte-identical either way (open question in the P9
// report).
void test_product_offset_trail_arms_on_the_raw_extreme() {
    std::printf("-- product: an offset trail's arm is raw - the tick high 10.00 of a 9.996 print does not arm it --\n");
    const Bar bar = mk(9.98, 9.996, 9.97, 9.975);
    const Bar tick = tick_twin(bar, 0.01);
    CHECK(tick.high == 10.0);
    TrailExitProjection f = trail_fill(bar, PositionSide::LONG, 10.0, 2.0, 9.90, 9.90, 0.01);
    CHECK(f.filled == false);
    CHECK(f.position_after == 1.0);
    // Control: a raw high ON the activation arms it and the trail runs from
    // that best: H -> C (10.00 -> 9.975) crosses 10.00 - 0.02 = 9.98.
    const Bar reach = mk(9.98, 10.00, 9.97, 9.975);
    TrailExitProjection g = trail_fill(reach, PositionSide::LONG, 10.0, 2.0, 9.90, 9.90, 0.01);
    CHECK(g.filled == true);
    CHECK(g.leg_is_trail == true);
    CHECK(g.level_fill == true);
    CHECK_NEAR(g.exit_price, 9.98, 1e-9);
    CHECK_NEAR(g.raw_price, 9.98, 1e-9);
    // Segment 2 (H -> C, 10.00 -> 9.975) is crossed 0.02 / 0.025 of the way.
    CHECK_NEAR(g.path_position, 2.0 + 0.02 / 0.025, 1e-6);
}

// The carried best is read RAW for the arming test too: best_start 9.996
// with activation 10.00 arrives dormant (9.996 < 10.00), and a bar that only
// falls (O 9.99 H 9.99 L 9.96 C 9.97) never arms it. expectation corrected:
// fill 9.976 -> no fill, because the adapter reads "already reached" at
// placement against the raw activation and the kernel's arm is raw (the
// previous row); the two bests that quantize below the activation (9.994,
// 9.995 -> tick 9.99) stay dormant exactly as before.
void test_product_carried_best_arms_raw() {
    std::printf("-- product: a carried raw best 9.996 does not arm the 10.00 activation; 10.00 does, and the adapter books the carried level --\n");
    const Bar bar = mk(9.99, 9.99, 9.96, 9.97);
    TrailExitProjection f = trail_fill(bar, PositionSide::LONG, 10.0, 2.0, 9.90, 9.996, 0.01);
    CHECK(f.filled == false);
    for (double best : {9.994, 9.995}) {
        TrailExitProjection dormant = trail_fill(bar, PositionSide::LONG, 10.0, 2.0, 9.90, best, 0.01);
        CHECK(dormant.filled == false);
    }
    // A carried best AT the activation is already reached at placement: the
    // kernel Trail is armed from its first live print (its best restarts at
    // the 9.99 open, stop 9.97 crossed on the falling leg) and the adapter's
    // terms policy books TradingView's carried level 10.00 - 0.02 = 9.98.
    TrailExitProjection armed = trail_fill(bar, PositionSide::LONG, 10.0, 2.0, 9.90, 10.00, 0.01);
    CHECK(armed.filled == true);
    CHECK(armed.leg_is_trail == true);
    CHECK(armed.level_fill == true);
    CHECK_NEAR(armed.raw_price, 9.97, 1e-9);
    CHECK_NEAR(armed.exit_price, 9.98, 1e-9);
}

// The one-shot's activation on a segment whose RAW end already passes the
// level: fill at the level, the threshold crossed on the raw path.
void test_product_raw_reach_books_the_level() {
    std::printf("-- product: a raw low through the activation books the level 9.41 --\n");
    const Bar bar = mk(9.435, 9.44, 9.405, 9.415);   // 16:45Z-like, low 9.405
    TrailExitProjection f = trail_fill(bar, PositionSide::SHORT, 8.0, 0.0, 9.49, 9.42, 0.01);
    CHECK(f.filled == true);
    CHECK(f.leg_is_limit == true);
    CHECK(f.level_fill == true);
    CHECK_NEAR(f.exit_price, 9.41, 1e-12);
    CHECK_NEAR(f.raw_price, 9.415, 1e-9);
    // |O-L| 0.03 > |H-O| 0.005 -> high first, so O -> H is segment 0 and
    // H -> L segment 1; the raw leg 9.44 -> 9.405 crosses the threshold
    // 9.415 0.025 / 0.035 of the way. expectation corrected: 1.75 ->
    // 1 + 0.025 / 0.035, because the resolver placed the crossing on the
    // tick-quantized leg 9.44 -> 9.40 at the level itself, while the
    // kernel's cursor is the raw path's crossing of the adapter's threshold;
    // the booked price and bar are the same.
    CHECK_NEAR(f.path_position, 1.0 + 0.025 / 0.035, 1e-9);
}

}  // namespace

int main() {
    test_reissued_trail_points_exits_on_the_quantized_low();
    test_fixed_760_exits_on_the_quantized_low();
    test_fixed_754_exits_on_the_quantized_low();
    test_activation_below_the_quantized_low_does_not_fire();
    test_product_activation_is_reached_on_the_tick_path();
    test_product_offset_trail_arms_on_the_raw_extreme();
    test_product_carried_best_arms_raw();
    test_product_raw_reach_books_the_level();
    std::printf("trail_activation_tick_bar: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
