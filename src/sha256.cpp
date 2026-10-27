#include "sha256.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#endif

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>
#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#endif

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

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

// Keep the archive's baseline ISA: only this function may use SHA/SSSE3/SSE4.1.
__attribute__((target("sha,ssse3,sse4.1"), noinline))
void compress_x86_sha(std::uint32_t* words, const std::uint8_t* block) noexcept {
    auto abcd = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words));
    auto efgh = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words + 4));
    const auto shuffled_abcd = _mm_shuffle_epi32(abcd, 0xb1);
    efgh = _mm_shuffle_epi32(efgh, 0x1b);
    // SHA256RNDS2 uses ABEF and CDGH, from the most to least significant lane.
    auto abef = _mm_alignr_epi8(shuffled_abcd, efgh, 8);
    auto cdgh = _mm_blend_epi16(efgh, shuffled_abcd, 0xf0);
    const auto saved_abef = abef;
    const auto saved_cdgh = cdgh;
    const auto swap_bytes = _mm_set_epi8(
        12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    auto m0 = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(block)), swap_bytes);
    auto m1 = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16)), swap_bytes);
    auto m2 = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32)), swap_bytes);
    auto m3 = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48)), swap_bytes);

    for (unsigned group = 0; group < 16; ++group) {
        auto rounds = _mm_add_epi32(m0, _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(round_constants + 4 * group)));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, rounds);
        rounds = _mm_shuffle_epi32(rounds, 0x0e);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, rounds);

        auto next = m3;
        if (group < 12) {
            next = _mm_sha256msg1_epu32(m0, m1);
            next = _mm_add_epi32(next, _mm_alignr_epi8(m3, m2, 4));
            next = _mm_sha256msg2_epu32(next, m3);
        }
        m0 = m1;
        m1 = m2;
        m2 = m3;
        m3 = next;
    }

    abef = _mm_add_epi32(abef, saved_abef);
    cdgh = _mm_add_epi32(cdgh, saved_cdgh);
    const auto shuffled_abef = _mm_shuffle_epi32(abef, 0x1b);
    cdgh = _mm_shuffle_epi32(cdgh, 0xb1);
    abcd = _mm_blend_epi16(shuffled_abef, cdgh, 0xf0);
    efgh = _mm_alignr_epi8(cdgh, shuffled_abef, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(words), abcd);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(words + 4), efgh);
}

bool x86_has_sha() noexcept {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    constexpr unsigned vector_features = (1u << 9) | (1u << 19); // SSSE3, SSE4.1
    if ((ecx & vector_features) != vector_features) return false;
    return __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)
        && (ebx & (1u << 29)) != 0; // SHA
}

#endif

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))

inline uint32x4_t load_big_endian_words(const std::uint8_t* block) noexcept {
    const auto bytes = vld1q_u8(block);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return vreinterpretq_u32_u8(bytes);
#else
    return vreinterpretq_u32_u8(vrev32q_u8(bytes));
#endif
}

#if defined(__clang__)
__attribute__((target("sha2"), noinline))
#else
__attribute__((target("+crypto"), noinline))
#endif
void compress_arm_sha2(std::uint32_t* words, const std::uint8_t* block) noexcept {
    auto abcd = vld1q_u32(words);
    auto efgh = vld1q_u32(words + 4);
    const auto saved_abcd = abcd;
    const auto saved_efgh = efgh;
    auto m0 = load_big_endian_words(block);
    auto m1 = load_big_endian_words(block + 16);
    auto m2 = load_big_endian_words(block + 32);
    auto m3 = load_big_endian_words(block + 48);

    for (unsigned group = 0; group < 16; ++group) {
        const auto rounds = vaddq_u32(m0, vld1q_u32(round_constants + 4 * group));
        const auto previous_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, rounds);
        efgh = vsha256h2q_u32(efgh, previous_abcd, rounds);

        auto next = m3;
        if (group < 12) {
            next = vsha256su0q_u32(m0, m1);
            next = vsha256su1q_u32(next, m2, m3);
        }
        m0 = m1;
        m1 = m2;
        m2 = m3;
        m3 = next;
    }

    vst1q_u32(words, vaddq_u32(abcd, saved_abcd));
    vst1q_u32(words + 4, vaddq_u32(efgh, saved_efgh));
}

bool arm_has_sha2() noexcept {
#if defined(__linux__) && defined(HWCAP_SHA2)
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#elif defined(__APPLE__)
    int supported = 0;
    std::size_t size = sizeof supported;
    return sysctlbyname("hw.optional.arm.FEAT_SHA256", &supported, &size, nullptr, 0) == 0
        && size == sizeof supported && supported != 0;
#else
    return false;
#endif
}

#endif

struct Backend {
    sha256_internal::Implementation implementation;
    sha256_internal::CompressBlock compress;
};

Backend choose_backend() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (x86_has_sha())
        return {sha256_internal::Implementation::x86_sha, compress_x86_sha};
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    if (arm_has_sha2())
        return {sha256_internal::Implementation::arm_sha2, compress_arm_sha2};
#endif
    return {sha256_internal::Implementation::portable, sha256_internal::compress_portable};
}

const Backend& selected_backend() noexcept {
    // C++ local-static initialization queries the platform once, thread-safely.
    static const Backend backend = choose_backend();
    return backend;
}

} // namespace

Sha256::Sha256() noexcept
    : words_{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}} {}

void Sha256::compress(const std::uint8_t* block) noexcept {
    selected_backend().compress(words_.data(), block);
}

sha256_internal::Implementation sha256_internal::selected_implementation() noexcept {
    return selected_backend().implementation;
}

sha256_internal::CompressBlock sha256_internal::hardware_compressor() noexcept {
    const auto& backend = selected_backend();
    return backend.implementation == Implementation::portable ? nullptr : backend.compress;
}

void sha256_internal::compress_portable(
    std::uint32_t* words, const std::uint8_t* block) noexcept {
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
    auto a = words[0];
    auto b = words[1];
    auto c = words[2];
    auto d = words[3];
    auto e = words[4];
    auto f = words[5];
    auto g = words[6];
    auto h = words[7];
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
    words[0] += a;
    words[1] += b;
    words[2] += c;
    words[3] += d;
    words[4] += e;
    words[5] += f;
    words[6] += g;
    words[7] += h;
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
