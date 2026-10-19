#pragma once

#include <pineforge/detail/disk_id_table.hpp>

#include <cstddef>
#include <cstdint>
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
    using Row = std::pair<const std::string, Value>;

public:
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
                owner_->spill();
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

    void set_directory(const std::string& directory) { index_.set_directory(directory); }
    void clear() noexcept { resident_.clear(); index_.clear(); }
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
        for (const auto& row : resident_)
            if (row.second->first == key) return {this, row.first, row.second};
        return end();
    }
    const_iterator find_resident(const std::string& key) const noexcept {
        for (const auto& row : resident_)
            if (row.second->first == key) return {this, row.first, row.second};
        return end();
    }
    std::pair<iterator, bool> emplace(const std::string& key, Value value) {
        spill();
        const auto placed = index_.insert(key, value.retired_record());
        if (!placed.second) return {{this, placed.first, load(placed.first)}, false};
        auto row = std::make_shared<Row>(key, std::move(value));
        resident_.emplace(placed.first, row);
        return {{this, placed.first, std::move(row)}, true};
    }
    void spill() const {
        for (auto at = resident_.begin(); at != resident_.end();) {
            if (at->second.use_count() == 1 && at->second->second.spillable()) {
                index_.set(at->first, at->second->second.retired_record());
                at = resident_.erase(at);
            } else ++at;
        }
    }

private:
    template<bool Const, class Owner>
    static Iterator<Const> resident_begin(Owner* owner) noexcept {
        const auto at = owner->resident_.rbegin();
        return {owner, at == owner->resident_.rend() ? 0 : at->first,
                at == owner->resident_.rend() ? nullptr : at->second};
    }
    std::shared_ptr<Row> load(Offset at) const {
        const auto found = resident_.find(at);
        if (found != resident_.end()) return found->second;
        auto row = std::make_shared<Row>(index_.key(at), Value::restore_record(index_.get(at)));
        resident_.emplace(at, row);
        return row;
    }
    mutable Index index_;
    mutable std::map<Offset, std::shared_ptr<Row>> resident_;
};
} // namespace pineforge::detail
