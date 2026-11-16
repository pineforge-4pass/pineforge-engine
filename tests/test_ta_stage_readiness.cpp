// Kernel observation clocks for composed EMA stages. These are bookkeeping
// invariants, independent of numerical seeding and Pine publication policy.
// Neither a clock nor a kernel composition below is a TradingView oracle.
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
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

struct Seeding {
    bool previous = ta::ema_na_warmup_flag();
    explicit Seeding(bool average) { ta::ema_na_warmup_flag() = average; }
    ~Seeding() { ta::ema_na_warmup_flag() = previous; }
};

double source_at(int bar, bool gaps, bool flat) {
    if (gaps && (bar < 2 || bar % 11 == 4)) return na<double>();
    return flat ? 7.0 : 100.0 + bar / 8.0 + ((37 * bar) % 29) / 4.0;
}

// Independent inputs-only count of finite changes and qualified long-stage
// observations. No expected clock is obtained from a class under test.
struct ChangeClock {
    double previous = na<double>();
    int long_seen = 0;
    int short_seen = 0;
    int short_length;
    int long_length;

    ChangeClock(int short_len, int long_len)
        : short_length(short_len), long_length(long_len) {}

    bool step(double src) {
        if (std::isfinite(src - previous)) {
            if (long_seen < long_length) ++long_seen;
            if (long_seen >= long_length && short_seen < short_length) ++short_seen;
        }
        previous = src;
        return short_seen >= short_length;
    }
};

// Pre-change numerical composition: keep this solely as a bit-preservation
// control for the new bookkeeping. It supplies no expected TV values.
struct Composition {
    ta::EMA long_stage, short_stage, abs_long_stage, abs_short_stage;
    double previous = na<double>();

    Composition(int short_len, int long_len)
        : long_stage(long_len), short_stage(short_len),
          abs_long_stage(long_len), abs_short_stage(short_len) {}

    double compute(double src) {
        if (is_na(previous)) {
            previous = src;
            return na<double>();
        }
        const double change = src - previous;
        previous = src;
        const double numerator = short_stage.compute(long_stage.compute(change));
        const double denominator =
            abs_short_stage.compute(abs_long_stage.compute(std::abs(change)));
        return denominator == 0.0 ? na<double>() : numerator / denominator;
    }
};

void tsi_clocks() {
    const int lengths[][2] = {{13, 25}, {1, 1}, {2, 5}, {5, 2}, {1, 4}, {4, 1}};
    for (bool average : {false, true}) {
        Seeding seed(average);
        for (const auto& pair : lengths) {
            for (bool gaps : {false, true}) {
                for (bool flat : {false, true}) {
                    ta::TSI live(pair[0], pair[1]), committed(pair[0], pair[1]);
                    Composition old_numbers(pair[0], pair[1]);
                    ChangeClock clock(pair[0], pair[1]);
                    int numbers = 0;
                    for (int bar = 0; bar < 260; ++bar) {
                        const double src = source_at(bar, gaps, flat);
                        live.compute(source_at(bar, false, false) + 9.0);
                        live.recompute(na<double>());
                        live.recompute(source_at(bar, false, false) - 4.0);
                        const double actual = live.recompute(src);
                        const double once = committed.compute(src);
                        CHECK(same(actual, once));
                        CHECK(same(actual, old_numbers.compute(src)));
                        const bool expected = clock.step(src);
                        CHECK(live.ready() == expected);
                        CHECK(committed.ready() == expected);
                        if (!is_na(actual)) ++numbers;

                        // A divergent branch must not move the original's
                        // value, its count, or its pre-update checkpoint.
                        auto branch = live;
                        branch.compute(500.0 + bar);
                        CHECK(same(live.recompute(src), actual));
                        CHECK(live.ready() == expected);
                        auto copy = live;
                        CHECK(same(copy.recompute(src), actual));
                        CHECK(copy.ready() == expected);
                    }
                    CHECK(clock.short_seen == pair[0]);
                    CHECK((numbers > 0) == !flat);
                }
            }
        }
    }
}

void channel_clocks() {
    for (bool average : {false, true}) {
        Seeding seed(average);
        for (int length : {1, 2, 3, 20}) {
            for (bool true_range : {false, true}) {
                for (bool gaps : {false, true}) {
                    ta::KC channel(length, 1.5, true_range);
                    ta::KCW width(length, 1.5, true_range);
                    int sources = 0, ranges = 0;
                    double previous_close = na<double>();
                    for (int bar = 0; bar < 100; ++bar) {
                        const double src = source_at(bar, gaps, false);
                        const double high = gaps && bar % 13 == 7
                            ? na<double>() : 200.0 + bar;
                        const double low = gaps && bar % 17 == 9
                            ? na<double>() : 190.0 + bar;
                        const double close = gaps && bar % 19 == 3
                            ? na<double>() : 195.0 + bar;
                        const double span = !true_range ? high - low
                            : (is_na(previous_close) ? na<double>()
                               : std::max(high - low,
                                    std::max(std::abs(high - previous_close),
                                             std::abs(low - previous_close))));
                        previous_close = close;
                        if (std::isfinite(src) && sources < length) ++sources;
                        if (std::isfinite(span) && ranges < length) ++ranges;
                        channel.compute(src, high, low, close);
                        width.compute(src, high, low, close);
                        CHECK(channel.basis_ready() == (sources >= length));
                        CHECK(channel.range_ready() == (ranges >= length));
                        CHECK(width.ready() == (sources >= length && ranges >= length));

                        const auto saved_channel = channel;
                        const auto saved_width = width;
                        channel.recompute(0.0, 10.0, -10.0, 0.0);
                        width.recompute(0.0, 10.0, -10.0, 0.0);
                        channel.recompute(src, high, low, close);
                        width.recompute(src, high, low, close);
                        CHECK(channel.basis_ready() == saved_channel.basis_ready());
                        CHECK(channel.range_ready() == saved_channel.range_ready());
                        CHECK(width.ready() == saved_width.ready());
                    }
                    CHECK(sources == length && ranges == length);
                }
            }
        }
    }
}

} // namespace

int main() {
    tsi_clocks();
    channel_clocks();
    return failures == 0 && checks > 0 ? 0 : 1;
}
