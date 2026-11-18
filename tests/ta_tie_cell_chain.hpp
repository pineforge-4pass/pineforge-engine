// ta_tie_cell_chain.hpp -- shared body of the two TradingView tie-cell tests
// (tests/test_ta_tie_cell_eurusd.cpp, tests/test_ta_tie_cell_xauusd.cpp).
//
// The script is the frozen "MACD & Stoch RSI Confluence S/R" strategy.
// Its calculation block is lowered here by hand, statement for statement as
// codegen lowers it (not generated code), onto the kernel's own ta:: classes:
//
//     macd_line   = ta.ema(close, 12) - ta.ema(close, 26)
//     signal_line = ta.ema(macd_line, 9)
//     rsi_val     = ta.rsi(close, 14)
//     rsi_low     = ta.lowest(rsi_val, 14)
//     rsi_high    = ta.highest(rsi_val, 14)
//     stoch       = (rsi_val - rsi_low) / (rsi_high - rsi_low) * 100
//     k_line      = ta.sma(stoch, 3)
//     d_line      = ta.sma(k_line, 3)
//     crosses of (macd_line, signal_line) and of (k_line, d_line)
//
// Each test feeds the chart feed's closes from the first bar of the backtest
// range to the last logged bar, and compares with what TradingView logged on
// that chart. The fixture include holds both, generated from the sealed
// capture by a named extraction step; no expected number is typed in a test
// and none comes from the engine.
//
// What TradingView logged, per logged bar: macd, signal, rsi, K, D and K - D
// as exact binary64 values, each with its previous-bar value, and the cross
// and entry booleans. A zero is logged without its sign, so a zero matches
// either zero; every other value is compared bit for bit.

#pragma once

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace tie_cell {

struct FeedBar {
    long long time_ms;   // bar open, UTC
    double close;        // the feed's close, copied as its decimal text
};

struct Logged {
    long long time_ms;   // bar open, UTC
    // binary64 bit patterns of TradingView's values; 0 is the unsigned zero
    std::uint64_t macd, macd_prev;
    std::uint64_t signal, signal_prev;
    std::uint64_t rsi, rsi_prev;
    std::uint64_t k, k_prev;
    std::uint64_t d, d_prev;
    std::uint64_t k_minus_d, k_minus_d_prev;
    int macd_bull_cross, macd_bear_cross;
    int srsi_bull_cross, srsi_bear_cross;
    int long_condition, short_condition;
};

// A trade of TradingView's own tape that the capture targeted. Carried for the
// record of which trades the logged bars decide; a kernel test has no orders
// and asserts nothing about them.
struct TargetTrade {
    long long entry_ms;
    int trade_number;
    const char* type;
};

struct Values {
    double macd, signal, rsi, rsi_low, rsi_high, stoch, k, d;
    bool macd_bull_cross, macd_bear_cross;
    bool srsi_bull_cross, srsi_bear_cross;
    bool long_condition, short_condition;
};

class Chain {
public:
    explicit Chain(pineforge::ta::EmaSeeding seeding)
        : ema_fast_(12, seeding), ema_slow_(26, seeding), ema_signal_(9, seeding),
          rsi_(14), lowest_(14), highest_(14), k_sma_(3), d_sma_(3) {}

    Values step(double close) {
        Values v{};
        const double fast = ema_fast_.compute(close);
        const double slow = ema_slow_.compute(close);
        v.macd = fast - slow;
        v.signal = ema_signal_.compute(v.macd);
        v.macd_bull_cross = macd_over_.compute(v.macd, v.signal);
        v.macd_bear_cross = macd_under_.compute(v.macd, v.signal);
        v.rsi = rsi_.compute(close);
        v.rsi_low = lowest_.compute(v.rsi);
        v.rsi_high = highest_.compute(v.rsi);
        // codegen: stoch = (((double)(rsi_val - rsi_low)) / ((double)(rsi_high - rsi_low))) * 100
        v.stoch = ((v.rsi - v.rsi_low) / (v.rsi_high - v.rsi_low)) * 100;
        v.k = k_sma_.compute(v.stoch);
        v.d = d_sma_.compute(v.k);
        v.srsi_bull_cross = k_over_.compute(v.k, v.d);
        v.srsi_bear_cross = k_under_.compute(v.k, v.d);
        // use_macd and use_srsi are true and both filters are off (the inputs' defaults,
        // which the capture logged), so a valid cross is a cross.
        v.long_condition = v.macd_bull_cross || v.srsi_bull_cross;
        v.short_condition = v.macd_bear_cross || v.srsi_bear_cross;
        return v;
    }

private:
    pineforge::ta::EMA ema_fast_, ema_slow_, ema_signal_;
    pineforge::ta::Crossover macd_over_;
    pineforge::ta::Crossunder macd_under_;
    pineforge::ta::RSI rsi_;
    pineforge::ta::Lowest lowest_;
    pineforge::ta::Highest highest_;
    pineforge::ta::SMA k_sma_, d_sma_;
    pineforge::ta::Crossover k_over_;
    pineforge::ta::Crossunder k_under_;
};

inline std::uint64_t bits_of(double value) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof raw);
    return raw;
}

inline double from_bits(std::uint64_t raw) {
    double value = 0.0;
    std::memcpy(&value, &raw, sizeof value);
    return value;
}

// Equal as TradingView logged it.
inline bool same(double got, std::uint64_t want) {
    if (pineforge::is_na(got)) return false;
    if (want == 0) return got == 0.0;
    return bits_of(got) == want;
}

inline std::string hex16(std::uint64_t raw) {
    char text[32];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(raw));
    return text;
}

inline std::string value_text(double value) {
    if (pineforge::is_na(value)) return "na";
    char text[64];
    std::snprintf(text, sizeof text, " %.17g", value);
    return hex16(bits_of(value)) + text;
}

inline std::string utc(long long time_ms) {
    const std::time_t seconds = static_cast<std::time_t>(time_ms / 1000);
    const std::tm* parts = std::gmtime(&seconds);
    char text[32] = "unknown time";
    if (parts != nullptr) std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", parts);
    return text;
}

struct Counts {
    int passed = 0;
    int failed = 0;
};

inline void check(Counts& counts, bool ok, const char* group, const std::string& what) {
    if (ok) {
        ++counts.passed;
        return;
    }
    ++counts.failed;
    std::printf("FAIL %s: %s\n", group, what.c_str());
}

inline void check_value(Counts& counts, const char* group, long long time_ms, const char* series,
                        double got, std::uint64_t want) {
    check(counts, same(got, want), group,
          utc(time_ms) + " UTC " + series + ": got " + value_text(got) + ", TradingView "
              + value_text(from_bits(want)));
}

inline void check_flag(Counts& counts, const char* group, long long time_ms, const char* series,
                       bool got, int want) {
    check(counts, got == (want != 0), group,
          utc(time_ms) + " UTC " + series + ": got " + (got ? "1" : "0") + ", TradingView "
              + (want != 0 ? "1" : "0"));
}

// Runs one cell. Returns the process exit code.
inline int run_cell(const char* name, pineforge::ta::EmaSeeding seeding, long long range_start_ms,
                    const FeedBar* feed, std::size_t feed_count,
                    const Logged* logged, std::size_t logged_count,
                    const TargetTrade* targets, std::size_t target_count) {
    Counts fixture, consistency, ema_step, values, crosses, ties, fresh;

    // --- the fixture binds bars by their UTC time, never by an assumed offset
    check(fixture, feed_count > 0 && logged_count > 0, "fixture", "the fixture holds feed bars and logged bars");
    if (feed_count == 0 || logged_count == 0) {
        std::printf("%s: fixture empty; nothing was measured\n", name);
        return 1;
    }
    check(fixture, feed[0].time_ms >= range_start_ms, "fixture",
          "the first feed bar " + utc(feed[0].time_ms) + " UTC is not before the range start");
    bool increasing = true;
    for (std::size_t i = 1; i < feed_count; ++i) {
        if (feed[i].time_ms <= feed[i - 1].time_ms) increasing = false;
    }
    check(fixture, increasing, "fixture", "feed bar times strictly increase");
    std::vector<std::size_t> at(logged_count, feed_count);
    bool located = true;
    for (std::size_t j = 0; j < logged_count; ++j) {
        for (std::size_t i = 0; i < feed_count; ++i) {
            if (feed[i].time_ms == logged[j].time_ms) {
                at[j] = i;
                break;
            }
        }
        if (at[j] == feed_count || at[j] == 0) located = false;
        if (j > 0 && logged[j].time_ms <= logged[j - 1].time_ms) located = false;
    }
    check(fixture, located, "fixture", "every logged bar is a feed bar with a bar before it, in time order");
    check(fixture, feed[feed_count - 1].time_ms == logged[logged_count - 1].time_ms, "fixture",
          "the feed prefix ends on the last logged bar");
    if (!located) {
        std::printf("%s: logged bars not located in the feed; nothing was measured\n", name);
        return 1;
    }

    // --- the script over the whole prefix
    std::vector<Values> series;
    series.reserve(feed_count);
    Chain chain(seeding);
    for (std::size_t i = 0; i < feed_count; ++i) series.push_back(chain.step(feed[i].close));

    // --- TradingView against itself: a previous-bar field is the previous bar's value
    for (std::size_t j = 1; j < logged_count; ++j) {
        if (at[j] != at[j - 1] + 1) continue;
        const Logged& now = logged[j];
        const Logged& before = logged[j - 1];
        const bool agree = now.macd_prev == before.macd && now.signal_prev == before.signal
            && now.rsi_prev == before.rsi && now.k_prev == before.k && now.d_prev == before.d
            && now.k_minus_d_prev == before.k_minus_d;
        check(consistency, agree, "capture",
              utc(now.time_ms) + " UTC previous-bar fields equal the values logged on the bar before");
    }

    // --- one EMA step on TradingView's own numbers, no history: the signal
    //     of a bar from the signal of the bar before and this bar's macd
    for (std::size_t j = 0; j < logged_count; ++j) {
        pineforge::ta::EMA one_step(9, pineforge::ta::EmaSeeding::FirstValue);
        one_step.compute(from_bits(logged[j].signal_prev));
        const double got = one_step.compute(from_bits(logged[j].macd));
        check_value(ema_step, "ema-step", logged[j].time_ms, "signal from (previous signal, macd)", got,
                    logged[j].signal);
    }

    // --- the chain against TradingView on every logged bar and the bar before it
    for (std::size_t j = 0; j < logged_count; ++j) {
        const Logged& tv = logged[j];
        const Values& now = series[at[j]];
        const Values& before = series[at[j] - 1];
        const long long before_ms = feed[at[j] - 1].time_ms;
        check_value(values, "macd", tv.time_ms, "macd", now.macd, tv.macd);
        check_value(values, "macd", tv.time_ms, "signal", now.signal, tv.signal);
        check_value(values, "macd", before_ms, "macd", before.macd, tv.macd_prev);
        check_value(values, "macd", before_ms, "signal", before.signal, tv.signal_prev);
        check_value(values, "rsi", tv.time_ms, "rsi", now.rsi, tv.rsi);
        check_value(values, "rsi", before_ms, "rsi", before.rsi, tv.rsi_prev);
        check_value(values, "kd", tv.time_ms, "K", now.k, tv.k);
        check_value(values, "kd", tv.time_ms, "D", now.d, tv.d);
        check_value(values, "kd", tv.time_ms, "K - D", now.k - now.d, tv.k_minus_d);
        check_value(values, "kd", before_ms, "K", before.k, tv.k_prev);
        check_value(values, "kd", before_ms, "D", before.d, tv.d_prev);
        check_value(values, "kd", before_ms, "K - D", before.k - before.d, tv.k_minus_d_prev);
        check_flag(crosses, "cross", tv.time_ms, "macd crossover", now.macd_bull_cross, tv.macd_bull_cross);
        check_flag(crosses, "cross", tv.time_ms, "macd crossunder", now.macd_bear_cross, tv.macd_bear_cross);
        check_flag(crosses, "cross", tv.time_ms, "K/D crossover", now.srsi_bull_cross, tv.srsi_bull_cross);
        check_flag(crosses, "cross", tv.time_ms, "K/D crossunder", now.srsi_bear_cross, tv.srsi_bear_cross);
        check_flag(crosses, "cross", tv.time_ms, "longCondition", now.long_condition, tv.long_condition);
        check_flag(crosses, "cross", tv.time_ms, "shortCondition", now.short_condition, tv.short_condition);
    }

    // --- the tie: where TradingView's K - D is zero, K equals D here too
    int tie_bars = 0;
    for (std::size_t j = 0; j < logged_count; ++j) {
        if (logged[j].k_minus_d != 0) continue;
        ++tie_bars;
        const Values& now = series[at[j]];
        check(ties, now.k == now.d, "tie",
              utc(logged[j].time_ms) + " UTC TradingView's K equals D; got K " + value_text(now.k) + ", D "
                  + value_text(now.d));
    }
    check(ties, tie_bars > 0, "tie", "the capture holds a bar where TradingView's K equals D");

    // --- control: a window of TradingView's own three equal K, summed from a
    //     clean state, is that K. It separates a wrong sum of the window from a
    //     state carried into it.
    int tie_windows = 0;
    for (std::size_t j = 1; j < logged_count; ++j) {
        if (at[j] != at[j - 1] + 1) continue;
        const std::uint64_t oldest = logged[j - 1].k_prev;
        const std::uint64_t middle = logged[j - 1].k;
        const std::uint64_t newest = logged[j].k;
        if (oldest != middle || middle != newest) continue;
        ++tie_windows;
        pineforge::ta::SMA clean(3);
        clean.compute(from_bits(oldest));
        clean.compute(from_bits(middle));
        const double got = clean.compute(from_bits(newest));
        check_value(fresh, "clean-window", logged[j].time_ms, "mean of three equal K from a clean state", got,
                    newest);
    }

    for (std::size_t t = 0; t < target_count; ++t) {
        std::printf("  capture target: trade %d, %s, entry %s UTC\n", targets[t].trade_number, targets[t].type,
                    utc(targets[t].entry_ms).c_str());
    }
    std::printf("  %s: %zu feed bars from %s UTC, %zu logged bars, %d tie bars, %d tie windows\n", name,
                feed_count, utc(feed[0].time_ms).c_str(), logged_count, tie_bars, tie_windows);
    std::printf("  fixture passed=%d failed=%d\n", fixture.passed, fixture.failed);
    std::printf("  capture passed=%d failed=%d\n", consistency.passed, consistency.failed);
    std::printf("  ema-step passed=%d failed=%d\n", ema_step.passed, ema_step.failed);
    std::printf("  values passed=%d failed=%d\n", values.passed, values.failed);
    std::printf("  cross passed=%d failed=%d\n", crosses.passed, crosses.failed);
    std::printf("  tie passed=%d failed=%d\n", ties.passed, ties.failed);
    std::printf("  clean-window passed=%d failed=%d\n", fresh.passed, fresh.failed);
    const int passed = fixture.passed + consistency.passed + ema_step.passed + values.passed + crosses.passed
        + ties.passed + fresh.passed;
    const int failed = fixture.failed + consistency.failed + ema_step.failed + values.failed + crosses.failed
        + ties.failed + fresh.failed;
    std::printf("%s: passed=%d failed=%d\n", name, passed, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace tie_cell
