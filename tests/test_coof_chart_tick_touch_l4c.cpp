#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
/*
 * Round 14 JOAT: an older plain exit may touch the chart's outward-rounded
 * H/L tick even though the raw COOF segment did not reach its level.
 * Synthetic arrays preserve TV sensor prices; no feed/corpus is loaded.
 *
 * F15 May29: short979@10.22, SL10.257194001727152, rawH10.255 ->tick10.26.
 * COOF and ordinary both exit13:45@10.26 (CSV0e7b9a63fa0efd4fefe504152a0b2859e06688dfe1c27d8a265fdfb3bc5cdc25).
 * SL10.27 instead exits14:00 (CSV596d5d38f39a5fcba3fa23e2519e3dd74f531b4575194590c7430012c0a800a3).
 * LongLIMIT at samelevel also exits13:45@10.26 (CSV9198b9f17b99b48dcd61201c72d9e0da162369be96dca8dbf95ff28e6c122b83).
 * F1D Jan26: rawL13.3448 ->tick13.34 reaches13.342 longSTOP/shortLIMIT
 * (CSV6b50fba7e9b743481319fa9a0d3d23f1c6fe3cf380305fc755dba9e96b76223f /
 * f2342928abbcd2d8b613af4ba96a78e8dded5ca375b3ee3c94b6b94fe65c8373).
 * LongSTOP13.33 waits for END nextopen13.64 (CSV6ad6b01ca8d512460ed77f6efbc4feeaf603d94aa2abe402baf57879b7691024).
 * All seven tapes are covered and actual intended seed quantities checked.
 */
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
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
bool near(double a, double b) { return std::abs(a-b) < 1e-8; }

class TickProbe : public pineforge::source::PineStrategyHost {
public:
    TickProbe(bool is_long, double qty, double stop, double limit,
              int end_bar, bool coof = true)
        : long_(is_long), qty_(qty), stop_(stop), limit_(limit), end_(end_bar) {
        initial_capital_ = 100000.0;
        margin_long_ = margin_short_ = 100.0;
        pyramiding_ = 0;
        qty_step_ = 1.0;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(.01);
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = .01;
        slippage_ = 0;
        calc_on_order_fills_ = coof;
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT)
            strategy_entry("E", long_, kNaN, kNaN, qty_, "SEED");
        if (position_side_ != PositionSide::FLAT) {
            strategy_exit("X", "E", limit_, stop_, kNaN, kNaN, kNaN, 100, "X");
            if (extra_order_)
                strategy_order("Idle", true, 1, kNaN, 1000.0);
        }
        if (trade_count() == 1) {
            ++observed_after_exit;
            exit_seen_bar = bar_index_;
            last_seen_raw_high = bar.high;
            last_seen_raw_low = bar.low;
        }
        if (bar_index_ == end_) strategy_close("", "END");
    }
    void extra_order() { extra_order_ = true; }
    const std::vector<Trade>& rows() const { return trades_; }
    double remaining() const { return position_qty_; }
    int observed_after_exit = 0;
    int exit_seen_bar = -1;
    double last_seen_raw_high = kNaN, last_seen_raw_low = kNaN;
private:
    bool long_;
    double qty_, stop_, limit_;
    int end_;
    bool extra_order_ = false;
};

std::vector<Bar> may_bars() {
    return {
        {10.205,10.205,10.17,10.18,1,1000},
        {10.215,10.22,10.17,10.19,1,2000},
        {10.185,10.255,10.185,10.25,1,3000},
        {10.255,10.275,10.22,10.235,1,4000},
        {10.23,10.23,10.2,10.21,1,5000},
    };
}
std::vector<Bar> jan_bars() {
    return {
        {13.405,13.77,13.405,13.77,1,1000},
        {13.78,13.84,13.7,13.71,1,2000},
        {13.7,13.7,13.55,13.56,1,3000},
        {13.56,13.655,13.3448,13.44,1,4000},
        {13.64,13.945,13.51,13.93,1,5000},
    };
}
void check(TickProbe& e, const std::vector<Bar>& bars, double qty,
           double entry, int64_t exit_time, double exit, const char* comment) {
    e.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(e.last_error().empty());
    CHECK(near(e.remaining(),0.0));
    CHECK(e.rows().size()==1);
    if (e.rows().size()!=1) return;
    const auto& t=e.rows()[0];
    CHECK(t.entry_time==2000);
    CHECK(near(t.entry_price,entry));
    CHECK(near(t.qty,qty));
    CHECK(t.exit_time==exit_time);
    CHECK(near(t.exit_price,exit));
    CHECK(t.exit_comment==comment);
    const double direction=t.is_long?1:-1;
    CHECK(near(t.pnl,direction*(exit-entry)*qty-(entry+exit)*qty*.0001));
}

void upper_controls() {
    for (bool coof : {false,true}) {
        TickProbe short_stop(false,979,10.257194001727152,kNaN,3,coof);
        check(short_stop,may_bars(),979,10.22,3000,10.26,"X");
    }
    TickProbe long_limit(true,979,kNaN,10.257194001727152,3);
    check(long_limit,may_bars(),979,10.22,3000,10.26,"X");
    TickProbe next_tick(false,979,10.27,kNaN,3);
    check(next_tick,may_bars(),979,10.22,4000,10.27,"X");
}
void lower_controls() {
    TickProbe stop(true,100,13.342,kNaN,3);
    check(stop,jan_bars(),100,13.78,4000,13.34,"X");
    TickProbe limit(false,100,kNaN,13.342,3);
    check(limit,jan_bars(),100,13.78,4000,13.34,"X");
    TickProbe next_tick(true,100,13.33,kNaN,3);
    check(next_tick,jan_bars(),100,13.78,5000,13.64,"END");
}
void narrow_scope_and_liveness() {
    // Scope compatibility: a competing pending order does not gain a new
    // ranking interaction through this single-exit fallback.
    TickProbe competing(false,979,10.257194001727152,kNaN,3);
    competing.extra_order();
    check(competing,may_bars(),979,10.22,4000,10.26,"X");

    TickProbe e(false,979,10.257194001727152,kNaN,3);
    auto input=may_bars();
    input.resize(3); // final bar is the raw extreme that causes the new fill
    check(e,input,979,10.22,3000,10.26,"X");
    CHECK(e.observed_after_exit==2); // fill recalc then ordinary close
    CHECK(e.exit_seen_bar==2);
    CHECK(near(e.last_seen_raw_high,10.255));
    CHECK(near(e.last_seen_raw_low,10.185));
    // Reusing the handle has no persisted chart-boundary or cursor state.
    e.observed_after_exit=0;
    check(e,input,979,10.22,3000,10.26,"X");
    CHECK(e.observed_after_exit==2);
}
}
int main() {
    upper_controls();
    lower_controls();
    narrow_scope_and_liveness();
    std::printf("%d passed, %d failed\n",passed,failed);
    return failed==0?0:1;
}
