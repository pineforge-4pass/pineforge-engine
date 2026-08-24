#pragma once
#include <string>
#include <cstdint>
#include "bar.hpp"

namespace pineforge {

// ─── Day-length constants ──────────────────────────────────────────────────────

inline constexpr int64_t kSecPerDay = 86400;
inline constexpr int64_t kMsPerDay  = 86400000;

// ─── AggregatedBar ─────────────────────────────────────────────────────────────

struct AggregatedBar {
    Bar bar;
    bool is_complete;
    int sub_bar_count;
};

// ─── TF string helpers ─────────────────────────────────────────────────────────

/// Convert a TradingView timeframe string to seconds.
/// Minute-based: "1","5","15","30","60","120","240" => minutes * 60
/// Day-based:    "D","1D" => 86400
/// Week-based:   "W","1W" => 604800
/// Month-based:  "M","1M" => -1 (calendar-based, not fixed seconds)
int tf_to_seconds(const std::string& tf);

/// Extract the numeric multiplier from a TF string (e.g. "15" -> 15, "D" -> 1).
inline int tf_multiplier(const std::string& tf) {
    int val = 0;
    for (char c : tf) {
        if (c >= '0' && c <= '9') val = val * 10 + (c - '0');
        else break;
    }
    return val > 0 ? val : 1;
}

inline bool tf_is_intraday(const std::string& tf) {
    int s = tf_to_seconds(tf);
    return s > 0 && s < kSecPerDay;
}

inline bool tf_is_daily(const std::string& tf) {
    return tf.find('D') != std::string::npos || tf_to_seconds(tf) == kSecPerDay;
}

inline bool tf_is_weekly(const std::string& tf) {
    return tf.find('W') != std::string::npos;
}

inline bool tf_is_monthly(const std::string& tf) {
    return !tf.empty() && tf.back() == 'M';
}

inline bool tf_is_seconds(const std::string& tf) {
    return tf.find('S') != std::string::npos;
}

/// Check if prev/curr timestamps cross a timeframe boundary.
bool tf_change(int64_t prev_ms, int64_t curr_ms, const std::string& tf);

/// Compute how many input bars fit into one target bar.
/// Returns positive int for ratio-based, -1 for calendar-based (month),
/// -2 if target < input (error).
int tf_ratio(const std::string& input_tf, const std::string& target_tf);

// ─── Auto-detect timeframe from bar timestamps ────────────────────────────────

/// Detect the timeframe string from an array of bars by computing the median
/// timestamp delta and mapping to the nearest standard TF.
/// Returns a TradingView-style TF string (e.g. "1", "5", "15", "60", "D", "W").
/// Uses up to the first `max_samples` bars for detection.
/// Returns "1" if detection fails (< 2 bars or irregular data).
std::string detect_timeframe(const Bar* bars, int n, int max_samples = 100);

// ─── Calendar boundary detection ───────────────────────────────────────────────

enum class CalendarPeriod { NONE, DAY, WEEK, MONTH };

/// Determine the calendar period for a target TF string.
CalendarPeriod calendar_period_for(const std::string& tf);

/// Check if two timestamps (Unix milliseconds) fall in different calendar periods.
bool crosses_boundary(int64_t prev_ms, int64_t curr_ms, CalendarPeriod period);

/// Timezone/session-aware variants. The tz-less forms above evaluate the
/// calendar in UTC and intraday buckets on the epoch grid — exactly TV's
/// behavior for 24x7 symbols (the corpus regime). Session symbols
/// (equities RTH, forex) anchor HTF buckets on the exchange clock instead:
/// daily boundaries at symbol-local midnight, intraday buckets offset by the
/// session-open minutes. With tz="UTC" and session ""/"24x7" these are
/// bit-identical to the UTC forms, so existing callers are unaffected.
bool crosses_boundary(int64_t prev_ms, int64_t curr_ms, CalendarPeriod period,
                      const std::string& tz, const std::string& session);
bool tf_change(int64_t prev_ms, int64_t curr_ms, const std::string& tf,
               const std::string& tz, const std::string& session);

// ─── Symbol-clock D/W/M bar anchors ────────────────────────────────────────────
//
// TradingView's daily bar is the SESSION day, not the UTC (nor the local
// calendar) day: OANDA:EURUSD (America/New_York, 1700-1700) opens its daily
// bar at 17:00 ET and its week at Sunday 17:00 ET; NASDAQ:AAPL (0930-1600)
// opens at 09:30 ET Monday..Friday; a 24x7 UTC symbol opens at 00:00 UTC.
// Every chart-level consumer of "the symbol's daily bar" — ta.vwap's default
// anchor, timeframe.change("1D"), time("D")/time_close("D") — has to key on
// this clock, the same one request.security aggregation already uses
// (crosses_boundary / session_period_key above). With tz="UTC" and an
// empty / "24x7" session all of these reduce to plain epoch integer math,
// bit-identical to the UTC forms the corpus pins.
//
// Period attribution follows TV's trading-date rule: a session that wraps
// midnight (1700-1700) is the trading day of the date it CLOSES on (the
// bar opening Sunday 17:00 ET is Monday's daily bar), so forex weeks open on
// the Sunday session and a month opens on the session whose close date is
// the 1st. Equity / 24x7 sessions close on their open date, so their weeks
// stay Monday-partitioned and months calendar-partitioned exactly as before.
// W/M opens are nominal (the Monday / 1st session-day); they do not consult
// a holiday calendar.

/// Ordinal of the session day containing `ms` (days since epoch on the
/// session clock). UTC + no session: ms / kMsPerDay.
int64_t session_day_index(int64_t ms, const std::string& tz,
                          const std::string& session);

/// Open (Unix ms) of the symbol's D/W/M bar that contains `ms`.
/// CalendarPeriod::NONE returns `ms` unchanged.
int64_t session_period_open_ms(int64_t ms, const std::string& tz,
                               const std::string& session,
                               CalendarPeriod period);

/// Exclusive close (Unix ms) of the symbol's D/W/M bar that contains `ms`:
/// DAY -> session-day open + session length (16:00 ET on equities, the next
/// 17:00 ET on forex, next midnight on 24x7); WEEK / MONTH -> the open of
/// the next period's first session-day. CalendarPeriod::NONE returns `ms`.
int64_t session_period_close_ms(int64_t ms, const std::string& tz,
                                const std::string& session,
                                CalendarPeriod period);

// ─── TimeframeAggregator ───────────────────────────────────────────────────────

class TimeframeAggregator {
public:
    /// Default: passthrough mode (no aggregation).
    TimeframeAggregator();

    /// Ratio-based: every `ratio` input bars produce one output bar.
    explicit TimeframeAggregator(int ratio);

    /// Calendar-based: aggregate until day/week/month boundary.
    TimeframeAggregator(const std::string& target_tf,
                        const std::string& input_tf);

    /// Calendar/ratio aggregation anchored on a symbol clock. tz is the Pine
    /// syminfo.timezone (exchange tz); session the Pine session string
    /// ("0930-1600", "24x7", ...). Defaults reproduce the UTC/24x7 forms
    /// bit-for-bit, so every existing construction site compiles unchanged.
    TimeframeAggregator(const std::string& target_tf,
                        const std::string& input_tf,
                        const std::string& tz,
                        const std::string& session = "");

    /// Feed one input bar. Returns aggregation state.
    AggregatedBar feed(const Bar& input_bar);

    /// Current in-progress bar.
    Bar current() const;

    /// Last completed aggregated bar.
    Bar last_completed() const;

    /// Whether aggregation is active (non-passthrough).
    bool is_active() const;

private:
    enum class Mode { PASSTHROUGH, RATIO, CALENDAR };

    Mode mode_ = Mode::PASSTHROUGH;
    int ratio_ = 1;                    // for RATIO mode
    CalendarPeriod cal_period_ = CalendarPeriod::NONE; // for CALENDAR mode
    int64_t target_seconds_ = 0;       // wall-clock seconds for RATIO boundary detection
    int64_t input_seconds_ = 0;        // input bar duration (seconds), when known
    std::string anchor_tz_ = "UTC";    // syminfo.timezone (exchange clock)
    std::string anchor_session_;       // syminfo.session ("" or "24x7" = none)

    Bar current_bar_{};
    Bar last_completed_bar_{};
    int sub_bar_count_ = 0;
    bool has_completed_ = false;
    bool current_emitted_complete_ = false;

    void reset_current(const Bar& bar);
    void merge_into_current(const Bar& bar);
};

} // namespace pineforge
