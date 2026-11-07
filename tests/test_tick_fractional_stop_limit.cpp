#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

// A stop-limit entry on observed ticks at tick 0.01. The stop is printed at
// its own level, beyond the limit, so the order activates there and cannot
// fill; a later print reaches the limit and one unit enters at that print on
// that bar. The fill projects the print onto its grid index n (floor for a
// buy, ceil for a sell) and books the binary64 product RN(n * tick) whenever
// the kernel admits it against the order's immutable limit (a buy at or
// below it, a sell at or above it). That product is booked as is by design:
// TradingView evaluates its equity and margin thresholds with it, bit for
// bit. For the long limits 99.99, 100.07 and 1.15 the product lies one ULP
// above the limit, so the kernel refused the fill as invalid terms and the
// order never traded, with no error. There the print is on the grid, and the
// fill must book its grid spelling, the double of n / 100: the print itself.
//
// RN is binary64 round to nearest, ties to even; a price in [2^e, 2^(e+1))
// has ulp 2^(e-52). 0.01 * 2^59 = 5764607523034234.88, so the tick is
// RN(0.01) = 5764607523034235 * 2^-59 = 0.01 + 0.12 * 2^-59 and, exactly,
// n * RN(0.01) = n / 100 + d ulp with d = 0.12 * n * 2^(-7-e). With
// F = (n / 100) / ulp, RN(n / 100) = round(F) and RN(n * tick) = round(F + d):
//
//   n      e  F                     d       F + d         RN(n * tick)
//   9999   6  7036170730324623.36   0.1465  ...623.5065   RN(99.99) + 1 ulp
//   10007  6  7041800229858836.48   0.1466  ...836.6266   RN(100.07) + 1 ulp
//   115    0  5179139571476070.4    0.1078  ...070.5078   RN(1.15) + 1 ulp
//   10001  6  7037578105208176.64   0.1465  ...176.7865   RN(100.01)
//   9900   6  6966505673588736      0.1450  ...736.1450   99
//   10100  6  7107243161944064      0.1479  ...064.1479   101
//   7     -4  5044031582654955.52   0.1050  ...955.6250   RN(0.07)
//   29    -2  5224175567749775.36   0.1088  ...775.4688   RN(0.29)
//   20000  7  7036874417766400      0.1465  ...400.1465   200
//
// The 115 row in integers: 115 * 5764607523034235 = 662929865148937025, and
// 662929865148937025 / 2^7 = 5179139571476070.5078125. So the buy rows at
// 99.99 (0x1.8ff5c28f5c28fp+6), 100.07 (0x1.9047ae147ae14p+6) and 1.15
// (0x1.2666666666666p+0) project one ULP above their limits, to
// 0x1.8ff5c28f5c290p+6, 0x1.9047ae147ae15p+6 and 0x1.2666666666667p+0; a sell
// at 1.15 admits 0x1.2666666666667p+0 = 1.1500000000000001; every other
// product equals its limit. Each print p below is RN(n / 100), and
// p / RN(0.01) lies within 1e-11 of n (three roundings of relative size at
// most 2^-53, n <= 20000), so floor(p / tick + 1e-9) and ceil(p / tick - 1e-9)
// both select n, and n / 100 rounds back to p.
//
// Before the fix the three refused rows print, for each of touch,
// delayed-touch (entry_bar=3), double-quantity and earlier-close:
//   FAIL long-fraction-99.99/touch: expected trades=1 entry_bar=2
//   entry_price=99.989999999999995 (0x1.8ff5c28f5c28fp+6); actual trades=0
//   entry_bar=-1 entry_price=nan (nan); position=0 pending=0
// and likewise with 100.06999999999999 (0x1.9047ae147ae14p+6) and
// 1.1499999999999999 (0x1.2666666666666p+0): the refusal is terminal, so
// nothing stays pending. Their better-print input passes there too (one tick,
// 1 * RN(0.01), is admitted by every buy limit), as do unreached-stop and
// untouched-limit, which never enter. Every other row passes before and after.

namespace {
using namespace pineforge;
constexpr std::int64_t kHour = 60 * 60 * 1000;
constexpr double kTick = 0.01;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

struct Case {
    const char* name;
    bool is_long;
    double stop;
    double limit;
    double print;     // the print that reaches the limit after activation
    double index;     // its grid index n, derived above
    double product;   // RN(n * tick), derived above
    double expected;  // the booked entry: the product, or n / 100 if refused
};

enum class Input {
    Touch, DelayedTouch, BetterPrint, UnreachedStop, UntouchedLimit, DoubleQuantity, EarlierClose
};

const char* input_name(Input input) {
    switch (input) {
    case Input::Touch: return "touch";
    case Input::DelayedTouch: return "delayed-touch";
    case Input::BetterPrint: return "better-print";
    case Input::UnreachedStop: return "unreached-stop";
    case Input::UntouchedLimit: return "untouched-limit";
    case Input::DoubleQuantity: return "double-quantity";
    case Input::EarlierClose: return "earlier-close";
    }
    throw std::logic_error("unknown input");
}

// The derivation above, checked on its own arithmetic before any order runs:
// a slip in it fails here, whatever the engine does, not as an order row.
bool geometry(const Case& row) {
    const double scaled = row.print / kTick;
    const double index = row.is_long ? std::floor(scaled + 1e-9) : std::ceil(scaled - 1e-9);
    const double product = index * kTick;
    const double decimal = index / 100;
    const bool ok = index == row.index && product == row.product && decimal == row.print;
    std::printf("%s geometry %s: index=%.17g (derived %.17g) product=%a (derived %a) "
                "decimal=%a (print %a)\n",
                ok ? "PASS" : "FAIL", row.name, index, row.index, product, row.product,
                decimal, row.print);
    return ok;
}

class Probe final : public source::PineStrategyHost {
public:
    Probe(const Case& row, Input input) : row_(row), input_(input) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.commission_value = 0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_mintick(kTick);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const Bar&) override {
        // Index 0 is historical warmup. Submit once at streamed close 1,
        // before the activation prints in 2. Never resubmit a rejected order.
        if (bar_index_ == 1)
            strategy_entry("Entry", row_.is_long, row_.limit, row_.stop,
                           input_ == Input::DoubleQuantity ? 2 * 1.0 : 1.0);
        if (bar_index_ == (input_ == Input::EarlierClose ? 3 : 4)) {
            strategy_cancel("Entry");
            if (signed_position_size() != 0) strategy_close("Entry");
        }
    }

private:
    Case row_;
    Input input_;
};

void require_stream(bool ok, const Probe& probe, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + probe.last_error());
}

void drive(Probe& probe, const Case& row, Input input) {
    // Prices begin strictly on the untriggered side: buy < stop, sell > stop.
    // The first streamed slot starts one hour after non-negative warmup 0.
    const double before = row.limit + (row.is_long ? -kTick : kTick);
    const Bar warmup{before, before, before, before, 1, 0};
    require_stream(probe.stream_begin(&warmup, 1, "60", "60"), probe, "begin");
    std::uint64_t sequence = 0;
    const int touch_bar = input == Input::DelayedTouch ? 3 : 2;
    // Input mutants use exact, strictly better grid prints: 1 cent < every
    // buy limit, 200 > every sell limit. These falsify the entry-price bound.
    const double reached = input == Input::BetterPrint
        ? (row.is_long ? 0.01 : 200.0) : row.print;
    for (int bar = 1; bar <= 5; ++bar) {
        std::array<double, 4> prices{{before, before, before, before}};
        if (bar >= 2) {
            prices.fill(reached);
            if (input == Input::UnreachedStop) {
                prices.fill(row.print);
            } else if (input == Input::UntouchedLimit || bar < touch_bar) {
                prices.fill(row.stop);
            } else if (bar == touch_bar) {
                // Distinct observed prints, not an interpolated OHLC path:
                // reach stop at equality, then reach the limit.
                prices = {{row.stop, row.stop, reached, reached}};
            }
        }
        for (std::size_t i = 0; i < prices.size(); ++i) {
            const TradeTick tick{bar * kHour + static_cast<std::int64_t>(i) * (kHour / 4),
                                 ++sequence, prices[i], 1.0 / 4};
            require_stream(probe.stream_push_tick(tick), probe, "push tick");
        }
        require_stream(probe.stream_advance_time((bar + 1) * kHour), probe, "seal close");
    }
    require_stream(probe.stream_end(false), probe, "end");
    if (!probe.last_error().empty() || probe.last_run_status() != 0)
        throw std::runtime_error("run status " + std::to_string(probe.last_run_status())
                                 + ": " + probe.last_error());
}

bool check(const Case& row, Input input) {
    // Stop equality in bar 2 activates the order, but cannot fill it:
    // buy stop > limit, sell stop < limit. The first later eligible print
    // reaches the limit, so one unit enters at that print on bar 2.
    // Delaying the return until bar 3 changes only the expected entry bar.
    // A close submitted on bar 4 sees its first eligible print at open 5;
    // five streamed bars therefore produce exactly one CLOSED trade.
    // Removing either activation or every limit touch instead gives zero.
    // Two further input mutants double the requested unit or move the close
    // from 4 to 3: quantity becomes 2*1, or exit becomes 3+1 instead of 4+1.
    // BetterPrint books 1 * RN(0.01) = 0.01 for a buy and 200 for a sell
    // (n = 20000 above), each admitted by every limit here.
    const bool fills = input != Input::UnreachedStop && input != Input::UntouchedLimit;
    const int expected_count = fills ? 1 : 0;
    const int expected_bar = fills ? (input == Input::DelayedTouch ? 3 : 2) : -1;
    const double expected_price = fills
        ? (input == Input::BetterPrint ? (row.is_long ? 0.01 : 200.0) : row.expected) : kNa;
    const double expected_qty = input == Input::DoubleQuantity ? 2 * 1.0 : 1.0;
    const int expected_exit = input == Input::EarlierClose ? 3 + 1 : 4 + 1;
    Probe probe(row, input);
    std::string error;
    try {
        drive(probe, row, input);
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    const int actual_count = probe.trade_count();
    int actual_bar = -1;
    double actual_price = kNa;
    bool trade_ok = true;
    if (actual_count > 0) {
        const auto trade = probe.get_trade(0);
        actual_bar = trade.entry_bar_index;
        actual_price = trade.entry_price;
        const bool within_limit = row.is_long
            ? actual_price <= row.limit : actual_price >= row.limit;
        trade_ok = fills && actual_bar == expected_bar
            && std::isfinite(actual_price) && actual_price == expected_price
            && within_limit && trade.qty == expected_qty
            && !trade.open_at_end && trade.exit_bar_index == expected_exit;
    }
    const bool ok = error.empty() && actual_count == expected_count && trade_ok
        && probe.live_position_size() == 0 && probe.pending_order_count() == 0;
    std::printf("%s %s/%s: expected trades=%d entry_bar=%d entry_price=%.17g (%a); "
                "actual trades=%d entry_bar=%d entry_price=%.17g (%a); "
                "position=%.17g pending=%d%s%s\n",
                ok ? "PASS" : "FAIL", row.name, input_name(input),
                expected_count, expected_bar, expected_price, expected_price,
                actual_count, actual_bar, actual_price, actual_price,
                probe.live_position_size(), probe.pending_order_count(),
                error.empty() ? "" : "; error=", error.c_str());
    if (!ok && actual_count > 0) {
        const auto trade = probe.get_trade(0);
        std::printf("  expected qty=%.17g closed=true exit_bar=%d; "
                    "actual qty=%.17g closed=%s exit_bar=%d\n",
                    expected_qty, expected_exit, trade.qty,
                    trade.open_at_end ? "false" : "true", trade.exit_bar_index);
    }
    return ok;
}
} // namespace

int main() {
    // Every quote is a decimal literal, the double nearest its cents, never
    // built as n * mintick. Whole-price controls share stop 100 with their
    // fractional counterparts. For small quotes, adjacent decimal cents keep
    // the stop strictly outside the limit; all stops are actually printed
    // after the submission close.
    const Case cases[] = {
        // Refused before the fix: the buy product is one ULP above the limit.
        // The fill books the print's grid spelling, n / 100 = the limit.
        {"long-fraction-99.99", true, 100.00, 99.99, 99.99, 9999,
         0x1.8ff5c28f5c290p+6, 99.99},
        {"long-fraction-100.07", true, 100.08, 100.07, 100.07, 10007,
         0x1.9047ae147ae15p+6, 100.07},
        {"long-fraction-1.15", true, 1.16, 1.15, 1.15, 115,
         0x1.2666666666667p+0, 1.15},
        // The same product admitted by a sell limit at 1.15 is booked as is:
        // 1.1500000000000001, one ULP above the limit and the print.
        {"short-fraction-1.15", false, 1.14, 1.15, 1.15, 115,
         0x1.2666666666667p+0, 0x1.2666666666667p+0},
        // Products equal to their limits, admitted and booked as is.
        {"short-fraction-100.01", false, 100.00, 100.01, 100.01, 10001, 100.01, 100.01},
        {"long-whole-99", true, 100.00, 99.00, 99.00, 9900, 99.00, 99.00},
        {"short-whole-101", false, 100.00, 101.00, 101.00, 10100, 101.00, 101.00},
        {"long-fraction-0.07", true, 0.08, 0.07, 0.07, 7, 0.07, 0.07},
        {"short-fraction-0.07", false, 0.06, 0.07, 0.07, 7, 0.07, 0.07},
        {"long-fraction-0.29", true, 0.30, 0.29, 0.29, 29, 0.29, 0.29},
        {"short-fraction-0.29", false, 0.28, 0.29, 0.29, 29, 0.29, 0.29},
        // Off the grid: the limit 99.995 lies half a tick between grid
        // points, past the entry's 0.0001-point snap, so the kernel's limit
        // is RN(99.995) = 7036522574045512 * 2^-46 (F = 7036522574045511.68).
        // The on-grid print 99.99 reaches it and projects to n = 9999, whose
        // product 0x1.8ff5c28f5c290p+6 = 99.990000000000009 is below that
        // limit: admitted, and booked as is, one ULP above the print.
        {"long-offgrid-limit-99.995", true, 100.01, 99.995, 99.99, 9999,
         0x1.8ff5c28f5c290p+6, 0x1.8ff5c28f5c290p+6},
    };
    int failures = 0;
    int geometry_count = 0;
    for (const auto& row : cases) {
        ++geometry_count;
        if (!geometry(row)) ++failures;
    }
    int count = 0;
    for (const auto& row : cases) {
        for (Input input : {Input::Touch, Input::DelayedTouch, Input::BetterPrint,
                            Input::UnreachedStop, Input::UntouchedLimit,
                            Input::DoubleQuantity, Input::EarlierClose}) {
            ++count;
            if (!check(row, input)) ++failures;
        }
    }
    std::printf("%s observed tick stop-limit: %d geometry checks, %d cases, %d failures\n",
                failures ? "FAIL" : "PASS", geometry_count, count, failures);
    return failures == 0 ? 0 : 1;
}
