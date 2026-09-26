/*
 * test_tv_strategy_defaults.cpp — lane TV-DEFAULTS.
 *
 * TradingView changed three Pine v6 strategy() defaults around
 * 2026-09-24T20:45Z. For a v6 script that omits them it now uses
 * initial_capital = 100000, default_qty_type = strategy.percent_of_equity and
 * default_qty_value = 100 -- 100 whatever the type: 100 contracts under
 * strategy.fixed, 100 of the account currency under strategy.cash -- where it
 * used 1000000 / strategy.fixed / 1. Pine v5 still uses the old three, and no
 * other strategy() default moved (the lane report holds the whole matrix:
 * every parameter, v5 and v6, four charts, omitted against given at its
 * documented default).
 *
 * Where the defaults live. They are Pine policy: what an omitted argument of
 * Pine's strategy() means, by language version. The kernel never learns them
 * (docs/adr/0001-kernel-adapter-boundary.md, boundary rules 1 and 2): a
 * default capital or order size has no meaning for a venue that states its
 * own account, and a kernel knob for it would put a hash-visible choice on
 * the public surface for a value every host already declares in its
 * NativeRunSpec (engine.hpp's initial_capital_ member default is overwritten
 * from the spec at every run begin). PineStrategyConfig's member defaults stay
 * the pre-change values, which are also Pine v5's and what every hand-built
 * source host relies on; the translation declares the v6 values: codegen's
 * constructor now emits all three for a script that omits them
 * (pineforge-codegen PINE_V6_STRATEGY_DEFAULTS).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/tv_strategy_defaults: a 10/30-bar SMA cross, long only,
 * entered from flat and closed on the opposite cross) through the Pine
 * adapter under the configuration the generated constructor declares for that
 * probe, over the BINANCE:ETHUSDT.P 15m bars embedded in bars.inc, and
 * requires every trade the tape closes inside those bars to be the engine's:
 * entry and exit time, side, price and quantity. The same probe under the
 * configuration the pre-lane constructor declared -- the host's own defaults
 * for the omitted parameters -- misses its tape. A probe that declares all
 * three gets the same configuration from both constructors.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

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

#ifndef PINEFORGE_TVD_FIXTURE_DIR
#error "PINEFORGE_TVD_FIXTURE_DIR must name tests/fixtures/tv_strategy_defaults"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/tv_strategy_defaults/bars.inc"

// TradingView's BINANCE:ETHUSDT.P quantity step: every tape quantity is on it.
constexpr double kLot = 0.0001;
// The bar before TradingView's first entry (2025-04-01 15:30 UTC), where its
// order was placed: the tapes' range opens at 2025-04-01 00:00 UTC and the
// harness trades from here, as run_strategy.py does for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1743520500000LL;  // 2025-04-01 15:15 UTC

// TradingView's BINANCE:ETHUSDT.P price tick. The engine snaps a fill to it
// by multiplication (182436 * 0.01 is 1824.3600000000001, the tape's text
// 1824.36 parses to 1824.3599999999999), so trades compare on the two grids.
constexpr double kTick = 0.01;
// The call-site token the generated TU passes to strategy_close for the probe's
// one close call ((line 8 << 32) | column 19 of fixtures/.../strategy.pine).
constexpr std::uint64_t kCloseCallsite = 34359738387ULL;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

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

// The tape's trades that close inside the replayed bars, in entry order.
std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_TVD_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
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
        // Trade number, Type, Date and time, Signal, Price USDT, Size (qty), ...
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
    std::vector<Row> rows;
    for (const auto& [number, row] : by_number)
        if (closed[number] && std::get<4>(row) < end_ms) rows.push_back(row);
    return rows;
}

// The probe, as the generated TU lowers it (fixtures/.../strategy.pine).
class SmaCrossHost final : public source::PineStrategyHost {
public:
    explicit SmaCrossHost(const source::PineStrategyConfig& config) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const bool advance = history_advances_new_bar();
        const double close = current_bar_.close;
        const double fast = advance ? fast_.compute(close) : fast_.recompute(close);
        const double slow = advance ? slow_.compute(close) : slow_.recompute(close);
        if (advance ? up_.compute(fast, slow) : up_.recompute(fast, slow))
            strategy_entry("L", true);
        if (advance ? down_.compute(fast, slow) : down_.recompute(fast, slow))
            strategy_close("L", "", std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), false, kCloseCallsite);
    }

private:
    ta::SMA fast_{10};
    ta::SMA slow_{30};
    ta::Crossover up_;
    ta::Crossunder down_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(const source::PineStrategyConfig& config, std::int64_t end_ms) {
    SmaCrossHost host(config);
    host.set_trade_start_time(kTradeStartMs);
    const std::vector<Bar> bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        // Each value must sit on its grid, then compares as a count of steps.
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                                t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

source::PineStrategyConfig config(double capital, QtyType type, double value) {
    source::PineStrategyConfig c{};
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(type);
    c.default_qty_value = value;
    return c;
}

struct Case {
    const char* tape;
    const char* declares;          // the probe's strategy() sizing arguments
    source::PineStrategyConfig lane;    // what the generated constructor declares now
    source::PineStrategyConfig legacy;  // what it declared before: host defaults for the rest
    std::size_t closed;            // tape trades closed inside the bars
};

bool same_sizing(const source::PineStrategyConfig& a, const source::PineStrategyConfig& b) {
    return a.initial_capital == b.initial_capital && a.default_qty_type == b.default_qty_type
        && a.default_qty_value == b.default_qty_value;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    constexpr QtyType kPercent = QtyType::PERCENT_OF_EQUITY;
    constexpr QtyType kFixed = QtyType::FIXED;
    constexpr QtyType kCash = QtyType::CASH;

    // The host's own defaults are the pre-change values, unchanged by design.
    const source::PineStrategyConfig host{};
    std::printf("-- PineStrategyConfig defaults stay 1000000 / fixed / 1\n");
    CHECK(host.initial_capital == 1000000.0);
    CHECK(host.default_qty_type == static_cast<int>(kFixed));
    CHECK(host.default_qty_value == 1.0);

    const Case cases[] = {
        {"tvd-all-omit-v6-eth15", "nothing",
         config(100000.0, kPercent, 100.0), config(1000000.0, kFixed, 1.0), 6},
        {"tvd-cap-omit-v6-eth15", "percent_of_equity, 100",
         config(100000.0, kPercent, 100.0), config(1000000.0, kPercent, 100.0), 6},
        {"tvd-qty-value1-v6-eth15", "1000000, value 1",
         config(1000000.0, kPercent, 1.0), config(1000000.0, kFixed, 1.0), 8},
        {"tvd-qty-typefixed-v6-eth15", "1000000, fixed",
         config(1000000.0, kFixed, 100.0), config(1000000.0, kFixed, 1.0), 8},
        {"tvd-qty-typecash-v6-eth15", "1000000, cash",
         config(1000000.0, kCash, 100.0), config(1000000.0, kCash, 1.0), 8},
        {"tvd-qty-fixed1-v6-eth15", "1000000, fixed, 1",
         config(1000000.0, kFixed, 1.0), config(1000000.0, kFixed, 1.0), 8},
        {"tvd-cap-x1m-v6-eth15", "percent_of_equity, 100, 1000000",
         config(1000000.0, kPercent, 100.0), config(1000000.0, kPercent, 100.0), 6},
    };

    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const std::vector<Row> tape = tape_trades(c.tape, end_ms);
        CHECK(tape.size() == c.closed);

        const Run lane = run(c.lane, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape);
        if (lane.trades != tape) {
            show("tape", tape);
            show("engine", lane.trades);
        }

        const Run legacy = run(c.legacy, end_ms);
        CHECK(legacy.error.empty());
        if (same_sizing(c.lane, c.legacy)) {
            // A probe that declares every sizing argument: unchanged.
            CHECK(legacy.trades == lane.trades);
        } else {
            // The host's own defaults are not TradingView's v6 defaults.
            CHECK(legacy.trades != tape);
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
