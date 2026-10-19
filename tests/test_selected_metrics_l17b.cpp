// CR-150 L17B: selected metric reads over P00 sources, and real selected
// sink-on/off fixtures. RED on b3c39238e: the selected source overloads are
// undeclared; with declarations only, they and SpillCurve fail at link.
// No fixture pin is harvested. All metric bytes come from the existing array
// report path or the frozen equity reference with its selected denominator.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>

#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"
#include "../src/selected_report_view.hpp"
#include "equity_stats_reference.hpp"

#include <cerrno>
#include <cmath>
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
    // The original execution point remains unchanged.
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
    CHECK(points[2].equity != presented.equity);
    CHECK(!std::signbit(points[2].open_profit));
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
    bool shrink = false;
    bool hide_curve = false;
    bool entered = false;
    mutable int presentations = 0;
    mutable std::uint64_t hash_at_capture = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        if (open_position && !entered && context.script_bar_open_ms >= kT + 2 * kMinute) {
            entered = true;
            (void)submit({no::Transact{1.0}, "entry", ""});
        }
    }
    void present_report(ReportC* out) const override {
        ++presentations;
        if (edit_tail && out->equity_curve_len > 1) {
            out->equity_curve[out->equity_curve_len - 1].equity = 1023.5;
            out->equity_curve[out->equity_curve_len - 1].open_profit = -0.0;
        }
        if (shrink && out->equity_curve_len > 1) out->equity_curve_len = 1;
        if (hide_curve) out->equity_curve_len = 0;
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

pf_report_sink_receipt_v1 receipt(const Host& host) {
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

void selected_run(Host& host, int start, bool gap, std::uint64_t generation) {
    std::vector<Bar> bars = {
        {50.0, 50.0, 50.0, 50.0, 1.0, kT},
        {50.0, 50.0, 50.0, 50.0, 1.0, kT + kMinute},
        {100.0, 100.0, 100.0, 100.0, 1.0, kT + 2 * kMinute},
        {100.0, 110.0, 100.0, 110.0, 1.0, kT + 3 * kMinute},
        {110.0, 110.0, 104.5, 104.5, 1.0, kT + 4 * kMinute}};
    if (gap) bars.erase(bars.begin() + 2);
    CHECK(host.configure_native(spec(generation)).status == NativeSetupStatus::Applied);
    const pf_selected_window_config_v1 window{
        sizeof window, 1, kT + start * kMinute, kT + 10 * kMinute};
    CHECK(native_set_selected_window_v1(&host, &window) == 0);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

void compare_selected_run(Directory& directory, const char* name, int start,
                          bool open, bool edit = false, bool shrink = false,
                          bool gap = false, bool hide = false) {
    Host baseline, streamed;
    baseline.open_position = streamed.open_position = open;
    baseline.edit_tail = streamed.edit_tail = edit;
    baseline.shrink = streamed.shrink = shrink;
    baseline.hide_curve = streamed.hide_curve = hide;
    const auto path = directory.file(name);
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L17B", directory.path.c_str()};
    CHECK(streamed.set_report_sink_v1(&sink) == 0);
    selected_run(baseline, start, gap, 1);
    selected_run(streamed, start, gap, 1);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    CHECK(native_selected_window_counts_v1(&streamed, &counts) == 0);
    CHECK(counts.window_script_bars == (start == 5 ? 0U : gap ? 2U : 3U));
    Report array, scratch, again;
    baseline.fill_report(&array.value);
    streamed.fill_report(&scratch.value);
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
        CHECK(array.value.equity_curve_len == 1);
        CHECK(std::isnan(scratch.value.metrics.equity.time_in_market_pct));
        CHECK(std::isnan(scratch.value.metrics.equity.buy_hold_return));
    }
    const auto sink_bytes = read(path);
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
    baseline.shrink = streamed.shrink = false;
    baseline.hide_curve = streamed.hide_curve = false;
    selected_run(baseline, 5, false, 2);
    selected_run(streamed, 5, false, 2);
    Report next_array, next_scratch;
    baseline.fill_report(&next_array.value);
    streamed.fill_report(&next_scratch.value);
    CHECK(same_metrics(next_array.value.metrics, next_scratch.value.metrics));
    CHECK(receipt(streamed).count_e == 1);
    CHECK(streamed.presentations == 2);
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
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "L17B exception: %s\n", error.what());
    }
    std::printf("test_selected_metrics_l17b: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
