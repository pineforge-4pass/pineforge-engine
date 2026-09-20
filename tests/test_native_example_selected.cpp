#include <pineforge/native_host.hpp>

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

static_assert(PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18 == 1,
              "selected example requires the v15 native host");
static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar),
              "C bar mirror must match the native host bar");
static_assert(sizeof(pf_report_t) == sizeof(pineforge::ReportC),
              "C report mirror must match the native host report");

int checks = 0;
int failures = 0;

void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s:%d: %s\n", __FILE__, line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename T>
T load(void* library, const char* name) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    if (error || !symbol) {
        std::printf("missing symbol %s: %s\n", name, error ? error : "not found");
        return nullptr;
    }
    return reinterpret_cast<T>(symbol);
}

using create_fn = pf_strategy_t (*)(const char*);
using free_fn = void (*)(pf_strategy_t);
using run_fn = void (*)(pf_strategy_t, pf_bar_t*, int, pf_report_t*);
using report_free_fn = void (*)(pf_report_t*);
using configure_fn = int (*)(pf_strategy_t, const pf_native_run_spec_v1*);
using stream_begin_fn = int (*)(pf_strategy_t, const pf_bar_t*, int, const char*, const char*);
using stream_push_fn = int (*)(pf_strategy_t, const pf_bar_t*);
using stream_end_fn = int (*)(pf_strategy_t, int);
using stream_report_fn = int (*)(pf_strategy_t, pf_report_t*);
using last_error_fn = const char* (*)(pf_strategy_t);
using epoch_fn = int (*)();
using state_fn = int (*)(pf_strategy_t);
using failure_discriminator_fn = std::uint32_t (*)(pf_strategy_t);
using count_fn = int (*)(pf_strategy_t);
using units_fn = double (*)(pf_strategy_t);

struct Abi {
    void* library = nullptr;
    create_fn create = nullptr;
    free_fn free_strategy = nullptr;
    run_fn run = nullptr;
    report_free_fn report_free = nullptr;
    configure_fn configure = nullptr;
    stream_begin_fn stream_begin = nullptr;
    stream_push_fn stream_push = nullptr;
    stream_end_fn stream_end = nullptr;
    stream_report_fn stream_report = nullptr;
    last_error_fn last_error = nullptr;
    epoch_fn host_epoch = nullptr;
    state_fn lifecycle = nullptr;
    state_fn failure_code = nullptr;
    failure_discriminator_fn failure_discriminator = nullptr;
    count_fn host_sized_applied = nullptr;
    units_fn host_sized_opened_units = nullptr;
    count_fn reversal_applied = nullptr;
    units_fn reversal_opened_units = nullptr;
    count_fn reversal_terminal_kind = nullptr;
    count_fn selected_applied = nullptr;
    units_fn selected_ticket = nullptr;
};

Abi load_library(const char* path) {
    Abi abi;
    abi.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!abi.library) {
        std::printf("dlopen failed: %s\n", dlerror());
        return abi;
    }
    abi.create = load<create_fn>(abi.library, "strategy_create");
    abi.free_strategy = load<free_fn>(abi.library, "strategy_free");
    abi.run = load<run_fn>(abi.library, "run_backtest");
    abi.report_free = load<report_free_fn>(abi.library, "report_free");
    abi.configure = load<configure_fn>(abi.library, "strategy_configure_native_v1");
    abi.stream_begin = load<stream_begin_fn>(abi.library, "strategy_stream_begin");
    abi.stream_push = load<stream_push_fn>(abi.library, "strategy_stream_push_bar");
    abi.stream_end = load<stream_end_fn>(abi.library, "strategy_stream_end");
    abi.stream_report = load<stream_report_fn>(abi.library, "strategy_stream_fill_report");
    abi.last_error = load<last_error_fn>(abi.library, "strategy_get_last_error");
    abi.host_epoch = load<epoch_fn>(abi.library, "native_selected_example_host_epoch");
    abi.lifecycle = load<state_fn>(abi.library, "native_selected_example_lifecycle");
    abi.failure_code = load<state_fn>(abi.library, "native_selected_example_failure_code");
    abi.failure_discriminator = load<failure_discriminator_fn>(
        abi.library, "native_selected_example_failure_discriminator");
    abi.host_sized_applied = load<count_fn>(
        abi.library, "native_selected_example_host_sized_applied");
    abi.host_sized_opened_units = load<units_fn>(
        abi.library, "native_selected_example_host_sized_opened_units");
    abi.reversal_applied = load<count_fn>(
        abi.library, "native_selected_example_reversal_applied");
    abi.reversal_opened_units = load<units_fn>(
        abi.library, "native_selected_example_reversal_opened_units");
    abi.reversal_terminal_kind = load<count_fn>(
        abi.library, "native_selected_example_reversal_terminal_kind");
    abi.selected_applied = load<count_fn>(
        abi.library, "native_selected_example_selected_applied");
    abi.selected_ticket = load<units_fn>(abi.library, "native_selected_example_selected_ticket");
    return abi;
}

bool ready(const Abi& abi) {
    return abi.library && abi.create && abi.free_strategy && abi.run && abi.report_free
        && abi.configure && abi.stream_begin && abi.stream_push && abi.stream_end
        && abi.stream_report && abi.last_error && abi.host_epoch && abi.lifecycle
        && abi.failure_code && abi.failure_discriminator && abi.host_sized_applied
        && abi.host_sized_opened_units && abi.reversal_applied && abi.reversal_opened_units
        && abi.reversal_terminal_kind && abi.selected_applied && abi.selected_ticket;
}

pf_native_run_spec_v1 complete_spec(const char* session_key, std::uint64_t run_number) {
    pf_native_run_spec_v1 spec{};
    spec.struct_size = sizeof(spec);
    spec.session_key = session_key;
    spec.run_number = run_number;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "SELECTED";
    spec.tickerid = "EXCHANGE:SELECTED";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native-selected-example";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = 2;  // NativeFeeKind::CashPerExecution
    spec.fee_value = 1.0;
    spec.optional_mask = 0;
    spec.close_execution = 0;         // NativeCloseExecution::NextEligiblePoint
    spec.allowed_open_directions = 3; // NativeOpenDirections::Both
    return spec;
}

void full_bars(pf_bar_t* bars) {
    const double ohlc[5][4] = {
        {100.0, 102.0, 100.0, 101.0},
        {102.0, 103.0, 101.0, 102.5},
        {103.0, 104.0, 102.0, 103.5},
        {104.0, 105.0, 103.0, 104.5},
        {105.0, 106.0, 104.0, 105.5},
    };
    for (int i = 0; i < 5; ++i) {
        bars[i].open = ohlc[i][0];
        bars[i].high = ohlc[i][1];
        bars[i].low = ohlc[i][2];
        bars[i].close = ohlc[i][3];
        bars[i].volume = 1.0;
        bars[i].timestamp = static_cast<std::int64_t>(i) * 300000;
    }
}

void reversal_only_bars(pf_bar_t* bars) {
    const double ohlc[5][4] = {
        {110.0, 111.0, 109.0, 110.5},
        {111.0, 112.0, 110.0, 111.5},
        {112.0, 113.0, 111.0, 112.5},
        {113.0, 114.0, 112.0, 113.5},
        {114.0, 115.0, 113.0, 114.5},
    };
    for (int i = 0; i < 5; ++i) {
        bars[i].open = ohlc[i][0];
        bars[i].high = ohlc[i][1];
        bars[i].low = ohlc[i][2];
        bars[i].close = ohlc[i][3];
        bars[i].volume = 1.0;
        bars[i].timestamp = static_cast<std::int64_t>(i) * 300000;
    }
}

struct Snapshot {
    int lifecycle = -1;
    int failure_code = -1;
    std::uint32_t failure_discriminator = 0;
    int host_sized_applied = -1;
    double host_sized_opened_units = 0.0;
    int reversal_applied = -1;
    double reversal_opened_units = 0.0;
    int reversal_terminal_kind = -1;
    int selected_applied = -1;
    double selected_ticket = 0.0;
    int first_stream_failure = 0;
    std::string error;
};

Snapshot snapshot(const Abi& abi, pf_strategy_t strategy) {
    Snapshot out;
    out.lifecycle = abi.lifecycle(strategy);
    out.failure_code = abi.failure_code(strategy);
    out.failure_discriminator = abi.failure_discriminator(strategy);
    out.host_sized_applied = abi.host_sized_applied(strategy);
    out.host_sized_opened_units = abi.host_sized_opened_units(strategy);
    out.reversal_applied = abi.reversal_applied(strategy);
    out.reversal_opened_units = abi.reversal_opened_units(strategy);
    out.reversal_terminal_kind = abi.reversal_terminal_kind(strategy);
    out.selected_applied = abi.selected_applied(strategy);
    out.selected_ticket = abi.selected_ticket(strategy);
    const char* error = abi.last_error(strategy);
    if (error) out.error = error;
    return out;
}

Snapshot run_batch(const Abi& abi, const char* session_key, std::uint64_t run_number,
                   pf_bar_t* bars, int count) {
    Snapshot out;
    const pf_strategy_t strategy = abi.create(nullptr);
    if (!strategy) return out;
    const auto spec = complete_spec(session_key, run_number);
    if (abi.configure(strategy, &spec) == 0) {
        pf_report_t report{};
        abi.run(strategy, bars, count, &report);
        out = snapshot(abi, strategy);
        abi.report_free(&report);
    } else {
        out = snapshot(abi, strategy);
    }
    abi.free_strategy(strategy);
    return out;
}

Snapshot run_stream(const Abi& abi, const char* session_key, std::uint64_t run_number,
                    pf_bar_t* bars, int count) {
    Snapshot out;
    const pf_strategy_t strategy = abi.create(nullptr);
    if (!strategy || count < 1) return out;
    const auto spec = complete_spec(session_key, run_number);
    if (abi.configure(strategy, &spec) == 0) {
        int first_stream_failure = 0;
        int result = abi.stream_begin(strategy, bars, 1, "5", "5");
        if (result == 0) {
            for (int i = 1; i < count; ++i) {
                result = abi.stream_push(strategy, &bars[i]);
                if (result != 0 && first_stream_failure == 0) first_stream_failure = i;
            }
            result = abi.stream_end(strategy, 0);
            if (result != 0 && first_stream_failure == 0) first_stream_failure = count;
        } else {
            first_stream_failure = -1;
        }
        out = snapshot(abi, strategy);
        out.first_stream_failure = first_stream_failure;
        pf_report_t report{};
        if (abi.stream_report(strategy, &report) == 0) abi.report_free(&report);
    } else {
        out = snapshot(abi, strategy);
    }
    abi.free_strategy(strategy);
    return out;
}

bool same(double actual, double expected) {
    return std::abs(actual - expected) <= 1e-12;
}

void integration_row(const char* name, bool passed, const Snapshot& result) {
    ++checks;
    if (passed) {
        std::printf("INTEGRATION %s: PASS\n", name);
        return;
    }
    ++failures;
    std::printf(
        "INTEGRATION %s: expected-red lifecycle=%d failure_code=%d discriminator=%u "
        "host_sized=%d host_units=%.17g reversal=%d reversal_units=%.17g reversal_terminal=%d "
        "selected=%d selected_ticket=%.17g stream_failure=%d error=%s\n",
        name, result.lifecycle, result.failure_code, result.failure_discriminator,
        result.host_sized_applied, result.host_sized_opened_units, result.reversal_applied,
        result.reversal_opened_units, result.reversal_terminal_kind, result.selected_applied, result.selected_ticket,
        result.first_stream_failure, result.error.empty() ? "(empty)" : result.error.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/native-selected-example\n", argv[0]);
        return 2;
    }

    Abi abi = load_library(argv[1]);
    CHECK(ready(abi));
    if (!ready(abi)) {
        if (abi.library) dlclose(abi.library);
        return 1;
    }
    CHECK(abi.host_epoch() == 15);
    std::printf("STRUCTURAL native-selected-example loaded with host epoch %d\n",
                abi.host_epoch());

    pf_bar_t full[5]{};
    full_bars(full);
    const Snapshot batch = run_batch(abi, "native-selected-batch", 1, full, 5);
    integration_row("batch -> Completed",
                    batch.lifecycle == static_cast<int>(pineforge::NativeLifecycleKind::Completed),
                    batch);

    const Snapshot stream = run_stream(abi, "native-selected-stream", 1, full, 5);
    integration_row("forward -> Completed",
                    stream.lifecycle == static_cast<int>(pineforge::NativeLifecycleKind::Completed),
                    stream);

    pf_bar_t reversal_only[5]{};
    reversal_only_bars(reversal_only);
    const Snapshot reversal = run_batch(abi, "native-selected-reversal", 1, reversal_only, 5);
    integration_row("one exact reversal Applied",
                    reversal.reversal_applied == 1
                        && same(reversal.reversal_opened_units, -1.0),
                    reversal);

    integration_row("one host-sized open Applied",
                    batch.host_sized_applied == 1
                        && same(batch.host_sized_opened_units, 2.0),
                    batch);
    integration_row("one selected-close ticket",
                    batch.selected_applied == 1 && batch.selected_ticket > 0.0,
                    batch);

    dlclose(abi.library);
    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
