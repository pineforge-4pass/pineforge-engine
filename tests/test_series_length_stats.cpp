// The series-length statistics classes of
// <pineforge/source/pine_ta_series_stats.hpp>: ta.stdev, ta.variance, ta.dev,
// ta.range, ta.median, ta.mode, ta.percentrank,
// ta.percentile_linear_interpolation, ta.correlation and math.sum with a
// length taken on each call.
//
// For each class, on an invented tape with na bars:
//   1. a length that stays the same gives the fixed-length class's values,
//      bit for bit, through compute() and recompute();
//   2. a length that changes on every call gives the function computed
//      directly over the window the class documents;
//   3. recompute() gives what compute() gives from the state the call found;
//   4. an invalid length stops the run with SeriesSma's text.
// The first three also run under a lookback cap, where a site keeps a
// bounded ring of its inputs.
#include <pineforge/source/pine_ta_series_stats.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/detail/ta_lookback.hpp>
#include <pineforge/math.hpp>
#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace pf = pineforge;

constexpr int kBars = 48;

// One bar of the invented tape: two sources and a percentage.
struct Row {
    double a;
    double b;
    double pct;
};

double missing() { return pf::na<double>(); }
bool is_missing(double value) { return pf::is_na(value); }

// Every value is a small multiple of 0.25, so window sums are exact.
Row tape(int bar) {
    Row row;
    row.a = 12.0 + 0.25 * static_cast<double>((bar * 7 + bar / 3) % 5);
    row.b = 3.0 + 0.5 * static_cast<double>((bar * 3 + bar / 5) % 7);
    row.pct = 10.0 + static_cast<double>((bar * 13) % 80);
    if (bar == 9 || bar == 21 || bar == 30) row.a = missing();
    if (bar == 14) row.b = missing();
    return row;
}

Row tape_without_na(int bar) {
    Row row = tape(bar);
    if (is_missing(row.a)) row.a = 11.5;
    if (is_missing(row.b)) row.b = 2.5;
    return row;
}

// What a bar carries before its final values: other numbers, na on some bars.
Row provisional(int bar) {
    Row row;
    row.a = bar % 8 == 2 ? missing() : 14.5 - 0.25 * static_cast<double>(bar % 6);
    row.b = 6.0 - 0.5 * static_cast<double>(bar % 5);
    row.pct = 90.0 - static_cast<double>(bar % 7);
    return row;
}

// 1, 2 or 3 on even bars and 4 or 5 on odd bars: no two calls in a row share
// a length.
int changing_length(int bar) {
    return bar % 2 == 0 ? 1 + (bar / 2) % 3 : 4 + (bar / 2) % 2;
}

// Another length in 1..5.
int other_length(int length) { return length % 5 + 1; }

std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

[[noreturn]] void fail(const char* subject, const char* check, int bar, double length,
                       double actual, double expected) {
    std::fprintf(stderr, "FAIL: %s: %s at bar %d, length %g: got %.17g, expected %.17g\n",
                 subject, check, bar, length, actual, expected);
    std::exit(1);
}

void require_same(const char* subject, const char* check, int bar, double length,
                  double actual, double expected) {
    const bool same = is_missing(expected)
        ? is_missing(actual)
        : (!is_missing(actual) && bits(actual) == bits(expected));
    if (!same) fail(subject, check, bar, length, actual, expected);
}

void require_close(const char* subject, const char* check, int bar, double length,
                   double actual, double expected) {
    bool matches;
    if (is_missing(actual) || is_missing(expected)) {
        matches = is_missing(actual) && is_missing(expected);
    } else {
        matches = std::fabs(actual - expected) <= 1e-9 * std::max(1.0, std::fabs(expected));
    }
    if (!matches) fail(subject, check, bar, length, actual, expected);
}

void require(bool condition, const char* subject, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s: %s\n", subject, message);
        std::exit(1);
    }
}

// --- the functions computed directly over their window -------------------

// The last `length` non-na first sources, oldest first; empty when the tape
// holds fewer.
std::vector<double> last_valid(const std::vector<Row>& seen, std::size_t length) {
    std::vector<double> window;
    for (std::size_t i = seen.size(); i != 0 && window.size() != length; --i) {
        if (!is_missing(seen[i - 1].a)) window.push_back(seen[i - 1].a);
    }
    if (window.size() != length) return {};
    std::reverse(window.begin(), window.end());
    return window;
}

double mean_of(const std::vector<double>& window) {
    double sum = 0.0;
    for (double value : window) sum += value;
    return sum / static_cast<double>(window.size());
}

// math.sum: the last `length` non-na sources; an na bar holds the sum.
double sum_model(const std::vector<Row>& seen, std::size_t length) {
    const std::vector<double> window = last_valid(seen, length);
    if (window.empty()) return missing();
    double sum = 0.0;
    for (double value : window) sum += value;
    return sum;
}

// ta.stdev: the last `length` non-na sources; an na bar holds the value.
double stdev_model(const std::vector<Row>& seen, std::size_t length, bool biased) {
    const std::vector<double> window = last_valid(seen, length);
    if (window.empty()) return missing();
    const double mean = mean_of(window);
    double squares = 0.0;
    for (double value : window) squares += (value - mean) * (value - mean);
    const double denominator = static_cast<double>(biased ? length : length - 1);
    return denominator > 0.0 ? std::sqrt(squares / denominator) : missing();
}

// ta.variance: the last `length` non-na sources; an na bar answers na.
double variance_model(const std::vector<Row>& seen, std::size_t length, bool biased) {
    if (is_missing(seen.back().a)) return missing();
    const std::vector<double> window = last_valid(seen, length);
    if (window.empty()) return missing();
    const double mean = mean_of(window);
    double squares = 0.0;
    for (double value : window) squares += (value - mean) * (value - mean);
    const double denominator = static_cast<double>(biased ? length : length - 1);
    return denominator > 0.0 ? squares / denominator : missing();
}

// ta.dev: the mean absolute deviation of the last `length` non-na sources.
double dev_model(const std::vector<Row>& seen, std::size_t length) {
    if (is_missing(seen.back().a)) return missing();
    const std::vector<double> window = last_valid(seen, length);
    if (window.empty()) return missing();
    const double mean = mean_of(window);
    double deviation = 0.0;
    for (double value : window) deviation += std::fabs(value - mean);
    return deviation / static_cast<double>(length);
}

double median_model(const std::vector<Row>& seen, std::size_t length) {
    if (is_missing(seen.back().a)) return missing();
    std::vector<double> window = last_valid(seen, length);
    if (window.empty()) return missing();
    std::sort(window.begin(), window.end());
    if (length % 2 == 1) return window[length / 2];
    return (window[length / 2 - 1] + window[length / 2]) / 2.0;
}

// ta.mode: the most frequent value, the smallest of several.
double mode_model(const std::vector<Row>& seen, std::size_t length) {
    if (is_missing(seen.back().a)) return missing();
    const std::vector<double> window = last_valid(seen, length);
    if (window.empty()) return missing();
    double best = window.front();
    std::ptrdiff_t best_count = 0;
    for (double value : window) {
        const std::ptrdiff_t count = std::count(window.begin(), window.end(), value);
        if (count > best_count || (count == best_count && value < best)) {
            best = value;
            best_count = count;
        }
    }
    return best;
}

// ta.range over the last `length` bars of a tape without na.
double range_model(const std::vector<Row>& seen, std::size_t length) {
    if (seen.size() < length) return missing();
    double high = seen.back().a;
    double low = seen.back().a;
    for (std::size_t i = seen.size() - length; i != seen.size(); ++i) {
        high = std::max(high, seen[i].a);
        low = std::min(low, seen[i].a);
    }
    return high - low;
}

// ta.percentrank: the share of the `length` earlier sources that are at most
// the current one, over `length` (an na value counts for nothing).
double percentrank_model(const std::vector<Row>& seen, std::size_t length) {
    if (seen.size() < length + 1) return missing();
    const double current = seen.back().a;
    int count = 0;
    for (std::size_t i = seen.size() - 1 - length; i != seen.size() - 1; ++i) {
        if (seen[i].a <= current) ++count;
    }
    return static_cast<double>(count) / static_cast<double>(length) * 100.0;
}

// ta.percentile_linear_interpolation over the last `length` calls, ranked in
// call order: each value enters before the first held value greater than it.
double percentile_model(const std::vector<Row>& seen, std::size_t length) {
    if (seen.size() < length) return missing();
    std::vector<double> ranked;
    for (std::size_t i = seen.size() - length; i != seen.size(); ++i) {
        const double value = seen[i].a;
        const auto at = std::find_if(ranked.begin(), ranked.end(),
                                     [&](double held) { return held > value; });
        ranked.insert(at, value);
    }
    const double n = static_cast<double>(length);
    const double k = std::min(std::max(seen.back().pct / 100.0 * n - 0.5, 0.0), n - 1.0);
    const std::size_t lower = static_cast<std::size_t>(std::floor(k));
    if (lower + 1 >= length) return ranked[length - 1];
    const double fraction = k - static_cast<double>(lower);
    return ranked[lower] * (1.0 - fraction) + ranked[lower + 1] * fraction;
}

// ta.correlation over the last `length` pairs, na pairs included.
double correlation_model(const std::vector<Row>& seen, std::size_t length) {
    if (seen.size() < length) return missing();
    double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (std::size_t i = seen.size() - length; i != seen.size(); ++i) {
        const double x = seen[i].a;
        const double y = seen[i].b;
        sx += x;
        sy += y;
        sxx += x * x;
        syy += y * y;
        sxy += x * y;
    }
    const double n = static_cast<double>(length);
    const double denominator = std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
    if (denominator == 0.0) return 0.0;
    return (n * sxy - sx * sy) / denominator;
}

// --- the checks every class gets ------------------------------------------

// make_fixed(length) builds the fixed-length class; series_call(series, row,
// length, again) and fixed_call(fixed, row, again) make one call (again:
// recompute); model(seen, length) is the function over the tape so far.
template <class Series, class MakeFixed, class SeriesCall, class FixedCall, class Model>
void check(const char* subject, MakeFixed make_fixed, SeriesCall series_call,
           FixedCall fixed_call, Model model, bool model_tape_without_na) {
    // 1. A length that stays the same: the fixed-length class, bit for bit.
    for (int length : {1, 2, 5}) {
        Series series;
        auto fixed = make_fixed(length);
        for (int bar = 0; bar != kBars; ++bar) {
            const Row row = tape(bar);
            if (bar % 4 == 2) {
                const Row first = provisional(bar);
                const double series_first = series_call(series, first, length, false);
                const double fixed_first = fixed_call(fixed, first, false);
                require_same(subject, "constant length, provisional values", bar, length,
                             series_first, fixed_first);
                const double series_final = series_call(series, row, length, true);
                const double fixed_final = fixed_call(fixed, row, true);
                require_same(subject, "constant length, recomputed", bar, length,
                             series_final, fixed_final);
            } else {
                const double series_value = series_call(series, row, length, false);
                const double fixed_value = fixed_call(fixed, row, false);
                require_same(subject, "constant length", bar, length, series_value,
                             fixed_value);
            }
        }
    }

    // 2. A length that changes on every call: the function over the window.
    {
        Series series;
        std::vector<Row> seen;
        for (int bar = 0; bar != kBars; ++bar) {
            const Row row = model_tape_without_na ? tape_without_na(bar) : tape(bar);
            seen.push_back(row);
            const int length = changing_length(bar);
            const double actual = series_call(series, row, length, false);
            const double expected = model(seen, static_cast<std::size_t>(length));
            require_close(subject, "changing length", bar, length, actual, expected);
        }
        pf::OwnedStorage storage;
        series.observe_owned_storage(storage);
        require(storage.usage().logical_payload_bytes > 0, subject,
                "the retained inputs are reported as owned storage");
    }

    // 3. recompute() runs the call again from the state the call found, with
    //    the same length (even bars) or another one (odd bars).
    {
        Series once;
        Series twice;
        for (int bar = 0; bar != kBars; ++bar) {
            const Row row = tape(bar);
            const int length = changing_length(bar);
            const double expected = series_call(once, row, length, false);
            series_call(twice, provisional(bar),
                        bar % 2 == 0 ? length : other_length(length), false);
            const double actual = series_call(twice, row, length, true);
            require_close(subject, "recompute", bar, length, actual, expected);
        }
    }

    // A copy continues on its own from the copied state.
    {
        Series original;
        for (int bar = 0; bar != 12; ++bar) series_call(original, tape(bar), 3, false);
        Series copy(original);
        for (int bar = 12; bar != 24; ++bar) {
            const Row row = tape(bar);
            const int length = changing_length(bar);
            const double from_copy = series_call(copy, row, length, false);
            const double from_original = series_call(original, row, length, false);
            require_same(subject, "copy", bar, length, from_copy, from_original);
        }
    }
}

template <class Call>
std::string stop_text(Call call) {
    try {
        call();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return std::string();
}

// SeriesSma's text for `length`, with `function` in place of 'sma'.
std::string sma_stop_text(double length, const char* function) {
    std::string text = stop_text([length] {
        pf::source::SeriesSma sma;
        sma.compute(1.0, length);
    });
    const std::string from = "'sma'";
    const std::size_t at = text.find(from);
    if (at != std::string::npos) {
        text.replace(at, from.size(), "'" + std::string(function) + "'");
    }
    return text;
}

void require_text(const char* subject, const char* check, double length,
                  const std::string& actual, const std::string& expected) {
    if (expected.empty() || actual != expected) {
        std::fprintf(stderr, "FAIL: %s: %s, length %g: got \"%s\", expected \"%s\"\n", subject,
                     check, length, actual.c_str(), expected.c_str());
        std::exit(1);
    }
}

// 4. The length is checked as SeriesSma checks it, with the function's name.
template <class Series, class SeriesCall>
void check_length(const char* subject, const char* function, SeriesCall series_call) {
    const double invalid[] = {0.0, -3.0, 0.5, missing(),
                              std::numeric_limits<double>::infinity()};
    for (double length : invalid) {
        Series series;
        const std::string actual =
            stop_text([&] { series_call(series, tape(0), length, false); });
        require_text(subject, "invalid length", length, actual,
                     sma_stop_text(length, function));
    }
    {
        // A cap of 3 bars back admits a length of 4 and refuses 6.
        pf::detail::TaLookbackScope cap(3);
        Series series;
        const std::string actual = stop_text([&] { series_call(series, tape(0), 6.0, false); });
        require_text(subject, "length beyond the lookback cap", 6.0, actual,
                     sma_stop_text(6.0, function));
        const std::string accepted =
            stop_text([&] { series_call(series, tape(0), 4.0, false); });
        require(accepted.empty(), subject, "a length within the lookback cap is accepted");
    }
    {
        // A window far longer than the inputs held answers na and builds nothing.
        Series series;
        for (double length : {1e18, 1e30}) {
            const double value = series_call(series, tape(0), length, false);
            require(is_missing(value), subject, "a very long window answers na");
        }
        const double value = series_call(series, tape(1), 1.0, false);
        require(!is_missing(value), subject, "a length of one answers on the next call");
    }
}

template <class Series>
double one_source(Series& series, const Row& row, double length, bool again) {
    return again ? series.recompute(row.a, length) : series.compute(row.a, length);
}

template <class Fixed>
double one_source_fixed(Fixed& fixed, const Row& row, bool again) {
    return again ? fixed.recompute(row.a) : fixed.compute(row.a);
}

void check_classes() {
    using pf::source::SeriesCorrelation;
    using pf::source::SeriesDev;
    using pf::source::SeriesMathSum;
    using pf::source::SeriesMedian;
    using pf::source::SeriesMode;
    using pf::source::SeriesPercentileLinear;
    using pf::source::SeriesPercentrank;
    using pf::source::SeriesRange;
    using pf::source::SeriesStdev;
    using pf::source::SeriesVariance;

    check<SeriesMathSum>(
        "math.sum", [](int length) { return pf::math::Sum(length); },
        [](SeriesMathSum& series, const Row& row, double length, bool again) {
            return one_source(series, row, length, again);
        },
        [](pf::math::Sum& fixed, const Row& row, bool again) {
            return one_source_fixed(fixed, row, again);
        },
        sum_model, false);

    for (bool biased : {true, false}) {
        check<SeriesStdev>(
            biased ? "ta.stdev" : "ta.stdev, unbiased",
            [biased](int length) { return pf::ta::StdDev(length, biased); },
            [biased](SeriesStdev& series, const Row& row, double length, bool again) {
                return again ? series.recompute(row.a, biased, length)
                             : series.compute(row.a, biased, length);
            },
            [](pf::ta::StdDev& fixed, const Row& row, bool again) {
                return one_source_fixed(fixed, row, again);
            },
            [biased](const std::vector<Row>& seen, std::size_t length) {
                return stdev_model(seen, length, biased);
            },
            false);
        check<SeriesVariance>(
            biased ? "ta.variance" : "ta.variance, unbiased",
            [biased](int length) { return pf::ta::Variance(length, biased); },
            [biased](SeriesVariance& series, const Row& row, double length, bool again) {
                return again ? series.recompute(row.a, biased, length)
                             : series.compute(row.a, biased, length);
            },
            [](pf::ta::Variance& fixed, const Row& row, bool again) {
                return one_source_fixed(fixed, row, again);
            },
            [biased](const std::vector<Row>& seen, std::size_t length) {
                return variance_model(seen, length, biased);
            },
            false);
    }

    check<SeriesDev>(
        "ta.dev", [](int length) { return pf::ta::Dev(length); },
        [](SeriesDev& series, const Row& row, double length, bool again) {
            return one_source(series, row, length, again);
        },
        [](pf::ta::Dev& fixed, const Row& row, bool again) {
            return one_source_fixed(fixed, row, again);
        },
        dev_model, false);

    check<SeriesMedian>(
        "ta.median", [](int length) { return pf::ta::Median(length); },
        [](SeriesMedian& series, const Row& row, double length, bool again) {
            return one_source(series, row, length, again);
        },
        [](pf::ta::Median& fixed, const Row& row, bool again) {
            return one_source_fixed(fixed, row, again);
        },
        median_model, false);

    check<SeriesMode>(
        "ta.mode", [](int length) { return pf::ta::Mode(length); },
        [](SeriesMode& series, const Row& row, double length, bool again) {
            return one_source(series, row, length, again);
        },
        [](pf::ta::Mode& fixed, const Row& row, bool again) {
            return one_source_fixed(fixed, row, again);
        },
        mode_model, false);

    // The direct range is over a tape without na: with na in the window the
    // series-length highest and lowest follow their own documented rule, which
    // check_range_by_hand holds ta.range to on values written by hand.
    check<SeriesRange>(
        "ta.range", [](int length) { return pf::ta::Range(length); },
        [](SeriesRange& series, const Row& row, double length, bool again) {
            return one_source(series, row, length, again);
        },
        [](pf::ta::Range& fixed, const Row& row, bool again) {
            return one_source_fixed(fixed, row, again);
        },
        range_model, true);

    check<SeriesPercentrank>(
        "ta.percentrank", [](int length) { return pf::ta::PercentRank(length); },
        [](SeriesPercentrank& series, const Row& row, double length, bool again) {
            return one_source(series, row, length, again);
        },
        [](pf::ta::PercentRank& fixed, const Row& row, bool again) {
            return one_source_fixed(fixed, row, again);
        },
        percentrank_model, false);

    check<SeriesPercentileLinear>(
        "ta.percentile_linear_interpolation",
        [](int length) { return pf::ta::PercentileLinearInterpolation(length); },
        [](SeriesPercentileLinear& series, const Row& row, double length, bool again) {
            return again ? series.recompute(row.a, row.pct, length)
                         : series.compute(row.a, row.pct, length);
        },
        [](pf::ta::PercentileLinearInterpolation& fixed, const Row& row, bool again) {
            return again ? fixed.recompute(row.a, row.pct) : fixed.compute(row.a, row.pct);
        },
        percentile_model, false);

    check<SeriesCorrelation>(
        "ta.correlation", [](int length) { return pf::ta::Correlation(length); },
        [](SeriesCorrelation& series, const Row& row, double length, bool again) {
            return again ? series.recompute(row.a, row.b, length)
                         : series.compute(row.a, row.b, length);
        },
        [](pf::ta::Correlation& fixed, const Row& row, bool again) {
            return again ? fixed.recompute(row.a, row.b) : fixed.compute(row.a, row.b);
        },
        correlation_model, false);
}

void check_lengths() {
    using pf::source::SeriesCorrelation;
    using pf::source::SeriesDev;
    using pf::source::SeriesMathSum;
    using pf::source::SeriesMedian;
    using pf::source::SeriesMode;
    using pf::source::SeriesPercentileLinear;
    using pf::source::SeriesPercentrank;
    using pf::source::SeriesRange;
    using pf::source::SeriesStdev;
    using pf::source::SeriesVariance;

    check_length<SeriesMathSum>("math.sum", "sum", one_source<SeriesMathSum>);
    check_length<SeriesStdev>("ta.stdev", "stdev", one_source<SeriesStdev>);
    check_length<SeriesVariance>("ta.variance", "variance", one_source<SeriesVariance>);
    check_length<SeriesDev>("ta.dev", "dev", one_source<SeriesDev>);
    check_length<SeriesMedian>("ta.median", "median", one_source<SeriesMedian>);
    check_length<SeriesMode>("ta.mode", "mode", one_source<SeriesMode>);
    check_length<SeriesRange>("ta.range", "range", one_source<SeriesRange>);
    check_length<SeriesPercentrank>("ta.percentrank", "percentrank",
                                    one_source<SeriesPercentrank>);
    check_length<SeriesPercentileLinear>(
        "ta.percentile_linear_interpolation", "percentile_linear_interpolation",
        [](SeriesPercentileLinear& series, const Row& row, double length, bool again) {
            return again ? series.recompute(row.a, row.pct, length)
                         : series.compute(row.a, row.pct, length);
        });
    check_length<SeriesCorrelation>(
        "ta.correlation", "correlation",
        [](SeriesCorrelation& series, const Row& row, double length, bool again) {
            return again ? series.recompute(row.a, row.b, length)
                         : series.compute(row.a, row.b, length);
        });
}

// The two-argument forms of ta.stdev and ta.variance are biased = true, and
// ta.range is the series-length highest minus the series-length lowest on a
// tape with na bars and a changing length.
void check_forms() {
    pf::source::SeriesStdev stdev_short;
    pf::source::SeriesStdev stdev_long;
    pf::source::SeriesVariance variance_short;
    pf::source::SeriesVariance variance_long;
    pf::source::SeriesRange range;
    pf::source::SeriesHighest highest;
    pf::source::SeriesLowest lowest;
    for (int bar = 0; bar != kBars; ++bar) {
        const double source = tape(bar).a;
        const int length = changing_length(bar);
        require_same("ta.stdev", "two-argument form", bar, length,
                     stdev_short.compute(source, length),
                     stdev_long.compute(source, true, length));
        require_same("ta.variance", "two-argument form", bar, length,
                     variance_short.compute(source, length),
                     variance_long.compute(source, true, length));
        const double high = highest.compute(source, length);
        const double low = lowest.compute(source, length);
        const double expected =
            is_missing(high) || is_missing(low) ? missing() : high - low;
        require_same("ta.range", "highest minus lowest", bar, length,
                     range.compute(source, length), expected);
    }
}

// ta.range with a length that changes on a source with na bars, against
// values written by hand. No class computed the three columns below: each was
// worked out on paper from the rule documented at SeriesWindowExtreme
// (pine_ta_length.hpp), which ta.range reads its highest and its lowest
// through. The extremum is kept as a cached {value, bar}:
//
//   * a call answers na while fewer than `length` bars exist, and on an na
//     source, which also drops the cache;
//   * a call whose length is longer than the call before first reads the
//     bars the longer window adds, oldest first, into the cache: a strictly
//     better value takes it, an na drops it, and the next value read starts
//     it again;
//   * then the source: without a cache the call answers its own source and
//     keeps nothing; a strictly better source takes the cache; a cache
//     `length` or more bars old is replaced by a read of the whole window,
//     oldest first, in which an na starts the extremum again;
//   * a call whose length is not longer and that has no cache reads the whole
//     window the same way.
//
//   bar source length  highest lowest  range
//    0   10.0    2       na      na     na    fewer than two bars
//    1   12.0    2      12.0    10.0    2.0
//    2   11.0    3      12.0    10.0    2.0   bar 0 is added: no better value
//    3    na     2       na      na     na    an na source
//    4    9.0    1       9.0     9.0    0.0   no cache: the window is bar 4
//    5    9.5    4       9.5     9.0    0.5   bars 2, 3, 4 are added: 11.0, the
//                                             na drops it, 9.0 starts again;
//                                             9.5 is the higher. The 11.0 of
//                                             bar 2, inside the window, does
//                                             not outlive the na
//    6   13.0    2      13.0     9.5    3.5   the lowest's 9.0 is two bars
//                                             old: bars 5 and 6 are read
//    7    na     3       na      na     na    an na source
//    8    8.0    2       8.0     8.0    0.0   no cache: bars 7 and 8, the na
//                                             first
//    9    8.5    4       8.5     8.5    0.0   bars 6 and 7 are added: 13.0,
//                                             then the na drops the cache, so
//                                             both sides answer the source
//   10    7.5    4       8.5     7.5    1.0   no cache: bars 7 to 10 are read,
//                                             the na first
//   11    9.0    5       9.0     9.0    0.0   bar 7 is added: the na drops
//                                             the cache, both sides answer
//                                             the source
//   12    6.0    5       9.0     6.0    3.0   no cache: bars 8 to 12 are read
//   13    7.0    3       9.0     6.0    3.0   bar 11's 9.0 and bar 12's 6.0
//                                             are still inside three bars
//
// A window that skipped its na members would answer 0.0, 2.0, 3.5, 5.0 and
// 1.5 on bars 3, 5, 7, 9 and 11.
void check_range_by_hand() {
    struct HandRow {
        double source;
        int length;
        double highest;
        double lowest;
        double range;
    };
    const double none = missing();
    const HandRow rows[] = {
        {10.0, 2, none, none, none}, {12.0, 2, 12.0, 10.0, 2.0}, {11.0, 3, 12.0, 10.0, 2.0},
        {none, 2, none, none, none}, {9.0, 1, 9.0, 9.0, 0.0},    {9.5, 4, 9.5, 9.0, 0.5},
        {13.0, 2, 13.0, 9.5, 3.5},   {none, 3, none, none, none}, {8.0, 2, 8.0, 8.0, 0.0},
        {8.5, 4, 8.5, 8.5, 0.0},     {7.5, 4, 8.5, 7.5, 1.0},    {9.0, 5, 9.0, 9.0, 0.0},
        {6.0, 5, 9.0, 6.0, 3.0},     {7.0, 3, 9.0, 6.0, 3.0},
    };
    const int count = static_cast<int>(sizeof rows / sizeof rows[0]);
    pf::source::SeriesRange range;
    int decided = 0;
    for (int bar = 0; bar != count; ++bar) {
        const HandRow& row = rows[bar];
        // The table agrees with itself: the range column is the highest
        // minus the lowest, na where either is.
        require_same("ta.range", "the hand-written columns", bar, row.length, row.range,
                     is_missing(row.highest) || is_missing(row.lowest)
                         ? missing()
                         : row.highest - row.lowest);
        require_same("ta.range", "written by hand", bar, row.length,
                     range.compute(row.source, row.length), row.range);
        // The range of the values that are not na among the last `length`
        // bars: what a window that skipped its na members would answer.
        double skipping = missing();
        if (bar + 1 >= row.length) {
            double high = missing();
            double low = missing();
            for (int i = bar + 1 - row.length; i <= bar; ++i) {
                const double value = rows[i].source;
                if (is_missing(value)) continue;
                if (is_missing(high) || value > high) high = value;
                if (is_missing(low) || value < low) low = value;
            }
            if (!is_missing(high)) skipping = high - low;
        }
        const bool same = is_missing(row.range) ? is_missing(skipping)
                                                : (!is_missing(skipping) &&
                                                   bits(skipping) == bits(row.range));
        if (!same) ++decided;
    }
    if (decided < 5) {
        std::fprintf(stderr,
                     "FAIL: ta.range: written by hand NOT COMPARED: the rows differ from the "
                     "na-skipping range on %d bars, at least 5 expected\n",
                     decided);
        std::exit(1);
    }
}

// A site that has recorded more inputs than one storage page holds: the
// retained inputs grow twice, the constant-length values stay the
// fixed-length class's, and a long window reads back across the pages.
void check_long_history() {
    const int bars = 3000;
    const int long_window = 2500;
    pf::source::SeriesMathSum series;
    pf::math::Sum fixed(3);
    std::vector<Row> seen;
    for (int bar = 0; bar != bars; ++bar) {
        const Row row = tape(bar % kBars);
        seen.push_back(row);
        const double series_value = series.compute(row.a, 3);
        const double fixed_value = fixed.compute(row.a);
        require_same("math.sum", "long history, constant length", bar, 3, series_value,
                     fixed_value);
    }
    const Row row = tape(7);
    seen.push_back(row);
    const double actual = series.compute(row.a, long_window);
    const double expected = sum_model(seen, static_cast<std::size_t>(long_window));
    require(!is_missing(expected), "math.sum", "the long window is full on the long tape");
    require_close("math.sum", "long history, long window", bars, long_window, actual, expected);
}

}  // namespace

int main() {
#if !defined(PINEFORGE_HAS_PINE_SERIES_STDEV) || !defined(PINEFORGE_HAS_PINE_SERIES_VARIANCE) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_DEV) || !defined(PINEFORGE_HAS_PINE_SERIES_RANGE) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_MEDIAN) || !defined(PINEFORGE_HAS_PINE_SERIES_MODE) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_PERCENTRANK) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_PERCENTILELINEAR) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_CORRELATION) || !defined(PINEFORGE_HAS_PINE_SERIES_MATHSUM)
#error "pine_ta_series_stats.hpp must define one feature macro per class"
#endif
    check_classes();
    check_lengths();
    check_forms();
    check_range_by_hand();
    check_long_history();
    {
        // The same checks with a lookback cap of 4 bars: lengths up to 5 are
        // admitted and each site keeps a ring of its last 6 inputs.
        pf::detail::TaLookbackScope cap(4);
        check_classes();
        check_forms();
    }
    std::printf("PASS: series-length statistics classes\n");
    return 0;
}
