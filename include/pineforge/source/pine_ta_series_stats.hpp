#pragma once
// Statistics built-ins whose length is a per-call (series) value: ta.stdev,
// ta.variance, ta.dev, ta.range, ta.median, ta.mode, ta.percentrank,
// ta.percentile_linear_interpolation, ta.correlation and math.sum.
//
// TradingView compiles a series length for each of them (observed in
// November 2026 on synthetic scripts whose length changes on every bar).
// What TradingView computes after a length change is not recorded by a tape
// for any of the ten: the rule below is this layer's definition of it.
//
// Every class takes the length on each call, as SeriesSma does, and answers
// what the fixed-length class of <pineforge/ta.hpp> answers for that length
// (for math.sum, math::Sum's window):
//
// * While the length of a call site stays the same, every call goes to one
//   fixed-length object (ta.range: see below), so the values are that
//   class's own, bit for bit, its na rule included. A site whose length
//   never changes is the fixed-length call.
// * A call with another length builds the fixed-length object of the new
//   length and replays the site's retained inputs through it, oldest first,
//   then the current input. The answer is the function over the last
//   `length` inputs as a new fixed-length object computes it.
// * ta.stdev, ta.variance, ta.dev, ta.median, ta.mode and math.sum never let
//   an na source into their window, so the site retains its non-na sources
//   and the window is the last `length` of them. An na call answers what the
//   fixed-length class answers: math.sum and ta.stdev hold the value of the
//   full window, the other four answer na.
// * ta.percentrank, ta.percentile_linear_interpolation and ta.correlation
//   keep one slot per call, na included: the window is the last `length`
//   calls (for ta.percentrank the current source and the `length` before
//   it). After a length change the percentile window is replayed in call
//   order, as SeriesPercentileNearestRank replays its own.
// * ta.range is SeriesHighest minus SeriesLowest on the same calls (the rule
//   documented at SeriesWindowExtreme), not a fixed-length object: it equals
//   ta::Range for a constant length only on a site that runs on every bar.
// * A call whose window needs more inputs than the site holds answers na and
//   builds nothing, so a very long length allocates nothing.
//
// The length is checked before anything is updated, by SeriesSma's rule and
// with SeriesSma's errors: an na or infinite length, a length below one
// after truncation toward zero, or a length beyond a configured lookback cap
// stops the run. Lookback is unlimited unless the host sets a cap; with a
// cap a site retains its last cap + 2 inputs.
//
// recompute() runs the current call again with other inputs, or another
// length, from the state the call found. Each compute() is one logical
// update of its site.
//
// math.sum and the biased ta.stdev keep compensated running sums. Those sums
// restart from the replayed window on a length change; SeriesSma, the one
// series-length sum recorded on TradingView, adjusts one running pair
// instead. The two can differ in the last bits of a sum.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/detail/ta_lookback.hpp>
#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/series.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/window_sum.hpp>

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge {
namespace source {

namespace series_stats_detail {

// simple_ta_length stops the run for an na, zero or negative value with
// TradingView's text; the callers pass only such a value.
[[noreturn]] inline void stop_for_length(double value, const char* function) {
    simple_ta_length(value, function, "length");
    pine_engine_invariant("a per-call length that must stop the run passed its check");
}

[[noreturn]] inline void stop_for_lookback(std::uint64_t offset, std::int64_t maximum,
                                           bool beyond_uint64) {
    const std::string text = run_error_bar_prefix() +
        "The script attempts to reference historical data that is too far from "
        "the current bar (" + (beyond_uint64 ? "more than " : "") + std::to_string(offset) +
        " bars back). The historical buffer's limit is " + std::to_string(maximum) + " bars.";
    pine_limit_stop("max_bars_back", maximum, text.c_str());
}

// SeriesSma's check of a per-call length, reported for `function`: the
// length truncated toward zero, or a stopped run.
inline std::uint64_t checked_length(double length, const char* function,
                                    std::int64_t maximum) {
    if (!std::isfinite(length)) stop_for_length(na<double>(), function);
    if (!(length >= 1.0)) stop_for_length(length, function);
    if (length >= 0x1p64) {
        constexpr auto last = std::numeric_limits<std::uint64_t>::max();
        if (maximum != 0) stop_for_lookback(last, maximum, length > 0x1p64);
        return last;
    }
    const auto requested = static_cast<std::uint64_t>(length);
    if (maximum != 0 && requested - 1 > static_cast<std::uint64_t>(maximum))
        stop_for_lookback(requested - 1, maximum, false);
    return requested;
}

// The length a fixed-length class is built with. A window longer than the
// inputs a site holds is never built, so the limit is not reached.
inline int fixed_length(std::uint64_t length) {
    return length > static_cast<std::uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(length);
}

// How many inputs a site retains: all of them (0) without a lookback cap,
// else the current one and the cap + 1 before it. A cap beyond addressable
// storage cannot cause eviction.
inline std::size_t retained_inputs(std::int64_t maximum) {
    constexpr std::size_t slots = std::numeric_limits<std::size_t>::max() / sizeof(double);
    if (maximum <= 0 || static_cast<std::uint64_t>(maximum) >= slots - 2) return 0;
    return static_cast<std::size_t>(maximum) + 2;
}

// The inputs one call site has recorded, oldest first. Without a limit every
// input is kept; with one the oldest leaves when the limit is reached.
// Numeric storage is copy-on-write and follows the cache capacity given at
// construction.
class Inputs {
public:
    Inputs(WindowCacheCapacity cache, std::size_t keep) : values_(cache), keep_(keep) {}
    // Copy even from an rvalue: the counts still name the source's storage.
    Inputs(const Inputs&) = default;
    Inputs& operator=(const Inputs&) = default;

    std::size_t size() const { return stored_; }
    void append(double value) {
        if (keep_ != 0 && stored_ == keep_) {
            oldest_ = oldest_ + 1 == values_.size() ? 0 : oldest_ + 1;
            --stored_;
        }
        if (stored_ == values_.size()) grow();
        values_.write(slot(stored_), value);
        ++stored_;
    }
    // The newest input leaves; the caller keeps size() above zero.
    void pop_back() { --stored_; }
    // The newest input is replaced; the caller keeps size() above zero.
    void replace_back(double value) { values_.write(slot(stored_ - 1), value); }
    // The input `back` places before the newest (0 is the newest); the
    // caller keeps `back` below size().
    double from_back(std::size_t back) const { return values_.read(slot(stored_ - 1 - back)); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        values_.observe_owned_storage(out);
    }

private:
    using Values = ::pineforge::detail::WindowValues<double>;

    std::size_t slot(std::size_t index) const {
        const std::size_t remaining = values_.size() - oldest_;
        return index >= remaining ? index - remaining : oldest_ + index;
    }
    // Storage grows only before a limit is reached, when it has not wrapped.
    void grow() {
        constexpr std::size_t most = std::numeric_limits<std::size_t>::max() / sizeof(double);
        const std::size_t held = values_.size();
        std::size_t capacity = held == 0 ? Values::kChunk : (held > most / 2 ? most : 2 * held);
        // One more than the representable extent is refused by the storage.
        if (capacity <= held) capacity = held + 1;
        if (keep_ != 0 && capacity > keep_) capacity = keep_;
        Values next(capacity, na<double>(), values_.cache_capacity());
        for (std::size_t index = 0; index < stored_; ++index)
            next.write(index, values_.read(slot(index)));
        values_ = std::move(next);
        oldest_ = 0;
    }

    Values values_;
    std::size_t keep_;
    std::size_t oldest_ = 0;   // where the oldest retained input is stored
    std::size_t stored_ = 0;
};

// The fixed-length object of `length`: built with `option` where the class
// takes one (ta::StdDev and ta::Variance: biased).
template <class Window>
void build_window(std::optional<Window>& window, int length, bool option) {
    if constexpr (std::is_constructible_v<Window, int, bool>) {
        window.emplace(length, option);
    } else {
        static_cast<void>(option);
        window.emplace(length);
    }
}

// math::Sum's compute() and recompute() on the window math::Sum uses, with
// the window's storage observable.
class SumWindow {
    KahanWindowSum window_;

public:
    explicit SumWindow(int length) : window_(length) {}
    double compute(double src) {
        if (is_na(src)) {
            window_.note_no_push();
            return window_.seeded() ? window_.sum() : na<double>();
        }
        const double sum = window_.push(src);
        return window_.seeded() ? sum : na<double>();
    }
    double recompute(double src) {
        if (is_na(src)) {
            window_.unpush();
            window_.note_no_push();
            return window_.seeded() ? window_.sum() : na<double>();
        }
        const double sum = window_.repush(src);
        return window_.seeded() ? sum : na<double>();
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        window_.observe_owned_storage(out);
    }
};

// A site of a function whose fixed-length class `Window` skips na sources:
// the site retains its non-na sources and `Window` holds the last `length`
// of them. `option` is the function's second constructor argument (biased),
// false for a function that has none. While a Window exists, its last call
// is this site's last call. A Window built by a call holds the sources of
// that call's window only, not the one that was the oldest before the call,
// so its own rewind cannot answer for the call run again (an na source would
// find a window that is not full): that call builds the Window again.
template <class Window>
class ValidSources {
public:
    explicit ValidSources(WindowCacheCapacity cache)
        : history_(cache, retained_inputs(max_lookback_)) {}
    ValidSources(const ValidSources&) = default;
    ValidSources& operator=(const ValidSources&) = default;

    double update(const char* function, double source, double length, bool option,
                  bool replace) {
        const std::uint64_t requested = checked_length(length, function, max_lookback_);
        const bool again = replace && called_;
        if (again && pushed_) history_.pop_back();
        const bool push = !is_na(source);
        if (push) history_.append(source);
        called_ = true;
        pushed_ = push;
        if (requested > history_.size()) {
            window_.reset();
            return na<double>();
        }
        const bool carried = window_.has_value() && !(again && built_) &&
                             window_length_ == requested && window_option_ == option;
        if (carried) {
            built_ = false;
            return again ? window_->recompute(source) : window_->compute(source);
        }
        series_stats_detail::build_window(window_, fixed_length(requested), option);
        built_ = true;
        window_length_ = requested;
        window_option_ = option;
        // The window's earlier sources, oldest first, then this call.
        const std::size_t newest = push ? 1 : 0;
        for (std::size_t earlier = static_cast<std::size_t>(requested) - newest; earlier != 0;
             --earlier)
            window_->compute(history_.from_back(newest + earlier - 1));
        return window_->compute(source);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, history_, window_);
    }

private:
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    Inputs history_;
    std::optional<Window> window_;
    std::uint64_t window_length_ = 0;
    bool window_option_ = false;
    bool called_ = false;
    bool pushed_ = false;      // the last call recorded its source
    bool built_ = false;       // the last call built window_
};

// A site of a one-source function whose fixed-length class `Window` keeps
// one slot per call, na included. `extra` is how many sources the window
// holds beyond `length`; `step(window, source, again)` makes one call of the
// fixed-length object. While a Window exists, its last call is this site's
// last call.
template <class Window>
class EveryCall {
public:
    explicit EveryCall(WindowCacheCapacity cache)
        : history_(cache, retained_inputs(max_lookback_)) {}
    EveryCall(const EveryCall&) = default;
    EveryCall& operator=(const EveryCall&) = default;

    template <class Step>
    double update(const char* function, double source, double length, std::size_t extra,
                  bool replace, Step step) {
        const std::uint64_t requested = checked_length(length, function, max_lookback_);
        const bool again = replace && called_;
        if (again) history_.replace_back(source);
        else history_.append(source);
        called_ = true;
        const std::size_t held = history_.size();
        if (held < extra || requested > held - extra) {
            window_.reset();
            return na<double>();
        }
        if (window_ && window_length_ == requested) return step(*window_, source, again);
        window_.emplace(fixed_length(requested));
        window_length_ = requested;
        for (std::size_t back = static_cast<std::size_t>(requested) + extra - 1; back != 0; --back)
            step(*window_, history_.from_back(back), false);
        return step(*window_, source, false);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, history_, window_);
    }

private:
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    Inputs history_;
    std::optional<Window> window_;
    std::uint64_t window_length_ = 0;
    bool called_ = false;
};

}  // namespace series_stats_detail

// math.sum(source, length) with a series length.
class SeriesMathSum {
public:
    SeriesMathSum() : SeriesMathSum(window_cache_capacity()) {}
    explicit SeriesMathSum(WindowCacheCapacity cache) : core_(cache) {}
    SeriesMathSum(const SeriesMathSum&) = default;
    SeriesMathSum& operator=(const SeriesMathSum&) = default;
    double compute(double source, double length) {
        return core_.update("sum", source, length, false, false);
    }
    double recompute(double source, double length) {
        return core_.update("sum", source, length, false, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    series_stats_detail::ValidSources<series_stats_detail::SumWindow> core_;
};

// ta.stdev(source, length, biased) with a series length. `biased` is taken
// on each call, before the length; the two-argument forms are biased = true.
class SeriesStdev {
public:
    SeriesStdev() : SeriesStdev(window_cache_capacity()) {}
    explicit SeriesStdev(WindowCacheCapacity cache) : core_(cache) {}
    SeriesStdev(const SeriesStdev&) = default;
    SeriesStdev& operator=(const SeriesStdev&) = default;
    double compute(double source, double length) { return compute(source, true, length); }
    double compute(double source, bool biased, double length) {
        return core_.update("stdev", source, length, biased, false);
    }
    double recompute(double source, double length) { return recompute(source, true, length); }
    double recompute(double source, bool biased, double length) {
        return core_.update("stdev", source, length, biased, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    series_stats_detail::ValidSources<ta::StdDev> core_;
};

// ta.variance(source, length, biased) with a series length. `biased` is
// taken on each call, before the length; the two-argument forms are
// biased = true.
class SeriesVariance {
public:
    SeriesVariance() : SeriesVariance(window_cache_capacity()) {}
    explicit SeriesVariance(WindowCacheCapacity cache) : core_(cache) {}
    SeriesVariance(const SeriesVariance&) = default;
    SeriesVariance& operator=(const SeriesVariance&) = default;
    double compute(double source, double length) { return compute(source, true, length); }
    double compute(double source, bool biased, double length) {
        return core_.update("variance", source, length, biased, false);
    }
    double recompute(double source, double length) { return recompute(source, true, length); }
    double recompute(double source, bool biased, double length) {
        return core_.update("variance", source, length, biased, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    series_stats_detail::ValidSources<ta::Variance> core_;
};

// ta.dev(source, length) with a series length.
class SeriesDev {
public:
    SeriesDev() : SeriesDev(window_cache_capacity()) {}
    explicit SeriesDev(WindowCacheCapacity cache) : core_(cache) {}
    SeriesDev(const SeriesDev&) = default;
    SeriesDev& operator=(const SeriesDev&) = default;
    double compute(double source, double length) {
        return core_.update("dev", source, length, false, false);
    }
    double recompute(double source, double length) {
        return core_.update("dev", source, length, false, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    series_stats_detail::ValidSources<ta::Dev> core_;
};

// ta.median(source, length) with a series length.
class SeriesMedian {
public:
    SeriesMedian() : SeriesMedian(window_cache_capacity()) {}
    explicit SeriesMedian(WindowCacheCapacity cache) : core_(cache) {}
    SeriesMedian(const SeriesMedian&) = default;
    SeriesMedian& operator=(const SeriesMedian&) = default;
    double compute(double source, double length) {
        return core_.update("median", source, length, false, false);
    }
    double recompute(double source, double length) {
        return core_.update("median", source, length, false, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    series_stats_detail::ValidSources<ta::Median> core_;
};

// ta.mode(source, length) with a series length.
class SeriesMode {
public:
    SeriesMode() : SeriesMode(window_cache_capacity()) {}
    explicit SeriesMode(WindowCacheCapacity cache) : core_(cache) {}
    SeriesMode(const SeriesMode&) = default;
    SeriesMode& operator=(const SeriesMode&) = default;
    double compute(double source, double length) {
        return core_.update("mode", source, length, false, false);
    }
    double recompute(double source, double length) {
        return core_.update("mode", source, length, false, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    series_stats_detail::ValidSources<ta::Mode> core_;
};

// ta.range(source, length) with a series length: the series-length highest
// minus the series-length lowest of the same calls, na where either is.
class SeriesRange {
public:
    SeriesRange() : SeriesRange(window_cache_capacity()) {}
    explicit SeriesRange(WindowCacheCapacity cache) : highest_(cache), lowest_(cache) {}
    SeriesRange(const SeriesRange&) = default;
    SeriesRange& operator=(const SeriesRange&) = default;
    double compute(double source, double length) {
        series_stats_detail::checked_length(length, "range", max_lookback_);
        const double high = highest_.compute(source, length);
        const double low = lowest_.compute(source, length);
        if (is_na(high) || is_na(low)) return na<double>();
        return high - low;
    }
    double recompute(double source, double length) {
        series_stats_detail::checked_length(length, "range", max_lookback_);
        const double high = highest_.recompute(source, length);
        const double low = lowest_.recompute(source, length);
        if (is_na(high) || is_na(low)) return na<double>();
        return high - low;
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        highest_.observe_owned_storage(out);
        lowest_.observe_owned_storage(out);
    }

private:
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    SeriesHighest highest_;
    SeriesLowest lowest_;
};

// ta.percentrank(source, length) with a series length: the current source
// ranked among the `length` sources of the calls before it.
class SeriesPercentrank {
public:
    SeriesPercentrank() : SeriesPercentrank(window_cache_capacity()) {}
    explicit SeriesPercentrank(WindowCacheCapacity cache) : core_(cache) {}
    SeriesPercentrank(const SeriesPercentrank&) = default;
    SeriesPercentrank& operator=(const SeriesPercentrank&) = default;
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        return core_.update("percentrank", source, length, 1, replace,
                            [](ta::PercentRank& window, double src, bool again) {
                                return again ? window.recompute(src) : window.compute(src);
                            });
    }

    series_stats_detail::EveryCall<ta::PercentRank> core_;
};

// ta.percentile_linear_interpolation(source, length, percentage) with a
// series length. The percentage is taken on each call, before the length.
class SeriesPercentileLinear {
public:
    SeriesPercentileLinear() : SeriesPercentileLinear(window_cache_capacity()) {}
    explicit SeriesPercentileLinear(WindowCacheCapacity cache) : core_(cache) {}
    SeriesPercentileLinear(const SeriesPercentileLinear&) = default;
    SeriesPercentileLinear& operator=(const SeriesPercentileLinear&) = default;
    double compute(double source, double percentage, double length) {
        return update(source, percentage, length, false);
    }
    double recompute(double source, double percentage, double length) {
        return update(source, percentage, length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        core_.observe_owned_storage(out);
    }

private:
    double update(double source, double percentage, double length, bool replace) {
        // The window's state does not depend on the percentage, so a replayed
        // call may carry the current one.
        return core_.update(
            "percentile_linear_interpolation", source, length, 0, replace,
            [percentage](ta::PercentileLinearInterpolation& window, double src, bool again) {
                return again ? window.recompute(src, percentage)
                             : window.compute(src, percentage);
            });
    }

    series_stats_detail::EveryCall<ta::PercentileLinearInterpolation> core_;
};

// ta.correlation(source1, source2, length) with a series length: every call
// records its pair, na included, and the window is the last `length` pairs.
class SeriesCorrelation {
public:
    SeriesCorrelation() : SeriesCorrelation(window_cache_capacity()) {}
    explicit SeriesCorrelation(WindowCacheCapacity cache)
        : first_(cache, series_stats_detail::retained_inputs(max_lookback_)),
          second_(cache, series_stats_detail::retained_inputs(max_lookback_)) {}
    SeriesCorrelation(const SeriesCorrelation&) = default;
    SeriesCorrelation& operator=(const SeriesCorrelation&) = default;
    double compute(double source1, double source2, double length) {
        return update(source1, source2, length, false);
    }
    double recompute(double source1, double source2, double length) {
        return update(source1, source2, length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, first_, second_, window_);
    }

private:
    double update(double source1, double source2, double length, bool replace) {
        const std::uint64_t requested =
            series_stats_detail::checked_length(length, "correlation", max_lookback_);
        const bool again = replace && called_;
        if (again) {
            first_.replace_back(source1);
            second_.replace_back(source2);
        } else {
            first_.append(source1);
            second_.append(source2);
        }
        called_ = true;
        if (requested > first_.size()) {
            window_.reset();
            return na<double>();
        }
        // While a window exists, its last call is this site's last call.
        if (window_ && window_length_ == requested)
            return again ? window_->recompute(source1, source2)
                         : window_->compute(source1, source2);
        window_.emplace(series_stats_detail::fixed_length(requested));
        window_length_ = requested;
        for (std::size_t back = static_cast<std::size_t>(requested) - 1; back != 0; --back)
            window_->compute(first_.from_back(back), second_.from_back(back));
        return window_->compute(source1, source2);
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_stats_detail::Inputs first_;
    series_stats_detail::Inputs second_;
    std::optional<ta::Correlation> window_;
    std::uint64_t window_length_ = 0;
    bool called_ = false;
};

}  // namespace source
}  // namespace pineforge

// Feature macros: one per class above.
#define PINEFORGE_HAS_PINE_SERIES_STDEV 1
#define PINEFORGE_HAS_PINE_SERIES_VARIANCE 1
#define PINEFORGE_HAS_PINE_SERIES_DEV 1
#define PINEFORGE_HAS_PINE_SERIES_RANGE 1
#define PINEFORGE_HAS_PINE_SERIES_MEDIAN 1
#define PINEFORGE_HAS_PINE_SERIES_MODE 1
#define PINEFORGE_HAS_PINE_SERIES_PERCENTRANK 1
#define PINEFORGE_HAS_PINE_SERIES_PERCENTILELINEAR 1
#define PINEFORGE_HAS_PINE_SERIES_CORRELATION 1
#define PINEFORGE_HAS_PINE_SERIES_MATHSUM 1
// The two names of more than one word, also spelled with the word break the
// other feature macros of this layer use.
#define PINEFORGE_HAS_PINE_SERIES_PERCENTILE_LINEAR 1
#define PINEFORGE_HAS_PINE_SERIES_MATH_SUM 1
