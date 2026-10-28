// Isolated trade half of the report-sink metrics test.
// Reuses L0's fixed rows, deterministic 20,000-row generator and verbatim
// trade_stats_reference.hpp. No sink, scratch file or module is required.
//
// RED on ee3083d4: metrics_stream.hpp declares TradeStatsAccumulator, but
// none of its methods is defined. This target must fail to link against
// that base for the missing feature. CORE-TOP records the lab diagnostic;
// no compilation or execution is permitted in this worktree on the Mac.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>

#include "trade_stats_reference.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                       \
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

bool same_trade_stats(const pf_trade_stats_t& got, const pf_trade_stats_t& want,
                      int n, pineforge::metrics::TradeFilter filter) {
    if (std::memcmp(&got, &want, sizeof(got)) == 0) return true;
    const auto* a = reinterpret_cast<const unsigned char*>(&got);
    const auto* b = reinterpret_cast<const unsigned char*>(&want);
    for (std::size_t i = 0; i < sizeof(got); ++i) {
        if (a[i] != b[i]) {
            std::printf("trade stats differ: n=%d filter=%d byte=%zu got=%02x want=%02x\n",
                        n, static_cast<int>(filter), i,
                        static_cast<unsigned>(a[i]), static_cast<unsigned>(b[i]));
            break;
        }
    }
    return false;
}

void check_trade_rows(const std::vector<pineforge::TradeC>& rows,
                      pineforge::metrics::TradeFilter filter, double capital) {
    const int n = static_cast<int>(rows.size());
    const auto check_result = [&](auto compute, const pf_trade_stats_t& expected, int count) {
        if (std::isfinite(capital)) {
            CHECK(same_trade_stats(compute(), expected, count, filter));
        } else {
            // A nonfinite capital is invalid even for an empty or filtered
            // prefix; finish must retain that refusal across reads and copies.
            bool refused = false;
            try {
                (void)compute();
            } catch (const std::exception& error) {
                const auto failure = pineforge::classify_run_failure(error);
                CHECK(failure.code == pineforge::RunFailureCode::report_result_invalid);
                CHECK(pineforge::run_failure_code_class(failure.code)
                      == pineforge::RunFailureClass::engine_fault);
                CHECK(!pineforge::run_failure_code_retryable(failure.code));
                const char* prefix = filter == pineforge::metrics::TradeFilter::ALL
                    ? "metrics.all" : filter == pineforge::metrics::TradeFilter::LONG
                    ? "metrics.longs" : "metrics.shorts";
                CHECK(failure.args && *failure.args == std::string("{\"field\":\"")
                      + prefix + ".net_profit_pct\",\"reason\":\"nonfinite\"}");
                refused = true;
            }
            CHECK(refused);
        }
    };
    const pf_trade_stats_t want = trade_stats_reference::compute_trade_stats(
        rows.data(), n, filter, capital);
    check_result([&] {
        return pineforge::metrics::compute_trade_stats(rows.data(), n, filter, capital);
    }, want, n);

    pineforge::metrics::TradeStatsAccumulator accumulator(filter, capital);
    for (const pineforge::TradeC& row : rows) accumulator.fold(row);
    check_result([&] { return accumulator.finish(); }, want, n);
    const auto& finished = accumulator;
    check_result([&] { return finished.finish(); }, want, n);

    // Model a sealed prefix, then continue each independent copy with the
    // unsealed and range-end rows. Finishing a prefix must not change it.
    std::vector<int> splits;
    if (n <= 10) {
        for (int split = 0; split <= n; ++split) splits.push_back(split);
    } else {
        splits = {0, 1, 2, n / 2, n - 1, n};
    }
    for (const int split : splits) {
        pineforge::metrics::TradeStatsAccumulator prefix(filter, capital);
        for (int i = 0; i < split; ++i) prefix.fold(rows[i]);
        const pf_trade_stats_t prefix_want = trade_stats_reference::compute_trade_stats(
            rows.data(), split, filter, capital);
        check_result([&] { return prefix.finish(); }, prefix_want, split);

        auto copied = prefix;
        // Assignment must replace filter/capital as well as every sum and
        // streak, even when the destination already folded different rows.
        const auto other_filter = filter == pineforge::metrics::TradeFilter::LONG
            ? pineforge::metrics::TradeFilter::SHORT
            : pineforge::metrics::TradeFilter::LONG;
        pineforge::metrics::TradeStatsAccumulator assigned(other_filter, -123.0);
        assigned.fold(trade(0, 7, -17.0, -5.0, true, 2.0));
        assigned.fold(trade(8, 9, 23.0, 4.0, false, 1.0));
        assigned = prefix;
        const auto& self = assigned;
        assigned = self;
        check_result([&] { return copied.finish(); }, prefix_want, split);
        check_result([&] { return assigned.finish(); }, prefix_want, split);

        for (int i = split; i < n; ++i) copied.fold(rows[i]);
        check_result([&] { return copied.finish(); }, want, n);
        check_result([&] { return prefix.finish(); }, prefix_want, split);
        check_result([&] { return assigned.finish(); }, prefix_want, split);
        for (int i = split; i < n; ++i) assigned.fold(rows[i]);
        check_result([&] { return assigned.finish(); }, want, n);
        check_result([&] { return prefix.finish(); }, prefix_want, split);
        for (int i = split; i < n; ++i) prefix.fold(rows[i]);
        check_result([&] { return prefix.finish(); }, want, n);
        check_result([&] { return prefix.finish(); }, want, n);
    }
}

void trade_stats_agree_with_the_reference() {
    // L0's fixed edge rows: empty, one win, one loss, one even, mixed.
    const std::vector<std::vector<pineforge::TradeC>> fixtures{
        {},
        {trade(0, 1, 10.0, 10.0, true, 0.5)},
        {trade(0, 1, -10.0, -10.0, false, 0.5)},
        {trade(0, 1, 0.0, 0.0, true, 0.0)},
        {trade(0, 1, 10.0, 10.0, true), trade(1, 2, -4.0, -4.0, false),
         trade(2, 3, 0.0, 0.0, true), trade(3, 9, 2.5, 2.5, false)},
        // Independent money/percent maxima, filtered streaks, even-trade
        // resets, inclusive same-bar duration and signed zero.
        {trade(0, 0, 100.0, 1.0, true, 0.25),
         trade(1, 2, -80.0, -2.0, false, 0.5),
         trade(3, 3, 2.0, 50.0, true, 0.125),
         trade(4, 7, -1.0, -90.0, false, 0.75),
         trade(8, 8, -0.0, -0.0, true),
         trade(9, 9, 3.0, 2.0, true),
         trade(10, 12, -4.0, -3.0, true),
         trade(13, 14, -2.0, -1.0, true)},
        // A different sum grouping loses the unit PnL in this sequence.
        {trade(0, 1, 0x1p53, 0x1p53, true, 0x1p53),
         trade(2, 3, -0x1p53, -0x1p53, true, 0.25),
         trade(4, 5, 1.0, 1.0, true, 0.25)}};
    for (const auto& rows : fixtures) {
        for (const auto filter : {pineforge::metrics::TradeFilter::ALL,
                                 pineforge::metrics::TradeFilter::LONG,
                                 pineforge::metrics::TradeFilter::SHORT}) {
            for (const double capital : {100000.0, 0.0, -0.0, -100.0,
                                         std::numeric_limits<double>::quiet_NaN(),
                                         std::numeric_limits<double>::infinity()}) {
                check_trade_rows(rows, filter, capital);
            }
        }
    }

    // L0's exact 20,000-row deterministic fixture and generator seed.
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
    for (const auto filter : {pineforge::metrics::TradeFilter::ALL,
                             pineforge::metrics::TradeFilter::LONG,
                             pineforge::metrics::TradeFilter::SHORT}) {
        check_trade_rows(rows, filter, 100000.0);
    }

    // Preserve the array entry point's old n <= 0 behavior without reading
    // a null pointer, including the otherwise unusual negative count.
    for (const auto filter : {pineforge::metrics::TradeFilter::ALL,
                             pineforge::metrics::TradeFilter::LONG,
                             pineforge::metrics::TradeFilter::SHORT}) {
        const auto want = trade_stats_reference::compute_trade_stats(nullptr, -1, filter, 0.0);
        const auto got = pineforge::metrics::compute_trade_stats(nullptr, -1, filter, 0.0);
        CHECK(same_trade_stats(got, want, -1, filter));
    }
}

}  // namespace

int main() {
    trade_stats_agree_with_the_reference();
    std::printf("trade-stats accumulator: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
