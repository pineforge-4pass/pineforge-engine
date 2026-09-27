/*
 * test_intraday_cap_tv_tapes.cpp — lane W10-DIAG-UNKNOWN, TradingView's
 * strategy.risk.max_intraday_filled_orders count.
 *
 * TradingView charges the cap with the orders that fill. A strategy.close
 * that fills is one of them, and an opposite entry the script placed AFTER
 * that close on the same bar is another: from a long, close then short then
 * a third fill trips the cap at the short. Only when the opposite entry was
 * placed BEFORE the close has the entry's reversal already closed the
 * position, so the close is void and the bar is one fill. The Pine cap's
 * declared "count full closes" candidate (intraday_cap_count_pooc_full_close
 * _fills) let the co-queued opposite entry inherit the close's slot whatever
 * the order the script placed them in, so the close-then-entry bar was one
 * fill and the cap tripped a bar late (rule CAP-ORDER).
 *
 * The three candidate switches of the Pine cap (a held-direction entry that
 * pyramiding makes a no-op is not a fill; the cap's close for a same-bar
 * process_orders_on_close fill is taken at the next open; a full close is a
 * fill) are TradingView's count, so a script's own
 * strategy.risk.max_intraday_filled_orders statement now turns on every
 * switch its host did not declare (rule CAP-ON). The base engine left them
 * off unless declared, and counted the no-op entries of day A as fills.
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/intraday_cap_tv, lab tv exports on BINANCE:ETHUSDT.P 15)
 * through the Pine adapter under the configuration the generated
 * constructor declares for it, over the corpus 15m bars embedded in bars.inc,
 * and requires every trade the tape closes inside those bars to be the
 * engine's: entry and exit time, side, price and quantity. It also reads the
 * count off TradingView's own rows.
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

#ifndef PINEFORGE_CAP_TV_FIXTURE_DIR
#error "PINEFORGE_CAP_TV_FIXTURE_DIR must name tests/fixtures/intraday_cap_tv"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/intraday_cap_tv/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The bar before the first cell (2025-04-08 00:00 UTC, whose close fills the
// first entry under process_orders_on_close); the harness trades from here,
// as run_strategy.py does for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1744069500000LL;  // 2025-04-07 23:45 UTC

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
    std::ifstream in(std::string(PINEFORGE_CAP_TV_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
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

enum class Probe { Abc, Rev };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine),
// the strategy.close call sites carrying the tokens the generated code gives
// them.
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, bool declare_switches, const source::PineStrategyConfig& config)
        : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
        if (declare_switches) {
            set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
            set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
        }
    }

    void on_source_bar(const Bar&) override {
        set_pine_risk_max_intraday_filled_orders(3);
        const std::int64_t t = current_bar_.timestamp;
        if (probe_ == Probe::Abc) abc(t);
        else rev(t);
    }

private:
    void entry(const char* id, bool is_long, const char* comment) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, comment);
    }
    void close(const char* id, const char* comment, std::uint64_t token) {
        strategy_close(id, comment, kNaN, kNaN, false, token);
    }
    void cleanup() { strategy_close("", "cleanup", kNaN, kNaN, false); }

    // A: a long, three same-direction entry calls while it is held, a close,
    // then a new long. B: a long, a close, then a short that is the day's
    // third fill. C: a long, then a close and an opposite entry on one bar,
    // then a new entry.
    void abc(std::int64_t t) {
        if (t == at(8, 0, 0)) entry("AL", true, "A long");
        if (t == at(8, 0, 15) || t == at(8, 0, 30) || t == at(8, 0, 45))
            entry("AL", true, "A long again");
        if (t == at(8, 1, 0)) close("AL", "A close", 94489280531ULL);
        if (t == at(8, 1, 15)) entry("AL2", true, "A second long");
        if (t == at(9, 0, 0)) entry("BL", true, "B long");
        if (t == at(9, 0, 30)) close("BL", "B close", 124554051603ULL);
        if (t == at(9, 1, 0)) entry("BS", false, "B short");
        if (t == at(10, 0, 0)) entry("CL", true, "C long");
        if (t == at(10, 1, 0)) {
            close("CL", "C close", 154618822675ULL);
            entry("CS", false, "C short");
        }
        if (t == at(10, 2, 0)) entry("CL2", true, "C long again");
        if (t == at(8, 6, 0) || t == at(9, 6, 0) || t == at(10, 6, 0)) cleanup();
    }

    // D: a long; then an opposite entry FOLLOWED by a close of the long on one
    // bar; then a new long. E: the opposite entry alone. F: a close FOLLOWED
    // by an opposite entry on one bar.
    void rev(std::int64_t t) {
        if (t == at(11, 0, 0)) entry("DL", true, "D long");
        if (t == at(11, 1, 0)) {
            entry("DS", false, "D short");
            close("DL", "D close", 90194313235ULL);
        }
        if (t == at(11, 2, 0)) entry("DL2", true, "D long again");
        if (t == at(12, 0, 0)) entry("EL", true, "E long");
        if (t == at(12, 1, 0)) entry("ES", false, "E short");
        if (t == at(12, 2, 0)) entry("EL2", true, "E long again");
        if (t == at(13, 0, 0)) entry("FL", true, "F long");
        if (t == at(13, 1, 0)) {
            close("FL", "F close", 150323855379ULL);
            entry("FS", false, "F short");
        }
        if (t == at(13, 2, 0)) entry("FL2", true, "F long again");
        if (t == at(11, 6, 0) || t == at(12, 6, 0) || t == at(13, 6, 0)) cleanup();
    }

    Probe probe_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, bool declare_switches, std::int64_t end_ms) {
    source::PineStrategyConfig config{};
    config.process_orders_on_close = true;
    config.initial_capital = 100000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = 0;
    config.commission_value = 0.0;
    config.slippage = 0;
    ProbeHost host(probe, declare_switches, config);
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

constexpr const char* kCapClose = "Close Position (Max number of filled orders in one day)";

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    std::map<std::string, Tape> tapes;
    tapes["w10-cap-abc"] = tape_trades("w10-cap-abc", end_ms);
    tapes["w10-cap-rev"] = tape_trades("w10-cap-rev", end_ms);
    CHECK(tapes["w10-cap-abc"].trades.size() == 6);
    CHECK(tapes["w10-cap-rev"].trades.size() == 8);

    // The run declares the Pine cap's three candidate switches, as a host
    // that opts into them does.
    for (const auto& [name, probe] : {std::pair<const char*, Probe>{"w10-cap-abc", Probe::Abc},
                                      std::pair<const char*, Probe>{"w10-cap-rev", Probe::Rev}}) {
        std::printf("-- %s, switches declared\n", name);
        const Run lane = run(probe, true, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tapes[name].trades);
        if (lane.trades != tapes[name].trades) {
            show("tape", tapes[name].trades);
            show("engine", lane.trades);
        }
    }

    // The run declares nothing, as a generated strategy's host does: the
    // script's own strategy.risk.max_intraday_filled_orders statement counts
    // as TradingView does (rule CAP-ON).
    for (const auto& [name, probe] : {std::pair<const char*, Probe>{"w10-cap-abc", Probe::Abc},
                                      std::pair<const char*, Probe>{"w10-cap-rev", Probe::Rev}}) {
        std::printf("-- %s, nothing declared\n", name);
        const Run lane = run(probe, false, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tapes[name].trades);
        if (lane.trades != tapes[name].trades) {
            show("tape", tapes[name].trades);
            show("engine", lane.trades);
        }
    }

    // What the tapes prove, read off TradingView's own rows.
    std::printf("-- the count, on the tapes\n");
    {
        const Tape& abc = tapes["w10-cap-abc"];
        if (abc.trades.size() == 6) {
            // A: three held-side entry calls are not fills: the close is the
            // second fill, the new long the third, and the cap closes it at
            // the next bar's open.
            CHECK(abc.exits[0] == "A close");
            CHECK(abc.entries[1] == "A second long" && abc.exits[1] == kCapClose);
            CHECK(std::get<4>(abc.trades[1]) == std::get<0>(abc.trades[1]) + 900'000);
            // B: a close then a short: the short is the third fill.
            CHECK(abc.exits[2] == "B close");
            CHECK(abc.entries[3] == "B short" && abc.exits[3] == kCapClose);
            // C: a close and an opposite entry placed after it on one bar are
            // two fills: the short is the third, and the next entry never fills.
            CHECK(abc.exits[4] == "C close");
            CHECK(abc.entries[5] == "C short" && abc.exits[5] == kCapClose);
            CHECK(std::get<0>(abc.trades[5]) == std::get<4>(abc.trades[4]));
        }
        const Tape& rev = tapes["w10-cap-rev"];
        if (rev.trades.size() == 8) {
            // D and E: an opposite entry placed before the close (or alone)
            // reverses the long; the close is void, and the third fill is the
            // next day's-bar long, closed by the cap a bar later.
            for (const std::size_t first : {std::size_t{0}, std::size_t{3}}) {
                CHECK(rev.exits[first] == rev.entries[first + 1]);
                CHECK(rev.exits[first + 1] == rev.entries[first + 2]);
                CHECK(rev.exits[first + 2] == kCapClose);
            }
            // F: close then opposite entry: the short is the third fill.
            CHECK(rev.exits[6] == "F close");
            CHECK(rev.entries[7] == "F short" && rev.exits[7] == kCapClose);
        }
    }

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
