// KERNEL-EDGE: four audit defects, each driven by a bare NativeStrategyHost.
// The copied audit probes and their before/after output live outside the tree.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int failures = 0;

void check(bool ok, const char* row) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", row);
    if (!ok) ++failures;
}

NativeRunSpec base_spec(const char* id, const char* input, const char* script,
                        const char* session = "24x7", const char* zone = "UTC") {
    NativeRunSpec spec;
    spec.identity = {id, 1};
    spec.input_tf = input;
    spec.script_tf = script;
    spec.ticker = "EDGE";
    spec.tickerid = "TEST:EDGE";
    spec.type = "stock";
    spec.currency = "USD";
    spec.timezone = zone;
    spec.session = session;
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    return spec;
}

struct RollHost final : NativeStrategyHost {
    bool sync = false;
    int bars = 0;
    mutable int rolls = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) {
            no::Request limit{no::Transact{1.0}, "limit", ""};
            limit.trigger = no::Limit{99.5};
            (void)submit(limit);
        }
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& context,
                               NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        if (reason == NativeCalculationReason::BarClose) {
            on_native_bar(bar, context);
        } else if (sync && cause && cause->request().label == "limit") {
            const auto accepted = submit({no::Transact{1.0}, "sync", ""});
            if (accepted.handle) (void)execute_current({*accepted.handle});
        }
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        if (point.kind == NativeMarginCheckKind::FxRoll) ++rolls;
        return true;
    }
};

void fx_roll_witness() {
    constexpr std::int64_t start = 1704067200000LL;
    constexpr std::int64_t minute = 60'000;
    const std::vector<Bar> bars = {
        {100, 100.2, 99.9, 100, 1, start},
        {100, 100.2, 99.9, 100, 1, start + minute},
        {100, 100.2, 99, 99.8, 1, start + 2 * minute},
        {99.8, 100, 99.7, 99.9, 1, start + 3 * minute},
        {99.9, 100, 99.8, 99.9, 1, start + 4 * minute},
    };
    auto spec = base_spec("edge-fx", "1", "1");
    spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    NativeMarginModel margin;
    margin.initial_long = 0;
    margin.maintenance_long = 0.1;
    margin.initial_short = 0.5;
    spec.margin = margin;
    for (bool sync : {false, true}) {
        RollHost host;
        host.sync = sync;
        check(host.configure_native(spec).status == NativeSetupStatus::Applied,
              "N1 configure");
        check(host.configure_native_fx_curve(
                  NativeFxCurve{{start + 3 * minute}, {2.0}}).status
                  == NativeSetupStatus::Applied,
              "N1 curve");
        host.run(bars.data(), static_cast<int>(bars.size()));
        check(host.native_state().kind == NativeLifecycleKind::Completed,
              "N1 completed");
        check(host.rolls == 1, sync ? "N1 one roll with execute_current"
                                    : "N1 one roll without execute_current");
    }
}

struct SessionFacts {
    bool seen = false;
    bool opens = false;
    bool closes = false;
};

struct SessionHost final : NativeStrategyHost {
    std::int64_t pre_label = 1704111300000LL;
    std::int64_t reopen_label = 1704115800000LL;
    SessionFacts pre_break;
    SessionFacts reopen;
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        auto* row = context.script_bar_open_ms == pre_label ? &pre_break
                  : context.script_bar_open_ms == reopen_label ? &reopen : nullptr;
        if (row) *row = {true, context.opens_session_day, context.closes_session_day};
    }
};

void session_witness() {
    constexpr std::int64_t day = 1704067200000LL; // 2024-01-01 00:00 UTC.
    std::vector<Bar> bars;
    for (int minute = 8 * 60; minute < 22 * 60; minute += 15) {
        if (minute >= 12 * 60 + 30 && minute < 13 * 60 + 30) continue;
        bars.push_back({100, 101, 99, 100, 1, day + minute * 60'000LL});
    }
    auto spec = base_spec("edge-session", "15", "15", "0800-1230,1330-2200");
    SessionHost batch;
    check(batch.configure_native(spec).status == NativeSetupStatus::Applied,
          "N2 batch configure");
    batch.run(bars.data(), static_cast<int>(bars.size()));
    check(batch.pre_break.seen && !batch.pre_break.closes
              && batch.reopen.seen && !batch.reopen.opens,
          "N2 batch lunch is one session day");
    SessionHost stream;
    check(stream.configure_native(spec).status == NativeSetupStatus::Applied,
          "N2 stream configure");
    constexpr int warmup = (12 * 60 + 30 - 8 * 60) / 15;
    bool ok = stream.stream_begin(bars.data(), warmup, "15", "15");
    for (std::size_t i = warmup; ok && i < bars.size(); ++i) ok = stream.stream_push_bar(bars[i]);
    if (ok) ok = stream.stream_end(false);
    check(ok, "N2 stream completed");
    check(stream.pre_break.seen && !stream.pre_break.closes
              && stream.reopen.seen && !stream.reopen.opens,
          "N2 stream lunch is one session day");

    // The reopened hourly bucket is labelled 13:00 although its first
    // eligible input is 13:30; both driving modes must read that same day.
    spec.identity = {"edge-session-hour", 1};
    spec.script_tf = "60";
    SessionHost hourly_batch;
    hourly_batch.pre_label = day + 12 * 60 * 60'000LL;
    hourly_batch.reopen_label = day + 13 * 60 * 60'000LL;
    check(hourly_batch.configure_native(spec).status == NativeSetupStatus::Applied,
          "N2 hourly batch configure");
    hourly_batch.run(bars.data(), static_cast<int>(bars.size()));
    check(hourly_batch.pre_break.seen && !hourly_batch.pre_break.closes
              && hourly_batch.reopen.seen && !hourly_batch.reopen.opens,
          "N2 hourly batch lunch is one session day");
    SessionHost hourly_stream;
    hourly_stream.pre_label = hourly_batch.pre_label;
    hourly_stream.reopen_label = hourly_batch.reopen_label;
    check(hourly_stream.configure_native(spec).status == NativeSetupStatus::Applied,
          "N2 hourly stream configure");
    ok = hourly_stream.stream_begin(bars.data(), warmup, "15", "60");
    for (std::size_t i = warmup; ok && i < bars.size(); ++i)
        ok = hourly_stream.stream_push_bar(bars[i]);
    if (ok) ok = hourly_stream.stream_end(false);
    check(ok, "N2 hourly stream completed");
    check(hourly_stream.pre_break.seen && !hourly_stream.pre_break.closes
              && hourly_stream.reopen.seen && !hourly_stream.reopen.opens,
          "N2 hourly stream lunch is one session day");
}

struct CountHost final : NativeStrategyHost {
    int calculations = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++calculations;
    }
};

void final_bucket_witness() {
    constexpr std::int64_t first = 1751376600000LL; // NY 2025-07-01 09:30.
    constexpr std::int64_t quarter = 15 * 60'000;
    std::vector<Bar> one_day;
    for (int i = 0; i < 26; ++i)
        one_day.push_back({100, 101, 99, 100, 1, first + i * quarter});
    auto spec = base_spec("edge-final", "15", "60", "0930-1600", "America/New_York");
    CountHost batch;
    check(batch.configure_native(spec).status == NativeSetupStatus::Applied,
          "B1 batch configure");
    batch.run(one_day.data(), static_cast<int>(one_day.size()));
    check(batch.calculations == 7, "B1 batch calculates final clipped hour");

    spec.identity = {"edge-final-stream", 1};
    spec.script_tf = "D";
    std::vector<Bar> three_days;
    for (int d = 0; d < 3; ++d)
        for (const auto& bar : one_day) {
            Bar next = bar;
            next.timestamp += d * 86'400'000LL;
            three_days.push_back(next);
        }
    for (bool finalize : {false, true}) {
        CountHost stream;
        check(stream.configure_native(spec).status == NativeSetupStatus::Applied,
              "B1 stream configure");
        bool ok = stream.stream_begin(three_days.data(), 52, "15", "D");
        for (int i = 52; ok && i < 78; ++i) ok = stream.stream_push_bar(three_days[i]);
        if (ok) ok = stream.stream_end(finalize);
        check(ok, "B1 stream completed");
        check(stream.calculations == (finalize ? 3 : 2),
              finalize ? "B1 stream_end(true) calculates final day"
                       : "B1 stream_end(false) leaves final day pending");
    }
}

template <typename Host, typename = void>
struct HasReportMark : std::false_type {};

template <typename Host>
struct HasReportMark<Host, std::void_t<decltype(
    std::declval<Host&>().mark_native_report_point(std::int64_t{}))>> : std::true_type {};

template <typename Host>
bool mark_if_available(Host& host, std::int64_t timestamp) {
    if constexpr (HasReportMark<Host>::value)
        return host.mark_native_report_point(timestamp);
    return false;
}

struct MarkHost final : NativeStrategyHost {
    int accepted_marks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        accepted_marks += mark_if_available(*this, context.script_bar_open_ms);
    }
};

void report_marks_witness() {
    constexpr std::int64_t first = 1718668800000LL;
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i)
        bars.push_back({10.0 + i, 11.0 + i, 9.0 + i, 10.5 + i, 1,
                        first + i * 60'000LL});
    auto spec = base_spec("edge-marks", "1", "1");
    spec.report_policy = NativeReportPolicy::KernelRecordedAtHostMarks;
    MarkHost host;
    check(host.configure_native(spec).status == NativeSetupStatus::Applied,
          "G1 host marks configure");
    host.run(bars.data(), static_cast<int>(bars.size()));
    ReportC report{};
    host.fill_report(&report);
    check(host.accepted_marks == 6 && report.equity_curve_len == 6,
          "G1 public host marks record six points");
    BacktestEngine::free_report(&report);
}

} // namespace

int main() {
    fx_roll_witness();
    session_witness();
    final_bucket_witness();
    report_marks_witness();
    std::printf("native_kernel_edge: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
