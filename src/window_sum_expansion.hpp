#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace pineforge::detail {

// Nonoverlapping partials have distinct highest set bits. Allow one slot per
// binary64 bit position, a zero, and room for the next insert.
inline constexpr std::size_t kWindowSumPartialCapacity =
    std::numeric_limits<double>::max_exponent -
    std::numeric_limits<double>::min_exponent +
    std::numeric_limits<double>::digits + 2;

// Insert a source into an expansion of nonoverlapping doubles, smallest first
// (Shewchuk's expansion sum, as in math.fsum). Each window supplies its own
// storage for the partials. False means a nonfinite source or overflow.
template <class Partials>
bool expand_window_sum(double x, std::size_t& parts, const Partials& partial) {
    std::size_t kept = 0;
    for (std::size_t j = 0; j < parts; ++j) {
        double y = partial(j);
        if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
        const double high = x + y;
        const double low = y - (high - x);  // exact: |x| >= |y|
        if (low != 0.0) partial(kept++) = low;
        x = high;
    }
    partial(kept++) = x;
    parts = kept;
    return std::isfinite(x);
}

struct RoundedWindowSum {
    double sum;
    double remainder;
    bool fits_pair;
};

template <class Partials>
RoundedWindowSum round_window_sum(std::size_t rest, const Partials& partial) {
    double high = 0.0;
    double low = 0.0;
    if (rest > 0) {
        high = partial(--rest);
        while (rest > 0) {
            const double x = high;
            const double y = partial(--rest);
            high = x + y;
            low = y - (high - x);
            if (low != 0.0) break;
        }
        // Exactly halfway between two doubles, the partial sums still below
        // decide the direction (round half to even otherwise).
        if (rest > 0 && ((low < 0.0 && partial(rest - 1) < 0.0)
                         || (low > 0.0 && partial(rest - 1) > 0.0))) {
            const double twice = low * 2.0;
            const double up = high + twice;
            if (twice == up - high) high = up;
        }
    }
    return {high, low, rest == 0};
}

// Each finite binary64 source is an integer multiple of 2^-1074. The
// signed accumulator has room for any window whose length fits in size_t.
class ExactWindowSum {
    using Word = std::uint64_t;
    static constexpr int word_bits = 64;
    static constexpr int fraction_bits = 52;
    static constexpr int unit_exponent = -1074;
    static constexpr int source_bits = 2098;
    static constexpr unsigned nonfinite_exponent = 0x7ff;
    static constexpr Word hidden_bit = Word{1} << fraction_bits;
    static constexpr std::size_t word_count =
        (source_bits + std::numeric_limits<std::size_t>::digits + word_bits) / word_bits;
    static_assert(sizeof(double) == sizeof(Word) && std::numeric_limits<double>::is_iec559
                  && std::numeric_limits<double>::digits == fraction_bits + 1,
                  "exact window sum requires IEEE binary64");

    std::array<Word, word_count> words_{};

    void add_word(std::size_t i, Word value, bool negative) noexcept {
        for (; value != 0 && i < words_.size(); ++i) {
            const Word before = words_[i];
            words_[i] = negative ? before - value : before + value;
            value = negative ? before < value : words_[i] < before;
        }
    }

public:
    bool add(double value) noexcept {
        Word bits;
        std::memcpy(&bits, &value, sizeof bits);
        const auto exponent = static_cast<unsigned>((bits >> fraction_bits) & nonfinite_exponent);
        if (exponent == nonfinite_exponent) return false;
        const Word significand = (bits & (hidden_bit - 1)) | (exponent ? hidden_bit : 0);
        const unsigned shift = exponent ? exponent - 1 : 0;
        const auto i = shift / word_bits;
        const auto offset = shift % word_bits;
        const bool negative = (bits >> (word_bits - 1)) != 0;
        add_word(i, significand << offset, negative);
        if (offset != 0) add_word(i + 1, significand >> (word_bits - offset), negative);
        return true;
    }

    bool rounded(double& result) noexcept {
        const bool negative = (words_.back() >> (word_bits - 1)) != 0;
        if (negative) {
            for (auto& word : words_) word = ~word;
            add_word(0, 1, false);
        }
        std::size_t used = words_.size();
        while (used != 0 && words_[used - 1] == 0) --used;
        if (used == 0) {
            result = 0.0;
            return true;
        }

        int high = static_cast<int>((used - 1) * word_bits);
        for (Word top = words_[used - 1] >> 1; top != 0; top >>= 1) ++high;
        if (high >= source_bits) return false;

        const int shift = std::max(0, high - fraction_bits);
        const auto i = static_cast<std::size_t>(shift / word_bits);
        const int offset = shift % word_bits;
        Word significand = words_[i] >> offset;
        if (offset != 0 && i + 1 < used) significand |= words_[i + 1] << (word_bits - offset);
        if (shift != 0) {
            const auto j = static_cast<std::size_t>((shift - 1) / word_bits);
            const Word half = Word{1} << ((shift - 1) % word_bits);
            if ((words_[j] & half) != 0) {
                bool above_half = (words_[j] & (half - 1)) != 0;
                for (std::size_t k = 0; k < j && !above_half; ++k)
                    above_half = words_[k] != 0;
                if (above_half || (significand & 1) != 0) ++significand;
            }
        }

        if (high == source_bits - 1 && significand == (hidden_bit << 1)) return false;
        const double magnitude = std::ldexp(static_cast<double>(significand), shift + unit_exponent);
        result = negative ? -magnitude : magnitude;
        return true;
    }
};

}  // namespace pineforge::detail
