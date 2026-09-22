/*
 * test_pooc_fill_stamp_aggregation.cpp — R5 lane E27.
 *
 * Lane E25 (its report, "(f) Findings" 2): on the AGGREGATED path — the input
 * timeframe finer than the chart's, 1m bars under a 5m or 15m script — a
 * process_orders_on_close fill is stamped at the script bar's CLOSE. The
 * chart-timeframe path and TradingView stamp the same fill at the bar's OPEN.
 * Same fill, same price, same quantity; a different instant on the trade row
 * (strategy.closedtrades.entry_time / .exit_time, Trade::entry_time /
 * ::exit_time, pf_trade_t::entry_time).
 *
 * TradingView's own answer is already in this tree: the e25-f-islastbar tape
 * (tests/fixtures/session_islastbar) enters with process_orders_on_close on the
 * flagged bar and closes on the next one, so each of its rows IS a POOC fill
 * dated by TradingView. It dates every one of them at the bar's open — 15:45
 * ET on a full NYSE:F session day, 12:45 ET on a half day — never at the close.
 *
 * Mechanism (measured, not inferred; exec/E27-probes):
 * - The stamp is the kernel's fill point. NativeExecutionConsumer books a
 *   settlement at `evaluation.cursor.point.effective_time_ms`
 *   (src/native_execution_consumer.cpp, `execution::PhysicalExecutionContext
 *   ctx`), which reaches the row as Trade::exit_time and, through
 *   PyramidEntry::time, as Trade::entry_time (src/engine_orders.cpp).
 * - For the calculation point and the AfterCalculationClose point that
 *   process_orders_on_close adds, that instant is
 *   `NativeExecutionConsumer::calculation_time(base)` = the greatest of the
 *   script interval's last traded close, its next period open, and the latest
 *   close its inputs contributed. For the 19:45 UTC bucket of a 15m NYSE:F
 *   chart that is 20:00 — the bar's close.
 * - The chart-timeframe path reports the open only because it hands the kernel
 *   a DEGENERATE interval. The Pine adapter opts every source host into
 *   NativeSlotLabelPolicy::FeedTolerant (src/source/pine_adapter.cpp), and with
 *   an equal input/script pairing that selects
 *   `NativeExecutionConsumer::uses_raw_label_partition()`, whose
 *   `timestamp_partition` answers {ts, ts, ts, ts, ts}: "No duration is
 *   available in this state." calculation_time of a zero-width interval is the
 *   label itself, so on that path every fill of a bar — open, both excursion
 *   legs, the close segment and the POOC close — is stamped at the bar's open.
 *   The aggregated path must partition (it is what buckets 1m into 15m), so it
 *   gets a real interval and a real close.
 *
 * RESIDUAL, not fixed. The brief for this lane expected the stamp to be Pine
 * delivery timing inside src/source/ and said to stop and report if it came
 * from the kernel's fill point. It does. What is left to the owner is which of
 * two changes to make, and both are rulings, not a worker's call: re-stamp the
 * booked row from the adapter (PineStrategyHost::on_native_applied already does
 * exactly this for entry_bar_index / exit_bar_index, but only when the bar
 * magnifier is on — it writes hashed kernel state after the kernel booked it),
 * or change what instant the kernel attributes to a calculation point (which is
 * also the instant its FX lookup, its decision floor and its ordering read).
 * The rows below pin what is true today, so whichever change is ruled has to
 * move them on purpose.
 *
 * SECOND RESIDUAL, a separate finding of the same mechanism: on the PLAIN
 * aggregated path the trade rows carry the INPUT bar index, not the chart bar
 * index — 375 and 390 where the chart path says 25 and 26, and where the same
 * tape row says "Duration (bars)" 1. The magnified aggregated path is right
 * because PineStrategyHost::on_native_applied re-stamps it; the plain one has
 * no such writer. It is pinned here because this lane measured it, and it is
 * not this lane's target.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
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

#ifndef PINEFORGE_E27_FIXTURE_DIR
#error "PINEFORGE_E27_FIXTURE_DIR must name tests/fixtures/session_islastbar"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/session_islastbar/bars.inc"

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kFifteen = 15 * kMinute;
constexpr std::int64_t kFive = 5 * kMinute;
// 2026-04-07 (Tuesday, EDT) 09:30 America/New_York.
constexpr std::int64_t kTue0930Et = 1775568600000LL;

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

// "YYYY-MM-DD HH:MM" at a fixed UTC offset in hours -> UTC milliseconds.
std::int64_t utc_ms(int y, int mo, int d, int h, int mi, int offset_hours = 0) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - offset_hours) * 60 + mi) * kMinute;
}

// One booked trade row, as Pine's strategy.closedtrades.* accessors read it.
struct Row {
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double qty = 0.0;
    int entry_bar = 0;
    int exit_bar = 0;
};

struct Run {
    std::vector<Row> closed;
    std::int64_t open_entry_ms = 0;
    double open_entry_price = 0.0;
    int open_entry_bar = 0;
    int open_count = 0;
    std::string error;
};

// The E25 probe, as its tape ran it: enter with process_orders_on_close on the
// listed chart bars, close the position on the bar after each one.
class PoocHost final : public source::PineStrategyHost {
public:
    PoocHost(const std::string& session, const std::string& timezone,
             std::set<std::int64_t> entries)
        : entries_(std::move(entries)) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 100;
        config.pyramiding = 1;
        config.process_orders_on_close = true;
        config.margin_long = 100;
        config.margin_short = 100;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (physical_position().signed_units != 0.0) strategy_close("L", "next");
        if (entries_.count(current_bar_.timestamp)) strategy_entry("L", true);
    }

    Run read() const {
        Run run;
        run.error = last_error();
        for (int i = 0; i < trade_count(); ++i) {
            run.closed.push_back(Row{closed_trade_entry_time(i), closed_trade_exit_time(i),
                                     closed_trade_entry_price(i), closed_trade_exit_price(i),
                                     closed_trade_size(i), closed_trade_entry_bar_index(i),
                                     closed_trade_exit_bar_index(i)});
        }
        run.open_count = position_entry_count_;
        if (run.open_count > 0) {
            run.open_entry_ms = open_trade_entry_time(0);
            run.open_entry_price = open_trade_entry_price(0);
            run.open_entry_bar = open_trade_entry_bar_index(0);
        }
        return run;
    }

private:
    std::set<std::int64_t> entries_;
};

Run run_batch(const std::vector<Bar>& bars, const std::string& session,
              const std::string& timezone, const char* input_tf, const char* script_tf,
              bool magnifier, const std::set<std::int64_t>& entries) {
    PoocHost host(session, timezone, entries);
    host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf, magnifier);
    return host.read();
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

Bar flat_bar(std::int64_t ts) {
    Bar b{};
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.5;
    b.volume = 1.0;
    return b;
}

std::vector<Bar> ladder(std::int64_t first, std::int64_t last, std::int64_t step) {
    std::vector<Bar> bars;
    for (std::int64_t ts = first; ts <= last; ts += step) bars.push_back(flat_bar(ts));
    return bars;
}

// One "Entry long" / "Exit long" row of a `lab tv` tape, times UTC+8.
struct TapeRow {
    bool entry = false;
    std::int64_t ms = 0;
    double price = 0.0;
};

std::vector<TapeRow> tape(const char* slug, std::int64_t first, std::int64_t last) {
    std::ifstream in(std::string(PINEFORGE_E27_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeRow> rows;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 5) continue;
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        const bool exit_row = cell[1].rfind("Exit", 0) == 0;
        if (!entry && !exit_row) continue;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
        const std::int64_t ms = utc_ms(y, mo, d, h, mi, 8);
        if (ms < first || ms > last) continue;
        rows.push_back(TapeRow{entry, ms, std::atof(cell[4].c_str())});
    }
    return rows;
}

// The engine's fill price is the tick-grid product k * mintick, so 11.79 is
// 1179 * 0.01 = 11.790000000000001. Compare a tape price within half a tick.
bool same_price(double engine, double tape_price) {
    const double delta = engine - tape_price;
    return (delta < 0 ? -delta : delta) < 0.005;
}

void show(const char* tag, const Run& run) {
    std::printf("    %-24s closed=%zu open=%d err='%s'\n", tag, run.closed.size(),
                run.open_count, run.error.c_str());
    for (const Row& row : run.closed) {
        std::printf("      entry %lld @%g bar %d   exit %lld @%g bar %d   qty %g\n",
                    static_cast<long long>(row.entry_ms), row.entry_price, row.entry_bar,
                    static_cast<long long>(row.exit_ms), row.exit_price, row.exit_bar, row.qty);
    }
    if (run.open_count > 0) {
        std::printf("      open  %lld @%g bar %d\n",
                    static_cast<long long>(run.open_entry_ms), run.open_entry_price,
                    run.open_entry_bar);
    }
}

// The three NYSE:F bars TradingView flagged inside the replayed days: 15:45 ET
// on 2025-07-02 and 2025-07-07, 12:45 ET on the 2025-07-03 half day.
std::set<std::int64_t> ford_entry_bars() {
    return {utc_ms(2025, 7, 2, 19, 45), utc_ms(2025, 7, 3, 16, 45),
            utc_ms(2025, 7, 7, 19, 45)};
}

// ── 1. TradingView dates a POOC fill at the bar's open ────────────────────

// The control, and the settlement of what the right answer is. The tape's own
// rows for the three replayed days: entries at the flagged bar's open, exits at
// the open of the next session's first bar.
void test_tv_tape_dates_the_open() {
    std::printf("test_tv_tape_dates_the_open\n");
    const auto rows = tape("e25-f-islastbar", utc_ms(2025, 7, 2, 13, 30),
                           utc_ms(2025, 7, 7, 19, 45));
    CHECK(rows.size() == 6);
    if (rows.size() != 6) {
        for (const TapeRow& row : rows)
            std::printf("    tape %s %lld @%g\n", row.entry ? "entry" : "exit ",
                        static_cast<long long>(row.ms), row.price);
        return;
    }
    // File order: by trade number, each trade's exit above its entry. The first
    // row is the exit of the trade whose entry is 2025-07-01's flagged bar,
    // before the replayed window; the last trade's exit is after it.
    CHECK(!rows[0].entry && rows[0].ms == utc_ms(2025, 7, 2, 13, 30) && rows[0].price == 11.45);
    CHECK(!rows[1].entry && rows[1].ms == utc_ms(2025, 7, 3, 13, 30) && rows[1].price == 11.94);
    CHECK(rows[2].entry && rows[2].ms == utc_ms(2025, 7, 2, 19, 45) && rows[2].price == 11.77);
    CHECK(!rows[3].entry && rows[3].ms == utc_ms(2025, 7, 7, 13, 30) && rows[3].price == 11.68);
    CHECK(rows[4].entry && rows[4].ms == utc_ms(2025, 7, 3, 16, 45) && rows[4].price == 11.79);
    CHECK(rows[5].entry && rows[5].ms == utc_ms(2025, 7, 7, 19, 45) && rows[5].price == 11.6);
    // Every stamp is a 15m bar OPEN on the chart grid, never a bar close: a
    // close would be the next bucket's open, 20:00 / 17:00 / 13:45 UTC.
    for (const TapeRow& row : rows) CHECK(row.ms % kFifteen == 0);
    CHECK(rows[2].ms + kFifteen == utc_ms(2025, 7, 2, 20, 0));
}

// ── 2. the chart-timeframe path agrees with the tape ──────────────────────

void test_chart_path_matches_the_tape() {
    std::printf("test_chart_path_matches_the_tape\n");
    const Run chart = run_batch(feed(kFord15), kRth, kNewYork, "15", "15", false,
                                ford_entry_bars());
    CHECK(chart.error.empty());
    CHECK(chart.closed.size() == 2);
    CHECK(chart.open_count == 1);
    if (chart.closed.size() != 2 || chart.open_count != 1) { show("chart 15 -> 15", chart); return; }

    CHECK(chart.closed[0].entry_ms == utc_ms(2025, 7, 2, 19, 45));
    CHECK(same_price(chart.closed[0].entry_price, 11.77));
    CHECK(chart.closed[0].exit_ms == utc_ms(2025, 7, 3, 13, 30));
    CHECK(same_price(chart.closed[0].exit_price, 11.94));
    CHECK(chart.closed[1].entry_ms == utc_ms(2025, 7, 3, 16, 45));
    CHECK(same_price(chart.closed[1].entry_price, 11.79));
    CHECK(chart.closed[1].exit_ms == utc_ms(2025, 7, 7, 13, 30));
    CHECK(same_price(chart.closed[1].exit_price, 11.68));
    CHECK(chart.open_entry_ms == utc_ms(2025, 7, 7, 19, 45));
    CHECK(same_price(chart.open_entry_price, 11.6));
    // One chart bar from entry to exit, the tape's "Duration (bars)" 1.
    CHECK(chart.closed[0].entry_bar == 25 && chart.closed[0].exit_bar == 26);
    CHECK(chart.closed[1].entry_bar == 39 && chart.closed[1].exit_bar == 40);
    CHECK(chart.open_entry_bar == 65);
}

// ── 3. the aggregated path books the same fills ───────────────────────────

// The divergence is the stamp alone: the same rows, the same prices, the same
// quantity, plain and magnified.
void test_aggregated_books_the_same_fills() {
    std::printf("test_aggregated_books_the_same_fills\n");
    const Run chart = run_batch(feed(kFord15), kRth, kNewYork, "15", "15", false,
                                ford_entry_bars());
    for (const bool magnifier : {false, true}) {
        const Run agg = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", magnifier,
                                  ford_entry_bars());
        CHECK(agg.error.empty());
        CHECK(agg.closed.size() == chart.closed.size());
        CHECK(agg.open_count == chart.open_count);
        if (agg.closed.size() != chart.closed.size()) {
            show(magnifier ? "aggregated 1 -> 15 mg" : "aggregated 1 -> 15", agg);
            continue;
        }
        for (std::size_t i = 0; i < agg.closed.size(); ++i) {
            CHECK(agg.closed[i].entry_price == chart.closed[i].entry_price);
            CHECK(agg.closed[i].exit_price == chart.closed[i].exit_price);
            CHECK(agg.closed[i].qty == chart.closed[i].qty);
        }
        CHECK(agg.open_entry_price == chart.open_entry_price);
    }
}

// ── 4. the stamp, on the aggregated path ──────────────────────────────────

// RESIDUAL. Every POOC fill of the aggregated run is dated one script bar on,
// at the bucket's close, plain and magnified. The fail-before of this lane's
// witness, with `== chart.closed[i].entry_ms` where the shift stands below:
//
//   FAIL  agg.closed[i].entry_ms == chart.closed[i].entry_ms
//   FAIL  agg.closed[i].exit_ms  == chart.closed[i].exit_ms
//   FAIL  agg.open_entry_ms      == chart.open_entry_ms
//     aggregated 1 -> 15       closed=2 open=1 err=''
//       entry 1751486400000 @11.77 bar 375   exit 1751550300000 @11.94 bar 390
//       entry 1751562000000 @11.79 bar 585   exit 1751895900000 @11.68 bar 600
//       open  1751918400000 @11.6 bar 975
//
// 1751486400000 is 2025-07-02 20:00 UTC, the close of the 19:45 bucket the
// chart path and the tape both date the fill at.
void test_aggregated_stamp() {
    std::printf("test_aggregated_stamp\n");
    const Run chart = run_batch(feed(kFord15), kRth, kNewYork, "15", "15", false,
                                ford_entry_bars());
    for (const bool magnifier : {false, true}) {
        const Run agg = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", magnifier,
                                  ford_entry_bars());
        if (agg.closed.size() != chart.closed.size()) continue;
        for (std::size_t i = 0; i < agg.closed.size(); ++i) {
            CHECK(agg.closed[i].entry_ms == chart.closed[i].entry_ms + kFifteen);
            CHECK(agg.closed[i].exit_ms == chart.closed[i].exit_ms + kFifteen);
        }
        CHECK(agg.open_entry_ms == chart.open_entry_ms + kFifteen);
        if (agg.closed[0].entry_ms != chart.closed[0].entry_ms + kFifteen)
            show(magnifier ? "aggregated 1 -> 15 mg" : "aggregated 1 -> 15", agg);
    }
    // Absolute, so the pin does not move with the control: the first fill is
    // dated 2025-07-02 20:00 UTC where TradingView dates it 19:45.
    const Run plain = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", false,
                                ford_entry_bars());
    if (plain.closed.empty()) return;
    CHECK(plain.closed[0].entry_ms == utc_ms(2025, 7, 2, 20, 0));
    CHECK(plain.closed[0].exit_ms == utc_ms(2025, 7, 3, 13, 45));
}

// ── 5. the same shift without a tape ──────────────────────────────────────

// Flat synthetic bars, 1m under a 5m script against 5m under a 5m script, so
// the shift is a property of the aggregation route and not of one feed.
void test_synthetic_five_minute() {
    std::printf("test_synthetic_five_minute\n");
    const std::int64_t entry_bar = kTue0930Et + 20 * kMinute;
    const auto one_minute = ladder(kTue0930Et, kTue0930Et + 34 * kMinute, kMinute);
    const auto five_minute = ladder(kTue0930Et, kTue0930Et + 30 * kMinute, kFive);
    const Run chart = run_batch(five_minute, kRth, kNewYork, "5", "5", false, {entry_bar});
    const Run agg = run_batch(one_minute, kRth, kNewYork, "1", "5", false, {entry_bar});
    CHECK(chart.error.empty());
    CHECK(agg.error.empty());
    CHECK(chart.closed.size() == 1);
    CHECK(agg.closed.size() == chart.closed.size());
    if (chart.closed.size() != 1 || agg.closed.size() != 1) {
        show("chart 5 -> 5", chart);
        show("aggregated 1 -> 5", agg);
        return;
    }
    CHECK(chart.closed[0].entry_ms == entry_bar);
    CHECK(chart.closed[0].exit_ms == entry_bar + kFive);
    CHECK(agg.closed[0].entry_price == chart.closed[0].entry_price);
    // RESIDUAL, as in row 4: one script bar late, here five minutes.
    CHECK(agg.closed[0].entry_ms == chart.closed[0].entry_ms + kFive);
    CHECK(agg.closed[0].exit_ms == chart.closed[0].exit_ms + kFive);
}

// ── 6. the trade row's bar index, on the aggregated path ──────────────────

void test_aggregated_bar_index() {
    std::printf("test_aggregated_bar_index\n");
    const Run plain = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", false,
                                ford_entry_bars());
    const Run magnified = run_batch(feed(kFord1m), kRth, kNewYork, "1", "15", true,
                                    ford_entry_bars());
    if (plain.closed.size() != 2 || magnified.closed.size() != 2) {
        show("aggregated 1 -> 15", plain);
        show("aggregated 1 -> 15 mg", magnified);
        return;
    }
    // The magnifier's re-stamp (PineStrategyHost::on_native_applied) gets the
    // chart bar index, the one the chart-timeframe path reports and the one the
    // tape's "Duration (bars)" 1 counts.
    CHECK(magnified.closed[0].entry_bar == 25 && magnified.closed[0].exit_bar == 26);
    CHECK(magnified.closed[1].entry_bar == 39 && magnified.closed[1].exit_bar == 40);
    CHECK(magnified.open_entry_bar == 65);
    // RESIDUAL (second finding): with no magnifier there is no re-stamp, so the
    // row carries the 1m INPUT index. 375 is the input bar the 19:45 bucket
    // opens on, 390 the one the 09:30 bucket after it opens on: 15 "bars"
    // between an entry and an exit one chart bar apart.
    CHECK(plain.closed[0].entry_bar == 375 && plain.closed[0].exit_bar == 390);
    CHECK(plain.closed[1].entry_bar == 585 && plain.closed[1].exit_bar == 600);
    CHECK(plain.open_entry_bar == 975);
}

}  // namespace

int main() {
    test_tv_tape_dates_the_open();
    test_chart_path_matches_the_tape();
    test_aggregated_books_the_same_fills();
    test_aggregated_stamp();
    test_synthetic_five_minute();
    test_aggregated_bar_index();

    std::printf("\npooc_fill_stamp_aggregation: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
