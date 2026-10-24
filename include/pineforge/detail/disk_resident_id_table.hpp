#pragma once

#include <pineforge/detail/disk_id_table.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// An id index plus the rows that still own live data. Value supplies a fixed
// Record, retired_record(), restore_record(Record), and spillable(). Retired
// scalar history lives on disk; live rosters stay resident. Iterators pin a
// row across nested lookups. No destructor does I/O: eviction writes before
// erasing, at the next lookup or explicit spill(), where failures can throw.
//
// begin()/end() visit RESIDENT rows, newest first. A spilled row
// has no live members, so source decisions over live members can omit it.
// all_sorted() visits EVERY logical row, including retired ones, for hashing.
// size() is the logical count, resident_size() the physical count.
template<class Value>
class DiskResidentIdTable {
    using Index = DiskIdTable<typename Value::Record>;
    using Offset = typename Index::Offset;
    class Row;
    using Residents = std::map<Offset, std::shared_ptr<Row>>;
    class Row {
        friend class DiskResidentIdTable;
        std::string key_;
        typename Residents::iterator resident_at_{};
        Row* left_ = nullptr;
        Row* right_ = nullptr;
        Row* parent_ = nullptr;
        std::uint32_t height_ = 1;
    public:
        const std::string& first;
        Value second;
        template<class Key, class Facts>
        Row(Key&& key, Facts&& value)
            : key_(std::forward<Key>(key)), first(key_), second(std::forward<Facts>(value)) {}
        Row(const Row& other) : Row(other.first, other.second) {}
        Row(Row&& other) : Row(other.first, std::move(other.second)) {}
        Row& operator=(const Row&) = delete;
        Row& operator=(Row&&) = delete;
    };
    struct WalkCache {
        typename Residents::node_type spare;
    };
    struct NoWalkCache {};

public:
    template<bool Const, bool Sorted = false, bool Insertion = false>
    class Iterator : private std::conditional_t<Sorted, WalkCache, NoWalkCache> {
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
        // Pins are shared; unused recyclable nodes belong to one cursor only.
        Iterator(const Iterator& other) noexcept
            : owner_(other.owner_), at_(other.at_), row_(other.row_) {}
        Iterator& operator=(const Iterator& other) noexcept {
            if (this != &other) {
                if constexpr (Sorted) this->spare = {};
                owner_ = other.owner_;
                at_ = other.at_;
                row_ = other.row_;
            }
            return *this;
        }
        Iterator(Iterator&&) noexcept = default;
        Iterator& operator=(Iterator&&) noexcept = default;
        reference operator*() const noexcept { return *row_; }
        pointer operator->() const noexcept { return row_.get(); }
        Iterator& operator++() {
            if constexpr (Sorted) {
                const auto next = owner_->index_.next_sorted(at_);
                owner_->template recycle_sorted<Const>(at_, row_, this->spare);
                at_ = next;
                if (at_ != 0) row_ = owner_->load_sorted(at_, this->spare);
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
    DiskResidentIdTable(DiskResidentIdTable&& other) noexcept
        : index_(std::move(other.index_)), resident_(std::move(other.resident_)),
          resident_root_(std::exchange(other.resident_root_, nullptr)) {}
    DiskResidentIdTable& operator=(DiskResidentIdTable&& other) noexcept {
        if (this != &other) {
            clear();
            index_ = std::move(other.index_);
            resident_ = std::move(other.resident_);
            resident_root_ = std::exchange(other.resident_root_, nullptr);
        }
        return *this;
    }

    void set_store(ScratchStore store) noexcept { index_.set_store(store); }
    void set_directory(const std::string& directory) { index_.set_directory(directory); }
    void bind_scratch(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource) {
        index_.bind_scratch(slab, resource);
    }
    void require_insert(const std::string& key) const { index_.require_insert(key); }
    template<class Enumerate> void require_inserts(Enumerate&& enumerate) const {
        index_.require_inserts(std::forward<Enumerate>(enumerate));
    }
    void clear() noexcept { resident_root_ = nullptr; resident_.clear(); index_.clear(); }
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
        spill();
        const auto at = index_.find(key);
        return at ? iterator{this, *at, load(*at)} : end();
    }
    const_iterator find(const std::string& key) const {
        spill();
        const auto at = index_.find(key);
        return at ? const_iterator{this, *at, load(*at)} : end();
    }
    // A retired row cannot carry exposure/opened handles. These reads stay
    // allocation-free and noexcept even when the disk is unavailable.
    iterator find_resident(const std::string& key) noexcept {
        if (const auto* row = resident_row(key))
            return {this, row->resident_at_->first, row->resident_at_->second};
        return end();
    }
    const_iterator find_resident(const std::string& key) const noexcept {
        if (const auto* row = resident_row(key))
            return {this, row->resident_at_->first, row->resident_at_->second};
        return end();
    }
    std::pair<iterator, bool> emplace(const std::string& key, Value value) {
        // A refused new logical row must not first overwrite/evict an eligible
        // resident row. This search reads index bytes only, without restoring.
        index_.require_insert(key);
        spill();
        const auto placed = index_.insert(key, value.retired_record());
        if (!placed.second) return {{this, placed.first, load(placed.first)}, false};
        auto row = std::make_shared<Row>(key, std::move(value));
        index_resident(resident_.emplace(placed.first, row).first);
        return {{this, placed.first, std::move(row)}, true};
    }
    void spill() const {
        for (auto at = resident_.begin(); at != resident_.end();) {
            if (at->second.use_count() == 1 && at->second->second.spillable()) {
                index_.set(at->first, at->second->second.retired_record());
                unindex_resident(at->second.get());
                at = resident_.erase(at);
            } else ++at;
        }
    }

private:
    // D-06: an intrusive AVL index over current resident rows. These links
    // are cache metadata, not logical state, and allocate no second map node.
    Row* resident_row(const std::string& key) const noexcept {
        auto* row = resident_root_;
        while (row) {
            const int order = key.compare(row->first);
            if (order == 0) return row;
            row = order < 0 ? row->left_ : row->right_;
        }
        return nullptr;
    }
    static std::uint32_t resident_height(const Row* row) noexcept {
        return row ? row->height_ : 0;
    }
    static int resident_balance(const Row* row) noexcept {
        return static_cast<int>(resident_height(row->left_))
            - static_cast<int>(resident_height(row->right_));
    }
    static void refresh_resident(Row* row) noexcept {
        row->height_ = 1 + std::max(resident_height(row->left_), resident_height(row->right_));
    }
    void replace_resident(Row* row, Row* replacement) const noexcept {
        if (!row->parent_) resident_root_ = replacement;
        else if (row->parent_->left_ == row) row->parent_->left_ = replacement;
        else row->parent_->right_ = replacement;
        if (replacement) replacement->parent_ = row->parent_;
    }
    Row* rotate_resident(Row* row, bool left) const noexcept {
        auto* top = left ? row->right_ : row->left_;
        auto* middle = left ? top->left_ : top->right_;
        replace_resident(row, top);
        row->parent_ = top;
        if (left) {
            row->right_ = middle;
            top->left_ = row;
        } else {
            row->left_ = middle;
            top->right_ = row;
        }
        if (middle) middle->parent_ = row;
        refresh_resident(row);
        refresh_resident(top);
        return top;
    }
    void rebalance_resident(Row* row) const noexcept {
        while (row) {
            refresh_resident(row);
            const int lean = resident_balance(row);
            if (lean > 1) {
                if (resident_balance(row->left_) < 0) rotate_resident(row->left_, true);
                row = rotate_resident(row, false);
            } else if (lean < -1) {
                if (resident_balance(row->right_) > 0) rotate_resident(row->right_, false);
                row = rotate_resident(row, true);
            }
            row = row->parent_;
        }
    }
    void index_resident(typename Residents::iterator at) const noexcept {
        auto* row = at->second.get();
        row->resident_at_ = at;
        row->left_ = row->right_ = row->parent_ = nullptr;
        row->height_ = 1;
        auto** link = &resident_root_;
        Row* parent = nullptr;
        while (*link) {
            parent = *link;
            link = row->first.compare(parent->first) < 0 ? &parent->left_ : &parent->right_;
        }
        *link = row;
        row->parent_ = parent;
        rebalance_resident(parent);
    }
    void unindex_resident(Row* row) const noexcept {
        Row* changed = row->parent_;
        if (!row->left_) replace_resident(row, row->right_);
        else if (!row->right_) replace_resident(row, row->left_);
        else {
            auto* successor = row->right_;
            while (successor->left_) successor = successor->left_;
            if (successor->parent_ != row) {
                changed = successor->parent_;
                replace_resident(successor, successor->right_);
                successor->right_ = row->right_;
                successor->right_->parent_ = successor;
            } else changed = successor;
            replace_resident(row, successor);
            successor->left_ = row->left_;
            successor->left_->parent_ = successor;
            refresh_resident(successor);
        }
        row->left_ = row->right_ = row->parent_ = nullptr;
        row->height_ = 1;
        rebalance_resident(changed);
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
    template<bool Const>
    void recycle_sorted(Offset offset, std::shared_ptr<Row>& row,
                        typename Residents::node_type& spare) const {
        // D-05: do not sweep unrelated residents or write an unchanged row.
        // A copied iterator/Range pin prevents both eviction and buffer reuse.
        if (row.use_count() == 2) {
            // Storage predicate hooks use the resident iterator named at.
            const auto at = row->resident_at_;
            if (at->second->second.spillable()) {
                const auto value = at->second->second.retired_record();
                const auto stored = index_.get(offset);
                const bool unchanged = std::memcmp(&value, &stored, sizeof value) == 0;
                if (unchanged || !Const) {
                    if (!unchanged) index_.set(offset, value);
                    unindex_resident(at->second.get());
                    spare = resident_.extract(at);
                }
            }
        }
        row.reset();
    }
    std::shared_ptr<Row> load_sorted(Offset at, typename Residents::node_type& spare) const {
        const auto found = resident_.find(at);
        if (found != resident_.end()) return found->second;
        if (spare.empty()) return load(at);
        auto& row = spare.mapped();
        index_.key(at, row->key_);
        row->second = restore_record(index_.get(at), 0);
        spare.key() = at;
        const auto placed = resident_.insert(std::move(spare)).position;
        index_resident(placed);
        return placed->second;
    }
    std::shared_ptr<Row> load(Offset at) const {
        const auto found = resident_.find(at);
        if (found != resident_.end()) return found->second;
        auto row = std::make_shared<Row>(index_.key(at), restore_record(index_.get(at), 0));
        index_resident(resident_.emplace(at, row).first);
        return row;
    }
    mutable Index index_;
    mutable Residents resident_;
    mutable Row* resident_root_ = nullptr;
};
} // namespace pineforge::detail
