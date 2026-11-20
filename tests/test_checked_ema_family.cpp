// The checked EMA-family source adapters of <pineforge/source/pine_ta_length.hpp>
// (PineEma, PineTsi, PineMacd, PineKc, PineKcw; codegen #291): for a valid
// length every result equals the fixed-length class of <pineforge/ta.hpp> bit
// for bit; an invalid length (na, 0, negative) never stops construction and
// stops the first compute() or recompute() with simple_ta_length's coded
// pine_invalid_argument error and TradingView's text.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/run_failure.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using pineforge::na;
using pineforge::OwnedStorage;
using pineforge::RunFailureCode;
using pineforge::RunFailureInfo;
namespace ta = pineforge::ta;
using pineforge::source::PineEma;
using pineforge::source::PineKc;
using pineforge::source::PineKcw;
using pineforge::source::PineMacd;
using pineforge::source::PineTsi;

namespace {

constexpr int kBars = 48;

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

void same(const ta::MACDResult& a, const ta::MACDResult& e) {
    same(a.macd_line, e.macd_line);
    same(a.signal_line, e.signal_line);
    same(a.histogram, e.histogram);
}

void same(const ta::KCResult& a, const ta::KCResult& e) {
    same(a.middle, e.middle);
    same(a.upper, e.upper);
    same(a.lower, e.lower);
}

// A deterministic bar sequence with na gaps: bar i has source
// 100 + 0.75 i + (i % 7) * 1.25 - (i % 3) * 2, na on every bar with i % 9 == 4
// and on bar 0's high; high = source + 1.5 + (i % 4) * 0.5,
// low = source - 1 - (i % 5) * 0.25, close = source + ((i % 2) ? 0.5 : -0.5).
// The Keltner rows recompute with high + d, so bar 0's na high also feeds
// their recompute, as intended.
struct Sample {
    double src, high, low, close;
};

std::vector<Sample> bars() {
    std::vector<Sample> out;
    for (int i = 0; i < kBars; ++i) {
        const double base = 100.0 + 0.75 * i + (i % 7) * 1.25 - (i % 3) * 2.0;
        Sample bar{base, base + 1.5 + (i % 4) * 0.5, base - 1.0 - (i % 5) * 0.25,
                base + ((i % 2) ? 0.5 : -0.5)};
        if (i % 9 == 4) bar.src = na<double>();
        if (i == 0) bar.high = na<double>();
        out.push_back(bar);
    }
    return out;
}

// Each bar: compute, then a recompute of the same bar with a moved source,
// then the recompute with the bar's own source (the committed value).
template <class Adapter, class Legacy, class Call>
void value_identity(Adapter& adapter, Legacy& legacy, Call call) {
    for (const Sample& bar : bars()) {
        same(call(adapter, bar, false, 0.0), call(legacy, bar, false, 0.0));
        same(call(adapter, bar, true, 3.5), call(legacy, bar, true, 3.5));
        same(call(adapter, bar, true, 0.0), call(legacy, bar, true, 0.0));
    }
}

// TradingView's texts as pinned at simple_ta_length
// (include/pineforge/source/pine_ta_length.hpp, RE10001 / RE10003).
std::string positive_text(const char* function, const char* argument, const char* value) {
    return std::string("Invalid value of the '") + argument + "' argument (" + value +
           ") in the '" + function + "' function. It must be > 0.";
}
std::string na_text(const char* function, const char* argument) {
    return std::string("Invalid value of the '") + argument + "' argument in the '" +
           function + "' function. It must not be na";
}

// Runs `call` and requires the coded invalid-argument stop with `text`.
template <class Call>
void stops(Call call, const std::string& text) {
    bool caught = false;
    try {
        call();
    } catch (const std::exception& error) {
        const auto* failure = dynamic_cast<const RunFailureInfo*>(&error);
        assert(failure != nullptr);
        assert(failure->run_failure_code() == RunFailureCode::pine_invalid_argument);
        assert(std::string(error.what()) == text);
        caught = true;
    }
    assert(caught);
}

// One invalid argument value: the text it must produce.
struct Invalid {
    double value;       // as a double argument
    bool int_na;        // also pass it as the int na
    const char* shown;  // the value in the text; nullptr = the na text
};

const Invalid kInvalid[] = {
    {na<double>(), true, nullptr},
    {0.0, false, "0"},
    {-1.0, false, "-1"},
    // Truncated toward zero by simple_ta_length: the engine's convention for a
    // float length, not a TradingView fact (a TradingView length is an int).
    {-2.5, false, "-2"},
};

// For one invalid argument: `make(value)` builds an adapter whose other
// lengths are valid; `run(adapter, again)` makes its first call.
template <class Make, class Run>
void refusal(Make make, Run run, const char* function, const char* argument) {
    for (const Invalid& row : kInvalid) {
        const std::string text =
            row.shown ? positive_text(function, argument, row.shown) : na_text(function, argument);
        for (bool again : {false, true}) {
            // Built from a double argument; and from the int argument where it
            // is an int (na<int>() is the integer sentinel, not a NaN).
            for (int form = 0; form < (row.int_na ? 2 : 1); ++form) {
                auto adapter = form == 0 ? make(row.value) : make(na<int>());
                stops([&] { run(adapter, again); }, text);
                stops([&] { run(adapter, false); }, text);  // fails again
                {
                    // Under an evaluation context the bar prefix is added.
                    ta::BarContextScope scope(17, 0);
                    auto fresh = form == 0 ? make(row.value) : make(na<int>());
                    stops([&] { run(fresh, again); }, "Error on bar 17: " + text);
                }
            }
        }
        if (row.value == -1.0) {
            // An int argument too.
            auto adapter = make(-1);
            stops([&] { run(adapter, false); }, text);
        }
    }
}

// One bar of value_identity: compute, a moved recompute, the committed one.
template <class Adapter, class Legacy, class Call>
void same_bar(Adapter& adapter, Legacy& legacy, Call call, const Sample& bar) {
    same(call(adapter, bar, false, 0.0), call(legacy, bar, false, 0.0));
    same(call(adapter, bar, true, 3.5), call(legacy, bar, true, 3.5));
    same(call(adapter, bar, true, 0.0), call(legacy, bar, true, 0.0));
}

// The drop-in shapes of generated code, for one adapter type and its legacy
// class built from the same `args`. `make(length)` builds an adapter whose
// first length is `length` and whose other arguments are valid; `function`
// and `argument` name that first length in the error text.
//
// (a) A recompute as the very first call reaches the legacy class's
// pristine state (src/ta_moving_averages.cpp:430): bars 0-4 (bar 4's source
// is na) are recomputed only, then the sequence runs compute-then-recompute.
template <class Adapter, class Legacy, class Call, class... Args>
void recompute_first(Call call, Args... args) {
    const std::vector<Sample> seq = bars();
    Adapter adapter(args...);
    Legacy legacy(args...);
    for (int i = 0; i < 5; ++i) {
        same(call(adapter, seq[i], true, 0.0), call(legacy, seq[i], true, 0.0));
    }
    for (const Sample& bar : seq) same_bar(adapter, legacy, call, bar);
}

// (b) The generated reset `member = Type(args)` after use: no state survives
// it, a reset to an invalid length stops instead of answering from the old
// object, and a reset back to valid starts again from bar 0.
template <class Adapter, class Legacy, class Call, class Make, class... Args>
void reset_after_use(Call call, Make make, const char* function, const char* argument,
                     Args... args) {
    const std::vector<Sample> seq = bars();
    Adapter member(args...);
    {
        Legacy legacy(args...);
        for (const Sample& bar : seq) same_bar(member, legacy, call, bar);
    }
    member = Adapter(args...);
    {
        Legacy fresh(args...);
        for (const Sample& bar : seq) same_bar(member, fresh, call, bar);
    }
    for (const Invalid& row : kInvalid) {
        const std::string text =
            row.shown ? positive_text(function, argument, row.shown) : na_text(function, argument);
        member = make(row.value);
        stops([&] { call(member, seq[0], false, 0.0); }, text);
        stops([&] { call(member, seq[0], true, 0.0); }, text);
        member = Adapter(args...);
        Legacy fresh(args...);
        for (const Sample& bar : seq) same_bar(member, fresh, call, bar);
    }
}

// (c) Copies between calls: taken after 20 bars (past every warm-up, with
// recomputes between them) by copy construction and by copy assignment over
// a built adapter, each continues as an independent copy of the legacy
// object; one copy runs ahead first, so a copy sharing state with its
// source would move the original. Copies of an unbuilt adapter each start
// from bar 0, and copies of an unbuilt invalid adapter each stop.
template <class Adapter, class Legacy, class Call, class Make, class... Args>
void copy_between_calls(Call call, Make make, const char* function, const char* argument,
                        Args... args) {
    const std::vector<Sample> seq = bars();
    const int split = 20;
    const int ahead = 26;
    Adapter original(args...);
    Legacy legacy(args...);
    for (int i = 0; i < split; ++i) same_bar(original, legacy, call, seq[i]);
    Adapter constructed(original);
    Legacy constructed_legacy(legacy);
    Adapter assigned(args...);
    call(assigned, seq[0], false, 0.0);  // built, then overwritten
    assigned = original;
    Legacy assigned_legacy(legacy);
    for (int i = split; i < ahead; ++i) {
        same_bar(constructed, constructed_legacy, call, seq[i]);
    }
    for (int i = split; i < kBars; ++i) same_bar(original, legacy, call, seq[i]);
    for (int i = split; i < kBars; ++i) same_bar(assigned, assigned_legacy, call, seq[i]);
    for (int i = ahead; i < kBars; ++i) {
        same_bar(constructed, constructed_legacy, call, seq[i]);
    }

    Adapter unbuilt(args...);
    Adapter unbuilt_constructed(unbuilt);
    Adapter unbuilt_assigned(args...);
    unbuilt_assigned = unbuilt;
    for (Adapter* adapter : {&unbuilt, &unbuilt_constructed, &unbuilt_assigned}) {
        Legacy fresh(args...);
        for (const Sample& bar : seq) same_bar(*adapter, fresh, call, bar);
    }

    const std::string text = positive_text(function, argument, "0");
    auto invalid = make(0.0);
    Adapter invalid_constructed(invalid);
    Adapter invalid_assigned(args...);
    invalid_assigned = invalid;
    stops([&] { call(invalid, seq[0], false, 0.0); }, text);
    stops([&] { call(invalid_constructed, seq[0], true, 0.0); }, text);
    stops([&] { call(invalid_assigned, seq[0], false, 0.0); }, text);
}

template <class Adapter, class Legacy, class Call, class Make, class... Args>
void shapes(Call call, Make make, const char* function, const char* argument, Args... args) {
    recompute_first<Adapter, Legacy>(call, args...);
    reset_after_use<Adapter, Legacy>(call, make, function, argument, args...);
    copy_between_calls<Adapter, Legacy>(call, make, function, argument, args...);
}

void ema() {
    ta::ema_na_warmup_flag() = false;
    PineEma adapter(13);
    ta::EMA legacy(13);
    value_identity(adapter, legacy, [](auto& o, const Sample& b, bool again, double d) {
        return again ? o.recompute(b.src + d) : o.compute(b.src);
    });
    refusal([](auto length) { return PineEma(length); },
            [](PineEma& o, bool again) { again ? o.recompute(1.0) : o.compute(1.0); },
            "ema", "length");
}

void tsi() {
    ta::ema_na_warmup_flag() = false;
    PineTsi adapter(7, 19);
    ta::TSI legacy(7, 19);
    value_identity(adapter, legacy, [](auto& o, const Sample& b, bool again, double d) {
        return again ? o.recompute(b.src + d) : o.compute(b.src);
    });
    auto run = [](PineTsi& o, bool again) { again ? o.recompute(1.0) : o.compute(1.0); };
    refusal([](auto length) { return PineTsi(length, 19); }, run, "tsi", "short_length");
    refusal([](auto length) { return PineTsi(7, length); }, run, "tsi", "long_length");
    // Two invalid lengths report the first in the signature's order (an
    // assumption, not a TradingView fact).
    PineTsi both(0, -1);
    stops([&] { both.compute(1.0); }, positive_text("tsi", "short_length", "0"));
}

void macd() {
    ta::ema_na_warmup_flag() = false;
    PineMacd adapter(5, 17, 4);
    ta::MACD legacy(5, 17, 4);
    value_identity(adapter, legacy, [](auto& o, const Sample& b, bool again, double d) {
        return again ? o.recompute(b.src + d) : o.compute(b.src);
    });
    auto run = [](PineMacd& o, bool again) { again ? o.recompute(1.0) : o.compute(1.0); };
    refusal([](auto length) { return PineMacd(length, 17, 4); }, run, "macd", "fastlen");
    refusal([](auto length) { return PineMacd(5, length, 4); }, run, "macd", "slowlen");
    refusal([](auto length) { return PineMacd(5, 17, length); }, run, "macd", "siglen");
    PineMacd all(0, 0, 0);
    stops([&] { all.recompute(1.0); }, positive_text("macd", "fastlen", "0"));
}

template <class Adapter, class Legacy>
void channel(const char* function) {
    for (bool flag : {false, true}) {
        for (bool use_true_range : {true, false}) {
            ta::ema_na_warmup_flag() = flag;
            Adapter adapter(6, 1.75, use_true_range);
            Legacy legacy(6, 1.75, use_true_range);
            value_identity(adapter, legacy, [](auto& o, const Sample& b, bool again, double d) {
                return again ? o.recompute(b.src + d, b.high + d, b.low, b.close)
                             : o.compute(b.src, b.high, b.low, b.close);
            });
            // The adapter neither reads nor changes the ambient seeding flag.
            assert(ta::ema_na_warmup_flag() == flag);
        }
    }
    ta::ema_na_warmup_flag() = false;
    // The two-argument form is the true range, as for the legacy class.
    Adapter adapter(6, 1.75);
    Legacy legacy(6, 1.75);
    value_identity(adapter, legacy, [](auto& o, const Sample& b, bool again, double d) {
        return again ? o.recompute(b.src + d, b.high, b.low, b.close)
                     : o.compute(b.src, b.high, b.low, b.close);
    });
    refusal([](auto length) { return Adapter(length, 1.75, false); },
            [](Adapter& o, bool again) {
                again ? (void)o.recompute(1.0, 2.0, 0.5, 1.0) : (void)o.compute(1.0, 2.0, 0.5, 1.0);
            },
            function, "length");
}

void kc() { channel<PineKc, ta::KC>("kc"); }
void kcw() { channel<PineKcw, ta::KCW>("kcw"); }

// The drop-in shapes (recompute first, reset after use, copies) per type.
auto single_call = [](auto& o, const Sample& b, bool again, double d) {
    return again ? o.recompute(b.src + d) : o.compute(b.src);
};
auto channel_call = [](auto& o, const Sample& b, bool again, double d) {
    return again ? o.recompute(b.src + d, b.high + d, b.low, b.close)
                 : o.compute(b.src, b.high, b.low, b.close);
};

void ema_shapes() {
    ta::ema_na_warmup_flag() = false;
    shapes<PineEma, ta::EMA>(single_call, [](auto length) { return PineEma(length); }, "ema",
                             "length", 13);
}

void tsi_shapes() {
    ta::ema_na_warmup_flag() = false;
    shapes<PineTsi, ta::TSI>(single_call, [](auto length) { return PineTsi(length, 19); },
                             "tsi", "short_length", 7, 19);
}

void macd_shapes() {
    ta::ema_na_warmup_flag() = false;
    shapes<PineMacd, ta::MACD>(single_call, [](auto length) { return PineMacd(length, 17, 4); },
                               "macd", "fastlen", 5, 17, 4);
}

template <class Adapter, class Legacy>
void channel_shapes(const char* function) {
    for (bool flag : {false, true}) {
        for (bool use_true_range : {true, false}) {
            ta::ema_na_warmup_flag() = flag;
            shapes<Adapter, Legacy>(
                channel_call,
                [use_true_range](auto length) { return Adapter(length, 1.75, use_true_range); },
                function, "length", 6, 1.75, use_true_range);
        }
    }
    ta::ema_na_warmup_flag() = false;
}

void kc_shapes() { channel_shapes<PineKc, ta::KC>("kc"); }
void kcw_shapes() { channel_shapes<PineKcw, ta::KCW>("kcw"); }

// A control that documents why the adapters exist, not a test of them: the
// fixed-length EMA takes length 0 and computes where a script must stop.
// alpha = 2.0 / (0 + 1) = 2 (src/ta_moving_averages.cpp:427-428); the first
// finite source seeds the value, the next steps ema + alpha * (src - ema):
// 10 + 2 * (12 - 10) = 14.
void legacy_zero_length_computes() {
    ta::ema_na_warmup_flag() = false;
    ta::EMA legacy(0);
    same(legacy.compute(10.0), 10.0);
    same(legacy.compute(12.0), 14.0);
}

// The generated-code shapes: FirstCallBound<PineEma>, a reset assignment and
// the owned-storage observation.
void generated_usage() {
    ta::ema_na_warmup_flag() = false;
    pineforge::source::FirstCallBound<PineEma> bound;
    PineEma member(4);
    member = PineEma(9);
    ta::EMA legacy(9);
    for (const Sample& bar : bars()) {
        const double expected = legacy.compute(bar.src);
        same(bound.compute([&] { return PineEma(pineforge::source::simple_ta_length(9, "ema", "length")); },
                           bar.src),
             expected);
        same(member.compute(bar.src), expected);
    }
    assert(bound.bound());
    OwnedStorage observed;
    bound.observe_owned_storage(observed);
    member.observe_owned_storage(observed);
    PineEma invalid(0);  // constructing and observing never stop
    invalid.observe_owned_storage(observed);
    PineMacd macd(0, 0, 0);
    macd.observe_owned_storage(observed);
    PineTsi tsi(0, 0);
    tsi.observe_owned_storage(observed);
    PineKc kc(0, 1.0);
    kc.observe_owned_storage(observed);
    PineKcw kcw(0, 1.0);
    kcw.observe_owned_storage(observed);
    assert(observed.usage().complete());
}

}  // namespace

int main() {
    const bool prior = ta::ema_na_warmup_flag();
    ema();
    tsi();
    macd();
    kc();
    kcw();
    ema_shapes();
    tsi_shapes();
    macd_shapes();
    kc_shapes();
    kcw_shapes();
    legacy_zero_length_computes();
    generated_usage();
    ta::ema_na_warmup_flag() = prior;
    std::printf("ok\n");
    return 0;
}
