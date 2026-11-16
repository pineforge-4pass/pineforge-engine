// Every EMA-family call validates its fixed length at execution, including
// NA sources, first-use recompute, and stages with no input yet.
//
// This is a unit test of the adapter's existing simple_ta_length contract.
// Only the EMA text below has retained TV provenance (tv/edge, documented in
// pine_ta_length.hpp). The other families' text and multi-invalid-argument
// precedence still need their own TV stops; neither is asserted here.
// Numerical startup/value expectations belong to the parity population.
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

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                         \
        std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        ++failures;                                                            \
    }                                                                          \
} while (false)

template <class Call>
void expect_length_stop(Call&& call, const char* function, const char* argument,
                        int length, const char* prefix = "") {
    bool caught = false;
    try {
        call();
    } catch (const std::runtime_error& error) {
        caught = true;
        const auto* info = dynamic_cast<const RunFailureInfo*>(&error);
        CHECK(info != nullptr);
        if (info) {
            CHECK(info->run_failure_code() == RunFailureCode::pine_invalid_argument);
            const std::string args = std::string("{\"argument\":\"") + argument
                + "\",\"function\":\"" + function + "\",\"rule\":\""
                + (is_na(length) ? "must_not_be_na" : "must_be_positive") + "\"}";
            CHECK(info->run_failure().args != nullptr);
            if (info->run_failure().args) CHECK(*info->run_failure().args == args);
        }
        if (*prefix)
            CHECK(std::string(error.what()).compare(0, std::string(prefix).size(), prefix) == 0);
        if (std::string(function) == "ema") {
            const std::string suffix = is_na(length)
                ? "Invalid value of the 'length' argument in the 'ema' function. It must not be na"
                : "Invalid value of the 'length' argument (" + std::to_string(length)
                    + ") in the 'ema' function. It must be > 0.";
            CHECK(std::string(error.what()) == std::string(prefix) + suffix);
        }
    } catch (const std::exception&) {
        caught = true;
        CHECK(false);
    }
    CHECK(caught);
}

template <class Make, class Call>
void every_execution(Make make, Call call, const char* function,
                     const char* argument, int length) {
    for (bool replace : {false, true}) {
        for (double src : {na<double>(), 101.5}) {
            // Construction does not execute a Pine call. An unused site
            // must not fail before its evaluation context has been entered.
            auto object = make();
            for (int repeat = 0; repeat < 2; ++repeat) {
                expect_length_stop([&] { call(object, src, replace); },
                                   function, argument, length);
            }
            auto checkpoint = object;
            ta::BarContextScope context(7, 0);
            expect_length_stop([&] { call(checkpoint, src, replace); },
                               function, argument, length, "Error on bar 7: ");
        }
    }
}

void invalid_lengths() {
    for (int bad : {0, -1, -3, na<int>()}) {
        every_execution([=] { return source::PineEma(bad); },
            [](auto& object, double src, bool replace) {
                if (replace) (void)object.recompute(src);
                else (void)object.compute(src);
            }, "ema", "length", bad);

        const auto tsi_call = [](auto& object, double src, bool replace) {
            if (replace) (void)object.recompute(src);
            else (void)object.compute(src);
        };
        every_execution([=] { return source::PineTsi(bad, 25); }, tsi_call,
                        "tsi", "short_length", bad);
        every_execution([=] { return source::PineTsi(13, bad); }, tsi_call,
                        "tsi", "long_length", bad);

        const auto channel_call = [](auto& object, double src, bool replace) {
            if (replace) (void)object.recompute(src, 104.0, 99.0, 102.0);
            else (void)object.compute(src, 104.0, 99.0, 102.0);
        };
        for (bool true_range : {false, true}) {
            for (double multiplier : {0.0, 1.5}) {
                every_execution([=] { return source::PineKc(bad, multiplier, true_range); },
                                channel_call, "kc", "length", bad);
                every_execution([=] { return source::PineKcw(bad, multiplier, true_range); },
                                channel_call, "kcw", "length", bad);
            }
        }

        const auto macd_call = [](auto& object, double src, bool replace) {
            if (replace) (void)object.recompute(src);
            else (void)object.compute(src);
        };
        every_execution([=] { return source::PineMacd(bad, 26, 9); }, macd_call,
                        "macd", "fastlen", bad);
        every_execution([=] { return source::PineMacd(12, bad, 9); }, macd_call,
                        "macd", "slowlen", bad);
        every_execution([=] { return source::PineMacd(12, 26, bad); }, macd_call,
                        "macd", "siglen", bad);
    }
}

void positive_lengths() {
    for (int length : {1, 2, 14, 200}) {
        source::PineEma ema(length);
        source::PineTsi tsi(length, length);
        source::PineKc kc(length, 0.0);
        source::PineKcw kcw(length, 1.5, false);
        source::PineMacd macd(length, length, length);
        bool stopped = false;
        try {
            for (double src : {na<double>(), 101.5, 0.0, -20.0}) {
                (void)ema.compute(src);
                (void)tsi.compute(src);
                (void)kc.compute(src, 104.0, 99.0, 102.0);
                (void)kcw.compute(src, 104.0, 99.0, 102.0);
                (void)macd.compute(src);
                (void)ema.recompute(src);
                (void)tsi.recompute(src);
                (void)kc.recompute(src, 104.0, 99.0, 102.0);
                (void)kcw.recompute(src, 104.0, 99.0, 102.0);
                (void)macd.recompute(src);
            }
        } catch (const std::exception&) {
            stopped = true;
        }
        CHECK(!stopped);
    }
}

} // namespace

int main() {
    invalid_lengths();
    positive_lengths();
    return failures == 0 && checks > 0 ? 0 : 1;
}
