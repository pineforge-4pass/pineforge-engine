// R14--R18. With the previous header closure this still builds and runs the
// real populations: the mode switch is absent and the heap-growth assertion
// fails. The array run below is the independent legacy reference, never a
// snapshot reconstructed from the streamed records under test.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/report_sink_writer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {
using namespace pineforge;
constexpr std::int64_t start = INT64_C(1704067200000);
constexpr std::int64_t step = 60000;

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}

enum class Row { Hash, Trace, Names, Series, Events, All };
bool selects(Row actual, Row wanted) { return actual == wanted || actual == Row::All; }

struct Directory {
    std::string path = "/tmp/pineforge-sequences-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create sequence directory"); }
    ~Directory() { ::rmdir(path.c_str()); }
};

NativeRunSpec spec() {
    NativeRunSpec result;
    result.identity = {"optional-sequences", 1};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:SEQUENCES";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 10000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.25;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

class Probe final : public NativeStrategyHost {
public:
    Row row;
    int calculations = 0;
    int recorded_rows = 0;
    int trace_calls = 0;
    int event_calls = 0;
    bool clear_same_row = false;
    bool recalculate_after_clear = false;

    explicit Probe(Row which) : row(which) { declare_outputs(3, 2, 2); }
    void on_native_run_begin() override {
        calculations = recorded_rows = trace_calls = event_calls = 0;
        output_run_begin();
        output_constant(0, -0.0);
        // Constant 1 deliberately remains unwritten.
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        const int b = calculations++;
        if (selects(row, Row::Trace) || selects(row, Row::Names)) {
            const std::string name = selects(row, Row::Names)
                ? std::string("dynamic\0name/", 13) + std::to_string(b) : "literal";
            for (int call = 0; call < 4; ++call) {
                trace(name, call == 1 ? -0.0 : bar.close + call);
                ++trace_calls;
            }
        }
        if (selects(row, Row::Series) || selects(row, Row::Events)) {
            const auto open = context.script_bar_open_ms;
            output_bar(open, open + step);
            ++recorded_rows;
            output_value(0, -123.0);
            if (selects(row, Row::Events)) {
                output_event(0, -999.0, "must be retracted");
                ++event_calls;
            }
            // Every actual bar recalculates its row. None of the provisional
            // values/events may reach the sink; sequence numbers are reissued.
            output_bar(open, b % 7 == 0 ? INT64_MIN : open + step + 1);
            output_value(0, bar.close);
            output_value(1, -0.0);
            if (selects(row, Row::Events)) {
                output_event(0, -0.0);
                output_event(1, bar.close, std::string("quoted\"\\\n\0end", 13));
                event_calls += 2;
                if (clear_same_row) {
                    output_events_clear();
                    if (recalculate_after_clear) output_bar(open, open + step);
                }
            }
        }
    }
};

std::vector<Bar> feed(int n) {
    std::vector<Bar> result;
    result.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + (i % 13) * 0.25;
        result.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0, start + i * step});
    }
    return result;
}

void configure(Probe& host, bool enabled, bool summary, const std::string& path) {
    require(host.configure_native(spec()).status == NativeSetupStatus::Applied,
            "configure real sequence fixture");
#ifdef PINEFORGE_HAS_OPTIONAL_REPORT_SEQUENCES_V1
    require(host.set_optional_report_mode(summary
                ? report_sequence::ReportMode::SummaryOnly
                : report_sequence::ReportMode::LegacyArrays) == 0, "select sequence mode");
#else
    (void)summary;
#endif
    host.set_broker_state_hash_recording(enabled && selects(host.row, Row::Hash));
    host.set_trace_enabled(enabled && (selects(host.row, Row::Trace)
                                      || selects(host.row, Row::Names)));
    require(host.set_outputs_enabled(enabled && (selects(host.row, Row::Series)
                                                || selects(host.row, Row::Events))),
            "select recorded outputs");
    if (!path.empty()) {
        const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "sequences", nullptr};
        require(host.set_report_sink_v1(&sink) == 0, "install external sequence sink");
    }
}

std::size_t measured(const std::vector<Bar>& bars, Row row, bool enabled,
                     bool streamed, const std::string& path) {
    require(global_allocation::live_bytes == 0, "previous census left live allocations");
    global_allocation::recording = true;
    std::size_t report_bytes = 0;
    {
        Probe host(row);
        configure(host, enabled, !streamed, streamed ? path : "");
        host.run(bars.data(), static_cast<int>(bars.size()));
        require(host.last_error().empty(), "population run failed");
        ReportC report{};
        host.fill_report(&report);
        // Assert the consumed population before measuring any bound.
        require(report.input_bars_processed == static_cast<std::int64_t>(bars.size())
                && report.script_bars_processed == static_cast<std::int64_t>(bars.size())
                && host.calculations == static_cast<int>(bars.size()),
                "fixture did not consume its advertised real bars");
        if (selects(row, Row::Trace) || selects(row, Row::Names))
            require(host.trace_calls == 4 * static_cast<int>(bars.size()),
                    "fixture trace population is empty or truncated");
        if (selects(row, Row::Series) || selects(row, Row::Events))
            require(host.recorded_rows == static_cast<int>(bars.size()),
                    "fixture output row population is empty or truncated");
        if (selects(row, Row::Events))
            require(host.event_calls == 3 * static_cast<int>(bars.size()),
                    "fixture event population is empty or truncated");
        report_bytes = global_allocation::live_bytes;
        BacktestEngine::free_report(&report);
    }
    require(global_allocation::live_bytes == 0, "sequence fixture leaked resident storage");
    global_allocation::recording = false;
    return report_bytes;
}

void growth(Row row, bool streamed, const Directory& directory) {
    std::array<std::int64_t, 2> charged{};
    int population = 0;
    for (const int n : {1000, 100000}) {
        const auto bars = feed(n); // shared immutable input, outside the census
        const auto path = directory.path + "/growth.ndjson";
        const auto control = measured(bars, row, false, streamed, path);
        const auto enabled = measured(bars, row, true, streamed, path);
        charged[population++] = static_cast<std::int64_t>(enabled)
            - static_cast<std::int64_t>(control);
        if (streamed) ::unlink(path.c_str());
    }
    std::printf("R%d %s: optional report bytes 1k=%lld 100k=%lld\n",
                14 + static_cast<int>(row), streamed ? "streamed" : "summary",
                static_cast<long long>(charged[0]), static_cast<long long>(charged[1]));
    // The off control removes unrelated equity storage owned by o1w-metrics.
    // No allocator size-class noise can explain a whole growing history.
    require(charged[1] <= charged[0] + 4096,
            "optional report resident bytes grow between 1,000 and 100,000 real bars");
}

std::vector<std::string> records(const std::string& path, const std::string& tag) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "open streamed report");
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line))
        if (line.compare(0, tag.size(), tag) == 0) result.push_back(line);
    require(input.eof(), "read complete streamed report");
    return result;
}

std::string hex(std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return report_sink::escaped_string(text);
}
std::string time(std::int64_t value) {
    return value == INT64_MIN ? "null" : std::to_string(value);
}

void differential(const Directory& directory, bool clear_events) {
    const auto bars = feed(1000);
    Probe legacy(Row::All), external(Row::All), summary(Row::All);
    legacy.clear_same_row = external.clear_same_row = summary.clear_same_row = clear_events;
    const auto path = directory.path + "/differential.ndjson";
    configure(legacy, true, false, "");
    configure(external, true, false, path);
    configure(summary, true, true, "");
    for (auto* host : {&legacy, &external, &summary})
        host->run(bars.data(), static_cast<int>(bars.size()));
    require(legacy.last_error().empty() && external.last_error().empty()
            && summary.last_error().empty(), "differential population failed");
    ReportC before{}, after{}, reduced{};
    legacy.fill_report(&before);
    external.fill_report(&after);
    summary.fill_report(&reduced);
    require(before.broker_state_hash_len == 1000 && before.trace_len == 4000
            && before.trace_names_len == 1000 && legacy.output_bars_len() == 1000,
            "legacy oracle has the wrong actual population");
    require(std::memcmp(&before.metrics, &after.metrics, sizeof before.metrics) == 0
            && std::memcmp(&before.metrics, &reduced.metrics, sizeof before.metrics) == 0,
            "optional recording changed a metric bit");
    std::vector<std::string> hashes, traces, rows, events;
    for (int i = 0; i < before.broker_state_hash_len; ++i)
        hashes.push_back("[\"H\"," + hex(before.broker_state_hash[i]) + "]");
    for (int i = 0; i < before.trace_len; ++i) {
        const auto& e = before.trace[i];
        require(e.name_id == i / 4, "legacy first-seen trace-name IDs changed");
        traces.push_back("[\"X\"," + std::to_string(e.timestamp) + ','
            + std::to_string(e.bar_index) + ','
            + report_sink::escaped_string(std::string_view(
                before.trace_names[e.name_id], 13 + std::to_string(i / 4).size()))
            + ',' + report_sink::f64(e.value) + "]");
    }
    for (int i = 0; i < 1000; ++i) {
        std::int64_t open = 0, close = 0, written = 0;
        require(legacy.output_bar_times_copy(i, &open, &close, 1, &written) && written == 1,
                "read legacy output row times");
        std::string line = "[\"O\"," + std::to_string(i) + ',' + time(open) + ',' + time(close);
        for (int slot = 0; slot < 3; ++slot) {
            double value = 0.0;
            require(legacy.output_series_copy(slot, i, &value, 1, &written) && written == 1,
                    "read legacy output value");
            line += ',' + report_sink::f64(value);
        }
        rows.push_back(line + ']');
    }
    // In clear mode the public legacy queue is intentionally empty. A second
    // uncleared oracle supplies the same final events and sequence numbers.
    Probe event_oracle(Row::All);
    configure(event_oracle, true, false, "");
    event_oracle.run(bars.data(), static_cast<int>(bars.size()));
    require(event_oracle.last_error().empty() && event_oracle.output_events_len() == 2000,
            "legacy event oracle population");
    for (int i = 0; i < event_oracle.output_events_len(); ++i) {
        const auto& e = *event_oracle.output_event_at(i);
        require(e.sequence == static_cast<std::uint64_t>(i + 1), "rollback changed sequence");
        events.push_back("[\"V\"," + std::to_string(e.sequence) + ','
            + std::to_string(e.output_index) + ',' + std::to_string(e.bar_index) + ','
            + time(e.bar_open_ms) + ',' + time(e.bar_close_ms) + ','
            + std::to_string(e.ordinal_in_bar) + ',' + std::to_string(e.phase) + ','
            + std::to_string(e.confirmed) + ',' + report_sink::f64(e.value) + ','
            + hex(e.message_hash64) + ','
            + (e.has_message ? report_sink::escaped_string(e.message) : "null") + ']');
    }
    require(records(path, "[\"H\",") == hashes, "hash leaves differ from legacy array");
    require(records(path, "[\"X\",") == traces, "trace records or dynamic names differ");
    require(records(path, "[\"O\",") == rows, "recorded rows differ from legacy arrays");
    require(records(path, "[\"V\",") == events, "events escaped rollback or changed sequences");
    require(records(path, "[\"K\"") == std::vector<std::string>{"[\"K\",-0.0,null]"},
            "run constants changed");
#ifdef PINEFORGE_HAS_OPTIONAL_REPORT_SEQUENCES_V1
    const auto witness = summary.optional_sequence_summary();
    const auto streamed = external.optional_sequence_summary();
    require(witness.hashes.count == 1000 && witness.traces.count == 4000
            && witness.rows.count == 1000 && witness.events.count == 2000,
            "summary witnesses did not consume the full population");
    require(witness.hashes.digest == streamed.hashes.digest
            && witness.traces.digest == streamed.traces.digest
            && witness.rows.digest == streamed.rows.digest
            && witness.events.digest == streamed.events.digest,
            "ordered summary witnesses differ from the external run");
    require(summary.output_events_len() == 0 && reduced.trace == nullptr
            && reduced.trace_names == nullptr && reduced.broker_state_hash == nullptr,
            "summary contains optional history arrays");
#endif
    BacktestEngine::free_report(&before);
    BacktestEngine::free_report(&after);
    BacktestEngine::free_report(&reduced);
    ::unlink(path.c_str());
}

} // namespace

int main() {
    try {
        Directory directory;
        // Warm the process-wide calendar/timezone caches over both census
        // populations before any allocation is recorded, so a lazy static
        // cache is not mistaken for sequence growth.
        for (const int n : {2, 1000, 100000}) {
            const auto warm = feed(n);
            Probe host(Row::Hash);
            configure(host, false, false, "");
            host.run(warm.data(), static_cast<int>(warm.size()));
            ReportC report{};
            host.fill_report(&report);
            BacktestEngine::free_report(&report);
        }
        for (const auto row : {Row::Hash, Row::Trace, Row::Names, Row::Series, Row::Events}) {
            growth(row, false, directory);
            growth(row, true, directory);
        }
        differential(directory, false);
        differential(directory, true);
        std::puts("optional report sequences: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "optional report sequences: %s\n", error.what());
        return 1;
    }
}
