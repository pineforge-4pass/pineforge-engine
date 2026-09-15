#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
/*
 * test_famx_declined_reversal_trail_leg.cpp — round 9 family X
 * (stevenygabbyperez-fast-scalper-with-stops on AAPL/XAUUSD/F/NIFTY@15):
 * finding-311's KILL is LEG-scoped, and a pending reversal is judged at the
 * open before the position's own gapped bracket.
 *
 * TradingView is ground truth: `lab tv` tapes scratchpad/r9/famX/pins
 * (campaign note log-20260905t173310z-c6f35398), time-gated scripts with
 * default_qty_type=percent_of_equity 100 and the probe's own
 * strategy.exit(stop=close*0.99|1.01, trail_points=close*0.02/mintick)
 * issued on the signal bar. Registry feed bars via `lab bars`.
 *
 *   NASDAQ:AAPL 15m  long signal 2025-07-31 16:30Z (fill 16:45Z 4778
 *   @209.27, activation 209.27 + 419t = 213.46), reversal short signal
 *   17:30Z DECLINED at the 17:45Z open (Q x open 997,169 > E_s 997,085):
 *     famx-aapl-trail-declrev      'Exit Long' 08-01 13:30Z @213.46 == ctrl
 *     famx-aapl-trailoff1-declrev  trail_offset=1: 08-01 13:30Z @213.57 == ctrl
 *     famx-aapl-stoptrail-declrev  stop=208.0 + trail in ONE call: the stop
 *                                  breached 18:00Z/19:45Z/08-01 never fills,
 *                                  the trail prints @213.46 (ctrl: stop @208
 *                                  at 18:00Z)
 *     famx-aapl-stop-laterbar      stop=208.0 only: never fills (held through
 *                                  the 08-01 crash; ctrl 18:00Z @208.00)
 *     famx-aapl-limit-declrev      limit=close*1.02: never fills (ctrl 08-01
 *                                  13:30Z @213.44)
 *     famx-aapl-stop-noexit-declrev the reversal issued WITHOUT its own exit:
 *                                  the stop still dies (the decline kills)
 *   OANDA:XAUUSD 15m (mintick 0.001, lot 0.01) short signal 2026-02-12
 *   14:30Z, reversal long signal 15:30Z declined with capital 1,000,010:
 *     famx-xau-declrev-c1000010    'Margin call' 1.6 @5061.6 on the entry
 *                                  bar, then 'Exit Short' 16:00Z @4955.207
 *                                  (= entry - 101128t, the probe's row the
 *                                  engine slid to the 16:15Z open 4951.245)
 *   NYSE:F 15m long signal 2025-06-06 13:30Z, reversal short 18:15Z declined:
 *     famx-f-trail-declrev         'Exit Long' 06-09 13:30Z @10.40 (the bar's
 *                                  high touches the 21t activation)
 *   NSE:NIFTY 15m short signal 2025-04-11 08:45Z (43 @22771.25), long signal
 *   09:45Z, next bar = 04-15 03:45Z gap open 23343.85 through the 22997.5 stop:
 *     famx-nifty-gap-declrev       'Margin call' 4 + 'Exit Short' 39
 *                                  @23343.85, NO long (43 x 23343.85 >
 *                                  E_s 996,850): the reversal is judged first
 *                                  and declined; the engine used to fill the
 *                                  older stop first and admit the long from
 *                                  flat with a 4-lot entry trim
 *     famx-nifty-gap-admit90       default_qty_value=90: the flip fills at the
 *                                  open (39 out, 39 long in), no stop, no
 *                                  margin call
 */

#include <cmath>
#include <cstdio>
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

Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

static const std::vector<Bar> kAapl = {
    mk(1753979400000LL, 208.95, 209.32, 208.86, 209.25),   // 2025-07-31 16:30Z
    mk(1753980300000LL, 209.27, 209.37, 208.95, 209.07),   // 2025-07-31 16:45Z
    mk(1753981200000LL, 209.09, 209.835, 208.75, 209.65),   // 2025-07-31 17:00Z
    mk(1753982100000LL, 209.64, 209.7, 208.5, 208.67),   // 2025-07-31 17:15Z
    mk(1753983000000LL, 208.65, 208.88, 208.33, 208.66),   // 2025-07-31 17:30Z
    mk(1753983900000LL, 208.7, 208.91, 208.14, 208.32),   // 2025-07-31 17:45Z
    mk(1753984800000LL, 208.3, 208.57, 207.92, 208.03),   // 2025-07-31 18:00Z
    mk(1753985700000LL, 208.02, 208.77, 207.59, 208.61),   // 2025-07-31 18:15Z
    mk(1753986600000LL, 208.61, 209.03, 208.55, 208.92),   // 2025-07-31 18:30Z
    mk(1753987500000LL, 208.93, 209.25, 208.7, 208.94),   // 2025-07-31 18:45Z
    mk(1753988400000LL, 209, 209.38, 208.73, 208.78),   // 2025-07-31 19:00Z
    mk(1753989300000LL, 208.78, 209.06, 208.49, 208.57),   // 2025-07-31 19:15Z
    mk(1753990200000LL, 208.59, 208.84, 208.395, 208.67),   // 2025-07-31 19:30Z
    mk(1753991100000LL, 208.68, 208.785, 207.18, 207.5),   // 2025-07-31 19:45Z
    mk(1754055000000LL, 210.83, 213.58, 208.18, 208.2),   // 2025-08-01 13:30Z
    mk(1754055900000LL, 208.22, 208.46, 206.27, 207.12),   // 2025-08-01 13:45Z
    mk(1754056800000LL, 207.04, 207.46, 205.52, 206.35),   // 2025-08-01 14:00Z
    mk(1754057700000LL, 206.35, 206.845, 204.35, 205.36),   // 2025-08-01 14:15Z
    mk(1754058600000LL, 205.38, 205.72, 203.65, 203.74),   // 2025-08-01 14:30Z
    mk(1754059500000LL, 203.78, 204.43, 203.75, 204.02),   // 2025-08-01 14:45Z
    mk(1754060400000LL, 204.05, 204.58, 203.69, 204.055),   // 2025-08-01 15:00Z
};

static const std::vector<Bar> kXau = {
    mk(1770906600000LL, 5060.11, 5071.815, 5053.915, 5056.385),   // 2026-02-12 14:30Z
    mk(1770907500000LL, 5056.335, 5061.6, 5039.7, 5044.625),   // 2026-02-12 14:45Z
    mk(1770908400000LL, 5044.63, 5066.08, 5039.815, 5064.99),   // 2026-02-12 15:00Z
    mk(1770909300000LL, 5064.975, 5069.955, 5062.115, 5066.08),   // 2026-02-12 15:15Z
    mk(1770910200000LL, 5065.945, 5072.375, 5065.275, 5070.77),   // 2026-02-12 15:30Z
    mk(1770911100000LL, 5070.845, 5074.87, 5060.09, 5060.57),   // 2026-02-12 15:45Z
    mk(1770912000000LL, 5060.585, 5068.36, 4948.28, 4951.365),   // 2026-02-12 16:00Z
    mk(1770912900000LL, 4951.245, 4966.545, 4878.5, 4897.315),   // 2026-02-12 16:15Z
    mk(1770913800000LL, 4897.235, 4969.515, 4894.22, 4953.43),   // 2026-02-12 16:30Z
};

static const std::vector<Bar> kFord = {
    mk(1749216600000LL, 10.16, 10.21, 10.15, 10.185),   // 2025-06-06 13:30Z
    mk(1749217500000LL, 10.185, 10.2, 10.16, 10.19),   // 2025-06-06 13:45Z
    mk(1749218400000LL, 10.185, 10.21, 10.175, 10.205),   // 2025-06-06 14:00Z
    mk(1749219300000LL, 10.205, 10.23, 10.2, 10.21),   // 2025-06-06 14:15Z
    mk(1749220200000LL, 10.215, 10.23, 10.195, 10.205),   // 2025-06-06 14:30Z
    mk(1749221100000LL, 10.205, 10.26, 10.195, 10.22),   // 2025-06-06 14:45Z
    mk(1749222000000LL, 10.215, 10.33, 10.21, 10.285),   // 2025-06-06 15:00Z
    mk(1749222900000LL, 10.28, 10.29, 10.225, 10.29),   // 2025-06-06 15:15Z
    mk(1749223800000LL, 10.295, 10.34, 10.29, 10.335),   // 2025-06-06 15:30Z
    mk(1749224700000LL, 10.33, 10.35, 10.32, 10.325),   // 2025-06-06 15:45Z
    mk(1749225600000LL, 10.325, 10.345, 10.29, 10.295),   // 2025-06-06 16:00Z
    mk(1749226500000LL, 10.295, 10.31, 10.295, 10.305),   // 2025-06-06 16:15Z
    mk(1749227400000LL, 10.31, 10.325, 10.275, 10.285),   // 2025-06-06 16:30Z
    mk(1749228300000LL, 10.28, 10.29, 10.265, 10.27),   // 2025-06-06 16:45Z
    mk(1749229200000LL, 10.275, 10.28, 10.265, 10.265),   // 2025-06-06 17:00Z
    mk(1749230100000LL, 10.265, 10.28, 10.26, 10.265),   // 2025-06-06 17:15Z
    mk(1749231000000LL, 10.26, 10.27, 10.26, 10.265),   // 2025-06-06 17:30Z
    mk(1749231900000LL, 10.265, 10.275, 10.255, 10.26),   // 2025-06-06 17:45Z
    mk(1749232800000LL, 10.26, 10.275, 10.255, 10.255),   // 2025-06-06 18:00Z
    mk(1749233700000LL, 10.26, 10.265, 10.24, 10.24),   // 2025-06-06 18:15Z
    mk(1749234600000LL, 10.25, 10.26, 10.25, 10.255),   // 2025-06-06 18:30Z
    mk(1749235500000LL, 10.255, 10.255, 10.24, 10.24),   // 2025-06-06 18:45Z
    mk(1749236400000LL, 10.24, 10.26, 10.24, 10.255),   // 2025-06-06 19:00Z
    mk(1749237300000LL, 10.25, 10.255, 10.24, 10.24),   // 2025-06-06 19:15Z
    mk(1749238200000LL, 10.245, 10.25, 10.23, 10.24),   // 2025-06-06 19:30Z
    mk(1749239100000LL, 10.23, 10.26, 10.22, 10.255),   // 2025-06-06 19:45Z
    mk(1749475800000LL, 10.3, 10.4, 10.29, 10.385),   // 2025-06-09 13:30Z
    mk(1749476700000LL, 10.39, 10.43, 10.36, 10.425),   // 2025-06-09 13:45Z
};

static const std::vector<Bar> kNifty = {
    mk(1744361100000LL, 22786.15, 22797.1, 22762, 22769.8),   // 2025-04-11 08:45Z
    mk(1744362000000LL, 22771.25, 22805.8, 22770.85, 22804.75),   // 2025-04-11 09:00Z
    mk(1744362900000LL, 22806.2, 22834.75, 22804.15, 22827),   // 2025-04-11 09:15Z
    mk(1744363800000LL, 22826.65, 22840.15, 22821.85, 22830.35),   // 2025-04-11 09:30Z
    mk(1744364700000LL, 22831.05, 22855.55, 22810.95, 22844.5),   // 2025-04-11 09:45Z
    mk(1744688700000LL, 23343.85, 23346.8, 23207, 23297.65),   // 2025-04-15 03:45Z
    mk(1744689600000LL, 23297.9, 23318.55, 23264.65, 23310.95),   // 2025-04-15 04:00Z
};

// One signal: strategy.entry + (optionally) the probe's strategy.exit, with
// the exit's legs chosen per fixture. Prices are computed from the signal
// bar's close exactly as the script does.
struct Signal {
    int bar;
    bool is_long;
    bool with_exit;
    // legs: NaN = omitted. stop_mult / limit_mult scale the signal close;
    // stop_abs overrides with an absolute price; trail = close*0.02/mintick.
    double stop_mult;
    double stop_abs;
    double limit_mult;
    bool trail;
    double trail_offset;
};

class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, double mintick, double qty_step, double pct = 100.0) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = pct;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = false;
        syminfo_mintick_ = mintick;
        qty_step_ = qty_step;
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
            if (!s.with_exit) continue;
            double stop = kNaN;
            if (!std::isnan(s.stop_abs)) stop = s.stop_abs;
            else if (!std::isnan(s.stop_mult)) stop = bar.close * s.stop_mult;
            const double limit = std::isnan(s.limit_mult)
                ? kNaN : bar.close * s.limit_mult;
            const double tp = s.trail
                ? bar.close * 0.02 / syminfo_mintick_ : kNaN;
            strategy_exit("Exit " + id, id, limit, stop, tp, s.trail_offset,
                          kNaN, 100.0, "");
        }
    }
    double x_price(int i) const { return closed_trade_exit_price(i); }
    double e_price(int i) const { return closed_trade_entry_price(i); }
    double t_size(int i) const { return closed_trade_size(i); }
    int x_bar(int i) const { return closed_trade_exit_bar_index(i); }
    int e_bar(int i) const { return closed_trade_entry_bar_index(i); }
    std::string x_comment(int i) const { return closed_trade_exit_comment(i); }
    std::string x_id(int i) const { return closed_trade_exit_id(i); }
    bool is_long_trade(int i) const { return closed_trade_entry_id(i) == "Long"; }
    double position() const { return signed_position_size(); }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::last_error_;
};

Signal probe_signal(int bar, bool is_long) {
    return Signal{bar, is_long, true, is_long ? 0.99 : 1.01, kNaN, kNaN,
                  true, kNaN};
}

bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// ── AAPL: the trail leg lives, the stop / limit legs die ─────────────────

const int kAaplLongSig = 0;     // 07-31 16:30Z
const int kAaplShortSig = 4;    // 07-31 17:30Z (declined at 17:45Z)
const int kAaplStopBar = 6;     // 18:00Z, low 207.92 < 208.0
const int kAaplTrailBar = 14;   // 08-01 13:30Z, high 213.58

void test_aapl_trail_leg_lives_after_declined_reversal() {
    std::printf("test_aapl_trail_leg_lives_after_declined_reversal\n");
    for (int with_reversal = 0; with_reversal < 2; ++with_reversal) {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {probe_signal(kAaplLongSig, true)};
        if (with_reversal) p.signals.push_back(probe_signal(kAaplShortSig, false));
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.last_error_.empty());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() >= 1) {
            CHECK(near(p.t_size(0), 4778.0));
            CHECK(near(p.e_price(0), 209.27));
            CHECK(near(p.x_price(0), 213.46));      // TV, both tapes
            CHECK(p.x_bar(0) == kAaplTrailBar);
        }
        CHECK(p.position_side_ == PositionSide::FLAT);
    }
}

void test_aapl_offset_trail_lives_after_declined_reversal() {
    std::printf("test_aapl_offset_trail_lives_after_declined_reversal\n");
    for (int with_reversal = 0; with_reversal < 2; ++with_reversal) {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {Signal{kAaplLongSig, true, true, kNaN, kNaN, kNaN, true, 1.0}};
        if (with_reversal) p.signals.push_back(probe_signal(kAaplShortSig, false));
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.last_error_.empty());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() >= 1) {
            CHECK(near(p.x_price(0), 213.57));      // peak 213.58 - 1t
            CHECK(p.x_bar(0) == kAaplTrailBar);
        }
    }
}

void test_aapl_stop_leg_dies_trail_leg_lives_in_one_call() {
    std::printf("test_aapl_stop_leg_dies_trail_leg_lives_in_one_call\n");
    // Control: no reversal -> the 208.0 stop fills at 18:00Z.
    {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {Signal{kAaplLongSig, true, true, kNaN, 208.0, kNaN, true, kNaN}};
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() >= 1) {
            CHECK(near(p.x_price(0), 208.0));
            CHECK(p.x_bar(0) == kAaplStopBar);
        }
    }
    // Declined reversal: the stop breached at 18:00Z, 19:45Z and through the
    // 08-01 crash never fills; the trail leg prints @213.46.
    {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {Signal{kAaplLongSig, true, true, kNaN, 208.0, kNaN, true, kNaN},
                     probe_signal(kAaplShortSig, false)};
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.last_error_.empty());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() >= 1) {
            CHECK(near(p.x_price(0), 213.46));
            CHECK(p.x_bar(0) == kAaplTrailBar);
        }
    }
}

void test_aapl_stop_only_bracket_stays_dead_for_days() {
    std::printf("test_aapl_stop_only_bracket_stays_dead_for_days\n");
    // With its own strategy.exit on the reversal, and without one: the
    // decline itself kills the standing stop; the long is held through
    // every later breach (08-05 range-end row on the tape).
    for (int reversal_exit = 0; reversal_exit < 2; ++reversal_exit) {
        Probe p(1'000'000.0, 0.01, 1.0);
        Signal rev = probe_signal(kAaplShortSig, false);
        rev.with_exit = reversal_exit != 0;
        p.signals = {Signal{kAaplLongSig, true, true, kNaN, 208.0, kNaN, false, kNaN}, rev};
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.last_error_.empty());
        CHECK(p.trade_count() == 0);
        CHECK(p.position_side_ == PositionSide::LONG);
        CHECK_NEAR(p.position_qty_, 4778.0, 1e-9);
    }
}

void test_aapl_limit_leg_dies() {
    std::printf("test_aapl_limit_leg_dies\n");
    {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {Signal{kAaplLongSig, true, true, kNaN, kNaN, 1.02, false, kNaN}};
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() >= 1) {
            CHECK(near(p.x_price(0), 213.44));      // TV ctrl: 213.435 -> 213.44
            CHECK(p.x_bar(0) == kAaplTrailBar);
        }
    }
    {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {Signal{kAaplLongSig, true, true, kNaN, kNaN, 1.02, false, kNaN},
                     probe_signal(kAaplShortSig, false)};
        p.run(kAapl.data(), (int)kAapl.size());
        CHECK(p.trade_count() == 0);
        CHECK(p.position_side_ == PositionSide::LONG);
    }
}

// ── XAUUSD: the probe's own row after a decline ──────────────────────────

void test_xau_trail_fires_at_activation_after_declined_reversal() {
    std::printf("test_xau_trail_fires_at_activation_after_declined_reversal\n");
    Probe p(1'000'010.0, 0.001, 0.01);
    p.signals = {probe_signal(0, false),
                 probe_signal(4, true)};
    p.run(kXau.data(), (int)kXau.size());
    CHECK(p.last_error_.empty());
    // TV: 'Margin call' 1.6 @5061.6 on the entry bar, 'Exit Short' 196.17
    // @4955.207 on 16:00Z (bar 6); no Long row.
    CHECK(p.trade_count() == 2);
    double closed_short = 0.0;
    bool long_row = false;
    for (int i = 0; i < p.trade_count(); ++i) {
        if (p.is_long_trade(i)) long_row = true;
        else closed_short += p.t_size(i);
    }
    CHECK(!long_row);
    CHECK_NEAR(closed_short, 197.77, 1e-6);
    if (p.trade_count() == 2) {
        CHECK_NEAR(p.t_size(0), 1.6, 1e-9);
        CHECK(near(p.x_price(0), 5061.6));
        CHECK_NEAR(p.t_size(1), 196.17, 1e-9);
        CHECK(near(p.x_price(1), 4955.207, 1e-9));
        CHECK(p.x_bar(1) == 6);
    }
    CHECK(p.position_side_ == PositionSide::FLAT);
}

// ── NYSE:F: activation touched by the high after a decline ───────────────

void test_ford_trail_touch_after_declined_reversal() {
    std::printf("test_ford_trail_touch_after_declined_reversal\n");
    for (int with_reversal = 0; with_reversal < 2; ++with_reversal) {
        Probe p(1'000'000.0, 0.01, 1.0);
        p.signals = {probe_signal(0, true)};
        if (with_reversal) p.signals.push_back(probe_signal(19, false));
        p.run(kFord.data(), (int)kFord.size());
        CHECK(p.last_error_.empty());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() >= 1) {
            CHECK(p.is_long_trade(0));
            CHECK(near(p.e_price(0), 10.19));
            CHECK(near(p.x_price(0), 10.40));
            CHECK(p.x_bar(0) == 26);
        }
        CHECK(p.position_side_ == PositionSide::FLAT);
    }
}

// ── NIFTY: the reversal is judged before the gapped stop ─────────────────

const int kNiftyShortSig = 0;
const int kNiftyLongSig = 4;
const int kNiftyGapBar = 5;

void test_nifty_declined_reversal_at_gap_open_leaves_no_long() {
    std::printf("test_nifty_declined_reversal_at_gap_open_leaves_no_long\n");
    Probe p(1'000'000.0, 0.05, 1.0);
    p.signals = {probe_signal(kNiftyShortSig, false), probe_signal(kNiftyLongSig, true)};
    p.run(kNifty.data(), (int)kNifty.size());
    CHECK(p.last_error_.empty());
    // TV: 'Margin call' 4 + 'Exit Short' 39 @23343.85 on the gap bar; no long.
    double closed_short = 0.0;
    bool long_row = false;
    for (int i = 0; i < p.trade_count(); ++i) {
        if (p.is_long_trade(i)) long_row = true;
        else {
            closed_short += p.t_size(i);
            CHECK(near(p.x_price(i), 23343.85));
            CHECK(p.x_bar(i) == kNiftyGapBar);
        }
    }
    CHECK(!long_row);
    CHECK_NEAR(closed_short, 43.0, 1e-9);
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK_NEAR(p.t_size(0), 4.0, 1e-9);      // 'Margin call' 4
        CHECK_NEAR(p.t_size(1), 39.0, 1e-9);     // 'Exit Short' 39
    }
    CHECK(p.position_side_ == PositionSide::FLAT);
}

void test_nifty_admitted_reversal_flips_at_gap_open() {
    std::printf("test_nifty_admitted_reversal_flips_at_gap_open\n");
    Probe p(1'000'000.0, 0.05, 1.0, /*pct=*/90.0);
    p.signals = {probe_signal(kNiftyShortSig, false), probe_signal(kNiftyLongSig, true)};
    p.run(kNifty.data(), (int)kNifty.size());
    CHECK(p.last_error_.empty());
    // TV: the short 39 closes 'Long' @23343.85 and a long 39 opens there.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() >= 1) {
        CHECK(!p.is_long_trade(0));
        CHECK_NEAR(p.t_size(0), 39.0, 1e-9);
        CHECK(near(p.x_price(0), 23343.85));
        CHECK(p.x_bar(0) == kNiftyGapBar);
    }
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 39.0, 1e-9);
}

}  // namespace

int main() {
    test_aapl_trail_leg_lives_after_declined_reversal();
    test_aapl_offset_trail_lives_after_declined_reversal();
    test_aapl_stop_leg_dies_trail_leg_lives_in_one_call();
    test_aapl_stop_only_bracket_stays_dead_for_days();
    test_aapl_limit_leg_dies();
    test_xau_trail_fires_at_activation_after_declined_reversal();
    test_ford_trail_touch_after_declined_reversal();
    test_nifty_declined_reversal_at_gap_open_leaves_no_long();
    test_nifty_admitted_reversal_flips_at_gap_open();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
