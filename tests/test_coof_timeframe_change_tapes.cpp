/*
 * test_coof_timeframe_change_tapes.cpp -- R5 lane TAIL-D.
 *
 * timeframe.change("D") compares the script bar with the bar before it, on
 * every calculation of the bar. Under calc_on_order_fills a fill's
 * recalculation publishes its bar ahead of the bar's close, and the host
 * moved the previous-bar clock onto the bar itself there: the bar's close
 * calculation then saw no day change. TradingView resets a day's counter on
 * the close of a day's first bar that a fill recalculated.
 *
 * The lane's own synthetic script (tests/fixtures/coof_timeframe_change, one
 * `lab tv --no-note` export, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-15,
 * calc_on_order_fills on): an entry A at 23:45 UTC carries a stop one point
 * under that close, which usually fills on the next day's first bar and so
 * recalculates it; a daily counter, reset by timeframe.change("D") and
 * incremented by A's placement, gates an entry B at 12:00 that enters only
 * when the reset ran. The row replays TradingView's own tape through the Pine
 * adapter over the tape's bars (bars.inc) and requires every trade the tape
 * closes inside those bars to be the engine's: entry and exit time, side,
 * price in ticks and quantity.
 *
 * Fail-before (lane report): no B enters on the 13 days A's stop filled on
 * the first bar.
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

#ifndef PINEFORGE_TAIL_D_TF_CHANGE_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_TF_CHANGE_FIXTURE_DIR must name tests/fixtures/coof_timeframe_change"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_timeframe_change/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;

// (entry ms, long, entry ticks, quantity, exit ms, exit ticks)
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
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
    std::vector<Row> trades;  // closed inside the replayed bars, in entry order
    std::size_t b_entries = 0;
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_TAIL_D_TF_CHANGE_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> signal;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = std::llround(std::stod(cell[5]));
            signal[number] = cell[3];
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        if (std::get<4>(row) <= 0 || std::get<4>(row) >= end_ms) continue;
        out.trades.push_back(row);
        if (signal[number] == "B") ++out.b_entries;
    }
    return out;
}

// The probe's body (tests/fixtures/coof_timeframe_change/td-m3a/strategy.pine).
class DayCounterHost final : public source::PineStrategyHost {
public:
    DayCounterHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.initial_capital = 10000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 0.0001);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar& bar) override {
        const int k = bar_index_;
        if (tf_change(prev_bar_timestamp_, current_bar_.timestamp, "D", syminfo_.timezone,
                      syminfo_.session)) {
            day_count_ = 0;
        }
        const double units = signed_position_size();
        const bool flat = units == 0.0;
        const std::int64_t minute_of_day = (current_bar_.timestamp / 60'000) % 1440;
        if (flat && minute_of_day == 23 * 60 + 45) {
            strategy_entry("A", true);
            strategy_exit("XA", "A", kNaN, bar.close - 1.0);
            ++day_count_;
        }
        if (!flat && minute_of_day == 11 * 60 + 45) strategy_close("", "noon", kNaN, kNaN, false);
        if (flat && minute_of_day == 12 * 60 && day_count_ == 0) strategy_entry("B", true);
        if (units > 0.0 && open_trade_entry_id(0) == "B" && k - open_trade_entry_bar_index(0) >= 2)
            strategy_close("B", "B done");
    }

protected:
    // var int dayCount: a fill's recalculation starts from the bar's committed
    // value and leaves it as it was.
    void snapshot_script_state() override { saved_ = day_count_; }
    void restore_script_state() override { day_count_ = saved_; }
    void commit_script_state() override { saved_ = day_count_; }

private:
    int day_count_ = 0;
    int saved_ = 0;
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

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-6s %lld %s @%lld q=%lld -> %lld @%lld\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

}  // namespace

int main() {
    std::printf("-- td-m3a\n");
    const std::vector<Bar> bars = feed();
    const std::int64_t end_ms = bars.back().timestamp;
    const Tape tape = tape_trades("td-m3a", end_ms);
    CHECK(tape.trades.size() == 27);
    CHECK(tape.b_entries == 14);
    DayCounterHost host;
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), std::llround(t.qty),
                            t.exit_time, ticks(t.exit_price));
    }
    CHECK(engine == tape.trades);
    if (engine != tape.trades) {
        show("tape", tape.trades);
        show("engine", engine);
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
