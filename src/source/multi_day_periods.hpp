#pragma once

// TradingView's N-day request bars ("2D", "3D", ...) as a generated period
// partition of bounded memory (CR-150 L19k).
//
// The rule (lane TAIL-A, lab tv pf-taila-ndgrid-*, tests/fixtures/
// multi_day_buckets): N consecutive trading dates of the symbol, counted from
// the first trading date of each calendar year, the year's last bar cut at its
// end. The trading dates are the session calendar's: every day on a 24x7
// symbol, every weekday on OANDA's (a holiday included: the 8D bar of EURUSD
// opens on 2025-12-25, a date its chart has no bar of), the exchange's
// sessions on a stock, an index or a future (NYSE:F's 8D bars skip Good
// Friday). The run knows an exchange's sessions from the bars it holds, the
// chart's and the auxiliary feed's; a date outside them counts when it is a
// weekday. A period opens at its first trading date's daily bar on a daily
// chart (NSE's Muhurat session opens its 8D bar at 13:45 IST), at that date's
// day stamp otherwise, and at the day stamp of the session day holding its
// local noon when no bar holds the date. The partition runs from the bar
// holding the first held date through the year after the last, whose dates
// follow the weekday rule: a stream's bars after the held ones.
//
// The bounded form. The periods are never stored. On ordered feeds a
// generator walks the feeds once, in time order, turning the sessions it
// meets into period opens one at a time: the days of a year are scanned in
// order, their held sessions are merged as they come, and only the running
// count of the year, the position in each feed and the day being scanned are
// kept. A source answers "which period holds this instant" from the last
// kMultiDayWindow - 1 opens the generator produced and the one it produced
// ahead; an instant before those restarts the generator from the first bar,
// and an instant after them advances it. Whatever the order of the questions
// the answers are the same, and for the questions a run asks -- the current
// period, the next input bar's, the previous one's -- they come from the
// buffer. The memory held is the same for a month of days and for forty years
// of them; ordered planning (make_multi_day_period_source) walks the feeds
// twice with the same few words of state, where the table it replaces held
// a node per held session day while it was built.
//
// The feeds are strictly increasing in time (the host refuses others), but
// their resolved session dates can go back at a clock change, including
// through the legacy coverage clock. When the bounded merge detects that,
// planning instead rescans both feeds for each calendar year's held dates.
// Only one year's date slots are retained, in kUnorderedRescanDays entries;
// all resolutions are visited, chart first, keeping each date's first stamp.
// Even a regression larger than the window is exact: the window limits
// retained dates, not how far back a rescan looks. The source reads the
// feeds' bars until the run that planned it is over; the aggregator it is
// installed in is rebuilt at the next run's prepare.
//
// Equivalence. The buffered form answers exactly what the stored table did;
// tests/test_multi_day_periods_bounded.cpp keeps that table's builder as the
// reference and compares the two bar for bar. The table was built at prepare,
// before the run's bar loop installs the chart's native day partition, so the
// session calendar it read was the nominal one; the generator reads the
// partition-free calendar too (NativeDayPartitionScope), however late in the
// run it is asked.

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pineforge::source {

/// The most period opens a multi-day period source holds at once: the recent
/// ones it answers short questions from, and the next one ahead.
inline constexpr std::size_t kMultiDayWindow = 16;

/// Bounds the fallback's retained trading-date slots to one calendar year,
/// including a leap year; N-day counting restarts each year. Clock changes
/// (also through the legacy coverage clock) normally regress dates by about
/// a day. No distance bound is assumed: full-feed rescans remain exact even
/// beyond this window, replacing the slots rather than accumulating years.
inline constexpr std::size_t kUnorderedRescanDays = 366;

/// What a run holds of the symbol the N-day request reads.
struct MultiDayFeeds {
    int days = 0;                    ///< N of the request, at least 2
    const Bar* chart = nullptr;      ///< the chart's bars, strictly increasing
    int n_chart = 0;
    bool daily_chart = false;        ///< a chart bar's own open dates its session day
    const Bar* aux = nullptr;        ///< the auxiliary feed's bars, strictly increasing
    std::size_t n_aux = 0;
    std::string tz;                  ///< the symbol's timezone
    std::string session;             ///< the symbol's session
    bool exchange_calendar = false;  ///< the held sessions are the trading dates
};

namespace multi_day_detail {

// A session day a feed holds: its ordinal (days since 1970-01-01 on the
// session clock) and the stamp of its bar.
struct HeldDay {
    std::int64_t ordinal = 0;
    std::int64_t value = 0;
};

// What planning learned of the run; immutable once the source exists.
struct Params {
    int days = 0;
    bool exchange_calendar = false;
    bool every_day = false;
    bool daily_chart = false;
    const Bar* chart = nullptr;
    std::size_t n_chart = 0;
    const Bar* aux = nullptr;
    std::size_t n_aux = 0;
    std::string tz;
    std::string session;
    native_calendar::SessionCalendar calendar;
    std::int64_t first = 0;       // the first and last session day any feed holds
    std::int64_t last = 0;
    std::int64_t first_year = 0;  // the year of the first held day
    std::int64_t last_year = 0;   // the year after the last held day's
};

// The session days one feed's bars fall in, in feed order. An intraday bar
// trades inside its session day: one inside the day the bar before it
// resolved is not resolved again (a daily chart resolves every bar).
class FeedWalk {
public:
    FeedWalk(const Params& params, const Bar* bars, std::size_t n, bool own_open)
        : params_(&params), bars_(bars), n_(bars == nullptr ? 0 : n), own_open_(own_open) {}

    bool next(HeldDay& out) {
        while (index_ < n_) {
            const std::int64_t ts = bars_[index_++].timestamp;
            if (!own_open_ && cycle_ && origin_ <= ts && ts < next_origin_) continue;
            const auto day = native_calendar::session_day_at(
                params_->calendar,
                session_covered_instant_ms(ts, params_->tz, params_->session));
            cycle_ = day.has_value();
            if (!day) continue;
            origin_ = day->origin_ms;
            next_origin_ = day->next_origin_ms;
            out.ordinal = day->ordinal;
            out.value = own_open_
                ? ts
                : session_period_open_ms(ts, params_->tz, params_->session, CalendarPeriod::DAY);
            return true;
        }
        return false;
    }

private:
    const Params* params_;
    const Bar* bars_;
    std::size_t n_;
    bool own_open_;
    std::size_t index_ = 0;
    bool cycle_ = false;
    std::int64_t origin_ = 0;
    std::int64_t next_origin_ = 0;
};

// One feed's days, each once and in ascending order, with the next one read
// ahead.
struct Lane {
    Lane(const Params& params, const Bar* bars, std::size_t n, bool own_open)
        : walk(params, bars, n, own_open) {}

    // Moves to the next day the feed holds beyond the one it gave last. A day
    // seen again keeps its first stamp; a day that goes back is refused.
    void refill(bool& ordered) {
        HeldDay day;
        while (walk.next(day)) {
            if (seen && day.ordinal == head.ordinal) continue;
            if (seen && day.ordinal < head.ordinal) {
                ordered = false;
                continue;
            }
            head = day;
            seen = true;
            live = true;
            return;
        }
        live = false;
    }

    FeedWalk walk;
    HeldDay head;
    bool live = false;
    bool seen = false;
};

// The session days either feed holds, ascending and each once. A day both
// hold keeps the chart's stamp.
class HeldDays {
public:
    explicit HeldDays(const Params& params)
        : chart_(params, params.chart, params.n_chart, params.daily_chart),
          aux_(params, params.aux, params.n_aux, false) {
        chart_.refill(ordered_);
        aux_.refill(ordered_);
    }

    bool next(HeldDay& out) {
        if (!chart_.live && !aux_.live) return false;
        if (chart_.live && (!aux_.live || chart_.head.ordinal <= aux_.head.ordinal)) {
            out = chart_.head;
            if (aux_.live && aux_.head.ordinal == out.ordinal) aux_.refill(ordered_);
            chart_.refill(ordered_);
        } else {
            out = aux_.head;
            aux_.refill(ordered_);
        }
        return true;
    }

    // False once a feed's days went back.
    bool ordered() const { return ordered_; }

private:
    Lane chart_;
    Lane aux_;
    bool ordered_ = true;
};

// The partition's period opens in ascending order, one at a time.
class Stamps {
public:
    explicit Stamps(const Params& params)
        : params_(&params),
          held_(params),
          year_(params.first_year),
          day_(native_calendar::native_civil_days(params.first_year, 1, 1)),
          year_end_(native_calendar::native_civil_days(params.first_year + 1, 1, 1)) {
        have_head_ = held_.next(head_);
    }

    // The next period's open; false at the end, or when the partition is
    // refused (failed()).
    bool next(std::int64_t& stamp) {
        if (failed_ || done_) return false;
        Open open;
        if (!started_) {
            started_ = true;
            // The first period is the last one opening at or before the first
            // held day; none opens there when the year's first trading date
            // comes after it.
            bool begun = false;
            Open begin;
            while (next_open(open)) {
                if (open.date > params_->first) {
                    ahead_ = open;
                    has_ahead_ = true;
                    break;
                }
                begin = open;
                begun = true;
            }
            if (!begun) {
                failed_ = true;
                return false;
            }
            open = begin;
        } else if (has_ahead_) {
            open = ahead_;
            has_ahead_ = false;
        } else if (!next_open(open)) {
            done_ = true;
            return false;
        }
        std::int64_t at = open.value;
        if (!open.held) {
            // A trading date no bar holds: the stamp of the session day
            // holding its local noon.
            const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(open.date);
            const auto noon = native_calendar::resolve_civil(
                params_->tz.empty() ? "UTC" : params_->tz, static_cast<int>(date.year),
                date.month, date.day, 12, 0);
            if (!noon) {
                failed_ = true;
                return false;
            }
            at = session_period_open_ms(noon->epoch_ms, params_->tz, params_->session,
                                        CalendarPeriod::DAY);
        }
        if (has_previous_ && at <= previous_) {
            failed_ = true;
            return false;
        }
        has_previous_ = true;
        previous_ = at;
        stamp = at;
        return true;
    }

    // True once the partition was refused: an open that does not come after
    // the one before it, a date whose local noon does not exist, or no period
    // opening at or before the first held day.
    bool failed() const { return failed_; }

private:
    struct Open {
        std::int64_t date = 0;   // the trading date (days since 1970-01-01)
        bool held = false;       // a feed holds it
        std::int64_t value = 0;  // its stamp when held
    };

    // The opens of every year from the first held day's through the year after
    // the last's: the year's trading dates counted from its first, one in N
    // opening a period.
    bool next_open(Open& open) {
        const Params& p = *params_;
        for (;;) {
            if (day_ >= year_end_) {
                if (year_ >= p.last_year) return false;
                ++year_;
                day_ = native_calendar::native_civil_days(year_, 1, 1);
                year_end_ = native_calendar::native_civil_days(year_ + 1, 1, 1);
                count_ = 0;
                continue;
            }
            const std::int64_t d = day_++;
            while (have_head_ && head_.ordinal < d) have_head_ = held_.next(head_);
            const bool is_held = have_head_ && head_.ordinal == d;
            bool trading = false;
            if (p.exchange_calendar && d >= p.first && d <= p.last) {
                trading = is_held;
            } else {
                const std::int64_t weekday = ((d % 7) + 7 + 4) % 7;  // 0 Sunday; 1970-01-01 a Thursday
                trading = p.every_day || (weekday >= 1 && weekday <= 5);
            }
            if (!trading) continue;
            if (count_++ % p.days == 0) {
                open.date = d;
                open.held = is_held;
                open.value = is_held ? head_.value : 0;
                return true;
            }
        }
    }

    const Params* params_;
    HeldDays held_;
    HeldDay head_;
    bool have_head_ = false;
    std::int64_t year_;
    std::int64_t day_;
    std::int64_t year_end_;
    std::int64_t count_ = 0;
    Open ahead_;
    bool has_ahead_ = false;
    bool started_ = false;
    bool done_ = false;
    bool failed_ = false;
    bool has_previous_ = false;
    std::int64_t previous_ = 0;
};

// The generated partition: the recent opens and the next one, over a
// generator that restarts when asked about an instant before them.
class Source final : public TimeframeAggregator::PeriodSource {
public:
    Source(std::shared_ptr<const Params> params, std::int64_t first_stamp,
           std::int64_t last_stamp)
        : params_(std::move(params)), first_stamp_(first_stamp), last_stamp_(last_stamp) {
        recent_.reserve(kBuffered + 1);
    }

    bool stamp_at(std::int64_t ms, std::int64_t& stamp, bool& last) override {
        if (ms < first_stamp_) return false;
        if (recent_.empty() || ms < recent_.front()) restart();
        if (recent_.empty()) return false;
        while (has_next_ && next_ <= ms) advance();
        const auto after = std::upper_bound(recent_.begin(), recent_.end(), ms);
        if (after == recent_.begin()) return false;
        stamp = *(after - 1);
        last = !has_next_ && after == recent_.end();
        return true;
    }

    std::int64_t last_stamp() const override { return last_stamp_; }

    std::size_t stamps_held() const override { return recent_.size() + (has_next_ ? 1u : 0u); }

private:
    static constexpr std::size_t kBuffered = kMultiDayWindow - 1;

    // Takes the next open into the recent ones, the oldest leaving when they
    // are full. The opens are the nominal calendar's whatever chart partition
    // the run has installed by now, as they were when the table was built at
    // prepare, before the run's bar loop.
    void advance() {
        NativeDayPartitionScope nominal(nullptr);
        recent_.push_back(next_);
        if (recent_.size() > kBuffered) recent_.erase(recent_.begin());
        has_next_ = stamps_->next(next_);
    }

    // Starts the generator over from the first bar: the first open and the
    // one after it.
    void restart() {
        NativeDayPartitionScope nominal(nullptr);
        stamps_ = std::make_unique<Stamps>(*params_);
        recent_.clear();
        has_next_ = false;
        std::int64_t first = 0;
        if (!stamps_->next(first)) return;
        recent_.push_back(first);
        has_next_ = stamps_->next(next_);
    }

    std::shared_ptr<const Params> params_;
    std::int64_t first_stamp_;
    std::int64_t last_stamp_;
    std::unique_ptr<Stamps> stamps_;
    std::vector<std::int64_t> recent_;
    std::int64_t next_ = 0;
    bool has_next_ = false;
};

// Separate fallback classes keep the ordered generator's code, object sizes
// and allocation path unchanged. A year's slots are the old map restricted
// to that year, rebuilt from the beginning of both feeds on every reload.
class RescannedHeldDays {
public:
    explicit RescannedHeldDays(const Params& params) : params_(&params) {}

    bool find(std::int64_t ordinal, std::int64_t& value) {
        if (failed_ || ordinal < params_->first || ordinal > params_->last) return false;
        if (!loaded_ || ordinal < begin_ || ordinal >= end_) {
            const std::int64_t year = native_calendar::native_civil_date(ordinal).year;
            begin_ = native_calendar::native_civil_days(year, 1, 1);
            end_ = native_calendar::native_civil_days(year + 1, 1, 1);
            if (end_ <= begin_
                || end_ - begin_ > static_cast<std::int64_t>(kUnorderedRescanDays)) {
                failed_ = true;
                return false;
            }
            present_.fill(false);
            scan(params_->chart, params_->n_chart, params_->daily_chart);
            if (!failed_) scan(params_->aux, params_->n_aux, false);
            loaded_ = true;
        }
        if (failed_) return false;
        if (ordinal < begin_ || ordinal >= end_) {
            failed_ = true;
            return false;
        }
        const std::size_t at = static_cast<std::size_t>(ordinal - begin_);
        if (at >= values_.size()) {
            failed_ = true;
            return false;
        }
        if (!present_[at]) return false;
        value = values_[at];
        return true;
    }

    bool failed() const { return failed_; }

private:
    void scan(const Bar* bars, std::size_t n, bool own_open) {
        FeedWalk walk(*params_, bars, n, own_open);
        HeldDay day;
        while (walk.next(day)) {
            // Never stop at a high ordinal: a later resolution can return
            // to this year, even from more than one window away.
            if (day.ordinal < begin_ || day.ordinal >= end_) continue;
            const std::size_t at = static_cast<std::size_t>(day.ordinal - begin_);
            if (at >= values_.size()) {
                failed_ = true;
                return;
            }
            if (!present_[at]) {
                present_[at] = true;
                values_[at] = day.value;
            }
        }
    }

    const Params* params_;
    std::array<std::int64_t, kUnorderedRescanDays> values_{};
    std::array<bool, kUnorderedRescanDays> present_{};
    std::int64_t begin_ = 0;
    std::int64_t end_ = 0;
    bool loaded_ = false;
    bool failed_ = false;
};

// The same date counting, first-period selection and stamp validation as
// Stamps, using a rescan lookup instead of the ordered held-date iterator.
class RescannedStamps {
public:
    explicit RescannedStamps(const Params& params)
        : params_(&params),
          held_(params),
          year_(params.first_year),
          day_(native_calendar::native_civil_days(params.first_year, 1, 1)),
          year_end_(native_calendar::native_civil_days(params.first_year + 1, 1, 1)) {}

    bool next(std::int64_t& stamp) {
        if (failed_ || done_) return false;
        Open open;
        if (!started_) {
            started_ = true;
            bool begun = false;
            Open begin;
            while (next_open(open)) {
                if (open.date > params_->first) {
                    ahead_ = open;
                    has_ahead_ = true;
                    break;
                }
                begin = open;
                begun = true;
            }
            if (!begun) {
                failed_ = true;
                return false;
            }
            open = begin;
        } else if (has_ahead_) {
            open = ahead_;
            has_ahead_ = false;
        } else if (!next_open(open)) {
            done_ = true;
            return false;
        }
        std::int64_t at = open.value;
        if (!open.held) {
            const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(open.date);
            const auto noon = native_calendar::resolve_civil(
                params_->tz.empty() ? "UTC" : params_->tz, static_cast<int>(date.year),
                date.month, date.day, 12, 0);
            if (!noon) {
                failed_ = true;
                return false;
            }
            at = session_period_open_ms(noon->epoch_ms, params_->tz, params_->session,
                                        CalendarPeriod::DAY);
        }
        if (has_previous_ && at <= previous_) {
            failed_ = true;
            return false;
        }
        has_previous_ = true;
        previous_ = at;
        stamp = at;
        return true;
    }

    bool failed() const { return failed_; }

private:
    struct Open {
        std::int64_t date = 0;
        bool held = false;
        std::int64_t value = 0;
    };

    bool next_open(Open& open) {
        const Params& p = *params_;
        for (;;) {
            if (day_ >= year_end_) {
                if (year_ >= p.last_year) return false;
                ++year_;
                day_ = native_calendar::native_civil_days(year_, 1, 1);
                year_end_ = native_calendar::native_civil_days(year_ + 1, 1, 1);
                count_ = 0;
                continue;
            }
            const std::int64_t d = day_++;
            std::int64_t value = 0;
            const bool is_held = held_.find(d, value);
            if (held_.failed()) {
                failed_ = true;
                return false;
            }
            bool trading = false;
            if (p.exchange_calendar && d >= p.first && d <= p.last) {
                trading = is_held;
            } else {
                const std::int64_t weekday = ((d % 7) + 7 + 4) % 7;
                trading = p.every_day || (weekday >= 1 && weekday <= 5);
            }
            if (!trading) continue;
            if (count_++ % p.days == 0) {
                open.date = d;
                open.held = is_held;
                open.value = value;
                return true;
            }
        }
    }

    const Params* params_;
    RescannedHeldDays held_;
    std::int64_t year_;
    std::int64_t day_;
    std::int64_t year_end_;
    std::int64_t count_ = 0;
    Open ahead_;
    bool has_ahead_ = false;
    bool started_ = false;
    bool done_ = false;
    bool failed_ = false;
    bool has_previous_ = false;
    std::int64_t previous_ = 0;
};

// The ordered Source's recent-open/restart policy over the fallback
// generator, without adding a fallback field or dispatch to Source.
class RescannedSource final : public TimeframeAggregator::PeriodSource {
public:
    RescannedSource(std::shared_ptr<const Params> params, std::int64_t first_stamp,
                    std::int64_t last_stamp)
        : params_(std::move(params)), first_stamp_(first_stamp), last_stamp_(last_stamp) {
        recent_.reserve(kBuffered + 1);
    }

    bool stamp_at(std::int64_t ms, std::int64_t& stamp, bool& last) override {
        if (ms < first_stamp_) return false;
        if (recent_.empty() || ms < recent_.front()) restart();
        if (recent_.empty()) return false;
        while (has_next_ && next_ <= ms) advance();
        const auto after = std::upper_bound(recent_.begin(), recent_.end(), ms);
        if (after == recent_.begin()) return false;
        stamp = *(after - 1);
        last = !has_next_ && after == recent_.end();
        return true;
    }

    std::int64_t last_stamp() const override { return last_stamp_; }

    std::size_t stamps_held() const override { return recent_.size() + (has_next_ ? 1u : 0u); }

private:
    static constexpr std::size_t kBuffered = kMultiDayWindow - 1;

    void advance() {
        NativeDayPartitionScope nominal(nullptr);
        recent_.push_back(next_);
        if (recent_.size() > kBuffered) recent_.erase(recent_.begin());
        has_next_ = stamps_->next(next_);
    }

    void restart() {
        NativeDayPartitionScope nominal(nullptr);
        // Release the previous date window before allocating the next one.
        stamps_.reset();
        stamps_ = std::make_unique<RescannedStamps>(*params_);
        recent_.clear();
        has_next_ = false;
        std::int64_t first = 0;
        if (!stamps_->next(first)) return;
        recent_.push_back(first);
        has_next_ = stamps_->next(next_);
    }

    std::shared_ptr<const Params> params_;
    std::int64_t first_stamp_;
    std::int64_t last_stamp_;
    std::unique_ptr<RescannedStamps> stamps_;
    std::vector<std::int64_t> recent_;
    std::int64_t next_ = 0;
    bool has_next_ = false;
};

inline std::shared_ptr<TimeframeAggregator::PeriodSource> make_rescanned_source(
        std::shared_ptr<Params> params) {
    bool any = false;
    const auto scan_bounds = [&](const Bar* bars, std::size_t n, bool own_open) {
        FeedWalk walk(*params, bars, n, own_open);
        HeldDay day;
        while (walk.next(day)) {
            if (!any) {
                params->first = day.ordinal;
                params->last = day.ordinal;
            } else {
                params->first = std::min(params->first, day.ordinal);
                params->last = std::max(params->last, day.ordinal);
            }
            any = true;
        }
    };
    scan_bounds(params->chart, params->n_chart, params->daily_chart);
    scan_bounds(params->aux, params->n_aux, false);
    if (!any) return nullptr;
    params->first_year = native_calendar::native_civil_date(params->first).year;
    params->last_year = native_calendar::native_civil_date(params->last).year + 1;
    std::int64_t first_stamp = 0;
    std::int64_t last_stamp = 0;
    {
        // Allocate the date arrays only after the ordered scan failed;
        // keep them out of the ordered generator and source objects.
        auto stamps = std::make_unique<RescannedStamps>(*params);
        std::int64_t stamp = 0;
        bool have_stamp = false;
        while (stamps->next(stamp)) {
            if (!have_stamp) first_stamp = stamp;
            last_stamp = stamp;
            have_stamp = true;
        }
        if (!have_stamp || stamps->failed()) return nullptr;
    }
    return std::make_shared<RescannedSource>(std::move(params), first_stamp, last_stamp);
}

}  // namespace multi_day_detail

/// The N-day partition of `feeds`, or null where TradingView's rule yields
/// none: fewer than two days, a session that does not parse, no held session
/// day, no period opening at or before the first held day, an open that does
/// not come after the one before it, or a date whose local noon does not
/// exist. Ordered planning walks the feeds twice (the first and last held
/// day, then every period once to prove the partition whole). An unordered
/// feed instead uses full-feed rescans with at most kUnorderedRescanDays
/// retained date slots; neither path keeps a table of all held days.
inline std::shared_ptr<TimeframeAggregator::PeriodSource> make_multi_day_period_source(
        const MultiDayFeeds& feeds) {
    namespace detail = multi_day_detail;
    if (feeds.days < 2) return nullptr;
    NativeDayPartitionScope nominal(nullptr);
    auto calendar = native_calendar::parse_session(feeds.session, feeds.tz);
    if (!calendar) return nullptr;
    auto params = std::make_shared<detail::Params>();
    params->days = feeds.days;
    params->exchange_calendar = feeds.exchange_calendar;
    params->every_day = feeds.session.empty() || feeds.session == "24x7";
    params->daily_chart = feeds.daily_chart;
    params->chart = feeds.chart;
    params->n_chart = feeds.n_chart > 0 ? static_cast<std::size_t>(feeds.n_chart) : 0;
    params->aux = feeds.aux;
    params->n_aux = feeds.n_aux;
    params->tz = feeds.tz;
    params->session = feeds.session;
    params->calendar = std::move(*calendar);
    {
        detail::HeldDays held(*params);
        detail::HeldDay day;
        bool any = false;
        while (held.next(day)) {
            if (!any) params->first = day.ordinal;
            params->last = day.ordinal;
            any = true;
        }
        if (!any || !held.ordered()) {
            if (!any) return nullptr;
            return detail::make_rescanned_source(std::move(params));
        }
    }
    params->first_year = native_calendar::native_civil_date(params->first).year;
    params->last_year = native_calendar::native_civil_date(params->last).year + 1;
    std::int64_t first_stamp = 0;
    std::int64_t last_stamp = 0;
    {
        detail::Stamps stamps(*params);
        std::int64_t stamp = 0;
        bool any = false;
        while (stamps.next(stamp)) {
            if (!any) first_stamp = stamp;
            last_stamp = stamp;
            any = true;
        }
        if (!any || stamps.failed()) return nullptr;
    }
    return std::make_shared<detail::Source>(std::move(params), first_stamp, last_stamp);
}

}  // namespace pineforge::source
