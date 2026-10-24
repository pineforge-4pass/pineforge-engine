#pragma once

#include <pineforge/detail/drawing_records.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <utility>
#include <vector>

namespace pineforge::detail {

// A declared cap fitting one page materializes its entire ring before use.
// Larger caps provision the same index space in sparse external blocks.
// There is no sentinel special case, reduced cap or runtime deque.
class DrawingOrder {
    static constexpr std::size_t inline_slots = o1::Shape{}.page_bytes / sizeof(std::int32_t);
    std::vector<std::int32_t> slots_;
    DrawingExternalRecords<std::int32_t> external_;
    std::size_t capacity_;
    std::size_t first_ = 0;
    std::size_t used_ = 0;

    std::int32_t& write_slot(std::size_t index) {
        return is_external() ? external_.write(static_cast<std::uint32_t>(index)) : slots_[index];
    }
    void erase_shift(std::size_t position) {
        for (std::size_t i = position; i + 1 < used_; ++i) {
            const std::int32_t value = (*this)[i + 1];
            write_slot((first_ + i) % capacity_) = value;
        }
        --used_;
    }
public:
    class PreparedPush {
        DrawingOrder* ring_;
        std::int32_t* slot_;
        friend class DrawingOrder;
        PreparedPush(DrawingOrder& ring, std::int32_t& slot) : ring_(&ring), slot_(&slot) {}
    public:
        // Only pop_front may run on this ring before commit; arena record
        // writes use another cache and cannot invalidate the prepared slot.
        void commit(std::int32_t id) noexcept { *slot_ = id; ++ring_->used_; }
    };
    class const_iterator {
        const DrawingOrder* ring_ = nullptr;
        std::size_t at_ = 0;
        friend class DrawingOrder;
        const_iterator(const DrawingOrder* ring, std::size_t at) : ring_(ring), at_(at) {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::int32_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;
        const_iterator() = default;
        reference operator*() const { return (*ring_)[at_]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() { ++at_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const const_iterator& rhs) const {
            return ring_ == rhs.ring_ && at_ == rhs.at_;
        }
        bool operator!=(const const_iterator& rhs) const { return !(*this == rhs); }
    };

    explicit DrawingOrder(std::size_t cap) : capacity_(cap) {
        if (is_external()) external_.reserve_sparse_slots(static_cast<std::uint32_t>(cap));
        else slots_.resize(cap);
    }
    DrawingOrder(const DrawingOrder&) = default;
    DrawingOrder& operator=(const DrawingOrder& rhs) {
        if (this != &rhs) {
            prepare_assignment(rhs);
            external_ = rhs.external_;
            slots_ = rhs.slots_; // same-cap restores reuse already materialized slots
            capacity_ = rhs.capacity_;
            first_ = rhs.first_;
            used_ = rhs.used_;
        }
        return *this;
    }
    void prepare_assignment(const DrawingOrder& rhs) {
        slots_.reserve(rhs.slots_.size()); // cap changes prepare before publishing a root
    }
    bool is_external() const noexcept { return capacity_ > inline_slots; }
    void swap(DrawingOrder& rhs) noexcept {
        using std::swap;
        slots_.swap(rhs.slots_);
        external_.swap(rhs.external_);
        swap(capacity_, rhs.capacity_);
        swap(first_, rhs.first_);
        swap(used_, rhs.used_);
    }
    std::size_t size() const noexcept { return used_; }
    bool empty() const noexcept { return used_ == 0; }
    const std::int32_t& operator[](std::size_t at) const {
        const auto index = (first_ + at) % capacity_;
        return is_external() ? external_.read(static_cast<std::uint32_t>(index)) : slots_[index];
    }
    const std::int32_t& front() const { return (*this)[0]; }
    const std::int32_t& back() const { return (*this)[used_ - 1]; }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, used_}; }
    void clear() noexcept { first_ = used_ = 0; }
    void pop_front() noexcept {
        first_ = (first_ + 1) % capacity_;
        --used_;
    }
    PreparedPush prepare_push() {
        return {*this, write_slot((first_ + used_) % capacity_)};
    }
    void push_back(std::int32_t id) { prepare_push().commit(id); }
    DrawingOrder erased(const_iterator at) const {
        DrawingOrder next(*this);
        next.erase_shift(at.at_);
        return next;
    }
    void erase(const_iterator at) {
        if (is_external()) {
            auto next = erased(at);
            swap(next);
        } else erase_shift(at.at_);
    }

    // Explicit legacy consumer boundary; the arena never constructs a deque.
    operator std::deque<std::int32_t>() const { return {begin(), end()}; }
    template<class Sequence> bool operator==(const Sequence& rhs) const {
        return size() == rhs.size() && std::equal(begin(), end(), rhs.begin());
    }
    template<class Sequence> bool operator!=(const Sequence& rhs) const { return !(*this == rhs); }
};

} // namespace pineforge::detail
