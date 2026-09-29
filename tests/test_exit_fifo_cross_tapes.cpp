/*
 * test_exit_fifo_cross_tapes.cpp -- R5 lane TAIL-D.
 *
 * TradingView reserves a qty= strategy.exit on its own entry's quantity, and
 * the position it closes is paired first in, first out: an exit of entry B
 * that fills while entry A still holds its last unit closes A's lot, and A's
 * own second exit still fills later, closing B's oldest lot (the -a control,
 * which the engine already booked: the exits wait while their entry is
 * pending on a flat book, and its opening settles them against the whole
 * position).
 *
 * When A is the reversal a strategy.close() of the held short and
 * strategy.entry("A") place on one bar, A waits in the adapter for that
 * close; its exits reached the book at once, bound to A's cohort, and after
 * the FIFO close left that cohort no lot A's second exit never filled. The
 * exits now wait for the held entry as they do on a flat book (-g).
 *
 * The lane's own synthetic scripts (tests/fixtures/exit_fifo_cross, one `lab
 * tv --no-note` export each, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-03,
 * pyramiding 2, fixed cells on 2025-04-01 UTC):
 *   -a  A (2 units) at 11:30 with AT1/AT2 (one unit each, limits 1862 and
 *       1873); B (2 units) at 12:00 with BT1/BT2 (limits 1871 and 1880);
 *       BT1 fills while A holds its last unit; close_all at 14:30;
 *   -g  the same after a short S of 2 (with its own two exits) that the
 *       11:30 bar reverses by strategy.close("S"), strategy.close("SA") (no
 *       such id) and strategy.entry("A"); A's and B's exits carry a stop,
 *       and at 12:30 B is called again (pyramiding full) and its exits are
 *       re-issued with a moved stop.
 * Each row replays TradingView's own tape through the Pine adapter over the
 * tape's bars (bars.inc) and requires every trade the tape closes inside
 * those bars to be the engine's: entry and exit time, side, price in ticks
 * and quantity.
 *
 * Fail-before (lane report): -g holds B's first lot to the 14:30 close_all.
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

#ifndef PINEFORGE_TAIL_D_FIFO_CROSS_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_FIFO_CROSS_FIXTURE_DIR must name tests/fixtures/exit_fifo_cross"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/exit_fifo_cross/bars.inc"

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

// 2025-04-01 <hour>:<minute> UTC.
std::int64_t at(int hour, int minute) {
    return ((days_from_civil(2025, 4, 1) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

std::vector<Row> tape_trades(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_TAIL_D_FIFO_CROSS_FIXTURE_DIR) + "/" + tape
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
    for (const auto& [number, row] : by_number) out.push_back(row);
    return out;
}

// The probes' bodies (tests/fixtures/exit_fifo_cross/<slug>/strategy.pine).
class FifoCrossHost final : public source::PineStrategyHost {
public:
    explicit FifoCrossHost(bool reversal) : reversal_(reversal) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.pyramiding = 2;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 0.0001);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        // -g's stops; -a's exits carry none.
        const double low_stop = reversal_ ? 1800.0 : kNaN;
        const auto leg = [this](const char* id, const char* from, double limit, double stop) {
            strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, 100.0, {}, 1.0);
        };
        if (reversal_ && t == at(11, 0)) {
            strategy_entry("S", false, kNaN, kNaN, 2.0);
            leg("ST1", "S", 1840.0, 1900.0);
            leg("ST2", "S", 1830.0, 1900.0);
        }
        if (t == at(11, 30)) {
            if (reversal_) {
                strategy_close("S");
                strategy_close("SA");
            }
            strategy_entry("A", true, kNaN, kNaN, 2.0);
            leg("AT1", "A", 1862.0, low_stop);
            leg("AT2", "A", 1873.0, low_stop);
        }
        if (t == at(12, 0)) {
            strategy_entry("B", true, kNaN, kNaN, 2.0);
            leg("BT1", "B", 1871.0, low_stop);
            leg("BT2", "B", 1880.0, low_stop);
        }
        if (reversal_ && t == at(12, 30)) {
            strategy_entry("B", true, kNaN, kNaN, 2.0);
            leg("BT1", "B", 1871.0, 1805.0);
            leg("BT2", "B", 1880.0, 1805.0);
        }
        if (t == at(14, 30)) strategy_close_all();
    }

private:
    bool reversal_;
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

void replay(const char* tape_name, bool reversal, std::size_t closed) {
    std::printf("-- %s\n", tape_name);
    static const std::vector<Bar> bars = feed();
    const std::vector<Row> tape = tape_trades(tape_name);
    CHECK(tape.size() == closed);
    FifoCrossHost host(reversal);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
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
    replay("td-m6a", false, 4);
    replay("td-m6g", true, 5);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
