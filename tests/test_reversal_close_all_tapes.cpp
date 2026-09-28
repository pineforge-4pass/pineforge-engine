/*
 * test_reversal_close_all_tapes.cpp — lane INT28-FIX, rule CA.
 *
 * A strategy.close_all() placed after the same bar's default-size reversal
 * entry, at 100 % of equity, belongs to that reversal, as a strategy.close()
 * of the held id does: TradingView flattens the held position through
 * neither. At the next open the reversal decides alone. Where the open funds
 * it, the reversal closes the position and opens the other side, and the
 * close_all does not flatten that side; where the open declines it (its
 * units at the open cost more than the equity they were sized from), the
 * position is held to its next exit and the close_all does not fill either.
 *
 * The lowering placed that close_all as a Flatten request of its own: at a
 * declined open it flattened the held position, and the flat book then took
 * the next bar's entries as two openings where TradingView reverses once
 * (job-2614-andrewwieiw-frosty-alerts on BINANCE:ETHUSDT.P 15, whose
 * end-of-window strategy.close_all follows its reversal entries every day).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/reversal_close_all, lab tv --no-note exports on
 * BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-17) through the Pine host under
 * the configuration its strategy() declares over Pine v6's defaults, on the
 * corpus 15m bars of tests/fixtures/margin_v6/bars.inc, and requires every
 * trade the tape closes to be the engine's: entry and exit time, side, price
 * in ticks and quantity in lots. Every probe runs the same twelve cells: a
 * position opened at a cell's close (long at the even cells, short at the
 * odd ones), the cell's orders at the close 30 minutes later, a cleanup
 * (cancel_all, close_all) 90 minutes after the cell. The next open declines
 * the 100 % reversal of eight cells (01 01:30, 02 12:30, 03 06:15, 03 18:15,
 * 04 15:30, 06 19:45, 07 03:45, 07 08:45 UTC) and funds the other four.
 *
 * - int28fix-ca2-rev (rule): the reversal entry, then strategy.close_all().
 *   TradingView holds the eight declined positions to the cleanup and
 *   reverses the four funded ones, which the cleanup closes.
 * - int28fix-ca2-frosty (rule): job-2614's order -- the long entry, the short
 *   entry, strategy.close() of the held id, strategy.close_all(). The same
 *   tape.
 * - int28fix-ca2-close-id (control): the reversal entry, then
 *   strategy.close() of the held id -- the pair the adapter already holds
 *   (tests/fixtures/margin_v6 w5-rv-*). The same tape again.
 * - int28fix-ca2-first (control): strategy.close_all() before the reversal
 *   entry flattens every cell at the open, and the entry then opens from flat.
 * - int28fix-ca2-p99 (control): at 99 % no reversal is declined; every cell
 *   reverses and the close_all never flattens the new side.
 *
 * Under process_orders_on_close the reversal fills at the close it was sized
 * at and is never declined, so the rule does not reach it (the lane's
 * int28fix-ca2-pooc tape, kept with its scratch evidence: TradingView
 * reverses every cell there too).
 *
 * On the lane's base the two rule rows fail (the engine flattens the eight
 * declined cells at the open) and the three control rows pass.
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

#ifndef PINEFORGE_REVERSAL_CLOSE_ALL_FIXTURE_DIR
#error "PINEFORGE_REVERSAL_CLOSE_ALL_FIXTURE_DIR must name tests/fixtures/reversal_close_all"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// BINANCE:ETHUSDT.P 15m, 2025-04-01 00:00 .. 2025-04-17 00:00 UTC.
#include "fixtures/margin_v6/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kLot = 0.0001;  // TradingView's BINANCE:ETHUSDT.P quantity step
constexpr std::int64_t kMinute = 60'000;

// (entry ms, long, entry price, quantity, exit ms, exit price), prices in
// ticks and quantity in lots.
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// timestamp("UTC", 2025, 4, day, hour, minute)
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * kMinute;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct Tape {
    std::vector<Row> trades;
    std::vector<std::string> exits;  // TradingView's exit signal per trade
};

Tape read_tape(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_REVERSAL_CLOSE_ALL_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    CHECK(in.good());
    std::map<int, Row> by_number;
    std::map<int, std::string> exit_signal;
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
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(std::stod(cell[4]));
            std::get<3>(row) = lots(std::stod(cell[5]));
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(std::stod(cell[4]));
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        out.trades.push_back(row);
        out.exits.push_back(exit_signal[number]);
    }
    return out;
}

// The cells' shapes, as their generated TUs lower them
// (tests/fixtures/reversal_close_all/*/strategy.pine).
enum class Probe { Rev, Frosty, CloseId, First };

const std::int64_t kCells[] = {
    at(1, 1, 30), at(2, 12, 30), at(2, 17, 45), at(2, 22, 45), at(3, 6, 15), at(3, 18, 15),
    at(4, 4, 15), at(4, 11, 30), at(4, 15, 30), at(6, 19, 45), at(7, 3, 45), at(7, 8, 45),
};

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config) : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        for (std::size_t i = 0; i < std::size(kCells); ++i) {
            const std::int64_t cell = kCells[i];
            const bool held_long = i % 2 == 0;
            const char* held = held_long ? "L" : "S";
            const char* other = held_long ? "S" : "L";
            if (t == cell) strategy_entry(held, held_long, kNaN, kNaN, kNaN, "");
            if (t == cell + 30 * kMinute) {
                switch (probe_) {
                case Probe::Rev:
                    strategy_entry(other, !held_long, kNaN, kNaN, kNaN, "");
                    close_all("flatten");
                    break;
                case Probe::Frosty:
                    strategy_entry("L", true, kNaN, kNaN, kNaN, "");
                    strategy_entry("S", false, kNaN, kNaN, kNaN, "");
                    strategy_close(held, "", kNaN, kNaN, false);
                    close_all("flatten");
                    break;
                case Probe::CloseId:
                    strategy_entry(other, !held_long, kNaN, kNaN, kNaN, "");
                    strategy_close(held, "", kNaN, kNaN, false);
                    break;
                case Probe::First:
                    close_all("flatten");
                    strategy_entry(other, !held_long, kNaN, kNaN, kNaN, "");
                    break;
                }
            }
            if (t == cell + 90 * kMinute) {
                strategy_cancel_all();
                close_all("cleanup");
            }
        }
    }

private:
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }

    Probe probe_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
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
    std::vector<std::string> exits;
    std::string error;
};

Run run(Probe probe, const source::PineStrategyConfig& config, const std::vector<Bar>& bars) {
    ProbeHost host(probe, config);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= bars.back().timestamp) continue;  // open at the range end
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                                t.exit_time, ticks(t.exit_price));
        out.exits.push_back(t.exit_comment);
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// What a probe's strategy() declares over Pine v6's defaults (capital 100000,
// 100 % of equity, pyramiding 1, margin 100 both ways).
source::PineStrategyConfig v6(double percent = 100.0) {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = percent;
    c.pyramiding = 1;
    return c;
}

std::size_t count(const std::vector<std::string>& signals, const std::string& signal) {
    return static_cast<std::size_t>(std::count(signals.begin(), signals.end(), signal));
}

}  // namespace

int main() {
    const std::vector<Bar> bars = feed();
    struct Case {
        const char* role;
        const char* tape;
        Probe probe;
        source::PineStrategyConfig lane;
        std::size_t trades;     // the tape's trades, all closed inside the bars
        std::size_t flattened;  // of them closed by the cells' close_all
        bool engine_signals;    // the engine books the same closing orders
    };
    // At 99 % the rule is out of scope: the engine flattens at the open and
    // opens the other side there, which books TradingView's trades exactly.
    const Case cases[] = {
        {"CA", "int28fix-ca2-rev", Probe::Rev, v6(), 25, 0, true},
        {"CA", "int28fix-ca2-frosty", Probe::Frosty, v6(), 25, 0, true},
        {"control", "int28fix-ca2-close-id", Probe::CloseId, v6(), 25, 0, true},
        {"control", "int28fix-ca2-first", Probe::First, v6(), 40, 12, true},
        {"control", "int28fix-ca2-p99", Probe::Rev, v6(99.0), 30, 0, false},
    };
    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.role, c.tape);
        const Tape tape = read_tape(c.tape);
        CHECK(tape.trades.size() == c.trades);
        CHECK(count(tape.exits, "flatten") == c.flattened);
        const Run lane = run(c.probe, c.lane, bars);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape.trades);
        if (c.engine_signals) CHECK(count(lane.exits, "flatten") == c.flattened);
        if (lane.trades != tape.trades) {
            show("tape", tape.trades);
            show("engine", lane.trades);
        }
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
