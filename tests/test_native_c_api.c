/*
 * test_native_c_api.c — the pure-C consumer of <pineforge/native_c_api.h>.
 *
 * Compiled by the C compiler, so any C++ syntax that leaks into the new
 * header is a hard build failure, exactly like test_c_abi.c. It owns both
 * halves of the L13 evidence that can be written in C:
 *
 *   - the two twin arms (market and Sized), whose records
 *     test_native_c_api_twin.cpp compares against the C++ host, and
 *   - pf_native_c_api_checks(), the behaviour suite: the documented refusals
 *     for a mis-sized struct and an unknown or unsupported tag, the
 *     replace / cancel / cancel_all / execute_current round-trips, the
 *     callback-failure latch and stable event polling.
 *
 * No assert(): the Release gate defines NDEBUG and would no-op every row.
 */

#include "native_c_api_twin.h"

#include <math.h>
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

#define CHECK_EQ_INT(actual, expected, msg)                                    \
    do {                                                                       \
        const long long _a = (long long)(actual);                              \
        const long long _e = (long long)(expected);                            \
        if (_a != _e) {                                                        \
            fprintf(stderr, "FAIL %s:%d  %s (got %lld, want %lld)\n",          \
                    __FILE__, __LINE__, (msg), _a, _e);                        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

/* ── The shared feed ────────────────────────────────────────────── */

#define TWIN_BARS 40

static pf_bar_t twin_bars[TWIN_BARS];
static int twin_bars_ready = 0;

const pf_bar_t* pf_twin_bars(int* n) {
    if (!twin_bars_ready) {
        int i;
        for (i = 0; i < TWIN_BARS; ++i) {
            const double open = 100.0 + (double)i;
            twin_bars[i].open = open;
            twin_bars[i].high = open + 2.0;
            twin_bars[i].low = open - 1.0;
            twin_bars[i].close = open + 1.0;
            twin_bars[i].volume = 5.0;
            twin_bars[i].timestamp = (int64_t)i * 300000;
        }
        twin_bars_ready = 1;
    }
    if (n) *n = TWIN_BARS;
    return twin_bars;
}

/* ── The run specification, identical in both twin arms ─────────── */

static pf_native_run_spec_v1 twin_spec(void) {
    pf_native_run_spec_v1 spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = (uint32_t)sizeof(spec);
    spec.session_key = "native-c-api-twin";
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
    spec.slippage_ticks = 0;
    spec.fee_kind = 0;            /* Percent */
    spec.fee_value = 0.0;
    spec.optional_mask = 0;
    spec.close_execution = 0;     /* NextEligiblePoint */
    spec.allowed_open_directions = 3; /* Both */
    return spec;
}

static pf_native_request_v1 blank_request(void) {
    pf_native_request_v1 request;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.version = PF_NATIVE_API_VERSION;
    return request;
}

static pf_native_callbacks_v1 blank_callbacks(void* user) {
    pf_native_callbacks_v1 table;
    memset(&table, 0, sizeof(table));
    table.struct_size = (uint32_t)sizeof(table);
    table.version = PF_NATIVE_API_VERSION;
    table.user = user;
    return table;
}

/* ── Twin arms ──────────────────────────────────────────────────── */

typedef struct twin_state {
    pf_strategy_t host;
    int           calculations;
    int           sized;          /* 1 = the Sized arm. */
    int           command_error;  /* first non-zero status a command returned. */
    uint64_t      entry;
    uint64_t      exit;
} twin_state;

static int twin_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    twin_state* state = (twin_state*)user;
    (void)bar;
    if (at->struct_size != sizeof(pf_native_decision_v1)) {
        state->command_error = -1000;
        return 0;
    }
    ++state->calculations;
    if (state->calculations == PF_TWIN_ENTRY_BAR) {
        pf_native_request_v1 request = blank_request();
        int rc;
        if (state->sized) {
            request.intent = PF_NATIVE_INTENT_SIZED;
            request.intent_value = 1000.0;
            request.side = PF_NATIVE_SIDE_LONG;
            request.size_basis = PF_NATIVE_SIZE_BASIS_CASH;
            request.size_time = PF_NATIVE_SIZE_AT_MATCH;
            request.grid_policy = PF_NATIVE_GRID_SNAP;
            request.label = "twin-sized";
        } else {
            request.intent = PF_NATIVE_INTENT_TRANSACT;
            request.intent_value = 1.0;
            request.label = "twin-long";
        }
        rc = strategy_native_submit_v1(state->host, &request, &state->entry, NULL);
        if (rc != PF_NATIVE_OK && state->command_error == 0) state->command_error = rc;
    } else if (state->calculations == PF_TWIN_EXIT_BAR) {
        pf_native_request_v1 request = blank_request();
        int rc;
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "twin-flat";
        rc = strategy_native_submit_v1(state->host, &request, &state->exit, NULL);
        if (rc != PF_NATIVE_OK && state->command_error == 0) state->command_error = rc;
    }
    return 0;
}

static void collect_events(pf_strategy_t host, pf_twin_result* out) {
    static pf_native_event_v1 events[PF_TWIN_MAX_EVENTS];
    int written;
    int i;
    memset(events, 0, sizeof(events));
    written = strategy_native_events_v1(host, 0, events, PF_TWIN_MAX_EVENTS);
    if (written < 0) written = 0;
    out->event_count = written;
    for (i = 0; i < written; ++i) {
        out->event_ordinal[i] = events[i].ordinal;
        out->event_kind[i] = events[i].kind;
        if (events[i].kind == PF_NATIVE_EVENT_APPLIED) {
            out->applied_price[i] = events[i].resolved_price;
            out->applied_closed[i] = events[i].closed_units;
            out->applied_opened[i] = events[i].opened_units;
        }
    }
}

static int twin_run(pf_twin_result* out, int sized) {
    twin_state state;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    pf_report_t report;
    const pf_bar_t* bars;
    int n = 0;
    int rc;
    int i;

    memset(out, 0, sizeof(*out));
    memset(&state, 0, sizeof(state));
    memset(&report, 0, sizeof(report));
    state.sized = sized;

    table = blank_callbacks(&state);
    table.on_bar = twin_on_bar;

    state.host = strategy_native_host_create_v1(&table);
    if (!state.host) return -1;

    if (strategy_configure_native_v1(state.host, &spec) != 0) {
        strategy_native_host_free(state.host);
        return -2;
    }

    bars = pf_twin_bars(&n);
    rc = strategy_native_run_v1(state.host, bars, n, &report);
    out->completed = (rc == PF_NATIVE_OK) ? 1 : 0;

    out->trade_count = report.total_trades;
    if (out->trade_count > PF_TWIN_MAX_TRADES) out->trade_count = PF_TWIN_MAX_TRADES;
    for (i = 0; i < out->trade_count; ++i) {
        out->trades[i].entry_price = report.trades[i].entry_price;
        out->trades[i].exit_price = report.trades[i].exit_price;
        out->trades[i].qty = report.trades[i].qty;
        out->trades[i].pnl = report.trades[i].pnl;
        out->trades[i].entry_time = report.trades[i].entry_time;
        out->trades[i].exit_time = report.trades[i].exit_time;
        out->trades[i].is_long = report.trades[i].is_long;
    }
    strategy_native_report_free_v1(&report);

    strategy_native_position_v1(state.host, &out->signed_units, &out->average_price,
                                &out->lots);
    collect_events(state.host, out);
    strategy_native_host_free(state.host);
    return state.command_error;
}

int pf_twin_run_c_market(pf_twin_result* out) { return twin_run(out, 0); }

int pf_twin_run_c_sized(pf_twin_result* out) { return twin_run(out, 1); }

/* ── The cancel_where twin arm ──────────────────────────────────── */

/* Three resting limits nobody can fill, whose identity fields cross: "leg" is
 * the label of two of them and the comment of the third. A form that read the
 * wrong field would therefore cancel the wrong rows, not none. */
typedef struct cancel_where_state {
    pf_strategy_t          host;
    int                    calculations;
    int                    command_error;
    pf_twin_cancel_where*  out;
} cancel_where_state;

static int cancel_where_submit(cancel_where_state* state, const char* label,
                               const char* comment) {
    pf_native_request_v1 request = blank_request();
    int rc;
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_LIMIT;
    request.p1 = 1.0;
    request.label = label;
    request.comment = comment;
    rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
    if (rc != PF_NATIVE_OK && state->command_error == 0) state->command_error = rc;
    return rc;
}

static int cancel_where_on_bar(void* user, const pf_bar_t* bar,
                               const pf_native_decision_v1* at) {
    cancel_where_state* state = (cancel_where_state*)user;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations != 5 || state->out->ran) return 0;
    state->out->ran = 1;

    if (cancel_where_submit(state, "leg", "entry") != PF_NATIVE_OK) return 0;
    if (cancel_where_submit(state, "leg", "exit") != PF_NATIVE_OK) return 0;
    if (cancel_where_submit(state, "other", "leg") != PF_NATIVE_OK) return 0;

    state->out->comment_hits = strategy_native_cancel_where_v1(
        state->host, "leg", PF_NATIVE_FIELD_COMMENT);
    state->out->live_after_comment = strategy_native_working_len_v1(state->host);
    state->out->label_hits = strategy_native_cancel_where_v1(
        state->host, "leg", PF_NATIVE_FIELD_LABEL);
    state->out->live_after_label = strategy_native_working_len_v1(state->host);
    state->out->unmatched_hits = strategy_native_cancel_where_v1(
        state->host, "nobody", PF_NATIVE_FIELD_LABEL);

    /* The two documented C refusals. Neither is a command: the book is
     * already empty and stays that way. */
    state->out->refused_unknown_field =
        strategy_native_cancel_where_v1(state->host, "leg", 77u) == PF_NATIVE_E_TAG;
    state->out->refused_null_text =
        strategy_native_cancel_where_v1(state->host, NULL, PF_NATIVE_FIELD_LABEL)
        == PF_NATIVE_E_ARGUMENT;
    return 0;
}

int pf_twin_run_c_cancel_where(pf_twin_cancel_where* out) {
    cancel_where_state state;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    const pf_bar_t* bars;
    int n = 0;
    int rc;

    memset(out, 0, sizeof(*out));
    memset(&state, 0, sizeof(state));
    state.out = out;

    table = blank_callbacks(&state);
    table.on_bar = cancel_where_on_bar;

    state.host = strategy_native_host_create_v1(&table);
    if (!state.host) return -1;
    if (strategy_configure_native_v1(state.host, &spec) != 0) {
        strategy_native_host_free(state.host);
        return -2;
    }
    bars = pf_twin_bars(&n);
    rc = strategy_native_run_v1(state.host, bars, n, NULL);
    out->completed = (rc == PF_NATIVE_OK) ? 1 : 0;
    strategy_native_host_free(state.host);
    return state.command_error;
}

/* ── Behaviour suite ────────────────────────────────────────────── */

/* Refusals are decided before the command reaches the kernel, so they are
 * observable from outside a callback on a merely Ready handle. */
static void check_struct_and_tag_refusals(void) {
    pf_native_callbacks_v1 table = blank_callbacks(NULL);
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_request_v1 request;
    pf_native_state_v1 state;
    pf_strategy_t host;
    pf_native_callbacks_v1 bad = blank_callbacks(NULL);

    bad.struct_size = (uint32_t)sizeof(bad) + 8u;
    CHECK(strategy_native_host_create_v1(&bad) == NULL, "mis-sized callback table accepted");
    bad = blank_callbacks(NULL);
    bad.version = PF_NATIVE_API_VERSION + 1u;
    CHECK(strategy_native_host_create_v1(&bad) == NULL, "unknown table version accepted");
    CHECK(strategy_native_host_create_v1(NULL) == NULL, "NULL callback table accepted");

    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "host create failed");
    if (!host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(host, &spec), 0, "configure_native_v1");

    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.struct_size = (uint32_t)sizeof(request) - 4u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_STRUCT, "mis-sized request not refused");

    request = blank_request();
    request.version = PF_NATIVE_API_VERSION + 1u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_STRUCT, "unknown request version not refused");

    request = blank_request();
    request.intent = 99u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_TAG, "unknown intent tag not refused");

    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.trigger = 42u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_TAG, "unknown trigger tag not refused");

    request = blank_request();
    request.intent = PF_NATIVE_INTENT_HOST_SIZED;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_UNSUPPORTED, "HostSized not refused as unsupported");

    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;  /* no incarnations supplied */
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_ARGUMENT, "owner without a parent not refused");

    /* A well-formed command outside a callback is a state refusal, never an
     * exception and never a mutation. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL),
                 PF_NATIVE_E_STATE, "submit outside a callback not refused");

    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state) + 4u;
    CHECK_EQ_INT(strategy_native_state_v1(host, &state), PF_NATIVE_E_STRUCT,
                 "mis-sized state struct not refused");
    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state);
    CHECK_EQ_INT(strategy_native_state_v1(host, &state), PF_NATIVE_OK, "state read");
    CHECK_EQ_INT(state.lifecycle, PF_NATIVE_LIFECYCLE_READY, "configured host is not Ready");
    CHECK_EQ_INT(state.failure_code, 0, "a Ready host carries a failure");

    /* A foreign handle is refused rather than misused. */
    CHECK_EQ_INT(strategy_native_working_len_v1(NULL), PF_NATIVE_E_HANDLE,
                 "NULL handle accepted");
    CHECK_EQ_INT(strategy_native_api_version(), PF_NATIVE_API_VERSION, "api version");

    strategy_native_host_free(host);
}

/* The spec extension replaces strategy_configure_native_v1 for a host that
 * needs the L2-L8 fields: the kernel configures a host exactly once. Every
 * refusal below must leave the handle usable, which is why the entry point
 * checks readiness itself instead of letting the kernel fail the host. */
static void check_spec_extension(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table = blank_callbacks(NULL);
    pf_native_state_v1 state;
    pf_report_t report;
    pf_strategy_t host;
    const pf_bar_t* bars;
    int n = 0;

    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "extension host create failed");
    if (!host) return;

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext) - 8u;
    ext.version = PF_NATIVE_API_VERSION;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_STRUCT,
                 "mis-sized spec extension not refused");

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_CALCULATION;
    ext.calculation = 7u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "unknown calculation trigger not refused");

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = 1u << 20;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "unknown extension block not refused");

    /* The risk block lives in the appended tail, so a caller sending the base
     * layout cannot declare it: the fields are not in its struct at all. */
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_RISK;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_STRUCT,
                 "the risk block was accepted from a base-layout extension");

    /* The mask is bounded before the tail is: an unknown block is E_TAG even
     * when the risk bit is set without the tail to back it. */
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_RISK | (1u << 20);
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "an unknown block is judged before the missing tail");

    /* None of those refusals may have touched the handle. */
    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state);
    CHECK_EQ_INT(strategy_native_state_v1(host, &state), PF_NATIVE_OK, "extension state read");
    CHECK_EQ_INT(state.lifecycle, PF_NATIVE_LIFECYCLE_UNCONFIGURED,
                 "a refused extension configured or failed the host");

    /* The kernel-recorded report policy is an L2 field the v1 spec predates. */
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT | PF_NATIVE_SPEC_EXT_CALCULATION;
    ext.report_policy = 1u;   /* KernelRecorded */
    ext.calculation = 1u;     /* BarCloseAndFills */
    ext.max_recalculations_per_point = 4u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_OK,
                 "the spec extension was refused");

    /* Configured once: a second attempt is refused, and the handle survives. */
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_STATE,
                 "a second extension configure was not refused");
    CHECK_EQ_INT(strategy_native_state_v1(host, &state), PF_NATIVE_OK, "post-refusal state read");
    CHECK_EQ_INT(state.lifecycle, PF_NATIVE_LIFECYCLE_READY,
                 "the refused second configure failed the host");

    /* A kernel-recorded report carries the per-bar broker-state hash too: the
     * existing recording switch is all a C host needs, one row per script bar
     * and per curve point. */
    strategy_set_broker_state_hash_recording(host, 1);
    memset(&report, 0, sizeof(report));
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(host, bars, n, &report), PF_NATIVE_OK,
                 "the extended run did not complete");
    CHECK(report.script_bars_processed > 0, "the extended run calculated no script bar");
    CHECK(report.broker_state_hash != NULL, "a kernel-recorded C run recorded no hash row");
    CHECK(report.broker_state_hash_len == report.script_bars_processed,
          "broker_state_hash_len is not 1:1 with script_bars_processed");
    CHECK(report.broker_state_hash_len == report.equity_curve_len,
          "broker_state_hash_len is not 1:1 with the kernel-recorded curve");
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(host);
}

/* replace / cancel / cancel_all / execute_current, all from inside a
 * callback, where the kernel's legality rule admits them. */
typedef struct lifecycle_state {
    pf_strategy_t host;
    int           calculations;
    int           done;
    int           failures;
    uint64_t      resting;
    uint64_t      successor;
} lifecycle_state;

/* Inside a callback there is no global CHECK context, so each row reports
 * its own line and counts into the callback's own tally. */
#define LCHECK(state, cond, msg)                                               \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
            ++(state)->failures;                                               \
        }                                                                      \
    } while (0)

static int lifecycle_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    lifecycle_state* state = (lifecycle_state*)user;
    pf_native_request_v1 request;
    pf_native_working_v1 working;
    uint32_t refusal = 0xffffffffu;
    uint64_t immediate = 0;
    double units = 0.0;
    int rc;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations != 5 || state->done) return 0;
    state->done = 1;

    /* A resting limit far below the market: it never fills on its own. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_LIMIT;
    request.p1 = 1.0;
    request.label = "resting";
    rc = strategy_native_submit_v1(state->host, &request, &state->resting, NULL);
    LCHECK(state, rc == PF_NATIVE_OK, "resting limit submit refused");
    if (rc != PF_NATIVE_OK) return 0;

    /* The working view sees exactly that request. */
    LCHECK(state, strategy_native_working_len_v1(state->host) == 1,
           "the working view does not hold one row");
    memset(&working, 0, sizeof(working));
    working.struct_size = (uint32_t)sizeof(working);
    rc = strategy_native_working_get_v1(state->host, 0, &working);
    LCHECK(state, rc == PF_NATIVE_OK, "working_get refused row 0");
    if (rc == PF_NATIVE_OK) {
        LCHECK(state, working.incarnation == state->resting, "working row names another request");
        LCHECK(state, working.intent == PF_NATIVE_INTENT_TRANSACT, "working intent differs");
        LCHECK(state, working.trigger == PF_NATIVE_TRIGGER_LIMIT, "working trigger differs");
        LCHECK(state, working.p1 == 1.0, "working trigger level differs");
        LCHECK(state, working.intent_value == 1.0, "working intent value differs");
        LCHECK(state, working.label != NULL && strcmp(working.label, "resting") == 0,
               "working label differs");
        LCHECK(state, working.remaining_kind == PF_NATIVE_REMAINING_UNITS
                      || working.remaining_kind == PF_NATIVE_REMAINING_UNBOUND,
               "working remaining kind is not a live projection");
    }
    memset(&working, 0, sizeof(working));
    working.struct_size = (uint32_t)sizeof(working);
    LCHECK(state, strategy_native_working_get_v1(state->host, 7, &working)
                  == PF_NATIVE_E_ARGUMENT,
           "an out-of-range working index was not refused");
    memset(&working, 0, sizeof(working));
    working.struct_size = (uint32_t)sizeof(working) + 4u;
    LCHECK(state, strategy_native_working_get_v1(state->host, 0, &working)
                  == PF_NATIVE_E_STRUCT,
           "a mis-sized working row was not refused");

    /* replace round-trip: a new level, a new incarnation, still one row. */
    request.p1 = 2.0;
    request.label = "replaced";
    rc = strategy_native_replace_v1(state->host, state->resting, &request, &state->successor);
    LCHECK(state, rc == PF_NATIVE_OK, "replace refused");
    LCHECK(state, state->successor != state->resting, "replace reused the incarnation");
    LCHECK(state, strategy_native_working_len_v1(state->host) == 1,
           "replace did not leave exactly one live request");
    LCHECK(state, strategy_native_replace_v1(state->host, state->resting, &request, NULL)
                  == PF_NATIVE_E_NOT_WORKING,
           "replacing the predecessor again was not refused");

    /* cancel round-trip. */
    LCHECK(state, strategy_native_cancel_v1(state->host, state->successor) == PF_NATIVE_OK,
           "cancel refused");
    LCHECK(state, strategy_native_cancel_v1(state->host, state->successor)
                  == PF_NATIVE_E_NOT_WORKING,
           "cancelling twice was not refused");
    LCHECK(state, strategy_native_working_len_v1(state->host) == 0,
           "the working view still holds a cancelled request");

    /* cancel_all: two fresh resting requests, both withdrawn. */
    request.label = "bulk-a";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "bulk-a submit refused");
    request.label = "bulk-b";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "bulk-b submit refused");
    LCHECK(state, strategy_native_working_len_v1(state->host) == 2,
           "the working view does not hold both bulk requests");
    LCHECK(state, strategy_native_cancel_all_v1(state->host) == 2,
           "cancel_all did not report two cancellations");
    LCHECK(state, strategy_native_working_len_v1(state->host) == 0,
           "cancel_all left a live request");

    /* execute_current: the two documented target refusals — a handle that is
     * not an issued one at all, and one this run never made live — then a
     * live market request that applies at this very point. */
    LCHECK(state, strategy_native_execute_current_v1(state->host, 0u,
                                                     PF_NATIVE_PRICE_AS_PRESENTED, &refusal)
                  == PF_NATIVE_E_REFUSED,
           "execute_current accepted the null handle");
    LCHECK(state, refusal == PF_NATIVE_REFUSAL_INVALID_HANDLE,
           "the null handle was not refused as InvalidHandle");
    refusal = 0xffffffffu;
    LCHECK(state, strategy_native_execute_current_v1(state->host, 999999u,
                                                     PF_NATIVE_PRICE_AS_PRESENTED, &refusal)
                  == PF_NATIVE_E_REFUSED,
           "execute_current accepted an unissued target");
    LCHECK(state, refusal == PF_NATIVE_REFUSAL_NOT_WORKING,
           "an unissued target was not refused as NotWorking");

    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.label = "immediate";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, &immediate, NULL)
                  == PF_NATIVE_OK,
           "immediate submit refused");
    LCHECK(state, strategy_native_execute_current_v1(state->host, immediate,
                                                     PF_NATIVE_PRICE_AS_PRESENTED, NULL)
                  == PF_NATIVE_EXECUTED_APPLIED,
           "execute_current did not apply");
    LCHECK(state, strategy_native_position_v1(state->host, &units, NULL, NULL) == PF_NATIVE_OK,
           "position read refused");
    LCHECK(state, units == 1.0, "execute_current did not open one unit");
    LCHECK(state, strategy_native_execute_current_v1(state->host, immediate, 77u, NULL)
                  == PF_NATIVE_E_TAG,
           "an unknown price rule was not refused");

    /* Flatten again so the run ends the way the other scenarios do. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_FLATTEN;
    request.label = "lifecycle-flat";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "lifecycle flatten refused");
    return 0;
}

static void check_lifecycle_round_trips(void) {
    lifecycle_state state;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    const pf_bar_t* bars;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = lifecycle_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "lifecycle host create failed");
    if (!state.host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(state.host, &spec), 0, "lifecycle configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "lifecycle run did not complete");
    CHECK_EQ_INT(state.failures, 0, "lifecycle round-trips reported failures");
    CHECK(state.done == 1, "the lifecycle callback never ran");
    strategy_native_host_free(state.host);
}

/* A callback that returns non-zero latches CallbackException and ends the run
 * Failed with the documented code. */
static int refusing_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    int* calls = (int*)user;
    (void)bar;
    (void)at;
    ++*calls;
    return (*calls == 3) ? 7 : 0;
}

static void check_callback_failure_latch(void) {
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_state_v1 state;
    pf_strategy_t host;
    const pf_bar_t* bars;
    int calls = 0;
    int n = 0;

    table = blank_callbacks(&calls);
    table.on_bar = refusing_on_bar;
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "refusing host create failed");
    if (!host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(host, &spec), 0, "refusing configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(host, bars, n, NULL), PF_NATIVE_E_RUN_FAILED,
                 "a refusing callback did not fail the run");

    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state);
    CHECK_EQ_INT(strategy_native_state_v1(host, &state), PF_NATIVE_OK, "failed-state read");
    CHECK_EQ_INT(state.lifecycle, PF_NATIVE_LIFECYCLE_FAILED, "lifecycle is not Failed");
    CHECK_EQ_INT(state.failure_code, PF_NATIVE_FAILURE_CALLBACK,
                 "failure is not CallbackException");
    CHECK_EQ_INT(calls, 3, "the run continued past the refusing callback");
    strategy_native_host_free(host);
}

/* Event polling: the history is append-only, so the same cursor always
 * answers the same prefix and advancing by the last ordinal never repeats or
 * skips a row. */
static void check_event_polling(void) {
    pf_twin_result result;
    pf_strategy_t host;
    twin_state state;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    static pf_native_event_v1 first[PF_TWIN_MAX_EVENTS];
    static pf_native_event_v1 again[PF_TWIN_MAX_EVENTS];
    static pf_native_event_v1 page[8];
    const pf_bar_t* bars;
    int n = 0;
    int a, b, i, seen;
    uint64_t cursor;

    memset(&result, 0, sizeof(result));
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = twin_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "polling host create failed");
    if (!state.host) return;
    host = state.host;
    CHECK_EQ_INT(strategy_configure_native_v1(host, &spec), 0, "polling configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(host, bars, n, NULL), PF_NATIVE_OK, "polling run");

    memset(first, 0, sizeof(first));
    memset(again, 0, sizeof(again));
    a = strategy_native_events_v1(host, 0, first, PF_TWIN_MAX_EVENTS);
    b = strategy_native_events_v1(host, 0, again, PF_TWIN_MAX_EVENTS);
    CHECK(a > 0, "the run recorded no events");
    CHECK_EQ_INT(a, b, "the same cursor answered a different length");
    for (i = 0; i < a && i < b; ++i) {
        CHECK(first[i].ordinal == again[i].ordinal, "event ordinal moved between calls");
        CHECK(first[i].kind == again[i].kind, "event kind moved between calls");
        /* Non-decreasing, not strictly increasing: an applied execution and
         * its account observation share one ordinal. */
        if (i > 0) CHECK(first[i].ordinal >= first[i - 1].ordinal, "events are not ordered");
    }

    /* Walking the history in pages of 8 visits exactly the same rows. */
    cursor = 0;
    seen = 0;
    for (;;) {
        int written;
        memset(page, 0, sizeof(page));
        written = strategy_native_events_v1(host, cursor, page, 8);
        CHECK(written >= 0, "paged polling failed");
        if (written <= 0) break;
        for (i = 0; i < written; ++i) {
            if (seen < a) {
                CHECK(page[i].ordinal == first[seen].ordinal, "paged ordinal differs");
                CHECK(page[i].kind == first[seen].kind, "paged kind differs");
            }
            ++seen;
        }
        cursor = page[written - 1].ordinal;
    }
    CHECK_EQ_INT(seen, a, "paged polling visited a different number of events");
    CHECK_EQ_INT(strategy_native_events_v1(host, first[a - 1].ordinal, page, 8), 0,
                 "polling past the last ordinal returned rows");
    strategy_native_host_free(host);
}

/* ── L9's risk limits, read back through the C event history ────── */

/* One opening per calculation, with a limit of one applied fill per day: the
 * first fill reaches the limit, the kernel appends a NativeRiskEvent, and
 * every later opening of that day is refused. All forty twin bars are five
 * minutes apart from epoch 0, so they are one session day and the block never
 * lifts inside the run. */
typedef struct risk_state {
    pf_strategy_t host;
    int           calculations;
    int           submit_error;
} risk_state;

static int risk_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    risk_state* state = (risk_state*)user;
    pf_native_request_v1 request;
    int rc;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations != 1 && state->calculations != 5 && state->calculations != 9) {
        return 0;
    }
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.label = state->calculations == 1 ? "risk-enter" : "risk-probe";
    rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
    /* The kernel accepts every one of them: a risk block refuses the MATCH,
     * not the command. */
    if (rc != PF_NATIVE_OK && state->submit_error == 0) state->submit_error = rc;
    return 0;
}

static void check_risk_event(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    risk_state state;
    static pf_native_event_v1 events[PF_TWIN_MAX_EVENTS];
    const pf_bar_t* bars;
    int n = 0;
    int written, i;
    int risk_events = 0;
    int applied = 0;
    int first_risk = -1;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = risk_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "risk host create failed");
    if (!state.host) return;

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_RISK;
    ext.risk_has_max_fills_per_day = 1u;
    ext.risk_max_fills_per_day = 1u;
    ext.risk_day_basis = PF_NATIVE_RISK_DAY_SESSION;
    ext.risk_action = PF_NATIVE_RISK_BLOCK_OPENINGS;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the risk extension was refused");

    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the risk run did not complete");
    CHECK_EQ_INT(state.submit_error, 0, "a command was refused during the risk run");

    memset(events, 0, sizeof(events));
    written = strategy_native_events_v1(state.host, 0, events, PF_TWIN_MAX_EVENTS);
    CHECK(written > 0, "the risk run recorded no events");
    for (i = 0; i < written; ++i) {
        if (events[i].kind == PF_NATIVE_EVENT_RISK && first_risk < 0) first_risk = i;
        if (events[i].kind == PF_NATIVE_EVENT_RISK) ++risk_events;
        if (events[i].kind == PF_NATIVE_EVENT_APPLIED) ++applied;
    }
    CHECK_EQ_INT(risk_events, 1, "the breach delivered a different number of risk events");
    CHECK_EQ_INT(applied, 1, "the risk block did not stop the later openings");
    if (first_risk >= 0) {
        const pf_native_event_v1* row = &events[first_risk];
        CHECK_EQ_INT(row->reason, PF_NATIVE_RISK_MAX_FILLS_PER_DAY,
                     "the risk event named another limit");
        CHECK(row->price == 1.0, "the risk event reported another observed value");
        CHECK(row->raw_price == 1.0, "the risk event reported another limit");
        CHECK(row->incarnation == 0u, "the risk event named a request");
        CHECK(row->successor != 0u, "the risk event carried no matching point");
        CHECK(row->effective_time_ms > 0, "the risk event carried no cursor");
        CHECK_EQ_INT(row->struct_size, (int)sizeof(pf_native_event_v1),
                     "the risk event is a different struct");
        CHECK_EQ_INT(row->version, PF_NATIVE_API_VERSION, "the risk event is another version");
        /* Epoch day 0 on a 24x7 session: the day key is the session day the
         * breach was measured in, and every twin bar is in it. */
        CHECK_EQ_INT(row->cycle_before, 0, "the risk event named another risk day");
    }
    strategy_native_host_free(state.host);
}

/* ── The read-only accessors the C++ host has answered all along ──
 *
 * Eight of them are optionals in C++; PF_NATIVE_ABSENT is the C spelling of
 * that empty, and these two scenarios execute BOTH sides of every one that
 * has two. */

typedef struct probe_state {
    pf_strategy_t host;
    int           calculations;
    int           applied;
    int           failures;
    int           absent_partial_at_close;
    int           present_partial_at_applied;
    int           absent_series_before_delivery;
    int           present_series_after_delivery;
    int           absent_liquidation_when_flat;
    double        liquidation_price;
    double        position_units;
    double        position_avg;
    double        marked_equity_probe;
    double        marked_equity_mark;
    uint64_t      trail;
    int           trail_unarmed_seen;
    int           trail_armed_seen;
    double        trail_best;
    double        trail_level;
    uint64_t      risk_fills_today;
    uint32_t      risk_blocked;
} probe_state;

static int plain_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    probe_state* state = (probe_state*)user;
    pf_bar_t partial;
    pf_native_trail_state_v1 trail;
    pf_native_risk_state_v1 risk;
    double price = 0.0;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations != 3) return 0;

    /* The bar's own close calculation is outside the path walk. */
    memset(&partial, 0, sizeof(partial));
    if (strategy_native_partial_bar_v1(state->host, &partial) == PF_NATIVE_ABSENT) {
        state->absent_partial_at_close = 1;
    }
    /* A run that declared no subscription has no series at any index. */
    if (strategy_native_series_bar_v1(state->host, 0u, &partial) == PF_NATIVE_ABSENT) {
        state->absent_series_before_delivery = 1;
    }
    /* No margin model and a flat book: no level to solve. */
    price = 1.0;
    if (strategy_native_liquidation_price_v1(state->host, &price) == PF_NATIVE_ABSENT
        && price != price) {
        state->absent_liquidation_when_flat = 1;
    }
    /* An incarnation this run never issued is not a live trail. */
    memset(&trail, 0, sizeof(trail));
    trail.struct_size = (uint32_t)sizeof(trail);
    LCHECK(state, strategy_native_trail_state_v1(state->host, 4242u, &trail)
                      == PF_NATIVE_ABSENT,
           "an unissued incarnation answered a trail state");
    /* A run with no risk block reports the zero ledger, not a refusal. */
    memset(&risk, 0, sizeof(risk));
    risk.struct_size = (uint32_t)sizeof(risk);
    LCHECK(state, strategy_native_risk_state_v1(state->host, &risk) == PF_NATIVE_OK,
           "the zero risk ledger was refused");
    LCHECK(state, risk.blocked == 0u && risk.has_reason == 0u && risk.fills_today == 0u
                      && risk.consecutive_loss_days == 0u && risk.peak_equity == 0.0
                      && risk.day_open_equity == 0.0,
           "a run without a risk block reported a nonzero ledger");
    return 0;
}

static void check_absent_accessors(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    pf_native_trail_state_v1 trail;
    pf_native_risk_state_v1 risk;
    probe_state state;
    const pf_bar_t* bars;
    pf_bar_t bar;
    uint64_t driven = 0xffffffffu;
    uint64_t skipped = 0xffffffffu;
    uint64_t first_hash = 0;
    uint64_t second_hash = 0;
    double value = 0.0;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = plain_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "absent-accessor host create failed");
    if (!state.host) return;

    /* Every accessor refuses a NULL output before it reads the kernel. */
    CHECK_EQ_INT(strategy_native_partial_bar_v1(state.host, NULL), PF_NATIVE_E_ARGUMENT,
                 "partial_bar accepted a NULL output");
    CHECK_EQ_INT(strategy_native_series_bar_v1(state.host, 0u, NULL), PF_NATIVE_E_ARGUMENT,
                 "series_bar accepted a NULL output");
    CHECK_EQ_INT(strategy_native_liquidation_price_v1(state.host, NULL), PF_NATIVE_E_ARGUMENT,
                 "liquidation_price accepted a NULL output");
    CHECK_EQ_INT(strategy_native_marked_equity_v1(state.host, 100.0, NULL),
                 PF_NATIVE_E_ARGUMENT, "marked_equity accepted a NULL output");
    CHECK_EQ_INT(strategy_native_continuation_hash_v1(state.host, NULL), PF_NATIVE_E_ARGUMENT,
                 "continuation_hash accepted a NULL output");
    CHECK_EQ_INT(strategy_native_risk_state_v1(state.host, NULL), PF_NATIVE_E_ARGUMENT,
                 "risk_state accepted a NULL output");
    CHECK_EQ_INT(strategy_native_trail_state_v1(state.host, 1u, NULL), PF_NATIVE_E_ARGUMENT,
                 "trail_state accepted a NULL output");
    /* Both counters are optional; asking for neither is still legal. */
    CHECK_EQ_INT(strategy_native_recalculations_v1(state.host, NULL, NULL), PF_NATIVE_OK,
                 "recalculations refused two NULL counters");

    /* The two size-prefixed outputs enforce their prefix. */
    memset(&trail, 0, sizeof(trail));
    trail.struct_size = (uint32_t)sizeof(trail) - 4u;
    CHECK_EQ_INT(strategy_native_trail_state_v1(state.host, 1u, &trail), PF_NATIVE_E_STRUCT,
                 "a mis-sized trail state was not refused");
    memset(&risk, 0, sizeof(risk));
    risk.struct_size = (uint32_t)sizeof(risk) - 4u;
    CHECK_EQ_INT(strategy_native_risk_state_v1(state.host, &risk), PF_NATIVE_E_STRUCT,
                 "a mis-sized risk state was not refused");

    /* A handle this API did not create is refused by every accessor. */
    CHECK_EQ_INT(strategy_native_partial_bar_v1(NULL, &bar), PF_NATIVE_E_HANDLE,
                 "partial_bar accepted a NULL handle");
    CHECK_EQ_INT(strategy_native_continuation_hash_v1(NULL, &first_hash), PF_NATIVE_E_HANDLE,
                 "continuation_hash accepted a NULL handle");

    /* Before configure there is no run identity for a handle to name. */
    memset(&trail, 0, sizeof(trail));
    trail.struct_size = (uint32_t)sizeof(trail);
    CHECK_EQ_INT(strategy_native_trail_state_v1(state.host, 1u, &trail), PF_NATIVE_E_STATE,
                 "trail_state answered before the run had an identity");

    CHECK_EQ_INT(strategy_configure_native_v1(state.host, &spec), 0, "absent-accessor configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the absent-accessor run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback absent-accessor rows failed");
    CHECK(state.absent_partial_at_close,
          "current_partial_bar answered inside the bar's own close calculation");
    CHECK(state.absent_series_before_delivery,
          "an undeclared subscription index answered a series bar");
    CHECK(state.absent_liquidation_when_flat,
          "a flat book with no margin model answered a liquidation price");

    /* The counters measure RE-calculations: the script bar's own close
     * calculation is not one, so a BarClose run drives none and suppresses
     * none however many bars it calculated. check_live_accessors() is the
     * other side of this, under BarCloseAndFills. */
    CHECK(state.calculations == n, "the plain run did not calculate every bar");
    CHECK_EQ_INT(strategy_native_recalculations_v1(state.host, &driven, &skipped),
                 PF_NATIVE_OK, "recalculation counters refused");
    CHECK_EQ_INT((int)driven, 0, "a BarClose run drove a recalculation");
    CHECK_EQ_INT((int)skipped, 0, "an unbudgeted BarClose run suppressed a calculation");

    CHECK_EQ_INT(strategy_native_continuation_hash_v1(state.host, &first_hash), PF_NATIVE_OK,
                 "continuation hash refused");
    CHECK(first_hash != 0u, "a completed run reported a zero continuation hash");

    /* Marked equity is answerable after the run: a flat book marks at the
     * capital the run started with, whatever the mark. */
    CHECK_EQ_INT(strategy_native_marked_equity_v1(state.host, 1000.0, &value), PF_NATIVE_OK,
                 "marked_equity refused");
    CHECK(value == spec.initial_capital,
          "a flat book did not mark at the initial capital");
    strategy_native_host_free(state.host);

    /* The identity is a function of the declarations and the inputs, so the
     * same run answers the same hash. */
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = plain_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "second absent-accessor host create failed");
    if (!state.host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(state.host, &spec), 0, "second configure");
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the second run did not complete");
    CHECK_EQ_INT(strategy_native_continuation_hash_v1(state.host, &second_hash), PF_NATIVE_OK,
                 "second continuation hash refused");
    CHECK(first_hash == second_hash,
          "two identical runs answered different continuation hashes");
    strategy_native_host_free(state.host);
}

/* The present side: a run that declares a margin model, a risk limit, a
 * higher-timeframe series and the fill-cascade trigger, so every accessor
 * that can answer does. */

#define PROBE_ENTRY_BAR 3
#define PROBE_TRAIL_BAR 6
#define PROBE_READ_BAR  12
#define PROBE_CAPITAL   100.0
#define PROBE_UNITS     2.0
#define PROBE_MAINTENANCE 0.25
#define PROBE_TRAIL_OFFSET 5.0

static int probe_on_applied(void* user, const pf_native_applied_v1* applied,
                            const pf_native_decision_v1* at) {
    probe_state* state = (probe_state*)user;
    pf_bar_t partial;
    (void)applied;
    (void)at;
    ++state->applied;
    if (state->applied != 1) return 0;
    /* An applied fill sits inside the script bar's path walk, which is
     * exactly where the bar so far exists. */
    memset(&partial, 0, sizeof(partial));
    if (strategy_native_partial_bar_v1(state->host, &partial) == PF_NATIVE_OK) {
        state->present_partial_at_applied =
            (partial.high >= partial.close && partial.low <= partial.close) ? 1 : 0;
    }
    return 0;
}

static int probe_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    probe_state* state = (probe_state*)user;
    pf_native_request_v1 request;
    pf_native_trail_state_v1 trail;
    pf_native_risk_state_v1 risk;
    pf_bar_t series;
    double price = 0.0;
    uint64_t incarnation = 0;
    (void)bar;
    (void)at;
    ++state->calculations;

    if (state->calculations == PROBE_ENTRY_BAR) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = PROBE_UNITS;
        request.label = "probe-entry";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                          == PF_NATIVE_OK, "the probe entry was refused");
        return 0;
    }
    if (state->calculations == PROBE_TRAIL_BAR) {
        /* A protective sell trail on the long, armed just above the entry.
         * The feed only rises, so it arms and tracks without ever firing. */
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_REDUCE;
        request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
        request.intent_value = PROBE_UNITS;
        request.trigger = PF_NATIVE_TRIGGER_TRAIL;
        request.p1 = PROBE_TRAIL_OFFSET;
        request.trail_has_arm_price = 1;
        request.p2 = bar->close + 1.0;
        request.label = "probe-trail";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &incarnation, NULL)
                          == PF_NATIVE_OK, "the probe trail was refused");
        state->trail = incarnation;
        memset(&trail, 0, sizeof(trail));
        trail.struct_size = (uint32_t)sizeof(trail);
        LCHECK(state, strategy_native_trail_state_v1(state->host, state->trail, &trail)
                          == PF_NATIVE_OK, "a live trail answered no state");
        state->trail_unarmed_seen = (trail.activated == 0u && trail.best_price == 0.0
                                     && trail.current_level == 0.0
                                     && trail.activation_ordinal == 0u) ? 1 : 0;
        return 0;
    }
    if (state->calculations != PROBE_READ_BAR) return 0;

    /* The trail has long since armed and is riding the running best. */
    memset(&trail, 0, sizeof(trail));
    trail.struct_size = (uint32_t)sizeof(trail);
    LCHECK(state, strategy_native_trail_state_v1(state->host, state->trail, &trail)
                      == PF_NATIVE_OK, "the armed trail answered no state");
    state->trail_armed_seen = (trail.activated != 0u && trail.activation_ordinal != 0u) ? 1 : 0;
    state->trail_best = trail.best_price;
    state->trail_level = trail.current_level;

    /* The "15" series over a "5" input has delivered completed buckets. */
    memset(&series, 0, sizeof(series));
    if (strategy_native_series_bar_v1(state->host, 0u, &series) == PF_NATIVE_OK) {
        state->present_series_after_delivery =
            (series.high >= series.low && series.timestamp >= 0) ? 1 : 0;
    }
    /* An index past the declared list stays absent. */
    LCHECK(state, strategy_native_series_bar_v1(state->host, 4u, &series) == PF_NATIVE_ABSENT,
           "an undeclared subscription index answered");

    /* A carried long under a maintenance-only model has a solvable level. */
    LCHECK(state, strategy_native_position_v1(state->host, &state->position_units,
                                              &state->position_avg, NULL) == PF_NATIVE_OK,
           "the probe position was refused");
    price = 0.0;
    if (strategy_native_liquidation_price_v1(state->host, &price) == PF_NATIVE_OK) {
        state->liquidation_price = price;
    }
    state->marked_equity_mark = state->position_avg + 10.0;
    LCHECK(state, strategy_native_marked_equity_v1(state->host, state->marked_equity_mark,
                                                   &state->marked_equity_probe)
                      == PF_NATIVE_OK, "marked equity was refused mid-run");

    memset(&risk, 0, sizeof(risk));
    risk.struct_size = (uint32_t)sizeof(risk);
    LCHECK(state, strategy_native_risk_state_v1(state->host, &risk) == PF_NATIVE_OK,
           "the live risk ledger was refused");
    state->risk_fills_today = risk.fills_today;
    state->risk_blocked = risk.blocked;
    LCHECK(state, risk.has_day != 0u, "a live risk ledger reported no day");
    LCHECK(state, risk.peak_equity > 0.0, "a live risk ledger reported no equity peak");
    return 0;
}

static void check_live_accessors(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_subscription_v1 series;
    pf_native_callbacks_v1 table;
    probe_state state;
    const pf_bar_t* bars;
    uint64_t driven = 0;
    uint64_t skipped = 0;
    double expected = 0.0;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = probe_on_bar;
    table.on_applied = probe_on_applied;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "live-accessor host create failed");
    if (!state.host) return;

    spec.initial_capital = PROBE_CAPITAL;

    memset(&series, 0, sizeof(series));
    series.struct_size = (uint32_t)sizeof(series);
    series.tf = "15";

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_MARGIN | PF_NATIVE_SPEC_EXT_RISK
                     | PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS | PF_NATIVE_SPEC_EXT_CALCULATION;
    /* Maintenance-only: no kernel opening requirement, so the host owns
     * admission and the leveraged probe position is legal. */
    ext.margin_initial_long = 0.0;
    ext.margin_initial_short = 0.0;
    ext.margin_has_maintenance_long = 1u;
    ext.margin_maintenance_long = PROBE_MAINTENANCE;
    ext.margin_has_maintenance_short = 1u;
    ext.margin_maintenance_short = PROBE_MAINTENANCE;
    ext.margin_sizing = 2u;   /* Flatten */
    /* Every field of a PRESENT block is read, so the multiple carries the
     * kernel's own 1.0 even under a sizing policy that never scales. */
    ext.margin_shortfall_multiple = 1.0;
    ext.margin_check = 0u;    /* PathAdverseExtreme */
    ext.risk_has_max_fills_per_day = 1u;
    ext.risk_max_fills_per_day = 1u;
    ext.risk_day_basis = 0u;  /* SessionDay */
    ext.risk_action = 0u;     /* BlockOpenings */
    ext.subscriptions = &series;
    ext.subscriptions_n = 1u;
    ext.calculation = 1u;     /* BarCloseAndFills */
    ext.max_recalculations_per_point = 4u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the live-accessor extension was refused");

    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the live-accessor run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback live-accessor rows failed");

    CHECK(state.present_partial_at_applied,
          "current_partial_bar answered nothing at an applied fill");
    CHECK(state.present_series_after_delivery,
          "a delivered 15m series answered no bar");
    CHECK(state.trail_unarmed_seen,
          "a trail before its arm did not report the zero projection");
    CHECK(state.trail_armed_seen, "an armed trail did not report activation");
    CHECK(state.trail_best > 0.0 && state.trail_level > 0.0,
          "an armed trail reported no best or level");
    /* The level rides exactly the declared offset behind the running best. */
    CHECK(fabs((state.trail_best - state.trail_level) - PROBE_TRAIL_OFFSET) < 1e-9,
          "the trail level is not the offset behind the best");

    /* equity(P) = capital + units * (P - avg); the level is where it meets
     * maintenance * units * P. Both are re-derived from what the run booked,
     * so this pins the accessor, not a hard-coded price. */
    CHECK(state.position_units == PROBE_UNITS, "the probe did not carry its units");
    expected = (PROBE_UNITS * state.position_avg - PROBE_CAPITAL)
             / (PROBE_UNITS * (1.0 - PROBE_MAINTENANCE));
    CHECK(state.liquidation_price > 0.0, "a carried long answered no liquidation price");
    CHECK(fabs(state.liquidation_price - expected) < 1e-9,
          "the liquidation price is not the maintenance solve of what was booked");

    expected = PROBE_CAPITAL + PROBE_UNITS * (state.marked_equity_mark - state.position_avg);
    CHECK(fabs(state.marked_equity_probe - expected) < 1e-9,
          "marked equity is not the capital plus the open profit at the mark");

    /* One fill was budgeted and one was taken, so the ledger is at its cap
     * and openings are blocked for the rest of the risk day. */
    CHECK_EQ_INT((int)state.risk_fills_today, 1, "the risk ledger miscounted the fills");
    CHECK_EQ_INT((int)state.risk_blocked, 1, "the fills-per-day cap did not block");

    /* BarCloseAndFills adds one calculation at the applied cursor. */
    CHECK_EQ_INT(strategy_native_recalculations_v1(state.host, &driven, &skipped),
                 PF_NATIVE_OK, "live recalculation counters refused");
    CHECK(driven >= 1u, "BarCloseAndFills drove no recalculation");
    strategy_native_host_free(state.host);
}

int pf_native_c_api_checks(void) {
    failures = 0;
    check_struct_and_tag_refusals();
    check_spec_extension();
    check_lifecycle_round_trips();
    check_callback_failure_latch();
    check_event_polling();
    check_risk_event();
    check_absent_accessors();
    check_live_accessors();
    return failures;
}
