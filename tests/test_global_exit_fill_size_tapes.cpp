/*
 * test_global_exit_fill_size_tapes.cpp -- lane W4-ENG-POOC-SAMEPASS, rule F19.
 *
 * A strategy.exit with from_entry "" and no quantity closes its percentage of
 * the position it FILLS against: TradingView books the lots a market add
 * opened after the exit was placed with it -- at the same open the exit fills
 * at, on a later bar, at a process_orders_on_close close -- whether the script
 * placed the exit before or after the add; at a shared price point the market
 * add goes first. An exit that names its entry closes that entry's lots only.
 * With or without process_orders_on_close and calc_on_order_fills.
 *
 * The engine sized such an exit to the position it was placed under (outside
 * one process_orders_on_close same-bar population), and at a shared open filled
 * it ahead of an add the adapter had batched behind it, so the add's lot
 * survived the take-profit and exited a bar or more later (job-2388's DCA
 * take-profit on its entry bar).
 *
 * Each row replays TradingView's own tape of the lane's synthetic probe
 * (tests/fixtures/global_exit_fill_size, lab tv exports on BINANCE:ETHUSDT.P
 * 15; README.md names each one) through the Pine adapter under the
 * configuration its generated constructor declares, and requires every trade
 * the tape closes inside the replayed bars -- entry and exit time, side, price
 * in ticks, quantity in lots -- to be the engine's. TradingView reports a
 * close that spans two entries' shares as one row per share (cell E: two rows
 * of 1 from the first lot); the comparison sums rows that share their entry
 * and exit fill. Under calc_on_order_fills the probe's cells E and F refill
 * their entry on the recalculations of its bar (a cascade outside this rule)
 * and are left out of the comparison on w4-f19a-plain-coof.
 *
 * Fail-before: see the lane report (every take-profit cell but D closes the
 * first lot only on every tape).
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
#include <iterator>
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

#ifndef PINEFORGE_W4_GLOBAL_EXIT_FIXTURE_DIR
#error "PINEFORGE_W4_GLOBAL_EXIT_FIXTURE_DIR must name tests/fixtures/global_exit_fill_size"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthF19: BINANCE:ETHUSDT.P 15m, 2025-04-10 00:00 .. 2025-04-11 23:45 UTC.
#include "fixtures/global_exit_fill_size/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kLot = 0.0001;  // TradingView's BINANCE:ETHUSDT.P quantity step
constexpr std::int64_t kMinute = 60'000;

// (entry ms, long, entry price ticks, exit ms, exit price ticks) -> lots.
using Fill = std::tuple<std::int64_t, bool, long long, std::int64_t, long long>;
using Rows = std::map<Fill, long long>;

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

// timestamp("UTC", 2025, 4, 11, hour, minute), the probe's cells.
std::int64_t at(int hour, int minute) {
    return ((days_from_civil(2025, 4, 11) * 24 + hour) * 60 + minute) * kMinute;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct TapeTrade {
    Fill fill;
    long long lots = 0;
    std::string entry_signal, exit_signal;
};

std::vector<TapeTrade> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_W4_GLOBAL_EXIT_FIXTURE_DIR) + "/" + tape
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
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        TapeTrade& t = by_number[number];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(t.fill) = tape_ms(cell[2]);
            std::get<1>(t.fill) = cell[1] == "Entry long";
            std::get<2>(t.fill) = ticks(price);
            t.lots = lots(std::stod(cell[5]));
            t.entry_signal = cell[3];
        } else {
            std::get<3>(t.fill) = tape_ms(cell[2]);
            std::get<4>(t.fill) = ticks(price);
            t.exit_signal = cell[3];
            closed.insert(number);
        }
    }
    std::vector<TapeTrade> out;
    for (const auto& [number, t] : by_number)
        if (closed.count(number) && std::get<3>(t.fill) < end_ms) out.push_back(t);
    return out;
}

struct Case {
    const char* tape;
    bool pooc, coof;
    std::set<std::string> outside;  // entry signals left out (see the header)
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
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 5;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double close = current_bar_.close;
        if (t == at(0, 0)) entry("A1");
        if (t == at(0, 30)) {
            entry("A2");
            tp("A-TP", "", close - 3, kNaN, 100.0, "A tp");
        }
        if (t == at(2, 0)) entry("B1");
        if (t == at(2, 30)) {
            tp("B-TP", "", close - 3, kNaN, 100.0, "B tp");
            entry("B2");
        }
        if (t == at(5, 0)) entry("C1");
        if (t == at(5, 30)) {
            entry("C2");
            tp("C-TP", "", close + 2, kNaN, 100.0, "C tp");
        }
        if (t == at(7, 0)) entry("D1");
        if (t == at(7, 30)) {
            entry("D2");
            tp("D-TP", "D1", close - 3, kNaN, 100.0, "D tp");
        }
        if (t == at(9, 0)) strategy_entry("E1", true, kNaN, kNaN, 2, "E1", "", 0, -1);
        if (t == at(9, 30)) {
            strategy_entry("E2", true, kNaN, kNaN, 2, "E2", "", 0, -1);
            tp("E-TP", "", close - 3, kNaN, 50, "E tp");
        }
        if (t == at(11, 0)) entry("F1");
        if (t == at(11, 15)) tp("F-TP", "", close + 12, kNaN, 100.0, "F tp");
        if (t == at(11, 45)) entry("F2");
        if (t == at(14, 0)) entry("G1");
        if (t == at(14, 30)) {
            entry("G2");
            tp("G-SL", "", kNaN, close + 3, 100.0, "G sl");
        }
        for (const auto& [h, m] : {std::pair<int, int>{1, 45}, {3, 45}, {6, 45}, {8, 45},
                                   {10, 45}, {13, 45}, {15, 45}}) {
            if (t == at(h, m)) {
                strategy_cancel_all();
                strategy_close("", "cleanup", kNaN, kNaN, false);
            }
        }
    }

private:
    void entry(const char* id) { strategy_entry(id, true, kNaN, kNaN, kNaN, id); }
    void tp(const char* id, const char* from, double limit, double stop, double percent,
            const char* comment) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, percent, comment, kNaN, "",
                      kNaN, kNaN);
    }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthF19) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

void show(const char* tag, const Rows& rows) {
    for (const auto& [f, q] : rows)
        std::printf("    %-7s %lld %s @%lld -> %lld @%lld  %lld lots\n", tag,
                    static_cast<long long>(std::get<0>(f)), std::get<1>(f) ? "long" : "short",
                    std::get<2>(f), static_cast<long long>(std::get<3>(f)), std::get<4>(f), q);
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
    host.set_trade_start_time(at(0, 0));
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());

    std::set<std::int64_t> skipped;  // entry fills of the cells left out
    Rows expected, engine;
    for (const TapeTrade& t : tape) {
        if (c.outside.count(t.entry_signal)) {
            skipped.insert(std::get<0>(t.fill));
            continue;
        }
        expected[t.fill] += t.lots;
    }
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms || skipped.count(t.entry_time)) continue;
        engine[{t.entry_time, t.is_long, ticks(t.entry_price), t.exit_time,
                ticks(t.exit_price)}] += lots(t.qty);
    }
    CHECK(engine == expected);
    if (engine != expected) {
        show("tape", expected);
        show("engine", engine);
    }
}

// The lots the tape closes by `exit_signal` at `exit_ms`.
long long closed_lots(const std::vector<TapeTrade>& tape, const char* exit_signal,
                      std::int64_t exit_ms) {
    long long sum = 0;
    for (const TapeTrade& t : tape)
        if (t.exit_signal == exit_signal && std::get<3>(t.fill) == exit_ms) sum += t.lots;
    return sum;
}

}  // namespace

int main() {
    const Case cases[] = {
        {"w4-f19a-plain", false, false, {}},
        {"w4-f19a-plain-coof", false, true, {"E1", "E2", "F1", "F2"}},
        {"w4-f19a-pooc", true, false, {}},
        {"w4-f19a-pooc-coof", true, true, {}},
    };
    for (const Case& c : cases) replay(c);

    std::printf("-- the rule, on the tapes\n");
    const long long one = lots(1.0);
    {
        // Without process_orders_on_close the exits fill at the open after
        // their placement (cell C on the limit the bar then reaches, cell F on
        // a later bar) and close both lots; the named exit only its entry's.
        const auto& t = tapes["w4-f19a-plain"];
        CHECK(closed_lots(t, "A tp", at(0, 45)) == 2 * one);
        CHECK(closed_lots(t, "B tp", at(2, 45)) == 2 * one);
        CHECK(closed_lots(t, "C tp", at(5, 45)) == 2 * one);
        CHECK(closed_lots(t, "D tp", at(7, 45)) == one);
        CHECK(closed_lots(t, "E tp", at(9, 45)) == 2 * one);  // 50 percent of 4
        CHECK(closed_lots(t, "F tp", at(12, 30)) == 2 * one);
        CHECK(closed_lots(t, "G sl", at(14, 45)) == 2 * one);
    }
    {
        // Under process_orders_on_close the add fills at the close first and
        // the exit, through there, closes both lots at that close.
        const auto& t = tapes["w4-f19a-pooc"];
        CHECK(closed_lots(t, "A tp", at(0, 30)) == 2 * one);
        CHECK(closed_lots(t, "B tp", at(2, 30)) == 2 * one);
        CHECK(closed_lots(t, "G sl", at(14, 30)) == 2 * one);
    }
    {
        // calc_on_order_fills changes nothing under process_orders_on_close.
        std::ifstream a(std::string(PINEFORGE_W4_GLOBAL_EXIT_FIXTURE_DIR) + "/w4-f19a-pooc/tv_trades.csv");
        std::ifstream b(std::string(PINEFORGE_W4_GLOBAL_EXIT_FIXTURE_DIR) + "/w4-f19a-pooc-coof/tv_trades.csv");
        const std::string sa((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
        const std::string sb((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
        CHECK(!sa.empty() && sa == sb);
    }

    std::printf("\n%s from_entry \"\" exit fill-size tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
