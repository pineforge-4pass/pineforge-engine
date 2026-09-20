/* hello, kernel — from C.
 *
 * The same host as hello_kernel.cpp, written against the C API instead of
 * subclassing NativeStrategyHost: hand the runtime a callback table, describe
 * the run once, feed it bars, read the closed trades back. No PineScript, no
 * codegen, no dlopen, and no C++ in this file.
 *
 *   cc -std=c11 hello_kernel_c.c -lpineforge -lstdc++ -o hello_kernel_c
 */

#include <pineforge/pineforge.h>

#include <stdio.h>
#include <string.h>

/* open, high, low, close, volume, timestamp (Unix milliseconds). */
static const pf_bar_t kBars[] = {
    {100.0, 102.0,  99.0, 101.0, 4.0,      0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 105.0, 103.0, 104.0, 4.0, 900000}
};
enum { kBarCount = 4 };

struct host_state {
    pf_strategy_t handle;
    int           bars;
};

/* One calculation point per closed script bar. Requests submitted here are
 * matched from the next eligible point on, never on the bar just closed. */
static int on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    struct host_state* state = (struct host_state*)user;
    pf_native_request_v1 request;
    (void)bar;
    (void)at;

    ++state->bars;
    if (state->bars != 1 && state->bars != 3) return 0;

    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.version = PF_NATIVE_API_VERSION;
    if (state->bars == 1) {
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.label = "hello-long";
    } else {
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "hello-flat";
    }
    /* Returning non-zero here would end the run Failed with
     * PF_NATIVE_FAILURE_CALLBACK, so report the refusal and keep going. */
    if (strategy_native_submit_v1(state->handle, &request, NULL, NULL) != PF_NATIVE_OK) {
        fprintf(stderr, "submit refused at bar %d\n", state->bars);
    }
    return 0;
}

/* Nothing is inferred: the spec names the clock, the instrument and the
 * account. Validation refuses an incomplete value at configure time. */
static pf_native_run_spec_v1 make_spec(void) {
    pf_native_run_spec_v1 spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = (uint32_t)sizeof(spec);
    spec.session_key = "hello-kernel-c";
    spec.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = 0;                /* Percent */
    spec.fee_value = 0.0;
    spec.close_execution = 0;         /* NextEligiblePoint */
    spec.allowed_open_directions = 3; /* Both */
    return spec;
}

int main(void) {
    struct host_state state;
    pf_native_callbacks_v1 callbacks;
    pf_native_run_spec_v1 spec = make_spec();
    pf_report_t report;
    int i;

    memset(&state, 0, sizeof(state));
    memset(&report, 0, sizeof(report));

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = (uint32_t)sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &state;
    callbacks.on_bar = on_bar;

    state.handle = strategy_native_host_create_v1(&callbacks);
    if (!state.handle) {
        fprintf(stderr, "create: the runtime refused the callback table\n");
        return 1;
    }

    if (strategy_configure_native_v1(state.handle, &spec) != 0) {
        fprintf(stderr, "configure: %s\n", strategy_get_last_error(state.handle));
        strategy_native_host_free(state.handle);
        return 1;
    }

    if (strategy_native_run_v1(state.handle, kBars, kBarCount, &report) != PF_NATIVE_OK) {
        fprintf(stderr, "run: %s\n", strategy_get_last_error(state.handle));
        strategy_native_report_free_v1(&report);
        strategy_native_host_free(state.handle);
        return 1;
    }

    printf("closed trades: %d\n", report.total_trades);
    for (i = 0; i < report.total_trades; ++i) {
        const pf_trade_t* trade = &report.trades[i];
        printf("  %s qty=%g entry=%g exit=%g pnl=%g\n",
               trade->is_long ? "long " : "short", trade->qty,
               trade->entry_price, trade->exit_price, trade->pnl);
    }

    {
        const int trades = report.total_trades;
        strategy_native_report_free_v1(&report);
        strategy_native_host_free(state.handle);
        return trades > 0 ? 0 : 1;
    }
}
