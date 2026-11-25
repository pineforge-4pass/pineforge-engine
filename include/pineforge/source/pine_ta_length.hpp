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
// * ta.percentile_nearest_rank with a series length appends its source on
//   every call at the call site, including repeated calls in one bar, then
//   ranks the last `length` appended values. It answers na until that many
//   values have been appended. While the length stays constant, the position
//   of na values among the others follows the constant-length class; after a
//   length change, the new window is replayed in appended time order.
//   Re-execution of a bar containing a call site that runs more than once is
//   not modelled, as with the constant-length class. Pinned by one TradingView
//   number pair of a loop call; the shape of a call inside an `if` is not
//   pinned by a tape.
// * ta.supertrend accepts a series factor but uses the factor of its first
//   execution for the whole run (PineSupertrend).
//
// A length of 0, a negative length or an na length stops the run, as it
// stops TradingView's (the texts below are TradingView's own).
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>
#include <pineforge/detail/ta_lookback.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
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

// A length argument of the checked adapters below as a double: ta_number for
// int, int64 and double, the only types whose integer na the adapters know.
// A length of another integer or floating type, including long long where
// std::int64_t is long (and the reverse), fails to compile by design; the
// codegen's own tests show which types generated code passes.
template <class T>
double ta_length_number(T value) {
    static_assert(std::is_same<T, int>::value || std::is_same<T, std::int64_t>::value ||
                      std::is_same<T, double>::value,
                  "a TA length is an int, an int64_t or a double");
    return ta_number(value);
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
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }

    template <class Make, class... Args>
    decltype(auto) compute(Make&& make, Args&&... args) {
        return bind(std::forward<Make>(make)).compute(std::forward<Args>(args)...);
    }
    template <class Make, class... Args>
    decltype(auto) recompute(Make&& make, Args&&... args) {
        return bind(std::forward<Make>(make)).recompute(std::forward<Args>(args)...);
    }
};

// Checked EMA-family adapters: ta.ema, ta.tsi, ta.macd, ta.kc and ta.kcw with
// the length check of simple_ta_length. The constructor stores the length
// arguments and never throws, so an invalid length cannot stop a run before
// its first bar. The first compute() or recompute() checks every length
// argument with TradingView's function and argument names, in the
// signature's order (which of two invalid lengths TradingView reports first
// is not pinned: the signature's order is an assumption), and then builds the
// fixed-length class of <pineforge/ta.hpp> from the checked lengths. A failed
// check builds nothing, so the next call checks again. After a passed check
// every call delegates to that class, so the values for a valid length are
// the class's own. The ambient ta::ema_na_warmup_flag() is neither read nor
// changed here: the built class reads it as it always does. A length argument
// is taken as an int, int64 or double (ta_number keeps an integer na as na);
// the function names are written without "ta.", as simple_ta_length's other
// callers write them ("supertrend", "sma"). A length that simple_ta_length
// limits to INT_MAX (a larger double or int64 value) is passed on as it is,
// as for every other call of this header. The fixed-length class computes
// its own coefficients: ta::EMA converts the length to double before
// adding one in the denominator of its smoothing factor.
// Covered: the length constructor (explicit only for PineEma, as for the
// classes), compute, recompute and observe_owned_storage: the paired codegen
// maps ta.ema, ta.tsi, ta.macd, ta.kc and ta.kcw to these types and its shims
// call only those members. Not forwarded: the EmaSeeding constructor,
// input_ready overloads, ready(), save(), restore().
class PineEma {
    double length_;
    std::optional<ta::EMA> object_;

    ta::EMA& bound() {
        if (!object_) object_.emplace(simple_ta_length(length_, "ema", "length"));
        return *object_;
    }

public:
    // Arithmetic lengths only, so the copy constructor is never shadowed.
    template <class Length,
              class = std::enable_if_t<std::is_arithmetic<Length>::value>>
    explicit PineEma(Length length) : length_(ta_length_number(length)) {}
    double compute(double src) { return bound().compute(src); }
    double recompute(double src) { return bound().recompute(src); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }
};

class PineTsi {
    double short_length_;
    double long_length_;
    std::optional<ta::TSI> object_;

    ta::TSI& bound() {
        if (!object_) {
            const int short_length = simple_ta_length(short_length_, "tsi", "short_length");
            const int long_length = simple_ta_length(long_length_, "tsi", "long_length");
            object_.emplace(short_length, long_length);
        }
        return *object_;
    }

public:
    template <class Short, class Long>
    PineTsi(Short short_length, Long long_length)
        : short_length_(ta_length_number(short_length)),
          long_length_(ta_length_number(long_length)) {}
    double compute(double src) { return bound().compute(src); }
    double recompute(double src) { return bound().recompute(src); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }
};

class PineMacd {
    double fastlen_;
    double slowlen_;
    double siglen_;
    std::optional<ta::MACD> object_;

    ta::MACD& bound() {
        if (!object_) {
            const int fastlen = simple_ta_length(fastlen_, "macd", "fastlen");
            const int slowlen = simple_ta_length(slowlen_, "macd", "slowlen");
            const int siglen = simple_ta_length(siglen_, "macd", "siglen");
            object_.emplace(fastlen, slowlen, siglen);
        }
        return *object_;
    }

public:
    template <class Fast, class Slow, class Signal>
    PineMacd(Fast fastlen, Slow slowlen, Signal siglen)
        : fastlen_(ta_length_number(fastlen)), slowlen_(ta_length_number(slowlen)),
          siglen_(ta_length_number(siglen)) {}
    ta::MACDResult compute(double src) { return bound().compute(src); }
    ta::MACDResult recompute(double src) { return bound().recompute(src); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }
};

class PineKc {
    double length_;
    double mult_;
    bool use_true_range_;
    std::optional<ta::KC> object_;

    ta::KC& bound() {
        if (!object_)
            object_.emplace(simple_ta_length(length_, "kc", "length"), mult_, use_true_range_);
        return *object_;
    }

public:
    template <class Length>
    PineKc(Length length, double mult, bool use_true_range = true)
        : length_(ta_length_number(length)), mult_(mult), use_true_range_(use_true_range) {}
    ta::KCResult compute(double src, double high, double low, double close) {
        return bound().compute(src, high, low, close);
    }
    ta::KCResult recompute(double src, double high, double low, double close) {
        return bound().recompute(src, high, low, close);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }
};

class PineKcw {
    double length_;
    double mult_;
    bool use_true_range_;
    std::optional<ta::KCW> object_;

    ta::KCW& bound() {
        if (!object_)
            object_.emplace(simple_ta_length(length_, "kcw", "length"), mult_, use_true_range_);
        return *object_;
    }

public:
    template <class Length>
    PineKcw(Length length, double mult, bool use_true_range = true)
        : length_(ta_length_number(length)), mult_(mult), use_true_range_(use_true_range) {}
    double compute(double src, double high, double low, double close) {
        return bound().compute(src, high, low, close);
    }
    double recompute(double src, double high, double low, double close) {
        return bound().recompute(src, high, low, close);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }
};

// The history a series-length window reads: one value per bar of the
// evaluation context, from the call site's first execution on. A bar the site
// executes holds its source; a bar it skips holds the value of the last
// execution before it. Keep 0 retains the whole history; a positive keep
// allows retirement in whole chunks. Numeric storage is copy-on-write; its
// resident cache size is independent of the retained lookback.
class HeldHistory {
public:
    static constexpr std::size_t kChunk = 1024;

    HeldHistory() : HeldHistory(window_cache_capacity()) {}
    explicit HeldHistory(WindowCacheCapacity cache)
        : values_(cache) {}
    bool empty() const { return count_ == 0; }
    long long first_bar() const { return first_bar_; }
    long long last_bar() const { return first_bar_ + count_ - 1; }
    // Appends bars up to and including `bar`, each skipped bar holding the
    // last value, then writes `value` for `bar`.
    void write(long long bar, double value, std::size_t keep);
    // The value held for `bar`; the caller keeps `bar` in
    // [first_bar(), last_bar()].
    double at(long long bar) const;
    WindowCacheCapacity cache_capacity() const { return values_.cache_capacity(); }
    WindowCacheStats cache_stats() const { return values_.cache_stats(); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        values_.observe_owned_storage(out);
    }

private:
    void ensure_slots(std::size_t minimum, std::size_t keep);
    std::size_t slot(std::size_t offset) const;
    void push(double value, std::size_t keep);
    void drop_front_chunks(std::size_t keep);

    ::pineforge::detail::WindowValues<double> values_;
    std::size_t head_ = 0;
    long long first_bar_ = 0;   // the bar at head_
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
//   (RE10001, na reported as 0). Lookback is unlimited unless the host sets a
//   cap. TradingView's 15,000-bar cap raises RE10008 (tv/edge on ta.lowest,
//   ta.highestbars and ta.sma).
class SeriesWindowExtreme {
public:
    // TradingView compatibility value, not the default run policy.
    static constexpr long long kMaxBarsBack = 15000;

    SeriesWindowExtreme() : SeriesWindowExtreme(window_cache_capacity()) {}
    explicit SeriesWindowExtreme(WindowCacheCapacity cache)
        : history_(cache) {}
    struct Result {
        double value;     // na: warm-up, or no member
        double offset;    // -bars back of the extremum; na during warm-up
    };
    Result update(double src, long long length, bool advance, bool want_max,
                  const char* function);
    Result update_series(double src, double length, bool advance, bool want_max,
                         const char* function);
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

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
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
};

class SeriesHighest {
    SeriesWindowExtreme core_;

public:
    SeriesHighest() = default;
    explicit SeriesHighest(WindowCacheCapacity cache) : core_(cache) {}
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }
    double compute(double src, double length);
    double recompute(double src, double length);
};

class SeriesLowest {
    SeriesWindowExtreme core_;

public:
    SeriesLowest() = default;
    explicit SeriesLowest(WindowCacheCapacity cache) : core_(cache) {}
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }
    double compute(double src, double length);
    double recompute(double src, double length);
};

class SeriesHighestBars {
    SeriesWindowExtreme core_;

public:
    SeriesHighestBars() = default;
    explicit SeriesHighestBars(WindowCacheCapacity cache) : core_(cache) {}
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }
    double compute(double src, double length);
    double recompute(double src, double length);
};

class SeriesLowestBars {
    SeriesWindowExtreme core_;

public:
    SeriesLowestBars() = default;
    explicit SeriesLowestBars(WindowCacheCapacity cache) : core_(cache) {}
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }
    double compute(double src, double length);
    double recompute(double src, double length);
};

// ta.percentile_nearest_rank(source, length, percentage) with a series
// length. Every compute() appends one source value to this call site's own
// ring, even when the site executes more than once in a bar. The answer is
// the nearest-rank percentile over the last `length` appended values and is
// na until that many values exist. While the length stays constant, the
// position of na values follows ta::PercentileNearestRank; after a length
// change, the requested window is rebuilt by replaying its retained values in
// time order. recompute() replaces the value appended by the previous call of
// this site. Re-execution of a bar containing a call site that runs more than
// once in a bar is not modelled, as with ta::PercentileNearestRank. The ring
// retains the full history when unlimited and at most cap + 1 values otherwise.
class SeriesPercentileNearestRank {
public:
    double compute(double src, double length, double percentage);
    double recompute(double src, double length, double percentage);
    void observe_owned_storage(OwnedStorage& out) const noexcept;

private:
    void append(double src);
    void replace_last(double src);
    double history_at(std::size_t offset) const;
    double evaluate(double src, std::uint64_t length, double percentage, bool replace);

    std::vector<double> history_;
    std::optional<ta::PercentileWindow> window_;
    std::size_t window_length_ = 0;
    std::size_t next_ = 0;
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
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
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, atr_);
    }

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

// Series-length SMA for one written site with one ordinary update per call.
// Validate current_length before updating: na/nonfinite, a nonpositive
// truncated length or a configured lookback cap exceeded stops the run.
// A valid call records only non-na sources, retaining them all by default
// or the last cap + 1 samples when capped. Warm-up returns na.
// Finite fractions truncate toward zero; changing length preserves the ring.
// The running order was recorded on TradingView in November 2026. Before
// inserting a source, remove or bring back older observations, oldest first,
// to keep length - 1 previous sources (or all that exist). With na, adjust
// to length instead and insert nothing. Each step adds its two-sum error to
// a separate double C. Emit (S + C) / length without folding C back into S.
// Overflow and retained infinities use the fixed window's recovery rule.
// Recompute starts from the same pre-bar sum, correction, active count and
// history even when the length changes. Copies retain that checkpoint;
// storage detaches on mutation. Cache capacity controls residency only.
class SeriesSma {
public:
    SeriesSma() : SeriesSma(window_cache_capacity()) {}
    explicit SeriesSma(WindowCacheCapacity cache) : history_(cache) {}
    // Copy even from an rvalue: the source's counts still name its history.
    SeriesSma(const SeriesSma&) = default;
    SeriesSma& operator=(const SeriesSma&) = default;
    double compute(double source, double current_length);
    double recompute(double source, double current_length);
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    std::uint64_t checked_length(double current_length) const;
    void ensure_capacity(std::size_t count);
    double update(double source, std::uint64_t length, bool replace);

    struct Window {
        double sum = 0.0;
        double correction = 0.0;
        std::size_t count = 0;
        bool exact_recovery = false;
    };

    ::pineforge::detail::WindowValues<double> history_;
    std::size_t head_ = 0; // next write; one past storage if it has to grow
    std::size_t count_ = 0;
    Window active_;
    Window pre_update_active_;
    std::size_t pre_update_head_ = 0;
    std::size_t pre_update_count_ = 0;
    double pre_update_evicted_ = na<double>();
    bool current_pushed_ = false;
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
};

}  // namespace source
}  // namespace pineforge

// Feature macro: the classes above exist.
#define PINEFORGE_HAS_TA_SERIES_LENGTH 1
