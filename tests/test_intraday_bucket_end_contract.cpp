// EXPECTATION: engine behaviour, not a TradingView measurement
//
// Session membership caps valid bucket ends at the declared session close.
// Outside-session points must still produce nonnegative bucket lengths.
// UTC widths dividing 1440 minutes retain the legacy epoch-grid algebra.

#include <pineforge/session_time.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace {

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kDay = 1440 * kMinute;
constexpr const char* kNy = "America/New_York";
constexpr const char* kSession = "0930-1600";

struct SessionDate {
    std::int64_t utc_midnight;
    int utc_offset_hours;
};

// 2025-06-10 (EDT) and 2025-01-15 (EST), away from DST transitions.
constexpr SessionDate kDates[] = {
    {1749513600000LL, 4},
    {1736899200000LL, 5},
};

int checks = 0;
int failures = 0;

void expect(bool condition, const char* rule, std::int64_t stamp, int width,
            std::int64_t actual, std::int64_t expected) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s stamp=%lld width=%d actual=%lld expected=%lld\n",
                    rule, static_cast<long long>(stamp), width,
                    static_cast<long long>(actual), static_cast<long long>(expected));
    }
}

std::int64_t local_ms(const SessionDate& date, int minute) {
    return date.utc_midnight + (date.utc_offset_hours * 60 + minute) * kMinute;
}

void outside_session_lengths() {
    constexpr int widths[] = {30, 60, 90, 120};
    constexpr int minutes[] = {0, 8 * 60, 9 * 60 + 29, 16 * 60,
                               16 * 60 + 10, 17 * 60, 18 * 60 + 45, 23 * 60 + 59};
    for (const SessionDate& date : kDates) {
        for (int width : widths) {
            for (int minute : minutes) {
                const std::int64_t stamp = local_ms(date, minute);
                const std::int64_t open = pineforge::session_intraday_bucket_open_ms(
                    stamp, width * 60, kNy, kSession);
                const std::int64_t end = pineforge::session_intraday_bucket_end_ms(
                    stamp, width * 60, kNy, kSession);
                expect(end >= open, "outside-session end >= open", stamp, width, end, open);
                expect(open <= stamp, "open <= input", stamp, width, open, stamp);
            }
        }
        // One hour past the declared close: the unchanged 60m grid opens
        // at 16:30. Clamping produces an empty, already-closed bucket there.
        struct EmptyRow { int minute; int open; };
        constexpr EmptyRow empty_rows[] = {
            {17 * 60, 16 * 60 + 30},
            {18 * 60 + 45, 18 * 60 + 30},
        };
        for (const EmptyRow& row : empty_rows) {
            const std::int64_t stamp = local_ms(date, row.minute);
            const std::int64_t expected = local_ms(date, row.open);
            const std::int64_t open = pineforge::session_intraday_bucket_open_ms(
                stamp, 3600, kNy, kSession);
            const std::int64_t end = pineforge::session_intraday_bucket_end_ms(
                stamp, 3600, kNy, kSession);
            expect(open == expected, "after-close grid open", stamp, 60, open, expected);
            expect(end == expected, "after-close empty end", stamp, 60, end, expected);
        }
    }
}

void declared_session_close() {
    struct Row { int minute; int open; int end; };
    constexpr Row rows[] = {
        {9 * 60 + 30, 9 * 60 + 30, 10 * 60 + 30},
        {14 * 60 + 30, 14 * 60 + 30, 15 * 60 + 30},
        {14 * 60 + 47, 14 * 60 + 30, 15 * 60 + 30},
        {15 * 60 + 30, 15 * 60 + 30, 16 * 60},
        {15 * 60 + 59, 15 * 60 + 30, 16 * 60},
    };
    for (const SessionDate& date : kDates) {
        for (const Row& row : rows) {
            const std::int64_t stamp = local_ms(date, row.minute);
            const std::int64_t expected_open = local_ms(date, row.open);
            const std::int64_t expected_end = local_ms(date, row.end);
            const std::int64_t open = pineforge::session_intraday_bucket_open_ms(
                stamp, 3600, kNy, kSession);
            const std::int64_t end = pineforge::session_intraday_bucket_end_ms(
                stamp, 3600, kNy, kSession);
            // Empty explicit-session argument deliberately reaches the
            // symbol-session consumer, not session_argument_time's branch:
            // pine_time_close -> timeframe_time_close (seven arguments)
            // -> compute_tf_close_ms -> session_intraday_bucket_end_ms.
            const std::int64_t pine_end = pineforge::pine_time_close(
                stamp, "60", "", "", "15", kNy, kSession);
            expect(open == expected_open, "session grid open", stamp, 60, open, expected_open);
            expect(end == expected_end, "declared session end", stamp, 60, end, expected_end);
            expect(pine_end == expected_end, "symbol-session time_close", stamp, 60,
                   pine_end, expected_end);
        }
    }
}

void utc_dividing_widths() {
    constexpr int widths[] = {120, 180, 240, 360, 480};
    constexpr std::int64_t offsets[] = {0, 1, 137 * kMinute + 12345,
                                       721 * kMinute, 1439 * kMinute + 59999};
    for (const SessionDate& date : kDates) {
        for (int day_offset : {-1, 0, 1}) {
            for (int width : widths) {
                const std::int64_t duration = width * kMinute;
                for (std::int64_t offset : offsets) {
                    const std::int64_t stamp = date.utc_midnight + day_offset * kDay + offset;
                    // width | 1440: UTC midnight is an epoch-grid boundary,
                    // and every bucket, including the last, ends by midnight.
                    // These independent epoch expectations do not reuse an
                    // actual open/end return value as their oracle.
                    const std::int64_t legacy_open = (stamp / duration) * duration;
                    const std::int64_t legacy_end = legacy_open + duration;
                    const std::string tf = std::to_string(width);
                    for (const char* session : {"", "24x7"}) {
                        const std::int64_t open = pineforge::session_intraday_bucket_open_ms(
                            stamp, width * 60, "UTC", session);
                        const std::int64_t end = pineforge::session_intraday_bucket_end_ms(
                            stamp, width * 60, "UTC", session);
                        const std::int64_t pine_end = pineforge::pine_time_close(
                            stamp, tf, "", "", "15", "UTC", session);
                        expect(open == legacy_open, "UTC legacy open", stamp, width,
                               open, legacy_open);
                        expect(end == legacy_end, "UTC legacy end", stamp, width,
                               end, legacy_end);
                        expect(pine_end == legacy_end, "UTC legacy time_close", stamp, width,
                               pine_end, legacy_end);
                    }
                }
            }
        }
    }
}

} // namespace

int main() {
    std::puts("EXPECTATION: engine behaviour, not a TradingView measurement");
    outside_session_lengths();
    declared_session_close();
    utc_dividing_widths();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
