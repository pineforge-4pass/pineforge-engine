// P23/P24/P25 (lane core-cr150-o1p2b): the two 64-bit membership sets and the
// named-entry cancel dictionary, moved from unordered containers onto the
// adapter's ordered store.
//
// Fail-before target: 6cfcff0696. There the members are
// std::unordered_set<std::uint64_t> / std::unordered_map<SourceId, ...>: the
// ordered-store API this unit calls (resident_bytes/resident_limit/contains/
// erase_key/first/upsert) does not exist, and the populations grow without any
// fixed resident bound. The reference below is an independent std::set /
// std::map: it never calls the production arithmetic and it is the order the
// source hash reader sorts these keys into before folding them.
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using a4handle = source::a4_o1p2b::ScratchHandle;

int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

// The adapter's private-access loophole already used by the lane's fee unit:
// an explicit instantiation may name a private member, and the injected
// friend hands the test its pointer without widening the adapter's surface.
template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
struct Debited { friend auto member(Debited); };
template struct Access<Debited, &Adapter::current_debited_applied_ordinals_>;
struct Shadowed { friend auto member(Shadowed); };
template struct Access<Shadowed, &Adapter::bracket_shadowed_openings_>;
struct Tokens { friend auto member(Tokens); };
template struct Access<Tokens, &Adapter::named_entry_cancel_tokens_>;

template<class Table>
std::vector<std::uint64_t> values(const Table& table) {
    std::vector<std::uint64_t> result;
    for (const auto value : table) result.push_back(value);
    return result;
}

// P24: bracket-shadowed openings.
void shadowed_openings() {
    Adapter adapter;
    auto& set = adapter.*member(Shadowed{});
    std::set<std::uint64_t> reference;
    for (const std::uint64_t value : {std::uint64_t{7}, std::uint64_t{2},
                                      std::uint64_t{9}, std::uint64_t{2},
                                      std::uint64_t{7}, std::uint64_t{11}}) {
        set.insert(value);
        reference.insert(value);
    }
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());
    CHECK(values(set) == std::vector<std::uint64_t>(reference.begin(), reference.end()));
    for (const auto value : reference) CHECK(set.contains(value));
    CHECK(!set.contains(13));
    // unordered_set::erase(key) count semantics.
    CHECK(set.erase(7) == 1);
    CHECK(set.erase(7) == 0);
    reference.erase(7);
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());

    // The resident workspace is the prepared configuration, not the population.
    const std::size_t bound_bytes = set.resident_bytes();
    CHECK(bound_bytes == set.resident_limit());
    for (std::uint64_t value = 100; value < 4200; ++value) {
        set.insert(value);
        reference.insert(value);
    }
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());
    CHECK(set.resident_bytes() == bound_bytes);

    // A rolled-back transaction leaves membership exactly as it was.
    set.begin_batch();
    a4handle first = 0;
    CHECK(set.first(first));
    set.erase_handle(first);
    set.rollback_batch();
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());
    for (const auto value : reference) CHECK(set.contains(value));

    set.clear();
    CHECK(set.size() == 0);
    CHECK(!set.contains(2));
}

// P23: applied ordinals already debited synchronously.
void debited_ordinals() {
    Adapter adapter;
    auto& set = adapter.*member(Debited{});
    std::set<std::uint64_t> reference;
    for (const std::uint64_t ordinal : {std::uint64_t{5}, std::uint64_t{3},
                                        std::uint64_t{5}, std::uint64_t{1},
                                        std::uint64_t{0}}) {
        set.insert(ordinal);
        reference.insert(ordinal);
    }
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());
    CHECK(values(set) == std::vector<std::uint64_t>(reference.begin(), reference.end()));
    CHECK(set.erase(3) == 1);
    CHECK(set.erase(3) == 0);
    reference.erase(3);
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());

    const std::size_t bound_bytes = set.resident_bytes();
    for (std::uint64_t ordinal = 700; ordinal < 4700; ++ordinal) {
        set.insert(ordinal);
        reference.insert(ordinal);
    }
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());
    CHECK(set.resident_bytes() == bound_bytes);

    set.begin_batch();
    a4handle first = 0;
    CHECK(set.first(first));
    set.erase_handle(first);
    set.rollback_batch();
    CHECK(static_cast<std::size_t>(set.size()) == reference.size());
    CHECK(values(set) == std::vector<std::uint64_t>(reference.begin(), reference.end()));
}

// P25: named-entry cancel tokens.
void named_cancel_tokens() {
    Adapter adapter;
    auto& tokens = adapter.*member(Tokens{});
    using Table = std::remove_reference_t<decltype(tokens)>;
    using Row = Table::Row;
    // Independent dictionary: key -> (entry incarnation, surviving exit).
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> reference;
    const struct { const char* id; std::uint64_t entry; std::uint64_t exit; } rows[] = {
        {"id-B", 22, 220}, {"id-A", 11, 110}, {"id-C", 33, 330},
        {"id-a", 44, 440}, {"id-A", 12, 120},
    };
    for (const auto& expected : rows) {
        Row row;
        row.first = expected.id;
        row.second.entry_incarnation = expected.entry;
        row.second.surviving_exit_incarnation = expected.exit;
        tokens.upsert(row.first, row);
        reference[row.first] = {expected.entry, expected.exit};
    }
    CHECK(static_cast<std::size_t>(tokens.size()) == reference.size());
    // Iteration is the ascending key order the source hash sorts into.
    std::vector<std::string> keys;
    for (const auto& row : tokens) keys.push_back(row.first);
    std::vector<std::string> sorted;
    for (const auto& row : reference) sorted.push_back(row.first);
    CHECK(keys == sorted);
    for (const auto& row : reference) {
        const auto value = tokens.at(row.first); // at() returns the value half
        CHECK(value.entry_incarnation == row.second.first);
        CHECK(value.surviving_exit_incarnation == row.second.second);
    }
    CHECK(tokens.contains("id-A"));
    CHECK(!tokens.contains("missing"));

    // An upsert under an existing key replaces the row and keeps its place.
    Row replacement;
    replacement.first = "id-B";
    replacement.second = tokens.at("id-B");
    replacement.second.surviving_exit_incarnation = 999;
    tokens.upsert(replacement.first, replacement);
    CHECK(tokens.at("id-B").surviving_exit_incarnation == 999);
    CHECK(static_cast<std::size_t>(tokens.size()) == reference.size());

    tokens.erase_key("id-A");
    reference.erase("id-A");
    CHECK(!tokens.contains("id-A"));
    CHECK(static_cast<std::size_t>(tokens.size()) == reference.size());

    // A rolled-back transaction restores the removed row.
    tokens.begin_batch();
    a4handle first = 0;
    CHECK(tokens.first(first));
    tokens.erase_handle(first);
    tokens.rollback_batch();
    CHECK(static_cast<std::size_t>(tokens.size()) == reference.size());

    const std::size_t bound_bytes = tokens.resident_bytes();
    for (std::uint64_t index = 0; index < 4000; ++index) {
        Row row;
        row.first = "id-" + std::to_string(index);
        row.second.entry_incarnation = index + 1;
        row.second.surviving_exit_incarnation = index + 2;
        tokens.upsert(row.first, row);
        reference[row.first] = {index + 1, index + 2};
    }
    CHECK(static_cast<std::size_t>(tokens.size()) == reference.size());
    CHECK(tokens.resident_bytes() == bound_bytes);
    keys.clear();
    for (const auto& row : tokens) keys.push_back(row.first);
    sorted.clear();
    for (const auto& row : reference) sorted.push_back(row.first);
    CHECK(keys == sorted);

    // at() on a missing key is the owning map's out_of_range.
    bool threw = false;
    try {
        (void)tokens.at("missing");
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}
}

int main() {
    shadowed_openings();
    debited_ordinals();
    named_cancel_tokens();
    std::printf("o1p2b ordered sets/tokens: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
