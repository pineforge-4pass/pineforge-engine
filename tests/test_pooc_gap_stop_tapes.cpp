/*
 * test_pooc_gap_stop_tapes.cpp — lane W10-DIAG-UNKNOWN, rule GAPSTOP.
 *
 * Under process_orders_on_close, a strategy.entry STOP sized by the default
 * percent_of_equity (at most 100) is sized at its level. When the bar after
 * its placement OPENS through the level, TradingView fills it at that open if
 * the equity it was sized from still pays for it there, and otherwise refuses
 * it: the order is gone, and a later cross of its level does not fill it.
 * The adapter admitted the gapped fill and let a one-unit margin call take the
 * excess (vasudevshenoy-manoj-betrayed-me on NASDAQ:AAPL 15, 2025-04-22 14:00,
 * where TradingView's script then cancelled the refused order's id).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/pooc_gap_stop, lab tv exports on NASDAQ:AAPL 15) through
 * the Pine adapter under the configuration the generated constructor
 * declares for it, over the lane's 15m bars embedded in bars.inc, and
 * requires every trade the tape closes inside those bars to be the engine's:
 * entry and exit time, side, price and quantity. It also reads the rule off
 * TradingView's own rows.
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

#ifndef PINEFORGE_GAPSTOP_FIXTURE_DIR
#error "PINEFORGE_GAPSTOP_FIXTURE_DIR must name tests/fixtures/pooc_gap_stop"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/pooc_gap_stop/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// The lane's NASDAQ:AAPL price tick and quantity step.
constexpr double kTick = 0.01;
constexpr double kLot = 1.0;
// The bar that places the first cell's order (2025-04-11 15:45 UTC); the
// harness trades from here, as run_strategy.py does for a tape.
constexpr std::int64_t kTradeStartMs = 1744386300000LL;

// One trade as both sides report it, prices in ticks:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kAapl15) {
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

// 2025-<month>-<day> <hour>:<minute> UTC, the probes' timestamp("UTC", ...) cells.
std::int64_t at(unsigned month, unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, month, day) * 24 + hour) * 60 + minute) * 60'000;
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
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_GAPSTOP_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal;
    std::map<int, bool> closed;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price USD, Size (qty), ...
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
            closed[number] = true;
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        if (closed[number] && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            out.entries.push_back(entry_signal[number]);
        }
    }
    return out;
}

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine):
// one script in three configurations.
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const source::PineStrategyConfig& config) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_mintick(kTick);
        set_syminfo_metadata("qty_step", kLot);
        set_syminfo_metadata("margin_long", 100.0);
        set_syminfo_metadata("margin_short", 100.0);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        // G1: 04-11 15:45 high 195.66; 16:00 opens 195.67.
        if (t == at(4, 11, 15, 45)) stop("G1", 195.66, "G1 gap stop");
        // K1: 04-15 17:00 high 202.41; 17:15 opens 202.42; cancelled at 17:15 if flat.
        if (t == at(4, 15, 17, 0)) stop("K1", 202.41, "K1 gap stop");
        if (t == at(4, 15, 17, 15) && signed_position_size() == 0.0) strategy_cancel("K1");
        // K2: 04-22 13:45 high 197.855; 14:00 opens 197.87; cancelled at 14:00 if flat.
        if (t == at(4, 22, 13, 45)) stop("K2", 197.855, "K2 gap stop");
        if (t == at(4, 22, 14, 0) && signed_position_size() == 0.0) strategy_cancel("K2");
        // G2: 04-29 15:30 high 211.17; 15:45 opens 211.18.
        if (t == at(4, 29, 15, 30)) stop("G2", 211.17, "G2 gap stop");
        // X: 05-01 14:00 high 211.59; 14:15 crosses it intrabar; cancelled at
        // 14:15 if flat.
        if (t == at(5, 1, 14, 0)) stop("X", 211.59, "X crossed stop");
        if (t == at(5, 1, 14, 15) && signed_position_size() == 0.0) strategy_cancel("X");
        if (t == at(4, 11, 18, 0) || t == at(4, 15, 19, 0) || t == at(4, 22, 16, 0)
            || t == at(4, 29, 19, 0) || t == at(5, 1, 17, 0)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

private:
    void stop(const char* id, double level, const char* comment) {
        strategy_entry(id, true, kNaN, level, kNaN, comment, "", 0, -1);
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
        std::printf("    %-8s %lld %s @%lld ticks q=%lld -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// What each probe's generated constructor declares.
source::PineStrategyConfig config(bool on_close, QtyType type, double value) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = on_close;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(type);
    c.default_qty_value = value;
    c.pyramiding = 0;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

struct Case {
    const char* tape;
    const char* declares;
    source::PineStrategyConfig lane;
    std::size_t closed;
};

}  // namespace

int main() {
    const std::int64_t end_ms = kAapl15[sizeof(kAapl15) / sizeof(kAapl15[0]) - 1].ts;
    const Case cases[] = {
        {"w10-pooc-gap-stop", "process_orders_on_close, fixed 1",
         config(true, QtyType::FIXED, 1.0), 5},
        {"w10-pooc-gap-stop-pct", "process_orders_on_close, v6 default percent_of_equity 100",
         config(true, QtyType::PERCENT_OF_EQUITY, 100.0), 4},
        {"w10-gap-stop-pct-noclose", "v6 default percent_of_equity 100",
         config(false, QtyType::PERCENT_OF_EQUITY, 100.0), 5},
    };

    std::map<std::string, Tape> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const Tape tape = tape_trades(c.tape, end_ms);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.closed);
        const Run lane = run(c.lane, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape.trades);
        if (lane.trades != tape.trades) {
            show("tape", tape.trades);
            show("engine", lane.trades);
        }
    }

    // What the tapes prove, read off TradingView's own rows.
    std::printf("-- the rule, on the tapes\n");
    {
        // Fixed 1: every gapped stop fills at the open that gapped through it.
        const Tape& one = tapes["w10-pooc-gap-stop"];
        CHECK(one.trades.size() == 5);
        // Default size under process_orders_on_close: G2 (486 sized at 211.17
        // cost 102,633.48 at the 211.18 open against 102,630.93) is refused
        // and never fills on the later crosses of its level; the others fill
        // whole at their opens.
        const Tape& pct = tapes["w10-pooc-gap-stop-pct"];
        CHECK(pct.trades.size() == 4);
        bool g2 = false;
        for (const auto& id : pct.entries) g2 = g2 || id == "G2 gap stop";
        CHECK(!g2);
        if (pct.trades.size() == 4) {
            CHECK(pct.entries[2] == "K2 gap stop");
            CHECK(std::get<0>(pct.trades[2]) == at(4, 22, 14, 0));
            CHECK(std::get<2>(pct.trades[2]) == ticks(197.87));
        }
        // Without it the same size is 485 there, which the open pays for.
        const Tape& noclose = tapes["w10-gap-stop-pct-noclose"];
        CHECK(noclose.trades.size() == 5);
        if (noclose.trades.size() == 5) {
            CHECK(noclose.entries[3] == "G2 gap stop");
            CHECK(std::get<3>(noclose.trades[3]) == 485);
        }
    }

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
