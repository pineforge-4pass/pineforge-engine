// test_chart_ema_batch_seed_invariants -- what the chart EMA seeding of a
// script's run preparation must leave as it is.
//
// The preparation (prepare_script_run) of a generated strategy computes its
// top-level chart TA for the whole feed when the engine allows it; see
// test_chart_ema_batch_origin_seed for the declared-origin behaviour. This
// file holds the runs that declare nothing, or switch the chart seeding off on
// purpose, or use no EMA at all: a feed-first-bar EMA script keeps its
// first-close seed on both paths, a deliberate chart_ema_na_warmup=0 after a
// declared origin wins, an RSI/ATR script is the same with and without a
// declaration, and a preparation that throws leaves the ambient default off.
//
// Public synthetic bars only: no TradingView measurement and no private data.
// The EMA expectations come from the definition (oracle_ema, oracle_run), never
// from an engine value. Every row also prints what it observed (OBS lines) so
// two builds can be compared line by line.

#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
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

// FNV-1a over the values' bit patterns: one number to compare two builds by.
std::uint64_t digest(const std::vector<double>& values) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const double value : values) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        if (std::isnan(value)) bits = 0x7ff8000000000000ULL;
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (bits >> (8 * byte)) & 0xffULL;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
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

struct Script {
    int fast_default = 3;
    int slow_default = 3;
    bool trades = false;  // the short breakdown rule below
    int cooldown = 0;     // bars between two entry signals, counted from bar 0
    int hold = 0;         // bars from the entry signal to the close signal
};

struct OracleRun {
    std::vector<int> entry_signals;
    std::vector<int> exit_signals;
};

// The script's rules over the definition's EMAs. A market order placed on one
// bar fills at the next bar's open, so the script first sees the new position
// on the bar after its signal.
OracleRun oracle_run(const std::vector<Bar>& bars, const Script& script, bool sma_seed) {
    const std::vector<double> fast = oracle_ema(bars, script.fast_default, sma_seed);
    const std::vector<double> slow = oracle_ema(bars, script.slow_default, sma_seed);
    OracleRun out;
    int last_signal = 0;
    int signal = -1;
    int pending = 0;
    bool in_position = false;
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        if (pending == 1) in_position = true;
        if (pending == -1) in_position = false;
        pending = 0;
        const bool gate = !std::isnan(fast[i]) && !std::isnan(slow[i]) && fast[i] < slow[i];
        const bool trigger = i > 0 && bars[i].close < bars[i - 1].low;
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
        fast_.emplace(script_.fast_default);
        slow_.emplace(script_.slow_default);
        fast_cache_.clear();
        slow_cache_.clear();
        used_cache = false;
        precalc_bars = 0;
        precalc_first_ms = -1;
        cache_reads = 0;
        cache_misses = 0;
        per_bar_computes = 0;
        previous_low_ = na<double>();
        last_signal_bar_ = 0;
        signal_bar_ = -1;
        dispatched_ms.clear();
        fast_seen.clear();
        slow_seen.clear();
        signals.clear();
        if (allow_precalculation && bars != nullptr && n > 0) {
            source::PineEma fast(script_.fast_default);
            source::PineEma slow(script_.slow_default);
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
        if (script_.trades) {
            const double position = signed_position_size();
            const bool gate = !is_na(fast) && !is_na(slow) && fast < slow;
            const bool trigger = !is_na(previous_low_) && bar.close < previous_low_;
            const bool cooled = index - last_signal_bar_ >= script_.cooldown;
            if (gate && trigger && cooled && position == 0.0) {
                strategy_entry("S", false, na<double>(), na<double>(), 1.0);
                last_signal_bar_ = index;
                signal_bar_ = index;
                signals.push_back(index);
            } else if (position != 0.0 && signal_bar_ >= 0
                       && index - signal_bar_ >= script_.hold) {
                strategy_close_all();
            }
        }
        previous_low_ = bar.low;
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
    std::optional<source::PineEma> fast_;
    std::optional<source::PineEma> slow_;
    std::vector<double> fast_cache_;
    std::vector<double> slow_cache_;
    double previous_low_ = na<double>();
    int last_signal_bar_ = 0;
    int signal_bar_ = -1;
};

// The same shape with no EMA: a top-level RSI 14 and ATR 14, long when the RSI
// is below 50 with a five-bar cooldown, closed three bars after the signal.
class NonEmaHost final : public source::PineStrategyHost {
public:
    explicit NonEmaHost(int last_entry_bar) : last_entry_bar_(last_entry_bar) {}

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        rsi_.emplace(14);
        atr_.emplace(14);
        rsi_cache_.clear();
        atr_cache_.clear();
        used_cache = false;
        precalc_bars = 0;
        precalc_first_ms = -1;
        cache_reads = 0;
        cache_misses = 0;
        per_bar_computes = 0;
        last_signal_bar_ = 0;
        signal_bar_ = -1;
        dispatched_ms.clear();
        fast_seen.clear();
        slow_seen.clear();
        signals.clear();
        if (allow_precalculation && bars != nullptr && n > 0) {
            ta::RSI rsi(14);
            ta::ATR atr(14);
            for (int i = 0; i < n; ++i) {
                rsi_cache_.push_back(rsi.compute(bars[i].close));
                atr_cache_.push_back(atr.compute(bars[i].high, bars[i].low, bars[i].close));
            }
            precalc_bars = n;
            precalc_first_ms = bars[0].timestamp;
            used_cache = true;
        }
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        dispatched_ms.push_back(bar.timestamp);
        double rsi = na<double>();
        double atr = na<double>();
        if (used_cache) {
            if (index >= 0 && static_cast<std::size_t>(index) < rsi_cache_.size()) {
                rsi = rsi_cache_[static_cast<std::size_t>(index)];
                atr = atr_cache_[static_cast<std::size_t>(index)];
                ++cache_reads;
            } else {
                ++cache_misses;
            }
        } else {
            rsi = rsi_->compute(bar.close);
            atr = atr_->compute(bar.high, bar.low, bar.close);
            ++per_bar_computes;
        }
        fast_seen.push_back(rsi);
        slow_seen.push_back(atr);
        const double position = signed_position_size();
        const bool gate = !is_na(rsi) && !is_na(atr) && atr > 0.0 && rsi < 50.0;
        const bool cooled = index - last_signal_bar_ >= 5;
        if (gate && cooled && position == 0.0 && index <= last_entry_bar_) {
            strategy_entry("L", true, na<double>(), na<double>(), 1.0);
            last_signal_bar_ = index;
            signal_bar_ = index;
            signals.push_back(index);
        } else if (position != 0.0 && signal_bar_ >= 0 && index - signal_bar_ >= 3) {
            strategy_close_all();
        }
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
    std::vector<double> fast_seen;  // the RSI the script read
    std::vector<double> slow_seen;  // the ATR the script read
    std::vector<int> signals;

private:
    int last_entry_bar_;
    std::optional<ta::RSI> rsi_;
    std::optional<ta::ATR> atr_;
    std::vector<double> rsi_cache_;
    std::vector<double> atr_cache_;
    int last_signal_bar_ = 0;
    int signal_bar_ = -1;
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

using Metadata = std::vector<std::pair<std::string, double>>;

// timeframe == nullptr is the batch run that allows precalculation; a named
// timeframe is the same feed on the per-bar path.
template <class Host>
Observed run_host(Host& host, const Metadata& metadata, const std::vector<Bar>& bars,
                  const char* timeframe) {
    ta::ema_na_warmup_flag() = false;
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

Observed run_script(const Script& script, const Metadata& metadata, const std::vector<Bar>& bars,
                    const char* timeframe) {
    GeneratedStyleHost host(script);
    return run_host(host, metadata, bars, timeframe);
}

int bar_of(const std::vector<Bar>& bars, std::int64_t time_ms) {
    for (std::size_t i = 0; i < bars.size(); ++i)
        if (bars[i].timestamp == time_ms) return static_cast<int>(i);
    return -1;
}

void print_observed(const char* leg, const Observed& o, const std::vector<Bar>& bars) {
    std::printf("OBS [%s] %s: allowed=%d used_cache=%d precalc_bars=%d precalc_first_ms=%lld "
                "cache_reads=%d per_bar_computes=%d dispatched=%zu first_dispatched_ms=%lld "
                "first_finite=%d/%d digest=%016llx/%016llx\n",
                row.c_str(), leg, o.allowed ? 1 : 0, o.used_cache ? 1 : 0, o.precalc_bars,
                static_cast<long long>(o.precalc_first_ms), o.cache_reads, o.per_bar_computes,
                o.dispatched_ms.size(),
                static_cast<long long>(o.dispatched_ms.empty() ? -1 : o.dispatched_ms.front()),
                first_finite(o.fast), first_finite(o.slow),
                static_cast<unsigned long long>(digest(o.fast)),
                static_cast<unsigned long long>(digest(o.slow)));
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

void expect_same_trades(const Observed& a, const Observed& b, const std::string& what) {
    expect_int(static_cast<long long>(a.trades.size()), static_cast<long long>(b.trades.size()),
               what + ": closed trades");
    expect(a.signals == b.signals, what + ": entry signal bars");
    for (std::size_t i = 0; i < a.trades.size() && i < b.trades.size(); ++i) {
        expect_int(a.trades[i].entry_time, b.trades[i].entry_time, what + ": entry time");
        expect_int(a.trades[i].exit_time, b.trades[i].exit_time, what + ": exit time");
        expect(a.trades[i].is_long == b.trades[i].is_long, what + ": entry side");
        expect(same(a.trades[i].entry_price, b.trades[i].entry_price), what + ": entry price");
        expect(same(a.trades[i].exit_price, b.trades[i].exit_price), what + ": exit price");
    }
}

void finish_row(int failures_before) {
    std::printf("%s [%s]\n", failures == failures_before ? "PASS" : "FAIL", row.c_str());
}

constexpr std::int64_t kQuarter = 15 * 60'000;
constexpr std::int64_t kIntradayStart = 1'704'067'200'000;  // 2024-01-01T00:00:00Z

// The falling 15-minute feed of test_chart_ema_batch_origin_seed: every close
// is below the previous bar's low.
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
                           kIntradayStart + static_cast<std::int64_t>(i) * kQuarter});
        previous_close = close;
    }
    return bars;
}

std::vector<Bar> minute_bars(const std::vector<double>& closes) {
    std::vector<Bar> bars;
    for (const double close : closes)
        bars.push_back(Bar{close, close, close, close, 1.0,
                           static_cast<std::int64_t>(bars.size() + 1) * 60'000});
    return bars;
}

// No declaration: the cached and the per-bar EMA 3 both start from the first close.
void undeclared_series_row() {
    row = "undeclared EMA 3 over closes 10 20 30 18 42 30 keeps the first-close seed";
    const int before = failures;
    const std::vector<Bar> bars = minute_bars({10.0, 20.0, 30.0, 18.0, 42.0, 30.0});
    const std::vector<double> written{10.0, 15.0, 22.5, 20.25, 31.125, 30.5625};
    const Script script{3, 3, false, 0, 0};
    const Observed cached = run_script(script, {}, bars, nullptr);
    const Observed per_bar = run_script(script, {}, bars, "1");
    print_observed("cached", cached, bars);
    print_observed("per-bar", per_bar, bars);
    check_run_path("cached", cached, bars, /*cached=*/true);
    check_run_path("per-bar", per_bar, bars, /*cached=*/false);
    expect_series(cached.fast, written, true, "cached chart EMA against the written values");
    expect_series(per_bar.fast, written, true, "per-bar chart EMA against the written values");
    expect_series(cached.fast, oracle_ema(bars, 3, /*sma_seed=*/false), false,
                  "cached chart EMA against the first-close definition");
    finish_row(before);
}

// The EMA 30/50 short breakdown script with a first-close seed: the cooldown
// ends on bar 48 and the short is placed there, on both paths.
void first_close_trading_row(const char* name, const Metadata& metadata) {
    row = name;
    const int before = failures;
    const std::vector<Bar> supplied = falling_feed(90);
    const std::vector<Bar> bars(supplied.begin() + 20, supplied.end());
    const Script script{30, 50, true, 48, 6};
    Metadata applied;
    for (const auto& item : metadata)
        applied.push_back({item.first, item.first == "series_origin_ms"
                                           ? static_cast<double>(bars.front().timestamp)
                                           : item.second});
    const OracleRun plan = oracle_run(bars, script, /*sma_seed=*/false);
    expect_int(static_cast<long long>(plan.entry_signals.size()), 1,
               "definition: first-close entry signals");
    expect_int(plan.entry_signals.empty() ? -1 : plan.entry_signals.front(), 48,
               "definition: first-close entry signal bar");
    const std::vector<double> fast = oracle_ema(bars, 30, /*sma_seed=*/false);
    const std::vector<double> slow = oracle_ema(bars, 50, /*sma_seed=*/false);
    const Observed cached = run_script(script, applied, bars, nullptr);
    const Observed per_bar = run_script(script, applied, bars, "15");
    print_observed("cached", cached, bars);
    print_observed("per-bar", per_bar, bars);
    check_run_path("cached", cached, bars, /*cached=*/true);
    check_run_path("per-bar", per_bar, bars, /*cached=*/false);
    const Observed* legs[] = {&cached, &per_bar};
    const char* names[] = {"cached", "per-bar"};
    for (int leg = 0; leg < 2; ++leg) {
        const Observed& o = *legs[leg];
        const std::string name_of(names[leg]);
        expect_int(first_finite(o.fast), 0, name_of + ": fast EMA first finite bar");
        expect_int(first_finite(o.slow), 0, name_of + ": slow EMA first finite bar");
        expect_series(o.fast, fast, false, name_of + ": fast EMA against the first-close definition");
        expect_series(o.slow, slow, false, name_of + ": slow EMA against the first-close definition");
        expect_int(static_cast<long long>(o.signals.size()), 1, name_of + ": entry signals");
        expect_int(o.signals.empty() ? -1 : o.signals.front(), 48, name_of + ": entry signal bar");
        expect_int(static_cast<long long>(o.trades.size()), 1, name_of + ": closed trades");
        if (o.trades.empty()) continue;
        expect_int(o.trades.front().entry_time, bars[49].timestamp, name_of + ": entry time");
        expect_int(bar_of(bars, o.trades.front().entry_time), 49, name_of + ": entry bar");
        expect(!o.trades.front().is_long, name_of + ": entry side");
    }
    expect_series(cached.fast, per_bar.fast, true, "cached fast EMA against the per-bar series");
    expect_series(cached.slow, per_bar.slow, true, "cached slow EMA against the per-bar series");
    expect_same_trades(cached, per_bar, "cached against per-bar");
    finish_row(before);
}

// No EMA anywhere: the four runs (cached or per-bar, declared or not) agree.
void non_ema_row() {
    row = "RSI 14 / ATR 14 script is the same with and without a declared origin";
    const int before = failures;
    const std::vector<Bar> bars = swing_feed(96);
    const Metadata declared{{"series_origin_ms", static_cast<double>(bars.front().timestamp)}};
    const char* names[] = {"cached undeclared", "cached declared", "per-bar undeclared",
                           "per-bar declared"};
    std::vector<Observed> runs;
    for (int index = 0; index < 4; ++index) {
        NonEmaHost host(/*last_entry_bar=*/80);
        runs.push_back(run_host(host, index % 2 == 1 ? declared : Metadata{}, bars,
                                index < 2 ? nullptr : "15"));
        print_observed(names[index], runs.back(), bars);
        check_run_path(names[index], runs.back(), bars, /*cached=*/index < 2);
    }
    for (int index = 1; index < 4; ++index) {
        const std::string what = std::string(names[index]) + " against " + names[0];
        expect_series(runs[static_cast<std::size_t>(index)].fast, runs[0].fast, true,
                      what + ": RSI series");
        expect_series(runs[static_cast<std::size_t>(index)].slow, runs[0].slow, true,
                      what + ": ATR series");
        expect_same_trades(runs[static_cast<std::size_t>(index)], runs[0], what);
    }
    // The control is not vacuous: both indicators produce values and trades.
    int finite_rsi = 0;
    double low_rsi = na<double>();
    double high_rsi = na<double>();
    for (const double value : runs[0].fast) {
        if (std::isnan(value)) continue;
        ++finite_rsi;
        if (std::isnan(low_rsi) || value < low_rsi) low_rsi = value;
        if (std::isnan(high_rsi) || value > high_rsi) high_rsi = value;
    }
    std::printf("OBS [%s] finite RSI values=%d lowest=%.17g highest=%.17g\n", row.c_str(),
                finite_rsi, low_rsi, high_rsi);
    expect(finite_rsi > 0 && high_rsi > low_rsi, "the RSI series is finite and not constant");
    expect(first_finite(runs[0].slow) >= 0, "the ATR series becomes finite");
    expect(!runs[0].trades.empty(), "the script closed at least one trade");
    finish_row(before);
}

class ThrowingPreparation final : public source::PineStrategyHost {
public:
    int bars_seen = 0;

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        if (!allow_precalculation || bars == nullptr || n <= 0) return;
        source::PineEma ema(3);
        (void)ema.compute(bars[0].close);
        throw std::runtime_error("preparation failure probe");
    }

    void on_source_bar(const Bar&) override { ++bars_seen; }
};

// A preparation that throws under a declared origin leaves the ambient default off.
void throwing_preparation_row() {
    row = "a throwing preparation leaves the ambient seeding default off";
    const int before = failures;
    const std::vector<Bar> bars = minute_bars({10.0, 20.0, 30.0});
    ta::ema_na_warmup_flag() = false;
    ThrowingPreparation host;
    host.set_syminfo_metadata("series_origin_ms", static_cast<double>(bars.front().timestamp));
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("OBS [%s] error='%s' bars_seen=%d ambient_after=%d\n", row.c_str(),
                host.last_error().c_str(), host.bars_seen, ta::ema_na_warmup_flag() ? 1 : 0);
    expect(!host.last_error().empty(), "the run records the preparation failure");
    expect_int(host.bars_seen, 0, "bars dispatched after the failed preparation");
    expect(!ta::ema_na_warmup_flag(), "the ambient seeding default is off after the failure");
    ta::ema_na_warmup_flag() = false;
    finish_row(before);
}

}  // namespace

int main() {
    undeclared_series_row();
    first_close_trading_row("feed-first-bar EMA 30/50 script, nothing declared", {});
    first_close_trading_row("declared origin, then chart_ema_na_warmup=0 on purpose",
                            {{"series_origin_ms", 0.0}, {"chart_ema_na_warmup", 0.0}});
    non_ema_row();
    throwing_preparation_row();

    std::printf("chart EMA batch seed invariants: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
