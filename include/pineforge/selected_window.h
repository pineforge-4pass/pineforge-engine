/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * selected_window.h - public C ABI for the selected-window configuration and
 * counters, version 1.
 *
 * Declarations only. The runtime supplies the definitions.
 *
 * Layout: natural C alignment, fixed-width scalars, no struct packing. Each
 * struct_size must equal sizeof its declaration, and each version must be 1.
 * Expected LP64 sizes are 24 (configuration) and 80 (counts).
 */

#ifndef PINEFORGE_SELECTED_WINDOW_H
#define PINEFORGE_SELECTED_WINDOW_H

#include <stdint.h>

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Selected-window configuration, handed to strategy_set_selected_window_v1().
 *  start_ms and end_ms are signed JSON-safe integer milliseconds with
 *  start_ms < end_ms. The selected window is [start_ms, end_ms); preroll is
 *  before start_ms. */
typedef struct pf_selected_window_config_v1 {
    uint32_t struct_size;
    uint32_t version;
    int64_t start_ms;
    int64_t end_ms;
} pf_selected_window_config_v1;

/** Native-owned counts for the latest attempt, filled by
 *  strategy_selected_window_counts_v1(). run_generation, attempt_serial and
 *  attempt_generation are the three identities the caller correlates with
 *  pf_execution_observation_v1 (see execution_observer.h).
 *
 *  Counts are facts of actual successful primary input and script consumption,
 *  not caller-provided expectations, and a host's diagnostic presentation does
 *  not rewrite them. On success, fed_input_bars equals preroll_input_bars plus
 *  window_input_bars, and fed_script_bars equals preroll_script_bars plus
 *  window_script_bars. */
typedef struct pf_selected_window_counts_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t run_generation;
    uint64_t attempt_serial;
    uint64_t attempt_generation;
    uint64_t fed_input_bars;
    uint64_t fed_script_bars;
    uint64_t preroll_input_bars;
    uint64_t preroll_script_bars;
    uint64_t window_input_bars;
    uint64_t window_script_bars;
} pf_selected_window_counts_v1;

/** Returns the selected-window ABI version, 1. */
PF_API uint32_t pf_selected_window_version(void);

/** Sets the pending selected-window configuration, or clears it with a NULL
 *  descriptor. A NULL descriptor disables selection for the next admitted
 *  generation. The setter copies only start_ms and end_ms.
 *
 *  Returns 0 when accepted; -1 for an invalid handle, struct_size, version or
 *  bounds; -2 for an unsupported execution contract (not a purity declaration
 *  or an override detector, and not permission to reject a custom presentation
 *  host); -3 for a nonquiescent or reentrant call; -4 when required private
 *  storage cannot be allocated. Every refusal leaves the configuration
 *  unchanged, and no exception crosses C.
 *
 *  The setter is quiescent only outside execution, capture and observer
 *  dispatch, including outside the existing preparing_begin, in_callback and
 *  processing_input barriers. Setting or clearing changes pending configuration
 *  only: it never reopens the provenance or results of an existing generation,
 *  rewrites the latest attempt or changes an existing capture. Configuration is
 *  copied into admitted state after the last begin refusal or reset and before
 *  the first admitted host callback, and a refused begin consumes no new
 *  selected generation. Copies and moves that discard the private consumer also
 *  discard pending configuration and any owned result, but preserve closed
 *  provenance. */
PF_API int strategy_set_selected_window_v1(
    pf_strategy_t, const pf_selected_window_config_v1 *);

/** Copies one complete counts snapshot for the latest attempt into *out. The
 *  caller first sets out->struct_size and out->version.
 *
 *  Returns 0 with one complete snapshot only when the latest attempt owns a
 *  successful selected Results generation. Returns -1 for a null handle or
 *  output, or a wrong struct_size or version. Returns -2 when no such result
 *  belongs to the latest attempt (fresh, copied, OFF, live, refused, failed,
 *  aborted or observer-faulted). On failure, valid caller storage is unchanged.
 *
 *  The getter never executes host code, allocates a missing consumer or
 *  materializes a report. Counts do not open early inside the observer, and
 *  these counters never authorize charges. */
PF_API int strategy_selected_window_counts_v1(
    pf_strategy_t, pf_selected_window_counts_v1 *);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_SELECTED_WINDOW_H */
