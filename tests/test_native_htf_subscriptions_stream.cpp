// Declared higher-timeframe series on a STREAM (NativeRunSpec::subscriptions
// through stream_begin / stream_push_bar / stream_end), so a forward-execution
// host reads the same series a backtest of the same bars reads.
//
// Witnesses, all independent of the feature under test:
//   1. the warmup is a batch: a stream_begin over N bars produces exactly the
//      callback sequence and the exact buckets a run() over those same N bars
//      produces, compared element by element (both publication modes);
//   2. live pushes continue the very bucket the warmup left open and deliver
//      it before the calculation of the bar that completed it;
//   3. a bucket still open when the stream ends is never delivered;
//   4. native_series_bar answers with the latest delivered bucket on both
//      sides of the warmup -> live boundary;
//   5. a stream whose spec declares no series keeps this tree's event
//      sequence and folds nothing new into its run-spec digest (the portable
//      half of the continuation identity), with the continuation and stream
//      state hashes compared between two streams in this process;
//   6. a session-clipped daily series (a calendar aggregator, whose buckets
//      close on the session close and not on a bar count) over a stream whose
//      warmup stops mid-session is the batch's series, bucket for bucket;
//   7. `authoritative_bars` are installed once, at begin: they cover the
//      buckets the warmup completes, and a live bucket with none of its own
//      aggregates the pushed input and is counted as a feed miss;
//   8. a stream that declares a series takes confirmed bars only: tick input
//      is refused by name without failing the host, and a stream that
//      declares none still takes ticks;
//   9. a feed installed through the engine's own setter -- the store the C ABI
//      writes -- is host ingress and survives a second stream_begin, which
//      re-registers the kernel's own series without taking it away;
//  10. the pump is ordered against the SCRIPT interval on a stream too: the
//      last "60" script bar of a 09:30-16:00 session is clipped short of its
//      nominal end, so the NEXT session's first bar seals it lazily -- once
//      inside the warmup, once live -- and that calculation reads the series
//      as its own session left them, before the new session's first bar
//      reaches any of them (seal(k) -> deliver(i+1) -> calc(i+1)).

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

int checks = 0;
int failures = 0;
const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expression)) {                                                   \
            ++failures;                                                        \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,         \
                        #expression);                                          \
        }                                                                      \
    } while (false)

bool same(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
}

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
std::int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
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

NativeRunSpec base_spec(const char* input, const char* script,
                        const char* session_key) {
    NativeRunSpec spec;
    spec.identity = {session_key, 1};
    spec.input_tf = input;
    spec.script_tf = script;
    spec.ticker = "SERIES";
    spec.tickerid = "TEST:SERIES";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native higher-timeframe subscription fixture";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// ---- hosts ----------------------------------------------------------------

struct Delivery {
    std::size_t subscription = 0;
    Bar bar{};
    NativeTimeframeBarContext context{};
    // How many script bars the host had already calculated when the bucket
    // arrived: the 0-based index of the bar it is delivered on.
    int bars_before = 0;
    // What native_series_bar answered inside the callback.
    bool accessor_matches = false;
};

class SeriesHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    std::vector<Delivery> deliveries;
    int bars_seen = 0;
    // Optional per-calculation probe: what native_series_bar(i) answered at
    // each script bar, for every i < probes. Zero -- the default -- records
    // nothing, so every scenario that does not ask for it is unchanged.
    std::size_t probes = 0;
    std::vector<std::vector<std::optional<Bar>>> series_at_bar;
    // How each calculated script bar was sealed: Confirmed by an input of
    // its own, LazyComplete by the first input of a later interval.
    std::vector<NativeCompletionKind> completions;

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
        delivery.bars_before = bars_seen;
        const auto pulled = native_series_bar(context.subscription);
        delivery.accessor_matches = pulled.has_value()
            && pulled->timestamp == bar.timestamp && same(pulled->open, bar.open)
            && same(pulled->high, bar.high) && same(pulled->low, bar.low)
            && same(pulled->close, bar.close) && same(pulled->volume, bar.volume);
        deliveries.push_back(delivery);
        log.push_back("htf:" + std::to_string(context.subscription) + "@"
                      + std::to_string(bar.timestamp));
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        ++bars_seen;
        if (probes > 0) {
            std::vector<std::optional<Bar>> row;
            row.reserve(probes);
            for (std::size_t i = 0; i < probes; ++i) row.push_back(native_series_bar(i));
            series_at_bar.push_back(std::move(row));
        }
        completions.push_back(context.coordinate.completion);
        log.push_back("bar@" + std::to_string(bar.timestamp));
    }
};

// ---- fixtures -------------------------------------------------------------

std::vector<Bar> quarter_hour_bars(int n) {
    const std::int64_t origin = utc_ms(2024, 1, 1);
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double open = 100.0 + i;
        Bar bar{};
        bar.open = open;
        bar.high = open + 2.0;
        bar.low = open - 1.0;
        bar.close = open + 0.5;
        bar.volume = static_cast<double>(i + 1);
        bar.timestamp = origin + static_cast<std::int64_t>(i) * kQuarter;
        bars.push_back(bar);
    }
    return bars;
}

// The oracle: a hand aggregation of one contiguous group of input bars.
Bar hand_aggregate(const std::vector<Bar>& bars, int from, int count,
                   std::int64_t label) {
    Bar out = bars[static_cast<std::size_t>(from)];
    out.timestamp = label;
    for (int i = 1; i < count; ++i) {
        const Bar& next = bars[static_cast<std::size_t>(from + i)];
        out.high = std::max(out.high, next.high);
        out.low = std::min(out.low, next.low);
        out.close = next.close;
        out.volume += next.volume;
    }
    return out;
}

bool bars_equal(const Bar& got, const Bar& want) {
    return got.timestamp == want.timestamp && same(got.open, want.open)
        && same(got.high, want.high) && same(got.low, want.low)
        && same(got.close, want.close) && same(got.volume, want.volume);
}

void check_bucket(const Bar& got, const Bar& want, const char* tag) {
    const bool ok = bars_equal(got, want);
    if (!ok) {
        std::printf("  %s: got t %lld o %.6g h %.6g l %.6g c %.6g v %.6g; "
                    "want t %lld o %.6g h %.6g l %.6g c %.6g v %.6g\n",
                    tag, static_cast<long long>(got.timestamp), got.open, got.high,
                    got.low, got.close, got.volume,
                    static_cast<long long>(want.timestamp), want.open, want.high,
                    want.low, want.close, want.volume);
    }
    CHECK(ok);
}

void check_logs_equal(const std::vector<std::string>& got,
                      const std::vector<std::string>& want) {
    CHECK(got == want);
    if (got == want) return;
    for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
        const std::string a = i < got.size() ? got[i] : std::string("-");
        const std::string b = i < want.size() ? want[i] : std::string("-");
        if (a != b) std::printf("  log[%zu]: got %s want %s\n", i, a.c_str(), b.c_str());
    }
}

// Every delivery field the host can observe, compared one by one.
void check_deliveries_equal(const std::vector<Delivery>& got,
                            const std::vector<Delivery>& want) {
    CHECK(got.size() == want.size());
    if (got.size() != want.size()) {
        std::printf("  %zu deliveries, want %zu\n", got.size(), want.size());
        return;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        check_bucket(got[i].bar, want[i].bar, "stream vs batch bucket");
        CHECK(got[i].subscription == want[i].subscription);
        CHECK(got[i].bars_before == want[i].bars_before);
        CHECK(got[i].accessor_matches == want[i].accessor_matches);
        CHECK(got[i].context.delivered_at_ms == want[i].context.delivered_at_ms);
        CHECK(got[i].context.completion == want[i].context.completion);
        CHECK(got[i].context.interval.open_ms == want[i].context.interval.open_ms);
        CHECK(got[i].context.interval.next_period_open_ms
              == want[i].context.interval.next_period_open_ms);
    }
}

NativeRunSpec hourly_spec(const char* session_key, bool lookahead) {
    NativeRunSpec spec = base_spec("15", "15", session_key);
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    hourly.lookahead = lookahead;
    spec.subscriptions.push_back(hourly);
    return spec;
}

// A batch of the first `n_warmup` bars: the oracle every stream warmup below
// is compared against.
void run_batch(SeriesHost& host, const std::vector<Bar>& bars, int n_warmup,
               const char* session_key, bool lookahead) {
    CHECK(host.configure_native(hourly_spec(session_key, lookahead)).status
          == NativeSetupStatus::Applied);
    host.run(bars.data(), n_warmup, "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
}

bool begin_stream(SeriesHost& host, const std::vector<Bar>& bars, int n_warmup,
                  const char* session_key, bool lookahead) {
    CHECK(host.configure_native(hourly_spec(session_key, lookahead)).status
          == NativeSetupStatus::Applied);
    const bool began = host.stream_begin(bars.data(), n_warmup, "15", "15");
    CHECK(began);
    if (!began) std::printf("  error: %s\n", host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(host.stream_is_realtime());
    return began;
}

// ---- 1. the warmup is a batch of the same bars ----------------------------

void test_warmup_matches_batch(bool lookahead) {
    scenario = lookahead ? "warmup vs batch (lookahead_on)"
                         : "warmup vs batch (lookahead_off)";
    // Ten bars: two whole hourly buckets and a third left half open, so the
    // comparison covers a warmup that does not end on a bucket boundary.
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const int n_warmup = 10;

    SeriesHost batch;
    run_batch(batch, bars, n_warmup, "native-htf-stream-batch", lookahead);

    SeriesHost stream;
    if (!begin_stream(stream, bars, n_warmup, "native-htf-stream-warmup", lookahead)) return;

    CHECK(batch.deliveries.size() == 2);
    check_logs_equal(stream.log, batch.log);
    check_deliveries_equal(stream.deliveries, batch.deliveries);

    // The buckets themselves, against the hand aggregation.
    const std::int64_t origin = bars.front().timestamp;
    for (std::size_t k = 0; k < stream.deliveries.size(); ++k) {
        const Bar want = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                        origin + static_cast<std::int64_t>(k) * kHour);
        check_bucket(stream.deliveries[k].bar, want, "warmup bucket");
        CHECK(stream.deliveries[k].accessor_matches);
        CHECK(stream.deliveries[k].context.completion == NativeCompletionKind::Confirmed);
        // lookahead_off publishes on the group's 4th bar, lookahead_on on its
        // first; both are historical resolution over the warmup input.
        CHECK(stream.deliveries[k].context.delivered_at_ms
              == bars[k * 4 + (lookahead ? 0 : 3)].timestamp);
    }
}

// ---- 2. live pushes continue the bucket the warmup left open --------------

void test_live_pushes_extend_buckets(bool lookahead) {
    scenario = lookahead ? "live pushes (lookahead_on)" : "live pushes (lookahead_off)";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const int n_warmup = 10;

    SeriesHost host;
    if (!begin_stream(host, bars, n_warmup, "native-htf-stream-live", lookahead)) return;
    const std::size_t warmup_deliveries = host.deliveries.size();
    CHECK(warmup_deliveries == 2);

    // Bars 10 and 11 complete the bucket bars 8 and 9 opened during the
    // warmup; bars 12..15 are a wholly live bucket.
    for (int i = n_warmup; i < 16; ++i) {
        const bool pushed = host.stream_push_bar(bars[static_cast<std::size_t>(i)]);
        CHECK(pushed);
        if (!pushed) {
            std::printf("  push %d error: %s\n", i, host.last_error().c_str());
            return;
        }
    }
    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() != 4) return;

    const std::int64_t origin = bars.front().timestamp;
    for (std::size_t k = 2; k < 4; ++k) {
        const Delivery& delivery = host.deliveries[k];
        const Bar want = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                        origin + static_cast<std::int64_t>(k) * kHour);
        check_bucket(delivery.bar, want, "live bucket");
        CHECK(delivery.subscription == 0);
        CHECK(delivery.accessor_matches);
        CHECK(delivery.context.completion == NativeCompletionKind::Confirmed);
        // A live bucket is delivered on the pushed bar that completed it --
        // the group's 4th -- under BOTH publication modes: a realtime bar has
        // no future for lookahead_on to resolve over.
        CHECK(delivery.context.delivered_at_ms == bars[k * 4 + 3].timestamp);
        // Delivered before that bar's calculation: exactly 4k+3 script bars
        // had been calculated when it arrived.
        CHECK(delivery.bars_before == static_cast<int>(k) * 4 + 3);
        // The bucket's own calendar span, from its first contributing bar --
        // bar 8, which the warmup accepted, for the straddling bucket.
        CHECK(delivery.context.interval.open_ms
              == origin + static_cast<std::int64_t>(k) * kHour);
        CHECK(delivery.context.interval.next_period_open_ms
              == origin + static_cast<std::int64_t>(k + 1) * kHour);
    }

    // The live sequence itself: input, then the bucket, then the calculation.
    std::vector<std::string> want_tail;
    for (int i = n_warmup; i < 16; ++i) {
        want_tail.push_back("input:" + std::to_string(i) + "@"
                            + std::to_string(bars[static_cast<std::size_t>(i)].timestamp));
        if (i % 4 == 3) {
            want_tail.push_back("htf:0@" + std::to_string(
                origin + static_cast<std::int64_t>(i / 4) * kHour));
        }
        want_tail.push_back("bar@"
            + std::to_string(bars[static_cast<std::size_t>(i)].timestamp));
    }
    CHECK(host.log.size() >= want_tail.size());
    if (host.log.size() < want_tail.size()) return;
    const std::vector<std::string> tail(host.log.end()
                                            - static_cast<std::ptrdiff_t>(want_tail.size()),
                                        host.log.end());
    check_logs_equal(tail, want_tail);
}

// ---- 3. a partial live bucket is never delivered --------------------------

void test_partial_live_bucket_is_not_delivered() {
    scenario = "partial live bucket";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const int n_warmup = 10;

    SeriesHost host;
    if (!begin_stream(host, bars, n_warmup, "native-htf-stream-partial", false)) return;
    // Through bar 13: the 12..15 bucket is two bars in and still open.
    for (int i = n_warmup; i < 14; ++i) {
        CHECK(host.stream_push_bar(bars[static_cast<std::size_t>(i)]));
    }
    CHECK(host.deliveries.size() == 3);
    const std::size_t before_end = host.deliveries.size();
    CHECK(host.stream_end(false));
    CHECK(host.deliveries.size() == before_end);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    // The last delivered bucket is still the completed one, never the partial.
    const std::int64_t origin = bars.front().timestamp;
    const auto latest = host.native_series_bar(0);
    CHECK(latest.has_value());
    if (latest) CHECK(latest->timestamp == origin + 2 * kHour);
}

// ---- 4. native_series_bar across the warmup -> live boundary --------------

void test_series_bar_across_the_boundary() {
    scenario = "series accessor across the boundary";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const int n_warmup = 10;
    const std::int64_t origin = bars.front().timestamp;

    SeriesHost host;
    if (!begin_stream(host, bars, n_warmup, "native-htf-stream-accessor", false)) return;

    const Bar hour1 = hand_aggregate(bars, 4, 4, origin + kHour);
    const Bar hour2 = hand_aggregate(bars, 8, 4, origin + 2 * kHour);

    // End of the warmup: the last bucket the warmup completed.
    auto pulled = host.native_series_bar(0);
    CHECK(pulled.has_value());
    if (pulled) check_bucket(*pulled, hour1, "after warmup");
    // An index nobody declared has no series at all.
    CHECK(!host.native_series_bar(1).has_value());

    // The first live bar contributes to the open bucket without completing it.
    CHECK(host.stream_push_bar(bars[10]));
    pulled = host.native_series_bar(0);
    CHECK(pulled.has_value());
    if (pulled) check_bucket(*pulled, hour1, "after the first live bar");

    // The bar that completes it moves the accessor, and only then.
    CHECK(host.stream_push_bar(bars[11]));
    pulled = host.native_series_bar(0);
    CHECK(pulled.has_value());
    if (pulled) check_bucket(*pulled, hour2, "after the completing live bar");

    CHECK(host.stream_push_bar(bars[12]));
    pulled = host.native_series_bar(0);
    CHECK(pulled.has_value());
    if (pulled) check_bucket(*pulled, hour2, "after the next live bar");

    CHECK(host.stream_end(false));
    pulled = host.native_series_bar(0);
    CHECK(pulled.has_value());
    if (pulled) check_bucket(*pulled, hour2, "after stream_end");
}

// ---- 5. a stream without subscriptions is unchanged -----------------------

// The portable pin of that identity. Neither native_continuation_hash() nor
// stream_state_hash() — which folds broker_state_hash(), and so the
// continuation — can be pinned as a constant: both carry the machine's
// resolved timezone resources (zoneinfo root and zone file paths), so the same
// stream hashes differently here and on each CI runner.
// native_run_spec_digest() is exactly the consumer's run-spec fold and nothing
// else, so it is the same number everywhere. Observed on THIS tree for
// base_spec("15", "15", "native-htf-stream-neutral"); it guards the fold's
// field list and order, while the neutrality itself is the event sequence and
// the in-process equalities below.
constexpr std::uint64_t kNeutralStreamSpecDigest = 10367175860638888234ull;

void test_stream_without_subscriptions_is_unchanged() {
    scenario = "stream neutrality";
    const std::vector<Bar> bars = quarter_hour_bars(12);
    NativeRunSpec spec = base_spec("15", "15", "native-htf-stream-neutral");
    CHECK(spec.subscriptions.empty());

    SeriesHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(host.stream_begin(bars.data(), 8, "15", "15"));
    for (int i = 8; i < 12; ++i) {
        CHECK(host.stream_push_bar(bars[static_cast<std::size_t>(i)]));
    }
    CHECK(host.stream_end(false));
    CHECK(host.last_error().empty());
    CHECK(host.bars_seen == 12);
    CHECK(host.deliveries.empty());

    // The event sequence: input then calculation, with nothing between them.
    std::vector<std::string> want_log;
    for (int i = 0; i < 12; ++i) {
        const std::string stamp =
            std::to_string(bars[static_cast<std::size_t>(i)].timestamp);
        want_log.push_back("input:" + std::to_string(i) + "@" + stamp);
        want_log.push_back("bar@" + stamp);
    }
    check_logs_equal(host.log, want_log);

    const std::uint64_t digest = native_run_spec_digest(spec);
    if (digest != kNeutralStreamSpecDigest) {
        std::printf("  spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kNeutralStreamSpecDigest));
    }
    CHECK(digest == kNeutralStreamSpecDigest);

    // Stating the field at its default — an empty series list — folds nothing.
    NativeRunSpec stated = spec;
    stated.subscriptions.clear();
    CHECK(native_run_spec_digest(stated) == kNeutralStreamSpecDigest);

    // The run's own identity is compared between two streams in this process,
    // never against a constant: the same stream under the spec with its empty
    // series list stated outright reaches the same event sequence, the same
    // continuation identity and the same stream state.
    const std::uint64_t continuation = host.native_continuation_hash();
    const std::uint64_t stream_state = host.stream_state_hash();
    SeriesHost restated;
    CHECK(restated.configure_native(stated).status == NativeSetupStatus::Applied);
    CHECK(restated.stream_begin(bars.data(), 8, "15", "15"));
    for (int i = 8; i < 12; ++i) {
        CHECK(restated.stream_push_bar(bars[static_cast<std::size_t>(i)]));
    }
    CHECK(restated.stream_end(false));
    CHECK(restated.last_error().empty());
    CHECK(restated.bars_seen == 12);
    CHECK(restated.deliveries.empty());
    check_logs_equal(restated.log, want_log);
    CHECK(restated.native_continuation_hash() == continuation);
    CHECK(restated.stream_state_hash() == stream_state);

    // Declaring a series cannot land on the same run-spec fold, nor on the
    // same continuation.
    CHECK(native_run_spec_digest(hourly_spec("native-htf-stream-neutral", false))
          != kNeutralStreamSpecDigest);
    SeriesHost declared;
    if (begin_stream(declared, bars, 8, "native-htf-stream-neutral", false)) {
        for (int i = 8; i < 12; ++i) {
            CHECK(declared.stream_push_bar(bars[static_cast<std::size_t>(i)]));
        }
        CHECK(declared.stream_end(false));
        CHECK(declared.native_continuation_hash() != continuation);
    }
}

// ---- 6. a session-clipped calendar series across the boundary -------------

// Three 09:30-16:00 New York sessions of 15-minute bars: 26 per session, the
// last one closing on the session close rather than on any nominal period end.
std::vector<Bar> session_bars(int sessions) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(sessions) * 26u);
    for (int day = 0; day < sessions; ++day) {
        // 09:30 New York is 14:30 UTC on these January dates.
        const std::int64_t open = utc_ms(2024, 1, 2 + day, 14, 30);
        for (int i = 0; i < 26; ++i) {
            const double price = 100.0 + day + i * 0.25;
            Bar bar{};
            bar.open = price;
            bar.high = price + 2.0;
            bar.low = price - 1.0;
            bar.close = price + 0.5;
            bar.volume = 1.0;
            bar.timestamp = open + static_cast<std::int64_t>(i) * kQuarter;
            bars.push_back(bar);
        }
    }
    return bars;
}

NativeRunSpec daily_spec(const char* session_key) {
    NativeRunSpec spec = base_spec("15", "15", session_key);
    spec.type = "stock";
    spec.timezone = "America/New_York";
    spec.session = "0930-1600";
    NativeTimeframeSubscription daily;
    daily.tf = "D";
    spec.subscriptions.push_back(daily);
    return spec;
}

void test_session_calendar_series_across_the_boundary() {
    scenario = "session-clipped daily across the boundary";
    const std::vector<Bar> bars = session_bars(3);
    // Mid second session: the day that straddles the warmup -> live boundary
    // is neither the first nor the last.
    const int n_warmup = 26 + 13;

    SeriesHost batch;
    CHECK(batch.configure_native(daily_spec("native-htf-stream-daily-batch")).status
          == NativeSetupStatus::Applied);
    batch.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(batch.last_error().empty());
    if (!batch.last_error().empty()) std::printf("  error: %s\n", batch.last_error().c_str());
    CHECK(batch.deliveries.size() == 3);

    SeriesHost stream;
    CHECK(stream.configure_native(daily_spec("native-htf-stream-daily-live")).status
          == NativeSetupStatus::Applied);
    const bool began = stream.stream_begin(bars.data(), n_warmup, "15", "15");
    CHECK(began);
    if (!began) {
        std::printf("  error: %s\n", stream.last_error().c_str());
        return;
    }
    CHECK(stream.deliveries.size() == 1);
    for (int i = n_warmup; i < static_cast<int>(bars.size()); ++i) {
        const bool pushed = stream.stream_push_bar(bars[static_cast<std::size_t>(i)]);
        CHECK(pushed);
        if (!pushed) {
            std::printf("  push %d error: %s\n", i, stream.last_error().c_str());
            return;
        }
    }
    // A calendar bucket completes on its own session close, which the stream
    // reaches on its own: the whole three-day series is the batch's, bucket
    // for bucket and delivery point for delivery point, straddling day and all.
    check_deliveries_equal(stream.deliveries, batch.deliveries);
    check_logs_equal(stream.log, batch.log);
    for (const Delivery& delivery : stream.deliveries) {
        CHECK(delivery.context.completion == NativeCompletionKind::Confirmed);
        // One unit of volume per contributing bar: a whole 26-bar session.
        CHECK(same(delivery.bar.volume, 26.0));
    }
    CHECK(stream.stream_end(false));
}

// ---- 7. authoritative bars cover the warmup only --------------------------

void test_authoritative_bars_cover_the_warmup() {
    scenario = "authoritative bars over a stream";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const std::int64_t origin = bars.front().timestamp;
    const int n_warmup = 10;

    // The exchange's own bars for the two hours the warmup completes. They are
    // installed once, at begin, so the hours the live phase completes have
    // none and aggregate the pushed input instead.
    std::vector<Bar> exchange;
    for (int k = 0; k < 2; ++k) {
        Bar bar{};
        bar.open = 1000.0 + k;
        bar.high = 1010.0 + k;
        bar.low = 990.0 + k;
        bar.close = 1005.0 + k;
        bar.volume = 7777.0 + k;
        bar.timestamp = origin + static_cast<std::int64_t>(k) * kHour;
        exchange.push_back(bar);
    }

    NativeRunSpec spec = base_spec("15", "15", "native-htf-stream-feed");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    hourly.authoritative_bars = exchange;
    spec.subscriptions.push_back(hourly);

    SeriesHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const bool began = host.stream_begin(bars.data(), n_warmup, "15", "15");
    CHECK(began);
    if (!began) {
        std::printf("  error: %s\n", host.last_error().c_str());
        return;
    }
    CHECK(host.deliveries.size() == 2);
    if (host.deliveries.size() != 2) return;
    for (std::size_t k = 0; k < 2; ++k) {
        check_bucket(host.deliveries[k].bar, exchange[k], "warmup exchange bucket");
    }
    CHECK(host.native_security_substitutions() == 2);
    CHECK(host.native_security_misses() == 0);

    for (int i = n_warmup; i < 16; ++i) {
        CHECK(host.stream_push_bar(bars[static_cast<std::size_t>(i)]));
    }
    CHECK(host.deliveries.size() == 4);
    if (host.deliveries.size() != 4) return;
    for (std::size_t k = 2; k < 4; ++k) {
        const Bar aggregate = hand_aggregate(bars, static_cast<int>(k) * 4, 4,
                                             origin + static_cast<std::int64_t>(k) * kHour);
        check_bucket(host.deliveries[k].bar, aggregate, "live aggregated bucket");
    }
    // Still the warmup's two substitutions; the live buckets are counted as
    // the misses they are, against an installed feed that does not reach them.
    CHECK(host.native_security_substitutions() == 2);
    CHECK(host.native_security_misses() == 2);
    CHECK(host.stream_end(false));
}

// ---- 8. a subscribed stream takes confirmed bars only ---------------------

void test_tick_input_is_refused_while_subscribed() {
    scenario = "tick input while subscribed";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const int n_warmup = 10;

    SeriesHost host;
    if (!begin_stream(host, bars, n_warmup, "native-htf-stream-ticks", false)) return;

    TradeTick tick{};
    tick.timestamp = bars[10].timestamp + kMinute;
    tick.sequence = 1;
    tick.price = bars[10].close;
    tick.quantity = 1.0;
    CHECK(!host.stream_push_tick(tick));
    CHECK(host.last_error()
          == "native timeframe subscriptions require confirmed-bar stream input");
    // A refusal, not a failure: the host is still running its stream.
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);
    CHECK(host.stream_is_realtime());

    CHECK(!host.stream_advance_time(bars[11].timestamp));
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);

    // Confirmed bars still work afterwards, and the series still completes.
    for (int i = n_warmup; i < 12; ++i) {
        CHECK(host.stream_push_bar(bars[static_cast<std::size_t>(i)]));
    }
    CHECK(host.deliveries.size() == 3);
    CHECK(host.stream_end(false));

    // A stream that declares no series is untouched by that rule.
    SeriesHost plain;
    NativeRunSpec spec = base_spec("15", "15", "native-htf-stream-plain-ticks");
    CHECK(plain.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(plain.stream_begin(bars.data(), n_warmup, "15", "15"));
    CHECK(plain.stream_push_tick(tick));
    CHECK(plain.last_error().empty());
    CHECK(plain.stream_end(true));
}

// ---- 9. a feed installed through the engine's own setter survives --------
//
// strategy_set_native_security_feed writes BacktestEngine's native security
// feed store before the run, and nothing in the spec names it. A later begin
// re-registers the kernel's own series, and it must not take that host
// ingress with it.

void test_installed_feed_survives_a_second_begin() {
    scenario = "installed feed across two stream begins";
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const std::int64_t origin = bars.front().timestamp;
    const int n_warmup = 10;

    std::vector<Bar> exchange;
    for (int k = 0; k < 4; ++k) {
        Bar bar{};
        bar.open = 2000.0 + k;
        bar.high = 2010.0 + k;
        bar.low = 1990.0 + k;
        bar.close = 2005.0 + k;
        bar.volume = 4242.0 + k;
        bar.timestamp = origin + static_cast<std::int64_t>(k) * kHour;
        exchange.push_back(bar);
    }

    SeriesHost host;
    // Host ingress, installed once and never named by a spec: the same store
    // the C ABI's strategy_set_native_security_feed writes.
    CHECK(host.set_native_security_feed("60", exchange.data(),
                                        static_cast<int>(exchange.size())));

    NativeRunSpec first = base_spec("15", "15", "native-htf-stream-installed");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";  // no authoritative_bars of its own
    first.subscriptions.push_back(hourly);
    CHECK(host.configure_native(first).status == NativeSetupStatus::Applied);
    CHECK(host.stream_begin(bars.data(), n_warmup, "15", "15"));
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    CHECK(host.deliveries.size() == 2);
    if (host.deliveries.size() == 2) {
        check_bucket(host.deliveries[0].bar, exchange[0], "first begin exchange bucket");
        check_bucket(host.deliveries[1].bar, exchange[1], "first begin exchange bucket");
    }
    CHECK(host.native_security_substitutions() == 2);
    CHECK(host.stream_end(false));

    // The second begin on the same host re-registers the kernel's series and
    // leaves the installed feed where it is.
    host.deliveries.clear();
    host.log.clear();
    NativeRunSpec second = first;
    second.identity = {"native-htf-stream-installed", 2};
    CHECK(host.configure_native(second).status == NativeSetupStatus::Applied);
    CHECK(host.stream_begin(bars.data(), n_warmup, "15", "15"));
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    CHECK(host.deliveries.size() == 2);
    if (host.deliveries.size() == 2) {
        check_bucket(host.deliveries[0].bar, exchange[0], "second begin exchange bucket");
        check_bucket(host.deliveries[1].bar, exchange[1], "second begin exchange bucket");
    }
    CHECK(host.native_security_substitutions() == 2);
    CHECK(host.stream_end(false));
}

// ---- 10. the pump is ordered against the script interval ------------------
//
// Input "15", script "60", a 09:30-16:00 New York session: seven script bars a
// day, the seventh -- 15:30 -- holding two inputs and clipped by the close
// half an hour short of its nominal end. No input of its own seals it; the
// NEXT session's 09:30 bar does, LazyComplete (the LAZY SEAL). No feed hole is
// involved: this is every session-clipped aggregated chart. Three series ride
// on that 09:30 bar:
//   0. "15": the input's own timeframe, so its bucket IS each input bar and
//      completes on it -- the bucket holding the very input that seals 15:30;
//   1. "60" gaps_on: the 15:30 bucket completed on 15:45 (the session close);
//      09:30 opens a new bucket and delivers nothing, so 09:30 CLEARS it;
//   2. "60": the same series standing (unchanged either way: the control).
// The 15:30 calculation reads the series as its own session left them: 0 the
// 15:45 bar, 1 and 2 the 15:30 bucket. Then 09:30 is delivered. Before this
// change the pump ran on 09:30 BEFORE the lazy seal, so yesterday's last
// calculation read series 0 = TOMORROW's 09:30 bar and series 1 = empty: the
// one-input leak this scenario pins shut (it fails on 87b3b06). The warmup
// stops mid second session, so the first lazy seal is the warmup's and the
// second one is live.

NativeRunSpec clipped_hour_spec(const char* session_key) {
    NativeRunSpec spec = base_spec("15", "60", session_key);
    spec.type = "stock";
    spec.timezone = "America/New_York";
    spec.session = "0930-1600";
    NativeTimeframeSubscription input_tf;
    input_tf.tf = "15";
    NativeTimeframeSubscription gapped;
    gapped.tf = "60";
    gapped.gaps = true;
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    spec.subscriptions.push_back(input_tf);
    spec.subscriptions.push_back(gapped);
    spec.subscriptions.push_back(hourly);
    return spec;
}

void check_series(const std::optional<Bar>& got, const Bar& want, const char* tag) {
    CHECK(got.has_value());
    if (!got) {
        std::printf("  %s: empty, want a bucket\n", tag);
        return;
    }
    check_bucket(*got, want, tag);
}

void test_lazy_seal_precedes_the_pump() {
    scenario = "lazy seal precedes the pump";
    const std::vector<Bar> bars = session_bars(3);
    const int per_session = 26;
    const int n_warmup = per_session + 13;
    const auto stamp = [&bars](int i) {
        return std::to_string(bars[static_cast<std::size_t>(i)].timestamp);
    };

    // The sequence by hand. Per input: the input; on a session's first bar,
    // the previous session's clipped 15:30 script bar, sealed lazily and
    // BEFORE anything of this input is delivered; this input's own "15"
    // bucket; on an hour's last bar (the 4th, or 15:45 at the close) the two
    // "60" buckets and, for a full hour, that hour's own calculation.
    std::vector<std::string> want_log;
    std::vector<NativeCompletionKind> want_completions;
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        const int day = i / per_session;
        const int slot = i % per_session;
        const int hour_first = day * per_session + (slot / 4) * 4;
        want_log.push_back("input:" + std::to_string(i) + "@" + stamp(i));
        if (slot == 0 && day > 0) {
            want_log.push_back("bar@" + stamp((day - 1) * per_session + 24));
            want_completions.push_back(NativeCompletionKind::LazyComplete);
        }
        want_log.push_back("htf:0@" + stamp(i));
        const bool full_hour_closes = slot % 4 == 3;
        const bool session_closes = slot == per_session - 1;
        if (full_hour_closes || session_closes) {
            want_log.push_back("htf:1@" + stamp(hour_first));
            want_log.push_back("htf:2@" + stamp(hour_first));
        }
        if (full_hour_closes) {
            want_log.push_back("bar@" + stamp(hour_first));
            want_completions.push_back(NativeCompletionKind::Confirmed);
        }
    }
    // Seven script bars a day; the last session's clipped one has no
    // successor to seal it, in a batch and on a stream alike.
    CHECK(want_completions.size() == 20);

    SeriesHost batch;
    batch.probes = 3;
    CHECK(batch.configure_native(clipped_hour_spec("native-htf-stream-lazy-batch")).status
          == NativeSetupStatus::Applied);
    batch.run(bars.data(), static_cast<int>(bars.size()), "15", "60", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(batch.last_error().empty());
    if (!batch.last_error().empty()) std::printf("  error: %s\n", batch.last_error().c_str());

    SeriesHost stream;
    stream.probes = 3;
    CHECK(stream.configure_native(clipped_hour_spec("native-htf-stream-lazy-live")).status
          == NativeSetupStatus::Applied);
    const bool began = stream.stream_begin(bars.data(), n_warmup, "15", "60");
    CHECK(began);
    if (!began) {
        std::printf("  error: %s\n", stream.last_error().c_str());
        return;
    }
    // The first lazy seal is the warmup's: seven script bars of session one
    // and three full hours of session two.
    CHECK(stream.bars_seen == 10);
    for (int i = n_warmup; i < static_cast<int>(bars.size()); ++i) {
        const bool pushed = stream.stream_push_bar(bars[static_cast<std::size_t>(i)]);
        CHECK(pushed);
        if (!pushed) {
            std::printf("  push %d error: %s\n", i, stream.last_error().c_str());
            return;
        }
    }

    // The stream is the batch, and both are the hand-built sequence.
    check_logs_equal(batch.log, want_log);
    check_logs_equal(stream.log, want_log);
    check_deliveries_equal(stream.deliveries, batch.deliveries);
    CHECK(batch.completions == want_completions);
    CHECK(stream.completions == want_completions);

    // What the two lazily sealed calculations read, and how many script bars
    // had been calculated when the sealing input's own bucket arrived: the
    // clipped bar's calculation is already among them.
    for (const SeriesHost* host : {&batch, &stream}) {
        CHECK(host->series_at_bar.size() == 20);
        if (host->series_at_bar.size() != 20) continue;
        for (int day = 0; day < 2; ++day) {
            const std::size_t calc = static_cast<std::size_t>(day) * 7 + 6;
            const int last = day * per_session + 25;
            const Bar clipped_hour = hand_aggregate(
                bars, last - 1, 2, bars[static_cast<std::size_t>(last - 1)].timestamp);
            const auto& row = host->series_at_bar[calc];
            CHECK(row.size() == 3);
            if (row.size() != 3) continue;
            check_series(row[0], bars[static_cast<std::size_t>(last)],
                         "clipped bar reads 15");
            check_series(row[1], clipped_hour, "clipped bar reads 60 gaps");
            check_series(row[2], clipped_hour, "clipped bar reads 60");
        }
        for (int day = 1; day < 3; ++day) {
            const std::int64_t sealing = bars[static_cast<std::size_t>(day * per_session)].timestamp;
            bool found = false;
            for (const Delivery& delivery : host->deliveries) {
                if (delivery.subscription != 0 || delivery.bar.timestamp != sealing) continue;
                found = true;
                CHECK(delivery.bars_before == day * 7);
                CHECK(delivery.context.delivered_at_ms == sealing);
            }
            CHECK(found);
        }
    }

    // stream_end seals nothing and delivers nothing.
    const std::size_t deliveries_before_end = stream.deliveries.size();
    CHECK(stream.stream_end(false));
    CHECK(stream.bars_seen == 20);
    CHECK(stream.deliveries.size() == deliveries_before_end);
}

}  // namespace

int main() {
    test_warmup_matches_batch(false);
    test_warmup_matches_batch(true);
    test_live_pushes_extend_buckets(false);
    test_live_pushes_extend_buckets(true);
    test_partial_live_bucket_is_not_delivered();
    test_series_bar_across_the_boundary();
    test_stream_without_subscriptions_is_unchanged();
    test_session_calendar_series_across_the_boundary();
    test_authoritative_bars_cover_the_warmup();
    test_tick_input_is_refused_while_subscribed();
    test_installed_feed_survives_a_second_begin();
    test_lazy_seal_precedes_the_pump();
    std::printf("native HTF subscriptions (stream): %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
