// With process_orders_on_close, a same-bar close before an explicit reversal
// entry keeps the held size: the reversing entry opens the requested quantity
// plus the position held when it was placed, and the new position's
// stop/limit exit closes all of it. Two command sequences on synthetic
// confirmed 5-minute bars: one reversal, and two chained reversals.
// No Pine evaluator or corpus is required.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kOrderQuantity = 1.0;
constexpr double kRiskReward = 1.5;
constexpr std::int64_t kStart = 1'736'121'600'000LL;
constexpr std::int64_t kBarMs = 300'000;
int checks = 0;
int failures = 0;
const char* current_case = "";

#define CHECK(expr) do { ++checks; if (!(expr)) { \
    ++failures; std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", \
                            current_case, __FILE__, __LINE__, #expr); \
} } while (false)

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 1e-8;
}

Bar mk(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0,
            kStart + static_cast<std::int64_t>(index) * kBarMs};
}

// Derive the opening size from the expected path, never from the engine's
// actual size: an incorrect first reversal must not lower the second oracle.
double reversal_quantity(double expected_position_before_close) {
    return kOrderQuantity + std::abs(expected_position_before_close);
}

class ReversalHost final : public source::PineStrategyHost {
public:
    explicit ReversalHost(bool chained) : chained_(chained) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        config.pyramiding = 0;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = config.margin_short = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.05);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        if (index == 0) {
            place(false, bar.close, chained_ ? 25650.0 : 24750.0);
        } else if (index == 1) {
            place(true, bar.close, chained_ ? 25500.0 : 24642.5);
        } else if (index == 2 && chained_) {
            place(false, bar.close, 25611.7);
        }
    }

private:
    void place(bool is_long, double entry_price, double stop) {
        const double position = live_position_size();
        const bool opposite = is_long ? position < 0.0 : position > 0.0;
        if (opposite) {
            strategy_close(is_long ? "Short" : "Long",
                           is_long ? "Reverse Short to Long" : "Reverse Long to Short");
            strategy_cancel(is_long ? "SLTP-Short" : "SLTP-Long");
        }
        const char* id = is_long ? "Long" : "Short";
        strategy_entry(id, is_long, kNaN, kNaN, kOrderQuantity);
        const double risk = std::abs(entry_price - stop);
        const double limit = entry_price + (is_long ? 1.0 : -1.0) * risk * kRiskReward;
        strategy_exit(is_long ? "SLTP-Long" : "SLTP-Short", id, limit, stop);
    }

    bool chained_;
};

void check_trade(const ReversalHost& host, int index, bool is_long,
                 double qty, double entry_price, double exit_price,
                 const char* exit_id) {
    CHECK(host.trade_count() > index);
    if (host.trade_count() <= index) return;
    const auto trade = host.get_trade(index);
    CHECK(trade.is_long == is_long);
    CHECK(trade.entry_id == (is_long ? "Long" : "Short"));
    CHECK(trade.exit_id == exit_id);
    CHECK(near(trade.entry_price, entry_price));
    CHECK(near(trade.exit_price, exit_price));
    CHECK(near(trade.qty, qty));
}

void test_close_then_reversal_quantity() {
    current_case = "close-then-reverse";
    ReversalHost host(false);
    // Resting short brackets cannot touch on the reversal bar. Only the last
    // bar reaches the new long stop; its open avoids a gap-price stop fill.
    const Bar bars[] = {
        mk(0, 24621.2, 24621.2, 24621.2, 24621.2),
        mk(1, 24673.95, 24673.95, 24673.95, 24673.95),
        mk(2, 24673.95, 24673.95, 24642.5, 24642.5),
    };
    const double short_qty = kOrderQuantity;                       // opening short
    const double long_qty = reversal_quantity(-short_qty);         // reversal: 1 + |-1|

    const bool began = host.stream_begin(bars, 1, "5", "5");
    CHECK(began);
    CHECK(host.last_error().empty());
    if (!began) return;
    CHECK(near(host.live_position_size(), -short_qty));            // short entry

    CHECK(host.stream_push_bar(bars[1]));
    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), long_qty));              // reversing long entry
    check_trade(host, 0, false, short_qty,24621.2, 24673.95,
                "__close__Short");                               // short closed by the reversal

    CHECK(host.stream_push_bar(bars[2]));
    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));                   // stop closes the whole long
    CHECK(host.trade_count() == 2);
    check_trade(host, 1, true, long_qty, 24673.95, 24642.5,
                "SLTP-Long");                                    // long's stop exit
    CHECK(host.stream_end(false));
}

void test_chained_close_then_reversal_quantity() {
    current_case = "chained-reversals";
    ReversalHost host(true);
    // The long stop stays below the second reversal, and all limits remain
    // out of range. Only the final bar reaches the fresh short's stop.
    const Bar bars[] = {
        mk(0, 25526.3, 25526.3, 25526.3, 25526.3),
        mk(1, 25588.55, 25588.55, 25588.55, 25588.55),
        mk(2, 25543.45, 25543.45, 25543.45, 25543.45),
        mk(3, 25543.45, 25611.7, 25543.45, 25611.7),
    };
    const double initial_short_qty = kOrderQuantity;              // opening short
    const double long_qty = reversal_quantity(-initial_short_qty); // first reversal: 1 + |-1|
    const double final_short_qty = reversal_quantity(long_qty);    // second reversal: 1 + |2|

    const bool began = host.stream_begin(bars, 1, "5", "5");
    CHECK(began);
    CHECK(host.last_error().empty());
    if (!began) return;
    CHECK(near(host.live_position_size(), -initial_short_qty));    // short entry

    CHECK(host.stream_push_bar(bars[1]));
    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), long_qty));              // first reversal opens the long
    check_trade(host, 0, false, initial_short_qty, 25526.3, 25588.55,
                "__close__Short");                               // short closed by the first reversal

    CHECK(host.stream_push_bar(bars[2]));
    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), -final_short_qty));      // second reversal opens the short
    check_trade(host, 1, true, long_qty, 25588.55, 25543.45,
                "__close__Long");                                // long closed by the second reversal

    CHECK(host.stream_push_bar(bars[3]));
    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));                   // stop closes the whole short
    CHECK(host.trade_count() == 3);
    check_trade(host, 2, false, final_short_qty, 25543.45, 25611.7,
                "SLTP-Short");                                   // short's stop exit
    CHECK(host.stream_end(false));
}

}  // namespace

int main() {
    test_close_then_reversal_quantity();
    test_chained_close_then_reversal_quantity();
    std::printf("test_pooc_explicit_close_reversal_qty: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
