/*
 * test_tvdef_drops_tapes.cpp — lane TVDEF-DROPS.
 *
 * Pine v6's default order size is 100% of equity (lane TV-DEFAULTS), so many
 * scripts that used to trade one contract now size every entry from the
 * account. Pine adapter rules broke under it, each lost when R4 slice C
 * lowered ab9714be's source broker onto the native kernel:
 *
 *   R1  A default percent_of_equity (<= 100) pure STOP entry that reverses a
 *       held position, but whose new side the equity cannot fund at placement,
 *       keeps its closing leg: TradingView closes the long at the stop and
 *       opens nothing (ab9714be pine_strategy_commands.cpp:408-419). The
 *       lowering dropped the whole order, so the long was held to the end.
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/tvdef_drops, lab tv exports on BINANCE:ETHUSDT.P 15) through
 * the Pine adapter under the configuration the generated constructor declares
 * for it, over the corpus 15m bars embedded in bars.inc, and requires every
 * trade the tape closes inside those bars to be the engine's: entry and exit
 * time, side, price and quantity. The rule tapes the lowering missed, and
 * the declared-size control of each rule that it already booked, are
 * replayed alike.
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

#ifndef PINEFORGE_TVDEF_DROPS_FIXTURE_DIR
#error "PINEFORGE_TVDEF_DROPS_FIXTURE_DIR must name tests/fixtures/tvdef_drops"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/tvdef_drops/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The bar before every tape's first entry (2025-04-08 00:15 UTC), where its
// order was placed; the harness trades from here, as run_strategy.py does
// for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1744070400000LL;  // 2025-04-08 00:00 UTC

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
    std::vector<std::string> signals;  // each trade's exit signal
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_TVDEF_DROPS_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
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
            out.signals.push_back(signal->second);
        }
    }
    return out;
}

enum class Probe { R1 };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config) : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::R1: stop_reversal(t); break;
        }
    }

private:
    void cleanup() {
        strategy_cancel_all();
        strategy_close("", "cleanup", kNaN, kNaN, false);
    }

    void stop_reversal(std::int64_t t) {
        if (t == at(8, 0, 0)) strategy_entry("A-L", true, kNaN, kNaN, kNaN, "A seed long");
        if (t == at(8, 0, 15))
            strategy_entry("A-S", false, kNaN, 1548.0, kNaN, "A reverse short stop", "", 0, -1);
        if (t == at(10, 0, 0)) strategy_entry("C-L", true, kNaN, kNaN, kNaN, "C seed long");
        if (t == at(10, 0, 15))
            strategy_entry("C-S", false, kNaN, 1640.0, kNaN, "C reverse short stop", "", 0, -1);
        if (t == at(8, 2, 0) || t == at(10, 2, 0)) cleanup();
    }

    Probe probe_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, const source::PineStrategyConfig& config, std::int64_t end_ms) {
    ProbeHost host(probe, config);
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

source::PineStrategyConfig config(QtyType type, double value) {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;  // every probe omits it: v6's default
    c.default_qty_type = static_cast<int>(type);
    c.default_qty_value = value;
    return c;
}

struct Case {
    const char* tape;
    Probe probe;
    const char* declares;            // the probe's strategy() sizing arguments
    source::PineStrategyConfig lane; // what its generated constructor declares
    std::size_t closed;              // tape trades closed inside the bars
};

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    constexpr QtyType kPercent = QtyType::PERCENT_OF_EQUITY;

    const Case cases[] = {
        {"tdd-r1-stop-reversal-close-only", Probe::R1, "nothing (v6: percent_of_equity, 100)",
         config(kPercent, 100.0), 2},
        {"tdd-r1-stop-reversal-half", Probe::R1, "percent_of_equity, 50",
         config(kPercent, 50.0), 4},
    };

    std::map<std::string, Tape> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const Tape tape = tape_trades(c.tape, end_ms);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.closed);

        const Run lane = run(c.probe, c.lane, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape.trades);
        if (lane.trades != tape.trades) {
            show("tape", tape.trades);
            show("engine", lane.trades);
        }
    }

    // What each rule tape proves, read off TradingView's own rows.
    std::printf("-- the rules, on the tapes\n");
    {
        // R1: at 100 % every sell-stop reversal only closes the long it
        // reverses; at 50 % the same stops fund and open the short.
        const Tape& all = tapes["tdd-r1-stop-reversal-close-only"];
        CHECK(all.trades.size() == 2);
        for (std::size_t i = 0; i < all.trades.size(); ++i) {
            CHECK(std::get<1>(all.trades[i]));
            CHECK(all.signals[i].find("reverse short stop") != std::string::npos);
        }
        const Tape& half = tapes["tdd-r1-stop-reversal-half"];
        CHECK(half.trades.size() == 4);
        if (half.trades.size() == 4) {
            CHECK(!std::get<1>(half.trades[1]));
            CHECK(!std::get<1>(half.trades[3]));
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
