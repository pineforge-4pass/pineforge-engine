// Build this file as a real whole-archive strategy module and
// its dlopen driver. On f00151f223d35d7638c299a60ab401df3f36cf0d the first
// hold-back check fails: after N points, before fill, count_e is 0, not N-1.
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/execution_observer.h>
#include <pineforge/selected_window.h>

#include <cstdint>

struct EquitySnapshot {
    pf_equity_point_t last{};
    double extremes[4]{};
    std::int64_t count = 0;
    std::int64_t presentations = 0;
    std::int64_t presented_length = 0;
};

#ifdef PINEFORGE_L6B2_MODULE
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/run_failure.hpp>
#include "../src/report_scratch.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace pineforge::source {
// A narrow friend seam, defined only in this lane's real strategy fixture.
// Qualifying the presentation call preserves the base implementation and
// avoids redispatching to EquityFixture's instrumented override.
struct ReportSinkEquityTestAccess {
    static void present(const PineStrategyHost& host, ReportC* report) {
        host.PineStrategyHost::present_report(report);
    }
    static void remark(PineStrategyHost& host, const Bar& terminal) {
        host.scheduler_record_range_end(terminal);
    }
};
} // namespace pineforge::source

namespace {
class EquityFixture final : public pineforge::source::PineStrategyHost {
public:
    EquityFixture() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 2.0;
        config.commission_value = 0.125;
        configure_pine_strategy(config);
    }
    std::string mode = "open";
    mutable std::int64_t presentations = 0;
    mutable std::int64_t presented_length = 0;

    void on_source_bar(const pineforge::Bar&) override {
        if (mode == "throw" && bar_index_ == 2) throw std::runtime_error("equity fixture failed");
        if (mode != "flat" && bar_index_ == 0) strategy_entry("held", true);
    }
    void present_report(pineforge::ReportC* report) const override {
        ++presentations;
        presented_length = report->equity_curve_len;
        pineforge::source::ReportSinkEquityTestAccess::present(*this, report);
        if (mode == "present" && report->equity_curve_len > 0) {
            auto& last = report->equity_curve[report->equity_curve_len - 1];
            last.time_ms += 60000;
            last.equity -= 0.75;
            last.open_profit = -0.0;
        }
        if (mode == "shrink" && report->equity_curve_len > 1)
            report->equity_curve_len = 1;
    }
    EquitySnapshot snapshot() const {
        EquitySnapshot result;
        if (!equity_curve_.empty()) result.last = equity_curve_.back();
        result.extremes[0] = max_equity_;
        result.extremes[1] = min_equity_;
        result.extremes[2] = max_drawdown_;
        result.extremes[3] = max_runup_;
        const auto prefix = report_sink_equity_scratch_region();
        result.count = prefix.scratch != nullptr || prefix.descriptor >= 0
            ? prefix.point_count + (last_equity_point() ? 1 : 0)
            : static_cast<std::int64_t>(equity_curve_.size());
        result.presentations = presentations;
        result.presented_length = presented_length;
        return result;
    }
    bool matches_rewalk() const {
        // Independent full walk, including the initial-capital seed and the
        // trough reset on a new peak. Compare the four doubles' exact bits.
        double peak = initial_capital_, trough = initial_capital_, dd = 0.0, ru = 0.0;
        const auto fold = [&](const pf_equity_point_t& point) {
            if (point.equity > peak) { peak = point.equity; trough = point.equity; }
            if (point.equity < trough) trough = point.equity;
            const double drawdown = peak - point.equity;
            if (drawdown > dd) dd = drawdown;
            const double runup = point.equity - trough;
            if (runup > ru) ru = runup;
        };
        const auto prefix = report_sink_equity_scratch_region();
        if (prefix.scratch || prefix.descriptor >= 0) {
            const pineforge::report_sink::ScratchCurveWindow curve(prefix);
            for (std::int64_t i = 0; i < curve.size(); ++i) fold(curve[i]);
            if (const auto* held = last_equity_point()) fold(*held);
        } else {
            // Sink-off and selected runs retain the independent array walk.
            for (const auto& point : equity_curve_) fold(point);
        }
        const double expected[] = {peak, trough, dd, ru};
        const auto actual = snapshot();
        return std::memcmp(expected, actual.extremes, sizeof expected) == 0;
    }
    void remark(double close) {
        auto terminal = current_bar_;
        terminal.close = close;
        pineforge::source::ReportSinkEquityTestAccess::remark(*this, terminal);
    }
};
EquityFixture* fixture(pf_strategy_t handle) {
    return static_cast<EquityFixture*>(static_cast<pineforge::BacktestEngine*>(handle));
}
}
extern "C" {
pf_strategy_t strategy_create(const char*) { return new EquityFixture; }
void strategy_free(pf_strategy_t handle) { delete fixture(handle); }
void strategy_set_input(pf_strategy_t handle, const char*, const char* value) {
    fixture(handle)->mode = value ? value : "open";
}
void strategy_set_override(pf_strategy_t, const char*, const char*) {}
int l6b2_run_only(pf_strategy_t handle, const pf_bar_t* bars, int n, int magnifier) {
    try {
        std::vector<pineforge::Bar> input;
        for (int i = 0; i < n; ++i)
            input.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                             bars[i].volume, bars[i].timestamp});
        fixture(handle)->run(input.data(), n, "1", magnifier ? "15" : "1", magnifier != 0);
        return fixture(handle)->last_error().empty() ? 0 : 1;
    } catch (const std::exception& error) {
        pineforge::note_run_failure(*fixture(handle), "l6b2_run_only", error);
        return 1;
    }
}
void l6b2_snapshot(pf_strategy_t handle, EquitySnapshot* out) { *out = fixture(handle)->snapshot(); }
int l6b2_rewalk(pf_strategy_t handle) { return fixture(handle)->matches_rewalk() ? 1 : 0; }
void l6b2_remark(pf_strategy_t handle, double close) { fixture(handle)->remark(close); }
int l6b2_quote(pf_strategy_t handle, std::int64_t time, double close) {
    return fixture(handle)->set_report_terminal_quote(time, close) ? 1 : 0;
}
void l6b2_fill(pf_strategy_t handle, pf_report_t* out) {
    fixture(handle)->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
}
void report_free(pf_report_t* report) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
}
}

#else
#include <pineforge/report_sink_writer.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class T> T symbol(void* module, const char* name) {
    const auto address = dlsym(module, name);
    if (!address) throw std::runtime_error(std::string("missing module symbol: ") + name);
    return reinterpret_cast<T>(address);
}
void* open_module() {
    void* module = dlopen(PINEFORGE_L6B2_MODULE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!module) throw std::runtime_error(dlerror());
    return module;
}
struct Api {
    void* module = open_module();
    decltype(&strategy_create) create = symbol<decltype(create)>(module, "strategy_create");
    decltype(&strategy_free) destroy = symbol<decltype(destroy)>(module, "strategy_free");
    decltype(&strategy_set_input) mode = symbol<decltype(mode)>(module, "strategy_set_input");
    decltype(&report_free) free = symbol<decltype(free)>(module, "report_free");
    decltype(&strategy_set_report_sink_v1) set = symbol<decltype(set)>(module, "strategy_set_report_sink_v1");
    decltype(&strategy_report_sink_receipt_v1) receipt = symbol<decltype(receipt)>(module, "strategy_report_sink_receipt_v1");
    decltype(&strategy_set_selected_window_v1) selected = symbol<decltype(selected)>(module, "strategy_set_selected_window_v1");
    decltype(&strategy_set_execution_observer_v1) observer = symbol<decltype(observer)>(module, "strategy_set_execution_observer_v1");
    decltype(&strategy_broker_state_hash) hash = symbol<decltype(hash)>(module, "strategy_broker_state_hash");
    decltype(&strategy_get_last_error) error = symbol<decltype(error)>(module, "strategy_get_last_error");
    decltype(&strategy_get_last_error_code) code = symbol<decltype(code)>(module, "strategy_get_last_error_code");
    int (*run)(pf_strategy_t, const pf_bar_t*, int, int) = symbol<decltype(run)>(module, "l6b2_run_only");
    void (*snapshot)(pf_strategy_t, EquitySnapshot*) = symbol<decltype(snapshot)>(module, "l6b2_snapshot");
    int (*rewalk)(pf_strategy_t) = symbol<decltype(rewalk)>(module, "l6b2_rewalk");
    void (*remark)(pf_strategy_t, double) = symbol<decltype(remark)>(module, "l6b2_remark");
    int (*quote)(pf_strategy_t, std::int64_t, double) = symbol<decltype(quote)>(module, "l6b2_quote");
    void (*fill)(pf_strategy_t, pf_report_t*) = symbol<decltype(fill)>(module, "l6b2_fill");
};
int run_with_diagnostics(const Api& api, pf_strategy_t handle, const pf_bar_t* bars,
                         int n, int magnifier) {
    const int result = api.run(handle, bars, n, magnifier);
    if (result != 0)
        std::fprintf(stderr, "L6b-2 engine run failed (bars=%d, magnifier=%d, code=%s): %s\n",
                     n, magnifier, api.code(handle), api.error(handle));
    return result;
}
std::string read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::vector<pf_bar_t> feed(int n) {
    std::vector<pf_bar_t> bars;
    std::uint32_t state = 0x1506b2;
    for (int i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        const double price = 100.0 + (static_cast<int>(state % 801) - 400) * 0.125;
        bars.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                        1.0, INT64_C(1700000100000) + i * INT64_C(60000)});
    }
    return bars;
}
pf_report_sink_receipt_v1 receipt(const Api& api, pf_strategy_t handle) {
    pf_report_sink_receipt_v1 value{};
    value.struct_size = sizeof value;
    value.version = 1;
    require(api.receipt(handle, &value) == 0, "receipt query failed");
    return value;
}
EquitySnapshot snapshot(const Api& api, pf_strategy_t handle) {
    EquitySnapshot value;
    api.snapshot(handle, &value);
    return value;
}
std::string equity(const pf_equity_point_t& point) {
    using pineforge::report_sink::f64;
    return "[\"E\"," + std::to_string(point.time_ms) + ',' + f64(point.equity)
        + ',' + f64(point.open_profit) + "]\n";
}
std::string equity_lines(const std::string& bytes) {
    std::string result;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto end = bytes.find('\n', at);
        require(end != std::string::npos, "sink lacks final LF");
        if (bytes.compare(at, 5, "[\"E\",") == 0) result.append(bytes, at, end + 1 - at);
        at = end + 1;
    }
    return result;
}
void compare(const Api& api, const std::string& path, const char* mode, int n,
             bool observed = false, bool selected = false, bool magnifier = false,
             bool remarks = false) {
    const auto bars = feed(n);
    const auto baseline = api.create("{}"), streamed = api.create("{}");
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L6b-2", nullptr};
    require(api.set(streamed, &sink) == 0, "sink install failed");
    for (const auto handle : {baseline, streamed}) {
        api.mode(handle, "mode", mode);
        if (std::strcmp(mode, "quote") == 0)
            require(api.quote(handle, bars.back().timestamp, 112.0) == 1, "terminal quote failed");
        if (selected) {
            // M=0: exercise the anchor-only presentation too.
            const auto start = bars.back().timestamp + 60000;
            const pf_selected_window_config_v1 window{sizeof window, 1, start, start + 60000};
            require(api.selected(handle, &window) == 0, "window install failed");
        }
        if (observed) {
            const pf_execution_observer_v1 observer{sizeof observer, 1, nullptr,
                [](void*, const pf_execution_boundary_v1*, pf_boundary_receipt_v1* receipt) -> int {
                    receipt->frame_bytes = 0;
                    return 0;
                }};
            require(api.observer(handle, &observer) == 0, "observer install failed");
        }
        require(run_with_diagnostics(api, handle, bars.data(), n, magnifier ? 1 : 0) == 0,
                "fixture run failed");
    }
    auto before = snapshot(api, streamed);
    if (!selected) {
        // FIRST RED on f00151f2: E is written only at fill, so this is zero.
        require(receipt(api, streamed).count_e == (before.count > 0 ? before.count - 1 : 0),
                "missing E hold-back: N-point run must emit N-1 E records before fill");
    }
    require(receipt(api, streamed).complete == 0, "sink completed before successful fill");
    require(api.rewalk(streamed) == 1 && api.rewalk(baseline) == 1,
            "range-end extremes differ from full re-walk");
    if (remarks) {
        // Replace a sole final peak/trough repeatedly. Restoring the prefix
        // must remove the old tail's extrema, rather than only extend them.
        for (const double close : {0.125, 900.0, 50.0, 100.0, 0.125, 120.0}) {
            api.remark(baseline, close);
            api.remark(streamed, close);
            require(api.rewalk(streamed) == 1, "O(1) replacement differs from re-walk");
            const auto left = snapshot(api, baseline), right = snapshot(api, streamed);
            require(std::memcmp(left.extremes, right.extremes, sizeof left.extremes) == 0,
                    "sink-on/off replacement extrema differ");
        }
        before = snapshot(api, streamed);
    }
    require(api.hash(baseline) == api.hash(streamed), "sink changed broker hash");
    pf_report_t expected{}, actual{}, again{};
    api.fill(baseline, &expected);
    api.fill(streamed, &actual);
    const auto after = snapshot(api, streamed);
    require(std::memcmp(&before.last, &after.last, sizeof before.last) == 0,
            "F7: presentation mutated the engine's held point");
    require(std::memcmp(before.extremes, after.extremes, sizeof before.extremes) == 0,
            "presentation mutated engine extrema");
    // The ordinary path, including observer-only capture, presents one tail.
    // L17a's selected tests exercise the full-curve presentation contract.
    if (!selected) {
        require(after.presented_length == (expected.equity_curve_len > 0 ? 1 : 0),
                "sink presentation must receive a one-point copy");
    }
    require(std::memcmp(&expected.metrics, &actual.metrics, sizeof expected.metrics) == 0,
            "sink-on/off metrics differ");
    require(expected.net_profit == actual.net_profit && expected.total_trades == actual.total_trades,
            "sink-on/off report scalars differ");
    std::string points;
    for (std::int64_t i = 0; i < expected.equity_curve_len; ++i) points += equity(expected.equity_curve[i]);
    const auto bytes = read(path);
    require(equity_lines(bytes) == points, "written E values differ from the presented report");
    const auto completed = receipt(api, streamed);
    require(completed.complete == 1 && completed.count_e == expected.equity_curve_len,
            "completed receipt has wrong E count");
    require(actual.equity_curve == nullptr && actual.equity_curve_len == 0,
            "sink report must expose summary only");
    api.fill(streamed, &again);
    require(read(path) == bytes, "repeated fill duplicated sink records");
    require(std::memcmp(&actual.metrics, &again.metrics, sizeof actual.metrics) == 0,
            "repeated fill changed metrics");
    const auto repeated = snapshot(api, streamed);
    require(std::memcmp(&before.last, &repeated.last, sizeof before.last) == 0,
            "repeated presentation mutated held point");
    // Ordinary reuse resets the prefix; one point stays held until fill.
    // Selected-to-selected reuse is covered by the unchanged L17a test.
    if (selected)
        require(api.selected(streamed, nullptr) == 0, "window clear before ordinary reuse failed");
    const auto single = feed(1);
    require(run_with_diagnostics(api, streamed, single.data(), 1, 0) == 0, "reused run failed");
    require(receipt(api, streamed).count_e == 0 && receipt(api, streamed).complete == 0,
            "reused run retained the old E prefix");
    api.free(&again); api.free(&actual); api.free(&expected);
    api.destroy(streamed); api.destroy(baseline);
}
void ordinary_shrunk_capture(const Api& api, const std::string& path) {
    std::printf("L6b-2 ordinary shrink: observed capture without a selected window\n");
    constexpr int n = 17;
    const auto bars = feed(n);
    const auto reference = api.create("{}"), shortened = api.create("{}"), streamed = api.create("{}");
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "ordinary-shrunk", nullptr};
    require(api.set(streamed, &sink) == 0, "ordinary shrink sink install failed");
    api.mode(reference, "mode", "open");
    api.mode(shortened, "mode", "shrink");
    api.mode(streamed, "mode", "shrink");
    // No window is installed. The observer takes the ordinary capture entry.
    const pf_execution_observer_v1 observer{sizeof observer, 1, nullptr,
        [](void*, const pf_execution_boundary_v1*, pf_boundary_receipt_v1* receipt) -> int {
            receipt->frame_bytes = 0;
            return 0;
        }};
    for (const auto handle : {reference, shortened, streamed}) {
        require(api.observer(handle, &observer) == 0, "ordinary shrink observer install failed");
        require(run_with_diagnostics(api, handle, bars.data(), n, 0) == 0,
                "ordinary shrink fixture run failed");
    }
    const auto full_before = snapshot(api, reference), short_before = snapshot(api, shortened);
    const auto before = snapshot(api, streamed);
    require(full_before.count == n && short_before.count == n && before.count == n,
            "ordinary shrink changed the recorded point count");
    require(full_before.presentations == 1 && short_before.presentations == 1
            && before.presentations == 1, "ordinary shrink must present once at capture");
    require(full_before.presented_length == n && short_before.presented_length == n
            && before.presented_length == 1, "ordinary shrink hook received the wrong curve");
    const auto pending = receipt(api, streamed);
    require(pending.count_e == n - 1 && pending.complete == 0,
            "ordinary shrink must hold exactly the final E record before fill");
    require(api.hash(reference) == api.hash(shortened) && api.hash(reference) == api.hash(streamed),
            "ordinary shrink changed broker execution");
    pf_report_t expected{}, cropped{}, actual{}, again{};
    api.fill(reference, &expected);
    api.fill(shortened, &cropped);
    api.fill(streamed, &actual);
    // The ordinary sink preserves N records: its one-point callback does not
    // satisfy the shrink hook's >1 condition. Sink-off publishes one point.
    // Derive the E values from the unmodified ordinary report, with no anchor.
    require(expected.equity_curve_len == n && cropped.equity_curve_len == 1,
            "ordinary shrink must distinguish full storage from the published curve");
    require(std::memcmp(expected.equity_curve, cropped.equity_curve,
                        sizeof(pf_equity_point_t)) == 0,
            "ordinary shrink changed the first published point");
    require(std::memcmp(&expected.metrics, &cropped.metrics, sizeof expected.metrics) == 0
            && std::memcmp(&expected.metrics, &actual.metrics, sizeof expected.metrics) == 0,
            "ordinary shrink changed metrics computed from the full storage");
    require(expected.net_profit == actual.net_profit && expected.total_trades == actual.total_trades,
            "ordinary shrink changed report scalars");
    std::string points;
    for (std::int64_t i = 0; i < expected.equity_curve_len; ++i) points += equity(expected.equity_curve[i]);
    const auto bytes = read(path);
    require(equity_lines(bytes) == points, "ordinary shrink lost or rewrote an E value");
    const auto completed = receipt(api, streamed);
    require(completed.complete == 1 && completed.count_e == n,
            "ordinary shrink completed with the wrong E count");
    require(actual.equity_curve == nullptr && actual.equity_curve_len == 0,
            "ordinary shrink sink report must expose summary only");
    const auto after = snapshot(api, streamed);
    require(std::memcmp(&before.last, &after.last, sizeof before.last) == 0
            && std::memcmp(before.extremes, after.extremes, sizeof before.extremes) == 0,
            "ordinary shrink mutated held execution state");
    require(after.presentations == 1, "ordinary shrink fill repeated presentation");
    api.fill(streamed, &again);
    require(read(path) == bytes && snapshot(api, streamed).presentations == 1,
            "ordinary shrink repeated fill changed records or presentation count");
    require(std::memcmp(&actual.metrics, &again.metrics, sizeof actual.metrics) == 0,
            "ordinary shrink repeated fill changed metrics");
    api.free(&again); api.free(&actual); api.free(&cropped); api.free(&expected);
    api.destroy(streamed); api.destroy(shortened); api.destroy(reference);
}
void failed_run(const Api& api, const std::string& path) {
    const auto bars = feed(8);
    const auto handle = api.create("{}");
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), nullptr, nullptr};
    require(api.set(handle, &sink) == 0, "failed-run sink install failed");
    api.mode(handle, "mode", "throw");
    require(run_with_diagnostics(api, handle, bars.data(), static_cast<int>(bars.size()), 0) != 0,
            "throwing run unexpectedly succeeded");
    pf_report_t report{};
    api.fill(handle, &report);
    require(receipt(api, handle).complete == 0, "failed run has complete receipt");
    require(read(path).find("{\"trailer\":") == std::string::npos, "failed run has trailer");
    api.free(&report); api.destroy(handle);
}
}
int main() {
    try {
        Api api;
        char directory[] = "/tmp/pineforge-l6b2-XXXXXX";
        require(mkdtemp(directory) != nullptr, "cannot create test output directory");
        const std::string root = directory;
        compare(api, root + "/open.ndjson", "open", 17, false, false, false, true);
        compare(api, root + "/single.ndjson", "flat", 1);
        compare(api, root + "/empty.ndjson", "flat", 0);
        compare(api, root + "/two.ndjson", "open", 2, false, false, false, true);
        compare(api, root + "/random.ndjson", "open", 513);
        compare(api, root + "/present.ndjson", "present", 17);
        compare(api, root + "/observed.ndjson", "present", 17, true);
        compare(api, root + "/magnifier.ndjson", "open", 61, false, false, true);
        compare(api, root + "/quote.ndjson", "quote", 61, false, false, true);
        compare(api, root + "/anchor.ndjson", "flat", 8, false, true);
        ordinary_shrunk_capture(api, root + "/ordinary-shrunk.ndjson");
        failed_run(api, root + "/failed.ndjson");
        std::printf("L6b-2 hold-back, exact tail fold, presentation copy: passed (%s)\n", directory);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L6b-2: %s\n", error.what());
        return 1;
    }
}
#endif
