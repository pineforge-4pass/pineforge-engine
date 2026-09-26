#pragma once
// TradingView's ta.* calls whose length (or factor) is not a constant.
//
// A Pine length argument is qualified const, input, simple or series.
// TradingView compiles a series length only for the functions that declare
// it (ta.highest, ta.lowest, ta.highestbars, ta.lowestbars, ta.sma, ...):
// ta.rma, ta.ema, ta.rsi, ta.atr, ta.dmi, ta.macd, ta.kc, ta.hma, ta.tsi,
// ta.rci and ta.supertrend's atrPeriod refuse it at compile time (CE10123
// "An argument of series int type was used but a simple int is expected").
// Pinned with `lab tv` compile probes on 52 calls (K-TA-DYNLEN scratch
// tv/acc). The generated code reaches this header only for a length the
// constructor cannot take: a const or input length keeps the fixed-length
// classes of <pineforge/ta.hpp> and their constructor.
//
// * A simple length is fixed for the whole run: TradingView answers exactly
//   the const-length call (rec-simple: ta.rsi/ema/rma/atr, chart and a
//   request.security("5") payload, 150/150 bars on BINANCE:BTCUSDT 15 and
//   NYSE:F 15; win-qual: a sparse ta.lowest / ta.highestbars / ta.sma with a
//   simple length equals the const one on 46/46 calls, never-written ring
//   slots reading 0 included). FirstCallBound builds the const-length class
//   from the arguments of the call site's first execution.
// * A series length re-windows every call: SeriesHighest, SeriesLowest,
//   SeriesHighestBars and SeriesLowestBars implement the rule documented at
//   SeriesWindowExtreme.
// * ta.supertrend accepts a series factor but uses the factor of its first
//   execution for the whole run (PineSupertrend).
//
// A length of 0, a negative length or an na length stops the run, as it
// stops TradingView's (the texts below are TradingView's own).
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pineforge {
namespace source {

// The integer a simple-qualified length argument carries, checked the way
// TradingView checks it on the call's first execution: na stops the run with
// "Invalid value of the '<argument>' argument in the '<function>' function.
// It must not be na" (RE10003), zero or a negative value with "Invalid value
// of the '<argument>' argument (<value>) in the '<function>' function. It
// must be > 0." (RE10001) -- pinned on ta.rsi / ta.ema with a simple length
// of 0, -3 and na (tv/edge). A float value truncates toward zero, as Pine's
// int() does.
int simple_ta_length(double value, const char* function, const char* argument);
int simple_ta_length(int value, const char* function, const char* argument);
int simple_ta_length(std::int64_t value, const char* function, const char* argument);

// A number argument of the classes below as the double they take: an int or
// int64 na (the integer sentinel, not a NaN) stays na.
inline double ta_number(double value) { return value; }
inline double ta_number(int value) {
    return is_na(value) ? na<double>() : static_cast<double>(value);
}
inline double ta_number(std::int64_t value) {
    return is_na(value) ? na<double>() : static_cast<double>(value);
}

// A fixed-length TA object built on the first execution of its call site
// from that execution's arguments (a simple length is the same on every
// bar, so the first execution's value is the run's value). `make` returns
// the object; later calls reuse it and never evaluate `make` again.
// compute(make, args...) / recompute(make, args...) bind, then forward.
template <class T>
class FirstCallBound {
    std::optional<T> object_;

public:
    template <class Make>
    T& bind(Make&& make) {
        if (!object_) object_.emplace(std::forward<Make>(make)());
        return *object_;
    }
    bool bound() const { return object_.has_value(); }

    template <class Make, class... Args>
    decltype(auto) compute(Make&& make, Args&&... args) {
        return bind(std::forward<Make>(make)).compute(std::forward<Args>(args)...);
    }
    template <class Make, class... Args>
    decltype(auto) recompute(Make&& make, Args&&... args) {
        return bind(std::forward<Make>(make)).recompute(std::forward<Args>(args)...);
    }
};

// The history a series-length window reads: one value per bar of the
// evaluation context, from the call site's first execution on. A bar the site
// executes holds its source; a bar it skips holds the value of the last
// execution before it. The last kMaxBarsBack + 1 bars are kept. Full chunks
// are immutable and shared between copies, so copying the object (the
// script-state checkpoint does it every bar) costs one chunk at most.
class HeldHistory {
public:
    static constexpr std::size_t kChunk = 1024;

    bool empty() const { return count_ == 0; }
    long long first_bar() const { return first_bar_; }
    long long last_bar() const { return first_bar_ + count_ - 1; }
    // Appends bars up to and including `bar`, each skipped bar holding the
    // last value, then writes `value` for `bar`.
    void write(long long bar, double value, std::size_t keep);
    // The value held for `bar`; the caller keeps `bar` in
    // [first_bar(), last_bar()].
    double at(long long bar) const;

private:
    void push(double value);
    void drop_front_chunks(std::size_t keep);

    std::vector<std::shared_ptr<const std::vector<double>>> frozen_;
    std::vector<double> tail_;
    long long first_bar_ = 0;   // the bar of frozen_[0][0] (or tail_[0])
    long long count_ = 0;       // bars held
};

// ta.highest / ta.lowest / ta.highestbars / ta.lowestbars with a series
// length. Fitted with no mismatch on 5,647 recorded calls of 35 call sites
// (lab tv, K-TA-DYNLEN scratch tv/val: win-every, win-cond, win-lazy,
// win-first, win-late on BINANCE:BTCUSDT 15 and NYSE:F 15, win-synth on a
// synthetic source; the reference model is tv/series_extreme_model.py):
//
// * The window is the last `length` BARS of the site's own source history:
//   a bar the site did not execute holds its last executed source (a call
//   inside an `if` or on the right of a lazy `and`), a bar before the site's
//   first execution is na, a bar before the context's first bar does not
//   exist. The ring of the const-length classes (never-written slots read 0,
//   stale slots alias) does not apply.
// * The answer is na (and so is the *Bars offset) while bar_index <
//   length - 1. After that an na answer carries the offset 0.
// * The extremum is read through a cached {value, bar}. A call whose length
//   exceeds the previous call's first walks the slots the longer window adds,
//   oldest first, into the cache; when the site first executed after the
//   context's first bar and the length exceeds every earlier length, the
//   cache is dropped before that walk. Then the source: a strictly better
//   source takes the cache (ties keep the older member); an invalid cache
//   answers the source for this call without storing it; an aged cache
//   (bar - cached bar >= length) rescans the window oldest first. An na
//   member met by a walk or a rescan resets the running extremum (the answer
//   is the extremum over the members newer than it); an na source answers na
//   and invalidates the cache.
// * A length of 0, a negative length or an na length stops the run
//   (RE10001, na reported as 0); a window reaching more than 15000 bars back
//   stops it too (RE10008) -- pinned on ta.lowest / ta.highestbars / ta.sma
//   (tv/edge), and 4990, 5001 and 9000-bar windows answered in full.
class SeriesWindowExtreme {
public:
    static constexpr long long kMaxBarsBack = 15000;

    struct Result {
        double value;     // na: warm-up, or no member
        double offset;    // -bars back of the extremum; na during warm-up
    };
    Result update(double src, long long length, bool advance, bool want_max,
                  const char* function);

private:
    struct Cache {
        bool valid = false;
        double value = 0.0;
        long long bar = 0;
    };
    struct BarState {
        Cache cache;
        long long prev_length = 0;
        long long max_length = 0;
        bool started = false;
    };

    HeldHistory history_;
    BarState state_;
    BarState saved_state_;           // the state before the current bar
    long long current_bar_ = 0;
    bool has_bar_ = false;
    long long first_exec_bar_ = 0;
    long long own_bar_ = -1;         // cadence when no context is installed
};

class SeriesHighest {
    SeriesWindowExtreme core_;

public:
    double compute(double src, double length);
    double recompute(double src, double length);
};

class SeriesLowest {
    SeriesWindowExtreme core_;

public:
    double compute(double src, double length);
    double recompute(double src, double length);
};

class SeriesHighestBars {
    SeriesWindowExtreme core_;

public:
    double compute(double src, double length);
    double recompute(double src, double length);
};

class SeriesLowestBars {
    SeriesWindowExtreme core_;

public:
    double compute(double src, double length);
    double recompute(double src, double length);
};

// ta.supertrend(factor, atrPeriod) as TradingView computes it: the Pine
// reference implementation (hl2 +/- factor * ta.atr(atrPeriod), the bands
// ratcheted against nz(band[1]) and close[1], direction 1 while atr[1] is na)
// with the factor and atrPeriod of the call site's FIRST execution for the
// whole run. TradingView accepts a series factor but never reads it again:
// with factors 1, 1.75, 2, 4, 4.5 and 5.5 after a first bar at 3 the line and
// direction equal ta.supertrend(3, 10) on 330/330 bars of BINANCE:BTCUSDT 15
// and NYSE:F 15 (tv/val st-factor); a factor of 2 on bar 0 only reads as 2 on
// 112/112 bars; an na first factor reads as na: the lower band sticks at 0 and
// the answer alternates 0 / na with direction -1 / 1 once atr[1] is valid,
// 120/120 (st-latch A and B). The warm-up is the reference's too: bar 0
// answers the line 0, bars 1 .. atrPeriod - 1 na, direction 1 throughout.
// close[1] and the true range read the site's own previous execution (the
// chart's for a site executed every bar).
class PineSupertrend {
public:
    ta::SupertrendResult compute(double factor, double atr_period, double high, double low,
                                 double close);
    ta::SupertrendResult recompute(double factor, double atr_period, double high, double low,
                                   double close);

private:
    struct State {
        double prev_upper = na<double>();
        double prev_lower = na<double>();
        double prev_st = na<double>();
        double prev_atr = na<double>();
        double prev_close = na<double>();
    };
    ta::SupertrendResult step(double high, double low, double close, bool again);

    bool latched_ = false;
    double factor_ = na<double>();
    std::optional<ta::ATR> atr_;
    State state_;
    State saved_state_;
};

}  // namespace source
}  // namespace pineforge

// Feature macro: the classes above exist.
#define PINEFORGE_HAS_TA_SERIES_LENGTH 1
