/*
 * Round 13 taro BTC: nested price-scale admission at an actual gap fill.
 * TV tapes under state/r13-taro-audit and state/r13-taro-btc:
 * BTC July03 Es910872.3625532,Q8.31589,close109533.95,fill109533.96.
 * Offsets +.0001,0,-.0001,-.00030 admit; -.00032,-.00036,-.001 drop.
 * -.00032 distinguishes sig10(sig10(E)/Q) from sig10(E/Q).
 * ETH Apr01 Q10,close1821.47,fill1821.48: C18214.799997 admits
 * (MC1 at fill, remainder9 on either side); C18214.799994 drops.
 * Small synthetic fixtures preserve those prices and source calls. No
 * corpus/feed/strategy/verifier is loaded. Existing exact-affordable and
 * non-scope contracts must keep their old behavior.
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
#define CHECK(expr) do { if (expr) ++passed; else { \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failed; \
} } while (0)

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
bool near(double a, double b, double tol = 1e-7) {
    return std::abs(a-b) < tol;
}
struct Config {
    double capital = 910872.3625532;
    double step = 0.00001;
    double tick = 0.01;
    bool flat = true;
    bool seed_long = true;
    double seed_qty = 8.31589;
    bool is_long = false;
    int signal_bar = 2;
    bool explicit_qty = false;
    bool raw = false;
    double fee = 0.0;
    bool provider = false;
    bool pooc = false;
    bool coof = false;
    bool magnifier = false;
    bool close_first = false;
};
class Probe : public pineforge::source::PineStrategyHost {
public:
    explicit Probe(Config config) : cfg_(config) {
        initial_capital_ = config.capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = config.fee;
        margin_long_ = margin_short_ = 100;
        pyramiding_ = 1;
        slippage_ = 0;
        qty_step_ = config.step;
        syminfo_.pointvalue = 1;
        set_syminfo_mintick(config.tick);
        process_orders_on_close_ = config.pooc;
        calc_on_order_fills_ = config.coof;
        set_margin_call_enabled(true);
        if (config.provider) {
            const int64_t times[] = {1000};
            const double rates[] = {1};
            CHECK(set_account_currency_fx_series(times, rates, 1));
        }
    }
    void on_source_bar(const Bar&) override {
        if (!cfg_.flat && bar_index_ == 0)
            strategy_entry("Seed", cfg_.seed_long, kNaN, kNaN,
                           cfg_.seed_qty, "SEED");
        if (bar_index_ == cfg_.signal_bar) {
            if (cfg_.close_first) strategy_close("Seed");
            if (cfg_.raw)
                strategy_order("Next", cfg_.is_long, kNaN);
            else
                strategy_entry("Next", cfg_.is_long, kNaN, kNaN,
                               cfg_.explicit_qty ? 8.31589 : kNaN, "ENTRY");
        }
        if (bar_index_ == cfg_.signal_bar+1) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
    double position() const { return signed_position_size(); }
    bool magnifier() const { return cfg_.magnifier; }
private:
    Config cfg_;
};
std::vector<Bar> btc() {
    return {
        {109393.88,109393.88,109393.88,109393.88,1,1000},
        {109393.88,109547.32,109382.93,109547.17,1,2000},
        {109547.16,109580,109471.6,109533.95,1,3000},
        {109533.96,109533.96,109377.57,109377.57,1,4000},
        {109377.57,109377.57,109377.57,109377.57,1,5000},
        {109377.57,109377.57,109377.57,109377.57,1,6000},
    };
}
std::vector<Bar> eth() {
    return {
        {1821.47,1821.47,1821.47,1821.47,1,1000},
        {1821.48,1829.36,1820.11,1826.38,1,2000},
        {1826.37,1826.37,1826.37,1826.37,1,3000},
        {1826.37,1826.37,1826.37,1826.37,1,4000},
    };
}
void run(Probe& engine, const std::vector<Bar>& bars) {
    if (engine.magnifier())
        engine.run(bars.data(), static_cast<int>(bars.size()), "1", "1", true, 4,
                   MagnifierDistribution::ENDPOINTS);
    else
        engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_error().empty());
    CHECK(near(engine.position(),0));
}
void reversal_offsets() {
    struct Offset { double delta; bool admit; };
    const Offset cases[] = {{.0001,true},{0,true},{-.0001,true},
        {-.00030,true},{-.00032,false},{-.00036,false},{-.001,false}};
    for (const auto& c : cases) {
        Config cfg;
        cfg.flat = false;
        cfg.capital = 909707.5558409+c.delta;
        Probe engine(cfg);
        run(engine,btc());
        const auto& rows=engine.rows();
        CHECK(rows.size() == (c.admit ? 2u : 1u));
        if (rows.empty()) continue;
        CHECK(near(rows[0].qty,8.31589));
        CHECK(rows[0].exit_time == (c.admit ? 4000 : 5000));
        if (c.admit && rows.size()==2) {
            CHECK(near(rows[1].qty,8.31589));
            CHECK(near(rows[1].entry_price,109533.96));
            CHECK(rows[1].entry_time==4000);
            CHECK(rows[1].exit_time==5000);
        }
    }
}
void opening_trim(Config cfg, const std::vector<Bar>& bars,
                  double fill, double qty) {
    Probe engine(cfg);
    // Reuse the handle: a rescued-gap event cannot survive reset or replay.
    for (int repeat=0;repeat<2;++repeat) {
        run(engine,bars);
        const auto& rows=engine.rows();
        CHECK(rows.size()==2);
        if (rows.size()!=2) continue;
        CHECK(rows[0].exit_comment=="Margin call");
        CHECK(near(rows[0].qty,1));
        CHECK(near(rows[0].entry_price,fill));
        CHECK(near(rows[0].exit_price,fill));
        CHECK(rows[0].entry_time==rows[0].exit_time);
        CHECK(rows[0].entry_time==(cfg.signal_bar+2)*1000);
        CHECK(near(rows[1].qty,qty-1));
        CHECK(rows[1].exit_comment!="Margin call");
        CHECK(rows[1].exit_time==(cfg.signal_bar+3)*1000);
        CHECK(near(rows[0].qty+rows[1].qty,qty));
    }
}
void flat_and_eth_controls() {
    for (bool is_long : {false,true}) {
        Config cfg; cfg.is_long=is_long;
        opening_trim(cfg,btc(),109533.96,8.31589);
        cfg.capital=18214.799997;cfg.step=.0001;cfg.signal_bar=0;
        opening_trim(cfg,eth(),1821.48,10);
    }
    Config drop;drop.capital=18214.799994;drop.step=.0001;drop.signal_bar=0;
    Probe rejected(drop);run(rejected,eth());CHECK(rejected.rows().empty());

    // Exactly affordable flat Long stays exempt: no new fill trim.
    Config affordable;affordable.capital=910872.3626532;affordable.is_long=true;
    Probe covered(affordable);run(covered,btc());CHECK(covered.rows().size()==1);
    if (!covered.rows().empty()) {
        CHECK(covered.rows()[0].exit_comment!="Margin call");
        CHECK(near(covered.rows()[0].qty,8.31589));
    }
}
void opposite_reversal() {
    for (bool admit : {true,false}) {
        Config cfg;cfg.flat=false;cfg.seed_long=false;cfg.seed_qty=1;
        cfg.is_long=true;cfg.capital=admit?911012.4325532:911012.4322332;
        Probe engine(cfg);run(engine,btc());const auto& rows=engine.rows();
        CHECK(rows.size()==(admit?3u:1u));
        if (rows.empty()) continue;
        CHECK(near(rows[0].qty,1));
        CHECK(rows[0].exit_time==(admit?4000:5000));
        if (admit && rows.size()==3) {
            CHECK(rows[1].exit_comment=="Margin call");
            CHECK(near(rows[1].qty,1));
            CHECK(near(rows[1].exit_price,109533.96));
            CHECK(near(rows[2].qty,7.31589));
        }
    }
}
void scope_controls() {
    // These all miss the newly pinned scope and retain exact-cost decline.
    Config explicit_qty;explicit_qty.explicit_qty=true;
    Probe explicit_order(explicit_qty);run(explicit_order,btc());
    CHECK(explicit_order.rows().empty());
    Config provider;provider.provider=true;
    Probe converted(provider);run(converted,btc());CHECK(converted.rows().empty());
    Config commissioned;commissioned.fee=.000001;
    Probe fee(commissioned);run(fee,btc());CHECK(fee.rows().empty());
    Config continuous;continuous.step=0;
    Probe no_lot(continuous);run(no_lot,btc());CHECK(no_lot.rows().empty());
    Config raw;raw.flat=false;raw.capital=909707.5558409;raw.raw=true;
    Probe raw_close(raw);run(raw_close,btc());CHECK(raw_close.rows().size()==1);
    if (!raw_close.rows().empty()) CHECK(raw_close.rows()[0].exit_time==4000);
    Config coof;
    coof.coof=true;
    Probe recalc(coof);run(recalc,btc());CHECK(recalc.rows().empty());
    Config mag;
    mag.magnifier=true;
    Probe magnifier(mag);run(magnifier,btc());CHECK(magnifier.rows().empty());
    Config pooc;
    pooc.pooc=true;pooc.is_long=true;
    Probe at_close(pooc);run(at_close,btc());CHECK(at_close.rows().size()==1);
    if (!at_close.rows().empty()) CHECK(at_close.rows()[0].exit_comment!="Margin call");
    // An explicit source-order close-first pair keeps its existing bypass
    // of the reversal gap gate; it is not a price-band rescued reversal.
    Config cf;cf.flat=false;cf.capital=909707.5555209;cf.close_first=true;
    Probe close_first(cf);run(close_first,btc());CHECK(close_first.rows().size()==2);
    if (close_first.rows().size()==2) {
        CHECK(close_first.rows()[0].exit_time==4000);
        CHECK(close_first.rows()[1].entry_time==4000);
    }
}
void historical_eth_pins() {
    // famr3e-Ex010-04010000: actual all-in +1-tick gap decline remains.
    Config cfg;cfg.capital=999999.9634;cfg.is_long=true;cfg.step=.0001;
    cfg.signal_bar=0;
    Probe gap(cfg);run(gap,eth());CHECK(gap.rows().empty());

    // famr3e-Eh-03312315: actual zero-gap admit548.5884 stays unchanged.
    cfg.capital=999999.8514;
    Probe flat(cfg);
    const std::vector<Bar> bars={
        {1822.86,1822.86,1822.86,1822.86,1,1000},
        {1822.86,1822.86,1822.86,1822.86,1,2000},
        {1824.93,1824.93,1824.93,1824.93,1,3000},
        {1824.93,1824.93,1824.93,1824.93,1,4000}};
    run(flat,bars);CHECK(flat.rows().size()==1);
    if (!flat.rows().empty()) CHECK(near(flat.rows()[0].qty,548.5884));
}
} // namespace
int main() {
    reversal_offsets();flat_and_eth_controls();opposite_reversal();
    scope_controls();historical_eth_pins();
    std::printf("%d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
