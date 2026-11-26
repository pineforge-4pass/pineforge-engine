#pragma once
// TradingView's oscillator and momentum ta.* calls whose length is a series:
// ta.stoch, ta.cci, ta.mfi, ta.mom, ta.roc, ta.cmo, ta.wpr, ta.cog, ta.rising
// and ta.falling.
//
// TradingView compiles each of these ten functions with a length that changes
// from bar to bar (observed on synthetic scripts, one call per script). The
// fixed-length classes of <pineforge/ta.hpp> size their window in the
// constructor, so they cannot take such a length. The classes below take the
// length on every call, as SeriesSma of <pineforge/source/pine_ta_length.hpp>
// does, and share its shape: a default constructor and one taking a
// WindowCacheCapacity, compute(<inputs>, length), recompute(<inputs>, length)
// and observe_owned_storage. The inputs are the ones the fixed-length class
// takes, in the same order; the length is always the last argument.
//
// The rule of every class: a call answers what the fixed-length call answers
// for the length of that call, on the inputs the site has received so far.
// A length that stays constant therefore gives the fixed-length class's
// values on every bar, bit for bit, na bars included, for a site that
// executes on every bar. The values TradingView gives after the length
// changes are not recorded for these ten functions: the rule above is the
// definition of each function applied to the last `length` bars.
//
// Which inputs a class keeps, and what an na input does, follow the
// fixed-length class of the same function:
//
// * SeriesMom, SeriesRoc: one position per call, na included. The answer
//   reads the position `length` calls back and is na when that position or
//   the current source is na (`source - source[length]`, and
//   `(source - source[length]) / source[length] * 100` with na for a zero
//   `source[length]`). On a source with no na after its first value this is
//   ta::Mom and ta::ROC bit for bit; after an na bar those two classes read
//   the length-th previous present sample instead, and the positional rule
//   here is the one of Pine's history subscript.
// * SeriesRising, SeriesFalling: the last `length + 1` non-na sources, as
//   ta::Rising and ta::Falling keep them. An na source answers 0 and records
//   nothing. The answer is 1 when every consecutive pair of that window
//   rises (falls) strictly, else 0; the result type is the double those two
//   classes return.
// * SeriesCci: the last `length` non-na sources, as ta::CCI keeps them. An na
//   source answers na and records nothing.
// * SeriesCog: one position per call, na included, as ta::COG keeps them; an
//   na inside the window makes the answer na.
// * SeriesCmo, SeriesMfi: one position per call, na included. The answer sums
//   the last `length` bar-to-bar moves (ta::CMO) or money flows (ta::MFI) and
//   is na until `length` earlier calls exist. A move out of an na source
//   counts as zero, as in the two classes.
// * SeriesStoch, SeriesWpr: the highest high and the lowest low of the last
//   `length` bars come from SeriesWindowExtreme, the series-length rule of
//   ta.highest and ta.lowest, where ta::Stoch and ta::WPR read ta::Highest
//   and ta::Lowest. For a site that executes on every bar the two rules give
//   the same extremes for a constant length. A site inside an `if` follows
//   SeriesWindowExtreme's held history, not the fixed classes' ring.
//
// The length is checked before anything is recorded, as SeriesSma checks it:
// an na or non-finite length stops the run ("... It must not be na"), a
// length below 1 stops it ("... It must be > 0."), a finite fraction
// truncates toward zero, and a configured lookback cap refuses a length
// whose `length - 1` exceeds it. The texts name the function without "ta.".
// A failed check changes nothing, so the next call starts from the same
// state.
//
// recompute() replaces the call made last: it starts from the state that
// call found, whatever its length was. A recompute() before the first
// compute() is that first compute().
//
// History is kept whole by default, or bounded by the lookback cap when the
// host sets one (enough for every length the cap admits). Storage is
// copy-on-write; the cache capacity controls residency only.
//
// The arithmetic is inline and is meant to be compiled with the flags the
// library exports (-ffp-contract=off): ta.cog's weighted sum must not be
// contracted into a fused multiply-add if it is to equal ta::COG's.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/detail/ta_lookback.hpp>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/series.hpp>
#include <pineforge/ta_compare_band.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace pineforge {
namespace source {

namespace series_osc_detail {

inline constexpr std::size_t kMaxHistorySlots =
    std::numeric_limits<std::size_t>::max() / sizeof(double);

[[noreturn]] inline void lookback_exceeded(std::uint64_t offset, std::int64_t maximum,
                                           bool beyond_uint64) {
    throw coded<std::runtime_error>(
        RunFailureCode::pine_runtime_limit,
        {{"limit", "max_bars_back"}, {"max", maximum}},
        run_error_bar_prefix() +
            "The script attempts to reference historical data that is too far from "
            "the current bar (" + (beyond_uint64 ? "more than " : "") +
            std::to_string(offset) + " bars back). The historical buffer's limit is " +
            std::to_string(maximum) + " bars.");
}

// The length of one call as the unsigned count it names, checked the way
// SeriesSma checks its own: the same rules, codes and texts, with this
// function's name.
inline std::uint64_t checked_length(double length, const char* function,
                                    std::int64_t maximum) {
    if (!std::isfinite(length)) {
        throw coded<std::runtime_error>(
            RunFailureCode::pine_invalid_argument,
            {{"function", function}, {"argument", "length"}, {"rule", "must_not_be_na"}},
            run_error_bar_prefix() + "Invalid value of the 'length' argument in the '" +
                function + "' function. It must not be na");
    }
    if (!(length >= 1.0)) {
        // Truncated toward zero, saturating far outside any window.
        const double truncated = std::trunc(length);
        const long long shown = truncated < -1e15 ? static_cast<long long>(-1e15)
                                                  : static_cast<long long>(truncated);
        throw coded<std::runtime_error>(
            RunFailureCode::pine_invalid_argument,
            {{"function", function}, {"argument", "length"}, {"rule", "must_be_positive"}},
            run_error_bar_prefix() + "Invalid value of the 'length' argument (" +
                std::to_string(shown) + ") in the '" + function +
                "' function. It must be > 0.");
    }
    if (length >= 0x1p64) {
        constexpr auto last = std::numeric_limits<std::uint64_t>::max();
        if (maximum != 0) lookback_exceeded(last, maximum, length > 0x1p64);
        return last;
    }
    // The unsigned conversion preserves the integer part, including 2^63.
    const auto requested = static_cast<std::uint64_t>(length);
    if (maximum != 0 && requested - 1 > static_cast<std::uint64_t>(maximum))
        lookback_exceeded(requested - 1, maximum, false);
    return requested;
}

// How many values a history keeps under the lookback cap `maximum`: enough
// for the longest length the cap admits (`maximum + 1`) plus `extra` older
// values the function reads behind its window. 0 keeps every value: no cap,
// or a cap beyond addressable storage, which cannot cause an eviction.
inline std::size_t history_slots(std::int64_t maximum, std::size_t extra) {
    const auto cap = static_cast<std::uint64_t>(maximum);
    if (maximum == 0 || cap >= kMaxHistorySlots - extra) return 0;
    return static_cast<std::size_t>(cap) + 1 + extra;
}

// The values one call site has recorded, oldest first: all of them, or the
// newest `limit`. undo() takes back the value recorded last and brings back
// the one it pushed out, so a recompute() starts from what its bar found.
class History {
public:
    static constexpr std::size_t kChunk = 1024;

    History(WindowCacheCapacity cache, std::size_t limit) : values_(cache), limit_(limit) {}
    // Copy even from an rvalue: the counts still name the source's storage.
    History(const History&) = default;
    History& operator=(const History&) = default;

    std::size_t size() const noexcept { return count_; }

    // The value `back` records before the newest one; `back` < size().
    double newest(std::size_t back) const {
        const std::size_t offset = back + 1;
        return values_.read(offset <= next_ ? next_ - offset
                                            : values_.size() - (offset - next_));
    }

    void push(double value) {
        if (limit_ != 0 && count_ == limit_) {
            // Full: the oldest value leaves and is kept for undo().
            const double oldest = values_.read(next_);
            values_.write(next_, value);
            evicted_value_ = oldest;
            evicted_ = true;
            next_ = next_ + 1 == limit_ ? 0 : next_ + 1;
        } else {
            if (next_ == values_.size()) grow(count_ + 1);
            values_.write(next_, value);
            evicted_ = false;
            ++count_;
            ++next_;
            if (count_ == limit_) next_ = 0;
        }
        pushed_ = true;
    }

    // The call recorded nothing: there is nothing for undo() to take back.
    void skip() noexcept { pushed_ = false; }

    void undo() {
        if (!pushed_) return;
        if (evicted_) {
            const std::size_t slot = next_ == 0 ? limit_ - 1 : next_ - 1;
            values_.write(slot, evicted_value_);
            next_ = slot;
        } else {
            next_ = (next_ == 0 ? values_.size() : next_) - 1;
            --count_;
        }
        pushed_ = false;
    }

    void observe_owned_storage(OwnedStorage& out) const noexcept {
        values_.observe_owned_storage(out);
    }

private:
    // Storage grows only before it has ever been full, so the values still
    // sit in slots [0, count_) and keep their indices in the copy.
    void grow(std::size_t minimum) {
        if (minimum > kMaxHistorySlots) {
            throw coded<std::length_error>(
                RunFailureCode::scratch_capacity_exceeded, {{"resource", "layout"}},
                "history byte extent is not representable");
        }
        const std::size_t current = values_.size();
        std::size_t wanted = kChunk;
        if (current != 0)
            wanted = current > kMaxHistorySlots / 2 ? kMaxHistorySlots : 2 * current;
        wanted = std::max(wanted, minimum);
        wanted = std::min(wanted, limit_ != 0 ? limit_ : kMaxHistorySlots);
        ::pineforge::detail::WindowValues<double> next(wanted, na<double>(),
                                                       values_.cache_capacity());
        for (std::size_t i = 0; i < count_; ++i) next.write(i, values_.read(i));
        values_ = std::move(next);
    }

    ::pineforge::detail::WindowValues<double> values_;
    std::size_t limit_;        // 0: every value is kept
    std::size_t next_ = 0;     // slot of the next write
    std::size_t count_ = 0;    // values held
    bool pushed_ = false;      // the last call recorded a value
    bool evicted_ = false;     // ... and that value pushed the oldest one out
    double evicted_value_ = na<double>();
};

// One position per call, na included. False while fewer than `length`
// earlier positions exist; otherwise `previous` is the value `length`
// positions back, na if that position holds na.
inline bool lagged(History& history, double source, std::uint64_t length, bool replace,
                   double& previous) {
    if (replace) history.undo();
    history.push(source);
    if (history.size() <= length) return false;
    previous = history.newest(static_cast<std::size_t>(length));
    return true;
}

// The last `count` non-na sources including this one. False when the source
// is na (nothing is recorded) or fewer than `count` are held.
inline bool present_window(History& history, double source, std::uint64_t count,
                           bool replace) {
    if (replace) history.undo();
    if (is_na(source)) {
        history.skip();
        return false;
    }
    history.push(source);
    return history.size() >= count;
}

}  // namespace series_osc_detail

// ta.stoch(source, high, low, length) with a series length:
// 100 * (source - lowest(low, length)) / (highest(high, length) - lowest(low, length)),
// the multiplication first, na when either extreme or the source is na and
// na on a zero range, as ta::Stoch answers.
class SeriesStoch {
public:
    SeriesStoch() : SeriesStoch(window_cache_capacity()) {}
    explicit SeriesStoch(WindowCacheCapacity cache) : highest_(cache), lowest_(cache) {}
    double compute(double source, double high, double low, double length) {
        return update(source, high, low, length, true);
    }
    double recompute(double source, double high, double low, double length) {
        return update(source, high, low, length, false);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        highest_.observe_owned_storage(out);
        lowest_.observe_owned_storage(out);
    }

private:
    double update(double source, double high, double low, double length, bool advance) {
        // Checked once here, so neither window moves on a refused length.
        (void)series_osc_detail::checked_length(length, "stoch", max_lookback_);
        const double hi = highest_.update_series(high, length, advance, true, "stoch").value;
        const double lo = lowest_.update_series(low, length, advance, false, "stoch").value;
        if (is_na(hi) || is_na(lo) || is_na(source)) return na<double>();
        const double range = hi - lo;
        if (range == 0.0) return na<double>();
        return 100.0 * (source - lo) / range;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    SeriesWindowExtreme highest_;
    SeriesWindowExtreme lowest_;
};

// ta.wpr(length) with a series length:
// (highest(high, length) - close) / (highest(high, length) - lowest(low, length)) * -100,
// na when either extreme is na or the two are equal, as ta::WPR answers. The
// inputs are the three ta::WPR takes, in its order: close, high, low.
class SeriesWpr {
public:
    SeriesWpr() : SeriesWpr(window_cache_capacity()) {}
    explicit SeriesWpr(WindowCacheCapacity cache) : highest_(cache), lowest_(cache) {}
    double compute(double close, double high, double low, double length) {
        return update(close, high, low, length, true);
    }
    double recompute(double close, double high, double low, double length) {
        return update(close, high, low, length, false);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        highest_.observe_owned_storage(out);
        lowest_.observe_owned_storage(out);
    }

private:
    double update(double close, double high, double low, double length, bool advance) {
        (void)series_osc_detail::checked_length(length, "wpr", max_lookback_);
        const double hh = highest_.update_series(high, length, advance, true, "wpr").value;
        const double ll = lowest_.update_series(low, length, advance, false, "wpr").value;
        if (is_na(hh) || is_na(ll) || hh == ll) return na<double>();
        return (hh - close) / (hh - ll) * -100.0;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    SeriesWindowExtreme highest_;
    SeriesWindowExtreme lowest_;
};

// ta.cci(source, length) with a series length, over the last `length` non-na
// sources: (source - mean) / (0.015 * mean absolute deviation), 0 on a zero
// deviation, in ta::CCI's operation order (both sums oldest first).
class SeriesCci {
public:
    SeriesCci() : SeriesCci(window_cache_capacity()) {}
    explicit SeriesCci(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 0)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n = series_osc_detail::checked_length(length, "cci", max_lookback_);
        if (!series_osc_detail::present_window(history_, source, n, replace))
            return na<double>();
        const auto count = static_cast<std::size_t>(n);
        double sum = 0.0;
        for (std::size_t back = count; back > 0; --back) sum += history_.newest(back - 1);
        const double mean = sum / static_cast<double>(n);
        double md_sum = 0.0;
        for (std::size_t back = count; back > 0; --back)
            md_sum += std::abs(history_.newest(back - 1) - mean);
        const double mean_dev = md_sum / static_cast<double>(n);
        if (mean_dev == 0.0) return 0.0;
        return (source - mean) / (0.015 * mean_dev);
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

// ta.mfi(series, length) with a series length. The inputs are the two
// ta::MFI takes, in its order: the source, then the bar's volume. Each call
// has a money flow `source * volume`, positive when the source is above the
// previous call's source by more than the comparison band, negative when it
// is below by more than the band, zero otherwise and after an na source.
// The answer is 100 - 100 / (1 + positive sum / negative sum) over the last
// `length` flows, summed oldest first, 100 on a zero negative sum, and na
// until `length` earlier calls exist.
class SeriesMfi {
public:
    SeriesMfi() : SeriesMfi(window_cache_capacity()) {}
    explicit SeriesMfi(WindowCacheCapacity cache)
        : sources_(cache, series_osc_detail::history_slots(max_lookback_, 1)),
          volumes_(cache, series_osc_detail::history_slots(max_lookback_, 1)) {}
    double compute(double source, double volume, double length) {
        return update(source, volume, length, false);
    }
    double recompute(double source, double volume, double length) {
        return update(source, volume, length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        sources_.observe_owned_storage(out);
        volumes_.observe_owned_storage(out);
    }

private:
    double update(double source, double volume, double length, bool replace) {
        const std::uint64_t n = series_osc_detail::checked_length(length, "mfi", max_lookback_);
        if (replace) {
            sources_.undo();
            volumes_.undo();
        }
        sources_.push(source);
        volumes_.push(volume);
        if (sources_.size() <= n) return na<double>();
        const auto count = static_cast<std::size_t>(n);
        double pos_sum = 0, neg_sum = 0;
        double previous = sources_.newest(count);
        for (std::size_t back = count; back > 0; --back) {
            const double src = sources_.newest(back - 1);
            double pos = 0, neg = 0;
            if (!is_na(previous)) {
                const double flow = src * volumes_.newest(back - 1);
                if (float_band_gt(src, previous)) pos = flow;
                else if (float_band_lt(src, previous)) neg = flow;
            }
            pos_sum += pos;
            neg_sum += neg;
            previous = src;
        }
        if (neg_sum == 0) return 100.0;
        return 100.0 - 100.0 / (1.0 + pos_sum / neg_sum);
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History sources_;
    series_osc_detail::History volumes_;
};

// ta.mom(source, length) with a series length: `source - source[length]`,
// positions counted one per call, na included.
class SeriesMom {
public:
    SeriesMom() : SeriesMom(window_cache_capacity()) {}
    explicit SeriesMom(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 1)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n = series_osc_detail::checked_length(length, "mom", max_lookback_);
        double previous = na<double>();
        if (!series_osc_detail::lagged(history_, source, n, replace, previous) ||
            is_na(source) || is_na(previous)) {
            return na<double>();
        }
        return source - previous;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

// ta.roc(source, length) with a series length:
// `(source - source[length]) / source[length] * 100` in ta::ROC's operation
// order, na for a zero `source[length]`, positions counted as SeriesMom
// counts them.
class SeriesRoc {
public:
    SeriesRoc() : SeriesRoc(window_cache_capacity()) {}
    explicit SeriesRoc(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 1)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n = series_osc_detail::checked_length(length, "roc", max_lookback_);
        double previous = na<double>();
        if (!series_osc_detail::lagged(history_, source, n, replace, previous) ||
            is_na(source) || is_na(previous) || previous == 0.0) {
            return na<double>();
        }
        return (source - previous) / previous * 100.0;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

// ta.cmo(series, length) with a series length. Each call has an upward move
// (the source's rise over the previous call's source) and a downward move
// (its fall, or minus a zero or na difference, as ta::CMO stores it); both
// are zero after an na source. The answer is 100 * (up - down) / (up + down)
// over the last `length` moves, summed oldest first, na on a zero total and
// na until `length` earlier calls exist.
class SeriesCmo {
public:
    SeriesCmo() : SeriesCmo(window_cache_capacity()) {}
    explicit SeriesCmo(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 1)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n = series_osc_detail::checked_length(length, "cmo", max_lookback_);
        if (replace) history_.undo();
        history_.push(source);
        if (history_.size() <= n) return na<double>();
        const auto count = static_cast<std::size_t>(n);
        double up_sum = 0, down_sum = 0;
        double previous = history_.newest(count);
        for (std::size_t back = count; back > 0; --back) {
            const double src = history_.newest(back - 1);
            double up = 0, down = 0;
            if (!is_na(previous)) {
                const double diff = src - previous;
                if (diff > 0) up = diff;
                else down = -diff;
            }
            up_sum += up;
            down_sum += down;
            previous = src;
        }
        const double denom = up_sum + down_sum;
        if (denom == 0) return na<double>();
        return 100.0 * (up_sum - down_sum) / denom;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

// ta.cog(source, length) with a series length, over the last `length` calls:
// minus the sum of source[i] * (i + 1), newest first, over the sum of the
// sources, oldest first, as ta::COG adds them; na on a zero sum and until
// `length` calls exist.
class SeriesCog {
public:
    SeriesCog() : SeriesCog(window_cache_capacity()) {}
    explicit SeriesCog(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 0)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n = series_osc_detail::checked_length(length, "cog", max_lookback_);
        if (replace) history_.undo();
        history_.push(source);
        if (history_.size() < n) return na<double>();
        const auto count = static_cast<std::size_t>(n);
        double num = 0, den = 0;
        for (std::size_t i = 0; i < count; ++i) {
            num += history_.newest(i) * static_cast<double>(i + 1);
            den += history_.newest(count - 1 - i);
        }
        if (den == 0) return na<double>();
        return -num / den;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

// ta.rising(source, length) with a series length: 1 when the last
// `length + 1` non-na sources rise strictly from each one to the next, else
// 0; 0 on an na source and until that many sources exist.
class SeriesRising {
public:
    SeriesRising() : SeriesRising(window_cache_capacity()) {}
    explicit SeriesRising(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 1)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n =
            series_osc_detail::checked_length(length, "rising", max_lookback_);
        // `length + 1` sources: more than `length` of them.
        if (!series_osc_detail::present_window(history_, source, 1, replace) ||
            history_.size() <= n) {
            return 0.0;
        }
        for (auto back = static_cast<std::size_t>(n); back > 0; --back) {
            if (history_.newest(back - 1) <= history_.newest(back)) return 0.0;
        }
        return 1.0;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

// ta.falling(source, length) with a series length: SeriesRising's mirror.
class SeriesFalling {
public:
    SeriesFalling() : SeriesFalling(window_cache_capacity()) {}
    explicit SeriesFalling(WindowCacheCapacity cache)
        : history_(cache, series_osc_detail::history_slots(max_lookback_, 1)) {}
    double compute(double source, double length) { return update(source, length, false); }
    double recompute(double source, double length) { return update(source, length, true); }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        history_.observe_owned_storage(out);
    }

private:
    double update(double source, double length, bool replace) {
        const std::uint64_t n =
            series_osc_detail::checked_length(length, "falling", max_lookback_);
        if (!series_osc_detail::present_window(history_, source, 1, replace) ||
            history_.size() <= n) {
            return 0.0;
        }
        for (auto back = static_cast<std::size_t>(n); back > 0; --back) {
            if (history_.newest(back - 1) >= history_.newest(back)) return 0.0;
        }
        return 1.0;
    }

    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
    series_osc_detail::History history_;
};

}  // namespace source
}  // namespace pineforge

// Feature macros: one per class above.
#define PINEFORGE_HAS_PINE_SERIES_STOCH 1
#define PINEFORGE_HAS_PINE_SERIES_CCI 1
#define PINEFORGE_HAS_PINE_SERIES_MFI 1
#define PINEFORGE_HAS_PINE_SERIES_MOM 1
#define PINEFORGE_HAS_PINE_SERIES_ROC 1
#define PINEFORGE_HAS_PINE_SERIES_CMO 1
#define PINEFORGE_HAS_PINE_SERIES_WPR 1
#define PINEFORGE_HAS_PINE_SERIES_COG 1
#define PINEFORGE_HAS_PINE_SERIES_RISING 1
#define PINEFORGE_HAS_PINE_SERIES_FALLING 1
