#pragma once
#include "na.hpp"
#include "series.hpp"
#include <cstddef>
#include <vector>

namespace pineforge {

// TradingView's sliding-window sum -- the arithmetic behind ta.sma and
// math.sum (see the note above its implementation in
// src/ta_moving_averages.cpp for the captures that pin it):
//
//   * the emitted sum S is the exact sum of the sources in the window,
//     rounded once. It is a function of the window alone: nothing of a
//     source that has left stays in it, so an all-zero window reads 0 and a
//     window of one repeated value reads its exact multiple;
//   * S is carried with what its rounding left out, so the two doubles hold
//     the exact sum and a source enters or leaves by an error-free addition;
//   * when the exact sum needs more than two doubles, the window is summed
//     exactly from its own sources on every bar until two suffice again.
//
// Both users divide S by `length` themselves (ta.sma) or emit S (math.sum).
// The class keeps the name of the compensated running sum it replaced.
class KahanWindowSum {
    struct Entry {
        double value;
        double addend;   // spare slot: a partial sum while the window is re-summed
        void observe_owned_storage(OwnedStorage&) const noexcept {} // inline scalars
    };
    // A checkpoint owns one deep-copy buffer. Each source sits beside its
    // spare slot so growth cannot publish only half a window.
    // Active slots are newest first from head_; unused slots are capacity.
    std::vector<Entry> window_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    int length_;
    int count_;          // valid sources pushed so far
    double sum_;         // S: the exact sum of the window, rounded to nearest
    double comp_;        // the exact sum minus S; na while two doubles do not hold it

    // Rewind state for the current bar: the pre-bar (S, remainder, count), the
    // source and spare slot the bar evicted (na when it evicted none), and
    // whether the bar pushed at all (an na input pushes nothing).
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
    bool add_exact(double v);
    bool resum();
    void enter(double src, bool carried);

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
    // recompute): same evicted source, same pre-bar sum. Returns S.
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
