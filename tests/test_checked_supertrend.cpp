// The checked fixed-argument supertrend adapter of
// <pineforge/source/pine_ta_length.hpp> (PineFixedSupertrend): a
// ta.supertrend call whose factor and atrPeriod are constants or inputs stops
// on its first execution exactly as a call whose factor is not a constant does
// (PineSupertrend) -- the same coded pine_invalid_argument error, the same
// text, the same bar -- and for a valid pair every line and direction equals
// ta::Supertrend's bit for bit. Construction never stops a run.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/run_failure.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

using pineforge::na;
using pineforge::OwnedStorage;
using pineforge::RunFailureCode;
using pineforge::RunFailureInfo;
namespace ta = pineforge::ta;
using pineforge::source::PineSupertrend;

// The type a ta.supertrend call with a constant or input factor and atrPeriod
// is built with. A header without the checked adapter leaves the class such a
// call was built with before the adapter existed, ta::Supertrend: there the
// stop rows below fail, because that class takes the arguments and computes.
#ifdef PINEFORGE_HAS_PINE_FIXED_SUPERTREND
using FixedSupertrend = pineforge::source::PineFixedSupertrend;
#else
using FixedSupertrend = ta::Supertrend;
#endif

namespace {

struct Bar {
    double high, low, close;
};

// Five invented bars: three flat ones, then a step up of 2.
const Bar kBars[] = {{12.0, 8.0, 10.0}, {12.0, 8.0, 10.0}, {12.0, 8.0, 10.0},
                     {14.0, 10.0, 12.0}, {14.0, 10.0, 12.0}};

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// Bit identity; any two NaNs count as the same na.
void same(double actual, double expected) {
    if (std::isnan(expected)) {
        assert(std::isnan(actual));
        return;
    }
    assert(!std::isnan(actual));
    assert(bits(actual) == bits(expected));
}

void same(const ta::SupertrendResult& actual, const ta::SupertrendResult& expected) {
    same(actual.value, expected.value);
    same(actual.direction, expected.direction);
}

std::string number(double value) {
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

// TradingView's texts as PineSupertrend raises them
// (src/source/pine_ta_length.cpp: factor_not_positive, simple_ta_length).
std::string positive_text(const char* argument, const char* value) {
    return std::string("Invalid value of the '") + argument + "' argument (" + value +
           ") in the 'supertrend' function. It must be > 0.";
}
std::string na_text(const char* argument) {
    return std::string("Invalid value of the '") + argument +
           "' argument in the 'supertrend' function. It must not be na";
}

// --- the stop rows: counted, not asserted, so that every call that computes
// where it must stop is named ---

int failures = 0;

void fail(const char* row, const char* context, const std::string& why) {
    std::printf("FAIL %s, %s: %s\n", row, context, why.c_str());
    ++failures;
}

struct Stop {
    const char* row;       // the Pine call
    double factor;
    int period;
    const char* argument;  // the argument the stop names
    const char* shown;     // its value in the text
};

const Stop kStops[] = {
    {"ta.supertrend(0.0, 2)", 0.0, 2, "factor", "0"},
    {"ta.supertrend(1.0, 0)", 1.0, 0, "atrPeriod", "0"},
    {"ta.supertrend(-1.0, 2)", -1.0, 2, "factor", "-1"},
    {"ta.supertrend(1.0, -1)", 1.0, -1, "atrPeriod", "-1"},
    {"ta.supertrend(-0.5, 2)", -0.5, 2, "factor", "-0.5"},
    // Both invalid: atrPeriod is checked first, as PineSupertrend checks it.
    {"ta.supertrend(0.0, 0)", 0.0, 0, "atrPeriod", "0"},
};

// One fixed call on its first execution, by compute() or by recompute(). It
// must stop with `prefix` + the argument's text, coded pine_invalid_argument
// with the function, argument and rule; the call with the same two values on
// the path of a factor that is not a constant must raise the very same error;
// and the failed check must build nothing, so the next call stops again.
void stop_row(const Stop& stop, const char* context, const char* prefix, bool again) {
    const Bar& bar = kBars[0];
    const std::string text = prefix + positive_text(stop.argument, stop.shown);
    FixedSupertrend fixed(stop.factor, stop.period);  // constructing never stops
    ta::SupertrendResult got{na<double>(), na<double>()};
    const Outcome first = outcome([&] {
        got = again ? fixed.recompute(bar.high, bar.low, bar.close)
                    : fixed.compute(bar.high, bar.low, bar.close);
    });
    if (!first.stopped) {
        fail(stop.row, context,
             "the call computed line " + number(got.value) + ", direction " +
                 number(got.direction) + "; expected the stop \"" + text + "\"");
        return;
    }
    if (!first.coded || first.code != RunFailureCode::pine_invalid_argument) {
        fail(stop.row, context, "the stop is not coded pine_invalid_argument: " + first.what);
    }
    if (first.what != text) {
        fail(stop.row, context, "the stop reads \"" + first.what + "\"; expected \"" + text + "\"");
    }
    const std::string argument = std::string("\"argument\":\"") + stop.argument + "\"";
    if (first.args.find(argument) == std::string::npos ||
        first.args.find("\"function\":\"supertrend\"") == std::string::npos ||
        first.args.find("\"rule\":\"must_be_positive\"") == std::string::npos) {
        fail(stop.row, context, "the coded arguments read " + first.args);
    }

    PineSupertrend series;
    const Outcome other = outcome([&] {
        if (again) series.recompute(stop.factor, stop.period, bar.high, bar.low, bar.close);
        else series.compute(stop.factor, stop.period, bar.high, bar.low, bar.close);
    });
    if (!other.stopped || other.code != first.code || other.what != first.what ||
        other.args != first.args) {
        fail(stop.row, context,
             "the path of a factor that is not a constant raises \"" + other.what + "\" " +
                 other.args + ", the fixed call \"" + first.what + "\" " + first.args);
    }

    const Outcome second = outcome([&] { fixed.compute(bar.high, bar.low, bar.close); });
    if (!second.stopped || second.what != text) {
        fail(stop.row, context, "the call after a failed check did not stop again");
    }
}

void stop_rows() {
    for (const Stop& stop : kStops) {
        for (bool again : {false, true}) {
            stop_row(stop, again ? "recompute, no bar context" : "compute, no bar context",
                     "Error before the first bar: ", again);
            {
                // The first chart bar.
                ta::BarContextScope scope(0, 0);
                stop_row(stop, again ? "recompute on bar 0" : "compute on bar 0",
                         "Error on bar 0: ", again);
            }
            {
                // A call first reached on a later bar stops there, not before.
                ta::BarContextScope scope(2, 0);
                stop_row(stop, again ? "recompute first reached on bar 2"
                                     : "compute first reached on bar 2",
                         "Error on bar 2: ", again);
            }
        }
    }
}

// --- the rows below run once every stop row passed ---

// Runs `call` and requires the coded invalid-argument stop with `text`.
template <class Call>
void stops(Call call, const std::string& text) {
    const Outcome got = outcome(call);
    assert(got.stopped);
    assert(got.coded);
    assert(got.code == RunFailureCode::pine_invalid_argument);
    assert(got.what == text);
}

// A deterministic sequence with na gaps: bar i has
// base = 100 + 0.75 i + (i % 7) * 1.25 - (i % 3) * 2, high = base + 1.5 +
// (i % 4) * 0.5, low = base - 1 - (i % 5) * 0.25, close = base +- 0.5; the
// close is na on every bar with i % 9 == 4 and the high on bar 0.
std::vector<Bar> sequence() {
    std::vector<Bar> out;
    for (int i = 0; i < 48; ++i) {
        const double base = 100.0 + 0.75 * i + (i % 7) * 1.25 - (i % 3) * 2.0;
        Bar bar{base + 1.5 + (i % 4) * 0.5, base - 1.0 - (i % 5) * 0.25,
                base + ((i % 2) ? 0.5 : -0.5)};
        if (i % 9 == 4) bar.close = na<double>();
        if (i == 0) bar.high = na<double>();
        out.push_back(bar);
    }
    return out;
}

// One bar: compute, a recompute of the same bar with a moved high, then the
// recompute with the bar's own values (the committed one).
template <class Adapter>
void same_bar(Adapter& adapter, ta::Supertrend& legacy, const Bar& bar) {
    same(adapter.compute(bar.high, bar.low, bar.close),
         legacy.compute(bar.high, bar.low, bar.close));
    same(adapter.recompute(bar.high + 3.5, bar.low, bar.close),
         legacy.recompute(bar.high + 3.5, bar.low, bar.close));
    same(adapter.recompute(bar.high, bar.low, bar.close),
         legacy.recompute(bar.high, bar.low, bar.close));
}

// A valid pair computes the class's own values, on the five bars and on the
// sequence with na gaps.
void valid_pair_is_the_class() {
    {
        FixedSupertrend adapter(1.0, 2);
        ta::Supertrend legacy(1.0, 2);
        for (const Bar& bar : kBars) same_bar(adapter, legacy, bar);
    }
    FixedSupertrend adapter(2.5, 7);
    ta::Supertrend legacy(2.5, 7);
    for (const Bar& bar : sequence()) same_bar(adapter, legacy, bar);
}

// An na factor is accepted, as PineSupertrend accepts it, and reads as na.
void na_factor_is_accepted() {
    FixedSupertrend adapter(na<double>(), 2);
    ta::Supertrend legacy(na<double>(), 2);
    for (const Bar& bar : kBars) same_bar(adapter, legacy, bar);
}

// An na atrPeriod stops the first call with simple_ta_length's na text, as
// an int na (the integer sentinel, not a NaN) and as a double one.
void na_period_stops() {
    const Bar& bar = kBars[0];
    const std::string text = "Error before the first bar: " + na_text("atrPeriod");
    FixedSupertrend from_int(1.0, na<int>());
    stops([&] { from_int.compute(bar.high, bar.low, bar.close); }, text);
    FixedSupertrend from_double(1.0, na<double>());
    stops([&] { from_double.recompute(bar.high, bar.low, bar.close); }, text);
}

// The argument types generated code passes: an int or int64 factor, an int64
// or double atrPeriod (a float period truncates toward zero, as
// simple_ta_length truncates it).
void argument_types() {
    const std::vector<Bar> seq = sequence();
    FixedSupertrend int_factor(3, 5);
    FixedSupertrend wide(static_cast<std::int64_t>(3), static_cast<std::int64_t>(5));
    FixedSupertrend real_period(3.0, 5.9);
    ta::Supertrend a(3.0, 5), b(3.0, 5), c(3.0, 5);
    for (const Bar& bar : seq) {
        same_bar(int_factor, a, bar);
        same_bar(wide, b, bar);
        same_bar(real_period, c, bar);
    }
}

// The drop-in shapes of generated code.
void shapes() {
    const std::vector<Bar> seq = sequence();
    const std::string zero_factor = "Error before the first bar: " + positive_text("factor", "0");

    // A recompute as the very first call reaches the class's pristine state.
    {
        FixedSupertrend adapter(2.5, 7);
        ta::Supertrend legacy(2.5, 7);
        for (int i = 0; i < 5; ++i) {
            same(adapter.recompute(seq[i].high, seq[i].low, seq[i].close),
                 legacy.recompute(seq[i].high, seq[i].low, seq[i].close));
        }
        for (const Bar& bar : seq) same_bar(adapter, legacy, bar);
    }

    // The generated reset `member = Type(args)` after use: no state survives
    // it, a reset to an invalid factor stops instead of answering from the
    // old object, and a reset back to valid starts again from bar 0.
    {
        FixedSupertrend member(2.5, 7);
        {
            ta::Supertrend legacy(2.5, 7);
            for (const Bar& bar : seq) same_bar(member, legacy, bar);
        }
        member = FixedSupertrend(0.0, 7);
        stops([&] { member.compute(seq[1].high, seq[1].low, seq[1].close); }, zero_factor);
        stops([&] { member.recompute(seq[1].high, seq[1].low, seq[1].close); }, zero_factor);
        member = FixedSupertrend(2.5, 7);
        ta::Supertrend fresh(2.5, 7);
        for (const Bar& bar : seq) same_bar(member, fresh, bar);
    }

    // Copies between calls each continue as an independent copy of the class;
    // one copy runs ahead first, so a copy sharing state would move the
    // original. Copies of an unbuilt invalid adapter each stop.
    {
        const int split = 20;
        const int ahead = 26;
        const int count = static_cast<int>(seq.size());
        FixedSupertrend original(2.5, 7);
        ta::Supertrend legacy(2.5, 7);
        for (int i = 0; i < split; ++i) same_bar(original, legacy, seq[i]);
        FixedSupertrend constructed(original);
        ta::Supertrend constructed_legacy(legacy);
        FixedSupertrend assigned(1.0, 3);
        assigned.compute(seq[1].high, seq[1].low, seq[1].close);  // built, then overwritten
        assigned = original;
        ta::Supertrend assigned_legacy(legacy);
        for (int i = split; i < ahead; ++i) same_bar(constructed, constructed_legacy, seq[i]);
        for (int i = split; i < count; ++i) same_bar(original, legacy, seq[i]);
        for (int i = split; i < count; ++i) same_bar(assigned, assigned_legacy, seq[i]);
        for (int i = ahead; i < count; ++i) same_bar(constructed, constructed_legacy, seq[i]);

        FixedSupertrend invalid(0.0, 7);
        FixedSupertrend invalid_constructed(invalid);
        FixedSupertrend invalid_assigned(2.5, 7);
        invalid_assigned = invalid;
        stops([&] { invalid.compute(seq[1].high, seq[1].low, seq[1].close); }, zero_factor);
        stops([&] { invalid_constructed.recompute(seq[1].high, seq[1].low, seq[1].close); },
              zero_factor);
        stops([&] { invalid_assigned.compute(seq[1].high, seq[1].low, seq[1].close); },
              zero_factor);
    }

    // Constructing and observing never stop, whatever the arguments.
    {
        OwnedStorage observed;
        FixedSupertrend valid(2.5, 7);
        valid.compute(seq[1].high, seq[1].low, seq[1].close);
        valid.observe_owned_storage(observed);
        FixedSupertrend unbuilt(2.5, 7);
        unbuilt.observe_owned_storage(observed);
        FixedSupertrend invalid(0.0, 0);
        invalid.observe_owned_storage(observed);
        assert(observed.usage().complete());
    }
}

}  // namespace

int main() {
    stop_rows();
    if (failures != 0) {
        std::printf("%d stop row(s) failed\n", failures);
        return 1;
    }
    valid_pair_is_the_class();
    na_factor_is_accepted();
    na_period_stops();
    argument_types();
    shapes();
    std::printf("ok\n");
    return 0;
}
