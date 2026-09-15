// Round15 JOAT: five covered TradingView panels from r14-joat-audit.
// F carry/fresh CSV 5eff7824, high control 2308af1a; EUR carry f0cce2d5,
// high control 69197a4b. Six synthetic bars retain the two relevant OHLC
// legs and explicit quantities, without loading or replaying a strategy/feed.
// A MARKET reentry at W1=H may place a marketable full long limit. It waits
// through H->L, then fills at the limit on the L->C recross. An earlier
// terminal-W2 entry (F) or a final leg that cannot recross (EUR) still rolls.
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
static int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { \
    ++failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
} } while (0)

namespace {
constexpr double N = std::numeric_limits<double>::quiet_NaN();
bool near(double a, double b) { return std::abs(a-b) < 1e-7; }
enum class Guard { None, RawParent, PricedParent, CompetingOrder,
                   DirectPartial, ReachableStop, LimitBelowLow };

class RecrossProbe : public pineforge::source::PineStrategyHost {
public:
    RecrossProbe(bool eur, bool high = false, bool fresh = false,
                 Guard guard = Guard::None)
        : eur_(eur), high_(high), fresh_(fresh), guard_(guard) {
        initial_capital_ = 100000;
        pyramiding_ = 0;
        margin_long_ = margin_short_ = 100;
        qty_step_ = eur ? .01 : 1;
        syminfo_.pointvalue = 1;
        set_syminfo_mintick(eur ? .00001 : .01);
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = .01;
        slippage_ = 0;
        calc_on_order_fills_ = true;
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trade_count() == 0)
            strategy_entry("L", true, N, N, eur_ ? 8389.91 : 840, "OLD");
        if (bar_index_ == 2 && position_side_ == PositionSide::FLAT
            && trade_count() == 1)
            strategy_entry("L", true, N, N, eur_ ? 8371.05 : 833, "REENTRY1");
        if (bar_index_ == 3 && position_side_ == PositionSide::FLAT
            && trade_count() == 2) {
            if (guard_ == Guard::RawParent)
                strategy_order("L", true, 832);
            else
                strategy_entry("L", true, N,
                    guard_ == Guard::PricedParent ? 12.04 : N,
                    eur_ ? 8369.44 : 832, "REENTRY2");
        }
        if (position_side_ == PositionSide::LONG) {
            const bool last = trade_count() >= 2;
            if (last && bar_index_ == 3 && guard_ == Guard::CompetingOrder)
                strategy_order("Far", false, 1, 20.0);
            if (last && trade_count() == 2 && bar_index_ == 3
                && guard_ == Guard::DirectPartial)
                strategy_close("L", "PARTIAL", 1, N, true);
            const double target = last && guard_ == Guard::LimitBelowLow ? 11.95
                : last && high_ ? (eur_ ? 1.175 : 12.20)
                : (eur_ ? 1.173199565095035 : 12.011758862989522);
            strategy_exit(last && fresh_ ? "FreshRisk" : "Risk", "L",
                target, last && guard_ == Guard::ReachableStop ? 12.00
                    : (eur_ ? 1.168520271815603 : 11.854525710631547),
                N, N, N, 100, "TP");
        }
        if (bar_index_ == 4) strategy_close("", "END");
    }
    uint64_t fills() const { return broker_fill_event_seq_; }
private:
    bool eur_, high_, fresh_;
    Guard guard_;
};

std::vector<Bar> bars(bool eur) {
    if (eur) return {
        {1.1703,1.1703,1.1703,1.1703,1,1000},
        {1.17033,1.171,1.1702,1.1705,1,2000},
        {1.17284,1.17322,1.17242,1.17318,1,3000},
        {1.17316,1.17342,1.1728,1.1734,1,4000},
        {1.1734,1.17461,1.17334,1.17454,1,5000},
        {1.17456,1.17508,1.17418,1.17418,1,6000},
    };
    return {
        {11.9,11.9,11.9,11.9,1,1000},
        {11.92,11.93,11.91,11.92,1,2000},
        {11.99,12.03,11.975,12.01,1,3000},
        {12.005,12.04,11.965,12.035,1,4000},
        {12.035,12.05,12.02,12.045,1,5000},
        {12.045,12.06,12.01,12.015,1,6000},
    };
}

void check_panel(bool eur, bool high = false, bool fresh = false) {
    RecrossProbe p(eur, high, fresh);
    const auto b = bars(eur);
    for (int repeat = 0; repeat < 2; ++repeat) {
        p.run(b.data(), b.size());
        CHECK(p.last_error().empty());
        CHECK(p.trade_count() == 3);
        CHECK(p.fills() == 6);
        if (p.trade_count() != 3) continue;
        const auto& first = p.get_trade(0);
        const auto& second = p.get_trade(1);
        const auto& last = p.get_trade(2);
        CHECK(first.entry_bar_index == 1 && first.exit_bar_index == 2);
        CHECK(second.entry_bar_index == 2 && second.exit_bar_index == 3);
        CHECK(near(first.entry_price, eur ? 1.17033 : 11.92));
        CHECK(near(first.exit_price, eur ? 1.1732 : 12.02));
        CHECK(near(second.entry_price, eur ? 1.17322 : 12.03));
        CHECK(near(second.exit_price, eur ? 1.1732 : 12.02));
        CHECK(near(last.entry_price, eur ? 1.17342 : 12.04));
        CHECK(last.entry_bar_index == 3);
        CHECK(near(last.qty, eur ? 8369.44 : 832));
        const double exit = high ? (eur ? 1.17456 : 12.05)
                                 : (eur ? 1.1732 : 12.02);
        CHECK(last.exit_bar_index == (high ? 5 : 3));
        CHECK(near(last.exit_price, exit));
        CHECK(last.exit_comment == (high ? "END" : "TP"));
        if (!high) CHECK(last.exit_id == (fresh ? "FreshRisk" : "Risk"));
        CHECK(near(last.commission, (last.entry_price + exit) * last.qty * .0001));
        CHECK(near(last.pnl, (exit - last.entry_price) * last.qty
                           - (last.entry_price + exit) * last.qty * .0001));
    }
}

// These are scope guards checked against both the unchanged parent runtime
// and this candidate, not claims that the related unpinned TV shapes are fixed.
void check_guards() {
    for (Guard g : {Guard::RawParent, Guard::PricedParent, Guard::CompetingOrder,
                    Guard::DirectPartial, Guard::ReachableStop, Guard::LimitBelowLow}) {
        RecrossProbe p(false, false, false, g);
        const auto b = bars(false);
        p.run(b.data(), b.size());
        CHECK(p.last_error().empty());
        const bool partial = g == Guard::DirectPartial;
        CHECK(p.trade_count() == (partial ? 4 : 3));
        CHECK(p.fills() == (partial ? 7 : 6));
        if (p.trade_count() != (partial ? 4 : 3)) continue;
        const auto& last = p.get_trade(partial ? 3 : 2);
        CHECK(last.entry_bar_index == 3);
        CHECK(near(last.entry_price, 12.04));
        CHECK(near(last.qty, partial ? 831 : 832));
        CHECK(last.exit_bar_index == (g == Guard::ReachableStop ? 3 : 4));
        CHECK(near(last.exit_price, g == Guard::ReachableStop ? 11.97 : 12.04));
        if (partial) {
            CHECK(near(p.get_trade(2).qty, 1));
            CHECK(p.get_trade(2).exit_bar_index == 3);
            CHECK(near(p.get_trade(2).exit_price, 12.04));
        }
    }
}
} // namespace

int main(int argc, char**) {
    if (argc == 1) {
        check_panel(false);
        check_panel(false, false, true);
        check_panel(false, true);
        check_panel(true);
        check_panel(true, true);
    }
    check_guards();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
