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

// The history a series-length window reads: one value per bar of the
// evaluation context, from the call site's first execution on. A bar the site
// executes holds its source; a bar it skips holds the value of the last
// execution before it. The last kMaxBarsBack + 1 bars are kept. Full chunks
// are shared between copies until written, so an in-memory checkpoint copies
// at most the current chunk. Numeric scratch backing is also copy-on-write;
// its resident cache size is independent of the retained lookback.
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
    void ensure_slots(std::size_t keep);
    std::size_t slot(std::size_t offset) const;
    void push(double value);
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
//   (RE10001, na reported as 0); a window reaching more than 15000 bars back
//   stops it too (RE10008) -- pinned on ta.lowest / ta.highestbars / ta.sma
//   (tv/edge), and 4990, 5001 and 9000-bar windows answered in full.
class SeriesWindowExtreme {
public:
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
// retains kMaxBarsBack + 1 values.
class SeriesPercentileNearestRank {
public:
    double compute(double src, double length, double percentage);
    double recompute(double src, double length, double percentage);
    void observe_owned_storage(OwnedStorage& out) const noexcept;

private:
    void append(double src);
    void replace_last(double src);
    double history_at(std::size_t offset) const;
    const std::vector<double>* rebuild_window(std::size_t length);

    std::vector<double> history_;
    std::optional<ta::PercentileWindow> window_;
    std::size_t window_length_ = 0;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
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
// truncated length, or a length above kMaxBarsBack + 1 stops via family errors.
// A valid call records only non-na sources and selects its current length
// from the last kMaxBarsBack + 1 non-na observations. Warm-up returns na.
// Finite fractions truncate toward zero; changing length preserves the ring.
// recompute replaces the most recent logical update: restore its pre-update
// head/count and any evicted oldest value, then record only a non-na source.
// Repeated recompute uses that same checkpoint; unused instances start there.
// Cache capacity controls residency only. Sum oldest first, with Neumaier
// double compensation. Finite TV clock/source-na evidence supports sample
// membership only; numerical order/domains, runtime overrides, whole-history
// TV parity and repeated/requested-context Pine clocks/rollback are unqualified.
// Copy/assignment preserve the ring and complete pre-update checkpoint;
// storage detaches on mutation.
class SeriesSma {
public:
    SeriesSma() : SeriesSma(window_cache_capacity()) {}
    explicit SeriesSma(WindowCacheCapacity cache) : history_(cache) {}
    double compute(double source, double current_length);
    double recompute(double source, double current_length);
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    static constexpr std::size_t kCapacity =
        static_cast<std::size_t>(SeriesWindowExtreme::kMaxBarsBack) + 1;
    static std::size_t checked_length(double current_length);
    void ensure_capacity();
    double mean(std::size_t length) const;

    ::pineforge::detail::WindowValues<double> history_;
    std::size_t head_ = 0; // next slot to write; the oldest when full
    std::size_t count_ = 0;
    std::size_t pre_update_head_ = 0;
    std::size_t pre_update_count_ = 0;
    double pre_update_evicted_ = na<double>();
    bool current_pushed_ = false;
};

// ta.ema, ta.tsi, ta.kc, ta.kcw and ta.macd as a script calls them: the
// kernel class of <pineforge/ta.hpp>, with the two rules a script's call has
// and the kernel's class does not.
//
// 1. A length of 0, a negative length or an na length stops the run. The
//    length is checked on every execution, compute() and recompute() alike,
//    before the source is read, so a first bar without a previous source, an
//    na source or an unready stage cannot answer na in its place. The stop is
//    simple_ta_length's: pine_invalid_argument with the rule must_be_positive
//    or must_not_be_na, under the function and argument names a simple length
//    of the same call is already checked under.
//    Measured: TradingView's stop and its text for ta.ema's length (see
//    simple_ta_length). Not measured: the stop and its text for ta.tsi,
//    ta.kc, ta.kcw and ta.macd, and which of several bad lengths TradingView
//    names. Those four apply the same check under the call's own names, and
//    the first bad length in the signature is the one reported here.
// 2. A value is published only once its inputs are ready. One rule for every
//    EMA stage: a stage has a value once `length` defined inputs have reached
//    it, and a stage fed by another stage counts only that stage's ready
//    values.
//    Measured: TradingView's built-in EMA is na until `length` defined
//    sources have accumulated (tests/test_chart_ema_na_warmup.cpp;
//    tests/test_series_origin_tailh_tape.cpp compares the declared-origin
//    warm-up with a TradingView tape), and an EMA fed by an EMA waits for the
//    ready values of the first: a weekly MACD's EMA26 first reads on its
//    26th bar and its EMA9 signal eight bars later (pine_security_eval.cpp,
//    retained weekly reference), and codegen issue 291 reports the histogram
//    of ta.macd(close, 12, 26, 9) first defined on bar 33.
//    Not measured: the first bars of ta.tsi, ta.kc and ta.kcw, and the bar a
//    stage becomes ready on when an na input falls inside its warm-up. The
//    classes apply the stage rule to the stages each function is built from:
//      ta.ema   after `length` defined sources;
//      ta.tsi   after `long_length` defined changes, then `short_length`
//               ready long-stage values;
//      ta.kc    the middle band after `length` defined sources, the bands
//               once the range stage has also seen `length` defined ranges;
//      ta.kcw   where ta.kc's bands are;
//      ta.macd  the kernel class already publishes by its stage clocks.
//    Only the publication moves. The numbers are the kernel's, bit for bit:
//    the seeding is still the one the instance latches (the ambient default
//    the host raises for a declared series origin, or the first value), an na
//    source still answers na and changes no state, and a zero ta.tsi
//    denominator still answers na. Under the average seeding the kernel's
//    value is already na exactly while the clock is not ready, so nothing
//    changes there. Which seeding a run without a declared origin should use
//    is not decided here and is not measured.
//
// Each class is a plain value: its only state is the kernel object and the
// lengths it was built with, so a copy is a checkpoint and recompute()
// rewinds the availability with the numbers.
class PineEma {
    ta::EMA impl_;
    int length_;

    void check() const {
        if (length_ <= 0) (void)simple_ta_length(length_, "ema", "length");
    }
    double published(double value) const {
        return impl_.ready() ? value : na<double>();
    }

public:
    explicit PineEma(int length) : impl_(length), length_(length) {}
    double compute(double src) {
        check();
        return published(impl_.compute(src));
    }
    double recompute(double src) {
        check();
        return published(impl_.recompute(src));
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, impl_);
    }
};

class PineTsi {
    ta::TSI impl_;
    int short_length_;
    int long_length_;

    void check() const {
        if (short_length_ <= 0)
            (void)simple_ta_length(short_length_, "tsi", "short_length");
        if (long_length_ <= 0)
            (void)simple_ta_length(long_length_, "tsi", "long_length");
    }
    double published(double value) const {
        return impl_.ready() ? value : na<double>();
    }

public:
    PineTsi(int short_length, int long_length)
        : impl_(short_length, long_length),
          short_length_(short_length), long_length_(long_length) {}
    double compute(double src) {
        check();
        return published(impl_.compute(src));
    }
    double recompute(double src) {
        check();
        return published(impl_.recompute(src));
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, impl_);
    }
};

class PineKc {
    ta::KC impl_;
    int length_;

    void check() const {
        if (length_ <= 0) (void)simple_ta_length(length_, "kc", "length");
    }
    ta::KCResult published(ta::KCResult result) const {
        const bool basis = impl_.basis_ready();
        if (!basis) result.middle = na<double>();
        if (!basis || !impl_.range_ready()) {
            result.upper = na<double>();
            result.lower = na<double>();
        }
        return result;
    }

public:
    PineKc(int length, double mult, bool use_true_range = true)
        : impl_(length, mult, use_true_range), length_(length) {}
    ta::KCResult compute(double src, double high, double low, double close) {
        check();
        return published(impl_.compute(src, high, low, close));
    }
    ta::KCResult recompute(double src, double high, double low, double close) {
        check();
        return published(impl_.recompute(src, high, low, close));
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, impl_);
    }
};

class PineKcw {
    ta::KCW impl_;
    int length_;

    void check() const {
        if (length_ <= 0) (void)simple_ta_length(length_, "kcw", "length");
    }
    double published(double value) const {
        return impl_.ready() ? value : na<double>();
    }

public:
    PineKcw(int length, double mult, bool use_true_range = true)
        : impl_(length, mult, use_true_range), length_(length) {}
    double compute(double src, double high, double low, double close) {
        check();
        return published(impl_.compute(src, high, low, close));
    }
    double recompute(double src, double high, double low, double close) {
        check();
        return published(impl_.recompute(src, high, low, close));
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, impl_);
    }
};

class PineMacd {
    ta::MACD impl_;
    int fast_length_;
    int slow_length_;
    int signal_length_;

    void check() const {
        if (fast_length_ <= 0)
            (void)simple_ta_length(fast_length_, "macd", "fastlen");
        if (slow_length_ <= 0)
            (void)simple_ta_length(slow_length_, "macd", "slowlen");
        if (signal_length_ <= 0)
            (void)simple_ta_length(signal_length_, "macd", "siglen");
    }

public:
    PineMacd(int fast_length, int slow_length, int signal_length)
        : impl_(fast_length, slow_length, signal_length),
          fast_length_(fast_length), slow_length_(slow_length),
          signal_length_(signal_length) {}
    ta::MACDResult compute(double src) {
        check();
        return impl_.compute(src);
    }
    ta::MACDResult recompute(double src) {
        check();
        return impl_.recompute(src);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, impl_);
    }
};

}  // namespace source
}  // namespace pineforge

// Feature macro: the classes above exist.
#define PINEFORGE_HAS_TA_SERIES_LENGTH 1

// Feature macro: PineEma, PineTsi, PineKc, PineKcw and PineMacd exist.
#define PINEFORGE_HAS_TA_EMA_FAMILY_CALLS 1
