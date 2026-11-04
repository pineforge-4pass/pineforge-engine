#pragma once

#include <cstddef>
#include <exception>
#include <vector>

// Private, process-local cold-book storage; no native object or journal schema.
namespace pineforge::detail::cold_order {

class FormatError final : public std::exception {
public:
    const char* what() const noexcept override { return "invalid cold book byte frame"; }
};

// V1 frame (no sniffing/magic): byte 0 = 1, byte 1 = mode (0 RAW, 1 RLE),
// bytes 2..9 = decoded length as an unsigned 64-bit little-endian word.
// RAW consumes exactly that many payload bytes. RLE tokens use bit 7 for a
// zero run; bits 0..6 encode length minus one (1..128). A literal token
// (bit 7 clear) is followed by exactly its length in bytes, which may be zero.
// Every payload byte belongs to a token; total decoded length must match.
//
// Encoding greedily splits zero/nonzero spans at 128 bytes. RLE is used only
// if its payload is strictly smaller; ties and empty input use RAW.
// encode accepts null only for zero-length input; decode requires a frame.
// FormatError is structural; allocation failures propagate unchanged.
// decode validates tokens and their
// actual expansion before allocating output, without an arbitrary size cap.
std::vector<unsigned char> encode(const unsigned char* bytes, std::size_t length);
std::vector<unsigned char> decode(const unsigned char* frame, std::size_t length);

}  // namespace pineforge::detail::cold_order
