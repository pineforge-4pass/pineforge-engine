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

/** The pure-C behaviour suite: refusals, replace/cancel/cancel_all,
 *  execute_current, the callback-failure latch and event polling.
 *  @return the number of failed checks. */
int pf_native_c_api_checks(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PINEFORGE_TEST_NATIVE_C_API_TWIN_H */
