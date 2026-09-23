// R5 lane PERF-P5611, item P5. The monthly Sharpe / Sortino walk keys every
// equity point by its UTC (year, month) with integer civil-from-days
// arithmetic instead of one gmtime_r call per point.
//
// month_key_utc (src/engine_metrics.cpp) answers gmtime_r's
// (tm_year + 1900) * 12 + tm_mon for the point's whole seconds -- the
// millisecond stamp divided by 1000, truncated toward zero, exactly as it
// always read it. PERF0-P measured the libc call, with the chart-day key's,
// at 59 ns per bar on every run: the walk visits every point of the equity
// curve. The arithmetic answers every second within +/-2^40 of the epoch
// (about 34,800 years each way) and hands the rest back to gmtime_r, so the
// key may not move anywhere.
//
// The witness is the key itself against libc: every hour from 1970-01-01 to
// 2100-12-31 with a pseudo-random millisecond inside it, every day from year
// 1 to year 9999 (every 15th under AddressSanitizer; pre-epoch stamps
// included, with the truncation of a negative millisecond), the seconds and
// milliseconds on both sides of every month boundary 1900-2100, and both
// sides of the +/-2^40-second fallback edge. The reference below is the
// pre-lane body of month_key_utc verbatim.
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>

namespace pineforge::metrics::detail {
// src/engine_metrics.cpp. External linkage only so this row can reach it;
// not part of the installed API.
int month_key_utc(std::int64_t ts_ms);
}  // namespace pineforge::metrics::detail

namespace {

int failures = 0;
long long checks = 0;

void check_key(std::int64_t ts_ms, int expected, int got) {
    ++checks;
    if (expected == got) return;
    if (++failures <= 20) {
        std::fprintf(stderr, "FAIL ts_ms=%lld expected %d got %d\n",
                     static_cast<long long>(ts_ms), expected, got);
    }
}

// The body month_key_utc had before this lane: gmtime_r on the truncated
// seconds, its return value unchecked.
int libc_month_key(std::int64_t ts_ms) {
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tb {};
    gmtime_r(&secs, &tb);
    return (tb.tm_year + 1900) * 12 + tb.tm_mon;
}

void check_at(std::int64_t ts_ms) {
    check_key(ts_ms, libc_month_key(ts_ms), pineforge::metrics::detail::month_key_utc(ts_ms));
}

// Under AddressSanitizer the year 1-9999 sweep reads every 15th day (every libc
// call is instrumented there); every other profile reads every day. The
// 1970-2100 sweeps run whole everywhere.
#if defined(__SANITIZE_ADDRESS__)
constexpr std::int64_t kFarStride = 15;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr std::int64_t kFarStride = 15;
#else
constexpr std::int64_t kFarStride = 1;
#endif
#else
constexpr std::int64_t kFarStride = 1;
#endif

constexpr std::int64_t kHourMs = 3600LL * 1000;
constexpr std::int64_t kDayMs = 86400LL * 1000;

void every_hour_1970_to_2100() {
    const std::int64_t end = 4133980800LL * 1000;  // 2101-01-01T00:00:00Z
    std::uint64_t mix = 0x9e3779b97f4a7c15ull;
    for (std::int64_t hour = 0; hour * kHourMs < end; ++hour) {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        check_at(hour * kHourMs + static_cast<std::int64_t>(mix % kHourMs));
    }
}

void every_day_year_1_to_9999() {
    const std::int64_t first = -62135596800LL;  // 0001-01-01T00:00:00Z
    const std::int64_t last = 253402300799LL;   // 9999-12-31T23:59:59Z
    for (std::int64_t day = first / 86400; day * 86400 <= last; day += kFarStride) {
        const std::int64_t ms = day * kDayMs;
        check_at(ms);
        check_at(ms - 1);          // a negative stamp truncates toward zero
        check_at(ms + kDayMs - 1);
    }
}

// 00:00:00 UTC of (year, month) by the same civil formula libc applies, used
// only to place probes; the probes are judged against gmtime_r.
std::int64_t month_open_seconds(int year, int month) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t yoe = year - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400;
}

void every_month_boundary_1900_to_2100() {
    for (int year = 1900; year <= 2100; ++year) {
        for (int month = 1; month <= 12; ++month) {
            const std::int64_t ms = month_open_seconds(year, month) * 1000;
            for (std::int64_t delta : {-1001LL, -1000LL, -999LL, -1LL, 0LL, 1LL, 999LL, 1000LL}) {
                check_at(ms + delta);
            }
        }
    }
}

void both_sides_of_the_fallback_edge() {
    const std::int64_t edge = std::int64_t{1} << 40;
    for (std::int64_t secs : {edge - 2, edge - 1, edge, edge + 1, -edge - 1, -edge,
                              -edge + 1, -edge + 2, edge * 2, -edge * 2, edge * 64,
                              -edge * 64}) {
        for (std::int64_t ms : {0LL, 1LL, 999LL}) {
            check_at(secs * 1000 + (secs < 0 ? -ms : ms));
        }
    }
}

}  // namespace

int main() {
    every_hour_1970_to_2100();
    every_day_year_1_to_9999();
    every_month_boundary_1900_to_2100();
    both_sides_of_the_fallback_edge();
    if (checks < 4000000 / kFarStride) {
        std::fprintf(stderr, "FAIL too few checks: %lld\n", checks);
        ++failures;
    }
    if (failures == 0) {
        std::printf("test_utc_month_key_arithmetic: ok (%lld keys against gmtime_r)\n", checks);
    }
    return failures == 0 ? 0 : 1;
}
