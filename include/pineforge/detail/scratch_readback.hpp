#pragma once

#include <pineforge/detail/scratch_backing.hpp>

namespace pineforge::detail {

using ScratchHandle = std::uint64_t;
inline constexpr ScratchHandle scratch_no_handle = 0;

// Borrowed, immutable, potentially larger than a resident page; size() is
// constant while the source is alive. read copies EXACTLY destination.size
// bytes at offset, possibly across physical pages. A zero-length read at
// size() is valid. Out-of-range/null nonempty buffers return
// programming_error. On error the destination may be partially filled.
// Callers use fixed buffers; no contiguous owning copy is required anywhere.
class ScratchByteSource {
public:
    virtual ~ScratchByteSource() noexcept = default;
    virtual std::uint64_t size() const noexcept = 0;
    virtual ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept = 0;
};

// Convenience adapter borrows the span; it never owns or allocates bytes.
class ScratchSpanSource final : public ScratchByteSource {
public:
    explicit ScratchSpanSource(ScratchBytes bytes) noexcept : bytes_(bytes) {}
    std::uint64_t size() const noexcept override { return bytes_.size; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (offset > bytes_.size || destination.size > bytes_.size - offset
            || (destination.size && (!bytes_.data || !destination.data)))
            return ScratchStatus::programming_error;
        if (destination.size)
            std::memmove(destination.data, bytes_.data + static_cast<std::size_t>(offset),
                         destination.size);
        return ScratchStatus::ok;
    }
private:
    const ScratchBytes bytes_;
};

struct ScratchRecordInfo {
    ScratchHandle handle = scratch_no_handle;
    std::uint64_t key_bytes = 0;
    std::uint64_t value_bytes = 0;
};

enum class ScratchField : std::uint32_t { key, value };

// View and sources live only until this callback returns. Sources resolve
// logical bytes, not cache pointers, and remain usable across nested reads.
struct ScratchRecordView {
    ScratchRecordInfo info;
    const ScratchByteSource& key;
    const ScratchByteSource& value;
};

struct ScratchChunkView {
    ScratchRecordInfo info;
    ScratchField field;
    std::uint64_t offset;
    ScratchBytes bytes; // aliases the caller's page, only for this callback
    bool final;        // final chunk of this FIELD
};

using ScratchRecordVisitor = bool (*)(void*, const ScratchRecordView&) noexcept;
using ScratchChunkVisitor = bool (*)(void*, const ScratchChunkView&) noexcept;

// Synchronous push readback, no materialised snapshot/owning sequence. All
// methods see the current transaction's successful edits, or committed state
// when no transaction is active. Calls are serialized. A visitor may issue
// nested const reads/walks/digests on the same store using DISJOINT fixed
// buffers; outer views/cursors remain valid. Mutation (including begin,
// commit, rollback, close) from any read/comparison callback is a programming
// error. No callback, source, context, buffer or view is retained afterwards.
class ScratchReadback {
public:
    virtual ~ScratchReadback() noexcept = default;
    virtual std::size_t page_size() const noexcept = 0;
    virtual std::uint64_t record_count() const noexcept = 0;

    // Invalid/erased/rolled-back handle => not_found; info is unchanged. read
    // obeys the ScratchByteSource range rules for offset and destination
    // (out-of-range or null nonempty buffer => programming_error) and returns
    // not_found for an absent handle.
    virtual ScratchStatus point(ScratchHandle, ScratchRecordInfo& info) const noexcept = 0;
    virtual ScratchStatus read(ScratchHandle, ScratchField, std::uint64_t offset,
                               ScratchBuffer destination) const noexcept = 0;

    // page is caller-owned, nonnull, exactly page_size() bytes (else
    // programming_error with no callback). Emit all key chunks followed by all
    // value chunks, each at most page_size() bytes, with contiguous offsets
    // starting at zero within each field. Emit exactly one final empty chunk
    // for an empty field. false => cancelled, with no further callback; an
    // absent handle => not_found with no callback.
    virtual ScratchStatus visit(ScratchHandle, ScratchBuffer page,
                                ScratchChunkVisitor, void* context) const noexcept = 0;

    // Same page rule; one view per live record in ascending caller-key order.
    // Empty stream => ok with no calls. false => cancelled, including on the
    // last record. Errors never masquerade as cancellation or empty success.
    virtual ScratchStatus for_each(ScratchBuffer page, ScratchRecordVisitor,
                                   void* context) const noexcept = 0;
};

// An owning-return compatibility wrapper may exist ONLY as a non-flat test
// oracle. It must not be used by a production per-run/per-bar path, advertised
// as O(1), or substituted for point/page/visitor reads by a consumer lane.

} // namespace pineforge::detail
