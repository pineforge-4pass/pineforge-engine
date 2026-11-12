// C-boundary setup failures must remain visible through the following run.
#include "fail_alloc_seam.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/selected_window.h>
#include <pineforge/native_module.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
std::size_t allocation_index = 0;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL line %d allocation %zu: %s\n", __LINE__,     \
                     allocation_index, #condition);                            \
        ++failures;                                                           \
    }                                                                         \
} while (false)

constexpr const char* kTestTimezone = "America/Argentina/Buenos_Aires";
// Longer than the small-string buffers of the supported standard libraries.
constexpr const char* session = "0000-0759,0800-1559,1600-2359:1234567";
constexpr const char* metadata_key = "shares_outstanding_total";

class Host : public NativeStrategyHost {
public:
    int callbacks = 0;
    std::size_t* clean_first_bar_allocation = nullptr;

    void prepare_native_begin(const NativeBeginArgs&) override {
        NativeRunSpec spec;
        spec.identity = {"setter-failure", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:SETTERS";
        spec.timezone = syminfo_.timezone;
        spec.session = syminfo_.session;
        spec.initial_capital = 10000.0;
        spec.point_value = syminfo_.pointvalue;
        spec.account_fx = 1.0;
        spec.price_tick = syminfo_.mintick;
        spec.path_order = static_cast<NativePathOrder>(path_order_mode_);
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        if (configure_native(spec).status != NativeSetupStatus::Applied) {
            auto failure = run_failure_value_of(*this);
            throw coded<std::logic_error>(std::move(failure), last_error());
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (callbacks == 0 && clean_first_bar_allocation != nullptr)
            *clean_first_bar_allocation = fail_alloc::attempts();
        const double quantity = get_syminfo_metadata(metadata_key);
        if (callbacks == 0)
            (void)submit({native_order::Transact{quantity}, "entry", ""});
        if (callbacks == 2)
            (void)submit({native_order::Transact{-quantity}, "exit", ""});
        ++callbacks;
    }
};

// A real source host also projects configuration before the run status reset.
class SourceHost : public source::PineStrategyHost {
public:
    int callbacks = 0;
    void on_source_bar(const Bar& bar) override {
        if (bar.close > 0.0) ++callbacks;
    }
};

class AdmissionHost : public NativeStrategyHost {
public:
    int callbacks = 0;
    const std::string& symbol_timezone() const { return syminfo_.timezone; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
};

class RenderingFailureHost : public AdmissionHost {
public:
    int setter_calls = 0;
    void set_syminfo_metadata(const std::string&, double) override {
        ++setter_calls;
        throw std::bad_alloc();
    }
};

class DurableFailureHost : public Host {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        throw coded<std::runtime_error>(RunFailureCode::strategy_runtime_error, {},
                                         "original run failure");
    }
};

class SelectedQuoteHost final : public source::PineStrategyHost {
public:
    int callbacks = 0;

    SelectedQuoteHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        if (bar_index_ == 0 || bar_index_ == 1) strategy_entry("open", true);
        if (bar_index_ == 2) strategy_close("open", {}, 1.0);
    }
};

void check_quote(bool condition, const char* expected, const SelectedQuoteHost& host,
                 int line) {
    if (condition) return;
    std::fprintf(stderr,
        "FAIL line %d: expected %s; observed status=%d code='%s' text='%s' "
        "lifecycle=%u callbacks=%d\n",
        line, expected, host.last_run_status(), run_failure_code_of(host),
        host.last_error().c_str(), static_cast<unsigned>(host.native_state().kind),
        host.callbacks);
    ++failures;
}

#define CHECK_QUOTE(host, condition) \
    check_quote(static_cast<bool>(condition), #condition, (host), __LINE__)

void check_sequence(bool condition, const char* row, const char* expected,
                    const BacktestEngine& host, int line) {
    if (condition) return;
    std::fprintf(stderr,
        "FAIL %s line %d: expected %s; observed status=%d code='%s' args='%s' "
        "text='%s' lifecycle=%u\n",
        row, line, expected, host.last_run_status(), run_failure_code_of(host),
        run_failure_args_of(host), host.last_error().c_str(),
        static_cast<unsigned>(static_cast<const NativeStrategyHost&>(host).native_state().kind));
    ++failures;
}

#define CHECK_SEQUENCE(row, host, condition) \
    check_sequence(static_cast<bool>(condition), (row), #condition, (host), __LINE__)

struct Report {
    pf_report_t value{};
    ~Report() { native_module::report_free(&value); }
};

template <typename H>
pf_strategy_t handle(native_module::Module<H>& host) {
    return static_cast<BacktestEngine*>(&host);
}

template <typename H>
void run(native_module::Module<H>& host, Report& report) {
    pf_bar_t bars[] = {
        {100, 101, 99, 100, 1, 1736121600000LL},
        {102, 103, 101, 102, 2, 1736121660000LL},
        {104, 105, 103, 104, 3, 1736121720000LL},
        {106, 107, 105, 106, 4, 1736121780000LL},
    };
    native_module::run_backtest_full<H>(handle(host), bars, 4, "1", "1", 0, 4,
                                       PF_MAGNIFIER_ENDPOINTS, &report.value);
}

void configure(pf_strategy_t strategy) {
    strategy_set_syminfo_timezone(strategy, kTestTimezone);
    strategy_set_syminfo_session(strategy, session);
    strategy_set_syminfo_metadata(strategy, metadata_key, 2.0);
    strategy_set_syminfo_mintick(strategy, 0.25);
    strategy_set_syminfo_pointvalue(strategy, 50.0);
    strategy_set_path_order(strategy, 2);
}

void check_failure(pf_strategy_t strategy, const char* code) {
    const int status = strategy_last_run_status(strategy);
    const char* text = strategy_get_last_error(strategy);
    const char* actual_code = strategy_get_last_error_code(strategy);
    CHECK(status == 1 || (text != nullptr && text[0] != '\0')
          || (actual_code != nullptr && actual_code[0] != '\0'));
    CHECK(text != nullptr && text[0] != '\0');
    CHECK(actual_code != nullptr && std::strcmp(actual_code, code) == 0);
}

void expect_sequence_refusal(const char* row, BacktestEngine& host,
                             const char* code, const char* text, const char* args) {
    const pf_strategy_t strategy = &host;
    CHECK_SEQUENCE(row, host, strategy_last_run_status(strategy) == 1);
    CHECK_SEQUENCE(row, host, std::strcmp(strategy_get_last_error_code(strategy), code) == 0);
    CHECK_SEQUENCE(row, host, std::strcmp(strategy_get_last_error_args(strategy), args) == 0);
    CHECK_SEQUENCE(row, host, strategy_get_last_error(strategy)[0] != '\0');
    if (text != nullptr)
        CHECK_SEQUENCE(row, host, std::strcmp(strategy_get_last_error(strategy), text) == 0);
}

void expect_no_sequence_report(const char* row, const BacktestEngine& host,
                               const Report& report) {
    CHECK_SEQUENCE(row, host, report.value.input_bars_processed == 0);
    CHECK_SEQUENCE(row, host, report.value.trades == nullptr && report.value.trades_len == 0);
    CHECK_SEQUENCE(row, host,
        report.value.equity_curve == nullptr && report.value.equity_curve_len == 0);
}

using StringSetter = void (*)(pf_strategy_t, const char*);

void metadata_setter(pf_strategy_t strategy, const char* key) {
    strategy_set_syminfo_metadata(strategy, key, 2.0);
}

template <typename H>
void sweep_setter(StringSetter setter, const char* value) {
    std::size_t count;
    {
        native_module::Module<H> host;
        fail_alloc::arm(0);
        setter(handle(host), value);
        count = fail_alloc::attempts();
        CHECK(!fail_alloc::disarm());
        CHECK(strategy_last_run_status(handle(host)) == 0);
        CHECK(strategy_get_last_error_code(handle(host))[0] == '\0');
    }
    CHECK(count > 0);  // The string/map call must actually allocate.
    for (std::size_t nth = 1; nth <= count; ++nth) {
        allocation_index = nth;
        native_module::Module<H> host;
        Report report;
        fail_alloc::arm(nth);
        setter(handle(host), value);
        const bool fired = fail_alloc::disarm();
        CHECK(fired);
        // Base drops the exception: all three reads are clean here.
        check_failure(handle(host), "out_of_memory");
        const std::string text = strategy_get_last_error(handle(host));
        // Clearing presentation during later setup must not erase this cause.
        clear_run_failure(host);
        note_run_failure(host, "later setup error", RunFailureCode::engine_invariant);
        check_failure(handle(host), "out_of_memory");
        CHECK(text == strategy_get_last_error(handle(host)));
        run(host, report);
        // A begin must not clear the failed setup and run with defaults.
        check_failure(handle(host), "out_of_memory");
        CHECK(text == strategy_get_last_error(handle(host)));
        CHECK(host.callbacks == 0);
        CHECK(report.value.trades == nullptr && report.value.trades_len == 0);
        CHECK(report.value.equity_curve == nullptr && report.value.equity_curve_len == 0);
        // The refused attempt consumes the setup latch, not the handle.
        // A repaired second run follows the same path as a clean fresh handle.
        configure(handle(host));
        run(host, report);
        CHECK(strategy_last_run_status(handle(host)) == 0);
        CHECK(strategy_get_last_error(handle(host))[0] == '\0');
        CHECK(strategy_get_last_error_code(handle(host))[0] == '\0');
        CHECK(host.callbacks == 4);
    }
    allocation_index = 0;
}

template <typename T>
void append(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof value);
}

// Serialize fields, not pointer addresses or padding. Compare the clean run's
// closed-trade list and its totals, equity series, and processed counts.
std::string report_bytes(const pf_report_t& report) {
    std::string bytes;
    append(bytes, report.input_bars_processed);
    append(bytes, report.script_bars_processed);
    append(bytes, report.total_trades);
    append(bytes, report.net_profit);
    append(bytes, report.trades_len);
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& trade = report.trades[i];
        append(bytes, trade.entry_time); append(bytes, trade.exit_time);
        append(bytes, trade.entry_price); append(bytes, trade.exit_price);
        append(bytes, trade.pnl); append(bytes, trade.pnl_pct);
        append(bytes, trade.is_long); append(bytes, trade.max_runup);
        append(bytes, trade.max_drawdown); append(bytes, trade.qty);
        append(bytes, trade.commission); append(bytes, trade.entry_bar_index);
        append(bytes, trade.exit_bar_index); append(bytes, trade.open_at_end);
    }
    append(bytes, report.equity_curve_len);
    for (int i = 0; i < report.equity_curve_len; ++i) {
        append(bytes, report.equity_curve[i].time_ms);
        append(bytes, report.equity_curve[i].equity);
        append(bytes, report.equity_curve[i].open_profit);
    }
    return bytes;
}

void selected_quote_refusal_keeps_run_clean() {
    // Match the source-run fixture of test_selected_quote_seal: 61 minute
    // inputs, a 15-minute chart, magnification, and a window over the whole feed.
    // The native-module wrapper cannot drive this shape: it rejects magnification.
    // expectation corrected: 4 input bars -> 61 input bars, because this control
    // now uses the magnified source fixture. Callback counts are checked against
    // the engine's chart-bar count and an independent fresh handle.
    std::vector<Bar> bars;
    bars.reserve(61);
    for (int i = 0; i <= 60; ++i) {
        const double price = 100.0 + i * 0.125;
        bars.push_back({price, price + 0.25, price - 0.25, price + 0.125,
                        1.0, 1736121600000LL + i * 60000LL});
    }
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = bars.front().timestamp;
    window.end_ms = bars.back().timestamp + 1000 * 60000LL;
    const double quote_time = static_cast<double>(bars.back().timestamp);

    SelectedQuoteHost host;
    const pf_strategy_t strategy = static_cast<BacktestEngine*>(&host);
    const int configured = strategy_set_selected_window_v1(strategy, &window);
    CHECK_QUOTE(host, configured == 0);
    strategy_set_syminfo_metadata(strategy, "report_terminal_quote_time_ms", quote_time);
    strategy_set_syminfo_metadata(strategy, "report_terminal_quote_close", 112.0);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
    CHECK_QUOTE(host, strategy_last_run_status(strategy) == 0);
    CHECK_QUOTE(host, strategy_get_last_error(strategy)[0] == '\0');
    CHECK_QUOTE(host, strategy_get_last_error_code(strategy)[0] == '\0');
    Report first;
    native_module::publish_report(&host, &first.value);
    CHECK_QUOTE(host, host.callbacks > 0);
    CHECK_QUOTE(host, first.value.script_bars_processed == host.callbacks);
    CHECK_QUOTE(host, first.value.input_bars_processed == static_cast<int>(bars.size()));
    CHECK_QUOTE(host, first.value.trades_len > 0);
    CHECK_QUOTE(host, strategy_state_query_status_v1(strategy)
          == PF_STATE_QUERY_SELECTED_WINDOW_AFTER_SEAL_V1);
    const auto before = host.native_state();
    const auto cause = run_failure_value_of(host);
    const std::string args = strategy_get_last_error_args(strategy);
    CHECK_QUOTE(host, before.kind == NativeLifecycleKind::Completed);
    CHECK_QUOTE(host, cause.code == RunFailureCode::none);

    // This named refusal is a clean-run control on both the base and the fix.
    // A different close would change the following run if this write were accepted.
    strategy_set_syminfo_metadata(strategy, "report_terminal_quote_close", 105.0);
    CHECK_QUOTE(host, strategy_last_run_status(strategy) == 0);
    CHECK_QUOTE(host, strategy_get_last_error(strategy)[0] == '\0');
    CHECK_QUOTE(host, strategy_get_last_error_code(strategy)[0] == '\0');
    CHECK_QUOTE(host, args == strategy_get_last_error_args(strategy));
    const auto after = host.native_state();
    const auto remaining_cause = run_failure_value_of(host);
    CHECK_QUOTE(host, remaining_cause.code == cause.code && remaining_cause.args == cause.args);
    CHECK_QUOTE(host, after.kind == before.kind);
    CHECK_QUOTE(host, after.failure.code == before.failure.code);
    CHECK_QUOTE(host, after.failure.operation == before.failure.operation);
    CHECK_QUOTE(host, after.failure.ordinal == before.failure.ordinal);
    CHECK_QUOTE(host, after.failure.discriminator == before.failure.discriminator);
    CHECK_QUOTE(host, strategy_state_query_status_v1(strategy)
          == PF_STATE_QUERY_SELECTED_WINDOW_AFTER_SEAL_V1);
    Report after_write;
    native_module::publish_report(&host, &after_write.value);
    CHECK_QUOTE(host, report_bytes(after_write.value) == report_bytes(first.value));

    // A following begin must not consume an unexpected-exception latch.
    const int callbacks_before = host.callbacks;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
    CHECK_QUOTE(host, strategy_last_run_status(strategy) == 0);
    CHECK_QUOTE(host, strategy_get_last_error(strategy)[0] == '\0');
    CHECK_QUOTE(host, strategy_get_last_error_code(strategy)[0] == '\0');
    CHECK_QUOTE(host, host.native_state().kind == NativeLifecycleKind::Completed);
    Report following;
    native_module::publish_report(&host, &following.value);

    SelectedQuoteHost fresh;
    const pf_strategy_t fresh_strategy = static_cast<BacktestEngine*>(&fresh);
    const int fresh_configured = strategy_set_selected_window_v1(fresh_strategy, &window);
    CHECK_QUOTE(fresh, fresh_configured == 0);
    strategy_set_syminfo_metadata(fresh_strategy, "report_terminal_quote_time_ms", quote_time);
    strategy_set_syminfo_metadata(fresh_strategy, "report_terminal_quote_close", 112.0);
    fresh.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
    CHECK_QUOTE(fresh, strategy_last_run_status(fresh_strategy) == 0);
    CHECK_QUOTE(fresh, strategy_get_last_error(fresh_strategy)[0] == '\0');
    CHECK_QUOTE(fresh, strategy_get_last_error_code(fresh_strategy)[0] == '\0');
    CHECK_QUOTE(fresh, fresh.native_state().kind == NativeLifecycleKind::Completed);
    Report clean;
    native_module::publish_report(&fresh, &clean.value);
    CHECK_QUOTE(fresh, fresh.callbacks > 0);
    CHECK_QUOTE(fresh, clean.value.script_bars_processed == fresh.callbacks);
    CHECK_QUOTE(fresh, clean.value.input_bars_processed == static_cast<int>(bars.size()));
    CHECK_QUOTE(fresh, clean.value.trades_len > 0);
    CHECK_QUOTE(host, host.callbacks - callbacks_before == fresh.callbacks);
    CHECK_QUOTE(host, following.value.input_bars_processed == static_cast<int>(bars.size()));
    CHECK_QUOTE(host, report_bytes(following.value) == report_bytes(clean.value));
}

struct Outcome {
    std::size_t attempts = 0;
    std::size_t first_bar_allocation = 0;
    bool fired = false;
    int status = -1;
    std::string code, text, bytes;
    bool failed() const { return status == 1 || !code.empty() || !text.empty(); }
};

Outcome complete_run(std::size_t nth) {
    native_module::Module<Host> host;
    Report report;
    Outcome out;
    // Only an uninjected run may supply the phase boundary.
    if (nth == 0) host.clean_first_bar_allocation = &out.first_bar_allocation;
    // Handle construction is outside the operation: all setter, configure,
    // callback and report allocations are inside it, with no sampled indices.
    fail_alloc::arm(nth);
    configure(handle(host));
    run(host, report);
    if (nth == 0) host.clean_first_bar_allocation = nullptr;
    out.attempts = fail_alloc::attempts();
    out.fired = fail_alloc::disarm();
    out.status = strategy_last_run_status(handle(host));
    out.code = strategy_get_last_error_code(handle(host));
    out.text = strategy_get_last_error(handle(host));
    // Like run_json.run_failure, decide failure before reading report arrays.
    // A failed C report can hold private partial rows; the harness publishes
    // neither report bytes nor a closed-trade result on that branch.
    if (!out.failed()) {
        CHECK(report.value.trades_len == 1);
        CHECK(report.value.input_bars_processed == 4);
        if (report.value.trades_len == 1) CHECK(report.value.trades[0].qty == 2.0);
        out.bytes = report_bytes(report.value);
    }
    return out;
}

static std::string compact_allocation_ranges(const std::vector<std::size_t>& indexes) {
    std::string ranges;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        const std::size_t first = indexes[i];
        while (i + 1 < indexes.size() && indexes[i + 1] - indexes[i] == 1) ++i;
        if (!ranges.empty()) ranges += ",";
        ranges += std::to_string(first);
        if (indexes[i] != first) ranges += "-" + std::to_string(indexes[i]);
    }
    return ranges;
}

static constexpr const char* kSetupPhaseCodes[] = {
    // src/run_failure.cpp:425 classifies propagated std::bad_alloc.
    "out_of_memory",
    // src/native_run_spec.cpp:872 -> AllocationFailure; src/native_execution_consumer.cpp:2956 -> engine_invariant.
    "engine_invariant",
    // src/native_calendar.cpp:880 catches resource failure; src/native_execution_consumer.cpp:3102 -> symbol_metadata_rejected.
    "symbol_metadata_rejected",
};

void differential_sweep() {
    // Warm process-wide calendar/standard-library caches before counting.
    CHECK(!complete_run(0).failed());
    const Outcome clean = complete_run(0);
    CHECK(!clean.failed() && !clean.bytes.empty() && clean.attempts > 0);
    const std::size_t first_bar_allocation = clean.first_bar_allocation;
    CHECK(first_bar_allocation > 0 && first_bar_allocation < clean.attempts);
    std::printf("differential_sweep: the first clean bar callback follows run allocation %zu\n",
                first_bar_allocation);
    std::size_t out_of_memory_failures = 0;
    std::size_t typed_setup_refusals = 0;
    std::map<std::string, std::size_t> setup_phase_codes;
    std::vector<std::size_t> completed_indexes;
    for (std::size_t nth = 1; nth <= clean.attempts; ++nth) {
        allocation_index = nth;
        const Outcome observed = complete_run(nth);
        CHECK(observed.fired);
        if (observed.failed()) {
            if (nth <= first_bar_allocation) {
                ++setup_phase_codes[observed.code];
                bool named_setup_code = false;
                for (const char* code : kSetupPhaseCodes)
                    named_setup_code = named_setup_code || observed.code == code;
                if (!named_setup_code || observed.text.empty()) {
                    std::fprintf(stderr,
                        "FAIL line %d allocation %zu: expected a typed setup refusal; "
                        "observed status=%d code='%s' text='%s'\n",
                        __LINE__, nth, observed.status, observed.code.c_str(),
                        observed.text.c_str());
                }
                CHECK(!observed.code.empty());
                CHECK(named_setup_code);
                if (!observed.code.empty() && !observed.text.empty()) ++typed_setup_refusals;
            } else {
                if (observed.code != "out_of_memory" || observed.text.empty()) {
                    std::fprintf(stderr,
                        "FAIL line %d allocation %zu: expected out_of_memory after the "
                        "first clean bar boundary %zu; observed status=%d code='%s' text='%s'\n",
                        __LINE__, nth, first_bar_allocation, observed.status,
                        observed.code.c_str(), observed.text.c_str());
                }
                CHECK(observed.code == "out_of_memory");
                if (observed.code == "out_of_memory" && !observed.text.empty())
                    ++out_of_memory_failures;
            }
            CHECK(!observed.text.empty());
            CHECK(observed.bytes.empty());
        } else {
            completed_indexes.push_back(nth);
            CHECK(observed.status == 0);
            CHECK(observed.bytes == clean.bytes);
        }
    }
    allocation_index = 0;
    CHECK(out_of_memory_failures > 0);
    std::printf("differential_sweep: swept all %zu run allocations, "
                "%zu out_of_memory failures, %zu typed setup refusals\n",
                clean.attempts, out_of_memory_failures, typed_setup_refusals);
    std::string code_counts;
    for (const auto& entry : setup_phase_codes) {
        if (!code_counts.empty()) code_counts += ",";
        code_counts += entry.first + "=" + std::to_string(entry.second);
    }
    std::printf("differential_sweep: setup-phase codes: %s\n", code_counts.c_str());
    const std::string ranges = compact_allocation_ranges(completed_indexes);
    std::printf("differential_sweep: %zu of %zu injected runs completed, "
                "at allocation indexes: %s\n",
                completed_indexes.size(), clean.attempts, ranges.c_str());
}

void invalid_values() {
    struct NumericCase {
        void (*setter)(pf_strategy_t, double);
        double value;
        const char* key;
        const char* shown;
        const char* args;
    };
    const NumericCase rows[] = {
        {strategy_set_syminfo_mintick, 0.0, "price tick", "0", "{\"field\":\"price_tick\"}"},
        {strategy_set_syminfo_mintick, -0.01, "price tick", "-0.01", "{\"field\":\"price_tick\"}"},
        {strategy_set_syminfo_mintick, std::numeric_limits<double>::quiet_NaN(),
         "price tick", "nan", "{\"field\":\"price_tick\"}"},
        {strategy_set_syminfo_pointvalue, 0.0, "point value", "0", "{\"field\":\"point_value\"}"},
        {strategy_set_syminfo_pointvalue, -50.0, "point value", "-50",
         "{\"field\":\"point_value\"}"},
    };
    for (const auto& row : rows) {
        native_module::Module<Host> host;
        const auto strategy = handle(host);
        row.setter(strategy, row.value);
        // Base silently keeps the default and exposes no failure.
        CHECK(strategy_last_run_status(strategy) == 1);
        check_failure(strategy, "symbol_metadata_rejected");
        CHECK(std::strstr(strategy_get_last_error(strategy), row.key) != nullptr);
        CHECK(std::strstr(strategy_get_last_error(strategy), row.shown) != nullptr);
        CHECK(std::strcmp(strategy_get_last_error_args(strategy), row.args) == 0);
        Report report;
        run(host, report);
        check_failure(strategy, "symbol_metadata_rejected");
        CHECK(host.callbacks == 0 && report.value.trades_len == 0);
    }
    for (int mode : {3, -1}) {
        native_module::Module<Host> host;
        const auto strategy = handle(host);
        strategy_set_path_order(strategy, mode);
        // Base clamps these modes to AUTO without a diagnostic.
        CHECK(strategy_last_run_status(strategy) == 1);
        check_failure(strategy, "setting_rejected");
        CHECK(std::strstr(strategy_get_last_error(strategy), "path_order") != nullptr);
        CHECK(std::strstr(strategy_get_last_error(strategy),
                          std::to_string(mode).c_str()) != nullptr);
        Report report;
        run(host, report);
        check_failure(strategy, "setting_rejected");
        CHECK(host.callbacks == 0 && report.value.trades_len == 0);
    }
}

void valid_values_and_null() {
    const double ticks[] = {0.00001, 0.25, 1.0};
    const double points[] = {0.5, 1.0, 50.0};
    for (int mode : {0, 1, 2}) {
        native_module::Module<Host> host;
        const auto strategy = handle(host);
        configure(strategy);
        strategy_set_syminfo_mintick(strategy, ticks[mode]);
        strategy_set_syminfo_pointvalue(strategy, points[mode]);
        strategy_set_path_order(strategy, mode);
        Report report;
        run(host, report);
        CHECK(strategy_last_run_status(strategy) == 0);
        CHECK(strategy_get_last_error(strategy)[0] == '\0');
        CHECK(strategy_get_last_error_code(strategy)[0] == '\0');
        const auto state = host.native_state();
        CHECK(state.spec != nullptr);
        if (state.spec) {
            CHECK(state.spec->price_tick == ticks[mode]);
            CHECK(state.spec->point_value == points[mode]);
            CHECK(state.spec->path_order == static_cast<NativePathOrder>(mode));
        }
        CHECK(strategy_report_pointvalue(strategy) == points[mode]);
        CHECK(host.callbacks == 4 && report.value.trades_len == 1);
    }
    strategy_set_syminfo_timezone(nullptr, kTestTimezone);
    strategy_set_syminfo_session(nullptr, session);
    strategy_set_syminfo_metadata(nullptr, metadata_key, 2.0);
    strategy_set_syminfo_mintick(nullptr, 0.0);
    strategy_set_syminfo_pointvalue(nullptr, -50.0);
    strategy_set_path_order(nullptr, 3);
    CHECK(strategy_last_run_status(nullptr) == -1);
    CHECK(strategy_get_last_error(nullptr) == nullptr);
    CHECK(strategy_get_last_error_code(nullptr) == nullptr);
}

void repeated_path_refusal_sequence() {
    const char* row = "S1 repeated path-order refusal";
    native_module::Module<SourceHost> host;
    const auto strategy = handle(host);
    configure(strategy);
    strategy_set_path_order(strategy, 3);
    Report first;
    run(host, first);
    expect_sequence_refusal(row, host, "setting_rejected",
        "Error before the first bar: path_order must be 0, 1 or 2, got 3",
        "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
    expect_no_sequence_report(row, host, first);
    CHECK_SEQUENCE(row, host, host.callbacks == 0);

    strategy_set_path_order(strategy, 3);
    Report second;
    run(host, second);
    expect_sequence_refusal(row, host, "setting_rejected",
        "Error before the first bar: path_order must be 0, 1 or 2, got 3",
        "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
    expect_no_sequence_report(row, host, second);
    CHECK_SEQUENCE(row, host, host.callbacks == 0);

    strategy_set_path_order(strategy, 1);
    const int callbacks_before = host.callbacks;
    Report repaired;
    run(host, repaired);
    CHECK_SEQUENCE(row, host, strategy_last_run_status(strategy) == 0);
    CHECK_SEQUENCE(row, host, strategy_get_last_error(strategy)[0] == '\0');
    CHECK_SEQUENCE(row, host, strategy_get_last_error_code(strategy)[0] == '\0');
    CHECK_SEQUENCE(row, host, host.callbacks - callbacks_before == 4);
    CHECK_SEQUENCE(row, host, repaired.value.input_bars_processed == 4);
}

void replacement_numeric_refusal_sequence() {
    const char* row = "S2 numeric refusal replaces consumed path refusal";
    native_module::Module<SourceHost> host;
    const auto strategy = handle(host);
    configure(strategy);
    strategy_set_path_order(strategy, 3);
    Report first;
    run(host, first);
    expect_sequence_refusal(row, host, "setting_rejected",
        "Error before the first bar: path_order must be 0, 1 or 2, got 3",
        "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
    expect_no_sequence_report(row, host, first);

    strategy_set_syminfo_mintick(strategy, 0.0);
    expect_sequence_refusal(row, host, "symbol_metadata_rejected",
        "Error before the first bar: price tick must be positive, got 0.000000",
        "{\"field\":\"price_tick\"}");
    Report second;
    run(host, second);
    expect_sequence_refusal(row, host, "symbol_metadata_rejected",
        "Error before the first bar: price tick must be positive, got 0.000000",
        "{\"field\":\"price_tick\"}");
    expect_no_sequence_report(row, host, second);
    CHECK_SEQUENCE(row, host, host.callbacks == 0);

    strategy_set_syminfo_mintick(strategy, 0.5);
    strategy_set_path_order(strategy, 0);
    const int callbacks_before = host.callbacks;
    Report repaired;
    run(host, repaired);
    CHECK_SEQUENCE(row, host, strategy_last_run_status(strategy) == 0);
    CHECK_SEQUENCE(row, host, strategy_get_last_error(strategy)[0] == '\0');
    CHECK_SEQUENCE(row, host, strategy_get_last_error_code(strategy)[0] == '\0');
    CHECK_SEQUENCE(row, host, host.callbacks - callbacks_before == 4);
    CHECK_SEQUENCE(row, host, repaired.value.input_bars_processed == 4);
}

void allocation_after_admission_refusal_sequence() {
    const char* row = "S3 allocation failure replaces admission refusal";
    native_module::Module<AdmissionHost> host;
    const auto strategy = handle(host);
    NativeRunSpec spec;
    spec.identity = {"setter-sequence", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:SETTERS";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    const auto configured = host.configure_native(spec);
    CHECK_SEQUENCE(row, host, configured.status == NativeSetupStatus::Applied);

    pf_bar_t bars[] = {
        {100, 101, 99, 100, 1, 1736121600000LL},
        {102, 103, 101, 102, 2, 1736121660000LL},
        {104, 105, 103, 104, 3, 1736121720000LL},
        {106, 107, 105, 106, 4, 1736121780000LL},
    };
    Report initial;
    native_module::run_backtest_full<AdmissionHost>(strategy, bars, 4, "2", "1", 0, 4,
        PF_MAGNIFIER_ENDPOINTS, &initial.value);
    expect_sequence_refusal(row, host, "run_options_rejected",
        "native timeframe arguments must be empty or match the spec",
        "{\"option\":\"timeframes\"}");
    expect_no_sequence_report(row, host, initial);
    CHECK_SEQUENCE(row, host, host.native_state().kind == NativeLifecycleKind::Ready);
    CHECK_SEQUENCE(row, host, host.callbacks == 0);
    CHECK_SEQUENCE(row, host, host.symbol_timezone() == "UTC");

    fail_alloc::arm(1);
    strategy_set_syminfo_timezone(strategy, kTestTimezone);
    const bool fired = fail_alloc::disarm();
    CHECK_SEQUENCE(row, host, fired);
    expect_sequence_refusal(row, host, "out_of_memory", nullptr, "{}");
    CHECK_SEQUENCE(row, host, std::strcmp(strategy_get_last_error(strategy),
        "native timeframe arguments must be empty or match the spec") != 0);
    CHECK_SEQUENCE(row, host, host.symbol_timezone() == "UTC");
    Report next;
    run(host, next);
    expect_sequence_refusal(row, host, "out_of_memory", nullptr, "{}");
    expect_no_sequence_report(row, host, next);
    CHECK_SEQUENCE(row, host, host.callbacks == 0);
    CHECK_SEQUENCE(row, host, host.native_state().kind == NativeLifecycleKind::Ready);
}

void first_cause_sequence_controls() {
    {
        const char* row = "S4 unconsumed setup first cause";
        native_module::Module<SourceHost> host;
        const auto strategy = handle(host);
        configure(strategy);
        strategy_set_path_order(strategy, 3);
        strategy_set_syminfo_mintick(strategy, 0.0);
        expect_sequence_refusal(row, host, "setting_rejected",
            "Error before the first bar: path_order must be 0, 1 or 2, got 3",
            "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
        Report refused;
        run(host, refused);
        expect_sequence_refusal(row, host, "setting_rejected",
            "Error before the first bar: path_order must be 0, 1 or 2, got 3",
            "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
        expect_no_sequence_report(row, host, refused);
        CHECK_SEQUENCE(row, host, host.callbacks == 0);
    }
    {
        const char* row = "S4 durable run first cause and int boundary";
        native_module::Module<DurableFailureHost> host;
        const auto strategy = handle(host);
        configure(strategy);
        Report failed;
        run(host, failed);
        expect_sequence_refusal(row, host, "strategy_runtime_error",
            "original run failure", "{}");
        CHECK_SEQUENCE(row, host, host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK_SEQUENCE(row, host, host.callbacks == 1);
        strategy_set_path_order(strategy, 3);
        // An existing run failure is preserved, not reformatted as a setter failure.
        expect_sequence_refusal(row, host, "strategy_runtime_error",
            "original run failure", "{}");
        const int refused = strategy_set_syminfo_string(strategy, "currency", "EUR");
        CHECK_SEQUENCE(row, host, refused == -1);
        expect_sequence_refusal(row, host, "strategy_runtime_error",
            "original run failure", "{}");
        CHECK_SEQUENCE(row, host, host.native_state().kind == NativeLifecycleKind::Failed);
    }
}

void void_setter_prefix_controls() {
    {
        const char* row = "void setter setup prefix";
        native_module::Module<SourceHost> host;
        const auto strategy = handle(host);
        CHECK_SEQUENCE(row, host, !ta::bar_context().installed);
        strategy_set_path_order(strategy, 3);
        const std::string text = strategy_get_last_error(strategy);
        constexpr char prefix[] = "Error before the first bar: ";
        constexpr char bare[] = "path_order must be 0, 1 or 2, got 3";
        CHECK_SEQUENCE(row, host, text.compare(0, sizeof(prefix) - 1, prefix) == 0);
        CHECK_SEQUENCE(row, host, text.size() >= sizeof(bare) - 1
            && text.compare(text.size() - (sizeof(bare) - 1), sizeof(bare) - 1, bare) == 0);
        expect_sequence_refusal(row, host, "setting_rejected",
            "Error before the first bar: path_order must be 0, 1 or 2, got 3",
            "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
    }
    {
        const char* row = "void setter bar context prefix";
        native_module::Module<SourceHost> host;
        const auto strategy = handle(host);
        CHECK_SEQUENCE(row, host, !ta::bar_context().installed);
        {
            ta::BarContextScope scope(7, 0);
            strategy_set_path_order(strategy, 3);
        }
        CHECK_SEQUENCE(row, host, !ta::bar_context().installed);
        const std::string text = strategy_get_last_error(strategy);
        constexpr char prefix[] = "Error on bar 7: ";
        CHECK_SEQUENCE(row, host, text.compare(0, sizeof(prefix) - 1, prefix) == 0);
        expect_sequence_refusal(row, host, "setting_rejected",
            "Error on bar 7: path_order must be 0, 1 or 2, got 3",
            "{\"input\":\"path_order\",\"reason\":\"invalid_enum_option\"}");
    }
    {
        const char* row = "void setter rendering fallback";
        native_module::Module<RenderingFailureHost> host;
        const auto strategy = handle(host);
        CHECK_SEQUENCE(row, host, !ta::bar_context().installed);
        // The short key fits the supported small-string buffers and the
        // fixture throws without new. The setup prefix exceeds those buffers,
        // so arm(1) refuses its first rendering allocation rather than setup.
        fail_alloc::arm(1);
        strategy_set_syminfo_metadata(strategy, "x", 1.0);
        const bool fired = fail_alloc::disarm();
        CHECK_SEQUENCE(row, host, fired);
        CHECK_SEQUENCE(row, host, host.setter_calls == 1);
        expect_sequence_refusal(row, host, "out_of_memory", "setter failed", "{}");
        Report refused;
        run(host, refused);
        expect_sequence_refusal(row, host, "out_of_memory", "setter failed", "{}");
        expect_no_sequence_report(row, host, refused);
        CHECK_SEQUENCE(row, host, host.callbacks == 0);
    }
}
} // namespace

int main() {
    try {
        sweep_setter<Host>(strategy_set_syminfo_timezone, kTestTimezone);
        sweep_setter<Host>(strategy_set_syminfo_session, session);
        sweep_setter<Host>(metadata_setter, metadata_key);
        sweep_setter<SourceHost>(strategy_set_syminfo_timezone, kTestTimezone);
        sweep_setter<SourceHost>(strategy_set_syminfo_session, session);
        sweep_setter<SourceHost>(metadata_setter, metadata_key);
        differential_sweep();
        invalid_values();
        valid_values_and_null();
        selected_quote_refusal_keeps_run_clean();
        repeated_path_refusal_sequence();
        replacement_numeric_refusal_sequence();
        allocation_after_admission_refusal_sequence();
        first_cause_sequence_controls();
        void_setter_prefix_controls();
    } catch (const std::exception& error) {
        fail_alloc::disarm();
        std::fprintf(stderr, "unexpected exception: %s\n", error.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
