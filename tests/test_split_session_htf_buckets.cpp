// Where intraday bars open on a session with a break, and where its rows land.
//
// TradingView's charts of HKEX:700 (session 0930-1200,1300-1600,
// Asia/Hong_Kong) open 60-minute bars at 09:30, 10:00, 11:00, 13:00, 14:00 and
// 15:00, and 240-minute bars at 09:30 and 13:00. That is a grid counted from
// 09:00, half an hour before the session opens, whose first bar of the day
// opens at the open. A grid counted from the 09:30 open, as every other
// session below counts from its own, opens them at 09:30, 10:30 .. 15:30 and
// at 09:30, 13:30: one bar inside the lunch break, and one across it.
//
// The record is the session text, so the same local opens hold in any zone:
// the session's own, a daylight-saving zone on both sides of a clock change,
// and a zone whose offset is not a whole hour, where a grid counted from the
// epoch would land elsewhere.
//
// Controls, whose opens do not move:
//   - 0900-1130,1230-1530 in Asia/Tokyo. On TradingView's TSE:7203 charts the
//     60-minute bar that opens at 12:00, inside the break, holds the 12:30
//     reopen, and the 240-minute bars open at 09:00 and 13:00, with none
//     between them: the 09:00 bar holds the morning and the half hour after
//     the reopen (test_session_break_open_bar_tape.cpp replays those tapes);
//   - 1900-0745,0830-1320 in America/Chicago. On CBOT:ZC1! the 60-minute bar
//     that opens at 08:00 holds the 08:30 reopen, and the 240-minute bars open
//     every four hours from 19:00: the 07:00 bar holds the minutes before the
//     07:45 break and those from the 08:30 reopen to 11:00 (the same replay);
//   - 0930-1600 in America/New_York: 09:30 .. 15:30, and 09:30, 13:30 (NYSE:F,
//     test_pine_time_day_stamp_grid.cpp);
//   - 24x7 in UTC: the epoch grid.
//
// Every row of a complete session lands in exactly one published bar: the one
// whose open is the last written-out open at or before the row. A bar that
// holds trading on both sides of a break is one bar with both parts. It is
// not a bar cut at the break, with the rows after the reopen in no bar.
//
// Every expected open is written out by hand from those readings, and each
// row's bar follows from the written-out opens alone. The feed is synthetic:
// one flat one-minute bar for every minute of every window of a few plain
// dates. Each session is read four ways, which must agree: the bars
// TimeframeAggregator(tf, "1", zone, session) completes through feed(), as
// tools/aggregate_feed.cpp drives it; the bars its fixed clock completes, as
// the kernel drives a timeframe subscription; and the native calendar's
// interval of every minute, without and with its session-day memo.
//
// Source-free: this TU links the generic kernel alone and runs in the
// kernel-only profile.

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_calendar_memo.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace nc = pineforge::native_calendar;
using pineforge::AggregatedBar;
using pineforge::Bar;
using pineforge::FixedPeriodClock;
using pineforge::TimeframeAggregator;

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

constexpr std::int64_t kMinute = 60000;

// A civil date in the session's zone.
struct CivilDay {
    int year;
    int month;
    int day;
};

// A civil time of a session day: `next_day` is 0 on the day itself and 1 on
// the civil day after it (an overnight session).
struct WallClock {
    int next_day;
    int hour;
    int minute;
};

// One trading window: its open and its length in minutes.
struct TradingWindow {
    WallClock open;
    int minutes;
};

struct SessionCase {
    const char* name;
    const char* session;
    const char* zone;
    std::vector<TradingWindow> windows;
    std::vector<CivilDay> days;
    std::vector<WallClock> opens_60;
    std::vector<WallClock> opens_240;
};

std::int64_t civil_ms(const char* zone, const CivilDay& day, const WallClock& at) {
    const auto resolved =
        nc::resolve_civil(zone, day.year, day.month, day.day + at.next_day, at.hour, at.minute);
    CHECK(resolved.has_value());
    return resolved ? resolved->epoch_ms : 0;
}

// `count` opens `step_hours` apart from `first`, rolling into the next civil
// day.
std::vector<WallClock> every(WallClock first, int step_hours, int count) {
    std::vector<WallClock> out;
    for (int k = 0; k < count; ++k) {
        const int hour = first.hour + k * step_hours;
        out.push_back(WallClock{first.next_day + hour / 24, hour % 24, first.minute});
    }
    return out;
}

// Every minute of every window of one session day, flat.
std::vector<Bar> minutes_of(const SessionCase& c, const CivilDay& day) {
    std::vector<Bar> bars;
    for (const TradingWindow& window : c.windows) {
        const std::int64_t open = civil_ms(c.zone, day, window.open);
        for (int k = 0; k < window.minutes; ++k) {
            Bar bar{};
            bar.open = bar.high = bar.low = bar.close = 100.0;
            bar.volume = 1.0;
            bar.timestamp = open + k * kMinute;
            bars.push_back(bar);
        }
    }
    return bars;
}

// The opens written out for one session day, as instants.
std::vector<std::int64_t> instants(const SessionCase& c, const CivilDay& day,
                                   const std::vector<WallClock>& opens) {
    std::vector<std::int64_t> out;
    for (const WallClock& open : opens) out.push_back(civil_ms(c.zone, day, open));
    return out;
}

// What one reading publishes over the feed: where each of its bars opens, in
// order, and for every feed row how many published bars hold it and where the
// last of them opens.
struct Reading {
    std::vector<std::int64_t> opens;
    std::vector<int> bars;
    std::vector<std::int64_t> open_of;
};

Reading nothing_published(std::size_t rows) {
    Reading reading;
    reading.bars.assign(rows, 0);
    reading.open_of.assign(rows, 0);
    return reading;
}

// One published bar that opens at `open` and holds `rows` feed rows from
// `first` on.
void publish(Reading& reading, std::int64_t open, std::size_t first, std::size_t rows) {
    reading.opens.push_back(open);
    for (std::size_t row = first; row < first + rows && row < reading.bars.size(); ++row) {
        ++reading.bars[row];
        reading.open_of[row] = open;
    }
}

// The bars the aggregator completes over the feed. A completed bar holds the
// rows merged into it: its last sub_bar_count rows, which end with this row,
// or before it when this row has already opened the next bar.
Reading aggregated(const SessionCase& c, const char* tf, const std::vector<Bar>& feed) {
    Reading reading = nothing_published(feed.size());
    TimeframeAggregator aggregator(tf, "1", c.zone, c.session);
    for (std::size_t i = 0; i < feed.size(); ++i) {
        const AggregatedBar step = aggregator.feed(feed[i]);
        if (!step.is_complete) continue;
        const bool holds_this_row = aggregator.current().timestamp == step.bar.timestamp;
        const std::size_t end = holds_this_row ? i + 1 : i;
        const std::size_t rows =
            step.sub_bar_count > 0 ? static_cast<std::size_t>(step.sub_bar_count) : 0;
        CHECK(rows > 0 && rows <= end);
        if (rows > 0 && rows <= end) publish(reading, step.bar.timestamp, end - rows, rows);
    }
    return reading;
}

// The bars the aggregator's fixed clock completes over the feed, driven as the
// kernel drives a timeframe subscription: complete what ends by this row's
// open, observe the row, complete what ends by its close. A completed bar
// holds the rows the clock observed into it; a period it observed nothing in
// publishes nothing.
Reading clocked(const SessionCase& c, const char* tf, const std::vector<Bar>& feed) {
    Reading reading = nothing_published(feed.size());
    const TimeframeAggregator aggregator(tf, "1", c.zone, c.session);
    CHECK(aggregator.supports_fixed_completion());
    FixedPeriodClock clock;
    std::size_t first = 0;  // the first row observed into the bar the clock holds
    const auto complete_through = [&](std::int64_t horizon) {
        while (const auto done = aggregator.next_fixed_completion(clock, horizon)) {
            if (done->sub_bar_count <= 0) continue;
            publish(reading, done->bar.timestamp, first,
                    static_cast<std::size_t>(done->sub_bar_count));
        }
    };
    for (std::size_t i = 0; i < feed.size(); ++i) {
        if (!clock.cursor_ms) clock.cursor_ms = feed[i].timestamp;
        complete_through(feed[i].timestamp);
        const int held = clock.observed_count;
        aggregator.observe_fixed(clock, feed[i]);
        if (held == 0 && clock.observed_count > 0) first = i;
        complete_through(feed[i].timestamp + kMinute);
    }
    return reading;
}

// The native calendar's interval of every feed row, read without or with a
// session-day memo. A bar is published wherever the interval's open changes,
// and it holds the rows whose instant lies inside it.
Reading intervals(const SessionCase& c, const char* tf, const std::vector<Bar>& feed,
                  bool with_memo) {
    Reading reading = nothing_published(feed.size());
    const auto calendar = nc::parse_session(c.session, c.zone);
    const auto timeframe = nc::parse_timeframe(tf);
    CHECK(calendar.has_value());
    CHECK(timeframe.has_value());
    if (!calendar || !timeframe) return reading;
    nc::SessionDayMemo memo;
    for (std::size_t i = 0; i < feed.size(); ++i) {
        const std::int64_t at = feed[i].timestamp;
        const auto interval =
            with_memo ? nc::interval_containing(*calendar, *timeframe, at, memo)
                      : nc::interval_containing(*calendar, *timeframe, at);
        CHECK(interval.has_value());
        if (!interval) continue;
        if (reading.opens.empty() || reading.opens.back() != interval->open_ms) {
            reading.opens.push_back(interval->open_ms);
        }
        if (interval->open_ms <= at && at < interval->next_period_open_ms) {
            ++reading.bars[i];
            reading.open_of[i] = interval->open_ms;
        }
    }
    return reading;
}

// The opens that fall inside one session day's feed.
std::vector<std::int64_t> within(const std::vector<std::int64_t>& opens, std::int64_t first,
                                 std::int64_t last) {
    std::vector<std::int64_t> out;
    for (const std::int64_t open : opens) {
        if (open >= first && open <= last) out.push_back(open);
    }
    return out;
}

// One reading of one session day against the opens written out by hand: the
// same number of bars, each opening where it should, and every row of the day
// in exactly one bar, the one whose open is the last written-out open at or
// before the row. Every open is a check of its own, so a moved grid names the
// opens it moved. Rows are counted, and the first and last one out of place
// are named by their minute after the day's first row.
void check_day(const SessionCase& c, const char* name, const char* tf, const CivilDay& day,
               const Reading& reading, const std::vector<Bar>& feed, std::size_t begin,
               std::size_t end, const std::vector<std::int64_t>& want) {
    const std::int64_t first = feed[begin].timestamp;
    const std::vector<std::int64_t> got = within(reading.opens, first, feed[end - 1].timestamp);
    const bool same_count = got.size() == want.size();
    CHECK(same_count);
    if (!same_count) {
        std::printf("    %s, %s, %s minutes, %04d-%02d-%02d: %zu bars, want %zu\n", c.name,
                    name, tf, day.year, day.month, day.day, got.size(), want.size());
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
        const bool same_open = i < got.size() && got[i] == want[i];
        CHECK(same_open);
        if (!same_open) {
            std::printf("    %s, %s, %s minutes, %04d-%02d-%02d: open %zu is %lld, want %lld\n",
                        c.name, name, tf, day.year, day.month, day.day, i + 1,
                        i < got.size() ? static_cast<long long>(got[i]) : -1LL,
                        static_cast<long long>(want[i]));
        }
    }
    std::size_t lost = 0;       // rows in no published bar
    std::size_t doubled = 0;    // rows in more than one
    std::size_t elsewhere = 0;  // rows in one bar, which opens somewhere else
    long long lost_from = 0, lost_to = 0, elsewhere_from = 0, elsewhere_to = 0;
    std::size_t k = 0;  // want[k] is the last written-out open at or before the row
    for (std::size_t row = begin; row < end; ++row) {
        const std::int64_t at = feed[row].timestamp;
        const long long minute = static_cast<long long>((at - first) / kMinute);
        while (k + 1 < want.size() && want[k + 1] <= at) ++k;
        if (reading.bars[row] != 1) {
            if (lost + doubled == 0) lost_from = minute;
            lost_to = minute;
            if (reading.bars[row] == 0) {
                ++lost;
            } else {
                ++doubled;
            }
        } else if (want.empty() || reading.open_of[row] != want[k]) {
            if (elsewhere == 0) elsewhere_from = minute;
            elsewhere_to = minute;
            ++elsewhere;
        }
    }
    const bool every_row_in_one_bar = lost == 0 && doubled == 0;
    CHECK(every_row_in_one_bar);
    if (!every_row_in_one_bar) {
        std::printf("    %s, %s, %s minutes, %04d-%02d-%02d: %zu rows in no bar and %zu in more"
                    " than one, minutes %lld to %lld after the day's first row\n",
                    c.name, name, tf, day.year, day.month, day.day, lost, doubled, lost_from,
                    lost_to);
    }
    const bool every_row_in_its_bar = elsewhere == 0;
    CHECK(every_row_in_its_bar);
    if (!every_row_in_its_bar) {
        std::printf("    %s, %s, %s minutes, %04d-%02d-%02d: %zu rows in a bar that opens"
                    " elsewhere, minutes %lld to %lld after the day's first row\n",
                    c.name, name, tf, day.year, day.month, day.day, elsewhere, elsewhere_from,
                    elsewhere_to);
    }
}

std::vector<SessionCase> session_cases() {
    const std::vector<CivilDay> plain_days = {CivilDay{2024, 6, 3}, CivilDay{2024, 6, 4}};
    // The Friday before and the Monday after each New York clock change of 2024.
    const std::vector<CivilDay> clock_change_days = {CivilDay{2024, 3, 8}, CivilDay{2024, 3, 11},
                                                     CivilDay{2024, 11, 1}, CivilDay{2024, 11, 4}};

    // The recorded session: HKEX:700's opens on TradingView, in any zone.
    const char* const recorded = "0930-1200,1300-1600";
    const std::vector<TradingWindow> recorded_windows = {
        TradingWindow{WallClock{0, 9, 30}, 150}, TradingWindow{WallClock{0, 13, 0}, 180}};
    const std::vector<WallClock> recorded_60 = {WallClock{0, 9, 30},  WallClock{0, 10, 0},
                                                WallClock{0, 11, 0},  WallClock{0, 13, 0},
                                                WallClock{0, 14, 0},  WallClock{0, 15, 0}};
    const std::vector<WallClock> recorded_240 = {WallClock{0, 9, 30}, WallClock{0, 13, 0}};

    // The controls count from their own first open, straight through a break.
    const std::vector<TradingWindow> tokyo_windows = {
        TradingWindow{WallClock{0, 9, 0}, 150}, TradingWindow{WallClock{0, 12, 30}, 180}};
    const std::vector<TradingWindow> chicago_windows = {
        TradingWindow{WallClock{0, 19, 0}, 765}, TradingWindow{WallClock{1, 8, 30}, 290}};
    const std::vector<TradingWindow> new_york_windows = {TradingWindow{WallClock{0, 9, 30}, 390}};
    const std::vector<TradingWindow> all_day_windows = {TradingWindow{WallClock{0, 0, 0}, 1440}};

    std::vector<SessionCase> cases;
    cases.push_back(SessionCase{"recorded session, its own zone", recorded, "Asia/Hong_Kong",
                                recorded_windows, plain_days, recorded_60, recorded_240});
    cases.push_back(SessionCase{"recorded session, daylight saving", recorded, "America/New_York",
                                recorded_windows, clock_change_days, recorded_60, recorded_240});
    cases.push_back(SessionCase{"recorded session, half-hour zone", recorded, "Asia/Kolkata",
                                recorded_windows, plain_days, recorded_60, recorded_240});
    cases.push_back(SessionCase{"control: break, open on the hour", "0900-1130,1230-1530",
                                "Asia/Tokyo", tokyo_windows, plain_days,
                                every(WallClock{0, 9, 0}, 1, 7), every(WallClock{0, 9, 0}, 4, 2)});
    cases.push_back(SessionCase{"control: overnight with a break", "1900-0745,0830-1320",
                                "America/Chicago", chicago_windows, plain_days,
                                every(WallClock{0, 19, 0}, 1, 19),
                                every(WallClock{0, 19, 0}, 4, 5)});
    cases.push_back(SessionCase{"control: one window", "0930-1600", "America/New_York",
                                new_york_windows, clock_change_days,
                                every(WallClock{0, 9, 30}, 1, 7),
                                every(WallClock{0, 9, 30}, 4, 2)});
    cases.push_back(SessionCase{"control: 24x7", "24x7", "UTC", all_day_windows, plain_days,
                                every(WallClock{0, 0, 0}, 1, 24),
                                every(WallClock{0, 0, 0}, 4, 6)});
    return cases;
}

int run() {
    std::printf("Intraday bars on sessions with a break: where they open, where rows land\n");
    const char* const names[] = {"aggregator", "fixed clock", "calendar", "calendar memo"};
    for (const SessionCase& c : session_cases()) {
        // The whole feed, and where each session day's rows begin in it.
        std::vector<Bar> feed;
        std::vector<std::size_t> begins;
        for (const CivilDay& day : c.days) {
            const std::vector<Bar> bars = minutes_of(c, day);
            begins.push_back(feed.size());
            feed.insert(feed.end(), bars.begin(), bars.end());
        }
        begins.push_back(feed.size());
        const int before = failures;
        for (const char* tf : {"60", "240"}) {
            const bool hourly = std::string(tf) == "60";
            const std::vector<WallClock>& opens = hourly ? c.opens_60 : c.opens_240;
            const Reading readings[] = {aggregated(c, tf, feed), clocked(c, tf, feed),
                                        intervals(c, tf, feed, false),
                                        intervals(c, tf, feed, true)};
            for (std::size_t d = 0; d < c.days.size(); ++d) {
                const std::vector<std::int64_t> want = instants(c, c.days[d], opens);
                for (std::size_t r = 0; r < 4; ++r) {
                    check_day(c, names[r], tf, c.days[d], readings[r], feed, begins[d],
                              begins[d + 1], want);
                }
            }
        }
        std::printf("  %-36s %-20s %-17s %s\n", c.name, c.session, c.zone,
                    failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() {
    return run();
}
