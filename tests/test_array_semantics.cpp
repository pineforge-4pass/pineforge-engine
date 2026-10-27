#include <pineforge/array.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include "array_store_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using array_test::require;

bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

void equal(const ScriptArray<double>& actual, const std::vector<double>& expected) {
    require(actual.size() == expected.size(), "array size differs from vector");
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(same_bits(actual.get_raw(static_cast<std::int64_t>(i)), expected[i]),
                "array element bytes differ from vector");
}

template<class Function>
void error(Function&& function, const char* expected) {
    bool raised = false;
    try {
        function();
    } catch (const coded<std::runtime_error>& failure) {
        require(failure.run_failure_code() == RunFailureCode::pine_array_error,
                "array diagnostic lost its code");
        require(std::string(failure.what()) == expected, "array diagnostic text changed");
        raised = true;
    }
    require(raised, "invalid array operation did not raise");
}

void indices(detail::o1::Store& store) {
    auto values = ScriptArray<double>::from(store, {10.0, -0.0, 30.0});
    require(is_na(values.get(-1)) && is_na(values.get(-100.0)), "negative raw read must be na");
    values.set(-1, 99.0);
    values.set(na<double>(), 99.0);
    require(values.last() == 30.0, "negative/na raw write changed the array");
    require(values.get_i(-1) == 30.0, "v6 negative read lost end-relative indexing");
    values.set_v6(-2.0, 12.0);
    require(values.get_raw(1) == 12.0, "v6 double index did not normalize");
    require(values.get(1.75) == 12.0, "generated numeric index truncation changed");
    values.insert(3, 40.0);
    values.insert(-1, 35.0);
    equal(values, {10.0, 12.0, 30.0, 35.0, 40.0});
    require(values.remove(-2) == 35.0, "negative removal returned the wrong element");
    error([&] { values.get_raw(-1); }, "Index -1 is out of bounds. Array size is 4");
    error([&] { values.set_raw(4, 0.0); }, "Index 4 is out of bounds. Array size is 4");
    error([&] { values.get_v6(na<double>()); }, "Index na is out of bounds. Array size is 4");
    error([&] { values.get_v6(na<int>()); }, "Index na is out of bounds. Array size is 4");
    error([&] { values.get_v6(std::numeric_limits<double>::infinity()); },
          "Index inf is out of bounds. Array size is 4");
    error([&] { values.get_v6(0x1p63); },
          "Index 9223372036854775808.000000 is out of bounds. Array size is 4");
    error([&] { values.get_v6(-0x1p63); },
          "Index -9223372036854775808 is out of bounds. Array size is 4");
    error([&] { values.get_i(na<std::int64_t>()); },
          "Index na is out of bounds. Array size is 4");
    error([&] { values.get_raw(na<int>()); },
          "Index na is out of bounds. Array size is 4");
    error([&] { values.set_raw(na<double>(), 0.0); },
          "Index na is out of bounds. Array size is 4");
    error([&] { values.remove_raw(std::numeric_limits<double>::infinity()); },
          "Index inf is out of bounds. Array size is 4");
    error([&] { values.insert_raw(-1, 0.0); },
          "Index -1 is out of bounds. Array size is 4");
    error([&] { values.insert_raw(na<int>(), 0.0); },
          "Index na is out of bounds. Array size is 4");
    require(values.get_raw(1.75) == 12.0, "v5 floating index did not truncate");
    values.insert_raw(4.75, 50.0);
    require(values.remove_raw(4.5) == 50.0, "v5 insert/remove did not permit the end insertion point");
    equal(values, {10.0, 12.0, 30.0, 40.0});
    error([&] { values.fill(1.0, na<double>(), 2); },
          "Index na is out of bounds. Array size is 4");
    error([&] { values.slice(0, std::numeric_limits<double>::infinity()); },
          "Index inf is out of bounds. Array size is 4");
    values.fill(9.0, 3, 1); // Compiler's inverted fill is empty, not an error.
    equal(values, {10.0, 12.0, 30.0, 40.0});
    error([&] { values.slice(3, 1); }, "Index 'from' should be less than index 'to'.");
    values.clear();
    error([&] { values.first(); }, "Cannot use first() if array is empty.");
    error([&] { values.last(); }, "Cannot use last() if array is empty.");
    error([&] { values.pop(); }, "Cannot use pop() if array is empty.");
    error([&] { values.shift(); }, "Cannot use shift() if array is empty.");
}

void differential(detail::o1::Store& store) {
    ScriptArray<double> actual(store);
    std::vector<double> reference;
    std::uint64_t state = UINT64_C(0x8af26d403be77195);
    for (int step = 0; step < 4096; ++step) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        const double value = step % 19 == 0 ? -0.0 : static_cast<double>(state % 65536) * 0.125;
        const auto at = reference.empty() ? 0 : static_cast<std::size_t>(state % reference.size());
        switch (state % 9) {
        case 0:
        case 1: actual.push(value); reference.push_back(value); break;
        case 2: actual.unshift(value); reference.insert(reference.begin(), value); break;
        case 3:
            actual.insert(static_cast<std::int64_t>(at), value);
            reference.insert(reference.begin() + static_cast<std::ptrdiff_t>(at), value);
            break;
        case 4:
            if (!reference.empty()) {
                require(same_bits(actual.remove_raw(static_cast<std::int64_t>(at)), reference[at]),
                        "remove return changed");
                reference.erase(reference.begin() + static_cast<std::ptrdiff_t>(at));
            }
            break;
        case 5:
            if (!reference.empty()) {
                actual.set_raw(static_cast<std::int64_t>(at), value);
                reference[at] = value;
            }
            break;
        case 6:
            actual.reverse(); std::reverse(reference.begin(), reference.end()); break;
        case 7:
            if (!reference.empty()) {
                require(same_bits(actual.pop(), reference.back()), "pop return changed");
                reference.pop_back();
            }
            break;
        case 8:
            if (step % 127 == 0) { actual.clear(); reference.clear(); }
            break;
        }
        equal(actual, reference);
    }

    actual.push(-1.125);
    reference.push_back(-1.125);
    auto copied = actual.copy();
    const auto old = actual.get_raw(0);
    copied.set_raw(0, old + 1.0);
    require(same_bits(actual.get_raw(0), old), "array.copy aliases its source");
    auto assigned = ScriptArray<double>::from(store, {-4.0});
    const auto assigned_id = assigned.id();
    assigned = actual;
    require(assigned.id() == assigned_id, "copy assignment replaced a stable root");
    equal(assigned, reference);
    auto middle = actual.slice(0, static_cast<std::int64_t>(actual.size()));
    equal(middle, reference);
    auto doubled = actual.copy();
    doubled.append(doubled);
    auto twice = reference;
    twice.insert(twice.end(), reference.begin(), reference.end());
    equal(doubled, twice);
    doubled.fill(-0.0);
    for (std::uint64_t i = 0; i < doubled.size(); ++i)
        require(same_bits(doubled.get_raw(static_cast<std::int64_t>(i)), -0.0), "fill lost signed zero");
}

void bytes_and_strings(detail::o1::Store& store) {
    const std::uint64_t pattern = UINT64_C(0x7ff80000000097a1);
    double payload;
    std::memcpy(&payload, &pattern, sizeof payload);
    auto numbers = ScriptArray<double>::from(store, {payload, -0.0, 0.0});
    auto copy = numbers.copy();
    require(same_bits(copy.first(), payload), "copy changed NaN payload bytes");
    require(same_bits(copy.get_i(1), -0.0), "copy changed negative zero bytes");

    std::string binary(3 * ScriptArray<std::string>::payload_page_bytes + 17, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<char>((i * 37) % 256);
    auto strings = ScriptArray<std::string>::from(store, {"", binary, "tail"});
    auto strings_copy = strings.copy();
    strings.set_i(1, "replacement");
    require(strings_copy.get_i(1) == binary, "string cell copy lost embedded zeros or aliased");
    require(strings_copy.remove_i(1) == binary, "string removal changed bytes");
    require(strings_copy.last() == "tail", "string removal damaged shifted ownership");
    strings_copy.clear();
    strings_copy.push(binary);
    require(strings_copy.first() == binary, "clear/reuse released a stale child id");
}

void primitive_defaults_and_moves(detail::o1::Store& store) {
    ScriptArray<double> floats(store, 2);
    ScriptArray<int> ints(store, 2);
    ScriptArray<std::int64_t> wide(store, 2);
    ScriptArray<bool> flags(store, 2);
    require(is_na(floats.first()) && is_na(ints.first()) && is_na(wide.first()),
            "array.new numeric defaults lost na");
    require(!flags.first() && !flags.last(), "array.new bool default is not false");
    flags.set_v6(-1, true);
    auto flags_copy = flags.copy();
    flags.set_raw(1, false);
    require(flags_copy.last() && !flags.last(), "bool cell copy shares its source");
    ints.set_raw(0, 123);
    wide.set_raw(0, INT64_C(9007199254740993));
    require(ints.first() == 123 && wide.first() == INT64_C(9007199254740993),
            "integral array cell passed through double");

    auto source = ScriptArray<double>::from(store, {-0.0, 2.0, 3.0});
    const auto source_id = source.id();
    auto moved = std::move(source);
    require(moved.id() == source_id && source.empty(), "move construction lost the owned root");
    auto target = ScriptArray<double>::from(store, {99.0});
    auto alias = target.alias();
    const auto target_id = target.id();
    target = std::move(moved);
    equal(alias, {-0.0, 2.0, 3.0});
    require(target.id() == target_id && moved.empty(), "move assignment rebound a prepared root");
    target = ScriptArray<double>{};
    require(alias.empty() && target.id() == target_id, "empty assignment broke a bound alias");
}

void cross_store_values(detail::o1::Store& first) {
    array_test::StoreFixture other(3);
    auto& second = *other.store;
    const auto original_live = second.stats().bytes_live;
    {
        auto child = ScriptArray<double>::from(first, {7.0, -0.0});
        ScriptArray<ScriptArray<double>> graph(first);
        graph.push(child);
        ScriptArray<ScriptArray<double>> copy(second);
        const auto root = copy.id();
        copy = graph;
        graph.mutate(0, [](ScriptArray<double>& value) { value.set_i(0, 19.0); });
        require(copy.first().first() == 7.0 && copy.id() == root,
                "cross-store nested assignment retained a foreign child id");
        graph.clear();
        require(same_bits(copy.first().last(), -0.0), "cross-store nested value was released with its source");
        auto strings = ScriptArray<std::string>::from(first, {"head", std::string(8193, 'q')});
        ScriptArray<std::string> text(second);
        text.append(strings);
        strings.clear();
        require(text.last() == std::string(8193, 'q'), "cross-store string append retained a foreign id");
    }
    require(second.stats().bytes_live == original_live, "cross-store copy leaked destination descendants");
}
} // namespace

int main() {
    try {
        array_test::StoreFixture fixture(3);
        const auto live = fixture.store->stats().bytes_live;
        indices(*fixture.store);
        differential(*fixture.store);
        bytes_and_strings(*fixture.store);
        primitive_defaults_and_moves(*fixture.store);
        cross_store_values(*fixture.store);
        require(fixture.store->stats().bytes_live == live, "array or string subtree leaked");
        std::puts("U01 semantics: vector differential, diagnostics, value copies, exact bytes");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "U01 semantics: %s\n", failure.what());
        return 1;
    }
}
