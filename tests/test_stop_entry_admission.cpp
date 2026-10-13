/*
 * test_stop_entry_admission.cpp — stop-entry admission under margin simulation.
 * Pinned against TradingView behaviour (recorded tapes are kept outside this
 * repository). The cent-tick, half-tick and fractional-lot paths below are
 * independently hand-designed synthetic data with timestamps 1000 + k * period.
 *
 * Placement costs lot-floored qty at tick-half-up(close), including point value,
 * FX and margin percentage, against script-visible equity. A flattening bar
 * uses post-exit equity; a reversal costs only the new side, and a position
 * already closed on the bar counts nothing.
 * Rejected placements are dropped; rejected same-id re-issues cancel resting
 * orders. Accepted resting orders are not re-checked at subsequent closes.
 * Fill admission uses the same quantity at the level for an intrabar touch or
 * at the rounded open for a gap; an unaffordable fill is dropped without trim.
 * Default percent sizing uses the snapped level and the same placement gate.
 * The crypto regression at the end retains its existing, out-of-scope data.
 */

#include <cmath>
#include <cstdio>
#include <functional>
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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

constexpr int64_t kMin15 = 15LL * 60LL * 1000LL;

// Stable indices preserve the source call order and every existing bar check.
enum FBar {
    BSeed = 0,
    BSeedFill = 1, BFlatten = 2, BGap = 3, BAffordable = 4,
    BTouch = 5, BHold = 6, BHoldNext = 7, BClose = 26,
    BExit = 27, BGapTouch = 28, BTail0 = 29, BTail1 = 30,
    BTail2 = 31, BRawCall = 32, BRawFill = 33, BTailEnd = 34,
};

std::vector<Bar> admission_bars() {
    std::vector<Bar> b;
    // All indices use one invented epoch; the last block needs no calendar.
    auto push = [&](double o, double h, double l, double c) {
        b.push_back(mk(1000LL + static_cast<int64_t>(b.size()) * kMin15,
                       o, h, l, c));
    };
    push(20.00, 20.01, 19.99, 20.00);       // seed / placement
    push(20.02, 20.03, 19.80, 20.02);       // seed fill; short touches 19.81
    push(20.015, 20.215, 20.01, 20.215);    // stop-out, half-up close 20.22
    push(20.215, 20.25, 20.04, 20.22);      // rounded gap open; exact short touch
    push(20.225, 20.23, 20.015, 20.026);    // market fill 20.23; close 20.03
    push(20.026, 20.08, 20.02, 20.06);      // long touches 20.04
    // Every intervening close costs more than 2001.50 for the gap probe's
    // 100 shares. Thus no intermediate re-issue can revive a rejected gap.
    for (int i = 6; i <= BClose; ++i) {
        const double c = 20.08 + (i % 3) * 0.01;
        push(20.08, 20.12, 20.06, c);
    }
    push(20.01, 20.02, 19.98, 19.985);      // exit; gap re-issue affordable
    push(19.985, 20.03, 19.98, 20.015);     // open 19.99, intrabar touch 20.00
    for (int i = BTail0; i <= BTail2; ++i) {
        const double c = 20.02 + (i % 2) * 0.01;
        push(20.02, 20.05, 20.00, c);
    }
    push(20.02, 20.04, 20.00, 20.015);      // floored-quantity call
    push(20.01, 20.04, 20.00, 20.02);       // fill at 20.01
    push(20.02, 20.04, 20.00, 20.02);
    return b;
}

enum HalfBar {
    HSignal = 0, HRejected1 = 1, HRejected2 = 2, HRejected3 = 3, HRejected4 = 4, HAccepted = 5,
    HFill = 6, HTail = 7, HEnd = 8,
};

std::vector<Bar> half_tick_bars() {
    // A short at 30.00 cannot be admitted at closes rounding to 30.01 or
    // above. Bar 5 first closes at 30.00, and bar 6 opens exactly at its stop.
    const double d[][4] = {
        {30.02, 30.03, 29.99, 30.005},
        {30.00, 30.06, 29.99, 30.05},
        {30.05, 30.09, 30.04, 30.08},
        {30.08, 30.09, 30.01, 30.02},
        {30.02, 30.04, 30.00, 30.015},
        {30.02, 30.03, 29.99, 30.00},
        {30.00, 30.01, 29.99, 30.00},
        {30.00, 30.02, 29.98, 29.99},
        {29.99, 30.01, 29.98, 30.00},
    };
    std::vector<Bar> b;
    for (int i = 0; i < 9; ++i) {
        b.push_back(mk(1000LL + i * kMin15, d[i][0], d[i][1], d[i][2], d[i][3]));
    }
    return b;
}

enum XBar {
    GSeed = 0, GEntry = 1, GFlatten = 2, GAccepted1 = 3, GAccepted2 = 4,
    GRejected = 5, GTouch = 6, GRetryTouch = 7, GTail = 8, GEnd = 9,
};

std::vector<Bar> fractional_lot_bars() {
    // Bar 2 stops the seed at 200.00. Bars 3..5 stay strictly above that
    // stop; bar 6 touches from an expensive open, and bar 7 repeats the touch.
    const double d[][4] = {
        {201.00, 201.50, 200.75, 201.10},
        {201.25, 201.50, 200.25, 200.50},
        {200.50, 200.75, 199.995, 200.05},
        {200.05, 200.10, 200.025, 200.06},
        {200.06, 200.10, 200.025, 200.07},
        {200.07, 200.20, 200.025, 200.15},
        {200.175, 200.20, 199.95, 200.02},
        {200.025, 200.05, 199.95, 199.975},
        {199.975, 200.05, 199.90, 199.95},
        {199.95, 200.05, 199.90, 200.00},
    };
    std::vector<Bar> b;
    for (int i = 0; i < 10; ++i) {
        b.push_back(mk(1000LL + i * kMin15, d[i][0], d[i][1], d[i][2], d[i][3]));
    }
    return b;
}

class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, double margin, double mintick, double lot) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = mintick;
        syminfo_mintick_ = mintick;
        qty_step_ = lot;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin;
        margin_short_ = margin;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(false);
    }
    // The Pine body, called with bar_index_ on every bar.
    std::function<void(Probe&, int)> script;
    void on_source_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }

    // strategy.equity as the script reads it on this bar.
    double equity() const {
        return current_equity() + open_profit(current_bar_.close);
    }
    double position_size() const { return signed_position_size(); }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool pending(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id == id) return true;
        return false;
    }
    size_t pending_count() const { return pending_orders_.size(); }
    // Placement verdict per bar for id: true = a PendingOrder exists right
    // after the strategy.entry call on that bar.
    std::vector<std::pair<int, bool>> placements;
    void entry_stop(const std::string& id, bool is_long, double level,
                    double qty, const std::string& comment = "") {
        strategy_entry(id, is_long, kNaN, level, qty, comment);
        placements.emplace_back(bar_index_, pending(id));
    }
    bool placed_on(int bar) const {
        for (const auto& p : placements) if (p.first == bar) return p.second;
        return false;
    }
    bool called_on(int bar) const {
        for (const auto& p : placements) if (p.first == bar) return true;
        return false;
    }
    void enable_margin_call() { set_margin_call_enabled(true); }
    void use_default_percent(double pct) {
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = pct;
    }
    double close_now() const { return current_bar_.close; }
    using pineforge::source::PineStrategyHost::strategy_entry;
    using pineforge::source::PineStrategyHost::strategy_exit;
    using pineforge::source::PineStrategyHost::strategy_close;
    using pineforge::source::PineStrategyHost::strategy_close_all;
    const std::vector<PyramidEntry>& pyramid_entries() const {
        return pyramid_entries_;
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

// Seed short: 80 at bar 1 open 20.02; bar 2 touches stop 20.04.
// Realized equity = capital + 80 * (20.02 - 20.04) = capital - 1.60.
void phase_a_short(Probe& p, int bar) {
    if (bar == BSeed) {
        p.strategy_entry("S", false, kNaN, kNaN, 80.0);
        p.strategy_exit("XS", "S", kNaN, 20.04);
    }
}

void check_phase_a(const Probe& p) {
    CHECK(p.trade_count() >= 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(!t.is_long);
        CHECK(t.entry_bar_index == BSeedFill);
        CHECK_NEAR(t.entry_price, 20.02, 1e-9);
        CHECK(t.exit_bar_index == BFlatten);
        CHECK_NEAR(t.exit_price, 20.04, 1e-9);
        CHECK_NEAR(t.qty, 80.0, 1e-9);
        CHECK_NEAR(t.pnl, -1.6, 1e-9); // 80 * (20.02 - 20.04)
    }
}

// Re-issue while flat; script equity sizes each call. The close on bar 26
// executes at bar 27 open 20.01. Rejected calls must leave no resting order.
void reissue_long_stop(Probe& p, int bar, double L, double frac, bool floor_qty,
                       int from = BFlatten) {
    if (bar >= from && bar <= BClose - 1 && p.flat()) {
        const double raw = frac * p.equity() / L;
        p.entry_stop("L", true, L, floor_qty ? std::floor(raw) : raw,
                     "p" + std::to_string(bar));
    }
    if (bar == BClose && p.position_size() > 0) {
        p.strategy_close("L", "eod");
    }
}

// Post-exit E = 2185.10 - 1.60 = 2183.50; floor(E / 20.04) = 108.
// Raw close cost 108 * 20.215 = 2183.22 is affordable, but rounded close cost
// 108 * 20.22 = 2183.76 rejects on bars 2/3. Bar 4 costs 2163.24 and accepts;
// bar 5 touches 20.04. The raw-quantity variant also lot-floors to 108.
void test_flatten_stop_floor() {
    std::printf("-- synthetic flatten stop floor --\n");
    for (bool floor_qty : {true, false}) {
        Probe p(2185.1, 100.0, 0.01, 1.0);
        p.script = [&](Probe& e, int bar) {
            phase_a_short(e, bar);
            reissue_long_stop(e, bar, 20.04, 1.0, floor_qty);
        };
        std::vector<Bar> bars = admission_bars();
        p.run(bars.data(), (int)bars.size());
        check_phase_a(p);
        CHECK(!p.placed_on(BFlatten));
        CHECK(!p.placed_on(BGap));
        CHECK(p.placed_on(BAffordable));
        CHECK(p.trade_count() == 2);
        if (p.trade_count() == 2) {
            const Trade& t = p.get_trade(1);
            CHECK(t.is_long);
            CHECK(t.entry_bar_index == BTouch);
            CHECK_NEAR(t.entry_price, 20.04, 1e-9);
            CHECK_NEAR(t.qty, 108.0, 1e-9); // floor(2183.50 / 20.04)
            CHECK(t.entry_comment == "p" + std::to_string((int)BAffordable));
            CHECK(t.exit_bar_index == BExit);
            CHECK_NEAR(t.exit_price, 20.01, 1e-9);
            CHECK_NEAR(t.pnl, -3.24, 1e-6); // 108 * (20.01 - 20.04)
        }
    }
}

// Starting flat at E = 2183.50 must give the same placement and fill bars
// as the post-exit scenario; flattening itself is not an admission exemption.
void test_fresh_stop_floor() {
    std::printf("-- synthetic fresh stop floor --\n");
    Probe p(2183.5, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        reissue_long_stop(e, bar, 20.04, 1.0, true);
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(!p.placed_on(BFlatten));
    CHECK(!p.placed_on(BGap));
    CHECK(p.placed_on(BAffordable));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == BTouch);
        CHECK_NEAR(t.entry_price, 20.04, 1e-9);
        CHECK_NEAR(t.qty, 108.0, 1e-9); // floor(2183.50 / 20.04)
    }
}

// The sole bar-2 call costs 108 * 20.22 = 2183.76 > 2183.50.
// Later bars cross its stop, but the rejected placement must never revive.
void test_flatten_stop_once_dropped() {
    std::printf("-- synthetic flatten stop once dropped --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        if (bar == BFlatten) {
            e.entry_stop("L", true, 20.04, std::floor(e.equity() / 20.04));
        }
        if (bar == BClose && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(!p.placed_on(BFlatten));
    CHECK(p.pending_count() == 0);
    CHECK(p.flat());
    CHECK(p.trade_count() == 1);
}

// floor(0.9 * 2183.50 / 20.04) = 98; placement costs 98 * 20.22 =
// 1981.56 <= 2183.50. Bar 3 gaps through and fills at rounded open 20.22.
void test_flatten_stop_90_accepted_on_flattening_bar() {
    std::printf("-- synthetic flatten stop 90 accepted on flattening bar --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 20.04, 0.9, true);
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == BGap);
        CHECK_NEAR(t.entry_price, 20.22, 1e-9);
        CHECK_NEAR(t.qty, 98.0, 1e-9); // floor(0.9 * 2183.50 / 20.04)
        CHECK(t.entry_comment == "p" + std::to_string((int)BFlatten));
        CHECK_NEAR(t.exit_price, 20.01, 1e-9);
        CHECK_NEAR(t.pnl, -20.58, 1e-6); // 98 * (20.01 - 20.22)
    }
}

// The seed short is already closed: only 98 * 20.22 = 1981.56 counts.
// Bar 3 opens above the short stop and its low exactly touches 20.04.
void test_flatten_samedir_90_closed_position_not_counted() {
    std::printf("-- synthetic flatten samedir 90 closed position not counted --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        if (bar >= BFlatten && bar <= BClose - 1 && e.flat()) {
            e.entry_stop("S2", false, 20.04,
                         std::floor(0.9 * e.equity() / 20.04),
                         "p" + std::to_string(bar));
        }
        if (bar == BClose && e.position_size() != 0) e.strategy_close_all();
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(!t.is_long);
        CHECK(t.entry_id == "S2");
        CHECK(t.entry_bar_index == BGap);
        CHECK_NEAR(t.entry_price, 20.04, 1e-9);
        CHECK_NEAR(t.qty, 98.0, 1e-9); // floor(0.9 * 2183.50 / 20.04)
        CHECK_NEAR(t.exit_price, 20.01, 1e-9);
        CHECK_NEAR(t.pnl, 2.94, 1e-6); // 98 * (20.04 - 20.01)
    }
}

// Half margin costs 108 * 20.22 * 0.5 = 1091.88 <= 2183.50,
// so bar 2 accepts and bar 3 fills at rounded gap open 20.22.
void test_flatten_stop_m50_halves_cost() {
    std::printf("-- synthetic flatten stop m50 halves cost --\n");
    Probe p(2185.1, 50.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 20.04, 1.0, true);
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.entry_bar_index == BGap);
        CHECK_NEAR(t.entry_price, 20.22, 1e-9);
        CHECK_NEAR(t.qty, 108.0, 1e-9); // floor(2183.50 / 20.04)
        CHECK_NEAR(t.pnl, -22.68, 1e-6); // 108 * (20.01 - 20.22)
    }
}

// Higher capital gives E_post = 2185.60 - 1.60 = 2184.00. The same
// 108 * 20.22 = 2183.76 now passes; pre-exit MTM = 2184.00 -
// 80 * (20.215 - 20.04) = 2170.00 would wrongly reject.
void test_flatten_stop_floor_c10029_post_exit_equity() {
    std::printf("-- synthetic flatten stop floor post exit equity --\n");
    Probe p(2185.6, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 20.04, 1.0, true);
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.entry_bar_index == BGap);
        CHECK_NEAR(t.entry_price, 20.22, 1e-9);
        CHECK_NEAR(t.qty, 108.0, 1e-9); // floor(2184.00 / 20.04)
    }
}

// While the short is open, script MTM = 2185.10 + 80 * (20.02 - 20.215)
// = 2169.50. floor(0.9 * 2169.50 / 20.04) = 97, rather than the post-exit
// 98. Only the new 97-share side costs margin; both legs fill on bar 3.
void test_flatten_closenext_90_reversal_new_side_only() {
    std::printf("-- synthetic flatten closenext 90 reversal new side only --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == BSeed) e.strategy_entry("S", false, kNaN, kNaN, 80.0);
        if (bar == BFlatten) e.strategy_close("S", "xnext");
        if (bar >= BFlatten && bar <= BClose - 1 && e.position_size() <= 0) {
            e.entry_stop("L", true, 20.04, std::floor(0.9 * e.equity() / 20.04),
                         "p" + std::to_string(bar));
        }
        if (bar == BClose && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& s = p.get_trade(0);
        CHECK(!s.is_long);
        CHECK_NEAR(s.entry_price, 20.02, 1e-9);
        CHECK(s.exit_bar_index == BGap);
        CHECK_NEAR(s.exit_price, 20.22, 1e-9);
        CHECK_NEAR(s.pnl, -16.0, 1e-9); // 80 * (20.02 - 20.22)
        const Trade& t = p.get_trade(1);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == BGap);
        CHECK_NEAR(t.entry_price, 20.22, 1e-9);
        CHECK_NEAR(t.qty, 97.0, 1e-9); // floor(0.9 * 2169.50 / 20.04)
        CHECK_NEAR(t.pnl, -20.37, 1e-6); // 97 * (20.01 - 20.22)
    }
}

// Immediate close uses rounded close 20.22: E = 2185.10 - 16 = 2169.10.
// floor(0.9 * 2169.10 / 20.04) = 97. The next gap still fills at 20.22.
void test_flatten_closeimm_90() {
    std::printf("-- synthetic flatten closeimm 90 --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == BSeed) e.strategy_entry("S", false, kNaN, kNaN, 80.0);
        if (bar == BFlatten) e.strategy_close("S", "ximm", kNaN, kNaN, true);
        if (bar >= BFlatten && bar <= BClose - 1 && e.position_size() <= 0) {
            e.entry_stop("L", true, 20.04, std::floor(0.9 * e.equity() / 20.04),
                         "p" + std::to_string(bar));
        }
        if (bar == BClose && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& s = p.get_trade(0);
        CHECK(s.exit_bar_index == BFlatten);
        CHECK_NEAR(s.exit_price, 20.22, 1e-9);
        CHECK_NEAR(s.pnl, -16.0, 1e-9); // 80 * (20.02 - 20.22)
        const Trade& t = p.get_trade(1);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == BGap);
        CHECK_NEAR(t.entry_price, 20.22, 1e-9);
        CHECK_NEAR(t.qty, 97.0, 1e-9); // floor(0.9 * 2169.10 / 20.04)
    }
}

// Market qty uses raw close: floor(2183.50 / 20.215) = 108, rejected
// at rounded-close cost 2183.76. Next call floor(2183.50 / 20.22) = 107
// costs 2163.54, passes, and fills at bar 4 rounded open 20.23 (cost 2164.61).
void test_flatten_market_floor_round5_rule_unchanged() {
    std::printf("-- synthetic flatten market floor round5 rule unchanged --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        if (bar >= BFlatten && bar <= BClose - 1 && e.flat()) {
            e.strategy_entry("L", true, kNaN, kNaN,
                             std::floor(e.equity() / e.close_now()),
                             "p" + std::to_string(bar));
            e.placements.emplace_back(bar, e.pending("L"));
        }
        if (bar == BClose && e.position_size() > 0) e.strategy_close("L", "eod");
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    check_phase_a(p);
    CHECK(!p.placed_on(BFlatten));
    CHECK(p.placed_on(BGap));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        CHECK(t.entry_bar_index == BAffordable);
        CHECK_NEAR(t.entry_price, 20.23, 1e-9);
        CHECK_NEAR(t.qty, 107.0, 1e-9); // floor(2183.50 / 20.22)
        CHECK_NEAR(t.pnl, -23.54, 1e-6); // 107 * (20.01 - 20.23)
    }
}

// Raw qty 2002.40 / 20.01 = 100.069965... is floored to 100 shares.
// Rounded close 20.02 costs 2002.00 <= 2002.40; raw qty would cost
// 2003.400699... > capital. Bar 33 fills at its exact level/open 20.01.
void test_fresh_floorraw_0814_floored_qty() {
    std::printf("-- synthetic fresh floorraw floored qty --\n");
    Probe p(2002.4, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar >= BRawCall && bar <= BTailEnd && e.flat()) {
            e.entry_stop("L", true, 20.01, e.equity() / 20.01,
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BRawCall));
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9); // floor(2002.40 / 20.01)
    CHECK_NEAR(p.position_entry_price_, 20.01, 1e-9);
    CHECK(p.trade_count() == 0);
}

// floor(2001.50 / 20.00) = 100; seed close cost 2000.00 passes.
// Next open 20.02 costs 2002.00 > 2001.50: drop the whole order, no trim.
void test_fresh_gap_once_fill_rejected_and_dropped() {
    std::printf("-- synthetic fresh gap once fill rejected and dropped --\n");
    Probe p(2001.5, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == BSeed) {
            e.entry_stop("L", true, 20.00, std::floor(e.equity() / 20.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BSeed));
    CHECK(p.flat());
    CHECK(p.pending_count() == 0);
    CHECK(p.trade_count() == 0);
}

// After the rejected gap, every bar 1..26 closes at 20.02 or above,
// costing at least 2002.00 > 2001.50. Bar 27 close rounds to 19.99 (1999.00)
// and accepts; bar 28 opens below 20.00 and touches it intrabar.
void test_fresh_gap_replace() {
    std::printf("-- synthetic fresh gap replace --\n");
    Probe p(2001.5, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar >= BSeed && bar <= BTailEnd && e.flat()) {
            e.entry_stop("L", true, 20.00, std::floor(e.equity() / 20.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BSeed));
    for (int b = BSeedFill; b <= BClose; ++b) CHECK(!p.placed_on(b));
    CHECK(p.placed_on(BExit));
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9); // floor(2001.50 / 20.00)
    CHECK_NEAR(p.position_entry_price_, 20.00, 1e-9);
    CHECK(p.trade_count() == 0);

    int last_call = -1;
    for (const auto& pl : p.placements) last_call = pl.first;
    CHECK(last_call == BExit);
}

// floor(2000.50 / 19.81) = 100. Placement at close 20.00 costs 2000.00;
// level-touch cost is 1981.00, while bar 1 open costs 2002.00 > capital.
void test_fresh_touch_once_fill_costed_at_level() {
    std::printf("-- synthetic fresh touch once fill costed at level --\n");
    Probe p(2000.5, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == BSeed) {
            e.entry_stop("S", false, 19.81, std::floor(e.equity() / 19.81),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BSeed));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9); // floor(2000.50 / 19.81)
    CHECK_NEAR(p.position_entry_price_, 19.81, 1e-9);
    CHECK(p.trade_count() == 0);
}

// The same 100-share touch is admitted before ordinary margin slicing.
// At bar 1 close 20.02, equity = 2000.50 + 100 * (19.81 - 20.02) = 1979.50
// < required 2002.00. The 22.50 deficit exceeds one whole share at 20.02,
// so a fill-bar slice must occur, with total quantity
// conserved across closed slices and the remaining position.
void test_fresh_touch_once_margin_call_slices() {
    std::printf("-- synthetic fresh touch once margin call slices --\n");
    Probe p(2000.5, 100.0, 0.01, 1.0);
    p.enable_margin_call();
    p.script = [&](Probe& e, int bar) {
        if (bar == BSeed) {
            e.entry_stop("S", false, 19.81, std::floor(e.equity() / 19.81),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_entry_price_, 19.81, 1e-9);
    double sliced = 0.0;
    for (int i = 0; i < p.trade_count(); ++i) {
        sliced += p.get_trade(i).qty;
        CHECK(p.get_trade(i).entry_bar_index == BSeedFill);
        CHECK_NEAR(p.get_trade(i).entry_price, 19.81, 1e-9);
    }
    CHECK_NEAR(sliced + p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() >= 1);
    if (p.trade_count() >= 1) {
        CHECK(p.get_trade(0).exit_bar_index == BSeedFill);
    }
}

// floor(3000.75 / 30.00) = 100. Close 30.005 rounds half-up to 30.01:
// Raw close cost 3000.50 <= 3000.75, but rounded cost 3001.00 rejects.
// The next open 30.00 is affordable; checking only at fill would also be wrong.
void test_fresh_0919_once_half_up_rejected() {
    std::printf("-- synthetic fresh once half up rejected --\n");
    Probe p(3000.75, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == HSignal) {
            e.entry_stop("S", false, 30.00, std::floor(e.equity() / 30.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = half_tick_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(!p.placed_on(HSignal));
    CHECK(p.flat());
    CHECK(p.trade_count() == 0);
}

// Bars 0..4 round to at least 30.01 and reject 100 shares. Bar 5
// closes at 30.00 (cost 3000.00 <= 3000.75); bar 6 opens exactly at 30.00.
void test_fresh_0919_replace() {
    std::printf("-- synthetic fresh replace --\n");
    Probe p(3000.75, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar >= HSignal && bar <= HEnd && e.flat()) {
            e.entry_stop("S", false, 30.00, std::floor(e.equity() / 30.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = half_tick_bars();
    p.run(bars.data(), (int)bars.size());
    for (int b = HSignal; b <= HRejected4; ++b) CHECK(!p.placed_on(b));
    CHECK(p.placed_on(HAccepted));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9); // floor(3000.75 / 30.00)
    CHECK_NEAR(p.position_entry_price_, 30.00, 1e-9);
    CHECK(p.trade_count() == 0);
}

// Explicit quantities isolate admission from script sizing. First control:
// 112 * raw 20.215 = 2264.08 <= 2264.20 < 112 * rounded 20.22 = 2264.64;
// 112 * 20.03 = 2243.36 passes bar 4. Second control: 110 * 30.01 =
// 3301.10 > 3300.75 (raw 110 * 30.005 = 3300.55 would pass), but
// 110 * 30.00 = 3300.00 passes bar 5.
void test_probe_decode_f_0813_and_0919() {
    std::printf("-- synthetic explicit-quantity admission controls --\n");
    {
        Probe p(2264.2, 100.0, 0.01, 1.0);
        p.script = [&](Probe& e, int bar) {
            if (bar >= BFlatten && bar <= BClose - 1 && e.flat()) {
                e.entry_stop("L", true, 20.04, 112.0, "p" + std::to_string(bar));
            }
        };
        std::vector<Bar> bars = admission_bars();
        p.run(bars.data(), (int)bars.size());
        CHECK(!p.placed_on(BFlatten));
        CHECK(!p.placed_on(BGap));
        CHECK(p.placed_on(BAffordable));
        CHECK(p.position_side_ == PositionSide::LONG);
        CHECK_NEAR(p.position_qty_, 112.0, 1e-9);
        CHECK_NEAR(p.position_entry_price_, 20.04, 1e-9);
        CHECK(p.trade_count() == 0);

        CHECK(!p.pyramid_entries().empty());
        if (!p.pyramid_entries().empty()) {
            CHECK(p.pyramid_entries().back().entry_bar_index == BTouch);
        }
    }
    {
        Probe p(3300.75, 100.0, 0.01, 1.0);
        p.script = [&](Probe& e, int bar) {
            if (bar >= HSignal && bar <= HEnd && e.flat()) {
                e.entry_stop("S", false, 30.00, 110.0, "p" + std::to_string(bar));
            }
        };
        std::vector<Bar> bars = half_tick_bars();
        p.run(bars.data(), (int)bars.size());
        for (int b = HSignal; b <= HRejected4; ++b) CHECK(!p.placed_on(b));
        CHECK(p.placed_on(HAccepted));
        CHECK(p.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(p.position_qty_, 110.0, 1e-9);
        CHECK_NEAR(p.position_entry_price_, 30.00, 1e-9);
        CHECK(!p.pyramid_entries().empty());
        if (!p.pyramid_entries().empty()) {
            CHECK(p.pyramid_entries().back().entry_bar_index == HFill);
        }
    }
}

// Fractional-lot seed: long 2 at 201.25 on bar 1, stopped at 200.00
// on bar 2. Realized equity = capital + 2 * (200.00 - 201.25) = capital - 2.50.
void xau_phase_a(Probe& p, int bar) {
    if (bar == GSeed) {
        p.strategy_entry("Lg", true, kNaN, kNaN, 2.0);
        p.strategy_exit("XL", "Lg", kNaN, 200.00);
    }
}

void check_xau_phase_a(const Probe& p) {
    CHECK(p.trade_count() >= 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == GEntry);
        CHECK_NEAR(t.entry_price, 201.25, 1e-9);
        CHECK(t.exit_bar_index == GFlatten);
        CHECK_NEAR(t.exit_price, 200.00, 1e-9);
        CHECK_NEAR(t.pnl, -2.5, 1e-9); // 2 * (200.00 - 201.25)
    }
}

double xau_qty(const Probe& p, double L) {
    return std::floor(100.0 * p.equity() / L) / 100.0;
}

// Control E = 2198.40 - 2.50 = 2195.90; floor(100 * E / 200) / 100
// = 10.97. Initial cost 10.97 * 200.05 = 2194.5485 passes. Bars 3..5 have
// lows above 200.00, so the order must rest until bar 6's level touch.
void test_xau_flatten_once_rests_until_touched() {
    std::printf("-- synthetic xau flatten once rests until touched --\n");
    Probe p(2198.4, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar == GFlatten) {
            e.entry_stop("S", false, 200.00, xau_qty(e, 200.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = fractional_lot_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(GFlatten));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 10.97, 1e-9); // floor(100 * 2195.90 / 200.00) / 100
    CHECK_NEAR(p.position_entry_price_, 200.00, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == GTouch);
    }
    CHECK(p.trade_count() == 1);
}

// E = 2197.50 - 2.50 = 2195.00; floor(100 * E / 200) / 100 = 10.97.
// Bar 5 close costs 10.97 * 200.15 = 2195.6455 > E; resting admission must
// survive. Bar 6 open costs 10.97 * 200.175 = 2195.91975 > E, whereas
// the touched level costs 10.97 * 200.00 = 2194.00 <= E.
void test_xau_flatten_once_c10983_no_recheck_fill_at_level() {
    std::printf("-- synthetic xau flatten once no recheck fill at level --\n");
    Probe p(2197.5, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar == GFlatten) {
            e.entry_stop("S", false, 200.00, xau_qty(e, 200.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = fractional_lot_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(GFlatten));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 10.97, 1e-9); // floor(100 * 2195.00 / 200.00) / 100
    CHECK_NEAR(p.position_entry_price_, 200.00, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == GTouch);
    }
}

// At E = 2195.00, bars 2/3/4 cost 2194.5485 / 2194.6582 / 2194.7679
// and pass. Bar 5 costs 2195.6455 and cancels the prior order, so bar 6
// cannot fill. Its close costs 10.97 * 200.02 = 2194.2194 and accepts a
// fresh order; bar 7 touches 200.00, carrying the bar-6 comment.
void test_xau_flatten_replace_c10983_rejected_reissue_cancels() {
    std::printf("-- synthetic xau flatten replace rejected reissue cancels --\n");
    Probe p(2197.5, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar >= GFlatten && bar <= GEnd && e.flat()) {
            e.entry_stop("S", false, 200.00, xau_qty(e, 200.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = fractional_lot_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(GFlatten));
    CHECK(p.placed_on(GAccepted1));
    CHECK(p.placed_on(GAccepted2));
    CHECK(!p.placed_on(GRejected));
    CHECK(p.called_on(GTouch));
    CHECK(p.placed_on(GTouch));
    CHECK(!p.called_on(GRetryTouch));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 10.97, 1e-9); // floor(100 * 2195.00 / 200.00) / 100
    CHECK_NEAR(p.position_entry_price_, 200.00, 1e-9);
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == GRetryTouch);
        CHECK(p.pyramid_entries().back().entry_comment
              == "p" + std::to_string((int)GTouch));
    }
}

// Control E = 2195.90 keeps the same 10.97 lots. Even bar 5 cost
// 2195.6455 is affordable, so its replacement fills on bar 6's touch.
void test_xau_flatten_replace_control() {
    std::printf("-- synthetic xau flatten replace control --\n");
    Probe p(2198.4, 100.0, 0.005, 0.01);
    p.script = [&](Probe& e, int bar) {
        xau_phase_a(e, bar);
        if (bar >= GFlatten && bar <= GEnd && e.flat()) {
            e.entry_stop("S", false, 200.00, xau_qty(e, 200.00),
                         "p" + std::to_string(bar));
        }
    };
    std::vector<Bar> bars = fractional_lot_bars();
    p.run(bars.data(), (int)bars.size());
    check_xau_phase_a(p);
    CHECK(p.placed_on(GRejected));
    CHECK(!p.called_on(GTouch));
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 10.97, 1e-9); // floor(100 * 2195.90 / 200.00) / 100
    CHECK(!p.pyramid_entries().empty());
    if (!p.pyramid_entries().empty()) {
        CHECK(p.pyramid_entries().back().entry_bar_index == GTouch);
        CHECK(p.pyramid_entries().back().entry_comment
              == "p" + std::to_string((int)GRejected));
    }
}

// The new long leg costs 120 * 20.22 = 2426.40 > script MTM 2169.50.
// Rejection drops that leg but keeps the closing leg: bar 3 gaps through
// and closes the 80-share short at 20.22 without opening a long.
void test_rejected_stop_reversal_keeps_close_leg() {
    std::printf("-- synthetic rejected stop reversal keeps close leg --\n");
    Probe p(2185.1, 100.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == BSeed) e.strategy_entry("S", false, kNaN, kNaN, 80.0);
        if (bar == BFlatten) e.entry_stop("L", true, 20.04, 120.0, "rev");
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BFlatten));
    CHECK(p.flat());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& s = p.get_trade(0);
        CHECK(!s.is_long);
        CHECK(s.exit_bar_index == BGap);
        CHECK_NEAR(s.exit_price, 20.22, 1e-9);
        CHECK(s.exit_id == "L");
    }
    CHECK(p.pending_count() == 0);
}

// Zero margin disables both admission halves. The bar-2 call rests
// and the bar-3 gap fills all floor(2183.50 / 20.04) = 108 shares at 20.22.
void test_margin_zero_control() {
    std::printf("-- synthetic margin zero control --\n");
    Probe p(2185.1, 0.0, 0.01, 1.0);
    p.script = [&](Probe& e, int bar) {
        phase_a_short(e, bar);
        reissue_long_stop(e, bar, 20.04, 1.0, true);
    };
    std::vector<Bar> bars = admission_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(BFlatten));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(p.get_trade(1).entry_bar_index == BGap);
        CHECK_NEAR(p.get_trade(1).entry_price, 20.22, 1e-9);
        CHECK_NEAR(p.get_trade(1).qty, 108.0, 1e-9); // floor(2183.50 / 20.04)
    }
}

// --- regression: ahtisham-ee-decoded-volatility-expansion-ahtisham ---
// BINANCE:ETHUSDT.P 15 (feed 27b62431096e, mintick 0.01, qty step 0.0001),
// strategy(default_qty_type = percent_of_equity, default_qty_value = 100),
// Pine v6 -> margin_long/short default 100. While flat the script re-issues
// BOTH strategy.entry("Long", stop = zoneHigh + 1.5 atr) and
// strategy.entry("Short", stop = zoneLow - 1.5 atr) every bar; in a position
// it re-issues strategy.exit(stop = zoneMid, limit = 2R). Levels below are the
// hand replay of the Pine indicators over the registry bars (ta.highest /
// ta.lowest of high[1] / low[1] over 20, RMA-14 ATR warmed from 03-20); they
// reproduce the engine's own levels to the tick (short 1859.6378 -> 1859.63,
// the candidate's fill; long 1912.3929 -> 1912.40, the base's fill; mid
// 1885.56, the base's exit).
//
// base-round7-harness-20260905 (c2032d1, = TV 591/591): after trade 4 (equity
// 9,880.86) nothing fills until the 15:30Z long touch @1912.40 (5.1667),
// stopped 17:30Z @1885.56 (-138.674228). cand-round7-engine-a-20260905
// (09fa1fe) instead FILLED the short at the 05:15Z touch: o 1866.16 > level
// 1859.63 >= l 1853.57, all-in 5.3133 x 1859.63 = 9,880.8 <= 9,880.86 at the
// level where 5.3133 x 1866.16 = 9,915.5 > 9,880.86 declines at the open
// (TV: no trade; 394 such shorts over the range, 591 -> 1,177 trades).
// Family K (this round) explains it from the placement side: the 05:00Z
// call sizes the sell stop at the level (5.3133 = floor(9,880.86 / 1859.63,
// 0.0001)) and rejects it at the close (5.3133 x 1866.16 = 9,915.5 >
// 9,880.86) — nothing rests for the 05:15Z touch. The buy stop at the level
// 1912.40 sizes 5.1667 (9,880.8 <= 9,880.86) and fills the touch unchanged.
struct EthRow { double o, h, l, c, buy_stop, sell_stop, mid; };
constexpr int kEthCount = 56;   // 2025-04-02 04:15Z .. 18:00Z
enum EthBar {
    E0402_0500 = 3, E0402_0515 = 4, E0402_0530 = 5,
    E0402_1515 = 44, E0402_1530 = 45, E0402_1730 = 53,
};
const EthRow kEth0402[kEthCount] = {
        {1880.67, 1882.31, 1878.63, 1880.67, 1925.6679, 1860.0021, 1892.835},   // b0 04:15Z
        {1880.66, 1880.67, 1876.19, 1876.59, 1925.3402, 1860.3298, 1892.835},   // b1 04:30Z
        {1876.59, 1879.81, 1874.72, 1878.14, 1920.0613, 1860.5687, 1890.315},   // b2 04:45Z
        {1878.14, 1878.38, 1862.53, 1866.16, 1919.6822, 1859.6378, 1889.660},   // b3 05:00Z
        {1866.16, 1866.46, 1853.57, 1855.88, 1916.6796, 1850.3104, 1883.495},   // b4 05:15Z
        {1855.88, 1862.18, 1854.03, 1859.99, 1912.2000, 1841.3500, 1876.775},   // b5 05:30Z
        {1859.99, 1863.69, 1854.1, 1855.08, 1908.8346, 1841.1954, 1875.015},   // b6 05:45Z
        {1855.07, 1859.35, 1853.86, 1857.2, 1907.9189, 1841.4911, 1874.705},   // b7 06:00Z
        {1857.2, 1861.45, 1850.69, 1855.8, 1908.2090, 1841.2010, 1874.705},   // b8 06:15Z
        {1855.79, 1861.72, 1851.7, 1860.89, 1908.3991, 1838.1309, 1873.265},   // b9 06:30Z
        {1860.89, 1860.9, 1852.63, 1852.84, 1905.8381, 1838.1419, 1871.990},   // b10 06:45Z
        {1852.84, 1856.59, 1851.34, 1854.99, 1904.6243, 1838.4757, 1871.550},   // b11 07:00Z
        {1854.98, 1861.6, 1853.33, 1856.15, 1898.6779, 1838.4621, 1868.570},   // b12 07:15Z
        {1856.16, 1863.88, 1855.2, 1858.9, 1898.7345, 1838.4055, 1868.570},   // b13 07:30Z
        {1858.9, 1865.78, 1858.33, 1864.07, 1898.6552, 1838.4848, 1868.570},   // b14 07:45Z
        {1864.06, 1866.44, 1861.81, 1864.52, 1898.2795, 1838.8605, 1868.570},   // b15 08:00Z
        {1864.51, 1869.36, 1861.61, 1867.93, 1898.2649, 1838.8751, 1868.570},   // b16 08:15Z
        {1867.93, 1875.54, 1867.11, 1871.42, 1894.9442, 1838.8158, 1866.880},   // b17 08:30Z
        {1871.41, 1872.6, 1865.88, 1866.91, 1894.0560, 1838.9440, 1866.500},   // b18 08:45Z
        {1866.9, 1887.23, 1865.38, 1883.98, 1895.5581, 1837.4419, 1866.500},   // b19 09:00Z
        {1883.98, 1885, 1874, 1878.59, 1900.7104, 1837.2096, 1868.960},   // b20 09:15Z
        {1878.58, 1881.73, 1876, 1876.58, 1900.3614, 1837.5586, 1868.960},   // b21 09:30Z
        {1876.57, 1880.38, 1874.98, 1878.44, 1900.0020, 1837.9180, 1868.960},   // b22 09:45Z
        {1878.43, 1883.8, 1876.15, 1880.03, 1899.9094, 1838.0106, 1868.960},   // b23 10:00Z
        {1880.02, 1886.7, 1878.19, 1878.91, 1899.9155, 1838.0045, 1868.960},   // b24 10:15Z
        {1878.9, 1878.98, 1871.85, 1874.48, 1899.7733, 1838.1467, 1868.960},   // b25 10:30Z
        {1874.47, 1876.52, 1870, 1871.92, 1899.5759, 1838.3441, 1868.960},   // b26 10:45Z
        {1871.93, 1877.62, 1871, 1874.59, 1899.4034, 1838.5166, 1868.960},   // b27 11:00Z
        {1874.59, 1877.53, 1872.5, 1872.6, 1899.0728, 1838.8472, 1868.960},   // b28 11:15Z
        {1872.6, 1873.99, 1868.5, 1871.4, 1898.8151, 1839.7549, 1869.285},   // b29 11:30Z
        {1871.39, 1872.64, 1867.32, 1868.05, 1898.5576, 1840.0124, 1869.285},   // b30 11:45Z
        {1868.08, 1869.97, 1859.53, 1865.27, 1898.8670, 1839.7030, 1869.285},   // b31 12:00Z
        {1865.26, 1865.99, 1857.06, 1857.87, 1898.9926, 1841.5674, 1870.280},   // b32 12:15Z
        {1857.87, 1867.48, 1857.54, 1866.86, 1899.2174, 1843.2126, 1871.215},   // b33 12:30Z
        {1866.85, 1872.55, 1863.5, 1871.16, 1899.3308, 1844.9592, 1872.145},   // b34 12:45Z
        {1871.16, 1871.17, 1860.21, 1860.9, 1899.6408, 1844.6492, 1872.145},   // b35 13:00Z
        {1860.91, 1863.27, 1856.89, 1858.15, 1899.4378, 1844.8522, 1872.145},   // b36 13:15Z
        {1858.15, 1876.61, 1853.12, 1872.19, 1901.0826, 1843.0374, 1872.060},   // b37 13:30Z
        {1872.27, 1892.7, 1869.82, 1870.41, 1902.5446, 1837.8054, 1870.175},   // b38 13:45Z
        {1870.4, 1880.71, 1856.5, 1863.13, 1909.5146, 1836.3054, 1872.910},   // b39 14:00Z
        {1863.14, 1872.65, 1860.01, 1870.75, 1909.6679, 1836.1521, 1872.910},   // b40 14:15Z
        {1870.75, 1875.85, 1863.52, 1870.75, 1909.7769, 1836.0431, 1872.910},   // b41 14:30Z
        {1870.74, 1877.04, 1868.55, 1874.69, 1909.4668, 1836.3532, 1872.910},   // b42 14:45Z
        {1874.7, 1894.47, 1873.16, 1881, 1910.5524, 1835.2676, 1872.910},   // b43 15:00Z
        {1881, 1893.06, 1880.5, 1889.52, 1912.3929, 1835.1971, 1873.795},   // b44 15:15Z
        {1889.52, 1918, 1889.3, 1913.94, 1914.1877, 1833.4023, 1873.795},   // b45 15:30Z
        {1913.9, 1918, 1892.39, 1900.09, 1939.0532, 1832.0668, 1885.560},   // b46 15:45Z
        {1900.1, 1905, 1894.05, 1901.26, 1938.7227, 1832.3973, 1885.560},   // b47 16:00Z
        {1901.26, 1907.4, 1900.77, 1906.86, 1937.9528, 1833.1672, 1885.560},   // b48 16:15Z
        {1906.86, 1909.21, 1897.1, 1902.19, 1937.8251, 1833.2949, 1885.560},   // b49 16:30Z
        {1902.19, 1906.94, 1898.41, 1901.99, 1937.3230, 1833.7970, 1885.560},   // b50 16:45Z
        {1902, 1913.85, 1900.8, 1910.59, 1937.3410, 1833.7790, 1885.560},   // b51 17:00Z
        {1910.59, 1910.99, 1896.9, 1896.9, 1937.4691, 1833.6509, 1885.560},   // b52 17:15Z
        {1896.9, 1897.97, 1883.27, 1888.15, 1937.6535, 1833.4665, 1885.560},   // b53 17:30Z
        {1888.14, 1893.88, 1881.09, 1891.61, 1937.6200, 1833.5000, 1885.560},   // b54 17:45Z
        {1891.6, 1896.75, 1887.29, 1891.34, 1937.2321, 1833.8879, 1885.560},   // b55 18:00Z
};

std::vector<Bar> eth_bars() {
    const int64_t t0 = 1743567300000LL;   // 2025-04-02 04:15Z
    std::vector<Bar> b;
    for (int i = 0; i < kEthCount; ++i) {
        b.push_back(mk(t0 + i * kMin15, kEth0402[i].o, kEth0402[i].h,
                       kEth0402[i].l, kEth0402[i].c));
    }
    return b;
}

void test_ahtisham_default_pct_stop() {
    std::printf("-- ahtisham: default percent 100 x margin 100, the 05:00Z sell stop is never placed (no 05:15Z touch fill), the 15:30Z long touch fills 5.1667 sized at the level --\n");
    Probe p(9880.86, 100.0, 0.01, 0.0001);
    p.use_default_percent(100.0);
    p.script = [&](Probe& e, int bar) {
        const EthRow& r = kEth0402[bar];
        if (bar == E0402_0530) {
            // The bar after the touch: the fill was declined and dropped,
            // nothing opened, no trade (the candidate had a 5.3133 short
            // here, already sliced 0.1552 @1866.46 by the margin call).
            CHECK(e.flat());
            CHECK(e.trade_count() == 0);
        }
        if (e.position_size() == 0) {
            e.entry_stop("Long", true, r.buy_stop, kNaN, "EXPANSION UP");
            e.entry_stop("Short", false, r.sell_stop, kNaN, "EXPANSION DOWN");
            if (bar == E0402_0500) {
                // Family K placement check: the all-in sell stop below the
                // close (5.3133 x 1866.16 > 9,880.86) is rejected; the buy
                // stop rests, sized at its level.
                CHECK(e.pending("Long"));
                CHECK(!e.pending("Short"));
            }
            if (bar == E0402_1515) {
                CHECK(e.pending("Long"));
                CHECK(!e.pending("Short"));
            }
        }
        if (e.position_size() > 0) {
            const double tp = r.buy_stop + std::fabs(r.buy_stop - r.mid) * 2.0;
            e.strategy_exit("L-Exit", "Long", tp, r.mid);
        }
        if (e.position_size() < 0) {
            const double tp = r.sell_stop - std::fabs(r.sell_stop - r.mid) * 2.0;
            e.strategy_exit("S-Exit", "Short", tp, r.mid);
        }
    };
    std::vector<Bar> bars = eth_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == E0402_1530);
        CHECK_NEAR(t.entry_price, 1912.40, 1e-9);
        CHECK_NEAR(t.qty, 5.1667, 1e-9);           // floor(9880.86 / 1912.4, 0.0001)
        CHECK(t.entry_comment == "EXPANSION UP");
        CHECK(t.exit_bar_index == E0402_1730);
        CHECK_NEAR(t.exit_price, 1885.56, 1e-9);
        CHECK_NEAR(t.pnl, -138.674228, 1e-6);
    }
    CHECK(p.flat());
}

// The same bars with the short passed an EXPLICIT qty of the same size: the
// explicit partition takes the pinned rule — the 05:00Z placement itself is
// rejected (5.3133 x tick(close 1866.16) = 9,915.5 > 9,880.86) and dropped,
// so nothing rests for the 05:15Z touch either; a smaller explicit short
// (5.2: 9,704 at the close) is accepted and FILLS at the level on the touch
// (5.2 x 1859.63 = 9,670 <= 9,880.86) — fresh-touch-once's shape.
void test_ahtisham_explicit_qty_control() {
    std::printf("-- ahtisham control: the explicit-qty short takes the pinned placement / level rule --\n");
    for (double q : {5.3133, 5.2}) {
        Probe p(9880.86, 100.0, 0.01, 0.0001);
        p.script = [&](Probe& e, int bar) {
            const EthRow& r = kEth0402[bar];
            if (bar <= E0402_0500 && e.flat()) {
                e.entry_stop("Short", false, r.sell_stop, q, "S");
            }
            if (bar == E0402_0530 && e.position_size() < 0) {
                e.strategy_close("Short", "x");
            }
        };
        std::vector<Bar> bars = eth_bars();
        p.run(bars.data(), (int)bars.size());
        if (q > 5.3) {
            CHECK(!p.placed_on(E0402_0500));
            CHECK(p.trade_count() == 0);
        } else {
            CHECK(p.placed_on(E0402_0500));
            CHECK(p.trade_count() == 1);
            if (p.trade_count() == 1) {
                CHECK(!p.get_trade(0).is_long);
                CHECK(p.get_trade(0).entry_bar_index == E0402_0515);
                CHECK_NEAR(p.get_trade(0).entry_price, 1859.63, 1e-9);
                CHECK_NEAR(p.get_trade(0).qty, 5.2, 1e-9);
            }
        }
    }
}

}  // namespace

int main() {
    std::printf("--- stop_entry_admission (synthetic admission paths) ---\n");
    test_flatten_stop_floor();
    test_fresh_stop_floor();
    test_flatten_stop_once_dropped();
    test_flatten_stop_90_accepted_on_flattening_bar();
    test_flatten_samedir_90_closed_position_not_counted();
    test_flatten_stop_m50_halves_cost();
    test_flatten_stop_floor_c10029_post_exit_equity();
    test_flatten_closenext_90_reversal_new_side_only();
    test_flatten_closeimm_90();
    test_flatten_market_floor_round5_rule_unchanged();
    test_fresh_floorraw_0814_floored_qty();
    test_fresh_gap_once_fill_rejected_and_dropped();
    test_fresh_gap_replace();
    test_fresh_touch_once_fill_costed_at_level();
    test_fresh_touch_once_margin_call_slices();
    test_fresh_0919_once_half_up_rejected();
    test_fresh_0919_replace();
    test_probe_decode_f_0813_and_0919();
    test_xau_flatten_once_rests_until_touched();
    test_xau_flatten_once_c10983_no_recheck_fill_at_level();
    test_xau_flatten_replace_c10983_rejected_reissue_cancels();
    test_xau_flatten_replace_control();
    test_rejected_stop_reversal_keeps_close_leg();
    test_margin_zero_control();
    test_ahtisham_default_pct_stop();
    test_ahtisham_explicit_qty_control();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
