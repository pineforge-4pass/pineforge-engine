#include "native_cold_order_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pineforge::detail::cold_order {
namespace {
constexpr std::size_t kHeader = 10;
constexpr unsigned char kVersion = 1;
constexpr unsigned char kRaw = 0;
constexpr unsigned char kRle = 1;

[[noreturn]] void malformed() { throw FormatError{}; }

std::size_t span_length(const unsigned char* bytes, std::size_t left) noexcept {
    const bool zero = bytes[0] == 0;
    const std::size_t limit = std::min(left, std::size_t{128});
    std::size_t length = 1;
    while (length < limit && (bytes[length] == 0) == zero) ++length;
    return length;
}
}  // namespace

std::vector<unsigned char> encode(const unsigned char* bytes, std::size_t length) {
    std::vector<unsigned char> frame;
    if ((!bytes && length != 0)
        || length > std::numeric_limits<std::uint64_t>::max()
        || length > frame.max_size() - kHeader) malformed();

    // Count before allocating. Saturating at RAW size avoids overflow and
    // avoids constructing a larger RLE candidate for incompressible input.
    std::size_t packed = 0;
    for (std::size_t at = 0; at < length;) {
        const std::size_t count = span_length(bytes + at, length - at);
        const std::size_t cost = bytes[at] == 0 ? 1 : count + 1;
        if (cost >= length - packed) {
            packed = length;
            break;
        }
        packed += cost;
        at += count;
    }
    const bool rle = packed < length;
    frame.resize(kHeader + (rle ? packed : length));
    frame[0] = kVersion;
    frame[1] = rle ? kRle : kRaw;
    const auto decoded = static_cast<std::uint64_t>(length);
    for (unsigned i = 0; i != 8; ++i)
        frame[2 + i] = static_cast<unsigned char>((decoded >> (8 * i)) & 0xffu);
    if (!rle) {
        if (length != 0) std::memcpy(frame.data() + kHeader, bytes, length);
        return frame;
    }
    std::size_t out = kHeader;
    for (std::size_t at = 0; at < length;) {
        const std::size_t count = span_length(bytes + at, length - at);
        const bool zero = bytes[at] == 0;
        frame[out++] = static_cast<unsigned char>((zero ? 0x80u : 0u) | (count - 1));
        if (!zero) {
            std::memcpy(frame.data() + out, bytes + at, count);
            out += count;
        }
        at += count;
    }
    return frame;
}

std::vector<unsigned char> decode(const unsigned char* frame, std::size_t length) {
    std::vector<unsigned char> bytes;
    if (!frame || length < kHeader || length > bytes.max_size()) malformed();
    if (frame[0] != kVersion || (frame[1] != kRaw && frame[1] != kRle)) malformed();
    std::uint64_t word = 0;
    for (unsigned i = 0; i != 8; ++i)
        word |= std::uint64_t{frame[2 + i]} << (8 * i);
    if (word > std::numeric_limits<std::size_t>::max() || word > bytes.max_size())
        malformed();
    const auto decoded = static_cast<std::size_t>(word);
    if (frame[1] == kRaw) {
        if (decoded != length - kHeader) malformed();
        bytes.resize(decoded);
        if (decoded != 0) std::memcpy(bytes.data(), frame + kHeader, decoded);
        return bytes;
    }

    // Preflight the complete payload before trusting the declared length for
    // allocation. Subtraction bounds every sum by both actual token expansion
    // and the representable decoded length, including on 32-bit hosts.
    std::size_t produced = 0;
    for (std::size_t at = kHeader; at < length;) {
        const unsigned char token = frame[at++];
        const std::size_t count = (token & 0x7fu) + 1u;
        if (count > decoded - produced) malformed();
        if ((token & 0x80u) == 0) {
            if (count > length - at) malformed();
            at += count;
        }
        produced += count;
    }
    if (produced != decoded) malformed();
    bytes.resize(decoded);
    std::size_t out = 0;
    for (std::size_t at = kHeader; at < length;) {
        const unsigned char token = frame[at++];
        const std::size_t count = (token & 0x7fu) + 1u;
        if ((token & 0x80u) != 0) {
            std::memset(bytes.data() + out, 0, count);
        } else {
            std::memcpy(bytes.data() + out, frame + at, count);
            at += count;
        }
        out += count;
    }
    return bytes;
}

}  // namespace pineforge::detail::cold_order
