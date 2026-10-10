/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * execution_observer.h - public C ABI for the execution observer, version 1.
 *
 * Declarations only. The runtime supplies the definitions.
 *
 * Layout: natural C alignment, fixed-width scalars, no struct packing. Each
 * struct_size must equal sizeof its declaration, and each version must be 1.
 * Expected LP64 sizes are 24 (boundary), 32 (receipt), 24 (observer) and 48
 * (observation).
 *
 * Pointers are borrowed for the lifetime stated on each declaration; the
 * runtime never takes ownership. A pf_strategy_t must be a valid, nonnull
 * handle from the matched runtime. Forged or dangling handles are outside the
 * contract.
 */

#ifndef PINEFORGE_EXECUTION_OBSERVER_H
#define PINEFORGE_EXECUTION_OBSERVER_H

#include <stdint.h>

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Boundary handed to the observer. The kernel fills every field before the
 *  callback. run_generation is the latest admitted epoch: nonzero at
 *  admission, monotonic, never wraps. attempt_serial is the serial of the
 *  public run or stream-begin entry that owns that epoch. */
typedef struct pf_execution_boundary_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t run_generation;
    uint64_t attempt_serial;
} pf_execution_boundary_v1;

/** Receipt the callback fills. Before the callback the kernel sets the
 *  receipt's struct_size, version and run_generation, frame_bytes to
 *  UINT32_MAX, and handed_bytes, export_requested and reserved to 0.
 *
 *  The callback must overwrite the UINT32_MAX frame_bytes sentinel even when
 *  nothing is exported. It succeeds only if it returns 0 with no exception and
 *  no earlier failure cause, and then:
 *  - struct_size, version and run_generation are unchanged;
 *  - reserved is 0;
 *  - export_requested is exactly 0 or exactly 1;
 *  - export_requested 0: frame_bytes and handed_bytes are both 0;
 *  - export_requested 1: 1 <= frame_bytes <= 1024 (the whole frame), and
 *    handed_bytes equals frame_bytes.
 *
 *  handed_bytes does not claim a parent ACK. Any failure leaves the generation
 *  sealed, with no result work and no retry. */
typedef struct pf_boundary_receipt_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t run_generation;
    uint32_t frame_bytes;
    uint32_t handed_bytes;
    uint32_t export_requested;
    uint32_t reserved;
} pf_boundary_receipt_v1;

/** Observer callback. The runtime calls it once, synchronously, after the
 *  generation seals on a successful terminal path and before results open.
 *  A failed or aborted generation closes without a successful callback. An
 *  exception thrown here is a failure and never unwinds through C. boundary
 *  and receipt are valid for this call only; context follows the registration
 *  lifetime. */
typedef int (*pf_before_results_fn_v1)(
    void *context, const pf_execution_boundary_v1 *boundary,
    pf_boundary_receipt_v1 *receipt);

/** Observer descriptor for strategy_set_execution_observer_v1(). */
typedef struct pf_execution_observer_v1 {
    uint32_t struct_size;
    uint32_t version;
    void *context;
    pf_before_results_fn_v1 before_results;
} pf_execution_observer_v1;

/** Snapshot of the execution state, filled by
 *  strategy_execution_observation_v1().
 *
 *  phase:              0 Idle, 1 Executing, 2 Capturing, 3 Sealed, 4 Results.
 *  fault_stage:        0 None, 1 Execution, 2 AfterExecution.
 *  attempt_outcome:    0 NotCompleted, 1 Refused, 2 Failed, 3 Aborted,
 *                      4 ResultsOpen, 5 Live.
 *  run_generation:     latest admitted epoch (see pf_execution_boundary_v1).
 *  attempt_serial:     latest minting public run or stream-begin entry.
 *  attempt_generation: 0 if that attempt admitted no generation.
 *
 *  The caller correlates its own serial and generation. A rejected later
 *  attempt never turns an older Results snapshot into permission for itself.
 *  Registration or removal does not reopen a closed generation. A refused
 *  attempt before admission mints no generation, so the previous closure
 *  stays. */
typedef struct pf_execution_observation_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t run_generation;
    uint64_t attempt_serial;
    uint64_t attempt_generation;
    uint32_t phase;
    uint32_t fault_stage;
    uint32_t attempt_outcome;
    uint32_t boundary_delivered;
} pf_execution_observation_v1;

/** Returns the execution observer ABI version, 1. */
PF_API uint32_t pf_execution_observer_version(void);

/** Registers, replaces or removes the strategy's execution observer.
 *
 *  Returns 0 when accepted; -1 for an invalid struct_size or version, a null
 *  before_results or a null strategy handle; -2 for an unsupported execution
 *  contract (not a purity assertion or an unknown-override detector); -3 for a
 *  nonquiescent or reentrant call; -4 when the private consumer storage cannot
 *  be allocated. No exception crosses C.
 *
 *  Registration copies the descriptor's values. context and before_results
 *  stay borrowed until a successful replacement or removal, or until the
 *  strategy is destroyed. A NULL descriptor removes the registration, but only
 *  when quiescent. A refusal leaves the installed descriptor unchanged. A
 *  forbidden reentry keeps its first cause latched, as the existing entry
 *  contract requires; an ordinary busy refusal does not rewrite a run outcome.
 *  Destroying the strategy never calls the observer. */
PF_API int strategy_set_execution_observer_v1(
    pf_strategy_t, const pf_execution_observer_v1 *);

/** Copies one complete observation into *out. The caller first sets
 *  out->struct_size and out->version.
 *
 *  Returns 0 with one complete snapshot, or -1 with *out unchanged. Allowed
 *  inside the observer; it dispatches no host code and allocates no missing
 *  consumer. A fresh or copied consumer reports generation 0 and no owned
 *  result. This is observation only, not permission to publish stale output. */
PF_API int strategy_execution_observation_v1(
    pf_strategy_t, pf_execution_observation_v1 *);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_EXECUTION_OBSERVER_H */
