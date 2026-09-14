// R24 covered BTC/XAU controls isolate signal-cost admission when a fractional
// minimum lot is worth more than one account unit. Synthetic timestamps keep
// these compact command fixtures independent of a historical backtest.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

class Reversal : public pineforge::source::PineStrategyHost {
public:
    bool separate_close, explicit_quantity;
    bool resting_bracket = false;
    double observed = qnan, frozen = qnan;
    Reversal(double extra, bool separate = true, bool literal = false)
        : separate_close(separate), explicit_quantity(literal) {
        initial_capital_ = 1060181.9245162997 + extra;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, qnan, qnan, 9.10793);
            if (resting_bracket) strategy_exit("Resting", "Short", 100.0, 200000.0);
        }
        if (bar_index_ == 1) {
            strategy_entry("Long", true, qnan, qnan, explicit_quantity ? 9.36259 : qnan);
            for (const auto& order : pending_orders_) {
                if (order.id == "Long") frozen = order.frozen_default_qty;
            }
            if (separate_close) strategy_close("Short", "Separate close");
        }
        if (bar_index_ == 2) { observed = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_reversal_signal_cost_boundary() {
    const std::vector<Bar> bars = {
        {111500.77, 111500.77, 111500.77, 111500.77, 1, 1000},
        {111500.77, 112400.0, 111500.77, 112380.33, 1, 2000},
        {112380.32, 112485.28, 112300.0, 112448.7, 1, 3000},
        {112297.92, 112575.27, 112259.05, 112312.64, 1, 4000},
    };
    for (double extra : {-0.001, -0.0004, -0.0001, 0.0, 0.0001, 0.0003, 0.0004, 0.0005, 0.001}) {
        Reversal engine(extra);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        const bool admitted = extra <= -0.0004 || extra >= 0.0004;
        const double quantity = extra <= -0.0004 ? 9.36258 : 9.36259;
        CHECK(near(engine.frozen, quantity));
        CHECK(near(engine.observed, admitted ? quantity : 0.0));
        CHECK(engine.rows().size() == (admitted ? 2u : 1u));
        if (engine.rows().empty()) continue;
        CHECK(engine.rows()[0].exit_time == 3000);
        CHECK(near(engine.rows()[0].qty, 9.10793));
        CHECK(near(engine.rows()[0].exit_price, 112380.32));
        if (admitted && engine.rows().size() == 2) {
            CHECK(engine.rows()[1].entry_time == 3000);
            CHECK(near(engine.rows()[1].qty, quantity));
        }
    }
    // Without a separate close, a rounded-cost decline must still close the
    // old side. This distinguishes it from the whole-order price-scale drop.
    Reversal bare(0.0, false);
    bare.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(bare.observed, 0.0));
    CHECK(bare.rows().size() == 1);
    CHECK(!bare.rows().empty() && bare.rows()[0].exit_time == 3000);
    // Literal quantities already use the max(signal, fill) affordability path.
    Reversal literal(0.0, true, true);
    literal.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(literal.observed, 0.0));
    CHECK(literal.rows().size() == 1);
    // A prearmed priced bracket is outside the newly pinned simple market
    // transaction. Its existing admission remains unchanged.
    Reversal bracket(0.0);
    bracket.resting_bracket = true;
    bracket.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(bracket.observed, 9.36259));
    CHECK(bracket.rows().size() == 2);
}

enum class Context { ORDINARY, FEE, FX, MULTIPLIER, INTEGER_LOTS, CLOSE_FILL, RESTING_ENTRY };
class Flat : public pineforge::source::PineStrategyHost {
public:
    double observed = qnan;
    Context context;
    Flat(double capital, double step, double tick, Context mode = Context::ORDINARY)
        : context(mode) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = step;
        syminfo_mintick_ = tick;
        syminfo_.pointvalue = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        if (mode == Context::FEE) commission_value_ = 0.1;
        if (mode == Context::FX) account_currency_fx_ = 2.0;
        if (mode == Context::MULTIPLIER) syminfo_.pointvalue = 2.0;
        if (mode == Context::INTEGER_LOTS) qty_step_ = 1.0;
        if (mode == Context::CLOSE_FILL) process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
            if (context == Context::RESTING_ENTRY) strategy_entry("Parked", true, 300.0, qnan, 0.01);
        }
        if (bar_index_ == 1) { observed = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_flat_signal_cost_across_price_scales() {
    const std::vector<Bar> btc = {
        {112380.33, 112380.33, 112380.33, 112380.33, 1, 1000},
        {112380.32, 112485.28, 112300.0, 112448.7, 1, 2000},
        {112297.92, 112575.27, 112259.05, 112312.64, 1, 3000},
    };
    for (double extra : {0.0, 0.0004}) {
        Flat engine(1052170.9536054998 + extra, 0.00001, 0.01);
        engine.run(btc.data(), static_cast<int>(btc.size()));
        CHECK(near(engine.observed, extra == 0.0 ? 0.0 : 9.36259));
        CHECK(engine.rows().size() == (extra == 0.0 ? 0u : 1u));
    }
    const std::vector<Bar> xau = {
        {3445.31, 3446.015, 3443.295, 3443.625, 1, 1000},
        {3443.565, 3446.015, 3443.295, 3444.0, 1, 2000},
        {3439.505, 3441.0, 3438.0, 3440.0, 1, 3000},
    };
    for (double extra : {-0.001, -0.0001, 0.0, 0.0001}) {
        Flat engine(1033087.5 + extra, 0.01, 0.001);
        engine.run(xau.data(), static_cast<int>(xau.size()));
        const bool admitted = extra != -0.0001;
        CHECK(near(engine.observed, !admitted ? 0.0 : extra == -0.001 ? 299.99 : 300.0));
        CHECK(engine.rows().size() == (admitted ? 1u : 0u));
        if (admitted && engine.rows().size() == 1) {
            CHECK(near(engine.rows()[0].entry_price, 3443.565));
            CHECK(near(engine.rows()[0].exit_price, 3439.505));
        }
    }
    // These financial contexts retain their existing fill/trim behavior; the
    // new fee-free, same-currency, ordinary single-market scope must not turn
    // their admitted position into a signal-cost rejection.
    for (Context mode : {Context::FEE, Context::FX, Context::MULTIPLIER,
                         Context::INTEGER_LOTS, Context::CLOSE_FILL, Context::RESTING_ENTRY}) {
        double capital = 1033087.4999;
        if (mode == Context::FX || mode == Context::MULTIPLIER) capital = 2066174.9999;
        if (mode == Context::CLOSE_FILL) capital = 1033156.3729;
        Flat engine(capital, 0.01, 0.001, mode);
        engine.run(xau.data(), static_cast<int>(xau.size()));
        CHECK(engine.observed > 0.0);
        CHECK(!engine.rows().empty());
    }
}
}
int main() {
    test_reversal_signal_cost_boundary();
    test_flat_signal_cost_across_price_scales();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
