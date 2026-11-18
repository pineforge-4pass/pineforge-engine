/*
 * strategy.order's comment, the eighth argument, reaches the trade list.
 *
 * TradingView shows an order's comment as the Signal text of its fills: the
 * entry row of a trade the order opens and the exit row of a trade it closes.
 * strategy_order takes the comment as its eighth argument, after oca_type; the
 * seven-argument form stays and is the call with an empty comment.
 *
 * Every expected comment below is the literal the script passes. The only rows
 * that compare the engine's output with itself are the state-digest rows, which
 * compare digests of runs that differ in the comment alone. The same scenarios
 * without a comment, with the trade rows pinned, are in
 * test_strategy_order_without_comment.cpp.
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
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/selected_window.h>
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

std::vector<Bar> rising_bars() {
    return {bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 11.0, 11.0, 11.0, 11.0),
            bar(2, 12.0, 12.0, 12.0, 12.0), bar(3, 12.0, 12.0, 12.0, 12.0),
            bar(4, 12.0, 12.0, 12.0, 12.0), bar(5, 12.0, 12.0, 12.0, 12.0)};
}

class Probe final : public pineforge::source::PineStrategyHost {
public:
    // The host exposes the open-trade readers to derived classes only; the
    // rows read them from outside the class.
    using source::PineStrategyHost::broker_state_hash;
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

// One-minute calendar bars for the selected-window rows below: every price is
// 100, so no order reaches a price it waits for and every fill is at 100.
constexpr std::int64_t kWindowT0 = 1704067200000LL;
constexpr std::int64_t kWindowMinute = 60000LL;

std::vector<Bar> flat_bars(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back(Bar{100.0, 100.0, 100.0, 100.0, 1.0, kWindowT0 + kWindowMinute * i});
    return bars;
}

// The selected window starts at bar `start_bar` and ends after bar `count - 1`.
void select_from(Probe& probe, int start_bar, int count) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = kWindowT0 + kWindowMinute * start_bar;
    window.end_ms = kWindowT0 + kWindowMinute * count;
    CHECK(strategy_set_selected_window_v1(static_cast<BacktestEngine*>(&probe), &window) == 0);
}

void test_the_comment_reaches_the_trade_list() {
    std::printf("test_the_comment_reaches_the_trade_list\n");
    const std::vector<Bar> bars = rising_bars();
    for (bool close_on_bar : {true, false}) {
        for (bool is_long : {true, false}) {
            std::printf("  %s, %s\n", close_on_bar ? "fills on the close" : "fills on the next open",
                        is_long ? "long" : "short");
            Probe probe(close_on_bar, 1, [is_long](Probe& p, int index) {
                if (index == 0)
                    p.strategy_order("open", is_long, 1.0, kNaN, kNaN, "", 0, "enter-note");
                if (index == 2)
                    p.strategy_order("close", !is_long, 1.0, kNaN, kNaN, "", 0, "leave-note");
            });
            run(probe, bars);
            CHECK(probe.last_error().empty());
            CHECK(probe.trade_count() == 1);
            if (probe.trade_count() != 1) continue;
            const Trade trade = probe.get_trade(0);
            CHECK(trade.is_long == is_long);
            CHECK(trade.entry_id == "open");
            CHECK(trade.exit_id == "close");
            CHECK(trade.entry_comment == "enter-note");
            CHECK(trade.exit_comment == "leave-note");
        }
    }
}

// The seven-argument form is the eight-argument one with an empty comment: the
// same trade rows, field by field.
void test_the_seven_argument_form_is_the_empty_comment() {
    std::printf("test_the_seven_argument_form_is_the_empty_comment\n");
    const std::vector<Bar> bars = rising_bars();
    for (bool close_on_bar : {true, false}) {
        for (bool is_long : {true, false}) {
            std::printf("  %s, %s\n", close_on_bar ? "fills on the close" : "fills on the next open",
                        is_long ? "long" : "short");
            std::vector<Trade> rows[2];
            for (int eight = 0; eight < 2; ++eight) {
                Probe probe(close_on_bar, 1, [is_long, eight](Probe& p, int index) {
                    if (index == 0) {
                        if (eight) p.strategy_order("open", is_long, 1.0, kNaN, kNaN, "", 0, std::string());
                        else p.strategy_order("open", is_long, 1.0, kNaN, kNaN, "", 0);
                    }
                    if (index == 2) {
                        if (eight) p.strategy_order("close", !is_long, 1.0, kNaN, kNaN, "", 0, std::string());
                        else p.strategy_order("close", !is_long, 1.0, kNaN, kNaN, "", 0);
                    }
                });
                run(probe, bars);
                CHECK(probe.last_error().empty());
                for (int i = 0; i < probe.trade_count(); ++i) rows[eight].push_back(probe.get_trade(i));
            }
            CHECK(rows[0].size() == 1);
            CHECK(rows[1].size() == 1);
            if (rows[0].size() != 1 || rows[1].size() != 1) continue;
            const Trade& seven = rows[0][0];
            const Trade& eight = rows[1][0];
            CHECK(seven.entry_comment.empty() && seven.exit_comment.empty());
            CHECK(eight.entry_comment.empty() && eight.exit_comment.empty());
            CHECK(seven.is_long == eight.is_long);
            CHECK(seven.entry_id == eight.entry_id && seven.exit_id == eight.exit_id);
            CHECK(seven.entry_time == eight.entry_time && seven.exit_time == eight.exit_time);
            CHECK(bits(seven.entry_price) == bits(eight.entry_price));
            CHECK(bits(seven.exit_price) == bits(eight.exit_price));
            CHECK(bits(seven.qty) == bits(eight.qty));
            CHECK(bits(seven.pnl) == bits(eight.pnl));
        }
    }
}

void test_pending_orders_keep_their_comment() {
    std::printf("test_pending_orders_keep_their_comment\n");
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
                if (limit) p.strategy_order("pending", true, 1.0, 9.0, kNaN, "", 0, "limit-note");
                else p.strategy_order("pending", true, 1.0, kNaN, 11.0, "", 0, "stop-note");
            }
            if (index == 3) p.strategy_order("close", false, 1.0, kNaN, kNaN, "", 0, "sell-note");
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() != 1) continue;
        const Trade trade = probe.get_trade(0);
        CHECK(trade.entry_id == "pending");
        CHECK(trade.exit_id == "close");
        CHECK(trade.entry_comment == (limit ? "limit-note" : "stop-note"));
        CHECK(trade.exit_comment == "sell-note");
        CHECK(trade.entry_time == bars[2].timestamp);
    }
}

// Each order carries its own comment: the entry rows of two orders differ, and
// the one order that closes both gives both exit rows its comment.
void test_each_order_has_its_own_comment() {
    std::printf("test_each_order_has_its_own_comment\n");
    const std::vector<Bar> bars = rising_bars();
    Probe probe(true, 2, [](Probe& p, int index) {
        if (index == 0) p.strategy_order("a", true, 1.0, kNaN, kNaN, "", 0, "first");
        if (index == 1) p.strategy_order("b", true, 1.0, kNaN, kNaN, "", 0, "second");
        if (index == 2) p.strategy_order("out", false, 2.0, kNaN, kNaN, "", 0, "both-out");
    });
    run(probe, bars);
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 2);
    if (probe.trade_count() != 2) return;
    const Trade first = probe.get_trade(0);
    const Trade second = probe.get_trade(1);
    CHECK(first.entry_id == "a" && first.entry_comment == "first");
    CHECK(second.entry_id == "b" && second.entry_comment == "second");
    CHECK(first.exit_id == "out" && first.exit_comment == "both-out");
    CHECK(second.exit_id == "out" && second.exit_comment == "both-out");
}

void test_the_open_trade_reports_the_comment() {
    std::printf("test_the_open_trade_reports_the_comment\n");
    const std::vector<Bar> bars = rising_bars();
    Probe probe(true, 1, [](Probe& p, int index) {
        if (index == 0) p.strategy_order("held", true, 1.0, kNaN, kNaN, "", 0, "held-note");
    });
    run(probe, bars);
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 0);
    CHECK(bits(probe.live_position_size()) == bits(1.0));
    CHECK(probe.open_trade_entry_id(0) == "held");
    CHECK(probe.open_trade_entry_comment(0) == "held-note");
}

// A comment belongs to the order that carried it: a cancelled order's comment
// does not reach a later order with the same id, and a replaced order fills with
// the comment of the order that replaced it.
void test_a_cancelled_or_replaced_order_leaves_no_comment_behind() {
    std::printf("test_a_cancelled_or_replaced_order_leaves_no_comment_behind\n");
    // Bar 3 reaches down to 4, so a buy limit at 5 placed earlier fills there.
    const std::vector<Bar> bars = {
        bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 10.0, 10.0, 10.0, 10.0),
        bar(2, 10.0, 10.0, 10.0, 10.0), bar(3, 10.0, 10.0, 4.0, 10.0),
        bar(4, 10.0, 10.0, 10.0, 10.0), bar(5, 10.0, 10.0, 10.0, 10.0)};
    {
        std::printf("  cancelled, then the same id at the market without a comment\n");
        Probe probe(true, 1, [](Probe& p, int index) {
            if (index == 0) p.strategy_order("x", true, 1.0, 5.0, kNaN, "", 0, "stale");
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
            CHECK(trade.entry_time == bars[2].timestamp);
        }
    }
    {
        std::printf("  replaced by the same id with another limit and another comment\n");
        Probe probe(true, 1, [](Probe& p, int index) {
            if (index == 0) p.strategy_order("y", true, 1.0, 6.0, kNaN, "", 0, "stale");
            if (index == 1) p.strategy_order("y", true, 1.0, 5.0, kNaN, "", 0, "fresh");
            if (index == 4) p.strategy_order("close", false, 1.0);
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() == 1) {
            const Trade trade = probe.get_trade(0);
            CHECK(trade.entry_id == "y");
            CHECK(trade.entry_comment == "fresh");
            CHECK(trade.exit_comment.empty());
            CHECK(bits(trade.entry_price) == bits(5.0));
            CHECK(trade.entry_time == bars[3].timestamp);
        }
    }
}

// The comment is part of what the engine keeps for a pending order, so it is
// part of the state hash: orders that differ only in their comment leave
// different states, and the same order twice leaves the same state.
void test_a_pending_orders_comment_is_part_of_the_state() {
    std::printf("test_a_pending_orders_comment_is_part_of_the_state\n");
    // The buy limit at 5 is never reached, so the order is still pending at the end.
    const std::vector<Bar> bars = {
        bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 10.0, 10.0, 10.0, 10.0),
        bar(2, 10.0, 10.0, 10.0, 10.0), bar(3, 10.0, 10.0, 10.0, 10.0)};
    const auto state_with = [&bars](const char* comment) {
        Probe probe(true, 1, [comment](Probe& p, int index) {
            if (index == 0) p.strategy_order("rest", true, 1.0, 5.0, kNaN, "", 0, comment);
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 0);
        CHECK(bits(probe.live_position_size()) == bits(0.0));
        return probe.broker_state_hash();
    };
    const std::uint64_t first = state_with("note-a");
    const std::uint64_t again = state_with("note-a");
    const std::uint64_t other = state_with("note-b");
    const std::uint64_t none = state_with("");
    CHECK(first == again);
    CHECK(first != other);
    CHECK(first != none);
}

// A comment is text: bytes that are not UTF-8 are refused as every other
// command's comment is, and the run reports the refusal.
void test_a_comment_that_is_not_utf8_is_refused() {
    std::printf("test_a_comment_that_is_not_utf8_is_refused\n");
    const std::vector<Bar> bars = rising_bars();
    Probe probe(true, 1, [](Probe& p, int index) {
        if (index == 0) p.strategy_order("open", true, 1.0, kNaN, kNaN, "", 0, std::string("\xff\xfe"));
    });
    run(probe, bars);
    CHECK(!probe.last_error().empty());
    CHECK(probe.trade_count() == 0);
    CHECK(bits(probe.live_position_size()) == bits(0.0));
}

// The pending-order mirror is a public struct a caller reads while an order
// waits: a resting order's comment is in it, and an order without one leaves
// the field empty.
void test_a_resting_orders_comment_reaches_the_pending_mirror() {
    std::printf("test_a_resting_orders_comment_reaches_the_pending_mirror\n");
    // The buy limit at 5 is never reached, so the order is still pending at the end.
    const std::vector<Bar> bars = {
        bar(0, 10.0, 10.0, 10.0, 10.0), bar(1, 10.0, 10.0, 10.0, 10.0),
        bar(2, 10.0, 10.0, 10.0, 10.0), bar(3, 10.0, 10.0, 10.0, 10.0)};
    std::uint64_t comment_hash[2] = {0, 0};
    for (int with_comment = 0; with_comment < 2; ++with_comment) {
        std::printf("  %s\n", with_comment ? "with a comment" : "seven arguments");
        Probe probe(true, 1, [with_comment](Probe& p, int index) {
            if (index != 0) return;
            if (with_comment) p.strategy_order("rest", true, 1.0, 5.0, kNaN, "", 0, "mirror-note");
            else p.strategy_order("rest", true, 1.0, 5.0, kNaN, "", 0);
        });
        run(probe, bars);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 0);
        const auto& view = probe.pending_intent_view();
        CHECK(view.size() == 1);
        if (view.size() != 1) continue;
        pf_pending_order_v1_t row{};
        CHECK(view.copy_v1(0, &row) == 0);
        CHECK(row.is_long == 1);
        CHECK(std::strcmp(row.comment, with_comment ? "mirror-note" : "") == 0);
        CHECK(row.comment_truncated == 0);
        comment_hash[with_comment] = row.comment_hash64;
    }
    CHECK(comment_hash[0] != comment_hash[1]);
}

// A selected window replays what its pre-roll placed. The order placed on the
// last bar before the window carries its comment through the replay into the
// trade list; one placed without a comment leaves the entry row empty.
void test_a_pre_roll_order_keeps_its_comment_through_the_replay() {
    std::printf("test_a_pre_roll_order_keeps_its_comment_through_the_replay\n");
    const int count = 8;
    const std::vector<Bar> bars = flat_bars(count);
    for (int with_comment = 0; with_comment < 2; ++with_comment) {
        std::printf("  %s\n", with_comment ? "with a comment" : "seven arguments");
        Probe probe(true, 1, [with_comment](Probe& p, int index) {
            if (index == 2) {
                if (with_comment) p.strategy_order("pre", true, 1.0, kNaN, kNaN, "", 0, "pre-note");
                else p.strategy_order("pre", true, 1.0, kNaN, kNaN, "", 0);
            }
            if (index == 5) p.strategy_order("close", false, 1.0, kNaN, kNaN, "", 0, "post-note");
        });
        select_from(probe, 3, count);
        probe.run(bars.data(), count, "1", "1", false);
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() != 1) continue;
        const Trade trade = probe.get_trade(0);
        CHECK(trade.is_long);
        CHECK(trade.entry_id == "pre");
        CHECK(trade.entry_comment == (with_comment ? "pre-note" : ""));
        CHECK(trade.exit_id == "close");
        CHECK(trade.exit_comment == "post-note");
        CHECK(bits(trade.entry_price) == bits(100.0));
        CHECK(bits(trade.exit_price) == bits(100.0));
    }
}

// The state a paused selected run keeps for the replay holds the order's
// comment: the digest of that state tells two comments apart, and an order
// without a comment digests as it did before the comment existed (its literal is
// in test_strategy_order_without_comment.cpp).
std::uint64_t retained_state_hash(int form) {
    Probe probe(true, 1, [form](Probe& p, int index) {
        if (index != 2) return;
        switch (form) {
            case 0: p.strategy_order("pre", true, 1.0, kNaN, kNaN, "", 0); break;
            case 1: p.strategy_order("pre", true, 1.0, kNaN, kNaN, "", 0, std::string()); break;
            case 2: p.strategy_order("pre", true, 1.0, kNaN, kNaN, "", 0, "note-a"); break;
            default: p.strategy_order("pre", true, 1.0, kNaN, kNaN, "", 0, "note-b"); break;
        }
    });
    select_from(probe, 3, 8);
    const std::vector<Bar> warmup = flat_bars(3);
    const bool started = probe.stream_begin(warmup.data(), 3, "1", "1");
    CHECK(started);
    CHECK(probe.last_error().empty());
    return probe.broker_state_hash();
}

void test_a_retained_pre_roll_orders_comment_is_part_of_the_state() {
    std::printf("test_a_retained_pre_roll_orders_comment_is_part_of_the_state\n");
    const std::uint64_t seven = retained_state_hash(0);
    const std::uint64_t empty = retained_state_hash(1);
    const std::uint64_t first = retained_state_hash(2);
    const std::uint64_t again = retained_state_hash(2);
    const std::uint64_t other = retained_state_hash(3);
    std::printf("  retained state hash, seven arguments: %016llx\n",
                static_cast<unsigned long long>(seven));
    CHECK(seven == empty);
    CHECK(first == again);
    CHECK(first != seven);
    CHECK(first != other);
}

}  // namespace

int main() {
    test_the_comment_reaches_the_trade_list();
    test_the_seven_argument_form_is_the_empty_comment();
    test_pending_orders_keep_their_comment();
    test_each_order_has_its_own_comment();
    test_the_open_trade_reports_the_comment();
    test_a_cancelled_or_replaced_order_leaves_no_comment_behind();
    test_a_pending_orders_comment_is_part_of_the_state();
    test_a_comment_that_is_not_utf8_is_refused();
    test_a_resting_orders_comment_reaches_the_pending_mirror();
    test_a_pre_roll_order_keeps_its_comment_through_the_replay();
    test_a_retained_pre_roll_orders_comment_is_part_of_the_state();
    std::printf("\n=== Results: %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
