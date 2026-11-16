#pragma once
// Inputs and definition-only availability clocks shared by
// test_ta_stage_readiness.cpp (the kernel's clocks) and
// test_pine_ta_ready_publication.cpp (the publication built on them).
//
// Nothing here reads a value back from a class under test. A clock counts
// from the inputs alone: a source is defined when it is not na; a change when
// this source and the previous one are; a true range when the high, the low
// and the previous bar's close are; high - low when the high and the low are.
// Source-free: it includes only the kernel's headers.
//
// What the clocks are. EmaClock is the one stage rule: a stage is ready once
// `length` defined inputs have reached it. TsiClock and KcClock apply that
// rule to the stages ta::TSI and ta::KC are built from. They state this
// implementation's rule so a test can hold a class to it on every bar; they
// are not TradingView observations, and no test may read a first bar out of
// them as one.

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cstring>

namespace stage_clock_fixture {

inline bool same(double a, double b) {
    if (pineforge::is_na(a) || pineforge::is_na(b))
        return pineforge::is_na(a) && pineforge::is_na(b);
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// A source that never repeats a change: consecutive values differ by 2.125 or
// by -5.125 exactly, so no change and no smoothed |change| is ever zero.
inline double price(int bar) {
    return 100.0 + bar / 8.0 + ((37 * bar) % 29) / 4.0;
}

// Leading and internal gaps: bars 0 and 1, then every eleventh bar.
inline bool gap(int bar) { return bar < 2 || bar % 11 == 4; }

enum class Shape { Plain, Gapped, Flat, FlatGapped };

inline const char* shape_name(Shape shape) {
    switch (shape) {
    case Shape::Plain: return "plain";
    case Shape::Gapped: return "gapped";
    case Shape::Flat: return "flat";
    case Shape::FlatGapped: return "flat-gapped";
    }
    return "?";
}

inline bool is_flat(Shape shape) {
    return shape == Shape::Flat || shape == Shape::FlatGapped;
}

inline bool is_gapped(Shape shape) {
    return shape == Shape::Gapped || shape == Shape::FlatGapped;
}

inline double source_of(Shape shape, int bar) {
    if (is_gapped(shape) && gap(bar)) return pineforge::na<double>();
    return is_flat(shape) ? 7.0 : price(bar);
}

// Raises the ambient EMA seeding default for one case and restores it.
struct SeedingCase {
    bool previous;
    explicit SeedingCase(bool average) : previous(pineforge::ta::ema_na_warmup_flag()) {
        pineforge::ta::ema_na_warmup_flag() = average;
    }
    ~SeedingCase() { pineforge::ta::ema_na_warmup_flag() = previous; }
    SeedingCase(const SeedingCase&) = delete;
    SeedingCase& operator=(const SeedingCase&) = delete;
};

inline const char* seeding_name(bool average) {
    return average ? "average" : "first-value";
}

// One EMA stage: ready once `length` defined inputs have been seen.
struct EmaClock {
    int length;
    int seen = 0;

    explicit EmaClock(int len) : length(len) {}

    bool step(double src) {
        if (!pineforge::is_na(src) && seen < length) ++seen;
        return seen >= length;
    }
    bool ready() const { return seen >= length; }
};

// The two-stage clock of a change smoothed by a long, then a short stage.
struct TsiClock {
    int short_length;
    int long_length;
    bool previous_defined = false;
    bool change_defined = false;
    int long_seen = 0;
    int short_seen = 0;

    TsiClock(int short_len, int long_len) : short_length(short_len), long_length(long_len) {}

    bool step(double src) {
        change_defined = previous_defined && !pineforge::is_na(src);
        previous_defined = !pineforge::is_na(src);
        if (change_defined) {
            if (long_seen < long_length) ++long_seen;
            // The short stage counts a long-stage value only once that stage
            // is ready, the bar it becomes ready included.
            if (long_seen >= long_length && short_seen < short_length) ++short_seen;
        }
        return short_seen >= short_length;
    }
    bool ready() const { return short_seen >= short_length; }
};

struct Candle {
    double src;
    double high;
    double low;
    double close;
};

// `dirty` adds na sources (the gap pattern), na highs and na closes.
inline Candle candle_of(int bar, bool dirty) {
    const double close = price(bar);
    const double open = bar == 0 ? close : price(bar - 1);
    Candle candle;
    candle.src = close;
    candle.high = std::max(open, close) + 1.0 + (bar % 5) / 4.0;
    candle.low = std::min(open, close) - 0.5 - (bar % 3) / 2.0;
    candle.close = close;
    if (dirty) {
        if (gap(bar)) candle.src = pineforge::na<double>();
        if (bar % 13 == 7) candle.high = pineforge::na<double>();
        if (bar % 17 == 9) candle.close = pineforge::na<double>();
    }
    return candle;
}

// The basis clock and the range clock of a channel.
struct KcClock {
    int length;
    bool use_true_range;
    int basis_seen = 0;
    int range_seen = 0;
    double previous_close = pineforge::na<double>();
    bool source_defined = false;
    bool range_defined = false;

    KcClock(int len, bool true_range) : length(len), use_true_range(true_range) {}

    void step(const Candle& candle) {
        source_defined = !pineforge::is_na(candle.src);
        const bool span = !pineforge::is_na(candle.high) && !pineforge::is_na(candle.low);
        range_defined = use_true_range ? span && !pineforge::is_na(previous_close) : span;
        previous_close = candle.close;
        if (source_defined && basis_seen < length) ++basis_seen;
        if (range_defined && range_seen < length) ++range_seen;
    }
    bool basis_ready() const { return basis_seen >= length; }
    bool range_ready() const { return range_seen >= length; }
    bool ready() const { return basis_ready() && range_ready(); }
};

}  // namespace stage_clock_fixture
