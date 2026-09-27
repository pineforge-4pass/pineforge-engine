/*
 * test_pooc_margin_call_sizing_tapes.cpp -- lane W4-ENG-POOC-SAMEPASS: a
 * default-quantity entry sizes on the equity a margin call of its bar leaves.
 *
 * A short held at 100 % of equity is margin-called on a bar whose high is
 * above its close; on that bar's calculation the script reverses it with
 * strategy.entry of a long (cell C). Under process_orders_on_close TradingView
 * books the margin call first and sizes the long on the equity after it
 * (53.8573 at 1787.58). The engine froze the core's quotient of the entry at
 * the command, on the equity before the call, and the margin call's sizing
 * revision never reached it: the stale units no longer fit the equity, and the
 * affordability gate dropped the long, leaving only the short's close.
 *
 * Each row replays TradingView's own tape of the lane's synthetic probe
 * (tests/fixtures/pooc_margin_call_sizing, lab tv exports on BINANCE:ETHUSDT.P
 * 15; README.md names each one) through the Pine adapter under the
 * configuration its generated constructor declares, and requires every trade
 * the tape closes inside the replayed bars -- entry and exit time, side, price
 * in ticks, quantity in lots, margin-call slices included -- to be the
 * engine's.
 *
 * Fail-before: see the lane report (w4-mcs-pooc: cell C's long is missing).
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>
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

#ifndef PINEFORGE_W4_MC_SIZING_FIXTURE_DIR
#error "PINEFORGE_W4_MC_SIZING_FIXTURE_DIR must name tests/fixtures/pooc_margin_call_sizing"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthMcs: BINANCE:ETHUSDT.P 15m, 2025-04-03 00:00 .. 23:45 UTC.
#include "fixtures/pooc_margin_call_sizing/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kLot = 0.0001;
constexpr std::int64_t kMinute = 60'000;

// (entry ms, long, entry ticks, exit ms, exit ticks, lots)
using Row = std::tuple<std::int64_t, bool, long long, std::int64_t, long long, long long>;

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

std::int64_t at(int hour, int minute) {
    return ((days_from_civil(2025, 4, 3) * 24 + hour) * 60 + minute) * kMinute;
}

std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct TapeTrade {
    Row row;
    std::string entry_signal, exit_signal;
    double net_pnl = 0.0;
};

std::vector<TapeTrade> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_W4_MC_SIZING_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, TapeTrade> by_number;
    std::set<int> closed;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 8) continue;
        const int number = std::stoi(cell[0]);
        TapeTrade& t = by_number[number];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(t.row) = tape_ms(cell[2]);
            std::get<1>(t.row) = cell[1] == "Entry long";
            std::get<2>(t.row) = ticks(price);
            std::get<5>(t.row) = lots(std::stod(cell[5]));
            t.entry_signal = cell[3];
        } else {
            std::get<3>(t.row) = tape_ms(cell[2]);
            std::get<4>(t.row) = ticks(price);
            t.exit_signal = cell[3];
            t.net_pnl = std::stod(cell[7]);
            closed.insert(number);
        }
    }
    std::vector<TapeTrade> out;
    for (const auto& [number, t] : by_number)
        if (closed.count(number) && std::get<3>(t.row) < end_ms) out.push_back(t);
    return out;
}

struct Case {
    const char* tape;
    bool pooc, coof;
};

// The probe, as its generated TU lowers it (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const Case& c) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = c.pooc;
        cfg.calc_on_order_fills = c.coof;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 100.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        if (t == at(4, 0)) entry("A-S", false, "A short");
        if (t == at(4, 45)) {
            strategy_close("A-S", "A close", kNaN, kNaN, false, 55834574867ULL);
            entry("A-L", true, "A long");
        }
        if (t == at(12, 45)) entry("B-S", false, "B short");
        if (t == at(13, 30)) {
            strategy_close("B-S", "B close", kNaN, kNaN, false, 81604378643ULL);
            entry("B-L", true, "B long");
        }
        if (t == at(17, 45)) entry("C-S", false, "C short");
        if (t == at(18, 45)) entry("C-L", true, "C long");
        if (t == at(6, 45) || t == at(15, 30) || t == at(20, 45)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

private:
    void entry(const char* id, bool is_long, const char* comment) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, comment);
    }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthMcs) {
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
        std::printf("    %-7s %lld %s @%lld -> %lld @%lld  %lld lots\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), static_cast<long long>(std::get<3>(r)), std::get<4>(r),
                    std::get<5>(r));
}

std::map<std::string, std::vector<TapeTrade>> tapes;

void replay(const Case& c) {
    std::printf("-- %s\n", c.tape);
    const std::vector<Bar> bars = feed();
    const std::int64_t end_ms = bars.back().timestamp;
    const auto tape = tape_trades(c.tape, end_ms);
    tapes[c.tape] = tape;
    CHECK(!tape.empty());

    ProbeHost host(c);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> expected, engine;
    for (const TapeTrade& t : tape) expected.push_back(t.row);
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), t.exit_time,
                            ticks(t.exit_price), lots(t.qty));
    }
    CHECK(engine == expected);
    if (engine != expected) {
        show("tape", expected);
        show("engine", engine);
    }
}

}  // namespace

int main() {
    const Case cases[] = {
        {"w4-mcs-pooc", true, false},
        {"w4-mcs-pooc-coof", true, true},
        {"w4-mcs-plain", false, false},
        {"w4-mcs-plain-coof", false, true},
    };
    for (const Case& c : cases) replay(c);

    std::printf("-- the rule, on the tapes\n");
    {
        // Cell C under process_orders_on_close: the margin call at the bar's
        // high (1798.57) fills before the reversal, and the long takes the
        // equity it leaves -- the initial capital and every net profit booked
        // through that close -- over the close 1787.58, floored to the lot.
        const auto& t = tapes["w4-mcs-pooc"];
        const std::int64_t flip = at(18, 45);
        double equity = 100000.0;
        long long long_lots = -1, call_lots = 0;
        long long call_ticks = 0;
        for (const TapeTrade& r : t) {
            if (std::get<3>(r.row) <= flip) equity += r.net_pnl;
            if (r.entry_signal == "C long") long_lots = std::get<5>(r.row);
            if (r.exit_signal == "Margin call" && std::get<3>(r.row) == flip) {
                call_lots = std::get<5>(r.row);
                call_ticks = std::get<4>(r.row);
            }
        }
        const double close = 1787.58;
        CHECK(call_lots > 0 && call_ticks == ticks(1798.57));
        CHECK(long_lots == static_cast<long long>(std::floor(equity / close / kLot + 1e-9)));
        // Marked at the close instead, the called slice's equity is larger
        // by its lots times the gap between the call and the close: a larger
        // long than TradingView's.
        const double before_call = equity + call_lots * kLot * (1798.57 - close);
        CHECK(static_cast<long long>(std::floor(before_call / close / kLot)) > long_lots);
    }

    std::printf("\n%s margin-call sizing tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
