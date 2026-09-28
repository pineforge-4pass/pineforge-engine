/*
 * test_margin_residual_tapes.cpp — lane W5B-ENG-MARGIN-RESIDUAL.
 *
 * The margin-family divergences left after lane W5-ENG-MARGIN-V6, at Pine
 * v6's defaults (100% of equity per order, margin 100 both ways). Each rule is
 * read off TradingView's own tapes of synthetic probes written for this lane:
 *
 *   SB  Entries a bar places while the book is flat all fill where they reach
 *       their fill point, each judged on its own cost against the equity it
 *       was sized from, the later ones behind the book the earlier ones
 *       opened; the combined book is margin-called once they have all filled
 *       -- at that fill, or under process_orders_on_close at the next open,
 *       sized at the close.
 *   PC  A short carried under process_orders_on_close is checked over its
 *       bar's path before the script runs at the close, fees or none, at any
 *       money class, a pending entry beside it or not: the script reads the
 *       called size, a take-profit later on the path meets what the call
 *       left, and the script's close, reversal or add fills after it. An add
 *       filled at the close is checked at the next open, not at that bar's
 *       high.
 *   PA  An add to a held position of its side is judged, when it is placed,
 *       on the held units plus its own at the signal close's tick against the
 *       equity there, with no lot of slack; under process_orders_on_close at
 *       its close fill.
 *
 * Each row replays one lab tv tape (tests/fixtures/margin_residual) through
 * the Pine adapter under the configuration the generated constructor declares
 * for its probe, over the corpus 15m bars of lane W5-ENG-MARGIN-V6
 * (tests/fixtures/margin_v6/bars.inc, BINANCE:ETHUSDT.P) or the lab lane f-15
 * NYSE:F 15m bars (tests/fixtures/margin_residual/f15_bars.inc), and requires
 * every trade the tape closes inside those bars to be the engine's: entry and
 * exit time, side, price in ticks and quantity in lots. On the lane's base
 * every rule row fails here, and every control row beside it passes.
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

#ifndef PINEFORGE_MARGIN_RESIDUAL_FIXTURE_DIR
#error "PINEFORGE_MARGIN_RESIDUAL_FIXTURE_DIR must name tests/fixtures/margin_residual"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/margin_v6/bars.inc"
#include "fixtures/margin_residual/f15_bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;       // both charts' price tick
constexpr double kEthLot = 0.0001;   // TradingView's BINANCE:ETHUSDT.P quantity step
constexpr double kShareLot = 1.0;    // NYSE:F
constexpr std::int64_t kMinute = 60'000;

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

// 2025-04-<day> <hour>:<minute> UTC, the probes' timestamp("UTC", ...).
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

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms, double lot) {
    std::ifstream in(std::string(PINEFORGE_MARGIN_RESIDUAL_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
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
    SameBarExplicit60,   // w5b-sb-market-60x2 / w5b-sb-pooc-60x2
    SameBarExplicit40,   // w5b-sb-market-40x2
    SameBarDefault3,     // w5b-sb-default-x3 / w5b-sb-pooc-default-x3
    CarriedNone,         // w5b-pc-carried-none
    CarriedReverse,      // w5b-pc-carried-reverse
    CarriedClose,        // w5b-pc-carried-close
    SeenSize,            // w5b-pc-seen-size
    SeenSizeParked,      // w5b-pc-seen-size-parked
    BracketTakeProfit,   // w5b-pc-bracket-tp
    AddCells,            // w5b-pa-pooc-p50 / -pooc-p40 / -market-p50
    AddNearFill,         // w5b-pa-pooc-p50-tick
    AddNyseF,            // w5b-pa-f-pooc-p50 / -f-market-p50 (NYSE:F)
};

// The fifteen signal closes of the w5b-pc-* probes (W5-ENG-MARGIN-V6's cells).
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
        if (probe == Probe::AddNyseF) {
            set_syminfo_timezone("America/New_York");
            set_syminfo_session("0930-1600");
        }
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::SameBarExplicit60: same_bar_explicit(t, 0.6); break;
        case Probe::SameBarExplicit40: same_bar_explicit(t, 0.4); break;
        case Probe::SameBarDefault3: same_bar_default(t); break;
        case Probe::CarriedNone:
        case Probe::CarriedReverse:
        case Probe::CarriedClose:
        case Probe::SeenSize:
        case Probe::SeenSizeParked:
        case Probe::BracketTakeProfit: carried(t); break;
        case Probe::AddCells: add_cells(t); break;
        case Probe::AddNearFill: add_near_fill(t); break;
        case Probe::AddNyseF: add_nyse_f(t); break;
        }
    }

private:
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }
    // strategy.position_avg_price: na while flat.
    double avg_price() const {
        return signed_position_size() == 0.0 ? kNaN : position_entry_price_;
    }
    int open_trades() const { return static_cast<int>(pyramid_entries_.size()); }

    // strategy.equity at this bar's close.
    double equity() { return current_equity() + open_profit(current_bar_.close); }

    // w5b-sb-*-60x2 / -40x2: every 3 hours from 2025-04-02 00:00 UTC, forty
    // cells, two same-side entries A and B from flat, each of explicit
    // quantity math.floor(strategy.equity * share / close * 10000) / 10000;
    // even cells long, odd short; flattened an hour later.
    void same_bar_explicit(std::int64_t t, double share) {
        const std::int64_t t0 = at(2, 0, 0);
        const std::int64_t step = 180 * kMinute;
        const std::int64_t rel = t - t0;
        if (rel < 0 || rel >= 40 * step) return;
        const std::int64_t phase = rel % step;
        const bool is_long = (rel / step) % 2 == 0;
        const double q = std::floor(equity() * share / current_bar_.close * 10000.0) / 10000.0;
        if (phase == 0 && signed_position_size() == 0.0) {
            strategy_entry("A", is_long, kNaN, kNaN, q, "A");
            strategy_entry("B", is_long, kNaN, kNaN, q, "B");
        }
        if (phase == 60 * kMinute) close_all("cleanup");
    }

    // w5b-sb-*default-x3: every 6 hours from 2025-04-02 01:00 UTC, twenty
    // cells, three same-side default entries A, B and C from flat; even
    // cells long, odd short; flattened an hour later.
    void same_bar_default(std::int64_t t) {
        const std::int64_t t0 = at(2, 1, 0);
        const std::int64_t step = 360 * kMinute;
        const std::int64_t rel = t - t0;
        if (rel < 0 || rel >= 20 * step) return;
        const std::int64_t phase = rel % step;
        const bool is_long = (rel / step) % 2 == 0;
        if (phase == 0 && signed_position_size() == 0.0) {
            strategy_entry("A", is_long, kNaN, kNaN, kNaN, "A");
            strategy_entry("B", is_long, kNaN, kNaN, kNaN, "B");
            strategy_entry("C", is_long, kNaN, kNaN, kNaN, "C");
        }
        if (phase == 60 * kMinute) close_all("cleanup");
    }

    // w5b-pc-*: at each cell a default short fills at the close; on the next
    // bar the script does its probe's action at the close; flattened 45
    // minutes after the cell.
    void carried(std::int64_t t) {
        for (const std::int64_t cell : kCells) {
            if (t == cell) {
                strategy_entry("S", false, kNaN, kNaN, kNaN, "short");
                if (probe_ == Probe::SeenSizeParked) {
                    strategy_entry("P", true, std::round(current_bar_.close * 0.5 * 100.0) / 100.0,
                                   kNaN, 0.001, "parked");
                }
                if (probe_ == Probe::BracketTakeProfit) {
                    strategy_exit("X", "S", current_bar_.close * 0.998, current_bar_.close * 1.03);
                }
            }
            if (t == cell + 15 * kMinute) {
                switch (probe_) {
                case Probe::CarriedNone: strategy_cancel_all(); break;
                case Probe::CarriedReverse:
                    strategy_entry("L", true, kNaN, kNaN, kNaN, "reverse");
                    break;
                case Probe::CarriedClose: close_all("close"); break;
                case Probe::SeenSize:
                case Probe::SeenSizeParked:
                    strategy_order("B", true, std::abs(signed_position_size()));
                    break;
                default: break;
                }
            }
            if (t == cell + 45 * kMinute) {
                if (probe_ == Probe::SeenSizeParked) strategy_cancel("P");
                close_all("cleanup");
            }
        }
    }

    // w5b-pa-pooc-p50 / -p40 / -market-p50: every 2 hours from 2025-04-02
    // 00:00 UTC, sixty cells; E1 from flat, E2 45 minutes later while E1 is
    // the only trade; flattened 90 minutes in. Even cells long, odd short.
    void add_cells(std::int64_t t) {
        const std::int64_t t0 = at(2, 0, 0);
        const std::int64_t step = 120 * kMinute;
        const std::int64_t rel = t - t0;
        if (rel < 0 || rel >= 60 * step) return;
        const std::int64_t phase = rel % step;
        const bool is_long = (rel / step) % 2 == 0;
        if (phase == 0 && signed_position_size() == 0.0)
            strategy_entry("E1", is_long, kNaN, kNaN, kNaN);
        if (phase == 45 * kMinute && open_trades() == 1)
            strategy_entry("E2", is_long, kNaN, kNaN, kNaN);
        if (phase == 90 * kMinute) close_all("cleanup");
    }

    // w5b-pa-pooc-p50-tick: every 2 hours from 2025-04-02 00:00 UTC, 168
    // cells; a long E1 from flat, E2 at the first later close within two
    // ticks of E1's price while E1 is the only trade; flattened 105 minutes in.
    void add_near_fill(std::int64_t t) {
        const std::int64_t t0 = at(2, 0, 0);
        const std::int64_t step = 120 * kMinute;
        const std::int64_t rel = t - t0;
        if (rel < 0 || rel >= 168 * step) return;
        const std::int64_t phase = rel % step;
        if (phase == 0 && signed_position_size() == 0.0)
            strategy_entry("E1", true, kNaN, kNaN, kNaN);
        if (phase > 0 && phase < 105 * kMinute && open_trades() == 1
            && std::abs(current_bar_.close - avg_price()) <= 2 * kTick + 1e-9)
            strategy_entry("E2", true, kNaN, kNaN, kNaN);
        if (phase == 105 * kMinute) close_all("cleanup");
    }

    // w5b-pa-f-*: New York 09:30 .. 14:30 on the half hour, a long E1 from
    // flat; the next bar an add E2 while E1 is the only trade; flattened at
    // the close 45 minutes after E1. The window is EDT (UTC-4) throughout.
    void add_nyse_f(std::int64_t t) {
        const std::int64_t minute_of_day = ((t / kMinute) % 1440 + 1440) % 1440;
        const int hour = static_cast<int>((minute_of_day / 60 + 24 - 4) % 24);
        const int minute = static_cast<int>(minute_of_day % 60);
        if (minute == 30 && hour >= 9 && hour <= 14 && signed_position_size() == 0.0)
            strategy_entry("E1", true, kNaN, kNaN, kNaN);
        if (minute == 45 && hour >= 9 && hour <= 14 && open_trades() == 1)
            strategy_entry("E2", true, kNaN, kNaN, kNaN);
        if (minute == 15 && hour >= 10 && hour <= 15) close_all("cleanup");
    }

    Probe probe_;
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
source::PineStrategyConfig config(bool pooc, int pyramiding, double percent = 100.0) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = pooc;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = percent;
    c.pyramiding = pyramiding;
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
    const std::vector<Bar> eth = feed(kEth15);
    const std::vector<Bar> f = feed(kF15);
    const std::int64_t eth_end = eth.back().timestamp;
    const std::int64_t f_end = f.back().timestamp;

    const Case cases[] = {
        {"SB", "w5b-sb-market-60x2", Probe::SameBarExplicit60, config(false, 2), false, 120},
        {"SB", "w5b-sb-pooc-60x2", Probe::SameBarExplicit60, config(true, 2), false, 120},
        {"SB", "w5b-sb-default-x3", Probe::SameBarDefault3, config(false, 3), false, 48},
        {"SB", "w5b-sb-pooc-default-x3", Probe::SameBarDefault3, config(true, 3), false, 60},
        {"SB control", "w5b-sb-market-40x2", Probe::SameBarExplicit40, config(false, 2), false,
         80},
        {"PC", "w5b-pc-seen-size", Probe::SeenSize, config(true, 1), false, 36},
        {"PC", "w5b-pc-seen-size-parked", Probe::SeenSizeParked, config(true, 1), false, 36},
        {"PC", "w5b-pc-carried-close", Probe::CarriedClose, config(true, 1), false, 36},
        {"PC", "w5b-pc-carried-reverse", Probe::CarriedReverse, config(true, 1), false, 51},
        {"PC", "w5b-pc-bracket-tp", Probe::BracketTakeProfit, config(true, 1), false, 35},
        {"PC", "w5b-pa-pooc-p50", Probe::AddCells, config(true, 2, 50.0), false, 109},
        {"PC control", "w5b-pc-carried-none", Probe::CarriedNone, config(true, 1), false, 37},
        {"PA", "w5b-pa-f-pooc-p50", Probe::AddNyseF, config(true, 2, 50.0), true, 248},
        {"PA", "w5b-pa-f-market-p50", Probe::AddNyseF, config(false, 2, 50.0), true, 245},
        {"PA control", "w5b-pa-pooc-p40", Probe::AddCells, config(true, 2, 40.0), false, 120},
        {"PA control", "w5b-pa-market-p50", Probe::AddCells, config(false, 2, 50.0), false,
         109},
        {"PA control", "w5b-pa-pooc-p50-tick", Probe::AddNearFill, config(true, 2, 50.0), false,
         171},
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
