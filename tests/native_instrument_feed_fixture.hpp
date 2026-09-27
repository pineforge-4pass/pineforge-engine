// Shared fixture of the instrument-feed test units (the kernel unit and its
// adapter twin): synthetic charts and synthetic feeds of other instruments,
// shaped after the merge cases XSYM-DESIGN pinned against TradingView
// (XSYM-DESIGN report section 3.3) but generated here, value by value, from
// formulas -- no TradingView bar enters the repository (corpus/LEGAL.md).
// Nothing here names a source layer, so the kernel unit survives a
// kernel-only build.
#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/native_calendar.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace instrument_fixture {
using namespace pineforge;

inline int checks = 0;
inline int failures = 0;
inline const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++::instrument_fixture::checks;                                        \
        if (!(expression)) {                                                   \
            ++::instrument_fixture::failures;                                  \
            std::printf("FAIL [%s] line %d: %s\n", ::instrument_fixture::scenario, \
                        __LINE__, #expression);                                \
        }                                                                      \
    } while (false)

inline bool same(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return std::abs(a - b) <= 1e-9;
}

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
inline std::int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kQuarter = 15 * kMinute;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

// Monday 2025-04-07 00:00 UTC. New York is on daylight time (UTC-4) for the
// whole fixture, so its 09:30-16:00 session is 13:30Z-20:00Z.
inline std::int64_t monday() { return utc_ms(2025, 4, 7); }

// Weekday of a UTC day start: 0 Monday .. 6 Sunday (1970-01-01 was a Thursday).
inline int weekday(std::int64_t day_ms) {
    const std::int64_t days = day_ms / kDay;
    return static_cast<int>(((days % 7) + 7 + 3) % 7);
}

// A deterministic bar: every field differs bar to bar and feed to feed, so a
// read that took the wrong bar cannot equal the right one.
inline Bar make_bar(std::int64_t open_ms, int k, double base, double volume = 1.0) {
    Bar bar{};
    bar.open = base + 0.125 * k;
    bar.high = bar.open + 0.5 + 0.01 * (k % 7);
    bar.low = bar.open - 0.4 - 0.01 * (k % 5);
    bar.close = bar.open + 0.05 * ((k % 3) - 1);
    bar.volume = volume;
    bar.timestamp = open_ms;
    return bar;
}

// ---- the chart side ------------------------------------------------------

// NYSE-like 15-minute bars, 13:30Z..19:45Z (26 a day), on each given day.
inline std::vector<Bar> nyse_quarter_chart(const std::vector<std::int64_t>& days) {
    std::vector<Bar> bars;
    int k = 0;
    for (const std::int64_t day : days) {
        for (int slot = 0; slot < 26; ++slot, ++k) {
            bars.push_back(make_bar(day + 13 * kHour + 30 * kMinute + slot * kQuarter, k, 12.0,
                                    1000.0 + k));
        }
    }
    return bars;
}

// NYSE-like daily bars stamped at the session open, 13:30Z.
inline std::vector<Bar> nyse_daily_chart(const std::vector<std::int64_t>& days) {
    std::vector<Bar> bars;
    int k = 0;
    for (const std::int64_t day : days) {
        bars.push_back(make_bar(day + 13 * kHour + 30 * kMinute, k, 200.0, 5000.0 + k));
        ++k;
    }
    return bars;
}

// A 24x7 daily chart (crypto-like): n bars from `first_day`, 00:00Z each.
inline std::vector<Bar> continuous_daily_chart(std::int64_t first_day, int n) {
    std::vector<Bar> bars;
    for (int k = 0; k < n; ++k) bars.push_back(make_bar(first_day + k * kDay, k, 80000.0, 9.0));
    return bars;
}

// A 24x7 quarter-hour chart (a continuous instrument, e.g. spot metal).
inline std::vector<Bar> continuous_quarter_chart(std::int64_t first_ms, int n) {
    std::vector<Bar> bars;
    for (int k = 0; k < n; ++k) bars.push_back(make_bar(first_ms + k * kQuarter, k, 3000.0, 2.0));
    return bars;
}

inline std::vector<std::int64_t> weekdays_from(std::int64_t first_day, int n) {
    std::vector<std::int64_t> days;
    for (std::int64_t day = first_day; static_cast<int>(days.size()) < n; day += kDay) {
        if (weekday(day) < 5) days.push_back(day);
    }
    return days;
}

inline NativeRunSpec nyse_spec(const char* key, const char* tf) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = tf;
    spec.script_tf = tf;
    spec.ticker = "F";
    spec.tickerid = "TEST:F";
    spec.type = "stock";
    spec.currency = "USD";
    spec.basecurrency = "";
    spec.description = "instrument feed fixture chart";
    spec.volumetype = "base";
    spec.timezone = "America/New_York";
    spec.session = "0930-1600";
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

inline NativeRunSpec continuous_spec(const char* key, const char* tf) {
    NativeRunSpec spec = nyse_spec(key, tf);
    spec.ticker = "BTC";
    spec.tickerid = "TEST:BTC";
    spec.type = "crypto";
    spec.timezone = "UTC";
    spec.session = "24x7";
    return spec;
}

// ---- the other instruments ------------------------------------------------

// A feed under construction: bars and their closes side by side.
struct FeedBuilder {
    NativeInstrumentFeed feed;
    int k = 0;
    double base = 100.0;
    FeedBuilder(const char* instrument, const char* tf, double price) {
        feed.instrument = instrument;
        feed.tf = tf;
        base = price;
    }
    void add(std::int64_t open_ms, std::int64_t close_ms) {
        // An index publishes no volume: NaN, which the feed admits.
        feed.bars.push_back(make_bar(open_ms, k++, base, std::nan("")));
        feed.close_ms.push_back(close_ms);
    }
};

// DXY-like: a 23-hour 15-minute instrument. It trades 22:00Z to 21:00Z the
// next day, Sunday 22:00Z to Friday 21:00Z, so it has bars at hours no
// NYSE-like chart has. Bars from `first_ms` (inclusive) to `end_ms`.
inline NativeInstrumentFeed dxy_like_quarter_feed(std::int64_t first_ms, std::int64_t end_ms) {
    FeedBuilder b("SYN:DXY", "15", 103.0);
    for (std::int64_t t = first_ms; t < end_ms; t += kQuarter) {
        const std::int64_t day = t - ((t % kDay) + kDay) % kDay;
        const std::int64_t tod = t - day;
        const int wd = weekday(day);
        if (tod >= 21 * kHour && tod < 22 * kHour) continue;      // the daily break
        if (wd == 5) continue;                                     // Saturday
        if (wd == 4 && tod >= 21 * kHour) continue;                // Friday after the close
        if (wd == 6 && tod < 22 * kHour) continue;                 // Sunday before the open
        b.add(t, t + kQuarter);
    }
    return b.feed;
}

// US10Y-like: a daily bar that opens 23:00Z the evening before its trade date
// and closes at 21:30Z on it -- after the NYSE-like chart's 20:00Z close.
inline NativeInstrumentFeed us10y_like_daily_feed(const std::vector<std::int64_t>& trade_days) {
    FeedBuilder b("SYN:US10Y", "D", 4.2);
    for (const std::int64_t day : trade_days) b.add(day - kHour, day + 21 * kHour + 30 * kMinute);
    return b.feed;
}

// A weekday-only daily bar: opens 22:00Z the evening before, closes 21:00Z.
inline NativeInstrumentFeed weekday_daily_feed(const std::vector<std::int64_t>& trade_days) {
    FeedBuilder b("SYN:DXY", "D", 103.5);
    for (const std::int64_t day : trade_days) b.add(day - 2 * kHour, day + 21 * kHour);
    return b.feed;
}

// VIX-like 240: four bars a trade date, the second clipped by a session
// break -- 07:15Z (to 11:15Z), 11:15Z (to 13:26Z), 13:30Z (to 17:30Z) and
// 17:30Z (to 20:15Z). Its closes are the feed's own; no four-hour grid
// predicts 13:26Z.
inline NativeInstrumentFeed vix_like_240_feed(const std::vector<std::int64_t>& trade_days) {
    FeedBuilder b("SYN:VIX", "240", 20.0);
    for (const std::int64_t day : trade_days) {
        b.add(day + 7 * kHour + 15 * kMinute, day + 11 * kHour + 15 * kMinute);
        b.add(day + 11 * kHour + 15 * kMinute, day + 13 * kHour + 26 * kMinute);
        b.add(day + 13 * kHour + 30 * kMinute, day + 17 * kHour + 30 * kMinute);
        b.add(day + 17 * kHour + 30 * kMinute, day + 20 * kHour + 15 * kMinute);
    }
    return b.feed;
}

// CBOE-VIX-like 15: an instrument that trades only 13:30Z..20:15Z, so a
// 24-hour chart holds hours before its first bar of the day.
inline NativeInstrumentFeed rth_only_quarter_feed(const std::vector<std::int64_t>& trade_days) {
    FeedBuilder b("SYN:CBOEVIX", "15", 21.0);
    for (const std::int64_t day : trade_days) {
        for (std::int64_t t = day + 13 * kHour + 30 * kMinute; t < day + 20 * kHour + 15 * kMinute;
             t += kQuarter) {
            b.add(t, t + kQuarter);
        }
    }
    return b.feed;
}

inline NativeTimeframeSubscription instrument_series(const NativeInstrumentFeed& feed,
                                                     bool lookahead = false, bool gaps = false) {
    NativeTimeframeSubscription subscription;
    subscription.tf = feed.tf;
    subscription.source = NativeSeriesSource::InstrumentFeed;
    subscription.instrument = feed.instrument;
    subscription.lookahead = lookahead;
    subscription.gaps = gaps;
    return subscription;
}

// ---- the merge rule, written out independently of the kernel --------------
//
// For chart bar j with open O_j and last traded close C_j, the feed index a
// series exposes after that bar's deliveries: lookahead off, the last feed bar
// whose close <= C_j; lookahead on, the last whose open <= O_j; -1 when none.
// `gaps` then blanks every bar whose exposed index did not move.
struct Exposure {
    std::int64_t index = -1;  // the feed bar the chart bar sees; -1 = none
    bool fresh = false;       // the chart bar is the first to see it
};

inline std::vector<Exposure> expected_exposure(const NativeInstrumentFeed& feed,
                                               const std::vector<std::int64_t>& chart_open,
                                               const std::vector<std::int64_t>& chart_close,
                                               bool lookahead) {
    std::vector<Exposure> out;
    std::int64_t previous = -1;
    for (std::size_t j = 0; j < chart_open.size(); ++j) {
        std::int64_t seen = -1;
        for (std::size_t i = 0; i < feed.bars.size(); ++i) {
            const bool visible = lookahead ? feed.bars[i].timestamp <= chart_open[j]
                                           : feed.close_ms[i] <= chart_close[j];
            if (visible) seen = static_cast<std::int64_t>(i);
        }
        out.push_back({seen, seen >= 0 && seen != previous});
        previous = seen;
    }
    return out;
}

// ---- a bare host recording what the kernel hands it -----------------------

struct Delivery {
    std::size_t subscription = 0;
    Bar bar{};
    NativeTimeframeBarContext context{};
    int chart_bars_before = 0;
    // The feed index of this bar: the host counts deliveries per series,
    // which the delivery rule (every bar, once, in order) makes exact.
    std::int64_t feed_index = -1;
};

struct ChartRead {
    std::int64_t chart_open = 0;
    std::vector<std::optional<Bar>> visible;  // native_series_bar(i)
    std::vector<std::int64_t> index;          // the host's own count - 1, or -1
    std::vector<std::int64_t> previous;       // the delivery before it: a [1] read
    std::vector<int> delivered_on_this_bar;
};

class InstrumentHost : public NativeStrategyHost {
public:
    std::size_t series = 0;
    std::vector<Delivery> deliveries;
    std::vector<ChartRead> reads;
    std::vector<std::int64_t> count;       // deliveries per series
    std::vector<int> since_bar;            // deliveries per series since the last bar
    std::vector<std::string> log;
    int bars_seen = 0;

    explicit InstrumentHost(std::size_t n = 1) : series(n), count(n, 0), since_bar(n, 0) {}

    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        log.push_back("input:" + std::to_string(context.input_index) + "@"
                      + std::to_string(bar.timestamp));
    }

    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        Delivery delivery;
        delivery.subscription = context.subscription;
        delivery.bar = bar;
        delivery.context = context;
        delivery.chart_bars_before = bars_seen;
        if (context.subscription < series) {
            delivery.feed_index = count[context.subscription]++;
            ++since_bar[context.subscription];
        }
        deliveries.push_back(delivery);
        log.push_back("series:" + std::to_string(context.subscription) + "@"
                      + std::to_string(bar.timestamp));
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ChartRead read;
        read.chart_open = bar.timestamp;
        for (std::size_t i = 0; i < series; ++i) {
            read.visible.push_back(native_series_bar(i));
            read.index.push_back(count[i] - 1);
            read.delivered_on_this_bar.push_back(since_bar[i]);
            since_bar[i] = 0;
            std::int64_t prev = -1;
            if (count[i] >= 2) {
                // The delivery before the latest one of this series.
                std::int64_t seen = 0;
                for (auto it = deliveries.rbegin(); it != deliveries.rend(); ++it) {
                    if (it->subscription != i) continue;
                    if (++seen == 2) {
                        prev = it->feed_index;
                        break;
                    }
                }
            }
            read.previous.push_back(prev);
        }
        reads.push_back(std::move(read));
        ++bars_seen;
        log.push_back("bar@" + std::to_string(bar.timestamp));
    }
};

inline bool bars_equal(const Bar& got, const Bar& want) {
    return got.timestamp == want.timestamp && same(got.open, want.open)
        && same(got.high, want.high) && same(got.low, want.low)
        && same(got.close, want.close) && same(got.volume, want.volume);
}

inline bool run_batch(NativeStrategyHost& host, const NativeRunSpec& spec,
                      const std::vector<Bar>& chart) {
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) {
        std::printf("  configure refused: error %d field %d\n",
                    static_cast<int>(setup.validation.error),
                    static_cast<int>(setup.validation.field));
        return false;
    }
    host.run(chart.data(), static_cast<int>(chart.size()), spec.input_tf, spec.script_tf, false,
             4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    return host.last_error().empty();
}

// The chart's own intervals, from the run's calendar: open and last traded
// close of every chart bar, which is what the merge rule reads.
inline void chart_intervals(const NativeRunSpec& spec, const std::vector<Bar>& chart,
                            std::vector<std::int64_t>& open, std::vector<std::int64_t>& close) {
    const auto calendar = native_calendar::parse_session(spec.session, spec.timezone);
    const auto tf = native_calendar::parse_timeframe(spec.input_tf);
    open.clear();
    close.clear();
    if (!calendar || !tf) return;
    for (const Bar& bar : chart) {
        const auto interval = native_calendar::interval_containing(*calendar, *tf, bar.timestamp);
        open.push_back(interval ? interval->open_ms : 0);
        close.push_back(interval ? interval->last_traded_close_ms : 0);
    }
}

}  // namespace instrument_fixture
