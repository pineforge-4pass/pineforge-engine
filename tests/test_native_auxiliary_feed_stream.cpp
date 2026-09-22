// The auxiliary finer feed on a STREAM (stream_begin / append_auxiliary_bars /
// stream_push_bar / stream_end): the twin of test_native_auxiliary_feed.cpp.
// A forward-execution host reads the very series a backtest of the same bars
// and the same feed reads.
//
// Witnesses, all independent of the feature under test:
//   1. the warmup is a batch: stream_begin over N inputs and a feed produces
//      exactly the callbacks and buckets a run() over those N inputs and that
//      feed produces, under both publication modes;
//   2. live: the host appends each input's finer bars and then pushes the
//      input, and the whole stream is the batch over ALL the bars and the
//      WHOLE feed, bucket for bucket and delivery point for delivery point --
//      for a series finer than the input and for one coarser than it;
//   3. bars the begin-time feed already holds beyond the warmup are consumed
//      by the live inputs by the same rule (the replay form), no append needed;
//   4. every malformed append is refused by name, changes nothing and leaves
//      the host running: no declared feed, not realtime, out of order, not
//      after the feed's last bar, invalid OHLCV, and a bar that opened inside
//      an input period already accepted;
//   5. an append from inside a callback is the contract failure every
//      reentrant stream input is;
//   6. appended bars are durable input and are hashed: an append moves the
//      continuation hash, two streams fed alike agree on it, and a stream that
//      declares no feed keeps the hash it always had across a refused append;
//   7. a bucket still open when the stream ends is never delivered.

#include "native_auxiliary_feed_fixture.hpp"

namespace {

NativeRunSpec feed_spec(const char* session_key, const std::vector<Bar>& feed,
                        const char* tf, bool lookahead = false) {
    NativeRunSpec spec = base_spec(session_key);
    spec.auxiliary_feed = minute_feed(feed);
    spec.subscriptions.push_back(series(tf, NativeSeriesSource::AuxiliaryFeed, lookahead));
    return spec;
}

bool begin_stream(FeedHost& host, const NativeRunSpec& spec, const std::vector<Bar>& inputs,
                  int n_warmup) {
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const bool began = host.stream_begin(inputs.data(), n_warmup, "15", "15");
    CHECK(began);
    if (!began) std::printf("  error: %s\n", host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(host.stream_is_realtime());
    return began;
}

// The minutes [from, to) of a feed.
std::vector<Bar> slice(const std::vector<Bar>& feed, std::size_t from, std::size_t to) {
    return std::vector<Bar>(feed.begin() + static_cast<std::ptrdiff_t>(from),
                            feed.begin() + static_cast<std::ptrdiff_t>(to));
}

// Live input `i`: its fifteen finer bars first, then the confirmed bar.
bool push_live(FeedHost& host, const std::vector<Bar>& feed, const std::vector<Bar>& inputs,
               std::size_t i) {
    const std::vector<Bar> finer = slice(feed, i * 15, i * 15 + 15);
    const bool appended = host.append_auxiliary_bars(finer.data(), finer.size());
    CHECK(appended);
    if (!appended) std::printf("  append %zu error: %s\n", i, host.last_error().c_str());
    const bool pushed = host.stream_push_bar(inputs[i]);
    CHECK(pushed);
    if (!pushed) std::printf("  push %zu error: %s\n", i, host.last_error().c_str());
    return appended && pushed;
}

// ---- 1. the warmup is a batch of the same bars and the same feed ----------

void test_warmup_matches_batch(bool lookahead) {
    scenario = lookahead ? "warmup vs batch (lookahead_on)" : "warmup vs batch (lookahead_off)";
    const std::int64_t origin = origin_ms();
    // Ten inputs: two whole hours and a third left half open.
    const std::vector<Bar> feed = minute_bars(origin, 150);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 10);

    FeedHost batch;
    if (!run_batch(batch, feed_spec("native-aux-stream-batch", feed, "60", lookahead), inputs)) {
        return;
    }
    FeedHost stream;
    if (!begin_stream(stream, feed_spec("native-aux-stream-warmup", feed, "60", lookahead),
                      inputs, 10)) {
        return;
    }
    CHECK(batch.deliveries.size() == 2);
    check_logs_equal(stream.log, batch.log);
    check_deliveries_equal(stream.deliveries, batch.deliveries);
    for (std::size_t k = 0; k < stream.deliveries.size(); ++k) {
        check_bucket(stream.deliveries[k].bar,
                     hand_aggregate(feed, k * 60, 60,
                                    origin + static_cast<std::int64_t>(k) * kHour),
                     "warmup hour");
        CHECK(stream.deliveries[k].context.delivered_at_ms
              == inputs[k * 4 + (lookahead ? 0 : 3)].timestamp);
    }
    CHECK(stream.stream_end(false));
}

// ---- 2. live appends continue the series the warmup left open -------------

void test_live_appends_match_the_whole_batch(const char* tf, std::size_t want_deliveries) {
    scenario = tf[0] == '5' ? "live appends (finer series)" : "live appends (coarser series)";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 180);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 12);
    // Six inputs of warmup: the second hour is left half open.
    const int n_warmup = 6;

    FeedHost batch;
    if (!run_batch(batch, feed_spec("native-aux-live-batch", feed, tf), inputs)) return;
    CHECK(batch.deliveries.size() == want_deliveries);

    // The stream is handed the feed only as far as its warmup reaches.
    FeedHost stream;
    if (!begin_stream(stream, feed_spec("native-aux-live", slice(feed, 0, 90), tf), inputs,
                      n_warmup)) {
        return;
    }
    for (std::size_t i = n_warmup; i < inputs.size(); ++i) {
        if (!push_live(stream, feed, inputs, i)) return;
    }
    check_deliveries_equal(stream.deliveries, batch.deliveries);
    check_logs_equal(stream.log, batch.log);
    // And the buckets themselves, against the hand aggregation.
    const std::size_t width = tf[0] == '5' ? 5 : 60;
    for (std::size_t k = 0; k < stream.deliveries.size(); ++k) {
        check_bucket(stream.deliveries[k].bar,
                     hand_aggregate(feed, k * width, width,
                                    origin + static_cast<std::int64_t>(k * width) * kMinute),
                     "live bucket");
        CHECK(stream.deliveries[k].accessor_matches);
    }
    CHECK(stream.stream_end(false));
}

// ---- 3. a begin-time feed that outruns the warmup (the replay form) -------

void test_feed_beyond_the_warmup_is_consumed_live() {
    scenario = "begin-time feed beyond the warmup";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 120);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 8);

    FeedHost batch;
    if (!run_batch(batch, feed_spec("native-aux-replay-batch", feed, "5"), inputs)) return;
    FeedHost stream;
    if (!begin_stream(stream, feed_spec("native-aux-replay", feed, "5"), inputs, 3)) return;
    // The warmup folded its own three slices and nothing later.
    CHECK(stream.deliveries.size() == 9);
    for (std::size_t i = 3; i < inputs.size(); ++i) {
        const bool pushed = stream.stream_push_bar(inputs[i]);
        CHECK(pushed);
        if (!pushed) return;
    }
    check_deliveries_equal(stream.deliveries, batch.deliveries);
    check_logs_equal(stream.log, batch.log);
    CHECK(stream.stream_end(false));
}

// ---- 4. malformed appends are refused by name -----------------------------

void test_append_refusals() {
    scenario = "append refusals";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 60);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 4);

    // Not realtime: configured but never begun.
    {
        FeedHost host;
        CHECK(host.configure_native(feed_spec("native-aux-refuse-idle", slice(feed, 0, 30), "5"))
                  .status == NativeSetupStatus::Applied);
        CHECK(!host.append_auxiliary_bars(feed.data() + 30, 15));
        CHECK(host.last_error() == "native append_auxiliary_bars requires realtime");
        // The presentation text is not a value. The typed answer names the
        // same refusal, and the bool is its status (R5 gap lane E18,
        // E12 finding 5).
        const auto refusal = host.append_auxiliary_bars_result(feed.data() + 30, 15);
        CHECK(refusal.status == NativeSetupStatus::Failed);
        CHECK(refusal.error == NativeAuxiliaryAppendError::NotRealtime);
        CHECK(refusal.index == 0);
    }
    // No declared feed.
    {
        FeedHost host;
        CHECK(host.configure_native(base_spec("native-aux-refuse-undeclared")).status
              == NativeSetupStatus::Applied);
        CHECK(host.stream_begin(inputs.data(), 2, "15", "15"));
        CHECK(!host.append_auxiliary_bars(feed.data() + 30, 15));
        CHECK(host.last_error()
              == "native append_auxiliary_bars requires a declared auxiliary feed");
        const auto refusal = host.append_auxiliary_bars_result(feed.data() + 30, 15);
        CHECK(refusal.status == NativeSetupStatus::Failed);
        CHECK(refusal.error == NativeAuxiliaryAppendError::NoAuxiliaryFeed);
        CHECK(host.stream_push_bar(inputs[2]));
        CHECK(host.stream_end(false));
    }

    FeedHost host;
    if (!begin_stream(host, feed_spec("native-aux-refuse", slice(feed, 0, 30), "5"), inputs, 2)) {
        return;
    }
    const std::size_t delivered = host.deliveries.size();
    CHECK(delivered == 6);

    // An empty append is nothing, and says so.
    CHECK(host.append_auxiliary_bars(nullptr, 0));
    {
        const auto nothing = host.append_auxiliary_bars_result(nullptr, 0);
        CHECK(nothing.status == NativeSetupStatus::Applied);
        CHECK(nothing.error == NativeAuxiliaryAppendError::None);
    }
    CHECK(!host.append_auxiliary_bars(nullptr, 3));
    CHECK(host.last_error() == "native auxiliary bar array is invalid");
    {
        const auto refusal = host.append_auxiliary_bars_result(nullptr, 3);
        CHECK(refusal.error == NativeAuxiliaryAppendError::InvalidBarArray);
        CHECK(refusal.index == 0);
    }

    // Inside an input period already accepted: minute 29 again, and minute 12.
    CHECK(!host.append_auxiliary_bars(feed.data() + 29, 1));
    CHECK(host.last_error() == "native auxiliary bars must be strictly increasing");
    {
        const auto refusal = host.append_auxiliary_bars_result(feed.data() + 29, 1);
        CHECK(refusal.error == NativeAuxiliaryAppendError::UnorderedBars);
        CHECK(refusal.index == 0);
    }
    {
        // A host that never declared the warmup's last minutes cannot add
        // them afterwards: their slice is closed.
        FeedHost late;
        if (!begin_stream(late, feed_spec("native-aux-refuse-late", slice(feed, 0, 25), "5"),
                          inputs, 2)) {
            return;
        }
        CHECK(!late.append_auxiliary_bars(feed.data() + 25, 5));
        CHECK(late.last_error()
              == "native auxiliary bar opened inside an input period that was already accepted");
        {
            const auto refusal = late.append_auxiliary_bars_result(feed.data() + 25, 5);
            CHECK(refusal.error == NativeAuxiliaryAppendError::InputPeriodAlreadyAccepted);
            CHECK(refusal.index == 0);
        }
        CHECK(late.native_state().kind == NativeLifecycleKind::Running);
        CHECK(late.stream_end(false));
    }

    std::vector<Bar> unordered = slice(feed, 30, 45);
    std::swap(unordered[3], unordered[4]);
    CHECK(!host.append_auxiliary_bars(unordered.data(), unordered.size()));
    CHECK(host.last_error() == "native auxiliary bars must be strictly increasing");
    {
        // Which bar of THIS call: the one that did not follow its predecessor.
        const auto refusal =
            host.append_auxiliary_bars_result(unordered.data(), unordered.size());
        CHECK(refusal.error == NativeAuxiliaryAppendError::UnorderedBars);
        CHECK(refusal.index == 4);
    }

    std::vector<Bar> broken = slice(feed, 30, 45);
    broken[6].low = broken[6].high + 1.0;
    CHECK(!host.append_auxiliary_bars(broken.data(), broken.size()));
    CHECK(host.last_error() == "native auxiliary bar has invalid OHLCV");
    {
        const auto refusal = host.append_auxiliary_bars_result(broken.data(), broken.size());
        CHECK(refusal.error == NativeAuxiliaryAppendError::InvalidBar);
        CHECK(refusal.index == 6);
    }

    // None of that failed the host or reached the series: the good append and
    // the input it rides on deliver exactly the three buckets a batch would.
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);
    CHECK(host.deliveries.size() == delivered);
    // The good append that follows every refusal is Applied and names nothing.
    {
        FeedHost accepted;
        if (!begin_stream(accepted, feed_spec("native-aux-refuse-ok", slice(feed, 0, 30), "5"),
                          inputs, 2)) {
            return;
        }
        const auto applied = accepted.append_auxiliary_bars_result(feed.data() + 30, 15);
        CHECK(applied.status == NativeSetupStatus::Applied);
        CHECK(applied.error == NativeAuxiliaryAppendError::None);
        CHECK(applied.index == 0);
        CHECK(accepted.stream_end(false));
    }
    if (!push_live(host, feed, inputs, 2)) return;
    CHECK(host.deliveries.size() == delivered + 3);
    for (std::size_t k = 6; k < host.deliveries.size(); ++k) {
        check_bucket(host.deliveries[k].bar,
                     hand_aggregate(feed, k * 5, 5,
                                    origin + static_cast<std::int64_t>(k) * 5 * kMinute),
                     "bucket after the refusals");
    }
    CHECK(host.stream_end(false));
}

// ---- 5. an append from inside a callback is a contract failure ------------

class ReentrantHost final : public FeedHost {
public:
    std::vector<Bar> more;
    bool answer = true;
    NativeAuxiliaryAppendResult reentrant_result;
    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        FeedHost::on_native_input(bar, context);
        if (context.input_index != 2) return;
        reentrant_result = append_auxiliary_bars_result(more.data(), more.size());
        answer = reentrant_result.status == NativeSetupStatus::Applied;
    }
};

void test_append_from_a_callback_fails_the_run() {
    scenario = "append from a callback";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 60);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 4);

    ReentrantHost host;
    host.more = slice(feed, 45, 60);
    if (!begin_stream(host, feed_spec("native-aux-reentrant", slice(feed, 0, 45), "5"), inputs,
                      2)) {
        return;
    }
    CHECK(!host.stream_push_bar(inputs[2]));
    CHECK(!host.answer);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
    // The two refusals the bool folded together: the call that CAUSED the
    // contract failure, and every call on the host it left failed.
    CHECK(host.reentrant_result.error == NativeAuxiliaryAppendError::Reentrant);
    const auto after = host.append_auxiliary_bars_result(host.more.data(), host.more.size());
    CHECK(after.status == NativeSetupStatus::Failed);
    CHECK(after.error == NativeAuxiliaryAppendError::HostFailed);
}

// ---- 6. appended bars are hashed ------------------------------------------

void test_appended_bars_are_hashed() {
    scenario = "appended bars are hashed";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 60);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 4);

    FeedHost first;
    FeedHost second;
    if (!begin_stream(first, feed_spec("native-aux-hash", slice(feed, 0, 30), "5"), inputs, 2)
        || !begin_stream(second, feed_spec("native-aux-hash", slice(feed, 0, 30), "5"), inputs,
                         2)) {
        return;
    }
    CHECK(first.native_continuation_hash() == second.native_continuation_hash());

    // The append alone -- no input accepted yet -- is durable state.
    const std::uint64_t before = first.native_continuation_hash();
    CHECK(first.append_auxiliary_bars(feed.data() + 30, 15));
    CHECK(first.native_continuation_hash() != before);
    CHECK(first.native_continuation_hash() != second.native_continuation_hash());
    CHECK(second.append_auxiliary_bars(feed.data() + 30, 15));
    CHECK(first.native_continuation_hash() == second.native_continuation_hash());

    // Different bars, different identity.
    std::vector<Bar> moved = slice(feed, 45, 60);
    moved[7].close += 0.01;
    CHECK(first.stream_push_bar(inputs[2]));
    CHECK(second.stream_push_bar(inputs[2]));
    CHECK(first.native_continuation_hash() == second.native_continuation_hash());
    CHECK(first.append_auxiliary_bars(feed.data() + 45, 15));
    CHECK(second.append_auxiliary_bars(moved.data(), moved.size()));
    CHECK(first.native_continuation_hash() != second.native_continuation_hash());
    CHECK(first.stream_end(false));
    CHECK(second.stream_end(false));

    // A stream that declares no feed is untouched by a refused append.
    FeedHost plain;
    CHECK(plain.configure_native(base_spec("native-aux-hash-plain")).status
          == NativeSetupStatus::Applied);
    CHECK(plain.stream_begin(inputs.data(), 2, "15", "15"));
    const std::uint64_t plain_before = plain.native_continuation_hash();
    CHECK(!plain.append_auxiliary_bars(feed.data() + 30, 15));
    CHECK(plain.native_continuation_hash() == plain_before);
    CHECK(plain.stream_end(false));
}

// ---- 7. an open bucket is not delivered by stream_end ---------------------

void test_open_bucket_is_not_delivered() {
    scenario = "open bucket at stream end";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> feed = minute_bars(origin, 90);
    const std::vector<Bar> inputs = quarter_bars_over(feed, 6);

    FeedHost stream;
    if (!begin_stream(stream, feed_spec("native-aux-open", slice(feed, 0, 60), "60"), inputs, 4)) {
        return;
    }
    CHECK(stream.deliveries.size() == 1);
    if (!push_live(stream, feed, inputs, 4) || !push_live(stream, feed, inputs, 5)) return;
    CHECK(stream.stream_end(false));
    // The second hour holds thirty minutes and was never complete.
    CHECK(stream.deliveries.size() == 1);
}

}  // namespace

int main() {
    test_warmup_matches_batch(false);
    test_warmup_matches_batch(true);
    test_live_appends_match_the_whole_batch("5", 36);
    test_live_appends_match_the_whole_batch("60", 3);
    test_feed_beyond_the_warmup_is_consumed_live();
    test_append_refusals();
    test_append_from_a_callback_fails_the_run();
    test_appended_bars_are_hashed();
    test_open_bucket_is_not_delivered();
    std::printf("test_native_auxiliary_feed_stream: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
