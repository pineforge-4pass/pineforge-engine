// EN-132 D6 rows 4/5/6/9/10. Written before the producers; expectations
// are pinned in .lane-out/MAP.md. This TU is source-free / kernel-profile safe.
// Real C callback hosts run through strategy_native_run_v1. The few tuples
// command admission cannot construct are injected inside their callbacks;
// they still execute the real sizing / preview / execution implementation.
#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
int checks = 0;
int failures = 0;
const char* row = "setup";
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d %s\n", row, __LINE__, #condition); } } while (0)
constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double largest = std::numeric_limits<double>::max();

struct State {
    pf_strategy_t host = nullptr;
    int bars = 0;
    int close_calls = 0;
    int margin_calls = 0;
    int size_calls = 0;
    double close_units = 1.0;
    std::optional<NativeMarginDecision> margin_answer;
    std::optional<double> margin_units;
    std::function<void(State&, int)> script;
    NativeStrategyHost& native() const {
        return *static_cast<NativeStrategyHost*>(static_cast<BacktestEngine*>(host));
    }
};

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& state = *static_cast<State*>(user);
    const int index = state.bars++;
    if (state.script) state.script(state, index);
    return 0;
}
int close_units(void* user, const pf_native_close_view_v1*, double* units) {
    auto& state = *static_cast<State*>(user);
    ++state.close_calls;
    *units = state.close_units;
    return PF_NATIVE_ANSWER_PROVIDED;
}
int margin_requirement(void* user, const pf_native_margin_view_v1*,
                       pf_native_margin_decision_v1* out) {
    auto& state = *static_cast<State*>(user);
    ++state.margin_calls;
    if (!state.margin_answer) return PF_NATIVE_ANSWER_DEFAULT;
    out->required = state.margin_answer->required;
    out->equity = state.margin_answer->equity;
    out->force_breach = state.margin_answer->force_breach;
    return PF_NATIVE_ANSWER_PROVIDED;
}
int margin_size(void* user, const pf_native_margin_view_v1*, double* out) {
    auto& state = *static_cast<State*>(user);
    ++state.size_calls;
    if (!state.margin_units) return PF_NATIVE_ANSWER_DEFAULT;
    *out = *state.margin_units;
    return PF_NATIVE_ANSWER_PROVIDED;
}

pf_native_run_spec_v1 spec() {
    pf_native_run_spec_v1 out{};
    out.struct_size = sizeof(out);
    out.session_key = "en132-sizing-margin";
    out.run_number = 1;
    out.input_tf = "1";
    out.script_tf = "1";
    out.ticker = "MOCK";
    out.tickerid = "TEST:MOCK";
    out.type = "crypto";
    out.currency = "USD";
    out.basecurrency = "MOCK";
    out.description = "";
    out.volumetype = "";
    out.timezone = "UTC";
    out.session = "24x7";
    out.chart_timezone = "";
    out.initial_capital = 1000.0;
    out.point_value = 1.0;
    out.account_fx = 1.0;
    out.price_tick = 0.01;
    out.fee_kind = 1; // CashPerExecution: no huge notional * zero fee.
    out.fee_value = 0.0;
    out.allowed_open_directions = 3;
    return out;
}
pf_native_run_spec_ext_v1 extension() {
    pf_native_run_spec_ext_v1 out{};
    out.struct_size = sizeof(out);
    out.version = PF_NATIVE_API_VERSION;
    out.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    out.report_open_position_at_end = 0;
    // Keep the events read by the controls after the run finishes.
    out.present_mask = PF_NATIVE_SPEC_EXT_EVENT_RETENTION;
    out.event_retention = PF_NATIVE_EVENT_RETENTION_FULL;
    return out;
}
pf_native_run_spec_ext_v1 margin_model(double initial, double maintenance,
                                      double multiple = 1.0) {
    auto out = extension();
    out.present_mask |= PF_NATIVE_SPEC_EXT_MARGIN;
    out.margin_initial_long = initial;
    out.margin_initial_short = initial;
    out.margin_has_maintenance_long = 1;
    out.margin_has_maintenance_short = 1;
    out.margin_maintenance_long = maintenance;
    out.margin_maintenance_short = maintenance;
    out.margin_shortfall_multiple = multiple;
    out.margin_sizing = PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM;
    out.margin_check = PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK;
    return out;
}
pf_native_request_v1 request(std::uint32_t intent, double value = 0.0) {
    pf_native_request_v1 out{};
    out.struct_size = sizeof(out);
    out.version = PF_NATIVE_API_VERSION;
    out.intent = intent;
    out.intent_value = value;
    out.label = "en132-request";
    return out;
}
std::uint64_t submit(State& state, const pf_native_request_v1& command) {
    std::uint64_t incarnation = 0;
    const int status = strategy_native_submit_v1(state.host, &command, &incarnation, nullptr);
    CHECK(status == PF_NATIVE_OK);
    CHECK(incarnation != 0);
    return incarnation;
}
void execute(State& state, std::uint64_t incarnation) {
    // A failed execution keeps its typed run failure; no subsequent command
    // is issued by a fault case that could overwrite it with a C refusal.
    std::uint32_t refusal = 0;
    (void)strategy_native_execute_current_v1(state.host, incarnation,
                                            PF_NATIVE_PRICE_AS_PRESENTED, &refusal);
}
no::RequestHandle handle(State& state, std::uint64_t incarnation) {
    const auto& core = NativeExecutionConsumer::bound(state.native()).request_core();
    return {core.identity(), incarnation};
}
std::uint64_t host_close(State& state) {
    std::uint64_t cohort = 0;
    CHECK(strategy_native_cohort_open_v1(state.host, &cohort) == PF_NATIVE_OK);
    const auto opened = submit(state, request(PF_NATIVE_INTENT_TRANSACT, 1.0));
    CHECK(strategy_native_cohort_add_v1(state.host, cohort, opened) == PF_NATIVE_OK);
    execute(state, opened);
    auto close = request(PF_NATIVE_INTENT_HOST_SIZED);
    close.owner = PF_NATIVE_OWNER_BIND_COHORT;
    close.cohort = cohort;
    return submit(state, close);
}
void fixture_pending_total(State& state, std::uint64_t incarnation, double total) {
    // Test-local injection into existing state through existing read seams.
    // Commands cannot store this: pending_sum checks it first. The underlying
    // live request is mutable (owned by the consumer), not a const object.
    const auto& core = NativeExecutionConsumer::bound(state.native()).request_core();
    auto* live = const_cast<no::LiveRequest*>(core.find_live(handle(state, incarnation)));
    CHECK(live != nullptr);
    if (live) live->pending = no::PendingDeferred{total, 1, {}};
}

struct Result {
    int rc = 0;
    int status = 0;
    std::string code;
    std::string args;
    std::vector<pf_trade_t> trades;
    std::vector<pf_native_event_v1> events;
    double signed_units = 0.0;
};
Result run(State& state, pf_native_run_spec_v1 setup = spec(),
           pf_native_run_spec_ext_v1 ext = extension(), double price = 100.0) {
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &state;
    callbacks.on_bar = on_bar;
    callbacks.on_close_units = close_units;
    callbacks.on_margin_requirement = margin_requirement;
    callbacks.on_margin_call_units = margin_size;
    state.host = strategy_native_host_create_v1(&callbacks);
    CHECK(state.host != nullptr);
    if (!state.host) return {};
    CHECK(strategy_configure_native_ext_v1(state.host, &setup, &ext) == PF_NATIVE_OK);
    pf_bar_t bars[4]{};
    for (int i = 0; i < 4; ++i) {
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = price;
        bars[i].volume = 1.0;
        bars[i].timestamp = 1736121600000LL + i * 60000LL;
    }
    pf_report_t report{};
    Result out;
    out.rc = strategy_native_run_v1(state.host, bars, 4, &report);
    out.status = strategy_last_run_status(state.host);
    const char* code = strategy_get_last_error_code(state.host);
    const char* args = strategy_get_last_error_args(state.host);
    CHECK(code != nullptr && args != nullptr);
    out.code = code ? code : "<null>";
    out.args = args ? args : "<null>";
    if (report.total_trades > 0 && report.trades)
        out.trades.assign(report.trades, report.trades + report.total_trades);
    CHECK(report.total_trades == static_cast<int>(out.trades.size()));
    strategy_native_report_free_v1(&report);
    // Read the C events while completed. A failed host's refusal APIs could
    // clear the presented error, so its code/args were copied first.
    pf_native_event_v1 events[64]{};
    const int n = strategy_native_events_v1(state.host, 0, events, 64);
    CHECK(n >= 0);
    if (n > 0) out.events.assign(events, events + n);
    double average_price = 0.0;
    std::uint64_t lots = 0;
    CHECK(strategy_native_position_v1(state.host, &out.signed_units, &average_price, &lots)
          == PF_NATIVE_OK);
    strategy_native_host_free(state.host);
    state.host = nullptr;
    return out;
}
void failed(const Result& result, const char* field, const char* stage, const char* reason) {
    CHECK(result.rc == PF_NATIVE_E_RUN_FAILED);
    CHECK(result.status == 1);
    CHECK(result.code == "engine_command_value_invalid");
    const std::string expected = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"" + reason + "\",\"stage\":\"" + stage + "\"}";
    CHECK(result.args == expected);
    CHECK(result.trades.empty());
}
void completed(const Result& result) {
    CHECK(result.rc == PF_NATIVE_OK);
    CHECK(result.status == 0);
    CHECK(result.code.empty());
    CHECK(result.args.empty());
}

void row4_sizing_input_and_output() {
    struct Case { const char* name; double cash, price, equity, fx; bool percent; const char* field; const char* reason; };
    const Case cases[] = {
        {"row4_cash_nonfinite", inf, 100, 1000, 1, false, "sizing.quantity", "nonfinite"},
        {"row4_equity_nonfinite", 1, 100, inf, 1, true, "sizing.basis", "nonfinite"},
        {"row4_price_nonfinite", 100, inf, 1000, 1, false, "sizing.quantity", "nonfinite"},
        {"row4_fx_nonfinite", 100, 100, 1000, inf, false, "sizing.quantity", "nonfinite"},
        {"row4_denominator_overflow", 100, largest, 1000, 2, false, "sizing.quantity", "nonfinite"},
        {"row4_quotient_overflow", largest, 0.5, 1000, 1, false, "sizing.quantity", "numeric_overflow"},
    };
    for (const auto& test : cases) {
        row = test.name;
        State state;
        state.script = [test](State& s, int index) {
            if (index) return;
            no::Sized sized;
            sized.basis = test.percent ? no::SizeBasis{no::EquityFraction{test.cash}}
                                      : no::SizeBasis{no::CashValue{test.cash}};
            // Inject a derived tuple at the host query, inside the real run.
            (void)s.native().native_sized_units(sized, test.price, test.equity, test.fx);
        };
        failed(run(state), test.field, "sizing", test.reason);
        CHECK(state.bars == 1);
    }
    for (const auto time : {PF_NATIVE_SIZE_AT_MATCH, PF_NATIVE_SIZE_AT_ACCEPTANCE}) {
        row = time == PF_NATIVE_SIZE_AT_MATCH ? "row4_match_door" : "row4_acceptance_door";
        State state;
        state.script = [time](State& s, int index) {
            if (index) return;
            if (time == PF_NATIVE_SIZE_AT_ACCEPTANCE) {
                auto sized = request(PF_NATIVE_INTENT_SIZED, 100.0);
                sized.size_time = time;
                std::uint64_t incarnation = 0;
                // submit_with_surface records the typed failure before the
                // C wrapper converts its exception to a command status.
                CHECK(strategy_native_submit_v1(s.host, &sized, &incarnation, nullptr)
                      != PF_NATIVE_OK);
                CHECK(incarnation == 0);
            } else {
                auto sized = request(PF_NATIVE_INTENT_SIZED, 100.0);
                (void)submit(s, sized);
            }
        };
        auto setup = spec();
        setup.point_value = largest;
        failed(run(state, setup), "sizing.quantity", "sizing", "nonfinite");
    }
}

void row5_host_sized_nonfinite() {
    for (const double units : {inf, -inf, nan}) {
        for (const bool preview : {false, true}) {
            row = preview ? "row5_host_sized_preview" : "row5_host_sized_execution";
            State state;
            state.close_units = units;
            state.script = [preview](State& s, int index) {
                if (index) return;
                const auto close = host_close(s);
                if (preview) (void)s.native().inspect_current_execution({handle(s, close)});
                else execute(s, close);
            };
            failed(run(state), "sizing.quantity", "sizing", "nonfinite");
            CHECK(state.close_calls == 1);
        }
    }
}

void row6_deduction_overflow() {
    for (const bool preview : {false, true}) {
        row = preview ? "row6_pending_preview" : "row6_pending_execution";
        State state;
        state.script = [preview](State& s, int index) {
            if (index) return;
            const auto close = host_close(s);
            fixture_pending_total(s, close, inf);
            if (preview) (void)s.native().inspect_current_execution({handle(s, close)});
            else execute(s, close);
        };
        failed(run(state), "sizing.quantity", "sizing", "numeric_overflow");
    }
}

void row9_opening_requirement_overflow() {
    row = "row9_initial_requirement_overflow";
    State state;
    state.script = [](State& s, int index) {
        if (!index) (void)submit(s, request(PF_NATIVE_INTENT_TRANSACT, 1.0e308));
    };
    auto setup = spec();
    setup.initial_capital = 1.0e308;
    failed(run(state, setup, margin_model(4.0, 0.5), 1.0),
           "margin.required", "margin", "nonfinite");
}

void open_on_first_bar(State& state, double units) {
    state.script = [units](State& s, int index) {
        if (!index) (void)submit(s, request(PF_NATIVE_INTENT_TRANSACT, units));
    };
}
void row10_maintenance_nonfinite() {
    row = "row10_unit_maintenance_overflow";
    State unit;
    open_on_first_bar(unit, 1.0);
    failed(run(unit, spec(), margin_model(0.0, largest), 2.0),
           "margin.maintenance", "margin", "nonfinite");
    row = "row10_held_maintenance_overflow";
    State held;
    open_on_first_bar(held, 2.0);
    failed(run(held, spec(), margin_model(0.0, largest), 1.0),
           "margin.maintenance", "margin", "nonfinite");
    for (const bool invalid_equity : {false, true}) {
        row = invalid_equity ? "row10_host_equity_nonfinite" : "row10_host_required_nonfinite";
        State host;
        open_on_first_bar(host, 1.0);
        host.margin_answer = NativeMarginDecision{invalid_equity ? 10.0 : inf,
                                                  invalid_equity ? nan : 1000.0, false};
        failed(run(host, spec(), margin_model(0.0, 0.1)),
               "margin.maintenance", "margin", "nonfinite");
        CHECK(host.margin_calls > 0);
    }
}
void row10_restore_overflow() {
    row = "row10_restore_multiple_overflow";
    State multiple;
    open_on_first_bar(multiple, 20.0);
    auto setup = spec();
    setup.initial_capital = 500.0;
    auto model = margin_model(0.0, 0.5, largest);
    model.margin_sizing = PF_NATIVE_LIQUIDATION_SIZING_SHORTFALL_MULTIPLE;
    failed(run(multiple, setup, model), "margin.restore", "margin", "numeric_overflow");
    for (const auto sizing : {PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM, PF_NATIVE_LIQUIDATION_SIZING_FLATTEN}) {
        row = sizing == PF_NATIVE_LIQUIDATION_SIZING_FLATTEN ? "row10_restore_flatten_overflow"
                                                     : "row10_restore_difference_overflow";
        State difference;
        open_on_first_bar(difference, 1.0);
        difference.margin_answer = NativeMarginDecision{largest, -largest, true};
        auto policy = margin_model(0.0, 0.1);
        policy.margin_sizing = sizing;
        failed(run(difference, spec(), policy), "margin.restore", "margin", "numeric_overflow");
    }
    row = "row10_host_restore_nonfinite";
    State host;
    open_on_first_bar(host, 1.0);
    host.margin_answer = NativeMarginDecision{100.0, 0.0, true};
    host.margin_units = inf;
    failed(run(host, spec(), margin_model(0.0, 0.1)),
           "margin.restore", "margin", "nonfinite");
    CHECK(host.size_calls > 0);
}

void keep_zero_sizing() {
    for (const double equity : {0.0, -1.0}) {
        row = "row4_keep_nonpositive_equity";
        State state;
        state.script = [equity](State& s, int index) {
            if (index) return;
            no::Sized sized;
            sized.basis = no::EquityFraction{1.0};
            CHECK(!s.native().native_sized_units(sized, 100.0, equity, 1.0));
        };
        const auto result = run(state);
        completed(result);
        CHECK(result.trades.empty());
        CHECK(result.signed_units == 0.0);
    }
    row = "row4_keep_zero_fraction";
    State zero;
    zero.script = [](State& s, int index) {
        if (index) return;
        no::Sized sized;
        sized.basis = no::EquityFraction{0.0};
        CHECK(!s.native().native_sized_units(sized, 100.0, 1000.0, 1.0));
    };
    const auto result = run(zero);
    completed(result);
    CHECK(result.trades.empty());
}
void keep_host_sized_finite_refusal() {
    for (const double units : {-1.0, 0.0}) {
        for (const bool preview : {false, true}) {
            row = units < 0 ? "row5_keep_negative_host_units" : "row5_keep_zero_host_units";
            State state;
            state.close_units = units;
            state.script = [preview, units](State& s, int index) {
                if (index) return;
                const auto close = host_close(s);
                if (preview) {
                    const auto view = s.native().inspect_current_execution({handle(s, close)});
                    if (units < 0) CHECK(view.terms_rejection == no::MatchRejectReason::InvalidTerms);
                    else CHECK(view.settlement_readiness == execution::Status::NoEffect);
                }
                execute(s, close);
            };
            const auto result = run(state);
            completed(result);
            CHECK(result.trades.empty());
            CHECK(result.signed_units == 1.0);
            bool witnessed = false;
            for (const auto& event : result.events)
                if (event.kind == (units < 0 ? PF_NATIVE_EVENT_MATCH_REJECTED : PF_NATIVE_EVENT_NO_EFFECT))
                    witnessed = true;
            CHECK(witnessed);
        }
    }
}
void keep_margin_finite_paths() {
    row = "row9_keep_finite_shortfall";
    State shortfall;
    open_on_first_bar(shortfall, 20.0);
    const auto refused = run(shortfall, spec(), margin_model(1.0, 0.5));
    completed(refused);
    CHECK(refused.trades.empty());
    CHECK(refused.signed_units == 0.0);
    row = "row9_keep_zero_initial_fraction";
    State waived;
    open_on_first_bar(waived, 1.0);
    const auto admitted = run(waived, spec(), margin_model(0.0, 0.1));
    completed(admitted);
    CHECK(admitted.signed_units == 1.0);
    row = "row10_keep_forced_nonpositive_restore";
    State forced;
    open_on_first_bar(forced, 1.0);
    forced.margin_answer = NativeMarginDecision{0.0, 1000.0, true};
    forced.margin_units = 0.0;
    const auto no_call = run(forced, spec(), margin_model(0.0, 0.1));
    completed(no_call);
    CHECK(no_call.signed_units == 1.0);
    CHECK(no_call.trades.empty());
    CHECK(forced.size_calls > 0);
}
bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof(double)) == 0;
}
void finite_boundary_is_bit_identical() {
    row = "finite_boundary_sized_vs_explicit";
    const double cash = std::nextafter(300.0, 0.0);
    const double quantity = cash / (100.0 * 1.0 * 1.0);
    State sized;
    sized.script = [cash](State& s, int index) {
        if (!index) {
            auto command = request(PF_NATIVE_INTENT_SIZED, cash);
            command.grid_policy = PF_NATIVE_GRID_EXPLICIT_UNITS;
            (void)submit(s, command);
        } else if (index == 2) (void)submit(s, request(PF_NATIVE_INTENT_FLATTEN));
    };
    State explicit_units;
    explicit_units.script = [quantity](State& s, int index) {
        if (!index) (void)submit(s, request(PF_NATIVE_INTENT_TRANSACT, quantity));
        else if (index == 2) (void)submit(s, request(PF_NATIVE_INTENT_FLATTEN));
    };
    const auto actual = run(sized, spec(), margin_model(1.0, 0.5));
    const auto reference = run(explicit_units, spec(), margin_model(1.0, 0.5));
    completed(actual);
    completed(reference);
    CHECK(actual.trades.size() == 1 && reference.trades.size() == 1);
    if (actual.trades.size() != 1 || reference.trades.size() != 1) return;
    const auto& a = actual.trades[0];
    const auto& b = reference.trades[0];
    CHECK(same_bits(a.qty, quantity));
    CHECK(same_bits(a.qty, b.qty));
    CHECK(same_bits(a.entry_price, b.entry_price));
    CHECK(same_bits(a.exit_price, b.exit_price));
    CHECK(same_bits(a.pnl, b.pnl));
    CHECK(same_bits(a.commission, b.commission));
    CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
}
} // namespace

int main() {
    row4_sizing_input_and_output();
    row5_host_sized_nonfinite();
    row6_deduction_overflow();
    row9_opening_requirement_overflow();
    row10_maintenance_nonfinite();
    row10_restore_overflow();
    keep_zero_sizing();
    keep_host_sized_finite_refusal();
    keep_margin_finite_paths();
    finite_boundary_is_bit_identical();
    std::printf("EN132 sizing/margin native: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
