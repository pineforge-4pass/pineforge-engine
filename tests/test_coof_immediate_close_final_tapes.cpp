/*
 * test_coof_immediate_close_final_tapes.cpp -- lane TAIL-E: a close the
 * bar's own calculation executes (strategy.close / close_all with
 * immediately = true) is final for the bar. (Lane TAIL-E named it
 * test_coof_immediate_close_tapes.cpp, the name lane TAIL-D's test of the
 * same fixture directory holds on the integrated tree, INT30.)
 *
 * The probe (tests/fixtures/coof_immediate_close, README.md there) closes its
 * position immediately at the close of every eighth bar and sends a new
 * market entry behind the close on the same bar; four bars later it sends the
 * entry first and the close after it. TradingView fills the entry sent behind
 * the close once, at the next bar's open, and never fills the one sent before
 * it while the position still held the pyramiding limit -- with and without
 * calc_on_order_fills, whose tapes are the same trades: no fill recalculation
 * follows such a close. Under calc_on_order_fills the engine recalculated
 * after it and released the entry at the close's own fill price; when the
 * recalculation closed again, the pair looped without end
 * (job-2660-pinecoderstasc-tasc-2026-06-one-percent-a-week-adaptive re-entered
 * at Friday's close where TradingView enters on Monday's open).
 *
 * Each case replays TradingView's tape of the probe on BINANCE:ETHUSDT.P 15
 * (the corpus-derived bars in bars.inc) through the Pine adapter, as the
 * probe's generated TU drives it, and requires every trade the tape closes --
 * entry and exit time, price in ticks, quantity, and the orders' names -- to
 * be the engine's.
 *
 * Fail-before: on 8988faff the calc_on_order_fills case does not return (the
 * close / entry recalculation loop).
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

#ifndef PINEFORGE_COOF_IMMEDIATE_CLOSE_FIXTURE_DIR
#error "PINEFORGE_COOF_IMMEDIATE_CLOSE_FIXTURE_DIR must name tests/fixtures/coof_immediate_close"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthImmediate: BINANCE:ETHUSDT.P 15m, 2025-04-01 00:00 .. 04-03 00:15 UTC.
#include "fixtures/coof_immediate_close/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr std::int64_t kMinute = 60'000;

long long ticks(double price) { return std::llround(price / kTick); }

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// A tape's "YYYY-MM-DD HH:MM", rendered at UTC+8, as UTC ms.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

// (entry ms, entry ticks, entry name, exit ms, exit ticks, exit name, qty)
using Row = std::tuple<std::int64_t, long long, std::string, std::int64_t, long long,
                       std::string, long long>;

std::vector<Row> tape_rows(const std::string& tape, std::int64_t end_ms, bool& ok) {
    std::ifstream in(std::string(PINEFORGE_COOF_IMMEDIATE_CLOSE_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    if (!in) { ok = false; return {}; }
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
        if (cell.size() < 6) { ok = false; continue; }
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = ticks(std::stod(cell[4]));
            std::get<2>(row) = cell[3];
            std::get<6>(row) = std::llround(std::stod(cell[5]));
        } else if (!cell[3].empty()) {  // not the range-end close of an open trade
            std::get<3>(row) = tape_ms(cell[2]);
            std::get<4>(row) = ticks(std::stod(cell[4]));
            std::get<5>(row) = cell[3];
            closed[number] = true;
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number)
        if (closed[number] && std::get<3>(row) < end_ms) out.push_back(row);
    if (out.empty()) ok = false;
    return out;
}

// The probe, as its generated TU drives the host (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(bool coof) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = coof;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 1;
        configure_pine_strategy(cfg);
    }

    void on_source_bar(const Bar&) override {
        const int k = pine_bar_index() % 8;
        const std::string tag = std::to_string(pine_bar_index());
        if (is_last_tick_ && k == 5) {
            if (signed_position_size() > 0.0)
                strategy_close("", "X" + tag, kNaN, kNaN, true);
            strategy_entry("A" + tag, true, kNaN, kNaN, kNaN, "A" + tag);
        }
        if (is_last_tick_ && k == 1) {
            strategy_entry("B" + tag, true, kNaN, kNaN, kNaN, "B" + tag);
            if (signed_position_size() > 0.0)
                strategy_close("", "Y" + tag, kNaN, kNaN, true);
        }
        if (k == 3 && signed_position_size() == 0.0)
            strategy_entry("C" + tag, true, kNaN, kNaN, kNaN, "C" + tag);
    }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthImmediate) {
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
        std::printf("    %-6s %lld @%lld %-4s -> %lld @%lld %-4s q=%lld\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r),
                    std::get<2>(r).c_str(), static_cast<long long>(std::get<3>(r)),
                    std::get<4>(r), std::get<5>(r).c_str(), std::get<6>(r));
}

void replay(const char* tape, bool coof) {
    const std::vector<Bar> bars = feed();
    const std::int64_t end_ms = bars.back().timestamp;
    bool ok = true;
    const auto expected = tape_rows(tape, end_ms, ok);
    CHECK(ok);
    ProbeHost host(coof);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        engine.emplace_back(t.entry_time, ticks(t.entry_price), t.entry_id, t.exit_time,
                            ticks(t.exit_price), t.exit_comment, std::llround(t.qty));
    }
    std::printf("  %-34s calc_on_order_fills=%d: TradingView %zu closed trades, engine %zu\n",
                tape, coof ? 1 : 0, expected.size(), engine.size());
    CHECK(engine == expected);
    if (engine != expected) {
        show("tape", expected);
        show("engine", engine);
    }
}

}  // namespace

int main() {
    std::printf("an immediate close the bar's calculation executes, against TradingView\n");
    replay("te-coof-immediate-reentry-eth15", true);
    replay("te-plain-immediate-reentry-eth15", false);
    // The two tapes are one set of trades: no fill recalculation follows the
    // close.
    bool ok = true;
    const std::int64_t end = feed().back().timestamp;
    CHECK(tape_rows("te-coof-immediate-reentry-eth15", end, ok)
          == tape_rows("te-plain-immediate-reentry-eth15", end, ok));
    CHECK(ok);
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
