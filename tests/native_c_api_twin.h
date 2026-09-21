/*
 * native_c_api_twin.h — the record the L13 twin compares.
 *
 * tests/test_native_c_api.c drives the scenario through the C API declared in
 * <pineforge/native_c_api.h>; tests/test_native_c_api_twin.cpp drives the same
 * bars through NativeStrategyHost directly and compares the two records field
 * by field. The header is plain C so the pure-C half really is compiled by the
 * C compiler.
 */

#ifndef PINEFORGE_TEST_NATIVE_C_API_TWIN_H
#define PINEFORGE_TEST_NATIVE_C_API_TWIN_H

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PF_TWIN_MAX_TRADES 8
#define PF_TWIN_MAX_EVENTS 256

/* 1-based calculation indices of the twin scenario's two commands. */
#define PF_TWIN_ENTRY_BAR 10
#define PF_TWIN_EXIT_BAR  30

/* The pyramid arm (R5 gap lane N18): three long lots, a partial FIFO reduce,
 * a reversal into a short, a flatten — and four observations of the open-lot
 * snapshot, marked at the bar's close, at the calculations below. Under the
 * twin spec's NextEligiblePoint close execution a market request submitted
 * at calculation k fills at the open of bar k + 1, before that bar's
 * calculation, so each observation sees the book the previous command left. */
#define PF_TWIN_PYRAMID_L1       10  /* Transact +1  "L1" / "first"  */
#define PF_TWIN_PYRAMID_L2       12  /* Transact +2  "L2" / "second" */
#define PF_TWIN_PYRAMID_L3       14  /* Transact +1  "L3" / "third"  */
#define PF_TWIN_PYRAMID_PARTIAL  18  /* Reduce 1.5   "partial"       */
#define PF_TWIN_PYRAMID_REVERSE  22  /* ReverseTo -1 "REV" / "flip"  */
#define PF_TWIN_PYRAMID_FLAT     30  /* Flatten      "flat"          */
#define PF_TWIN_PYRAMID_OBSERVE_A 16 /* three long lots               */
#define PF_TWIN_PYRAMID_OBSERVE_B 20 /* L2 (1.5 left) and L3          */
#define PF_TWIN_PYRAMID_OBSERVE_C 24 /* the short lot, new cycle      */
#define PF_TWIN_PYRAMID_OBSERVE_D 31 /* flat: no rows                 */
#define PF_TWIN_OBSERVATIONS 4
#define PF_TWIN_MAX_LOTS 4
#define PF_TWIN_TEXT 32
/* One cash ticket per execution in the pyramid arm, so the fee-net P&L and
 * the entry fee's partial-realization share are exercised through C. */
#define PF_TWIN_PYRAMID_FEE 2.0

typedef struct pf_twin_lot {
    uint64_t ordinal;
    uint64_t entry_incarnation;
    int64_t  cycle;
    uint32_t side;
    int32_t  entry_bar_index;
    int64_t  entry_time_ms;
    double   entry_price;
    double   signed_units;
    double   entry_commission;
    double   mark;
    double   unrealized_pnl;
    double   favorable_excursion;
    double   adverse_excursion;
    char     entry_label[PF_TWIN_TEXT];
    char     entry_comment[PF_TWIN_TEXT];
} pf_twin_lot;

typedef struct pf_twin_observation {
    int         calculation;
    int         count;              /* rows the snapshot answered (or a negative status) */
    pf_twin_lot lots[PF_TWIN_MAX_LOTS];
} pf_twin_observation;

typedef struct pf_twin_trade {
    double  entry_price;
    double  exit_price;
    double  qty;
    double  pnl;
    int64_t entry_time;
    int64_t exit_time;
    int     is_long;
} pf_twin_trade;

typedef struct pf_twin_result {
    int            completed;      /* 1 when the run reached Completed. */
    int            trade_count;
    pf_twin_trade  trades[PF_TWIN_MAX_TRADES];
    double         signed_units;
    double         average_price;
    uint64_t       lots;
    int            event_count;
    uint64_t       event_ordinal[PF_TWIN_MAX_EVENTS];
    uint32_t       event_kind[PF_TWIN_MAX_EVENTS];
    /* Applied events only; 0 elsewhere. */
    double         applied_price[PF_TWIN_MAX_EVENTS];
    double         applied_closed[PF_TWIN_MAX_EVENTS];
    double         applied_opened[PF_TWIN_MAX_EVENTS];
    /* Pyramid arm only: the open-lot snapshots, in observation order. */
    int                  observation_count;
    pf_twin_observation  observations[PF_TWIN_OBSERVATIONS];
    /* Pyramid arm only: closed rows the report carried beyond the trade
     * fields above — commission and excursions, for the lot cross-check. */
    double         trade_commission[PF_TWIN_MAX_TRADES];
    double         trade_max_runup[PF_TWIN_MAX_TRADES];
    double         trade_max_drawdown[PF_TWIN_MAX_TRADES];
} pf_twin_result;

/** The twin's bar feed. Both arms read this one array, so the inputs cannot
 *  drift between them. */
const pf_bar_t* pf_twin_bars(int* n);

/** Run the twin scenario through the C API. Market entry at
 *  #PF_TWIN_ENTRY_BAR, flatten at #PF_TWIN_EXIT_BAR.
 *  @return 0 on success, non-zero when the C API itself refused. */
int pf_twin_run_c_market(pf_twin_result* out);

/** The same scenario with a kernel-sized opening (PF_NATIVE_INTENT_SIZED,
 *  cash basis) instead of the market Transact. */
int pf_twin_run_c_sized(pf_twin_result* out);

/** The record of the cancel_where twin: three resting requests whose label
 *  and comment cross ("leg" is two labels and one comment), then one bulk
 *  call per field. Both arms report the counts the kernel answered, in the
 *  order the scenario makes them. */
typedef struct pf_twin_cancel_where {
    int completed;           /* 1 when the run reached Completed. */
    int ran;                 /* 1 when the scenario callback really ran. */
    int comment_hits;        /* cancel_where("leg", COMMENT) */
    int live_after_comment;  /* live rows left after it */
    int label_hits;          /* cancel_where("leg", LABEL) */
    int live_after_label;    /* live rows left after it */
    int unmatched_hits;      /* cancel_where("nobody", LABEL) */
    /* C-only refusals: the C++ overload cannot spell either. */
    int refused_unknown_field;
    int refused_null_text;
} pf_twin_cancel_where;

/** Run the cancel_where scenario through the C API.
 *  @return 0 on success, non-zero when the C API itself refused. */
int pf_twin_run_c_cancel_where(pf_twin_cancel_where* out);

/** The pyramid arm (N18): the PF_TWIN_PYRAMID_* commands, with the open-lot
 *  snapshot read through strategy_native_open_lot_count_v1 / _get_v1 at each
 *  PF_TWIN_PYRAMID_OBSERVE_* calculation, marked at that bar's close.
 *  @return 0 on success, non-zero when the C API itself refused. */
int pf_twin_run_c_pyramid(pf_twin_result* out);

/** The pure-C behaviour suite: refusals, replace/cancel/cancel_all,
 *  execute_current, the callback-failure latch and event polling.
 *  @return the number of failed checks. */
int pf_native_c_api_checks(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PINEFORGE_TEST_NATIVE_C_API_TWIN_H */
