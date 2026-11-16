// Native ownership control: a genuine PerEntryExit child must retain its
// own quantity cap even when FIFO closes lots of a different origin.
// The entry-origin/FIFO rule is the existing reported #694 case in
// test_issue694_reported_exits.cpp. Unequal units isolate the cap, rather
// than asserting additional TradingView rounding or modification rules.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double absent = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = 1'735'689'600'000LL;
constexpr std::int64_t interval = 900'000;
constexpr double first_units = 2.0, second_units = 6.0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

class ChildHost final : public source::PineStrategyHost {
public:
    ChildHost(bool long_side, bool named, bool split)
        : long_(long_side), named_(named), split_(split) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1'000'000.0;
        config.pyramiding = 2;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        qty_step_ = 0.25;
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0 || index == 1)
            strategy_entry("selected", long_, absent, absent,
                           index == 0 ? first_units : second_units);
        if (!split_ || index < 2)
            strategy_exit("relative", named_ ? "selected" : "", absent, absent,
                          absent, absent, absent, 100.0, "", absent, "", absent, 10.0);
    }
private:
    bool long_, named_, split_;
};

void check(bool long_side, bool named, bool split = false) {
    ChildHost host(long_side, named, split);
    const auto mirror = [=](double p) { return long_side ? p : 200.0 - p; };
    const auto bar = [&](int i, double o, double h, double l, double c) {
        return Bar{mirror(o), mirror(long_side ? h : l),
                   mirror(long_side ? l : h), mirror(c), 1000.0, start + i * interval};
    };
    const std::vector<Bar> input{
        bar(0, 100, 101, 99, 100), bar(1, 100, 101, 99, 100),
        bar(2, 104, 105, 103, 104),
        split ? bar(3, 104, 105, 93, 96) : bar(3, 100, 105, 89, 100),
        split ? bar(4, 96, 97, 95, 96) : bar(4, 100, 101, 89, 100),
        bar(5, 96, 97, 89, 91),
    };
    host.run(input.data(), static_cast<int>(input.size()), "15", "15", false);
    require(host.last_error().empty(), "native child control returned an engine error");
    std::vector<double> closed;
    double opened = 0.0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!fill) continue;
        opened += std::abs(fill->opened_units);
        if (fill->request().label == "relative" && fill->closed_units > 0.0) {
            closed.push_back(fill->closed_units);
            require(fill->interval_index() == (split && closed.size() == 2 ? 5 : 3),
                    "both children must fill on their first crossing bar");
        }
    }
    require(near(opened, first_units + second_units), "both unequal openings must execute");
    require(closed.size() == 2, "collect both actual native child fills");
    require(near(closed[0], second_units) && near(closed[1], first_units),
            "the first triggered child must close its own units, then the residual child");
    require(host.trade_count() == 3,
            "FIFO must split the larger child across the two unequal source lots");
    double settled = 0.0;
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        require(!trade.open_at_end && trade.exit_id == "relative",
                "native children must settle the entire book without an open remainder");
        settled += trade.qty;
    }
    require(near(settled, opened), "child caps must conserve all opened units");
}

// F05 TV facts: test_exit_queue_tapes.cpp:585 reads creation-order
// reservations from w3f05-s7/s8, and :605 reads cancel/redeclare from s15.
// Two native units, with one already reserved, leave exactly one for X.
class QueueHost final : public source::PineStrategyHost {
public:
    QueueHost(bool long_side, bool cancel) : long_(long_side), cancel_(cancel) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1'000'000.0;
        config.pyramiding = 1;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        qty_step_ = 0.25;
        fixture_retain_all_events();
    }
    double price(double p) const { return long_ ? p : 200.0 - p; }
    void tick_exit(const char* id, double loss) {
        strategy_exit(id, "entry", absent, absent, absent, absent, absent,
                      100.0, "", absent, "", absent, loss);
    }
    source::PlacementSnapshot target() const {
        source::PlacementSnapshot result{};
        int count = 0;
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            if (row.snapshot.source_id == "X") {
                result = row.snapshot;
                ++count;
            }
        }
        require(count == 1, "setup: collect the singleton relative stop");
        return result;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("entry", long_, absent, absent, 2.0);
        if (pine_bar_index() == 1) {
            strategy_exit("holder", "entry", absent, price(70.0),
                          absent, absent, absent, 100.0, "", 1.0);
        }
        if (pine_bar_index() == 2) {
            // The ordinary holder's staged leg has become working before
            // X is created. Do not grant a new staged leg a queue rank.
            tick_exit("X", 10.0);
        }
        if (pine_bar_index() == 3) {
            before = target();
            if (cancel_) {
                strategy_cancel("X");
                tick_exit("later", 8.0);
            }
            tick_exit("X", 8.0);
            if (!cancel_) after = target();
            changed = true;
        }
    }
    source::PlacementSnapshot before{}, after{};
    bool changed = false;
private:
    bool long_, cancel_;
};

void check_queue(bool long_side, bool cancel) {
    QueueHost host(long_side, cancel);
    const auto bar = [&](int i, double o, double h, double l, double c) {
        return Bar{host.price(o), host.price(long_side ? h : l),
                   host.price(long_side ? l : h), host.price(c), 1000.0, start + i * interval};
    };
    const std::vector<Bar> input{
        bar(0, 100, 101, 99, 100), bar(1, 100, 101, 99, 100),
        bar(2, 100, 101, 99, 100), bar(3, 100, 101, 99, 100),
        bar(4, 100, 101, 91, 94), bar(5, 94, 95, 93, 94),
    };
    host.run(input.data(), static_cast<int>(input.size()), "15", "15", false);
    require(host.last_error().empty() && host.changed, "setup: queue modification must execute");
    if (!cancel) {
        require(host.before.chain_origin_sequence != 0
                && host.before.chain_origin_sequence == host.after.chain_origin_sequence,
                "changed ticks must retain the named exit's reservation rank");
        require(near(host.after.projection_remaining_qty, 1.0)
                && near(host.after.per_entry_units, 1.0),
                "named relative child must reserve only the unit left by the earlier exit");
    }
    int fills = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!fill || !(fill->closed_units > 0.0)) continue;
        ++fills;
        require(fill->request().label == (cancel ? "later" : "X")
                && near(fill->closed_units, 1.0) && fill->interval_index() == 4,
                "native fill must honor reservation order and cancel/redeclare chronology");
    }
    require(fills == 1, "collect exactly one native stop fill, leaving the holder's unit");
}

// Native contracts only. Open TV questions: does a
// signal-time singleton round fractional relative operands away from its
// fill, and does a named singleton beside another id price from its own fill?
// Pin declared native levels and directional fills without calling them TV facts.
class PricingHost final : public source::PineStrategyHost {
public:
    PricingHost(bool long_side, bool selected, bool signal)
        : long_(long_side), selected_(selected), signal_(signal) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1'000'000.0;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        qty_step_ = 0.25;
        fixture_retain_all_events();
    }
    double price(double p) const { return long_ ? p : 200.0 - p; }
    void stop() {
        strategy_exit("priced", "B", absent, absent, absent, absent, absent,
                      100.0, "", absent, "", absent, 5.5);
    }
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) {
            strategy_entry(selected_ ? "A" : "B", long_, absent, absent, 2.0);
            if (signal_) stop();
        }
        if (index == 1 && selected_)
            strategy_entry("B", long_, absent, absent, 6.0);
        if (index == 2) {
            if (!signal_) stop();
            int count = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.snapshot.source_id != "priced") continue;
                level = row.snapshot.exit_levels.stop;
                ++count;
            }
            require(count == 1, "setup: collect exactly one selected native stop");
            average = position_avg_price();
            observed = true;
        }
    }
    double level = absent, average = absent;
    bool observed = false;
private:
    bool long_, selected_, signal_;
};

void check_pricing(bool long_side, bool selected, bool signal) {
    PricingHost host(long_side, selected, signal);
    const auto bar = [&](int i, double o, double h, double l, double c) {
        return Bar{host.price(o), host.price(long_side ? h : l),
                   host.price(long_side ? l : h), host.price(c), 1000.0, start + i * interval};
    };
    const std::vector<Bar> input{
        bar(0, 100, 101, 99, 100), bar(1, 100, 101, 99, 100),
        selected ? bar(2, 104, 105, 103, 104) : bar(2, 100, 101, 99, 100),
        selected ? bar(3, 104, 105, 97, 100) : bar(3, 100, 101, 93, 96),
        bar(4, 100, 101, 99, 100),
    };
    host.run(input.data(), static_cast<int>(input.size()), "15", "15", false);
    require(host.last_error().empty() && host.observed, "setup: native pricing must execute");
    const double selected_fill = selected ? 104.0 : 100.0;
    const double raw_stop = host.price(selected_fill - 5.5);
    require(near(host.level, raw_stop),
            "native relative level must retain fractional ticks from its selected fill");
    if (selected) {
        const double average_stop = host.average + (long_side ? -5.5 : 5.5);
        require(!near(raw_stop, average_stop),
                "selected-entry control must distinguish the aggregate average");
    }
    int fills = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!fill || fill->request().label != "priced" || !(fill->closed_units > 0.0)) continue;
        ++fills;
        const double directional_stop = long_side ? std::floor(raw_stop) : std::ceil(raw_stop);
        require(near(fill->resolved_price, directional_stop)
                && near(fill->closed_units, selected ? 6.0 : 2.0)
                && fill->interval_index() == 3,
                "actual native fill must use the selected fractional stop and child cap");
    }
    require(fills == 1, "collect the actual selected native stop fill");
}
} // namespace

int main() {
    int failures = 0;
    for (bool named : {false, true}) {
        for (bool long_side : {true, false}) {
            try {
                check(long_side, named);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "named=%d long=%d: %s\n",
                             named, long_side, error.what());
            }
        }
    }
    for (bool cancel : {false, true}) {
        for (bool long_side : {true, false}) {
            try {
                check_queue(long_side, cancel);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "queue cancel=%d long=%d: %s\n",
                             cancel, long_side, error.what());
            }
        }
    }
    // With no later script call, a filled child's receipt must not withdraw
    // an independent child of another origin under the same named exit.
    for (bool long_side : {true, false}) {
        try {
            check(long_side, true, true);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "split named long=%d: %s\n", long_side, error.what());
        }
    }
    for (bool long_side : {true, false}) {
        for (bool signal : {false, true}) {
            try {
                check_pricing(long_side, false, signal);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "fractional long=%d signal=%d: %s\n",
                             long_side, signal, error.what());
            }
        }
        try {
            check_pricing(long_side, true, false);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "selected pricing long=%d: %s\n", long_side, error.what());
        }
    }
    return failures == 0 ? 0 : 1;
}
