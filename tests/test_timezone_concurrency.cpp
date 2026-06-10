/*
 * test_timezone_concurrency.cpp — regression guard for finding O6
 * (docs/production-readiness-findings.md): the public docs promise that
 * multi-threaded harnesses on different chart timezones are safe
 * (pineforge.h, strategy_set_chart_timezone: "process-global mutex so
 * multi-threaded harnesses don't corrupt each other's wall time").
 *
 * Pre-fix, pine_tz::ScopedTimezone released the mutex at the END of its
 * CONSTRUCTOR, so the caller's localtime_r/mktime decomposition ran
 * unlocked — two threads on different timezones could decompose under the
 * wrong TZ. Post-fix the lock is held for the guard's full RAII scope.
 *
 * Strategy: compute single-threaded baselines for several (timestamp, tz)
 * pairs through the public TZ-dependent helpers, then hammer the same
 * helpers from many threads on DIFFERENT timezones and require every
 * result to equal its serial baseline. A race is timing-dependent, so a
 * pass is not an airtight proof — but with the constructor-unlock bug in
 * place this test fails within milliseconds on every machine we tried,
 * and it can never false-positive (a failure is always a real wrong
 * decomposition).
 */

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <pineforge/session_time.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

struct Probe {
    int64_t bar_ms;
    std::string tz;
    int64_t expected_day_open_ms = 0;  // filled by serial baseline
    bool expected_in_session = false;  // 0930-1600 filter, filled serially
};

// Timestamps chosen so the day-open differs across all three zones
// (mid-UTC-day) and one lands near a DST boundary in America/New_York.
std::vector<Probe> make_probes() {
    return {
        {1743391800000LL, "Asia/Taipei"},        // 2025-03-31 03:30 UTC
        {1743391800000LL, "America/New_York"},
        {1743391800000LL, "UTC"},
        {1741514400000LL, "America/New_York"},   // 2025-03-09 (US spring-forward)
        {1741514400000LL, "Asia/Taipei"},
        {1735689600000LL, "Europe/London"},      // 2025-01-01 00:00 UTC
        {1735689600000LL, "Pacific/Auckland"},
    };
}

}  // namespace

static void test_concurrent_decomposition_matches_serial_baseline() {
    std::printf("test_concurrent_decomposition_matches_serial_baseline\n");

    std::vector<Probe> probes = make_probes();

    // Serial baselines (single thread — trivially race-free).
    for (auto& p : probes) {
        p.expected_day_open_ms = calendar_day_open_local_ms(p.bar_ms, p.tz);
        p.expected_in_session = passes_session_filter("0930-1600", p.tz, p.bar_ms);
    }

    // Sanity: the zones genuinely disagree, otherwise a TZ race would be
    // invisible to this test.
    CHECK(probes[0].expected_day_open_ms != probes[1].expected_day_open_ms);
    CHECK(probes[1].expected_day_open_ms != probes[2].expected_day_open_ms);

    constexpr int kThreads = 8;
    constexpr int kIters = 4000;
    std::atomic<int> mismatches{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < kIters; ++i) {
                // Stagger the probe order per thread to maximise
                // different-TZ interleavings.
                const Probe& p = probes[(i + t) % probes.size()];
                int64_t got = calendar_day_open_local_ms(p.bar_ms, p.tz);
                if (got != p.expected_day_open_ms) mismatches.fetch_add(1);
                bool in = passes_session_filter("0930-1600", p.tz, p.bar_ms);
                if (in != p.expected_in_session) mismatches.fetch_add(1);
            }
        });
    }
    for (auto& w : workers) w.join();

    CHECK(mismatches.load() == 0);
}

int main() {
    test_concurrent_decomposition_matches_serial_baseline();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
