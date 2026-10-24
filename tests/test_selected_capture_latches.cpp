#include "global_allocation_replacement.hpp"
#include "../src/selected_capture_latches.hpp"
#include "../src/presentation_capture_window.hpp"
#include <pineforge/metrics_fold.hpp>
#include <pineforge/metrics_stream.hpp>
#include "equity_stats_reference.hpp"
#include "trade_stats_reference.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using pineforge::report::SelectedCaptureLatches;
constexpr std::int64_t kStart = 100;
constexpr std::int64_t kEnd = 200;
constexpr double kCapital = 1000.0;
int checks = 0;
int failures = 0;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

// Independent old-array oracle. Keep the original scan's index comparison
// and immediate account refusal, followed by contiguity and then count.
const char* array_error(const std::vector<pf_equity_point_t>& points,
                        std::uint64_t expected, bool trade, bool lot) {
    if (trade) return "native selected report: a trade was entered before the window start";
    if (lot) return "native selected report: a position was carried in before the window start";
    std::size_t first = 0;
    std::int64_t observed = 0;
    bool contiguous = true;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        if (point.time_ms < kStart) {
            if (point.equity != kCapital || point.open_profit != 0.0)
                return "native selected report: the account changed before the window start";
        } else if (point.time_ms < kEnd) {
            if (observed == 0) first = i;
            else if (i != first + static_cast<std::size_t>(observed)) contiguous = false;
            ++observed;
        }
    }
    if (!contiguous) return "native selected report: the window's equity observations are not one run";
    if (static_cast<std::uint64_t>(observed) != expected)
        return "native selected report: the window's equity observations differ from its script bars";
    return nullptr;
}

void compare(const std::vector<pf_equity_point_t>& points, std::uint64_t expected,
             bool trade = false, bool lot = false) {
    const char* reference = array_error(points, expected, trade, lot);
    SelectedCaptureLatches latch(kStart, kEnd, kCapital);
    // Reverse notification order to prove failure precedence is not arrival
    // order. Equality at T is legal; negative zero is an untouched P/L.
    latch.note_open_lot(lot ? kStart - 1 : kStart);
    latch.note_trade(trade ? kStart - 1 : kStart);
    for (const auto& point : points) latch.observe_final(point);
    bool refused = false;
    try {
        latch.validate(expected);
    } catch (const pineforge::coded<std::logic_error>& error) {
        refused = true;
        CHECK(reference != nullptr);
        if (reference) CHECK(std::strcmp(error.what(), reference) == 0);
        CHECK(error.run_failure_code() == pineforge::RunFailureCode::engine_invariant);
    }
    CHECK(refused == (reference != nullptr));
}

void precedence_and_final_values() {
    compare({}, 0); // M=0: the anchor is supplied separately, never observed.
    compare({{kStart, kCapital, -0.0}}, 1);
    for (unsigned faults = 0; faults < 32; ++faults) {
        const bool trade = (faults & 1U) != 0;
        const bool lot = (faults & 2U) != 0;
        const bool account = (faults & 4U) != 0;
        const bool gap = (faults & 8U) != 0;
        const bool count = (faults & 16U) != 0;
        std::vector<pf_equity_point_t> points{
            {kStart - 1, account ? kCapital + 1.0 : kCapital, -0.0},
            {kStart, 1100.0, 100.0},
            {gap ? kEnd : kStart + 1, 1050.0, -0.0},
            {kStart + 2, 1023.5, -0.0}};
        compare(points, count ? 8 : (gap ? 2 : 3), trade, lot);
    }
    // A recorder must hold the editable point. Judging the provisional point
    // would latch a false account failure that a final repair cannot undo.
    SelectedCaptureLatches prefix(kStart, kEnd, kCapital);
    pf_equity_point_t held{kStart - 1, kCapital + 1.0, 2.0};
    auto provisional = prefix;
    provisional.observe_final(held);
    bool refused = false;
    try { provisional.validate(0); }
    catch (const pineforge::coded<std::logic_error>&) { refused = true; }
    CHECK(refused);
    held.equity = kCapital;
    held.open_profit = -0.0;
    prefix.observe_final(held);
    prefix.validate(0);
    CHECK(prefix.observations() == 0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    compare({{kStart - 1, nan, 0.0}}, 0);
    compare({{kStart - 1, kCapital, nan}}, 0);
    compare({{kStart, nan, nan}}, 1); // window money is not a carry-in check
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void random_array_differential() {
    std::uint64_t random = 0x3a154b817e49d2c1ULL;
    for (int trial = 0; trial < 2048; ++trial) {
        const auto n = next_random(random) % 65;
        std::vector<pf_equity_point_t> points;
        std::uint64_t observed = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto time = static_cast<std::int64_t>(next_random(random) % 300);
            const double equity = time < kStart ? kCapital
                : static_cast<double>(next_random(random) % 3000) / 8.0;
            points.push_back({time, equity, -0.0});
            if (time >= kStart && time < kEnd) ++observed;
        }
        for (const auto expected : {observed, observed + 1})
            compare(points, expected);
    }
}

void latches_allocate_no_history() {
    for (const std::uint64_t n : {100000U, 400000U}) {
        SelectedCaptureLatches latch(kStart, kEnd, kCapital);
        const auto before = global_allocation::allocations;
        for (std::uint64_t i = 0; i < n; ++i)
            latch.observe_final({kStart, kCapital + static_cast<double>(i % 31), -0.0});
        latch.validate(n);
        CHECK(latch.observations() == n); // assert population before its bound
        CHECK(global_allocation::allocations == before);
        CHECK(latch.first_index() == 0);
    }
}

void window_matches_array_presentation() {
    std::uint64_t random = 0x1409825d93ab72c1ULL;
    for (int trial = 0; trial < 256; ++trial) {
        const auto m = static_cast<std::int64_t>(trial < 2 ? trial : next_random(random) % 129);
        pineforge::report::PresentationCaptureWindow window;
        std::vector<pf_equity_point_t> expected{{kStart, kCapital, 0.0}};
        std::vector<pf_equity_point_t> emitted;
        window.append_final(expected[0], [&](const auto& point) { emitted.push_back(point); });
        for (std::int64_t i = 0; i < m; ++i) {
            const pf_equity_point_t point{
                kStart + i, static_cast<double>(next_random(random) % 3000) / 8.0, -0.0};
            expected.push_back(point);
            window.append_final(point, [&](const auto& released) { emitted.push_back(released); });
        }
        CHECK(window.logical_count() == m + 1);
        const auto held = std::min<std::int64_t>(m + 1, 2);
        CHECK(window.size() == held);
        const auto first = m + 1 - held;
        CHECK(emitted.size() == static_cast<std::size_t>(first));
        CHECK(std::memcmp(window.data(), expected.data() + first,
                          static_cast<std::size_t>(held) * sizeof(pf_equity_point_t)) == 0);
        // Both the anchor (M=0/1) and the penultimate observation can be
        // edited. Hiding points never removes them from the metric stream.
        for (std::int64_t i = 0; i < held; ++i) {
            window[static_cast<std::size_t>(i)].equity -= 0.75;
            window[static_cast<std::size_t>(i)].open_profit = -0.0;
            expected[static_cast<std::size_t>(first + i)].equity -= 0.75;
            expected[static_cast<std::size_t>(first + i)].open_profit = -0.0;
        }
        for (std::int64_t visible = 0; visible <= held; ++visible)
            CHECK(window.published_count(visible) == m + 1 - (held - visible));
        window.emit_final_tail([&](const auto& point) { emitted.push_back(point); });
        CHECK(emitted.size() == expected.size());
        CHECK(std::memcmp(emitted.data(), expected.data(),
                          expected.size() * sizeof(pf_equity_point_t)) == 0);
    }
}

void window_releases_without_allocating() {
    for (const std::int64_t n : {100000, 400000}) {
        pineforge::report::PresentationCaptureWindow window;
        std::int64_t released = 0;
        const auto before = global_allocation::allocations;
        const auto visit = [&](const pf_equity_point_t& point) {
            CHECK(point.time_ms == released);
            ++released;
        };
        for (std::int64_t i = 0; i < n; ++i)
            window.append_final({i, kCapital, -0.0}, visit);
        window.emit_final_tail(visit);
        CHECK(window.logical_count() == n && released == n);
        CHECK(global_allocation::allocations == before);
        CHECK(window.size() == 2);
    }
}

void window_keeps_exposure_flags() {
    pineforge::report::PresentationCaptureWindow window;
    std::int64_t released = 0;
    const auto visit = [&](const pf_equity_point_t& point, bool observation, bool in_market) {
        CHECK(point.time_ms == released);
        CHECK(observation == (released != 0));
        CHECK(in_market == (released % 2 == 1));
        ++released;
    };
    for (std::int64_t i = 0; i < 7; ++i)
        window.append_metric_point({i, kCapital, 0.0}, i != 0, i % 2 == 1, visit);
    // Presentation changes money only; the original observation/exposure
    // flags remain alongside each held slot.
    window[0].equity += 7.0;
    window[1].open_profit = -0.0;
    window.emit_metric_tail(visit);
    CHECK(released == 7);
}

void metric_passes_consume_the_final_window() {
    using namespace pineforge::metrics;
    pineforge::TradeC rows[3]{};
    rows[0].pnl = 7.0;
    rows[0].pnl_pct = 0.7;
    rows[0].is_long = 1;
    rows[0].exit_bar_index = 1;
    rows[1] = rows[0];
    rows[1].pnl = -2.5;
    rows[1].pnl_pct = -0.25;
    rows[1].is_long = 0;
    rows[2] = rows[0];
    rows[2].pnl = 0.0;
    rows[2].pnl_pct = -0.0;
    TradeStatsAccumulator all(TradeFilter::ALL, kCapital);
    TradeStatsAccumulator longs(TradeFilter::LONG, kCapital);
    TradeStatsAccumulator shorts(TradeFilter::SHORT, kCapital);
    for (const auto& row : rows) {
        all.fold(row);
        longs.fold(row);
        shorts.fold(row);
    }
    std::uint64_t random = 0x1520492b53e74ca1ULL;
    constexpr std::int64_t start = 1704067200000LL;
    constexpr std::int64_t day = 86400000LL;
    for (int trial = 0; trial < 96; ++trial) {
        const auto m = static_cast<std::int64_t>(trial < 2 ? trial : next_random(random) % 129);
        const std::string zone = trial % 2 == 0 ? "UTC" : "America/New_York";
        std::vector<pf_equity_point_t> original{{start, kCapital, 0.0}};
        for (std::int64_t i = 0; i < m; ++i)
            original.push_back({start + i * 20 * day,
                static_cast<double>(next_random(random) % 20001) / 8.0 - 200.0, -0.0});
        auto expected = original; // test-only original owning-curve oracle
        const auto held = std::min<std::int64_t>(m + 1, 2);
        for (std::int64_t i = m + 1 - held; i <= m; ++i) {
            expected[static_cast<std::size_t>(i)].equity -= 0.75;
            expected[static_cast<std::size_t>(i)].open_profit = -0.0;
        }

        EquityMetricsFold first;
        pineforge::report::PresentationCaptureWindow window;
        const auto first_emit = [&](const pf_equity_point_t& point, bool observation, bool exposed) {
            first.fold(point, equity_month_key(point.time_ms, zone), observation, exposed);
        };
        std::int64_t exposed = 0;
        for (std::int64_t i = 0; i <= m; ++i) {
            const bool observation = i != 0;
            const bool in_market = observation && i % 2 == 1;
            if (in_market) ++exposed;
            window.append_metric_point(original[static_cast<std::size_t>(i)],
                                       observation, in_market, first_emit);
        }
        for (std::int64_t i = 0; i < window.size(); ++i) {
            window[static_cast<std::size_t>(i)].equity -= 0.75;
            window[static_cast<std::size_t>(i)].open_profit = -0.0;
        }
        window.emit_metric_tail(first_emit);
        const auto pass_a = first.first_pass();
        CHECK(pass_a.point_count == m + 1 && pass_a.observation_count == m);
        CHECK(pass_a.bars_in_market == exposed);
        const auto published = window.published_count(0);
        CHECK(published == m + 1 - held);

        EquityMetricsSecondPass second(pass_a);
        pineforge::report::PresentationCaptureWindow replay_window;
        const auto second_emit = [&](const pf_equity_point_t& point, bool observation, bool in_market) {
            second.fold(point, equity_month_key(point.time_ms, zone), observation, in_market);
        };
        for (std::int64_t i = 0; i <= m; ++i)
            replay_window.append_metric_point(original[static_cast<std::size_t>(i)],
                                              i != 0, i != 0 && i % 2 == 1, second_emit);
        for (std::int64_t i = 0; i < replay_window.size(); ++i) {
            replay_window[static_cast<std::size_t>(i)].equity -= 0.75;
            replay_window[static_cast<std::size_t>(i)].open_profit = -0.0;
        }
        replay_window.emit_metric_tail(second_emit);
        const auto ratios = second.finish();
        CHECK(ratios.matches(pass_a));
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const EquityMetricInputs inputs{
            kCapital, m > 0 ? 100.0 : nan, m > 0 ? 104.5 : nan, 4.5};
        const auto actual = finalize_metrics(
            pass_a, inputs, all.finish(), longs.finish(), shorts.finish(), &ratios);
        pf_metrics_t reference{};
        reference.all = trade_stats_reference::compute_trade_stats(rows, 3, TradeFilter::ALL, kCapital);
        reference.longs = trade_stats_reference::compute_trade_stats(rows, 3, TradeFilter::LONG, kCapital);
        reference.shorts = trade_stats_reference::compute_trade_stats(rows, 3, TradeFilter::SHORT, kCapital);
        reference.equity = equity_stats_reference::compute_equity_stats(
            expected.data(), m + 1, kCapital, zone, inputs.first_open, inputs.last_close, exposed, 4.5);
        reference.equity.time_in_market_pct = m > 0
            ? static_cast<double>(exposed) / static_cast<double>(m) * 100.0 : nan;
        CHECK(sizeof actual == 768);
        CHECK(std::memcmp(&actual, &reference, sizeof actual) == 0);
        auto mismatched = expected.back();
        mismatched.open_profit += 1.0;
        CHECK(second.replace_last(mismatched, equity_month_key(mismatched.time_ms, zone)));
        CHECK(!second.finish().matches(pass_a));
    }
}

} // namespace

int main() {
    try {
        precedence_and_final_values();
        random_array_differential();
        latches_allocate_no_history();
        window_matches_array_presentation();
        window_releases_without_allocating();
        window_keeps_exposure_flags();
        metric_passes_consume_the_final_window();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "selected capture latch exception: %s\n", error.what());
    }
    std::printf("test_selected_capture_latches: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
