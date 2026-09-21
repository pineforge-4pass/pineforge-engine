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

    /* Three published request layouts. */
    CHECK(PF_NATIVE_REQUEST_V1_BASE_SIZE < PF_NATIVE_REQUEST_V1_ANCHOR_SIZE,
          "the anchored-leg tail is not past the base layout");
    CHECK(PF_NATIVE_REQUEST_V1_ANCHOR_SIZE < (uint32_t)sizeof(pf_native_request_v1),
          "the sizing tail is not past the anchored-leg layout");

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
    check_request_sizing_tail();
    check_scope_basis_tail();
    check_intrabar_and_policies();
    check_feed_policies();
    check_margin_tail_fields();
    check_cohort_roster();
    check_auxiliary_feed();
    return failures;
}
