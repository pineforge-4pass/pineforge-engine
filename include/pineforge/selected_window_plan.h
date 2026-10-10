/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * selected_window_plan.h - public C bridge to the selected primary planner
 * (pineforge::plan_selected_primary, selected_window_plan.hpp), version 1.
 *
 * The definitions live in the standalone window-plan shared library (a CMake
 * target named in CMakeLists.txt, installed as a shared object on
 * Linux). It links only the kernel archive: it is not a strategy library, loads
 * no strategy, runs no host or library callback and exports nothing of the
 * compiled-strategy ABI in pineforge.h. A caller names the helper by an absolute
 * path from its own pinned build. C++ callers may keep linking PineForge::kernel
 * and call the C++ planner directly.
 *
 * Layout: natural C alignment, fixed-width scalars, no struct packing. The
 * caller presets struct_size to sizeof of its declaration and version to 1 in
 * both descriptors. Expected LP64 sizes are 80 (request) and 248 (result).
 *
 * Rows: a read-only array of pf_bar_t (pineforge.h: five doubles, then the int64
 * Unix-millisecond timestamp), which the bridge reads in place as the identical
 * pineforge::Bar. Only the timestamps are read; no row is copied. The array and
 * all five strings need only stay valid for the duration of the call; the bridge
 * keeps nothing, owns nothing it hands back, and has no free function.
 */

#ifndef PINEFORGE_SELECTED_WINDOW_PLAN_H
#define PINEFORGE_SELECTED_WINDOW_PLAN_H

#include <stdint.h>

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One selected-window planning request. Field meanings are those of the C++
 *  SelectedPlanRequest: start_ms (T), end_ms (E) and fed_start_ms (F) are
 *  Unix milliseconds, preroll_bars is N, and the five strings are the input
 *  timeframe, the script timeframe, the chart and engine timezones and the
 *  session.
 *
 *  All five strings must be non-NULL, NUL-terminated UTF-8. They are copied
 *  during the call. An empty chart_timezone, engine_timezone or session keeps
 *  the C++ default meaning; an empty timeframe is a planner refusal, not a
 *  bridge failure. feed_tolerant is exactly 0 or 1. */
typedef struct pf_selected_plan_request_v1 {
    uint32_t struct_size;
    uint32_t version;
    int64_t start_ms;
    int64_t end_ms;
    int64_t fed_start_ms;
    uint32_t preroll_bars;
    uint32_t feed_tolerant;
    const char *input_tf;
    const char *script_tf;
    const char *chart_timezone;
    const char *engine_timezone;
    const char *session;
} pf_selected_plan_request_v1;

/** One complete planner result, plain data, written whole by a successful call.
 *
 *  status is the C++ SelectedPlanStatus mapped explicitly, not inferred from the
 *  enumerator's width: 0 Ok, 1 RequestInvalid, 2 CalendarUnsupported,
 *  3 BoundaryUnaligned, 4 FeedRangeInvalid, 5 RowsUnordered, 6 InternalError.
 *  bound is -1 (no boundary detail), 0 (T), 1 (E) or 2 (F). Every other field
 *  has the meaning and the equalities of the C++ SelectedPrimaryPlan.
 *
 *  present_mask bit i (0..7) says whether the i-th of the eight optional
 *  timestamps is present, in their field order below: preroll_first_bar_ms,
 *  preroll_last_bar_ms, supplied_first_data_ms, supplied_last_data_ms,
 *  fed_first_data_ms, fed_last_data_ms, window_first_data_ms,
 *  window_last_data_ms. An absent timestamp is zero, and a zero with its bit set
 *  is a real timestamp. shortfall and complete_pending_preroll_at_horizon are
 *  exactly 0 or 1; reserved is 0. option is ASCII, NUL-terminated and
 *  zero-padded (the C++ option string, at most 31 characters). */
typedef struct pf_selected_plan_result_v1 {
    uint32_t struct_size;
    uint32_t version;
    int32_t status;
    int32_t bound;
    int64_t value_ms;
    int64_t previous_boundary_ms;
    int64_t next_boundary_ms;
    uint64_t supplied_input_bars;
    uint64_t supplied_script_bars;
    uint64_t available_script_bars;
    uint64_t used_script_bars;
    uint64_t trimmed_script_bars;
    uint64_t trim_index;
    uint64_t fed_input_bars;
    uint64_t fed_script_bars;
    uint64_t preroll_input_bars;
    uint64_t window_input_bars;
    uint64_t window_script_bars;
    int64_t trim_start_ms;
    int64_t preroll_first_bar_ms;
    int64_t preroll_last_bar_ms;
    int64_t supplied_first_data_ms;
    int64_t supplied_last_data_ms;
    int64_t fed_first_data_ms;
    int64_t fed_last_data_ms;
    int64_t window_first_data_ms;
    int64_t window_last_data_ms;
    uint32_t present_mask;
    uint32_t shortfall;
    uint32_t complete_pending_preroll_at_horizon;
    uint32_t reserved;
    char option[32];
} pf_selected_plan_result_v1;

/** Returns the selected-planner C bridge version, 1. */
PF_API uint32_t pf_selected_plan_version(void);

/** Plans the primary rows of one selected-window request.
 *
 *  Returns 0 when one whole result was written to *out, INCLUDING every planner
 *  refusal (a refusal is carried in out->status, never in the return value).
 *  Returns -1 for a NULL request or output, a wrong struct_size or version in
 *  either descriptor, a NULL string pointer, a feed_tolerant other than 0 or 1,
 *  or a count that does not fit size_t; -2 when memory could not be allocated;
 *  -3 for any other bridge failure or broken bridge invariant (for example a
 *  planner option longer than 31 characters, which is never truncated). On every
 *  negative return a valid caller output is byte-unchanged.
 *
 *  rows may be NULL only with count 0. A NULL rows with a positive count is not
 *  dereferenced by the bridge: the planner refuses it as RequestInvalid. The
 *  bridge builds a local result and commits it to *out with one assignment, calls
 *  no host or library callback, and lets no exception cross C. */
PF_API int pf_plan_selected_primary_v1(const pf_selected_plan_request_v1 *,
                                       const pf_bar_t *rows, uint64_t count,
                                       pf_selected_plan_result_v1 *);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_SELECTED_WINDOW_PLAN_H */
