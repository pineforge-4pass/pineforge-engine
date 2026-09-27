/*
 * test_coof_reissued_exit_tapes.cpp -- lane W4-ENG-POOC-SAMEPASS: under
 * calc_on_order_fills an exit re-issued unchanged keeps resting at its level.
 *
 * A short's exit limit, placed in the recalculation of the short's open fill
 * and re-issued on every calculation while short, is re-issued unchanged by
 * the recalculation of a later fill on the same bar whose remaining leg
 * crosses its level: a margin call at the bar's high (cell MC) or a sell-stop
 * add on the way down (cell ADD). TradingView fills it at its own level where
 * the path crosses it. The engine treated the re-issue as an exit born in that
 * recalculation and held it on the rest of the leg, gap-filling it at the
 * bar's low. An exit the add's recalculation places for the first time
 * (cell NEW) is held there by TradingView too, and fills at the low.
 *
 * Each row replays TradingView's own tape of the lane's synthetic probe
 * (tests/fixtures/coof_reissued_exit, lab tv exports on BINANCE:ETHUSDT.P 15;
 * README.md names each one) through the Pine adapter under the configuration
 * its generated constructor declares, and requires every trade the tape
 * closes inside the replayed bars -- entry and exit time, side, price in
 * ticks, quantity -- to be the engine's.
 *
 * Fail-before: see the lane report (w4-rex-coof: cells MC and ADD exit at the
 * bar's low).
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

#ifndef PINEFORGE_W4_REISSUED_EXIT_FIXTURE_DIR
#error "PINEFORGE_W4_REISSUED_EXIT_FIXTURE_DIR must name tests/fixtures/coof_reissued_exit"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthRex: BINANCE:ETHUSDT.P 15m, 2025-04-10 12:00 .. 2025-04-16 07:00 UTC.
#include "fixtures/coof_reissued_exit/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr std::int64_t kMinute = 60'000;

// (entry ms, long, entry ticks, exit ms, exit ticks, qty x 1e4)
using Row = std::tuple<std::int64_t, bool, long long, std::int64_t, long long, long long>;

long long ticks(double price) { return std::llround(price / kTick); }

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * kMinute;
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
};

std::vector<TapeTrade> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_W4_REISSUED_EXIT_FIXTURE_DIR) + "/" + tape
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
            std::get<0>(t.row) = tape_ms(cell[2]);
            std::get<1>(t.row) = cell[1] == "Entry long";
            std::get<2>(t.row) = ticks(price);
            std::get<5>(t.row) = std::llround(std::stod(cell[5]) * 1e4);
            t.entry_signal = cell[3];
        } else {
            std::get<3>(t.row) = tape_ms(cell[2]);
            std::get<4>(t.row) = ticks(price);
            t.exit_signal = cell[3];
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
        cfg.pyramiding = 2;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", 0.0001);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double size = signed_position_size();
        const auto on = [t](unsigned d, int h, int m) {
            return t >= at(d, h, m) && t < at(d, h, m) + 15 * kMinute;
        };
        if (t == at(10, 13, 15))
            strategy_entry("MC-S", false, kNaN, kNaN, kNaN, "MC short");
        if (on(10, 13, 30) && size < 0.0)
            strategy_exit("MC-X", "MC-S", 1575.0, kNaN, kNaN, kNaN, kNaN, 100.0, "MC exit", kNaN,
                          "", kNaN, kNaN);
        if (t == at(13, 19, 15)) {
            strategy_entry("ADD-S1", false, kNaN, kNaN, 1, "ADD short", "", 0, -1);
            strategy_entry("ADD-S2", false, kNaN, 1600.0, 1, "ADD add", "", 0, -1);
        }
        if (on(13, 19, 30) && size < 0.0)
            strategy_exit("ADD-X", "ADD-S1", 1580.0, kNaN, kNaN, kNaN, kNaN, 100.0, "ADD exit",
                          kNaN, "", kNaN, kNaN);
        if (t == at(16, 5, 0)) {
            strategy_entry("NEW-S1", false, kNaN, kNaN, 1, "NEW short", "", 0, -1);
            strategy_entry("NEW-S2", false, kNaN, 1575.0, 1, "NEW add", "", 0, -1);
        }
        if (on(16, 5, 15) && size <= -2.0)
            strategy_exit("NEW-X", "NEW-S1", 1560.0, kNaN, kNaN, kNaN, kNaN, 100.0, "NEW exit",
                          kNaN, "", kNaN, kNaN);
        if (t == at(10, 14, 0) || t == at(13, 20, 0) || t == at(16, 5, 45)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthRex) {
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
        std::printf("    %-7s %lld %s @%lld -> %lld @%lld  q=%lld\n", tag,
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
    CHECK(tape.size() == 6);

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
                            ticks(t.exit_price), std::llround(t.qty * 1e4));
    }
    CHECK(engine == expected);
    if (engine != expected) {
        show("tape", expected);
        show("engine", engine);
    }
}

const TapeTrade* exit_of(const std::vector<TapeTrade>& tape, const std::string& signal) {
    for (const TapeTrade& t : tape)
        if (t.exit_signal == signal) return &t;
    return nullptr;
}

}  // namespace

int main() {
    const Case cases[] = {
        {"w4-rex-coof", false, true},
        {"w4-rex-plain", false, false},
        {"w4-rex-pooc", true, false},
        {"w4-rex-pooc-coof", true, true},
    };
    for (const Case& c : cases) replay(c);

    std::printf("-- the rule, on the tapes\n");
    {
        // Under calc_on_order_fills the re-issued exits fill at their own
        // levels on their entry bar, the first-placed one at the bar's low.
        const auto& t = tapes["w4-rex-coof"];
        const TapeTrade* mc = exit_of(t, "MC exit");
        const TapeTrade* add = exit_of(t, "ADD exit");
        const TapeTrade* fresh = exit_of(t, "NEW exit");
        CHECK(mc != nullptr && std::get<3>(mc->row) == at(10, 13, 30)
              && std::get<4>(mc->row) == ticks(1575.0));
        CHECK(add != nullptr && std::get<3>(add->row) == at(13, 19, 30)
              && std::get<4>(add->row) == ticks(1580.0));
        CHECK(fresh != nullptr && std::get<3>(fresh->row) == at(16, 5, 15)
              && std::get<4>(fresh->row) == ticks(1550.0));
        // The recalculation before cell MC's exit is the margin call's.
        bool called = false;
        for (const TapeTrade& r : t)
            if (r.exit_signal == "Margin call" && std::get<3>(r.row) == at(10, 13, 30))
                called = true;
        CHECK(called);
        // Under process_orders_on_close + calc_on_order_fills every exit is
        // first placed in a recalculation of its entry bar, and is held there.
        const auto& p = tapes["w4-rex-pooc-coof"];
        const TapeTrade* pmc = exit_of(p, "MC exit");
        const TapeTrade* padd = exit_of(p, "ADD exit");
        CHECK(pmc != nullptr && std::get<4>(pmc->row) == ticks(1555.0));
        CHECK(padd != nullptr && std::get<4>(padd->row) == ticks(1559.4));
    }

    std::printf("\n%s calc_on_order_fills re-issued exit tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
