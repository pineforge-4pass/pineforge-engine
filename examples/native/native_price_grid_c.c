/* The instrument price grid — from C.
 *
 * The same strategy, the same six bars and the same four broker models as
 * native_price_grid_strategy.cpp, written against the C API: the grid is
 * declared through the run-spec extension (PF_NATIVE_SPEC_EXT_PRICE_GRID,
 * `price_grid` / `grid_rounding`), and every fill is read back from the event
 * history with both of its prices — the raw modeled one and the one the run
 * booked. The expected numbers are the C++ example's, hand-computed from the
 * rounding rules; a C host and a C++ host get the same grid.
 *
 *   cc -std=c11 native_price_grid_c.c -lpineforge_kernel -lstdc++ -o price_grid_c
 */

#include <pineforge/pineforge.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define QUARTER_MS (15LL * 60LL * 1000LL)

/* open, high, low, close, volume, timestamp (Unix milliseconds). The
 * instrument trades on a 0.25 ladder; every print of this feed is sub-tick. */
static const pf_bar_t kBars[] = {
    {100.00, 100.20,  99.90, 100.10, 10.0, 0 * QUARTER_MS},
    {100.10, 100.30, 100.05, 100.20, 10.0, 1 * QUARTER_MS},
    {100.20, 100.80, 100.15, 100.70, 10.0, 2 * QUARTER_MS},
    { 99.40,  99.45,  99.20,  99.30, 10.0, 3 * QUARTER_MS},
    { 99.45,  99.65,  99.35,  99.60, 10.0, 4 * QUARTER_MS},
    { 99.60,  99.62,  99.30,  99.40, 10.0, 5 * QUARTER_MS}
};
enum { kBarCount = 6, kMaxFills = 5, kMaxEvents = 128 };

struct host_state {
    pf_strategy_t handle;
    int           bars;
    int           refused;
};

static void submit(struct host_state* state, uint32_t intent, double value,
                   uint32_t trigger, double price, const char* label) {
    pf_native_request_v1 request;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.version = PF_NATIVE_API_VERSION;
    request.intent = intent;
    request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
    request.intent_value = value;
    request.trigger = trigger;
    request.p1 = price;
    request.label = label;
    request.comment = "grid";
    if (strategy_native_submit_v1(state->handle, &request, NULL, NULL) != PF_NATIVE_OK) {
        ++state->refused;
    }
}

static int on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    struct host_state* state = (struct host_state*)user;
    double units = 0.0;
    (void)bar;
    (void)at;

    ++state->bars;
    if (state->bars == 1) {
        submit(state, PF_NATIVE_INTENT_TRANSACT, 2.0, PF_NATIVE_TRIGGER_MARKET, 0.0, "entry");
    } else if (state->bars == 2) {
        /* Both levels are ladder prices. */
        submit(state, PF_NATIVE_INTENT_REDUCE, 1.0, PF_NATIVE_TRIGGER_LIMIT, 100.75, "target");
        submit(state, PF_NATIVE_INTENT_REDUCE, 1.0, PF_NATIVE_TRIGGER_STOP, 99.50, "protect");
    } else if (state->bars == 4) {
        submit(state, PF_NATIVE_INTENT_TRANSACT, 1.0, PF_NATIVE_TRIGGER_STOP, 99.75, "breakout");
    } else if (state->bars == 5) {
        if (strategy_native_position_v1(state->handle, &units, NULL, NULL) == PF_NATIVE_OK
            && units > 0.0) {
            submit(state, PF_NATIVE_INTENT_FLATTEN, 0.0, PF_NATIVE_TRIGGER_MARKET, 0.0,
                   "breakout-exit");
        }
    }
    return 0;
}

static pf_native_run_spec_v1 make_spec(const char* key, double tick) {
    pf_native_run_spec_v1 spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = (uint32_t)sizeof(spec);
    spec.session_key = key;
    spec.run_number = 1;
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "futures";
    spec.currency = "USD";
    spec.basecurrency = "";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = tick;
    spec.fee_kind = PF_NATIVE_FEE_PERCENT;
    spec.fee_value = 0.0;
    spec.close_execution = PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    return spec;
}

static pf_native_run_spec_ext_v1 make_ext(pf_native_price_grid_t grid,
                                          pf_native_grid_rounding_t rounding) {
    pf_native_run_spec_ext_v1 ext;
    memset(&ext, 0, sizeof(ext));
    ext.struct_size = (uint32_t)sizeof(ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_PRICE_GRID;
    ext.price_grid = (uint32_t)grid;
    ext.grid_rounding = (uint32_t)rounding;
    return ext;
}

struct mode {
    const char*               name;
    pf_native_price_grid_t    grid;
    pf_native_grid_rounding_t rounding;
    int         fills;
    double      raw[kMaxFills];     /* the modeled price of the path, never rounded */
    double      booked[kMaxFills];  /* hand-computed from the rounding rule */
    int         trades;
};

/* entry: the market fill at bar 1's open 100.10 (nearest tick 100.00, adverse
 * buy tick 100.25); target: the limit at the ladder price 100.75, a fixed
 * point of every mode; protect: the stop at 99.50 gapped through by the open
 * 99.40 (nearest 99.50, adverse sell tick 99.25); breakout: the stop at 99.75
 * that only the quantized path reaches (raw high 99.65, nearest tick 99.75),
 * and its market exit at the open 99.60 (nearest 99.50). */
static const struct mode kModes[] = {
    {"None", PF_NATIVE_PRICE_GRID_NONE, PF_NATIVE_GRID_ROUNDING_HALF_UP, 3,
     {100.10, 100.75, 99.40}, {100.10, 100.75, 99.40}, 2},
    {"QuantizeFills/HalfUp", PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS,
     PF_NATIVE_GRID_ROUNDING_HALF_UP, 3,
     {100.10, 100.75, 99.40}, {100.00, 100.75, 99.50}, 2},
    {"QuantizeFills/Directional", PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS,
     PF_NATIVE_GRID_ROUNDING_DIRECTIONAL, 3,
     {100.10, 100.75, 99.40}, {100.25, 100.75, 99.25}, 2},
    {"QuantizeFillsAndTriggers/HalfUp", PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS,
     PF_NATIVE_GRID_ROUNDING_HALF_UP, 5,
     {100.10, 100.75, 99.40, 99.75, 99.60}, {100.00, 100.75, 99.50, 99.75, 99.50}, 3}
};
enum { kModeCount = 4 };

/* Returns the closed-trade count, or -1 when a number is not the expected one. */
static int run_mode(const struct mode* mode) {
    static pf_native_event_v1 events[kMaxEvents];
    struct host_state state;
    pf_native_callbacks_v1 callbacks;
    pf_native_run_spec_v1 spec = make_spec(mode->name, 0.25);
    pf_native_run_spec_ext_v1 ext = make_ext(mode->grid, mode->rounding);
    pf_report_t report;
    int written;
    int fills = 0;
    int trades;
    int ok = 1;
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
        fprintf(stderr, "%s create: the runtime refused the callback table\n", mode->name);
        return -1;
    }
    if (strategy_configure_native_ext_v1(state.handle, &spec, &ext) != PF_NATIVE_OK) {
        fprintf(stderr, "%s configure: %s\n", mode->name, strategy_get_last_error(state.handle));
        strategy_native_host_free(state.handle);
        return -1;
    }
    if (strategy_native_run_v1(state.handle, kBars, kBarCount, &report) != PF_NATIVE_OK) {
        fprintf(stderr, "%s run: %s\n", mode->name, strategy_get_last_error(state.handle));
        strategy_native_report_free_v1(&report);
        strategy_native_host_free(state.handle);
        return -1;
    }

    printf("%-34s", mode->name);
    memset(events, 0, sizeof(events));
    written = strategy_native_events_v1(state.handle, 0, events, kMaxEvents);
    for (i = 0; i < written; ++i) {
        if (events[i].kind != PF_NATIVE_EVENT_APPLIED) continue;
        printf(" %.2f->%.2f", events[i].raw_price, events[i].resolved_price);
        if (fills < mode->fills
            && (events[i].raw_price != mode->raw[fills]
                || events[i].resolved_price != mode->booked[fills])) {
            ok = 0;
        }
        /* Under a grid every booked price is a ladder price. */
        if (mode->grid != PF_NATIVE_PRICE_GRID_NONE
            && events[i].resolved_price != round(events[i].resolved_price / 0.25) * 0.25) {
            ok = 0;
        }
        ++fills;
    }
    trades = report.total_trades;
    printf(" | trades=%d\n", trades);
    if (fills != mode->fills || trades != mode->trades || state.refused != 0) ok = 0;

    strategy_native_report_free_v1(&report);
    strategy_native_host_free(state.handle);
    if (!ok) {
        fprintf(stderr, "%s: the fills are not the hand-computed ones\n", mode->name);
        return -1;
    }
    return trades;
}

/* A quantizing grid needs a ladder: the same extension over price_tick = 0 is
 * refused at configure time, and the handle stays unconfigured. */
static int grid_without_a_ladder_is_refused(void) {
    struct host_state state;
    pf_native_callbacks_v1 callbacks;
    pf_native_run_spec_v1 spec = make_spec("no-ladder", 0.0);
    pf_native_run_spec_ext_v1 ext =
        make_ext(PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS, PF_NATIVE_GRID_ROUNDING_HALF_UP);
    int rc;

    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = (uint32_t)sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &state;
    callbacks.on_bar = on_bar;
    state.handle = strategy_native_host_create_v1(&callbacks);
    if (!state.handle) return 0;
    rc = strategy_configure_native_ext_v1(state.handle, &spec, &ext);
    printf("QuantizeFills with price_tick = 0: %s\n",
           rc != PF_NATIVE_OK ? "refused at configure" : "NOT refused");
    strategy_native_host_free(state.handle);
    return rc != PF_NATIVE_OK;
}

int main(void) {
    int closed = 0;
    int i;

    for (i = 0; i < kModeCount; ++i) {
        const int trades = run_mode(&kModes[i]);
        if (trades < 0) return 1;
        closed += trades;
    }
    if (!grid_without_a_ladder_is_refused()) return 1;

    printf("closed trades: %d\n", closed);
    return closed > 0 ? 0 : 1;
}
