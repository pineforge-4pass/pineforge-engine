// Series-length moving averages and bands (pine_ta_series_ma.hpp): every
// class answers the fixed-length window of the call's own length.
//
// Two checks per class on one invented tape (two bars without a source, one
// without a volume):
//   1. a length that stays the same gives the fixed-length path's values bit
//      for bit on every bar, replaced calls of a bar included;
//   2. a length that changes on every bar (2 + bar % 3) matches the window
//      written out here over the last `length` samples, and the fixed-length
//      class of that length run over the whole tape so far.
// Then the length stops, the history kept under a lookback limit, copies and
// a history longer than its first storage.
#include <pineforge/source/pine_ta_series_ma.hpp>

#include <pineforge/detail/ta_lookback.hpp>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#if !defined(PINEFORGE_HAS_PINE_SERIES_WMA) || !defined(PINEFORGE_HAS_PINE_SERIES_VWMA) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_ALMA) || !defined(PINEFORGE_HAS_PINE_SERIES_LINREG) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_BB) || !defined(PINEFORGE_HAS_PINE_SERIES_BBW) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_VWAPBANDS) || !defined(PINEFORGE_HAS_PINE_SERIES_VWAP_BANDS)
#error "pine_ta_series_ma.hpp must define one feature macro per class"
#endif

namespace {

using pineforge::na;
namespace ta = pineforge::ta;
using pineforge::source::SeriesAlma;
using pineforge::source::SeriesBb;
using pineforge::source::SeriesBbw;
using pineforge::source::SeriesLinreg;
using pineforge::source::SeriesSma;
using pineforge::source::SeriesVwapBands;
using pineforge::source::SeriesVwma;
using pineforge::source::SeriesWma;

using Values = std::vector<double>;

int failures = 0;

bool both_na(double a, double b) { return std::isnan(a) && std::isnan(b); }

void report(const char* what, int bar, std::size_t field, double got, double expected) {
    std::printf("FAIL %s bar=%d field=%zu got=%.17g expected=%.17g\n", what, bar, field, got,
                expected);
    ++failures;
}

bool same_count(const char* what, int bar, const Values& got, const Values& expected) {
    if (got.size() == expected.size()) return true;
    std::printf("FAIL %s bar=%d NOT COMPARED: %zu values against %zu\n", what, bar, got.size(),
                expected.size());
    ++failures;
    return false;
}

// Bit for bit, na matching na. Returns how many expected values are not na.
int same_bits(const char* what, int bar, const Values& got, const Values& expected) {
    if (!same_count(what, bar, got, expected)) return 0;
    int present = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!std::isnan(expected[i])) ++present;
        if (both_na(got[i], expected[i])) continue;
        if (std::memcmp(&got[i], &expected[i], sizeof(double)) != 0)
            report(what, bar, i, got[i], expected[i]);
    }
    return present;
}

// Independent arithmetic: equal within one part in 1e9, na matching na.
int close_to(const char* what, int bar, const Values& got, const Values& expected) {
    if (!same_count(what, bar, got, expected)) return 0;
    int present = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!std::isnan(expected[i])) ++present;
        if (both_na(got[i], expected[i])) continue;
        const bool close = !std::isnan(got[i]) && !std::isnan(expected[i]) &&
                           std::fabs(got[i] - expected[i]) <=
                               1e-9 * std::max(1.0, std::fabs(expected[i]));
        if (!close) report(what, bar, i, got[i], expected[i]);
    }
    return present;
}

// A check that compared no value, or too few, is not a pass.
void require_compared(const char* what, const char* check, int present, int minimum) {
    if (present >= minimum) return;
    std::printf("FAIL %s %s NOT COMPARED: %d values present, at least %d expected\n", what, check,
                present, minimum);
    ++failures;
}

void require(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL %s\n", what);
    ++failures;
}

struct Tape {
    std::vector<double> source;
    std::vector<double> volume;
    int bars() const { return static_cast<int>(source.size()); }
};

// Invented values. Bars 3 and 13 have no source (their volume is present);
// bar 5 has no volume.
Tape invented_tape() {
    const double missing = na<double>();
    Tape tape;
    tape.source = {12.5,  14.25, 13.0,  missing, 15.5,  16.75, 11.25, 18.0,
                   17.5,  19.25, 16.0,  21.0,    20.5,  missing, 19.0, 23.5,
                   22.25, 24.0,  21.75, 25.5,    23.0,  26.25, 24.5,  27.0};
    tape.volume = {100.0, 250.0, 175.0, 300.0, 125.0, missing, 400.0, 150.0,
                   225.0, 350.0, 275.0, 200.0, 325.0, 180.0,   260.0, 310.0,
                   140.0, 290.0, 210.0, 330.0, 160.0, 240.0,   380.0, 270.0};
    return tape;
}

int length_at(int bar) { return 2 + bar % 3; }

// A bar whose call is first made with another value and then replaced.
bool replaced_bar(const Tape& tape, int bar) {
    return bar % 4 == 1 || std::isnan(tape.source[bar]);
}

// The value of that first call: present where the bar's own source is
// missing, missing on bars 1, 9 and 17, another number elsewhere.
double earlier_value(const Tape& tape, int bar) {
    if (std::isnan(tape.source[bar])) return 3.0;
    return bar % 8 == 1 ? na<double>() : tape.source[bar] + 1.5;
}

// The last `count` values of values[0..bar] that are not na, oldest first;
// empty when fewer exist.
std::vector<double> last_present(const std::vector<double>& values, int bar, int count) {
    std::vector<double> window;
    for (int i = bar; i >= 0 && static_cast<int>(window.size()) < count; --i) {
        if (!std::isnan(values[static_cast<std::size_t>(i)]))
            window.push_back(values[static_cast<std::size_t>(i)]);
    }
    if (static_cast<int>(window.size()) < count) return {};
    std::reverse(window.begin(), window.end());
    return window;
}

// values[bar - count + 1 .. bar], na included; empty when fewer bars exist.
std::vector<double> last_bars(const std::vector<double>& values, int bar, int count) {
    if (bar + 1 < count) return {};
    return std::vector<double>(values.begin() + (bar - count + 1), values.begin() + bar + 1);
}

using SeriesStep = std::function<Values(int bar, double source, double length, bool again)>;
using FixedStep = std::function<Values(int bar, double source, bool again)>;

struct Subject {
    const char* name = "";
    // The fixed-length class sums its window on every call, so an object of
    // one length run over the whole tape equals the series answer bit for bit.
    bool sums_each_call = false;
    std::function<SeriesStep()> series;                 // a new call site
    std::function<FixedStep(int length)> fixed;         // the fixed-length path of one length
    std::function<Values(int bar, int length)> direct;  // the window written out
};

Subject wma_subject(const Tape& tape) {
    Subject subject;
    subject.name = "ta.wma";
    subject.sums_each_call = true;
    subject.series = [] {
        auto site = std::make_shared<SeriesWma>();
        return SeriesStep([site](int, double source, double length, bool again) {
            return Values{again ? site->recompute(source, length) : site->compute(source, length)};
        });
    };
    subject.fixed = [](int length) {
        auto fixed = std::make_shared<ta::WMA>(length);
        return FixedStep([fixed](int, double source, bool again) {
            return Values{again ? fixed->recompute(source) : fixed->compute(source)};
        });
    };
    subject.direct = [&tape](int bar, int length) {
        if (std::isnan(tape.source[static_cast<std::size_t>(bar)])) return Values{na<double>()};
        const std::vector<double> window = last_present(tape.source, bar, length);
        if (window.empty()) return Values{na<double>()};
        double weighted = 0.0;
        double weights = 0.0;
        for (int i = 0; i < length; ++i) {
            weighted += window[static_cast<std::size_t>(i)] * (i + 1);
            weights += i + 1;
        }
        return Values{weighted / weights};
    };
    return subject;
}

// ta.vwma as the reference manual defines it:
// ta.sma(source * volume, length) / ta.sma(volume, length).
Subject vwma_subject(const Tape& tape) {
    Subject subject;
    subject.name = "ta.vwma";
    subject.sums_each_call = false;
    subject.series = [&tape] {
        auto site = std::make_shared<SeriesVwma>();
        return SeriesStep([site, &tape](int bar, double source, double length, bool again) {
            const double volume = tape.volume[static_cast<std::size_t>(bar)];
            return Values{again ? site->recompute(source, volume, length)
                                : site->compute(source, volume, length)};
        });
    };
    subject.fixed = [&tape](int length) {
        auto products = std::make_shared<ta::SMA>(length);
        auto volumes = std::make_shared<ta::SMA>(length);
        return FixedStep([products, volumes, &tape](int bar, double source, bool again) {
            const double volume = tape.volume[static_cast<std::size_t>(bar)];
            const double product_mean = again ? products->recompute(source * volume)
                                              : products->compute(source * volume);
            const double volume_mean = again ? volumes->recompute(volume)
                                             : volumes->compute(volume);
            if (volume_mean == 0.0) return Values{na<double>()};
            return Values{product_mean / volume_mean};
        });
    };
    subject.direct = [&tape](int bar, int length) {
        std::vector<double> products(tape.source.size());
        for (std::size_t i = 0; i < products.size(); ++i)
            products[i] = tape.source[i] * tape.volume[i];
        const std::vector<double> product_window = last_present(products, bar, length);
        const std::vector<double> volume_window = last_present(tape.volume, bar, length);
        if (product_window.empty() || volume_window.empty()) return Values{na<double>()};
        double product_sum = 0.0;
        double volume_sum = 0.0;
        for (int i = 0; i < length; ++i) {
            product_sum += product_window[static_cast<std::size_t>(i)];
            volume_sum += volume_window[static_cast<std::size_t>(i)];
        }
        if (volume_sum == 0.0) return Values{na<double>()};
        return Values{(product_sum / length) / (volume_sum / length)};
    };
    return subject;
}

Subject alma_subject(const Tape& tape, bool floor) {
    // Static, so the lambdas below read them without a capture.
    static constexpr double offset = 0.85;
    static constexpr double sigma = 6.0;
    Subject subject;
    subject.name = floor ? "ta.alma floor" : "ta.alma";
    subject.sums_each_call = true;
    subject.series = [floor] {
        auto site = std::make_shared<SeriesAlma>();
        return SeriesStep([site, floor](int, double source, double length, bool again) {
            if (floor) {
                return Values{again ? site->recompute(source, offset, sigma, true, length)
                                    : site->compute(source, offset, sigma, true, length)};
            }
            return Values{again ? site->recompute(source, offset, sigma, length)
                                : site->compute(source, offset, sigma, length)};
        });
    };
    subject.fixed = [floor](int length) {
        auto fixed = std::make_shared<ta::ALMA>(length, offset, sigma, floor);
        return FixedStep([fixed](int, double source, bool again) {
            return Values{again ? fixed->recompute(source) : fixed->compute(source)};
        });
    };
    subject.direct = [&tape, floor](int bar, int length) {
        const std::vector<double> window = last_bars(tape.source, bar, length);
        if (window.empty()) return Values{na<double>()};
        double centre = offset * (length - 1);
        if (floor) centre = std::floor(centre);
        const double spread = length / sigma;
        double weighted = 0.0;
        double weights = 0.0;
        for (int i = 0; i < length; ++i) {
            const double weight = std::exp(-((i - centre) * (i - centre)) / (2.0 * spread * spread));
            weights += weight;
            weighted += window[static_cast<std::size_t>(i)] * weight;
        }
        return Values{weighted / weights};  // na when the window holds an na source
    };
    return subject;
}

Subject linreg_subject(const Tape& tape) {
    static constexpr double offset = 1.0;
    Subject subject;
    subject.name = "ta.linreg";
    subject.sums_each_call = true;
    subject.series = [] {
        auto site = std::make_shared<SeriesLinreg>();
        return SeriesStep([site](int, double source, double length, bool again) {
            return Values{again ? site->recompute(source, offset, length)
                                : site->compute(source, offset, length)};
        });
    };
    subject.fixed = [](int length) {
        auto fixed = std::make_shared<ta::Linreg>(length);
        return FixedStep([fixed](int, double source, bool again) {
            return Values{again ? fixed->recompute(source, offset)
                                : fixed->compute(source, offset)};
        });
    };
    subject.direct = [&tape](int bar, int length) {
        const std::vector<double> window = last_bars(tape.source, bar, length);
        if (window.empty()) return Values{na<double>()};
        double mean_y = 0.0;
        for (double y : window) {
            if (std::isnan(y)) return Values{na<double>()};
            mean_y += y;
        }
        mean_y /= length;
        const double mean_x = (length - 1) / 2.0;
        double sxy = 0.0;
        double sxx = 0.0;
        for (int i = 0; i < length; ++i) {
            sxy += (i - mean_x) * (window[static_cast<std::size_t>(i)] - mean_y);
            sxx += (i - mean_x) * (i - mean_x);
        }
        if (sxx == 0.0) return Values{mean_y};
        const double slope = sxy / sxx;
        const double intercept = mean_y - slope * mean_x;
        return Values{intercept + slope * (length - 1 - offset)};
    };
    return subject;
}

// {middle, upper, lower} of the last `length` sources that are not na.
Values direct_bands(const Tape& tape, int bar, int length, double mult) {
    const std::vector<double> window = last_present(tape.source, bar, length);
    if (window.empty()) return Values{na<double>(), na<double>(), na<double>()};
    double sum = 0.0;
    double squares = 0.0;
    for (double x : window) {
        sum += x;
        squares += x * x;
    }
    const double mean = sum / length;
    const double variance = squares / length - mean * mean;
    const double deviation = std::sqrt(variance > 0.0 ? variance : 0.0);
    return Values{mean, mean + mult * deviation, mean - mult * deviation};
}

Subject bb_subject(const Tape& tape) {
    static constexpr double mult = 2.0;
    Subject subject;
    subject.name = "ta.bb";
    subject.sums_each_call = false;
    subject.series = [] {
        auto site = std::make_shared<SeriesBb>();
        return SeriesStep([site](int, double source, double length, bool again) {
            const ta::BBResult bands = again ? site->recompute(source, mult, length)
                                             : site->compute(source, mult, length);
            return Values{bands.middle, bands.upper, bands.lower};
        });
    };
    subject.fixed = [](int length) {
        auto fixed = std::make_shared<ta::BB>(length, mult);
        return FixedStep([fixed](int, double source, bool again) {
            const ta::BBResult bands = again ? fixed->recompute(source) : fixed->compute(source);
            return Values{bands.middle, bands.upper, bands.lower};
        });
    };
    subject.direct = [&tape](int bar, int length) { return direct_bands(tape, bar, length, mult); };
    return subject;
}

Subject bbw_subject(const Tape& tape) {
    static constexpr double mult = 2.0;
    Subject subject;
    subject.name = "ta.bbw";
    subject.sums_each_call = false;
    subject.series = [] {
        auto site = std::make_shared<SeriesBbw>();
        return SeriesStep([site](int, double source, double length, bool again) {
            return Values{again ? site->recompute(source, mult, length)
                                : site->compute(source, mult, length)};
        });
    };
    subject.fixed = [](int length) {
        auto fixed = std::make_shared<ta::BBW>(length, mult);
        return FixedStep([fixed](int, double source, bool again) {
            return Values{again ? fixed->recompute(source) : fixed->compute(source)};
        });
    };
    subject.direct = [&tape](int bar, int length) {
        const Values bands = direct_bands(tape, bar, length, mult);
        if (std::isnan(bands[0]) || bands[0] == 0.0) return Values{na<double>()};
        return Values{(bands[1] - bands[2]) / bands[0] * 100.0};
    };
    return subject;
}

// Check 1: the length never changes.
void constant_length(const Subject& subject, const Tape& tape) {
    for (int length : {1, 2, 3, 5, 7}) {
        const SeriesStep series = subject.series();
        const FixedStep fixed = subject.fixed(length);
        int present = 0;
        for (int bar = 0; bar < tape.bars(); ++bar) {
            const double source = tape.source[static_cast<std::size_t>(bar)];
            bool again = false;
            if (replaced_bar(tape, bar)) {
                const double earlier = earlier_value(tape, bar);
                series(bar, earlier, length, false);
                fixed(bar, earlier, false);
                again = true;
            }
            const Values got = series(bar, source, length, again);
            const Values expected = fixed(bar, source, again);
            present += same_bits(subject.name, bar, got, expected);
        }
        require_compared(subject.name, "constant length", present, 4);
    }
}

// Check 2: the length changes on every bar. With `replaced`, every bar is
// first called with another value and another length, then replaced.
void changing_length(const Subject& subject, const Tape& tape, bool replaced) {
    const SeriesStep series = subject.series();
    int present = 0;
    for (int bar = 0; bar < tape.bars(); ++bar) {
        const double source = tape.source[static_cast<std::size_t>(bar)];
        const int length = length_at(bar);
        if (replaced) series(bar, earlier_value(tape, bar), 1 + (bar * 5) % 6, false);
        const Values got = series(bar, source, length, replaced);
        present += close_to(subject.name, bar, got, subject.direct(bar, length));

        const FixedStep whole = subject.fixed(length);
        Values expected;
        for (int past = 0; past <= bar; ++past)
            expected = whole(past, tape.source[static_cast<std::size_t>(past)], false);
        if (subject.sums_each_call) same_bits(subject.name, bar, got, expected);
        else close_to(subject.name, bar, got, expected);
    }
    require_compared(subject.name, replaced ? "changing length, replaced calls" : "changing length",
                     present, 6);
}

// Hourly bars from 17:00 UTC of an invented day: bars 0..6 are one UTC day,
// bar 7 opens the next.
std::int64_t time_of(int bar) {
    return INT64_C(1728000000000) + (17 + bar) * INT64_C(3600000);
}

Values bands_of(const ta::VWAPBandsResult& result) {
    return Values{result.vwap, result.upper, result.lower};
}

// ta.vwap bands, check 1: a multiplier that never changes is the fixed class.
void vwap_bands_constant(const Tape& tape) {
    constexpr double mult = 1.5;
    const std::string tz = "UTC";
    const std::string session = "24x7";
    SeriesVwapBands day_site, session_site, anchor_site, clock_anchor_site;
    ta::VWAPBands day_fixed(mult), session_fixed(mult);
    ta::AnchoredVWAPBands anchor_fixed(mult);
    int present = 0;
    for (int bar = 0; bar < tape.bars(); ++bar) {
        const double source = tape.source[static_cast<std::size_t>(bar)];
        const double volume = tape.volume[static_cast<std::size_t>(bar)];
        const std::int64_t stamp = time_of(bar);
        const bool anchor = bar == 0 || bar == 7;
        if (replaced_bar(tape, bar)) {
            const double earlier = earlier_value(tape, bar);
            day_site.compute(earlier, volume, stamp, mult);
            day_fixed.compute(earlier, volume, stamp);
            session_site.compute(earlier, volume, stamp, tz, session, mult);
            session_fixed.compute(earlier, volume, stamp, tz, session);
            anchor_site.compute(earlier, volume, anchor, mult);
            clock_anchor_site.compute(earlier, volume, stamp, tz, session, anchor, mult);
            anchor_fixed.compute(earlier, volume, anchor);
            present += same_bits("ta.vwap bands, day", bar,
                                 bands_of(day_site.recompute(source, volume, stamp, mult)),
                                 bands_of(day_fixed.recompute(source, volume, stamp)));
            same_bits("ta.vwap bands, session day", bar,
                      bands_of(session_site.recompute(source, volume, stamp, tz, session, mult)),
                      bands_of(session_fixed.recompute(source, volume, stamp, tz, session)));
            const Values anchored = bands_of(anchor_fixed.recompute(source, volume, anchor));
            same_bits("ta.vwap bands, anchor", bar,
                      bands_of(anchor_site.recompute(source, volume, anchor, mult)), anchored);
            same_bits("ta.vwap bands, anchor with clock arguments", bar,
                      bands_of(clock_anchor_site.recompute(source, volume, stamp, tz, session,
                                                           anchor, mult)),
                      anchored);
            continue;
        }
        present += same_bits("ta.vwap bands, day", bar,
                             bands_of(day_site.compute(source, volume, stamp, mult)),
                             bands_of(day_fixed.compute(source, volume, stamp)));
        same_bits("ta.vwap bands, session day", bar,
                  bands_of(session_site.compute(source, volume, stamp, tz, session, mult)),
                  bands_of(session_fixed.compute(source, volume, stamp, tz, session)));
        const Values anchored = bands_of(anchor_fixed.compute(source, volume, anchor));
        same_bits("ta.vwap bands, anchor", bar,
                  bands_of(anchor_site.compute(source, volume, anchor, mult)), anchored);
        same_bits("ta.vwap bands, anchor with clock arguments", bar,
                  bands_of(clock_anchor_site.compute(source, volume, stamp, tz, session, anchor,
                                                     mult)),
                  anchored);
    }
    require_compared("ta.vwap bands", "constant multiplier", present, 30);
}

// ta.vwap bands, check 2: a multiplier that changes on every bar scales the
// deviation of its own bar. The sums are written out here; they restart on
// bar 7, where the day changes and the anchor is raised.
void vwap_bands_changing(const Tape& tape) {
    SeriesVwapBands day_site, anchor_site;
    double price_volume = 0.0;
    double square_volume = 0.0;
    double total_volume = 0.0;
    int present = 0;
    for (int bar = 0; bar < tape.bars(); ++bar) {
        const double source = tape.source[static_cast<std::size_t>(bar)];
        const double volume = tape.volume[static_cast<std::size_t>(bar)];
        const double mult = 0.5 + 0.25 * (bar % 4);
        const bool anchor = bar == 0 || bar == 7;
        Values expected{na<double>(), na<double>(), na<double>()};
        if (!std::isnan(source) && !std::isnan(volume)) {
            if (bar == 7) {
                price_volume = 0.0;
                square_volume = 0.0;
                total_volume = 0.0;
            }
            price_volume += source * volume;
            square_volume += source * source * volume;
            total_volume += volume;
            const double mean = price_volume / total_volume;
            const double variance = square_volume / total_volume - mean * mean;
            const double deviation = std::sqrt(variance > 0.0 ? variance : 0.0);
            expected = Values{mean, mean + mult * deviation, mean - mult * deviation};
        }
        present += close_to("ta.vwap bands, day, changing multiplier", bar,
                            bands_of(day_site.compute(source, volume, time_of(bar), mult)),
                            expected);
        close_to("ta.vwap bands, anchor, changing multiplier", bar,
                 bands_of(anchor_site.compute(source, volume, anchor, mult)), expected);
    }
    require_compared("ta.vwap bands", "changing multiplier", present, 30);
}

struct Stop {
    bool stopped = false;
    std::string text;
    pineforge::RunFailureCode code = pineforge::RunFailureCode::none;
};

template <class Call>
Stop stop_of(Call&& call) {
    Stop stop;
    try {
        call();
    } catch (const std::exception& error) {
        stop.stopped = true;
        stop.text = error.what();
        stop.code = pineforge::classify_run_failure(error).code;
    }
    return stop;
}

bool ends_with(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// SeriesSma's stop text with its function name replaced.
std::string renamed(std::string text, const std::string& function) {
    const std::string from = "'sma'";
    const std::size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), "'" + function + "'");
    return text;
}

struct Checked {
    const char* function;
    std::function<void(double length)> call;
};

std::vector<Checked> checked_calls() {
    return {
        {"wma", [](double length) { SeriesWma().compute(10.0, length); }},
        // expectation corrected: 'vwma'->'sma' -> 'vwma', because a series
        // length is checked with simple_ta_length, which keeps the text of
        // the released engine: every call is named by itself. Only the
        // checked fixed-length adapters word a stop as TradingView does.
        {"vwma", [](double length) { SeriesVwma().compute(10.0, 5.0, length); }},
        {"alma", [](double length) { SeriesAlma().compute(10.0, 0.85, 6.0, length); }},
        {"linreg", [](double length) { SeriesLinreg().compute(10.0, 0.0, length); }},
        {"bb", [](double length) { SeriesBb().compute(10.0, 2.0, length); }},
        {"bbw", [](double length) { SeriesBbw().compute(10.0, 2.0, length); }},
    };
}

// The length is checked as SeriesSma checks its own: the same stop, with the
// call's function name.
void length_stops() {
    const std::vector<Checked> calls = checked_calls();
    const double invalid[] = {0.0, -2.5, 0.75, na<double>(), HUGE_VAL, -HUGE_VAL};
    int compared = 0;
    for (const Checked& checked : calls) {
        for (double length : invalid) {
            const Stop got = stop_of([&] { checked.call(length); });
            const Stop sma = stop_of([&] { SeriesSma().compute(10.0, length); });
            const std::string expected = renamed(sma.text, checked.function);
            if (!sma.stopped) {
                std::printf("FAIL %s length=%g NOT COMPARED: SeriesSma did not stop\n",
                            checked.function, length);
                ++failures;
                continue;
            }
            ++compared;
            if (!got.stopped || got.text != expected || got.code != sma.code) {
                std::printf("FAIL %s length=%g stopped=%d text=\"%s\" expected=\"%s\"\n",
                            checked.function, length, got.stopped ? 1 : 0, got.text.c_str(),
                            expected.c_str());
                ++failures;
            }
        }
    }
    require_compared("length stops", "argument", compared, 36);

    const Stop zero = stop_of([] { SeriesWma().compute(10.0, 0.0); });
    require(ends_with(zero.text, "Invalid value of the 'length' argument (0) in the 'wma' "
                                 "function. It must be > 0."),
            "ta.wma length 0 stop text");
    const Stop missing = stop_of([] { SeriesBb().compute(10.0, 2.0, na<double>()); });
    require(ends_with(missing.text, "Invalid value of the 'length' argument in the 'bb' "
                                    "function. It must not be na"),
            "ta.bb na length stop text");

    // A lookback limit of 3 bars: a length of 4 reads 3 bars back and is
    // allowed, a length of 6 reads 5 and stops.
    const pineforge::detail::TaLookbackScope limit(3);
    int limited = 0;
    for (const Checked& checked : calls) {
        const Stop allowed = stop_of([&] { checked.call(4.0); });
        const Stop got = stop_of([&] { checked.call(6.0); });
        const Stop sma = stop_of([] { SeriesSma().compute(10.0, 6.0); });
        if (!sma.stopped) {
            std::printf("FAIL %s lookback NOT COMPARED: SeriesSma did not stop\n",
                        checked.function);
            ++failures;
            continue;
        }
        ++limited;
        if (allowed.stopped || !got.stopped || got.text != sma.text || got.code != sma.code ||
            !ends_with(got.text, "(5 bars back). The historical buffer's limit is 3 bars.")) {
            std::printf("FAIL %s lookback allowed_stopped=%d stopped=%d text=\"%s\" "
                        "expected=\"%s\"\n",
                        checked.function, allowed.stopped ? 1 : 0, got.stopped ? 1 : 0,
                        got.text.c_str(), sma.text.c_str());
            ++failures;
        }
    }
    require_compared("length stops", "lookback limit", limited, 6);
}

// A refused length records nothing: the next call answers as if the refused
// one had not been made.
void refused_call_records_nothing() {
    SeriesWma refused, clean;
    refused.compute(10.0, 2.0);
    clean.compute(10.0, 2.0);
    const Stop stop = stop_of([&] { refused.compute(99.0, 0.0); });
    require(stop.stopped, "ta.wma length 0 after a valid call stops");
    const double got = refused.compute(14.0, 2.0);
    same_bits("ta.wma after a refused call", 1, Values{got}, Values{clean.compute(14.0, 2.0)});
    close_to("ta.wma after a refused call, value", 1, Values{got}, Values{38.0 / 3.0});
}

// Under a lookback limit of 4 an object keeps five samples; lengths up to 5
// read no further back, so it answers what an object without a limit does.
void limited_history(const Tape& tape) {
    SeriesWma open_wma;
    SeriesAlma open_alma;
    SeriesBb open_bb;
    const pineforge::detail::TaLookbackScope limit(4);
    SeriesWma kept_wma;
    SeriesAlma kept_alma;
    SeriesBb kept_bb;
    int present = 0;
    for (int bar = 0; bar < tape.bars(); ++bar) {
        const double source = tape.source[static_cast<std::size_t>(bar)];
        const double length = 2 + bar % 4;
        present += same_bits("ta.wma under a lookback limit", bar,
                             Values{kept_wma.compute(source, length)},
                             Values{open_wma.compute(source, length)});
        present += same_bits("ta.alma under a lookback limit", bar,
                             Values{kept_alma.compute(source, 0.85, 6.0, length)},
                             Values{open_alma.compute(source, 0.85, 6.0, length)});
        const ta::BBResult kept = kept_bb.compute(source, 2.0, length);
        const ta::BBResult open = open_bb.compute(source, 2.0, length);
        present += same_bits("ta.bb under a lookback limit", bar,
                             Values{kept.middle, kept.upper, kept.lower},
                             Values{open.middle, open.upper, open.lower});
    }
    require_compared("lookback limit", "kept history", present, 40);
}

// A copy takes the history as it is and goes its own way: it answers what a
// new object given the same calls answers, and the object it was copied from
// goes on as one that was never copied (`twin`).
void copies_are_independent(const Tape& tape) {
    SeriesBb site, twin;
    int present = 0;
    for (int bar = 0; bar < tape.bars(); ++bar) {
        const double source = tape.source[static_cast<std::size_t>(bar)];
        const double length = length_at(bar);
        if (bar == 12) {
            SeriesBb branch = site;
            SeriesBb replayed;
            for (int past = 0; past < bar; ++past)
                replayed.compute(tape.source[static_cast<std::size_t>(past)], 2.0, length_at(past));
            for (double other : {40.0, 41.5, 39.25}) {
                const ta::BBResult a = branch.compute(other, 2.0, 2.0);
                const ta::BBResult b = replayed.compute(other, 2.0, 2.0);
                present += same_bits("ta.bb copy", bar, Values{a.middle, a.upper, a.lower},
                                     Values{b.middle, b.upper, b.lower});
            }
        }
        const ta::BBResult a = site.compute(source, 2.0, length);
        const ta::BBResult b = twin.compute(source, 2.0, length);
        present += same_bits("ta.bb beside a copy", bar, Values{a.middle, a.upper, a.lower},
                             Values{b.middle, b.upper, b.lower});
    }
    require_compared("copies", "ta.bb", present, 40);
}

// 2,200 invented sources: the history outgrows its first 1,024 slots twice,
// and two calls ask for a window of 1,050 that reaches across that growth.
void long_history() {
    constexpr int bars = 2200;
    constexpr int wide = 1050;
    std::vector<double> source(static_cast<std::size_t>(bars));
    for (int i = 0; i < bars; ++i)
        source[static_cast<std::size_t>(i)] = 40.0 + 0.25 * ((i * 37) % 101) - 0.125 * (i % 7);
    SeriesWma site;
    ta::WMA four(4);  // the fixed-length class of length 4 over the whole history
    int present = 0;
    for (int bar = 0; bar < bars; ++bar) {
        const double value = source[static_cast<std::size_t>(bar)];
        const bool is_wide = bar == 1100 || bar == 2150;
        const double got = site.compute(value, is_wide ? wide : 4);
        const double four_value = four.compute(value);
        if (!is_wide) {
            present += same_bits("ta.wma long history, length 4", bar, Values{got},
                                 Values{four_value});
            continue;
        }
        ta::WMA whole(wide);
        double expected = na<double>();
        for (int past = 0; past <= bar; ++past)
            expected = whole.compute(source[static_cast<std::size_t>(past)]);
        present += same_bits("ta.wma long history, length 1050", bar, Values{got},
                             Values{expected});
    }
    require_compared("long history", "ta.wma", present, 2190);
}

}  // namespace

int main() {
    const Tape tape = invented_tape();
    const std::vector<Subject> subjects = {
        wma_subject(tape),    vwma_subject(tape), alma_subject(tape, false),
        alma_subject(tape, true), linreg_subject(tape), bb_subject(tape),
        bbw_subject(tape),
    };
    for (const Subject& subject : subjects) {
        constant_length(subject, tape);
        changing_length(subject, tape, false);
        changing_length(subject, tape, true);
    }
    vwap_bands_constant(tape);
    vwap_bands_changing(tape);
    length_stops();
    refused_call_records_nothing();
    limited_history(tape);
    copies_are_independent(tape);
    long_history();
    if (failures != 0) {
        std::printf("FAILED %d checks\n", failures);
        return 1;
    }
    std::puts("PASS series-length moving averages and bands answer the fixed-length window "
              "of each call");
    return 0;
}
