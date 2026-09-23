// R5 lane PERF-L1: the per-bar lookup memos against fresh resolutions.
//
// A script bar asks for its own input and script intervals around a lookahead
// to the next bar's (present_session_day), and reads three session points --
// its label, the bar before and the bar after. The interval cache kept one
// entry per kind, so the lookahead evicted the bar's own answers and canonical
// labels resolved the calendar six times a bar for three instants; each
// session point went through the session-day memo every time. The lane keeps,
// per kind, the answer the slot held before its last replacement (the prior),
// and the last two session points answered, and serves a repeated instant
// from them.
//
// Both memos are exact if every answer they hold is the answer a fresh
// resolution gives for its instant at the moment it is served. This witness
// checks exactly that, inside every NativeStrategyHost hook of randomized runs
// -- canonical and tolerant labels, UTC, New York (RTH, with a clock change),
// Tokyo with a lunch break and London, 1- to 60-minute scripts over 1- and
// 5-minute inputs, batch runs and confirmed-bar streams, with orders -- and
// after every public call: each held slot and prior is interval_containing's
// memo-free answer (with the tolerant fallback), each held session point is
// session_day_at's memo-free answer, and a lookup the test makes itself at the
// bar's label, the instants around it and random instants answers the fresh
// resolution. Every value those memos feed a host -- intervals, coordinates,
// session-day facts -- is also pinned by tests/test_native_calendar_hash_witness
// and tests/test_native_lean_path.
//
// Fail-before: at the lane's base the cache has no prior and the consumer no
// session-point memo, so this TU does not compile there (the lane report
// records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v18 {

struct NativeExecutionConsumerProbe {
    using Interval = std::optional<native_calendar::NativeInterval>;
    using Point = NativeExecutionConsumer::SessionPoint;

    // The pre-lane resolution of one kind: memo-free, with the tolerant
    // fallback, or the raw partition.
    static Interval fresh(const NativeExecutionConsumer& c, bool script, std::int64_t ts) {
        if (c.uses_raw_label_partition()) return NativeExecutionConsumer::timestamp_partition(ts);
        auto interval = native_calendar::interval_containing(
            c.calendar_, script ? c.script_tf_ : c.input_tf_, ts);
        if (!interval && c.legacy_tolerant_slot_labels()) {
            interval = NativeExecutionConsumer::timestamp_partition(ts);
        }
        return interval;
    }
    static Point fresh_point(const NativeExecutionConsumer& c, std::int64_t ms) {
        const auto day = native_calendar::session_day_at(c.calendar_, ms);
        if (!day) return {};
        return {day->in_session_at(ms), day->ordinal};
    }
    static Interval input(const NativeExecutionConsumer& c, std::int64_t ts) {
        return c.input_interval_at(ts);
    }
    static Interval script(const NativeExecutionConsumer& c, std::int64_t ts) {
        return c.script_interval_at(ts);
    }
    static Point point(const NativeExecutionConsumer& c, std::int64_t ms) {
        return c.session_point(ms);
    }
    static const NativeExecutionConsumer::IntervalCache& cache(const NativeExecutionConsumer& c) {
        return c.interval_cache_;
    }
    static const auto& points(const NativeExecutionConsumer& c) { return c.session_points_; }
    static bool running(const NativeExecutionConsumer& c) {
        return std::holds_alternative<NativeRunning>(c.state_);
    }
};

}  // inline namespace engine_script_run_v18
}  // namespace pineforge

using namespace pineforge;
namespace no = pineforge::native_order;
using Probe = NativeExecutionConsumerProbe;

namespace {

int passed = 0;
int failed = 0;
std::uint64_t comparisons = 0;
std::uint64_t prior_checks = 0;
std::uint64_t point_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define SAME(where, ok)                                                          \
    do {                                                                         \
        ++comparisons;                                                           \
        if (ok) {                                                                \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "MISMATCH at %s: %s\n", where, #ok);            \
        }                                                                        \
    } while (0)

bool same_interval(const Probe::Interval& a, const Probe::Interval& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->open_ms == b->open_ms && a->eligible_open_ms == b->eligible_open_ms
        && a->last_traded_close_ms == b->last_traded_close_ms
        && a->next_period_open_ms == b->next_period_open_ms
        && a->next_input_open_ms == b->next_input_open_ms;
}

bool same_point(const Probe::Point& a, const Probe::Point& b) {
    return a.in_session == b.in_session && a.ordinal == b.ordinal;
}

constexpr std::int64_t kMinute = 60000;

struct Zone {
    const char* timezone;
    const char* session;
    std::int64_t t0;  // a session open in the zone
};

// UTC around the clock; New York's regular session across the 2024 spring
// clock change; Tokyo with its lunch break; London across its 2024 autumn
// clock change.
constexpr Zone kZones[] = {
    {"UTC", "24x7", 1704067200000LL},                                  // 2024-01-01 00:00Z
    {"America/New_York", "0930-1600", 1709908200000LL},                // 2024-03-08 14:30Z
    {"Asia/Tokyo", "0900-1130,1230-1500", 1704416400000LL},            // 2024-01-05 00:00Z
    {"Europe/London", "0800-1630", 1729843200000LL},                   // 2024-10-25 08:00Z
};

struct Shape {
    const char* input_tf;
    const char* script_tf;
    std::int64_t step;
};
constexpr Shape kShapes[] = {{"1", "1", kMinute}, {"1", "5", kMinute}, {"5", "15", 5 * kMinute},
                             {"5", "60", 5 * kMinute}, {"1", "60", kMinute}};

struct MemoHost final : NativeStrategyHost {
    std::mt19937_64* rng = nullptr;
    std::int64_t step = kMinute;
    int closes = 0;
    // A run's fresh resolutions, each taken once per instant: nothing they
    // read changes inside a run, and the memo-free zoned calendar costs tens
    // of microseconds an instant.
    mutable std::map<std::pair<bool, std::int64_t>, Probe::Interval> fresh_intervals;
    mutable std::map<std::int64_t, Probe::Point> fresh_points;

    Probe::Interval fresh(const NativeExecutionConsumer& c, bool script, std::int64_t ts) const {
        const auto key = std::make_pair(script, ts);
        const auto found = fresh_intervals.find(key);
        if (found != fresh_intervals.end()) return found->second;
        return fresh_intervals[key] = Probe::fresh(c, script, ts);
    }
    Probe::Point fresh_point(const NativeExecutionConsumer& c, std::int64_t ms) const {
        const auto found = fresh_points.find(ms);
        if (found != fresh_points.end()) return found->second;
        return fresh_points[ms] = Probe::fresh_point(c, ms);
    }
    void forget_fresh() {
        fresh_intervals.clear();
        fresh_points.clear();
    }

    const NativeExecutionConsumer& consumer() const {
        return static_cast<const NativeExecutionConsumer&>(execution_consumer());
    }

    void verify(const char* where, std::int64_t label, bool around_too = false) const {
        const NativeExecutionConsumer& c = consumer();
        // Every answer the memos hold is a fresh resolution's.
        const auto& cache = Probe::cache(c);
        if (cache.input_held) {
            ++prior_checks;
            SAME(where, same_interval(cache.input_interval, fresh(c, false, cache.input_ts)));
        }
        if (cache.script_held) {
            ++prior_checks;
            SAME(where, same_interval(cache.script_interval, fresh(c, true, cache.script_ts)));
        }
        if (cache.input_prior.held) {
            ++prior_checks;
            SAME(where, same_interval(cache.input_prior.interval,
                                      fresh(c, false, cache.input_prior.ts)));
        }
        if (cache.script_prior.held) {
            ++prior_checks;
            SAME(where, same_interval(cache.script_prior.interval,
                                      fresh(c, true, cache.script_prior.ts)));
        }
        for (const auto& entry : Probe::points(c)) {
            if (!entry.held) continue;
            ++point_checks;
            SAME(where, same_point(entry.point, fresh_point(c, entry.ms)));
        }
        // And, once a bar, a lookup made here answers a fresh resolution, at
        // the bar's own label, the instants around it and a random instant
        // near it.
        if (!around_too || !Probe::running(c)) return;
        const std::int64_t random = closes % 4 == 0
            ? label + static_cast<std::int64_t>((*rng)() % 4000) * kMinute - 2000 * kMinute
            : label;
        const std::int64_t around[] = {label, label + step, label - step, random};
        for (std::int64_t ts : around) {
            SAME(where, same_interval(Probe::input(c, ts), fresh(c, false, ts)));
            SAME(where, same_interval(Probe::script(c, ts), fresh(c, true, ts)));
        }
        // Session points served for instants one to three minutes apart,
        // around the label and the step after it: consecutive asks that
        // straddle a session edge answer differently, and each must answer
        // its own instant.
        for (int k = -3; k <= 3; ++k) {
            const std::int64_t ms = label + step + k * kMinute;
            SAME(where, same_point(Probe::point(c, ms), fresh_point(c, ms)));
            SAME(where, same_point(Probe::point(c, label + k * kMinute),
                                   fresh_point(c, label + k * kMinute)));
        }
    }

    void on_native_input(const Bar& bar, const NativeInputContext&) override {
        verify("on_native_input", bar.timestamp);
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext& context) override {
        verify("on_native_bar_open", context.script_bar_open_ms);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        verify("on_native_bar", context.script_bar_open_ms, true);
        const int index = closes++;
        switch ((*rng)() % 5) {
        case 0: (void)submit({no::Transact{1.0}, "l", ""}); break;
        case 1: (void)submit({no::Transact{-1.0}, "s", ""}); break;
        case 2: (void)submit({no::Flatten{}, "x", ""}); break;
        default: break;
        }
        (void)index;
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext& context) override {
        verify("on_native_applied", context.script_bar_open_ms);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        verify("margin_check_allowed", point.cursor.point.open_ms);
        return NativeStrategyHost::margin_check_allowed(point);
    }
};

NativeRunSpec spec_for(const Zone& zone, const Shape& shape, bool tolerant,
                       std::uint64_t run_number) {
    NativeRunSpec s;
    s.identity = {"perf-l1-lookup-memos", run_number};
    s.input_tf = shape.input_tf;
    s.script_tf = shape.script_tf;
    s.slot_label_policy = tolerant ? NativeSlotLabelPolicy::FeedTolerant
                                   : NativeSlotLabelPolicy::Canonical;
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "stock";
    s.currency = "USD";
    s.timezone = zone.timezone;
    s.session = zone.session;
    s.initial_capital = 100000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

// In-session input slots of `zone` from its session open, on the input
// timeframe's grid: `n` of them, skipping closed time.
std::vector<Bar> session_bars(const Zone& zone, const Shape& shape, int n) {
    NativeRunSpec probe = spec_for(zone, shape, false, 1);
    auto calendar = native_calendar::parse_session(probe.session, probe.timezone);
    std::vector<Bar> bars;
    if (!calendar) return bars;
    double p = 100.0;
    for (std::int64_t t = zone.t0; static_cast<int>(bars.size()) < n; t += shape.step) {
        if (!native_calendar::in_session(*calendar, t)) continue;
        p += (static_cast<int>(bars.size() * 7919 % 11) - 5) * 0.05;
        bars.push_back(Bar{p, p + 0.5, p - 0.5, p + 0.1, 1.0, t});
    }
    return bars;
}

void memos_answer_as_fresh_resolutions() {
    std::mt19937_64 rng(0xA5A5DEADBEEF1234ULL);
    int runs = 0;
    int streams = 0;
    int completed = 0;
    for (int round = 0; round < 120; ++round) {
        const Zone& zone = kZones[rng() % (sizeof(kZones) / sizeof(kZones[0]))];
        const Shape& shape = kShapes[rng() % (sizeof(kShapes) / sizeof(kShapes[0]))];
        const bool tolerant = rng() % 2 == 0;
        MemoHost host;
        host.rng = &rng;
        host.step = shape.step;
        const int reuse = 1 + static_cast<int>(rng() % 2);
        for (int run = 0; run < reuse; ++run) {
            if (host.configure_native(spec_for(zone, shape, tolerant,
                                                static_cast<std::uint64_t>(run + 1))).status
                != NativeSetupStatus::Applied) {
                break;
            }
            host.forget_fresh();
            host.verify("after configure_native", zone.t0);
            host.closes = 0;
            const auto bars = session_bars(zone, shape, 40 + static_cast<int>(rng() % 80));
            ++runs;
            if (rng() % 3 == 0) {
                ++streams;
                const int warmup = 8;
                if (host.stream_begin(bars.data(), warmup, shape.input_tf, shape.script_tf)) {
                    for (std::size_t i = warmup; i < bars.size(); ++i) {
                        if (!host.stream_push_bar(bars[i])) break;
                        host.verify("after stream_push_bar", bars[i].timestamp);
                    }
                    (void)host.stream_end(false);
                }
            } else {
                host.run(bars.data(), static_cast<int>(bars.size()));
            }
            host.verify("after the run", zone.t0);
            if (host.native_state().kind == NativeLifecycleKind::Completed) ++completed;
        }
    }
    std::printf("  %d runs (%d streams, %d completed): %llu comparisons, %llu held slots"
                " and priors, %llu held session points checked\n",
                runs, streams, completed, static_cast<unsigned long long>(comparisons),
                static_cast<unsigned long long>(prior_checks),
                static_cast<unsigned long long>(point_checks));
    // Not vacuous: the memos held answers and were checked while they did.
    CHECK(completed > 80);
    CHECK(prior_checks > 10000);
    CHECK(point_checks > 10000);
    CHECK(comparisons > 50000);
}

}  // namespace

int main() {
    memos_answer_as_fresh_resolutions();
    std::printf("test_native_lookup_memos: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
