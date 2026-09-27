/*
 * test_same_point_entries_tapes.cpp -- lane W6-ENG-FILL-ORDER.
 *
 * Which order fills first when several reach one fill point, on
 * TradingView's own tapes of synthetic probes (tests/fixtures/
 * same_point_entries, lab tv exports on BINANCE:ETHUSDT.P 15).
 *
 *   F10  Two strategy.entry calls placed on one bar from flat, both already
 *        marketable, fill at one price. TradingView fills them in a fixed
 *        order of side and kind -- buy market, buy stop, sell market, sell
 *        stop, buy limit, sell limit -- and one rank in placement order. The
 *        later of two opposite calls is one transaction of its own quantity
 *        plus the earlier pending MARKET call's (a pending stop or limit
 *        lends it nothing), and the earlier call trades only its own, so a
 *        buy that fills first opens both quantities as one lot the sell then
 *        partly closes: the tape's two rows of the buy's signal.
 *        TradingView costs that transaction at the signal, and past the
 *        equity it drops the later call.
 *
 * Each row replays one tape through the Pine adapter under the configuration
 * the generated constructor declares for its probe, over the corpus 15m bars
 * of fixtures/tvdef_drops/bars.inc, trading from the bar before TradingView's
 * first entry as run_strategy.py does for a corpus tape, and requires every
 * trade the tape closes in the rows' cells -- entry and exit time, side,
 * price in ticks of 0.01 and quantity in lots of 0.0001 -- to be the
 * engine's, in the tape's order. A row names the cells it covers; the README
 * lists the cells TradingView and the engine still book differently.
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

#ifndef PINEFORGE_SAME_POINT_FIXTURE_DIR
#error "PINEFORGE_SAME_POINT_FIXTURE_DIR must name tests/fixtures/same_point_entries"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/tvdef_drops/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
constexpr std::int64_t kBarMs = 15 * 60'000;
constexpr std::int64_t kStepMs = 2 * 60 * 60'000;
constexpr std::int64_t kCleanupMs = 30 * 60'000;

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

// 2025-04-<day> <hour>:<minute> UTC, the probes' timestamp("UTC", ...) cells.
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Tape {
    std::vector<Row> trades;           // closed inside the replayed bars, in tape order
    std::vector<std::string> entries;  // each trade's entry signal
    std::vector<std::string> exits;    // each trade's exit signal
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_SAME_POINT_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal, exit_signal;
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
            entry_signal[number] = cell[3];
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        const auto exit = exit_signal.find(number);
        if (exit != exit_signal.end() && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            out.entries.push_back(entry_signal[number]);
            out.exits.push_back(exit->second);
        }
    }
    return out;
}

// One strategy.entry leg of an F10 cell: side, kind (M market, S stop, L
// limit; a priced leg sits at half or twice the close, where it is already
// marketable), quantity and comment.
struct Leg {
    bool is_long;
    char kind;
    double qty;
    const char* comment;
};
struct Cell {
    Leg first, second;
};

// The probes' cell tables, as their strategy.pine sources declare them.
const std::vector<Cell> kOpenPair = {
    {{true, 'M', 1, "MM-LF-1"}, {false, 'M', 1, "MM-LF-2"}},
    {{false, 'M', 1, "MM-SF-1"}, {true, 'M', 1, "MM-SF-2"}},
    {{true, 'M', 1, "MS-LF-1"}, {false, 'S', 1, "MS-LF-2"}},
    {{false, 'M', 1, "MS-SF-1"}, {true, 'S', 1, "MS-SF-2"}},
    {{true, 'S', 1, "SM-LF-1"}, {false, 'M', 1, "SM-LF-2"}},
    {{false, 'S', 1, "SM-SF-1"}, {true, 'M', 1, "SM-SF-2"}},
    {{true, 'S', 1, "SS-LF-1"}, {false, 'S', 1, "SS-LF-2"}},
    {{false, 'S', 1, "SS-SF-1"}, {true, 'S', 1, "SS-SF-2"}},
    {{false, 'M', 1, "ML-SF-1"}, {true, 'L', 1, "ML-SF-2"}},
    {{false, 'L', 1, "LM-SF-1"}, {true, 'M', 1, "LM-SF-2"}},
    {{false, 'L', 1, "LL-SF-1"}, {true, 'L', 1, "LL-SF-2"}},
    {{false, 'L', 1, "LS-SF-1"}, {true, 'S', 1, "LS-SF-2"}},
    {{false, 'S', 1, "SL-SF-1"}, {true, 'L', 1, "SL-SF-2"}},
    {{false, 'M', 2, "MS-SF-Q2-1"}, {true, 'S', 3, "MS-SF-Q3-2"}},
};
const std::vector<Cell> kLimitClass = {
    {{true, 'L', 1, "LM-LF-1"}, {false, 'M', 1, "LM-LF-2"}},
    {{true, 'L', 1, "LS-LF-1"}, {false, 'S', 1, "LS-LF-2"}},
    {{true, 'L', 1, "LL-LF-1"}, {false, 'L', 1, "LL-LF-2"}},
    {{true, 'M', 1, "ML-LF-1"}, {false, 'L', 1, "ML-LF-2"}},
    {{true, 'S', 1, "SL-LF-1"}, {false, 'L', 1, "SL-LF-2"}},
    {{true, 'L', 2, "LM-LF-Q2-1"}, {false, 'M', 3, "LM-LF-Q3-2"}},
    {{false, 'M', 2, "ML-SF-Q2-1"}, {true, 'L', 3, "ML-SF-Q3-2"}},
    {{false, 'L', 2, "LM-SF-Q2-1"}, {true, 'M', 3, "LM-SF-Q3-2"}},
};
const std::vector<Cell> kSameSide = {
    {{true, 'L', 1, "LM-L-1"}, {true, 'M', 2, "LM-L-2"}},
    {{true, 'L', 1, "LS-L-1"}, {true, 'S', 2, "LS-L-2"}},
    {{true, 'S', 1, "SM-L-1"}, {true, 'M', 2, "SM-L-2"}},
    {{true, 'M', 1, "MS-L-1"}, {true, 'S', 2, "MS-L-2"}},
    {{false, 'L', 1, "LM-S-1"}, {false, 'M', 2, "LM-S-2"}},
    {{false, 'L', 1, "LS-S-1"}, {false, 'S', 2, "LS-S-2"}},
    {{false, 'S', 1, "SM-S-1"}, {false, 'M', 2, "SM-S-2"}},
    {{false, 'M', 1, "MS-S-1"}, {false, 'S', 2, "MS-S-2"}},
};
const std::vector<Cell> kGross = {
    {{true, 'M', 1, "MM-LF-1"}, {false, 'M', 1, "MM-LF-2"}},
    {{false, 'M', 1, "MM-SF-1"}, {true, 'M', 1, "MM-SF-2"}},
    {{true, 'M', 1, "MS-LF-1"}, {false, 'S', 1, "MS-LF-2"}},
    {{false, 'M', 1, "MS-SF-1"}, {true, 'S', 1, "MS-SF-2"}},
};
enum class Probe { Pair };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const std::vector<Cell>* cells, int passes,
              const source::PineStrategyConfig& config)
        : probe_(probe), cells_(cells), passes_(passes) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override { pair(current_bar_.timestamp); }

private:
    // A buy stop / sell limit at half the close, a sell stop / buy limit at
    // twice it: each is already through the price.
    void place(const std::string& id, const Leg& leg) {
        const double lo = current_bar_.close * 0.5;
        const double hi = current_bar_.close * 2.0;
        const double stop = leg.kind == 'S' ? (leg.is_long ? lo : hi) : kNaN;
        const double limit = leg.kind == 'L' ? (leg.is_long ? hi : lo) : kNaN;
        strategy_entry(id, leg.is_long, limit, stop, leg.qty, leg.comment, "", 0, -1);
    }

    // The pair every two hours from 2025-04-08 00:00 UTC while flat,
    // flattened 30 minutes later.
    void pair(std::int64_t t) {
        const std::int64_t n = static_cast<std::int64_t>(cells_->size());
        const std::int64_t rel = t - at(8, 0, 0);
        const bool in_range = rel >= 0 && rel < passes_ * n * kStepMs;
        const bool event = in_range && rel % kStepMs == 0;
        const bool cleanup = in_range && rel % kStepMs == kCleanupMs;
        if (event && signed_position_size() == 0.0) {
            const Cell& cell = (*cells_)[static_cast<std::size_t>((rel / kStepMs) % n)];
            place("E1", cell.first);
            place("E2", cell.second);
        }
        if (cleanup) {
            strategy_cancel_all();
            strategy_close("", "FLT", kNaN, kNaN, false);
        }
    }

    Probe probe_;
    const std::vector<Cell>* cells_;
    std::int64_t passes_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, const std::vector<Cell>* cells, int passes,
        const source::PineStrategyConfig& config, std::int64_t trade_start_ms,
        std::int64_t end_ms) {
    ProbeHost host(probe, cells, passes, config);
    host.set_trade_start_time(trade_start_ms);
    const std::vector<Bar> bars = feed();
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
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

source::PineStrategyConfig fixed_config(int pyramiding, bool pooc, bool coof,
                                        double capital = 1000000.0) {
    source::PineStrategyConfig c{};
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyramiding;
    c.process_orders_on_close = pooc;
    c.calc_on_order_fills = coof;
    return c;
}

// The pair cell an entry time belongs to (its two-hour slot), -1 outside.
int pair_cell(std::int64_t entry_ms, std::size_t cells, int passes) {
    const std::int64_t rel = entry_ms - at(8, 0, 0);
    if (rel < 0 || rel >= passes * static_cast<std::int64_t>(cells) * kStepMs) return -1;
    return static_cast<int>((rel / kStepMs) % static_cast<std::int64_t>(cells));
}

struct Case {
    const char* tape;
    Probe probe;
    const std::vector<Cell>* cells;
    int passes;                        // how many times the cell table repeats
    const char* declares;              // the probe's strategy() arguments that matter
    source::PineStrategyConfig lane;   // what its generated constructor declares
    std::vector<int> covered;          // the cells this row requires (empty: every trade)
    std::size_t closed;                // tape trades closed inside the bars
};

std::vector<Row> covered_rows(const std::vector<Row>& rows, const Case& c) {
    std::vector<Row> out;
    for (const Row& row : rows) {
        const std::int64_t entry = std::get<0>(row);
        bool keep = c.covered.empty();
        for (const int wanted : c.covered)
            if (pair_cell(entry, c.cells->size(), c.passes) == wanted) keep = true;
        if (keep) out.push_back(row);
    }
    return out;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    const std::vector<int> all14 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    const std::vector<int> all8 = {0, 1, 2, 3, 4, 5, 6, 7};

    const Case cases[] = {
        {"w6-f10a-open-pair", Probe::Pair, &kOpenPair, 2, "fixed 1, pyramiding 0",
         fixed_config(0, false, false), all14, 40},
        {"w6-f10b-limit-class", Probe::Pair, &kLimitClass, 2, "fixed 1, pyramiding 0",
         fixed_config(0, false, false), all8, 24},
        {"w6-f10g-pair-gross", Probe::Pair, &kGross, 7, "fixed 1, pyramiding 0, capital 3000",
         fixed_config(0, false, false, 3000.0), {0, 1, 2, 3}, 38},
        {"w6-f10h-pair-gross-pyramiding1", Probe::Pair, &kGross, 7,
         "fixed 1, pyramiding 1, capital 3000", fixed_config(1, false, false, 3000.0),
         {0, 1, 2, 3}, 38},
    };

    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const Tape tape = tape_trades(c.tape, end_ms);
        CHECK(tape.trades.size() == c.closed);
        const std::int64_t trade_start = tape.trades.empty()
            ? at(7, 0, 0) : std::get<0>(tape.trades.front()) - kBarMs;
        const Run lane = run(c.probe, c.cells, c.passes, c.lane, trade_start, end_ms);
        CHECK(lane.error.empty());
        const std::vector<Row> want = covered_rows(tape.trades, c);
        const std::vector<Row> got = covered_rows(lane.trades, c);
        CHECK(!want.empty());
        CHECK(got == want);
        if (got != want) {
            show("tape", want);
            show("engine", got);
        }
    }

    // What TradingView's own rows say, on every pair tape -- the ones the
    // engine replays above and the ones it does not yet (README). In each
    // cell where both legs traded, the first row is the leg of the lower
    // rank, and when that is the later call opposite an earlier market, it
    // opened both quantities as one lot the earlier call then partly closed.
    std::printf("-- the order, on the tapes\n");
    const auto rank = [](const Leg& leg) {
        const int kind = leg.kind == 'M' ? 0 : (leg.kind == 'S' ? 1 : 4);
        return kind == 4 ? (leg.is_long ? 4 : 5) : kind + (leg.is_long ? 0 : 2);
    };
    struct PairTape {
        const char* tape;
        const std::vector<Cell>* cells;
        std::size_t both_traded;  // cells, over every pass, where both legs traded
    };
    const PairTape pair_tapes[] = {
        {"w6-f10a-open-pair", &kOpenPair, 28},
        {"w6-f10b-limit-class", &kLimitClass, 16},
        {"w6-f10c-same-side-class", &kSameSide, 16},
        {"w6-f10g-pair-gross", &kGross, 10},
        {"w6-f10h-pair-gross-pyramiding1", &kGross, 10},
    };
    for (const PairTape& pt : pair_tapes) {
        const Tape tape = tape_trades(pt.tape, end_ms);
        std::map<std::int64_t, std::vector<std::size_t>> slots;
        for (std::size_t i = 0; i < tape.trades.size(); ++i) {
            const std::int64_t rel = std::get<0>(tape.trades[i]) - at(8, 0, 0);
            if (rel >= 0) slots[rel / kStepMs].push_back(i);
        }
        std::size_t both = 0;
        for (const auto& [slot, rows] : slots) {
            const Cell& cell = (*pt.cells)[static_cast<std::size_t>(slot) % pt.cells->size()];
            bool first_traded = false, second_traded = false;
            long long second_lots = 0;
            for (const std::size_t i : rows) {
                for (const std::string* signal : {&tape.entries[i], &tape.exits[i]}) {
                    first_traded = first_traded || *signal == cell.first.comment;
                    second_traded = second_traded || *signal == cell.second.comment;
                }
                if (tape.entries[i] == cell.second.comment) second_lots += std::get<3>(tape.trades[i]);
            }
            if (!first_traded || !second_traded) continue;
            ++both;
            const bool second_leads = rank(cell.second) < rank(cell.first);
            const Leg& lead = second_leads ? cell.second : cell.first;
            CHECK(tape.entries[rows.front()] == lead.comment);
            if (second_leads && cell.first.is_long != cell.second.is_long) {
                const double pending = cell.first.kind == 'M' ? cell.first.qty : 0.0;
                CHECK(second_lots == lots(cell.second.qty + pending));
            }
        }
        std::printf("   %s: %zu cells with both legs traded\n", pt.tape, both);
        CHECK(both == pt.both_traded);
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
