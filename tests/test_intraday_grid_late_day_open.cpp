// An intraday grid starts a late-opening day at that day's own open, dated by
// the installed daily feed (R5 lane W15-KERNEL-CAL, item B).
//
// The intraday higher-timeframe grid is anchored at the symbol's day stamp:
// 09:15 IST on NSE, so a "60" series buckets at 09:15, 10:15, ... 15:15.
// NSE's Muhurat session of Tue 2025-10-21 traded 13:45 .. 14:45 IST only, and
// TradingView laid that day's grid from the session's own open: one "60" bar
// stamped 13:45 (o 25904, h 25934.35, l 25826.3, c 25833.3), completed on the
// 14:30 chart bar, and its daily bar is stamped 13:45 too (lab tv
// w14-htf-60-nifty15, w14-htf-d-nifty15; NSE:NIFTY 15, 2025-04-01 ..
// 2026-05-01). The day-stamp grid made two bars there (13:15, 14:15), so the
// "60" bar_index ran one ahead for the rest of the feed and an EMA200 on it
// drifted off TradingView's (w14-htf-60ema-nifty15). The chart's own clock
// reads agree: time("60") is 13:45 on all four bars, time_close("60") 14:45,
// and timeframe.change("60") is true on the 13:45 bar alone (lab tv
// w15-time60-nifty15, the same chart).
//
// No session template holds that session; the venue's own daily bars do. A
// daily stamp later than its session-day's day stamp, with no input bar of
// that day before it, is where the day opened (native_late_day_opens), and:
//   * a request evaluator's RATIO aggregator lays that day's grid from it
//     (TimeframeAggregator::set_native_day_opens, installed from the daily
//     feed by the feed store -- a bare host's "D" authoritative bars too);
//   * the chart's native day partition carries the same opens
//     (NativeDayPartition::late_opens) for the language runtime's grid
//     readers, session_intraday_bucket_open_ms and tf_change.
// Every other day keeps the day-stamp grid bit for bit, and no feed, no
// partition: no change at all.
//
// Kernel-only (timeframe.hpp, native_host.hpp). Synthetic prices on the real
// NSE clock (Asia/Kolkata, 0915-1530).

#include <pineforge/bar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "init";

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL [%s] line %d: %s\n", scenario, __LINE__, #expr); \
        }                                                                      \
    } while (0)

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kQuarter = 15 * kMinute;
const std::string kTz = "Asia/Kolkata";
const std::string kSession = "0915-1530";

// Unix ms of an IST civil date-time (fixed UTC+05:30).
std::int64_t ist_ms(int y, int m, int d, int h, int mi) {
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000
        - (5 * 3600 + 30 * 60) * 1000LL;
}

struct Session { int y, m, d, h, mi, bars; };

std::vector<Bar> quarter_hours(const std::vector<Session>& sessions) {
    std::vector<Bar> bars;
    int k = 0;
    for (const Session& s : sessions) {
        const std::int64_t open = ist_ms(s.y, s.m, s.d, s.h, s.mi);
        for (int i = 0; i < s.bars; ++i, ++k) {
            Bar bar{};
            bar.timestamp = open + i * kQuarter;
            bar.open = 25000.0 + 3.0 * k;
            bar.high = bar.open + 5.0 + (k % 4);
            bar.low = bar.open - 4.0 - (k % 3);
            bar.close = bar.open + 1.0;
            bar.volume = 1.0 + k;
            bars.push_back(bar);
        }
    }
    return bars;
}

// Mon 10-20 (a full session), the Muhurat Tue 10-21 (13:45 .. 14:30, four
// bars), Thu 10-23 (a full session).
const std::vector<Session> kMuhurat = {
    {2025, 10, 20, 9, 15, 25}, {2025, 10, 21, 13, 45, 4}, {2025, 10, 23, 9, 15, 25}};
constexpr std::size_t kMuhuratFirst = 25;   // the 13:45 bar's index

// The venue's daily stamps of those three days: the 09:15 day stamp, the
// Muhurat session's own 13:45 open, the 09:15 day stamp.
std::vector<std::int64_t> muhurat_stamps() {
    return {ist_ms(2025, 10, 20, 9, 15), ist_ms(2025, 10, 21, 13, 45),
            ist_ms(2025, 10, 23, 9, 15)};
}

bool same(double a, double b) { return std::abs(a - b) <= 1e-9; }

Bar aggregate(const std::vector<Bar>& bars, std::size_t from, std::size_t to,
              std::int64_t label) {
    Bar out = bars[from];
    out.timestamp = label;
    for (std::size_t i = from + 1; i < to; ++i) {
        out.high = std::max(out.high, bars[i].high);
        out.low = std::min(out.low, bars[i].low);
        out.close = bars[i].close;
        out.volume += bars[i].volume;
    }
    return out;
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && same(a.open, b.open) && same(a.high, b.high)
        && same(a.low, b.low) && same(a.close, b.close) && same(a.volume, b.volume);
}

struct Completion {
    std::size_t at = 0;
    Bar bar{};
    int subs = 0;
};

std::vector<Completion> feed_all(TimeframeAggregator& agg, const std::vector<Bar>& bars) {
    std::vector<Completion> out;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const std::int64_t next = i + 1 < bars.size() ? bars[i + 1].timestamp : 0;
        const AggregatedBar ab = agg.feed(bars[i], next);
        if (ab.is_complete) out.push_back({i, ab.bar, ab.sub_bar_count});
    }
    return out;
}

// ---- 1. which stamps open their day late ----------------------------------

void test_late_day_opens() {
    scenario = "native_late_day_opens";
    const std::vector<Bar> bars = quarter_hours(kMuhurat);
    const std::vector<std::int64_t> stamps = muhurat_stamps();
    const std::vector<std::int64_t> late =
        native_late_day_opens(kTz, kSession, stamps, bars.data(), static_cast<int>(bars.size()));
    CHECK(late.size() == 1);
    CHECK(!late.empty() && late[0] == ist_ms(2025, 10, 21, 13, 45));

    // The input says the day already traded before the stamp: not an open.
    std::vector<Bar> traded = bars;
    Bar early = bars[kMuhuratFirst];
    early.timestamp = ist_ms(2025, 10, 21, 10, 0);
    traded.insert(traded.begin() + static_cast<std::ptrdiff_t>(kMuhuratFirst), early);
    CHECK(native_late_day_opens(kTz, kSession, stamps, traded.data(),
                                static_cast<int>(traded.size())).empty());

    // A venue that dates its daily bars at local midnight: every stamp lies
    // in the previous session-day, after that day's trading -- none opens a day.
    const std::vector<std::int64_t> midnight = {
        ist_ms(2025, 10, 20, 0, 0), ist_ms(2025, 10, 21, 0, 0), ist_ms(2025, 10, 23, 0, 0)};
    CHECK(native_late_day_opens(kTz, kSession, midnight, bars.data(),
                                static_cast<int>(bars.size())).empty());

    // No input to contradict it: the late stamp stands.
    CHECK(native_late_day_opens(kTz, kSession, stamps, nullptr, 0).size() == 1);

    // An evening session after the template's close (NSE's Muhurat of
    // 2024-11-01, 18:00 IST) is not a late open: the template knows neither
    // where it opens nor where it ends.
    CHECK(native_late_day_opens(kTz, kSession,
                                {ist_ms(2024, 10, 31, 9, 15), ist_ms(2024, 11, 1, 18, 0)},
                                nullptr, 0).empty());

    // Non-increasing stamps: nothing.
    const std::vector<std::int64_t> unsorted = {stamps[1], stamps[0]};
    CHECK(native_late_day_opens(kTz, kSession, unsorted, bars.data(),
                                static_cast<int>(bars.size())).empty());

    // A 24x7 UTC clock whose daily stamps sit at midnight: none is late.
    const std::vector<std::int64_t> utc_days = {
        ist_ms(2025, 10, 20, 5, 30), ist_ms(2025, 10, 21, 5, 30)};
    CHECK(native_late_day_opens("UTC", "24x7", utc_days, nullptr, 0).empty());
}

// ---- 2. a request evaluator's "60" grid -----------------------------------

void test_hourly_aggregator() {
    scenario = "60 aggregator";
    const std::vector<Bar> bars = quarter_hours(kMuhurat);

    // The day-stamp grid: 13:15 and 14:15 buckets on the Muhurat day.
    TimeframeAggregator nominal("60", "15", kTz, kSession);
    const std::vector<Completion> plain = feed_all(nominal, bars);
    std::vector<Completion> plain_muhurat;
    for (const Completion& c : plain) {
        if (c.at >= kMuhuratFirst && c.at < kMuhuratFirst + 4) plain_muhurat.push_back(c);
    }
    CHECK(plain_muhurat.size() == 2);
    if (plain_muhurat.size() == 2) {
        CHECK(plain_muhurat[0].bar.timestamp == ist_ms(2025, 10, 21, 13, 15));
        CHECK(plain_muhurat[1].bar.timestamp == ist_ms(2025, 10, 21, 14, 15));
    }

    // With the Muhurat open installed: one bucket, stamped 13:45, holding the
    // four bars, completed on the 14:30 bar.
    TimeframeAggregator hourly("60", "15", kTz, kSession);
    hourly.set_native_day_opens({ist_ms(2025, 10, 21, 13, 45)});
    CHECK(hourly.has_native_periods());
    const std::vector<Completion> anchored = feed_all(hourly, bars);
    std::vector<Completion> muhurat;
    for (const Completion& c : anchored) {
        if (c.at >= kMuhuratFirst && c.at < kMuhuratFirst + 4) muhurat.push_back(c);
    }
    CHECK(muhurat.size() == 1);
    if (muhurat.size() == 1) {
        CHECK(muhurat[0].at == kMuhuratFirst + 3);
        CHECK(muhurat[0].subs == 4);
        CHECK(same_bar(muhurat[0].bar, aggregate(bars, kMuhuratFirst, kMuhuratFirst + 4,
                                                 ist_ms(2025, 10, 21, 13, 45))));
    }
    // One bucket fewer over the feed, and every bucket of the regular days
    // exactly as before.
    CHECK(anchored.size() + 1 == plain.size());
    std::vector<Completion> plain_regular;
    std::vector<Completion> anchored_regular;
    for (const Completion& c : plain) {
        if (c.at < kMuhuratFirst || c.at >= kMuhuratFirst + 4) plain_regular.push_back(c);
    }
    for (const Completion& c : anchored) {
        if (c.at < kMuhuratFirst || c.at >= kMuhuratFirst + 4) anchored_regular.push_back(c);
    }
    CHECK(plain_regular.size() == anchored_regular.size());
    for (std::size_t k = 0; k < plain_regular.size() && k < anchored_regular.size(); ++k) {
        CHECK(plain_regular[k].at == anchored_regular[k].at);
        CHECK(same_bar(plain_regular[k].bar, anchored_regular[k].bar));
    }

    // The queries read the same grid.
    const std::int64_t t1430 = ist_ms(2025, 10, 21, 14, 30);
    CHECK(hourly.bucket_open_ms(t1430) == ist_ms(2025, 10, 21, 13, 45));
    CHECK(hourly.bar_label_ms(ist_ms(2025, 10, 21, 14, 15)) == ist_ms(2025, 10, 21, 13, 45));
    CHECK(nominal.bucket_open_ms(t1430) == ist_ms(2025, 10, 21, 14, 15));
    CHECK(!hourly.period_changes(ist_ms(2025, 10, 21, 14, 0), ist_ms(2025, 10, 21, 14, 15)));
    CHECK(hourly.period_changes(ist_ms(2025, 10, 20, 15, 15), ist_ms(2025, 10, 21, 13, 45)));
    CHECK(hourly.period_changes(t1430, ist_ms(2025, 10, 23, 9, 15)));
    CHECK(hourly.bucket_open_ms(ist_ms(2025, 10, 23, 10, 45)) == ist_ms(2025, 10, 23, 10, 15));
    CHECK(!nominal.period_changes(ist_ms(2025, 10, 21, 14, 0), ist_ms(2025, 10, 21, 14, 15)));

    // "240" keeps one bucket either way; its stamp moves to the open.
    TimeframeAggregator four("240", "15", kTz, kSession);
    four.set_native_day_opens({ist_ms(2025, 10, 21, 13, 45)});
    CHECK(four.bucket_open_ms(t1430) == ist_ms(2025, 10, 21, 13, 45));

    scenario = "75 aggregator";
    // A width that does not divide the day ("75", 1440 = 19 x 75 + 15): the
    // open is still a bucket boundary, the day's four bars one bucket that
    // maps its label onto itself, and every other day's bucket is the one
    // the day-stamp clock gives.
    TimeframeAggregator plain75("75", "15", kTz, kSession);
    TimeframeAggregator late75("75", "15", kTz, kSession);
    late75.set_native_day_opens({ist_ms(2025, 10, 21, 13, 45)});
    const std::vector<Completion> plain75_done = feed_all(plain75, bars);
    const std::vector<Completion> late75_done = feed_all(late75, bars);
    std::vector<Completion> late75_muhurat;
    for (const Completion& c : late75_done) {
        if (c.at >= kMuhuratFirst && c.at < kMuhuratFirst + 4) late75_muhurat.push_back(c);
    }
    CHECK(late75_muhurat.size() == 1);
    if (late75_muhurat.size() == 1) {
        CHECK(late75_muhurat[0].at == kMuhuratFirst + 3);
        CHECK(same_bar(late75_muhurat[0].bar,
                       aggregate(bars, kMuhuratFirst, kMuhuratFirst + 4,
                                 ist_ms(2025, 10, 21, 13, 45))));
    }
    CHECK(late75.bar_label_ms(ist_ms(2025, 10, 21, 13, 45)) == ist_ms(2025, 10, 21, 13, 45));
    CHECK(late75.bucket_open_ms(t1430) == ist_ms(2025, 10, 21, 13, 45));
    CHECK(!late75.period_changes(ist_ms(2025, 10, 21, 13, 45), t1430));
    std::size_t regular75 = 0;
    for (const Completion& c : plain75_done) {
        if (c.at >= kMuhuratFirst && c.at < kMuhuratFirst + 4) continue;
        bool found = false;
        for (const Completion& d : late75_done) {
            if (d.at == c.at && same_bar(d.bar, c.bar)) found = true;
        }
        regular75 += found ? 1 : 0;
        CHECK(found);
    }
    CHECK(regular75 > 0);

    scenario = "pre-open minute";
    // A finer feed that printed before the open (a 13:30 minute the daily
    // stamp does not know): the grid moves by less than one width, so the
    // keys stay monotonic across the open and the day is one bucket, never
    // a run of singletons.
    std::vector<Bar> minutes;
    for (int i = 0; i < 75; ++i) {
        Bar bar{};
        bar.timestamp = ist_ms(2025, 10, 21, 13, 30) + i * kMinute;
        bar.open = 100.0 + i;
        bar.high = bar.open + 1.0;
        bar.low = bar.open - 1.0;
        bar.close = bar.open + 0.5;
        bar.volume = 1.0;
        minutes.push_back(bar);
    }
    TimeframeAggregator minute_hours("60", "1", kTz, kSession);
    minute_hours.set_native_day_opens({ist_ms(2025, 10, 21, 13, 45)});
    std::vector<Completion> minute_done;
    for (std::size_t i = 0; i < minutes.size(); ++i) {
        const std::int64_t next = i + 1 < minutes.size() ? minutes[i + 1].timestamp
                                                          : ist_ms(2025, 10, 23, 9, 15);
        const AggregatedBar ab = minute_hours.feed(minutes[i], next);
        if (ab.is_complete) minute_done.push_back({i, ab.bar, ab.sub_bar_count});
    }
    CHECK(minute_done.size() == 1);

    // Calendar and count-only aggregators ignore the opens; unsorted opens
    // install nothing.
    TimeframeAggregator day("D", "15", kTz, kSession);
    day.set_native_day_opens({ist_ms(2025, 10, 21, 13, 45)});
    CHECK(!day.has_native_periods());
    TimeframeAggregator counted(4);
    counted.set_native_day_opens({ist_ms(2025, 10, 21, 13, 45)});
    CHECK(!counted.has_native_periods());
    TimeframeAggregator unsorted("60", "15", kTz, kSession);
    unsorted.set_native_day_opens({ist_ms(2025, 10, 23, 9, 15), ist_ms(2025, 10, 21, 13, 45)});
    CHECK(!unsorted.has_native_periods());
}

// ---- 3. the chart partition's grid readers ---------------------------------

void test_chart_partition_grid() {
    scenario = "chart partition grid";
    const std::vector<Bar> bars = quarter_hours(kMuhurat);
    NativeDayPartition partition;
    CHECK(build_native_day_partition(partition, kTz, kSession, muhurat_stamps(),
                                     bars.data(), static_cast<int>(bars.size())));
    CHECK(partition.late_opens.size() == 1);
    const std::int64_t t1400 = ist_ms(2025, 10, 21, 14, 0);
    const std::int64_t t1415 = ist_ms(2025, 10, 21, 14, 15);
    const std::int64_t t1430 = ist_ms(2025, 10, 21, 14, 30);
    // Without the partition: the day-stamp grid.
    CHECK(session_intraday_bucket_open_ms(t1430, 3600, kTz, kSession) == t1415);
    CHECK(tf_change(t1400, t1415, "60", kTz, kSession));
    {
        NativeDayPartitionScope scope(&partition);
        CHECK(session_intraday_bucket_open_ms(t1430, 3600, kTz, kSession)
              == ist_ms(2025, 10, 21, 13, 45));
        CHECK(session_intraday_bucket_open_ms(t1430, 4 * 3600, kTz, kSession)
              == ist_ms(2025, 10, 21, 13, 45));
        CHECK(!tf_change(t1400, t1415, "60", kTz, kSession));
        CHECK(tf_change(ist_ms(2025, 10, 20, 15, 15), ist_ms(2025, 10, 21, 13, 45), "60",
                        kTz, kSession));
        CHECK(session_intraday_bucket_open_ms(t1430, 75 * 60, kTz, kSession)
              == ist_ms(2025, 10, 21, 13, 45));
        CHECK(!tf_change(ist_ms(2025, 10, 21, 13, 45), t1430, "75", kTz, kSession));
        CHECK(tf_change(t1430, ist_ms(2025, 10, 23, 9, 15), "60", kTz, kSession));
        // A regular day and another symbol's clock are untouched.
        CHECK(session_intraday_bucket_open_ms(ist_ms(2025, 10, 23, 11, 0), 3600, kTz, kSession)
              == ist_ms(2025, 10, 23, 10, 15));
        CHECK(session_intraday_bucket_open_ms(t1430, 3600, "UTC", "24x7")
              == session_intraday_bucket_open_ms(t1430, 3600, "UTC", ""));
    }
    CHECK(session_intraday_bucket_open_ms(t1430, 3600, kTz, kSession) == t1415);

    // A partition whose stamps all sit at the day stamp has no late opens.
    NativeDayPartition regular;
    CHECK(build_native_day_partition(regular, kTz, kSession,
                                     {ist_ms(2025, 10, 20, 9, 15), ist_ms(2025, 10, 23, 9, 15)},
                                     bars.data(), static_cast<int>(bars.size())));
    CHECK(regular.late_opens.empty());
}

// ---- 4. a bare host's "60" series with the venue's daily bars --------------

class SeriesHost final : public NativeStrategyHost {
public:
    struct Delivery {
        std::size_t subscription = 0;
        Bar bar{};
        int bars_before = 0;
    };
    std::vector<Delivery> deliveries;
    int bars_seen = 0;
    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        deliveries.push_back({context.subscription, bar, bars_seen});
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bars_seen; }
};

NativeRunSpec nse_spec(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "NIFTY";
    spec.tickerid = "TEST:NIFTY";
    spec.type = "index";
    spec.currency = "INR";
    spec.basecurrency = "INR";
    spec.description = "late day open";
    spec.volumetype = "base";
    spec.timezone = kTz;
    spec.session = kSession;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.05;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

std::vector<SeriesHost::Delivery> hourly_on_muhurat(const SeriesHost& host) {
    std::vector<SeriesHost::Delivery> out;
    for (const auto& d : host.deliveries) {
        if (d.subscription != 0) continue;
        if (d.bar.timestamp >= ist_ms(2025, 10, 21, 0, 0)
            && d.bar.timestamp < ist_ms(2025, 10, 22, 0, 0)) {
            out.push_back(d);
        }
    }
    return out;
}

void test_bare_host_hourly_series() {
    scenario = "bare host 60 series";
    const std::vector<Bar> bars = quarter_hours(kMuhurat);
    std::vector<Bar> daily;
    for (std::int64_t stamp : muhurat_stamps()) {
        Bar bar{};
        bar.timestamp = stamp;
        bar.open = 25900.0;
        bar.high = 25950.0;
        bar.low = 25800.0;
        bar.close = 25850.0;
        bar.volume = 1000.0;
        daily.push_back(bar);
    }

    // Without the venue's daily bars: the day-stamp grid, two buckets.
    {
        NativeRunSpec spec = nse_spec("w15-late-open-plain");
        NativeTimeframeSubscription hourly;
        hourly.tf = "60";
        spec.subscriptions.push_back(hourly);
        SeriesHost host;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        CHECK(host.last_error().empty());
        const auto muhurat = hourly_on_muhurat(host);
        CHECK(muhurat.size() == 2);
    }

    // With them (a "D" series carrying authoritative bars): one bucket stamped
    // 13:45, the aggregate of the four bars, delivered on the 14:30 bar.
    NativeRunSpec spec = nse_spec("w15-late-open-daily");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    NativeTimeframeSubscription day;
    day.tf = "D";
    day.authoritative_bars = daily;
    spec.subscriptions.push_back(hourly);
    spec.subscriptions.push_back(day);
    SeriesHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    const auto muhurat = hourly_on_muhurat(host);
    CHECK(muhurat.size() == 1);
    if (muhurat.size() == 1) {
        CHECK(same_bar(muhurat[0].bar, aggregate(bars, kMuhuratFirst, kMuhuratFirst + 4,
                                                 ist_ms(2025, 10, 21, 13, 45))));
        CHECK(muhurat[0].bars_before == static_cast<int>(kMuhuratFirst + 3));
    }
    // The regular days' hourly buckets are the day-stamp grid's.
    std::size_t hourly_total = 0;
    for (const auto& d : host.deliveries) {
        if (d.subscription == 0) ++hourly_total;
    }
    // 10-20: 7 buckets (09:15 .. 15:15), the Muhurat day 1, 10-23: 7 (the
    // last completes on the feed's final bar at its session close).
    CHECK(hourly_total == 15);
}

}  // namespace

int main() {
    test_late_day_opens();
    test_hourly_aggregator();
    test_chart_partition_grid();
    test_bare_host_hourly_series();
    std::printf("intraday grid late day open: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
