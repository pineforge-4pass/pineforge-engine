#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/version.h>
#include "sha256.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

struct Stub {
    std::string mode;
    std::string sink_path;
    std::string run_id;
    int sink_fd = -1;
    bool failed = false;
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
    const auto stream = header(state) + "\n";
    pineforge::report_sink::Sha256 digest;
    digest.update(stream);
    const auto trailer = std::string(
        "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":0,\"T\":0},"
        "\"sha256\":\"") + digest.hex_digest() + "\"}\n";
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
    if (const char* mode = std::getenv("PINEFORGE_STDOUT_TEST_MODE")) state->mode = mode;
    return state;
}

PF_API void strategy_free(pf_strategy_t strategy) {
    delete stub(strategy);
}

PF_API void strategy_set_input(pf_strategy_t strategy, const char* key, const char* value) {
    if (key && std::string(key) == "mode") stub(strategy)->mode = value ? value : "";
}

PF_API void strategy_set_override(pf_strategy_t, const char*, const char*) {}

PF_API void run_backtest_full(pf_strategy_t strategy, pf_bar_t*, int count,
                              const char*, const char*, int, int,
                              pf_magnifier_distribution_t, pf_report_t* report) {
    auto& state = *stub(strategy);
    state.failed = false;
    state.receipt = {};
    state.summary = {};
    state.diagnostics = {};
    *report = {};
    report->input_bars_processed = count;
    report->script_bars_processed = count;
    report->input_tf_seconds = 60;
    report->script_tf_seconds = 60;
    report->script_tf_ratio = 1;
    if (state.mode == "print" || state.mode == "print-fail") {
        static constexpr char marker[] = "NATIVE_STDOUT_MARKER\n";
        write_all(STDOUT_FILENO, marker);
    }
    if (state.mode == "print-fail") {
        state.failed = true;
        return;
    }
    write_sink(state);
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
