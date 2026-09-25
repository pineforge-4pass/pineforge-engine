/*
 * test_adapter_typed_entry_admission.cpp -- R5 lane B-ADAPTER, item 1.
 *
 * strategy.entry(qty=, qty_type=strategy.cash / strategy.percent_of_equity)
 * names MONEY. Until this lane PineExecutionAdapter::entry() priced every
 * explicit quantity as units at placement (required = |qty| * mark * point
 * value * fx * margin / 100), so a 1000-cash entry at 125 on 100 000 of
 * equity was silently dropped (R5 lane F7's finding), and a typed cash
 * opening on a lot grid was booked off the grid and refused by the kernel
 * (MatchRejectReason::InvalidTerms), and the pending-order fill-quantity probe
 * answered the cash or percentage number as contracts.
 *
 * The TradingView-consistent expectation. TradingView's Pine has no per-call
 * qty_type: its compiler refuses `strategy.entry("L", strategy.long,
 * qty=1000, qty_type=strategy.cash)` with 'The "{signature}" function does not
 * have an argument with the name "{name}"' (lab tv, 2026-09-25, pine-facade
 * save/new_draft). The per-call spelling is codegen's (signatures.py) and a
 * C++ caller's, and the only TradingView meaning it can carry is the same
 * quantity declared as the strategy's default: TradingView sizes a default
 * cash / percent-of-equity entry by converting its money into contracts and
 * admits it on those contracts (the declared-default sizing is
 * test_adapter_sizing_relower's measured rule; its placement admission is the
 * round-6 tapes pin-cash-afford-m100 -- cash 20 000 on 10 000 at margin 100,
 * no entry -- and -m50 -- 1 982 shares -- that the adapter's placement half
 * restates). So every typed run below is compared with the run that declares
 * that type and value as its default and enters default-sized: same account,
 * same bars, the same booked contracts, bit for bit.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// src/source/pine_adapter.cpp floor_quantity_grid, restated (file-local there).
double floor_quantity_grid(double units, double grid) {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!(grid > 0.0) || !std::isfinite(grid)) return units;
    const double floored = std::floor(units / grid + 1e-6) * grid;
    return floored < units ? floored : units;
}

Bar mk_bar(int index, double o, double h, double l, double c) {
    Bar b;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    b.volume = 1.0;
    b.timestamp = 300000LL * index;
    return b;
}

struct Case {
    const char* name;
    double capital;
    double price;         // every bar's open, high, low and close but bar 1's low
    double qty_step;      // 0 = no quantity grid
    double fee_percent;
    double margin;
    QtyType type;
    double value;
    double limit;         // NaN: a market entry
    double expected;      // the contracts TradingView's declared default books
};

// Bar 0 is the signal; bar 1 reaches `limit` (its low) or fills the market
// entry at its open; bar 4 closes everything.
std::vector<Bar> case_bars(const Case& c) {
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) bars.push_back(mk_bar(i, c.price, c.price, c.price, c.price));
    if (!std::isnan(c.limit)) bars[1].low = c.limit;
    return bars;
}

class Host : public pineforge::source::PineStrategyHost {
public:
    // per_call: strategy.entry(qty=value, qty_type=type) over a FIXED default;
    // otherwise the declared default (type, value) and a default-sized entry.
    Host(const Case& c, bool per_call) : case_(c), per_call_(per_call) {
        initial_capital_ = c.capital;
        syminfo_mintick_ = 0.01;
        qty_step_ = c.qty_step;
        default_qty_type_ = per_call ? QtyType::FIXED : c.type;
        default_qty_value_ = per_call ? 1.0 : c.value;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = c.fee_percent;
        margin_long_ = c.margin;
        margin_short_ = c.margin;
        pyramiding_ = 1;
        margin_call_enabled_ = false;
    }
    double probed = kNaN;
    int partition = -9;
    int pending_after_command = -1;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            if (per_call_) {
                strategy_entry("L", true, case_.limit, kNaN, case_.value, "", "", 0,
                               static_cast<int>(case_.type));
            } else {
                strategy_entry("L", true, case_.limit);
            }
            pending_after_command = pending_order_count();
            if (pending_after_command > 0) {
                int close_only = 0;
                const double fill = std::isnan(case_.limit) ? case_.price : case_.limit;
                (void)probe_fill_qty(pending_after_command - 1, fill, &probed, &close_only,
                                     &partition);
            }
        }
        if (bar_index_ == 4) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
private:
    Case case_;
    bool per_call_;
};

double booked(const Host& host) {
    return host.rows().empty() ? 0.0 : host.rows()[0].qty;
}

void a_typed_entry_is_admitted_and_booked_like_its_declared_default() {
    const Case cases[] = {
        // R5 lane F7's finding: 1000 / 125 = 8 contracts, 1000 of margin.
        {"cash 1000 @125", 100000.0, 125.0, 0.0, 0.0, 100.0, QtyType::CASH, 1000.0, kNaN, 8.0},
        // Money smaller than one contract: 99 / 125 = 0.792.
        {"cash 99 @125 on 100", 100.0, 125.0, 0.0, 0.0, 100.0, QtyType::CASH, 99.0, kNaN,
         99.0 / 125.0},
        // 50 % of 100 000 at 5000 = 10 contracts (the percentage as units cost 250 000).
        {"percent 50 @5000", 100000.0, 5000.0, 0.0, 0.0, 100.0, QtyType::PERCENT_OF_EQUITY,
         50.0, kNaN, 10.0},
        // The percentage fee is reserved out of the money: 50 000 / 1.001 / 125.
        {"percent 50 @125 fee 0.1", 100000.0, 125.0, 0.0, 0.1, 100.0,
         QtyType::PERCENT_OF_EQUITY, 50.0, kNaN, 50000.0 / (1.0 + 0.1 / 100.0) / 125.0},
        // A lot grid floors the quotient: 1000 / 99.99 = 10.001 -> 10.
        {"cash 1000 @99.99 grid 1", 100000.0, 99.99, 1.0, 0.0, 100.0, QtyType::CASH, 1000.0,
         kNaN, 10.0},
        // Margin 50 admits money twice the equity's half: 150 000 / 125 = 1200.
        {"cash 150000 @125 m50", 100000.0, 125.0, 0.0, 0.0, 50.0, QtyType::CASH, 150000.0,
         kNaN, 1200.0},
        // A LIMIT entry converts at its own fill: 1000 / 124.
        {"cash 1000 limit 124", 100000.0, 125.0, 0.0, 0.0, 100.0, QtyType::CASH, 1000.0, 124.0,
         1000.0 / 124.0},
        // Unaffordable money is not admitted, typed or declared (pin-cash-afford-m100).
        {"cash 200000 @125 m100", 100000.0, 125.0, 0.0, 0.0, 100.0, QtyType::CASH, 200000.0,
         kNaN, 0.0},
    };
    for (const Case& c : cases) {
        const auto bars = case_bars(c);
        Host typed(c, true);
        typed.run(bars.data(), static_cast<int>(bars.size()));
        Host declared(c, false);
        declared.run(bars.data(), static_cast<int>(bars.size()));
        std::printf("  [%-24s] per-call %.17g (pending %d, error '%s') | declared default %.17g"
                    " | expected %.17g\n",
                    c.name, booked(typed), typed.pending_after_command,
                    typed.last_error().c_str(), booked(declared), c.expected);
        CHECK(typed.last_error().empty());
        CHECK(declared.last_error().empty());
        CHECK(bits(booked(declared)) == bits(c.expected));
        CHECK(bits(booked(typed)) == bits(booked(declared)));
        if (c.expected > 0.0) {
            CHECK(typed.rows().size() == 1);
            CHECK(declared.rows().size() == 1);
            CHECK(!typed.rows().empty() && !declared.rows().empty()
                  && bits(typed.rows()[0].entry_price) == bits(declared.rows()[0].entry_price)
                  && std::abs(typed.rows()[0].entry_price
                              - (std::isnan(c.limit) ? c.price : c.limit)) < 1e-9);
        } else {
            CHECK(typed.rows().empty());
            CHECK(typed.pending_after_command == 0);
        }
    }
}

// The fill-quantity probe answers a typed entry's contracts at the price it is
// asked about -- the units its fill books -- not the money (partition 0,
// EXPLICIT: "the explicit percent/cash budget sized at the fill for a per-call
// qty_type override", include/pineforge/pineforge.h).
void a_typed_entry_probe_answers_its_booked_contracts() {
    const Case cases[] = {
        {"cash 1000 @99.99 grid 1", 100000.0, 99.99, 1.0, 0.0, 100.0, QtyType::CASH, 1000.0,
         kNaN, 10.0},
        {"percent 50 @125 fee 0.1", 100000.0, 125.0, 0.0, 0.1, 100.0,
         QtyType::PERCENT_OF_EQUITY, 50.0, kNaN, 50000.0 / (1.0 + 0.1 / 100.0) / 125.0},
        {"cash 1000 limit 124", 100000.0, 125.0, 0.0, 0.0, 100.0, QtyType::CASH, 1000.0, 124.0,
         1000.0 / 124.0},
        {"percent 20 limit 124 grid 0.001", 100000.0, 125.0, 0.001, 0.0, 100.0,
         QtyType::PERCENT_OF_EQUITY, 20.0, 124.0, floor_quantity_grid(20000.0 / 124.0, 0.001)},
    };
    for (const Case& c : cases) {
        const auto bars = case_bars(c);
        Host typed(c, true);
        typed.run(bars.data(), static_cast<int>(bars.size()));
        std::printf("  [probe %-24s] partition %d probe %.17g | booked %.17g\n", c.name,
                    typed.partition, typed.probed, booked(typed));
        CHECK(typed.last_error().empty());
        CHECK(typed.partition == 0);
        CHECK(bits(booked(typed)) == bits(c.expected));
        CHECK(bits(typed.probed) == bits(booked(typed)));
    }
}

void test(const char* name, void (*fn)()) {
    const int before = failures;
    std::printf("-- %s\n", name);
    fn();
    if (failures != before) std::printf("   ^^ %d failure(s)\n", failures - before);
}

}  // namespace

int main() {
    test("a typed entry is admitted and booked like its declared default",
         a_typed_entry_is_admitted_and_booked_like_its_declared_default);
    test("a typed entry's fill-quantity probe answers its booked contracts",
         a_typed_entry_probe_answers_its_booked_contracts);
    std::printf("R5 B-ADAPTER typed entries: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
