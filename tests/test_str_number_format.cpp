// R5 lane B-ENGINE: the engine's number text follows the rule TradingView's
// tapes pin, the one the codegen formatter emits (lane C6b).
//
// str_tostring's default, percent, volume and pattern modes and
// str_format_values' placeholders render a number from its shortest
// round-trip decimal spelling, scaling and rounding (half-up) on those digits.
// Rows, each an exact string:
//
//   c6b_*    the codegen formatter's 42-case edge battery, through the
//            engine's public calls: ties exact in binary64 (0.125, 2.5), ties
//            only in their shortest spelling (1.005, 1.015, 0.285), the
//            doubles just below and above a tie, grouping at 1e15 and 1e17,
//            negative ties and values rounding to zero, -0.0, NaN, the
//            infinities, required fraction zeros, the default, percent,
//            number-style percent and currency, volume, and a grouped
//            str.format;
//   tape_*   every numeric string of lane C6's three TradingView exports
//            (tests/fixtures/c6_tv_evidence in the codegen repository), the
//            tape's own text;
//   text_*   str_format with text arguments: quoting, kept placeholders and
//            no re-substitution of an inserted text;
//   mintick_*  the mintick mode, which generated code delegates to the engine,
//            byte-identical to 8cf3be58 (pinned from that build).
//
// With an argument the rows are also written, one "name<TAB>text" line each,
// to that file: the lane runs it on macOS arm64 and on Linux x86-64 (Ubuntu,
// GCC 13) and the two files are byte-identical.
//
// Fail-before: on 8cf3be58 this TU does not compile (no str_format_values);
// the lane report records the first diagnostic and the rows the old
// str_tostring answers differently. Source-free: the kernel-only profile
// registers the row.
#include <pineforge/na.hpp>
#include <pineforge/str_utils.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

struct Row {
    std::string name;
    std::string got;
    std::string want;
};

std::vector<Row> rows;

void row(const char* name, const std::string& got, const char* want) {
    rows.push_back({name, got, want});
}

double below(double x) { return std::nextafter(x, -std::numeric_limits<double>::infinity()); }
double above(double x) { return std::nextafter(x, std::numeric_limits<double>::infinity()); }

std::string fmt1(const char* format, double value) {
    return str_format_values(format, {value});
}

void c6b_battery() {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    row("c6b_exact_0125", str_tostring(0.125, "#.##"), "0.13");
    row("c6b_exact_25", str_tostring(2.5, "#"), "3");
    row("c6b_short_1005", str_tostring(1.005, "#.##"), "1.01");
    row("c6b_short_1015", str_tostring(1.015, "#.##"), "1.02");
    row("c6b_short_0285", str_tostring(0.285, "#.##"), "0.29");
    // The literal is the same binary64 value as 1.005, whose shortest
    // spelling is "1.005": it ties upward.
    row("c6b_literal_10049999999999999", str_tostring(1.0049999999999999, "#.##"), "1.01");
    row("c6b_below_1005", str_tostring(below(1.005), "#.##"), "1");
    row("c6b_above_1005", str_tostring(above(1.005), "#.##"), "1.01");
    row("c6b_below_0125", str_tostring(below(0.125), "#.##"), "0.12");
    row("c6b_above_0125", str_tostring(above(0.125), "#.##"), "0.13");
    row("c6b_large_1e15", str_tostring(1e15, "#,###"), "1,000,000,000,000,000");
    row("c6b_large_half_1e15", str_tostring(999999999999999.5, "#,###"),
        "1,000,000,000,000,000");
    row("c6b_large_below_1e15", str_tostring(below(999999999999999.5), "#,###"),
        "999,999,999,999,999");
    row("c6b_large_above_1e15", str_tostring(above(999999999999999.5), "#,###"),
        "1,000,000,000,000,000");
    row("c6b_large_1e17", str_tostring(1e17, "#,###"), "100,000,000,000,000,000");
    row("c6b_large_below_1e17", str_tostring(below(1e17), "#,###"), "99,999,999,999,999,984");
    row("c6b_large_above_1e17", str_tostring(above(1e17), "#,###"), "100,000,000,000,000,016");
    row("c6b_negative_tie", str_tostring(-0.125, "#.##"), "-0.13");
    row("c6b_negative_rounded_zero", str_tostring(-std::nextafter(0.005, 0.0), "#.##"), "0");
    row("c6b_negative_005", str_tostring(-0.005, "#.##"), "-0.01");
    row("c6b_negative_zero", str_tostring(-0.0, "#.##"), "0");
    row("c6b_min_fraction", str_tostring(1.2, "#.00"), "1.20");
    row("c6b_default_tostring", str_tostring(0.1234567890123), "0.123456789");
    row("c6b_tostring_percent", str_tostring(1.005, "percent"), "1.01%");
    row("c6b_pattern_percent_below", str_tostring(std::nextafter(0.01005, 0.0), "#.##%"), "1%");
    row("c6b_pattern_percent_tie", str_tostring(0.01005, "#.##%"), "1.01%");
    row("c6b_pattern_percent_above", str_tostring(std::nextafter(0.01005, 1.0), "#.##%"),
        "1.01%");
    row("c6b_preset_percent_below", fmt1("{0,number,percent}", std::nextafter(0.125, 0.0)),
        "12%");
    row("c6b_preset_percent_tie", fmt1("{0,number,percent}", 0.125), "13%");
    row("c6b_currency_below", fmt1("{0,number,currency}", std::nextafter(1.005, 0.0)), "$1.00");
    row("c6b_currency_tie", fmt1("{0,number,currency}", 1.005), "$1.01");
    row("c6b_currency_negative", fmt1("{0,number,currency}", -1234.567), "-$1,234.57");
    row("c6b_volume_below", str_tostring(std::nextafter(1005.0, 0.0), "volume"), "1K");
    row("c6b_volume_tie", str_tostring(1005.0, "volume"), "1.01K");
    row("c6b_volume_above", str_tostring(std::nextafter(1005.0, 2000.0), "volume"), "1.01K");
    row("c6b_volume_small", str_tostring(12.34, "volume"), "12");
    row("c6b_volume_trillion", str_tostring(1e12, "volume"), "1T");
    row("c6b_format_grouped", fmt1("{0}", 1234.56789), "1,234.568");
    row("c6b_nan", str_tostring(nan, "#.##"), "NaN");
    row("c6b_positive_inf", str_tostring(inf, "#.##"), "Infinity");
    row("c6b_negative_inf", str_tostring(-inf, "#.##"), "-Infinity");
    row("c6b_nan_percent_pattern", str_tostring(nan, "#.##%"), "NaN");
}

// The numeric strings of the three C6 exports (BINANCE:ETHUSDT.P, whose tick
// is 0.01), each the Signal text TradingView printed.
void tape_rows() {
    // number_rendering.pine
    row("tape_dflt", str_tostring(1234.56789), "1234.56789");
    row("tape_pattern", str_tostring(1.2, "#.##"), "1.2");
    row("tape_zeros", str_tostring(1.2, "#.00"), "1.20");
    row("tape_percent", str_tostring(1.2345, "percent"), "1.23%");
    row("tape_volume", str_tostring(1250, "volume"), "1.25K");
    row("tape_mintick", str_tostring(1.2345, "mintick", 0.01), "1.23");
    row("tape_fmt_default", fmt1("{0}", 1234.56789), "1,234.568");
    row("tape_fmt_pattern", fmt1("{0,number,#.##}", 1234.56789), "1234.57");
    row("tape_fmt_integer", fmt1("{0,number,integer}", 1234.56789), "1,235");
    row("tape_fmt_percent", fmt1("{0,number,percent}", 0.1234), "12%");
    row("tape_fmt_currency", fmt1("{0,number,currency}", 1234.56789), "$1,234.57");
    row("tape_fmt_quote", fmt1("{0}", 1234.56789), "1,234.568");
    // number_edge.pine
    row("tape_vol2500", str_tostring(2500, "volume"), "2.5K");
    row("tape_vol1000", str_tostring(1000, "volume"), "1K");
    row("tape_vol12", str_tostring(12.34, "volume"), "12");
    row("tape_pc1", str_tostring(1.005, "percent"), "1.01%");
    row("tape_na", str_tostring(na<double>()), "NaN");
    row("tape_prec", str_tostring(0.1234567890123), "0.123456789");
    row("tape_group", str_tostring(1234.56789, "#,###.##"), "1,234.57");
    row("tape_negcurr", fmt1("{0,number,currency}", -1234.567), "-$1,234.57");
    row("tape_negpc", fmt1("{0,number,percent}", -0.1234), "-12%");
    row("tape_quoted", fmt1("'{0}' {0}", 2.5), "{0} 2.5");
    row("tape_fmtzero", fmt1("{0,number,#.00}", 1.2), "1.20");
    row("tape_bool", str_format_values("{0}", {true}), "true");
    // number_pattern.pine
    row("tape_mintick_trail", str_tostring(1.2, "mintick", 0.01), "1.20");
    row("tape_mintick_int", str_tostring(2, "mintick", 0.01), "2.00");
    row("tape_tostring_pcpat", str_tostring(0.1234, "#.##%"), "12.34%");
    row("tape_format_pcpat", fmt1("{0,number,#.##%}", 0.1234), "12.34%");
    row("tape_format_quote2", fmt1("''{0}", 2.5), "'2.5");
}

void text_rows() {
    row("text_basic", str_format("{0} is {1}", {"Hello", "world"}), "Hello is world");
    row("text_repeated", str_format("{0} and {0}", {"X"}), "X and X");
    row("text_missing", str_format("{0}", {}), "{0}");
    row("text_quoted", str_format("'{0}' {0}", {"a"}), "{0} a");
    row("text_quote_pair", str_format("it''s {0}", {"a"}), "it's a");
    row("text_style_on_text", str_format("{0,number,#.##} and {1}", {"1.2345", "x"}),
        "1.2345 and x");
    row("text_no_resubstitution", str_format("{0}{1}", {"{1}", "x"}), "{1}x");
    row("text_unclosed", str_format("a {0", {"x"}), "a {0");
    row("text_not_an_index", str_format("{a} {0}", {"x"}), "{a} x");
    row("text_index_blanks", str_format("{ 0 }", {"x"}), "x");
    row("text_huge_index", str_format("{123456789012345678901234567890}", {"x"}),
        "{123456789012345678901234567890}");
    row("values_mixed", str_format_values("{0}: {1,number,integer} ({2})",
                                          {"lots", 12345.5, false}), "lots: 12,346 (false)");
    row("values_other_type", str_format_values("{0,date,short}", {1.5}), "{0,date,short}");
    row("values_int_na", str_format_values("{0}", {na<int>()}), "NaN");
    row("values_unsigned_zero", str_format_values("{0}", {0u}), "0");
}

// Pinned from 8cf3be58's str_tostring(value, "mintick", tick).
void mintick_rows() {
    row("mintick_basic", str_tostring(1.23456, "mintick", 0.01), "1.23");
    row("mintick_round_up", str_tostring(1.236, "mintick", 0.01), "1.24");
    // The mode counts one decimal for a 0.25 tick (0.25 * 10 is not below
    // one): kept byte-identical, see the lane report.
    row("mintick_quarter", str_tostring(0.3, "mintick", 0.25), "0.2");
    row("mintick_half", str_tostring(12345.678, "mintick", 0.5), "12345.5");
    row("mintick_negative", str_tostring(-2.345, "mintick", 0.01), "-2.35");
    row("mintick_negative_zero", str_tostring(-0.001, "mintick", 0.01), "-0.00");
    row("mintick_whole", str_tostring(1837.6, "mintick", 1.0), "1838");
    row("mintick_fine", str_tostring(0.000123456, "mintick", 0.00001), "0.00012");
    row("mintick_large", str_tostring(123456789.126, "mintick", 0.01), "123456789.13");
    row("mintick_nan", str_tostring(na<double>(), "mintick", 0.01), "NaN");
}

}  // namespace

int main(int argc, char** argv) {
    c6b_battery();
    tape_rows();
    text_rows();
    mintick_rows();
    int failures = 0;
    for (const auto& r : rows) {
        const bool ok = r.got == r.want;
        if (!ok) ++failures;
        std::printf("%s %-32s %s%s%s\n", ok ? "ok  " : "FAIL", r.name.c_str(), r.got.c_str(),
                    ok ? "" : "  want ", ok ? "" : r.want.c_str());
    }
    if (argc > 1) {
        std::ofstream out(argv[1], std::ios::binary);
        for (const auto& r : rows) out << r.name << '\t' << r.got << '\n';
    }
    std::printf("test_str_number_format: %zu rows, %d failures\n", rows.size(), failures);
    return failures == 0 ? 0 : 1;
}
