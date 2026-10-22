#pragma once

#include <pineforge/detail/scratch_store.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

namespace pineforge::detail {

// Payload schema v1: an ordered stream of named, typed leaves. Object begin/end
// delimit nested values; sequence counts and variant tags are unsigned leaves.
// Scalars use LE64 (double preserves all bits, including NaN and signed zero).
// Text is bytes, including embedded NUL. No C++ object representation is stored.
enum class PayloadType : std::uint64_t {
    unsigned_integer, signed_integer, real, boolean, text, object_begin, object_end
};
enum class PayloadSchema : std::uint64_t { request = 1, placement = 2, pending = 3 };
inline constexpr std::uint64_t payload_root_kind = 0x4f31533200000001ULL;
inline constexpr std::uint64_t payload_field_kind = 0x4f31533200000002ULL;

inline void payload_encode(std::uint64_t value, std::byte* bytes) noexcept {
    for (unsigned i = 0; i != 8; ++i) bytes[i] = std::byte(value >> (8 * i));
}
inline std::uint64_t payload_decode(const std::byte* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[i])) << (8 * i);
    return value;
}

/// A logical slice, stable across cache eviction and nested reads. The store
/// and its version must outlive this value. Scope erasure invalidates it.
class PayloadBytes final : public ScratchByteSource {
public:
    PayloadBytes() noexcept = default;
    PayloadBytes(const ScratchReadback& store, ScratchHandle handle,
                 std::uint64_t offset, std::uint64_t size) noexcept
        : store_(&store), handle_(handle), offset_(offset), size_(size) {}
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (!store_ || offset > size_ || destination.size > size_ - offset
            || (destination.size && !destination.data))
            return ScratchStatus::programming_error;
        return store_->read(handle_, ScratchField::value, offset_ + offset, destination);
    }
private:
    const ScratchReadback* store_ = nullptr;
    ScratchHandle handle_ = 0;
    std::uint64_t offset_ = 0;
    std::uint64_t size_ = 0;
};

struct PayloadField {
    PayloadType type = PayloadType::object_begin;
    PayloadBytes name;
    PayloadBytes bytes;
    ScratchStatus unsigned_value(std::uint64_t& out) const noexcept;
    ScratchStatus signed_value(std::int64_t& out) const noexcept;
    ScratchStatus real_value(double& out) const noexcept;
    ScratchStatus boolean_value(bool& out) const noexcept;
};

/// All reads borrow store/version; no owning row or text reconstruction.
class PayloadReadback {
public:
    PayloadReadback() noexcept = default;
    PayloadReadback(const ScratchStore& store, ScratchHandle version,
                    PayloadSchema schema) noexcept
        : store_(&store), version_(version), schema_(schema) {}
    ScratchStatus field_count(std::uint64_t& out) const noexcept;
    ScratchStatus field(std::uint64_t index, PayloadField& out) const noexcept;
    // Top-level named fields; nested values are traversed by field ordinal.
    ScratchStatus find(std::string_view name, PayloadField& out) const noexcept;
    ScratchStatus subobject(std::string_view name, PayloadReadback& out) const noexcept;
    ScratchStatus visit(void*, bool (*)(void*, const PayloadField&)) const;
    ScratchHandle version() const noexcept { return version_; }
private:
    ScratchStatus root(std::array<std::uint64_t, 3>&) const noexcept;
    const ScratchStore* store_ = nullptr;
    ScratchHandle version_ = 0;
    PayloadSchema schema_ = PayloadSchema::request;
    std::uint64_t first_ = 0;
    std::uint64_t limit_ = std::numeric_limits<std::uint64_t>::max();
};

/// Paged text emitted from a same-store source. Fixed chunks permit the writer
/// to finish each input read before put; this reader joins them without a copy.
class PayloadChunkBytes final : public ScratchByteSource {
public:
    PayloadChunkBytes() noexcept = default;
    PayloadChunkBytes(PayloadReadback reader, std::uint64_t first, std::uint64_t size) noexcept
        : reader_(reader), first_(first), size_(size) {}
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
        if (offset > size_ || out.size > size_ - offset || (out.size && !out.data))
            return ScratchStatus::programming_error;
        while (out.size) {
            PayloadField field;
            auto status = reader_.field(first_ + offset / 128, field);
            if (status != ScratchStatus::ok) return status;
            const auto base = offset / 128 * 128;
            const auto expected = std::min<std::uint64_t>(128, size_ - base);
            if (field.type != PayloadType::text || field.bytes.size() != expected)
                return ScratchStatus::corrupt;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(
                out.size, expected - offset % 128));
            status = field.bytes.read(offset % 128, {out.data, n});
            if (status != ScratchStatus::ok) return status;
            offset += n;
            out.data += n;
            out.size -= n;
        }
        return ScratchStatus::ok;
    }
private:
    PayloadReadback reader_;
    std::uint64_t first_ = 0;
    std::uint64_t size_ = 0;
};

} // namespace pineforge::detail
