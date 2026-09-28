/*
 * test_margin_opposite_tapes.cpp — lane W13-ENG-MARGIN-OPP.
 *
 * TradingView checks a book at a bar's close after the script has run there,
 * and a margin call or close it has placed is an order sized when it was
 * placed, executed as a plain market order: one whose book shrank before it
 * filled opens the rest on the other side. Each rule is read off
 * TradingView's own tapes of synthetic probes written for this lane
 * (tests/fixtures/margin_opposite):
 *
 *   CP  A call the close owes is taken there after the script: the script
 *       reads the book before it, and a strategy.close_all() the script placed
 *       on that bar keeps the size it was placed with, so it closes the called
 *       units again and opens them on the other side -- at the next open, or
 *       at that close under process_orders_on_close.
 *   CQ  The script's strategy.cancel_all() withdraws that call too; it is then
 *       placed again behind the script's orders and executes after them at the
 *       next open: on the book the close_all flattened it opens the called
 *       units on the other side, on a book still held it reduces it.
 *
 * Each row replays one lab tv tape through the Pine adapter under the
 * configuration its strategy() declares, over the lane's bars, and requires
 * every trade the tape closes inside those bars (or before the row's own
 * window end) to be the engine's: entry and exit time, side, price in ticks
 * and quantity in lots. A row's open cells -- entries whose trades neither
 * side is held to, each a shape the fixtures' README lists under "Open" --
 * are left out on both sides. On the commit before a rule every row of that
 * rule fails here, and every control row passes.
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

#ifndef PINEFORGE_MARGIN_OPPOSITE_FIXTURE_DIR
#error "PINEFORGE_MARGIN_OPPOSITE_FIXTURE_DIR must name tests/fixtures/margin_opposite"
#endif
#ifndef PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR
#error "PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR must name tests/fixtures/slipped_short"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/slipped_short/bars.inc"
#include "fixtures/margin_residual/eur15_bars.inc"
#include "fixtures/margin_opposite/xau15_bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60'000;

enum class Chart { NyseF, XauUsd, EurUsd };

struct Instrument {
    double tick;
    double lot;
};

Instrument instrument(Chart chart) {
    switch (chart) {
    case Chart::NyseF: return {0.01, 1.0};        // one share
    case Chart::XauUsd: return {0.001, 0.01};     // OANDA:XAUUSD
    case Chart::EurUsd: return {0.00001, 0.01};   // OANDA:EURUSD
    }
    return {0.01, 1.0};
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

// The probes, as their generated TUs lower them (<tape>/strategy.pine).
enum class Probe {
    // w13-b*-f / w13-a3-mkt-flip-f: from flat every 4 bars a short one tick
    // under the close sized at the close (qty = equity / close); on the bar it
    // fills, per mode, strategy.close_all(), strategy.cancel_all() then
    // strategy.close_all(), strategy.cancel_all() alone, or nothing; anything
    // still open later is closed with strategy.close_all().
    EveryFourBars,
    // w13-x*-xau2: the same from flat at the top of every hour, ten ticks under
    // the close.
    TopOfHour,
    // w13-p1-eur / w13-p2-eur: every 3 bars from flat a default long (100 % of
    // equity), strategy.close_all() on the next bar -- under
    // process_orders_on_close (p1) at that bar's close, else at the next open.
    CloseNextBar,
};

enum class Mode { Close, CancelClose, Cancel, None, Market };

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, Mode mode, Chart chart, const source::PineStrategyConfig& config)
        : probe_(probe), mode_(mode) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", instrument(chart).lot);
        set_syminfo_mintick(instrument(chart).tick);
        set_syminfo_timezone("America/New_York");
        switch (chart) {
        case Chart::NyseF: set_syminfo_session("0930-1600"); break;
        case Chart::XauUsd: set_syminfo_session("1800-1700"); break;
        case Chart::EurUsd: set_syminfo_session("1700-1700"); break;
        }
    }

    void on_source_bar(const Bar&) override {
        switch (probe_) {
        case Probe::EveryFourBars:
            stop_cells(pine_bar_index() % 4 == 0, syminfo_.mintick);
            break;
        case Probe::TopOfHour: {
            const std::int64_t minute = ((current_bar_.timestamp / kMinute) % 60 + 60) % 60;
            stop_cells(minute == 0, 10.0 * syminfo_.mintick);
            break;
        }
        case Probe::CloseNextBar: {
            const int phase = pine_bar_index() % 3;
            if (signed_position_size() == 0.0 && phase == 0) strategy_entry("L", true);
            if (phase == 1 || (phase == 2 && signed_position_size() != 0.0))
                strategy_close("", "", kNaN, kNaN, false);
            break;
        }
        }
    }

private:
    // var int st = 0; lvl = close - offset
    void stop_cells(bool cell, double offset) {
        const double lvl = current_bar_.close - offset;
        const double qty = (current_equity() + open_profit(current_bar_.close))
            / current_bar_.close;
        if (st_ == 0) {
            if (signed_position_size() == 0.0 && cell) {
                if (mode_ == Mode::Market) strategy_entry("S", false, kNaN, kNaN, qty);
                else strategy_entry("S", false, kNaN, lvl, qty);
                st_ = 1;
            }
        } else if (st_ == 1) {
            if (signed_position_size() < 0.0) {
                if (mode_ == Mode::Close || mode_ == Mode::Market) {
                    strategy_close("", "C0", kNaN, kNaN, false);
                } else if (mode_ == Mode::CancelClose) {
                    strategy_cancel_all();
                    strategy_close("", "C0", kNaN, kNaN, false);
                } else if (mode_ == Mode::Cancel) {
                    strategy_cancel_all();
                }
                st_ = 2;
            } else {
                if (mode_ != Mode::Market) strategy_cancel_all();
                st_ = 0;
            }
        } else if (signed_position_size() != 0.0) {
            strategy_close("", "C1", kNaN, kNaN, false);
        } else {
            st_ = 0;
        }
    }

    Probe probe_;
    Mode mode_;
    int st_ = 0;
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
Run run(Probe probe, Mode mode, Chart chart, const source::PineStrategyConfig& config,
        const std::vector<Bar>& bars, std::int64_t end_ms) {
    ProbeHost host(probe, mode, chart, config);
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
    if (tape_only.size() > 6) tape_only.resize(6);
    if (engine_only.size() > 6) engine_only.resize(6);
    show("tape", tape_only);
    show("engine", engine_only);
}

// What a probe's strategy() declares, over v6's defaults (100 % of equity,
// pyramiding 1, margin 100 both ways).
source::PineStrategyConfig config(double capital, double commission_percent, int slippage,
                                  bool process_on_close = false) {
    source::PineStrategyConfig c{};
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 1;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission_percent;
    c.slippage = slippage;
    c.process_orders_on_close = process_on_close;
    return c;
}

struct Case {
    const char* rule;
    const char* tape;
    const char* dir;   // fixture directory holding the tape
    Probe probe;
    Mode mode;
    Chart chart;
    source::PineStrategyConfig lane;   // what its strategy() declares
    std::size_t closed;                // tape trades compared
    const char* until = nullptr;       // UTC; trades closed from here on are not compared
    std::vector<const char*> open = {};   // UTC entry times of the open cells
};

// The trades of a row: those closed before its window end, less its open
// cells.
std::vector<Row> compared(std::vector<Row> rows, std::int64_t end,
                          const std::vector<std::int64_t>& open) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) {
                   return std::get<4>(r) >= end
                       || std::find(open.begin(), open.end(), std::get<0>(r)) != open.end();
               }),
               rows.end());
    return rows;
}

}  // namespace

int main() {
    const std::vector<Bar> f15 = feed(kF15Q2);
    const std::vector<Bar> xau15 = feed(kXau15);
    const std::vector<Bar> eur15 = feed(kEurUsd15);
    const char* opposite = PINEFORGE_MARGIN_OPPOSITE_FIXTURE_DIR;

    const Case cases[] = {
        {"CP", "w13-b1-close-f", opposite, Probe::EveryFourBars, Mode::Close, Chart::NyseF,
         config(10000.0, 0.0, 0), 358},
        {"CP", "w13-x1-close-xau2", opposite, Probe::TopOfHour, Mode::Close, Chart::XauUsd,
         config(10000.0, 0.0, 0), 1384},
        {"CP", "w13-p1-eur", opposite, Probe::CloseNextBar, Mode::None, Chart::EurUsd,
         config(10000000.0, 0.0, 0, true), 510},
        {"CP", "w13-p2-eur", opposite, Probe::CloseNextBar, Mode::None, Chart::EurUsd,
         config(10000000.0, 0.0, 0), 297, nullptr, {"2025-04-02 16:45", "2025-04-03 06:15"}},
        {"CQ", "w13-b2-cancel-close-f", opposite, Probe::EveryFourBars, Mode::CancelClose,
         Chart::NyseF, config(10000.0, 0.0, 0), 331},
        {"CQ", "w13-b2s-cancel-close-f", opposite, Probe::EveryFourBars, Mode::CancelClose,
         Chart::NyseF, config(10000.0, 0.0, 1), 355},
        {"CQ", "w13-b4-cancel-f", opposite, Probe::EveryFourBars, Mode::Cancel, Chart::NyseF,
         config(10000.0, 0.0, 0), 434},
        {"CQ", "w13-x2-cancel-close-xau2", opposite, Probe::TopOfHour, Mode::CancelClose,
         Chart::XauUsd, config(10000.0, 0.0, 0), 1358},
        {"control", "w13-b5-none-f", opposite, Probe::EveryFourBars, Mode::None, Chart::NyseF,
         config(10000.0, 0.0, 0), 436},
        {"control", "w13-x5-none-xau2", opposite, Probe::TopOfHour, Mode::None, Chart::XauUsd,
         config(10000.0, 0.0, 0), 1477},
        {"control", "w13-a3-mkt-flip-f", opposite, Probe::EveryFourBars, Mode::Market,
         Chart::NyseF, config(10000.0, 0.0, 0), 435},
    };

    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.rule, c.tape);
        const std::vector<Bar>& bars = c.chart == Chart::NyseF ? f15
            : c.chart == Chart::XauUsd ? xau15 : eur15;
        const std::int64_t bars_end = bars.back().timestamp;
        const std::int64_t end = c.until ? std::min(utc_ms(c.until), bars_end) : bars_end;
        std::vector<std::int64_t> open;
        for (const char* cell : c.open) open.push_back(utc_ms(cell));
        const Instrument inst = instrument(c.chart);
        const std::vector<Row> tape = compared(
            tape_trades(std::string(c.dir) + "/" + c.tape, inst, bars_end), end, open);
        CHECK(tape.size() == c.closed);
        if (tape.size() != c.closed) std::printf("    tape closes %zu\n", tape.size());
        const Run lane = run(c.probe, c.mode, c.chart, c.lane, bars, bars_end);
        CHECK(lane.error.empty());
        const std::vector<Row> engine = compared(lane.trades, end, open);
        CHECK(engine == tape);
        if (engine != tape) {
            std::printf("    tape %zu trades, engine %zu\n", tape.size(), engine.size());
            show_difference(tape, engine);
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
