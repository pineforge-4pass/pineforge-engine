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
 *
 * Each row replays one lab tv tape (tests/fixtures/margin_residual) through
 * the Pine adapter under the configuration the generated constructor declares
 * for its probe, over the corpus 15m bars of lane W5-ENG-MARGIN-V6
 * (tests/fixtures/margin_v6/bars.inc, BINANCE:ETHUSDT.P), and requires every
 * trade the tape closes inside those bars to be the engine's: entry and exit
 * time, side, price in ticks and quantity in lots. On the lane's base every
 * rule row fails here, and every control row beside it passes.
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

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;       // BINANCE:ETHUSDT.P's price tick
constexpr double kEthLot = 0.0001;   // TradingView's BINANCE:ETHUSDT.P quantity step
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
};

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config, double lot)
        : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", lot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::SameBarExplicit60: same_bar_explicit(t, 0.6); break;
        case Probe::SameBarExplicit40: same_bar_explicit(t, 0.4); break;
        case Probe::SameBarDefault3: same_bar_default(t); break;
        }
    }

private:
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }

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
source::PineStrategyConfig config(bool pooc, int pyramiding) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = pooc;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = pyramiding;
    return c;
}

struct Case {
    const char* rule;
    const char* tape;
    Probe probe;
    source::PineStrategyConfig lane;  // what its generated constructor declares
    std::size_t closed;               // tape trades closed inside the bars
};

}  // namespace

int main() {
    const std::vector<Bar> eth = feed(kEth15);
    const std::int64_t eth_end = eth.back().timestamp;

    const Case cases[] = {
        {"SB", "w5b-sb-market-60x2", Probe::SameBarExplicit60, config(false, 2), 120},
        {"SB", "w5b-sb-pooc-60x2", Probe::SameBarExplicit60, config(true, 2), 120},
        {"SB", "w5b-sb-default-x3", Probe::SameBarDefault3, config(false, 3), 48},
        {"SB", "w5b-sb-pooc-default-x3", Probe::SameBarDefault3, config(true, 3), 60},
        {"SB control", "w5b-sb-market-40x2", Probe::SameBarExplicit40, config(false, 2), 80},
    };

    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.rule, c.tape);
        const std::vector<Row> tape = tape_trades(c.tape, eth_end, kEthLot);
        CHECK(tape.size() == c.closed);
        const Run lane = run(c.probe, c.lane, eth, kEthLot, eth_end);
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
