/*
 * test_run_harness_window_tapes.cpp — lane RUN-HARNESS, item 1.
 *
 * TradingView's deep backtest over the ws report channel computes a script
 * from the first chart bar at or after the requested range start and from no
 * earlier bar, and its broker is live from that bar: an order placed on any
 * bar before the tape's first fill can make that fill. scripts/run_strategy.py
 * used to gate every tape-graded run at the bar before TradingView's first
 * entry, so such an order was never placed (jaysharma: a supertrend stop
 * placed before 08:00 UTC fills at 08:15; ki62: phase A's stop, armed once).
 * A run whose feed starts on TradingView's first bar now opens its window
 * there (_tv_entry_emit_window); scripts/test_run_strategy_emit_window.py
 * pins that decision and reads the rule off every tape's metrics.json.
 *
 * Each row replays a synthetic probe's TradingView tape
 * (tests/fixtures/run_harness_window, lab tv exports on BINANCE:ETHUSDT.P 15,
 * 2025-04-01 .. 2025-04-08) through the Pine adapter over the corpus 15m bars
 * embedded in bars.inc, and requires every trade the tape closes inside those
 * bars to be the engine's: entry and exit time, side, price in ticks of 0.01
 * and quantity. A run starting on TradingView's first bar with the gate there
 * books each tape exactly; gated at the signal bar before TradingView's first
 * entry, as the old window was, rh-inrange-preplaced loses S1, the stop placed
 * at 01:00 UTC that TradingView filled at 06:30. A warmed feed keeps the
 * signal-bar gate: on rh-prerange-fills it books TradingView's trade too.
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

#ifndef PINEFORGE_RUN_HARNESS_WINDOW_FIXTURE_DIR
#error "PINEFORGE_RUN_HARNESS_WINDOW_FIXTURE_DIR must name tests/fixtures/run_harness_window"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/run_harness_window/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;      // BINANCE:ETHUSDT.P
constexpr std::int64_t kBarMs = 900'000;

// (entry ms, long, entry price, quantity, exit ms, exit price), prices in ticks.
using Row = std::tuple<std::int64_t, bool, long long, double, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }

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

// TradingView's first computed bar on every tape here (returnedRange.from).
const std::int64_t kTvFirstBar = at(4, 1, 0, 0);

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

std::vector<Row> tape_trades(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_RUN_HARNESS_WINDOW_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
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
        Row& row = by_number[std::stoi(cell[0])];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(std::stod(cell[4]));
            std::get<3>(row) = std::stod(cell[5]);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(std::stod(cell[4]));
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number) out.push_back(row);
    return out;
}

enum class Probe { InRangePreplaced, PreRangeStop, PreRangeLimit, PreRangeFills, DiagFirstBar };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(Probe probe) : probe_(probe) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1000000000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::InRangePreplaced:
            if (t == at(4, 1, 1, 0)) strategy_entry("S1", true, kNaN, 1850.0, 1.0);
            if (t == at(4, 1, 12, 0)) strategy_close("S1", "S1_X", kNaN, kNaN, false);
            if (t == at(4, 1, 13, 0)) strategy_entry("L1", true, 1790.0, kNaN, 2.0);
            if (t == at(4, 3, 12, 0)) strategy_close("L1", "L1_X", kNaN, kNaN, false);
            break;
        case Probe::PreRangeStop:
            if (t == at(3, 31, 18, 0)) strategy_entry("P_STOP", true, kNaN, 1850.0, 1.0);
            if (t == at(4, 1, 12, 0)) strategy_close("P_STOP", "P_STOP_X", kNaN, kNaN, false);
            break;
        case Probe::PreRangeLimit:
            if (t == at(3, 31, 18, 0)) strategy_entry("P_LIMIT", true, 1812.0, kNaN, 2.0);
            if (t == at(4, 3, 12, 0)) strategy_close("P_LIMIT", "P_LIMIT_X", kNaN, kNaN, false);
            break;
        case Probe::PreRangeFills:
            if (t == at(3, 31, 19, 0)) strategy_entry("P_PRE", true, kNaN, kNaN, 3.0);
            if (t == at(3, 31, 20, 0)) strategy_close("P_PRE", "P_PRE_X", kNaN, kNaN, false);
            if (t == at(3, 31, 21, 0)) strategy_entry("P_CARRY", true, kNaN, kNaN, 4.0);
            if (t == at(4, 1, 3, 0)) strategy_close("P_CARRY", "P_CARRY_X", kNaN, kNaN, false);
            if (t == at(4, 2, 0, 0)) strategy_entry("IN", true, kNaN, kNaN, 5.0);
            if (t == at(4, 2, 1, 0)) strategy_close("IN", "IN_X", kNaN, kNaN, false);
            break;
        case Probe::DiagFirstBar:
            diag_first_bar(t);
            break;
        }
    }

private:
    // D1 qty = bar_index + 1 on the first bar at or after 2025-04-01 00:00 UTC;
    // D2 qty = 1 + the 15-minute steps from the first bar to it; D3 qty = 1 +
    // the bars before it.
    void diag_first_bar(std::int64_t t) {
        if (first_time_ < 0) first_time_ = t;
        if (t < kTvFirstBar) ++pre_bars_;
        times_.push_back(t);
        if (t >= kTvFirstBar && step_ == 0) {
            step_ = 1;
            strategy_entry("D1", true, kNaN, kNaN, pine_bar_index() + 1.0);
        } else if (step_ == 1) {
            step_ = 2;
            strategy_close("D1", "D1_X", kNaN, kNaN, false);
        } else if (step_ == 2) {
            step_ = 3;
            const std::int64_t two_back = times_[times_.size() - 3];
            strategy_entry("D2", true, kNaN, kNaN,
                           1.0 + static_cast<double>(two_back - first_time_) / kBarMs);
        } else if (step_ == 3) {
            step_ = 4;
            strategy_close("D2", "D2_X", kNaN, kNaN, false);
        } else if (step_ == 4) {
            step_ = 5;
            strategy_entry("D3", true, kNaN, kNaN, 1.0 + pre_bars_);
        } else if (step_ == 5) {
            step_ = 6;
            strategy_close("D3", "D3_X", kNaN, kNaN, false);
        }
    }

    Probe probe_;
    std::int64_t first_time_ = -1;
    int pre_bars_ = 0;
    int step_ = 0;
    std::vector<std::int64_t> times_;
};

std::vector<Bar> feed_from(std::int64_t first_ms) {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
        if (row.ts < first_ms) continue;
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

// The engine's trades closed inside the bars; a position still open after
// the last bar is closed there by the range end and is not a tape trade.
std::vector<Row> run(Probe probe, std::int64_t feed_start_ms, std::int64_t gate_ms,
                     std::string* error) {
    ProbeHost host(probe);
    host.set_trade_start_time(gate_ms);
    const std::vector<Bar> bars = feed_from(feed_start_ms);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    *error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= bars.back().timestamp) continue;
        out.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), t.qty,
                         t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%g -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

void expect(const char* what, const std::vector<Row>& engine, const std::vector<Row>& tape) {
    std::printf("-- %s\n", what);
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }
}

}  // namespace

int main() {
    std::string error;

    // TradingView's first entry of rh-inrange-preplaced fills at 06:30 UTC;
    // the old window opened on the bar before it.
    const std::vector<Row> preplaced = tape_trades("rh-inrange-preplaced");
    CHECK(preplaced.size() == 2);
    if (preplaced.size() == 2) {
        CHECK(std::get<0>(preplaced[0]) == at(4, 1, 6, 30));
        CHECK(std::get<2>(preplaced[0]) == ticks(1850.0));
        CHECK(std::get<0>(preplaced[1]) == at(4, 2, 22, 45));
        CHECK(std::get<2>(preplaced[1]) == ticks(1790.0));
    }
    const std::int64_t signal_bar = at(4, 1, 6, 15);

    // A run starting on TradingView's first bar, gated there: TV's two trades.
    const std::vector<Row> from_first = run(Probe::InRangePreplaced, kTvFirstBar, kTvFirstBar,
                                            &error);
    CHECK(error.empty());
    expect("rh-inrange-preplaced, window from TradingView's first bar", from_first, preplaced);

    // The same run gated at the signal bar: S1's 01:00 stop is never placed.
    const std::vector<Row> from_signal = run(Probe::InRangePreplaced, kTvFirstBar, signal_bar,
                                             &error);
    CHECK(error.empty());
    std::printf("-- rh-inrange-preplaced, window from the signal bar (the old rule)\n");
    CHECK(from_signal.size() == 1);
    if (from_signal.size() == 1 && preplaced.size() == 2) CHECK(from_signal[0] == preplaced[1]);

    // Nothing runs before TradingView's first bar: the 2025-03-31 cells never
    // trade, and the script reads bar_index 0 at the range start.
    const struct { const char* tape; Probe probe; std::size_t trades; } trimmed[] = {
        {"rh-prerange-stop", Probe::PreRangeStop, 0},
        {"rh-prerange-limit", Probe::PreRangeLimit, 0},
        {"rh-prerange-fills", Probe::PreRangeFills, 1},
        {"rh-diag-firstbar", Probe::DiagFirstBar, 3},
    };
    for (const auto& row : trimmed) {
        const std::vector<Row> tape = tape_trades(row.tape);
        CHECK(tape.size() == row.trades);
        const std::vector<Row> engine = run(row.probe, kTvFirstBar, kTvFirstBar, &error);
        CHECK(error.empty());
        expect(row.tape, engine, tape);
    }
    const std::vector<Row> diag = tape_trades("rh-diag-firstbar");
    for (const Row& r : diag) CHECK(std::get<3>(r) == 1.0);

    // A warmed feed (from 2025-03-31 00:00) keeps the signal-bar gate
    // (TradingView's first entry fills at 2025-04-02 00:15): the pre-range
    // cells are ignored, as TradingView never ran them.
    const std::vector<Row> fills = tape_trades("rh-prerange-fills");
    const std::vector<Row> warmed = run(Probe::PreRangeFills, at(3, 31, 0, 0), at(4, 2, 0, 0),
                                        &error);
    CHECK(error.empty());
    expect("rh-prerange-fills, warmed feed, window from the signal bar", warmed, fills);

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
