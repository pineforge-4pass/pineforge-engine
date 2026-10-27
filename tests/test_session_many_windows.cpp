#include "global_allocation_replacement.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

#include <pineforge/session_time.hpp>
#include <pineforge/timeframe.hpp>

namespace {

constexpr int64_t kMinute = 60000;
// 1970-01-01 was Thursday; four days later is Monday 1970-01-05 UTC.
constexpr int64_t kMonday = 4 * 24 * 60 * kMinute;
int failures = 0;

void expect(int windows, const char* what, int minute,
            int64_t actual, int64_t expected) {
    if (actual != expected) {
        std::printf("FAIL %d-window %s at minute %d: got %lld, want %lld\n",
                    windows, what, minute, static_cast<long long>(actual),
                    static_cast<long long>(expected));
        ++failures;
    }
}

std::string hourly_session(int windows) {
    std::string session;
    for (int hour = 1; hour <= windows; ++hour) {
        char window[16];
        std::snprintf(window, sizeof(window), "%02d00-%02d30", hour, hour);
        if (!session.empty()) session += ',';
        session += window;
    }
    return session + ":23456";
}

void check_session(int windows) {
    const std::string session = hourly_session(windows);
    // Hand oracle: windows are [01:00,01:30), [02:00,02:30), ...,
    // [N:00,N:30). The day closes at minute 60*N+30, not minute 90.
    // N=9: [01:00,09:30) spans 510 minutes, close minute 570.
    // N=8 control: [01:00,08:30) spans 450 minutes, close minute 510.
    // N=17 also exceeds a hypothetical replacement cap of sixteen.
    const int last_bar_minute = windows * 60 + 15;
    const int64_t last_bar = kMonday + last_bar_minute * kMinute;
    const int64_t day_close = kMonday + (windows * 60 + 30) * kMinute;

    // Public client interface, with the symbol session in the final slot:
    // an explicit session argument uses a different parser and alone would
    // miss the timeframe schedule defect.
    expect(windows, "time_close(D)", last_bar_minute,
           pineforge::timeframe_time_close(last_bar, "D", "", "", "15",
                                           "UTC", session),
           day_close);
    expect(windows, "session_period_close(D)", last_bar_minute,
           pineforge::session_period_close_ms(last_bar, "UTC", session,
                                              pineforge::CalendarPeriod::DAY),
           day_close);
    expect(windows, "explicit-session time_close(D)", last_bar_minute,
           pineforge::timeframe_time_close(last_bar, "D", session, "UTC",
                                           "15", "UTC", "24x7"),
           day_close);

    pineforge::TimeframeAggregator daily("D", "15", "UTC", session);
    pineforge::TimeframeAggregator hourly("60", "15", "UTC", session);
    for (int hour = 1; hour <= windows; ++hour) {
        for (int quarter = 0; quarter < 2; ++quarter) {
            const int minute = hour * 60 + quarter * 15;
            const pineforge::Bar bar{100.0, 101.0, 99.0, 100.0, 1.0,
                                     kMonday + minute * kMinute};
            const auto day = daily.feed(bar);
            const auto bucket = hourly.feed(bar);

            // Each hour contains two 15m bars, then a 30m break. An hourly
            // bucket completes on its second bar; the daily bucket waits
            // through every earlier break until the final window's end.
            const bool final_bar = hour == windows && quarter == 1;
            expect(windows, "daily completion", minute,
                   day.is_complete, final_bar);
            expect(windows, "hourly completion", minute,
                   bucket.is_complete, quarter == 1);
            if (quarter == 1) {
                expect(windows, "hourly child count", minute,
                       bucket.sub_bar_count, 2);
            }
            if (final_bar) {
                // Two unit-volume bars per window: 2*N children and volume.
                expect(windows, "daily child count", minute,
                       day.sub_bar_count, 2 * windows);
                if (day.bar.volume != 2.0 * windows) {
                    std::printf("FAIL %d-window daily volume: got %.17g, want %.17g\n",
                                windows, day.bar.volume, 2.0 * windows);
                    ++failures;
                }
            }
        }
    }
}

void check_normalized_storage_routes() {
    const std::string sessions[] = {
        "2200-0030,0100-0130,0200-0230,0300-0330,0400-0430,0500-0530,0600-0630,0700-0730:23456",
        "2200-2300,2300-0030,0100-0130,0200-0230,0300-0330,0400-0430,0500-0530,0600-0630,0700-0730:23456"
    };
    // Hand oracle: splitting [22:00,00:30) at 23:00 changes eight declared
    // windows to nine, but touching pieces merge back to the same schedule.
    // Both include a midnight-crossing window. Relative to Monday 22:00,
    // the merged windows are [0,150), [180,210), [240,270), [300,330),
    // [360,390), [420,450), [480,510), [540,570). Thus the day closes at
    // Monday minute 1320+570=1890, i.e. Tuesday 07:30. Fifteen-minute bars
    // give 10+7*2=24 children, regardless of the declared-window count.
    const auto active = [](int offset) {
        return offset < 150 || (offset >= 180 && offset % 60 < 30);
    };
    const std::string utc = "UTC";
    const int64_t origin = kMonday + 1320 * kMinute;
    const int64_t day_close = origin + 570 * kMinute;
    for (int route = 0; route < 2; ++route) {
        const int windows = 8 + route;
        const std::string& session = sessions[route];
        pineforge::TimeframeAggregator daily("D", "15", utc, session);
        pineforge::TimeframeAggregator hourly("60", "15", utc, session);
        int children = 0;
        for (int offset = 0; offset < 570; offset += 15) {
            if (!active(offset)) continue;
            const int minute = 1320 + offset;
            const int64_t timestamp = origin + offset * kMinute;
            const auto before_close = global_allocation::allocations;
            const int64_t close = pineforge::session_period_close_ms(
                timestamp, utc, session, pineforge::CalendarPeriod::DAY);
            const auto close_allocations =
                global_allocation::allocations - before_close;
            expect(windows, "normalized daily close", minute, close, day_close);
            // This public call parses on every bar. Zero vs nonzero counts
            // distinguish the storage routes and reject the always-vector fix.
            expect(windows, "schedule heap route", minute,
                   close_allocations > 0, route == 1);
            expect(windows, "normalized time_close(D)", minute,
                   pineforge::timeframe_time_close(timestamp, "D", "", "", "15",
                                                   utc, session),
                   day_close);

            int next = offset + 15;
            while (next < 570 && !active(next)) next += 15;
            // The final bar names the next session's open. Wrapped days
            // complete through the existing next-input boundary rule.
            const int64_t next_timestamp = origin + (next < 570 ? next : 1440) * kMinute;
            const pineforge::Bar bar{100.0, 101.0, 99.0, 100.0, 1.0, timestamp};
            const auto before_day = global_allocation::allocations;
            const auto day = daily.feed(bar, next_timestamp);
            const auto day_allocations = global_allocation::allocations - before_day;
            const auto before_hour = global_allocation::allocations;
            const auto bucket = hourly.feed(bar, next_timestamp);
            const auto hour_allocations = global_allocation::allocations - before_hour;
            if (route == 0) {
                expect(windows, "daily feed heap allocations", minute, day_allocations, 0);
                expect(windows, "hourly feed heap allocations", minute, hour_allocations, 0);
            }

            ++children;
            expect(windows, "normalized daily completion", minute,
                   day.is_complete, offset == 555);
            expect(windows, "normalized daily child count", minute,
                   day.sub_bar_count, children);
            expect(windows, "normalized daily label", minute, day.bar.timestamp, origin);
            // The first 150 minutes make buckets of four, four, and two
            // children; each later half-hour window makes two children.
            const bool hour_done = offset == 45 || offset == 105 || offset == 135
                || (offset >= 180 && offset % 60 == 15);
            expect(windows, "normalized hourly completion", minute,
                   bucket.is_complete, hour_done);
            expect(windows, "normalized hourly child count", minute,
                   bucket.sub_bar_count, offset % 60 / 15 + 1);
            expect(windows, "normalized hourly label", minute,
                   bucket.bar.timestamp, origin + (offset / 60) * 60 * kMinute);
            if (offset == 555) {
                expect(windows, "normalized final daily children", minute, children, 24);
                if (day.bar.volume != 24.0) {
                    std::printf("FAIL %d-window normalized daily volume: got %.17g, want 24\n",
                                windows, day.bar.volume);
                    ++failures;
                }
            }
        }
    }
}

}  // namespace

int main() {
    check_session(9);
    check_session(8);
    check_session(17);
    check_normalized_storage_routes();
    return failures == 0 ? 0 : 1;
}
