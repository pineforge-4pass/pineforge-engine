// Stable run-failure codes (include/pineforge/run_failure.hpp) through real
// generated strategies and the C ABI.
//
// Each row runs a strategy codegen emitted (tests/fixtures/run_failure_codes,
// compiled into this binary by run_failure_codes_strategies.hpp) through the C
// entry points a harness calls, and asserts four things a consumer reads after
// a run: the code (strategy_get_last_error_code), its arguments as canonical JSON
// (strategy_get_last_error_args), the English (strategy_get_last_error) and the
// run status (strategy_last_run_status), plus the report's trade count. The
// English, the status and the trade count are the ones the base engine
// (7a1f01c0, before run-failure codes existed) reported for the same run: the
// codes ride beside the text and change nothing else.
//
// Native hosts (no Pine source layer) then drive the kernel's own bar preflight
// and the callback catch sites that wrote no text before. Without a run: every
// generated-code helper, the field -> code table of the adapter's run-spec
// refusal for every NativeRunSpecField, and the registry against
// docker/run_failure_codes.json.

#include "run_failure_codes_strategies.hpp"

#include <pineforge/checked_settings.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_module.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/run_failure.hpp>

#include "../src/source/pine_run_failure.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_RUN_FAILURE_CATALOG
#error "PINEFORGE_RUN_FAILURE_CATALOG must name docker/run_failure_codes.json"
#endif

namespace {

using pineforge::RunFailureArg;
using pineforge::RunFailureArgs;
using pineforge::RunFailureClass;
using pineforge::RunFailureCode;
using pineforge::RunFailureValue;

int g_checks = 0;
int g_failures = 0;

std::string json_quote(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7F) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

void check(bool ok, const std::string& what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what.c_str());
}

void check_eq(const std::string& got, const std::string& want, const std::string& what,
              int line) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n  got:  %s\n  want: %s\n", line, what.c_str(),
                 json_quote(got).c_str(), json_quote(want).c_str());
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(what, got, want) check_eq((got), (want), (what), __LINE__)

std::string code_name(RunFailureCode code) { return pineforge::run_failure_code_name(code); }

// A value's arguments as the C getter spells them: "{}" when there are none.
std::string args_json(const RunFailureValue& value) {
    return value.args ? *value.args : std::string("{}");
}

// ---------------------------------------------------------------------------
// Strategy rows.

struct Observed {
    std::string text, code, args;
    bool text_null = false, code_null = false, args_null = false;
    int status = -9;
    int trades = -1;
};

struct Expected {
    const char* code;
    const char* args;
    // strategy_get_last_error: byte for byte what base 7a1f01c0 printed for the
    // same run, except the native-host rows that name new English (base
    // printed "" there: a silent failure).
    const char* text;
    int status;  // strategy_last_run_status, as base reported it
    int trades;  // the report's total_trades, as base reported it (-1: no report)
};

Observed observe(void* s) {
    Observed o;
    const char* text = strategy_get_last_error(s);
    const char* code = strategy_get_last_error_code(s);
    const char* args = strategy_get_last_error_args(s);
    o.text_null = text == nullptr;
    o.code_null = code == nullptr;
    o.args_null = args == nullptr;
    o.text = text ? text : "";
    o.code = code ? code : "";
    o.args = args ? args : "";
    o.status = strategy_last_run_status(s);
    return o;
}

// One handle of one generated strategy, freed at scope exit.
struct Handle {
    const rfc::Strategy& strategy;
    void* s;
    explicit Handle(const rfc::Strategy& which) : strategy(which), s(which.create(nullptr)) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (s) strategy.free(s);
    }
    void input(const char* name, const char* value) const { strategy.set_input(s, name, value); }
    Observed run(std::vector<pineforge::Bar> bars, const char* input_tf,
                 const char* script_tf) const {
        pineforge::ReportC report{};
        strategy.run_full(s, bars.data(), static_cast<int>(bars.size()), input_tf, script_tf, 0, 4,
                          PF_MAGNIFIER_ENDPOINTS, &report);
        Observed o = observe(s);
        o.trades = report.total_trades;
        strategy.report_free(&report);
        return o;
    }
};

void expect_row(const char* row, const Observed& got, const Expected& want) {
    std::printf("row %-38s code=%s args=%s status=%d trades=%d text=%s\n", row, got.code.c_str(),
                got.args.c_str(), got.status, got.trades, json_quote(got.text).c_str());
    const std::string at = std::string(row) + ": ";
    // A live handle never answers NULL.
    CHECK(!got.text_null && !got.code_null && !got.args_null);
    CHECK_EQ(at + "code", got.code, want.code);
    CHECK_EQ(at + "args", got.args, want.args);
    CHECK_EQ(at + "English", got.text, want.text);
    CHECK_EQ(at + "run status (base 7a1f01c0)", std::to_string(got.status),
             std::to_string(want.status));
    if (want.trades >= 0) {
        CHECK_EQ(at + "report trades (base 7a1f01c0)", std::to_string(got.trades),
                 std::to_string(want.trades));
    }
}

const char* const kNoDataTail = ": no data is pinned for this request, and its value was read";

std::string stop_text(const std::string& call, int line) {
    return call + " at line " + std::to_string(line) + kNoDataTail;
}

// runtime.error carrying the English of every forgeable class
// still reads strategy_runtime_error, whatever it says -- the empty message
// included, whose text is "" while the code says a run failed.
void forged_runtime_error_rows(const std::vector<pineforge::Bar>& minute) {
    const std::string no_data_shape =
        stop_text(R"(request.security("BINANCE:ETHUSDT", "60", ...))", 3);
    const struct {
        const char* row;
        const char* forged_case;
        std::string text;
    } rows[] = {
        {"forged_native_prefix", "1", "native x"},
        {"forged_bad_alloc", "2", "std::bad_alloc"},
        {"forged_no_data_stop", "3", no_data_shape},
        {"forged_symbol_feeds", "4", "--symbol-feeds: x"},
        {"forged_spec_field_13", "5", "Pine adapter produced invalid native run spec field 13"},
        {"forged_empty_message", "6", ""},
    };
    for (const auto& row : rows) {
        Handle h(rfc::kForged);
        h.input("Case", row.forged_case);
        expect_row(row.row, h.run(minute, "1", "1"),
                   {"strategy_runtime_error", "{}", row.text.c_str(), 1, 1});
    }
}

// recalc_cap. The first-open loop guard of pine_scheduler_native.cpp
// ("Pine COOF first-open loop guard exhausted") needs more than 1 << 20 market
// orders accepted in ONE first-open recalculation, and accepting and executing
// them costs quadratic time (10,000 took 24.5 s): no script reaches it. A
// calc_on_order_fills strategy that reverses on every fill advances along the
// bar's path and completes; the kernel's own recalculation cap only skips.
void calc_on_order_fills_rows(const std::vector<pineforge::Bar>& minute) {
    Handle h(rfc::kCoof);
    expect_row("coof_runaway_completes", h.run(minute, "1", "1"), {"", "", "", 0, 236});
    // The guard's exception as its site throws it: still a std::overflow_error
    // with the same text, read as recalc_cap.
    try {
        throw pineforge::coded<std::overflow_error>(RunFailureCode::recalc_cap, {},
                                                    "Pine COOF first-open loop guard exhausted");
    } catch (const std::overflow_error& error) {
        const RunFailureValue value = pineforge::classify_run_failure(error);
        CHECK_EQ("recalc_cap site: code", code_name(value.code), "recalc_cap");
        CHECK_EQ("recalc_cap site: args", args_json(value), "{}");
        CHECK_EQ("recalc_cap site: what()", error.what(),
                 "Pine COOF first-open loop guard exhausted");
    }
}

// A syminfo session the run spec cannot parse (field 13)
// and a duplicate chart timestamp are refused before the run begins (status
// 0); after the duplicate, a valid run on the same handle clears the record.
void refused_begin_rows(const std::vector<pineforge::Bar>& minute) {
    {
        Handle h(rfc::kForged);
        h.input("Case", "0");
        strategy_set_syminfo_session(h.s, "bogus");
        expect_row("spec_field_13_unparseable_session", h.run(minute, "1", "1"),
                   {"symbol_metadata_rejected", R"({"field":"session"})",
                    "Pine adapter produced invalid native run spec field 13", 0, 0});
    }
    {
        // Field 22 (QuantityGrid, lot_grid_rejected) has no C ABI road: the
        // setter keeps only a finite positive qty_step and drops the rest, so
        // the projected spec never carries a grid the validator refuses. The
        // field's code is pinned by run_spec_field_rows below.
        Handle h(rfc::kForged);
        h.input("Case", "0");
        strategy_set_syminfo_metadata(h.s, "qty_step", -1.0);
        expect_row("bad_qty_step_dropped_not_refused", h.run(minute, "1", "1"),
                   {"", "", "", 0, 30});
    }
    {
        Handle h(rfc::kForged);
        h.input("Case", "0");
        expect_row("duplicate_chart_timestamp", h.run(rfc::make_bars(60, 60000, 5), "1", "1"),
                   {"chart_bars_rejected",
                    R"({"field":"timestamp","index":5,"reason":"not_increasing"})",
                    "bar[5].timestamp must be strictly increasing", 0, 0});
        // The next successful run on the same handle clears the record.
        expect_row("success_after_coded_failure", h.run(minute, "1", "1"), {"", "", "", 0, 30});
    }
    {
        // One handle: the session refusal, then a script whose runtime.error
        // prints that refusal's English. The second run reads
        // strategy_runtime_error, never the earlier record's code.
        Handle h(rfc::kForged);
        h.input("Case", "0");
        strategy_set_syminfo_session(h.s, "bogus");
        expect_row("same_handle_session_refusal", h.run(minute, "1", "1"),
                   {"symbol_metadata_rejected", R"({"field":"session"})",
                    "Pine adapter produced invalid native run spec field 13", 0, 0});
        strategy_set_syminfo_session(h.s, "24x7");
        h.input("Case", "5");
        expect_row("same_handle_forged_refusal_text", h.run(minute, "1", "1"),
                   {"strategy_runtime_error", "{}",
                    "Pine adapter produced invalid native run spec field 13", 1, -1});
    }
}

// A request.security of another symbol on an aggregated
// chart, and a request.security finer than the input, both refused while the
// script is prepared: the lifecycle completes (status 0, an empty report), and
// the text and the code say the run failed while the status does not.
void preparation_rows(const std::vector<pineforge::Bar>& minute,
                      const std::vector<pineforge::Bar>& five) {
    {
        Handle h(rfc::kForeign);
        CHECK(strategy_set_symbol_feed(h.s, "BINANCE:ETHUSDT", "60", nullptr, nullptr, 0) == 0);
        expect_row("foreign_chart_input_aggregated", h.run(minute, "1", "5"),
                   {"symbol_feeds_refused",
                    R"({"input_tf":"1","reason":"chart_input_aggregated","script_tf":"5"})",
                    "request.security of another symbol needs the chart's own bars as input; "
                    "input '1' aggregated to chart '5' is not supported",
                    0, 0});
    }
    {
        Handle h(rfc::kFiner);
        expect_row("preparation_failure_completes", h.run(five, "5", "5"),
                   {"request_timeframe_unsupported",
                    R"({"input_tf":"5","reason":"finer_than_input"})",
                    "request.security: requested timeframe '1' is finer than input '5'. Use "
                    "request.security_lower_tf for sub-input timeframes.",
                    0, 0});
    }
}

// matrix.add_col on an empty matrix<int> is a std::logic_error,
// which the classifier's type fallback would read as engine_invariant: the
// code is the throw site's own.
void matrix_rows(const std::vector<pineforge::Bar>& minute) {
    Handle h(rfc::kMatrix);
    expect_row("matrix_add_col_empty", h.run(minute, "1", "1"),
               {"pine_matrix_error", R"({"function":"matrix.add_col","reason":"empty_matrix"})",
                "matrix.add_col on empty matrix: use add_row first", 1, 1});
    try {
        auto matrix = pineforge::PineGenericMatrix<int>::new_(0, 0, 0);
        matrix.add_col(0, std::vector<int>{1});
        CHECK(false && "matrix.add_col on an empty matrix did not throw");
    } catch (const std::logic_error& error) {
        const RunFailureValue value = pineforge::classify_run_failure(error);
        CHECK_EQ("matrix.add_col: still a std::logic_error, code", code_name(value.code),
                 "pine_matrix_error");
        CHECK_EQ("matrix.add_col: what()", error.what(),
                 "matrix.add_col on empty matrix: use add_row first");
    }
}

// The generated no-data and other-symbol stops, hand-edited into
// the helpers (fixtures/run_failure_codes/helper_stops/provenance.json), carry
// their call spelling, function and line; a function outside the helper's
// closed list cannot mint a code.
void helper_stop_rows(const std::vector<pineforge::Bar>& minute) {
    const std::string aapl = R"(request.financial("NASDAQ:AAPL", "TOTAL_REVENUE", ...))";
    const std::string eth = R"(request.security("BINANCE:ETHUSDT", "60", ...))";
    const std::string sol =
        R"(request.security(input.symbol("BINANCE:SOLUSDT", "Other") at line 6, "60", ...))";
    const std::string msft = R"(request.financial("NASDAQ:MSFT", "TOTAL_REVENUE", ...))";
    const struct {
        const char* row;
        const char* stop_case;
        const char* code;
        std::string args;
        std::string text;
    } rows[] = {
        {"helper_no_data_request", "1", "no_data_request",
         R"js({"call":"request.financial(\"NASDAQ:AAPL\", \"TOTAL_REVENUE\", ...)",)js"
         R"js("function":"request.financial","line":4})js",
         stop_text(aapl, 4)},
        {"helper_other_symbol_literal", "2", "other_symbol_request",
         R"js({"call":"request.security(\"BINANCE:ETHUSDT\", \"60\", ...)",)js"
         R"js("function":"request.security","line":5,"symbol":"BINANCE:ETHUSDT"})js",
         stop_text(eth, 5)},
        {"helper_other_symbol_computed", "3", "other_symbol_request",
         R"js({"call":"request.security(input.symbol(\"BINANCE:SOLUSDT\", \"Other\") at line 6, )js"
         R"js(\"60\", ...)","function":"request.security","line":6})js",
         stop_text(sol, 6)},
        {"helper_function_outside_list", "4", "engine_invariant", "{}", stop_text(msft, 7)},
    };
    for (const auto& row : rows) {
        Handle h(rfc::kHelpers);
        h.input("Case", row.stop_case);
        expect_row(row.row, h.run(minute, "1", "1"),
                   {row.code, row.args.c_str(), row.text.c_str(), 1, 1});
    }
}

// Every generated-code helper of run_failure.hpp, raised directly: the same
// std::runtime_error with the English as what(), the helper's own code with
// its arguments, and engine_invariant for any value outside a closed list.
void helper_contract_rows() {
    struct Row {
        const char* row;
        std::function<void()> raise;
        const char* code;
        const char* args;
    };
    using namespace pineforge;
    const Row rows[] = {
        {"pine_runtime_error", [] { pine_runtime_error("E"); }, "strategy_runtime_error", "{}"},
        {"pine_no_data_stop", [] { pine_no_data_stop("request.earnings", "c(\"x\")", 9, "E"); },
         "no_data_request", R"js({"call":"c(\"x\")","function":"request.earnings","line":9})js"},
        {"pine_no_data_stop outside list",
         [] { pine_no_data_stop("request.security", "c", 9, "E"); }, "engine_invariant", "{}"},
        // Canonical escaping: quote and backslash escaped, control bytes as
        // six-character escapes, a byte that is not well-formed UTF-8 as
        // U+FFFD, the rest kept.
        {"pine_no_data_stop escaping",
         [] { pine_no_data_stop("request.earnings", "q\"\\\x01\x7f\xc3(\xe2\x82\xac", 9, "E"); },
         "no_data_request",
         "{\"call\":\"q\\\"\\\\\\u0001\\u007f\xEF\xBF\xBD(\xE2\x82\xAC\",\"function\":"
         "\"request.earnings\",\"line\":9}"},
        {"pine_other_symbol_stop symbol",
         [] { pine_other_symbol_stop("request.security_lower_tf", "NYSE:IBM", "c", 3, "E"); },
         "other_symbol_request",
         R"({"call":"c","function":"request.security_lower_tf","line":3,"symbol":"NYSE:IBM"})"},
        {"pine_other_symbol_stop no symbol",
         [] { pine_other_symbol_stop("request.security", nullptr, "c", 3, "E"); },
         "other_symbol_request", R"({"call":"c","function":"request.security","line":3})"},
        {"pine_other_symbol_stop outside list",
         [] { pine_other_symbol_stop("request.financial", nullptr, "c", 3, "E"); },
         "engine_invariant", "{}"},
        {"pine_array_stop method",
         [] { pine_array_stop("index_out_of_bounds", "get", "E"); }, "pine_array_error",
         R"({"method":"get","reason":"index_out_of_bounds"})"},
        {"pine_array_stop no method", [] { pine_array_stop("empty_array_access", nullptr, "E"); },
         "pine_array_error", R"({"reason":"empty_array_access"})"},
        {"pine_array_stop outside list", [] { pine_array_stop("off_by_one", "get", "E"); },
         "engine_invariant", "{}"},
        {"pine_collection_stop na", [] { pine_collection_stop("matrix", "na_reference", "E"); },
         "pine_na_reference", R"({"object":"matrix"})"},
        {"pine_collection_stop historical",
         [] { pine_collection_stop("array", "historical_modified", "E"); }, "pine_array_error",
         R"({"collection":"array","reason":"historical_modified"})"},
        {"pine_collection_stop outside list",
         [] { pine_collection_stop("map", "historical_modified", "E"); }, "engine_invariant",
         "{}"},
        {"pine_collection_stop na outside list",
         [] { pine_collection_stop("map", "na_reference", "E"); }, "engine_invariant", "{}"},
        {"pine_collection_stop unknown reason",
         [] { pine_collection_stop("array", "stale", "E"); }, "engine_invariant", "{}"},
        {"pine_na_stop", [] { pine_na_stop("udt_object", "E"); }, "pine_na_reference",
         R"({"object":"udt_object"})"},
        {"pine_na_stop outside list", [] { pine_na_stop("struct", "E"); }, "engine_invariant",
         "{}"},
        {"pine_limit_stop max", [] { pine_limit_stop("map_pairs", 50000, "E"); },
         "pine_runtime_limit", R"({"limit":"map_pairs","max":50000})"},
        {"pine_limit_stop no max", [] { pine_limit_stop("udt_objects", -1, "E"); },
         "pine_runtime_limit", R"({"limit":"udt_objects"})"},
        {"pine_limit_stop outside list", [] { pine_limit_stop("loop_iterations", 5, "E"); },
         "engine_invariant", "{}"},
        {"pine_unsupported_stop line",
         [] { pine_unsupported_stop("nested_heikinashi_request", 12, "E"); },
         "request_unsupported", R"({"line":12,"reason":"nested_heikinashi_request"})"},
        {"pine_unsupported_stop no line",
         [] { pine_unsupported_stop("lower_tf_lookahead_or_gaps", -1, "E"); },
         "request_unsupported", R"({"reason":"lower_tf_lookahead_or_gaps"})"},
        {"pine_unsupported_stop outside list", [] { pine_unsupported_stop("tick_charts", 1, "E"); },
         "engine_invariant", "{}"},
        {"pine_string_stop", [] { pine_string_stop("substring_out_of_range", "E"); },
         "pine_string_error", R"({"reason":"substring_out_of_range"})"},
        {"pine_string_stop outside list", [] { pine_string_stop("bad_utf8", "E"); },
         "engine_invariant", "{}"},
        {"pine_engine_invariant", [] { pine_engine_invariant("E"); }, "engine_invariant", "{}"},
    };
    for (const Row& row : rows) {
        const std::string at = std::string("helper ") + row.row + ": ";
        bool thrown = false;
        try {
            row.raise();
        } catch (const std::runtime_error& error) {
            thrown = true;
            const RunFailureValue value = pineforge::classify_run_failure(error);
            CHECK_EQ(at + "code", code_name(value.code), row.code);
            CHECK_EQ(at + "args", args_json(value), row.args);
            CHECK_EQ(at + "what()", error.what(), "E");
        }
        CHECK_EQ(at + "throws a std::runtime_error", thrown ? "thrown" : "not thrown", "thrown");
    }
    std::printf("helpers: %zu helper calls checked\n", std::size(rows));
}

// The record's lifetime on the C ABI.
void record_lifecycle_rows(const std::vector<pineforge::Bar>& minute) {
    CHECK(strategy_get_last_error(nullptr) == nullptr);
    CHECK(strategy_get_last_error_code(nullptr) == nullptr);
    CHECK(strategy_get_last_error_args(nullptr) == nullptr);
    {
        Handle h(rfc::kForged);
        Observed fresh = observe(h.s);
        fresh.trades = 0;
        expect_row("fresh_handle", fresh, {"", "", "", 0, 0});
    }
    {
        // A failed run latches its host: the next run on the handle is refused
        // at configure ("native host already failed", engine_invariant), and
        // that refusal's code -- not the earlier failure's -- rides the text
        // the source layer writes over it.
        Handle h(rfc::kForged);
        h.input("Case", "1");
        expect_row("reuse_failed_handle_first_run", h.run(minute, "1", "1"),
                   {"strategy_runtime_error", "{}", "native x", 1, 1});
        h.input("Case", "0");
        expect_row("reuse_failed_handle_refused", h.run(minute, "1", "1"),
                   {"engine_invariant", "{}",
                    "Pine native adapter failed to configure projected run spec", 1, 0});
    }
    {
        // The generated wrapper's contract (note_run_failure with an entrypoint).
        Handle h(rfc::kForged);
        auto& engine = *static_cast<pineforge::BacktestEngine*>(h.s);
        try {
            pineforge::pine_no_data_stop("request.splits", "c", 2, "what");
        } catch (const std::exception& error) {
            pineforge::note_run_failure(engine, "run_backtest_full", error);
        }
        Observed noted = observe(h.s);
        noted.trades = 0;
        expect_row("wrapper_noted_exception", noted,
                   {"no_data_request", R"({"call":"c","function":"request.splits","line":2})",
                    "run_backtest_full: what", 0, 0});
        pineforge::note_run_failure_unknown(engine, "run_backtest_full");
        Observed unknown = observe(h.s);
        unknown.trades = 0;
        expect_row("wrapper_noted_unknown", unknown,
                   {"engine_unclassified_error", "{}",
                    "run_backtest_full: unknown C++ exception", 0, 0});
        pineforge::clear_run_failure(engine);
        Observed cleared = observe(h.s);
        cleared.trades = 0;
        expect_row("wrapper_cleared", cleared, {"", "", "", 0, 0});
    }
}

// A legacy setter the generated strategy latched (an override value std::stod
// cannot read) refuses the run where the script is prepared: the latched text
// (std::stod's what() differs between C++ libraries, so only its stable prefix
// is pinned), coded setting_rejected without arguments -- the engine knows the
// failure only by its type. The run fails (status 1) and reports nothing.
void latched_setting_rows(const std::vector<pineforge::Bar>& minute) {
    Handle h(rfc::kForged);
    h.input("Case", "0");
    rfc_forged::rfc_forged_strategy_set_override(h.s, "initial_capital", "not a number");
    const Observed got = h.run(minute, "1", "1");
    std::printf("row %-38s code=%s args=%s status=%d trades=%d text=%s\n",
                "legacy_setter_latched", got.code.c_str(), got.args.c_str(), got.status,
                got.trades, json_quote(got.text).c_str());
    CHECK(!got.text_null && !got.code_null && !got.args_null);
    CHECK_EQ("legacy_setter_latched: code", got.code, "setting_rejected");
    CHECK_EQ("legacy_setter_latched: args", got.args, "{}");
    CHECK(got.text.rfind("strategy_set_override: stod", 0) == 0);
    CHECK_EQ("legacy_setter_latched: run status", std::to_string(got.status), "1");
    CHECK_EQ("legacy_setter_latched: report trades", std::to_string(got.trades), "0");
}

// The generated strategy's legacy setter writes its text before any run, as
// the transpiler that emitted the fixture did, without a code: the getters
// read engine_unclassified_error for a text no coded site wrote.
void uncoded_writer_rows() {
    Handle h(rfc::kForged);
    rfc_forged::rfc_forged_strategy_set_override(h.s, "initial_capital", "not a number");
    Observed got = observe(h.s);
    got.trades = 0;
    std::printf("row %-38s code=%s args=%s status=%d text=%s\n", "uncoded_setter_text",
                got.code.c_str(), got.args.c_str(), got.status, json_quote(got.text).c_str());
    CHECK(!got.text_null && !got.code_null && !got.args_null);
    CHECK(got.text.rfind("strategy_set_override: stod", 0) == 0);
    CHECK_EQ("uncoded_setter_text: code", got.code, "engine_unclassified_error");
    CHECK_EQ("uncoded_setter_text: args", got.args, "{}");
    CHECK_EQ("uncoded_setter_text: status", std::to_string(got.status), "0");
}

// The latched setter failure as the transpiler's run-failure-codes emission
// spells it (the contract in run_failure.hpp): its own type derives from
// LatchedSettingsFailure and RunFailureInfo, a RunFailureInfo member is built
// with the (code, args) constructor and copy-assigned, and the exception is
// thrown where the script is prepared. The engine rethrows it as is, so its
// arguments reach the getters.
class CodedLatchedSettingStrategy final : public rfc_forged::GeneratedStrategy {
public:
    struct Latched : pineforge::checked_settings::LatchedSettingsFailure,
                     pineforge::RunFailureInfo {
        Latched(const std::string& text, const pineforge::RunFailureInfo& info)
            : pineforge::checked_settings::LatchedSettingsFailure(text),
              pineforge::RunFailureInfo(info) {}
    };
    pineforge::RunFailureInfo info{RunFailureCode::none, {}};
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        info = pineforge::RunFailureInfo(
            RunFailureCode::setting_rejected,
            {{"entrypoint", "strategy_set_input"}, {"reason", "expected_integer"}});
        throw Latched("strategy_set_input: Case: expected an integer", info);
    }
};

void coded_latched_setting_rows(const std::vector<pineforge::Bar>& minute) {
    auto* strategy = new CodedLatchedSettingStrategy();
    void* s = static_cast<rfc_forged::GeneratedStrategy*>(strategy);
    std::vector<pineforge::Bar> bars = minute;
    pineforge::ReportC report{};
    rfc::kForged.run_full(s, bars.data(), static_cast<int>(bars.size()), "1", "1", 0, 4,
                          PF_MAGNIFIER_ENDPOINTS, &report);
    Observed got = observe(s);
    got.trades = report.total_trades;
    rfc::kForged.report_free(&report);
    expect_row("coded_latched_setting", got,
               {"setting_rejected",
                R"({"entrypoint":"strategy_set_input","reason":"expected_integer"})",
                "strategy_set_input: Case: expected an integer", 1, 0});
    CHECK_EQ("coded_latched_setting: member", code_name(strategy->info.run_failure().code),
             "setting_rejected");
    rfc::kForged.free(s);
}

// A native host that records its own failure and then throws a non-standard
// exception from on_native_applied: the catch-all keeps the record, an empty
// text included, rather than writing "native applied callback exception".
class RecordThenThrowHost final : public pineforge::NativeStrategyHost {
public:
    const char* text = "";
    int bars = 0;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (bars++ == 0) {
            submit_market(pineforge::native_order::Request{
                pineforge::order_action::Transact{1.0}, "buy", ""});
        }
    }
    void on_native_applied(const pineforge::native_order::ExecutionAppliedEvent&,
                           const pineforge::NativeDecisionContext&) override {
        pineforge::note_run_failure(*this, text, RunFailureCode::strategy_runtime_error);
        throw 42;
    }
};

// A failure inside execute_current, here the host's terms hook running out of
// memory while the request settles: the frame keeps its own text and carries
// that failure's code.
class TermsThrowHost final : public pineforge::NativeStrategyHost {
public:
    int bars = 0;
    bool executed = false;
    pineforge::native_order::ExecutionTerms resolve_execution_terms(
            const pineforge::NativeExecutionTermsFacts&) const override {
        throw std::bad_alloc();
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (bars++ != 0) return;
        const auto submitted = submit_market(pineforge::native_order::Request{
            pineforge::order_action::Transact{1.0}, "buy", ""});
        if (!submitted.handle) return;
        executed = true;
        (void)execute_current(pineforge::NativeCurrentExecution{
            *submitted.handle, pineforge::NativeCurrentPriceRule::AsPresented});
    }
};

// A native host whose bar callback throws a standard exception: the code is
// the classifier's answer by type.
class StdThrowHost final : public pineforge::NativeStrategyHost {
public:
    bool bad_alloc = true;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (bad_alloc) throw std::bad_alloc();
        throw std::logic_error("host invariant");
    }
};

void recorded_failure_rows() {
    using pineforge::NativeSetupStatus;
    const auto bars = rfc::native_bars();
    for (const char* text : {"host stop", ""}) {
        RecordThenThrowHost host;
        host.text = text;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-record-then-throw")).status
              == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        expect_row(*text ? "native_recorded_then_nonstd_throw"
                         : "native_recorded_empty_then_nonstd_throw",
                   observe(rfc::as_handle(host)), {"strategy_runtime_error", "{}", text, 1, -1});
    }
    {
        // A C entry point refused on a host whose run failed keeps the run's
        // own text and code.
        rfc::AppliedThrowHost host;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-failed-setter")).status
              == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK_EQ("setter on a failed host",
                 std::to_string(strategy_set_native_security_feed(rfc::as_handle(host), "5",
                                                                  nullptr, 0)),
                 "-1");
        expect_row("native_failed_host_setter_keeps_run", observe(rfc::as_handle(host)),
                   {"engine_unclassified_error", "{}", "native applied callback exception", 1,
                    -1});
    }
    {
        TermsThrowHost host;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-terms-throw")).status
              == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.executed);
        expect_row("native_current_execution_carries_cause", observe(rfc::as_handle(host)),
                   {"out_of_memory", "{}", "native current execution failed", 1, -1});
    }
    for (const bool bad_alloc : {true, false}) {
        StdThrowHost host;
        host.bad_alloc = bad_alloc;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-std-throw")).status
              == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        if (bad_alloc) {
            expect_row("native_bar_bad_alloc", observe(rfc::as_handle(host)),
                       {"out_of_memory", "{}", "std::bad_alloc", 1, -1});
        } else {
            expect_row("native_bar_logic_error", observe(rfc::as_handle(host)),
                       {"engine_invariant", "{}", "host invariant", 1, -1});
        }
    }
}

// A stream entry point's refusal: a confirmed bar pushed out of order after
// the warmup returns -1 and leaves an input-class code beside its text.
void stream_refusal_rows(const std::vector<pineforge::Bar>& minute) {
    Handle h(rfc::kForged);
    h.input("Case", "0");
    const auto* warmup = reinterpret_cast<const pf_bar_t*>(minute.data());
    CHECK_EQ("stream begin", std::to_string(strategy_stream_begin(h.s, warmup, 10, "1", "1")),
             "0");
    pf_bar_t stale{};
    std::memcpy(&stale, &minute[3], sizeof stale);
    CHECK_EQ("stream push out of order", std::to_string(strategy_stream_push_bar(h.s, &stale)),
             "-1");
    Observed got = observe(h.s);
    got.trades = -1;
    expect_row("stream_bar_out_of_order", got,
               {"stream_input_rejected", "{}",
                "native confirmed bar timestamp is out of order or off the input grid", 1, -1});
}

// An external native module's text-only note_error, the form it had before
// run-failure codes, still compiles and reads engine_unclassified_error.
class ModuleHost : public pineforge::NativeStrategyHost {
public:
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

void native_module_rows() {
    pineforge::native_module::Module<ModuleHost> module;
    CHECK(module.configure_native(rfc::native_spec("rfc-native-module")).status
          == pineforge::NativeSetupStatus::Applied);
    module.note_error("module text");
    Observed member = observe(rfc::as_handle(module));
    member.trades = 0;
    expect_row("native_module_note_error_text", member,
               {"engine_unclassified_error", "{}", "module text", 0, 0});
    pineforge::native_module::note_error<ModuleHost>(rfc::as_handle(module), "module free text");
    Observed free_form = observe(rfc::as_handle(module));
    free_form.trades = 0;
    expect_row("native_module_free_note_error_text", free_form,
               {"engine_unclassified_error", "{}", "module free text", 0, 0});
}

// The kernel without a Pine source layer (native hosts): its own bar
// preflight, and the catch (...) sites that latched a failure with no text
// before run-failure codes (base 7a1f01c0 reported "" for these runs; the
// texts are new English, engine_unclassified_error).
void native_host_rows() {
    using pineforge::NativeSetupStatus;
    {
        rfc::QuietHost host;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-preflight")).status
              == NativeSetupStatus::Applied);
        auto bars = rfc::native_bars();
        bars[1].timestamp = bars[0].timestamp;
        host.run(bars.data(), static_cast<int>(bars.size()));
        expect_row("native_preflight_duplicate_timestamp", observe(rfc::as_handle(host)),
                   {"chart_bars_rejected",
                    R"({"field":"timestamp","index":1,"reason":"not_increasing"})",
                    "bar[1].timestamp must be strictly increasing", 1, -1});
    }
    {
        rfc::AppliedThrowHost host;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-applied")).status
              == NativeSetupStatus::Applied);
        const auto bars = rfc::native_bars();
        host.run(bars.data(), static_cast<int>(bars.size()));
        expect_row("native_applied_callback_nonstd_throw", observe(rfc::as_handle(host)),
                   {"engine_unclassified_error", "{}", "native applied callback exception", 1,
                    -1});
    }
    {
        rfc::RecalcThrowHost host;
        auto spec = rfc::native_spec("rfc-native-recalculation");
        spec.calculation = pineforge::NativeCalculationTrigger::BarCloseAndFills;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const auto bars = rfc::native_bars();
        host.run(bars.data(), static_cast<int>(bars.size()));
        expect_row("native_recalculation_nonstd_throw", observe(rfc::as_handle(host)),
                   {"engine_unclassified_error", "{}", "native recalculation callback exception",
                    1, -1});
    }
    {
        // The C setter on the host's own handle from inside the run: -1, and
        // the refusal it used to swallow is now the handle's failure. The
        // refusal latches without fail(), so the run status stays 0, as on
        // base, where this run read as a success with an empty text.
        rfc::MutationHost host;
        Observed at_setter;
        host.after_setter = [](pf_strategy_t s, void* context) {
            *static_cast<Observed*>(context) = observe(s);
        };
        host.after_setter_context = &at_setter;
        CHECK(host.configure_native(rfc::native_spec("rfc-native-mutation")).status
              == NativeSetupStatus::Applied);
        const auto bars = rfc::native_bars();
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK_EQ("native in-run setter result", std::to_string(host.setter_result), "-1");
        expect_row("native_in_run_mutation_at_setter", at_setter,
                   {"engine_unclassified_error", "{}",
                    "native host refuses source mutation: set_native_security_feed", 0, -1});
        expect_row("native_in_run_mutation_after_run", observe(rfc::as_handle(host)),
                   {"engine_unclassified_error", "{}",
                    "native host refuses source mutation: set_native_security_feed", 0, -1});
    }
}

// ---------------------------------------------------------------------------
// The adapter's run-spec refusal ("Pine adapter produced
// invalid native run spec field N") is coded by its field. The switch below
// names every enumerator without a default and is compiled with -Wswitch as an
// error, so a field added to NativeRunSpecField fails this test until it is
// mapped here (and in run_spec_field_failure).

struct FieldExpectation {
    const char* code;
    const char* args;
};

constexpr int kNativeRunSpecFieldCount = 72;  // None .. SubscriptionInstrument
static_assert(static_cast<int>(pineforge::NativeRunSpecField::SubscriptionInstrument)
                  == kNativeRunSpecFieldCount - 1,
              "a NativeRunSpecField enumerator was added or removed: map it below");

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif
FieldExpectation expected_field_failure(pineforge::NativeRunSpecField field) {
    using F = pineforge::NativeRunSpecField;
    switch (field) {
    // The symbol's catalog metadata.
    case F::Ticker: return {"symbol_metadata_rejected", R"({"field":"ticker"})"};
    case F::TickerId: return {"symbol_metadata_rejected", R"({"field":"ticker_id"})"};
    case F::Type: return {"symbol_metadata_rejected", R"({"field":"type"})"};
    case F::Currency: return {"symbol_metadata_rejected", R"({"field":"currency"})"};
    case F::BaseCurrency: return {"symbol_metadata_rejected", R"({"field":"base_currency"})"};
    case F::Description: return {"symbol_metadata_rejected", R"({"field":"description"})"};
    case F::VolumeType: return {"symbol_metadata_rejected", R"({"field":"volume_type"})"};
    case F::Timezone: return {"symbol_metadata_rejected", R"({"field":"timezone"})"};
    case F::Session: return {"symbol_metadata_rejected", R"({"field":"session"})"};
    case F::ChartTimezone: return {"symbol_metadata_rejected", R"({"field":"chart_timezone"})"};
    case F::PointValue: return {"symbol_metadata_rejected", R"({"field":"point_value"})"};
    case F::AccountFx: return {"symbol_metadata_rejected", R"({"field":"account_fx"})"};
    case F::PriceTick: return {"symbol_metadata_rejected", R"({"field":"price_tick"})"};
    case F::QuantityGrid: return {"lot_grid_rejected", "{}"};
    // The strategy() declaration and its overrides.
    case F::InitialCapital: return {"strategy_settings_rejected", R"({"field":"initial_capital"})"};
    case F::SlippageTicks: return {"strategy_settings_rejected", R"({"field":"slippage_ticks"})"};
    case F::FeeKind: return {"strategy_settings_rejected", R"({"field":"fee_kind"})"};
    case F::FeeValue: return {"strategy_settings_rejected", R"({"field":"fee_value"})"};
    case F::CloseExecution:
        return {"strategy_settings_rejected", R"({"field":"close_execution"})"};
    case F::MaxAbsUnits: return {"strategy_settings_rejected", R"({"field":"max_abs_units"})"};
    case F::MaxOpenLots: return {"strategy_settings_rejected", R"({"field":"max_open_lots"})"};
    case F::AllowedOpenDirections:
        return {"strategy_settings_rejected", R"({"field":"allowed_open_directions"})"};
    case F::InitialMarginFraction:
        return {"strategy_settings_rejected", R"({"field":"initial_margin_fraction"})"};
    case F::PriceGrid: return {"strategy_settings_rejected", R"({"field":"price_grid"})"};
    case F::GridRounding: return {"strategy_settings_rejected", R"({"field":"grid_rounding"})"};
    case F::MarginModel: return {"strategy_settings_rejected", R"({"field":"margin_model"})"};
    case F::MarginInitial: return {"strategy_settings_rejected", R"({"field":"margin_initial"})"};
    case F::MarginMaintenance:
        return {"strategy_settings_rejected", R"({"field":"margin_maintenance"})"};
    case F::MarginSizing: return {"strategy_settings_rejected", R"({"field":"margin_sizing"})"};
    case F::MarginShortfallMultiple:
        return {"strategy_settings_rejected", R"({"field":"margin_shortfall_multiple"})"};
    case F::MarginMinUnits:
        return {"strategy_settings_rejected", R"({"field":"margin_min_units"})"};
    case F::MarginCheck: return {"strategy_settings_rejected", R"({"field":"margin_check"})"};
    case F::MarginEquityBasis:
        return {"strategy_settings_rejected", R"({"field":"margin_equity_basis"})"};
    case F::MarginLevelBase:
        return {"strategy_settings_rejected", R"({"field":"margin_level_base"})"};
    case F::Calculation: return {"strategy_settings_rejected", R"({"field":"calculation"})"};
    case F::OpenBarView: return {"strategy_settings_rejected", R"({"field":"open_bar_view"})"};
    case F::RiskLimits: return {"strategy_settings_rejected", R"({"field":"risk_limits"})"};
    case F::RiskDrawdown: return {"strategy_settings_rejected", R"({"field":"risk_drawdown"})"};
    case F::RiskIntradayLoss:
        return {"strategy_settings_rejected", R"({"field":"risk_intraday_loss"})"};
    case F::RiskLossDays: return {"strategy_settings_rejected", R"({"field":"risk_loss_days"})"};
    case F::RiskFillsPerDay:
        return {"strategy_settings_rejected", R"({"field":"risk_fills_per_day"})"};
    case F::RiskDayBasis: return {"strategy_settings_rejected", R"({"field":"risk_day_basis"})"};
    case F::RiskAction: return {"strategy_settings_rejected", R"({"field":"risk_action"})"};
    case F::QuantityTolerance:
        return {"strategy_settings_rejected", R"({"field":"quantity_tolerance"})"};
    // The run request's own options.
    case F::InputTimeframe: return {"run_options_rejected", R"({"option":"input_timeframe"})"};
    case F::ScriptTimeframe: return {"run_options_rejected", R"({"option":"script_timeframe"})"};
    case F::IntrabarTimeframe:
        return {"run_options_rejected", R"({"option":"intrabar_timeframe"})"};
    case F::IntrabarSamples: return {"run_options_rejected", R"({"option":"intrabar_samples"})"};
    case F::IntrabarDistribution:
        return {"run_options_rejected", R"({"option":"intrabar_distribution"})"};
    case F::IntrabarVolumeSamples:
        return {"run_options_rejected", R"({"option":"intrabar_volume_samples"})"};
    case F::IntrabarSampleEligibility:
        return {"run_options_rejected", R"({"option":"intrabar_sample_eligibility"})"};
    case F::PathOrder: return {"run_options_rejected", R"({"option":"path_order"})"};
    // Another symbol's feed series the kernel refused.
    case F::InstrumentFeedInstrument:
        return {"symbol_feeds_refused",
                R"({"field":"instrument_feed_instrument","reason":"kernel_refused_series"})"};
    case F::InstrumentFeedTimeframe:
        return {"symbol_feeds_refused",
                R"({"field":"instrument_feed_timeframe","reason":"kernel_refused_series"})"};
    case F::InstrumentFeedBars:
        return {"symbol_feeds_refused",
                R"({"field":"instrument_feed_bars","reason":"kernel_refused_series"})"};
    case F::InstrumentFeedClose:
        return {"symbol_feeds_refused",
                R"({"field":"instrument_feed_close","reason":"kernel_refused_series"})"};
    case F::InstrumentFeedColumns:
        return {"symbol_feeds_refused",
                R"({"field":"instrument_feed_columns","reason":"kernel_refused_series"})"};
    case F::SubscriptionInstrument:
        return {"symbol_feeds_refused",
                R"({"field":"subscription_instrument","reason":"kernel_refused_series"})"};
    // What the adapter builds itself: a refusal is an engine bug.
    case F::None:
    case F::SessionKey:
    case F::RunNumber:
    case F::AbortReporting:
    case F::TimeframeUndetected:
    case F::SlotLabelPolicy:
    case F::LegacyTolerance:
    case F::ReportPolicy:
    case F::SubscriptionTimeframe:
    case F::SubscriptionBars:
    case F::AuxiliaryFeedTimeframe:
    case F::AuxiliaryFeedBars:
    case F::SubscriptionSource:
    case F::EventRetention:
        return {"engine_invariant", "{}"};
    }
    return {"<unmapped field>", "<unmapped field>"};
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

void run_spec_field_rows() {
    std::map<std::string, int> per_code;
    for (int value = 0; value < kNativeRunSpecFieldCount; ++value) {
        const auto field = static_cast<pineforge::NativeRunSpecField>(value);
        const FieldExpectation want = expected_field_failure(field);
        const RunFailureValue got = pineforge::source::run_spec_field_failure(field);
        const std::string at = "run spec field " + std::to_string(value) + ": ";
        CHECK_EQ(at + "code", code_name(got.code), want.code);
        CHECK_EQ(at + "args", args_json(got), want.args);
        ++per_code[code_name(got.code)];
    }
    for (const int value : {kNativeRunSpecFieldCount, 200, 255}) {
        const RunFailureValue got = pineforge::source::run_spec_field_failure(
            static_cast<pineforge::NativeRunSpecField>(value));
        const std::string at = "out-of-range run spec field " + std::to_string(value) + ": ";
        CHECK_EQ(at + "code", code_name(got.code), "engine_invariant");
        CHECK_EQ(at + "args", args_json(got), "{}");
    }
    std::printf("run spec fields: %d mapped:", kNativeRunSpecFieldCount);
    for (const auto& [code, count] : per_code) std::printf(" %s=%d", code.c_str(), count);
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// The registry the library validates against is the catalog
// docker/run_failure_codes.json, read here at run time.

struct Json {
    enum class Kind { null, boolean, number, string, array, object };
    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;  // in document order

    const Json* find(const std::string& key) const {
        for (const auto& entry : object) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    Json document() {
        Json value = parse();
        skip();
        if (pos_ != text_.size()) fail("trailing bytes");
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const char* why) const {
        throw std::runtime_error(std::string("catalog JSON: ") + why + " at byte "
                                 + std::to_string(pos_));
    }
    void skip() {
        while (pos_ < text_.size()
               && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r'
                   || text_[pos_] == '\t')) {
            ++pos_;
        }
    }
    char next() {
        if (pos_ >= text_.size()) fail("unexpected end");
        return text_[pos_++];
    }
    bool literal(const char* word) {
        const std::size_t length = std::strlen(word);
        if (text_.compare(pos_, length, word) != 0) return false;
        pos_ += length;
        return true;
    }
    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    std::uint32_t hex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = next();
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return value;
    }
    std::string string_value() {
        if (next() != '"') fail("expected a string");
        std::string out;
        for (;;) {
            const char c = next();
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            const char escape = next();
            switch (escape) {
            case '"': case '\\': case '/': out += escape; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (next() != '\\' || next() != 'u') fail("lone surrogate");
                    const std::uint32_t low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF) fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
    }
    Json parse() {
        skip();
        if (pos_ >= text_.size()) fail("unexpected end");
        Json value;
        const char c = text_[pos_];
        if (c == '{') {
            value.kind = Json::Kind::object;
            ++pos_;
            skip();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return value;
            }
            for (;;) {
                skip();
                std::string key = string_value();
                skip();
                if (next() != ':') fail("expected ':'");
                value.object.emplace_back(std::move(key), parse());
                skip();
                const char separator = next();
                if (separator == '}') return value;
                if (separator != ',') fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            value.kind = Json::Kind::array;
            ++pos_;
            skip();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return value;
            }
            for (;;) {
                value.array.push_back(parse());
                skip();
                const char separator = next();
                if (separator == ']') return value;
                if (separator != ',') fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            value.kind = Json::Kind::string;
            value.string = string_value();
            return value;
        }
        if (literal("true")) {
            value.kind = Json::Kind::boolean;
            value.boolean = true;
            return value;
        }
        if (literal("false")) {
            value.kind = Json::Kind::boolean;
            return value;
        }
        if (literal("null")) return value;
        const char* begin = text_.c_str() + pos_;
        char* end = nullptr;
        value.kind = Json::Kind::number;
        value.number = std::strtod(begin, &end);
        if (end == begin) fail("unexpected character");
        pos_ += static_cast<std::size_t>(end - begin);
        return value;
    }
};

const char* class_name(RunFailureClass cls) {
    switch (cls) {
    case RunFailureClass::strategy: return "strategy";
    case RunFailureClass::strategy_limit: return "strategy_limit";
    case RunFailureClass::no_data: return "no_data";
    case RunFailureClass::symbol_metadata: return "symbol_metadata";
    case RunFailureClass::symbol_feeds: return "symbol_feeds";
    case RunFailureClass::input: return "input";
    case RunFailureClass::unsupported: return "unsupported";
    case RunFailureClass::resource: return "resource";
    case RunFailureClass::engine_fault: return "engine_fault";
    }
    return "<unknown class>";
}

struct ArgSpec {
    std::string name;
    std::string kind;
    bool optional = false;
    std::vector<std::string> values;
};

// A value the arg's kind accepts, and the JSON it canonicalizes to.
RunFailureArg sample_arg(const ArgSpec& spec, std::string& json_value) {
    if (spec.kind == "integer") {
        json_value = "7";
        return RunFailureArg(spec.name.c_str(), std::int64_t{7});
    }
    if (spec.kind == "number") {
        json_value = "1.5";
        return RunFailureArg(spec.name.c_str(), 1.5);
    }
    std::string text = "sample_text";
    if (spec.kind == "vocab") text = spec.values.front();
    if (spec.kind == "pine_source") text = R"(request.financial("NASDAQ:AAPL", "EPS", ...))";
    if (spec.kind == "symbol") text = "BINANCE:ETHUSDT";
    if (spec.kind == "timeframe") text = "60";
    json_value = json_quote(text);
    return RunFailureArg(spec.name.c_str(), text);
}

void check_code_args(RunFailureCode code, const std::string& name, const Json* args) {
    const std::string at = "catalog " + name + ": ";
    std::vector<ArgSpec> specs;
    if (args != nullptr) {
        for (const auto& [arg_name, arg] : args->object) {
            ArgSpec spec;
            spec.name = arg_name;
            const Json* kind = arg.find("kind");
            const Json* optional = arg.find("optional");
            const Json* values = arg.find("values");
            spec.kind = kind ? kind->string : "";
            spec.optional = optional && optional->boolean;
            if (values) {
                for (const Json& value : values->array) spec.values.push_back(value.string);
            }
            CHECK_EQ(at + arg_name + " has a kind", spec.kind.empty() ? "none" : "kind", "kind");
            CHECK_EQ(at + arg_name + " vocab has values",
                     spec.kind == "vocab" && spec.values.empty() ? "empty" : "ok", "ok");
            specs.push_back(std::move(spec));
        }
    }
    std::sort(specs.begin(), specs.end(),
              [](const ArgSpec& l, const ArgSpec& r) { return l.name < r.name; });

    const auto expect = [&](const std::string& what, const RunFailureArgs& raised,
                            RunFailureCode want_code, const std::string& want_args) {
        const RunFailureValue value = pineforge::make_run_failure(code, raised);
        CHECK_EQ(at + what + ": code", code_name(value.code), code_name(want_code));
        CHECK_EQ(at + what + ": args", args_json(value), want_args);
    };
    const RunFailureCode invariant = RunFailureCode::engine_invariant;

    // Every declared arg, then the required ones alone: the code stands, the
    // arguments come back as canonical JSON (keys sorted, no whitespace).
    RunFailureArgs all, required;
    std::string all_json = "{", required_json = "{";
    for (const ArgSpec& spec : specs) {
        std::string value_json;
        RunFailureArg arg = sample_arg(spec, value_json);
        const std::string member = json_quote(spec.name) + ":" + value_json;
        all_json += (all_json.size() > 1 ? "," : "") + member;
        all.push_back(arg);
        if (!spec.optional) {
            required_json += (required_json.size() > 1 ? "," : "") + member;
            required.push_back(arg);
        }
    }
    all_json += "}";
    required_json += "}";
    expect("every arg", all, code, all_json);
    expect("required args", required, code, required_json);

    // Each required arg left out; an undeclared arg; a repeated arg.
    for (std::size_t i = 0; i < required.size(); ++i) {
        RunFailureArgs missing = required;
        missing.erase(missing.begin() + static_cast<std::ptrdiff_t>(i));
        expect(std::string("without ") + required[i].name, missing, invariant, "{}");
    }
    RunFailureArgs undeclared = all;
    undeclared.emplace_back("not_an_argument", "x");
    expect("undeclared arg", undeclared, invariant, "{}");
    if (!all.empty()) {
        RunFailureArgs repeated = all;
        repeated.push_back(all.front());
        expect("repeated arg", repeated, invariant, "{}");
    }

    // Kinds and closed lists, one arg at a time.
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const ArgSpec& spec = specs[i];
        const auto with = [&](RunFailureArg replacement) {
            RunFailureArgs raised = all;
            raised[i] = std::move(replacement);
            return raised;
        };
        const char* arg = spec.name.c_str();
        if (spec.kind == "vocab") {
            for (const std::string& value : spec.values) {
                const RunFailureValue got = pineforge::make_run_failure(code, with({arg, value}));
                CHECK_EQ(at + spec.name + "=" + value + " accepted", code_name(got.code), name);
            }
            expect(spec.name + " outside its list", with({arg, "not_a_listed_value"}), invariant,
                   "{}");
            expect(spec.name + " as an integer", with({arg, std::int64_t{1}}), invariant, "{}");
        } else if (spec.kind == "integer") {
            expect(spec.name + " as text", with({arg, "7"}), invariant, "{}");
            expect(spec.name + " as a number", with({arg, 7.5}), invariant, "{}");
        } else if (spec.kind == "number") {
            expect(spec.name + " as text", with({arg, "1.5"}), invariant, "{}");
            expect(spec.name + " not finite",
                   with({arg, std::numeric_limits<double>::quiet_NaN()}), invariant, "{}");
        } else {
            expect(spec.name + " as an integer", with({arg, std::int64_t{1}}), invariant, "{}");
        }
    }
}

void registry_rows() {
    std::ifstream in(PINEFORGE_RUN_FAILURE_CATALOG, std::ios::binary);
    CHECK_EQ("catalog readable", in ? "open" : "missing", "open");
    if (!in) return;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json catalog;
    try {
        catalog = JsonReader(text).document();
    } catch (const std::exception& error) {
        CHECK_EQ("catalog parses", error.what(), "");
        return;
    }
    const Json* schema = catalog.find("schema");
    CHECK_EQ("catalog schema", schema ? schema->string : "",
             "pineforge-run-failure-catalog/v1");
    const Json* codes = catalog.find("codes");
    CHECK(codes != nullptr && codes->kind == Json::Kind::object);
    if (codes == nullptr) return;

    // The enum is the catalog in order: code i is the catalog's i-th name, and
    // every name round-trips through exactly one code.
    CHECK_EQ("code count", std::to_string(pineforge::kRunFailureCodeCount),
             std::to_string(codes->object.size() + 1));
    CHECK_EQ("none has no name", code_name(RunFailureCode::none), "");
    CHECK_EQ("past the last code has no name",
             code_name(static_cast<RunFailureCode>(pineforge::kRunFailureCodeCount)), "");
    std::map<std::string, RunFailureCode> by_name;
    for (std::uint16_t i = 1; i < pineforge::kRunFailureCodeCount; ++i) {
        const auto code = static_cast<RunFailureCode>(i);
        const bool unique = by_name.emplace(code_name(code), code).second;
        CHECK_EQ("code name unique: " + code_name(code), unique ? "unique" : "repeated",
                 "unique");
    }
    std::size_t index = 1;
    for (const auto& [name, entry] : codes->object) {
        const auto code = static_cast<RunFailureCode>(index++);
        CHECK_EQ("catalog order", code_name(code), name);
        const auto found = by_name.find(name);
        CHECK_EQ("name round trip: " + name,
                 found == by_name.end() ? "" : code_name(found->second), name);
        const Json* cls = entry.find("class");
        const Json* retryable = entry.find("retryable");
        CHECK_EQ("class of " + name, class_name(pineforge::run_failure_code_class(code)),
                 cls ? cls->string : "");
        CHECK_EQ("retryable of " + name,
                 pineforge::run_failure_code_retryable(code) ? "true" : "false",
                 retryable && retryable->boolean ? "true" : "false");
        check_code_args(code, name, entry.find("args"));
    }
    // No code, an unknown code: engine_invariant, never a code of their own.
    CHECK_EQ("make none", code_name(pineforge::make_run_failure(RunFailureCode::none, {}).code),
             "engine_invariant");
    CHECK_EQ("make unknown",
             code_name(pineforge::make_run_failure(
                 static_cast<RunFailureCode>(pineforge::kRunFailureCodeCount), {}).code),
             "engine_invariant");
    std::printf("registry: %zu catalog codes checked against the library\n",
                codes->object.size());
}

}  // namespace

int main() {
    const auto minute = rfc::make_bars(60, 60000);
    const auto five = rfc::make_bars(60, 300000);
    forged_runtime_error_rows(minute);
    calc_on_order_fills_rows(minute);
    refused_begin_rows(minute);
    preparation_rows(minute, five);
    matrix_rows(minute);
    helper_stop_rows(minute);
    helper_contract_rows();
    record_lifecycle_rows(minute);
    latched_setting_rows(minute);
    uncoded_writer_rows();
    coded_latched_setting_rows(minute);
    native_host_rows();
    recorded_failure_rows();
    native_module_rows();
    stream_refusal_rows(minute);
    run_spec_field_rows();
    registry_rows();
    std::printf("test_run_failure_codes: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
