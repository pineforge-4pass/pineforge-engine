#include <pineforge/timeframe.hpp>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <vector>
#include <string>
#include <cmath>

namespace pineforge {

// ─── Auto-detect timeframe from bar timestamps ────────────────────────────────

std::string detect_timeframe(const Bar* bars, int n, int max_samples) {
    if (n < 2) return "1";

    int count = std::min(n - 1, max_samples);
    std::vector<int64_t> deltas;
    deltas.reserve(count);

    for (int i = 0; i < count; ++i) {
        int64_t d = bars[i + 1].timestamp - bars[i].timestamp;
        if (d > 0) deltas.push_back(d);
    }

    if (deltas.empty()) return "1";

    // Compute median delta
    std::sort(deltas.begin(), deltas.end());
    int64_t median_ms = deltas[deltas.size() / 2];

    // Convert to seconds
    int64_t secs = median_ms / 1000;

    // Map to nearest standard TF (TradingView format)
    // Standard TFs in seconds: 1m=60, 3m=180, 5m=300, 15m=900, 30m=1800,
    //   1h=3600, 2h=7200, 4h=14400, 1D=86400, 1W=604800
    struct TFEntry { int64_t seconds; const char* label; };
    static const TFEntry standard_tfs[] = {
        {60,     "1"},
        {180,    "3"},
        {300,    "5"},
        {900,    "15"},
        {1800,   "30"},
        {3600,   "60"},
        {7200,   "120"},
        {14400,  "240"},
        {86400,  "D"},
        {604800, "W"},
    };

    // Find the closest standard TF
    int64_t best_diff = std::abs(secs - standard_tfs[0].seconds);
    const char* best_label = standard_tfs[0].label;

    for (const auto& tf : standard_tfs) {
        int64_t diff = std::abs(secs - tf.seconds);
        if (diff < best_diff) {
            best_diff = diff;
            best_label = tf.label;
        }
    }

    return std::string(best_label);
}

// ─── TF string parsing ────────────────────────────────────────────────────────

int tf_to_seconds(const std::string& tf) {
    if (tf.empty()) return 0;

    // Check for trailing letter
    char last = tf.back();

    if (last == 'M') {
        return -1;  // calendar-based (monthly)
    }

    if (last == 'D') {
        // "D" or "1D" or "2D" etc.
        if (tf.size() == 1) return 86400;
        int n = std::stoi(tf.substr(0, tf.size() - 1));
        return n * 86400;
    }

    if (last == 'W') {
        // "W" or "1W" etc.
        if (tf.size() == 1) return 604800;
        int n = std::stoi(tf.substr(0, tf.size() - 1));
        return n * 604800;
    }

    // All-numeric: minutes
    int minutes = std::stoi(tf);
    return minutes * 60;
}

int tf_ratio(const std::string& input_tf, const std::string& target_tf) {
    int input_s = tf_to_seconds(input_tf);
    int target_s = tf_to_seconds(target_tf);

    // If target is calendar-based (monthly), return -1
    if (target_s < 0) return -1;

    // If input is calendar-based, that's unusual but treat as error
    if (input_s <= 0) return -2;

    if (target_s < input_s) return -2;  // target < input: error
    if (target_s == input_s) return 1;

    return target_s / input_s;
}

// ─── Calendar boundary detection ───────────────────────────────────────────────

CalendarPeriod calendar_period_for(const std::string& tf) {
    if (tf.empty()) return CalendarPeriod::NONE;
    char last = tf.back();
    if (last == 'M') return CalendarPeriod::MONTH;
    if (last == 'W') return CalendarPeriod::WEEK;
    if (last == 'D') return CalendarPeriod::DAY;
    // Numeric (minutes): not calendar-based in the sense of boundary detection,
    // but for aggregation purposes we don't use CALENDAR mode for minute TFs.
    return CalendarPeriod::NONE;
}

static void decompose_utc(int64_t ms, struct tm& out) {
    time_t secs = static_cast<time_t>(ms / 1000);
    gmtime_r(&secs, &out);
}

bool crosses_boundary(int64_t prev_ms, int64_t curr_ms, CalendarPeriod period) {
    struct tm prev_tm, curr_tm;
    decompose_utc(prev_ms, prev_tm);
    decompose_utc(curr_ms, curr_tm);

    switch (period) {
        case CalendarPeriod::DAY:
            return prev_tm.tm_yday != curr_tm.tm_yday ||
                   prev_tm.tm_year != curr_tm.tm_year;
        case CalendarPeriod::WEEK: {
            // ISO week boundary: Monday is start of week.
            // Compare (year, week-of-year) using Monday-based week number.
            auto monday_week = [](const struct tm& t) -> int {
                // tm_wday: 0=Sunday..6=Saturday. Convert to Mon=0..Sun=6.
                int dow = (t.tm_wday + 6) % 7;
                return (t.tm_yday - dow + 7) / 7;
            };
            return prev_tm.tm_year != curr_tm.tm_year ||
                   monday_week(prev_tm) != monday_week(curr_tm);
        }
        case CalendarPeriod::MONTH:
            return prev_tm.tm_mon != curr_tm.tm_mon ||
                   prev_tm.tm_year != curr_tm.tm_year;
        case CalendarPeriod::NONE:
            return false;
    }
    return false;
}

// ─── tf_change ────────────────────────────────────────────────────────────────

bool tf_change(int64_t prev_ms, int64_t curr_ms, const std::string& tf) {
    if (prev_ms == 0 || curr_ms == 0) return false;
    CalendarPeriod period = calendar_period_for(tf);
    if (period != CalendarPeriod::NONE) {
        return crosses_boundary(prev_ms, curr_ms, period);
    }
    int secs = tf_to_seconds(tf);
    if (secs <= 0) return false;
    int64_t bucket_ms = static_cast<int64_t>(secs) * 1000;
    return (prev_ms / bucket_ms) != (curr_ms / bucket_ms);
}

// ─── TimeframeAggregator ───────────────────────────────────────────────────────

TimeframeAggregator::TimeframeAggregator()
    : mode_(Mode::PASSTHROUGH), ratio_(1) {}

TimeframeAggregator::TimeframeAggregator(int ratio)
    : mode_(Mode::RATIO), ratio_(ratio < 1 ? 1 : ratio) {}

TimeframeAggregator::TimeframeAggregator(const std::string& target_tf,
                                         const std::string& input_tf) {
    CalendarPeriod target_period = calendar_period_for(target_tf);

    if (target_period != CalendarPeriod::NONE) {
        // Calendar-based aggregation (D, W, M targets, or ratio targets
        // that also have calendar semantics like daily from intraday).
        mode_ = Mode::CALENDAR;
        cal_period_ = target_period;
    } else {
        // Both are ratio-based TFs. Compute ratio.
        int r = tf_ratio(input_tf, target_tf);
        if (r > 0) {
            mode_ = Mode::RATIO;
            ratio_ = r;
            target_seconds_ = tf_to_seconds(target_tf);
        } else {
            // Fallback to passthrough on error
            mode_ = Mode::PASSTHROUGH;
            ratio_ = 1;
        }
    }
    int in_s = tf_to_seconds(input_tf);
    if (in_s > 0) input_seconds_ = in_s;
}

void TimeframeAggregator::reset_current(const Bar& bar) {
    current_bar_ = bar;
    sub_bar_count_ = 1;
    current_emitted_complete_ = false;
}

void TimeframeAggregator::merge_into_current(const Bar& bar) {
    // open stays from the first bar, timestamp stays from first bar
    if (bar.high > current_bar_.high) current_bar_.high = bar.high;
    if (bar.low < current_bar_.low)   current_bar_.low = bar.low;
    current_bar_.close = bar.close;
    current_bar_.volume += bar.volume;
    ++sub_bar_count_;
}

// ─── feed() per-mode helpers (file-local) ─────────────────────────────────────
//
// Three mode branches (PASSTHROUGH / RATIO / CALENDAR) are independent state
// machines big enough to deserve their own functions. They cannot be private
// members on TimeframeAggregator without modifying timeframe.hpp, so they
// live as free helpers and receive a ``FeedState`` reference-pack that
// ``feed()`` populates from its own (private) members. ``feed_reset_current``
// / ``feed_merge_into_current`` mirror the private member-method versions
// above; the duplication is local and trivial (3-5 lines each).

namespace {

struct FeedState {
    Bar& current_bar;
    int& sub_bar_count;
    bool& current_emitted_complete;
    Bar& last_completed_bar;
    bool& has_completed;
};

void feed_reset_current(FeedState s, const Bar& bar) {
    s.current_bar = bar;
    s.sub_bar_count = 1;
    s.current_emitted_complete = false;
}

void feed_merge_into_current(FeedState s, const Bar& bar) {
    // open stays from the first bar, timestamp stays from first bar
    if (bar.high > s.current_bar.high) s.current_bar.high = bar.high;
    if (bar.low  < s.current_bar.low)  s.current_bar.low  = bar.low;
    s.current_bar.close = bar.close;
    s.current_bar.volume += bar.volume;
    ++s.sub_bar_count;
}

AggregatedBar feed_passthrough_mode(const Bar& input_bar, FeedState s) {
    AggregatedBar result;
    result.bar = input_bar;
    result.is_complete = true;
    result.sub_bar_count = 1;
    s.last_completed_bar = input_bar;
    s.has_completed = true;
    s.current_bar = input_bar;
    s.sub_bar_count = 1;
    return result;
}

AggregatedBar feed_ratio_mode(const Bar& input_bar, FeedState s,
                               int ratio, int64_t target_seconds) {
    AggregatedBar result;
    if (target_seconds > 0 && s.sub_bar_count > 0) {
        // Time-bucket aware ratio mode:
        // - emit completed bars at the LAST sub-bar of the bucket (count hit),
        // - still emit at boundary for sparse/irregular data (partial bucket).
        int64_t bucket_ms = static_cast<int64_t>(target_seconds) * 1000;
        int64_t curr_bucket = s.current_bar.timestamp / bucket_ms;
        int64_t next_bucket = input_bar.timestamp / bucket_ms;
        bool boundary = next_bucket != curr_bucket;

        if (boundary) {
            if (s.current_emitted_complete) {
                feed_reset_current(s, input_bar);
                result.bar = s.current_bar;
                result.is_complete = false;
                result.sub_bar_count = s.sub_bar_count;
                return result;
            }
            s.last_completed_bar = s.current_bar;
            s.has_completed = true;
            result.bar = s.last_completed_bar;
            result.is_complete = true;
            result.sub_bar_count = s.sub_bar_count;
            feed_reset_current(s, input_bar);
            return result;
        }

        feed_merge_into_current(s, input_bar);
        bool complete = (s.sub_bar_count == ratio);
        if (complete) {
            s.last_completed_bar = s.current_bar;
            s.has_completed = true;
            s.current_emitted_complete = true;
        }
        result.bar = s.current_bar;
        result.is_complete = complete;
        result.sub_bar_count = s.sub_bar_count;
        return result;
    }
    // Fallback: original count-based logic.
    if (s.sub_bar_count == 0) {
        // First bar ever
        feed_reset_current(s, input_bar);
    } else if (s.sub_bar_count >= ratio) {
        // Previous cycle completed; start new one
        feed_reset_current(s, input_bar);
    } else {
        feed_merge_into_current(s, input_bar);
    }

    bool complete = (s.sub_bar_count >= ratio);
    if (complete) {
        s.last_completed_bar = s.current_bar;
        s.has_completed = true;
        s.current_emitted_complete = true;
    }

    result.bar = s.current_bar;
    result.is_complete = complete;
    result.sub_bar_count = s.sub_bar_count;
    return result;
}

AggregatedBar feed_calendar_mode(const Bar& input_bar, FeedState s,
                                  CalendarPeriod cal_period,
                                  int64_t input_seconds) {
    AggregatedBar result;
    if (s.sub_bar_count == 0) {
        // Very first bar
        feed_reset_current(s, input_bar);
        result.bar = s.current_bar;
        result.is_complete = false;
        result.sub_bar_count = s.sub_bar_count;
        return result;
    }

    // Does the new bar fall in a different calendar period than
    // the current group's first bar?
    if (crosses_boundary(s.current_bar.timestamp, input_bar.timestamp,
                          cal_period)) {
        if (s.current_emitted_complete) {
            feed_reset_current(s, input_bar);
            result.bar = s.current_bar;
            result.is_complete = false;
            result.sub_bar_count = s.sub_bar_count;
            return result;
        }
        // First *actual* bar of the next calendar period: finalize the previous
        // period if it was not already closed by end-of-period projection below.
        int completed_subs = s.sub_bar_count;
        s.last_completed_bar = s.current_bar;
        s.has_completed = true;

        // Start new aggregation with this bar
        feed_reset_current(s, input_bar);

        result.bar = s.last_completed_bar;
        result.is_complete = true;
        result.sub_bar_count = completed_subs;
        return result;
    }

    // Same calendar period: merge. TradingView-style: the aggregated period is
    // complete once we have processed a bar such that the *next* bar of input_tf
    // duration would cross the calendar boundary (e.g. last 1m of the session
    // completes the daily bar). This matches request.security HTF behavior used in
    // validation; "complete only on the next period's first bar" shifts all HTF
    // series and breaks TV parity.
    feed_merge_into_current(s, input_bar);
    bool complete = false;
    if (input_seconds > 0) {
        int64_t next_ms = input_bar.timestamp + input_seconds * 1000;
        complete = crosses_boundary(input_bar.timestamp, next_ms, cal_period);
    }
    if (complete) {
        s.last_completed_bar = s.current_bar;
        s.has_completed = true;
        s.current_emitted_complete = true;
        result.bar = s.last_completed_bar;
        result.is_complete = true;
        result.sub_bar_count = s.sub_bar_count;
        return result;
    }
    result.bar = s.current_bar;
    result.is_complete = false;
    result.sub_bar_count = s.sub_bar_count;
    return result;
}

}  // namespace

AggregatedBar TimeframeAggregator::feed(const Bar& input_bar) {
    FeedState s{current_bar_, sub_bar_count_, current_emitted_complete_,
                last_completed_bar_, has_completed_};
    switch (mode_) {
        case Mode::PASSTHROUGH:
            return feed_passthrough_mode(input_bar, s);
        case Mode::RATIO:
            return feed_ratio_mode(input_bar, s, ratio_, target_seconds_);
        case Mode::CALENDAR:
            return feed_calendar_mode(input_bar, s, cal_period_, input_seconds_);
    }

    // Unreachable
    AggregatedBar result;
    result.bar = input_bar;
    result.is_complete = true;
    result.sub_bar_count = 1;
    return result;
}

Bar TimeframeAggregator::current() const {
    return current_bar_;
}

Bar TimeframeAggregator::last_completed() const {
    return last_completed_bar_;
}

bool TimeframeAggregator::is_active() const {
    return mode_ != Mode::PASSTHROUGH;
}

} // namespace pineforge
