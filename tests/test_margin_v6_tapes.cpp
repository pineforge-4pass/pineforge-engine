/*
 * test_margin_v6_tapes.cpp — lane W5-ENG-MARGIN-V6.
 *
 * At Pine v6's defaults (100% of equity per order, margin 100 both ways) a
 * strategy's margin calls and entry admission decide most of its trades. The
 * Pine adapter rules below were missing or wrong under those defaults; each
 * is read off TradingView's own tapes of synthetic probes written for this
 * lane:
 *
 *   MR  A from_entry="" exit is the held position's: a same-bar declined
 *       reversal holds it, and a margin call revives it, as it holds and
 *       revives a named bracket of that side.
 *   M1  A default percent_of_equity (<= 100) MARKET add to a held position is
 *       judged at placement against the held units plus its own: TradingView
 *       drops an add whose combined margin exceeds the equity, even where an
 *       exit empties the position at the open the add would fill at.
 *   C1  A commissioned or slipped short carried under process_orders_on_close
 *       is called at the bar's high before the script runs at the close: the
 *       script reads the called book, and its close fills meet what the call
 *       left.
 *   M2  A short process_orders_on_close fills at a signal close is margined
 *       at that close: TradingView sizes the call there and executes it at
 *       the next open, and checks again at its slipped fill.
 *   QP  A qty_percent exit's basis is its entry's quantity: the units margin
 *       calls took from the position count.
 *   C2  The close fill that opens or reverses into a commissioned
 *       process_orders_on_close position is not called there: TradingView
 *       calls it at the next open.
 *   MK  A slipped MARKET short's opening call is sized at the print its fill
 *       slipped from, and executed there with the call's own slippage.
 *
 * Each row replays one lab tv tape (tests/fixtures/margin_v6) through the
 * Pine adapter under the configuration the generated constructor declares for
 * its probe, over the corpus 15m bars in bars.inc (BINANCE:ETHUSDT.P) or the
 * NYSE:F 15m bars of the V19D-P1 fixture, and requires every trade the tape
 * closes inside those bars to be the engine's: entry and exit time, side,
 * price in ticks and quantity in lots. On the lane's base every rule row
 * fails here, and every control row beside it passes.
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

#include "test_adapter_margin_revival_cancel_data.hpp"

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

#ifndef PINEFORGE_MARGIN_V6_FIXTURE_DIR
#error "PINEFORGE_MARGIN_V6_FIXTURE_DIR must name tests/fixtures/margin_v6"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/margin_v6/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;       // both charts' price tick
constexpr double kEthLot = 0.0001;   // TradingView's BINANCE:ETHUSDT.P quantity step
constexpr double kShareLot = 1.0;    // NYSE:F

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty, double lot) { return std::llround(qty / lot); }
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

// 2025-<month>-<day> <hour>:<minute> UTC, the probes' timestamp("UTC", ...).
std::int64_t utc(unsigned month, unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, month, day) * 24 + hour) * 60 + minute) * 60'000;
}
std::int64_t at(unsigned day, int hour, int minute) { return utc(4, day, hour, minute); }
// timestamp("America/New_York", ...): EDT (UTC-4) throughout the NYSE:F bars.
std::int64_t new_york(unsigned month, unsigned day, int hour, int minute) {
    return utc(month, day, hour + 4, minute);
}
constexpr std::int64_t kMinute = 60'000;

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms, double lot) {
    std::ifstream in(std::string(PINEFORGE_MARGIN_V6_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
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
        CHECK(on_grid(price, kTick));
        if (cell[1].rfind("Entry", 0) == 0) {
            const double qty = std::stod(cell[5]);
            CHECK(on_grid(qty, lot));
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = lots(qty, lot);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
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

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
enum class Probe {
    MrGlobal, MrNamed,             // w5-mr-global / -named
    MrOnceGlobal, MrOnceNamed,     // w5-mr-once-global / -once-named
    RvGlobalHeld, RvNamed,         // w5-rv-global-held / -named-control (NYSE:F)
    AddExit,                       // w5-m1-addexit-p100 / -p60
    AddClose,                      // w5-m1-addclose
    PoocShortComm,                 // w5-m2-pooc-short-comm
    PoocShortOpen, PoocShortPlain, // w5-m2-pooc-short-open / -plain
    SeenSize,                      // w5-c1-seen-size
    CarriedClose, CarriedNone,     // w5-m2-pooc-carried-close / -none
    QpReissue, QpOnce,             // w5-qp-pooc-reissue / -once
    RevToLong, RevToShort,         // w5-c2-pooc-rev-long / -rev-short
    LongOpen,                      // w5-c2-pooc-long-open
    FlatExplicitLong, FlatExplicitShort, // w5-c2-pooc-flat-explicit-long / -short
    CarriedReverse,                // w5-m2-pooc-carried-reverse
    QpMarketReissue, QpMarketOnce, // w5-qp-mkt-reissue / -once
    MarketRevToShort,              // w5-mk-short-rev
};

// The fifteen signal closes of w5-m2-pooc-short-open, shared by its siblings.
const std::int64_t kCells[] = {
    at(2, 0, 0), at(2, 3, 0), at(2, 6, 0), at(2, 9, 0), at(2, 18, 0),
    at(3, 21, 0), at(4, 12, 0), at(5, 3, 0), at(5, 9, 0), at(5, 12, 0),
    at(5, 15, 0), at(5, 21, 0), at(6, 9, 0), at(6, 12, 0), at(6, 15, 0),
};

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config, double lot)
        : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", lot);
        if (probe == Probe::RvGlobalHeld || probe == Probe::RvNamed) {
            set_syminfo_timezone("America/New_York");
            set_syminfo_session("0930-1600");
        }
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        // strategy.position_avg_price: na while flat.
        const double avg = signed_position_size() == 0.0 ? kNaN : position_entry_price_;
        switch (probe_) {
        case Probe::MrGlobal: declined_reversal_exit(t, avg, ""); break;
        case Probe::MrNamed: declined_reversal_exit(t, avg, "S"); break;
        case Probe::MrOnceGlobal: declined_reversal_exit_once(t, avg, ""); break;
        case Probe::MrOnceNamed: declined_reversal_exit_once(t, avg, "S"); break;
        case Probe::RvGlobalHeld: revival(t, false); break;
        case Probe::RvNamed: revival(t, true); break;
        case Probe::AddExit: add_and_exit(t, avg); break;
        case Probe::AddClose: add_and_close(t); break;
        case Probe::PoocShortComm: pooc_shorts(t, false); break;
        case Probe::PoocShortOpen: cells(t, 0, nullptr, false, "S", "short", 30); break;
        case Probe::PoocShortPlain: pooc_shorts(t, true); break;
        case Probe::SeenSize: carried(t, 1); break;
        case Probe::CarriedClose: carried(t, 2); break;
        case Probe::CarriedNone: carried(t, 4); break;
        case Probe::QpReissue: qty_percent_exits(t, at(15, 19, 15), false); break;
        case Probe::QpOnce: qty_percent_exits(t, at(15, 19, 15), true); break;
        case Probe::RevToLong: cells(t, -15, "S", false, "L", "reverse to long", 30, true); break;
        case Probe::RevToShort: cells(t, -15, "L", true, "S", "reverse to short", 30); break;
        case Probe::LongOpen: cells(t, 0, nullptr, false, "L", "long", 30, true); break;
        case Probe::FlatExplicitLong: flat_explicit(t, true); break;
        case Probe::FlatExplicitShort: flat_explicit(t, false); break;
        case Probe::CarriedReverse: carried(t, 3); break;
        case Probe::QpMarketReissue: qty_percent_exits(t, at(15, 19, 0), false); break;
        case Probe::QpMarketOnce: qty_percent_exits(t, at(15, 19, 0), true); break;
        case Probe::MarketRevToShort: cells(t, -15, "L", true, "S", "short", 45); break;
        }
    }

private:
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }
    void cleanup() {
        strategy_cancel_all();
        close_all("cleanup");
    }

    void declined_reversal_exit(std::int64_t t, double avg, const char* from_entry) {
        for (const std::int64_t cell : {at(2, 13, 15), at(3, 0, 15), at(7, 0, 0),
                                        at(7, 17, 0), at(8, 19, 30), at(28, 10, 15)}) {
            if (t == cell) strategy_entry("S", false, kNaN, kNaN, kNaN, "short");
            if (t == cell + 15 * kMinute) strategy_entry("L", true, kNaN, kNaN, kNaN, "reverse long");
            if (t == cell + 120 * kMinute) cleanup();
        }
        if (signed_position_size() < 0.0)
            strategy_exit("X", from_entry, avg - 30.0, avg + 3.0, kNaN, kNaN, kNaN, 100.0, "X",
                          kNaN, "", kNaN, kNaN);
    }

    // As declined_reversal_exit, the exit placed once, with the reversal.
    void declined_reversal_exit_once(std::int64_t t, double avg, const char* from_entry) {
        for (const std::int64_t cell : {at(2, 13, 15), at(3, 0, 15), at(7, 17, 0),
                                        at(28, 10, 15)}) {
            if (t == cell) strategy_entry("S", false, kNaN, kNaN, kNaN, "short");
            if (t == cell + 15 * kMinute) {
                strategy_entry("L", true, kNaN, kNaN, kNaN, "reverse long");
                strategy_exit("X", from_entry, avg - 30.0, avg + 3.0, kNaN, kNaN, kNaN, 100.0,
                              "X", kNaN, "", kNaN, kNaN);
            }
            if (t == cell + 120 * kMinute) cleanup();
        }
    }

    // The V19D-P1 shape: a 940-share short with stop exit X, declined by the
    // 05-01 open's long + close pair, called at the 05-02 09:30 high.
    void revival(std::int64_t t, bool named) {
        if (t == new_york(4, 29, 15, 45)) {
            strategy_entry("S", false, kNaN, kNaN, 940.0);
            if (named) strategy_exit("X", "S", kNaN, 10.25);
        }
        if (!named && t == new_york(4, 30, 10, 0)) strategy_exit("X", "", kNaN, 10.25);
        if (t == new_york(4, 30, 15, 45)) {
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
    }

    void add_and_exit(std::int64_t t, double avg) {
        // A -- short, add declared before the exit.
        if (t == at(4, 7, 30)) strategy_entry("AS", false, kNaN, kNaN, kNaN, "A seed short");
        if (t == at(4, 7, 45)) {
            strategy_entry("AS", false, kNaN, kNaN, kNaN, "A add short");
            strategy_exit("AX", "AS", avg - 0.05, avg + 0.04, kNaN, kNaN, kNaN, 100.0, "A exit",
                          kNaN, "", kNaN, kNaN);
        }
        // B -- short, exit declared before the add.
        if (t == at(9, 7, 30)) strategy_entry("BS", false, kNaN, kNaN, kNaN, "B seed short");
        if (t == at(9, 7, 45)) {
            strategy_exit("BX", "BS", avg - 0.05, avg + 0.04, kNaN, kNaN, kNaN, 100.0, "B exit",
                          kNaN, "", kNaN, kNaN);
            strategy_entry("BS", false, kNaN, kNaN, kNaN, "B add short");
        }
        // C -- long, add declared before the exit.
        if (t == at(15, 7, 30)) strategy_entry("CL", true, kNaN, kNaN, kNaN, "C seed long");
        if (t == at(15, 7, 45)) {
            strategy_entry("CL", true, kNaN, kNaN, kNaN, "C add long");
            strategy_exit("CX", "CL", avg + 0.05, avg - 0.04, kNaN, kNaN, kNaN, 100.0, "C exit",
                          kNaN, "", kNaN, kNaN);
        }
        // D and E lie beyond the replayed bars.
        if (t == at(4, 9, 0) || t == at(9, 9, 0) || t == at(15, 9, 0)) cleanup();
    }

    // An add, then a close of the held position on the same bar: of its id
    // (A, C) or of every position (B).
    void add_and_close(std::int64_t t) {
        if (t == at(8, 0, 0)) strategy_entry("A", true, kNaN, kNaN, kNaN, "A seed long");
        if (t == at(8, 0, 30)) {
            strategy_entry("A2", true, kNaN, kNaN, kNaN, "A add long");
            strategy_close("A", "A close", kNaN, kNaN, false);
        }
        if (t == at(9, 0, 0)) strategy_entry("B", true, kNaN, kNaN, kNaN, "B seed long");
        if (t == at(9, 0, 30)) {
            strategy_entry("B2", true, kNaN, kNaN, kNaN, "B add long");
            close_all("B close all");
        }
        if (t == at(10, 0, 0)) strategy_entry("C", false, kNaN, kNaN, kNaN, "C seed short");
        if (t == at(10, 0, 30)) {
            strategy_entry("C2", false, kNaN, kNaN, kNaN, "C add short");
            strategy_close("C", "C close", kNaN, kNaN, false);
        }
        if (t == at(8, 2, 0) || t == at(9, 2, 0) || t == at(10, 2, 0)) close_all("cleanup");
    }

    // w5-m2-pooc-short-comm, and -plain with its fourth short D (C lies
    // beyond the replayed bars).
    void pooc_shorts(std::int64_t t, bool with_d) {
        if (t == at(10, 17, 0)) strategy_entry("A", false, kNaN, kNaN, kNaN, "A short");
        if (t == at(2, 13, 30)) strategy_entry("B", false, kNaN, kNaN, kNaN, "B short");
        if (with_d && t == at(7, 0, 0)) strategy_entry("D", false, kNaN, kNaN, kNaN, "D short");
        if (t == at(10, 18, 30) || t == at(2, 15, 0) || (with_d && t == at(7, 1, 30)))
            cleanup();
    }

    // One of the fifteen-cell scripts: an optional seed entry `seed_offset`
    // minutes before each cell (named `seed`, long when `seed_long`), the
    // cell's own entry at the cell, and a close_all `cleanup_after` minutes
    // after it.
    void cells(std::int64_t t, int seed_offset, const char* seed, bool seed_long,
               const char* id, const char* comment, int cleanup_after,
               bool cell_long = false) {
        for (const std::int64_t cell : kCells) {
            if (seed && t == cell + seed_offset * kMinute)
                strategy_entry(seed, seed_long, kNaN, kNaN, kNaN, seed_long ? "long" : "short");
            if (t == cell) strategy_entry(id, cell_long, kNaN, kNaN, kNaN, comment);
            if (t == cell + cleanup_after * kMinute) close_all("cleanup");
        }
    }

    // The carried short of 2025-04-04 12:00, called again at the 12:30 high;
    // at that bar's close the script reverses by the size it reads (1),
    // closes (2), reverses (3) or only cancels (4).
    void carried(std::int64_t t, int shape) {
        if (t == at(4, 12, 0)) strategy_entry("S", false, kNaN, kNaN, kNaN, "short");
        if (t == at(4, 12, 30)) {
            if (shape == 1) {
                strategy_entry("L", true, kNaN, kNaN, std::abs(signed_position_size()),
                               "reverse by the size seen");
            }
            if (shape == 2) close_all("close at the called bar");
            if (shape == 3) strategy_entry("L", true, kNaN, kNaN, kNaN, "reverse at the called bar");
            if (shape == 4) strategy_cancel_all();
        }
        if (t == at(4, 13, 30)) close_all("cleanup");
    }

    // w5-qp-*: TP1/TP2 close 35 % each and TP3 the rest; `once` places them
    // at the entry bar only, otherwise every bar while short.
    void qty_percent_exits(std::int64_t t, std::int64_t entry_bar, bool once) {
        if (t == at(15, 19, 0)) placed_ = false;
        const bool entry = t == entry_bar;
        if (entry) strategy_entry("S", false, kNaN, kNaN, kNaN, "short");
        const bool short_held = signed_position_size() < 0.0;
        if (once ? ((short_held || entry) && !placed_) : short_held) {
            placed_ = true;
            strategy_exit("TP1", "S", 1577.0, 1700.0, kNaN, kNaN, kNaN, 35.0, "TP1", kNaN, "",
                          kNaN, kNaN);
            strategy_exit("TP2", "S", 1552.0, 1700.0, kNaN, kNaN, kNaN, 35.0, "TP2", kNaN, "",
                          kNaN, kNaN);
            strategy_exit("TP3", "S", 1500.0, 1700.0, kNaN, kNaN, kNaN, 100.0, "TP3", kNaN, "",
                          kNaN, kNaN);
        }
        if (t == utc(4, 16, 6, 0)) cleanup();
    }

    // w5-c2-pooc-flat-explicit-*: at each cell an entry of explicit quantity
    // math.floor(strategy.equity / close * 10000) / 10000.
    void flat_explicit(std::int64_t t, bool is_long) {
        for (const std::int64_t cell : kCells) {
            if (t == cell) {
                const double equity = current_equity() + open_profit(current_bar_.close);
                strategy_entry(is_long ? "L" : "S", is_long, kNaN, kNaN,
                               std::floor(equity / current_bar_.close * 10000.0) / 10000.0,
                               is_long ? "long" : "short");
            }
            if (t == cell + 30 * kMinute) close_all("cleanup");
        }
    }

    Probe probe_;
    bool placed_ = false;
};

std::vector<Bar> eth_feed() {
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

std::vector<Bar> f_feed() {
    std::vector<Bar> bars;
    for (const auto& row : margin_revival_cancel_data::kBars) {
        Bar b{};
        b.timestamp = static_cast<std::int64_t>(row[0]);
        b.open = row[1]; b.high = row[2]; b.low = row[3]; b.close = row[4];
        b.volume = row[5];
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
Run run(Probe probe, const source::PineStrategyConfig& config, const std::vector<Bar>& bars,
        double lot, std::int64_t end_ms) {
    ProbeHost host(probe, config, lot);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, lot));
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty, lot),
                                t.exit_time, ticks(t.exit_price));
    }
    std::sort(out.trades.begin(), out.trades.end());
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// What a probe's strategy() declares, over v6's defaults (initial capital
// 100000, 100 % of equity, pyramiding 1, margin 100 both ways).
source::PineStrategyConfig config(bool pooc, double commission, int slippage,
                                  double percent = 100.0, int pyramiding = 1,
                                  double capital = 100000.0) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = pooc;
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = percent;
    c.pyramiding = pyramiding;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission;
    c.slippage = slippage;
    return c;
}

struct Case {
    const char* rule;
    const char* tape;
    Probe probe;
    source::PineStrategyConfig lane;  // what its generated constructor declares
    bool nyse_f;
    std::size_t closed;               // tape trades closed inside the bars
};

}  // namespace

int main() {
    const std::vector<Bar> eth = eth_feed();
    const std::vector<Bar> f = f_feed();
    const std::int64_t eth_end = eth.back().timestamp;
    const std::int64_t f_end = f.back().timestamp;

    const Case cases[] = {
        {"MR", "w5-mr-global", Probe::MrGlobal, config(false, 0.0, 0), false, 8},
        {"MR control", "w5-mr-named", Probe::MrNamed, config(false, 0.0, 0), false, 8},
        {"MR", "w5-mr-once-global", Probe::MrOnceGlobal, config(false, 0.0, 0), false, 8},
        {"MR control", "w5-mr-once-named", Probe::MrOnceNamed, config(false, 0.0, 0), false, 8},
        {"MR", "w5-rv-global-held", Probe::RvGlobalHeld,
         config(false, 0.0, 0, 100.0, 1, 10000.0), true, 2},
        {"MR control", "w5-rv-named-control", Probe::RvNamed,
         config(false, 0.0, 0, 100.0, 1, 10000.0), true, 2},
        {"M1", "w5-m1-addexit-p100", Probe::AddExit, config(false, 0.0, 0, 100.0, 10), false, 5},
        {"M1 control", "w5-m1-addexit-p60", Probe::AddExit, config(false, 0.0, 0, 60.0, 10),
         false, 3},
        {"M1", "w5-m1-addclose", Probe::AddClose, config(false, 0.0, 0, 100.0, 2), false, 4},
        {"C1", "w5-m2-pooc-short-comm", Probe::PoocShortComm, config(true, 0.05, 1), false, 6},
        {"M2", "w5-m2-pooc-short-open", Probe::PoocShortOpen, config(true, 0.05, 1), false, 46},
        {"M2 control", "w5-m2-pooc-short-plain", Probe::PoocShortPlain, config(true, 0.0, 0),
         false, 8},
        {"C1", "w5-c1-seen-size", Probe::SeenSize, config(true, 0.05, 1), false, 5},
        {"C1", "w5-m2-pooc-carried-close", Probe::CarriedClose, config(true, 0.05, 1), false, 4},
        {"C1", "w5-m2-pooc-carried-none", Probe::CarriedNone, config(true, 0.05, 1), false, 4},
        {"QP", "w5-qp-pooc-reissue", Probe::QpReissue, config(true, 0.05, 1), false, 5},
        {"QP control", "w5-qp-pooc-once", Probe::QpOnce, config(true, 0.05, 1), false, 5},
        {"C2", "w5-c2-pooc-rev-long", Probe::RevToLong, config(true, 0.05, 1), false, 72},
        {"C2", "w5-c2-pooc-rev-long-noslip", Probe::RevToLong, config(true, 0.05, 0), false, 59},
        {"C2", "w5-c2-pooc-rev-short", Probe::RevToShort, config(true, 0.05, 1), false, 60},
        {"C2 control", "w5-c2-pooc-long-open", Probe::LongOpen, config(true, 0.05, 1), false, 15},
        {"C2", "w5-c2-pooc-flat-explicit-long", Probe::FlatExplicitLong, config(true, 0.1, 0),
         false, 30},
        {"C2", "w5-c2-pooc-flat-explicit-short", Probe::FlatExplicitShort, config(true, 0.1, 0),
         false, 42},
        {"C1+C2", "w5-m2-pooc-carried-reverse", Probe::CarriedReverse, config(true, 0.05, 1),
         false, 6},
        {"MK", "w5-qp-mkt-once", Probe::QpMarketOnce, config(false, 0.05, 1), false, 5},
        {"MK", "w5-qp-mkt-reissue", Probe::QpMarketReissue, config(false, 0.05, 1), false, 5},
        {"MK", "w5-mk-short-rev", Probe::MarketRevToShort, config(false, 0.05, 1), false, 61},
    };

    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.rule, c.tape);
        const double lot = c.nyse_f ? kShareLot : kEthLot;
        const std::int64_t end = c.nyse_f ? f_end : eth_end;
        const std::vector<Row> tape = tape_trades(c.tape, end, lot);
        CHECK(tape.size() == c.closed);
        const Run lane = run(c.probe, c.lane, c.nyse_f ? f : eth, lot, end);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape);
        if (lane.trades != tape) {
            show("tape", tape);
            show("engine", lane.trades);
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
