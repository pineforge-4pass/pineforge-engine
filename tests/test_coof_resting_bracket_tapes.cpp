/*
 * test_coof_resting_bracket_tapes.cpp -- R5 lane TAIL-D.
 *
 * Under calc_on_order_fills a limit entry E that fills inside a leg of the
 * bar's path starts a recalculation there. The bracket X the script placed
 * while E rested becomes live at E's fill, and TradingView fills its stop at
 * its level on the rest of that leg -- also when the recalculation re-issues
 * X, with its limit changed or unchanged and the stop the same. The engine
 * took the re-issued stop for an exit born in the recalculation, held it to
 * the end of the leg and booked it at the extreme.
 *
 * The lane's own synthetic scripts (tests/fixtures/coof_resting_bracket, one
 * `lab tv --no-note` export each, BINANCE:ETHUSDT.P 15, 2025-04-01 ..
 * 2025-04-05, calc_on_order_fills on): on every 2-hour UTC slot a flat book
 * places E, a limit 2 points through the close (long on 4-hour slots, short
 * between), with X: a stop 3 points beyond E and a limit 20 points past it;
 * E is cancelled a bar later when it did not fill, and the position is
 * closed after four bars. While in the position the script --
 *   -a  re-issues X with its limit moved to 10 points (stop the same);
 *   -b  re-issues X unchanged;
 *   -c  does not re-issue X (control).
 * Each row replays TradingView's own tape through the Pine adapter over the
 * tape's bars (bars.inc) and requires every trade the tape closes inside
 * those bars to be the engine's: entry and exit time, side, price in ticks
 * and quantity.
 *
 * Fail-before (lane report): -a and -b book the stop at the leg's extreme.
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

#ifndef PINEFORGE_TAIL_D_RESTING_BRACKET_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_RESTING_BRACKET_FIXTURE_DIR must name tests/fixtures/coof_resting_bracket"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_resting_bracket/bars.inc"

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

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_TAIL_D_RESTING_BRACKET_FIXTURE_DIR) + "/" + tape
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
        if (cell.size() < 6) continue;
        Row& row = by_number[std::stoi(cell[0])];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = std::llround(std::stod(cell[5]));
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number)
        if (std::get<4>(row) > 0 && std::get<4>(row) < end_ms) out.push_back(row);
    return out;
}

enum class Reissue { Moved, Unchanged, None };

struct Variant {
    const char* tape;
    Reissue reissue;
    std::size_t closed;  // tape trades closed inside the bars
};

// The probes' bodies (tests/fixtures/coof_resting_bracket/<slug>/strategy.pine).
class RestingBracketHost final : public source::PineStrategyHost {
public:
    explicit RestingBracketHost(const Variant& v) : v_(v) {
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
        const int n = trade_count();
        const bool exit_here = n > 0 && closed_trade_exit_bar_index(n - 1) == k;
        const bool flat = signed_position_size() == 0.0;
        const std::int64_t minute = current_bar_.timestamp / 60'000;
        const bool slot = minute % 120 == 0;
        if (flat && slot && !exit_here) {
            long_ = minute % 240 == 0;
            level_ = long_ ? bar.close - 2.0 : bar.close + 2.0;
            strategy_entry("E", long_, level_);
            strategy_exit("X", "E", long_ ? level_ + 20.0 : level_ - 20.0,
                          long_ ? level_ - 3.0 : level_ + 3.0);
        }
        if (flat && !slot) strategy_cancel("E");
        if (!flat) {
            if (v_.reissue != Reissue::None) {
                const double reach = v_.reissue == Reissue::Moved ? 10.0 : 20.0;
                strategy_exit("X", "E", long_ ? level_ + reach : level_ - reach,
                              long_ ? level_ - 3.0 : level_ + 3.0);
            }
            if (k - open_trade_entry_bar_index(0) >= 4) strategy_close("E");
        }
    }

protected:
    // var float L / var bool isLong: a fill's recalculation starts from the
    // bar's committed values and leaves them as they were.
    void snapshot_script_state() override { saved_ = {level_, long_}; }
    void restore_script_state() override { std::tie(level_, long_) = saved_; }
    void commit_script_state() override { saved_ = {level_, long_}; }

private:
    const Variant& v_;
    double level_ = kNaN;
    bool long_ = true;
    std::tuple<double, bool> saved_{kNaN, true};
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

void replay(const Variant& v) {
    std::printf("-- %s\n", v.tape);
    static const std::vector<Bar> bars = feed();
    const std::int64_t end_ms = bars.back().timestamp;
    const std::vector<Row> tape = tape_trades(v.tape, end_ms);
    CHECK(tape.size() == v.closed);
    RestingBracketHost host(v);
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
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"td-m2a", Reissue::Moved, 37},
        {"td-m2b", Reissue::Unchanged, 37},
        {"td-m2c", Reissue::None, 37},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
