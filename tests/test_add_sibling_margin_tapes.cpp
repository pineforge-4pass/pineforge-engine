/*
 * test_add_sibling_margin_tapes.cpp — lane TAIL-B.
 *
 * Several MARKET entries a bar places on top of a held position of their own
 * side (a grid bot's adds), read off TradingView's own tapes of synthetic
 * probes written for this lane:
 *
 *   AS  Each add is judged against the position held when it was placed plus
 *       its own units, not against a sibling filled a moment earlier at the
 *       same point: each that fits beside the held position alone fills,
 *       under process_orders_on_close at the close as at the next open.
 *   AC  The grown book is margin-called once, after the last add has filled,
 *       four times its lot-floored shortfall: at that fill for a market add
 *       (a commissioned explicit short at the print its fill slipped from),
 *       under process_orders_on_close at the next open, sized at the close.
 *
 * Each row replays one lab tv tape (tests/fixtures/add_sibling_margin,
 * BINANCE:ETHUSDT.P 15) through the Pine adapter under the configuration the
 * generated constructor declares for its probe, over the corpus 15m bars of
 * lane W5-ENG-MARGIN-V6 (tests/fixtures/margin_v6/bars.inc), and requires
 * every trade the tape closes inside those bars to be the engine's: entry and
 * exit time, side, price in ticks of 0.01 and quantity in lots of 0.0001. On
 * the commit before a rule's, every row of that rule fails here.
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

#ifndef PINEFORGE_ADD_SIBLING_FIXTURE_DIR
#error "PINEFORGE_ADD_SIBLING_FIXTURE_DIR must name tests/fixtures/add_sibling_margin"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/margin_v6/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;     // BINANCE:ETHUSDT.P's price tick
constexpr double kLot = 0.0001;    // and TradingView's quantity step there
constexpr std::int64_t kMinute = 60'000;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
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

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_ADD_SIBLING_FIXTURE_DIR) + "/" + tape
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
            CHECK(on_grid(qty, kLot));
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = lots(qty);
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
    Cycle,  // tailb-adds-*: every 3 hours a seed, then 1, 2, 3 or 5 adds
    Loss,   // tailb-loss-*: adds on top of a seed the price has moved away from
};

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config, bool is_long)
        : probe_(probe), long_(is_long) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::Cycle: cycle(t); break;
        case Probe::Loss: loss(t); break;
        }
    }

private:
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }
    // qOf(money): math.floor(money / close * 10000) / 10000
    double q_of(double money) const {
        return std::floor(money / current_bar_.close * 10000.0) / 10000.0;
    }
    void entry(const std::string& id, double qty) {
        strategy_entry(id, long_, kNaN, kNaN, qty, id);
    }
    // addN(n): n adds A1 .. An of 700 each, on one bar.
    void adds(int n) {
        const double q = q_of(700.0);
        for (int i = 1; i <= n; ++i) entry("A" + std::to_string(i), q);
    }

    // tailb-adds-*: every 3 hours from 2025-04-02 00:00 UTC, forty cells. From
    // flat a seed of 9000 (the fifth kind: two seeds of 4500, a bar apart);
    // two bars later 1, 2, 3 or 5 adds (the fifth kind: 3), placed on one bar;
    // flattened 75 minutes into the cell.
    void cycle(std::int64_t t) {
        const std::int64_t t0 = at(2, 0, 0);
        const std::int64_t step = 180 * kMinute;
        const std::int64_t rel = t - t0;
        if (rel < 0 || rel >= 40 * step) return;
        const std::int64_t phase = rel % step;
        const int kind = static_cast<int>((rel / step) % 5);
        const int n = kind == 0 ? 1 : kind == 1 ? 2 : kind == 2 ? 3 : kind == 3 ? 5 : 3;
        if (phase == 0 && signed_position_size() == 0.0) {
            if (kind == 4) entry("S1", q_of(4500.0));
            else entry("S", q_of(9000.0));
        }
        if (phase == 15 * kMinute && kind == 4) entry("S2", q_of(4500.0));
        if (phase == 30 * kMinute) adds(n);
        if (phase == 75 * kMinute) close_all("cleanup");
    }

    // tailb-loss-*: fixed cells; a seed, then (after the price has moved)
    // several adds on one bar; flattened after each cell.
    void loss(std::int64_t t) {
        if (t == at(6, 6, 0) || t == at(8, 12, 0) || t == at(11, 6, 0)) entry("S", q_of(9000.0));
        if (t == at(10, 0, 0)) entry("S1", q_of(2250.0));
        if (t == at(10, 0, 15)) entry("S2", q_of(2250.0));
        if (t == at(10, 0, 30)) entry("S3", q_of(2250.0));
        if (t == at(10, 0, 45)) entry("S4", q_of(2250.0));
        if (t == at(7, 6, 0) || t == at(10, 18, 0) || t == at(11, 6, 30)) adds(3);
        if (t == at(9, 6, 0)) adds(5);
        if (t == at(7, 7, 0) || t == at(9, 7, 0) || t == at(10, 19, 0) || t == at(11, 7, 30))
            close_all("cleanup");
    }

    Probe probe_;
    bool long_;
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

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, const source::PineStrategyConfig& config, bool is_long,
        const std::vector<Bar>& bars, std::int64_t end_ms) {
    ProbeHost host(probe, config, is_long);
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

// What a probe's strategy() declares: pyramiding 200, 10000 of capital,
// margins of 100, a percent commission and a slippage in ticks.
source::PineStrategyConfig config(bool pooc, double commission, int slippage) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = pooc;
    c.initial_capital = 10000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 200;
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
    bool is_long;
    std::size_t closed;               // tape trades closed inside the bars
};

}  // namespace

int main() {
    const std::vector<Bar> bars = feed();
    const Case cases[] = {
        {"AS AC", "tailb-adds-pooc-c6s3", Probe::Cycle, config(true, 0.06, 3), true, 109},
        {"AS AC", "tailb-adds-pooc-c10s3", Probe::Cycle, config(true, 0.1, 3), true, 97},
        {"AS AC", "tailb-adds-pooc-c0s0", Probe::Cycle, config(true, 0.0, 0), true, 152},
        {"AS AC", "tailb-adds-pooc-c6s3-short", Probe::Cycle, config(true, 0.06, 3), false, 195},
        {"AS AC", "tailb-adds-mkt-c6s3", Probe::Cycle, config(false, 0.06, 3), true, 109},
        {"AS AC", "tailb-adds-mkt-c10s2", Probe::Cycle, config(false, 0.1, 2), true, 97},
        {"AS AC", "tailb-adds-mkt-c0s0", Probe::Cycle, config(false, 0.0, 0), true, 152},
        {"AS AC", "tailb-adds-mkt-c6s3-short", Probe::Cycle, config(false, 0.06, 3), false, 195},
        {"AS AC", "tailb-loss-pooc-c6s3", Probe::Loss, config(true, 0.06, 3), true, 20},
        {"AS AC", "tailb-loss-mkt-c10s2", Probe::Loss, config(false, 0.1, 2), true, 20},
    };

    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.rule, c.tape);
        const std::int64_t end = bars.back().timestamp;
        const std::vector<Row> tape = tape_trades(c.tape, end);
        CHECK(tape.size() == c.closed);
        const Run lane = run(c.probe, c.lane, c.is_long, bars, end);
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
