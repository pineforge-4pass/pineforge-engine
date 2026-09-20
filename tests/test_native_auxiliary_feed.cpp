// The auxiliary finer feed of a bare native host (NativeRunSpec::auxiliary_feed
// and NativeSeriesSource::AuxiliaryFeed): a host whose input is 15-minute bars
// hands the kernel the 1-minute bars that input does not have, and reads a
// declared series built from them. No source layer, no Pine expression, no
// chart-slice mapping: the routing is by time.
//
// Witnesses, all independent of the feature under test:
//   1. a "5" series over a "15" input is refused without a feed and built from
//      the feed with one: every bucket equals the hand aggregation of its five
//      minutes AND the bucket a standalone TimeframeAggregator("5","1") makes,
//      three ride on each input, oldest first, before that input's calculation;
//   2. it is the FEED the series is built from: a "60" series from the input
//      and a "60" series from the feed, declared side by side over a feed that
//      carries two prints the input does not, differ in exactly those prints;
//   3. routing by time: feed bars earlier than the first input are folded on
//      that first input (history), bars in a hole of the input ride on the
//      next accepted input, bars later than the last input's period are never
//      folded, and a bucket the FEED leaves short completes lazily on the next
//      feed bar, which rides on the next input;
//   4. lookahead = true delivers a bucket's final values on the input whose
//      slice held its first feed bar; gaps = true clears the series on every
//      input it delivers nothing on; a series of the feed's own timeframe
//      passes every feed bar through;
//   5. every named refusal, and the three accepted pairings (finer than the
//      input, equal to it, coarser);
//   6. neutrality: a list of input-built series keeps the digest the pre-field
//      fold gave it (re-derived here by hand), a spec that declares no feed
//      keeps its run-spec digest, and both digests move with the feed;
//   7. the begin-time declaration: legal only inside on_native_run_begin, feed
//      first and series second, refused where it would strand a staged series,
//      and the staged spec names what ran;
//   8. two runs of one spec share a continuation hash, a run over a different
//      feed does not.

#include "native_auxiliary_feed_fixture.hpp"

namespace {

// ---- 1. a series finer than the input, built from the feed ----------------

void test_finer_series_is_built_from_the_feed() {
    scenario = "finer series from the feed";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> minutes = minute_bars(origin, 120);
    const std::vector<Bar> inputs = quarter_bars_over(minutes, 8);

    // Without a feed the very same series is what it always was: refused.
    {
        NativeRunSpec spec = base_spec("native-aux-finer-refused");
        spec.subscriptions.push_back(series("5", NativeSeriesSource::Input));
        const auto refused = validate_native_run_spec(spec);
        CHECK(refused.error == NativeRunSpecError::SubscriptionFinerThanInput);
        CHECK(refused.field == NativeRunSpecField::SubscriptionTimeframe);
    }

    NativeRunSpec spec = base_spec("native-aux-finer");
    spec.auxiliary_feed = minute_feed(minutes);
    spec.subscriptions.push_back(series("5", NativeSeriesSource::AuxiliaryFeed));
    FeedHost host;
    host.probes = 1;
    if (!run_batch(host, spec, inputs)) return;

    // 120 minutes = 24 five-minute buckets, three per fifteen-minute input.
    CHECK(host.deliveries.size() == 24);
    CHECK(host.bars_seen == 8);

    // The standalone aggregator over the same minutes: the engine-free oracle.
    TimeframeAggregator oracle("5", "1", "UTC", "24x7");
    std::vector<Bar> oracle_buckets;
    for (std::size_t i = 0; i < minutes.size(); ++i) {
        const std::int64_t next = i + 1 < minutes.size() ? minutes[i + 1].timestamp : 0;
        const AggregatedBar step = oracle.feed(minutes[i], next);
        if (step.is_complete) oracle_buckets.push_back(step.bar);
    }
    CHECK(oracle_buckets.size() == 24);

    for (std::size_t k = 0; k < host.deliveries.size() && k < 24; ++k) {
        const Delivery& delivery = host.deliveries[k];
        const std::int64_t label = origin + static_cast<std::int64_t>(k) * 5 * kMinute;
        check_bucket(delivery.bar, hand_aggregate(minutes, k * 5, 5, label),
                     "five-minute bucket vs hand aggregate");
        if (k < oracle_buckets.size()) {
            check_bucket(delivery.bar, oracle_buckets[k],
                         "five-minute bucket vs TimeframeAggregator");
        }
        CHECK(delivery.subscription == 0);
        CHECK(delivery.accessor_matches);
        CHECK(delivery.context.completion == NativeCompletionKind::Confirmed);
        // Buckets 3j, 3j+1, 3j+2 ride on input j, before its calculation.
        const std::size_t input = k / 3;
        CHECK(delivery.bars_before == static_cast<int>(input));
        CHECK(delivery.context.delivered_at_ms == inputs[input].timestamp);
        // The span is the bucket's own, read over the feed's timeframe.
        CHECK(delivery.context.interval.open_ms == label);
        CHECK(delivery.context.interval.next_period_open_ms == label + 5 * kMinute);
    }

    // What the calculation of input j reads: its own last five-minute bucket.
    CHECK(host.series_at_bar.size() == 8);
    for (std::size_t j = 0; j < host.series_at_bar.size(); ++j) {
        const auto& read = host.series_at_bar[j][0];
        CHECK(read.has_value());
        if (!read) continue;
        const std::size_t k = j * 3 + 2;
        check_bucket(*read, hand_aggregate(minutes, k * 5, 5,
                                           origin + static_cast<std::int64_t>(k) * 5 * kMinute),
                     "series at calculation");
    }

    // The callback order on one input: input, its three buckets, its bar.
    const std::vector<std::string> head(host.log.begin(), host.log.begin() + 5);
    const std::vector<std::string> want_head = {
        "input:0@" + std::to_string(origin),
        "htf:0@" + std::to_string(origin),
        "htf:0@" + std::to_string(origin + 5 * kMinute),
        "htf:0@" + std::to_string(origin + 10 * kMinute),
        "bar@" + std::to_string(origin),
    };
    check_logs_equal(head, want_head);

    // The staged spec names the feed that ran.
    const auto view = host.native_state();
    CHECK(view.spec != nullptr && view.spec->auxiliary_feed.has_value());
    if (view.spec && view.spec->auxiliary_feed) {
        CHECK(view.spec->auxiliary_feed->tf == "1");
        CHECK(view.spec->auxiliary_feed->bars.size() == 120);
    }
}

// ---- 2. it is the feed the series is built from ---------------------------

void test_the_feed_carries_what_the_input_does_not() {
    scenario = "the feed the input does not have";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> quiet = minute_bars(origin, 60);
    const std::vector<Bar> inputs = quarter_bars_over(quiet, 4);
    // The feed is the same hour with two prints the input never saw: minute
    // 37 trades 9.0 above its open and minute 52 trades 20.0 below it (the
    // fixture's prices rise 0.25 a minute, so each is the hour's extreme).
    std::vector<Bar> feed = quiet;
    feed[37].high = feed[37].open + 9.0;
    feed[52].low = feed[52].open - 20.0;

    NativeRunSpec spec = base_spec("native-aux-distinct");
    spec.auxiliary_feed = minute_feed(feed);
    spec.subscriptions.push_back(series("60", NativeSeriesSource::Input));
    spec.subscriptions.push_back(series("60", NativeSeriesSource::AuxiliaryFeed));
    FeedHost host;
    if (!run_batch(host, spec, inputs)) return;

    // One hour, one bucket per series, both on the hour's fourth input, in
    // declaration order.
    CHECK(host.deliveries.size() == 2);
    if (host.deliveries.size() != 2) return;
    const Delivery& from_input = host.deliveries[0];
    const Delivery& from_feed = host.deliveries[1];
    CHECK(from_input.subscription == 0);
    CHECK(from_feed.subscription == 1);
    CHECK(from_input.context.delivered_at_ms == inputs[3].timestamp);
    CHECK(from_feed.context.delivered_at_ms == inputs[3].timestamp);
    CHECK(from_input.bars_before == 3);
    CHECK(from_feed.bars_before == 3);

    check_bucket(from_input.bar, hand_aggregate(inputs, 0, 4, origin), "hour from the input");
    check_bucket(from_feed.bar, hand_aggregate(feed, 0, 60, origin), "hour from the feed");
    // The two differ in exactly the prints the input does not have.
    CHECK(same(from_feed.bar.high, feed[37].open + 9.0));
    CHECK(same(from_feed.bar.low, feed[52].open - 20.0));
    CHECK(from_input.bar.high < from_feed.bar.high);
    CHECK(from_input.bar.low > from_feed.bar.low);
    CHECK(same(from_input.bar.open, from_feed.bar.open));
    CHECK(same(from_input.bar.close, from_feed.bar.close));
    CHECK(same(from_input.bar.volume, from_feed.bar.volume));
}

// ---- 3. routing by time ---------------------------------------------------

void test_history_before_the_first_input() {
    scenario = "feed history before the first input";
    const std::int64_t origin = origin_ms();
    // The feed opens a whole hour before the input does.
    const std::vector<Bar> feed = minute_bars(origin - kHour, 120);
    const std::vector<Bar> in_range(feed.begin() + 60, feed.end());
    const std::vector<Bar> inputs = quarter_bars_over(in_range, 4);

    NativeRunSpec spec = base_spec("native-aux-history");
    spec.auxiliary_feed = minute_feed(feed);
    spec.subscriptions.push_back(series("60", NativeSeriesSource::AuxiliaryFeed));
    FeedHost host;
    if (!run_batch(host, spec, inputs)) return;

    CHECK(host.deliveries.size() == 2);
    if (host.deliveries.size() != 2) return;
    // The hour the input never reached, folded on the first input, ahead of it.
    check_bucket(host.deliveries[0].bar, hand_aggregate(feed, 0, 60, origin - kHour),
                 "history hour");
    CHECK(host.deliveries[0].bars_before == 0);
    CHECK(host.deliveries[0].context.delivered_at_ms == inputs[0].timestamp);
    CHECK(host.deliveries[0].context.interval.open_ms == origin - kHour);
    CHECK(host.deliveries[0].context.completion == NativeCompletionKind::Confirmed);
    // The input's own hour, on its fourth bar.
    check_bucket(host.deliveries[1].bar, hand_aggregate(feed, 60, 60, origin), "input hour");
    CHECK(host.deliveries[1].bars_before == 3);
    CHECK(host.deliveries[1].context.delivered_at_ms == inputs[3].timestamp);
}

void test_hole_in_the_input_and_trailing_feed() {
    scenario = "input hole and trailing feed";
    const std::int64_t origin = origin_ms();
    // Ninety minutes of feed; the input has bars 0, 1, 3 (a hole over 00:30)
    // and stops there, so the feed's last thirty minutes trail it.
    const std::vector<Bar> feed = minute_bars(origin, 90);
    const std::vector<Bar> all = quarter_bars_over(feed, 4);
    const std::vector<Bar> inputs = {all[0], all[1], all[3]};

    NativeRunSpec spec = base_spec("native-aux-hole");
    spec.auxiliary_feed = minute_feed(feed);
    // A series of the input's own period, but built from the feed.
    spec.subscriptions.push_back(series("15", NativeSeriesSource::AuxiliaryFeed));
    FeedHost host;
    if (!run_batch(host, spec, inputs)) return;

    // Quarters 0..3 are delivered; quarters 4 and 5 opened after the last
    // input's period ended and are never folded.
    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() != 4) return;
    for (std::size_t k = 0; k < 4; ++k) {
        check_bucket(host.deliveries[k].bar,
                     hand_aggregate(feed, k * 15, 15,
                                    origin + static_cast<std::int64_t>(k) * kQuarter),
                     "quarter from the feed");
    }
    // Quarter 2 lies in the input's hole: it rides on the next accepted
    // input, ahead of that input's own quarter.
    CHECK(host.deliveries[0].context.delivered_at_ms == all[0].timestamp);
    CHECK(host.deliveries[1].context.delivered_at_ms == all[1].timestamp);
    CHECK(host.deliveries[2].context.delivered_at_ms == all[3].timestamp);
    CHECK(host.deliveries[3].context.delivered_at_ms == all[3].timestamp);
    CHECK(host.deliveries[2].bars_before == 2);
    CHECK(host.deliveries[3].bars_before == 2);
}

void test_short_feed_bucket_completes_lazily() {
    scenario = "a bucket the feed leaves short";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> whole = minute_bars(origin, 30);
    const std::vector<Bar> inputs = quarter_bars_over(whole, 2);
    // The feed has no 00:14 bar: the first quarter holds fourteen minutes and
    // nothing tells the series it is over until 00:15 arrives.
    std::vector<Bar> feed = whole;
    feed.erase(feed.begin() + 14);

    NativeRunSpec spec = base_spec("native-aux-short");
    spec.auxiliary_feed = minute_feed(feed);
    spec.subscriptions.push_back(series("15", NativeSeriesSource::AuxiliaryFeed));
    FeedHost host;
    if (!run_batch(host, spec, inputs)) return;

    CHECK(host.deliveries.size() == 2);
    if (host.deliveries.size() != 2) return;
    // Fourteen minutes, closed by the next feed bar, which belongs to the
    // second input's slice.
    check_bucket(host.deliveries[0].bar, hand_aggregate(feed, 0, 14, origin), "short quarter");
    CHECK(host.deliveries[0].context.completion == NativeCompletionKind::LazyComplete);
    CHECK(host.deliveries[0].context.delivered_at_ms == inputs[1].timestamp);
    CHECK(host.deliveries[0].bars_before == 1);
    check_bucket(host.deliveries[1].bar, hand_aggregate(feed, 14, 15, origin + kQuarter),
                 "whole quarter");
    CHECK(host.deliveries[1].context.completion == NativeCompletionKind::Confirmed);
    CHECK(host.deliveries[1].context.delivered_at_ms == inputs[1].timestamp);
}

// ---- 4. publication modes and the passthrough period ----------------------

void test_lookahead_and_gaps() {
    scenario = "lookahead and gaps from the feed";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 120);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 8);

    NativeRunSpec spec = base_spec("native-aux-modes");
    spec.auxiliary_feed = minute_feed(feed);
    spec.subscriptions.push_back(series("60", NativeSeriesSource::AuxiliaryFeed, true, false));
    spec.subscriptions.push_back(series("60", NativeSeriesSource::AuxiliaryFeed, false, true));
    FeedHost host;
    host.probes = 2;
    if (!run_batch(host, spec, inputs)) return;

    const Bar hour0 = hand_aggregate(feed, 0, 60, origin);
    const Bar hour1 = hand_aggregate(feed, 60, 60, origin + kHour);
    std::vector<Delivery> ahead;
    std::vector<Delivery> gapped;
    for (const Delivery& delivery : host.deliveries) {
        (delivery.subscription == 0 ? ahead : gapped).push_back(delivery);
    }
    CHECK(ahead.size() == 2);
    CHECK(gapped.size() == 2);
    if (ahead.size() != 2 || gapped.size() != 2) return;

    // lookahead_on: the hour's FINAL values on the input that opened it.
    check_bucket(ahead[0].bar, hour0, "lookahead hour 0");
    check_bucket(ahead[1].bar, hour1, "lookahead hour 1");
    CHECK(ahead[0].context.delivered_at_ms == inputs[0].timestamp);
    CHECK(ahead[1].context.delivered_at_ms == inputs[4].timestamp);
    CHECK(ahead[0].bars_before == 0);
    CHECK(ahead[1].bars_before == 4);

    // lookahead_off + gaps_on: on the input that closed it, and only there.
    check_bucket(gapped[0].bar, hour0, "gapped hour 0");
    check_bucket(gapped[1].bar, hour1, "gapped hour 1");
    CHECK(gapped[0].context.delivered_at_ms == inputs[3].timestamp);
    CHECK(gapped[1].context.delivered_at_ms == inputs[7].timestamp);
    CHECK(host.series_at_bar.size() == 8);
    for (std::size_t j = 0; j < host.series_at_bar.size(); ++j) {
        // The lookahead series stands from its first delivery on.
        CHECK(host.series_at_bar[j][0].has_value());
        // The gapped series answers only on the bars it publishes on.
        CHECK(host.series_at_bar[j][1].has_value() == (j == 3 || j == 7));
    }
}

void test_series_of_the_feed_timeframe_passes_through() {
    scenario = "series of the feed's own timeframe";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 30);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 2);

    NativeRunSpec spec = base_spec("native-aux-passthrough");
    spec.auxiliary_feed = minute_feed(feed);
    spec.subscriptions.push_back(series("1", NativeSeriesSource::AuxiliaryFeed));
    FeedHost host;
    if (!run_batch(host, spec, inputs)) return;

    CHECK(host.deliveries.size() == 30);
    for (std::size_t k = 0; k < host.deliveries.size() && k < feed.size(); ++k) {
        check_bucket(host.deliveries[k].bar, feed[k], "feed bar");
        CHECK(host.deliveries[k].bars_before == static_cast<int>(k / 15));
        CHECK(host.deliveries[k].context.delivered_at_ms == inputs[k / 15].timestamp);
    }
}

// ---- 5. validation --------------------------------------------------------

NativeRunSpecValidation judged(const char* feed_tf, std::vector<Bar> bars,
                               std::vector<NativeTimeframeSubscription> subscriptions) {
    NativeRunSpec spec = base_spec("native-aux-validation");
    NativeAuxiliaryFeed feed;
    feed.tf = feed_tf;
    feed.bars = std::move(bars);
    spec.auxiliary_feed = std::move(feed);
    spec.subscriptions = std::move(subscriptions);
    return validate_native_run_spec(spec);
}

void test_validation() {
    scenario = "validation";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> good = minute_bars(origin, 3);

    // Accepted: a bare feed, an empty feed, and the three series pairings.
    CHECK(judged("1", good, {}).ok());
    CHECK(judged("1", {}, {}).ok());
    CHECK(judged("1", good, {series("5", NativeSeriesSource::AuxiliaryFeed)}).ok());
    CHECK(judged("1", good, {series("15", NativeSeriesSource::AuxiliaryFeed)}).ok());
    CHECK(judged("1", good, {series("60", NativeSeriesSource::AuxiliaryFeed)}).ok());
    CHECK(judged("1", good, {series("D", NativeSeriesSource::AuxiliaryFeed)}).ok());
    CHECK(judged("1", good, {series("1", NativeSeriesSource::AuxiliaryFeed)}).ok());
    CHECK(judged("5", good, {series("60", NativeSeriesSource::Input),
                             series("5", NativeSeriesSource::AuxiliaryFeed)}).ok());

    // The feed itself.
    auto result = judged("15", good, {});
    CHECK(result.error == NativeRunSpecError::AuxiliaryFeedNotFinerThanInput);
    CHECK(result.field == NativeRunSpecField::AuxiliaryFeedTimeframe);
    result = judged("60", good, {});
    CHECK(result.error == NativeRunSpecError::AuxiliaryFeedNotFinerThanInput);
    result = judged("10", good, {});  // 15 is not a multiple of 10
    CHECK(result.error == NativeRunSpecError::InvalidAuxiliaryFeedTimeframe);
    result = judged("fast", good, {});
    CHECK(result.error == NativeRunSpecError::InvalidAuxiliaryFeedTimeframe);
    result = judged("", good, {});
    CHECK(result.error == NativeRunSpecError::EmptyRequiredString);
    CHECK(result.field == NativeRunSpecField::AuxiliaryFeedTimeframe);

    std::vector<Bar> unordered = good;
    std::swap(unordered[1], unordered[2]);
    result = judged("1", unordered, {});
    CHECK(result.error == NativeRunSpecError::UnorderedAuxiliaryFeedBars);
    CHECK(result.field == NativeRunSpecField::AuxiliaryFeedBars);
    std::vector<Bar> repeated = good;
    repeated[2].timestamp = repeated[1].timestamp;
    CHECK(judged("1", repeated, {}).error == NativeRunSpecError::UnorderedAuxiliaryFeedBars);
    std::vector<Bar> broken = good;
    broken[1].high = broken[1].low - 1.0;
    result = judged("1", broken, {});
    CHECK(result.error == NativeRunSpecError::InvalidAuxiliaryFeedBar);
    CHECK(result.field == NativeRunSpecField::AuxiliaryFeedBars);

    {
        NativeRunSpec spec = base_spec("native-aux-undetected");
        spec.input_tf.clear();
        spec.script_tf.clear();
        spec.timeframe_undetected = true;
        spec.auxiliary_feed = minute_feed(good);
        result = validate_native_run_spec(spec);
        CHECK(result.error == NativeRunSpecError::AuxiliaryFeedWithoutTimeframe);
        CHECK(result.field == NativeRunSpecField::AuxiliaryFeedTimeframe);
    }

    // The series.
    {
        NativeRunSpec spec = base_spec("native-aux-series-without-feed");
        spec.subscriptions.push_back(series("60", NativeSeriesSource::AuxiliaryFeed));
        result = validate_native_run_spec(spec);
        CHECK(result.error == NativeRunSpecError::SubscriptionWithoutAuxiliaryFeed);
        CHECK(result.field == NativeRunSpecField::SubscriptionSource);
    }
    result = judged("5", good, {series("1", NativeSeriesSource::AuxiliaryFeed)});
    CHECK(result.error == NativeRunSpecError::SubscriptionFinerThanAuxiliaryFeed);
    CHECK(result.field == NativeRunSpecField::SubscriptionTimeframe);
    // The feed changes nothing for a series still built from the input.
    result = judged("1", good, {series("5", NativeSeriesSource::Input)});
    CHECK(result.error == NativeRunSpecError::SubscriptionFinerThanInput);
    result = judged("1", good, {series("7", NativeSeriesSource::AuxiliaryFeed)});
    CHECK(result.ok());  // seven minutes is a whole multiple of the feed
    result = judged("5", good, {series("7", NativeSeriesSource::AuxiliaryFeed)});
    CHECK(result.error == NativeRunSpecError::InvalidSubscriptionTimeframe);
    NativeTimeframeSubscription unknown = series("60", NativeSeriesSource::AuxiliaryFeed);
    unknown.source = static_cast<NativeSeriesSource>(7);
    result = judged("1", good, {unknown});
    CHECK(result.error == NativeRunSpecError::UnknownSeriesSource);
    CHECK(result.field == NativeRunSpecField::SubscriptionSource);

    // The begin-time judges agree with configure.
    CHECK(validate_native_auxiliary_feed(std::nullopt, "15", false).ok());
    CHECK(validate_native_auxiliary_feed(minute_feed(good), "15", false).ok());
    CHECK(validate_native_auxiliary_feed(minute_feed(good), "1", false).error
          == NativeRunSpecError::AuxiliaryFeedNotFinerThanInput);
    CHECK(validate_native_timeframe_subscriptions(
              {series("5", NativeSeriesSource::AuxiliaryFeed)}, "15", false).error
          == NativeRunSpecError::SubscriptionWithoutAuxiliaryFeed);
    CHECK(validate_native_timeframe_subscriptions(
              {series("5", NativeSeriesSource::AuxiliaryFeed)}, "15", false,
              minute_feed(good)).ok());

    // configure_native refuses what validate refuses, and stages nothing.
    FeedHost host;
    NativeRunSpec refused = base_spec("native-aux-configure-refused");
    refused.auxiliary_feed = minute_feed(good);
    refused.auxiliary_feed->tf = "15";
    const auto setup = host.configure_native(refused);
    CHECK(setup.status == NativeSetupStatus::Failed);
    CHECK(setup.validation.error == NativeRunSpecError::AuxiliaryFeedNotFinerThanInput);
}

// ---- 6. neutrality --------------------------------------------------------

// The subscription digest exactly as it was folded before a series had a
// source: FNV-1a over the count, then per series the literal, the lookahead
// word, the gaps marker where set, and the authoritative bars.
std::uint64_t pre_source_subscriptions_digest(
        const std::vector<NativeTimeframeSubscription>& subscriptions) {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto u = [&bytes](std::uint64_t value) { bytes(&value, sizeof value); };
    u(subscriptions.size());
    for (const auto& subscription : subscriptions) {
        u(subscription.tf.size());
        bytes(subscription.tf.data(), subscription.tf.size());
        u(subscription.lookahead ? 1u : 0u);
        if (subscription.gaps) u(2u);
        u(subscription.authoritative_bars.size());
        for (const Bar& bar : subscription.authoritative_bars) {
            bytes(&bar.open, sizeof bar.open);
            bytes(&bar.high, sizeof bar.high);
            bytes(&bar.low, sizeof bar.low);
            bytes(&bar.close, sizeof bar.close);
            bytes(&bar.volume, sizeof bar.volume);
            bytes(&bar.timestamp, sizeof bar.timestamp);
        }
    }
    return state;
}

void test_digests_are_neutral_and_sensitive() {
    scenario = "digest neutrality";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 30);

    std::vector<NativeTimeframeSubscription> from_input = {
        series("60", NativeSeriesSource::Input),
        series("D", NativeSeriesSource::Input, true, true),
    };
    from_input[0].authoritative_bars = {hand_aggregate(feed, 0, 30, origin)};
    // Input-built series fold exactly what they folded before the field.
    CHECK(native_timeframe_subscriptions_digest(from_input)
          == pre_source_subscriptions_digest(from_input));
    // A series built from the feed does not.
    std::vector<NativeTimeframeSubscription> from_feed = from_input;
    from_feed[0].source = NativeSeriesSource::AuxiliaryFeed;
    CHECK(native_timeframe_subscriptions_digest(from_feed)
          != native_timeframe_subscriptions_digest(from_input));

    // A spec that declares no feed keeps its digest whether or not the field
    // exists: two specs equal but for an ABSENT feed are one digest, and the
    // digest moves with a declared feed's presence, timeframe and every bar.
    NativeRunSpec plain = base_spec("native-aux-digest");
    plain.subscriptions = from_input;
    NativeRunSpec also_plain = plain;
    also_plain.auxiliary_feed.reset();
    CHECK(native_run_spec_digest(plain) == native_run_spec_digest(also_plain));

    NativeRunSpec declared = plain;
    declared.auxiliary_feed = minute_feed(feed);
    CHECK(native_run_spec_digest(declared) != native_run_spec_digest(plain));
    NativeRunSpec empty_feed = plain;
    empty_feed.auxiliary_feed = minute_feed({});
    CHECK(native_run_spec_digest(empty_feed) != native_run_spec_digest(plain));
    CHECK(native_run_spec_digest(empty_feed) != native_run_spec_digest(declared));
    NativeRunSpec other_tf = declared;
    other_tf.auxiliary_feed->tf = "3";
    CHECK(native_run_spec_digest(other_tf) != native_run_spec_digest(declared));
    NativeRunSpec other_bar = declared;
    other_bar.auxiliary_feed->bars[17].close += 0.01;
    CHECK(native_run_spec_digest(other_bar) != native_run_spec_digest(declared));
    CHECK(native_auxiliary_feed_digest(*other_bar.auxiliary_feed)
          != native_auxiliary_feed_digest(*declared.auxiliary_feed));
}

// ---- 7. declaring at begin ------------------------------------------------

class DeclaringHost final : public FeedHost {
public:
    std::optional<NativeAuxiliaryFeed> feed;
    std::vector<NativeTimeframeSubscription> declared;
    bool series_first = false;
    bool feed_accepted = false;
    bool series_accepted = false;

    void on_native_run_begin() override {
        if (series_first) {
            series_accepted = declare_timeframe_subscriptions(declared);
            feed_accepted = declare_auxiliary_feed(feed);
            return;
        }
        feed_accepted = declare_auxiliary_feed(feed);
        series_accepted = declare_timeframe_subscriptions(declared);
    }
};

void test_declaring_at_begin() {
    scenario = "declaring the feed at begin";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 60);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 4);

    // Feed first, series second: the run is the run a staged spec would make.
    {
        FeedHost staged;
        NativeRunSpec spec = base_spec("native-aux-begin");
        spec.auxiliary_feed = minute_feed(feed);
        spec.subscriptions.push_back(series("5", NativeSeriesSource::AuxiliaryFeed));
        if (!run_batch(staged, spec, inputs)) return;

        DeclaringHost host;
        host.feed = minute_feed(feed);
        host.declared = {series("5", NativeSeriesSource::AuxiliaryFeed)};
        if (!run_batch(host, base_spec("native-aux-begin"), inputs)) return;
        CHECK(host.feed_accepted);
        CHECK(host.series_accepted);
        check_deliveries_equal(host.deliveries, staged.deliveries);
        check_logs_equal(host.log, staged.log);
        // The staged spec names what ran, so the identity folds it.
        const auto view = host.native_state();
        CHECK(view.spec != nullptr && view.spec->auxiliary_feed.has_value());
        CHECK(host.native_continuation_hash() == staged.native_continuation_hash());
    }

    // Series first: the series names bars nobody has declared yet.
    {
        DeclaringHost host;
        host.series_first = true;
        host.feed = minute_feed(feed);
        host.declared = {series("5", NativeSeriesSource::AuxiliaryFeed)};
        if (!run_batch(host, base_spec("native-aux-begin-order"), inputs)) return;
        CHECK(!host.series_accepted);
        CHECK(host.feed_accepted);
        CHECK(host.deliveries.empty());
    }

    // A feed the input would refuse, and a withdrawal that would strand a
    // staged series, change nothing.
    {
        DeclaringHost host;
        host.feed = minute_feed(feed);
        host.feed->tf = "15";
        if (!run_batch(host, base_spec("native-aux-begin-refused"), inputs)) return;
        CHECK(!host.feed_accepted);
        const auto view = host.native_state();
        CHECK(view.spec != nullptr && !view.spec->auxiliary_feed.has_value());
    }
    {
        DeclaringHost host;
        host.feed = std::nullopt;
        host.declared = {series("5", NativeSeriesSource::AuxiliaryFeed)};
        NativeRunSpec spec = base_spec("native-aux-begin-strand");
        spec.auxiliary_feed = minute_feed(feed);
        spec.subscriptions = host.declared;
        if (!run_batch(host, spec, inputs)) return;
        CHECK(!host.feed_accepted);  // the staged "5" series still needs it
        CHECK(host.series_accepted);
        CHECK(host.deliveries.size() == 12);
    }

    // Outside on_native_run_begin both hooks answer false and stage nothing.
    {
        FeedHost host;
        CHECK(!host.declare_auxiliary_feed(minute_feed(feed)));
        CHECK(host.configure_native(base_spec("native-aux-begin-outside")).status
              == NativeSetupStatus::Applied);
        CHECK(!host.declare_auxiliary_feed(minute_feed(feed)));
        const auto view = host.native_state();
        CHECK(view.spec != nullptr && !view.spec->auxiliary_feed.has_value());
        // A batch has no live phase to append to.
        CHECK(!host.append_auxiliary_bars(feed.data(), feed.size()));
    }
}

// ---- 8. the continuation identity folds the feed --------------------------

void test_continuation_identity() {
    scenario = "continuation identity";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 60);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 4);

    const auto hash_of = [&inputs](const std::vector<Bar>& bars, const char* tf) {
        NativeRunSpec spec = base_spec("native-aux-identity");
        spec.auxiliary_feed = minute_feed(bars);
        spec.subscriptions.push_back(series(tf, NativeSeriesSource::AuxiliaryFeed));
        FeedHost host;
        run_batch(host, spec, inputs);
        return host.native_continuation_hash();
    };
    const std::uint64_t first = hash_of(feed, "5");
    CHECK(first == hash_of(feed, "5"));
    std::vector<Bar> moved = feed;
    moved[41].close += 0.01;
    CHECK(first != hash_of(moved, "5"));
    CHECK(first != hash_of(feed, "3"));
}

}  // namespace

int main() {
    test_finer_series_is_built_from_the_feed();
    test_the_feed_carries_what_the_input_does_not();
    test_history_before_the_first_input();
    test_hole_in_the_input_and_trailing_feed();
    test_short_feed_bucket_completes_lazily();
    test_lookahead_and_gaps();
    test_series_of_the_feed_timeframe_passes_through();
    test_validation();
    test_digests_are_neutral_and_sensitive();
    test_declaring_at_begin();
    test_continuation_identity();
    std::printf("test_native_auxiliary_feed: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
