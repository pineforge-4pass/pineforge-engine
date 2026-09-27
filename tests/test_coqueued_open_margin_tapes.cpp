/*
 * test_coqueued_open_margin_tapes.cpp — lane W10-DIAG-UNKNOWN, rule COQ.
 *
 * TradingView judges each MARKET entry it fills at a bar's open against the
 * position held when the entry was placed, not against a sibling entry that
 * filled at the same open a moment earlier. Two entries placed on one bar
 * that each fit the equity alone both fill, even when together they do not;
 * the excess is then called at the fill, first in, first out. The adapter's
 * fill-time admission counted the sibling's lot as held and refused the
 * second entry (pf-probe-thula-live-explicit-add-free-funds, six lanes).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/coqueued_open_margin, lab tv exports on BINANCE:ETHUSDT.P
 * 15) through the Pine adapter under the configuration the generated
 * constructor declares for it, over the corpus 15m bars embedded in bars.inc,
 * and requires every trade the tape closes inside those bars to be the
 * engine's: entry and exit time, side, price and quantity. It also reads the
 * rule off TradingView's own rows.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

#ifndef PINEFORGE_COQ_FIXTURE_DIR
#error "PINEFORGE_COQ_FIXTURE_DIR must name tests/fixtures/coqueued_open_margin"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coqueued_open_margin/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The bar that places the first cell's orders (2025-04-08 00:00 UTC); the
// harness trades from here, as run_strategy.py does for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1744070400000LL;

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

// 2025-04-<day> <hour>:<minute> UTC, the probes' timestamp("UTC", ...) cells.
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Tape {
    std::vector<Row> trades;           // closed inside the replayed bars, in entry order
    std::vector<std::string> entries;  // each trade's entry signal
    std::vector<std::string> exits;    // each trade's exit signal
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_COQ_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
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

// The probe, as its generated TU lowers it (fixtures/.../strategy.pine). The
// fixed-default variant omits the qty of the A cell's two entries.
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(bool fixed_default, const source::PineStrategyConfig& config)
        : fixed_default_(fixed_default) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double a_qty = fixed_default_ ? kNaN : 34.0;
        // A: flat, two longs of 34 (together over the equity).
        if (t == at(8, 0, 0)) {
            strategy_entry("A1", true, kNaN, kNaN, a_qty, "A1 long 34", "", 0, -1);
            strategy_entry("A2", true, kNaN, kNaN, a_qty, "A2 long 34", "", 0, -1);
        }
        // B: flat, two shorts of 33 (together over the equity).
        if (t == at(8, 4, 0)) {
            strategy_entry("B1", false, kNaN, kNaN, 33, "B1 short 33", "", 0, -1);
            strategy_entry("B2", false, kNaN, kNaN, 33, "B2 short 33", "", 0, -1);
        }
        // C: flat, two longs of 30 (together inside the equity): control.
        if (t == at(8, 8, 0)) {
            strategy_entry("C1", true, kNaN, kNaN, 30, "C1 long 30", "", 0, -1);
            strategy_entry("C2", true, kNaN, kNaN, 30, "C2 long 30", "", 0, -1);
        }
        // D: a held long of 20, then two adds of 28 on one bar.
        if (t == at(9, 0, 0)) strategy_entry("D0", true, kNaN, kNaN, 20, "D0 seed 20", "", 0, -1);
        if (t == at(9, 0, 15)) {
            strategy_entry("D1", true, kNaN, kNaN, 28, "D1 add 28", "", 0, -1);
            strategy_entry("D2", true, kNaN, kNaN, 28, "D2 add 28", "", 0, -1);
        }
        // E: a held long of 20, then one add of 56 (over the equity): control.
        if (t == at(9, 4, 0)) strategy_entry("E0", true, kNaN, kNaN, 20, "E0 seed 20", "", 0, -1);
        if (t == at(9, 4, 15)) strategy_entry("E1", true, kNaN, kNaN, 56, "E1 add 56", "", 0, -1);
        if (t == at(8, 1, 0) || t == at(8, 5, 0) || t == at(8, 9, 0) || t == at(9, 1, 0)
            || t == at(9, 5, 0)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

private:
    bool fixed_default_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(bool fixed_default, const source::PineStrategyConfig& config, std::int64_t end_ms) {
    ProbeHost host(fixed_default, config);
    host.set_trade_start_time(kTradeStartMs);
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

// What the generated constructor declares: v6's 100000 capital, margins of
// 100, pyramiding 4, and the probe's default size.
source::PineStrategyConfig config(QtyType type, double value) {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(type);
    c.default_qty_value = value;
    c.pyramiding = 4;
    c.commission_value = 0.0;
    c.slippage = 0;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

struct Case {
    const char* tape;
    bool fixed_default;
    const char* declares;            // the probe's strategy() sizing arguments
    source::PineStrategyConfig lane; // what its generated constructor declares
    std::size_t closed;              // tape trades closed inside the bars
};

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;

    const Case cases[] = {
        {"w10-coq-flat-pair", false, "nothing (v6: percent_of_equity, 100); explicit qty",
         config(QtyType::PERCENT_OF_EQUITY, 100.0), 13},
        {"w10-coq-flat-pair-fixed", true, "fixed, 34; the A cell omits qty",
         config(QtyType::FIXED, 34.0), 13},
    };

    std::map<std::string, Tape> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const Tape tape = tape_trades(c.tape, end_ms);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.closed);

        const Run lane = run(c.fixed_default, c.lane, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape.trades);
        if (lane.trades != tape.trades) {
            show("tape", tape.trades);
            show("engine", lane.trades);
        }
    }

    // What the tape proves, read off TradingView's own rows.
    std::printf("-- the rule, on the tape\n");
    {
        const Tape& tape = tapes["w10-coq-flat-pair"];
        // The two tapes are one file: a default fixed size is admitted alike.
        CHECK(tape.trades == tapes["w10-coq-flat-pair-fixed"].trades);
        CHECK(tape.trades.size() == 13);
        if (tape.trades.size() == 13) {
            // A and B: both entries of the pair fill at the next open, and the
            // first one's excess is called at that fill, at its entry price.
            for (const std::size_t first : {std::size_t{0}, std::size_t{3}}) {
                CHECK(tape.exits[first] == "Margin call");
                CHECK(std::get<0>(tape.trades[first]) == std::get<4>(tape.trades[first]));
                CHECK(std::get<2>(tape.trades[first]) == std::get<5>(tape.trades[first]));
                CHECK(tape.entries[first] == tape.entries[first + 1]);
                CHECK(tape.exits[first + 1] == "cleanup");
                CHECK(tape.exits[first + 2] == "cleanup");
                CHECK(std::get<0>(tape.trades[first + 2]) == std::get<0>(tape.trades[first]));
                // The first entry's two rows are its whole quantity, the
                // second entry's own (each pair asks for one size twice).
                CHECK(std::get<3>(tape.trades[first]) + std::get<3>(tape.trades[first + 1])
                      == std::get<3>(tape.trades[first + 2]));
            }
            CHECK(std::get<3>(tape.trades[2]) == lots(34.0));
            CHECK(std::get<3>(tape.trades[5]) == lots(33.0));
            // C: a pair that fits fills whole, with no call.
            CHECK(tape.exits[6] == "cleanup" && tape.exits[7] == "cleanup");
            CHECK(std::get<3>(tape.trades[6]) == lots(30.0));
            CHECK(std::get<3>(tape.trades[7]) == lots(30.0));
            // D: two adds to a held long both fill; the call takes the held
            // seed whole, then the first add, first in first out.
            CHECK(tape.entries[8] == "D0 seed 20" && tape.exits[8] == "Margin call");
            CHECK(std::get<3>(tape.trades[8]) == lots(20.0));
            CHECK(tape.entries[9] == "D1 add 28" && tape.exits[9] == "Margin call");
            CHECK(tape.entries[10] == "D1 add 28" && tape.exits[10] == "cleanup");
            CHECK(std::get<3>(tape.trades[9]) + std::get<3>(tape.trades[10]) == lots(28.0));
            CHECK(tape.entries[11] == "D2 add 28");
            CHECK(std::get<3>(tape.trades[11]) == lots(28.0));
            // E: a single add over the equity is refused: only the seed trades.
            CHECK(tape.entries[12] == "E0 seed 20");
        }
    }

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
