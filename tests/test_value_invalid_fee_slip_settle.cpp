// Rows 11, 12, 13, 14, 21, 22, 23. Expectations were pinned in
// .lane-out/MAP.md before product edits. No assert(): Release defines NDEBUG.
// Scripted callbacks drive the public native C ABI. The source fixture also
// supplies the ordinary per-strategy run_backtest_full C ABI bridge.
//
// Build both this TU and the archive with PINEFORGE_VALUE_INVALID_FIXTURES.
// PINEFORGE_VALUE_INVALID_KERNEL_ONLY omits the source-layer half in the
// kernel profile. Neither definition is a production feature or ABI export.
// The supervisor executes red/green and records the fail-before diagnostic.
#include <pineforge/native_c_api.h>
#include <pineforge/run_failure.hpp>

#ifndef PINEFORGE_VALUE_INVALID_KERNEL_ONLY
#include <pineforge/source/pine_strategy_host.hpp>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pineforge::internal {
void set_fused_settlement(bool) noexcept;
void fixture_execution_values(double (*)(const char*, double)) noexcept;
void fixture_execution_counters(std::int64_t (*)(const char*, std::int64_t)) noexcept;
}
#ifndef PINEFORGE_VALUE_INVALID_KERNEL_ONLY
namespace pineforge::source {
void fixture_adapter_execution_values(double (*)(const char*, double)) noexcept;
}
#endif

namespace {
int checks = 0;
int failures = 0;
const char* selected = nullptr;
const char* active_row = "";

void check(bool ok, const char* expression, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::fprintf(stderr, "FAIL %s line %d: %s\n", active_row, line, expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

bool row(const char* name) {
    active_row = name;
    return !selected || std::strcmp(selected, name) == 0;
}

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

const double infinity = std::numeric_limits<double>::infinity();
const double nan = std::numeric_limits<double>::quiet_NaN();

struct Injection {
    const char* site = nullptr;
    double value = 0.0;
    int hits = 0;
    int skip_hits = 0;
    std::vector<std::pair<std::string, std::uint64_t>> observed;
} injection;

double fixture_value(const char* site, double value) {
    injection.observed.emplace_back(site, bits(value));
    if (!injection.site || std::strcmp(injection.site, site) != 0) return value;
    ++injection.hits;
    if (injection.hits <= injection.skip_hits) return value;
    return injection.value;
}

const char* counter_site = nullptr;
int counter_hits = 0;
std::int64_t fixture_counter(const char* site, std::int64_t value) {
    if (!counter_site || std::strcmp(site, counter_site) != 0) return value;
    ++counter_hits;
    return std::strstr(site, "entry") ? std::numeric_limits<int>::max()
                                    : std::numeric_limits<std::int64_t>::max();
}

struct FixtureScope {
    explicit FixtureScope(bool fused) {
        injection = Injection{};
        counter_site = nullptr;
        counter_hits = 0;
        pineforge::internal::set_fused_settlement(fused);
        pineforge::internal::fixture_execution_values(fixture_value);
        pineforge::internal::fixture_execution_counters(fixture_counter);
#ifndef PINEFORGE_VALUE_INVALID_KERNEL_ONLY
        pineforge::source::fixture_adapter_execution_values(fixture_value);
#endif
    }
    ~FixtureScope() {
        pineforge::internal::fixture_execution_values(nullptr);
        pineforge::internal::fixture_execution_counters(nullptr);
        pineforge::internal::set_fused_settlement(true);
#ifndef PINEFORGE_VALUE_INVALID_KERNEL_ONLY
        pineforge::source::fixture_adapter_execution_values(nullptr);
#endif
    }
};

std::vector<pf_bar_t> bars(double price = 100.0) {
    std::vector<pf_bar_t> tape(40);
    for (std::size_t i = 0; i < tape.size(); ++i) {
        tape[i].timestamp = static_cast<std::int64_t>(i) * 300000;
        tape[i].open = price;
        tape[i].high = price;
        tape[i].low = price;
        tape[i].close = price;
        tape[i].volume = 1.0;
    }
    return tape;
}

pf_native_run_spec_v1 spec() {
    pf_native_run_spec_v1 out{};
    out.struct_size = sizeof out;
    out.session_key = "en132-g2";
    out.run_number = 1;
    out.input_tf = out.script_tf = "5";
    out.ticker = "MOCK";
    out.tickerid = "TEST:MOCK";
    out.type = "crypto";
    out.currency = "USD";
    out.basecurrency = "MOCK";
    out.description = out.volumetype = out.chart_timezone = "";
    out.timezone = "UTC";
    out.session = "24x7";
    out.initial_capital = 10000.0;
    out.point_value = out.account_fx = 1.0;
    out.price_tick = 0.01;
    out.allowed_open_directions = 3;
    return out;
}

struct Script {
    pf_strategy_t host = nullptr;
    bool current = false;
    bool two_lots = false;
    bool invalid_terms = false;
    bool invalid_excursion = false;
    bool precommit = false;
    double quantity = 1.0;
    int calculations = 0;
    int commands = 0;
    int terms_calls = 0;
    int excursions = 0;
};

int submit(Script& script, std::uint32_t intent, double units, const char* label) {
    pf_native_request_v1 request{};
    request.struct_size = sizeof request;
    request.version = PF_NATIVE_API_VERSION;
    request.intent = intent;
    request.intent_value = units;
    request.label = label;
    std::uint64_t handle = 0;
    const int result = strategy_native_submit_v1(script.host, &request, &handle, nullptr);
    CHECK(result == PF_NATIVE_OK);
    CHECK(handle != 0);
    if (result != PF_NATIVE_OK) return result;
    ++script.commands;
    if (script.current) {
        return strategy_native_execute_current_v1(
            script.host, handle, PF_NATIVE_PRICE_AS_PRESENTED, nullptr);
    }
    return result;
}

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& script = *static_cast<Script*>(user);
    ++script.calculations;
    if (script.calculations == 1 || (script.two_lots && script.calculations == 2)) {
        submit(script, PF_NATIVE_INTENT_TRANSACT, script.quantity, "entry");
    } else if (script.calculations == 4) {
        submit(script, PF_NATIVE_INTENT_FLATTEN, 0.0, "close");
    }
    return 0;
}

int terms(void* user, const pf_native_terms_view_v1*, pf_native_terms_v1* out) {
    auto& script = *static_cast<Script*>(user);
    ++script.terms_calls;
    if (!script.invalid_terms) return PF_NATIVE_ANSWER_DEFAULT;
    out->resolved_price = infinity;
    return PF_NATIVE_ANSWER_PROVIDED;
}

int excursion(void* user, const pf_native_lot_excursion_v1*, double* up, double* down) {
    auto& script = *static_cast<Script*>(user);
    ++script.excursions;
    *up = script.invalid_excursion ? infinity : 0.0;
    *down = 0.0;
    return PF_NATIVE_ANSWER_PROVIDED;
}

int precommit(void*, const pf_native_precommit_view_v1*, std::uint32_t*) {
    return PF_NATIVE_ANSWER_DEFAULT;
}

struct Outcome {
    int rc = 0;
    int status = 0;
    std::string code;
    std::string args;
    std::vector<pf_trade_t> trades;
    std::vector<pf_native_event_v1> events;
    double net_profit = 0.0;
};

void read_failure(Outcome& out, pf_strategy_t host) {
    const char* code = strategy_get_last_error_code(host);
    const char* args = strategy_get_last_error_args(host);
    CHECK(code != nullptr);
    CHECK(args != nullptr);
    out.code = code ? code : "";
    out.args = args ? args : "";
    out.status = strategy_last_run_status(host);
}

Outcome run(Script& script, const pf_native_run_spec_v1& configuration,
            const std::vector<pf_bar_t>& tape) {
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = sizeof callbacks;
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &script;
    callbacks.on_bar = on_bar;
    callbacks.on_execution_terms = terms;
    if (script.precommit) callbacks.on_precommit = precommit;
    if (script.invalid_excursion) callbacks.on_lot_excursion = excursion;
    script.host = strategy_native_host_create_v1(&callbacks);
    Outcome out;
    CHECK(script.host != nullptr);
    if (!script.host) { out.rc = PF_NATIVE_E_HANDLE; return out; }
    CHECK(strategy_configure_native_v1(script.host, &configuration) == PF_NATIVE_OK);
    pf_report_t report{};
    out.rc = strategy_native_run_v1(script.host, tape.data(), static_cast<int>(tape.size()), &report);
    read_failure(out, script.host);
    out.net_profit = report.net_profit;
    if (report.total_trades > 0) {
        CHECK(report.trades != nullptr);
        if (report.trades) out.trades.assign(report.trades, report.trades + report.total_trades);
    }
    pf_native_event_v1 events[128]{};
    const int count = strategy_native_events_v1(script.host, 0, events, 128);
    CHECK(count >= 0);
    if (count > 0) out.events.assign(events, events + count);
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(script.host);
    script.host = nullptr;
    return out;
}

void expect_g(const Outcome& out, const char* field, const char* stage, const char* reason) {
    CHECK(out.rc != PF_NATIVE_OK);
    CHECK(out.status != 0);
    CHECK(out.code == "engine_command_value_invalid");
    const std::string expected = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"" + reason + "\",\"stage\":\"" + stage + "\"}";
    CHECK(out.args == expected);
    CHECK(out.trades.empty());
    CHECK(pineforge::run_failure_code_class(pineforge::RunFailureCode::engine_command_value_invalid)
          == pineforge::RunFailureClass::engine_fault);
    CHECK(!pineforge::run_failure_code_retryable(pineforge::RunFailureCode::engine_command_value_invalid));
}

void fee_abi_overflow() {
    for (const bool fused : {false, true}) {
        if (!row(fused ? "d6_11_modeled_fused" : "d6_11_modeled_staged")) continue;
        FixtureScope scope(fused);
        Script script;
        script.quantity = 2.0;
        auto configuration = spec();
        configuration.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
        configuration.fee_value = std::numeric_limits<double>::max();
        expect_g(run(script, configuration, bars()), "fee.commission", "fee", "numeric_overflow");
        CHECK(script.commands == 1);
    }
}

void fee_injected_rows() {
    struct Case { const char* name; const char* site; bool fused; double value; const char* reason; int skip = 0; };
    const Case cases[] = {
        {"d6_11_units_fused", "fee.one_lot.units", true, infinity, "numeric_overflow"},
        {"d6_11_units_staged", "fee.staged.units", false, infinity, "numeric_overflow"},
        {"d6_11_ticket_fused", "fee.one_lot.ticket", true, infinity, "numeric_overflow"},
        {"d6_11_ticket_staged", "fee.staged.ticket", false, infinity, "numeric_overflow"},
        {"d6_11_pinned_ticket_fused", "fee.one_lot.ticket", true, infinity, "nonfinite", 1},
        {"d6_11_pinned_ticket_staged", "fee.staged.ticket", false, infinity, "nonfinite", 1},
        {"d6_11_cost_fused", "fee.one_lot.cost", true, nan, "nonfinite"},
        {"d6_11_cost_staged", "fee.staged.cost", false, nan, "nonfinite"},
        {"d6_11_sum_fused", "fee.one_lot.sum", true, infinity, "numeric_overflow"},
        {"d6_11_sum_staged", "fee.staged.sum", false, infinity, "numeric_overflow"}
    };
    for (const auto& test : cases) {
        if (!row(test.name)) continue;
        FixtureScope scope(test.fused);
        injection.site = test.site;
        injection.value = test.value;
        injection.skip_hits = test.skip;
        Script script;
        script.precommit = test.skip > 0;
        auto configuration = spec();
        configuration.fee_kind = PF_NATIVE_FEE_CASH_PER_EXECUTION;
        configuration.fee_value = 1.0;
        expect_g(run(script, configuration, bars()), "fee.commission", "fee", test.reason);
        CHECK(injection.hits > injection.skip_hits);
    }
}

void current_and_candidate_prices() {
    if (row("d6_13_current_nonfinite")) {
        FixtureScope scope(true);
        Script script;
        script.current = script.invalid_terms = true;
        expect_g(run(script, spec(), bars()), "slippage.fill_price", "slippage", "nonfinite");
        CHECK(script.terms_calls > 0);
    }
    if (row("d6_12_candidate_keep")) {
        FixtureScope scope(true);
        Script script;
        script.invalid_terms = true;
        const auto out = run(script, spec(), bars());
        CHECK(out.rc == PF_NATIVE_OK);
        CHECK(out.status == 0);
        CHECK(out.code.empty());
        CHECK(out.trades.empty());
        int rejects = 0;
        int applied = 0;
        for (const auto& event : out.events) {
            if (event.kind == PF_NATIVE_EVENT_MATCH_REJECTED) {
                ++rejects;
                CHECK(event.reason == PF_NATIVE_MATCH_REJECT_NONPOSITIVE_PRICE);
            }
            if (event.kind == PF_NATIVE_EVENT_APPLIED) ++applied;
        }
        CHECK(rejects > 0);
        CHECK(applied == 0);
    }
    if (row("d6_13_current_slippage_overflow")) {
        FixtureScope scope(true);
        Script script;
        script.current = true;
        auto configuration = spec();
        configuration.price_tick = std::numeric_limits<double>::max();
        configuration.slippage_ticks = 2;
        expect_g(run(script, configuration, bars(1.0)), "slippage.fill_price", "slippage", "nonfinite");
    }
    if (row("d6_13_current_slippage_bits")) {
        FixtureScope scope(true);
        Script script;
        script.current = true;
        auto configuration = spec();
        configuration.price_tick = 0.1;
        configuration.slippage_ticks = 1;
        const auto out = run(script, configuration, bars(100.0));
        CHECK(out.rc == PF_NATIVE_OK);
        CHECK(out.status == 0);
        CHECK(out.code.empty());
        CHECK(out.trades.size() == 1);
        if (out.trades.size() == 1) {
            // python3 -I -B .lane-out/find_inputs.py; grid None gives on_grid=100.0.
            // ADD: 100.1 (0x1.9066666666666p+6); DIVIDE: 100.10000000000001
            // (0x1.9066666666667p+6).
            const double expected_double = 0x1.9066666666666p+6;
            const double price = out.trades.front().entry_price;
            CHECK(price == expected_double);
        }
    }
}

void counter_rows() {
    struct Case { const char* name; const char* site; bool fused; bool two_lots; };
    const Case cases[] = {
        {"d6_23_cycle_project_fused", "counter.one_lot.project", true, false},
        {"d6_23_cycle_prepare_fused", "counter.one_lot.prepare", true, false},
        {"d6_23_cycle_project_staged", "counter.staged.project", false, false},
        {"d6_23_entry_staged", "counter.staged.entry", false, true}
    };
    for (const auto& test : cases) {
        if (!row(test.name)) continue;
        FixtureScope scope(test.fused);
        counter_site = test.site;
        Script script;
        script.two_lots = test.two_lots;
        script.precommit = std::strstr(test.site, "project") != nullptr;
        const auto out = run(script, spec(), bars());
        CHECK(counter_hits > 0);
        CHECK(out.rc != PF_NATIVE_OK);
        CHECK(out.status != 0);
        CHECK(out.code == "engine_invariant");
        CHECK(out.args == "{}");
        CHECK(out.trades.empty());
    }
}

// D6 rows 21 and 22 are ruled KEEP engine_invariant (ENGINE-TOP 14:09, EN-134 decides that branch):
// a non-finite settlement row keeps the typed engine_invariant failure pinned by the settlement safety rows
// (tests/test_native_settlement_row_finite.cpp), so this file carries no settlement.row row.

void finite_fee_slippage_bits() {
    if (!row("d6_11_13_finite_bits")) return;
    for (const std::uint32_t fee_kind : {0u, 1u, 2u}) {
        for (const bool current : {false, true}) {
            auto configuration = spec();
            configuration.fee_kind = fee_kind;
            configuration.fee_value = fee_kind == 0 ? 0.07 : 0.03125;
            configuration.slippage_ticks = 3;
            configuration.price_tick = 0.03;
            const double price = 97.13;
            const double quantity = 1.234567;
            const double entry = price + static_cast<double>(configuration.slippage_ticks) * configuration.price_tick;
            const double exit = price - static_cast<double>(configuration.slippage_ticks) * configuration.price_tick;
            const auto fee = [&](double fill) {
                if (fee_kind == 0) return std::abs(fill) * quantity * 1.0 * 1.0 * (configuration.fee_value / 100.0);
                if (fee_kind == PF_NATIVE_FEE_CASH_PER_EXECUTION) return configuration.fee_value;
                return configuration.fee_value * quantity;
            };
            Outcome fused;
            Outcome staged;
            for (const bool use_fused : {false, true}) {
                FixtureScope scope(use_fused);
                Script script;
                script.current = current;
                script.quantity = quantity;
                auto out = run(script, configuration, bars(price));
                CHECK(out.rc == PF_NATIVE_OK);
                CHECK(out.status == 0);
                CHECK(out.code.empty());
                CHECK(out.trades.size() == 1);
                if (out.trades.size() == 1) {
                    const auto& trade = out.trades.front();
                    CHECK(bits(trade.entry_price) == bits(entry));
                    CHECK(bits(trade.exit_price) == bits(exit));
                    CHECK(bits(trade.qty) == bits(quantity));
                    const double commission = fee(entry) + fee(exit);
                    CHECK(bits(trade.commission) == bits(commission));
                    CHECK(bits(trade.pnl) == bits((exit - entry) * quantity * 1.0 * 1.0 - commission));
                }
                if (use_fused) fused = std::move(out); else staged = std::move(out);
            }
            CHECK(fused.trades.size() == staged.trades.size());
            CHECK(bits(fused.net_profit) == bits(staged.net_profit));
            if (fused.trades.size() == 1 && staged.trades.size() == 1) {
                const auto& a = fused.trades.front();
                const auto& b = staged.trades.front();
                CHECK(bits(a.pnl_pct) == bits(b.pnl_pct));
                CHECK(bits(a.max_runup) == bits(b.max_runup));
                CHECK(bits(a.max_drawdown) == bits(b.max_drawdown));
            }
        }
    }
}

void fee_evaluation_order_bits() {
    if (!row("d6_11_fee_evaluation_bits")) return;
    for (const bool fused : {false, true}) {
        FixtureScope scope(fused);
        Script script;
        script.quantity = 0.2;
        auto configuration = spec();
        configuration.fee_value = 30.0;
        const double price = 0.1;
        const auto out = run(script, configuration, bars(price));
        CHECK(out.rc == PF_NATIVE_OK);
        CHECK(out.trades.size() == 1);
        const double old_fee = std::abs(price) * script.quantity * configuration.point_value
            * configuration.account_fx * (configuration.fee_value / 100.0);
        if (out.trades.size() == 1) {
            CHECK(bits(out.trades.front().commission) == bits(old_fee + old_fee));
            CHECK(bits(out.trades.front().pnl) == bits(0.0 - (old_fee + old_fee)));
        }
    }
}
} // namespace

#ifndef PINEFORGE_VALUE_INVALID_KERNEL_ONLY
namespace {
class SourceScript final : public pineforge::source::PineStrategyHost {
public:
    enum class Mode { Default, Core, Signal, Stop, RecalcEntry, RecalcClose, RecalcOrder, Fee, GrossFee };
    explicit SourceScript(Mode selected_mode) : mode(selected_mode) {
        pineforge::source::PineStrategyConfig configuration;
        configuration.initial_capital = 100000.0;
        configuration.default_qty_type = static_cast<int>(pineforge::QtyType::PERCENT_OF_EQUITY);
        configuration.default_qty_value = 10.0;
        configuration.margin_long = configuration.margin_short = 0.0;
        configuration.slippage = 2;
        configuration.calc_on_order_fills = mode == Mode::RecalcEntry
            || mode == Mode::RecalcClose || mode == Mode::RecalcOrder;
        if (configuration.calc_on_order_fills) configuration.pyramiding = 3;
        if (mode == Mode::Signal) {
            configuration.margin_long = configuration.margin_short = 100.0;
            configuration.default_qty_value = 100.0;
        }
        if (mode == Mode::GrossFee) {
            configuration.process_orders_on_close = true;
            configuration.commission_value = 0.05;
        }
        if (mode == Mode::Fee) {
            configuration.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
            configuration.commission_type = static_cast<int>(pineforge::CommissionType::CASH_PER_CONTRACT);
            configuration.commission_value = 0.1;
        }
        configure_pine_strategy(configuration);
        strategy_set_syminfo_metadata(this, "mintick", 0.01);
        strategy_set_syminfo_metadata(this, "pointvalue", 1.0);
        strategy_set_syminfo_metadata(this, "qty_step", 0.01);
    }
    Mode mode;
    int calls = 0;
    bool recalc_written = false;
    void on_source_bar(const pineforge::Bar& bar) override {
        ++calls;
        if (mode == Mode::Core) {
            adapter_core_sizes_default_opening(true);
            return;
        }
        if (calls == 1) {
            if (mode == Mode::Stop) strategy_entry("entry", true, nan, bar.close + 1.0);
            else if (mode == Mode::Signal) strategy_entry("entry", true, nan, nan, 1.0);
            else if (mode == Mode::Default || mode == Mode::GrossFee)
                strategy_entry("entry", true);
            else if (mode == Mode::RecalcEntry || mode == Mode::RecalcClose || mode == Mode::RecalcOrder)
                strategy_entry("entry", true, nan, bar.close + 0.5, 1.0);
            else strategy_entry("entry", true, nan, nan, 1.0);
        } else if (mode == Mode::Signal && calls == 3) {
            strategy_entry("reverse", false);
        } else if (!recalc_written && (mode == Mode::RecalcEntry
                    || mode == Mode::RecalcClose || mode == Mode::RecalcOrder)) {
            recalc_written = true;
            if (mode == Mode::RecalcClose) strategy_close("entry");
            else if (mode == Mode::RecalcOrder) strategy_order("order", true, 1.0);
            else strategy_entry("second", true, nan, nan, 1.0);
        } else if (calls == 5) strategy_close_all();
    }
};
} // namespace

// A per-strategy bridge with the public signature, as generated strategies
// provide. Runtime error getters and report freeing are the archive's own.
extern "C" void run_backtest_full(pf_strategy_t handle, pf_bar_t* tape, int n,
        const char* input_tf, const char* script_tf, int magnifier, int samples,
        pf_magnifier_distribution_t distribution, pf_report_t* report) {
    auto& host = *static_cast<SourceScript*>(handle);
    pineforge::SymInfo symbol;
    symbol.ticker = "MOCK";
    symbol.tickerid = "TEST:MOCK";
    symbol.timezone = "UTC";
    symbol.session = "24x7";
    host.run(reinterpret_cast<const pineforge::Bar*>(tape), n, input_tf, script_tf,
             {}, symbol, nullptr, magnifier != 0, samples,
             static_cast<pineforge::MagnifierDistribution>(distribution));
    host.fill_report(reinterpret_cast<pineforge::ReportC*>(report));
}

namespace {
Outcome source_run(SourceScript::Mode mode) {
    SourceScript host(mode);
    auto tape = bars();
    for (auto& bar : tape) { bar.high += 2.0; bar.low -= 2.0; }
    pf_report_t report{};
    run_backtest_full(&host, tape.data(), static_cast<int>(tape.size()), "5", "5", 0, 4,
                      PF_MAGNIFIER_ENDPOINTS, &report);
    Outcome out;
    read_failure(out, &host);
    out.rc = out.status == 0 ? PF_NATIVE_OK : PF_NATIVE_E_RUN_FAILED;
    if (report.total_trades > 0 && report.trades) {
        out.trades.assign(report.trades, report.trades + report.total_trades);
    }
    strategy_native_report_free_v1(&report);
    return out;
}

void adapter_rows() {
    struct Case { const char* name; const char* site; SourceScript::Mode mode; const char* field; const char* stage; };
    const Case cases[] = {
        {"d6_14_default_core", "slippage.default.core", SourceScript::Mode::Core, "slippage.fill_price", "slippage"},
        {"d6_14_default_signal", "slippage.default.signal", SourceScript::Mode::Signal, "slippage.fill_price", "slippage"},
        {"d6_14_default_snapshot", "slippage.default.snapshot", SourceScript::Mode::Default, "slippage.fill_price", "slippage"},
        {"d6_14_default_stop", "slippage.default.stop", SourceScript::Mode::Stop, "slippage.fill_price", "slippage"},
        {"d6_14_recalc_entry", "slippage.recalc.entry", SourceScript::Mode::RecalcEntry, "slippage.fill_price", "slippage"},
        {"d6_14_recalc_entry_tick", "slippage.recalc.entry_tick", SourceScript::Mode::RecalcEntry, "slippage.fill_price", "slippage"},
        {"d6_14_recalc_close", "slippage.recalc.close", SourceScript::Mode::RecalcClose, "slippage.fill_price", "slippage"},
        {"d6_14_recalc_order", "slippage.recalc.order", SourceScript::Mode::RecalcOrder, "slippage.fill_price", "slippage"},
        {"d6_11_adapter_opening_fee", "fee.adapter.opening", SourceScript::Mode::Fee, "fee.commission", "fee"},
        {"d6_11_adapter_grossed_unit", "fee.adapter.grossed_unit", SourceScript::Mode::GrossFee, "fee.commission", "fee"}
    };
    for (const auto& test : cases) {
        if (!row(test.name)) continue;
        FixtureScope scope(false);
        injection.site = test.site;
        injection.value = infinity;
        expect_g(source_run(test.mode), test.field, test.stage, "numeric_overflow");
        CHECK(injection.hits > 0);
    }
}

void adapter_finite_bits() {
    if (!row("d6_11_14_adapter_finite_bits")) return;
    const auto nearest = [](double value, double tick) {
        return std::floor(value / tick + 0.5) * tick;
    };
    const auto money_round = [](double value) {
        const double magnitude = std::floor(std::log10(std::abs(value)));
        const double scale = std::pow(10.0, 9.0 - magnitude);
        const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
        return value < 0.0 ? -rounded : rounded;
    };
    const double tick = 0.01;
    const double mark = nearest(100.0, tick);
    const double slipped = nearest(mark + 1.0 * 2 * tick, tick);
    struct Case { SourceScript::Mode mode; const char* site; double old_value; };
    const Case cases[] = {
        {SourceScript::Mode::Core, "slippage.default.core", slipped},
        {SourceScript::Mode::Default, "slippage.default.snapshot", slipped},
        {SourceScript::Mode::GrossFee, "fee.adapter.grossed_unit", money_round(slipped * (1.0 + 0.05 / 100.0))},
        {SourceScript::Mode::Fee, "fee.adapter.opening", 0.1 * 1.0}
    };
    for (const auto& test : cases) {
        FixtureScope scope(false);
        const auto out = source_run(test.mode);
        CHECK(out.rc == PF_NATIVE_OK);
        CHECK(out.status == 0);
        CHECK(out.code.empty());
        bool found = false;
        for (const auto& measured : injection.observed) {
            if (measured.first != test.site) continue;
            CHECK(measured.second == bits(test.old_value));
            found = true;
        }
        CHECK(found);
        if (test.mode != SourceScript::Mode::Core) CHECK(!out.trades.empty());
    }
}
} // namespace
#endif

int main(int argc, char** argv) {
    if (argc == 2) selected = argv[1];
    fee_abi_overflow();
    fee_injected_rows();
    current_and_candidate_prices();
    counter_rows();
    finite_fee_slippage_bits();
    fee_evaluation_order_bits();
#ifndef PINEFORGE_VALUE_INVALID_KERNEL_ONLY
    adapter_rows();
    adapter_finite_bits();
#endif
    CHECK(checks > 1);
    std::printf("test_value_invalid_fee_slip_settle: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
