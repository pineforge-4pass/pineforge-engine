#pragma once
#include "na.hpp"
#include "window_ring.hpp"

namespace pineforge {

// TradingView's sliding-window sum -- the arithmetic behind ta.sma and
// math.sum, fitted bit-for-bit on the round-7 synthetic pins (see the note
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
// Both users divide S by `length` themselves (ta.sma) or emit S (math.sum).
class KahanWindowSum {
    WindowRing<double> values_;    // window sources, front = newest
    WindowRing<double> addends_;   // the compensated addends, front = newest
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

    // A valid source enters the window; returns S. Never pass na: both users
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
};

} // namespace pineforge
