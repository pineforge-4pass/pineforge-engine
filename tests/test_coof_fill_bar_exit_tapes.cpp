/*
 * test_coof_fill_bar_exit_tapes.cpp -- lane W4-ENG-POOC-SAMEPASS, rule F19
 * under calc_on_order_fills.
 *
 * A script that places its exit only once the position exists
 * (strategy.position_size != 0), priced off strategy.position_avg_price, places
 * it under calc_on_order_fills in the recalculation of the entry's open fill.
 * TradingView keeps that exit live for the rest of the entry bar: a stop
 * already through the fill executes at the fill itself (cells LA and SA, a
 * zero-length trade at the open), and a limit or stop the bar's remaining path
 * reaches fills at its level there (cells LB, LC, SB, SC). Without
 * calc_on_order_fills the exit is placed by the bar's close calculation and
 * acts from the next bar; under process_orders_on_close the entry fills at the
 * close, whose fill recalculates nothing, with or without
 * calc_on_order_fills (the tapes are one file).
 *
 * The engine held the through stop of the first open fill's recalculation for
 * the next bar (the wrong-side deferral of exit()), so the trade ran on to the
 * cleanup; the path-reached legs it already booked on the entry bar.
 *
 * Each row replays TradingView's own tape of the lane's synthetic probe
 * (tests/fixtures/coof_fill_bar_exit, lab tv exports on BINANCE:ETHUSDT.P 15;
 * README.md names each one) through the Pine adapter under the configuration
 * its generated constructor declares, and requires every trade the tape closes
 * inside the replayed bars -- entry and exit time, side, price in ticks,
 * quantity -- to be the engine's.
 *
 * Fail-before: see the lane report (cells LA and SA on both calc_on_order_fills
 * tapes run to the cleanup).
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
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

#ifndef PINEFORGE_W4_COOF_FILL_BAR_FIXTURE_DIR
#error "PINEFORGE_W4_COOF_FILL_BAR_FIXTURE_DIR must name tests/fixtures/coof_fill_bar_exit"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthF19c: BINANCE:ETHUSDT.P 15m, 2025-04-11 00:00 .. 05:00 UTC.
#include "fixtures/coof_fill_bar_exit/bars.inc"

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

std::int64_t at(int hour, int minute) {
    return ((days_from_civil(2025, 4, 11) * 24 + hour) * 60 + minute) * kMinute;
}

std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

std::string fixture(const std::string& tape) {
    return std::string(PINEFORGE_W4_COOF_FILL_BAR_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv";
}

struct TapeTrade {
    Row row;
    std::string entry_signal, exit_signal;
};

std::vector<TapeTrade> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(fixture(tape));
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
    bool long_side, pooc, coof;
};

// The probe, as its generated TU lowers it (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const Case& c) : long_(c.long_side) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = c.pooc;
        cfg.calc_on_order_fills = c.coof;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", 0.0001);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const std::string p = long_ ? "L" : "S";
        const double sign = long_ ? 1.0 : -1.0;
        if (t == at(0, 30)) { strategy_entry(p + "C", long_, kNaN, kNaN, kNaN, p + "C"); active_ = p + "C"; }
        if (t == at(1, 15)) { strategy_entry(p + "B", long_, kNaN, kNaN, kNaN, p + "B"); active_ = p + "B"; }
        if (t == at(2, 45)) { strategy_entry(p + "A", long_, kNaN, kNaN, kNaN, p + "A"); active_ = p + "A"; }
        const double size = signed_position_size();
        if (long_ ? size > 0.0 : size < 0.0) {
            const double avg = position_entry_price_;
            if (active_ == p + "A")
                strategy_exit(p + "A-X", p + "A", kNaN, avg + sign * 1, kNaN, kNaN, kNaN, 100.0,
                              p + "A stop through", kNaN, "", kNaN, kNaN);
            if (active_ == p + "B")
                strategy_exit(p + "B-X", p + "B", avg + sign * 10, avg - sign * 5, kNaN, kNaN,
                              kNaN, 100.0, p + "B exit", kNaN, "", kNaN, kNaN);
            if (active_ == p + "C")
                strategy_exit(p + "C-X", p + "C", avg + sign * 5, avg - sign * 10, kNaN, kNaN,
                              kNaN, 100.0, p + "C exit", kNaN, "", kNaN, kNaN);
        }
        if (t == at(1, 0) || t == at(2, 15) || t == at(4, 0)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

private:
    bool long_;
    std::string active_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthF19c) {
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
    CHECK(tape.size() == 3);

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

const TapeTrade* find(const std::vector<TapeTrade>& tape, const std::string& signal) {
    for (const TapeTrade& t : tape)
        if (t.entry_signal == signal) return &t;
    return nullptr;
}

}  // namespace

int main() {
    const Case cases[] = {
        {"w4-f19c-long-plain-coof", true, false, true},
        {"w4-f19c-long-plain", true, false, false},
        {"w4-f19c-long-pooc-coof", true, true, true},
        {"w4-f19c-long-pooc", true, true, false},
        {"w4-f19c-short-plain-coof", false, false, true},
        {"w4-f19c-short-plain", false, false, false},
        {"w4-f19c-short-pooc-coof", false, true, true},
        {"w4-f19c-short-pooc", false, true, false},
    };
    for (const Case& c : cases) replay(c);

    std::printf("-- the rule, on the tapes\n");
    for (const char* side : {"long", "short"}) {
        const std::string p = std::string(side) == "long" ? "L" : "S";
        const auto& coof = tapes[std::string("w4-f19c-") + side + "-plain-coof"];
        const auto& plain = tapes[std::string("w4-f19c-") + side + "-plain"];
        // Every calc_on_order_fills exit fills on its entry's bar; the through
        // stop at the entry's own fill, a zero-length trade.
        for (const std::string cell : {"A", "B", "C"}) {
            const TapeTrade* t = find(coof, p + cell);
            CHECK(t != nullptr);
            if (t == nullptr) continue;
            CHECK(std::get<3>(t->row) == std::get<0>(t->row));
        }
        const TapeTrade* a = find(coof, p + "A");
        CHECK(a != nullptr && std::get<4>(a->row) == std::get<2>(a->row));
        // Without the recalculation none of them does.
        for (const std::string cell : {"A", "B", "C"}) {
            const TapeTrade* t = find(plain, p + cell);
            CHECK(t != nullptr && std::get<3>(t->row) > std::get<0>(t->row));
        }
        // Under process_orders_on_close calc_on_order_fills changes nothing.
        std::ifstream x(fixture(std::string("w4-f19c-") + side + "-pooc"));
        std::ifstream y(fixture(std::string("w4-f19c-") + side + "-pooc-coof"));
        const std::string sx((std::istreambuf_iterator<char>(x)), std::istreambuf_iterator<char>());
        const std::string sy((std::istreambuf_iterator<char>(y)), std::istreambuf_iterator<char>());
        CHECK(!sx.empty() && sx == sy);
    }

    std::printf("\n%s calc_on_order_fills entry-bar exit tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
