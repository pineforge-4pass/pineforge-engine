#pragma once
#include <deque>
#include <vector>
#include <algorithm>
#include "na.hpp"

namespace pineforge {

template<typename T>
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
        return buffer_[(head_ + offset) % capacity_];
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

/*
 * PineScript history indexing for transpiled series (close, open, user vars, etc.)
 *
 * Pine uses expr[k] with k = 0 on the current bar, k >= 1 meaning k bars into the past
 * (close[1] = previous bar). Newest values live at the front of the deque; operator[](k)
 * reads k steps into the past. Codegen emits the same k for _s_close[k] and Series vars.
 *
 * push() — new bar; update() — same bar (magnifier intrabar). Negative or out-of-range
 * offsets return na (insufficient history).
 */

template<typename T>
class Series {
    std::deque<T> buf;
    int max_len;

public:
    explicit Series(int max_len = 500) : max_len(max_len) {}

    void push(T value) {
        buf.push_front(value);
        if ((int)buf.size() > max_len) {
            buf.pop_back();
        }
    }

    void update(T value) {
        if (buf.empty()) {
            push(value);
        } else {
            buf.front() = value;
        }
    }

    T operator[](int offset) const {
        if (offset < 0 || offset >= (int)buf.size()) {
            return na<T>();
        }
        return buf[offset];
    }

    T current() const {
        return (*this)[0];
    }

    int size() const {
        return (int)buf.size();
    }

    void clear() {
        buf.clear();
    }
};

} // namespace pineforge
