#pragma once

#include "../src/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#elif defined(__aarch64__)
#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#endif

namespace sha256_fixture {

using pineforge::report_sink::Sha256;
namespace sha = pineforge::report_sink::sha256_internal;
using Digest = std::array<std::uint8_t, 32>;

inline void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(condition) require((condition), #condition, __LINE__)

inline std::array<char, 65> hex(const Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 65> result{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[2 * i] = digits[digest[i] >> 4];
        result[2 * i + 1] = digits[digest[i] & 15];
    }
    return result;
}

inline void expect_equal(const Digest& actual, const Digest& expected,
                         const char* path, std::size_t length) {
    if (actual != expected) {
        const auto actual_hex = hex(actual);
        const auto expected_hex = hex(expected);
        std::fprintf(stderr, "%s, %zu bytes: got %s, expected %s\n",
                     path, length, actual_hex.data(), expected_hex.data());
        std::exit(EXIT_FAILURE);
    }
}

inline void expect_hex(const Digest& actual, const char* expected, std::size_t length) {
    REQUIRE(std::strlen(expected) == 64);
    const auto actual_hex = hex(actual);
    if (std::memcmp(actual_hex.data(), expected, 64) != 0) {
        std::fprintf(stderr, "pinned vector, %zu bytes: got %s, expected %s\n",
                     length, actual_hex.data(), expected);
        std::exit(EXIT_FAILURE);
    }
}

// Independent byte-at-a-time buffering and padding. The supplied compressor
// is called directly; neither reference digest goes through Sha256::compress.
class DirectHash {
public:
    explicit DirectHash(sha::CompressBlock compress) : compress_(compress) {}

    void update(std::string_view bytes) {
        REQUIRE(compress_ != nullptr);
        bytes_ += bytes.size();
        for (unsigned char byte : bytes) {
            block_[used_++] = byte;
            if (used_ == block_.size()) {
                compress_(words_.data(), block_.data());
                used_ = 0;
            }
        }
    }

    Digest digest() const {
        REQUIRE(compress_ != nullptr);
        auto words = words_;
        std::array<std::uint8_t, 128> tail{};
        std::copy_n(block_.begin(), used_, tail.begin());
        tail[used_] = 0x80;
        const std::size_t padded_size = used_ < 56 ? 64 : 128;
        for (unsigned i = 0; i < 8; ++i)
            tail[padded_size - 1 - i] = static_cast<std::uint8_t>((bytes_ * 8) >> (8 * i));
        for (std::size_t offset = 0; offset < padded_size; offset += 64)
            compress_(words.data(), tail.data() + offset);
        Digest result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = static_cast<std::uint8_t>(words[i / 4] >> (24 - 8 * (i % 4)));
        return result;
    }

private:
    sha::CompressBlock compress_;
    std::array<std::uint32_t, 8> words_{{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t bytes_ = 0;
};

class Generator {
public:
    std::uint32_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

private:
    std::uint32_t state_ = 0x6d2b79f5;
};

class Comparison {
public:
    explicit Comparison(sha::CompressBlock hardware)
        : hardware_(hardware), split_hardware_(hardware), hardware_compressor_(hardware) {
        REQUIRE(hardware == nullptr || hardware != sha::compress_portable);
    }

    void bulk(std::string_view bytes) {
        portable_.update(bytes);
        dispatched_.update(bytes);
        if (hardware_compressor_) hardware_.update(bytes);
        bulk_bytes_ += bytes.size();
    }

    void split(std::string_view bytes) {
        split_portable_.update(bytes);
        split_dispatched_.update(bytes);
        if (hardware_compressor_) split_hardware_.update(bytes);
        split_bytes_ += bytes.size();
    }

    void check_prefix() const {
        const auto prefix = split_portable_.digest();
        expect_equal(split_dispatched_.digest(), prefix, "dispatch prefix", split_bytes_);
        if (hardware_compressor_)
            expect_equal(split_hardware_.digest(), prefix, "hardware prefix", split_bytes_);
    }

    bool verify(const char* expected) const {
        REQUIRE(bulk_bytes_ == split_bytes_);
        const auto reference = portable_.digest();
        if (expected) expect_hex(reference, expected, bulk_bytes_);
        expect_equal(dispatched_.digest(), reference, "dispatch bulk", bulk_bytes_);
        expect_equal(dispatched_.digest(), reference, "dispatch repeated digest", bulk_bytes_);
        expect_equal(split_portable_.digest(), reference, "portable split", bulk_bytes_);
        expect_equal(split_dispatched_.digest(), reference, "dispatch split", bulk_bytes_);
        if (!hardware_compressor_) return false;
        expect_equal(hardware_.digest(), reference, "hardware bulk", bulk_bytes_);
        expect_equal(split_hardware_.digest(), reference, "hardware split", bulk_bytes_);
        return true;
    }

private:
    DirectHash portable_{sha::compress_portable};
    Sha256 dispatched_;
    DirectHash hardware_;
    DirectHash split_portable_{sha::compress_portable};
    Sha256 split_dispatched_;
    DirectHash split_hardware_;
    sha::CompressBlock hardware_compressor_;
    std::size_t bulk_bytes_ = 0;
    std::size_t split_bytes_ = 0;
};

inline bool check_message(std::string_view message, Generator& generator,
                          sha::CompressBlock hardware, const char* expected = nullptr) {
    Comparison comparison(hardware);
    comparison.bulk(message);
    comparison.split({});
    std::size_t offset = 0;
    std::size_t splits = 0;
    while (offset < message.size()) {
        const auto take = std::min(message.size() - offset,
                                   std::size_t(1 + generator.next() % 193));
        comparison.split(message.substr(offset, take));
        offset += take;
        ++splits;
        if (splits == 1) comparison.check_prefix(); // Then continue updating.
        if ((generator.next() & 3) == 0) comparison.split({});
    }
    REQUIRE(offset == message.size());
    REQUIRE(message.empty() || splits != 0);
    comparison.split({});
    return comparison.verify(expected);
}

inline void check_repeated_a(std::size_t count, const char* expected,
                             Generator& generator, sha::CompressBlock hardware) {
    REQUIRE(count > 0);
    std::array<char, 1024> bytes{};
    bytes.fill('a');
    Comparison comparison(hardware);
    std::size_t fed = 0;
    while (fed < count) {
        const auto take = std::min(count - fed, bytes.size());
        comparison.bulk({bytes.data(), take});
        fed += take;
    }
    REQUIRE(fed == count);
    fed = 0;
    std::size_t splits = 0;
    comparison.split({});
    while (fed < count) {
        const auto take = std::min(count - fed,
                                   std::size_t(1 + generator.next() % bytes.size()));
        comparison.split({bytes.data(), take});
        fed += take;
        ++splits;
        if (splits == 1) comparison.check_prefix();
        comparison.split({});
    }
    REQUIRE(fed == count);
    REQUIRE(splits != 0);
    REQUIRE(comparison.verify(expected) == (hardware != nullptr));
}

// NIST SHA-256 examples, recalled as literals. Supplementary repeated-'a'
// boundary fixtures below are separate from those published examples.
constexpr char empty_digest[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr char abc_digest[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr char message_448[] =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
constexpr char digest_448[] =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

struct RepeatedVector {
    std::size_t count;
    const char* expected;
};

constexpr RepeatedVector repeated_vectors[] = {
    {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
    {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
    {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
    {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
    {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
    {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
    {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
    {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    {1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
};

inline void check_vectors(sha::CompressBlock hardware) {
    Generator generator;
    std::size_t checked = 0;
    REQUIRE(check_message("", generator, hardware, empty_digest) == (hardware != nullptr));
    ++checked;
    REQUIRE(check_message("abc", generator, hardware, abc_digest) == (hardware != nullptr));
    ++checked;
    static_assert(sizeof message_448 - 1 == 56, "The published message is 448 bits");
    REQUIRE(check_message(message_448, generator, hardware, digest_448) == (hardware != nullptr));
    ++checked;
    for (const auto& vector : repeated_vectors) {
        check_repeated_a(vector.count, vector.expected, generator, hardware);
        ++checked;
    }
    REQUIRE(checked == 13);
    std::printf("pinned vectors checked: %zu\n", checked);
}

inline const char* name(sha::Implementation implementation) {
    switch (implementation) {
    case sha::Implementation::portable: return "portable";
    case sha::Implementation::x86_sha: return "x86 SHA-NI";
    case sha::Implementation::arm_sha2: return "AArch64 SHA2";
    }
    std::exit(EXIT_FAILURE);
}

// Query the platform independently, without consulting the library's selector.
inline sha::Implementation platform_implementation() {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__get_cpuid_max(0, nullptr) < 7) return sha::Implementation::portable;
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(1, 0, eax, ebx, ecx, edx);
    const bool vectors = (ecx & bit_SSSE3) != 0 && (ecx & bit_SSE4_1) != 0;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    if (vectors && (ebx & bit_SHA) != 0) return sha::Implementation::x86_sha;
#elif defined(__aarch64__) && defined(__linux__) && defined(HWCAP_SHA2)
    if ((getauxval(AT_HWCAP) & HWCAP_SHA2) != 0) return sha::Implementation::arm_sha2;
#elif defined(__aarch64__) && defined(__APPLE__)
    int supported = 0;
    std::size_t size = sizeof supported;
    if (sysctlbyname("hw.optional.arm.FEAT_SHA256", &supported, &size, nullptr, 0) == 0
        && size == sizeof supported && supported != 0)
        return sha::Implementation::arm_sha2;
#endif
    return sha::Implementation::portable;
}

inline sha::CompressBlock check_selection() {
    const auto expected = platform_implementation();
    const auto selected = sha::selected_implementation();
    const auto hardware = sha::hardware_compressor();
    std::printf("feature query: %s; selected: %s; direct hardware: %s\n",
                name(expected), name(selected), hardware ? "present" : "absent");
    REQUIRE(selected == expected);
    REQUIRE((hardware != nullptr) == (expected != sha::Implementation::portable));
    REQUIRE(hardware == nullptr || hardware != sha::compress_portable);
    Sha256 dispatched;
    dispatched.update("abc");
    expect_hex(dispatched.digest(), abc_digest, 3);
    if (hardware) {
        DirectHash direct(hardware);
        direct.update("abc");
        expect_hex(direct.digest(), abc_digest, 3);
    }
    REQUIRE(sha::selected_implementation() == selected);
    REQUIRE(sha::hardware_compressor() == hardware);
    return hardware;
}

inline void check_differential(sha::CompressBlock hardware) {
    check_vectors(hardware);
    constexpr std::size_t iterations = 20000;
    static_assert(iterations >= 20000, "Exercise at least twenty thousand messages");
    Generator generator;
    alignas(16) std::array<std::uint8_t, 1502> storage{};
    std::array<bool, 1501> seen_lengths{};
    std::size_t checked = 0;
    std::size_t hardware_checked = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        // Exhaust every length first, then vary it with the fixed-seed generator.
        const auto length = i < seen_lengths.size() ? i
            : std::size_t(generator.next() % seen_lengths.size());
        seen_lengths[length] = true;
        for (std::size_t j = 0; j < length; ++j)
            storage[j + 1] = static_cast<std::uint8_t>(generator.next() >> 24);
        // Deliberately unaligned input exercises both hardware load sequences.
        const std::string_view message(
            reinterpret_cast<const char*>(storage.data() + 1), length);
        if (check_message(message, generator, hardware, length == 0 ? empty_digest : nullptr))
            ++hardware_checked;
        ++checked;
    }
    REQUIRE(checked == iterations);
    REQUIRE(hardware_checked == (hardware ? iterations : 0));
    for (bool seen : seen_lengths) REQUIRE(seen); // Includes every edge 0..130.
    std::printf("differential messages: %zu; hardware messages: %zu; lengths: 0..1500\n",
                checked, hardware_checked);
}

#undef REQUIRE

} // namespace sha256_fixture
