#!/usr/bin/env python3
"""Shared local/CI verification driver. Stdlib only. Not a command generator.

Profiles: release, debug, sanitizers, native, live-sanitizers, live-tsan, kernel. Default dir build-ci-PROFILE.
Source guards, explicit configure, full rebuild, pinned e60/0e/v13/v14/v15/v16 ABI prepare/reuse,
CTest, install+find_package+VERSION smoke, native help / required WebSocket.
Fail fast on configure/build. After a successful build collect independent
CTest and package failures in the same run. Never deletes source, tests, or
build trees; a mismatched ABI base is a refusal to a fresh --build-dir.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Callable

from prepare_settlement_cpp_abi_base import (
    BASE_COMMIT,
    BASE_TREE,
    PROVIDERS,
    read_cache,
    reusable_prepared_base,
)

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ('release', 'debug', 'sanitizers', 'native', 'live-sanitizers', 'live-tsan', 'kernel')
LIVE_ONLY_PROFILES = frozenset(('live-sanitizers', 'live-tsan'))
DEFAULT_JOBS = 4
JOBS_MIN, JOBS_MAX = 1, 64
SCHEMA = 'pineforge-ci-verify/v1'
SANITIZER_FLAG = '-fsanitize=address,undefined'
# Kernel-row floor. The kernel profile registers only the source-free CTest
# rows (tests/CMakeLists.txt drops every test TU whose include closure reaches
# pineforge/source or compat/pine), so a lane whose native pin sits beside an
# adapter twin in one TU silently leaves the kernel-only gate. The floor pins
# the row count the profile is expected to run: adding source-free rows never
# trips it, losing them does, and a run that reports no count fails closed.
# 176 = the 158 rows the profile ran before lane N3, plus the six native halves
# N3 freed (L2 report truth, L4b margin hooks, L5 calc timing, L6 HTF
# subscriptions, L8/L8b price grid, L11a lot excursion) = 164, plus the ten
# example_* rows N4 brought into this profile when it turned
# PINEFORGE_BUILD_EXAMPLES on here, plus N5's test_native_host_hash_extension
# and N6's test_native_margin_fx_roll. N9 and N8 added no source-free TU of
# their own: both extended rows the profile already ran. Raise it when a new
# source-free row lands. --min-tests overrides it for any profile.
# 181 = those 176 rows plus gap wave B's five: N13's test_native_arm_options,
# N7's test_native_auxiliary_feed and test_native_auxiliary_feed_stream (its
# adapter twin is a source-layer TU and stays out), and N11's two source-guard
# rows test_adapter_spec_shadowing / _mutations, which are Python guards over
# the sources and so register in every profile. N10 and N12 added no
# source-free row: N10 re-pointed six trail TUs at a tests-only oracle header
# (they keep running here) and N12's differential is an adapter TU.
# 182 = those 181 plus N18's test_native_open_lots, plus N14's
# test_native_tail_override_seam, minus N14's test_market_admission_causality,
# which left this profile when the admission journal became source-layer state
# (it still runs in every profile that builds the source layer). N15 adds no
# CTest row: its corpus-parity gate is a ci_preflight stage and a CI job.
# 183 = those 182 plus gap lane P2's test_kernel_residuals, the Python guard
# that holds ADR-0001's residual-vocabulary table against libpineforge_kernel.a
# (a source-free row: it reads the archive every profile builds).
# 184 = those 183 plus P5's test_native_limit_fill_through (the market-if-
# touched geometry of Limit{price, fill_through}); P5's closed_trade(i) and
# liquidation-through-the-lots witnesses extend existing source-free TUs.
# 187 = those 184 plus audit lane P6's three example_* rows for the two
# features the adapter never declares: example_native_price_grid_strategy,
# example_native_price_grid_c and example_native_risk_limits_strategy.
# 189 = those 187 plus P6's two source-guard rows test_native_feature_rulings /
# _mutations, which are Python guards over the sources and so register in
# every profile, as N11's two do.
# 190 = those 189 plus lane P7's test_example_runner, the self-test of the
# runner every example_* row goes through; it registers wherever the examples
# do, so here and in release.
# expectation corrected: 190 -> 189, because lane Q12's floor counts only the
# rows that RAN (ctest_rows below), never a skipped one, and of those 190
# registered rows test_native_live_websocket skips (exit 77) wherever the
# linked libcurl has no WebSocket support -- a system libcurl; the native CI
# lane builds its own and refuses the skip -- so 189 run. A host whose
# libcurl has it runs 190.
# 193 = those 189 plus the four rows the R5 documentation wave registers in
# every profile that builds the examples: L14-A's two guard suites
# (test_doc_anchors, test_doc_lint) and L14-B's two example hosts
# (example_native_auxiliary_feed_strategy, example_native_open_lots_strategy).
# 194 registered, 193 run: the WebSocket row still skips on a system libcurl.
# 199 = those 193 plus the six TUs the R5 follow-up wave (INT13, wave D) adds
# that this profile builds -- one per lane, measured on the integrated tree,
# not summed from the lanes' own bases:
#   +1 E1  test_native_anchored_trail_absent_arm
#   +1 E3  test_native_margin_fx_clock
#   +1 E4  test_abi_receipt_skips        (registers in every profile)
#   +1 E6  test_e6_entry_bar_mask_declaration
#   +1 E10 test_e10_dead_path_prefix
#   +1 E14 test_native_trail_best_seed
# Lanes E5 and E9 add a row each as well (test_offset_trail_quantized_arm,
# test_trail_activation_tick_reach), but both TUs reach the source layer, so
# they register in release only -- see RELEASE_MIN_TESTS. E2, E7, E8, E11,
# E12, E13 and E15 add no row: every one of their witnesses is a scenario
# inside a TU that already existed.
# 200 registered, 199 run: the WebSocket row still skips on a system libcurl.
# 202 run = those 199 plus the three TUs of the R5 follow-up wave (INT14,
# wave E) that this profile builds -- measured on the integrated tree, not
# summed from the lanes' own bases:
#   +1 E16 test_native_trail_stop_ladder
#   +1 E23 test_native_continuation_portable
#   +1 E24 test_native_continuation_digest_tail
# All three are source-free: each drives NativeStrategyHost alone, so the
# include-closure filter above keeps them. Lanes E19 and E25 add a row each
# (test_e19_excursion_path_order, test_session_islastbar_aggregation), but both
# TUs reach the source layer, so they register in release only -- see
# RELEASE_MIN_TESTS. E17 adds nothing: it ruled the engine not guilty and has
# no commit. E18, E20, E21 and E22 add no row either: every one of their
# witnesses is a scenario inside a TU that already existed.
# 203 registered, 202 run: the WebSocket row still skips on a system libcurl.
# The R5 post-wave-E lanes (INT15) leave this profile alone: E26 changes
# src/source/pine_strategy_host.cpp and adds its checks to the TU lane E25
# already registered, and E27's witness includes pineforge/source/, so it
# registers in release only -- see RELEASE_MIN_TESTS. Still 203 registered,
# 202 run, recounted on the integrated tree.
# 213 run = those 202 plus the eleven TUs of the R5 wave F (INT16) that this
# profile builds -- measured on the integrated tree (ctest -N), not summed
# from the lanes' own bases:
#   +3 F12 example_native_fee_reserve_strategy, example_native_fx_roll_strategy,
#          example_native_broker_hash_strategy
#   +3 F6  test_f6_dead_kernel_members, test_timeframe_trace_switch_once,
#          test_removed_public_spellings (renamed by lane REL10)
#   +1 F3  test_native_bare_host_contracts
#   +1 F4  test_native_c_api_c99
#   +1 F5  test_native_session_day_facts
#   +1 F14 test_native_session_boundaries
#   +1 F2  test_pine_to_native_worked
# All eleven are source-free. Lanes F1, F5 and F9 add a release-only row each
# (their TUs reach the source layer -- see RELEASE_MIN_TESTS); F7 and F10 add
# no row. 214 registered, 213 run: the WebSocket row still skips on a system
# libcurl.
# 229 run = those 213 plus the sixteen TUs of R5 performance slice A (INT17)
# that this profile builds -- measured on the integrated tree (ctest -N), not
# summed from the lanes' own bases:
#   +1 P1    test_native_continuation_view
#   +3 P23   test_native_projection_witness, test_native_projection_compare,
#            test_native_callback_caches
#   +1 P4    test_native_command_history_read
#   +3 K3    test_native_match_rescan_scaling, test_native_match_row_reuse,
#            test_native_match_hash_witness
#   +2 K24   test_native_intrabar_lower_lookup, test_native_batch_log_presize
#   +3 K1    test_native_calendar_hash_witness, test_native_calendar_memo,
#            test_native_calendar_memo_cost
#   +3 P5611 test_utc_month_key_arithmetic, test_local_time_fields,
#            test_local_time_lock_free_hit
# All sixteen are source-free. Lanes P1, P23, P4 and P5611 add five
# release-only rows between them (their TUs reach the source layer -- see
# RELEASE_MIN_TESTS). 230 registered, 229 run: the WebSocket row still skips
# on a system libcurl.
# 237 run = those 229 plus the eight TUs of R5 performance slice B (INT18)
# that this profile builds -- measured on the integrated tree (ctest -N), not
# summed from the lanes' own bases:
#   +5 L1 test_native_host_reads, test_native_lean_path,
#         test_native_intrabar_scratch, test_native_lookup_memos,
#         test_native_point_checks
#   +3 L5 test_native_match_band_witness, test_native_match_allocations,
#         test_native_match_band_precheck
# All eight are source-free. Lane L5 adds one release-only row and lane L4
# three (their TUs reach the source layer -- see RELEASE_MIN_TESTS). 238
# registered, 237 run: the WebSocket row still skips on a system libcurl.
# 242 run = those 237 plus the five source-free TUs of R5 lane TA1 (the TA
# inputs TradingView has and the KC middle band on bar 0):
#   +5 TA1 test_ta_anchored_vwap, test_ta_alma_floor, test_ta_kc_basis,
#          test_ta_kc_range, test_ta_pivot_point_levels
# 243 registered, 242 run.
# 245 run = those 242 plus the three TUs of INT19 (V19-A, K-COHORT, PERF-P7)
# that this profile builds -- measured on the integrated tree (ctest -N), not
# summed from the lanes' own bases:
#   +1 V19-A    test_native_state_continuation
#   +1 K-COHORT test_native_cohort_sliced_close
#   +1 PERF-P7  test_native_definition_index
# All three are source-free. PERF-P7 adds four release-only rows (their TUs
# reach the source layer -- see RELEASE_MIN_TESTS); FPC adds no row. 246
# registered, 245 run: the WebSocket row still skips on a system libcurl.
# 249 run = those 245 plus the four TUs of INT20 (V19-B, PERF-L2) -- measured
# on the integrated tree (ctest -N), not summed from the lanes' own bases:
#   +2 V19-B   test_native_journal_window, test_native_event_retention
#   +2 PERF-L2 test_native_fused_settlement,
#              test_native_fused_settlement_allocations
# All four are source-free, so they register in release too. INT20 retires no
# row: test_native_definition_index stays, re-targeted at the core's chain
# index. 250 registered, 249 run: the WebSocket row still skips on a system
# libcurl.
# 257 run = those 249 plus the eight TUs of INT22 (L3, D2-A) -- measured on
# the integrated tree (ctest -N), not summed from the lanes' own bases:
#   +2 L3   test_native_direct_mutation,
#           test_native_direct_mutation_allocations
#   +6 D2-A test_native_command_after, test_native_quiet_point,
#           test_magnifier_ordered_sampler, test_magnifier_endpoints4,
#           test_native_bar_open_hook, test_native_precommit_hook
# All eight are source-free, so they register in release too; LAYOUT1 adds
# no row. 258 registered, 257 run: the WebSocket row still skips on a system
# libcurl.
# 262 run = those 257 plus the five source-free TUs of INT23 (D2-D, D2-C,
# V19-D) -- measured on the integrated tree (ctest -N), not summed from the
# lanes' own bases:
#   +1 D2-D  test_utc_month_memo
#   +3 D2-C  test_native_runtime_ambient, test_native_in_place_reads,
#            test_native_settlement_carry
#   +1 V19-D test_native_handle_stable_replace
# All five register in release too; the lanes' other rows reach the source
# layer (see RELEASE_MIN_TESTS). 263 registered, 262 run: the WebSocket row
# still skips on a system libcurl.
# 271 run = those 262 plus the nine source-free TUs of INT24 (K-ULP2, K-ULP3,
# B-ENGINE, B-ADAPTER, B-GATES) -- measured on the integrated tree (ctest -N),
# not summed from the lanes' own bases:
#   +1 K-ULP2    test_native_partial_close_split
#   +1 K-ULP3    test_native_exact_sum_close
#   +2 B-ENGINE  test_native_crossing_transact, test_str_number_format
#   +2 B-ADAPTER test_native_script_bucket_completions,
#                test_native_current_execution_rate
#   +3 B-GATES   test_benchmark_harness, test_benchmark_provenance,
#                test_docs_doxygen_retry
# All nine register in release too; B-ADAPTER's other four rows and INT24's
# dual-entry witness reach the source layer (see RELEASE_MIN_TESTS), and
# B-C-SURFACE and B-DOCS add no row. 272 registered, 271 run: the WebSocket
# row still skips on a system libcurl.
# 277 run = those 271 plus the six source-free TUs of wave G (INT25) --
# measured on the integrated tree (ctest -N), not summed from the lanes' own
# bases:
#   +1 C-SURFACE-1 test_native_c_api_int23_header (the INT23 frozen-header
#                  decision-tail witness)
#   +1 KERNEL-EDGE test_native_kernel_edge
#   +2 K-ULP4      test_native_unrepresentable_refusal,
#                  test_native_quantity_tolerance
#   +1 K-ULP5      test_native_group_absorption
#   +1 DOC-TRUTH-4 test_native_engine_complete_host (the guide's Complete host,
#                  extracted from the page and run)
# All six register in release too; RATIO-HARDEN, V19-FIX and K-IDX add no row.
# 278 registered, 277 run: the WebSocket row still skips on a system libcurl.
# 286 run = those 277 plus the nine source-free rows of wave H (INT26),
# counted with ctest -N on the integrated tree:
#   +1 H-MEASURE    test_native_acid_composite (the G1 acid composite, C++
#                   and C)
#   +3 H-DOCGATES   test_doc_reverts and test_kernel_seam_rows (the two gates'
#                   self-tests, Python rows every profile registers) and
#                   test_native_runtime_ambient_lifo (the runtime blocks' LIFO
#                   death row)
#   +1 K-OCA-KEEP   test_native_group_keep_handle
#   +1 PAR-ORDERS-2 test_native_current_cohort_refusal
#   +1 PAR-MARGIN   test_native_margin_intrabar_samples
#   +1 PAR-MARGIN-2 test_native_margin_post_fill_path
#   +1 PERF-KEDGE   test_native_session_day_utc (session-day facts on a UTC
#                   calendar without a calendar interval per bar)
# REL10 renames F6's row (test_deprecated_public_spellings ->
# test_removed_public_spellings); PAR-ORDERS and CI-FLAKE add no row. All
# nine register in release too. 287 registered, 286 run: the WebSocket row
# still skips on a system libcurl.
# 289 run = those 286 plus INT27's three source-free rows, counted with ctest
# -N on the integrated tree:
#   +1 PERF-ZONED   test_native_session_day_zoned (session-day facts on a
#                   certified cycle of an exchange zone without a calendar
#                   interval per bar: certificates at real transitions, the
#                   POSIX twin's facts at every decision point, the cost
#                   pinned)
#   +1 MAG-INTRABAR test_native_intrabar_turning_points (a retained lower bar
#                   walked through its four turning points; one print, once)
#   +1 K-DRAWSNAP   test_drawing_checkpoint (drawing arenas share records
#                   between checkpoint copies: the differential against the
#                   previous arena, the cost pins)
# All three register in release too. 290 registered, 289 run: the WebSocket
# row still skips on a system libcurl.
# 291 run = those 289 plus lane XSYM-D's two source-free rows, counted with
# ctest -N on the lane's tree:
#   +1 test_native_instrument_feed (another symbol's bars merged by interval:
#      a foreign session, a daily close seen a day late, weekend carry and its
#      clearing, a lower-timeframe feed under lookahead, history before the
#      first input, the raw label partition)
#   +1 test_run_strategy_requests (the harness's probe request data: the
#      manifest, the sha256 pins, the refusals by name)
# Both register in release too. 292 registered, 291 run: the WebSocket row
# still skips on a system libcurl.
# 293 run = those 291 plus lane K-SESSION-WINDOWS' two source-free rows,
# counted with ctest -N on the rebased tree (kernel profile, 294 registered):
#   +1 F6 test_session_prepost_windows_tape (pre- and post-market over every
#         session window, on TradingView's session-flag tapes)
#   +1 F1 test_native_session_day_bar_open (a script bar's session-day facts
#         at its own label, on TradingView's extended-hours and break-open
#         tapes)
# Both register in release too. 294 registered, 293 run: the WebSocket row
# still skips on a system libcurl.
# 295 run = those 293 plus lane RUN-HARNESS's two source-free harness suites,
# counted with ctest -N on the integrated tree (kernel profile, 296 registered):
#   +1 test_run_strategy_magnifier (a script declaring use_bar_magnifier = true
#      runs magnified over the runner-named finer feed)
#   +1 test_run_strategy_mincontract (syminfo.mincontract is the lane
#      template's quantity step)
# Both register in release too. 296 registered, 295 run: the WebSocket row
# still skips on a system libcurl.
# 296 run = those 295 plus lane W11-ENG-TIME-COLOR's source-free row, counted
# with ctest -N on INT29's integrated tree (kernel profile, 297 registered):
#   +1 test_color_tapes (colours as TradingView reads them back)
# It registers in release too. 297 registered, 296 run: the WebSocket row
# still skips on a system libcurl.
# 298 run = those 296 plus lane W15-KERNEL-CAL's two source-free rows:
#   +1 test_calendar_bucket_closes_once (a D/W/M bucket completes once, on
#      its period's last input bar: NSE's weekend sessions)
#   +1 test_intraday_grid_late_day_open (a late-opening day's intraday grid
#      starts at the daily feed's stamp: NSE's Muhurat session)
# Both register in release too. 299 registered, 298 run: the WebSocket row
# still skips on a system libcurl.
# 300 run = those 298 plus lane XAU-CAL's two source-free rows:
#   +1 test_native_day_labels (a daily bar the installed daily feed dates is
#      labelled by its stamp: OANDA's 17:00 ET break stamps)
#   +1 test_symbol_calendar (TradingView's symbol calendar as a run input: the
#      tape converter, the loader, the harness's syminfo-metadata transport)
# Both register in release too. 300 registered, 299 run after parser removal: the WebSocket row
# still skips on a system libcurl. Lane SM-E1's rows follow EXCLUDED_REGISTERED_MIN below.
# KERNEL_MIN_TESTS itself is defined with RELEASE_MIN_TESTS below, both derived from
# PRIVATE_FIXTURE_PROFILE_FLOORS (the one place the numbers live).
# Release-row floor, the same gate for the default profile. Before lane P7
# only the kernel profile had one, so a row that left release alone (a
# source-bound TU dropped from TEST_SOURCES, a deleted twin or ABI row) left a
# smaller green run. 546 = the 545 rows release registered at b8e7976e plus
# P7's test_example_runner. The debug and sanitizers profiles register a
# subset of these rows (the same CMake without the examples), so a row they
# share cannot vanish unnoticed either. Raise it when a row lands in release;
# --min-tests overrides it.
# 553 = those 546 plus the release rows of the gap lanes integrated before P7:
# P2's test_kernel_residuals, P5's test_native_limit_fill_through, and P6's
# three example_* rows and two test_native_feature_rulings guard rows.
# Lane Q12 recounted it on the rows that ran, and it stays 553: under
# ci_verify no release row skips -- the four receipt-gated rows run with
# --require-receipts, which fails rather than skips, and the WebSocket row is
# a live-runner row this profile does not build.
# 554 = those 553 plus gap lane Q6's test_adapter_range_end_relower: its TU
# includes pineforge/source/, so it registers in release only and the kernel
# floor does not move.
# 558 = those 554 plus the same four documentation-wave rows: the two guard
# suites register wherever the tests do, the two example hosts wherever
# PINEFORGE_BUILD_EXAMPLES is ON. No release row skips, so 558 registered is
# 558 run.
# 566 = those 558 plus the eight TUs of the R5 follow-up wave (INT13, wave D):
# the six KERNEL_MIN_TESTS lists above, which register here too, plus the two
# source-bound ones the kernel profile does not build:
#   +1 E5  test_offset_trail_quantized_arm
#   +1 E9  test_trail_activation_tick_reach
# No release row skips, so 566 registered is 566 run.
# 571 = those 566 plus the five TUs of the R5 follow-up wave (INT14, wave E):
# the three KERNEL_MIN_TESTS lists above, which register here too, plus the two
# source-bound ones the kernel profile does not build:
#   +1 E19 test_e19_excursion_path_order
#   +1 E25 test_session_islastbar_aggregation
# No release row skips, so 571 registered is 571 run.
# 572 = those 571 plus the one TU of the R5 post-wave-E lanes (INT15):
#   +1 E27 test_pooc_fill_stamp_aggregation
# Its TU reaches the source layer, so it registers in release only and the
# kernel floor does not move. Lane E26 adds no row: its witnesses are
# scenarios inside test_session_islastbar_aggregation, which lane E25
# already registered. No release row skips, so 572 registered is 572 run.
# 586 = those 572 plus the fourteen TUs of the R5 wave F (INT16): the eleven
# KERNEL_MIN_TESTS lists above, which register here too, plus the three
# source-bound ones the kernel profile does not build:
#   +1 F1  test_aggregated_path_regressions
#   +1 F5  test_session_day_facts_adapter
#   +1 F9  test_adapter_recording_hash_witness
# No release row skips, so 586 registered is 586 run.
# 607 = those 586 plus the twenty-one TUs of R5 performance slice A (INT17):
# the sixteen KERNEL_MIN_TESTS lists above, which register here too, plus the
# five source-bound ones the kernel profile does not build:
#   +1 P1    test_adapter_continuation_view
#   +1 P23   test_adapter_host_view_memo
#   +1 P4    test_adapter_receipts_in_place
#   +2 P5611 test_chart_day_key_arithmetic, test_aggregates_input_bars_literals
# No release row skips, so 607 registered is 607 run.
# 619 = those 607 plus the twelve rows of R5 performance slice B (INT18): the
# eight KERNEL_MIN_TESTS lists above, which register here too, plus the four
# source-bound ones the kernel profile does not build:
#   +1 L5 test_adapter_command_allocations
#   +3 L4 test_adapter_quiet_bar, test_adapter_quiet_bar_probe,
#         test_adapter_quiet_bar_differential
# No release row skips, so 619 registered is 619 run.
# 624 = those 619 plus the five TA1 rows KERNEL_MIN_TESTS lists above, which
# register here too. No release row skips, so 624 registered is 624 run.
# 631 = those 624 plus the seven rows of INT19: the three KERNEL_MIN_TESTS
# lists above, which register here too, plus the four source-bound ones the
# kernel profile does not build:
#   +4 PERF-P7 test_adapter_lookup_index_witness, test_adapter_purge_index,
#              test_adapter_exit_leg_index, test_adapter_lookup_index_scaling
# No release row skips, so 631 registered is 631 run.
# 635 = those 631 plus the four INT20 rows KERNEL_MIN_TESTS lists above, which
# register here too; neither lane adds a release-only row. No release row
# skips, so 635 registered is 635 run.
# 637 = those 635 plus V19-E's two rows (INT21), counted with ctest -N on the
# integrated tree; their TUs reach the source layer, so they register in
# release only and the kernel floor does not move (CORPUS2 adds no row):
#   +2 V19-E test_adapter_live_state_equivalence, test_adapter_live_state_scaling
# No release row skips, so 637 registered is 637 run.
# 645 = those 637 plus the eight INT22 rows KERNEL_MIN_TESTS lists above,
# which register here too, counted with ctest -N on the integrated tree;
# neither L3 nor D2-A adds a release-only row and LAYOUT1 adds no row. No
# release row skips, so 645 registered is 645 run.
# 658 = those 645 plus the five INT23 rows KERNEL_MIN_TESTS lists above, which
# register here too, and eight source-bound rows the kernel profile does not
# build, counted with ctest -N on the integrated tree:
#   +2 D2-D  test_chart_day_memo, test_publication_witness
#   +3 D2-C  test_adapter_runtime_ambient, test_adapter_in_place_reads,
#            test_adapter_settlement_carry
#   +3 V19-D test_adapter_reissue_binding, test_bracket_roster_parking,
#            test_adapter_margin_revival_erasure
# No release row skips, so 658 registered is 658 run.
# 672 = those 658 plus the nine INT24 rows KERNEL_MIN_TESTS lists above, which
# register here too, and five source-bound rows the kernel profile does not
# build, counted with ctest -N on the integrated tree:
#   +4 B-ADAPTER test_adapter_fill_qty_probe,
#                test_adapter_typed_entry_admission,
#                test_adapter_range_end_fx,
#                test_adapter_margin_revival_cancel
#   +1 INT24     test_adapter_dual_entry_tie (the ruling on B-ADAPTER's
#                finding 1)
# B-C-SURFACE's witnesses are rows inside test_native_c_api.
# 678 = those 672 plus the six wave-G (INT25) rows KERNEL_MIN_TESTS lists
# above -- C-SURFACE-1 +1, KERNEL-EDGE +1, K-ULP4 +2, K-ULP5 +1, DOC-TRUTH-4 +1
# -- which register here too, counted with ctest -N on the integrated tree; no
# wave-G lane adds a source-bound row (the K-ULP4 and K-ULP5 C checks and
# V19-FIX's scaling rows are rows inside existing TUs). No release row skips,
# so 678 registered is 678 run.
# 702 = those 678 plus wave H's twenty-four (INT26), counted with ctest -N on
# the integrated tree: the nine KERNEL_MIN_TESTS lists above, which register
# here too, H-MEASURE's eleven source-bound rows the kernel profile does not
# build (AUDIT4 X14) and PAR-ORDERS-2's three tape rows:
#   +1 G2-09/-10/-12/-13, E20 f1  test_adapter_margin_schedule_differential
#   +3 G2-15, G2-18, E5/E14       test_pyramiding_count_differential,
#                                 test_zero_trail_sibling_stop,
#                                 test_pending_entry_trail_tapes
#   +4 G2-22, G2-21, G2-32, G2-36 test_e19_excursion_tape,
#                                 test_short_seed_report_swap,
#                                 test_pine_dust_sweep_paired,
#                                 test_session_ismarket_tape
#   +3 F1(e), F1 magnified, G2-23 test_aggregated_entry_bar_index_tape,
#                                 test_magnified_aggregated_tape,
#                                 test_adapter_security_route_conditions
#   +3 PAR-ORDERS-2 items 1-3     test_flat_coof_exit_tapes,
#                                 test_second_extreme_order_tapes,
#                                 test_pooc_reversing_stop_tapes
#   +1 PAR-CASHFEE                test_cash_fee_sizing_tapes
# PAR-MARGIN's own count also held the margin differential, which that lane
# had cherry-picked for its pins; it counts once, as H-MEASURE's. No release
# row skips, so 702 registered is 702 run.
# 703 = those 702 plus INT26 round 2's source-bound tape row,
# test_pooc_stop_reentry_bracket_tapes (the gate sweep's lost re-entry
# bracket); 703 registered is 703 run.
# 716 = those 703 plus INT27's thirteen, counted with ctest -N on the
# integrated tree: the three KERNEL_MIN_TESTS rows above, which register here
# too, and ten source-bound rows the kernel profile does not build:
#   +1 K-RUNERR     test_security_finer_than_feed (a request.security finer
#                   than every supplied feed reads na)
#   +1 TV-DEFAULTS  test_tv_strategy_defaults (TradingView's Pine v6
#                   strategy() defaults, replayed under the configuration the
#                   generated constructor declares)
#   +2 MAG-INTRABAR test_magnifier_tradingview_intrabars (the table, the grid
#                   and the ownership of TradingView's intrabars),
#                   test_adapter_magnifier_intrabar_tapes (TradingView's tapes)
#   +1 PAR-ORDERS-3 test_pooc_limit_close_tapes (a process_orders_on_close
#                   limit entry at the close that placed it)
#   +2 K-DRAWSNAP   test_coof_drawing_checkpoint_cost (a drawing
#                   calc_on_order_fills run's checkpoint cost through the Pine
#                   host), test_coof_drawing_rollback_tape (TradingView's
#                   drawing-rollback tape)
#   +3 K-TA-DYNLEN  test_ta_series_length_extremes,
#                   test_ta_series_length_supertrend, test_ta_simple_length
#                   (ta.* with a simple or series length, on TradingView's
#                   tapes)
# No release row skips, so 716 registered is 716 run.
# 720 = those 716 plus lane XSYM-D's four, counted with ctest -N on the lane's
# tree: the two KERNEL_MIN_TESTS rows above, which register here too, and two
# source-bound rows the kernel profile does not build:
#   +1 test_native_instrument_feed_twin (the Pine adapter's request sites of
#      another symbol against the kernel's delivery rule)
#   +1 test_symbol_request_data (the symbol-data C ABI, the recorded series
#      store, the stream refusal)
# 724 = those 720 plus lane K-SESSION-WINDOWS' four, counted with ctest -N on
# the rebased tree: its two KERNEL_MIN_TESTS rows above, which register here
# too, and two source-bound rows:
#   +1 F2 test_session_extended_day_tape (session.isfirstbar / islastbar and
#         their _regular twins through the Pine host)
#   +1 F7 test_session_break_open_bar_tape (aggregated bars that open inside a
#         session break)
# No release row skips, so 724 registered is 724 run.
# 748 = those 724 plus INT28's twenty-two source-bound rows and RUN-HARNESS's
# two kernel-profile rows above, counted with ctest -N on the integrated tree
# (the kernel profile builds none of the twenty-two):
#   +1 TVDEF-DROPS        test_tvdef_drops_tapes
#   +1 W3-ENG-EXIT-ALLOC  test_exit_queue_tapes (W3B-ENG-GRID adds its rows
#                         to this one)
#   +1 W3B-ENG-GRID       test_grid_close_tapes
#   +1 W5-ENG-MARGIN-V6   test_margin_v6_tapes
#   +1 W6-ENG-FILL-ORDER  test_same_point_entries_tapes (W6B-ENG-PAIRS adds
#                         its rows to this one)
#   +1 W8C-SECURITY       test_ltf_lookahead_calling_open_tapes
#   +1 W8D-NOTRADES       test_coof_refill_waypoint_tapes
#   +1 W8E-EXITS          test_ki62_same_id_cover
#   +6 W4-ENG-POOC-SAMEPASS test_pooc_samepass_tapes,
#                         test_global_exit_fill_size_tapes,
#                         test_coof_reissued_exit_tapes,
#                         test_coof_fill_bar_exit_tapes,
#                         test_pooc_margin_call_sizing_tapes,
#                         test_pooc_short_slip_admission_tapes
#   +4 W8A-SIGSTATE-1     test_intraday_loss_open_fill_tape,
#                         test_pyramiding_open_order_tape,
#                         test_pooc_same_pass_close_tape,
#                         test_limit_entry_affordability_tape
#   +2 R1-CONSOLIDATE     test_same_bar_reissue_tapes,
#                         test_open_fill_order_tapes
#   +1 RUN-HARNESS        test_run_harness_window_tapes
#   +1 INT28              test_int28_rule_compositions (two lanes' rules in
#                         one run, on TradingView's tapes)
# No release row skips, so 748 registered is 748 run.
# 751 = those 748 plus INT28-FIX's three source-bound rows, counted the same
# way (the kernel profile builds none of them):
#   +1 INT28-FIX CA       test_reversal_close_all_tapes
#   +1 W5B-ENG-MARGIN-RESIDUAL test_margin_residual_tapes (picked by INT28-FIX)
#   +1 INT28-FIX SS       test_slipped_short_tapes (OU and OF add their rows
#                         to this one)
# No release row skips, so 751 registered is 751 run.
# 752 = those 751 plus INT28-FIX round 3's source-bound row, counted the same
# way (the kernel profile builds none):
#   +1 INT28-FIX RC       test_coof_recalc_security_tapes
# No release row skips, so 752 registered is 752 run.
# 759 = those 752 plus INT29's seven rows, counted the same way on the
# integrated tree (the kernel profile builds only the first):
#   +1 W11-ENG-TIME-COLOR test_color_tapes
#   +4 W10-DIAG-UNKNOWN   test_coqueued_open_margin_tapes (COQ),
#                         test_intraday_cap_tv_tapes (CAP-ORDER, CAP-ON),
#                         test_pooc_gap_stop_tapes (GAPSTOP),
#                         test_same_open_reversal_tapes (SAMEOPEN-REV)
#   +1 W13-ENG-MARGIN-OPP test_margin_opposite_tapes (CP, CQ, CU, CW)
#   +1 INT29              test_int29_rule_compositions (W10's and W5B's
#                         margin rules at one open, on TradingView's tape)
# No release row skips, so 759 registered is 759 run.
# 761 = those 759 plus the two W15-KERNEL-CAL rows KERNEL_MIN_TESTS lists
# above, which register here too. No release row skips, so 761 registered
# is 761 run.
# 785 = those 761 plus INT30's twenty-four source-bound rows, counted with
# ctest -N on the integrated tree (the kernel profile builds none of them):
#   +1 TAIL-A  test_security_multi_day_buckets_tapes (N-day request.security)
#   +1 TAIL-B  test_add_sibling_margin_tapes (AS, AC, LF)
#   +1 TAIL-G  test_foreign_break_stamp_tapes (other symbols' requests on a
#              daily chart stamped in its session's break)
#   +10 TAIL-D test_coof_immediate_close_tapes (M1),
#              test_coof_same_point_exits_tapes (M1e),
#              test_coof_resting_bracket_tapes (M2),
#              test_coof_timeframe_change_tapes (M3),
#              test_exit_fifo_cross_tapes (M6),
#              test_coof_open_limit_tapes (FP17),
#              test_exit_reissue_na_tapes (X1),
#              test_pooc_pair_gross_tapes (H1),
#              test_whole_lot_tie_tapes (JD),
#              test_coof_inflight_cancel_tapes (M7)
#   +1 TAIL-H  test_exit_reservation_tapes (PS, PK)
#   +2 TAIL-E  test_time_bars_back_tapes (M4),
#              test_coof_immediate_close_final_tapes (M5)
#   +7 TAIL-C  test_open_entry_fee_charged (W8E rule A),
#              test_coof_w2_refill_tapes (W2),
#              test_coof_open_newborn_exit_tape (OPEN),
#              test_magnified_coof_refill_tape (MAG),
#              test_pooc_add_close_tape (POOC),
#              test_explicit_market_admission_tapes (ADM),
#              test_nonfinite_entry_qty_tape (INF)
#   +1 INT30   test_int30_rule_compositions (TAIL-B's AS, AC and LF beside
#              TAIL-H's PK, on TradingView's tapes)
# No release row skips, so 785 registered is 785 run.
# 786 = those 785 plus lane TAIL-I's source-bound row, counted with ctest -N
# on the lane's tree (the kernel profile builds none):
#   +1 TAIL-I  test_margin_open_print_tapes (an all-in commissioned and
#              slipped short's calls from the opening print, every point of
#              the path, the follow-up of a call of any size)
# No release row skips, so 786 registered is 786 run.
# 790 = those 786 plus lane XAU-CAL's four rows: the two KERNEL_MIN_TESTS
# lists above and two source-bound ones, counted the same way (the kernel
# profile builds neither):
#   +1 XAU-CAL  test_daily_magnifier_tapes (the bar magnifier on a daily
#               chart, dated by the daily stamps)
#   +1 XAU-CAL  test_symbol_calendar_tapes (the bar after the current one on
#               TradingView's symbol calendar)
# No release row skips, so 790 registered is 790 run.
# 792 = those 790 plus lane FIX-E1E2's two source-bound rows, counted the
# same way (the kernel profile builds neither):
#   +1 FIX-E1E2 test_report_timeframe_ratio (the report's script_tf_ratio
#               and needs_aggregation on every run route)
#   +1 FIX-E1E2 test_aggregated_admission_receipt_tapes (an aggregated
#               chart's admission receipts in their command's bar space)
# No release row skips, so 792 registered is 792 run (SM-E1: below EXCLUDED_REGISTERED_MIN).
# RELEASE_MIN_TESTS itself is defined with KERNEL_MIN_TESTS below, both derived from
# PRIVATE_FIXTURE_PROFILE_FLOORS (the one place the numbers live).
# ADR-0001 ruled-count floors, beside the ctest floors (R5 lane H-DOCGATES,
# AUDIT4-opus X13 / docs-a N7). check_kernel_residuals.py counts the rulings
# its vocabulary reads -- 174 identifiers and 45 texts on the lane's tree: the
# 175 AUDIT4 counted, less the bare word `Pine`, and the 3 feed texts plus
# the 42 texts the lane rules by their exact words (6 deprecated-alias texts,
# map.hpp's 4 static_assert texts, the runtime's 32 dotted argument checks).
# A row the vocabulary reads cannot leave silently -- its name turns unruled
# -- unless its name left in the same change; the floor makes that drop an
# edit of this file. A lane that adds rulings raises the floor with them:
# 46 texts once the lane also ruled Eigen's `matrix.cols() ==
# matrix.rows()` assert text, which a debug or sanitizers archive carries.
# 170 identifiers and 40 texts once lane REL10 removed the four deprecated
# spellings for 1.0 (sharpe_tv, sortino_tv, Coof, MagnifierCoof): their names
# and their six deprecation and static_assert texts left the headers, the
# archive and the ADR's residual table together (INT26 pick of 38e8ad1e).
# 193 identifiers and 59 texts with the run-failure codes' three rows: 23
# identifiers (7 code names, 6 argument values, the 9 generated-code helpers
# and the `pine_source` kind) and 19 texts (18 dotted argument values and the
# `line.get_price` function literal), the public run-failure wire vocabulary
# the catalog docker/run_failure_codes.json publishes.
ADR_RULED_IDENTIFIERS_MIN = 193
ADR_RULED_TEXTS_MIN = 59
# PR-only registration floors: the complete CTest populations of the three
# excluded profiles at INT25, counted with ctest -N on the integrated tree --
# 653/653/662 at 91d65ad6 (INT24) plus wave G's six rows (C-SURFACE-1 +1,
# KERNEL-EDGE +1, K-ULP4 +2, K-ULP5 +1, DOC-TRUTH-4 +1) in each; 683/683/692
# at INT26, each with wave H's twenty-four rows of RELEASE_MIN_TESTS
# (H-MEASURE +12, H-DOCGATES +3, K-OCA-KEEP +1, PAR-ORDERS-2 +4, PAR-MARGIN
# +1, PAR-MARGIN-2 +1, PERF-KEDGE +1, PAR-CASHFEE +1), counted the same way;
# 684/684/693 with INT26 round 2's tape row; 697/697/706 at INT27, each with
# INT27's thirteen rows of RELEASE_MIN_TESTS (K-RUNERR +1, PERF-ZONED +1,
# TV-DEFAULTS +1, MAG-INTRABAR +3, PAR-ORDERS-3 +1, K-DRAWSNAP +3,
# K-TA-DYNLEN +3), counted the same way; 701/701/710 with lane XSYM-D's four
# rows of RELEASE_MIN_TESTS in each, counted the same way; 705/705/714 with
# lane K-SESSION-WINDOWS' four, counted the same way on the rebased tree;
# 729/729/738 at INT28, each with its twenty-four rows of RELEASE_MIN_TESTS,
# counted the same way; 732/732/741 at INT28-FIX, each with its three rows of
# RELEASE_MIN_TESTS, counted the same way; 733/733/742 with INT28-FIX round
# 3's row of RELEASE_MIN_TESTS, counted the same way; 740/740/749 at INT29,
# each with its seven rows of RELEASE_MIN_TESTS, counted the same way;
# 742/742/751 with W15-KERNEL-CAL's two rows, counted the same way;
# 766/766/775 at INT30, each with its twenty-four rows of RELEASE_MIN_TESTS,
# counted the same way; 767/767/776 with lane TAIL-I's row of
# RELEASE_MIN_TESTS, counted the same way;
# 771/771/780 with lane XAU-CAL's four rows of RELEASE_MIN_TESTS in each,
# counted the same way; 773/773/782 with lane FIX-E1E2's two rows of
# RELEASE_MIN_TESTS in each, counted the same way.
# An excluded run must still discover at least this many rows before -LE.
EXCLUDED_REGISTERED_MIN = {
    # +28 rows in every engine profile: verifier 8 + preflight 8 + command-after 8 + report 4.
    # Each is slices + cover - parent; native adds equivalence: 964 + 28 + 8 = 1000.
    'debug': 904, 'sanitizers': 895, 'native': 1000,
    # D1, run 39221717231 of the PR TSV, historical REGISTERED counts
    # (selected + 29 then; the current slow pin covers 60): Debug 938 - 876 = 62 and
    # sanitizers 929 - 867 = 62 below their registry; Release and kernel-only
    # ran full there: 957 and 371. Release: 957 - 62 = 895. Kernel: the offset
    # gives 309, 17 below its registry at that vintage (326), so the original bound
    # used 337 (34 below 371); the four public splits now add 28 rows. The lab's
    # ctest -N confirms both; registration floors, not reduced run floors.
    # Basis of the offset: the existing keys count rows registered BEFORE -LE,
    # so a selected count (909, 900) is never compared with a key (876, 867).
    # See docs/ci.md, "PR tier versus MAIN/RELEASE", and COVERAGE-FLOORS.
    'release': 923, 'kernel': 365,
}
# test_native_live_websocket exits 77 wherever the linked libcurl has no
# WebSocket support (a system libcurl); main's kernel floor counted that skip
# as a run -- its number sat exactly one below the registered count for this
# row -- so the ONE named row may skip without failing the floor, overlay or
# not, and without being pinned. Every other unpinned skip stays a loss.
WEBSOCKET_SKIP_ROW = 'test_native_live_websocket'
# Lane SM-E1 (recorded outputs, docs/outputs.md) raises the three floors
# above, its comments kept here so that no line cited by line number moves.
# Counted with ctest -N on the lane's tree:
#   KERNEL_MIN_TESTS 305 run = those 299 plus six source-free rows:
#     +5 test_outputs_recorder, test_outputs_stream_equivalence,
#        test_outputs_truncation, test_outputs_run_reuse,
#        test_outputs_off_identity
#     +1 test_outputs_c_api_c99 (group pf_outputs as strict C99)
#     All six register in release too. 306 registered, 305 run: the
#     WebSocket row still skips on a system libcurl.
#   RELEASE_MIN_TESTS 799 = those 792 plus the six above and one source-bound
#     row the kernel profile does not build:
#     +1 test_run_json_outputs (docker/run_json.py --outputs as a process,
#        over a module that records outputs and a generated one that records
#        none)
#     No release row skips, so 799 registered is 799 run.
#   EXCLUDED_REGISTERED_MIN 780/779/789 = 773/773/782 plus the seven, counted
#     the same way; the sanitizers profile builds no module that loads into
#     the interpreter, so it registers six of them.
# The 1.5.0-rc.1 selected-window backtests add fifteen rows,
# counted with ctest -N, configure only, on 873b25da and on 9f53a56ab: each
# profile configured with the definitions this file gives it, the runner-built
# ones with the pinned curl 8.14.1 install (--curl-dir) and the corpus
# submodule at its gitlink b40aa8ec, which both commits share:
#   +1 test_compile_inventory_pipeline
#   +1 test_request_feed_inventory_admission
#   +1 test_run_execution_observer
#   +1 test_run_json_selected_window
#   +1 test_run_phase_transport
#   +1 test_selected_primary_plan
#   +1 test_selected_production_routes
#   +1 test_selected_quote_seal
#   +1 test_selected_report_view
#   +1 test_selected_retained_count
#   +1 test_selected_window_native_counts
#   +1 test_selected_window_plan_c
#   +1 test_selected_window_plan_python
#   +1 test_selected_window_report_integration
#   +1 test_selected_window_routes
# All fifteen register in release, debug, sanitizers and native; the kernel
# profile registers thirteen, every one but test_selected_quote_seal and
# test_selected_window_routes, which are source-bound. The runner's own CTest
# tree gains none, so its floor is unchanged. The other floors stood below the
# counts registered at 873b25da (kernel 312 run, release 880,
# debug/sanitizers/native 861/852/948), so each is set to its count at
# 9f53a56ab, not summed onto its old value:
#   RELEASE_MIN_TESTS 895 = the 880 rows registered at 873b25da plus the
#     fifteen; no release row skips, so 895 registered is 895 run.
#   KERNEL_MIN_TESTS 325 run = the 313 registered at 873b25da plus the
#     thirteen = 326 registered, less the WebSocket row that still skips on a
#     system libcurl.
#   EXCLUDED_REGISTERED_MIN 876/867/963 = the 861/852/948 registered at
#     873b25da plus the fifteen, counted before -LE (-LE slow selects
#     847/838/934).
# The verifier supplies no stage or per-test deadline (owner rule,
# 2026-10-12); the platform's job envelope is the only hang catcher.
LIVE_SANITIZERS_MIN_TESTS = 21  # 13 - 1 + 8 probe rows + 1 cover row; see docs/ci.md.
# Overlay-aware row floors. The private-fixture campaign splits the tree in
# two: the private-fixture PR removes the private fixture data, and P1-B
# registers each affected ctest row as a SKIP placeholder in
# tests/CMakeLists.txt (LABELS private_fixtures, SKIP_REGULAR_EXPRESSION
# '^SKIP private fixtures not present: ' -- empty output when the fixtures are
# gone). PINEFORGE_PRIVATE_FIXTURES, set and non-empty, means the private
# fixtures are present (Lab's L6 / S-73 overlay copies them into the tree).
#
#   set     the floor is the profile's FULL floor from
#           PRIVATE_FIXTURE_PROFILE_FLOORS and every one of those rows must
#           RUN: with the overlay present a skip means a row left the suite,
#           so ctest-floor (and ctest-private-fixtures) fails on it.
#   unset   the overlay is absent, so the pinned private-fixture rows the
#           profile's overlay-present registry registers SKIP in public CI as
#           placeholders. The floor is that same FULL floor minus the pinned
#           count PRIVATE_FIXTURE_PROFILE_FLOORS states for the profile: a
#           constant, never the skips this run observed and never the `ctest
#           -N` count it listed, so a run that loses a public row -- from the
#           registry or from the ran count -- falls under the floor however
#           its own registration moved.
# An explicit --min-tests value is used verbatim in BOTH modes and is never
# lowered (R2-F8): the keying is on the profile, not on the floor's value.
#
# EXCLUDED_REGISTERED_MIN is a REGISTERED floor, read only on an
# --exclude-label run, and stays out of the overlay rule entirely. `ctest -N`
# lists a placeholder's row in both modes, but the registered population is
# NOT the same with and without the overlay: the campaign's two-world runs
# measured release 902/901, kernel 331/424, debug 883/882 and sanitizers
# 874/875 (overlay present/absent).
#
# The count is pinned from tests/private_fixture_rows.txt (the row list P1-B
# commits alongside the placeholders): that file's own header records the
# count and the same PR 1 manifest (l44/pr1-manifest.tsv, sha256
# 9a2727d455c1b470d515665428e2c6e090a22ac07f87255d1ca9c20d5528bd62) this
# campaign pins -- a later reader takes the count from that header, and the
# pinned names from the file itself, never from a number quoted here (a count
# in this comment went stale once already). The count is NOT the population
# any one profile lowers by: how many pinned rows each profile registers is a
# measured quantity, stated only in PRIVATE_FIXTURE_PROFILE_FLOORS below. The two
# floor constants above keep their values and their lines so the doc anchors
# at ci_verify.py:321 and ci_verify.py:595 do not move; the campaign's own
# gate is PRIVATE_FIXTURE_PROFILE_FLOORS below, the one place its numbers
# live. The class-fix round added the three remaining split parents (test_coof_close_slippage_tapes, test_cross_mechanism_tapes, test_margin_rules_forward_replay): each reads its fixture data at run time, the private-fixture removals take the whole directory it reads (the removed file beside distributed ones in PR 1, the remaining files in PR 1b), and the row had stayed unconditionally registered -- the fixture-less tree would have run it against absent data. Each now pins whole beside its *_private half on the same EXISTS guard (tests/CMakeLists.txt), exactly as test_source_buying_power_tapes already did at 324; a two-world registration sweep over tests/CMakeLists.txt and runner/CMakeLists.txt closes at `duplicate names: 0` in both worlds. test_callback_lifecycle_tapes joined at 123: its tapes.txt is a distributed list the private-fixture PR leaves in the tree, but 60 of the cases it names are removed, so tests/CMakeLists.txt registers the row as a placeholder while the listed cases are incomplete. One of the 324, test_run_json_outputs, is registered only by the release-shaped profiles (the source layer gates its block); the observed-skip lowering counts it only in the profiles it registers in, so the kernel profile is no longer lowered one row deeper than the rows it skips. Six more joined at 129 (the fifth proof-box round): test_native_oracle_day_key_l2 and test_native_oracle_reversal_same_bar_tx_l2 (their twin source #includes oracle/test_oracle_*.cpp), test_adapter_in_place_reads, test_adapter_runtime_ambient and test_adapter_settlement_carry (tests/pine_d2c_scenarios_fixture.hpp), and test_coof_forced_waypoint_tapes (tests/fixtures/coof_forced_waypoint/cases.inc) -- each target now exists only while its removed include does, so the compile that killed the fifth configure-through run cannot recur. The PR 1b batch moved the remaining 6,242 fixture/probe files (l50/pr1b-inventory.tsv) and took the count to 319: 134 rows in tests/CMakeLists.txt (each a row whose source .cpp, quoted #include or runtime fixture the removal takes, or one of five fixture-fed targets) and 53 in runner/CMakeLists.txt (the strategy-capabilities and confirmed-capabilities e2e rows), and 3 rows whose fixture data the sources read at run time through their fixture-dir defines (test_color_tapes, test_session_transition_clock_tapes, test_source_buying_power_tapes); PR 1's fix round then pinned five rows of its own that PR 1b's 319 does not carry -- test_carried_trail_open_tapes, pinned whole, and the *_private halves of test_coof_close_slippage_tapes, test_cross_mechanism_tapes, test_source_buying_power_tapes and test_margin_rules_forward_replay (each parent row keeps replaying the distributed part, the *_private row beside it runs the same binary with --private-only) -- and the union of the two pin sets, sorted and de-duplicated, is 324; the pinned file header still names the PR 1 manifest sha. The stream-security round took it to 361: tests/CMakeLists.txt registered the fifteen test_stream_security_<case> targets, from fixtures/stream_security/<case>/generated.cpp, inside an unguarded bare-form foreach -- the shape the box's run 1 died on at configure (add_executable could not find the removed source), and the shape the four class sweeps' line walker did not expand -- so each case now sits on its own if(EXISTS ...) and, absent, registers its two rows plus the four test_stream_security_control_* rows as pinned SKIP placeholders: 327 + 34 = 361. The kernel-python round (L6's S-73 two-mode round 4, class 4) took it to 364: three Python rows the kernel profile runs outside the placeholder loop now skip whole when their private input is gone instead of failing on read_text() -- test_native_cpp_abi (scripts/check_native_cpp_abi.py gates on tests/fixtures/native_cpp_abi/), test_pending_order_v1_full_prefix and test_pending_order_v1_compiler_prefix (the two check_pending_order_prefix* scripts gate on tests/fixtures/script_cpp_abi/basev8 and basev9) -- each printing the pinned literal and exiting 77 under SKIP_RETURN_CODE 77, each carrying LABELS private_fixtures so the pinned-vs-labelled self-test still closes at zero. The same round replaced the static lowering (floor minus PRIVATE_FIXTURE_ROW_COUNT, which read -36 for the kernel floor) with an observed-skip lowering, bounded then by a floor-minimum constant of 1; PR 1b's second pass (L5 01:43's ruling) replaced that bound with a per-profile margin. The fix round (R2-F1) replaced that whole machinery with PRIVATE_FIXTURE_PROFILE_FLOORS: the observed-skip lowering moved with the registry it was meant to check (release absent ran 603 against a floor of 602 after two of main's rows had already left the registry, and up to 99 kernel rows could vanish with the floor still met), and no margin repairs a floor that follows the run down. PR 1b's first pass, L5 01:37, took it back down to 347: it had also guarded the 17 legacy-owner base names (L3A_LEGACY_OWNER_TEST_SOURCES intersected with the private rows), which main never compiles -- only their _l4a/_l4b/_l4c/_l4d twins run -- and the fix dropped those guard blocks and moved this count and the pinned file together (364 -> 347), restoring main's registry. PR 1b's second pass (L5 01:43, class 3) took it to 351: the four receipt-gated rows in tests/CMakeLists.txt are now registered on an EXISTS guard over the seven ABI-provider manifests their providers gate on, so absent manifests make each the pinned SKIP placeholder (the literal, LABELS private_fixtures, SKIP_RETURN_CODE 77) and present manifests leave the registration exactly as it was -- check_abi_receipt_skips no longer counts them as failures whose input is simply absent, the preparer's own 77+literal is a skip rather than a failed preparation (ensure_prepared_provider accepts it), and the four skips are pinned accounting like every other private-fixture row's. The class-sweep round (the private-fixtures lane's iteration 2) took it to 354: scripts/check_private_fixture_reads.py found three rows (test_coof_marketable_stop_tapes, test_pooc_close_bar_fills_tapes, test_pooc_pending_stop_tapes) still registered by the TEST_SOURCES loop while every case each replays sits under a removed directory -- they ran on the fixture-less tree and aborted -- and each now pins whole on a removed file it reads, with the kernel-absent branch its source-bound TU needs.
PRIVATE_FIXTURE_ROW_COUNT = 376
PRIVATE_FIXTURE_LABEL = 'private_fixtures'
PRIVATE_FIXTURE_ROWS_FILE = 'tests/private_fixture_rows.txt'
# The literal every private-fixtures placeholder prints before CTest marks it
# SKIP; tests/CMakeLists.txt pins it as each such row's SKIP_REGULAR_EXPRESSION.
PRIVATE_FIXTURE_SKIP_LITERAL = 'SKIP private fixtures not present: '
# THE table: the two private-fixture profiles' row floors, the one place the
# campaign's numbers live. Per profile `full` is that profile's
# overlay-present REGISTERED population -- the RUN floor while the overlay is
# present, where every one of those rows must run -- and `pinned` is how many
# rows of tests/private_fixture_rows.txt that overlay-present registry
# registers; absent the overlay those pinned rows register as SKIP
# placeholders, so the absent floor is `full - pinned`.
#
# Re-measured at 024fdf92c from that head's four Lab jobs, each job's own
# `ctest`. Release overlay rj-20261012t003156-430909: 910 tests, 0 skipped,
# 4 failed (this lane's harness rows plus test_doc_lint), 300 pinned. Release
# absent rj-20261012t003153-098c2a: 910 registered, 610 ran, those 300
# skipped. Kernel overlay rj-20261012t003157-d19ae9: 337 tests, 0 skipped,
# the same four failed, 28 pinned. Kernel absent rj-20261012t003154-1c9988:
# 337 registered, 309 ran, those 28 skipped. No row skipped in either overlay
# job, so those four failures are that head's own red rows, not floor losses.
# Both `full` numbers come from the OVERLAY-PRESENT registry only: never from
# the mode's own `ctest -N`, never from a run's observed skips. That is what
# makes an absent run's floor unable to sink with the registry it is meant to
# check. debug, sanitizers, native and the live profiles carry no row floor of
# this kind (their Profile.min_tests is None or LIVE_SANITIZERS_MIN_TESTS) and
# are not keyed by this table. The bound L5 states -- never below `full` minus
# PRIVATE_FIXTURE_ROW_COUNT -- holds by construction: `pinned` is a subset of
# that file's names.
# The two overlay full floors are plain module-level integer literals; the proof kit
# reads each from one line, and the table references the names. Re-measuring a row
# means editing these literals and the `pinned` counts in the table.
# +28 public rows: verifier 8 + preflight 8 + command-after 8 + report 4; 910 + 28 = 938, 337 + 28 = 365.
RELEASE_MIN_TESTS = 938
KERNEL_MIN_TESTS = 365
PRIVATE_FIXTURE_PROFILE_FLOORS = {
    'release': (RELEASE_MIN_TESTS, 307),
    'kernel': (KERNEL_MIN_TESTS, 31),
}

# The earlier manifest-controls split replaced one public row with four shards (+3
# registered/run rows in every engine profile, in either fixture world).
# These pinned minimum floors (absent: 938 - 307 = 631, 365 - 31 = 334) and the PR
# registration minima remain lower bounds; more rows only raise the count.
# The receipt/private-fixture populations and runner-only floors are unchanged.


def private_fixtures_overlay() -> bool:
    """True when PINEFORGE_PRIVATE_FIXTURES is set and non-empty.

    The environment variable, not a file, carries the signal: Lab's S-73
    overlay exports it into the gate that runs with the fixtures present.
    """
    return bool(os.environ.get('PINEFORGE_PRIVATE_FIXTURES', '').strip())


def profile_full_floor(name: str) -> 'int | None':
    """The profile's FULL overlay-present run floor, from the one table."""
    entry = PRIVATE_FIXTURE_PROFILE_FLOORS.get(name)
    return None if entry is None else entry[0]


def overlay_aware_floor(profile: 'Profile') -> 'int | None':
    """The profile's RUN floor with the private-fixture overlay taken into account.

    For the profiles in PRIVATE_FIXTURE_PROFILE_FLOORS the overlay-present
    floor is the table's `full` population -- held there too, so a lost public
    row fails with the overlay as well -- and the overlay-absent floor is
    `full` minus the table's `pinned` count, the rows that profile registers
    as SKIP placeholders without the overlay. Both are constants of the
    profile: the floor never moves with this run's registry, its skips or its
    own `ctest -N` count, so a run that loses a public row -- from the
    registry or from the ran count -- falls under it whatever its own
    registration did, and a row that starts skipping is a loss with no margin
    left to absorb it. Every other profile gets its declared Profile.min_tests
    unchanged, or None where it declares none; an explicit --min-tests value
    never reaches this function and is used verbatim (R2-F8).
    """
    entry = PRIVATE_FIXTURE_PROFILE_FLOORS.get(profile.name)
    if entry is None:
        return profile.min_tests
    full, pinned = entry
    if private_fixtures_overlay():
        return full
    return full - min(pinned, PRIVATE_FIXTURE_ROW_COUNT)


def expand_equivalence_test_names(names: set[str] | frozenset[str], source: Path) -> frozenset[str]:
    """Replace the equivalence driver's parent name with its probe rows."""
    import ast

    parent = 'native_live_equivalence_e2e'
    if parent not in names:
        return frozenset(names)
    script = source / 'tests' / (parent + '.py')
    declarations = [node for node in ast.parse(script.read_text()).body
                    if isinstance(node, ast.Assign) and any(
                        isinstance(target, ast.Name) and target.id == 'PROBES'
                        for target in node.targets)]
    if len(declarations) != 1 or not isinstance(declarations[0].value, ast.Dict):
        raise ValueError('the equivalence driver must declare one literal PROBES dictionary')
    probes = [ast.literal_eval(key) for key in declarations[0].value.keys]
    if (not probes or any(not isinstance(probe, str)
                         or not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', probe)
                         for probe in probes) or len(probes) != len(set(probes))):
        raise ValueError('the equivalence driver must declare distinct probe slugs')
    return frozenset(names - {parent}) | frozenset(parent + '_' + probe for probe in probes)


def private_fixture_rows(source: Path, *, expand_equivalence: bool = True) -> 'frozenset[str]':
    """The ctest rows carrying LABELS private_fixtures, from the pinned list.

    tests/CMakeLists.txt gives the label to exactly the rows P1-B lists in
    tests/private_fixture_rows.txt. The equivalence parent pin denotes its
    probe rows; expand_equivalence=False reads the raw pins for the unchanged
    file-count integrity check. Comment lines start with ``//``.
    """
    names = set()
    for line in (source / PRIVATE_FIXTURE_ROWS_FILE).read_text().splitlines():
        name = line.strip()
        if name and not name.startswith('//'):
            names.add(name)
    return (expand_equivalence_test_names(names, source) if expand_equivalence
            else frozenset(names))


# tests/test_scratch_arena_jailed_seals.cpp returns 77 in its non-Linux
# branch; tests/cr150.d/core-cr150-cr267.cmake registers it on every host.
# No other unpinned platform skip is authorized by this table.
PLATFORM_CONDITIONAL_SKIP_ROWS: dict[str, tuple[Callable[[str], bool], str]] = {
    'test_scratch_arena_jailed_seals': (
        lambda host_platform: host_platform != 'linux',
        'Linux memfd seals are required'),
}


def permitted_exclusion_skips(pinned: frozenset[str], host_platform: str) -> frozenset[str]:
    """Only absent-fixture pins and named skips whose platform predicate holds."""
    return pinned | frozenset(
        name for name, (may_skip, _) in PLATFORM_CONDITIONAL_SKIP_ROWS.items()
        if may_skip(host_platform))


# Static slow-label policy: the 43-name CMake loop plus 18
# direct LABELS assignments. Never derive this bound from the run's registry.
# Conditional registration may omit pins; every discovered slow name must
# belong to this set. A new slow row requires an explicit policy change.
# Each verifier and command-after split replaces one slow name with eight (+7).
# Four report slices replace one direct name (+3); 43 loop + 18 direct = 61, covers unlabelled.
PINNED_SLOW_TEST_ROWS = frozenset((
    # tests/CMakeLists.txt: the timing list, labelled by its slow loop.
    'test_adapter_live_state_equivalence',
    'test_adapter_live_state_scaling',
    'test_adapter_reissue_binding',
    'test_aggregate_cpp_versions',
    'test_broker_state_hash_coverage_mutations',
    'test_chart_day_memo',
    *(f'test_ci_verify_slice_{index}' for index in range(1, 9)),
    'test_l10recon1_merge_fill_liveness',
    'test_local_time_fields',
    'test_magnifier_endpoints4',
    'test_native_bar_open_hook',
    'test_native_command_after_slice_1',
    'test_native_command_after_slice_2',
    'test_native_command_after_slice_3',
    'test_native_command_after_slice_4',
    'test_native_command_after_slice_5',
    'test_native_command_after_slice_6',
    'test_native_command_after_slice_7',
    'test_native_command_after_slice_8',
    'test_native_cpp_abi',
    'test_native_cpp_versions',
    'test_native_definition_index',
    'test_native_direct_mutation',
    'test_native_event_retention',
    'test_native_group_absorption',
    'test_native_group_keep_handle',
    'test_native_handle_stable_replace',
    'test_native_host_reads',
    'test_native_lookup_memos',
    'test_native_match_band_precheck',
    'test_native_match_row_reuse',
    'test_native_precommit_hook',
    'test_native_quiet_point',
    'test_native_runtime_ambient',
    'test_native_script_bucket_completions',
    'test_native_settlement_carry',
    # tests/CMakeLists.txt:7354.
    'test_run_json_selected_sink_memory_l9s',
    # tests/CMakeLists.txt:7354 (report-memory bound row).
    'test_report_sink_scratch_curve_resident',
    # tests/cr150.d/core-cr150-cr257.cmake:10.
    'test_cr257_source_exit_scratch_io_100k',
    # tests/cr150.d/core-cr150-l18.cmake:6,9.
    'test_position_tail_scale_out',
    'test_position_tail_scale_out_100k',
    # tests/cr150.d/core-cr150-l22u.cmake:5,8.
    'test_arena_bound_runtime',
    'test_arena_bound_corpus',
    # Public report population slices and the related report rows.
    'test_open_report_rows_flat_slice_1',
    'test_open_report_rows_flat_slice_2',
    'test_open_report_rows_flat_slice_3',
    'test_open_report_rows_flat_slice_4',
    'test_open_report_rows_retention_flat',
    'test_report_trades_summary_flat',
    # tests/cr150.d/o1w-ledger.cmake:4,7.
    'test_ledger_history_scale',
    'test_ledger_history_scale_400k',
    # tests/cr150.d/o1w-tail.cmake:6.
    'test_position_tail_summary_scale',
    # tests/cr150.d/p2cx-a5s.cmake:3.
    'test_o1_store',
    # tests/cr150.d/core-cr150-cohort.cmake:5,11 (Release-only timing ratio).
    'test_adapter_spaced_ids_scaling',
    # tests/cr150.d/p2cx-a5arr.cmake:5.
    'test_array_growth_1m',
    # tests/cr150.d/report-trade-retirement.cmake and report-trade-reference.cmake:
    # the closed-trade retirement witnesses, the identity row against the frozen
    # reference, the reference corruption row and the sink-consumer row.
    'test_report_sink_trade_comparison',
    'test_report_sink_trade_history',
    'test_report_sink_trade_identity',
    'test_report_sink_trade_legacy',
    'test_report_sink_trade_reference',
    'test_report_sink_trade_residency',
    'test_report_sink_trade_retirement',
    'test_sink_history_consumers',
))


# CTest's closing summary: '100% tests passed out of N' when nothing failed,
# '97% tests passed, 3 tests failed out of N' otherwise. N includes a skipped
# row (counted as passed) and a row CTest could not start (counted as
# failed), but not a disabled one; the lists printed after it tell them apart.
CTEST_ROW_COUNT = re.compile(
    r'% tests passed(?:, (?P<failed>\d+) tests? failed)? out of (?P<total>\d+)')
# A row listed after the summary: '\t182 - name (Skipped)' or '(Disabled)' under 'The
# following tests did not run:'; '\t  6 - name (Not Run)', '(Failed)', '(Timeout)', ...
# under 'The following tests FAILED:'. CTest appends a labelled row's labels ('  slow').
CTEST_LISTED_ROW = re.compile(r'^\s*\d+ - (.+?) \(([^()\n]+)\)(?:[ \t]+[^\s()][^()\n]*)?[ \t]*$', re.MULTILINE)
# A skipped row's own result line: ' 4/10 Test  #3: name .....***Skipped   0.01 sec'.
CTEST_SKIPPED_RESULT = re.compile(r'^\s*\d+/\d+ Test\s+#\d+: .*\*\*\*Skipped\b', re.MULTILINE)
CTEST_LIST_COUNT = re.compile(r'^Total Tests:\s*(\d+)\s*$', re.MULTILINE)
CTEST_LIST_NAME = re.compile(r'^\s*Test\s+#(?P<number>\d+):\s*(?P<name>.+?)\s*$', re.MULTILINE)
# LeakSanitizer is unavailable in Apple's ASan runtime.  Keep the Linux CI
# lane strict, while allowing the local macOS ASan/UBSan profile to execute
# its actual instrumented tests instead of failing during runtime startup.
_ASAN_LEAKS = '0' if sys.platform == 'darwin' else '1'
SANITIZER_RUN_ENV = {
    'ASAN_OPTIONS': f'detect_leaks={_ASAN_LEAKS}:halt_on_error=1:abort_on_error=1',
    'UBSAN_OPTIONS': 'print_stacktrace=1:halt_on_error=1',
}
SOURCE_GUARD_SCRIPTS = (
    ('source-guard-c-abi', ['scripts/check_c_abi_runtime.py']),
    ('source-guard-native-source', ['scripts/test_native_source_guard.py']),
    ('source-guard-broker-hash', ['scripts/check_broker_state_hash_coverage.py']),
    ('source-guard-pending-mirror', ['scripts/gen_pending_order_mirror.py', '--check']),
    ('source-guard-native-versions', ['scripts/check_native_cpp_versions.py']),
    ('source-guard-native-c-surface', ['scripts/check_native_c_api_surface.py']),
    ('source-guard-aggregate-versions', ['scripts/check_aggregate_cpp_versions.py']),
    ('source-guard-adapter-spec-shadowing', ['scripts/check_adapter_spec_shadowing.py']),
    ('source-guard-dangling-comment-names', ['scripts/check_dangling_comment_names.py']),
    # R5 run-failure codes: the catalog against its generated enum and registry,
    # the published catalog of the newest release tag and the uncoded throws;
    # then the catalog's per-release diff, recomputed byte for byte. Both read
    # the newest release tag, so every checkout that runs them fetches history.
    ('source-guard-run-failure-codes', ['scripts/check_run_failure_codes.py']),
    ('source-guard-run-failure-diff', ['scripts/gen_run_failure_catalog_diff.py', '--check']),
    ('source-guard-selected-window-ownership', ['scripts/check_selected_window_ownership.py']),
)
NATIVE_INCLUDE_INDEPENDENCE_PROFILES = frozenset(('release', 'native', 'kernel'))
# The kernel-only archive must name no TradingView vocabulary outside ADR-0001's
# residual table (R5 gap lane P2). The archive exists in every profile and the
# CTest row test_kernel_residuals checks it there too; this stage is the
# kernel profile's own claim, run right after the build so a failing name is
# reported before the suite runs.
KERNEL_RESIDUALS_PROFILES = frozenset(('kernel',))
TWIN_PARITY_PROFILES = frozenset(('release', 'native'))
# The Pine-free hosts under examples/native/ are built, and their example_*
# ctest rows executed, in the two profiles they are written for: the default
# release build and the kernel-only one. Every example links PineForge::kernel.
EXAMPLES_PROFILES = frozenset(('release', 'kernel'))
# A compile's CMake target, read from its object directory as CMake names it
# for every generator: <dir>/CMakeFiles/<target>.dir/<source>.o.
OBJECT_DIR = re.compile(r'(?:^|[/\\])CMakeFiles[/\\]([^/\\]+)\.dir[/\\]')
# The two static archives every profile builds, by CMake target: libpineforge.a
# (the kernel, and the source layer wherever the profile compiles it) and
# libpineforge_kernel.a (the kernel alone). stale-binaries holds each against
# the files its own compiles read.
ARCHIVE_TARGETS = ('pineforge', 'pineforge_kernel')
INCLUDE_DIRECTIVE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)


class ConfigError(Exception):
    """CLI / profile validation; exit status 2."""


CTEST_SHARED_STAGES = frozenset(name for name, _ in SOURCE_GUARD_SCRIPTS) | frozenset({
    'source-guard-twin-parity', 'native-include-independence', 'kernel-residuals',
    'install', 'smoke-configure', 'smoke-build', 'smoke-version', 'smoke-version-check',
    'native-help', 'require-websocket',
})
CTEST_SHARD_PROFILES = frozenset((
    'release', 'debug', 'sanitizers', 'kernel', 'native', 'live-sanitizers', 'live-tsan',
))
# Pinned to shards.mjs ABSENT_SKIP_STAGES, not a prefix or a moving guard list.
CTEST_ABSENT_SKIP_STAGES = frozenset((
    'source-guard-c-abi', 'source-guard-native-source', 'source-guard-broker-hash',
    'source-guard-pending-mirror', 'source-guard-native-versions', 'source-guard-native-c-surface',
    'source-guard-aggregate-versions', 'source-guard-adapter-spec-shadowing',
    'source-guard-dangling-comment-names', 'source-guard-run-failure-codes',
    'source-guard-run-failure-diff', 'source-guard-selected-window-ownership',
    'abi-base', 'abi-prior', 'abi-v13', 'abi-v14',
    'abi-v15-frozen', 'abi-v16-frozen', 'abi-v18-frozen',
))


@dataclass(frozen=True)
class CTestShard:
    index: int
    count: int

    def __post_init__(self) -> None:
        if (type(self.index) is not int or type(self.count) is not int
                or not 1 <= self.index <= self.count <= 8):
            raise ConfigError('--ctest-shard requires 1 <= I <= N <= 8')


def parse_ctest_shard(value: str) -> CTestShard:
    if not re.fullmatch(r'[1-8]/[1-8]', value):
        raise ConfigError('--ctest-shard must be I/N with 1 <= I <= N <= 8')
    index, count = map(int, value.split('/'))
    return CTestShard(index, count)


def ctest_registry(output: bytes) -> tuple[str, ...]:
    """Read every discovery row without collapsing duplicate names or numbers."""
    matches = list(CTEST_LIST_NAME.finditer(output.decode('utf-8')))
    names = tuple(match.group('name') for match in matches)
    numbers = [int(match.group('number')) for match in matches]
    if (not names or ctest_list_count(output) != len(names)
            or len(set(names)) != len(names) or len(set(numbers)) != len(numbers)):
        raise ValueError('CTest registry is empty, truncated or contains duplicate rows')
    return names


def ctest_registry_sha256(names: tuple[str, ...] | list[str]) -> str:
    """Hash UTF-8 names sorted lexically, each followed by one LF."""
    from hashlib import sha256
    if not names or len(set(names)) != len(names):
        raise ValueError('CTest registry is empty or contains duplicate rows')
    if any(not isinstance(name, str) or not name or '\n' in name or '\r' in name
           for name in names):
        raise ValueError('CTest registry names must be nonempty single lines')
    return sha256(''.join(name + '\n' for name in sorted(names)).encode('utf-8')).hexdigest()


def ctest_partitions(names: tuple[str, ...] | list[str],
                     count: int) -> tuple[tuple[str, ...], ...]:
    """Assign sorted names by SHA-256(UTF-8 name), big-endian integer modulo N.

    Adding a name never moves an existing name at fixed N. No timing table,
    Python hash seed, host state or discovery order participates.
    """
    from hashlib import sha256
    CTestShard(1, count)
    ctest_registry_sha256(names)
    partitions: list[list[str]] = [[] for _ in range(count)]
    for name in sorted(names):
        shard = int.from_bytes(sha256(name.encode('utf-8')).digest(), 'big') % count
        partitions[shard].append(name)
    if any(not part for part in partitions):
        raise ValueError('CTest shard would select zero rows; use fewer shards')
    return tuple(tuple(part) for part in partitions)


CTEST_NAMED_RESULT = re.compile(
    r'^\s*\d+/\d+ Test\s+#\d+:\s*(?P<name>.+?)\s+\.{2,}\s*'
    r'(?P<result>.+?)\s+\d+(?:\.\d+)? sec\s*$', re.MULTILINE)


def ctest_shard_evidence(output: bytes, selected: tuple[str, ...]) -> dict[str, list[str]]:
    """Prove actual result names, never infer that an unreported row ran."""
    text = output.decode('utf-8')
    rows = ctest_rows(output)
    if rows is None:
        raise ValueError('CTest printed no closing summary')
    summary = ctest_summary(text)
    matches = list(CTEST_NAMED_RESULT.finditer(text[:summary.start()]))
    observed = [match.group('name') for match in matches]
    if len(observed) != len(set(observed)):
        raise ValueError('CTest printed a duplicated result row')
    if set(observed) != set(selected) or not selected:
        raise ValueError('CTest result names differ from the selected shard (lost or extra row)')
    evidence = {'ran': [], 'skipped': [], 'notRun': [], 'disabled': []}
    for match in matches:
        result = match.group('result')
        category = ('disabled' if '(Disabled)' in result else
                    'skipped' if result.startswith('***Skipped') else
                    'notRun' if result.startswith('***Not Run') else 'ran')
        evidence[category].append(match.group('name'))
    expected = {'skipped': rows.skipped, 'notRun': rows.not_run, 'disabled': rows.disabled}
    for category, names in expected.items():
        if (len(names) != len(set(names))
                or sorted(names) != sorted(evidence[category])):
            raise ValueError('CTest result/list disagreement for ' + category)
    if rows.total != len(selected) - len(rows.disabled) or rows.ran != len(evidence['ran']):
        raise ValueError('CTest summary does not account for every selected shard row')
    return {category: sorted(names) for category, names in evidence.items()}


def ctest_shared_stages(profile: str, require_websocket: bool) -> frozenset[str]:
    """The profile-specific shard 1 proof set, mirrored in docs/ci.md."""
    names = {name for name, _ in SOURCE_GUARD_SCRIPTS} | {
        'install', 'smoke-configure', 'smoke-build', 'smoke-version', 'smoke-version-check',
    }
    if profile in TWIN_PARITY_PROFILES:
        names.add('source-guard-twin-parity')
    if profile in NATIVE_INCLUDE_INDEPENDENCE_PROFILES:
        names.add('native-include-independence')
    if profile in KERNEL_RESIDUALS_PROFILES:
        names.add('kernel-residuals')
    if PROFILE[profile].live_runner:
        names.add('native-help')
    if require_websocket:
        names.add('require-websocket')
    return frozenset(names)


def ctest_every_stages(profile: str) -> frozenset[str]:
    """Required local proofs for the profile's shard stage table."""
    names = {
        'tool-versions', 'corpus-submodule-init', 'corpus-submodule-status', 'corpus-submodule-pin',
        'configure', 'profile-options', 'build', 'stale-binaries', 'native-binary',
        'ctest-list-all', 'ctest-list-shard', 'ctest', 'ctest-floor', 'ctest-shard-accounting',
    }
    if profile == 'kernel' or profile in LIVE_ONLY_PROFILES:
        names.add('abi-providers-skipped')
    else:
        names.update(('abi-base', 'abi-prior', 'abi-v13', 'abi-v14',
                      'abi-v15-frozen', 'abi-v16-frozen', 'abi-v18-frozen'))
    if profile in EXAMPLES_PROFILES or PROFILE[profile].live_runner:
        names.add('examples-assert-live')
    if PROFILE[profile].sanitizers:
        names.add('sanitizer-public-flag')
    if profile in LIVE_ONLY_PROFILES:
        names.update(('live-test-inventory', 'live-test-inventory-required'))
    if profile == 'live-sanitizers':
        names.add('live-sanitizer-coverage')
    if profile == 'live-tsan':
        names.add('runner-thread-sanitizer-coverage')
    return frozenset(names)


def validate_ctest_shard_union(
        summaries: list[dict], *,
        read_stage_log: Callable[[dict, dict], str | None] | None = None) -> None:
    """Check ctest-shard-union/v1 leaf and union evidence, never invent proofs.

    The caller supplies original logs for authorized absent-fixture skips.
    Tree/receipt identity, overlay pins and CTest key lines remain lab checks.
    """
    if not summaries:
        raise ValueError('no CTest shard summaries')
    first = summaries[0]
    count = first['ctestShard']['count']
    CTestShard(1, count)
    if len(summaries) != count:
        raise ValueError('missing or duplicated CTest shard summary')
    profile = first['profile']
    if profile not in CTEST_SHARD_PROFILES:
        raise ValueError('profile is outside the lab CTest shard contract')

    def names_valid(names) -> bool:
        return isinstance(names, list) and all(
            isinstance(name, str) and name and '\r' not in name and '\n' not in name
            for name in names)

    registry = first['ctestRegisteredNames']
    if not names_valid(registry):
        raise ValueError('invalid CTest registry names')
    digest = ctest_registry_sha256(registry)
    exclude_label = PROFILE[profile].exclude_label
    excluded = first.get('ctestExcludedNames')
    if (not names_valid(excluded) or excluded != sorted(set(excluded))
            or not set(excluded) <= set(registry)
            or (exclude_label is None and excluded)):
        raise ValueError('invalid CTest excluded names')
    expected_selection = sorted(set(registry) - set(excluded))
    overlay = first['ctestPrivateFixturesOverlay']
    if type(overlay) is not bool:
        raise ValueError('fixture mode must be a boolean')
    floor = first['minTests']
    entry = PRIVATE_FIXTURE_PROFILE_FLOORS.get(profile)
    minimum = None if entry is None else entry[0] - (0 if overlay else entry[1])
    if profile in LIVE_ONLY_PROFILES:
        if type(first['requireWebsocket']) is not bool:
            raise ValueError('runner WebSocket requirement must be a boolean')
        # Runner-only profiles automatically require WebSocket with transport.cpp.
        # Validate remote evidence without following its source path.
        minimum = LIVE_SANITIZERS_MIN_TESTS - int(
            not first['requireWebsocket'] and WEBSOCKET_SKIP_ROW not in registry)
    if ((floor is not None and (type(floor) is not int or floor < 0 or len(registry) < floor))
            or (minimum is not None and (type(floor) is not int or floor < minimum))):
        raise ValueError('lowered the registration floor')

    every = ctest_every_stages(profile)
    shared = ctest_shared_stages(profile, first['requireWebsocket'])

    def passed(stage: dict) -> bool:
        return (stage.get('status') == 'passed' and type(stage.get('exitCode')) is int
                and stage['exitCode'] == 0)

    def absent_skip(summary: dict, stage: dict) -> bool:
        if (overlay or stage['name'] not in CTEST_ABSENT_SKIP_STAGES
                or stage.get('status') != 'skipped' or type(stage.get('exitCode')) is not int
                or stage['exitCode'] != 77 or stage.get('log') != f"ci-logs/{stage['name']}.log"
                or read_stage_log is None):
            return False
        text = read_stage_log(summary, stage)
        return isinstance(text, str) and re.search(
            r'^SKIP private fixtures not present: [^\r\n]+$', text, re.MULTILINE) is not None

    stage_tables = {}
    for summary in summaries:
        stages = summary.get('stages')
        if (not isinstance(stages, list)
                or any(not isinstance(stage, dict) or not isinstance(stage.get('name'), str)
                       for stage in stages)
                or len({stage['name'] for stage in stages}) != len(stages)):
            raise ValueError('malformed or duplicated stage')
        stage_tables[id(summary)] = {stage['name']: stage for stage in stages}
    indices, union = set(), set()
    counted = 0
    owner = next((summary for summary in summaries
                  if summary['ctestShard']['index'] == 1), None)
    if owner is None:
        raise ValueError('missing shard 1 stage proofs')
    owner_stages = stage_tables[id(owner)]
    for name in sorted(shared):
        proof = owner_stages.get(name)
        if proof is None or not (passed(proof) or absent_skip(owner, proof)):
            raise ValueError('no successful shard 1 proof for ' + name)
    for summary in summaries:
        shard = summary['ctestShard']
        CTestShard(shard['index'], shard['count'])
        if shard['count'] != count or shard['index'] in indices:
            raise ValueError('mismatched or duplicated CTest shard index/count')
        indices.add(shard['index'])
        if (summary['status'] != 'passed' or type(summary['exitCode']) is not int
                or summary['exitCode'] != 0 or summary.get('failures') != []
                or summary['profile'] != profile
                or type(summary['minTests']) is not type(floor) or summary['minTests'] != floor
                or summary['excludeLabel'] != exclude_label
                or summary.get('ctestExcludedNames') != excluded
                or summary['requireWebsocket'] != first['requireWebsocket']
                or (profile in LIVE_ONLY_PROFILES
                    and type(summary['requireWebsocket']) is not bool)
                or type(summary['ctestPrivateFixturesOverlay']) is not bool
                or summary['ctestPrivateFixturesOverlay'] != overlay
                or summary['ctestRegistrySha256'] != digest
                or not names_valid(summary['ctestRegisteredNames'])
                or sorted(summary['ctestRegisteredNames']) != sorted(registry)
                or type(summary['ctestRegistered']) is not int
                or summary['ctestRegistered'] != len(registry)):
            raise ValueError('failed shard or mismatched CTest registry hash/configuration')
        selection = summary['ctestUnshardedSelectedNames']
        if (not names_valid(selection) or selection != expected_selection
                or type(summary['ctestUnshardedSelected']) is not int
                or summary['ctestUnshardedSelected'] != len(expected_selection)):
            raise ValueError('changed unsharded selection')
        lists = [summary[key] for key in
                 ('ctestRan', 'ctestSkipped', 'ctestNotRun', 'ctestDisabled')]
        if not all(names_valid(names) for names in lists):
            raise ValueError('missing full row NAME lists')
        names = [name for category in lists for name in category]
        if not names or len(names) != len(set(names)) or union.intersection(names):
            raise ValueError('empty shard or duplicated CTest row')
        if (not set(names) <= set(expected_selection)
                or any(type(summary[key]) is not int
                       for key in ('ctestSelected', 'ctestRows', 'ctestTotal'))
                or summary['ctestSelected'] != len(names)
                or summary['ctestRows'] != len(summary['ctestRan'])
                or summary['ctestTotal'] != len(names) - len(summary['ctestDisabled'])):
            raise ValueError('CTest shard count/name disagreement')
        if summary['ctestNotRun'] or summary['ctestDisabled']:
            raise ValueError('CTest shard contains not-run or disabled rows')
        union.update(names)
        counted += summary['ctestRows']
        if (profile not in LIVE_ONLY_PROFILES and not overlay
                and WEBSOCKET_SKIP_ROW in summary['ctestSkipped']):
            counted += 1
        reported = stage_tables[id(summary)]
        required = every | shared
        if profile in LIVE_ONLY_PROFILES and summary['ctestSkipped']:
            required |= {'live-test-skips'}
        for name in sorted(required):
            if name not in reported:
                raise ValueError('did not prove required stage ' + name)
        if shard['index'] != 1:
            for name in sorted(shared):
                if reported[name]['status'] != 'deferred':
                    raise ValueError('shared stage must defer to its shard 1 proof: ' + name)
        for stage in summary['stages']:
            if stage['status'] == 'deferred':
                if (stage['name'] not in shared or shard['index'] == 1
                        or type(stage.get('ownerShard')) is not int or stage['ownerShard'] != 1
                        or stage['exitCode'] is not None or stage['name'] in every):
                    raise ValueError('deferred stage has no successful shard 1 proof')
            elif not (passed(stage) or absent_skip(summary, stage)):
                raise ValueError('stage did not pass: ' + stage['name'])
    if union != set(expected_selection):
        raise ValueError('CTest shard union lost or added a row')
    if floor is not None and counted < floor:
        raise ValueError('CTest union ran fewer rows than the unchanged global floor')


@dataclass(frozen=True)
class Profile:
    name: str
    build_type: str
    sanitizers: bool
    live_runner: bool
    tutorial: bool
    source_layer: bool
    # Minimum CTest rows the profile must run; None leaves the count ungated.
    min_tests: int | None = None
    thread_sanitizers: bool = False

    @property
    def exclude_label(self) -> str | None:
        # Runner-only profiles select runner/, which has no slow ledger rows.
        # Every other Debug build compiles the quadratic Debug-only checks.
        return ('slow' if self.build_type == 'Debug' and self.name not in LIVE_ONLY_PROFILES
                else None)


PROFILE = {
    'release': Profile('release', 'Release', False, False, True, True,
                       profile_full_floor('release')),
    'debug': Profile('debug', 'Debug', False, False, True, True),
    'sanitizers': Profile('sanitizers', 'Debug', True, False, True, True),
    'native': Profile('native', 'Release', False, True, False, True),
    'live-sanitizers': Profile('live-sanitizers', 'Debug', True, True, False, True,
                               LIVE_SANITIZERS_MIN_TESTS),
    'live-tsan': Profile('live-tsan', 'Debug', False, True, False, True,
                         LIVE_SANITIZERS_MIN_TESTS, True),
    'kernel': Profile('kernel', 'Release', False, True, False, False,
                      profile_full_floor('kernel')),
}


def profile_min_tests(profile: Profile, source: Path) -> int | None:
    if profile.name in LIVE_ONLY_PROFILES and not (source / 'runner' / 'transport.cpp').is_file():
        return LIVE_SANITIZERS_MIN_TESTS - 1
    # The profile's declared floor is its full count; the private-fixture
    # overlay decides whether the placeholder rows are expected to run.
    return overlay_aware_floor(profile)


def runner_e2e_test_names(source: Path) -> set[str]:
    # Require every driver, including corpus equivalence: native-live.yml
    # supplies git-lfs and ensure_corpus_submodule fetches the corpus/feed.
    # Missing prerequisites may skip in direct CTest, but fail CI (docs/ci.md).
    return set(expand_equivalence_test_names(
        {path.stem for path in (source / 'tests').glob('native_live*_e2e.py')}, source))


@dataclass
class Completed:
    returncode: int
    stdout: bytes = b''
    stderr: bytes = b''


Runner = Callable[..., Completed]


@dataclass
class VerifyConfig:
    profile: Profile
    source: Path
    build_dir: Path
    jobs: int
    generator: str | None
    curl_dir: Path | None
    ccache_path: str | None
    require_websocket: bool
    runner: Runner
    stream_output: bool = True
    exclude_label: str | None = None
    # The effective CTest row floor: --min-tests, else the profile's own.
    min_tests: int | None = None
    # True when --min-tests was given: the floor's own note keys on the table
    # having lowered the profile floor, never on a value that merely matches.
    min_tests_explicit: bool = False
    ctest_shard: CTestShard | None = None


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise ConfigError(message)


def cmake_on(value: str | None) -> bool:
    return (value or '').strip().upper() in {'1', 'ON', 'TRUE', 'YES', 'Y'}


def expected_version(source: Path) -> str:
    return ''.join((source / 'VERSION').read_text().split())


def default_build_dir(source: Path, profile: str) -> Path:
    return source / f'build-ci-{profile}'


def call_runner(runner: Runner, argv: list[str], *, extra_env: dict[str, str] | None = None,
                combine_stderr: bool = True,
                stream_output: bool = False) -> Completed:
    return runner(argv, extra_env=extra_env,
                  combine_stderr=combine_stderr, stream_output=stream_output)


def ctest_summary(text: str) -> re.Match | None:
    """CTest's closing summary: the last one, after every test's own output."""
    match = None
    for match in CTEST_ROW_COUNT.finditer(text):
        pass
    return match


def ctest_row_count(output: bytes) -> int | None:
    """The row count CTest prints in its closing summary line, or None."""
    match = ctest_summary(output.decode('utf-8', 'replace'))
    return int(match.group('total')) if match else None


def ctest_list_count(output: bytes) -> int | None:
    """Read CTest's own `ctest -N` discovery count, failing closed on ambiguity."""
    counts = CTEST_LIST_COUNT.findall(output.decode('utf-8', 'replace'))
    return int(counts[0]) if len(counts) == 1 else None


def ctest_list_names(output: bytes) -> 'frozenset[str] | None':
    """The row names a `ctest -N` listing carries, or None when it carries none.

    CTest prints one ``Test #N: name`` line per registered row above its
    ``Total Tests:`` line. The caller uses this registry to ask which pinned
    rows a run registered; a listing with no readable names fails closed
    rather than answering about an empty registry.
    """
    names = frozenset(match.group('name')
                      for match in CTEST_LIST_NAME.finditer(
                          output.decode('utf-8', 'replace')))
    return names or None


@dataclass(frozen=True)
class CTestRows:
    """The rows of one CTest run: its own count, and those that did not run."""
    total: int
    # SKIP_RETURN_CODE / SKIP_REGULAR_EXPRESSION rows; inside total, as passed.
    skipped: tuple[str, ...] = ()
    # Rows CTest could not start (a missing executable); inside total, as failed.
    not_run: tuple[str, ...] = ()
    # DISABLED rows; CTest leaves them out of total.
    disabled: tuple[str, ...] = ()

    @property
    def ran(self) -> int:
        """Rows whose test ran to a verdict, pass or fail: what a floor counts."""
        return self.total - len(self.skipped) - len(self.not_run)

    def not_counted(self) -> str:
        """The rows left out of `ran`, as a clause for the floor's message."""
        parts = [f'{len(names)} {label} ({", ".join(names)})'
                 for label, names in (('skipped', self.skipped), ('not run', self.not_run),
                                      ('disabled', self.disabled)) if names]
        return '; not counted: ' + ', '.join(parts) if parts else ''


def ctest_rows(output: bytes) -> CTestRows | None:
    """Which CTest rows ran, or None when CTest printed no closing summary.

    The summary alone cannot tell a row that ran from one CTest skipped (it
    counts that as passed); the lists printed after it can. Raises ValueError
    when those lists disagree with the output -- a FAILED list whose length is
    not the summary's failed count, or skipped result lines the did-not-run
    list does not match -- so a floor fails closed on output it cannot read.
    """
    text = output.decode('utf-8', 'replace')
    summary = ctest_summary(text)
    if summary is None:
        return None
    total = int(summary.group('total'))
    failed_count = int(summary.group('failed') or 0)
    listed = CTEST_LISTED_ROW.findall(text[summary.end():])
    skipped = tuple(name for name, why in listed if why == 'Skipped')
    disabled = tuple(name for name, why in listed if why == 'Disabled')
    failed = [(name, why) for name, why in listed if why not in ('Skipped', 'Disabled')]
    not_run = tuple(name for name, why in failed if why == 'Not Run')
    if len(failed) != failed_count:
        raise ValueError(f'the summary counts {failed_count} failed rows, the FAILED list '
                         f'names {len(failed)}')
    skipped_lines = len(CTEST_SKIPPED_RESULT.findall(text[:summary.start()]))
    if skipped_lines != len(skipped):
        raise ValueError(f'{skipped_lines} result lines say Skipped, the did-not-run list '
                         f'names {len(skipped)} skipped rows')
    if len(skipped) + len(not_run) > total:
        raise ValueError(f'{len(skipped) + len(not_run)} rows did not run out of {total}')
    return CTestRows(total, skipped, not_run, disabled)


def ctest_supports_junit(runner: Runner) -> bool:
    try:
        result = call_runner(runner, ['ctest', '--help'], stream_output=False)
    except OSError:
        return False
    return '--output-junit' in (result.stdout + result.stderr).decode('utf-8', 'replace')


def source_guard_commands(source: Path) -> list[tuple[str, list[str]]]:
    python = sys.executable
    return [(name, [python, str(source / rel[0]), *rel[1:]]) for name, rel in SOURCE_GUARD_SCRIPTS]


def native_include_independence_command(cfg: VerifyConfig, prefix: Path) -> list[str]:
    argv = [sys.executable, str(cfg.source / 'scripts/check_native_include_independence.py'),
            '--build-dir', str(cfg.build_dir), '--prefix', str(prefix)]
    if not cfg.profile.source_layer:
        argv += ['--kernel-archive', str(cfg.build_dir / 'lib' / 'libpineforge_kernel.a')]
    return argv


def twin_parity_command(source: Path) -> list[str]:
    return [sys.executable, str(source / 'scripts/check_twin_parity.py')]


def kernel_residuals_command(cfg: VerifyConfig) -> list[str]:
    return [sys.executable, str(cfg.source / 'scripts/check_kernel_residuals.py'),
            '--archive', str(cfg.build_dir / 'lib' / 'libpineforge_kernel.a'),
            '--adr', str(cfg.source / 'docs/adr/0001-kernel-adapter-boundary.md'),
            '--min-ruled-identifiers', str(ADR_RULED_IDENTIFIERS_MIN),
            '--min-ruled-texts', str(ADR_RULED_TEXTS_MIN)]


def cmake_cache_definitions(cfg: VerifyConfig) -> dict[str, str]:
    profile = cfg.profile
    values = {
        'CMAKE_BUILD_TYPE': profile.build_type,
        'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON',
        # Preserve the invoking virtualenv path: resolving its symlink can
        # silently select the base interpreter for CMake-registered tests.
        'Python3_EXECUTABLE': sys.executable,
        'PINEFORGE_BUILD_TESTS': 'ON',
        'PINEFORGE_BUILD_TUTORIAL': 'ON' if profile.tutorial else 'OFF',
        'PINEFORGE_BUILD_LIVE_RUNNER': 'ON' if profile.live_runner else 'OFF',
        'PINEFORGE_BUILD_SOURCE_LAYER': 'ON' if profile.source_layer else 'OFF',
        'PINEFORGE_ENABLE_SANITIZERS': 'ON' if profile.sanitizers else 'OFF',
        'PINEFORGE_BUILD_CORPUS_STRATEGIES': 'OFF',
        'PINEFORGE_BUILD_BENCH_STRATEGIES': 'OFF',
        'PINEFORGE_BUILD_SPEED_BENCH': 'OFF',
        'PINEFORGE_BUILD_EXAMPLES': 'ON' if profile.name in EXAMPLES_PROFILES else 'OFF',
        'PINEFORGE_ENABLE_COVERAGE': 'OFF',
        'PINEFORGE_STRICT_WARNINGS': 'OFF',
        'PINEFORGE_REQUIRE_ABI_RECEIPTS': 'ON',
        'PINEFORGE_VERSION_SOURCE': 'FILE',
    }
    if profile.live_runner:
        values['PINEFORGE_LIVE_TSAN'] = 'ON' if profile.thread_sanitizers else 'OFF'
    if cfg.curl_dir is not None:
        values['CURL_DIR'] = str(cfg.curl_dir)
    if cfg.ccache_path:
        values['CMAKE_C_COMPILER_LAUNCHER'] = cfg.ccache_path
        values['CMAKE_CXX_COMPILER_LAUNCHER'] = cfg.ccache_path
    return values


def cmake_configure_argv(cfg: VerifyConfig) -> list[str]:
    argv = ['cmake', '-S', str(cfg.source), '-B', str(cfg.build_dir)]
    if cfg.generator:
        argv += ['-G', cfg.generator]
    argv += [f'-D{key}={value}' for key, value in cmake_cache_definitions(cfg).items()]
    return argv


def parse_args(argv: list[str] | None, *, source: Path = ROOT) -> argparse.Namespace:
    parser = Parser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        'profile', choices=PROFILES,
        help='release/debug keep tutorial ON and native OFF; '
             'sanitizers enable PUBLIC ASan/UBSan; native enables the live runner; '
             'live-sanitizers/live-tsan instrument every runner target and run all runner tests; '
             'kernel is native with the Pine source layer built OFF')
    parser.add_argument('--build-dir', type=Path, default=None,
                        help='default: <source>/build-ci-PROFILE')
    parser.add_argument('--jobs', type=int, default=DEFAULT_JOBS)
    parser.add_argument('--generator', default=None)
    parser.add_argument('--curl-dir', type=Path, default=None,
                        help='CURL CMake package dir (native WS-enabled curl install)')
    parser.add_argument('--ccache', action='store_true',
                        help='require installed ccache and bind CMAKE_*_COMPILER_LAUNCHER')
    parser.add_argument('--require-websocket', action='store_true',
                        help='native/live-sanitizers/live-tsan: execute test_native_live_websocket and '
                             'refuse skip (77); automatic in runner sanitizer profiles while transport.cpp exists')
    parser.add_argument('--exclude-label', default=None,
                        help='exclude one CTest label; verify the run count against '
                             'CTest discovery with and without -LE; default: slow '
                             'for Debug build types except runner-only profiles '
                             '(currently debug and sanitizers); otherwise no exclusion; '
                             'slow names must belong to PINNED_SLOW_TEST_ROWS')
    parser.add_argument('--ctest-shard', type=parse_ctest_shard, default=None, metavar='I/N',
                        help='run deterministic CTest shard I of N (1 <= I <= N <= 8); '
                             'all shards are required for a full verification')
    parser.add_argument('--min-tests', type=int, default=None,
                        help='minimum executed CTest rows for full runs and Debug-build-type '
                             'profiles (ctest-floor); with a label exclusion also raises '
                             'the registration floor; other excluded profiles use only '
                             'the registration bound; skipped or not-run rows do not count '
                             'toward a Debug execution floor; '
                             'the defaults are the profiles\' own floors, held in '
                             'PRIVATE_FIXTURE_PROFILE_FLOORS for kernel and release '
                             '(the overlay-present full population, minus the pinned '
                             'private-fixture rows while the overlay is absent), in '
                             f'Profile.min_tests elsewhere, and at {LIVE_SANITIZERS_MIN_TESTS} '
                             f'(or {LIVE_SANITIZERS_MIN_TESTS - 1} without transport.cpp) for '
                             'the runner sanitizer profiles; explicit execution floors '
                             'are used verbatim')
    args = parser.parse_args(argv)
    if args.build_dir is None:
        args.build_dir = default_build_dir(source, args.profile)
    return args


def validate_config(args: argparse.Namespace, *, source: Path = ROOT,
                    which: Callable[[str], str | None] = shutil.which) -> VerifyConfig:
    if not JOBS_MIN <= args.jobs <= JOBS_MAX:
        raise ConfigError(f'--jobs must be {JOBS_MIN}..{JOBS_MAX}')
    if args.require_websocket and args.profile not in {'native', 'live-sanitizers', 'live-tsan'}:
        raise ConfigError('--require-websocket is only valid with the native or '
                          'live-sanitizers or live-tsan profile')
    if args.profile in LIVE_ONLY_PROFILES and args.exclude_label is not None:
        raise ConfigError(f'{args.profile} must run every runner row; --exclude-label is invalid')
    if args.exclude_label is not None:
        label = args.exclude_label.strip()
        if not label or any(not (char.isalnum() or char in '_.-') for char in label):
            raise ConfigError('--exclude-label must be a simple CTest label')
        args.exclude_label = label
    exclude_label = (args.exclude_label if args.exclude_label is not None else
                     PROFILE[args.profile].exclude_label)
    if args.curl_dir is not None and not args.curl_dir.is_dir():
        raise ConfigError(f'--curl-dir is not a directory: {args.curl_dir}')
    if args.min_tests is not None and args.min_tests < 1:
        raise ConfigError('--min-tests must be at least 1')
    if args.ctest_shard is not None and args.ctest_shard.count > 1:
        if args.profile not in CTEST_SHARD_PROFILES:
            raise ConfigError('--ctest-shard with N > 1 requires release, debug, sanitizers, kernel, '
                              'native, live-sanitizers or live-tsan')
        if exclude_label is not None and exclude_label != PROFILE[args.profile].exclude_label:
            raise ConfigError('--ctest-shard with N > 1 only permits the declared '
                              'Debug-build-type exclusion of label slow')
        minimum = profile_min_tests(PROFILE[args.profile], source)
        if minimum is not None and args.min_tests is not None and args.min_tests < minimum:
            raise ConfigError('--ctest-shard must not lower the undivided profile floor '
                              f'of {minimum}')
    ccache_path = None
    if args.ccache:
        found = which('ccache')
        if not found:
            raise ConfigError('optional --ccache requires an installed ccache tool')
        ccache_path = str(Path(found).resolve())
    for tool in ('cmake', 'ctest', 'git'):
        if not which(tool):
            raise ConfigError(f'required tool not found: {tool}')
    build_dir = args.build_dir.expanduser()
    build_dir = build_dir.resolve() if build_dir.is_absolute() else (Path.cwd() / build_dir).resolve()
    source = source.resolve()
    if build_dir == source:
        raise ConfigError('--build-dir cannot be the source root')
    return VerifyConfig(
        profile=PROFILE[args.profile],
        source=source,
        build_dir=build_dir,
        jobs=args.jobs,
        generator=args.generator,
        curl_dir=args.curl_dir.resolve() if args.curl_dir is not None else None,
        ccache_path=ccache_path,
        require_websocket=bool(args.require_websocket or
                               (args.profile in LIVE_ONLY_PROFILES and
                                (source / 'runner' / 'transport.cpp').is_file())),
        runner=default_runner,
        exclude_label=exclude_label,
        min_tests=(args.min_tests if args.min_tests is not None else
                   profile_min_tests(PROFILE[args.profile], source)),
        min_tests_explicit=args.min_tests is not None,
        ctest_shard=args.ctest_shard,
    )


def ctest_command(cfg: VerifyConfig, test_dir: Path, *, jobs: int, junit: bool) -> list[str]:
    argv = ['ctest', '--test-dir', str(test_dir), '--output-on-failure',
            '--no-tests=error', '--parallel', str(jobs)]
    # Explicit zero requests no default per-test deadline; a row's own
    # TIMEOUT property remains untouched.
    argv += ['--timeout', '0']
    if cfg.exclude_label:
        argv += ['-LE', cfg.exclude_label]
    if junit:
        argv += ['--output-junit', str(cfg.build_dir / 'ctest-junit.xml')]
    return argv


def default_runner(argv: list[str], *, extra_env: dict[str, str] | None = None,
                   combine_stderr: bool = True,
                   stream_output: bool = True) -> Completed:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    stderr = subprocess.STDOUT if combine_stderr else subprocess.PIPE
    try:
        result = subprocess.run(
            argv, stdout=subprocess.PIPE, stderr=stderr, env=env)
    except OSError as error:
        return Completed(127, b'', str(error).encode())
    if stream_output and result.stdout:
        sys.stdout.buffer.write(result.stdout)
        sys.stdout.buffer.flush()
    return Completed(result.returncode, result.stdout or b'',
                     b'' if combine_stderr else (result.stderr or b''))


def compile_command_text(entry: dict) -> str:
    if 'command' in entry:
        return entry['command']
    return ' '.join(entry.get('arguments') or [])


def sanitizer_flag_on_library(build_dir: Path) -> bool:
    path = build_dir / 'compile_commands.json'
    if not path.is_file():
        raise RuntimeError('compile_commands.json missing; sanitizer PUBLIC flag cannot be verified')
    entries = json.loads(path.read_text())
    library = []
    for entry in entries:
        file = (entry.get('file') or '').replace('\\', '/')
        if not file.endswith('/src/matrix.cpp') and Path(file).name != 'matrix.cpp':
            continue
        if '/src/' in file or Path(file).parent.name == 'src':
            library.append(compile_command_text(entry))
    if not library:
        raise RuntimeError('compile_commands.json has no src/matrix.cpp entry')
    return all(SANITIZER_FLAG in command for command in library)


def ndebug_defined(argv: list[str]) -> bool:
    """Whether NDEBUG is defined once the compiler has read argv in order."""
    defined = False
    tokens = iter(argv)
    for token in tokens:
        if token in ('-D', '-U'):
            token += next(tokens, '')
        if token == '-DNDEBUG' or token.startswith('-DNDEBUG='):
            defined = True
        elif token == '-UNDEBUG':
            defined = False
    return defined


def compile_argv(entry: dict) -> list[str]:
    """One compile_commands.json entry's compiler argv."""
    return entry.get('arguments') or shlex.split(entry.get('command') or '')


def compile_unit(entry: dict) -> Path:
    """The translation unit one compile_commands.json entry compiles, resolved."""
    unit = Path(entry.get('file') or '')
    if not unit.is_absolute():
        unit = Path(entry.get('directory') or '.') / unit
    return unit.resolve()


def compile_target(entry: dict, argv: list[str]) -> str | None:
    """The CMake target one compile_commands.json entry builds an object for."""
    output = entry.get('output') or next(
        (value for flag, value in zip(argv, argv[1:]) if flag == '-o'), '')
    match = OBJECT_DIR.search(output)
    return match.group(1) if match else None


def runner_translation_units(source: Path) -> list[Path]:
    return sorted(unit for unit in (source / 'runner').rglob('*')
                  if unit.suffix.lower() in {'.c', '.cc', '.cpp', '.cxx'} and unit.is_file())


def runner_sanitizer_coverage(build_dir: Path, source: Path,
                              sanitizer_flag: str = SANITIZER_FLAG) -> list[str]:
    path = build_dir / 'compile_commands.json'
    if not path.is_file():
        raise RuntimeError('compile_commands.json missing; runner instrumentation cannot be verified')
    runner_build = (build_dir / 'runner').resolve()
    compiled = set()
    coverage = []
    for entry in json.loads(path.read_text()):
        argv = compile_argv(entry)
        output = entry.get('output') or next(
            (value for flag, value in zip(argv, argv[1:]) if flag == '-o'), '')
        object_path = Path(output)
        if not object_path.is_absolute():
            object_path = Path(entry.get('directory') or build_dir) / object_path
        if runner_build not in object_path.resolve().parents:
            continue
        unit = compile_unit(entry)
        target = compile_target(entry, argv)
        location = (str(unit.relative_to(source.resolve())) if unit.is_relative_to(source.resolve())
                    else f'{unit} (outside source tree)')
        label = f'{target}: {location}'
        disabled = any(flag.startswith('-fno-sanitize=') or
                       flag == '-fomit-frame-pointer' for flag in argv)
        if sanitizer_flag not in argv or '-fno-omit-frame-pointer' not in argv or disabled:
            instrumentation = 'ASan/UBSan' if sanitizer_flag == SANITIZER_FLAG else 'ThreadSanitizer'
            raise RuntimeError(f'runner compile lacks {instrumentation}/frame pointers: {label}')
        compiled.add(unit)
        coverage.append(label)
    required = {unit.resolve() for unit in runner_translation_units(source)}
    missing = required - compiled
    if not coverage or missing:
        raise RuntimeError('runner compile commands missing: ' +
                           ', '.join(os.path.relpath(unit, source.resolve())
                                     for unit in sorted(missing)))
    return sorted(coverage)


def example_targets_with_ndebug(build_dir: Path, source: Path) -> tuple[list[str], list[str]]:
    """The targets compile_commands.json builds from an examples/native source,
    and those built with NDEBUG.

    Every example checks its own results with assert(), and two of them are
    compiled twice: as their example_* executable (examples/native/) and as
    the MODULE the live runner dlopens (runner/CMakeLists.txt), which
    test_native_example_batch and test_native_example_selected drive. A
    compile that leaves NDEBUG defined turns every assert() in it into a no-op.
    """
    path = build_dir / 'compile_commands.json'
    if not path.is_file():
        raise RuntimeError("compile_commands.json missing; the examples' assert() state "
                           'cannot be verified')
    examples = (source / 'examples' / 'native').resolve()
    targets, with_ndebug = set(), set()
    for entry in json.loads(path.read_text()):
        unit = compile_unit(entry)
        if examples not in unit.parents:
            continue
        argv = compile_argv(entry)
        target = compile_target(entry, argv) or str(unit)
        targets.add(target)
        if ndebug_defined(argv):
            with_ndebug.add(target)
    if not targets:
        raise RuntimeError('compile_commands.json has no compile of an examples/native source')
    return sorted(targets), sorted(with_ndebug)


def include_search_path(argv: list[str], directory: Path) -> tuple[list[Path], list[Path]]:
    """Where one compile looks for a header: the extra directories of a
    "quoted" include (-iquote; the including file's own directory comes
    first), then those of every include (-I, then -isystem), in the compiler's
    order."""
    quote, ordinary, system = [], [], []
    tokens = iter(argv)
    for token in tokens:
        for flag, found in (('-iquote', quote), ('-isystem', system), ('-I', ordinary)):
            if token == flag:
                value = next(tokens, '')
            elif token.startswith(flag):
                value = token[len(flag):]
            else:
                continue
            found.append(directory / value)
            break
    return quote, ordinary + system


def archive_inputs(source: Path, build_dir: Path, target: str) -> list[Path]:
    """Every file of the source or the build tree that the compiles of target's objects read.

    That is each translation unit compile_commands.json compiles into target,
    and every header they reach through #include that resolves inside either
    tree -- the pineforge/version.h CMake generates into the build tree
    included, wherever that tree lives: a "quoted" include is looked up beside
    the including file first, then in the compile's -iquote, -I and -isystem
    directories; an <angled> one in the last two. A header outside both trees
    (the standard library, a system Eigen) is neither an input nor followed.
    The scan reads #include lines as text: one under a false #if still counts,
    one spelled through a macro is not followed, and a header reached from
    several units is read with the first one's search path (an archive's units
    share theirs).
    """
    path = build_dir / 'compile_commands.json'
    if not path.is_file():
        raise RuntimeError(f'compile_commands.json missing; the inputs of {target} cannot be listed')
    trees = (source.resolve(), build_dir.resolve())

    def in_trees(candidate: Path) -> bool:
        return any(tree in candidate.parents for tree in trees)

    units = []
    for entry in json.loads(path.read_text()):
        argv = compile_argv(entry)
        if compile_target(entry, argv) == target:
            directory = Path(entry.get('directory') or build_dir)
            units.append((compile_unit(entry), include_search_path(argv, directory)))
    if not units:
        raise RuntimeError(f'compile_commands.json has no compile command of target {target}')
    read, includes = set(), {}
    for unit, (quote_dirs, dirs) in units:
        pending = [unit]
        while pending:
            current = pending.pop()
            if current in read:
                continue
            read.add(current)
            if current not in includes:
                includes[current] = INCLUDE_DIRECTIVE.findall(current.read_text(errors='replace'))
            for kind, name in includes[current]:
                search = [current.parent, *quote_dirs, *dirs] if kind == '"' else dirs
                found = next((candidate for candidate in (directory / name for directory in search)
                              if candidate.is_file()), None)
                if found is not None and in_trees(found.resolve()):
                    pending.append(found.resolve())
    return sorted(path for path in read if in_trees(path))


def stale_archive_sources(source: Path, archive: Path) -> list[str]:
    """The files the archive <build>/lib/lib<target>.a is compiled from that are newer than it.

    An archive's inputs are what its own compiles read (archive_inputs), not
    every file under src/, include/ and CMakeLists.txt: the kernel profile's
    archives never compile src/source/ or src/compat/pine/, and no archive
    compiles a header only an example or a test includes, so an edit to one of
    those rebuilds no archive and is no sign of a stale build. A change to
    CMakeLists.txt that matters reaches the archive as a changed compile.
    """
    if not archive.is_file():
        raise RuntimeError(f'full build did not produce {archive}')
    named = re.fullmatch(r'lib(.+)\.a', archive.name)
    if named is None:
        raise RuntimeError(f'{archive} is not a static archive named lib<target>.a')
    newest = archive.stat().st_mtime
    source = source.resolve()
    return [str(path.relative_to(source)) if source in path.parents else str(path)
            for path in archive_inputs(source, archive.parent.parent, named.group(1))
            if path.stat().st_mtime > newest]


def smoke_prefix(cache: dict[str, str], install_prefix: Path) -> str:
    parts = [str(install_prefix)]
    extra = cache.get('CMAKE_PREFIX_PATH')
    if extra:
        for item in extra.split(';'):
            if item and item not in parts:
                parts.append(item)
    eigen = cache.get('Eigen3_DIR')
    if eigen and eigen not in parts:
        parts.append(eigen)
    return ';'.join(parts)


def pinned_object_present(source: Path, runner: Runner, commit: str = BASE_COMMIT) -> bool:
    result = call_runner(
        runner, ['git', '-C', str(source), 'cat-file', '-e', commit + '^{commit}'],
        stream_output=False)
    return result.returncode == 0


def tool_version_commands(cfg: VerifyConfig) -> list[list[str]]:
    commands = [
        ['cmake', '--version'],
        ['ctest', '--version'],
        ['git', '--version'],
        [sys.executable, '--version'],
    ]
    compiler = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
    if compiler:
        commands.append([compiler, '--version'])
    if cfg.ccache_path:
        commands.append([cfg.ccache_path, '--version'])
    return commands


class Driver:
    def __init__(self, cfg: VerifyConfig):
        self.cfg = cfg
        self.logs = cfg.build_dir / 'ci-logs'
        self.install_prefix = cfg.build_dir / 'ci-install'
        self.smoke_dir = cfg.build_dir / 'ci-smoke'
        self.summary_path = cfg.build_dir / 'ci-summary.json'
        self.failures: list[dict] = []
        self.stages: list[dict] = []
        self.expected_version = expected_version(cfg.source)
        self.actual_version: str | None = None
        self.abi_action = 'not-started'
        self.abi_prior_action = 'not-started'
        self.abi_v13_action = 'not-started'
        self.abi_v14_action = 'not-started'
        self.abi_v15_frozen_action = 'not-started'
        self.abi_v16_frozen_action = 'not-started'
        self.abi_v18_frozen_action = 'not-started'
        self.summary: dict = {
            'schemaVersion': SCHEMA,
            'status': 'incomplete',
            'profile': cfg.profile.name,
            'source': str(cfg.source),
            'buildDir': str(cfg.build_dir),
            'installPrefix': str(self.install_prefix),
            'smokeDir': str(self.smoke_dir),
            'jobs': cfg.jobs,
            'generator': cfg.generator,
            'ccache': cfg.ccache_path,
            'requireWebsocket': cfg.require_websocket,
            'curlDir': str(cfg.curl_dir) if cfg.curl_dir else None,
            'minTests': cfg.min_tests,
            'excludeLabel': cfg.exclude_label,
            'ctestRegistered': None,
            'ctestSelected': None,
            # Rows that ran, the floor's count; CTest's own count; the rows
            # it did not run, listed beside ctestRows and never counted.
            'ctestRows': None,
            'ctestTotal': None,
            'ctestSkipped': None,
            'ctestNotRun': None,
            'ctestDisabled': None,
            'versionSource': 'FILE',
            'cmakeDefinitions': cmake_cache_definitions(cfg),
            'expectedVersion': self.expected_version,
            'actualVersion': None,
            'exitCode': None,
            'abi': {'action': self.abi_action},
            'abiPrior': {'action': self.abi_prior_action},
            'abiV13': {'action': self.abi_v13_action},
            'abiV14': {'action': self.abi_v14_action},
            'abiV15Frozen': {'action': self.abi_v15_frozen_action},
            'abiV16Frozen': {'action': self.abi_v16_frozen_action},
            'abiV18Frozen': {'action': self.abi_v18_frozen_action},
            'stages': self.stages,
            'failures': self.failures,
        }
        if cfg.ctest_shard is not None:
            self.summary.update({
                'ctestShard': {'index': cfg.ctest_shard.index, 'count': cfg.ctest_shard.count},
                'ctestRegisteredNames': None,
                'ctestRegistrySha256': None,
                'ctestExcludedNames': [],
                'ctestUnshardedSelected': None,
                'ctestUnshardedSelectedNames': None,
                'ctestPrivateFixturesOverlay': private_fixtures_overlay(),
                'ctestRan': None,
            })

    def shared_stage(self, name: str) -> bool:
        """Only shard 1 proves this stage; a deferral is never a passed check."""
        if name not in CTEST_SHARED_STAGES:
            raise ValueError('stage is not in the documented shard 1 table: ' + name)
        shard = self.cfg.ctest_shard
        if shard is None or shard.index == 1:
            return True
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text('Deferred to shard 1; this shard did not execute the check.\n')
        self.stages.append({
            'name': name, 'argv': [], 'exitCode': None, 'status': 'deferred',
            'ownerShard': 1, 'reason': 'shared stage; requires shard 1 result',
            'log': str(log.relative_to(self.cfg.build_dir)),
            'extraEnvKeys': [],
        })
        self.write_summary()
        return False

    def run_sharded_ctest(self, test_dir: Path, jobs: int, all_rows: Completed,
                          selected_rows: Completed) -> Completed | None:
        try:
            if all_rows.returncode or selected_rows.returncode:
                raise ValueError('CTest discovery command failed')
            registry = ctest_registry(all_rows.stdout + all_rows.stderr)
            selection = ctest_registry(selected_rows.stdout + selected_rows.stderr)
            if not set(selection) <= set(registry):
                raise ValueError('label-selected CTest names are outside the registry')
            shard = self.cfg.ctest_shard
            if (shard.count > 1 and self.cfg.exclude_label is not None
                    and self.cfg.exclude_label != self.cfg.profile.exclude_label):
                raise ValueError('ctest-shard-union/v1 only permits the declared '
                                 'Debug-build-type exclusion')
            excluded = ()
            if self.cfg.exclude_label:
                labelled = self.invoke(
                    'ctest-list-excluded',
                    ['ctest', '--test-dir', str(test_dir), '-N', '-L', self.cfg.exclude_label],
                    stream_output=False)
                if labelled.returncode:
                    raise ValueError('CTest excluded-label discovery command failed')
                excluded = ctest_registry(labelled.stdout + labelled.stderr)
            self.summary['ctestExcludedNames'] = sorted(excluded)
            if (not set(excluded) <= set(registry)
                    or set(selection) != set(registry) - set(excluded)):
                raise ValueError('CTest selection must equal the registry minus labelled exclusions')
            if shard.count > 1 and self.cfg.exclude_label == 'slow':
                unpinned = sorted(set(excluded) - PINNED_SLOW_TEST_ROWS)
                if unpinned:
                    raise ValueError('CTest excluded unpinned slow rows: ' + ', '.join(unpinned))
            partitions = ctest_partitions(selection, shard.count)
            selected = partitions[shard.index - 1]
            self.summary.update({
                'ctestRegistered': len(registry),
                'ctestRegisteredNames': sorted(registry),
                'ctestRegistrySha256': ctest_registry_sha256(registry),
                'ctestUnshardedSelected': len(selection),
                'ctestUnshardedSelectedNames': sorted(selection),
                'ctestSelected': len(selected),
            })
            # Anchors and escaping make the regex an exact name set. Ask
            # CTest to prove that interpretation before executing anything.
            expression = '^(' + '|'.join(re.escape(name) for name in selected) + ')$'
            listing = ['ctest', '--test-dir', str(test_dir), '-N', '-R', expression]
            if self.cfg.exclude_label:
                listing += ['-LE', self.cfg.exclude_label]
            discovered = self.invoke('ctest-list-shard', listing, stream_output=False)
            if (discovered.returncode
                    or set(ctest_registry(discovered.stdout + discovered.stderr)) != set(selected)):
                raise ValueError('CTest shard filter lost or added a row')
        except (OSError, ValueError) as error:
            self.fail_stage('ctest-shard-discovery', str(error))
            return None
        command = ctest_command(self.cfg, test_dir, jobs=jobs,
                                junit=ctest_supports_junit(self.cfg.runner))
        # 1/1 keeps the unsharded execution command and accounting.
        if shard.count != 1:
            command += ['-R', expression]
        ran = self.invoke('ctest', command, extra_env=self.sanitizer_env())
        try:
            evidence = ctest_shard_evidence(ran.stdout + ran.stderr, selected)
        except ValueError as error:
            self.fail_stage('ctest-shard-accounting', str(error))
            return ran
        self.summary['ctestRan'] = evidence['ran']
        if shard.count == 1:
            self.enforce_test_floor(ran, registered=len(registry), selected=len(selection),
                                    registered_names=frozenset(registry),
                                    excluded_names=frozenset(excluded))
            if self.cfg.min_tests is None:
                self.pass_stage('ctest-floor', 'no execution floor configured; '
                                'the complete 1/1 name set was accounted')
        else:
            self.enforce_sharded_test_floor(ran, registry, selection, selected)
        self.pass_stage(
            'ctest-shard-accounting',
            f'shard {shard.index}/{shard.count}: {len(selected)} result names exactly '
            f'match discovery; registry {len(registry)} sha256 '
            + self.summary['ctestRegistrySha256'])
        return ran

    def enforce_sharded_test_floor(self, ran: Completed, registry: tuple[str, ...],
                                   selection: tuple[str, ...],
                                   selected: tuple[str, ...]) -> None:
        """Global floors bind the registry; every local selected row is accounted."""
        rows = ctest_rows(ran.stdout + ran.stderr)  # already checked by name
        self.summary.update({
            'ctestTotal': rows.total, 'ctestRows': rows.ran,
            'ctestSkipped': list(rows.skipped), 'ctestNotRun': list(rows.not_run),
            'ctestDisabled': list(rows.disabled),
        })
        overlay = private_fixtures_overlay()
        if overlay:
            self.reject_private_fixture_skips(rows)
        stage = 'ctest-floor'
        pinned = frozenset()
        absent = self.cfg.profile.name in PRIVATE_FIXTURE_PROFILE_FLOORS and not overlay
        if not overlay and (self.cfg.exclude_label or self.cfg.min_tests is not None):
            pinned = self.pinned_private_fixture_rows(stage)
            if pinned is None:
                return
        if set(selection) != set(registry) - set(self.summary['ctestExcludedNames']):
            self.fail_stage(stage, 'CTest selection must equal the registry minus labelled exclusions')
        if self.cfg.exclude_label:
            minimum = max(EXCLUDED_REGISTERED_MIN.get(self.cfg.profile.name, 0),
                          self.cfg.min_tests or 0)
            if len(registry) < minimum:
                self.fail_stage(stage, f'CTest registered {len(registry)} rows, below the '
                                f'PR registration floor of {minimum}')
            if not 0 < len(selection) < len(registry):
                self.fail_stage(stage, f'CTest registered {len(registry)} rows and selected '
                                f'{len(selection)}; expected a nonempty '
                                f'{self.cfg.exclude_label} exclusion')
        if self.cfg.min_tests is not None:
            capacity = len(registry) - len(set(registry) & pinned)
            if capacity < self.cfg.min_tests:
                self.fail_stage(stage, f'CTest registry can run {capacity} rows, below '
                                f'the floor of {self.cfg.min_tests} for the '
                                f'{self.cfg.profile.name} profile')
        if absent:
            did_not_skip = (set(selected) & pinned) - set(rows.skipped)
            if did_not_skip:
                self.fail_stage(stage, 'pinned rows that ran: ' + ', '.join(sorted(did_not_skip))
                                + '; every selected pinned private-fixture row must skip '
                                'without the overlay')
        allowed = (set(permitted_exclusion_skips(pinned, sys.platform))
                   if self.cfg.exclude_label else set())
        if not overlay:
            allowed.update(pinned)
            if self.cfg.min_tests is not None and not self.cfg.exclude_label:
                allowed.add(WEBSOCKET_SKIP_ROW)
        check_skips = bool(self.cfg.exclude_label) or self.cfg.min_tests is not None
        unpermitted = set(rows.skipped) - allowed if check_skips else set()
        if (rows.not_run or rows.disabled or unpermitted
                or rows.ran + len(rows.skipped) != len(selected)):
            self.fail_stage(stage, f'CTest shard selected {len(selected)}, ran {rows.ran} '
                            f'with {len(rows.skipped)} skips; ran + permitted skips must '
                            f'equal the shard selection{rows.not_counted()}'
                            + ('; unpermitted skips: ' + ', '.join(sorted(unpermitted))
                               if unpermitted else ''))
        if not any(failure['stage'] == stage for failure in self.failures):
            self.pass_stage(stage, f'global registry {len(registry)}, unsharded selection '
                            f'{len(selection)}, shard selection {len(selected)} = '
                            f'{rows.ran} ran + {len(rows.skipped)} permitted skips; '
                            f'global floor {self.cfg.min_tests}')

    def write_summary(self) -> None:
        self.summary['abi'] = {'action': self.abi_action}
        self.summary['abiPrior'] = {'action': self.abi_prior_action}
        self.summary['abiV13'] = {'action': self.abi_v13_action}
        self.summary['abiV14'] = {'action': self.abi_v14_action}
        self.summary['abiV15Frozen'] = {'action': self.abi_v15_frozen_action}
        self.summary['abiV16Frozen'] = {'action': self.abi_v16_frozen_action}
        self.summary['abiV18Frozen'] = {'action': self.abi_v18_frozen_action}
        self.summary['actualVersion'] = self.actual_version
        self.summary['stages'] = self.stages
        self.summary['failures'] = self.failures
        self.summary_path.parent.mkdir(parents=True, exist_ok=True)
        self.summary_path.write_text(json.dumps(self.summary, indent=2, sort_keys=True) + '\n')

    def invoke(self, name: str, argv: list[str], *, extra_env: dict[str, str] | None = None,
               combine_stderr: bool = True,
               stream_output: bool | None = None, accept_skip: bool = False) -> Completed:
        if self.cfg.profile.thread_sanitizers and name in {'ctest', 'native-help', 'require-websocket'}:
            argv = ['setarch', os.uname().machine, '-R', *argv]
        if self.cfg.ccache_path:
            extra_env = {**(extra_env or {}), 'CCACHE_COMPILERCHECK': 'content'}
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        started = time.time()
        header = '+ ' + shlex.join(map(str, argv)) + '\n'
        stage = {
            'name': name, 'argv': list(map(str, argv)), 'exitCode': None,
            'status': 'running', 'log': str(log.relative_to(self.cfg.build_dir)),
            'extraEnvKeys': sorted(extra_env) if extra_env else [],
        }
        self.stages.append(stage)
        log.write_text(header)
        self.write_summary()
        stream = self.cfg.stream_output if stream_output is None else stream_output
        if stream:
            print(f'ci_verify: {name}: {shlex.join(map(str, argv))}', flush=True)
        try:
            result = call_runner(
                self.cfg.runner, list(map(str, argv)), extra_env=extra_env,
                combine_stderr=combine_stderr, stream_output=stream)
        except Exception as error:
            result = Completed(1, b'', str(error).encode())
        body = result.stdout + (b'' if not result.stderr else b'\n' + result.stderr)
        log.write_bytes(header.encode() + body)
        # A source guard that reads a private fixture input may exit 77 with
        # the pinned literal naming the missing path (the absent-overlay
        # contract).  The literal is required: a bare 77 without it stays a
        # failure, so the skip can never become a silent pass.
        skipped = (accept_skip and result.returncode == 77
                   and PRIVATE_FIXTURE_SKIP_LITERAL.encode() in body)
        if skipped and self.cfg.ctest_shard is not None and self.cfg.ctest_shard.count > 1:
            skipped = (name in CTEST_ABSENT_SKIP_STAGES and re.search(
                r'^SKIP private fixtures not present: [^\r\n]+$',
                body.decode('utf-8', 'replace'), re.MULTILINE) is not None)
        stage.update({
            'exitCode': result.returncode,
            'status': 'skipped' if skipped else ('passed' if result.returncode == 0
                                                 else 'failed'),
            'durationSeconds': round(time.time() - started, 3),
        })
        if skipped:
            reason = next((line for line in body.decode('utf-8', 'replace').splitlines()
                           if PRIVATE_FIXTURE_SKIP_LITERAL in line), '')
            print(f'ci_verify: {name}: skipped (exit 77): {reason}', flush=True)
        if result.returncode != 0 and not skipped:
            self.failures.append({
                'stage': name,
                'exitCode': result.returncode,
                'log': stage['log'],
            })
        self.write_summary()
        return result

    def fail_stage(self, name: str, message: str, *, argv: list[str] | None = None) -> None:
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(message + '\n')
        self.stages.append({
            'name': name,
            'argv': argv or [],
            'exitCode': 1,
            'status': 'failed',
            'log': str(log.relative_to(self.cfg.build_dir)),
            'error': message,
            'extraEnvKeys': [],
        })
        self.failures.append({
            'stage': name, 'exitCode': 1, 'error': message,
            'log': str(log.relative_to(self.cfg.build_dir)),
        })
        self.write_summary()

    def pass_stage(self, name: str, message: str, *, argv: list[str] | None = None) -> None:
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(message + '\n')
        self.stages.append({
            'name': name,
            'argv': argv or [],
            'exitCode': 0,
            'status': 'passed',
            'log': str(log.relative_to(self.cfg.build_dir)),
            'extraEnvKeys': [],
        })
        self.write_summary()

    def sanitizer_env(self) -> dict[str, str] | None:
        if self.cfg.profile.thread_sanitizers:
            return {'TSAN_OPTIONS': 'halt_on_error=1:second_deadlock_stack=1'}
        return dict(SANITIZER_RUN_ENV) if self.cfg.profile.sanitizers else None

    def collect_tool_versions(self) -> bool:
        chunks = []
        failed = False
        for argv in tool_version_commands(self.cfg):
            result = call_runner(
                self.cfg.runner, argv, stream_output=False)
            chunks.append('+ ' + shlex.join(argv))
            chunks.append((result.stdout + result.stderr).decode('utf-8', 'replace'))
            if result.returncode != 0 and Path(argv[0]).name in {'cmake', 'ctest', 'git'}:
                failed = True
        self.logs.mkdir(parents=True, exist_ok=True)
        (self.logs / 'tool-versions.log').write_text('\n'.join(chunks) + '\n')
        self.stages.append({
            'name': 'tool-versions',
            'argv': ['tool-versions'],
            'exitCode': 1 if failed else 0,
            'status': 'failed' if failed else 'passed',
            'log': 'ci-logs/tool-versions.log',
            'extraEnvKeys': [],
        })
        if failed:
            self.failures.append({'stage': 'tool-versions', 'exitCode': 1})
        self.write_summary()
        return not failed

    def ensure_corpus_submodule(self) -> bool:
        """Materialize the exact public corpus gitlink before sweep-adjacent CI."""
        update = [
            'git', '-C', str(self.cfg.source), 'submodule', 'update', '--init',
            '--depth', '1', '--', 'corpus',
        ]
        if self.invoke('corpus-submodule-init', update).returncode != 0:
            return False
        status = self.invoke(
            'corpus-submodule-status',
            ['git', '-C', str(self.cfg.source), 'submodule', 'status', '--', 'corpus'])
        if status.returncode != 0:
            return False
        value = status.stdout.decode('utf-8', 'replace').strip()
        if not value or value[0] in '-+':
            self.fail_stage(
                'corpus-submodule-pin',
                'corpus submodule is absent or not at the recorded gitlink: ' + repr(value),
                argv=['git', 'submodule', 'status', '--', 'corpus'])
            return False
        self.pass_stage('corpus-submodule-pin', value,
                        argv=['git', 'submodule', 'status', '--', 'corpus'])
        if self.cfg.profile.live_runner and self.cfg.profile.source_layer:
            feed = self.invoke('corpus-live-feed', [
                'git', '-C', str(self.cfg.source / 'corpus'), 'lfs', 'pull',
                '--include=data/ohlcv_ETH-USDT-USDT_1m.csv', '--exclude='])
            if feed.returncode != 0:
                return False
        return True

    def verify_configured_profile(self, cache: dict[str, str]) -> str | None:
        profile = self.cfg.profile
        if cache.get('CMAKE_BUILD_TYPE') != profile.build_type:
            return (f'CMAKE_BUILD_TYPE expected {profile.build_type!r} '
                    f'got {cache.get("CMAKE_BUILD_TYPE")!r}')
        if cache.get('PINEFORGE_VERSION_SOURCE') != 'FILE':
            return (f'PINEFORGE_VERSION_SOURCE expected FILE '
                    f'got {cache.get("PINEFORGE_VERSION_SOURCE")!r}')
        if cache.get('Python3_EXECUTABLE') != sys.executable:
            return (f'Python3_EXECUTABLE expected {sys.executable!r} '
                    f'got {cache.get("Python3_EXECUTABLE")!r}')
        for key, wanted in (
            ('PINEFORGE_BUILD_TESTS', True),
            ('PINEFORGE_BUILD_TUTORIAL', profile.tutorial),
            ('PINEFORGE_BUILD_LIVE_RUNNER', profile.live_runner),
            ('PINEFORGE_BUILD_SOURCE_LAYER', profile.source_layer),
            ('PINEFORGE_ENABLE_SANITIZERS', profile.sanitizers),
            ('PINEFORGE_LIVE_TSAN', profile.thread_sanitizers),
            ('PINEFORGE_BUILD_EXAMPLES', profile.name in EXAMPLES_PROFILES),
            ('PINEFORGE_REQUIRE_ABI_RECEIPTS', True),
        ):
            if cmake_on(cache.get(key)) != wanted:
                return f'{key} expected {"ON" if wanted else "OFF"} got {cache.get(key)!r}'
        if self.cfg.ccache_path:
            for launcher in ('CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER'):
                if cache.get(launcher) != self.cfg.ccache_path:
                    return f'{launcher} expected {self.cfg.ccache_path!r} got {cache.get(launcher)!r}'
        return None

    def ensure_abi_base(self) -> None:
        self.abi_action = self.ensure_prepared_provider(
            self.cfg.build_dir / 'settlement-abi-base', BASE_COMMIT, BASE_TREE,
            extra_argv=[], stage='abi-base', fetch_stage='abi-fetch')

    def ensure_abi_prior(self) -> None:
        provider = PROVIDERS['0e']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_prior_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-prior', fetch_stage='abi-prior-fetch')

    def ensure_abi_v13(self) -> None:
        provider = PROVIDERS['v13']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v13_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v13', fetch_stage='abi-v13-fetch')

    def ensure_abi_v14(self) -> None:
        provider = PROVIDERS['v14']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v14_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v14', fetch_stage='abi-v14-fetch')

    def ensure_abi_v15_frozen(self) -> None:
        provider = PROVIDERS['v15-frozen']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v15_frozen_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v15-frozen', fetch_stage='abi-v15-frozen-fetch')

    def ensure_abi_v16_frozen(self) -> None:
        provider = PROVIDERS['v16-frozen']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v16_frozen_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v16-frozen', fetch_stage='abi-v16-frozen-fetch')

    def ensure_abi_v18_frozen(self) -> None:
        provider = PROVIDERS['v18-frozen']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v18_frozen_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v18-frozen', fetch_stage='abi-v18-frozen-fetch')

    def ensure_prepared_provider(self, output: Path, commit: str, tree: str, *,
                                 extra_argv: list[str], stage: str, fetch_stage: str) -> str:
        prepare = [
            sys.executable, str(self.cfg.source / 'scripts/prepare_settlement_cpp_abi_base.py'),
            '--source-repo', str(self.cfg.source),
            '--current-build', str(self.cfg.build_dir),
            '--output', str(output),
            '--jobs', str(self.cfg.jobs),
            *extra_argv,
        ]
        if output.exists():
            try:
                receipt = reusable_prepared_base(output, self.cfg.build_dir, commit=commit, tree=tree)
            except Exception as error:
                self.fail_stage(stage, str(error), argv=['reuse-matching-receipt', str(output)])
                return 'refused'
            self.pass_stage(
                stage,
                json.dumps({'action': 'reused', 'receipt': str(output / 'receipt.json'),
                            'archiveSha256': receipt.get('archiveSha256')}, indent=2, sort_keys=True),
                argv=['reuse-matching-receipt', str(output / 'receipt.json')])
            return 'reused'
        if not pinned_object_present(self.cfg.source, self.cfg.runner, commit):
            fetch = ['git', '-C', str(self.cfg.source), 'fetch', '--no-tags', '--depth=1',
                     'origin', commit]
            fetched = self.invoke(fetch_stage, fetch)
            if fetched.returncode != 0:
                return 'failed'
        # The preparer answers a missing private-fixture manifest with the
        # pinned SKIP literal and exit 77 -- the absent-overlay contract. That
        # is a skip only while the overlay is ABSENT: with
        # PINEFORGE_PRIVATE_FIXTURES set the manifests are in the tree, so a 77
        # means the overlay did not reach it and is a failure (R2-F3). A bare
        # 77 without the literal is a failure in both modes: invoke's
        # accept_skip requires the literal.
        prepared = self.invoke(stage, prepare,
                               accept_skip=not private_fixtures_overlay())
        if prepared.returncode == 0:
            return 'prepared'
        return 'skipped' if self.stages[-1]['status'] == 'skipped' else 'failed'

    def run_smoke(self, cache: dict[str, str]) -> None:
        prefix = smoke_prefix(cache, self.install_prefix)
        configure = [
            'cmake', '-S', str(self.cfg.source / 'cmake/smoke_consumer'),
            '-B', str(self.smoke_dir),
            f'-DCMAKE_BUILD_TYPE={self.cfg.profile.build_type}',
            f'-DCMAKE_PREFIX_PATH={prefix}',
        ]
        if cache.get('Eigen3_DIR'):
            configure.append(f'-DEigen3_DIR={cache["Eigen3_DIR"]}')
        if self.invoke('smoke-configure', configure).returncode != 0:
            return
        smoke_build = [
            'cmake', '--build', str(self.smoke_dir), '--parallel', str(self.cfg.jobs)]
        if self.invoke('smoke-build', smoke_build).returncode != 0:
            return
        binary = self.smoke_dir / 'smoke_version'
        ran = self.invoke(
            'smoke-version', [str(binary)], extra_env=self.sanitizer_env(),
            combine_stderr=False)
        actual = ran.stdout.decode('utf-8', 'replace').strip()
        self.actual_version = actual
        self.summary['actualVersion'] = actual
        if ran.returncode != 0:
            self.failures[-1]['error'] = (
                f'version smoke exited {ran.returncode}; expected {self.expected_version!r} '
                f'got {actual!r}')
            self.write_summary()
            return
        if actual != self.expected_version:
            self.fail_stage(
                'smoke-version-check',
                f'version smoke expected {self.expected_version!r} got {actual!r}',
                argv=[str(binary)])
            return
        self.pass_stage(
            'smoke-version-check',
            f'printed VERSION {actual} matches {self.cfg.source / "VERSION"}',
            argv=[str(binary)])

    def enforce_native_include_independence(self) -> bool:
        if self.cfg.profile.name not in NATIVE_INCLUDE_INDEPENDENCE_PROFILES:
            return True
        if not self.shared_stage('native-include-independence'):
            return True
        with tempfile.TemporaryDirectory(prefix='pineforge-native-include-') as temporary:
            result = self.invoke(
                'native-include-independence',
                native_include_independence_command(self.cfg, Path(temporary)))
        return result.returncode == 0

    def enforce_kernel_residuals(self) -> bool:
        if self.cfg.profile.name not in KERNEL_RESIDUALS_PROFILES:
            return True
        if not self.shared_stage('kernel-residuals'):
            return True
        result = self.invoke('kernel-residuals', kernel_residuals_command(self.cfg))
        return result.returncode == 0

    def run(self) -> int:
        self.logs.mkdir(parents=True, exist_ok=True)
        self.write_summary()
        if not self.collect_tool_versions():
            return self.finish('failed', 1)
        if not self.ensure_corpus_submodule():
            return self.finish('failed', 1)
        guard_failed = False
        # A guard's exit 77 with the pinned literal is the absent-overlay
        # contract, accepted only while the overlay is absent: with
        # PINEFORGE_PRIVATE_FIXTURES set the guard's private input is in the
        # tree, so its skip means the overlay did not reach it and is a
        # failure (R2-F3), not a smaller green run.
        for name, argv in source_guard_commands(self.cfg.source):
            if not self.shared_stage(name):
                continue
            result = self.invoke(name, argv,
                                 accept_skip=not private_fixtures_overlay())
            if result.returncode != 0 and self.stages[-1]['status'] != 'skipped':
                guard_failed = True
        if (self.cfg.profile.name in TWIN_PARITY_PROFILES
                and self.shared_stage('source-guard-twin-parity')):
            if self.invoke('source-guard-twin-parity',
                           twin_parity_command(self.cfg.source)).returncode != 0:
                guard_failed = True
        if guard_failed:
            return self.finish('failed', 1)
        if self.invoke('configure', cmake_configure_argv(self.cfg)).returncode != 0:
            return self.finish('failed', 1)
        cache_path = self.cfg.build_dir / 'CMakeCache.txt'
        if not cache_path.is_file():
            self.fail_stage('configure-cache', f'missing {cache_path}')
            return self.finish('failed', 1)
        cache = read_cache(cache_path)
        profile_error = self.verify_configured_profile(cache)
        if profile_error:
            self.fail_stage('profile-options', profile_error)
            return self.finish('failed', 1)
        self.pass_stage('profile-options', json.dumps({
            'tutorial': cmake_on(cache.get('PINEFORGE_BUILD_TUTORIAL')),
            'liveRunner': cmake_on(cache.get('PINEFORGE_BUILD_LIVE_RUNNER')),
            'sourceLayer': cmake_on(cache.get('PINEFORGE_BUILD_SOURCE_LAYER')),
            'sanitizers': cmake_on(cache.get('PINEFORGE_ENABLE_SANITIZERS')),
            'examples': cmake_on(cache.get('PINEFORGE_BUILD_EXAMPLES')),
            'versionSource': cache.get('PINEFORGE_VERSION_SOURCE'),
            'python': cache.get('Python3_EXECUTABLE'),
            'buildType': cache.get('CMAKE_BUILD_TYPE'),
        }, indent=2, sort_keys=True))
        if self.cfg.profile.sanitizers:
            try:
                if not sanitizer_flag_on_library(self.cfg.build_dir):
                    self.fail_stage(
                        'sanitizer-public-flag',
                        f'src/matrix.cpp compile command missing PUBLIC {SANITIZER_FLAG}')
                    return self.finish('failed', 1)
            except Exception as error:
                self.fail_stage('sanitizer-public-flag', str(error))
                return self.finish('failed', 1)
            self.pass_stage('sanitizer-public-flag', f'library compile uses {SANITIZER_FLAG}')
            if self.cfg.profile.live_runner:
                try:
                    coverage = runner_sanitizer_coverage(self.cfg.build_dir, self.cfg.source)
                except Exception as error:
                    self.fail_stage('live-sanitizer-coverage', str(error))
                    return self.finish('failed', 1)
                self.pass_stage('live-sanitizer-coverage',
                                f'all {len(coverage)} runner compiles use ASan/UBSan and '
                                'frame pointers:\n' + '\n'.join(coverage))
        if self.cfg.profile.thread_sanitizers:
            try:
                coverage = runner_sanitizer_coverage(self.cfg.build_dir, self.cfg.source, '-fsanitize=thread')
            except Exception as error:
                self.fail_stage('runner-thread-sanitizer-coverage', str(error))
                return self.finish('failed', 1)
            self.pass_stage('runner-thread-sanitizer-coverage',
                            f'{len(coverage)} runner compile units use ThreadSanitizer and frame pointers:\n' + '\n'.join(coverage))
        # The examples (release, kernel) and the live runner's two modules
        # built from example sources (kernel, native).
        if self.cfg.profile.name in EXAMPLES_PROFILES or self.cfg.profile.live_runner:
            try:
                targets, with_ndebug = example_targets_with_ndebug(
                    self.cfg.build_dir, self.cfg.source)
            except Exception as error:
                self.fail_stage('examples-assert-live', str(error))
                return self.finish('failed', 1)
            if with_ndebug:
                self.fail_stage(
                    'examples-assert-live',
                    'NDEBUG stays defined in the compile of ' + ', '.join(with_ndebug)
                    + ', so an assert() there is a no-op; examples/native/CMakeLists.txt '
                    '(the example_* targets) and runner/CMakeLists.txt (the modules built '
                    'from example sources) give each -UNDEBUG after the build-type flags')
                return self.finish('failed', 1)
            self.pass_stage('examples-assert-live',
                            f'assert() is live in all {len(targets)} targets built from '
                            'examples/native sources: ' + ', '.join(targets))
        build_argv = ['cmake', '--build', str(self.cfg.build_dir), '--parallel', str(self.cfg.jobs)]
        if self.invoke('build', build_argv).returncode != 0:
            return self.finish('failed', 1)
        archives = [self.cfg.build_dir / 'lib' / f'lib{target}.a' for target in ARCHIVE_TARGETS]
        for archive in archives:
            try:
                stale = stale_archive_sources(self.cfg.source, archive)
            except Exception as error:
                self.fail_stage('stale-binaries', str(error))
                return self.finish('failed', 1)
            if stale:
                self.fail_stage(
                    'stale-binaries',
                    f'{archive.name} predates source it is compiled from; full rebuild '
                    'required: ' + ', '.join(stale[:40]))
                return self.finish('failed', 1)
        self.pass_stage('stale-binaries',
                        ' and '.join(archive.name for archive in archives)
                        + ' are newer than every file of the source and build trees their '
                        'compiles read')
        if not self.enforce_native_include_independence():
            return self.finish('failed', 1)
        if not self.enforce_kernel_residuals():
            return self.finish('failed', 1)
        live = self.cfg.build_dir / 'bin' / 'pineforge-live'
        if self.cfg.profile.live_runner:
            if not live.is_file():
                self.fail_stage('native-binary', f'native profile missing {live}')
                return self.finish('failed', 1)
            self.pass_stage('native-binary', str(live))
        elif live.is_file():
            self.fail_stage(
                'native-binary',
                'non-native profile produced pineforge-live; live runner must stay OFF')
            return self.finish('failed', 1)
        else:
            self.pass_stage('native-binary', 'live runner absent as required for this profile')

        if self.cfg.profile.name in LIVE_ONLY_PROFILES:
            self.pass_stage('abi-providers-skipped',
                            'runner-only CTest inventory has no receipt-backed ABI rows')
        elif self.cfg.profile.source_layer:
            self.ensure_abi_base()
            self.ensure_abi_prior()
            self.ensure_abi_v13()
            self.ensure_abi_v14()
            self.ensure_abi_v15_frozen()
            self.ensure_abi_v16_frozen()
            self.ensure_abi_v18_frozen()
        else:
            # Every receipt-backed row pairs through a source::PineStrategyHost
            # TU, so the kernel-only build registers none of them.
            self.pass_stage('abi-providers-skipped',
                            'kernel-only build registers no receipt-backed ABI row')

        # AppleClang's ASan runtime serializes shadow-memory initialization
        # behind a process-global spin lock. Starting several instrumented
        # binaries at once can wedge them before main(). Keep an AppleClang
        # Darwin sanitizer lane serial; a caller that explicitly selects a
        # GNU g++ runtime can retain normal parallelism, as can Linux CI.
        cxx_name = Path(os.environ.get('CXX', '')).name
        apple_asan = (self.cfg.profile.sanitizers and sys.platform == 'darwin'
                      and not cxx_name.startswith('g++'))
        ctest_jobs = 1 if apple_asan else self.cfg.jobs
        test_dir = self.cfg.build_dir
        if self.cfg.profile.name in LIVE_ONLY_PROFILES:
            test_dir /= 'runner'
            inventory = self.invoke('live-test-inventory',
                                    ['ctest', '--test-dir', str(test_dir), '--show-only=json-v1'],
                                    stream_output=False)
            try:
                names = {test['name'] for test in json.loads(inventory.stdout)['tests']}
                required = {'native_live_help', 'test_live_json',
                            'test_native_live_startup', 'test_native_live_store',
                            'test_native_live_routing',
                            'test_native_example_batch', 'test_native_example_selected',
                            'test_native_live_equivalence_split'}
                if (self.cfg.source / 'runner' / 'transport.cpp').is_file():
                    required.add('test_native_live_websocket')
                required.update(runner_e2e_test_names(self.cfg.source))
                if inventory.returncode != 0 or required - names:
                    raise RuntimeError('missing runner CTest rows: ' +
                                       ', '.join(sorted(required - names)))
            except Exception as error:
                self.fail_stage('live-test-inventory-required', str(error))
                return self.finish('failed', 1)
            self.pass_stage('live-test-inventory-required',
                            'all required runner rows registered: ' + ', '.join(sorted(names)))
        listing = ['ctest', '--test-dir', str(test_dir), '-N']
        all_rows = self.invoke('ctest-list-all', listing,
                               stream_output=False)
        registered = (ctest_list_count(all_rows.stdout + all_rows.stderr)
                      if all_rows.returncode == 0 else None)
        selected = registered
        selected_rows = all_rows
        if self.cfg.exclude_label:
            selected_rows = self.invoke('ctest-list-selected',
                                        listing + ['-LE', self.cfg.exclude_label],
                                        stream_output=False)
            selected = (ctest_list_count(selected_rows.stdout + selected_rows.stderr)
                        if selected_rows.returncode == 0 else None)
        self.summary['ctestRegistered'] = registered
        self.summary['ctestSelected'] = selected
        if registered is None or selected is None:
            self.fail_stage('ctest-discovery', 'could not read registered/selected CTest counts')
        self.write_summary()
        if self.cfg.ctest_shard is not None:
            ran = self.run_sharded_ctest(test_dir, ctest_jobs, all_rows, selected_rows)
            if ran is None:
                return self.finish('failed', 1)
        else:
            excluded_names = None
            if self.cfg.exclude_label == 'slow':
                labelled = self.invoke('ctest-list-excluded', listing + ['-L', 'slow'],
                                       stream_output=False)
                try:
                    if labelled.returncode:
                        raise ValueError('CTest slow-label discovery command failed')
                    excluded_names = frozenset(ctest_registry(labelled.stdout + labelled.stderr))
                except ValueError as error:
                    self.fail_stage('ctest-exclusion', str(error))
                    return self.finish('failed', 1)
            ctest = ctest_command(self.cfg, test_dir, jobs=ctest_jobs,
                                  junit=ctest_supports_junit(self.cfg.runner))
            ran = self.invoke('ctest', ctest, extra_env=self.sanitizer_env())
            registered_names = (ctest_list_names(all_rows.stdout + all_rows.stderr)
                                if all_rows.returncode == 0 else None)
            self.enforce_test_floor(ran, registered=registered, selected=selected,
                                    registered_names=registered_names, excluded_names=excluded_names)
        if self.cfg.profile.name in LIVE_ONLY_PROFILES:
            try:
                rows = ctest_rows(ran.stdout + ran.stderr)
                if rows is None or rows.not_run or rows.disabled:
                    raise RuntimeError(f'{self.cfg.profile.name} cannot accept skipped, disabled or unreadable rows')
                required_skips = sorted(set(rows.skipped) & runner_e2e_test_names(self.cfg.source))
                if required_skips:
                    raise RuntimeError('required runner e2e rows skipped: ' + ', '.join(required_skips))
                if rows.skipped and not private_fixtures_overlay():
                    # The required native-live job's shape (X2): without the
                    # overlay runner/ registers its pinned private-fixture
                    # placeholders and they SKIP, which is not a lost row --
                    # the same rule the PR-exclusion branch states. Every
                    # skipped row must be a pinned private-fixture row; an
                    # unpinned skip, a not-run row and a disabled row are all
                    # refused, and the pin file fails closed when unreadable or
                    # truncated (pinned_private_fixture_rows).
                    pinned = self.pinned_private_fixture_rows('live-test-skips')
                    if pinned is not None:
                        unpinned = [name for name in rows.skipped if name not in pinned]
                        if unpinned:
                            raise RuntimeError(
                                f'{self.cfg.profile.name} cannot accept skipped rows that '
                                f'are not pinned private-fixture placeholders (the overlay '
                                f'is absent): ' + ', '.join(unpinned))
                        self.pass_stage(
                            'live-test-skips',
                            f'{len(rows.skipped)} pinned private-fixture rows skipping in '
                            f'the fixtures-absent world; no unpinned skip, no not-run and '
                            f'no disabled row')
                elif rows.skipped:
                    # Overlay present: every runner row must RUN; a skipped row
                    # is a lost row, pinned or not.
                    raise RuntimeError(f'{self.cfg.profile.name} cannot accept skipped, disabled or unreadable rows')
            except Exception as error:
                self.fail_stage('live-test-skips', str(error))

        installed = None
        if self.shared_stage('install'):
            installed = self.invoke(
                'install',
                ['cmake', '--install', str(self.cfg.build_dir), '--prefix', str(self.install_prefix)])
        else:
            for name in ('smoke-configure', 'smoke-build', 'smoke-version', 'smoke-version-check'):
                self.shared_stage(name)
            if self.cfg.profile.live_runner:
                self.shared_stage('native-help')
        if installed is not None and installed.returncode == 0:
            self.run_smoke(cache)
            if self.cfg.profile.live_runner:
                help_bin = self.install_prefix / 'bin' / 'pineforge-live'
                if not help_bin.is_file():
                    self.fail_stage('native-help', f'installed native executable missing: {help_bin}')
                else:
                    self.invoke('native-help', [str(help_bin), '--help'],
                                extra_env=self.sanitizer_env())
        if self.cfg.require_websocket and self.shared_stage('require-websocket'):
            ws = self.cfg.build_dir / 'bin' / 'test_native_live_websocket'
            if not ws.is_file():
                self.fail_stage(
                    'require-websocket',
                    f'--require-websocket cannot accept skip: missing {ws}')
            else:
                ran = self.invoke(
                    'require-websocket', [str(ws)], extra_env=self.sanitizer_env())
                if ran.returncode == 77:
                    self.fail_stage(
                        'require-websocket-skip',
                        '--require-websocket cannot accept skip (exit 77)',
                        argv=[str(ws)])
        status = 'passed' if not self.failures else 'failed'
        return self.finish(status, 0 if status == 'passed' else 1)

    def enforce_test_floor(self, ran: Completed, *, registered: int | None = None,
                           selected: int | None = None,
                           registered_names: 'frozenset[str] | None' = None,
                           excluded_names: 'frozenset[str] | None' = None) -> None:
        """Refuse a CTest run in which fewer rows ran than the profile's floor.

        The count is the rows whose test ran to a verdict, pass or fail. A row
        CTest skipped (SKIP_RETURN_CODE, SKIP_REGULAR_EXPRESSION) or could not
        start proves nothing, so it is listed beside the count and never
        counted, although CTest's own closing summary includes it. A run whose
        output the floor cannot read (no summary: no tests found, a crash
        before it; row lists that disagree with it) fails closed rather than
        passing an empty or truncated suite through the floor.
        """
        output = ran.stdout + ran.stderr
        self.summary['ctestTotal'] = ctest_row_count(output)
        try:
            rows, unreadable = ctest_rows(output), None
        except ValueError as error:
            rows, unreadable = None, str(error)
        self.summary['ctestRows'] = rows.ran if rows else None
        self.summary['ctestSkipped'] = list(rows.skipped) if rows else None
        self.summary['ctestNotRun'] = list(rows.not_run) if rows else None
        self.summary['ctestDisabled'] = list(rows.disabled) if rows else None
        # With the overlay present every private-fixtures row must run; this
        # check is profile-independent (a floorless profile refuses the skip
        # too) and runs before the exclude-label branch returns.
        if private_fixtures_overlay():
            self.reject_private_fixture_skips(rows)
        if self.cfg.exclude_label:
            minimum = max(EXCLUDED_REGISTERED_MIN.get(self.cfg.profile.name, 0),
                          self.cfg.min_tests or 0)
            unpinned_slow = (sorted(excluded_names - PINNED_SLOW_TEST_ROWS)
                             if self.cfg.exclude_label == 'slow' and excluded_names is not None
                             else [])
            if self.cfg.exclude_label == 'slow' and excluded_names is None:
                self.fail_stage('ctest-exclusion',
                                'CTest slow-label names were unreadable; cannot prove the pin bound')
            elif unpinned_slow:
                self.fail_stage('ctest-exclusion',
                                'CTest excluded unpinned slow rows: ' + ', '.join(unpinned_slow))
            elif unreadable or rows is None or registered is None or selected is None:
                self.fail_stage('ctest-exclusion',
                                'CTest discovery or run count was unreadable; cannot prove '
                                'registered - labelled = ran + permitted skips'
                                + (f' ({unreadable})' if unreadable else ''))
            elif registered < minimum:
                self.fail_stage('ctest-exclusion',
                                f'CTest registered {registered} rows, below the PR '
                                f'registration floor of {minimum}')
            elif not 0 < selected < registered:
                self.fail_stage('ctest-exclusion',
                                f'CTest registered {registered} rows and selected {selected}; '
                                f'expected a nonempty {self.cfg.exclude_label} exclusion')
            else:
                # Pins permit skips only without the overlay. The one named
                # platform exception applies in either fixture world. Neither
                # allowance can fill in a missing, not-run or disabled result.
                pinned = frozenset()
                if not private_fixtures_overlay():
                    pinned = self.pinned_private_fixture_rows('ctest-exclusion')
                    if pinned is None:
                        return
                allowed = permitted_exclusion_skips(pinned, sys.platform)
                skipped_pinned = [name for name in rows.skipped if name in pinned]
                skipped_platform = [name for name in rows.skipped if name in allowed - pinned]
                unpinned = [name for name in rows.skipped if name not in allowed]
                platform_note = ('; platform skips: ' + ', '.join(
                    f'{name} ({PLATFORM_CONDITIONAL_SKIP_ROWS[name][1]})'
                    for name in skipped_platform)) if skipped_platform else ''
                if (rows.not_run or rows.disabled or unpinned
                        or rows.ran + len(skipped_pinned) + len(skipped_platform) != selected):
                    self.fail_stage(
                        'ctest-exclusion',
                        f'CTest registered {registered}, labelled '
                        f'{registered - selected}, selected {selected}, but ran '
                        f'{rows.ran} beside {len(skipped_pinned)} pinned private-fixture '
                        f'rows skipping and {len(skipped_platform)} permitted platform skips '
                        f'(ran + pinned skips + permitted platform skips must equal selected; '
                        f'pins permit skips only without the overlay){rows.not_counted()}'
                        + (f'; skipped but not pinned or permitted on this platform: '
                           f'{", ".join(unpinned)}' if unpinned else '')
                        + platform_note)
                else:
                    self.pass_stage('ctest-exclusion',
                                    f'ctest registered {registered}, labelled '
                                    f'{registered - selected}, ran {rows.ran} '
                                    f'with {len(skipped_pinned)} pinned private-fixture '
                                    f'rows skipping and {len(skipped_platform)} permitted '
                                    f'platform skips (ran + permitted skips = '
                                    f'registered - labelled){platform_note}')
            # Debug-type profiles retain null default execution floors. An
            # explicit --min-tests also binds actual execution, as it does
            # for the shard union, and supplies the required 1/1 stage proof.
            if self.cfg.profile.exclude_label is not None and self.cfg.min_tests is not None:
                floor = self.cfg.min_tests
                if rows is None:
                    self.fail_stage('ctest-floor',
                                    f'CTest row count is unreadable; cannot prove floor {floor}')
                elif rows.ran < floor:
                    self.fail_stage('ctest-floor',
                                    f'ctest ran {rows.ran} rows, below the floor of {floor} '
                                    f'for the {self.cfg.profile.name} profile{rows.not_counted()}')
                else:
                    self.pass_stage('ctest-floor', f'ctest ran {rows.ran} rows (floor {floor})'
                                    f'{rows.not_counted()}')
            return
        floor = self.cfg.min_tests
        if floor is None:
            self.write_summary()
            return
        # The floor the run is held to: --min-tests, or the profile's
        # overlay-aware floor -- the FULL population with the overlay present,
        # the FULL population minus the pinned private-fixture placeholders it
        # registers without the overlay. Both are constants of the profile
        # (PRIVATE_FIXTURE_PROFILE_FLOORS), computed before the run and never
        # moved by it: not by its `registered` count, not by its observed
        # skips, so a public row that left the registry or stopped running
        # falls under the floor either way, and no margin absorbs it. An
        # explicit --min-tests value is used verbatim (R2-F8).
        effective = floor
        note = ''
        entry = PRIVATE_FIXTURE_PROFILE_FLOORS.get(self.cfg.profile.name)
        absent = entry is not None and not private_fixtures_overlay()
        if absent:
            full, pinned = entry
            pinned = min(pinned, PRIVATE_FIXTURE_ROW_COUNT)
            if floor == full - pinned and not self.cfg.min_tests_explicit:
                # Only the table's own lowering is reported: an explicit
                # --min-tests that happens to equal full minus pinned was not
                # lowered by anything (N5).
                note = (f'; lowered by {pinned} pinned private-fixture '
                        f'placeholder rows (full floor {full})')
            # N4: without the overlay every pinned row this run registers must
            # have SKIPPED. The floor counts every row that ran, pinned or not,
            # so a pinned row that ran without its fixtures -- or one that was
            # disabled, or never started -- would hide the loss of a public
            # row behind its own count. (A row registered only in one world is
            # the two-world registration check's finding, not the floor's.)
            if rows is not None:
                pinned_names = self.pinned_private_fixture_rows('ctest-floor')
                if pinned_names is not None:
                    if registered_names is None:
                        self.fail_stage(
                            'ctest-floor',
                            'the ctest -N listing carried no row names, so the pinned '
                            'rows that ran cannot be checked in the fixtures-absent world')
                    else:
                        did_not_skip = [name for name in sorted(registered_names & pinned_names)
                                        if name not in set(rows.skipped)]
                        if did_not_skip:
                            self.fail_stage(
                                'ctest-floor',
                                f'pinned rows that ran: {len(did_not_skip)} '
                                f'({", ".join(did_not_skip)}); every registered pinned '
                                f'private-fixture row must skip without the overlay')
        # N6: test_native_live_websocket's 77 is not a lost row in the ABSENT
        # world, where main's kernel floor counted that skip as a run (its
        # number sat one below the registered population for this row). The
        # one named row keeps that allowance there, without being pinned;
        # with the overlay present every row must run, as before, nothing
        # else changes.
        tolerated = ([WEBSOCKET_SKIP_ROW]
                     if rows is not None and WEBSOCKET_SKIP_ROW in rows.skipped
                     and not private_fixtures_overlay() else [])
        counted = rows.ran + len(tolerated) if rows is not None else 0
        clause = (f' ({WEBSOCKET_SKIP_ROW} skipped: exit 77 without WebSocket '
                  f'support, counted as run, allowed as on main)' if tolerated else '')
        if unreadable:
            self.fail_stage(
                'ctest-floor',
                f'ctest printed row lists the floor cannot read ({unreadable}); '
                f'the floor of {floor} rows cannot be verified')
        elif rows is None:
            self.fail_stage(
                'ctest-floor',
                f'ctest printed no row count; the floor of {floor} rows cannot be verified')
        elif counted < effective:
            self.fail_stage(
                'ctest-floor',
                f'ctest ran {rows.ran} rows, below the floor of {effective} for the '
                f'{self.cfg.profile.name} profile{rows.not_counted()}{note}{clause}; a registered '
                'row left the suite or stopped running, or lower the floor with --min-tests '
                'on purpose')
        else:
            self.pass_stage('ctest-floor',
                            f'ctest ran {rows.ran} rows (floor {effective})'
                            f'{rows.not_counted()}{note}{clause}')

    def pinned_private_fixture_rows(self, stage: str) -> 'frozenset[str] | None':
        """The pinned row names, or None after failing `stage` closed.

        The pin file speaks for the pinned population only while it carries the
        PRIVATE_FIXTURE_ROW_COUNT names its own header records: an unreadable or
        truncated pin cannot clear the rows it lost, so both fail closed.
        """
        try:
            pinned = private_fixture_rows(self.cfg.source, expand_equivalence=False)
        except OSError as error:
            self.fail_stage(
                stage,
                f'{PRIVATE_FIXTURE_ROWS_FILE} is unreadable, so the '
                f'private_fixtures rows cannot be checked ({error})')
            return None
        if len(pinned) != PRIVATE_FIXTURE_ROW_COUNT:
            self.fail_stage(
                stage,
                f'{PRIVATE_FIXTURE_ROWS_FILE} names {len(pinned)} rows, not the '
                f'{PRIVATE_FIXTURE_ROW_COUNT} its own header records; a truncated pin '
                'cannot clear the rows it lost')
            return None
        try:
            return expand_equivalence_test_names(pinned, self.cfg.source)
        except (OSError, SyntaxError, ValueError) as error:
            self.fail_stage(stage, f'cannot expand the equivalence private-fixture pin: {error}')
            return None

    def reject_private_fixture_skips(self, rows: 'CTestRows | None') -> None:
        """With PINEFORGE_PRIVATE_FIXTURES set, refuse a placeholder that did not run.

        Every row the pinned tests/private_fixture_rows.txt names must RUN when
        the overlay is present: the profile's floor counts on exactly those
        rows, so one that skips, is disabled or cannot start leaves a
        silently-smaller run. The check fails closed on everything it cannot
        read: row lists that did not parse at all (R2-F10: a floorless profile
        would otherwise pass this rule vacuously), a pin file that is unreadable
        or does not carry the PRIVATE_FIXTURE_ROW_COUNT names the file itself
        records (a truncated pin cannot clear the rows it lost).
        """
        if rows is None:
            self.fail_stage(
                'ctest-private-fixtures',
                'PINEFORGE_PRIVATE_FIXTURES is set but CTest printed no row lists this '
                'run can read, so the ' + PRIVATE_FIXTURE_LABEL + ' rows cannot be checked')
            return
        pinned = self.pinned_private_fixture_rows('ctest-private-fixtures')
        if pinned is None:
            return
        offenders = ([name for name in rows.skipped if name in pinned]
                     + [name for name in rows.disabled if name in pinned]
                     + [name for name in rows.not_run if name in pinned])
        if offenders:
            self.fail_stage(
                'ctest-private-fixtures',
                f'PINEFORGE_PRIVATE_FIXTURES is set but these '
                f'{PRIVATE_FIXTURE_LABEL} rows did not run, a skip printed '
                + repr(PRIVATE_FIXTURE_SKIP_LITERAL)
                + ': ' + ', '.join(offenders)
                + '; the overlay did not reach the tree')
        else:
            self.pass_stage(
                'ctest-private-fixtures',
                f'PINEFORGE_PRIVATE_FIXTURES set; none of the {len(pinned)} '
                f'{PRIVATE_FIXTURE_LABEL} rows skipped')

    def finish(self, status: str, code: int) -> int:
        self.summary['status'] = status
        self.summary['exitCode'] = code
        self.write_summary()
        return code


def build_config(argv: list[str] | None = None, *, source: Path = ROOT,
                 runner: Runner | None = None, stream_output: bool | None = None,
                 which: Callable[[str], str | None] = shutil.which) -> VerifyConfig:
    args = parse_args(argv, source=source)
    cfg = validate_config(args, source=source, which=which)
    if runner is not None:
        cfg.runner = runner
    if stream_output is not None:
        cfg.stream_output = stream_output
    return cfg


def main(argv: list[str] | None = None, *, runner: Runner | None = None,
         stream_output: bool | None = None,
         which: Callable[[str], str | None] = shutil.which) -> int:
    try:
        cfg = build_config(argv, runner=runner, stream_output=stream_output, which=which)
    except ConfigError as error:
        print(f'ci_verify: {error}', file=sys.stderr)
        return 2
    driver = Driver(cfg)
    try:
        return driver.run()
    except ConfigError as error:
        print(f'ci_verify: {error}', file=sys.stderr)
        driver.fail_stage('config', str(error))
        return driver.finish('failed', 2)
    except Exception as error:
        driver.fail_stage('driver', str(error))
        return driver.finish('failed', 1)


if __name__ == '__main__':
    raise SystemExit(main())
