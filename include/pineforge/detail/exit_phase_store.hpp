#pragma once

#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pineforge::detail {

// The adapter may reread any closed trade when a final phase is rewritten.
// Keep every logical byte, including None-filled gaps, in L19l's anonymous
// scratch file. Only one fixed read page is resident; writes are synchronous.
// Size, final cursor and digest remain the adapter's existing hash state.
class ExitPhaseStore {
public:
    ExitPhaseStore() = default;
    ExitPhaseStore(const ExitPhaseStore&) = delete;
    ExitPhaseStore& operator=(const ExitPhaseStore&) = delete;
    ExitPhaseStore(ExitPhaseStore&& other) noexcept { *this = std::move(other); }
    ExitPhaseStore& operator=(ExitPhaseStore&& other) noexcept {
        if (this != &other) {
            file_ = std::move(other.file_);
            reference_ = std::move(other.reference_);
            memory_ = std::exchange(other.memory_, false);
            size_ = std::exchange(other.size_, 0);
            extent_ = std::exchange(other.extent_, 0);
            cache_size_ = other.cache_size_ = 0;
        }
        return *this;
    }

    void set_directory(const std::string& directory) { file_.set_directory(directory); }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::uint64_t disk_bytes() const noexcept { return extent_; }
    void clear() noexcept {
        file_.clear();
        std::vector<std::uint8_t>{}.swap(reference_);
        size_ = cache_size_ = 0;
        extent_ = 0;
        // As with L19l's fixture backends, reset preserves the chosen backend
        // and directory. No strategy or process-wide switch selects memory.
    }
    void fixture_use_memory_reference() {
        if (size_ != 0 || extent_ != 0)
            throw std::logic_error("exit phase fixture backend changed after use");
        memory_ = true;
    }

    void resize(std::size_t count, std::uint8_t fill) {
        if (memory_) {
            reference_.resize(count, fill);
            size_ = count;
            return;
        }
        if (count > size_) {
            ScratchFile::check_extent(0, count);
            file_.open("pineforge-exit-phases");
            std::array<std::uint8_t, kPageBytes> bytes{};
            bytes.fill(fill);
            for (std::size_t at = size_; at < count;) {
                const auto length = std::min(bytes.size(), count - at);
                file_.write(at, bytes.data(), length);
                at += length;
            }
            extent_ = std::max(extent_, static_cast<std::uint64_t>(count));
        }
        // Publish size only after the complete extension. A failed write is
        // a typed run failure, never an absent/default historical phase.
        size_ = count;
        cache_size_ = 0;
    }

    std::uint8_t operator[](std::size_t index) const {
        check_index(index);
        if (memory_) return reference_[index];
        if (cache_size_ == 0 || index < cache_begin_
            || index - cache_begin_ >= cache_size_) {
            cache_size_ = 0; // A failed read must not publish a partial page.
            const auto begin = index - index % kPageBytes;
            const auto length = std::min(kPageBytes, size_ - begin);
            file_.read(begin, cache_.data(), length);
            cache_begin_ = begin;
            cache_size_ = length;
        }
        return cache_[index - cache_begin_];
    }
    void set(std::size_t index, std::uint8_t phase) {
        check_index(index);
        if (memory_) {
            reference_[index] = phase;
            return;
        }
        cache_size_ = 0;
        file_.write(index, &phase, sizeof phase);
    }

private:
    void check_index(std::size_t index) const {
        if (index >= size_) throw std::out_of_range("exit phase index");
    }

    static constexpr std::size_t kPageBytes = 4096;
    ScratchFile file_;
    // Empty in production; this is the old vector implementation, available
    // only through the explicit per-instance native fixture seam.
    std::vector<std::uint8_t> reference_;
    bool memory_ = false;
    std::size_t size_ = 0;
    std::uint64_t extent_ = 0;
    mutable std::array<std::uint8_t, kPageBytes> cache_{};
    mutable std::size_t cache_begin_ = 0, cache_size_ = 0;
};

} // namespace pineforge::detail
