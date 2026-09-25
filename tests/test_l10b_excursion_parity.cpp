// R4-D L10b: per-trade favorable/adverse excursion (Trade.max_runup /
// max_drawdown) on a margin-call bar matches the legacy owner (ab9714be
// pine_scheduler.cpp: process_pending_orders -> update_per_trade_extremes ->
// process_margin_call). Literals are that owner's output for the same
// excerpt: corpus/anomaly-equity-mirror-strategy-equity-01 trade #3
// (lab trades diff exp-ci-preflight-0679210-20260913 vs
// exp-native-r4-d-aaa2c3a-20260916) and the short + percent-of-equity
// same-bar shape taken from rootCauses.H16.rows
// (data/alpha-wizard-channel-volume-profil trade #1).
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

bool near(double a, double b, double tol = 1e-8) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(double cap, int qtype, double qval, int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = cap;
    c.default_qty_type = qtype;
    c.default_qty_value = qval;
    c.pyramiding = pyr;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.commission_type = (int)CommissionType::PERCENT;
    c.slippage = 0;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

// 04-21 ETH 15m excerpt (lab bars BINANCE:ETHUSDT.P 15 around 2025-04-21
// 00:15). Bar 1 is the Monday 00:00Z signal; bar 2 fills the long at the
// 00:15Z open; bar 3 is the 00:30Z money-residual margin-call bar
// (low-first: |H-O|=10.90 > |O-L|=2.79).
std::vector<Bar> bars_0421() {
    return {
        mk(1745192700000LL, 1583.8, 1587, 1583.49, 1586.56),
        mk(1745193600000LL, 1586.57, 1593.75, 1585.28, 1592.52),
        mk(1745194500000LL, 1592.52, 1613.8, 1592.52, 1608.96),
        mk(1745195400000LL, 1608.96, 1619.86, 1606.17, 1613.78),
        mk(1745196300000LL, 1613.78, 1620, 1608.08, 1609.49),
        mk(1745197200000LL, 1609.5, 1618, 1607.26, 1610.81),
    };
}

// 10:45 ETH 15m excerpt from H16 row data/alpha-wizard-channel-volume-profil
// trade #1 (lab bars around 2025-04-07 10:45). High-first
// (|H-O|=2.18 < |O-L|=13.35): short at the open, liquidation at the high.
std::vector<Bar> bars_h16_short() {
    return {
        mk(1744020900000LL, 1505.62, 1513.62, 1487.09, 1501.56),
        mk(1744021800000LL, 1501.56, 1503.74, 1488.21, 1490.7),
        mk(1744022700000LL, 1490.71, 1503.2, 1489.69, 1493.13),
    };
}

class MirrorLong : public source::PineStrategyHost {
public:
    explicit MirrorLong(double capital, double qty) : qty_(qty) {
        configure_pine_strategy(cfg(capital, (int)QtyType::FIXED, 1.0, 0));
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 && live_position_size() == 0.0) {
            strategy_entry("E", true, kNaN, kNaN, qty_, "qty = equity/close");
        }
        if (live_position_size() > 0.0 && pine_bar_index() > entry_bar_) {
            if (entry_bar_ < 0) entry_bar_ = pine_bar_index();
            if (pine_bar_index() > entry_bar_) strategy_close("E", "next-bar flatten");
        }
    }
private:
    double qty_;
    int entry_bar_ = -1;
};

class ShortPct : public source::PineStrategyHost {
public:
    explicit ShortPct(double capital) {
        configure_pine_strategy(
            cfg(capital, (int)QtyType::PERCENT_OF_EQUITY, 100.0, 0));
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0 && live_position_size() == 0.0)
            strategy_entry("S", false);
        if (pine_bar_index() == 2) strategy_close_all();
    }
};

void expect_trade(const char* tag, const Trade& t,
                  const char* entry, bool is_long,
                  double qty, double entry_px, double exit_px,
                  double fav, double adv, double pnl) {
    std::printf("%s %s qty=%.4f %s@%.2f->%s@%.2f pnl=%.4f mfe=%.4f mae=%.4f comment=%s\n",
                tag, is_long ? "long" : "short", t.qty,
                t.entry_id.c_str(), t.entry_price,
                t.exit_id.c_str(), t.exit_price,
                t.pnl, t.max_runup, t.max_drawdown,
                t.exit_comment.c_str());
    CHECK(t.entry_id == entry);
    CHECK(t.is_long == is_long);
    CHECK(near(t.qty, qty, 1e-6));
    CHECK(near(t.entry_price, entry_px, 1e-6));
    CHECK(near(t.exit_price, exit_px, 1e-6));
    CHECK(near(t.pnl, pnl, 1e-6));
    CHECK(near(t.max_runup, fav, 1e-6));
    CHECK(near(t.max_drawdown, adv, 1e-6));
}

} // namespace

int main() {
    {
        // Probe ledger 04-21: E 992399.54089, Q 623.163 @1592.52.
        // Legacy trade #3: Margin call 1 @1606.17, favorableUsd 27.34
        // (exit-bar high 1619.86 - 1592.52), adverseUsd 0. Native without
        // the full-bar pre-margin sample reports 21.28 (entry-bar high only).
        // expectation corrected (R5 lane H-THIN, E19: the kernel samples the Pine
        // host's lots; the host-owned model is gone):
        // 27.34 -> 21.28, which is TradingView's own number for this row
        // (corpus tape anomaly-equity-mirror-strategy-equity-01 trade #3:
        // Favorable excursion 21.28, Adverse 0); ab9714be's 27.34 folded an
        // extreme after the liquidation.
        MirrorLong host(992399.54089, 623.163);
        auto bars = bars_0421();
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 2);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            CHECK(t.exit_comment == std::string("Margin call"));
            expect_trade("0421-mc", t, "E", true, 1.0, 1592.52, 1606.17,
                         21.28, 0.0, 13.65);
        }
        if (host.trade_count() >= 2) {
            const auto& t = host.get_trade(1);
            CHECK(t.exit_comment == std::string("next-bar flatten"));
            CHECK(near(t.qty, 622.163, 1e-6));
            CHECK(near(t.exit_price, 1613.78, 1e-6));
        }
        CHECK(near(host.live_position_size(), 0.0));
    }
    {
        // H16 short + percent-of-equity: 100% short at 1501.56 on a
        // high-first bar whose high (1503.74) is the liquidation print.
        // Legacy samples the bar low (1488.21) before process_margin_call
        // and scales that extreme to the closed slice (ab9714be output
        // of this excerpt: mfe=(1501.56-1488.21)*qty).
        // expectation corrected (R5 lane H-THIN, E19: the kernel samples the Pine
        // host's lots; the host-owned model is gone):
        // the high-first walk reaches the liquidation at the high before the
        // 1488.21 low, so no favorable price belongs to the slice (mfe 0,
        // was (1501.56-1488.21)*qty); TradingView folds no extreme after a
        // closing fill (tests/test_e19_excursion_tape.cpp, class B, and the
        // row above). No tape of this excerpt exists.
        const double capital = 7.7232 * 1501.56;
        ShortPct host(capital);
        auto bars = bars_h16_short();
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() >= 1);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            const double fav = 0.0;
            const double adv = (1503.74 - 1501.56) * t.qty;
            CHECK(t.exit_comment == std::string("Margin call"));
            expect_trade("h16-short", t, "S", false, t.qty, 1501.56, 1503.74,
                         fav, adv, -adv);
            CHECK(t.qty > 0.0);
        }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
