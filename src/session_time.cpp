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

static int hhmm_to_minutes(const std::string& hhmm) {
    if (hhmm.size() < 4)
        return -1;
    int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    int m = (hhmm[2] - '0') * 10 + (hhmm[3] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

static bool minute_in_window(int mod, int start, int end) {
    if (start <= end)
        return mod >= start && mod < end;
    return mod >= start || mod < end;
}

static bool local_time_in_session_windows(const std::string& windows_body,
                                          const struct tm& local_tm) {
    if (windows_body.empty() || windows_body == "24x7")
        return true;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;

    std::string s = windows_body;
    trim_inplace(s);

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t comma = s.find(',', pos);
        std::string seg = (comma == std::string::npos) ? s.substr(pos)
                                                       : s.substr(pos, comma - pos);
        trim_inplace(seg);
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
        if (minute_in_window(mod, sm, em))
            return true;
    }
    return false;
}

static bool passes_session_filter(const std::string& session,
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

}  // namespace

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
