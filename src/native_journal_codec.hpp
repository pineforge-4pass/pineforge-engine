#pragma once
#include <pineforge/native_order.hpp>

namespace pineforge::native_order {
inline namespace native_order_v8 {
// Validates without allocating decoded text, definitions or arrays.
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, std::uint64_t& ordinal) noexcept;
}
}
