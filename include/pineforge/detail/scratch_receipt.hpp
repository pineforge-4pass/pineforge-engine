#pragma once

// Lane V (CR-221 scope A): the engine-side reader of the codegen's additive
// `scratch_facts` receipt object. It adds no public ABI or export and binds no
// backend; it only decodes and validates the certificate before a run. The
// facts are immutable pre-begin values outside the engine's durable state, so
// this header changes no hash leaf and no trading byte.

#include <pineforge/detail/scratch_facts.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pineforge::detail {

// Version of the additive JSON object, independent of both the public
// order-shapes receipt version and kScratchFactsWireVersion.
inline constexpr std::uint32_t kScratchFactsReceiptVersion = 1;

namespace scratch_receipt_detail {

// Lowercase only: the producer emits lowercase hex, and accepting any other
// spelling would make two byte strings with one meaning compare unequal.
inline int hex_digit(char character) noexcept {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

// Strict hex decode: even length and lowercase hex digits only. Returns false
// without publishing a partial byte string on any violation.
inline bool decode_hex(std::string_view hex, std::string& out) {
    if (hex.size() % 2 != 0) {
        out.clear();
        return false;
    }
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const int high = hex_digit(hex[index]);
        const int low = hex_digit(hex[index + 1]);
        if (high < 0 || low < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

} // namespace scratch_receipt_detail

// Read one parsed `scratch_facts` object: its integer version, its
// classification label and the lowercase hex of the wire v1 certificate.
//
// The label is never trusted. "in_subset" is accepted only when the decoded
// certificate itself proves every one of the five rosters complete; otherwise
// the result is incomplete_vocabulary. "out_of_subset" is accepted over any
// certificate, complete or not. Hostile input returns a typed error from
// ScratchFactsError, never throws and never publishes partial facts.
//
// Error mapping, using lane F's closed enum (this header does not extend it):
//   version != 1                  -> unsupported_version
//   unknown classification label  -> invalid_complete_flag (the label is the
//                                    receipt's completeness flag)
//   odd length or non-[0-9a-f]    -> invalid_length (the no-partial-output
//                                    length contract of the wire)
//   every wire defect             -> decode_scratch_facts' own error unchanged
inline ScratchFactsResult<ScratchVocabularyFacts> scratch_facts_from_receipt(
        std::uint32_t version, std::string_view classification,
        std::string_view certificate_hex) noexcept {
    if (version != kScratchFactsReceiptVersion)
        return {ScratchFactsError::unsupported_version, std::nullopt};
    const bool in_subset = classification == "in_subset";
    if (!in_subset && classification != "out_of_subset")
        return {ScratchFactsError::invalid_complete_flag, std::nullopt};
    try {
        std::string wire;
        if (!scratch_receipt_detail::decode_hex(certificate_hex, wire))
            return {ScratchFactsError::invalid_length, std::nullopt};
        auto facts = decode_scratch_facts(wire);
        if (!facts) return facts;
        if (in_subset && !facts.value->complete())
            return {ScratchFactsError::incomplete_vocabulary, std::nullopt};
        return facts;
    } catch (const std::bad_alloc&) {
        return {ScratchFactsError::allocation_failure, std::nullopt};
    } catch (const std::length_error&) {
        return {ScratchFactsError::arithmetic_overflow, std::nullopt};
    }
}

} // namespace pineforge::detail
