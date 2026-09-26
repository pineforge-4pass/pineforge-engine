/*
 * test_magnified_aggregated_tape.cpp — R5 lane H-MEASURE (AUDIT4 X14, "F1's magnified residual").
 *
 * Lane F1 recorded two open items for a 1m feed aggregated to a 15m chart
 * under the bar magnifier: three entry-bar tests compared the chart bar with
 * the kernel's input slot, and lane F1's differential script booked 12 rows
 * where the pre-R4 engine ab9714be booked 9. TradingView settles the second,
 * and replays the first's inputs:
 *
 * - Lane F1's differential probe (its eight variants v0..v7) as Pine,
 *   tests/fixtures/magnified_aggregated/hm-{mag,chart}-diff-v<i> (`lab tv`,
 *   ws-report-v1, rangeProof covered), NYSE:F 15m over the three days of
 *   tests/fixtures/session_islastbar, use_bar_magnifier=true (hm-mag-*) and false
 *   (hm-chart-*). TradingView books 12 rows (v0, v2) and 14 (v4), as this engine
 *   does on every path: ab9714be's 9 was its own divergence.
 * - TradingView dates every fill at its chart bar's open, a magnified one too
 *   (218 of 218 fills on the sixteen tapes), and so does the engine on every path
 *   since R5 lane PAR-ORDERS: each row's instants are asserted exactly. (The
 *   adapter kept a magnified sub-bar fill's own instant, design AG2, and the row
 *   compared the bar each instant opens: 71 of the 152 instants the magnified
 *   replays book sat inside their bar.) Excursions are not asserted (the E19
 *   excursion model differs on every path, the chart timeframe's included).
 * - Each row's bar indices are the chart bars its instants open, and its "Duration
 *   (bars)" is exit bar - entry bar, on the magnified path as on the chart path:
 *   since K-IDX the kernel books lots and rows in script-bar space.
 *
 * Variants kept here are the ones the magnified run books exactly as TradingView:
 * v0 (plain), v1 (process_orders_on_close), v2 (calc_on_order_fills), v4
 * (slippage 2, pyramiding 3), v5 (process_orders_on_close, slippage 2,
 * pyramiding 3); controls: the chart and plain-aggregated paths against
 * hm-chart-diff-v0 / v1 / v2 / v4 / v5.
 *
 * v1 and v5 (R5 lane PAR-ORDERS): under process_orders_on_close a stop entry
 * whose stop the placing close already reached fills at that close, the
 * close's tick slipped like any stop fill -- five entries per tape, which the
 * engine used to fill a bar late at the next open, on every path. The
 * aggregated and magnified paths also needed the close pass itself: its
 * quiet-bar gate read the script-bar index where the pass reads the input
 * slot, and skipped it on every such run. The same stop entries under
 * calc_on_order_fills fill there too: v3 (all three paths) and v7 (chart and
 * aggregated) book them at TradingView's bar and price.
 *
 * Row 5 of v2, v3 and v7 (chart, aggregated; R5 lane PAR-ORDERS-2, H-MEASURE
 * Finding 6d): the calc_on_order_fills market entry the fill at bar 19's high
 * re-issues books that high (11.72 / 11.72 / 11.74), as TradingView does: the
 * fill is the matcher's, PX's limit at its own level on the leg to the bar's
 * second extreme, and a market order its recalculation places fills at that
 * extreme. It used to book the close (11.71 / 11.73) under
 * process_orders_on_close and the next open without it.
 *
 * The other rows below are RECORDED divergences, each asserted to differ, so a
 * fix flips them deliberately:
 *   - v7 rows 8 and 9 (chart, aggregated): after bar 33's close_all and
 *     same-bar add TradingView keeps the add (ML 11.86) and fills bar 38's
 *     stop entry at 11.83, the engine drops the add and books 11.82;
 *   - v3 row 2 (magnified): a fill recalculation's market entry books 11.58
 *     where TradingView books 11.54.
 * v7's magnified run is not replayed: TradingView books two more ML rows at
 * bar 59 there (16 against 14).
 *
 * The pyramiding cap (R5 lane PAR-ORDERS): no path of any variant ever holds
 * more lots than its pyramiding setting at a script call. Under the magnifier,
 * process_orders_on_close and calc_on_order_fills (v3, pyramiding=1) a resting
 * opposite limit entry used to lift the cap from every fill recalculation's
 * market add: 85 lots on one bar, where TradingView's tape (hm-mag-diff-v3)
 * books 12 rows and ends flat.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
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

#ifndef PINEFORGE_HM_MAG_FIXTURE_DIR
#error "PINEFORGE_HM_MAG_FIXTURE_DIR must name tests/fixtures/magnified_aggregated"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/session_islastbar/bars.inc"

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kBar = 15 * kMinute;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

// 2025-07-07 16:00 ET: no order is placed after the replayed days; the tape
// closes whatever is open on the next session's first bar ("after-window").
constexpr std::int64_t kWindowEnd = 1751918400000LL;

struct TapeTrade {
    bool is_long = true;
    std::int64_t entry_ms = 0, exit_ms = 0;
    double entry_price = 0.0, exit_price = 0.0, qty = 0.0, net_pnl = 0.0;
    std::string entry_signal, exit_signal;
    int duration_bars = -1;
};

std::vector<TapeTrade> read_tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_HM_MAG_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 17) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (trades.size() < n) trades.resize(n);
        TapeTrade& t = trades[n - 1];
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        t.is_long = cell[1].find("long") != std::string::npos;
        (entry ? t.entry_ms : t.exit_ms) = tape_ms(cell[2]);
        (entry ? t.entry_signal : t.exit_signal) = cell[3];
        (entry ? t.entry_price : t.exit_price) = std::stod(cell[4]);
        t.qty = std::stod(cell[5]);
        t.net_pnl = std::stod(cell[7]);
        t.duration_bars = std::stoi(cell[16]);
    }
    return trades;
}

struct Variant { const char* tag; bool pooc; bool coof; int slippage; int pyramiding; };

// The Pine port's body, as generated code reads it: units = strategy.position_size.
class MagDiffHost final : public source::PineStrategyHost {
public:
    explicit MagDiffHost(const Variant& v) {
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100;
        c.pyramiding = v.pyramiding;
        c.process_orders_on_close = v.pooc;
        c.calc_on_order_fills = v.coof;
        c.slippage = v.slippage;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.01;
        c.margin_long = 100;
        c.margin_short = 100;
        configure_pine_strategy(c);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar& b) override {
        const int k = bar_index_;  // the replay's first bar is the Pine counter's origin
        const double units = signed_position_size();
        max_lots = std::max(max_lots, physical_position().lot_count);
        if (k % 7 == 3 && units == 0.0) {
            strategy_entry("PL", true, kNaN, b.high - 0.02);
            strategy_exit("PX", "PL", b.close + 0.06, b.close - 0.06);
        }
        if (k % 9 == 4 && units == 0.0) {
            strategy_entry("PS", false, b.close + 0.02);
            strategy_exit("SX", "PS", b.close - 0.05, b.close + 0.07);
        }
        if (k % 13 == 6 && units == 0.0) {
            strategy_entry("ML", true);
            strategy_exit("MX", "ML", b.close + 0.08, b.close - 0.08);
        }
        if (k % 13 == 7 && units > 0.0) strategy_entry("ML", true);
        if (k % 17 == 16 && units != 0.0) strategy_close_all();
    }

    struct Row {
        bool is_long = true;
        std::int64_t entry_ms = 0, exit_ms = 0;
        double entry_price = 0.0, exit_price = 0.0, qty = 0.0, profit = 0.0;
        int entry_bar = 0, exit_bar = 0;
        std::string entry_id, exit_id;
    };
    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (int i = 0; i < trade_count(); ++i) {
            Row r;
            r.is_long = trades_[static_cast<std::size_t>(i)].is_long;
            r.entry_ms = closed_trade_entry_time(i);
            r.exit_ms = closed_trade_exit_time(i);
            r.entry_price = closed_trade_entry_price(i);
            r.exit_price = closed_trade_exit_price(i);
            r.qty = closed_trade_size(i);
            r.profit = closed_trade_profit(i);
            r.entry_bar = closed_trade_entry_bar_index(i);
            r.exit_bar = closed_trade_exit_bar_index(i);
            r.entry_id = closed_trade_entry_id(i);
            r.exit_id = closed_trade_exit_id(i);
            out.push_back(r);
        }
        return out;
    }
    double open_units() const { return physical_position().signed_units; }
    std::size_t max_lots = 0;  // the most lots open at any script call
};

enum class Path { Chart, Aggregated, Magnified };

const char* path_name(Path path) {
    switch (path) {
        case Path::Chart: return "chart 15 -> 15";
        case Path::Aggregated: return "aggregated 1 -> 15";
        case Path::Magnified: return "aggregated 1 -> 15, magnifier";
    }
    return "?";
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

// The chart bar (fixture order) whose open is `ms`; -1 when none is.
int chart_bar_of(std::int64_t ms) {
    for (std::size_t i = 0; i < sizeof(kFord15) / sizeof(kFord15[0]); ++i)
        if (kFord15[i].ts == ms) return static_cast<int>(i);
    return -1;
}


// TradingView's exit signal for strategy.close_all is "Close position order".
std::string exit_signal(const MagDiffHost::Row& r) {
    return r.exit_id.rfind("__", 0) == 0 ? "Close position order" : r.exit_id;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void run_path(MagDiffHost& host, Path path) {
    static const std::vector<Bar> fifteen = feed(kFord15);
    static const std::vector<Bar> one = feed(kFord1m);
    if (path == Path::Chart) {
        host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    } else {
        host.run(one.data(), static_cast<int>(one.size()), "1", "15", path == Path::Magnified);
    }
}

void replay(const char* slug, const Variant& v, Path path, unsigned recorded = 0u) {
    std::printf("-- %s  [%s, %s]\n", slug, v.tag, path_name(path));
    std::vector<TapeTrade> tape;
    for (const TapeTrade& t : read_tape(slug))
        if (t.exit_ms < kWindowEnd) tape.push_back(t);
    const bool after_window = read_tape(slug).size() != tape.size();
    MagDiffHost host(v);
    run_path(host, path);
    CHECK(host.last_error().empty());
    const auto rows = host.rows();
    CHECK(!tape.empty());
    CHECK(rows.size() == tape.size());
    // The tape closes nothing after the window, and the replay ends flat.
    CHECK(!after_window);
    CHECK(host.open_units() == 0.0);
    const std::size_t n = std::min(rows.size(), tape.size());
    for (std::size_t i = 0; i < n; ++i) {
        const TapeTrade& tv = tape[i];
        const MagDiffHost::Row& r = rows[i];
        const bool ok = r.is_long == tv.is_long
            && r.entry_ms == tv.entry_ms && r.exit_ms == tv.exit_ms
            && near(r.entry_price, tv.entry_price, 1e-9)
            && near(r.exit_price, tv.exit_price, 1e-9)
            && r.qty == tv.qty && near(r.profit, tv.net_pnl, 0.00006)
            && r.entry_id == tv.entry_signal && exit_signal(r) == tv.exit_signal
            && r.entry_bar == chart_bar_of(tv.entry_ms)
            && r.exit_bar == chart_bar_of(tv.exit_ms)
            && r.exit_bar - r.entry_bar == tv.duration_bars;
        const bool divergence = (recorded & (1u << (i + 1))) != 0;
        CHECK(ok != divergence);
        if (divergence) std::printf("      row %zu: RECORDED divergence\n", i + 1);
        if (!ok) {
            std::printf("      row %zu: TradingView %s %s bar %d @%.2f -> bar %d @%.2f %s dur %d pnl %.4f\n"
                        "              engine      %s %s bar %d @%.2f -> bar %d @%.2f %s dur %d pnl %.4f"
                        " (instants %lld -> %lld)\n",
                        i + 1, tv.entry_signal.c_str(), tv.is_long ? "L" : "S",
                        chart_bar_of(tv.entry_ms), tv.entry_price, chart_bar_of(tv.exit_ms),
                        tv.exit_price, tv.exit_signal.c_str(), tv.duration_bars, tv.net_pnl,
                        r.entry_id.c_str(), r.is_long ? "L" : "S", r.entry_bar, r.entry_price,
                        r.exit_bar, r.exit_price, exit_signal(r).c_str(), r.exit_bar - r.entry_bar,
                        r.profit, static_cast<long long>(r.entry_ms),
                        static_cast<long long>(r.exit_ms));
        }
    }
}

// The cap on every path of every variant, and the v3 magnified run against its
// tape: TradingView's 12 rows, the book flat at the end.
void pyramiding_cap_holds(const Variant (&variants)[8]) {
    for (const Variant& v : variants) {
        for (const Path path : {Path::Chart, Path::Aggregated, Path::Magnified}) {
            MagDiffHost host(v);
            run_path(host, path);
            CHECK(host.last_error().empty());
            const bool within = host.max_lots <= static_cast<std::size_t>(v.pyramiding);
            CHECK(within);
            if (!within)
                std::printf("      %s, %s: %zu lots open at once, pyramiding %d\n", v.tag,
                            path_name(path), host.max_lots, v.pyramiding);
        }
    }
    MagDiffHost host(variants[3]);
    run_path(host, Path::Magnified);
    std::vector<TapeTrade> tape;
    for (const TapeTrade& t : read_tape("hm-mag-diff-v3"))
        if (t.exit_ms < kWindowEnd) tape.push_back(t);
    CHECK(tape.size() == 12);
    CHECK(host.rows().size() == tape.size());
    CHECK(host.open_units() == 0.0);
    std::printf("-- the pyramiding cap: v3 magnified books %zu rows (TradingView %zu), "
                "%zu lots at most, %g units open at the end\n", host.rows().size(),
                tape.size(), host.max_lots, host.open_units());
}

}  // namespace

int main() {
    const Variant v0{"v0 plain", false, false, 0, 1};
    const Variant v1{"v1 process_orders_on_close", true, false, 0, 1};
    const Variant v2{"v2 calc_on_order_fills", false, true, 0, 1};
    const Variant v4{"v4 slippage 2, pyramiding 3", false, false, 2, 3};
    const Variant v3{"v3 process_orders_on_close, calc_on_order_fills", true, true, 0, 1};
    const Variant v5{"v5 process_orders_on_close, slippage 2, pyramiding 3", true, false, 2, 3};
    const Variant v7{"v7 process_orders_on_close, calc_on_order_fills, slippage 2, pyramiding 3",
                     true, true, 2, 3};
    const Variant all[8] = {
        v0, v1, v2, v3, v4, v5,
        {"v6 calc_on_order_fills, slippage 2, pyramiding 3", false, true, 2, 3}, v7};
    const auto rows = [](std::initializer_list<int> list) {
        unsigned mask = 0;
        for (const int row : list) mask |= 1u << row;
        return mask;
    };
    // 1. the magnified run against TradingView's magnifier tapes
    replay("hm-mag-diff-v0", v0, Path::Magnified);
    replay("hm-mag-diff-v1", v1, Path::Magnified);
    replay("hm-mag-diff-v2", v2, Path::Magnified);
    replay("hm-mag-diff-v4", v4, Path::Magnified);
    replay("hm-mag-diff-v5", v5, Path::Magnified);
    replay("hm-mag-diff-v3", v3, Path::Magnified, rows({2}));
    // 2. controls: the chart and plain-aggregated paths against the magnifier-off tapes
    for (const Path path : {Path::Chart, Path::Aggregated}) {
        replay("hm-chart-diff-v0", v0, path);
        replay("hm-chart-diff-v1", v1, path);
        replay("hm-chart-diff-v4", v4, path);
        replay("hm-chart-diff-v5", v5, path);
        replay("hm-chart-diff-v2", v2, path);
        replay("hm-chart-diff-v3", v3, path);
        replay("hm-chart-diff-v7", v7, path, rows({8, 9}));
    }
    // 3. the pyramiding cap
    pyramiding_cap_holds(all);
    std::printf("\n%s magnified aggregated tape: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
