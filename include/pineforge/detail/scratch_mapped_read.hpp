#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

namespace pineforge::detail {

void prepare_scratch_mapped_read();
// Only trivial automatic storage lies between sigsetjmp and the copy.
// Unrelated SIGBUS faults chain to the previously installed handler.
bool copy_scratch_mapped_bytes(void* destination, const void* source,
                               std::size_t length) noexcept;

class ScratchFile;

// Caller-owned memoization, never embedded in ScratchFile. The owner must
// unmap before writing, resizing, clearing or moving its ScratchFile, and
// before logical rewind/publication. No mapped pointer escapes read_mapped.
// Arena copies borrow its locked LRU instead; no arena pointer is retained.
class MappedIndexView {
public:
    MappedIndexView() = default;
    MappedIndexView(const MappedIndexView&) = delete;
    MappedIndexView& operator=(const MappedIndexView&) = delete;
    ~MappedIndexView() { (void)reset(); }

private:
    friend class ScratchFile;
    bool reset() noexcept {
        valid_ = false;
        owner_ = nullptr;
        if (address_ != MAP_FAILED && ::munmap(address_, length_) != 0) return false;
        address_ = MAP_FAILED;
        offset_ = length_ = backing_size_ = 0;
        return true;
    }
    void* address_ = MAP_FAILED;
    std::uint64_t offset_ = 0;
    std::size_t length_ = 0;
    std::uint64_t backing_size_ = 0;
    const ScratchFile* owner_ = nullptr;
    bool valid_ = false;
    bool page_checked_ = false;
};
static_assert(sizeof(void*) != 8 || sizeof(MappedIndexView) == 48);

} // namespace pineforge::detail
