#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "report_event_trace_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
using namespace event_trace_fixture;
std::size_t callback_peak = 0;

void observe(Host&) {
    callback_peak = std::max(callback_peak, global_allocation::live_bytes);
}

struct Sample { std::size_t retained, peak, trace_capacity; };

Sample sample(const std::vector<Bar>& bars, Route route, bool enabled,
              bool reuse_full, const Directory& directory) {
    require(global_allocation::live_bytes == 0, "allocation window was not empty");
    const auto path = directory.path + "/memory.ndjson";
    callback_peak = 0;
    global_allocation::recording = true;
    Sample result{};
    {
        Host host;
        if (reuse_full) {
            configure(host, Route::Full, true, "", directory.path);
            run(host, bars);
            Envelope previous;
            capture(host, previous, static_cast<int>(bars.size()), true);
            require(previous.value.report.trace_len == static_cast<int>(bars.size())
                        && previous.value.report.trace_names_len == static_cast<int>(bars.size())
                        && host.output_events_len() == static_cast<int>(bars.size()),
                    "FULL warmup did not populate records and distinct names");
        }
        configure(host, route, enabled, path, directory.path, reuse_full ? 2 : 1);
        host.observe = observe;
        run(host, bars);
        Envelope report;
        capture(host, report, static_cast<int>(bars.size()), enabled);
        if (route == Route::Sink) {
            const auto got = receipt(host);
            require(got.complete == 1 && got.count_x == (enabled ? bars.size() : 0)
                        && got.count_v == (enabled ? bars.size() : 0),
                    "sink memory fixture did not emit its nonzero enabled population");
        }
        result = {global_allocation::live_bytes, callback_peak, host.trace_capacity_bytes()};
    }
    require(global_allocation::live_bytes == 0, "event/trace fixture leaked counted storage");
    global_allocation::recording = false;
    return result;
}

void growth(Route route, bool reuse_full, const Directory& directory) {
    std::array<std::int64_t, 2> retained{}, peak{}, trace_capacity{};
    int populations = 0;
    for (const int count : {2000, 8000}) {
        const auto bars = feed(count); // Immutable input is outside the allocation window.
        const auto off = sample(bars, route, false, false, directory);
        const auto on = sample(bars, route, true, reuse_full, directory);
        retained[populations] = static_cast<std::int64_t>(on.retained) - off.retained;
        peak[populations] = static_cast<std::int64_t>(on.peak) - off.peak;
        trace_capacity[populations] = static_cast<std::int64_t>(on.trace_capacity)
            - off.trace_capacity;
        ++populations;
    }
    require(populations == 2, "both memory populations must run");
    std::printf("event/trace route=%d reuse=%d retained=%lld/%lld peak=%lld/%lld"
                " trace_capacity=%lld/%lld populations=2000/8000 slack=4096\n",
                static_cast<int>(route), reuse_full,
                static_cast<long long>(retained[0]), static_cast<long long>(retained[1]),
                static_cast<long long>(peak[0]), static_cast<long long>(peak[1]),
                static_cast<long long>(trace_capacity[0]),
                static_cast<long long>(trace_capacity[1]));
    require(retained[1] <= retained[0] + 4096,
            "retained event/trace bytes grow with the population");
    require(peak[1] <= peak[0] + 4096,
            "callback event/trace resident bytes grow with the population");
    require(trace_capacity[1] <= trace_capacity[0] + 4096,
            "retained trace/name capacity grows with the previous FULL population");
}
} // namespace

int main() {
    try {
        Directory directory;
        // Fresh sinks/optional summaries already pass on the previous tree.
        // Explicit summary retains all optional records there; reused sinks
        // retain the previous FULL name-vector and hash-bucket capacities.
        growth(Route::Summary, false, directory);
        growth(Route::Sink, false, directory);
        growth(Route::OptionalSummary, false, directory);
        growth(Route::Sink, true, directory);
        growth(Route::Summary, true, directory);
        growth(Route::OptionalSummary, true, directory);
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
