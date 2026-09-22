/*
 * test_adapter_sizing_relower.cpp — R5 R2.
 *
 * The differential for the Pine adapter's default-quantity sizing after it is
 * re-lowered onto the core's L3/L3b `native_order::Sized`.  Every case drives
 * BOTH the real Pine adapter (source::PineStrategyHost) and the real core (a
 * bare NativeStrategyHost that emits the kernel value kinds) on the SAME
 * account, instrument and bars.
 *
 * What each case proves, in this order:
 *   1. the classification is live — the adapter really does lower these
 *      commands onto native_order::Sized, and really does not lower the ones
 *      the core cannot name;
 *   2. the re-lowering is NEUTRAL — the adapter books exactly the quantity it
 *      booked before R2, restated here from the pre-R2 source and pinned as a
 *      literal;
 *   3. the core plus the source override reproduce that same quantity bit for
 *      bit: the core resolves cash / (signal price * point value * fx) with
 *      the percentage fee reserve, and the override applies only the source
 *      lot floor on top.
 *
 * Provenance of every pinned "previous resolution": it is what
 *   src/source/pine_adapter.cpp@c54855e:1568-1591  default_sizing_units()
 * computed over
 *   src/source/pine_adapter.cpp@c54855e:1551-1566  sizing_snapshot()
 *   src/source/pine_adapter.cpp@c54855e:4628-4633  the slipped, re-snapped
 *                                                  market sizing price
 * restated by previous_default_units() below and cross-checked against the
 * literal quantity the run books.
 *
 * The exits are deliberately NOT re-lowered; case 5 is the measurement that
 * rules that out, and tests/test_native_sizing_bases.cpp keeps the matching
 * neutrality witness for the reduction bases.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

// The release gate compiles with -DNDEBUG, so assert() is a no-op here: every
// assertion is a returning CHECK.
#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

#define REQUIRE(expr)                                                          \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s (fatal)\n", __FILE__, __LINE__,     \
                        #expr);                                                \
            return;                                                            \
        }                                                                      \
    } while (0)

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Bars are stamped on the canonical 5-minute slot grid: the native preflight
// refuses a confirmed bar whose timestamp is not a slot label, and both hosts
// must see literally the same bars.
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

// --- the pre-R2 source arithmetic, restated -------------------------------
//
// nearest_tick, floor_quantity_grid, source_money_round and
// source_money_floor_lot are src/source/pine_adapter.cpp@c54855e:252-255,
// :386-394, :396-402 and :404-413.  They are restated rather than called
// because they are file-local to the adapter; every one is a literal copy.

double nearest_tick(double value, double tick) {
    if (!std::isfinite(value) || !(tick > 0.0)) return value;
    return std::floor(value / tick + 0.5) * tick;
}

double floor_quantity_grid(double units, double grid) {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!(grid > 0.0) || !std::isfinite(grid)) return units;
    const double floored = std::floor(units / grid + 1e-6) * grid;
    return floored < units ? floored : units;
}

double source_money_round(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    const double magnitude = std::floor(std::log10(std::abs(value)));
    const double scale = std::pow(10.0, 9.0 - magnitude);
    const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
    return value < 0.0 ? -rounded : rounded;
}

double source_money_floor_lot(double units, double grid) {
    if (!(grid > 0.0) || !std::isfinite(grid)) return units;
    if (!std::isfinite(units) || units <= 0.0) return units;
    double floored = std::floor(units / grid) * grid;
    if (grid == 0.01) {
        const double cent_candidate = std::floor(units * 100.0) * grid;
        if (cent_candidate > floored && cent_candidate <= units) floored = cent_candidate;
    }
    return floored < units ? floored : units;
}

// --- the account the two hosts share --------------------------------------

struct Account {
    double capital = 10000.0;
    double point_value = 1.0;
    double mintick = 0.01;
    double qty_step = 0.0;          // 0 = no quantity grid
    double fee_percent = 0.0;       // TradingView commission_value, in PERCENT
    int slippage = 0;               // TradingView slippage, in ticks
};

// The sizing price of a default-sized MARKET entry before R2, and the sizing
// price SizePrice::SignalOnTick names after it.
double market_sizing_price(const Account& a, double signal_close, bool is_long) {
    const double mark = nearest_tick(signal_close, a.mintick);
    return nearest_tick(mark + (is_long ? 1.0 : -1.0) * a.slippage * a.mintick, a.mintick);
}

// default_sizing_units() exactly as it stood before R2: the money, the
// percentage fee reserve, the division and the type-specific lot floor, in
// that order.
double previous_default_units(const Account& a, QtyType type, double value,
                              double signal_close, bool is_long, double equity) {
    const double price = market_sizing_price(a, signal_close, is_long);
    const double fx = 1.0;
    if (!(price > 0.0) || !(fx > 0.0)) return 0.0;
    if (type == QtyType::CASH) {
        const double denominator = price * a.point_value * fx;
        if (!(denominator > 0.0)) return 0.0;
        return floor_quantity_grid(value / denominator, a.qty_step);
    }
    if (type != QtyType::PERCENT_OF_EQUITY || !(equity > 0.0)) return 0.0;
    const double marked = a.qty_step > 0.0 ? source_money_round(equity) : equity;
    double cash = value / 100.0 * marked;
    if (a.fee_percent > 0.0) cash /= 1.0 + a.fee_percent / 100.0;
    const double denominator = price * a.point_value * fx;
    if (!(denominator > 0.0)) return 0.0;
    const double units = cash / denominator;
    return a.qty_step > 0.0 ? source_money_floor_lot(units, a.qty_step)
                            : floor_quantity_grid(units, a.qty_step);
}

// The source money the re-lowered command hands the core as CashValue, and
// the lot floor its resolve_terms override applies to the core's quotient.
double source_sizing_cash(const Account& a, QtyType type, double value, double equity) {
    if (type == QtyType::CASH) return value;
    const double marked = a.qty_step > 0.0 ? source_money_round(equity) : equity;
    return value / 100.0 * marked;
}

double source_lot_floor(const Account& a, QtyType type, double units) {
    if (type == QtyType::PERCENT_OF_EQUITY && a.qty_step > 0.0) {
        return source_money_floor_lot(units, a.qty_step);
    }
    return floor_quantity_grid(units, a.qty_step);
}

// --- the Pine half ---------------------------------------------------------

// Scripted source probe.  Script chars, indexed by the source bar:
//   'L' default-sized long entry      'S' default-sized short entry
//   'e' explicit 10-unit long entry   'C' close all
//   'x' two 50 % limit exits on the live entry
//   '.' nothing
class PineProbe : public pineforge::source::PineStrategyHost {
public:
    PineProbe(const Account& account, QtyType qty_type, double qty_value) {
        initial_capital_ = account.capital;
        syminfo_.pointvalue = account.point_value;
        syminfo_mintick_ = account.mintick;
        qty_step_ = account.qty_step;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = account.fee_percent;
        slippage_ = account.slippage;
        pyramiding_ = 0;
        process_orders_on_close_ = false;
        margin_call_enabled_ = false;
    }
    std::string script;
    double exit_limit = kNaN;
    // The R2 classification observed at every command the script writes.
    std::vector<int> core_sized_at_command;

    // The placement-time frozen default quantity and its sizing price, read
    // from the adapter's pending view right after the command is written --
    // the number the source's own money band and affordability gates consume
    // before any request reaches the core.
    std::vector<double> frozen_at_command;
    std::vector<double> sizing_price_at_command;
    void capture_frozen() {
        const auto& view = source_pending_view();
        if (view.empty()) return;
        frozen_at_command.push_back(view.back().frozen_default_qty);
        sizing_price_at_command.push_back(view.back().default_stop_sizing_price);
    }

    void on_source_bar(const Bar& bar) override {
        current_close_ = bar.close;
        if (bar_index_ < 0 || bar_index_ >= static_cast<int>(script.size())) return;
        switch (script[bar_index_]) {
        case 'L':
            core_sized_at_command.push_back(adapter_core_sizes_default_opening(true) ? 1 : 0);
            strategy_entry("L", true);
            capture_frozen();
            break;
        case 'S':
            core_sized_at_command.push_back(adapter_core_sizes_default_opening(false) ? 1 : 0);
            strategy_entry("S", false);
            capture_frozen();
            break;
        case 'T':
            // A pure-stop default-sized entry one point above the signal
            // close: sized at its own trigger level, never at the signal rule.
            strategy_entry("T", true, kNaN, current_close_ + 1.0);
            capture_frozen();
            break;
        case 'e': strategy_entry("L", true, kNaN, kNaN, 10.0); break;
        case 'C': strategy_close_all(); break;
        case 'x':
            strategy_exit("X1", "L", exit_limit, kNaN, kNaN, kNaN, kNaN, 50.0);
            strategy_exit("X2", "L", exit_limit, kNaN, kNaN, kNaN, kNaN, 50.0);
            break;
        default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    const std::vector<Trade>& rows() const { return trades_; }
private:
    double current_close_ = kNaN;
};

// --- the core half ---------------------------------------------------------

// A bare native host.  It names the kernel value kinds and overrides
// resolve_execution_terms with NOTHING BUT the source lot floor, which is the
// division of labour the re-lowered adapter uses: the core resolves the
// quantity and publishes it as the facts' remaining units, the override
// applies the one source quirk on top.
class CoreProbe : public pineforge::NativeStrategyHost {
public:
    std::function<void(CoreProbe&, int)> act;
    std::function<double(double)> lot_floor;
    int bar = -1;
    mutable int terms_passes = 0;
    mutable double published_quotient = kNaN;

    void on_native_run_begin() override { bar = -1; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar;
        if (act) act(*this, bar);
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms out{facts.default_resolved_price, std::nullopt,
                               no::OpeningShape::Transact};
        const auto* units = std::get_if<no::RemainingUnits>(&facts.remaining);
        if (!units) return out;
        ++terms_passes;
        published_quotient = units->q;
        if (lot_floor) out.units = lot_floor(units->q);
        return out;
    }
    using pineforge::NativeStrategyHost::submit;
};

NativeRunSpec core_spec(const Account& account, const char* key) {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = account.capital;
    spec.point_value = account.point_value;
    spec.account_fx = 1.0;
    spec.price_tick = account.mintick;
    spec.slippage_ticks = static_cast<std::uint32_t>(account.slippage < 0 ? 0 : account.slippage);
    spec.fee_kind = NativeFeeKind::Percent;
    // The adapter stages TradingView's commission straight through
    // (src/source/pine_adapter.cpp:1448-1449): fee_value is a PERCENT.
    spec.fee_value = account.fee_percent;
    // The Pine run leaves the price grid at None: the source owns every
    // booked price.  That is exactly why the sizing price needs
    // SizePrice::SignalOnTick and cannot use SizePrice::Signal.
    if (account.qty_step > 0.0) spec.quantity_grid = account.qty_step;
    return spec;
}

// The request the re-lowered adapter writes.
no::Request core_sized_open(double cash, no::Side side, bool reserve_fee,
                            no::SizePrice price, const char* label) {
    no::Sized sized;
    sized.side = side;
    sized.basis = no::CashValue{cash};
    sized.time = no::SizeTime::AtAcceptance;
    sized.price = price;
    sized.grid_policy = no::ExecutionGridPolicy::ExplicitUnits;
    sized.reserve_percent_fee = reserve_fee;
    no::Request out;
    out.intent = sized;
    out.label = label;
    return out;
}

struct CoreRun {
    double units = kNaN;
    double quotient = kNaN;
    int terms_passes = 0;
};

// Run the core probe over `bars`, opening once with `request` on bar 0 and
// flattening on bar `flatten_bar`.
CoreRun core_opened_units(const Account& account, const char* key,
                          const no::Request& request, std::function<double(double)> floor,
                          const std::vector<Bar>& bars, int flatten_bar) {
    CoreRun out;
    CoreProbe host;
    host.lot_floor = std::move(floor);
    host.act = [&request, flatten_bar](CoreProbe& self, int bar) {
        if (bar == 0) {
            (void)self.submit(request);
        } else if (bar == flatten_bar) {
            no::Request flat;
            flat.intent = no::Flatten{};
            flat.label = "flat";
            (void)self.submit(flat);
        }
    };
    if (host.configure_native(core_spec(account, key)).status != NativeSetupStatus::Applied) {
        std::printf("  configure_native: %s\n", host.last_error().c_str());
        return out;
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    out.quotient = host.published_quotient;
    out.terms_passes = host.terms_passes;
    if (host.trade_count() < 1) return out;
    out.units = host.get_trade(0).qty;
    return out;
}

std::vector<Bar> flat_bars(double close, int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) bars.push_back(mk_bar(i, close, close, close, close));
    return bars;
}

// ===========================================================================
// 1. The classification is live.
// ===========================================================================
void the_adapter_lowers_exactly_the_paths_the_core_can_name() {
    Account account;
    account.capital = 100000.0;
    const auto bars = flat_bars(125.0, 4);

    // A declaration-level cash default quantity on a market entry: lowered.
    PineProbe cash(account, QtyType::CASH, 5000.0);
    cash.script = "L.C.";
    cash.run(bars.data(), static_cast<int>(bars.size()));
    REQUIRE(cash.core_sized_at_command.size() == 1);
    CHECK(cash.core_sized_at_command[0] == 1);

    // A percentage default quantity: lowered.
    PineProbe percent(account, QtyType::PERCENT_OF_EQUITY, 25.0);
    percent.script = "L.C.";
    percent.run(bars.data(), static_cast<int>(bars.size()));
    REQUIRE(percent.core_sized_at_command.size() == 1);
    CHECK(percent.core_sized_at_command[0] == 1);

    // A FIXED declaration has no money of its own to convert: not lowered,
    // and its quantity never leaves the source.
    PineProbe fixed(account, QtyType::FIXED, 3.0);
    fixed.script = "L.C.";
    fixed.run(bars.data(), static_cast<int>(bars.size()));
    REQUIRE(fixed.core_sized_at_command.size() == 1);
    CHECK(fixed.core_sized_at_command[0] == 0);
    REQUIRE(fixed.trade_count() == 1);
    CHECK(bits(fixed.rows()[0].qty) == bits(3.0));

    std::printf("  [classification] cash=%d percent=%d fixed=%d\n",
                cash.core_sized_at_command[0], percent.core_sized_at_command[0],
                fixed.core_sized_at_command[0]);
}

// ===========================================================================
// 2. A cash entry: neutral, and reproduced by the core plus the floor.
// ===========================================================================
//
// bar0  close(S)  default-sized (cash) long entry
// bar1  fills at open carrying the frozen quantity
// bar2  close all
void a_cash_entry_is_the_core_quotient_under_the_source_floor() {
    struct Row {
        const char* name;
        double signal_close;
        int slippage;
        double qty_step;
        double previous;      // the literal quantity the pre-R2 adapter booked
    };
    // 125.00 is exactly 12500 * 0.01 in binary64; 2517.70 is not — the decimal
    // literal the corpus feeds carry is one ULP below 251770 * 0.01, which is
    // why the sizing price must be rounded onto the tick ladder at all.
    const Row rows[] = {
        {"on-grid close, no slip", 125.0, 0, 0.0, 40.0},
        {"off-grid close, no slip", 2517.70, 0, 0.0, 1.9859395480001587},
        {"off-grid close, 3 ticks", 2517.70, 3, 0.0, 1.9859158845467941},
        {"on-grid close, lot grid", 125.0, 0, 1.0, 40.0},
        {"off-grid close, lot grid", 2517.70, 0, 1.0, 1.0},
    };
    for (const Row& row : rows) {
        Account account;
        account.capital = 100000.0;
        account.slippage = row.slippage;
        account.qty_step = row.qty_step;
        const double value = 5000.0;
        const auto bars = flat_bars(row.signal_close, 4);

        // (a) the pre-R2 resolution, restated and pinned.
        const double previous = previous_default_units(
            account, QtyType::CASH, value, row.signal_close, true, account.capital);
        CHECK(bits(previous) == bits(row.previous));

        // (b) the adapter after R2 books exactly it.
        PineProbe pine(account, QtyType::CASH, value);
        pine.script = "L.C.";
        pine.run(bars.data(), static_cast<int>(bars.size()));
        REQUIRE(pine.trade_count() == 1);
        const double adapter_units = pine.rows()[0].qty;
        REQUIRE(pine.core_sized_at_command.size() == 1);
        CHECK(pine.core_sized_at_command[0] == 1);
        CHECK(bits(adapter_units) == bits(previous));

        // (c) the core resolves the quotient, the override applies the floor.
        const auto core = core_opened_units(
            account, "r2-cash",
            core_sized_open(source_sizing_cash(account, QtyType::CASH, value, account.capital),
                            no::Side::Long, /*reserve_fee=*/false,
                            no::SizePrice::SignalOnTick, "cash-open"),
            [&](double q) { return source_lot_floor(account, QtyType::CASH, q); },
            bars, 2);
        CHECK(bits(core.units) == bits(adapter_units));
        // One terms pass: the quantity is resolved once, at acceptance.
        CHECK(core.terms_passes == 1);
        // And the published number is the RAW quotient, not a core-floored
        // one — ExecutionGridPolicy::ExplicitUnits leaves the floor to the
        // source, which is what keeps the two lot floors out of the core.
        CHECK(bits(core.quotient)
              == bits(value / (market_sizing_price(account, row.signal_close, true)
                               * account.point_value * 1.0)));

        std::printf("  [cash %-24s] previous=%.17g adapter=%.17g core=%.17g match=%d\n",
                    row.name, previous, adapter_units, core.units,
                    bits(core.units) == bits(previous) ? 1 : 0);
    }
}

// ===========================================================================
// 3. A percent-of-equity entry with a percentage fee and a lot grid.
// ===========================================================================
void a_percent_entry_with_fee_and_grid_is_the_core_quotient() {
    Account account;
    account.capital = 10000.0;
    account.qty_step = 1.0;
    account.fee_percent = 0.1;
    const double value = 50.0;               // 50 % of equity
    const double close = 100.0;
    const auto bars = flat_bars(close, 4);

    // 50 % of 10000 = 5000; 5000 / 1.001 = 4995.004995004995; / 100 = 49.95…
    // floored onto the 1-unit lot grid = 49.
    const double previous = previous_default_units(
        account, QtyType::PERCENT_OF_EQUITY, value, close, true, account.capital);
    CHECK(bits(previous) == bits(49.0));

    PineProbe pine(account, QtyType::PERCENT_OF_EQUITY, value);
    pine.script = "L.C.";
    pine.run(bars.data(), static_cast<int>(bars.size()));
    REQUIRE(pine.trade_count() == 1);
    CHECK(bits(pine.rows()[0].qty) == bits(previous));
    REQUIRE(pine.core_sized_at_command.size() == 1);
    CHECK(pine.core_sized_at_command[0] == 1);

    const auto core = core_opened_units(
        account, "r2-percent",
        core_sized_open(
            source_sizing_cash(account, QtyType::PERCENT_OF_EQUITY, value, account.capital),
            no::Side::Long, /*reserve_fee=*/true, no::SizePrice::SignalOnTick, "pct-open"),
        [&](double q) { return source_lot_floor(account, QtyType::PERCENT_OF_EQUITY, q); },
        bars, 2);
    CHECK(bits(core.units) == bits(previous));
    CHECK(core.terms_passes == 1);
    // The reserve is the exact inverse of the charge: 0.1 % reserves 0.1 %.
    CHECK(bits(core.quotient) == bits(5000.0 / (1.0 + 0.1 / 100.0) / 100.0));

    // The floor is the SOURCE's: the money floor, not the core's generic one.
    // Both land on 49 here; the grid sweep below is where they part.
    std::printf("  [percent fee+grid] previous=%.17g adapter=%.17g core=%.17g quotient=%.17g\n",
                previous, pine.rows()[0].qty, core.units, core.quotient);

    // A quotient a proportional epsilon below a lot boundary: the source cash
    // floor keeps the raw quotient, the core's generic floor would take it
    // down a whole lot.  This is why the core hands back the quotient.
    const double just_under = 2.9999999999;
    CHECK(bits(source_lot_floor(account, QtyType::CASH, just_under)) == bits(just_under));
    CHECK(bits(source_lot_floor(account, QtyType::PERCENT_OF_EQUITY, just_under)) == bits(2.0));
}

// ===========================================================================
// 4. A reversal: the core-sized opening serves the reversal shape.
// ===========================================================================
void a_default_sized_reversal_keeps_both_rows() {
    Account account;
    account.capital = 10000.0;
    account.qty_step = 1.0;
    const double close = 100.0;
    const auto bars = flat_bars(close, 6);

    PineProbe pine(account, QtyType::PERCENT_OF_EQUITY, 100.0);
    pine.script = "L.S.C.";
    pine.run(bars.data(), static_cast<int>(bars.size()));
    REQUIRE(pine.trade_count() == 2);
    REQUIRE(pine.core_sized_at_command.size() == 2);
    CHECK(pine.core_sized_at_command[0] == 1);
    CHECK(pine.core_sized_at_command[1] == 1);
    const double opened_long = pine.rows()[0].qty;
    const double opened_short = pine.rows()[1].qty;
    // 100 % of 10000 at 100 = 100 units long; the reversal closes them and
    // opens the short the same way.
    CHECK(bits(opened_long) == bits(100.0));
    CHECK(opened_short > 0.0);
    std::printf("  [reversal] rows=%d long=%.17g short=%.17g core_sized=%d,%d\n",
                static_cast<int>(pine.trade_count()), opened_long, opened_short,
                pine.core_sized_at_command[0], pine.core_sized_at_command[1]);

    // The core accepts a Sized opening for the same shape: a 100-unit long
    // reversed into the sized short is one transaction on the declared side.
    const auto core = core_opened_units(
        account, "r2-reversal",
        core_sized_open(source_sizing_cash(account, QtyType::PERCENT_OF_EQUITY, 100.0,
                                           account.capital),
                        no::Side::Long, /*reserve_fee=*/false,
                        no::SizePrice::SignalOnTick, "rev-open"),
        [&](double q) { return source_lot_floor(account, QtyType::PERCENT_OF_EQUITY, q); },
        bars, 2);
    CHECK(bits(core.units) == bits(100.0));
}

// ===========================================================================
// 5. A 50 % exit with siblings — the measurement that keeps exits host-sized.
// ===========================================================================
//
// The core resolves a ScopeFraction as units = scope * fraction, one binary64
// multiplication.  TradingView resolves basis * percent / 100.0
// (src/source/pine_adapter.cpp:2545) and then applies its own percentage-exit
// quantizer (:2550-2565, with a +1e-6 floor and a keep-one-lot rule).  The two
// agree on the integer lot below and disagree on a fractional basis, so the
// percentage exits are NOT re-lowered.
void a_fifty_percent_exit_with_siblings_is_not_representable() {
    Account account;
    account.capital = 10000.0;
    account.qty_step = 1.0;
    const double close = 100.0;
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) bars.push_back(mk_bar(i, close, close + 5.0, close, close));

    PineProbe pine(account, QtyType::FIXED, 10.0);
    pine.script = "e.xC.";
    pine.exit_limit = close + 2.0;
    pine.run(bars.data(), static_cast<int>(bars.size()));
    // Two 50 % siblings on a 10-unit lot both claim 5: the source reserves
    // each against the PLACEMENT-time basis, which is ScopeBasis::AtAcceptance.
    REQUIRE(pine.trade_count() >= 2);
    CHECK(bits(pine.rows()[0].qty) == bits(5.0));
    CHECK(bits(pine.rows()[1].qty) == bits(5.0));
    std::printf("  [50%% exits] rows=%d %.17g %.17g\n",
                static_cast<int>(pine.trade_count()), pine.rows()[0].qty, pine.rows()[1].qty);

    // The core's arithmetic, on the same 10-unit scope, agrees here.
    CHECK(bits(10.0 * 0.5) == bits(10.0 * 50.0 / 100.0));
    // And parts on a fractional one, which no Sized/ScopeFraction field can
    // reconcile: the association is the source's own.
    const double basis = 698554.2358392038;
    CHECK(bits(basis * 0.5) != bits(basis * 50.0 / 100.0));
    std::printf("  [association] scope*(p/100)=%.17g scope*p/100=%.17g equal=%d\n",
                basis * 0.5, basis * 50.0 / 100.0,
                bits(basis * 0.5) == bits(basis * 50.0 / 100.0) ? 1 : 0);
}

// ===========================================================================
// 6. Why SizePrice::SignalOnTick, and not SizePrice::Signal.
// ===========================================================================
void the_sizing_price_rule_is_the_tick_ladder_not_the_fill_grid() {
    Account account;
    account.capital = 100000.0;
    account.slippage = 3;
    const double close = 2517.70;
    const double value = 5000.0;
    const auto bars = flat_bars(close, 4);

    const double expected_price = market_sizing_price(account, close, true);
    CHECK(bits(expected_price) == bits(2517.73));

    const auto on_tick = core_opened_units(
        account, "r2-price-ontick",
        core_sized_open(value, no::Side::Long, false, no::SizePrice::SignalOnTick, "on-tick"),
        nullptr, bars, 2);
    CHECK(bits(on_tick.quotient)
          == bits(value / (expected_price * account.point_value * 1.0)));

    // SizePrice::Signal on this run (price_grid = None, which is what the Pine
    // adapter stages) never touches the ladder: it divides by the raw close
    // plus the slippage.
    const auto signal = core_opened_units(
        account, "r2-price-signal",
        core_sized_open(value, no::Side::Long, false, no::SizePrice::Signal, "signal"),
        nullptr, bars, 2);
    CHECK(bits(signal.quotient)
          == bits(value / ((close + 3 * account.mintick) * account.point_value * 1.0)));

    // SizePrice::Resolved divides by the acceptance point's own raw price.
    const auto resolved = core_opened_units(
        account, "r2-price-resolved",
        core_sized_open(value, no::Side::Long, false, no::SizePrice::Resolved, "resolved"),
        nullptr, bars, 2);
    CHECK(bits(resolved.quotient) == bits(value / (close * account.point_value * 1.0)));

    std::printf("  [price rule 3 ticks] on-tick=%.17g signal=%.17g resolved=%.17g\n",
                on_tick.quotient, signal.quotient, resolved.quotient);

    // Without slippage the two existing rules collapse onto the raw close and
    // land one ULP away from the ladder price the source divides by: this is
    // the difference that made a new rule necessary rather than a reuse of
    // SizePrice::Signal, which on a price_grid = None run is unquantized.
    Account still = account;
    still.slippage = 0;
    const double still_price = market_sizing_price(still, close, true);
    const auto still_on_tick = core_opened_units(
        still, "r2-price-ontick-0",
        core_sized_open(value, no::Side::Long, false, no::SizePrice::SignalOnTick, "on-tick-0"),
        nullptr, bars, 2);
    const auto still_signal = core_opened_units(
        still, "r2-price-signal-0",
        core_sized_open(value, no::Side::Long, false, no::SizePrice::Signal, "signal-0"),
        nullptr, bars, 2);
    CHECK(bits(still_on_tick.quotient)
          == bits(value / (still_price * still.point_value * 1.0)));
    CHECK(bits(still_signal.quotient) == bits(value / (close * still.point_value * 1.0)));
    CHECK(bits(still_signal.quotient) != bits(still_on_tick.quotient));
    std::printf("  [price rule 0 ticks] on-tick=%.17g signal=%.17g differ=%d\n",
                still_on_tick.quotient, still_signal.quotient,
                bits(still_signal.quotient) != bits(still_on_tick.quotient) ? 1 : 0);

    // The pre-rounding is what makes a whole-tick slippage step exact: the
    // ladder price plus n ticks is a ladder price, so the second rounding is a
    // re-normalization and never a second, different quantization.
    for (int k = 0; k < 64; ++k) {
        const double p = 0.005 + 25.0 * k + 0.0033 * k;
        const double pre = nearest_tick(nearest_tick(p, 0.01) + 3 * 0.01, 0.01);
        CHECK(bits(pre) == bits(std::round((std::round(p / 0.01) * 0.01 + 3 * 0.01) / 0.01) * 0.01));
    }
}

// ===========================================================================
// 7. The positive neutrality witness: the source layer names the sizing basis.
// ===========================================================================

bool names_identifier(const std::string& text, const std::string& name) {
    for (std::size_t at = text.find(name); at != std::string::npos;
         at = text.find(name, at + 1)) {
        const auto word = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_';
        };
        const bool left = at > 0 && word(text[at - 1]);
        const std::size_t after = at + name.size();
        const bool right = after < text.size() && word(text[after]);
        if (!left && !right) return true;
    }
    return false;
}

std::string read_file(const char* path) {
    std::FILE* input = std::fopen(path, "rb");
    if (!input) return {};
    std::string out;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, input)) > 0) out.append(buffer, got);
    std::fclose(input);
    return out;
}

void the_source_layer_names_the_sizing_basis_and_the_core_names_no_source() {
#if defined(PINEFORGE_R2_ADAPTER_FILE) && defined(PINEFORGE_R2_CORE_FILES)
    const std::string adapter = read_file(PINEFORGE_R2_ADAPTER_FILE);
    REQUIRE(!adapter.empty());
    for (const char* name : {"Sized", "CashValue", "SizeTime", "SizePrice"}) {
        const bool found = names_identifier(adapter, name);
        if (!found) std::printf("  adapter no longer names %s\n", name);
        CHECK(found);
    }
    // The reduction basis stays out of the source layer; that is the ruling
    // case 5 measures, and tests/test_native_sizing_bases.cpp is its witness.
    CHECK(!names_identifier(adapter, "ScopeFraction"));
    CHECK(!names_identifier(adapter, "ScopeBasis"));

    // The core keeps naming no source SYMBOL: the sizing rule the adapter was
    // lowered onto is spelled in the run spec's own vocabulary and never
    // reaches for a source-layer identifier or a source constant.  (Prose
    // comments in the core do cite TradingView, which is why the scan is on
    // identifiers the core would have to USE, not on the word.)
    std::string joined = PINEFORGE_R2_CORE_FILES;
    std::size_t start = 0;
    int scanned = 0;
    while (start <= joined.size()) {
        const std::size_t at = joined.find('|', start);
        const std::string piece = joined.substr(
            start, at == std::string::npos ? std::string::npos : at - start);
        if (!piece.empty()) {
            const std::string text = read_file(piece.c_str());
            REQUIRE(!text.empty());
            ++scanned;
            for (const char* name : {"PineExecutionAdapter", "PineStrategyHost",
                                     "PineSizingSnapshot", "PineStrategyConfig",
                                     "default_qty_type", "commission_value"}) {
                const bool found = names_identifier(text, name);
                if (found) std::printf("  core names %s in %s\n", name, piece.c_str());
                CHECK(!found);
            }
        }
        if (at == std::string::npos) break;
        start = at + 1;
    }
    // A vacuous pass would be worse than a failure.
    CHECK(scanned >= 2);
#else
    std::printf("  PINEFORGE_R2_ADAPTER_FILE / PINEFORGE_R2_CORE_FILES undefined\n");
    CHECK(false);
#endif
}

// ===========================================================================
// 8. The conversion exists once (R5 N11).
// ===========================================================================
//
// The adapter's placement-time frozen quantity -- the number its money band
// and affordability gates consume before any request exists -- is the
// kernel's own arithmetic, read through NativeStrategyHost::native_sized_units
// and floored by the source, and it is bit for bit the quantity the pre-R2
// adapter computed with a conversion of its own.
void the_conversion_exists_once() {
    struct Row {
        const char* name;
        QtyType type;
        double value;
        double signal_close;
        int slippage;
        double qty_step;
        double fee_percent;
        double previous;      // the literal quantity the pre-R2 adapter froze
    };
    const Row rows[] = {
        {"cash on-grid close, no slip", QtyType::CASH, 5000.0, 125.0, 0, 0.0, 0.0, 40.0},
        {"cash off-grid close, 3 ticks", QtyType::CASH, 5000.0, 2517.70, 3, 0.0, 0.0,
         1.9859158845467941},
        {"cash off-grid close, lot grid", QtyType::CASH, 5000.0, 2517.70, 0, 1.0, 0.0, 1.0},
        {"percent fee+grid", QtyType::PERCENT_OF_EQUITY, 50.0, 100.0, 0, 1.0, 0.1, 49.0},
    };
    for (const Row& row : rows) {
        Account account;
        account.capital = row.type == QtyType::CASH ? 100000.0 : 10000.0;
        account.slippage = row.slippage;
        account.qty_step = row.qty_step;
        account.fee_percent = row.fee_percent;
        const auto bars = flat_bars(row.signal_close, 4);
        const double price = market_sizing_price(account, row.signal_close, true);

        // (a) the pre-R2 conversion, restated and pinned.
        const double previous = previous_default_units(
            account, row.type, row.value, row.signal_close, true, account.capital);
        CHECK(bits(previous) == bits(row.previous));

        // (b) the adapter freezes exactly it at the command, before submit.
        PineProbe pine(account, row.type, row.value);
        pine.script = "L.C.";
        pine.run(bars.data(), static_cast<int>(bars.size()));
        REQUIRE(pine.frozen_at_command.size() == 1);
        CHECK(bits(pine.frozen_at_command[0]) == bits(previous));
        CHECK(bits(pine.sizing_price_at_command[0]) == bits(price));
        REQUIRE(pine.trade_count() == 1);
        CHECK(bits(pine.rows()[0].qty) == bits(previous));

        // (c) the kernel's query on the same inputs is the pre-floor quotient,
        // and the source floor on top of it is the frozen number: the
        // division and the fee reserve exist in the kernel alone.
        CoreProbe core;
        REQUIRE(core.configure_native(core_spec(account, "n11-once")).status
                == NativeSetupStatus::Applied);
        const auto request = core_sized_open(
            source_sizing_cash(account, row.type, row.value, account.capital),
            no::Side::Long, row.type == QtyType::PERCENT_OF_EQUITY && row.fee_percent > 0.0,
            no::SizePrice::SignalOnTick, "once");
        const auto& sized = std::get<no::Sized>(request.intent);
        const auto quotient = core.native_sized_units(sized, price, account.capital, 1.0);
        REQUIRE(quotient.has_value());
        double cash = source_sizing_cash(account, row.type, row.value, account.capital);
        if (sized.reserve_percent_fee) cash /= 1.0 + row.fee_percent / 100.0;
        CHECK(bits(*quotient) == bits(cash / (price * account.point_value * 1.0)));
        CHECK(bits(source_lot_floor(account, row.type, *quotient)) == bits(previous));
        std::printf("  [once %-28s] quotient=%.17g frozen=%.17g previous=%.17g booked=%.17g\n",
                    row.name, *quotient, pine.frozen_at_command[0], previous,
                    pine.rows()[0].qty);
    }
}

// ===========================================================================
// 9. Why the two lot floors stay the adapter's -- measured, not asserted.
// ===========================================================================
//
// The kernel's SnapToGrid is the largest grid multiple at or below the
// quotient, with the engine's on-grid tolerance. Neither source floor is
// that function on every input, so the adapter asks for ExplicitUnits and
// floors the kernel's quotient itself (retained, why):
//   * the cash floor floor(u/g + 1e-6)*g keeps a quotient within a millionth
//     of a lot below a boundary RAW, where the kernel takes a whole lot off;
//   * the percent floor floor(u/g)*g has no on-grid tolerance, so a quotient
//     that IS a lot multiple can come out one lot short when u/g lands an ulp
//     below the integer (0.0392 / 0.0001 = 391.99999999999994).
void the_source_lot_floors_are_not_the_kernel_floor() {
    Account cash_account;
    cash_account.capital = 100000.0;
    cash_account.qty_step = 1.0;
    CoreProbe host;
    REQUIRE(host.configure_native(core_spec(cash_account, "n11-floors")).status
            == NativeSetupStatus::Applied);

    // 299.99999999 / 100 = 2.9999999999: a millionth of a lot below three.
    const double just_under = 299.99999999 / 100.0;
    CHECK(bits(just_under) == bits(2.9999999999));
    no::Sized snapped;
    snapped.basis = no::CashValue{299.99999999};
    snapped.grid_policy = no::ExecutionGridPolicy::SnapToGrid;
    const auto kernel_cash = host.native_sized_units(snapped, 100.0, cash_account.capital, 1.0);
    REQUIRE(kernel_cash.has_value());
    CHECK(bits(*kernel_cash) == bits(2.0));
    CHECK(bits(source_lot_floor(cash_account, QtyType::CASH, just_under)) == bits(just_under));
    CHECK(bits(source_lot_floor(cash_account, QtyType::PERCENT_OF_EQUITY, just_under))
          == bits(2.0));

    // 3.92 / 100 = 0.0392 exactly on a 0.0001 grid: the kernel keeps the lot
    // multiple, the percent floor drops to 0.0391.
    Account fine_account;
    fine_account.capital = 100000.0;
    fine_account.qty_step = 0.0001;
    CoreProbe fine;
    REQUIRE(fine.configure_native(core_spec(fine_account, "n11-floors-fine")).status
            == NativeSetupStatus::Applied);
    const double on_grid = 3.92 / 100.0;
    CHECK(bits(on_grid) == bits(0.0392));
    no::Sized lot;
    lot.basis = no::CashValue{3.92};
    lot.grid_policy = no::ExecutionGridPolicy::SnapToGrid;
    const auto kernel_lot = fine.native_sized_units(lot, 100.0, fine_account.capital, 1.0);
    REQUIRE(kernel_lot.has_value());
    CHECK(bits(*kernel_lot) == bits(0.0392));
    CHECK(bits(source_lot_floor(fine_account, QtyType::PERCENT_OF_EQUITY, on_grid))
          == bits(0.0391));
    CHECK(bits(source_lot_floor(fine_account, QtyType::CASH, on_grid)) == bits(0.0392));
    std::printf("  [floors] 2.9999999999: kernel=%.17g cash-floor=%.17g percent-floor=%.17g\n",
                *kernel_cash, source_lot_floor(cash_account, QtyType::CASH, just_under),
                source_lot_floor(cash_account, QtyType::PERCENT_OF_EQUITY, just_under));
    std::printf("  [floors] 0.0392 on 0.0001: kernel=%.17g percent-floor=%.17g\n",
                *kernel_lot, source_lot_floor(fine_account, QtyType::PERCENT_OF_EQUITY, on_grid));
}

// ===========================================================================
// 10. A retained host-sized branch: the pure-stop default entry.
// ===========================================================================
//
// A default-sized STOP entry is sized at its own directionally snapped
// trigger level, which no SizePrice names; it keeps HostSized and the frozen
// quantity of its own resolve_terms branch. Harvested from the adapter at
// 683a82f (the tree before this lane touched src/source/): the placement-time
// frozen quantity, its sizing price and the booked row.
//
// bar0 close 100  stop entry at 101 (percent 100 % of 10000 on a 1-unit grid)
// bar1 high 102   fills at the stop
// bar2            close all
void a_pure_stop_default_entry_keeps_its_own_sizing_branch() {
    Account account;
    account.capital = 10000.0;
    account.qty_step = 1.0;
    std::vector<Bar> bars;
    bars.push_back(mk_bar(0, 100.0, 100.0, 100.0, 100.0));
    bars.push_back(mk_bar(1, 100.0, 102.0, 100.0, 101.5));
    bars.push_back(mk_bar(2, 101.5, 101.5, 101.5, 101.5));
    bars.push_back(mk_bar(3, 101.5, 101.5, 101.5, 101.5));

    PineProbe pine(account, QtyType::PERCENT_OF_EQUITY, 100.0);
    pine.script = "T.C.";
    pine.run(bars.data(), static_cast<int>(bars.size()));
    REQUIRE(pine.frozen_at_command.size() == 1);
    REQUIRE(pine.trade_count() == 1);
    std::printf("  [stop entry] frozen=%.17g sizing_price=%.17g booked=%.17g entry=%.17g\n",
                pine.frozen_at_command[0], pine.sizing_price_at_command[0],
                pine.rows()[0].qty, pine.rows()[0].entry_price);
    // Harvested at 683a82f: the command freezes its sizing PRICE (the snapped
    // stop level) but no quantity -- the stop branch of resolve_terms sizes
    // it at the fill against that level: 10000 / 101 = 99.0099 -> 99 on the
    // lot grid, not the 100 units the signal close would give.
    CHECK(bits(pine.sizing_price_at_command[0]) == bits(101.0));
    CHECK(std::isnan(pine.frozen_at_command[0]));
    CHECK(bits(pine.rows()[0].qty) == bits(99.0));
    CHECK(bits(pine.rows()[0].entry_price) == bits(101.0));
    CHECK(bits(pine.rows()[0].qty) != bits(100.0));
}

// ===========================================================================
// 11. A typed per-entry quantity is the kernel's conversion too (R5 lane F7).
// ===========================================================================
//
// strategy.entry(qty=, qty_type=strategy.cash / strategy.percent_of_equity)
// names money of its own. The source still chooses that money -- the cash
// itself, a percentage of TradingView's margin equity at the fill for a flat
// opening, or of the hypothetical Flatten's realized balance for a reversal
// (ab9714be pine_orders.cpp:96-191) -- and still applies its lot floor; the
// conversion cash / (price * point value * fx) and the percentage fee reserve
// are the kernel's (NativeStrategyHost::native_sized_units), exactly as for a
// default quantity (case 8). Every booked quantity below was harvested from
// the adapter at fd785928, the tree before this lane touched src/source/,
// where the two resolve_terms branches still divided on their own.

// Scripted typed entries. Bar 0 opens a FIXED `seed` long when `seed` > 0;
// `bar`'s typed entry fills at the next open; `close_bar` flattens.
class TypedProbe : public pineforge::source::PineStrategyHost {
public:
    explicit TypedProbe(const Account& account) {
        initial_capital_ = account.capital;
        syminfo_.pointvalue = account.point_value;
        syminfo_mintick_ = account.mintick;
        qty_step_ = account.qty_step;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = account.fee_percent;
        slippage_ = account.slippage;
        pyramiding_ = 0;
        process_orders_on_close_ = false;
        margin_call_enabled_ = false;
    }
    double seed = 0.0;
    int bar = 2;
    bool is_long = true;
    double qty = kNaN;
    QtyType qty_type = QtyType::CASH;
    int close_bar = 4;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 && seed > 0.0) strategy_entry("Seed", true, kNaN, kNaN, seed);
        if (bar_index_ == bar) {
            strategy_entry(is_long ? "L" : "S", is_long, kNaN, kNaN, qty, "", "", 0,
                           static_cast<int>(qty_type));
        }
        if (bar_index_ == close_bar) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

// The kernel's conversion of `cash` at `price`, raw (ExplicitUnits), on the
// run spec the adapter projects for `account`.
double kernel_quotient(const Account& account, double cash, double price, bool reserve_fee) {
    CoreProbe core;
    if (core.configure_native(core_spec(account, "f7-typed")).status
        != NativeSetupStatus::Applied) {
        return kNaN;
    }
    no::Sized sized;
    sized.basis = no::CashValue{cash};
    sized.grid_policy = no::ExecutionGridPolicy::ExplicitUnits;
    sized.reserve_percent_fee = reserve_fee;
    const auto units = core.native_sized_units(sized, price, account.capital, 1.0);
    return units ? *units : kNaN;
}

std::string collapse_whitespace(const std::string& text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

void a_typed_quantity_is_the_kernel_quotient_under_the_source_floor() {
    // (a) A typed CASH opening from flat divides the cash by the fill price;
    //     this branch applies no lot floor of its own. (The placement
    //     admission at pine_adapter.cpp entry() prices the typed quantity as
    //     if it were units -- 1000 * 125 > 100000 drops a 1000-cash entry at
    //     125 -- so both fills stay at or below 100: a separate finding.)
    {
        struct Row { const char* name; double close; double previous; };
        const Row rows[] = {
            {"on-grid fill", 100.0, 10.0},
            {"off-grid fill", 99.99, 10.00100010001},
        };
        for (const Row& row : rows) {
            Account account;
            account.capital = 100000.0;
            const auto bars = flat_bars(row.close, 6);
            TypedProbe pine(account);
            pine.qty = 1000.0;
            pine.qty_type = QtyType::CASH;
            pine.run(bars.data(), static_cast<int>(bars.size()));
            REQUIRE(pine.trade_count() == 1);
            // The fill books at the source's chart-tick fill price (99.99 books
            // 99.990000000000009), which is the price the quantity divides by.
            const double booked = pine.rows()[0].qty;
            const double fill = pine.rows()[0].entry_price;
            CHECK(bits(booked) == bits(row.previous));
            CHECK(bits(kernel_quotient(account, 1000.0, fill, false)) == bits(booked));
            std::printf("  [typed cash %-13s] fill=%.17g booked=%.17g kernel=%.17g\n", row.name,
                        fill, booked, kernel_quotient(account, 1000.0, fill, false));
        }
    }
    // (b) A typed PERCENT opening from flat: 50 % of the margin equity, the
    //     0.1 % fee reserve, the source floor on a one-unit grid.
    //     5000 / 1.001 / 100 = 49.95 -> 49.
    {
        Account account;
        account.capital = 10000.0;
        account.qty_step = 1.0;
        account.fee_percent = 0.1;
        const auto bars = flat_bars(100.0, 6);
        TypedProbe pine(account);
        pine.qty = 50.0;
        pine.qty_type = QtyType::PERCENT_OF_EQUITY;
        pine.run(bars.data(), static_cast<int>(bars.size()));
        REQUIRE(pine.trade_count() == 1);
        const double booked = pine.rows()[0].qty;
        CHECK(bits(booked) == bits(49.0));
        CHECK(bits(pine.rows()[0].entry_price) == bits(100.0));
        const double quotient = kernel_quotient(account, 10000.0 * 50.0 / 100.0, 100.0, true);
        CHECK(bits(quotient) == bits(5000.0 / (1.0 + 0.1 / 100.0) / 100.0));
        CHECK(bits(floor_quantity_grid(quotient, account.qty_step)) == bits(booked));
        std::printf("  [typed percent flat ] booked=%.17g kernel=%.17g\n", booked, quotient);
    }
    // (c) A typed CASH reversal: 10 long at 100 reversed by a 500-cash short
    //     at 104. The closing row keeps the long's 10; the short opens
    //     floor(500 / 104) = 4 on the one-unit grid.
    {
        Account account;
        account.capital = 10000.0;
        account.qty_step = 1.0;
        std::vector<Bar> bars;
        for (int i = 0; i < 3; ++i) bars.push_back(mk_bar(i, 100.0, 100.0, 100.0, 100.0));
        for (int i = 3; i < 7; ++i) bars.push_back(mk_bar(i, 104.0, 104.0, 104.0, 104.0));
        TypedProbe pine(account);
        pine.seed = 10.0;
        pine.is_long = false;
        pine.qty = 500.0;
        pine.qty_type = QtyType::CASH;
        pine.close_bar = 5;
        pine.run(bars.data(), static_cast<int>(bars.size()));
        REQUIRE(pine.trade_count() == 2);
        CHECK(bits(pine.rows()[0].qty) == bits(10.0));
        const double booked = pine.rows()[1].qty;
        CHECK(bits(booked) == bits(4.0));
        CHECK(bits(pine.rows()[1].entry_price) == bits(104.0));
        const double quotient = kernel_quotient(account, 500.0, 104.0, false);
        CHECK(bits(floor_quantity_grid(quotient, account.qty_step)) == bits(booked));
        std::printf("  [typed cash reversal] booked=%.17g kernel=%.17g\n", booked, quotient);
    }
    // (d) A typed PERCENT reversal sizes from the hypothetical Flatten's
    //     realized balance, net of the 0.1 % reserve: 10 long at 100, reversed
    //     at 110 by a 50 % short. Balance 10000 + 100 - 1.0 (entry fee)
    //     - 1.1 (the flatten's fee) = 10097.9; 5048.95 / 1.001 / 110 = 45.85
    //     -> 45 on the one-unit grid.
    {
        Account account;
        account.capital = 10000.0;
        account.qty_step = 1.0;
        account.fee_percent = 0.1;
        std::vector<Bar> bars;
        for (int i = 0; i < 3; ++i) bars.push_back(mk_bar(i, 100.0, 100.0, 100.0, 100.0));
        for (int i = 3; i < 7; ++i) bars.push_back(mk_bar(i, 110.0, 110.0, 110.0, 110.0));
        TypedProbe pine(account);
        pine.seed = 10.0;
        pine.is_long = false;
        pine.qty = 50.0;
        pine.qty_type = QtyType::PERCENT_OF_EQUITY;
        pine.close_bar = 5;
        pine.run(bars.data(), static_cast<int>(bars.size()));
        REQUIRE(pine.trade_count() == 2);
        CHECK(bits(pine.rows()[0].qty) == bits(10.0));
        const double booked = pine.rows()[1].qty;
        CHECK(bits(booked) == bits(45.0));
        CHECK(bits(pine.rows()[1].entry_price) == bits(110.0));
        const double balance = 10000.0 + (110.0 - 100.0) * 10.0 - 1.0 - 1.1;
        const double quotient = kernel_quotient(account, balance * 50.0 / 100.0, 110.0, true);
        CHECK(bits(floor_quantity_grid(quotient, account.qty_step)) == bits(booked));
        std::printf("  [typed percent rev  ] booked=%.17g kernel=%.17g\n", booked, quotient);
    }
#if defined(PINEFORGE_R2_ADAPTER_FILE)
    // (e) The division and the reserve exist in the kernel alone: neither
    //     typed resolve_terms branch restates cash / (fill price * point value
    //     * fx) or the 1 + c / 100 divisor (the audit's M3 / A3-6 sites).
    const std::string adapter = collapse_whitespace(read_file(PINEFORGE_R2_ADAPTER_FILE));
    REQUIRE(!adapter.empty());
    for (const char* restated : {
             "1.0 + config_.commission_value / 100.0",
             "resolved_price * staged_.syminfo.pointvalue * facts.active_fx"}) {
        const bool found = adapter.find(restated) != std::string::npos;
        if (found) std::printf("  adapter still restates `%s`\n", restated);
        CHECK(!found);
    }
#else
    std::printf("  PINEFORGE_R2_ADAPTER_FILE undefined\n");
    CHECK(false);
#endif
}

// ===========================================================================
// 12. A staged FX series keeps the kernel's Sized path wherever the kernel
//     converts at the source's rate (R5 lane F7, M4).
// ===========================================================================
//
// The kernel freezes a Sized{AtAcceptance} quotient at the account FX of the
// acceptance coordinate (a script calculation's coordinate is the NEXT bar's
// open); the source sizes at the rate of the current sub-bar's open. On a
// constant curve, and on every bar of a stepped one but the bar whose next
// open is the step, those are one rate, so the command is lowered exactly as
// without a series; on the step's own bar the two differ and the command
// keeps its host-resolved intent. Until F7 any staged series withdrew the
// Sized path (design §4.4, N6 follow-up 2; audit M4). Booked quantities are
// harvested at fd785928 and do not move: which intent carries the opening is
// the only thing that changes.
void a_staged_fx_series_keeps_sized_where_the_rates_agree() {
    Account account;
    account.capital = 10000.0;
    account.qty_step = 1.0;
    const auto bars = flat_bars(100.0, 12);

    // (a) A one-point series restating the scalar rate: the same book as the
    //     run without a series, lowered the same way.
    PineProbe plain(account, QtyType::PERCENT_OF_EQUITY, 50.0);
    plain.script = "L.C.";
    plain.run(bars.data(), 4);
    PineProbe restated(account, QtyType::PERCENT_OF_EQUITY, 50.0);
    const std::int64_t one_ts[] = {0};
    const double one_rate[] = {1.0};
    REQUIRE(restated.set_account_currency_fx_series(one_ts, one_rate, 1));
    restated.script = "L.C.";
    restated.run(bars.data(), 4);
    REQUIRE(plain.trade_count() == 1);
    REQUIRE(restated.trade_count() == 1);
    REQUIRE(restated.core_sized_at_command.size() == 1);
    CHECK(plain.core_sized_at_command[0] == 1);
    CHECK(restated.core_sized_at_command[0] == 1);
    CHECK(bits(restated.rows()[0].qty) == bits(plain.rows()[0].qty));
    CHECK(bits(restated.rows()[0].qty) == bits(50.0));
    std::printf("  [fx one point] core_sized=%d booked=%.17g (no series: %d, %.17g)\n",
                restated.core_sized_at_command[0], restated.rows()[0].qty,
                plain.core_sized_at_command[0], plain.rows()[0].qty);

    // (b) On an aggregated chart (1-minute inputs, 5-minute script bars) a
    //     script calculation is accepted at the NEXT script bar's open while
    //     the source sizes at its own bar's open. A 1 -> 2 step at script bar
    //     4's open (1 200 000 ms): bar 0 converts at 1 on both sides; bar 3's
    //     calculation is accepted AT the step, where the kernel would convert
    //     at 2 and the source sized at 1, so it keeps its host-resolved
    //     intent; bar 7 is past the step on both sides.
    std::vector<Bar> minutes;
    for (int i = 0; i < 55; ++i) {
        Bar b = mk_bar(0, 100.0, 100.0, 100.0, 100.0);
        b.timestamp = 60000LL * i;
        minutes.push_back(b);
    }
    PineProbe stepped(account, QtyType::PERCENT_OF_EQUITY, 50.0);
    const std::int64_t step_ts[] = {0, 1200000};
    const double step_rate[] = {1.0, 2.0};
    REQUIRE(stepped.set_account_currency_fx_series(step_ts, step_rate, 2));
    stepped.script = "L.CL.C.L.C.";
    stepped.run(minutes.data(), static_cast<int>(minutes.size()), "1", "5", false);
    REQUIRE(stepped.last_error().empty());
    REQUIRE(stepped.core_sized_at_command.size() == 3);
    REQUIRE(stepped.trade_count() == 3);
    CHECK(stepped.core_sized_at_command[0] == 1);
    CHECK(stepped.core_sized_at_command[1] == 0);
    CHECK(stepped.core_sized_at_command[2] == 1);
    CHECK(bits(stepped.rows()[0].qty) == bits(50.0));
    CHECK(bits(stepped.rows()[1].qty) == bits(50.0));
    CHECK(bits(stepped.rows()[2].qty) == bits(25.0));
    std::printf("  [fx step] core_sized=%d,%d,%d booked=%.17g,%.17g,%.17g\n",
                stepped.core_sized_at_command[0], stepped.core_sized_at_command[1],
                stepped.core_sized_at_command[2], stepped.rows()[0].qty,
                stepped.rows()[1].qty, stepped.rows()[2].qty);

    // (c) Both rates the comparison reads are the kernel's own lookup
    //     (BacktestEngine::account_currency_fx_at over the engine's curve):
    //     the adapter no longer walks the staged series itself.
#if defined(PINEFORGE_R2_ADAPTER_FILE)
    const std::string adapter = read_file(PINEFORGE_R2_ADAPTER_FILE);
    REQUIRE(!adapter.empty());
    const bool walks_series = adapter.find("account_fx_per_quote") != std::string::npos;
    if (walks_series) std::printf("  adapter still walks account_fx_per_quote\n");
    CHECK(!walks_series);
#else
    std::printf("  PINEFORGE_R2_ADAPTER_FILE undefined\n");
    CHECK(false);
#endif
}

void test(const char* name, void (*fn)()) {
    const int before = failures;
    std::printf("-- %s\n", name);
    fn();
    if (failures != before) std::printf("   ^^ %d failure(s)\n", failures - before);
}

}  // namespace

int main() {
    test("the classification is live", the_adapter_lowers_exactly_the_paths_the_core_can_name);
    test("cash entry", a_cash_entry_is_the_core_quotient_under_the_source_floor);
    test("percent entry with fee and grid", a_percent_entry_with_fee_and_grid_is_the_core_quotient);
    test("default-sized reversal", a_default_sized_reversal_keeps_both_rows);
    test("50 % exit with siblings", a_fifty_percent_exit_with_siblings_is_not_representable);
    test("sizing price rule", the_sizing_price_rule_is_the_tick_ladder_not_the_fill_grid);
    test("re-lowering witness",
         the_source_layer_names_the_sizing_basis_and_the_core_names_no_source);
    test("the conversion exists once", the_conversion_exists_once);
    test("the source lot floors are not the kernel floor",
         the_source_lot_floors_are_not_the_kernel_floor);
    test("pure-stop default entry keeps its own branch",
         a_pure_stop_default_entry_keeps_its_own_sizing_branch);
    test("a typed quantity is the kernel quotient",
         a_typed_quantity_is_the_kernel_quotient_under_the_source_floor);
    test("a staged FX series keeps Sized where the rates agree",
         a_staged_fx_series_keeps_sized_where_the_rates_agree);
    std::printf("R5 R2 adapter sizing re-lowering: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
