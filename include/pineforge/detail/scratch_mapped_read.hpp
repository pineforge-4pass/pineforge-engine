#pragma once

#include <cstddef>

namespace pineforge::detail {

// Install once, before exposing any mapped reads. The process handler chains
// unrelated faults to the previous disposition. Only the leaf copy below has
// a thread-local recovery point; no C++ destructor is bypassed by recovery.
void prepare_scratch_mapped_read();
bool copy_scratch_mapped_bytes(void* destination, const void* source,
                               std::size_t length) noexcept;

} // namespace pineforge::detail
