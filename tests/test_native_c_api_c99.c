/*
 * test_native_c_api_c99.c -- the public C headers as a strict C99 consumer
 * sees them (R5 lane F4, item 9).
 *
 * tests/CMakeLists.txt compiles this translation unit as C99 with extensions
 * off and -pedantic-errors, so any C11-only construct in the public headers
 * fails the build. The one they held, the anonymous union behind P2c's
 * sharpe_tv / sortino_tv aliases, left with those spellings at 1.0 (lane
 * REL10). Its body checks the equity offsets those spellings shared, and a
 * native C host configures through the typed enumerations, runs, and reads
 * its words back by their C names.
 *
 * No assert(): the Release gate defines NDEBUG and would no-op every row.
 */

#include <pineforge/pineforge.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

/* open, high, low, close, volume, timestamp (Unix milliseconds). */
static const pf_bar_t kBars[] = {
    {100.0, 102.0,  99.0, 101.0, 4.0,      0},
    {101.0, 103.0, 100.0, 102.0, 4.0, 300000},
    {102.0, 104.0, 101.0, 103.0, 4.0, 600000},
    {103.0, 105.0, 102.0, 104.0, 4.0, 900000}
};

struct c99_host {
    pf_strategy_t handle;
    int           calculations;
    int           unnamed;
};

static int c99_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    struct c99_host* host = (struct c99_host*)user;
    pf_native_request_v1 request;
    (void)bar;
    if (at->provenance != PF_NATIVE_PROVENANCE_CALCULATION
        || at->path_phase != PF_NATIVE_PATH_PHASE_NONE
        || at->completion != PF_NATIVE_COMPLETION_KIND_CONFIRMED) {
        ++host->unnamed;
    }
    if (++host->calculations != 1) return 0;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.version = PF_NATIVE_API_VERSION;
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_MARKET;
    return strategy_native_submit_v1(host->handle, &request, NULL, NULL) == PF_NATIVE_OK ? 0 : 1;
}

static void check_equity_layout(void) {
    pf_equity_stats_t stats;
    CHECK(offsetof(pf_equity_stats_t, sharpe_monthly) == 48, "sharpe_monthly offset");
    CHECK(offsetof(pf_equity_stats_t, sortino_monthly) == 56, "sortino_monthly offset");
    CHECK(sizeof(pf_equity_stats_t) == 120, "equity stats size");
    memset(&stats, 0, sizeof(stats));
    stats.sharpe_monthly = 1.25;
    stats.sortino_monthly = -0.5;
    CHECK(stats.sharpe_monthly == 1.25, "sharpe_monthly roundtrip");
    CHECK(stats.sortino_monthly == -0.5, "sortino_monthly roundtrip");
}

static void check_native_run(void) {
    struct c99_host host;
    pf_native_callbacks_v1 callbacks;
    pf_native_run_spec_v1 spec;
    pf_native_state_v1 state;
    pf_native_event_v1 events[16];
    double units = 0.0;
    uint64_t window = 0;
    int written, i, applied = 0;

    memset(&host, 0, sizeof(host));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = (uint32_t)sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &host;
    callbacks.on_bar = c99_on_bar;
    host.handle = strategy_native_host_create_v1(&callbacks);
    CHECK(host.handle != NULL, "a C99 caller's callback table was refused");
    if (!host.handle) return;

    memset(&spec, 0, sizeof(spec));
    spec.struct_size = (uint32_t)sizeof(spec);
    spec.session_key = "native-c-api-c99";
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
    spec.fee_kind = PF_NATIVE_FEE_PERCENT;
    spec.close_execution = PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    spec.optional_mask = PF_NATIVE_SPEC_OPTIONAL_QUANTITY_GRID;
    spec.quantity_grid = 1.0;
    CHECK(strategy_configure_native_v1(host.handle, &spec) == 0, "the C99 spec was refused");
    CHECK(strategy_execution_contract(host.handle) == PF_EXECUTION_CONTRACT_NATIVE_MARKET_V1,
          "a native host is not on the native market contract");
    CHECK(strategy_native_run_v1(host.handle, kBars, 4, NULL) == PF_NATIVE_OK,
          "the C99 run did not complete");
    CHECK(host.calculations == 4 && host.unnamed == 0,
          "a C99 calculation was not the named calculation point");

    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state);
    CHECK(strategy_native_state_v1(host.handle, &state) == PF_NATIVE_OK, "C99 state read");
    CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED
              && state.completion == PF_NATIVE_COMPLETION_BATCH_COMPLETE
              && state.failure_code == PF_NATIVE_FAILURE_NONE,
          "the C99 run did not end as a completed batch");
    memset(events, 0, sizeof(events));
    written = strategy_native_events_v1(host.handle, 0, events, 16);
    for (i = 0; i < written; ++i) {
        if (events[i].kind == PF_NATIVE_EVENT_APPLIED && events[i].terminal
            && events[i].reason == PF_NATIVE_TERMINAL_WORKING_UNITS_SATISFIED) {
            ++applied;
        }
    }
    CHECK(applied == 1, "the C99 entry did not fill as one satisfied execution");
    /* V19-B: the journal window's two words. This caller read every event
     * after the run; its spec predates the retention word, so nothing was
     * dropped and the window starts at 1, and an acknowledgement after the
     * run has ended records nothing and fails nothing. */
    CHECK(strategy_native_event_window_v1(host.handle, &window) == PF_NATIVE_OK && window == 1u,
          "the C99 window did not start at 1");
    CHECK(strategy_native_acknowledge_events_v1(
              host.handle, written > 0 ? events[written - 1].ordinal : 0u)
              == PF_NATIVE_OK,
          "the C99 acknowledgement was refused");
    CHECK(strategy_native_event_window_v1(host.handle, NULL) == PF_NATIVE_E_ARGUMENT,
          "a NULL window out-parameter was accepted");
    CHECK(strategy_native_acknowledge_events_v1(NULL, 0u) == PF_NATIVE_E_HANDLE,
          "a NULL handle's acknowledgement was accepted");
    CHECK(strategy_native_position_v1(host.handle, &units, NULL, NULL) == PF_NATIVE_OK
              && units == 1.0,
          "the C99 entry did not leave a long 1");
    strategy_native_host_free(host.handle);
}

int main(void) {
    check_equity_layout();
    check_native_run();
    printf("test_native_c_api_c99: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
