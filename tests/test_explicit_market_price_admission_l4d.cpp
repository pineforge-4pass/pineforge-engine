// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Explicit MARKET admission: independent covered TradingView boundary controls.
// Fixtures pin observed decisions; the test does not recompute the price rule.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
using namespace pineforge;
namespace {
struct Case { const char* name; bool is_long; double capital, qty, signal, fill, fee; bool admitted; };
const Case cases[] = {
    {"short-fee-base", false, 1993833.36146004495, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"short-fee-funded", false, 1993833.36246004495, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"short-fee-less-lot", false, 1993833.36146004495, 1751896.45, 1.1381, 1.1381, 0.05, true},
    {"short-fee-more-lot", false, 1993833.36146004495, 1751896.47, 1.1381, 1.1381, 0.05, false},
    {"short-fee-gap-up", false, 1992747.1863208, 1751896.46, 1.13748, 1.13749, 0.05, false},
    {"short-fee-gap-down", false, 1992974.9328606, 1751896.46, 1.13761, 1.13760, 0.05, true},
    {"short-no-fee-base", false, 1993833.36146004495, 1751896.46, 1.1381, 1.1381, 0, false},
    {"short-no-fee-funded", false, 1993833.36246004495, 1751896.46, 1.1381, 1.1381, 0, false},
    {"short-no-fee-less-lot", false, 1993833.36146004495, 1751896.45, 1.1381, 1.1381, 0, true},
    {"short-no-fee-more-lot", false, 1993833.36146004495, 1751896.47, 1.1381, 1.1381, 0, false},
    {"short-no-fee-gap-up", false, 1992747.1863208, 1751896.46, 1.13748, 1.13749, 0, false},
    {"short-no-fee-gap-down", false, 1992974.9328606, 1751896.46, 1.13761, 1.13760, 0, true},
    {"long-fee-base", true, 1993833.36146004495, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"long-fee-funded", true, 1993833.36246004495, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"long-fee-less-lot", true, 1993833.36146004495, 1751896.45, 1.1381, 1.1381, 0.05, true},
    {"long-fee-more-lot", true, 1993833.36146004495, 1751896.47, 1.1381, 1.1381, 0.05, false},
    {"long-fee-gap-up", true, 1992747.1863208, 1751896.46, 1.13748, 1.13749, 0.05, false},
    {"long-fee-gap-down", true, 1992974.9328606, 1751896.46, 1.13761, 1.13760, 0.05, true},
    {"long-no-fee-base", true, 1993833.36146004495, 1751896.46, 1.1381, 1.1381, 0, false},
    {"long-no-fee-funded", true, 1993833.36246004495, 1751896.46, 1.1381, 1.1381, 0, false},
    {"long-no-fee-less-lot", true, 1993833.36146004495, 1751896.45, 1.1381, 1.1381, 0, true},
    {"long-no-fee-more-lot", true, 1993833.36146004495, 1751896.47, 1.1381, 1.1381, 0, false},
    {"long-no-fee-gap-up", true, 1992747.1863208, 1751896.46, 1.13748, 1.13749, 0, false},
    {"long-no-fee-gap-down", true, 1992974.9328606, 1751896.46, 1.13761, 1.13760, 0, true},
    {"short-fee-control-budget", false, 1993814.9874513869, 1751880.31, 1.1381, 1.1381, 0.05, true},
    {"short-fee-offset-00103", false, 1993833.36249004495, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"short-fee-offset-00105", false, 1993833.36251004495, 1751896.46, 1.1381, 1.1381, 0.05, true},
    {"short-fee-offset-002", false, 1993833.36346004495, 1751896.46, 1.1381, 1.1381, 0.05, true},
    {"short-fee-source-equity", false, 1993833.3614605318, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"short-no-fee-offset-00103", false, 1993833.36249004495, 1751896.46, 1.1381, 1.1381, 0, false},
    {"short-no-fee-offset-00105", false, 1993833.36251004495, 1751896.46, 1.1381, 1.1381, 0, true},
    {"short-no-fee-offset-002", false, 1993833.36346004495, 1751896.46, 1.1381, 1.1381, 0, true},
    {"short-no-fee-source-equity", false, 1993833.3614605318, 1751896.46, 1.1381, 1.1381, 0, false},
    {"long-fee-offset-00103", true, 1993833.36249004495, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"long-fee-offset-00105", true, 1993833.36251004495, 1751896.46, 1.1381, 1.1381, 0.05, true},
    {"long-fee-offset-002", true, 1993833.36346004495, 1751896.46, 1.1381, 1.1381, 0.05, true},
    {"long-fee-source-equity", true, 1993833.3614605318, 1751896.46, 1.1381, 1.1381, 0.05, false},
    {"long-no-fee-offset-00103", true, 1993833.36249004495, 1751896.46, 1.1381, 1.1381, 0, false},
    {"long-no-fee-offset-00105", true, 1993833.36251004495, 1751896.46, 1.1381, 1.1381, 0, true},
    {"long-no-fee-offset-002", true, 1993833.36346004495, 1751896.46, 1.1381, 1.1381, 0, true},
    {"long-no-fee-source-equity", true, 1993833.3614605318, 1751896.46, 1.1381, 1.1381, 0, false},
    {"fill-price-controls-long-fee0-q46", true, 1993833.36146004495, 1751896.46, 1.13810, 1.13807, 0, false},
    {"fill-price-controls-long-fee0-q47", true, 1993833.36146004495, 1751896.47, 1.13810, 1.13807, 0, false},
    {"fill-price-controls-long-fee0p05-q46", true, 1993833.36146004495, 1751896.46, 1.13810, 1.13807, 0.05, false},
    {"fill-price-controls-long-fee0p05-q47", true, 1993833.36146004495, 1751896.47, 1.13810, 1.13807, 0.05, false},
    {"fill-price-controls-short-fee0-q46", false, 1993833.36146004495, 1751896.46, 1.13810, 1.13807, 0, false},
    {"fill-price-controls-short-fee0-q47", false, 1993833.36146004495, 1751896.47, 1.13810, 1.13807, 0, false},
    {"fill-price-controls-short-fee0p05-q46", false, 1993833.36146004495, 1751896.46, 1.13810, 1.13807, 0.05, false},
    {"fill-price-controls-short-fee0p05-q47", false, 1993833.36146004495, 1751896.47, 1.13810, 1.13807, 0.05, false},
    {"rounded-cost-controls-long-fee0-base", true, 1962090.3636613733, 1728820.60, 1.13493, 1.13493, 0, false},
    {"rounded-cost-controls-long-fee0-below", true, 1962090.3639, 1728820.60, 1.13493, 1.13493, 0, false},
    {"rounded-cost-controls-long-fee0-equal", true, 1962090.364, 1728820.60, 1.13493, 1.13493, 0, true},
    {"rounded-cost-controls-long-fee0-above", true, 1962090.3641, 1728820.60, 1.13493, 1.13493, 0, true},
    {"rounded-cost-controls-long-fee0-less-lot", true, 1962090.3636613733, 1728820.59, 1.13493, 1.13493, 0, true},
    {"rounded-cost-controls-long-fee0-more-lot", true, 1962090.3636613733, 1728820.61, 1.13493, 1.13493, 0, false},
    {"rounded-cost-controls-long-fee0p05-base", true, 1962090.3636613733, 1728820.60, 1.13493, 1.13493, 0.05, false},
    {"rounded-cost-controls-long-fee0p05-below", true, 1962090.3639, 1728820.60, 1.13493, 1.13493, 0.05, false},
    {"rounded-cost-controls-long-fee0p05-equal", true, 1962090.364, 1728820.60, 1.13493, 1.13493, 0.05, true},
    {"rounded-cost-controls-long-fee0p05-above", true, 1962090.3641, 1728820.60, 1.13493, 1.13493, 0.05, true},
    {"rounded-cost-controls-long-fee0p05-less-lot", true, 1962090.3636613733, 1728820.59, 1.13493, 1.13493, 0.05, true},
    {"rounded-cost-controls-long-fee0p05-more-lot", true, 1962090.3636613733, 1728820.61, 1.13493, 1.13493, 0.05, false},
    {"rounded-cost-controls-short-fee0-base", false, 1962090.3636613733, 1728820.60, 1.13493, 1.13493, 0, false},
    {"rounded-cost-controls-short-fee0-below", false, 1962090.3639, 1728820.60, 1.13493, 1.13493, 0, false},
    {"rounded-cost-controls-short-fee0-equal", false, 1962090.364, 1728820.60, 1.13493, 1.13493, 0, true},
    {"rounded-cost-controls-short-fee0-above", false, 1962090.3641, 1728820.60, 1.13493, 1.13493, 0, true},
    {"rounded-cost-controls-short-fee0-less-lot", false, 1962090.3636613733, 1728820.59, 1.13493, 1.13493, 0, true},
    {"rounded-cost-controls-short-fee0-more-lot", false, 1962090.3636613733, 1728820.61, 1.13493, 1.13493, 0, false},
    {"rounded-cost-controls-short-fee0p05-base", false, 1962090.3636613733, 1728820.60, 1.13493, 1.13493, 0.05, false},
    {"rounded-cost-controls-short-fee0p05-below", false, 1962090.3639, 1728820.60, 1.13493, 1.13493, 0.05, false},
    {"rounded-cost-controls-short-fee0p05-equal", false, 1962090.364, 1728820.60, 1.13493, 1.13493, 0.05, true},
    {"rounded-cost-controls-short-fee0p05-above", false, 1962090.3641, 1728820.60, 1.13493, 1.13493, 0.05, true},
    {"rounded-cost-controls-short-fee0p05-less-lot", false, 1962090.3636613733, 1728820.59, 1.13493, 1.13493, 0.05, true},
    {"rounded-cost-controls-short-fee0p05-more-lot", false, 1962090.3636613733, 1728820.61, 1.13493, 1.13493, 0.05, false},
};
int passed = 0, failed = 0;
int current_defaults = -1;
void check(bool value, const char* name, const char* property) {
    if (value) ++passed;
    else { ++failed; std::printf("FAIL %s default=%d: %s\n", name, current_defaults, property); }
}
class Probe : public pineforge::source::PineStrategyHost {
    const Case& fixture_;
public:
    double admitted_qty = 0;
    double balance_on_signal = 0;
    std::size_t pending_after_fill = 0;
    explicit Probe(const Case& f, QtyType defaults, double margin = 100) : fixture_(f) {
        initial_capital_ = f.capital;
        default_qty_type_ = defaults;
        default_qty_value_ = 100;
        margin_long_ = margin_short_ = margin;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = f.fee;
        slippage_ = 0;
        pyramiding_ = 0;
        qty_step_ = 0.01;
        syminfo_.pointvalue = 1;
        set_syminfo_mintick(0.00001);
        set_margin_call_enabled(true);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            balance_on_signal = current_equity();
            const double na = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("Entry", fixture_.is_long, na, na, fixture_.qty);
        }
        if (bar_index_ == 1) {
            admitted_qty = std::abs(signed_position_size());
            for (const auto& trade : trades_) {
                if (trade.entry_id == "Entry") admitted_qty += trade.qty;
            }
            pending_after_fill = pending_orders_.size();
        }
    }
    bool flat_without_trades() const {
        return position_side_ == PositionSide::FLAT && trades_.empty();
    }
};
Bar bar(int n, double price) {
    Bar result;
    result.timestamp = 1000000LL + n * 900000LL;
    result.open = result.high = result.low = result.close = price;
    result.volume = 1;
    return result;
}
void run(const Case& f, QtyType defaults, double margin = 100, bool force_admit = false) {
    current_defaults = static_cast<int>(defaults);
    Probe p(f, defaults, margin);
    const std::vector<Bar> bars = {bar(0, f.signal), bar(1, f.fill), bar(2, f.fill)};
    p.run(bars.data(), static_cast<int>(bars.size()));
    const bool expected = f.admitted || force_admit;
    check(p.balance_on_signal == f.capital, f.name, "unaltered initial capital");
    check((p.admitted_qty > 0) == expected, f.name, "observed admission decision");
    if (expected) {
        check(std::abs(p.admitted_qty - f.qty) < 1e-6, f.name, "actual floored qty including margin fragments");
    } else {
        check(p.flat_without_trades(), f.name, "decline creates no position or trade");
        check(p.pending_after_fill == 0, f.name, "decline leaves no pending parent");
    }
}
}
int main() {
    for (const auto& f : cases) {
        for (const auto defaults : {QtyType::FIXED, QtyType::CASH, QtyType::PERCENT_OF_EQUITY}) {
            run(f, defaults);
        }
    }
    // Existing lower-margin admission is outside this price-scale scope.
    run(cases[0], QtyType::FIXED, 50, true);
    std::printf("explicit market price admission: %d passed / %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
