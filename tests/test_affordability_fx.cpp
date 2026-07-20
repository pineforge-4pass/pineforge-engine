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
    explicit FxProbe(double fx_or_nan, double commission_percent = 0.0) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_percent;
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
    double first_pnl() const { return trades() ? get_trade(0).pnl : kNaN; }
};

// A default 100%-of-equity order is placed under FX=1.0 and fills on the
// next bar after FX rolls to 1.001. TV admits the frozen signal snapshot, then
// revalues the live fill and emits a broker margin trim at the new rate.
class FrozenFxRolloverProbe : public BacktestEngine {
public:
    FrozenFxRolloverProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.04;
        margin_long_ = 100.0;
        qty_step_ = 0.0001;
        process_orders_on_close_ = false;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true);
    }
    int trades() const { return trade_count(); }
    double open_qty() const {
        return position_side_ == PositionSide::LONG ? position_qty_ : 0.0;
    }
    const Trade& trade(int i) const { return get_trade(i); }
};

// A live 1x long crosses a timestamped FX epoch on bar 2.  The broker must
// consume that epoch and emit any required trim at bar OPEN, before on_bar can
// observe the position or place another order.
class CarriedFxRolloverOrderingProbe : public BacktestEngine {
public:
    CarriedFxRolloverOrderingProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        qty_step_ = 0.0001;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& /*bar*/) override {
        ++on_bar_calls_;
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 100.0);
        } else if (bar_index_ == 2) {
            observed_qty_ = position_side_ == PositionSide::LONG
                ? position_qty_ : 0.0;
            observed_trades_ = trade_count();
        }
    }
    void enable_coof() { calc_on_order_fills_ = true; }
    int on_bar_calls() const { return on_bar_calls_; }
    int observed_trades() const { return observed_trades_; }
    double observed_qty() const { return observed_qty_; }
    double open_qty() const {
        return position_side_ == PositionSide::LONG ? position_qty_ : 0.0;
    }
    int trades() const { return trade_count(); }
    const Trade& trade(int i) const { return get_trade(i); }

private:
    int on_bar_calls_ = 0;
    int observed_trades_ = -1;
    double observed_qty_ = kNaN;
};

// Rate changes on carried shorts and leveraged longs do not yet have a
// TV-pinned broker-open liquidation rule. They must reject before on_bar
// instead of silently falling through to the end-of-bar adverse-price pass.
class UnsupportedCarriedFxRolloverProbe : public BacktestEngine {
public:
    UnsupportedCarriedFxRolloverProbe(bool is_long, double margin_pct)
        : is_long_(is_long) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        margin_long_ = is_long ? margin_pct : 100.0;
        margin_short_ = is_long ? 100.0 : margin_pct;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& /*bar*/) override {
        ++on_bar_calls_;
        if (bar_index_ == 0) {
            strategy_entry(is_long_ ? "L" : "S", is_long_,
                           kNaN, kNaN, 1.0);
        }
    }
    int on_bar_calls() const { return on_bar_calls_; }

private:
    bool is_long_;
    int on_bar_calls_ = 0;
};

// A pending entry is born before an FX epoch, then fills after the broker has
// crossed that epoch while still flat.  The flat crossing must consume the
// rollover permanently: once margin calls are enabled after the fill, the next
// bar must not replay the old epoch against the newly opened position.
class FlatEpochConsumptionProbe : public BacktestEngine {
public:
    FlatEpochConsumptionProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        qty_step_ = 0.1;
        process_orders_on_close_ = false;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            // Signal under FX=1.0; the frozen admission tuple lets this fill at
            // the next open after FX=1.001 becomes effective.
            strategy_entry("L", true);
        } else if (bar_index_ == 1) {
            qty_after_fill_ = position_side_ == PositionSide::LONG
                ? position_qty_ : 0.0;
            trades_after_fill_ = trade_count();
        } else if (bar_index_ == 2) {
            observed_qty_ = position_side_ == PositionSide::LONG
                ? position_qty_ : 0.0;
            observed_trades_ = trade_count();
        }
    }
    double qty_after_fill() const { return qty_after_fill_; }
    int trades_after_fill() const { return trades_after_fill_; }
    double observed_qty() const { return observed_qty_; }
    int observed_trades() const { return observed_trades_; }

private:
    double qty_after_fill_ = kNaN;
    int trades_after_fill_ = -1;
    double observed_qty_ = kNaN;
    int observed_trades_ = -1;
};

// Entry fees are paid in account currency at the entry fill. A later FX epoch
// changes open gross PnL and exit-time trade reporting, but must not reprice the
// already-paid fee exposed by strategy.opentrades.* while the slice is live.
class EntryFeeAccessorLifecycleProbe : public BacktestEngine {
public:
    EntryFeeAccessorLifecycleProbe(CommissionType type, double value,
                                   double qty)
        : qty_(qty) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = type;
        commission_value_ = value;
        margin_long_ = 0.0;  // isolate accounting from broker liquidation
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, qty_);
        } else if (bar_index_ == 1) {
            observed_open_commission_ = open_trade_commission(0);
            observed_open_profit_ = open_trade_profit(0);
            strategy_close("L");
        }
    }
    double observed_open_commission() const {
        return observed_open_commission_;
    }
    double observed_open_profit() const { return observed_open_profit_; }
    int trades() const { return trade_count(); }
    const Trade& trade(int i) const { return get_trade(i); }

private:
    double qty_;
    double observed_open_commission_ = kNaN;
    double observed_open_profit_ = kNaN;
};

// Exercises lifecycle transitions that retain or replace PyramidEntry slices:
// a rate-1 entry, a rate-2 pyramid add, a FIFO partial exit, then a reversal.
class PyramidEntryFeeLifecycleProbe : public BacktestEngine {
public:
    PyramidEntryFeeLifecycleProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        margin_long_ = 0.0;
        margin_short_ = 0.0;
        process_orders_on_close_ = true;
        pyramiding_ = 3;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L1", true, kNaN, kNaN, 2.0);
        } else if (bar_index_ == 1) {
            strategy_entry("L2", true, kNaN, kNaN, 1.0);
        } else if (bar_index_ == 2) {
            before_partial_first_ = open_trade_commission(0);
            before_partial_second_ = open_trade_commission(1);
            strategy_close("L1", "partial", /*qty=*/1.0,
                           /*qty_percent=*/kNaN, /*immediately=*/true);
            after_partial_first_ = open_trade_commission(0);
            after_partial_second_ = open_trade_commission(1);
            partial_trade_commission_ = get_trade(0).commission;
        } else if (bar_index_ == 3) {
            strategy_entry("S", false, kNaN, kNaN, 1.0);
        } else if (bar_index_ == 4) {
            reversal_commission_ = open_trade_commission(0);
            reversal_is_short_ = position_side_ == PositionSide::SHORT;
        }
    }

    double before_partial_first() const { return before_partial_first_; }
    double before_partial_second() const { return before_partial_second_; }
    double after_partial_first() const { return after_partial_first_; }
    double after_partial_second() const { return after_partial_second_; }
    double partial_trade_commission() const {
        return partial_trade_commission_;
    }
    double reversal_commission() const { return reversal_commission_; }
    bool reversal_is_short() const { return reversal_is_short_; }

private:
    double before_partial_first_ = kNaN;
    double before_partial_second_ = kNaN;
    double after_partial_first_ = kNaN;
    double after_partial_second_ = kNaN;
    double partial_trade_commission_ = kNaN;
    double reversal_commission_ = kNaN;
    bool reversal_is_short_ = false;
};

// With a 2x FX rollover, repricing the old 10% entry fee would manufacture a
// broker-open deficit and an 0.08-contract margin row. The paid rate-1 fee
// leaves the carried 2.75-contract position affordable.
class CarriedEntryFeeSnapshotProbe : public BacktestEngine {
public:
    CarriedEntryFeeSnapshotProbe() {
        initial_capital_ = 600.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        margin_long_ = 100.0;
        qty_step_ = 0.01;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 2.75);
        } else if (bar_index_ == 1) {
            observed_trades_ = trade_count();
            observed_qty_ = position_qty_;
        }
    }
    int observed_trades() const { return observed_trades_; }
    double observed_qty() const { return observed_qty_; }

private:
    int observed_trades_ = -1;
    double observed_qty_ = kNaN;
};

// Post-fill affordability must sum each live slice's paid fee: rate-1 L1 costs
// 20 and rate-2 L2 costs 10. Repricing both at rate 2 would use 50 instead of
// 30 and manufacture a margin trim from an otherwise affordable position.
class PostFillEntryFeeSnapshotProbe : public BacktestEngine {
public:
    PostFillEntryFeeSnapshotProbe() {
        initial_capital_ = 540.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        margin_long_ = 100.0;
        qty_step_ = 0.01;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L1", true, kNaN, kNaN, 2.0);
        } else if (bar_index_ == 1) {
            strategy_entry("L2", true, kNaN, kNaN, 0.5);
        } else if (bar_index_ == 2) {
            observed_trades_ = trade_count();
            observed_qty_ = position_qty_;
        }
    }
    int observed_trades() const { return observed_trades_; }
    double observed_qty() const { return observed_qty_; }

private:
    int observed_trades_ = -1;
    double observed_qty_ = kNaN;
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

    // D. A timestamped rate is selected as-of each broker event. The entry at
    // t=1000 uses the scalar fallback 1.0, while the close at t=2000 uses the
    // rate 2.0 that became effective at t=1500, doubling quote-currency PnL.
    {
        // Keep the carried position affordable after the rollover so this
        // case isolates PnL conversion rather than the broker-open margin path
        // pinned separately below.
        std::vector<Bar> bars = {mk_bar(1000, 400.0), mk_bar(2000, 450.0)};
        const int64_t timestamps[] = {1500};
        const double rates[] = {2.0};
        FxProbe eng(1.0);
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 1));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trades() == 1);
        CHECK(std::abs(eng.first_pnl() - 100.0) < 1e-12);

        // Configuration survives a reused handle and the as-of lookup does not
        // leak an end-of-run cursor into the next run.
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trades() == 1);
        CHECK(std::abs(eng.first_pnl() - 100.0) < 1e-12);

        // Invalid replacement is atomic: the installed valid curve remains.
        const int64_t unsorted[] = {1500, 1400};
        const double valid_rates[] = {2.0, 3.0};
        CHECK(!eng.set_account_currency_fx_series(unsorted, valid_rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(std::abs(eng.first_pnl() - 100.0) < 1e-12);

        // n=0 clears the provider and restores scalar fallback behavior.
        CHECK(eng.set_account_currency_fx_series(nullptr, nullptr, 0));
        eng.run(bars.data(), (int)bars.size());
        CHECK(std::abs(eng.first_pnl() - 50.0) < 1e-12);
    }

    // E. A series point effective on the entry bar participates in the same
    // affordability gate as a scalar FX value.
    {
        std::vector<Bar> bars = {mk_bar(1000, 600.0), mk_bar(2000, 600.0)};
        const int64_t timestamps[] = {1000};
        const double rates[] = {2.0};
        FxProbe eng(1.0);
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 1));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trades() == 0);
    }

    // E2. TradingView converts a realized trade's complete net symbol-currency
    // PnL at the EXIT bar's daily rate, including both percent-commission legs
    // (validation-adhoc/4emarsi.../tv_commission_crack.py: 335/336 exact).
    // gross 50*2 - entry fee 400*10%*2 - exit fee 450*10%*2 = -70.
    {
        std::vector<Bar> bars = {mk_bar(1000, 400.0), mk_bar(2000, 450.0)};
        const int64_t timestamps[] = {1500};
        const double rates[] = {2.0};
        FxProbe eng(/*scalar_fx=*/1.0, /*commission_percent=*/10.0);
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 1));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trades() == 1);
        CHECK(std::abs(eng.first_pnl() - (-70.0)) < 1e-12);
    }

    // E3. The live entry fee stays at its entry-time account value, while a
    // closed trade intentionally converts both percent-fee legs at exit-time
    // FX. Cash fee modes are already account-currency-native and remain
    // unchanged by the provider.
    {
        std::vector<Bar> bars = {mk_bar(1000, 100.0), mk_bar(2000, 100.0)};
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 2.0};

        EntryFeeAccessorLifecycleProbe percent(
            CommissionType::PERCENT, /*value=*/10.0, /*qty=*/1.0);
        CHECK(percent.set_account_currency_fx_series(timestamps, rates, 2));
        percent.run(bars.data(), (int)bars.size());
        CHECK(std::abs(percent.observed_open_commission() - 10.0) < 1e-12);
        CHECK(std::abs(percent.observed_open_profit() - (-10.0)) < 1e-12);
        CHECK(percent.trades() == 1);
        CHECK(std::abs(percent.trade(0).commission - 40.0) < 1e-12);
        CHECK(std::abs(percent.trade(0).pnl - (-40.0)) < 1e-12);

        EntryFeeAccessorLifecycleProbe cash_order(
            CommissionType::CASH_PER_ORDER, /*value=*/7.0, /*qty=*/1.0);
        CHECK(cash_order.set_account_currency_fx_series(timestamps, rates, 2));
        cash_order.run(bars.data(), (int)bars.size());
        CHECK(std::abs(cash_order.observed_open_commission() - 7.0) < 1e-12);
        CHECK(std::abs(cash_order.observed_open_profit() - (-7.0)) < 1e-12);
        CHECK(std::abs(cash_order.trade(0).commission - 14.0) < 1e-12);

        EntryFeeAccessorLifecycleProbe cash_contract(
            CommissionType::CASH_PER_CONTRACT, /*value=*/3.0, /*qty=*/2.0);
        CHECK(cash_contract.set_account_currency_fx_series(
            timestamps, rates, 2));
        cash_contract.run(bars.data(), (int)bars.size());
        CHECK(std::abs(cash_contract.observed_open_commission() - 6.0)
              < 1e-12);
        CHECK(std::abs(cash_contract.observed_open_profit() - (-6.0))
              < 1e-12);
        CHECK(std::abs(cash_contract.trade(0).commission - 12.0) < 1e-12);
    }

    // E4. Snapshots follow physical pyramid slices across partial exits and
    // are replaced on reversal. The partial trade still uses rate-2 realized
    // reporting (40), while the surviving half of the rate-1 L1 fee is 10.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0), mk_bar(2000, 100.0),
            mk_bar(3000, 100.0), mk_bar(4000, 100.0),
            mk_bar(5000, 100.0),
        };
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 2.0};
        PyramidEntryFeeLifecycleProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(std::abs(eng.before_partial_first() - 20.0) < 1e-12);
        CHECK(std::abs(eng.before_partial_second() - 20.0) < 1e-12);
        CHECK(std::abs(eng.after_partial_first() - 10.0) < 1e-12);
        CHECK(std::abs(eng.after_partial_second() - 20.0) < 1e-12);
        CHECK(std::abs(eng.partial_trade_commission() - 40.0) < 1e-12);
        CHECK(eng.reversal_is_short());
        CHECK(std::abs(eng.reversal_commission() - 20.0) < 1e-12);

        // A reused engine clears all position slices, then recreates the same
        // entry-time snapshots from the still-configured provider.
        eng.run(bars.data(), (int)bars.size());
        CHECK(std::abs(eng.before_partial_first() - 20.0) < 1e-12);
        CHECK(std::abs(eng.after_partial_first() - 10.0) < 1e-12);
        CHECK(std::abs(eng.reversal_commission() - 20.0) < 1e-12);
    }

    // F. A rate rollover between placement and fill does not retroactively
    // reject a frozen all-in order. Admission uses the complete signal-time
    // tuple (qty/equity/price/FX); post-fill affordability uses the new FX and
    // trims 4x the minimum restore quantity.
    {
        std::vector<Bar> bars = {mk_bar(1000, 100.0), mk_bar(2000, 100.0)};
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 1.001};
        FrozenFxRolloverProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trades() == 1);
        CHECK(eng.trade(0).exit_comment == std::string("Margin call"));
        CHECK(std::abs(eng.trade(0).qty - 0.3992) < 1e-12);
        CHECK(std::abs(eng.open_qty() - 99.5608) < 1e-12);
    }

    // G. A carried-position rollover is a broker-open event.  The new rate
    // makes 100 units require 10010 of margin against 10000 equity; flooring
    // the minimum restore quantity to 0.0999 lots and applying TV's 4x rule
    // closes 0.3996 before the bar-2 script body runs.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0),
            mk_bar(2000, 100.0),
            mk_bar(3000, 100.0),
        };
        const int64_t timestamps[] = {1000, 3000};
        const double rates[] = {1.0, 1.001};
        CarriedFxRolloverOrderingProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.last_error().empty());
        CHECK(eng.observed_trades() == 1);
        CHECK(std::abs(eng.observed_qty() - 99.6004) < 1e-12);
        CHECK(eng.trades() == 1);
        CHECK(eng.trade(0).exit_comment == std::string("Margin call"));
        CHECK(eng.trade(0).exit_id == std::string("__margin_call__"));
        CHECK(std::abs(eng.trade(0).qty - 0.3996) < 1e-12);
        CHECK(std::abs(eng.trade(0).exit_price - 100.0) < 1e-12);
        CHECK(eng.trade(0).exit_time == 3000);
        CHECK(std::abs(eng.open_qty() - 99.6004) < 1e-12);

        // The consumed-epoch cursor is per-run state: a reused engine must
        // reproduce the same broker-open row instead of retaining epoch 2.
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.last_error().empty());
        CHECK(eng.observed_trades() == 1);
        CHECK(std::abs(eng.observed_qty() - 99.6004) < 1e-12);
        CHECK(eng.trades() == 1);
    }

    // G2. A real carried-rollover deficit can be smaller than one 0.0001 lot.
    // TV's source-faithful crypt tape still emits a one-CONTRACT margin row at
    // this discontinuity (2025-07-06 08:00), rather than one qty_step or no
    // trade. The broker action must again be visible before on_bar.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0),
            mk_bar(2000, 100.0),
            mk_bar(3000, 100.0),
        };
        const int64_t timestamps[] = {1000, 3000};
        const double rates[] = {1.0, 1.0000005};
        CarriedFxRolloverOrderingProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.last_error().empty());
        CHECK(eng.observed_trades() == 1);
        CHECK(std::abs(eng.observed_qty() - 99.0) < 1e-12);
        CHECK(eng.trades() == 1);
        CHECK(std::abs(eng.trade(0).qty - 1.0) < 1e-12);
        CHECK(eng.trade(0).exit_comment == std::string("Margin call"));
        CHECK(eng.trade(0).exit_time == 3000);
    }

    // G3. An FX point crossed while flat is still a consumed broker event. A
    // position filled later must not inherit and replay that historical event.
    // The zero-fee frozen-all-in opening is exempt from a fill-time trim. A
    // stale epoch would therefore be the only event capable of wrongly closing
    // one contract from the new 100-contract position on bar 2.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0),
            mk_bar(2000, 100.0),
            mk_bar(3000, 100.0),
        };
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 1.001};
        FlatEpochConsumptionProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.last_error().empty());
        CHECK(eng.trades_after_fill() == 0);
        CHECK(std::abs(eng.qty_after_fill() - 100.0) < 1e-12);
        CHECK(eng.observed_trades() == 0);
        CHECK(std::abs(eng.observed_qty() - 100.0) < 1e-12);
    }

    // G4. Broker-open affordability uses the rate-1 paid entry fee after the
    // provider doubles. Repricing that fee at rate 2 would emit a false 0.08
    // margin row from this deliberately chosen boundary.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0), mk_bar(2000, 100.0),
        };
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 2.0};
        CarriedEntryFeeSnapshotProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.observed_trades() == 0);
        CHECK(std::abs(eng.observed_qty() - 2.75) < 1e-12);
    }

    // G5. Post-fill affordability sums each pyramid lot's own entry-time fee.
    // L1 paid 20 at rate 1 and L2 pays 10 at rate 2, keeping the 2.5-contract
    // position affordable. Repricing L1 would manufacture a margin row.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0), mk_bar(2000, 100.0),
            mk_bar(3000, 100.0),
        };
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 2.0};
        PostFillEntryFeeSnapshotProbe eng;
        CHECK(eng.set_account_currency_fx_series(timestamps, rates, 2));
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.observed_trades() == 0);
        CHECK(std::abs(eng.observed_qty() - 2.5) < 1e-12);
    }

    // G6. Unsupported carried-position rate changes fail before the script
    // body. A duplicate provider epoch with the same numeric rate is harmless
    // and is consumed without inventing a broker event.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0), mk_bar(2000, 100.0),
            mk_bar(3000, 100.0),
        };
        const int64_t timestamps[] = {1000, 3000};
        const double changed_rates[] = {1.0, 1.001};
        const double unchanged_rates[] = {1.0, 1.0};

        UnsupportedCarriedFxRolloverProbe short_position(
            /*is_long=*/false, /*margin_pct=*/100.0);
        CHECK(short_position.set_account_currency_fx_series(
            timestamps, changed_rates, 2));
        short_position.run(bars.data(), (int)bars.size());
        CHECK(short_position.last_error().find("carried 1x long")
              != std::string::npos);
        CHECK(short_position.on_bar_calls() == 2);

        UnsupportedCarriedFxRolloverProbe leveraged_long(
            /*is_long=*/true, /*margin_pct=*/50.0);
        CHECK(leveraged_long.set_account_currency_fx_series(
            timestamps, changed_rates, 2));
        leveraged_long.run(bars.data(), (int)bars.size());
        CHECK(leveraged_long.last_error().find("carried 1x long")
              != std::string::npos);
        CHECK(leveraged_long.on_bar_calls() == 2);

        UnsupportedCarriedFxRolloverProbe same_rate_short(
            /*is_long=*/false, /*margin_pct=*/100.0);
        CHECK(same_rate_short.set_account_currency_fx_series(
            timestamps, unchanged_rates, 2));
        same_rate_short.run(bars.data(), (int)bars.size());
        CHECK(same_rate_short.last_error().empty());
        CHECK(same_rate_short.on_bar_calls() == 3);
    }

    // H. Timestamped FX is currently authoritative only on ordinary
    // historical dispatch.  Unsupported schedulers fail before on_bar runs.
    {
        std::vector<Bar> bars = {mk_bar(1000, 100.0), mk_bar(2000, 100.0)};
        const int64_t timestamps[] = {1000, 2000};
        const double rates[] = {1.0, 1.001};

        CarriedFxRolloverOrderingProbe coof;
        CHECK(coof.set_account_currency_fx_series(timestamps, rates, 2));
        coof.enable_coof();
        coof.run(bars.data(), (int)bars.size());
        CHECK(coof.last_error().find("calc_on_order_fills") != std::string::npos);
        CHECK(coof.on_bar_calls() == 0);

        CarriedFxRolloverOrderingProbe magnifier;
        CHECK(magnifier.set_account_currency_fx_series(timestamps, rates, 2));
        magnifier.run(bars.data(), (int)bars.size(), "1", "1", true, 4,
                      MagnifierDistribution::ENDPOINTS);
        CHECK(magnifier.last_error().find("bar magnifier") != std::string::npos);
        CHECK(magnifier.on_bar_calls() == 0);

        CarriedFxRolloverOrderingProbe stream;
        CHECK(stream.set_account_currency_fx_series(timestamps, rates, 2));
        CHECK(!stream.stream_begin(bars.data(), (int)bars.size(), "1", "1"));
        CHECK(stream.last_error().find("streaming") != std::string::npos);
        CHECK(stream.on_bar_calls() == 0);
    }

    std::printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
