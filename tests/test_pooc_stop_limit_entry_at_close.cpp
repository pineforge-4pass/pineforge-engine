#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kTick = 0.01;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
enum class Entry {
    StopLimit, Limit, Stop, BelowLimit, UnreachedStop, Gap,
    BothReached, StopReachedLimitWaiting
};
enum class Input { ModeledBars, Ticks };

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<Bar> bars(bool is_long = true) {
    std::vector<Bar> result;
    for (int i = 0; i < 112; ++i) {
        Bar bar{100, 100, 100, 100, 1, i * 3600000LL};
        if (i >= 101 && i <= 103) bar = {110, 112, 109, 110, 1, bar.timestamp};
        if (i == 104) bar = {110, 112, 100, 110, 1, bar.timestamp};
        if (i >= 105) bar = {110, 110, 110, 110, 1, bar.timestamp};
        if (!is_long) {
            // Reflect every price about 100: p' = 2*100-p. The up-gap to
            // 110 becomes a down-gap to 90; highs and lows exchange roles.
            bar = {200-bar.open, 200-bar.low, 200-bar.high, 200-bar.close,
                   bar.volume, bar.timestamp};
        }
        result.push_back(bar);
    }
    return result;
}

class Probe final : public source::PineStrategyHost {
public:
    explicit Probe(Entry entry, bool is_long = true, int slippage = 0,
                   bool cancel_on_next_bar = false, double limit_distance = kTick)
        : entry_(entry), is_long_(is_long), cancel_on_next_bar_(cancel_on_next_bar),
          limit_distance_(limit_distance) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.commission_value = 0;
        config.slippage = slippage;
        configure_pine_strategy(config);
        set_syminfo_mintick(kTick);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const Bar& bar) override {
        const int index = bar_index_ - input_offset();
        if (index < 0) return;
        const bool flat = signed_position_size() == 0;
        if (flat && index % 50 == 0 && (entry_ != Entry::Gap || index == 100)) {
            double limit = bar.close;
            double stop = bar.close;
            const double direction = is_long_ ? 1 : -1;
            if (entry_ == Entry::Limit) stop = kNa;
            if (entry_ == Entry::Stop) limit = kNa;
            if (entry_ == Entry::BelowLimit) limit -= direction*limit_distance_;
            if (entry_ == Entry::UnreachedStop) { stop += 0.5; limit += 1; }
            if (entry_ == Entry::Gap) stop += 0.5;
            if (entry_ == Entry::BothReached) { stop -= direction*0.5; limit += direction; }
            if (entry_ == Entry::StopReachedLimitWaiting) { stop -= 0.5; limit -= 1; }
            strategy_entry(is_long_ ? "Long" : "Short", is_long_, limit, stop);
        }
        if (cancel_on_next_bar_ && index == 1)
            strategy_cancel(is_long_ ? "Long" : "Short");
        if (!flat && index % 50 == 10) strategy_close(is_long_ ? "Long" : "Short");
    }

    double remaining() const { return live_position_size(); }
    int pending() const { return pending_order_count(); }
    void use_ticks() { ticks_ = true; }
    int input_offset() const { return ticks_ ? 1 : 0; }
    int close_fill_bar(int submitted_bar) const { return submitted_bar + (ticks_ ? 1 : 0); }

private:
    Entry entry_;
    bool is_long_;
    bool cancel_on_next_bar_;
    double limit_distance_;
    bool ticks_ = false;
};

void run(Probe& probe, const std::vector<Bar>& input, Input mode) {
    if (mode == Input::ModeledBars) {
        probe.run(input.data(), static_cast<int>(input.size()), "60", "60", false);
    } else {
        // Shift tick input forward one hour so the preceding warmup is
        // non-negative; constructed bar indices and OHLC stay unchanged.
        // Four ordered prints reproduce each constructed bar's OHLC exactly;
        // offsets are i*(3600000/4) ms, with volume/4 per print. Explicit
        // time advances seal the closes without a next-slot print.
        const auto& first = input.front();
        probe.use_ticks();
        const Bar warmup{first.open, first.open, first.open, first.open, 1,
                         first.timestamp};
        if (!probe.stream_begin(&warmup, 1, "60", "60"))
            throw std::runtime_error("tick stream begin failed: " + probe.last_error());
        std::uint64_t sequence = 0;
        for (const auto& bar : input) {
            const auto timestamp = bar.timestamp + 3600000;
            const std::array<double, 4> prices{{bar.open, bar.high, bar.low, bar.close}};
            for (std::size_t i = 0; i < prices.size(); ++i)
                require(probe.stream_push_tick(
                    {timestamp + static_cast<std::int64_t>(i)*900000,
                     ++sequence, prices[i], bar.volume/4}), "tick input refused");
            require(probe.stream_advance_time(timestamp + 3600000),
                    "tick close advance failed");
        }
        require(probe.stream_end(false), "tick stream end failed");
    }
    require(probe.last_error().empty(), "run error: " + probe.last_error());
    require(probe.last_run_status() == 0, "run status must be successful");
}

void check_trade(const Probe& probe, int index, int entry, double price, int exit_submission) {
    const auto trade = probe.get_trade(index);
    // The tick stream's one warmup bar precedes constructed input index zero.
    entry += probe.input_offset();
    // In this raw-label tick stream, advance_time seals the slot at
    // (j+2)*hour, the order's birth-time floor. Its quote is the last print at
    // (j+1)*hour+3*hour/4, before that floor. The next open is the first
    // eligible print, so a close submitted on j fills on j+1, plus warmup.
    const int exit = probe.close_fill_bar(exit_submission) + probe.input_offset();
    if (trade.entry_bar_index != entry || trade.entry_price != price) {
        char diagnostic[200];
        std::snprintf(diagnostic, sizeof diagnostic,
            "entry[%d]: expected bar %d price %.12g; actual bar %d price %.12g",
            index, entry, price, trade.entry_bar_index, trade.entry_price);
        require(false, diagnostic);
    }
    require(trade.exit_bar_index == exit, "exit bar differs");
    require(trade.qty == 1, "fixed entry quantity differs");
    require(!trade.open_at_end, "expected a closed trade");
}

void check_close_entries(Input mode, Entry kind, bool is_long = true, int slippage = 0) {
    // At each multiple of 50 the book is flat and C=100. A buy stop at C
    // activates at equality; its buy limit at C is simultaneously marketable.
    // POOC therefore enters at [0,50,100] @ 100. The first later remainder
    // 10 closes at [10,60,110], giving 1 + (100-0)/50 = 3 closed trades.
    // The same inequalities separately establish the limit and stop controls.
    // Mirroring gives sell stop C and sell limit C at the same three closes;
    // the final exit is at 90 instead of 110, still on bar 110.
    // Unequal reached levels are stop=C-direction*0.5, limit=C+direction.
    // A pure stop alone slips by direction*slippage*tick; a limit never does.
    // In ticks, orders born at closes 0,50,100 first see opens 1,51,101.
    // Opens 1 and 51 are 100. The last open gaps to 110 (90 short): a pure
    // stop fills there, while every limit here waits for the first return
    // print, low104=100 (high104=100 short). No tick interpolates at 101.
    // Market closes submitted on 10,60,110 first see opens 11,61,111.
    Probe probe(kind, is_long, slippage);
    const auto input = bars(is_long);
    run(probe, input, mode);
    require(probe.trade_count() == 3, "expected three closed trades");
    const std::array<int, 3> entries{{0, 50, 100}};
    const std::array<int, 3> exits{{10, 60, 110}};
    // Express slipped prints as tick counts: 100/0.01=10000 entry ticks,
    // with +/-slippage for an entry, and the opposite sign for a market exit.
    for (int i = 0; i < 3; ++i) {
        const int entry = mode == Input::Ticks
            ? (i == 2 && kind != Entry::Stop ? 104 : entries[i] + 1) : entries[i];
        const double basis = mode == Input::Ticks ? input[entry].open : 100;
        const double price = kind == Entry::Stop
            ? (basis/kTick + (is_long ? 1 : -1)*slippage)*kTick : 100;
        check_trade(probe, i, entry, price, exits[i]);
        const double exit_quote = mode == Input::Ticks
            ? input[probe.close_fill_bar(exits[i])].open : input[exits[i]].close;
        const double exit_price =
            (exit_quote/kTick - (is_long ? 1 : -1)*slippage)*kTick;
        require(probe.get_trade(i).exit_price == exit_price, "exit close price differs");
    }
    require(probe.remaining() == 0, "expected a flat final book");
}

void check_unmarketable_limit(Input mode, bool is_long = true) {
    // Lowering the limit by the configured tick gives 100-0.01=99.99.
    // C=100 reaches the stop but exceeds that buy limit. Every later low is
    // >=100, including the return at 104, so no order can fill anywhere.
    // The mirror raises the sell limit to 100+0.01=100.01, while all later
    // highs are <=100. It must likewise never sell below its limit.
    Probe probe(Entry::BelowLimit, is_long);
    run(probe, bars(is_long), mode);
    require(probe.trade_count() == 0, "below-close limit must never fill");
    require(probe.remaining() == 0, "below-close limit must leave the book flat");
}

void check_unreached_stop(Input mode) {
    // Stops at 100+0.5 are unreached at closes 0,50,100. All prices through
    // bar 100 are 100. At open 101=110 the last same-id request activates,
    // but 110 exceeds its 100+1=101 limit. Lows 109 on 101..103 cannot fill;
    // bar 104 first crosses 101 on its way to low 100. The next exit is 110.
    // Tick input has no print at 101: its first print <=limit101 is low104
    // itself, 100, so it buys at that better price and exits at open111.
    Probe probe(Entry::UnreachedStop);
    run(probe, bars(), mode);
    require(probe.trade_count() == 1, "unreached stop must wait for the gap and return");
    check_trade(probe, 0, 104, mode == Input::Ticks ? 100 : 101, 110);
    require(probe.remaining() == 0, "expected a flat final book");

    // Input mutant: low103=limit101 reaches the limit one bar earlier.
    // Ticks now book the actual print101, so both entry104 and price100
    // are falsifiable bounds; modeled matching still books limit101.
    auto earlier_input = bars();
    earlier_input[103].low = 101;
    Probe earlier(Entry::UnreachedStop);
    run(earlier, earlier_input, mode);
    require(earlier.trade_count() == 1, "earlier return must fill once");
    check_trade(earlier, 0, 103, 101, 110);
    require(earlier.remaining() == 0, "earlier return must leave a flat final book");
}

void check_gap(Input mode) {
    // Place only at 100, with stop=100+0.5 and limit=100. The close cannot
    // activate it. Open 101=110 activates; lows 109 through 103 stay above
    // the limit. Low 104=100 reaches the working limit, filling at 100.
    Probe probe(Entry::Gap);
    run(probe, bars(), mode);
    require(probe.trade_count() == 1, "gap order must fill once on its return");
    check_trade(probe, 0, 104, 100, 110);

    // Input mutant: low103=limit100 moves the first touch from104 to103.
    // Raising only bar111 to close110+1 separates the next-open market exit
    // (ticks:111) from the submitted close (modeled:110), without changing
    // the entry or exit command schedule.
    auto earlier_input = bars();
    earlier_input[103].low = 100;
    auto& next_open = earlier_input[111];
    next_open.open = next_open.high = next_open.low = next_open.close =
        earlier_input[110].close + 1;
    Probe earlier(Entry::Gap);
    run(earlier, earlier_input, mode);
    require(earlier.trade_count() == 1, "earlier gap return must fill once");
    check_trade(earlier, 0, 103, 100, 110);
    const double exit_quote = mode == Input::Ticks ? next_open.open : earlier_input[110].close;
    require(earlier.get_trade(0).exit_price == exit_quote, "market exit quote differs");
    require(earlier.remaining() == 0, "earlier gap return must leave a flat final book");
}

std::vector<Bar> activated_limit_bars(bool is_long, double limit_distance = kTick) {
    auto input = bars(is_long);
    input.resize(12); // Bars 0..11 include the scheduled close at 10.
    // Limit=100 +/- distance; the default gives 99.99 long or 100.01 short.
    // All later OHLC equal that limit, strictly beyond the stop in the
    // direction that cannot activate a pending stop-limit.
    const double limit = 100 + (is_long ? -limit_distance : limit_distance);
    for (std::size_t i = 1; i < input.size(); ++i) {
        input[i].open = input[i].high = input[i].low = input[i].close = limit;
    }
    return input;
}

void check_activated_stop_limit_rests_as_limit(Input mode, bool is_long) {
    // Close0=100 reaches stop100 but not the unequal limit. Activation must
    // survive the close: long low1=99.99 reaches buy limit99.99; mirrored
    // short high1=100.01 reaches sell limit100.01. Each open equals its limit,
    // so ordinary limit matching enters at that price on 1. Neither side's
    // stop can be reached again. The first scheduled close is 10, at the same
    // price, leaving one closed unit trade, no pending order and a flat book.
    // Tick input cannot activate at the placement close: that quote precedes
    // the birth-time floor. Every subsequent long print is <stop100, and
    // every short print is >stop100. Both must remain pending without a fill.
    auto input = activated_limit_bars(is_long);
    Probe probe(Entry::BelowLimit, is_long);
    run(probe, input, mode);
    const bool ticks = mode == Input::Ticks;
    const int expected_trades = ticks ? 0 : 1;
    require(probe.trade_count() == expected_trades, ticks
        ? "unreached tick stop must remain pending without a trade"
        : (is_long
            ? "long activated stop-limit must rest as a limit: expected one closed trade"
            : "short activated stop-limit must rest as a limit: expected one closed trade"));
    const double limit = 100 + (is_long ? -kTick : kTick);
    // A market close books floor(close/tick+0.5)*tick, not the raw print.
    // With zero slippage this is 9999*0.01 long or 10001*0.01 short.
    const double exit_price = std::floor(input[10].close/kTick + 0.5)*kTick;
    if (!ticks) {
        check_trade(probe, 0, 1, limit, 10);
        require(probe.get_trade(0).exit_price == exit_price,
                "activated limit must exit at the scheduled close price");
    }
    require(probe.remaining() == 0, "activated limit must leave a flat final book");
    require(probe.pending() == (ticks ? 1 : 0), "stop-limit pending count differs");

    // Input mutant: an open one tick better than the limit gives 99.98 long
    // or 100.02 short: (100/0.01 +/- 2)*0.01 expresses executable tick prices.
    // Ordinary gap improvement changes the entry price, while the unchanged
    // close10 still exits at 99.99 or 100.01.
    // The better tick print moves farther from the unreached stop, so it
    // cannot activate that pending order either; zero trades stays required.
    const double better_open = (100/kTick + (is_long ? -2 : 2))*kTick;
    input[1].open = input[1].high = input[1].low = input[1].close = better_open;
    Probe improved(Entry::BelowLimit, is_long);
    run(improved, input, mode);
    require(improved.trade_count() == expected_trades, "better open trade count differs");
    if (!ticks) {
        check_trade(improved, 0, 1, better_open, 10);
        require(improved.get_trade(0).exit_price == exit_price,
                "better entry must retain the scheduled exit price");
    }
    require(improved.remaining() == 0, "better open must leave a flat final book");
    require(improved.pending() == (ticks ? 1 : 0), "better open pending count differs");

    if (ticks) {
        // Input mutant: print100 at open1 reaches the pending stop. The later
        // better print99.98/100.02 first reaches its live limit99.99/100.01.
        // This must change zero trades to one, with entry1 at the better
        // print and the close10 command executing at open11.
        input[1].open = 100;
        input[1].high = std::max(100.0, better_open);
        input[1].low = std::min(100.0, better_open);
        Probe reached(Entry::BelowLimit, is_long);
        run(reached, input, mode);
        require(reached.trade_count() == 1, "later stop print must activate and fill once");
        check_trade(reached, 0, 1, better_open, 10);
        require(reached.get_trade(0).exit_price == exit_price,
                "later activation must retain the scheduled exit quote");
        require(reached.remaining() == 0, "later activation must leave a flat final book");
        require(reached.pending() == 0, "filled later activation must leave no pending entry");
    }
}

void check_activated_limit_without_touch(Input mode) {
    // Distance d=1 for ticks, 0.01 for modeled bars: buy limit L=100-d.
    // Close0=stop100 reaches the stop but exceeds L=99 (ticks), 99.99 (modeled).
    // Ticks first activate at eligible open1=100; modeled activates at close0.
    // Every later low is 100>L: activation permits resting, not a fill.
    // Only bar0 places an order within bars0..11; with no fill or cancellation
    // it remains the single pending order, with zero trades and zero position.
    // For binary64 t=0.01, 99/t rounds to integer9900, and 9900*t rounds to99;
    // the whole-price limit and projected tick price therefore agree exactly.
    const double limit_distance = mode == Input::Ticks ? 1 : kTick;
    const double limit = 100-limit_distance;
    auto input = bars();
    input.resize(12);
    Probe probe(Entry::BelowLimit, true, 0, false, limit_distance);
    run(probe, input, mode);
    require(probe.trade_count() == 0, "activation without a limit touch must not fill");
    require(probe.remaining() == 0, "untouched activated limit must leave the book flat");
    require(probe.pending() == 1, "untouched activated limit must remain pending");

    // Input mutant: low2=L first reaches the live buy limit on 2, at L.
    // Close10=100 exits modeled; ticks exit at open11=100 (reported bar12).
    input[2].low = limit;
    Probe touched(Entry::BelowLimit, true, 0, false, limit_distance);
    run(touched, input, mode);
    require(touched.trade_count() == 1, "later limit touch must fill the resting order");
    check_trade(touched, 0, 2, limit, 10);
    require(touched.get_trade(0).exit_price == input[10].close,
            "later limit touch must exit at the scheduled close price");
    require(touched.remaining() == 0, "touched limit must leave a flat final book");
    require(touched.pending() == 0, "touched limit must leave no pending entry");
}

void check_activated_limit_cancelled_before_touch(Input mode) {
    const double limit_distance = mode == Input::Ticks ? 1 : kTick;
    const double limit = 100-limit_distance;
    auto input = activated_limit_bars(true, limit_distance);
    // Close0=stop100>L; ticks activate at open1=100, modeled at close0.
    // Bar1 stays flat100, so low1>L. The script cancels at close1,
    // before bar2's first limit touch. Bars2..11 remain L, so a cancelled
    // live limit produces zero trades, zero position and zero pending orders.
    input[1].open = input[1].high = input[1].low = input[1].close = 100;
    Probe cancelled(Entry::BelowLimit, true, 0, true, limit_distance);
    run(cancelled, input, mode);
    require(cancelled.trade_count() == 0, "cancelled activated limit must not fill later");
    require(cancelled.remaining() == 0, "cancelled activated limit must leave the book flat");
    require(cancelled.pending() == 0, "cancelled activated limit must leave no pending order");

    // Script mutant: without cancellation the same first touch is open2,
    // hence entry2@L, exit10@L modeled; ticks report entry3@99, exit12@99
    // from open11=L. Cancellation is discriminating for both input modes.
    Probe retained(Entry::BelowLimit, true, 0, false, limit_distance);
    run(retained, input, mode);
    require(retained.trade_count() == 1, "uncancelled activated limit must fill on the later touch");
    check_trade(retained, 0, 2, limit, 10);
    // Market exit rounds L to floor(L/0.01+0.5)*0.01; open11=close10=L.
    const double exit_price = std::floor(input[10].close/kTick + 0.5)*kTick;
    require(retained.get_trade(0).exit_price == exit_price,
            "uncancelled limit must exit at the scheduled close price");
    require(retained.remaining() == 0, "uncancelled limit must leave a flat final book");
    require(retained.pending() == 0, "filled uncancelled limit must leave no pending entry");
}

void check_stop_reached_limit_waiting(Input mode) {
    // Close0=100 reaches stop=100-0.5=99.5, but misses limit=100-1=99.
    // The reached stop activates at that close, but low1=99.25
    // misses limit99. Low2=99 first fills at 99; the next exit is close10.
    // Lowering low1 to 99 would move the entry to 1, falsifying the bar bound.
    auto input = bars();
    input.resize(12);
    input[1].low = 100-0.75;
    input[2].low = 100-1;
    Probe probe(Entry::StopReachedLimitWaiting);
    run(probe, input, mode);
    require(probe.trade_count() == 1, "unmarketable close must wait for the limit");
    check_trade(probe, 0, 2, 99, 10);
    require(probe.remaining() == 0, "expected a flat final book");

    input[1].low = 100-1;
    Probe mutant(Entry::StopReachedLimitWaiting);
    run(mutant, input, mode);
    require(mutant.trade_count() == 1, "earlier limit touch must fill once");
    check_trade(mutant, 0, 1, 99, 10);
}
} // namespace

int main() {
    int failures = 0;
    for (const auto mode : {Input::ModeledBars, Input::Ticks}) {
        const auto check = [&](auto body) {
            try {
                body();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "%s: %s\n",
                    mode == Input::ModeledBars ? "modeled bars" : "ticks", error.what());
                ++failures;
            }
        };
        check([&] { check_close_entries(mode, Entry::StopLimit); });
        check([&] { check_close_entries(mode, Entry::Limit); });
        check([&] { check_close_entries(mode, Entry::Stop); });
        check([&] { check_close_entries(mode, Entry::StopLimit, false); });
        check([&] { check_close_entries(mode, Entry::BothReached); });
        check([&] { check_close_entries(mode, Entry::BothReached, false); });
        // Two ticks of slippage mutate any accidental stop-slippage predicate:
        // a slipped stop-limit entry would be 100+/-0.02 and fail the price check.
        check([&] { check_close_entries(mode, Entry::StopLimit, true, 2); });
        check([&] { check_close_entries(mode, Entry::StopLimit, false, 2); });
        check([&] { check_close_entries(mode, Entry::Limit, true, 2); });
        check([&] { check_close_entries(mode, Entry::Stop, true, 2); });
        check([&] { check_close_entries(mode, Entry::Limit, false, 2); });
        check([&] { check_close_entries(mode, Entry::Stop, false, 2); });
        check([&] { check_unmarketable_limit(mode); });
        check([&] { check_unmarketable_limit(mode, false); });
        check([&] { check_unreached_stop(mode); });
        check([&] { check_gap(mode); });
        check([&] { check_activated_stop_limit_rests_as_limit(mode, true); });
        check([&] { check_activated_stop_limit_rests_as_limit(mode, false); });
        check([&] { check_activated_limit_without_touch(mode); });
        check([&] { check_activated_limit_cancelled_before_touch(mode); });
        check([&] { check_stop_reached_limit_waiting(mode); });
    }
    return failures == 0 ? 0 : 1;
}
