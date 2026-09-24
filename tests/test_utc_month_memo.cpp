// R5 lane D2-D. The monthly Sharpe / Sortino walk (src/engine_metrics.cpp)
// keys every equity point by its UTC (year, month); it now keeps the key with
// the seconds of its civil month (detail::UtcMonthMemo) and reads a point
// inside that month without the civil arithmetic.
//
// Three rows, all against month_key_utc itself and against the walk as it was
// at 6c081f5d (tests/equity_stats_reference.hpp, transcribed verbatim):
//   * the kept key equals month_key_utc at every point of fixed-step walks
//     (1 s to 1 W, forward and backward), of session-shaped walks with nightly
//     and weekend gaps, of random-order reads, at every month boundary of
//     1900-2100 to the millisecond (pre-epoch truncation included), at every
//     day of years 1-9999, and across the +/-2^40-second edge of the
//     arithmetic;
//   * the kept range is exactly the civil month of the second's floor day --
//     computed here by days_from_civil, the inverse the walk does not use --
//     clamped to the arithmetic's span, and a second beyond the span keeps
//     nothing;
//   * compute_equity_stats equals the 6c081f5d walk field for field, bit for
//     bit, over curves of every bar width, UTC spelling and length, and over
//     two chart timezones whose walk the memo does not touch.
#include <pineforge/metrics.hpp>

#include "../src/metrics_month_memo.hpp"
#include "equity_stats_reference.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

namespace detail = pineforge::metrics::detail;
namespace ref = equity_stats_reference;

int failures = 0;
long long checks = 0;

std::map<int, long long> failed_lines;  // a failing CHECK's line -> its failures

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++checks;                                                               \
        if (!(cond)) {                                                          \
            ++failed_lines[__LINE__];                                           \
            if (++failures <= 20)                                               \
                std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                       \
    } while (0)

#if defined(__SANITIZE_ADDRESS__)
constexpr std::int64_t kStride = 15;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr std::int64_t kStride = 15;
#else
constexpr std::int64_t kStride = 1;
#endif
#else
constexpr std::int64_t kStride = 1;
#endif

constexpr std::int64_t kSecond = 1000;
constexpr std::int64_t kMinute = 60 * kSecond;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kCivilSpan = std::int64_t{1} << 40;

// Howard Hinnant's days_from_civil: the day number of y-m-d (proleptic
// Gregorian). The walk only runs the other direction (civil_from_days), so
// the month bounds below are an independent answer.
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Floor division for the key's (year, month0).
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

long long kept_months = 0;
long long reads = 0;

// One read through the memo: its key against month_key_utc and the 6c081f5d
// body, and what it keeps against the month's own seconds.
void read(detail::UtcMonthMemo& memo, std::int64_t ts_ms) {
    ++reads;
    const detail::UtcMonthMemo before = memo;
    const int direct = detail::month_key_utc(ts_ms);
    const int kept = detail::month_key_utc(memo, ts_ms);
    CHECK(kept == direct);
    CHECK(kept == ref::month_key_utc(ts_ms));
    if (kept != direct && failures <= 20) {
        std::fprintf(stderr, "  ts_ms=%" PRId64 " kept %d direct %d\n", ts_ms, kept, direct);
    }
    const std::int64_t secs = ts_ms / 1000;
    if (secs > -kCivilSpan && secs < kCivilSpan) {
        const std::int64_t year = floor_div(direct, 12);
        const std::int64_t month = direct - year * 12 + 1;
        const std::int64_t first = days_from_civil(year, month, 1);
        const std::int64_t next = month == 12 ? days_from_civil(year + 1, 1, 1)
                                              : days_from_civil(year, month + 1, 1);
        const std::int64_t lo = std::max(first * 86400, -kCivilSpan + 1);
        const std::int64_t hi = std::min(next * 86400, kCivilSpan);
        CHECK(lo <= secs && secs < hi);
        CHECK(memo.lo == lo);
        CHECK(memo.hi == hi);
        CHECK(memo.key == direct);
        if (memo.lo != before.lo || memo.hi != before.hi) ++kept_months;
    } else {
        // gmtime_r's answer: nothing kept, the memo as it was.
        CHECK(memo.lo == before.lo && memo.hi == before.hi && memo.key == before.key);
    }
}

void walk(std::int64_t from, std::int64_t to, std::int64_t step, bool backward = true) {
    detail::UtcMonthMemo memo;
    for (std::int64_t ts = from; ts < to; ts += step) read(memo, ts);
    if (!backward) return;
    detail::UtcMonthMemo back;
    for (std::int64_t ts = to - 1; ts >= from; ts -= step) read(back, ts);
}

void fixed_steps() {
    const std::int64_t y1970 = 0;
    const std::int64_t y2000 = 946684800LL * kSecond;
    const std::int64_t y2024 = 1704067200LL * kSecond;
    walk(y2024 - 3 * kDay, y2024 + 3 * kDay, kSecond * kStride);
    walk(y2024 - 40 * kDay, y2024 + 40 * kDay, 59 * kSecond);
    walk(y2000 - 400 * kDay, y2000 + 400 * kDay, kMinute * kStride);
    walk(y1970 - 800 * kDay, y2024 + 800 * kDay, 15 * kMinute * kStride);
    walk(-2208988800LL * kSecond, 4133980800LL * kSecond, kHour * 4 * kStride);  // 1900-2101
    walk(-62135596800LL * kSecond, 253402300800LL * kSecond, kDay * kStride, false);  // years 1-9999
    walk(-62135596800LL * kSecond, 253402300800LL * kSecond, 7 * kDay);
    walk(y1970 + 3 * kDay, y1970 + 4000 * 7 * kDay, 7 * kDay + 17 * kSecond);
}

// Session-shaped walks: 09:30-16:00 weekdays and a 23-hour futures day, the
// gaps crossing month ends on weekends and nights.
void session_walks() {
    const std::int64_t from = 1577836800LL * kSecond;  // 2020-01-01
    const std::int64_t to = 1830297600LL * kSecond;    // 2028-01-01
    detail::UtcMonthMemo stocks;
    detail::UtcMonthMemo futures;
    for (std::int64_t day = from; day < to; day += kDay) {
        const std::int64_t weekday = (day / kDay + 4) % 7;  // 0 = Sunday; 1970-01-01 was a Thursday
        if (weekday != 0 && weekday != 6) {
            for (std::int64_t ts = day + 14 * kHour + 30 * kMinute; ts < day + 21 * kHour;
                 ts += 5 * kMinute * kStride) {
                read(stocks, ts);
            }
        }
        if (weekday != 6) {
            for (std::int64_t ts = day + 23 * kHour; ts < day + kDay + 22 * kHour;
                 ts += kHour) {
                read(futures, ts);
            }
        }
    }
}

// Every month boundary 1900-2100, to the millisecond on both sides; the
// pre-epoch ones read the truncation of a negative stamp.
void month_boundaries() {
    detail::UtcMonthMemo memo;
    for (std::int64_t year = 1900; year <= 2100; ++year) {
        for (std::int64_t month = 1; month <= 12; ++month) {
            const std::int64_t at = days_from_civil(year, month, 1) * kDay;
            for (std::int64_t delta : {-86400001LL, -1001LL, -1000LL, -999LL, -1LL, 0LL, 1LL,
                                       999LL, 1000LL, 1001LL, 86399999LL}) {
                read(memo, at + delta);
            }
        }
    }
    detail::UtcMonthMemo around_zero;
    for (std::int64_t ts = -5000; ts <= 5000; ts += 7) read(around_zero, ts);
}

// Random-order reads: the memo keeps whichever month the last read had.
void random_order() {
    std::uint64_t mix = 0x243f6a8885a308d3ull;
    const auto next = [&mix]() {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        return mix;
    };
    detail::UtcMonthMemo memo;
    const std::int64_t span = 200LL * 365 * kDay;
    for (int i = 0; i < 400000 / static_cast<int>(kStride); ++i) {
        const std::int64_t base = -30LL * 365 * kDay + static_cast<std::int64_t>(next() % span);
        read(memo, base);
        read(memo, base + static_cast<std::int64_t>(next() % (40 * kDay)) - 20 * kDay);
    }
}

// The +/-2^40-second edge: the arithmetic's last seconds are kept, clamped
// to the span; gmtime_r's seconds beyond it keep nothing.
void span_edges() {
    for (std::int64_t sign : {1LL, -1LL}) {
        detail::UtcMonthMemo memo;
        const std::int64_t edge = sign * kCivilSpan;
        for (std::int64_t d = -3 * 86400; d <= 3 * 86400; d += 3607) {
            read(memo, (edge + d) * kSecond);
        }
        for (std::int64_t secs : {edge - 1, edge, edge + 1, edge - sign * 2, 2 * edge}) {
            read(memo, secs * kSecond);
            read(memo, secs * kSecond + (secs < 0 ? -999 : 999));
        }
    }
}

// ── compute_equity_stats against the 6c081f5d walk ───────────────────────
struct Curve {
    std::vector<pf_equity_point_t> points;
    double first_open = 0.0;
    double last_close = 0.0;
    std::int64_t in_market = 0;
    double net_profit = 0.0;
};

Curve curve(std::uint64_t seed, std::int64_t start, std::int64_t step, std::int64_t n,
            bool gaps, bool touches_zero) {
    std::uint64_t mix = seed * 0x9e3779b97f4a7c15ull + 1;
    const auto next = [&mix]() {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        return mix;
    };
    Curve c;
    double equity = 10000.0;
    std::int64_t ts = start;
    for (std::int64_t i = 0; i < n; ++i) {
        const double move = (static_cast<double>(next() % 2001) - 1000.0) / 25.0;
        equity += move;
        if (touches_zero && next() % 97 == 0) equity = next() % 2 ? 0.0 : -equity;
        const double open_profit = (static_cast<double>(next() % 401) - 200.0) / 8.0;
        c.points.push_back({ts, equity, open_profit});
        ts += step;
        if (gaps && next() % 5 == 0) ts += step * static_cast<std::int64_t>(next() % 40);
    }
    c.first_open = next() % 11 == 0 ? std::numeric_limits<double>::quiet_NaN()
                                    : 100.0 + static_cast<double>(next() % 1000) / 7.0;
    c.last_close = 100.0 + static_cast<double>(next() % 1000) / 3.0;
    c.in_market = n > 0 ? static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(n + 1)) : 0;
    c.net_profit = (static_cast<double>(next() % 20001) - 10000.0) / 3.0;
    return c;
}

long long stat_blocks = 0;

void compare(const Curve& c, double initial_capital, const std::string& tz) {
    ++stat_blocks;
    const auto n = static_cast<std::int64_t>(c.points.size());
    const pf_equity_point_t* data = c.points.empty() ? nullptr : c.points.data();
    const pf_equity_stats_t got = pineforge::metrics::compute_equity_stats(
        data, n, initial_capital, tz, c.first_open, c.last_close, c.in_market, c.net_profit);
    const pf_equity_stats_t want = ref::compute_equity_stats(
        data, n, initial_capital, tz, c.first_open, c.last_close, c.in_market, c.net_profit);
    const char* differs = ref::first_difference(got, want);
    CHECK(differs == nullptr);
    if (differs != nullptr && failures <= 20) {
        std::fprintf(stderr, "  tz='%s' n=%" PRId64 " first point %" PRId64 ": %s differs\n",
                     tz.c_str(), n, n > 0 ? c.points[0].time_ms : 0, differs);
    }
}

void equity_stats_equal_the_base_walk() {
    const std::int64_t starts[] = {
        -315619200LL * kSecond,   // 1960-01-01
        0,
        951782400LL * kSecond,    // 2000-02-29
        1704067200LL * kSecond - 3 * kDay,
        4102444800LL * kSecond,   // 2100-01-01
        (kCivilSpan - 400LL * 86400) * kSecond,
    };
    const std::int64_t steps[] = {kMinute, 15 * kMinute, kHour, 4 * kHour, kDay, 7 * kDay,
                                  30 * kDay + 10 * kHour};
    const std::int64_t lengths[] = {0, 1, 2, 3, 17, 400, 6000};
    std::uint64_t seed = 1;
    for (const char* tz : {"", "UTC", "Etc/UTC"}) {
        for (std::int64_t start : starts) {
            for (std::int64_t step : steps) {
                for (std::int64_t n : lengths) {
                    for (bool gaps : {false, true}) {
                        ++seed;
                        const bool touches_zero = seed % 3 == 0;
                        const Curve c = curve(seed, start, step, n / kStride + (n > 0 ? 1 : 0),
                                              gaps, touches_zero);
                        compare(c, 10000.0, tz);
                        compare(c, seed % 5 == 0 ? 0.0 : 25000.0, tz);
                    }
                }
            }
        }
    }
    // A chart timezone keeps its per-point localtime_r walk; the memo is not
    // on its path, and it answers what it always did.
    for (const char* tz : {"America/New_York", "Asia/Tokyo"}) {
        for (std::int64_t step : {kHour, kDay}) {
            const Curve c = curve(++seed, 1672531200LL * kSecond, step, 3000 / kStride, true, false);
            compare(c, 10000.0, tz);
        }
    }
}

}  // namespace

int main() {
    fixed_steps();
    session_walks();
    month_boundaries();
    random_order();
    span_edges();
    equity_stats_equal_the_base_walk();
    std::printf("test_utc_month_memo: %lld reads (%lld kept months) against month_key_utc and "
                "the 6c081f5d body, %lld statistics blocks bit for bit against the 6c081f5d walk\n",
                reads, kept_months, stat_blocks);
    CHECK(reads > 1000000 / kStride);
    CHECK(kept_months > 100000 / kStride);
    CHECK(stat_blocks > 1500);
    if (failures == 0) {
        std::printf("test_utc_month_memo: ok (%lld checks)\n", checks);
        return 0;
    }
    for (const auto& [line, count] : failed_lines)
        std::fprintf(stderr, "  line %d failed %lld times\n", line, count);
    std::fprintf(stderr, "test_utc_month_memo: %d of %lld checks failed\n", failures, checks);
    return 1;
}
