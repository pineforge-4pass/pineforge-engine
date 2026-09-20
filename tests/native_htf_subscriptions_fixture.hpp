#pragma once
// Shared fixture of the R5 L6 higher-timeframe subscription witnesses: the
// check harness, the civil-time helper, the base run spec, the recording
// SeriesHost and the hand-aggregation oracle. It is source-free so that
// tests/test_native_htf_subscriptions.cpp (the native half) runs under
// PINEFORGE_BUILD_SOURCE_LAYER=OFF, while
// tests/test_native_htf_subscriptions_twin.cpp (the Pine-path twin) binds
// pineforge/source and is registered only when the source layer is built.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace l6_fixture {
using namespace pineforge;

inline int checks = 0;
inline int failures = 0;
inline const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++l6_fixture::checks;                                                  \
        if (!(expression)) {                                                   \
            ++l6_fixture::failures;                                            \
            std::printf("FAIL [%s] line %d: %s\n", l6_fixture::scenario,      \
                        __LINE__, #expression);                                \
        }                                                                      \
    } while (false)

inline bool same(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
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

inline NativeRunSpec base_spec(const char* input, const char* script,
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
    // The push side of barmerge.gaps_on: clear_security calls per sec_id.
    std::map<int, int> clears;
    // Optional per-calculation probe: what native_series_bar(i) answered at
    // each script bar, for every i < probes. Zero — the default — records
    // nothing, so every scenario that does not ask for it is unchanged.
    std::size_t probes = 0;
    std::vector<std::vector<std::optional<Bar>>> series_at_bar;

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

    void clear_security(int sec_id) override { ++clears[sec_id]; }

    // How each calculated script bar was sealed: Confirmed by an input of
    // its own, LazyComplete by the first input of a later interval.
    std::vector<NativeCompletionKind> completions;

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

inline std::vector<Bar> quarter_hour_bars(int n) {
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
inline Bar hand_aggregate(const std::vector<Bar>& bars, int from, int count,
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

inline void check_bucket(const Bar& got, const Bar& want, const char* tag) {
    const bool ok = got.timestamp == want.timestamp && same(got.open, want.open)
        && same(got.high, want.high) && same(got.low, want.low)
        && same(got.close, want.close) && same(got.volume, want.volume);
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

}  // namespace l6_fixture
