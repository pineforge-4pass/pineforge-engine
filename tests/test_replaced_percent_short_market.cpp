// R18 covered TV controls (OANDA:XAUUSD, 2025-04-01..2026-05-01):
// same-id default-percent short replacement is a plain sell transaction.
// Seed 4.54, First 4.53 => 0.01 LONG; seed 10 => 5.47 LONG;
// seed 3 => 1.53 SHORT under First; equal quantities => FLAT. A later
// same-direction MARKET does not fill. The old long bracket stays dormant
// until reissued. The buy-side mirror is deliberately outside this fix.
// Small synthetic unit bars below scale that arithmetic to 3 - 2 = 1.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;
namespace {
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
int failed = 0;
int passed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; \
    std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a-b) < 1e-9; }

class Probe : public pineforge::source::PineStrategyHost {
public:
    double seed_qty = 3;
    int calls = 2;
    bool sibling = true, child = true, last_child = true;
    bool mirror = false, revive = false, default_seed = false;
    bool long_only_at_race = false;
    bool replace_after_sibling = false, explicit_qty = false;
    bool priced_first = false, cancel_first = false, reenter = false;
    int issued_calls = 0;
    struct State { PositionSide side; double qty; std::string id; size_t closed; };
    std::vector<State> seen;
    std::vector<Trade> closed;
    Probe() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 2;
        pyramiding_ = 1;
        commission_value_ = 0;
        slippage_ = 0;
        qty_step_ = 0.01;
    }
    void percent(double value) { default_qty_value_ = value; }
    void first() {
        if (cancel_first && issued_calls == 1) strategy_cancel("First");
        strategy_entry("First", mirror, nan,
            priced_first && issued_calls == 0 ? 90 : nan,
            explicit_qty ? 2 : nan, "");
        ++issued_calls;
        if (child) strategy_exit("First exit", "First", mirror ? 120 : 80,
            mirror ? 80 : 120, nan, nan, nan, 100, "", nan, "");
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            issued_calls = 0;
            strategy_entry("Seed", !mirror, nan, nan,
                default_seed ? nan : seed_qty, "");
            strategy_exit("Seed exit", "Seed", mirror ? 80 : 110,
                mirror ? 120 : 80, nan, nan, nan, 100, "", nan, "");
        }
        if (bar_index_ == 2) {
            if (long_only_at_race) risk_direction_ = RiskDirection::LONG_ONLY;
            for (int i=0; i<(replace_after_sibling ? 1 : calls); ++i) first();
            if (sibling) {
                strategy_entry("Last", mirror, nan, nan, nan, "");
                if (last_child) strategy_exit("Last exit", "Last", mirror ? 125 : 75,
                    mirror ? 75 : 125, nan, nan, nan, 100, "", nan, "");
            }
            if (replace_after_sibling) for (int i=1;i<calls;++i) first();
        }
        if (bar_index_ == 4 && reenter)
            strategy_entry("Last", false, nan, nan, nan, "");
        if (bar_index_ == 5 && revive) {
            strategy_exit("Seed exit", "Seed", 110, 80,
                nan, nan, nan, 100, "", nan, "");
        }
        seen.push_back({position_side_,position_qty_,
            pyramid_entries_.empty() ? "" : pyramid_entries_.front().entry_id,
            trades_.size()});
        closed = trades_;
    }
};
std::vector<Bar> feed(bool touch = false) {
    std::vector<Bar> bars(8);
    for (int i=0;i<8;++i) bars[i] = {
        100,100.5,99.5,100,1000,(i+1)*900000LL};
    if (touch) { bars[4].high=112; bars[6].high=112; }
    return bars;
}
void run(Probe& p,bool touch=false) {
    p.seen.clear(); p.closed.clear();
    const auto bars=feed(touch); p.run(bars.data(),static_cast<int>(bars.size()));
}
void test_partial_and_topology() {
    for (int variant=0;variant<6;++variant) {
        Probe p;
        p.sibling=variant!=1; p.child=variant!=2;
        p.calls=variant==3 ? 3 : 2;
        p.replace_after_sibling=variant==4;
        p.last_child=variant!=5;
        run(p); run(p);  // reuse must not carry a cancelled sibling marker
        CHECK(p.seen[3].side==PositionSide::LONG);
        CHECK(near(p.seen[3].qty,1));
        CHECK(p.seen[3].id=="Seed");
        CHECK(p.closed.size()==1);
        if (p.closed.size()==1) {
            CHECK(near(p.closed[0].qty,2));
            CHECK(p.closed[0].entry_id=="Seed");
            CHECK(p.closed[0].exit_id=="First");
        }
    }
}
void test_equal_and_crossing() {
    Probe equal; equal.seed_qty=2; run(equal);
    CHECK(equal.seen[3].side==PositionSide::FLAT);
    CHECK(near(equal.seen[3].qty,0));
    CHECK(equal.closed.size()==1);
    Probe cross; cross.seed_qty=1; run(cross);
    CHECK(cross.seen[3].side==PositionSide::SHORT);
    CHECK(near(cross.seen[3].qty,1));
    CHECK(cross.seen[3].id=="First");
    CHECK(cross.closed.size()==1);
    Probe tiny; tiny.seed_qty=2.01; run(tiny);
    CHECK(tiny.seen[3].side==PositionSide::LONG);
    CHECK(near(tiny.seen[3].qty,0.01));
}
void test_old_bracket_lifetime() {
    Probe p; p.revive=true; run(p,true);
    CHECK(p.seen[4].side==PositionSide::LONG);
    CHECK(near(p.seen[4].qty,1));
    CHECK(p.seen[6].side==PositionSide::FLAT);
    CHECK(p.closed.size()==2);
    if (p.closed.size()==2) {
        CHECK(p.closed[1].entry_id=="Seed");
        CHECK(p.closed[1].exit_id=="Seed exit");
        CHECK(near(p.closed[1].qty,1));
        CHECK(near(p.closed[1].exit_price,110));
    }
}
void test_default_seed_and_high_percent() {
    Probe p; p.default_seed=true;
    auto bars=feed();
    for (int i=0;i<2;++i) bars[i]={99,99.5,98.5,99,1000,(i+1)*900000LL};
    p.run(bars.data(),static_cast<int>(bars.size()));
    CHECK(near(p.seen[1].qty,2.02));
    CHECK(p.seen[3].side==PositionSide::LONG);
    CHECK(near(p.seen[3].qty,0.02));
    CHECK(p.closed.size()==1);
    if (p.closed.size()==1) CHECK(near(p.closed[0].qty,2));
    // Reversal admission has held=0 (only SAME-direction adds reserve the
    // held margin), so a funded 75/99-percent sell is not declined at 50%.
    for (double pct : {51.0,75.0,99.0}) {
        Probe high; high.percent(pct); run(high);
        CHECK(high.seen[3].side==PositionSide::SHORT);
        CHECK(near(high.seen[3].qty,pct-3));
        CHECK(high.seen[3].id=="First");
        CHECK(high.closed.size()==1);
    }
}
void test_direction_risk_exclusion() {
    Probe p; p.seed_qty=1; p.long_only_at_race=true; run(p);
    CHECK(p.seen[3].side==PositionSide::FLAT);
    CHECK(near(p.seen[3].qty,0));
    CHECK(p.closed.size()==1);
    if (p.closed.size()==1) CHECK(near(p.closed[0].qty,1));
}
// Preserve the existing engine lanes that this narrow sell-side repair
// does not claim to redefine. The old same-tick suite pins their details.
void test_excluded_lanes() {
    Probe single; single.calls=1; single.sibling=false; run(single);
    CHECK(single.seen[3].side==PositionSide::SHORT);
    CHECK(near(single.seen[3].qty,2));
    CHECK(single.seen[3].id=="First");
    Probe mirror; mirror.mirror=true; run(mirror);
    CHECK(mirror.seen[3].side==PositionSide::LONG);
    CHECK(near(mirror.seen[3].qty,2));
    CHECK(mirror.seen[3].id=="Last");
    Probe explicit_order; explicit_order.explicit_qty=true;
    explicit_order.sibling=false; run(explicit_order);
    CHECK(explicit_order.seen[3].side==PositionSide::SHORT);
    CHECK(near(explicit_order.seen[3].qty,2));
    for (bool priced : {false,true}) {
        Probe replaced; replaced.sibling=false;
        replaced.priced_first=priced; replaced.cancel_first=!priced;
        run(replaced);
        CHECK(replaced.seen[3].side==PositionSide::SHORT);
        CHECK(near(replaced.seen[3].qty,2));
    }
    Probe fresh; fresh.reenter=true; run(fresh);
    CHECK(fresh.seen[3].side==PositionSide::LONG);
    CHECK(fresh.seen[5].side==PositionSide::SHORT);
    CHECK(near(fresh.seen[5].qty,2));
}
}
int main() {
    test_partial_and_topology(); test_equal_and_crossing();
    test_old_bracket_lifetime(); test_excluded_lanes();
    test_default_seed_and_high_percent(); test_direction_risk_exclusion();
    std::printf("%d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
}
