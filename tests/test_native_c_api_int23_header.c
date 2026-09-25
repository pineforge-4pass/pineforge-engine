/* C-SURFACE-1: compile this entire caller against db98990c's frozen public
 * header closure, then link it to the current kernel. Its policy-hook table
 * was 168 bytes and already entitled to the decision's session tail. */
#include <pineforge/native_c_api.h>

#include <stdio.h>
#include <string.h>

struct caller_state { int saw_tail; int saw_callback; };

_Static_assert(sizeof(pf_native_callbacks_v1) == 168,
               "the INT23 callback table fixture changed");

static int on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    struct caller_state* state = (struct caller_state*)user;
    (void)bar;
    ++state->saw_callback;
    if (at->struct_size == sizeof(pf_native_decision_v1)
        && at->has_script_interval == 1u) state->saw_tail = 1;
    return 0;
}

int main(void) {
    struct caller_state state = {0, 0};
    pf_native_callbacks_v1 callbacks;
    pf_native_run_spec_v1 spec;
    pf_bar_t bars[3] = {
        {100, 102, 99, 101, 1, 0},
        {101, 103, 100, 102, 1, 300000},
        {102, 104, 101, 103, 1, 600000}
    };
    pf_report_t report;
    pf_strategy_t host;
    int rc;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = (uint32_t)sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &state;
    callbacks.on_bar = on_bar;
    printf("CALLBACK TABLE %zu\n", sizeof(callbacks));
    host = strategy_native_host_create_v1(&callbacks);
    if (!host) { puts("HOST REFUSED"); return 2; }
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = (uint32_t)sizeof(spec);
    spec.session_key = "oldcaller";
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
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.allowed_open_directions = 3;
    if (strategy_configure_native_v1(host, &spec) != 0) {
        puts("CONFIGURE REFUSED"); strategy_native_host_free(host); return 2;
    }
    memset(&report, 0, sizeof(report));
    rc = strategy_native_run_v1(host, bars, 3, &report);
    if (rc != PF_NATIVE_OK || state.saw_callback == 0) {
        printf("RUN REFUSED %d CALLBACKS %d\n", rc, state.saw_callback);
        strategy_native_host_free(host); return 2;
    }
    puts(state.saw_tail ? "TAIL PRESENTED" : "TAIL WITHHELD");
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(host);
    return state.saw_tail ? 0 : 1;
}
