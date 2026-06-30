/*
 * test_affordability_fx.cpp — account-currency FX on the broker affordability
 * gate.
 *
 * The market-entry affordability gate admits an order only when
 *   required_margin = qty * close * pointvalue * fx * (margin_pct/100) <= equity.
 * When a script declares currency=currency.XXX differing from the symbol's
 * quote currency (e.g. currency.INR on a USDT-quoted perp), TradingView keeps
 * equity in the account currency but converts the quote-currency notional via
 * the account-currency FX rate before this comparison. The engine exposes that
 * rate through the syminfo-metadata channel ("account_currency_fx"); it
 * defaults to 1.0 (no-op) so the validation corpus is byte-identical.
 *
 * This pins:
 *   A. FX 1.0 (default): a qty-1 long whose notional (600) fits inside equity
 *      (1000) is ACCEPTED -> 1 closed trade.
 *   B. FX 2.0: the same notional scales to 1200 > 1000 and the entry is
 *      REJECTED -> 0 trades. Proves the FX factor reaches required_margin.
 *   C. A non-positive / non-finite FX resets to the 1.0 default (accepted).
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double c) {
    Bar b;
    b.open = c; b.high = c; b.low = c; b.close = c; b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Enters one fixed-size long market order on bar 0 (fills at the bar close
// because process_orders_on_close is on), then closes it on bar 1. Whether the
// entry survives the affordability gate is observable as trade_count() == 1 (or
// 0 if rejected).
class FxProbe : public BacktestEngine {
public:
    explicit FxProbe(double fx_or_nan) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;             // 1x -> required_margin == notional
        process_orders_on_close_ = true;  // market entry fills at bar close
        if (!std::isnan(fx_or_nan))
            set_syminfo_metadata("account_currency_fx", fx_or_nan);
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0);  // qty=1 market long
        else if (bar_index_ == 1)
            strategy_close("L");
    }
    int trades() const { return trade_count(); }
};

void run_case(double fx, int expected_trades, const char* label) {
    std::vector<Bar> bars = {
        mk_bar(1000, 600.0),  // 0: long fills @600, notional = 1*600 = 600
        mk_bar(2000, 600.0),  // 1: close
    };
    FxProbe eng(fx);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trades() == expected_trades);
    std::printf("  %s: fx=%.2f trades=%d (expected %d)\n", label, fx,
                eng.trades(), expected_trades);
}

}  // namespace

int main() {
    std::printf("--- affordability_fx ---\n");
    // A. Default FX (1.0): notional 600 <= equity 1000 -> accepted.
    run_case(1.0, 1, "fx=1 accepts");
    // Same as default when no metadata is injected at all.
    {
        std::vector<Bar> bars = {mk_bar(1000, 600.0), mk_bar(2000, 600.0)};
        FxProbe eng(kNaN);
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trades() == 1);
        std::printf("  no-fx (default 1.0): trades=%d (expected 1)\n", eng.trades());
    }
    // B. FX 2.0: required_margin = 600*2 = 1200 > 1000 -> rejected.
    run_case(2.0, 0, "fx=2 rejects");
    // C. Non-positive FX resets to 1.0 default -> accepted.
    run_case(-5.0, 1, "fx<=0 resets to 1.0");

    std::printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
