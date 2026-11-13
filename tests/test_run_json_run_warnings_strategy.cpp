#include <pineforge/pineforge.h>
#include <pineforge/execution_observer.h>
#include <pineforge/report_sink.h>
#include <pineforge/selected_window.h>
#include <pineforge/version.h>
#include "sha256.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Stub {
    std::string mode;
    std::string sink_path;
    std::string run_id;
    int sink_fd = -1;
    bool failed = false;
    bool completed = false;
    bool selected = false;
    pf_selected_window_config_v1 window{};
    pf_selected_window_counts_v1 counts{};
    pf_execution_observer_v1 observer{};
    pf_execution_observation_v1 observation{};
    std::vector<pf_equity_point_t> curve;
    pf_report_sink_receipt_v1 receipt{};
    pf_report_sink_summary_v1 summary{};
    pf_report_sink_diagnostics_v1 diagnostics{};
};

Stub* stub(pf_strategy_t strategy) {
    return static_cast<Stub*>(strategy);
}

void write_all(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        offset += static_cast<std::size_t>(written);
    }
}

std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (const char character : value) {
        if (character == '"' || character == '\\') result += '\\';
        result += character;
    }
    result += '"';
    return result;
}

// The sink reader accepts an f64 column only as the Python repr of its double
// (1000000.0, not 1000000.000000).
std::string repr_double(double value) {
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value,
                                         std::chars_format::fixed);
    std::string text(buffer, converted.ptr);
    if (text.find('.') == std::string::npos) text += ".0";
    return text;
}

std::string header(const Stub& state) {
    return "{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":"
        + quoted(state.run_id) + ",\"engine\":" + quoted(PINEFORGE_VERSION_STRING)
        + ",\"records\":[{\"type\":\"E\",\"fields\":[\"time_ms\",\"equity\","
          "\"open_profit\"],\"types\":[\"i64\",\"f64?\",\"f64?\"],\"units\":["
          "\"unix_ms\",\"account_ccy\",\"account_ccy\"]},{\"type\":\"T\",\"fields\":["
          "\"n\",\"side\",\"entry_time\",\"exit_time\",\"entry_price\",\"exit_price\","
          "\"qty\",\"pnl\",\"pnl_pct\",\"max_runup\",\"max_drawdown\",\"commission\","
          "\"entry_bar_index\",\"exit_bar_index\",\"open_at_end\",\"entry_incarnation\","
          "\"close_cause\",\"entry_id\",\"exit_id\",\"exit_comment\"],\"types\":[\"i64\","
          "\"str\",\"i64\",\"i64\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\","
          "\"f64?\",\"f64?\",\"i64\",\"i64\",\"bool\",\"u64\",\"i64\",\"str\",\"str\","
          "\"str\"],\"units\":[\"\",\"\",\"unix_ms\",\"unix_ms\",\"quote_ccy\",\"quote_ccy\","
          "\"qty\",\"account_ccy\",\"percent\",\"account_ccy\",\"account_ccy\","
          "\"account_ccy\",\"script_bar\",\"script_bar\",\"\",\"\",\"\",\"\",\"\",\"\"]}]}";
}

void write_sink(Stub& state) {
    auto stream = header(state) + "\n";
    for (const auto& point : state.curve) {
        stream += "[\"E\"," + std::to_string(point.time_ms) + ","
            + repr_double(point.equity) + ","
            + repr_double(point.open_profit) + "]\n";
    }
    pineforge::report_sink::Sha256 digest;
    digest.update(stream);
    const auto trailer = std::string(
        "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":")
        + std::to_string(state.curve.size()) + ",\"T\":0},\"sha256\":\""
        + digest.hex_digest() + "\"}\n";
    const int fd = state.sink_fd >= 0
        ? state.sink_fd
        : ::open(state.sink_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    write_all(fd, stream);
    write_all(fd, trailer);
    if (state.sink_fd < 0) ::close(fd);

    state.receipt = {};
    state.receipt.struct_size = sizeof(state.receipt);
    state.receipt.version = 1;
    state.receipt.count_e = static_cast<int64_t>(state.curve.size());
    std::memcpy(state.receipt.sha256_hex, digest.hex_digest().c_str(), 65);
    state.receipt.complete = 1;
    state.receipt.trades_retained = 1;
    state.summary = {};
    state.summary.struct_size = sizeof(state.summary);
    state.summary.version = 1;
    state.summary.complete = 1;
    state.diagnostics = {};
    state.diagnostics.struct_size = sizeof(state.diagnostics);
    state.diagnostics.version = 1;
    state.diagnostics.count_e = state.receipt.count_e;
    if (!state.curve.empty()) {
        state.diagnostics.first = state.curve.front();
        state.diagnostics.last = state.curve.back();
    }
    state.diagnostics.complete = 1;
}

}  // namespace

extern "C" {

PF_API int pf_abi_version(void) {
    return PF_ABI_VERSION;
}

PF_API pf_version_t pf_version_get(void) {
    return {PINEFORGE_VERSION_MAJOR, PINEFORGE_VERSION_MINOR,
            PINEFORGE_VERSION_PATCH, ""};
}

PF_API const char* pf_version_string(void) {
    return PINEFORGE_VERSION_STRING;
}

PF_API pf_strategy_t strategy_create(const char*) {
    auto* state = new Stub;
    if (const char* mode = std::getenv("PINEFORGE_RUN_WARNINGS_TEST_MODE"))
        state->mode = mode;
    return state;
}

PF_API void strategy_free(pf_strategy_t strategy) {
    delete stub(strategy);
}

PF_API void strategy_set_input(pf_strategy_t strategy, const char* key, const char* value) {
    if (key && std::string(key) == "mode") stub(strategy)->mode = value ? value : "";
}

PF_API void strategy_set_override(pf_strategy_t, const char*, const char*) {}

PF_API void run_backtest_full(pf_strategy_t strategy, pf_bar_t* bars, int count,
                              const char* input_tf, const char* script_tf, int, int,
                              pf_magnifier_distribution_t, pf_report_t* report) {
    auto& state = *stub(strategy);
    state.failed = false;
    state.completed = false;
    state.receipt = {};
    state.summary = {};
    state.diagnostics = {};
    state.curve.clear();
    *report = {};
    report->input_bars_processed = count;
    report->script_bars_processed = count;
    report->input_tf_seconds = std::strtol(input_tf, nullptr, 10) * 60;
    report->script_tf_seconds = std::strtol(script_tf, nullptr, 10) * 60;
    report->script_tf_ratio = 1;

    state.counts = {};
    state.counts.struct_size = sizeof(state.counts);
    state.counts.version = 1;
    state.counts.run_generation = ++state.observation.run_generation;
    state.counts.attempt_serial = ++state.observation.attempt_serial;
    state.counts.attempt_generation = state.counts.run_generation;
    state.observation.struct_size = sizeof(state.observation);
    state.observation.version = 1;
    state.observation.attempt_generation = state.counts.run_generation;
    state.observation.phase = 3;
    state.observation.fault_stage = 0;
    state.observation.attempt_outcome = 0;
    state.observation.boundary_delivered = 0;
    if (state.selected) {
        state.counts.fed_input_bars = state.counts.fed_script_bars = count;
        state.curve.push_back({state.window.start_ms, 1000000.0, 0.0});
        for (int i = 0; i < count; ++i) {
            if (bars[i].timestamp < state.window.start_ms) {
                ++state.counts.preroll_input_bars;
                ++state.counts.preroll_script_bars;
            } else if (bars[i].timestamp < state.window.end_ms) {
                ++state.counts.window_input_bars;
                ++state.counts.window_script_bars;
                state.curve.push_back({bars[i].timestamp, 1000000.0, 0.0});
            }
        }
    }
    if (state.observer.before_results) {
        pf_execution_boundary_v1 boundary{
            sizeof(boundary), 1, state.counts.run_generation, state.counts.attempt_serial};
        pf_boundary_receipt_v1 receipt{
            sizeof(receipt), 1, state.counts.run_generation, UINT32_MAX, 0, 0, 0};
        if (state.observer.before_results(state.observer.context, &boundary, &receipt) != 0) {
            state.failed = true;
            return;
        }
        state.observation.boundary_delivered = 1;
    }
    write_sink(state);
    if (state.sink_fd < 0 && state.sink_path.empty()) {
        report->equity_curve = state.curve.data();
        report->equity_curve_len = static_cast<int64_t>(state.curve.size());
    }
    state.observation.phase = 4;
    state.observation.attempt_outcome = 4;
    state.completed = true;
}

PF_API void report_free(pf_report_t*) {}

PF_API const char* strategy_get_last_error(pf_strategy_t strategy) {
    return stub(strategy)->failed ? "native stub failure" : "";
}

PF_API const char* strategy_get_last_error_code(pf_strategy_t strategy) {
    return stub(strategy)->failed ? "strategy_runtime_error" : "";
}

PF_API const char* strategy_get_last_error_args(pf_strategy_t) {
    return "{}";
}

PF_API int strategy_last_run_status(pf_strategy_t strategy) {
    return stub(strategy)->failed ? 1 : 0;
}

#ifndef PINEFORGE_TEST_OMIT_RUN_WARNINGS
PF_API const char* strategy_get_run_warnings(pf_strategy_t strategy) {
    if (!strategy) return nullptr;
    const auto& state = *stub(strategy);
    if (!state.completed) return "[]";
    if (state.mode == "unavailable")
        return R"([{"args":{},"code":"run_warnings_unavailable"}])";
    // Matches test_security_requested_timeframe_warning.cpp's single-site
    // canonical-text row and CANONICAL in test_run_json_run_warnings.py.
    if (state.mode == "warning")
        return R"([{"args":{"input_tf":"60","requested_tf":"75","sec_id":0},"code":"request_timeframe_not_multiple"}])";
    return "[]";
}
#endif

PF_API uint32_t pf_selected_window_version(void) {
    return 1;
}

PF_API int strategy_set_selected_window_v1(
    pf_strategy_t strategy, const pf_selected_window_config_v1* config) {
    auto& state = *stub(strategy);
    if (config && (config->struct_size != sizeof(*config) || config->version != 1
                   || config->start_ms >= config->end_ms))
        return -1;
    state.selected = config != nullptr;
    state.window = config ? *config : pf_selected_window_config_v1{};
    return 0;
}

PF_API int strategy_selected_window_counts_v1(
    pf_strategy_t strategy, pf_selected_window_counts_v1* counts) {
    if (!counts || counts->struct_size != sizeof(*counts) || counts->version != 1)
        return -1;
    if (!stub(strategy)->selected || !stub(strategy)->completed) return -2;
    *counts = stub(strategy)->counts;
    return 0;
}

PF_API uint32_t pf_execution_observer_version(void) {
    return 1;
}

PF_API int strategy_set_execution_observer_v1(
    pf_strategy_t strategy, const pf_execution_observer_v1* observer) {
    if (observer && (observer->struct_size != sizeof(*observer) || observer->version != 1
                     || !observer->before_results))
        return -1;
    stub(strategy)->observer = observer ? *observer : pf_execution_observer_v1{};
    return 0;
}

PF_API int strategy_execution_observation_v1(
    pf_strategy_t strategy, pf_execution_observation_v1* observation) {
    if (!observation || observation->struct_size != sizeof(*observation)
            || observation->version != 1)
        return -1;
    *observation = stub(strategy)->observation;
    return 0;
}

PF_API uint32_t pf_report_sink_version(void) {
    return 1;
}

PF_API int strategy_set_report_sink_v1(pf_strategy_t strategy,
                                       const pf_report_sink_v1* descriptor) {
    auto& state = *stub(strategy);
    state.sink_fd = -1;
    state.sink_path.clear();
    state.run_id.clear();
    if (descriptor) {
        state.sink_path = descriptor->path ? descriptor->path : "";
        state.run_id = descriptor->run_id ? descriptor->run_id : "";
    }
    return 0;
}

PF_API int strategy_set_report_sink_fd_v1(pf_strategy_t strategy,
                                          const pf_report_sink_fd_v1* descriptor) {
    auto& state = *stub(strategy);
    state.sink_fd = -1;
    state.sink_path.clear();
    state.run_id.clear();
    if (descriptor) {
        state.sink_fd = descriptor->fd;
        state.run_id = descriptor->run_id ? descriptor->run_id : "";
    }
    return 0;
}

PF_API int strategy_report_sink_receipt_v1(
    pf_strategy_t strategy, pf_report_sink_receipt_v1* receipt) {
    if (!receipt || receipt->struct_size != sizeof(*receipt) || receipt->version != 1)
        return -1;
    *receipt = stub(strategy)->receipt;
    receipt->struct_size = sizeof(*receipt);
    receipt->version = 1;
    return 0;
}

PF_API int strategy_report_sink_summary_v1(
    pf_strategy_t strategy, pf_report_sink_summary_v1* summary) {
    if (!summary || summary->struct_size != sizeof(*summary) || summary->version != 1)
        return -1;
    *summary = stub(strategy)->summary;
    summary->struct_size = sizeof(*summary);
    summary->version = 1;
    return 0;
}

PF_API int strategy_report_sink_diagnostics_v1(
    pf_strategy_t strategy, pf_report_sink_diagnostics_v1* diagnostics) {
    if (!diagnostics || diagnostics->struct_size != sizeof(*diagnostics)
            || diagnostics->version != 1)
        return -1;
    *diagnostics = stub(strategy)->diagnostics;
    diagnostics->struct_size = sizeof(*diagnostics);
    diagnostics->version = 1;
    return 0;
}

}  // extern "C"
