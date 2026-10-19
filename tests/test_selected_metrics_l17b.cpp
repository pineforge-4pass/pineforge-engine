// CR-150 L17B: selected metric reads over P00 sources, and real selected
// sink-on/off fixtures. RED on b3c39238e: the selected source overloads are
// undeclared; with declarations only, they and SpillCurve fail at link.
// No fixture pin is harvested. All metric bytes come from the existing array
// report path or the frozen equity reference with its selected denominator.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>
#include <pineforge/report_sink_writer.hpp>

#ifdef PINEFORGE_L17B_SOURCE
#include <pineforge/source/pine_strategy_host.hpp>
#endif

#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"
#include "../src/report_scratch.hpp"
#include "../src/selected_report_view.hpp"
#include "equity_stats_reference.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_selected_window_counts_v1(const BacktestEngine*, pf_selected_window_counts_v1*) noexcept;
}
}

#ifdef PINEFORGE_L17B_SOURCE
namespace pineforge::source {
// Reuse the existing presentation-test seam: the qualified call executes the
// real Pine hook, without redispatching to the fixture's counting override.
struct ReportSinkEquityTestAccess {
    static void present(const PineStrategyHost& host, ReportC* report) {
        host.PineStrategyHost::present_report(report);
    }
};
}
#endif

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr std::int64_t kT = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kDay = 86400000;
constexpr double kCapital = 1000.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool same_metrics(const pf_metrics_t& a, const pf_metrics_t& b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// Bit-identical points: equal length and equal bytes, and never an empty pair.
bool same_points(const std::vector<pf_equity_point_t>& a,
                 const std::vector<pf_equity_point_t>& b) {
    return !a.empty() && a.size() == b.size()
        && std::memcmp(a.data(), b.data(), a.size() * sizeof(pf_equity_point_t)) == 0;
}

// A file with a nonzero, deliberately unaligned region offset and unrelated
// bytes after it. Only tests retain the reference array; production reads the
// engine-owned region through its one factory and SpillCurve's bounded cache.
struct Scratch {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file{std::tmpfile(), &std::fclose};
    metrics::ScratchCurveRegion region{};

    Scratch(const pf_equity_point_t* points, std::int64_t count) {
        if (!file) throw std::runtime_error("L17B tmpfile failed");
        constexpr char prefix[] = "unrelated-prefix";
        constexpr char suffix[] = "unrelated-suffix";
        if (std::fwrite(prefix, 1, sizeof prefix, file.get()) != sizeof prefix
            || std::fwrite(points, sizeof(*points), static_cast<std::size_t>(count), file.get())
                != static_cast<std::size_t>(count)
            || std::fwrite(suffix, 1, sizeof suffix, file.get()) != sizeof suffix
            || std::fflush(file.get()) != 0)
            throw std::runtime_error("L17B scratch write failed");
        region = {fileno(file.get()), static_cast<std::int64_t>(sizeof prefix), count};
        if (region.descriptor < 0) throw std::runtime_error("L17B fileno failed");
    }

    void replace_tail(const pf_equity_point_t& point) {
        const auto offset = region.byte_offset
            + (region.point_count - 1) * static_cast<std::int64_t>(sizeof point);
        const char* bytes = reinterpret_cast<const char*>(&point);
        std::size_t done = 0;
        while (done < sizeof point) {
            const ssize_t written = pwrite(region.descriptor, bytes + done, sizeof point - done,
                                           static_cast<off_t>(offset + static_cast<std::int64_t>(done)));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("L17B scratch tail write failed");
            done += static_cast<std::size_t>(written);
        }
    }
};

void compare_sources(const std::vector<pf_equity_point_t>& observations,
                     const std::string& zone = "UTC", double capital = kCapital,
                     double first_open = 100.0, double last_close = 104.5) {
    const auto m = static_cast<std::int64_t>(observations.size());
    const auto selected = report::build_selected_equity_curve(
        kT, capital, observations.empty() ? nullptr : observations.data(), m);
    const std::int64_t in_market = m / 2;
    const double pnl = 4.5;
    TradeC trades[3]{};
    trades[0].entry_time = kT;
    trades[0].exit_time = kT + kDay;
    trades[0].entry_bar_index = 0;
    trades[0].exit_bar_index = 3;
    trades[0].pnl = 7.0;
    trades[0].pnl_pct = 7.0;
    trades[0].is_long = 1;
    trades[1] = trades[0];
    trades[1].pnl = -2.5;
    trades[1].pnl_pct = -2.5;
    trades[1].is_long = 0;
    trades[2] = trades[0];
    trades[2].pnl = 0.0;
    trades[2].pnl_pct = -0.0;
    trades[2].open_at_end = 1;

    pf_metrics_t expected{};
    internal::compute_report_metrics(
        expected, trades, 3, selected.points.get(), selected.count,
        pnl, capital, zone, first_open, last_close, in_market, m);
    // Independent frozen equity formulae: the anchor changes only exposure's
    // denominator. Keep the exact expression and quiet-NaN convention.
    auto frozen = equity_stats_reference::compute_equity_stats(
        selected.points.get(), selected.count, capital, zone,
        first_open, last_close, in_market, pnl);
    frozen.time_in_market_pct = m > 0
        ? static_cast<double>(in_market) / static_cast<double>(m) * 100.0 : kNaN;
    CHECK(std::memcmp(&expected.equity, &frozen, sizeof frozen) == 0);

    const metrics::ArrayCurve array(selected.points.get(), selected.count);
    Scratch scratch(selected.points.get(), selected.count);
    const metrics::SpillCurve spill(scratch.region);
    CHECK(spill.size() == selected.count);
    // First/last and reverse cache misses prove that a source is re-readable.
    for (const auto i : {selected.count - 1, std::int64_t{0}, selected.count / 2}) {
        const auto point = spill[i];
        CHECK(std::memcmp(&point, &selected.points[i], sizeof point) == 0);
    }
    pf_metrics_t from_array{}, from_scratch{}, repeated{};
    internal::compute_selected_report_metrics(
        from_array, trades, 3, array, selected.count, pnl, capital, zone,
        first_open, last_close, in_market, m);
    internal::compute_selected_report_metrics(
        from_scratch, trades, 3, spill, selected.count, pnl, capital, zone,
        first_open, last_close, in_market, m);
    internal::compute_selected_report_metrics(
        repeated, trades, 3, spill, selected.count, pnl, capital, zone,
        first_open, last_close, in_market, m);
    CHECK(same_metrics(from_array, expected));
    CHECK(same_metrics(from_scratch, expected));
    CHECK(same_metrics(repeated, expected));
}

void selected_array_fixtures() {
    compare_sources({}, "UTC", kCapital, kNaN, kNaN); // F13: M=0, anchor only
    compare_sources({{kT, 1100.0, 100.0}}); // observation at the anchor time
    compare_sources({{kT + 182 * kDay, 1100.0, 0.0},
                     {kT + 365 * kDay, 1045.0, -5.5}});
    compare_sources({{kT + kDay, 0.0, -0.0}, {kT + 2 * kDay, -100.0, -2.0},
                     {kT + 3 * kDay, 50.0, 1.5}});
    compare_sources({{kT, 1100.0, 0.0}, {kT, 1045.0, -0.0}});
    compare_sources({{kT + kDay, 10.0, 10.0}, {kT + 2 * kDay, 20.0, 20.0}},
                    "UTC", 0.0);
    // More than two 64 KiB windows; month boundaries, DST and return walks
    // all cross refills. A deterministic walk supplies expected array values.
    std::vector<pf_equity_point_t> observations;
    double equity = kCapital;
    for (std::int64_t i = 0; i < 6000; ++i) {
        equity += static_cast<double>((i * 37) % 31 - 15) / 8.0;
        observations.push_back({kT + i * 6 * 60 * kMinute, equity,
                                i % 2 == 0 ? -0.0 : static_cast<double>(i % 11)});
    }
    for (const char* zone : {"UTC", "America/New_York", "Asia/Tokyo"})
        compare_sources(observations, zone);
}

void fresh_reads_use_final_scratch_values() {
    const pf_equity_point_t points[] = {
        {kT, kCapital, 0.0}, {kT, 1100.0, 100.0},
        {kT + 365 * kDay, 1045.0, 45.0}};
    Scratch scratch(points, 3);
    pf_metrics_t before{}, after{}, expected{};
    {
        const metrics::SpillCurve source(scratch.region);
        internal::compute_selected_report_metrics(
            before, nullptr, 0, source, 3, 0.0, kCapital, "UTC", 100.0, 104.5, 1, 2);
    }
    // The new read sees the final presentation's copy, including negative zero.
    // That the execution input stays untouched is checked on a host, in
    // compare_selected_run.
    pf_equity_point_t presented = points[2];
    presented.equity = 1023.5;
    presented.open_profit = -0.0;
    scratch.replace_tail(presented);
    {
        const metrics::SpillCurve source(scratch.region);
        internal::compute_selected_report_metrics(
            after, nullptr, 0, source, 3, 0.0, kCapital, "UTC", 100.0, 104.5, 1, 2);
    }
    pf_equity_point_t final_points[] = {points[0], points[1], presented};
    internal::compute_report_metrics(
        expected, nullptr, 0, final_points, 3, 0.0, kCapital, "UTC", 100.0, 104.5, 1, 2);
    CHECK(!same_metrics(before, after));
    CHECK(same_metrics(after, expected));
    CHECK(std::signbit(after.equity.open_pl));
}

void invalid_selected_counts_refuse() {
    const pf_equity_point_t point{kT, kCapital, 0.0};
    const metrics::ArrayCurve source(&point, 1);
    for (const auto n : {std::int64_t{0}, std::int64_t{2}}) {
        bool refused = false;
        try {
            pf_metrics_t result{};
            internal::compute_selected_report_metrics(
                result, nullptr, 0, source, n, 0.0, kCapital, "UTC", kNaN, kNaN, 0, 0);
        } catch (const std::logic_error& error) {
            refused = true;
            CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
        }
        CHECK(refused);
    }
    for (const auto m : {std::int64_t{-1}, std::int64_t{1}}) {
        bool refused = false;
        try {
            pf_metrics_t result{};
            internal::compute_selected_report_metrics(
                result, nullptr, 0, source, 1, 0.0, kCapital, "UTC", kNaN, kNaN, 0, m);
        } catch (const std::logic_error& error) {
            refused = true;
            CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
        }
        CHECK(refused);
    }
}

struct Directory {
    std::string path;
    std::vector<std::string> files;
    Directory() {
        char pattern[] = "/tmp/pineforge-l17b-XXXXXX";
        const char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("L17B mkdtemp failed");
        path = created;
    }
    ~Directory() {
        for (const auto& file : files) std::remove(file.c_str());
        rmdir(path.c_str());
    }
    std::string file(const char* name) {
        files.push_back(path + '/' + name);
        return files.back();
    }
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

struct Host final : NativeStrategyHost {
    bool open_position = false;
    bool edit_tail = false;
    bool edit_middle = false;
    bool shrink = false;
    bool hide_curve = false;
    bool legacy_full_curve = false;
    std::int64_t published_limit = -1;
    std::int64_t tail_cut = 0;
    bool entered = false;
    mutable int presentations = 0;
    mutable std::int64_t length_at_presentation = 0;
    mutable std::int64_t actual_length_at_presentation = 0;
    mutable std::array<pf_equity_point_t, kSelectedPresentationWindowPoints> tail_at_presentation{};
    mutable std::uint64_t hash_at_capture = 0;
    // The execution input as the presentation hook sees it, before any edit.
    mutable std::vector<pf_equity_point_t> held_at_presentation;

    std::vector<pf_equity_point_t> held_curve() const { return equity_curve_; }
    std::int64_t held_bars_in_market() const { return bars_in_market_; }
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        if (open_position && !entered && context.script_bar_open_ms >= kT + 2 * kMinute) {
            entered = true;
            (void)submit({no::Transact{1.0}, "entry", ""});
        }
    }
    void present_report(ReportC* out) const override {
        ++presentations;
        actual_length_at_presentation = out->equity_curve_len;
        length_at_presentation = legacy_full_curve ? out->equity_curve_len
            : std::min(out->equity_curve_len, kSelectedPresentationWindowPoints);
        held_at_presentation = equity_curve_;
        if (legacy_full_curve) {
            // Preserve the old unrestricted hook for sink-OFF regressions.
            if (edit_middle && out->equity_curve_len > 2) {
                auto& point = out->equity_curve[out->equity_curve_len / 2];
                point.equity -= 0.75;
                point.open_profit = -0.0;
            }
            if (edit_tail && out->equity_curve_len > 1) {
                out->equity_curve[out->equity_curve_len - 1].equity = 1023.5;
                out->equity_curve[out->equity_curve_len - 1].open_profit = -0.0;
            }
            if (shrink && out->equity_curve_len > 1) out->equity_curve_len = 1;
            if (published_limit >= 0 && out->equity_curve_len > published_limit)
                out->equity_curve_len = published_limit;
            if (hide_curve) out->equity_curve_len = 0;
        } else {
            // The independent sink-OFF oracle still receives the full array.
            // Its test hook explicitly applies the same tail-only operation;
            // no full logical length is supplied to the sink-ON hook.
            const auto window_len = length_at_presentation;
            auto* const tail = out->equity_curve + (out->equity_curve_len - window_len);
            for (std::int64_t i = 0; i < window_len; ++i)
                tail_at_presentation[static_cast<std::size_t>(i)] = tail[i];
            if (edit_middle && window_len > 1) {
                auto& point = tail[window_len - 2];
                point.equity -= 0.75;
                point.open_profit = -0.0;
            }
            if (edit_tail && window_len > 1) {
                tail[window_len - 1].equity = 1023.5;
                tail[window_len - 1].open_profit = -0.0;
            }
            auto remaining = window_len;
            if (shrink && remaining > 1) --remaining;
            remaining -= std::min(remaining, tail_cut);
            if (hide_curve) remaining = 0;
            out->equity_curve_len -= window_len - remaining;
        }
        hash_at_capture = broker_state_hash();
    }
};

NativeRunSpec spec(std::uint64_t generation) {
    NativeRunSpec result;
    result.identity = {"cr150-l17b", generation};
    result.input_tf = result.script_tf = "1";
    result.ticker = "MOCK";
    result.tickerid = "TEST:MOCK";
    result.type = "crypto";
    result.currency = "USD";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = kCapital;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

pf_report_sink_receipt_v1 receipt(const BacktestEngine& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    CHECK(host.report_sink_receipt_v1(&result) == 0);
    return result;
}

std::string read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("L17B sink read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> record_lines(const std::string& bytes, const char* tag) {
    std::vector<std::string> result;
    const std::string prefix = std::string("[\"") + tag + "\",";
    for (std::size_t at = 0; at < bytes.size();) {
        const auto end = bytes.find('\n', at);
        CHECK(end != std::string::npos);
        if (end == std::string::npos) break;
        if (bytes.compare(at, prefix.size(), prefix) == 0)
            result.push_back(bytes.substr(at, end - at));
        at = end + 1;
    }
    return result;
}

std::string equity_line(const pf_equity_point_t& point) {
    return "[\"E\"," + std::to_string(point.time_ms) + ','
        + report_sink::f64(point.equity) + ',' + report_sink::f64(point.open_profit) + ']';
}

std::string trade_line(const BacktestEngine& host, const ReportC& report, int index) {
    const auto& row = report.trades[index];
    const auto& identity = host.get_report_trade(index);
    std::string line = "[\"T\"," + std::to_string(index + 1)
        + (row.is_long ? ",\"long\"," : ",\"short\",")
        + std::to_string(row.entry_time) + ',' + std::to_string(row.exit_time);
    for (const double value : {row.entry_price, row.exit_price, row.qty, row.pnl, row.pnl_pct,
                              row.max_runup, row.max_drawdown, row.commission})
        line += ',' + report_sink::f64(value);
    line += ',' + std::to_string(row.entry_bar_index) + ','
        + std::to_string(row.exit_bar_index) + (row.open_at_end ? ",true," : ",false,")
        + std::to_string(identity.entry_incarnation) + ','
        + std::to_string(host.closed_trade_close_cause(index)) + ','
        + report_sink::escaped_string(identity.entry_id) + ','
        + report_sink::escaped_string(identity.exit_id) + ','
        + report_sink::escaped_string(identity.exit_comment) + ']';
    return line;
}

// TradeC's two padding gaps are not part of a row's values. Compare every
// actual field byte, including signed zeros, after clearing only those gaps.
void canonical_trade(TradeC& row) {
    auto* bytes = reinterpret_cast<unsigned char*>(&row);
    std::memset(bytes + offsetof(TradeC, is_long) + sizeof(row.is_long), 0,
                offsetof(TradeC, max_runup) - offsetof(TradeC, is_long) - sizeof(row.is_long));
    std::memset(bytes + offsetof(TradeC, open_at_end) + sizeof(row.open_at_end), 0,
                sizeof(row) - offsetof(TradeC, open_at_end) - sizeof(row.open_at_end));
}

void compare_trades(const ReportC& array, const ReportC& restored) {
    CHECK(restored.trades_len == array.trades_len);
    for (int i = 0; i < array.trades_len && i < restored.trades_len; ++i) {
        auto expected = array.trades[i];
        auto actual = restored.trades[i];
        canonical_trade(expected);
        canonical_trade(actual);
        CHECK(std::memcmp(&actual, &expected, sizeof expected) == 0);
    }
}

void compare_sink_records(const BacktestEngine& baseline, const ReportC& array,
                          const std::string& bytes) {
    const auto equities = record_lines(bytes, "E");
    const auto trades = record_lines(bytes, "T");
    CHECK(equities.size() == static_cast<std::size_t>(array.equity_curve_len));
    for (std::size_t i = 0; i < equities.size()
         && i < static_cast<std::size_t>(array.equity_curve_len); ++i)
        CHECK(equities[i] == equity_line(array.equity_curve[i]));
    CHECK(trades.size() == static_cast<std::size_t>(array.trades_len));
    for (std::size_t i = 0; i < trades.size()
         && i < static_cast<std::size_t>(array.trades_len); ++i)
        CHECK(trades[i] == trade_line(baseline, array, static_cast<int>(i)));
}

void selected_run(Host& host, int start, bool gap, std::uint64_t generation,
                  int extra_bars = 0) {
    std::vector<Bar> bars = {
        {50.0, 50.0, 50.0, 50.0, 1.0, kT},
        {50.0, 50.0, 50.0, 50.0, 1.0, kT + kMinute},
        {100.0, 100.0, 100.0, 100.0, 1.0, kT + 2 * kMinute},
        {100.0, 110.0, 100.0, 110.0, 1.0, kT + 3 * kMinute},
        {110.0, 110.0, 104.5, 104.5, 1.0, kT + 4 * kMinute}};
    for (int i = 0; i < extra_bars; ++i)
        bars.push_back({104.5, 104.5, 104.5, 104.5, 1.0, kT + (5LL + i) * kMinute});
    if (gap) bars.erase(bars.begin() + 2);
    CHECK(host.configure_native(spec(generation)).status == NativeSetupStatus::Applied);
    const pf_selected_window_config_v1 window{
        sizeof window, 1, kT + start * kMinute, kT + (10LL + extra_bars) * kMinute};
    CHECK(native_set_selected_window_v1(&host, &window) == 0);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

void compare_selected_run(Directory& directory, const char* name, int start,
                          bool open, bool edit = false, bool shrink = false,
                          bool gap = false, bool hide = false, int extra_bars = 0,
                          bool edit_penultimate = false, std::int64_t tail_cut = 0) {
    Host baseline, streamed;
    baseline.open_position = streamed.open_position = open;
    baseline.edit_tail = streamed.edit_tail = edit;
    baseline.edit_middle = streamed.edit_middle = edit_penultimate;
    baseline.shrink = streamed.shrink = shrink;
    baseline.hide_curve = streamed.hide_curve = hide;
    baseline.tail_cut = streamed.tail_cut = tail_cut;
    const auto path = directory.file(name);
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L17B", directory.path.c_str()};
    CHECK(streamed.set_report_sink_v1(&sink) == 0);
    selected_run(baseline, start, gap, 1, extra_bars);
    selected_run(streamed, start, gap, 1, extra_bars);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    CHECK(native_selected_window_counts_v1(&streamed, &counts) == 0);
    const int expected_observations = std::max(0, 5 - start)
        - (gap && start <= 2 ? 1 : 0) + extra_bars;
    CHECK(counts.window_script_bars == static_cast<std::uint64_t>(expected_observations));
    Report array, scratch, again;
    baseline.fill_report(&array.value);
    // Presentation edits only its own copy: the host's held curve is
    // bit-identical before and after the sink-on read, and to what the
    // presentation hook saw before it edited.
    const auto held_before = streamed.held_curve();
    streamed.fill_report(&scratch.value);
    const auto held_after = streamed.held_curve();
    CHECK(same_points(held_before, held_after));
    CHECK(same_points(streamed.held_at_presentation, held_after));
    CHECK(streamed.length_at_presentation == baseline.length_at_presentation);
    const auto logical_len = static_cast<std::int64_t>(counts.window_script_bars) + 1;
    const auto window_len = std::min(logical_len, kSelectedPresentationWindowPoints);
    CHECK(baseline.actual_length_at_presentation == logical_len);
    CHECK(streamed.actual_length_at_presentation == window_len);
    CHECK(streamed.length_at_presentation == window_len);
    CHECK(std::memcmp(streamed.tail_at_presentation.data(), baseline.tail_at_presentation.data(),
                      static_cast<std::size_t>(window_len) * sizeof(pf_equity_point_t)) == 0);
    const auto after_shrink = shrink && window_len > 1 ? window_len - 1 : window_len;
    const auto cut = hide ? window_len : window_len - after_shrink
        + std::min(after_shrink, tail_cut);
    CHECK(array.value.equity_curve_len == logical_len - cut);
    CHECK(same_metrics(array.value.metrics, scratch.value.metrics));
    CHECK(array.value.net_profit == scratch.value.net_profit);
    CHECK(array.value.total_trades == scratch.value.total_trades);
    CHECK(baseline.hash_at_capture == streamed.hash_at_capture);
    CHECK(scratch.value.equity_curve == nullptr && scratch.value.equity_curve_len == 0);
    CHECK(scratch.value.trades == nullptr && scratch.value.trades_len == 0);
    CHECK(receipt(streamed).count_e == array.value.equity_curve_len);
    CHECK(receipt(streamed).complete == 1);
    if (open) {
        CHECK(array.value.trades_len > 0);
        if (array.value.trades_len > 0)
            CHECK(array.value.trades[array.value.trades_len - 1].open_at_end == 1);
    }
    if (start == 5) {
        if (hide) CHECK(array.value.equity_curve_len == 0);
        else CHECK(array.value.equity_curve_len == 1);
        CHECK(std::isnan(scratch.value.metrics.equity.time_in_market_pct));
        CHECK(std::isnan(scratch.value.metrics.equity.buy_hold_return));
    }
    const auto sink_bytes = read(path);
    compare_sink_records(baseline, array.value, sink_bytes);
    CHECK(receipt(streamed).count_t == array.value.trades_len);
    streamed.fill_report(&again.value);
    CHECK(same_metrics(scratch.value.metrics, again.value.metrics));
    CHECK(read(path) == sink_bytes);
    CHECK(streamed.presentations == 1);
    CHECK(baseline.presentations == 1);
    // Sink setters are allowed while quiescent. Removing the path writer
    // must not invalidate this generation's independent metric snapshot.
    CHECK(streamed.set_report_sink_v1(nullptr) == 0);
    Report without_sink;
    streamed.fill_report(&without_sink.value);
    CHECK(same_metrics(array.value.metrics, without_sink.value.metrics));
    CHECK(without_sink.value.equity_curve_len == array.value.equity_curve_len);
    compare_trades(array.value, without_sink.value);
    // Removing the sink reads the private metric region through SpillCurve.
    // Both paths allocate max(published length, M+1), so compare ALL stored
    // points, including a suffix hidden by shrink/hide. The reference is the
    // independently captured sink-off report, never the NDJSON or a literal.
    const auto metric_points = static_cast<std::size_t>(counts.window_script_bars + 1);
    CHECK(array.value.equity_curve != nullptr && without_sink.value.equity_curve != nullptr);
    if (array.value.equity_curve && without_sink.value.equity_curve) {
        CHECK(std::memcmp(without_sink.value.equity_curve, array.value.equity_curve,
                          metric_points * sizeof(pf_equity_point_t)) == 0);
    }
    if (without_sink.value.equity_curve != nullptr && array.value.equity_curve != nullptr
        && without_sink.value.equity_curve_len == array.value.equity_curve_len) {
        CHECK(std::memcmp(without_sink.value.equity_curve, array.value.equity_curve,
                          static_cast<std::size_t>(array.value.equity_curve_len)
                              * sizeof(pf_equity_point_t)) == 0);
    } else {
        CHECK(false);
    }
    CHECK(streamed.presentations == 1);
    CHECK(streamed.set_report_sink_v1(&sink) == 0);

    // A new admitted M=0 generation must use its own anchor and region after
    // an open or edited predecessor, without reusing any captured metric data.
    baseline.open_position = streamed.open_position = false;
    baseline.edit_tail = streamed.edit_tail = false;
    baseline.edit_middle = streamed.edit_middle = false;
    baseline.shrink = streamed.shrink = false;
    baseline.hide_curve = streamed.hide_curve = false;
    baseline.published_limit = streamed.published_limit = -1;
    baseline.tail_cut = streamed.tail_cut = 0;
    selected_run(baseline, 5, false, 2);
    selected_run(streamed, 5, false, 2);
    Report next_array, next_scratch;
    baseline.fill_report(&next_array.value);
    streamed.fill_report(&next_scratch.value);
    CHECK(same_metrics(next_array.value.metrics, next_scratch.value.metrics));
    CHECK(receipt(streamed).count_e == 1);
    CHECK(streamed.presentations == 2);
}

void legacy_sink_off_hooks_stay_full() {
    const auto extra = static_cast<int>(3 * report_sink::kScratchWindowPoints);
    Host plain;
    plain.legacy_full_curve = true;
    plain.open_position = true;
    selected_run(plain, 2, false, 1, extra);
    Report original;
    plain.fill_report(&original.value);
    const auto logical_len = original.value.equity_curve_len;
    CHECK(logical_len > static_cast<std::int64_t>(report_sink::kScratchWindowPoints));
    for (int mode = 0; mode < 3; ++mode) {
        Host legacy;
        legacy.legacy_full_curve = true;
        legacy.open_position = true;
        legacy.edit_middle = legacy.edit_tail = true;
        legacy.shrink = mode == 0;
        legacy.hide_curve = mode == 1;
        legacy.published_limit = mode == 2
            ? static_cast<std::int64_t>(report_sink::kScratchWindowPoints) + 1 : -1;
        selected_run(legacy, 2, false, 1, extra);
        Report actual;
        legacy.fill_report(&actual.value);
        CHECK(legacy.actual_length_at_presentation == logical_len);
        CHECK(legacy.length_at_presentation == logical_len);
        const auto published = mode == 0 ? std::int64_t{1}
            : mode == 1 ? std::int64_t{0} : legacy.published_limit;
        CHECK(actual.value.equity_curve_len == published);
        std::vector<pf_equity_point_t> expected(original.value.equity_curve,
                                               original.value.equity_curve + logical_len);
        expected[static_cast<std::size_t>(logical_len / 2)].equity -= 0.75;
        expected[static_cast<std::size_t>(logical_len / 2)].open_profit = -0.0;
        expected.back().equity = 1023.5;
        expected.back().open_profit = -0.0;
        CHECK(actual.value.equity_curve != nullptr);
        if (actual.value.equity_curve)
            CHECK(std::memcmp(actual.value.equity_curve, expected.data(),
                              expected.size() * sizeof(pf_equity_point_t)) == 0);
        pf_metrics_t expected_metrics{};
        internal::compute_report_metrics(
            expected_metrics, original.value.trades, original.value.trades_len,
            expected.data(), logical_len, original.value.net_profit, kCapital, "UTC",
            100.0, 104.5, plain.held_bars_in_market(), logical_len - 1);
        CHECK(same_metrics(actual.value.metrics, expected_metrics));
        compare_trades(original.value, actual.value);
        CHECK(legacy.presentations == 1);
    }
}

#ifdef PINEFORGE_L17B_SOURCE
struct PineHost final : source::PineStrategyHost {
    explicit PineHost(bool open, bool long_side) : open_(open), long_side_(long_side) {
        source::PineStrategyConfig config;
        config.initial_capital = kCapital;
        config.default_qty_value = 1.0;
        config.commission_value = 0.125;
        configure_pine_strategy(config);
    }
    mutable int presentations = 0;
    mutable std::int64_t presented_length = 0;
    void on_source_bar(const Bar&) override {
        if (open_ && bar_index_ == 2) strategy_entry("held", long_side_);
    }
    void present_report(ReportC* report) const override {
        ++presentations;
        presented_length = report->equity_curve_len;
        source::ReportSinkEquityTestAccess::present(*this, report);
    }
private:
    bool open_;
    bool long_side_;
};

void compare_pine_selected(Directory& directory, const char* name, bool open,
                           bool long_side, bool quote, int extra_bars = 0) {
    PineHost baseline(open, long_side), streamed(open, long_side);
    std::vector<Bar> bars = {
        {50.0, 50.0, 50.0, 50.0, 1.0, kT},
        {50.0, 50.0, 50.0, 50.0, 1.0, kT + kMinute},
        {100.0, 100.0, 100.0, 100.0, 1.0, kT + 2 * kMinute},
        {100.0, 110.0, 100.0, 110.0, 1.0, kT + 3 * kMinute},
        {110.0, 110.0, 104.5, 104.5, 1.0, kT + 4 * kMinute}};
    for (int i = 0; i < extra_bars; ++i)
        bars.push_back({104.5, 104.5, 104.5, 104.5, 1.0, kT + (5LL + i) * kMinute});
    if (quote) {
        // Match the real terminal-quote eligibility: a magnified 15-minute
        // script over 61 one-minute inputs, as test_report_terminal_quote.
        bars.clear();
        for (int i = 0; i <= 60; ++i) {
            const double price = 100.0 + i * 0.125;
            bars.push_back({price, price + 0.25, price - 0.25, price + 0.125,
                            1.0, kT + i * kMinute});
        }
    }
    const auto path = directory.file(name);
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L17B", directory.path.c_str()};
    CHECK(streamed.set_report_sink_v1(&sink) == 0);
    // Empty coverage is after all inputs; otherwise entry at bar 2 is in range.
    const pf_selected_window_config_v1 window{sizeof window, 1,
        open ? (quote ? kT : kT + 2 * kMinute) : bars.back().timestamp + kMinute,
        bars.back().timestamp + 10 * kMinute};
    CHECK(native_set_selected_window_v1(&baseline, &window) == 0);
    CHECK(native_set_selected_window_v1(&streamed, &window) == 0);
    if (quote) {
        CHECK(baseline.set_report_terminal_quote(bars.back().timestamp, 112.0));
        CHECK(streamed.set_report_terminal_quote(bars.back().timestamp, 112.0));
    }
    baseline.run(bars.data(), static_cast<int>(bars.size()), "1", quote ? "15" : "1", quote);
    streamed.run(bars.data(), static_cast<int>(bars.size()), "1", quote ? "15" : "1", quote);
    CHECK(baseline.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(streamed.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 array_counts{}, scratch_counts{};
    array_counts.struct_size = scratch_counts.struct_size = sizeof array_counts;
    array_counts.version = scratch_counts.version = 1;
    CHECK(native_selected_window_counts_v1(&baseline, &array_counts) == 0);
    CHECK(native_selected_window_counts_v1(&streamed, &scratch_counts) == 0);
    CHECK(array_counts.window_script_bars == scratch_counts.window_script_bars);
    Report array, summary, restored, again;
    baseline.fill_report(&array.value);
    streamed.fill_report(&summary.value);
    CHECK(same_metrics(array.value.metrics, summary.value.metrics));
    CHECK(std::memcmp(&array.value.net_profit, &summary.value.net_profit,
                      sizeof array.value.net_profit) == 0);
    CHECK(array.value.total_trades == summary.value.total_trades);
    const auto logical_length = static_cast<std::int64_t>(array_counts.window_script_bars) + 1;
    CHECK(baseline.presented_length == logical_length);
    CHECK(streamed.presented_length
          == std::min(logical_length, kSelectedPresentationWindowPoints));
    CHECK(receipt(streamed).count_e == array.value.equity_curve_len);
    CHECK(receipt(streamed).count_t == array.value.trades_len);
    CHECK(receipt(streamed).complete == 1);
    const auto bytes = read(path);
    compare_sink_records(baseline, array.value, bytes);
    if (open) {
        CHECK(array.value.trades_len > 0);
        if (array.value.trades_len > 0) {
            const auto& last = array.value.trades[array.value.trades_len - 1];
            CHECK(last.open_at_end == 1);
            if (quote) {
                CHECK(baseline.report_terminal_quote_applied(array.value));
                CHECK(last.exit_time == bars.back().timestamp);
                CHECK(last.exit_price == 112.0);
            }
        }
    }
    streamed.fill_report(&again.value);
    CHECK(same_metrics(summary.value.metrics, again.value.metrics));
    CHECK(read(path) == bytes);
    CHECK(streamed.set_report_sink_v1(nullptr) == 0);
    streamed.fill_report(&restored.value);
    CHECK(same_metrics(array.value.metrics, restored.value.metrics));
    CHECK(restored.value.equity_curve_len == array.value.equity_curve_len);
    if (array.value.equity_curve && restored.value.equity_curve)
        CHECK(std::memcmp(array.value.equity_curve, restored.value.equity_curve,
                          static_cast<std::size_t>(logical_length)
                              * sizeof(pf_equity_point_t)) == 0);
    else CHECK(false);
    compare_trades(array.value, restored.value);
    if (quote) CHECK(streamed.report_terminal_quote_applied(restored.value));
    CHECK(baseline.presentations == 1 && streamed.presentations == 1);
}
#endif

#if PINEFORGE_LIBRARY_DEBUG_CHECKS
struct InvalidWindowHost final : NativeStrategyHost {
    enum class Fault { grow, negative, replace, throw_after_replace };
    explicit InvalidWindowHost(Fault fault) : fault_(fault) {}
    mutable int presentations = 0;
    mutable pf_equity_point_t replacement{};
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    void present_report(ReportC* report) const override {
        ++presentations;
        switch (fault_) {
        case Fault::grow: ++report->equity_curve_len; break;
        case Fault::negative: report->equity_curve_len = -1; break;
        case Fault::replace: report->equity_curve = &replacement; break;
        case Fault::throw_after_replace:
            report->equity_curve = &replacement;
            throw std::runtime_error("L17B hook threw after replacing its window");
        }
    }
private:
    Fault fault_;
};

void invalid_window_hooks_refuse(Directory& directory) {
    for (const auto fault : {InvalidWindowHost::Fault::grow, InvalidWindowHost::Fault::negative,
                            InvalidWindowHost::Fault::replace,
                            InvalidWindowHost::Fault::throw_after_replace}) {
        InvalidWindowHost host(fault);
        const auto path = directory.file("invalid-window.ndjson");
        const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L17B",
                                     directory.path.c_str()};
        CHECK(host.set_report_sink_v1(&sink) == 0);
        CHECK(host.configure_native(spec(1)).status == NativeSetupStatus::Applied);
        const pf_selected_window_config_v1 window{sizeof window, 1, kT, kT + 10 * kMinute};
        CHECK(native_set_selected_window_v1(&host, &window) == 0);
        const Bar bars[] = {{100.0, 100.0, 100.0, 100.0, 1.0, kT},
                            {100.0, 100.0, 100.0, 100.0, 1.0, kT + kMinute}};
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.presentations == 1);
        const auto message = fault == InvalidWindowHost::Fault::throw_after_replace
            ? "L17B hook threw after replacing its window"
            : "native selected report: presentation exceeded its tail window";
        CHECK(host.last_error().find(message) != std::string::npos);
        CHECK(receipt(host).complete == 0);
        Report refused;
        host.fill_report(&refused.value);
        CHECK(refused.value.equity_curve == nullptr && refused.value.trades == nullptr);
        CHECK(read(path).find("{\"trailer\":") == std::string::npos);
    }
}
#endif

struct CaptureStorage {
    std::size_t largest_request = 0;
    std::size_t peak_live = 0;
};

CaptureStorage measured_capture;
void record_capture_allocation(std::size_t bytes) {
    measured_capture.largest_request = std::max(measured_capture.largest_request, bytes);
    measured_capture.peak_live = std::max(measured_capture.peak_live,
                                         global_allocation::live_bytes);
}

struct CaptureAllocationScope {
    CaptureAllocationScope() {
        measured_capture = {};
        global_allocation::live_bytes = 0;
        global_allocation::on_recorded = record_capture_allocation;
        global_allocation::recording = true;
    }
    ~CaptureAllocationScope() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

// Keep the test's input, warmup and recorded execution storage outside the
// measured interval. stream_end(false) appends no bar: it seals the existing
// selected window and performs the real capture. This host allocates nothing
// in its presentation hook, unlike Host's independent input snapshots above.
struct StorageHost final : NativeStrategyHost {
    mutable int presentations = 0;
    mutable std::int64_t presented_length = 0;
    mutable pf_equity_point_t first_presented{}, last_presented{};
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    void present_report(ReportC* report) const override {
        ++presentations;
        presented_length = report->equity_curve_len;
        if (presented_length > 0) {
            first_presented = report->equity_curve[0];
            last_presented = report->equity_curve[presented_length - 1];
        }
    }
};

CaptureStorage selected_capture_storage(Directory& directory, const char* name, int m) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(m));
    for (int i = 0; i < m; ++i)
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0, kT + i * kMinute});
    StorageHost host;
    CHECK(host.configure_native(spec(1)).status == NativeSetupStatus::Applied);
    const auto path = directory.file(name);
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L17B", directory.path.c_str()};
    CHECK(host.set_report_sink_v1(&sink) == 0);
    const pf_selected_window_config_v1 window{sizeof window, 1, kT, kT + m * kMinute};
    CHECK(native_set_selected_window_v1(&host, &window) == 0);
    const bool warmed = host.stream_begin(bars.data(), m, "", "");
    CHECK(warmed);
    if (!warmed) throw std::runtime_error("L17B storage warmup failed");
    CHECK(host.presentations == 0);
    bool ended = false;
    {
        const CaptureAllocationScope allocation_scope;
        ended = host.stream_end();
    }
    const auto storage = measured_capture;
    CHECK(ended);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.presentations == 1);
    const auto window_length =
        std::min<std::int64_t>(static_cast<std::int64_t>(m) + 1,
                              kSelectedPresentationWindowPoints);
    CHECK(host.presented_length == window_length);
    CHECK(host.first_presented.time_ms
          == bars[static_cast<std::size_t>(m - window_length)].timestamp);
    CHECK(host.last_presented.time_ms == bars.back().timestamp);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    CHECK(counts.window_script_bars == static_cast<std::uint64_t>(m));
    pf_report_sink_receipt_v1 written{};
    written.struct_size = sizeof written;
    written.version = 1;
    CHECK(host.report_sink_receipt_v1(&written) == 0);
    CHECK(written.count_e == static_cast<std::int64_t>(m) + 1);
    // No fill_report/metrics/all-array read participates in this measurement.
    return storage;
}

void selected_capture_storage_is_bounded(Directory& directory) {
    const auto small = selected_capture_storage(directory, "storage-1k.ndjson", 1000);
    const auto large = selected_capture_storage(directory, "storage-1m.ndjson", 1000000);
    std::printf("selected capture heap: 1k largest=%zu peak=%zu; 1m largest=%zu peak=%zu\n",
                small.largest_request, small.peak_live,
                large.largest_request, large.peak_live);
    // One production scratch batch plus at most one batch worth of fixed
    // bookkeeping/formatting overhead. Count EVERY replaceable C++ allocation
    // form and live bytes, so briefly allocating and deleting M+1 points fails.
    const auto budget = 2 * report_sink::kScratchWindowBytes;
    CHECK(small.largest_request > 0);
    CHECK(small.peak_live > 0);
    CHECK(small.largest_request == large.largest_request);
    CHECK(small.peak_live == large.peak_live);
    CHECK(small.largest_request <= budget && large.largest_request <= budget);
    CHECK(small.peak_live <= budget && large.peak_live <= budget);
}
}

int main() {
    try {
        selected_array_fixtures();
        fresh_reads_use_final_scratch_values();
        invalid_selected_counts_refuse();
        Directory directory;
        compare_selected_run(directory, "flat.ndjson", 2, false);
        compare_selected_run(directory, "open.ndjson", 2, true); // F13: M=3, open at E
        compare_selected_run(directory, "empty.ndjson", 5, false); // F13: M=0
        compare_selected_run(directory, "presented.ndjson", 2, true, true);
        compare_selected_run(directory, "shrunk.ndjson", 2, true, false, true);
        compare_selected_run(directory, "hidden.ndjson", 2, true, true, false, false, true);
        compare_selected_run(directory, "gap.ndjson", 2, false, false, false, true);
        compare_selected_run(directory, "one-observation.ndjson", 4, false);
        compare_selected_run(directory, "one-observation-cut.ndjson", 4, false,
                             true, false, false, false, 0, false, 1);
        compare_selected_run(directory, "empty-hidden.ndjson", 5, false,
                             false, false, false, true);
        const auto refills = static_cast<int>(3 * report_sink::kScratchWindowPoints);
        compare_selected_run(directory, "refills.ndjson", 2, true, true,
                             false, false, false, refills);
        compare_selected_run(directory, "refills-shrunk.ndjson", 2, true, true,
                             true, false, false, refills);
        compare_selected_run(directory, "refills-hidden.ndjson", 2, true, true,
                             false, false, true, refills);
        compare_selected_run(directory, "refills-custom.ndjson", 2, true, true,
                             false, false, false, refills, true, 1);
        compare_selected_run(directory, "refills-cut-window.ndjson", 2, true, true,
                             false, false, false, refills, true,
                             kSelectedPresentationWindowPoints);
        legacy_sink_off_hooks_stay_full();
#ifdef PINEFORGE_L17B_SOURCE
        compare_pine_selected(directory, "pine-open.ndjson", true, true, false);
        compare_pine_selected(directory, "pine-quote-long.ndjson", true, true, true);
        compare_pine_selected(directory, "pine-quote-short.ndjson", true, false, true);
        compare_pine_selected(directory, "pine-empty.ndjson", false, true, false);
        compare_pine_selected(directory, "pine-refills.ndjson", true, true, false, refills);
#endif
#if PINEFORGE_LIBRARY_DEBUG_CHECKS
        invalid_window_hooks_refuse(directory);
#endif
        selected_capture_storage_is_bounded(directory);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "L17B exception: %s\n", error.what());
    }
    std::printf("test_selected_metrics_l17b: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
