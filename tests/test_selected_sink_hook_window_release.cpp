// 1.7.0 review B1-10. A selected-sink hook borrows exactly the two editable
// presentation points; it may shrink the window but cannot grow it, make its
// length negative, or replace its storage. These checks must survive Release.
//
// The unit-test helper strips NDEBUG from this TU, but the linked engine is
// still compiled with the selected build profile. Assertions here use require
// regardless. On the starting Release library the window guard is absent:
// the invalid hooks are not refused at the presentation contract boundary.
#include "p2cx_kernel_consumer_fixture.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>

#include <array>
#include <cstdint>

// The same native bridges used by test_selected_metrics_l17b, with their
// definitions in native_execution_consumer.cpp and no strategy wrapper.
namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_selected_window_counts_v1(const BacktestEngine*, pf_selected_window_counts_v1*) noexcept;
}
}

namespace {

using namespace pineforge;
using p2cx_fixture::require;

constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kWindowPoints = 2; // PresentationCaptureWindow's contract.
constexpr std::int64_t kFullCurvePoints = 3; // Two selected observations + anchor.
constexpr const char* kRefusal =
    "native selected report: presentation exceeded its tail window";

enum class Hook { Grow, Negative, Replace, Keep, Shrink };

const char* name(Hook hook) {
    switch (hook) {
    case Hook::Grow: return "grow";
    case Hook::Negative: return "negative";
    case Hook::Replace: return "replace";
    case Hook::Keep: return "keep";
    case Hook::Shrink: return "shrink";
    }
    throw std::logic_error("unknown fixture hook");
}

class WindowHost final : public NativeStrategyHost {
public:
    explicit WindowHost(Hook hook) : hook_(hook) {}

    int calculations = 0;
    mutable int presentations = 0;
    mutable std::int64_t length_at_hook = -1;
    mutable bool borrowed_curve = false;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++calculations; }

    void present_report(ReportC* out) const override {
        ++presentations;
        length_at_hook = out->equity_curve_len;
        borrowed_curve = out->equity_curve != nullptr;
        switch (hook_) {
        case Hook::Grow:
            out->equity_curve_len += 3; // No out-of-bounds write in the test hook.
            break;
        case Hook::Negative:
            out->equity_curve_len = -1;
            break;
        case Hook::Replace:
            // Host-owned storage: refusal must not delete a replacement that
            // the engine never owned, nor silently publish the original tail.
            out->equity_curve = replacement_.data();
            break;
        case Hook::Keep:
            break;
        case Hook::Shrink:
            --out->equity_curve_len;
            break;
        }
    }

private:
    Hook hook_;
    mutable std::array<pf_equity_point_t, kWindowPoints> replacement_{};
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

NativeRunSpec spec() {
    NativeRunSpec result;
    result.identity = {"b1-10-selected-window-hook", 1};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:B110";
    result.timezone = "UTC";
    result.session = "24x7";
    result.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    result.initial_capital = 1000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

void run(Hook hook) {
    std::fprintf(stderr, "B1-10 hook: %s\n", name(hook));
    p2cx_fixture::Directory directory("B1-10");
    WindowHost host(hook);
    require(host.configure_native(spec()).status == NativeSetupStatus::Applied,
            "configure selected-window fixture");
    const auto output = directory.child("run.ndjson");
    const pf_report_sink_v1 sink{
        sizeof sink, 1, output.c_str(), "B1-10", directory.path.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "install selected-window path sink");
    const pf_selected_window_config_v1 window{
        sizeof window, 1, kStart + kMinute, kStart + 3 * kMinute};
    require(native_set_selected_window_v1(&host, &window) == 0,
            "admit selected-window configuration for a custom hook");
    const Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 1.0, kStart},
        {100.0, 100.0, 100.0, 100.0, 1.0, kStart + kMinute},
        {100.0, 100.0, 100.0, 100.0, 1.0, kStart + 2 * kMinute},
    };
    host.run(bars, 3);
    require(host.calculations == 3, "the fixture must finish all bars before capture");
    require(host.presentations == 1 && host.borrowed_curve,
            "the selected sink must reach the custom presentation hook exactly once");
    require(host.length_at_hook == kWindowPoints, "the hook must receive the two-point window");

    const bool valid = hook == Hook::Keep || hook == Hook::Shrink;
    if (!valid) {
        require(host.last_error().find(kRefusal) != std::string::npos,
                "the invalid hook must be refused at the window contract boundary in Release");
        require(host.native_state().kind == NativeLifecycleKind::Failed,
                "an invalid borrowed-window hook must fail its capture");
        require(run_failure_value_of(host).code == RunFailureCode::engine_invariant,
                "window contract refusal must carry typed engine_invariant");
    } else {
        require(host.native_state().kind == NativeLifecycleKind::Completed,
                "an unchanged or shrinking borrowed window remains legal");
        require(host.last_error().empty()
                    && run_failure_value_of(host).code == RunFailureCode::none,
                "a legal borrowed-window hook must not record a failure");
    }

    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    const int count_status = native_selected_window_counts_v1(&host, &counts);
    require(count_status == (valid ? 0 : -2),
            "only a successful capture may expose selected results");
    if (valid)
        require(counts.window_script_bars == 2, "the fixture selects exactly two script bars");

    Report report;
    host.fill_report(&report.value); // Also attempts the normal sink trailer publication.
    require(host.presentations == 1, "reading a sealed capture must not repeat the hook");
    if (!valid)
        require(report.value.equity_curve == nullptr && report.value.equity_curve_len == 0
                    && report.value.trades == nullptr && report.value.trades_len == 0,
                "failed capture must expose no report arrays");
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    require(host.report_sink_receipt_v1(&receipt) == 0, "read sink receipt");
    require(receipt.complete == (valid ? 1U : 0U),
            "a refused hook must never publish a successful trailer");
    if (valid)
        require(receipt.count_e == kFullCurvePoints - (hook == Hook::Shrink ? 1 : 0),
                "legal hooks publish the full curve or exactly the requested shrink");
    else
        require(receipt.count_e == 0, "refusal must happen before publishing any selected equity");
}

} // namespace

int main() {
    try {
        run(Hook::Grow);
        run(Hook::Negative);
        run(Hook::Replace);
        run(Hook::Keep);
        run(Hook::Shrink);
        std::puts("test_selected_sink_hook_window_release: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_selected_sink_hook_window_release: FAIL: %s\n", error.what());
        return 1;
    }
}
