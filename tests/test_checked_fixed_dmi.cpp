// Fixed-length ta.dmi (source::PineFixedDmi): a diLength or an adxSmoothing of
// 0, a negative one or an na one, as a constant or an input, stops the call's
// first execution with TradingView's error, which names the two arguments
// 'DI Length' and 'ADX Smoothing' and words the second its own way:
//
//   Invalid value of the 'DI Length' argument (0) in the 'dmi' function. It
//   must be > 0.
//   Invalid value of the 'ADX Smoothing' argument (0) in the 'dmi' function.
//   It must be greater than 0.
//
// A simple length of the same value raises the same coded failure and keeps
// the released text, which names the two arguments as the call does and words
// both with "> 0":
//
//   Invalid value of the 'diLength' argument (0) in the 'dmi' function. It
//   must be > 0.
//   Invalid value of the 'adxSmoothing' argument (0) in the 'dmi' function.
//   It must be > 0.
//
// A valid pair computes the values of source::PineDmiReference, the Pine type
// a ta.dmi call is built with: ta::DMI's values, except that both indices
// keep their last values on a bar whose averaged true range is zero.
// PineDmiReference itself takes the two lengths and computes, as ta::DMI
// does: with ta.dmi(1, 0) it answers both directional indices beside an na
// ADX, with ta.dmi(0, 1) three na.
//
// The helpers and the order of the rows are those of
// tests/checked_fixed_length_rows.hpp: the stop rows run first, are counted
// and are the only ones a type without the adapter is given an invalid
// length for.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.dmi call with valid lengths answers as.
using Wrapped = pineforge::source::PineDmiReference;

// The type a ta.dmi call with constant or input lengths is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedDmi;
#else
using Fixed = Wrapped;
#endif

// The call is ta.dmi(diLength, adxSmoothing); TradingView's text names the
// two 'DI Length' and 'ADX Smoothing', and the second with "greater than".
const Site kDi{"dmi", "diLength", "'dmi'", "DI Length", "> 0"};
const Site kAdx{"dmi", "adxSmoothing", "'dmi'", "ADX Smoothing", "greater than 0"};

// Five invented bars (high, low, close), each one higher than the last: every
// move has +DM 1, -DM 0 and a true range of 2.
const Bar kBars[] = {{10, 8, 9, 1}, {11, 9, 10, 1}, {12, 10, 11, 1}, {13, 11, 12, 1},
                     {14, 12, 13, 1}};

std::string triple(const ta::DMIResult& result) {
    return "(+DI " + number(result.diplus) + ", -DI " + number(result.diminus) + ", ADX " +
           number(result.adx) + ")";
}

bool same_triple(const ta::DMIResult& a, const ta::DMIResult& b) {
    return same_bits(a.diplus, b.diplus) && same_bits(a.diminus, b.diminus) &&
           same_bits(a.adx, b.adx);
}

// Numeric equality of the three fields; na matches na only.
bool same_values(const ta::DMIResult& a, const ta::DMIResult& b) {
    return same_value(a.diplus, b.diplus) && same_value(a.diminus, b.diminus) &&
           same_value(a.adx, b.adx);
}

template <class Call>
ta::DMIResult compute(Call& call, const Bar& bar) {
    return call.compute(bar.high, bar.low, bar.close);
}
template <class Call>
ta::DMIResult recompute(Call& call, const Bar& bar) {
    return call.recompute(bar.high, bar.low, bar.close);
}

// --- the stop rows ---

struct Zero {
    const char* row;   // the Pine call
    int di_length;
    int adx_smoothing;
    const Site* site;  // the argument the stop names
};

const Zero kZeros[] = {
    {"ta.dmi(1, 0)", 1, 0, &kAdx},
    {"ta.dmi(0, 1)", 0, 1, &kDi},
};

// One fixed call with one length of 0 on its first execution. It must stop
// with `prefix` + that argument's text, and the failed check must build
// nothing, so the next call stops again.
void zero_row(const Zero& zero, const char* context, const char* prefix) {
    const std::string text = prefix + positive_text(*zero.site, "0");
    Fixed fixed(zero.di_length, zero.adx_smoothing);  // constructing never stops
    ta::DMIResult got{na<double>(), na<double>(), na<double>()};
    const Outcome first = outcome([&] { got = compute(fixed, kBars[0]); });
    if (!first.stopped) {
        // The call ran: the other four bars show what it goes on to answer.
        for (int i = 1; i < 5; ++i) got = compute(fixed, kBars[i]);
        fail(zero.row, context,
             "the call computed and answers " + triple(got) +
                 " on the fifth bar; expected the stop \"" + text + "\"");
        return;
    }
    expect_stop(first, zero.row, context, text, *zero.site, "must_be_positive");
    const Outcome second = outcome([&] { compute(fixed, kBars[0]); });
    if (!second.stopped || second.what != text) {
        fail(zero.row, context, "the call after a failed check did not stop again");
    }
}

void zero_rows() {
    for (const Zero& zero : kZeros) {
        zero_row(zero, "compute, no bar context", "Error before the first bar: ");
        {
            // The first chart bar.
            ta::BarContextScope scope(0, 0);
            zero_row(zero, "compute on bar 0", "Error on bar 0: ");
        }
        {
            // A call first reached on a later bar stops there, not before.
            ta::BarContextScope scope(2, 0);
            zero_row(zero, "compute first reached on bar 2", "Error on bar 2: ");
        }
    }
}

// --- the rows below run once every stop row passed ---

// A call built with the two lengths stops its first execution, by compute()
// and by recompute(), with the text of `site`, and stops again on the next
// call.
template <class Di, class Adx>
void stops_with(Di di_length, Adx adx_smoothing, const std::string& row, const Site& site,
                const std::string& text, const char* rule) {
    const std::string full = "Error before the first bar: " + text;
    for (bool again : {false, true}) {
        Fixed fixed(di_length, adx_smoothing);
        const Outcome first = outcome([&] {
            if (again) recompute(fixed, kBars[0]);
            else compute(fixed, kBars[0]);
        });
        expect_stop(first, row, again ? "recompute as the first call" : "compute", full, site,
                    rule);
        const Outcome second = outcome([&] { recompute(fixed, kBars[0]); });
        expect_stop(second, row, "the call after a failed check", full, site, rule);
    }
}

void invalid_lengths_stop() {
    stops_with(1, 0, "ta.dmi(1, 0)", kAdx, positive_text(kAdx, "0"), "must_be_positive");
    stops_with(0, 1, "ta.dmi(0, 1)", kDi, positive_text(kDi, "0"), "must_be_positive");
    stops_with(-1, 2, "ta.dmi(-1, 2)", kDi, positive_text(kDi, "-1"), "must_be_positive");
    stops_with(2, -1, "ta.dmi(2, -1)", kAdx, positive_text(kAdx, "-1"), "must_be_positive");
    // Both invalid: diLength is checked first, the signature's order.
    stops_with(0, 0, "ta.dmi(0, 0)", kDi, positive_text(kDi, "0"), "must_be_positive");
    stops_with(static_cast<std::int64_t>(0), 2, "ta.dmi(0, 2) with an int64 diLength", kDi,
               positive_text(kDi, "0"), "must_be_positive");
    // A float length truncates toward zero before the check.
    stops_with(0.5, 2, "ta.dmi with a diLength of 0.5", kDi, positive_text(kDi, "0"),
               "must_be_positive");
    stops_with(2, 0.5, "ta.dmi with an adxSmoothing of 0.5", kAdx, positive_text(kAdx, "0"),
               "must_be_positive");
    // An na length, as the integer sentinel and as a NaN.
    stops_with(na<int>(), 2, "ta.dmi with an int na diLength", kDi, na_text(kDi),
               "must_not_be_na");
    stops_with(2, na<double>(), "ta.dmi with a double na adxSmoothing", kAdx, na_text(kAdx),
               "must_not_be_na");
}

// The stop is the coded failure a simple length of the same value raises: the
// same code and arguments. The two texts differ for both arguments (the
// adapter's is TradingView's, the simple length's the released one), and
// invalid_lengths_stop and simple_length_row hold each to its own.
void same_failure_as_a_simple_length() {
    for (const Zero& zero : kZeros) {
        Fixed fixed(zero.di_length, zero.adx_smoothing);
        const Outcome got = outcome([&] { compute(fixed, kBars[0]); });
        const Outcome simple = outcome([&] {
            pineforge::source::simple_ta_length(0, zero.site->function, zero.site->argument);
        });
        if (plain_wording(*zero.site)) {
            expect_same_stop(got, simple, zero.row, "against a simple length");
        } else {
            expect_same_failure(got, simple, zero.row, "against a simple length");
        }
    }
}

// One bar on both objects: compute, a recompute of the same bar moved, then
// the recompute with the bar's own values (the committed one).
void same_bar(Fixed& fixed, Wrapped& wrapped, const Bar& bar, const std::string& row,
              std::size_t index) {
    const Bar other = moved(bar);
    const ta::DMIResult got[3] = {compute(fixed, bar), recompute(fixed, other),
                                  recompute(fixed, bar)};
    const ta::DMIResult want[3] = {compute(wrapped, bar), recompute(wrapped, other),
                                   recompute(wrapped, bar)};
    const char* const steps[3] = {"compute", "recompute of the moved bar",
                                  "recompute of the bar's own values"};
    for (int step = 0; step < 3; ++step) {
        if (!same_triple(got[step], want[step])) {
            fail(row, std::string(steps[step]) + " on bar " + std::to_string(index),
                 "the adapter answered " + triple(got[step]) + ", the wrapped type " +
                     triple(want[step]));
        }
    }
}

// A valid pair computes the wrapped type's own values, bit for bit, on the
// sequence with na gaps; int, int64 and double lengths.
void valid_pair_is_the_wrapped_type() {
    const std::vector<Bar> seq = sequence();
    {
        Fixed fixed(7, 5);
        Wrapped wrapped(7, 5);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar(fixed, wrapped, seq[i], "ta.dmi(7, 5)", i);
    }
    {
        Fixed fixed(static_cast<std::int64_t>(5), 3.9);
        Wrapped wrapped(5, 3);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar(fixed, wrapped, seq[i], "ta.dmi with an int64 5 and a float 3.9", i);
    }
    {
        Fixed fixed(1, 1);
        Wrapped wrapped(1, 1);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar(fixed, wrapped, seq[i], "ta.dmi(1, 1)", i);
    }
}

// The drop-in shapes of generated code.
void shapes() {
    const std::vector<Bar> seq = sequence();
    const std::string zero_adx = "Error before the first bar: " + positive_text(kAdx, "0");

    // A recompute as the very first call reaches the wrapped type's pristine
    // state.
    {
        Fixed fixed(7, 5);
        Wrapped wrapped(7, 5);
        for (std::size_t i = 0; i < 5; ++i) {
            const ta::DMIResult got = recompute(fixed, seq[i]);
            const ta::DMIResult want = recompute(wrapped, seq[i]);
            if (!same_triple(got, want)) {
                fail("ta.dmi(7, 5)", "recompute before any compute, bar " + std::to_string(i),
                     "the adapter answered " + triple(got) + ", the wrapped type " +
                         triple(want));
            }
        }
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar(fixed, wrapped, seq[i], "ta.dmi(7, 5) after recomputes as first calls", i);
    }

    // The generated reset `member = Type(lengths)` after use: no state
    // survives it, a reset to an invalid length stops instead of answering
    // from the old object, and a reset back to valid starts again.
    {
        Fixed member(7, 5);
        {
            Wrapped wrapped(7, 5);
            for (std::size_t i = 0; i < seq.size(); ++i)
                same_bar(member, wrapped, seq[i], "ta.dmi(7, 5) before a reset", i);
        }
        member = Fixed(1, 0);
        expect_stop(outcome([&] { compute(member, kBars[0]); }), "ta.dmi",
                    "compute after a reset to (1, 0)", zero_adx, kAdx, "must_be_positive");
        expect_stop(outcome([&] { recompute(member, kBars[0]); }), "ta.dmi",
                    "recompute after a reset to (1, 0)", zero_adx, kAdx, "must_be_positive");
        member = Fixed(7, 5);
        Wrapped fresh(7, 5);
        for (std::size_t i = 0; i < seq.size(); ++i)
            same_bar(member, fresh, seq[i], "ta.dmi(7, 5) after a reset", i);
    }

    // Copies between calls each continue as an independent copy of the
    // wrapped type; one copy runs ahead first. Copies of an unbuilt invalid
    // adapter each stop.
    {
        const std::size_t split = 20;
        const std::size_t ahead = 26;
        Fixed original(7, 5);
        Wrapped wrapped(7, 5);
        for (std::size_t i = 0; i < split; ++i)
            same_bar(original, wrapped, seq[i], "ta.dmi(7, 5) before a copy", i);
        Fixed constructed(original);
        Wrapped constructed_wrapped(wrapped);
        Fixed assigned(3, 2);
        compute(assigned, seq[1]);  // built, then overwritten
        assigned = original;
        Wrapped assigned_wrapped(wrapped);
        for (std::size_t i = split; i < ahead; ++i)
            same_bar(constructed, constructed_wrapped, seq[i],
                     "ta.dmi(7, 5), copy-constructed copy running ahead", i);
        for (std::size_t i = split; i < seq.size(); ++i)
            same_bar(original, wrapped, seq[i], "ta.dmi(7, 5), the original", i);
        for (std::size_t i = split; i < seq.size(); ++i)
            same_bar(assigned, assigned_wrapped, seq[i], "ta.dmi(7, 5), copy-assigned copy", i);
        for (std::size_t i = ahead; i < seq.size(); ++i)
            same_bar(constructed, constructed_wrapped, seq[i],
                     "ta.dmi(7, 5), copy-constructed copy", i);

        Fixed invalid(1, 0);
        Fixed invalid_constructed(invalid);
        Fixed invalid_assigned(7, 5);
        invalid_assigned = invalid;
        expect_stop(outcome([&] { compute(invalid, kBars[0]); }), "ta.dmi",
                    "an unbuilt adapter with (1, 0)", zero_adx, kAdx, "must_be_positive");
        expect_stop(outcome([&] { recompute(invalid_constructed, kBars[0]); }), "ta.dmi",
                    "a copy-constructed copy of it", zero_adx, kAdx, "must_be_positive");
        expect_stop(outcome([&] { compute(invalid_assigned, kBars[0]); }), "ta.dmi",
                    "a copy-assigned copy of it", zero_adx, kAdx, "must_be_positive");
    }

    // Constructing, observing and destroying never stop, whatever the
    // lengths: a call site that is never reached does not stop a run. A built
    // adapter reports its object's storage as completely as the wrapped type
    // does.
    {
        const Outcome quiet = outcome([&] {
            OwnedStorage built;
            Fixed valid(7, 5);
            compute(valid, seq[1]);
            valid.observe_owned_storage(built);
            OwnedStorage plain;
            Wrapped wrapped(7, 5);
            compute(wrapped, seq[1]);
            wrapped.observe_owned_storage(plain);
            if (built.usage().complete() != plain.usage().complete()) {
                fail("ta.dmi", "observing",
                     "the adapter's observation and the wrapped type's differ in completeness");
            }
            OwnedStorage unbuilt_storage;
            Fixed unbuilt(7, 5);
            unbuilt.observe_owned_storage(unbuilt_storage);
            Fixed invalid(0, 0);
            invalid.observe_owned_storage(unbuilt_storage);
            Fixed negative(-4, 2);
            negative.observe_owned_storage(unbuilt_storage);
            Fixed missing(2, na<int>());
            missing.observe_owned_storage(unbuilt_storage);
            if (!unbuilt_storage.usage().complete()) {
                fail("ta.dmi", "observing", "an adapter that built nothing reports storage it "
                                            "cannot account for");
            }
        });
        if (quiet.stopped) {
            fail("ta.dmi", "constructing and observing without a call",
                 "it stopped: " + quiet.what);
        }
    }
}

// ta.dmi(1, 1) on the five bars: three na on the first bar, which only sets
// the previous high, low and close; then +DI = 100 * 1 / 2, -DI = 0 and
// ADX = 100 * |50 - 0| / 50 on every bar.
void control_values() {
    Fixed fixed(1, 1);
    for (std::size_t i = 0; i < 5; ++i) {
        const ta::DMIResult got = compute(fixed, kBars[i]);
        const bool ok = i == 0
            ? std::isnan(got.diplus) && std::isnan(got.diminus) && std::isnan(got.adx)
            : got.diplus == 50.0 && got.diminus == 0.0 && got.adx == 100.0;
        if (!ok) {
            fail("ta.dmi(1, 1)", "bar " + std::to_string(i),
                 "the call answered " + triple(got) + ", expected " +
                     (i == 0 ? std::string("(+DI na, -DI na, ADX na)")
                             : std::string("(+DI 50, -DI 0, ADX 100)")));
        }
    }
}

// Bars the Pine type decides, ta.dmi(1, 1): the adapter wraps
// PineDmiReference, not ta::DMI. Bar 2 has high = low = bar 1's close, so
// its true range is zero and both quotients are 0 / 0.
//
//   bar 1  +DM 1 over a range of 2: +DI 50, -DI 0, ADX 100.
//   bar 2  no range: the Pine type keeps bar 1's pair (50, 0) and gives ADX
//          one more step from it, 100. ta::DMI answers three na here.
//   bar 3  +DM 2 over a range of 2: +DI 100, -DI 0, ADX 100.
//   bar 4  +DM 1 over a range of 2: +DI 50, -DI 0, ADX 100.
void pine_type_values() {
    const double none = na<double>();
    const Bar bars[] = {{10, 8, 9, 1}, {11, 9, 10, 1}, {10, 10, 10, 1}, {12, 10, 11, 1},
                        {13, 11, 12, 1}};
    const ta::DMIResult values[] = {{none, none, none}, {50.0, 0.0, 100.0}, {50.0, 0.0, 100.0},
                                    {100.0, 0.0, 100.0}, {50.0, 0.0, 100.0}};
    const std::string row = "ta.dmi(1, 1), bars the Pine type decides";
    Fixed fixed(1, 1);
    ta::DMI plain(1, 1);
    bool decided = false;
    for (std::size_t i = 0; i < 5; ++i) {
        const ta::DMIResult got = compute(fixed, bars[i]);
        if (!same_values(got, values[i])) {
            fail(row, "bar " + std::to_string(i),
                 "the call answered " + triple(got) + ", expected the Pine type's " +
                     triple(values[i]));
        }
        if (!same_values(compute(plain, bars[i]), values[i])) decided = true;
    }
    if (!decided) {
        fail(row, "all bars",
             "the fixed-length class answers the same values: the bars decide nothing");
    }
}

}  // namespace

int main() {
    zero_rows();
    // The same arguments as simple lengths keep the released text.
    simple_length_row(kDi);
    simple_length_row(kAdx);
    if (failures() != 0) {
        std::printf("checked_fixed_dmi: %d stop row(s) failed\n", failures());
        return 1;
    }
    invalid_lengths_stop();
    same_failure_as_a_simple_length();
    valid_pair_is_the_wrapped_type();
    shapes();
    control_values();
    pine_type_values();
    return finish("checked_fixed_dmi");
}
