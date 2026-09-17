// R4-D L10aj: on the native execution route a position that TradingView closes
// in FULL is flat afterwards, and a pyramided lot is never fragmented by the
// 1x-margin (margin_long = margin_short = 100) path.  ab9714be decides "is this
// reservation a full exit?" inside a kFullPercentEps band of 100
// (src/source/pine_fills.cpp:6893-6896), spells whole-position coverage as
// `qty - kQtyEpsilon` (pine_fills.cpp:1618) and then books that covered
// residual through the WHOLE-position exit instead of a sized reduction
// (pine_fills.cpp:1708-1723, routed from pine_fills.cpp:384).  The switched
// route instead re-derived the units as `live_basis * percent / 100` for a
// percentage that only misses 100 by the binary64 rounding of the owner's own
// `reserved / live_basis * 100` (pine_fills.cpp:7124), and submitted the stale
// residual as an off-grid sized reduction.  Two population probes diverge:
//
// (a) data-OANDA-EURUSD / doriannnq-tjr-v4-strategy -- the owner and this tree
//     agree on rows #1-#4 (#4 Entry short 2025-08-15 18:00 @1.170430
//     q=979624.57, Exit short 2025-08-17 22:00 @1.171620) and the owner is
//     FLAT afterwards: it enters long #5 on 2025-08-25 15:00 (1661812 units,
//     same-bar stop-out) and 11 more trades.  This tree kept a residual SHORT
//     lot smaller than one qty_step open from 2025-08-15 18:00 to the range end
//     ("5 Entry short 2025-08-15 18:00 1.170430 qty 0 ... Exit short
//     2026-04-29 18:00 1.167500 qty 0, Engine range-end = open"), so
//     strategy.position_size never returned to 0, the script's flat-only
//     entries never fired again and the tape collapsed to 5 trades (owner 16).
//
// (b) data-BINANCE-BTCUSDT / projectsyndicate-strong-breakout-signals-
//     projectsyndicate -- at 2025-04-02 13:30 the owner holds TWO long lots
//     (#11 0.00583 and #12 0.00118 BTC, both entered @85470.15) and exits both
//     at 14:00 @85197.91.  This tree split the second lot into 0.00047 +
//     0.00059 (both exit 15:30 @87178.63) plus 0.00012 that never exits
//     (range-end open): the 1x-margin slice fragments the lot and strands a
//     piece, collapsing 1395 trades to 14.
//
// The units below replay both shapes on the lane facts the grader forwards for
// each probe (OANDA:EURUSD forex qty_step 0.01 / mintick 1e-5 / pointvalue 1 /
// session 1700-1700 America/New_York; BINANCE:BTCUSDT crypto qty_step 1e-5 /
// mintick 0.01 / session 24x7 UTC; margin_long = margin_short = 100 on both,
// i.e. no leverage, so a percent-of-equity opening consumes the whole equity).
//
// Bars are embedded literals copied out of the two probes' lane feeds
// (pineforge-lab evidence ohlcv_OANDA-EURUSD_15m.csv rows 2025-08-15
// 17:45..20:45 UTC and ohlcv_BINANCE-BTCUSDT_15m.csv rows 2025-04-02
// 13:00..14:15 UTC); this test must never open corpus files or absolute paths
// at runtime (CI has no corpus or lane checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

void dump(const char* tag, const Trade& t) {
    std::printf("%s id=%s/%s %s entry=%.6f@%lld exit=%.6f@%lld qty=%.17g "
                "pnl=%.6f open_at_end=%d incarn=%llu\n",
                tag, t.entry_id.c_str(), t.exit_id.c_str(),
                t.is_long ? "L" : "S", t.entry_price,
                static_cast<long long>(t.entry_time), t.exit_price,
                static_cast<long long>(t.exit_time), t.qty, t.pnl,
                static_cast<int>(t.open_at_end),
                static_cast<unsigned long long>(t.entry_incarnation));
}

// ---------------------------------------------------------------------------
// Lane feeds.  EURUSD index 1 (18:00 UTC) is the bar the owner's #4 short is
// signalled on and index 9 (20:00 UTC, high 1.17082) is the first bar that
// trades the 1.17080 stop; every low of the window stays above the 1.16800 /
// 1.16900 / 1.16850 profit legs, so the ladder can only exit through the stop.
// BTCUSDT indices 2 and 3 (13:30 / 14:00 UTC) are the two bars the owner's #11
// and #12 lots are sized off (closes 85470.15 / 85215.82) and index 4 (14:15
// UTC, low 85181.52) is the first bar that trades the 85197.91 stop; the
// 86500 / 87500 / 88500 legs are never reached.
// ---------------------------------------------------------------------------
std::vector<Bar> eurusd_bars() {
    return {
        mk(1755279900000LL, 1.17047, 1.17060, 1.17022, 1.17044),  // 0: 17:45
        mk(1755280800000LL, 1.17044, 1.17052, 1.16986, 1.17042),  // 1: 18:00
        mk(1755281700000LL, 1.17043, 1.17066, 1.17029, 1.17030),  // 2: 18:15
        mk(1755282600000LL, 1.17028, 1.17046, 1.17014, 1.17026),  // 3: 18:30
        mk(1755283500000LL, 1.17026, 1.17052, 1.17011, 1.17026),  // 4: 18:45
        mk(1755284400000LL, 1.17025, 1.17034, 1.17010, 1.17021),  // 5: 19:00
        mk(1755285300000LL, 1.17022, 1.17022, 1.17000, 1.17011),  // 6: 19:15
        mk(1755286200000LL, 1.17010, 1.17036, 1.17010, 1.17016),  // 7: 19:30
        mk(1755287100000LL, 1.17016, 1.17024, 1.17000, 1.17009),  // 8: 19:45
        mk(1755288000000LL, 1.17008, 1.17082, 1.17008, 1.17080),  // 9: 20:00
        mk(1755288900000LL, 1.17076, 1.17080, 1.17036, 1.17038),  // 10: 20:15
        mk(1755289800000LL, 1.17037, 1.17045, 1.17006, 1.17018),  // 11: 20:30
        mk(1755290700000LL, 1.17018, 1.17068, 1.17009, 1.17050),  // 12: 20:45
    };
}

std::vector<Bar> btcusdt_bars() {
    return {
        mk(1743598800000LL, 85049.99, 85050.00, 84844.00, 84850.63),  // 0: 13:00
        mk(1743599700000LL, 84850.62, 84879.99, 84614.59, 84628.23),  // 1: 13:15
        mk(1743600600000LL, 84628.24, 85539.74, 84471.76, 85470.15),  // 2: 13:30
        mk(1743601500000LL, 85472.49, 86136.36, 85215.75, 85215.82),  // 3: 14:00
        mk(1743602400000LL, 85215.79, 85807.23, 85181.52, 85435.98),  // 4: 14:15
        mk(1743603300000LL, 85435.97, 85764.16, 85327.39, 85736.26),  // 5: 14:30
    };
}

struct Lane {
    const char* ticker;
    const char* type;
    const char* timezone;
    const char* session;
    double mintick;
    double qty_step;
};

constexpr Lane kEurUsd{"OANDA:EURUSD", "forex", "America/New_York", "1700-1700",
                       1e-5, 0.01};
constexpr Lane kBtcUsdt{"BINANCE:BTCUSDT", "crypto", "UTC", "24x7", 0.01, 1e-5};

struct Shape {
    double capital = 10000.0;
    double qty_percent = 10.0;
    double explicit_qty = kNaN;
    int pyramiding = 1;
    bool process_orders_on_close = false;
    int slippage = 0;
    int entry_bars = 1;        // consecutive bars that each open one lot
    int first_entry_bar = 1;
    int exit_legs = 1;         // 1 -> one 100% bracket, 3 -> the 40/50/100 ladder
    bool is_short = false;
    double stop = 0.0;
    double limit = 0.0;
    double tp2 = 0.0;
    double tp3 = 0.0;
};

// The probes' order shape: entries only while flat (or up to the pyramiding
// cap), and the bracket re-issued with the SAME ids on every bar the position
// is held (L10n re-issue).
class MarginLaneHost : public source::PineStrategyHost {
public:
    MarginLaneHost(const Lane& lane, const Shape& s) : s_(s) {
        source::PineStrategyConfig c;
        c.initial_capital = s.capital;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = s.qty_percent;
        c.pyramiding = s.pyramiding;
        c.process_orders_on_close = s.process_orders_on_close;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.0;   // neither probe declares a commission
        c.slippage = s.slippage;
        // The lane fact under test: no leverage, so a percent-of-equity
        // opening consumes the whole equity and every exit is a 1x-margin
        // reduction candidate.
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        // set_syminfo_metadata() is a generic key/value map that only honours
        // qty_step / account_currency_fx, so the lane's remaining symbol facts
        // are pinned on the engine members directly -- the idiom used by
        // tests/test_l10ac_full_equity_single_lot.cpp.
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = lane.mintick;
        syminfo_mintick_ = lane.mintick;
        syminfo_.ticker = lane.ticker;
        syminfo_.tickerid = lane.ticker;
        syminfo_.type = lane.type;
        syminfo_.timezone = lane.timezone;
        syminfo_.session = lane.session;
        set_syminfo_metadata("qty_step", lane.qty_step);
    }

    // strategy.position_size is the freeze-aware script view, i.e. exactly what
    // the probes' flat-only entry gates read, and the physical lot count is the
    // book the 1x-margin slice reduces.
    double position_at(int bar) const {
        return bar < static_cast<int>(pos_.size()) ? pos_[bar] : kNaN;
    }
    int lots_at(int bar) const {
        return bar < static_cast<int>(lots_.size()) ? lots_[bar] : -1;
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i >= 0) {
            if (static_cast<int>(pos_.size()) <= i) {
                pos_.resize(static_cast<std::size_t>(i) + 1, 0.0);
                lots_.resize(static_cast<std::size_t>(i) + 1, 0);
            }
            pos_[static_cast<std::size_t>(i)] = signed_position_size();
            lots_[static_cast<std::size_t>(i)] =
                static_cast<int>(physical_position().lot_count);
        }
        const double pos = signed_position_size();
        const bool held = s_.is_short ? pos < 0.0 : pos > 0.0;
        const std::string id = s_.is_short ? "Short" : "Long";
        for (int k = 0; k < s_.entry_bars; ++k) {
            if (i == s_.first_entry_bar + k)
                strategy_entry(id, !s_.is_short, kNaN, kNaN, s_.explicit_qty);
        }
        if (!held) return;
        if (s_.exit_legs == 1) {
            strategy_exit("X", id, s_.limit, s_.stop, kNaN, kNaN, kNaN, 100.0);
            return;
        }
        strategy_exit("X1", id, s_.limit, s_.stop, kNaN, kNaN, kNaN, 40.0);
        strategy_exit("X2", id, s_.tp2, s_.stop, kNaN, kNaN, kNaN, 50.0);
        strategy_exit("X3", id, s_.tp3, s_.stop, kNaN, kNaN, kNaN, 100.0);
    }

private:
    Shape s_;
    std::vector<double> pos_;
    std::vector<int> lots_;
};

// ---------------------------------------------------------------------------
// Unit 1 -- probe (a): a percent-of-equity short on the EURUSD lane facts whose
// 40/50/100 ladder closes it in full must leave the book FLAT, with no sub-lot
// residual surviving to the range end.
// ---------------------------------------------------------------------------
void test_full_percent_short_is_flat_after_its_full_exit() {
    Shape s;
    s.capital = 1500000.0;
    s.qty_percent = 99.0;
    s.pyramiding = 0;
    s.slippage = 1;
    s.is_short = true;
    s.first_entry_bar = 1;
    s.exit_legs = 3;
    s.stop = 1.17080;
    s.limit = 1.16800;
    s.tp2 = 1.16900;
    s.tp3 = 1.16850;
    MarginLaneHost host(kEurUsd, s);
    const auto bars = eurusd_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    std::printf("trades=%d lots=%d live=%.17g\n", host.trade_count(),
                static_cast<int>(host.physical_position().lot_count),
                host.live_position_size());
    for (int i = 0; i < host.trade_count(); ++i) dump("  eur", host.get_trade(i));

    CHECK(host.position_at(8) < 0.0);        // the short is open before the stop
    // The owner is flat after the ladder's 100% leg; the pre-fix route kept a
    // residual short lot open to the range end (its row 5 is an open_at_end
    // Exit short 2026-04-29 18:00 with "Engine range-end = open").
    CHECK(host.trade_count() == 3);
    CHECK(host.live_position_size() == 0.0);
    CHECK(host.physical_position().signed_units == 0.0);
    CHECK(host.physical_position().lot_count == 0);
    // strategy.position_size is 0 on the bar AFTER the exit bar and stays 0 to
    // the end of the tape, so the script's flat-only entries can fire again.
    for (int i = 10; i < static_cast<int>(bars.size()); ++i)
        CHECK(host.position_at(i) == 0.0);
    if (host.trade_count() != 3) return;

    double closed = 0.0;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(!t.is_long);
        CHECK(!t.open_at_end);
        CHECK(t.entry_id == "Short");
        CHECK(t.exit_id == "X1" || t.exit_id == "X2" || t.exit_id == "X3");
        CHECK(t.exit_from_bracket);
        CHECK(t.entry_time == bars[2].timestamp);   // fills on the 18:15 open
        CHECK(near(t.entry_price, 1.170420, 1e-9)); // open 1.17043 - 1 slip tick
        CHECK(t.exit_time == bars[9].timestamp);    // the 20:00 stop bar
        CHECK(near(t.exit_price, 1.170810, 1e-9));  // stop 1.17080 + 1 slip tick
        CHECK(t.qty >= 0.01);                       // never below one qty_step
        closed += t.qty;
    }
    // The three legs add up to the whole 99%-of-equity opening (the single-leg
    // twin of this shape books 1268786.15 units in one trade).
    CHECK(near(closed, 1268786.15, 1e-6));
    CHECK(near(host.get_trade(0).qty, 507514.46, 1e-6));
    CHECK(near(host.get_trade(1).qty, 634393.07, 1e-6));
    CHECK(near(host.get_trade(2).qty, 126878.62, 1e-6));
}

// ---------------------------------------------------------------------------
// Unit 2 -- probe (b): two pyramided lots on the BTCUSDT lane facts stay TWO
// lots and both close in full on the exit bar.
// ---------------------------------------------------------------------------
void test_two_pyramided_lots_stay_two_lots_and_both_close() {
    Shape s;
    s.capital = 10000.0;
    s.explicit_qty = 0.00583;    // the owner's #11/#12 lot size
    s.pyramiding = 2;
    s.process_orders_on_close = true;
    s.entry_bars = 2;            // the 13:15 and 13:30 bars each open one lot
    s.first_entry_bar = 1;
    s.exit_legs = 1;
    s.stop = 85197.91;           // the owner's 14:00 exit price
    s.limit = 86500.0;
    MarginLaneHost host(kBtcUsdt, s);
    const auto bars = btcusdt_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    std::printf("trades=%d lots=%d live=%.17g\n", host.trade_count(),
                static_cast<int>(host.physical_position().lot_count),
                host.live_position_size());
    for (int i = 0; i < host.trade_count(); ++i) dump("  btc", host.get_trade(i));

    // Orders process on the close, so the two lots are both in the book in the
    // 14:00 script view (the bar before the stop trades) and the exit bar's own
    // view already carries that bar's fill.
    CHECK(host.lots_at(2) == 1);
    CHECK(host.lots_at(3) == 2);
    for (int i = 0; i < static_cast<int>(bars.size()); ++i)
        CHECK(host.lots_at(i) <= 2);   // the 1x-margin slice never fragments them
    CHECK(host.trade_count() == 2);
    CHECK(host.live_position_size() == 0.0);
    CHECK(host.physical_position().signed_units == 0.0);
    CHECK(host.physical_position().lot_count == 0);
    if (host.trade_count() != 2) return;

    const double entry_prices[] = {84628.23, 85470.15};
    for (int i = 0; i < 2; ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(t.is_long);
        CHECK(!t.open_at_end);
        CHECK(t.entry_id == "Long");
        CHECK(t.exit_id == "X");
        CHECK(t.exit_from_bracket);
        CHECK(t.entry_incarnation == static_cast<std::uint64_t>(i + 1));
        CHECK(t.entry_time == bars[static_cast<std::size_t>(i) + 1].timestamp);
        CHECK(near(t.entry_price, entry_prices[i], 1e-9));
        CHECK(t.exit_time == bars[4].timestamp);
        CHECK(near(t.exit_price, 85197.91, 1e-9));
        // Bitwise: the owner books the WHOLE lot (pine_fills.cpp:1618 spells
        // the coverage `qty - kQtyEpsilon`), so a leg that closes a lot in full
        // carries that lot's own quantity, not a re-quantized binary64 residual
        // one ULP short of it (the pre-fix second lot closed 0.0058299999999999984).
        CHECK(t.qty == s.explicit_qty);
    }
    for (int i = 5; i < 6; ++i) CHECK(host.position_at(i) == 0.0);
}

// ---------------------------------------------------------------------------
// Unit 3 -- probe (b)'s ladder twin: one lot closed by a 40/50/100 ladder on
// the BTCUSDT lane facts must flatten on the exit bar instead of stranding its
// residual leg open to the range end.
// ---------------------------------------------------------------------------
void test_ladder_exit_flattens_the_whole_lot() {
    Shape s;
    s.capital = 10000.0;
    s.qty_percent = 10.0;
    s.pyramiding = 1;
    s.process_orders_on_close = true;
    s.entry_bars = 1;
    s.first_entry_bar = 2;
    s.exit_legs = 3;
    s.stop = 85197.91;
    s.limit = 86500.0;
    s.tp2 = 87500.0;
    s.tp3 = 88500.0;
    MarginLaneHost host(kBtcUsdt, s);
    const auto bars = btcusdt_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    std::printf("trades=%d lots=%d live=%.17g\n", host.trade_count(),
                static_cast<int>(host.physical_position().lot_count),
                host.live_position_size());
    for (int i = 0; i < host.trade_count(); ++i) dump("  ladder", host.get_trade(i));

    for (int i = 0; i < static_cast<int>(bars.size()); ++i)
        CHECK(host.lots_at(i) <= 1);   // one lot stays one lot
    CHECK(host.trade_count() == 3);
    CHECK(host.live_position_size() == 0.0);
    CHECK(host.physical_position().signed_units == 0.0);
    CHECK(host.physical_position().lot_count == 0);
    if (host.trade_count() != 3) return;

    double closed = 0.0;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(t.is_long);
        CHECK(!t.open_at_end);         // the pre-fix residual leg never exited
        CHECK(t.entry_id == "Long");
        CHECK(t.exit_from_bracket);
        CHECK(t.entry_time == bars[2].timestamp);
        CHECK(near(t.entry_price, 85470.15, 1e-9));
        CHECK(t.exit_time == bars[4].timestamp);
        CHECK(near(t.exit_price, 85197.91, 1e-9));
        CHECK(t.qty >= 1e-5);          // never below one qty_step
        closed += t.qty;
    }
    CHECK(near(closed, 0.01169, 1e-12));   // the whole 10%-of-equity opening
    CHECK(near(host.get_trade(0).qty, 0.00467, 1e-12));
    CHECK(near(host.get_trade(1).qty, 0.00584, 1e-12));
    CHECK(near(host.get_trade(2).qty, 0.00118, 1e-12));
    CHECK(host.position_at(5) == 0.0);
}

}  // namespace

int main() {
    test_full_percent_short_is_flat_after_its_full_exit();
    test_two_pyramided_lots_stay_two_lots_and_both_close();
    test_ladder_exit_flattens_the_whole_lot();
    std::printf("test_l10aj_margin_full_exit_flat: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
