#pragma once
#include <pineforge/native_order.hpp>

namespace pineforge::native_order {
inline namespace native_order_v8 {
// Validates without allocating decoded text, definitions or arrays.
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, std::uint64_t& ordinal) noexcept;
// The core records refused input as submitted. Only that inert request may
// contain undeclared enum numbers; executable definitions are always checked.
detail::ScratchStatus read_stored_journal_event(const detail::ScratchByteSource&,
    detail::ScratchBuffer, CommandEvent&);
}
}
