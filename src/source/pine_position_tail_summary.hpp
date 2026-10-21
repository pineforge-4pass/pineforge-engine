#pragma once

#include "pine_position_tail.hpp"

#include <pineforge/detail/adapter_exposure_storage.hpp>
#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pineforge::source::detail {

// A proof, not a differently rounded floating-point accumulator. Each term
// is a nonnegative integer times 2^quantum; the numerator of the WHOLE
// selected sum must be strictly below 2^53. Every subset/partial sum is then
// exactly representable, including the original newest-first left fold.
// Reducing the quantum checks integer shifts BEFORE doing them. No rounded
// subtotal, cancellation or subtraction of a retired origin is used.
struct PositionTailExact {
    static constexpr std::uint64_t maximum = (UINT64_C(1) << 53) - 1;
    std::uint64_t magnitude = 0;
    std::int16_t quantum = 0;
    bool valid = true;

    void add(double value) noexcept {
        if (!valid) return;
        if (!std::isfinite(value) || std::signbit(value)) {
            valid = false;
            return;
        }
        if (value == 0.0) return;
        int exponent = 0;
        const double fraction = std::frexp(value, &exponent);
        auto integer = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
        exponent -= 53;
        while ((integer & 1) == 0) {
            integer >>= 1;
            ++exponent;
        }
        PositionTailExact term;
        term.magnitude = integer;
        term.quantum = static_cast<std::int16_t>(exponent);
        merge(term);
    }

    void merge(const PositionTailExact& other) noexcept {
        if (!valid || !other.valid) {
            valid = false;
            return;
        }
        if (other.magnitude == 0) return;
        if (magnitude == 0) {
            *this = other;
            return;
        }
        const int common = std::min(quantum, other.quantum);
        std::uint64_t left = magnitude, right = other.magnitude;
        const auto shift = [](std::uint64_t& value, int bits) {
            if (bits >= 53 || value > (maximum >> bits)) return false;
            value <<= bits;
            return true;
        };
        if (!shift(left, quantum - common) || !shift(right, other.quantum - common)
            || right > maximum - left) {
            valid = false;
            return;
        }
        magnitude = left + right;
        quantum = static_cast<std::int16_t>(common);
    }

    bool read(double& result) const noexcept {
        if (!valid) return false;
        const double value = std::ldexp(static_cast<double>(magnitude), quantum);
        if (!std::isfinite(value)) return false;
        result = value;
        return true;
    }
};
static_assert(sizeof(PositionTailExact) == 16, "compact dyadic certificate");
static_assert(std::numeric_limits<double>::is_iec559
              && std::numeric_limits<double>::digits == 53, "binary64 proof");

class PositionTailSummary {
    struct Origin {
        PositionTailExact completed;
        std::uint32_t visited = 0;
        bool is_long = false;
    };
    using Origins = std::unordered_map<std::uint64_t, Origin>;
    static_assert(sizeof(Origins::value_type) == 32, "32 bytes per origin, excluding map overhead");
    struct Margins {
        PositionTailExact side[2];
    };

public:
    PineExecutionAdapter::PositionTailWork work{};

    void set_directory(const std::string& directory) { margins_.set_directory(directory); }

    // This cache can be discarded at any point: uncertified/missing facts
    // ALWAYS select replay. It owns no logical broker transition. Reset
    // rebuilds reachable equal-time rows from the current row source rather
    // than assuming an empty position tail at an opening/reversal.
    template<class Read>
    void begin_position(std::int64_t barrier, std::size_t base, std::size_t end, Read&& read) {
        forget_origins();
        margins_.clear();
        all_margins_ = {};
        high_closed_origin_ = 0;
        barrier_ = barrier;
        observed_end_ = end;
        active_ = true;
        safe_ = true;
        have_time_ = false;
        std::int64_t newer_time = 0;
        for (std::size_t index = end; index > base;) {
            const Trade& row = read(--index);
            ++work.update_rows;
            if (row.exit_time < barrier) break; // strict barrier, equality survives
            if (have_time_ && row.exit_time > newer_time) {
                invalidate();
                break;
            }
            if (!have_time_) last_time_ = row.exit_time;
            have_time_ = true;
            newer_time = row.exit_time;
            accumulate(row);
        }
    }

    template<class Read>
    void observe(std::size_t base, std::size_t end, Read&& read) {
        if (!active_ || !safe_) return;
        if (end < observed_end_ || observed_end_ < base) {
            invalidate(); // rewind, missed publication, or uncertain reset
            return;
        }
        while (observed_end_ < end) {
            const Trade& row = read(observed_end_++);
            ++work.update_rows;
            if (row.exit_time < barrier_ || (have_time_ && row.exit_time < last_time_)) {
                invalidate(); // a row-order barrier cannot be treated as a time filter
                return;
            }
            last_time_ = row.exit_time;
            have_time_ = true;
            accumulate(row);
        }
    }

    void open_origin(std::uint64_t incarnation, bool is_long) {
        if (!safe_ || !active_) return;
        auto found = origins_.find(incarnation);
        if (found == origins_.end()) {
            Origin row;
            row.is_long = is_long;
            // A genuinely new incarnation cannot have earlier completed
            // rows. An old/reactivated incarnation whose subtotal was
            // retired is uncertain and must replay instead of starting at 0.
            row.completed.valid = incarnation > high_closed_origin_;
            origins_.emplace(incarnation, row);
        } else if (found->second.is_long != is_long) {
            found->second.completed.valid = false;
        }
    }

    void retire_origin(std::uint64_t incarnation) {
        origins_.erase(incarnation);
        if (origins_.empty()) {
            ::pineforge::detail::clear_exposure_storage(origins_);
        } else if (origins_.bucket_count() / 4 > origins_.size()) {
            origins_.rehash(origins_.size());
        }
    }

    void forget_origins() {
        ::pineforge::detail::clear_exposure_storage(origins_);
        ::pineforge::detail::clear_exposure_storage(live_cohorts_);
    }
    std::size_t resident_origins() const noexcept { return origins_.size(); }

    // Nonempty live maps pin their table rows. The adapter updates this
    // derived index at the same insert/erase/clear seams as those maps.
    // Querying only these rows avoids scanning resident historical cohorts
    // whose rosters are pinned for unrelated source bookkeeping.
    void live_cohort(const void* row, bool nonempty) {
        if (nonempty) {
            if (active_ && safe_) live_cohorts_.insert(row);
        } else {
            live_cohorts_.erase(row);
            if (live_cohorts_.empty())
                ::pineforge::detail::clear_exposure_storage(live_cohorts_);
            else if (live_cohorts_.bucket_count() / 4 > live_cohorts_.size())
                live_cohorts_.rehash(live_cohorts_.size());
        }
    }
    template<class Visit>
    void for_each_live_cohort(Visit&& visit) const {
        for (const auto* row : live_cohorts_) visit(row);
    }

    void invalidate() {
        safe_ = false;
        forget_origins();
        margins_.clear();
        all_margins_ = {};
    }

    template<class Select>
    bool ancestry(bool is_long, Select&& select, double& result) {
        ++work.queries;
        if (!safe_ || !active_) return finish(false);
        if (++query_generation_ == 0) {
            for (auto& row : origins_) row.second.visited = 0;
            ++query_generation_;
        }
        PositionTailExact total;
        select([&](std::uint64_t incarnation) {
            ++work.origin_visits;
            const auto found = origins_.find(incarnation);
            if (found == origins_.end()) {
                // No row for a fresh incarnation means an exact zero.
                // Missing older origins are deliberately uncertified.
                if (incarnation <= high_closed_origin_) total.valid = false;
                return;
            }
            auto& row = found->second;
            if (row.visited == query_generation_) return; // duplicate cohort membership
            row.visited = query_generation_;
            if (!row.completed.valid) total.valid = false;
            else if (row.is_long == is_long) total.merge(row.completed);
        });
        return finish(total.read(result));
    }

    bool margin(bool is_long, const std::string* named_entry, double& result) {
        ++work.queries;
        if (!safe_ || !active_) return finish(false);
        const auto side = is_long ? 1 : 0;
        if (!named_entry) return finish(all_margins_.side[side].read(result));
        const auto offset = margins_.find(*named_entry);
        if (!offset) {
            result = 0.0;
            return finish(true);
        }
        return finish(margins_.get(*offset).side[side].read(result));
    }

private:
    bool finish(bool certified) noexcept {
        if (certified) ++work.certified_queries;
        else ++work.fallback_queries;
        return certified;
    }

    void accumulate(const Trade& trade) {
        high_closed_origin_ = std::max(high_closed_origin_, trade.entry_incarnation);
        const auto origin = origins_.find(trade.entry_incarnation);
        if (origin != origins_.end()) {
            if (origin->second.is_long != trade.is_long)
                origin->second.completed.valid = false;
            origin->second.completed.add(trade.qty);
        }
        if (trade.exit_id != kMarginCallLabel) return;
        const auto side = trade.is_long ? 1 : 0;
        all_margins_.side[side].add(trade.qty);
        const auto inserted = margins_.insert(trade.entry_id, Margins{});
        auto value = margins_.get(inserted.first);
        value.side[side].add(trade.qty);
        margins_.set(inserted.first, value);
    }

    Origins origins_;
    std::unordered_set<const void*> live_cohorts_;
    // Historical margin ids must stay queryable even after their last live
    // origin drains. Only fixed tree metadata lives in RAM; values/ids are
    // in the existing external table, without a resident directory/cache.
    ::pineforge::detail::DiskIdTable<Margins> margins_;
    Margins all_margins_{};
    std::uint64_t high_closed_origin_ = 0;
    std::size_t observed_end_ = 0;
    std::int64_t barrier_ = 0, last_time_ = 0;
    std::uint32_t query_generation_ = 0;
    bool active_ = false, safe_ = false, have_time_ = false;
};

// The fallback is deliberately the old ordered fold over the row source.
// The predicate and the additions have the same evaluation/order as L18:
// resident newest-to-oldest, then external newest-to-oldest, first strict
// timestamp barrier terminates the entire walk. It remains O(history).
template<class Read, class Include>
double replay_position_tail(std::size_t end, std::size_t base, Read&& read,
                            const PositionTailSpill* spill, std::int64_t barrier,
                            Include&& include, const std::string& from_entry,
                            PineExecutionAdapter::PositionTailWork& work) {
    double result = 0.0;
    for (std::size_t index = end; index > base;) {
        const Trade& trade = read(--index);
        ++work.replay_rows;
        if (trade.exit_time < barrier) return result;
        if (include(trade.is_long, trade.entry_incarnation, trade.exit_id == kMarginCallLabel,
                    [&] { return trade.entry_id == from_entry; })) result += trade.qty;
    }
    if (spill) {
        auto cursor = spill->size_bytes();
        PositionTailSpill::Row trade;
        while (spill->previous(cursor, trade)) {
            ++work.replay_rows;
            if (trade.exit_time < barrier) break;
            if (include(trade.is_long, trade.entry_incarnation, trade.margin_call,
                        [&] { return spill->entry_id_equals(trade, from_entry); })) result += trade.qty;
        }
    }
    return result;
}

} // namespace pineforge::source::detail
