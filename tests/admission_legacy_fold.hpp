#pragma once

#include <pineforge/detail/journal_codec.hpp>

namespace admission_test {
// Hash pass of the original scan-then-fold oracle. Its caller has already
// validated these exact bytes; keep its page reads independent of the schema
// fold so the cost, error-priority and carried-I/O comparisons can fail.
inline pineforge::detail::ScratchStatus fold_after_scan(
    const pineforge::detail::ScratchByteSource& source, pineforge::detail::ScratchBuffer page,
    std::uint64_t& digest) noexcept {
    using Status = pineforge::detail::ScratchStatus;
    if (!page.data || !page.size) return Status::programming_error;
    pineforge::detail::journal_codec::Reader in{source,page};
    const auto rows = in.word();
    if (in.status != Status::ok) return in.status;
    if (rows > in.remaining() / 24) return Status::corrupt;
    auto h = digest;
    const auto byte = [&](unsigned value) { h = (h ^ value) * 1099511628211ULL; };
    const auto word = [&](std::uint64_t value) {
        for (unsigned i = 0; i != 8; ++i) byte((value >> (8 * i)) & 255);
    };
    const auto text = [&] {
        const auto length = in.word();
        if (in.status != Status::ok) return;
        if (length > in.remaining()) { in.status = Status::corrupt; return; }
        word(length);
        std::array<std::byte, 97> bytes{};
        for (std::uint64_t at = 0; at < length && in.status == Status::ok;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),length-at));
            in.bytes(bytes.data(),count);
            for (std::size_t i = 0; i != count; ++i) byte(std::to_integer<unsigned>(bytes[i]));
            at += count;
        }
    };
    for (std::uint64_t i = 0; i != rows && in.status == Status::ok; ++i) {
        text();
        const auto kind = in.word();
        if (in.status != Status::ok) break;
        word(kind);
        if (kind == 3) text();
        else if (kind < 3) {
            auto value = in.word();
            if (kind == 2) {
                if ((value & 0x7fffffffffffffffULL) == 0) value = 0;
                else if ((value & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
                         && (value & 0x000fffffffffffffULL)) value = 0x7ff8000000000000ULL;
            }
            word(value);
        } else in.status = Status::corrupt;
    }
    const auto status = in.finish();
    if (status == Status::ok) digest = h;
    return status;
}
} // namespace admission_test
