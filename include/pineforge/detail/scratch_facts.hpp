#pragma once

#include <pineforge/detail/scratch_tables.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge::detail {

enum class ScratchFactsError {
    none,
    incomplete_vocabulary,
    arithmetic_overflow,
    allocation_failure,
    unsupported_version,
    truncated,
    invalid_complete_flag,
    invalid_length,
    duplicate_entry,
    unsorted_entry,
    trailing_bytes,
};

// Failure always has a disengaged value: no partial certificate or plan.
template<class Value>
struct ScratchFactsResult {
    ScratchFactsError error = ScratchFactsError::none;
    std::optional<Value> value;
    explicit operator bool() const noexcept {
        return error == ScratchFactsError::none && value.has_value();
    }
};

// Unsigned-byte lexical order, independent of the host's char signedness.
struct ScratchByteLess {
    bool operator()(std::string_view left, std::string_view right) const noexcept {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
    }
};

struct ScratchVocabularyInput {
    std::vector<std::string> strings;
    bool complete = false;
};

class ScratchVocabularyFacts;

// Owned binary strings exposed only through const access. No source-adapter
// types, borrowed byte lifetimes, hashes as identities, or mutable run state.
class ScratchVocabularySet {
public:
    const std::vector<std::string>& strings() const noexcept { return strings_; }
    bool complete() const noexcept { return complete_; }
private:
    std::vector<std::string> strings_;
    bool complete_ = false;
    friend class ScratchVocabularyFacts;
};

class ScratchVocabularyFacts {
public:
    ScratchVocabularyFacts() = default;
    ScratchVocabularyFacts(const ScratchVocabularyFacts&) = default;
    ScratchVocabularyFacts(ScratchVocabularyFacts&& other) noexcept : sets_(std::move(other.sets_)) {
        // A moved-from roster is no longer a proof of the original domain.
        for (auto& set : other.sets_) { set.strings_.clear(); set.complete_ = false; }
    }
    ScratchVocabularyFacts& operator=(const ScratchVocabularyFacts&) = delete;
    ScratchVocabularyFacts& operator=(ScratchVocabularyFacts&&) = delete;
    // Fixed roster order: S source IDs, F from_entry, E exit IDs,
    // H cohort arguments, J eligible entry/order-fill IDs.
    using Inputs = std::array<ScratchVocabularyInput, 5>;
    static ScratchFactsResult<ScratchVocabularyFacts> canonicalize(const Inputs& inputs) noexcept;
    const ScratchVocabularySet& source_ids() const noexcept { return sets_[0]; }
    const ScratchVocabularySet& from_entries() const noexcept { return sets_[1]; }
    const ScratchVocabularySet& exit_ids() const noexcept { return sets_[2]; }
    const ScratchVocabularySet& cohorts() const noexcept { return sets_[3]; }
    const ScratchVocabularySet& eligible_ids() const noexcept { return sets_[4]; }
    const std::array<ScratchVocabularySet, 5>& sets() const noexcept { return sets_; }
    bool complete() const noexcept {
        for (const auto& set : sets_) if (!set.complete()) return false;
        return true;
    }
private:
    std::array<ScratchVocabularySet, 5> sets_{};
};

inline ScratchFactsResult<ScratchVocabularyFacts>
ScratchVocabularyFacts::canonicalize(const Inputs& inputs) noexcept {
    try {
        ScratchVocabularyFacts facts;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto& set = facts.sets_[i];
            set.strings_ = inputs[i].strings;
            std::sort(set.strings_.begin(), set.strings_.end(), ScratchByteLess{});
            set.strings_.erase(std::unique(set.strings_.begin(), set.strings_.end()), set.strings_.end());
            set.complete_ = inputs[i].complete;
        }
        return {ScratchFactsError::none, std::move(facts)};
    } catch (const std::bad_alloc&) {
        return {ScratchFactsError::allocation_failure, std::nullopt};
    } catch (const std::length_error&) {
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    }
}

namespace scratch_facts_detail {
inline bool add(std::uint64_t left, std::uint64_t right, std::uint64_t& out) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
    out = left + right;
    return true;
}
inline bool multiply(std::uint64_t left, std::uint64_t right, std::uint64_t& out) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) return false;
    out = left * right;
    return true;
}
inline bool size_word(std::size_t size, std::uint64_t& out) noexcept {
    if constexpr (std::numeric_limits<std::size_t>::digits > 64) {
        if (size > std::numeric_limits<std::uint64_t>::max()) return false;
    }
    out = static_cast<std::uint64_t>(size);
    return true;
}
inline bool host_size(std::uint64_t size) noexcept {
    if constexpr (std::numeric_limits<std::size_t>::digits < 64)
        return size <= std::numeric_limits<std::size_t>::max();
    return true;
}
struct Totals { std::uint64_t count = 0, bytes = 0; };
inline bool totals(const ScratchVocabularySet& set, Totals& out) noexcept {
    if (!size_word(set.strings().size(), out.count)) return false;
    for (const auto& string : set.strings()) {
        std::uint64_t length = 0;
        if (!size_word(string.size(), length) || !add(out.bytes, length, out.bytes)) return false;
    }
    return true;
}

struct ErasedCapacity { std::uint64_t keys = 0, bytes = 0; };
// Numeric core is separately callable for representational-boundary tests;
// the certificate API obtains these numbers only from the owned rosters.
inline ScratchFactsResult<ErasedCapacity> erased_capacity(
        std::uint64_t a, std::uint64_t f, std::uint64_t s, std::uint64_t t) noexcept {
    std::uint64_t variants = 0, source_bytes = 0, from_bytes = 0;
    ErasedCapacity result;
    if (!add(1, f, variants) || !multiply(a, variants, result.keys)
        || !multiply(variants, s, source_bytes) || !multiply(a, t, from_bytes)
        || !add(source_bytes, from_bytes, result.bytes))
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    return {ScratchFactsError::none, result};
}
} // namespace scratch_facts_detail

// Pure pre-begin calculation. S/F use separate single/pair identities; c uses
// the existing joined byte string, including its intentional delimiter aliases.
// R retains its old all-successors meaning and is NEVER a live-root capacity.
inline ScratchFactsResult<ScratchManifest>
derive_scratch_manifest(const ScratchVocabularyFacts& facts) noexcept {
    using namespace scratch_facts_detail;
    if (!facts.complete()) return {ScratchFactsError::incomplete_vocabulary, std::nullopt};
    try {
        Totals source, from, cohort;
        if (!totals(facts.source_ids(), source) || !totals(facts.from_entries(), from)
            || !totals(facts.cohorts(), cohort))
            return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        const auto erased = erased_capacity(source.count, from.count, source.bytes, from.bytes);
        if (!erased) return {erased.error, std::nullopt};
        ScratchManifest manifest{};
        manifest.K_e = erased.value->keys;
        manifest.B_e = erased.value->bytes;
        manifest.U = cohort.count;
        manifest.B_U = cohort.bytes;

        std::uint64_t exit_count = 0, pair_count = 0;
        if (!size_word(facts.exit_ids().strings().size(), exit_count)
            || !multiply(exit_count, from.count, pair_count) || !host_size(pair_count))
            return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        std::vector<std::string> joined;
        if (pair_count > joined.max_size()) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        joined.reserve(static_cast<std::size_t>(pair_count));
        for (const auto& exit : facts.exit_ids().strings()) {
            for (const auto& entry : facts.from_entries().strings()) {
                std::uint64_t left = 0, right = 0, length = 0;
                if (!size_word(exit.size(), left) || !size_word(entry.size(), right)
                    || !add(left, 1, length) || !add(length, right, length) || !host_size(length))
                    return {ScratchFactsError::arithmetic_overflow, std::nullopt};
                std::string key;
                if (length > key.max_size()) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
                key.reserve(static_cast<std::size_t>(length));
                key.append(exit);
                key.push_back('\x1f');
                key.append(entry);
                joined.push_back(std::move(key));
            }
        }
        std::sort(joined.begin(), joined.end(), ScratchByteLess{});
        joined.erase(std::unique(joined.begin(), joined.end()), joined.end());
        if (!size_word(joined.size(), manifest.K_c)) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        for (const auto& key : joined) {
            std::uint64_t length = 0;
            if (!size_word(key.size(), length) || !add(manifest.B_c, length, manifest.B_c))
                return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        }

        // Retain the largest 27 lengths without copying or sorting J's bytes.
        std::array<std::uint64_t, 27> largest{};
        std::size_t used = 0;
        for (const auto& key : facts.eligible_ids().strings()) {
            std::uint64_t length = 0;
            if (!size_word(key.size(), length)) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
            if (used < largest.size()) largest[used++] = length;
            else {
                const auto smallest = std::min_element(largest.begin(), largest.end());
                if (*smallest < length) *smallest = length;
            }
        }
        if (!size_word(used, manifest.V)) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        for (std::size_t i = 0; i < used; ++i)
            if (!add(manifest.B_V, largest[i], manifest.B_V))
                return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        // K_q=B_q=R=0. Future root pools have their own separate plan below.
        return {ScratchFactsError::none, manifest};
    } catch (const std::bad_alloc&) {
        return {ScratchFactsError::allocation_failure, std::nullopt};
    } catch (const std::length_error&) {
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    }
}

// Each present number is a compiler/host PROOF for the effective trial, on a
// closed future-query surface: working definitions, filled lot incarnations,
// cohort memberships, other atomic leases, additional preparation leases.
// Missing proof, unrestricted native observers, or a bound dependent on bars
// or replacement counts MUST be represented as absent, never as a guessed cap.
struct LiveBounds {
    std::optional<std::uint64_t> Lw, Ll, M, P, W;
    bool complete() const noexcept { return Lw && Ll && M && P && W; }
};

enum class ScratchBackendDecision { finite_certified, external_history };

struct ScratchFinitePlan {
    ScratchManifest tables{}; // Internal table fields only; tables.R is unused zero.
    std::uint64_t root_slots = 0;
    std::uint64_t alias_slots = 0;
};

struct ScratchEffectivePlan {
    ScratchBackendDecision backend = ScratchBackendDecision::external_history;
    std::optional<ScratchFinitePlan> finite;
};

// Invoke after effective inputs/overrides, before semantic begin. This is an
// additive contract, NOT a v1 descriptor/setter or an activation of any backend.
// Overflow/allocation errors publish no plan and never authorize bounded use.
inline ScratchFactsResult<ScratchEffectivePlan> decide_scratch_backend(
        const ScratchVocabularyFacts& facts, const std::optional<LiveBounds>& bounds = std::nullopt) noexcept {
    if (!facts.complete() || !bounds || !bounds->complete())
        return {ScratchFactsError::none, ScratchEffectivePlan{}};
    ScratchFinitePlan finite;
    using scratch_facts_detail::add;
    if (!add(*bounds->Lw, *bounds->Ll, finite.alias_slots)
        || !add(finite.alias_slots, *bounds->P, finite.alias_slots)
        || !add(finite.alias_slots, *bounds->W, finite.alias_slots)
        || !add(finite.alias_slots, *bounds->M, finite.root_slots))
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    const auto tables = derive_scratch_manifest(facts);
    if (!tables) return {tables.error, std::nullopt};
    finite.tables = *tables.value;
    return {ScratchFactsError::none,
            ScratchEffectivePlan{ScratchBackendDecision::finite_certified, finite}};
}

// Certificate wire v1: one version byte, then five sets in S,F,E,H,J order.
// Each set: one complete byte (exactly 0/1), u64 LE count, then count entries
// of u64 LE byte length + raw bytes. Strict unsigned-byte lexical order,
// no duplicates/padding/trailer. Empty strings, NUL and 0x1f are ordinary bytes.
// LiveBounds is a separate effective-trial proof, deliberately not serialized
// here. Versioning is independent of the public scratch descriptor's version.
inline constexpr std::uint8_t kScratchFactsWireVersion = 1;

namespace scratch_facts_detail {
inline void append_word(std::string& out, std::uint64_t word) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<char>(static_cast<unsigned char>(word >> shift)));
}

// The first pass validates the ENTIRE wire without allocating. The second
// pass copies only validated bytes into private temporaries. No partial output
// escapes either pass; sizes are compared by subtraction before conversion.
inline ScratchFactsError read_wire(std::string_view wire, ScratchVocabularyFacts::Inputs* output) {
    std::size_t offset = 0;
    const auto read_byte = [&](std::uint8_t& byte) {
        if (offset == wire.size()) return false;
        byte = static_cast<std::uint8_t>(static_cast<unsigned char>(wire[offset++]));
        return true;
    };
    const auto read_word = [&](std::uint64_t& word) {
        if (wire.size() - offset < 8) return false;
        word = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            word |= static_cast<std::uint64_t>(static_cast<unsigned char>(wire[offset++])) << shift;
        return true;
    };
    std::uint8_t version = 0;
    if (!read_byte(version)) return ScratchFactsError::truncated;
    if (version != kScratchFactsWireVersion) return ScratchFactsError::unsupported_version;
    for (std::size_t set = 0; set < 5; ++set) {
        std::uint8_t complete = 0;
        if (!read_byte(complete)) return ScratchFactsError::truncated;
        if (complete > 1) return ScratchFactsError::invalid_complete_flag;
        std::uint64_t count = 0, minimum = 0;
        if (!read_word(count)) return ScratchFactsError::truncated;
        if (!multiply(count, 8, minimum)) return ScratchFactsError::arithmetic_overflow;
        if (minimum > wire.size() - offset) return ScratchFactsError::invalid_length;
        if (output) {
            (*output)[set].complete = complete != 0;
            (*output)[set].strings.reserve(static_cast<std::size_t>(count));
        }
        std::string_view previous;
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t length = 0;
            if (!read_word(length)) return ScratchFactsError::truncated;
            if (length > wire.size() - offset) return ScratchFactsError::invalid_length;
            const auto key = wire.substr(offset, static_cast<std::size_t>(length));
            if (i != 0 && !ScratchByteLess{}(previous, key))
                return previous == key ? ScratchFactsError::duplicate_entry : ScratchFactsError::unsorted_entry;
            if (output) (*output)[set].strings.emplace_back(key);
            previous = key;
            offset += static_cast<std::size_t>(length);
        }
    }
    return offset == wire.size() ? ScratchFactsError::none : ScratchFactsError::trailing_bytes;
}
} // namespace scratch_facts_detail

inline ScratchFactsResult<std::string> encode_scratch_facts(const ScratchVocabularyFacts& facts) noexcept {
    using namespace scratch_facts_detail;
    try {
        std::uint64_t total = 1;
        for (const auto& set : facts.sets()) {
            if (!add(total, 9, total)) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
            for (const auto& key : set.strings()) {
                std::uint64_t length = 0;
                if (!size_word(key.size(), length) || !add(total, 8, total) || !add(total, length, total))
                    return {ScratchFactsError::arithmetic_overflow, std::nullopt};
            }
        }
        std::string wire;
        if (!host_size(total) || total > wire.max_size()) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
        wire.reserve(static_cast<std::size_t>(total));
        wire.push_back(static_cast<char>(kScratchFactsWireVersion));
        for (const auto& set : facts.sets()) {
            std::uint64_t count = 0;
            if (!size_word(set.strings().size(), count)) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
            wire.push_back(set.complete() ? '\1' : '\0');
            append_word(wire, count);
            for (const auto& key : set.strings()) {
                std::uint64_t length = 0;
                if (!size_word(key.size(), length)) return {ScratchFactsError::arithmetic_overflow, std::nullopt};
                append_word(wire, length);
                wire.append(key);
            }
        }
        return {ScratchFactsError::none, std::move(wire)};
    } catch (const std::bad_alloc&) {
        return {ScratchFactsError::allocation_failure, std::nullopt};
    } catch (const std::length_error&) {
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    }
}

inline ScratchFactsResult<ScratchVocabularyFacts> decode_scratch_facts(std::string_view wire) noexcept {
    try {
        const auto validation = scratch_facts_detail::read_wire(wire, nullptr);
        if (validation != ScratchFactsError::none) return {validation, std::nullopt};
        ScratchVocabularyFacts::Inputs inputs;
        const auto copied = scratch_facts_detail::read_wire(wire, &inputs);
        if (copied != ScratchFactsError::none) return {copied, std::nullopt};
        return ScratchVocabularyFacts::canonicalize(inputs);
    } catch (const std::bad_alloc&) {
        return {ScratchFactsError::allocation_failure, std::nullopt};
    } catch (const std::length_error&) {
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    }
}

} // namespace pineforge::detail
