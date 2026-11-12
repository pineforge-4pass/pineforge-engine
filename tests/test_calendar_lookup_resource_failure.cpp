#include "../src/native_calendar_memo.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_c_api.h>
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "fail_alloc_seam.hpp"

namespace pineforge {
inline namespace engine_script_run_v20 {

struct NativeExecutionConsumerProbe {
    static bool in_session(const BacktestEngine& host, std::int64_t ms) {
        return NativeExecutionConsumer::bound(host).session_point_resolved(ms).in_session;
    }
    static std::optional<std::int64_t> risk_day(const BacktestEngine& host, std::int64_t ms) {
        return NativeExecutionConsumer::bound(host).risk_day(ms);
    }
};

}  // inline namespace engine_script_run_v20
}  // namespace pineforge

namespace {

using namespace pineforge;
namespace nc = pineforge::native_calendar;
namespace no = pineforge::native_order;

int failures = 0;
const char* row = "setup";
std::size_t allocation = 0;

struct Outcome;
struct CallbackObservations;
struct CheckDiagnostics;
CheckDiagnostics* current_diagnostics = nullptr;
void report_check_failure(int line, const char* condition);

struct CheckDiagnostics {
    explicit CheckDiagnostics(pf_strategy_t value = nullptr)
        : previous(current_diagnostics), handle(value) { current_diagnostics = this; }
    ~CheckDiagnostics() { current_diagnostics = previous; }

    CheckDiagnostics* previous;
    pf_strategy_t handle;
    const Outcome* outcome = nullptr;
    const CallbackObservations* callbacks = nullptr;
    std::size_t closed_trades = 0;
    const char* call = "run";
    int result = 0;
    bool has_result = false;
    bool has_handle_result = false;
    std::optional<std::uint32_t> lifecycle;
};

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        report_check_failure(__LINE__, #condition);                            \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t kHour = 3'600'000;
constexpr std::int64_t kDay = 24 * kHour;
constexpr int kBars = 4;

enum class Scenario { Session, RiskSessionDay, RiskCalendarDay, CCallbacks };

bool has_risk(Scenario scenario) {
    return scenario == Scenario::RiskSessionDay || scenario == Scenario::RiskCalendarDay;
}

std::array<Bar, kBars> tape(Scenario scenario) {
    // Recife's daylight-saving changes were only seven days apart. Unlike
    // ordinary New York DST, these cycles cannot use the certified fast path.
    // The loss-limit tape crosses midnight in Paris: the new day's opening
    // equity is 9990, so the limit fires at 80. Losing that day key can
    // instead flatten at 90 against the previous day's opening equity.
    const std::int64_t first = has_risk(scenario)
        ? nc::native_civil_days(2024, 3, 9) * kDay + 22 * kHour
        : nc::native_civil_days(2000, 10, 8) * kDay + 2 * kHour;
    std::array<Bar, kBars> bars{};
    for (int i = 0; i < kBars; ++i) {
        const double price = has_risk(scenario)
            ? (i == 0 ? 100.0 : i == 1 ? 90.0 : 80.0) : 100.0 + i;
        bars[static_cast<std::size_t>(i)] =
            {price, price, price, price, 1.0, first + i * kHour};
    }
    return bars;
}

NativeRunSpec spec(Scenario scenario) {
    NativeRunSpec value;
    value.identity = {"calendar-resource-failure", 1};
    value.input_tf = value.script_tf = "60";
    value.tickerid = "TEST:CALENDAR";
    value.timezone = has_risk(scenario) ? "Europe/Paris" : "America/Recife";
    value.session = "0000-0000";
    value.initial_capital = 10'000.0;
    value.point_value = value.account_fx = 1.0;
    value.price_tick = 0.01;
    value.close_execution = NativeCloseExecution::AfterCalculation;
    value.report_policy = NativeReportPolicy::KernelRecorded;
    value.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    if (scenario == Scenario::CCallbacks) {
        // Equal timeframes with raw provider labels carry no calendar interval.
        // The C decision's day flags require a real calendar interval.
        value.slot_label_policy = NativeSlotLabelPolicy::Canonical;
    }
    if (has_risk(scenario)) {
        NativeRiskLimits limits;
        limits.max_intraday_loss = NativeLossLimit{5.0, false};
        limits.day_basis = scenario == Scenario::RiskSessionDay
            ? NativeRiskDay::SessionDay : NativeRiskDay::CalendarDayInTimezone;
        limits.action = NativeRiskAction::FlattenAndBlock;
        value.risk = limits;
    }
    return value;
}

class Host final : public NativeStrategyHost {
public:
    explicit Host(Scenario scenario) : scenario_(scenario) {
        declare_native_bar_open_hook(false);
    }

    int callback_count() const noexcept { return calculations_; }

private:
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        const int index = calculations_++;
        no::Request request;
        if (has_risk(scenario_)) {
            if (index == 0) request.intent = no::Transact{1.0};
            else if (index == kBars - 1) request.intent = no::Flatten{};
            else return;
        } else if (index % 2 == 0) {
            if (!context.in_session) return;
            request.intent = no::Transact{1.0};
        } else {
            request.intent = no::Flatten{};
        }
        request.label = index % 2 == 0 ? "entry" : "exit";
        (void)submit(request);
    }

    Scenario scenario_;
    int calculations_ = 0;
};

struct CallbackObservations {
    int callbacks = 0;
    int in_session = 0;
    int has_script_interval = 0;
    int has_session_day = 0;
    int has_next_input_session_day = 0;
    int submissions = 0;
    bool command_refused = false;
    std::optional<int> last_submit_result;
};

struct Outcome {
    std::size_t attempts = 0;
    bool fired = false;
    bool escaped = false;
    int status = -1;
    int c_result = PF_NATIVE_OK;
    int callback_count = 0;
    bool command_refused = false;
    std::size_t first_callback_allocations = 0;
    std::optional<CallbackObservations> callback_observations;
    std::string code;
    std::string text;
    bool published = false;
    std::vector<pf_trade_t> trades;
    std::string bytes;
};

void print_callback_observations(std::FILE* stream, const CallbackObservations& observed,
                                 std::size_t closed_trades) {
    std::fprintf(stream, " closed_trades=%zu callbacks=%d in_session=%d"
                 " has_script_interval=%d has_session_day=%d"
                 " has_next_input_session_day=%d submissions=%d command_refused=%d",
                 closed_trades, observed.callbacks, observed.in_session,
                 observed.has_script_interval, observed.has_session_day,
                 observed.has_next_input_session_day, observed.submissions,
                 observed.command_refused ? 1 : 0);
    if (observed.last_submit_result) {
        std::fprintf(stream, " last_submit_result=%d", *observed.last_submit_result);
    } else {
        std::fprintf(stream, " last_submit_result=not_called");
    }
}

void report_check_failure(int line, const char* condition) {
    const auto* diagnostics = current_diagnostics;
    const auto* outcome = diagnostics ? diagnostics->outcome : nullptr;
    const auto handle = diagnostics ? diagnostics->handle : nullptr;
    const char* code = outcome ? outcome->code.c_str() : strategy_get_last_error_code(handle);
    const char* text = outcome ? outcome->text.c_str() : strategy_get_last_error(handle);
    const int status = outcome ? outcome->status : strategy_last_run_status(handle);
    std::fprintf(stderr, "FAIL [%s allocation %zu] line %d: %s; call=%s",
                 row, allocation, line, condition, diagnostics ? diagnostics->call : "none");
    if (outcome) {
        std::fprintf(stderr, " c_result=%d escaped=%d callback_count=%d"
                     " first_callback_allocation=%zu",
                     outcome->c_result, outcome->escaped ? 1 : 0, outcome->callback_count,
                     outcome->first_callback_allocations);
    } else if (diagnostics && diagnostics->has_handle_result) {
        std::fprintf(stderr, " return_handle=%p", handle);
    } else if (diagnostics && diagnostics->has_result) {
        std::fprintf(stderr, " return=%d", diagnostics->result);
    }
    std::fprintf(stderr, " status=%d code=\"%s\" text=\"%s\"",
                 status, code ? code : "<null>", text ? text : "<null>");
    if (diagnostics && diagnostics->lifecycle)
        std::fprintf(stderr, " lifecycle=%u", static_cast<unsigned>(*diagnostics->lifecycle));
    const auto* callbacks = outcome && outcome->callback_observations
        ? &*outcome->callback_observations : diagnostics ? diagnostics->callbacks : nullptr;
    if (callbacks) {
        print_callback_observations(stderr, *callbacks,
            outcome ? outcome->trades.size() : diagnostics->closed_trades);
    }
    std::fputc('\n', stderr);
}

template<class T>
void append(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void collect(pf_strategy_t handle, Outcome& outcome) {
    CheckDiagnostics diagnostics(handle);
    if (outcome.callback_observations)
        diagnostics.callbacks = &*outcome.callback_observations;
    diagnostics.call = "strategy_last_run_status/strategy_get_last_error_code/strategy_get_last_error";
    // The harness checks these three accessors before publishing a report.
    outcome.status = strategy_last_run_status(handle);
    const char* code = strategy_get_last_error_code(handle);
    const char* text = strategy_get_last_error(handle);
    CHECK(code != nullptr && text != nullptr);
    outcome.code = code ? code : "";
    outcome.text = text ? text : "";
    if (outcome.status == 1 || !outcome.code.empty() || !outcome.text.empty()) return;

    struct Report {
        pf_report_t value{};
        ~Report() { strategy_native_report_free_v1(&value); }
    } report;
    diagnostics.call = "strategy_stream_fill_report";
    diagnostics.result = strategy_stream_fill_report(handle, &report.value);
    diagnostics.has_result = true;
    CHECK(diagnostics.result == 0);
    CHECK(report.value.trades_len >= 0);
    CHECK(report.value.trades_len == 0 || report.value.trades != nullptr);
    if (report.value.trades_len < 0
        || (report.value.trades_len > 0 && !report.value.trades)) return;
    outcome.published = true;
    append(outcome.bytes, report.value.total_trades);
    append(outcome.bytes, report.value.net_profit);
    append(outcome.bytes, report.value.input_bars_processed);
    append(outcome.bytes, report.value.script_bars_processed);
    append(outcome.bytes, report.value.trades_len);
    for (int i = 0; i < report.value.trades_len; ++i) {
        const auto& trade = report.value.trades[i];
        outcome.trades.push_back(trade);
        // Serialize fields, excluding padding and report-owned pointers.
        append(outcome.bytes, trade.entry_time); append(outcome.bytes, trade.exit_time);
        append(outcome.bytes, trade.entry_price); append(outcome.bytes, trade.exit_price);
        append(outcome.bytes, trade.pnl); append(outcome.bytes, trade.pnl_pct);
        append(outcome.bytes, trade.is_long);
        append(outcome.bytes, trade.max_runup); append(outcome.bytes, trade.max_drawdown);
        append(outcome.bytes, trade.qty); append(outcome.bytes, trade.commission);
        append(outcome.bytes, trade.entry_bar_index); append(outcome.bytes, trade.exit_bar_index);
        append(outcome.bytes, trade.open_at_end);
    }
}

Outcome run_native(Scenario scenario, std::size_t nth) {
    Host host(scenario);
    const auto configuration = spec(scenario);
    const auto bars = tape(scenario);
    CHECK(host.configure_native(configuration).status == NativeSetupStatus::Applied);
    Outcome outcome;
    fail_alloc::arm(nth);
    try {
        host.run(bars.data(), kBars);
    } catch (...) {
        outcome.escaped = true;
    }
    outcome.attempts = fail_alloc::attempts();
    outcome.fired = fail_alloc::disarm();
    outcome.callback_count = host.callback_count();
    collect(static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&host)), outcome);
    return outcome;
}

struct CCallbackState {
    pf_strategy_t handle = nullptr;
    int calculations = 0;
    CallbackObservations observed;
    std::size_t first_callback_allocations = 0;
    ~CCallbackState() { strategy_native_host_free(handle); }
};

int on_c_bar(void* user, const pf_bar_t*, const pf_native_decision_v1* decision) {
    auto& state = *static_cast<CCallbackState*>(user);
    if (state.calculations == 0)
        state.first_callback_allocations = fail_alloc::attempts();
    const int index = state.calculations++;
    state.observed.callbacks = state.calculations;
    state.observed.in_session += decision->in_session ? 1 : 0;
    state.observed.has_script_interval += decision->has_script_interval ? 1 : 0;
    state.observed.has_session_day += decision->has_session_day ? 1 : 0;
    state.observed.has_next_input_session_day += decision->has_next_input_session_day ? 1 : 0;
    if (index % 2 == 0
        && !(decision->in_session && decision->has_session_day
             && decision->has_next_input_session_day)) return 0;
    pf_native_request_v1 request{};
    request.struct_size = static_cast<std::uint32_t>(sizeof(request));
    request.version = PF_NATIVE_API_VERSION;
    request.intent = index % 2 == 0 ? PF_NATIVE_INTENT_TRANSACT : PF_NATIVE_INTENT_FLATTEN;
    request.intent_value = index % 2 == 0 ? 1.0 : 0.0;
    request.trigger = PF_NATIVE_TRIGGER_MARKET;
    request.label = index % 2 == 0 ? "entry" : "exit";
    ++state.observed.submissions;
    const int result = strategy_native_submit_v1(state.handle, &request, nullptr, nullptr);
    state.observed.last_submit_result = result;
    if (result == PF_NATIVE_OK) return 0;
    state.observed.command_refused = true;
    return 1;
}

Outcome run_c(std::size_t nth) {
    CCallbackState state;
    CheckDiagnostics diagnostics;
    diagnostics.callbacks = &state.observed;
    Outcome outcome;
    outcome.callback_observations = state.observed;
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof(callbacks));
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &state;
    callbacks.on_bar = &on_c_bar;
    diagnostics.call = "strategy_native_host_create_v1";
    state.handle = strategy_native_host_create_v1(&callbacks);
    diagnostics.handle = state.handle;
    diagnostics.has_handle_result = true;
    CHECK(state.handle != nullptr);
    if (!state.handle) return outcome;
    const auto native_spec = spec(Scenario::CCallbacks);
    pf_native_run_spec_v1 configuration{};
    configuration.struct_size = static_cast<std::uint32_t>(sizeof(configuration));
    configuration.session_key = native_spec.identity.session_key.c_str();
    configuration.run_number = native_spec.identity.run_number;
    configuration.input_tf = native_spec.input_tf.c_str();
    configuration.script_tf = native_spec.script_tf.c_str();
    configuration.ticker = native_spec.ticker.c_str();
    configuration.tickerid = native_spec.tickerid.c_str();
    configuration.type = native_spec.type.c_str();
    configuration.currency = native_spec.currency.c_str();
    configuration.basecurrency = native_spec.basecurrency.c_str();
    configuration.description = native_spec.description.c_str();
    configuration.volumetype = native_spec.volumetype.c_str();
    configuration.chart_timezone = native_spec.chart_timezone.c_str();
    configuration.timezone = native_spec.timezone.c_str();
    configuration.session = native_spec.session.c_str();
    configuration.initial_capital = native_spec.initial_capital;
    configuration.point_value = native_spec.point_value;
    configuration.account_fx = native_spec.account_fx;
    configuration.price_tick = native_spec.price_tick;
    configuration.fee_kind = PF_NATIVE_FEE_PERCENT;
    configuration.fee_value = native_spec.fee_value;
    configuration.close_execution = PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION;
    configuration.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    pf_native_run_spec_ext_v1 extension{};
    extension.struct_size = static_cast<std::uint32_t>(sizeof(extension));
    extension.version = PF_NATIVE_API_VERSION;
    extension.present_mask = PF_NATIVE_SPEC_EXT_REPORT | PF_NATIVE_SPEC_EXT_FEED_POLICY
        | PF_NATIVE_SPEC_EXT_EVENT_RETENTION;
    extension.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    extension.slot_label_policy = PF_NATIVE_SLOT_LABEL_CANONICAL;
    extension.feed_tolerance = 0;
    extension.path_order = PF_NATIVE_PATH_ORDER_AUTO;
    extension.abort_reporting = PF_NATIVE_ABORT_ERROR;
    // The C base layout retains Full unless this tail selects the native
    // fixture's default Window explicitly.
    extension.event_retention = PF_NATIVE_EVENT_RETENTION_WINDOW;
    diagnostics.call = "strategy_configure_native_ext_v1";
    diagnostics.result = strategy_configure_native_ext_v1(
        state.handle, &configuration, &extension);
    diagnostics.has_result = true;
    diagnostics.has_handle_result = false;
    CHECK(diagnostics.result == PF_NATIVE_OK);
    const auto native_bars = tape(Scenario::CCallbacks);
    std::array<pf_bar_t, kBars> bars{};
    static_assert(sizeof(bars) == sizeof(native_bars), "bar layouts must match");
    std::memcpy(bars.data(), native_bars.data(), sizeof(bars));
    diagnostics.call = "strategy_native_run_v1";
    fail_alloc::arm(nth);
    try {
        outcome.c_result = strategy_native_run_v1(state.handle, bars.data(), kBars, nullptr);
    } catch (...) {
        outcome.escaped = true;
    }
    outcome.attempts = fail_alloc::attempts();
    outcome.fired = fail_alloc::disarm();
    outcome.callback_count = state.observed.callbacks;
    outcome.command_refused = state.observed.command_refused;
    outcome.first_callback_allocations = state.first_callback_allocations;
    outcome.callback_observations = state.observed;
    diagnostics.result = outcome.c_result;
    collect(state.handle, outcome);
    diagnostics.closed_trades = outcome.trades.size();
    pf_native_state_v1 lifecycle{};
    lifecycle.struct_size = static_cast<std::uint32_t>(sizeof(lifecycle));
    diagnostics.call = "strategy_native_state_v1";
    diagnostics.result = strategy_native_state_v1(state.handle, &lifecycle);
    diagnostics.lifecycle = lifecycle.lifecycle;
    CHECK(diagnostics.result == PF_NATIVE_OK);
    CHECK(lifecycle.lifecycle == (outcome.status == 1
        ? PF_NATIVE_LIFECYCLE_FAILED : PF_NATIVE_LIFECYCLE_COMPLETED));
    return outcome;
}

void check_callback_clean(const Outcome& outcome) {
    CheckDiagnostics diagnostics;
    diagnostics.outcome = &outcome;
    CHECK(outcome.callback_observations.has_value());
    if (!outcome.callback_observations) return;
    const auto& observed = *outcome.callback_observations;
    CHECK(observed.callbacks == kBars);
    CHECK(observed.in_session == kBars);
    CHECK(observed.has_script_interval == kBars);
    CHECK(observed.has_session_day == kBars);
    CHECK(observed.has_next_input_session_day == kBars);
    CHECK(observed.submissions == kBars);
    CHECK(!observed.command_refused);
    CHECK(observed.last_submit_result == PF_NATIVE_OK);
}

static void print_allocation_ranges(const std::vector<std::size_t>& indexes) {
    for (std::size_t i = 0; i < indexes.size();) {
        const std::size_t first = i++;
        while (i < indexes.size() && indexes[i] - indexes[i - 1] == 1) ++i;
        std::printf("%s%zu", first == 0 ? "" : ",", indexes[first]);
        if (i - first > 1) std::printf("-%zu", indexes[i - 1]);
    }
}

template<class Run>
Outcome differential_sweep(const char* name, Run run, bool risk, bool callbacks = false) {
    CheckDiagnostics diagnostics;
    row = name;
    allocation = 0;
    const Outcome warm = run(0);
    diagnostics.outcome = &warm;
    CHECK(!warm.escaped && warm.status == 0 && warm.published);
    if (callbacks) check_callback_clean(warm);
    const Outcome clean = run(0);
    diagnostics.outcome = &clean;
    CHECK(!clean.escaped && !clean.fired && clean.attempts > 0);
    CHECK(clean.status == 0 && clean.code.empty() && clean.text.empty());
    CHECK(clean.c_result == PF_NATIVE_OK && clean.published);
    if (callbacks) check_callback_clean(clean);
    CHECK(clean.bytes == warm.bytes);
    CHECK(clean.trades.size() == (risk ? 1u : 2u));
    if (clean.trades.size() == (risk ? 1u : 2u)) {
        CHECK(clean.trades[0].entry_price == 100.0);
        CHECK(clean.trades[0].exit_price == (risk ? 80.0 : 101.0));
        CHECK(clean.trades[0].pnl == (risk ? -20.0 : 1.0));
        if (!risk) {
            CHECK(clean.trades[1].entry_price == 102.0);
            CHECK(clean.trades[1].exit_price == 103.0);
        }
    }
    std::size_t oom = 0;
    std::vector<std::size_t> completed;
    for (std::size_t nth = 1; nth <= clean.attempts; ++nth) {
        allocation = nth;
        diagnostics.outcome = nullptr;
        const Outcome actual = run(nth);
        diagnostics.outcome = &actual;
        CHECK(actual.fired);
        CHECK(!actual.escaped);
        if (actual.status == 1 || !actual.code.empty() || !actual.text.empty()) {
            CHECK(actual.status == 1 && !actual.code.empty() && !actual.text.empty());
            CHECK(!actual.published && actual.bytes.empty());
            CHECK(actual.c_result == PF_NATIVE_OK || actual.c_result == PF_NATIVE_E_RUN_FAILED);
            // A failed zone-resource read is an existing typed setup refusal
            // only before the first host callback.
            // Allocation failures after callbacks begin must be out_of_memory.
            CHECK(actual.code == "out_of_memory"
                  || (actual.code == "symbol_metadata_rejected" && actual.callback_count == 0));
            if (actual.code == "out_of_memory") ++oom;
        } else {
            completed.push_back(nth);
            CHECK(actual.c_result == PF_NATIVE_OK && actual.published);
            CHECK(actual.bytes == clean.bytes);
        }
    }
    diagnostics.outcome = &clean;
    CHECK(oom > 0);
    allocation = 0;
    const Outcome repeated = run(0);
    diagnostics.outcome = &repeated;
    CHECK(!repeated.escaped && repeated.status == 0 && repeated.code.empty()
          && repeated.text.empty() && repeated.published);
    CHECK(repeated.bytes == clean.bytes);
    if (callbacks) check_callback_clean(repeated);
    std::printf("%s: swept all %zu run allocations, %zu out_of_memory failures\n",
                name, clean.attempts, oom);
    if (clean.first_callback_allocations != 0) {
        std::printf("%s: first clean callback follows run allocation %zu\n",
                    name, clean.first_callback_allocations);
    }
    if (clean.callback_observations) {
        std::printf("%s:", name);
        print_callback_observations(stdout, *clean.callback_observations, clean.trades.size());
        std::fputc('\n', stdout);
    }
    std::printf("%s: %zu of %zu injected runs completed, at allocation indexes: ",
                name, completed.size(), clean.attempts);
    print_allocation_ranges(completed);
    std::fputc('\n', stdout);
    return clean;
}

void calendar_controls() {
    CheckDiagnostics diagnostics;
    row = "calendar_controls";
    allocation = 0;
    const auto calendar = nc::parse_session("0000-0000", "America/Recife");
    CHECK(calendar.has_value());
    if (!calendar) return;
    nc::SessionDayMemo memo;
    const auto bars = tape(Scenario::Session);
    for (const auto& bar : bars) {
        CHECK(!nc::cycle_certificate(*calendar, bar.timestamp, memo));
        CHECK(nc::in_session(*calendar, bar.timestamp));
    }
    const auto before = nc::session_day_at(*calendar, bars[0].timestamp);
    const auto after = nc::session_day_at(*calendar, bars[1].timestamp);
    CHECK(before && after && before->ordinal + 1 == after->ordinal);
    CHECK(after && after->next_origin_ms - after->origin_ms == 23 * kHour);

    Host host(Scenario::RiskCalendarDay);
    diagnostics.handle = static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&host));
    auto configuration = spec(Scenario::RiskCalendarDay);
    configuration.timezone = "UTC";
    CHECK(host.configure_native(configuration).status == NativeSetupStatus::Applied);
    const auto risk_bars = tape(Scenario::RiskCalendarDay);
    host.run(risk_bars.data(), kBars);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(NativeExecutionConsumerProbe::risk_day(host, risk_bars[0].timestamp).has_value());
    CHECK(NativeExecutionConsumerProbe::in_session(host, risk_bars[0].timestamp));
    const auto utc = nc::parse_session("0000-0000", "UTC");
    CHECK(utc.has_value());
    if (!utc) return;
    const auto unkeyable = std::numeric_limits<std::int64_t>::max();
    CHECK(!nc::session_day_ordinal(*utc, unkeyable));
    CHECK(!NativeExecutionConsumerProbe::risk_day(host, unkeyable));
    CHECK(!NativeExecutionConsumerProbe::in_session(host, unkeyable));
    CHECK(host.last_run_status() == 0 && host.last_error().empty());
    CHECK(std::strcmp(strategy_get_last_error_code(
        static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&host))), "") == 0);
}

}  // namespace

int main() {
    calendar_controls();
    (void)differential_sweep("session_entries",
        [](std::size_t nth) { return run_native(Scenario::Session, nth); }, false);
    (void)differential_sweep("risk_session_day",
        [](std::size_t nth) { return run_native(Scenario::RiskSessionDay, nth); }, true);
    (void)differential_sweep("risk_calendar_day",
        [](std::size_t nth) { return run_native(Scenario::RiskCalendarDay, nth); }, true);
    const Outcome callback = differential_sweep("callback_session_days", run_c, false, true);
    row = "clean_c_and_cpp_trades";
    // Compare the C fixture with the same canonical spec in a C++ host.
    // Raw provider labels have different execution times at bar close.
    const Outcome native = run_native(Scenario::CCallbacks, 0);
    CheckDiagnostics diagnostics;
    if (callback.callback_observations)
        diagnostics.callbacks = &*callback.callback_observations;
    diagnostics.outcome = &native;
    CHECK(!native.escaped && native.status == 0 && native.code.empty()
          && native.text.empty() && native.published);
    CHECK(native.trades.size() == 2u);
    diagnostics.outcome = &callback;
    CHECK(callback.bytes == native.bytes);
    return failures == 0 ? 0 : 1;
}
