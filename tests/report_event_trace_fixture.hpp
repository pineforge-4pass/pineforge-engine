#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace event_trace_fixture {
using namespace pineforge;
inline constexpr std::int64_t start_ms = INT64_C(1704067200000);
inline constexpr std::int64_t step_ms = 60000;

inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

enum class Route { Full, Summary, OptionalSummary, Sink };

struct Directory {
    std::string path;
    Directory() {
        const char* temporary = std::getenv("TMPDIR");
        path = std::string(temporary && *temporary ? temporary : "/tmp")
            + "/pineforge-event-trace-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "create event/trace directory");
    }
    ~Directory() { std::filesystem::remove_all(path); }
};

inline NativeRunSpec spec(std::uint64_t generation = 1) {
    NativeRunSpec result;
    result.identity = {"event-trace", generation};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:EVENT_TRACE";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 10000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.25;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

inline void prime_identity_intern_pool() {
    const auto warmup = spec();
    (void)warmup;
}

inline std::vector<Bar> feed(int count) {
    require(count > 0, "event/trace feed must be nonempty");
    std::vector<Bar> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        result.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                          start_ms + index * step_ms});
    return result;
}

inline std::string name(int index) {
    return std::string("trace/\"\n\0", 9) + std::to_string(index) + std::string(48, 'n');
}

inline std::string message(int index) {
    return std::string("event/\"\\\n\0", 10) + std::to_string(index) + std::string(160, 'm');
}

inline double value(int index) {
    return index % 3 == 0 ? -0.0
        : index % 3 == 1 ? std::numeric_limits<double>::quiet_NaN() : 1.25;
}

class Host : public NativeStrategyHost {
public:
    int calculations = 0, events = 0, traces = 0;
    bool enabled = true;
    bool recalculate = false;
    bool clear_events = false;
    bool recalculate_after_clear = false;
    void (*observe)(Host&) = nullptr;

    Host() { declare_outputs(1, 1, 1); }
    void on_native_run_begin() override {
        calculations = events = traces = 0;
        output_run_begin();
        output_constant(0, -0.0);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        const int index = calculations++;
        const auto open = context.script_bar_open_ms;
        output_bar(open, open + step_ms);
        if (recalculate) {
            output_value(0, -999.0);
            output_event(0, -999.0, "retracted");
            output_bar(open, index % 2 == 0 ? INT64_MIN : open + step_ms + 1);
        }
        output_value(0, value(index));
        if (index % 3 == 0) output_event(0, value(index));
        else output_event(0, value(index), index % 3 == 1 ? "" : message(index));
        ++events;
        if (clear_events) {
            output_events_clear();
            if (recalculate_after_clear) output_bar(open, open + step_ms);
        }
        trace(name(index), value(index));
        ++traces;
        if (observe) observe(*this);
        (void)bar;
    }
    std::size_t trace_rows() const { return trace_buffer_.size(); }
    std::size_t trace_labels() const { return trace_names_.size(); }
    std::size_t trace_capacity_bytes() const {
        return trace_buffer_.capacity() * sizeof(TraceEntryC)
            + trace_names_.capacity() * sizeof(std::string)
            + trace_name_index_.bucket_count() * sizeof(void*);
    }
};

inline void configure(Host& host, Route route, bool enabled, const std::string& path,
                      const std::string& directory, std::uint64_t generation = 1) {
    require(host.configure_native(spec(generation)).status == NativeSetupStatus::Applied,
            "configure event/trace host");
    require(host.set_optional_report_mode(route == Route::OptionalSummary
                ? report_sequence::ReportMode::SummaryOnly
                : report_sequence::ReportMode::LegacyArrays) == 0,
            "configure optional recording");
    host.enabled = enabled;
    require(host.set_outputs_enabled(enabled), "configure event recording");
    host.set_trace_enabled(enabled);
    if (route == Route::Sink) {
        const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "event-trace",
                                    directory.c_str()};
        require(host.set_report_sink_v1(&sink) == 0, "install event/trace sink");
    }
    const auto mode = route == Route::Summary ? ReportMode::SUMMARY_ONLY
        : route == Route::Sink ? ReportMode::STREAMED : ReportMode::FULL;
    require(host.set_report_mode_v1(mode) == PF_REPORT_MODE_ACCEPTED,
            "configure event/trace report mode");
}

struct Envelope {
    ReportEnvelopeV1 value{};
    Envelope() {
        value.metadata.struct_size = sizeof value.metadata;
        value.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    }
    ~Envelope() { BacktestEngine::free_report(&value.report); }
};

inline void run(Host& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "event/trace fixture run failed");
    require(host.calculations == static_cast<int>(bars.size())
                && host.events == static_cast<int>(bars.size())
                && host.traces == static_cast<int>(bars.size()),
            "event/trace fixture did not execute its full nonzero population");
}

inline void capture(Host& host, Envelope& result, int count, bool enabled,
                    int expected_events = -1) {
    require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
            "read event/trace envelope");
    const auto& metadata = result.value.metadata;
    require(metadata.complete == 1 && result.value.report.input_bars_processed == count
                && result.value.report.script_bars_processed == count,
            "event/trace report missed input bars");
    require(metadata.output_rows.logical_count == (enabled ? count : 0)
                && metadata.output_events.logical_count
                    == (enabled ? (expected_events < 0 ? count : expected_events) : 0)
                && metadata.trace.logical_count == (enabled ? count : 0),
            "event/trace logical population changed");
    require(result.value.report.total_trades == 0 && result.value.report.net_profit == 0.0,
            "flat event/trace script unexpectedly traded");
}

inline pf_report_sink_receipt_v1 receipt(const Host& host) {
    pf_report_sink_receipt_v1 result{sizeof result, 1};
    require(host.report_sink_receipt_v1(&result) == 0, "read event/trace receipt");
    return result;
}

inline std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open event/trace artifact");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace event_trace_fixture
