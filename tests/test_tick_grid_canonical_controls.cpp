#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

// Controls for the observed-tick stop-limit fill on ticks other than 0.01, in
// the same shape: the stop prints at its level beyond the limit and activates
// the order, a later print at the limit enters one unit on that bar, and the
// fill books RN(n * tick), the binary64 product of the print's grid index n
// (floor for a buy, ceil for a sell). The kernel admits every product here
// against its limit, so each row books that product, bit for bit, before and
// after the stop-limit repair; a change to that booking turns a row red.
//
// RN is binary64 round to nearest, ties to even; a price in [2^e, 2^(e+1))
// has ulp 2^(e-52).
// - 0.05 * 2^56 = 3602879701896396.8, so RN(0.05) = 3602879701896397 * 2^-56
//   = 0.05 * (1 + 2^-54), and n * RN(0.05) = (n / 20) * (1 + 2^-54) exactly.
//   n = 20: 1 + 2^-54, a quarter ulp above 1, rounds to 1.
//   n = 23: 1.15 * 2^52 = 5179139571476070.4, so RN(1.15) = ...070 * 2^-52
//   = 0x1.2666666666666p+0, and the product is ...070.4 + 1.15 * 2^-2 =
//   ...070.6875 ulp (in integers 23 * 3602879701896397 = 82866233143617131
//   = 16 * 5179139571476070 + 11), which rounds to ...071:
//   0x1.2666666666667p+0 = 1.1500000000000001, one ULP above the sell limit.
//   A sell admits it, and the fill books it as is: the booked price keeps the
//   binary product by design, since TradingView evaluates its equity and
//   margin thresholds with that product.
//   n = 4000 (the better sell print 200): 200 + 200 * 2^-54 = 200 + 0.39 ulp,
//   which rounds to 200.
// - 0.25 is exact, and so are 5 * 0.25 = 1.25 and 800 * 0.25 = 200.
// - 0.0004 * 2^64 = 7378697629483820.6464, so RN(0.0004) = 7378697629483821
//   * 2^-64 = 0.0004 + 0.3536 * 2^-64. n = 2500: 1 + 884 * 2^-64 = 1 + 0.2158
//   ulp, which rounds to 1. n = 500000 (the better sell print 200): 200 +
//   176800 * 2^-64 = 200 + 0.3372 ulp, which rounds to 200.
// - The better buy print is one tick, n = 1: the product is the tick itself.
// Each print p is the decimal literal of its grid point, and p / tick lies
// within 2e-10 of n (three roundings of relative size at most 2^-53,
// n <= 500000), so floor(p / tick + 1e-9) and ceil(p / tick - 1e-9) both
// select n. Every row passes before and after the repair.

namespace {
using namespace pineforge;
constexpr std::int64_t kHour = 60 * 60 * 1000;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

struct Case {
    const char* name;
    bool is_long;
    double tick;
    double stop;
    double limit;     // also the print that reaches it after activation
    double index;     // its grid index n, derived above
    double product;   // RN(n * tick), derived above: the booked entry price
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
    const double scaled = row.limit / row.tick;
    const double index = row.is_long ? std::floor(scaled + 1e-9) : std::ceil(scaled - 1e-9);
    const double product = index * row.tick;
    const bool admitted = row.is_long ? product <= row.limit : product >= row.limit;
    const bool ok = index == row.index && product == row.product && admitted;
    std::printf("%s geometry %s: tick=%.17g index=%.17g (derived %.17g) "
                "product=%a (derived %a) admitted=%s\n",
                ok ? "PASS" : "FAIL", row.name, row.tick, index, row.index, product,
                row.product, admitted ? "true" : "false");
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
        set_syminfo_mintick(row.tick);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const Bar&) override {
        // Warm up at 0, submit at streamed close 1, activate in 2.
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
    const double before = row.limit + (row.is_long ? -row.tick : row.tick);
    const Bar warmup{before, before, before, before, 1, 0};
    require_stream(probe.stream_begin(&warmup, 1, "60", "60"), probe, "begin");
    std::uint64_t sequence = 0;
    const int touch_bar = input == Input::DelayedTouch ? 3 : 2;
    // One tick is strictly below every buy limit, and 200 is strictly above
    // every sell limit; both are on every grid here (n derived above). These
    // mutants must change the entry-price bound.
    const double reached = input == Input::BetterPrint
        ? (row.is_long ? row.tick : 200.0) : row.limit;
    for (int bar = 1; bar <= 5; ++bar) {
        std::array<double, 4> prices{{before, before, before, before}};
        if (bar >= 2) {
            prices.fill(reached);
            if (input == Input::UnreachedStop) {
                prices.fill(row.limit);
            } else if (input == Input::UntouchedLimit || bar < touch_bar) {
                prices.fill(row.stop);
            } else if (bar == touch_bar) {
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
    // At bar 2 the stop print activates, but is worse than the limit and
    // cannot fill. The next print is exactly the written limit: entry is
    // bar 2 at its product, one unit. DelayedTouch moves that print to bar 3.
    // UnreachedStop removes activation; UntouchedLimit removes every
    // eligible print: either gives zero trades. BetterPrint falsifies price,
    // DoubleQuantity falsifies qty=1, EarlierClose falsifies exit_bar=4+1.
    // Closing at close 4 first executes on open 5, so the trade is closed.
    const bool fills = input != Input::UnreachedStop && input != Input::UntouchedLimit;
    const int expected_count = fills ? 1 : 0;
    const int expected_bar = fills ? (input == Input::DelayedTouch ? 3 : 2) : -1;
    const double expected_price = fills
        ? (input == Input::BetterPrint ? (row.is_long ? row.tick : 200.0) : row.product)
        : kNa;
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
    std::printf("%s %s/%s tick=%.17g limit=%.17g: "
                "expected trades=%d entry_bar=%d entry_price=%.17g (%a); "
                "actual trades=%d entry_bar=%d entry_price=%.17g (%a); "
                "position=%.17g pending=%d%s%s\n",
                ok ? "PASS" : "FAIL", row.name, input_name(input), row.tick, row.limit,
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
    const Case cases[] = {
        // 20 * RN(0.05) = 1; the stops lie one tick beyond the limit.
        {"long-nickel", true, 0.05, 1.05, 1.00, 20, 1.00},
        {"short-nickel", false, 0.05, 0.95, 1.00, 20, 1.00},
        // 23 * RN(0.05) = 1.1500000000000001, one ULP above the sell limit
        // 1.15 and admitted by it: booked as is, the binary product.
        {"short-nickel-1.15", false, 0.05, 1.10, 1.15, 23, 0x1.2666666666667p+0},
        // 5 * 0.25 = 1.25, exact.
        {"long-quarter", true, 0.25, 1.50, 1.25, 5, 1.25},
        {"short-quarter", false, 0.25, 1.00, 1.25, 5, 1.25},
        // 2500 * RN(0.0004) = 1; the stops differ by one tick from 1.
        {"long-small-tick", true, 0.0004, 1.0004, 1.0000, 2500, 1.0000},
        {"short-small-tick", false, 0.0004, 0.9996, 1.0000, 2500, 1.0000},
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
    std::printf("%s stop-limit tick grid controls: %d geometry checks, %d cases, "
                "%d failures\n", failures ? "FAIL" : "PASS", geometry_count, count, failures);
    return failures == 0 ? 0 : 1;
}
