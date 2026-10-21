// Frozen 6cfcff0696: scratch_facts.hpp does not exist. The lab must record
// that missing-header first diagnostic before compiling the implementation.
#include <pineforge/detail/scratch_facts.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::detail;
using Inputs = ScratchVocabularyFacts::Inputs;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Value>
Value take(ScratchFactsResult<Value> result) {
    require(result.error == ScratchFactsError::none && result.value.has_value(), "expected complete success value");
    return std::move(*result.value);
}

template<class Value>
void reject(const ScratchFactsResult<Value>& result, ScratchFactsError error) {
    require(!result && result.error == error && !result.value, "typed failure must expose no partial value");
}

Inputs empty_inputs() {
    Inputs inputs;
    for (auto& set : inputs) set.complete = true;
    return inputs;
}

// Independent reference: materialize the actual single and tagged pair key
// lists, deduplicate by identity, then count rows/bytes by visiting those keys.
// In particular, (single,X) and (pair,X,empty) are different tuple values.
ScratchManifest enumerate_domain(const Inputs& input) {
    using Key = std::tuple<bool, std::string, std::string>;
    std::vector<Key> keys;
    for (const auto& source : input[0].strings) {
        keys.emplace_back(false, source, std::string{});
        for (const auto& from : input[1].strings) keys.emplace_back(true, source, from);
    }
    ScratchManifest expected{};
    const std::set<Key> erased(keys.begin(), keys.end());
    for (const auto& key : erased) {
        ++expected.K_e;
        expected.B_e += std::get<1>(key).size();
        expected.B_e += std::get<2>(key).size();
    }
    std::set<std::string> joined;
    for (const auto& exit : input[2].strings) {
        for (const auto& from : input[1].strings) {
            std::string key = exit;
            key += static_cast<char>(31);
            key += from;
            joined.insert(key);
        }
    }
    for (const auto& key : joined) {
        ++expected.K_c;
        expected.B_c += key.size();
    }
    const std::set<std::string> cohorts(input[3].strings.begin(), input[3].strings.end());
    for (const auto& key : cohorts) {
        ++expected.U;
        expected.B_U += key.size();
    }
    const std::set<std::string> eligible(input[4].strings.begin(), input[4].strings.end());
    std::vector<std::string> by_length(eligible.begin(), eligible.end());
    std::stable_sort(by_length.begin(), by_length.end(),
        [](const auto& left, const auto& right) { return left.size() > right.size(); });
    for (const auto& key : by_length) {
        if (expected.V == 27) break; // The specified rank population limit.
        ++expected.V;
        expected.B_V += key.size();
    }
    return expected;
}

void same_manifest(const ScratchManifest& actual, const ScratchManifest& expected) {
    require(actual.K_q == expected.K_q && actual.B_q == expected.B_q, "q remains empty");
    require(actual.K_e == expected.K_e && actual.B_e == expected.B_e, "e matches enumerated single/pair domain");
    require(actual.K_c == expected.K_c && actual.B_c == expected.B_c, "c matches distinct joined byte strings");
    require(actual.U == expected.U && actual.B_U == expected.B_U, "cohorts match enumerated domain");
    require(actual.V == expected.V && actual.B_V == expected.B_V, "rank covers largest allowed distinct keys");
    require(actual.R == expected.R, "legacy R is never assigned a live capacity");
}

std::string fixture_wire(const Inputs& ordered);

// Rosters in the documented canonical order, taken from std::set rather than
// from the production comparator (std::string orders its bytes as unsigned).
Inputs canonical_rosters(const Inputs& input) {
    Inputs ordered = input;
    for (auto& set : ordered) {
        const std::set<std::string> unique(set.strings.begin(), set.strings.end());
        set.strings.assign(unique.begin(), unique.end());
    }
    return ordered;
}

void check_domain(const Inputs& input) {
    const auto facts = take(ScratchVocabularyFacts::canonicalize(input));
    const auto expected = enumerate_domain(input);
    same_manifest(take(derive_scratch_manifest(facts)), expected);
    // The same certificate crosses the wire: canonical bytes from the
    // independent producer, and the decoded copy derives the same capacities.
    const auto wire = take(encode_scratch_facts(facts));
    require(wire == fixture_wire(canonical_rosters(input)), "canonical wire matches the independent producer");
    same_manifest(take(derive_scratch_manifest(take(decode_scratch_facts(wire)))), expected);
}

void finite_domains() {
    check_domain(empty_inputs());
    auto empty_string = empty_inputs();
    for (auto& set : empty_string) set.strings = {"", ""};
    check_domain(empty_string);
    auto singles_only = empty_inputs();
    singles_only[0].strings = {"X", "X", "longer"};
    check_domain(singles_only);
    singles_only[1].strings = {"", ""};
    check_domain(singles_only);

    const std::string separator(1, static_cast<char>(31));
    auto alias = empty_inputs();
    alias[0].strings = {"", "X", "X", std::string("a\0b", 3)};
    alias[1].strings = {"", "b" + separator + "c", "c", "c"};
    alias[2].strings = {"a", "a" + separator + "b", "a"};
    alias[3].strings = {"", "cohort", "cohort", std::string("a\0b", 3)};
    alias[4].strings = alias[0].strings;
    // These two different component pairs MUST occupy one c key.
    require(std::string("a") + separator + alias[1].strings[1]
            == alias[2].strings[1] + separator + "c", "fixture contains a real joined-key alias");
    const std::set<std::string> distinct_exits(alias[2].strings.begin(), alias[2].strings.end());
    const std::set<std::string> distinct_froms(alias[1].strings.begin(), alias[1].strings.end());
    require(enumerate_domain(alias).K_c + 1 == distinct_exits.size() * distinct_froms.size(),
            "the alias fixture merges exactly one pair of joined keys");
    check_domain(alias);

    // Exhaust small rosters independently in all five roles, with duplicates,
    // empty components, NUL bytes and delimiter-containing components.
    const std::array<std::vector<std::string>, 4> rosters{{
        {}, {""}, {"z", "a", "z"}, {"", separator, std::string("n\0", 2)}
    }};
    for (const auto& s : rosters) for (const auto& f : rosters)
        for (const auto& e : rosters) for (const auto& h : rosters)
            for (const auto& j : rosters) {
                auto input = empty_inputs();
                input[0].strings = s;
                input[1].strings = f;
                input[2].strings = e;
                input[3].strings = h;
                input[4].strings = j;
                check_domain(input);
            }

    auto clipped = alias;
    for (std::size_t length = 0; length < 40; ++length) {
        clipped[4].strings.emplace_back(length, 'x');
        clipped[4].strings.emplace_back(length, 'x');
    }
    check_domain(clipped);
    // Equal long lengths and a reversed producer order.
    clipped[4].strings.push_back(std::string(100, 'a'));
    clipped[4].strings.push_back(std::string(100, 'b'));
    std::reverse(clipped[4].strings.begin(), clipped[4].strings.end());
    check_domain(clipped);
    auto tied = empty_inputs();
    for (unsigned i = 0; i < 40; ++i) tied[4].strings.emplace_back(4, static_cast<char>(i + 32));
    check_domain(tied);
}

// The derived value is the existing ScratchManifest, so the existing layout
// function is its consumer. It must accept the value (rank clipped to its
// limit of 27 keys) and give q and the legacy all-successors region no bytes.
void layout_accepts_derived_manifest() {
    auto input = empty_inputs();
    input[0].strings = {"", "X", "a"};
    input[1].strings = {"", "from"};
    input[2].strings = {"exit", "exit2"};
    input[3].strings = {"cohort"};
    for (std::size_t length = 0; length < 40; ++length) input[4].strings.emplace_back(length, 'x');
    const auto facts = take(ScratchVocabularyFacts::canonicalize(input));
    const auto manifest = take(derive_scratch_manifest(facts));
    same_manifest(manifest, enumerate_domain(input));
    const auto layout = compute_layout(manifest);
    require(layout.regions[0].capacity == 0 && layout.regions[1].capacity == 0, "q regions have no capacity");
    require(layout.regions[8].capacity == 0, "the legacy all-successors region has no capacity");
    // Control: the consumer does refuse an unclipped rank count, so acceptance
    // of the derived value above is a real check and the clip is load-bearing.
    auto unclipped = manifest;
    unclipped.V = input[4].strings.size();
    bool refused = false;
    try {
        (void)compute_layout(unclipped);
    } catch (const ScratchCapacityError&) {
        refused = true;
    }
    require(refused, "the layout contract rejects more than 27 rank keys");
}

// A separate producer fixture: division/remainder emits documented LE words,
// without calling any production serialization or canonicalization helper.
void fixture_word(std::string& wire, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        wire.push_back(static_cast<char>(static_cast<unsigned char>(value % 256)));
        value /= 256;
    }
}

std::string fixture_wire(const Inputs& ordered) {
    std::string wire(1, '\1');
    for (const auto& set : ordered) {
        wire.push_back(set.complete ? '\1' : '\0');
        fixture_word(wire, static_cast<std::uint64_t>(set.strings.size()));
        for (const auto& key : set.strings) {
            fixture_word(wire, static_cast<std::uint64_t>(key.size()));
            wire += key;
        }
    }
    return wire;
}

void serialization_and_ownership() {
    static_assert(std::is_same<decltype(std::declval<const ScratchVocabularyFacts&>().source_ids().strings()),
                               const std::vector<std::string>&>::value, "certificate exposes const bytes only");
    static_assert(noexcept(decode_scratch_facts(std::string_view{})), "hostile decoder cannot throw");
    static_assert(noexcept(encode_scratch_facts(std::declval<const ScratchVocabularyFacts&>())), "encoder returns errors");
    static_assert(!std::is_copy_assignable<ScratchVocabularyFacts>::value
                  && !std::is_move_assignable<ScratchVocabularyFacts>::value, "certificate is immutable after construction");
    auto ordered = empty_inputs();
    ordered[0].strings = {"", std::string("\0s", 2), "ascii", std::string(1, '\x7f'),
                          std::string(1, '\x80'), std::string(1, '\xff')};
    ordered[1].strings = {"", "from"};
    ordered[2].strings = {std::string(258, 'e')}; // Nontrivial LE length word.
    ordered[3].strings = {"", std::string(1, static_cast<char>(31)), "cohort"};
    ordered[4].strings = {"entry"};
    ordered[3].complete = false;
    const auto expected_wire = fixture_wire(ordered);
    const auto decoded = take(decode_scratch_facts(expected_wire));
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        require(decoded.sets()[i].strings() == ordered[i].strings, "documented roster order and binary bytes");
        require(decoded.sets()[i].complete() == ordered[i].complete, "all completeness flags survive decode");
    }
    require(!decoded.complete(), "one unproved set makes the whole certificate incomplete");
    require(take(encode_scratch_facts(decoded)) == expected_wire, "independent wire fixture round trip");

    auto shuffled = ordered;
    for (auto& set : shuffled) {
        std::reverse(set.strings.begin(), set.strings.end());
        if (!set.strings.empty()) set.strings.push_back(set.strings.front());
    }
    const auto canonical = take(ScratchVocabularyFacts::canonicalize(shuffled));
    require(take(encode_scratch_facts(canonical)) == expected_wire, "canonical encoding ignores input order/duplicates");
    shuffled[0].strings.clear();
    shuffled[1].strings[0] = "mutated producer buffer";
    require(take(encode_scratch_facts(canonical)) == expected_wire, "certificate owns immutable producer bytes");
    auto transient_wire = expected_wire;
    const auto owned_decode = take(decode_scratch_facts(transient_wire));
    transient_wire.assign(transient_wire.size(), 'x');
    require(take(encode_scratch_facts(owned_decode)) == expected_wire, "decoder owns bytes beyond wire lifetime");
    auto move_inputs = ordered;
    for (auto& set : move_inputs) set.complete = true;
    auto move_source = take(ScratchVocabularyFacts::canonicalize(move_inputs));
    require(move_source.complete(), "move fixture starts with a complete proof");
    const auto transferred = std::move(move_source);
    require(transferred.complete() && !move_source.complete(), "moved-from certificate fails closed");
    require(take(encode_scratch_facts(transferred)) == fixture_wire(move_inputs), "move preserves every certified byte");
    reject(derive_scratch_manifest(move_source), ScratchFactsError::incomplete_vocabulary);

    const auto empty_wire = fixture_wire(empty_inputs());
    require(take(encode_scratch_facts(take(decode_scratch_facts(empty_wire)))) == empty_wire,
            "complete empty rosters have a canonical wire");
    auto with_empty = empty_inputs();
    with_empty[0].strings = {""};
    const auto empty_certificate = take(decode_scratch_facts(empty_wire));
    const auto with_empty_certificate = take(ScratchVocabularyFacts::canonicalize(with_empty));
    require(empty_certificate.source_ids().strings().empty()
            && with_empty_certificate.source_ids().strings().size() == 1,
            "empty set differs from set containing empty string");
    require(take(encode_scratch_facts(with_empty_certificate)) == fixture_wire(with_empty),
            "a set holding the empty string has its own canonical wire");
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        auto flags = empty_inputs();
        flags[i].complete = false;
        const auto facts = take(decode_scratch_facts(fixture_wire(flags)));
        require(!facts.complete() && !facts.sets()[i].complete(), "incomplete flag is legal wire data");
    }
}

// A vector typed by hand from the documented format, for producers in other
// languages: it uses neither fixture_wire nor any production helper.
void golden_wire_vector() {
    const unsigned char bytes[] = {
        1,                                                   // version
        1, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 'a', 'b',  // S = {"ab"}
        1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // F = {""}
        1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 'x',      // E = {"x"}
        1, 0, 0, 0, 0, 0, 0, 0, 0,                                   // H = {}
        1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 'q',      // J = {"q"}
    };
    const std::string golden(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    auto input = empty_inputs();
    input[0].strings = {"ab"};
    input[1].strings = {""};
    input[2].strings = {"x"};
    input[4].strings = {"q"};
    const auto facts = take(ScratchVocabularyFacts::canonicalize(input));
    require(take(encode_scratch_facts(facts)) == golden, "encoder writes the hand-typed bytes");
    const auto decoded = take(decode_scratch_facts(golden));
    for (std::size_t i = 0; i < input.size(); ++i) {
        require(decoded.sets()[i].strings() == input[i].strings, "decoder reads the hand-typed bytes");
        require(decoded.sets()[i].complete(), "every flag byte of the vector is one");
    }
}

void hostile_wire() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto empty_wire = fixture_wire(empty_inputs());
    reject(decode_scratch_facts({}), ScratchFactsError::truncated);
    for (const auto version : {0, 2, 255}) {
        auto wire = empty_wire;
        wire[0] = static_cast<char>(version);
        reject(decode_scratch_facts(wire), ScratchFactsError::unsupported_version);
    }
    for (std::size_t set = 0; set < 5; ++set) {
        const auto flag = 1 + 9 * set; // Independent fixture offsets for empty rosters.
        reject(decode_scratch_facts(std::string_view(empty_wire).substr(0, flag)), ScratchFactsError::truncated);
        for (std::size_t count_bytes = 0; count_bytes < 8; ++count_bytes)
            reject(decode_scratch_facts(std::string_view(empty_wire).substr(0, flag + 1 + count_bytes)),
                   ScratchFactsError::truncated);
        for (const auto invalid_flag : {2, 255}) {
            auto wire = empty_wire;
            wire[flag] = static_cast<char>(invalid_flag);
            reject(decode_scratch_facts(wire), ScratchFactsError::invalid_complete_flag);
        }
        auto duplicates = empty_inputs();
        duplicates[set].strings = {"same", "same"};
        reject(decode_scratch_facts(fixture_wire(duplicates)), ScratchFactsError::duplicate_entry);
        auto unsorted = empty_inputs();
        unsorted[set].strings = {"later", "earlier"};
        reject(decode_scratch_facts(fixture_wire(unsorted)), ScratchFactsError::unsorted_entry);
    }
    auto unsigned_unsorted = empty_inputs();
    unsigned_unsorted[0].strings = {std::string(1, '\x80'), std::string(1, '\x7f')};
    reject(decode_scratch_facts(fixture_wire(unsigned_unsorted)), ScratchFactsError::unsorted_entry);
    for (const auto count : {maximum, maximum / 8, std::uint64_t{1}}) {
        std::string wire("\1\1", 2);
        fixture_word(wire, count);
        reject(decode_scratch_facts(wire), count == maximum ? ScratchFactsError::arithmetic_overflow
                                                          : ScratchFactsError::invalid_length);
    }
    for (const auto length : {maximum, std::uint64_t{4}}) {
        std::string wire("\1\1", 2);
        fixture_word(wire, 1);
        fixture_word(wire, length);
        wire += "abc";
        reject(decode_scratch_facts(wire), ScratchFactsError::invalid_length);
    }
    // The count's minimum length words fit, but the first string consumes
    // them all: specifically exercise a truncated entry-length word.
    std::string missing_length("\1\1", 2);
    fixture_word(missing_length, 2);
    fixture_word(missing_length, 8);
    missing_length += "abcdefgh";
    reject(decode_scratch_facts(missing_length), ScratchFactsError::truncated);
    reject(decode_scratch_facts(empty_wire + std::string(1, '\0')), ScratchFactsError::trailing_bytes);
    reject(decode_scratch_facts(empty_wire + "trailer"), ScratchFactsError::trailing_bytes);

    auto valid = empty_inputs();
    for (auto& set : valid) set.strings = {"", std::string("a\0", 2), "longer"};
    const auto wire = fixture_wire(valid);
    for (std::size_t cut = 0; cut < wire.size(); ++cut) {
        const auto result = decode_scratch_facts(std::string_view(wire).substr(0, cut));
        require(!result && result.error != ScratchFactsError::none && !result.value,
                "every truncated certificate is rejected without partial facts");
    }
}

void arithmetic_boundaries() {
    namespace math = pineforge::detail::scratch_facts_detail;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto output = maximum;
    require(!math::add(maximum, 1, output) && output == maximum, "addition never publishes wrapped output");
    require(math::add(maximum, 0, output) && output == maximum, "maximum addition boundary");
    require(!math::multiply(maximum, 2, output) && output == maximum, "multiplication checks large left operand");
    require(!math::multiply(2, maximum, output) && output == maximum, "multiplication checks large right operand");
    require(math::multiply(0, maximum, output) && output == 0, "zero multiplication is representable");
    require(math::multiply(maximum, 1, output) && output == maximum, "maximum multiplication boundary");
    // Exercise every checked e intermediate without allocating fictitious
    // exabyte strings. These are scalar boundary tests, not a capacity oracle.
    reject(math::erased_capacity(1, maximum, 0, 0), ScratchFactsError::arithmetic_overflow);
    reject(math::erased_capacity(maximum, 1, 0, 0), ScratchFactsError::arithmetic_overflow);
    reject(math::erased_capacity(1, 1, maximum, 0), ScratchFactsError::arithmetic_overflow);
    reject(math::erased_capacity(2, 1, 1, maximum), ScratchFactsError::arithmetic_overflow);
    reject(math::erased_capacity(1, 1, maximum / 2, 2), ScratchFactsError::arithmetic_overflow);
    require(take(math::erased_capacity(1, 0, maximum, 0)).bytes == maximum, "e byte maximum is accepted without wrapping");
    require(take(math::erased_capacity(1, 1, maximum / 2, 1)).bytes == maximum, "e final sum maximum is accepted");
}

LiveBounds witness_bounds() { return LiveBounds{2, 3, 5, 7, 11}; }

void backend_decisions() {
    using BoundMember = std::optional<std::uint64_t> LiveBounds::*;
    const std::array<BoundMember, 5> members{{&LiveBounds::Lw, &LiveBounds::Ll, &LiveBounds::M,
                                            &LiveBounds::P, &LiveBounds::W}};
    auto input = empty_inputs();
    for (auto& set : input) set.strings = {"", "X", "longer", "X"};
    const auto facts = take(ScratchVocabularyFacts::canonicalize(input));
    const auto external = [&](const ScratchVocabularyFacts& certificate, const std::optional<LiveBounds>& bounds) {
        const auto plan = take(decide_scratch_backend(certificate, bounds));
        require(plan.backend == ScratchBackendDecision::external_history && !plan.finite,
                "missing proof selects external history before execution");
    };
    external(facts, std::nullopt);
    external(facts, LiveBounds{});
    const auto default_bounds = take(decide_scratch_backend(facts));
    require(default_bounds.backend == ScratchBackendDecision::external_history && !default_bounds.finite,
            "omitting LiveBounds never selects a finite backend");
    for (const auto member : members) {
        auto bounds = witness_bounds();
        (bounds.*member).reset();
        external(facts, bounds);
    }
    for (std::size_t set = 0; set < input.size(); ++set) {
        auto incomplete = input;
        incomplete[set].complete = false;
        const auto certificate = take(ScratchVocabularyFacts::canonicalize(incomplete));
        external(certificate, witness_bounds());
        reject(derive_scratch_manifest(certificate), ScratchFactsError::incomplete_vocabulary);
        // A complete but unrepresentable live proof must not bypass the
        // incomplete-certificate dispatch into exact external history.
        external(certificate, LiveBounds{std::numeric_limits<std::uint64_t>::max(), 1, 0, 0, 0});
    }
    const auto bounds = witness_bounds();
    const auto plan = take(decide_scratch_backend(facts, bounds));
    require(plan.backend == ScratchBackendDecision::finite_certified && plan.finite,
            "all five complete rosters and all five live proofs select finite plan");
    // Independent live-bound oracle: enumerate labelled atomic owners, with
    // memberships contributing a root but no alias, and count the records.
    std::vector<std::pair<std::size_t, std::uint64_t>> roots, aliases;
    for (std::size_t kind = 0; kind < members.size(); ++kind) {
        for (std::uint64_t owner = 0; owner < *(bounds.*members[kind]); ++owner) {
            roots.emplace_back(kind, owner);
            if (members[kind] != &LiveBounds::M) aliases.emplace_back(kind, owner);
        }
    }
    require(plan.finite->root_slots == roots.size() && plan.finite->alias_slots == aliases.size(),
            "root and alias capacities cover enumerated owner categories");
    same_manifest(plan.finite->tables, enumerate_domain(input));
    const auto zero = take(decide_scratch_backend(facts, LiveBounds{0, 0, 0, 0, 0}));
    require(zero.backend == ScratchBackendDecision::finite_certified && zero.finite
            && zero.finite->root_slots == 0 && zero.finite->alias_slots == 0,
            "proved zero is present, not a missing bound");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto other : {&LiveBounds::Ll, &LiveBounds::P, &LiveBounds::W, &LiveBounds::M}) {
        LiveBounds overflow{maximum, 0, 0, 0, 0};
        overflow.*other = 1;
        reject(decide_scratch_backend(facts, overflow), ScratchFactsError::arithmetic_overflow);
    }
    const auto boundary = take(decide_scratch_backend(facts, LiveBounds{maximum, 0, 0, 0, 0}));
    require(boundary.finite && boundary.finite->root_slots == maximum && boundary.finite->alias_slots == maximum,
            "root numeric boundary derives a plan without allocating root pools");
    const auto empty = take(decide_scratch_backend(take(ScratchVocabularyFacts::canonicalize(empty_inputs())), bounds));
    require(empty.backend == ScratchBackendDecision::finite_certified && empty.finite,
            "proved empty vocabulary is complete");
    same_manifest(empty.finite->tables, enumerate_domain(empty_inputs()));
}
} // namespace

int main() {
    try {
        finite_domains();
        layout_accepts_derived_manifest();
        serialization_and_ownership();
        golden_wire_vector();
        hostile_wire();
        arithmetic_boundaries();
        backend_decisions();
        std::puts("test_scratch_facts: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_facts: %s\n", error.what());
        return 1;
    }
}
