// test_chart_ema_batch_origin_seed -- the chart EMA seeding a run declares
// (series_origin_ms, or the raw chart_ema_na_warmup key) also governs the EMAs
// a script computes in its run preparation.
//
// A generated strategy overrides prepare_script_run(). It resets its state
// there and, when the engine allows precalculation (a batch run with no
// explicit timeframe, no magnifier and no stream), computes its top-level chart
// TA for the whole feed and reads that cache in the per-bar callback instead
// of computing per bar. GeneratedStyleHost below is that shape written by hand.
// The existing test_chart_ema_na_warmup covers the per-bar callback only.
//
// Public synthetic bars only: no TradingView measurement and no private data.
// Every expectation comes from the definition (oracle_ema, oracle_run) and
// never from an engine value.

#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
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

// At most four differing bars are printed, then their count.
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

enum class Rule { None, ShortBreakdown, LongBreakout };

struct Script {
    int fast_default = 3;
    int slow_default = 3;
    Rule rule = Rule::None;
    int cooldown = 0;  // bars between two entry signals, counted from bar 0
    int hold = 0;      // bars from the entry signal to the close signal
};

struct OracleRun {
    std::vector<int> entry_signals;
    std::vector<int> exit_signals;
};

// The script's rules over the definition's EMAs. A market order placed on one
// bar fills at the next bar's open, so the script first sees the new position
// on the bar after its signal.
OracleRun oracle_run(const std::vector<Bar>& bars, const Script& script, int fast_length,
                     int slow_length, bool sma_seed) {
    const std::vector<double> fast = oracle_ema(bars, fast_length, sma_seed);
    const std::vector<double> slow = oracle_ema(bars, slow_length, sma_seed);
    const bool is_short = script.rule == Rule::ShortBreakdown;
    OracleRun out;
    int last_signal = 0;
    int signal = -1;
    int pending = 0;
    bool in_position = false;
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        if (pending == 1) in_position = true;
        if (pending == -1) in_position = false;
        pending = 0;
        const bool have = !std::isnan(fast[i]) && !std::isnan(slow[i]);
        const bool gate = have && (is_short ? fast[i] < slow[i] : fast[i] > slow[i]);
        const bool trigger = i > 0 && (is_short ? bars[i].close < bars[i - 1].low
                                                : bars[i].close > bars[i - 1].high);
        const bool cooled = i - last_signal >= script.cooldown;
        if (gate && trigger && cooled && !in_position) {
            out.entry_signals.push_back(i);
            last_signal = i;
            signal = i;
            pending = 1;
        } else if (in_position && signal >= 0 && i - signal >= script.hold) {
            out.exit_signals.push_back(i);
            pending = -1;
        }
    }
    return out;
}

// A generated strategy's shape: lifecycle reset, then the static TA cache when
// the engine allows it; the per-bar callback reads the cache or computes.
class GeneratedStyleHost final : public source::PineStrategyHost {
public:
    explicit GeneratedStyleHost(const Script& script) : script_(script) {}

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        fast_length_ = get_input_int("Fast", script_.fast_default);
        slow_length_ = get_input_int("Slow", script_.slow_default);
        fast_.emplace(fast_length_);
        slow_.emplace(slow_length_);
        fast_cache_.clear();
        slow_cache_.clear();
        used_cache = false;
        precalc_bars = 0;
        precalc_first_ms = -1;
        cache_reads = 0;
        cache_misses = 0;
        per_bar_computes = 0;
        previous_low_ = na<double>();
        previous_high_ = na<double>();
        last_signal_bar_ = 0;
        signal_bar_ = -1;
        dispatched_ms.clear();
        fast_seen.clear();
        slow_seen.clear();
        signals.clear();
        if (allow_precalculation && bars != nullptr && n > 0) {
            source::PineEma fast(fast_length_);
            source::PineEma slow(slow_length_);
            fast_cache_.reserve(static_cast<std::size_t>(n));
            slow_cache_.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                fast_cache_.push_back(fast.compute(bars[i].close));
                slow_cache_.push_back(slow.compute(bars[i].close));
            }
            precalc_bars = n;
            precalc_first_ms = bars[0].timestamp;
            used_cache = true;
        }
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        dispatched_ms.push_back(bar.timestamp);
        double fast = na<double>();
        double slow = na<double>();
        if (used_cache) {
            if (index >= 0 && static_cast<std::size_t>(index) < fast_cache_.size()) {
                fast = fast_cache_[static_cast<std::size_t>(index)];
                slow = slow_cache_[static_cast<std::size_t>(index)];
                ++cache_reads;
            } else {
                ++cache_misses;
            }
        } else {
            fast = fast_->compute(bar.close);
            slow = slow_->compute(bar.close);
            ++per_bar_computes;
        }
        fast_seen.push_back(fast);
        slow_seen.push_back(slow);
        if (script_.rule != Rule::None) {
            const bool is_short = script_.rule == Rule::ShortBreakdown;
            const double position = signed_position_size();
            const bool have = !is_na(fast) && !is_na(slow);
            const bool gate = have && (is_short ? fast < slow : fast > slow);
            const bool trigger = is_short
                ? (!is_na(previous_low_) && bar.close < previous_low_)
                : (!is_na(previous_high_) && bar.close > previous_high_);
            const bool cooled = index - last_signal_bar_ >= script_.cooldown;
            if (gate && trigger && cooled && position == 0.0) {
                strategy_entry(is_short ? "S" : "L", !is_short, na<double>(), na<double>(), 1.0);
                last_signal_bar_ = index;
                signal_bar_ = index;
                signals.push_back(index);
            } else if (position != 0.0 && signal_bar_ >= 0
                       && index - signal_bar_ >= script_.hold) {
                strategy_close_all();
            }
        }
        previous_low_ = bar.low;
        previous_high_ = bar.high;
    }

    int preparations = 0;
    bool allowed = false;
    bool used_cache = false;
    int precalc_bars = 0;
    std::int64_t precalc_first_ms = -1;
    int cache_reads = 0;
    int cache_misses = 0;
    int per_bar_computes = 0;
    std::vector<std::int64_t> dispatched_ms;
    std::vector<double> fast_seen;
    std::vector<double> slow_seen;
    std::vector<int> signals;

private:
    Script script_;
    int fast_length_ = 0;
    int slow_length_ = 0;
    std::optional<source::PineEma> fast_;
    std::optional<source::PineEma> slow_;
    std::vector<double> fast_cache_;
    std::vector<double> slow_cache_;
    double previous_low_ = na<double>();
    double previous_high_ = na<double>();
    int last_signal_bar_ = 0;
    int signal_bar_ = -1;
};

struct Setup {
    Script script;
    std::vector<std::pair<std::string, double>> metadata;
    std::vector<std::pair<std::string, std::string>> inputs;
    std::optional<std::int64_t> trade_start_ms;
};

struct Observed {
    std::string error;
    int preparations = 0;
    bool allowed = false;
    bool used_cache = false;
    int precalc_bars = 0;
    std::int64_t precalc_first_ms = -1;
    int cache_reads = 0;
    int cache_misses = 0;
    int per_bar_computes = 0;
    std::vector<std::int64_t> dispatched_ms;
    std::vector<double> fast;
    std::vector<double> slow;
    std::vector<int> signals;
    std::vector<Trade> trades;
    bool ambient_after = false;
};

// timeframe == nullptr is the batch run that allows precalculation; a named
// timeframe is the same feed on the per-bar path.
Observed run_script(const Setup& setup, const std::vector<Bar>& bars, const char* timeframe) {
    ta::ema_na_warmup_flag() = false;
    GeneratedStyleHost host(setup.script);
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    for (const auto& input : setup.inputs) host.set_input(input.first, input.second);
    for (const auto& item : setup.metadata) host.set_syminfo_metadata(item.first, item.second);
    if (setup.trade_start_ms) host.set_trade_start_time(*setup.trade_start_ms);
    if (timeframe == nullptr) {
        host.run(bars.data(), static_cast<int>(bars.size()));
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()), timeframe, timeframe);
    }
    Observed out;
    out.error = host.last_error();
    out.preparations = host.preparations;
    out.allowed = host.allowed;
    out.used_cache = host.used_cache;
    out.precalc_bars = host.precalc_bars;
    out.precalc_first_ms = host.precalc_first_ms;
    out.cache_reads = host.cache_reads;
    out.cache_misses = host.cache_misses;
    out.per_bar_computes = host.per_bar_computes;
    out.dispatched_ms = host.dispatched_ms;
    out.fast = host.fast_seen;
    out.slow = host.slow_seen;
    out.signals = host.signals;
    for (int i = 0; i < host.trade_count(); ++i) out.trades.push_back(host.get_trade(i));
    out.ambient_after = ta::ema_na_warmup_flag();
    ta::ema_na_warmup_flag() = false;
    return out;
}

int bar_of(const std::vector<Bar>& bars, std::int64_t time_ms) {
    for (std::size_t i = 0; i < bars.size(); ++i)
        if (bars[i].timestamp == time_ms) return static_cast<int>(i);
    return -1;
}

void print_observed(const char* leg, const Observed& o, const std::vector<Bar>& bars) {
    std::printf("OBS [%s] %s: allowed=%d used_cache=%d precalc_bars=%d precalc_first_ms=%lld "
                "cache_reads=%d per_bar_computes=%d dispatched=%zu first_dispatched_ms=%lld "
                "fast_first_finite=%d slow_first_finite=%d\n",
                row.c_str(), leg, o.allowed ? 1 : 0, o.used_cache ? 1 : 0, o.precalc_bars,
                static_cast<long long>(o.precalc_first_ms), o.cache_reads, o.per_bar_computes,
                o.dispatched_ms.size(),
                static_cast<long long>(o.dispatched_ms.empty() ? -1 : o.dispatched_ms.front()),
                first_finite(o.fast), first_finite(o.slow));
    for (const int signal : o.signals)
        std::printf("OBS [%s] %s: entry signal on bar %d\n", row.c_str(), leg, signal);
    std::printf("OBS [%s] %s: closed trades=%zu\n", row.c_str(), leg, o.trades.size());
    for (const Trade& trade : o.trades)
        std::printf("OBS [%s] %s: trade %s entry_ms=%lld entry_bar=%d exit_ms=%lld exit_bar=%d\n",
                    row.c_str(), leg, trade.is_long ? "long" : "short",
                    static_cast<long long>(trade.entry_time), bar_of(bars, trade.entry_time),
                    static_cast<long long>(trade.exit_time), bar_of(bars, trade.exit_time));
}

void check_run_path(const char* leg, const Observed& o, const std::vector<Bar>& bars,
                    bool cached) {
    const std::string name(leg);
    const long long n = static_cast<long long>(bars.size());
    const long long first_ms = bars.front().timestamp;
    if (!o.error.empty())
        std::printf("FAIL [%s] %s: run error: %s\n", row.c_str(), leg, o.error.c_str());
    expect(o.error.empty(), name + ": the run reports no error");
    expect_int(o.preparations, 1, name + ": preparations");
    expect(o.allowed == cached, name + ": the engine's precalculation flag");
    expect(o.used_cache == cached, name + ": the script's cache choice");
    expect_int(o.precalc_bars, cached ? n : 0, name + ": bars precalculated");
    expect_int(o.precalc_first_ms, cached ? first_ms : -1, name + ": first precalculated bar");
    expect_int(o.cache_reads, cached ? n : 0, name + ": values read from the cache");
    expect_int(o.per_bar_computes, cached ? 0 : n, name + ": values computed per bar");
    expect_int(o.cache_misses, 0, name + ": cache reads outside the cache");
    expect_int(static_cast<long long>(o.dispatched_ms.size()), n, name + ": bars dispatched");
    expect_int(o.dispatched_ms.empty() ? -1 : o.dispatched_ms.front(), first_ms,
               name + ": first dispatched bar");
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

// A length-three (or input-length) top-level chart EMA, read from the cache.
void cached_series_row(const char* name, const std::vector<double>& closes, int fast_input,
                       const char* key, const std::vector<double>* literal) {
    row = name;
    const int before = failures;
    const std::vector<Bar> bars = minute_bars(closes);
    Setup setup;
    setup.script = Script{3, 3, Rule::None, 0, 0};
    if (fast_input > 0) setup.inputs.push_back({"Fast", std::to_string(fast_input)});
    const std::string metadata_key(key);
    setup.metadata.push_back({metadata_key, metadata_key == "series_origin_ms"
                                                ? static_cast<double>(bars.front().timestamp)
                                                : 1.0});
    const int length = fast_input > 0 ? fast_input : 3;
    const std::vector<double> expected = oracle_ema(bars, length, /*sma_seed=*/true);
    const Observed cached = run_script(setup, bars, nullptr);
    const Observed per_bar = run_script(setup, bars, "1");
    print_observed("cached", cached, bars);
    print_observed("per-bar", per_bar, bars);
    for (std::size_t i = 0; i < cached.fast.size(); ++i)
        std::printf("OBS [%s] bar %zu close=%.17g cached=%.17g per-bar=%.17g definition=%.17g\n",
                    row.c_str(), i, bars[i].close, cached.fast[i],
                    i < per_bar.fast.size() ? per_bar.fast[i] : na<double>(),
                    i < expected.size() ? expected[i] : na<double>());
    check_run_path("cached", cached, bars, /*cached=*/true);
    check_run_path("per-bar", per_bar, bars, /*cached=*/false);
    expect_series(per_bar.fast, expected, false, "per-bar chart EMA against the SMA-seed definition");
    expect_series(cached.fast, expected, false, "cached chart EMA against the SMA-seed definition");
    expect_series(cached.fast, per_bar.fast, true, "cached chart EMA against the per-bar series");
    if (literal != nullptr)
        expect_series(cached.fast, *literal, true, "cached chart EMA against the written values");
    expect_int(first_finite(cached.fast), length - 1, "cached chart EMA: first finite bar");
    finish_row(before);
}

constexpr std::int64_t kQuarter = 15 * 60'000;
constexpr std::int64_t kIntradayStart = 1'704'067'200'000;  // 2024-01-01T00:00:00Z

// A falling 15-minute feed: every close is below the previous bar's low.
std::vector<Bar> falling_feed(int count) {
    std::vector<Bar> bars;
    double previous_close = 210.5;
    for (int i = 0; i < count; ++i) {
        const double wiggle = 0.0625 * static_cast<double>((i * 7) % 5 - 2);
        const double close = 210.0 - 0.5 * static_cast<double>(i) + wiggle;
        const double open = previous_close;
        bars.push_back(Bar{open, std::max(open, close) + 0.25, std::min(open, close) - 0.125,
                           close, 100.0 + static_cast<double>(i),
                           kIntradayStart + static_cast<std::int64_t>(i) * kQuarter});
        previous_close = close;
    }
    return bars;
}

void check_trading_leg(const char* leg, const Observed& o, const std::vector<Bar>& bars,
                       const Script& script, const OracleRun& plan, bool is_long) {
    const std::string name(leg);
    const std::vector<double> fast = oracle_ema(bars, script.fast_default, /*sma_seed=*/true);
    const std::vector<double> slow = oracle_ema(bars, script.slow_default, /*sma_seed=*/true);
    expect_int(first_finite(o.fast), script.fast_default - 1, name + ": fast EMA first finite bar");
    expect_int(first_finite(o.slow), script.slow_default - 1, name + ": slow EMA first finite bar");
    expect_series(o.fast, fast, false, name + ": fast EMA against the SMA-seed definition");
    expect_series(o.slow, slow, false, name + ": slow EMA against the SMA-seed definition");
    expect_int(static_cast<long long>(o.signals.size()),
               static_cast<long long>(plan.entry_signals.size()), name + ": entry signals");
    expect_int(static_cast<long long>(o.trades.size()),
               static_cast<long long>(plan.entry_signals.size()), name + ": closed trades");
    for (std::size_t i = 0; i < plan.entry_signals.size(); ++i) {
        const int signal = plan.entry_signals[i];
        if (i < o.signals.size())
            expect_int(o.signals[i], signal, name + ": entry signal bar");
        if (i >= o.trades.size()) continue;
        const Trade& trade = o.trades[i];
        expect_int(trade.entry_time, bars[static_cast<std::size_t>(signal) + 1].timestamp,
                   name + ": entry time");
        expect_int(bar_of(bars, trade.entry_time), signal + 1, name + ": entry bar");
        expect(trade.is_long == is_long, name + ": entry side");
        if (i < plan.exit_signals.size())
            expect_int(bar_of(bars, trade.exit_time), plan.exit_signals[i] + 1,
                       name + ": exit bar");
    }
}

// Fast 30 / slow 50 EMAs, a one-bar breakdown and a 48-bar cooldown: with a
// first-close seed the short is placed on bar 48, with the SMA seed the slow
// EMA is na until bar 49 and the short is placed there.
void early_entry_row(const char* name, const std::vector<Bar>& bars, std::int64_t origin_ms,
                     std::optional<std::int64_t> trade_start_ms) {
    row = name;
    const int before = failures;
    Setup setup;
    setup.script = Script{30, 50, Rule::ShortBreakdown, 48, 6};
    setup.metadata.push_back({"series_origin_ms", static_cast<double>(origin_ms)});
    setup.trade_start_ms = trade_start_ms;
    const OracleRun sma = oracle_run(bars, setup.script, 30, 50, /*sma_seed=*/true);
    const OracleRun first_close = oracle_run(bars, setup.script, 30, 50, /*sma_seed=*/false);
    expect_int(static_cast<long long>(sma.entry_signals.size()), 1,
               "definition: SMA-seeded entry signals");
    expect_int(sma.entry_signals.empty() ? -1 : sma.entry_signals.front(), 49,
               "definition: SMA-seeded entry signal bar");
    expect_int(static_cast<long long>(first_close.entry_signals.size()), 1,
               "definition: first-close entry signals");
    expect_int(first_close.entry_signals.empty() ? -1 : first_close.entry_signals.front(), 48,
               "definition: first-close entry signal bar");
    std::printf("OBS [%s] supplied bars=%zu first_bar_ms=%lld declared_origin_ms=%lld "
                "bars_before_origin=%d trade_start_ms=%lld\n",
                row.c_str(), bars.size(), static_cast<long long>(bars.front().timestamp),
                static_cast<long long>(origin_ms), bar_of(bars, origin_ms),
                static_cast<long long>(trade_start_ms ? *trade_start_ms : -1));
    const Observed cached = run_script(setup, bars, nullptr);
    const Observed per_bar = run_script(setup, bars, "15");
    print_observed("cached", cached, bars);
    print_observed("per-bar", per_bar, bars);
    check_run_path("cached", cached, bars, /*cached=*/true);
    check_run_path("per-bar", per_bar, bars, /*cached=*/false);
    check_trading_leg("per-bar", per_bar, bars, setup.script, sma, /*is_long=*/false);
    check_trading_leg("cached", cached, bars, setup.script, sma, /*is_long=*/false);
    expect_series(cached.fast, per_bar.fast, true, "cached fast EMA against the per-bar series");
    expect_series(cached.slow, per_bar.slow, true, "cached slow EMA against the per-bar series");
    finish_row(before);
}

constexpr std::int64_t kDay = 86'400'000;
constexpr std::int64_t kDailyStart = 1'577'836'800'000;  // 2020-01-01T00:00:00Z

// Daily closes 100 (bars 0-99), 120 (100-199), 109 (200-310). Bar 99 carries a
// high of 125 so the step to 120 is no one-bar breakout. Bars 250, 262 and 274
// close at 109.5 above a previous high of 109: the only breakouts in the feed.
std::vector<Bar> daily_feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 311; ++i) {
        const double level = i < 100 ? 100.0 : (i < 200 ? 120.0 : 109.0);
        Bar bar{level, level, level, level, 1000.0,
                kDailyStart + static_cast<std::int64_t>(i) * kDay};
        if (i == 99) bar.high = 125.0;
        if (i == 250 || i == 262 || i == 274) {
            bar.close = 109.5;
            bar.high = 109.5;
        }
        bars.push_back(bar);
    }
    return bars;
}

// Fast 50 / slow 200 EMAs. On bar 250 the SMA-seeded spread is above zero and
// the first-close spread below it, and the first-close spread stays below zero
// on the two later breakouts, so a first-close run misses the entry for good.
void missed_gate_row() {
    row = "missed gate: daily EMA 50/200, breakout 250 bars from the declared origin";
    const int before = failures;
    const std::vector<Bar> bars = daily_feed();
    Setup setup;
    setup.script = Script{50, 200, Rule::LongBreakout, 0, 30};
    setup.metadata.push_back({"series_origin_ms", static_cast<double>(bars.front().timestamp)});
    const std::vector<double> sma_fast = oracle_ema(bars, 50, true);
    const std::vector<double> sma_slow = oracle_ema(bars, 200, true);
    const std::vector<double> first_fast = oracle_ema(bars, 50, false);
    const std::vector<double> first_slow = oracle_ema(bars, 200, false);
    int breakouts = 0;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (!(bars[i].close > bars[i - 1].high)) continue;
        ++breakouts;
        std::printf("OBS [%s] definition: breakout bar %zu spread sma=%.17g first-close=%.17g\n",
                    row.c_str(), i, sma_fast[i] - sma_slow[i], first_fast[i] - first_slow[i]);
        expect(first_fast[i] - first_slow[i] < -0.5,
               "definition: the first-close spread is below zero on every breakout");
    }
    expect_int(breakouts, 3, "definition: breakout bars in the feed");
    expect(sma_fast[250] - sma_slow[250] > 0.5,
           "definition: the SMA-seeded spread is above zero on bar 250");
    const OracleRun sma = oracle_run(bars, setup.script, 50, 200, /*sma_seed=*/true);
    const OracleRun first_close = oracle_run(bars, setup.script, 50, 200, /*sma_seed=*/false);
    expect_int(static_cast<long long>(sma.entry_signals.size()), 1,
               "definition: SMA-seeded entry signals");
    expect_int(sma.entry_signals.empty() ? -1 : sma.entry_signals.front(), 250,
               "definition: SMA-seeded entry signal bar");
    expect_int(static_cast<long long>(first_close.entry_signals.size()), 0,
               "definition: first-close entry signals");
    const Observed cached = run_script(setup, bars, nullptr);
    const Observed per_bar = run_script(setup, bars, "1D");
    print_observed("cached", cached, bars);
    print_observed("per-bar", per_bar, bars);
    check_run_path("cached", cached, bars, /*cached=*/true);
    check_run_path("per-bar", per_bar, bars, /*cached=*/false);
    check_trading_leg("per-bar", per_bar, bars, setup.script, sma, /*is_long=*/true);
    check_trading_leg("cached", cached, bars, setup.script, sma, /*is_long=*/true);
    expect_series(cached.fast, per_bar.fast, true, "cached fast EMA against the per-bar series");
    expect_series(cached.slow, per_bar.slow, true, "cached slow EMA against the per-bar series");
    finish_row(before);
}

}  // namespace

int main() {
    const std::vector<double> written_10_20_30{na<double>(), na<double>(), 20.0,
                                               19.0, 30.5, 30.25};
    cached_series_row("cached EMA 3 over closes 10 20 30 18 42 30, declared origin",
                      {10.0, 20.0, 30.0, 18.0, 42.0, 30.0}, 0, "series_origin_ms",
                      &written_10_20_30);
    const std::vector<double> written_12_18_33{na<double>(), na<double>(), 21.0,
                                               21.0, 15.0, 21.0};
    cached_series_row("cached EMA 3 over changed closes 12 18 33 21 9 27, declared origin",
                      {12.0, 18.0, 33.0, 21.0, 9.0, 27.0}, 0, "series_origin_ms",
                      &written_12_18_33);
    cached_series_row("cached EMA with the length input changed to 2, declared origin",
                      {10.0, 20.0, 30.0, 18.0, 42.0, 30.0}, 2, "series_origin_ms", nullptr);
    cached_series_row("cached EMA 3 over closes 10 20 30 18 42 30, raw chart key",
                      {10.0, 20.0, 30.0, 18.0, 42.0, 30.0}, 0, "chart_ema_na_warmup",
                      &written_10_20_30);

    // Ninety supplied bars; the declared origin is bar 20.
    const std::vector<Bar> supplied = falling_feed(90);
    const std::int64_t origin_ms = supplied[20].timestamp;
    // The reference run fed from its origin: the twenty earlier bars are not loaded.
    const std::vector<Bar> from_origin(supplied.begin() + 20, supplied.end());
    early_entry_row("early entry: intraday EMA 30/50, feed starts on the declared origin",
                    from_origin, origin_ms, std::nullopt);
    // The whole supplied feed, with a trading window that opens on bar 30.
    early_entry_row("early entry: intraday EMA 30/50, twenty supplied bars before the origin",
                    supplied, origin_ms, supplied[30].timestamp);

    missed_gate_row();

    std::printf("chart EMA batch origin seed: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
