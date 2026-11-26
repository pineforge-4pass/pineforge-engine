// test_chart_ema_batch_interior_na -- a chart EMA whose source is missing on a
// bar inside its warm state, read from a script's run preparation cache and
// computed per bar.
//
// The source of the EMA below is `volume == 0 ? na : close`: every bar has a
// valid open, high, low and close, and a bar with no volume has no source.
// On such a bar ta.ema answers na and keeps its state; the next bar with a
// source continues from the last valid EMA. A missing bar is no sample: the
// warm-up needs `length` bars that have a source. MissingSourceStrategy is the
// generated shape of test_chart_ema_generated_cache_seed with that source.
// The rows run with nothing declared (the default: na, then the simple
// average), with a declared origin, and with chart_ema_na_warmup = 0 (the
// first-close seed).
//
// Wrong answers the rows tell apart: the missing bar's close taken as the
// source, the previous EMA repeated on the missing bar, and a recursion that
// starts again after it.
//
// Public synthetic bars only: no TradingView measurement and no private data.
// Every expectation comes from the definition (oracle_ema) or is written out
// in the row, never from an engine value.

#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
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

// Definition only, over a source that may be missing. A missing source gives
// na and changes nothing. sma_seed: na until `length` sources have been seen,
// then their simple average, then the recursion. Otherwise the first source
// seeds the recursion.
std::vector<double> oracle_ema(const std::vector<double>& sources, int length, bool sma_seed) {
    std::vector<double> values(sources.size(), na<double>());
    const double alpha = 2.0 / (static_cast<double>(length) + 1.0);
    double previous = na<double>();
    double sum = 0.0;
    int samples = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const double source = sources[i];
        if (std::isnan(source)) continue;
        if (sma_seed && samples < length) {
            sum += source;
            ++samples;
            if (samples < length) continue;
            previous = sum / static_cast<double>(length);
        } else if (!sma_seed && std::isnan(previous)) {
            previous = source;
        } else {
            previous = previous + alpha * (source - previous);
        }
        values[i] = previous;
    }
    return values;
}

// `x = ta.ema(volume == 0 ? na : close, 3)`: the generated shape for a
// constant length, with a source that a bar may lack.
class MissingSourceStrategy final : public source::PineStrategyHost {
public:
    ta::EMA _ta_ema_1;
    std::vector<double> _precalc__ta_ema_1;
    bool _use_precalc = false;
    double x = 0.0;

    // What the rows read back; no generated member.
    int preparations = 0;
    bool allowed = false;
    int precalc_bars = 0;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> seen;
    std::vector<double> sources_seen;

    MissingSourceStrategy() : _ta_ema_1(3) {}

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        precalc_bars = 0;
        cache_reads = 0;
        per_bar_computes = 0;
        seen.clear();
        sources_seen.clear();
        this->_ta_ema_1 = decltype(this->_ta_ema_1)(3);
        this->_precalc__ta_ema_1 = decltype(this->_precalc__ta_ema_1){};
        this->_use_precalc = false;
        this->x = 0.0;
        if (allow_precalculation) precalculate(bars, n);
    }

    void on_source_bar(const Bar& bar) override {
        (void)bar;
        if (_use_precalc) ++cache_reads;
        else ++per_bar_computes;
        sources_seen.push_back((current_bar_.volume == 0.0 ? na<double>() : current_bar_.close));
        x = (_use_precalc ? _precalc__ta_ema_1[bar_index_] : (history_advances_new_bar() ? _ta_ema_1.compute((current_bar_.volume == 0.0 ? na<double>() : current_bar_.close)) : _ta_ema_1.recompute((current_bar_.volume == 0.0 ? na<double>() : current_bar_.close))));
        seen.push_back(x);
    }

    void precalculate(const Bar* bars, int n) {
        _use_precalc = false;
        if (n <= 0 || bars == nullptr) return;

        _precalc__ta_ema_1.resize(n);

        _ta_ema_1 = ta::EMA(3);

        for (int i = 0; i < n; ++i) {
            _precalc__ta_ema_1[i] = _ta_ema_1.compute((bars[i].volume == 0.0 ? na<double>() : bars[i].close));
        }

        _ta_ema_1 = ta::EMA(3);

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
    std::vector<double> sources_seen;
    bool ambient_after = false;
};

enum class Declared {
    Nothing,     // the default: the SMA seed
    Origin,      // series_origin_ms: the SMA seed
    FirstClose,  // chart_ema_na_warmup = 0: the first-close seed, on purpose
};

// timeframe == nullptr is the batch run that allows precalculation; a named
// timeframe is the same feed on the per-bar path.
Observed run_script(Declared declared, const std::vector<Bar>& bars, const char* timeframe) {
    ta::ema_na_warmup_flag() = false;
    MissingSourceStrategy host;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    if (declared == Declared::Origin)
        host.set_syminfo_metadata("series_origin_ms", static_cast<double>(bars.front().timestamp));
    if (declared == Declared::FirstClose) host.set_syminfo_metadata("chart_ema_na_warmup", 0.0);
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
    out.sources_seen = host.sources_seen;
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

// One minute bar per source. A bar whose source is na carries the valid close
// `missing_close` and no volume; every other bar closes at its source.
std::vector<Bar> bars_for(const std::vector<double>& sources, double missing_close) {
    std::vector<Bar> bars;
    for (const double source : sources) {
        const bool missing = std::isnan(source);
        const double close = missing ? missing_close : source;
        bars.push_back(Bar{close, close + 1.0, close - 1.0, close, missing ? 0.0 : 1.0,
                           static_cast<std::int64_t>(bars.size() + 1) * 60'000});
    }
    return bars;
}

void series_row(const char* name, const std::vector<double>& sources, double missing_close,
                Declared declared, const std::vector<double>& written) {
    row = name;
    const int before = failures;
    const std::vector<Bar> bars = bars_for(sources, missing_close);
    const bool sma_seed = declared != Declared::FirstClose;
    const std::vector<double> expected = oracle_ema(sources, 3, sma_seed);
    // The same definition if the missing bars' closes were taken as sources.
    std::vector<double> closes;
    for (const Bar& bar : bars) closes.push_back(bar.close);
    const std::vector<double> close_taken = oracle_ema(closes, 3, sma_seed);
    const Observed cached = run_script(declared, bars, nullptr);
    const Observed per_bar = run_script(declared, bars, "1");
    for (std::size_t i = 0; i < bars.size(); ++i)
        std::printf("OBS [%s] bar %zu open=%.17g high=%.17g low=%.17g close=%.17g volume=%.17g "
                    "source=%.17g cached=%.17g per-bar=%.17g definition=%.17g close-taken=%.17g\n",
                    row.c_str(), i, bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                    bars[i].volume, sources[i],
                    i < cached.seen.size() ? cached.seen[i] : na<double>(),
                    i < per_bar.seen.size() ? per_bar.seen[i] : na<double>(), expected[i],
                    close_taken[i]);
    check_path("cached", cached, bars, /*cached=*/true);
    check_path("per-bar", per_bar, bars, /*cached=*/false);
    // The fixture: every bar's prices are valid, the script saw the written
    // sources, and taking a missing bar's close gives another series.
    bool prices_valid = true;
    for (const Bar& bar : bars)
        prices_valid = prices_valid && std::isfinite(bar.open) && std::isfinite(bar.high)
            && std::isfinite(bar.low) && std::isfinite(bar.close);
    expect(prices_valid, "fixture: every bar has a valid open, high, low and close");
    expect_series(cached.sources_seen, sources, true, "cached: the sources the script read");
    expect_series(per_bar.sources_seen, sources, true, "per-bar: the sources the script read");
    bool differs = false;
    for (std::size_t i = 0; i < expected.size(); ++i)
        differs = differs || !same(expected[i], close_taken[i]);
    expect(differs, "fixture: taking a missing bar's close would give another series");
    expect_series(expected, written, false, "definition against the written values");
    expect_series(per_bar.seen, expected, false, "per-bar chart EMA against the definition");
    expect_series(cached.seen, expected, false, "cached chart EMA against the definition");
    expect_series(per_bar.seen, written, true, "per-bar chart EMA against the written values");
    expect_series(cached.seen, written, true, "cached chart EMA against the written values");
    expect_series(cached.seen, per_bar.seen, true, "cached chart EMA against the per-bar series");
    finish_row(before);
}

}  // namespace

int main() {
    const double none = na<double>();
    // Nothing declared. Warm state, then one missing bar: na there, 20 -> 30
    // -> 40 after it.
    series_row("nothing declared, sources 10 20 30 na 40 50",
               {10.0, 20.0, 30.0, none, 40.0, 50.0}, 35.0, Declared::Nothing,
               {none, none, 20.0, none, 30.0, 40.0});
    series_row("nothing declared, changed sources 12 18 33 na 21 9",
               {12.0, 18.0, 33.0, none, 21.0, 9.0}, 60.0, Declared::Nothing,
               {none, none, 21.0, none, 21.0, 15.0});
    series_row("nothing declared, two missing bars in a row: 10 20 30 na na 40",
               {10.0, 20.0, 30.0, none, none, 40.0}, 35.0, Declared::Nothing,
               {none, none, 20.0, none, none, 30.0});
    // A missing bar inside the warm-up is no sample: the seed is one bar later.
    series_row("nothing declared, a missing bar inside the warm-up: 10 na 20 30 40 50",
               {10.0, none, 20.0, 30.0, 40.0, 50.0}, 35.0, Declared::Nothing,
               {none, none, none, 20.0, 30.0, 40.0});
    // A declared origin asks for what the default gives.
    series_row("declared origin, sources 10 20 30 na 40 50",
               {10.0, 20.0, 30.0, none, 40.0, 50.0}, 35.0, Declared::Origin,
               {none, none, 20.0, none, 30.0, 40.0});
    // chart_ema_na_warmup = 0: the first-close recursion skips the missing bar too.
    series_row("chart_ema_na_warmup=0, sources 10 20 30 na 40 50",
               {10.0, 20.0, 30.0, none, 40.0, 50.0}, 35.0, Declared::FirstClose,
               {10.0, 15.0, 22.5, none, 31.25, 40.625});
    series_row("chart_ema_na_warmup=0, changed sources 12 18 33 na 21 9",
               {12.0, 18.0, 33.0, none, 21.0, 9.0}, 60.0, Declared::FirstClose,
               {12.0, 15.0, 24.0, none, 22.5, 15.75});

    std::printf("chart EMA batch interior na: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
