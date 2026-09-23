// R5 lane PERF-P5611, item P5. The Pine adapter's chart-day key reads the UTC
// day and month by integer civil-from-days arithmetic instead of one
// gmtime_r call per bar.
//
// PineExecutionAdapter::chart_day_key answers tm_mday * 100 + tm_mon + 1 for
// the stamp's whole seconds (the millisecond stamp divided by 1000, truncated
// toward zero). The intraday day ledger reads it on every bar whether or not
// a loss or fill cap is declared, and the cap clock on every cap read; with
// month_key_utc it was the 59 ns/bar PERF0-P measured in libc. On a UTC chart
// (no chart timezone, "UTC" or "Etc/UTC") the arithmetic answers every second
// within +/-2^40 of the epoch and gmtime_r keeps the rest; a chart timezone
// keeps its localtime_r path untouched.
//
// The witness is the key against libc through the host's own fixture read,
// for each UTC spelling: every hour from 1970-01-01 to 2100-12-31 with a
// pseudo-random millisecond inside it, every day of years 1-1969 and
// 2101-9999 (every 15th under AddressSanitizer), both sides of every day
// boundary 1969-2101, the truncation of a
// negative millisecond, and both sides of the +/-2^40-second fallback edge.
// The references below are the pre-lane bodies verbatim; the New York rows
// show the zoned path answering what localtime_r does, as before.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/timezone.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <string>

namespace {
using namespace pineforge;

int failures = 0;
long long checks = 0;

class DayKeyHost final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
    // set_chart_timezone reaches the adapter at the next begin.
    void stage_chart_timezone(const std::string& timezone) {
        set_chart_timezone(timezone);
        const Bar bars[] = {
            {100.0, 100.0, 100.0, 100.0, 1.0, 0},
            {100.0, 100.0, 100.0, 100.0, 1.0, 60'000},
        };
        run(bars, 2);
    }
    std::int64_t day_key(std::int64_t timestamp_ms) const noexcept {
        return fixture_chart_day_key(timestamp_ms);
    }
};

// chart_day_key's UTC body before this lane.
std::int64_t libc_utc_day_key(std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm fields{};
    if (::gmtime_r(&seconds, &fields) == nullptr) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(fields.tm_mday) * 100
        + static_cast<std::int64_t>(fields.tm_mon + 1);
}

// chart_day_key's zoned body before this lane (a chart timezone that parses).
std::int64_t libc_zoned_day_key(std::int64_t timestamp_ms, const std::string& timezone) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm fields{};
    {
        tz_util::ScopedTimezone guard(timezone);
        if (::localtime_r(&seconds, &fields) == nullptr) return libc_utc_day_key(timestamp_ms);
    }
    return static_cast<std::int64_t>(fields.tm_mday) * 100
        + static_cast<std::int64_t>(fields.tm_mon + 1);
}

void check_key(const char* label, std::int64_t ts_ms, std::int64_t expected, std::int64_t got) {
    ++checks;
    if (expected == got) return;
    if (++failures <= 20) {
        std::fprintf(stderr, "FAIL %s ts_ms=%lld expected %lld got %lld\n", label,
                     static_cast<long long>(ts_ms), static_cast<long long>(expected),
                     static_cast<long long>(got));
    }
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

void sweep_utc_spelling(const char* spelling) {
    DayKeyHost host;
    if (spelling[0] != '\0') host.stage_chart_timezone(spelling);
    const auto at = [&](std::int64_t ts_ms) {
        check_key(spelling, ts_ms, libc_utc_day_key(ts_ms), host.day_key(ts_ms));
    };
    // Every hour 1970-01-01 .. 2100-12-31, a pseudo-random millisecond in it.
    const std::int64_t end_2101 = 4133980800LL * 1000;
    std::uint64_t mix = 0x2545f4914f6cdd1dull;
    for (std::int64_t hour = 0; hour * kHourMs < end_2101; ++hour) {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        at(hour * kHourMs + static_cast<std::int64_t>(mix % kHourMs));
    }
    // Both sides of every day boundary 1969 .. 2101, negative stamps included.
    for (std::int64_t day = -365; day * kDayMs <= end_2101 + 365 * kDayMs; ++day) {
        for (std::int64_t delta : {-1001LL, -1000LL, -1LL, 0LL, 1LL, 999LL, 1000LL}) {
            at(day * kDayMs + delta);
        }
    }
    // Every day of years 1..1969 and 2101..9999.
    const std::int64_t first = -62135596800LL / 86400;  // 0001-01-01
    const std::int64_t last = 253402300799LL / 86400;   // 9999-12-31
    for (std::int64_t day = first; day <= last; day += kFarStride) {
        if (day >= -365 && day * kDayMs <= end_2101 + 365 * kDayMs) continue;
        at(day * kDayMs + 43200LL * 1000);
    }
    // Both sides of the +/-2^40-second fallback edge.
    const std::int64_t edge = std::int64_t{1} << 40;
    for (std::int64_t secs : {edge - 1, edge, edge + 1, -edge - 1, -edge, -edge + 1,
                              edge * 2, -edge * 2}) {
        at(secs * 1000);
        at(secs * 1000 + (secs < 0 ? -999 : 999));
    }
}

void zoned_chart_keeps_localtime() {
    const std::string zone = "America/New_York";
    DayKeyHost host;
    host.stage_chart_timezone(zone);
    // 2020-2026 every six hours: two DST edges a year, midnight on both sides.
    for (std::int64_t ts = 1577836800000LL; ts < 1798761600000LL; ts += 6 * kHourMs + 60000) {
        check_key("America/New_York", ts, libc_zoned_day_key(ts, zone),
                  host.day_key(ts));
    }
}

}  // namespace

int main() {
    for (const char* spelling : {"", "UTC", "Etc/UTC"}) sweep_utc_spelling(spelling);
    zoned_chart_keeps_localtime();
    if (checks < 3000000) {
        std::fprintf(stderr, "FAIL too few checks: %lld\n", checks);
        ++failures;
    }
    if (failures == 0) {
        std::printf("test_chart_day_key_arithmetic: ok (%lld keys against libc)\n", checks);
    }
    return failures == 0 ? 0 : 1;
}
