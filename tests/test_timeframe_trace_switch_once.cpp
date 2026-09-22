// R5 lane F6 item 3: the calendar aggregator reads its diagnostic switch once.
//
// src/timeframe.cpp carries a stderr diagnostic behind the PF_SEC_TRACE
// environment variable. Until this lane both of its sites asked the
// environment on every call -- `getenv("PF_SEC_TRACE")` inside
// crosses_boundary() and again at the top of the calendar-mode feed -- and
// those two run once per input bar of every calendar bucket (a D/W/M chart
// period, a request.security or subscription series over a finer feed). A
// process-wide environment lookup per bar is a hot-path cost with no reader
// that needs it fresh: the switch is a process-start setting.
//
// The change resolves the switch once per process, on first use. What a
// caller can observe is exactly that: once the aggregator has run, changing
// the environment no longer changes what it prints. That is this witness.
//
//   1. The switch unset, run a D aggregation over hourly bars and the
//      boundary predicate: nothing reaches stderr.
//   2. Set PF_SEC_TRACE, run the same work again. Resolved once, the switch
//      is still off: nothing reaches stderr. (Fail-before at fd785928: every
//      calendar feed printed a "[calmode] ts=..." line and every sub-hour
//      boundary test an "[xsb] prev=..." line, because each call re-read
//      the environment.)
//   3. The two runs aggregated the same bars into the same buckets, bit for
//      bit: the switch never touches a result, and neither did its removal
//      from the hot path.
//
// Source-free: it links the kernel and nothing else.
#include <pineforge/bar.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                      \
    ++checks;                                                                 \
    if (!(expr)) {                                                            \
        ++failures;                                                           \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);            \
    }                                                                         \
} while (false)

constexpr std::int64_t kHourMs = 3600000;
constexpr std::int64_t kT0 = 1735689600000LL;  // 2025-01-01 00:00 UTC

struct Run {
    std::vector<Bar> completed;
    int boundaries = 0;
};

// Everything the diagnostic sits on: the calendar-mode feed (one D bucket
// out of every 24 hourly bars, three days) and the boundary predicate over
// consecutive hourly stamps (the sub-hour-gap case the trace prints for).
Run workload() {
    Run run;
    TimeframeAggregator daily("D", "60");
    for (int i = 0; i < 72; ++i) {
        const double base = 100.0 + 0.25 * i;
        const Bar bar{base, base + 1.0, base - 1.0, base + 0.5, 10.0 + i,
                      kT0 + i * kHourMs};
        const AggregatedBar out = daily.feed(bar);
        if (out.is_complete) run.completed.push_back(out.bar);
        if (i > 0 && crosses_boundary(kT0 + (i - 1) * kHourMs, kT0 + i * kHourMs,
                                      CalendarPeriod::DAY, "UTC", "")) {
            ++run.boundaries;
        }
    }
    return run;
}

// stderr redirected into a temporary file for the duration of `body`;
// returns what was written there.
template <class Body>
std::string captured_stderr(Body body) {
    std::fflush(stderr);
    std::FILE* sink = std::tmpfile();
    if (!sink) return "<tmpfile failed>";
    const int saved = dup(fileno(stderr));
    dup2(fileno(sink), fileno(stderr));
    body();
    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    std::string text;
    std::rewind(sink);
    char buffer[512];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, sink)) > 0) text.append(buffer, got);
    std::fclose(sink);
    return text;
}

int lines(const std::string& text) {
    int count = 0;
    for (char c : text) count += c == '\n';
    return count;
}

bool same_bars(const std::vector<Bar>& a, const std::vector<Bar>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i].open, &b[i].open, sizeof(double)) != 0
            || std::memcmp(&a[i].high, &b[i].high, sizeof(double)) != 0
            || std::memcmp(&a[i].low, &b[i].low, sizeof(double)) != 0
            || std::memcmp(&a[i].close, &b[i].close, sizeof(double)) != 0
            || std::memcmp(&a[i].volume, &b[i].volume, sizeof(double)) != 0
            || a[i].timestamp != b[i].timestamp) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    unsetenv("PF_SEC_TRACE");
    Run unset_run;
    const std::string unset_err = captured_stderr([&] { unset_run = workload(); });
    std::printf("switch unset:           %zu completed buckets, %d boundaries, "
                "%d stderr lines\n",
                unset_run.completed.size(), unset_run.boundaries, lines(unset_err));
    CHECK(unset_err.empty());
    // Each day completes on its own last hourly bar, whose end reaches the
    // period's close: three buckets for three days.
    CHECK(unset_run.completed.size() == 3);
    CHECK(unset_run.boundaries == 2);

    // After first use the switch is resolved: a later change of the
    // environment is not read again on the per-bar path.
    setenv("PF_SEC_TRACE", "1", 1);
    Run set_run;
    const std::string set_err = captured_stderr([&] { set_run = workload(); });
    std::printf("switch set after use:   %zu completed buckets, %d boundaries, "
                "%d stderr lines\n",
                set_run.completed.size(), set_run.boundaries, lines(set_err));
    if (!set_err.empty()) {
        std::printf("first stderr line: %.*s\n",
                    static_cast<int>(set_err.find('\n')), set_err.c_str());
    }
    CHECK(set_err.empty());
    unsetenv("PF_SEC_TRACE");

    CHECK(set_run.boundaries == unset_run.boundaries);
    CHECK(same_bars(set_run.completed, unset_run.completed));
    if (unset_run.completed.size() == 3) {
        CHECK(unset_run.completed[0].timestamp == kT0);
        CHECK(unset_run.completed[1].timestamp == kT0 + 24 * kHourMs);
        CHECK(unset_run.completed[2].timestamp == kT0 + 48 * kHourMs);
        CHECK(unset_run.completed[0].open == 100.0);
        CHECK(unset_run.completed[0].close == 100.0 + 0.25 * 23 + 0.5);
    }

    std::printf("test_timeframe_trace_switch_once: %d passed, %d failed\n",
                checks - failures, failures);
    return failures ? 1 : 0;
}
