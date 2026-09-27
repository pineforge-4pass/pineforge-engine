/*
 * test_pooc_samepass_tapes.cpp -- lane W4-ENG-POOC-SAMEPASS, rule F08.
 *
 * Under process_orders_on_close TradingView decides the exits a bar's close
 * calculation places in the same close pass that fills the calculation's
 * market orders. The market orders go first -- a strategy.close, or an entry
 * that reverses the held side, voids that side's exits -- and then every exit
 * leg the calculation placed fills at that close when the close's TICK
 * reaches its level on the closing side (a sell stop at or above it, a sell
 * limit at or below it, and the mirror for a buy); otherwise it rests for the
 * next bar. That holds for a position held from an earlier bar, one the close
 * pass opened or reversed into (the exit placed with its entry), and a
 * from_entry "" exit, with or without calc_on_order_fills, at any sizing; a
 * stop fill is slipped like any stop fill, a limit fill is not. Without
 * process_orders_on_close the same exits fill at the next bar's open.
 *
 * The engine filled a held position's exit at the close only when it was a
 * short's (the close pass tested every leg on the buy side), compared the raw
 * close instead of its tick outside one pinned short shape, and never decided
 * the exit of a position the close pass opened: that exit filled at the next
 * bar's open.
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/pooc_samepass, lab tv exports; README.md names each one)
 * through the Pine adapter under the configuration its generated constructor
 * declares, and requires every trade the tape closes inside the replayed bars
 * -- entry and exit time, side, price in ticks, quantity in lots -- to be the
 * engine's. Two cells are outside this rule and are left out of the
 * comparison: cell I (an exit placed before the entry it names, which
 * TradingView never applies) on every w4-f08 tape, and cell H of w4-f08-plain
 * (without process_orders_on_close, the exit placed with a close + reversal).
 * Their TradingView rows are asserted below all the same.
 *
 * Fail-before, this TU against the lane's base (r5/wavej-base 8633d944):
 * see the lane report (every POOC "through" cell but G, and F under
 * w4-f08-pooc, exits a bar late).
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

#ifndef PINEFORGE_W4_POOC_SAMEPASS_FIXTURE_DIR
#error "PINEFORGE_W4_POOC_SAMEPASS_FIXTURE_DIR must name tests/fixtures/pooc_samepass"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthW4: BINANCE:ETHUSDT.P 15m, 2025-04-07 00:00 .. 2025-04-10 12:00 UTC.
#include "fixtures/pooc_samepass/bars.inc"
// kFord15: NYSE:F 15m from 2025-07-02 13:30 UTC (R5 lane E25's fixture).
#include "fixtures/session_islastbar/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;       // both symbols' price tick
constexpr double kEthLot = 0.0001;   // TradingView's BINANCE:ETHUSDT.P quantity step
constexpr std::int64_t kMinute = 60'000;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
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

// timestamp("UTC", 2025, month, day, hour, minute), the probes' cells.
std::int64_t utc(unsigned month, unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, month, day) * 24 + hour) * 60 + minute) * kMinute;
}
std::int64_t at(unsigned day, int hour, int minute) { return utc(4, day, hour, minute); }
std::int64_t ford(int hour, int minute) { return utc(7, 2, hour, minute); }

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

std::string fixture(const std::string& tape, const std::string& file) {
    return std::string(PINEFORGE_W4_POOC_SAMEPASS_FIXTURE_DIR) + "/" + tape + "/" + file;
}

struct TapeTrade {
    Row row;
    std::string entry_signal, exit_signal;
};

// Every trade the tape closes before end_ms, in trade-number order.
std::vector<TapeTrade> tape_trades(const std::string& tape, double lot, std::int64_t end_ms) {
    std::ifstream in(fixture(tape, "tv_trades.csv"));
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
        // Trade number, Type, Date and time, Signal, Price, Size (qty), ...
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        TapeTrade& t = by_number[number];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(t.row) = tape_ms(cell[2]);
            std::get<1>(t.row) = cell[1] == "Entry long";
            std::get<2>(t.row) = ticks(price);
            std::get<3>(t.row) = std::llround(std::stod(cell[5]) / lot);
            t.entry_signal = cell[3];
        } else {
            std::get<4>(t.row) = tape_ms(cell[2]);
            std::get<5>(t.row) = ticks(price);
            t.exit_signal = cell[3];
            closed.insert(number);
        }
    }
    std::vector<TapeTrade> out;
    for (const auto& [number, t] : by_number)
        if (closed.count(number) && std::get<4>(t.row) < end_ms) out.push_back(t);
    return out;
}

enum class Script { Main, Interact, Sizing, HalfCent };

struct Case {
    const char* tape;
    Script script;
    bool pooc, coof;
    QtyType qty_type;
    double qty_value;
    int slippage;
    // Entry signals of the cells outside this rule (see the header).
    std::set<std::string> outside;
};

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const Case& c) : script_(c.script) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = c.pooc;
        cfg.calc_on_order_fills = c.coof;
        cfg.initial_capital = 100000.0;  // every probe omits it: v6's default
        cfg.default_qty_type = static_cast<int>(c.qty_type);
        cfg.default_qty_value = c.qty_value;
        cfg.slippage = c.slippage;
        configure_pine_strategy(cfg);
        if (c.script == Script::HalfCent) {
            set_syminfo_session("0930-1600");
            set_syminfo_timezone("America/New_York");
            set_syminfo_metadata("qty_step", 1.0);
        } else {
            set_syminfo_metadata("qty_step", kEthLot);
        }
    }

    void on_source_bar(const Bar&) override {
        t_ = current_bar_.timestamp;
        switch (script_) {
        case Script::Main: main_cells(); break;
        case Script::Interact: interaction_cells(); break;
        case Script::Sizing: sizing_cells(); break;
        case Script::HalfCent: half_cent_cells(); break;
        }
    }

private:
    double close() const { return current_bar_.close; }
    void entry(const char* id, bool is_long, const char* comment) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, comment);
    }
    void bracket(const char* id, const char* from, double limit, double stop, const char* comment) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, 100.0, comment, kNaN, "",
                      kNaN, kNaN);
    }
    // strategy.cancel_all(); strategy.close_all(comment="cleanup"), as codegen
    // lowers them.
    void cleanup() {
        strategy_cancel_all();
        strategy_close("", "cleanup", kNaN, kNaN, false);
    }

    // w4-f08-{pooc,plain}{,-coof}
    void main_cells() {
        if (t_ == at(8, 0, 0)) {
            entry("A-S", false, "A short");
            bracket("A-X", "A-S", kNaN, close() - 3, "A stop through");
        }
        if (t_ == at(8, 4, 0)) {
            entry("B-L", true, "B long");
            bracket("B-X", "B-L", kNaN, close() + 3, "B stop through");
        }
        if (t_ == at(8, 8, 0)) {
            entry("C-L", true, "C long");
            bracket("C-X", "C-L", close() - 3, kNaN, "C limit through");
        }
        if (t_ == at(8, 12, 0)) {
            entry("D-S", false, "D short");
            bracket("D-X", "D-S", close() + 3, kNaN, "D limit through");
        }
        if (t_ == at(8, 16, 0)) {
            entry("E-L", true, "E long");
            bracket("E-X", "E-L", close() + 30, close() - 30, "E bracket");
        }
        if (t_ == at(9, 0, 0)) entry("F-L", true, "F long");
        if (t_ == at(9, 0, 30)) bracket("F-X", "F-L", kNaN, close() + 3, "F stop through");
        if (t_ == at(9, 4, 0)) entry("G-S", false, "G short");
        if (t_ == at(9, 4, 30)) bracket("G-X", "G-S", kNaN, close() - 3, "G stop through");
        if (t_ == at(9, 8, 0)) entry("H-L", true, "H long");
        if (t_ == at(9, 8, 30)) {
            strategy_close("H-L", "H close", kNaN, kNaN, false, 193273528339ULL);
            entry("H-S", false, "H short");
            bracket("H-X", "H-S", kNaN, close() - 3, "H stop through");
        }
        if (t_ == at(9, 12, 0)) {
            bracket("I-X", "I-L", kNaN, close() + 3, "I stop through");
            entry("I-L", true, "I long");
        }
        if (t_ == at(9, 16, 0)) {
            entry("J-L", true, "J long");
            bracket("J-X", "", kNaN, close() + 3, "J stop through");
        }
        if (t_ == at(10, 0, 0)) {
            entry("K-L", true, "K long");
            bracket("K-X", "K-L", close() - 3, close() + 3, "K both through");
        }
        if (t_ == at(10, 4, 0)) entry("M-L", true, "M long");
        if (t_ == at(10, 4, 30)) {
            entry("M-S", false, "M short");
            bracket("M-X", "M-S", kNaN, close() - 3, "M stop through");
        }
        if (t_ == at(10, 8, 0)) {
            entry("N-S", false, "N short");
            bracket("N-X", "N-S", kNaN, close(), "N stop at close");
        }
        for (const auto& [d, h] : {std::pair<unsigned, int>{8, 2}, {8, 6}, {8, 10}, {8, 14},
                                   {8, 18}, {9, 2}, {9, 6}, {9, 10}, {9, 14}, {9, 18},
                                   {10, 2}, {10, 6}, {10, 10}}) {
            if (t_ == at(d, h, 0)) cleanup();
        }
    }

    // w4-f08b-{pooc,plain}{,-coof}
    void interaction_cells() {
        if (t_ == at(8, 0, 0)) entry("P-L", true, "P long");
        if (t_ == at(8, 0, 30)) {
            bracket("P-XL", "P-L", kNaN, close() + 3, "P long exit through");
            entry("P-S", false, "P short");
        }
        if (t_ == at(8, 4, 0)) entry("Q-L", true, "Q long");
        if (t_ == at(8, 4, 30)) {
            strategy_close("Q-L", "Q close", kNaN, kNaN, false, 81604378643ULL);
            bracket("Q-XL", "Q-L", kNaN, close() + 3, "Q long exit through");
        }
        if (t_ == at(8, 8, 0)) entry("R-L", true, "R long");
        if (t_ == at(8, 8, 30)) {
            bracket("R-XL", "R-L", kNaN, close() + 3, "R long exit through");
            entry("R-S", false, "R short");
            bracket("R-XS", "R-S", kNaN, close() - 3, "R short exit through");
        }
        if (t_ == at(8, 12, 0)) entry("S-L", true, "S long");
        if (t_ == at(8, 12, 30)) {
            bracket("S-X", "", kNaN, close() + 3, "S global exit through");
            entry("S-S", false, "S short");
        }
        if (t_ == at(8, 16, 0)) entry("U-L", true, "U long");
        if (t_ == at(8, 16, 30)) {
            entry("U-S", false, "U short");
            bracket("U-XL", "U-L", kNaN, close() + 3, "U long exit through");
        }
        if (t_ == at(9, 0, 0)) entry("V-L", true, "V long");
        if (t_ == at(9, 0, 15) || t_ == at(9, 0, 30))
            bracket("V-XL", "V-L", kNaN, close() - 30, "V long exit");
        if (t_ == at(9, 0, 45)) bracket("V-XL", "V-L", kNaN, close() + 3, "V long exit through");
        if (t_ == at(9, 4, 0)) {
            entry("W-L", true, "W long");
            bracket("W-X1", "W-L", close() + 30, kNaN, "W target");
            bracket("W-X2", "W-L", kNaN, close() + 3, "W stop through");
        }
        for (const auto& [d, h] : {std::pair<unsigned, int>{8, 2}, {8, 6}, {8, 10}, {8, 14},
                                   {8, 18}, {9, 2}, {9, 6}}) {
            if (t_ == at(d, h, 0)) cleanup();
        }
    }

    // w4-f08s-*: cells A..G of w4-f08 under other sizing and slippage.
    void sizing_cells() {
        if (t_ == at(8, 0, 0)) {
            entry("A-S", false, "A short");
            bracket("A-X", "A-S", kNaN, close() - 3, "A stop through");
        }
        if (t_ == at(8, 4, 0)) {
            entry("B-L", true, "B long");
            bracket("B-X", "B-L", kNaN, close() + 3, "B stop through");
        }
        if (t_ == at(8, 8, 0)) {
            entry("C-L", true, "C long");
            bracket("C-X", "C-L", close() - 3, kNaN, "C limit through");
        }
        if (t_ == at(8, 12, 0)) {
            entry("D-S", false, "D short");
            bracket("D-X", "D-S", close() + 3, kNaN, "D limit through");
        }
        if (t_ == at(8, 16, 0)) {
            entry("E-L", true, "E long");
            bracket("E-X", "E-L", close() + 30, close() - 30, "E bracket");
        }
        if (t_ == at(9, 0, 0)) entry("F-L", true, "F long");
        if (t_ == at(9, 0, 30)) bracket("F-X", "F-L", kNaN, close() + 3, "F stop through");
        if (t_ == at(9, 4, 0)) entry("G-S", false, "G short");
        if (t_ == at(9, 4, 30)) bracket("G-X", "G-S", kNaN, close() - 3, "G stop through");
        for (const auto& [d, h] : {std::pair<unsigned, int>{8, 2}, {8, 6}, {8, 10}, {8, 14},
                                   {8, 18}, {9, 2}, {9, 6}}) {
            if (t_ == at(d, h, 0)) cleanup();
        }
    }

    // w4-f08t-pooc: exit levels 0.002 from half-cent closes (NYSE:F).
    void half_cent_cells() {
        if (t_ == ford(13, 45)) {
            entry("L1", true, "L1 long");
            bracket("L1-X", "L1", kNaN, close() + 0.002, "L1 stop");
        }
        if (t_ == ford(14, 15)) {
            entry("L2", true, "L2 long");
            bracket("L2-X", "L2", close() + 0.002, kNaN, "L2 limit");
        }
        if (t_ == ford(14, 45)) {
            entry("S1", false, "S1 short");
            bracket("S1-X", "S1", kNaN, close() + 0.002, "S1 stop");
        }
        if (t_ == ford(15, 15)) {
            entry("S2", false, "S2 short");
            bracket("S2-X", "S2", close() + 0.002, kNaN, "S2 limit");
        }
        if (t_ == ford(16, 15)) {
            entry("L3", true, "L3 long");
            bracket("L3-X", "L3", kNaN, close() - 0.002, "L3 stop");
        }
        if (t_ == ford(16, 45)) {
            entry("S3", false, "S3 short");
            bracket("S3-X", "S3", kNaN, close() - 0.002, "S3 stop");
        }
        if (t_ == ford(17, 15)) entry("H1", true, "H1 long");
        if (t_ == ford(17, 30)) bracket("H1-X", "H1", kNaN, close() + 0.002, "H1 stop");
        if (t_ == ford(18, 0)) entry("H2", false, "H2 short");
        if (t_ == ford(18, 15)) bracket("H2-X", "H2", kNaN, close() + 0.002, "H2 stop");
        if (t_ == ford(18, 30)) entry("H3", false, "H3 short");
        if (t_ == ford(18, 45)) bracket("H3-X", "H3", close() + 0.002, kNaN, "H3 limit");
        if (t_ == ford(19, 0)) entry("H4", true, "H4 long");
        if (t_ == ford(19, 15)) bracket("H4-X", "H4", close() + 0.002, kNaN, "H4 limit");
        for (const auto& [h, m] : {std::pair<int, int>{14, 0}, {14, 30}, {15, 0}, {15, 30},
                                   {16, 30}, {17, 0}, {17, 45}, {18, 30}, {19, 0}, {19, 30}}) {
            if (t_ == ford(h, m)) cleanup();
        }
    }

    Script script_;
    std::int64_t t_ = 0;
};

std::vector<Bar> feed(Script script) {
    std::vector<Bar> bars;
    const auto push = [&](const FeedBar& row) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    };
    if (script == Script::HalfCent) {
        for (const FeedBar& row : kFord15) push(row);
    } else {
        for (const FeedBar& row : kEthW4) push(row);
    }
    return bars;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

std::map<std::string, std::vector<TapeTrade>> tapes;

void replay(const Case& c) {
    std::printf("-- %s\n", c.tape);
    const std::vector<Bar> bars = feed(c.script);
    const std::int64_t end_ms = bars.back().timestamp;
    const double lot = c.script == Script::HalfCent ? 1.0 : kEthLot;
    const auto tape = tape_trades(c.tape, lot, end_ms);
    tapes[c.tape] = tape;
    CHECK(!tape.empty());

    ProbeHost host(c);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());

    // The cells outside this rule, by the entry that opens them.
    std::set<std::pair<std::int64_t, bool>> skipped;
    std::vector<Row> expected;
    for (const TapeTrade& t : tape) {
        if (c.outside.count(t.entry_signal)) {
            skipped.insert({std::get<0>(t.row), std::get<1>(t.row)});
            continue;
        }
        expected.push_back(t.row);
    }
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        if (skipped.count({t.entry_time, t.is_long})) continue;
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price),
                            std::llround(t.qty / lot), t.exit_time, ticks(t.exit_price));
    }
    CHECK(engine == expected);
    if (engine != expected) {
        show("tape", expected);
        show("engine", engine);
    }
}

const TapeTrade* find(const std::vector<TapeTrade>& tape, const std::string& entry_signal) {
    for (const TapeTrade& t : tape)
        if (t.entry_signal == entry_signal) return &t;
    return nullptr;
}

// The trade opened by `entry_signal` exits by `exit_signal` on the bar at
// `exit_ms` (its close under process_orders_on_close, its open without).
void exits(const std::vector<TapeTrade>& tape, const char* entry_signal,
           const char* exit_signal, std::int64_t exit_ms) {
    const TapeTrade* t = find(tape, entry_signal);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(t->exit_signal == exit_signal);
    CHECK(std::get<4>(t->row) == exit_ms);
}

}  // namespace

int main() {
    constexpr QtyType kFixed = QtyType::FIXED;
    constexpr QtyType kPercent = QtyType::PERCENT_OF_EQUITY;
    const std::set<std::string> none;
    const Case cases[] = {
        {"w4-f08-pooc", Script::Main, true, false, kFixed, 1.0, 0, {"I long"}},
        {"w4-f08-pooc-coof", Script::Main, true, true, kFixed, 1.0, 0, {"I long"}},
        {"w4-f08-plain", Script::Main, false, false, kFixed, 1.0, 0, {"I long", "H short"}},
        {"w4-f08-plain-coof", Script::Main, false, true, kFixed, 1.0, 0, {"I long"}},
        {"w4-f08b-pooc", Script::Interact, true, false, kFixed, 1.0, 0, none},
        {"w4-f08b-pooc-coof", Script::Interact, true, true, kFixed, 1.0, 0, none},
        {"w4-f08b-plain", Script::Interact, false, false, kFixed, 1.0, 0, none},
        {"w4-f08b-plain-coof", Script::Interact, false, true, kFixed, 1.0, 0, none},
        {"w4-f08s-pooc-slip", Script::Sizing, true, false, kFixed, 1.0, 5, none},
        {"w4-f08s-plain-slip", Script::Sizing, false, false, kFixed, 1.0, 5, none},
        {"w4-f08s-pooc-pct50", Script::Sizing, true, false, kPercent, 50.0, 0, none},
        {"w4-f08s-pooc-v6", Script::Sizing, true, false, kPercent, 100.0, 0, none},
        {"w4-f08t-pooc", Script::HalfCent, true, false, kFixed, 1.0, 0, none},
    };
    for (const Case& c : cases) replay(c);

    std::printf("-- the rule, on the tapes\n");
    {
        // calc_on_order_fills changes nothing: the tapes are one file each.
        for (const auto& [a, b] : {std::pair<const char*, const char*>{"w4-f08-pooc", "w4-f08-pooc-coof"},
                                   {"w4-f08-plain", "w4-f08-plain-coof"},
                                   {"w4-f08b-pooc", "w4-f08b-pooc-coof"},
                                   {"w4-f08b-plain", "w4-f08b-plain-coof"}}) {
            std::ifstream fa(fixture(a, "tv_trades.csv")), fb(fixture(b, "tv_trades.csv"));
            const std::string sa((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
            const std::string sb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
            CHECK(!sa.empty() && sa == sb);
        }
    }
    {
        // Every through exit placed with its entry fills on the entry's bar:
        // at its close under process_orders_on_close, at the next open without.
        const auto& pooc = tapes["w4-f08-pooc"];
        const auto& plain = tapes["w4-f08-plain"];
        const std::pair<const char*, std::int64_t> through[] = {
            {"A short", at(8, 0, 0)}, {"B long", at(8, 4, 0)}, {"C long", at(8, 8, 0)},
            {"D short", at(8, 12, 0)}, {"J long", at(9, 16, 0)}, {"K long", at(10, 0, 0)},
            {"N short", at(10, 8, 0)}};
        for (const auto& [signal, cell] : through) {
            const TapeTrade* p = find(pooc, signal);
            const TapeTrade* q = find(plain, signal);
            CHECK(p != nullptr && q != nullptr);
            if (p == nullptr || q == nullptr) continue;
            CHECK(std::get<0>(p->row) == cell && std::get<4>(p->row) == cell);
            CHECK(std::get<0>(q->row) == cell + 15 * kMinute
                  && std::get<4>(q->row) == cell + 15 * kMinute);
        }
        // An exit placed on a later bar of the held position: at that close.
        exits(pooc, "F long", "F stop through", at(9, 0, 30));
        exits(pooc, "G short", "G stop through", at(9, 4, 30));
        exits(plain, "F long", "F stop through", at(9, 0, 45));
        // The close + reversal's short, and the reversal alone's, exit there too.
        exits(pooc, "H short", "H stop through", at(9, 8, 30));
        exits(pooc, "M short", "M stop through", at(10, 4, 30));
        // An exit placed before its entry is never applied.
        for (const char* tape : {"w4-f08-pooc", "w4-f08-plain"}) {
            const TapeTrade* i = find(tapes[tape], "I long");
            CHECK(i != nullptr && i->exit_signal == "cleanup");
        }
    }
    {
        // The calculation's market orders go first: a reversal or a close
        // takes the held long, whose own exit (through) never fills; the
        // reversal's short is decided against the same close.
        const auto& b = tapes["w4-f08b-pooc"];
        exits(b, "P long", "P short", at(8, 0, 30));
        exits(b, "Q long", "Q close", at(8, 4, 30));
        exits(b, "R long", "R short", at(8, 8, 30));
        exits(b, "R short", "R short exit through", at(8, 8, 30));
        exits(b, "S long", "S short", at(8, 12, 30));
        exits(b, "U long", "U short", at(8, 16, 30));
        exits(b, "V long", "V long exit through", at(9, 0, 45));
    }
    {
        // The close's tick decides, not the raw close: a level 0.002 past a
        // half-cent close fills there only when the tick reaches it.
        const auto& t = tapes["w4-f08t-pooc"];
        exits(t, "L2 long", "L2 limit", ford(14, 15));
        exits(t, "S1 short", "S1 stop", ford(14, 45));
        exits(t, "S3 short", "S3 stop", ford(16, 45));
        exits(t, "H2 short", "H2 stop", ford(18, 15));
        exits(t, "L1 long", "L1 stop", ford(14, 0));
        exits(t, "S2 short", "S2 limit", ford(15, 30));
        exits(t, "L3 long", "L3 stop", ford(16, 30));
        exits(t, "H1 long", "H1 stop", ford(17, 45));
    }
    {
        // A stop exit at the close is slipped, a limit exit is not.
        const auto& s = tapes["w4-f08s-pooc-slip"];
        const TapeTrade* a = find(s, "A short");
        const TapeTrade* c = find(s, "C long");
        CHECK(a != nullptr && c != nullptr);
        if (a != nullptr && c != nullptr) {
            CHECK(std::get<5>(a->row) - std::get<2>(a->row) == 10);  // sold -5, bought +5
            CHECK(std::get<2>(c->row) - std::get<5>(c->row) == 5);   // bought +5, limit at the close
        }
    }

    std::printf("\n%s process_orders_on_close same-pass exit tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
