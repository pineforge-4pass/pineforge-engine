// Valid adapter calls preserve the current kernel publication and execution
// state: recompute, copies, first-call binding, nested consumers, and distinct
// evaluation contexts. The kernel comparison is an invariant ONLY.
//
// No native result, counted clock, textbook formula, or synthetic first bar
// below is a TV value expectation. Startup/value parity remains unqualified
// until the corresponding retained/captured TV tape is bound to each case.
#include <pineforge/na.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace pineforge;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                         \
        if (failures < 20)                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (false)

bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return std::memcmp(&a, &b, sizeof a) == 0;
}

bool same(const ta::KCResult& a, const ta::KCResult& b) {
    return same(a.middle, b.middle) && same(a.upper, b.upper) && same(a.lower, b.lower);
}

bool same(const ta::MACDResult& a, const ta::MACDResult& b) {
    return same(a.macd_line, b.macd_line) && same(a.signal_line, b.signal_line)
        && same(a.histogram, b.histogram);
}

double selected(double value) { return value; }
double selected(const ta::KCResult& value) { return value.middle; }
double selected(const ta::MACDResult& value) { return value.histogram; }

struct Seeding {
    bool previous = ta::ema_na_warmup_flag();
    explicit Seeding(bool average) { ta::ema_na_warmup_flag() = average; }
    ~Seeding() { ta::ema_na_warmup_flag() = previous; }
};

struct Candle {
    double src, high, low, close;
};

Candle input(int bar, bool gaps, bool flat) {
    const double close = 100.0 + bar / 8.0 + ((37 * bar) % 29) / 4.0;
    Candle result{flat ? 7.0 : close, close + 2.0, close - 3.0, close};
    if (gaps) {
        if (bar < 2 || bar % 11 == 4) result.src = na<double>();
        if (bar % 13 == 7) result.high = na<double>();
        if (bar % 17 == 9) result.low = na<double>();
        if (bar % 19 == 3) result.close = na<double>();
    }
    return result;
}

template <class MakePine, class MakeKernel, class Call>
void run_case(MakePine make_pine, MakeKernel make_kernel, Call call, int outer_length) {
    using Adapter = decltype(make_pine());
    for (bool average : {false, true}) {
        Seeding chart_seed(average);
        for (bool gaps : {false, true}) {
            for (bool flat : {false, true}) {
                auto live = make_pine();
                auto ordinary = make_pine();
                auto kernel = make_kernel();
                auto other = make_pine();
                auto other_kernel = make_kernel();
                source::FirstCallBound<Adapter> bound;
                source::PineEma nested(outer_length);
                ta::EMA nested_kernel(outer_length);
                int constructions = 0, values = 0;
                {
                    auto first_recompute = make_pine();
                    auto kernel_recompute = make_kernel();
                    const Candle first = input(0, gaps, flat);
                    CHECK(same(call(first_recompute, first, true),
                               call(kernel_recompute, first, true)));
                }
                for (int bar = 0; bar < 300; ++bar) {
                    ta::BarContextScope chart(bar, 10'000);
                    const Candle final = input(bar, gaps, flat);
                    const Candle tentative{900.0 + bar, 1'000.0, 800.0, 950.0};
                    const Candle missing{na<double>(), na<double>(), na<double>(), na<double>()};
                    call(live, tentative, false);
                    nested.compute(20.0 + bar);
                    call(live, missing, true);
                    const auto actual = call(live, final, true);
                    const auto expected = call(kernel, final, false);
                    CHECK(same(actual, expected));
                    CHECK(same(call(ordinary, final, false), expected));
                    CHECK(same(nested.recompute(selected(actual)),
                               nested_kernel.compute(selected(expected))));
                    if (!is_na(selected(expected))) ++values;

                    auto& first_bound = bound.bind([&] {
                        ++constructions;
                        return make_pine();
                    });
                    CHECK(same(call(first_bound, final, false), expected));

                    const auto checkpoint = live;
                    auto branch = checkpoint;
                    call(branch, tentative, true);
                    call(branch, tentative, false);
                    CHECK(same(call(live, final, true), expected));
                    branch = checkpoint;
                    CHECK(same(call(branch, final, true), expected));

                    // Unit-level isolation of two evaluation contexts and
                    // their first-use seeding. This is not an executed
                    // request.security or generated-strategy parity probe.
                    {
                        Seeding other_seed(!average);
                        ta::BarContextScope requested(3 * bar + 2, 20'000);
                        const Candle other_input = input(bar + 5, !gaps, false);
                        CHECK(same(call(other, other_input, false),
                                   call(other_kernel, other_input, false)));
                    }
                    CHECK(ta::ema_na_warmup_flag() == average);
                    CHECK(ta::bar_context().bar_index == bar);
                    CHECK(ta::bar_context().origin == 10'000);
                }
                CHECK(constructions == 1);
                if (!flat) CHECK(values > 0);
            }
        }
    }
}

void families() {
    const auto unary = [](auto& object, const Candle& candle, bool replace) {
        return replace ? object.recompute(candle.src) : object.compute(candle.src);
    };
    const auto channel = [](auto& object, const Candle& candle, bool replace) {
        return replace
            ? object.recompute(candle.src, candle.high, candle.low, candle.close)
            : object.compute(candle.src, candle.high, candle.low, candle.close);
    };
    for (int length : {14, 1, 2, 3, 20, 200}) {
        run_case([=] { return source::PineEma(length); },
                 [=] { return ta::EMA(length); }, unary, 3);
    }
    const int tsi_lengths[][2] = {{13, 25}, {1, 1}, {2, 5}, {5, 2}, {1, 4}, {4, 1}};
    for (const auto& pair : tsi_lengths) {
        run_case([=] { return source::PineTsi(pair[0], pair[1]); },
                 [=] { return ta::TSI(pair[0], pair[1]); }, unary, pair[0]);
    }
    for (int length : {20, 1, 2, 3}) {
        for (bool true_range : {false, true}) {
            for (double multiplier : {1.5, 0.0}) {
                run_case([=] { return source::PineKc(length, multiplier, true_range); },
                         [=] { return ta::KC(length, multiplier, true_range); }, channel, length);
                run_case([=] { return source::PineKcw(length, multiplier, true_range); },
                         [=] { return ta::KCW(length, multiplier, true_range); }, channel, length);
            }
        }
    }
    const int macd_lengths[][3] = {{12, 26, 9}, {2, 5, 3}, {5, 2, 1}, {1, 1, 2}, {1, 4, 1}};
    for (const auto& lengths : macd_lengths) {
        run_case([=] { return source::PineMacd(lengths[0], lengths[1], lengths[2]); },
                 [=] { return ta::MACD(lengths[0], lengths[1], lengths[2]); }, unary, lengths[2]);
    }
}

// Both omitted range arguments retain the existing true-range default.
void omitted_range() {
    for (bool average : {false, true}) {
        Seeding seed(average);
        source::PineKc omitted(20, 1.5), explicit_true(20, 1.5, true);
        source::PineKcw omitted_width(20, 1.5), explicit_width(20, 1.5, true);
        for (int bar = 0; bar < 60; ++bar) {
            const auto c = input(bar, true, false);
            CHECK(same(omitted.compute(c.src, c.high, c.low, c.close),
                       explicit_true.compute(c.src, c.high, c.low, c.close)));
            CHECK(same(omitted_width.compute(c.src, c.high, c.low, c.close),
                       explicit_width.compute(c.src, c.high, c.low, c.close)));
        }
    }
}

} // namespace

int main() {
    families();
    omitted_range();
    return failures == 0 && checks > 0 ? 0 : 1;
}
