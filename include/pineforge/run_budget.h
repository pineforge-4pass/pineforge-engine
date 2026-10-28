#ifndef PINEFORGE_RUN_BUDGET_H
#define PINEFORGE_RUN_BUDGET_H

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Cooperative per-run resource limits. Initialize struct_size and version=1.
 *  Zero means unlimited; NULL at the setter removes both limits.
 *  Each value must fit INT64_MAX milliseconds. The descriptor is copied.
 *  Wall time uses CLOCK_MONOTONIC; CPU time uses CLOCK_PROCESS_CPUTIME_ID
 *  (all process threads). Both origins include public begin preparation.
 *  Check at begin, before every input bar, and after the last input; a
 *  returning begin callback is checked before any input. A tick-driven
 *  stream checks before each tick as well. Elapsed whole milliseconds must
 *  exceed the limit to fail. Wall wins if both limits are exceeded.
 *  Failure is run_budget_exceeded with kind=wall|cpu and integer limit/used
 *  in milliseconds. An unreached budget changes no report or hash bytes.
 *  This cooperative API cannot interrupt a callback that never returns,
 *  nor bound compilation; hosts need a process watchdog for those cases. */
typedef struct pf_run_budget_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t wall_ms;
    uint64_t cpu_ms;
} pf_run_budget_v1;

PF_STATIC_ASSERT(sizeof(pf_run_budget_v1) == 24);
PF_STATIC_ASSERT(offsetof(pf_run_budget_v1, wall_ms) == 8);
PF_STATIC_ASSERT(offsetof(pf_run_budget_v1, cpu_ms) == 16);

/** Returns run budget ABI version 1. */
PF_API uint32_t pf_run_budget_version(void);

/** Install the next run's limits, or remove with NULL. Returns 0 accepted;
 *  -1 bad handle/size/version or a limit above INT64_MAX; -2 unsupported
 *  execution contract; -3 active run/begin/callback/results dispatch;
 *  -4 consumer allocation failed. A refusal preserves the prior limits and
 *  run outcome. Calls on one handle must be serialized. Limits persist
 *  across runs until replaced or removed, with fresh origins at each begin. */
PF_API int strategy_set_run_budget_v1(pf_strategy_t, const pf_run_budget_v1*);

#ifdef __cplusplus
}
#endif

#endif
