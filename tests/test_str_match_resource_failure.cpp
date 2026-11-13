#include "fail_alloc_seam.hpp"

#include <pineforge/native_module.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/ta.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

int failures = 0;

constexpr char kInvalidRegexText[] = "Invalid regex pattern in the 'str.match' function.";
constexpr char kInvalidRegexArgs[] = "{\"reason\":\"invalid_regex_pattern\"}";
constexpr char kRegexResourceText[] =
    "A regular expression exceeds the limits of the engine's regular-expression library.";
constexpr char kRegexResourceArgs[] = "{\"reason\":\"regex_resource_limit\"}";

struct MatchInput {
    const char* source;
    const char* pattern;
};

constexpr MatchInput kInvalidPatterns[] = {
    {"a(b", "("},
    {"abc", "[a-"},
    {"abc", "*abc"},
    {"aa", "a{2,1}"},
    {"a\\b", "\\"},
    {"ab", "(?<n>a)(?<n>b)"},
};

constexpr MatchInput kLongMatch{
    "ignored prefix; payload=abcdefghijklmnopqrstuvwxyz0123456789; other=UNCHANGED",
    "(?:payload|other)=([a-z0-9]+);",
};

void check(bool condition, const char* message, std::size_t nth = 0) {
    if (condition) return;
    ++failures;
    std::fprintf(stderr, "FAIL [allocation %zu] %s\n", nth, message);
}

bool has_single_bar_prefix(const std::string& text, const char* bare_text) {
    const std::size_t bare_size = std::strlen(bare_text);
    if (text.size() < bare_size) return false;
    const std::size_t prefix_size = text.size() - bare_size;
    if (text.compare(prefix_size, bare_size, bare_text) != 0) return false;

    constexpr char before_first_bar[] = "Error before the first bar: ";
    if (prefix_size == sizeof(before_first_bar) - 1
        && text.compare(0, prefix_size, before_first_bar) == 0) return true;

    constexpr char on_bar[] = "Error on bar ";
    constexpr std::size_t number_start = sizeof(on_bar) - 1;
    if (prefix_size < number_start + 3 || text.compare(0, number_start, on_bar) != 0
        || text.compare(prefix_size - 2, 2, ": ") != 0) return false;
    std::size_t pos = number_start;
    if (text[pos] == '-') ++pos;
    const std::size_t number_end = prefix_size - 2;
    if (pos == number_end) return false;
    for (; pos < number_end; ++pos) {
        if (text[pos] < '0' || text[pos] > '9') return false;
    }
    return true;
}

void match_allocation_sweep(const char* name, const std::string& source, const std::string& pattern,
                            const std::string& expected) {
    bool clean_call = true;
    bool clean_answer_ok = false;
    bool returned = false;
    bool answer_ok = false;
    std::size_t out_of_memory_failures = 0;
    const std::size_t count = fail_alloc::sweep(
        [&] {
            returned = false;
            const std::string answer = pineforge::str_match(source, pattern);
            returned = true;
            answer_ok = answer == expected;
            if (clean_call) {
                clean_answer_ok = answer_ok;
                clean_call = false;
            }
        },
        [&](std::size_t nth, bool fired, bool threw) {
            check(fired, "every measured match allocation must be refused", nth);
            if (fired) {
                if (threw) ++out_of_memory_failures;
                check(threw, "a refused match allocation must escape as bad_alloc", nth);
                check(!returned, "a refused match allocation must not return text", nth);
            } else {
                check(!threw && returned && answer_ok,
                      "an uninjected match must return its expected text", nth);
            }
        });
    check(count > 0, "the match sweep must observe real allocations");
    check(clean_answer_ok, "the clean match must return its expected text");
    std::printf("%s: swept all %zu run allocations, %zu out_of_memory failures, "
                "%zu typed setup refusals\n", name, count, out_of_memory_failures,
                std::size_t{0});
}

void invalid_pattern_controls(const char* expected_prefix = "Error before the first bar: ") {
    // The generated-code forward reaches the production invalid-pattern
    // result policy; this test does not duplicate the error-code decision.
    const std::string expected_text = std::string(expected_prefix) + kInvalidRegexText;
    for (const auto& input : kInvalidPatterns) {
        bool typed_stop = false;
        try {
            (void)pineforge::pine_str_match(input.source, input.pattern);
        } catch (const std::exception& error) {
            const auto* typed = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
            typed_stop = typed != nullptr
                && typed->run_failure_code() == pineforge::RunFailureCode::pine_string_error
                && typed->run_failure().args
                && *typed->run_failure().args == kInvalidRegexArgs
                && std::strcmp(error.what(), expected_text.c_str()) == 0;
        }
        check(typed_stop, "an invalid pattern must throw the typed string error");
    }
}

void valid_pattern_controls() {
    const struct {
        const char* source;
        const char* pattern;
        const char* expected;
    } rows[] = {
        {"abc123def456", "[0-9]+", "123"},
        {"price: 42.5", "price: ([0-9.]+)", "42.5"},
        {"lowerCASE", "[A-Z]+", "CASE"},
        {"abc123", "^([a-z]+)([0-9]+)$", "abc"},
        {"letters only", "[0-9]+", ""},
        {"", "[0-9]+", ""},
        {"abc", "", ""},
        {"", "", ""},
    };
    for (const auto& row : rows) {
        check(pineforge::pine_str_match(row.source, row.pattern) == row.expected,
              "a valid pattern must retain its expected result");
    }
}

void construction_resource_limit(const char* expected_prefix = "Error before the first bar: ") {
#if defined(__GLIBCXX__)
    const std::string expected_text = std::string(expected_prefix) + kRegexResourceText;
    const auto resource_regex_code = [](std::regex_constants::error_type code) {
        return code == std::regex_constants::error_space
            || code == std::regex_constants::error_complexity
            || code == std::regex_constants::error_stack;
    };
    // libstdc++ defaults to 100000 NFA states. Three nested counts expand
    // to at least 100^3 character states, before group and terminal states.
    static_assert(100u * 100u * 100u > _GLIBCXX_REGEX_STATE_LIMIT,
                  "the nested counts must exceed the active NFA state limit");
    const std::string pattern = "((a{100}){100}){100}";
    bool construction_limited = false;
    try {
        const std::regex expression(pattern);
        (void)expression;
    } catch (const std::regex_error& error) {
        construction_limited = resource_regex_code(error.code());
        const char* name = error.code() == std::regex_constants::error_space ? "error_space"
            : error.code() == std::regex_constants::error_complexity ? "error_complexity"
            : error.code() == std::regex_constants::error_stack ? "error_stack" : "non-resource";
        std::fprintf(stderr, "nested pattern: %s (%d): %s\n", name,
                     static_cast<int>(error.code()), error.what());
    }
    check(construction_limited, "the nested pattern must reach the library's state limit");

    bool typed_resource_failure = false;
    try {
        (void)pineforge::str_match("a", pattern);
    } catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        const auto value = pineforge::classify_run_failure(error);
        typed_resource_failure = typed != nullptr
            && typed->run_failure_code() == pineforge::RunFailureCode::pine_string_error
            && typed->run_failure().args
            && *typed->run_failure().args == kRegexResourceArgs
            && value.code == pineforge::RunFailureCode::pine_string_error
            && std::strcmp(pineforge::run_failure_code_name(value.code), "pine_string_error") == 0
            && !pineforge::run_failure_code_retryable(value.code)
            && std::strcmp(error.what(), expected_text.c_str()) == 0;
    }
    check(typed_resource_failure, "the state limit must escape as a typed resource failure");
#else
    (void)expected_prefix;
#endif
}

void bar_prefix_controls() {
    check(!pineforge::ta::bar_context().installed,
          "the direct-call prefix control must start without a bar context");
    check(pineforge::run_error_bar_prefix() == "Error before the first bar: ",
          "an absent bar context must give the exact before-first-bar prefix");
    {
        pineforge::ta::BarContextScope scope(7, 0);
        check(pineforge::run_error_bar_prefix() == "Error on bar 7: ",
              "an installed context must give the exact bar-seven prefix");
        invalid_pattern_controls("Error on bar 7: ");
        construction_resource_limit("Error on bar 7: ");
    }
    check(!pineforge::ta::bar_context().installed,
          "the bar scope must restore the absent context");
    check(pineforge::run_error_bar_prefix() == "Error before the first bar: ",
          "the restored context must give the exact before-first-bar prefix");
}

pineforge::NativeRunSpec native_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "str-match-resource-failure";
    spec.identity.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    return spec;
}

// The standard-exception callback fixture in test_run_failure_codes.cpp,
// with a real match allocation failure instead of an explicit throw.
class MatchHost : public pineforge::NativeStrategyHost {
public:
    std::size_t fail_at = 0;
    std::size_t first_call_allocations = 0;
    bool inject_first_match = true;
    bool fired = false;
    bool first_call_returned = false;
    bool answers_ok = true;
    int calls = 0;
    std::string source = kLongMatch.source;
    std::string pattern = kLongMatch.pattern;

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        std::string answer;
        if (calls++ == 0) {
            if (inject_first_match) fail_alloc::arm(fail_at);
            const std::size_t before = fail_alloc::attempts();
            try {
                answer = pineforge::pine_str_match(source, pattern);
            } catch (...) {
                first_call_allocations = fail_alloc::attempts() - before;
                if (inject_first_match) fired = fail_alloc::disarm();
                throw;
            }
            first_call_allocations = fail_alloc::attempts() - before;
            if (inject_first_match) fired = fail_alloc::disarm();
            first_call_returned = true;
        } else {
            answer = pineforge::pine_str_match(source, pattern);
        }
        answers_ok = answers_ok && answer == "abcdefghijklmnopqrstuvwxyz0123456789";
        // A swallowed match failure changes the closed-trade list: an empty
        // result on the first bar omits the entry, on the second its exit.
        if (!answer.empty() && calls == 1) {
            submit_market({pineforge::order_action::Transact{
                               static_cast<double>(answer.size())}, "match-entry", ""});
        } else if (!answer.empty() && calls == 2) {
            submit_market({pineforge::execution::Flatten{}, "match-exit", ""});
        }
    }
};

struct RunObservation {
    std::size_t allocations = 0;
    bool fired = false;
    bool first_call_returned = false;
    bool answers_ok = false;
    int calls = 0;
    int status = -1;
    int trades = 0;
    std::string code;
    std::string text;
    std::string args;
    std::string result;

    bool failed() const noexcept {
        return !text.empty() || !code.empty() || status == 1;
    }
};

struct Report {
    pf_report_t value{};
    ~Report() { pineforge::native_module::report_free(&value); }
};

template <class Value>
void append_bytes(std::string& out, const Value& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}

std::string closed_trade_bytes(const pf_report_t& report) {
    std::string out;
    append_bytes(out, report.input_bars_processed);
    append_bytes(out, report.script_bars_processed);
    append_bytes(out, report.total_trades);
    append_bytes(out, report.trades_len);
    append_bytes(out, report.net_profit);
    check(report.trades_len >= 0 && (report.trades_len == 0 || report.trades != nullptr),
          "the completed report must own its closed-trade list");
    if (report.trades != nullptr) {
        for (int i = 0; i < report.trades_len; ++i) {
            const auto& trade = report.trades[i];
            // Append every field separately: pointer values and struct
            // padding are not part of the closed-trade byte oracle.
            append_bytes(out, trade.entry_time);
            append_bytes(out, trade.exit_time);
            append_bytes(out, trade.entry_price);
            append_bytes(out, trade.exit_price);
            append_bytes(out, trade.pnl);
            append_bytes(out, trade.pnl_pct);
            append_bytes(out, trade.is_long);
            append_bytes(out, trade.max_runup);
            append_bytes(out, trade.max_drawdown);
            append_bytes(out, trade.qty);
            append_bytes(out, trade.commission);
            append_bytes(out, trade.entry_bar_index);
            append_bytes(out, trade.exit_bar_index);
            append_bytes(out, trade.open_at_end);
        }
    }
    return out;
}

enum class InjectionPoint { first_match, whole_run };

RunObservation run_match(std::size_t nth, InjectionPoint injection = InjectionPoint::first_match,
                         const MatchInput& input = kLongMatch) {
    pineforge::native_module::Module<MatchHost> host;
    host.fail_at = nth;
    host.inject_first_match = injection == InjectionPoint::first_match;
    host.source = input.source;
    host.pattern = input.pattern;
    check(host.configure_native(native_spec()).status == pineforge::NativeSetupStatus::Applied,
          "the match host must configure before injection", nth);
    std::array<pf_bar_t, 3> bars{{
        {101, 103, 100, 102, 1, 60000},
        {102, 104, 101, 103, 1, 120000},
        {103, 105, 102, 104, 1, 180000},
    }};
    const auto handle = static_cast<pf_strategy_t>(
        static_cast<pineforge::BacktestEngine*>(&host));
    Report report;
    fail_alloc::arm(injection == InjectionPoint::whole_run ? nth : 0);
    try {
        pineforge::native_module::run_backtest<MatchHost>(
            handle, bars.data(), static_cast<int>(bars.size()), &report.value);
    } catch (...) {
        (void)fail_alloc::disarm();
        throw;
    }
    const std::size_t run_allocations = fail_alloc::attempts();
    const bool run_fired = fail_alloc::disarm();

    RunObservation observed;
    observed.allocations = injection == InjectionPoint::whole_run
        ? run_allocations : host.first_call_allocations;
    observed.fired = injection == InjectionPoint::whole_run ? run_fired : host.fired;
    observed.first_call_returned = host.first_call_returned;
    observed.answers_ok = host.answers_ok;
    observed.calls = host.calls;
    observed.status = strategy_last_run_status(handle);
    const char* code = strategy_get_last_error_code(handle);
    const char* text = strategy_get_last_error(handle);
    const char* args = strategy_get_last_error_args(handle);
    check(code != nullptr && text != nullptr && args != nullptr,
          "live failure getters must return text", nth);
    observed.code = code ? code : "";
    observed.text = text ? text : "";
    observed.args = args ? args : "";
    // docker/run_json.py: run_failure rejects any text, code or status 1,
    // and returns 1 before publishing a report. Retain results only on its
    // success branch; the raw C report may otherwise contain a partial run.
    if (!observed.failed()) {
        observed.trades = report.value.total_trades;
        observed.result = closed_trade_bytes(report.value);
        check(report.value.input_bars_processed == static_cast<int>(bars.size())
                  && report.value.script_bars_processed == static_cast<int>(bars.size()),
              "a completed report must account for the supplied bars", nth);
    }
    return observed;
}

void expect_failed_run(const RunObservation& observed, std::size_t nth) {
    if (observed.status != 1 || observed.code.empty() || observed.text.empty()) {
        std::fprintf(stderr, "run failure: status=%d code=%s text=%s (allocation %zu)\n",
                     observed.status, observed.code.c_str(), observed.text.c_str(), nth);
    }
    check(observed.failed(), "the harness must classify the run as failed", nth);
    check(observed.status == 1, "a failed run must report status 1", nth);
    check(!observed.code.empty() && !observed.text.empty(),
          "a failed run must carry a code and non-empty text", nth);
    check(observed.result.empty(), "the harness must publish no report after failure", nth);
}

void invalid_pattern_run_controls() {
    for (const auto& input : kInvalidPatterns) {
        const auto observed = run_match(0, InjectionPoint::first_match, input);
        expect_failed_run(observed, 0);
        check(observed.code == "pine_string_error", "invalid patterns must fail with pine_string_error");
        check(observed.args == kInvalidRegexArgs, "invalid patterns must carry invalid_regex_pattern");
        check(has_single_bar_prefix(observed.text, kInvalidRegexText),
              "invalid patterns must keep the exact error text after one valid bar prefix");
        check(observed.calls == 1 && !observed.first_call_returned,
              "an invalid pattern must stop the first bar's callback");
    }
}

void run_allocation_sweep() {
    const auto clean = run_match(0);
    check(clean.status == 0 && !clean.failed() && clean.answers_ok && clean.calls == 3,
          "the clean run must match on every supplied bar");
    check(clean.trades == 1 && !clean.result.empty(),
          "the clean run must publish the match-dependent closed trade");
    check(clean.allocations > 0, "the first bar's clean match must allocate");
    std::size_t out_of_memory_failures = 0;
    for (std::size_t nth = 1; nth <= clean.allocations; ++nth) {
        const auto observed = run_match(nth);
        check(observed.fired, "every first-bar match allocation must be swept", nth);
        check(!observed.first_call_returned && observed.calls == 1,
              "the failed first match must stop subsequent bars", nth);
        expect_failed_run(observed, nth);
        check(observed.code == "out_of_memory", "the harness must read out_of_memory", nth);
        if (observed.fired && observed.failed() && observed.code == "out_of_memory")
            ++out_of_memory_failures;
    }
    std::printf("first_match_sweep: swept all %zu run allocations, "
                "%zu out_of_memory failures, %zu typed setup refusals\n",
                clean.allocations, out_of_memory_failures, std::size_t{0});
}

void differential_run_sweep() {
    const auto clean = run_match(0, InjectionPoint::whole_run);
    check(clean.status == 0 && !clean.failed() && clean.trades == 1 && !clean.result.empty(),
          "the differential control must complete with its closed trade");
    check(clean.allocations > 0, "the complete clean run must allocate");
    std::size_t out_of_memory_failures = 0;
    std::size_t typed_setup_refusals = 0;
    std::vector<std::size_t> completed_indexes;
    // Fresh host/spec each time, injection over run_backtest including its
    // report capture. Fixture construction and oracle copies stay outside it.
    for (std::size_t nth = 1; nth <= clean.allocations; ++nth) {
        const auto observed = run_match(nth, InjectionPoint::whole_run);
        check(observed.fired, "every complete-run allocation must be swept", nth);
        if (observed.failed()) {
            const bool out_of_memory = observed.code == "out_of_memory";
            // A failed zone-resource read is an existing typed setup refusal.
            const bool typed_setup_refusal = observed.calls == 0
                && observed.code == "symbol_metadata_rejected";
            if ((!out_of_memory && !typed_setup_refusal) || observed.text.empty()) {
                std::fprintf(stderr, "run failure: status=%d code=%s text=%s (allocation %zu)\n",
                             observed.status, observed.code.c_str(), observed.text.c_str(), nth);
            }
            check(out_of_memory || typed_setup_refusal,
                  "a failed injected run must carry out_of_memory or a setup-only symbol_metadata_rejected",
                  nth);
            check(!observed.text.empty(),
                  "a failed injected run must carry non-empty failure text", nth);
            check(observed.result.empty() && observed.trades == 0,
                  "the harness must publish no report or closed trades after failure", nth);
            if (observed.fired) {
                if (out_of_memory) ++out_of_memory_failures;
                else if (typed_setup_refusal) ++typed_setup_refusals;
            }
        } else {
            completed_indexes.push_back(nth);
            check(observed.status == 0 && observed.result == clean.result,
                  "a completed injected run must keep a closed-trade list (and its totals) "
                  "identical to the clean run", nth);
        }
    }
    check(out_of_memory_failures > 0,
          "the complete-run sweep must reach an out_of_memory failure");
    std::printf("differential_run_sweep: swept all %zu run allocations, "
                "%zu out_of_memory failures, %zu typed setup refusals\n",
                clean.allocations, out_of_memory_failures, typed_setup_refusals);
    std::printf("differential_run_sweep: %zu of %zu injected runs completed, at allocation indexes: ",
                completed_indexes.size(), clean.allocations);
    for (std::size_t i = 0; i < completed_indexes.size(); ++i) {
        std::printf("%s%zu", i == 0 ? "" : ",", completed_indexes[i]);
    }
    std::putchar('\n');
}

} // namespace

int main() {
    try {
        match_allocation_sweep("simple_match_sweep", "abc123", "[0-9]+", "123");
        match_allocation_sweep("grouped_match_sweep", kLongMatch.source, kLongMatch.pattern,
                               "abcdefghijklmnopqrstuvwxyz0123456789");
        construction_resource_limit();
        invalid_pattern_controls();
        valid_pattern_controls();
        bar_prefix_controls();
        invalid_pattern_run_controls();
        run_allocation_sweep();
        differential_run_sweep();
    } catch (const std::exception& error) {
        (void)fail_alloc::disarm();
        std::fprintf(stderr, "FAIL unexpected exception: %s\n", error.what());
        return 1;
    } catch (...) {
        (void)fail_alloc::disarm();
        std::fputs("FAIL unexpected non-standard exception\n", stderr);
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
