#pragma once
#include <string>
#include <cstdint>
#include "bar.hpp"

namespace pineforge {

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
    return s > 0 && s < 86400;
}

inline bool tf_is_daily(const std::string& tf) {
    return tf.find('D') != std::string::npos || tf_to_seconds(tf) == 86400;
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

    Bar current_bar_{};
    Bar last_completed_bar_{};
    int sub_bar_count_ = 0;
    bool has_completed_ = false;
    bool current_emitted_complete_ = false;

    void reset_current(const Bar& bar);
    void merge_into_current(const Bar& bar);
};

} // namespace pineforge
