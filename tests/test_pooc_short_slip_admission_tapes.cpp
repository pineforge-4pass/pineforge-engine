/*
 * test_pooc_short_slip_admission_tapes.cpp -- lane W4-ENG-POOC-SAMEPASS, rule
 * F14.
 *
 * Under process_orders_on_close TradingView sizes a default-quantity
 * percent_of_equity MARKET entry at its slipped fill -- the close plus the
 * slippage ticks for a buy, less them for a sell -- and opens a short only
 * while those units' margin at the close itself fits the equity the
 * calculation marked there. At 100 % of equity and positive slippage a short
 * therefore needs more than the equity unless a coarse lot step absorbs the
 * gap: TradingView drops it from a flat book and keeps only the closing leg
 * of one that reverses a long, whether a strategy.close of that long precedes
 * the entry or follows it. 99.99 % of 100000 at slippage 15 opens (64.1385 x
 * 1559.12 = 99999.6), 99.995 % does not.
 *
 * The engine opened each such short, sized at the slipped fill, and margin-
 * called it on its entry bar -- the zero-length and fragmented trades of
 * julzalgo's NYSE:F, ETH and EURUSD tapes, which TradingView books no short
 * on at all.
 *
 * A strategy.exit placed after its entry on the calculation that also closes
 * the held side survives the flat that close makes (cells F): TradingView fills
 * the bracket for the new position, where the engine removed it with the closed
 * side's exits.
 *
 * Each row replays TradingView's own tape of the lane's synthetic probes
 * (tests/fixtures/pooc_short_slip_admission, lab tv exports on BINANCE:ETHUSDT.P
 * 15; README.md names each one) through the Pine adapter under the
 * configuration its generated constructor declares. The rows compare every
 * trade the tape closes inside the replayed bars; on the two tapes whose
 * admitted short a margin call slices on its entry bar (99.99 % and 99.985 %),
 * whose slices are the margin model's and not this rule's, they compare the
 * short's admission: its entry fill and the quantity it opened.
 *
 * Fail-before: see the lane report (w4-f14-pooc-v6-slip opens every short and
 * the 99.995 % short opens; before the lane's bracket rule the cell F brackets
 * are lost as well).
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

#ifndef PINEFORGE_W4_SHORT_SLIP_FIXTURE_DIR
#error "PINEFORGE_W4_SHORT_SLIP_FIXTURE_DIR must name tests/fixtures/pooc_short_slip_admission"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthF14: BINANCE:ETHUSDT.P 15m, 2025-04-08 00:00 .. 2025-04-09 08:00 UTC.
#include "fixtures/pooc_short_slip_admission/bars.inc"

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
    std::ifstream in(std::string(PINEFORGE_W4_SHORT_SLIP_FIXTURE_DIR) + "/" + tape
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
            std::get<5>(t.row) = lots(std::stod(cell[5]));
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

enum class Script { Cells, Single };

struct Case {
    const char* tape;
    Script script;
    bool pooc;
    QtyType qty_type;
    double qty_value;
    int slippage;
    bool admission_only;  // compare the entries' fills and units only
};

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const Case& c) : script_(c.script) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = c.pooc;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(c.qty_type);
        cfg.default_qty_value = c.qty_value;
        cfg.slippage = c.slippage;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        if (script_ == Script::Single) {
            if (t == at(8, 0, 0)) entry("S", false, "S");
            if (t == at(8, 4, 0)) entry("L", true, "L");
            if (t == at(8, 1, 0) || t == at(8, 5, 0)) close_all("cleanup");
            return;
        }
        if (t == at(8, 0, 0)) entry("A-L", true, "A long");
        if (t == at(8, 0, 30)) {
            strategy_close("A-L", "A close", kNaN, kNaN, false, 55834574867ULL);
            entry("A-S", false, "A short");
        }
        if (t == at(8, 4, 0)) entry("B-L", true, "B long");
        if (t == at(8, 4, 30)) {
            entry("B-S", false, "B short");
            strategy_close("B-L", "B close", kNaN, kNaN, false, 85899345939ULL);
        }
        if (t == at(8, 8, 0)) entry("C-L", true, "C long");
        if (t == at(8, 8, 30)) entry("C-S", false, "C short");
        if (t == at(8, 12, 0)) entry("D-S", false, "D short");
        if (t == at(8, 12, 30)) {
            strategy_close("D-S", "D close", kNaN, kNaN, false, 128849018899ULL);
            entry("D-L", true, "D long");
        }
        if (t == at(8, 16, 30)) {
            strategy_close("E-L", "E close", kNaN, kNaN, false, 146028888083ULL);
            entry("E-S", false, "E short");
        }
        if (t == at(9, 0, 0)) entry("F-L", true, "F long");
        if (t == at(9, 0, 30)) {
            strategy_close("F-L", "F close", kNaN, kNaN, false, 171798691859ULL);
            entry("F-S", false, "F short");
            strategy_exit("F-X", "F-S", current_bar_.close - 40, current_bar_.close + 40, kNaN,
                          kNaN, kNaN, 100.0, "F bracket", kNaN, "", kNaN, kNaN);
        }
        if (t == at(9, 4, 0)) entry("G-L", true, "G long");
        if (t == at(9, 4, 30)) {
            close_all("G close all");
            entry("G-S", false, "G short");
        }
        for (const auto& [d, h] : {std::pair<unsigned, int>{8, 2}, {8, 6}, {8, 10}, {8, 14},
                                   {8, 18}, {9, 2}, {9, 6}}) {
            if (t == at(d, h, 0)) {
                strategy_cancel_all();
                close_all("cleanup");
            }
        }
    }

private:
    void entry(const char* id, bool is_long, const char* comment) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, comment);
    }
    // strategy.close_all(comment=...), as codegen lowers it.
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }

    Script script_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthF14) {
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

// The short entries' fills: (entry ms, entry ticks) -> lots, summed over the
// rows a margin call slices. (The later long is sized off the equity those
// slices leave, which is the margin model's too.)
std::map<std::tuple<std::int64_t, long long>, long long> short_entries(const std::vector<Row>& rows) {
    std::map<std::tuple<std::int64_t, long long>, long long> out;
    for (const Row& r : rows)
        if (!std::get<1>(r)) out[{std::get<0>(r), std::get<2>(r)}] += std::get<5>(r);
    return out;
}

std::map<std::string, std::vector<TapeTrade>> tapes;

void replay(const Case& c) {
    std::printf("-- %s\n", c.tape);
    const std::vector<Bar> bars = feed();
    const std::int64_t end_ms = bars.back().timestamp;
    const auto tape = tape_trades(c.tape, end_ms);
    tapes[c.tape] = tape;

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
    const bool same = c.admission_only ? short_entries(engine) == short_entries(expected)
                                       : engine == expected;
    CHECK(same);
    if (!same) {
        show("tape", expected);
        show("engine", engine);
    }
}

bool opens_short(const std::vector<TapeTrade>& tape) {
    for (const TapeTrade& t : tape)
        if (!std::get<1>(t.row)) return true;
    return false;
}

}  // namespace

int main() {
    constexpr QtyType kPercent = QtyType::PERCENT_OF_EQUITY;
    constexpr QtyType kFixed = QtyType::FIXED;
    const Case cases[] = {
        {"w4-f14-pooc-v6-slip", Script::Cells, true, kPercent, 100.0, 15, false},
        {"w4-f14-pooc-pct50", Script::Cells, true, kPercent, 50.0, 0, false},
        {"w4-f14-plain-fixed1", Script::Cells, false, kFixed, 1.0, 0, false},
        {"w4-f14m-p99995", Script::Single, true, kPercent, 99.995, 15, false},
        {"w4-f14m-p9999", Script::Single, true, kPercent, 99.99, 15, true},
        {"w4-f14m-p99985", Script::Single, true, kPercent, 99.985, 15, true},
        {"w4-f14m-p9998", Script::Single, true, kPercent, 99.98, 15, false},
    };
    for (const Case& c : cases) replay(c);

    std::printf("-- the rule, on the tapes\n");
    {
        // At 100 % with slippage 15 no short opens, whatever precedes it;
        // every long does, and every reversal closes the long it reverses.
        const auto& t = tapes["w4-f14-pooc-v6-slip"];
        CHECK(!t.empty() && !opens_short(t));
        std::set<std::string> by_reversal;
        for (const TapeTrade& r : t)
            if (r.exit_signal == "B short" || r.exit_signal == "C short") by_reversal.insert(r.exit_signal);
        CHECK(by_reversal.size() == 2);
        // Without slippage the same shorts open (w4-f14-pooc-pct50 at 50 %).
        CHECK(opens_short(tapes["w4-f14-pooc-pct50"]));
    }
    {
        // The threshold sits between 99.99 % and 99.995 % of equity.
        CHECK(!opens_short(tapes["w4-f14m-p99995"]));
        CHECK(opens_short(tapes["w4-f14m-p9999"]));
        CHECK(opens_short(tapes["w4-f14m-p99985"]));
        CHECK(opens_short(tapes["w4-f14m-p9998"]));
    }
    {
        // The bracket placed after its entry on the closing calculation fills
        // for the new short (limit 40 below the placing close).
        for (const char* tape : {"w4-f14-pooc-pct50", "w4-f14-plain-fixed1"}) {
            bool bracket = false;
            for (const TapeTrade& r : tapes[tape])
                if (r.entry_signal == "F short" && r.exit_signal == "F bracket") bracket = true;
            CHECK(bracket);
        }
    }

    std::printf("\n%s process_orders_on_close slipped short admission tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
