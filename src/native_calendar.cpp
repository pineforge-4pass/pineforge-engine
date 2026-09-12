#include <pineforge/native_calendar.hpp>

#include "timezone.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace pineforge::native_calendar {
inline namespace native_calendar_v2 {
namespace {

constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr int64_t kMsPerHour = 60 * kMsPerMinute;
constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();

// Week 0 Monday is 1969-12-29 (the Monday of the week containing 1970-01-01).
constexpr int64_t kWeekZeroMondayDays = -3;

struct CivilDate {
    int year = 1970;
    int month = 1;
    int day = 1;
};

struct CivilStamp {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

struct EpochSpan {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

struct SessionDay {
    CivilDate open_date{};
    CivilDate trading_date{};
    int64_t origin_ms = 0;
    int64_t last_traded_ms = 0;
    int64_t next_origin_ms = 0;
    std::vector<EpochSpan> spans;
};

struct PeriodCore {
    int64_t open_ms = 0;
    int64_t eligible_open_ms = 0;
    int64_t last_traded_close_ms = 0;
    int64_t next_period_open_ms = 0;
};

bool mul_ok(int64_t a, int64_t b, int64_t& out) {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a == kInt64Min || b == kInt64Min) return false;
    if (a > 0 && b > 0 && a > kInt64Max / b) return false;
    if (a < 0 && b < 0 && a < kInt64Max / b) return false;
    if (a > 0 && b < 0 && b < kInt64Min / a) return false;
    if (a < 0 && b > 0 && a < kInt64Min / b) return false;
    out = a * b;
    return true;
}

bool add_ok(int64_t a, int64_t b, int64_t& out) {
    if (b > 0 && a > kInt64Max - b) return false;
    if (b < 0 && a < kInt64Min - b) return false;
    out = a + b;
    return true;
}

int64_t floor_div(int64_t a, int64_t b) {
    if (b == 0) return 0;
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && a < 0) --q;
    return q;
}

// Howard Hinnant days_from_civil / civil_from_days.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

CivilDate civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    return CivilDate{y, static_cast<int>(m), static_cast<int>(d)};
}

bool valid_civil_date(int y, int m, int d) {
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    const CivilDate back = civil_from_days(days_from_civil(y, static_cast<unsigned>(m),
                                                          static_cast<unsigned>(d)));
    return back.year == y && back.month == m && back.day == d;
}

int weekday_sun0(const CivilDate& date) {
    const int64_t days = days_from_civil(date.year, static_cast<unsigned>(date.month),
                                         static_cast<unsigned>(date.day));
    return static_cast<int>(((days + 4) % 7 + 7) % 7);
}

CivilDate add_days(const CivilDate& date, int64_t n) {
    return civil_from_days(days_from_civil(date.year, static_cast<unsigned>(date.month),
                                           static_cast<unsigned>(date.day))
                           + n);
}

int cmp_stamp(const CivilStamp& a, const CivilStamp& b) {
    if (a.year != b.year) return a.year < b.year ? -1 : 1;
    if (a.month != b.month) return a.month < b.month ? -1 : 1;
    if (a.day != b.day) return a.day < b.day ? -1 : 1;
    if (a.hour != b.hour) return a.hour < b.hour ? -1 : 1;
    if (a.minute != b.minute) return a.minute < b.minute ? -1 : 1;
    if (a.second != b.second) return a.second < b.second ? -1 : 1;
    return 0;
}

bool is_utc_zone(std::string_view tz) {
    const std::string n = pine_tz::normalize_timezone_for_posix(std::string(tz));
    return n.empty() || n == "UTC" || n == "Etc/UTC";
}

int64_t utc_civil_ms(const CivilStamp& c) {
    const int64_t days = days_from_civil(c.year, static_cast<unsigned>(c.month),
                                         static_cast<unsigned>(c.day));
    int64_t secs = 0;
    if (!mul_ok(days, 86400, secs)) return 0;
    int64_t hms = static_cast<int64_t>(c.hour) * 3600 + static_cast<int64_t>(c.minute) * 60
                  + c.second;
    int64_t total = 0;
    if (!add_ok(secs, hms, total)) return 0;
    int64_t ms = 0;
    if (!mul_ok(total, 1000, ms)) return 0;
    return ms;
}

bool epoch_to_stamp_utc(int64_t ms, CivilStamp& out) {
    int64_t secs = ms / 1000;
    if (ms < 0 && ms % 1000 != 0) --secs;
    const time_t t = static_cast<time_t>(secs);
    if (static_cast<int64_t>(t) != secs) return false;
    std::tm g{};
    if (gmtime_r(&t, &g) == nullptr) return false;
    out.year = g.tm_year + 1900;
    out.month = g.tm_mon + 1;
    out.day = g.tm_mday;
    out.hour = g.tm_hour;
    out.minute = g.tm_min;
    out.second = g.tm_sec;
    return true;
}

bool epoch_to_stamp_local(int64_t ms, const std::string& tz, CivilStamp& out) {
    int64_t secs = ms / 1000;
    if (ms < 0 && ms % 1000 != 0) --secs;
    const time_t t = static_cast<time_t>(secs);
    if (static_cast<int64_t>(t) != secs) return false;
    std::tm loc{};
    if (is_utc_zone(tz)) {
        if (gmtime_r(&t, &loc) == nullptr) return false;
    } else {
        pine_tz::ScopedTimezone guard(tz);
        if (localtime_r(&t, &loc) == nullptr) return false;
    }
    out.year = loc.tm_year + 1900;
    out.month = loc.tm_mon + 1;
    out.day = loc.tm_mday;
    out.hour = loc.tm_hour;
    out.minute = loc.tm_min;
    out.second = loc.tm_sec;
    return true;
}

bool stamp_matches_tm(const CivilStamp& c, const std::tm& t) {
    return t.tm_year + 1900 == c.year && t.tm_mon + 1 == c.month && t.tm_mday == c.day
           && t.tm_hour == c.hour && t.tm_min == c.minute && t.tm_sec == c.second;
}

std::optional<int64_t> mktime_candidate(const CivilStamp& c, int isdst, const std::string& tz) {
    std::tm t{};
    t.tm_year = c.year - 1900;
    t.tm_mon = c.month - 1;
    t.tm_mday = c.day;
    t.tm_hour = c.hour;
    t.tm_min = c.minute;
    t.tm_sec = c.second;
    t.tm_isdst = isdst;
    time_t sec;
    if (is_utc_zone(tz)) {
        // timegm is not required: UTC civil is exact integer math.
        return utc_civil_ms(c);
    }
    pine_tz::ScopedTimezone guard(tz);
    sec = mktime(&t);
    if (sec == static_cast<time_t>(-1)) return std::nullopt;
    std::tm back{};
    if (localtime_r(&sec, &back) == nullptr) return std::nullopt;
    if (!stamp_matches_tm(c, back)) return std::nullopt;
    int64_t ms = 0;
    if (!mul_ok(static_cast<int64_t>(sec), 1000, ms)) return std::nullopt;
    return ms;
}

std::optional<int64_t> first_representable_at_or_after(const CivilStamp& declared,
                                                       const std::string& tz) {
    const int64_t naive = utc_civil_ms(declared);
    int64_t lo = 0;
    int64_t hi = 0;
    if (!add_ok(naive, -14 * kMsPerHour, lo) || !add_ok(naive, 14 * kMsPerHour, hi)) {
        return std::nullopt;
    }
    auto ge = [&](int64_t ms) -> int {
        CivilStamp got{};
        if (!epoch_to_stamp_local(ms, tz, got)) return 1;
        return cmp_stamp(got, declared);
    };
    // Leftmost epoch with local civil >= declared, inside [lo, hi].
    if (ge(hi) < 0) return std::nullopt;
    while (lo < hi) {
        const int64_t mid = lo + (hi - lo) / 2;
        if (ge(mid) >= 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    CivilStamp got{};
    if (!epoch_to_stamp_local(lo, tz, got)) return std::nullopt;
    if (cmp_stamp(got, declared) < 0) return std::nullopt;
    return lo;
}

std::optional<CivilResolution> resolve_stamp(const std::string& tz, const CivilStamp& c) {
    if (!valid_civil_date(c.year, c.month, c.day)) return std::nullopt;
    if (c.hour < 0 || c.hour > 23 || c.minute < 0 || c.minute > 59 || c.second < 0
        || c.second > 59) {
        return std::nullopt;
    }
    if (is_utc_zone(tz)) {
        CivilResolution r;
        r.epoch_ms = utc_civil_ms(c);
        r.kind = CivilKind::Unique;
        return r;
    }
    std::vector<int64_t> hits;
    for (int isdst : {0, 1}) {
        if (auto ms = mktime_candidate(c, isdst, tz)) {
            if (std::find(hits.begin(), hits.end(), *ms) == hits.end()) hits.push_back(*ms);
        }
    }
    if (hits.size() == 1) {
        return CivilResolution{*hits.begin(), CivilKind::Unique};
    }
    if (hits.size() >= 2) {
        std::sort(hits.begin(), hits.end());
        return CivilResolution{hits.front(), CivilKind::Fold};
    }
    auto gap = first_representable_at_or_after(c, tz);
    if (!gap) return std::nullopt;
    CivilStamp got{};
    if (!epoch_to_stamp_local(*gap, tz, got)) return std::nullopt;
    if (cmp_stamp(got, c) == 0) {
        // Unique that mktime(0/1) missed; still Unique.
        return CivilResolution{*gap, CivilKind::Unique};
    }
    return CivilResolution{*gap, CivilKind::Gap};
}

std::optional<int64_t> resolve_hms(const std::string& tz,
                                   CivilDate date,
                                   int minutes_from_midnight) {
    if (minutes_from_midnight >= 1440) {
        date = add_days(date, minutes_from_midnight / 1440);
        minutes_from_midnight %= 1440;
    }
    if (minutes_from_midnight < 0) return std::nullopt;
    auto r = resolve_stamp(tz,
                           CivilStamp{date.year, date.month, date.day, minutes_from_midnight / 60,
                                      minutes_from_midnight % 60, 0});
    if (!r) return std::nullopt;
    return r->epoch_ms;
}

bool day_allowed(const SessionCalendar& cal, const CivilDate& trading_date) {
    const int pine_dow = weekday_sun0(trading_date) + 1;
    return (cal.day_mask() & static_cast<std::uint8_t>(1u << pine_dow)) != 0;
}

bool same_civil_date(const CivilDate& a, const CivilDate& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

// Recurring instance of one declared window on a civil date. Wrap and
// equal-start full-day end on the next civil date. 2400 ends at next midnight.
int64_t civil_minutes(const CivilDate& date, int minutes) {
    return days_from_civil(date.year, static_cast<unsigned>(date.month),
                           static_cast<unsigned>(date.day))
               * 1440
           + minutes;
}

bool civil_window_instance(const SessionWindow& w,
                           const CivilDate& date,
                           int64_t& start_cm,
                           int64_t& end_cm) {
    start_cm = civil_minutes(date, w.start_minutes());
    if (w.full_day()) {
        end_cm = civil_minutes(add_days(date, 1), w.start_minutes());
    } else if (w.wraps()) {
        end_cm = civil_minutes(add_days(date, 1), w.end_minutes());
    } else if (w.end_minutes() == 1440) {
        end_cm = civil_minutes(add_days(date, 1), 0);
    } else {
        end_cm = civil_minutes(date, w.end_minutes());
    }
    return end_cm > start_cm;
}

std::optional<int64_t> resolve_civil_minutes(const std::string& tz, int64_t cm) {
    const int64_t days = floor_div(cm, 1440);
    const int minutes = static_cast<int>(cm - days * 1440);
    return resolve_hms(tz, civil_from_days(days), minutes);
}

std::vector<EpochSpan> merge_spans(std::vector<EpochSpan> spans) {
    if (spans.empty()) return spans;
    std::sort(spans.begin(), spans.end(),
              [](const EpochSpan& a, const EpochSpan& b) { return a.start_ms < b.start_ms; });
    std::vector<EpochSpan> out;
    out.push_back(spans.front());
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (out.back().end_ms < spans[i].start_ms) {
            out.push_back(spans[i]);
        } else if (spans[i].end_ms > out.back().end_ms) {
            out.back().end_ms = spans[i].end_ms;
        }
    }
    return out;
}

int64_t first_overlap_open(int64_t open_ms,
                           int64_t raw_end,
                           const std::vector<EpochSpan>& spans) {
    int64_t eligible = open_ms;
    bool any = false;
    for (const EpochSpan& s : spans) {
        const int64_t a = std::max(open_ms, s.start_ms);
        const int64_t b = std::min(raw_end, s.end_ms);
        if (b > a && (!any || a < eligible)) {
            eligible = a;
            any = true;
        }
    }
    return eligible;
}

std::optional<int64_t> origin_on(const SessionCalendar& cal, const CivilDate& open_date) {
    return resolve_hms(cal.timezone(), open_date, cal.origin_minutes());
}

std::optional<CivilStamp> local_stamp(const std::string& tz, int64_t ms);

int64_t last_span_end(const std::vector<EpochSpan>& spans, int64_t origin_ms) {
    int64_t last = origin_ms;
    bool any = false;
    for (const EpochSpan& s : spans) {
        if (s.end_ms > s.start_ms) {
            if (!any || s.end_ms > last) last = s.end_ms;
            any = true;
        }
    }
    return last;
}

std::optional<SessionDay> session_day_from_open(const SessionCalendar& cal,
                                                const CivilDate& open_date) {
    if (!cal.valid()) return std::nullopt;
    const int origin = cal.origin_minutes();
    const int64_t cycle_start_cm = civil_minutes(open_date, origin);
    const int64_t cycle_end_cm = civil_minutes(add_days(open_date, 1), origin);
    if (cycle_end_cm <= cycle_start_cm) return std::nullopt;

    std::vector<EpochSpan> civil_raw;
    if (cal.all_day()) {
        civil_raw.push_back(EpochSpan{cycle_start_cm, cycle_end_cm});
    } else {
        for (int n = -1; n <= 1; ++n) {
            const CivilDate inst_date = add_days(open_date, n);
            for (const SessionWindow& w : cal.windows()) {
                int64_t start_cm = 0;
                int64_t end_cm = 0;
                if (!civil_window_instance(w, inst_date, start_cm, end_cm)) continue;
                const int64_t a = std::max(start_cm, cycle_start_cm);
                const int64_t b = std::min(end_cm, cycle_end_cm);
                if (b > a) civil_raw.push_back(EpochSpan{a, b});
            }
        }
    }
    auto civil_union = merge_spans(std::move(civil_raw));
    CivilDate trading = open_date;
    if (!civil_union.empty()) {
        const int64_t last_cm = last_span_end(civil_union, cycle_start_cm);
        trading = civil_from_days(floor_div(last_cm - 1, 1440));
    }

    auto cycle_start = origin_on(cal, open_date);
    auto cycle_end = origin_on(cal, add_days(open_date, 1));
    if (!cycle_start || !cycle_end) return std::nullopt;
    if (*cycle_end <= *cycle_start) return std::nullopt;

    std::vector<EpochSpan> resolved;
    for (const EpochSpan& civ : civil_union) {
        auto a = resolve_civil_minutes(cal.timezone(), civ.start_ms);
        auto b = resolve_civil_minutes(cal.timezone(), civ.end_ms);
        if (!a || !b || *b <= *a) continue;
        resolved.push_back(EpochSpan{*a, *b});
    }
    resolved = merge_spans(std::move(resolved));

    SessionDay day;
    day.open_date = open_date;
    day.origin_ms = *cycle_start;
    day.next_origin_ms = *cycle_end;
    day.trading_date = trading;
    if (!resolved.empty() && day_allowed(cal, trading)) {
        day.spans = std::move(resolved);
        day.last_traded_ms = last_span_end(day.spans, day.origin_ms);
    } else {
        day.spans = {};
        day.last_traded_ms = day.origin_ms;
    }
    return day;
}

std::optional<SessionDay> session_day_with_trading(const SessionCalendar& cal,
                                                   const CivilDate& trading) {
    for (int off : {0, -1, 1, -2, 2}) {
        auto d = session_day_from_open(cal, add_days(trading, off));
        if (d && same_civil_date(d->trading_date, trading)) return d;
    }
    return std::nullopt;
}

std::optional<CivilStamp> local_stamp(const std::string& tz, int64_t ms) {
    CivilStamp c{};
    if (is_utc_zone(tz)) {
        if (!epoch_to_stamp_utc(ms, c)) return std::nullopt;
    } else {
        if (!epoch_to_stamp_local(ms, tz, c)) return std::nullopt;
    }
    return c;
}

std::optional<SessionDay> session_day_containing(const SessionCalendar& cal, int64_t ms) {
    if (!cal.valid()) return std::nullopt;
    auto loc = local_stamp(cal.timezone(), ms);
    if (!loc) return std::nullopt;
    const CivilDate local_date{loc->year, loc->month, loc->day};
    const CivilDate candidates[3] = {local_date, add_days(local_date, -1),
                                     add_days(local_date, 1)};
    for (const CivilDate& open_date : candidates) {
        auto day = session_day_from_open(cal, open_date);
        if (!day) continue;
        if (day->origin_ms <= ms && ms < day->next_origin_ms) return day;
    }
    return session_day_from_open(cal, local_date);
}

bool ms_in_spans(const std::vector<EpochSpan>& spans, int64_t ms) {
    for (const EpochSpan& s : spans) {
        if (ms >= s.start_ms && ms < s.end_ms) return true;
    }
    return false;
}

int64_t overlap_end(const std::vector<EpochSpan>& spans, int64_t open_ms, int64_t raw_end) {
    int64_t last = open_ms;
    bool any = false;
    for (const EpochSpan& s : spans) {
        const int64_t a = std::max(open_ms, s.start_ms);
        const int64_t b = std::min(raw_end, s.end_ms);
        if (b > a) {
            if (!any || b > last) last = b;
            any = true;
        }
    }
    return last;
}

int64_t unit_ms(TimeframeUnit unit) {
    switch (unit) {
    case TimeframeUnit::Second:
        return kMsPerSecond;
    case TimeframeUnit::Minute:
        return kMsPerMinute;
    default:
        return 0;
    }
}

// Fixed-length seconds matching baseline tf_to_seconds / tf_ratio (D=86400,
// W=604800). Month is calendar-only (tf_to_seconds == -1); no invented month
// length is used to refuse D/nD→M or minutes→M.
std::optional<int64_t> known_period_seconds(const Timeframe& tf) {
    if (!tf.valid()) return std::nullopt;
    int64_t n = tf.count();
    int64_t s = 0;
    switch (tf.unit()) {
    case TimeframeUnit::Second:
        return n;
    case TimeframeUnit::Minute:
        if (!mul_ok(n, 60, s)) return std::nullopt;
        return s;
    case TimeframeUnit::Day:
        if (!mul_ok(n, 86400, s)) return std::nullopt;
        return s;
    case TimeframeUnit::Week:
        if (!mul_ok(n, 604800, s)) return std::nullopt;
        return s;
    case TimeframeUnit::Month:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<PeriodCore> calendar_core(const SessionCalendar& cal,
                                        TimeframeUnit unit,
                                        int count,
                                        int64_t ms) {
    auto day = session_day_containing(cal, ms);
    if (!day) return std::nullopt;
    const int64_t trading_days = days_from_civil(day->trading_date.year,
                                                 static_cast<unsigned>(day->trading_date.month),
                                                 static_cast<unsigned>(day->trading_date.day));
    int64_t key = 0;
    int64_t next_key = 0;
    CivilDate first_trading;
    CivilDate next_trading;
    if (unit == TimeframeUnit::Day) {
        key = floor_div(trading_days, count) * count;
        next_key = key + count;
        first_trading = civil_from_days(key);
        next_trading = civil_from_days(next_key);
    } else if (unit == TimeframeUnit::Week) {
        const int dow_mon0 = (weekday_sun0(day->trading_date) + 6) % 7;
        const int64_t monday_days = trading_days - dow_mon0;
        const int64_t week_i = floor_div(monday_days - kWeekZeroMondayDays, 7);
        key = floor_div(week_i, count) * count;
        next_key = key + count;
        first_trading = civil_from_days(kWeekZeroMondayDays + key * 7);
        next_trading = civil_from_days(kWeekZeroMondayDays + next_key * 7);
    } else if (unit == TimeframeUnit::Month) {
        const int64_t month_i =
            static_cast<int64_t>(day->trading_date.year) * 12 + (day->trading_date.month - 1);
        key = floor_div(month_i, count) * count;
        next_key = key + count;
        int y = static_cast<int>(floor_div(key, 12));
        int m = static_cast<int>(key - static_cast<int64_t>(y) * 12) + 1;
        first_trading = CivilDate{y, m, 1};
        int ny = static_cast<int>(floor_div(next_key, 12));
        int nm = static_cast<int>(next_key - static_cast<int64_t>(ny) * 12) + 1;
        next_trading = CivilDate{ny, nm, 1};
    } else {
        return std::nullopt;
    }
    auto first = session_day_with_trading(cal, first_trading);
    auto nxt = session_day_with_trading(cal, next_trading);
    if (!first || !nxt) return std::nullopt;
    int64_t last = first->origin_ms;
    int64_t eligible = first->origin_ms;
    bool any = false;
    bool found_eligible = false;
    const int64_t first_ord = days_from_civil(first_trading.year,
                                              static_cast<unsigned>(first_trading.month),
                                              static_cast<unsigned>(first_trading.day));
    const int64_t next_ord = days_from_civil(next_trading.year,
                                             static_cast<unsigned>(next_trading.month),
                                             static_cast<unsigned>(next_trading.day));
    for (int64_t ord = first_ord; ord < next_ord; ++ord) {
        const CivilDate td = civil_from_days(ord);
        auto d = session_day_with_trading(cal, td);
        if (!d) continue;
        for (const EpochSpan& s : d->spans) {
            if (s.end_ms > s.start_ms) {
                if (!found_eligible) {
                    eligible = s.start_ms;
                    found_eligible = true;
                }
                break;
            }
        }
        if (found_eligible) break;
    }
    for (int64_t ord = next_ord - 1; ord >= first_ord; --ord) {
        const CivilDate td = civil_from_days(ord);
        auto d = session_day_with_trading(cal, td);
        if (!d) continue;
        if (d->last_traded_ms > d->origin_ms) {
            last = d->last_traded_ms;
            any = true;
            break;
        }
    }
    PeriodCore core;
    core.open_ms = first->origin_ms;
    core.eligible_open_ms = found_eligible ? eligible : first->origin_ms;
    core.last_traded_close_ms = any ? last : first->origin_ms;
    core.next_period_open_ms = nxt->origin_ms;
    return core;
}

std::optional<PeriodCore> fixed_core(const SessionCalendar& cal,
                                     const Timeframe& tf,
                                     int64_t ms) {
    int64_t bucket = 0;
    if (!tf.valid() || !mul_ok(tf.count(), unit_ms(tf.unit()), bucket) || bucket <= 0) {
        return std::nullopt;
    }
    auto day = session_day_containing(cal, ms);
    if (!day) return std::nullopt;
    const int64_t elapsed = ms - day->origin_ms;
    const int64_t idx = floor_div(elapsed, bucket);
    int64_t offset = 0;
    if (!mul_ok(idx, bucket, offset)) return std::nullopt;
    int64_t open = 0;
    if (!add_ok(day->origin_ms, offset, open)) return std::nullopt;
    int64_t raw_end = 0;
    if (!add_ok(open, bucket, raw_end)) return std::nullopt;
    PeriodCore core;
    core.open_ms = open;
    core.last_traded_close_ms = overlap_end(day->spans, open, raw_end);
    core.eligible_open_ms = first_overlap_open(open, raw_end, day->spans);
    core.next_period_open_ms = raw_end;
    return core;
}

std::optional<PeriodCore> core_containing(const SessionCalendar& cal,
                                          const Timeframe& tf,
                                          int64_t ms) {
    if (!cal.valid() || !tf.valid()) return std::nullopt;
    if (tf.is_fixed()) return fixed_core(cal, tf, ms);
    return calendar_core(cal, tf.unit(), tf.count(), ms);
}

std::optional<int64_t> next_span_start_at_or_after(const SessionCalendar& cal, int64_t after) {
    auto day0 = session_day_containing(cal, after);
    if (!day0) return std::nullopt;
    for (int i = 0; i <= 800; ++i) {
        auto d = session_day_from_open(cal, add_days(day0->open_date, i));
        if (!d) return std::nullopt;
        for (const EpochSpan& s : d->spans) {
            if (s.end_ms <= after) continue;
            if (s.start_ms >= after) return s.start_ms;
            return after;
        }
    }
    return std::nullopt;
}

std::optional<int64_t> first_tradable_open(const SessionCalendar& cal,
                                           const Timeframe& tf,
                                           int64_t at_or_after) {
    if (tf.is_fixed()) {
        return next_span_start_at_or_after(cal, at_or_after);
    }
    auto core = core_containing(cal, tf, at_or_after);
    if (!core) return std::nullopt;
    int64_t t = at_or_after;
    for (int i = 0; i < 800; ++i) {
        auto n = core_containing(cal, tf, t);
        if (!n) return std::nullopt;
        if (n->open_ms >= at_or_after && n->last_traded_close_ms > n->open_ms) {
            return n->eligible_open_ms >= at_or_after ? n->eligible_open_ms : n->open_ms;
        }
        if (n->next_period_open_ms <= t) return std::nullopt;
        t = n->next_period_open_ms;
    }
    return std::nullopt;
}

void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

bool all_mask_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '1' || c > '7') return false;
    }
    return true;
}

bool tz_has_nul_or_control_or_space(std::string_view s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F || std::isspace(c)) return true;
    }
    return false;
}

bool tz_is_utc_gmt_name(std::string_view s) {
    return s == "UTC" || s == "GMT" || s == "Etc/UTC" || s == "Etc/GMT";
}

bool tz_parse_uint_bounded(std::string_view s, int max_digits, int max_value, int& out) {
    if (s.empty() || static_cast<int>(s.size()) > max_digits) return false;
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const int d = c - '0';
        if (n > (max_value - d) / 10) return false;
        n = n * 10 + d;
    }
    if (n > max_value) return false;
    out = n;
    return true;
}

bool tz_utc_gmt_offset_accepted(std::string_view tz) {
    std::size_t prefix = 0;
    if (tz.size() >= 3 && tz.substr(0, 3) == "UTC")
        prefix = 3;
    else if (tz.size() >= 3 && tz.substr(0, 3) == "GMT")
        prefix = 3;
    else
        return false;
    if (prefix >= tz.size()) return false;
    const char sign = tz[prefix];
    if (sign != '+' && sign != '-') return false;
    const std::string_view body = tz.substr(prefix + 1);
    if (body.empty()) return false;
    std::string_view hour_s;
    std::string_view minute_s;
    const auto colon = body.find(':');
    if (colon != std::string_view::npos) {
        hour_s = body.substr(0, colon);
        minute_s = body.substr(colon + 1);
        if (minute_s.size() != 2) return false;
    } else if (body.size() <= 2) {
        hour_s = body;
        minute_s = {};
    } else if (body.size() == 3 || body.size() == 4) {
        hour_s = body.substr(0, body.size() - 2);
        minute_s = body.substr(body.size() - 2);
    } else {
        return false;
    }
    int hours = 0;
    int minutes = 0;
    if (!tz_parse_uint_bounded(hour_s, 2, 23, hours)) return false;
    if (!minute_s.empty() && !tz_parse_uint_bounded(minute_s, 2, 59, minutes)) return false;
    return true;
}

bool tz_looks_like_utc_gmt_offset(std::string_view tz) {
    if (tz.size() < 4) return false;
    if (tz.substr(0, 3) != "UTC" && tz.substr(0, 3) != "GMT") return false;
    const char sign = tz[3];
    return sign == '+' || sign == '-';
}

bool tz_iana_name_chars(std::string_view name) {
    if (name.empty() || name.size() > 255) return false;
    if (name.front() == '/' || name.back() == '/') return false;
    std::size_t i = 0;
    while (i < name.size()) {
        const auto slash = name.find('/', i);
        const auto part = name.substr(i, slash == std::string_view::npos ? std::string_view::npos
                                                                        : slash - i);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) {
            if (!(std::isalnum(c) || c == '_' || c == '+' || c == '-')) return false;
        }
        if (slash == std::string_view::npos) break;
        i = slash + 1;
    }
    return true;
}

bool tzdir_realpath_dir(const char* path, std::string& out) {
    if (path == nullptr || path[0] == '\0') return false;
    char buf[PATH_MAX];
    if (::realpath(path, buf) == nullptr) return false;
    struct stat st {};
    if (::stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    out.assign(buf);
    return true;
}

// Effective zoneinfo root used by this libc for relative TZ names.
// macOS tzset(3) reads /var/db/timezone/zoneinfo and does not document TZDIR;
// probes on this Darwin libc ignore TZDIR. glibc tzfile.c uses TZDIR when
// nonempty (relative included) and the compile-time default only when TZDIR
// is unset or empty; a missing/non-directory TZDIR is fail-closed so names
// are not accepted from a tree libc will not read.
std::string tzdir_canonical() {
    std::string out;
#if defined(__APPLE__)
    if (tzdir_realpath_dir("/var/db/timezone/zoneinfo", out)) return out;
    if (tzdir_realpath_dir("/usr/share/zoneinfo", out)) return out;
    return {};
#elif defined(__GLIBC__)
    const char* env = std::getenv("TZDIR");
    if (env != nullptr && env[0] != '\0') {
        if (tzdir_realpath_dir(env, out)) return out;
        return {};
    }
    if (tzdir_realpath_dir("/usr/share/zoneinfo", out)) return out;
    return {};
#else
    if (tzdir_realpath_dir("/usr/share/zoneinfo", out)) return out;
    return {};
#endif
}

std::optional<std::string> tz_file_actual_path(std::string_view name) {
    if (!tz_iana_name_chars(name)) return std::nullopt;
    const std::string dir = tzdir_canonical();
    if (dir.empty()) return std::nullopt;
    std::string full = dir;
    full.push_back('/');
    full.append(name.begin(), name.end());
    char resolved[PATH_MAX];
    if (::realpath(full.c_str(), resolved) == nullptr) return std::nullopt;
    const std::string res(resolved);
    if (res != dir && res.rfind(dir + "/", 0) != 0) return std::nullopt;
    const int fd = ::open(res.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    char mag[4] = {};
    const ssize_t n = ::read(fd, mag, 4);
    ::close(fd);
    if (n != 4 || std::memcmp(mag, "TZif", 4) != 0) return std::nullopt;
    return res;
}

bool tz_file_is_tzif(std::string_view name) {
    return tz_file_actual_path(name).has_value();
}

bool posix_consume_name(std::string_view s, std::size_t& p) {
    if (p >= s.size()) return false;
    if (s[p] == '<') {
        const auto end = s.find('>', p + 1);
        if (end == std::string_view::npos || end - (p + 1) < 3) return false;
        for (std::size_t i = p + 1; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (!(std::isalnum(c) || c == '+' || c == '-')) return false;
        }
        p = end + 1;
        return true;
    }
    const std::size_t start = p;
    while (p < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[p]);
        if (std::isdigit(c) || c == '+' || c == '-' || c == ',') break;
        ++p;
    }
    return p - start >= 3;
}

bool posix_consume_offset(std::string_view s, std::size_t& p) {
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
    int hours = 0;
    int digits = 0;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p])) && digits < 2) {
        hours = hours * 10 + (s[p] - '0');
        ++p;
        ++digits;
    }
    if (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) return false;
    if (hours > 24) return false;
    auto colon_part = [&]() -> bool {
        if (p >= s.size() || s[p] != ':') return true;
        ++p;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
        int v = 0;
        int d = 0;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p])) && d < 2) {
            v = v * 10 + (s[p] - '0');
            ++p;
            ++d;
        }
        if (d < 1 || v > 59) return false;
        return true;
    };
    if (!colon_part()) return false;
    if (!colon_part()) return false;
    return true;
}

bool posix_consume_rule(std::string_view s, std::size_t& p) {
    if (p >= s.size()) return false;
    if (s[p] == 'M') {
        ++p;
        int m = 0, n = 0, d = 0;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            m = m * 10 + (s[p] - '0');
            ++p;
            if (m > 12) return false;
        }
        if (m < 1 || p >= s.size() || s[p] != '.') return false;
        ++p;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
        n = s[p] - '0';
        ++p;
        if (n < 1 || n > 5 || p >= s.size() || s[p] != '.') return false;
        ++p;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
        d = s[p] - '0';
        ++p;
        if (d > 6) return false;
    } else if (s[p] == 'J') {
        ++p;
        int n = 0;
        int digits = 0;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            n = n * 10 + (s[p] - '0');
            ++p;
            ++digits;
            if (digits > 3 || n > 365) return false;
        }
        if (n < 1) return false;
    } else if (std::isdigit(static_cast<unsigned char>(s[p]))) {
        int n = 0;
        int digits = 0;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            n = n * 10 + (s[p] - '0');
            ++p;
            ++digits;
            if (digits > 3 || n > 365) return false;
        }
    } else {
        return false;
    }
    if (p < s.size() && s[p] == '/') {
        ++p;
        if (!posix_consume_offset(s, p)) return false;
    }
    return true;
}

bool tz_posix_spec_accepted(std::string_view s) {
    std::size_t p = 0;
    if (!posix_consume_name(s, p)) return false;
    if (!posix_consume_offset(s, p)) return false;
    if (p == s.size()) return true;
    if (!posix_consume_name(s, p)) return false;
    if (p < s.size() && (s[p] == '+' || s[p] == '-' || std::isdigit(static_cast<unsigned char>(s[p])))) {
        if (!posix_consume_offset(s, p)) return false;
    }
    if (p == s.size()) return true;
    if (s[p] != ',') return false;
    ++p;
    if (!posix_consume_rule(s, p)) return false;
    if (p >= s.size() || s[p] != ',') return false;
    ++p;
    if (!posix_consume_rule(s, p)) return false;
    return p == s.size();
}

struct TzClass {
    enum Kind { Reject, Utc, Offset, Tzfile, Posix } kind = Reject;
    std::string_view tzfile_name{};
};

bool tz_slash_is_file_path(std::string_view tz) {
    const auto slash = tz.find('/');
    const auto comma = tz.find(',');
    return slash != std::string_view::npos
        && (comma == std::string_view::npos || slash < comma);
}

TzClass timezone_classify(std::string_view tz) {
    TzClass out;
    if (tz.empty()) {
        out.kind = TzClass::Utc;
        return out;
    }
    if (tz_has_nul_or_control_or_space(tz)) return out;
    if (tz_is_utc_gmt_name(tz)) {
        out.kind = TzClass::Utc;
        return out;
    }
    if (tz_looks_like_utc_gmt_offset(tz)) {
        out.kind = tz_utc_gmt_offset_accepted(tz) ? TzClass::Offset : TzClass::Reject;
        return out;
    }
    if (tz[0] == ':') {
        const std::string_view file = tz.substr(1);
        if (file.empty() || !tz_file_is_tzif(file)) return out;
        out.kind = TzClass::Tzfile;
        out.tzfile_name = file;
        return out;
    }
    // POSIX DST rule times use '/' (M3.2.0/2). A slash before any comma is a
    // tzfile path (America/New_York, US/Eastern), not a POSIX spec.
    if (tz_slash_is_file_path(tz)) {
        if (!tz_file_is_tzif(tz)) return out;
        out.kind = TzClass::Tzfile;
        out.tzfile_name = tz;
        return out;
    }
    if (tz_file_is_tzif(tz)) {
        out.kind = TzClass::Tzfile;
        out.tzfile_name = tz;
        return out;
    }
    if (tz_posix_spec_accepted(tz)) {
        out.kind = TzClass::Posix;
        return out;
    }
    return out;
}

bool timezone_spec_accepted(std::string_view tz) {
    return timezone_classify(tz).kind != TzClass::Reject;
}

bool posix_is_default_dst(std::string_view s) {
    std::size_t p = 0;
    if (!posix_consume_name(s, p) || !posix_consume_offset(s, p)) return false;
    if (p == s.size()) return false;
    if (!posix_consume_name(s, p)) return false;
    if (p < s.size()
        && (s[p] == '+' || s[p] == '-' || std::isdigit(static_cast<unsigned char>(s[p])))) {
        if (!posix_consume_offset(s, p)) return false;
    }
    return p == s.size();
}

int parse_hhmm(std::string_view s, bool allow_2400) {
    if (s.size() != 4) return -1;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
    }
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[2] - '0') * 10 + (s[3] - '0');
    if (allow_2400 && h == 24 && m == 0) return 1440;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

}  // namespace

bool timezone_accepted(std::string_view timezone) {
    return timezone_spec_accepted(timezone);
}

bool TimezoneIdentityDescriptor::valid() const noexcept {
    if (semantics_version != kSemanticsVersion) return false;
    if (effective_definition.empty()) return false;
    for (const std::string& p : resource_paths) {
        if (p.empty() || p.front() != '/') return false;
    }
    switch (kind) {
        case TimezoneSourceKind::Utc:
        case TimezoneSourceKind::FixedOffset:
        case TimezoneSourceKind::PosixExplicit:
            return true;
        case TimezoneSourceKind::PosixDefaultDst:
        case TimezoneSourceKind::Tzfile:
            return !resource_paths.empty();
        default:
            return false;
    }
}

std::optional<TimezoneIdentityDescriptor>
timezone_identity_descriptor(std::string_view timezone) {
    const TzClass cls = timezone_classify(timezone);
    if (cls.kind == TzClass::Reject) return std::nullopt;

    TimezoneIdentityDescriptor d;
    d.semantics_version = TimezoneIdentityDescriptor::kSemanticsVersion;
    d.input.assign(timezone.begin(), timezone.end());
    d.zoneinfo_root = tzdir_canonical();
    const std::string norm =
        pine_tz::normalize_timezone_for_posix(std::string(timezone.begin(), timezone.end()));

    auto finish = [&]() -> std::optional<TimezoneIdentityDescriptor> {
        if (!d.valid()) return std::nullopt;
        return d;
    };

    if (cls.kind == TzClass::Utc || norm == "UTC") {
        d.kind = TimezoneSourceKind::Utc;
        d.effective_definition = "UTC";
        if (auto p = tz_file_actual_path("UTC")) d.resource_paths.push_back(*p);
        return finish();
    }

    if (cls.kind == TzClass::Offset) {
        d.kind = TimezoneSourceKind::FixedOffset;
        d.effective_definition = norm;
        return finish();
    }

    if (cls.kind == TzClass::Tzfile) {
        auto p = tz_file_actual_path(cls.tzfile_name);
        if (!p) return std::nullopt;
        d.kind = TimezoneSourceKind::Tzfile;
        d.effective_definition.assign(cls.tzfile_name.begin(), cls.tzfile_name.end());
        d.resource_paths.push_back(*p);
        return finish();
    }

    d.effective_definition.assign(timezone.begin(), timezone.end());
    if (posix_is_default_dst(timezone)) {
        auto p = tz_file_actual_path("posixrules");
        if (!p) return std::nullopt;
        d.kind = TimezoneSourceKind::PosixDefaultDst;
        d.resource_paths.push_back(*p);
        return finish();
    }
    if (timezone.find(',') != std::string_view::npos) {
        d.kind = TimezoneSourceKind::PosixExplicit;
        return finish();
    }
    d.kind = TimezoneSourceKind::FixedOffset;
    return finish();
}

std::optional<CivilResolution> resolve_civil(std::string_view timezone,
                                             int year,
                                             int month,
                                             int day,
                                             int hour,
                                             int minute,
                                             int second) {
    if (!timezone_spec_accepted(timezone)) return std::nullopt;
    return resolve_stamp(std::string(timezone),
                         CivilStamp{year, month, day, hour, minute, second});
}

bool SessionCalendar::valid() const noexcept {
    if (!valid_) return false;
    if (timezone_.empty()) return false;
    if ((day_mask_ & 0x01) != 0) return false;
    if ((day_mask_ & 0xFE) == 0) return false;
    if (all_day_) return windows_.empty() && origin_minutes_ == 0;
    if (windows_.empty()) return false;
    if (origin_minutes_ < 0 || origin_minutes_ >= 1440) return false;
    if (origin_minutes_ != windows_.front().start_minutes()) return false;
    for (const SessionWindow& w : windows_) {
        if (!w.valid()) return false;
    }
    return true;
}

std::optional<Timeframe> parse_timeframe(std::string_view text) {
    if (text.empty()) return std::nullopt;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) return std::nullopt;
    }
    char suffix = 0;
    std::string_view digits = text;
    const char last = text.back();
    if (last == 'S' || last == 'D' || last == 'W' || last == 'M') {
        suffix = last;
        digits = text.substr(0, text.size() - 1);
    } else if (last < '0' || last > '9') {
        return std::nullopt;
    }
    int count = 1;
    if (digits.empty()) {
        if (suffix == 'S') return std::nullopt;
        if (suffix == 0) return std::nullopt;
        count = 1;
    } else {
        if (digits[0] == '0') return std::nullopt;
        int64_t n = 0;
        const int64_t count_max = std::numeric_limits<int>::max();
        for (char c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            const int digit = c - '0';
            if (n > (kInt64Max - digit) / 10) return std::nullopt;
            n = n * 10 + digit;
            if (n > count_max) return std::nullopt;
        }
        if (n < 1) return std::nullopt;
        count = static_cast<int>(n);
    }
    TimeframeUnit unit = TimeframeUnit::Minute;
    switch (suffix) {
    case 'S':
        unit = TimeframeUnit::Second;
        break;
    case 'D':
        unit = TimeframeUnit::Day;
        break;
    case 'W':
        unit = TimeframeUnit::Week;
        break;
    case 'M':
        unit = TimeframeUnit::Month;
        break;
    default:
        unit = TimeframeUnit::Minute;
        break;
    }
    if (unit == TimeframeUnit::Second || unit == TimeframeUnit::Minute) {
        int64_t dummy = 0;
        if (!mul_ok(count, unit_ms(unit), dummy)) return std::nullopt;
    }
    return Timeframe(unit, count, std::string(text));
}

TimeframeCompatibility compatibility(const Timeframe& input, const Timeframe& script) {
    TimeframeCompatibility out;
    if (!input.valid() || !script.valid()) {
        out.pairing = TimeframePairing::Invalid;
        return out;
    }
    if (input.literal() == script.literal()) {
        out.pairing = TimeframePairing::Passthrough;
        out.group_factor = 1;
        return out;
    }
    if (input.unit() == script.unit()) {
        if (script.count() < input.count()) {
            out.pairing = TimeframePairing::ScriptFiner;
            return out;
        }
        if (script.count() % input.count() == 0) {
            out.pairing = TimeframePairing::SameUnitMultiple;
            out.group_factor = script.count() / input.count();
            return out;
        }
        out.pairing = TimeframePairing::IndivisibleFixed;
        return out;
    }
    if (input.is_fixed() && script.is_fixed()) {
        int64_t in_ms = 0;
        int64_t sc_ms = 0;
        if (!mul_ok(input.count(), unit_ms(input.unit()), in_ms)
            || !mul_ok(script.count(), unit_ms(script.unit()), sc_ms)) {
            out.pairing = TimeframePairing::IndivisibleFixed;
            return out;
        }
        if (sc_ms < in_ms) {
            out.pairing = TimeframePairing::ScriptFiner;
            return out;
        }
        if (in_ms > 0 && sc_ms % in_ms == 0) {
            out.pairing = TimeframePairing::FixedDivisible;
            out.group_factor = sc_ms / in_ms;
            return out;
        }
        out.pairing = TimeframePairing::IndivisibleFixed;
        return out;
    }
    const auto in_secs = known_period_seconds(input);
    const auto sc_secs = known_period_seconds(script);
    if (in_secs && sc_secs && *sc_secs < *in_secs) {
        out.pairing = TimeframePairing::ScriptFiner;
        return out;
    }
    if (input.is_fixed() && script.is_calendar()) {
        out.pairing = TimeframePairing::FixedToCalendar;
        return out;
    }
    if ((input.unit() == TimeframeUnit::Day
         && (script.unit() == TimeframeUnit::Week || script.unit() == TimeframeUnit::Month))
        || (input.unit() == TimeframeUnit::Week && script.unit() == TimeframeUnit::Month)) {
        out.pairing = TimeframePairing::CalendarToCalendar;
        return out;
    }
    if (script.is_fixed()
        || (input.unit() == TimeframeUnit::Week && script.unit() == TimeframeUnit::Day)
        || (input.unit() == TimeframeUnit::Month
            && (script.unit() == TimeframeUnit::Day || script.unit() == TimeframeUnit::Week))) {
        out.pairing = TimeframePairing::ScriptFiner;
        return out;
    }
    out.pairing = TimeframePairing::IndivisibleFixed;
    return out;
}

TimeframeCompatibility stream_compatibility(const Timeframe& input, const Timeframe& script) {
    if (!input.valid() || !script.valid()) {
        TimeframeCompatibility out;
        out.pairing = TimeframePairing::Invalid;
        return out;
    }
    if (input.unit() == TimeframeUnit::Month) {
        TimeframeCompatibility out;
        out.pairing = TimeframePairing::StreamMonthlyInputRefused;
        return out;
    }
    return compatibility(input, script);
}

std::optional<SessionCalendar> parse_session(std::string_view session, std::string_view timezone) {
    if (!timezone_spec_accepted(timezone)) return std::nullopt;
    std::string tz(timezone.begin(), timezone.end());
    if (tz.empty()) tz = "UTC";
    std::string literal(session.begin(), session.end());
    std::string body = literal;
    trim(body);
    std::uint8_t day_mask = 0xFE;
    if (body.empty() || body == "24x7") {
        return SessionCalendar(std::move(tz), std::move(literal), {}, day_mask, 0, true);
    }
    const auto colon = body.rfind(':');
    if (colon != std::string::npos && colon + 1 < body.size()
        && all_mask_digits(std::string_view(body).substr(colon + 1))) {
        day_mask = 0;
        for (std::size_t i = colon + 1; i < body.size(); ++i) {
            day_mask = static_cast<std::uint8_t>(day_mask | (1u << (body[i] - '0')));
        }
        if ((day_mask & 0xFE) == 0) return std::nullopt;
        body = body.substr(0, colon);
        trim(body);
    }
    if (body.empty() || body == "24x7") {
        return SessionCalendar(std::move(tz), std::move(literal), {}, day_mask, 0, true);
    }
    std::vector<SessionWindow> windows;
    int origin_minutes = 0;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const auto comma = body.find(',', pos);
        std::string seg = body.substr(pos, comma == std::string::npos ? std::string::npos
                                                                      : comma - pos);
        trim(seg);
        if (seg.empty()) return std::nullopt;
        const auto dash = seg.find('-');
        if (dash != 4 || seg.size() != 9) return std::nullopt;
        const int start = parse_hhmm(std::string_view(seg).substr(0, 4), false);
        const int end = parse_hhmm(std::string_view(seg).substr(5, 4), true);
        if (start < 0 || end < 0) return std::nullopt;
        const bool full_day = (end != 1440 && start == end);
        const bool wraps = (!full_day && end < start);
        SessionWindow w(start, end, wraps, full_day);
        if (!w.valid()) return std::nullopt;
        if (windows.empty()) origin_minutes = start;
        windows.push_back(w);
        if (comma == std::string::npos) break;
        pos = comma + 1;
        if (pos > body.size()) return std::nullopt;
    }
    if (windows.empty()) return std::nullopt;
    return SessionCalendar(std::move(tz), std::move(literal), std::move(windows), day_mask,
                           origin_minutes, false);
}

bool in_session(const SessionCalendar& calendar, int64_t ms) {
    if (!calendar.valid()) return false;
    auto day = session_day_containing(calendar, ms);
    if (!day) return false;
    return ms_in_spans(day->spans, ms);
}

std::optional<int64_t> session_day_ordinal(const SessionCalendar& calendar, int64_t ms) {
    if (!calendar.valid()) return std::nullopt;
    auto day = session_day_containing(calendar, ms);
    if (!day) return std::nullopt;
    return days_from_civil(day->trading_date.year, static_cast<unsigned>(day->trading_date.month),
                           static_cast<unsigned>(day->trading_date.day));
}

std::optional<int64_t> session_week_ordinal(const SessionCalendar& calendar, int64_t ms) {
    if (!calendar.valid()) return std::nullopt;
    auto day = session_day_containing(calendar, ms);
    if (!day) return std::nullopt;
    const int64_t trading_days =
        days_from_civil(day->trading_date.year, static_cast<unsigned>(day->trading_date.month),
                        static_cast<unsigned>(day->trading_date.day));
    const int dow_mon0 = (weekday_sun0(day->trading_date) + 6) % 7;
    const int64_t monday_days = trading_days - dow_mon0;
    return floor_div(monday_days - kWeekZeroMondayDays, 7);
}

std::optional<int64_t> session_month_ordinal(const SessionCalendar& calendar, int64_t ms) {
    if (!calendar.valid()) return std::nullopt;
    auto day = session_day_containing(calendar, ms);
    if (!day) return std::nullopt;
    return static_cast<int64_t>(day->trading_date.year) * 12 + (day->trading_date.month - 1);
}

std::optional<int64_t> period_key(const SessionCalendar& calendar, const Timeframe& tf, int64_t ms) {
    if (!calendar.valid() || !tf.valid()) return std::nullopt;
    if (tf.unit() == TimeframeUnit::Day) {
        auto ord = session_day_ordinal(calendar, ms);
        if (!ord) return std::nullopt;
        return floor_div(*ord, tf.count()) * tf.count();
    }
    if (tf.unit() == TimeframeUnit::Week) {
        auto ord = session_week_ordinal(calendar, ms);
        if (!ord) return std::nullopt;
        return floor_div(*ord, tf.count()) * tf.count();
    }
    if (tf.unit() == TimeframeUnit::Month) {
        auto ord = session_month_ordinal(calendar, ms);
        if (!ord) return std::nullopt;
        return floor_div(*ord, tf.count()) * tf.count();
    }
    auto core = core_containing(calendar, tf, ms);
    if (!core) return std::nullopt;
    return core->open_ms;
}

std::optional<NativeInterval> interval_containing(const SessionCalendar& calendar,
                                                  const Timeframe& script_tf,
                                                  const Timeframe& input_tf,
                                                  int64_t ms) {
    if (!calendar.valid() || !script_tf.valid() || !input_tf.valid()) return std::nullopt;
    auto core = core_containing(calendar, script_tf, ms);
    if (!core) return std::nullopt;
    auto next_in = first_tradable_open(calendar, input_tf, core->last_traded_close_ms);
    if (!next_in) return std::nullopt;
    NativeInterval iv;
    iv.open_ms = core->open_ms;
    iv.eligible_open_ms = core->eligible_open_ms;
    iv.last_traded_close_ms = core->last_traded_close_ms;
    iv.next_period_open_ms = core->next_period_open_ms;
    iv.next_input_open_ms = *next_in;
    return iv;
}

std::optional<NativeInterval> interval_containing(const SessionCalendar& calendar,
                                                  const Timeframe& tf,
                                                  int64_t ms) {
    return interval_containing(calendar, tf, tf, ms);
}

}  // inline namespace native_calendar_v2
}  // namespace pineforge::native_calendar
