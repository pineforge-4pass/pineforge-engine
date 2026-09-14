/*
 * test_zero_offset_trail_rides.cpp — round 10 family AC: the EXPLICIT
 * trail_offset = 0 (or sub-tick) trailing exit is a zero-distance trailing
 * stop on the raw running best once it is armed. A bar that opens past its
 * activation is NOT automatically the one-shot fill the omitted-offset shape
 * takes: whether that open fills or the trail rides from it is decided by the
 * TICK GRID the open lands on (rule 3). Pinned with 76 `lab
 * tv` tapes on NYSE:F 15m and NASDAQ:AAPL 15m (scratchpad/r10/famAC/pins,
 * pins2, pins3, pins4, pins5; ledger log-20260905t214404z-57eb640d, its pins2
 * supplement and the pins4/pins5 notes of 2026-09-05 23:2xZ),
 * replayed here on the registry feed bars (`lab bars`, feeds 80f404ae85ef /
 * ae2b03d3736f). The seed: boztilkiserhan-serhan1-wma-rsi-trailing-scalp on
 * NYSE:F 15m re-issues strategy.exit(trail_points = close * 1.5% /
 * syminfo.mintick, trail_offset = 0) on every bar; its long filled
 * 2025-10-24 13:45Z @13.26 and TV exits at the 14:00Z open PRINT 13.49
 * (O 13.485), where the round-7 open-as-level shortcut (fa8fd0e) floored
 * 13.48 — one cent that re-rolled every later all-in placement.
 *
 * The rule, in the order TradingView applies it:
 *   (1) ARMING. The trail arms at the first price that reaches its
 *       activation: the placement bar's close (the running extreme restarts
 *       there — round 9 family Z; the entry bar's high does NOT arm an exit
 *       issued at that bar's close: p26 / p27), then each bar's raw open
 *       (g1 / g1b: 12.255 does not arm a 12.26 activation), then the
 *       tick-quantized path extremes. An exit issued ALONGSIDE the entry is
 *       live on the entry bar itself and fires there (a1 / a2: 13.52, 13.50
 *       at 13:45Z).
 *   (2) OPEN AT / THROUGH THE LEVEL. Armed before the bar, level = best; an
 *       open at or through it (long: open <= best) is a stop the open
 *       filled — the OPEN PRINT, nearest-in-double like every gapped-through
 *       print (13.485 -> 13.49, 9.465 -> 9.47, 9.485 -> 9.48, 9.665 -> 9.66,
 *       13.915 -> 13.91, 10.075 -> 10.07), regardless of the bar's path
 *       (high-first flat opens fill at the open, not at the high: 9.47 not
 *       9.50, 9.57 not 9.62; short 9.27 not 9.25).
 *   (3) OPEN BEYOND THE BEST. A favourable gap raises the best to the open
 *       (arming it if it was dormant). TradingView places the fresh stop at
 *       that open snapped DIRECTIONALLY to the tick grid (long floor, short
 *       ceil) and tests it at once against the open's PRINT (the nearest
 *       tick, floor(x / tick + 0.5) in doubles — bar_fill_price):
 *         - print AT the stop (it rounds toward the stop side; an on-grid
 *           open trivially): the exit fills at the open, a level fill booked
 *           at that stop, whatever the bar's path — on-grid AAPL 05-12
 *           211.05 (not the 211.26 high), 04-08 186.65, 09-03 237.18, 04-14
 *           211.44, 03-23 254.13, 07-25 214.75, 10-31 276.90, 04-17 197.13,
 *           01-31 247.07, 08-07 218.90, armed-before-the-bar 05-16 212.31,
 *           07-23 215.00, 09-05 239.96; NYSE:F 03-23 11.89, 05-20 10.80,
 *           07-25 11.33, 03-16 11.84, short 03-31 9.58, 03-12 11.96;
 *           sub-tick with the print toward the stop: long 272.335 ->
 *           272.33, 271.835 -> 271.83; short 208.955 -> 208.96, 227.125 ->
 *           227.13, 234.445 -> 234.45, 217.005 -> 217.01, 193.665 ->
 *           193.67, 221.025 -> 221.03, 189.945 -> 189.95, NYSE:F 9.515 ->
 *           9.52, 14.335 -> 14.34;
 *         - print one tick BEYOND the stop (it rounds away from it): nothing
 *           is touched at the open and the trail RIDES the path from best =
 *           the raw open: it fills on the first against-direction leg at the
 *           level = best, snapped directionally (long floor 196.135 ->
 *           196.13, 9.085 -> 9.08, 12.105 -> 12.10; short ceil 9.325 ->
 *           9.33, 9.735 -> 9.74), or rides a with-direction first leg to the
 *           extreme and fills there (12.255 -> the 12.32 high, 12.075 ->
 *           12.11, 203.575 -> 203.78, 211.895 -> 212.39, 253.205 -> 254.32,
 *           226.185 -> 226.95; short 9.915 -> the 9.86 low, 9.575 -> 9.51).
 *       The 41 pins4 / pins5 tapes vary the ratio |O-L|/|H-O|, |H-O|, the
 *       candle, the gap size, the symbol, the side and the dormant / armed
 *       state across this branch: they separate on the print-vs-level test
 *       and on nothing else (the sibling on-grid-only rule 9befe0e is
 *       refuted by every sub-tick open whose print rounds toward the stop).
 *   (4) INTRABAR ACTIVATION. An activation first reached by a path leg is
 *       the one-shot fill at the activation itself (13.50 / 13.52 / 13.53,
 *       12.26) — unchanged.
 * Sub-tick offsets round down to zero ticks and follow the same rule
 * (test_trail_open_arm_subtick_offset); a whole-tick offset trails at
 * best -/+ K ticks as before (p21-off1: 13.54 = the 14:00Z high - 1t;
 * aapl-ctl-tp384-off1: 196.12).
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/engine_internal.hpp"

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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b{};
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// How the sensor script issued its strategy.exit:
//   kLive          on every bar the position is live, from the entry bar's
//                  close (`if strategy.position_size > 0`, the probe's shape)
//   kOnce          on the first live bar only (p21-once)
//   kAlongside     with the entry on the signal bar, never again (a1 / a2)
//   kProbeReissue  kLive with trail_points = close * 1.5% / mintick, the
//                  probe's own re-issue (p0-reissue)
enum class Mode { kLive, kOnce, kAlongside, kProbeReissue };

struct TapeCase {
    const char* name;
    const char* symbol;
    const char* feed;
    const char* signal_utc;   // the sig bar; the entry fills at the next open
    bool is_long;
    double trail_points;      // NaN under kProbeReissue
    double trail_offset;
    Mode mode;
    double tv_exit_price;     // the tape's "Exit long/short" price
    int tv_exit_bar;          // index in `bars` (1 = the entry bar)
    std::vector<Bar> bars;    // sig bar + 4, registry feed bars (UTC)
};

#include "zero_offset_trail_rides_cases.inc"

class TapeProbe : public pineforge::source::PineStrategyHost {
public:
    explicit TapeProbe(const TapeCase& c) : c_(c) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("E", c_.is_long, kNaN, kNaN, /*qty=*/1.0);
            if (c_.mode == Mode::kAlongside) issue(bar);
            return;
        }
        if (position_side_ == PositionSide::FLAT) return;
        switch (c_.mode) {
            case Mode::kLive:
            case Mode::kProbeReissue:
                issue(bar);
                break;
            case Mode::kOnce:
                if (!issued_) issue(bar);
                break;
            case Mode::kAlongside:
                break;
        }
    }

    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position() const { return signed_position_size(); }

private:
    void issue(const Bar& bar) {
        const double tp = (c_.mode == Mode::kProbeReissue)
            ? bar.close * 0.015 / syminfo_mintick_
            : c_.trail_points;
        strategy_exit("X", "E", /*limit=*/kNaN, /*stop=*/kNaN,
                      tp, c_.trail_offset, /*trail_price=*/kNaN);
        issued_ = true;
    }

    const TapeCase& c_;
    bool issued_ = false;
};

void test_tape(const TapeCase& c) {
    std::printf("tape %-32s %s %s\n", c.name, c.symbol, c.signal_utc);
    TapeProbe eng(c);
    eng.run(c.bars.data(), (int)c.bars.size());
    CHECK(eng.last_error().empty());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() != 1) return;
    // The entry is the bar-1 open, booked as a print — the nearest tick, which
    // is the open itself wherever that open is already on the grid. TV's own
    // tapes report exactly this for the three sub-tick entry opens in the set
    // (NYSE:F 11.425 -> 11.43 and 12.005 -> 12.01, NASDAQ:AAPL 198.695 ->
    // 198.70), so the print rounding is the tape's, not the harness's.
    const double entry_print =
        std::floor(c.bars[1].open / 0.01 + 0.5) * 0.01;
    CHECK(near(eng.entry_price(0), entry_print, 1e-6));
    if (!near(eng.exit_price(0), c.tv_exit_price) || eng.exit_bar(0) != c.tv_exit_bar) {
        std::printf("        engine exit %.5f @bar %d, TV %.2f @bar %d\n",
                    eng.exit_price(0), eng.exit_bar(0), c.tv_exit_price,
                    c.tv_exit_bar);
    }
    CHECK(near(eng.exit_price(0), c.tv_exit_price));
    CHECK(eng.exit_bar(0) == c.tv_exit_bar);
    CHECK(near(eng.position(), 0.0));
}

}  // namespace

int main() {
    std::printf("=== test_zero_offset_trail_rides ===\n");
    for (const TapeCase& c : kCases) test_tape(c);
    std::printf("passed=%d failed=%d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
