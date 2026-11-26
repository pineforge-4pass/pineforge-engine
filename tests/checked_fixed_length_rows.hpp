#pragma once
// Rows shared by the checked fixed-length adapter tests
// (tests/test_checked_fixed_*.cpp, one per Pine function).
//
// The adapters of <pineforge/source/pine_ta_length.hpp> (PineFixedLength,
// PineFixedDmiLengths and their aliases) are the types a ta.* call with a
// constant or input length is built with: a length of 0, a negative length or
// an na length stops the call's first execution with the coded
// pine_invalid_argument error, worded as TradingView words it for that call
// (PineLengthWording), and a valid length computes the values of the type the
// adapter wraps. That type is the fixed-length class of <pineforge/ta.hpp>
// for most calls and the Pine type of the same header for ta.vwma (PineVwma),
// ta.mfi (PineMfi) and ta.dmi (PineDmiReference).
//
// Every test names its type through an alias: the adapter where the header
// defines PINEFORGE_HAS_PINE_FIXED_LENGTH, otherwise the type such a call was
// built with before the adapter existed, which is the wrapped type itself.
// There the zero-length rows fail, because that type takes the length and
// computes. The stop rows run first and are counted, not asserted, so that
// every call that computes where it must stop, and every stop that reads
// otherwise than TradingView's, is printed; nothing else runs after a failed
// one. The zero-length rows call compute() only, on a bar each test chooses so
// that the type without the adapter stays inside its defined behaviour for a
// length of 0. Every other row is reached only once the stop rows pass, and
// so never builds a wrapped type from an invalid length.
//
// Only the adapters word a stop as TradingView does. A simple length, which
// generated code checks with simple_ta_length, keeps the text of the released
// engine byte for byte: the function and argument names of the Pine call and
// "> 0" (simple_length_row). For most calls the two texts are the same one;
// for ta.rsi, ta.atr, ta.vwma and both lengths of ta.dmi they differ, and
// each path is held to its own.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <string>
#include <vector>

namespace checked_fixed_length {

using pineforge::na;
using pineforge::OwnedStorage;
using pineforge::RunFailureCode;
using pineforge::RunFailureInfo;
namespace ta = pineforge::ta;

// One invented bar. A test's Feed says which fields its function reads.
struct Bar {
    double high;
    double low;
    double close;
    double volume;
};

// One length argument of one Pine call, and how its error reads. The coded
// failure names the function and the argument as the compiled strategy spells
// them. The text names them as TradingView does: for most calls the same two
// names ("the 'length' argument ... in the 'sma' function. It must be > 0."),
// for some another argument name, the function TradingView raises the error
// in, or another relation. Each test writes its own text parts out.
struct Site {
    const char* function;        // the Pine function, without "ta."
    const char* argument;        // the Pine argument
    const char* shown_function;  // the function part of the text, quotes included
    const char* shown_argument;  // the argument name in the text
    const char* relation;        // "> 0", or "greater than 0"
};

// Bit identity; any two NaNs count as the same na.
inline bool same_bits(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

// Numeric equality; any two NaNs count as the same na, and -0 equals 0.
inline bool same_value(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return a == b;
}

inline std::string number(double value) {
    if (std::isnan(value)) return "na";
    char text[40];
    std::snprintf(text, sizeof text, "%.17g", value);
    return text;
}

// What one call did: the coded stop it raised, or nothing.
struct Outcome {
    bool stopped = false;
    bool coded = false;
    RunFailureCode code = RunFailureCode::none;
    std::string args;  // the canonical arguments of the coded failure
    std::string what;
};

template <class Call>
Outcome outcome(Call call) {
    Outcome out;
    try {
        call();
    } catch (const std::exception& error) {
        out.stopped = true;
        out.what = error.what();
        if (const auto* failure = dynamic_cast<const RunFailureInfo*>(&error)) {
            out.coded = true;
            out.code = failure->run_failure_code();
            if (failure->run_failure().args) out.args = *failure->run_failure().args;
        }
    }
    return out;
}

// The text of a length of zero or less, `shown` being the value as printed,
// and of an na length, without the bar prefix.
inline std::string positive_text(const Site& site, const std::string& shown) {
    return std::string("Invalid value of the '") + site.shown_argument + "' argument (" + shown +
           ") in the " + site.shown_function + " function. It must be " + site.relation + ".";
}
inline std::string na_text(const Site& site) {
    return std::string("Invalid value of the '") + site.shown_argument + "' argument in the " +
           site.shown_function + " function. It must not be na";
}

// The same two texts as simple_ta_length words them, without the bar prefix:
// the argument and the function as the Pine call names them, each in one pair
// of quotes, and "> 0". This is the text of the released engine.
inline std::string released_positive_text(const Site& site, const std::string& shown) {
    return std::string("Invalid value of the '") + site.argument + "' argument (" + shown +
           ") in the '" + site.function + "' function. It must be > 0.";
}
inline std::string released_na_text(const Site& site) {
    return std::string("Invalid value of the '") + site.argument + "' argument in the '" +
           site.function + "' function. It must not be na";
}

// Whether TradingView words the call with its own two names and "> 0". Then
// the adapter's text and the released text are one and the same.
inline bool plain_wording(const Site& site) {
    return std::string(site.shown_argument) == site.argument &&
           std::string(site.shown_function) == std::string("'") + site.function + "'" &&
           std::string(site.relation) == "> 0";
}

inline std::string call_name(const Site& site) { return std::string("ta.") + site.function; }

inline int& failures() {
    static int count = 0;
    return count;
}

inline void fail(const std::string& row, const std::string& context, const std::string& why) {
    std::printf("FAIL %s, %s: %s\n", row.c_str(), context.c_str(), why.c_str());
    ++failures();
}

// `got` must be the coded Pine stop with `text`, whose arguments name the
// site's function and argument and the rule.
inline void expect_stop(const Outcome& got, const std::string& row, const std::string& context,
                        const std::string& text, const Site& site, const char* rule) {
    if (!got.stopped) {
        fail(row, context, "the call did not stop; expected the stop \"" + text + "\"");
        return;
    }
    if (!got.coded || got.code != RunFailureCode::pine_invalid_argument) {
        fail(row, context, "the stop is not coded pine_invalid_argument: " + got.what);
    }
    if (got.what != text) {
        fail(row, context, "the stop reads \"" + got.what + "\"; expected \"" + text + "\"");
    }
    const std::string function_field = std::string("\"function\":\"") + site.function + "\"";
    const std::string argument_field = std::string("\"argument\":\"") + site.argument + "\"";
    const std::string rule_field = std::string("\"rule\":\"") + rule + "\"";
    if (got.args.find(function_field) == std::string::npos ||
        got.args.find(argument_field) == std::string::npos ||
        got.args.find(rule_field) == std::string::npos) {
        fail(row, context,
             "the coded arguments read " + got.args + "; expected " + function_field + ", " +
                 argument_field + " and " + rule_field);
    }
}

// Two calls must have raised the very same stop: code, text and arguments.
inline void expect_same_stop(const Outcome& got, const Outcome& other, const std::string& row,
                             const std::string& context) {
    if (!got.stopped || !other.stopped || got.code != other.code || got.what != other.what ||
        got.args != other.args) {
        fail(row, context,
             "the fixed call raised \"" + got.what + "\" " + got.args + ", the other path \"" +
                 other.what + "\" " + other.args);
    }
}

// Two calls must have raised the same coded failure: the code and the
// arguments. Their texts are not compared here; each has its own row.
inline void expect_same_failure(const Outcome& got, const Outcome& other, const std::string& row,
                                const std::string& context) {
    if (!got.stopped || !other.stopped || !got.coded || !other.coded || got.code != other.code ||
        got.args != other.args) {
        fail(row, context,
             "the fixed call raised " + got.args + " (\"" + got.what + "\"), the other path " +
                 other.args + " (\"" + other.what + "\")");
    }
}

// A deterministic invented sequence with na gaps: bar i has
// base = 100 + 0.75 i + (i % 7) * 1.25 - (i % 3) * 2, high = base + 1.5 +
// (i % 4) * 0.5, low = base - 1 - (i % 5) * 0.25, close = base +- 0.5 and
// volume = 10 + (i % 6) * 2.5; the close is na on every bar with i % 9 == 4,
// the high on bar 0 and the volume on every bar with i % 11 == 7.
inline std::vector<Bar> sequence() {
    std::vector<Bar> out;
    for (int i = 0; i < 48; ++i) {
        const double base = 100.0 + 0.75 * i + (i % 7) * 1.25 - (i % 3) * 2.0;
        Bar bar{base + 1.5 + (i % 4) * 0.5, base - 1.0 - (i % 5) * 0.25,
                base + ((i % 2) ? 0.5 : -0.5), 10.0 + (i % 6) * 2.5};
        if (i % 9 == 4) bar.close = na<double>();
        if (i == 0) bar.high = na<double>();
        if (i % 11 == 7) bar.volume = na<double>();
        out.push_back(bar);
    }
    return out;
}

// The same bar after a recalculation moved it.
inline Bar moved(const Bar& bar) {
    return Bar{bar.high + 3.5, bar.low + 3.5, bar.close + 3.5, bar.volume + 1.0};
}

// A function with no series-length form among these rows.
struct NoSeriesTwin {
    static constexpr bool present = false;
    static void call(double, const Bar&) {}
};

// --- the stop rows: the only ones a type without the adapter runs ---

// One fixed call with a length of 0 on its first execution. It must stop with
// `prefix` + the length's text, and the failed check must build nothing, so
// the next call stops again.
template <class Fixed, class Feed>
void zero_length_row(const Site& site, const Bar& bar, const char* context, const char* prefix) {
    const std::string row = call_name(site) + " with a length of 0";
    const std::string text = prefix + positive_text(site, "0");
    Fixed fixed(0);  // constructing never stops
    double got = na<double>();
    const Outcome first = outcome([&] { got = Feed::compute(fixed, bar); });
    if (!first.stopped) {
        fail(row, context,
             "the call computed " + number(got) + "; expected the stop \"" + text + "\"");
        return;
    }
    expect_stop(first, row, context, text, site, "must_be_positive");
    const Outcome second = outcome([&] { Feed::compute(fixed, bar); });
    if (!second.stopped || second.what != text) {
        fail(row, context, "the call after a failed check did not stop again");
    }
}

template <class Fixed, class Feed>
void zero_length_rows(const Site& site, const Bar& bar) {
    zero_length_row<Fixed, Feed>(site, bar, "compute, no bar context",
                                 "Error before the first bar: ");
    {
        // The first chart bar.
        ta::BarContextScope scope(0, 0);
        zero_length_row<Fixed, Feed>(site, bar, "compute on bar 0", "Error on bar 0: ");
    }
    {
        // A call first reached on a later bar stops there, not before.
        ta::BarContextScope scope(2, 0);
        zero_length_row<Fixed, Feed>(site, bar, "compute first reached on bar 2",
                                     "Error on bar 2: ");
    }
}

// The same argument as a simple length: one that is fixed for the run but is
// neither a constant nor an input, which generated code checks with
// simple_ta_length under the Pine function and argument names. That path is
// the released one and keeps its text byte for byte, whatever TradingView's
// wording of the adapter's stop is: 0, a negative value and na, the last as a
// NaN and as the integer sentinel.
// expectation corrected: the adapter's wording -> the released text, because
// only the checked fixed-length adapters word a stop as TradingView does. For
// ta.rsi ('y'), ta.atr ('atr'->'rma'), ta.vwma ('vwma'->'sma') and the two
// lengths of ta.dmi ('DI Length', and 'ADX Smoothing' with "greater than")
// the two texts differ; for every other call they are the same text.
inline void simple_length_row(const Site& site) {
    using pineforge::source::simple_ta_length;
    const std::string name = call_name(site) + " with a simple " + site.argument;
    const std::string prefix = "Error before the first bar: ";
    expect_stop(outcome([&] { simple_ta_length(0, site.function, site.argument); }),
                name + " of 0", "simple_ta_length", prefix + released_positive_text(site, "0"),
                site, "must_be_positive");
    expect_stop(outcome([&] { simple_ta_length(-3, site.function, site.argument); }),
                name + " of -3", "simple_ta_length", prefix + released_positive_text(site, "-3"),
                site, "must_be_positive");
    expect_stop(outcome([&] { simple_ta_length(na<double>(), site.function, site.argument); }),
                name + " of na", "simple_ta_length", prefix + released_na_text(site), site,
                "must_not_be_na");
    expect_stop(outcome([&] { simple_ta_length(na<int>(), site.function, site.argument); }),
                name + " of an int na", "simple_ta_length", prefix + released_na_text(site), site,
                "must_not_be_na");
}

// --- the rows below run once every stop row passed ---

// A call built with `length` stops its first execution, by compute() and by
// recompute(), and stops again on the next call.
template <class Fixed, class Feed, class Length>
void stops_with(const Site& site, const Bar& bar, Length length, const std::string& row,
                const std::string& text, const char* rule) {
    const std::string full = "Error before the first bar: " + text;
    for (bool again : {false, true}) {
        Fixed fixed(length);
        const Outcome first = outcome([&] {
            if (again) Feed::recompute(fixed, bar);
            else Feed::compute(fixed, bar);
        });
        expect_stop(first, row, again ? "recompute as the first call" : "compute", full, site,
                    rule);
        const Outcome second = outcome([&] { Feed::recompute(fixed, bar); });
        expect_stop(second, row, "the call after a failed check", full, site, rule);
    }
}

template <class Fixed, class Feed>
void invalid_lengths_stop(const Site& site, const Bar& bar) {
    const std::string name = call_name(site);
    stops_with<Fixed, Feed>(site, bar, 0, name + " with a length of 0",
                            positive_text(site, "0"), "must_be_positive");
    stops_with<Fixed, Feed>(site, bar, -1, name + " with a length of -1",
                            positive_text(site, "-1"), "must_be_positive");
    stops_with<Fixed, Feed>(site, bar, -3, name + " with a length of -3",
                            positive_text(site, "-3"), "must_be_positive");
    stops_with<Fixed, Feed>(site, bar, static_cast<std::int64_t>(0),
                            name + " with an int64 length of 0", positive_text(site, "0"),
                            "must_be_positive");
    // A float length truncates toward zero before the check.
    stops_with<Fixed, Feed>(site, bar, 0.5, name + " with a length of 0.5",
                            positive_text(site, "0"), "must_be_positive");
    // An na length, as the integer sentinel and as a NaN.
    stops_with<Fixed, Feed>(site, bar, na<int>(), name + " with an int na length",
                            na_text(site), "must_not_be_na");
    stops_with<Fixed, Feed>(site, bar, na<double>(), name + " with a double na length",
                            na_text(site), "must_not_be_na");
}

// The stop is the coded failure a simple length of the same value raises
// (simple_ta_length): the same code and arguments, and the same text where
// TradingView words the call with its own names. Where it does not, the
// adapter's text is TradingView's and the simple length's the released one;
// invalid_lengths_stop and simple_length_row hold each to its own. For a
// function with a series-length twin the stop is the one that form raises for
// the same value, text included.
template <class Fixed, class Feed, class Twin>
void same_stop_as_the_other_paths(const Site& site, const Bar& bar) {
    const std::string name = call_name(site);
    for (int length : {0, -1}) {
        const std::string row = name + " with a length of " + std::to_string(length);
        Fixed fixed(length);
        const Outcome got = outcome([&] { Feed::compute(fixed, bar); });
        const Outcome simple = outcome([&] {
            pineforge::source::simple_ta_length(length, site.function, site.argument);
        });
        if (plain_wording(site)) expect_same_stop(got, simple, row, "against a simple length");
        else expect_same_failure(got, simple, row, "against a simple length");
        if constexpr (Twin::present) {
            const Outcome series =
                outcome([&] { Twin::call(static_cast<double>(length), bar); });
            expect_same_stop(got, series, row, "against a series length");
        }
    }
}

// One bar on both objects: compute, a recompute of the same bar moved, then
// the recompute with the bar's own values (the committed one).
template <class Fixed, class Wrapped, class Feed>
void same_bar(Fixed& fixed, Wrapped& wrapped, const Bar& bar, const std::string& row,
              std::size_t index) {
    const Bar other = moved(bar);
    const double pairs[3][2] = {
        {Feed::compute(fixed, bar), Feed::compute(wrapped, bar)},
        {Feed::recompute(fixed, other), Feed::recompute(wrapped, other)},
        {Feed::recompute(fixed, bar), Feed::recompute(wrapped, bar)},
    };
    const char* const steps[3] = {"compute", "recompute of the moved bar",
                                  "recompute of the bar's own values"};
    for (int step = 0; step < 3; ++step) {
        if (!same_bits(pairs[step][0], pairs[step][1])) {
            fail(row, std::string(steps[step]) + " on bar " + std::to_string(index),
                 "the adapter answered " + number(pairs[step][0]) + ", the wrapped type " +
                     number(pairs[step][1]));
        }
    }
}

// A valid length computes the wrapped type's own values, bit for bit, on the
// sequence with na gaps; int, int64 and double lengths (a float one truncates
// toward zero, as the check truncates it).
template <class Fixed, class Wrapped, class Feed>
void valid_length_is_the_wrapped_type(const Site& site) {
    const std::vector<Bar> seq = sequence();
    const std::string name = call_name(site);
    {
        Fixed fixed(7);
        Wrapped wrapped(7);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(fixed, wrapped, seq[i], name + " with a length of 7",
                                           i);
    }
    {
        Fixed fixed(static_cast<std::int64_t>(5));
        Wrapped wrapped(5);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(fixed, wrapped, seq[i],
                                           name + " with an int64 length of 5", i);
    }
    {
        Fixed fixed(5.9);
        Wrapped wrapped(5);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(fixed, wrapped, seq[i],
                                           name + " with a length of 5.9", i);
    }
    {
        Fixed fixed(1);
        Wrapped wrapped(1);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(fixed, wrapped, seq[i], name + " with a length of 1",
                                           i);
    }
}

// The drop-in shapes of generated code.
template <class Fixed, class Wrapped, class Feed>
void shapes(const Site& site, const Bar& bar) {
    const std::vector<Bar> seq = sequence();
    const std::string name = call_name(site);
    const std::string zero = "Error before the first bar: " + positive_text(site, "0");

    // A recompute as the very first call reaches the wrapped type's pristine
    // state.
    {
        Fixed fixed(7);
        Wrapped wrapped(7);
        for (std::size_t i = 0; i < 5; ++i) {
            const double got = Feed::recompute(fixed, seq[i]);
            const double want = Feed::recompute(wrapped, seq[i]);
            if (!same_bits(got, want)) {
                fail(name, "recompute before any compute, bar " + std::to_string(i),
                     "the adapter answered " + number(got) + ", the wrapped type " +
                         number(want));
            }
        }
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(fixed, wrapped, seq[i],
                                           name + " after recomputes as first calls", i);
    }

    // The generated reset `member = Type(length)` after use: no state survives
    // it, a reset to an invalid length stops instead of answering from the
    // old object, and a reset back to valid starts again from the first bar.
    {
        Fixed member(7);
        {
            Wrapped wrapped(7);
            for (std::size_t i = 0; i < seq.size(); ++i)
                same_bar<Fixed, Wrapped, Feed>(member, wrapped, seq[i], name + " before a reset",
                                               i);
        }
        member = Fixed(0);
        expect_stop(outcome([&] { Feed::compute(member, bar); }), name,
                    "compute after a reset to a length of 0", zero, site, "must_be_positive");
        expect_stop(outcome([&] { Feed::recompute(member, bar); }), name,
                    "recompute after a reset to a length of 0", zero, site, "must_be_positive");
        member = Fixed(7);
        Wrapped fresh(7);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(member, fresh, seq[i], name + " after a reset", i);
    }

    // Copies between calls each continue as an independent copy of the
    // wrapped type; one copy runs ahead first, so a copy sharing state would
    // move the original. Copies of an unbuilt invalid adapter each stop.
    {
        const std::size_t split = 20;
        const std::size_t ahead = 26;
        Fixed original(7);
        Wrapped wrapped(7);
        for (std::size_t i = 0; i < split; ++i)
            same_bar<Fixed, Wrapped, Feed>(original, wrapped, seq[i], name + " before a copy", i);
        Fixed constructed(original);
        Wrapped constructed_wrapped(wrapped);
        Fixed assigned(3);
        Feed::compute(assigned, seq[1]);  // built, then overwritten
        assigned = original;
        Wrapped assigned_wrapped(wrapped);
        for (std::size_t i = split; i < ahead; ++i)
            same_bar<Fixed, Wrapped, Feed>(constructed, constructed_wrapped, seq[i],
                                           name + ", copy-constructed copy running ahead", i);
        for (std::size_t i = split; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(original, wrapped, seq[i], name + ", the original", i);
        for (std::size_t i = split; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(assigned, assigned_wrapped, seq[i],
                                           name + ", copy-assigned copy", i);
        for (std::size_t i = ahead; i < seq.size(); ++i)
            same_bar<Fixed, Wrapped, Feed>(constructed, constructed_wrapped, seq[i],
                                           name + ", copy-constructed copy", i);

        Fixed invalid(0);
        Fixed invalid_constructed(invalid);
        Fixed invalid_assigned(7);
        invalid_assigned = invalid;
        expect_stop(outcome([&] { Feed::compute(invalid, bar); }), name,
                    "an unbuilt adapter with a length of 0", zero, site, "must_be_positive");
        expect_stop(outcome([&] { Feed::recompute(invalid_constructed, bar); }), name,
                    "a copy-constructed copy of it", zero, site, "must_be_positive");
        expect_stop(outcome([&] { Feed::compute(invalid_assigned, bar); }), name,
                    "a copy-assigned copy of it", zero, site, "must_be_positive");
    }

    // Constructing, observing and destroying never stop, whatever the length:
    // a call site that is never reached does not stop a run. A built adapter
    // reports its object's storage as completely as the wrapped type reports
    // it.
    {
        const Outcome quiet = outcome([&] {
            OwnedStorage built;
            Fixed valid(7);
            Feed::compute(valid, seq[1]);
            valid.observe_owned_storage(built);
            OwnedStorage plain;
            Wrapped wrapped(7);
            Feed::compute(wrapped, seq[1]);
            wrapped.observe_owned_storage(plain);
            if (built.usage().complete() != plain.usage().complete()) {
                fail(name, "observing",
                     "the adapter's observation and the wrapped type's differ in completeness");
            }
            OwnedStorage unbuilt_storage;
            Fixed unbuilt(7);
            unbuilt.observe_owned_storage(unbuilt_storage);
            Fixed invalid(0);
            invalid.observe_owned_storage(unbuilt_storage);
            Fixed negative(-4);
            negative.observe_owned_storage(unbuilt_storage);
            Fixed missing(na<int>());
            missing.observe_owned_storage(unbuilt_storage);
            if (!unbuilt_storage.usage().complete()) {
                fail(name, "observing", "an adapter that built nothing reports storage it "
                                        "cannot account for");
            }
        });
        if (quiet.stopped) {
            fail(name, "constructing and observing without a call", "it stopped: " + quiet.what);
        }
    }
}

// The hand-derived values of a valid length on a test's own invented bars.
template <class Fixed, class Feed>
void control_values(const Site& site, int length, const std::vector<Bar>& bars,
                    const std::vector<double>& values) {
    const std::string row = call_name(site) + " with a length of " + std::to_string(length);
    Fixed fixed(length);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double got = Feed::compute(fixed, bars[i]);
        if (!same_value(got, values[i])) {
            fail(row, "bar " + std::to_string(i),
                 "the call answered " + number(got) + ", expected " + number(values[i]));
        }
    }
}

// For a call whose adapter wraps the Pine type of the call, not the
// fixed-length class of <pineforge/ta.hpp>: `values` are the Pine type's
// hand-derived values on bars where the two types differ, and the fixed call
// must answer them. `Class` is that fixed-length class; it must answer
// otherwise on at least one of the bars, or the row would decide nothing.
template <class Fixed, class Class, class Feed>
void pine_type_values(const Site& site, int length, const std::vector<Bar>& bars,
                      const std::vector<double>& values) {
    const std::string row = call_name(site) + " with a length of " + std::to_string(length) +
                            ", bars the Pine type decides";
    Fixed fixed(length);
    Class plain(length);
    bool decided = false;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double got = Feed::compute(fixed, bars[i]);
        if (!same_value(got, values[i])) {
            fail(row, "bar " + std::to_string(i),
                 "the call answered " + number(got) + ", expected the Pine type's " +
                     number(values[i]));
        }
        if (!same_value(Feed::compute(plain, bars[i]), values[i])) decided = true;
    }
    if (!decided) {
        fail(row, "all bars",
             "the fixed-length class answers the same values: the bars decide nothing");
    }
}

// Ends a test: 0 when no row failed.
inline int finish(const char* test) {
    if (failures() != 0) {
        std::printf("%s: %d row(s) failed\n", test, failures());
        return 1;
    }
    std::printf("%s: ok\n", test);
    return 0;
}

// The whole test of a function with one length. `Wrapped` is the type the
// adapter wraps. `zero_bar` is the bar of the zero-length rows, the only one
// a type without the adapter is given with a length of 0; `bar` is the bar of
// every other stop row; `control_*` are the test's own bars and their
// hand-derived values for a valid length.
template <class Fixed, class Wrapped, class Feed, class Twin>
int run_single_length(const char* test, const Site& site, const Bar& zero_bar, const Bar& bar,
                      int control_length, const std::vector<Bar>& control_bars,
                      const std::vector<double>& control) {
    zero_length_rows<Fixed, Feed>(site, zero_bar);
    simple_length_row(site);
    if (failures() != 0) {
        std::printf("%s: %d stop row(s) failed\n", test, failures());
        return 1;
    }
    invalid_lengths_stop<Fixed, Feed>(site, bar);
    same_stop_as_the_other_paths<Fixed, Feed, Twin>(site, bar);
    valid_length_is_the_wrapped_type<Fixed, Wrapped, Feed>(site);
    shapes<Fixed, Wrapped, Feed>(site, bar);
    control_values<Fixed, Feed>(site, control_length, control_bars, control);
    return finish(test);
}

}  // namespace checked_fixed_length
