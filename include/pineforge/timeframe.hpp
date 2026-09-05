#pragma once
#include <string>
#include <cstdint>
#include <vector>
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

/// True for a daily-or-higher chart timeframe ("D", "1D", "2D", "W", "M"):
/// a bar that covers whole session days rather than a time of day (the
/// aggregation path's CALENDAR classification). Intraday timeframes and an
/// empty (undetected) one are false.
inline bool tf_is_daily_or_higher(const std::string& tf) {
    return calendar_period_for(tf) != CalendarPeriod::NONE;
}

/// Check if two timestamps (Unix milliseconds) fall in different calendar periods.
bool crosses_boundary(int64_t prev_ms, int64_t curr_ms, CalendarPeriod period);

/// Timezone/session-aware variants. The tz-less forms above evaluate the
/// calendar in UTC and intraday buckets on the epoch grid — exactly TV's
/// behavior for 24x7 symbols (the corpus regime). Session symbols
/// (equities RTH, forex) anchor HTF buckets on the exchange clock instead:
/// daily boundaries at symbol-local midnight, intraday buckets offset by the
/// symbol's day stamp -- the session open, except OANDA's 1800-1700 metals
/// session whose day (and TradingView's "45"/"240" grid) rolls at the 17:00
/// ET forex roll one hour before it opens (session_day_stamp_offset_minutes
/// in timeframe.cpp cites the pin). With tz="UTC" and session ""/"24x7"
/// these are bit-identical to the UTC forms, so existing callers are
/// unaffected.
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
// (crosses_boundary / session_period_key above). The bar OPENS at the
// session-day's DAY STAMP — the session open on every session but OANDA's
// 1800-1700, whose D bar is stamped 17:00 ET an hour before it trades
// (time("D") reads 17:00 ET on OANDA:XAUUSD, pin-time-hours; the session
// open stays where the tape trades from). With tz="UTC" and an empty /
// "24x7" session all of these reduce to plain epoch integer math,
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

/// Open (Unix ms) of the symbol's D/W/M bar that contains `ms`: the day
/// stamp of the period's first session-day (17:00 ET on OANDA 1800-1700,
/// the session open elsewhere). CalendarPeriod::NONE returns `ms` unchanged.
int64_t session_period_open_ms(int64_t ms, const std::string& tz,
                               const std::string& session,
                               CalendarPeriod period);

/// Open (Unix ms) of the `bucket_sec`-wide intraday bucket that contains
/// `ms` on the symbol's day-stamp-anchored grid: exchange-tz time since
/// local midnight + day stamp, floored to the bucket width. This is THE
/// intraday HTF grid — request.security's ratio buckets
/// (TimeframeAggregator::bucket_open_ms), tf_change and
/// time("<intraday tf>") / time_close all read it (pin-time-hours:
/// time("60") on NYSE:F is 09:30 / 10:30 / .. / 15:30 ET, time("240") on
/// NSE:NIFTY 09:15 / 13:15 IST, on OANDA:XAUUSD 17:00 ET + 4h*k). With
/// tz="UTC" and session ""/"24x7" it is the epoch grid, bit-identical to
/// the tz-less forms. bucket_sec <= 0 returns `ms`.
int64_t session_intraday_bucket_open_ms(int64_t ms, int64_t bucket_sec,
                                        const std::string& tz,
                                        const std::string& session);

/// The session instant a native CALENDAR chart stamp covers. A stamp inside
/// its session (open to exclusive close) returns unchanged. A stamp in the
/// inter-session gap -- before its session-day's open (the 1800-1700 day
/// stamp hour) or at / after its close -- covers the session about to open
/// -- OANDA stamps daily FX/metal bars at the 17:00 ET break under an
/// 1800-1700 session -- and rolls forward to that session's open.
/// ""/24x7 sessions have no gap and always return `ms` unchanged.
int64_t session_covered_instant_ms(int64_t ms, const std::string& tz,
                                   const std::string& session);

/// Exclusive close (Unix ms) of the symbol's D/W/M bar that contains `ms`:
/// DAY -> session-day open + session length (16:00 ET on equities, the next
/// 17:00 ET on forex, next midnight on 24x7); WEEK / MONTH -> the open of
/// the next period's first session-day. CalendarPeriod::NONE returns `ms`.
int64_t session_period_close_ms(int64_t ms, const std::string& tz,
                                const std::string& session,
                                CalendarPeriod period);

/// Exclusive close (Unix ms) of the LAST TRADED session-day of the D/W/M
/// bar that contains `ms`: DAY is session_period_close_ms; WEEK / MONTH
/// step back from the period's last session-day over weekend TRADING dates
/// (Saturday / Sunday never hold a session on exchange-calendar symbols),
/// so an equity week ends Friday 16:00 ET, a month whose last calendar day
/// is a weekend ends on its last Friday, and the forex week ends Friday
/// 17:00 ET (the Friday-open session is Saturday's trading date). Exchange
/// holidays are not modelled. CalendarPeriod::NONE returns `ms`.
int64_t session_period_last_traded_close_ms(int64_t ms, const std::string& tz,
                                            const std::string& session,
                                            CalendarPeriod period);

// ─── Native daily partition for the CHART symbol's D period ────────────────────
//
// TradingView's D period on an exchange-calendar intraday chart is its own
// daily bar, not the nominal session-day: on CME_MINI:NQ1!/ES1! 15m the
// time("D"), ta.change(time("D")), timeframe.change("1D") and ta.vwap's
// default daily anchor follow the exchange TRADE-DATE daily bars, so the
// 17:00 CT reopen after a US-holiday early close stays inside the D bar that
// opened before the holiday (2025-05-26 Memorial Day evening: the bar
// stamped Sun 05-25 17:00 runs to Tue 05-27 16:00; 06-19 Juneteenth; the Sun
// 07-06 after Independence Day, whose Thu 07-03 17:00 stamp runs to Mon
// 07-07 16:00; 09-01 Labor Day; 11-27 Thanksgiving, to Fri 11-28 12:15;
// 2026-01-19 MLK; 02-16 Presidents' Day) and the Good-Friday-eve session
// (Thu 04-17 17:00 CT, 2025) belongs to the Wed 04-16 17:00 bar that runs to
// the Sun 04-20 reopen -- pinned 2026-09-05 by lab tv o-cme-dayanchor-full
// (ledger log-20260905t123531z-7fe6b95a: 255 D periods over 2025-04-01 ..
// 2026-05-01, every start a registry daily-feed row, feed == TV data on
// 25530/25530 closes). Every nominal session-day rule stays as it is; when
// the run installs TradingView's own daily bars (strategy_set_native_
// security_feed "D") on an intraday chart, the engine builds this partition
// from their stamps -- the same partition request.security "D" evaluators
// take (TimeframeAggregator::set_native_periods) -- and the chart-level
// consumers below read it for the symbol's own clock (tz + session equal to
// the partition's): the D period holding an instant is the native bar whose
// stamp is the latest at or before it; its trade day is the session-day of
// its last chart bar (the merged Memorial-Day bar is Tuesday's); a W / M
// period groups the native days by the nominal week / month of their trade
// day and opens on the group's first stamp (b29152c's request.security rule);
// an instant before the first stamp, or at / after the LAST stamp's nominal
// session-day close, keeps the nominal rules. Without an installed partition
// (no native daily feed, a 1D chart, another symbol's clock) every function
// is bit-identical to the nominal session calendar.
struct NativeDayPartition {
    std::string tz;
    std::string session;
    std::vector<int64_t> stamps;       // native daily stamps, strictly increasing
    std::vector<int64_t> trade_day;    // per stamp: session_day_index of its trade day
    std::vector<int64_t> week_open;    // per stamp: the W group's first stamp
    std::vector<int64_t> month_open;   // per stamp: the M group's first stamp
    int64_t last_bound = 0;            // nominal session-day close of the last stamp
    bool empty() const { return stamps.empty(); }
};

/// Build the partition for the symbol clock (tz, session) from the native
/// daily stamps and the chart's input bars (the trade instants: the last
/// input bar before the next stamp, bounded by the last stamp's nominal
/// session-day close; the stamp itself when no input bar lies in the
/// period). Returns false -- and leaves `out` empty -- for no stamps or
/// non-increasing stamps.
bool build_native_day_partition(NativeDayPartition& out,
                                const std::string& tz,
                                const std::string& session,
                                const std::vector<int64_t>& stamps,
                                const Bar* input_bars, int n_input);

/// Index of the native period holding `ms` under `p`: -1 before the first
/// stamp and at / after the last stamp's nominal session-day close.
int native_day_partition_index(const NativeDayPartition& p, int64_t ms);

/// The calling thread's active chart partition, read by session_day_index /
/// session_period_open_ms / session_period_close_ms /
/// session_period_last_traded_close_ms / crosses_boundary (and so by
/// tf_change, the symbol-clock pine_time forms and ta::VWAP's session
/// anchor) whenever their (tz, session) equal the partition's. nullptr (the
/// default) leaves every rule nominal. set_ returns the previous pointer;
/// BacktestEngine::run installs its chart partition through
/// NativeDayPartitionScope for exactly the run's bar loop.
const NativeDayPartition* set_active_native_day_partition(const NativeDayPartition* p);
const NativeDayPartition* active_native_day_partition();

class NativeDayPartitionScope {
public:
    explicit NativeDayPartitionScope(const NativeDayPartition* p)
        : prev_(set_active_native_day_partition(p)) {}
    ~NativeDayPartitionScope() { set_active_native_day_partition(prev_); }
    NativeDayPartitionScope(const NativeDayPartitionScope&) = delete;
    NativeDayPartitionScope& operator=(const NativeDayPartitionScope&) = delete;
private:
    const NativeDayPartition* prev_;
};

// Feature probe: the chart-level native daily partition above exists.
#define PINEFORGE_HAS_NATIVE_DAY_PARTITION_V1 1

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

    /// feed() with the NEXT input bar's timestamp known (0 = unknown, the
    /// form above). A historical run holds its whole feed, and on a declared
    /// exchange session TradingView finalizes a D/W/M bar on the period's
    /// actual last chart bar whatever the nominal calendar says that bar is:
    /// the 12:45 ET bar of a 13:00 half-day (NYSE:F Fri 2025-11-28), the
    /// Thursday 15:45 bar before a holiday Friday, the 15:45 CT bar closing
    /// an overnight CME session-day (lab tv wm-security-buckets tapes,
    /// 2026-09-05). CALENDAR mode therefore also completes the running
    /// bucket when the next input bar opens a new period. RATIO and
    /// PASSTHROUGH ignore the hint, and so do ""/"24x7" sessions (a data
    /// hole before a 24x7 midnight is not an early close), so every caller
    /// passing 0 and every session-less feed stay bit-identical. The rule
    /// applies where TradingView's session template knows the early close
    /// -- exchange calendars; set_early_close_completes(false) switches a
    /// symbol whose template does not (OANDA's cfd / forex streams) back to
    /// the nominal close and the lazy completion on the next period's first
    /// bar.
    AggregatedBar feed(const Bar& input_bar, int64_t next_input_ms);

    /// feed(bar, next_input_ms) with the CALLING chart bar's nominal close
    /// known (0 = unknown, the forms above): TradingView's time_close of
    /// the chart bar this input bar belongs to -- the input bar's own end
    /// on a single-feed run, the native D/W/M chart bar's session close on
    /// the split-feed path, where a finer auxiliary slice advances
    /// request.security. Read only by the OTC rule
    /// (set_early_close_completes(false)): TradingView surfaces a D/W/M
    /// value on the chart bar whose time_close reaches the period's
    /// nominal close, and the daily bar of an early-close day keeps its
    /// nominal time_close -- on the OANDA:XAUUSD 1D chart the Fri
    /// 2025-07-04 bar (data to 12:45 ET) still closes at 17:00 ET, so
    /// request.security(tickerid, "W", x) advances on it (cW 3336.61, the
    /// early close) exactly as on every regular Friday, and "D" is the
    /// chart bar itself there and on Mon 2026-02-16 (14:15 early close);
    /// on the 15m chart the 12:45 / 14:15 bar's own time_close falls short
    /// and the value waits for the next session's first bar (lab tv
    /// oanda1d-{jul,feb,novm,decm} beside the oanda 15m pin, 2026-09-05).
    /// So on an OTC stream a bucket the next input bar leaves completes on
    /// this input bar iff calling_close_ms reaches the period's nominal
    /// close (session_period_last_traded_close_ms); with 0 it waits, as
    /// before. Exchange kinds (early_close_completes) ignore the hint: the
    /// actual last bar completes the period either way.
    AggregatedBar feed(const Bar& input_bar, int64_t next_input_ms,
                       int64_t calling_close_ms);

    /// Current in-progress bar.
    Bar current() const;

    /// RATIO (fixed intraday target) only: whether the bucket in progress
    /// holds sub-bars no completion has emitted yet -- the tail of a chart
    /// bar whose last sub-bars never reached the bucket's count, real end
    /// or session close (the 21:57Z 3m bucket of OANDA:XAUUSD's Thanksgiving
    /// 2025-11-26 session holds only the 21:59Z minute). CALENDAR and
    /// PASSTHROUGH answer false; so does a count-only ratio with no
    /// wall-clock width.
    bool has_pending_partial() const;

    /// Finalize the pending partial bucket exactly as feed() finalizes a
    /// bucket -- the sub-bars it holds ARE the bucket -- and return it
    /// complete. TradingView surfaces the LAST intrabar of a chart bar at
    /// that bar's close whatever its minute count (lab tv
    /// dca-ltf-last-intrabar, 2026-09-05), so a request.security evaluator
    /// served by a finer feed calls this when its calling chart bar
    /// completes. The bucket is emitted once: the next boundary resets it
    /// without re-emitting (current_emitted_complete), and a later sub-bar
    /// of the same bucket merges without completing it again, as after any
    /// early completion. Without a pending partial nothing changes and the
    /// current bar is returned incomplete.
    AggregatedBar complete_pending_partial();

    /// Last completed aggregated bar.
    Bar last_completed() const;

    /// Whether aggregation is active (non-passthrough).
    bool is_active() const;

    /// The D/W/M period a CALENDAR aggregator buckets on; NONE for RATIO
    /// and PASSTHROUGH.
    CalendarPeriod calendar_period() const;

    /// TradingView's own period partition for a CALENDAR aggregator, from
    /// the native request.security feed of the aggregated timeframe: the
    /// stamps (Unix ms, strictly increasing) are the exchange's native bars'
    /// opens, and trade_instants[k] is an instant inside the session-day the
    /// k-th native bar COMPLETES on -- its last input bar (the stamp itself
    /// when no input bar falls in the period), the trade date TradingView
    /// files the bar under. Installed, they replace the nominal session
    /// calendar as the period key: an input bar belongs to the native bar
    /// whose stamp is the latest at or before it, so a D period is exactly
    /// that native bar's span. On CME_MINI:ES1! a holiday session that
    /// pauses at 12:00 CT and reopens at 17:00 the same day (Labor Day,
    /// Thanksgiving, Independence Day) is folded by TradingView into the
    /// NEXT trade date's daily bar with no stamp of its own (lab tv esd pin,
    /// 2026-09-05: the Sun 08-31 17:00 stamp runs to Tue 09-02 15:45 and is
    /// Tuesday's bar), so no D period closes at the pause, none opens at the
    /// reopen, and a data hole inside a native day is not a close. A W / M
    /// period groups the native days by the nominal week / month of their
    /// trade date (the holiday session belongs to the next trade date, so to
    /// its week). bar_label_ms is the stamp of the bar's native period,
    /// bucket_open_ms the group's first stamp, period_changes compares
    /// those, and feed(bar, next_input_ms) completes the running bucket on
    /// the last input bar before the next native stamp. The partition covers
    /// the feed: an input bar before the first stamp keeps the nominal key,
    /// and so does one at or after the LAST stamp's nominal period close
    /// (session_period_close_ms of feed_period, the feed's own timeframe --
    /// DAY for a daily feed), since without a next stamp nothing proves the
    /// last native bar reaches further; a chart day the feed does not hold
    /// stays a nominal session-day bucket with no native bar to substitute.
    /// Empty vectors (the default) leave every nominal rule bit-identical;
    /// RATIO / PASSTHROUGH ignore the call. Mismatched sizes or
    /// non-increasing stamps install nothing.
    void set_native_periods(std::vector<int64_t> stamps,
                            std::vector<int64_t> trade_instants,
                            CalendarPeriod feed_period);
    bool has_native_periods() const { return !native_stamps_.empty(); }

    /// Whether a session ending BEFORE its nominal close completes the
    /// running D/W/M bucket on that session's actual last input bar
    /// (feed(bar, next_input_ms) seeing the next input bar open a new
    /// period) -- true, the default -- or the bucket waits for the period's
    /// nominal close (the 16:45 bar reaching 17:00, the last traded
    /// session-day's close) and, when no input bar reaches it, completes
    /// lazily on the next period's first bar (false) -- unless the CALLING
    /// chart bar's nominal close reaches the period's (feed(bar,
    /// next_input_ms, calling_close_ms): the 1D chart's early-close daily
    /// bar, whose time_close stays 17:00 ET). TradingView finalizes
    /// on the actual last bar only where its session template carries the
    /// early close: exchange calendars (stock / futures / index -- NYSE:F's
    /// 13:00 half-days, CME's 12:00 CT early closes). OTC quote streams have
    /// no holiday template: on OANDA:XAUUSD 15m (cfd, 1800-1700 ET) the Fri
    /// 2025-07-04 session ends at the 12:45 ET bar, yet
    /// request.security(tickerid, "D", x) lookahead_off surfaces that day on
    /// Sun 07-06 18:00 -- the next session's first bar -- never on the 12:45
    /// bar; so does the Mon 2026-02-16 day ending 14:15 (on Mon 18:00) and
    /// the week holding the 07-04 close (lab tv oanda-{jul,julw,feb,febw}
    /// pin, ledger log-20260905t034240z-30be11fe, 2026-09-05). The engine
    /// sets it from syminfo.type (BacktestEngine::
    /// session_template_knows_early_close: false for forex / cfd / crypto).
    /// Read only by the CALENDAR next-input-bar rule without a native
    /// partition: ""/"24x7" sessions never reach that rule, the nominal
    /// rules are untouched, and an installed native period partition
    /// (set_native_periods) stays authoritative either way.
    void set_early_close_completes(bool on) { early_close_completes_ = on; }
    bool early_close_completes() const { return early_close_completes_; }

    /// CALENDAR: whether `prev_ms` and `curr_ms` lie in different periods
    /// of this aggregator -- different native periods / W-M groups when
    /// native periods are installed, crosses_boundary on the nominal
    /// session calendar otherwise. RATIO / PASSTHROUGH: false.
    bool period_changes(int64_t prev_ms, int64_t curr_ms) const;

    /// Open (Unix ms) of the target-TF bucket an input bar stamped `ms`
    /// belongs to, on the aggregator's anchor clock (syminfo tz + session):
    /// CALENDAR -> session_period_open_ms of the bar's D/W/M period (the
    /// forex week opens Sunday 17:00 ET, its month on the session whose
    /// close date is the 1st); RATIO -> session_intraday_bucket_open_ms,
    /// the day-stamp-anchored grid bucket (the same key feed() splits on
    /// and time("<intraday tf>") reads); PASSTHROUGH, or a
    /// count-only ratio with no wall-clock width, -> `ms` itself. Pure
    /// function of the configuration: it neither reads nor advances the
    /// aggregation state, so callers may query it before feeding the bar.
    int64_t bucket_open_ms(int64_t ms) const;

    /// Timestamp of the target-TF bar OPENED by an input bar stamped `ms`
    /// — what TradingView dates the aggregated bar, and what feed() stamps
    /// on every bucket it starts (finding 473). RATIO -> bucket_open_ms
    /// (the session-anchored grid open, whether or not the grid-opening
    /// sub-bar traded: a forex 1m tape that starts at 17:04 ET still yields
    /// the 17:00 chart bar); CALENDAR -> the day stamp of the session-day
    /// holding `ms` (the D/W/M bar is dated by its first TRADED session-day,
    /// so a holiday-Monday week stays Tuesday's bar, but never by a
    /// thin-open sub-bar inside that day; on OANDA 1800-1700 the stamp is
    /// 17:00 ET, an hour before the day trades); PASSTHROUGH -> `ms`.
    /// Gap-free feeds whose open is the stamp are bit-identical: there the
    /// first sub-bar IS the bucket open.
    int64_t bar_label_ms(int64_t ms) const;

private:
    enum class Mode { PASSTHROUGH, RATIO, CALENDAR };

    Mode mode_ = Mode::PASSTHROUGH;
    int ratio_ = 1;                    // for RATIO mode
    CalendarPeriod cal_period_ = CalendarPeriod::NONE; // for CALENDAR mode
    int64_t target_seconds_ = 0;       // wall-clock seconds for RATIO boundary detection
    int64_t input_seconds_ = 0;        // input bar duration (seconds), when known
    std::string anchor_tz_ = "UTC";    // syminfo.timezone (exchange clock)
    std::string anchor_session_;       // syminfo.session ("" or "24x7" = none)

    // Native period partition (set_native_periods): the stamps, and per
    // stamp the open of the W / M group it belongs to (the stamp itself
    // for DAY). Empty unless a native feed installed them.
    std::vector<int64_t> native_stamps_;
    std::vector<int64_t> native_group_open_;
    int64_t native_last_bound_ = 0;   // nominal close of the last stamp's period
    // set_early_close_completes: the next-input-bar completion applies.
    bool early_close_completes_ = true;

    Bar current_bar_{};
    Bar last_completed_bar_{};
    int sub_bar_count_ = 0;
    bool has_completed_ = false;
    bool current_emitted_complete_ = false;

    void reset_current(const Bar& bar);
    void merge_into_current(const Bar& bar);
    // Index of the native period holding `ms` (-1 before the first stamp).
    int native_index(int64_t ms) const;
};

} // namespace pineforge
