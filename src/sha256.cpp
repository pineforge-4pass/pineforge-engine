#include "sha256.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pineforge {
namespace report_sink {
namespace {

constexpr std::uint32_t round_constants[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned shift) noexcept {
    return (value >> shift) | (value << (32 - shift));
}

} // namespace

Sha256::Sha256() noexcept
    : words_{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}} {}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::uint32_t schedule[64];
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned offset = 4 * i;
        schedule[i] = (std::uint32_t(block[offset]) << 24)
            | (std::uint32_t(block[offset + 1]) << 16)
            | (std::uint32_t(block[offset + 2]) << 8)
            | std::uint32_t(block[offset + 3]);
    }
    for (unsigned i = 16; i < 64; ++i) {
        const auto x = schedule[i - 15];
        const auto y = schedule[i - 2];
        const auto small0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
        const auto small1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
        schedule[i] = schedule[i - 16] + small0 + schedule[i - 7] + small1;
    }
    auto a = words_[0];
    auto b = words_[1];
    auto c = words_[2];
    auto d = words_[3];
    auto e = words_[4];
    auto f = words_[5];
    auto g = words_[6];
    auto h = words_[7];
    for (unsigned i = 0; i < 64; ++i) {
        const auto big1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto first = h + big1 + choose + round_constants[i] + schedule[i];
        const auto big0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto second = big0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    words_[0] += a;
    words_[1] += b;
    words_[2] += c;
    words_[3] += d;
    words_[4] += e;
    words_[5] += f;
    words_[6] += g;
    words_[7] += h;
}

void Sha256::update(std::string_view bytes) {
    const auto max_bytes = std::numeric_limits<std::uint64_t>::max() / 8;
    if (bytes.size() > max_bytes - bytes_)
        throw std::length_error("SHA-256 message exceeds the FIPS 180-4 length bound");
    bytes_ += bytes.size();
    if (used_ != 0) {
        const auto take = std::min(block_.size() - used_, bytes.size());
        if (take != 0) std::memcpy(block_.data() + used_, bytes.data(), take);
        used_ += take;
        bytes.remove_prefix(take);
        if (used_ == block_.size()) {
            compress(block_.data());
            used_ = 0;
        }
    }
    while (bytes.size() >= block_.size()) {
        compress(reinterpret_cast<const std::uint8_t*>(bytes.data()));
        bytes.remove_prefix(block_.size());
    }
    if (!bytes.empty()) {
        std::memcpy(block_.data(), bytes.data(), bytes.size());
        used_ = bytes.size();
    }
}

std::array<std::uint8_t, 32> Sha256::digest() const noexcept {
    Sha256 copy = *this;
    const auto bits = bytes_ * 8;
    copy.block_[copy.used_++] = 0x80;
    if (copy.used_ > 56) {
        std::fill(copy.block_.begin() + copy.used_, copy.block_.end(), 0);
        copy.compress(copy.block_.data());
        copy.used_ = 0;
    }
    std::fill(copy.block_.begin() + copy.used_, copy.block_.begin() + 56, 0);
    for (unsigned i = 0; i < 8; ++i)
        copy.block_[63 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    copy.compress(copy.block_.data());
    std::array<std::uint8_t, 32> result{};
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j)
            result[4 * i + j] = static_cast<std::uint8_t>(copy.words_[i] >> (24 - 8 * j));
    return result;
}

std::string Sha256::hex_digest() const {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest()) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

} // namespace report_sink
} // namespace pineforge
