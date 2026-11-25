#pragma once
#include "na.hpp"
#include "series.hpp"
#include <cstddef>
#include <vector>

namespace pineforge {

// Fixed-window arithmetic for math.sum and ta.sma. Starting at S = C = 0,
// subtract the oldest raw source when full, then add the incoming source.
// Each addition updates S and adds its two-sum error to C in ordinary double
// precision. Emit S + C; SMA divides that result by length. C is never folded
// back into S. The rounding of C can retain information about earlier bars.
// Series-length SMA uses the same arithmetic to adjust its suffix, oldest
// first, before inserting the source, as recorded on TradingView in November 2026.
//
// November 2026 sums of stochastic, K, closes and squared closes, and SMAs
// of K, support this order. The exact window sum missed three stochastic
// rows. The September rule evicted compensated addends and sometimes re-summed
// the window. See src/ta_moving_averages.cpp for the comparisons and history.
class KahanWindowSum {
    // One owning buffer. Active sources are newest first from head_.
    std::vector<double> window_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    int length_;
    int count_;          // valid sources pushed so far
    double sum_;         // S: running total
    double comp_;        // C: running sum of the addition errors

    // Pre-bar state and the evicted raw source (na if the ring was not full).
    double saved_sum_;
    double saved_comp_;
    int saved_count_;
    double saved_evicted_value_;
    bool saved_pushed_;
    bool exact_recovery_ = false;
    bool saved_exact_recovery_ = false;

    std::size_t index(std::size_t offset) const noexcept {
        const auto position = head_ + offset;
        return position < window_.size() ? position : position - window_.size();
    }
    void ensure_room();
    void pop_front() noexcept {
        head_ = head_ + 1 == window_.size() ? 0 : head_ + 1;
        --size_;
    }
    bool add(double v) noexcept;
    void recover() noexcept;

public:
    // As for the original deque window, callers supply a positive length.
    explicit KahanWindowSum(int length);
    // Rvalue copies also preserve the source's storage/index relationship.
    KahanWindowSum(const KahanWindowSum&) = default;
    KahanWindowSum& operator=(const KahanWindowSum&) = default;

    // A non-na source enters the window; returns S + C.
    double push(double src);
    // The current bar carried no source (na input): records that a later
    // repush() starts a fresh push and unpush() has nothing to rewind.
    void note_no_push();
    // Replay the current bar with a different valid source (intrabar
    // recompute): same evicted source, same pre-bar S and C. Returns S + C.
    double repush(double src);
    // Rewind the current bar entirely (its source leaves, the evicted source
    // returns) so the window reads as it did before the bar.
    void unpush();

    int count() const { return count_; }
    int length() const { return length_; }
    bool seeded() const { return count_ >= length_; }
    double sum() const { return sum_ + comp_; }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        observe_owned_members(out, window_);
    }
};

} // namespace pineforge
