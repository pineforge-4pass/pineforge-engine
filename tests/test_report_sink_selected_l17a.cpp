// CR-150 L17A: selected capture emits the anchor and final window E values,
// and checks the selected invariants before a sink can acquire a trailer.
// Uses the existing L6b-1 ABI, so the first red is a runtime feature failure:
// on f00151f22 count_e is still zero after capture, until fill_report runs.
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/selected_window.h>

#include "../src/sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_selected_window_counts_v1(const BacktestEngine*, pf_selected_window_counts_v1*) noexcept;
int native_set_execution_observer_v1(BacktestEngine*, const pf_execution_observer_v1*) noexcept;
}
}

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr std::int64_t kBase = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;
constexpr double kCapital = 1000.0;

struct Directory {
    std::string path;
    std::vector<std::string> files;
    Directory() {
        char pattern[] = "/tmp/pineforge-l17a-XXXXXX";
        const char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
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

// KernelRecorded exercises actual orders, including an open range-end row.
// HostRecorded supplies deliberately inconsistent observations to exercise
// the capture's refusal precedence, without modifying the private consumer.
struct Host final : NativeStrategyHost {
    std::int64_t enter_at = -1;
    bool entered = false;
    bool record_own = false;
    bool rewrite_first = false;
    double rewritten_equity = kCapital;
    bool change_presented_tail = false;
    bool shrink_presented_curve = false;
    bool throw_in_present = false;
    std::vector<pf_equity_point_t> observations;
    std::size_t next = 0;
    mutable int presentations = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        if (enter_at >= 0 && !entered && context.script_bar_open_ms >= enter_at) {
            entered = true;
            (void)submit({no::Transact{1.0}, "entry", ""});
        }
        if (record_own && next < observations.size()) {
            equity_curve_.push_back(observations[next++]);
            // A last-point rewrite is judged at capture, after the edit.
            if (rewrite_first && next == observations.size())
                equity_curve_.front().equity = rewritten_equity;
        }
    }
    void present_report(ReportC* report) const override {
        ++presentations;
        if (throw_in_present) throw std::runtime_error("L17A presentation refused");
        if (change_presented_tail && report->equity_curve_len > 1) {
            report->equity_curve[report->equity_curve_len - 1].equity = 1023.5;
            report->equity_curve[report->equity_curve_len - 1].open_profit = -0.0;
        }
        if (shrink_presented_curve && report->equity_curve_len > 1)
            report->equity_curve_len = 1;
    }
};

NativeRunSpec spec(bool own = false, std::uint64_t generation = 1) {
    NativeRunSpec result;
    result.identity = {"cr150-l17a", generation};
    result.input_tf = "1";
    result.script_tf = "1";
    result.ticker = "MOCK";
    result.tickerid = "TEST:MOCK";
    result.type = "crypto";
    result.currency = "USD";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = kCapital;
    result.point_value = 1.0;
    result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = own ? NativeReportPolicy::HostRecorded
                              : NativeReportPolicy::KernelRecorded;
    return result;
}

std::vector<Bar> tape() {
    return {{50.0, 50.0, 50.0, 50.0, 1.0, kBase},
            {50.0, 50.0, 50.0, 50.0, 1.0, kBase + kMinute},
            {100.0, 100.0, 100.0, 100.0, 1.0, kBase + 2 * kMinute},
            {100.0, 110.0, 100.0, 110.0, 1.0, kBase + 3 * kMinute},
            {110.0, 110.0, 104.5, 104.5, 1.0, kBase + 4 * kMinute}};
}

pf_selected_window_config_v1 window(int start, int end = 10) {
    return {sizeof(pf_selected_window_config_v1), 1,
            kBase + start * kMinute, kBase + end * kMinute};
}

void install(Host& host, const std::string& path) {
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L17A", nullptr};
    CHECK(host.set_report_sink_v1(&sink) == 0);
}

void run(Host& host, const std::vector<Bar>& bars,
         const pf_selected_window_config_v1& selected, std::uint64_t generation = 1) {
    CHECK(host.configure_native(spec(host.record_own, generation)).status == NativeSetupStatus::Applied);
    CHECK(native_set_selected_window_v1(&host, &selected) == 0);
    host.run(bars.data(), static_cast<int>(bars.size()));
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
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> equity_lines(const std::string& bytes) {
    std::vector<std::string> result;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto end = bytes.find('\n', at);
        CHECK(end != std::string::npos);
        if (end == std::string::npos) break;
        if (bytes.compare(at, 5, "[\"E\",") == 0) result.push_back(bytes.substr(at, end - at));
        at = end + 1;
    }
    return result;
}

std::string equity_line(const pf_equity_point_t& point) {
    return "[\"E\"," + std::to_string(point.time_ms) + ','
        + report_sink::f64(point.equity) + ',' + report_sink::f64(point.open_profit) + ']';
}

void compare_selected(Directory& directory, const char* name, int start, bool trade,
                      bool edit = false, bool shrink = false) {
    Host baseline, streamed;
    baseline.enter_at = streamed.enter_at = trade ? kBase + 2 * kMinute : -1;
    baseline.change_presented_tail = streamed.change_presented_tail = edit;
    baseline.shrink_presented_curve = streamed.shrink_presented_curve = shrink;
    const auto path = directory.file(name);
    install(streamed, path);
    const auto selected = window(start);
    auto bars = tape();
    if (start == 2 && !trade && !edit && !shrink) bars.erase(bars.begin() + 2); // first window point after T
    run(baseline, bars, selected);
    run(streamed, bars, selected);
    CHECK(baseline.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(streamed.native_state().kind == NativeLifecycleKind::Completed);
    Report before, after;
    baseline.fill_report(&before.value);
    // FIRST RED on f00151f22: capture has not written any E record yet.
    CHECK(receipt(streamed).count_e == before.value.equity_curve_len);
    CHECK(receipt(streamed).complete == 0); // observer success still precedes final fill
    CHECK(streamed.presentations == 1);
    streamed.fill_report(&after.value);
    CHECK(after.value.equity_curve == nullptr && after.value.equity_curve_len == 0);
    CHECK(after.value.trades == nullptr && after.value.trades_len == 0);
    CHECK(before.value.total_trades == after.value.total_trades);
    CHECK(before.value.net_profit == after.value.net_profit);
    CHECK(std::memcmp(&before.value.metrics, &after.value.metrics, sizeof(pf_metrics_t)) == 0);
    CHECK(baseline.broker_state_hash() == streamed.broker_state_hash());
    const auto bytes = read(path);
    const auto lines = equity_lines(bytes);
    CHECK(lines.size() == static_cast<std::size_t>(before.value.equity_curve_len));
    for (std::size_t i = 0; i < lines.size()
         && i < static_cast<std::size_t>(before.value.equity_curve_len); ++i)
        CHECK(lines[i] == equity_line(before.value.equity_curve[i]));
    CHECK(!lines.empty());
    if (!lines.empty()) CHECK(lines.front() == equity_line({selected.start_ms, kCapital, 0.0}));
    if (start == 5) CHECK(lines.size() == 1); // M = 0: anchor only
    const auto got = receipt(streamed);
    CHECK(got.complete == 1);
    CHECK(got.count_e == before.value.equity_curve_len);
    CHECK(got.count_t == before.value.trades_len);
    const auto trailer = bytes.find("{\"trailer\":");
    CHECK(trailer != std::string::npos);
    if (trailer != std::string::npos) {
        report_sink::Sha256 hash;
        hash.update(std::string_view(bytes).substr(0, trailer));
        CHECK(hash.hex_digest() == got.sha256_hex);
    }
    Report again;
    streamed.fill_report(&again.value);
    CHECK(read(path) == bytes);
    CHECK(receipt(streamed).count_e == got.count_e);
    CHECK(streamed.presentations == 1);
    CHECK(std::memcmp(&after.value.metrics, &again.value.metrics, sizeof(pf_metrics_t)) == 0);
}

void refused(Directory& directory, const char* name, Host& host, int start,
             const char* message, int end = 10) {
    const auto path = directory.file(name);
    install(host, path);
    run(host, tape(), window(start, end));
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.last_error().find(message) != std::string::npos);
    CHECK(receipt(host).complete == 0);
    CHECK(read(path).find("{\"trailer\":") == std::string::npos);
    Report report;
    host.fill_report(&report.value);
    CHECK(report.value.equity_curve == nullptr && report.value.trades == nullptr);
    CHECK(receipt(host).complete == 0);
    CHECK(read(path).find("{\"trailer\":") == std::string::npos);
}

void latch_failures_and_final_values(Directory& directory) {
    Host account;
    account.record_own = true;
    account.observations = {{kBase, kCapital, 0.0}, {kBase + kMinute, kCapital, 0.0},
                            {kBase + 2 * kMinute, kCapital, 0.0},
                            {kBase + 3 * kMinute, kCapital, 0.0},
                            {kBase + 4 * kMinute, kCapital, 0.0}};
    account.rewrite_first = true;
    account.rewritten_equity = kCapital + 1.0;
    refused(directory, "account.ndjson", account, 2, "the account changed before the window start");
    CHECK(account.presentations == 0);

    Host repaired;
    repaired.record_own = true;
    repaired.observations = {{kBase, kCapital + 1.0, 0.0}, {kBase + kMinute, kCapital, -0.0},
                             {kBase + 2 * kMinute, kCapital, 0.0},
                             {kBase + 3 * kMinute, kCapital, 0.0},
                             {kBase + 4 * kMinute, kCapital, 0.0}};
    repaired.rewrite_first = true;
    const auto repair_path = directory.file("repaired.ndjson");
    install(repaired, repair_path);
    run(repaired, tape(), window(2));
    CHECK(repaired.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(receipt(repaired).count_e == 4);
    Report repaired_report;
    repaired.fill_report(&repaired_report.value);
    CHECK(receipt(repaired).complete == 1);

    Host contiguity;
    contiguity.record_own = true;
    contiguity.observations = {{kBase, kCapital, 0.0},
                               {kBase + 2 * kMinute, kCapital, 0.0},
                               {kBase + 10 * kMinute, kCapital, 0.0},
                               {kBase + 3 * kMinute, kCapital, 0.0},
                               {kBase + 4 * kMinute, kCapital, 0.0}};
    refused(directory, "contiguity.ndjson", contiguity, 2, "observations are not one run");

    Host count;
    count.record_own = true;
    refused(directory, "count.ndjson", count, 2, "observations differ from its script bars");

    // All lower-priority point failures coexist with a real carry-in trade.
    Host carry;
    carry.enter_at = kBase;
    carry.record_own = true;
    carry.observations = {{kBase, kCapital + 1.0, 0.0}};
    refused(directory, "trade-first.ndjson", carry, 3, "a trade was entered before the window start");
    CHECK(carry.presentations == 0);

    Host presentation;
    presentation.throw_in_present = true;
    refused(directory, "presentation.ndjson", presentation, 2, "L17A presentation refused");
    CHECK(presentation.presentations == 1);
}

int reject_observer(void*, const pf_execution_boundary_v1*, pf_boundary_receipt_v1*) { return 1; }

void failed_observer_and_reuse(Directory& directory) {
    Host host;
    const auto path = directory.file("reuse.ndjson");
    install(host, path);
    const pf_execution_observer_v1 observer{sizeof observer, 1, nullptr, reject_observer};
    CHECK(host.configure_native(spec()).status == NativeSetupStatus::Applied);
    CHECK(native_set_execution_observer_v1(&host, &observer) == 0);
    CHECK(native_set_selected_window_v1(&host, nullptr) == 0);
    // Admit with a selected window after the observer, as the host harness does.
    const auto first = window(2);
    CHECK(native_set_selected_window_v1(&host, &first) == 0);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(receipt(host).complete == 0);
    CHECK(read(path).find("{\"trailer\":") == std::string::npos);
    CHECK(native_set_execution_observer_v1(&host, nullptr) == 0);
    run(host, bars, window(5), 2);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(receipt(host).count_e == 1);
    Report report;
    host.fill_report(&report.value);
    CHECK(receipt(host).complete == 1 && receipt(host).count_e == 1);
    CHECK(equity_lines(read(path)).size() == 1);
}
}

int main() {
    try {
        Directory directory;
        compare_selected(directory, "flat.ndjson", 2, false);
        compare_selected(directory, "open.ndjson", 2, true);
        compare_selected(directory, "empty.ndjson", 5, false);
        compare_selected(directory, "presented.ndjson", 2, true, true);
        compare_selected(directory, "shrunk.ndjson", 2, true, false, true);
        latch_failures_and_final_values(directory);
        failed_observer_and_reuse(directory);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "L17A exception: %s\n", error.what());
    }
    std::printf("test_report_sink_selected_l17a: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
