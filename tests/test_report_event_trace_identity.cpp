#include "report_event_trace_fixture.hpp"

#include <pineforge/version.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>

namespace {
using namespace event_trace_fixture;

// Independent spelling of the documented ordered semantic witnesses. All
// inputs below come from the script formula, not a report row or sink parser.
struct Digest {
    std::uint64_t value = 0;
    void byte(unsigned char part) { value = (value ^ part) * UINT64_C(1099511628211); }
    void word(std::uint64_t part) {
        for (int shift = 0; shift != 64; shift += 8)
            byte(static_cast<unsigned char>(part >> shift));
    }
    void real(double part) {
        std::uint64_t representation = 0;
        std::memcpy(&representation, &part, sizeof part);
        word(representation);
    }
    void text(const std::string& part) {
        word(part.size());
        for (const unsigned char character : part) byte(character);
    }
};

std::uint64_t message_hash(const std::string& text) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char character : text)
        hash = (hash ^ character) * UINT64_C(1099511628211);
    return hash;
}

std::array<std::uint64_t, 3> expected(int count, bool recalculate) {
    Digest rows, events, traces;
    for (int index = 0; index < count; ++index) {
        const auto open = start_ms + index * step_ms;
        const auto close = recalculate
            ? (index % 2 == 0 ? INT64_MIN : open + step_ms + 1) : open + step_ms;
        rows.word(index); rows.word(open); rows.word(close); rows.word(1);
        rows.real(value(index));
        events.word(index + 1); events.word(0); events.word(index);
        events.word(open); events.word(close);
        events.word(0); events.word(0); events.word(1);
        events.real(value(index));
        const bool present = index % 3 != 0;
        const auto text = index % 3 == 2 ? message(index) : "";
        events.word(present ? message_hash(text) : 0);
        events.word(present ? 1 : 0);
        if (present) events.text(text);
        // Native batch calculations occur at the one-minute bar's close.
        traces.word(open + step_ms); traces.word(index); traces.text(name(index));
        traces.real(value(index));
    }
    return {rows.value, events.value, traces.value};
}

void check_witness(const Host& host, int count, bool recalculate) {
    const auto wanted = expected(count, recalculate);
    const auto got = host.optional_sequence_summary();
    require(got.rows.count == static_cast<std::uint64_t>(count)
                && got.events.count == static_cast<std::uint64_t>(count)
                && got.traces.count == static_cast<std::uint64_t>(count),
            "ordered witnesses did not count the complete population");
    require(got.rows.digest == wanted[0] && got.events.digest == wanted[1]
                && got.traces.digest == wanted[2],
            "ordered witness bytes differ from the independent input formula");
}

void independent_wire(const std::string& path, int count, bool recalculate, bool clear,
                      const pf_report_sink_receipt_v1& got) {
    const auto oracle = (std::filesystem::path(__FILE__).parent_path()
        / "report_event_trace_wire.py").string();
    const auto population = std::to_string(count);
    const auto child = ::fork();
    require(child >= 0, "fork independent wire oracle");
    if (child == 0) {
        ::execlp("python3", "python3", "-B", oracle.c_str(), path.c_str(),
                 PINEFORGE_VERSION_STRING, population.c_str(), recalculate ? "1" : "0",
                 clear ? "1" : "0", got.sha256_hex, static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status)
                && WEXITSTATUS(status) == 0,
            "independent v1 wire bytes/counts/SHA oracle failed");
}

void identity(Route route, bool recalculate, bool clear, const Directory& directory) {
    constexpr int count = 503; // Exercises more than one 64 KiB writer window.
    const auto bars = feed(count);
    const auto path = directory.path + "/identity.ndjson";
    Host host;
    host.recalculate = recalculate;
    host.clear_events = clear;
    configure(host, route, true, path, directory.path);
    run(host, bars);
    std::string first_bytes;
    pf_report_sink_receipt_v1 first_receipt{};
    for (int read_index = 0; read_index < 2; ++read_index) {
        Envelope report;
        capture(host, report, count, true, route == Route::Full && clear ? 0 : count);
        const auto& payload = report.value.report;
        const auto& metadata = report.value.metadata;
        if (route == Route::Full) {
            require(payload.trace_len == count && payload.trace_names_len == count
                        && host.output_events_len() == (clear ? 0 : count),
                    "FULL changed its trace/name/event contents");
            for (int index = 0; index < count; ++index) {
                const auto& trace_entry = payload.trace[index];
                const auto trace_name = name(index);
                const auto wanted_value = value(index);
                require(trace_entry.timestamp == start_ms + (index + 1) * step_ms
                            && trace_entry.bar_index == index && trace_entry.name_id == index
                            && std::memcmp(&trace_entry.value, &wanted_value,
                                           sizeof wanted_value) == 0
                            && std::memcmp(payload.trace_names[index], trace_name.data(),
                                           trace_name.size()) == 0,
                        "FULL trace order, first-seen name IDs or value bits changed");
                if (!clear) {
                    const auto* event = host.output_event_at(index);
                    const auto open = start_ms + index * step_ms;
                    const auto close = recalculate
                        ? (index % 2 == 0 ? INT64_MIN : open + step_ms + 1) : open + step_ms;
                    const auto text = index % 3 == 2 ? message(index) : "";
                    require(event && event->sequence == static_cast<std::uint64_t>(index + 1)
                                && event->output_index == 0 && event->bar_index == index
                                && event->bar_open_ms == open && event->bar_close_ms == close
                                && event->ordinal_in_bar == 0 && event->phase == 0
                                && event->confirmed == 1
                                && event->has_message == (index % 3 != 0)
                                && event->message == text
                                && event->message_hash64
                                    == (index % 3 == 0 ? 0 : message_hash(text))
                                && std::memcmp(&event->value, &wanted_value,
                                               sizeof wanted_value) == 0,
                            "FULL event sequence, ordinal or original message changed");
                }
            }
        } else {
            require(payload.trace == nullptr && payload.trace_len == 0
                        && payload.trace_names == nullptr && payload.trace_names_len == 0
                        && host.trace_rows() == 0 && host.trace_labels() == 0,
                    "bounded report retained trace/name rows");
            check_witness(host, count, recalculate);
            require(metadata.trace_names.logical_count == 0, "bounded route retained name dictionary");
            if (route != Route::Sink)
                require(host.output_events_len() == 0, "summary retained an event queue");
        }
        if (route == Route::Sink) {
            const auto got = receipt(host);
            require(got.complete == 1 && got.count_e == count && got.count_t == 0
                        && got.count_o == count && got.count_v == count
                        && got.count_k == 1 && got.count_x == count && got.count_h == 0,
                    "sink receipt lost, duplicated or reordered a population");
            independent_wire(path, count, recalculate, clear, got);
            const auto bytes = event_trace_fixture::read(path);
            if (read_index == 0) { first_bytes = bytes; first_receipt = got; }
            else require(bytes == first_bytes
                             && std::memcmp(&got, &first_receipt, sizeof got) == 0,
                         "repeated report read appended records or changed the receipt");
        }
    }
}

void next_run_policy(const Directory& directory) {
    const auto bars = feed(17);
    Host host;
    configure(host, Route::Summary, true, "", directory.path);
    run(host, bars);
    check_witness(host, 17, false);
    require(host.set_optional_report_mode(report_sequence::ReportMode::LegacyArrays) == 0
                && host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED
                && host.set_report_sink_v1(nullptr) == 0,
            "configure subsequent FULL generation");
    // Idle configuration must not reset the frozen summary's witnesses.
    check_witness(host, 17, false);
    configure(host, Route::Full, true, "", directory.path, 2);
    run(host, bars);
    Envelope full;
    capture(host, full, 17, true);
    require(full.value.report.trace_len == 17 && full.value.report.trace_names_len == 17
                && host.output_events_len() == 17,
            "a summary generation changed subsequent FULL retention");
}
} // namespace

int main() {
    try {
        Directory directory;
        int cases = 0;
        for (const auto route : {Route::Full, Route::Summary, Route::OptionalSummary, Route::Sink})
            for (const bool recalculate : {false, true})
                for (const bool clear : {false, true}) {
                    identity(route, recalculate, clear, directory);
                    ++cases;
                }
        require(cases == 16, "all event/trace identity cases must run");
        next_run_policy(directory);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
