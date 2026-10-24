#include "p2_a5strmap_fixture.hpp"
#include "p2_a5strmap_store_fixture.hpp"
#include <pineforge/str_utils.hpp>
#include <pineforge/drawing.hpp>

#include <cfenv>
#include <cstring>
#include <functional>
#include <string_view>

namespace {
using namespace pineforge;
using a5_fixture::require;

std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
double old_number(const std::string& value) {
    try { return std::stod(value); }
    catch (const std::invalid_argument&) { return na<double>(); }
    catch (const std::out_of_range&) { return na<double>(); }
}
void parse(a5_fixture::Session& session, const std::string& bytes) {
    ScriptString text(*session.store, std::string_view(bytes));
    const auto expected = bits(old_number(bytes));
    const auto before = a5_heap::live.load();
    a5_heap::reset_peak();
    {
        a5_heap::Window count;
        require(bits(str_tonumber(text)) == expected, "str.tonumber changed binary64 bits");
    }
    require(a5_heap::growth(a5_heap::peak.load(), before) <= a5_fixture::operation_ceiling,
            "numeric parser allocated an input-sized temporary");
}

void bytes_and_numbers(a5_fixture::Session& session) {
    std::vector<std::string> corpus{
        "", "na", "NaN", "nan(123)", "nan(0x123)", "nan(077)", "nan(_)",
        "Infinity", "+inf", "-infinity", "-0", "+0", "  1.25suffix", "1e+",
        "0x1.8p+2", "0x!", "--1", "0b10", std::string("12\0.5", 5),
        std::string("\0abc", 4), "\xc3\xa9\xe4\xb8\xad\xf0\x9f\x99\x82",
        "\xff\x80\xc0", "4.9406564584124654e-324", "2.2250738585072014e-308",
        "1.7976931348623157e308", "1e309", "1e-999"
    };
    std::string all_bytes;
    for (unsigned i = 0; i < 256; ++i) all_bytes += static_cast<char>(i);
    corpus.push_back(all_bytes);
    for (const auto& lhs : corpus) {
        ScriptString left(*session.store, std::string_view(lhs));
        require(left.size() == lhs.size() && left.to_std_string() == lhs,
                "string byte length or boundary encoding changed");
        require(left.hash() == std::hash<std::string>{}(lhs), "string hash changed");
        parse(session, lhs);
        for (const auto& rhs : corpus) {
            ScriptString right(*session.store, std::string_view(rhs));
            require((left == right) == (lhs == rhs) && (left < right) == (lhs < rhs),
                    "string equality or unsigned-byte ordering changed");
            require(left.find(right) == lhs.find(rhs), "string find changed");
            const auto actual = str_split(left, right);
            const auto expected = str_split(lhs, rhs);
            require(actual.size() == expected.size(), "split part count changed");
            for (std::size_t i = 0; i < expected.size(); ++i)
                require(actual[i] == expected[i], "split part bytes changed");
        }
    }
    require(na<ScriptString>().empty(), "na string stopped using the empty sentinel");
    std::uint32_t random = 41;
    for (std::size_t length : {4095U, 4096U, 4097U, 8191U, 8192U, 16389U, 65537U}) {
        std::string bytes(length, '\0');
        for (auto& ch : bytes) {
            random = random * 1664525U + 1013904223U;
            ch = static_cast<char>(random >> 24);
        }
        ScriptString external(*session.store, std::string_view(bytes));
        const auto expected = std::hash<std::string>{}(bytes);
        const auto before = a5_heap::live.load();
        a5_heap::reset_peak();
        {
            a5_heap::Window count;
            require(external.hash() == expected, "streaming hash changed across a page or word tail");
        }
        require(a5_heap::growth(a5_heap::peak.load(), before) <= a5_fixture::operation_ceiling,
                "streaming hash allocated a whole-payload temporary");
    }
    constexpr std::size_t long_digits = 64 * 1024;
    const std::string zeros(long_digits, '0'), nines(long_digits, '9');
    const auto decimal_exponent = std::to_string(long_digits);
    const auto hex_exponent = std::to_string(4 * long_digits);
    const std::vector<std::string> long_numbers{
        zeros + "42.25tail", "1." + zeros + "1", "-0." + zeros + "1e" + decimal_exponent,
        "0." + zeros + "1e" + std::to_string(long_digits + 1),
        "1" + zeros + "e-" + decimal_exponent,
        "1" + zeros + "e-" + std::to_string(long_digits + 1),
        std::string(long_digits, ' ') + "+12.25", "1e" + nines, "1e-" + nines,
        "1.00000000000000011102230246251565404236316680908203125" + zeros,
        "1.00000000000000011102230246251565404236316680908203125" + zeros + "1",
        "1.00000000000000011102230246251565404236316680908203124" + nines,
        "0x1" + zeros + "p-" + hex_exponent, "0x0." + zeros + "8p" + hex_exponent,
        "nan(" + zeros + "123)", "-nan(" + nines + ")", "nan(foo" + zeros + ")",
        "-nan(" + nines + "_)", "-nan(" + nines + "-)", "-nan(0x" + nines + "g)",
        std::string("12\0", 3) + nines, "0x!" + zeros
    };
    const int previous = std::fegetround();
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        std::fesetround(rounding);
        for (const auto& bytes : long_numbers) parse(session, bytes);
    }
    std::fesetround(previous);
}

void values_and_boundaries(a5_fixture::Session& session) {
    auto& store = *session.store;
    const auto initial_live = store.stats().bytes_live;
    {
        ScriptString value(store, "prefix", 8);
        const auto checkpoint = value;
        const auto slice = value.substr(1, 3);
        value += "-later";
        auto branch = checkpoint + "-branch";
        require(value == "prefix-later" && checkpoint == "prefix" && slice == "ref" &&
                    branch == "prefix-branch", "string mutation changed a retained value");
        branch += branch;
        require(branch == "prefix-branchprefix-branch", "self append changed bytes");
        ScriptString prepared(store, {}, 16);
        const auto prepared_live = store.stats().bytes_live;
        prepared += "0123456789abcdef";
        require(store.stats().bytes_live == prepared_live, "proven bound was not prepared");
        prepared += "beyond-proof";
        require(prepared == "0123456789abcdefbeyond-proof", "proof hint became a capacity limit");
        const auto pin = value.pin();
        auto decoded = ScriptString::from_handle(store, pin);
        ScriptString::unpin(store, pin);
        require(decoded == value, "record codec did not retain exact bytes");

        const auto before_tx = value;
        {
            auto tx = store.begin();
            value += "-tentative";
            value = before_tx; // Restore C++ handles before transactional refcounts.
            tx->rollback();
        }
        require(value == before_tx, "string transaction did not restore exact value");
        value.with_std_string([](std::string& text) { text.append("\0tail", 5); });
        const auto boundary = value.to_std_string();
        require(boundary == std::string("prefix-later\0tail", 17), "raw string boundary lost NUL");
        const auto before_exception = value;
        try {
            value.with_std_string([](std::string& text) {
                text = "discarded";
                throw std::runtime_error("boundary callback");
            });
        } catch (const std::runtime_error&) {}
        require(value == before_exception, "failed raw string callback published partial bytes");
        DrawingArena<LabelRec> labels;
        const auto label = pf_label_new(labels, 3, 1.25, boundary);
        require(pf_label_get_text(labels, label) == boundary, "label boundary changed exact bytes");
    }
    require(store.stats().bytes_live == initial_live, "unreachable string payload was retained");
}

void formatting(a5_fixture::Session& session) {
    auto& store = *session.store;
    for (double value : {0.0, -0.0, 1.005, -1.005, 1234.5, 1e30,
                         0.00012345678901234567, std::nextafter(1e17, 0.0),
                         std::nextafter(1e17, 1e18), 999999999999999.5,
                         std::numeric_limits<double>::max(), na<double>(),
                         std::numeric_limits<double>::infinity()}) {
        for (const char* mode : {"", "mintick", "percent", "volume", "#,###.00", "#.##%"}) {
            const ScriptString external_mode(store, mode);
            const auto expected = str_tostring(value, mode, 0.01);
            const auto before_calls = a5_heap::calls.load();
            {
                a5_heap::Window count;
                require(str_tostring(store, value, external_mode, 0.01) == expected,
                        "str.tostring formatting changed");
            }
            if (std::string_view(mode) != "mintick")
                require(a5_heap::calls.load() == before_calls,
                        "prepared numeric rendering allocated C++ heap scratch");
        }
    }
    ScriptString text(store, std::string_view("text\0tail", 9));
    for (const char* pattern : {"{0}|{1}", "'{0}' '' {1,number,percent}",
                               "{9} {x} {0,unknown,style}", "{0,number,currency} {"}) {
        ScriptString external_pattern(store, pattern);
        const auto actual = str_format_values(external_pattern,
            std::vector<ScriptStrFormatValue>{1234.5, text});
        const auto expected = str_format_values(std::string(pattern),
            std::vector<StrFormatValue>{1234.5, text.to_std_string()});
        require(actual == expected, "str.format grammar or argument bytes changed");
    }
    ScriptString long_pattern(store, {}, 20000);
    for (int i = 0; i < 20000; ++i) long_pattern += i % 2 ? '#' : '0';
    const auto expected = str_tostring(1.25, long_pattern.to_std_string());
    const auto before = a5_heap::live.load();
    a5_heap::reset_peak();
    {
        a5_heap::Window count;
        const auto output = str_tostring(store, 1.25, long_pattern);
        require(output == expected,
                "large format pattern changed rendering");
    }
    require(a5_heap::growth(a5_heap::peak.load(), before) <= a5_fixture::operation_ceiling,
            "numeric formatting copied its complete pattern");
}
} // namespace

int main() {
    try {
        a5_fixture::Session session;
        ScriptStringScope scope(*session.store);
        bytes_and_numbers(session);
        values_and_boundaries(session);
        formatting(session);
        std::puts("A5STRING parity: exact bytes/hash/parse/format/boundaries checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5STRING parity FAIL: %s\n", error.what());
        return 1;
    }
}
