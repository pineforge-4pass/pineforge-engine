#pragma once
// TradingView's moving-average and band calls with a length that is not the
// same on every bar: ta.wma, ta.vwma, ta.alma, ta.linreg, ta.bb and ta.bbw
// with a series length, and the bands of ta.vwap with a series stdev
// multiplier.
//
// TradingView compiles each of these calls with a series value in that
// argument: observed on synthetic scripts, one call each, whose value is
// `2 + bar_index % 3`. It refuses a series `offset` for ta.linreg and a
// series `mult` for ta.bb. Which values TradingView computes when the length
// changes has not been recorded: the rule below is the fixed-length class's
// own window, taken at the call's length.
//
// The rule, for the six classes with a length:
//
// * A class keeps the samples its fixed-length class of <pineforge/ta.hpp>
//   records, in call order. ta::WMA, ta::SMA and ta::StdDev record a source
//   that is not na; ta::ALMA and ta::Linreg record every call, na included.
// * A call with `length` builds the fixed-length class for that length, gives
//   it the last `length` samples recorded before the bar, oldest first, and
//   then makes the call on it. The following calls with the same length (and
//   the same other constructor arguments) are that object's own compute() and
//   recompute(). A length that never changes therefore answers the
//   fixed-length class's values bit for bit, warm-up included.
// * A call with another length builds a new object in the same way. Where
//   the fixed-length class sums its window on every call (ta::WMA, ta::ALMA,
//   ta::Linreg) the answer is bit for bit what that class answers for the new
//   length after the whole history. Where it keeps running sums (ta::SMA and
//   ta::StdDev: SeriesVwma, SeriesBb and SeriesBbw) the sums start again at
//   the oldest sample given, so the answer can differ in the last place from
//   an object of that length that has run since the first bar.
// * While fewer than `length` samples have been recorded the answer is na
//   (every band of ta.bb), as the fixed-length class answers before its
//   window is full, and no object of that length is built.
// * recompute() replaces the call of the current bar: the bar is answered
//   again from the samples recorded before it, whatever the two lengths are.
//   A recompute() before the first compute() is that first compute().
// * The length is checked before anything is recorded, as SeriesSma checks
//   its own (pine_ta_length.hpp): a length that is not finite stops the run
//   with "Invalid value of the 'length' argument in the '<function>'
//   function. It must not be na", a length below 1 with "Invalid value of the
//   'length' argument (<value>) in the '<function>' function. It must be >
//   0.", and a length whose window reaches further back than the host's
//   lookback limit with the max_bars_back text. A finite fraction truncates
//   toward zero. Without a limit every recorded sample is kept; with one, the
//   last limit + 1.
//
// The compute() and recompute() arguments are the Pine call's inputs in the
// call's order, without the length, and then the length. An int or int64
// length goes through ta_number (pine_ta_length.hpp), which keeps an integer
// na as na.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/ta_lookback.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge {
namespace source {

namespace series_ma_detail {

// The stop of a window that reaches further back than the host's lookback
// limit: the code, the arguments and the text SeriesSma stops with.
[[noreturn]] inline void lookback_exceeded(std::uint64_t offset, std::int64_t maximum,
                                           bool beyond_uint64) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "%sThe script attempts to reference historical data that is too far from "
                  "the current bar (%s%llu bars back). The historical buffer's limit is "
                  "%lld bars.",
                  run_error_bar_prefix().c_str(), beyond_uint64 ? "more than " : "",
                  static_cast<unsigned long long>(offset), static_cast<long long>(maximum));
    pine_limit_stop("max_bars_back", maximum, text);
}

// A per-call length as the window size it asks for. The two argument stops
// are simple_ta_length's own, so their code and text are the ones every
// checked length of the source layer has.
inline std::uint64_t checked_length(double current_length, const char* function,
                                    std::int64_t maximum) {
    if (!std::isfinite(current_length)) {
        (void)simple_ta_length(na<double>(), function, "length");
        pine_engine_invariant("a series length that is not finite passed its check");
    }
    if (!(current_length >= 1.0)) {
        (void)simple_ta_length(current_length, function, "length");
        pine_engine_invariant("a series length below one passed its check");
    }
    constexpr std::uint64_t last = std::numeric_limits<std::uint64_t>::max();
    if (current_length >= 0x1p64) {
        if (maximum != 0) lookback_exceeded(last, maximum, current_length > 0x1p64);
        return last;
    }
    // The unsigned conversion keeps the integer part, including 2^63.
    const std::uint64_t requested = static_cast<std::uint64_t>(current_length);
    if (maximum != 0 && requested - 1 > static_cast<std::uint64_t>(maximum))
        lookback_exceeded(requested - 1, maximum, false);
    return requested;
}

constexpr std::size_t kMaxSamples = std::numeric_limits<std::size_t>::max() / sizeof(double);
constexpr std::size_t kFirstSamples = 1024;

// How many recorded samples are kept: all of them without a lookback limit
// (0), the limit plus one with it, which is the longest window a checked
// length can ask for.
inline std::size_t sample_limit(std::int64_t maximum) {
    if (maximum == 0 || static_cast<std::uint64_t>(maximum) >= kMaxSamples) return 0;
    return static_cast<std::size_t>(maximum) + 1;
}

inline bool same_number(double a, double b) { return a == b || (is_na(a) && is_na(b)); }

// The recorded samples of one window, oldest to newest. Storage grows as
// samples arrive; with a limit it becomes a ring of that many samples.
// Numeric storage is copy-on-write, so a copy shares pages until it writes.
class RecordedSamples {
public:
    explicit RecordedSamples(WindowCacheCapacity cache) : values_(cache) {}

    std::size_t count() const { return count_; }
    // The sample recorded `back` samples ago, 1 being the newest. The caller
    // keeps `back` in [1, count()].
    double newest(std::size_t back) const {
        return values_.read(back <= head_ ? head_ - back : values_.size() - (back - head_));
    }
    // Adds one sample. With a limit the oldest leaves once `limit` are held.
    void append(double value, std::size_t limit) {
        const bool full = limit != 0 && count_ == limit;
        if (!full) ensure_room(limit);
        values_.write(head_, value);
        const std::size_t next = head_ + 1;
        head_ = next == limit ? 0 : next;
        if (!full) ++count_;
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        values_.observe_owned_storage(out);
    }

private:
    void ensure_room(std::size_t limit) {
        const std::size_t size = values_.size();
        if (count_ < size) return;
        if (size >= kMaxSamples)
            throw coded<std::length_error>(RunFailureCode::scratch_capacity_exceeded,
                {{"resource", "layout"}}, "history byte extent is not representable");
        std::size_t capacity = kFirstSamples;
        if (size != 0) capacity = size > kMaxSamples / 2 ? kMaxSamples : 2 * size;
        if (limit != 0 && capacity > limit) capacity = limit;
        // Storage that still grows has never wrapped: the samples held are
        // the first count_ slots.
        ::pineforge::detail::WindowValues<double> grown(capacity, na<double>(),
                                                        values_.cache_capacity());
        for (std::size_t i = 0; i < count_; ++i) grown.write(i, values_.read(i));
        values_ = std::move(grown);
    }

    ::pineforge::detail::WindowValues<double> values_;
    std::size_t head_ = 0;   // the next write
    std::size_t count_ = 0;  // samples held
};

// One fixed-length object that follows the length (and the other constructor
// arguments) of every call, by the rule of the file note. `Window` names the
// fixed-length class and how it is built and fed.
template <class Window>
class Rewindowed {
public:
    using Fixed = typename Window::Fixed;
    using Arguments = typename Window::Arguments;

    explicit Rewindowed(WindowCacheCapacity cache) : samples_(cache), cache_(cache) {}
    // Copy even from an rvalue: the counts still name the source's samples.
    Rewindowed(const Rewindowed&) = default;
    Rewindowed& operator=(const Rewindowed&) = default;

    std::uint64_t checked(double current_length, const char* function) const {
        return checked_length(current_length, function, max_lookback_);
    }

    // One call of the site with its checked length. `sample` is what the
    // fixed-length class records for the call; `on_fixed(fixed, replace)`
    // makes the call on it, as recompute() when `replace` is true and as
    // compute() otherwise. Returns what `on_fixed` returns.
    template <class Call>
    auto call(double sample, std::uint64_t length, const Arguments& arguments, bool again,
              Call&& on_fixed) -> decltype(on_fixed(std::declval<Fixed&>(), false)) {
        const bool replace = again && called_;
        const bool records = Window::kRecordsNa || !is_na(sample);
        if (!replace && pending_recorded_) {
            // The previous bar is over: its sample joins the history.
            samples_.append(pending_, sample_limit(max_lookback_));
            pending_recorded_ = false;
        }
        called_ = true;
        // A call that does not complete may leave the object holding part of
        // it: the object is then dropped, and the next call builds anew.
        Unfinished unfinished(fixed_);
        auto result = fixed_ && fixed_length_ == length && fixed_arguments_ == arguments
                          ? on_fixed(*fixed_, replace)
                          : rebuilt(records, length, arguments, on_fixed);
        unfinished.drop = false;
        pending_ = sample;
        pending_recorded_ = records;
        return result;
    }

    void observe_owned_storage(OwnedStorage& out) const noexcept {
        samples_.observe_owned_storage(out);
        ::pineforge::observe_owned_storage(out, fixed_);
    }

private:
    // Drops the fixed-length object when a call leaves by an exception.
    struct Unfinished {
        explicit Unfinished(std::optional<Fixed>& object) : fixed(object) {}
        ~Unfinished() {
            if (drop) fixed.reset();
        }
        Unfinished(const Unfinished&) = delete;
        Unfinished& operator=(const Unfinished&) = delete;
        std::optional<Fixed>& fixed;
        bool drop = true;
    };

    // The call on a new object for `length`, built from the samples recorded
    // before the bar.
    template <class Call>
    auto rebuilt(bool records, std::uint64_t length, const Arguments& arguments,
                 Call& on_fixed) -> decltype(on_fixed(std::declval<Fixed&>(), false)) {
        fixed_.reset();
        const std::size_t held = samples_.count();
        // The window cannot be full yet: na, and nothing of that size is built.
        if (length > static_cast<std::uint64_t>(held) + (records ? 1u : 0u))
            return Window::missing();
        if (length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            throw coded<std::length_error>(RunFailureCode::scratch_capacity_exceeded,
                {{"resource", "layout"}}, "window length is not representable");
        Window::build(fixed_, static_cast<int>(length), arguments, cache_);
        fixed_length_ = length;
        fixed_arguments_ = arguments;
        std::size_t back = held < length ? held : static_cast<std::size_t>(length);
        for (; back != 0; --back) Window::feed(*fixed_, samples_.newest(back));
        return on_fixed(*fixed_, false);
    }

    RecordedSamples samples_;          // the samples recorded before the current bar
    WindowCacheCapacity cache_;
    std::optional<Fixed> fixed_;       // has taken those samples and the current bar's call
    std::uint64_t fixed_length_ = 0;
    Arguments fixed_arguments_{};
    double pending_ = na<double>();    // the current bar's sample
    bool pending_recorded_ = false;    // whether it joins samples_ when the bar is over
    bool called_ = false;
    std::int64_t max_lookback_ = ::pineforge::detail::ta_lookback_limit();
};

struct NoArguments {
    friend bool operator==(const NoArguments&, const NoArguments&) { return true; }
};

struct WmaWindow {
    using Fixed = ta::WMA;
    using Arguments = NoArguments;
    static constexpr bool kRecordsNa = false;
    static void build(std::optional<Fixed>& out, int length, const Arguments&,
                      WindowCacheCapacity cache) {
        out.emplace(length, cache);
    }
    static void feed(Fixed& fixed, double sample) { fixed.compute(sample); }
    static double missing() { return na<double>(); }
};

struct SmaWindow {
    using Fixed = ta::SMA;
    using Arguments = NoArguments;
    static constexpr bool kRecordsNa = false;
    static void build(std::optional<Fixed>& out, int length, const Arguments&,
                      WindowCacheCapacity) {
        out.emplace(length);
    }
    static void feed(Fixed& fixed, double sample) { fixed.compute(sample); }
    static double missing() { return na<double>(); }
};

struct AlmaWindow {
    using Fixed = ta::ALMA;
    struct Arguments {
        double offset = 0.0;
        double sigma = 0.0;
        bool floor = false;
        friend bool operator==(const Arguments& a, const Arguments& b) {
            return same_number(a.offset, b.offset) && same_number(a.sigma, b.sigma) &&
                   a.floor == b.floor;
        }
    };
    static constexpr bool kRecordsNa = true;
    static void build(std::optional<Fixed>& out, int length, const Arguments& arguments,
                      WindowCacheCapacity) {
        out.emplace(length, arguments.offset, arguments.sigma, arguments.floor);
    }
    static void feed(Fixed& fixed, double sample) { fixed.compute(sample); }
    static double missing() { return na<double>(); }
};

struct LinregWindow {
    using Fixed = ta::Linreg;
    using Arguments = NoArguments;
    static constexpr bool kRecordsNa = true;
    static void build(std::optional<Fixed>& out, int length, const Arguments&,
                      WindowCacheCapacity) {
        out.emplace(length);
    }
    // The offset only shifts the answer of its own call; an earlier sample's
    // answer is not read.
    static void feed(Fixed& fixed, double sample) { fixed.compute(sample, 0.0); }
    static double missing() { return na<double>(); }
};

struct BandArguments {
    double mult = 0.0;
    friend bool operator==(const BandArguments& a, const BandArguments& b) {
        return same_number(a.mult, b.mult);
    }
};

struct BbWindow {
    using Fixed = ta::BB;
    using Arguments = BandArguments;
    static constexpr bool kRecordsNa = false;
    static void build(std::optional<Fixed>& out, int length, const Arguments& arguments,
                      WindowCacheCapacity) {
        out.emplace(length, arguments.mult);
    }
    static void feed(Fixed& fixed, double sample) { fixed.compute(sample); }
    static ta::BBResult missing() {
        ta::BBResult result;
        result.middle = na<double>();
        result.upper = na<double>();
        result.lower = na<double>();
        return result;
    }
};

struct BbwWindow {
    using Fixed = ta::BBW;
    using Arguments = BandArguments;
    static constexpr bool kRecordsNa = false;
    static void build(std::optional<Fixed>& out, int length, const Arguments& arguments,
                      WindowCacheCapacity) {
        out.emplace(length, arguments.mult);
    }
    static void feed(Fixed& fixed, double sample) { fixed.compute(sample); }
    static double missing() { return na<double>(); }
};

}  // namespace series_ma_detail

// ta.wma(source, length) with a series length: ta::WMA at the call's length.
// An na source answers na and is in no window: the window is the last
// `length` sources that were not na, weighted 1 (oldest) to `length`.
class SeriesWma {
public:
    SeriesWma() : SeriesWma(window_cache_capacity()) {}
    explicit SeriesWma(WindowCacheCapacity cache) : window_(cache) {}
    double compute(double source, double current_length) {
        return call(source, current_length, false);
    }
    double recompute(double source, double current_length) {
        return call(source, current_length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        window_.observe_owned_storage(out);
    }

private:
    double call(double source, double current_length, bool again) {
        const std::uint64_t length = window_.checked(current_length, "wma");
        return window_.call(source, length, {}, again, [source](ta::WMA& fixed, bool replace) {
            return replace ? fixed.recompute(source) : fixed.compute(source);
        });
    }

    series_ma_detail::Rewindowed<series_ma_detail::WmaWindow> window_;
};

// ta.vwma(source, length) with a series length, as the Pine reference manual
// defines the call:
//     ta.sma(source * volume, length) / ta.sma(volume, length)
// with two ta::SMA windows that take their inputs separately, both at the
// call's length. A bar whose product is na keeps the product window's last
// mean, as ta::SMA does for an na input; a bar whose volume is not na enters
// the volume window whatever the source is. A zero volume mean answers na.
// This is not ta::VWMA, which keeps complete pairs and answers na on a bar
// whose source or volume is na: on bars without na the two agree up to the
// last place.
class SeriesVwma {
public:
    SeriesVwma() : SeriesVwma(window_cache_capacity()) {}
    explicit SeriesVwma(WindowCacheCapacity cache) : product_(cache), volume_(cache) {}
    double compute(double source, double volume, double current_length) {
        return call(source, volume, current_length, false);
    }
    double recompute(double source, double volume, double current_length) {
        return call(source, volume, current_length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        product_.observe_owned_storage(out);
        volume_.observe_owned_storage(out);
    }

private:
    using Mean = series_ma_detail::Rewindowed<series_ma_detail::SmaWindow>;

    static double mean(Mean& window, double sample, std::uint64_t length, bool again) {
        return window.call(sample, length, {}, again, [sample](ta::SMA& fixed, bool replace) {
            return replace ? fixed.recompute(sample) : fixed.compute(sample);
        });
    }
    double call(double source, double volume, double current_length, bool again) {
        const std::uint64_t length = product_.checked(current_length, "vwma");
        const double product_mean = mean(product_, source * volume, length, again);
        const double volume_mean = mean(volume_, volume, length, again);
        if (volume_mean == 0.0) return na<double>();
        return product_mean / volume_mean;
    }

    Mean product_;
    Mean volume_;
};

// ta.alma(series, length, offset, sigma[, floor]) with a series length:
// ta::ALMA at the call's length. Every call holds a place in the window, an
// na source included, so the answer is na while an na source is among the
// last `length` calls. offset, sigma and floor are constructor arguments of
// ta::ALMA: a call that changes one of them builds a new object, as a call
// that changes the length does.
class SeriesAlma {
public:
    SeriesAlma() : SeriesAlma(window_cache_capacity()) {}
    explicit SeriesAlma(WindowCacheCapacity cache) : window_(cache) {}
    double compute(double source, double offset, double sigma, double current_length) {
        return call(source, offset, sigma, false, current_length, false);
    }
    double compute(double source, double offset, double sigma, bool floor,
                   double current_length) {
        return call(source, offset, sigma, floor, current_length, false);
    }
    double recompute(double source, double offset, double sigma, double current_length) {
        return call(source, offset, sigma, false, current_length, true);
    }
    double recompute(double source, double offset, double sigma, bool floor,
                     double current_length) {
        return call(source, offset, sigma, floor, current_length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        window_.observe_owned_storage(out);
    }

private:
    double call(double source, double offset, double sigma, bool floor, double current_length,
                bool again) {
        const std::uint64_t length = window_.checked(current_length, "alma");
        series_ma_detail::AlmaWindow::Arguments arguments;
        arguments.offset = offset;
        arguments.sigma = sigma;
        arguments.floor = floor;
        return window_.call(source, length, arguments, again,
                            [source](ta::ALMA& fixed, bool replace) {
                                return replace ? fixed.recompute(source) : fixed.compute(source);
                            });
    }

    series_ma_detail::Rewindowed<series_ma_detail::AlmaWindow> window_;
};

// ta.linreg(source, length, offset) with a series length: ta::Linreg at the
// call's length. Every call holds a place in the window, an na source
// included, so the answer is na while an na source is among the last `length`
// calls. The offset is the call's own and is read on every call.
class SeriesLinreg {
public:
    SeriesLinreg() : SeriesLinreg(window_cache_capacity()) {}
    explicit SeriesLinreg(WindowCacheCapacity cache) : window_(cache) {}
    double compute(double source, double offset, double current_length) {
        return call(source, offset, current_length, false);
    }
    double recompute(double source, double offset, double current_length) {
        return call(source, offset, current_length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        window_.observe_owned_storage(out);
    }

private:
    double call(double source, double offset, double current_length, bool again) {
        const std::uint64_t length = window_.checked(current_length, "linreg");
        return window_.call(source, length, {}, again,
                            [source, offset](ta::Linreg& fixed, bool replace) {
                                return replace ? fixed.recompute(source, offset)
                                               : fixed.compute(source, offset);
                            });
    }

    series_ma_detail::Rewindowed<series_ma_detail::LinregWindow> window_;
};

// ta.bb(series, length, mult) with a series length: ta::BB at the call's
// length. An na source is in no window; once `length` sources that were not
// na exist, a bar with an na source answers the bands of the last `length` of
// them again, as ta::BB does. mult is a constructor argument of ta::BB: a
// call that changes it builds a new object, as a call that changes the length
// does.
class SeriesBb {
public:
    SeriesBb() : SeriesBb(window_cache_capacity()) {}
    explicit SeriesBb(WindowCacheCapacity cache) : window_(cache) {}
    ta::BBResult compute(double source, double mult, double current_length) {
        return call(source, mult, current_length, false);
    }
    ta::BBResult recompute(double source, double mult, double current_length) {
        return call(source, mult, current_length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        window_.observe_owned_storage(out);
    }

private:
    ta::BBResult call(double source, double mult, double current_length, bool again) {
        const std::uint64_t length = window_.checked(current_length, "bb");
        series_ma_detail::BandArguments arguments;
        arguments.mult = mult;
        return window_.call(source, length, arguments, again,
                            [source](ta::BB& fixed, bool replace) {
                                return replace ? fixed.recompute(source) : fixed.compute(source);
                            });
    }

    series_ma_detail::Rewindowed<series_ma_detail::BbWindow> window_;
};

// ta.bbw(series, length, mult) with a series length: ta::BBW at the call's
// length, with SeriesBb's window and its treatment of an na source and of
// mult.
class SeriesBbw {
public:
    SeriesBbw() : SeriesBbw(window_cache_capacity()) {}
    explicit SeriesBbw(WindowCacheCapacity cache) : window_(cache) {}
    double compute(double source, double mult, double current_length) {
        return call(source, mult, current_length, false);
    }
    double recompute(double source, double mult, double current_length) {
        return call(source, mult, current_length, true);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        window_.observe_owned_storage(out);
    }

private:
    double call(double source, double mult, double current_length, bool again) {
        const std::uint64_t length = window_.checked(current_length, "bbw");
        series_ma_detail::BandArguments arguments;
        arguments.mult = mult;
        return window_.call(source, length, arguments, again,
                            [source](ta::BBW& fixed, bool replace) {
                                return replace ? fixed.recompute(source) : fixed.compute(source);
                            });
    }

    series_ma_detail::Rewindowed<series_ma_detail::BbwWindow> window_;
};

// ta.vwap(source, anchor, stdev_mult) with a series stdev multiplier. The
// accumulation has no window: ta::VWAP and ta::AnchoredVWAP already take the
// multiplier on every call (compute_bands), and the bands of a bar are its
// mean plus and minus that bar's multiplier times its deviation. A multiplier
// that never changes answers ta::VWAPBands / ta::AnchoredVWAPBands bit for
// bit. The multiplier is the last argument of every form:
//
//   (source, volume, timestamp_ms, stdev_mult)                the UTC day
//   (source, volume, timestamp_ms, tz, session, stdev_mult)   the session day
//   (source, volume, anchor, stdev_mult)                      the caller's anchor
//   (source, volume, timestamp_ms, tz, session, anchor, stdev_mult)
//       the caller's anchor again; the three clock arguments are not read
//
// `anchor` is taken only as a bool and `timestamp_ms` only as another
// integer type, so the two four-argument forms never compete. One object
// serves one call site, which uses one anchoring: the day forms and the
// anchor forms keep separate sums. No argument is checked here: an na
// multiplier answers the mean and na bands, as the two classes do.
class SeriesVwapBands {
public:
    SeriesVwapBands() = default;
    explicit SeriesVwapBands(WindowCacheCapacity) {}

    template <class Time, std::enable_if_t<std::is_integral<Time>::value &&
                                               !std::is_same<Time, bool>::value, int> = 0>
    ta::VWAPBandsResult compute(double source, double volume, Time timestamp_ms,
                                double stdev_mult) {
        return day_.compute_bands(source, volume, static_cast<std::int64_t>(timestamp_ms),
                                  stdev_mult);
    }
    template <class Time, std::enable_if_t<std::is_integral<Time>::value &&
                                               !std::is_same<Time, bool>::value, int> = 0>
    ta::VWAPBandsResult recompute(double source, double volume, Time timestamp_ms,
                                  double stdev_mult) {
        return day_.recompute_bands(source, volume, static_cast<std::int64_t>(timestamp_ms),
                                    stdev_mult);
    }
    template <class Time, std::enable_if_t<std::is_integral<Time>::value &&
                                               !std::is_same<Time, bool>::value, int> = 0>
    ta::VWAPBandsResult compute(double source, double volume, Time timestamp_ms,
                                const std::string& tz, const std::string& session,
                                double stdev_mult) {
        return day_.compute_bands(source, volume, static_cast<std::int64_t>(timestamp_ms),
                                  stdev_mult, tz, session);
    }
    template <class Time, std::enable_if_t<std::is_integral<Time>::value &&
                                               !std::is_same<Time, bool>::value, int> = 0>
    ta::VWAPBandsResult recompute(double source, double volume, Time timestamp_ms,
                                  const std::string& tz, const std::string& session,
                                  double stdev_mult) {
        return day_.recompute_bands(source, volume, static_cast<std::int64_t>(timestamp_ms),
                                    stdev_mult, tz, session);
    }

    template <class Anchor, std::enable_if_t<std::is_same<Anchor, bool>::value, int> = 0>
    ta::VWAPBandsResult compute(double source, double volume, Anchor anchor,
                                double stdev_mult) {
        return anchored_.compute_bands(source, volume, anchor, stdev_mult);
    }
    template <class Anchor, std::enable_if_t<std::is_same<Anchor, bool>::value, int> = 0>
    ta::VWAPBandsResult recompute(double source, double volume, Anchor anchor,
                                  double stdev_mult) {
        return anchored_.recompute_bands(source, volume, anchor, stdev_mult);
    }
    template <class Time, std::enable_if_t<std::is_integral<Time>::value &&
                                               !std::is_same<Time, bool>::value, int> = 0>
    ta::VWAPBandsResult compute(double source, double volume, Time, const std::string&,
                                const std::string&, bool anchor, double stdev_mult) {
        return anchored_.compute_bands(source, volume, anchor, stdev_mult);
    }
    template <class Time, std::enable_if_t<std::is_integral<Time>::value &&
                                               !std::is_same<Time, bool>::value, int> = 0>
    ta::VWAPBandsResult recompute(double source, double volume, Time, const std::string&,
                                  const std::string&, bool anchor, double stdev_mult) {
        return anchored_.recompute_bands(source, volume, anchor, stdev_mult);
    }

    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, day_, anchored_);
    }

private:
    ta::VWAP day_;
    ta::AnchoredVWAP anchored_;
};

}  // namespace source
}  // namespace pineforge

// Feature macros: one per class above.
#define PINEFORGE_HAS_PINE_SERIES_WMA 1
#define PINEFORGE_HAS_PINE_SERIES_VWMA 1
#define PINEFORGE_HAS_PINE_SERIES_ALMA 1
#define PINEFORGE_HAS_PINE_SERIES_LINREG 1
#define PINEFORGE_HAS_PINE_SERIES_BB 1
#define PINEFORGE_HAS_PINE_SERIES_BBW 1
// SeriesVwapBands, under both spellings of its name in upper case.
#define PINEFORGE_HAS_PINE_SERIES_VWAPBANDS 1
#define PINEFORGE_HAS_PINE_SERIES_VWAP_BANDS 1
