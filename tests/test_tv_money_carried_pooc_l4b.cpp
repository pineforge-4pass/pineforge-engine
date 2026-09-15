/*
 * Round 13 D: a carried POOC 1x long uses the rounded-money broker check
 * BEFORE the close-time script. A new close fill has no remaining bar path.
 *
 * Synthetic five-bar fixtures retain the quantities/prices/capital of the
 * July 2 EURUSD TV sensors, without loading a feed, corpus or verifier.
 * Prior pin: log-20260906t033048z-72b15543 (seven valid controls). New
 * full/partial-close pin: log-20260906t091207z-83d4bea0. All windows covered.
 *
 * Exact/default and explicit CSV d95732d8e479e1806f1cba9b0dfce4dedce3d5cd6ecf9781da50bc3d6922d865:
 * C1037042.0056329, Q878945.99 at1.17987, cash.0004116. The next bar's
 * open residual.0003188 passes, low residual.0004905 calls 1 at1.17905.
 * C+.0001 removes the call; C-.0001 moves it to next open1.17988.
 * Non-POOC stop at the same entry price retains the same next-low call.
 *
 * Q878945.98/C1037041.9938226 (cash.0004): entry-bar high1.17996 has
 * residual.0004392. POOC close has NO call (CSV963e70dd1af18e4cdf9167872f97e31ad3003e22740279b2ed13834998d3a5e5),
 * while the earlier stop fill calls 1 at that high (CSV8fe09e6f0c5551a775605186a846d2173e79380e0ee63bdc6bc8843eaca15686).
 *
 * Closing on the next-low trigger bar must see PS878944.99/E1036558.5850684:
 * full close CSV1ec1d2c4cd65984908b778dabb443c1ca2856e4c27439e59c650d484c7c1e286;
 * 30% close CSV637350d51f562446a90bc35e83ed0afca1ffa1b2df80ed216843409f943434d5
 * takes263683.49 at1.17932, then615261.5 at1.17958 on the next bar.
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

static int passed = 0;
static int failed = 0;
#define CHECK(expr) do { \
    if (expr) { ++passed; } else { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failed; \
    } \
} while (0)

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCapital = 1037042.0056329;
constexpr double kQty = 878945.99;
enum class Entry { DEFAULT_CLOSE, EXPLICIT_CLOSE, EXPLICIT_STOP };
enum class Close { LATER, FULL_AT_TRIGGER, PARTIAL_AT_TRIGGER };

bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a-b) < tolerance;
}

class MoneyProbe : public pineforge::source::PineStrategyHost {
public:
    MoneyProbe(double capital = kCapital, double qty = kQty,
               Entry entry = Entry::DEFAULT_CLOSE, Close close = Close::LATER,
               int flatten = 3)
        : qty_(qty), entry_(entry), close_(close), flatten_(flatten) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        slippage_ = 0;
        margin_long_ = margin_short_ = 100.0;
        pyramiding_ = 0;
        qty_step_ = 0.01;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        process_orders_on_close_ = entry != Entry::EXPLICIT_STOP;
        set_margin_call_enabled(true);
    }

    void on_source_bar(const Bar& bar) override {
        if ((entry_ == Entry::EXPLICIT_STOP && bar_index_ == 0)
            || (entry_ != Entry::EXPLICIT_STOP && bar_index_ == 1)) {
            strategy_entry("L", true, kNaN,
                           entry_ == Entry::EXPLICIT_STOP ? 1.17987 : kNaN,
                           entry_ == Entry::DEFAULT_CLOSE ? kNaN : qty_, "ENTRY");
            if (resting_stop_) {
                strategy_exit("Resting", "L", kNaN, 1.0,
                              kNaN, kNaN, kNaN, 100.0, "RESTING");
            }
        }
        if (bar_index_ == 2) {
            trigger_script_qty = signed_position_size();
            trigger_script_equity = current_equity() + open_profit(bar.close);
            if (close_ == Close::FULL_AT_TRIGGER) {
                strategy_close("", "TRIGGER_CLOSE");
            } else if (close_ == Close::PARTIAL_AT_TRIGGER) {
                strategy_close("L", "TRIGGER_REDUCE", kNaN, 30.0);
            }
        }
        if (bar_index_ == flatten_) strategy_close("", "END");
    }

    const std::vector<Trade>& rows() const { return trades_; }
    double physical_qty() const { return position_qty_; }
    void resting_stop() { resting_stop_ = true; }
    void commission(double value) { commission_value_ = value; }
    void pyramiding(int value) { pyramiding_ = value; }
    void scalar_fx(double value) { account_currency_fx_ = value; }
    void lot_step(double value) { qty_step_ = value; }
    void intraday_cap(int value) { adapter_.cap = value; }
    double trigger_script_qty = kNaN;
    double trigger_script_equity = kNaN;

private:
    double qty_;
    Entry entry_;
    Close close_;
    int flatten_;
    bool resting_stop_ = false;
};

std::vector<Bar> bars() {
    return {
        {1.17867, 1.17898, 1.17858, 1.17885, 1, 1000},
        {1.17884, 1.17996, 1.17884, 1.17987, 1, 2000},
        {1.17988, 1.18002, 1.17905, 1.17932, 1, 3000},
        {1.17933, 1.17980, 1.17933, 1.17958, 1, 4000},
        {1.17958, 1.17992, 1.17946, 1.17956, 1, 5000},
    };
}

void run(MoneyProbe& engine) {
    const auto input = bars();
    engine.run(input.data(), static_cast<int>(input.size()));
    CHECK(engine.last_error().empty());
    CHECK(near(engine.physical_qty(), 0.0));
}

int margin_rows(const MoneyProbe& engine) {
    int count = 0;
    for (const auto& row : engine.rows())
        if (row.exit_comment == "Margin call") ++count;
    return count;
}

void check_entries(const MoneyProbe& engine, double qty) {
    double total = 0.0;
    for (const auto& row : engine.rows()) {
        CHECK(row.entry_id == "L");
        CHECK(row.entry_time == 2000);
        CHECK(near(row.entry_price, 1.17987));
        total += row.qty;
    }
    CHECK(near(total, qty)); // every negative control must actually enter
}

void check_margin(const Trade& row, int64_t time, double price) {
    CHECK(row.exit_comment == "Margin call");
    CHECK(row.exit_id == "__margin_call__");
    CHECK(row.exit_time == time);
    CHECK(near(row.exit_price, price));
    CHECK(near(row.qty, 1.0));
    CHECK(near(row.pnl, price-1.17987, 1e-9));
}

void carried(Entry entry, double capital, bool fire, double price = 1.17905) {
    MoneyProbe engine(capital, kQty, entry);
    run(engine);
    check_entries(engine, kQty);
    CHECK(margin_rows(engine) == (fire ? 1 : 0));
    CHECK(engine.rows().size() == (fire ? 2u : 1u));
    if (engine.rows().size() != (fire ? 2u : 1u)) return;
    if (fire) {
        check_margin(engine.rows()[0], 3000, price);
        if (entry != Entry::EXPLICIT_STOP) {
            const bool at_open = near(price, 1.17988, 1e-9);
            CHECK(near(engine.rows()[0].max_runup, at_open ? .00001 : .00015, 1e-9));
            CHECK(near(engine.rows()[0].max_drawdown, at_open ? 0.0 : .00082, 1e-9));
        }
    }
    const auto& final = engine.rows().back();
    CHECK(final.exit_comment == "END");
    CHECK(final.exit_time == (entry == Entry::EXPLICIT_STOP ? 5000 : 4000));
    CHECK(near(final.exit_price, 1.17958));
    CHECK(near(final.qty, kQty-(fire ? 1.0 : 0.0)));
    if (entry != Entry::EXPLICIT_STOP)
        CHECK(near(engine.trigger_script_qty, kQty-(fire ? 1.0 : 0.0)));
}

void close_fill_has_no_past_path(bool pooc) {
    MoneyProbe engine(1037041.9938226, 878945.98,
        pooc ? Entry::EXPLICIT_CLOSE : Entry::EXPLICIT_STOP, Close::LATER, 2);
    run(engine);
    check_entries(engine, 878945.98);
    CHECK(engine.rows().size() == (pooc ? 1u : 2u));
    CHECK(margin_rows(engine) == (pooc ? 0 : 1));
    if (engine.rows().size() != (pooc ? 1u : 2u)) return;
    if (!pooc) check_margin(engine.rows()[0], 2000, 1.17996);
    const auto& final = engine.rows().back();
    CHECK(final.exit_time == (pooc ? 3000 : 4000));
    CHECK(near(final.exit_price, pooc ? 1.17932 : 1.17933));
    CHECK(near(final.qty, pooc ? 878945.98 : 878944.98));
}

void on_close_observes_margin_first(Close close) {
    MoneyProbe engine(kCapital, kQty, Entry::DEFAULT_CLOSE, close);
    run(engine);
    check_entries(engine, kQty);
    CHECK(near(engine.trigger_script_qty, 878944.99));
    CHECK(near(engine.trigger_script_equity, 1036558.5850684));
    CHECK(margin_rows(engine) == 1);
    const bool partial = close == Close::PARTIAL_AT_TRIGGER;
    CHECK(engine.rows().size() == (partial ? 3u : 2u));
    if (engine.rows().size() != (partial ? 3u : 2u)) return;
    check_margin(engine.rows()[0], 3000, 1.17905);
    CHECK(near(engine.rows()[0].max_runup, .00015, 1e-9));
    CHECK(near(engine.rows()[0].max_drawdown, .00082, 1e-9));
    const auto& close_row = engine.rows()[1];
    CHECK(close_row.exit_time == 3000);
    CHECK(near(close_row.exit_price, 1.17932));
    CHECK(close_row.exit_comment == (partial ? "TRIGGER_REDUCE" : "TRIGGER_CLOSE"));
    CHECK(near(close_row.qty, partial ? 263683.49 : 878944.99));
    if (partial) {
        CHECK(engine.rows()[2].exit_time == 4000);
        CHECK(engine.rows()[2].exit_comment == "END");
        CHECK(near(engine.rows()[2].exit_price, 1.17958));
        CHECK(near(engine.rows()[2].qty, 615261.5));
    }
}

void preserved_scope() {
    // These are compatibility controls, not new TV margin claims. The
    // formerly excluded POOC scopes must not be pulled into this extension.
    for (int scope = 0; scope < 6; ++scope) {
        MoneyProbe engine(kCapital, kQty, Entry::EXPLICIT_CLOSE);
        switch (scope) {
        case 0: engine.resting_stop(); break; // no pending-order chronology pin
        case 1: engine.commission(1e-11); break; // still affordable, same residual
        case 2: engine.pyramiding(2); break; // adds remain on established paths
        case 3: {
            const int64_t times[] = {1000};
            const double rates[] = {1.0};
            CHECK(engine.set_account_currency_fx_series(times, rates, 1));
            break;
        }
        case 4: engine.set_margin_call_enabled(false); break;
        case 5: engine.intraday_cap(100); break;
        }
        run(engine);
        check_entries(engine, kQty);
        CHECK(margin_rows(engine) == 0);
        CHECK(engine.rows().size() == 1);
    }
}
} // namespace

int main() {
    carried(Entry::DEFAULT_CLOSE, kCapital, true);
    carried(Entry::EXPLICIT_CLOSE, kCapital, true);
    carried(Entry::DEFAULT_CLOSE, kCapital+.0001, false);
    carried(Entry::DEFAULT_CLOSE, kCapital-.0001, true, 1.17988);
    carried(Entry::EXPLICIT_STOP, kCapital, true);
    close_fill_has_no_past_path(true);
    close_fill_has_no_past_path(false);
    on_close_observes_margin_first(Close::FULL_AT_TRIGGER);
    on_close_observes_margin_first(Close::PARTIAL_AT_TRIGGER);
    preserved_scope();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
