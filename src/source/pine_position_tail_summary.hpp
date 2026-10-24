#pragma once

#include "pine_position_tail.hpp"

#include <pineforge/detail/adapter_exposure_storage.hpp>
#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace pineforge::source::detail {

using PositionTailGuard = PineExecutionAdapter::PositionTailGuard;

// The adapter's existing test-TU seam supplies PositionTailDiagnostics.
// The default instantiation has no diagnostic branches, increments, clock
// reads or callbacks. Neither choice depends on NDEBUG or a fixture value.
struct PositionTailNoDiagnostics {
    static constexpr bool enabled = false;
};
struct PositionTailDiagnostics {
    static constexpr bool enabled = true;
};

template<class Diagnostics = PositionTailNoDiagnostics>
struct PositionTailRejection {
    PositionTailGuard reason = PositionTailGuard::None;
    PositionTailGuard value() const noexcept { return reason; }
    void reject(PositionTailGuard value) noexcept {
        if constexpr (Diagnostics::enabled)
            if (reason == PositionTailGuard::None) reason = value;
    }
};
template<>
struct PositionTailRejection<PositionTailNoDiagnostics> {
    constexpr PositionTailGuard value() const noexcept { return PositionTailGuard::None; }
    void reject(PositionTailGuard) const noexcept {}
};

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
    PositionTailGuard reason = PositionTailGuard::None; // occupies existing padding

    template<class Diagnostics = PositionTailNoDiagnostics>
    void add(double value, PositionTailRejection<Diagnostics>& rejection) noexcept {
        if (!valid) return;
        if (!std::isfinite(value) || std::signbit(value)) {
            rejection.reject(PositionTailGuard::InvalidQuantity);
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
        merge(term, rejection);
    }

    void add(double value) noexcept {
        PositionTailRejection<> rejection;
        add(value, rejection);
    }

    template<class Diagnostics>
    void merge(const PositionTailExact& other,
               PositionTailRejection<Diagnostics>& rejection) noexcept {
        if (!valid || !other.valid) {
            rejection.reject(PositionTailGuard::InvalidCertificate);
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
        const auto shift = [&](std::uint64_t& value, int bits) {
            if (bits >= 53 || value > (maximum >> bits)) {
                if constexpr (Diagnostics::enabled)
                    rejection.reject(bits >= 53 ? PositionTailGuard::QuantumShift
                                                : PositionTailGuard::MagnitudeShift);
                return false;
            }
            value <<= bits;
            return true;
        };
        if (!shift(left, quantum - common) || !shift(right, other.quantum - common)
            || right > maximum - left) {
            rejection.reject(PositionTailGuard::MagnitudeOverflow);
            valid = false;
            return;
        }
        magnitude = left + right;
        quantum = static_cast<std::int16_t>(common);
    }

    template<class Diagnostics>
    bool read(double& result, PositionTailRejection<Diagnostics>& rejection) const noexcept {
        if (!valid) {
            rejection.reject(PositionTailGuard::InvalidCertificate);
            return false;
        }
        const double value = std::ldexp(static_cast<double>(magnitude), quantum);
        if (!std::isfinite(value)) {
            rejection.reject(PositionTailGuard::NonfiniteResult);
            return false;
        }
        result = value;
        return true;
    }

    bool read(double& result) const noexcept {
        PositionTailRejection<> rejection;
        return read(result, rejection);
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
    static_assert(sizeof(Margins) == 32, "unchanged disk summary width");
    // Per-position derived summaries. Small identity sets never enter the
    // scratch tree; resetting the proof drops their unreachable dirty values.
    // Excess/oversized ids keep the existing exact disk representation.
    static constexpr std::size_t kMarginRows = 32;
    static constexpr std::size_t kMarginKeyBytes = 128;
    struct MarginRow {
        std::array<char, kMarginKeyBytes> key{};
        Margins value{};
        std::size_t key_size = 0;
        std::uint64_t offset = 0; // Zero until this row actually spills.
        bool valid = false, dirty = false;
    };

public:
    PineExecutionAdapter::PositionTailWork work{};

    void set_directory(const std::string& directory) { margins_.set_directory(directory); }

    // This cache can be discarded at any point: uncertified/missing facts
    // ALWAYS select replay. It owns no logical broker transition. Reset
    // rebuilds reachable equal-time rows from the current row source rather
    // than assuming an empty position tail at an opening/reversal.
    template<class Read, class Diagnostics = PositionTailNoDiagnostics>
    void begin_position(std::int64_t barrier, std::size_t base, std::size_t end, Read&& read,
                        Diagnostics diagnostics = {}) {
        if constexpr (Diagnostics::enabled) {
            ++work.begin_updates;
            unsafe_reason_ = PositionTailGuard::None;
        }
        forget_origins();
        clear_margins();
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
            if constexpr (Diagnostics::enabled) {
                ++work.host_rows;
                ++work.begin_rows;
            }
            if (row.exit_time < barrier) break; // strict barrier, equality survives
            if (have_time_ && row.exit_time > newer_time) {
                invalidate(PositionTailGuard::NonmonotoneTime, diagnostics);
                break;
            }
            if (!have_time_) last_time_ = row.exit_time;
            have_time_ = true;
            newer_time = row.exit_time;
            accumulate(row, diagnostics);
        }
    }

    template<class Read, class Diagnostics = PositionTailNoDiagnostics>
    void observe(std::size_t base, std::size_t end, Read&& read, Diagnostics diagnostics = {}) {
        if constexpr (Diagnostics::enabled) ++work.sync_updates;
        if (!active_ || !safe_) return;
        if (end < observed_end_ || observed_end_ < base) {
            if constexpr (Diagnostics::enabled)
                unsafe_reason_ = end < observed_end_ ? PositionTailGuard::PublicationRewind
                                                    : PositionTailGuard::PublicationGap;
            invalidate(PositionTailGuard::Invalidated, diagnostics);
            return;
        }
        while (observed_end_ < end) {
            const Trade& row = read(observed_end_++);
            ++work.update_rows;
            if constexpr (Diagnostics::enabled) {
                ++work.host_rows;
                ++work.closed_updates;
            }
            if (row.exit_time < barrier_ || (have_time_ && row.exit_time < last_time_)) {
                if constexpr (Diagnostics::enabled)
                    unsafe_reason_ = row.exit_time < barrier_ ? PositionTailGuard::BeforeBarrier
                                                             : PositionTailGuard::NonmonotoneTime;
                invalidate(PositionTailGuard::Invalidated, diagnostics);
                return;
            }
            last_time_ = row.exit_time;
            have_time_ = true;
            accumulate(row, diagnostics);
        }
    }

    template<class Diagnostics = PositionTailNoDiagnostics>
    void open_origin(std::uint64_t incarnation, bool is_long, Diagnostics = {}) {
        if constexpr (Diagnostics::enabled) ++work.open_updates;
        if (!safe_ || !active_) return;
        auto found = origins_.find(incarnation);
        if (found == origins_.end()) {
            Origin row;
            row.is_long = is_long;
            // A genuinely new incarnation cannot have earlier completed
            // rows. An old/reactivated incarnation whose subtotal was
            // retired is uncertain and must replay instead of starting at 0.
            row.completed.valid = incarnation > high_closed_origin_;
            if constexpr (Diagnostics::enabled)
                if (!row.completed.valid)
                    row.completed.reason = PositionTailGuard::ReactivatedOrigin;
            origins_.emplace(incarnation, row);
        } else if (found->second.is_long != is_long) {
            if constexpr (Diagnostics::enabled)
                if (found->second.completed.reason == PositionTailGuard::None)
                    found->second.completed.reason = PositionTailGuard::SideMismatch;
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

    template<class Diagnostics = PositionTailNoDiagnostics>
    void invalidate(PositionTailGuard reason = PositionTailGuard::Invalidated, Diagnostics = {}) {
        if constexpr (Diagnostics::enabled) {
            ++work.invalidations;
            if (unsafe_reason_ == PositionTailGuard::None
                || unsafe_reason_ == PositionTailGuard::Inactive)
                unsafe_reason_ = reason;
        }
        safe_ = false;
        forget_origins();
        clear_margins();
        all_margins_ = {};
    }

    template<class Select, class Diagnostics = PositionTailNoDiagnostics>
    bool ancestry(bool is_long, Select&& select, double& result, Diagnostics diagnostics = {}) {
        ++work.queries;
        if (!safe_ || !active_) return finish(false, unsafe_reason_, diagnostics);
        if (++query_generation_ == 0) {
            for (auto& row : origins_) row.second.visited = 0;
            ++query_generation_;
        }
        PositionTailExact total;
        PositionTailRejection<Diagnostics> rejection;
        select([&](std::uint64_t incarnation) {
            ++work.origin_visits;
            const auto found = origins_.find(incarnation);
            if (found == origins_.end()) {
                // No row for a fresh incarnation means an exact zero.
                // Missing older origins are deliberately uncertified.
                if (incarnation <= high_closed_origin_) {
                    rejection.reject(PositionTailGuard::MissingOrigin);
                    total.valid = false;
                }
                return;
            }
            auto& row = found->second;
            if (row.visited == query_generation_) return; // duplicate cohort membership
            row.visited = query_generation_;
            if (!row.completed.valid) {
                if constexpr (Diagnostics::enabled)
                    rejection.reject(row.completed.reason == PositionTailGuard::None
                        ? PositionTailGuard::InvalidCertificate : row.completed.reason);
                total.valid = false;
            } else if (row.is_long == is_long) total.merge(row.completed, rejection);
        });
        const bool certified = total.read(result, rejection);
        return finish(certified, rejection.value(), diagnostics);
    }

    template<class Diagnostics = PositionTailNoDiagnostics>
    bool margin(bool is_long, const std::string* named_entry, double& result,
                Diagnostics diagnostics = {}) {
        ++work.queries;
        if (!safe_ || !active_) return finish(false, unsafe_reason_, diagnostics);
        const auto side = is_long ? 1 : 0;
        PositionTailRejection<Diagnostics> rejection;
        if (!named_entry) {
            if constexpr (Diagnostics::enabled)
                rejection.reject(all_margins_.side[side].reason);
            const bool certified = all_margins_.side[side].read(result, rejection);
            return finish(certified, rejection.value(), diagnostics);
        }
        const auto value = find_margin(*named_entry);
        if (!value) {
            result = 0.0;
            return finish(true, PositionTailGuard::None, diagnostics);
        }
        if constexpr (Diagnostics::enabled) rejection.reject(value->side[side].reason);
        const bool certified = value->side[side].read(result, rejection);
        return finish(certified, rejection.value(), diagnostics);
    }

private:
    void clear_margins() noexcept {
        // No write-back: the old position's entire derived table is dead.
        for (auto& row : resident_margins_) row.valid = row.dirty = false;
        next_margin_ = 0;
        margins_.clear();
    }
    MarginRow* resident_margin(const std::string& key) noexcept {
        if (key.size() > kMarginKeyBytes) return nullptr;
        for (auto& row : resident_margins_)
            if (row.valid && row.key_size == key.size()
                && std::string_view(row.key.data(), row.key_size) == key) return &row;
        return nullptr;
    }
    MarginRow& retain_margin(const std::string& key, const Margins& value,
                             std::uint64_t offset, bool dirty) {
        auto& row = resident_margins_[next_margin_];
        if (row.valid && row.dirty) {
            // Persist the CURRENT subtotal before changing the key or value.
            // A failed insertion/write keeps the owned dirty victim intact.
            if (row.offset == 0) {
                const auto inserted = margins_.insert(
                    std::string(row.key.data(), row.key_size), row.value);
                if (!inserted.second) margins_.set(inserted.first, row.value);
            } else margins_.set(row.offset, row.value);
        }
        if (!key.empty()) std::memcpy(row.key.data(), key.data(), key.size());
        row.key_size = key.size();
        row.value = value;
        row.offset = offset;
        row.valid = true;
        row.dirty = dirty;
        next_margin_ = (next_margin_ + 1) % kMarginRows;
        return row;
    }
    std::optional<Margins> find_margin(const std::string& key) {
        if (auto* row = resident_margin(key)) return row->value;
        // With no spill every possible live value is in the fixed array.
        if (margins_.size() == 0) return std::nullopt;
        const auto offset = margins_.find(key);
        if (!offset) return std::nullopt;
        const auto value = margins_.get(*offset);
        if (key.size() <= kMarginKeyBytes) retain_margin(key, value, *offset, false);
        return value;
    }
    MarginRow& writable_margin(const std::string& key) {
        if (auto* row = resident_margin(key)) return *row;
        const auto offset = margins_.size() == 0 ? std::optional<std::uint64_t>{}
                                                : margins_.find(key);
        const auto value = offset ? margins_.get(*offset) : Margins{};
        return retain_margin(key, value, offset.value_or(0), false);
    }
    template<class Diagnostics>
    bool finish(bool certified, PositionTailGuard reason, Diagnostics) noexcept {
        if (certified) ++work.certified_queries;
        else ++work.fallback_queries;
        if constexpr (Diagnostics::enabled) {
            work.guard_reason = certified ? PositionTailGuard::None : reason;
            ++work.guard_reasons[static_cast<std::size_t>(work.guard_reason)];
        }
        return certified;
    }

    template<class Diagnostics>
    void accumulate(const Trade& trade, Diagnostics) {
        high_closed_origin_ = std::max(high_closed_origin_, trade.entry_incarnation);
        const auto origin = origins_.find(trade.entry_incarnation);
        if (origin != origins_.end()) {
            PositionTailRejection<Diagnostics> rejection;
            if constexpr (Diagnostics::enabled) rejection.reason = origin->second.completed.reason;
            if (origin->second.is_long != trade.is_long) {
                rejection.reject(PositionTailGuard::SideMismatch);
                origin->second.completed.valid = false;
            }
            origin->second.completed.add(trade.qty, rejection);
            if constexpr (Diagnostics::enabled)
                origin->second.completed.reason = rejection.reason;
        }
        if (trade.exit_id != kMarginCallLabel) return;
        const auto side = trade.is_long ? 1 : 0;
        PositionTailRejection<Diagnostics> all_rejection, named_rejection;
        if constexpr (Diagnostics::enabled)
            all_rejection.reason = all_margins_.side[side].reason;
        all_margins_.side[side].add(trade.qty, all_rejection);
        if constexpr (Diagnostics::enabled)
            all_margins_.side[side].reason = all_rejection.reason;
        auto* resident = trade.entry_id.size() <= kMarginKeyBytes
            ? &writable_margin(trade.entry_id) : nullptr;
        const auto offset = resident ? 0 : margins_.insert(trade.entry_id, Margins{}).first;
        auto value = resident ? resident->value : margins_.get(offset);
        if constexpr (Diagnostics::enabled) named_rejection.reason = value.side[side].reason;
        value.side[side].add(trade.qty, named_rejection);
        if constexpr (Diagnostics::enabled) value.side[side].reason = named_rejection.reason;
        if (resident) {
            resident->value = value;
            resident->dirty = true;
        } else margins_.set(offset, value);
    }

    Origins origins_;
    std::unordered_set<const void*> live_cohorts_;
    // Historical margin ids remain queryable after their last live origin
    // drains. Only eviction/oversized keys reach the external tree.
    ::pineforge::detail::DiskIdTable<Margins> margins_;
    std::array<MarginRow, kMarginRows> resident_margins_{};
    std::size_t next_margin_ = 0;
    Margins all_margins_{};
    std::uint64_t high_closed_origin_ = 0;
    std::size_t observed_end_ = 0;
    std::int64_t barrier_ = 0, last_time_ = 0;
    std::uint32_t query_generation_ = 0;
    bool active_ = false, safe_ = false, have_time_ = false;
    PositionTailGuard unsafe_reason_ = PositionTailGuard::Inactive;
};

// The fallback is deliberately the old ordered fold over the row source.
// The predicate and the additions have the same evaluation/order as L18:
// resident newest-to-oldest, then external newest-to-oldest, first strict
// timestamp barrier terminates the entire walk. It remains O(history).
template<class Read, class Include, class Diagnostics = PositionTailNoDiagnostics>
double replay_position_tail(std::size_t end, std::size_t base, Read&& read,
                            const PositionTailSpill* spill, std::int64_t barrier,
                            Include&& include, const std::string& from_entry,
                            PineExecutionAdapter::PositionTailWork& work, Diagnostics = {}) {
    double result = 0.0;
    for (std::size_t index = end; index > base;) {
        const Trade& trade = read(--index);
        ++work.replay_rows;
        if constexpr (Diagnostics::enabled) {
            ++work.host_rows;
            ++work.replay_host_rows;
        }
        if (trade.exit_time < barrier) return result;
        if (include(trade.is_long, trade.entry_incarnation, trade.exit_id == kMarginCallLabel,
                    [&] { return trade.entry_id == from_entry; })) result += trade.qty;
    }
    if (spill) {
        auto cursor = spill->size_bytes();
        PositionTailSpill::Row trade;
        while (spill->previous(cursor, trade)) {
            ++work.replay_rows;
            if constexpr (Diagnostics::enabled) ++work.spill_rows;
            if (trade.exit_time < barrier) break;
            if (include(trade.is_long, trade.entry_incarnation, trade.margin_call,
                        [&] { return spill->entry_id_equals(trade, from_entry); })) result += trade.qty;
        }
    }
    return result;
}

} // namespace pineforge::source::detail
