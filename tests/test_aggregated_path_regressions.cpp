/*
 * test_aggregated_path_regressions.cpp — R5 lane F1.
 *
 * The final audit (AUDIT3-opus, report sections 3.2 and 7 F1) reproduced three
 * Pine-visible regressions that R4 slice C (73817c1d) introduced and that no
 * corpus or population probe can see: no aggregated probe reads a bar index,
 * both aggregated corpus probes run with process_orders_on_close off, and the
 * two SPY session probes have no baseline rows. The oracle is therefore the
 * pre-R4 engine ab9714be, whose outputs the rows below pin verbatim, plus
 * TradingView's own tapes (tests/fixtures/session_islastbar).
 *
 * (a) On a PLAIN aggregated chart — script timeframe coarser than the input
 *     feed, no bar magnifier — the trade rows carried the kernel's interval
 *     index, which on that path is the INPUT bar its bucket opens on (390 for
 *     the 26th 15m bar of a 1m feed), not the chart bar a Pine script counts.
 *     `bar_index - strategy.opentrades.entry_bar_index(0) >= 2` read -364 and
 *     never fired: one trade stayed open where ab9714be and TradingView close
 *     two. The magnified path was right only because
 *     PineStrategyHost::on_native_applied re-stamped its rows.
 * (b) A process_orders_on_close fill on an aggregated chart was dated at the
 *     bucket's CLOSE (20:00Z) where the tape and ab9714be date it at the
 *     script bar's open (19:45Z). So was every fill on a plain aggregated
 *     bar's close leg (lane E27's finding 3).
 * (c) A calc_on_order_fills recalculation published the PREVIOUS bar's
 *     session flags: the scheduler computed them only in the close callback,
 *     after the recalculation had already run. On the chart-timeframe path
 *     the close callback then looked one retained bar too far ahead once a
 *     recalculation had counted the bar, flagging session.islastbar on the
 *     bar before a session's last bar and session.isfirstbar on the last.
 *
 * Ruling (supervisor, ADR-0001 rule 2): all three are TradingView presentation
 * conventions of the adapter, fixed in the source layer; the kernel's
 * calculation instant, its interval index and every hash recipe are
 * untouched, no epoch moves.
 *
 * The chart-timeframe rows are the control: they pass before and after.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
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

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/session_islastbar/bars.inc"

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kDay = 1440 * kMinute;
// 2026-04-07 (Tuesday, EDT) 09:30 America/New_York.
constexpr std::int64_t kTue0930Et = 1775568600000LL;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

const std::string kRth = "0930-1600";
const std::string kNewYork = "America/New_York";

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t utc_ms(int y, int mo, int d, int h, int mi) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h) * 60 + mi) * kMinute;
}

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

// One booked trade row, as Pine's strategy.closedtrades.* accessors read it.
struct Row {
    bool is_long = true;
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double qty = 0.0;
    double pnl = 0.0;
    double runup = 0.0;
    double drawdown = 0.0;
    int entry_bar = 0;
    int exit_bar = 0;
    std::string entry_id;
    std::string exit_id;
};

bool operator==(const Row& a, const Row& b) {
    return a.is_long == b.is_long && a.entry_ms == b.entry_ms && a.exit_ms == b.exit_ms
        && a.entry_price == b.entry_price && a.exit_price == b.exit_price && a.qty == b.qty
        && a.pnl == b.pnl && a.runup == b.runup && a.drawdown == b.drawdown
        && a.entry_bar == b.entry_bar && a.exit_bar == b.exit_bar
        && a.entry_id == b.entry_id && a.exit_id == b.exit_id;
}

struct Run {
    std::vector<Row> closed;
    int open_count = 0;
    std::int64_t open_entry_ms = 0;
    double open_entry_price = 0.0;
    int open_entry_bar = 0;
    double net_profit = 0.0;
    int callbacks = 0;
    std::string error;
};

// The three ways one chart reaches a script: its 15m bars fed as they are,
// and the fixture's 1m bars aggregated to 15m, plain and under the bar
// magnifier. Every 15m chart bar of the fixture is the aggregate of its
// fifteen 1m bars, so a script sees the same chart on all three.
enum class Path { Chart, Aggregated, Magnified };
const Path kPaths[] = {Path::Chart, Path::Aggregated, Path::Magnified};

const char* path_name(Path path) {
    switch (path) {
        case Path::Chart: return "chart 15 -> 15";
        case Path::Aggregated: return "aggregated 1 -> 15";
        case Path::Magnified: return "aggregated 1 -> 15, magnifier";
    }
    return "?";
}

template <class Host>
void run_path(Host& host, Path path) {
    static const std::vector<Bar> fifteen = feed(kFord15);
    static const std::vector<Bar> one = feed(kFord1m);
    if (path == Path::Chart) {
        host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    } else {
        host.run(one.data(), static_cast<int>(one.size()), "1", "15",
                 path == Path::Magnified);
    }
}

// The reviewer's probe hosts share one configuration: 100 shares fixed, one
// position, full margin, NYSE regular hours, a one-cent tick.
class ProbeHost : public source::PineStrategyHost {
public:
    ProbeHost(bool pooc, bool coof, int slippage = 0, int pyramiding = 1,
              double commission_percent = 0.0) {
        set_syminfo_session(kRth);
        set_syminfo_timezone(kNewYork);
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 100;
        config.pyramiding = pyramiding;
        config.process_orders_on_close = pooc;
        config.calc_on_order_fills = coof;
        config.slippage = slippage;
        config.commission_value = commission_percent;
        config.margin_long = 100;
        config.margin_short = 100;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }

    Run read() const {
        Run run;
        run.error = last_error();
        for (int i = 0; i < trade_count(); ++i) {
            const Trade& t = trades_[static_cast<std::size_t>(i)];
            run.closed.push_back(Row{t.is_long, closed_trade_entry_time(i),
                                     closed_trade_exit_time(i), closed_trade_entry_price(i),
                                     closed_trade_exit_price(i), closed_trade_size(i),
                                     closed_trade_profit(i), closed_trade_max_runup(i),
                                     closed_trade_max_drawdown(i),
                                     closed_trade_entry_bar_index(i),
                                     closed_trade_exit_bar_index(i), closed_trade_entry_id(i),
                                     closed_trade_exit_id(i)});
        }
        run.open_count = position_entry_count_;
        if (run.open_count > 0) {
            run.open_entry_ms = open_trade_entry_time(0);
            run.open_entry_price = open_trade_entry_price(0);
            run.open_entry_bar = open_trade_entry_bar_index(0);
        }
        run.net_profit = net_profit_sum_;
        run.callbacks = callbacks_;
        return run;
    }

protected:
    int callbacks_ = 0;
};

void show(const char* tag, const Run& run) {
    std::printf("    %-30s closed=%zu open=%d err='%s'\n", tag, run.closed.size(),
                run.open_count, run.error.c_str());
    for (const Row& row : run.closed) {
        std::printf("      entry %lld @%.2f bar %-4d exit %lld @%.2f bar %d  %s/%s\n",
                    static_cast<long long>(row.entry_ms), row.entry_price, row.entry_bar,
                    static_cast<long long>(row.exit_ms), row.exit_price, row.exit_bar,
                    row.entry_id.c_str(), row.exit_id.c_str());
    }
    if (run.open_count > 0) {
        std::printf("      open  %lld @%.2f bar %d\n", static_cast<long long>(run.open_entry_ms),
                    run.open_entry_price, run.open_entry_bar);
    }
}

// The engine's fill price is the tick-grid product k * mintick; compare a
// probe's two-decimal price within half a tick.
bool near(double engine, double expected) { return std::fabs(engine - expected) < 0.005; }

bool row_is(const Row& row, std::int64_t entry_ms, double entry_price, int entry_bar,
            std::int64_t exit_ms, double exit_price, int exit_bar) {
    return row.entry_ms == entry_ms && near(row.entry_price, entry_price)
        && row.entry_bar == entry_bar && row.exit_ms == exit_ms
        && near(row.exit_price, exit_price) && row.exit_bar == exit_bar;
}

// The NYSE:F bars TradingView flagged inside the replayed days (tape
// e25-f-islastbar): 2025-07-02 19:45Z, the 2025-07-03 half day's 16:45Z and
// 2025-07-07 19:45Z. The reviewer's probes enter on them.
std::set<std::int64_t> ford_entry_bars() {
    return {utc_ms(2025, 7, 2, 19, 45), utc_ms(2025, 7, 3, 16, 45),
            utc_ms(2025, 7, 7, 19, 45)};
}

// ── 1. bar_index - strategy.opentrades.entry_bar_index(0) >= 2 ────────────

// The reviewer's heldbars_probe.cpp: a market entry on each flagged bar,
// closed once the position has been held two chart bars.
class HeldBarsHost final : public ProbeHost {
public:
    HeldBarsHost() : ProbeHost(/*pooc=*/false, /*coof=*/false) {}
    void on_source_bar(const Bar&) override {
        ++callbacks_;
        // Pine: if strategy.opentrades > 0 and
        //        bar_index - strategy.opentrades.entry_bar_index(0) >= 2
        if (position_entry_count_ > 0) {
            const int held = bar_index_ - open_trade_entry_bar_index(0);
            held_.push_back(held);
            if (held >= 2) strategy_close("L", "held2");
        }
        if (entries_.count(current_bar_.timestamp) && position_entry_count_ == 0)
            strategy_entry("L", true);
    }
    std::vector<int> held_;

private:
    std::set<std::int64_t> entries_ = ford_entry_bars();
};

// ab9714be, heldbars_ab9714be.out, identical on all three paths:
//   closed=2 open=0
//   entry 07-03 13:30Z @11.75 bar 26   exit 07-03 14:15Z @11.87 bar 29
//   entry 07-07 13:30Z @11.62 bar 40   exit 07-07 14:15Z @11.76 bar 43
//   log: [bi=26 ebi=26 held=0] [bi=27 ebi=26 held=1]
// Before this lane the plain aggregated run read ebi=390, held=-364, and kept
// one position open to the end: closed=0 open=1, open 07-03 13:30Z bar 390.
void test_held_bars() {
    std::printf("test_held_bars\n");
    for (const Path path : kPaths) {
        HeldBarsHost host;
        run_path(host, path);
        const Run run = host.read();
        CHECK(run.error.empty());
        CHECK(run.closed.size() == 2);
        CHECK(run.open_count == 0);
        if (run.closed.size() != 2 || run.open_count != 0) {
            show(path_name(path), run);
            continue;
        }
        CHECK(row_is(run.closed[0], utc_ms(2025, 7, 3, 13, 30), 11.75, 26,
                     utc_ms(2025, 7, 3, 14, 15), 11.87, 29));
        CHECK(row_is(run.closed[1], utc_ms(2025, 7, 7, 13, 30), 11.62, 40,
                     utc_ms(2025, 7, 7, 14, 15), 11.76, 43));
        // Held 0, 1, then 2 chart bars, for each of the two trades.
        CHECK((host.held_ == std::vector<int>{0, 1, 2, 0, 1, 2}));
        if (host.held_ != std::vector<int>{0, 1, 2, 0, 1, 2}) show(path_name(path), run);
    }
}

// ── 2. fills at the next bar's open ───────────────────────────────────────

// The reviewer's nonpooc_probe.cpp: enter on each flagged bar, close on the
// next callback; both fill at the next bar's open.
class NextBarHost final : public ProbeHost {
public:
    explicit NextBarHost(bool pooc) : ProbeHost(pooc, /*coof=*/false) {}
    void on_source_bar(const Bar&) override {
        ++callbacks_;
        if (position_entry_count_ > 0) strategy_close("L", "next");
        if (entries_.count(current_bar_.timestamp)) strategy_entry("L", true);
    }

private:
    std::set<std::int64_t> entries_ = ford_entry_bars();
};

// ab9714be, nonpooc_ab9714be.out, identical on all three paths:
//   entry 07-03 13:30Z @11.75 bar 26   exit 07-03 13:45Z @11.95 bar 27
//   entry 07-07 13:30Z @11.62 bar 40   exit 07-07 13:45Z @11.68 bar 41
// Before this lane the plain aggregated rows read bars 390/405 and 600/615.
void test_next_open_fills() {
    std::printf("test_next_open_fills\n");
    for (const Path path : kPaths) {
        NextBarHost host(/*pooc=*/false);
        run_path(host, path);
        const Run run = host.read();
        CHECK(run.error.empty());
        CHECK(run.closed.size() == 2);
        CHECK(run.open_count == 0);
        if (run.closed.size() != 2) {
            show(path_name(path), run);
            continue;
        }
        const bool first = row_is(run.closed[0], utc_ms(2025, 7, 3, 13, 30), 11.75, 26,
                                  utc_ms(2025, 7, 3, 13, 45), 11.95, 27);
        const bool second = row_is(run.closed[1], utc_ms(2025, 7, 7, 13, 30), 11.62, 40,
                                   utc_ms(2025, 7, 7, 13, 45), 11.68, 41);
        CHECK(first);
        CHECK(second);
        if (!first || !second) show(path_name(path), run);
    }
}

// ── 3. process_orders_on_close fills ──────────────────────────────────────

// The reviewer's pooc_probe.cpp: the same script with process_orders_on_close
// on, so every fill is at a bar's close and TradingView dates it at that
// bar's open (the e25-f-islastbar tape, lane E27).
//
// ab9714be, pooc_probe_ab9714be.out, identical on all three paths:
//   entry 07-02 19:45Z @11.77 bar 25   exit 07-03 13:30Z @11.94 bar 26
//   entry 07-03 16:45Z @11.79 bar 39   exit 07-07 13:30Z @11.68 bar 40
//   open  07-07 19:45Z @11.60 bar 65
// Before this lane the plain aggregated run dated every fill one bucket late
// and on input bars (entry 07-02 20:00Z bar 375 ...), the magnified one one
// bucket late (entry 07-02 20:00Z bar 25 ...).
void test_pooc_fills() {
    std::printf("test_pooc_fills\n");
    for (const Path path : kPaths) {
        NextBarHost host(/*pooc=*/true);
        run_path(host, path);
        const Run run = host.read();
        CHECK(run.error.empty());
        CHECK(run.closed.size() == 2);
        CHECK(run.open_count == 1);
        if (run.closed.size() != 2 || run.open_count != 1) {
            show(path_name(path), run);
            continue;
        }
        const bool first = row_is(run.closed[0], utc_ms(2025, 7, 2, 19, 45), 11.77, 25,
                                  utc_ms(2025, 7, 3, 13, 30), 11.94, 26);
        const bool second = row_is(run.closed[1], utc_ms(2025, 7, 3, 16, 45), 11.79, 39,
                                   utc_ms(2025, 7, 7, 13, 30), 11.68, 40);
        const bool open = run.open_entry_ms == utc_ms(2025, 7, 7, 19, 45)
            && near(run.open_entry_price, 11.60) && run.open_entry_bar == 65;
        CHECK(first);
        CHECK(second);
        CHECK(open);
        if (!first || !second || !open) show(path_name(path), run);
    }
}

// ── 4. a plain aggregated chart is the chart ──────────────────────────────

// Beyond the three probes: a script that places priced entries with same-bar
// brackets, a limit short, a market entry with a priced from_entry leg and a
// market pyramid add under it, and a periodic close_all, under every
// combination of process_orders_on_close, calc_on_order_fills and slippage.
// The chart a script sees does not depend on the timeframe its bars were fed
// at, so the plain aggregated run must book the chart run's rows exactly:
// the same fills, prices, P&L, excursions, ids — and, since this lane, the
// same instants and chart-bar indices. Before this lane every field but those
// two already agreed; the re-stamp must not move any of the others (the
// entry-bar masks, the slippage mask, the KI-62 same-bar add cover and the
// calc_on_order_fills extreme sample all compare a lot's entry bar with the
// current one).
class BracketsHost final : public ProbeHost {
public:
    BracketsHost(bool pooc, bool coof, int slippage, int pyramiding)
        : ProbeHost(pooc, coof, slippage, pyramiding, /*commission_percent=*/0.01) {}
    void on_source_bar(const Bar& bar) override {
        ++callbacks_;
        const int k = bar_index_;
        const double units = physical_position().signed_units;
        if (k % 7 == 3 && units == 0.0) {
            strategy_entry("PL", true, kNaN, bar.high - 0.02);
            strategy_exit("PX", "PL", bar.close + 0.06, bar.close - 0.06);
        }
        if (k % 9 == 4 && units == 0.0) {
            strategy_entry("PS", false, bar.close + 0.02);
            strategy_exit("SX", "PS", bar.close - 0.05, bar.close + 0.07);
        }
        if (k % 13 == 6 && units == 0.0) {
            strategy_entry("ML", true);
            strategy_exit("MX", "ML", bar.close + 0.08, bar.close - 0.08);
        }
        if (k % 13 == 7 && units > 0.0) strategy_entry("ML", true);
        if (k % 17 == 16 && units != 0.0) strategy_close_all();
    }
};

void test_aggregated_equals_chart() {
    std::printf("test_aggregated_equals_chart\n");
    struct Variant { bool pooc; bool coof; int slippage; int pyramiding; };
    const Variant variants[] = {
        {false, false, 0, 1}, {true, false, 0, 1}, {false, true, 0, 1}, {true, true, 0, 1},
        {false, false, 2, 3}, {true, false, 2, 3}, {false, true, 2, 3}, {true, true, 2, 3},
    };
    for (const Variant& v : variants) {
        BracketsHost chart_host(v.pooc, v.coof, v.slippage, v.pyramiding);
        run_path(chart_host, Path::Chart);
        BracketsHost agg_host(v.pooc, v.coof, v.slippage, v.pyramiding);
        run_path(agg_host, Path::Aggregated);
        const Run chart = chart_host.read();
        const Run agg = agg_host.read();
        CHECK(chart.error.empty());
        CHECK(agg.error.empty());
        CHECK(chart.closed.size() >= 12);
        CHECK(agg.callbacks == chart.callbacks);
        CHECK(agg.net_profit == chart.net_profit);
        CHECK(agg.closed.size() == chart.closed.size());
        CHECK(agg.open_count == chart.open_count);
        bool rows_equal = agg.closed.size() == chart.closed.size();
        for (std::size_t i = 0; rows_equal && i < agg.closed.size(); ++i)
            rows_equal = agg.closed[i] == chart.closed[i];
        CHECK(rows_equal);
        if (!rows_equal) {
            std::printf("    pooc=%d coof=%d slippage=%d pyramiding=%d\n", v.pooc, v.coof,
                        v.slippage, v.pyramiding);
            for (std::size_t i = 0; i < agg.closed.size() && i < chart.closed.size(); ++i) {
                if (agg.closed[i] == chart.closed[i]) continue;
                const Row& a = agg.closed[i];
                const Row& c = chart.closed[i];
                std::printf("      [%zu] chart %lld..%lld bar %d..%d ru %.4f dd %.4f\n"
                            "          agg   %lld..%lld bar %d..%d ru %.4f dd %.4f\n",
                            i, static_cast<long long>(c.entry_ms),
                            static_cast<long long>(c.exit_ms), c.entry_bar, c.exit_bar,
                            c.runup, c.drawdown, static_cast<long long>(a.entry_ms),
                            static_cast<long long>(a.exit_ms), a.entry_bar, a.exit_bar,
                            a.runup, a.drawdown);
            }
        }
    }
}

// The KI-62 cover (ab9714be pine_fills.cpp:7026-7033), which the brackets
// script above never reaches: a same-id MARKET add opened on the bar a priced
// from_entry leg fills is still open behind the leg's FIFO reduction, and the
// owner covers it at the leg's price as a second fill of the same order. The
// adapter finds those adds by comparing each lot's entry bar with the fill's
// bar, so the plain aggregated chart must state both on the chart bar. The
// shape of test_l10as_priced_exit_fill's EUR/USD add-on case on NYSE:F: two
// units at the bar-36 open, a two-unit add at the bar-43 open, and two
// one-unit legs whose re-issued stop (11.72) bar 43 crosses after its open
// (11.755, low 11.695).
class AddOnHost final : public ProbeHost {
public:
    AddOnHost() : ProbeHost(/*pooc=*/false, /*coof=*/false, /*slippage=*/0, /*pyramiding=*/2) {}
    void on_source_bar(const Bar& bar) override {
        ++callbacks_;
        const int i = bar_index_;
        if (i != 35 && i != 42) return;
        const double stop = i == 35 ? bar.close - 5.0 : 11.72;
        strategy_entry("Long", true, kNaN, kNaN, 2.0);
        strategy_exit("LongT1", "Long", bar.close + 5.0, stop, kNaN, kNaN, kNaN, 100.0,
                      "T1 Exit", 1.0);
        strategy_exit("LongT2", "Long", bar.close + 5.0, stop, kNaN, kNaN, kNaN, 100.0,
                      "T2 Exit", 1.0);
    }
};

void test_same_bar_add_cover() {
    std::printf("test_same_bar_add_cover\n");
    AddOnHost chart_host;
    run_path(chart_host, Path::Chart);
    AddOnHost agg_host;
    run_path(agg_host, Path::Aggregated);
    const Run chart = chart_host.read();
    const Run agg = agg_host.read();
    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    // The control: the carried lot's first unit, the cover of the whole add,
    // then the carried lot's last unit, all at the stop on bar 43.
    CHECK(chart.closed.size() == 3);
    CHECK(chart.open_count == 0);
    if (chart.closed.size() == 3) {
        const double qty[] = {1.0, 2.0, 1.0};
        const int entry_bar[] = {36, 43, 36};
        for (std::size_t k = 0; k < 3; ++k) {
            CHECK(chart.closed[k].qty == qty[k]);
            CHECK(chart.closed[k].entry_bar == entry_bar[k]);
            CHECK(chart.closed[k].exit_bar == 43);
            CHECK(near(chart.closed[k].exit_price, 11.72));
        }
    } else {
        show(path_name(Path::Chart), chart);
    }
    bool rows_equal = agg.closed.size() == chart.closed.size();
    for (std::size_t i = 0; rows_equal && i < agg.closed.size(); ++i)
        rows_equal = agg.closed[i] == chart.closed[i];
    CHECK(rows_equal);
    CHECK(agg.open_count == chart.open_count);
    if (!rows_equal) show(path_name(Path::Aggregated), agg);
}

// ── 5. session flags under calc_on_order_fills ────────────────────────────

// The reviewer's coof_session.cpp: a market order placed on a session day's
// last bar fills at the next session day's first bar open, and the
// calc_on_order_fills recalculation runs the script ON that bar — where
// session.isfirstbar holds and session.islastbar does not. Chart-timeframe
// bars, 09:30 .. 15:45 ET (or .. 16:45 with post-market bars) on Tuesday
// 2026-04-07 and Wednesday 2026-04-08.
struct Seen {
    std::int64_t ts = 0;
    int bar = 0;
    bool ismarket = false;
    bool isfirstbar = false;
    bool islastbar = false;
    int open_entries = 0;
};

bool operator==(const Seen& a, const Seen& b) {
    return a.ts == b.ts && a.bar == b.bar && a.ismarket == b.ismarket
        && a.isfirstbar == b.isfirstbar && a.islastbar == b.islastbar;
}

class SessionFlagsHost final : public source::PineStrategyHost {
public:
    SessionFlagsHost(bool coof, int entry_minute_et) : entry_minute_et_(entry_minute_et) {
        set_syminfo_session(kRth);
        set_syminfo_timezone(kNewYork);
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        config.pyramiding = 10;
        config.calc_on_order_fills = coof;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        seen.push_back({current_bar_.timestamp, bar_index_, session_ismarket_,
                        session_isfirstbar_, session_islastbar_, position_entry_count_});
        const int et = static_cast<int>(
            ((current_bar_.timestamp / kMinute - 240) % 1440 + 1440) % 1440);
        if (et == entry_minute_et_ && !entered_) {
            strategy_entry("L", true);
            entered_ = true;
        }
    }
    std::vector<Seen> seen;

private:
    int entry_minute_et_;
    bool entered_ = false;
};

Bar session_bar(std::int64_t ts, double px) {
    Bar b{};
    b.timestamp = ts;
    b.open = px; b.high = px + 0.2; b.low = px - 0.2; b.close = px + 0.1;
    b.volume = 1;
    return b;
}

// Two session days of 15m chart bars from 09:30 ET; `per_day` = 26 ends at
// 15:45, 30 adds the out-of-session 16:00 .. 16:45 bars.
std::vector<Bar> two_days(int per_day) {
    std::vector<Bar> bars;
    double px = 100.0;
    for (const std::int64_t day : {kTue0930Et, kTue0930Et + kDay}) {
        for (int k = 0; k < per_day; ++k) {
            bars.push_back(session_bar(day + k * 15 * kMinute, px));
            px += 0.05;
        }
    }
    return bars;
}

std::vector<Seen> run_flags(bool coof, int per_day, int entry_minute_et, std::string* error) {
    SessionFlagsHost host(coof, entry_minute_et);
    const auto bars = two_days(per_day);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    *error = host.last_error();
    return host.seen;
}

std::string flags_text(const std::vector<Seen>& seen, std::int64_t from, std::int64_t to) {
    std::string text;
    for (const Seen& s : seen) {
        if (s.ts < from || s.ts > to) continue;
        const int et = static_cast<int>(((s.ts / kMinute - 240) % 1440 + 1440) % 1440);
        char cell[64];
        std::snprintf(cell, sizeof cell, " %02d:%02d[bi%d m%d f%d l%d pos%d]", et / 60, et % 60,
                      s.bar, s.ismarket, s.isfirstbar, s.islastbar, s.open_entries);
        text += cell;
    }
    return text;
}

// Every callback of one bar reads the same flags, and collapsing the
// recalculation into its bar gives the calc_on_order_fills-off run back:
// ab9714be's shape (coof_session_ab9714be.out), where both callbacks of the
// fill bar agree and the COOF run reads the COOF-off run's flags bar for bar.
bool one_answer_per_bar(const std::vector<Seen>& coof_on, const std::vector<Seen>& coof_off) {
    std::vector<Seen> collapsed;
    for (const Seen& s : coof_on) {
        if (!collapsed.empty() && collapsed.back().bar == s.bar) {
            if (!(collapsed.back() == s)) return false;
            continue;
        }
        collapsed.push_back(s);
    }
    if (collapsed.size() != coof_off.size()) return false;
    for (std::size_t i = 0; i < collapsed.size(); ++i)
        if (!(collapsed[i] == coof_off[i])) return false;
    return true;
}

const Seen* callback(const std::vector<Seen>& seen, std::int64_t ts, int nth) {
    for (const Seen& s : seen) {
        if (s.ts != ts) continue;
        if (nth-- == 0) return &s;
    }
    return nullptr;
}

// The order placed at Tuesday's 15:45 fills at Wednesday's 09:30 open. The
// recalculation on that fill runs on the Wednesday 09:30 bar: isfirstbar,
// not islastbar. Before this lane it read Tuesday 15:45's flags:
//   COOF on  ... 15:45[bi25 f0 l1 pos0] 09:30[bi26 f0 l1 pos1] 09:30[bi26 f1 l0 pos1]
void test_coof_session_flags() {
    std::printf("test_coof_session_flags\n");
    std::string off_error, on_error;
    const auto off = run_flags(false, 26, 15 * 60 + 45, &off_error);
    const auto on = run_flags(true, 26, 15 * 60 + 45, &on_error);
    CHECK(off_error.empty());
    CHECK(on_error.empty());
    const std::int64_t wed0930 = kTue0930Et + kDay;
    const std::int64_t tue1545 = kTue0930Et + 375 * kMinute;
    // The control, E26's session-day flags without a recalculation.
    CHECK(off.size() == 52);
    CHECK(flags_text(off, tue1545, wed0930)
          == " 15:45[bi25 m1 f0 l1 pos0] 09:30[bi26 m1 f1 l0 pos1]");
    // The recalculation is the first of Wednesday 09:30's two callbacks.
    CHECK(on.size() == 53);
    const Seen* recalc = callback(on, wed0930, 0);
    const Seen* close = callback(on, wed0930, 1);
    CHECK(recalc != nullptr && close != nullptr);
    if (recalc && close) {
        CHECK(recalc->open_entries == 1);
        CHECK(recalc->ismarket && recalc->isfirstbar && !recalc->islastbar);
        CHECK(close->ismarket && close->isfirstbar && !close->islastbar);
    }
    CHECK(one_answer_per_bar(on, off));
    if (!one_answer_per_bar(on, off)) {
        std::printf("    COOF off%s\n", flags_text(off, tue1545, wed0930).c_str());
        std::printf("    COOF on %s\n", flags_text(on, tue1545, wed0930).c_str());
    }
}

// The same with out-of-session bars after the close: the order placed at
// 15:45 fills at 16:00 ET, out of session, where no session flag holds.
// Before this lane the recalculation read 15:45's islastbar:
//   COOF on  ... 15:45[bi25 f0 l1 pos0] 16:00[bi26 f0 l1 pos1] 16:00[bi26 f0 l0 pos1]
void test_coof_session_flags_post_market() {
    std::printf("test_coof_session_flags_post_market\n");
    std::string off_error, on_error;
    const auto off = run_flags(false, 30, 15 * 60 + 45, &off_error);
    const auto on = run_flags(true, 30, 15 * 60 + 45, &on_error);
    CHECK(off_error.empty());
    CHECK(on_error.empty());
    const std::int64_t tue1545 = kTue0930Et + 375 * kMinute;
    const std::int64_t tue1600 = kTue0930Et + 390 * kMinute;
    CHECK(flags_text(off, tue1545, tue1600)
          == " 15:45[bi25 m1 f0 l1 pos0] 16:00[bi26 m0 f0 l0 pos1]");
    const Seen* recalc = callback(on, tue1600, 0);
    CHECK(recalc != nullptr);
    if (recalc) {
        CHECK(recalc->open_entries == 1);
        CHECK(!recalc->ismarket && !recalc->isfirstbar && !recalc->islastbar);
    }
    CHECK(one_answer_per_bar(on, off));
    if (!one_answer_per_bar(on, off)) {
        std::printf("    COOF off%s\n", flags_text(off, tue1545, tue1600).c_str());
        std::printf("    COOF on %s\n", flags_text(on, tue1545, tue1600).c_str());
    }
}

// The order placed at 15:15 fills at 15:30, the bar BEFORE the session's last.
// The recalculation counts the 15:30 bar before its close callback, and the
// close callback used to read its session lookahead off that count: one
// retained bar too far, Wednesday's 09:30, a different session day. Before
// this lane (chart-timeframe path):
//   COOF on  15:30[bi24 f0 l0 pos1] 15:30[bi24 f0 l1 pos1] 15:45[bi25 f1 l1 pos1]
// — 15:30 read as the session's last bar and 15:45 as its first.
void test_coof_fill_before_the_last_bar() {
    std::printf("test_coof_fill_before_the_last_bar\n");
    std::string off_error, on_error;
    const auto off = run_flags(false, 26, 15 * 60 + 15, &off_error);
    const auto on = run_flags(true, 26, 15 * 60 + 15, &on_error);
    CHECK(off_error.empty());
    CHECK(on_error.empty());
    const std::int64_t tue1530 = kTue0930Et + 360 * kMinute;
    const std::int64_t wed0930 = kTue0930Et + kDay;
    CHECK(flags_text(off, tue1530, wed0930)
          == " 15:30[bi24 m1 f0 l0 pos1] 15:45[bi25 m1 f0 l1 pos1] 09:30[bi26 m1 f1 l0 pos1]");
    CHECK(flags_text(on, tue1530, wed0930)
          == " 15:30[bi24 m1 f0 l0 pos1] 15:30[bi24 m1 f0 l0 pos1] 15:45[bi25 m1 f0 l1 pos1]"
             " 09:30[bi26 m1 f1 l0 pos1]");
    CHECK(one_answer_per_bar(on, off));
    if (!one_answer_per_bar(on, off)) {
        std::printf("    COOF off%s\n", flags_text(off, tue1530, wed0930).c_str());
        std::printf("    COOF on %s\n", flags_text(on, tue1530, wed0930).c_str());
    }
}

}  // namespace

int main() {
    test_held_bars();
    test_next_open_fills();
    test_pooc_fills();
    test_aggregated_equals_chart();
    test_same_bar_add_cover();
    test_coof_session_flags();
    test_coof_session_flags_post_market();
    test_coof_fill_before_the_last_bar();

    std::printf("\naggregated_path_regressions: %d passed, %d failed\n", tests_passed,
                tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
