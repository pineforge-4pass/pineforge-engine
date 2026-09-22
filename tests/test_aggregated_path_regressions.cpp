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

}  // namespace

int main() {
    test_held_bars();
    test_next_open_fills();

    std::printf("\naggregated_path_regressions: %d passed, %d failed\n", tests_passed,
                tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
