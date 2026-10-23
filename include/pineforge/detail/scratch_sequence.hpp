#pragma once

#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace pineforge::detail {

// A numeric sequence over ScratchFile, with one fixed 64 KiB write-back
// window. Prefix retirement changes an offset, never moves rows or builds
// an in-memory directory. These are exact bytes, not a hash summary.
template<class T>
class ScratchSequence {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) <= 64 * 1024);
    static constexpr std::size_t kValues = 64 * 1024 / sizeof(T);
public:
    ScratchSequence() { file_.bound_cache(); }
    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = T;
        using pointer = void;
        const_iterator() = default;
        T operator*() const { return owner_->at(index_); }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        const_iterator operator+(std::size_t n) const { return {owner_, index_ + n}; }
        bool operator==(const const_iterator& o) const {
            return owner_ == o.owner_ && index_ == o.index_;
        }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }
    private:
        friend class ScratchSequence;
        const_iterator(const ScratchSequence* owner, std::size_t index)
            : owner_(owner), index_(index) {}
        const ScratchSequence* owner_ = nullptr;
        std::size_t index_ = 0;
    };

    void set_directory(const std::string& directory) {
        file_.set_directory(directory);
        directory_ = directory;
    }
    std::size_t size() const noexcept { return end_ - base_; }
    bool empty() const noexcept { return size() == 0; }
    static constexpr std::size_t resident_capacity() noexcept { return kValues; }
    void clear() noexcept {
        file_.clear();
        base_ = end_ = page_ = loaded_ = 0;
        dirty_ = false;
    }
    T at(std::size_t index) const {
        if (index >= size()) throw std::out_of_range("scratch sequence index");
        load(base_ + index);
        return window_[(base_ + index) % kValues];
    }
    T operator[](std::size_t index) const { return at(index); }
    void set(std::size_t index, T value) {
        if (index >= size()) throw std::out_of_range("scratch sequence index");
        load(base_ + index);
        window_[(base_ + index) % kValues] = value;
        dirty_ = true;
    }
    void push_back(T value) {
        if (end_ == std::numeric_limits<std::size_t>::max())
            throw ScratchIoError(EFBIG, "scratch sequence extent");
        ScratchFile::check_extent(byte_offset(end_), sizeof(T));
        load(end_);
        window_[end_ % kValues] = value;
        loaded_ = std::max(loaded_, end_ % kValues + 1);
        dirty_ = true;
        ++end_;
    }
    void resize(std::size_t count, T value = T{}) {
        while (size() < count) push_back(value);
        if (count == size()) return;
        flush();
        end_ = base_ + count;
        loaded_ = 0;
    }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }
    void erase(const_iterator first, const_iterator last) {
        if (first.owner_ != this || last.owner_ != this
            || first.index_ != 0 || last.index_ > size())
            throw std::logic_error("scratch sequence erase is not a prefix");
        base_ += last.index_;
    }
private:
    static std::uint64_t byte_offset(std::size_t index) {
        if (index > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
            throw ScratchIoError(EFBIG, "scratch sequence offset");
        return static_cast<std::uint64_t>(index) * sizeof(T);
    }
    void flush() const {
        if (!dirty_) return;
        file_.open("pineforge-sequence");
        file_.write(byte_offset(page_), window_.data(), loaded_ * sizeof(T));
        dirty_ = false;
    }
    void load(std::size_t global) const {
        const std::size_t next = global - global % kValues;
        if (loaded_ != 0 && page_ == next) return;
        flush();
        const auto count = next < end_ ? std::min(kValues, end_ - next) : 0;
        loaded_ = 0;
        if (count != 0)
            file_.read(byte_offset(next), window_.data(), count * sizeof(T));
        page_ = next;
        loaded_ = count;
    }
    mutable ScratchFile file_;
    std::string directory_;
    mutable std::array<T, kValues> window_{};
    std::size_t base_ = 0, end_ = 0;
    mutable std::size_t page_ = 0, loaded_ = 0;
    mutable bool dirty_ = false;
};
} // namespace pineforge::detail
