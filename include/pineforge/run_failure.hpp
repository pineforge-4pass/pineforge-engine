#pragma once

// Stable run-failure codes: every engine-originated run failure carries one.
//
// A run that fails leaves two things beside each other: the English text
// strategy_get_last_error() has always returned, unchanged byte for byte, and
// a code from a closed vocabulary (strategy_get_last_error_code()) with its
// typed arguments as one canonical JSON object
// (strategy_get_last_error_args()). The vocabulary, its classes, its argument
// kinds and closed value lists, and the English each code is written with,
// are published as docker/run_failure_codes.json (schema
// pineforge-run-failure-catalog/v1); include/pineforge/run_failure_codes.hpp
// and src/run_failure_registry.inc are generated from it
// (scripts/check_run_failure_codes.py --write-registry) and the source guard
// holds them to it.
//
// The rules the channel keeps:
//  * a code is chosen where the failure is raised, never read from data: a
//    text a script writes (runtime.error) can only ever read
//    strategy_runtime_error, whatever it says;
//  * an argument is built only from a literal the transpiler emitted (a call's
//    spelling, its line, a literal symbol, an input title), a closed engine
//    vocabulary, or a value the run request supplied (a row of its OHLCV, its
//    timeframes) -- never from a value the script computes while it runs;
//  * a vocabulary argument outside its closed list, an undeclared argument, a
//    wrong kind or a missing required argument turns the failure into
//    engine_invariant (the English is kept): a wrong call cannot mint a code
//    or an open string.
//
// Thrown types do not change: coded<Base> derives from Base, so every typed
// catch and every what() is what it was. The classifier
// (classify_run_failure) reads the carried code; an exception without one is
// classified by its type as a safety net only -- std::bad_alloc is
// out_of_memory, the std::logic_error family engine_invariant, anything else
// engine_unclassified_error.
//
// ---------------------------------------------------------------------------
// The codegen contract (PINEFORGE_HAS_RUN_FAILURE_CODES_V1). Generated code
// may call exactly these, under `#ifdef PINEFORGE_HAS_RUN_FAILURE_CODES_V1`
// (and keep `pine_runtime_error(std::string(english))` in the `#else` arm).
// Every helper is [[noreturn]], throws coded<std::runtime_error>, hardwires its
// code and never takes one; `english` is the text the stop has today, and is
// what strategy_get_last_error() reads, byte for byte. Pointer arguments are
// codegen literals (`const char*`, never NULL except where marked); `line` is
// the call's line in the source the transpiler read.
//
//   pine_runtime_error(const std::string& message)              (log.hpp)
//       -> strategy_runtime_error {}
//   pine_no_data_stop(const char* function, const char* call,
//                     std::int32_t line, const char* english)
//       -> no_data_request {function, call, line}
//          function: request.financial | request.earnings | request.dividends |
//                    request.splits | request.footprint | request.economic |
//                    request.quandl | request.seed | request.currency_rate
//   pine_other_symbol_stop(const char* function,
//                          const char* symbol_literal_or_null,
//                          const char* call, std::int32_t line,
//                          const char* english)
//       -> other_symbol_request {function, symbol?, call, line}
//          function: request.security | request.security_lower_tf; symbol only
//          when the call spells it as a literal (NULL otherwise)
//   pine_array_stop(const char* reason, const char* method_or_null,
//                   const char* english)
//       -> pine_array_error {reason, method?}
//          reason: index_out_of_bounds | slice_range_inverted |
//                  empty_array_access | size_invalid
//          method: new | get | set | insert | remove | percentrank | fill |
//                  slice | first | last | pop | shift
//   pine_collection_stop(const char* collection, const char* reason,
//                        const char* english)
//       collection: array | matrix
//       reason historical_modified -> pine_array_error {reason, collection}
//       reason na_reference        -> pine_na_reference {object: collection}
//   pine_na_stop(const char* object, const char* english)
//       -> pine_na_reference {object}
//          object: array | matrix | map | line | box | label | linefill |
//                  polyline | table | udt_object
//   pine_limit_stop(const char* limit, std::int64_t max, const char* english)
//       -> pine_runtime_limit {limit, max}
//          limit: map_pairs | max_bars_back | matrix_elements | udt_objects;
//          max is the limit's engine or transpiler constant (pass -1 for none)
//   pine_unsupported_stop(const char* reason, std::int32_t line,
//                         const char* english)
//       -> request_unsupported {reason, line}
//          reason: lower_tf_lookahead_or_gaps | nested_heikinashi_request
//          (line -1: none)
//   pine_string_stop(const char* reason, const char* english)
//       -> pine_string_error {reason}
//          reason: substring_out_of_range | format_index_overflow
//   pine_engine_invariant(const char* english)
//       -> engine_invariant {}
//
//   note_run_failure(BacktestEngine&, const char* entrypoint,
//                    const std::exception&)
//       the generated wrapper's catch: writes "<entrypoint>: <what()>" (the
//       text it writes today) with the exception's code;
//   note_run_failure_unknown(BacktestEngine&, const char* entrypoint)
//       its catch (...): "<entrypoint>: unknown C++ exception",
//       engine_unclassified_error;
//   note_run_failure(BacktestEngine&, std::string text, RunFailureCode,
//                    const RunFailureArgs&)
//       a latched setter refusal: setting_rejected with its entrypoint and
//       reason.
// ---------------------------------------------------------------------------

#include <pineforge/run_failure_codes.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define PINEFORGE_HAS_RUN_FAILURE_CODES_V1 1

namespace pineforge {

inline namespace engine_script_run_v19 {
class BacktestEngine;
}

/// The class a code belongs to (the catalog's `class`).
enum class RunFailureClass : std::uint8_t {
    strategy,
    strategy_limit,
    no_data,
    symbol_metadata,
    symbol_feeds,
    input,
    unsupported,
    resource,
    engine_fault,
};

/// An argument's kind (the catalog's `kind`).
enum class RunFailureArgKind : std::uint8_t {
    identifier,
    keyword,
    vocab,
    integer,
    number,
    pine_source,
    symbol,
    timeframe,
};

/// One typed argument as a raise site spells it. `name` is a literal; a text
/// value is copied.
struct RunFailureArg {
    enum class Type : std::uint8_t { text, integer, number };
    const char* name = "";
    Type type = Type::text;
    std::string text;
    std::int64_t integer = 0;
    double number = 0.0;

    // Out of line (src/run_failure.cpp): a literal value is handed over by
    // address, so it stays one contiguous string wherever it is compiled.
    RunFailureArg(const char* arg_name, const char* value);
    RunFailureArg(const char* arg_name, std::string value)
        : name(arg_name), text(std::move(value)) {}
    template <typename Integer,
              typename std::enable_if<std::is_integral<Integer>::value
                                      && !std::is_same<Integer, bool>::value, int>::type = 0>
    RunFailureArg(const char* arg_name, Integer value)
        : name(arg_name), type(Type::integer), integer(static_cast<std::int64_t>(value)) {}
    template <typename Floating,
              typename std::enable_if<std::is_floating_point<Floating>::value, int>::type = 0>
    RunFailureArg(const char* arg_name, Floating value)
        : name(arg_name), type(Type::number), number(static_cast<double>(value)) {}
};

using RunFailureArgs = std::vector<RunFailureArg>;

/// A validated code and its canonical arguments: the JSON object with its keys
/// sorted and no whitespace, or null for a code without arguments ("{}").
struct RunFailureValue {
    RunFailureCode code = RunFailureCode::none;
    std::shared_ptr<const std::string> args;
};

/// Validate `args` against `code`'s registry entry and canonicalize them. Any
/// violation yields engine_invariant without arguments. Allocation failure
/// yields out_of_memory.
RunFailureValue make_run_failure(RunFailureCode code, const RunFailureArgs& args) noexcept;

/// The code's catalog name ("" for none or an unknown value).
const char* run_failure_code_name(RunFailureCode code) noexcept;
/// The code's catalog class and retryable flag.
RunFailureClass run_failure_code_class(RunFailureCode code) noexcept;
bool run_failure_code_retryable(RunFailureCode code) noexcept;

/// The code an exception carries: a coded exception's own; else the safety
/// net by type (std::bad_alloc -> out_of_memory, std::logic_error ->
/// engine_invariant, other -> engine_unclassified_error).
RunFailureValue classify_run_failure(const std::exception& error) noexcept;

/// The code carried beside a thrown exception. A mixin: it adds no base of
/// std::exception, so coded<Base> is still exactly a Base to every catch.
class RunFailureInfo {
public:
    RunFailureInfo(RunFailureCode code, const RunFailureArgs& args) noexcept
        : value_(make_run_failure(code, args)) {}
    // An already validated value (a code another site recorded, carried on).
    explicit RunFailureInfo(RunFailureValue value) noexcept : value_(std::move(value)) {}
    RunFailureInfo(const RunFailureInfo&) noexcept = default;
    RunFailureInfo& operator=(const RunFailureInfo&) noexcept = default;
    virtual ~RunFailureInfo();

    RunFailureCode run_failure_code() const noexcept { return value_.code; }
    const RunFailureValue& run_failure() const noexcept { return value_; }

private:
    RunFailureValue value_;
};

/// `Base` with a code: throw coded<std::invalid_argument>(code, {args}, text).
template <class Base>
class coded : public Base, public RunFailureInfo {
public:
    coded(RunFailureCode code, const RunFailureArgs& args, const std::string& what)
        : Base(what), RunFailureInfo(code, args) {}
    coded(RunFailureCode code, const RunFailureArgs& args, const char* what)
        : Base(what), RunFailureInfo(code, args) {}
    coded(RunFailureValue value, const std::string& what)
        : Base(what), RunFailureInfo(std::move(value)) {}
    coded(RunFailureValue value, const char* what)
        : Base(what), RunFailureInfo(std::move(value)) {}
};

// The record beside BacktestEngine::last_error(): writes the text and the
// code together, so they never disagree. Defined with the execution consumer
// that stores the record (src/run_failure.cpp).
void note_run_failure(BacktestEngine& engine, std::string text, RunFailureCode code,
                      const RunFailureArgs& args = {});
// The same for a literal text (preferred for one): taken by address.
void note_run_failure(BacktestEngine& engine, const char* text, RunFailureCode code,
                      const RunFailureArgs& args = {});
void note_run_failure(BacktestEngine& engine, std::string text, const RunFailureValue& value);
void note_run_failure(BacktestEngine& engine, const char* entrypoint,
                      const std::exception& error);
void note_run_failure_unknown(BacktestEngine& engine, const char* entrypoint);
void clear_run_failure(BacktestEngine& engine) noexcept;

/// The getters' answers (strategy_get_last_error_code / _args): the stored code
/// while its text is the engine's last error; engine_unclassified_error for a
/// non-empty error no coded site wrote; "" when there is no failure.
const char* run_failure_code_of(const BacktestEngine& engine) noexcept;
const char* run_failure_args_of(const BacktestEngine& engine) noexcept;
/// The recorded value under the same rule (none when there is no failure).
RunFailureValue run_failure_value_of(const BacktestEngine& engine) noexcept;

// ---------------------------------------------------------------------------
// The generated-code helpers (the codegen contract above).

[[noreturn]] inline void pine_no_data_stop(const char* function, const char* call,
                                           std::int32_t line, const char* english) {
    throw coded<std::runtime_error>(
        RunFailureCode::no_data_request,
        {{"function", function}, {"call", call}, {"line", line}}, english ? english : "");
}

[[noreturn]] inline void pine_other_symbol_stop(const char* function,
                                                const char* symbol_literal_or_null,
                                                const char* call, std::int32_t line,
                                                const char* english) {
    RunFailureArgs args{{"function", function}, {"call", call}, {"line", line}};
    if (symbol_literal_or_null) args.emplace_back("symbol", symbol_literal_or_null);
    throw coded<std::runtime_error>(RunFailureCode::other_symbol_request, args,
                                    english ? english : "");
}

[[noreturn]] inline void pine_array_stop(const char* reason, const char* method_or_null,
                                         const char* english) {
    RunFailureArgs args{{"reason", reason}};
    if (method_or_null) args.emplace_back("method", method_or_null);
    throw coded<std::runtime_error>(RunFailureCode::pine_array_error, args,
                                    english ? english : "");
}

[[noreturn]] inline void pine_collection_stop(const char* collection, const char* reason,
                                              const char* english) {
    const char* text = english ? english : "";
    if (reason && std::strcmp(reason, "na_reference") == 0) {
        throw coded<std::runtime_error>(RunFailureCode::pine_na_reference,
                                        {{"object", collection}}, text);
    }
    if (reason && std::strcmp(reason, "historical_modified") == 0) {
        throw coded<std::runtime_error>(
            RunFailureCode::pine_array_error,
            {{"reason", "historical_modified"}, {"collection", collection}}, text);
    }
    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {}, text);
}

[[noreturn]] inline void pine_na_stop(const char* object, const char* english) {
    throw coded<std::runtime_error>(RunFailureCode::pine_na_reference, {{"object", object}},
                                    english ? english : "");
}

[[noreturn]] inline void pine_limit_stop(const char* limit, std::int64_t max,
                                         const char* english) {
    RunFailureArgs args{{"limit", limit}};
    if (max >= 0) args.emplace_back("max", max);
    throw coded<std::runtime_error>(RunFailureCode::pine_runtime_limit, args,
                                    english ? english : "");
}

[[noreturn]] inline void pine_unsupported_stop(const char* reason, std::int32_t line,
                                               const char* english) {
    RunFailureArgs args{{"reason", reason}};
    if (line >= 0) args.emplace_back("line", line);
    throw coded<std::runtime_error>(RunFailureCode::request_unsupported, args,
                                    english ? english : "");
}

[[noreturn]] inline void pine_string_stop(const char* reason, const char* english) {
    throw coded<std::runtime_error>(RunFailureCode::pine_string_error, {{"reason", reason}},
                                    english ? english : "");
}

[[noreturn]] inline void pine_engine_invariant(const char* english) {
    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                    english ? english : "");
}

}  // namespace pineforge
