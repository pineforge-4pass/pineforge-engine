#pragma once
#include "na.hpp"
#include "series.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace pineforge {

// SMA's exact sum of the retained raw sources, rounded once to binary64.
// Each finite source is an integer multiple of 2^-1074. Separate unsigned
// positive/negative magnitudes make removal an exact inverse of insertion.
// The highest finite source bit is 2097; 34 limbs leave room for the sum of
// any positive 32-bit int window length. No arithmetic operation scans values_.
class ExactWindowSum {
    using Magnitude = std::array<std::uint64_t, 34>;

    OwnedDeque<double> values_;    // raw window sources, front = newest
    Magnitude positive_{};
    Magnitude negative_{};
    int length_;
    int count_ = 0;                // retained valid sources, at most length_
    int positive_infinities_ = 0;
    int negative_infinities_ = 0;

    // Only the evicted raw source and membership need saving. Replacement
    // and rewind invert the current source's contribution, without saving
    // copies of the magnitudes. Eviction is explicit: infinity is not NA.
    int saved_count_ = 0;
    double saved_evicted_value_ = 0.0;
    bool saved_evicted_ = false;
    bool saved_pushed_ = false;

    static void update_word(Magnitude& magnitude, std::size_t index,
                            std::uint64_t word, bool remove);
    void update(double src, bool remove);

public:
    explicit ExactWindowSum(int length);

    // Never pass NA. SMA holds its seeded window and calls note_no_push().
    double push(double src);
    // A later repush starts a fresh push; unpush has nothing to rewind.
    void note_no_push();
    // Replace the provisional front while preserving the pre-bar eviction.
    double repush(double src);
    // Remove the provisional front and restore that saved eviction, if any.
    void unpush();

    int count() const { return count_; }
    int length() const { return length_; }
    bool seeded() const { return count_ >= length_; }
    double sum() const;
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        observe_owned_members(out, values_);
    }
};

// Legacy sliding-window sum used by math.sum and biased StdDev, fitted
// bit-for-bit on the round-7 synthetic pins (see the historical note
// above its implementation in src/ta_moving_averages.cpp):
//
//   * a Kahan-compensated running sum S with compensation c;
//   * the ring stores the COMPENSATED addend y = x - c each source entered
//     with, and subtracts that y (through the same Kahan step) when the
//     source leaves the window `length` bars later -- sub before add;
//   * on the bar whose incoming source UNDER-applies the MAGNITUDE of the
//     carried compensation when it is ADDED (c != 0 and, with z = fl(x + |c|),
//     |z - x| < |c|: fully swallowed or rounded toward x -- the round-10
//     family-W pin; round 9's x - c form and round 7's swallow case are the
//     cases where x - c and x + |c| round alike), the sum is re-summed
//     newest-first over the window's sources, c is reset and the bar's ring
//     addend is its raw source.
//
// Consumers emit S or use it in their legacy arithmetic; SMA uses
// ExactWindowSum instead.
class KahanWindowSum {
    OwnedDeque<double> values_;    // window sources, front = newest
    OwnedDeque<double> addends_;   // the compensated addends, front = newest
    int length_;
    int count_;          // valid sources pushed so far
    double sum_;         // S
    double comp_;        // c

    // Rewind state for the current bar: the pre-bar (S, c, count), the
    // source/addend the bar evicted (na when it evicted none), and whether
    // the bar pushed at all (an na input pushes nothing).
    double saved_sum_;
    double saved_comp_;
    int saved_count_;
    double saved_evicted_value_;
    double saved_evicted_addend_;
    bool saved_pushed_;

    void kahan_add(double v);
    void enter(double src, double comp_before);

public:
    explicit KahanWindowSum(int length);

    // A valid source enters the window; returns S. Never pass na: callers
    // hold their seeded value on na input and call note_no_push() instead.
    double push(double src);
    // The current bar carried no source (na input): records that a later
    // repush() starts a fresh push and unpush() has nothing to rewind.
    void note_no_push();
    // Replay the current bar with a different valid source (intrabar
    // recompute): same evicted addend, same pre-bar (S, c). Returns S.
    double repush(double src);
    // Rewind the current bar entirely (its source leaves, the evicted source
    // returns) so the window reads as it did before the bar.
    void unpush();

    int count() const { return count_; }
    int length() const { return length_; }
    bool seeded() const { return count_ >= length_; }
    double sum() const { return sum_; }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        observe_owned_members(out, values_, addends_);
    }
};

} // namespace pineforge
