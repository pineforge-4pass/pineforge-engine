// CR-150 L6B1: the real, whole-archive strategy module below is built from
// this same file. On f397d88d linking/loading fails on the missing sink ABI:
// P00 declares it but supplies no runtime definitions. No fake runtime/tape.
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/execution_observer.h>
#include <pineforge/selected_window.h>

#ifdef PINEFORGE_L6B1_MODULE
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/run_failure.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
class SinkFixture final : public pineforge::source::PineStrategyHost {
public:
    SinkFixture() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        config.commission_value = 0.125;
        configure_pine_strategy(config);
    }
    std::string mode;
    int busy_result = 99;
    void on_source_bar(const pineforge::Bar&) override {
        if (bar_index_ == 0) busy_result = strategy_set_report_sink_v1(this, nullptr);
        if (bar_index_ == 2 && mode == "throw") throw std::runtime_error("fixture failed");
        if (bar_index_ == 2 && mode == "abort") request_abort();
        if (mode != "empty" && (bar_index_ == 0 || bar_index_ == 2 || bar_index_ == 4))
            strategy_entry(bar_index_ == 2 ? "S\\\"\n" : "L", bar_index_ != 2,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), 1.0, "entry");
    }
};
SinkFixture* fixture(pf_strategy_t s) {
    return static_cast<SinkFixture*>(static_cast<pineforge::BacktestEngine*>(s));
}
std::vector<pineforge::Bar> input(const pf_bar_t* bars, int n) {
    std::vector<pineforge::Bar> result;
    for (int i = 0; i < n; ++i)
        result.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                          bars[i].volume, bars[i].timestamp});
    return result;
}
}
extern "C" {
pf_strategy_t strategy_create(const char*) { return new SinkFixture; }
void strategy_free(pf_strategy_t s) { delete fixture(s); }
void strategy_set_input(pf_strategy_t s, const char*, const char* value) {
    fixture(s)->mode = value ? value : "";
}
void strategy_set_override(pf_strategy_t, const char*, const char*) {}
void run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n, const char* input_tf,
                       const char* script_tf, int magnifier, int samples,
                       pf_magnifier_distribution_t distribution, pf_report_t* out) {
    *out = {};
    try {
        const auto values = input(bars, n);
        fixture(s)->run(values.data(), n, input_tf, script_tf, magnifier != 0, samples,
                        static_cast<pineforge::MagnifierDistribution>(distribution));
        fixture(s)->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
    } catch (const std::exception& error) {
        pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(out));
        *out = {};
        pineforge::note_run_failure(*fixture(s), "run_backtest_full", error);
    }
}
void run_backtest(pf_strategy_t s, pf_bar_t* bars, int n, pf_report_t* out) {
    run_backtest_full(s, bars, n, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}
void report_free(pf_report_t* report) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
}
int l6b1_busy_result(pf_strategy_t s) { return fixture(s)->busy_result; }
void l6b1_fill_again(pf_strategy_t s, pf_report_t* out) {
    fixture(s)->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
}
}

#else
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/version.h>
#include "../src/sha256.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void* open_module() {
    void* module = dlopen(PINEFORGE_L6B1_MODULE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!module) throw std::runtime_error(dlerror());
    return module;
}
template <class T> T symbol(void* module, const char* name) {
    const auto address = dlsym(module, name);
    if (!address) throw std::runtime_error(std::string("missing runtime export: ") + name);
    return reinterpret_cast<T>(address);
}
struct Api {
    void* module = open_module();
    // First red: P00 provides the declaration, L6B1 must supply the symbol.
    decltype(&pf_report_sink_version) version = symbol<decltype(version)>(module, "pf_report_sink_version");
    decltype(&strategy_set_report_sink_v1) set = symbol<decltype(set)>(module, "strategy_set_report_sink_v1");
    decltype(&strategy_report_sink_receipt_v1) receipt = symbol<decltype(receipt)>(module, "strategy_report_sink_receipt_v1");
    decltype(&strategy_create) create = symbol<decltype(create)>(module, "strategy_create");
    decltype(&strategy_free) destroy = symbol<decltype(destroy)>(module, "strategy_free");
    decltype(&run_backtest) run = symbol<decltype(run)>(module, "run_backtest");
    decltype(&report_free) free = symbol<decltype(free)>(module, "report_free");
    decltype(&strategy_set_input) mode = symbol<decltype(mode)>(module, "strategy_set_input");
    decltype(&strategy_get_last_error) error = symbol<decltype(error)>(module, "strategy_get_last_error");
    decltype(&strategy_get_last_error_code) code = symbol<decltype(code)>(module, "strategy_get_last_error_code");
    decltype(&strategy_last_run_status) status = symbol<decltype(status)>(module, "strategy_last_run_status");
    decltype(&strategy_broker_state_hash) hash = symbol<decltype(hash)>(module, "strategy_broker_state_hash");
    decltype(&strategy_closed_trade_entry_incarnation) incarnation = symbol<decltype(incarnation)>(module, "strategy_closed_trade_entry_incarnation");
    decltype(&strategy_closed_trade_close_cause) cause = symbol<decltype(cause)>(module, "strategy_closed_trade_close_cause");
    decltype(&strategy_closed_trade_entry_id) entry = symbol<decltype(entry)>(module, "strategy_closed_trade_entry_id");
    decltype(&strategy_closed_trade_exit_id) exit = symbol<decltype(exit)>(module, "strategy_closed_trade_exit_id");
    decltype(&strategy_closed_trade_exit_comment) comment = symbol<decltype(comment)>(module, "strategy_closed_trade_exit_comment");
    decltype(&strategy_set_selected_window_v1) selected = symbol<decltype(selected)>(module, "strategy_set_selected_window_v1");
    decltype(&strategy_set_execution_observer_v1) observer = symbol<decltype(observer)>(module, "strategy_set_execution_observer_v1");
    decltype(&strategy_stream_begin) stream = symbol<decltype(stream)>(module, "strategy_stream_begin");
    decltype(&strategy_stream_end) end = symbol<decltype(end)>(module, "strategy_stream_end");
    int (*busy)(pf_strategy_t) = symbol<decltype(busy)>(module, "l6b1_busy_result");
    void (*again)(pf_strategy_t, pf_report_t*) = symbol<decltype(again)>(module, "l6b1_fill_again");
};
std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::vector<pf_bar_t> feed() {
    std::vector<pf_bar_t> bars;
    for (int i = 0; i < 8; ++i) {
        const double price = 100.0 + i * 0.25;
        bars.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                        1.0, INT64_C(1700000040000) + i * INT64_C(60000)});
    }
    return bars;
}
pf_report_sink_receipt_v1 receipt(const Api& api, pf_strategy_t handle) {
    pf_report_sink_receipt_v1 value{};
    value.struct_size = sizeof value;
    value.version = 1;
    assert(api.receipt(handle, &value) == 0);
    return value;
}
void install(const Api& api, pf_strategy_t handle, const std::string& path) {
    // All three buffers go out of scope or are changed before the run.
    std::string name = path, id = "lane-\"L6B1\"\n", scratch = ".";
    pf_report_sink_v1 sink{sizeof sink, 1, name.c_str(), id.c_str(), scratch.c_str()};
    assert(api.set(handle, &sink) == 0);
    name.assign("wrong-path"); id.assign("wrong-id"); scratch.assign("wrong-dir");
}
std::vector<std::string> records(const std::string& bytes) {
    std::vector<std::string> result;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto end = bytes.find('\n', at);
        assert(end != std::string::npos);
        if (bytes[at] == '[') result.push_back(bytes.substr(at, end - at));
        at = end + 1;
    }
    return result;
}
std::string equity(const pf_equity_point_t& point) {
    using pineforge::report_sink::f64;
    return "[\"E\"," + std::to_string(point.time_ms) + ',' + f64(point.equity)
        + ',' + f64(point.open_profit) + ']';
}
std::string trade(const Api& api, pf_strategy_t handle, int i, const pf_trade_t& row) {
    using pineforge::report_sink::f64;
    using pineforge::report_sink::escaped_string;
    std::string line = "[\"T\"," + std::to_string(i + 1)
        + (row.is_long ? ",\"long\"," : ",\"short\",")
        + std::to_string(row.entry_time) + ',' + std::to_string(row.exit_time);
    for (const auto value : {row.entry_price, row.exit_price, row.qty, row.pnl, row.pnl_pct,
                             row.max_runup, row.max_drawdown, row.commission})
        line += ',' + f64(value);
    line += ',' + std::to_string(row.entry_bar_index) + ',' + std::to_string(row.exit_bar_index);
    line += row.open_at_end ? ",true," : ",false,";
    line += std::to_string(api.incarnation(handle, i)) + ',' + std::to_string(api.cause(handle, i));
    for (const auto value : {api.entry(handle, i), api.exit(handle, i), api.comment(handle, i)})
        line += ',' + escaped_string(value ? value : "");
    return line + ']';
}
int observed(void* context, const pf_execution_boundary_v1* boundary, pf_boundary_receipt_v1* out) {
    ++*static_cast<int*>(context);
    *out = {sizeof(*out), 1, boundary->run_generation, 0, 0, 0, 0};
    return 0;
}
int rejected(void*, const pf_execution_boundary_v1*, pf_boundary_receipt_v1*) { return -1; }

void verify_reference_reader(const std::string& path) {
    // An independent Python parse and hashlib digest over this real module's
    // bytes. Every prefix ending before the trailer must be refused.
    static constexpr const char* script = R"PY(
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
from pf_sink import SinkReader, SinkFormatError
path = Path(sys.argv[2])
data = path.read_bytes()
lines = data.splitlines(keepends=True)
trailer = json.loads(lines[-1])
assert hashlib.sha256(b''.join(lines[:-1])).hexdigest() == trailer['sha256']
with SinkReader(path) as reader:
    rows = list(reader)
    assert reader.complete and reader.trailer == trailer
    assert reader.counts['E'] > 0
    assert set(reader.counts) == {'E', 'T'}
for count in range(len(lines)):
    cut = path.with_name(path.name + '.cut-' + str(count))
    cut.write_bytes(b''.join(lines[:count]))
    try:
        with SinkReader(cut) as reader:
            list(reader)
    except SinkFormatError:
        pass
    else:
        raise AssertionError('reader accepted a sink without its trailer')
)PY";
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(PINEFORGE_L6B1_PYTHON, PINEFORGE_L6B1_PYTHON, "-B", "-c", script,
              PINEFORGE_L6B1_DOCKER, path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void equal_run(const Api& api, const std::string& path, bool selected, bool observer, bool empty) {
    const auto baseline = api.create("{}"), streamed = api.create("{}");
    auto bars = feed();
    int callbacks = 0;
    pf_execution_observer_v1 ob{sizeof ob, 1, &callbacks, observed};
    for (const auto handle : {baseline, streamed}) {
        if (empty) api.mode(handle, "mode", "empty");
        if (selected) {
            pf_selected_window_config_v1 window{sizeof window, 1, bars.front().timestamp,
                                                bars.back().timestamp + 60000};
            assert(api.selected(handle, &window) == 0);
        }
        if (observer) assert(api.observer(handle, &ob) == 0);
    }
    install(api, streamed, path);
    assert(receipt(api, streamed).complete == 0);
    pf_report_t before{}, after{};
    api.run(baseline, bars.data(), static_cast<int>(bars.size()), &before);
    api.run(streamed, bars.data(), static_cast<int>(bars.size()), &after);
    assert(api.error(baseline)[0] == '\0' && api.error(streamed)[0] == '\0');
    assert(api.status(streamed) == 0 && api.busy(streamed) == -3);
    assert(api.hash(baseline) == api.hash(streamed));
    assert(before.total_trades == after.total_trades && before.net_profit == after.net_profit);
    assert(std::memcmp(&before.metrics, &after.metrics, sizeof(before.metrics)) == 0);
    assert(after.trades == nullptr && after.trades_len == 0);
    assert(after.equity_curve == nullptr && after.equity_curve_len == 0);
    assert(after.broker_state_hash == nullptr && after.broker_state_hash_len == 0);
    assert(after.trace == nullptr && after.trace_len == 0);
    assert(after.script_bars_processed == before.script_bars_processed);
    const auto bytes = read(path);
    const auto rows = records(bytes);
    assert(bytes.find("\"run_id\":\"lane-\\\"L6B1\\\"\\u000a\"") != std::string::npos);
    assert(bytes.find(std::string("\"engine\":\"") + PINEFORGE_VERSION_STRING + '"') != std::string::npos);
    assert(bytes.find("\"type\":\"E\"") < bytes.find("\"type\":\"T\""));
    assert(rows.size() == static_cast<std::size_t>(before.equity_curve_len + before.trades_len));
    std::size_t cursor = 0;
    for (int64_t i = 0; i < before.equity_curve_len; ++i) assert(rows[cursor++] == equity(before.equity_curve[i]));
    bool has_closed = false, has_open = false, has_short = false;
    for (int i = 0; i < before.trades_len; ++i) {
        assert(rows[cursor++] == trade(api, baseline, i, before.trades[i]));
        has_closed |= !before.trades[i].open_at_end;
        has_open |= before.trades[i].open_at_end != 0;
        has_short |= !before.trades[i].is_long;
    }
    assert(empty ? before.trades_len == 0 : (has_closed && has_open && has_short));
    const auto got = receipt(api, streamed);
    assert(got.complete == 1 && got.trades_retained == 1);
    assert(got.count_e == before.equity_curve_len && got.count_t == before.trades_len);
    assert(got.count_o == 0 && got.count_v == 0 && got.count_k == 0 && got.count_x == 0 && got.count_h == 0);
    const auto trailer = bytes.find("{\"trailer\":");
    assert(trailer != std::string::npos);
    pineforge::report_sink::Sha256 hash;
    hash.update(std::string_view(bytes).substr(0, trailer));
    assert(hash.hex_digest() == got.sha256_hex);
    assert(bytes.substr(trailer) == "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":"
        + std::to_string(got.count_e) + ",\"T\":" + std::to_string(got.count_t)
        + "},\"sha256\":\"" + got.sha256_hex + "\"}\n");
    for (const auto byte : got.reserved) assert(byte == 0);
    verify_reference_reader(path);
    pf_report_t again{};
    api.again(streamed, &again);
    assert(read(path) == bytes && receipt(api, streamed).count_e == got.count_e);
    assert(std::memcmp(&again.metrics, &after.metrics, sizeof(after.metrics)) == 0);
    api.free(&again); api.free(&after); api.free(&before);
    // A reused handle truncates a fresh generation and resets both counts.
    api.mode(streamed, "mode", "empty");
    api.run(streamed, bars.data(), static_cast<int>(bars.size()), &after);
    assert(receipt(api, streamed).complete == 1 && receipt(api, streamed).count_t == 0);
    api.free(&after);
    assert(api.set(streamed, nullptr) == 0 && receipt(api, streamed).complete == 0);
    api.destroy(streamed); api.destroy(baseline);
    assert(!observer || callbacks == 3);
}
void refusals(const Api& api, const std::string& directory) {
    auto handle = api.create("{}");
    assert(api.version() == 1);
    assert(api.set(nullptr, nullptr) == -1);
    pf_report_sink_receipt_v1 bad;
    std::memset(&bad, 0xa5, sizeof bad);
    const auto saved = bad;
    assert(api.receipt(handle, &bad) == -1 && std::memcmp(&bad, &saved, sizeof bad) == 0);
    bad.struct_size = sizeof bad; bad.version = 1;
    const auto valid = bad;
    assert(api.receipt(nullptr, &bad) == -1 && std::memcmp(&bad, &valid, sizeof bad) == 0);
    assert(api.receipt(handle, nullptr) == -1);
    bad.version = 2;
    const auto wrong_version = bad;
    assert(api.receipt(handle, &bad) == -1
        && std::memcmp(&bad, &wrong_version, sizeof bad) == 0);
    auto initial = receipt(api, handle);
    assert(initial.count_e == 0 && initial.complete == 0 && initial.sha256_hex[0] == '\0');
    pf_report_sink_v1 sink{sizeof sink, 1, "", nullptr, nullptr};
    assert(api.set(handle, &sink) == -1);
    sink.path = "."; sink.version = 2;
    assert(api.set(handle, &sink) == -1);
    sink.version = 1; --sink.struct_size;
    assert(api.set(handle, &sink) == -1);
    sink = {sizeof sink, 1, nullptr, nullptr, nullptr};
    assert(api.set(handle, &sink) == -1);
    auto bars = feed();
    install(api, handle, directory + "/stream-refused.ndjson");
    assert(api.stream(handle, bars.data(), 1, "1", "1") != 0);
    assert(receipt(api, handle).complete == 0);
    assert(api.set(handle, nullptr) == 0);
    assert(api.stream(handle, bars.data(), 1, "1", "1") == 0);
    sink = {sizeof sink, 1, "/unused", nullptr, nullptr};
    assert(api.set(handle, &sink) == -2 && api.set(handle, nullptr) == -2);
    assert(api.end(handle, 0) == 0);
    api.destroy(handle);
    handle = api.create("{}");
    const auto default_path = directory + "/default-strings.ndjson";
    sink = {sizeof sink, 1, default_path.c_str(), nullptr, nullptr};
    assert(api.set(handle, &sink) == 0);
    api.mode(handle, "mode", "empty");
    pf_report_t defaults{};
    api.run(handle, bars.data(), static_cast<int>(bars.size()), &defaults);
    const auto completed = receipt(api, handle);
    assert(completed.complete == 1);
    assert(read(default_path).find("\"run_id\":\"\"") != std::string::npos);
    sink.version = 2;
    assert(api.set(handle, &sink) == -1);
    const auto unchanged = receipt(api, handle);
    assert(std::memcmp(&completed, &unchanged, sizeof completed) == 0);
    api.free(&defaults);
    api.destroy(handle);
    for (const std::string mode : {"throw", "abort", "observer", "selected", "io", "write-io"}) {
        handle = api.create("{}");
        const std::string path = mode == "io" ? directory + "/missing/failed.ndjson"
            : mode == "write-io" ? "/dev/full" : directory + '/' + mode + ".ndjson";
        if (mode == "write-io") {
            std::ifstream full("/dev/full");
            if (!full) { api.destroy(handle); continue; } // Linux-only write fault.
        }
        install(api, handle, path);
        if (mode == "throw" || mode == "abort") api.mode(handle, "mode", mode.c_str());
        if (mode == "observer") {
            pf_execution_observer_v1 ob{sizeof ob, 1, nullptr, rejected};
            assert(api.observer(handle, &ob) == 0);
        }
        if (mode == "selected") {
            pf_selected_window_config_v1 window{sizeof window, 1, bars[3].timestamp,
                                                bars.back().timestamp + 60000};
            assert(api.selected(handle, &window) == 0);
        }
        pf_report_t report{};
        api.run(handle, bars.data(), static_cast<int>(bars.size()), &report);
        const auto got = receipt(api, handle);
        assert(got.complete == 0 && got.sha256_hex[0] == '\0');
        assert(api.status(handle) != 0 || api.error(handle)[0] != '\0');
        if (mode == "io" || mode == "write-io") assert(std::strcmp(api.code(handle), "report_sink_failed") == 0);
        if (mode != "write-io") assert(read(path).find("{\"trailer\":") == std::string::npos);
        api.free(&report); api.destroy(handle);
    }
}
}
int main() {
    try {
        Api api;
        char directory[] = "/tmp/pineforge-l6b1-XXXXXX";
        if (!mkdtemp(directory)) throw std::runtime_error("cannot create test output directory");
        refusals(api, directory);
        equal_run(api, std::string(directory) + "/ordinary.ndjson", false, false, false);
        equal_run(api, std::string(directory) + "/selected.ndjson", true, false, false);
        equal_run(api, std::string(directory) + "/observed.ndjson", false, true, false);
        equal_run(api, std::string(directory) + "/selected-observed.ndjson", true, true, false);
        equal_run(api, std::string(directory) + "/empty.ndjson", false, false, true);
        std::printf("L6B1 sink ABI, real-module E/T identity, receipt, lifecycle: passed (%s)\n", directory);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L6B1: %s\n", error.what());
        return 1;
    }
}
#endif
