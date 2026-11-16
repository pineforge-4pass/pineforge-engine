#pragma once
#include "na.hpp"
#include "series.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pineforge {

// TradingView's sliding-window sum -- the arithmetic behind ta.sma and
// math.sum: the EXACT sum of the sources in the window, rounded once to the
// nearest double, ties to even. It is a function of the window alone: nothing
// is carried from a source that left, whatever the order, signs and
// magnitudes of the sources. ta.sma divides that rounded sum by `length`,
// which is not the rounding of the exact mean.
//
// Pinned on the TradingView captures of 2026-11-16: the price tape
// (tests/test_ta_sma_price_tape.cpp) and the pulse and fold captures
// (tests/test_ta_sma_exact_compensation.cpp). It replaces a compensated
// running sum with a re-sum rule, fitted in September 2026 on tapes
// (tests/test_ta_sma_window_residual.cpp) that the November captures of the
// same series contradict; the September rows that encode a carried residue
// are not reproduced. Biased ta.stdev and ta.bb are built on this sum and
// were not captured in November.
//
// The class keeps the name its users were written against; it no longer
// holds a Kahan compensation.
class KahanWindowSum {
    // One window slot: the source as it entered.
    struct Entry {
        double value;
        void observe_owned_storage(OwnedStorage&) const noexcept {} // an inline scalar
    };
    // A checkpoint owns one deep-copy buffer: the window's sources, newest
    // first from head_; unused slots are capacity.
    std::vector<Entry> window_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    int length_;
    int count_;          // valid sources pushed so far

    // The exact sum of the finite sources in the window as one two's-complement
    // integer in units of 2^-1074, least significant limb first. Every finite
    // double is a whole number of those units below 2^2098 and a window holds
    // fewer than 2^31 sources, so 34 limbs (2176 bits) cannot overflow.
    static constexpr std::size_t limbs = 34;
    std::array<std::uint64_t, limbs> exact_{};
    // The non-finite sources in the window, by kind: they decide the sum alone.
    int nans_ = 0;
    int positive_infinities_ = 0;
    int negative_infinities_ = 0;
    double sum_;         // the rounding of the window's exact sum

    // Rewind state for the current bar: the pre-bar (sum, count), the source
    // the bar evicted when it evicted one, and whether the bar pushed at all
    // (an na input pushes nothing).
    double saved_sum_;
    int saved_count_;
    double saved_evicted_value_;
    bool saved_evicted_;
    bool saved_pushed_;

    std::size_t index(std::size_t offset) const noexcept {
        const auto position = head_ + offset;
        return position < window_.size() ? position : position - window_.size();
    }
    void ensure_room();
    void pop_front() noexcept {
        head_ = head_ + 1 == window_.size() ? 0 : head_ + 1;
        --size_;
    }
    void carry(std::size_t limb, std::uint64_t word, bool subtract) noexcept;
    void account(double src, bool leaving) noexcept;
    double rounded() const noexcept;

public:
    // As for the original deque window, callers supply a positive length.
    explicit KahanWindowSum(int length);
    // Rvalue copies also preserve the source's storage/index relationship.
    KahanWindowSum(const KahanWindowSum&) = default;
    KahanWindowSum& operator=(const KahanWindowSum&) = default;

    // A valid source enters the window, the oldest leaving once the window is
    // full; returns the sum. Callers never pass na: they hold their seeded
    // value on na input and call note_no_push() instead.
    double push(double src);
    // The current bar carried no source (na input): records that a later
    // repush() starts a fresh push and unpush() has nothing to rewind.
    void note_no_push();
    // Replay the current bar with a different valid source (intrabar
    // recompute): the source the bar evicted stays out. Returns the sum.
    double repush(double src);
    // Rewind the current bar entirely (its source leaves, the evicted source
    // returns) so the window reads as it did before the bar.
    void unpush();

    int count() const { return count_; }
    int length() const { return length_; }
    bool seeded() const { return count_ >= length_; }
    double sum() const { return sum_; }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        observe_owned_members(out, window_);
    }
};

} // namespace pineforge
