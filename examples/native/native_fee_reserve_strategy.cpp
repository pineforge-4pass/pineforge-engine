// Pine-free native example: a cash-sized opening that reserves its own
// commission (Sized::reserve_percent_fee), from a JPY account trading a
// USD-quoted stock.
//
// The account holds 3,000,000 JPY. The stock is quoted in USD, converted at a
// scalar account_fx of 150 JPY per USD, on a 0.05 tick ladder with whole-share
// lots (quantity_grid 1). The broker charges a percent of the account notional
// on every execution: fee_kind Percent with fee_value 0.1, which is a PERCENT
// -- 0.1 %, not 10 %.
//
// The host names what the opening is worth, 1,497,700 JPY, and the kernel
// resolves the shares once, at acceptance (SizeTime::AtAcceptance), against
// the decision close on the instrument's own ladder (SizePrice::SignalOnTick:
// 212.33 -> 212.35), so one share costs 212.35 * 150 = 31,852.50 JPY:
//
//   without the reserve   1,497,700 / 31,852.50          = 47.02 -> 47 shares
//   with the reserve      1,497,700 / 1.001 / 31,852.50  = 46.97 -> 46 shares
//
// 47 shares are 1,497,067.50 JPY of notional and pay a 1,497.07 JPY entry fee
// on top: 864.57 JPY more than the cash the host named. The reserve divides
// the cash by (1 + fee) first, so the shares AND the commission they pay fit
// inside it -- here that costs one share. native_sized_units() answers the
// same resolution as a pure query, before anything is submitted.
//
// Both runs then trade the same tape -- in at 212.45, out at 213.05 -- and
// every leg pays 0.1 % of its own account notional:
//
//   46 shares  entry 1,465.905  exit 1,470.045  commission 2,935.950
//              gross (213.05 - 212.45) * 46 * 150 = 4,140  -> net 1,204.050
//   47 shares  entry 1,497.7725 exit 1,502.0025 commission 2,999.775
//              gross (213.05 - 212.45) * 47 * 150 = 4,230  -> net 1,230.225
//
// Each fill reports the commission it booked (ExecutionAppliedEvent::
// current_ticket, account currency); the open lot carries the entry leg's
// (NativeOpenLot::entry_commission); the closed row carries both.
//
// Under any other fee kind the reserve is an exact no-op: a CashPerExecution
// broker's ticket is not a fraction of the notional, so there is nothing to
// divide by.
//
//   c++ -std=c++17 native_fee_reserve_strategy.cpp -lpineforge_kernel -o fee_reserve
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>

namespace {

namespace no = pineforge::native_order;

constexpr double kCash = 1497700.0;        // JPY the host spends on the opening
constexpr double kFx = 150.0;              // JPY per USD
constexpr double kFeePercent = 0.1;        // 0.1 % of the account notional
constexpr double kSizingPrice = 212.35;    // the 212.33 close on the 0.05 ladder
constexpr double kEntryPrice = 212.45;     // the next bar's open
constexpr double kExitPrice = 213.05;      // the open after the flatten

// Money is compared to a relative 1e-9: binary64 carries the JPY amounts
// above to about 1e-10 of their size, and nothing here rounds to a currency.
bool money(double value, double expected) {
    return std::isfinite(value) && std::fabs(value - expected) <= 1e-9 * std::fabs(expected);
}

// The commission one leg of `units` shares pays at `price`, in JPY.
double leg_fee(double units, double price) {
    return units * price * kFx * kFeePercent / 100.0;
}

no::Sized cash_opening(bool reserve) {
    no::Sized sized;
    sized.side = no::Side::Long;
    sized.basis = no::CashValue{kCash};
    sized.time = no::SizeTime::AtAcceptance;
    sized.price = no::SizePrice::SignalOnTick;
    sized.reserve_percent_fee = reserve;
    return sized;
}

class FeeReserveExample : public pineforge::NativeStrategyHost {
public:
    explicit FeeReserveExample(bool reserve) : reserve_(reserve) {}

    // Read back by main().
    std::optional<double> preview_reserved;    // native_sized_units, reserve on
    std::optional<double> preview_plain;       // native_sized_units, reserve off
    double entry_units = 0.0;
    double entry_price = 0.0;
    double entry_ticket = 0.0;                 // the entry leg's commission
    double exit_ticket = 0.0;                  // the exit leg's commission
    double lot_entry_commission = 0.0;         // what the open lot still carries

private:
    bool reserve_ = false;
    int bar_ = -1;
    std::optional<no::RequestHandle> entry_;
    std::optional<no::RequestHandle> exit_;

    void on_native_run_begin() override { bar_ = -1; }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == 0) {
            // The kernel's own resolution as a query, both ways, at the
            // sizing price the submit below freezes. The equity argument is
            // only read by an EquityFraction basis.
            const double equity = native_marked_equity(bar.close);
            preview_reserved = native_sized_units(cash_opening(true), kSizingPrice, equity, kFx);
            preview_plain = native_sized_units(cash_opening(false), kSizingPrice, equity, kFx);
            entry_ = submit({cash_opening(reserve_), "entry", "fee-reserve"}).handle;
        } else if (bar_ == 2) {
            // The lot the entry opened still carries the entry leg's fee.
            const auto lots = native_open_lots(bar.close);
            assert(lots.size() == 1);
            lot_entry_commission = lots[0].entry_commission;
            exit_ = submit({no::Flatten{}, "exit", "fee-reserve"}).handle;
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (entry_ && event.handle() == *entry_) {
            entry_units = event.opened_units;
            entry_price = event.resolved_price;
            entry_ticket = event.current_ticket;
        } else if (exit_ && event.handle() == *exit_) {
            exit_ticket = event.current_ticket;
        }
    }
};

pineforge::NativeRunSpec make_spec(pineforge::NativeFeeKind fee_kind, double fee_value) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-fee-reserve-example";
    spec.identity.run_number = 1;
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "ACME";
    spec.tickerid = "TEST:ACME";
    spec.type = "stock";
    spec.currency = "USD";           // the instrument's quote currency
    spec.basecurrency = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 3000000.0;  // the account, in JPY
    spec.point_value = 1.0;
    spec.account_fx = kFx;           // JPY per USD: quote -> account
    spec.price_tick = 0.05;
    spec.fee_kind = fee_kind;
    spec.fee_value = fee_value;
    spec.quantity_grid = 1.0;        // whole shares: the sized units floor onto it
    return spec;
}

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;

// open, high, low, close, volume, timestamp (Unix milliseconds).
const pineforge::Bar kBars[] = {
    {212.12, 212.40, 211.95, 212.33, 900.0, 0 * kQuarter},    // decide: size and submit
    {212.45, 212.70, 212.30, 212.62, 1200.0, 1 * kQuarter},   // entry fills at the open
    {212.62, 212.95, 212.55, 212.90, 1500.0, 2 * kQuarter},   // read the lot, flatten
    {213.05, 213.35, 212.80, 213.20, 1800.0, 3 * kQuarter},   // exit fills at the open
};
constexpr int kBarCount = 4;

// What one run leaves behind: its closed row and how many it closed.
struct Outcome {
    pineforge::Trade trade;
    int closed_trades = 0;
};

// One run over the tape; everything it asserts is printed first.
Outcome run_once(bool reserve) {
    FeeReserveExample host(reserve);
    const auto setup = host.configure_native(make_spec(pineforge::NativeFeeKind::Percent,
                                                       kFeePercent));
    assert(setup.status == pineforge::NativeSetupStatus::Applied);
    host.run(kBars, kBarCount);
    assert(host.native_state().kind == pineforge::NativeLifecycleKind::Completed);

    const double units = reserve ? 46.0 : 47.0;
    std::printf("%s: preview %g / %g shares (reserve / none); filled %g @ %.2f\n",
                reserve ? "reserve" : "no reserve",
                host.preview_reserved.value_or(-1.0), host.preview_plain.value_or(-1.0),
                host.entry_units, host.entry_price);
    assert(host.preview_reserved == 46.0 && host.preview_plain == 47.0);
    assert(host.entry_units == units && host.entry_price == kEntryPrice);

    // The commission, leg by leg: what each fill booked, what the open lot
    // kept of the entry's, and what the closed row sums.
    assert(host.trade_count() == 1);
    const pineforge::Trade trade = host.get_trade(0);
    std::printf("  entry fee %.4f (lot keeps %.4f)  exit fee %.4f  row commission %.4f  "
                "net %.4f\n",
                host.entry_ticket, host.lot_entry_commission, host.exit_ticket,
                trade.commission, trade.pnl);
    assert(money(host.entry_ticket, leg_fee(units, kEntryPrice)));
    assert(money(host.lot_entry_commission, host.entry_ticket));
    assert(money(host.exit_ticket, leg_fee(units, kExitPrice)));
    assert(money(trade.commission, leg_fee(units, kEntryPrice) + leg_fee(units, kExitPrice)));
    assert(money(trade.commission, reserve ? 2935.95 : 2999.775));
    assert(trade.qty == units && trade.entry_price == kEntryPrice
           && trade.exit_price == kExitPrice);
    assert(money(trade.pnl, (kExitPrice - kEntryPrice) * units * kFx - trade.commission));
    assert(money(trade.pnl, reserve ? 1204.05 : 1230.225));
    return {trade, host.trade_count()};
}

}  // namespace

int main() {
    // The arithmetic the reserve exists for: 47 shares at the sizing price
    // overspend the cash once their own fee is paid; 46 do not.
    const double share = kSizingPrice * kFx;
    const double with_fee = 1.0 + kFeePercent / 100.0;
    std::printf("one share %.2f JPY; 47 shares + fee %.4f, 46 shares + fee %.4f, cash %.2f\n",
                share, 47.0 * share * with_fee, 46.0 * share * with_fee, kCash);
    assert(47.0 * share * with_fee > kCash && 46.0 * share * with_fee <= kCash);

    const Outcome reserved = run_once(true);
    const Outcome plain = run_once(false);
    // Same tape, same prices: only the share count -- and with it every
    // money figure -- differs.
    assert(reserved.closed_trades == plain.closed_trades);
    assert(reserved.trade.entry_time == plain.trade.entry_time
           && reserved.trade.exit_time == plain.trade.exit_time);

    // Any other fee kind: the reserve changes nothing. A 250 JPY ticket per
    // execution is not a fraction of the notional, so the query resolves the
    // same 47 shares with the flag on as off.
    FeeReserveExample cash_ticket(true);
    const auto setup = cash_ticket.configure_native(
            make_spec(pineforge::NativeFeeKind::CashPerExecution, 250.0));
    assert(setup.status == pineforge::NativeSetupStatus::Applied);
    const auto flat_fee_units =
            cash_ticket.native_sized_units(cash_opening(true), kSizingPrice, kCash, kFx);
    std::printf("CashPerExecution: reserve on -> %g shares\n", flat_fee_units.value_or(-1.0));
    assert(flat_fee_units == 47.0);

    // The summary line, printed only once every check above has passed.
    std::printf("reserve %g shares, none %g  commission %.4f / %.4f  closed trades: %d per run\n",
                reserved.trade.qty, plain.trade.qty, reserved.trade.commission,
                plain.trade.commission, reserved.closed_trades);
    return 0;
}
