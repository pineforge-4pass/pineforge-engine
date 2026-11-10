#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
        }                                                                      \
    } while (0)

struct Instrument {
    double pointvalue;
    double mintick;
    double qty_step;
};

constexpr Instrument kWholeLot{1.0, 0.01, 1.0};
constexpr double kCapital = 10000.0;
constexpr double kPrice = 100.0;

Bar flat_bar(std::int64_t timestamp, double price) {
    Bar bar;
    bar.open = bar.high = bar.low = bar.close = price;
    bar.volume = 1.0;
    bar.timestamp = timestamp;
    return bar;
}

class PercentEntry final : public source::PineStrategyHost {
public:
    PercentEntry(double declared_percent, double margin, bool is_long)
        : is_long_(is_long) {
        source::PineStrategyConfig config;
        config.process_orders_on_close = false;
        config.initial_capital = kCapital;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = declared_percent;
        config.margin_long = margin;
        config.margin_short = margin;
        configure_pine_strategy(config);
        initial_capital_ = kCapital;
        syminfo_.pointvalue = kWholeLot.pointvalue;
        syminfo_mintick_ = kWholeLot.mintick;
        qty_step_ = kWholeLot.qty_step;
        set_syminfo_metadata("qty_step", kWholeLot.qty_step);
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        slippage_ = 0;
        margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) strategy_entry("entry", is_long_);
        if (bar_index_ == 5) strategy_close("entry");
    }

    bool flat() const {
        return position_side_ == PositionSide::FLAT && signed_position_size() == 0.0;
    }

private:
    bool is_long_;
};

double reference_units(double equity, double declared_percent, double price, double margin) {
    const double percent_used = margin == 0.0
        ? std::min(declared_percent, 100.0) : declared_percent;
    const double units = std::floor(equity * percent_used / 100.0 / price);
    if (margin > 0.0 && units * price * margin / 100.0 > equity) return 0.0;
    return units;
}

void check_row(double declared_percent, double margin, bool is_long,
               const std::vector<Bar>& bars) {
    const double expected_units = reference_units(kCapital, declared_percent, kPrice, margin);
    const int expected_trades = expected_units > 0.0 ? 1 : 0;
    PercentEntry engine(declared_percent, margin, is_long);
    engine.run(bars.data(), static_cast<int>(bars.size()));

    const int failures_before = failures;
    const std::size_t observed_trades = engine.closed_trade_count();
    CHECK(observed_trades == static_cast<std::size_t>(expected_trades));
    if (expected_trades == 0) {
        CHECK(engine.flat());
    } else {
        for (std::size_t i = 0; i < observed_trades; ++i) {
            const auto trade = engine.get_trade(static_cast<int>(i));
            CHECK(trade.is_long == is_long);
            CHECK(trade.qty == expected_units);
            CHECK(trade.entry_price == kPrice);
        }
    }
    if (failures != failures_before) {
        std::printf("percent=%.17g margin=%.17g side=%s expected_units=%.17g "
                    "expected_trades=%d observed_trades=%zu flat=%d\n",
                    declared_percent, margin, is_long ? "long" : "short",
                    expected_units, expected_trades, observed_trades,
                    static_cast<int>(engine.flat()));
        for (std::size_t i = 0; i < observed_trades; ++i) {
            const auto trade = engine.get_trade(static_cast<int>(i));
            std::printf("  trade=%zu side=%s qty=%.17g entry_price=%.17g\n",
                        i, trade.is_long ? "long" : "short", trade.qty, trade.entry_price);
        }
    }
}

enum class Sequence {
    EntryAndClose,
    AddAndLaterClose,
    AddAndSameBarClose,
    ReversalAndClose,
};

struct CommissionRow {
    const char* name;
    CommissionType type;
    double value;
    bool process_on_close;
    Instrument instrument;
};

class PercentSequence final : public source::PineStrategyHost {
public:
    PercentSequence(double declared_percent, bool is_long, const CommissionRow& row,
                    Sequence sequence = Sequence::EntryAndClose)
        : is_long_(is_long), sequence_(sequence) {
        source::PineStrategyConfig config;
        config.initial_capital = kCapital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = declared_percent;
        config.margin_long = config.margin_short = 0.0;
        config.commission_type = static_cast<int>(row.type);
        config.commission_value = row.value;
        config.process_orders_on_close = row.process_on_close;
        config.calc_on_order_fills = false;
        config.slippage = 0;
        config.pyramiding = 2;
        config.close_entries_rule_any = false;
        configure_pine_strategy(config);
        initial_capital_ = kCapital;
        syminfo_.pointvalue = row.instrument.pointvalue;
        syminfo_mintick_ = row.instrument.mintick;
        qty_step_ = row.instrument.qty_step;
        set_syminfo_metadata("qty_step", row.instrument.qty_step);
        account_currency_fx_ = 1.0;
        // No timestamped FX series is installed.
        commission_type_ = row.type;
        commission_value_ = row.value;
        slippage_ = 0;
        margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) strategy_entry("entry", is_long_);
        if (sequence_ == Sequence::EntryAndClose && bar_index_ == 5)
            strategy_close("entry");

        if (bar_index_ == 3
            && (sequence_ == Sequence::AddAndLaterClose
                || sequence_ == Sequence::AddAndSameBarClose)) {
            // pine_adapter.cpp:9502-9508, in the 100-percent run:
            // 9502: quantity omitted; 9503: percent-of-equity declaration.
            // 9504: declared 100; the comparison run declares 150 instead.
            // 9505: no fill recalculation and no process_orders_on_close.
            // 9506: pyramiding 2; the bar-1 entry filled at bar 2.
            // 9507: same side as the live position; 9508: market, no stop/limit.
            CHECK(signed_position_size() != 0.0);
            CHECK((signed_position_size() > 0.0) == is_long_);
            strategy_entry("add", is_long_);
            // pine_adapter.cpp:10478-10495, in the 100-percent run:
            // 10478: no fill recalculation or process_orders_on_close.
            // 10479: the add above is staged; 10483: no opposite entry exists.
            // 10486: percent-of-equity; 10487: declared 100, versus 150.
            // 10488: the add is the only staged entry.
            // 10490: entry family; 10491: omitted quantity is NaN.
            // 10492-10493: omitted limit and stop are NaN.
            // 10495: the staged add is kept while closing the live entry.
            if (sequence_ == Sequence::AddAndSameBarClose) strategy_close("entry");
        }
        if (sequence_ == Sequence::AddAndLaterClose && bar_index_ == 5)
            strategy_close("entry");

        if (sequence_ == Sequence::ReversalAndClose && bar_index_ == 3) {
            // pine_adapter.cpp:10511-10560, in the 100-percent run:
            // 10511-10512: FIFO close, quantity and percentage both omitted.
            // 10516: the bar-1 position is live.
            // 10522: an opening entry; 10523: opposite to the live position.
            // 10524-10525: entry and close are issued on this same bar.
            // 10530: margin 0 avoids the earlier full-margin signal refusal.
            // 10535-10536: percent-of-equity; 10537: declared 100 versus 150.
            // 10538-10539: positive fractional grid, 0.00000001.
            // 10540: point value 1; 10541: FX 1; 10542: no FX series.
            // 10543: commission and slippage 0.
            // 10544-10545: no process_orders_on_close or fill recalculation.
            // 10546-10548: positive finite frozen units, equity and price.
            // 10558-10560: numerical predicates checked from inputs below.
            CHECK(signed_position_size() != 0.0);
            CHECK((signed_position_size() > 0.0) == is_long_);
            strategy_entry("reverse", !is_long_);
            strategy_close("entry");
        }
    }

private:
    bool is_long_;
    Sequence sequence_;
};

// Round positive money to ten significant decimal digits.
double reference_sig10(double money) {
    const double scale = std::pow(10.0, 9.0 - std::floor(std::log10(money)));
    return std::round(money * scale) / scale;
}

double reference_lot_floor(double units, double grid) {
    // All these instruments have decimal power-of-ten grids. Divide the
    // integer lot count by the reciprocal grid to keep the decimal quantity.
    const double lots_per_unit = 1.0 / grid;
    return std::floor(units * lots_per_unit) / lots_per_unit;
}

double reference_commission_units(double equity, double declared_percent, double price,
                                  const CommissionRow& row) {
    // Both margins are off: cap the money before reserving the order's fee.
    const double money = equity * std::min(declared_percent, 100.0) / 100.0;
    double units;
    if (row.type == CommissionType::CASH_PER_ORDER) {
        units = (money - row.value) / price;
    } else if (row.type == CommissionType::CASH_PER_CONTRACT) {
        units = money / (price + row.value);
    } else {
        const double unit = price * (1.0 + row.value / 100.0);
        units = row.process_on_close
            ? reference_sig10(money) / reference_sig10(unit) : money / unit;
    }
    return reference_lot_floor(units, row.instrument.qty_step);
}

void check_trades_equal(const source::PineStrategyHost& hundred,
                        const source::PineStrategyHost& above) {
    CHECK(hundred.closed_trade_count() == above.closed_trade_count());
    const auto count = std::min(hundred.closed_trade_count(), above.closed_trade_count());
    for (std::size_t i = 0; i < count; ++i) {
        const Trade left = hundred.get_trade(static_cast<int>(i));
        const Trade right = above.get_trade(static_cast<int>(i));
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.qty == right.qty);
        CHECK(left.pnl == right.pnl);
        CHECK(left.pnl_pct == right.pnl_pct);
        CHECK(left.is_long == right.is_long);
        CHECK(left.entry_bar_index == right.entry_bar_index);
        CHECK(left.exit_bar_index == right.exit_bar_index);
        CHECK(left.entry_id == right.entry_id);
        CHECK(left.entry_comment == right.entry_comment);
        CHECK(left.exit_comment == right.exit_comment);
        CHECK(left.exit_id == right.exit_id);
        CHECK(left.exit_from_bracket == right.exit_from_bracket);
        CHECK(left.max_runup == right.max_runup);
        CHECK(left.max_drawdown == right.max_drawdown);
        CHECK(left.commission == right.commission);
        CHECK(left.entry_incarnation == right.entry_incarnation);
        CHECK(left.open_at_end == right.open_at_end);
        CHECK(left.close_cause == right.close_cause);
    }
    const auto left = hundred.physical_position();
    const auto right = above.physical_position();
    CHECK(left.signed_units == right.signed_units);
    CHECK(left.average_price == right.average_price);
    CHECK(left.lot_count == right.lot_count);
}

void check_commission_pair(const CommissionRow& row, bool is_long,
                           const std::vector<Bar>& bars) {
    const int failures_before = failures;
    PercentSequence hundred(100.0, is_long, row);
    PercentSequence above(150.0, is_long, row);
    hundred.run(bars.data(), static_cast<int>(bars.size()));
    above.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(hundred.last_error().empty());
    CHECK(above.last_error().empty());
    check_trades_equal(hundred, above);
    for (bool over_hundred : {false, true}) {
        const auto& engine = over_hundred ? above : hundred;
        const double declared_percent = over_hundred ? 150.0 : 100.0;
        const double expected = reference_commission_units(
            kCapital, declared_percent, kPrice, row);
        CHECK(engine.closed_trade_count() == 1);
        CHECK(engine.physical_position().signed_units == 0.0);
        for (std::size_t i = 0; i < engine.closed_trade_count(); ++i) {
            const auto trade = engine.get_trade(static_cast<int>(i));
            CHECK(trade.qty == expected);
            CHECK(trade.is_long == is_long);
            CHECK(trade.entry_price == kPrice);
            CHECK(trade.exit_price == kPrice);
            CHECK(trade.entry_bar_index == (row.process_on_close ? 1 : 2));
            CHECK(trade.exit_bar_index == (row.process_on_close ? 5 : 6));
        }
    }
    if (failures != failures_before)
        std::printf("commission=%s side=%s declared=100/150\n",
                    row.name, is_long ? "long" : "short");
}

void check_lifecycle_pair(const CommissionRow& row, Sequence sequence, bool is_long,
                          const std::vector<Bar>& bars) {
    const int failures_before = failures;
    PercentSequence hundred(100.0, is_long, row, sequence);
    PercentSequence above(150.0, is_long, row, sequence);
    hundred.run(bars.data(), static_cast<int>(bars.size()));
    above.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(hundred.last_error().empty());
    CHECK(above.last_error().empty());
    CHECK(hundred.closed_trade_count() > 0);
    CHECK(above.closed_trade_count() > 0);
    check_trades_equal(hundred, above);
    if (failures != failures_before)
        std::printf("sequence=%s side=%s declared=100/150\n",
                    row.name, is_long ? "long" : "short");
}

} // namespace

int main() {
    struct Row {
        double margin;
        double declared_percent;
    };
    // Without the cap, the four long/short runs at margin 0 with 150 or 1000
    // percent fail; every other row passes both with and without the cap.
    constexpr Row rows[] = {
        {0.0, 80.0},
        {0.0, 100.0},
        {0.0, 150.0},
        {0.0, 1000.0},
        {1.0, 150.0},
        {100.0, 150.0},
    };
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) bars.push_back(flat_bar(60000LL * i, kPrice));
    for (const auto& row : rows) {
        for (bool is_long : {true, false}) {
            check_row(row.declared_percent, row.margin, is_long, bars);
        }
    }
    constexpr CommissionRow commission_rows[] = {
        {"cash per order", CommissionType::CASH_PER_ORDER, 2.0, false, kWholeLot},
        {"cash per contract", CommissionType::CASH_PER_CONTRACT, 0.05, false, kWholeLot},
        // 10000 / sig10(100 * 1.0005) is about 0.000025 above its
        // 0.001 lot floor, far beyond floating-point rounding uncertainty.
        {"percent on close", CommissionType::PERCENT, 0.05, true, {1.0, 0.01, 0.001}},
    };
    for (const auto& row : commission_rows) {
        for (bool is_long : {true, false}) check_commission_pair(row, is_long, bars);
    }

    constexpr CommissionRow add_later{
        "add then later close", CommissionType::PERCENT, 0.0, false, kWholeLot};
    constexpr CommissionRow add_close{
        "add then same-bar close", CommissionType::PERCENT, 0.0, false, kWholeLot};
    for (bool is_long : {true, false}) {
        check_lifecycle_pair(add_later, Sequence::AddAndLaterClose, is_long, bars);
        check_lifecycle_pair(add_close, Sequence::AddAndSameBarClose, is_long, bars);
    }

    constexpr double reversal_price = 1.13384;
    constexpr CommissionRow reversal{
        "reversal then close", CommissionType::PERCENT, 0.0, false,
        {1.0, 0.00001, 0.00000001}};
    std::vector<Bar> reversal_bars;
    for (int i = 0; i < 8; ++i)
        reversal_bars.push_back(flat_bar(60000LL * i, reversal_price));
    // pine_adapter.cpp:10550-10560: flat prices keep equity at capital
    // apart from sub-cent tick-rounding residue, below the money-grid precision.
    // The fine lot grid makes rounded notional equal capital; the ten-digit
    // affordable price is below the tick-index product of the signal price.
    // Check both numerical predicates from the inputs before comparing runs.
    const double tick_price = std::round(reversal_price / reversal.instrument.mintick)
        * reversal.instrument.mintick;
    const double reversal_units = reference_commission_units(
        kCapital, 100.0, tick_price, reversal);
    CHECK(kCapital + 1e-9 >= reference_sig10(reversal_units * tick_price));
    CHECK(std::isfinite(reference_sig10(reference_sig10(kCapital) / reversal_units)));
    CHECK(reference_sig10(reference_sig10(kCapital) / reversal_units) < tick_price);
    for (bool is_long : {true, false})
        check_lifecycle_pair(reversal, Sequence::ReversalAndClose, is_long, reversal_bars);

    std::printf("percent-equity cap with margins off: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
