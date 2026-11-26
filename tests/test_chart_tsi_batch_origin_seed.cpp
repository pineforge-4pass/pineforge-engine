// test_chart_tsi_batch_origin_seed -- the chart seeding a run declares also
// governs the EMAs inside a ta.tsi a script computes in its run preparation.
//
// ta.tsi(source, short, long) is the ratio of two double-smoothed series: the
// source's bar-to-bar change, and that change's absolute value, each through
// an EMA of `long` and then an EMA of `short`. With the chart seeding declared
// the inner EMAs are na until `long` changes exist and the outer ones until
// `short` inner values exist, so the first value is on bar long + short - 1;
// with nothing declared every EMA starts from its first input and the first
// value is on bar 1. TsiStrategy is the generated shape of
// test_chart_ema_generated_cache_seed with the checked ta.tsi adapter and
// constant lengths.
//
// Public synthetic bars only: no TradingView measurement and no private data.
// Every expectation comes from the definition (oracle_tsi), never from an
// engine value.

#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;
int checks = 0;
std::string row;

void expect(bool ok, const std::string& what) {
    ++checks;
    if (ok) return;
    std::printf("FAIL [%s] %s\n", row.c_str(), what.c_str());
    ++failures;
}

void expect_int(long long observed, long long expected, const std::string& what) {
    ++checks;
    if (observed == expected) return;
    std::printf("FAIL [%s] %s: observed %lld, expected %lld\n", row.c_str(), what.c_str(),
                observed, expected);
    ++failures;
}

bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

bool near(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
}

void expect_series(const std::vector<double>& observed, const std::vector<double>& expected,
                   bool exact, const std::string& what) {
    ++checks;
    if (observed.size() != expected.size()) {
        std::printf("FAIL [%s] %s: observed %zu bars, expected %zu\n", row.c_str(),
                    what.c_str(), observed.size(), expected.size());
        ++failures;
        return;
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < observed.size(); ++i) {
        if (exact ? same(observed[i], expected[i]) : near(observed[i], expected[i])) continue;
        if (++differing <= 4)
            std::printf("FAIL [%s] %s: bar %zu observed %.17g, expected %.17g\n", row.c_str(),
                        what.c_str(), i, observed[i], expected[i]);
    }
    if (differing == 0) return;
    std::printf("FAIL [%s] %s: %zu of %zu bars differ\n", row.c_str(), what.c_str(), differing,
                observed.size());
    ++failures;
}

int first_finite(const std::vector<double>& values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!std::isnan(values[i])) return static_cast<int>(i);
    return -1;
}

// Definition only, over an input that may be missing. A missing input gives na
// and changes nothing. sma_seed: na until `length` inputs have been seen, then
// their simple average, then the recursion. Otherwise the first input seeds
// the recursion.
std::vector<double> oracle_ema(const std::vector<double>& inputs, int length, bool sma_seed) {
    std::vector<double> values(inputs.size(), na<double>());
    const double alpha = 2.0 / (static_cast<double>(length) + 1.0);
    double previous = na<double>();
    double sum = 0.0;
    int samples = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const double input = inputs[i];
        if (std::isnan(input)) continue;
        if (sma_seed && samples < length) {
            sum += input;
            ++samples;
            if (samples < length) continue;
            previous = sum / static_cast<double>(length);
        } else if (!sma_seed && std::isnan(previous)) {
            previous = input;
        } else {
            previous = previous + alpha * (input - previous);
        }
        values[i] = previous;
    }
    return values;
}

// Definition only: the double-smoothed change over the double-smoothed
// absolute change; na where either is na or the divisor is zero.
std::vector<double> oracle_tsi(const std::vector<Bar>& bars, int short_length, int long_length,
                               bool sma_seed) {
    std::vector<double> change(bars.size(), na<double>());
    std::vector<double> magnitude(bars.size(), na<double>());
    for (std::size_t i = 1; i < bars.size(); ++i) {
        change[i] = bars[i].close - bars[i - 1].close;
        magnitude[i] = std::fabs(change[i]);
    }
    const std::vector<double> smoothed =
        oracle_ema(oracle_ema(change, long_length, sma_seed), short_length, sma_seed);
    const std::vector<double> smoothed_magnitude =
        oracle_ema(oracle_ema(magnitude, long_length, sma_seed), short_length, sma_seed);
    std::vector<double> values(bars.size(), na<double>());
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (std::isnan(smoothed[i]) || std::isnan(smoothed_magnitude[i])) continue;
        if (smoothed_magnitude[i] == 0.0) continue;
        values[i] = smoothed[i] / smoothed_magnitude[i];
    }
    return values;
}

constexpr int kShort = 3;
constexpr int kLong = 5;

// `x = ta.tsi(close, 3, 5)`: the generated shape for constant lengths.
class TsiStrategy final : public source::PineStrategyHost {
public:
    source::PineTsi _ta_tsi_1;
    std::vector<double> _precalc__ta_tsi_1;
    bool _use_precalc = false;
    double x = 0.0;

    // What the rows read back; no generated member.
    int preparations = 0;
    bool allowed = false;
    int precalc_bars = 0;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> seen;

    TsiStrategy() : _ta_tsi_1(kShort, kLong) {}

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        precalc_bars = 0;
        cache_reads = 0;
        per_bar_computes = 0;
        seen.clear();
        this->_ta_tsi_1 = decltype(this->_ta_tsi_1)(kShort, kLong);
        this->_precalc__ta_tsi_1 = decltype(this->_precalc__ta_tsi_1){};
        this->_use_precalc = false;
        this->x = 0.0;
        if (allow_precalculation) precalculate(bars, n);
    }

    void on_source_bar(const Bar& bar) override {
        (void)bar;
        if (_use_precalc) ++cache_reads;
        else ++per_bar_computes;
        x = (_use_precalc ? _precalc__ta_tsi_1[bar_index_] : (history_advances_new_bar() ? _ta_tsi_1.compute(current_bar_.close) : _ta_tsi_1.recompute(current_bar_.close)));
        seen.push_back(x);
    }

    void precalculate(const Bar* bars, int n) {
        _use_precalc = false;
        if (n <= 0 || bars == nullptr) return;

        _precalc__ta_tsi_1.resize(n);

        _ta_tsi_1 = source::PineTsi(kShort, kLong);

        for (int i = 0; i < n; ++i) {
            _precalc__ta_tsi_1[i] = _ta_tsi_1.compute(bars[i].close);
        }

        _ta_tsi_1 = source::PineTsi(kShort, kLong);

        precalc_bars = n;
        _use_precalc = true;
    }
};

struct Observed {
    std::string error;
    int preparations = 0;
    bool allowed = false;
    int precalc_bars = 0;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> seen;
    bool ambient_after = false;
};

using Metadata = std::vector<std::pair<std::string, double>>;

// timeframe == nullptr is the batch run that allows precalculation; a named
// timeframe is the same feed on the per-bar path.
Observed run_script(const Metadata& metadata, const std::vector<Bar>& bars,
                    const char* timeframe) {
    ta::ema_na_warmup_flag() = false;
    TsiStrategy host;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    for (const auto& item : metadata) host.set_syminfo_metadata(item.first, item.second);
    if (timeframe == nullptr) {
        host.run(bars.data(), static_cast<int>(bars.size()));
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()), timeframe, timeframe);
    }
    Observed out;
    out.error = host.last_error();
    out.preparations = host.preparations;
    out.allowed = host.allowed;
    out.precalc_bars = host.precalc_bars;
    out.cache_reads = host.cache_reads;
    out.per_bar_computes = host.per_bar_computes;
    out.seen = host.seen;
    out.ambient_after = ta::ema_na_warmup_flag();
    ta::ema_na_warmup_flag() = false;
    return out;
}

void check_path(const char* leg, const Observed& o, const std::vector<Bar>& bars, bool cached) {
    const std::string name(leg);
    const long long n = static_cast<long long>(bars.size());
    if (!o.error.empty())
        std::printf("FAIL [%s] %s: run error: %s\n", row.c_str(), leg, o.error.c_str());
    expect(o.error.empty(), name + ": the run reports no error");
    expect_int(o.preparations, 1, name + ": preparations");
    expect(o.allowed == cached, name + ": the engine's precalculation flag");
    expect_int(o.precalc_bars, cached ? n : 0, name + ": bars precalculated");
    expect_int(o.cache_reads, cached ? n : 0, name + ": values read from the cache");
    expect_int(o.per_bar_computes, cached ? 0 : n, name + ": values computed per bar");
    expect_int(static_cast<long long>(o.seen.size()), n, name + ": bars dispatched");
    expect(!o.ambient_after, name + ": the ambient seeding default is restored");
}

void finish_row(int failures_before) {
    std::printf("%s [%s]\n", failures == failures_before ? "PASS" : "FAIL", row.c_str());
}

constexpr std::int64_t kQuarter = 15 * 60'000;
constexpr std::int64_t kStart = 1'704'067'200'000;  // 2024-01-01T00:00:00Z

// A 15-minute feed that rises for eight bars and falls for eight.
std::vector<Bar> swing_feed(int count) {
    std::vector<Bar> bars;
    double previous_close = 100.0;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 16;
        const double triangle = static_cast<double>(phase < 8 ? phase : 16 - phase);
        const double wiggle = 0.0625 * static_cast<double>((i * 7) % 5 - 2);
        const double close = 100.0 + 1.5 * triangle + wiggle + 0.03125 * static_cast<double>(i);
        const double open = previous_close;
        bars.push_back(Bar{open, std::max(open, close) + 0.5, std::min(open, close) - 0.5,
                           close, 100.0 + static_cast<double>(i),
                           kStart + static_cast<std::int64_t>(i) * kQuarter});
        previous_close = close;
    }
    return bars;
}

// The metadata of a row, with series_origin_ms set to the feed's first bar.
Metadata applied_to(const Metadata& metadata, const std::vector<Bar>& bars) {
    Metadata applied;
    for (const auto& item : metadata)
        applied.push_back({item.first, item.first == "series_origin_ms"
                                           ? static_cast<double>(bars.front().timestamp)
                                           : item.second});
    return applied;
}

void tsi_row(const char* name, const Metadata& metadata, bool sma_seed) {
    row = name;
    const int before = failures;
    const std::vector<Bar> bars = swing_feed(40);
    const Metadata applied = applied_to(metadata, bars);
    const std::vector<double> expected = oracle_tsi(bars, kShort, kLong, sma_seed);
    const std::vector<double> other = oracle_tsi(bars, kShort, kLong, !sma_seed);
    const Observed cached = run_script(applied, bars, nullptr);
    const Observed per_bar = run_script(applied, bars, "15");
    std::printf("OBS [%s] cached: allowed=%d precalc_bars=%d cache_reads=%d per_bar_computes=%d "
                "first_finite=%d\n",
                row.c_str(), cached.allowed ? 1 : 0, cached.precalc_bars, cached.cache_reads,
                cached.per_bar_computes, first_finite(cached.seen));
    std::printf("OBS [%s] per-bar: allowed=%d precalc_bars=%d cache_reads=%d per_bar_computes=%d "
                "first_finite=%d\n",
                row.c_str(), per_bar.allowed ? 1 : 0, per_bar.precalc_bars, per_bar.cache_reads,
                per_bar.per_bar_computes, first_finite(per_bar.seen));
    for (std::size_t i = 0; i < bars.size() && i < 12; ++i)
        std::printf("OBS [%s] bar %zu close=%.17g cached=%.17g per-bar=%.17g definition=%.17g "
                    "other-seeding=%.17g\n",
                    row.c_str(), i, bars[i].close,
                    i < cached.seen.size() ? cached.seen[i] : na<double>(),
                    i < per_bar.seen.size() ? per_bar.seen[i] : na<double>(), expected[i],
                    other[i]);
    check_path("cached", cached, bars, /*cached=*/true);
    check_path("per-bar", per_bar, bars, /*cached=*/false);
    // The definition tells the two seedings apart on this feed.
    const int first_expected = sma_seed ? kLong + kShort - 1 : 1;
    expect_int(first_finite(expected), first_expected, "definition: first finite bar");
    expect_int(first_finite(other), sma_seed ? 1 : kLong + kShort - 1,
               "definition: first finite bar under the other seeding");
    int finite = 0;
    int differing = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!std::isnan(expected[i])) ++finite;
        if (!near(expected[i], other[i])) ++differing;
    }
    expect(finite >= 30, "definition: at least thirty finite values");
    expect(differing >= 8, "definition: the two seedings differ on at least eight bars");
    expect_int(first_finite(per_bar.seen), first_expected, "per-bar ta.tsi: first finite bar");
    expect_int(first_finite(cached.seen), first_expected, "cached ta.tsi: first finite bar");
    expect_series(per_bar.seen, expected, false, "per-bar ta.tsi against the definition");
    expect_series(cached.seen, expected, false, "cached ta.tsi against the definition");
    expect_series(cached.seen, per_bar.seen, true, "cached ta.tsi against the per-bar series");
    finish_row(before);
}

}  // namespace

int main() {
    tsi_row("ta.tsi 3/5, declared origin", {{"series_origin_ms", 0.0}}, /*sma_seed=*/true);
    tsi_row("ta.tsi 3/5, raw chart key", {{"chart_ema_na_warmup", 1.0}}, /*sma_seed=*/true);
    tsi_row("ta.tsi 3/5, nothing declared", {}, /*sma_seed=*/false);
    tsi_row("ta.tsi 3/5, declared origin, then chart_ema_na_warmup=0 on purpose",
            {{"series_origin_ms", 0.0}, {"chart_ema_na_warmup", 0.0}}, /*sma_seed=*/false);

    std::printf("chart ta.tsi batch origin seed: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
