#pragma once

#include <pineforge/native_order_identity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace pineforge::source::detail {

struct LiveHandleHash {
    std::size_t operator()(const native_order::RequestHandle& h) const noexcept {
        auto value = std::hash<std::string>{}(h.run.session_key.string());
        value ^= std::hash<std::uint64_t>{}(h.run.run_number) + (value << 6) + (value >> 2);
        return value ^ (std::hash<std::uint64_t>{}(h.incarnation) + (value << 6) + (value >> 2));
    }
};

// Canonical live order, with derived slot membership. No tombstone, position,
// ordinal or cached snapshot belongs to a hash/continuation. Resident
// prototype: slots <= 2*live between mutations, capacity follows peak live.
class LiveHandleSequence {
    using Handle = native_order::RequestHandle;
    struct Slot { Handle handle; std::uint64_t order; bool live = true; std::size_t occurrence = 0; };
public:
    class const_iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = Handle;
        using difference_type = std::ptrdiff_t;
        using pointer = const Handle*;
        using reference = const Handle&;
        const_iterator() = default;
        reference operator*() const { return owner_->slots_[position_].handle; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() { ++position_; skip(); return *this; }
        const_iterator operator++(int) { auto before = *this; ++*this; return before; }
        const_iterator& operator--() {
            do { --position_; } while (!owner_->slots_[position_].live);
            return *this;
        }
        const_iterator operator--(int) { auto before = *this; --*this; return before; }
        bool operator==(const const_iterator& b) const {
            return owner_ == b.owner_ && position_ == b.position_;
        }
        bool operator!=(const const_iterator& b) const { return !(*this == b); }
    private:
        friend class LiveHandleSequence;
        const_iterator(const LiveHandleSequence* owner, std::size_t position)
            : owner_(owner), position_(position) { skip(); }
        void skip() {
            while (position_ < owner_->slots_.size() && !owner_->slots_[position_].live)
                ++position_;
        }
        const LiveHandleSequence* owner_ = nullptr;
        std::size_t position_ = 0;
    };
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using iterator = const_iterator;
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, slots_.size()}; }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    std::size_t size() const noexcept { return live_count_; }
    bool empty() const noexcept { return positions_.empty(); }
    bool contains(const Handle& h) const noexcept { return positions_.find(h) != positions_.end(); }
    const Handle& front() const { return *begin(); }
    const Handle& back() const { return *rbegin(); }
    const Handle& operator[](std::size_t rank) const { return snapshot()[rank]; }
    std::uint64_t order(const Handle& h) const { return slots_[positions_.at(h).front()].order; }
    std::vector<std::uint64_t> orders(const Handle& h) const;
    void push_back(const Handle&);
    // Returns actual slot work (lookup + stable-compaction visits), so the
    // temporary diagnostic does not hide compaction inside an uncounted call.
    std::size_t erase_handle(const Handle&) noexcept;
    void move_to_tail(const Handle&);
    void clear() noexcept;
    void assign(const std::vector<Handle>&);
    const std::vector<Handle>& snapshot() const;
    std::vector<Handle> take();
    template<class Compare> void stable_sort(Compare compare) {
        auto compact = snapshot();
        std::stable_sort(compact.begin(), compact.end(), compare);
        assign(compact);
    }
private:
    std::size_t compact_if_needed() noexcept;
    std::vector<Slot> slots_;
    // Production remember suppresses duplicates. Raw observer fixtures can
    // deliberately append aliases; preserve those vector-visible occurrences.
    std::unordered_map<Handle, std::vector<std::size_t>, LiveHandleHash> positions_;
    std::size_t live_count_ = 0;
    mutable std::vector<Handle> snapshot_;
    mutable bool snapshot_valid_ = false;
    std::uint64_t next_order_ = 0;
};

} // namespace pineforge::source::detail
