#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using namespace pineforge;
constexpr double lot = 0.000001;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

Bar bar(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, (index + 1) * std::int64_t{86'400'000}};
}

struct Call {
    int bar;
    double price;
    double units;
};

class LongBook final : public source::PineStrategyHost {
public:
    // The export rows read the closed trades from outside the book.
    using source::PineStrategyHost::closed_trade_entry_bar_index;
    using source::PineStrategyHost::closed_trade_entry_price;
    using source::PineStrategyHost::closed_trade_exit_bar_index;
    using source::PineStrategyHost::closed_trade_exit_comment;
    using source::PineStrategyHost::closed_trade_exit_price;
    using source::PineStrategyHost::closed_trade_size;

    LongBook(double capital, double tick, double quantity, double fee = 0.0,
             double share = 100.0, int pyramiding = 1, int slippage = 0,
             double quantity_step = lot)
        : quantity_(quantity), tick_(tick), quantity_step_(quantity_step) {
        attach_pine_execution_adapter();
        set_syminfo_mintick(tick);
        set_syminfo_metadata("qty_step", quantity_step_);
        config_.initial_capital = capital;
        config_.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config_.default_qty_value = share;
        config_.commission_type = static_cast<int>(CommissionType::PERCENT);
        config_.commission_value = fee;
        config_.margin_long = 100.0;
        config_.margin_short = 100.0;
        config_.pyramiding = pyramiding;
        config_.slippage = slippage;
        configure_pine_strategy(config_);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("L", true, nan, nan, quantity_);
        if (pine_bar_index() == close_bar) {
            if (close_entry) strategy_close("L");
            else strategy_close_all();
        }
        if (pine_bar_index() == 3)
            position_at_bar_3 = physical_position().signed_units;
        if (pine_bar_index() == 1) {
            entered = physical_position().signed_units;
            // Read the shared money helper itself, independently of which
            // checkpoint dispatches it. No rounding of equity belongs here.
            source::PineExecutionAdapter reader(*this);
            reader.set_configuration(config_);
            source::StagedConfiguration staged;
            staged.syminfo.mintick = tick_;
            staged.syminfo.pointvalue = 1.0;
            staged.quantity_grid = quantity_step_;
            reader.set_staged_configuration(staged);
            equity_at_fill = reader.source_margin_money(
                position_avg_price(), 0).equity;
            if (std::isfinite(requirement_mark))
                requirement = read_shared_requirement
                    ? reader.source_margin_money(requirement_mark, 0)
                    : adapter_.source_margin_money(requirement_mark, 0);
        }
    }

    std::vector<Call> calls() const {
        std::vector<Call> result;
        for (int i = 0; i < trade_count(); ++i) {
            if (closed_trade_exit_comment(i) == "Margin call")
                result.push_back({closed_trade_exit_bar_index(i),
                                  closed_trade_exit_price(i), closed_trade_size(i)});
        }
        return result;
    }

    int close_bar = 2;
    bool close_entry = false;
    double entered = 0.0;
    double position_at_bar_3 = nan;
    double equity_at_fill = nan;
    double requirement_mark = nan;
    bool read_shared_requirement = false;
    source::PineExecutionAdapter::SourceMarginMoney requirement;

private:
    source::PineStrategyConfig config_;
    double quantity_;
    double tick_;
    double quantity_step_;
};

void expect_calls(const LongBook& book, const std::vector<Call>& expected) {
    const auto actual = book.calls();
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(actual[i].bar == expected[i].bar);
        assert(actual[i].price == expected[i].price);
        // Much less than one lot: an adjacent lot must fail.
        assert(std::abs(actual[i].units - expected[i].units) < 1e-10);
    }
}

void tick_mark_and_plain_equity() {
    // Explicit units isolate the margin rule from entry sizing.
    // T=99999.000028, F=1, E=99999.0000305. The signal's rounded
    // equity and the fill's requirement are 99999.00003: the entry admits
    // and is covered. At the next open, P=round_tick(2.49)=2:
    // required=r10(199998.000056)=199998.0001, equity=199998.0000585,
    // deficit=.0000415, cover=20 lots, sale=80 lots. At raw 2.49,
    // required=248997.5101, equity=248997.51007222, deficit=.00002778,
    // cover=11 lots and the sale would be 44 lots. Rounding equity at P=2
    // would erase its deficit. After the tick sale, required=199997.9999,
    // equity is still 199998.0000585: no owed sale and no later call.
    LongBook book(99999.0000305, 1.0, 99999.000028);
    const std::vector<Bar> bars = {
        bar(0, 1.01, 1.01, 1.01, 1.01),
        bar(1, 1.01, 1.01, 1.01, 1.01),
        bar(2, 2.49, 3.49, 1.29, 2.49),
        bar(3, 2.49, 2.49, 2.49, 2.49)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 0.000080}});
}

void owed_sale(bool next_point_calls) {
    // T=99999.000048, F=.1, E=9999.9000055. The fill is covered:
    // r10(T F)=9999.900005 < E. At P=2, required=199998.0001,
    // equity=199998.0000967, deficit=.0000033, cover=1 lot, sale=4 lots.
    // Re-check R=99999.000044 at that SAME P: exact=199998.000088,
    // required is still 199998.0001 and equity is unchanged. Owe 4 lots.
    // MADE: next P=3 has required=299997.0001 < equity=299997.0001407;
    // sell the owed 4 lots at 3. The remaining low and close are covered.
    // DROPPED: next P=4 has required=399996.0002, equity=399996.0001847,
    // deficit=.0000153, cover=3 lots: sell 12 lots, discard the owed 4.
    // R=99999.000032 then requires 399996.0001: no further debt.
    LongBook book(9999.9000055, 0.1, 99999.000048);
    const Bar check = next_point_calls
        ? bar(2, 3.01, 4.02, 2.01, 3.01)   // low first, then high
        : bar(2, 2.01, 3.01, 0.91, 1.01);  // high first, then low
    const std::vector<Bar> bars = {
        bar(0, 0.11, 0.11, 0.11, 0.11),
        bar(1, 0.11, 0.11, 0.11, 0.11), check,
        bar(3, check.close, check.close, check.close, check.close)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 0.000004},
                       {2, next_point_calls ? 4.0 : 3.0,
                           next_point_calls ? 0.000012 : 0.000004}});
}

void owed_sale_sized_at_close() {
    // T=99999.000048, F=.1, E=9999.90000546, tick=.1, lot=.000001.
    // At the check P=2, required=199998.0001 and equity=199998.00009666,
    // so the shortfall is .00000334. The first sale is
    // floor(.00000334 / 2 / lot) * lot = floor(1.67) lots = .000001,
    // hence 4 lots=.000004. The same-price re-check leaves the .00000334
    // shortfall owed. The bar close .11 rounds to C=.1. If the owed sale
    // were sized at the check, it would again be .000004. The close-sized
    // rule is floor(.00000334 / .1 / lot) * lot
    // = floor(33.4) lots = .000033, hence 4 lots=.000132; the one-unit
    // fallback is unused because the floor is 33 lots.
    // The open 2.01 is nearer the high 3.01 than the low .91, so the next
    // waypoint is P=3, where the reduced book is covered and the owed sale
    // must execute before the script closes the position. After the first
    // sale, R=99999.000044 and at P=3 its requirement is
    // r10(299997.000132)=299997.0001, while equity is the cash left after
    // the entry (.00000066) plus the first sale (.000008) plus R*3 =
    // 299997.00014066, so the owed sale executes there. After that sale,
    // R2=99998.999912; at P=3, r10(299996.999736)=299996.9997,
    // below the unchanged equity. The remaining low and close are P=.9 and
    // P=.1: requirements are r10(89999.0999208)=89999.09992 and
    // r10(9999.8999912)=9999.899991, against equities (cash .00040466 after
    // both sales plus R2*P) 89999.10032546 and 9999.90039586, so no
    // further call occurs.
    LongBook book(9999.90000546, 0.1, 99999.000048);
    const std::vector<Bar> bars = {
        bar(0, 0.11, 0.11, 0.11, 0.11),
        bar(1, 0.11, 0.11, 0.11, 0.11),
        bar(2, 2.01, 3.01, 0.91, 0.11),
        bar(3, 0.11, 0.11, 0.11, 0.11)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 0.000004}, {2, 3.0, 0.000132}});
}

void one_unit() {
    // T=10000.000003, E=10000.0000055, F=1. At P=2:
    // required=20000.00001, equity=20000.0000085, deficit=.0000015;
    // deficit/P=.00000075 < lot, so one whole unit is sold.
    LongBook book(10000.0000055, 1.0, 10000.000003);
    const std::vector<Bar> bars = {
        bar(0, 1.01, 1.01, 1.01, 1.01),
        bar(1, 1.01, 1.01, 1.01, 1.01),
        bar(2, 2.49, 3.49, 1.29, 2.49),
        bar(3, 2.49, 2.49, 2.49, 2.49)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 1.0}});
}

void owed_sale_across_close() {
    // The same T, F and E as owed_sale. The first three marks 3, 3, .1
    // are covered; the close at 2 calls 4 lots and owes 4 more on its
    // unchanged 199998.0001 requirement. The next open's 3 is covered
    // (required=299997.0001, equity=299997.0001407), so it sells those
    // 4 lots at 3. Debt must survive the script and the bar boundary.
    LongBook book(9999.9000055, 0.1, 99999.000048);
    book.close_bar = 3;
    const std::vector<Bar> bars = {
        bar(0, 0.11, 0.11, 0.11, 0.11),
        bar(1, 0.11, 0.11, 0.11, 0.11),
        bar(2, 3.01, 3.01, 0.11, 2.01),
        bar(3, 3.01, 3.01, 0.91, 1.01),
        bar(4, 1.01, 1.01, 1.01, 1.01)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 0.000004}, {3, 3.0, 0.000004}});
}

void commissioned_open(bool on_tick) {
    // T=99.95, F=100, E=10000.00003, fee=.001. Equity=9990.00503,
    // required=9995, deficit=4.99497; cover=floor(.0499497/lot)*lot
    // =.049949, sale=.199796. Exit fee=.0199796 leaves equity
    // 9989.9850504, against required=9975.0204: no further call.
    LongBook book(10000.00003, 1.0, 99.95, 0.1);
    const double p = on_tick ? 100.0 : 100.01;
    const std::vector<Bar> bars = {
        bar(0, p, p, p, p), bar(1, p, p, p, p),
        bar(2, p, p, p, p), bar(3, p, p, p, p)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{1, 100.0, 0.199796}});
    assert(std::abs(book.equity_at_fill - 9989.9850504) < 1e-9);
}

void commissioned_later_bar() {
    // R1: r10(E)=10009.89991, r10(.1 * 1.001)=.1001;
    // floor(10009.89991 / .1001 / lot)*lot = T=99999.000099.
    // E=10009.8999101, F=.1, entry fee=9.9999000099.
    // At the fill: equity=9999.9000100901 > required=9999.90001.
    // At the next open P=2: required=r10(199998.000198)=199998.0002,
    // plain equity=199998.0001980901, deficit=.0000019099;
    // cover=0 lots, so sell one unit. Its .002 exit fee leaves
    // equity=199997.9981980901 > required=199996.0002. No later call.
    LongBook book(10009.8999101, 0.1, nan, 0.1);
    const std::vector<Bar> bars = {
        bar(0, 0.11, 0.11, 0.11, 0.11),
        bar(1, 0.11, 0.11, 0.11, 0.11),
        bar(2, 2.01, 3.01, 0.91, 1.01),
        bar(3, 1.01, 1.01, 1.01, 1.01)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    assert(std::abs(book.entered - 99999.000099) < 1e-10);
    assert(std::abs(book.equity_at_fill - 9999.9000100901) < 1e-9);
    expect_calls(book, {{2, 2.0, 1.0}});
}

void plain_shortfall_floor() {
    // T=99.95, F=100, E=10000.0000000001, fee=.001.
    // required=9995, equity=9990.0050000001, deficit=4.9949999999.
    // floor(deficit/100/lot)=49949, sale=.199796; putting the deficit
    // on the ten-digit ladder first gives 4.995 and can add a cover lot.
    LongBook book(10000.0000000001, 1.0, 99.95, 0.1);
    const std::vector<Bar> bars = {
        bar(0, 100.01, 100.01, 100.01, 100.01),
        bar(1, 100.01, 100.01, 100.01, 100.01),
        bar(2, 100.01, 100.01, 100.01, 100.01),
        bar(3, 100.01, 100.01, 100.01, 100.01)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{1, 100.0, 0.199796}});
}

void covered(bool default_quantity) {
    // Default 95%: floor(9500 / 100.1 / lot)*lot=94.905094.
    // At F=100, equity=9990.5094906 and required=9490.5094: covered.
    // Explicit control: T=90.000003, E=10000.000004, entry fee=9.0000003,
    // plain equity=9991.0000037 (r10 would be 9991.000004), required=9000.0003.
    LongBook book(default_quantity ? 10000.0 : 10000.000004, 1.0,
                  default_quantity ? nan : 90.000003, 0.1, 95.0);
    const std::vector<Bar> bars = {
        bar(0, 100.01, 100.01, 100.01, 100.01),
        bar(1, 100.01, 101.01, 99.01, 100.01),
        bar(2, 100.01, 102.01, 98.01, 100.01),
        bar(3, 100.01, 100.01, 100.01, 100.01)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {});
    assert(std::abs(book.entered - (default_quantity ? 94.905094 : 90.000003)) < 1e-10);
    assert(std::abs(book.equity_at_fill
        - (default_quantity ? 9990.5094906 : 9991.0000037)) < 1e-9);
}

void decimal_half_boundary(bool on_tick) {
    // T=36744.079716, F=1, E=36744.079816 (free cash=.0001).
    // At P=125, exact T*P=4593009.9645, required=4593009.965,
    // equity=4593009.9646, deficit=.0004, cover=3 lots, sale=12 lots.
    // The binary product rounds down to 4593009.964 and misses the call.
    // Re-check R=36744.079704: required=4593009.963 < equity; no debt.
    // Both feeds have the same tick marks; the whole-tick feed must arm too.
    LongBook book(36744.079816, 1.0, 36744.079716);
    const double f = on_tick ? 1.0 : 1.01;
    const double p = on_tick ? 125.0 : 125.01;
    const std::vector<Bar> bars = {
        bar(0, f, f, f, f), bar(1, f, f, f, f),
        bar(2, p, p, p, p), bar(3, p, p, p, p)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 125.0, 0.000012}});
}

void decimal_half_boundary_ladders() {
    struct Row {
        double units;
        double capital;
        double price;
        double sale;
    };
    const Row rows[] = {
        // F=1, free cash=.00012; 289949.4593*25=7248736.4825,
        // required=7248736.483, equity=7248736.48262, deficit=.00038,
        // cover=floor(15.2 lots)=15 lots, sale=60 lots. Re-check:
        // R=289949.45924, required=7248736.481 < equity; no debt.
        {289949.4593, 289949.45942, 25.0, 0.000060},
        // F=1, free cash=.01; 45029.645*16570=746141217.65,
        // required=746141217.7, equity=746141217.66, deficit=.04,
        // cover=floor(2.414... lots)=2 lots, sale=8 lots. Re-check:
        // R=45029.644992, required=746141217.5 < equity; no debt.
        {45029.645, 45029.655, 16570.0, 0.000008},
        // F=1, free cash=.1; 771690.36875*9520=7346492310.5,
        // required=7346492311, equity=7346492310.6, deficit=.4,
        // cover=floor(42.0168... lots)=42 lots, sale=168 lots. Re-check:
        // R=771690.368582, required=7346492309 < equity; no debt.
        {771690.36875, 771690.46875, 9520.0, 0.000168}};
    for (const auto& row : rows) {
        LongBook book(row.capital, 1.0, row.units);
        const double p = row.price + 0.01;
        const std::vector<Bar> bars = {
            bar(0, 1.01, 1.01, 1.01, 1.01),
            bar(1, 1.01, 1.01, 1.01, 1.01),
            bar(2, p, p, p, p), bar(3, p, p, p, p)};
        book.run(bars.data(), static_cast<int>(bars.size()));
        expect_calls(book, {{2, row.price, row.sale}});
    }
}

void entry_fill_sub_lot() {
    // Commissioned entry (fee=.001): T=10000.000009, F=4, lot=.000001,
    // E=40040.000036536; the entry fee=40.000000036 leaves plain fill
    // equity=40000.0000365. Exact T*4=40000.000036, required=40000.00004,
    // deficit=.0000035: cover=floor(.875 lots)=0, so the fill check sells
    // ONE whole unit at 4. That ONE costs a fee of .004, leaving equity
    // 39999.9960365 > required=39996.00004 at the same-price re-check.
    // Later equity is 59997.9960545 at 6, 10002.9960095 at 1 and
    // 39999.9960365 at 4, exceeding the same requirements: no later call.
    // Without a fee this entry does not open (the engine admits an entry
    // only when equity covers its cost on the same ten-digit ladder), so
    // there is no fill check to test and no row for it.
    LongBook book(40040.000036536, 1.0, 10000.000009, 0.1);
    const std::vector<Bar> bars = {
        bar(0, 4.01, 4.01, 4.01, 4.01),
        bar(1, 4.01, 6.01, 1.01, 4.01),
        bar(2, 4.01, 4.01, 4.01, 4.01),
        bar(3, 4.01, 4.01, 4.01, 4.01)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{1, 4.0, 1.0}});
}

void later_open_on_tick() {
    // T=10000.000003, F=1, E=10000.0000055, fee=0.
    // Fill: required=10000 < equity. At a LATER open P=2:
    // exact=20000.000006, required=20000.00001, equity=20000.0000085,
    // deficit=.0000015, cover=floor(.75 lots)=0: sell ONE whole unit.
    // R=9999.000003 then requires 19998.00001 < equity; no debt.
    LongBook book(10000.0000055, 1.0, 10000.000003);
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0), bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 2.0, 3.0, 1.0, 2.0), bar(3, 2.0, 2.0, 2.0, 2.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 1.0}});
}

void covered_on_tick(bool default_quantity) {
    // Explicit T=10000, E=10001, F=1: at every tick P, equity=T*P+1
    // exceeds required=T*P, so cover and sale are zero.
    // Default 95%: T=floor(9500/1/lot)*lot=9500, E=10000;
    // at every tick P, equity=9500*P+500 > required=9500*P.
    LongBook book(default_quantity ? 10000.0 : 10001.0, 1.0,
                  default_quantity ? nan : 10000.0, 0.0,
                  default_quantity ? 95.0 : 100.0);
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0), bar(1, 1.0, 3.0, 1.0, 2.0),
        bar(2, 2.0, 4.0, 1.0, 3.0), bar(3, 3.0, 3.0, 3.0, 3.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {});
    assert(std::abs(book.entered - (default_quantity ? 9500.0 : 10000.0)) < 1e-10);
}

void close_point_call_after_script() {
    // T=10000.000003, E=10000.0000055, F=1, tick=1, lot=.000001,
    // fee=0. source_bar_fill_tick uses floor(price/tick + .5): bar 2's
    // OHLC marks are 1, 3, 1, 2. The low-first walk is 1, 1, 3, 2.
    // Open P=1: required=r10(10000.000003)=10000,
    // equity=E+T*(1-F)=10000.0000055: covered.
    // Low P=1: required=10000, equity=10000.0000055: covered.
    // High P=3: required=r10(30000.000009)=30000.00001,
    // equity=E+T*(3-F)=30000.0000115: covered.
    // Close P=2: required=r10(20000.000006)=20000.00001,
    // equity=E+T*(2-F)=20000.0000085, deficit=.0000015.
    // deficit/P=.00000075 < lot, so sell ONE whole unit at 2 on bar 2.
    // R=9999.000003 requires 19998.00001 < unchanged equity: no debt.
    // The script placed close_all for T BEFORE the close-point call.
    // At bar 3's open it sells T against R: R-T=-1, a one-unit short.
    // A call before the script instead sizes close_all at R and leaves 0.
    LongBook book(10000.0000055, 1.0, 10000.000003);
    book.close_bar = 2;
    const std::vector<Bar> bars = {
        bar(0, 1.01, 1.01, 1.01, 1.01),
        bar(1, 1.01, 1.01, 1.01, 1.01),
        bar(2, 1.01, 3.01, 0.91, 2.01),
        bar(3, 2.01, 2.01, 2.01, 2.01)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    expect_calls(book, {{2, 2.0, 1.0}});
    assert(std::abs(book.position_at_bar_3 - (-1.0)) < 1e-10);
}

void walk_call_then_close() {
    // Rule-derived; TradingView's export 3 shows the same shape: an open sale,
    // then a fresh shortfall sold at the same bar's close after the script.
    // r10 below rounds half-up to ten significant digits; equity is unrounded.
    // T=36744.079716, F=1, E=36744.079816, lot=.000001, tick=.01,
    // margin=100, fee=0: cash=E-T*F=.0001. Fill: required=r10(T*F)
    // =36744.07972 < E, so no call. Bar 2's open is nearer its low:
    // |125-125|=0 < |300.10-125|=175.10; walk=125,125,300.10,300.05.
    // Point 0: T*125=4593009.9645, required=r10(...)=4593009.965,
    // equity=.0001+4593009.9645=4593009.9646, deficit=.0004.
    // cover=floor(.0004/125/lot)*lot=3*lot; sale=4*cover=12*lot=.000012.
    // R=T-.000012=36744.079704; cash=.0001+.000012*125=.0016.
    // Same-price re-check and point 1: R*125=4593009.963 exactly,
    // required=4593009.963, equity=.0016+4593009.963=4593009.9646,
    // deficit=-.0016: no call, cover=0, sale=0, nothing owed.
    // Point 2: R*300.10=11026898.3191704, required=11026898.32,
    // equity=.0016+11026898.3191704=11026898.3207704,
    // deficit=-.0007704: no call, cover=0, sale=0.
    // Point 3 (AFTER the script): R*300.05=11025061.1151852,
    // required=11025061.12, equity=.0016+11025061.1151852=11025061.1167852,
    // deficit=.0032148. Since 10*300.05*lot=.0030005 < .0032148
    // < 11*300.05*lot=.00330055, cover=10*lot, sale=40*lot=.000040.
    // R2=R-.000040=36744.079664; cash=.0016+.000040*300.05=.013602.
    // Same-price re-check: R2*300.05=11025061.1031832,
    // required=11025061.10, equity=.013602+11025061.1031832=11025061.1167852,
    // deficit=-.0167852: no call, cover=0, sale=0, nothing owed.
    // All four bar 3 marks are also 300.05: that same covered book means
    // no call there. Skipping point 3 instead leaves R and cash=.0016,
    // so bar 3's open finds the .0032148 deficit and sells .000040 there.
    LongBook book(36744.079816, 0.01, 36744.079716);
    // No carried close_all at bar 3's open: it would end the walk first.
    book.close_bar = 3;
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 125.0, 300.10, 125.0, 300.05),
        bar(3, 300.05, 300.05, 300.05, 300.05)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    const double open_cover = std::floor(0.0004 / bars[2].open / lot) * lot;
    const double close_cover = std::floor(0.0032148 / bars[2].close / lot) * lot;
    expect_calls(book, {{2, bars[2].open, 4.0 * open_cover},
                       {2, bars[2].close, 4.0 * close_cover}});
}

double signal_units(double capital, double close) {
    const auto r10 = [](double value) {
        const double scale = std::pow(10.0, 9.0 - std::floor(std::log10(value)));
        return std::floor(value * scale + 0.5) / scale;
    };
    const double signal_price = std::floor(close + 0.5);
    return std::floor(r10(capital) / r10(signal_price) / lot) * lot;
}

void expect_export(double capital, std::int64_t first_day, std::vector<Bar> bars,
                   const std::vector<Call>& calls, double entry_price,
                   double exit_price, double remaining, int places) {
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i].timestamp = (first_day + static_cast<std::int64_t>(i)) * 86'400'000;
    LongBook book(capital, 1.0, nan, 0.0, 100.0, 0);
    book.close_bar = 3;
    book.close_entry = true;
    book.run(bars.data(), static_cast<int>(bars.size()));
    const double units = signal_units(capital, bars.front().close);
    // Less than half a lot, allowing binary representation at this scale.
    expect_calls(book, calls);
    assert(book.trade_count() == static_cast<int>(calls.size()) + 1);
    double closed_units = 0.0;
    for (int i = 0; i < book.trade_count(); ++i) {
        assert(book.closed_trade_entry_bar_index(i) == 1);
        assert(book.closed_trade_entry_price(i) == entry_price);
        closed_units += book.closed_trade_size(i);
    }
    assert(std::abs(closed_units - units) < lot / 2.0);
    const int last = book.trade_count() - 1;
    assert(book.closed_trade_exit_comment(last) != "Margin call");
    assert(book.closed_trade_exit_bar_index(last) == 4);
    assert(book.closed_trade_exit_price(last) == exit_price);
    const double scale = std::pow(10.0, places);
    assert(std::floor(book.closed_trade_size(last) * scale + 0.5) / scale == remaining);
}

void export1_one_call_above_ten_digits() {
    // TradingView's List of Trades for this script: one call, then the whole close.
    expect_export(9579831952.43, 16381, { // 2014-11-07 UTC
        bar(0, 349.99, 352.15, 340.40, 342.16),
        bar(1, 342.16, 347.75, 340.60, 345.39),
        bar(2, 346.19, 366.35, 343.40, 364.77),
        bar(3, 365.37, 377.77, 357.30, 366.43),
        bar(4, 366.43, 371.52, 360.28, 370.65)},
        {{2, 343.0, 0.000372}}, 342.0, 366.0, 28011204.5376, 4);
}

void pair_below_owed_sale_at_close() {
    // TradingView's List of Trades for this script.
    expect_export(9999999999.00, 16381, { // 2014-11-07 UTC
        bar(0, 349.99, 352.15, 340.40, 342.16),
        bar(1, 342.16, 347.75, 340.60, 345.39),
        bar(2, 346.19, 366.35, 343.40, 364.77),
        bar(3, 365.37, 377.77, 357.30, 366.43),
        bar(4, 366.43, 371.52, 360.28, 370.65)},
        {{1, 341.0, 0.000924}, {1, 348.0, 0.000912}},
        342.0, 366.0, 29239766.0771, 4);
}

void pair_above_owed_sale_at_close() {
    // TradingView's List of Trades for this script.
    expect_export(10000000000.01, 16381, { // 2014-11-07 UTC
        bar(0, 349.99, 352.15, 340.40, 342.16),
        bar(1, 342.16, 347.75, 340.60, 345.39),
        bar(2, 346.19, 366.35, 343.40, 364.77),
        bar(3, 365.37, 377.77, 357.30, 366.43),
        bar(4, 366.43, 371.52, 360.28, 370.65)},
        {{1, 341.0, 0.00084}, {1, 348.0, 0.000832}},
        342.0, 366.0, 29239766.0802, 4);
}

void export2_extreme_then_close() {
    // TradingView's List of Trades for this script: high and close calls on one bar.
    expect_export(1017605642.17, 17034, { // 2016-08-21 UTC
        bar(0, 577.70, 583.99, 576.01, 577.55),
        bar(1, 577.55, 585.95, 572.50, 582.78),
        bar(2, 582.78, 584.99, 576.00, 580.23),
        bar(3, 580.27, 582.10, 575.40, 577.58),
        bar(4, 577.65, 578.75, 571.16, 575.71)},
        {{2, 585.0, 0.000468}, {2, 580.0, 0.000264}},
        578.0, 578.0, 1760563.39373, 5);
}

void export3_open_sale_then_close() {
    // TradingView's List of Trades for this script: owed open sale and fresh close call.
    expect_export(3141592653.17, 16570, { // 2015-05-15 UTC
        bar(0, 237.12, 239.28, 235.12, 237.37),
        bar(1, 237.39, 237.40, 234.00, 235.66),
        bar(2, 235.43, 237.00, 234.15, 235.97),
        bar(3, 235.69, 236.94, 231.44, 232.87),
        bar(4, 233.03, 234.24, 230.86, 231.40)},
        {{1, 236.0, 0.000548}, {2, 235.0, 0.000548}, {2, 236.0, 0.000560}},
        237.0, 233.0, 13255665.2009, 4);
}

double requirement_at(double units, double mark, double quantity_step,
                      double tick, int pyramiding, bool expect_long_tick_book,
                      double entered_tolerance = 0.0) {
    LongBook book(units + 100.0, tick, units, 0.0, 100.0, pyramiding, 0,
                  quantity_step);
    book.requirement_mark = mark;
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0), bar(1, 1.0, 1.0, 1.0, 1.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    if (entered_tolerance > 0.0)
        assert(std::abs(book.entered - units) <= entered_tolerance);
    else
        assert(book.entered == units);
    assert(book.requirement.valid);
    if (expect_long_tick_book)
        assert(book.requirement.long_tick_book);
    expect_calls(book, {});
    return book.requirement.required;
}

void requirement_below_ten_digits() {
    // Exact product: 9999999999.5 * 1.0 = 9999999999.5, on the .5
    // quantity grid and the 1.0 tick grid. Below 1e10, the ten-significant-
    // digit result (unit step here) is 10000000000.0 and the capped result
    // is also 10000000000.0. This is GREEN on both the pre-fix and fixed
    // products.
    assert(requirement_at(9999999999.5, 1.0, 0.5, 1.0, 2, false)
        == 10000000000.0);
}

void requirement_binary_rounding_selected() {
    // Exact product: 9999999999.4 * 1.0 = 9999999999.4. The .000001
    // quantity count is 9,999,999,999,400,000, above 2^53 =
    // 9,007,199,254,740,991, so the decimal count guard rejects it and
    // binary rounding is selected. The ten-significant-digit result is
    // 9999999999.0 (the product is below 1e10), GREEN on both the pre-fix
    // and fixed products. At this magnitude a binary64 step is larger than
    // a half-lot comparison can resolve, so entered uses four upward
    // nextafter steps as its tolerance; every other row keeps exact entry
    // equality.
    const double units = 9999999999.4;
    const double entered_tolerance = 4.0 * (std::nextafter(
        units, std::numeric_limits<double>::infinity()) - units);
    assert(requirement_at(units, 1.0, 0.000001, 1.0, 1, true,
                          entered_tolerance) == 9999999999.0);
}

void requirement_above_ten_digits() {
    // Exact product: 10000000001.0 * 1.0 = 10000000001.0, on the .5
    // quantity grid and the 1.0 tick grid. The pre-fix step-10 result is
    // 10000000000.0; the one-unit cap result is 10000000001.0. RED on the
    // pre-fix product (step 10), GREEN on the fixed product (cap).
    assert(requirement_at(10000000001.0, 1.0, 0.5, 1.0, 2, false)
        == 10000000001.0);
}

void requirement_unit_half() {
    // Exact product: 10000000000.5 * 1.0 = 10000000000.5, on the .5
    // quantity grid and the 1.0 tick grid. The pre-fix step-10 result is
    // 10000000000.0; half-away-from-zero unit capping gives 10000000001.0.
    // RED on the pre-fix product, GREEN on the fixed product.
    assert(requirement_at(10000000000.5, 1.0, 0.5, 1.0, 2, false)
        == 10000000001.0);
}

void requirement_decimal_above_ten_digits() {
    // Exact product: 100000000094.0 * .1 = 10000000009.4. The .1
    // quantity and .1 tick counts are 1000000000940 and 1, both below 2^53;
    // the .1 quantity step times the bar high 1.0 is also below one, so
    // this is the qualifying decimal long-tick path. Its pre-fix step-10
    // result is 10000000010.0; decimal exact-grid rounding with the cap is
    // 10000000009.0. RED on the pre-fix product, GREEN on the fixed product.
    const double required = requirement_at(
        100000000094.0, 0.1, 0.1, 0.1, 1, true);
    assert(required == 10000000009.0);
}

void requirement_long_tick_units_digit() {
    // Exact decimal-grid product: 100000000010 units / .1 lot =
    // 1,000,000,000,100 lots, times .1/.1 = 1 tick, so the product is
    // 10000000001.0. Both counts are below 2^53 and .1*1.0=.1 is below
    // one, so this is a pyramiding-1 long-tick book. Ten significant digits
    // would give 10000000000.0 by the pre-fix step-10 rule; the one-currency-
    // unit cap gives 10000000001.0. The cap call is std::round(value), which
    // is half away from zero; the decimal path's count call is
    // std::round(value / step), followed by the same half-away-from-zero
    // remainder decision. RED at stage A, GREEN at the head.
    assert(requirement_at(100000000010.0, 0.1, 0.1, 0.1, 1, true)
        == 10000000001.0);
}

void requirement_long_tick_half_unit() {
    // Exact decimal-grid product: 100000000005 units / .1 lot =
    // 1,000,000,000,050 lots, times .1/.1 = 1 tick, so the product is
    // 10000000000.5. Both counts are below 2^53 and .1*1.0=.1 is below
    // one, so this is a pyramiding-1 long-tick book. Ten significant digits
    // would give 10000000000.0 by the pre-fix step-10 rule; the one-currency-
    // unit cap rounds the .5 half away from zero to 10000000001.0. The cap
    // call is std::round(value), and the decimal path uses
    // std::round(value / step) for counts plus an integer remainder >= half
    // for the same half-away-from-zero result. RED at stage A, GREEN at the
    // head.
    assert(requirement_at(100000000005.0, 0.1, 0.1, 0.1, 1, true)
        == 10000000001.0);
}

void requirement_cross_path_equal() {
    // The decimal long-tick path uses .1 x 100000000015 units:
    // 100000000015/.1 = 1,000,000,000,150 lots and .1/.1 = 1 tick,
    // both below 2^53, giving the exact product 10000000001.5. The general
    // ladder uses 10000000001.5 units at mark 1.0, with .5 lots and 1.0
    // ticks; its exact product is the same. The pre-fix step-10 result for
    // both is 10000000000.0; half-away-from-zero one-unit capping gives
    // 10000000002.0. RED at stage A, GREEN at the head.
    const double decimal_required = requirement_at(
        100000000015.0, 0.1, 0.1, 0.1, 1, true);
    const double general_required = requirement_at(
        10000000001.5, 1.0, 0.5, 1.0, 2, false);
    assert(decimal_required == 10000000002.0);
    assert(general_required == 10000000002.0);
    assert(decimal_required == general_required);
}

void rule_derived_shared_requirement() {
    // Rule-derived shared requirement, sampled without the long tick policy.
    // Q=100000.00006, P=100000: Q*P=10000000006.
    // Step 10 gives 10000000010; the one-unit cap gives 10000000006.
    // The live feed stays at 1, so neither rounding can liquidate the book
    // before the independent reader samples the requirement at P.
    // no TradingView observation above 1e10 on this path; the expected value follows the one-unit rule TradingView showed for the long requirement
    LongBook book(100100.00006, 1.0, 100000.00006);
    book.requirement_mark = 100000.0;
    book.read_shared_requirement = true;
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0), bar(1, 1.0, 1.0, 1.0, 1.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    assert(std::abs(book.entered - 100000.00006) < 1e-10);
    assert(book.requirement.valid && !book.requirement.long_tick_book);
    assert(book.requirement.required == 10000000006.0);
    expect_calls(book, {});
}

void rule_derived_slipped_long_requirement() {
    // Rule-derived slipped-long opening check: Q=100000.00006, tick=1,
    // slippage=1, entry print=2, E=200003.95012, free cash=3.95.
    // At the next open P=100000, Q*P=10000000006 and marked equity
    // =E+Q*(P-2)=10000000009.95. Step 10 requires 10000000010,
    // a .05 deficit: floor(.05/100000/.000001)=0, so it sells one unit.
    // The cap requires 10000000006 < equity, hence no call.
    // Slippage excludes the long tick policy; the .05 deficit is safely
    // inside the .1 lot value despite binary multiplication roundoff.
    // no TradingView observation above 1e10 on this path; the expected value follows the one-unit rule TradingView showed for the long requirement
    LongBook book(200003.95012, 1.0, 100000.00006, 0.0, 100.0, 1, 1);
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0), bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 100000.0, 100000.0, 100000.0, 100000.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    assert(std::abs(book.entered - 100000.00006) < 1e-10);
    expect_calls(book, {});
}

void rule_derived_long_residual() {
    // Rule-derived long residual: Q=100000.00006, entry=1,
    // E=100001.00006, free cash=1. Pyramiding=2 excludes the long tick
    // policy and trail scheduling; the .1 lot value admits the residual
    // check. At the next open P=100000, Q*P=10000000006.
    // Step 10 gives 10000000010, residual=4 > free cash, selling one unit.
    // The cap gives 10000000006, residual=0 < free cash, hence no call.
    // Both gaps exceed binary roundoff, including the guarded alternative
    // comparison against marked equity 10000000007.
    // no TradingView observation above 1e10 on this path; the expected value follows the one-unit rule TradingView showed for the long requirement
    LongBook book(100001.00006, 1.0, 100000.00006, 0.0, 100.0, 2);
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0), bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 100000.0, 100000.0, 100000.0, 100000.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    assert(std::abs(book.entered - 100000.00006) < 1e-10);
    expect_calls(book, {});
}

void rule_derived_lagged_follow_up_deficit() {
    // Rule-derived lagged long follow-up, sampled directly: required=1e11,
    // equity=89999999993.75, shortfall=10000000006.25. One called unit
    // with entry cost 1 leaves the unbooked-lot shortfall 10000000005.25.
    // Step 10 gives 10000000010; the cap gives 10000000005.
    // Unit margin=1 and grid=.5 give minimum restores 10000000010 and
    // 10000000005, respectively, hence follow-ups 40000000040 versus
    // 40000000020. Both fit in the remaining 99999999999 units.
    // no TradingView observation above 1e10 on this path; the expected value follows the one-unit rule TradingView showed for the long requirement
    LongBook book(1000.0, 1.0, 1.0);
    source::PineExecutionAdapter reader(book);
    source::PineStrategyConfig config;
    reader.set_configuration(config);
    source::StagedConfiguration staged;
    staged.syminfo.pointvalue = 1.0;
    staged.quantity_grid = 0.5;
    reader.set_staged_configuration(staged);
    source::PineExecutionAdapter::SourceMarginMoney money;
    money.valid = true;
    money.mark = 1.0;
    money.held = 100000000000.0;
    money.unit_margin = 1.0;
    money.exact_required = 100000000000.0;
    money.required = 100000000000.0;
    money.equity = 89999999993.75;
    money.scale = 1.0;
    NativeOpenLot open_lot;
    open_lot.signed_units = money.held;
    open_lot.entry_price = 1.0;
    assert(reader.lagged_margin_follow_up_units(
        {open_lot}, money, 1.0, 1.0, 0) == 40000000020.0);
}
} // namespace

int main(int argc, char* argv[]) {
    struct Row {
        const char* name;
        void (*run)();
    };
    const Row rows[] = {
        {"tick_mark_and_plain_equity", tick_mark_and_plain_equity},
        {"owed_sale_made", [] { owed_sale(false); }},
        {"owed_sale_dropped", [] { owed_sale(true); }},
        {"owed_sale_sized_at_close", owed_sale_sized_at_close},
        {"one_unit", one_unit},
        {"owed_sale_across_close", owed_sale_across_close},
        {"commissioned_open_off_tick", [] { commissioned_open(false); }},
        {"commissioned_open_on_tick", [] { commissioned_open(true); }},
        {"commissioned_later_bar", commissioned_later_bar},
        {"plain_shortfall_floor", plain_shortfall_floor},
        {"covered_default_quantity", [] { covered(true); }},
        {"covered_explicit_quantity", [] { covered(false); }},
        {"decimal_half_boundary_on_tick", [] { decimal_half_boundary(true); }},
        {"decimal_half_boundary_off_tick", [] { decimal_half_boundary(false); }},
        {"decimal_half_boundary_ladders", decimal_half_boundary_ladders},
        {"entry_fill_sub_lot_with_commission", entry_fill_sub_lot},
        {"later_open_on_tick", later_open_on_tick},
        {"covered_on_tick_explicit_quantity", [] { covered_on_tick(false); }},
        {"covered_on_tick_default_quantity", [] { covered_on_tick(true); }},
        {"close_point_call_after_script", close_point_call_after_script},
        {"walk_call_then_close", walk_call_then_close},
        {"export1_one_call_above_ten_digits", export1_one_call_above_ten_digits},
        {"pair_below_owed_sale_at_close", pair_below_owed_sale_at_close},
        {"pair_above_owed_sale_at_close", pair_above_owed_sale_at_close},
        {"export2_extreme_then_close", export2_extreme_then_close},
        {"export3_open_sale_then_close", export3_open_sale_then_close},
        {"requirement_below_ten_digits", requirement_below_ten_digits},
        {"requirement_binary_rounding_selected", requirement_binary_rounding_selected},
        {"requirement_above_ten_digits", requirement_above_ten_digits},
        {"requirement_unit_half", requirement_unit_half},
        {"requirement_decimal_above_ten_digits", requirement_decimal_above_ten_digits},
        {"requirement_long_tick_units_digit", requirement_long_tick_units_digit},
        {"requirement_long_tick_half_unit", requirement_long_tick_half_unit},
        {"requirement_cross_path_equal", requirement_cross_path_equal},
        {"rule_derived_shared_requirement", rule_derived_shared_requirement},
        {"rule_derived_slipped_long_requirement", rule_derived_slipped_long_requirement},
        {"rule_derived_long_residual", rule_derived_long_residual},
        {"rule_derived_lagged_follow_up_deficit", rule_derived_lagged_follow_up_deficit},
    };
    if (argc == 1) {
        for (const auto& row : rows)
            row.run();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& row : rows)
            std::puts(row.name);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--row") == 0) {
        for (const auto& row : rows) {
            if (std::strcmp(argv[2], row.name) == 0) {
                row.run();
                return 0;
            }
        }
    }
    std::fprintf(stderr, "Usage: %s [--list | --row NAME]\n", argv[0]);
    return 2;
}
