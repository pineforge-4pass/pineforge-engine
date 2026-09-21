/*
 * test_calendar_wm_open_utc_fastpath.cpp — KI-35 completion proof.
 *
 * calendar_week_open_local_ms / calendar_month_open_local_ms gained the same
 * UTC fast path calendar_day_open_local_ms has had since eab8676 (KI-35):
 * pure integer / gmtime_r arithmetic, no tz_util::ScopedTimezone, no tzset.
 * This test proves the fast path is BIT-EQUAL to the ScopedTimezone slow path
 * it bypasses. The slow-path bodies are replicated here verbatim (localtime_r
 * + mktime under an explicitly pinned process TZ — exactly what
 * ScopedTimezone pins, minus the mutex) because the *_tz slow-path functions
 * are file-local to session_time.cpp.
 *
 * Coverage:
 *   - dense sweep, every 3601 s (cycling ms offsets 0/1/499/999) over
 *     1969-01-01 .. 2027-01-01 UTC (~508k instants): spans pre-epoch
 *     negatives, every week/month/year boundary in the range, leap days —
 *     fast "W"/"M" opens via pine_time must equal the replicated slow path.
 *   - explicit boundary/DST instants (epoch ±1 ms, Sunday-23:59:59.999 /
 *     Monday-00:00:00.000 week edges, the Dec/Jan year-straddling week,
 *     Feb-29 leap edges, month first/last ms, US/EU DST transition instants).
 *   - absolute anchors (hand-computed Monday/first-of-month opens) so the
 *     comparison is not purely self-referential.
 *   - UTC-alias zones ("", "Etc/UTC", "GMT", "GMT+0") produce identical values.
 *   - non-UTC zones (fixed offset "UTC+2", IANA Asia/Kolkata) still take the
 *     untouched ScopedTimezone slow path with unchanged values.
 *   - the session predicates (passes_session_filter / is(pre|post)market),
 *     whose decomposition now routes through decompose_ms_local instead of an
 *     unconditional ScopedTimezone, are swept every 61 s across three full
 *     weeks for plain, overnight, day-filtered, and all-day sessions against
 *     a pinned-TZ replica of the old ScopedTimezone body — bit-equal booleans.
 */

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <pineforge/na.hpp>
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

// Build a Unix-ms for YYYY-MM-DD HH:MM:SS in UTC (timegm ignores TZ).
static int64_t utc_ms(int y, int m, int d, int hh, int mm, int ss) {
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    t.tm_isdst = 0;
    return static_cast<int64_t>(timegm(&t)) * 1000LL;
}

static void pin_tz(const std::string& posix_tz) {
    setenv("TZ", posix_tz.c_str(), 1);
    tzset();
}

// --- verbatim replicas of the session_time.cpp *_tz slow-path bodies, with
// --- the process TZ pinned by the caller instead of ScopedTimezone ----------

static int64_t slow_week_open_pinned(int64_t bar_ms) {
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    int wday = local_tm.tm_wday;  // 0=Sun
    int days_from_mon = (wday + 6) % 7;
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    local_tm.tm_mday -= days_from_mon;
    time_t wk0 = mktime(&local_tm);
    return static_cast<int64_t>(wk0) * 1000;
}

static int64_t slow_month_open_pinned(int64_t bar_ms) {
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    local_tm.tm_mday = 1;
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    time_t m0 = mktime(&local_tm);
    return static_cast<int64_t>(m0) * 1000;
}

// Fast path entry: pine_time with empty session routes straight to
// compute_tf_open_ms -> calendar_{week,month}_open_local_ms.
static int64_t fast_open(int64_t bar_ms, const char* tf, const char* tz) {
    return pine_time(bar_ms, tf, "", tz, "60");
}

static void check_instant(int64_t bar_ms, int64_t* mism_w, int64_t* mism_m) {
    int64_t fw = fast_open(bar_ms, "W", "UTC");
    int64_t sw = slow_week_open_pinned(bar_ms);
    if (fw != sw) {
        if (*mism_w < 5)
            std::printf("  WEEK mismatch at bar_ms=%lld: fast=%lld slow=%lld\n",
                        (long long)bar_ms, (long long)fw, (long long)sw);
        ++*mism_w;
    }
    int64_t fm = fast_open(bar_ms, "M", "UTC");
    int64_t sm = slow_month_open_pinned(bar_ms);
    if (fm != sm) {
        if (*mism_m < 5)
            std::printf("  MONTH mismatch at bar_ms=%lld: fast=%lld slow=%lld\n",
                        (long long)bar_ms, (long long)fm, (long long)sm);
        ++*mism_m;
    }
}

// ---------------------------------------------------------------------------
// Dense sweep 1969-01-01 .. 2027-01-01, every 3601 s, ms offsets cycling
// through {0, 1, 499, 999} so sub-second (and pre-1970 negative-truncation)
// bar_ms -> secs mapping is exercised on both paths.
// ---------------------------------------------------------------------------
static void test_utc_fast_vs_slow_dense_sweep() {
    std::printf("test_utc_fast_vs_slow_dense_sweep\n");
    pin_tz("UTC");  // == what ScopedTimezone("UTC") pins for the slow path

    const int64_t start_s = utc_ms(1969, 1, 1, 0, 0, 0) / 1000;   // -31536000
    const int64_t end_s   = utc_ms(2027, 1, 1, 0, 0, 0) / 1000;   // 1798761600
    const int64_t ms_off[4] = {0, 1, 499, 999};

    int64_t mism_w = 0, mism_m = 0, n = 0;
    for (int64_t s = start_s; s <= end_s; s += 3601) {
        check_instant(s * 1000 + ms_off[n & 3], &mism_w, &mism_m);
        ++n;
    }
    std::printf("  swept %lld instants (W+M each)\n", (long long)n);
    CHECK(n > 500000);
    CHECK(mism_w == 0);
    CHECK(mism_m == 0);
}

// ---------------------------------------------------------------------------
// Explicit boundary + DST-transition instants.
// ---------------------------------------------------------------------------
static void test_utc_fast_vs_slow_boundary_instants() {
    std::printf("test_utc_fast_vs_slow_boundary_instants\n");
    pin_tz("UTC");

    const int64_t instants[] = {
        // epoch edges (1970-01-01 was a Thursday)
        -1, 0, 1,
        utc_ms(1969, 12, 31, 23, 59, 59) + 999,
        // week edges: Sun 2026-04-05 23:59:59.999 / Mon 2026-04-06 00:00:00.000
        utc_ms(2026, 4, 5, 23, 59, 59) + 999,
        utc_ms(2026, 4, 6, 0, 0, 0),
        utc_ms(2026, 4, 6, 0, 0, 0) + 1,
        // the Dec/Jan straddling week: Mon 2025-12-29 .. Sun 2026-01-04
        utc_ms(2025, 12, 28, 23, 59, 59) + 999,
        utc_ms(2025, 12, 29, 0, 0, 0),
        utc_ms(2026, 1, 1, 0, 0, 0),
        utc_ms(2026, 1, 4, 23, 59, 59) + 999,
        utc_ms(2026, 1, 5, 0, 0, 0),
        // leap-day edges 2024
        utc_ms(2024, 2, 28, 23, 59, 59) + 999,
        utc_ms(2024, 2, 29, 0, 0, 0),
        utc_ms(2024, 2, 29, 23, 59, 59) + 999,
        utc_ms(2024, 3, 1, 0, 0, 0),
        // month first/last ms
        utc_ms(2026, 1, 31, 23, 59, 59) + 999,
        utc_ms(2026, 2, 1, 0, 0, 0),
        utc_ms(2026, 12, 31, 23, 59, 59) + 999,
        utc_ms(2027, 1, 1, 0, 0, 0),
        // DST transition instants (no-ops in UTC, listed as sensitive points):
        utc_ms(2026, 3, 8, 7, 0, 0) - 1000,   // US spring-forward 02:00 EST
        utc_ms(2026, 3, 8, 7, 0, 0),
        utc_ms(2026, 3, 8, 7, 0, 0) + 1000,
        utc_ms(2026, 11, 1, 6, 0, 0),         // US fall-back
        utc_ms(2026, 3, 29, 1, 0, 0),         // EU spring-forward
        utc_ms(2026, 10, 25, 1, 0, 0),        // EU fall-back
    };

    int64_t mism_w = 0, mism_m = 0;
    for (int64_t bar_ms : instants)
        check_instant(bar_ms, &mism_w, &mism_m);
    CHECK(mism_w == 0);
    CHECK(mism_m == 0);

    // Absolute anchors (hand-computed, not self-referential):
    // Wed 2026-04-08 12:00 -> week opens Mon 2026-04-06, month opens Apr 1.
    CHECK(fast_open(utc_ms(2026, 4, 8, 12, 0, 0), "W", "UTC") ==
          utc_ms(2026, 4, 6, 0, 0, 0));
    CHECK(fast_open(utc_ms(2026, 4, 8, 12, 0, 0), "M", "UTC") ==
          utc_ms(2026, 4, 1, 0, 0, 0));
    // Thu 1970-01-01 12:00 -> week opens Mon 1969-12-29 (negative epoch),
    // month opens 1970-01-01 00:00.
    CHECK(fast_open(utc_ms(1970, 1, 1, 12, 0, 0), "W", "UTC") ==
          utc_ms(1969, 12, 29, 0, 0, 0));
    CHECK(fast_open(utc_ms(1970, 1, 1, 12, 0, 0), "M", "UTC") == 0);
    // Wed 1969-06-18 -> week opens Mon 1969-06-16, month opens Jun 1 (pre-epoch).
    CHECK(fast_open(utc_ms(1969, 6, 18, 6, 0, 0), "W", "UTC") ==
          utc_ms(1969, 6, 16, 0, 0, 0));
    CHECK(fast_open(utc_ms(1969, 6, 18, 6, 0, 0), "M", "UTC") ==
          utc_ms(1969, 6, 1, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// All UTC-alias spellings must take the same fast path values.
// ---------------------------------------------------------------------------
static void test_utc_alias_zones_identical() {
    std::printf("test_utc_alias_zones_identical\n");
    const int64_t bars[] = {
        utc_ms(2026, 4, 8, 12, 0, 0),
        utc_ms(2024, 2, 29, 23, 59, 59) + 999,
        utc_ms(1969, 6, 18, 6, 0, 0),
    };
    for (int64_t bar : bars) {
        int64_t w_ref = fast_open(bar, "W", "UTC");
        int64_t m_ref = fast_open(bar, "M", "UTC");
        for (const char* tz : {"", "Etc/UTC", "GMT", "GMT+0"}) {
            CHECK(fast_open(bar, "W", tz) == w_ref);
            CHECK(fast_open(bar, "M", tz) == m_ref);
        }
    }
}

// ---------------------------------------------------------------------------
// Session predicates: decomposition now goes through decompose_ms_local
// (gmtime_r for UTC) instead of an unconditional ScopedTimezone. Replicate the
// OLD body (localtime_r under pinned TZ + day filter + window check) and
// require bit-equal booleans across a dense three-week sweep.
// ---------------------------------------------------------------------------

// Replica of the pre-change passes_session_filter tail for a session already
// split into (windows, optional day set). localtime_r under the caller-pinned
// TZ is exactly what ScopedTimezone(tz)+localtime_r computed.
static bool slow_ismarket_pinned(const std::string& windows,
                                 const int* days, int n_days, int64_t bar_ms) {
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    int day_of_week_sun1 = local_tm.tm_wday + 1;  // 1=Sunday
    if (n_days > 0) {
        bool hit = false;
        for (int i = 0; i < n_days; ++i)
            if (days[i] == day_of_week_sun1) hit = true;
        if (!hit)
            return false;
    }
    return local_time_in_session_windows(windows, local_tm);
}

static void test_session_predicates_utc_fast_vs_slow_sweep() {
    std::printf("test_session_predicates_utc_fast_vs_slow_sweep\n");
    pin_tz("UTC");

    // Mon 2026-04-06 .. Mon 2026-04-27 (three full weeks), every 61 s.
    const int64_t start_s = utc_ms(2026, 4, 6, 0, 0, 0) / 1000;
    const int64_t end_s   = utc_ms(2026, 4, 27, 0, 0, 0) / 1000;

    const int mon_fri[] = {2, 3, 4, 5, 6};
    struct Case {
        const char* session;   // as handed to the engine
        const char* windows;   // replica: windows body
        const int* days;       // replica: day filter (nullptr = none)
        int n_days;
    };
    const Case cases[] = {
        {"0930-1600", "0930-1600", nullptr, 0},
        {"2200-0500", "2200-0500", nullptr, 0},           // overnight wrap
        {"0800-1600:23456", "0800-1600", mon_fri, 5},     // Mon-Fri filter
        {"0000-2400", "0000-2400", nullptr, 0},           // all-day window
    };

    int64_t mismatches = 0, n = 0;
    for (int64_t s = start_s; s <= end_s; s += 61) {
        int64_t bar_ms = s * 1000 + ((n & 1) ? 999 : 0);
        ++n;
        for (const Case& c : cases) {
            bool fast = pine_session_ismarket(c.session, "UTC", bar_ms);
            bool slow = slow_ismarket_pinned(c.windows, c.days, c.n_days, bar_ms);
            if (fast != slow) {
                if (mismatches < 5)
                    std::printf("  SESSION mismatch '%s' at bar_ms=%lld: "
                                "fast=%d slow=%d\n",
                                c.session, (long long)bar_ms, (int)fast, (int)slow);
                ++mismatches;
            }
        }
        // is(pre|post)market for "0930-1600": pre = [04:00, 09:30),
        // post = [16:00, 20:00) — replica of the old bodies' arithmetic on the
        // same pinned-TZ decomposition.
        {
            time_t secs = static_cast<time_t>(bar_ms / 1000);
            struct tm lt {};
            localtime_r(&secs, &lt);
            int mod = lt.tm_hour * 60 + lt.tm_min;
            bool pre_slow  = (mod >= 4 * 60 && mod < 9 * 60 + 30);
            bool post_slow = (mod >= 16 * 60 && mod < 20 * 60);
            if (pine_session_ispremarket("0930-1600", "UTC", bar_ms) != pre_slow ||
                pine_session_ispostmarket("0930-1600", "UTC", bar_ms) != post_slow) {
                if (mismatches < 5)
                    std::printf("  PRE/POST mismatch at bar_ms=%lld\n",
                                (long long)bar_ms);
                ++mismatches;
            }
        }
    }
    std::printf("  swept %lld instants x %d sessions\n",
                (long long)n, (int)(sizeof(cases) / sizeof(cases[0])));
    CHECK(n > 29000);
    CHECK(mismatches == 0);

    // Non-UTC session predicate still exact: NY 0930-1600 on a Tue in EDT.
    int64_t bar = utc_ms(2026, 4, 7, 14, 30, 0);  // 10:30 America/New_York
    bool m = pine_session_ismarket("0930-1600", "America/New_York", bar);
    pin_tz("America/New_York");
    CHECK(m == slow_ismarket_pinned("0930-1600", nullptr, 0, bar));
    CHECK(m == true);
}

// ---------------------------------------------------------------------------
// Non-UTC zones must still take the untouched ScopedTimezone slow path.
// (Engine call first, then the pinned-TZ replica with the SAME normalized
// zone, so the engine's lazy g_active_tz cache never desyncs from the env.)
// ---------------------------------------------------------------------------
static void test_non_utc_zones_unchanged_slow_path() {
    std::printf("test_non_utc_zones_unchanged_slow_path\n");
    int64_t bar = utc_ms(2026, 4, 6, 1, 0, 0);  // Mon 01:00 UTC

    // Fixed offset "UTC+2" (POSIX "UTC-2"): local Mon 03:00.
    int64_t w_off = fast_open(bar, "W", "UTC+2");
    int64_t m_off = fast_open(bar, "M", "UTC+2");
    pin_tz(normalize_timezone_for_posix("UTC+2"));
    CHECK(w_off == slow_week_open_pinned(bar));
    CHECK(m_off == slow_month_open_pinned(bar));
    CHECK(w_off == utc_ms(2026, 4, 5, 22, 0, 0));   // Mon 00:00 local = Sun 22:00 UTC
    CHECK(m_off == utc_ms(2026, 3, 31, 22, 0, 0));  // Apr 1 00:00 local
    CHECK(w_off != fast_open(bar, "W", "UTC"));     // really NOT the UTC fast path

    // IANA Asia/Kolkata (+05:30): local Mon 06:30.
    int64_t w_kol = fast_open(bar, "W", "Asia/Kolkata");
    int64_t m_kol = fast_open(bar, "M", "Asia/Kolkata");
    pin_tz("Asia/Kolkata");
    CHECK(w_kol == slow_week_open_pinned(bar));
    CHECK(m_kol == slow_month_open_pinned(bar));
    CHECK(w_kol == utc_ms(2026, 4, 5, 18, 30, 0));   // Mon 00:00 IST
    CHECK(m_kol == utc_ms(2026, 3, 31, 18, 30, 0));  // Apr 1 00:00 IST
}

int main() {
    test_utc_fast_vs_slow_dense_sweep();
    test_utc_fast_vs_slow_boundary_instants();
    test_utc_alias_zones_identical();
    test_session_predicates_utc_fast_vs_slow_sweep();
    test_non_utc_zones_unchanged_slow_path();

    std::printf("calendar_wm_open_utc_fastpath: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
