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

// ta.mom(source, length) and ta.roc(source, length) on a source that has
// missing (na) bars.
//
// TradingView's reference defines ta.mom as `source - source[length]` and
// ta.roc as `100 * ta.change(source, length) / source[length]`. A history
// subscript counts bars, so a bar whose source is na still holds a position:
// the call reads the bar `length` positions back whatever lies between, and
// answers na when that bar or the current one is na. This is the rule of the
// reference text; no TradingView tape pins it yet for a source with an na
// bar. ta.change already follows it: ta::Change keeps one slot per call, na
// included. ta::Mom and ta::ROC keep a window of non-missing samples
// instead, so after an na bar they read the length-th previous PRESENT
// sample, which is older than `source[length]`, until the na bar is more
// than `length` bars back. That window is those two classes' own contract
// and stays as it is; PineMom and PineRoc are the Pine call's objects.
//
// PositionalLag is the history the two share: one slot per compute(), na
// included, the last `length + 1` of them. recompute() rewrites the newest
// slot, so a recalculation of the same bar adds no position; a recompute()
// before the first compute() is that first compute().
//
// For a source with no na after its first value (leading na bars included)
// PineMom and PineRoc answer what ta::Mom and ta::ROC answer, bit for bit,
// for every length: na until `length` earlier values exist, then the same
// subtraction and the same `(source - previous) / previous * 100` with na
// for a previous value of zero. A length of 0 reads the current value
// (ta.mom 0, ta.roc 0 or na for a zero source) and a negative length answers
// na on every call, as the two classes do. The length is taken as the int
// the classes take; no length stops the run here.
class PositionalLag {
    // Newest first from head_, as ta::Change keeps its history. The ring
    // grows as calls arrive, so a large length costs nothing before that
    // many calls have been made.
    std::vector<double> lag_slots_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    int length_;

    std::size_t index(std::size_t offset) const noexcept {
        const std::size_t position = head_ + offset;
        return position < lag_slots_.size() ? position : position - lag_slots_.size();
    }
    // Room for one more slot while fewer than `keep` are held. Nothing
    // changes if the allocation fails.
    void ensure_room(std::size_t keep) {
        if (size_ < lag_slots_.size() || size_ >= keep) return;
        const std::size_t capacity = lag_slots_.size();
        const std::size_t initial = keep < 8 ? keep : std::size_t{8};
        const std::size_t missing = keep - capacity;
        const std::size_t next =
            capacity == 0 ? initial : capacity + (capacity < missing ? capacity : missing);
        std::vector<double> grown(next);
        for (std::size_t i = 0; i < size_; ++i) grown[i] = lag_slots_[index(i)];
        lag_slots_.swap(grown);
        head_ = 0;
    }

public:
    explicit PositionalLag(int length) : length_(length) {}

    // One call of the site. `advance` records `src` at a new position;
    // otherwise `src` replaces the newest position. Returns false while
    // fewer than `length` earlier positions exist, and for a negative
    // length; otherwise `previous` is the value `length` positions back,
    // na if that position holds na.
    bool step(double src, bool advance, double& previous) {
        if (length_ < 0) return false;
        const std::size_t keep = static_cast<std::size_t>(length_) + 1;
        if (advance || size_ == 0) {
            ensure_room(keep);
            head_ = head_ ? head_ - 1 : lag_slots_.size() - 1;
            lag_slots_[head_] = src;
            if (size_ < keep) ++size_;
        } else {
            lag_slots_[head_] = src;
        }
        if (size_ < keep) return false;
        previous = lag_slots_[index(keep - 1)];
        return true;
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, lag_slots_);
    }
};

// ta.mom(source, length): `source - source[length]`, positions counted as
// PositionalLag counts them. A drop-in for ta::Mom in generated code: the
// same constructor argument, compute, recompute and observe_owned_storage.
class PineMom {
    PositionalLag lag_;

    double answer(double src, bool advance) {
        double previous = na<double>();
        if (!lag_.step(src, advance, previous) || is_na(src) || is_na(previous)) {
            return na<double>();
        }
        return src - previous;
    }

public:
    explicit PineMom(int length) : lag_(length) {}
    double compute(double src) { return answer(src, true); }
    double recompute(double src) { return answer(src, false); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        lag_.observe_owned_storage(out);
    }
};

// ta.roc(source, length): `100 * (source - source[length]) / source[length]`
// in ta::ROC's operation order, na for a zero `source[length]`, positions
// counted as PositionalLag counts them. A drop-in for ta::ROC in generated
// code, as PineMom is for ta::Mom.
class PineRoc {
    PositionalLag lag_;

    double answer(double src, bool advance) {
        double previous = na<double>();
        if (!lag_.step(src, advance, previous) || is_na(src) || is_na(previous) ||
            previous == 0.0) {
            return na<double>();
        }
        return (src - previous) / previous * 100.0;
    }

public:
    explicit PineRoc(int length) : lag_(length) {}
    double compute(double src) { return answer(src, true); }
    double recompute(double src) { return answer(src, false); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        lag_.observe_owned_storage(out);
    }
};

// The check ta.supertrend's two arguments get on the first execution of a
// call site, whatever their qualifier: PineSupertrend::compute makes it for a
// factor that is not a constant, PineFixedSupertrend below for a constant or
// input one. atrPeriod first, through simple_ta_length ("supertrend",
// "atrPeriod": na, zero or a negative period stops the run), then the factor:
// zero or a negative factor stops the run with "Invalid value of the 'factor'
// argument (<value>) in the 'supertrend' function. It must be > 0."; an na
// factor is accepted. Returns the checked atrPeriod.
int supertrend_first_execution_period(double factor, double atr_period);

// ta.supertrend(factor, atrPeriod) with a constant or input factor and
// atrPeriod: the checked form of ta::Supertrend, as PineEma is of ta::EMA.
// The constructor stores the two arguments and never throws, so an invalid
// one cannot stop a run before its first bar. The first compute() or
// recompute() makes the check above, the one a factor that is not a constant
// gets, and then builds ta::Supertrend from the checked arguments. A failed
// check builds nothing, so the next call checks again. After a passed check
// every call delegates to that class, so the line and direction for a valid
// pair are the class's own. The factor is taken as an int, int64 or double
// (ta_number keeps an integer na as na) and atrPeriod as a length
// (ta_length_number). Covered: the two-argument constructor, compute,
// recompute and observe_owned_storage.
class PineFixedSupertrend {
    double factor_;
    double atr_period_;
    std::optional<ta::Supertrend> object_;

    ta::Supertrend& bound() {
        if (!object_)
            object_.emplace(factor_, supertrend_first_execution_period(factor_, atr_period_));
        return *object_;
    }

public:
    template <class Factor, class Period>
    PineFixedSupertrend(Factor factor, Period atr_period)
        : factor_(ta_number(factor)), atr_period_(ta_length_number(atr_period)) {}
    ta::SupertrendResult compute(double high, double low, double close) {
        return bound().compute(high, low, close);
    }
    ta::SupertrendResult recompute(double high, double low, double close) {
        return bound().recompute(high, low, close);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_storage(out, object_);
    }
};

// ta.vwma(source, length) as the Pine reference manual defines it:
//     ta.sma(source * volume, length) / ta.sma(volume, length)
// with two ta::SMA windows that take their inputs separately. ta::VWMA keeps
// complete pairs instead: a bar whose source or volume is na enters neither
// of its windows and answers na. Here a bar whose source is na and whose
// volume is not still enters the volume window, and the product window holds
// its last mean, as ta::SMA does for an na input. So the answer on such a bar
// is the held product mean over the new volume mean, and the two windows
// cover different bars until `length` further complete pairs have arrived.
// An na volume is missing in both windows. On bars without na the values are
// those of the two ta::SMA calls, not ta::VWMA's running sums, and can differ
// from ta::VWMA in the last place. A zero volume mean answers na, as a zero
// total weight does in ta::VWMA. The definition is the manual's; no
// TradingView recording of a bar with an na source backs it yet.
// Members: the length constructor, compute, recompute (each window replays
// its own bar) and observe_owned_storage, the ones ta::VWMA has.
class PineVwma {
    ta::SMA product_;
    ta::SMA volume_;

    static double quotient(double product_mean, double volume_mean) {
        if (volume_mean == 0.0) return na<double>();
        return product_mean / volume_mean;
    }

public:
    explicit PineVwma(int length) : product_(length), volume_(length) {}
    double compute(double source, double volume) {
        const double product_mean = product_.compute(source * volume);
        return quotient(product_mean, volume_.compute(volume));
    }
    double recompute(double source, double volume) {
        const double product_mean = product_.recompute(source * volume);
        return quotient(product_mean, volume_.recompute(volume));
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, product_, volume_);
    }
};

// ta.dmi(diLength, adxSmoothing) by the Pine reference definition:
//
//   plus  = fixnan(100 * ta.rma(plusDM, diLength) / ta.rma(ta.tr, diLength))
//   minus = fixnan(100 * ta.rma(minusDM, diLength) / ta.rma(ta.tr, diLength))
//   sum   = plus + minus
//   adx   = 100 * ta.rma(math.abs(plus - minus) / (sum == 0 ? 1 : sum), adxSmoothing)
//
// The directional movements, the true range and the four averages are
// ta::DMI's, and so is every value on a bar the two rules below do not
// decide, bit for bit:
//
// * fixnan: a quotient that is na keeps the last value it had. The quotient
//   is na while the averages are, and on a zero averaged true range (0 / 0:
//   the averaged range is zero only while every directional movement is
//   zero too). On such a bar both indices read their last values and ADX
//   takes one more step from them, where ta::DMI answers three na and leaves
//   ADX where it was. Nothing is held before the first valid quotient, so
//   flat bars at the start of the series stay na and give ADX no input. With
//   diLength 1 the averaged range is the bar's own, so every bar whose high
//   and low equal the previous close is such a bar; with a longer diLength
//   the averaged range stays positive, short of underflow, once one bar has
//   had a range.
// The directional movements compare with the tolerant relational of
// <pineforge/ta_compare_band.hpp>, exactly as ta::DMI does, and the index sum
// is tested against zero exactly, as ta::DMI tests it.
//
// A TradingView capture of a synthetic script found the built-in's indices
// finite on bars whose own true range is zero. The scaling keeps ta::DMI's order
// (100 * |plus - minus| / divisor, then the average), which is what keeps
// the other bars' bits. An na high, low or close answers three na and
// changes nothing, as in ta::DMI. The lengths are the two ints ta::DMI
// takes and are not checked here, as they are not checked there.
// recompute() replaces the bar computed last: the held indices, the
// previous bar and the four averages return to what that bar found.
class PineDmiReference {
public:
    PineDmiReference(int di_length, int adx_smoothing);
    ta::DMIResult compute(double high, double low, double close);
    ta::DMIResult recompute(double high, double low, double close);
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, rma_plus_, rma_minus_, rma_tr_, rma_adx_);
    }

private:
    struct State {
        double prev_high = na<double>();
        double prev_low = na<double>();
        double prev_close = na<double>();
        bool first_bar = true;
        double held_plus = na<double>();    // fixnan: the last non-na +DI
        double held_minus = na<double>();   // fixnan: the last non-na -DI
    };

    ta::RMA rma_plus_, rma_minus_, rma_tr_, rma_adx_;
    State state_;
    State saved_state_;
};

// The relational comparisons inside PineSar. TradingView's built-in ta.sar
// compares its internal values the way a Pine relational does: two finite
// values at most 1e-10 apart are equal, whatever their size (the absolute band
// of <pineforge/ta_compare_band.hpp>; an na operand makes every relational
// false). On a synthetic script its output followed a banded copy of the
// reference definition on every bar where a banded and a raw copy disagreed.
// PineSar compares with float_band_gt / float_band_lt, so a projected SAR
// 6e-11 beyond the bar's low is a touch and keeps the side it was on. ta::SAR
// keeps the exact C++ comparisons: nothing here changes the kernel, and a call
// site chooses this class to get TradingView's comparisons. (ta.supertrend's
// built-in compares raw, as ta::Supertrend does, so it has no banded twin.)
//
// ta.sar(start, inc, max) with those comparisons. The recipe and its
// arithmetic are ta::SAR's, statement for statement: na until the second bar
// with values; the first trend bar is long when close is greater than
// close[1], takes the bar's high as its extreme (the low when short) and
// starts the line at the previous low (high); every bar projects the line
// toward the extreme by the acceleration, reverses when the projection is
// greater than the bar's low (long) or less than its high (short), steps the
// acceleration when the bar sets a new extreme (never on a first trend bar),
// and holds the line behind the previous two bars' lows (highs). The five
// comparisons named in that sentence are the banded ones; min and max are
// exact. While no compared pair lies inside the band the answers are
// ta::SAR's, bit for bit. Same constructor and members as ta::SAR.
class PineSar {
public:
    PineSar(double start, double increment, double maximum)
        : start_(start), increment_(increment), maximum_(maximum) {}
    double compute(double high, double low, double close);
    double recompute(double high, double low, double close);
    // All retained fields are inline scalars.
    void observe_owned_storage(OwnedStorage&) const noexcept {}

private:
    struct State {
        double af = 0.0;
        double ep = 0.0;
        double sar = 0.0;
        bool is_long = true;
        bool initialized = false;
        double prev_high = na<double>();
        double prev_low = na<double>();
        double prev_close = na<double>();
        double prev_prev_high = na<double>();
        double prev_prev_low = na<double>();
    };
    double step(double high, double low, double close);

    double start_;
    double increment_;
    double maximum_;
    State state_;
    State saved_state_;  // the state before the current bar
};

}  // namespace source
}  // namespace pineforge

// Feature macro: the classes above exist.
#define PINEFORGE_HAS_TA_SERIES_LENGTH 1
// Feature macro: PineMom and PineRoc exist.
#define PINEFORGE_HAS_PINE_POSITIONAL_LAG 1
// Feature macro: PineFixedSupertrend exists.
#define PINEFORGE_HAS_PINE_FIXED_SUPERTREND 1
// Feature macro: PineVwma exists.
#define PINEFORGE_HAS_PINE_VWMA 1
// Feature macro: PineDmiReference exists.
#define PINEFORGE_HAS_PINE_DMI_REFERENCE 1
// Feature macro: PineSar exists.
#define PINEFORGE_HAS_PINE_SAR 1
