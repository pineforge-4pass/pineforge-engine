#include <pineforge/session_time.hpp>
#include <pineforge/na.hpp>
#include <pineforge/timeframe.hpp>
#include "timezone.hpp"
#include <cctype>
#include <ctime>
#include <string>
#include <unordered_set>
#include <vector>

namespace pineforge {

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace — file-local only)
// ---------------------------------------------------------------------------
namespace {

static void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
}

// Suffix :1234567 — digits 1–7 are Sun–Sat (TradingView session weekdays)
static void parse_day_filter(const std::string& session_in,
                             std::string& windows_out,
                             std::unordered_set<int>* days_out) {
    windows_out = session_in;
    if (days_out)
        days_out->clear();
    if (session_in.empty())
        return;

    std::size_t colon = session_in.rfind(':');
    if (colon == std::string::npos || colon + 1 >= session_in.size())
        return;

    bool all_digits = true;
    for (std::size_t i = colon + 1; i < session_in.size(); ++i) {
        char c = session_in[i];
        if (c < '1' || c > '7') {
            all_digits = false;
            break;
        }
    }
    if (!all_digits)
        return;

    windows_out = session_in.substr(0, colon);
    trim_inplace(windows_out);
    if (days_out) {
        for (std::size_t i = colon + 1; i < session_in.size(); ++i)
            days_out->insert(session_in[i] - '0');
    }
}

static bool minute_in_window(int mod, int start, int end) {
    if (start <= end)
        return mod >= start && mod < end;
    return mod >= start || mod < end;
}

static int64_t utc_bucket_open_ms(int64_t bar_ms, int period_sec) {
    if (period_sec <= 0)
        return bar_ms;
    int64_t period_ms = static_cast<int64_t>(period_sec) * 1000;
    if (bar_ms >= 0)
        return (bar_ms / period_ms) * period_ms;
    int64_t q = bar_ms / period_ms;
    if (bar_ms % period_ms != 0 && bar_ms < 0)
        --q;
    return q * period_ms;
}

static int64_t calendar_day_open_local_ms(int64_t bar_ms, const std::string& tz) {
    pine_tz::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    time_t day0 = mktime(&local_tm);
    return static_cast<int64_t>(day0) * 1000;
}

static int64_t calendar_week_open_local_ms(int64_t bar_ms, const std::string& tz) {
    pine_tz::ScopedTimezone guard(tz);
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

static int64_t calendar_month_open_local_ms(int64_t bar_ms, const std::string& tz) {
    pine_tz::ScopedTimezone guard(tz);
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

static int64_t compute_tf_open_ms(int64_t bar_ms,
                                  const std::string& tf,
                                  const std::string& tz) {
    if (tf.empty())
        return bar_ms;

    CalendarPeriod cp = calendar_period_for(tf);
    if (cp == CalendarPeriod::DAY)
        return calendar_day_open_local_ms(bar_ms, tz);
    if (cp == CalendarPeriod::WEEK)
        return calendar_week_open_local_ms(bar_ms, tz);
    if (cp == CalendarPeriod::MONTH)
        return calendar_month_open_local_ms(bar_ms, tz);

    int sec = tf_to_seconds(tf);
    if (sec > 0 && sec < 86400)
        return utc_bucket_open_ms(bar_ms, sec);
    return bar_ms;
}

static int64_t compute_tf_close_ms(int64_t open_ms,
                                   const std::string& tf,
                                   const std::string& tz) {
    CalendarPeriod cp = calendar_period_for(tf);
    int sec = tf_to_seconds(tf);

    if (sec > 0 && sec < 86400) {
        return open_ms + static_cast<int64_t>(sec) * 1000 - 1;
    }

    pine_tz::ScopedTimezone guard(tz);
    time_t osec = static_cast<time_t>(open_ms / 1000);
    struct tm local_tm {};
    localtime_r(&osec, &local_tm);

    if (cp == CalendarPeriod::DAY) {
        local_tm.tm_mday += 1;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000 - 1;
    }
    if (cp == CalendarPeriod::WEEK) {
        local_tm.tm_mday += 7;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000 - 1;
    }
    if (cp == CalendarPeriod::MONTH) {
        local_tm.tm_mon += 1;
        local_tm.tm_mday = 1;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000 - 1;
    }
    return open_ms;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public session predicate helpers (exposed via session_time.hpp)
// ---------------------------------------------------------------------------

int hhmm_to_minutes(const std::string& hhmm) {
    if (hhmm.size() < 4)
        return -1;
    int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    int m = (hhmm[2] - '0') * 10 + (hhmm[3] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

bool local_time_in_session_windows(const std::string& windows_body,
                                   const struct tm& local_tm) {
    if (windows_body.empty() || windows_body == "24x7")
        return true;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;

    std::string s = windows_body;
    // trim_inplace is file-local; inline it here via a lambda approach would
    // require capturing s — simpler to just duplicate the trim logic inline.
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t comma = s.find(',', pos);
        std::string seg = (comma == std::string::npos) ? s.substr(pos)
                                                       : s.substr(pos, comma - pos);
        while (!seg.empty() && std::isspace((unsigned char)seg.front())) seg.erase(seg.begin());
        while (!seg.empty() && std::isspace((unsigned char)seg.back())) seg.pop_back();
        if (!seg.empty())
            parts.push_back(seg);
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    if (parts.empty())
        parts.push_back(s);

    for (const std::string& win : parts) {
        std::size_t dash = win.find('-');
        if (dash == std::string::npos || dash < 4 || win.size() < dash + 4)
            continue;
        std::string left = win.substr(0, 4);
        std::string right = win.substr(dash + 1, 4);
        int sm = hhmm_to_minutes(left);
        int em = hhmm_to_minutes(right);
        if (sm < 0 || em < 0)
            continue;
        bool in_win = (sm <= em) ? (mod >= sm && mod < em)
                                 : (mod >= sm || mod < em);
        if (in_win)
            return true;
    }
    return false;
}

bool passes_session_filter(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return true;

    std::string windows;
    std::unordered_set<int> day_filter;
    parse_day_filter(session, windows, &day_filter);

    pine_tz::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);

    int tv_dow = local_tm.tm_wday + 1;  // 1=Sunday
    if (!day_filter.empty() && day_filter.count(tv_dow) == 0)
        return false;

    return local_time_in_session_windows(windows, local_tm);
}

// ---------------------------------------------------------------------------
// Session predicate public free functions
// ---------------------------------------------------------------------------

// pine_session_ismarket: true when bar_ms falls inside the regular session.
bool pine_session_ismarket(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms) {
    return passes_session_filter(session, tz, bar_ms);
}

// pine_session_ispremarket: true when bar_ms falls in the pre-market window.
// Assumes standard ETH window: 04:00 local to RTH open (parsed from session
// string first window start).  For exchanges with non-standard ETH (e.g. LSE
// auctions) accuracy may degrade — document this limitation.
bool pine_session_ispremarket(const std::string& session,
                              const std::string& tz,
                              int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return false;

    // Parse RTH open from session string (first window start hhmm).
    std::string windows;
    std::unordered_set<int> day_filter;
    parse_day_filter(session, windows, &day_filter);

    // Find first window's start time (first 4-char hhmm before a '-').
    std::string rth_open_str;
    {
        std::size_t dash = windows.find('-');
        if (dash >= 4)
            rth_open_str = windows.substr(0, 4);
    }
    int rth_open_min = hhmm_to_minutes(rth_open_str);
    if (rth_open_min < 0)
        return false;

    int pre_open_min = 4 * 60;  // 04:00 standard ETH open

    pine_tz::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);

    // Day filter check (same days as RTH session).
    int tv_dow = local_tm.tm_wday + 1;
    if (!day_filter.empty() && day_filter.count(tv_dow) == 0)
        return false;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    // Pre-market window: [04:00, rth_open_min)
    return (mod >= pre_open_min && mod < rth_open_min);
}

// pine_session_ispostmarket: true when bar_ms falls in the post-market window.
// Assumes standard ETH window: RTH close to 20:00 local.
bool pine_session_ispostmarket(const std::string& session,
                               const std::string& tz,
                               int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return false;

    std::string windows;
    std::unordered_set<int> day_filter;
    parse_day_filter(session, windows, &day_filter);

    // Find first window's end time (4-char hhmm after first '-').
    std::string rth_close_str;
    {
        std::size_t dash = windows.find('-');
        if (dash != std::string::npos && dash + 4 < windows.size())
            rth_close_str = windows.substr(dash + 1, 4);
    }
    int rth_close_min = hhmm_to_minutes(rth_close_str);
    if (rth_close_min < 0)
        return false;

    int post_close_min = 20 * 60;  // 20:00 standard ETH close

    pine_tz::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);

    int tv_dow = local_tm.tm_wday + 1;
    if (!day_filter.empty() && day_filter.count(tv_dow) == 0)
        return false;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    // Post-market window: [rth_close_min, 20:00)
    return (mod >= rth_close_min && mod < post_close_min);
}

// ---------------------------------------------------------------------------
// pine_time / pine_time_close (existing public API)
// ---------------------------------------------------------------------------

int64_t pine_time(int64_t bar_ms,
                  const std::string& tf_in,
                  const std::string& session,
                  const std::string& tz_in,
                  const std::string& chart_tf) {
    std::string tf = tf_in.empty() ? chart_tf : tf_in;
    if (tf.empty())
        tf = "1";

    std::string tz = tz_in.empty() ? "UTC" : tz_in;

    if (!session.empty() && !passes_session_filter(session, tz, bar_ms))
        return na<int64_t>();

    return compute_tf_open_ms(bar_ms, tf, tz);
}

int64_t pine_time_close(int64_t bar_ms,
                        const std::string& tf_in,
                        const std::string& session,
                        const std::string& tz_in,
                        const std::string& chart_tf) {
    std::string tf = tf_in.empty() ? chart_tf : tf_in;
    if (tf.empty())
        tf = "1";

    std::string tz = tz_in.empty() ? "UTC" : tz_in;

    if (!session.empty() && !passes_session_filter(session, tz, bar_ms))
        return na<int64_t>();

    int64_t t_open = compute_tf_open_ms(bar_ms, tf, tz);
    return compute_tf_close_ms(t_open, tf, tz);
}

} // namespace pineforge
