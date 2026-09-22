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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ── The pyramid arm (N18): open lots through the C API ─────────── */

typedef struct pyramid_state {
    pf_strategy_t   host;
    int             calculations;
    int             command_error;  /* first non-zero status a command returned. */
    int             failures;       /* refusal rows checked inside the callback. */
    pf_twin_result* out;
} pyramid_state;

static void copy_text(char* dst, const char* src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= PF_TWIN_TEXT) n = PF_TWIN_TEXT - 1;
    memset(dst, 0, PF_TWIN_TEXT);
    if (n) memcpy(dst, src, n);
}

static void observe_lots(pyramid_state* state, double mark) {
    pf_twin_result* out = state->out;
    pf_twin_observation* obs;
    int count, i;
    if (out->observation_count >= PF_TWIN_OBSERVATIONS) return;
    obs = &out->observations[out->observation_count++];
    memset(obs, 0, sizeof(*obs));
    obs->calculation = state->calculations;
    count = strategy_native_open_lot_count_v1(state->host, mark);
    obs->count = count;
    for (i = 0; i < count && i < PF_TWIN_MAX_LOTS; ++i) {
        pf_native_open_lot_v1 row;
        pf_twin_lot* lot = &obs->lots[i];
        memset(&row, 0, sizeof(row));
        row.struct_size = (uint32_t)sizeof(row);
        if (strategy_native_open_lot_get_v1(state->host, i, &row) != PF_NATIVE_OK) {
            if (state->command_error == 0) state->command_error = -2000 - i;
            continue;
        }
        lot->ordinal = row.ordinal;
        lot->entry_incarnation = row.entry_incarnation;
        lot->cycle = row.cycle;
        lot->side = row.side;
        lot->entry_bar_index = row.entry_bar_index;
        lot->entry_time_ms = row.entry_time_ms;
        lot->entry_price = row.entry_price;
        lot->signed_units = row.signed_units;
        lot->entry_commission = row.entry_commission;
        lot->mark = row.mark;
        lot->unrealized_pnl = row.unrealized_pnl;
        lot->favorable_excursion = row.favorable_excursion;
        lot->adverse_excursion = row.adverse_excursion;
        copy_text(lot->entry_label, row.entry_label);
        copy_text(lot->entry_comment, row.entry_comment);
        /* The row is its own version and its own size. */
        if (row.struct_size != sizeof(row) || row.version != PF_NATIVE_API_VERSION) {
            if (state->command_error == 0) state->command_error = -3000 - i;
        }
    }
}

/* Inside a callback there is no global CHECK context; see LCHECK below. */
#define PCHECK(state, cond, msg)                                               \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
            ++(state)->failures;                                               \
        }                                                                      \
    } while (0)

static int pyramid_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    pyramid_state* state = (pyramid_state*)user;
    pf_native_request_v1 request;
    int rc = PF_NATIVE_OK;
    (void)at;
    ++state->calculations;

    /* Observations first: the book the previous command left. */
    if (state->calculations == PF_TWIN_PYRAMID_OBSERVE_A
        || state->calculations == PF_TWIN_PYRAMID_OBSERVE_B
        || state->calculations == PF_TWIN_PYRAMID_OBSERVE_C
        || state->calculations == PF_TWIN_PYRAMID_OBSERVE_D) {
        observe_lots(state, bar->close);
    }
    if (state->calculations == PF_TWIN_PYRAMID_OBSERVE_A) {
        /* The documented refusals, and a NaN mark: booking facts kept, the
         * marked P&L NaN, the label still borrowed from the new snapshot. */
        pf_native_open_lot_v1 row;
        const double nan = strtod("nan", NULL);
        int n;
        memset(&row, 0, sizeof(row));
        row.struct_size = (uint32_t)sizeof(row);
        PCHECK(state, strategy_native_open_lot_get_v1(state->host, 3, &row) == PF_NATIVE_E_ARGUMENT,
               "an out-of-range open-lot index was not refused");
        PCHECK(state, strategy_native_open_lot_get_v1(state->host, -1, &row) == PF_NATIVE_E_ARGUMENT,
               "a negative open-lot index was not refused");
        PCHECK(state, strategy_native_open_lot_get_v1(state->host, 0, NULL) == PF_NATIVE_E_ARGUMENT,
               "a NULL open-lot row was not refused");
        row.struct_size = (uint32_t)sizeof(row) + 8u;
        PCHECK(state, strategy_native_open_lot_get_v1(state->host, 0, &row) == PF_NATIVE_E_STRUCT,
               "a mis-sized open-lot row was not refused");
        n = strategy_native_open_lot_count_v1(state->host, nan);
        PCHECK(state, n == 3, "a NaN mark changed the row count");
        memset(&row, 0, sizeof(row));
        row.struct_size = (uint32_t)sizeof(row);
        if (strategy_native_open_lot_get_v1(state->host, 0, &row) == PF_NATIVE_OK) {
            PCHECK(state, row.unrealized_pnl != row.unrealized_pnl, "a NaN mark did not leave the P&L NaN");
            PCHECK(state, row.mark != row.mark, "a NaN mark was not reported as NaN");
            PCHECK(state, row.entry_label != NULL && strcmp(row.entry_label, "L1") == 0,
                   "a NaN mark lost the entry label");
            PCHECK(state, row.signed_units == 1.0, "a NaN mark lost the units");
        } else {
            PCHECK(state, 0, "open_lot_get refused row 0 after a NaN-mark snapshot");
        }
        /* Restore the marked snapshot the record compares. */
        PCHECK(state, strategy_native_open_lot_count_v1(state->host, bar->close) == 3,
               "the re-marked snapshot has another row count");
    }

    request = blank_request();
    switch (state->calculations) {
    case PF_TWIN_PYRAMID_L1:
        request.intent = PF_NATIVE_INTENT_TRANSACT; request.intent_value = 1.0;
        request.label = "L1"; request.comment = "first";
        rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
        break;
    case PF_TWIN_PYRAMID_L2:
        request.intent = PF_NATIVE_INTENT_TRANSACT; request.intent_value = 2.0;
        request.label = "L2"; request.comment = "second";
        rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
        break;
    case PF_TWIN_PYRAMID_L3:
        request.intent = PF_NATIVE_INTENT_TRANSACT; request.intent_value = 1.0;
        request.label = "L3"; request.comment = "third";
        rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
        break;
    case PF_TWIN_PYRAMID_PARTIAL:
        request.intent = PF_NATIVE_INTENT_REDUCE;
        request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
        request.intent_value = 1.5;
        request.label = "partial";
        rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
        break;
    case PF_TWIN_PYRAMID_REVERSE:
        request.intent = PF_NATIVE_INTENT_REVERSE_TO; request.intent_value = -1.0;
        request.label = "REV"; request.comment = "flip";
        rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
        break;
    case PF_TWIN_PYRAMID_FLAT:
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "flat";
        rc = strategy_native_submit_v1(state->host, &request, NULL, NULL);
        break;
    default:
        break;
    }
    if (rc != PF_NATIVE_OK && state->command_error == 0) state->command_error = rc;
    return 0;
}

int pf_twin_run_c_pyramid(pf_twin_result* out) {
    pyramid_state state;
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
    state.out = out;
    spec.fee_kind = 2;   /* CashPerExecution */
    spec.fee_value = PF_TWIN_PYRAMID_FEE;

    table = blank_callbacks(&state);
    table.on_bar = pyramid_on_bar;

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
        out->trade_commission[i] = report.trades[i].commission;
        out->trade_max_runup[i] = report.trades[i].max_runup;
        out->trade_max_drawdown[i] = report.trades[i].max_drawdown;
    }
    strategy_native_report_free_v1(&report);

    strategy_native_position_v1(state.host, &out->signed_units, &out->average_price,
                                &out->lots);
    /* After the run the book is flat: the snapshot is empty from outside a
     * callback too, and a foreign handle is refused. */
    if (strategy_native_open_lot_count_v1(state.host, 100.0) != 0 && state.command_error == 0) {
        state.command_error = -4000;
    }
    if (strategy_native_open_lot_count_v1(NULL, 100.0) != PF_NATIVE_E_HANDLE
        && state.command_error == 0) {
        state.command_error = -4001;
    }
    collect_events(state.host, out);
    strategy_native_host_free(state.host);
    if (state.failures != 0 && state.command_error == 0) state.command_error = -5000 - state.failures;
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
    uint32_t replace_reject = 0xffffffffu;
    uint64_t replace_successor = 0xffffffffu;
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

    /* A REJECTED replace. strategy_native_replace_v1 answers
     * PF_NATIVE_E_REJECTED and stops there: its signature has nowhere to put
     * the RequestRejectReason that both the C++ replace and the C SUBMIT
     * surface, so a C host learns "rejected" and nothing else.
     * strategy_native_replace_ext_v1 is that same call with submit's own
     * `reject` out-parameter (R5 gap lane E18, lane E12 finding 6). */
    request.intent = PF_NATIVE_INTENT_REDUCE;
    request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
    request.intent_value = 0.0;
    request.label = "rejected-quantity";
    LCHECK(state, strategy_native_replace_v1(state->host, state->successor, &request, NULL)
                  == PF_NATIVE_E_REJECTED,
           "a reduce of zero units was not a rejected replace");
    replace_reject = 0xffffffffu;
    replace_successor = 0xffffffffu;
    LCHECK(state, strategy_native_replace_ext_v1(state->host, state->successor, &request,
                                                 &replace_successor, &replace_reject)
                  == PF_NATIVE_E_REJECTED,
           "the extended replace did not report the rejection");
    /* InvalidQuantity is 0, so the sentinel is what proves it was WRITTEN. */
    LCHECK(state, replace_reject == 0u, "the rejected replace named another reason");
    LCHECK(state, replace_successor == 0xffffffffu,
           "a rejected replace wrote a successor handle");

    /* A second reason, so the value is the reason and not a zeroing: a limit
     * below zero is InvalidTrigger. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_LIMIT;
    request.p1 = -1.0;
    request.label = "rejected-trigger";
    replace_reject = 0xffffffffu;
    LCHECK(state, strategy_native_replace_ext_v1(state->host, state->successor, &request,
                                                 NULL, &replace_reject)
                  == PF_NATIVE_E_REJECTED,
           "a negative limit was not a rejected replace");
    LCHECK(state, replace_reject == 2u, "the rejected replace named another reason");

    /* Neither refusal touched the book: the named order is still working and
     * is still the successor the accepted replace issued. */
    LCHECK(state, strategy_native_working_len_v1(state->host) == 1,
           "a rejected replace changed the working book");
    memset(&working, 0, sizeof(working));
    working.struct_size = (uint32_t)sizeof(working);
    LCHECK(state, strategy_native_working_get_v1(state->host, 0, &working) == PF_NATIVE_OK
                  && working.incarnation == state->successor,
           "a rejected replace retired the request it named");

    /* The extended spelling answers every other verdict exactly as the v1 one
     * does, writes the successor on success, and leaves `reject` untouched. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_LIMIT;
    request.p1 = 3.0;
    request.label = "replaced-ext";
    replace_reject = 0xffffffffu;
    replace_successor = 0;
    LCHECK(state, strategy_native_replace_ext_v1(state->host, state->successor, &request,
                                                 &replace_successor, &replace_reject)
                  == PF_NATIVE_OK,
           "the extended replace refused a good amendment");
    LCHECK(state, replace_reject == 0xffffffffu,
           "an accepted replace wrote a rejection reason");
    LCHECK(state, replace_successor != 0u && replace_successor != state->successor,
           "the extended replace did not issue a successor");
    state->successor = replace_successor;
    LCHECK(state, strategy_native_replace_ext_v1(state->host, state->resting, &request,
                                                 NULL, NULL)
                  == PF_NATIVE_E_NOT_WORKING,
           "the extended replace admitted a retired incarnation");
    LCHECK(state, strategy_native_replace_ext_v1(state->host, 0u, &request, NULL, NULL)
                  == PF_NATIVE_E_INVALID_TARGET,
           "the extended replace admitted a handle this run never issued");
    LCHECK(state, strategy_native_replace_ext_v1(NULL, state->successor, &request, NULL, NULL)
                  == PF_NATIVE_E_HANDLE,
           "the extended replace accepted a NULL host");
    LCHECK(state, strategy_native_replace_ext_v1(state->host, state->successor, NULL, NULL,
                                                 NULL)
                  == PF_NATIVE_E_ARGUMENT,
           "the extended replace accepted a NULL request");

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

/* ── cancel_where and the begin-time subscription declaration ───── */

typedef struct command_state {
    pf_strategy_t host;
    int           calculations;
    int           failures;
    int           cancelled_fade;
    int           cancelled_unmatched;
    int           cancelled_blank;
    int           working_after_fade;
    int           working_after_blank;
    int           declared_in_begin;
    int           declared_refused_in_bar;
    int           declared_refused_finer;
    int           series_after_declaration;
} command_state;

static int resting_limit(command_state* state, const char* label, const char* comment,
                         double price) {
    pf_native_request_v1 request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_LIMIT;
    request.p1 = price;
    request.label = label;
    request.comment = comment;
    return strategy_native_submit_v1(state->host, &request, NULL, NULL);
}

static int command_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    command_state* state = (command_state*)user;
    pf_native_subscription_v1 row;
    (void)at;
    ++state->calculations;

    if (state->calculations == 2) {
        /* Three resting buys far below the feed: they never match, so the
         * only thing that takes them off the book is a cancellation. */
        LCHECK(state, resting_limit(state, "fade-a", "fade", bar->low - 40.0) == PF_NATIVE_OK,
               "fade-a refused");
        LCHECK(state, resting_limit(state, "fade-b", "fade", bar->low - 41.0) == PF_NATIVE_OK,
               "fade-b refused");
        LCHECK(state, resting_limit(state, "hold-a", "hold", bar->low - 42.0) == PF_NATIVE_OK,
               "hold-a refused");
        /* One with no comment at all, to pin what NULL means. */
        LCHECK(state, resting_limit(state, "bare-a", NULL, bar->low - 43.0) == PF_NATIVE_OK,
               "bare-a refused");
        LCHECK(state, strategy_native_working_len_v1(state->host) == 4,
               "the four resting requests are not all working");

        /* A comment no live request carries cancels nothing and is not an
         * error. */
        state->cancelled_unmatched = strategy_native_cancel_where_v1(
            state->host, "no-such-comment", PF_NATIVE_FIELD_COMMENT);
        /* The two that share the comment leave; the other two stay. */
        state->cancelled_fade =
            strategy_native_cancel_where_v1(state->host, "fade", PF_NATIVE_FIELD_COMMENT);
        state->working_after_fade = strategy_native_working_len_v1(state->host);
        /* "" is the text a comment-less request carries: it takes that one and
         * leaves "hold" alone. */
        state->cancelled_blank =
            strategy_native_cancel_where_v1(state->host, "", PF_NATIVE_FIELD_COMMENT);
        state->working_after_blank = strategy_native_working_len_v1(state->host);
        return 0;
    }
    if (state->calculations == 3) {
        /* Legal only inside on_run_begin. */
        memset(&row, 0, sizeof(row));
        row.struct_size = (uint32_t)sizeof(row);
        row.tf = "15";
        state->declared_refused_in_bar =
            strategy_native_declare_subscriptions_v1(state->host, &row, 1);
        return 0;
    }
    if (state->calculations == 20) {
        pf_bar_t series;
        memset(&series, 0, sizeof(series));
        state->series_after_declaration =
            (strategy_native_series_bar_v1(state->host, 0u, &series) == PF_NATIVE_OK
             && series.high >= series.low) ? 1 : 0;
    }
    return 0;
}

static int command_on_run_begin(void* user) {
    command_state* state = (command_state*)user;
    pf_native_subscription_v1 row;

    /* A series strictly finer than the input is a different contract: the
     * same validation configure applies, so the kernel refuses and stages
     * nothing. */
    memset(&row, 0, sizeof(row));
    row.struct_size = (uint32_t)sizeof(row);
    row.tf = "1";
    state->declared_refused_finer =
        strategy_native_declare_subscriptions_v1(state->host, &row, 1);

    memset(&row, 0, sizeof(row));
    row.struct_size = (uint32_t)sizeof(row);
    row.tf = "15";
    state->declared_in_begin = strategy_native_declare_subscriptions_v1(state->host, &row, 1);
    return 0;
}

static void check_command_spellings(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    pf_native_subscription_v1 row;
    command_state state;
    const pf_bar_t* bars;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = command_on_bar;
    table.on_run_begin = command_on_run_begin;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "command host create failed");
    if (!state.host) return;

    CHECK_EQ_INT(strategy_native_cancel_where_v1(NULL, "x", PF_NATIVE_FIELD_COMMENT),
                 PF_NATIVE_E_HANDLE, "cancel_where accepted a NULL handle");
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(NULL, NULL, 0), PF_NATIVE_E_HANDLE,
                 "declare_subscriptions accepted a NULL handle");
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, NULL, 2),
                 PF_NATIVE_E_ARGUMENT, "declare_subscriptions accepted a NULL row array");
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, NULL, -1),
                 PF_NATIVE_E_ARGUMENT, "declare_subscriptions accepted a negative count");
    memset(&row, 0, sizeof(row));
    row.struct_size = (uint32_t)sizeof(row) - 4u;
    row.tf = "15";
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, &row, 1),
                 PF_NATIVE_E_STRUCT, "declare_subscriptions accepted a mis-sized row");
    memset(&row, 0, sizeof(row));
    row.struct_size = (uint32_t)sizeof(row);
    row.tf = "15";
    row.lookahead = 2u;
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, &row, 1),
                 PF_NATIVE_E_TAG, "declare_subscriptions accepted an unknown lookahead");

    /* Outside on_run_begin the kernel refuses; before a run there is no
     * callback at all. */
    memset(&row, 0, sizeof(row));
    row.struct_size = (uint32_t)sizeof(row);
    row.tf = "15";
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, &row, 1),
                 PF_NATIVE_E_STATE, "declare_subscriptions was legal before the run");

    CHECK_EQ_INT(strategy_configure_native_v1(state.host, &spec), 0, "command configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the command run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback command rows failed");

    CHECK_EQ_INT(state.cancelled_unmatched, 0, "cancel_where cancelled an unmatched comment");
    CHECK_EQ_INT(state.cancelled_fade, 2, "cancel_where did not cancel both fades");
    CHECK_EQ_INT(state.working_after_fade, 2, "cancel_where took the wrong requests");
    CHECK_EQ_INT(state.cancelled_blank, 1,
                 "the empty comment did not cancel exactly the comment-less request");
    CHECK_EQ_INT(state.working_after_blank, 1, "the empty comment cancelled too much");

    /* The spec staged no subscription; the run's series came from the
     * begin-time declaration alone. */
    CHECK_EQ_INT(state.declared_in_begin, PF_NATIVE_OK,
                 "the begin-time subscription declaration was refused");
    CHECK_EQ_INT(state.declared_refused_finer, PF_NATIVE_E_STATE,
                 "a finer-than-input series was staged");
    CHECK_EQ_INT(state.declared_refused_in_bar, PF_NATIVE_E_STATE,
                 "declare_subscriptions was legal outside on_run_begin");
    CHECK(state.series_after_declaration,
          "the declared series delivered no bucket");
    strategy_native_host_free(state.host);
}

/* ── The additive hook tail ──────────────────────────────────────
 *
 * Six slots past PF_NATIVE_CALLBACKS_V1_BASE_SIZE: two observation hooks and
 * the four answering hooks, whose return value selects whose answer the
 * kernel uses rather than reporting success. */

typedef struct hook_state {
    pf_strategy_t host;
    int      calculations;
    int      failures;
    int      bar_calls;
    int      recalc_bar_close;
    int      recalc_order_fill;
    int      recalc_cause_null_at_close;
    double   recalc_cause_price;
    double   applied_price;
    int      check_bar_open;
    int      check_after_applied;
    int      check_calculation;
    int      requirement_bar_open;
    int      requirement_after_applied;
    int      requirement_consistent;
    int      forced;
    int      inspected;
    int      liquidation_origin_rows;
    double   liquidation_units;
    int      call_units_calls;
    double   call_units_view_position;
    int      call_units_view_facts;
    int      excursion_calls;
    int      excursion_facts_ok;
} hook_state;

/* --- the recalculation hook --- */

static int hook_on_bar_never(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    hook_state* state = (hook_state*)user;
    (void)bar;
    (void)at;
    ++state->bar_calls;
    return 0;
}

static int hook_on_applied_price(void* user, const pf_native_applied_v1* applied,
                                 const pf_native_decision_v1* at) {
    hook_state* state = (hook_state*)user;
    (void)at;
    if (state->applied_price == 0.0) state->applied_price = applied->resolved_price;
    return 0;
}

static int hook_on_recalculate(void* user, const pf_bar_t* bar,
                               const pf_native_decision_v1* at, uint32_t reason,
                               const pf_native_applied_v1* cause) {
    hook_state* state = (hook_state*)user;
    pf_native_request_v1 request;
    (void)bar;
    (void)at;
    if (reason == PF_NATIVE_CALC_BAR_CLOSE) {
        ++state->recalc_bar_close;
        if (cause == NULL) ++state->recalc_cause_null_at_close;
        ++state->calculations;
        if (state->calculations == 4) {
            request = blank_request();
            request.intent = PF_NATIVE_INTENT_TRANSACT;
            request.intent_value = 1.0;
            request.label = "recalc-entry";
            LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                              == PF_NATIVE_OK, "the recalculation entry was refused");
        }
    } else if (reason == PF_NATIVE_CALC_ORDER_FILL) {
        ++state->recalc_order_fill;
        if (cause != NULL && state->recalc_cause_price == 0.0) {
            state->recalc_cause_price = cause->resolved_price;
        }
    }
    return 0;
}

static void check_recalculation_hook(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    hook_state state;
    const pf_bar_t* bars;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = hook_on_bar_never;
    table.on_recalculate = hook_on_recalculate;
    table.on_applied = hook_on_applied_price;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "recalculation host create failed");
    if (!state.host) return;

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_CALCULATION;
    ext.calculation = 1u;   /* BarCloseAndFills */
    ext.max_recalculations_per_point = 4u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the recalculation extension was refused");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the recalculation run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback recalculation rows failed");

    /* Installing on_recalculate REPLACES on_bar, exactly as overriding
     * on_native_recalculate replaces the C++ default forwarding. */
    CHECK_EQ_INT(state.bar_calls, 0, "on_bar still fired beside on_recalculate");
    CHECK_EQ_INT(state.recalc_bar_close, n,
                 "the close calculation did not reach on_recalculate once per bar");
    CHECK_EQ_INT(state.recalc_cause_null_at_close, n,
                 "a BAR_CLOSE recalculation carried a cause");
    CHECK(state.recalc_order_fill >= 1, "BarCloseAndFills drove no ORDER_FILL recalculation");
    CHECK(state.applied_price > 0.0, "the entry never applied");
    CHECK(state.recalc_cause_price == state.applied_price,
          "the ORDER_FILL cause is not the applied execution it is about");
    strategy_native_host_free(state.host);
}

/* --- the three margin hooks --- */

static int margin_on_check(void* user, const pf_native_margin_view_v1* at, int32_t* allowed) {
    hook_state* state = (hook_state*)user;
    if (at->kind == PF_NATIVE_MARGIN_CHECK_BAR_OPEN) ++state->check_bar_open;
    if (at->kind == PF_NATIVE_MARGIN_CHECK_CALCULATION) ++state->check_calculation;
    if (at->kind == PF_NATIVE_MARGIN_CHECK_AFTER_APPLIED) {
        ++state->check_after_applied;
        /* A broker model that does not check mid-path suppresses that point;
         * the kernel then evaluates nothing there. */
        *allowed = 0;
        return PF_NATIVE_ANSWER_PROVIDED;
    }
    return PF_NATIVE_ANSWER_DEFAULT;
}

static int margin_on_requirement(void* user, const pf_native_margin_view_v1* view,
                                 pf_native_margin_decision_v1* out) {
    hook_state* state = (hook_state*)user;
    if (view->kind == PF_NATIVE_MARGIN_CHECK_AFTER_APPLIED) ++state->requirement_after_applied;
    if (view->kind != PF_NATIVE_MARGIN_CHECK_BAR_OPEN) return PF_NATIVE_ANSWER_DEFAULT;
    ++state->requirement_bar_open;
    /* The kernel's own two numbers: the requirement is the maintenance
     * fraction of the whole position at the mark it is about to measure. */
    if (fabs(view->required - PROBE_MAINTENANCE * fabs(view->signed_units) * view->mark)
        < 1e-9) {
        ++state->requirement_consistent;
    }
    if (state->requirement_bar_open == 4 && !state->forced) {
        /* A broker whose money rule calls here even though this kernel would
         * not: answer numbers that do not breach, and force it anyway. */
        out->required = view->equity + 1.0;
        out->equity = view->equity;
        out->force_breach = 1u;
        state->forced = 1;
        return PF_NATIVE_ANSWER_PROVIDED;
    }
    return PF_NATIVE_ANSWER_DEFAULT;
}

static int margin_on_call_units(void* user, const pf_native_margin_view_v1* view,
                                double* units) {
    hook_state* state = (hook_state*)user;
    ++state->call_units_calls;
    state->call_units_view_position = view->signed_units;
    if (view->mark > 0.0 && view->equity != 0.0 && view->required > 0.0
        && view->kind == 0u && view->liquidation_resting == 0u) {
        state->call_units_view_facts = 1;
    }
    /* Half the book, against a spec whose sizing policy says Flatten. */
    *units = 1.0;
    return PF_NATIVE_ANSWER_PROVIDED;
}

static int margin_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    hook_state* state = (hook_state*)user;
    pf_native_request_v1 request;
    pf_native_working_v1 working;
    int len = 0;
    int i;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations == PROBE_ENTRY_BAR) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = PROBE_UNITS;
        request.label = "margin-entry";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                          == PF_NATIVE_OK, "the margin entry was refused");
        return 0;
    }
    if (!state->forced || state->inspected) return 0;
    /* The forced breach rested a kernel-originated reduction at the solved
     * level; the feed only rises, so it is still on the book right here. */
    state->inspected = 1;
    len = strategy_native_working_len_v1(state->host);
    for (i = 0; i < len; ++i) {
        memset(&working, 0, sizeof(working));
        working.struct_size = (uint32_t)sizeof(working);
        if (strategy_native_working_get_v1(state->host, i, &working) != PF_NATIVE_OK) continue;
        if (working.origin != 1u) continue;
        ++state->liquidation_origin_rows;
        state->liquidation_units = working.intent_value;
    }
    return 0;
}

static void check_margin_hooks(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    hook_state state;
    const pf_bar_t* bars;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = margin_on_bar;
    table.on_margin_check = margin_on_check;
    table.on_margin_requirement = margin_on_requirement;
    table.on_margin_call_units = margin_on_call_units;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "margin hook host create failed");
    if (!state.host) return;

    spec.initial_capital = PROBE_CAPITAL;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_MARGIN;
    ext.margin_has_maintenance_long = 1u;
    ext.margin_maintenance_long = PROBE_MAINTENANCE;
    ext.margin_has_maintenance_short = 1u;
    ext.margin_maintenance_short = PROBE_MAINTENANCE;
    ext.margin_sizing = 2u;   /* Flatten — the policy the units hook overrules */
    ext.margin_shortfall_multiple = 1.0;
    ext.margin_check = 0u;    /* PathAdverseExtreme */
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the margin hook extension was refused");

    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the margin hook run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback margin hook rows failed");

    CHECK(state.check_bar_open > 0, "no bar-open check point was offered");
    CHECK(state.check_after_applied > 0, "no after-applied check point was offered");
    CHECK(state.requirement_bar_open > 0, "the requirement hook was never consulted");
    CHECK_EQ_INT(state.requirement_after_applied, 0,
                 "a suppressed check point was still evaluated");
    CHECK_EQ_INT(state.requirement_consistent, state.requirement_bar_open,
                 "the kernel's own requirement is not maintenance * units * mark");
    CHECK(state.forced, "the requirement hook never forced a breach");
    CHECK_EQ_INT(state.call_units_calls, 1, "the units hook was not consulted exactly once");
    CHECK(state.call_units_view_facts, "the call view carried the wrong facts");
    CHECK(state.call_units_view_position == PROBE_UNITS,
          "the call view named another position");
    CHECK_EQ_INT(state.liquidation_origin_rows, 1,
                 "the forced breach rested no kernel liquidation");
    CHECK(state.liquidation_units == 1.0,
          "the units hook did not overrule the spec's Flatten sizing");
    strategy_native_host_free(state.host);
}

/* --- the excursion hook --- */

#define HOOK_FAVORABLE 7.5
#define HOOK_ADVERSE   2.5

static int excursion_on_lot(void* user, const pf_native_lot_excursion_v1* facts,
                            double* favorable, double* adverse) {
    hook_state* state = (hook_state*)user;
    ++state->excursion_calls;
    if (facts->struct_size == (uint32_t)sizeof(*facts)
        && facts->version == PF_NATIVE_API_VERSION
        && facts->is_long == 1u && facts->lot_qty == 1.0 && facts->closed_qty == 1.0
        && facts->entry_price > 0.0 && facts->fill_price > 0.0
        && facts->entry_incarnation != 0u && facts->entry_bar_index >= 0
        && facts->exit_bar_index > facts->entry_bar_index) {
        state->excursion_facts_ok = 1;
    }
    if (state->forced) return PF_NATIVE_ANSWER_DEFAULT;
    *favorable = HOOK_FAVORABLE;
    *adverse = HOOK_ADVERSE;
    return PF_NATIVE_ANSWER_PROVIDED;
}

static int excursion_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    hook_state* state = (hook_state*)user;
    pf_native_request_v1 request;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations == PROBE_ENTRY_BAR) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.label = "excursion-entry";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                          == PF_NATIVE_OK, "the excursion entry was refused");
    } else if (state->calculations == PROBE_READ_BAR) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "excursion-exit";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                          == PF_NATIVE_OK, "the excursion exit was refused");
    }
    return 0;
}

static void run_excursion(int decline, hook_state* state, pf_report_t* report) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    const pf_bar_t* bars;
    int n = 0;

    memset(state, 0, sizeof(*state));
    state->forced = decline;   /* reused as "answer DEFAULT" */
    table = blank_callbacks(state);
    table.on_bar = excursion_on_bar;
    table.on_lot_excursion = excursion_on_lot;
    state->host = strategy_native_host_create_v1(&table);
    CHECK(state->host != NULL, "excursion host create failed");
    if (!state->host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(state->host, &spec), 0, "excursion configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state->host, bars, n, report), PF_NATIVE_OK,
                 "the excursion run did not complete");
    CHECK_EQ_INT(state->failures, 0, "in-callback excursion rows failed");
}

static void check_excursion_hook(void) {
    hook_state state;
    pf_report_t report;
    int answered_runup = 0;
    int answered_drawdown = 0;
    int declined_zero = 0;
    int i;

    /* Installing the hook IS owns_lot_excursions(): the closing row takes
     * both magnitudes from it. */
    memset(&report, 0, sizeof(report));
    run_excursion(0, &state, &report);
    if (!state.host) return;
    /* The kernel consults the hook per closing evaluation, not per closed
     * row, so the count is a floor: what is pinned is that it was consulted
     * and that the row kept the answer. */
    CHECK(state.excursion_calls >= 1, "the excursion hook was never consulted");
    CHECK(state.excursion_facts_ok, "the excursion facts did not describe the closing lot");
    CHECK(report.trades_len == 1, "the excursion run booked another number of trades");
    for (i = 0; i < report.trades_len; ++i) {
        if (report.trades[i].max_runup == HOOK_FAVORABLE) ++answered_runup;
        if (report.trades[i].max_drawdown == HOOK_ADVERSE) ++answered_drawdown;
    }
    CHECK_EQ_INT(answered_runup, report.trades_len, "the closing row kept another run-up");
    CHECK_EQ_INT(answered_drawdown, report.trades_len,
                 "the closing row kept another drawdown");
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(state.host);

    /* Ownership is declared for the whole run, so a lot the hook declines
     * gets zero magnitudes: the consumer has stopped sampling. On this rising
     * feed an unowned run would have booked a nonzero run-up. */
    memset(&report, 0, sizeof(report));
    run_excursion(1, &state, &report);
    if (!state.host) return;
    CHECK(report.trades_len == 1, "the declining excursion run booked another trade count");
    for (i = 0; i < report.trades_len; ++i) {
        if (report.trades[i].max_runup == 0.0 && report.trades[i].max_drawdown == 0.0) {
            ++declined_zero;
        }
    }
    CHECK_EQ_INT(declined_zero, report.trades_len,
                 "a declined lot did not take the kernel's zero magnitudes");
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(state.host);
}

/* --- the entry-bar mask, declared from C ---
 *
 * strategy_native_declare_opened_lot_entry_bar_mask_v1 is the C spelling of
 * BacktestEngine::declare_opened_lot_entry_bar_mask: the owner of a lot's
 * excursion says where the lot's opening fill sat on its entry bar, and the
 * kernel derives which end of that bar the path had already reached. The
 * numbers are those of the C++ witness of the same seam,
 * tests/test_e6_entry_bar_mask_declaration.cpp: two units entered at the
 * close of a high-first bar (O 100 H 101 L 90 C 95: the path is O->H->L->C
 * and the fill at 95 is reached after the high, before the low) and at the
 * close of a low-first one (O 100 H 110 L 99 C 105: O->L->H->C), here as the
 * two lots of ONE run, and an owner whose magnitudes skip the masked end of
 * each lot's entry bar. Every row of that witness is one lot below, and so
 * are its forced-order rows (R5 lane E15): a run that declares
 * pf_native_run_spec_ext_v1::path_order walks both bars in that order, and
 * the masks are derived from that same walk. */

#define MASK_BARS    6
#define MASK_QTY     2.0
#define MASK_NOTHING (-1)
/* 1-based calculations: lot A enters at the close of bar 1, lot B at the
 * close of bar 2, and both are flattened at the close of bar 4. */
#define MASK_ENTRY_A 2
#define MASK_ENTRY_B 3
#define MASK_EXIT    5

static pf_bar_t mask_bars[MASK_BARS];

static void mask_fill(void) {
    static const double ohlc[MASK_BARS][4] = {
        {100.0, 100.0, 100.0, 100.0},
        {100.0, 101.0, 90.0, 95.0},   /* high-first: |H - O| = 1 < |O - L| = 10 */
        {100.0, 110.0, 99.0, 105.0},  /* low-first: |H - O| = 10, |O - L| = 1 */
        {105.0, 105.0, 105.0, 105.0},
        {105.0, 105.0, 105.0, 105.0},
        {105.0, 105.0, 105.0, 105.0},
    };
    int i;
    for (i = 0; i < MASK_BARS; ++i) {
        mask_bars[i].open = ohlc[i][0];
        mask_bars[i].high = ohlc[i][1];
        mask_bars[i].low = ohlc[i][2];
        mask_bars[i].close = ohlc[i][3];
        mask_bars[i].volume = 5.0;
        mask_bars[i].timestamp = (int64_t)i * 300000;
    }
}

/* What the host declares for lot A (high-first bar) and lot B (low-first
 * bar), and what each lot's closing facts and closed row must then carry. */
typedef struct mask_case {
    const char* tag;
    int         declare[2];        /* MASK_NOTHING or a pf_native_opened_lot_fill_point_e */
    int         other_incarnation; /* declare for an incarnation that opened no lot */
    int         want_high_masked[2];
    int         want_low_masked[2];
    double      want_favorable[2];
    double      want_adverse[2];
    uint32_t    path_order;        /* pf_native_path_order_e; AUTO keeps the plain spec */
} mask_case;

typedef struct mask_lot {
    uint64_t incarnation;
    double   bar_high;     /* the owner's own record of the entry bar */
    double   bar_low;
    int      facts;        /* closing facts the owner received */
    int      high_masked;
    int      low_masked;
    double   entry_price;
    double   closed_qty;
} mask_lot;

typedef struct mask_state {
    pf_strategy_t    host;
    const mask_case* spec;
    pf_bar_t         current;   /* the bar under calculation: a close fill's entry bar */
    int              calculations;
    int              opened;
    int              failures;
    int              hook_calls;
    int              late_refusals;
    mask_lot         lot[2];
} mask_state;

static int mask_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    mask_state* state = (mask_state*)user;
    pf_native_request_v1 request;
    (void)at;
    ++state->calculations;
    state->current = *bar;
    /* Commands are legal here; the declaration is legal inside on_applied
     * alone. The target is a live lot once one exists, so a refusal that
     * leaked would also move that lot's mask. */
    LCHECK(state, strategy_native_declare_opened_lot_entry_bar_mask_v1(
                      state->host, state->opened > 0 ? state->lot[0].incarnation : 1u, bar,
                      PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH) == PF_NATIVE_E_STATE,
           "a declaration from on_bar was not refused");
    if (state->calculations != MASK_ENTRY_A && state->calculations != MASK_ENTRY_B
        && state->calculations != MASK_EXIT) {
        return 0;
    }
    request = blank_request();
    if (state->calculations == MASK_EXIT) {
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "mask-exit";
    } else {
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = MASK_QTY;
        request.label = state->calculations == MASK_ENTRY_A ? "mask-a" : "mask-b";
    }
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "a mask-run command was refused");
    return 0;
}

static int mask_on_applied(void* user, const pf_native_applied_v1* applied,
                           const pf_native_decision_v1* at) {
    mask_state* state = (mask_state*)user;
    mask_lot* lot;
    int point;
    (void)at;
    if (applied->opened_lot_incarnation == 0u) return 0;   /* the flatten opens nothing */
    if (state->opened >= 2) {
        LCHECK(state, 0, "the mask run opened a third lot");
        return 0;
    }
    point = state->spec->declare[state->opened];
    lot = &state->lot[state->opened++];
    lot->incarnation = applied->opened_lot_incarnation;
    lot->bar_high = state->current.high;
    lot->bar_low = state->current.low;
    /* The documented refusals, here where the call is legal: each changes
     * nothing, which the "nothing" case's clear masks then show. */
    LCHECK(state, strategy_native_declare_opened_lot_entry_bar_mask_v1(
                      state->host, lot->incarnation, NULL,
                      PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH) == PF_NATIVE_E_ARGUMENT,
           "a NULL entry bar was not refused");
    LCHECK(state, strategy_native_declare_opened_lot_entry_bar_mask_v1(
                      state->host, lot->incarnation, &state->current, 2u) == PF_NATIVE_E_TAG,
           "a fill point outside the enumeration was not refused");
    LCHECK(state, strategy_native_declare_opened_lot_entry_bar_mask_v1(
                      NULL, lot->incarnation, &state->current,
                      PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH) == PF_NATIVE_E_HANDLE,
           "a NULL handle was not refused");
    if (point == MASK_NOTHING) return 0;
    LCHECK(state, strategy_native_declare_opened_lot_entry_bar_mask_v1(
                      state->host,
                      state->spec->other_incarnation ? lot->incarnation + 1000u
                                                     : lot->incarnation,
                      &state->current, (uint32_t)point) == PF_NATIVE_OK,
           "a declaration from on_applied was refused");
    return 0;
}

static int mask_on_lot(void* user, const pf_native_lot_excursion_v1* facts,
                       double* favorable, double* adverse) {
    mask_state* state = (mask_state*)user;
    mask_lot* lot = NULL;
    double high;
    double low;
    int i;
    ++state->hook_calls;
    /* Too late to declare here: these facts already carry the mask. */
    if (strategy_native_declare_opened_lot_entry_bar_mask_v1(
            state->host, facts->entry_incarnation, &state->current,
            PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH) == PF_NATIVE_E_STATE) {
        ++state->late_refusals;
    }
    for (i = 0; i < state->opened; ++i) {
        if (state->lot[i].incarnation == facts->entry_incarnation) lot = &state->lot[i];
    }
    if (!lot) {
        LCHECK(state, 0, "the owner was handed a lot it never opened");
        return PF_NATIVE_ANSWER_DEFAULT;
    }
    LCHECK(state, lot->facts == 0 || (lot->high_masked == facts->entry_bar_high_masked
                                      && lot->low_masked == facts->entry_bar_low_masked),
           "one lot's closing facts carried two different masks");
    ++lot->facts;
    lot->high_masked = facts->entry_bar_high_masked;
    lot->low_masked = facts->entry_bar_low_masked;
    lot->entry_price = facts->entry_price;
    lot->closed_qty = facts->closed_qty;
    /* The C++ witness's owner: a masked end of the entry bar is not the
     * lot's, so it counts from the entry price. */
    high = facts->entry_bar_high_masked ? facts->entry_price : lot->bar_high;
    low = facts->entry_bar_low_masked ? facts->entry_price : lot->bar_low;
    *favorable = (high - facts->entry_price) * facts->closed_qty;
    *adverse = (facts->entry_price - low) * facts->closed_qty;
    return PF_NATIVE_ANSWER_PROVIDED;
}

static void run_mask_case(const mask_case* mc, mask_state* state, pf_report_t* report) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;

    memset(state, 0, sizeof(*state));
    state->spec = mc;
    table = blank_callbacks(state);
    table.on_bar = mask_on_bar;
    table.on_applied = mask_on_applied;
    table.on_lot_excursion = mask_on_lot;
    state->host = strategy_native_host_create_v1(&table);
    CHECK(state->host != NULL, "mask host create failed");
    if (!state->host) return;
    spec.session_key = "native-c-api-entry-bar-mask";
    spec.close_execution = 1;   /* AfterCalculation: each entry fills at its own bar's close */
    if (mc->path_order == PF_NATIVE_PATH_ORDER_AUTO) {
        CHECK_EQ_INT(strategy_configure_native_v1(state->host, &spec), 0, "mask configure");
    } else {
        /* The run's leg order is declared once, in the feed-policy block; its
         * three other fields keep the kernel's defaults. */
        memset(&ext, 0, sizeof(ext));
        ext.struct_size = (uint32_t)sizeof(ext);
        ext.version = PF_NATIVE_API_VERSION;
        ext.present_mask = PF_NATIVE_SPEC_EXT_FEED_POLICY;
        ext.slot_label_policy = PF_NATIVE_SLOT_LABEL_CANONICAL;
        ext.feed_tolerance = PF_NATIVE_FEED_TOLERANCE_NONE;
        ext.path_order = mc->path_order;
        ext.abort_reporting = PF_NATIVE_ABORT_ERROR;
        CHECK_EQ_INT(strategy_configure_native_ext_v1(state->host, &spec, &ext), PF_NATIVE_OK,
                     "forced-order mask configure");
    }
    CHECK_EQ_INT(strategy_native_declare_opened_lot_entry_bar_mask_v1(
                     state->host, 1u, &mask_bars[1], PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH),
                 PF_NATIVE_E_STATE, "a declaration before the run was not refused");
    CHECK_EQ_INT(strategy_native_run_v1(state->host, mask_bars, MASK_BARS, report), PF_NATIVE_OK,
                 "the mask run did not complete");
    CHECK_EQ_INT(state->failures, 0, "in-callback mask rows failed");
}

static void check_entry_bar_mask_declaration(void) {
    /* Lot A: (101 - 95) and (95 - 90) per unit of its whole entry bar; lot
     * B: (110 - 105) and (105 - 99). A masked end counts zero. */
    static const mask_case cases[] = {
        {"nothing", {MASK_NOTHING, MASK_NOTHING}, 0, {0, 0}, {0, 0},
         {(101.0 - 95.0) * MASK_QTY, (110.0 - 105.0) * MASK_QTY},
         {(95.0 - 90.0) * MASK_QTY, (105.0 - 99.0) * MASK_QTY}},
        {"onpath-high-first, afterpath-low-first",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH}, 0,
         {1, 1}, {0, 1}, {0.0, 0.0}, {(95.0 - 90.0) * MASK_QTY, 0.0}},
        {"afterpath-high-first, onpath-low-first",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH}, 0,
         {1, 0}, {1, 1}, {0.0, (110.0 - 105.0) * MASK_QTY}, {0.0, 0.0}},
        {"other-incarnation",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH},
         1, {0, 0}, {0, 0},
         {(101.0 - 95.0) * MASK_QTY, (110.0 - 105.0) * MASK_QTY},
         {(95.0 - 90.0) * MASK_QTY, (105.0 - 99.0) * MASK_QTY}},
        /* Forced HIGH_FIRST: lot A's bar keeps its own answer (the high), lot
         * B's path is O->H->L->C, so its fill at 105 is touched on the O->H
         * leg before either end and masks nothing. */
        {"forced-high-first, onpath both",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH}, 0,
         {1, 0}, {0, 0}, {0.0, (110.0 - 105.0) * MASK_QTY},
         {(95.0 - 90.0) * MASK_QTY, (105.0 - 99.0) * MASK_QTY}, PF_NATIVE_PATH_ORDER_HIGH_FIRST},
        {"forced-high-first, afterpath-high-first, onpath-low-first",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH}, 0,
         {1, 0}, {1, 0}, {0.0, (110.0 - 105.0) * MASK_QTY},
         {0.0, (105.0 - 99.0) * MASK_QTY}, PF_NATIVE_PATH_ORDER_HIGH_FIRST},
        /* Forced LOW_FIRST: lot A's path is O->L->H->C, so its fill at 95 is
         * touched on the O->L leg before either end; lot B keeps its own
         * answer (the low). */
        {"forced-low-first, onpath both",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH}, 0,
         {0, 0}, {0, 1}, {(101.0 - 95.0) * MASK_QTY, (110.0 - 105.0) * MASK_QTY},
         {(95.0 - 90.0) * MASK_QTY, 0.0}, PF_NATIVE_PATH_ORDER_LOW_FIRST},
        {"forced-low-first, onpath-high-first, afterpath-low-first",
         {PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH, PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH}, 0,
         {0, 1}, {0, 1}, {(101.0 - 95.0) * MASK_QTY, 0.0},
         {(95.0 - 90.0) * MASK_QTY, 0.0}, PF_NATIVE_PATH_ORDER_LOW_FIRST},
    };
    static const double entry[2] = {95.0, 105.0};
    size_t c;

    mask_fill();
    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const mask_case* mc = &cases[c];
        const int before = failures;
        mask_state state;
        pf_report_t report;
        int i;
        int row;

        memset(&report, 0, sizeof(report));
        run_mask_case(mc, &state, &report);
        if (!state.host) continue;
        CHECK_EQ_INT(state.opened, 2, "the mask run opened another number of lots");
        CHECK_EQ_INT(report.trades_len, 2, "the mask run booked another number of rows");
        CHECK(state.hook_calls >= 2, "the owner was not handed both lots");
        CHECK_EQ_INT(state.late_refusals, state.hook_calls,
                     "a declaration from the excursion hook was not refused");
        for (i = 0; i < 2; ++i) {
            const mask_lot* lot = &state.lot[i];
            int matched = 0;
            CHECK(lot->facts >= 1, "a lot closed without its owner's facts");
            CHECK(lot->entry_price == entry[i], "a lot's facts carried another entry price");
            CHECK(lot->closed_qty == MASK_QTY, "a lot's facts closed another quantity");
            CHECK_EQ_INT(lot->high_masked, mc->want_high_masked[i],
                         "a lot's facts carried another entry-bar high mask");
            CHECK_EQ_INT(lot->low_masked, mc->want_low_masked[i],
                         "a lot's facts carried another entry-bar low mask");
            for (row = 0; row < report.trades_len; ++row) {
                if (report.trades[row].entry_price != entry[i]) continue;
                ++matched;
                CHECK_EQ_INT(report.trades[row].is_long, 1, "a mask row closed another side");
                CHECK(report.trades[row].qty == MASK_QTY, "a mask row closed another quantity");
                CHECK(fabs(report.trades[row].max_runup - mc->want_favorable[i]) < 1e-9,
                      "a mask row kept another run-up");
                CHECK(fabs(report.trades[row].max_drawdown - mc->want_adverse[i]) < 1e-9,
                      "a mask row kept another drawdown");
            }
            CHECK_EQ_INT(matched, 1, "a lot has no closed row of its own");
        }
        /* Outside every callback, after the run, it is refused as well. */
        CHECK_EQ_INT(strategy_native_declare_opened_lot_entry_bar_mask_v1(
                         state.host, state.lot[0].incarnation, &mask_bars[1],
                         PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH),
                     PF_NATIVE_E_STATE, "a declaration after the run was not refused");
        if (failures != before) fprintf(stderr, "  in the entry-bar mask case \"%s\"\n", mc->tag);
        strategy_native_report_free_v1(&report);
        strategy_native_host_free(state.host);
    }
}

static void check_callback_tail_layouts(void) {
    pf_native_callbacks_v1 table;
    pf_strategy_t host;

    /* The base layout the lane first shipped is still accepted, and its
     * six-hook tail is simply absent. */
    memset(&table, 0, sizeof(table));
    table.struct_size = PF_NATIVE_CALLBACKS_V1_BASE_SIZE;
    table.version = PF_NATIVE_API_VERSION;
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "a base-layout callback table was refused");
    strategy_native_host_free(host);

    CHECK(PF_NATIVE_CALLBACKS_V1_BASE_SIZE < (uint32_t)sizeof(pf_native_callbacks_v1),
          "the callback tail is not past the base layout");

    /* Any third length is a caller this runtime cannot read. */
    memset(&table, 0, sizeof(table));
    table.struct_size = PF_NATIVE_CALLBACKS_V1_BASE_SIZE + 4u;
    table.version = PF_NATIVE_API_VERSION;
    CHECK(strategy_native_host_create_v1(&table) == NULL,
          "a third callback-table length was accepted");
}

/* ── The two additive spec / request tails ───────────────────────
 *
 * pf_native_request_v1 gains L3b's sizing detail and pf_native_run_spec_ext_v1
 * gains the intrabar path, the four feed-shape policies and the margin
 * model's remaining knobs. Both are third published layouts. */

/* A falling feed, so a resting limit fills at a price the signal is not. */
#define FALL_BARS 24
static pf_bar_t fall_bars[FALL_BARS];
static int fall_ready = 0;

static const pf_bar_t* falling_feed(int* n) {
    if (!fall_ready) {
        int i;
        for (i = 0; i < FALL_BARS; ++i) {
            const double open = 200.0 - (double)i;
            fall_bars[i].open = open;
            fall_bars[i].high = open + 1.0;
            fall_bars[i].low = open - 2.0;
            fall_bars[i].close = open - 1.0;
            fall_bars[i].volume = 5.0;
            fall_bars[i].timestamp = (int64_t)i * 300000;
        }
        fall_ready = 1;
    }
    if (n) *n = FALL_BARS;
    return fall_bars;
}

#define SIZED_CASH        900.0
#define SIZED_SUBMIT_BAR  4
#define SIZED_SLIPPAGE    10u
#define SIZED_TICK        0.01

typedef struct tail_state {
    pf_strategy_t host;
    int      calculations;
    int      failures;
    uint32_t size_price;
    double   signal_reference;
    double   filled_units;
    int      sub_bars;
    int      sub_bar_partial_ok;
} tail_state;

static int sized_on_applied(void* user, const pf_native_applied_v1* applied,
                            const pf_native_decision_v1* at) {
    tail_state* state = (tail_state*)user;
    (void)at;
    if (state->filled_units == 0.0) state->filled_units = applied->opened_units;
    return 0;
}

static int sized_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    tail_state* state = (tail_state*)user;
    pf_native_request_v1 request;
    (void)at;
    ++state->calculations;
    if (state->calculations != SIZED_SUBMIT_BAR) return 0;
    /* The decision price the Signal rule freezes, carried to the expected
     * market fill by the side's own tick slippage. */
    state->signal_reference = bar->close + (double)SIZED_SLIPPAGE * SIZED_TICK;
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_SIZED;
    request.side = PF_NATIVE_SIDE_LONG;
    request.size_basis = PF_NATIVE_SIZE_BASIS_CASH;
    request.intent_value = SIZED_CASH;
    request.size_time = PF_NATIVE_SIZE_AT_MATCH;
    request.size_price = state->size_price;   /* the arm under test */
    request.trigger = PF_NATIVE_TRIGGER_LIMIT;
    request.p1 = bar->close - 20.0;   /* reached several bars down the fall */
    request.label = "sized-entry";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "the sized entry was refused");
    return 0;
}

static void run_sized_price(uint32_t size_price, tail_state* state) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    const pf_bar_t* bars;
    int n = 0;

    memset(state, 0, sizeof(*state));
    state->size_price = size_price;
    table = blank_callbacks(state);
    table.on_bar = sized_on_bar;
    table.on_applied = sized_on_applied;
    state->host = strategy_native_host_create_v1(&table);
    CHECK(state->host != NULL, "sized-price host create failed");
    if (!state->host) return;
    spec.initial_capital = 100000.0;
    spec.slippage_ticks = SIZED_SLIPPAGE;
    spec.price_tick = SIZED_TICK;
    CHECK_EQ_INT(strategy_configure_native_v1(state->host, &spec), 0, "sized-price configure");
    bars = falling_feed(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state->host, bars, n, NULL), PF_NATIVE_OK,
                 "the sized-price run did not complete");
    CHECK_EQ_INT(state->failures, 0, "in-callback sized-price rows failed");
}

static void check_request_sizing_tail(void) {
    pf_native_request_v1 request;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    pf_strategy_t host;
    tail_state resolved;
    tail_state signal;

    /* Four published request layouts. */
    CHECK(PF_NATIVE_REQUEST_V1_BASE_SIZE < PF_NATIVE_REQUEST_V1_ANCHOR_SIZE,
          "the anchored-leg tail is not past the base layout");
    CHECK(PF_NATIVE_REQUEST_V1_ANCHOR_SIZE < PF_NATIVE_REQUEST_V1_SIZING_SIZE,
          "the sizing tail is not past the anchored-leg layout");
    CHECK(PF_NATIVE_REQUEST_V1_SIZING_SIZE < (uint32_t)sizeof(pf_native_request_v1),
          "the trail-seed tail is not past the sizing layout");

    table = blank_callbacks(NULL);
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "sizing-tail host create failed");
    if (!host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(host, &spec), 0, "sizing-tail configure");

    /* An unknown sizing-detail tag is refused, and only from a caller whose
     * struct carries the tail. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_SIZED;
    request.intent_value = 100.0;
    request.size_price = 9u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL), PF_NATIVE_E_TAG,
                 "an unknown size_price was accepted");
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_REDUCE;
    request.reduce_size = PF_NATIVE_REDUCE_SCOPE_FRACTION;
    request.intent_value = 0.5;
    request.reduce_basis = 9u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL), PF_NATIVE_E_TAG,
                 "an unknown reduce_basis was accepted");
    /* The anchored-leg layout has no such fields, so the same bytes are not
     * even looked at: the request is refused for its own reason, never for a
     * tail its caller does not have. */
    request = blank_request();
    request.struct_size = PF_NATIVE_REQUEST_V1_ANCHOR_SIZE;
    request.intent = PF_NATIVE_INTENT_SIZED;
    request.intent_value = 100.0;
    request.size_price = 9u;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL), PF_NATIVE_E_STATE,
                 "an anchored-layout caller was judged on a tail it does not carry");
    strategy_native_host_free(host);

    /* The two sizing prices resolve different unit counts on a falling feed,
     * where the signal the request was born on is not the price it fills at. */
    run_sized_price(PF_NATIVE_SIZE_PRICE_RESOLVED, &resolved);
    run_sized_price(PF_NATIVE_SIZE_PRICE_SIGNAL, &signal);
    if (!resolved.host || !signal.host) return;
    CHECK(resolved.filled_units > 0.0, "the RESOLVED arm never filled");
    CHECK(signal.filled_units > 0.0, "the SIGNAL arm never filled");
    CHECK(resolved.filled_units != signal.filled_units,
          "size_price did not change what the basis converted at");
    CHECK(fabs(signal.filled_units - SIZED_CASH / signal.signal_reference) < 1e-9,
          "the SIGNAL arm did not size against the frozen decision price");
    strategy_native_host_free(resolved.host);
    strategy_native_host_free(signal.host);
}

/* --- the trail seed (E14): where a trail's running best starts --- */

/* Five bars. The lot opens on the first, the host's own level 101.00 is
 * reached on the second, and the trail is submitted on the third already
 * armed -- so the kernel's own start is the fourth bar's open, 100.60. With a
 * 0.50 offset that is a stop at 100.20 and the shallow fifth bar, whose low is
 * 100.45, never reaches it. Seeded at 101.00 the stop is 100.50 and the
 * shallow bar books it. */
static pf_bar_t seed_bars_storage[5];
static int seed_bars_ready = 0;

static const pf_bar_t* seed_feed(int* n) {
    if (!seed_bars_ready) {
        static const double rows[5][4] = {
            {100.00, 100.00, 100.00, 100.00},
            {100.20, 101.00, 100.10, 100.60},
            {100.60, 100.70, 100.55, 100.60},
            {100.60, 100.60, 100.45, 100.50},
            {100.50, 100.50, 100.50, 100.50},
        };
        int i;
        for (i = 0; i < 5; ++i) {
            seed_bars_storage[i].open = rows[i][0];
            seed_bars_storage[i].high = rows[i][1];
            seed_bars_storage[i].low = rows[i][2];
            seed_bars_storage[i].close = rows[i][3];
            seed_bars_storage[i].volume = 1.0;
            seed_bars_storage[i].timestamp = (int64_t)i * 300000;
        }
        seed_bars_ready = 1;
    }
    if (n) *n = 5;
    return seed_bars_storage;
}

typedef struct seed_state {
    pf_strategy_t host;
    int      calculations;
    int      failures;
    int      has_seed;        /* 0 no seed, 1 seeded, 2 seeded past an old layout */
    double   final_units;
} seed_state;

static int seed_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    seed_state* state = (seed_state*)user;
    pf_native_request_v1 request;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations == 1) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.label = "seed-entry";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                          == PF_NATIVE_OK, "the seed entry was refused");
        return 0;
    }
    if (state->calculations != 2) return 0;
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_REDUCE;
    request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_TRAIL;
    request.p1 = 0.5;
    request.label = "seed-trail";
    if (state->has_seed != 0) {
        request.trail_best_seed = 101.0;
        request.trail_has_best_seed = 1;
    }
    if (state->has_seed == 2) {
        /* A caller compiled before the seed existed sends the third layout.
         * The bytes are in this struct, but the runtime must not read a tail
         * its caller does not carry. */
        request.struct_size = PF_NATIVE_REQUEST_V1_SIZING_SIZE;
    }
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "the seed trail was refused");
    return 0;
}

static void run_seeded_trail(int has_seed, seed_state* state) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    const pf_bar_t* bars;
    int n = 0;

    memset(state, 0, sizeof(*state));
    state->has_seed = has_seed;
    table = blank_callbacks(state);
    table.on_bar = seed_on_bar;
    state->host = strategy_native_host_create_v1(&table);
    CHECK(state->host != NULL, "trail-seed host create failed");
    if (!state->host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(state->host, &spec), 0, "trail-seed configure");
    bars = seed_feed(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state->host, bars, n, NULL), PF_NATIVE_OK,
                 "the trail-seed run did not complete");
    CHECK_EQ_INT(state->failures, 0, "in-callback trail-seed rows failed");
    CHECK_EQ_INT(strategy_native_position_v1(state->host, &state->final_units, NULL, NULL),
                 PF_NATIVE_OK, "the trail-seed position was refused");
}

static void check_trail_best_seed_tail(void) {
    pf_native_request_v1 request;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    pf_strategy_t host;
    seed_state without;
    seed_state with;
    seed_state old_layout;

    /* An out-of-range flag is a tag error, and a seed that is not a price
     * level is the kernel's own rejection. */
    table = blank_callbacks(NULL);
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "trail-seed tag host create failed");
    if (!host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(host, &spec), 0, "trail-seed tag configure");
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_REDUCE;
    request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
    request.intent_value = 1.0;
    request.trigger = PF_NATIVE_TRIGGER_TRAIL;
    request.p1 = 0.5;
    request.trail_has_best_seed = 2;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL), PF_NATIVE_E_TAG,
                 "an out-of-range trail_has_best_seed was accepted");
    request.trail_has_best_seed = 1;
    request.trail_best_seed = 0.0;
    CHECK_EQ_INT(strategy_native_submit_v1(host, &request, NULL, NULL), PF_NATIVE_E_STATE,
                 "a nonpositive trail seed was judged before the command legality rule");
    strategy_native_host_free(host);

    run_seeded_trail(0, &without);
    run_seeded_trail(1, &with);
    run_seeded_trail(2, &old_layout);
    if (!without.host || !with.host || !old_layout.host) return;
    /* Without a seed the ride restarts at the next print and the shallow bar
     * never reaches the stop: the lot is still open at the end. */
    CHECK(fabs(without.final_units - 1.0) < 1e-9,
          "the unseeded trail closed the lot after all");
    CHECK(fabs(with.final_units) < 1e-9, "the seeded trail did not close the lot");
    /* The third layout is the whole append-only claim: identical bytes above
     * the tail, the tail unread, the unseeded outcome. */
    CHECK(fabs(old_layout.final_units - without.final_units) < 1e-9,
          "an older-layout caller was given a tail it does not carry");
    strategy_native_host_free(without.host);
    strategy_native_host_free(with.host);
    strategy_native_host_free(old_layout.host);
}

/* --- the frozen scope basis --- */

typedef struct scope_state {
    pf_strategy_t host;
    int      calculations;
    int      failures;
    uint32_t basis;
    uint64_t entry;
    int64_t  cycle;
    double   final_units;
} scope_state;

static int scope_on_applied(void* user, const pf_native_applied_v1* applied,
                            const pf_native_decision_v1* at) {
    scope_state* state = (scope_state*)user;
    (void)at;
    if (applied->opened_units > 0.0) state->cycle = applied->cycle_after;
    return 0;
}

static int scope_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    scope_state* state = (scope_state*)user;
    pf_native_request_v1 request;
    uint64_t owner[1];
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations == 3) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 10.0;
        request.label = "scope-entry";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->entry, NULL)
                          == PF_NATIVE_OK, "the scope entry was refused");
        return 0;
    }
    if (state->calculations != 6) return 0;
    owner[0] = state->entry;
    /* Two siblings, both half the bound scope, accepted together and matched
     * in order at the next point. */
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_REDUCE;
    request.reduce_size = PF_NATIVE_REDUCE_SCOPE_FRACTION;
    request.reduce_claim = PF_NATIVE_SCOPE_GROSS;
    request.reduce_basis = state->basis;
    request.intent_value = 0.5;
    request.owner = PF_NATIVE_OWNER_BIND_OPENING;
    request.owner_incarnations = owner;
    request.owner_n = 1u;
    request.owner_cycle = state->cycle;
    request.label = "scope-a";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "scope-a was refused");
    request.label = "scope-b";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "scope-b was refused");
    return 0;
}

static void run_scope_basis(uint32_t basis, scope_state* state) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    const pf_bar_t* bars;
    int n = 0;

    memset(state, 0, sizeof(*state));
    state->basis = basis;
    table = blank_callbacks(state);
    table.on_bar = scope_on_bar;
    table.on_applied = scope_on_applied;
    state->host = strategy_native_host_create_v1(&table);
    CHECK(state->host != NULL, "scope host create failed");
    if (!state->host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(state->host, &spec), 0, "scope configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state->host, bars, n, NULL), PF_NATIVE_OK,
                 "the scope run did not complete");
    CHECK_EQ_INT(state->failures, 0, "in-callback scope rows failed");
    CHECK_EQ_INT(strategy_native_position_v1(state->host, &state->final_units, NULL, NULL),
                 PF_NATIVE_OK, "the scope position was refused");
}

static void check_scope_basis_tail(void) {
    scope_state at_match;
    scope_state at_acceptance;

    run_scope_basis(PF_NATIVE_SCOPE_BASIS_AT_MATCH, &at_match);
    run_scope_basis(PF_NATIVE_SCOPE_BASIS_AT_ACCEPTANCE, &at_acceptance);
    if (!at_match.host || !at_acceptance.host) return;
    /* AT_MATCH reads the scope as it stands: 5 then half of what is left.
     * AT_ACCEPTANCE froze it at 10, so both siblings claim 5. */
    CHECK(fabs(at_match.final_units - 2.5) < 1e-9,
          "AT_MATCH did not re-read the scope at the second candidate");
    CHECK(fabs(at_acceptance.final_units - 0.0) < 1e-9,
          "AT_ACCEPTANCE did not freeze the scope at acceptance");
    strategy_native_host_free(at_match.host);
    strategy_native_host_free(at_acceptance.host);
}

/* --- the intrabar path, and with it the sub-bar hook --- */

#define SUB_PER_BAR 5
static pf_bar_t lower_bars[TWIN_BARS * SUB_PER_BAR];
static int lower_ready = 0;

/* Five 1m bars that aggregate EXACTLY to each 5m twin bar: same open, same
 * high, same low, same close, same total volume. */
static const pf_bar_t* lower_feed(int* n) {
    if (!lower_ready) {
        int i;
        for (i = 0; i < TWIN_BARS; ++i) {
            const double open = 100.0 + (double)i;
            const double high = open + 2.0;
            const double low = open - 1.0;
            const double close = open + 1.0;
            const double points[SUB_PER_BAR + 1] = {open, open, high, high, low, close};
            int j;
            for (j = 0; j < SUB_PER_BAR; ++j) {
                pf_bar_t* row = &lower_bars[i * SUB_PER_BAR + j];
                const double a = points[j];
                const double b = points[j + 1];
                row->open = a;
                row->close = b;
                row->high = a > b ? a : b;
                row->low = a < b ? a : b;
                row->volume = 1.0;
                row->timestamp = (int64_t)i * 300000 + (int64_t)j * 60000;
            }
        }
        lower_ready = 1;
    }
    if (n) *n = TWIN_BARS * SUB_PER_BAR;
    return lower_bars;
}

static int intrabar_on_sub_bar(void* user, const pf_bar_t* sub,
                               const pf_native_decision_v1* at) {
    tail_state* state = (tail_state*)user;
    pf_bar_t partial;
    (void)at;
    ++state->sub_bars;
    if (sub->high < sub->low) LCHECK(state, 0, "a sub-bar arrived inverted");
    /* A sub-bar is inside the script bar's path walk, so the bar so far
     * exists there too. */
    memset(&partial, 0, sizeof(partial));
    if (strategy_native_partial_bar_v1(state->host, &partial) == PF_NATIVE_OK) {
        state->sub_bar_partial_ok = 1;
    }
    return 0;
}

static int intrabar_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    tail_state* state = (tail_state*)user;
    (void)bar;
    (void)at;
    ++state->calculations;
    return 0;
}

static void check_intrabar_and_policies(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    tail_state state;
    pf_strategy_t probe;
    const pf_bar_t* bars;
    const pf_bar_t* lower;
    int n = 0;
    int lower_n = 0;

    /* Three published extension layouts. */
    CHECK(PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE < PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE,
          "the risk tail is not past the base extension layout");
    CHECK(PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE < (uint32_t)sizeof(pf_native_run_spec_ext_v1),
          "the intrabar/policy tail is not past the risk layout");

    /* The two new blocks need the tail their fields live in. */
    table = blank_callbacks(NULL);
    probe = strategy_native_host_create_v1(&table);
    CHECK(probe != NULL, "intrabar probe host create failed");
    if (!probe) return;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_INTRABAR;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(probe, &spec, &ext), PF_NATIVE_E_STRUCT,
                 "the intrabar block was accepted from a risk-layout extension");
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_FEED_POLICY;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(probe, &spec, &ext), PF_NATIVE_E_STRUCT,
                 "the feed-policy block was accepted from a base-layout extension");
    /* Unknown enumerators in either block. */
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_INTRABAR;
    ext.intrabar_kind = 7u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(probe, &spec, &ext), PF_NATIVE_E_TAG,
                 "an unknown intrabar kind was accepted");
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_FEED_POLICY;
    ext.feed_tolerance = 1u << 5;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(probe, &spec, &ext), PF_NATIVE_E_TAG,
                 "an unpublished feed-tolerance bit was accepted");
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_INTRABAR;
    ext.intrabar_kind = PF_NATIVE_INTRABAR_LOWER_TF;
    ext.intrabar_tf = NULL;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(probe, &spec, &ext), PF_NATIVE_E_ARGUMENT,
                 "a lower-timeframe path without a timeframe was accepted");
    /* None of those refusals touched the handle. */
    CHECK_EQ_INT(strategy_native_partial_bar_v1(probe, &lower_bars[0]), PF_NATIVE_ABSENT,
                 "a refused extension started a run");
    strategy_native_host_free(probe);

    /* A real retained lower feed: five 1m bars per 5m script bar, so every
     * script bar has sub-bars of its own and on_sub_bar is delivered. */
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = intrabar_on_bar;
    table.on_sub_bar = intrabar_on_sub_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "intrabar host create failed");
    if (!state.host) return;

    lower = lower_feed(&lower_n);
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_INTRABAR | PF_NATIVE_SPEC_EXT_FEED_POLICY;
    ext.intrabar_kind = PF_NATIVE_INTRABAR_LOWER_TF;
    ext.intrabar_tf = "1";
    ext.intrabar_bars = lower;
    ext.intrabar_n = lower_n;
    ext.intrabar_samples = 4;
    ext.intrabar_distribution = PF_MAGNIFIER_ENDPOINTS;
    ext.intrabar_volume_weighted = 0u;
    ext.intrabar_volume_weighted_min_samples = 2;
    ext.intrabar_volume_weighted_max_samples = 64;
    ext.intrabar_sample_eligibility = PF_NATIVE_SAMPLE_CONTINUOUS_SEGMENTS;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the intrabar block was refused");

    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the intrabar run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback intrabar rows failed");
    CHECK_EQ_INT(state.calculations, n, "the intrabar run did not calculate every bar");
    CHECK(state.sub_bars > 0, "a retained lower feed delivered no sub-bar");
    CHECK(state.sub_bar_partial_ok, "the bar so far was absent inside a sub-bar");
    strategy_native_host_free(state.host);

    /* A synthesized path retains no feed of its own, so it has no sub-bars
     * to deliver — the C spelling of the same C++ contract. */
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = intrabar_on_bar;
    table.on_sub_bar = intrabar_on_sub_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "synthesized host create failed");
    if (!state.host) return;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_INTRABAR;
    ext.intrabar_kind = PF_NATIVE_INTRABAR_SYNTHESIZED;
    ext.intrabar_samples = 4;
    ext.intrabar_distribution = PF_MAGNIFIER_ENDPOINTS;
    ext.intrabar_volume_weighted_min_samples = 2;
    ext.intrabar_volume_weighted_max_samples = 64;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the synthesized intrabar path was refused");
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the synthesized run did not complete");
    CHECK_EQ_INT(state.sub_bars, 0, "a synthesized path delivered a sub-bar");
    strategy_native_host_free(state.host);
}

/* The four feed-shape and presentation policies. They are declarations, not
 * callbacks, so what proves they were READ rather than merely accepted is the
 * run's own continuation identity: the spec digest folds them, so a run that
 * declares them cannot share an identity with one that does not. */
static uint64_t policy_run_hash(int declare) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    tail_state state;
    const pf_bar_t* bars;
    uint64_t hash = 0;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = intrabar_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "policy host create failed");
    if (!state.host) return 0;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    if (declare) {
        ext.present_mask = PF_NATIVE_SPEC_EXT_FEED_POLICY;
        ext.slot_label_policy = PF_NATIVE_SLOT_LABEL_FEED_TOLERANT;
        ext.feed_tolerance = PF_NATIVE_FEED_TOLERANCE_BATCH_STRUCTURAL
                           | PF_NATIVE_FEED_TOLERANCE_WARMUP_NONNEGATIVE;
        ext.path_order = PF_NATIVE_PATH_ORDER_LOW_FIRST;
        ext.abort_reporting = PF_NATIVE_ABORT_QUIET;
    }
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the feed-policy block was refused");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the feed-policy run did not complete");
    CHECK_EQ_INT(strategy_native_continuation_hash_v1(state.host, &hash), PF_NATIVE_OK,
                 "the feed-policy continuation hash was refused");
    strategy_native_host_free(state.host);
    return hash;
}

static void check_feed_policies(void) {
    const uint64_t plain = policy_run_hash(0);
    const uint64_t declared = policy_run_hash(1);
    CHECK(plain != 0u && declared != 0u, "a policy run reported no continuation hash");
    CHECK(plain != declared,
          "declaring the four feed-shape policies left the continuation identity unmoved");
    CHECK(policy_run_hash(1) == declared, "the declared policy run is not reproducible");
}

/* --- the margin model's remaining knobs --- */

static void check_margin_tail_fields(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    pf_strategy_t host;

    table = blank_callbacks(NULL);
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "margin-tail host create failed");
    if (!host) return;

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_MARGIN;
    ext.margin_has_maintenance_long = 1u;
    ext.margin_maintenance_long = PROBE_MAINTENANCE;
    ext.margin_has_maintenance_short = 1u;
    ext.margin_maintenance_short = PROBE_MAINTENANCE;
    ext.margin_shortfall_multiple = 1.0;
    ext.margin_sizing = 0u;
    ext.margin_equity_basis = 9u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "an unknown margin equity basis was accepted");
    ext.margin_equity_basis = PF_NATIVE_MARGIN_EQUITY_BEFORE_OPEN_COMMISSION;
    ext.margin_level_base = 9u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "an unknown margin level base was accepted");
    ext.margin_level_base = PF_NATIVE_MARGIN_LEVEL_REALIZED_ONLY;
    /* The third liquidation check had no C value at all before this lane. */
    ext.margin_check = PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK;
    ext.margin_liquidation_label = "n8-liq";
    ext.margin_liquidation_comment = "n8 margin call";
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_OK,
                 "the margin tail fields were refused");
    strategy_native_host_free(host);
}

/* ── Cohort rosters and the cohort-bound owner ───────────────────
 *
 * strategy_native_cohort_open/add/remove_v1 shipped with the L13 lane and had
 * no scenario at all, which left PF_NATIVE_OWNER_BIND_COHORT unreachable in
 * any executed path: the C++ twin's cohort close is HostSized, and HOST_SIZED
 * is deliberately refused in C. A fractional reduce over the cohort's own
 * scope is the C spelling of that close. */

#define COHORT_A_UNITS 3.0
#define COHORT_B_UNITS 2.0
#define COHORT_U_UNITS 4.0

typedef struct cohort_state {
    pf_strategy_t host;
    int      calculations;
    int      failures;
    uint64_t cohort;
    uint64_t entry_a;
    uint64_t entry_b;
    uint64_t unrelated;
    int      close_submitted;
    int      close_reject;
    double   units_before_close;
    double   units_after_close;
    int      rejected_zero_cohort;
    int      close_units_calls;
    double   close_scope_units;
} cohort_state;

/* The units half of resolve_execution_terms: a cohort close takes the whole
 * roster's live exposure and nothing else. */
static int cohort_on_close_units(void* user, const pf_native_close_view_v1* view,
                                 double* units) {
    cohort_state* state = (cohort_state*)user;
    ++state->close_units_calls;
    state->close_scope_units = view->scope_exposure_units;
    LCHECK(state, view->struct_size == (uint32_t)sizeof(*view),
           "the close view is a different struct");
    LCHECK(state, view->default_resolved_price > 0.0, "the close view carried no price");
    *units = view->scope_exposure_units;
    return PF_NATIVE_ANSWER_PROVIDED;
}

static int cohort_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    cohort_state* state = (cohort_state*)user;
    pf_native_request_v1 request;
    uint32_t reject = 0;
    (void)bar;
    (void)at;
    ++state->calculations;

    if (state->calculations == 3) {
        LCHECK(state, strategy_native_cohort_open_v1(state->host, &state->cohort)
                          == PF_NATIVE_OK, "cohort_open was refused");
        LCHECK(state, state->cohort != 0u, "cohort_open answered the null roster");
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = COHORT_A_UNITS;
        request.label = "cohort-a";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->entry_a, NULL)
                          == PF_NATIVE_OK, "cohort entry A was refused");
        LCHECK(state, strategy_native_cohort_add_v1(state->host, state->cohort,
                                                    state->entry_a) == PF_NATIVE_OK,
               "cohort_add of A was refused");
        /* The roster handle is not optional. */
        LCHECK(state, strategy_native_cohort_add_v1(state->host, 0u, state->entry_a)
                          == PF_NATIVE_E_ARGUMENT, "cohort_add accepted the null roster");
        LCHECK(state, strategy_native_cohort_remove_v1(state->host, 0u, state->entry_a)
                          == PF_NATIVE_E_ARGUMENT, "cohort_remove accepted the null roster");
        /* A BIND_COHORT request needs one too. */
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_HOST_SIZED;
        request.owner = PF_NATIVE_OWNER_BIND_COHORT;
        request.cohort = 0u;
        request.label = "cohort-null";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, &reject)
                          == PF_NATIVE_E_ARGUMENT,
               "a cohort-bound request with the null roster was accepted");
        state->rejected_zero_cohort = 1;
        return 0;
    }
    if (state->calculations == 5) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = COHORT_B_UNITS;
        request.label = "cohort-b";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->entry_b, NULL)
                          == PF_NATIVE_OK, "cohort entry B was refused");
        LCHECK(state, strategy_native_cohort_add_v1(state->host, state->cohort,
                                                    state->entry_b) == PF_NATIVE_OK,
               "cohort_add of B was refused");
        /* Never added to the roster, so the cohort close must not reach it. */
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = COHORT_U_UNITS;
        request.label = "cohort-unrelated";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->unrelated, NULL)
                          == PF_NATIVE_OK, "the unrelated entry was refused");
        return 0;
    }
    if (state->calculations == 8) {
        LCHECK(state, strategy_native_position_v1(state->host, &state->units_before_close,
                                                  NULL, NULL) == PF_NATIVE_OK,
               "the pre-close position was refused");
        /* A is taken OFF the roster before the close, so the close is scoped
         * to B alone even though both are still live. */
        LCHECK(state, strategy_native_cohort_remove_v1(state->host, state->cohort,
                                                       state->entry_a) == PF_NATIVE_OK,
               "cohort_remove of A was refused");
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_HOST_SIZED;
        request.owner = PF_NATIVE_OWNER_BIND_COHORT;
        request.cohort = state->cohort;
        request.label = "cohort-close";
        state->close_submitted = strategy_native_submit_v1(state->host, &request, NULL,
                                                           &reject);
        state->close_reject = (int)reject;
        return 0;
    }
    if (state->calculations == 12 && state->units_after_close == 0.0) {
        LCHECK(state, strategy_native_position_v1(state->host, &state->units_after_close,
                                                  NULL, NULL) == PF_NATIVE_OK,
               "the post-close position was refused");
    }
    return 0;
}

static void check_cohort_roster(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_callbacks_v1 table;
    cohort_state state;
    const pf_bar_t* bars;
    uint64_t cohort = 0;
    int n = 0;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = cohort_on_bar;
    table.on_close_units = cohort_on_close_units;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "cohort host create failed");
    if (!state.host) return;

    CHECK_EQ_INT(strategy_native_cohort_open_v1(NULL, &cohort), PF_NATIVE_E_HANDLE,
                 "cohort_open accepted a NULL handle");
    CHECK_EQ_INT(strategy_native_cohort_open_v1(state.host, NULL), PF_NATIVE_E_ARGUMENT,
                 "cohort_open accepted a NULL output");
    /* Before configure there is no run identity for a roster member to
     * belong to. */
    CHECK_EQ_INT(strategy_native_cohort_add_v1(state.host, 1u, 1u), PF_NATIVE_E_STATE,
                 "cohort_add answered before the run had an identity");
    CHECK_EQ_INT(strategy_native_cohort_remove_v1(state.host, 1u, 1u), PF_NATIVE_E_STATE,
                 "cohort_remove answered before the run had an identity");

    CHECK_EQ_INT(strategy_configure_native_v1(state.host, &spec), 0, "cohort configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the cohort run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback cohort rows failed");

    CHECK(state.rejected_zero_cohort, "the null-roster refusal never ran");
    CHECK_EQ_INT(state.close_submitted, PF_NATIVE_OK, "the cohort close was refused");
    CHECK_EQ_INT(state.close_reject, 0, "the cohort close reject reason");
    CHECK(fabs(state.units_before_close - (COHORT_A_UNITS + COHORT_B_UNITS + COHORT_U_UNITS))
              < 1e-9,
          "the three entries did not all carry before the close");
    /* Exactly B left: A was removed from the roster and the unrelated entry
     * was never on it. */
    CHECK(state.close_units_calls >= 1, "the close-units hook was never consulted");
    /* A was removed from the roster and the unrelated entry was never on it,
     * so the scope the close was resolved against is exactly B. */
    CHECK(fabs(state.close_scope_units - COHORT_B_UNITS) < 1e-9,
          "the cohort close was scoped to something other than its roster");
    CHECK_EQ_INT((int)(state.units_after_close * 100.0),
                 (int)((COHORT_A_UNITS + COHORT_U_UNITS) * 100.0),
                 "the cohort close did not take exactly the roster's own units");
    strategy_native_host_free(state.host);
}

/* ── The auxiliary finer feed ───────────────────────────────────── */

/* Eight 15-minute inputs over 120 one-minute feed bars. The feed rises 0.2 a
 * minute, so a five-minute bucket's open / low are its first minute's and its
 * high / close its fifth's: every expected value below is written down by
 * hand from that, not read back from the runtime. */
#define AUX_INPUTS 8
#define AUX_MINUTES (AUX_INPUTS * 15)

static pf_bar_t aux_inputs[AUX_INPUTS];
static pf_bar_t aux_minutes[AUX_MINUTES];

static void aux_fill(void) {
    int i;
    for (i = 0; i < AUX_MINUTES; ++i) {
        const double open = 100.0 + 0.2 * (double)i;
        aux_minutes[i].open = open;
        aux_minutes[i].high = open + 0.3;
        aux_minutes[i].low = open - 0.2;
        aux_minutes[i].close = open + 0.1;
        aux_minutes[i].volume = 1.0;
        aux_minutes[i].timestamp = (int64_t)i * 60000;
    }
    for (i = 0; i < AUX_INPUTS; ++i) {
        aux_inputs[i].open = aux_minutes[i * 15].open;
        aux_inputs[i].high = aux_minutes[i * 15 + 14].high;
        aux_inputs[i].low = aux_minutes[i * 15].low;
        aux_inputs[i].close = aux_minutes[i * 15 + 14].close;
        aux_inputs[i].volume = 15.0;
        aux_inputs[i].timestamp = (int64_t)i * 900000;
    }
}

typedef struct aux_state {
    int from_input;   /* buckets of row 0, the "60" series built from the input */
    int from_feed;    /* buckets of row 1, the "5" series built from the feed */
    int wrong;        /* a bucket that is not the hand-derived one */
} aux_state;

static int aux_on_timeframe_bar(void* user, const pf_bar_t* bar, uint32_t subscription,
                                uint32_t completion, int64_t delivered_at_ms) {
    aux_state* state = (aux_state*)user;
    if (subscription == 0u) {
        ++state->from_input;
        return 0;
    }
    {
        const int k = state->from_feed++;
        const pf_bar_t* first = &aux_minutes[k * 5];
        const pf_bar_t* last = &aux_minutes[k * 5 + 4];
        if (subscription != 1u || completion != 0u
            || bar->timestamp != (int64_t)k * 300000
            || bar->open != first->open || bar->low != first->low
            || bar->high != last->high || bar->close != last->close
            || bar->volume != 5.0
            /* Buckets 3j, 3j+1, 3j+2 ride on input j. */
            || delivered_at_ms != (int64_t)(k / 3) * 900000) {
            ++state->wrong;
        }
    }
    return 0;
}

static pf_native_run_spec_ext_v1 aux_ext(const pf_native_subscription_v1* rows,
                                         const uint32_t* sources, int32_t feed_n) {
    pf_native_run_spec_ext_v1 ext;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS | PF_NATIVE_SPEC_EXT_AUXILIARY_FEED;
    ext.subscriptions = rows;
    ext.subscriptions_n = 2u;
    ext.subscription_sources = sources;
    ext.auxiliary_tf = "1";
    ext.auxiliary_bars = aux_minutes;
    ext.auxiliary_n = feed_n;
    return ext;
}

static void check_auxiliary_feed(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_subscription_v1 rows[2];
    const uint32_t sources[2] = {PF_NATIVE_SERIES_SOURCE_INPUT,
                                 PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED};
    uint32_t bad_sources[2] = {PF_NATIVE_SERIES_SOURCE_INPUT, 9u};
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    pf_native_state_v1 lifecycle;
    aux_state state;
    pf_strategy_t host;
    int i;

    aux_fill();
    spec.session_key = "native-c-api-auxiliary";
    spec.input_tf = "15";
    spec.script_tf = "15";
    memset(rows, 0, sizeof(rows));
    rows[0].struct_size = (uint32_t)sizeof(rows[0]);
    rows[0].tf = "60";
    rows[1].struct_size = (uint32_t)sizeof(rows[1]);
    rows[1].tf = "5";

    /* Refusals first, all on one handle that must stay Unconfigured. */
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_timeframe_bar = aux_on_timeframe_bar;
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "auxiliary host create failed");
    if (!host) return;

    /* The feed lives in the appended tail: a caller sending the risk layout
     * (the header as it was before the tail) cannot declare it... */
    ext = aux_ext(rows, sources, AUX_MINUTES);
    ext.struct_size = PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_STRUCT,
                 "the auxiliary feed was accepted from a risk-layout extension");
    ext = aux_ext(rows, sources, AUX_MINUTES);
    ext.auxiliary_tf = NULL;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_ARGUMENT,
                 "an auxiliary feed without a timeframe was accepted");
    ext = aux_ext(rows, sources, AUX_MINUTES);
    ext.auxiliary_n = -1;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_ARGUMENT,
                 "a negative auxiliary bar count was accepted");
    ext = aux_ext(rows, sources, AUX_MINUTES);
    ext.reserved1 = 1u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "a non-zero reserved word was accepted");
    ext = aux_ext(rows, bad_sources, AUX_MINUTES);
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_E_TAG,
                 "an unknown series source was accepted");
    memset(&lifecycle, 0, sizeof(lifecycle));
    lifecycle.struct_size = (uint32_t)sizeof(lifecycle);
    CHECK_EQ_INT(strategy_native_state_v1(host, &lifecycle), PF_NATIVE_OK, "auxiliary state read");
    CHECK_EQ_INT(lifecycle.lifecycle, PF_NATIVE_LIFECYCLE_UNCONFIGURED,
                 "a refused auxiliary feed configured or failed the host");
    /* An append has nothing to append to before a realtime stream. */
    CHECK_EQ_INT(strategy_native_append_auxiliary_bars_v1(host, aux_minutes, 15),
                 PF_NATIVE_E_STATE, "an append was accepted before any run");
    CHECK_EQ_INT(strategy_native_append_auxiliary_bars_v1(NULL, aux_minutes, 15),
                 PF_NATIVE_E_HANDLE, "an append was accepted on a NULL handle");
    CHECK_EQ_INT(strategy_native_append_auxiliary_bars_v1(host, aux_minutes, -1),
                 PF_NATIVE_E_ARGUMENT, "a negative append count was accepted");

    /* A series may not name a feed nobody declared, and a feed of the input's
     * own timeframe is no finer feed at all. Both are the KERNEL's validation,
     * which fails the host it refuses, so each takes a handle of its own. */
    {
        pf_native_callbacks_v1 plain = blank_callbacks(NULL);
        pf_strategy_t refused = strategy_native_host_create_v1(&plain);
        CHECK(refused != NULL, "undeclared-feed host create failed");
        if (refused) {
            ext = aux_ext(rows, sources, AUX_MINUTES);
            ext.present_mask = PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS;
            CHECK_EQ_INT(strategy_configure_native_ext_v1(refused, &spec, &ext),
                         PF_NATIVE_E_ARGUMENT,
                         "a series was built from an undeclared auxiliary feed");
            strategy_native_host_free(refused);
        }
        refused = strategy_native_host_create_v1(&plain);
        CHECK(refused != NULL, "coarse-feed host create failed");
        if (refused) {
            ext = aux_ext(rows, sources, AUX_MINUTES);
            ext.auxiliary_tf = "15";
            CHECK_EQ_INT(strategy_configure_native_ext_v1(refused, &spec, &ext),
                         PF_NATIVE_E_ARGUMENT,
                         "an auxiliary feed no finer than the input was accepted");
            strategy_native_host_free(refused);
        }
    }

    /* Batch: the "60" series from the input, the "5" series from the feed. */
    ext = aux_ext(rows, sources, AUX_MINUTES);
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_OK,
                 "the auxiliary feed was refused");
    CHECK_EQ_INT(strategy_native_run_v1(host, aux_inputs, AUX_INPUTS, NULL), PF_NATIVE_OK,
                 "the auxiliary batch did not complete");
    CHECK_EQ_INT(state.from_input, 2, "the input-built hour series");
    CHECK_EQ_INT(state.from_feed, 24, "the feed-built five-minute series");
    CHECK_EQ_INT(state.wrong, 0, "a feed-built bucket is not the hand-derived one");
    strategy_native_host_free(host);

    /* A caller compiled before the tail existed still configures its risk
     * block: the second published layout stays accepted. */
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "risk-layout host create failed");
    if (!host) return;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_RISK;
    ext.risk_has_max_fills_per_day = 1u;
    ext.risk_max_fills_per_day = 3u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_OK,
                 "a risk-layout extension is no longer accepted");
    strategy_native_host_free(host);

    /* Stream: the feed is declared as far as the four warmup inputs reach;
     * each live input's fifteen finer bars are appended, then the input is
     * pushed. The series is the batch's, bucket for bucket. */
    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_timeframe_bar = aux_on_timeframe_bar;
    host = strategy_native_host_create_v1(&table);
    CHECK(host != NULL, "auxiliary stream host create failed");
    if (!host) return;
    ext = aux_ext(rows, sources, 60);
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, &spec, &ext), PF_NATIVE_OK,
                 "the streaming auxiliary feed was refused");
    CHECK_EQ_INT(strategy_stream_begin(host, aux_inputs, 4, "15", "15"), 0,
                 "the auxiliary stream did not begin");
    CHECK_EQ_INT(state.from_feed, 12, "the warmup's feed-built buckets");
    /* A bar of a period the warmup already accepted is refused by name and
     * leaves the stream running. */
    CHECK_EQ_INT(strategy_native_append_auxiliary_bars_v1(host, &aux_minutes[59], 1),
                 PF_NATIVE_E_STATE, "a repeated auxiliary bar was accepted");
    CHECK(strategy_get_last_error(host) != NULL && strategy_get_last_error(host)[0] != '\0',
          "a refused append left no reason");
    for (i = 4; i < AUX_INPUTS; ++i) {
        CHECK_EQ_INT(strategy_native_append_auxiliary_bars_v1(host, &aux_minutes[i * 15], 15),
                     PF_NATIVE_OK, "a live append was refused");
        CHECK_EQ_INT(strategy_stream_push_bar(host, &aux_inputs[i]), 0,
                     "a live input was refused");
    }
    CHECK_EQ_INT(strategy_stream_end(host, 0), 0, "the auxiliary stream did not end");
    CHECK_EQ_INT(state.from_input, 2, "the streamed input-built hour series");
    CHECK_EQ_INT(state.from_feed, 24, "the streamed feed-built five-minute series");
    CHECK_EQ_INT(state.wrong, 0, "a streamed feed-built bucket is not the hand-derived one");
    strategy_native_host_free(host);
}

/* --- the FX roll check point, and the ticket its liquidation books --- */

/* Audit lane P4. NativeMarginCheckKind::FxRoll shipped in N6 without a C
 * name: pf_native_margin_check_kind_e stopped at CALCULATION, so a C host was
 * handed the number 3 and had to guess what it meant. The scenario below is
 * the C spelling of the kernel test's own acceptance row
 * (tests/test_native_margin_fx_roll.cpp), with the same hand arithmetic --
 * four flat bars at 100 and a long 10 opened at bar 1's open, under a curve
 * whose single step to 2.5 sits at bar 1's CLOSE coordinate:
 *
 *     equity(100, fx)   = 1000                       (no price ever moves)
 *     required(100, fx) = 10 * 100 * fx * 0.5        = 500 at 1.0, 1250 at 2.5
 *
 * Nothing moves after the entry -- no price, no position -- so the roll is
 * the only point that re-measures anything, and it is the point that
 * breaches. The liquidation level a long 10 solves to is 200 * (fx - 1) / fx:
 * 0 at rate 1.0, which no price the run prints ever reaches, and 120 at 2.5,
 * which is already through the standing 100. That is MG9's whole claim. The
 * kernel rests the liquidation at the roll and takes it at the next opening
 * print, restoring the minimum: (1250 - 1000) / (100 * 2.5 * 0.5) = 2 units.
 *
 * expectation corrected (R5 follow-up lane E3): FX_ROLL_STEP_MS 900000 ->
 * 600000, because P4 wrote this step one bar LATER than its scenario wanted,
 * to dodge a kernel defect it had just found -- 600000 is bar 1's close
 * coordinate, the clock the engine presents while the ENTRY fill's applied
 * check is drained, and the margin model converted there instead of at the
 * fill's own cursor (300000), so the entry itself breached at `required 1250`
 * and the roll never re-measured. E3 made every check point convert at its
 * own cursor, so the step belongs where the scenario meant it: the entry is
 * quiet at rate 1.0 and the roll at 600000 is what breaches. The C rows below
 * are unchanged, and `call_time_ms` is added to pin WHICH point called.
 *
 * The second half reads the TICKET that slice is booked under. A C host has
 * no pf_trade_t::exit_id -- that struct is the codegen ABI's and grows only
 * with PF_ABI_VERSION -- but it does not need one: the run declares the
 * ticket (margin_liquidation_label) and reads it back per report row with
 * strategy_closed_trade_exit_id, a kernel-archive PF_API symbol that takes
 * any handle, this header's included. That is the route the COVERAGE block
 * names, and this is its executed proof. */
#define FX_ROLL_CAPITAL     1000.0
#define FX_ROLL_UNITS       10.0
#define FX_ROLL_MAINTENANCE 0.5
#define FX_ROLL_RATE        2.5
#define FX_ROLL_SLICE       2.0
#define FX_ROLL_MARK        100.0
#define FX_ROLL_STEP_MS     600000   /* bar 1's close == bar 2's open coordinate */
#define FX_ROLL_N           4
#define FX_ROLL_TICKET      "c-fx-roll-liquidation"
#define FX_ROLL_COMMENT     "rolled into the maintenance line"
#define FX_ROLL_CAUSE_MARGIN_CALL 3

static pf_bar_t fx_roll_bars[FX_ROLL_N];

static void fx_roll_fill(void) {
    int i;
    for (i = 0; i < FX_ROLL_N; ++i) {
        fx_roll_bars[i].open = FX_ROLL_MARK;
        fx_roll_bars[i].high = FX_ROLL_MARK;
        fx_roll_bars[i].low = FX_ROLL_MARK;
        fx_roll_bars[i].close = FX_ROLL_MARK;
        fx_roll_bars[i].volume = 1.0;
        fx_roll_bars[i].timestamp = (int64_t)i * 300000;
    }
}

typedef struct fx_roll_state {
    pf_strategy_t host;
    int failures;
    int calculations;
    int offered[PF_NATIVE_MARGIN_CHECK_FX_ROLL + 1]; /* one counter per named kind */
    int offered_unnamed;      /* a kind this header cannot spell */
    int roll_facts;           /* rolls whose point facts are the hand-derived ones */
    int roll_measured;        /* requirement views tagged FX_ROLL */
    int roll_numbers;         /* ... whose two numbers are the new rate's */
    int margin_calls;
    double called_units;
    int64_t call_time_ms;     /* the cursor the call was made at */
} fx_roll_state;

static int fx_roll_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    fx_roll_state* state = (fx_roll_state*)user;
    pf_native_request_v1 request;
    (void)bar;
    (void)at;
    if (state->calculations++ != 0) return 0;
    request = blank_request();
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = FX_ROLL_UNITS;
    request.trigger = PF_NATIVE_TRIGGER_MARKET;
    request.label = "fx-roll-entry";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "the fx-roll entry was refused");
    return 0;
}

static int fx_roll_on_check(void* user, const pf_native_margin_view_v1* at, int32_t* allowed) {
    fx_roll_state* state = (fx_roll_state*)user;
    (void)allowed;
    if (at->kind <= (uint32_t)PF_NATIVE_MARGIN_CHECK_FX_ROLL) {
        ++state->offered[at->kind];
    } else {
        ++state->offered_unnamed;
    }
    if (at->kind == (uint32_t)PF_NATIVE_MARGIN_CHECK_FX_ROLL
        && at->struct_size == (uint32_t)sizeof(*at)
        && at->version == PF_NATIVE_API_VERSION
        /* The head of the step's own segment: the rate moved, nothing else. */
        && at->cursor_effective_time_ms == (int64_t)FX_ROLL_STEP_MS
        && at->cursor_t == 0.0
        && at->mark == FX_ROLL_MARK
        && at->signed_units == FX_ROLL_UNITS
        && at->liquidation_resting == 0u) {
        ++state->roll_facts;
    }
    return PF_NATIVE_ANSWER_DEFAULT;
}

static int fx_roll_on_requirement(void* user, const pf_native_margin_view_v1* view,
                                  pf_native_margin_decision_v1* out) {
    fx_roll_state* state = (fx_roll_state*)user;
    (void)out;
    if (view->kind != (uint32_t)PF_NATIVE_MARGIN_CHECK_FX_ROLL) return PF_NATIVE_ANSWER_DEFAULT;
    ++state->roll_measured;
    if (fabs(view->equity - FX_ROLL_CAPITAL) < 1e-9
        && fabs(view->required
                - FX_ROLL_UNITS * FX_ROLL_MARK * FX_ROLL_RATE * FX_ROLL_MAINTENANCE) < 1e-9) {
        ++state->roll_numbers;
    }
    return PF_NATIVE_ANSWER_DEFAULT;
}

static int fx_roll_on_margin_call(void* user, const pf_native_event_v1* call) {
    fx_roll_state* state = (fx_roll_state*)user;
    ++state->margin_calls;
    state->called_units = call->closed_units;
    state->call_time_ms = call->effective_time_ms;
    return 0;
}

static void check_fx_roll_margin_point(void) {
    const int64_t step_ms = (int64_t)FX_ROLL_STEP_MS;
    const double step_rate = FX_ROLL_RATE;
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext;
    pf_native_fx_curve_v1 curve;
    pf_native_callbacks_v1 table;
    fx_roll_state state;
    pf_report_t report;
    int ticket_rows = 0;
    int i;

    fx_roll_fill();
    memset(&state, 0, sizeof(state));
    memset(&report, 0, sizeof(report));
    table = blank_callbacks(&state);
    table.on_bar = fx_roll_on_bar;
    table.on_margin_check = fx_roll_on_check;
    table.on_margin_requirement = fx_roll_on_requirement;
    table.on_margin_call = fx_roll_on_margin_call;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "fx-roll host create failed");
    if (!state.host) return;

    spec.session_key = "native-c-api-fx-roll";
    spec.initial_capital = FX_ROLL_CAPITAL;
    spec.account_fx = 1.0;

    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_MARGIN;
    ext.margin_has_maintenance_long = 1u;
    ext.margin_maintenance_long = FX_ROLL_MAINTENANCE;
    ext.margin_has_maintenance_short = 1u;
    ext.margin_maintenance_short = FX_ROLL_MAINTENANCE;
    ext.margin_sizing = 0u;    /* RestoreMinimum */
    ext.margin_shortfall_multiple = 1.0;
    ext.margin_check = 0u;     /* PathAdverseExtreme */
    ext.margin_liquidation_label = FX_ROLL_TICKET;
    ext.margin_liquidation_comment = FX_ROLL_COMMENT;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the fx-roll margin extension was refused");

    memset(&curve, 0, sizeof(curve));
    curve.struct_size = (uint32_t)sizeof(curve);
    curve.n = 1u;
    curve.effective_from_ms = &step_ms;
    curve.account_per_quote = &step_rate;
    CHECK_EQ_INT(strategy_configure_native_fx_curve_v1(state.host, &curve), 0,
                 "the fx-roll curve was refused");

    CHECK_EQ_INT(strategy_native_run_v1(state.host, fx_roll_bars, FX_ROLL_N, &report),
                 PF_NATIVE_OK, "the fx-roll run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback fx-roll rows failed");

    /* The fourth kind reaches a C host under its own name, exactly once. */
    CHECK_EQ_INT(state.offered[PF_NATIVE_MARGIN_CHECK_FX_ROLL], 1,
                 "the curve step was not offered once as PF_NATIVE_MARGIN_CHECK_FX_ROLL");
    CHECK_EQ_INT(state.offered_unnamed, 0, "a check point arrived with no C name");
    CHECK(state.offered[PF_NATIVE_MARGIN_CHECK_BAR_OPEN] > 0,
          "no bar-open check point was offered");
    CHECK_EQ_INT(state.offered[PF_NATIVE_MARGIN_CHECK_CALCULATION], 0,
                 "a path-checking model was offered a CalculationOnly point");
    CHECK_EQ_INT(state.roll_facts, 1, "the roll named another point");
    CHECK_EQ_INT(state.roll_measured, 1, "the roll measured nothing");
    CHECK_EQ_INT(state.roll_numbers, 1,
                 "the roll's two numbers are not the new rate at the unchanged price");
    CHECK_EQ_INT(state.margin_calls, 1, "the roll's breach called no margin");
    CHECK(fabs(state.called_units - FX_ROLL_SLICE) < 1e-9,
          "the liquidation sliced another quantity");
    /* R5 lane E3: the call belongs to the ROLL's instant. The entry fill's own
     * applied check stands at 300000, where the curve still says 1.0 and the
     * requirement is 500 against equity 1000 -- quiet. The engine is
     * presenting 600000 while that check is drained, and converting there is
     * what used to breach at the entry. */
    CHECK(state.offered[PF_NATIVE_MARGIN_CHECK_AFTER_APPLIED] > 0,
          "no applied check point was offered");
    CHECK(state.call_time_ms == (int64_t)FX_ROLL_STEP_MS,
          "the margin call was not made at the roll's own instant");

    /* And the closed row it books carries the ticket this run declared. */
    CHECK(report.trades_len > 0, "the fx-roll run booked no closed row");
    for (i = 0; i < report.trades_len; ++i) {
        const char* exit_id = strategy_closed_trade_exit_id(state.host, i);
        const char* comment = strategy_closed_trade_exit_comment(state.host, i);
        CHECK(exit_id != NULL, "a report row answered no exit ticket");
        if (!exit_id || strcmp(exit_id, FX_ROLL_TICKET) != 0) continue;
        ++ticket_rows;
        CHECK(comment != NULL && strcmp(comment, FX_ROLL_COMMENT) == 0,
              "the liquidation row carries another exit comment");
        CHECK_EQ_INT(strategy_closed_trade_close_cause(state.host, i),
                     FX_ROLL_CAUSE_MARGIN_CALL,
                     "the liquidation row is not classified as a margin call");
        CHECK_EQ_INT(report.trades[i].is_long, 1, "the liquidation row closed another side");
        CHECK(fabs(report.trades[i].qty - FX_ROLL_SLICE) < 1e-9,
              "the liquidation row closed another quantity");
        CHECK(fabs(report.trades[i].entry_price - FX_ROLL_MARK) < 1e-9,
              "the liquidation row entered at another price");
    }
    CHECK_EQ_INT(ticket_rows, 1, "no closed row carries the declared liquidation ticket");
    CHECK(strategy_closed_trade_exit_id(state.host, report.trades_len) == NULL,
          "an out-of-range report row answered a ticket");

    strategy_native_report_free_v1(&report);
    strategy_native_host_free(state.host);
}

/* ── A subscription row's two delivery words (lane E7) ──────────────
 *
 * pf_native_subscription_v1::lookahead holds a pf_native_lookahead_e value and
 * ::gaps a pf_native_gaps_e value: the same two uint32_t words at the same
 * offsets, now translated by an exhaustive switch like every other enum-valued
 * field. Four rows of one "15" series, one per pair of values, run over
 * thirteen whole buckets of the twin's "5" feed, and each value must select
 * its own rule as a C host sees it: WHEN a bucket is delivered
 * (on_timeframe_bar's `delivered_at_ms` against the bucket's own open) and
 * what the series answers on the inputs that deliver nothing
 * (strategy_native_series_bar_v1). A value outside either enumeration is
 * refused on both entry points that take a row, and changes nothing. */

#define DELIVERY_ROWS        4
#define DELIVERY_BUCKET_BARS 3   /* one "15" bucket of the "5" feed */
#define DELIVERY_BUCKETS     13
#define DELIVERY_BARS        (DELIVERY_BUCKETS * DELIVERY_BUCKET_BARS)

static const uint32_t delivery_lookahead[DELIVERY_ROWS] = {
    PF_NATIVE_LOOKAHEAD_AT_COMPLETION, PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT,
    PF_NATIVE_LOOKAHEAD_AT_COMPLETION, PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT};
static const uint32_t delivery_gaps[DELIVERY_ROWS] = {
    PF_NATIVE_GAPS_HOLD, PF_NATIVE_GAPS_HOLD, PF_NATIVE_GAPS_CLEAR, PF_NATIVE_GAPS_CLEAR};

typedef struct delivery_state {
    pf_strategy_t host;
    int           calculations;
    int           failures;
    int           deliveries[DELIVERY_ROWS];
    int           off_input[DELIVERY_ROWS];    /* rode on another input than the rule names */
    int           off_bucket[DELIVERY_ROWS];   /* values that are not the whole bucket */
    int           off_presence[DELIVERY_ROWS]; /* series_bar answered against the rule */
} delivery_state;

static int delivery_on_timeframe_bar(void* user, const pf_bar_t* bar, uint32_t subscription,
                                     uint32_t completion, int64_t delivered_at_ms) {
    delivery_state* state = (delivery_state*)user;
    const pf_bar_t* feed = pf_twin_bars(NULL);
    const int64_t bucket_ms = (int64_t)DELIVERY_BUCKET_BARS * 300000;
    int first;
    int last;
    int rides_on;
    (void)completion;
    LCHECK(state, subscription < DELIVERY_ROWS, "a bucket was delivered under no declared row");
    if (subscription >= DELIVERY_ROWS) return 0;
    ++state->deliveries[subscription];
    first = (int)(bar->timestamp / bucket_ms) * DELIVERY_BUCKET_BARS;
    last = first + DELIVERY_BUCKET_BARS - 1;
    LCHECK(state, bar->timestamp % bucket_ms == 0 && first >= 0 && last < DELIVERY_BARS,
           "a delivered bucket is not one of the feed's thirteen");
    if (first < 0 || last >= DELIVERY_BARS) return 0;
    /* AT_FIRST_INPUT: the bucket's final values ride on its first input.
     * AT_COMPLETION: the bucket rides on the input that completes it. */
    rides_on = delivery_lookahead[subscription] == PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT ? first
                                                                                        : last;
    if (delivered_at_ms != feed[rides_on].timestamp) ++state->off_input[subscription];
    /* The whole bucket under either rule. The feed rises bar by bar, so its
     * high and close are the last input's and its open and low the first's. */
    if (bar->open != feed[first].open || bar->low != feed[first].low
        || bar->high != feed[last].high || bar->close != feed[last].close
        || bar->volume != (double)DELIVERY_BUCKET_BARS * feed[first].volume) {
        ++state->off_bucket[subscription];
    }
    return 0;
}

static int delivery_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    delivery_state* state = (delivery_state*)user;
    const int input = state->calculations++;  /* 0-based: one calculation per input */
    const int phase = input % DELIVERY_BUCKET_BARS;
    uint32_t row;
    (void)bar;
    (void)at;
    for (row = 0; row < DELIVERY_ROWS; ++row) {
        const int at_first = delivery_lookahead[row] == PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT;
        /* Whether this input delivers the row's bucket, and whether any
         * input has delivered one yet. */
        const int delivered_here = at_first ? phase == 0 : phase == DELIVERY_BUCKET_BARS - 1;
        const int delivered_yet = at_first ? 1 : input >= DELIVERY_BUCKET_BARS - 1;
        const int want = delivery_gaps[row] == PF_NATIVE_GAPS_CLEAR ? delivered_here
                                                                    : delivered_yet;
        pf_bar_t series;
        int rc;
        memset(&series, 0, sizeof(series));
        rc = strategy_native_series_bar_v1(state->host, row, &series);
        LCHECK(state, rc == PF_NATIVE_OK || rc == PF_NATIVE_ABSENT,
               "a declared series answered neither a bar nor absent");
        if ((rc == PF_NATIVE_OK) != want) ++state->off_presence[row];
    }
    return 0;
}

static void check_subscription_delivery_words(void) {
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_subscription_v1 rows[DELIVERY_ROWS];
    pf_native_subscription_v1 row;
    pf_native_run_spec_ext_v1 ext;
    pf_native_callbacks_v1 table;
    pf_native_state_v1 lifecycle;
    delivery_state state;
    char what[128];
    uint32_t i;

    /* Typing the words moved nothing: each is still one uint32_t, zero is the
     * default of both, and on LP64 -- every CI host's data model -- the row
     * keeps the offsets and the size measured on 0a47cbf7, before the two
     * enumerations existed. */
    memset(&row, 0, sizeof(row));
    CHECK_EQ_INT(sizeof(row.lookahead), sizeof(uint32_t), "lookahead is not one uint32_t word");
    CHECK_EQ_INT(sizeof(row.gaps), sizeof(uint32_t), "gaps is not one uint32_t word");
    CHECK_EQ_INT(row.lookahead, PF_NATIVE_LOOKAHEAD_AT_COMPLETION,
                 "a zero-filled row does not deliver at completion");
    CHECK_EQ_INT(row.gaps, PF_NATIVE_GAPS_HOLD, "a zero-filled row does not hold its bucket");
    if (sizeof(void*) == 8) {
        CHECK_EQ_INT(offsetof(pf_native_subscription_v1, lookahead), 4, "lookahead moved");
        CHECK_EQ_INT(offsetof(pf_native_subscription_v1, tf), 8, "tf moved");
        CHECK_EQ_INT(offsetof(pf_native_subscription_v1, authoritative_bars), 16,
                     "authoritative_bars moved");
        CHECK_EQ_INT(offsetof(pf_native_subscription_v1, authoritative_n), 24,
                     "authoritative_n moved");
        CHECK_EQ_INT(offsetof(pf_native_subscription_v1, gaps), 28, "gaps moved");
        CHECK_EQ_INT(sizeof(pf_native_subscription_v1), 32, "pf_native_subscription_v1 resized");
    }

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = delivery_on_bar;
    table.on_timeframe_bar = delivery_on_timeframe_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "delivery-word host create failed");
    if (!state.host) return;

    spec.session_key = "native-c-api-delivery-words";
    memset(rows, 0, sizeof(rows));
    for (i = 0; i < DELIVERY_ROWS; ++i) {
        rows[i].struct_size = (uint32_t)sizeof(rows[i]);
        rows[i].tf = "15";
        rows[i].lookahead = delivery_lookahead[i];
        rows[i].gaps = delivery_gaps[i];
    }
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS;
    ext.subscriptions = rows;
    ext.subscriptions_n = DELIVERY_ROWS;

    /* One past the end of each enumeration: refused at the boundary with the
     * tag error every enum-valued field uses, at configure time and at begin
     * time alike, leaving the handle Unconfigured. */
    rows[1].lookahead = PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT + 1u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_E_TAG,
                 "a lookahead past pf_native_lookahead_e was configured");
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, rows, DELIVERY_ROWS),
                 PF_NATIVE_E_TAG, "a lookahead past pf_native_lookahead_e was declared");
    rows[1].lookahead = delivery_lookahead[1];
    rows[2].gaps = PF_NATIVE_GAPS_CLEAR + 1u;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_E_TAG,
                 "a gaps word past pf_native_gaps_e was configured");
    CHECK_EQ_INT(strategy_native_declare_subscriptions_v1(state.host, rows, DELIVERY_ROWS),
                 PF_NATIVE_E_TAG, "a gaps word past pf_native_gaps_e was declared");
    rows[2].gaps = delivery_gaps[2];
    memset(&lifecycle, 0, sizeof(lifecycle));
    lifecycle.struct_size = (uint32_t)sizeof(lifecycle);
    CHECK_EQ_INT(strategy_native_state_v1(state.host, &lifecycle), PF_NATIVE_OK,
                 "delivery-word state read");
    CHECK_EQ_INT(lifecycle.lifecycle, PF_NATIVE_LIFECYCLE_UNCONFIGURED,
                 "a refused delivery word configured or failed the host");

    /* Both values of both words, on one run. */
    CHECK_EQ_INT(strategy_configure_native_ext_v1(state.host, &spec, &ext), PF_NATIVE_OK,
                 "the four typed rows were refused");
    CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), DELIVERY_BARS, NULL),
                 PF_NATIVE_OK, "the delivery-word run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback delivery-word rows failed");
    CHECK_EQ_INT(state.calculations, DELIVERY_BARS, "the run calculated another input count");
    for (i = 0; i < DELIVERY_ROWS; ++i) {
        snprintf(what, sizeof(what), "row %u (lookahead %u, gaps %u): ", (unsigned)i,
                 (unsigned)delivery_lookahead[i], (unsigned)delivery_gaps[i]);
        if (state.deliveries[i] != DELIVERY_BUCKETS) {
            fprintf(stderr, "  %sdelivered another bucket count\n", what);
        }
        CHECK_EQ_INT(state.deliveries[i], DELIVERY_BUCKETS, "a row delivered another bucket count");
        if (state.off_input[i] != 0) {
            fprintf(stderr, "  %sa bucket rode on another input than its lookahead names\n", what);
        }
        CHECK_EQ_INT(state.off_input[i], 0, "a bucket rode on another input than its lookahead names");
        if (state.off_bucket[i] != 0) {
            fprintf(stderr, "  %sa delivered bucket is not the whole bucket\n", what);
        }
        CHECK_EQ_INT(state.off_bucket[i], 0, "a delivered bucket is not the whole bucket");
        if (state.off_presence[i] != 0) {
            fprintf(stderr, "  %sthe series answered against its gaps word\n", what);
        }
        CHECK_EQ_INT(state.off_presence[i], 0, "the series answered against its gaps word");
    }
    strategy_native_host_free(state.host);
}

/* ── A trail's arm, read back ────────────────────────────────────
 *
 * pf_native_request_v1 has always said whether a TRAIL carries an arm price
 * (`trail_has_arm_price`), but pf_native_working_v1 carried only p1 and p2,
 * so a trail with no arm and a trail armed at 0.0 read back as the same row.
 * Since lane E1 an anchored trail's absent arm is a spelling of its own,
 * which made that observable: the two legs below differ in nothing else a C
 * caller can read. The readout's appended tail carries the request's own
 * flag, and a caller compiled against the base layout is still served --
 * only as far as its own struct reaches. */

#define PRESENCE_ENTRY_BAR    3
#define PRESENCE_TRAIL_OFFSET 5.0
#define PRESENCE_ANCHOR       1.0
#define PRESENCE_SENTINEL     0xA5

typedef struct presence_state {
    pf_strategy_t host;
    int      calculations;
    int      failures;
    uint64_t entry;
    uint64_t absent;        /* anchored, arm left unwritten */
    uint64_t placeholder;   /* anchored, arm written as 0.0 */
    double   entry_fill;    /* the owner's booked fill; NaN until it lands */
    int      read_waiting;  /* 1 once the two waiting rows were told apart */
    int      read_armed;    /* 1 once both armed rows read back their level */
} presence_state;

/* The index of `incarnation` in a fresh working snapshot, or -1. */
static int presence_index(pf_strategy_t host, uint64_t incarnation) {
    pf_native_working_v1 row;
    const int n = strategy_native_working_len_v1(host);
    int i;
    for (i = 0; i < n; ++i) {
        memset(&row, 0, sizeof(row));
        row.struct_size = (uint32_t)sizeof(row);
        if (strategy_native_working_get_v1(host, i, &row) == PF_NATIVE_OK
            && row.incarnation == incarnation) {
            return i;
        }
    }
    return -1;
}

static int presence_row(pf_strategy_t host, uint64_t incarnation, pf_native_working_v1* row) {
    const int index = presence_index(host, incarnation);
    memset(row, 0, sizeof(*row));
    row->struct_size = (uint32_t)sizeof(*row);
    if (index < 0) return PF_NATIVE_E_ARGUMENT;
    return strategy_native_working_get_v1(host, index, row);
}

static int presence_on_applied(void* user, const pf_native_applied_v1* applied,
                               const pf_native_decision_v1* at) {
    presence_state* state = (presence_state*)user;
    (void)at;
    if (applied->incarnation == state->entry) state->entry_fill = applied->resolved_price;
    return 0;
}

static int presence_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    presence_state* state = (presence_state*)user;
    pf_native_request_v1 request;
    pf_native_working_v1 absent;
    pf_native_working_v1 placeholder;
    pf_native_working_v1 earlier;
    const unsigned char* bytes;
    uint64_t owner;
    int index;
    int untouched;
    int rc;
    size_t i;
    (void)bar;
    (void)at;
    ++state->calculations;

    if (state->calculations == PRESENCE_ENTRY_BAR) {
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.label = "presence-entry";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->entry, NULL)
                          == PF_NATIVE_OK, "the presence entry was refused");
        owner = state->entry;

        /* Two protective sell trails on that entry, each arming one point
         * above its fill. They differ only in how they spell the arm the
         * fill will supply: left unwritten, or written as the 0.0
         * placeholder. */
        request = blank_request();
        request.intent = PF_NATIVE_INTENT_REDUCE;
        request.reduce_size = PF_NATIVE_REDUCE_OWNER_OPENED;
        request.trigger = PF_NATIVE_TRIGGER_TRAIL;
        request.p1 = PRESENCE_TRAIL_OFFSET;
        request.anchor = PF_NATIVE_ANCHOR_FROM_OWNER_FILL;
        request.anchor_offset = PRESENCE_ANCHOR;
        request.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;
        request.owner_n = 1u;
        request.owner_incarnations = &owner;
        request.label = "presence-absent";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->absent, NULL)
                          == PF_NATIVE_OK, "the absent-arm anchored trail was refused");
        request.trail_has_arm_price = 1;
        request.p2 = 0.0;
        request.label = "presence-placeholder";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, &state->placeholder,
                                                NULL) == PF_NATIVE_OK,
               "the 0.0-arm anchored trail was refused");

        /* Waiting on the entry, each reads back the spelling it was
         * submitted with, and that flag is the one field telling them
         * apart. */
        LCHECK(state, presence_row(state->host, state->absent, &absent) == PF_NATIVE_OK,
               "the absent-arm trail is not a working row");
        LCHECK(state, presence_row(state->host, state->placeholder, &placeholder)
                          == PF_NATIVE_OK, "the 0.0-arm trail is not a working row");
        LCHECK(state, absent.trigger == PF_NATIVE_TRIGGER_TRAIL
                          && placeholder.trigger == PF_NATIVE_TRIGGER_TRAIL,
               "a waiting leg does not read back as a trail");
        LCHECK(state, absent.intent == placeholder.intent
                          && absent.intent_value == placeholder.intent_value
                          && absent.owner == placeholder.owner
                          && absent.trigger_state == placeholder.trigger_state
                          && absent.remaining_kind == placeholder.remaining_kind
                          && absent.p1 == PRESENCE_TRAIL_OFFSET
                          && placeholder.p1 == PRESENCE_TRAIL_OFFSET
                          && absent.p2 == 0.0 && placeholder.p2 == 0.0,
               "the two waiting trails differ in more than their arm spelling");
        LCHECK(state, absent.trail_has_arm_price == 0u,
               "the absent-arm trail reads back an arm price");
        LCHECK(state, placeholder.trail_has_arm_price == 1u,
               "the 0.0-arm trail reads back no arm price");
        LCHECK(state, absent.reserved1 == 0u && placeholder.reserved1 == 0u,
               "the tail's reserved word is not 0");
        state->read_waiting = absent.trail_has_arm_price == 0u
                              && placeholder.trail_has_arm_price == 1u;

        /* A caller compiled against the base layout sends the base length.
         * It is served, and written exactly that far: the bytes past its own
         * struct keep whatever it left there. */
        index = presence_index(state->host, state->absent);
        memset(&earlier, PRESENCE_SENTINEL, sizeof(earlier));
        earlier.struct_size = PF_NATIVE_WORKING_V1_BASE_SIZE;
        rc = strategy_native_working_get_v1(state->host, index, &earlier);
        LCHECK(state, rc == PF_NATIVE_OK, "a base-length working row was refused");
        if (rc == PF_NATIVE_OK) {
            LCHECK(state, earlier.struct_size == PF_NATIVE_WORKING_V1_BASE_SIZE,
                   "a base-length row came back with another length");
            LCHECK(state, earlier.incarnation == state->absent
                              && earlier.trigger == PF_NATIVE_TRIGGER_TRAIL
                              && earlier.p1 == PRESENCE_TRAIL_OFFSET && earlier.p2 == 0.0
                              && earlier.label != NULL
                              && strcmp(earlier.label, "presence-absent") == 0,
                   "a base-length row does not carry the base fields");
            bytes = (const unsigned char*)&earlier;
            untouched = 1;
            for (i = PF_NATIVE_WORKING_V1_BASE_SIZE; i < sizeof(earlier); ++i) {
                if (bytes[i] != PRESENCE_SENTINEL) untouched = 0;
            }
            LCHECK(state, untouched, "the runtime wrote past a base-length caller's struct");
        }

        /* Any third length is a caller this runtime cannot write. */
        memset(&earlier, 0, sizeof(earlier));
        earlier.struct_size = PF_NATIVE_WORKING_V1_BASE_SIZE + 4u;
        LCHECK(state, strategy_native_working_get_v1(state->host, index, &earlier)
                          == PF_NATIVE_E_STRUCT,
               "a third working-row length was accepted");
        return 0;
    }

    if (state->calculations == PRESENCE_ENTRY_BAR + 1) {
        /* The entry filled at this bar's open and armed both legs: the arm
         * installs fill + anchor into each definition, so both now carry the
         * same arm price and read it back the same way. */
        LCHECK(state, !isnan(state->entry_fill), "the presence entry never filled");
        LCHECK(state, presence_row(state->host, state->absent, &absent) == PF_NATIVE_OK,
               "the armed absent-arm trail is not a working row");
        LCHECK(state, presence_row(state->host, state->placeholder, &placeholder)
                          == PF_NATIVE_OK, "the armed 0.0-arm trail is not a working row");
        LCHECK(state, absent.trail_has_arm_price == 1u
                          && placeholder.trail_has_arm_price == 1u,
               "an armed trail reads back no arm price");
        LCHECK(state, absent.p2 == state->entry_fill + PRESENCE_ANCHOR
                          && placeholder.p2 == state->entry_fill + PRESENCE_ANCHOR,
               "an armed trail reads back another arm level");
        state->read_armed = absent.trail_has_arm_price == 1u
                            && placeholder.trail_has_arm_price == 1u;
    }
    return 0;
}

static void check_working_arm_presence(void) {
    presence_state state;
    pf_native_callbacks_v1 table;
    pf_native_run_spec_v1 spec = twin_spec();
    const pf_bar_t* bars;
    int n = 0;

    /* Two published working-row lengths. */
    CHECK(PF_NATIVE_WORKING_V1_BASE_SIZE < (uint32_t)sizeof(pf_native_working_v1),
          "the arm-presence tail is not past the base layout");

    memset(&state, 0, sizeof(state));
    state.entry_fill = NAN;
    table = blank_callbacks(&state);
    table.on_bar = presence_on_bar;
    table.on_applied = presence_on_applied;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "presence host create failed");
    if (!state.host) return;
    CHECK_EQ_INT(strategy_configure_native_v1(state.host, &spec), 0, "presence configure");
    bars = pf_twin_bars(&n);
    CHECK_EQ_INT(strategy_native_run_v1(state.host, bars, n, NULL), PF_NATIVE_OK,
                 "the presence run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback arm-presence rows failed");
    CHECK(state.read_waiting == 1, "the two waiting trails were not told apart");
    CHECK(state.read_armed == 1, "the two armed trails did not both read back their arm");
    strategy_native_host_free(state.host);
}

/* ── The run specifications' enum-valued words (lane E22) ───────────
 *
 * Nine words of the two run specifications hold a value of a C enumeration
 * named after the kernel rule it selects: the base spec's `fee_kind`
 * (pf_native_fee_kind_t), `close_execution` (pf_native_close_execution_t) and
 * `allowed_open_directions` (pf_native_open_directions_t), and the
 * extension's `report_policy` (pf_native_report_policy_t), `price_grid`
 * (pf_native_price_grid_t), `grid_rounding` (pf_native_grid_rounding_t),
 * `calculation` (pf_native_calc_trigger_t), `open_bar_view`
 * (pf_native_open_bar_view_t) and `margin_sizing`
 * (pf_native_liquidation_sizing_t). They are the same uint32_t words at the
 * same offsets, accepted and refused exactly as before. One scenario per word
 * runs every value and reads, from C alone, the rule that value selects; one
 * past the end of each enumeration is PF_NATIVE_E_TAG on a fresh handle,
 * which stays Unconfigured and then configures with the word restored. */

#define WORD_FEE        1.0   /* fee_value under every fee kind */
#define WORD_UNITS      2.0   /* the fee scenario's position */
#define WORD_CAPITAL    300.0 /* the sizing scenario's account */
#define WORD_MAINTENANCE 0.5  /* its maintenance fraction */
#define WORD_HELD       4.0   /* its position */
#define WORD_MULTIPLE   2.5   /* its shortfall multiple */

typedef struct word_step {
    int      calculation;   /* 1-based: the step runs in that close calculation */
    uint32_t intent;
    double   value;
} word_step;

typedef struct word_state {
    pf_strategy_t    host;
    const word_step* steps;
    int              n_steps;
    int              calculations;
    int              failures;
    int              fills;
    int              first_fill_interval;  /* the decision's script interval, first fill */
    int              long_openings;
    int              short_openings;
    int              bar_opens;
    int              complete_opens;    /* on_bar_open was handed the whole script bar */
    int              open_only_opens;   /* on_bar_open was handed H = L = C = open, no volume */
    int              recalculations[4]; /* on_recalculate calls, by pf_native_calc_reason_t */
    int              answered;          /* the requirement hook answered its one breach */
    int              inspected;
    int              liquidation_rows;
    uint32_t         liquidation_intent;
    double           liquidation_units;
} word_state;

static pf_native_run_spec_ext_v1 word_ext(uint32_t present_mask) {
    pf_native_run_spec_ext_v1 ext;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = present_mask;
    return ext;
}

static uint32_t word_lifecycle(pf_strategy_t host) {
    pf_native_state_v1 lifecycle;
    memset(&lifecycle, 0, sizeof(lifecycle));
    lifecycle.struct_size = (uint32_t)sizeof(lifecycle);
    CHECK_EQ_INT(strategy_native_state_v1(host, &lifecycle), PF_NATIVE_OK, "word state read");
    return lifecycle.lifecycle;
}

/* `word` points into `spec` or `ext`. One past the end of its enumeration is
 * the tag error; the refused configure leaves the handle Unconfigured, and the
 * same handle then configures with the word restored.
 *
 * A base-spec word is also read by strategy_configure_native_v1, which has
 * no boundary refusal of its own: the word reaches the kernel's validation,
 * which rejects the spec and, as it does for every spec it rejects, fails
 * the host. That entry point answers its one failure, -1, on a fresh handle
 * that is then Failed -- pinned here so the asymmetry cannot move quietly. */
static int configure_past_refusal(pf_strategy_t host, pf_native_run_spec_v1* spec,
                                  pf_native_run_spec_ext_v1* ext, uint32_t* word,
                                  uint32_t past_end, int base_word, const char* what) {
    const uint32_t kept = *word;
    *word = past_end;
    CHECK_EQ_INT(strategy_configure_native_ext_v1(host, spec, ext), PF_NATIVE_E_TAG, what);
    CHECK_EQ_INT(word_lifecycle(host), PF_NATIVE_LIFECYCLE_UNCONFIGURED,
                 "a refused word configured or failed the host");
    if (base_word) {
        pf_native_callbacks_v1 table = blank_callbacks(NULL);
        pf_strategy_t fresh = strategy_native_host_create_v1(&table);
        CHECK(fresh != NULL, "word-refusal host create failed");
        if (fresh) {
            CHECK_EQ_INT(strategy_configure_native_v1(fresh, spec), -1, what);
            CHECK_EQ_INT(word_lifecycle(fresh), PF_NATIVE_LIFECYCLE_FAILED,
                         "configure_native_v1 did not fail the host its kernel refused");
            strategy_native_host_free(fresh);
        }
    }
    *word = kept;
    return strategy_configure_native_ext_v1(host, spec, ext);
}

static void word_run_steps(word_state* state) {
    pf_native_request_v1 request;
    int i;
    ++state->calculations;
    for (i = 0; i < state->n_steps; ++i) {
        if (state->steps[i].calculation != state->calculations) continue;
        request = blank_request();
        request.intent = state->steps[i].intent;
        request.intent_value = state->steps[i].value;
        request.label = "word-step";
        LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL)
                          == PF_NATIVE_OK, "a word-scenario command was refused");
    }
}

static int word_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    (void)bar;
    (void)at;
    word_run_steps((word_state*)user);
    return 0;
}

static int word_on_applied(void* user, const pf_native_applied_v1* applied,
                           const pf_native_decision_v1* at) {
    word_state* state = (word_state*)user;
    if (state->fills++ == 0) state->first_fill_interval = at->interval_index;
    if (applied->opened_units > 0.0) ++state->long_openings;
    if (applied->opened_units < 0.0) ++state->short_openings;
    return 0;
}

/* A fresh host driven by `steps` from on_bar, with on_applied recording. */
static pf_strategy_t word_host(word_state* state, const word_step* steps, int n_steps) {
    pf_native_callbacks_v1 table;
    memset(state, 0, sizeof(*state));
    state->steps = steps;
    state->n_steps = n_steps;
    state->first_fill_interval = -1;
    table = blank_callbacks(state);
    table.on_bar = word_on_bar;
    table.on_applied = word_on_applied;
    state->host = strategy_native_host_create_v1(&table);
    CHECK(state->host != NULL, "word host create failed");
    return state->host;
}

/* A base-spec word is read by both entry points that take the base spec:
 * `entry` 0 is strategy_configure_native_v1 (0 on success, like PF_NATIVE_OK),
 * 1 is strategy_configure_native_ext_v1 with no extension block. The first
 * value's ext run also carries the refusal. */
static int configure_base_word(word_state* state, pf_native_run_spec_v1* spec, uint32_t* word,
                               uint32_t past_end, int entry, int first, const char* what) {
    pf_native_run_spec_ext_v1 ext = word_ext(0u);
    if (entry == 0) return strategy_configure_native_v1(state->host, spec);
    if (first) return configure_past_refusal(state->host, spec, &ext, word, past_end, 1, what);
    return strategy_configure_native_ext_v1(state->host, spec, &ext);
}

/* --- fee_kind: what one execution costs --- */

static const word_step fee_steps[] = {
    {3, PF_NATIVE_INTENT_TRANSACT, WORD_UNITS},   /* fills at the fourth open, 103 */
    {6, PF_NATIVE_INTENT_FLATTEN, 0.0}};          /* fills at the seventh open, 106 */

static void check_fee_kind_word(void) {
    static const pf_native_fee_kind_t kinds[] = {
        PF_NATIVE_FEE_PERCENT, PF_NATIVE_FEE_CASH_PER_UNIT, PF_NATIVE_FEE_CASH_PER_EXECUTION};
    const double entry_price = 103.0;
    const double exit_price = 106.0;
    word_state state;
    pf_report_t report;
    int k;
    int entry;

    for (k = 0; k < 3; ++k) {
        for (entry = 0; entry < 2; ++entry) {
            pf_native_run_spec_v1 spec = twin_spec();
            double want = 0.0;
            spec.session_key = "native-c-api-word-fee-kind";
            spec.fee_kind = kinds[k];
            spec.fee_value = WORD_FEE;
            if (!word_host(&state, fee_steps, 2)) return;
            CHECK_EQ_INT(configure_base_word(&state, &spec, &spec.fee_kind,
                                             PF_NATIVE_FEE_CASH_PER_EXECUTION + 1u, entry, k == 0,
                                             "a fee kind past pf_native_fee_kind_e was accepted"),
                         PF_NATIVE_OK, "a typed fee kind was refused");
            memset(&report, 0, sizeof(report));
            CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, &report),
                         PF_NATIVE_OK, "the fee-kind run did not complete");
            CHECK_EQ_INT(state.failures, 0, "in-callback fee-kind rows failed");
            switch (kinds[k]) {
            case PF_NATIVE_FEE_PERCENT:
                /* abs(units) x price x point_value x account_fx x fee / 100, per execution */
                want = WORD_UNITS * entry_price * WORD_FEE / 100.0
                       + WORD_UNITS * exit_price * WORD_FEE / 100.0;
                break;
            case PF_NATIVE_FEE_CASH_PER_UNIT:
                want = 2.0 * WORD_UNITS * WORD_FEE;
                break;
            case PF_NATIVE_FEE_CASH_PER_EXECUTION:
                want = 2.0 * WORD_FEE;
                break;
            }
            CHECK_EQ_INT(report.total_trades, 1, "the fee-kind run closed another trade count");
            if (report.total_trades == 1) {
                CHECK(report.trades[0].entry_price == entry_price
                          && report.trades[0].exit_price == exit_price
                          && report.trades[0].qty == WORD_UNITS,
                      "the fee-kind trade is not the scripted one");
                if (fabs(report.trades[0].commission - want) > 1e-9) {
                    fprintf(stderr, "  fee kind %u, entry %d: commission %.12g, want %.12g\n",
                            (unsigned)kinds[k], entry, report.trades[0].commission, want);
                }
                CHECK(fabs(report.trades[0].commission - want) <= 1e-9,
                      "a fee kind charged another rule's commission");
            }
            strategy_native_report_free_v1(&report);
            strategy_native_host_free(state.host);
        }
    }
}

/* --- close_execution: where a close-calculation order may first match --- */

static const word_step close_steps[] = {{3, PF_NATIVE_INTENT_TRANSACT, 1.0}};

static void check_close_execution_word(void) {
    static const pf_native_close_execution_t rules[] = {
        PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT,
        PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION};
    word_state state;
    int k;
    int entry;

    for (k = 0; k < 2; ++k) {
        for (entry = 0; entry < 2; ++entry) {
            pf_native_run_spec_v1 spec = twin_spec();
            /* The order is born in the third close calculation, script bar 2:
             * NEXT_ELIGIBLE_POINT waits for bar 3's open, AFTER_CALCULATION
             * matches at bar 2's own modeled close. */
            const int want = rules[k] == PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION ? 2 : 3;
            spec.session_key = "native-c-api-word-close-execution";
            spec.close_execution = rules[k];
            if (!word_host(&state, close_steps, 1)) return;
            CHECK_EQ_INT(configure_base_word(&state, &spec, &spec.close_execution,
                                             PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION + 1u,
                                             entry, k == 0,
                                             "a close execution past its enumeration was accepted"),
                         PF_NATIVE_OK, "a typed close execution was refused");
            CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, NULL),
                         PF_NATIVE_OK, "the close-execution run did not complete");
            CHECK_EQ_INT(state.failures, 0, "in-callback close-execution rows failed");
            CHECK_EQ_INT(state.fills, 1, "the close-execution run filled another count");
            CHECK_EQ_INT(state.first_fill_interval, want,
                         "a close-calculation order matched where another rule would");
            strategy_native_host_free(state.host);
        }
    }
}

/* --- allowed_open_directions: which openings the run admits --- */

static const word_step direction_steps[] = {
    {3, PF_NATIVE_INTENT_TRANSACT, 1.0},    /* a long opening */
    {6, PF_NATIVE_INTENT_FLATTEN, 0.0},
    {9, PF_NATIVE_INTENT_TRANSACT, -1.0},   /* a short opening */
    {12, PF_NATIVE_INTENT_FLATTEN, 0.0}};

/* Walks the whole event history a page at a time and answers how many
 * events are of `kind`, copying the first `cap` of them into `out`. */
static int page_events(pf_strategy_t host, uint32_t kind, pf_native_event_v1* out, int cap) {
    static pf_native_event_v1 page[32];
    uint64_t cursor = 0;
    int count = 0;
    int written;
    int i;
    for (;;) {
        memset(page, 0, sizeof(page));
        written = strategy_native_events_v1(host, cursor, page, 32);
        CHECK(written >= 0, "a word scenario's event history did not read");
        if (written <= 0) break;
        for (i = 0; i < written; ++i) {
            if (page[i].kind != kind) continue;
            if (count < cap) out[count] = page[i];
            ++count;
        }
        cursor = page[written - 1].ordinal;
    }
    return count;
}

static int count_events(pf_strategy_t host, uint32_t kind) {
    return page_events(host, kind, NULL, 0);
}

static void check_open_directions_word(void) {
    static const pf_native_open_directions_t admitted[] = {
        PF_NATIVE_OPEN_DIRECTIONS_NONE, PF_NATIVE_OPEN_DIRECTIONS_LONG,
        PF_NATIVE_OPEN_DIRECTIONS_SHORT, PF_NATIVE_OPEN_DIRECTIONS_BOTH};
    word_state state;
    int k;
    int entry;

    for (k = 0; k < 4; ++k) {
        for (entry = 0; entry < 2; ++entry) {
            pf_native_run_spec_v1 spec = twin_spec();
            const int want_long = admitted[k] == PF_NATIVE_OPEN_DIRECTIONS_LONG
                                  || admitted[k] == PF_NATIVE_OPEN_DIRECTIONS_BOTH;
            const int want_short = admitted[k] == PF_NATIVE_OPEN_DIRECTIONS_SHORT
                                   || admitted[k] == PF_NATIVE_OPEN_DIRECTIONS_BOTH;
            spec.session_key = "native-c-api-word-open-directions";
            spec.allowed_open_directions = admitted[k];
            if (!word_host(&state, direction_steps, 4)) return;
            CHECK_EQ_INT(configure_base_word(&state, &spec, &spec.allowed_open_directions,
                                             PF_NATIVE_OPEN_DIRECTIONS_BOTH + 1u, entry, k == 0,
                                             "an opening mask past its enumeration was accepted"),
                         PF_NATIVE_OK, "a typed opening mask was refused");
            CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, NULL),
                         PF_NATIVE_OK, "the open-directions run did not complete");
            CHECK_EQ_INT(state.failures, 0, "in-callback open-directions rows failed");
            CHECK_EQ_INT(state.long_openings, want_long, "a long opening met another rule");
            CHECK_EQ_INT(state.short_openings, want_short, "a short opening met another rule");
            /* A refused opening is a match rejection, not a silent drop. */
            CHECK_EQ_INT(count_events(state.host, PF_NATIVE_EVENT_MATCH_REJECTED) > 0,
                         !(want_long && want_short),
                         "an opening was match-rejected against its direction mask");
            strategy_native_host_free(state.host);
        }
    }
}

/* --- report_policy: who records the per-bar report series --- */

static void check_report_policy_word(void) {
    static const pf_native_report_policy_t policies[] = {
        PF_NATIVE_REPORT_HOST_RECORDED, PF_NATIVE_REPORT_KERNEL_RECORDED};
    word_state state;
    pf_report_t report;
    int rc;
    int k;

    for (k = 0; k < 2; ++k) {
        pf_native_run_spec_v1 spec = twin_spec();
        pf_native_run_spec_ext_v1 ext = word_ext(PF_NATIVE_SPEC_EXT_REPORT);
        spec.session_key = "native-c-api-word-report-policy";
        ext.report_policy = policies[k];
        if (!word_host(&state, fee_steps, 2)) return;
        if (k == 0) {
            /* One past the end is also the kernel's third policy,
             * KernelRecordedAtHostMarks, which has no C name: its host marks
             * the report points, and no C callback can mark one. */
            rc = configure_past_refusal(state.host, &spec, &ext, &ext.report_policy,
                                        PF_NATIVE_REPORT_KERNEL_RECORDED + 1u, 0,
                                        "a report policy past its enumeration was accepted");
        } else {
            rc = strategy_configure_native_ext_v1(state.host, &spec, &ext);
        }
        CHECK_EQ_INT(rc, PF_NATIVE_OK, "a typed report policy was refused");
        memset(&report, 0, sizeof(report));
        CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, &report),
                     PF_NATIVE_OK, "the report-policy run did not complete");
        CHECK_EQ_INT(state.failures, 0, "in-callback report-policy rows failed");
        CHECK_EQ_INT(report.script_bars_processed, TWIN_BARS,
                     "the report-policy run calculated another bar count");
        /* HOST_RECORDED leaves the series to the host, which records nothing
         * here; KERNEL_RECORDED marks one point per script calculation. */
        CHECK_EQ_INT(report.equity_curve_len,
                     policies[k] == PF_NATIVE_REPORT_KERNEL_RECORDED ? TWIN_BARS : 0,
                     "a report policy recorded another rule's series");
        strategy_native_report_free_v1(&report);
        strategy_native_host_free(state.host);
    }
}

/* --- price_grid / grid_rounding: the instrument tick ladder --- */

#define GRID_BARS  6
#define GRID_FILLS 5
#define GRID_MS    (15LL * 60LL * 1000LL)

/* examples/native/native_price_grid_c.c's tape: a 0.25 ladder under a feed
 * whose every print is sub-tick. */
static const pf_bar_t grid_bars[GRID_BARS] = {
    {100.00, 100.20,  99.90, 100.10, 10.0, 0 * GRID_MS},
    {100.10, 100.30, 100.05, 100.20, 10.0, 1 * GRID_MS},
    {100.20, 100.80, 100.15, 100.70, 10.0, 2 * GRID_MS},
    { 99.40,  99.45,  99.20,  99.30, 10.0, 3 * GRID_MS},
    { 99.45,  99.65,  99.35,  99.60, 10.0, 4 * GRID_MS},
    { 99.60,  99.62,  99.30,  99.40, 10.0, 5 * GRID_MS}};

typedef struct grid_state {
    pf_strategy_t host;
    int           calculations;
    int           failures;
} grid_state;

static void grid_submit(grid_state* state, uint32_t intent, double value, uint32_t trigger,
                        double price) {
    pf_native_request_v1 request = blank_request();
    request.intent = intent;
    request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
    request.intent_value = value;
    request.trigger = trigger;
    request.p1 = price;
    request.label = "grid-word";
    LCHECK(state, strategy_native_submit_v1(state->host, &request, NULL, NULL) == PF_NATIVE_OK,
           "a grid-word command was refused");
}

static int grid_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    grid_state* state = (grid_state*)user;
    double units = 0.0;
    (void)bar;
    (void)at;
    ++state->calculations;
    if (state->calculations == 1) {
        /* entry: the market fill at bar 1's open 100.10 */
        grid_submit(state, PF_NATIVE_INTENT_TRANSACT, 2.0, PF_NATIVE_TRIGGER_MARKET, 0.0);
    } else if (state->calculations == 2) {
        /* a limit at the ladder price 100.75, and a stop at 99.50 that the
         * open 99.40 gaps through */
        grid_submit(state, PF_NATIVE_INTENT_REDUCE, 1.0, PF_NATIVE_TRIGGER_LIMIT, 100.75);
        grid_submit(state, PF_NATIVE_INTENT_REDUCE, 1.0, PF_NATIVE_TRIGGER_STOP, 99.50);
    } else if (state->calculations == 4) {
        /* a stop at 99.75 only the quantized path reaches: the raw high is 99.65 */
        grid_submit(state, PF_NATIVE_INTENT_TRANSACT, 1.0, PF_NATIVE_TRIGGER_STOP, 99.75);
    } else if (state->calculations == 5) {
        if (strategy_native_position_v1(state->host, &units, NULL, NULL) == PF_NATIVE_OK
            && units > 0.0) {
            grid_submit(state, PF_NATIVE_INTENT_FLATTEN, 0.0, PF_NATIVE_TRIGGER_MARKET, 0.0);
        }
    }
    return 0;
}

/* Runs the tape under one grid and one rounding. `refuse` 1 refuses a price
 * grid, 2 a grid rounding, one past its enumeration first, on the same
 * handle; 0 refuses nothing. Answers the fill count, and the first fills' raw
 * and booked prices in `raw` / `booked`. */
static int run_grid_tape(pf_native_price_grid_t grid, pf_native_grid_rounding_t rounding,
                         int refuse, double* raw, double* booked) {
    pf_native_event_v1 applied[GRID_FILLS];
    pf_native_run_spec_v1 spec = twin_spec();
    pf_native_run_spec_ext_v1 ext = word_ext(PF_NATIVE_SPEC_EXT_PRICE_GRID);
    pf_native_callbacks_v1 table;
    grid_state state;
    int fills;
    int rc;
    int i;

    memset(&state, 0, sizeof(state));
    table = blank_callbacks(&state);
    table.on_bar = grid_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    CHECK(state.host != NULL, "grid-word host create failed");
    if (!state.host) return -1;
    spec.session_key = "native-c-api-word-price-grid";
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.price_tick = 0.25;
    ext.price_grid = grid;
    ext.grid_rounding = rounding;
    if (refuse == 1) {
        rc = configure_past_refusal(state.host, &spec, &ext, &ext.price_grid,
                                    PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS + 1u, 0,
                                    "a price grid past its enumeration was accepted");
    } else if (refuse == 2) {
        rc = configure_past_refusal(state.host, &spec, &ext, &ext.grid_rounding,
                                    PF_NATIVE_GRID_ROUNDING_DIRECTIONAL + 1u, 0,
                                    "a grid rounding past its enumeration was accepted");
    } else {
        rc = strategy_configure_native_ext_v1(state.host, &spec, &ext);
    }
    CHECK_EQ_INT(rc, PF_NATIVE_OK, "a typed price grid or grid rounding was refused");
    CHECK_EQ_INT(strategy_native_run_v1(state.host, grid_bars, GRID_BARS, NULL), PF_NATIVE_OK,
                 "the grid-word run did not complete");
    CHECK_EQ_INT(state.failures, 0, "in-callback grid-word rows failed");
    memset(applied, 0, sizeof(applied));
    fills = page_events(state.host, PF_NATIVE_EVENT_APPLIED, applied, GRID_FILLS);
    for (i = 0; i < fills && i < GRID_FILLS; ++i) {
        raw[i] = applied[i].raw_price;
        booked[i] = applied[i].resolved_price;
    }
    strategy_native_host_free(state.host);
    return fills;
}

static int grid_fills_are(int fills, const double* raw, const double* booked, int want_fills,
                          const double* want_raw, const double* want_booked) {
    int i;
    if (fills != want_fills) return 0;
    for (i = 0; i < fills; ++i) {
        if (raw[i] != want_raw[i] || booked[i] != want_booked[i]) return 0;
    }
    return 1;
}

static void check_price_grid_word(void) {
    /* The hand-computed fills of examples/native/native_price_grid_c.c, all
     * under HALF_UP: the market entry at 100.10, the limit at the ladder
     * price 100.75 (a fixed point), the stop gapped through at 99.40, then --
     * on the quantized path only -- the stop at 99.75 and its exit at 99.60. */
    static const pf_native_price_grid_t grids[] = {
        PF_NATIVE_PRICE_GRID_NONE, PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS,
        PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS};
    static const int want_fills[] = {3, 3, 5};
    static const double want_raw[3][GRID_FILLS] = {
        {100.10, 100.75, 99.40}, {100.10, 100.75, 99.40}, {100.10, 100.75, 99.40, 99.75, 99.60}};
    static const double want_booked[3][GRID_FILLS] = {
        {100.10, 100.75, 99.40}, {100.00, 100.75, 99.50}, {100.00, 100.75, 99.50, 99.75, 99.50}};
    double raw[GRID_FILLS];
    double booked[GRID_FILLS];
    int fills;
    int k;

    for (k = 0; k < 3; ++k) {
        memset(raw, 0, sizeof(raw));
        memset(booked, 0, sizeof(booked));
        fills = run_grid_tape(grids[k], PF_NATIVE_GRID_ROUNDING_HALF_UP, k == 0 ? 1 : 0, raw,
                              booked);
        if (!grid_fills_are(fills, raw, booked, want_fills[k], want_raw[k], want_booked[k])) {
            fprintf(stderr, "  price grid %u: %d fills, first booked %.2f\n", (unsigned)grids[k],
                    fills, booked[0]);
        }
        CHECK(grid_fills_are(fills, raw, booked, want_fills[k], want_raw[k], want_booked[k]),
              "a price grid booked or triggered by another rule");
    }
}

static void check_grid_rounding_word(void) {
    /* QUANTIZE_FILLS under both roundings: the market buy at 100.10 books the
     * nearest tick or the adverse buy tick, the gapped sell stop at 99.40 the
     * nearest or the adverse sell tick; the ladder limit is a fixed point. */
    static const pf_native_grid_rounding_t roundings[] = {
        PF_NATIVE_GRID_ROUNDING_HALF_UP, PF_NATIVE_GRID_ROUNDING_DIRECTIONAL};
    static const double want_raw[GRID_FILLS] = {100.10, 100.75, 99.40};
    static const double want_booked[2][GRID_FILLS] = {
        {100.00, 100.75, 99.50}, {100.25, 100.75, 99.25}};
    double raw[GRID_FILLS];
    double booked[GRID_FILLS];
    int fills;
    int k;

    for (k = 0; k < 2; ++k) {
        memset(raw, 0, sizeof(raw));
        memset(booked, 0, sizeof(booked));
        fills = run_grid_tape(PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS, roundings[k], k == 0 ? 2 : 0,
                              raw, booked);
        if (!grid_fills_are(fills, raw, booked, 3, want_raw, want_booked[k])) {
            fprintf(stderr, "  grid rounding %u: %d fills, first booked %.2f\n",
                    (unsigned)roundings[k], fills, booked[0]);
        }
        CHECK(grid_fills_are(fills, raw, booked, 3, want_raw, want_booked[k]),
              "a grid rounding booked by another rule");
    }
}

/* --- calculation: when the kernel asks the host to calculate --- */

static const word_step calc_steps[] = {{4, PF_NATIVE_INTENT_TRANSACT, 1.0}};

static int word_on_recalculate(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at,
                               uint32_t reason, const pf_native_applied_v1* cause) {
    word_state* state = (word_state*)user;
    (void)bar;
    (void)at;
    (void)cause;
    LCHECK(state, reason <= PF_NATIVE_CALC_SUB_BAR, "a recalculation carried an unknown reason");
    if (reason <= PF_NATIVE_CALC_SUB_BAR) ++state->recalculations[reason];
    if (reason == PF_NATIVE_CALC_BAR_CLOSE) word_run_steps(state);
    return 0;
}

static void check_calc_trigger_word(void) {
    static const pf_native_calc_trigger_t triggers[] = {
        PF_NATIVE_CALC_TRIGGER_BAR_CLOSE, PF_NATIVE_CALC_TRIGGER_BAR_CLOSE_AND_FILLS,
        PF_NATIVE_CALC_TRIGGER_EVERY_MODELED_POINT};
    word_state state;
    pf_native_callbacks_v1 table;
    uint64_t driven;
    int rc;
    int k;

    for (k = 0; k < 3; ++k) {
        pf_native_run_spec_v1 spec = twin_spec();
        pf_native_run_spec_ext_v1 ext = word_ext(PF_NATIVE_SPEC_EXT_CALCULATION);
        memset(&state, 0, sizeof(state));
        state.steps = calc_steps;
        state.n_steps = 1;
        table = blank_callbacks(&state);
        table.on_recalculate = word_on_recalculate;
        table.on_applied = word_on_applied;
        state.host = strategy_native_host_create_v1(&table);
        CHECK(state.host != NULL, "calculation-word host create failed");
        if (!state.host) return;
        spec.session_key = "native-c-api-word-calculation";
        ext.calculation = triggers[k];
        ext.max_recalculations_per_point = 4u;
        if (k == 0) {
            rc = configure_past_refusal(state.host, &spec, &ext, &ext.calculation,
                                        PF_NATIVE_CALC_TRIGGER_EVERY_MODELED_POINT + 1u, 0,
                                        "a calculation trigger past its enumeration was accepted");
        } else {
            rc = strategy_configure_native_ext_v1(state.host, &spec, &ext);
        }
        CHECK_EQ_INT(rc, PF_NATIVE_OK, "a typed calculation trigger was refused");
        CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, NULL),
                     PF_NATIVE_OK, "the calculation-word run did not complete");
        CHECK_EQ_INT(state.failures, 0, "in-callback calculation-word rows failed");
        CHECK_EQ_INT(state.fills, 1, "the calculation-word entry did not fill once");
        driven = 0;
        CHECK_EQ_INT(strategy_native_recalculations_v1(state.host, &driven, NULL), PF_NATIVE_OK,
                     "calculation-word recalculation counters");
        /* Every trigger keeps the close calculation; each one above BAR_CLOSE
         * adds the fill's recalculation, and EVERY_MODELED_POINT the modeled
         * points'. BAR_CLOSE drives no recalculation at all. */
        CHECK_EQ_INT(state.recalculations[PF_NATIVE_CALC_BAR_CLOSE], TWIN_BARS,
                     "a calculation trigger lost the close calculation");
        CHECK_EQ_INT(state.recalculations[PF_NATIVE_CALC_ORDER_FILL] > 0,
                     triggers[k] != PF_NATIVE_CALC_TRIGGER_BAR_CLOSE,
                     "a fill was recalculated against its calculation trigger");
        CHECK_EQ_INT(state.recalculations[PF_NATIVE_CALC_TICK] > 0,
                     triggers[k] == PF_NATIVE_CALC_TRIGGER_EVERY_MODELED_POINT,
                     "a modeled point was recalculated against its calculation trigger");
        CHECK_EQ_INT(driven > 0, triggers[k] != PF_NATIVE_CALC_TRIGGER_BAR_CLOSE,
                     "the driven recalculation count contradicts the calculation trigger");
        strategy_native_host_free(state.host);
    }
}

/* --- open_bar_view: what on_bar_open is handed --- */

static int word_on_bar_open(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    word_state* state = (word_state*)user;
    const pf_bar_t* feed = pf_twin_bars(NULL);
    const int64_t i = bar->timestamp / 300000;
    (void)at;
    ++state->bar_opens;
    LCHECK(state, i >= 0 && i < TWIN_BARS && bar->timestamp % 300000 == 0,
           "on_bar_open was handed a bar outside the feed");
    if (i < 0 || i >= TWIN_BARS) return 0;
    if (bar->open == feed[i].open && bar->high == feed[i].high && bar->low == feed[i].low
        && bar->close == feed[i].close && bar->volume == feed[i].volume) {
        ++state->complete_opens;
    }
    if (bar->open == feed[i].open && bar->high == feed[i].open && bar->low == feed[i].open
        && bar->close == feed[i].open && bar->volume == 0.0) {
        ++state->open_only_opens;
    }
    return 0;
}

static void check_open_bar_view_word(void) {
    static const pf_native_open_bar_view_t views[] = {
        PF_NATIVE_OPEN_BAR_VIEW_COMPLETE, PF_NATIVE_OPEN_BAR_VIEW_OPEN_ONLY};
    word_state state;
    pf_native_callbacks_v1 table;
    int rc;
    int k;

    for (k = 0; k < 2; ++k) {
        pf_native_run_spec_v1 spec = twin_spec();
        pf_native_run_spec_ext_v1 ext = word_ext(PF_NATIVE_SPEC_EXT_OPEN_BAR_VIEW);
        memset(&state, 0, sizeof(state));
        table = blank_callbacks(&state);
        table.on_bar_open = word_on_bar_open;
        state.host = strategy_native_host_create_v1(&table);
        CHECK(state.host != NULL, "open-bar-view host create failed");
        if (!state.host) return;
        spec.session_key = "native-c-api-word-open-bar-view";
        ext.open_bar_view = views[k];
        if (k == 0) {
            rc = configure_past_refusal(state.host, &spec, &ext, &ext.open_bar_view,
                                        PF_NATIVE_OPEN_BAR_VIEW_OPEN_ONLY + 1u, 0,
                                        "an open-bar view past its enumeration was accepted");
        } else {
            rc = strategy_configure_native_ext_v1(state.host, &spec, &ext);
        }
        CHECK_EQ_INT(rc, PF_NATIVE_OK, "a typed open-bar view was refused");
        CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, NULL),
                     PF_NATIVE_OK, "the open-bar-view run did not complete");
        CHECK_EQ_INT(state.failures, 0, "in-callback open-bar-view rows failed");
        CHECK_EQ_INT(state.bar_opens, TWIN_BARS, "on_bar_open ran another bar count");
        /* COMPLETE hands over the whole script bar; OPEN_ONLY masks its
         * lookahead: H = L = C = open and no volume. The twin feed's bars are
         * never flat, so no bar can satisfy both. */
        CHECK_EQ_INT(state.complete_opens,
                     views[k] == PF_NATIVE_OPEN_BAR_VIEW_COMPLETE ? TWIN_BARS : 0,
                     "an open-bar view handed over another view's bar");
        CHECK_EQ_INT(state.open_only_opens,
                     views[k] == PF_NATIVE_OPEN_BAR_VIEW_OPEN_ONLY ? TWIN_BARS : 0,
                     "an open-bar view handed over another view's bar");
        strategy_native_host_free(state.host);
    }
}

/* --- margin_sizing: how many units a kernel liquidation reduces --- */

static const word_step sizing_steps[] = {{3, PF_NATIVE_INTENT_TRANSACT, WORD_HELD}};

/* Four units bought at 103 on a 300 account at 0.5 maintenance: the book's
 * liquidation level solves to 56, a real level far under a rising feed, so
 * every check point consults the requirement hook -- a level that does not
 * solve rests nothing and asks nothing -- while the kernel's own numbers
 * never breach. The one breach is the host's: at the first bar-open check of
 * the held book it answers a requirement exactly one unit's margin above the
 * equity, so the restore is one unit whatever the mark. */
static int sizing_on_requirement(void* user, const pf_native_margin_view_v1* view,
                                 pf_native_margin_decision_v1* out) {
    word_state* state = (word_state*)user;
    if (state->answered || view->kind != PF_NATIVE_MARGIN_CHECK_BAR_OPEN
        || !(view->signed_units > 0.0)) {
        return PF_NATIVE_ANSWER_DEFAULT;
    }
    state->answered = 1;
    out->equity = view->equity;
    out->required = view->equity + view->mark * 1.0 * 1.0 * WORD_MAINTENANCE;
    out->force_breach = 0u;
    return PF_NATIVE_ANSWER_PROVIDED;
}

static int sizing_on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    word_state* state = (word_state*)user;
    pf_native_working_v1 working;
    int len;
    int i;
    word_run_steps(state);
    (void)bar;
    (void)at;
    if (!state->answered || state->inspected) return 0;
    /* The breach rested a kernel-originated reduction at the solved level
     * below a rising feed: it is still on the book here. */
    state->inspected = 1;
    len = strategy_native_working_len_v1(state->host);
    for (i = 0; i < len; ++i) {
        memset(&working, 0, sizeof(working));
        working.struct_size = (uint32_t)sizeof(working);
        if (strategy_native_working_get_v1(state->host, i, &working) != PF_NATIVE_OK) continue;
        if (working.origin != 1u) continue;
        ++state->liquidation_rows;
        state->liquidation_intent = working.intent;
        state->liquidation_units = working.intent_value;
    }
    return 0;
}

static void check_liquidation_sizing_word(void) {
    static const pf_native_liquidation_sizing_t sizings[] = {
        PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM,
        PF_NATIVE_LIQUIDATION_SIZING_SHORTFALL_MULTIPLE, PF_NATIVE_LIQUIDATION_SIZING_FLATTEN};
    /* RESTORE_MINIMUM reduces the one-unit restore, SHORTFALL_MULTIPLE that
     * restore times the multiple; FLATTEN takes the whole book, which the
     * kernel spells as a Flatten request with no scalar of its own. */
    static const uint32_t want_intent[] = {
        PF_NATIVE_INTENT_REDUCE, PF_NATIVE_INTENT_REDUCE, PF_NATIVE_INTENT_FLATTEN};
    static const double want_units[] = {1.0, WORD_MULTIPLE, 0.0};
    word_state state;
    pf_native_callbacks_v1 table;
    int rc;
    int k;

    for (k = 0; k < 3; ++k) {
        pf_native_run_spec_v1 spec = twin_spec();
        pf_native_run_spec_ext_v1 ext = word_ext(PF_NATIVE_SPEC_EXT_MARGIN);
        memset(&state, 0, sizeof(state));
        state.steps = sizing_steps;
        state.n_steps = 1;
        table = blank_callbacks(&state);
        table.on_bar = sizing_on_bar;
        table.on_margin_requirement = sizing_on_requirement;
        state.host = strategy_native_host_create_v1(&table);
        CHECK(state.host != NULL, "sizing-word host create failed");
        if (!state.host) return;
        spec.session_key = "native-c-api-word-margin-sizing";
        spec.initial_capital = WORD_CAPITAL;
        ext.margin_has_maintenance_long = 1u;
        ext.margin_maintenance_long = WORD_MAINTENANCE;
        ext.margin_has_maintenance_short = 1u;
        ext.margin_maintenance_short = WORD_MAINTENANCE;
        ext.margin_sizing = sizings[k];
        ext.margin_shortfall_multiple = WORD_MULTIPLE;
        ext.margin_check = PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME;
        if (k == 0) {
            rc = configure_past_refusal(state.host, &spec, &ext, &ext.margin_sizing,
                                        PF_NATIVE_LIQUIDATION_SIZING_FLATTEN + 1u, 0,
                                        "a liquidation sizing past its enumeration was accepted");
        } else {
            rc = strategy_configure_native_ext_v1(state.host, &spec, &ext);
        }
        CHECK_EQ_INT(rc, PF_NATIVE_OK, "a typed liquidation sizing was refused");
        CHECK_EQ_INT(strategy_native_run_v1(state.host, pf_twin_bars(NULL), TWIN_BARS, NULL),
                     PF_NATIVE_OK, "the sizing-word run did not complete");
        CHECK_EQ_INT(state.failures, 0, "in-callback sizing-word rows failed");
        CHECK(state.answered, "the sizing-word breach was never answered");
        CHECK_EQ_INT(state.liquidation_rows, 1,
                     "the sizing-word breach rested no kernel liquidation");
        if (state.liquidation_intent != want_intent[k]
            || fabs(state.liquidation_units - want_units[k]) > 1e-9) {
            fprintf(stderr, "  liquidation sizing %u: intent %u, %.12g units; want %u, %.12g\n",
                    (unsigned)sizings[k], (unsigned)state.liquidation_intent,
                    state.liquidation_units, (unsigned)want_intent[k], want_units[k]);
        }
        CHECK(state.liquidation_intent == want_intent[k]
                  && fabs(state.liquidation_units - want_units[k]) <= 1e-9,
              "a liquidation sizing reduced another rule's units");
        strategy_native_host_free(state.host);
    }
}

/* --- the nine words' layout --- */

static void check_spec_word_layout(void) {
    pf_native_run_spec_v1 spec;
    pf_native_run_spec_ext_v1 ext;
    memset(&spec, 0, sizeof(spec));
    memset(&ext, 0, sizeof(ext));
    /* Typing the words moved nothing: each is still one uint32_t word. Zero
     * is the kernel default of eight of them; allowed_open_directions is the
     * exception, so a zero-filled base spec admits no opening. */
    CHECK(sizeof(spec.fee_kind) == sizeof(uint32_t)
              && sizeof(spec.close_execution) == sizeof(uint32_t)
              && sizeof(spec.allowed_open_directions) == sizeof(uint32_t)
              && sizeof(ext.report_policy) == sizeof(uint32_t)
              && sizeof(ext.price_grid) == sizeof(uint32_t)
              && sizeof(ext.grid_rounding) == sizeof(uint32_t)
              && sizeof(ext.calculation) == sizeof(uint32_t)
              && sizeof(ext.open_bar_view) == sizeof(uint32_t)
              && sizeof(ext.margin_sizing) == sizeof(uint32_t),
          "a typed run-spec word is not one uint32_t word");
    CHECK(spec.fee_kind == PF_NATIVE_FEE_PERCENT
              && spec.close_execution == PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT
              && spec.allowed_open_directions == PF_NATIVE_OPEN_DIRECTIONS_NONE
              && ext.report_policy == PF_NATIVE_REPORT_HOST_RECORDED
              && ext.price_grid == PF_NATIVE_PRICE_GRID_NONE
              && ext.grid_rounding == PF_NATIVE_GRID_ROUNDING_HALF_UP
              && ext.calculation == PF_NATIVE_CALC_TRIGGER_BAR_CLOSE
              && ext.open_bar_view == PF_NATIVE_OPEN_BAR_VIEW_COMPLETE
              && ext.margin_sizing == PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM,
          "a zero-filled run-spec word is not its enumeration's zero");
    /* On LP64, every CI host's data model, the offsets and sizes measured on
     * e9ad37dd, before the enumerations existed. */
    if (sizeof(void*) == 8) {
        CHECK_EQ_INT(offsetof(pf_native_run_spec_v1, fee_kind), 156, "fee_kind moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_v1, close_execution), 172,
                     "close_execution moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_v1, allowed_open_directions), 176,
                     "allowed_open_directions moved");
        CHECK_EQ_INT(sizeof(pf_native_run_spec_v1), 216, "pf_native_run_spec_v1 resized");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_ext_v1, report_policy), 12,
                     "report_policy moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_ext_v1, price_grid), 20, "price_grid moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_ext_v1, grid_rounding), 24,
                     "grid_rounding moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_ext_v1, calculation), 28, "calculation moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_ext_v1, open_bar_view), 36,
                     "open_bar_view moved");
        CHECK_EQ_INT(offsetof(pf_native_run_spec_ext_v1, margin_sizing), 40,
                     "margin_sizing moved");
        CHECK_EQ_INT(sizeof(pf_native_run_spec_ext_v1), 304, "pf_native_run_spec_ext_v1 resized");
    }
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
    check_command_spellings();
    check_callback_tail_layouts();
    check_recalculation_hook();
    check_margin_hooks();
    check_excursion_hook();
    check_entry_bar_mask_declaration();
    check_request_sizing_tail();
    check_trail_best_seed_tail();
    check_scope_basis_tail();
    check_intrabar_and_policies();
    check_feed_policies();
    check_margin_tail_fields();
    check_cohort_roster();
    check_auxiliary_feed();
    check_fx_roll_margin_point();
    check_subscription_delivery_words();
    check_working_arm_presence();
    check_spec_word_layout();
    check_fee_kind_word();
    check_close_execution_word();
    check_open_directions_word();
    check_report_policy_word();
    check_price_grid_word();
    check_grid_rounding_word();
    check_calc_trigger_word();
    check_open_bar_view_word();
    check_liquidation_sizing_word();
    return failures;
}
