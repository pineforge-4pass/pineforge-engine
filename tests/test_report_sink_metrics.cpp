// CR-150 lane L0, acceptance (d), unit half (DESIGN.md section 7, 3.3).
//
// RED ON 3fc010a8b: this file does not compile. It includes
// <pineforge/report_sink.h> and ../src/report_scratch.hpp, neither of which
// exists before lanes L6b-1 and L6b-4/L7c. The first diagnostic is the
// recorded fail-before.
//
// What it asserts once the lanes land:
//   1. Trade stats: metrics::compute_trade_stats over an array, and the
//      metrics::TradeStatsAccumulator fold()/finish() path, are byte-equal to
//      tests/trade_stats_reference.hpp (the verbatim 3fc010a8b loop) on fixed
//      edge rows and on 20,000 random rows, for ALL / LONG / SHORT.
//   2. Equity stats: the array source, metrics::SpillCurve over a scratch file,
//      and tests/equity_stats_reference.hpp agree for every one of the fifteen
//      doubles on 200 random curves (empty, one point, two points, monthly
//      spans, zero/negative equity).
//   3. Fixture strategy: a real generated module run with the sink off and on
//      reports the same 768-byte pf_metrics_t, the same net_profit and the
//      same counts (the sink keeps rows in the sink, not in the report).
//
// The module is the tutorial MACD generated.cpp linked into this executable
// (tests/CMakeLists.txt, the release-shaped MODULE recipe); no fake library and
// no Python.

#include <pineforge/pineforge.h>
#include <pineforge/metrics.hpp>
#include <pineforge/report_sink.h>

#include "../src/report_scratch.hpp"

#include "equity_stats_reference.hpp"
#include "trade_stats_reference.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++checks;                                                                 \
        if (!(condition)) {                                                       \
            ++failures;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);       \
        }                                                                         \
    } while (0)

std::uint64_t g_rng = 0x243f6a8885a308d3ull;

std::uint64_t next_random() {
    g_rng += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = g_rng;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double uniform(double lo, double hi) {
    return lo + (hi - lo) * static_cast<double>(next_random() >> 11) /
                    static_cast<double>(std::uint64_t{1} << 53);
}

// ── 1. Trade stats ──────────────────────────────────────────────────────

pineforge::TradeC trade(std::int64_t entry, std::int64_t exit, double pnl,
                        double pnl_pct, bool is_long, double commission = 0.0) {
    pineforge::TradeC row{};
    row.entry_time = entry;
    row.exit_time = exit;
    row.entry_price = 100.0;
    row.exit_price = 100.0 + pnl;
    row.pnl = pnl;
    row.pnl_pct = pnl_pct;
    row.is_long = is_long ? 1 : 0;
    row.max_runup = pnl > 0.0 ? pnl : 0.0;
    row.max_drawdown = pnl < 0.0 ? -pnl : 0.0;
    row.qty = 1.0;
    row.commission = commission;
    row.entry_bar_index = static_cast<std::int32_t>(entry);
    row.exit_bar_index = static_cast<std::int32_t>(exit);
    row.open_at_end = 0;
    return row;
}

void print_trade_fields(const pf_trade_stats_t& got, const pf_trade_stats_t& want) {
    const double* a = reinterpret_cast<const double*>(&got);
    const double* b = reinterpret_cast<const double*>(&want);
    for (std::size_t i = 0; i < sizeof(pf_trade_stats_t) / sizeof(double); ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(double)) != 0) {
            std::uint64_t a_bits = 0, b_bits = 0;
            std::memcpy(&a_bits, &a[i], sizeof a_bits);
            std::memcpy(&b_bits, &b[i], sizeof b_bits);
            std::printf("  field %zu: got %.17g (0x%016llx) want %.17g (0x%016llx)\n", i,
                        a[i], static_cast<unsigned long long>(a_bits), b[i],
                        static_cast<unsigned long long>(b_bits));
            return;
        }
    }
}

bool same_trade_stats(const pf_trade_stats_t& got, const pf_trade_stats_t& want,
                      int n, pineforge::metrics::TradeFilter filter) {
    const bool equal = std::memcmp(&got, &want, sizeof(got)) == 0;
    if (!equal) {
        std::printf("trade stats differ: n=%d filter=%d\n", n, static_cast<int>(filter));
        print_trade_fields(got, want);
    }
    return equal;
}

void check_trade_rows(const std::vector<pineforge::TradeC>& rows,
                      pineforge::metrics::TradeFilter filter) {
    const pf_trade_stats_t want = trade_stats_reference::compute_trade_stats(
        rows.data(), static_cast<int>(rows.size()), filter, 100000.0);
    const pf_trade_stats_t got = pineforge::metrics::compute_trade_stats(
        rows.data(), static_cast<int>(rows.size()), filter, 100000.0);
    CHECK(same_trade_stats(got, want, static_cast<int>(rows.size()), filter));

    pineforge::metrics::TradeStatsAccumulator accumulator(filter, 100000.0);
    for (const pineforge::TradeC& row : rows) accumulator.fold(row);
    const pf_trade_stats_t folded = accumulator.finish();
    CHECK(same_trade_stats(folded, want, rows.data(), static_cast<int>(rows.size()), filter));
}

void trade_stats_agree_with_the_reference() {
    // Fixed edge rows: empty, one win, one loss, one even, mixed.
    for (const std::vector<pineforge::TradeC>& rows :
         std::vector<std::vector<pineforge::TradeC>>{
             {},
             {trade(0, 1, 10.0, 10.0, true, 0.5)},
             {trade(0, 1, -10.0, -10.0, false, 0.5)},
             {trade(0, 1, 0.0, 0.0, true, 0.0)},
             {trade(0, 1, 10.0, 10.0, true), trade(1, 2, -4.0, -4.0, false),
              trade(2, 3, 0.0, 0.0, true), trade(3, 9, 2.5, 2.5, false)}}) {
        for (const pineforge::metrics::TradeFilter filter :
             {pineforge::metrics::TradeFilter::ALL, pineforge::metrics::TradeFilter::LONG,
              pineforge::metrics::TradeFilter::SHORT}) {
            check_trade_rows(rows, filter);
        }
    }

    // 20,000 random rows: wins, losses, evens, long and short, with
    // commissions; bar indices increase so the inclusive-bars rule is exercised.
    std::vector<pineforge::TradeC> rows;
    rows.reserve(20000);
    std::int64_t bar = 0;
    for (int i = 0; i < 20000; ++i) {
        const int side = static_cast<int>(next_random() & 1u);
        const int kind = static_cast<int>(next_random() % 20u);
        double pnl = 0.0, pct = 0.0;
        if (kind == 0) {
            pnl = 0.0;
            pct = 0.0;
        } else if (kind < 13) {
            pnl = uniform(0.01, 500.0);
            pct = uniform(0.01, 20.0);
        } else {
            pnl = uniform(-500.0, -0.01);
            pct = uniform(-20.0, -0.01);
        }
        const std::int64_t hold = 1 + static_cast<std::int64_t>(next_random() % 50u);
        rows.push_back(trade(bar, bar + hold, pnl, pct, side != 0,
                             uniform(0.0, 2.0)));
        bar += hold + 1;
    }
    for (const pineforge::metrics::TradeFilter filter :
         {pineforge::metrics::TradeFilter::ALL, pineforge::metrics::TradeFilter::LONG,
          pineforge::metrics::TradeFilter::SHORT}) {
        check_trade_rows(rows, filter);
    }
}

// ── 2. Equity stats: array vs SpillCurve vs the reference ───────────────

std::string temp_path() {
    char name[] = "/tmp/pf_spill_XXXXXX";
    const int fd = mkstemp(name);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    return name;
}

void check_curve(const std::vector<pf_equity_point_t>& curve, const char* label,
                 const std::string& chart_tz) {
    const std::int64_t n = static_cast<std::int64_t>(curve.size());
    const double first_open = 100.0;
    const double last_close = 101.0;
    const std::int64_t bars_in_market = n / 2;
    const double net_profit = n > 0 ? curve[n - 1].equity - 10000.0 : 0.0;

    const pf_equity_stats_t array_stats = pineforge::metrics::compute_equity_stats(
        curve.empty() ? nullptr : curve.data(), n, 10000.0, chart_tz, first_open,
        last_close, bars_in_market, net_profit);
    const pf_equity_stats_t want = equity_stats_reference::compute_equity_stats(
        curve.empty() ? nullptr : curve.data(), n, 10000.0, chart_tz, first_open,
        last_close, bars_in_market, net_profit);
    const char* difference = equity_stats_reference::first_difference(array_stats, want);
    if (difference != nullptr) {
        std::printf("equity stats differ from the reference: curve=%s field=%s\n", label,
                    difference);
        CHECK(difference == nullptr);
    }

    const std::string path = temp_path();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        CHECK(out.good());
        if (!curve.empty()) {
            out.write(reinterpret_cast<const char*>(curve.data()),
                      static_cast<std::streamsize>(curve.size() * sizeof(curve[0])));
        }
    }
    pineforge::metrics::SpillCurve spill(path, n);
    const pf_equity_stats_t spilled = pineforge::metrics::compute_equity_stats(
        spill, n, 10000.0, chart_tz, first_open, last_close, bars_in_market, net_profit);
    difference = equity_stats_reference::first_difference(spilled, want);
    if (difference != nullptr) {
        std::printf("SpillCurve differs from the reference: curve=%s field=%s\n", label,
                    difference);
        CHECK(difference == nullptr);
    }
    std::remove(path.c_str());
}

void equity_stats_agree_with_the_reference() {
    // Empty, one point, two points.
    check_curve({}, "empty", "UTC");
    check_curve({{1700000000000LL, 10000.0, 0.0}}, "one", "UTC");
    check_curve({{1700000000000LL, 10000.0, 0.0}, {1700000060000LL, 10050.0, 5.0}}, "two",
                "UTC");

    // 200 random curves over spans from hours to years, with zero and
    // negative equity points, monthly costs and non-UTC charts.
    for (int shape = 0; shape < 200; ++shape) {
        const std::int64_t n = 3 + static_cast<std::int64_t>(next_random() % 2000u);
        const std::int64_t step_ms =
            (shape % 5 == 0) ? 86400000LL      // daily
            : (shape % 5 == 1) ? 3600000LL     // hourly
            : (shape % 5 == 2) ? 604800000LL   // weekly
            : 60000LL;                         // minute
        std::vector<pf_equity_point_t> curve;
        curve.reserve(static_cast<std::size_t>(n));
        double equity = 10000.0;
        for (std::int64_t i = 0; i < n; ++i) {
            const double shock = uniform(-0.03, 0.03);
            equity *= (1.0 + shock);
            if (next_random() % 101u == 0) equity = 0.0;
            if (next_random() % 251u == 0) equity = -1.0;
            curve.push_back({1700000000000LL + i * step_ms, equity, uniform(-5.0, 5.0)});
        }
        const char* tz = (shape % 2 == 0) ? "UTC" : "America/New_York";
        check_curve(curve, "random", tz);
    }
}

// ── 3. Fixture strategy: sink off vs sink on ────────────────────────────

extern "C" {
void* strategy_create(const char* params_json);
void run_backtest_full(void* strategy, pf_bar_t* bars, int n, const char* input_tf,
                       const char* script_tf, int bar_magnifier, int magnifier_samples,
                       int magnifier_dist, pf_report_t* out);
void strategy_free(void* strategy);
void report_free(pf_report_t* report);
int strategy_set_report_sink_v1(pf_strategy_t strategy, const pf_report_sink_v1* sink);
}

std::vector<pf_bar_t> oscillating_bars(int n) {
    std::vector<pf_bar_t> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double price = 100.0 + ((i / 3) % 2 == 0 ? 1.0 : -1.0);
        bars.push_back({price, price + 0.5, price - 0.5, price, 1000.0,
                        1700000000000LL + static_cast<std::int64_t>(i) * 60000LL});
    }
    return bars;
}

struct RunResult {
    pf_metrics_t metrics{};
    double net_profit = 0.0;
    int total_trades = 0;
    bool has_trades = false;
    bool has_curve = false;
};

RunResult run_fixture(const std::vector<pf_bar_t>& bars, const char* sink_path) {
    RunResult result;
    void* strategy = strategy_create(nullptr);
    CHECK(strategy != nullptr);
    if (sink_path != nullptr) {
        pf_report_sink_v1 sink{};
        sink.struct_size = sizeof(sink);
        sink.version = 1;
        sink.path = sink_path;
        sink.run_id = "l0-metrics";
        sink.scratch_dir = nullptr;
        const int accepted = strategy_set_report_sink_v1(strategy, &sink);
        CHECK(accepted == 0);
    }
    pf_report_t report{};
    run_backtest_full(strategy,
                      const_cast<pf_bar_t*>(bars.data()), static_cast<int>(bars.size()),
                      "1", "1", 0, 4, 0, &report);
    result.metrics = report.metrics;
    result.net_profit = report.net_profit;
    result.total_trades = report.total_trades;
    result.has_trades = report.trades != nullptr && report.trades_len > 0;
    result.has_curve = report.equity_curve != nullptr && report.equity_curve_len > 0;
    report_free(&report);
    strategy_free(strategy);
    return result;
}

void fixture_strategy_is_byte_identical_with_and_without_the_sink() {
    static_assert(sizeof(pf_metrics_t) == 768, "pf_metrics_t is 768 bytes");
    const std::vector<pf_bar_t> bars = oscillating_bars(4000);
    const std::string path = temp_path();
    const RunResult without = run_fixture(bars, nullptr);
    const RunResult with = run_fixture(bars, path.c_str());
    std::remove(path.c_str());

    CHECK(without.total_trades > 0);
    CHECK(with.total_trades == without.total_trades);
    CHECK(std::memcmp(&with.metrics, &without.metrics, sizeof(pf_metrics_t)) == 0);
    CHECK(std::memcmp(&with.net_profit, &without.net_profit, sizeof(double)) == 0);
    // DESIGN.md 6: with a sink the report carries the summary only, while the
    // sink-off run still carries its arrays.
    CHECK(!with.has_trades);
    CHECK(!with.has_curve);
    CHECK(without.has_trades);
    CHECK(without.has_curve);
}

}  // namespace

int main() {
    trade_stats_agree_with_the_reference();
    equity_stats_agree_with_the_reference();
    fixture_strategy_is_byte_identical_with_and_without_the_sink();
    std::printf("report-sink metrics: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
