#pragma once
#include "na.hpp"
#include "series.hpp"
#include <cstddef>
#include <vector>

namespace pineforge {

// TradingView's sliding-window sum -- the arithmetic behind ta.sma and
// math.sum (see the note above its implementation in
// src/ta_moving_averages.cpp for the tapes that pin each piece):
//
//   * a Kahan-compensated running sum S whose compensation c is the exact
//     rounding error of each step, fl(S + y) - (S + y), whatever the
//     magnitudes of S and y (the 2026-11-16 pulse pin);
//   * each source enters as y = x - c and, `length` bars later, the SOURCE x
//     leaves through the same Kahan step -- sub before add. The compensation
//     is not remembered with the source, so S - c stays the sum of the
//     window's sources whenever c was applied exactly (the 2026-11-16 price
//     pin);
//   * on the bar whose incoming source UNDER-applies the MAGNITUDE of the
//     carried compensation when it is ADDED (c != 0 and, with z = fl(x + |c|),
//     |z - x| < |c|: fully swallowed or rounded toward x -- the round-10
//     family-W pin; round 9's x - c form and round 7's swallow case are the
//     cases where x - c and x + |c| round alike), the sum is re-summed
//     newest-first over the window's sources and c is reset.
//
// Both users divide S by `length` themselves (ta.sma) or emit S (math.sum).
class KahanWindowSum {
    struct Entry {
        double value;
        // What the slot subtracts when it leaves the window. Equal to `value`
        // since the 2026-11-16 pin; kept as its own field so the record's
        // layout, and every checkpoint that copies it, does not move.
        double addend;
        void observe_owned_storage(OwnedStorage&) const noexcept {} // inline scalars
    };
    // A checkpoint owns one deep-copy buffer. Pair each source with what it
    // subtracts on leaving so growth cannot publish only half a window.
    // Active slots are newest first from head_; unused slots are capacity.
    std::vector<Entry> window_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
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

    std::size_t index(std::size_t offset) const noexcept {
        const auto position = head_ + offset;
        return position < window_.size() ? position : position - window_.size();
    }
    void ensure_room();
    void pop_front() noexcept {
        head_ = head_ + 1 == window_.size() ? 0 : head_ + 1;
        --size_;
    }
    void kahan_add(double v);
    void enter(double src, double comp_before);

public:
    // As for the original deque window, callers supply a positive length.
    explicit KahanWindowSum(int length);
    // Rvalue copies also preserve the source's storage/index relationship.
    KahanWindowSum(const KahanWindowSum&) = default;
    KahanWindowSum& operator=(const KahanWindowSum&) = default;

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
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        observe_owned_members(out, window_);
    }
};

} // namespace pineforge
