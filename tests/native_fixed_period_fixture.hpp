#pragma once
#include "tv_fixed_period_75_fact.hpp"
#include <pineforge/bar.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace native_fixed_period_fixture {
using namespace pineforge;
using namespace tv_fixed_period_75_fact;

// Deliberately unrelated input prices: copying an input or holding a literal
// cannot satisfy the native substitution or reporter quantity assertions.
inline std::vector<Bar> input(int step, int end = 2880) {
    std::vector<Bar> bars;
    for (int minute = 0; minute < end; minute += step) {
        const double value = 100.25 + minute * 0.125;
        bars.push_back({value - 0.5, value + 1, value - 1, value, 3.0,
                       kOriginMs + minute * kMinuteMs});
    }
    return bars;
}
inline std::vector<Bar> native() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBarCount; ++i) {
        const auto& row = kBars[i];
        // Only close/time are TV facts. O/H/L/V are synthetic payload sentinels.
        bars.push_back({row.value - 2, row.value + 4, row.value - 5, row.value,
                       1000.5 + i, kOriginMs + row.open_minute * kMinuteMs});
    }
    return bars;
}

// An exchange session whose middle session-day closes early.
//
// Native engine controls, not TradingView readings: three supplied
// session-days of a 09:30-16:00 session (Monday to Wednesday from the origin).
// The middle day stops at `early_close`; the first and the last run to 16:00.
// Everything a test expects from this namespace is arithmetic on the supplied
// bars and on the intervals they are accepted with. No value, na or time here
// is a captured price, and no expectation is read back from the engine.
namespace exchange {
inline constexpr char kTimezone[] = "UTC";
inline constexpr char kSession[] = "0930-1600";
constexpr int kOpenMinute = 9 * 60 + 30;
constexpr int kCloseMinute = 16 * 60;
constexpr int kDayMinutes = 24 * 60;
constexpr int kDays = 3;
constexpr int kEarlyDay = 1;

inline std::int64_t at(int day, int minute) {
    return kOriginMs + (static_cast<std::int64_t>(day) * kDayMinutes + minute) * kMinuteMs;
}
inline int day_of(std::int64_t ms) {
    return static_cast<int>((ms - kOriginMs) / (kDayMinutes * kMinuteMs));
}
// The minute a supplied session-day stops at.
constexpr int day_close(int day, int early_close) {
    return day == kEarlyDay ? early_close : kCloseMinute;
}

// Supplied bars of `step` minutes on the session grid. Their prices are
// unrelated to the native rows below.
inline std::vector<Bar> bars(int step, int early_close) {
    std::vector<Bar> out;
    for (int day = 0; day < kDays; ++day) {
        for (int minute = kOpenMinute; minute < day_close(day, early_close); minute += step) {
            const double value = 100.25 + day * 50 + (minute - kOpenMinute) * 0.125;
            out.push_back({value - 0.5, value + 1, value - 1, value, 3.0, at(day, minute)});
        }
    }
    return out;
}

// The exchange's own bars of `requested` minutes: one per requested period
// that opened before its session-day stopped, so the early day ends with the
// period its close cut short. Every field is a synthetic sentinel no supplied
// bar and no aggregate of supplied bars can equal.
inline std::vector<Bar> native(int requested, int early_close) {
    std::vector<Bar> out;
    for (int day = 0; day < kDays; ++day) {
        for (int minute = kOpenMinute; minute < day_close(day, early_close);
             minute += requested) {
            const double value = 5000.5 + day * 1000 + (minute - kOpenMinute);
            out.push_back({value - 2, value + 4, value - 5, value,
                           7000.5 + static_cast<double>(out.size()), at(day, minute)});
        }
    }
    return out;
}

// The same supplied bars without those that open in [from, to) of `day`: a
// hole inside one session-day, which no session close explains.
inline std::vector<Bar> without(std::vector<Bar> all, int day, int from, int to) {
    const std::int64_t first = at(day, from);
    const std::int64_t end = at(day, to);
    all.erase(std::remove_if(all.begin(), all.end(),
                             [first, end](const Bar& bar) {
                                 return bar.timestamp >= first && bar.timestamp < end;
                             }),
              all.end());
    return all;
}

// A supplied bar is accepted over its own width, clipped by the session close.
inline std::int64_t bar_close(const Bar& bar, int step) {
    return std::min(bar.timestamp + step * kMinuteMs,
                    at(day_of(bar.timestamp), kCloseMinute));
}

// Where a native row of `requested` minutes ends on the session grid.
inline std::int64_t nominal_end(const Bar& row, int requested) {
    return bar_close(row, requested);
}

// When each native row is complete, from the bars its series is built from
// (`source`, of `step` minutes):
//   - at its nominal end, clipped by the session close, whenever its own
//     session-day's source bars reach that end;
//   - on an exchange calendar, at the close of the session-day's LAST source
//     bar when that bar opened inside the row and a later session-day follows:
//     the early close ends the period it leaves open;
//   - otherwise not before the next supplied source bar opens, the first
//     accepted input that shows the nominal end has passed.
// A row no later source bar reveals is never complete in this feed.
inline std::vector<std::int64_t> row_closes(const std::vector<Bar>& rows, int requested,
                                           const std::vector<Bar>& source, int step,
                                           bool exchange_calendar) {
    std::vector<std::int64_t> out;
    out.reserve(rows.size());
    for (const Bar& row : rows) {
        const int day = day_of(row.timestamp);
        const std::int64_t nominal = nominal_end(row, requested);
        const Bar* last = nullptr;
        const Bar* successor = nullptr;
        for (const Bar& bar : source) {
            if (day_of(bar.timestamp) == day) last = &bar;
            if (day_of(bar.timestamp) > day) {
                successor = &bar;
                break;
            }
        }
        std::int64_t close = nominal;
        if (last == nullptr || bar_close(*last, step) < nominal) {
            const bool early = exchange_calendar && last != nullptr && successor != nullptr
                && last->timestamp >= row.timestamp;
            close = early ? bar_close(*last, step)
                : successor != nullptr ? successor->timestamp
                : std::numeric_limits<std::int64_t>::max();
        }
        out.push_back(close);
    }
    return out;
}

// The chart bars a supplied primary input forms: each opens on the session
// grid and is accepted up to the close of its last supplied input.
struct Chart {
    std::int64_t open_ms;
    std::int64_t close_ms;
};
inline std::vector<Chart> charts(const std::vector<Bar>& input, int step, int chart) {
    std::vector<Chart> out;
    for (const Bar& bar : input) {
        const int day = day_of(bar.timestamp);
        const int minute = static_cast<int>((bar.timestamp - at(day, 0)) / kMinuteMs);
        const std::int64_t open =
            at(day, kOpenMinute + (minute - kOpenMinute) / chart * chart);
        if (out.empty() || out.back().open_ms != open) out.push_back({open, bar_close(bar, step)});
        else out.back().close_ms = bar_close(bar, step);
    }
    return out;
}

// The row a chart bar reads with lookahead off: the latest one complete by
// the chart bar's own accepted close. With `gaps`, only a row that completed
// after the previous chart bar's close. -1: the chart bar reads no row.
inline int expected_row(const std::vector<std::int64_t>& closes, std::int64_t chart_close,
                        std::int64_t previous_chart_close, bool gaps) {
    int found = -1;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (closes[i] > chart_close) continue;
        if (gaps && closes[i] <= previous_chart_close) continue;
        found = static_cast<int>(i);
    }
    return found;
}

// The accepted primary input a completed row is delivered on: the first whose
// own close has reached the row's. -1: none of the supplied inputs.
inline std::int64_t delivered_at(std::int64_t row_close, const std::vector<Bar>& input,
                                 int step) {
    for (const Bar& bar : input) {
        if (bar_close(bar, step) >= row_close) return bar.timestamp;
    }
    return -1;
}
} // namespace exchange
} // namespace native_fixed_period_fixture
