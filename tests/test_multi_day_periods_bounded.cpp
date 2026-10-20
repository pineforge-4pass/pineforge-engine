/*
 * test_multi_day_periods_bounded.cpp -- CR-150 L19k.
 *
 * An N-day request.security ("2D" .. "13D") buckets the symbol's trading
 * dates N at a time from each year's first. The partition used to be a stored
 * table (one stamp per N trading dates per site, built from a map with one
 * node per held session day); it is now generated from the feeds as the run
 * asks for it (src/source/multi_day_periods.hpp) and the aggregator keeps a
 * window of it, not the table.
 *
 * The reference here is that table's builder, kept verbatim as
 * reference_multi_day_stamps and installed with set_native_periods exactly as
 * the host installed it. Every row builds a feed (a 24x7 crypto day tape, an
 * OANDA weekday tape with holidays that have no bar, a NYSE daily tape with
 * holidays and week-long outages, an NSE tape with weekend sessions and
 * Muhurat-late opens, intraday charts, a chart plus an auxiliary feed that
 * each hold days the other lacks, an auxiliary feed that starts before the
 * chart and ends after it, a chart starting mid-year) and, for each N
 * in {2, 3, 5, 7, 10} and both calendar rules, compares the generated
 * partition with the stored one on:
 *   - bucket_open_ms, bar_label_ms and period_changes at every bar, one
 *     millisecond either side of it, at every period's open, between opens,
 *     before the first, and after the last period's nominal close;
 *   - the same questions asked from the last instant back, and in a fixed
 *     shuffle: a source that only worked forward would fail them;
 *   - the bucket in progress, the completed bucket and the sub-bar count of
 *     feed() over the whole feed, with the next input bar known.
 * The retained state is then measured: after a full sweep the generated
 * partition holds at most kMultiDayWindow opens whether the feed has two
 * years or forty, where the stored one grows with them.
 *
 * The table was built at prepare, before the run's bar loop installs the
 * chart's native day partition; the generator is asked during the loop. A
 * partition on the chart's own clock, whose stamps move the calendar, is
 * installed around the questions and around a second planning: the answers
 * stay the nominal calendar's.
 *
 * Last, a host run on a 3300-day 24x7 tape registers "2D".."10D" sites,
 * and the aggregator the host installed answers each chart bar with the open
 * the closed form of the rule gives (a day count from the year's first), the
 * reference's, and within the window.
 */

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/source/multi_day_periods.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;
static int failures_printed = 0;

#define CHECK_CTX(expr, ctx)                                                   \
    do {                                                                       \
        if (!(expr)) {                                                         \
            if (failures_printed++ < 40)                                       \
                std::printf("  FAIL  %s:%d  %s  [%s]\n", __FILE__, __LINE__,   \
                            #expr, std::string(ctx).c_str());                  \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK(expr) CHECK_CTX(expr, "")

namespace {

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

[[noreturn]] void die(const char* what) {
    std::printf("  FATAL  %s\n", what);
    std::exit(2);
}

// ─── The reference: the stored table's builder, as the host had it ───────────
//
// Verbatim from src/source/pine_strategy_host.cpp at b3c39238e
// (tradingview_multi_day_stamps), under another name.
std::vector<std::int64_t> reference_multi_day_stamps(
        int days, const Bar* chart, int n_chart, bool daily_chart, const std::vector<Bar>& aux,
        const std::string& tz, const std::string& session, bool exchange_calendar) {
    namespace cal = native_calendar;
    const auto calendar = cal::parse_session(session, tz);
    if (!calendar || days < 2) return {};
    // Trading date (days since 1970-01-01) -> its bar's open.
    std::map<std::int64_t, std::int64_t> held;
    std::optional<cal::NativeSessionDay> cycle;
    const auto hold = [&](std::int64_t ts, bool own_open) {
        cycle = cal::session_day_at(*calendar, session_covered_instant_ms(ts, tz, session));
        if (cycle && held.find(cycle->ordinal) == held.end()) {
            held.emplace(cycle->ordinal,
                         own_open ? ts : session_period_open_ms(ts, tz, session, CalendarPeriod::DAY));
        }
    };
    // An intraday bar trades inside its session day: one inside the day the
    // bar before it resolved is not resolved again.
    for (int i = 0; i < n_chart; ++i) {
        if (daily_chart || !cycle || !cycle->holds(chart[i].timestamp))
            hold(chart[i].timestamp, daily_chart);
    }
    cycle.reset();
    for (const Bar& bar : aux) {
        if (!cycle || !cycle->holds(bar.timestamp)) hold(bar.timestamp, false);
    }
    if (held.empty()) return {};
    const std::int64_t first = held.begin()->first;
    const std::int64_t last = held.rbegin()->first;
    const bool every_day = session.empty() || session == "24x7";
    const auto trades = [&](std::int64_t d) {
        if (exchange_calendar && d >= first && d <= last) return held.count(d) > 0;
        const std::int64_t weekday = ((d % 7) + 7 + 4) % 7;  // 0 Sunday; 1970-01-01 a Thursday
        return every_day || (weekday >= 1 && weekday <= 5);
    };
    // The bar-opening dates of every year from the first held date's through
    // the year after the last's, whose first trading date opens a bar.
    std::vector<std::int64_t> opens;
    const std::int64_t first_year = cal::native_civil_date(first).year;
    const std::int64_t last_year = cal::native_civil_date(last).year + 1;
    for (std::int64_t year = first_year; year <= last_year; ++year) {
        const std::int64_t end = cal::native_civil_days(year + 1, 1, 1);
        std::int64_t count = 0;
        for (std::int64_t d = cal::native_civil_days(year, 1, 1); d < end; ++d) {
            if (!trades(d)) continue;
            if (count++ % days == 0) opens.push_back(d);
        }
    }
    auto begin = std::upper_bound(opens.begin(), opens.end(), first);
    if (begin == opens.begin()) return {};
    --begin;
    // Through the next year's bars too, on the weekday rule: a stream's bars
    // after the held ones.
    std::vector<std::int64_t> stamps;
    for (auto it = begin; it != opens.end(); ++it) {
        const auto found = held.find(*it);
        if (found != held.end()) {
            stamps.push_back(found->second);
            continue;
        }
        // A trading date no bar holds: the stamp of the session day holding
        // its local noon.
        const cal::NativeCivilDate date = cal::native_civil_date(*it);
        const auto noon = cal::resolve_civil(tz.empty() ? "UTC" : tz, static_cast<int>(date.year),
                                             date.month, date.day, 12, 0);
        if (!noon) return {};
        stamps.push_back(session_period_open_ms(noon->epoch_ms, tz, session, CalendarPeriod::DAY));
    }
    for (std::size_t k = 1; k < stamps.size(); ++k) {
        if (stamps[k] <= stamps[k - 1]) return {};
    }
    return stamps;
}

// ─── Feeds ───────────────────────────────────────────────────────────────────

std::int64_t civil(int year, int month, int day) {
    return native_calendar::native_civil_days(year, month, day);
}

std::int64_t weekday_of(std::int64_t day) { return ((day % 7) + 7 + 4) % 7; }  // 0 Sunday

// Epoch ms of hour:minute on civil day `day` (days since 1970-01-01) in `tz`.
std::int64_t at_local(const std::string& tz, std::int64_t day, int hour, int minute) {
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(day);
    const auto at = native_calendar::resolve_civil(tz, static_cast<int>(date.year), date.month,
                                                   date.day, hour, minute);
    if (!at) die("a civil time of the fixtures does not resolve");
    return at->epoch_ms;
}

// A fixed pseudo-random word of a day: the same holidays on every machine.
std::uint32_t mix(std::int64_t v) {
    std::uint64_t x = static_cast<std::uint64_t>(v) * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 32;
    return static_cast<std::uint32_t>(x);
}

Bar bar_at(std::int64_t ts, std::size_t i) {
    const double v = 100.0 + static_cast<double>(i % 17);
    return Bar{v, v + 1.0, v - 1.0, v + 0.5, 1.0 + static_cast<double>(i % 5), ts};
}

bool us_closed(std::int64_t d) {
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(d);
    if (date.month == 1 && date.day == 1) return true;
    if (date.month == 12 && date.day == 25) return true;
    if (mix(d) % 29 == 0) return true;  // a holiday somewhere in the year
    return (d >= civil(2016, 3, 7) && d <= civil(2016, 3, 13))     // a week-long outage
        || (d >= civil(2017, 9, 4) && d <= civil(2017, 9, 13));    // and a longer one
}

bool fx_closed(std::int64_t d) {
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(d);
    return (date.month == 1 && date.day == 1) || (date.month == 12 && date.day == 25)
        || mix(d) % 53 == 0;
}

bool index_closed(std::int64_t d) { return mix(d) % 31 == 0; }

bool aux_closed(std::int64_t d) {
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(d);
    return (date.month == 1 && date.day == 1) || mix(d) % 37 == 0;
}

template <class Closed>
std::vector<std::int64_t> weekdays(std::int64_t from, std::int64_t to, Closed closed) {
    std::vector<std::int64_t> out;
    for (std::int64_t d = from; d < to; ++d) {
        const std::int64_t wd = weekday_of(d);
        if (wd >= 1 && wd <= 5 && !closed(d)) out.push_back(d);
    }
    return out;
}

struct Scenario {
    std::string name;
    std::string tz;
    std::string session;
    std::string input_tf;  // the chart's timeframe
    std::string aux_tf;    // the auxiliary feed's, when it has bars
    bool daily_chart = false;
    std::vector<Bar> chart;
    std::vector<Bar> aux;
};

Scenario crypto_daily(const char* name, std::int64_t first_day, int n) {
    Scenario sc;
    sc.name = name;
    sc.tz = "UTC";
    sc.session = "24x7";
    sc.input_tf = "1D";
    sc.daily_chart = true;
    for (int i = 0; i < n; ++i)
        sc.chart.push_back(bar_at((first_day + i) * kDay, static_cast<std::size_t>(i)));
    return sc;
}

Scenario crypto_intraday(const char* name, std::int64_t first_day, int n_days) {
    Scenario sc;
    sc.name = name;
    sc.tz = "UTC";
    sc.session = "24x7";
    sc.input_tf = "240";
    sc.daily_chart = false;
    std::size_t i = 0;
    for (int d = 0; d < n_days; ++d)
        for (int h = 0; h < 24; h += 4)
            sc.chart.push_back(bar_at((first_day + d) * kDay + h * kHour, i++));
    return sc;
}

// OANDA: a weekday session 1700-1700 ET; Monday's bar is stamped Sunday 17:00.
Scenario fx_daily(const char* name, std::int64_t from, std::int64_t to) {
    Scenario sc;
    sc.name = name;
    sc.tz = "America/New_York";
    sc.session = "1700-1700";
    sc.input_tf = "1D";
    sc.daily_chart = true;
    std::size_t i = 0;
    for (const std::int64_t d : weekdays(from, to, fx_closed))
        sc.chart.push_back(bar_at(at_local(sc.tz, d - 1, 17, 0), i++));
    return sc;
}

Scenario stock_daily(const char* name, std::int64_t from, std::int64_t to) {
    Scenario sc;
    sc.name = name;
    sc.tz = "America/New_York";
    sc.session = "0930-1600";
    sc.input_tf = "1D";
    sc.daily_chart = true;
    std::size_t i = 0;
    for (const std::int64_t d : weekdays(from, to, us_closed))
        sc.chart.push_back(bar_at(at_local(sc.tz, d, 9, 30), i++));
    return sc;
}

Scenario stock_intraday(const char* name, std::int64_t from, std::int64_t to) {
    Scenario sc;
    sc.name = name;
    sc.tz = "America/New_York";
    sc.session = "0930-1600";
    sc.input_tf = "60";
    sc.daily_chart = false;
    std::size_t i = 0;
    for (const std::int64_t d : weekdays(from, to, us_closed))
        for (int k = 0; k < 7; ++k)
            sc.chart.push_back(bar_at(at_local(sc.tz, d, 9, 30) + k * kHour, i++));
    return sc;
}

// NSE: weekdays, a few weekend sessions, and days whose daily bar opens late
// (the Muhurat session's 13:45).
Scenario index_daily(const char* name, std::int64_t from, std::int64_t to) {
    Scenario sc;
    sc.name = name;
    sc.tz = "Asia/Kolkata";
    sc.session = "0915-1530";
    sc.input_tf = "1D";
    sc.daily_chart = true;
    std::size_t i = 0;
    for (std::int64_t d = from; d < to; ++d) {
        const std::int64_t wd = weekday_of(d);
        const bool weekend = wd == 0 || wd == 6;
        if (weekend ? mix(d) % 97 != 1 : index_closed(d)) continue;
        const bool late = mix(d) % 61 == 7;
        sc.chart.push_back(bar_at(late ? at_local(sc.tz, d, 13, 45) : at_local(sc.tz, d, 9, 15), i++));
    }
    return sc;
}

// A daily chart and an hourly auxiliary feed that each hold days the other
// lacks; on days both hold the chart's bar opens late, so the chart's stamp is
// the one that must win.
Scenario chart_with_aux(const char* name) {
    Scenario sc;
    sc.name = name;
    sc.tz = "America/New_York";
    sc.session = "0930-1600";
    sc.input_tf = "1D";
    sc.aux_tf = "60";
    sc.daily_chart = true;
    std::size_t i = 0;
    for (const std::int64_t d : weekdays(civil(2013, 1, 2), civil(2019, 1, 1), us_closed)) {
        const bool late = mix(d) % 43 == 3;
        sc.chart.push_back(bar_at(late ? at_local(sc.tz, d, 13, 45) : at_local(sc.tz, d, 9, 30), i++));
    }
    std::size_t j = 0;
    for (const std::int64_t d : weekdays(civil(2015, 6, 1), civil(2018, 12, 31), aux_closed))
        for (int k = 0; k < 7; ++k)
            sc.aux.push_back(bar_at(at_local(sc.tz, d, 9, 30) + k * kHour, j++));
    return sc;
}

// An auxiliary feed that starts before the chart's first bar and ends after
// its last: the feed alone sets the first and last held day and the years the
// partition runs through, and the chart's late stamps still win where both
// hold a day.
Scenario aux_around_chart(const char* name) {
    Scenario sc;
    sc.name = name;
    sc.tz = "America/New_York";
    sc.session = "0930-1600";
    sc.input_tf = "1D";
    sc.aux_tf = "60";
    sc.daily_chart = true;
    std::size_t i = 0;
    for (const std::int64_t d : weekdays(civil(2014, 3, 3), civil(2016, 9, 1), us_closed)) {
        const bool late = mix(d) % 43 == 3;
        sc.chart.push_back(bar_at(late ? at_local(sc.tz, d, 13, 45) : at_local(sc.tz, d, 9, 30), i++));
    }
    std::size_t j = 0;
    for (const std::int64_t d : weekdays(civil(2013, 11, 4), civil(2017, 2, 1), aux_closed))
        for (int k = 0; k < 7; ++k)
            sc.aux.push_back(bar_at(at_local(sc.tz, d, 9, 30) + k * kHour, j++));
    return sc;
}

// ─── The two installs under comparison ───────────────────────────────────────

using Partition = std::shared_ptr<TimeframeAggregator::PeriodSource>;

// The aggregator of one "<N>D" site, as the host builds it: fed the auxiliary
// feed when the run has one, the chart otherwise.
TimeframeAggregator make_aggregator(const Scenario& sc, int days) {
    return TimeframeAggregator(std::to_string(days) + "D", sc.aux.empty() ? sc.input_tf : sc.aux_tf,
                               sc.tz, sc.session);
}

const std::vector<Bar>& input_of(const Scenario& sc) { return sc.aux.empty() ? sc.chart : sc.aux; }

source::MultiDayFeeds feeds_of(const Scenario& sc, int days, bool exchange) {
    source::MultiDayFeeds feeds;
    feeds.days = days;
    feeds.chart = sc.chart.data();
    feeds.n_chart = static_cast<int>(sc.chart.size());
    feeds.daily_chart = sc.daily_chart;
    feeds.aux = sc.aux.data();
    feeds.n_aux = sc.aux.size();
    feeds.tz = sc.tz;
    feeds.session = sc.session;
    feeds.exchange_calendar = exchange;
    return feeds;
}

Partition make_partition(const Scenario& sc, int days, bool exchange) {
    return source::make_multi_day_period_source(feeds_of(sc, days, exchange));
}

std::vector<std::int64_t> reference_of(const Scenario& sc, int days, bool exchange) {
    return reference_multi_day_stamps(days, sc.chart.data(), static_cast<int>(sc.chart.size()),
                                      sc.daily_chart, sc.aux, sc.tz, sc.session, exchange);
}

// The stored table, installed the way the host installed it.
TimeframeAggregator stored_partition(const Scenario& sc, int days,
                                     const std::vector<std::int64_t>& stamps) {
    TimeframeAggregator agg = make_aggregator(sc, days);
    if (!stamps.empty()) agg.set_native_periods(stamps, stamps, CalendarPeriod::DAY);
    return agg;
}

TimeframeAggregator generated_partition(const Scenario& sc, int days, const Partition& partition) {
    TimeframeAggregator agg = make_aggregator(sc, days);
    agg.set_native_period_source(partition);
    return agg;
}

// ─── The questions ───────────────────────────────────────────────────────────

std::vector<std::int64_t> probe_points(const Scenario& sc, const std::vector<std::int64_t>& stamps,
                                       std::int64_t last_bound) {
    std::vector<std::int64_t> p;
    const auto around = [&](std::int64_t t) {
        p.push_back(t - 1);
        p.push_back(t);
        p.push_back(t + 1);
    };
    const std::size_t chart_step = sc.daily_chart ? 1 : 3;
    for (std::size_t i = 0; i < sc.chart.size(); i += chart_step) around(sc.chart[i].timestamp);
    for (std::size_t i = 0; i < sc.aux.size(); i += 5) around(sc.aux[i].timestamp);
    for (std::size_t k = 0; k < stamps.size(); ++k) {
        around(stamps[k]);
        if (k + 1 < stamps.size()) p.push_back(stamps[k] + (stamps[k + 1] - stamps[k]) / 2);
    }
    if (!stamps.empty()) {
        p.push_back(stamps.front() - 40 * kDay);
        p.push_back(stamps.back() + 2 * kDay);
        p.push_back(stamps.back() + 400 * kDay);
        around(last_bound - kDay);
        around(last_bound);
    }
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    return p;
}

std::vector<std::int64_t> every_nth(const std::vector<std::int64_t>& points, std::size_t at_most) {
    const std::size_t step = std::max<std::size_t>(1, points.size() / at_most);
    std::vector<std::int64_t> out;
    for (std::size_t i = 0; i < points.size(); i += step) out.push_back(points[i]);
    return out;
}

// Both aggregators are asked the same things in the same order.
void compare_in_order(const TimeframeAggregator& ref, const TimeframeAggregator& made,
                      const std::vector<std::int64_t>& order, const std::string& ctx) {
    std::int64_t previous = 0;
    bool have_previous = false;
    for (const std::int64_t p : order) {
        CHECK_CTX(made.bucket_open_ms(p) == ref.bucket_open_ms(p),
                  ctx + " bucket_open_ms at " + std::to_string(p));
        CHECK_CTX(made.bar_label_ms(p) == ref.bar_label_ms(p),
                  ctx + " bar_label_ms at " + std::to_string(p));
        if (have_previous) {
            CHECK_CTX(made.period_changes(previous, p) == ref.period_changes(previous, p),
                      ctx + " period_changes at " + std::to_string(p));
        }
        previous = p;
        have_previous = true;
    }
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.open == b.open && a.high == b.high && a.low == b.low && a.close == b.close
        && a.volume == b.volume && a.timestamp == b.timestamp;
}

// feed() over the whole input with the next input bar known; returns the most
// opens the generated partition held at any bar.
std::size_t compare_feed(const Scenario& sc, int days, const std::vector<std::int64_t>& stamps,
                         const Partition& partition, const std::string& ctx) {
    const std::vector<Bar>& input = input_of(sc);
    TimeframeAggregator ref = stored_partition(sc, days, stamps);
    TimeframeAggregator made = generated_partition(sc, days, partition);
    std::size_t most = made.native_period_stamps_held();
    for (std::size_t i = 0; i < input.size(); ++i) {
        const std::int64_t next_ms = i + 1 < input.size() ? input[i + 1].timestamp : 0;
        const AggregatedBar a = ref.feed(input[i], next_ms, 0);
        const AggregatedBar b = made.feed(input[i], next_ms, 0);
        const bool same = same_bar(a.bar, b.bar) && a.is_complete == b.is_complete
            && a.sub_bar_count == b.sub_bar_count;
        CHECK_CTX(same, ctx + " feed " + std::to_string(i));
        most = std::max(most, made.native_period_stamps_held());
    }
    CHECK_CTX(same_bar(ref.current(), made.current()), ctx + " current bar");
    CHECK_CTX(same_bar(ref.last_completed(), made.last_completed()), ctx + " last completed bar");
    return most;
}

// One feed, one N, one calendar rule: the generated partition against the
// stored one.
void run_scenario(const Scenario& sc, int days, bool exchange, bool expect_partition) {
    const std::string ctx = sc.name + " " + std::to_string(days) + "D"
        + (exchange ? " exchange" : " weekday");
    const std::vector<std::int64_t> stamps = reference_of(sc, days, exchange);
    if (expect_partition) CHECK_CTX(!stamps.empty(), ctx + " has a reference partition");

    // One partition serves every aggregator below, one after the other: what
    // each question is answered with must not depend on what was asked before.
    const Partition partition = make_partition(sc, days, exchange);
    CHECK_CTX((partition != nullptr) == !stamps.empty(), ctx + " plans a partition");
    if (stamps.empty() || !partition) return;
    {
        TimeframeAggregator installed = make_aggregator(sc, days);
        CHECK_CTX(installed.set_native_period_source(partition), ctx + " installs");
        CHECK_CTX(installed.has_native_periods(), ctx + " has native periods");
    }

    const TimeframeAggregator ref = stored_partition(sc, days, stamps);
    const std::int64_t last_bound = session_period_close_ms(
        stamps.back(), sc.tz, sc.session, CalendarPeriod::DAY);
    const std::vector<std::int64_t> points = probe_points(sc, stamps, last_bound);
    CHECK_CTX(points.size() > 10, ctx + " has probes");

    // Forward, as a run asks.
    compare_in_order(ref, generated_partition(sc, days, partition), points, ctx + " ascending");

    // The feed's pattern: the next input bar's period, then this bar's.
    {
        const TimeframeAggregator made = generated_partition(sc, days, partition);
        const std::vector<Bar>& input = input_of(sc);
        const std::size_t step = std::max<std::size_t>(1, input.size() / 4000);
        for (std::size_t i = 0; i + 1 < input.size(); i += step) {
            const std::int64_t next_ms = input[i + 1].timestamp;
            const std::int64_t ms = input[i].timestamp;
            CHECK_CTX(made.bucket_open_ms(next_ms) == ref.bucket_open_ms(next_ms), ctx + " next");
            CHECK_CTX(made.bucket_open_ms(ms) == ref.bucket_open_ms(ms), ctx + " current");
        }
    }

    // Backwards and shuffled: the answers do not depend on the order. A
    // question that far back restarts the generator, so a dozen of them.
    const std::vector<std::int64_t> some = every_nth(points, 12);
    {
        const std::vector<std::int64_t> back(some.rbegin(), some.rend());
        compare_in_order(ref, generated_partition(sc, days, partition), back, ctx + " descending");
    }
    {
        std::vector<std::int64_t> shuffled = some;
        std::uint64_t state = 0x2545F4914F6CDD1DULL + static_cast<std::uint64_t>(days);
        for (std::size_t i = shuffled.size(); i > 1; --i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            std::swap(shuffled[i - 1], shuffled[(state >> 33) % i]);
        }
        compare_in_order(ref, generated_partition(sc, days, partition), shuffled, ctx + " shuffled");
    }

    const std::size_t most = compare_feed(sc, days, stamps, partition, ctx);
    CHECK_CTX(most <= source::kMultiDayWindow, ctx + " holds a bounded window");
}

void test_equals_the_stored_table() {
    std::printf("test_equals_the_stored_table\n");
    std::vector<Scenario> scenarios;
    scenarios.push_back(crypto_daily("crypto-1d", civil(2012, 1, 1), 3300));
    scenarios.push_back(crypto_intraday("crypto-4h", civil(2020, 2, 10), 500));
    scenarios.push_back(fx_daily("oanda-1d", civil(2011, 1, 3), civil(2017, 6, 1)));
    scenarios.push_back(stock_daily("nyse-1d", civil(2012, 1, 3), civil(2018, 6, 1)));
    scenarios.push_back(stock_daily("nyse-1d-midyear", civil(2014, 7, 15), civil(2017, 3, 22)));
    scenarios.push_back(stock_intraday("nyse-60", civil(2018, 1, 2), civil(2019, 7, 1)));
    scenarios.push_back(index_daily("nse-1d", civil(2012, 1, 2), civil(2018, 6, 1)));
    scenarios.push_back(chart_with_aux("nyse-1d-aux"));
    scenarios.push_back(aux_around_chart("nyse-1d-aux-around"));
    for (const Scenario& sc : scenarios) {
        CHECK_CTX(sc.chart.size() > 100, sc.name + " has a chart");
        // The held sessions are the trading dates only on an exchange's
        // calendar; the 24x7 and OANDA tapes hold every date their rule counts.
        const bool exchange_tape = sc.tz != "UTC" && sc.session != "1700-1700";
        for (const int days : {2, 3, 5, 7, 10}) {
            run_scenario(sc, days, false, true);
            if (exchange_tape || days == 3) run_scenario(sc, days, true, true);
        }
    }
    // Two longer N, and a chart of one bar.
    run_scenario(scenarios[3], 8, true, true);
    run_scenario(scenarios[3], 13, true, true);
    run_scenario(crypto_daily("one-bar", civil(2020, 6, 15), 1), 3, false, true);
}

// ─── Where the rule yields no partition ──────────────────────────────────────

void test_no_partition_where_the_table_was_empty() {
    std::printf("test_no_partition_where_the_table_was_empty\n");
    // One day is not an N-day request.
    {
        const Scenario sc = crypto_daily("one-day", civil(2020, 1, 1), 400);
        CHECK(reference_of(sc, 1, false).empty());
        CHECK(make_partition(sc, 1, false) == nullptr);
    }
    // Nothing held.
    {
        const Scenario sc = crypto_daily("empty", civil(2020, 1, 1), 0);
        CHECK(reference_of(sc, 3, false).empty());
        CHECK(make_partition(sc, 3, false) == nullptr);
    }
    // A session that does not parse, and a chart whose first bar is a
    // Saturday on a weekday rule (no period opens at or before it): whatever
    // the stored table gave, the generated partition gives the same.
    {
        Scenario sc = crypto_daily("no-session", civil(2020, 1, 1), 400);
        sc.session = "not a session";
        CHECK(reference_of(sc, 3, false).empty() == (make_partition(sc, 3, false) == nullptr));
    }
    {
        Scenario sc;
        sc.name = "saturday-first";
        sc.tz = "UTC";
        sc.session = "0930-1600";
        sc.input_tf = "1D";
        sc.daily_chart = true;
        const std::int64_t saturday = civil(2011, 1, 1);  // 2011-01-01 was a Saturday
        CHECK(weekday_of(saturday) == 6);
        for (int i = 0; i < 20; ++i)
            sc.chart.push_back(bar_at((saturday + i) * kDay + 9 * kHour + 30 * kMinute,
                                      static_cast<std::size_t>(i)));
        for (const bool exchange : {false, true}) {
            const bool stored = !reference_of(sc, 3, exchange).empty();
            const bool made = make_partition(sc, 3, exchange) != nullptr;
            CHECK_CTX(stored == made, exchange ? "exchange" : "weekday");
        }
    }
}

// ─── The aggregator's side of the contract ───────────────────────────────────

void test_installs_replace_each_other() {
    std::printf("test_installs_replace_each_other\n");
    const Scenario sc = crypto_daily("replace", civil(2015, 1, 1), 800);
    const std::vector<std::int64_t> stamps = reference_of(sc, 3, false);
    const Partition partition = make_partition(sc, 3, false);
    CHECK(!stamps.empty());
    CHECK(partition != nullptr);
    if (stamps.empty() || !partition) return;

    TimeframeAggregator agg = make_aggregator(sc, 3);
    CHECK(!agg.has_native_periods());
    CHECK(agg.native_period_stamps_held() == 0);
    agg.set_native_periods(stamps, stamps, CalendarPeriod::DAY);
    CHECK(agg.native_period_stamps_held() == 2 * stamps.size());
    CHECK(agg.set_native_period_source(partition));
    CHECK(agg.has_native_periods());
    CHECK(agg.native_period_stamps_held() <= source::kMultiDayWindow);
    const TimeframeAggregator ref = stored_partition(sc, 3, stamps);
    const std::int64_t last_bound =
        session_period_close_ms(stamps.back(), sc.tz, sc.session, CalendarPeriod::DAY);
    for (const std::int64_t p : every_nth(probe_points(sc, stamps, last_bound), 400))
        CHECK(agg.bucket_open_ms(p) == ref.bucket_open_ms(p));
    agg.set_native_periods(stamps, stamps, CalendarPeriod::DAY);
    CHECK(agg.native_period_stamps_held() == 2 * stamps.size());

    // Only a CALENDAR aggregator on DAY takes a source; a null source and a
    // refused install leave no partition behind.
    TimeframeAggregator ratio("60", "15", "UTC", "24x7");
    CHECK(!ratio.set_native_period_source(partition));
    CHECK(!ratio.has_native_periods());
    TimeframeAggregator weekly("W", "1D", "UTC", "24x7");
    CHECK(!weekly.set_native_period_source(partition));
    CHECK(!weekly.has_native_periods());
    CHECK(!agg.set_native_period_source(nullptr));
    CHECK(!agg.has_native_periods());
    CHECK(agg.native_period_stamps_held() == 0);
}

// ─── Retained state: flat in the days ────────────────────────────────────────

void test_state_does_not_grow_with_days() {
    std::printf("test_state_does_not_grow_with_days\n");
    const Scenario shorter = crypto_daily("two-years", civil(2010, 1, 1), 2 * 365);
    const Scenario longer = crypto_daily("forty-years", civil(1990, 1, 1), 40 * 365);
    for (const int days : {2, 3, 5, 7, 10}) {
        const std::string ctx = std::to_string(days) + "D";
        const std::vector<std::int64_t> stamps_short = reference_of(shorter, days, false);
        const std::vector<std::int64_t> stamps_long = reference_of(longer, days, false);
        const Partition partition_short = make_partition(shorter, days, false);
        const Partition partition_long = make_partition(longer, days, false);
        CHECK_CTX(!stamps_short.empty() && !stamps_long.empty(), ctx);
        CHECK_CTX(partition_short != nullptr && partition_long != nullptr, ctx + " plans");
        if (!partition_short || !partition_long) continue;
        const std::size_t most_short =
            compare_feed(shorter, days, stamps_short, partition_short, ctx + " short");
        const std::size_t most_long =
            compare_feed(longer, days, stamps_long, partition_long, ctx + " long");
        CHECK_CTX(most_short > 0 && most_long > 0, ctx + " generated");
        CHECK_CTX(most_short <= source::kMultiDayWindow, ctx + " short window");
        CHECK_CTX(most_long <= source::kMultiDayWindow, ctx + " long window");
        CHECK_CTX(most_long <= most_short, ctx + " does not grow with days");
        // The control: the stored form does grow with them.
        const std::size_t stored_short =
            stored_partition(shorter, days, stamps_short).native_period_stamps_held();
        const std::size_t stored_long =
            stored_partition(longer, days, stamps_long).native_period_stamps_held();
        CHECK_CTX(stored_long > source::kMultiDayWindow, ctx + " stored form is larger");
        CHECK_CTX(stored_long > 5 * stored_short, ctx + " stored form grows with days");
    }
}

// ─── The nominal calendar, however late the run asks ─────────────────────────

// The stored table was built at prepare, before the run's bar loop installs
// the chart's native day partition. The generator is asked during that loop,
// and must read the calendar the table read.
void test_generation_ignores_the_chart_partition() {
    std::printf("test_generation_ignores_the_chart_partition\n");
    const Scenario sc = stock_intraday("nyse-60-partition", civil(2018, 1, 2), civil(2019, 1, 2));
    // A native daily feed on the chart's own clock whose stamps are not the
    // nominal day stamps (09:31, not 09:30).
    std::vector<std::int64_t> native;
    for (const std::int64_t d : weekdays(civil(2018, 1, 2), civil(2019, 1, 2), us_closed))
        native.push_back(at_local(sc.tz, d, 9, 31));
    NativeDayPartition chart_partition;
    CHECK(build_native_day_partition(chart_partition, sc.tz, sc.session, native, sc.chart.data(),
                                     static_cast<int>(sc.chart.size())));
    // The control: the partition does move the calendar the generator reads.
    const std::int64_t second_bar = sc.chart[1].timestamp;
    const std::int64_t nominal_open =
        session_period_open_ms(second_bar, sc.tz, sc.session, CalendarPeriod::DAY);
    std::int64_t partitioned_open = 0;
    {
        NativeDayPartitionScope scope(&chart_partition);
        partitioned_open = session_period_open_ms(second_bar, sc.tz, sc.session, CalendarPeriod::DAY);
    }
    CHECK(partitioned_open != nominal_open);

    for (const int days : {3, 8}) {
        const std::string ctx = sc.name + " " + std::to_string(days) + "D";
        const std::vector<std::int64_t> stamps = reference_of(sc, days, true);
        CHECK_CTX(!stamps.empty(), ctx + " has a reference partition");
        if (stamps.empty()) continue;
        const TimeframeAggregator ref = stored_partition(sc, days, stamps);
        const std::int64_t last_bound =
            session_period_close_ms(stamps.back(), sc.tz, sc.session, CalendarPeriod::DAY);
        std::vector<std::int64_t> inside;
        for (const std::int64_t p : every_nth(probe_points(sc, stamps, last_bound), 600))
            if (p >= stamps.front() && p < last_bound) inside.push_back(p);
        CHECK_CTX(inside.size() > 50, ctx + " has probes");
        std::vector<std::int64_t> expected;
        for (const std::int64_t p : inside) expected.push_back(ref.bucket_open_ms(p));

        // One partition planned before the loop, one planned inside it; both
        // are asked inside it.
        const Partition before = make_partition(sc, days, true);
        Partition during;
        {
            NativeDayPartitionScope scope(&chart_partition);
            during = make_partition(sc, days, true);
        }
        CHECK_CTX(before != nullptr && during != nullptr, ctx + " plans");
        if (!before || !during) continue;
        const TimeframeAggregator planned_before = generated_partition(sc, days, before);
        const TimeframeAggregator planned_during = generated_partition(sc, days, during);
        NativeDayPartitionScope scope(&chart_partition);
        for (std::size_t i = 0; i < inside.size(); ++i) {
            CHECK_CTX(planned_before.bucket_open_ms(inside[i]) == expected[i],
                      ctx + " planned before at " + std::to_string(inside[i]));
            CHECK_CTX(planned_during.bucket_open_ms(inside[i]) == expected[i],
                      ctx + " planned during at " + std::to_string(inside[i]));
        }
    }
}

// ─── The host's own run ──────────────────────────────────────────────────────

constexpr int kSites = 5;
const char* const kTimeframes[kSites] = {"2D", "3D", "5D", "7D", "10D"};
const int kDays[kSites] = {2, 3, 5, 7, 10};

// Registers one N-day request.security site per N, as the generated
// constructor does, and records what each site's aggregator answers for the
// chart bar being calculated.
class MultiDayProbe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t time;
        std::int64_t open[kSites];
    };
    std::vector<Row> rows;
    std::size_t most_held[kSites] = {};
    bool installed[kSites] = {};

    MultiDayProbe() { attach_pine_execution_adapter(); }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (int i = 0; i < kSites; ++i)
            register_security_eval(i, kTimeframes[i], input_tf_, true, false);
    }

    void evaluate_security(int, const Bar&, bool) override {}

    void on_source_bar(const Bar&) override {
        Row row{current_bar_.timestamp, {}};
        for (int i = 0; i < kSites && i < static_cast<int>(security_eval_states_.size()); ++i) {
            const TimeframeAggregator& agg =
                security_eval_states_[static_cast<std::size_t>(i)].aggregator;
            row.open[i] = agg.bucket_open_ms(current_bar_.timestamp);
            installed[i] = agg.has_native_periods();
            most_held[i] = std::max(most_held[i], agg.native_period_stamps_held());
        }
        rows.push_back(row);
    }
};

void test_host_installs_the_generated_partition() {
    std::printf("test_host_installs_the_generated_partition\n");
    const Scenario sc = crypto_daily("host-crypto", civil(2012, 1, 1), 3300);
    MultiDayProbe probe;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    strategy_set_syminfo_type(handle, "crypto");
    probe.run(sc.chart.data(), static_cast<int>(sc.chart.size()), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.rows.size() == sc.chart.size());
    if (probe.rows.size() != sc.chart.size()) return;

    for (int site = 0; site < kSites; ++site) {
        const int days = kDays[site];
        const std::string ctx = std::string(kTimeframes[site]) + " site";
        CHECK_CTX(probe.installed[site], ctx + " is installed");
        CHECK_CTX(probe.most_held[site] > 0, ctx + " holds opens");
        CHECK_CTX(probe.most_held[site] <= source::kMultiDayWindow, ctx + " holds a bounded window");
        const std::vector<std::int64_t> stamps = reference_of(sc, days, false);
        CHECK_CTX(!stamps.empty(), ctx + " has a reference partition");
        for (const auto& row : probe.rows) {
            // The closed form of the rule on a 24x7 tape: N days at a time
            // from the year's first, the year's last period cut at its end.
            const std::int64_t day = row.time / kDay;
            const std::int64_t year_start =
                civil(static_cast<int>(native_calendar::native_civil_date(day).year), 1, 1);
            const std::int64_t closed_form = (year_start + ((day - year_start) / days) * days) * kDay;
            const auto after = std::upper_bound(stamps.begin(), stamps.end(), row.time);
            const std::int64_t stored = after == stamps.begin() ? -1 : *(after - 1);
            CHECK_CTX(row.open[site] == closed_form, ctx + " closed form at " + std::to_string(row.time));
            CHECK_CTX(row.open[site] == stored, ctx + " stored table at " + std::to_string(row.time));
        }
    }
}

}  // namespace

int main() {
    std::printf("[l19k] N-day request.security partition, generated and bounded\n");
    test_equals_the_stored_table();
    test_no_partition_where_the_table_was_empty();
    test_installs_replace_each_other();
    test_state_does_not_grow_with_days();
    test_generation_ignores_the_chart_partition();
    test_host_installs_the_generated_partition();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
