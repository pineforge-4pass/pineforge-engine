#pragma once

#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/feed_reader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// Inclusive optional payload + page-tag budget, per projection reader.
// Transfer buffers and the fixed owner object are reported separately.
inline constexpr std::size_t kFeedProjectionCacheBytes = 256 * 1024;

struct FeedProjectionStats {
    std::uint64_t read_calls = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t bypass_reads = 0;
    std::uint64_t allocation_bypasses = 0;
    std::uint64_t backing_reads = 0;
    std::uint64_t backing_bytes = 0;
    std::uint64_t backing_errors = 0;
    std::size_t resident_bytes = 0;
    std::size_t peak_resident_bytes = 0;
};

template<class Row> class FeedProjectionBuilder;

// Read-only whole-record adapter over the shared cache. Copies share only
// the immutable provider, never cache addresses. The cache control is inline;
// its only optional allocation is the shared cache's payload and metadata.
template<class Row>
class FeedProjection final : public FeedReader<Row> {
    static_assert(std::is_trivially_copyable_v<Row>);
    static_assert(std::is_nothrow_default_constructible_v<Row>);
    static_assert(sizeof(Row) <= 4096);
    static constexpr std::size_t kRows = 4096 / sizeof(Row);
public:
    FeedProjection() = default;
    explicit FeedProjection(std::shared_ptr<const FeedReader<Row>> reader,
                            std::size_t capacity_bytes = kFeedProjectionCacheBytes)
        : reader_(std::move(reader)) {
        set_capacity_bytes(capacity_bytes);
    }
    FeedProjection(std::initializer_list<Row> rows);
    FeedProjection(const FeedProjection& other)
        : FeedProjection(other.reader_, other.capacity_bytes_) {}
    FeedProjection& operator=(const FeedProjection& other) {
        if (this != &other) {
            FeedProjection copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    FeedProjection(FeedProjection&& other) noexcept
        : reader_(std::move(other.reader_)),
          capacity_bytes_(other.capacity_bytes_), stats_(other.stats_) {
        // The shared cache is nonmovable and borrows this adapter. Rebuild a
        // cold control object; immutable provider identity and values survive.
        stats_.resident_bytes = 0;
        initialize_cache();
        other.cache_.reset();
        other.read_failure_ = nullptr;
        other.stats_.resident_bytes = 0;
    }
    FeedProjection& operator=(FeedProjection&& other) noexcept {
        if (this != &other) {
            cache_.reset();
            read_failure_ = nullptr;
            reader_ = std::move(other.reader_);
            capacity_bytes_ = other.capacity_bytes_;
            stats_ = other.stats_;
            stats_.resident_bytes = 0;
            initialize_cache();
            other.cache_.reset();
            other.read_failure_ = nullptr;
            other.stats_.resident_bytes = 0;
        }
        return *this;
    }

    static constexpr std::size_t page_payload_bytes() noexcept { return kRows * sizeof(Row); }
    static std::size_t page_bytes() noexcept {
        return ReadThroughCache::line_bytes(page_payload_bytes());
    }
    static constexpr std::size_t transfer_workspace_bytes() noexcept {
        return page_payload_bytes();
    }
    static constexpr std::size_t rows_per_page() noexcept { return kRows; }
    std::size_t size() const noexcept override { return reader_ ? reader_->size() : 0; }
    bool empty() const noexcept { return size() == 0; }
    Row operator[](std::size_t index) const { return this->at(index); }
    Row back() const {
        if (empty()) throw std::out_of_range("feed projection back");
        return this->at(size() - 1);
    }
    const FeedProjectionStats& stats() const noexcept { return stats_; }
    // Shared-cache counters cover the current capacity epoch. The projection
    // counters above retain completed epochs across resizing and moves.
    ReadThroughCacheStats cache_stats() const noexcept {
        return cache_ ? cache_->stats() : ReadThroughCacheStats{};
    }
    ReadThroughCachePolicy cache_policy() const noexcept {
        return cache_ ? cache_->policy() : ReadThroughCachePolicy{0, page_payload_bytes()};
    }
    std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }
    const std::shared_ptr<const FeedReader<Row>>& reader() const noexcept { return reader_; }

    // Zero/sub-page budgets and optional allocation failures all read through.
    // No logical row limit follows from the number of allocated cache slots.
    void set_capacity_bytes(std::size_t bytes) noexcept {
        cache_.reset();
        read_failure_ = nullptr;
        stats_.resident_bytes = 0;
        capacity_bytes_ = bytes;
        initialize_cache();
    }

    void read(std::size_t first, Row* destination,
              std::size_t count) const override {
        if (first > size() || count > size() - first)
            throw std::out_of_range("feed projection range");
        if (count && !destination)
            throw std::invalid_argument("feed projection destination");
        if (count == 0) return;
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(Row))
            throw AuxiliaryScratchIoError(EFBIG, "feed projection transfer");
        increment(stats_.read_calls);
        // Typed, aligned objects belong to this call. No pointer into a cache
        // payload escapes, including on a hit, bypass or failed page fill.
        std::array<Row, kRows> page;
        while (count != 0) {
            const auto page_first = first - first % kRows;
            const auto rows = std::min(kRows, size() - page_first);
            const auto before = cache_->stats();
            read_failure_ = nullptr;
            // One immutable provider per cache; the identity/generation
            // namespace is local to this control object and never reused.
            const auto status = cache_->get(
                {0, 0, static_cast<std::uint64_t>(first / kRows)},
                {reinterpret_cast<std::byte*>(page.data()), rows * sizeof(Row)});
            record_cache_delta(before);
            if (status != ScratchStatus::ok) {
                if (read_failure_)
                    std::rethrow_exception(std::exchange(read_failure_, {}));
                throw std::logic_error("feed projection cache contract");
            }
            const auto copied = std::min(count, kRows - (first - page_first));
            std::memcpy(destination, page.data() + (first - page_first),
                        copied * sizeof(Row));
            destination += copied;
            first += copied;
            count -= copied;
        }
    }
private:
    static void increment(std::uint64_t& counter, std::uint64_t amount = 1) noexcept {
        counter += std::min(amount, std::numeric_limits<std::uint64_t>::max() - counter);
    }
    void initialize_cache() noexcept {
        if (!reader_ || size() == 0) return;
        const auto useful = size() / kRows + (size() % kRows != 0);
        const auto slots = std::min(capacity_bytes_ / page_bytes(), useful);
        // Multiplication is bounded by capacity_bytes_, including all tags.
        cache_.emplace(ReadThroughCachePolicy{
            slots * page_bytes(), page_payload_bytes(),
            ReadThroughCacheReplacement::direct_mapped,
            ReadThroughCacheWritePolicy::write_through},
            ReadThroughCacheBacking{this, read_page, nullptr});
    }
    static ReadThroughCacheIoResult read_page(
            void* context, ReadThroughCacheKey key, ScratchBuffer destination) noexcept {
        auto& self = *static_cast<FeedProjection*>(context);
        if (key.backing_identity || key.generation
            || key.page_number > std::numeric_limits<std::size_t>::max() / kRows
            || destination.size % sizeof(Row) != 0)
            return {ScratchStatus::programming_error, 0};
        try {
            // get() supplies the aligned, already-live Row array above.
            // The provider remains the authority for extent and real I/O.
            self.read_backing(static_cast<std::size_t>(key.page_number) * kRows,
                              reinterpret_cast<Row*>(destination.data),
                              destination.size / sizeof(Row));
            return {ScratchStatus::ok, destination.size};
        } catch (...) {
            // FeedReader's exception vocabulary includes auxiliary scratch,
            // arena/slab failures and provider exceptions beyond the cache's
            // carried report-error type. Rethrow the exact exception outside
            // the noexcept port. A failed fill never publishes a cache tag.
            self.read_failure_ = std::current_exception();
            return {ScratchStatus::io_error, 0};
        }
    }
    void record_cache_delta(const ReadThroughCacheStats& before) const noexcept {
        const auto after = cache_->stats();
        increment(stats_.hits, after.hits - before.hits);
        increment(stats_.misses, after.misses - before.misses);
        increment(stats_.evictions, after.evictions - before.evictions);
        increment(stats_.bypass_reads, after.bypass_reads - before.bypass_reads);
        increment(stats_.allocation_bypasses,
                  after.allocation_bypass_count - before.allocation_bypass_count);
        stats_.resident_bytes = static_cast<std::size_t>(after.current_bytes);
        stats_.peak_resident_bytes = std::max(
            stats_.peak_resident_bytes, static_cast<std::size_t>(after.peak_bytes));
    }
    void read_backing(std::size_t first, Row* destination, std::size_t count) const {
        increment(stats_.backing_reads);
        try {
            reader_->read(first, destination, count);
        } catch (...) {
            increment(stats_.backing_errors);
            throw;
        }
        increment(stats_.backing_bytes, static_cast<std::uint64_t>(count) * sizeof(Row));
    }
    std::shared_ptr<const FeedReader<Row>> reader_;
    mutable std::optional<ReadThroughCache> cache_;
    mutable std::exception_ptr read_failure_;
    std::size_t capacity_bytes_ = kFeedProjectionCacheBytes;
    mutable FeedProjectionStats stats_;
};

// Immutable positional provider. Physical counters are at the ScratchFile
// transfer, below the optional cache, so tests can prove a cold read occurred.
template<class Row>
class ScratchFeedReader final : public FeedReader<Row> {
    static_assert(std::is_trivially_copyable_v<Row>);
    friend class FeedProjectionBuilder<Row>;
public:
    explicit ScratchFeedReader(const std::string& directory = {})
        : file_(ScratchFile::IoRaiser::auxiliary) {
        file_.bound_cache();
        file_.set_directory(directory);
    }
    std::size_t size() const noexcept override { return count_; }
    void read(std::size_t first, Row* destination,
              std::size_t count) const override {
        if (first > count_ || count > count_ - first)
            throw std::out_of_range("scratch feed range");
        if (count && !destination)
            throw std::invalid_argument("scratch feed destination");
        if (count == 0) return;
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(Row))
            throw AuxiliaryScratchIoError(EFBIG, "feed projection transfer");
        ++read_calls_;
        file_.read(byte_offset(first), destination, count * sizeof(Row));
        read_bytes_ += static_cast<std::uint64_t>(count) * sizeof(Row);
    }
    std::uint64_t read_calls() const noexcept { return read_calls_; }
    std::uint64_t read_bytes() const noexcept { return read_bytes_; }
private:
    static std::uint64_t byte_offset(std::size_t index) {
        if (index > std::numeric_limits<std::uint64_t>::max() / sizeof(Row))
            throw AuxiliaryScratchIoError(EFBIG, "feed projection offset");
        return static_cast<std::uint64_t>(index) * sizeof(Row);
    }
    ScratchFile file_;
    std::size_t count_ = 0;
    mutable std::uint64_t read_calls_ = 0;
    mutable std::uint64_t read_bytes_ = 0;
};

// Immutable provider for a production projection that fits in its one
// transfer page. The owner is bounded by Row and never grows with input.
template<class Row>
class ResidentFeedReader final : public FeedReader<Row> {
    static_assert(std::is_trivially_copyable_v<Row>);
    static_assert(std::is_nothrow_default_constructible_v<Row>);
    static_assert(sizeof(Row) <= 4096);
    static constexpr std::size_t kRows = 4096 / sizeof(Row);
public:
    ResidentFeedReader(const Row* rows, std::size_t count) : count_(count) {
        if (count > kRows)
            throw std::invalid_argument("resident feed projection extent");
        if (count != 0)
            std::memcpy(rows_.data(), rows, count * sizeof(Row));
    }
    std::size_t size() const noexcept override { return count_; }
    void read(std::size_t first, Row* destination,
              std::size_t count) const override {
        if (first > count_ || count > count_ - first)
            throw std::out_of_range("resident feed range");
        if (count && !destination)
            throw std::invalid_argument("resident feed destination");
        if (count == 0) return;
        std::memcpy(destination, rows_.data() + first, count * sizeof(Row));
    }
private:
    std::array<Row, kRows> rows_{};
    std::size_t count_ = 0;
};

// Sequential production uses one mandatory transfer page, independent of
// input length and optional read capacity. Resident mode publishes that page
// in memory until a later append requires scratch; finish() flushes any
// pending scratch page, and destruction never flushes or commits.
template<class Row>
class FeedProjectionBuilder {
    static_assert(std::is_trivially_copyable_v<Row>);
    static_assert(sizeof(Row) <= 4096);
    static constexpr std::size_t kRows = 4096 / sizeof(Row);
public:
    explicit FeedProjectionBuilder(const std::string& directory = {},
                                   bool resident_if_single_page = false)
        : directory_(directory),
          resident_if_single_page_(resident_if_single_page) {
        if (!resident_if_single_page_)
            reader_ = std::make_shared<ScratchFeedReader<Row>>(directory_);
    }
    FeedProjectionBuilder(const FeedProjectionBuilder&) = delete;
    FeedProjectionBuilder& operator=(const FeedProjectionBuilder&) = delete;

    void append(const Row& row) {
        if (published_) throw std::logic_error("feed projection already published");
        if (resident_if_single_page_ && !reader_ && buffered_ == kRows)
            spill_to_scratch();
        if (buffered_ == kRows) flush();
        if (reader_) {
            if (reader_->count_ > std::numeric_limits<std::size_t>::max()
                                      - buffered_ - 1)
                throw AuxiliaryScratchIoError(EFBIG, "feed projection extent");
            const auto offset =
                ScratchFeedReader<Row>::byte_offset(reader_->count_ + buffered_);
            ScratchFile::check_extent(offset, sizeof(Row), false,
                                      ScratchFile::IoRaiser::auxiliary);
        }
        std::memcpy(&buffer_[buffered_++], &row, sizeof(Row));
    }
    FeedProjection<Row> finish(std::size_t capacity_bytes = kFeedProjectionCacheBytes) {
        if (published_) throw std::logic_error("feed projection already published");
        std::shared_ptr<const FeedReader<Row>> provider;
        if (reader_) {
            flush();
            provider = reader_;
        } else {
            provider = std::make_shared<ResidentFeedReader<Row>>(buffer_.data(), buffered_);
        }
        FeedProjection<Row> result(std::move(provider), capacity_bytes);
        published_ = true;
        reader_.reset();
        return result;
    }
private:
    void spill_to_scratch() {
        reader_ = std::make_shared<ScratchFeedReader<Row>>(directory_);
        flush();
    }

    void flush() {
        if (buffered_ == 0) return;
        reader_->file_.open("pineforge-feed");
        reader_->file_.write(ScratchFeedReader<Row>::byte_offset(reader_->count_),
                             buffer_.data(), buffered_ * sizeof(Row));
        // Retain the entire transfer page if a write fails.
        reader_->count_ += buffered_;
        buffered_ = 0;
    }
    std::string directory_;
    bool resident_if_single_page_ = false;
    bool published_ = false;
    std::shared_ptr<ScratchFeedReader<Row>> reader_;
    std::array<Row, kRows> buffer_;
    std::size_t buffered_ = 0;
};

template<class Row>
FeedProjection<Row>::FeedProjection(std::initializer_list<Row> rows) {
    FeedProjectionBuilder<Row> builder;
    for (const auto& row : rows) builder.append(row);
    *this = builder.finish();
}

} // namespace pineforge::detail
