/*
 * test_pine_ta_length_stop.cpp — source::PineEma / PineTsi / PineKc / PineKcw
 * / PineMacd stop the run on a length of 0, a negative length or an na
 * length.
 *
 * One target: the stop happens on every execution of the call, compute() and
 * recompute() alike, before the source is read. The three ways a bad length
 * used to answer a value instead are each driven:
 *   - an na source (the kernel EMA answers na without looking at its length);
 *   - the first bar of ta.tsi (no previous source, so no stage runs);
 *   - an unready stage of ta.macd (the line and the signal are withheld, so a
 *     bad length would read as an ordinary warm-up).
 *
 * The failure is the one a simple length of the same call already stops with
 * (source::simple_ta_length): code pine_invalid_argument, the rule
 * must_be_positive or must_not_be_na, the call's function and argument names.
 * The expected failure of every case is therefore taken from that helper,
 * called here with the same names and value in the same bar context; the
 * class under test never supplies its own expectation.
 *
 * What is and is not a TradingView observation here.
 *   - Measured (include/pineforge/source/pine_ta_length.hpp, tv/edge: ta.rsi
 *     and ta.ema with a simple length of 0, -3 and na): TradingView stops
 *     ta.ema with the texts spelled out in row `ema`. Only that row compares
 *     a text with a literal.
 *   - Not measured: whether and with which text TradingView stops ta.tsi,
 *     ta.kc, ta.kcw and ta.macd, and which of several bad lengths it names.
 *     Rows `tsi`, `kc`, `kcw` and `macd` hold those calls to this
 *     implementation's rule (the same failure a simple length gives under
 *     the call's own names); they assert no TradingView text. Row `several`
 *     requires the stop of one of the bad lengths and does not say which.
 *     Both stay to be measured on the issue 291 matrix (rows 45 to 76).
 *
 * Rows:
 *   ema / tsi / kc / kcw / macd   every bad value of every length argument.
 *   several                       several bad lengths: one of them stops.
 *   valid                         control: a positive length never stops.
 *
 * NDEBUG-proof: a counting CHECK and a nonzero exit, never a bare assert().
 */

#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cstdio>
#include <exception>
#include <initializer_list>
#include <stdexcept>
#include <string>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr, ...)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            if (tests_failed < 40) {                                           \
                std::printf("  FAIL %s:%d %s -- ", __FILE__, __LINE__, #expr); \
                std::printf(__VA_ARGS__);                                      \
                std::printf("\n");                                             \
            }                                                                  \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

// The three bad lengths: zero, a negative one, and the integer na.
static const int kBadLengths[] = {0, -1, na<int>()};

// What a call did: nothing thrown, or the exception it stopped with.
struct Failure {
    bool thrown = false;
    bool runtime_error = false;
    bool coded = false;
    RunFailureCode code = RunFailureCode::none;
    std::string args;
    std::string text;
};

template <class Call>
static Failure failure_of(Call&& call) {
    Failure failure;
    try {
        call();
    } catch (const std::runtime_error& error) {
        failure.thrown = true;
        failure.runtime_error = true;
        failure.text = error.what();
        if (const auto* info = dynamic_cast<const RunFailureInfo*>(&error)) {
            failure.coded = true;
            failure.code = info->run_failure_code();
            if (info->run_failure().args != nullptr) failure.args = *info->run_failure().args;
        }
    } catch (const std::exception& error) {
        failure.thrown = true;
        failure.text = error.what();
    }
    return failure;
}

static bool is_length_stop(const Failure& failure) {
    return failure.thrown && failure.runtime_error && failure.coded
        && failure.code == RunFailureCode::pine_invalid_argument;
}

// The stop a simple length of (function, argument, value) gives, in the bar
// context of the caller.
static Failure reference_stop(const char* function, const char* argument, int value) {
    const Failure reference = failure_of(
        [&] { (void)source::simple_ta_length(value, function, argument); });
    const char* rule = is_na(value) ? "must_not_be_na" : "must_be_positive";
    CHECK(is_length_stop(reference) && reference.args.find(rule) != std::string::npos
              && reference.args.find(function) != std::string::npos
              && reference.args.find(argument) != std::string::npos,
          "the reference stop of %s.%s(%d) is \"%s\" %s", function, argument, value,
          reference.text.c_str(), reference.args.c_str());
    return reference;
}

// Runs `call` and requires the length stop of (function, argument, value).
template <class Call>
static void expect_stop(Call&& call, const char* function, const char* argument, int value,
                        const char* what) {
    const Failure want = reference_stop(function, argument, value);
    const Failure got = failure_of(call);
    CHECK(got.thrown, "%s %s.%s(%d): answered a value instead of stopping", what, function,
          argument, value);
    CHECK(is_length_stop(got), "%s %s.%s(%d): not a pine_invalid_argument failure: %s", what,
          function, argument, value, got.text.c_str());
    CHECK(got.args == want.args, "%s %s.%s(%d): arguments %s want %s", what, function, argument,
          value, got.args.c_str(), want.args.c_str());
    CHECK(got.text == want.text, "%s %s.%s(%d): text \"%s\" want \"%s\"", what, function,
          argument, value, got.text.c_str(), want.text.c_str());
}

// One object, every execution path, without and with a bar context. `object`
// is rebuilt by `make` for each path, so no path depends on another's state.
template <class Make, class ComputeNa, class ComputeValue, class Recompute>
static void expect_every_path(Make&& make, ComputeNa&& compute_na, ComputeValue&& compute_value,
                              Recompute&& recompute_value, const char* function,
                              const char* argument, int value) {
    {
        auto object = make();
        expect_stop([&] { compute_na(object); }, function, argument, value,
                    "compute(na source)");
        // The length is checked on every execution, not only the first.
        expect_stop([&] { compute_na(object); }, function, argument, value,
                    "second compute(na source)");
    }
    {
        auto object = make();
        expect_stop([&] { compute_value(object); }, function, argument, value, "first compute");
    }
    {
        auto object = make();
        expect_stop([&] { recompute_value(object); }, function, argument, value, "recompute");
    }
    {
        auto object = make();
        ta::BarContextScope bar(7, 0);
        expect_stop([&] { compute_value(object); }, function, argument, value,
                    "compute on bar 7");
    }
}

static void test_ema() {
    std::printf("ema\n");
    // TradingView's own texts for ta.ema's length, spelled out once each and
    // compared with what the class stops with.
    {
        source::PineEma zero(0);
        const Failure got = failure_of([&] { (void)zero.compute(101.5); });
        CHECK(is_length_stop(got)
                  && got.text == "Invalid value of the 'length' argument (0) in the 'ema' "
                                 "function. It must be > 0.",
              "ema(0) stopped with \"%s\"", got.text.c_str());
        CHECK(got.args == "{\"argument\":\"length\",\"function\":\"ema\","
                          "\"rule\":\"must_be_positive\"}",
              "ema(0) arguments %s", got.args.c_str());
    }
    {
        source::PineEma missing(na<int>());
        const Failure got = failure_of([&] { (void)missing.compute(101.5); });
        CHECK(is_length_stop(got)
                  && got.text == "Invalid value of the 'length' argument in the 'ema' function. "
                                 "It must not be na",
              "ema(na) stopped with \"%s\"", got.text.c_str());
        CHECK(got.args == "{\"argument\":\"length\",\"function\":\"ema\","
                          "\"rule\":\"must_not_be_na\"}",
              "ema(na) arguments %s", got.args.c_str());
    }
    {
        // The bar of the stop is named when a bar context is installed.
        source::PineEma negative(-1);
        ta::BarContextScope bar(7, 0);
        const Failure got = failure_of([&] { (void)negative.compute(101.5); });
        CHECK(is_length_stop(got)
                  && got.text == "Error on bar 7: Invalid value of the 'length' argument (-1) "
                                 "in the 'ema' function. It must be > 0.",
              "ema(-1) on bar 7 stopped with \"%s\"", got.text.c_str());
    }
    for (int bad : kBadLengths) {
        auto make = [bad] { return source::PineEma(bad); };
        auto compute_na = [](source::PineEma& ema) { (void)ema.compute(na<double>()); };
        auto compute_value = [](source::PineEma& ema) { (void)ema.compute(101.5); };
        auto recompute_value = [](source::PineEma& ema) { (void)ema.recompute(101.5); };
        expect_every_path(make, compute_na, compute_value, recompute_value, "ema", "length",
                          bad);
    }
}

static void test_tsi() {
    std::printf("tsi\n");
    auto compute_na = [](source::PineTsi& tsi) { (void)tsi.compute(na<double>()); };
    // The first execution has no previous source: the kernel answers na
    // before any stage runs, so only the check can stop it.
    auto compute_value = [](source::PineTsi& tsi) { (void)tsi.compute(101.5); };
    auto recompute_value = [](source::PineTsi& tsi) { (void)tsi.recompute(101.5); };
    for (int bad : kBadLengths) {
        auto short_bad = [bad] { return source::PineTsi(bad, 25); };
        expect_every_path(short_bad, compute_na, compute_value, recompute_value, "tsi",
                          "short_length", bad);
        auto long_bad = [bad] { return source::PineTsi(13, bad); };
        expect_every_path(long_bad, compute_na, compute_value, recompute_value, "tsi",
                          "long_length", bad);
    }
}

static void test_kc() {
    std::printf("kc\n");
    auto compute_na = [](source::PineKc& kc) {
        (void)kc.compute(na<double>(), 103.0, 99.0, 101.0);
    };
    auto compute_value = [](source::PineKc& kc) { (void)kc.compute(101.5, 103.0, 99.0, 101.0); };
    auto recompute_value = [](source::PineKc& kc) {
        (void)kc.recompute(101.5, 103.0, 99.0, 101.0);
    };
    for (int bad : kBadLengths) {
        for (bool use_true_range : {true, false}) {
            auto make = [bad, use_true_range] {
                return source::PineKc(bad, 1.5, use_true_range);
            };
            expect_every_path(make, compute_na, compute_value, recompute_value, "kc", "length",
                              bad);
        }
        // A zero multiplier does not make the length irrelevant.
        auto zero_mult = [bad] { return source::PineKc(bad, 0.0); };
        expect_every_path(zero_mult, compute_na, compute_value, recompute_value, "kc", "length",
                          bad);
    }
}

static void test_kcw() {
    std::printf("kcw\n");
    auto compute_na = [](source::PineKcw& kcw) {
        (void)kcw.compute(na<double>(), 103.0, 99.0, 101.0);
    };
    auto compute_value = [](source::PineKcw& kcw) {
        (void)kcw.compute(101.5, 103.0, 99.0, 101.0);
    };
    auto recompute_value = [](source::PineKcw& kcw) {
        (void)kcw.recompute(101.5, 103.0, 99.0, 101.0);
    };
    for (int bad : kBadLengths) {
        for (bool use_true_range : {true, false}) {
            auto make = [bad, use_true_range] {
                return source::PineKcw(bad, 1.5, use_true_range);
            };
            expect_every_path(make, compute_na, compute_value, recompute_value, "kcw", "length",
                              bad);
        }
        auto zero_mult = [bad] { return source::PineKcw(bad, 0.0); };
        expect_every_path(zero_mult, compute_na, compute_value, recompute_value, "kcw",
                          "length", bad);
    }
}

static void test_macd() {
    std::printf("macd\n");
    auto compute_na = [](source::PineMacd& macd) { (void)macd.compute(na<double>()); };
    // On its first bar no stage of 12/26/9 is ready and every output is
    // withheld, so only the check can tell a bad length from a warm-up.
    auto compute_value = [](source::PineMacd& macd) { (void)macd.compute(101.5); };
    auto recompute_value = [](source::PineMacd& macd) { (void)macd.recompute(101.5); };
    for (int bad : kBadLengths) {
        auto fast_bad = [bad] { return source::PineMacd(bad, 26, 9); };
        expect_every_path(fast_bad, compute_na, compute_value, recompute_value, "macd",
                          "fastlen", bad);
        auto slow_bad = [bad] { return source::PineMacd(12, bad, 9); };
        expect_every_path(slow_bad, compute_na, compute_value, recompute_value, "macd",
                          "slowlen", bad);
        auto signal_bad = [bad] { return source::PineMacd(12, 26, bad); };
        expect_every_path(signal_bad, compute_na, compute_value, recompute_value, "macd",
                          "siglen", bad);
    }
}

struct BadArgument {
    const char* function;
    const char* argument;
    int value;
};

// Runs `call`, whose lengths are bad in every argument of `candidates`, and
// requires the length stop of one of them. Which one is not required.
template <class Call>
static void expect_stop_of_one(Call&& call, std::initializer_list<BadArgument> candidates,
                               const char* what) {
    const Failure got = failure_of(call);
    CHECK(is_length_stop(got), "%s: not a pine_invalid_argument failure: %s", what,
          got.thrown ? got.text.c_str() : "(answered a value)");
    int named = 0;
    for (const BadArgument& candidate : candidates) {
        const Failure want =
            reference_stop(candidate.function, candidate.argument, candidate.value);
        if (got.args == want.args && got.text == want.text) ++named;
    }
    CHECK(named == 1, "%s: the stop \"%s\" names %d of the bad lengths", what, got.text.c_str(),
          named);
}

// Several bad lengths in one call: the run stops, and the stop is that of
// one of the bad lengths. Which one TradingView names is not measured, so
// none is required here.
static void test_several() {
    std::printf("several\n");
    {
        source::PineTsi tsi(0, -1);
        expect_stop_of_one([&] { (void)tsi.compute(101.5); },
                           {{"tsi", "short_length", 0}, {"tsi", "long_length", -1}},
                           "tsi(0,-1)");
    }
    {
        source::PineMacd macd(-1, 0, na<int>());
        expect_stop_of_one([&] { (void)macd.compute(101.5); },
                           {{"macd", "fastlen", -1}, {"macd", "slowlen", 0},
                            {"macd", "siglen", na<int>()}},
                           "macd(-1,0,na)");
    }
    {
        source::PineMacd macd(12, 0, -1);
        expect_stop_of_one([&] { (void)macd.recompute(101.5); },
                           {{"macd", "slowlen", 0}, {"macd", "siglen", -1}}, "macd(12,0,-1)");
    }
}

// Control: a positive length, the smallest one included, never stops, with
// an na source or a number, on compute or recompute.
static void test_valid() {
    std::printf("valid\n");
    bool stopped = false;
    try {
        for (int length : {1, 2, 14}) {
            source::PineEma ema(length);
            (void)ema.compute(na<double>());
            (void)ema.compute(101.5);
            (void)ema.recompute(102.5);
            source::PineTsi tsi(length, length);
            (void)tsi.compute(na<double>());
            (void)tsi.compute(101.5);
            (void)tsi.recompute(102.5);
            source::PineKc kc(length, 1.5);
            (void)kc.compute(na<double>(), 103.0, 99.0, 101.0);
            (void)kc.compute(101.5, 103.0, 99.0, 101.0);
            (void)kc.recompute(102.5, 104.0, 100.0, 102.0);
            source::PineKcw kcw(length, 1.5, false);
            (void)kcw.compute(na<double>(), 103.0, 99.0, 101.0);
            (void)kcw.compute(101.5, 103.0, 99.0, 101.0);
            (void)kcw.recompute(102.5, 104.0, 100.0, 102.0);
            source::PineMacd macd(length, length, length);
            (void)macd.compute(na<double>());
            (void)macd.compute(101.5);
            (void)macd.recompute(102.5);
        }
    } catch (const std::exception& error) {
        stopped = true;
        std::printf("  a positive length stopped: %s\n", error.what());
    }
    CHECK(!stopped, "a positive length stopped the run");
    // A length of 1 has no value to wait for: it answers its first defined
    // source, which is the first value and the average of one value alike.
    source::PineEma one(1);
    CHECK(is_na(one.compute(na<double>())), "ema(1) answered a value for an na source");
    const double first = one.compute(101.5);
    CHECK(first == 101.5, "ema(1) first source %.17g", first);
}

int main(int argc, char** argv) {
    // A row name runs that row alone; no argument runs them all.
    const std::string row = argc == 2 ? argv[1] : "";
    const char* rows[] = {"ema", "tsi", "kc", "kcw", "macd", "several", "valid"};
    bool known = row.empty();
    for (const char* name : rows) known = known || row == name;
    if (argc > 2 || !known) {
        std::fprintf(stderr, "unknown length-stop row\n");
        return 2;
    }
    if (row.empty() || row == "ema") test_ema();
    if (row.empty() || row == "tsi") test_tsi();
    if (row.empty() || row == "kc") test_kc();
    if (row.empty() || row == "kcw") test_kcw();
    if (row.empty() || row == "macd") test_macd();
    if (row.empty() || row == "several") test_several();
    if (row.empty() || row == "valid") test_valid();
    std::printf("test_pine_ta_length_stop: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 && tests_passed > 0 ? 0 : 1;
}
