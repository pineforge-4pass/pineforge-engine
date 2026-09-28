/*
 * test_int29_rule_compositions.cpp — lane INT29.
 *
 * INT29 integrates lane W10-DIAG-UNKNOWN onto main, which already carries lane
 * W5B-ENG-MARGIN-RESIDUAL (through INT28-FIX). Their fill-time margin rules
 * meet in one function, PineExecutionAdapter::validate_precommit:
 *   SB (W5B)           a flat bar's same-side MARKET entries each fill on
 *                      their own cost; the book is called once they have;
 *   PA (W5B)           an add is judged on its combined margin with no lot of
 *                      slack;
 *   COQ (W10)          a MARKET entry filled at the open after its bar is
 *                      judged against the position held at its placement,
 *                      not a sibling that filled at that open a moment
 *                      earlier;
 *   SAMEOPEN-REV (W10) a default-size MARKET entry that reverses a position
 *                      its own bar's entry opened at that open must fit with
 *                      that position still margined.
 * SB decides a flat-placed sibling before COQ's placement-held requirement
 * is reached, so the two split the co-queued entries by the side they were
 * placed on; SAMEOPEN-REV and PA act in the frozen-sizing arm, one on a
 * reversal and one on an add.
 *
 * The row replays TradingView's own tape of a synthetic probe written for
 * INT29 (tests/fixtures/int29_compositions, lab tv --no-note on
 * BINANCE:ETHUSDT.P 15) through the Pine adapter under the configuration the
 * generated constructor declares, over the corpus 15m bars embedded in
 * bars.inc, and requires every trade the tape closes inside those bars to be
 * the engine's: entry and exit time, side, price and quantity. Its five
 * cell kinds put the rules together at one open: A (SB), B (COQ), C (SB and
 * SAMEOPEN-REV), D (COQ and PA) and E (PA on two co-queued default adds). It
 * also reads what each kind proves off TradingView's own rows.
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

#ifndef PINEFORGE_INT29_COMPOSITIONS_FIXTURE_DIR
#error "PINEFORGE_INT29_COMPOSITIONS_FIXTURE_DIR must name tests/fixtures/int29_compositions"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/int29_compositions/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
constexpr std::int64_t kMinute = 60'000;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

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

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// 2025-04-<day> <hour>:<minute> UTC.
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * kMinute;
}

// The probe's first cell (2025-04-02 00:00 UTC); the harness trades from the
// bar that places it, as run_strategy.py does for a corpus tape.
const std::int64_t kFirstCellMs = at(2, 0, 0);
constexpr std::int64_t kStep = 120 * kMinute;
constexpr std::int64_t kQuarter = 15 * kMinute;
constexpr std::int64_t kCells = 120;

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct Tape {
    std::vector<Row> trades;           // closed inside the replayed bars, in entry order
    std::vector<std::string> entries;  // each trade's entry signal
    std::vector<std::string> exits;    // each trade's exit signal
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_INT29_COMPOSITIONS_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal, exit_signal;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price USDT, Size (qty), ...
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        const double price = std::stod(cell[4]);
        CHECK(on_grid(price, kTick));
        if (cell[1].rfind("Entry", 0) == 0) {
            const double qty = std::stod(cell[5]);
            CHECK(on_grid(qty, kLot));
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = lots(qty);
            entry_signal[number] = cell[3];
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        const auto signal = exit_signal.find(number);
        if (signal != exit_signal.end() && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            out.entries.push_back(entry_signal[number]);
            out.exits.push_back(signal->second);
        }
    }
    return out;
}

// The probe, as its generated TU lowers it
// (fixtures/int29_compositions/int29-c1-open-pairs/strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const source::PineStrategyConfig& config) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t rel = current_bar_.timestamp - kFirstCellMs;
        if (rel < 0 || rel >= kCells * kStep) return;
        const std::int64_t phase = rel % kStep;
        const std::int64_t kind = (rel / kStep) % 5;
        if (phase == 0 && signed_position_size() == 0.0) {
            if (kind == 0) {
                strategy_entry("A1", true, kNaN, kNaN, q(60), "A1");
                strategy_entry("A2", true, kNaN, kNaN, q(60), "A2");
            } else if (kind == 1) {
                strategy_entry("B0", true, kNaN, kNaN, q(30), "B0");
            } else if (kind == 2) {
                strategy_entry("C1", true, kNaN, kNaN, q(80), "C1");
                strategy_entry("CS", false, kNaN, kNaN, kNaN, "CS");
                strategy_entry("C2", true, kNaN, kNaN, q(30), "C2");
            } else {
                strategy_entry("D0", true, kNaN, kNaN, q(50), "D0");
            }
        }
        if (phase == kQuarter && open_trades() == 1) {
            if (kind == 1) {
                strategy_entry("B1", true, kNaN, kNaN, q(40), "B1");
                strategy_entry("B2", true, kNaN, kNaN, q(40), "B2");
            } else if (kind == 3) {
                strategy_entry("DE", true, kNaN, kNaN, q(30), "DE");
                strategy_entry("DD", true, kNaN, kNaN, kNaN, "DD");
            } else if (kind == 4) {
                strategy_entry("E1", true, kNaN, kNaN, kNaN, "E1");
                strategy_entry("E2", true, kNaN, kNaN, kNaN, "E2");
            }
        }
        if (phase == 3 * kQuarter) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

private:
    int open_trades() const { return static_cast<int>(pyramid_entries_.size()); }
    // strategy.equity at this bar's close.
    double equity() { return current_equity() + open_profit(current_bar_.close); }
    // q(pct): math.floor(strategy.equity * pct / 100 / close * 10000) / 10000.
    double q(double pct) {
        return std::floor(equity() * pct / 100.0 / current_bar_.close * 10000.0) / 10000.0;
    }
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(const source::PineStrategyConfig& config, std::int64_t end_ms) {
    ProbeHost host(config);
    host.set_trade_start_time(kFirstCellMs);
    const std::vector<Bar> bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                                t.exit_time, ticks(t.exit_price));
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

// What the generated constructor declares: v6's 100000 capital and margins
// of 100, pyramiding 4, and the probe's default size of 30 % of equity.
source::PineStrategyConfig config() {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 30.0;
    c.pyramiding = 4;
    c.commission_value = 0.0;
    c.slippage = 0;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;

    std::printf("-- int29-c1-open-pairs (declares percent_of_equity 30, pyramiding 4)\n");
    const Tape tape = tape_trades("int29-c1-open-pairs", end_ms);
    const Run engine = run(config(), end_ms);
    CHECK(engine.error.empty());
    CHECK(engine.trades == tape.trades);
    if (engine.trades != tape.trades) {
        std::size_t first = 0;
        while (first < engine.trades.size() && first < tape.trades.size()
               && engine.trades[first] == tape.trades[first]) {
            ++first;
        }
        std::printf("  first difference at trade %zu of %zu (tape) / %zu (engine)\n", first,
                    tape.trades.size(), engine.trades.size());
        const std::size_t from = first < 3 ? 0 : first - 3;
        show("tape", std::vector<Row>(tape.trades.begin() + static_cast<std::ptrdiff_t>(
                                          std::min(from, tape.trades.size())),
                                      tape.trades.begin() + static_cast<std::ptrdiff_t>(
                                          std::min(first + 6, tape.trades.size()))));
        show("engine", std::vector<Row>(engine.trades.begin() + static_cast<std::ptrdiff_t>(
                                            std::min(from, engine.trades.size())),
                                        engine.trades.begin() + static_cast<std::ptrdiff_t>(
                                            std::min(first + 6, engine.trades.size()))));
    }

    // What each kind proves, read off TradingView's own rows: every one of
    // the 120 cells (24 of each kind) books the same shape.
    std::printf("-- the composition, on the tape\n");
    CHECK(tape.trades.size() == 432);
    std::map<std::int64_t, std::vector<std::size_t>> by_cell;
    for (std::size_t i = 0; i < tape.trades.size(); ++i)
        by_cell[(std::get<0>(tape.trades[i]) - kFirstCellMs) / kStep].push_back(i);
    CHECK(by_cell.size() == static_cast<std::size_t>(kCells));
    int shaped[5] = {0, 0, 0, 0, 0};
    for (const auto& item : by_cell) {
        const std::vector<std::size_t>& rows = item.second;
        const std::int64_t kind = item.first % 5;
        const auto entry = [&](std::size_t k) { return tape.entries[rows[k]]; };
        const auto exit = [&](std::size_t k) { return tape.exits[rows[k]]; };
        const auto opened = [&](std::size_t k) { return std::get<0>(tape.trades[rows[k]]); };
        const auto closed = [&](std::size_t k) { return std::get<4>(tape.trades[rows[k]]); };
        const auto qty = [&](std::size_t k) { return std::get<3>(tape.trades[rows[k]]); };
        bool ok = false;
        if (kind == 0 && rows.size() == 3) {
            // A (SB): both flat longs fill at one open; the book is called
            // there, the first entry whole and the second in part.
            ok = entry(0) == "A1" && exit(0) == "Margin call" && closed(0) == opened(0)
                && entry(1) == "A2" && exit(1) == "Margin call" && closed(1) == opened(1)
                && entry(2) == "A2" && exit(2) == "cleanup" && opened(2) == opened(0)
                && qty(0) == qty(1) + qty(2);
        } else if (kind == 1 && rows.size() == 4) {
            // B (COQ): both adds fill at one open on top of the seed; the
            // call there takes the seed whole and the first add in part.
            ok = entry(0) == "B0" && exit(0) == "Margin call" && closed(0) == opened(1)
                && entry(1) == "B1" && exit(1) == "Margin call" && closed(1) == opened(1)
                && entry(2) == "B1" && exit(2) == "cleanup"
                && entry(3) == "B2" && exit(3) == "cleanup" && opened(3) == opened(1);
        } else if (kind == 2 && rows.size() == 3) {
            // C (SB + SAMEOPEN-REV): both longs fill at one open; the default
            // short that would reverse them there is refused (no short row);
            // the long book is called at that open.
            ok = entry(0) == "C1" && exit(0) == "Margin call" && closed(0) == opened(0)
                && entry(1) == "C1" && exit(1) == "cleanup"
                && entry(2) == "C2" && exit(2) == "cleanup" && opened(2) == opened(0)
                && std::get<1>(tape.trades[rows[0]]) && std::get<1>(tape.trades[rows[2]]);
        } else if ((kind == 3 || kind == 4) && rows.size() == 4) {
            // D (COQ + PA) and E (PA pair): both adds -- an explicit one and a
            // default one, or two default ones -- fill at one open on top of
            // the seed, each judged on the seed and itself; the call there
            // takes part of the seed.
            const bool d = kind == 3;
            ok = entry(0) == "D0" && exit(0) == "Margin call" && closed(0) == opened(2)
                && entry(1) == "D0" && exit(1) == "cleanup"
                && entry(2) == (d ? "DE" : "E1") && exit(2) == "cleanup"
                && entry(3) == (d ? "DD" : "E2") && exit(3) == "cleanup"
                && opened(3) == opened(2);
        }
        CHECK(ok);
        shaped[kind] += ok ? 1 : 0;
    }
    for (const int n : shaped) CHECK(n == 24);

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
