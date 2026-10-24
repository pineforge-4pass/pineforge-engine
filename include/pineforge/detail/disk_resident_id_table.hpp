#pragma once

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/resident_budget.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace pineforge::detail {

// An id index plus live rows and a bounded cache of retired rows. Value
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
// SortWork is a test-only observer (comparison()/rebuild()); the void default
// removes its calls and stores no counters in production instantiations.
// KeyWork observes hash()/equal() inside the key index for tests; void keeps
// the standard hash/equality types and adds no production state or calls.
template<class Value, class SortWork = void, class KeyWork = void>
class DiskResidentIdTable {
    using Index = DiskIdTable<typename Value::Record>;
    using Offset = typename Index::Offset;
    using Row = std::pair<const std::string, Value>;
    using ResidentRows = std::map<Offset, std::shared_ptr<Row>>;
    struct ObservedKeyHash {
        std::size_t operator()(const std::string& key) const {
            if constexpr (!std::is_void_v<KeyWork>) KeyWork::hash();
            return std::hash<std::string>{}(key);
        }
    };
    struct ObservedKeyEqual {
        bool operator()(const std::string& a, const std::string& b) const {
            if constexpr (!std::is_void_v<KeyWork>) KeyWork::equal();
            return a == b;
        }
    };
    // Map iterators do not own/pin a Row and survive inserts and table moves.
    using ResidentKeys = std::unordered_map<std::string, typename ResidentRows::iterator,
        std::conditional_t<std::is_void_v<KeyWork>, std::hash<std::string>, ObservedKeyHash>,
        std::conditional_t<std::is_void_v<KeyWork>, std::equal_to<std::string>, ObservedKeyEqual>>;
    using SortedRow = const typename ResidentRows::value_type*;

public:
    // The limit counts ALL resident rows, independent of distinct-ever ids.
    // Live and caller-pinned rows count but are never evicted; if their count
    // exceeds the limit, no retired row can remain cached unless also pinned.
    // Byte accounting awaits lab measurement (resident_budget.hpp).
    // Oldest means first logical insertion (ascending disk offset), matching
    // the existing resident iteration order, rather than lookup recency.
    static constexpr std::size_t kResidentRows = static_cast<std::size_t>(
        resident_budget(ResidentKind::idtable_entries).default_entries);
    static_assert(resident_budget(ResidentKind::idtable_entries).max_entries
                  <= std::numeric_limits<std::size_t>::max(), "resident limit fits size_t");

    // Standard type sizes only: allocator-specific node/control-block overhead
    // and key/container heap allocations are deliberately not guessed here.
    struct ResidentLayout {
        std::size_t value_bytes, row_bytes, shared_ptr_bytes;
        std::size_t map_value_bytes, key_index_value_bytes;
        std::size_t map_bytes, key_index_bytes, table_bytes;
    };
    static constexpr ResidentLayout resident_layout() noexcept {
        return {sizeof(Value), sizeof(Row), sizeof(std::shared_ptr<Row>),
                sizeof(typename ResidentRows::value_type),
                sizeof(typename ResidentKeys::value_type),
                sizeof(ResidentRows), sizeof(ResidentKeys), sizeof(DiskResidentIdTable)};
    }

    template<bool Const, bool Sorted = false, bool Insertion = false>
    class Iterator {
        friend class DiskResidentIdTable;
        using Owner = std::conditional_t<Const, const DiskResidentIdTable, DiskResidentIdTable>;
        Owner* owner_ = nullptr;
        Offset at_ = 0;
        std::shared_ptr<Row> row_;
        std::size_t sorted_position_ = 0;

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
                const auto next = owner_->next_sorted(at_, row_->first, sorted_position_);
                row_.reset();
                // Bounded, not eager: the state hash walks every cohort at each
                // report point; an eager flush there would write back and then
                // re-read the whole resident cache every bar (CR150-CX edit).
                owner_->spill_to(owner_->resident_limit_);
                at_ = next;
                if (at_ != 0) row_ = owner_->load_sorted(at_, sorted_position_);
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
    // No I/O in the setter. A shrink trims at the next find() (even a
    // resident hit), emplace(), or ordinary eviction point. find_resident()
    // remains noexcept and never trims. Live rows/caller pins cannot be evicted.
    void set_resident_limit(std::size_t limit) noexcept {
        resident_limit_ = limit;
        trim_pending_ = resident_.size() > limit;
        if (sorted_size_ > limit) invalidate_sorted();
    }
    std::size_t resident_limit() const noexcept { return resident_limit_; }
    void require_insert(const std::string& key) const {
        if (resident_at(key) == resident_.end()) index_.require_insert(key);
    }
    template<class Enumerate> void require_inserts(Enumerate&& enumerate) const {
        index_.require_inserts(std::forward<Enumerate>(enumerate));
    }
    void clear() noexcept {
        invalidate_sorted();
        ResidentKeys{}.swap(resident_keys_);
        resident_.clear();
        index_.clear();
        trim_pending_ = false;
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
        const auto at = first_sorted();
        return Range<sorted_iterator>{{this, at, at ? load_sorted(at, 0) : nullptr}, {this, 0, {}}};
    }
    auto all_sorted() const {
        const auto at = first_sorted();
        return Range<const_sorted_iterator>{{this, at, at ? load_sorted(at, 0) : nullptr}, {this, 0, {}}};
    }
    // Keys only: no restore, row pin or key copy on the resident paths.
    // Visitors may insert/evict: successors are chosen AFTER the callback,
    // just like the disk links (new later keys are visited). Resident keys
    // are borrowed until a membership change; consume/copy before mutating.
    // Clearing or moving this table from a visitor invalidates the walk.
    template<class F> void for_each_id(F&& visit) const {
        if (fully_resident_within_limit()) {
            for (auto row = resident_.begin(); row != resident_.end();) {
                const auto current = row->first;
                visit(row->second->first);
                if (!fully_resident_within_limit()) {
                    for (auto at = index_.next_inserted(current); at != 0;
                         at = index_.next_inserted(at)) visit(index_.key(at));
                    return;
                }
                // The visitor can evict and restore even the current row.
                row = resident_.upper_bound(current);
            }
            return;
        }
        for (auto at = index_.first_inserted(); at != 0; at = index_.next_inserted(at))
            visit(index_.key(at));
    }
    // Decisions over historical ids must not depend on row residency.
    // Walk keys alone, without restoring facts or retaining a key vector.
    template<class F> void for_each_sorted_id(F&& visit) const {
        if (fully_resident_within_limit()) {
            ensure_sorted();
            std::size_t position = 0;
            while (position < sorted_size_) {
                const auto current = sorted_rows_[position]->first;
                visit(sorted_rows_[position]->second->first);
                if (!fully_resident_within_limit()) {
                    for (auto at = index_.next_sorted(current); at != 0;
                         at = index_.next_sorted(at)) visit(index_.key(at));
                    return;
                }
                ensure_sorted(); // The visitor may have invalidated the array.
                if (position < sorted_size_ && sorted_rows_[position]->first == current)
                    ++position;
                else {
                    // Retrieve the current key afresh: the callback may have
                    // evicted/restored its node. No shared_ptr copy pins it.
                    const auto row = resident_.find(current);
                    if (row == resident_.end())
                        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                                      "scratch id walk invalidated");
                    (void)next_sorted(current, row->second->first, position);
                }
            }
            return;
        }
        for (auto at = index_.first_sorted(); at != 0; at = index_.next_sorted(at))
            visit(index_.key(at));
    }
    iterator find(const std::string& key) {
        trim_pending_limit();
        const auto resident = resident_at(key);
        if (resident != resident_.end()) return {this, resident->first, resident->second};
        spill_to(resident_limit_);
        const auto at = index_.find(key);
        return at ? iterator{this, *at, load(*at)} : end();
    }
    const_iterator find(const std::string& key) const {
        trim_pending_limit();
        const auto resident = resident_at(key);
        if (resident != resident_.end()) return {this, resident->first, resident->second};
        spill_to(resident_limit_);
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
        if (resident != resident_.end()) {
            const auto at = resident->first;
            auto row = resident->second; // Pin the duplicate through a deferred shrink.
            trim_pending_limit();
            return {{this, at, std::move(row)}, false};
        }
        // A refused new logical row must not first overwrite/evict an eligible
        // resident row. This search reads index bytes only, without restoring.
        index_.require_insert(key);
        trim_pending_limit();
        // Make room BEFORE publishing the id: a failed eviction write must
        // leave the new key absent. Live/caller-pinned rows can exceed the limit.
        spill_to(resident_limit_ == 0 ? 0 : resident_limit_ - 1);
        const auto placed = index_.insert(key, value.retired_record());
        if (!placed.second) return {{this, placed.first, load(placed.first)}, false};
        auto row = std::make_shared<Row>(key, std::move(value));
        retain(placed.first, row);
        return {{this, placed.first, std::move(row)}, true};
    }
    // Evict only what exceeds the row limit (a no-op within it). The per-bar
    // retirement sweep calls this instead of spill(): an eager spill() there
    // flushed the whole cache every bar and defeated the resident index.
    void trim_to_budget() const { spill_to(resident_limit_); }
    void spill() const {
        spill_to(0); // Explicit flushes are eager; sorted walks retain the bounded cache.
        // Do not retain a historical live-population peak in the key buckets.
        if (resident_.empty()) ResidentKeys{}.swap(resident_keys_);
        else if (resident_keys_.bucket_count() / 4 > resident_keys_.size())
            resident_keys_.rehash(resident_keys_.size());
    }

private:
    bool fully_resident_within_limit() const noexcept {
        return resident_.size() == index_.size() && resident_.size() <= resident_limit_;
    }
    void invalidate_sorted() const noexcept {
        // Release before any node can disappear; no dangling borrowed pointers
        // or allocation retained from a larger resident population.
        sorted_rows_.reset();
        sorted_size_ = 0;
    }
    static bool resident_key_less(const std::string& a, const std::string& b) {
        if constexpr (!std::is_void_v<SortWork>) SortWork::comparison();
        return a.compare(b) < 0; // Same byte-string comparison as DiskIdTable.
    }
    void ensure_sorted() const {
        if (resident_.empty()) {
            invalidate_sorted(); // Also normalizes the size in a moved-from table.
            return;
        }
        if (sorted_rows_) return;
        // Exact-size array: one non-owning pointer per resident row, at most
        // resident_limit_. No key copies, shared pins, vector slack or second
        // buffer during a rebuild. Membership changes discard the old array.
        const auto count = resident_.size();
        auto rows = std::make_unique<SortedRow[]>(count);
        std::size_t i = 0;
        for (const auto& row : resident_) rows[i++] = &row;
        std::sort(rows.get(), rows.get() + count, [](SortedRow a, SortedRow b) {
            return resident_key_less(a->second->first, b->second->first);
        });
        sorted_rows_ = std::move(rows);
        sorted_size_ = count;
        if constexpr (!std::is_void_v<SortWork>) SortWork::rebuild();
    }
    // Build O(R log R) once per membership change. Stable walks advance by
    // ordinal without key comparisons, allocations or disk reads. Partial or
    // over-budget resident populations keep the disk-tree walk.
    Offset first_sorted() const {
        if (!fully_resident_within_limit()) return index_.first_sorted();
        ensure_sorted();
        return sorted_size_ == 0 ? 0 : sorted_rows_[0]->first;
    }
    Offset next_sorted(Offset current, const std::string& key, std::size_t& position) const {
        if (!fully_resident_within_limit()) return index_.next_sorted(current);
        ensure_sorted();
        if (position < sorted_size_ && sorted_rows_[position]->first == current) {
            ++position;
        } else {
            // An insert/restore/eviction during a nested lookup may change the
            // ordinal. Re-seek once by key; never trust a stale position.
            const auto first = sorted_rows_.get();
            const auto next = std::upper_bound(first, first + sorted_size_, key,
                [](const std::string& value, SortedRow row) {
                    return resident_key_less(value, row->second->first);
                });
            position = static_cast<std::size_t>(next - first);
        }
        return position == sorted_size_ ? 0 : sorted_rows_[position]->first;
    }
    void trim_pending_limit() const {
        if (!trim_pending_) return;
        spill_to(resident_limit_);
        trim_pending_ = false;
    }
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
                invalidate_sorted();
                resident_keys_.erase(at->second->first);
                at = resident_.erase(at);
            } else ++at;
        }
    }
    void retain(Offset at, const std::shared_ptr<Row>& row) const {
        const auto placed = resident_.emplace(at, row);
        if (!placed.second)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "scratch id duplicate resident offset");
        try {
            if (!resident_keys_.emplace(row->first, placed.first).second)
                throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                              "scratch id duplicate resident key");
        } catch (...) {
            resident_.erase(placed.first);
            throw;
        }
        invalidate_sorted();
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
    std::shared_ptr<Row> load_sorted(Offset at, std::size_t position) const {
        if (sorted_rows_ && position < sorted_size_ && sorted_rows_[position]->first == at)
            return sorted_rows_[position]->second;
        return load(at);
    }
    std::shared_ptr<Row> load(Offset at) const {
        const auto found = resident_.find(at);
        if (found != resident_.end()) return found->second;
        auto row = std::make_shared<Row>(index_.key(at), restore_record(index_.get(at), 0));
        retain(at, row);
        spill_to(resident_limit_); // Do not let a sequence of disk restores grow the cache.
        return row;
    }
    mutable Index index_;
    mutable ResidentRows resident_;
    mutable ResidentKeys resident_keys_;
    mutable std::unique_ptr<SortedRow[]> sorted_rows_;
    mutable std::size_t sorted_size_ = 0;
    std::size_t resident_limit_ = kResidentRows;
    mutable bool trim_pending_ = false;
};
} // namespace pineforge::detail
