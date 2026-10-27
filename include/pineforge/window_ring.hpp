#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <vector>

namespace pineforge {

inline std::size_t window_capacity(int length, std::size_t spare = 1) {
    return (length > 0 ? static_cast<std::size_t>(length) : 0) + spare;
}

// Declared-cardinality deque replacement. No implicit eviction: algorithms
// keep their original push/pop and arithmetic order. The spare slot permits
// push-then-evict; equal-capacity checkpoint assignment reuses prepared RAM.
template<class T>
class WindowRing {
    std::vector<T> slots_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t slot(std::size_t i) const {
        const auto at = head_ + i;
        return at < slots_.size() ? at : at - slots_.size();
    }
public:
    explicit WindowRing(std::size_t capacity) : slots_(capacity) {}
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }
    void reserve_capacity(std::size_t capacity) {
        if (capacity <= slots_.size()) return;
        std::vector<T> next(capacity);
        for (std::size_t i = 0; i < size_; ++i) next[i] = (*this)[i];
        slots_.swap(next);
        head_ = 0;
    }
    bool empty() const { return size_ == 0; }
    T& operator[](std::size_t i) { return slots_[slot(i)]; }
    const T& operator[](std::size_t i) const { return slots_[slot(i)]; }
    T& front() { return (*this)[0]; }
    const T& front() const { return (*this)[0]; }
    T& back() { return (*this)[size_ - 1]; }
    const T& back() const { return (*this)[size_ - 1]; }
    void push_back(T value) {
        assert(size_ < slots_.size());
        (*this)[size_++] = value;
    }
    void push_front(T value) {
        assert(size_ < slots_.size());
        head_ = head_ ? head_ - 1 : slots_.size() - 1;
        slots_[head_] = value;
        ++size_;
    }
    void pop_front() {
        assert(size_ != 0);
        head_ = slot(1);
        --size_;
    }
    void pop_back() { assert(size_ != 0); --size_; }
    void clear() { head_ = size_ = 0; }
    class const_iterator {
        const WindowRing* ring_;
        std::size_t at_;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;
        const_iterator(const WindowRing* ring, std::size_t at) : ring_(ring), at_(at) {}
        reference operator*() const { return (*ring_)[at_]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() { ++at_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const const_iterator& rhs) const {
            return ring_ == rhs.ring_ && at_ == rhs.at_;
        }
        bool operator!=(const const_iterator& rhs) const { return !(*this == rhs); }
    };
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size_}; }
};

} // namespace pineforge
