#include "run_results_release_fixture.hpp"

#if defined(__linux__)

#include <pineforge/native_c_api.h>

namespace {

using namespace run_results_test;
using Report = OwnedReport<pf_report_t, &strategy_native_report_free_v1>;

struct Error {
    std::string code;
    std::string message;

    bool is(std::string_view expected_code, std::string_view expected_message) const {
        return code == expected_code && message == expected_message;
    }
};

Error last_error(pf_strategy_t host) {
    const char* code = strategy_get_last_error_code(host);
    const char* message = strategy_get_last_error(host);
    return {code ? code : "", message ? message : ""};
}

class CallbackHost {
public:
    CallbackHost() {
        pf_native_callbacks_v1 callbacks{};
        callbacks.struct_size = sizeof(callbacks);
        callbacks.version = PF_NATIVE_API_VERSION;
        callbacks.user = this;
        callbacks.on_run_begin = [](void* user) {
            static_cast<CallbackHost*>(user)->bars_ = 0;
            return 0;
        };
        callbacks.on_bar = [](void* user, const pf_bar_t*, const pf_native_decision_v1*) {
            return static_cast<CallbackHost*>(user)->on_bar();
        };
        handle_ = strategy_native_host_create_v1(&callbacks);
        require(handle_ != nullptr, "fixture: strategy_native_host_create_v1 refused");

        const pf_cache_policy_entry_v1 ledger{PF_CACHE_KIND_LEDGER_CACHE, 0, 0};
        const pf_cache_policy_v1 cache{sizeof(cache), 1, 1, &ledger, {0, 0}};
        if (strategy_set_cache_policy_v1(handle_, &cache) != 0) {
            strategy_native_host_free(handle_);
            throw std::runtime_error("fixture: strategy_set_cache_policy_v1 refused");
        }
    }

    CallbackHost(const CallbackHost&) = delete;
    CallbackHost& operator=(const CallbackHost&) = delete;
    ~CallbackHost() { strategy_native_host_free(handle_); }

    pf_strategy_t handle() const { return handle_; }

    void require_ok(int status, const char* call) const {
        if (status != 0) throw std::runtime_error(std::string("fixture: ") + call + ": "
                                                + strategy_get_last_error(handle_));
    }

    void begin(int run) {
        run_ = run;
        pf_native_run_spec_v1 spec{};
        spec.struct_size = sizeof(spec);
        spec.session_key = "results-release-c";
        spec.run_number = static_cast<std::uint64_t>(run);
        spec.input_tf = spec.script_tf = "5";
        spec.ticker = "MOCK";
        spec.tickerid = "TEST:MOCK";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.description = spec.volumetype = spec.chart_timezone = "";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = kCapital;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = kTick;
        spec.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
        spec.close_execution = PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT;
        spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;

        pf_native_run_spec_ext_v1 ext{};
        ext.struct_size = sizeof(ext);
        ext.version = PF_NATIVE_API_VERSION;
        ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT | PF_NATIVE_SPEC_EXT_EVENT_RETENTION;
        ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
        ext.event_retention = PF_NATIVE_EVENT_RETENTION_WINDOW;
        std::uint32_t validation = 0;
        // The typed entry point admits the same handle's next run as well.
        require_ok(strategy_configure_native_ext_result_v1(handle_, &spec, &ext,
                                                            &validation, nullptr),
                   "strategy_configure_native_ext_result_v1");
        const auto warmup = bar<pf_bar_t>(0);
        require_ok(strategy_stream_begin(handle_, &warmup, 1, "5", "5"), "strategy_stream_begin");
    }

    void feed(int first, int last) const {
        for (int i = first; i <= last; ++i) {
            const auto input = bar<pf_bar_t>(i);
            require_ok(strategy_stream_push_bar(handle_, &input), "strategy_stream_push_bar");
        }
    }

    pf_native_state_v1 state() const {
        pf_native_state_v1 result{};
        result.struct_size = sizeof(result);
        result.version = PF_NATIVE_API_VERSION;
        require_ok(strategy_native_state_v1(handle_, &result), "strategy_native_state_v1");
        return result;
    }

    void complete() const {
        require_ok(strategy_stream_end(handle_, 0), "strategy_stream_end");
        require(state().lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED, "fixture: C stream did not complete");
    }

    void read_rows(int n) const {
        for (int i = 0; i < n; ++i) {
            const auto* entry = strategy_closed_trade_entry_id(handle_, i);
            require(entry && order_label("entry", run_, i) == entry,
                    "fixture: C entry ID differs from booked trade");
            const auto* exit = strategy_closed_trade_exit_id(handle_, i);
            require(exit && order_label("exit", run_, i) == exit,
                    "fixture: C exit ID differs from booked trade");
            const auto* comment = strategy_closed_trade_exit_comment(handle_, i);
            require(comment && std::string_view(comment) == "close",
                    "fixture: C exit comment differs from booked trade");
        }
    }

    void report(Report& report, int n) const {
        require_ok(strategy_stream_fill_report(handle_, &report.value), "strategy_stream_fill_report");
        check_full_report(report.value, n);
        read_rows(n);
    }

private:
    int on_bar() {
        const int i = bars_++;
        if (i % 4 != 1 && i % 4 != 3) return 0;
        const bool open = i % 4 == 1;
        const auto label = order_label(open ? "entry" : "exit", run_, i / 4);
        const auto comment = open ? entry_comment(run_, i / 4) : std::string("close");
        pf_native_request_v1 request{};
        request.struct_size = sizeof(request);
        request.version = PF_NATIVE_API_VERSION;
        request.intent = open ? PF_NATIVE_INTENT_TRANSACT : PF_NATIVE_INTENT_FLATTEN;
        request.intent_value = open ? 1.0 : 0.0;
        request.trigger = PF_NATIVE_TRIGGER_MARKET;
        request.label = label.c_str();
        request.comment = comment.c_str();
        return strategy_native_submit_v1(handle_, &request, nullptr, nullptr);
    }

    pf_strategy_t handle_ = nullptr;
    int run_ = 0;
    int bars_ = 0;
};

ReleaseResult stream_case(Measurements& out) {
    constexpr char scenario[] = "stream";
    ReleaseResult result{};
    {
        CallbackHost host;
        const auto handle = host.handle();
        host.begin(1);
        host.feed(1, 4 * kRoundTrips);
        const auto a = out.capture("A", scenario);
        require_ledger(a);

        host.complete();
        const auto ended = out.capture("B", scenario, "end");
        require_ledger(ended);
        Report before;
        host.report(before, kRoundTrips);
        const auto b = out.capture("B", scenario);
        require_ledger(b);
        require(keeps_files(a, ended, kCommandScratch) && keeps_files(ended, b) && keeps_files(b, ended),
                "stage B: completion or report read closed or replaced a results file");

        const auto release = strategy_release_run_results_v1(handle);
        const auto release_error = last_error(handle);
        const auto r = out.capture("R", scenario);
        out.line("R", scenario) << " status=" << release << " code=" << release_error.code
            << " message=" << std::quoted(release_error.message) << '\n';

        bool row_refused = true;
        for (const int i : {0, kRoundTrips - 1}) {
            const bool null_row = strategy_closed_trade_entry_id(handle, i) == nullptr;
            const auto row_error = last_error(handle);
            out.line("R", scenario) << " read=entry_id row=" << i << " null=" << null_row
                << " code=" << row_error.code << " message=" << std::quoted(row_error.message) << '\n';
            row_refused = null_row && row_error.is("engine_invariant", kReleasedMessage) && row_refused;
        }
        Report after;
        // A separate handle supplies a clean last-error record for the report
        // check below. This handle has just recorded the string read's error.
        const auto report = strategy_stream_fill_report(handle, &after.value);
        const auto report_error = last_error(handle);
        out.line("R", scenario) << " read=fill_report status=" << report << " code="
            << report_error.code << " message=" << std::quoted(report_error.message) << '\n';
        result.released = release == 0 && r.empty() && row_refused && report != 0
            && release_error.code.empty() && release_error.message.empty()
            && report_error.is("engine_invariant", kReleasedMessage)
            && !after.value.trades && after.value.trades_len == 0;
        result.stub = release != 0 && release_error.is("setting_rejected", kStubMessage);

        const auto again = strategy_release_run_results_v1(handle);
        const auto repeat_error = last_error(handle);
        const auto r2 = out.capture("R2", scenario);
        const bool stable = same_files(r, r2);
        out.line("R2", scenario) << " status=" << again << " unchanged=" << stable
            << " message=" << std::quoted(repeat_error.message) << '\n';
        result.repeated = again == 0 && stable && r2.empty()
            && strategy_closed_trade_entry_id(handle, 0) == nullptr
            && last_error(handle).is("engine_invariant", kReleasedMessage);

        host.begin(2);
        check_next_begin(b, out.capture("C", scenario, "begin"));
        host.feed(1, 4);
        const auto next_files = out.capture("C", scenario, "first_close");
        require_ledger(next_files);
        require(retired_files(b, next_files), "stage C: first close reused the previous run's files");
        host.complete();
        Report next;
        host.report(next, 1);
    }
    require(out.capture("D", scenario).empty(), "stage D: C host left scratch files open");
    return result;
}

bool report_refusal(Measurements& out) {
    constexpr char scenario[] = "report_read";
    bool refused = false;
    {
        CallbackHost host;
        const auto handle = host.handle();
        host.begin(1);
        host.feed(1, 4);
        host.complete();
        Report before;
        host.report(before, 1);
        require_ledger(out.capture("B", scenario));

        const auto release = strategy_release_run_results_v1(handle);
        const auto files = out.capture("R", scenario);
        const auto before_read = last_error(handle);
        Report after;
        const auto status = strategy_stream_fill_report(handle, &after.value);
        const auto error = last_error(handle);
        out.line("R", scenario) << " release_status=" << release << " report_status=" << status
            << " code=" << error.code << " message=" << std::quoted(error.message) << '\n';
        refused = release == 0 && files.empty() && before_read.code.empty() && before_read.message.empty()
            && status != 0 && error.is("engine_invariant", kReleasedMessage)
            && !after.value.trades && after.value.trades_len == 0;
    }
    require(out.capture("D", scenario).empty(), "stage D: report host left scratch files open");
    return refused;
}

void active_refusal(Measurements& out, bool stub) {
    constexpr char scenario[] = "active_stream";
    Report control;
    {
        CallbackHost host;
        host.begin(1);
        host.feed(1, 4 * kRoundTrips);
        host.complete();
        host.report(control, kRoundTrips);
    }
    {
        CallbackHost host;
        const auto handle = host.handle();
        host.begin(1);
        host.feed(1, 4);
        const auto before = out.capture("REFUSAL", scenario, "before");
        require_ledger(before);
        const auto state = host.state();
        const auto status = strategy_release_run_results_v1(handle);
        const auto error = last_error(handle);
        const auto after = out.capture("REFUSAL", scenario, "after");
        out.line("REFUSAL", scenario) << " status=" << status << " code=" << error.code
            << " message=" << std::quoted(error.message) << '\n';
        require(status != 0 && error.is("setting_rejected", stub ? kStubMessage : kBusyMessage),
                "stage REFUSAL: C release did not report a coded busy refusal");
        const auto current = host.state();
        require(same_files(before, after) && current.lifecycle == PF_NATIVE_LIFECYCLE_RUNNING
                    && current.phase == state.phase && current.failure_code == state.failure_code
                    && current.decision_floor_ms == state.decision_floor_ms
                    && current.consumed_high_water == state.consumed_high_water
                    && strategy_last_run_status(handle) == 0,
                "stage REFUSAL: C release changed the stream");
        host.read_rows(1);
        host.feed(5, 4 * kRoundTrips);
        host.complete();
        Report actual;
        host.report(actual, kRoundTrips);
        require(same_report(control.value, actual.value), "stage REFUSAL: C refusal changed the report");
    }
    require(out.capture("D", scenario).empty(), "stage D: C refusal host left scratch files open");
}

}  // namespace

int main() {
    return run_results_test::measured_main("c_abi", [](auto& out) -> std::string_view {
        require(strategy_release_run_results_v1(nullptr) == -1,
                "fixture: C release did not reject a null handle");
        const auto stream = stream_case(out);
        active_refusal(out, stream.stub);
        const auto report = report_refusal(out);
        if (!stream.released || !report) return "stage R: completed results were not released correctly";
        if (!stream.repeated) return "stage R2: repeated release was not a no-op";
        return {};
    });
}

#else
int main() {
    std::cout << "RELEASE summary=SKIP arm=c_abi reason=\"Linux /proc/self/fd is required\"\n";
    return 77;
}
#endif
