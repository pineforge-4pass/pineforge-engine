#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pineforge {
namespace report_sink {

// FIPS 180-4 SHA-256. Finalization reads a copy, so a digest query neither
// consumes the prefix nor changes the bytes a later update will hash.
class Sha256 {
public:
    Sha256() noexcept;
    void update(std::string_view bytes);
    std::array<std::uint8_t, 32> digest() const noexcept;
    std::string hex_digest() const;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> words_;
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t bytes_ = 0;
};

} // namespace report_sink
} // namespace pineforge
