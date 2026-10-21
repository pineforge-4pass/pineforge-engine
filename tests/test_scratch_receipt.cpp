// Frozen lane-F base: include/pineforge/detail/scratch_receipt.hpp does not
// exist there. The lab must record that missing-header first diagnostic before
// compiling this implementation; every body below is a real assertion.
//
// Independent oracle: every wire below is assembled byte by byte in this file
// from docs/design/scratch-facts.md, and the three cross-language vectors come
// from tests/data/scratch_receipt_golden.json, whose bytes were typed by hand.
// No expected value is read back from the production encoder.
#include <pineforge/detail/scratch_receipt.hpp>

#include "../runner/json.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::detail;
namespace json = pineforge::live;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Value>
Value take(ScratchFactsResult<Value> result) {
    require(result.error == ScratchFactsError::none && result.value.has_value(),
            "expected complete success value");
    return std::move(*result.value);
}

template<class Value>
void reject(const ScratchFactsResult<Value>& result, ScratchFactsError error) {
    require(!result && result.error == error && !result.value,
            "typed failure must expose no partial value");
}

void append_word(std::string& out, std::uint64_t word) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<char>(static_cast<unsigned char>(word >> shift)));
}

std::string lowercase_hex(std::string_view bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return hex;
}

// Wire v1 with five empty complete rosters, built here and not by the encoder.
std::string empty_wire() {
    std::string wire(1, static_cast<char>(1));
    for (int set = 0; set < 5; ++set) {
        wire.push_back(static_cast<char>(1));
        append_word(wire, 0);
    }
    return wire;
}

std::string empty_certificate() { return lowercase_hex(empty_wire()); }

std::string read_file(int argc, char** argv) {
    std::vector<std::string> candidates;
    if (argc > 1) candidates.emplace_back(argv[1]);
    const std::string here = __FILE__;
    const auto slash = here.find_last_of('/');
    if (slash != std::string::npos)
        candidates.push_back(here.substr(0, slash) + "/data/scratch_receipt_golden.json");
    // Relative fallbacks for a generator that passes a source-relative path.
    candidates.emplace_back("tests/data/scratch_receipt_golden.json");
    candidates.emplace_back("../tests/data/scratch_receipt_golden.json");
    for (const auto& path : candidates) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }
    throw std::runtime_error("golden file is missing");
}

void strict_hex_and_labels() {
    // Version of the additive receipt object.
    for (const std::uint32_t version : {0u, 2u, 99u}) {
        reject(scratch_facts_from_receipt(version, "out_of_subset", empty_certificate()),
               ScratchFactsError::unsupported_version);
    }
    // The classification label is the receipt's completeness flag: anything
    // else is a typed error, never a default.
    for (const std::string_view label : {"", "IN_SUBSET", "in-subset", "yes"}) {
        reject(scratch_facts_from_receipt(1, label, empty_certificate()),
               ScratchFactsError::invalid_complete_flag);
    }
    // A valid out_of_subset certificate decodes to complete facts (the label
    // does not weaken decoding).
    const auto external = take(scratch_facts_from_receipt(1, "out_of_subset", empty_certificate()));
    require(external.complete(), "out_of_subset accepts a complete certificate");
    // Strict lowercase hex: odd length, uppercase and non-hex digits fail.
    const std::string certificate = empty_certificate();
    reject(scratch_facts_from_receipt(1, "out_of_subset", certificate.substr(0, certificate.size() - 1)),
           ScratchFactsError::invalid_length);
    for (const std::string_view hostile : {"", "zz", "0g", "00ffGG"}) {
        reject(scratch_facts_from_receipt(1, "out_of_subset", hostile),
               hostile.empty() ? ScratchFactsError::truncated : ScratchFactsError::invalid_length);
    }
    std::string upper = certificate;
    upper[0] = 'A';
    reject(scratch_facts_from_receipt(1, "out_of_subset", upper), ScratchFactsError::invalid_length);
    // Even-length valid hex of a truncated wire reaches F's own error.
    reject(scratch_facts_from_receipt(1, "out_of_subset", "01"), ScratchFactsError::truncated);
}

void every_decoder_error_through_the_receipt() {
    const auto empty = empty_wire();
    // Unsupported certificate version byte.
    std::string version = empty;
    version[0] = static_cast<char>(2);
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(version)),
           ScratchFactsError::unsupported_version);
    // Invalid complete flag.
    for (std::size_t set = 0; set < 5; ++set) {
        std::string flag = empty;
        flag[1 + 9 * set] = static_cast<char>(2);
        reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(flag)),
               ScratchFactsError::invalid_complete_flag);
    }
    // Trailing bytes.
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(empty + std::string(1, '\0'))),
           ScratchFactsError::trailing_bytes);
    // Arithmetic overflow in count*8, and a count whose minimum length words
    // do not fit the remaining bytes.
    std::string huge("\1\1", 2);
    append_word(huge, std::uint64_t{1} << 61);
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(huge)),
           ScratchFactsError::arithmetic_overflow);
    std::string short_count("\1\1", 2);
    append_word(short_count, 2);   // count = 2
    append_word(short_count, 8);   // one 8-byte length word, no entry bytes
    // Two entries need at least 16 bytes of length words; only 8 remain.
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(short_count)),
           ScratchFactsError::invalid_length);
    // A length word larger than the remaining bytes.
    std::string long_entry("\1\1", 2);
    append_word(long_entry, 1);
    append_word(long_entry, 4);
    long_entry += "abc";
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(long_entry)),
           ScratchFactsError::invalid_length);
    // Duplicate and unsorted entries inside S.
    std::string duplicate("\1\1", 2);
    append_word(duplicate, 2);
    append_word(duplicate, 2);
    duplicate += "ab";
    append_word(duplicate, 2);
    duplicate += "ab";
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(duplicate)),
           ScratchFactsError::duplicate_entry);
    std::string unsorted("\1\1", 2);
    append_word(unsorted, 2);
    append_word(unsorted, 1);
    unsorted += "b";
    append_word(unsorted, 1);
    unsorted += "a";
    reject(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(unsorted)),
           ScratchFactsError::unsorted_entry);
    // Every truncated prefix of a valid certificate returns a typed error.
    std::string valid = empty_wire();
    for (std::size_t cut = 0; cut < valid.size(); ++cut) {
        std::string prefix;
        prefix.reserve(cut);
        for (std::size_t index = 0; index < cut; ++index) prefix.push_back(valid[index]);
        const auto result = scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(prefix));
        require(!result && result.error != ScratchFactsError::none && !result.value,
                "every truncated certificate is rejected without partial facts");
    }
}

void the_label_is_never_trusted() {
    // An insecure producer can claim in_subset over an incomplete roster.
    std::string wire(1, static_cast<char>(1));
    wire.push_back(static_cast<char>(1));
    append_word(wire, 0);
    for (int set = 1; set < 5; ++set) {
        wire.push_back(static_cast<char>(set == 2 ? 0 : 1));
        append_word(wire, 0);
    }
    reject(scratch_facts_from_receipt(1, "in_subset", lowercase_hex(wire)),
           ScratchFactsError::incomplete_vocabulary);
    const auto accepted = take(scratch_facts_from_receipt(1, "out_of_subset", lowercase_hex(wire)));
    require(!accepted.complete() && !accepted.exit_ids().complete(),
            "out_of_subset keeps the decoded facts, incomplete roster included");
}

void golden_vectors(int argc, char** argv) {
    // Wire arithmetic, from docs/design/scratch-facts.md:50-62: each set is
    // 1 complete byte + 8 count bytes + per entry (8 length bytes + its bytes).
    //   doc_worked_vector 1 + (1+8+8+2) + (1+8+8+0) + (1+8+8+1) + (1+8) + (1+8+8+1) = 82
    //   incomplete_exit   1 + 18 + 17 + 18 + (1+8+8+8+1) + 18                          = 98
    //   sorted/NUL        1 + (1+8+16+2) + 9 + (1+8+8+3) + 9 + (1+8+16+2)            = 93
    const auto document = json::parse_json(read_file(argc, argv));
    require(document.at("version").integer<int>() == 1, "golden schema version");
    const auto& vectors = document.at("vectors");
    require(vectors.kind == json::Json::Kind::Array && !vectors.items.empty(), "golden vectors");
    for (const auto& vector : vectors.items) {
        // The hand-typed layout and the hand-typed certificate agree.
        std::string joined;
        for (const auto& block : vector.at("layout").items) joined += block.text();
        const auto certificate = vector.at("certificate").text();
        require(joined == certificate, "layout and certificate are two transcriptions of one vector");
        const auto facts = take(scratch_facts_from_receipt(
            1, vector.at("classification").text(), certificate));
        const auto& sets = vector.at("sets");
        require(sets.kind == json::Json::Kind::Array && sets.items.size() == 5,
                "one golden entry per roster");
        for (std::size_t set = 0; set < 5; ++set) {
            const auto& expected = sets.items[set];
            const bool complete = expected.at("complete").value == "true";
            require(facts.sets()[set].complete() == complete, "golden completeness flag");
            std::vector<std::string> strings;
            for (const auto& item : expected.at("strings").items) strings.push_back(item.text());
            require(facts.sets()[set].strings() == strings, "golden roster bytes");
        }
        if (vector.at("classification").text() == "in_subset") {
            require(facts.complete(), "an in_subset golden decodes to complete facts");
        }
    }
    // The incomplete golden is rejected when it claims to be in_subset.
    const auto& incomplete = vectors.items[1];
    reject(scratch_facts_from_receipt(1, "in_subset", incomplete.at("certificate").text()),
           ScratchFactsError::incomplete_vocabulary);
}

void hostile_input_never_throws() {
    static_assert(noexcept(scratch_facts_from_receipt(1, std::string_view{}, std::string_view{})),
                  "the receipt reader is noexcept");
    // Long garbage of every parity, plus embedded NULs and non-ASCII bytes.
    const std::string junk(4096, 'g');
    for (const std::string_view hostile : {
             std::string_view(junk), std::string_view(junk.data(), 4095),
             std::string_view("00\0ff", 5), std::string_view("\xff\xfe\xfd", 3)}) {
        const auto result = scratch_facts_from_receipt(1, "out_of_subset", hostile);
        require(!result && result.error != ScratchFactsError::none,
                "hostile certificate returns a typed error");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        strict_hex_and_labels();
        every_decoder_error_through_the_receipt();
        the_label_is_never_trusted();
        golden_vectors(argc, argv);
        hostile_input_never_throws();
        std::puts("test_scratch_receipt: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_receipt: %s\n", error.what());
        return 1;
    }
}
