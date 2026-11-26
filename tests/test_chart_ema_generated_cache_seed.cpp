// test_chart_ema_generated_cache_seed -- a chart ta.ema is na until `length`
// closes exist and then starts from their simple average, with nothing
// declared, whether a generated strategy reads it from the static TA cache it
// fills in its run preparation or computes it per bar; chart_ema_na_warmup = 0
// asks for the first-close seed on both paths.
//
// For `x = ta.ema(close, 3)` the code generator emits a member built with the
// literal, a `_precalc__ta_ema_1` vector, a `_use_precalc` flag, a
// prepare_script_run() that resets all three and calls precalculate() when the
// engine allows it, and a per-bar statement that reads the vector while the
// flag is set and computes otherwise. LiteralLengthStrategy below is that
// shape, statement for statement as the public corpus's generated.cpp files
// carry it. InputLengthStrategy is the same script with the length read from
// an input: its member is rebuilt from the input on the first bar and its
// per-bar statement always computes, so its cache is filled and never read.
// test_chart_ema_batch_origin_seed covers the checked adapter type with
// input lengths; this file covers the plain class with a literal length, the
// two shapes side by side, and one host reused across run modes.
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

int first_finite(const std::vector<double>& values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!std::isnan(values[i])) return static_cast<int>(i);
    return -1;
}

// Definition only. sma_seed: na for length - 1 bars, then the simple average of
// the first `length` closes, then the recursion. Otherwise the first close
// seeds the recursion.
std::vector<double> oracle_ema(const std::vector<Bar>& bars, int length, bool sma_seed) {
    std::vector<double> values(bars.size(), na<double>());
    const double alpha = 2.0 / (static_cast<double>(length) + 1.0);
    double previous = na<double>();
    double sum = 0.0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double close = bars[i].close;
        if (sma_seed && i < static_cast<std::size_t>(length)) {
            sum += close;
            if (i + 1 == static_cast<std::size_t>(length))
                previous = sum / static_cast<double>(length);
        } else if (!sma_seed && i == 0) {
            previous = close;
        } else {
            previous = previous + alpha * (close - previous);
        }
        values[i] = previous;
    }
    return values;
}

// `x = ta.ema(close, 3)`: the generated shape for a constant length.
class LiteralLengthStrategy final : public source::PineStrategyHost {
public:
    ta::EMA _ta_ema_1;
    std::vector<double> _precalc__ta_ema_1;
    bool _use_precalc = false;
    double x = 0.0;

    // What the rows read back; no generated member.
    int preparations = 0;
    bool allowed = false;
    bool seeding_in_preparation = false;
    int precalc_bars = 0;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> seen;

    LiteralLengthStrategy() : _ta_ema_1(3) {}

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        seeding_in_preparation = ta::ema_na_warmup_flag();
        precalc_bars = 0;
        cache_reads = 0;
        per_bar_computes = 0;
        seen.clear();
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
        x = (_use_precalc ? _precalc__ta_ema_1[bar_index_] : (history_advances_new_bar() ? _ta_ema_1.compute(current_bar_.close) : _ta_ema_1.recompute(current_bar_.close)));
        seen.push_back(x);
    }

    void precalculate(const Bar* bars, int n) {
        _use_precalc = false;
        if (n <= 0 || bars == nullptr) return;

        _precalc__ta_ema_1.resize(n);

        _ta_ema_1 = ta::EMA(3);

        for (int i = 0; i < n; ++i) {
            _precalc__ta_ema_1[i] = _ta_ema_1.compute(bars[i].close);
        }

        _ta_ema_1 = ta::EMA(3);

        precalc_bars = n;
        _use_precalc = true;
    }
};

// `len = input.int(3, "Length")` and `x = ta.ema(close, len)`: the generated
// shape for an input length. precalculate() fills a vector no statement reads.
class InputLengthStrategy final : public source::PineStrategyHost {
public:
    ta::EMA _ta_ema_1;
    std::vector<double> _precalc__ta_ema_1;
    bool _use_precalc = false;
    int len = 0;
    double x = 0.0;
    bool _ta_initialized_ = false;
    bool _inputs_initialized_ = false;

    // What the rows read back; no generated member.
    int preparations = 0;
    bool allowed = false;
    bool seeding_in_preparation = false;
    int precalc_bars = 0;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> seen;

    InputLengthStrategy() : _ta_ema_1(3) {}

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        seeding_in_preparation = ta::ema_na_warmup_flag();
        precalc_bars = 0;
        cache_reads = 0;
        per_bar_computes = 0;
        seen.clear();
        this->_ta_ema_1 = decltype(this->_ta_ema_1)(3);
        this->_precalc__ta_ema_1 = decltype(this->_precalc__ta_ema_1){};
        this->_use_precalc = false;
        this->len = 0;
        this->x = 0.0;
        this->_ta_initialized_ = false;
        this->_inputs_initialized_ = false;
        if (allow_precalculation) precalculate(bars, n);
    }

    void on_source_bar(const Bar& bar) override {
        (void)bar;
        if (!_inputs_initialized_) {
            len = get_input_int("Length", 3);
            _inputs_initialized_ = true;
        }
        if (!_ta_initialized_) {
            _ta_ema_1 = ta::EMA(get_input_int("Length", 3));
            _ta_initialized_ = true;
        }
        ++per_bar_computes;
        x = (history_advances_new_bar() ? _ta_ema_1.compute(current_bar_.close) : _ta_ema_1.recompute(current_bar_.close));
        seen.push_back(x);
    }

    void precalculate(const Bar* bars, int n) {
        _use_precalc = false;
        if (n <= 0 || bars == nullptr) return;

        _precalc__ta_ema_1.resize(n);

        _ta_ema_1 = ta::EMA(get_input_int("Length", 3));

        for (int i = 0; i < n; ++i) {
            _precalc__ta_ema_1[i] = _ta_ema_1.compute(bars[i].close);
        }

        _ta_ema_1 = ta::EMA(get_input_int("Length", 3));

        precalc_bars = n;
        _use_precalc = true;
    }
};

struct Observed {
    std::string error;
    int preparations = 0;
    bool allowed = false;
    bool seeding_in_preparation = false;
    int precalc_bars = 0;
    int cache_reads = 0;
    int per_bar_computes = 0;
    bool use_precalc_after = false;
    std::size_t cache_size_after = 0;
    std::vector<double> seen;
    bool ambient_after = false;
};

// timeframe == nullptr is the batch run that allows precalculation; a named
// timeframe is the same feed on the per-bar path.
template <class Host>
Observed run_once(Host& host, const std::vector<Bar>& bars, const char* timeframe) {
    ta::ema_na_warmup_flag() = false;
    if (timeframe == nullptr) {
        host.run(bars.data(), static_cast<int>(bars.size()));
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()), timeframe, timeframe);
    }
    Observed out;
    out.error = host.last_error();
    out.preparations = host.preparations;
    out.allowed = host.allowed;
    out.seeding_in_preparation = host.seeding_in_preparation;
    out.precalc_bars = host.precalc_bars;
    out.cache_reads = host.cache_reads;
    out.per_bar_computes = host.per_bar_computes;
    out.use_precalc_after = host._use_precalc;
    out.cache_size_after = host._precalc__ta_ema_1.size();
    out.seen = host.seen;
    out.ambient_after = ta::ema_na_warmup_flag();
    ta::ema_na_warmup_flag() = false;
    return out;
}

using Metadata = std::vector<std::pair<std::string, double>>;

template <class Host>
void configure(Host& host, const Metadata& metadata) {
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    for (const auto& item : metadata) host.set_syminfo_metadata(item.first, item.second);
}

void print_observed(const char* leg, const Observed& o) {
    std::printf("OBS [%s] %s: preparations=%d allowed=%d seeding_in_preparation=%d "
                "precalc_bars=%d cache_reads=%d per_bar_computes=%d use_precalc_after=%d "
                "cache_size_after=%zu first_finite=%d ambient_after=%d\n",
                row.c_str(), leg, o.preparations, o.allowed ? 1 : 0,
                o.seeding_in_preparation ? 1 : 0, o.precalc_bars, o.cache_reads,
                o.per_bar_computes, o.use_precalc_after ? 1 : 0, o.cache_size_after,
                first_finite(o.seen), o.ambient_after ? 1 : 0);
    for (std::size_t i = 0; i < o.seen.size(); ++i)
        std::printf("OBS [%s] %s: bar %zu value=%.17g\n", row.c_str(), leg, i, o.seen[i]);
}

enum class Path {
    CacheRead,       // the literal shape in a batch run: every bar read from the cache
    CacheUnread,     // the input-length shape in a batch run: cache filled, every bar computed
    PerBar,          // a run with timeframes: no precalculation, every bar computed
};

void check_path(const char* leg, const Observed& o, const std::vector<Bar>& bars, Path path,
                int preparations) {
    const std::string name(leg);
    const long long n = static_cast<long long>(bars.size());
    const bool batch = path != Path::PerBar;
    if (!o.error.empty())
        std::printf("FAIL [%s] %s: run error: %s\n", row.c_str(), leg, o.error.c_str());
    expect(o.error.empty(), name + ": the run reports no error");
    expect_int(o.preparations, preparations, name + ": preparations");
    expect(o.allowed == batch, name + ": the engine's precalculation flag");
    expect_int(o.precalc_bars, batch ? n : 0, name + ": bars precalculated");
    expect(o.use_precalc_after == batch, name + ": the cache flag after the run");
    expect_int(static_cast<long long>(o.cache_size_after), batch ? n : 0,
               name + ": cache entries after the run");
    expect_int(o.cache_reads, path == Path::CacheRead ? n : 0,
               name + ": values read from the cache");
    expect_int(o.per_bar_computes, path == Path::CacheRead ? 0 : n,
               name + ": values computed per bar");
    expect_int(static_cast<long long>(o.seen.size()), n, name + ": bars dispatched");
    expect(!o.ambient_after, name + ": the ambient seeding default is restored");
}

void finish_row(int failures_before) {
    std::printf("%s [%s]\n", failures == failures_before ? "PASS" : "FAIL", row.c_str());
}

std::vector<Bar> minute_bars(const std::vector<double>& closes) {
    std::vector<Bar> bars;
    for (const double close : closes)
        bars.push_back(Bar{close, close, close, close, 1.0,
                           static_cast<std::int64_t>(bars.size() + 1) * 60'000});
    return bars;
}

// The metadata of a row, with series_origin_ms set to the feed's first bar.
Metadata applied_to(const Metadata& declared, const std::vector<Bar>& bars) {
    Metadata applied;
    for (const auto& item : declared)
        applied.push_back({item.first, item.first == "series_origin_ms"
                                           ? static_cast<double>(bars.front().timestamp)
                                           : item.second});
    return applied;
}

// One literal-length strategy on the cache and one on the per-bar path, under
// the same metadata. An empty list declares nothing: the default, which is the
// SMA seed; chart_ema_na_warmup = 0 asks for the first-close seed.
void literal_row(const char* name, const std::vector<double>& closes, const Metadata& declared,
                 bool sma_seed, const std::vector<double>& written) {
    row = name;
    const int before = failures;
    const std::vector<Bar> bars = minute_bars(closes);
    const Metadata metadata = applied_to(declared, bars);
    const std::vector<double> expected = oracle_ema(bars, 3, sma_seed);
    LiteralLengthStrategy cached_host;
    configure(cached_host, metadata);
    const Observed cached = run_once(cached_host, bars, nullptr);
    LiteralLengthStrategy per_bar_host;
    configure(per_bar_host, metadata);
    const Observed per_bar = run_once(per_bar_host, bars, "1");
    print_observed("cached", cached);
    print_observed("per-bar", per_bar);
    check_path("cached", cached, bars, Path::CacheRead, 1);
    check_path("per-bar", per_bar, bars, Path::PerBar, 1);
    expect(cached.seeding_in_preparation == sma_seed,
           "cached: the seeding default inside the batch preparation is the chart's");
    expect(!per_bar.seeding_in_preparation,
           "per-bar: the preparation of a run with timeframes is outside the chart scope");
    expect_series(expected, written, false, "definition against the written values");
    expect_series(per_bar.seen, written, true, "per-bar chart EMA against the written values");
    expect_series(per_bar.seen, expected, false, "per-bar chart EMA against the definition");
    expect_series(cached.seen, expected, false, "cached chart EMA against the definition");
    expect_series(cached.seen, written, true, "cached chart EMA against the written values");
    expect_series(cached.seen, per_bar.seen, true, "cached chart EMA against the per-bar series");
    expect_int(first_finite(cached.seen), sma_seed ? 2 : 0, "cached chart EMA: first finite bar");
    finish_row(before);
}

// The literal-length strategy against its input-length twin in the same batch
// run: one reads the cache, the other computes per bar.
void twin_row(const char* name, const std::vector<double>& closes, int length_input,
              const Metadata& declared, bool sma_seed) {
    row = name;
    const int before = failures;
    const std::vector<Bar> bars = minute_bars(closes);
    const Metadata metadata = applied_to(declared, bars);
    const int length = length_input > 0 ? length_input : 3;
    const std::vector<double> expected = oracle_ema(bars, length, sma_seed);
    InputLengthStrategy twin_host;
    configure(twin_host, metadata);
    if (length_input > 0) twin_host.set_input("Length", std::to_string(length_input));
    const Observed twin = run_once(twin_host, bars, nullptr);
    print_observed("input-length twin", twin);
    check_path("input-length twin", twin, bars, Path::CacheUnread, 1);
    expect_series(twin.seen, expected, false, "input-length twin against the definition");
    expect_int(first_finite(twin.seen), sma_seed ? length - 1 : 0,
               "input-length twin: first finite bar");
    if (length == 3) {
        LiteralLengthStrategy literal_host;
        configure(literal_host, metadata);
        const Observed literal = run_once(literal_host, bars, nullptr);
        print_observed("literal", literal);
        check_path("literal", literal, bars, Path::CacheRead, 1);
        expect_series(literal.seen, twin.seen, true,
                      "literal-length cache against its input-length twin");
    }
    finish_row(before);
}

// One host, three runs: batch, with timeframes, batch again. Nothing declared.
void reuse_row() {
    row = "one host reused, nothing declared: batch, then with timeframes, then batch";
    const int before = failures;
    const std::vector<Bar> bars = minute_bars({10.0, 20.0, 30.0, 18.0, 42.0, 30.0});
    const std::vector<double> expected = oracle_ema(bars, 3, /*sma_seed=*/true);
    LiteralLengthStrategy host;
    configure(host, {});
    const Observed first = run_once(host, bars, nullptr);
    const Observed second = run_once(host, bars, "1");
    const Observed third = run_once(host, bars, nullptr);
    print_observed("first batch", first);
    print_observed("with timeframes", second);
    print_observed("second batch", third);
    check_path("first batch", first, bars, Path::CacheRead, 1);
    check_path("with timeframes", second, bars, Path::PerBar, 2);
    check_path("second batch", third, bars, Path::CacheRead, 3);
    expect(first.seeding_in_preparation, "first batch: chart seeding inside the preparation");
    expect(!second.seeding_in_preparation,
           "with timeframes: the preparation is outside the chart scope");
    expect(third.seeding_in_preparation, "second batch: chart seeding inside the preparation");
    expect_series(first.seen, expected, false, "first batch against the definition");
    expect_series(second.seen, expected, false, "with timeframes against the definition");
    expect_series(third.seen, expected, false, "second batch against the definition");
    expect_series(first.seen, second.seen, true, "first batch against the run with timeframes");
    expect_series(third.seen, second.seen, true, "second batch against the run with timeframes");
    finish_row(before);
}

// One host, three batch runs: nothing declared, the chart seeding switched off
// on purpose, an origin declared after that. Each run's cache is its own.
void redeclared_row() {
    row = "one host, three batch runs: nothing declared, switched off, origin declared";
    const int before = failures;
    const std::vector<Bar> bars = minute_bars({10.0, 20.0, 30.0, 18.0, 42.0, 30.0});
    const std::vector<double> first_close = oracle_ema(bars, 3, /*sma_seed=*/false);
    const std::vector<double> sma = oracle_ema(bars, 3, /*sma_seed=*/true);
    LiteralLengthStrategy host;
    configure(host, {});
    const Observed undeclared = run_once(host, bars, nullptr);
    host.set_syminfo_metadata("chart_ema_na_warmup", 0.0);
    const Observed switched_off = run_once(host, bars, nullptr);
    host.set_syminfo_metadata("series_origin_ms", static_cast<double>(bars.front().timestamp));
    const Observed declared = run_once(host, bars, nullptr);
    print_observed("nothing declared", undeclared);
    print_observed("switched off", switched_off);
    print_observed("origin declared", declared);
    check_path("nothing declared", undeclared, bars, Path::CacheRead, 1);
    check_path("switched off", switched_off, bars, Path::CacheRead, 2);
    check_path("origin declared", declared, bars, Path::CacheRead, 3);
    expect(undeclared.seeding_in_preparation,
           "nothing declared: chart seeding inside the preparation");
    expect(!switched_off.seeding_in_preparation,
           "switched off: no chart seeding inside the preparation");
    expect(declared.seeding_in_preparation,
           "origin declared: chart seeding inside the preparation");
    expect_series(undeclared.seen, sma, false, "nothing declared against the SMA-seed definition");
    expect_series(switched_off.seen, first_close, false,
                  "switched off against the first-close definition");
    expect_series(declared.seen, sma, false, "origin declared against the SMA-seed definition");
    expect_series(declared.seen, undeclared.seen, true, "origin declared against nothing declared");
    finish_row(before);
}

}  // namespace

int main() {
    const double none = na<double>();
    const std::vector<double> closes{10.0, 20.0, 30.0, 18.0, 42.0, 30.0};
    const std::vector<double> changed{12.0, 18.0, 33.0, 21.0, 9.0, 27.0};
    const std::vector<double> sma_closes{none, none, 20.0, 19.0, 30.5, 30.25};
    const std::vector<double> sma_changed{none, none, 21.0, 21.0, 15.0, 21.0};
    const std::vector<double> first_closes{10.0, 15.0, 22.5, 20.25, 31.125, 30.5625};
    const std::vector<double> first_changed{12.0, 15.0, 24.0, 22.5, 15.75, 21.375};
    // Nothing declared: the default is the SMA seed, on the cache and per bar.
    literal_row("literal EMA 3 over closes 10 20 30 18 42 30, nothing declared", closes, {},
                /*sma_seed=*/true, sma_closes);
    literal_row("literal EMA 3 over changed closes 12 18 33 21 9 27, nothing declared", changed,
                {}, /*sma_seed=*/true, sma_changed);
    // A declared origin and the raw chart key ask for what the default gives.
    literal_row("literal EMA 3 over closes 10 20 30 18 42 30, declared origin", closes,
                {{"series_origin_ms", 0.0}}, /*sma_seed=*/true, sma_closes);
    literal_row("literal EMA 3 over closes 10 20 30 18 42 30, chart_ema_na_warmup=1", closes,
                {{"chart_ema_na_warmup", 1.0}}, /*sma_seed=*/true, sma_closes);
    // chart_ema_na_warmup = 0 is the first-close seed, on purpose.
    literal_row("literal EMA 3 over closes 10 20 30 18 42 30, chart_ema_na_warmup=0", closes,
                {{"chart_ema_na_warmup", 0.0}}, /*sma_seed=*/false, first_closes);
    literal_row("literal EMA 3 over changed closes 12 18 33 21 9 27, chart_ema_na_warmup=0",
                changed, {{"chart_ema_na_warmup", 0.0}}, /*sma_seed=*/false, first_changed);
    literal_row("literal EMA 3, declared origin, then chart_ema_na_warmup=0", closes,
                {{"series_origin_ms", 0.0}, {"chart_ema_na_warmup", 0.0}}, /*sma_seed=*/false,
                first_closes);
    twin_row("literal EMA 3 against its input-length twin, nothing declared", closes, 0, {},
             /*sma_seed=*/true);
    twin_row("input-length twin with the length input changed to 2, nothing declared", closes, 2,
             {}, /*sma_seed=*/true);
    twin_row("literal EMA 3 against its input-length twin, chart_ema_na_warmup=0", closes, 0,
             {{"chart_ema_na_warmup", 0.0}}, /*sma_seed=*/false);
    reuse_row();
    redeclared_row();

    std::printf("chart EMA generated cache seed: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
