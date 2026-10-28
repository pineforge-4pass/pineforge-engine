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
// The general path keeps the window in an external versioned tree whose slot
// address is the running push count modulo a power of two: the smallest one
// not below the largest capacity the ring has had. A push overwrites the slot
// of the value that just left the window, so the tree holds at most that many
// slots (their blocks and one path of nodes), whatever the number of pushes.
// size/capacity still define the readable window exactly: growth never
// resurrects discarded values, and a shrink keeps the newest values.
//
// A shrink, and a growth the modulus still holds, change metadata only, so a
// length that moves every bar never copies the window. A growth past the
// modulus copies the retained window once under a fresh root with the larger
// modulus. A copy retains the root; calc-on-fill restore discards speculative
// writes and restores the readable-window scalars with that root. A slot whose
// block a copy shares is overwritten in a private copy of the block and its
// path, so no copy sees a later value and nothing it reads is released.
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
    std::uint64_t mask_ = 0; // the external slot modulus minus one
    bool external_ = false;

    std::size_t slot(std::size_t offset) const {
        return offset >= capacity_ - head_ ? offset - (capacity_ - head_) : head_ + offset;
    }
    // The smallest power of two not below the capacity, minus one. Above 2^63
    // the modulus is 2^64 itself. Every modulus divides 2^64, so the push
    // count may wrap without moving any slot.
    static std::uint64_t mask_for(std::size_t capacity) noexcept {
        constexpr std::uint64_t top = UINT64_C(1) << 63;
        if (capacity > top) return std::numeric_limits<std::uint64_t>::max();
        std::uint64_t modulus = 1;
        while (modulus < capacity) modulus <<= 1;
        return modulus - 1;
    }
    // Copies the newest `retained` values, oldest first, into slots
    // [0, retained) of a fresh tree with the modulus mask + 1. The replaced
    // root is released before the copy is installed, and the scalars change
    // only after both, so a throw leaves the ring as it was.
    void relayout(std::uint64_t mask, std::size_t retained, std::size_t capacity) {
        Records next = records_.fresh();
        for (std::size_t i = retained; i != 0; --i)
            next.push_back(T((*this)[i - 1]));
        records_ = std::move(next);
        size_ = retained;
        capacity_ = capacity;
        next_ = retained;
        mask_ = mask;
    }
    void promote(std::size_t capacity) {
        Records next = records_.fresh();
        next.reserve_sparse_slots(size_);
        for (std::size_t i = 0; i < size_; ++i)
            next.write(i) = buffer_[slot(size_ - i - 1)];
        records_ = std::move(next);
        std::vector<T>().swap(buffer_);
        next_ = size_;
        mask_ = mask_for(capacity);
        head_ = 0;
        capacity_ = capacity;
        external_ = true;
    }
public:
    explicit PagedHistoryRing(std::size_t capacity) : PagedHistoryRing(capacity, {}) {}
    PagedHistoryRing(std::size_t capacity, std::shared_ptr<DrawingStorage> storage)
        : records_(std::move(storage)), capacity_(capacity) {
        if (capacity <= inline_slots) buffer_.resize(capacity, na<T>());
        else {
            records_.reserve_sparse_slots(0);
            mask_ = mask_for(capacity);
            external_ = true;
        }
    }
    PagedHistoryRing(const PagedHistoryRing&) = default;
    PagedHistoryRing& operator=(const PagedHistoryRing& rhs) {
        if (this != &rhs) {
            // Complete throwing preparation before publishing either logical
            // scalars or the new root; an equal-size inline restore reuses RAM.
            // The records' assignment (the new root's retain and the replaced
            // root's release) is all or nothing, so a Store error propagates
            // with the ring as it was.
            buffer_.reserve(rhs.buffer_.size());
            records_ = rhs.records_;
            buffer_ = rhs.buffer_;
            head_ = rhs.head_;
            size_ = rhs.size_;
            capacity_ = rhs.capacity_;
            next_ = rhs.next_;
            mask_ = rhs.mask_;
            external_ = rhs.external_;
        }
        return *this;
    }
    PagedHistoryRing(PagedHistoryRing&& rhs) noexcept
        : buffer_(std::move(rhs.buffer_)), records_(std::move(rhs.records_)),
          head_(std::exchange(rhs.head_, 0)), size_(std::exchange(rhs.size_, 0)),
          capacity_(std::exchange(rhs.capacity_, 0)), next_(std::exchange(rhs.next_, 0)),
          mask_(std::exchange(rhs.mask_, 0)), external_(std::exchange(rhs.external_, false)) {}
    // Releasing the replaced root can meet a Store error. It runs first and
    // propagates with both rings as they were.
    PagedHistoryRing& operator=(PagedHistoryRing&& rhs) {
        if (this != &rhs) {
            records_ = std::move(rhs.records_);
            buffer_ = std::move(rhs.buffer_);
            head_ = std::exchange(rhs.head_, 0);
            size_ = std::exchange(rhs.size_, 0);
            capacity_ = std::exchange(rhs.capacity_, 0);
            next_ = std::exchange(rhs.next_, 0);
            mask_ = std::exchange(rhs.mask_, 0);
            external_ = std::exchange(rhs.external_, false);
        }
        return *this;
    }
    void push_front(T value) {
        if (capacity_ == 0) return;
        if (external_) {
            records_.write(next_ & mask_) = value;
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
        else if (external_) records_.write((next_ - 1) & mask_) = value;
        else buffer_[head_] = value;
    }
    T operator[](std::size_t offset) const {
        if (offset >= size_ || capacity_ == 0) return na<T>();
        return external_ ? records_.read((next_ - offset - 1) & mask_) : buffer_[slot(offset)];
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
            if (capacity == 0 || capacity - 1 <= mask_) {
                size_ = std::min(size_, capacity);
                capacity_ = capacity;
            } else {
                relayout(mask_for(capacity), size_, capacity);
            }
            return;
        }
        if (capacity > inline_slots) {
            promote(capacity);
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
