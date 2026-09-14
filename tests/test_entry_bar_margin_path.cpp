/*
 * test_entry_bar_margin_path.cpp — round 7 family L: the ENTRY bar's margin
 * call follows the fill on the OHLC path.
 *
 * Rule (campaign pin log-20260905t093952z-0c4938cb, 2026-09-05; lab tv tapes
 * scratchpad/r7/pins/xau15-mcpath-{a,b} on OANDA:XAUUSD 15 and the round-7
 * family-E tape fresh-touch-once on NYSE:F 15; probe rows waranyutrkm
 * asian-box-breakout XAUUSD@15 2025-04-01 15:45Z and mdfe3757 trade-strategy
 * v8.4 XAUUSD@15 2025-04-08 13:30Z, engine rows from cand-round7f-engine-a):
 * on the bar a position OPENS, TradingView evaluates the forced liquidation
 * only over the part of the synthesized O-H-L-C / O-L-H-C path AFTER the
 * entry fill —
 *
 *   - a sell stop filled below the open of a BEARISH (high-first) bar sees L
 *     then C: no slice at that bar's pre-fill high; the bar CLOSE is a mark;
 *   - a fill at the open (a market order, or a stop the open gapped through)
 *     sees H, L, C — the whole bar — so a same-bar slice at the post-fill
 *     high is right;
 *   - carried bars keep the whole-bar extreme, the fill-price checkpoint
 *     (opening-affordability trim) runs first, then the post-fill pass.
 *
 * The engine marked the just-opened position at the whole bar's extreme
 * (wrong both ways: a phantom 1.0-lot slice at 3124.295 on asian-box, no
 * same-bar slice on mdfe3757). Every replay below runs on the registry's own
 * bars (lab bars OANDA:XAUUSD 15, feed 248086b8b82d; NYSE:F 15, feed
 * 80f404ae85ef) with the tape's capital and orders, and asserts TV's rows:
 *
 *   A. xau15-mcpath-a — bearish entry bar, stop fill below the open: NO
 *      entry-bar slice although the pre-fill high 2975.73 breaches; 1.0 lot
 *      "Margin call" on the NEXT bar at its high 2975.345 (a carried bar,
 *      whole-bar extreme), the 2.36 remainder closes 22:15Z @2984.185.
 *   B. xau15-mcpath-b — bullish entry bar, fill at the open (control): the
 *      1.0-lot slice at the post-fill high 2980 stays on the entry bar.
 *   C. waranyutrkm asian-box 2025-04-01 15:45Z — bearish entry bar, stop
 *      3120.335 x 3.2: no slice at all (TV #1: 3.2 lots to the EOD close
 *      22:15Z @3112.245, +25.888); the engine printed 1.0 @3124.295.
 *   D. mdfe3757 2025-04-08 13:30Z — explicit-qty MARKET short at the open of
 *      a bearish bar: the 1.28-lot fee trim at the fill 3013.745 (unchanged),
 *      THEN 2.4 lots at the same bar's high 3017.3; nothing at 14:00Z (the
 *      engine printed 3.88 @3018.125 there and nothing on the entry bar).
 *   E. fresh-touch-once — short stop 11.23 x 890 touched below the 11.29 open
 *      of a bar whose high IS the open: 8 @11.25 on the entry bar (the CLOSE
 *      is a post-fill mark), then the unchanged carried cascade 24 @11.33,
 *      1 @11.45, 4 @11.46, 4 @11.49 (TV's own slices; the engine printed
 *      32 @11.29 on the entry bar).
 *   F. Synthetic leveraged LONG (margin 20): a buy stop filled above the open
 *      of a bullish (low-first) bar ignores the pre-fill low (no slice); the
 *      same bar entered by a market order at the open is sliced at that low
 *      with the raw-low mark (12 @92).
 *   G. Round-7 family-H residual (NYSE:F 1D short tape 2025-04-23 /
 *      2026-04-08, replayed row-for-row in test_market_admission_commission):
 *      a whole-position strategy.close resting for a gap-open fills BEFORE
 *      the open's margin evaluation — no open slice; a partial close keeps
 *      the finding-430 open slice ahead of it (unpinned, unchanged).
 *   H. Round-8 regression (cand-round8-engine-a-20260905, 19 all-in reversal
 *      scripts down): the close of a `strategy.entry(long) + strategy.close
 *      (short)` reversal pair is NOT that unconditional close. TradingView
 *      decides the reversal's admission at the open first and a declined
 *      reversal voids its close (pin log-20260905t111645z-e1783b94), so the
 *      open slice stands: amandaborgeson06 bias-status NYSE:F@15 2025-05-01
 *      13:30Z (40 @10.15, TV #34) and hexatrades technical-strength-gauge
 *      NASDAQ:AAPL@15 2025-07-29 13:30Z (24 @214.16 THEN 72 @214.81, TV
 *      #253/#254), both on the registry bars with TV's own equity. The
 *      over-general guard stood down and sliced 96 @10.23 / 168 @214.81.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

struct BarRow {
    int64_t ts;
    double open, high, low, close;
};

template <size_t N>
std::vector<Bar> to_bars(const BarRow (&rows)[N]) {
    std::vector<Bar> out;
    out.reserve(N);
    for (const BarRow& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

// OANDA:XAUUSD 15 (feed 248086b8b82d), 2025-04-07 16:30Z .. 22:15Z. The feed
// has no 21:00Z-21:45Z bars (the OANDA daily break); bar_index counts the
// rows as given.
static const BarRow kXauMcpathA[] = {
    {1744043400000LL, 2977.895, 2984.98, 2975.098, 2975.22},   // [0] 16:30
    {1744044300000LL, 2975.185, 2975.73, 2969.975, 2970.925},  // [1] 16:45 entry bar
    {1744045200000LL, 2970.945, 2975.345, 2959.6, 2966.36},    // [2] 17:00
    {1744046100000LL, 2966.395, 2970.9, 2956.89, 2959.17},     // [3] 17:15
    {1744047000000LL, 2958.725, 2968.995, 2956.565, 2968.215}, // [4] 17:30
    {1744047900000LL, 2968.25, 2973.775, 2966.11, 2970.09},    // [5] 17:45
    {1744048800000LL, 2970.075, 2973.545, 2969.435, 2971.04},  // [6] 18:00
    {1744049700000LL, 2971.035, 2974.21, 2969.255, 2969.895},  // [7] 18:15
    {1744050600000LL, 2969.955, 2976.745, 2968.585, 2971.85},  // [8] 18:30
    {1744051500000LL, 2971.855, 2972.545, 2964.145, 2965.92},  // [9] 18:45
    {1744052400000LL, 2965.9, 2972.86, 2965.495, 2971.715},    // [10] 19:00
    {1744053300000LL, 2971.605, 2975.48, 2969.405, 2974.05},   // [11] 19:15
    {1744054200000LL, 2974.04, 2982.325, 2973.46, 2978.67},    // [12] 19:30
    {1744055100000LL, 2978.765, 2978.91, 2974.865, 2976.76},   // [13] 19:45
    {1744056000000LL, 2976.8, 2980.8, 2970.13, 2979.44},       // [14] 20:00
    {1744056900000LL, 2979.425, 2985.93, 2979.425, 2983.9},    // [15] 20:15
    {1744057800000LL, 2983.91, 2986.055, 2983.315, 2985.315},  // [16] 20:30
    {1744058700000LL, 2985.32, 2985.38, 2981.905, 2981.94},    // [17] 20:45
    {1744063200000LL, 2982.095, 2984.25, 2981.195, 2983.95},   // [18] 22:00 close_all
    {1744064100000LL, 2984.185, 2985.91, 2982.6, 2985.745},    // [19] 22:15 fill
};

// OANDA:XAUUSD 15, 2025-04-08 23:30Z .. 2025-04-09 04:15Z.
static const BarRow kXauMcpathB[] = {
    {1744155000000LL, 2981.015, 2981.275, 2969.97, 2973.78},   // [0] 23:30 placement
    {1744155900000LL, 2973.84, 2980, 2970.48, 2978.56},        // [1] 23:45 entry bar
    {1744156800000LL, 2978.575, 2984.95, 2975.085, 2978.03},   // [2] 00:00
    {1744157700000LL, 2978.08, 2985.72, 2975.73, 2984.39},     // [3] 00:15
    {1744158600000LL, 2984.37, 2987.1, 2976.79, 2984.715},     // [4] 00:30
    {1744159500000LL, 2984.735, 2986.3, 2974.78, 2980.495},    // [5] 00:45
    {1744160400000LL, 2980.45, 2981.32, 2972.415, 2978.675},   // [6] 01:00
    {1744161300000LL, 2978.51, 2991.8, 2977.275, 2990.315},    // [7] 01:15
    {1744162200000LL, 2990.335, 3002.77, 2988.59, 3000.3},     // [8] 01:30
    {1744163100000LL, 3000.35, 3009.81, 2999.65, 3008.895},    // [9] 01:45
    {1744164000000LL, 3008.68, 3011.15, 3004.37, 3007.335},    // [10] 02:00
    {1744164900000LL, 3007.355, 3009.64, 3000.055, 3004.715},  // [11] 02:15
    {1744165800000LL, 3004.735, 3007.805, 3002.82, 3006.24},   // [12] 02:30
    {1744166700000LL, 3006.18, 3008.865, 3000.075, 3003.195},  // [13] 02:45
    {1744167600000LL, 3003.115, 3008.005, 3002.77, 3007.705},  // [14] 03:00
    {1744168500000LL, 3007.685, 3008.965, 3004.615, 3008.37},  // [15] 03:15
    {1744169400000LL, 3008.385, 3009.84, 3003.155, 3006.675},  // [16] 03:30
    {1744170300000LL, 3006.67, 3009.205, 3003.665, 3004.435},  // [17] 03:45
    {1744171200000LL, 3004.39, 3009.395, 2999.57, 3006.78},    // [18] 04:00 close_all
    {1744172100000LL, 3006.775, 3012.185, 3006.25, 3008.66},   // [19] 04:15 fill
};

// OANDA:XAUUSD 15, 2025-04-01 15:30Z .. 22:15Z (no 21:00Z-21:45Z bars).
static const BarRow kXauAsianBox[] = {
    {1743521400000LL, 3126.63, 3127.345, 3119.33, 3121.325},   // [0] 15:30 placement
    {1743522300000LL, 3121.33, 3124.295, 3113.44, 3113.79},    // [1] 15:45 entry bar
    {1743523200000LL, 3113.755, 3116.855, 3106.715, 3107.08},  // [2] 16:00
    {1743524100000LL, 3107.125, 3108.84, 3100.87, 3107.885},   // [3] 16:15
    {1743525000000LL, 3107.9, 3110.66, 3105.27, 3107.205},     // [4] 16:30
    {1743525900000LL, 3107.195, 3110.845, 3106.425, 3106.715}, // [5] 16:45
    {1743526800000LL, 3106.705, 3108.815, 3105.77, 3108.69},   // [6] 17:00
    {1743527700000LL, 3108.685, 3114.37, 3108.685, 3114.045},  // [7] 17:15
    {1743528600000LL, 3114.05, 3114.675, 3112.215, 3113.175},  // [8] 17:30
    {1743529500000LL, 3113.13, 3114.51, 3112.7, 3113.755},     // [9] 17:45
    {1743530400000LL, 3113.735, 3117.18, 3113.36, 3116.89},    // [10] 18:00
    {1743531300000LL, 3116.955, 3117.35, 3114.13, 3116.42},    // [11] 18:15
    {1743532200000LL, 3116.47, 3118.74, 3113.42, 3113.58},     // [12] 18:30
    {1743533100000LL, 3113.595, 3115.135, 3113.435, 3114.17},  // [13] 18:45
    {1743534000000LL, 3114.175, 3115.255, 3113.775, 3115.205}, // [14] 19:00
    {1743534900000LL, 3115.065, 3118.41, 3115.04, 3117.085},   // [15] 19:15
    {1743535800000LL, 3117.1, 3117.705, 3116.405, 3116.835},   // [16] 19:30
    {1743536700000LL, 3116.895, 3119.165, 3116.75, 3118.89},   // [17] 19:45
    {1743537600000LL, 3118.695, 3119.11, 3118.065, 3118.385},  // [18] 20:00
    {1743538500000LL, 3118.36, 3119.985, 3118.115, 3119.62},   // [19] 20:15
    {1743539400000LL, 3119.575, 3120.78, 3118.29, 3118.505},   // [20] 20:30
    {1743540300000LL, 3118.545, 3119.495, 3113.19, 3114.475},  // [21] 20:45
    {1743544800000LL, 3114.095, 3114.875, 3112.22, 3112.295},  // [22] 22:00 close_all
    {1743545700000LL, 3112.245, 3112.565, 3107.83, 3111.43},   // [23] 22:15 fill
};

// OANDA:XAUUSD 15, 2025-04-08 13:15Z .. 14:45Z.
static const BarRow kXauMdfe[] = {
    {1744118100000LL, 3018.405, 3022.76, 3013.255, 3013.72},   // [0] 13:15 signal
    {1744119000000LL, 3013.745, 3017.3, 3006.43, 3006.91},     // [1] 13:30 entry bar
    {1744119900000LL, 3006.91, 3012.095, 3004.005, 3010.38},   // [2] 13:45
    {1744120800000LL, 3010.405, 3018.125, 3009.245, 3011.61},  // [3] 14:00
    {1744121700000LL, 3011.6, 3014.94, 3008.39, 3012.57},      // [4] 14:15
    {1744122600000LL, 3012.575, 3017.72, 3010.16, 3014.085},   // [5] 14:30
    {1744123500000LL, 3014.14, 3018.275, 3005.865, 3006.26},   // [6] 14:45
};

// NYSE:F 15 (feed 80f404ae85ef), 2025-08-12 19:45Z, then 2025-08-13 13:30Z ..
// 19:45Z (the same rows test_stop_entry_admission.cpp replays the family-E
// tapes on). [0] = placement bar; [1] = the entry bar; [2] = 13:45Z; ...
std::vector<Bar> ford_bars() {
    constexpr int64_t kMin15 = 15LL * 60LL * 1000LL;
    const int64_t t0812 = 1755027900000LL;   // 2025-08-12 19:45Z
    const int64_t t0813 = 1755091800000LL;   // 2025-08-13 13:30Z
    const double d13[][4] = {
        {11.29, 11.29, 11.19, 11.25},      {11.255, 11.325, 11.25, 11.325},
        {11.325, 11.365, 11.32, 11.33},    {11.335, 11.335, 11.26, 11.285},
        {11.285, 11.34, 11.28, 11.335},    {11.33, 11.335, 11.3, 11.325},
        {11.33, 11.36, 11.325, 11.355},    {11.355, 11.415, 11.355, 11.39},
        {11.39, 11.4, 11.375, 11.385},     {11.385, 11.385, 11.345, 11.37},
        {11.375, 11.42, 11.37, 11.415},    {11.415, 11.45, 11.415, 11.425},
        {11.425, 11.45, 11.425, 11.44},    {11.445, 11.45, 11.435, 11.445},
        {11.44, 11.45, 11.41, 11.41},      {11.415, 11.445, 11.415, 11.425},
        {11.425, 11.43, 11.4, 11.415},     {11.415, 11.435, 11.415, 11.425},
        {11.425, 11.45, 11.415, 11.415},   {11.415, 11.44, 11.415, 11.435},
        {11.44, 11.445, 11.42, 11.43},     {11.43, 11.45, 11.43, 11.435},
        {11.435, 11.455, 11.435, 11.455},  {11.455, 11.47, 11.455, 11.465},
        {11.465, 11.485, 11.46, 11.475},   {11.475, 11.48, 11.425, 11.425},
    };
    std::vector<Bar> b;
    Bar first;
    first.timestamp = t0812;
    first.open = 11.23; first.high = 11.25; first.low = 11.2; first.close = 11.24;
    first.volume = 1.0;
    b.push_back(first);
    for (int i = 0; i < 26; ++i) {
        Bar x;
        x.timestamp = t0813 + i * kMin15;
        x.open = d13[i][0]; x.high = d13[i][1]; x.low = d13[i][2];
        x.close = d13[i][3];
        x.volume = 1.0;
        b.push_back(x);
    }
    return b;
}

// The tapes' broker: explicit-qty entries (default FIXED 1 is never used),
// 1x margin both sides, margin calls on, market fills at the next open.
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, double mintick, double lot, double commission_pct,
          double margin_pct = 100.0) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = mintick;
        syminfo_mintick_ = mintick;
        qty_step_ = lot;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_pct;
        margin_long_ = margin_pct;
        margin_short_ = margin_pct;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    std::function<void(Probe&, int)> script;
    void on_source_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }
    void entry_stop(const std::string& id, bool is_long, double level,
                    double qty) {
        strategy_entry(id, is_long, kNaN, level, qty, "");
    }
    void entry_market(const std::string& id, bool is_long, double qty) {
        strategy_entry(id, is_long, kNaN, kNaN, qty, "");
    }
    void close_all() { strategy_close_all(); }
    // default_qty_type = percent_of_equity, 100 (the all-in reversal scripts).
    void all_in() {
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
    }
    using pineforge::source::PineStrategyHost::strategy_close;
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    int margin_call_rows() const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_comment == "Margin call") ++n;
        }
        return n;
    }
    int rows_exiting_on(int bar) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_bar_index == bar) ++n;
        }
        return n;
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

void print_trades(const Probe& p) {
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        std::printf("      trade %d: %s entry bar %d @ %.5f qty %.4f exit bar %d @ %.5f pnl %.5f [%s]\n",
                    i, t.is_long ? "long" : "short", t.entry_bar_index,
                    t.entry_price, t.qty, t.exit_bar_index, t.exit_price,
                    t.pnl, t.exit_comment.c_str());
    }
}

void check_trade(const Probe& p, int i, bool is_long, int entry_bar,
                 double entry_price, double qty, int exit_bar,
                 double exit_price, const char* exit_comment, double pnl) {
    CHECK(i < p.trade_count());
    if (i >= p.trade_count()) return;
    const Trade& t = p.get_trade(i);
    CHECK(t.is_long == is_long);
    CHECK(t.entry_bar_index == entry_bar);
    CHECK_NEAR(t.entry_price, entry_price, 1e-9);
    CHECK_NEAR(t.qty, qty, 1e-9);
    CHECK(t.exit_bar_index == exit_bar);
    CHECK_NEAR(t.exit_price, exit_price, 1e-9);
    CHECK(t.exit_comment == exit_comment);
    CHECK_NEAR(t.pnl, pnl, 5e-3);
}

// ---------------------------------------------------------------------------
// A. xau15-mcpath-a: sell stop 2970.215 x 3.36 (capital 10,000) placed at the
// 04-07 16:30Z close 2975.22; the 16:45Z entry bar is BEARISH (O 2975.185 H
// 2975.73 L 2969.975 C 2970.925, high first). The pre-fill high breaches
// (equity 9981.5 < 3.36 x 2975.73) yet TV slices nothing on the entry bar:
// after the fill the path is L then C (3.36 x 2970.925 = 9982.3 <= 9997.6).
// The slice comes on the NEXT bar at its high 2975.345 — 1.0 lot (the restore
// quantity 0.0049 floors below the 0.01 lot: one-contract fallback), duration
// 1 — and the 2.36 remainder closes at the 22:15Z open 2984.185.
// ---------------------------------------------------------------------------
void test_mcpath_a_bearish_stop_fill_no_entry_bar_slice() {
    std::printf("-- A. xau15-mcpath-a: bearish entry bar, no pre-fill-high slice; next bar 1.0 @2975.345 --\n");
    Probe p(10000.0, 0.005, 0.01, 0.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_stop("S", false, 2970.215, 3.36);
        if (bar == 18) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kXauMcpathA);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.rows_exiting_on(1) == 0);              // nothing on the entry bar
    CHECK(p.margin_call_rows() == 1);
    check_trade(p, 0, false, 1, 2970.215, 1.0, 2, 2975.345, "Margin call",
                -5.13);
    check_trade(p, 1, false, 1, 2970.215, 2.36, 19, 2984.185, "", -32.9692);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// B. xau15-mcpath-b (control): sell stop 2973.84 x 3.36 (capital 10,020)
// placed at the 04-08 23:30Z close 2973.78; the 23:45Z bar opens AT the level
// (O 2973.84 H 2980 L 2970.48 C 2978.56, low first) so the fill is the open and
// the whole bar follows it: 1.0 lot "Margin call" at the high 2980 on the
// entry bar itself (equity 9999.3 < 3.36 x 2980 = 10012.8), duration 0; the
// 2.36 remainder closes 04-09 04:15Z @3006.775.
// ---------------------------------------------------------------------------
void test_mcpath_b_bullish_open_fill_same_bar_slice() {
    std::printf("-- B. xau15-mcpath-b: fill at the open, same-bar slice 1.0 @2980 --\n");
    Probe p(10020.0, 0.005, 0.01, 0.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_stop("S", false, 2973.84, 3.36);
        if (bar == 18) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kXauMcpathB);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.margin_call_rows() == 1);
    check_trade(p, 0, false, 1, 2973.84, 1.0, 1, 2980.0, "Margin call", -6.16);
    check_trade(p, 1, false, 1, 2973.84, 2.36, 19, 3006.775, "", -77.7266);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// C. waranyutrkm asian-box XAUUSD@15, TV trade #1: sell stop 3120.335 x 3.2
// (capital 10,000) resting from the 15:30Z bar; the 15:45Z entry bar is
// BEARISH (O 3121.33 H 3124.295 L 3113.44 C 3113.79, high first). At the
// pre-fill high 3.2 x 3124.295 = 9997.74 > 9987.33 — the engine sliced 1.0 lot
// there (its row: 1 @3124.295, -4.17); TV slices nothing: after the fill the
// path is L then C, both below the entry. The EOD close_all at the 22:00Z bar
// fills the whole 3.2 at the 22:15Z open 3112.245, +25.888 (TV's row).
// ---------------------------------------------------------------------------
void test_asian_box_0401_no_phantom_slice() {
    std::printf("-- C. asian-box 2025-04-01 15:45Z: no slice, 3.2 lots to the EOD close --\n");
    Probe p(10000.0, 0.005, 0.01, 0.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_stop("Short Breakout", false, 3120.335, 3.2);
        if (bar == 22) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kXauAsianBox);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.margin_call_rows() == 0);
    CHECK(p.trade_count() == 1);
    check_trade(p, 0, false, 1, 3120.335, 3.2, 23, 3112.245, "", 25.888);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// D. mdfe3757 XAUUSD@15 2025-04-08 13:30Z: explicit qty min(riskQty,
// equity/close) = floor2(1,998,000.02 / 3013.72) = 662.96 lots, 0.05%
// commission, market short at the 13:30Z open 3013.745 of a BEARISH bar (O
// 3013.745 H 3017.3 L 3006.43 C 3006.91). TV (rows 3-5): the fill-price
// checkpoint trims 1.28 lots at 3013.745 (cost 1,997,992.4 vs equity net of
// the 998.996 fee 1,997,001.03: restore 0.329 -> 0.32 x 4), THEN the survivor
// 661.68 is marked at the same bar's post-fill high 3017.3: 2.4 lots (restore
// 0.6099 -> 0.60 x 4), and the 659.28 remainder rides on (no deficit at the
// 14:00Z high 3018.125: equity 1,994,099 >= 1,989,790). The engine printed
// the 1.28 trim, nothing else on the entry bar, and 3.88 @3018.125 at 14:00Z.
// ---------------------------------------------------------------------------
void test_mdfe3757_0408_market_open_fill_same_bar_cascade() {
    std::printf("-- D. mdfe3757 2025-04-08 13:30Z: 1.28 trim at the fill, 2.4 @3017.3 same bar, nothing at 14:00Z --\n");
    Probe p(2000000.0 - 1999.9751, 0.005, 0.01, 0.05);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_market("Short", false, 662.96);
    };
    std::vector<Bar> bars = to_bars(kXauMdfe);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.margin_call_rows() == 2);
    CHECK(p.rows_exiting_on(1) == 2);
    CHECK(p.rows_exiting_on(3) == 0);              // no 14:00Z slice
    check_trade(p, 0, false, 1, 3013.745, 1.28, 1, 3013.745, "Margin call",
                -3.8575935);
    check_trade(p, 1, false, 1, 3013.745, 2.4, 1, 3017.3, "Margin call",
                -15.769254);
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 659.28, 1e-9);
}

// ---------------------------------------------------------------------------
// E. fresh-touch-once (family-E tape, NYSE:F 15): short stop 11.23 x 890
// (capital 10,004.2) placed 08-12 19:45Z; 08-13 13:30Z opens 11.29 = its high
// (O 11.29 H 11.29 L 11.19 C 11.25) and touches. TV: 8 @11.25 on the entry
// bar — the CLOSE is the only post-fill mark above the entry (890 x 11.25 =
// 10012.5 > 9986.4: restore 2.32 -> 2 x 4) — then the carried cascade at the
// rounded highs: 24 @11.33 (13:45Z, high 11.325), 1 @11.45 (16:15Z, sub-lot
// -> one contract), 4 @11.46 (19:00Z, high 11.455), 4 @11.49 (19:30Z, high
// 11.485); 849 remain. The engine printed 32 @11.29 on the entry bar (the
// open, which the short never saw).
// ---------------------------------------------------------------------------
void test_fresh_touch_once_close_is_a_post_fill_mark() {
    std::printf("-- E. fresh-touch-once: 8 @11.25 (the entry bar's close), then 24/1/4/4 --\n");
    Probe p(10004.2, 0.01, 1.0, 0.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_stop("S", false, 11.23, 890.0);
    };
    std::vector<Bar> bars = ford_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 5);
    CHECK(p.margin_call_rows() == p.trade_count());
    check_trade(p, 0, false, 1, 11.23, 8.0, 1, 11.25, "Margin call", -0.16);
    check_trade(p, 1, false, 1, 11.23, 24.0, 2, 11.33, "Margin call", -2.4);
    check_trade(p, 2, false, 1, 11.23, 1.0, 12, 11.45, "Margin call", -0.22);
    check_trade(p, 3, false, 1, 11.23, 4.0, 23, 11.46, "Margin call", -0.92);
    check_trade(p, 4, false, 1, 11.23, 4.0, 25, 11.49, "Margin call", -1.04);
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 849.0, 1e-9);
}

// ---------------------------------------------------------------------------
// F. Synthetic leveraged LONG (margin_long 20, capital 1,000, 40 contracts):
// bar 1 is BULLISH / low-first (O 100 L 92 H 110 C 105). A buy stop at 101
// fills on the L->H leg; the post-fill path is H then C, and 1,160 >= 0.2 x
// 40 x 105 = 840 at the close: no slice, although the pre-fill low 92 breaches
// (equity 640 < 736). The same bar entered by a market order at the open
// (position 0) sees the low: one "Margin call" at the RAW low 92, quantity
// 4 x floor(40 - 680 / (92 x 0.2)) = 12.
// ---------------------------------------------------------------------------
std::vector<Bar> synthetic_long_bars() {
    const BarRow rows[] = {
        {1000LL, 100.0, 100.5, 99.5, 100.0},   // [0] placement
        {2000LL, 100.0, 110.0, 92.0, 105.0},   // [1] entry bar (low first)
        {3000LL, 105.0, 106.0, 104.0, 105.5},  // [2]
        {4000LL, 105.5, 106.0, 105.0, 105.5},  // [3]
    };
    return to_bars(rows);
}

void test_leveraged_long_stop_fill_ignores_pre_fill_low() {
    std::printf("-- F1. leveraged long, buy stop above the open of a low-first bar: no slice at the pre-fill low --\n");
    Probe p(1000.0, 0.01, 1.0, 0.0, /*margin_pct=*/20.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_stop("L", true, 101.0, 40.0);
    };
    std::vector<Bar> bars = synthetic_long_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 0);
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 40.0, 1e-9);
    CHECK_NEAR(p.position_entry_price_, 101.0, 1e-9);
}

void test_leveraged_long_market_open_fill_sees_the_low() {
    std::printf("-- F2. the same bar entered at the open: sliced 12 at the raw low 92 --\n");
    Probe p(1000.0, 0.01, 1.0, 0.0, /*margin_pct=*/20.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_market("L", true, 40.0);
    };
    std::vector<Bar> bars = synthetic_long_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 1);
    check_trade(p, 0, true, 1, 100.0, 12.0, 1, 92.0, "Margin call", -96.0);
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 28.0, 1e-9);
}

// ---------------------------------------------------------------------------
// G. A short 10 @100 (1x margin, capital 1,000; its entry bar never trades
// above the fill) carried into a gap-up open: bar 2 opens 105 (deficit at the
// open: 950 < 1050). With a whole-position
// strategy.close resting from bar 1 the close fills 10 @105 and no "Margin
// call" is booked (the F short tape's 2025-04-23 / 2026-04-08 shape); with a
// HALF close resting, the finding-430 open slice still runs first on the full
// position (restore 10 - 950/105 = 0.95 floors to zero -> the one-contract
// fallback: 1 @105), then the close takes its 5.
// ---------------------------------------------------------------------------
std::vector<Bar> gap_open_bars() {
    const BarRow rows[] = {
        {1000LL, 100.0, 100.5, 99.5, 100.0},    // [0] placement (next open fill)
        {2000LL, 100.0, 100.0, 99.0, 99.5},     // [1] entry bar (no deficit); close resting from here
        {3000LL, 105.0, 106.0, 104.5, 105.5},   // [2] gap-up open: deficit at 105
        {4000LL, 105.5, 105.8, 105.0, 105.2},   // [3]
    };
    return to_bars(rows);
}

void test_pending_whole_close_preempts_open_slice() {
    std::printf("-- G1. whole-position close resting for a gap-open: the close fills, no open slice --\n");
    Probe p(1000.0, 0.01, 1.0, 0.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_market("S", false, 10.0);
        if (bar == 1) e.close_all();
    };
    std::vector<Bar> bars = gap_open_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 1);
    CHECK(p.margin_call_rows() == 0);
    check_trade(p, 0, false, 1, 100.0, 10.0, 2, 105.0, "", -50.0);
    CHECK(p.flat());
}

void test_pending_partial_close_keeps_open_slice() {
    std::printf("-- G2. a HALF close resting for the same open: the open slice still runs first --\n");
    Probe p(1000.0, 0.01, 1.0, 0.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_market("S", false, 10.0);
        if (bar == 1) e.strategy_close("S", "", kNaN, 50.0, false);
    };
    std::vector<Bar> bars = gap_open_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.margin_call_rows() >= 1);
    CHECK(p.trade_count() >= 2);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.exit_comment == "Margin call");
        CHECK(t.exit_bar_index == 2);
        CHECK_NEAR(t.exit_price, 105.0, 1e-9);
        CHECK_NEAR(t.qty, 1.0, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// H. The reversal pair's close is not an unconditional close (round 8).
//
// H1. amandaborgeson06 bias-status NYSE:F@15 (feed 80f404ae85ef), TV's own
// equity 10,000 - 900.69 = 9,099.31 before TV #33: the all-in short placed at
// the 2025-04-30 13:45Z close fills 920 @9.89 at the 14:00Z open (TV #33-35),
// 36 sliced at the entry bar's high 9.94 (TV #33). The buy signal at the
// 19:45Z close issues `strategy.entry(long)` + `strategy.close(short)`; the
// 05-01 13:30Z open gaps to 10.15: the all-in reversal (901 x 10.15 > E_s
// 9,009.11) is declined and its close voided, the open slice 40 @10.15 stands
// (TV #34, restore 10.34 -> 4 x 10), the 844 remainder carries (TV #35 closes
// it at 19:30Z). The over-general guard printed 96 @10.23 (the high) instead.
// ---------------------------------------------------------------------------
static const BarRow kFordAmanda[] = {
    {1746020700000LL, 9.93, 9.965, 9.88, 9.89},     // [0] 04-30 13:45 signal
    {1746021600000LL, 9.89, 9.94, 9.89, 9.94},      // [1] 14:00 entry bar
    {1746022500000LL, 9.935, 9.995, 9.925, 9.985},  // [2] 14:15
    {1746023400000LL, 9.985, 10.01, 9.97, 9.985},   // [3] 14:30
    {1746024300000LL, 9.985, 9.985, 9.915, 9.935},  // [4] 14:45
    {1746025200000LL, 9.935, 9.97, 9.935, 9.955},   // [5] 15:00
    {1746026100000LL, 9.95, 9.96, 9.92, 9.94},      // [6] 15:15
    {1746027000000LL, 9.935, 9.955, 9.92, 9.945},   // [7] 15:30
    {1746027900000LL, 9.945, 9.955, 9.915, 9.93},   // [8] 15:45
    {1746028800000LL, 9.94, 9.95, 9.925, 9.925},    // [9] 16:00
    {1746029700000LL, 9.93, 9.94, 9.9, 9.905},      // [10] 16:15
    {1746030600000LL, 9.9, 9.92, 9.89, 9.905},      // [11] 16:30
    {1746031500000LL, 9.91, 9.93, 9.9, 9.92},       // [12] 16:45
    {1746032400000LL, 9.915, 9.93, 9.865, 9.89},    // [13] 17:00
    {1746033300000LL, 9.885, 9.93, 9.885, 9.93},    // [14] 17:15
    {1746034200000LL, 9.925, 9.95, 9.925, 9.945},   // [15] 17:30
    {1746035100000LL, 9.94, 9.95, 9.92, 9.935},     // [16] 17:45
    {1746036000000LL, 9.935, 9.955, 9.9, 9.935},    // [17] 18:00
    {1746036900000LL, 9.93, 9.95, 9.92, 9.93},      // [18] 18:15
    {1746037800000LL, 9.93, 9.945, 9.905, 9.915},   // [19] 18:30
    {1746038700000LL, 9.915, 9.94, 9.91, 9.915},    // [20] 18:45
    {1746039600000LL, 9.92, 9.93, 9.91, 9.93},      // [21] 19:00
    {1746040500000LL, 9.93, 9.94, 9.91, 9.91},      // [22] 19:15
    {1746041400000LL, 9.915, 9.945, 9.91, 9.945},   // [23] 19:30
    {1746042300000LL, 9.945, 10.04, 9.945, 9.99},   // [24] 19:45 buy signal
    {1746106200000LL, 10.15, 10.23, 10.025, 10.07}, // [25] 05-01 13:30 gap open
    {1746107100000LL, 10.07, 10.12, 10.05, 10.1},   // [26] 13:45
    {1746108000000LL, 10.095, 10.245, 10.095, 10.225}, // [27] 14:00
};

void test_declined_reversal_close_keeps_open_slice_ford() {
    std::printf("-- H1. amandaborgeson06 F@15 2025-05-01 13:30Z: declined reversal, open slice 40 @10.15 stands --\n");
    Probe p(9099.31, 0.01, 1.0, 0.0);
    p.all_in();
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_market("S", false, kNaN);
        if (bar == 24) {
            e.entry_market("L", true, kNaN);
            e.strategy_close("S", "", kNaN, kNaN, false);
        }
    };
    std::vector<Bar> bars = to_bars(kFordAmanda);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.margin_call_rows() == 2);
    check_trade(p, 0, false, 1, 9.89, 36.0, 1, 9.94, "Margin call", -1.8);
    check_trade(p, 1, false, 1, 9.89, 40.0, 25, 10.15, "Margin call", -10.4);
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 844.0, 1e-9);
}

// ---------------------------------------------------------------------------
// H2. hexatrades technical-strength-gauge NASDAQ:AAPL@15 (feed ae2b03d3736f).
// TV's short 6008 @214.03 (TV #252-255) was a long->short reversal sized on
// E_s = realized 1,289,453.22 + the open long marked at tick(close); replayed
// here from flat with an explicit 6008 and the capital 6008 x 214.04 =
// 1,285,952.32 that the placement check (qty x max(tick(close), fill) <=
// equity) admits — every slice below reproduces TV's quantity from it. The
// short fills at the 2025-07-28 17:15Z open; 1 share at the entry bar's high
// 214.04 (TV #252, restore 0.28 -> one contract). The buy signal at the
// 19:45Z close issues the reversal pair; the 07-29 13:30Z open gaps to
// 214.16: the all-in reversal (6009 x 214.16 > E_s 1,286,072.45) is declined,
// its close voided, and the open slice 24 @214.16 (TV #253, restore 6.01 ->
// 4 x 6) is followed by the extreme slice 72 @214.81 (TV #254, restore 18.28
// -> 4 x 18) on the same bar; 5911 carry (TV #255). The over-general guard
// printed 168 @214.81 alone.
// ---------------------------------------------------------------------------
static const BarRow kAaplHexa[] = {
    {1753722000000LL, 214.335, 214.44, 213.98, 214.04},  // [0] 07-28 17:00 signal
    {1753722900000LL, 214.03, 214.04, 213.42, 213.645},  // [1] 17:15 entry bar
    {1753723800000LL, 213.64, 213.95, 213.63, 213.84},   // [2] 17:30
    {1753724700000LL, 213.83, 214.02, 213.73, 213.74},   // [3] 17:45
    {1753725600000LL, 213.74, 213.93, 213.68, 213.9},    // [4] 18:00
    {1753726500000LL, 213.91, 213.98, 213.7, 213.81},    // [5] 18:15
    {1753727400000LL, 213.825, 213.83, 213.45, 213.46},  // [6] 18:30
    {1753728300000LL, 213.47, 213.6, 213.06, 213.15},    // [7] 18:45
    {1753729200000LL, 213.14, 213.32, 213.06, 213.32},   // [8] 19:00
    {1753730100000LL, 213.32, 213.43, 213.085, 213.43},  // [9] 19:15
    {1753731000000LL, 213.42, 213.86, 213.325, 213.85},  // [10] 19:30
    {1753731900000LL, 213.84, 214.04, 213.66, 214.01},   // [11] 19:45 buy signal
    {1753795800000LL, 214.16, 214.81, 213.76, 213.89},   // [12] 07-29 13:30 gap open
    {1753796700000LL, 213.89, 213.89, 211.51, 212.57},   // [13] 13:45
};

void test_declined_reversal_close_keeps_open_and_extreme_slices_aapl() {
    std::printf("-- H2. hexatrades AAPL@15 2025-07-29 13:30Z: declined reversal, 24 @214.16 then 72 @214.81 --\n");
    Probe p(1285952.32, 0.01, 1.0, 0.0);
    p.all_in();
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_market("S", false, 6008.0);
        if (bar == 11) {
            e.entry_market("L", true, kNaN);
            e.strategy_close("S", "", kNaN, kNaN, false);
        }
    };
    std::vector<Bar> bars = to_bars(kAaplHexa);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 3);
    CHECK(p.margin_call_rows() == 3);
    check_trade(p, 0, false, 1, 214.03, 1.0, 1, 214.04, "Margin call", -0.01);
    check_trade(p, 1, false, 1, 214.03, 24.0, 12, 214.16, "Margin call", -3.12);
    check_trade(p, 2, false, 1, 214.03, 72.0, 12, 214.81, "Margin call", -56.16);
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 5911.0, 1e-9);
}

}  // namespace

int main() {
    std::printf("--- entry_bar_margin_path (round 7 family L) ---\n");
    test_mcpath_a_bearish_stop_fill_no_entry_bar_slice();
    test_mcpath_b_bullish_open_fill_same_bar_slice();
    test_asian_box_0401_no_phantom_slice();
    test_mdfe3757_0408_market_open_fill_same_bar_cascade();
    test_fresh_touch_once_close_is_a_post_fill_mark();
    test_leveraged_long_stop_fill_ignores_pre_fill_low();
    test_leveraged_long_market_open_fill_sees_the_low();
    test_pending_whole_close_preempts_open_slice();
    test_pending_partial_close_keeps_open_slice();
    test_declined_reversal_close_keeps_open_slice_ford();
    test_declined_reversal_close_keeps_open_and_extreme_slices_aapl();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
