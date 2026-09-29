/*
 * test_exit_reservation_tapes.cpp — lane TAIL-H.
 *
 * TradingView reserves an entry's quantity for its strategy.exit orders in
 * the order the exits were created (lane W3-ENG-EXIT-ALLOC), and two rules
 * of that reservation are read off TradingView's own tapes of synthetic
 * probes written for this lane (tests/fixtures/exit_reservation):
 *
 *   PS  A percentage exit's share floors to the symbol's quantity step, but
 *       an exit whose floored share is nothing keeps one step while the
 *       position holds one: 50 % of one 0.01 lot of OANDA:XAUUSD reserves
 *       the whole lot, as 50 % of one contract does. An exit created after it
 *       holds nothing, so its stop never fires (tailh-d1a); created before
 *       it, the stop exit holds the lot (tailh-d1c). strategy.close with
 *       qty_percent = 50 closes the whole lot the same way, and 0.01 of 0.03
 *       (tailh-d1e-01, tailh-d1e-03).
 *   PK  A percentage exit keeps the share it reserved when the position
 *       shrinks under it (margin calls): 50 % of 25 contracts holds 12, and
 *       with 8 contracts left it holds all 8, so the exit created after it
 *       holds nothing and its stop never fires (tailh-d2a); created before
 *       it, the stop exit fires for the 8 (tailh-d2b).
 *
 * Each row replays one lab tv tape through the Pine adapter under the
 * configuration its strategy() declares, over the lane's bars, and requires
 * every trade the tape closes inside those bars to be the engine's: entry
 * and exit time, side, price in ticks and quantity in lots.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_EXIT_RESERVATION_FIXTURE_DIR
#error "PINEFORGE_EXIT_RESERVATION_FIXTURE_DIR must name tests/fixtures/exit_reservation"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/exit_reservation/es15_bars.inc"
#include "fixtures/exit_reservation/xau15_q4_bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60'000;

enum class Chart { Es1, XauUsd };

struct Instrument {
    double tick;
    double lot;
    double point_value;
    const char* timezone;
    const char* session;
};

Instrument instrument(Chart chart) {
    switch (chart) {
    case Chart::Es1: return {0.25, 1.0, 50.0, "America/Chicago", "1700-1600"};  // CME_MINI:ES1!
    case Chart::XauUsd: return {0.001, 0.01, 1.0, "America/New_York", "1800-1700"};  // OANDA:XAUUSD
    }
    return {0.01, 1.0, 1.0, "UTC", "24x7"};
}

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// A stamp "YYYY-MM-DD HH:MM" at UTC+`offset_hours` -> UTC milliseconds.
std::int64_t stamp_ms(const std::string& stamp, int offset_hours) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - offset_hours) * 60 + mi) * kMinute;
}

// A tape stamp is rendered at UTC+8; the rows below name UTC times.
std::int64_t tape_ms(const std::string& stamp) { return stamp_ms(stamp, 8); }
std::int64_t utc_ms(const std::string& stamp) { return stamp_ms(stamp, 0); }

std::vector<Row> tape_trades(const std::string& path, Instrument inst, std::int64_t end_ms) {
    std::ifstream in(path + "/tv_trades.csv");
    CHECK(in.good());
    std::map<int, Row> by_number;
    std::map<int, bool> closed;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price, Size (qty), ...
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        const double price = std::stod(cell[4]);
        CHECK(on_grid(price, inst.tick));
        if (cell[1].rfind("Entry", 0) == 0) {
            const double qty = std::stod(cell[5]);
            CHECK(on_grid(qty, inst.lot));
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = std::llround(price / inst.tick);
            std::get<3>(row) = std::llround(qty / inst.lot);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = std::llround(price / inst.tick);
            closed[number] = true;
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number) {
        if (closed.count(number) && std::get<4>(row) < end_ms) out.push_back(row);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The probes, as their generated TUs lower them (<tape>/strategy.pine): on
// the bar opening at `entry` a long of `qty` and two exits from it, a 50 %
// limit exit ("TP1_L") and a stop-and-limit exit ("MAIN_L"), in `order`; or
// (Order::ClosePercent) strategy.close("L", qty_percent = 50) on the next bar
// and strategy.close("L") of the rest an hour later.
enum class Order { PercentFirst, StopFirst, ClosePercent };

struct Probe {
    const char* entry;   // UTC
    double qty;
    double percent_limit;
    double stop;
    double stop_limit;
    Order order;
};

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(const Probe& probe, Chart chart, const source::PineStrategyConfig& config)
        : probe_(probe), entry_ms_(utc_ms(probe.entry)) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        const Instrument inst = instrument(chart);
        set_syminfo_metadata("qty_step", inst.lot);
        set_syminfo_mintick(inst.tick);
        set_syminfo_pointvalue(inst.point_value);
        set_syminfo_timezone(inst.timezone);
        set_syminfo_session(inst.session);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t at = current_bar_.timestamp;
        if (probe_.order == Order::ClosePercent) {
            if (at == entry_ms_ + 15 * kMinute) strategy_close("L", "half", kNaN, 50.0);
            if (at == entry_ms_ + 75 * kMinute) strategy_close("L", "rest");
        }
        if (at != entry_ms_) return;
        strategy_entry("L", true, kNaN, kNaN, probe_.qty);
        if (probe_.order == Order::PercentFirst) {
            percent_exit();
            stop_exit();
        } else if (probe_.order == Order::StopFirst) {
            stop_exit();
            percent_exit();
        }
    }

private:
    void percent_exit() {
        strategy_exit("TP1_L", "L", probe_.percent_limit, kNaN, kNaN, kNaN, kNaN, 50.0);
    }
    void stop_exit() { strategy_exit("MAIN_L", "L", probe_.stop_limit, probe_.stop); }

    Probe probe_;
    std::int64_t entry_ms_;
};

template <std::size_t N>
std::vector<Bar> feed(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& row : rows) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(const Probe& probe, Chart chart, const source::PineStrategyConfig& config,
        const std::vector<Bar>& bars, std::int64_t end_ms) {
    ProbeHost host(probe, chart, config);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    const Instrument inst = instrument(chart);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        CHECK(on_grid(t.entry_price, inst.tick));
        CHECK(on_grid(t.exit_price, inst.tick));
        CHECK(on_grid(t.qty, inst.lot));
        out.trades.emplace_back(t.entry_time, t.is_long, std::llround(t.entry_price / inst.tick),
                                std::llround(t.qty / inst.lot), t.exit_time,
                                std::llround(t.exit_price / inst.tick));
    }
    std::sort(out.trades.begin(), out.trades.end());
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// Only the rows the two sides do not share, so a failure reads as the first
// trades that differ.
void show_difference(const std::vector<Row>& tape, const std::vector<Row>& engine) {
    std::vector<Row> tape_only, engine_only;
    std::set_difference(tape.begin(), tape.end(), engine.begin(), engine.end(),
                        std::back_inserter(tape_only));
    std::set_difference(engine.begin(), engine.end(), tape.begin(), tape.end(),
                        std::back_inserter(engine_only));
    if (tape_only.size() > 8) tape_only.resize(8);
    if (engine_only.size() > 8) engine_only.resize(8);
    show("tape", tape_only);
    show("engine", engine_only);
}

// What every probe's strategy() declares: 100000 of capital, a fixed default
// quantity of 1, pyramiding 1, orders processed on close, margin 1 % both ways.
source::PineStrategyConfig config() {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 1;
    c.process_orders_on_close = true;
    c.margin_long = 1.0;
    c.margin_short = 1.0;
    return c;
}

struct Case {
    const char* rule;
    const char* tape;
    Chart chart;
    Probe probe;
    std::size_t closed;   // tape trades compared
};

}  // namespace

int main() {
    const std::vector<Bar> es15 = feed(kEs15);
    const std::vector<Bar> xau15 = feed(kXau15Q4);
    const std::string dir = PINEFORGE_EXIT_RESERVATION_FIXTURE_DIR;

    const Case cases[] = {
        {"PS", "tailh-d1a", Chart::XauUsd,
         {"2025-10-21 08:45", 0.01, 4331.72, 4245.365, 4389.29, Order::PercentFirst}, 1},
        {"PS", "tailh-d1b", Chart::XauUsd,
         {"2025-10-21 08:45", 0.03, 4331.72, 4245.365, 4389.29, Order::PercentFirst}, 2},
        {"PS-control", "tailh-d1c", Chart::XauUsd,
         {"2025-10-21 08:45", 0.01, 4331.72, 4245.365, 4389.29, Order::StopFirst}, 1},
        {"PS", "tailh-d1e-01", Chart::XauUsd,
         {"2025-10-21 08:45", 0.01, kNaN, kNaN, kNaN, Order::ClosePercent}, 1},
        {"PS", "tailh-d1e-03", Chart::XauUsd,
         {"2025-10-21 08:45", 0.03, kNaN, kNaN, kNaN, Order::ClosePercent}, 2},
        {"PK", "tailh-d2a", Chart::Es1,
         {"2025-04-02 22:15", 25.0, 5595.75, 5477.25, 5674.75, Order::PercentFirst}, 7},
        {"PK-control", "tailh-d2b", Chart::Es1,
         {"2025-04-02 22:15", 25.0, 5595.75, 5477.25, 5674.75, Order::StopFirst}, 3},
    };

    int rows_ok = 0;
    for (const Case& c : cases) {
        const std::vector<Bar>& bars = c.chart == Chart::Es1 ? es15 : xau15;
        const std::int64_t end = bars.back().timestamp + 15 * kMinute;
        const auto tape = tape_trades(dir + "/" + c.tape, instrument(c.chart), end);
        const Run engine = run(c.probe, c.chart, config(), bars, end);
        const bool same = engine.error.empty() && tape == engine.trades;
        std::printf("  %-10s %-10s tape %zu trades, engine %zu: %s\n", c.rule, c.tape,
                    tape.size(), engine.trades.size(), same ? "same" : "DIFFERENT");
        if (!engine.error.empty()) std::printf("    engine error: %s\n", engine.error.c_str());
        if (!same) show_difference(tape, engine.trades);
        CHECK(tape.size() == c.closed);
        CHECK(same);
        rows_ok += same ? 1 : 0;
    }
    std::printf("\nexit_reservation_tapes: %d of %zu rows replay; %d passed, %d failed\n", rows_ok,
                sizeof(cases) / sizeof(cases[0]), tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
