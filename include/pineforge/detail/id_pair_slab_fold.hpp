#pragma once

#include <pineforge/detail/id_pair_ordered_fold.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace pineforge::detail {

// The folded single-key slab already budgets 16*Q index bytes, Q >= 2*K.
// Store just the record offset in each hash slot (the Header owns its full
// hash), then use the remainder for sorted offsets and bucket transforms.
// No change to ScratchSlab's allocation, capacities or data-record format.
class IdPairSlabFold {
    struct Bucket {
        std::uint64_t count = 0; // Includes dead rows, which fold as identity.
        BrokerStateByteFold fold;
    };
    struct Workspace {
        Bucket bucket;
        BrokerStateByteFold next;
        std::array<unsigned char, 512> buffer{};
    };
    static_assert(sizeof(Bucket) == 2064, "slab bucket has one count and the exact seed map");
    static_assert(std::is_trivially_copyable_v<Bucket>, "slab copies complete bucket representations");

public:
    void configure(std::uint64_t data_capacity, std::uint64_t keys,
                   std::uint64_t slots) noexcept {
        width_ = data_capacity <= std::numeric_limits<std::uint32_t>::max() ? 4 : 8;
        bucket_capacity_ = width_ == 4 ? 256 : 1024;
        rows_at_ = width_ * slots;
        buckets_at_ = rows_at_ + width_ * keys;
        // Small manifests need no stored bucket map: root_ is their one map.
        cached_buckets_ = keys > bucket_capacity_;
        const auto minimum = bucket_capacity_ / 2;
        bucket_limit_ = cached_buckets_ ? keys / minimum + (keys % minimum != 0) : 0;
        reset();
    }
    void reset() noexcept {
        root_.reset();
        bucket_count_ = 0;
    }
    std::uint64_t apply(std::uint64_t seed) const noexcept { return root_.apply(seed); }
    std::uint64_t storage_bytes() const noexcept {
        return buckets_at_ + bucket_limit_ * sizeof(Bucket);
    }
    std::uint64_t offset_width() const noexcept { return width_; }
    std::uint64_t bucket_capacity() const noexcept { return bucket_capacity_; }
    std::uint64_t read_slot(const ScratchSlab& slab, ScratchResource region,
                            std::uint64_t at) const {
        return read_offset(slab, region, width_ * at);
    }
    void write_slot(ScratchSlab& slab, ScratchResource region,
                    std::uint64_t at, std::uint64_t offset_plus_one) const {
        write_offset(slab, region, width_ * at, offset_plus_one);
    }

    // count is the table's ever-key count BEFORE this mutation; append says
    // the record is genuinely new. Dead records keep their sorted position.
    // Admission of all data/key spans precedes this call. The entire index
    // region is already readable, and every write below fits its fixed span.
    template<class Reader>
    void update(ScratchSlab& slab, ScratchResource region, std::uint64_t count,
                bool append, std::string_view key, std::uint64_t row, Reader& reader) {
        Workspace work;
        std::uint64_t low = 0, high = count;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (reader.compare(read_row(slab, region, middle), key) < 0) low = middle + 1;
            else high = middle;
        }
        if (!append && (low == count || read_row(slab, region, low) != row))
            throw std::logic_error("id slab fold missing sorted row");

        // Locate a bucket using its count, without comparing keys again.
        // An insertion at a boundary belongs to the following bucket.
        std::uint64_t bucket = 0, start = 0;
        if (cached_buckets_ && bucket_count_ != 0) {
            for (;;) {
                read_bucket(slab, region, bucket, work.bucket);
                if (low < start + work.bucket.count || bucket + 1 == bucket_count_) break;
                start += work.bucket.count;
                ++bucket;
            }
        }
        if (append) {
            // Backward, bounded-buffer copies preserve all offsets on an
            // overlapping shift. Existing buckets retain exactly their rows;
            // only the receiving bucket gains a row.
            const auto per_copy = work.buffer.size() / width_;
            for (auto end = count; end > low;) {
                const auto n = std::min<std::uint64_t>(per_copy, end - low);
                const auto bytes = static_cast<std::size_t>(width_ * n);
                slab.read_at(region, rows_at_ + width_ * (end - n), work.buffer.data(), bytes);
                slab.write_at(region, rows_at_ + width_ * (end - n + 1), work.buffer.data(), bytes);
                end -= n;
            }
            write_offset(slab, region, rows_at_ + width_ * low, row);
            ++count;
            ++work.bucket.count;
        }
        if (!cached_buckets_) {
            work.bucket.count = count;
            fold_bucket(slab, region, 0, work.bucket, reader);
            root_ = work.bucket.fold;
            return;
        }
        if (bucket_count_ == 0) bucket_count_ = 1;
        if (work.bucket.count <= bucket_capacity_) {
            fold_bucket(slab, region, start, work.bucket, reader);
            write_bucket(slab, region, bucket, work.bucket);
        } else {
            const auto total = work.bucket.count; // Exactly capacity + 1.
            // Make room for the right half, preserving every unchanged map.
            for (auto at = bucket_count_; at > bucket + 1; --at) {
                read_bucket(slab, region, at - 1, work.bucket);
                write_bucket(slab, region, at, work.bucket);
            }
            ++bucket_count_;
            work.bucket.count = total / 2;
            fold_bucket(slab, region, start, work.bucket, reader);
            write_bucket(slab, region, bucket, work.bucket);
            start += work.bucket.count;
            work.bucket.count = total - work.bucket.count;
            fold_bucket(slab, region, start, work.bucket, reader);
            write_bucket(slab, region, bucket + 1, work.bucket);
        }
        // The disk fold's exact concatenation algebra, in lexical order.
        // Reuse cached maps; no unchanged bucket's bytes are folded here.
        for (std::uint64_t at = 0; at < bucket_count_; ++at) {
            read_bucket(slab, region, at, work.bucket);
            reader.compose(work.next, work.bucket.fold);
        }
        root_ = work.next;
    }
    static constexpr std::size_t workspace_bytes() noexcept { return sizeof(Workspace); }

private:
    std::uint64_t read_offset(const ScratchSlab& slab, ScratchResource region,
                              std::uint64_t at) const {
        if (width_ == 4) {
            std::uint32_t value = 0;
            slab.read_at(region, at, &value, sizeof(value));
            return value;
        }
        std::uint64_t value = 0;
        slab.read_at(region, at, &value, sizeof(value));
        return value;
    }
    void write_offset(ScratchSlab& slab, ScratchResource region, std::uint64_t at,
                      std::uint64_t value) const {
        if (width_ == 4) {
            const auto narrow = static_cast<std::uint32_t>(value);
            slab.write_at(region, at, &narrow, sizeof(narrow));
        } else slab.write_at(region, at, &value, sizeof(value));
    }
    std::uint64_t read_row(const ScratchSlab& slab, ScratchResource region,
                           std::uint64_t at) const {
        return read_offset(slab, region, rows_at_ + width_ * at);
    }
    void read_bucket(const ScratchSlab& slab, ScratchResource region,
                     std::uint64_t at, Bucket& bucket) const {
        slab.read_at(region, buckets_at_ + at * sizeof(Bucket), &bucket, sizeof(bucket));
    }
    void write_bucket(ScratchSlab& slab, ScratchResource region,
                      std::uint64_t at, const Bucket& bucket) const {
        slab.write_at(region, buckets_at_ + at * sizeof(Bucket), &bucket, sizeof(bucket));
    }
    template<class Reader>
    void fold_bucket(const ScratchSlab& slab, ScratchResource region, std::uint64_t start,
                     Bucket& bucket, Reader& reader) const {
        bucket.fold.reset();
        for (std::uint64_t at = 0; at < bucket.count; ++at)
            reader.append(read_row(slab, region, start + at), bucket.fold);
    }

    BrokerStateByteFold root_;
    std::uint64_t width_ = 4, bucket_capacity_ = 256;
    std::uint64_t rows_at_ = 0, buckets_at_ = 0;
    std::uint64_t bucket_count_ = 0, bucket_limit_ = 0;
    bool cached_buckets_ = false;
};
static_assert(IdPairSlabFold::workspace_bytes() <= IdPairOrderedFold::workspace_bytes(),
              "slab update fits the existing disk fold workspace allowance");

} // namespace pineforge::detail
