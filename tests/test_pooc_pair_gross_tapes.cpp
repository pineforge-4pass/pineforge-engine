/*
 * test_pooc_pair_gross_tapes.cpp -- R5 lane TAIL-D.
 *
 * A flat script that places two opposite fixed-quantity market entries on
 * one bar keeps only the earlier call on TradingView when the pair's gross
 * is over the equity at full margin; a pair that fits keeps its two-leg
 * shape. The adapter's gross decline (apply_terminal_explicit_market_policy)
 * held only under calc_on_order_fills, without slippage and without
 * commission; under process_orders_on_close alone, or with costs, the later
 * call became a full reversal (officialjackofalltrades-helios on OANDA:EURUSD
 * 1D: slippage 1, a 0.01 % commission, two engine-only reversals).
 *
 * The lane's own synthetic scripts (tests/fixtures/pooc_pair_gross, one `lab
 * tv --no-note` export each, OANDA:EURUSD 1D, 2025-04-01 .. 2026-05-01,
 * process_orders_on_close on, calc_on_order_fills off, pyramiding 0, an
 * initial capital of 100000): on a 12-bar cycle a flat book places a pair,
 * each leg 80 % of the equity (O: the gross 160 % is over it) or 30 % (U:
 * 60 %, within it), the long call first (LF) or the short (SF), and
 * close_all two bars later --
 *   -h1 with slippage 1 and a 0.01 % commission (the probe's costs);
 *   -h2 without costs.
 * Each row replays TradingView's own tape through the Pine adapter over the
 * tape's bars (bars.inc) and requires every trade the tape closes inside
 * those bars to be the engine's: entry and exit time, side, price in ticks
 * and quantity.
 *
 * Fail-before (lane report): every O pair books a reversal (LF) or a
 * double-size buy (SF), 188 engine rows against 140.
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

#ifndef PINEFORGE_TAIL_D_PAIR_GROSS_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_PAIR_GROSS_FIXTURE_DIR must name tests/fixtures/pooc_pair_gross"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/pooc_pair_gross/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.00001;  // OANDA:EURUSD
constexpr double kLot = 0.01;

// (entry ms, long, entry ticks, lots, exit ms, exit ticks)
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }

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
    std::ifstream in(std::string(PINEFORGE_TAIL_D_PAIR_GROSS_FIXTURE_DIR) + "/" + tape
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
            std::get<3>(row) = lots(std::stod(cell[5]));
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

struct Variant {
    const char* tape;
    bool costs;          // slippage 1 and a 0.01 % commission
    std::size_t closed;  // tape trades closed inside the bars
};

// The probes' bodies (tests/fixtures/pooc_pair_gross/<slug>/strategy.pine).
class PairGrossHost final : public source::PineStrategyHost {
public:
    explicit PairGrossHost(const Variant& v) {
        attach_pine_execution_adapter();
        set_syminfo_session("1700-1700");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.process_orders_on_close = true;
        c.pyramiding = 0;
        c.initial_capital = 100000.0;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 100.0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        if (v.costs) {
            c.slippage = 1;
            c.commission_type = static_cast<int>(CommissionType::PERCENT);
            c.commission_value = 0.01;
        }
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kLot);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar& bar) override {
        const int phase = bar_index_ % 12;
        const bool flat = signed_position_size() == 0.0;
        // strategy.equity of a flat book.
        const double equity = current_equity();
        const double q_over = std::floor(0.8 * equity / bar.close * 100.0) / 100.0;
        const double q_within = std::floor(0.3 * equity / bar.close * 100.0) / 100.0;
        const auto pair = [this](bool long_first, double qty, const char* tag) {
            const std::string first = std::string(tag) + (long_first ? "-LF-L" : "-SF-S");
            const std::string second = std::string(tag) + (long_first ? "-LF-S" : "-SF-L");
            strategy_entry(long_first ? "L" : "S", long_first, kNaN, kNaN, qty, first);
            strategy_entry(long_first ? "S" : "L", !long_first, kNaN, kNaN, qty, second);
        };
        if (flat && phase == 1) pair(true, q_over, "O");
        if (flat && phase == 4) pair(false, q_over, "O");
        if (flat && phase == 7) pair(true, q_within, "U");
        if (flat && phase == 10) pair(false, q_within, "U");
        if (phase == 3 || phase == 6 || phase == 9 || phase == 0)
            strategy_close("", "X", kNaN, kNaN, false);
    }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEur1d) {
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
    PairGrossHost host(v);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "1D", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
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
        {"hel-h1-pooc-pair-gross-slip", true, 139},
        {"hel-h2-pooc-pair-gross-zero", false, 139},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
