// R5 lane PERF-P5611, item P11. The language time built-ins (hour(),
// minute(), dayofweek(), ... and the session filters) decompose a stamp in a
// timezone without per-call libc: a zone that normalizes to UTC by integer
// civil arithmetic, any other zone from the calling thread's memoised offset
// intervals, whose every edge is a localtime_r answer
// (src/session_time.cpp, decompose_ms_local).
//
// Until this lane each call was gmtime_r, or localtime_r under the zone's
// process-global ScopedTimezone (a mutex, the normalization strings and, on a
// zone switch, setenv + tzset). The decomposition may not move a single
// field, so this row compares it with what that pre-lane body answers (below:
// the same libc calls, the zone's guard held across a batch of stamps) field by
// field -- the nine ISO fields, tm_gmtoff and the tm_zone text:
//
//   * every UTC spelling that normalizes to UTC, hourly 1970-2100 for the
//     spellings the engine passes ("" and "UTC"), daily for the rest, plus
//     both sides of the +/-2^40-second arithmetic edge;
//   * every transition 2000-2100 of 38 zones chosen for their shapes
//     (southern and negative DST, half- and quarter-hour offsets, 30-minute
//     DST, midnight transitions, Ramadan suspensions, a dateline jump, zones
//     that stopped observing DST): the seconds around it, then a 72-hour walk
//     across it at 15 minutes forward, and at 15 minutes and 7 seconds back;
//   * every minute of the corpus feed's span (ohlcv_ETH-USDT-USDT_1m.csv,
//     2020-01-01T00:00Z .. 2026-05-04T15:14Z) -- so every bar stamp of every
//     derived feed -- in the zones corpus scripts name (America/New_York,
//     Asia/Taipei, Europe/London, Asia/Tokyo) and in UTC;
//   * seeded random stamps 1900-2200 per zone in random order, and ten zones
//     read in turn on one thread (more zones than the memo holds);
//   * fixed-offset spellings, which normalize to a POSIX offset.
//
// Then the UTC calendar paths the lane moved off libc, against their pre-lane
// bodies: the month open (gmtime_r) and the D / W / M period close (localtime_r
// + mktime under ScopedTimezone("UTC")), through timeframe_time /
// timeframe_time_close. (Under AddressSanitizer the bulk sweeps read every 15th
// stamp; see kBulkStride.)
#include <pineforge/session_time.hpp>

#include "../src/timezone.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

namespace pineforge {
// src/session_time.cpp. External linkage only so this row can compare it with
// libc; not part of the installed API.
void decompose_ms_local(int64_t bar_ms, const std::string& tz, struct tm& out);
}  // namespace pineforge

namespace {
using namespace pineforge;

int failures = 0;
long long checks = 0;

// What decompose_ms_local answered before this lane: gmtime_r for a zone that
// normalizes to UTC, otherwise localtime_r under the zone's ScopedTimezone. The
// pre-lane body took the guard per call; here it is held across a batch of
// stamps, which changes nothing localtime_r answers (the process TZ is the
// zone's for every stamp either way) and keeps the reference affordable under
// the sanitizer profiles. tm_zone is copied at once: it may point into libc's
// zone state, which the next zone switch rewrites.
struct Expected {
    struct tm fields;
    char zone[64];
};

std::vector<Expected> libc_answers(const std::string& tz, const std::vector<int64_t>& stamps) {
    std::vector<Expected> out(stamps.size());
    const std::string t = normalize_timezone_for_posix(tz);
    const bool utc = t.empty() || t == "UTC" || t == "Etc/UTC";
    const auto read = [&](std::size_t i) {
        time_t secs = static_cast<time_t>(stamps[i] / 1000);
        Expected& e = out[i];
        e.fields = {};
        if (utc) {
            gmtime_r(&secs, &e.fields);
        } else {
            localtime_r(&secs, &e.fields);
        }
        std::strncpy(e.zone, e.fields.tm_zone ? e.fields.tm_zone : "", sizeof e.zone - 1);
        e.zone[sizeof e.zone - 1] = '\0';
    };
    if (utc) {
        for (std::size_t i = 0; i < stamps.size(); ++i) read(i);
    } else {
        tz_util::ScopedTimezone guard(tz);
        for (std::size_t i = 0; i < stamps.size(); ++i) read(i);
    }
    return out;
}

void compare(const std::string& tz, int64_t bar_ms, const Expected& e) {
    struct tm got {};
    decompose_ms_local(bar_ms, tz, got);
    const char* got_zone = got.tm_zone != nullptr ? got.tm_zone : "";
    const struct tm& expected = e.fields;
    ++checks;
    const bool same = expected.tm_sec == got.tm_sec && expected.tm_min == got.tm_min
        && expected.tm_hour == got.tm_hour && expected.tm_mday == got.tm_mday
        && expected.tm_mon == got.tm_mon && expected.tm_year == got.tm_year
        && expected.tm_wday == got.tm_wday && expected.tm_yday == got.tm_yday
        && expected.tm_isdst == got.tm_isdst && expected.tm_gmtoff == got.tm_gmtoff
        && (expected.tm_zone == nullptr) == (got.tm_zone == nullptr)
        && std::strcmp(e.zone, got_zone) == 0;
    if (!same && ++failures <= 20) {
        std::fprintf(stderr,
                     "FAIL tz=\"%s\" ms=%lld expected %04d-%02d-%02d %02d:%02d:%02d wday %d yday %d "
                     "dst %d off %ld %s, got %04d-%02d-%02d %02d:%02d:%02d wday %d yday %d dst %d "
                     "off %ld %s\n",
                     tz.c_str(), static_cast<long long>(bar_ms), expected.tm_year + 1900,
                     expected.tm_mon + 1, expected.tm_mday, expected.tm_hour, expected.tm_min,
                     expected.tm_sec, expected.tm_wday, expected.tm_yday, expected.tm_isdst,
                     static_cast<long>(expected.tm_gmtoff), e.zone, got.tm_year + 1900,
                     got.tm_mon + 1, got.tm_mday, got.tm_hour, got.tm_min, got.tm_sec, got.tm_wday,
                     got.tm_yday, got.tm_isdst, static_cast<long>(got.tm_gmtoff), got_zone);
    }
}

// libc's answers for `stamps`, then the decomposition of each, in their order
// (the order is part of the test: it walks the memo forward, back and at random).
void check_stamps(const std::string& tz, const std::vector<int64_t>& stamps) {
    const auto expected = libc_answers(tz, stamps);
    for (std::size_t i = 0; i < stamps.size(); ++i) compare(tz, stamps[i], expected[i]);
}

void check_fields(const std::string& tz, int64_t bar_ms) {
    check_stamps(tz, {bar_ms});
}

// Under AddressSanitizer every libc call and every decomposition is instrumented
// and the bulk sweeps (the UTC spellings, the corpus minutes, the calendar
// paths: 20 M reads) would take many minutes, so there they read every 15th
// stamp; the release and debug profiles read all of them. The transition,
// random, zone-turn and fixed-offset sections run whole everywhere.
#if defined(__SANITIZE_ADDRESS__)
constexpr int64_t kBulkStride = 15;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int64_t kBulkStride = 15;
#else
constexpr int64_t kBulkStride = 1;
#endif
#else
constexpr int64_t kBulkStride = 1;
#endif

constexpr int64_t kMinuteMs = 60LL * 1000;
constexpr int64_t kHourMs = 3600LL * 1000;
constexpr int64_t kDayMs = 86400LL * 1000;

std::uint64_t mix_state = 0x9e3779b97f4a7c15ull;
std::uint64_t next_random() {
    mix_state ^= mix_state << 13;
    mix_state ^= mix_state >> 7;
    mix_state ^= mix_state << 17;
    return mix_state;
}

void utc_spellings() {
    const int64_t end_2101 = 4133980800LL * 1000;
    for (const char* spelling : {"", "UTC"}) {
        std::vector<int64_t> stamps;
        for (int64_t hour = 0; hour * kHourMs < end_2101; hour += kBulkStride) {
            stamps.push_back(hour * kHourMs + static_cast<int64_t>(next_random() % kHourMs));
        }
        check_stamps(spelling, stamps);
    }
    for (const char* spelling : {"Etc/UTC", "GMT", "Etc/GMT", "GMT+0", "UTC-0", "UTC+00:00",
                                 "GMT+0000"}) {
        std::vector<int64_t> stamps;
        for (int64_t day = -25567; day * kDayMs < end_2101; day += kBulkStride) {  // 1900 ..
            stamps.push_back(day * kDayMs + static_cast<int64_t>(next_random() % kDayMs));
        }
        check_stamps(spelling, stamps);
    }
    const int64_t edge = int64_t{1} << 40;
    for (const char* spelling : {"", "UTC", "GMT+0"}) {
        for (int64_t secs : {edge - 1, edge, edge + 1, -edge - 1, -edge, -edge + 1}) {
            check_fields(spelling, secs * 1000);
            check_fields(spelling, secs * 1000 + (secs < 0 ? -999 : 999));
        }
    }
}

bool same_state(const struct tm& a, const struct tm& b) {
    return a.tm_gmtoff == b.tm_gmtoff && a.tm_isdst == b.tm_isdst && a.tm_zone != nullptr
        && b.tm_zone != nullptr && std::strcmp(a.tm_zone, b.tm_zone) == 0;
}

// Every state change of `tz` in [from, to): 6-hour libc samples, each change
// bisected to its exact second. Both samples are read under one guard.
std::vector<int64_t> transitions(const std::string& tz, int64_t from, int64_t to) {
    std::vector<int64_t> out;
    tz_util::ScopedTimezone guard(tz);
    auto state_at = [](int64_t secs, struct tm& t, char* zone) {
        const time_t s = static_cast<time_t>(secs);
        localtime_r(&s, &t);
        std::snprintf(zone, 64, "%s", t.tm_zone ? t.tm_zone : "");
        t.tm_zone = zone;
    };
    struct tm previous {};
    char previous_zone[64];
    state_at(from, previous, previous_zone);
    for (int64_t at = from + 6 * 3600; at < to; at += 6 * 3600) {
        struct tm now {};
        char now_zone[64];
        state_at(at, now, now_zone);
        if (!same_state(previous, now)) {
            int64_t inside = at - 6 * 3600, outside = at;
            while (outside - inside > 1) {
                const int64_t mid = inside + (outside - inside) / 2;
                struct tm probe {};
                char probe_zone[64];
                state_at(mid, probe, probe_zone);
                if (same_state(previous, probe))
                    inside = mid;
                else
                    outside = mid;
            }
            out.push_back(outside);
        }
        previous = now;
        std::memcpy(previous_zone, now_zone, sizeof previous_zone);
        previous.tm_zone = previous_zone;
    }
    return out;
}

const char* const kShapeZones[] = {
    "America/New_York", "America/Chicago", "America/Denver", "America/Los_Angeles",
    "America/Anchorage", "America/Halifax", "America/St_Johns", "America/Havana",
    "America/Sao_Paulo", "America/Santiago", "America/Asuncion", "America/Mexico_City",
    "America/Caracas", "Atlantic/Azores", "Europe/London", "Europe/Dublin", "Europe/Berlin",
    "Europe/Moscow", "Europe/Istanbul", "Africa/Casablanca", "Africa/Cairo", "Asia/Tokyo",
    "Asia/Taipei", "Asia/Kolkata", "Asia/Kathmandu", "Asia/Tehran", "Asia/Jerusalem",
    "Asia/Gaza", "Asia/Beirut", "Australia/Sydney", "Australia/Adelaide", "Australia/Lord_Howe",
    "Pacific/Auckland", "Pacific/Chatham", "Pacific/Apia", "Pacific/Kiritimati",
    "Antarctica/Troll", "America/Nuuk",
};

void every_transition_2000_to_2100(long long* transition_count) {
    const int64_t from = 946684800;   // 2000-01-01
    const int64_t to = 4133980800;    // 2101-01-01
    for (const char* zone : kShapeZones) {
        const std::string tz = zone;
        std::vector<int64_t> stamps;
        for (int64_t t : transitions(tz, from, to)) {
            ++*transition_count;
            for (int64_t d = -3; d <= 3; ++d) {
                stamps.push_back((t + d) * 1000);
                stamps.push_back((t + d) * 1000 + 999);
            }
            const int64_t grid = (t * 1000 / (15 * kMinuteMs)) * (15 * kMinuteMs);
            for (int64_t ms = grid - 36 * kHourMs; ms <= grid + 36 * kHourMs; ms += 15 * kMinuteMs) {
                stamps.push_back(ms);
            }
            for (int64_t ms = grid + 36 * kHourMs; ms >= grid - 36 * kHourMs;
                 ms -= 15 * kMinuteMs + 7000) {
                stamps.push_back(ms);
            }
        }
        check_stamps(tz, stamps);
    }
}

void every_minute_of_the_corpus_feed() {
    const int64_t first = 1577836800000LL;  // 2020-01-01T00:00:00Z, the feed's first bar
    const int64_t last = 1777907640000LL;   // 2026-05-04T15:14:00Z, its last
    for (const char* zone : {"America/New_York", "Asia/Taipei", "Europe/London", "Asia/Tokyo",
                             "UTC"}) {
        const std::string tz = zone;
        for (int64_t day = first; day <= last; day += kDayMs) {
            std::vector<int64_t> stamps;
            for (int64_t ms = day; ms < day + kDayMs && ms <= last; ms += kBulkStride * kMinuteMs) {
                stamps.push_back(ms);
            }
            check_stamps(tz, stamps);
        }
    }
}

void random_stamps_and_zone_turns() {
    const int64_t from = -2208988800LL * 1000;  // 1900-01-01
    const int64_t span = (7258118400LL + 2208988800LL) * 1000;  // .. 2200-01-01
    for (const char* zone : kShapeZones) {
        std::vector<int64_t> stamps;
        for (int i = 0; i < 4000; ++i) {
            stamps.push_back(from + static_cast<int64_t>(next_random() % static_cast<std::uint64_t>(span)));
        }
        check_stamps(zone, stamps);
    }
    // Ten zones in turn per bar on one thread: more zones than the memo holds,
    // so most reads miss and switch the process zone (kept short for that).
    const char* const turns[] = {"America/New_York", "Europe/London", "Asia/Tokyo", "Asia/Kolkata",
                                 "Australia/Sydney", "America/Sao_Paulo", "Europe/Berlin",
                                 "Pacific/Auckland", "Asia/Taipei", "America/Chicago"};
    std::vector<int64_t> bars;
    for (int64_t ms = 1583020800000LL; ms < 1583020800000LL + 300 * 15 * kMinuteMs;
         ms += 15 * kMinuteMs) {
        bars.push_back(ms);
    }
    std::vector<std::vector<Expected>> expected;
    for (const char* zone : turns) expected.push_back(libc_answers(zone, bars));
    for (std::size_t i = 0; i < bars.size(); ++i) {
        for (std::size_t z = 0; z < expected.size(); ++z) compare(turns[z], bars[i], expected[z][i]);
    }
}

void fixed_offsets() {
    for (const char* zone : {"UTC+2", "GMT-5", "UTC+05:30", "GMT+0545", "UTC-3:30", "GMT+14",
                             "UTC-12", "Etc/GMT+5", "Etc/GMT-14"}) {
        std::vector<int64_t> stamps;
        for (int64_t ms = 1577836800000LL; ms < 1777907640000LL; ms += 7 * kHourMs + 13 * kMinuteMs) {
            stamps.push_back(ms);
        }
        check_stamps(zone, stamps);
    }
}

// timeframe_time's month open and timeframe_time_close's D / W / M close in
// UTC, against their pre-lane bodies.
int64_t libc_month_open(int64_t bar_ms) {
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    int64_t days = static_cast<int64_t>(secs) / 86400;
    if (secs % 86400 != 0 && secs < 0) --days;
    struct tm g {};
    gmtime_r(&secs, &g);
    return (days - (g.tm_mday - 1)) * 86400000LL;
}

int64_t libc_period_close(int64_t open_ms, char period) {
    tz_util::ScopedTimezone guard("UTC");
    time_t osec = static_cast<time_t>(open_ms / 1000);
    struct tm local_tm {};
    localtime_r(&osec, &local_tm);
    if (period == 'D') local_tm.tm_mday += 1;
    if (period == 'W') local_tm.tm_mday += 7;
    if (period == 'M') {
        local_tm.tm_mon += 1;
        local_tm.tm_mday = 1;
    }
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    time_t nx = mktime(&local_tm);
    return static_cast<int64_t>(nx) * 1000 - 1;
}

void check_value(const char* what, int64_t bar_ms, int64_t expected, int64_t got) {
    ++checks;
    if (expected != got && ++failures <= 20) {
        std::fprintf(stderr, "FAIL %s ms=%lld expected %lld got %lld\n", what,
                     static_cast<long long>(bar_ms), static_cast<long long>(expected),
                     static_cast<long long>(got));
    }
}

void utc_calendar_paths() {
    for (int64_t ms = -2208988800LL * 1000; ms < 4133980800LL * 1000;
         ms += kBulkStride * (7 * kHourMs + 7 * kMinuteMs + 1001)) {
        check_value("month open", ms, libc_month_open(ms),
                    timeframe_time(ms, "M", "", "UTC", "15"));
        const int64_t day_open = timeframe_time(ms, "D", "", "UTC", "15");
        const int64_t week_open = timeframe_time(ms, "W", "", "UTC", "15");
        const int64_t month_open = timeframe_time(ms, "M", "", "UTC", "15");
        check_value("D close", ms, libc_period_close(day_open, 'D'),
                    timeframe_time_close(ms, "D", "", "UTC", "15"));
        check_value("W close", ms, libc_period_close(week_open, 'W'),
                    timeframe_time_close(ms, "W", "", "", "15"));
        check_value("M close", ms, libc_period_close(month_open, 'M'),
                    timeframe_time_close(ms, "M", "", "GMT+0", "15"));
    }
}

}  // namespace

int main() {
    long long transition_count = 0;
    utc_spellings();
    every_transition_2000_to_2100(&transition_count);
    every_minute_of_the_corpus_feed();
    random_stamps_and_zone_turns();
    fixed_offsets();
    utc_calendar_paths();
    if (transition_count < 2000) {
        std::fprintf(stderr, "FAIL too few transitions found: %lld\n", transition_count);
        ++failures;
    }
    if (failures == 0) {
        std::printf("test_local_time_fields: ok (%lld comparisons, %lld transitions)\n", checks,
                    transition_count);
    }
    return failures == 0 ? 0 : 1;
}
