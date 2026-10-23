#pragma once

// Ordered rounding transitions, E1 P7. These summaries model an RN-even
// binary64 LEFT fold, not a reassociated sum. No store or engine dependency.
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pineforge::source::cohort_exposure_arith {

static_assert(sizeof(double) == 8, "exposure requires an eight-byte double");
static_assert(std::numeric_limits<double>::is_iec559, "exposure requires IEC559");
static_assert(std::numeric_limits<double>::digits == 53, "exposure requires binary64");
static_assert(std::numeric_limits<double>::radix == 2, "exposure requires radix two");
static_assert(std::numeric_limits<double>::min_exponent == -1021
                  && std::numeric_limits<double>::max_exponent == 1024,
              "exposure requires binary64 exponent limits");
#if defined(FLT_EVAL_METHOD)
static_assert(FLT_EVAL_METHOD == 0, "exposure forbids excess evaluation precision");
#endif

inline constexpr std::uint64_t B = UINT64_C(1) << 52;
inline constexpr std::uint64_t S = UINT64_C(1) << 53;
inline constexpr std::uint64_t FRAC = B - 1;
inline constexpr std::uint64_t INF = UINT64_C(0x7ff0000000000000);
inline constexpr std::size_t summary_rows = 54;
inline constexpr std::size_t summary_bytes = 872;
using Pair = std::array<std::uint64_t, 2>;

inline std::uint64_t bits(double value) noexcept {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, 8);
    return raw;
}

inline double from_bits(std::uint64_t raw) noexcept {
    double value = 0.0;
    std::memcpy(&value, &raw, 8);
    return value;
}

inline bool eligible(std::uint64_t raw) noexcept {
    const auto exponent = (raw >> 52) & UINT64_C(0x7ff);
    return (raw >> 63) == 0 && exponent != 0x7ff
        && (exponent != 0 || (raw & FRAC) != 0);
}

inline std::uint64_t operand_base(std::uint64_t raw) noexcept {
    if (!eligible(raw)) return 0;
    return std::max(UINT64_C(1), (raw >> 52) & UINT64_C(0x7ff));
}

inline void require_region(std::int32_t g) {
    if (g < 1 || g > 2046)
        throw std::logic_error("exposure region outside 1..2046");
}

inline Pair leaf_pair(std::uint64_t raw, std::int32_t g) {
    require_region(g);
    if (!eligible(raw)) return {0, 0};
    const auto E = static_cast<std::int32_t>((raw >> 52) & UINT64_C(0x7ff));
    const auto f = raw & FRAC;
    const std::uint64_t M = E == 0 ? f : B + f;
    const std::int32_t t = E == 0 ? 1 - g : E - g;
    if (t >= 0) {
        if (t >= 53) return {S, S};
        if (M >= (S >> t)) return {S, S};
        // Here 0 <= t <= 52 and M*2^t < S.
        const auto m = M << t;
        return {m, m};
    }
    const std::int32_t n = -t;
    if (n >= 54) return {0, 0}; // M < 2^53: strictly below half a quantum.
    // Here 1 <= n <= 53; no shift reaches the width of uint64_t.
    const auto m = M >> n;
    const auto rem = M & ((UINT64_C(1) << n) - 1);
    const auto half = UINT64_C(1) << (n - 1);
    if (rem < half) return {m, m};
    if (rem > half) return {m + 1, m + 1};
    return {m + (m & 1), m + ((1 + (m & 1)) & 1)};
}

// Bpair is pair-like so that a saturated first increment need not read its
// successor at all. S carries no outgoing parity. The ordinary Pair is used
// in summaries; a caller may also supply a checked/lazy pair source.
template<class FollowingPair>
inline Pair compose_pair(const Pair& A, const FollowingPair& Bpair) {
    Pair D{};
    for (std::size_t p = 0; p < 2; ++p) {
        const auto a = A[p];
        if (a > S) throw std::logic_error("exposure increment exceeds S");
        if (a == S) {
            D[p] = S;
            continue;
        }
        const auto p_after = (p + (a & 1)) & 1;
        const std::uint64_t b = Bpair[p_after];
        if (b > S) throw std::logic_error("exposure increment exceeds S");
        if (b == S || a >= S - b) {
            D[p] = S;
        } else {
            D[p] = a + b;
        }
    }
    return D;
}

struct Summary {
    std::uint64_t b = 0; // 0: identity; otherwise largest clamped operand binade.
    std::array<Pair, summary_rows> row{};
};
static_assert(sizeof(Summary) == summary_bytes, "exposure summary must be 872 bytes");

inline Pair get_pair(const Summary& T, std::int32_t g) {
    require_region(g);
    if (T.b > 2046) throw std::logic_error("exposure summary header exceeds 2046");
    if (T.b == 0) return {0, 0};
    if (g < static_cast<std::int32_t>(T.b)) return {S, S};
    if (g >= static_cast<std::int32_t>(T.b) + 54) return {0, 0};
    const auto pair = T.row[static_cast<std::size_t>(g - static_cast<std::int32_t>(T.b))];
    if (pair[0] > S || pair[1] > S)
        throw std::logic_error("exposure increment exceeds S");
    return pair;
}

inline Summary make_leaf(std::uint64_t raw) {
    Summary T{};
    T.b = operand_base(raw);
    if (T.b == 0) return T;
    for (std::size_t i = 0; i < summary_rows; ++i) {
        const auto g = static_cast<std::int32_t>(T.b) + static_cast<std::int32_t>(i);
        if (g <= 2046) T.row[i] = leaf_pair(raw, g);
    }
    return T;
}

inline Summary compose_summary(const Summary& A, const Summary& Bsummary) {
    if (A.b > 2046 || Bsummary.b > 2046)
        throw std::logic_error("exposure summary header exceeds 2046");
    Summary T{};
    T.b = std::max(A.b, Bsummary.b); // Recompute after replacement/deletion too.
    if (T.b == 0) return T;
    for (std::size_t i = 0; i < summary_rows; ++i) {
        const auto g = static_cast<std::int32_t>(T.b) + static_cast<std::int32_t>(i);
        if (g <= 2046) T.row[i] = compose_pair(get_pair(A, g), get_pair(Bsummary, g));
    }
    return T;
}

// Persistence is explicit LE64, never a memcpy of host struct layout.
using SummaryBytes = std::array<std::uint8_t, summary_bytes>;

inline void write_le64(std::uint8_t* destination, std::uint64_t word) noexcept {
    for (unsigned i = 0; i < 8; ++i)
        destination[i] = static_cast<std::uint8_t>(word >> (8 * i));
}

inline std::uint64_t read_le64(const std::uint8_t* source) noexcept {
    std::uint64_t word = 0;
    for (unsigned i = 0; i < 8; ++i)
        word |= static_cast<std::uint64_t>(source[i]) << (8 * i);
    return word;
}

inline void validate_summary(const Summary& T) {
    if (T.b > 2046) throw std::logic_error("exposure summary header exceeds 2046");
    for (std::size_t i = 0; i < summary_rows; ++i) {
        const auto& pair = T.row[i];
        if (pair[0] > S || pair[1] > S)
            throw std::logic_error("exposure increment exceeds S");
        if ((T.b == 0 || T.b + i > 2046) && (pair[0] != 0 || pair[1] != 0))
            throw std::logic_error("exposure unused summary row must be zero");
    }
}

inline SummaryBytes serialize_summary(const Summary& T) {
    validate_summary(T);
    SummaryBytes encoded{};
    write_le64(encoded.data(), T.b);
    for (std::size_t i = 0; i < summary_rows; ++i)
        for (std::size_t p = 0; p < 2; ++p)
            write_le64(encoded.data() + 8 + 16 * i + 8 * p, T.row[i][p]);
    return encoded;
}

inline Summary parse_summary(const std::uint8_t* encoded, std::size_t length) {
    if (length != summary_bytes || encoded == nullptr)
        throw std::logic_error("exposure summary length must be 872");
    Summary T{};
    T.b = read_le64(encoded);
    for (std::size_t i = 0; i < summary_rows; ++i)
        for (std::size_t p = 0; p < 2; ++p)
            T.row[i][p] = read_le64(encoded + 8 + 16 * i + 8 * p);
    validate_summary(T);
    return T;
}

struct Accumulator {
    std::int32_t g;
    std::uint64_t k;
};

inline Accumulator decode_accumulator(std::uint64_t raw) {
    if (raw != 0 && !eligible(raw))
        throw std::logic_error("exposure accumulator must be positive finite or +0");
    const auto E = static_cast<std::int32_t>((raw >> 52) & UINT64_C(0x7ff));
    return {std::max(1, E), E == 0 ? raw & FRAC : B + (raw & FRAC)};
}

inline std::uint64_t encode_accumulator(std::int32_t g, std::uint64_t k) {
    require_region(g);
    if (k >= S || (g != 1 && k < B))
        throw std::logic_error("exposure accumulator significand outside region");
    if (g == 1 && k < B) return k;
    return (static_cast<std::uint64_t>(g) << 52) | (k - B);
}

// Keep a separate actual binary64 operation, including in optimized builds.
// Volatile operands also make this a useful dynamic-environment unit probe.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
inline std::uint64_t real_binary64_add(std::uint64_t raw_x, std::uint64_t raw_a) noexcept {
    double x = 0.0, a = 0.0;
    std::memcpy(&x, &raw_x, 8);
    std::memcpy(&a, &raw_a, 8);
    volatile double actual_x = x, actual_a = a;
    const double y = actual_x + actual_a; // The only double addition in this core.
    std::uint64_t raw_y = 0;
    std::memcpy(&raw_y, &y, 8);
    return raw_y;
}

// Node{} is the null handle. Source supplies left/right/is_leaf/summary/
// raw_units(node). Save handles before any child read: a store adapter may
// reuse its transfer frame on the next call. Only rejected blocks descend.
// Round 2's word-depth tree must fit this fixed 128-handle workspace.
template<class NodeSource, class Node>
inline std::uint64_t evaluate_raw(NodeSource& source, Node node,
                                  std::uint64_t raw_total = 0) {
    std::array<Node, 128> pending{};
    std::size_t count = 0;
    for (;;) {
        if (raw_total == INF) return raw_total; // Absorbing until final clamp.
        if (node != Node{}) {
            const auto accumulator = decode_accumulator(raw_total);
            const Pair D = get_pair(source.summary(node), accumulator.g);
            const auto d = D[accumulator.k & 1];
            if (d < S - accumulator.k) {
                raw_total = encode_accumulator(accumulator.g, accumulator.k + d);
            } else if (source.is_leaf(node)) {
                const auto raw = source.raw_units(node);
                if (eligible(raw)) raw_total = real_binary64_add(raw_total, raw);
            } else {
                const Node left = source.left(node);
                const Node right = source.right(node);
                if (right != Node{}) {
                    if (count == pending.size())
                        throw std::logic_error("exposure traversal exceeds word-depth workspace");
                    pending[count++] = right;
                }
                node = left;
                continue; // Resume here after crossings, never from the root.
            }
        }
        if (count == 0) return raw_total;
        node = pending[--count];
    }
}

template<class NodeSource, class Node>
inline std::uint64_t evaluate(NodeSource& source, Node root) {
    const auto raw = evaluate_raw(source, root);
    return raw == INF ? UINT64_C(0) : raw;
}

inline bool exposure_fp_environment_ok() noexcept {
    volatile double one = 0x1p0, half = 0x1p-53;
    volatile double odd = 0x1.0000000000001p0;
    volatile double subnormal = 0x1p-1074, zero = 0.0;
    volatile double low_even = 0x1p-1021, low_odd = 0x1.0000000000001p-1021;
    // 1 + 2^-53 ties down to even 1.
    const bool even_tie = real_binary64_add(bits(one), bits(half)) == UINT64_C(0x3ff0000000000000);
    // (1 + 2^-52) + 2^-53 ties up to 1 + 2^-51.
    const bool odd_tie = real_binary64_add(bits(odd), bits(half)) == UINT64_C(0x3ff0000000000002);
    // At 2^-1021 the quantum is 2^-1073: minimum subnormal is half.
    const bool low_even_tie =
        real_binary64_add(bits(low_even), bits(subnormal)) == UINT64_C(0x0020000000000000);
    const bool low_odd_tie =
        real_binary64_add(bits(low_odd), bits(subnormal)) == UINT64_C(0x0020000000000002);
    // 2^-1074 + 0 = 2^-1074 and 2^-1074 * 1 = 2^-1074, exactly.
    const bool gradual_add = real_binary64_add(bits(subnormal), bits(zero)) == UINT64_C(1);
    volatile double product = subnormal * one;
    const bool gradual_multiply = bits(product) == UINT64_C(1);
    return even_tie && odd_tie && low_even_tie && low_odd_tie
        && gradual_add && gradual_multiply;
}

[[noreturn]] inline void exposure_fp_environment_refuse() {
    throw coded<std::logic_error>(
        RunFailureCode::engine_invariant, {},
        "cohort exposure requires binary64 round-to-nearest/even with gradual underflow");
}

} // namespace pineforge::source::cohort_exposure_arith
