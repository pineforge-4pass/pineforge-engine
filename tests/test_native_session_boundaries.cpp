// Kernel correctness at session boundaries (R5 lane F14; AUDIT3-opus2 H1).
//
// A session that declares a BREAK -- two windows on one session day,
// "0000-0230,0330-0600", or the acid test's lunch break
// "0900-1130,1230-1500:23456" -- is still ONE session day. Its buckets are
// clipped where trading stops (the break, the day's close) and nowhere else.
// The session helpers the higher-timeframe aggregator keys on
// (src/timeframe.cpp) read the first window alone, so the day "closed" at the
// break: every later bar was taken for the session's last and completed its
// bucket on its own first bar (AUDIT3-opus2 probe4: v = 1/1/1 after the break
// where the hand aggregation reads 2/4/4). Witnesses, each against a hand
// aggregation of the tape, never against the code under test:
//
//   1. the public TimeframeAggregator("60", "15") on the two-window tape
//      completes 4, 4, 2 (clipped at the 02:30 break), 2 (opened by the 03:30
//      reopen), 4, 4 -- each on its own last bar -- and the one-window control
//      "0000-0600" keeps its six full buckets;
//   2. "D" over the same tape completes ONCE, on the day's last bar, with all
//      twenty inputs, and the day's close is the last window's, 06:00;
//   3. a native "60" subscription over the 15-minute input delivers those
//      buckets, each Confirmed on its own last input, in a batch and on a
//      stream's live pump alike, and with a "60" script interval the
//      subscription's buckets equal the script bars the native calendar
//      builds from the same inputs;
//   4. the acid tape (Asia/Tokyo 15m, three session days) with a "60" series
//      from the input and a "10" and a "60" series from a "5" auxiliary feed:
//      every bucket, before and after the lunch break, equals the hand
//      aggregation, rides on the input holding its last contributing bar, and
//      the feed's "60" equals the input's "60" bucket for bucket.
//
// Ruling on a bucket a session window clips (the 02:00 bucket at 02:30, the
// acid 11:00 bucket at the 11:30 lunch): it is delivered Confirmed, on its own
// last input. The declared calendar proves where the window closes, so no
// later input is needed to reveal the bucket complete -- the one case
// LazyComplete names ("the next period's first input closed it"). A stream's
// live pump, which knows no next input at all, delivers it on the same bar.
//
// Source-free: this TU links the generic kernel alone and runs in the
// kernel-only profile.

#include <pineforge/native_host.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,        \
                        #expression);                                          \
        }                                                                      \
    } while (false)

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

bool same(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
}

bool same_bar(const Bar& a, const Bar& b) {
    return same(a.open, b.open) && same(a.high, b.high) && same(a.low, b.low)
        && same(a.close, b.close) && same(a.volume, b.volume);
}

// Wall-clock HH:MM of `ms` on a clock whose midnight is `midnight`.
std::string hhmm(std::int64_t ms, std::int64_t midnight = 0) {
    const std::int64_t minutes = ((ms - midnight) / kMinute) % 1440;
    char text[8];
    std::snprintf(text, sizeof text, "%02lld:%02lld",
                  static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
    return text;
}

// One hand-aggregated bucket: the consecutive inputs [first, first + count)
// that share one bucket key. The bucket is complete on its last input.
struct Group {
    std::int64_t key_ms = 0;  // the bucket's grid open
    std::size_t first = 0;
    std::size_t count = 0;
    Bar bar{};
};

// Consecutive bars grouped by the `width`-wide grid anchored at `origin`: the
// independent oracle every witness compares to.
std::vector<Group> hand_groups(const std::vector<Bar>& bars, std::int64_t origin,
                               std::int64_t width) {
    std::vector<Group> groups;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const std::int64_t key =
            origin + ((bars[i].timestamp - origin) / width) * width;
        if (groups.empty() || groups.back().key_ms != key) {
            Group group;
            group.key_ms = key;
            group.first = i;
            group.count = 1;
            group.bar = bars[i];
            groups.push_back(group);
            continue;
        }
        Group& group = groups.back();
        group.bar.high = std::max(group.bar.high, bars[i].high);
        group.bar.low = std::min(group.bar.low, bars[i].low);
        group.bar.close = bars[i].close;
        group.bar.volume += bars[i].volume;
        ++group.count;
    }
    return groups;
}

// The two-window tape of AUDIT3-opus2 probe3/probe4: 15-minute bars at the
// UTC minutes-of-day `minutes`, the close a function of the minute so every
// bucket's OHLC names its inputs.
Bar minute_bar(int minute) {
    const double base = 100.0 + minute / 100.0;
    return Bar{base, base + 1.0, base - 1.0, base, 1.0, minute * kMinute};
}

std::vector<Bar> tape_of(const std::vector<int>& minutes) {
    std::vector<Bar> bars;
    for (int minute : minutes) bars.push_back(minute_bar(minute));
    return bars;
}

std::vector<int> one_window_minutes() {
    std::vector<int> minutes;
    for (int m = 0; m < 360; m += 15) minutes.push_back(m);
    return minutes;
}

std::vector<int> two_window_minutes() {
    std::vector<int> minutes;
    for (int m = 0; m < 150; m += 15) minutes.push_back(m);    // 00:00 .. 02:15
    for (int m = 210; m < 360; m += 15) minutes.push_back(m);  // 03:30 .. 05:45
    return minutes;
}

const char* const kOneWindow = "0000-0600";
const char* const kTwoWindows = "0000-0230,0330-0600";

// ---- 1. the public aggregator, "60" over "15" ------------------------------

struct Completion {
    std::int64_t on_ms = 0;  // the input bar the bucket completed on
    Bar bucket{};
    int sub_bars = 0;
};

std::vector<Completion> aggregate(const std::string& target, const std::string& session,
                                  const std::vector<Bar>& bars) {
    TimeframeAggregator aggregator(target, "15", "UTC", session);
    std::vector<Completion> out;
    for (const Bar& bar : bars) {
        const AggregatedBar step = aggregator.feed(bar);
        if (!step.is_complete) continue;
        Completion completion;
        completion.on_ms = bar.timestamp;
        completion.bucket = step.bar;
        completion.sub_bars = step.sub_bar_count;
        out.push_back(completion);
    }
    return out;
}

void check_hourly_completions(const char* session) {
    const std::vector<Bar> bars =
        tape_of(std::string(session) == kOneWindow ? one_window_minutes()
                                                   : two_window_minutes());
    const std::vector<Group> want = hand_groups(bars, 0, kHour);
    const std::vector<Completion> got = aggregate("60", session, bars);
    CHECK(want.size() == 6);
    CHECK(got.size() == want.size());
    for (std::size_t k = 0; k < std::min(want.size(), got.size()); ++k) {
        const Group& w = want[k];
        const Bar& last = bars[w.first + w.count - 1];
        const bool ok = got[k].on_ms == last.timestamp && got[k].bucket.timestamp == w.key_ms
            && same_bar(got[k].bucket, w.bar)
            && got[k].sub_bars == static_cast<int>(w.count);
        CHECK(ok);
        if (!ok) {
            std::printf("    bucket %s: want v=%g on %s | got v=%g on %s (bucket %s)\n",
                        hhmm(w.key_ms).c_str(), w.bar.volume, hhmm(last.timestamp).c_str(),
                        got[k].bucket.volume, hhmm(got[k].on_ms).c_str(),
                        hhmm(got[k].bucket.timestamp).c_str());
        }
    }
}

void test_aggregator_hourly_across_a_break() {
    scenario = "aggregator 60 over 15, one window";
    check_hourly_completions(kOneWindow);
    scenario = "aggregator 60 over 15, two windows";
    check_hourly_completions(kTwoWindows);
    // The buckets the break shapes, by volume: 02:00 is clipped at 02:30,
    // 03:00 opens at the 03:30 reopen.
    const std::vector<Completion> got = aggregate("60", kTwoWindows, tape_of(two_window_minutes()));
    std::vector<double> volumes;
    for (const Completion& c : got) volumes.push_back(c.bucket.volume);
    CHECK((volumes == std::vector<double>{4, 4, 2, 2, 4, 4}));
}

// ---- 2. the session day, "D" over "15" --------------------------------------

void test_daily_bucket_is_the_whole_day() {
    for (const char* session : {kOneWindow, kTwoWindows}) {
        scenario = std::string(session) == kOneWindow ? "aggregator D over 15, one window"
                                                      : "aggregator D over 15, two windows";
        const std::vector<Bar> bars =
            tape_of(std::string(session) == kOneWindow ? one_window_minutes()
                                                       : two_window_minutes());
        const std::vector<Group> want = hand_groups(bars, 0, kDay);
        const std::vector<Completion> got = aggregate("D", session, bars);
        CHECK(want.size() == 1);
        CHECK(got.size() == 1);
        if (!got.empty()) {
            CHECK(got[0].on_ms == bars.back().timestamp);
            CHECK(got[0].bucket.timestamp == 0);
            CHECK(same_bar(got[0].bucket, want[0].bar));
            CHECK(got[0].sub_bars == static_cast<int>(bars.size()));
        }
        if (got.size() != 1 || got[0].on_ms != bars.back().timestamp) {
            for (const Completion& c : got) {
                std::printf("    D completed on %s with v=%g\n", hhmm(c.on_ms).c_str(),
                            c.bucket.volume);
            }
        }
        // The session day closes at its LAST window's close, whichever bar
        // asks, and opens at its first window's open.
        int wrong_close = 0, wrong_open = 0;
        std::int64_t first_wrong_close = 0;
        for (const Bar& bar : bars) {
            const std::int64_t close =
                session_period_close_ms(bar.timestamp, "UTC", session, CalendarPeriod::DAY);
            if (close != 6 * kHour && wrong_close++ == 0) first_wrong_close = close;
            if (session_period_open_ms(bar.timestamp, "UTC", session, CalendarPeriod::DAY) != 0) {
                ++wrong_open;
            }
        }
        CHECK(wrong_close == 0);
        CHECK(wrong_open == 0);
        if (wrong_close != 0) {
            std::printf("    %d of %zu bars read the day's close at %s, not 06:00\n", wrong_close,
                        bars.size(), hhmm(first_wrong_close).c_str());
        }
    }
}

// ---- 3. a native "60" subscription ------------------------------------------

struct Delivery {
    std::size_t subscription = 0;
    Bar bucket{};
    NativeCompletionKind completion = NativeCompletionKind::Confirmed;
    std::int64_t delivered_at_ms = 0;
};

class SessionHost final : public NativeStrategyHost {
public:
    std::vector<Delivery> deliveries;
    std::vector<Bar> script_bars;

    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        Delivery delivery;
        delivery.subscription = context.subscription;
        delivery.bucket = bar;
        delivery.completion = context.completion;
        delivery.delivered_at_ms = context.delivered_at_ms;
        deliveries.push_back(delivery);
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        script_bars.push_back(bar);
    }
};

NativeRunSpec session_spec(const char* key, const char* session, const char* script_tf,
                           const char* timezone = "UTC") {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "15";
    spec.script_tf = script_tf;
    spec.ticker = "BREAK";
    spec.tickerid = "TEST:BREAK";
    spec.type = "futures";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "session-break fixture";
    spec.volumetype = "base";
    spec.timezone = timezone;
    spec.session = session;
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// Series `subscription` of `host` against the hand groups: one Confirmed
// delivery per group, on the input `carrier(group)`.
template <typename Carrier>
void check_series(const std::vector<Delivery>& deliveries, std::size_t subscription,
                  const std::vector<Group>& want, const std::vector<Bar>& inputs,
                  std::int64_t midnight, Carrier carrier) {
    std::vector<const Delivery*> got;
    for (const Delivery& d : deliveries) {
        if (d.subscription == subscription) got.push_back(&d);
    }
    CHECK(got.size() == want.size());
    int shown = 0;
    for (std::size_t k = 0; k < std::min(got.size(), want.size()); ++k) {
        const std::int64_t on = inputs[carrier(want[k])].timestamp;
        const bool ok = got[k]->bucket.timestamp == want[k].key_ms
            && same_bar(got[k]->bucket, want[k].bar) && got[k]->delivered_at_ms == on
            && got[k]->completion == NativeCompletionKind::Confirmed;
        CHECK(ok);
        if (!ok && shown++ < 8) {
            std::printf("    series %zu bucket %s: want v=%g Confirmed on %s | got bucket %s v=%g "
                        "%s on %s\n",
                        subscription, hhmm(want[k].key_ms, midnight).c_str(), want[k].bar.volume,
                        hhmm(on, midnight).c_str(),
                        hhmm(got[k]->bucket.timestamp, midnight).c_str(), got[k]->bucket.volume,
                        got[k]->completion == NativeCompletionKind::Confirmed ? "Confirmed"
                                                                             : "LazyComplete",
                        hhmm(got[k]->delivered_at_ms, midnight).c_str());
        }
    }
}

std::size_t last_input_of(const Group& group) { return group.first + group.count - 1; }

void test_subscription_across_a_break() {
    const std::vector<Bar> inputs = tape_of(two_window_minutes());
    const std::vector<Group> want = hand_groups(inputs, 0, kHour);
    for (const char* script_tf : {"15", "60"}) {
        scenario = std::string(script_tf) == "15" ? "subscription 60, script 15, batch"
                                                  : "subscription 60, script 60, batch";
        NativeRunSpec spec = session_spec("f14-sub-batch", kTwoWindows, script_tf);
        NativeTimeframeSubscription hourly;
        hourly.tf = "60";
        spec.subscriptions = {hourly};
        SessionHost host;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(inputs.data(), static_cast<int>(inputs.size()));
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        check_series(host.deliveries, 0, want, inputs, 0, last_input_of);
        if (std::string(script_tf) != "60") continue;
        // The script path builds its "60" bars from the native calendar: the
        // subscription's buckets are those bars, bar for bar.
        CHECK(host.script_bars.size() == want.size());
        for (std::size_t k = 0; k < std::min(host.script_bars.size(), want.size()); ++k) {
            CHECK(same_bar(host.script_bars[k], want[k].bar));
            if (k < host.deliveries.size()) {
                CHECK(same_bar(host.script_bars[k], host.deliveries[k].bucket));
            }
        }
    }
    // A stream continues the same aggregator live, where no input knows its
    // successor: the calendar alone closes the clipped bucket.
    scenario = "subscription 60, script 15, stream";
    NativeRunSpec spec = session_spec("f14-sub-stream", kTwoWindows, "15");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    spec.subscriptions = {hourly};
    SessionHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    constexpr int kWarmup = 3;
    CHECK(host.stream_begin(inputs.data(), kWarmup, "15", "15"));
    for (std::size_t i = kWarmup; i < inputs.size(); ++i) {
        const bool pushed = host.stream_push_bar(inputs[i]);
        CHECK(pushed);
        if (!pushed) {
            std::printf("    push %s refused: %s\n", hhmm(inputs[i].timestamp).c_str(),
                        host.last_error().c_str());
            break;
        }
    }
    CHECK(host.stream_end(false));
    check_series(host.deliveries, 0, want, inputs, 0, last_input_of);
}

// ---- 4. the acid tape --------------------------------------------------------

// AUDIT3-opus2's acid tape: Mon/Tue 2026-06-15/16 whole session days and the
// Wed morning, 15-minute inputs, each split into three 5-minute feed bars that
// aggregate to it exactly.
constexpr std::int64_t kAcidOrigin = 1781481600000LL;  // 2026-06-15 09:00 JST (00:00Z)
constexpr std::int64_t kJstMidnight = kAcidOrigin - 9 * kHour;
const char* const kAcidSession = "0900-1130,1230-1500:23456";

struct AcidTape {
    std::vector<Bar> inputs;
    std::vector<Bar> feed;
};

AcidTape acid_tape() {
    // Session-day slots: 09:00 .. 11:15 and 12:30 .. 14:45 JST (ten each).
    std::vector<int> slots;
    for (int m = 0; m < 150; m += 15) slots.push_back(m);
    for (int m = 210; m < 360; m += 15) slots.push_back(m);
    AcidTape tape;
    double price = 100.0;
    for (int day = 0; day < 3; ++day) {
        for (int slot : slots) {
            if (day == 2 && slot >= 150) break;  // Wed: morning only
            const std::int64_t ts = kAcidOrigin + day * kDay + slot * kMinute;
            const double o = price;
            const double c = price + ((tape.inputs.size() % 3 == 0) ? 0.35 : -0.15);
            const double h = std::max(o, c) + 0.20;
            const double l = std::min(o, c) - 0.25;
            tape.inputs.push_back(Bar{o, h, l, c, 30.0, ts});
            const double m = (o + h) / 2.0;
            const double n = (l + c) / 2.0;
            tape.feed.push_back(Bar{o, h, std::min(o, m), m, 10.0, ts});
            tape.feed.push_back(Bar{m, std::max(m, n), l, n, 10.0, ts + 5 * kMinute});
            tape.feed.push_back(Bar{n, std::max(n, c), std::min(n, c), c, 10.0, ts + 10 * kMinute});
            price = c;
        }
    }
    return tape;
}

void test_acid_tape() {
    scenario = "acid tape, three series";
    const AcidTape tape = acid_tape();
    CHECK(tape.inputs.size() == 50);
    CHECK(tape.feed.size() == 150);
    NativeRunSpec spec = session_spec("f14-acid", kAcidSession, "15", "Asia/Tokyo");
    NativeTimeframeSubscription hourly;          // 0: "60" of the input
    hourly.tf = "60";
    NativeTimeframeSubscription ten;             // 1: "10" of the "5" feed
    ten.tf = "10";
    ten.source = NativeSeriesSource::AuxiliaryFeed;
    NativeTimeframeSubscription hourly_feed;     // 2: "60" of the "5" feed
    hourly_feed.tf = "60";
    hourly_feed.source = NativeSeriesSource::AuxiliaryFeed;
    spec.subscriptions = {hourly, ten, hourly_feed};
    NativeAuxiliaryFeed feed;
    feed.tf = "5";
    feed.bars = tape.feed;
    spec.auxiliary_feed = feed;
    SessionHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(tape.inputs.data(), static_cast<int>(tape.inputs.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);

    // Hourly buckets on the 09:00 origin: per whole day 09:00 (4), 10:00 (4),
    // 11:00 (2, clipped by the lunch break), 12:00 (2, opened by the 12:30
    // reopen), 13:00 (4), 14:00 (4); Wednesday's three morning buckets.
    const std::vector<Group> hours = hand_groups(tape.inputs, kAcidOrigin, kHour);
    CHECK(hours.size() == 15);
    scenario = "acid tape, series 0 (\"60\" of the input)";
    check_series(host.deliveries, 0, hours, tape.inputs, kJstMidnight, last_input_of);
    // Ten-minute buckets of two feed bars each, riding on the input whose
    // period holds the second (three feed bars per input).
    const std::vector<Group> tens = hand_groups(tape.feed, kAcidOrigin, 10 * kMinute);
    CHECK(tens.size() == 75);
    scenario = "acid tape, series 1 (\"10\" of the feed)";
    check_series(host.deliveries, 1, tens, tape.inputs, kJstMidnight,
                 [](const Group& group) { return last_input_of(group) / 3; });
    const std::vector<Group> feed_hours = hand_groups(tape.feed, kAcidOrigin, kHour);
    scenario = "acid tape, series 2 (\"60\" of the feed)";
    check_series(host.deliveries, 2, feed_hours, tape.inputs, kJstMidnight,
                 [](const Group& group) { return last_input_of(group) / 3; });
    // The feed's hourly series IS the input's, delivery point included.
    std::vector<const Delivery*> s0, s2;
    for (const Delivery& d : host.deliveries) {
        if (d.subscription == 0) s0.push_back(&d);
        if (d.subscription == 2) s2.push_back(&d);
    }
    CHECK(s0.size() == s2.size());
    for (std::size_t k = 0; k < std::min(s0.size(), s2.size()); ++k) {
        CHECK(s0[k]->bucket.timestamp == s2[k]->bucket.timestamp);
        CHECK(same_bar(s0[k]->bucket, s2[k]->bucket));
        CHECK(s0[k]->completion == s2[k]->completion);
        CHECK(s0[k]->delivered_at_ms == s2[k]->delivered_at_ms);
    }
}

}  // namespace

int main() {
    test_aggregator_hourly_across_a_break();
    test_daily_bucket_is_the_whole_day();
    test_subscription_across_a_break();
    test_acid_tape();
    std::printf("native session boundaries: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
