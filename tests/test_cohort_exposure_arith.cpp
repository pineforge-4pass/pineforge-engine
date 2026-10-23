// EI1 round 1. Pure in-memory arithmetic; box execution supplies the verdict.
#include "cohort_exposure_oracle.hpp"
#include <pineforge/source/cohort_exposure_arith.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace ar = pineforge::source::cohort_exposure_arith;
namespace ce = cohort_exposure_test;

struct Checks {
    std::string name;
    std::size_t checkpoints = 0;

    void require(bool condition, const char* message) {
        ++checkpoints;
        if (!condition) throw std::runtime_error(message);
    }
    void word(std::uint64_t actual, std::uint64_t expected, const char* message) {
        ++checkpoints;
        if (actual == expected) return;
        std::fprintf(stderr, "case=%s actual=%016llx expected=%016llx\n",
                     name.c_str(), static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(expected));
        throw std::runtime_error(message);
    }
    void pair(const ar::Pair& actual, const ar::Pair& expected, const char* message) {
        word(actual[0], expected[0], message);
        word(actual[1], expected[1], message);
    }
    template<class Action>
    void refuses(Action action, const char* message) {
        bool refused = false;
        try {
            action();
        } catch (const std::logic_error&) {
            refused = true;
        }
        require(refused, message);
    }
};

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        auto z = (state += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        return z ^ (z >> 31);
    }
};

enum class Shape { left_comb, right_comb, balanced, random_split };

// This test container is deliberately rebuilt/repaired in RAM. It is NOT
// the round-2 store or a claim about mutation cost. The arithmetic payload
// and the evaluator are the production helpers.
class TransitionTree {
public:
    using Handle = std::size_t;
    Handle root = 0;
    std::size_t summary_reads = 0, operand_reads = 0;
    std::vector<Handle> summary_trace;

    TransitionTree(const std::vector<std::uint64_t>& operands, Shape shape,
                   std::uint64_t seed = UINT64_C(0x534534454931)) {
        leaf_handles_.resize(operands.size());
        Rng rng{seed};
        root = build(operands, 0, operands.size(), shape, rng);
    }

    Handle left(Handle h) const { return nodes_.at(h - 1).left; }
    Handle right(Handle h) const { return nodes_.at(h - 1).right; }
    bool is_leaf(Handle h) const { return nodes_.at(h - 1).leaf; }
    const ar::Summary& summary(Handle h) {
        ++summary_reads;
        summary_trace.push_back(h);
        return nodes_.at(h - 1).sum;
    }
    std::uint64_t raw_units(Handle h) {
        ++operand_reads;
        return nodes_.at(h - 1).raw;
    }
    const ar::Summary& root_summary() const { return nodes_.at(root - 1).sum; }
    void reset_counts() {
        summary_reads = 0;
        operand_reads = 0;
        summary_trace.clear();
    }

    // A deleted position may be represented by an identity leaf in this
    // arithmetic-only fixture. The rebuilt comparison actually omits it.
    void set_operand(std::size_t position, std::uint64_t raw) {
        auto& leaf = nodes_.at(leaf_handles_.at(position) - 1);
        leaf.raw = raw;
        leaf.sum = ar::make_leaf(raw);
        repair(root);
    }

private:
    struct Node {
        Handle left = 0, right = 0;
        bool leaf = true;
        std::uint64_t raw = 0;
        ar::Summary sum{};
    };
    std::vector<Node> nodes_;
    std::vector<Handle> leaf_handles_;

    Handle build(const std::vector<std::uint64_t>& v, std::size_t first,
                 std::size_t last, Shape shape, Rng& rng) {
        if (first == last) return 0;
        Node node;
        if (last - first == 1) {
            node.raw = v[first];
            node.sum = ar::make_leaf(node.raw);
            nodes_.push_back(node);
            leaf_handles_[first] = nodes_.size();
            return nodes_.size();
        }
        std::size_t split = first + (last - first) / 2;
        if (shape == Shape::left_comb) split = last - 1;
        if (shape == Shape::right_comb) split = first + 1;
        if (shape == Shape::random_split)
            split = first + 1 + static_cast<std::size_t>(rng.next() % (last - first - 1));
        node.leaf = false;
        node.left = build(v, first, split, shape, rng);
        node.right = build(v, split, last, shape, rng);
        node.sum = ar::compose_summary(nodes_.at(node.left - 1).sum,
                                       nodes_.at(node.right - 1).sum);
        nodes_.push_back(node);
        return nodes_.size();
    }

    void repair(Handle h) {
        if (h == 0 || nodes_.at(h - 1).leaf) return;
        auto& node = nodes_.at(h - 1);
        repair(node.left);
        repair(node.right);
        node.sum = ar::compose_summary(nodes_.at(node.left - 1).sum,
                                       nodes_.at(node.right - 1).sum);
    }
};

struct HandVector {
    const char* name;
    std::vector<std::uint64_t> operands;
    std::uint64_t expected;
};

// Exact P7 table (E3's existing twelve examples are a different set).
std::vector<HandVector> p7_vectors() {
    return {
        // NaNs, infinities, negatives and signed zeros are all identities.
        {"p7_01_filter", {0x7ff8000000000042ULL, 0x7ff0000000000001ULL,
             0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0xbff0000000000000ULL,
             0x8000000000000001ULL, 0, 0x8000000000000000ULL}, 0},
        // delta + (B-1)delta = Bdelta; +Bdelta = 2Bdelta. Quantum is now
        // 2delta: +delta ties down, +2delta increments, +delta ties up.
        {"p7_02_subnormal_start", {1, 0x000fffffffffffffULL, 0x0010000000000000ULL,
             1, 2, 1}, 0x0020000000000002ULL},
        // q=2^-52 at 1; q/4 is strictly below half.
        {"p7_03_below_half", {0x3ff0000000000000ULL, 0x3c90000000000000ULL},
             0x3ff0000000000000ULL},
        // Each q/2 separately ties down at even k=B.
        {"p7_04_lost_halves", {0x3ff0000000000000ULL, 0x3ca0000000000000ULL,
             0x3ca0000000000000ULL}, 0x3ff0000000000000ULL},
        // Odd k=B+1 plus q/2 chooses even k=B+2.
        {"p7_05_odd_half", {0x3ff0000000000001ULL, 0x3ca0000000000000ULL},
             0x3ff0000000000002ULL},
        // 3q/4 is above half, increment one.
        {"p7_06_above_half", {0x3ff0000000000000ULL, 0x3ca8000000000000ULL},
             0x3ff0000000000001ULL},
        // .3 has odd M=5404319552844595. .1+.2 ties to (M+1)2^-54;
        // adding .3 ties again, choosing even (M+1)2^-53.
        {"p7_07_decimal", {0x3fb999999999999aULL, 0x3fc999999999999aULL,
             0x3fd3333333333333ULL}, 0x3fe3333333333334ULL},
        // 1.5q gives (2,1): even B -> B+2; +q -> B+3; +1.5q -> B+4.
        {"p7_08_reversed_tie", {0x3ff0000000000000ULL, 0x3cb8000000000000ULL,
             0x3cb0000000000000ULL, 0x3cb8000000000000ULL}, 0x3ff0000000000004ULL},
        // (2-q)+q/2 ties to 2; at the new 2q quantum the final q is lost.
        {"p7_09_boundary_tie", {0x3fffffffffffffffULL, 0x3ca0000000000000ULL,
             0x3cb0000000000000ULL}, 0x4000000000000000ULL},
        // 9*2^45 + (2^100-2^47) = 2^100+5*2^45; 5/8 of the NEW
        // 2^48 quantum rounds up. Operand-quantum rounding would tie down.
        {"p7_10_double_rounding", {0x42f2000000000000ULL, 0x462fffffffffffffULL},
             0x4630000000000001ULL},
        // (S-2)+1.5 ties up to S. Each trailing 1 then ties down at even S.
        // At q=1 the first-two summary must retain (S,S-1), not (S,S).
        {"p7_11_sentinel_parity", {0x433ffffffffffffeULL, 0x3ff8000000000000ULL,
             0x3ff0000000000000ULL, 0x3ff0000000000000ULL}, 0x4340000000000000ULL},
        // max=(S-1)2^971; +2^970 is the RN overflow tie. +1 stays +inf;
        // only the final return clamps to positive zero.
        {"p7_12_overflow_absorbing", {0x7fefffffffffffffULL, 0x7c90000000000000ULL,
             0x3ff0000000000000ULL}, 0}
    };
}

std::uint64_t oracle_words(const std::vector<std::uint64_t>& words) {
    std::vector<ce::Row> rows;
    for (std::size_t i = 0; i < words.size(); ++i)
        rows.push_back({i, static_cast<std::uint64_t>(i + 1), ce::from_bits(words[i])});
    return ce::bits(ce::oracle(rows));
}

void check_hand_vector(Checks& c, const HandVector& hand) {
    c.word(oracle_words(hand.operands), hand.expected, "P7 pin disagrees with independent fold");
    const std::array<Shape, 4> shapes{Shape::left_comb, Shape::right_comb,
                                    Shape::balanced, Shape::random_split};
    for (std::size_t n = 0; n <= hand.operands.size(); ++n) {
        const std::vector<std::uint64_t> prefix(hand.operands.begin(), hand.operands.begin() + n);
        const auto expected = oracle_words(prefix);
        for (const auto shape : shapes) {
            TransitionTree tree(prefix, shape);
            c.word(ar::evaluate(tree, tree.root), expected, "tree shape/prefix differs from fold");
        }
    }
    if (hand.name == std::string("p7_11_sentinel_parity")) {
        TransitionTree first_two({hand.operands[0], hand.operands[1]}, Shape::balanced);
        c.pair(ar::get_pair(first_two.root_summary(), 1075),
               {UINT64_C(1) << 53, (UINT64_C(1) << 53) - 1},
               "first-two S-2 block must preserve the unsaturated odd row");
    }
    if (hand.name == std::string("p7_12_overflow_absorbing")) {
        TransitionTree tree(hand.operands, Shape::right_comb);
        c.word(ar::evaluate_raw(tree, tree.root), 0x7ff0000000000000ULL,
               "raw overflow must remain infinity before final clamp");
        c.require(tree.operand_reads == 2, "overflow stops before reading the trailing operand");
    }
}

// Independent reference: round the ENTIRE exact integer numerator of each
// lattice addition, rather than computing a leaf increment or composing
// parity pairs. __int128 holds at most 107 significant bits here.
using Wide = unsigned __int128;
std::uint64_t reference_lattice(const std::vector<std::uint64_t>& operands,
                                std::int32_t region, unsigned incoming_parity) {
    constexpr std::uint64_t limit = UINT64_C(1) << 53;
    Wide current = incoming_parity;
    for (const auto raw : operands) {
        const auto e = static_cast<std::int32_t>((raw >> 52) & 2047);
        const auto fraction = raw & UINT64_C(0x000fffffffffffff);
        if ((raw >> 63) != 0 || e == 2047 || (e == 0 && fraction == 0)) continue;
        const std::uint64_t mantissa =
            fraction | (e == 0 ? UINT64_C(0) : UINT64_C(0x0010000000000000));
        const std::int32_t scale = (e == 0 ? 1 : e) - region;
        if (scale >= 53) return limit; // mantissa >= 1, so this alone adds >=S.
        if (scale >= 0) {
            current += static_cast<Wide>(mantissa) << scale;
        } else if (scale > -54) {
            const auto denominator = static_cast<Wide>(1) << (-scale);
            const auto numerator = current * denominator + mantissa;
            current = numerator / denominator;
            const auto remainder = numerator % denominator;
            if (2 * remainder > denominator
                || (2 * remainder == denominator && (current & 1) != 0))
                ++current;
        } // scale <= -54: a < q/2, so the integer accumulator is unchanged.
        if (current - incoming_parity >= limit) return limit;
    }
    return static_cast<std::uint64_t>(current - incoming_parity);
}

ar::Pair block_pair(const std::vector<std::uint64_t>& operands, std::int32_t g) {
    ar::Pair result{};
    for (const auto raw : operands)
        result = ar::compose_pair(result, ar::leaf_pair(raw, g));
    return result;
}

std::uint64_t random_operand(Rng& rng, std::int32_t g) {
    const auto choice = rng.next();
    std::int32_t e = static_cast<std::int32_t>(rng.next() % 2048);
    // Most blocks exercise live (unsaturated) parity paths near this
    // lattice; the remaining full-range operands exercise saturation.
    if (choice % 4 != 0)
        e = std::max(0, g - static_cast<std::int32_t>(choice % 55));
    auto fraction = rng.next() & UINT64_C(0x000fffffffffffff);
    switch (choice % 7) {
    case 0: fraction = 0; break;
    case 1: fraction = 1; break;
    case 2: fraction = UINT64_C(0x000fffffffffffff); break;
    case 3: fraction = UINT64_C(0x0008000000000000); break;
    default: break;
    }
    const auto sign = choice % 11 == 0 ? UINT64_C(1) << 63 : UINT64_C(0);
    return sign | (static_cast<std::uint64_t>(e) << 52) | fraction;
}

void composition_law_full_regions(Checks& c) {
    Rng rng{UINT64_C(0x434f4d504f5345)};
    std::size_t regions = 0;
    for (std::int32_t g = 1; g <= 2046; ++g) {
        ++regions;
        for (unsigned trial = 0; trial < 6; ++trial) {
            std::vector<std::uint64_t> A, B;
            const auto a_size = static_cast<std::size_t>(rng.next() % 6);
            const auto b_size = static_cast<std::size_t>(rng.next() % 6);
            for (std::size_t i = 0; i < a_size; ++i) A.push_back(random_operand(rng, g));
            for (std::size_t i = 0; i < b_size; ++i) B.push_back(random_operand(rng, g));
            auto AB = A;
            AB.insert(AB.end(), B.begin(), B.end());
            const ar::Pair expected{reference_lattice(AB, g, 0), reference_lattice(AB, g, 1)};
            const auto composed = ar::compose_pair(block_pair(A, g), block_pair(B, g));
            c.pair(composed, expected, "composition differs from exact sequential lattice");
            TransitionTree tree(AB, Shape::random_split, rng.next());
            if (tree.root != 0)
                c.pair(ar::get_pair(tree.root_summary(), g), expected,
                       "54-row summary differs from explicit lattice");
            else
                c.pair(ar::get_pair(ar::Summary{}, g), expected, "empty block is identity");
        }
    }
    c.require(regions == 2046, "composition must cover every finite region");
}

void window_edges_54(Checks& c) {
    const auto leaf = ar::make_leaf(0x3ff0000000000000ULL); // 2^r, r=0, b=1023.
    c.word(leaf.b, 1023, "largest operand supplies b");
    c.pair(ar::get_pair(leaf, 1075), {1, 1}, "b+52 is exactly one quantum");
    c.pair(ar::get_pair(leaf, 1076), {0, 1}, "b+53 retains the odd half-quantum tie");
    c.pair(ar::get_pair(leaf, 1077), {0, 0}, "b+54 is implicit identity");
    const auto block = ar::compose_summary(leaf, ar::make_leaf(1));
    c.pair(ar::get_pair(block, 1075), {1, 1}, "block b+52 keeps an exact quantum");
    c.pair(ar::get_pair(block, 1076), {0, 1}, "block b+53 keeps the selected odd tie");
    c.pair(ar::get_pair(block, 1077), {0, 0}, "block b+54 ignores each tiny operand");
    const auto top = ar::make_leaf(0x7fe0000000000000ULL);
    c.word(top.b, 2046, "top-binade header");
    c.pair(ar::get_pair(top, 2046), {UINT64_C(1) << 52, UINT64_C(1) << 52},
           "top first row is explicit");
    for (std::size_t i = 1; i < top.row.size(); ++i)
        c.pair(top.row[i], {0, 0}, "rows beyond finite region range are zero-filled");
    const auto almost_top = ar::make_leaf(0x7fd0000000000000ULL);
    c.word(almost_top.b, 2045, "penultimate-binade header");
    c.pair(ar::get_pair(almost_top, 2046), {UINT64_C(1) << 51, UINT64_C(1) << 51},
           "penultimate binade retains its second explicit row");
    for (std::size_t i = 2; i < almost_top.row.size(); ++i)
        c.pair(almost_top.row[i], {0, 0}, "penultimate window also zero-fills past 2046");
}

void implicit_lower_and_identity(Checks& c) {
    const auto leaf = ar::make_leaf(0x3ff0000000000000ULL);
    for (std::int32_t g : {1, 511, 1022})
        c.pair(ar::get_pair(leaf, g), {UINT64_C(1) << 53, UINT64_C(1) << 53},
               "regions below b mean saturated, not absent");
    const ar::Summary identity{};
    for (std::int32_t g = 1; g <= 2046; ++g)
        c.pair(ar::get_pair(identity, g), {0, 0}, "b=0 is identity at every region");
    c.word(ar::compose_summary(identity, leaf).b, 1023, "left identity keeps live header");
    c.word(ar::compose_summary(leaf, identity).b, 1023, "right identity keeps live header");
}

void selected_parity_acceptance(Checks& c) {
    // At q=1 the block [.5,1] gives (1,2). Incoming k=S-2 is even:
    // selected d=1 < S-k=2, while max(1,2)=2 would reject needlessly.
    TransitionTree tree({0x3fe0000000000000ULL, 0x3ff0000000000000ULL}, Shape::balanced);
    c.pair(ar::get_pair(tree.root_summary(), 1075), {1, 2}, "selected-parity witness");
    tree.reset_counts();
    c.word(ar::evaluate_raw(tree, tree.root, 0x433ffffffffffffeULL),
           0x433fffffffffffffULL, "accepted block advances to S-1");
    c.require(tree.summary_reads == 1 && tree.operand_reads == 0,
              "selected parity must accept the root without child reads");

    // Equality is a crossing, not an accepted lattice result.
    TransitionTree boundary({0x3ca0000000000000ULL}, Shape::balanced);
    c.word(ar::evaluate_raw(boundary, boundary.root, 0x3fffffffffffffffULL),
           0x4000000000000000ULL, "strict boundary tie must use real addition");
    c.require(boundary.operand_reads == 1, "d == S-k must descend to a real leaf add");
}

void saturation_sentinel(Checks& c) {
    const auto limit = UINT64_C(1) << 53;
    c.pair(ar::compose_pair(ar::Pair{limit, limit}, ar::Pair{1, 1}), {limit, limit},
           "saturation is not an ordinary known increment");
    c.pair(ar::compose_pair(ar::Pair{limit - 2, limit - 2}, ar::Pair{2, 1}),
           {limit, limit - 1}, "saturate independently without losing the other parity");
    c.pair(ar::compose_pair(ar::Pair{1, 1}, ar::Pair{limit, limit}), {limit, limit},
           "second-block sentinel is short-circuited");
    c.pair(ar::compose_pair(ar::Pair{limit - 1, limit - 1}, ar::Pair{1, 1}),
           {limit, limit}, "exact S is represented by the sentinel");
}

void saturation_short_circuit(Checks& c) {
    struct CountingPair {
        mutable std::size_t reads = 0;
        std::uint64_t operator[](std::size_t) const { ++reads; return 1; }
    } following;
    const auto limit = UINT64_C(1) << 53;
    c.pair(ar::compose_pair(ar::Pair{limit, limit}, following), {limit, limit},
           "saturated first block composes to saturation");
    c.require(following.reads == 0, "a==S must not inspect a fabricated outgoing parity");
    c.pair(ar::compose_pair(ar::Pair{limit, 0}, following), {limit, 1},
           "only unsaturated incoming parity reads the second block");
    c.require(following.reads == 1, "only the live parity may inspect Bpair");
}

void compare_summaries(Checks& c, const ar::Summary& A, const ar::Summary& B) {
    c.word(A.b, B.b, "summary maximum must be recomputed");
    for (std::int32_t g = 1; g <= 2046; ++g)
        c.pair(ar::get_pair(A, g), ar::get_pair(B, g), "repaired block differs from rebuilt block");
    c.require(ar::serialize_summary(A) == ar::serialize_summary(B),
              "repaired and rebuilt serialized summaries must match");
}

void maximum_replacement_deletion(Checks& c) {
    const std::vector<std::uint64_t> initial{
        0x5ff0000000000000ULL, 0x3ff0000000000000ULL, 0x3ca0000000000000ULL};
    TransitionTree tree(initial, Shape::balanced);
    c.word(tree.root_summary().b, 1535, "fixture starts with a unique large maximum");
    tree.set_operand(0, 0x3fe0000000000000ULL); // Replace 2^512 with .5.
    TransitionTree replaced({0x3fe0000000000000ULL, initial[1], initial[2]}, Shape::right_comb);
    compare_summaries(c, tree.root_summary(), replaced.root_summary());
    c.word(tree.root_summary().b, 1023, "replacement drops the old maximum header");
    c.word(ar::evaluate(tree, tree.root), oracle_words({0x3fe0000000000000ULL, initial[1], initial[2]}),
           "replacement answer matches independent fold");
    tree.set_operand(1, 0); // Delete the new maximum 1, retaining .5 and 2^-53.
    TransitionTree deleted({0x3fe0000000000000ULL, initial[2]}, Shape::left_comb);
    compare_summaries(c, tree.root_summary(), deleted.root_summary());
    c.word(tree.root_summary().b, 1022, "deleting the maximum drops b again");
    c.word(ar::evaluate(tree, tree.root), oracle_words({0x3fe0000000000000ULL, initial[2]}),
           "deletion answer matches a block that omits the position");
    tree.set_operand(0, 0);
    tree.set_operand(2, 0);
    c.word(tree.root_summary().b, 0, "last deletion restores identity header");
    c.word(ar::evaluate(tree, tree.root), 0, "empty arithmetic block returns +0");
}

void integer_ties_near_half(Checks& c) {
    // With g=54, delta's integer scale is 2^-53. M=B is exactly half;
    // adjacent M=B-1/B+1 differ by one exact integer, however close in FP.
    c.pair(ar::leaf_pair(0x000fffffffffffffULL, 54), {0, 0}, "one integer below half");
    c.pair(ar::leaf_pair(0x0010000000000000ULL, 54), {0, 1}, "exact half");
    c.pair(ar::leaf_pair(0x0010000000000001ULL, 54), {1, 1}, "one integer above half");
    c.pair(ar::leaf_pair(0x3cb8000000000000ULL, 1023), {2, 1},
           "1.5 quanta ties according to result parity");
    c.pair(ar::leaf_pair(1, 1), {1, 1}, "minimum subnormal is an exact lowest-region step");
    c.pair(ar::leaf_pair(1, 2), {0, 1}, "minimum subnormal ties at next region");
    c.pair(ar::leaf_pair(1, 55), {0, 0}, "n>=54 is strictly below half");
    for (std::int32_t g = 1; g <= 2046; ++g) {
        const std::vector<std::uint64_t> probes{
            1, 0x000fffffffffffffULL, 0x0010000000000000ULL, 0x0010000000000001ULL,
            0x3cb8000000000000ULL, 0x7fefffffffffffffULL};
        for (const auto raw : probes)
            c.pair(ar::leaf_pair(raw, g),
                   {reference_lattice({raw}, g, 0), reference_lattice({raw}, g, 1)},
                   "signed exponent/shift branch differs from exact numerator reference");
    }
}

void invalid_interspersed(Checks& c) {
    std::vector<std::uint64_t> invalid{
        0, 0x8000000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0xbff0000000000000ULL, 0x8000000000000001ULL, 0x800fffffffffffffULL,
        0xffefffffffffffffULL};
    // Both signs, every payload-bit position, quiet/signaling encodings,
    // all-one payloads and a deterministic set of mixed payloads.
    for (const auto sign : {UINT64_C(0), UINT64_C(0x8000000000000000)}) {
        for (unsigned bit = 0; bit < 52; ++bit) {
            invalid.push_back(sign | UINT64_C(0x7ff0000000000000) | (UINT64_C(1) << bit));
            invalid.push_back(sign | UINT64_C(0x7ff8000000000000) | (UINT64_C(1) << bit));
        }
        invalid.push_back(sign | UINT64_C(0x7fffffffffffffff));
        Rng rng{UINT64_C(0x4e414e5041594c)};
        for (unsigned i = 0; i < 64; ++i)
            invalid.push_back(sign | UINT64_C(0x7ff0000000000000)
                              | (rng.next() & UINT64_C(0x000fffffffffffff)) | 1);
    }
    const auto identity_bytes = ar::serialize_summary(ar::Summary{});
    std::vector<std::uint64_t> mixed;
    const std::array<std::uint64_t, 3> positive{
        0x3ff0000000000000ULL, 0x3ca0000000000000ULL, 1};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        const auto raw = invalid[i];
        c.require(!ar::eligible(raw), "invalid operand must fail bit predicate");
        c.word(ar::operand_base(raw), 0, "invalid operand has identity base");
        const auto summary = ar::make_leaf(raw);
        c.require(ar::serialize_summary(summary) == identity_bytes,
                  "invalid operand has canonical identity bytes");
        for (std::int32_t g : {1, 2, 1023, 2046})
            c.pair(ar::leaf_pair(raw, g), {0, 0}, "invalid leaf pair is identity");
        mixed.push_back(raw);
        mixed.push_back(positive[i % positive.size()]);
    }
    for (const auto shape : {Shape::balanced, Shape::random_split}) {
        TransitionTree tree(mixed, shape);
        c.word(ar::evaluate(tree, tree.root), oracle_words(mixed),
               "interspersed invalid rows must not disturb the fold");
    }
}

void empty_and_accumulator_codec(Checks& c) {
    TransitionTree empty({}, Shape::balanced);
    c.word(ar::evaluate(empty, empty.root), 0, "empty input returns +0");
    c.require(empty.summary_reads == 0 && empty.operand_reads == 0, "empty input reads no nodes");
    for (std::int32_t g = 1; g <= 2046; ++g) {
        for (const auto f : {UINT64_C(0), UINT64_C(1), UINT64_C(0x000ffffffffffffe),
                             UINT64_C(0x000fffffffffffff)}) {
            const auto raw = (static_cast<std::uint64_t>(g) << 52) | f;
            const auto decoded = ar::decode_accumulator(raw);
            c.word(static_cast<std::uint64_t>(decoded.g), static_cast<std::uint64_t>(g),
                   "decode retains biased region");
            c.word(decoded.k, (UINT64_C(1) << 52) + f, "decode retains exact significand");
            c.word(ar::encode_accumulator(decoded.g, decoded.k), raw, "normal codec round trip");
        }
    }
    for (const auto raw : {UINT64_C(0), UINT64_C(1), UINT64_C(0x000fffffffffffff)}) {
        const auto decoded = ar::decode_accumulator(raw);
        c.word(static_cast<std::uint64_t>(decoded.g), 1, "zero/subnormals share lowest region");
        c.word(ar::encode_accumulator(decoded.g, decoded.k), raw, "subnormal codec round trip");
    }
    c.refuses([] { (void)ar::decode_accumulator(0x8000000000000000ULL); },
              "negative zero is not a canonical accumulator");
    c.refuses([] { (void)ar::decode_accumulator(0x7ff0000000000000ULL); },
              "infinity is handled before accumulator decode");
    c.refuses([] { (void)ar::encode_accumulator(2, 1); }, "normal region needs a hidden bit");
    c.refuses([] { (void)ar::encode_accumulator(1, UINT64_C(1) << 53); },
              "S belongs to the next region, never the codec's current region");
}

void random_crossing_jumps(Checks& c) {
    Rng rng{UINT64_C(0x43524f5353494e47)};
    std::size_t jumps = 0;
    for (std::size_t i = 0; i < 512; ++i) {
        const auto low_e = static_cast<std::uint64_t>(1 + rng.next() % 1800);
        const auto high_e = low_e + 1 + rng.next() % (2046 - low_e);
        const auto a_fraction = rng.next() & UINT64_C(0x000fffffffffffff);
        const auto b_fraction = rng.next() & UINT64_C(0x000fffffffffffff);
        const std::vector<std::uint64_t> words{
            (low_e << 52) | a_fraction, (high_e << 52) | b_fraction,
            0x3ca0000000000000ULL, (low_e << 52) | a_fraction};
        ++jumps;
        const auto expected = oracle_words(words);
        for (const auto shape : {Shape::left_comb, Shape::right_comb,
                                  Shape::balanced, Shape::random_split}) {
            TransitionTree tree(words, shape, rng.next());
            c.word(ar::evaluate(tree, tree.root), expected, "random real crossing differs from fold");
        }
    }
    c.require(jumps == 512, "random crossing corpus actually ran");
}

void resume_after_crossings(Checks& c) {
    const std::vector<std::uint64_t> operands{
        1, 0x0010000000000000ULL, 0x3ff0000000000000ULL, 0x3ca0000000000000ULL,
        0x42f2000000000000ULL, 0x462fffffffffffffULL, 1, 0x4c70000000000000ULL,
        1, 0x58f0000000000000ULL};
    const auto expected = oracle_words(operands);
    for (const auto shape : {Shape::left_comb, Shape::right_comb,
                              Shape::balanced, Shape::random_split}) {
        TransitionTree tree(operands, shape);
        c.word(ar::evaluate(tree, tree.root), expected, "resume uses the updated accumulator");
        c.require(tree.operand_reads >= 4, "witness must contain multiple real region crossings");
        c.require(tree.summary_reads <= 2 * operands.size() - 1, "evaluation never restarts the tree");
        auto visited = tree.summary_trace;
        std::sort(visited.begin(), visited.end());
        c.require(std::adjacent_find(visited.begin(), visited.end()) == visited.end(),
                  "no summary is rescanned after a real crossing addition");
    }
}

class Candidate final : public ce::ExposureCandidate {
public:
    std::size_t queries = 0, inserts = 0, sets = 0, erases = 0, clears = 0;

    void insert(std::size_t position, std::uint64_t incarnation, double value) override {
        ++inserts;
        for (auto& row : rows_) if (row.incarnation == incarnation) row.raw = ce::bits(value);
        rows_.push_back({position, incarnation, ce::bits(value)});
    }
    void set(std::uint64_t incarnation, double value) override {
        ++sets;
        for (auto& row : rows_) if (row.incarnation == incarnation) row.raw = ce::bits(value);
    }
    void erase(std::uint64_t incarnation) override {
        ++erases;
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                                  [incarnation](const Row& row) {
                                      return row.incarnation == incarnation;
                                  }), rows_.end());
    }
    void clear() override { ++clears; rows_.clear(); }
    double exposure() override {
        ++queries;
        auto selected = rows_;
        // Independent selection: (position, incarnation), dedup BEFORE
        // arithmetic filtering. The oracle instead repeatedly scans rows.
        std::sort(selected.begin(), selected.end(), [](const Row& a, const Row& b) {
            return a.position < b.position
                || (a.position == b.position && a.incarnation < b.incarnation);
        });
        std::vector<std::uint64_t> ordered;
        bool have_position = false;
        std::size_t previous = 0;
        for (const auto& row : selected) {
            if (have_position && row.position == previous) continue;
            ordered.push_back(row.raw); // Invalid winners remain identity leaves.
            previous = row.position;
            have_position = true;
        }
        TransitionTree tree(ordered, Shape::random_split, rng_.next());
        return ce::from_bits(ar::evaluate(tree, tree.root));
    }

private:
    struct Row {
        std::size_t position;
        std::uint64_t incarnation, raw;
    };
    std::vector<Row> rows_;
    Rng rng_{UINT64_C(0x453343414e444944)};
};

void whole_e3_corpus(Checks& c) {
    const auto corpus = ce::prepare(ce::adversarial_corpus());
    c.require(!corpus.empty(), "whole E3 corpus is present");
    std::size_t expected_comparisons = 0;
    for (const auto& row : corpus) {
        c.require(!row.checkpoints.empty(), "E3 named row has real mutation checkpoints");
        expected_comparisons += 1 + row.checkpoints.size();
    }
    Candidate candidate;
    const auto verdict = ce::exercise_candidate(candidate, corpus);
    for (const auto& mismatch : verdict.rejected_cases)
        std::fprintf(stderr, "case=whole_e3_corpus E3=%s step=%zu expected=%016llx actual=%016llx\n",
                     mismatch.case_name.c_str(), mismatch.step,
                     static_cast<unsigned long long>(mismatch.expected),
                     static_cast<unsigned long long>(mismatch.actual));
    c.require(verdict.comparisons > 0, "zero-checkpoint corpus is never a pass");
    c.require(verdict.comparisons == expected_comparisons, "every E3 checkpoint was evaluated");
    c.require(candidate.queries == expected_comparisons, "candidate query was never skipped");
    c.require(candidate.inserts > 0 && candidate.sets > 0 && candidate.erases > 0
                  && candidate.clears > 0, "all four candidate edit families ran");
    c.require(verdict.mismatches == 0 && verdict.rejected_cases.empty(),
              "transition candidate must accept the WHOLE E3 corpus");
    std::printf("E3 cases=%zu comparisons=%zu mismatches=%zu\n",
                corpus.size(), verdict.comparisons, verdict.mismatches);
}

void fp_default_canary(Checks& c) {
    c.require(std::fegetround() == FE_TONEAREST, "default test environment must be RN-even");
    c.require(ar::exposure_fp_environment_ok(), "default environment must pass canary");
    volatile double even = 1.0, odd = 0x1.0000000000001p0, half = 0x1p-53;
    c.word(ar::real_binary64_add(ce::bits(even), ce::bits(half)), 0x3ff0000000000000ULL,
           "separate non-inlined real-add probe: even half-ulp");
    c.word(ar::real_binary64_add(ce::bits(odd), ce::bits(half)), 0x3ff0000000000002ULL,
           "separate non-inlined real-add probe: odd half-ulp");
}

void fp_upward_canary(Checks& c) {
    const int saved = std::fegetround();
    c.require(saved != -1, "rounding mode must be readable");
    struct Restore {
        int mode;
        ~Restore() { (void)std::fesetround(mode); }
    } restore{saved}; // Covers every throwing path as well as normal return.
    c.require(std::fesetround(FE_UPWARD) == 0, "FE_UPWARD control must actually be installed");
    const bool accepted = ar::exposure_fp_environment_ok();
    const int restored = std::fesetround(saved);
    c.require(restored == 0 && std::fegetround() == saved, "restore mode before judging canary");
    c.require(!accepted, "canary must reject upward rounding");
}

// Unevaluated contract checks keep this test independent of the runtime
// archive. The real typed helper is exercised by its run-begin caller in r2.
static_assert(std::is_same<decltype(ar::exposure_fp_environment_refuse()), void>::value,
              "environment refusal has the specified void signature");
static_assert(!noexcept(ar::exposure_fp_environment_refuse()), "environment refusal throws");
static_assert(std::is_base_of<pineforge::RunFailureInfo,
                             pineforge::coded<std::logic_error>>::value,
              "logic_error refusal also carries RunFailureInfo");

void summary_byte_format(Checks& c) {
    ar::Summary input{};
    input.b = 0x123;
    for (std::size_t i = 0; i < input.row.size(); ++i)
        input.row[i] = {static_cast<std::uint64_t>(3 * i),
                        static_cast<std::uint64_t>(3 * i + 1)};
    input.row[0][0] = UINT64_C(0x0001020304050607);
    input.row[0][1] = UINT64_C(1) << 53;
    input.row[53][1] = (UINT64_C(1) << 53) - 1;
    const auto bytes = ar::serialize_summary(input);
    c.require(sizeof(ar::Summary) == 872 && bytes.size() == 872, "exact fixed summary length");
    const std::array<std::uint8_t, 8> header{0x23, 0x01, 0, 0, 0, 0, 0, 0};
    const std::array<std::uint8_t, 8> first{7, 6, 5, 4, 3, 2, 1, 0};
    const std::array<std::uint8_t, 8> sentinel{0, 0, 0, 0, 0, 0, 0x20, 0};
    for (std::size_t i = 0; i < 8; ++i) {
        c.word(bytes[i], header[i], "LE header bytes");
        c.word(bytes[8 + i], first[i], "LE increment bytes");
        c.word(bytes[16 + i], sentinel[i], "LE S sentinel bytes");
    }
    const auto parsed = ar::parse_summary(bytes.data(), bytes.size());
    c.word(parsed.b, input.b, "parse header");
    for (std::size_t i = 0; i < input.row.size(); ++i)
        c.pair(parsed.row[i], input.row[i], "parse every field");
    c.require(ar::serialize_summary(parsed) == bytes, "parse/serialize canonical round trip");

    // Every one of the 108 increment fields is independently corrupted.
    for (std::size_t field = 0; field < 108; ++field) {
        auto bad = bytes;
        const auto offset = 8 + 8 * field;
        for (std::size_t j = 0; j < 8; ++j) bad[offset + j] = 0;
        bad[offset] = 1;
        bad[offset + 6] = 0x20; // S+1, written independently of the production codec.
        c.refuses([&] { (void)ar::parse_summary(bad.data(), bad.size()); },
                  "parse must reject above-S value in every field");
        auto bad_summary = input;
        bad_summary.row[field / 2][field % 2] = (UINT64_C(1) << 53) + 1;
        c.refuses([&] { (void)ar::serialize_summary(bad_summary); },
                  "serialization must range-check every increment");
    }
    auto bad_header = bytes;
    bad_header[0] = 0xff;
    bad_header[1] = 0x07; // 2047.
    c.refuses([&] { (void)ar::parse_summary(bad_header.data(), bad_header.size()); },
              "header above 2046 must be rejected");
    auto bad_summary = input;
    bad_summary.b = 2047;
    c.refuses([&] { (void)ar::serialize_summary(bad_summary); }, "serialize rejects bad header");
    for (const std::size_t length : {std::size_t{0}, std::size_t{871}, std::size_t{873}})
        c.refuses([&] { (void)ar::parse_summary(bytes.data(), length); },
                  "length other than 872 must be rejected before any reads");
    c.refuses([] { (void)ar::parse_summary(nullptr, 872); }, "null input must be rejected");

    ar::Summary identity{};
    const auto identity_bytes = ar::serialize_summary(identity);
    for (const auto byte : identity_bytes) c.word(byte, 0, "identity encoding is all zero");
    identity.row[0][0] = 1;
    c.refuses([&] { (void)ar::serialize_summary(identity); }, "identity rows must be zero-filled");
    auto top = ar::make_leaf(0x7fe0000000000000ULL);
    const auto top_bytes = ar::serialize_summary(top);
    c.word(ar::parse_summary(top_bytes.data(), top_bytes.size()).b, 2046,
           "maximum legal header round trip");
    top.row[1][0] = 1;
    c.refuses([&] { (void)ar::serialize_summary(top); }, "rows above 2046 must be zero");
    c.refuses([&] { (void)ar::get_pair(input, 0); }, "lookup rejects region zero");
    c.refuses([&] { (void)ar::get_pair(input, 2047); }, "lookup rejects non-finite region");
}

using Test = std::pair<std::string, std::function<void(Checks&)>>;
std::vector<Test> named_cases() {
    std::vector<Test> cases;
    for (const auto& hand : p7_vectors())
        cases.emplace_back(hand.name, [hand](Checks& c) { check_hand_vector(c, hand); });
    cases.emplace_back("composition_law_full_regions", composition_law_full_regions);
    cases.emplace_back("window_edges_54", window_edges_54);
    cases.emplace_back("implicit_lower_and_identity", implicit_lower_and_identity);
    cases.emplace_back("selected_parity_acceptance", selected_parity_acceptance);
    cases.emplace_back("saturation_sentinel", saturation_sentinel);
    cases.emplace_back("saturation_short_circuit", saturation_short_circuit);
    cases.emplace_back("maximum_replacement_deletion", maximum_replacement_deletion);
    cases.emplace_back("integer_ties_near_half", integer_ties_near_half);
    cases.emplace_back("invalid_interspersed", invalid_interspersed);
    cases.emplace_back("empty_and_accumulator_codec", empty_and_accumulator_codec);
    cases.emplace_back("random_crossing_jumps", random_crossing_jumps);
    cases.emplace_back("resume_after_crossings", resume_after_crossings);
    cases.emplace_back("whole_e3_corpus", whole_e3_corpus);
    cases.emplace_back("fp_default_canary", fp_default_canary);
    cases.emplace_back("fp_upward_canary", fp_upward_canary);
    cases.emplace_back("summary_byte_format", summary_byte_format);
    return cases;
}

} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc == 2 ? argv[1] : "";
    if (argc > 2) {
        std::fprintf(stderr, "usage: test_cohort_exposure_arith [named-case]\n");
        return 1;
    }
    std::size_t cases_run = 0, checkpoints = 0;
    for (const auto& test : named_cases()) {
        if (!selected.empty() && selected != test.first) continue;
        Checks checks{test.first};
        try {
            test.second(checks);
            checks.require(checks.checkpoints != 0, "named case must execute real checkpoints");
            ++cases_run;
            checkpoints += checks.checkpoints;
            std::printf("case=%s checkpoints=%zu PASS\n", test.first.c_str(), checks.checkpoints);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "case=%s checkpoints=%zu FAIL: %s\n",
                         test.first.c_str(), checks.checkpoints, error.what());
            return 1; // A failed control stops this execution.
        }
    }
    if (cases_run == 0 || checkpoints == 0) {
        std::fprintf(stderr, "FAIL: no matching named case/checkpoints (%s)\n", selected.c_str());
        return 1;
    }
    std::printf("cohort exposure transitions: cases=%zu checkpoints=%zu PASS\n",
                cases_run, checkpoints);
    return 0;
}
