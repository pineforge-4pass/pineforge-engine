// CR-150 L6b-3 red contract on f00151f223d35d7638c299a60ab401df3f36cf0d:
// the header advertises O/V/K/X/H, but the runtime writes none of them.
// Existing APIs only: the first post-fill assertion diagnoses missing O rows.
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>

#ifdef PINEFORGE_L6B3_SOURCE
#include <pineforge/source/pine_strategy_host.hpp>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace pineforge;
using report_sink::escaped_string;
using report_sink::f64;
constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t step_ms = 60000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

std::string hex64(std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return escaped_string(text);
}

std::string time_token(std::int64_t value) {
    return value == INT64_MIN ? "null" : std::to_string(value);
}

std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

pf_report_sink_receipt_v1 receipt(const BacktestEngine& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_receipt_v1(&result) == 0, "receipt query refused");
    return result;
}

void install(BacktestEngine& host, const std::string& path) {
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L6b-3", nullptr};
    require(host.set_report_sink_v1(&sink) == 0, "sink install refused");
}

std::vector<Bar> feed() {
    std::vector<Bar> result;
    for (int i = 0; i < 8; ++i) {
        const double price = 100.0 + i * 0.25;
        result.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                          1.0, start_ms + i * step_ms});
    }
    return result;
}

NativeRunSpec spec(std::uint64_t run_number = 1) {
    NativeRunSpec result;
    result.identity = {"L6b-3", run_number};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:L6B3";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 10000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    result.report_open_position_at_end = true;
    return result;
}

class NativeProbe final : public NativeStrategyHost {
public:
    explicit NativeProbe(bool streaming) : streaming_(streaming) { declare_outputs(3, 2, 3); }
    bool bounded = true;
    bool fail = false;
    bool clear_events = false;
    bool no_rows = false;
    int calculations = 0;
    std::size_t message_bytes = 0;

    void on_native_run_begin() override {
        calculations = 0;
        bounded = true;
        output_run_begin();
        output_constant(0, -0.0);
        output_constant(1, std::numeric_limits<double>::infinity());
        // Constant 2 stays unwritten, and must also be null.
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        const int k = calculations++;
        if (streaming_) bounded &= broker_state_hashes_.size() <= 1;
        if (!no_rows) {
            // Calculation 3 recalculates row 2, retracting its values/events.
            const auto open = context.script_bar_open_ms - (k == 3 ? step_ms : 0);
            const auto close = k == 5 ? INT64_MIN : open + step_ms + (k == 3 ? 1 : 0);
            output_bar(open, close);
            output_value(0, bar.close);
            output_value(1, k == 4 ? -0.0 : missing);
            output_event(0, -0.0);
            const std::string message = message_bytes ? std::string(message_bytes, 'x')
                : k == 2 ? "retracted" : std::string("quote\"\\\n\0UTF-8:\xc3\xa9", 17);
            output_event(1, missing, message);
            if (clear_events && k == 4) output_events_clear();
        }
        trace("price\"\\\n", bar.close);
        trace("missing", missing);
        if (streaming_)
            bounded &= output_bars_len() <= 1 && output_events_len() <= 2 && trace_buffer_.empty();
        if (fail && k == 5) throw std::runtime_error("L6b-3 deliberate callback failure");
        if (k == 0 || k == 4 || k == 6) {
            native_order::Request order;
            order.intent = native_order::Transact{k == 4 ? -1.0 : 1.0};
            order.label = k == 4 ? "close" : "entry";
            submit(order);
        }
    }
private:
    bool streaming_;
};

#ifdef PINEFORGE_L6B3_SOURCE
class PineProbe final : public source::PineStrategyHost {
public:
    PineProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        declare_outputs(1, 1, 1);
    }
    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0) output_run_begin();
        output_bar(bar.timestamp, bar.timestamp + step_ms);
        output_value(0, bar.close);
        output_event(0, bar.close, "Pine host marks");
        output_constant(0, 42.0);
        trace("Pine", bar.close);
        if (bar_index_ == 0 || bar_index_ == 4 || bar_index_ == 6)
            strategy_entry(bar_index_ == 4 ? "S" : "L", bar_index_ != 4,
                           missing, missing, 1.0);
    }
};
#endif

// Snapshot the sink-off run through the same public readers its host uses.
// These are expected values, not a fabricated report-sink producer.
void expected_rows(const BacktestEngine& host, const ReportC& report, const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    require(bool(file), "cannot open expected-value snapshot");
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) {
        const auto& row = report.equity_curve[i];
        file << "[\"E\"," << row.time_ms << ',' << f64(row.equity)
             << ',' << f64(row.open_profit) << "]\n";
    }
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& row = report.trades[i];
        const auto& identity = host.get_report_trade(i);
        file << "[\"T\"," << i + 1 << (row.is_long ? ",\"long\"," : ",\"short\",")
             << row.entry_time << ',' << row.exit_time;
        for (const double value : {row.entry_price, row.exit_price, row.qty, row.pnl, row.pnl_pct,
                                  row.max_runup, row.max_drawdown, row.commission})
            file << ',' << f64(value);
        file << ',' << row.entry_bar_index << ',' << row.exit_bar_index
             << (row.open_at_end ? ",true," : ",false,") << identity.entry_incarnation
             << ',' << host.closed_trade_close_cause(i) << ',' << escaped_string(identity.entry_id)
             << ',' << escaped_string(identity.exit_id) << ',' << escaped_string(identity.exit_comment) << "]\n";
    }
    const auto rows = host.output_bars_len();
    std::vector<std::int64_t> opens(static_cast<std::size_t>(rows)), closes(opens.size());
    std::int64_t written = -1;
    require(host.output_bar_times_copy(0, opens.data(), closes.data(), rows, &written)
            && written == rows, "baseline output times refused");
    std::vector<std::vector<double>> values(static_cast<std::size_t>(host.output_series_count()),
                                           std::vector<double>(opens.size()));
    for (int slot = 0; slot < host.output_series_count(); ++slot)
        require(host.output_series_copy(slot, 0, values[slot].data(), rows, &written)
                && written == rows, "baseline output values refused");
    for (std::int64_t i = 0; i < rows; ++i) {
        file << "[\"O\"," << i << ',' << time_token(opens[i]) << ',' << time_token(closes[i]);
        for (const auto& column : values) file << ',' << f64(column[i]);
        file << "]\n";
    }
    for (int i = 0; i < host.output_events_len(); ++i) {
        const auto& row = *host.output_event_at(i);
        file << "[\"V\"," << row.sequence << ',' << row.output_index << ',' << row.bar_index
             << ',' << time_token(row.bar_open_ms) << ',' << time_token(row.bar_close_ms)
             << ',' << row.ordinal_in_bar << ',' << row.phase << ',' << row.confirmed
             << ',' << f64(row.value) << ',' << hex64(row.message_hash64) << ','
             << (row.has_message ? escaped_string(row.message) : "null") << "]\n";
    }
    file << "[\"K\"";
    for (int i = 0; i < host.output_constants_count(); ++i) file << ',' << f64(host.output_constant_value(i));
    file << "]\n";
    for (int i = 0; i < report.trace_len; ++i) {
        const auto& row = report.trace[i];
        file << "[\"X\"," << row.timestamp << ',' << row.bar_index << ','
             << escaped_string(report.trace_names[row.name_id]) << ',' << f64(row.value) << "]\n";
    }
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        file << "[\"H\"," << hex64(report.broker_state_hash[i]) << "]\n";
    file.close();
    require(bool(file), "expected-value snapshot write failed");
}

void reference_reader(const std::string& path, const std::string& expected) {
    static constexpr const char* script = R"PY(
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.path.insert(0, sys.argv[1])
from pf_sink import SinkReader
path, expected = map(Path, sys.argv[2:])
def exact(value):
    if type(value) is float:
        return ('f64', struct.pack('>d', value))
    if type(value) is list:
        return tuple(map(exact, value))
    return (type(value).__name__, value)
want = {tag: [] for tag in 'ETOVKXH'}
for line in expected.read_text().splitlines():
    row = json.loads(line)
    want[row[0]].append(exact(row))
got = {tag: [] for tag in want}
with SinkReader(path) as reader:
    for row in reader:
        got[row.tag].append(exact([row.tag, *row.values]))
    assert reader.complete
    assert list(reader.counts) == list('ETOVKXH')
    assert reader.counts == {tag: len(rows) for tag, rows in want.items()}
    assert got == want, [(tag, len(got[tag]), len(want[tag])) for tag in want if got[tag] != want[tag]]
lines = path.read_bytes().splitlines(keepends=True)
assert hashlib.sha256(b''.join(lines[:-1])).hexdigest() == reader.sha256
)PY";
    const auto child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        execl(PINEFORGE_L6B3_PYTHON, PINEFORGE_L6B3_PYTHON, "-B", "-c", script,
              PINEFORGE_L6B3_DOCKER, path.c_str(), expected.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "reference reader or exact sink-off value comparison failed");
}

template<class Host> void equal_run(Host& baseline, Host& streamed, const std::string& path) {
    for (auto* host : {&baseline, &streamed}) {
        require(host->set_outputs_enabled(true), "output enable refused");
        host->set_trace_enabled(true);
        host->set_broker_state_hash_recording(true);
    }
    install(streamed, path);
    const auto bars = feed();
    baseline.run(bars.data(), static_cast<int>(bars.size()));
    streamed.run(bars.data(), static_cast<int>(bars.size()));
    require(baseline.last_error().empty() && streamed.last_error().empty(), "fixture run failed");
    ReportC before{}, after{};
    baseline.fill_report(&before);
    streamed.fill_report(&after);
    const auto got = receipt(streamed);
    // First red on the base: E/T finish successfully, but count_o is zero.
    require(got.count_o == baseline.output_bars_len(), "missing O records: header advertises outputs but no output rows were written");
    require(got.complete == 1 && got.count_v == baseline.output_events_len()
            && got.count_k == 1 && got.count_x == before.trace_len
            && got.count_h == before.broker_state_hash_len, "optional record counts differ");
    require(got.count_e == before.equity_curve_len && got.count_t == before.trades_len
            && before.trades_len > 0, "E/T fixture is empty or differs");
    require(std::memcmp(&before.metrics, &after.metrics, sizeof before.metrics) == 0
            && baseline.broker_state_hash() == streamed.broker_state_hash(), "sink changed metrics or broker state");
    require(after.trace == nullptr && after.broker_state_hash == nullptr, "sink report contains historical arrays");
    require(streamed.output_bars_len() <= 1 && streamed.output_events_len() <= 2,
            "outputs retain more than the held row");
    const auto expected = path + ".expected";
    expected_rows(baseline, before, expected);
    reference_reader(path, expected);
    const auto bytes = read(path);
    BacktestEngine::free_report(&after);
    streamed.fill_report(&after);
    require(read(path) == bytes && receipt(streamed).count_k == 1, "repeated fill appended records");
    BacktestEngine::free_report(&before);
    BacktestEngine::free_report(&after);
}

void native_rows(const std::string& directory) {
    NativeProbe baseline(false), streamed(true);
    for (auto* host : {&baseline, &streamed})
        require(host->configure_native(spec()).status == NativeSetupStatus::Applied, "native configure refused");
    equal_run(baseline, streamed, directory + "/native.ndjson");
    require(streamed.bounded, "in-run output/trace/hash history grew beyond hold-back");
    // Reusing the hosts resets row numbering, sequences, hashes and receipt
    // counts. This second run has constants but no O or V rows.
    for (auto* host : {&baseline, &streamed}) {
        host->no_rows = true;
        require(host->configure_native(spec(2)).status == NativeSetupStatus::Applied, "native reconfigure refused");
    }
    equal_run(baseline, streamed, directory + "/constants-only.ndjson");
}

void cleared_events_and_failure(const std::string& directory) {
    for (const bool fail : {false, true}) {
        NativeProbe host(true);
        host.clear_events = !fail;
        host.fail = fail;
        require(host.configure_native(spec()).status == NativeSetupStatus::Applied, "native configure refused");
        require(host.set_outputs_enabled(true), "output enable refused");
        host.set_trace_enabled(true);
        host.set_broker_state_hash_recording(true);
        const auto path = directory + (fail ? "/failed.ndjson" : "/cleared.ndjson");
        install(host, path);
        const auto bars = feed();
        host.run(bars.data(), static_cast<int>(bars.size()));
        ReportC report{};
        host.fill_report(&report);
        if (fail) {
            require(!host.last_error().empty() && receipt(host).complete == 0,
                    "failed run completed its sink");
            require(read(path).find("{\"trailer\":") == std::string::npos, "failed run wrote a trailer");
        } else {
            // Seven output rows after one recalculation, two events each;
            // clearing row 3's queue must not erase any event from the sink.
            require(host.last_error().empty() && receipt(host).count_o == 7
                    && receipt(host).count_v == 14 && receipt(host).complete == 1,
                    "events_clear erased sink events or changed sequence accounting");
            require(read(path).find("retracted") == std::string::npos, "sink retained retracted events");
        }
        BacktestEngine::free_report(&report);
    }
}
}

int main() {
    try {
        char directory[] = "/tmp/pineforge-l6b3-XXXXXX";
        require(mkdtemp(directory) != nullptr, "cannot create test directory");
        native_rows(directory);
        cleared_events_and_failure(directory);
#ifdef PINEFORGE_L6B3_SOURCE
        PineProbe baseline, streamed;
        equal_run(baseline, streamed, std::string(directory) + "/pine.ndjson");
#endif
        std::printf("L6b-3 O/V/K/X/H exact read-back, hold-back and lifecycle: passed (%s)\n", directory);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L6b-3: %s\n", error.what());
        return 1;
    }
}
