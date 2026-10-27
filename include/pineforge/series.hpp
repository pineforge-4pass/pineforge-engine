#pragma once
#include <deque>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <utility>
#include <pineforge/detail/history_ring.hpp>
#include "na.hpp"

namespace pineforge {

template<typename T, bool Paged = std::is_trivially_copyable_v<T>>
class DynamicRingBuffer {
    std::vector<T> buffer_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

public:
    explicit DynamicRingBuffer(std::size_t capacity)
        : buffer_(capacity, na<T>()), capacity_(capacity) {}

    void push_front(T val) {
        if (capacity_ == 0) return;
        if (size_ == 0) {
            buffer_[0] = val;
            size_ = 1;
            head_ = 0;
        } else {
            head_ = (head_ == 0) ? capacity_ - 1 : head_ - 1;
            buffer_[head_] = val;
            if (size_ < capacity_) {
                size_++;
            }
        }
    }

    void update_front(T val) {
        if (size_ == 0) {
            push_front(val);
        } else {
            buffer_[head_] = val;
        }
    }

    T operator[](std::size_t offset) const {
        if (offset >= size_ || capacity_ == 0) {
            return na<T>();
        }
        // offset < size_ <= capacity_ and head_ < capacity_, so
        // head_ + offset < 2 * capacity_: one conditional subtract
        // replaces the hardware divide of `% capacity_`.
        std::size_t idx = head_ + offset;
        if (idx >= capacity_) idx -= capacity_;
        return buffer_[idx];
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }

    void clear() {
        head_ = 0;
        size_ = 0;
        std::fill(buffer_.begin(), buffer_.end(), na<T>());
    }

    void resize(std::size_t new_capacity) {
        if (new_capacity == capacity_) return;
        std::vector<T> new_buffer(new_capacity, na<T>());
        std::size_t new_size = std::min(size_, new_capacity);
        for (std::size_t i = 0; i < new_size; ++i) {
            new_buffer[i] = (*this)[i];
        }
        buffer_ = std::move(new_buffer);
        capacity_ = new_capacity;
        head_ = 0;
        size_ = new_size;
    }
};

template<typename T>
class DynamicRingBuffer<T, true> : public detail::PagedHistoryRing<T> {
public:
    using detail::PagedHistoryRing<T>::PagedHistoryRing;
};

/*
 * PineScript history indexing for transpiled series (close, open, user vars, etc.)
 *
 * Pine uses expr[k] with k = 0 on the current bar, k >= 1 meaning k bars into the past
 * (close[1] = previous bar). Newest values live at the front of the ring; operator[](k)
 * reads k steps into the past. Codegen emits the same k for _s_close[k] and Series vars.
 *
 * push() — new bar; update() — same bar (magnifier intrabar). Negative or out-of-range
 * offsets return na (insufficient history).
 */

template<typename T>
class Series {
    DynamicRingBuffer<T> buf;

public:
    explicit Series(int max_len = 500) : buf(max_len) {}

    void push(T value) {
        buf.push_front(value);
    }

    void update(T value) {
        buf.update_front(value);
    }

    T operator[](int offset) const {
        if (offset < 0) {
            return na<T>();
        }
        return buf[static_cast<std::size_t>(offset)];
    }

    T current() const {
        return (*this)[0];
    }

    int size() const {
        return static_cast<int>(buf.size());
    }

    void clear() {
        buf.clear();
    }
    Series(int max_len, std::shared_ptr<detail::DrawingStorage> storage)
        : buf(static_cast<std::size_t>(max_len), std::move(storage)) {}

    template<class Buffer = DynamicRingBuffer<T>>
    auto storage_stats() const -> decltype(std::declval<const Buffer&>().storage_stats()) {
        return buf.storage_stats();
    }
    template<class Buffer = DynamicRingBuffer<T>>
    auto state_hash() const -> decltype(std::declval<const Buffer&>().state_hash()) {
        return buf.state_hash();
    }
};

} // namespace pineforge
