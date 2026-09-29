/*
 * test_exit_reissue_na_tapes.cpp -- R5 lane TAIL-D.
 *
 * A strategy.exit call that names a level replaces the whole exit order: a
 * price leg the new call leaves na is withdrawn on TradingView, a stop and a
 * limit alike. The adapter re-issued only the levels the call named and kept
 * the old leg resting (fran-pineda-strategy-501 / -502 on OANDA:XAUUSD 15: a
 * break-even re-issue with limit = na, after which the old take-profit
 * filled; the legacy route erased every leg of the exit id before
 * re-creating it, ab9714be pine_strategy_commands.cpp clear_existing_exit_order).
 *
 * The lane's own synthetic script (tests/fixtures/exit_reissue_na, one `lab tv
 * --no-note` export, OANDA:XAUUSD 15, 2025-08-14 .. 2025-08-20, pyramiding 2,
 * a fixed size of 1): longs C and D (Friday 13:15 fill) carry CX / DX, stop
 * 3336 and limit 3345; at the 13:15 close CX is re-issued with stop = na, DX
 * with both levels (control). Shorts A and B (16:15 fill) carry AX / BX, stop
 * 3348.1 and limit 3332.95; on the Sunday reopen bar AX is re-issued with a
 * new stop 3340.525 and limit = na, BX with that stop and the limit
 * (control). The na levels are a runtime `var float` na, as a script resets
 * one. The row replays TradingView's own tape through the Pine adapter over
 * the tape's bars (bars.inc) and requires every trade to be the engine's:
 * entry and exit time, side, price in ticks and quantity.
 *
 * Fail-before (lane report): CX's dropped stop fills at 3336 (13:30) and
 * AX's dropped limit at 3332.95 (Sunday 23:00).
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

#ifndef PINEFORGE_TAIL_D_REISSUE_NA_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_REISSUE_NA_FIXTURE_DIR must name tests/fixtures/exit_reissue_na"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/exit_reissue_na/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.001;  // OANDA:XAUUSD

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

// 2025-08-<day> <hour>:<minute> UTC.
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 8, day) * 24 + hour) * 60 + minute) * 60'000;
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
    std::ifstream in(std::string(PINEFORGE_TAIL_D_REISSUE_NA_FIXTURE_DIR) + "/" + tape
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

// The probe's body (tests/fixtures/exit_reissue_na/fran-x1-exit-reissue-na-limit/strategy.pine).
class ReissueNaHost final : public source::PineStrategyHost {
public:
    ReissueNaHost() {
        attach_pine_execution_adapter();
        set_syminfo_session("1800-1700");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.pyramiding = 2;
        c.initial_capital = 10000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 0.01);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double na_level = kNaN;  // var float naLevel = na
        if (t == at(15, 13, 0)) {
            strategy_entry("C", true);
            strategy_entry("D", true);
            strategy_exit("CX", "C", 3345.0, 3336.0);
            strategy_exit("DX", "D", 3345.0, 3336.0);
        }
        if (t == at(15, 13, 15)) {
            strategy_exit("CX", "C", 3345.0, na_level);
            strategy_exit("DX", "D", 3345.0, 3336.0);
        }
        if (t == at(15, 16, 0) && signed_position_size() == 0.0) {
            strategy_entry("A", false);
            strategy_entry("B", false);
            strategy_exit("AX", "A", 3332.95, 3348.1);
            strategy_exit("BX", "B", 3332.95, 3348.1);
        }
        if (t == at(17, 22, 0)) {
            strategy_exit("AX", "A", na_level, 3340.525);
            strategy_exit("BX", "B", 3332.95, 3340.525);
        }
        if (t == at(18, 12, 0)) strategy_close_all();
    }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kXau15) {
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
    std::printf("-- fran-x1-exit-reissue-na-limit\n");
    const std::vector<Bar> bars = feed();
    const std::vector<Row> tape = tape_trades("fran-x1-exit-reissue-na-limit");
    CHECK(tape.size() == 4);
    ReissueNaHost host;
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price),
                            std::llround(t.qty), t.exit_time, ticks(t.exit_price));
    }
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
