#pragma once

#include <pineforge/detail/drawing_records.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace pineforge::detail {

// A small declared lookback is eagerly prepared, up to one page of values.
// The general path addresses chronological slots in an external versioned
// tree. size/capacity still define the readable window exactly: growth never
// resurrects discarded values, and a shrink keeps the newest values.
//
// Slot addresses are monotone rather than capacity-modulo addresses so raw
// resize changes metadata, not a history-sized heap buffer (or a history-sized
// copy). A copy retains the root; calc-on-fill restore discards speculative
// writes and restores the readable-window scalars with that root.
template<class T>
class PagedHistoryRing {
    static_assert(std::is_trivially_copyable_v<T>, "owning history values need their owner's codec");
    static constexpr std::size_t inline_slots = o1::Shape{}.page_bytes / sizeof(T);
    using Records = DrawingExternalRecords<T, DrawingTrivialCodec<T>, 12>;
    std::vector<T> buffer_;
    Records records_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::uint64_t next_ = 0;
    bool external_ = false;

    std::size_t slot(std::size_t offset) const {
        return offset >= capacity_ - head_ ? offset - (capacity_ - head_) : head_ + offset;
    }
    // Only reached after the uint64 chronological address space is consumed.
    // Rebase the retained window under a fresh root with bounded workspace;
    // this is compaction, never a refusal or an offset truncation.
    void rebase() {
        Records next = records_.fresh();
        for (std::size_t i = size_; i != 0; --i)
            next.push_back(T((*this)[i - 1]));
        records_ = std::move(next);
        next_ = size_;
    }
    void promote() {
        Records next = records_.fresh();
        next.reserve_sparse_slots(size_);
        for (std::size_t i = 0; i < size_; ++i)
            next.write(i) = buffer_[slot(size_ - i - 1)];
        records_ = std::move(next);
        next_ = size_;
        std::vector<T>().swap(buffer_);
        head_ = 0;
        external_ = true;
    }
public:
    explicit PagedHistoryRing(std::size_t capacity) : PagedHistoryRing(capacity, {}) {}
    PagedHistoryRing(std::size_t capacity, std::shared_ptr<DrawingStorage> storage)
        : records_(std::move(storage)), capacity_(capacity) {
        if (capacity <= inline_slots) buffer_.resize(capacity, na<T>());
        else {
            records_.reserve_sparse_slots(0);
            external_ = true;
        }
    }
    PagedHistoryRing(const PagedHistoryRing&) = default;
    PagedHistoryRing& operator=(const PagedHistoryRing& rhs) {
        if (this != &rhs) {
            // Complete throwing preparation before publishing either logical
            // scalars or the new root; an equal-size inline restore reuses RAM.
            buffer_.reserve(rhs.buffer_.size());
            Records next(rhs.records_);
            buffer_ = rhs.buffer_;
            records_ = std::move(next);
            head_ = rhs.head_;
            size_ = rhs.size_;
            capacity_ = rhs.capacity_;
            next_ = rhs.next_;
            external_ = rhs.external_;
        }
        return *this;
    }
    PagedHistoryRing(PagedHistoryRing&& rhs) noexcept
        : buffer_(std::move(rhs.buffer_)), records_(std::move(rhs.records_)),
          head_(std::exchange(rhs.head_, 0)), size_(std::exchange(rhs.size_, 0)),
          capacity_(std::exchange(rhs.capacity_, 0)), next_(std::exchange(rhs.next_, 0)),
          external_(std::exchange(rhs.external_, false)) {}
    PagedHistoryRing& operator=(PagedHistoryRing&& rhs) noexcept {
        if (this != &rhs) {
            buffer_ = std::move(rhs.buffer_);
            records_ = std::move(rhs.records_);
            head_ = std::exchange(rhs.head_, 0);
            size_ = std::exchange(rhs.size_, 0);
            capacity_ = std::exchange(rhs.capacity_, 0);
            next_ = std::exchange(rhs.next_, 0);
            external_ = std::exchange(rhs.external_, false);
        }
        return *this;
    }
    void push_front(T value) {
        if (capacity_ == 0) return;
        if (external_) {
            if (next_ == std::numeric_limits<std::uint64_t>::max()) rebase();
            records_.write(next_) = value;
            ++next_;
            if (size_ < capacity_) ++size_;
            return;
        }
        head_ = size_ == 0 ? 0 : (head_ ? head_ - 1 : capacity_ - 1);
        buffer_[head_] = value;
        if (size_ < capacity_) ++size_;
    }
    void update_front(T value) {
        if (size_ == 0) push_front(value);
        else if (external_) records_.write(next_ - 1) = value;
        else buffer_[head_] = value;
    }
    T operator[](std::size_t offset) const {
        if (offset >= size_ || capacity_ == 0) return na<T>();
        return external_ ? records_.read(next_ - offset - 1) : buffer_[slot(offset)];
    }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    void clear() {
        if (external_) records_.reset();
        else std::fill(buffer_.begin(), buffer_.end(), na<T>());
        head_ = size_ = 0;
        next_ = 0;
    }
    void resize(std::size_t capacity) {
        if (capacity == capacity_) return;
        if (external_) {
            size_ = std::min(size_, capacity);
            capacity_ = capacity;
            return;
        }
        if (capacity > inline_slots) {
            promote();
            capacity_ = capacity;
            return;
        }
        std::vector<T> next(capacity, na<T>());
        const auto retained = std::min(size_, capacity);
        for (std::size_t i = 0; i < retained; ++i) next[i] = (*this)[i];
        buffer_.swap(next);
        head_ = 0;
        size_ = retained;
        capacity_ = capacity;
    }
    o1::Stats storage_stats() const { return records_.stats(); }

    // Canonical logical window, independent of inline/paged representation.
    // Generated state hashing must fold this alongside other script values.
    std::uint64_t state_hash() const {
        std::uint64_t hash = UINT64_C(14695981039346656037);
        const auto word = [&](std::uint64_t value) {
            for (unsigned i = 0; i < 8; ++i) {
                hash ^= value & 255u;
                hash *= UINT64_C(1099511628211);
                value >>= 8;
            }
        };
        word(capacity_);
        word(size_);
        for (std::size_t i = 0; i < size_; ++i) {
            const T value = (*this)[i];
            unsigned char bytes[sizeof(T)];
            std::memcpy(bytes, &value, sizeof value);
            for (unsigned char byte : bytes) {
                hash ^= byte;
                hash *= UINT64_C(1099511628211);
            }
        }
        return hash;
    }
};

} // namespace pineforge::detail
