#pragma once

#include <pineforge/detail/disk_id_table.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace pineforge::detail {

// An id index plus live rows and a fixed-budget cache of retired rows. Value
// supplies a fixed Record, retired_record(), restore_record(Record), and
// spillable(). For a spillable row the record must preserve its logical value.
// Retired scalar history lives on disk; live rosters stay resident. Iterators
// pin a row across nested lookups. No destructor writes back: eviction writes
// before erasing, at a lookup/insertion or explicit spill(), where failures
// can throw.
//
// begin()/end() visit RESIDENT rows, newest first. A spilled row
// has no live members, so source decisions over live members can omit it.
// all_sorted() visits EVERY logical row, including retired ones, for hashing.
// size() is the logical count, resident_size() the physical count.
template<class Value>
class DiskResidentIdTable {
    using Index = DiskIdTable<typename Value::Record>;
    using Offset = typename Index::Offset;
    using Row = std::pair<const std::string, Value>;
    using ResidentRows = std::map<Offset, std::shared_ptr<Row>>;
    // Map iterators do not own/pin a Row and survive inserts and table moves.
    using ResidentKeys = std::unordered_map<std::string, typename ResidentRows::iterator>;

public:
    // Budget for lookup caching, independent of the distinct-ever id count.
    // A retired CohortFacts has a 40-byte record and about 200 bytes of empty
    // live containers/scalars on 64-bit STLs: 512 rows plus both index nodes
    // cost roughly 200 KiB, plus two copies of non-SSO key bytes. Live rows
    // and caller pins may exceed this budget; they must never be evicted.
    // Oldest means first logical insertion (ascending disk offset), matching
    // the existing resident iteration order, rather than lookup recency.
    static constexpr std::size_t kResidentRows = 512;

    template<bool Const, bool Sorted = false, bool Insertion = false>
    class Iterator {
        friend class DiskResidentIdTable;
        using Owner = std::conditional_t<Const, const DiskResidentIdTable, DiskResidentIdTable>;
        Owner* owner_ = nullptr;
        Offset at_ = 0;
        std::shared_ptr<Row> row_;

        Iterator(Owner* owner, Offset at, std::shared_ptr<Row> row) noexcept
            : owner_(owner), at_(at), row_(std::move(row)) {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const Row&, Row&>;
        using pointer = std::conditional_t<Const, const Row*, Row*>;
        Iterator() = default;
        reference operator*() const noexcept { return *row_; }
        pointer operator->() const noexcept { return row_.get(); }
        Iterator& operator++() {
            if constexpr (Sorted) {
                const auto next = owner_->index_.next_sorted(at_);
                row_.reset();
                // Bounded, not eager: the state hash walks every cohort at each
                // report point; an eager flush there would write back and then
                // re-read the whole resident cache every bar (CR150-CX edit).
                owner_->spill_to(kResidentRows);
                at_ = next;
                if (at_ != 0) row_ = owner_->load(at_);
            } else if constexpr (Insertion) {
                // Nested lookups can erase other empty rows. Re-seek by
                // offset rather than keeping an invalidatable map iterator.
                const auto next = owner_->resident_.upper_bound(at_);
                at_ = next == owner_->resident_.end() ? 0 : next->first;
                row_ = at_ == 0 ? nullptr : next->second;
            } else {
                auto next = owner_->resident_.lower_bound(at_);
                if (next == owner_->resident_.begin()) {
                    at_ = 0;
                    row_.reset();
                } else {
                    --next;
                    at_ = next->first;
                    row_ = next->second;
                }
            }
            return *this;
        }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const noexcept {
            return owner_ == other.owner_ && at_ == other.at_;
        }
        bool operator!=(const Iterator& other) const noexcept { return !(*this == other); }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    using sorted_iterator = Iterator<false, true>;
    using const_sorted_iterator = Iterator<true, true>;
    using insertion_iterator = Iterator<true, false, true>;

    template<class Iter> struct Range {
        Iter first, last;
        Iter begin() const { return first; }
        Iter end() const { return last; }
    };
    template<class Iter> class KeyIterator {
        Iter at_;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::string;
        using difference_type = std::ptrdiff_t;
        using reference = const std::string&;
        using pointer = const std::string*;
        KeyIterator() = default;
        explicit KeyIterator(Iter at) : at_(std::move(at)) {}
        reference operator*() const noexcept { return at_->first; }
        pointer operator->() const noexcept { return &at_->first; }
        KeyIterator& operator++() { ++at_; return *this; }
        KeyIterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const KeyIterator& other) const noexcept { return at_ == other.at_; }
        bool operator!=(const KeyIterator& other) const noexcept { return !(*this == other); }
    };

    DiskResidentIdTable() = default;
    DiskResidentIdTable(const DiskResidentIdTable&) = delete;
    DiskResidentIdTable& operator=(const DiskResidentIdTable&) = delete;
    DiskResidentIdTable(DiskResidentIdTable&&) noexcept = default;
    DiskResidentIdTable& operator=(DiskResidentIdTable&&) noexcept = default;

    void set_store(ScratchStore store) noexcept { index_.set_store(store); }
    void set_directory(const std::string& directory) { index_.set_directory(directory); }
    void bind_scratch(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource) {
        index_.bind_scratch(slab, resource);
    }
    void require_insert(const std::string& key) const {
        if (resident_at(key) == resident_.end()) index_.require_insert(key);
    }
    template<class Enumerate> void require_inserts(Enumerate&& enumerate) const {
        index_.require_inserts(std::forward<Enumerate>(enumerate));
    }
    void clear() noexcept {
        ResidentKeys{}.swap(resident_keys_);
        resident_.clear();
        index_.clear();
    }
    std::uint64_t size() const noexcept { return index_.size(); }
    std::size_t resident_size() const noexcept { return resident_.size(); }
    iterator begin() noexcept { return resident_begin<false>(this); }
    iterator end() noexcept { return {this, 0, {}}; }
    const_iterator begin() const noexcept { return resident_begin<true>(this); }
    const_iterator end() const noexcept { return {this, 0, {}}; }
    auto resident_ids() const noexcept {
        const auto at = resident_.begin();
        const insertion_iterator first{this, at == resident_.end() ? 0 : at->first,
                                       at == resident_.end() ? nullptr : at->second};
        return Range<KeyIterator<insertion_iterator>>{
            KeyIterator<insertion_iterator>(first),
            KeyIterator<insertion_iterator>(insertion_iterator{this, 0, {}})};
    }
    auto all_sorted() {
        const auto at = index_.first_sorted();
        return Range<sorted_iterator>{{this, at, at ? load(at) : nullptr}, {this, 0, {}}};
    }
    auto all_sorted() const {
        const auto at = index_.first_sorted();
        return Range<const_sorted_iterator>{{this, at, at ? load(at) : nullptr}, {this, 0, {}}};
    }
    // Full insertion-order replay without a growing vector of keys.
    template<class F> void for_each_id(F&& visit) const {
        for (auto at = index_.first_inserted(); at != 0; at = index_.next_inserted(at))
            visit(index_.key(at));
    }
    // Decisions over historical ids must not depend on row residency.
    // Walk keys alone, without restoring facts or retaining a key vector.
    template<class F> void for_each_sorted_id(F&& visit) const {
        for (auto at = index_.first_sorted(); at != 0; at = index_.next_sorted(at))
            visit(index_.key(at));
    }
    iterator find(const std::string& key) {
        const auto resident = resident_at(key);
        if (resident != resident_.end()) return {this, resident->first, resident->second};
        spill_to(kResidentRows);
        const auto at = index_.find(key);
        return at ? iterator{this, *at, load(*at)} : end();
    }
    const_iterator find(const std::string& key) const {
        const auto resident = resident_at(key);
        if (resident != resident_.end()) return {this, resident->first, resident->second};
        spill_to(kResidentRows);
        const auto at = index_.find(key);
        return at ? const_iterator{this, *at, load(*at)} : end();
    }
    // A retired row cannot carry exposure/opened handles. These reads stay
    // allocation-free and noexcept even when the disk is unavailable.
    iterator find_resident(const std::string& key) noexcept {
        const auto at = resident_at(key);
        return at == resident_.end() ? end() : iterator{this, at->first, at->second};
    }
    const_iterator find_resident(const std::string& key) const noexcept {
        const auto at = resident_at(key);
        return at == resident_.end() ? end() : const_iterator{this, at->first, at->second};
    }
    std::pair<iterator, bool> emplace(const std::string& key, Value value) {
        const auto resident = resident_at(key);
        if (resident != resident_.end())
            return {{this, resident->first, resident->second}, false};
        // A refused new logical row must not first overwrite/evict an eligible
        // resident row. This search reads index bytes only, without restoring.
        index_.require_insert(key);
        spill_to(kResidentRows);
        const auto placed = index_.insert(key, value.retired_record());
        if (!placed.second) return {{this, placed.first, load(placed.first)}, false};
        auto row = std::make_shared<Row>(key, std::move(value));
        retain(placed.first, row);
        spill_to(kResidentRows); // The local shared_ptr pins the returned row.
        return {{this, placed.first, std::move(row)}, true};
    }
    void spill() const {
        spill_to(0); // Explicit flushes, including sorted walks, remain eager.
        // Do not retain a historical live-population peak in the key buckets.
        if (resident_.empty()) ResidentKeys{}.swap(resident_keys_);
        else if (resident_keys_.bucket_count() / 4 > resident_keys_.size())
            resident_keys_.rehash(resident_keys_.size());
    }

private:
    typename ResidentRows::iterator resident_at(const std::string& key) const noexcept {
        const auto found = resident_keys_.find(key);
        return found == resident_keys_.end() ? resident_.end() : found->second;
    }
    void spill_to(std::size_t limit) const {
        // The common resident-hit path never calls this, even to scan pins.
        for (auto at = resident_.begin(); resident_.size() > limit && at != resident_.end();) {
            if (at->second.use_count() == 1 && at->second->second.spillable()) {
                // References can mutate Value without a table callback.
                // Treat every resident row as potentially dirty; write its
                // CURRENT record only on eviction, before removing either
                // index. A failed write leaves the resident value/pin intact.
                index_.set(at->first, at->second->second.retired_record());
                resident_keys_.erase(at->second->first);
                at = resident_.erase(at);
            } else ++at;
        }
    }
    void retain(Offset at, const std::shared_ptr<Row>& row) const {
        const auto placed = resident_.emplace(at, row);
        try {
            resident_keys_.emplace(row->first, placed.first);
        } catch (...) {
            resident_.erase(placed.first);
            throw;
        }
    }
    // Codecs that validate stored bytes may accept their physical origin.
    // Keep the original one-argument codec contract for other value types.
    template<class T = Value>
    auto restore_record(const typename T::Record& record, int) const
        -> decltype(T::restore_record(record, bool{})) {
        return T::restore_record(record, index_.arena_backed());
    }
    Value restore_record(const typename Value::Record& record, long) const {
        return Value::restore_record(record);
    }
    template<bool Const, class Owner>
    static Iterator<Const> resident_begin(Owner* owner) noexcept {
        const auto at = owner->resident_.rbegin();
        return {owner, at == owner->resident_.rend() ? 0 : at->first,
                at == owner->resident_.rend() ? nullptr : at->second};
    }
    std::shared_ptr<Row> load(Offset at) const {
        const auto found = resident_.find(at);
        if (found != resident_.end()) return found->second;
        auto row = std::make_shared<Row>(index_.key(at), restore_record(index_.get(at), 0));
        retain(at, row);
        spill_to(kResidentRows); // Do not let a sequence of disk restores grow the cache.
        return row;
    }
    mutable Index index_;
    mutable ResidentRows resident_;
    mutable ResidentKeys resident_keys_;
};
} // namespace pineforge::detail
