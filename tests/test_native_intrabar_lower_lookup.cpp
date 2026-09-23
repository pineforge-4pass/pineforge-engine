// R5 lane PERF-K24 (K2). A retained lower feed hands each script bar its
// sub-bars by binary search, not by a scan of the whole feed.
//
// deliver_intrabar_script chose a script bar's lower bars by testing EVERY
// bar of IntrabarPath::lower_tf against the script bar's window, so each
// script bar cost the length of the whole feed and a lower-timeframe run was
// quadratic in its length: 47 us per bar at 20,000 bars on spark in lane
// PERF0-K, and the scan ran even where no window can hold a bar (a raw-label
// partition is zero-width, so every script bar there falls back to its own
// OHLC path after the scan). preflight_intrabar_path refuses a lower feed
// whose stamps do not strictly increase, so the bars of one window are one
// contiguous run of the feed, in feed order: a lower_bound to its first bar
// and a walk to the window's end select the same bars in the same order.
//
// The witness is the SHAPE of the cost, because the VALUE may not move.
// Four times the script bars (and four times the lower feed, as a real run
// has) must cost less than kShapeBound times as much. Measured on this TU,
// Release flags, best of three process CPU seconds per leg, three runs each:
//   before (scan)         4,000 bars 0.031 s, 16,000 bars 0.494 s: ratio 15.7-16.0
//   after (lower_bound)   4,000 bars 0.002 s, 16,000 bars 0.007 s: ratio  3.6-4.1
// on an Apple M4 Max (clang 17), and 15.7-16.1 before / 4.18-4.26 after on
// Linux aarch64 (gcc 13, one pinned core). 6 separates the two shapes with
// more than 40 % of room above the linear one and a factor of 2.6 below the
// quadratic one. The sample is process CPU time (std::clock), which the
// runtime-budget ruling (A40 rev 7) found load-robust.
//
// The value half pins what the selection feeds -- the driver points, the
// fills and every hash over them -- for three feeds: a lower feed that tiles
// its script bars, a ragged one (bars before the first window and after the
// last, windows holding none, one or several bars, a one-bar window sampled by
// the distribution sampler), and the tiling feed under raw labels, where every
// window is empty. The spec's zone is a fixed offset, so the continuation
// folds no tzdata file and the pins hold on every host; every price is an
// exact binary fraction on a 0.25 tick.
//
// Provenance of the pinned data: this TU compiled unchanged against the
// engine at fc7aad62 (before the change) with -DPINEFORGE_K24_HARVEST, which
// prints the observed values as the initializers below instead of checking
// them. Rebuild them the same way; never edit one by hand to make a run pass.
#include <pineforge/native_host.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t T = 1736121600000LL;  // 2025-01-06 00:00 UTC
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kScriptMs = 5 * kMinute;

// The ragged feed starts before the first script bar, so minutes go negative.
std::int64_t wrap(std::int64_t value, std::int64_t period) {
    const std::int64_t r = value % period;
    return r < 0 ? r + period : r;
}

// A slow triangular trend with a one-point zig-zag on top, in quarter steps:
// every price sits on the 0.25 tick, and every script bar swings through both
// legs of a bracket, so the ORDER of its sub-bars -- not the whole-bar OHLC
// path a script bar without them walks -- decides which leg fills.
double price_at(std::int64_t minute) {
    const std::int64_t phase = wrap(minute, 48);
    const std::int64_t triangle = phase < 24 ? phase : 48 - phase;
    return 100.0 + 0.25 * static_cast<double>(triangle)
        + (wrap(minute, 2) == 0 ? 1.0 : -1.0);
}

Bar minute_bar(std::int64_t minute) {
    const double open = price_at(minute);
    const double close = price_at(minute + 1);
    const double high = (open > close ? open : close) + 0.25;
    const double low = (open < close ? open : close) - 0.25;
    return {open, high, low, close, 1.0 + static_cast<double>(wrap(minute, 4)),
            T + minute * kMinute};
}

// One script bar aggregated from minutes [5k, 5k + 5), whether or not the
// lower feed keeps them.
Bar script_bar(int index) {
    const std::int64_t first = 5 * static_cast<std::int64_t>(index);
    Bar out = minute_bar(first);
    for (std::int64_t m = first + 1; m < first + 5; ++m) {
        const Bar part = minute_bar(m);
        if (part.high > out.high) out.high = part.high;
        if (part.low < out.low) out.low = part.low;
        out.close = part.close;
        out.volume += part.volume;
    }
    out.timestamp = T + static_cast<std::int64_t>(index) * kScriptMs;
    return out;
}

std::vector<Bar> script_feed(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) bars.push_back(script_bar(i));
    return bars;
}

enum class Feed { Tiling, Ragged };

// Tiling: every minute of every script bar. Ragged: three minutes before the
// first script bar, then script bar k keeps k % 6 of its minutes (none, one,
// ... five), then four minutes after the last one.
std::vector<Bar> lower_feed(Feed shape, int count) {
    std::vector<Bar> bars;
    if (shape == Feed::Tiling) {
        bars.reserve(static_cast<std::size_t>(count) * 5);
        for (std::int64_t m = 0; m < 5 * static_cast<std::int64_t>(count); ++m)
            bars.push_back(minute_bar(m));
        return bars;
    }
    for (std::int64_t m = -3; m < 0; ++m) bars.push_back(minute_bar(m));
    for (int k = 0; k < count; ++k) {
        for (int j = 0; j < k % 6; ++j)
            bars.push_back(minute_bar(5 * static_cast<std::int64_t>(k) + j));
    }
    for (std::int64_t m = 0; m < 4; ++m)
        bars.push_back(minute_bar(5 * static_cast<std::int64_t>(count) + m));
    return bars;
}

NativeRunSpec lower_spec(Feed shape, int count, bool raw_labels, const char* session_key) {
    NativeRunSpec spec;
    // Reads its whole event record once the run has ended (V19-B).
    spec.event_retention = NativeEventRetention::Full;
    spec.identity = {session_key, 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:K24";
    spec.timezone = "UTC+5";
    spec.session = "24x7";
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 1.0;
    if (raw_labels) spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    IntrabarPath::lower_tf lower;
    lower.bars = lower_feed(shape, count);
    lower.tf = "1";
    if (shape == Feed::Ragged) {
        lower.samples = 3;
        lower.sample_eligibility = IntrabarPath::SampleEligibility::DistributionSamples;
    }
    spec.intrabar.value = lower;
    return spec;
}

// No orders: the scaling witness times the driver alone.
struct Idle final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

// Every sixth bar while flat: a market entry and a take-profit / stop-loss
// pair in one cancel group, armed by the entry's fill, so the sub-bar path
// decides where the bracket fills.
struct Bracket final : NativeStrategyHost {
    int bar = 0;
    void on_native_run_begin() override { bar = 0; }
    static no::Request leg(no::Trigger trigger, std::uint64_t group, no::RequestHandle parent) {
        no::Request request;
        request.intent = no::Reduce{no::ExplicitUnits{1.0}};
        request.label = "x";
        request.trigger = trigger;
        no::Member member;
        member.group = group;
        member.cohort = 0;
        member.effect = no::GroupEffect::Cancel;
        request.group = member;
        no::WaitForApplied owner;
        owner.parent = parent;
        request.owner = owner;
        return request;
    }
    void on_native_bar(const Bar& b, const NativeDecisionContext&) override {
        ++bar;
        if (physical_position().signed_units != 0.0 || bar % 6 != 0) return;
        no::Request entry;
        entry.intent = no::Transact{1.0};
        entry.label = "E";
        const auto accepted = submit(entry);
        if (!accepted.handle) return;
        const auto group = static_cast<std::uint64_t>(bar);
        submit(leg(no::Limit{b.close + 0.5}, group, *accepted.handle));
        submit(leg(no::Stop{b.close - 0.5}, group, *accepted.handle));
    }
};

// Raw labels, the source adapter's own policy: every window is empty there, so
// the run's cost is the driver's and the lookup's alone, and a scan of the
// feed stands out against it. (Under canonical labels the per-bar calendar
// dominates a run this short and would mask the shape.)
double run_cpu(int count) {
    Idle host;
    const auto setup = host.configure_native(lower_spec(Feed::Tiling, count, true, "k24-scale"));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const std::vector<Bar> bars = script_feed(count);
    const std::clock_t started = std::clock();
    host.run(bars.data(), count);
    const double seconds = static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    return seconds;
}

double best_of_three(int count) {
    double best = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const double sample = run_cpu(count);
        if (attempt == 0 || sample < best) best = sample;
    }
    return best;
}

constexpr int kBars = 4000;
constexpr double kShapeBound = 6.0;

void the_lower_feed_lookup_is_linear_in_the_run() {
    const double small = best_of_three(kBars);
    const double large = best_of_three(kBars * 4);
    const double ratio = small > 0.0 ? large / small : 0.0;
    std::printf("lower-feed run: %d bars %.4fs, %d bars %.4fs, ratio %.2f (bound %.1f)\n",
                kBars, small, kBars * 4, large, ratio, kShapeBound);
    CHECK(small > 0.0);
    CHECK(ratio < kShapeBound);
}

struct Observed {
    int trades;
    std::uint64_t trade_digest;
    std::uint64_t driver_points;
    std::uint64_t continuation;
    std::uint64_t broker;
};

std::uint64_t fnv(std::uint64_t h, double value) {
    unsigned char bytes[sizeof value];
    std::memcpy(bytes, &value, sizeof value);
    for (unsigned char byte : bytes) {
        h ^= byte;
        h *= 1099511628211ull;
    }
    return h;
}

Observed observe(Feed shape, bool raw_labels, const char* session_key) {
    constexpr int kScenarioBars = 240;
    Bracket host;
    const auto setup =
        host.configure_native(lower_spec(shape, kScenarioBars, raw_labels, session_key));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const std::vector<Bar> bars = script_feed(kScenarioBars);
    host.run(bars.data(), kScenarioBars);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    Observed out{};
    out.trades = host.trade_count();
    out.trade_digest = 1469598103934665603ull;
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        out.trade_digest = fnv(out.trade_digest, trade.entry_price);
        out.trade_digest = fnv(out.trade_digest, trade.exit_price);
        out.trade_digest = fnv(out.trade_digest, trade.qty);
        out.trade_digest = fnv(out.trade_digest, trade.pnl);
    }
    for (const auto& event : host.native_events(0))
        if (event.driver) ++out.driver_points;
    out.continuation = host.native_continuation_hash();
    out.broker = host.broker_state_hash();
    return out;
}

struct Pinned {
    Feed shape;
    bool raw_labels;
    const char* name;
    Observed expected;
};

// expectation corrected (6 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); trades, trade digests and driver points did not move:
//   11952820133879191213ull -> 10889724778366893781ull
//   13409848690567451958ull -> 11890728307108792147ull
//   11796713569757805492ull -> 11131379411467769408ull
//   3688284093780238334ull -> 19126489985415469ull
//   6853731202851505960ull -> 11214347720918037394ull
//   6726314833335123700ull -> 1957262624383604956ull
// expectation corrected (v19-B, 6 values), because v19-B: the run keeps
// NativeEventRetention::Full to count its driver points, and the spec digest
// folds a retention that is not the default Window; trades, trade digests
// and driver points did not move:
//   10889724778366893781ull -> 12678158763721467250ull
//   11890728307108792147ull -> 82279288603700875ull
//   11131379411467769408ull -> 27817212454474162ull
//   19126489985415469ull -> 11026420420329913868ull
//   11214347720918037394ull -> 10331922125992720379ull
//   1957262624383604956ull -> 4486098428384108224ull
const Pinned kPinned[] = {
    {Feed::Tiling, false, "k24-lower-tiling",
     {39, 0xd2ac6e3a775d5206ull, 4800ull, 12678158763721467250ull, 82279288603700875ull}},
    {Feed::Ragged, false, "k24-lower-ragged",
     {39, 0xd1675820a42f7e8cull, 2520ull, 27817212454474162ull, 11026420420329913868ull}},
    {Feed::Tiling, true, "k24-lower-raw",
     {39, 0xd1675820a42f7e8cull, 960ull, 10331922125992720379ull, 4486098428384108224ull}},
};

void the_selected_sub_bars_feed_the_same_run() {
    for (const auto& pin : kPinned) {
        const Observed got = observe(pin.shape, pin.raw_labels, pin.name);
#ifdef PINEFORGE_K24_HARVEST
        std::printf("    {Feed::%s, %s, \"%s\",\n     {%d, 0x%016" PRIx64 "ull, %" PRIu64
                    "ull, %" PRIu64 "ull, %" PRIu64 "ull}},\n",
                    pin.shape == Feed::Tiling ? "Tiling" : "Ragged",
                    pin.raw_labels ? "true" : "false", pin.name, got.trades, got.trade_digest,
                    got.driver_points, got.continuation, got.broker);
#else
        if (got.trades != pin.expected.trades || got.trade_digest != pin.expected.trade_digest
            || got.driver_points != pin.expected.driver_points
            || got.continuation != pin.expected.continuation
            || got.broker != pin.expected.broker) {
            std::fprintf(stderr,
                         "%s: trades %d digest %016" PRIx64 " points %" PRIu64
                         " continuation %" PRIu64 " broker %" PRIu64 "\n",
                         pin.name, got.trades, got.trade_digest, got.driver_points,
                         got.continuation, got.broker);
        }
        CHECK(got.trades == pin.expected.trades);
        CHECK(got.trade_digest == pin.expected.trade_digest);
        CHECK(got.driver_points == pin.expected.driver_points);
        CHECK(got.continuation == pin.expected.continuation);
        CHECK(got.broker == pin.expected.broker);
#endif
    }
}

} // namespace

int main() {
    the_selected_sub_bars_feed_the_same_run();
#ifndef PINEFORGE_K24_HARVEST
    the_lower_feed_lookup_is_linear_in_the_run();
#endif
    if (failures == 0) std::printf("test_native_intrabar_lower_lookup: ok\n");
    return failures == 0 ? 0 : 1;
}
