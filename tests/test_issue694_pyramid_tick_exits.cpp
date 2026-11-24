/*
 * Named tick exits over pyramided entries.
 *
 * With pyramiding, one entry id can open several trades:
 *
 *     strategy.entry("L", strategy.long)                       // twice
 *     strategy.exit("X", "L", profit = 50000, loss = 50000)    // every bar
 *
 * A profit or loss given in ticks is a distance from an entry's own fill
 * price, so each fill of "L" has its own bracket: 500.00 away from its own
 * price, for its own quantity. A bracket that fills closes the oldest open
 * trade (first in, first out), and fills once. An entry whose order the
 * script places after the exit's last call has no bracket until the exit is
 * called again.
 *
 * Every row drives the real source host over real bars and reads the trades
 * the engine closed. The expected exits are computed here from the row's own
 * inputs: each fill bar's open plus or minus the tick distance. Rows marked
 * "red-on-base" fail on a tree that prices every lot from the position's
 * average; rows marked "control" hold on both trees.
 *
 * Output: for each row a RUN line, one OBSERVED and one EXPECTED line per
 * trade, and a PASS or FAIL line; then one SUMMARY line. Exit status 1 when a
 * row failed.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr std::int64_t kFirstBarMs = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kBarMs = 60000LL;

int rows_run = 0;
int rows_failed = 0;

Bar bar_at(int index, double open, double high, double low, double close) {
    Bar bar{};
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.volume = 1000.0;
    bar.timestamp = kFirstBarMs + static_cast<std::int64_t>(index) * kBarMs;
    return bar;
}

// The strategy.exit("X", "L", ...) call of one bar.
struct ExitPlan {
    bool call = false;
    double profit_ticks = kNaN;
    double loss_ticks = kNaN;
    double stop = kNaN;  // a priced stop, for the control that uses no ticks
    double qty = kNaN;
    double percent = 100.0;
};

ExitPlan ticks(double profit_ticks, double loss_ticks) {
    ExitPlan plan;
    plan.call = true;
    plan.profit_ticks = profit_ticks;
    plan.loss_ticks = loss_ticks;
    return plan;
}

ExitPlan priced_stop(double stop) {
    ExitPlan plan;
    plan.call = true;
    plan.stop = stop;
    return plan;
}

class PyramidHost;

struct Script {
    bool is_long = true;
    bool close_any = false;
    // Whether a bar calls strategy.exit before strategy.entry.
    bool exit_before_entry = false;
    // strategy.entry("L", ...) calls: (bar, quantity). Each fills at the next
    // bar's open.
    std::vector<std::pair<int, double>> entries;
    std::function<ExitPlan(int)> exit_at;
    std::function<void(PyramidHost&, int)> after_orders;
};

class PyramidHost final : public source::PineStrategyHost {
public:
    explicit PyramidHost(Script script) : script_(std::move(script)) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.close_entries_rule_any = script_.close_any;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(kTick);
    }

    void on_source_bar(const Bar&) override {
        const int bar = calls_++;
        const ExitPlan plan = script_.exit_at ? script_.exit_at(bar) : ExitPlan{};
        if (script_.exit_before_entry) place("L", plan);
        for (const auto& entry : script_.entries) {
            if (entry.first != bar) continue;
            strategy_entry("L", script_.is_long, kNaN, kNaN, entry.second);
        }
        if (!script_.exit_before_entry) place("L", plan);
        if (script_.after_orders) script_.after_orders(*this, bar);
    }

    using PineStrategyHost::strategy_close;
    using PineStrategyHost::strategy_entry;
    using PineStrategyHost::strategy_exit_cancel_bracket;

    void place(const char* id, const ExitPlan& plan) {
        if (!plan.call) return;
        strategy_exit("X", id, kNaN, plan.stop, kNaN, kNaN, kNaN, plan.percent, "",
                      plan.qty, "", plan.profit_ticks, plan.loss_ticks);
    }

private:
    Script script_;
    int calls_ = 0;
};

struct Row {
    double entry = kNaN;
    double exit = kNaN;
    double quantity = kNaN;
    bool is_long = true;
    int entry_bar = -1;
    int exit_bar = -1;
};

Row expect(double entry, double exit, double quantity, bool is_long, int entry_bar, int exit_bar) {
    Row row;
    row.entry = entry;
    row.exit = exit;
    row.quantity = quantity;
    row.is_long = is_long;
    row.entry_bar = entry_bar;
    row.exit_bar = exit_bar;
    return row;
}

bool near(double left, double right) {
    return std::isfinite(left) && std::isfinite(right) && std::fabs(left - right) <= 1e-6;
}

const char* side_name(bool is_long) { return is_long ? "long" : "short"; }

// The closed trades of the run in the order the engine closed them, against
// `want` in the same order.
void check_trades(const char* row, const char* kind, const Script& script,
                  const std::vector<Bar>& bars, const std::vector<Row>& want) {
    PyramidHost host(script);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const std::string error = host.last_error();
    std::vector<Row> got;
    int open_rows = 0;
    for (int index = 0; index < host.report_trade_count(); ++index) {
        const Trade trade = host.get_report_trade(index);
        if (trade.open_at_end) {
            ++open_rows;
            continue;
        }
        Row seen;
        seen.entry = trade.entry_price;
        seen.exit = trade.exit_price;
        seen.quantity = trade.qty;
        seen.is_long = trade.is_long;
        seen.entry_bar = trade.entry_bar_index;
        seen.exit_bar = trade.exit_bar_index;
        got.push_back(seen);
    }
    bool pass = error.empty() && open_rows == 0 && got.size() == want.size();
    for (std::size_t index = 0; pass && index < want.size(); ++index) {
        pass = got[index].is_long == want[index].is_long
            && near(got[index].entry, want[index].entry)
            && near(got[index].exit, want[index].exit)
            && near(got[index].quantity, want[index].quantity)
            && got[index].entry_bar == want[index].entry_bar
            && got[index].exit_bar == want[index].exit_bar;
    }
    ++rows_run;
    if (!pass) ++rows_failed;
    std::printf("ROW %s RUN kind=%s closed=%zu expected_closed=%zu open_rows=%d "
                "error=\"%s\"\n",
                row, kind, got.size(), want.size(), open_rows, error.c_str());
    for (std::size_t index = 0; index < got.size(); ++index) {
        std::printf("ROW %s OBSERVED trade=%zu side=%s entry=%.8f exit=%.8f qty=%.8f "
                    "entry_bar=%d exit_bar=%d\n",
                    row, index, side_name(got[index].is_long), got[index].entry,
                    got[index].exit, got[index].quantity, got[index].entry_bar,
                    got[index].exit_bar);
    }
    for (std::size_t index = 0; index < want.size(); ++index) {
        std::printf("ROW %s EXPECTED trade=%zu side=%s entry=%.8f exit=%.8f qty=%.8f "
                    "entry_bar=%d exit_bar=%d\n",
                    row, index, side_name(want[index].is_long), want[index].entry,
                    want[index].exit, want[index].quantity, want[index].entry_bar,
                    want[index].exit_bar);
    }
    std::printf("ROW %s %s\n", row, pass ? "PASS" : "FAIL");
}

// The reported shape: two market entries of one unit, a 500.00 bracket in
// ticks called on every bar after the entry call. The fills are bar 1's and
// bar 3's opens; bar 5 falls through both stops.
std::vector<Bar> reported_bars() {
    return {
        bar_at(0, 43950.00, 43960.00, 43940.00, 43950.00),
        bar_at(1, 43946.26, 43960.00, 43940.00, 43950.00),
        bar_at(2, 43950.00, 43960.00, 43925.00, 43930.00),
        bar_at(3, 43931.10, 43945.00, 43920.00, 43940.00),
        bar_at(4, 43940.00, 43950.00, 43900.00, 43910.00),
        bar_at(5, 43910.00, 43915.00, 43400.00, 43420.00),
        bar_at(6, 43420.00, 43425.00, 43415.00, 43420.00),
    };
}

// The same two prices in the other order: the older entry is the lower one,
// so the newer entry's stop is reached first (bar 5) and the older entry's a
// bar later (bar 6).
std::vector<Bar> reversed_bars() {
    return {
        bar_at(0, 43935.00, 43945.00, 43925.00, 43935.00),
        bar_at(1, 43931.10, 43945.00, 43925.00, 43935.00),
        bar_at(2, 43935.00, 43950.00, 43930.00, 43945.00),
        bar_at(3, 43946.26, 43955.00, 43940.00, 43950.00),
        bar_at(4, 43950.00, 43955.00, 43900.00, 43905.00),
        bar_at(5, 43905.00, 43910.00, 43440.00, 43450.00),
        bar_at(6, 43450.00, 43455.00, 43420.00, 43425.00),
        bar_at(7, 43425.00, 43430.00, 43420.00, 43425.00),
    };
}

// Fills at 100 (bar 1) and 104 (bar 3); bar 4 reaches 94 and not 90; bar 5
// reaches 88.
std::vector<Bar> unequal_bars() {
    return {
        bar_at(0, 100.0, 100.5, 99.5, 100.0),
        bar_at(1, 100.0, 100.5, 99.5, 100.0),
        bar_at(2, 100.0, 104.5, 99.5, 104.0),
        bar_at(3, 104.0, 105.0, 103.0, 104.0),
        bar_at(4, 104.0, 104.5, 93.5, 95.0),
        bar_at(5, 95.0, 95.5, 87.0, 88.0),
        bar_at(6, 88.0, 88.5, 87.5, 88.0),
    };
}

// Short entries at 100 (bar 1) and 104 (bar 3); bar 5 rises through 110 and
// 114.
std::vector<Bar> short_bars() {
    return {
        bar_at(0, 100.0, 100.5, 99.5, 100.0),
        bar_at(1, 100.0, 100.5, 99.5, 100.0),
        bar_at(2, 100.0, 104.5, 99.5, 104.0),
        bar_at(3, 104.0, 104.5, 103.5, 104.0),
        bar_at(4, 104.0, 104.5, 103.5, 104.0),
        bar_at(5, 104.0, 115.0, 103.5, 114.5),
        bar_at(6, 114.5, 115.0, 114.0, 114.5),
    };
}

// Fills at 100 (bar 1) and 104 (bar 3). Bar 3 itself falls to 93, below the
// second entry's own 94. Bar 4 opens at 91.5, below 94, and falls to 89.
std::vector<Bar> cutoff_bars() {
    return {
        bar_at(0, 100.0, 100.5, 99.5, 100.0),
        bar_at(1, 100.0, 100.5, 99.5, 100.0),
        bar_at(2, 100.0, 104.5, 99.5, 104.0),
        bar_at(3, 104.0, 104.5, 93.0, 95.0),
        bar_at(4, 91.5, 92.0, 89.0, 89.5),
        bar_at(5, 89.5, 90.0, 89.0, 89.5),
    };
}

// Fills at 100 (bar 1) and 104 (bar 3); bar 5 falls through 90.
std::vector<Bar> priced_stop_bars() {
    return {
        bar_at(0, 100.0, 100.5, 99.5, 100.0),
        bar_at(1, 100.0, 100.5, 99.5, 100.0),
        bar_at(2, 100.0, 104.5, 99.5, 104.0),
        bar_at(3, 104.0, 104.5, 103.5, 104.0),
        bar_at(4, 104.0, 104.5, 103.5, 104.0),
        bar_at(5, 104.0, 104.5, 89.0, 89.5),
        bar_at(6, 89.5, 90.0, 89.0, 89.5),
    };
}

// BINANCE:BTCUSDT, hourly bars from 2024-01-01 through 2024-03-01 inclusive.
std::vector<Bar> btc_hours() {
    const std::string source = __FILE__;
    const auto path = source.substr(0, source.find_last_of("/\\") + 1)
        + "fixtures/qty_step_mincontract/btc60/feed/bars.csv";
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != "timestamp,open,high,low,close,volume") {
        throw std::runtime_error("BTCUSDT hourly feed is missing or has an unexpected header");
    }
    std::vector<Bar> bars;
    while (std::getline(input, line)) {
        std::istringstream in(line);
        Bar b{};
        const auto comma = [&] { return in.get() == ','; };
        if (!(in >> b.timestamp) || !comma() || !(in >> b.open) || !comma()
            || !(in >> b.high) || !comma() || !(in >> b.low) || !comma()
            || !(in >> b.close) || !comma() || !(in >> b.volume) || !in.eof()
            || b.timestamp != kFirstBarMs + static_cast<std::int64_t>(bars.size()) * 3600000
            || !(b.low > 0 && b.low <= b.open && b.open <= b.high
                 && b.low <= b.close && b.close <= b.high && b.volume >= 0)) {
            throw std::runtime_error("invalid BTCUSDT hourly bar " + std::to_string(bars.size()));
        }
        bars.push_back(b);
    }
    if (!input.eof() || bars.size() != 1441 || !near(bars[118].open, 43946.26)
        || !near(bars[124].open, 43931.10)) {
        throw std::runtime_error("expected the 1,441 hourly bars of the reported range");
    }
    return bars;
}

class CrossoverHost final : public source::PineStrategyHost {
public:
    explicit CrossoverHost(int pyramiding) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 10000;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10;
        config.pyramiding = pyramiding;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_type("crypto");
        set_syminfo_mintick(kTick);
        set_syminfo_metadata("mincontract", 0.00001);
        set_syminfo_metadata("series_origin_ms", kFirstBarMs);
    }

    void on_source_bar(const Bar& bar) override {
        first_possible_entry.push_back(next_entry_);
        const double fast = fast_.compute(bar.close);
        const double slow = slow_.compute(bar.close);
        const bool cross = trend_.compute(fast, slow);
        const bool add = rally_.compute(bar.close, fast);
        const double held = signed_position_size();
        if (held == 0) next_entry_ = pine_bar_index() + 1;
        if (cross || (held > 0 && add)) strategy_entry("Long", true);
        strategy_exit("Long exit", "Long", kNaN, kNaN, kNaN, kNaN, kNaN, 100, "", kNaN,
                      "", 50000, 50000);
    }

    // A bracket cannot outlive a flat book. At a bar's intrabar fill the
    // last observed calculation was at the preceding bar's close.
    std::vector<int> first_possible_entry;

private:
    int next_entry_ = 0;
    ta::EMA fast_{9};
    ta::EMA slow_{21};
    ta::Crossover trend_;
    ta::Crossover rally_;
};

void check_crossover(const std::vector<Bar>& bars, int pyramiding) {
    CrossoverHost host(pyramiding);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    std::vector<int> openings;
    for (int i = 0; i < host.report_trade_count(); ++i) {
        openings.push_back(host.get_report_trade(i).entry_bar_index);
    }
    int other_level = 0;
    int invalid = 0;
    int first_stop = 0;
    int second_stop = 0;
    double first_exit = kNaN;
    double second_exit = kNaN;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade t = host.get_trade(i);
        const int entry = t.entry_bar_index;
        const int exit = t.exit_bar_index;
        if (entry < 0 || entry > exit || exit >= static_cast<int>(bars.size())
            || exit >= static_cast<int>(host.first_possible_entry.size()) || !t.is_long
            || t.entry_id != "Long" || t.exit_id != "Long exit" || !(t.qty > 0)
            || !near(t.entry_price, bars[entry].open)) {
            ++invalid;
            continue;
        }
        const auto at_level = [&](int origin) {
            return near(std::fabs(t.exit_price - bars[origin].open), 500.0);
        };
        if (!at_level(entry)) {
            ++other_level;
            bool found = false;
            for (const int origin : openings) {
                if (origin >= host.first_possible_entry[exit] && origin <= exit
                    && at_level(origin)) {
                    found = true;
                    break;
                }
            }
            if (!found) ++invalid;
        }
        if (entry == 118) {
            first_exit = t.exit_price;
            if (exit == 126 && near(t.exit_price, 43446.26)) ++first_stop;
        }
        if (entry == 124) {
            second_exit = t.exit_price;
            if (exit == 126 && near(t.exit_price, 43431.10)) ++second_stop;
        }
    }
    const bool pyramid = pyramiding == 2;
    const int want_closed = pyramid ? 63 : 30;
    const int want_other = pyramid ? 20 : 0;
    const bool pass = host.last_error().empty() && host.first_possible_entry.size() == bars.size()
        && host.trade_count() == want_closed && invalid == 0 && other_level == want_other
        && (!pyramid || (first_stop == 1 && second_stop == 1));
    const char* row = pyramid ? "hourly-crossover-pyramid" : "hourly-crossover-single";
    std::printf("ROW %s RUN kind=%s\n", row, pyramid ? "red-on-base" : "control");
    std::printf("ROW %s OBSERVED closed=%d other_level=%d invalid=%d exit_118=%.2f "
                "exit_124=%.2f error=\"%s\"\n", row, host.trade_count(), other_level, invalid,
                first_exit, second_exit, host.last_error().c_str());
    std::printf("ROW %s EXPECTED closed=%d other_level=%d invalid=0%s\n", row, want_closed,
                want_other, pyramid ? " bar_126=43446.26,43431.10" : "");
    std::printf("ROW %s %s\n", row, pass ? "PASS" : "FAIL");
    ++rows_run;
    if (!pass) ++rows_failed;
}

Script script_of(bool is_long, bool exit_before_entry,
                 std::vector<std::pair<int, double>> entries,
                 std::function<ExitPlan(int)> exit_at) {
    Script script;
    script.is_long = is_long;
    script.exit_before_entry = exit_before_entry;
    script.entries = std::move(entries);
    script.exit_at = std::move(exit_at);
    return script;
}

}  // namespace

int main() {
    const double distance = 50000.0 * kTick;  // 500.00
    const auto hourly = btc_hours();
    check_crossover(hourly, 2);
    check_crossover(hourly, 1);

    // ---- The reported shape: each fill closes at its own stop --------------
    {
        const std::vector<Bar> bars = reported_bars();
        const double first = bars[1].open;
        const double second = bars[3].open;
        check_trades("long.each-entry-own-stop", "red-on-base",
                     script_of(true, false, {{0, 1.0}, {2, 1.0}},
                               [](int) { return ticks(50000.0, 50000.0); }),
                     bars,
                     {expect(first, first - distance, 1.0, true, 1, 5),
                      expect(second, second - distance, 1.0, true, 3, 5)});
        // One entry only: its bracket is 500.00 from its own fill on both trees.
        check_trades("control.long.single-entry", "control",
                     script_of(true, false, {{0, 1.0}},
                               [](int) { return ticks(50000.0, 50000.0); }),
                     bars, {expect(first, first - distance, 1.0, true, 1, 5)});
    }

    // ---- First in, first out: the newer entry's stop is reached first and
    // closes the older trade; the older entry's stop then closes the newer.
    {
        const std::vector<Bar> bars = reversed_bars();
        const double older = bars[1].open;
        const double newer = bars[3].open;
        check_trades("long.fifo-closes-oldest-at-newer-stop", "red-on-base",
                     script_of(true, false, {{0, 1.0}, {2, 1.0}},
                               [](int) { return ticks(50000.0, 50000.0); }),
                     bars,
                     {expect(older, newer - distance, 1.0, true, 1, 5),
                      expect(newer, older - distance, 1.0, true, 3, 6)});
        auto script = script_of(true, false, {{0, 1.0}, {2, 1.0}},
                                [](int) { return ticks(kNaN, 50000.0); });
        script.close_any = true;
        check_trades("long.any-closes-triggering-entry", "red-on-base", script, bars,
                     {expect(newer, newer - distance, 1.0, true, 3, 5),
                      expect(older, older - distance, 1.0, true, 1, 6)});
        script.entries = {{0, 1.0}};
        script.after_orders = [](PyramidHost& host, int bar) {
            if (bar == 2) host.strategy_entry("B", true, kNaN, kNaN, 1.0);
            if (bar >= 2) host.place("B", ticks(kNaN, 50000));
        };
        check_trades("long.same-exit-id-different-entries", "red-on-base", script, bars,
                     {expect(newer, newer - distance, 1.0, true, 3, 5),
                      expect(older, older - distance, 1.0, true, 1, 6)});
    }

    // ---- Unequal quantities, and a changed distance ------------------------
    // Two units at 100 and one at 104, loss = 1000 ticks: stops 90 (two
    // units) and 94 (one unit). Bar 4 reaches 94: one unit of the older trade
    // closes. From bar 4's close the script asks for 1200 ticks: the older
    // entry's stop moves to 88 and still holds two units, the two that are
    // open, across both trades.
    {
        const std::vector<Bar> bars = unequal_bars();
        const double older = bars[1].open;
        const double newer = bars[3].open;
        check_trades("long.unequal-sizes-and-repriced-distance", "red-on-base",
                     script_of(true, false, {{0, 2.0}, {2, 1.0}},
                               [](int bar) { return ticks(kNaN, bar >= 4 ? 1200.0 : 1000.0); }),
                     bars,
                     {expect(older, newer - 1000.0 * kTick, 1.0, true, 1, 4),
                      expect(older, older - 1200.0 * kTick, 1.0, true, 1, 5),
                      expect(newer, older - 1200.0 * kTick, 1.0, true, 3, 5)});
    }

    // ---- The short side ------------------------------------------------------
    {
        const std::vector<Bar> bars = short_bars();
        const double first = bars[1].open;
        const double second = bars[3].open;
        check_trades("short.each-entry-own-stop", "red-on-base",
                     script_of(false, false, {{0, 1.0}, {2, 1.0}},
                               [](int) { return ticks(1000.0, 1000.0); }),
                     bars,
                     {expect(first, first + 1000.0 * kTick, 1.0, false, 1, 5),
                      expect(second, second + 1000.0 * kTick, 1.0, false, 3, 5)});
        auto script = script_of(true, false, {{0, 2.0}, {2, 1.0}},
            [](int bar) { return bar <= 3 ? ticks(1000.0, kNaN) : ExitPlan{}; });
        script.after_orders = [](PyramidHost& host, int bar) {
            if (bar == 3) host.strategy_close("L", "", 1);
        };
        check_trades("long.profit-on-remaining-units", "red-on-base", script, bars,
                     {expect(first, bars[4].open, 1.0, true, 1, 4),
                      expect(first, first + 1000.0 * kTick, 1.0, true, 1, 5),
                      expect(second, second + 1000.0 * kTick, 1.0, true, 3, 5)});
    }

    // ---- The call that precedes the entry does not cover it -----------------
    // Each bar calls strategy.exit first and strategy.entry second, so the
    // second entry's order is younger than the exit's last call when it fills
    // on bar 3: it has no bracket on that bar, whose low (93) is below its own
    // 94. The call at bar 3's close gives it one. Bar 4 opens below 94: that
    // stop fills at the open and closes the older trade; the older entry's
    // own stop at 90 then closes the newer trade.
    {
        const std::vector<Bar> bars = cutoff_bars();
        const double older = bars[1].open;
        const double newer = bars[3].open;
        check_trades("long.entry-after-exit-call-waits-for-next-call", "red-on-base",
                     script_of(true, true, {{0, 1.0}, {2, 1.0}},
                               [](int) { return ticks(kNaN, 1000.0); }),
                     bars,
                     {expect(older, bars[4].open, 1.0, true, 1, 4),
                      expect(newer, older - 1000.0 * kTick, 1.0, true, 3, 4)});
        check_trades("long.pending-add-gets-stop-at-fill", "red-on-base",
                     script_of(true, false, {{0, 1.0}, {2, 1.0}},
                               [](int bar) { return bar == 2 ? ticks(kNaN, 1000) : ExitPlan{}; }),
                     bars, {expect(older, newer - 1000 * kTick, 1.0, true, 1, 3),
                            expect(newer, older - 1000 * kTick, 1.0, true, 3, 4)});
    }

    // ---- Control: a priced stop is one level for the whole entry id ---------
    {
        const std::vector<Bar> bars = priced_stop_bars();
        check_trades("control.long.priced-stop-both-entries", "control",
                     script_of(true, false, {{0, 1.0}, {2, 1.0}},
                               [](int) { return priced_stop(90.0); }),
                     bars,
                     {expect(bars[1].open, 90.0, 1.0, true, 1, 5),
                      expect(bars[3].open, 90.0, 1.0, true, 3, 5)});
        auto repriced = script_of(true, false, {{0, 1.0}, {2, 1.0}}, [](int bar) {
            return bar < 4 ? ticks(500.0, 500.0) : priced_stop(90.0);
        });
        check_trades("control.tick-exit-replaced-by-price", "control", repriced, bars,
                     {expect(100.0, 90.0, 1.0, true, 1, 5),
                      expect(104.0, 90.0, 1.0, true, 3, 5)});
    }
    {
        auto bars = short_bars();
        bars.push_back(bar_at(7, 114.5, 115.0, 114.0, 114.5));
        for (bool units : {true, false}) {
            auto partial = script_of(true, false, {{0, 2.0}}, [units](int bar) {
                if (bar == 1) return ticks(kNaN, 1000);
                if (bar != 2) return ExitPlan{};
                auto plan = ticks(1000, kNaN);
                if (units) plan.qty = 1;
                else plan.percent = 50;
                return plan;
            });
            partial.after_orders = [](PyramidHost& host, int bar) {
                if (bar == 5) host.strategy_close("L", "", 1);
            };
            check_trades(units ? "control.reissue-qty" : "control.reissue-qty-percent",
                         "control", partial, bars,
                         {expect(100, 110, 1, true, 1, 5), expect(100, 114.5, 1, true, 1, 6)});
        }
        auto reverse = script_of(true, false, {{0, 1.0}, {2, 1.0}},
            [](int bar) { return bar <= 3 ? ticks(1000, 1000) : ExitPlan{}; });
        reverse.after_orders = [](PyramidHost& host, int bar) {
            if (bar == 3) host.strategy_entry("S", false, kNaN, kNaN, 1);
            if (bar == 5) host.strategy_close("S");
        };
        check_trades("control.reversal-retires-both-brackets", "control", reverse, bars,
                     {expect(100, 104, 1, true, 1, 4), expect(104, 104, 1, true, 3, 4),
                      expect(104, 114.5, 1, false, 4, 6)});
        auto cancel = script_of(true, false, {{0, 1.0}, {2, 1.0}}, [](int bar) {
            if (bar == 4 || bar == 5) return ExitPlan{};
            return ticks(1000, 1000);
        });
        cancel.after_orders = [](PyramidHost& host, int bar) {
            if (bar == 4) host.strategy_exit_cancel_bracket("X", "L");
        };
        check_trades("control.withdraw-and-reissue", "control", cancel, bars,
                     {expect(100, 114.5, 1, true, 1, 7), expect(104, 114.5, 1, true, 3, 7)});
    }

    std::printf("SUMMARY rows=%d passed=%d failed=%d\n", rows_run,
                rows_run - rows_failed, rows_failed);
    return rows_failed == 0 ? 0 : 1;
}
