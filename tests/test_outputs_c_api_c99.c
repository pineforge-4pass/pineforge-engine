/*
 * test_outputs_c_api_c99.c -- the recorded-outputs group of pineforge.h as a
 * strict C99 consumer compiles it.
 *
 * tests/CMakeLists.txt builds this translation unit as C99 with extensions
 * off and -pedantic-errors, so a C11 construct in group pf_outputs (parts A
 * and B) fails the build. The event record's layout is pinned at compile time
 * -- every 8-byte field at a multiple of 8, the 72-byte prefix before the one
 * pointer -- and the three per-library declarations are checked against their
 * signatures without referencing them (a runtime library does not define
 * them). The body runs a native C host, which declares no outputs, and reads
 * every runtime export of the group: recording is refused, every reader
 * answers empty, and a NULL handle answers -1.
 *
 * No assert(): the Release gate defines NDEBUG and would no-op every row.
 */

#include <pineforge/pineforge.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(PINEFORGE_HAS_OUTPUTS_V1) || PINEFORGE_HAS_OUTPUTS_V1 != 1
#error "pineforge.h does not announce the outputs group"
#endif

PF_STATIC_ASSERT(PF_OUTPUTS_API_VERSION == 1u);
PF_STATIC_ASSERT(sizeof(pf_output_phase_t) == 4);
PF_STATIC_ASSERT(PF_OUTPUT_PHASE_BATCH == 0);
PF_STATIC_ASSERT(PF_OUTPUT_PHASE_WARMUP == 1);
PF_STATIC_ASSERT(PF_OUTPUT_PHASE_REALTIME == 2);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, struct_version) == 0);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, size) == 4);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, sequence) == 8);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, output_index) == 16);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, bar_index) == 20);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, bar_open_ms) == 24);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, bar_close_ms) == 32);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, ordinal_in_bar) == 40);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, phase) == 44);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, confirmed) == 48);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, reserved0) == 52);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, value) == 56);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, message_hash64) == 64);
PF_STATIC_ASSERT(offsetof(pf_output_event_v1_t, message) == 72);
#if UINTPTR_MAX == UINT64_MAX
PF_STATIC_ASSERT(sizeof(pf_output_event_v1_t) == 80);
#endif

/* Part A, generated per library: each declaration has exactly this type. A
 * conditional expression with operands of incompatible pointer types is a
 * constraint violation, and sizeof does not evaluate (or reference) its
 * operand. */
typedef uint32_t (*outputs_api_version_fn)(void);
typedef int (*outputs_json_fn)(pf_strategy_t, char*, size_t, size_t*, char*, size_t);
PF_STATIC_ASSERT(sizeof(1 ? strategy_outputs_api_version : (outputs_api_version_fn)0)
                 == sizeof(outputs_api_version_fn));
PF_STATIC_ASSERT(sizeof(1 ? strategy_outputs_manifest : (outputs_json_fn)0)
                 == sizeof(outputs_json_fn));
PF_STATIC_ASSERT(sizeof(1 ? strategy_signal_safety_receipt : (outputs_json_fn)0)
                 == sizeof(outputs_json_fn));

static int failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const pf_bar_t kBars[] = {
    {100.0, 102.0,  99.0, 101.0, 4.0,      0},
    {101.0, 103.0, 100.0, 102.0, 4.0, 300000},
    {102.0, 104.0, 101.0, 103.0, 4.0, 600000}
};

static int c99_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    int* calculations = (int*)user;
    (void)bar;
    (void)at;
    ++*calculations;
    return 0;
}

static void check_undeclared_host(void) {
    pf_native_callbacks_v1 callbacks;
    pf_native_run_spec_v1 spec;
    pf_output_event_v1_t event;
    pf_strategy_t handle;
    int64_t times[4];
    double values[4];
    int64_t written = -7;
    int calculations = 0;
    const char* error;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = (uint32_t)sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &calculations;
    callbacks.on_bar = c99_on_bar;
    handle = strategy_native_host_create_v1(&callbacks);
    CHECK(handle != NULL, "the callback table was refused");
    if (!handle) return;

    memset(&spec, 0, sizeof(spec));
    spec.struct_size = (uint32_t)sizeof(spec);
    spec.session_key = "outputs-c99";
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
    CHECK(strategy_configure_native_v1(handle, &spec) == 0, "the spec was refused");
    CHECK(strategy_native_run_v1(handle, kBars, 3, NULL) == PF_NATIVE_OK, "the run did not complete");
    CHECK(calculations == 3, "the run did not calculate three bars");

    /* A C host records nothing in outputs v1: it declares no outputs. */
    CHECK(strategy_outputs_set_enabled(handle, 1) == -1, "recording was not refused");
    error = strategy_get_last_error(handle);
    CHECK(error != NULL && strcmp(error, "outputs: this module declares no outputs") == 0,
          "the refusal text");
    CHECK(strategy_outputs_set_enabled(handle, 0) == 0, "switching off was refused");
    CHECK(strategy_outputs_series_count(handle) == 0, "series count");
    CHECK(strategy_outputs_bars_len(handle) == 0, "bars length");
    CHECK(strategy_outputs_events_len(handle) == 0, "events length");
    CHECK(strategy_outputs_bar_times_copy(handle, 0, times, times + 2, 2, &written) == 0
              && written == 0,
          "bar times from bar 0");
    CHECK(strategy_outputs_bar_times_copy(handle, 1, times, NULL, 2, &written) == -1,
          "bar times past the end");
    CHECK(strategy_outputs_series_copy(handle, 0, 0, values, 4, &written) == -1, "series copy");
    memset(&event, 0, sizeof(event));
    CHECK(strategy_outputs_event_get(handle, 0, &event, sizeof(event)) == -1, "event copy");
    strategy_outputs_events_clear(handle);
    CHECK(strategy_outputs_constants_copy(handle, values, 4) == 0, "constants");
    CHECK(strategy_outputs_constants_copy(handle, NULL, 1) == -1, "constants into NULL");
    strategy_native_host_free(handle);

    CHECK(strategy_outputs_set_enabled(NULL, 0) == -1, "NULL set_enabled");
    CHECK(strategy_outputs_series_count(NULL) == -1, "NULL series_count");
    CHECK(strategy_outputs_bars_len(NULL) == -1, "NULL bars_len");
    CHECK(strategy_outputs_bar_times_copy(NULL, 0, times, NULL, 1, &written) == -1,
          "NULL bar_times_copy");
    CHECK(strategy_outputs_series_copy(NULL, 0, 0, values, 1, &written) == -1, "NULL series_copy");
    CHECK(strategy_outputs_events_len(NULL) == -1, "NULL events_len");
    CHECK(strategy_outputs_event_get(NULL, 0, &event, sizeof(event)) == -1, "NULL event_get");
    strategy_outputs_events_clear(NULL);
    CHECK(strategy_outputs_constants_copy(NULL, values, 1) == -1, "NULL constants_copy");
}

int main(void) {
    check_undeclared_host();
    printf("test_outputs_c_api_c99: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
