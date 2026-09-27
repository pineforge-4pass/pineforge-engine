/*
 * test_grid_close_tapes.cpp — lane W3B-ENG-GRID (family F02).
 *
 * Under the default FIFO close_entries_rule TradingView sizes a
 * strategy.close(id) without qty from a ledger of the units entered under
 * `id` that no fill has booked yet, not from the lots still carrying the id:
 * the FIFO rule has usually closed those lots already. The grid bots of the
 * F02 family (pyramiding up to 200, one strategy.close call site in a loop
 * over the grid levels) read that ledger on every take-profit, and
 * TradingView keeps it by these rules:
 *
 *   - One strategy.close call site places one order per bar. A later call of
 *     the site re-sizes that order for its own id and comment, but the
 *     order's fill books against the id of the site's first call on the bar.
 *   - A fill books against its id's unbooked units, oldest first; what they
 *     cannot cover spills over every id's unbooked units in the order the
 *     entries filled.
 *   - A call whose id has no unbooked units places nothing and leaves its
 *     site's order as it was.
 *
 * The adapter kept a per-id ledger, but it erased the first id's units whole
 * and reserved the survivor's fill against the other ids instead of booking
 * the fill, so the units the first id could not cover stayed on the books:
 * close("L38") closed 0.19 where TradingView closes 0.18 (w3f02-x2), or
 * nothing (w3f02-x1).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/grid_close, lab tv exports on BINANCE:ETHUSDT.P 15)
 * through the Pine adapter under the configuration the generated constructor
 * declares for it, over the corpus 15m bars of tests/fixtures/exit_queue
 * (bars.inc), and requires every trade the tape closes inside those bars to
 * be the engine's: entry and exit time, side, price, quantity and the exit's
 * signal. Then it reads the rules off TradingView's own rows.
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

#ifndef PINEFORGE_GRID_CLOSE_FIXTURE_DIR
#error "PINEFORGE_GRID_CLOSE_FIXTURE_DIR must name tests/fixtures/grid_close"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/exit_queue/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// Every probe places its first entry at 2025-04-08 00:00 UTC; the harness
// trades from there, as run_strategy.py does for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1744070400000LL;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price, exit signal).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long,
                       std::string>;

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

// 2025-04-<day> 00:00 UTC plus `step` fifteen-minute bars.
std::int64_t at(unsigned day, int step) {
    return (days_from_civil(2025, 4, day) * 24 * 60 + step * 15) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

// Every trade the tape closes inside the replayed bars, in trade order.
std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_GRID_CLOSE_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
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
            std::get<6>(row) = cell[3];
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number)
        if (std::get<4>(row) > 0 && std::get<4>(row) < end_ms) out.push_back(row);
    return out;
}

// One source command of a probe, on 2025-04-<day> at `step`.
//   E  strategy.entry(id, long, qty) through the entry call site
//   C  strategy.close(id) through call site `site`, with comment `comment`
//   A  strategy.close_all (strategy.close with an empty id)
struct Event {
    unsigned day;
    int step;
    char kind;
    std::string id;
    double qty;
    std::uint64_t site;
    std::string comment;
};

struct Probe {
    const char* tape;
    bool pooc;
    int pyramiding;
    std::vector<Event> events;
};

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(const Probe& probe, const source::PineStrategyConfig& config)
        : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        for (const Event& e : probe_.events) {
            if (at(e.day, e.step) != t) continue;
            switch (e.kind) {
            case 'E':
                strategy_entry(e.id, true, kNaN, kNaN, e.qty, e.comment, "", 0, -1);
                break;
            case 'C':
                strategy_close(e.id, e.comment, kNaN, kNaN, false, e.site);
                break;
            case 'A':
                strategy_close("", e.comment, kNaN, kNaN, false);
                break;
            default:
                break;
            }
        }
    }

private:
    const Probe& probe_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(const Probe& probe, std::int64_t end_ms) {
    source::PineStrategyConfig config{};
    config.process_orders_on_close = probe.pooc;
    config.initial_capital = 100000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = probe.pyramiding;
    ProbeHost host(probe, config);
    host.set_trade_start_time(kTradeStartMs);
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
                                t.exit_time, ticks(t.exit_price), t.exit_comment);
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks %s\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r), std::get<6>(r).c_str());
}

// ---- probe programs --------------------------------------------------------

constexpr std::uint64_t kLoopSite = 101;   // the grid's loop call site
constexpr std::uint64_t kSoleSite = 102;   // a second, sole call site

Event entry(unsigned day, int step, const std::string& id, double qty,
            const std::string& comment) {
    return {day, step, 'E', id, qty, 0, comment};
}
Event close_at(unsigned day, int step, const std::string& id, std::uint64_t site,
               const std::string& comment) {
    return {day, step, 'C', id, kNaN, site, comment};
}
Event cleanup(unsigned day, int step) { return {day, step, 'A', "", kNaN, 0, "cleanup"}; }

// w3f02-x1 / -x2: a grid bot's order sequence; entries L<n> through one
// call site, closes through one loop site, cleanup at step 20 (UTC 05:00).
Probe grid_sequence(const char* tape,
                    const std::vector<std::tuple<int, int, double>>& entries,
                    const std::vector<std::pair<int, int>>& closes) {
    Probe p{tape, true, 200, {}};
    for (int step = 0; step <= 20; ++step) {
        for (const auto& [bar, n, qty] : entries)
            if (bar == step)
                p.events.push_back(entry(8, step, "L" + std::to_string(n), qty,
                                         "BUY_L" + std::to_string(n)));
        for (const auto& [bar, n] : closes)
            if (bar == step)
                p.events.push_back(close_at(8, step, "L" + std::to_string(n), kLoopSite,
                                            "TP_L" + std::to_string(n)));
    }
    p.events.push_back(cleanup(8, 20));
    return p;
}

// The w3bf02 probes (tools/closeseq.py): entries "BUY_<id>" through one call
// site, loop closes "TP_<id>" through one loop site, sole closes
// "SOLE_<id>" through a second site; cleanup at step 20.
struct Seq {
    int step;
    char kind;  // 'E' entry, 'C' loop close, 'S' sole close
    const char* id;
    double qty;
};
Probe close_sequence(const char* tape, bool pooc, int pyramiding, const std::vector<Seq>& seq) {
    Probe p{tape, pooc, pyramiding, {}};
    for (const Seq& s : seq) {
        if (s.kind == 'E') p.events.push_back(entry(8, s.step, s.id, s.qty, std::string("BUY_") + s.id));
        if (s.kind == 'C') p.events.push_back(close_at(8, s.step, s.id, kLoopSite, std::string("TP_") + s.id));
        if (s.kind == 'S') p.events.push_back(close_at(8, s.step, s.id, kSoleSite, std::string("SOLE_") + s.id));
    }
    p.events.push_back(cleanup(8, 20));
    return p;
}

// The w3f02-g probes: every cell runs on 2025-04-08, 09 and 10 (UTC), each
// strategy.close line its own call site; cleanup at 05:00.
Probe cells(const char* tape, int pyramiding, const std::vector<Event>& day_events) {
    Probe p{tape, true, pyramiding, {}};
    for (unsigned day : {8u, 9u, 10u}) {
        for (Event e : day_events) {
            e.day = day;
            p.events.push_back(e);
        }
        p.events.push_back(cleanup(day, 20));
    }
    return p;
}
int hm(int hour, int minute) { return (hour * 60 + minute) / 15; }

std::vector<Probe> probes() {
    std::vector<Probe> out;
    out.push_back(grid_sequence("w3f02-x2-min-5call",
        {{0, 38, 0.19}, {0, 39, 0.2}, {0, 40, 0.2}, {0, 41, 0.2}, {0, 42, 0.2}, {0, 43, 0.2},
         {1, 44, 0.21}, {1, 45, 0.21}, {1, 46, 0.21},
         {2, 47, 0.21}, {2, 48, 0.22}, {2, 49, 0.22}},
        {{3, 45}, {3, 46}, {3, 47}, {3, 48}, {3, 49}, {13, 38}}));
    out.push_back(grid_sequence("w3f02-x1-xlm-sequence",
        {{0, 38, 0.19}, {0, 39, 0.2}, {0, 40, 0.2}, {0, 41, 0.2}, {0, 42, 0.2}, {0, 43, 0.2},
         {1, 44, 0.21}, {1, 45, 0.21}, {1, 46, 0.21},
         {2, 47, 0.21}, {2, 48, 0.22}, {2, 49, 0.22},
         {4, 45, 0.21}, {4, 46, 0.21}, {4, 47, 0.21}, {7, 43, 0.2}, {8, 44, 0.21},
         {14, 36, 0.19}},
        {{3, 45}, {3, 46}, {3, 47}, {3, 48}, {3, 49}, {5, 47}, {6, 43}, {6, 44}, {6, 45},
         {6, 46}, {9, 44}, {10, 43}, {11, 42}, {12, 39}, {12, 40}, {12, 41}, {13, 38},
         {15, 36}}));
    out.push_back(close_sequence("w3bf02-a1-spill-oldest-record", true, 200,
        {{0, 'E', "B", 0.1}, {1, 'E', "A", 0.2}, {2, 'E', "C", 0.3}, {2, 'E', "D", 0.5},
         {3, 'C', "C", 0}, {3, 'C', "D", 0}, {5, 'S', "A", 0}, {7, 'S', "B", 0},
         {9, 'S', "D", 0}}));
    out.push_back(close_sequence("w3bf02-a2-loop-reversed", true, 200,
        {{0, 'E', "B", 0.1}, {1, 'E', "A", 0.2}, {2, 'E', "C", 0.3}, {2, 'E', "D", 0.5},
         {3, 'C', "D", 0}, {3, 'C', "C", 0}, {5, 'S', "A", 0}, {7, 'S', "B", 0},
         {9, 'S', "D", 0}, {11, 'S', "C", 0}}));
    out.push_back(close_sequence("w3bf02-b1-reentry-record-age", true, 200,
        {{0, 'E', "A", 0.1}, {1, 'E', "B", 0.2}, {2, 'S', "A", 0}, {3, 'E', "A", 0.1},
         {4, 'E', "C", 0.1}, {4, 'E', "D", 0.3}, {5, 'C', "C", 0}, {5, 'C', "D", 0},
         {7, 'S', "A", 0}, {9, 'S', "B", 0}, {11, 'S', "D", 0}}));
    out.push_back(close_sequence("w3bf02-c1-first-call-never-entered", true, 200,
        {{0, 'E', "A", 0.1}, {1, 'E', "Y", 0.2}, {2, 'E', "Z", 0.3}, {3, 'C', "N", 0},
         {3, 'C', "Y", 0}, {5, 'S', "A", 0}, {7, 'S', "Y", 0}, {9, 'S', "Z", 0}}));
    out.push_back(close_sequence("w3bf02-c2-last-call-never-entered", true, 200,
        {{0, 'E', "A", 0.1}, {1, 'E', "Y", 0.2}, {2, 'E', "Z", 0.3}, {3, 'C', "Y", 0},
         {3, 'C', "N", 0}, {5, 'S', "A", 0}, {7, 'S', "Y", 0}, {9, 'S', "Z", 0}}));
    out.push_back(close_sequence("w3bf02-c3-first-call-closed-by-name", true, 200,
        {{0, 'E', "A", 0.1}, {1, 'E', "Y", 0.2}, {2, 'E', "Z", 0.3}, {2, 'E', "W", 0.1},
         {3, 'S', "W", 0}, {5, 'C', "W", 0}, {5, 'C', "Y", 0}, {7, 'S', "A", 0},
         {9, 'S', "Y", 0}, {11, 'S', "Z", 0}}));
    out.push_back(close_sequence("w3bf02-e1-pyramiding-refused", true, 3,
        {{0, 'E', "A", 0.1}, {1, 'E', "B", 0.2}, {2, 'E', "C", 0.3}, {3, 'E', "D", 0.4},
         {5, 'C', "D", 0}, {5, 'C', "C", 0}, {7, 'S', "A", 0}, {9, 'S', "B", 0},
         {11, 'S', "D", 0}}));
    out.push_back(close_sequence("w3bf02-e2-offgrid-qty", true, 200,
        {{0, 'E', "A", 0.123456}, {0, 'E', "B", 0.2}, {1, 'E', "C", 0.1},
         {1, 'E', "D", 0.300049}, {2, 'C', "C", 0}, {2, 'C', "D", 0}, {4, 'S', "B", 0},
         {6, 'S', "A", 0}, {8, 'S', "D", 0}}));

    // w3f02-g2 .. g9 (lane W3-ENG-EXIT-ALLOC): the single-unit controls.
    out.push_back(cells("w3f02-g2-pooc-close-after-fifo-consumed", 5,
        {entry(0, hm(0, 0), "A", 1, "A"), entry(0, hm(1, 0), "B", 2, "B"),
         close_at(0, hm(2, 0), "B", 1, "close B first"), close_at(0, hm(3, 0), "A", 2, "close A"),
         close_at(0, hm(4, 0), "B", 3, "close B again")}));
    out.push_back(cells("w3f02-g3-pooc-close-remaining-fragment", 5,
        {entry(0, hm(0, 0), "A", 2, "A"), entry(0, hm(1, 0), "B", 1, "B"),
         close_at(0, hm(2, 0), "B", 1, "close B"), close_at(0, hm(3, 0), "A", 2, "close A")}));
    const std::vector<Event> four = {
        entry(0, hm(0, 0), "L1", 1, "L1"), entry(0, hm(0, 15), "L2", 1, "L2"),
        entry(0, hm(0, 30), "L3", 1, "L3"), entry(0, hm(0, 45), "L4", 1, "L4")};
    auto with = [](std::vector<Event> base, const std::vector<Event>& more) {
        base.insert(base.end(), more.begin(), more.end());
        return base;
    };
    out.push_back(cells("w3f02-g4-pooc-loop-close-consumed-ids", 5, with(four,
        {close_at(0, hm(2, 0), "L3", 1, "close L3"), close_at(0, hm(2, 15), "L4", 2, "close L4"),
         close_at(0, hm(4, 0), "L1", 3, "loop close L1"),
         close_at(0, hm(4, 0), "L2", 3, "loop close L2")})));
    out.push_back(cells("w3f02-g5-pooc-two-sites-close-consumed-ids", 5, with(four,
        {close_at(0, hm(2, 0), "L3", 1, "close L3"), close_at(0, hm(2, 15), "L4", 2, "close L4"),
         close_at(0, hm(4, 0), "L1", 3, "close L1"), close_at(0, hm(4, 0), "L2", 4, "close L2")})));
    out.push_back(cells("w3f02-g6-pooc-close-id-loop-survivor-consumed", 5, with(four,
        {close_at(0, hm(2, 0), "L3", 1, "loop close L3"),
         close_at(0, hm(2, 0), "L4", 1, "loop close L4"),
         close_at(0, hm(3, 0), "L1", 2, "close L1")})));
    out.push_back(cells("w3f02-g7-pooc-close-id-sole-close-consumed", 5, with(four,
        {close_at(0, hm(2, 0), "L4", 1, "close L4"), close_at(0, hm(3, 0), "L1", 2, "close L1")})));
    const std::vector<Event> three = {
        entry(0, hm(0, 0), "L1", 1, "L1"), entry(0, hm(0, 0), "L2", 1, "L2"),
        entry(0, hm(0, 0), "L3", 1, "L3")};
    out.push_back(cells("w3f02-g8-pooc-loop-entries-close-first", 5, with(three,
        {close_at(0, hm(2, 0), "L3", 1, "close L3"), close_at(0, hm(3, 0), "L1", 2, "close L1")})));
    out.push_back(cells("w3f02-g9-pooc-loop-entries-close-second", 5, with(three,
        {close_at(0, hm(2, 0), "L3", 1, "close L3"), close_at(0, hm(3, 0), "L2", 2, "close L2")})));
    return out;
}

// The tape's closed quantity, in lots, on 2025-04-08 at `step` (all signals).
long long closed_at(const std::vector<Row>& tape, int step) {
    long long total = 0;
    for (const Row& r : tape)
        if (std::get<4>(r) == at(8, step)) total += std::get<3>(r);
    return total;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;

    std::map<std::string, std::vector<Row>> tapes;
    for (const Probe& probe : probes()) {
        std::printf("-- %s\n", probe.tape);
        const std::vector<Row> tape = tape_trades(probe.tape, end_ms);
        tapes[probe.tape] = tape;
        CHECK(!tape.empty());
        const Run lane = run(probe, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape);
        if (lane.trades != tape) {
            show("tape", tape);
            show("engine", lane.trades);
        }
    }

    // The rules, read off TradingView's own rows (quantities in lots).
    std::printf("-- the close ledger, on the tapes\n");
    {
        // x2: of five loop calls only the last one's order fills, sized for
        // L49 (0.22), but it books against L45 (0.21): the 0.01 it cannot
        // cover comes off the oldest entry, L38, whose close ten bars later
        // closes 0.18, not its 0.19.
        const auto& x2 = tapes["w3f02-x2-min-5call"];
        CHECK(closed_at(x2, 3) == 2200);
        CHECK(closed_at(x2, 13) == 1800);
        // x1: later loops spill the rest of L38 away; its close is void.
        CHECK(closed_at(tapes["w3f02-x1-xlm-sequence"], 13) == 0);
        // a1: the spill takes the entry that filled first (B), not the
        // smallest id (A): close("A") closes A's 0.1 that the spill left and
        // close("B") nothing. a2: the reversed loop books D (0.5) for C's
        // 0.3, leaving D 0.2 and C whole.
        const auto& a1 = tapes["w3bf02-a1-spill-oldest-record"];
        CHECK(closed_at(a1, 3) == 5000);
        CHECK(closed_at(a1, 5) == 1000);
        CHECK(closed_at(a1, 7) == 0);
        const auto& a2 = tapes["w3bf02-a2-loop-reversed"];
        CHECK(closed_at(a2, 3) == 3000);
        CHECK(closed_at(a2, 9) == 2000);
        CHECK(closed_at(a2, 11) == 3000);
        // b1: a re-entered id's new units spill after every older entry's.
        const auto& b1 = tapes["w3bf02-b1-reentry-record-age"];
        CHECK(closed_at(b1, 7) == 1000);
        CHECK(closed_at(b1, 9) == 0);
        // c1/c2/c3: a call whose id has nothing unbooked is no call -- first
        // or last in the loop, never entered or closed by name before.
        CHECK(tapes["w3bf02-c1-first-call-never-entered"]
              == tapes["w3bf02-c2-last-call-never-entered"]);
        CHECK(closed_at(tapes["w3bf02-c1-first-call-never-entered"], 3) == 2000);
        CHECK(closed_at(tapes["w3bf02-c1-first-call-never-entered"], 7) == 0);
        CHECK(closed_at(tapes["w3bf02-c3-first-call-closed-by-name"], 5) == 2000);
        CHECK(closed_at(tapes["w3bf02-c3-first-call-closed-by-name"], 9) == 0);
        // e1: an entry pyramiding refuses enters nothing to close.
        CHECK(closed_at(tapes["w3bf02-e1-pyramiding-refused"], 5) == 3000);
        CHECK(closed_at(tapes["w3bf02-e1-pyramiding-refused"], 11) == 0);
        // e2: the ledger holds the entries' quantities on the lot grid.
        CHECK(closed_at(tapes["w3bf02-e2-offgrid-qty"], 4) == 1234);
        CHECK(closed_at(tapes["w3bf02-e2-offgrid-qty"], 6) == 0);
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
