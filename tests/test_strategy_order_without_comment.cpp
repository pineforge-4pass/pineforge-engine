/*
 * strategy_order called without a comment: the seven-argument form.
 *
 * test_strategy_order_comment.cpp adds an eighth argument, the order's comment,
 * and keeps this form as the call with an empty comment. This test pins what
 * the seven-argument form produces, with literals worked out from the bars
 * below: the trade rows (ids, direction, quantity, prices, profit, and times
 * where the order fills on the bar that places it) and empty comments, for an
 * order that fills at once, for limit and stop orders that wait for their
 * price, for two orders closed by one, for a cancelled and a replaced order,
 * and for the open trade a script leaves. It passed before the comment
 * existed and has to keep passing.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
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
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (false)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

Bar bar(int index, double open, double high, double low, double close) {
    return Bar{open, high, low, close, 1.0, 300000LL * index};
}

// Every price is a small integer, so every profit below is exact in binary64.
// The close rises 10, 11, 12 and stays; no bar reaches below its open except
// where a test builds its own bars.
std::vector<Bar> rising_bars() {
    return {bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 11.0, 11.0, 11.0, 11.0),
            bar(2, 12.0, 12.0, 12.0, 12.0), bar(3, 12.0, 12.0, 12.0, 12.0),
            bar(4, 12.0, 12.0, 12.0, 12.0), bar(5, 12.0, 12.0, 12.0, 12.0)};
}

class Probe final : public pineforge::source::PineStrategyHost {
public:
    // The host exposes the open-trade readers to derived classes only; the
    // rows read them from outside the class.
    using source::PineStrategyHost::open_trade_entry_comment;
    using source::PineStrategyHost::open_trade_entry_id;

    using Script = std::function<void(Probe&, int)>;

    Probe(bool close_on_bar, int pyramiding, Script script) : script_(std::move(script)) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = pyramiding;
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.slippage = 0;
        config.process_orders_on_close = close_on_bar;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_margin_call_enabled(false);
    }

    void on_source_bar(const Bar&) override { script_(*this, pine_bar_index()); }

private:
    Script script_;
};

void run(Probe& probe, const std::vector<Bar>& bars) {
    probe.run(bars.data(), static_cast<int>(bars.size()));
}

void test_an_order_that_fills_at_once_has_no_comment() {
    std::printf("test_an_order_that_fills_at_once_has_no_comment\n");
    const std::vector<Bar> bars = rising_bars();
    for (bool close_on_bar : {true, false}) {
        for (bool is_long : {true, false}) {
            std::printf("  %s, %s\n", close_on_bar ? "fills on the close" : "fills on the next open",
                        is_long ? "long" : "short");
            Probe probe(close_on_bar, 1, [is_long](Probe& p, int index) {
                if (index == 0) p.strategy_order("open", is_long, 1.0);
                if (index == 2) p.strategy_order("close", !is_long, 1.0);
            });
            run(probe, bars);
            CHECK(probe.last_error().empty());
            CHECK(probe.trade_count() == 1);
            if (probe.trade_count() != 1) continue;
            const Trade trade = probe.get_trade(0);
            CHECK(trade.is_long == is_long);
            CHECK(trade.entry_id == "open");
            CHECK(trade.exit_id == "close");
            CHECK(trade.entry_comment.empty());
            CHECK(trade.exit_comment.empty());
            CHECK(bits(trade.qty) == bits(1.0));
            // On the close the order fills at the close of the bar that places it,
            // otherwise at the open of the bar after.
            const double want_entry = close_on_bar ? 10.0 : 11.0;
            const double want_exit = 12.0;
            CHECK(bits(trade.entry_price) == bits(want_entry));
            CHECK(bits(trade.exit_price) == bits(want_exit));
            CHECK(bits(trade.pnl) == bits(is_long ? want_exit - want_entry : want_entry - want_exit));
            if (close_on_bar) {
                CHECK(trade.entry_time == bars[0].timestamp);
                CHECK(trade.exit_time == bars[2].timestamp);
            }
        }
    }
}

void test_pending_orders_have_no_comment() {
    std::printf("test_pending_orders_have_no_comment\n");
    // A buy limit at 9 waits through bar 1 and fills on bar 2, whose low reaches
    // it; a buy stop at 11 fills on bar 2, whose high reaches it.
    const std::vector<Bar> bars = {
        bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 10.0, 10.0, 10.0, 10.0),
        bar(2, 10.0, 12.0, 8.0, 10.0), bar(3, 10.0, 10.0, 10.0, 10.0),
        bar(4, 10.0, 10.0, 10.0, 10.0)};
    for (int kind = 0; kind < 2; ++kind) {
        const bool limit = kind == 0;
        std::printf("  %s order\n", limit ? "limit" : "stop");
        Probe probe(true, 1, [limit](Probe& p, int index) {
            if (index == 0) {
                if (limit) p.strategy_order("pending", true, 1.0, 9.0, kNaN);
                else p.strategy_order("pending", true, 1.0, kNaN, 11.0);
            }
            if (index == 3) p.strategy_order("close", false, 1.0);
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() != 1) continue;
        const Trade trade = probe.get_trade(0);
        CHECK(trade.is_long);
        CHECK(trade.entry_id == "pending");
        CHECK(trade.exit_id == "close");
        CHECK(trade.entry_comment.empty());
        CHECK(trade.exit_comment.empty());
        CHECK(bits(trade.entry_price) == bits(limit ? 9.0 : 11.0));
        CHECK(bits(trade.exit_price) == bits(10.0));
        CHECK(trade.entry_time == bars[2].timestamp);
        CHECK(trade.exit_time == bars[3].timestamp);
    }
}

void test_two_orders_closed_by_one() {
    std::printf("test_two_orders_closed_by_one\n");
    const std::vector<Bar> bars = rising_bars();
    Probe probe(true, 2, [](Probe& p, int index) {
        if (index == 0) p.strategy_order("a", true, 1.0);
        if (index == 1) p.strategy_order("b", true, 1.0);
        if (index == 2) p.strategy_order("out", false, 2.0);
    });
    run(probe, bars);
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 2);
    if (probe.trade_count() != 2) return;
    const Trade first = probe.get_trade(0);
    const Trade second = probe.get_trade(1);
    CHECK(first.entry_id == "a");
    CHECK(second.entry_id == "b");
    CHECK(first.exit_id == "out");
    CHECK(second.exit_id == "out");
    CHECK(first.entry_comment.empty() && second.entry_comment.empty());
    CHECK(first.exit_comment.empty() && second.exit_comment.empty());
    CHECK(bits(first.entry_price) == bits(10.0));
    CHECK(bits(second.entry_price) == bits(11.0));
    CHECK(bits(first.pnl) == bits(2.0));
    CHECK(bits(second.pnl) == bits(1.0));
}

void test_a_cancelled_and_a_replaced_order() {
    std::printf("test_a_cancelled_and_a_replaced_order\n");
    // Bar 3 reaches down to 4, so a buy limit at 5 placed earlier fills there.
    const std::vector<Bar> bars = {
        bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 10.0, 10.0, 10.0, 10.0),
        bar(2, 10.0, 10.0, 10.0, 10.0), bar(3, 10.0, 10.0, 4.0, 10.0),
        bar(4, 10.0, 10.0, 10.0, 10.0), bar(5, 10.0, 10.0, 10.0, 10.0)};
    {
        std::printf("  cancelled, then the same id at the market\n");
        Probe probe(true, 1, [](Probe& p, int index) {
            if (index == 0) p.strategy_order("x", true, 1.0, 5.0, kNaN);
            if (index == 1) p.strategy_cancel("x");
            if (index == 2) p.strategy_order("x", true, 1.0);
            if (index == 4) p.strategy_order("close", false, 1.0);
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() == 1) {
            const Trade trade = probe.get_trade(0);
            CHECK(trade.entry_id == "x");
            CHECK(trade.entry_comment.empty());
            CHECK(trade.exit_comment.empty());
            CHECK(bits(trade.entry_price) == bits(10.0));
            CHECK(trade.entry_time == bars[2].timestamp);
        }
    }
    {
        std::printf("  replaced by the same id with another limit\n");
        Probe probe(true, 1, [](Probe& p, int index) {
            if (index == 0) p.strategy_order("y", true, 1.0, 6.0, kNaN);
            if (index == 1) p.strategy_order("y", true, 1.0, 5.0, kNaN);
            if (index == 4) p.strategy_order("close", false, 1.0);
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() == 1) {
            const Trade trade = probe.get_trade(0);
            CHECK(trade.entry_id == "y");
            CHECK(trade.entry_comment.empty());
            CHECK(trade.exit_comment.empty());
            CHECK(bits(trade.entry_price) == bits(5.0));
            CHECK(trade.entry_time == bars[3].timestamp);
        }
    }
}

void test_the_open_trade_has_no_comment() {
    std::printf("test_the_open_trade_has_no_comment\n");
    const std::vector<Bar> bars = rising_bars();
    Probe probe(true, 1, [](Probe& p, int index) {
        if (index == 0) p.strategy_order("held", true, 1.0);
    });
    run(probe, bars);
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 0);
    CHECK(bits(probe.live_position_size()) == bits(1.0));
    CHECK(probe.open_trade_entry_id(0) == "held");
    CHECK(probe.open_trade_entry_comment(0).empty());
}

}  // namespace

int main() {
    test_an_order_that_fills_at_once_has_no_comment();
    test_pending_orders_have_no_comment();
    test_two_orders_closed_by_one();
    test_a_cancelled_and_a_replaced_order();
    test_the_open_trade_has_no_comment();
    std::printf("\n=== Results: %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
