#include "global_allocation_replacement.hpp"
#include "../src/native_cold_order_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <vector>

namespace {
namespace codec = pineforge::detail::cold_order;
using Bytes = std::vector<unsigned char>;
int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);      \
    }                                                                          \
} while (false)

// Independent V1 wire specification:
// [1, mode, decoded length LE64], mode 0 = verbatim, mode 1 = token stream.
// Token 0..127 means copy the next token+1 bytes (including literal zero).
// Token 128..255 means emit token-127 zero bytes. No terminator or trailer.
// Greedy encoding splits zero/nonzero spans at 128; use mode 1 only on shrink.
// Golden frames below are stated from this spec, never from a product result.
void golden(const Bytes& input, const Bytes& expected) {
    CHECK(codec::encode(input.data(), input.size()) == expected);
    CHECK(codec::decode(expected.data(), expected.size()) == input);
}

void rejects(const Bytes& frame) {
    bool rejected = false;
    try {
        (void)codec::decode(frame.data(), frame.size());
    } catch (const codec::FormatError&) {
        rejected = true;
    }
    CHECK(rejected);
}

void golden_frames() {
    golden({}, {1, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    golden({0}, {1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}); // nonshrinking
    golden({0x91}, {1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0x91});
    golden({0, 0}, {1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0x81});
    golden({7, 0, 0}, {1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0}); // tie
    golden({0, 0, 0, 7}, {1, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0x82, 0, 7});
    golden({0, 0, 0, 0, 0x81, 0xff, 0, 0, 0, 0},
           {1, 1, 10, 0, 0, 0, 0, 0, 0, 0, 0x83, 1, 0x81, 0xff, 0x83});
    golden(Bytes(127, 0), {1, 1, 127, 0, 0, 0, 0, 0, 0, 0, 0xfe});
    golden(Bytes(128, 0), {1, 1, 128, 0, 0, 0, 0, 0, 0, 0, 0xff});
    golden(Bytes(129, 0), {1, 1, 129, 0, 0, 0, 0, 0, 0, 0, 0xff, 0x80});
    golden(Bytes(256, 0), {1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0xff, 0xff});

    // Literal lengths at both ends of the 7-bit range, inside a shrinking
    // frame. Oracle construction uses fixed spans, not the encoder's walk.
    for (const std::size_t n : {std::size_t{1}, std::size_t{127},
                                std::size_t{128}, std::size_t{129}}) {
        Bytes input(128, 0);
        input.insert(input.end(), n, 0xa5);
        input.insert(input.end(), 128, 0);
        const auto length = static_cast<std::uint64_t>(256 + n);
        Bytes expected{1, 1, static_cast<unsigned char>(length & 255u),
                       static_cast<unsigned char>(length >> 8), 0, 0, 0, 0, 0, 0, 0xff};
        if (n <= 128) {
            expected.push_back(static_cast<unsigned char>(n - 1));
            expected.insert(expected.end(), n, 0xa5);
        } else {
            expected.push_back(0x7f);
            expected.insert(expected.end(), 128, 0xa5);
            expected.push_back(0);
            expected.push_back(0xa5);
        }
        expected.push_back(0xff);
        golden(input, expected);
    }

    // Decoder accepts literal zero and a noncanonical but valid RLE stream.
    const Bytes literal_zero{1, 1, 3, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0xff, 0};
    CHECK(codec::decode(literal_zero.data(), literal_zero.size()) == (Bytes{0, 0xff, 0}));
    const Bytes all_tokens{1, 1, 129, 0, 0, 0, 0, 0, 0, 0, 0xff, 0x80};
    CHECK(codec::decode(all_tokens.data(), all_tokens.size()) == Bytes(129, 0));
}

void varied_bytes() {
    // Every byte value and byte pair, including high-bit literal bytes.
    Bytes all;
    for (unsigned i = 0; i != 256; ++i) all.push_back(static_cast<unsigned char>(i));
    const auto raw = codec::encode(all.data(), all.size());
    Bytes expected{1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    expected.insert(expected.end(), all.begin(), all.end());
    CHECK(raw == expected);
    CHECK(codec::decode(raw.data(), raw.size()) == all);
    for (unsigned first = 0; first != 256; ++first) {
        for (unsigned second = 0; second != 256; ++second) {
            const Bytes input{static_cast<unsigned char>(first),
                              static_cast<unsigned char>(second)};
            const auto frame = codec::encode(input.data(), input.size());
            CHECK(codec::decode(frame.data(), frame.size()) == input);
        }
    }

    // Reproducible varied bytes, alternate zero density and literal spans.
    // Equality oracle is the original owning input, never reserialized state.
    std::uint32_t state = 0x52e3a7b1u;
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{127},
                                std::size_t{128}, std::size_t{129},
                                std::size_t{257}, std::size_t{4099}}) {
        for (unsigned shape = 0; shape != 4; ++shape) {
            Bytes input(n);
            for (std::size_t i = 0; i != n; ++i) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                input[i] = shape == 0 || (shape == 1 && (i % 131) < 128)
                    ? 0 : static_cast<unsigned char>(state);
            }
            const auto frame = codec::encode(input.data(), input.size());
            CHECK(codec::decode(frame.data(), frame.size()) == input);
            CHECK(codec::encode(input.data(), input.size()) == frame); // determinism
        }
    }
}

void malformed_frames() {
    const Bytes valid{1, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0x83};
    // Every header truncation; then a complete header missing its token.
    for (std::size_t n = 0; n < valid.size(); ++n)
        rejects(Bytes(valid.begin(), valid.begin() + n));
    rejects({0, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0x83}); // old/unknown version
    rejects({2, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0x83});
    rejects({1, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0x83}); // unknown mode
    rejects({1, 255, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0x83});
    rejects({1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 9}); // RAW short
    rejects({1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 8}); // RAW trailer
    rejects({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}); // empty RAW trailer
    rejects({1, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0x83}); // too much zero output
    rejects({1, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0x83}); // too little output
    rejects({1, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0x83, 0x80}); // token trailer
    rejects({1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}); // literal with no byte
    rejects({1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 1, 9}); // short literal
    rejects({1, 1, 128, 0, 0, 0, 0, 0, 0, 0, 0x7f, 9}); // max literal short
    rejects({1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 9, 8}); // literal over output
    rejects({1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x80}); // run into empty output
    // Claims near arithmetic/conversion boundaries with tiny actual payload.
    // A bad implementation that reserves from this word fails by allocation,
    // which is deliberately NOT accepted as a format rejection here.
    rejects({1, 1, 255, 255, 255, 255, 255, 255, 255, 255, 0xff});
    rejects({1, 0, 255, 255, 255, 255, 255, 255, 255, 255});
    rejects({1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0xff}); // 2^32
    rejects({1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0xff}); // 2^56

    bool rejected = false;
    try { (void)codec::encode(nullptr, 1); }
    catch (const codec::FormatError&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { (void)codec::decode(nullptr, 10); }
    catch (const codec::FormatError&) { rejected = true; }
    CHECK(rejected);
    CHECK(codec::encode(nullptr, 0) == (Bytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0}));

    // Reject unrepresentable frame/input extent before touching the pointer.
    const unsigned char byte = 0;
    rejected = false;
    try { (void)codec::encode(&byte, std::numeric_limits<std::size_t>::max()); }
    catch (const codec::FormatError&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { (void)codec::decode(&byte, std::numeric_limits<std::size_t>::max()); }
    catch (const codec::FormatError&) { rejected = true; }
    CHECK(rejected);
}

void allocation_failures() {
    const unsigned char input[]{9, 8, 7};
    const unsigned char raw[]{1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 9, 8, 7};
    const unsigned char rle[]{1, 1, 128, 0, 0, 0, 0, 0, 0, 0, 0xff};
    // Disarm on every path; assertions/output occur after the refusing scope.
    for (unsigned operation = 0; operation != 3; ++operation) {
        bool allocation_failed = false;
        global_allocation::fail_after = 0;
        try {
            if (operation == 0) (void)codec::encode(input, sizeof input);
            if (operation == 1) (void)codec::decode(raw, sizeof raw);
            if (operation == 2) (void)codec::decode(rle, sizeof rle);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        } catch (...) {
            global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
            throw;
        }
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        CHECK(allocation_failed);
    }
    const unsigned char huge[]{1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
    bool format_failed = false;
    const auto refused_before = global_allocation::refused;
    global_allocation::refusing = true;
    try {
        (void)codec::decode(huge, sizeof huge);
    } catch (const codec::FormatError&) {
        format_failed = true;
    } catch (...) {
        global_allocation::refusing = false;
        throw;
    }
    global_allocation::refusing = false;
    CHECK(format_failed);
    CHECK(global_allocation::refused == refused_before); // preflight allocated nothing
}
}  // namespace

int main() {
    try {
        golden_frames();
        varied_bytes();
        malformed_frames();
        allocation_failures();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "cold_order_encoding: %s\n", error.what());
    }
    std::printf("cold_order_encoding checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
