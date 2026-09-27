// Instrument feeds of a bare native host (NativeRunSpec::instrument_feeds and
// NativeSeriesSource::InstrumentFeed, lane XSYM-D): another instrument's own
// bars, installed as data and handed to a declared series by interval and by
// nothing else. No source layer and no Pine expression: the host records what
// the kernel hands it, and every expectation is the merge rule written out in
// the fixture (expected_exposure), independently of the kernel.
//
// The cases are the ones XSYM-DESIGN pinned against TradingView (report
// section 3.3), rebuilt on synthetic bars:
//   1. a foreign session that differs from the chart's: a NYSE-like 15-minute
//      chart reads a DXY-like 23-hour feed; every feed bar is handed over, the
//      overnight ones on the next session's first bar, and the first chart bar
//      sees the 55th bar of the context (index 54), the report's own pin --
//      the same under a raw label partition, whose inputs carry no duration,
//      where a label outside the session reads the calendar's slot for it
//      (which traded nothing: its last traded close is its open);
//   2. a daily foreign bar that closes after the chart's close is seen one day
//      late (a NYSE-like daily chart and a US10Y-like daily feed);
//   3. a weekend carry under gaps = false, and na on the weekend under
//      gaps = true (a 24x7 daily chart and a weekday-only daily feed);
//   4. a finer foreign feed under a daily chart with lookahead = true and a
//      [1] read: a VIX-like 240 feed whose second bar closes at 13:26Z, a close
//      no grid predicts; the first chart bar sees index 2 and reads index 1;
//   5. history before the first input: bars days older than the first chart
//      bar are handed over on it, in order, before it is calculated;
//   6. the context start: a 24-hour chart reading an instrument that opens at
//      13:30Z sees nothing until the instrument's first bar has closed;
//   7. every named refusal of the spec;
//   8. an instrument series beside an input series, run twice on one host:
//      the input series is what it is alone, and the second run is the first;
//   9. streams refuse an installed instrument feed at stream_begin;
//  10. neutrality and identity: no feed, no digest change; a feed, a digest;
//      one spec, one continuation; another close, another continuation;
//  11. a named column rides with its bar: a host that counts deliveries reads
//      the column of the bar it was handed.

#include "native_instrument_feed_fixture.hpp"

#include <string>

namespace {
using namespace instrument_fixture;

// Every delivery of series `s` is the feed bar the host's count names, with
// the feed's own interval, on the chart bar the merge rule says, before that
// bar's calculation; and each chart bar reads the exposure the rule gives.
void check_against_rule(const InstrumentHost& host, std::size_t s,
                        const NativeInstrumentFeed& feed, const std::vector<Bar>& chart,
                        const std::vector<Exposure>& want, bool gaps) {
    CHECK(host.reads.size() == chart.size());
    CHECK(want.size() == chart.size());
    if (host.reads.size() != chart.size() || want.size() != chart.size()) return;
    // The host saw feed index i on chart bar j exactly when the rule says the
    // exposure first reached i at j: the deliveries of bar j are the feed
    // bars (want[j-1].index, want[j].index].
    std::int64_t handed = -1;
    std::size_t next_delivery = 0;
    std::vector<const Delivery*> mine;
    for (const auto& delivery : host.deliveries) {
        if (delivery.subscription == s) mine.push_back(&delivery);
    }
    for (std::size_t j = 0; j < chart.size(); ++j) {
        const std::int64_t until = want[j].index;
        const int expected_count = static_cast<int>(std::max<std::int64_t>(until - handed, 0));
        CHECK(host.reads[j].delivered_on_this_bar[s] == expected_count);
        for (std::int64_t i = handed + 1; i <= until; ++i) {
            CHECK(next_delivery < mine.size());
            if (next_delivery >= mine.size()) return;
            const Delivery& got = *mine[next_delivery++];
            CHECK(got.feed_index == i);
            CHECK(got.chart_bars_before == static_cast<int>(j));
            CHECK(bars_equal(got.bar, feed.bars[static_cast<std::size_t>(i)]));
            CHECK(got.context.delivered_at_ms == chart[j].timestamp);
            CHECK(got.context.completion == NativeCompletionKind::Confirmed);
            CHECK(got.context.interval.open_ms == feed.bars[static_cast<std::size_t>(i)].timestamp);
            CHECK(got.context.interval.eligible_open_ms
                  == feed.bars[static_cast<std::size_t>(i)].timestamp);
            CHECK(got.context.interval.last_traded_close_ms
                  == feed.close_ms[static_cast<std::size_t>(i)]);
            CHECK(got.context.interval.next_period_open_ms
                  == feed.close_ms[static_cast<std::size_t>(i)]);
        }
        if (until > handed) handed = until;
        const auto& visible = host.reads[j].visible[s];
        const bool shows = want[j].index >= 0 && (!gaps || want[j].fresh);
        CHECK(visible.has_value() == shows);
        if (visible && shows) {
            CHECK(bars_equal(*visible, feed.bars[static_cast<std::size_t>(want[j].index)]));
        }
        CHECK(host.reads[j].index[s] == want[j].index);
    }
    CHECK(next_delivery == mine.size());
}

// ---- 1. a foreign session that differs from the chart's -------------------

void test_foreign_session_differs() {
    scenario = "F-15-like chart <- DXY-like 23h feed";
    const std::vector<std::int64_t> days = {monday(), monday() + kDay};
    const std::vector<Bar> chart = nyse_quarter_chart(days);
    const NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + 2 * kDay);
    NativeRunSpec spec = nyse_spec("xsym-session", "15");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));

    // The chart calendar the merge reads: a 15-minute NYSE-like bar closes
    // 15 minutes after it opens, and the session opens at 13:30Z.
    std::vector<std::int64_t> open, close;
    chart_intervals(spec, chart, open, close);
    CHECK(!open.empty() && open.front() == monday() + 13 * kHour + 30 * kMinute);
    CHECK(!close.empty() && close.front() == monday() + 13 * kHour + 45 * kMinute);

    InstrumentHost host;
    if (!run_batch(host, spec, chart)) return;
    const auto want = expected_exposure(feed, open, close, false);
    check_against_rule(host, 0, feed, chart, want, false);

    // The report's pin, rebuilt: the context starts at 00:00Z, so the chart's
    // first bar (13:30Z) sees the context's bar 54 -- the bar that opened at
    // 13:30Z and closed with the chart bar -- and all 55 ride on that bar.
    CHECK(host.reads[0].index[0] == 54);
    CHECK(host.reads[0].delivered_on_this_bar[0] == 55);
    CHECK(host.reads[0].visible[0].has_value()
          && host.reads[0].visible[0]->timestamp == monday() + 13 * kHour + 30 * kMinute);
    // Monday's last chart bar (19:45Z) sees bar 79; Tuesday's first sees the
    // 67 bars the instrument traded overnight (20:00Z..20:45Z, then 22:00Z
    // Monday to 13:30Z Tuesday) handed over on it, and index 146.
    CHECK(host.reads[25].index[0] == 79);
    CHECK(host.reads[26].delivered_on_this_bar[0] == 67);
    CHECK(host.reads[26].index[0] == 146);
    // Nothing the feed carries past the last chart bar's close is handed over.
    CHECK(host.count[0] == want.back().index + 1);
    CHECK(static_cast<std::size_t>(host.count[0]) < feed.bars.size());
}

// A host whose provider labels its bars itself (the tolerant raw-label
// ingress a source host opts into) presents every input as a zero-length
// interval; the merge still reads the input's calendar interval, so the same
// chart and feed hand over the same bars on the same chart bars.
void test_raw_label_partition_reads_the_calendar() {
    scenario = "raw label partition";
    const std::vector<std::int64_t> days = {monday(), monday() + kDay};
    const std::vector<Bar> chart = nyse_quarter_chart(days);
    const NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + 2 * kDay);
    for (const bool lookahead : {false, true}) {
        NativeRunSpec canonical = nyse_spec("xsym-labels-canonical", "15");
        canonical.instrument_feeds.push_back(feed);
        canonical.subscriptions.push_back(instrument_series(feed, lookahead));
        NativeRunSpec raw = canonical;
        raw.identity.session_key = "xsym-labels-raw";
        raw.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        raw.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
        InstrumentHost strict;
        InstrumentHost tolerant;
        if (!run_batch(strict, canonical, chart) || !run_batch(tolerant, raw, chart)) return;
        std::vector<std::int64_t> open, close;
        chart_intervals(canonical, chart, open, close);
        check_against_rule(tolerant, 0, feed, chart,
                           expected_exposure(feed, open, close, lookahead), false);
        CHECK(tolerant.reads.size() == strict.reads.size());
        for (std::size_t j = 0; j < tolerant.reads.size() && j < strict.reads.size(); ++j) {
            CHECK(tolerant.reads[j].index[0] == strict.reads[j].index[0]);
        }
    }
}

// A raw label outside the session -- a pre-market print under 09:30-16:00 --
// sits in the calendar's slot for it, which traded nothing: its last traded
// close is its open. The rule reads that slot like any other, so a series
// merged by close sees the bars closed by 12:00Z and one merged by open the
// bars opened by it, with no calendar guessed for the print itself.
void test_raw_label_off_the_session() {
    scenario = "raw label off the session";
    std::vector<Bar> chart = nyse_quarter_chart({monday()});
    Bar early = chart.front();
    early.timestamp = monday() + 12 * kHour;  // 08:00 New York
    chart.insert(chart.begin(), early);
    const NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + kDay);
    for (const bool lookahead : {false, true}) {
        NativeRunSpec raw = nyse_spec("xsym-labels-off", "15");
        raw.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        raw.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
        raw.instrument_feeds.push_back(feed);
        raw.subscriptions.push_back(instrument_series(feed, lookahead));
        InstrumentHost host;
        if (!run_batch(host, raw, chart)) return;
        std::vector<std::int64_t> open, close;
        chart_intervals(raw, chart, open, close);
        CHECK(!open.empty() && open.front() == early.timestamp);
        CHECK(!close.empty() && close.front() == early.timestamp);  // traded nothing
        check_against_rule(host, 0, feed, chart, expected_exposure(feed, open, close, lookahead),
                           false);
        // 00:00Z..11:45Z closed by 12:00Z; 00:00Z..12:00Z opened by it.
        CHECK(host.reads.front().index[0] == (lookahead ? 48 : 47));
    }
}

// ---- 2. a daily foreign close after the chart's close ---------------------

void test_daily_foreign_close_seen_a_day_late() {
    scenario = "F-1D-like chart <- US10Y-like daily feed";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 10);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = us10y_like_daily_feed(days);
    NativeRunSpec spec = nyse_spec("xsym-daily-late", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));

    std::vector<std::int64_t> open, close;
    chart_intervals(spec, chart, open, close);
    // The chart's daily bar closes at the 20:00Z session close; the feed's
    // bar of the same trade date closes at 21:30Z.
    CHECK(!close.empty() && close.front() == monday() + 20 * kHour);
    CHECK(feed.close_ms.front() == monday() + 21 * kHour + 30 * kMinute);

    InstrumentHost host;
    if (!run_batch(host, spec, chart)) return;
    const auto want = expected_exposure(feed, open, close, false);
    check_against_rule(host, 0, feed, chart, want, false);
    // One day late: chart day j sees the feed's trade date j - 1, and the
    // first chart day sees nothing yet.
    for (std::size_t j = 0; j < chart.size(); ++j) {
        CHECK(host.reads[j].index[0] == static_cast<std::int64_t>(j) - 1);
    }
    CHECK(!host.reads[0].visible[0].has_value());
}

// ---- 3. the weekend: carried under gaps = false, na under gaps = true ------

void test_weekend_carry_and_gaps() {
    scenario = "24x7 daily chart <- weekday-only daily feed";
    const std::vector<Bar> chart = continuous_daily_chart(monday(), 14);
    const NativeInstrumentFeed feed = weekday_daily_feed(weekdays_from(monday(), 10));
    NativeRunSpec spec = continuous_spec("xsym-weekend", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed, false, false));
    spec.subscriptions.push_back(instrument_series(feed, false, true));

    std::vector<std::int64_t> open, close;
    chart_intervals(spec, chart, open, close);
    CHECK(!close.empty() && close.front() == monday() + kDay);

    InstrumentHost host(2);
    if (!run_batch(host, spec, chart)) return;
    const auto want = expected_exposure(feed, open, close, false);
    check_against_rule(host, 0, feed, chart, want, false);
    check_against_rule(host, 1, feed, chart, want, true);
    for (std::size_t j = 0; j < chart.size(); ++j) {
        const int wd = weekday(chart[j].timestamp);
        if (wd >= 5) {
            // Saturday and Sunday: the carried Friday under gaps = false ...
            CHECK(host.reads[j].visible[0].has_value());
            if (host.reads[j].visible[0]) {
                CHECK(weekday(host.reads[j].visible[0]->timestamp + 2 * kHour) == 4);
            }
            // ... and na under gaps = true.
            CHECK(!host.reads[j].visible[1].has_value());
        } else {
            CHECK(host.reads[j].visible[0].has_value() && host.reads[j].visible[1].has_value());
        }
    }
}

// ---- 4. a finer foreign feed under a daily chart, lookahead and [1] -------

void test_finer_feed_under_daily_lookahead() {
    scenario = "AAPL-1D-like chart <- VIX-240-like feed, lookahead on, [1]";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 8);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = vix_like_240_feed(days);
    NativeRunSpec spec = nyse_spec("xsym-ltf-lookahead", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed, true, false));
    spec.subscriptions.push_back(instrument_series(feed, false, false));

    std::vector<std::int64_t> open, close;
    chart_intervals(spec, chart, open, close);
    CHECK(!open.empty() && open.front() == monday() + 13 * kHour + 30 * kMinute);

    InstrumentHost host(2);
    if (!run_batch(host, spec, chart)) return;
    check_against_rule(host, 0, feed, chart, expected_exposure(feed, open, close, true), false);
    check_against_rule(host, 1, feed, chart, expected_exposure(feed, open, close, false), false);
    for (std::size_t j = 0; j < chart.size(); ++j) {
        const auto d = static_cast<std::int64_t>(j);
        // lookahead: the bar in progress at the chart bar's open (13:30Z),
        // whose final values it carries, and the [1] read is the bar before
        // it in the context, clipped at 13:26Z.
        CHECK(host.reads[j].index[0] == 4 * d + 2);
        CHECK(host.reads[j].previous[0] == 4 * d + 1);
        // lookahead off: at the 20:00Z close the 13:30Z bar has closed
        // (17:30Z) and the 17:30Z one has not (20:15Z).
        CHECK(host.reads[j].index[1] == 4 * d + 2);
    }
    // The first chart bar: the report's pin (context bar 2, [1] = the bar
    // that opened 11:15Z and closed 13:26Z), with the close the feed gave it.
    CHECK(host.reads[0].index[0] == 2);
    const std::size_t first_prev = static_cast<std::size_t>(host.reads[0].previous[0]);
    CHECK(feed.bars[first_prev].timestamp == monday() + 11 * kHour + 15 * kMinute);
    CHECK(feed.close_ms[first_prev] == monday() + 13 * kHour + 26 * kMinute);
    bool saw_clipped_close = false;
    for (const auto& delivery : host.deliveries) {
        if (delivery.subscription == 0 && delivery.feed_index == 1) {
            saw_clipped_close =
                delivery.context.interval.last_traded_close_ms == monday() + 13 * kHour + 26 * kMinute;
        }
    }
    CHECK(saw_clipped_close);
}

// ---- 5. history before the first input -----------------------------------

void test_history_before_first_input() {
    scenario = "feed history before the first chart bar";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 3);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    // The feed starts three trade dates before the chart does.
    std::vector<std::int64_t> feed_days = weekdays_from(monday() - 7 * kDay, 8);
    const NativeInstrumentFeed feed = weekday_daily_feed(feed_days);
    NativeRunSpec spec = nyse_spec("xsym-history", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));

    std::vector<std::int64_t> open, close;
    chart_intervals(spec, chart, open, close);
    InstrumentHost host;
    if (!run_batch(host, spec, chart)) return;
    const auto want = expected_exposure(feed, open, close, false);
    check_against_rule(host, 0, feed, chart, want, false);
    // The five older bars and Monday's own (closed 21:00Z, after the 20:00Z
    // chart close, so not yet) -- the five ride on the first chart bar, in
    // order, before its calculation.
    CHECK(host.reads[0].delivered_on_this_bar[0] == 5);
    CHECK(host.reads[0].index[0] == 4);
    const std::vector<std::string> want_head = {
        "input:0@" + std::to_string(chart[0].timestamp),
        "series:0@" + std::to_string(feed.bars[0].timestamp),
        "series:0@" + std::to_string(feed.bars[1].timestamp),
        "series:0@" + std::to_string(feed.bars[2].timestamp),
        "series:0@" + std::to_string(feed.bars[3].timestamp),
        "series:0@" + std::to_string(feed.bars[4].timestamp),
        "bar@" + std::to_string(chart[0].timestamp),
    };
    CHECK(host.log.size() >= want_head.size());
    if (host.log.size() >= want_head.size()) {
        CHECK(std::vector<std::string>(host.log.begin(), host.log.begin() + 7) == want_head);
    }
}

// ---- 6. the context start -------------------------------------------------

void test_context_start() {
    scenario = "24h chart <- instrument opening 13:30Z";
    const std::vector<Bar> chart = continuous_quarter_chart(monday(), 2 * 96);
    const NativeInstrumentFeed feed = rth_only_quarter_feed({monday(), monday() + kDay});
    NativeRunSpec spec = continuous_spec("xsym-context-start", "15");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));

    std::vector<std::int64_t> open, close;
    chart_intervals(spec, chart, open, close);
    InstrumentHost host;
    if (!run_batch(host, spec, chart)) return;
    const auto want = expected_exposure(feed, open, close, false);
    check_against_rule(host, 0, feed, chart, want, false);
    // 13:30Z is chart bar 54: before it the series is empty, on it the first
    // feed bar (context bar 0) closes with the chart bar and is seen.
    for (std::size_t j = 0; j < 54; ++j) CHECK(!host.reads[j].visible[0].has_value());
    CHECK(host.reads[54].index[0] == 0);
    CHECK(host.reads[54].visible[0].has_value());
    // After the instrument's close the value stands (gaps = false) until the
    // next day's first bar has closed.
    CHECK(host.reads[95].index[0] == host.reads[81].index[0]);
    CHECK(host.reads[96 + 54].index[0] == host.reads[95].index[0] + 1);
}

// ---- 7. the named refusals ------------------------------------------------

void expect_refused(const NativeRunSpec& spec, NativeRunSpecError error,
                    NativeRunSpecField field, const char* what) {
    const auto result = validate_native_run_spec(spec);
    if (result.error != error || result.field != field) {
        std::printf("  %s: got error %d field %d, want %d %d\n", what,
                    static_cast<int>(result.error), static_cast<int>(result.field),
                    static_cast<int>(error), static_cast<int>(field));
    }
    CHECK(result.error == error);
    CHECK(result.field == field);
}

void test_refusals() {
    scenario = "refusals";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 3);
    const NativeInstrumentFeed good = us10y_like_daily_feed(days);
    const NativeRunSpec base = nyse_spec("xsym-refusals", "D");
    {
        NativeRunSpec spec = base;
        spec.instrument_feeds.push_back(good);
        spec.subscriptions.push_back(instrument_series(good));
        CHECK(validate_native_run_spec(spec).ok());
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.instrument.clear();
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::EmptyRequiredString,
                       NativeRunSpecField::InstrumentFeedInstrument, "empty instrument");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.tf = "7Q";
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedTimeframe,
                       NativeRunSpecField::InstrumentFeedTimeframe, "bad literal");
    }
    {
        // "D" and "1D" are one key.
        NativeRunSpec spec = base;
        NativeInstrumentFeed again = good;
        again.tf = "1D";
        spec.instrument_feeds.push_back(good);
        spec.instrument_feeds.push_back(again);
        expect_refused(spec, NativeRunSpecError::DuplicateInstrumentFeed,
                       NativeRunSpecField::InstrumentFeedTimeframe, "duplicate key");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        std::swap(feed.bars[0], feed.bars[1]);
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::UnorderedInstrumentFeedBars,
                       NativeRunSpecField::InstrumentFeedBars, "unordered bars");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.bars[1].high = std::nan("");
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedBar,
                       NativeRunSpecField::InstrumentFeedBars, "non-finite price");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.bars[1].volume = -1.0;
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedBar,
                       NativeRunSpecField::InstrumentFeedBars, "negative volume");
    }
    {
        // A negative or zero price is data (a spread, a yield): admitted.
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.bars[1].low = -0.5;
        feed.bars[1].open = 0.0;
        spec.instrument_feeds.push_back(feed);
        CHECK(validate_native_run_spec(spec).ok());
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.close_ms.pop_back();
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedClose,
                       NativeRunSpecField::InstrumentFeedClose, "short closes");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.close_ms[0] = feed.bars[0].timestamp;
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedClose,
                       NativeRunSpecField::InstrumentFeedClose, "close at the open");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.close_ms[0] = feed.bars[1].timestamp + 1;
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedClose,
                       NativeRunSpecField::InstrumentFeedClose, "close past the next open");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.columns.push_back({"fp_delta", std::vector<double>(feed.bars.size() - 1, 1.0)});
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedColumn,
                       NativeRunSpecField::InstrumentFeedColumns, "short column");
    }
    {
        NativeRunSpec spec = base;
        NativeInstrumentFeed feed = good;
        feed.columns.push_back({"fp_delta", std::vector<double>(feed.bars.size(), 1.0)});
        feed.columns.push_back({"fp_delta", std::vector<double>(feed.bars.size(), 2.0)});
        spec.instrument_feeds.push_back(feed);
        expect_refused(spec, NativeRunSpecError::InvalidInstrumentFeedColumn,
                       NativeRunSpecField::InstrumentFeedColumns, "repeated column");
    }
    {
        NativeRunSpec spec = base;
        spec.instrument_feeds.push_back(good);
        NativeTimeframeSubscription series = instrument_series(good);
        series.instrument = "SYN:NOPE";
        spec.subscriptions.push_back(series);
        expect_refused(spec, NativeRunSpecError::SubscriptionWithoutInstrumentFeed,
                       NativeRunSpecField::SubscriptionInstrument, "unknown instrument");
    }
    {
        // The key is (instrument, timeframe): the right symbol at another
        // timeframe names no feed.
        NativeRunSpec spec = base;
        spec.instrument_feeds.push_back(good);
        NativeTimeframeSubscription series = instrument_series(good);
        series.tf = "60";
        spec.subscriptions.push_back(series);
        expect_refused(spec, NativeRunSpecError::SubscriptionWithoutInstrumentFeed,
                       NativeRunSpecField::SubscriptionInstrument, "wrong timeframe");
    }
    {
        NativeRunSpec spec = base;
        spec.instrument_feeds.push_back(good);
        NativeTimeframeSubscription series = instrument_series(good);
        series.instrument.clear();
        spec.subscriptions.push_back(series);
        expect_refused(spec, NativeRunSpecError::EmptyRequiredString,
                       NativeRunSpecField::SubscriptionInstrument, "series without a key");
    }
    {
        NativeRunSpec spec = base;
        spec.instrument_feeds.push_back(good);
        NativeTimeframeSubscription series = instrument_series(good);
        series.authoritative_bars = good.bars;
        spec.subscriptions.push_back(series);
        expect_refused(spec, NativeRunSpecError::InstrumentSubscriptionBars,
                       NativeRunSpecField::SubscriptionBars, "authoritative bars");
    }
    {
        NativeRunSpec spec = base;
        NativeTimeframeSubscription series;
        series.tf = "D";
        series.instrument = "SYN:US10Y";
        spec.subscriptions.push_back(series);
        expect_refused(spec, NativeRunSpecError::InstrumentOnNonInstrumentSeries,
                       NativeRunSpecField::SubscriptionInstrument, "key on an input series");
    }
    {
        // The begin-time judgement knows the running spec's feeds; the
        // four-argument one knows none and refuses the series by name.
        const auto four = validate_native_timeframe_subscriptions(
            {instrument_series(good)}, "D", false, std::nullopt);
        CHECK(four.error == NativeRunSpecError::SubscriptionWithoutInstrumentFeed);
        const auto five = validate_native_timeframe_subscriptions(
            {instrument_series(good)}, "D", false, std::nullopt, {good});
        CHECK(five.ok());
        CHECK(validate_native_instrument_feeds({good}).ok());
        CHECK(validate_native_instrument_feeds({}).ok());
    }
}

// ---- 8. beside an input series, twice on one host --------------------------

void test_beside_an_input_series() {
    scenario = "instrument series beside an input series";
    const std::vector<std::int64_t> days = {monday()};
    const std::vector<Bar> chart = nyse_quarter_chart(days);
    const NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + kDay);
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";

    NativeRunSpec alone = nyse_spec("xsym-alone", "15");
    alone.subscriptions.push_back(hourly);
    InstrumentHost solo(1);
    if (!run_batch(solo, alone, chart)) return;

    NativeRunSpec both = nyse_spec("xsym-both", "15");
    both.instrument_feeds.push_back(feed);
    both.subscriptions.push_back(instrument_series(feed));
    both.subscriptions.push_back(hourly);
    InstrumentHost host(2);
    for (int run = 1; run <= 2; ++run) {
        both.identity.run_number = static_cast<std::uint64_t>(run);
        host.deliveries.clear();
        host.reads.clear();
        host.log.clear();
        host.bars_seen = 0;
        host.count.assign(2, 0);
        host.since_bar.assign(2, 0);
        if (!run_batch(host, both, chart)) return;
        std::vector<const Delivery*> hourly_got;
        for (const auto& delivery : host.deliveries) {
            if (delivery.subscription == 1) hourly_got.push_back(&delivery);
        }
        CHECK(hourly_got.size() == solo.deliveries.size());
        for (std::size_t k = 0; k < hourly_got.size() && k < solo.deliveries.size(); ++k) {
            CHECK(bars_equal(hourly_got[k]->bar, solo.deliveries[k].bar));
            CHECK(hourly_got[k]->context.delivered_at_ms
                  == solo.deliveries[k].context.delivered_at_ms);
            CHECK(hourly_got[k]->context.interval.open_ms
                  == solo.deliveries[k].context.interval.open_ms);
        }
        std::vector<std::int64_t> open, close;
        chart_intervals(both, chart, open, close);
        check_against_rule(host, 0, feed, chart, expected_exposure(feed, open, close, false),
                           false);
    }
}

// ---- 9. streams refuse an installed instrument feed -----------------------

void test_stream_refused() {
    scenario = "stream_begin refuses instrument feeds";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 3);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    NativeRunSpec spec = nyse_spec("xsym-stream", "D");
    spec.instrument_feeds.push_back(us10y_like_daily_feed(days));
    InstrumentHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const bool began = host.stream_begin(chart.data(), static_cast<int>(chart.size()), "D", "D");
    CHECK(!began);
    CHECK(host.last_error() == "native instrument feeds are not supported by streaming");
    // A refusal, not a failure: the host is still Ready, its spec unconsumed.
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
    // Without the feed, the same spec streams.
    InstrumentHost fresh;
    CHECK(fresh.configure_native(nyse_spec("xsym-stream", "D")).status
          == NativeSetupStatus::Applied);
    CHECK(fresh.stream_begin(chart.data(), static_cast<int>(chart.size()), "D", "D"));
    CHECK(fresh.stream_end(true));
}

// ---- 10. neutrality and identity -------------------------------------------

void test_neutrality_and_identity() {
    scenario = "digests and continuation";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 5);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = us10y_like_daily_feed(days);

    const NativeRunSpec plain = nyse_spec("xsym-identity", "D");
    NativeRunSpec with_feed = plain;
    with_feed.instrument_feeds.push_back(feed);
    NativeRunSpec other_close = with_feed;
    other_close.instrument_feeds[0].close_ms[2] -= kMinute;
    NativeRunSpec other_column = with_feed;
    other_column.instrument_feeds[0].columns.push_back(
        {"extra", std::vector<double>(feed.bars.size(), 0.5)});

    // No feed, no fold: an empty list digests exactly as the spec did before
    // the field existed (the digest of the same spec, built without touching
    // the field), and every installed variation moves it.
    NativeRunSpec untouched = nyse_spec("xsym-identity", "D");
    CHECK(native_run_spec_digest(plain) == native_run_spec_digest(untouched));
    CHECK(native_run_spec_digest(with_feed) != native_run_spec_digest(plain));
    CHECK(native_run_spec_digest(other_close) != native_run_spec_digest(with_feed));
    CHECK(native_run_spec_digest(other_column) != native_run_spec_digest(with_feed));
    CHECK(native_instrument_feeds_digest({feed}) != native_instrument_feeds_digest({}));
    // A NaN volume folds as one quiet NaN whatever its payload.
    NativeRunSpec other_nan = with_feed;
    other_nan.instrument_feeds[0].bars[0].volume = -std::nan("7");
    CHECK(native_run_spec_digest(other_nan) == native_run_spec_digest(with_feed));

    // A series reading the feed digests its key; an input series does not
    // change its digest for the field's presence.
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    NativeTimeframeSubscription hourly_again = hourly;
    CHECK(native_timeframe_subscriptions_digest({hourly})
          == native_timeframe_subscriptions_digest({hourly_again}));
    NativeTimeframeSubscription a = instrument_series(feed);
    NativeTimeframeSubscription b = a;
    b.instrument = "SYN:OTHER";
    CHECK(native_timeframe_subscriptions_digest({a}) != native_timeframe_subscriptions_digest({b}));

    // One spec, one continuation; another close, another continuation.
    const auto continuation = [&](NativeRunSpec spec) {
        spec.subscriptions.push_back(instrument_series(spec.instrument_feeds[0]));
        InstrumentHost host;
        if (!run_batch(host, spec, chart)) return std::uint64_t{0};
        return host.native_continuation_hash();
    };
    const std::uint64_t first = continuation(with_feed);
    CHECK(first != 0);
    CHECK(continuation(with_feed) == first);
    CHECK(continuation(other_close) != first);
}

// ---- 11. a named column rides with its bar ---------------------------------

class ColumnHost : public InstrumentHost {
public:
    const NativeInstrumentFeed* feed = nullptr;
    std::vector<double> read_values;
    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        InstrumentHost::on_native_timeframe_bar(bar, context);
        const std::int64_t index = deliveries.back().feed_index;
        read_values.push_back(feed->columns[0].values[static_cast<std::size_t>(index)]);
    }
};

void test_column_rides_with_its_bar() {
    scenario = "a named column";
    const std::vector<std::int64_t> days = {monday()};
    const std::vector<Bar> chart = nyse_quarter_chart(days);
    NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + kDay);
    NativeInstrumentColumn delta{"fp_delta_100_70", {}};
    for (std::size_t i = 0; i < feed.bars.size(); ++i) {
        delta.values.push_back(i % 9 == 0 ? std::nan("") : 17.0 * static_cast<double>(i) - 300.0);
    }
    feed.columns.push_back(delta);
    NativeRunSpec spec = nyse_spec("xsym-column", "15");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));
    ColumnHost host;
    host.feed = &spec.instrument_feeds[0];
    if (!run_batch(host, spec, chart)) return;
    CHECK(!host.read_values.empty());
    for (std::size_t k = 0; k < host.read_values.size(); ++k) {
        CHECK(same(host.read_values[k], delta.values[k]));
    }
}

}  // namespace

int main() {
    test_foreign_session_differs();
    test_raw_label_partition_reads_the_calendar();
    test_raw_label_off_the_session();
    test_daily_foreign_close_seen_a_day_late();
    test_weekend_carry_and_gaps();
    test_finer_feed_under_daily_lookahead();
    test_history_before_first_input();
    test_context_start();
    test_refusals();
    test_beside_an_input_series();
    test_stream_refused();
    test_neutrality_and_identity();
    test_column_rides_with_its_bar();
    std::printf("test_native_instrument_feed: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
