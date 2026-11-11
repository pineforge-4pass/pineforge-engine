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
    LongBook(double capital, double tick, double quantity, double fee = 0.0,
             double share = 100.0)
        : quantity_(quantity), tick_(tick) {
        attach_pine_execution_adapter();
        set_syminfo_mintick(tick);
        set_syminfo_metadata("qty_step", lot);
        config_.initial_capital = capital;
        config_.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config_.default_qty_value = share;
        config_.commission_type = static_cast<int>(CommissionType::PERCENT);
        config_.commission_value = fee;
        config_.margin_long = 100.0;
        config_.margin_short = 100.0;
        config_.pyramiding = 1;
        configure_pine_strategy(config_);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("L", true, nan, nan, quantity_);
        if (pine_bar_index() == close_bar)
            strategy_close_all();
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
            staged.quantity_grid = lot;
            reader.set_staged_configuration(staged);
            equity_at_fill = reader.source_margin_money(
                position_avg_price(), 0).equity;
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
    double entered = 0.0;
    double position_at_bar_3 = nan;
    double equity_at_fill = nan;

private:
    source::PineStrategyConfig config_;
    double quantity_;
    double tick_;
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
