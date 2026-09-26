#!/usr/bin/env python3
"""Shared local/CI verification driver. Stdlib only. Not a command generator.

Profiles: release, debug, sanitizers, native, kernel. Default build dir build-ci-PROFILE.
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
PROFILES = ('release', 'debug', 'sanitizers', 'native', 'kernel')
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
KERNEL_MIN_TESTS = 286
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
RELEASE_MIN_TESTS = 703
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
ADR_RULED_IDENTIFIERS_MIN = 170
ADR_RULED_TEXTS_MIN = 40
# PR-only registration floors: the complete CTest populations of the three
# excluded profiles at INT25, counted with ctest -N on the integrated tree --
# 653/653/662 at 91d65ad6 (INT24) plus wave G's six rows (C-SURFACE-1 +1,
# KERNEL-EDGE +1, K-ULP4 +2, K-ULP5 +1, DOC-TRUTH-4 +1) in each; 683/683/692
# at INT26, each with wave H's twenty-four rows of RELEASE_MIN_TESTS
# (H-MEASURE +12, H-DOCGATES +3, K-OCA-KEEP +1, PAR-ORDERS-2 +4, PAR-MARGIN
# +1, PAR-MARGIN-2 +1, PERF-KEDGE +1, PAR-CASHFEE +1), counted the same way;
# 684/684/693 with INT26 round 2's tape row.
# An excluded run must still discover at least this many rows before -LE.
EXCLUDED_REGISTERED_MIN = {'debug': 684, 'sanitizers': 684, 'native': 693}
# The ctest stage's bound. A full sanitizers run (push to main, a manual
# dispatch, the maintainers' verification) ran out of its 30 minutes twice on
# main's four-core runner before every row had finished, so it gets an hour; a
# run that excludes a label (the PR set) and every other profile keep 30
# minutes.
CTEST_TIMEOUT = 1800
SANITIZERS_FULL_CTEST_TIMEOUT = 3600


def ctest_timeout(cfg: 'VerifyConfig') -> int:
    if cfg.profile.sanitizers and not cfg.exclude_label:
        return SANITIZERS_FULL_CTEST_TIMEOUT
    return CTEST_TIMEOUT


# CTest's closing summary: '100% tests passed out of N' when nothing failed,
# '97% tests passed, 3 tests failed out of N' otherwise. N includes a skipped
# row (counted as passed) and a row CTest could not start (counted as
# failed), but not a disabled one; the lists printed after it tell them apart.
CTEST_ROW_COUNT = re.compile(
    r'% tests passed(?:, (?P<failed>\d+) tests? failed)? out of (?P<total>\d+)')
# A row listed after the summary: '\t182 - name (Skipped)' or '(Disabled)'
# under 'The following tests did not run:'; '\t  6 - name (Not Run)',
# '(Failed)', '(Timeout)', ... under 'The following tests FAILED:'.
CTEST_LISTED_ROW = re.compile(r'^\s*\d+ - (.+) \(([^()\n]+)\)\s*$', re.MULTILINE)
# A skipped row's own result line: ' 4/10 Test  #3: name .....***Skipped   0.01 sec'.
CTEST_SKIPPED_RESULT = re.compile(r'^\s*\d+/\d+ Test\s+#\d+: .*\*\*\*Skipped\b', re.MULTILINE)
CTEST_LIST_COUNT = re.compile(r'^Total Tests:\s*(\d+)\s*$', re.MULTILINE)
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


PROFILE = {
    'release': Profile('release', 'Release', False, False, True, True, RELEASE_MIN_TESTS),
    'debug': Profile('debug', 'Debug', False, False, True, True),
    'sanitizers': Profile('sanitizers', 'Debug', True, False, True, True),
    'native': Profile('native', 'Release', False, True, False, True),
    'kernel': Profile('kernel', 'Release', False, True, False, False, KERNEL_MIN_TESTS),
}


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
                timeout: int = 600, combine_stderr: bool = True,
                stream_output: bool = False) -> Completed:
    return runner(argv, extra_env=extra_env, timeout=timeout,
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
        result = call_runner(runner, ['ctest', '--help'], timeout=30, stream_output=False)
    except (OSError, subprocess.TimeoutExpired):
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
                        help='native only: execute test_native_live_websocket and refuse skip (77)')
    parser.add_argument('--exclude-label', default=None,
                        help='exclude one CTest label; verify the run count against '
                             'CTest discovery with and without -LE')
    parser.add_argument('--min-tests', type=int, default=None,
                        help='fail the ctest-floor stage unless at least N CTest rows ran '
                             '(a skipped or not-run row is listed, never counted); '
                             f'the kernel profile defaults to {KERNEL_MIN_TESTS}, the release '
                             f'profile to {RELEASE_MIN_TESTS}, the others to no floor')
    args = parser.parse_args(argv)
    if args.build_dir is None:
        args.build_dir = default_build_dir(source, args.profile)
    return args


def validate_config(args: argparse.Namespace, *, source: Path = ROOT,
                    which: Callable[[str], str | None] = shutil.which) -> VerifyConfig:
    if not JOBS_MIN <= args.jobs <= JOBS_MAX:
        raise ConfigError(f'--jobs must be {JOBS_MIN}..{JOBS_MAX}')
    if args.require_websocket and args.profile != 'native':
        raise ConfigError('--require-websocket is only valid with the native profile')
    if args.exclude_label is not None:
        label = args.exclude_label.strip()
        if not label or any(not (char.isalnum() or char in '_.-') for char in label):
            raise ConfigError('--exclude-label must be a simple CTest label')
        args.exclude_label = label
    if args.curl_dir is not None and not args.curl_dir.is_dir():
        raise ConfigError(f'--curl-dir is not a directory: {args.curl_dir}')
    if args.min_tests is not None and args.min_tests < 1:
        raise ConfigError('--min-tests must be at least 1')
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
        require_websocket=bool(args.require_websocket),
        runner=default_runner,
        exclude_label=args.exclude_label,
        min_tests=args.min_tests if args.min_tests is not None else PROFILE[args.profile].min_tests,
    )


def default_runner(argv: list[str], *, extra_env: dict[str, str] | None = None,
                   timeout: int = 600, combine_stderr: bool = True,
                   stream_output: bool = True) -> Completed:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    stderr = subprocess.STDOUT if combine_stderr else subprocess.PIPE
    try:
        result = subprocess.run(
            argv, stdout=subprocess.PIPE, stderr=stderr, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        out = error.stdout or b''
        err = b'' if combine_stderr else (error.stderr or b'')
        if stream_output and out:
            sys.stdout.buffer.write(out)
            sys.stdout.buffer.flush()
        return Completed(124, out, err)
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
        timeout=30, stream_output=False)
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
               timeout: int = 600, combine_stderr: bool = True,
               stream_output: bool | None = None) -> Completed:
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
                self.cfg.runner, list(map(str, argv)), extra_env=extra_env, timeout=timeout,
                combine_stderr=combine_stderr, stream_output=stream)
        except Exception as error:
            result = Completed(1, b'', str(error).encode())
        body = result.stdout + (b'' if not result.stderr else b'\n' + result.stderr)
        log.write_bytes(header.encode() + body)
        stage.update({
            'exitCode': result.returncode,
            'status': 'passed' if result.returncode == 0 else 'failed',
            'durationSeconds': round(time.time() - started, 3),
        })
        if result.returncode != 0:
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
        return dict(SANITIZER_RUN_ENV) if self.cfg.profile.sanitizers else None

    def collect_tool_versions(self) -> bool:
        chunks = []
        failed = False
        for argv in tool_version_commands(self.cfg):
            result = call_runner(
                self.cfg.runner, argv, timeout=30, stream_output=False)
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
        if self.invoke('corpus-submodule-init', update, timeout=600).returncode != 0:
            return False
        status = self.invoke(
            'corpus-submodule-status',
            ['git', '-C', str(self.cfg.source), 'submodule', 'status', '--', 'corpus'],
            timeout=60)
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
            fetched = self.invoke(fetch_stage, fetch, timeout=120)
            if fetched.returncode != 0:
                return 'failed'
        prepared = self.invoke(stage, prepare, timeout=1800)
        return 'prepared' if prepared.returncode == 0 else 'failed'

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
        if self.invoke('smoke-configure', configure, timeout=120).returncode != 0:
            return
        if self.invoke(
                'smoke-build',
                ['cmake', '--build', str(self.smoke_dir), '--parallel', str(self.cfg.jobs)],
                timeout=300).returncode != 0:
            return
        binary = self.smoke_dir / 'smoke_version'
        ran = self.invoke(
            'smoke-version', [str(binary)], extra_env=self.sanitizer_env(),
            timeout=60, combine_stderr=False)
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
        with tempfile.TemporaryDirectory(prefix='pineforge-native-include-') as temporary:
            result = self.invoke(
                'native-include-independence',
                native_include_independence_command(self.cfg, Path(temporary)),
                timeout=300)
        return result.returncode == 0

    def enforce_kernel_residuals(self) -> bool:
        if self.cfg.profile.name not in KERNEL_RESIDUALS_PROFILES:
            return True
        result = self.invoke('kernel-residuals', kernel_residuals_command(self.cfg), timeout=300)
        return result.returncode == 0

    def run(self) -> int:
        self.logs.mkdir(parents=True, exist_ok=True)
        self.write_summary()
        if not self.collect_tool_versions():
            return self.finish('failed', 1)
        if not self.ensure_corpus_submodule():
            return self.finish('failed', 1)
        guard_failed = False
        for name, argv in source_guard_commands(self.cfg.source):
            if self.invoke(name, argv, timeout=120).returncode != 0:
                guard_failed = True
        if self.cfg.profile.name in TWIN_PARITY_PROFILES:
            if self.invoke('source-guard-twin-parity',
                           twin_parity_command(self.cfg.source), timeout=120).returncode != 0:
                guard_failed = True
        if guard_failed:
            return self.finish('failed', 1)
        if self.invoke('configure', cmake_configure_argv(self.cfg), timeout=180).returncode != 0:
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
        if self.invoke(
                'build',
                ['cmake', '--build', str(self.cfg.build_dir), '--parallel', str(self.cfg.jobs)],
                timeout=1800).returncode != 0:
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

        if self.cfg.profile.source_layer:
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
        registered = selected = None
        if self.cfg.exclude_label:
            listing = ['ctest', '--test-dir', str(self.cfg.build_dir), '-N']
            all_rows = self.invoke('ctest-list-all', listing, timeout=120,
                                   stream_output=False)
            selected_rows = self.invoke('ctest-list-selected',
                                        listing + ['-LE', self.cfg.exclude_label],
                                        timeout=120, stream_output=False)
            if all_rows.returncode == 0:
                registered = ctest_list_count(all_rows.stdout + all_rows.stderr)
            if selected_rows.returncode == 0:
                selected = ctest_list_count(selected_rows.stdout + selected_rows.stderr)
            self.summary['ctestRegistered'] = registered
            self.summary['ctestSelected'] = selected
            self.write_summary()
        ctest = ['ctest', '--test-dir', str(self.cfg.build_dir),
                 '--output-on-failure', '--no-tests=error', '--parallel', str(ctest_jobs)]
        if self.cfg.exclude_label:
            ctest += ['-LE', self.cfg.exclude_label]
        if ctest_supports_junit(self.cfg.runner):
            ctest += ['--output-junit', str(self.cfg.build_dir / 'ctest-junit.xml')]
        ran = self.invoke('ctest', ctest, extra_env=self.sanitizer_env(),
                          timeout=ctest_timeout(self.cfg))
        self.enforce_test_floor(ran, registered=registered, selected=selected)

        installed = self.invoke(
            'install',
            ['cmake', '--install', str(self.cfg.build_dir), '--prefix', str(self.install_prefix)],
            timeout=180)
        if installed.returncode == 0:
            self.run_smoke(cache)
            if self.cfg.profile.live_runner:
                help_bin = self.install_prefix / 'bin' / 'pineforge-live'
                if not help_bin.is_file():
                    self.fail_stage('native-help', f'installed native executable missing: {help_bin}')
                else:
                    self.invoke('native-help', [str(help_bin), '--help'], timeout=30)
        if self.cfg.require_websocket:
            ws = self.cfg.build_dir / 'bin' / 'test_native_live_websocket'
            if not ws.is_file():
                self.fail_stage(
                    'require-websocket',
                    f'--require-websocket cannot accept skip: missing {ws}')
            else:
                ran = self.invoke(
                    'require-websocket', [str(ws)], extra_env=self.sanitizer_env(), timeout=120)
                if ran.returncode == 77:
                    self.fail_stage(
                        'require-websocket-skip',
                        '--require-websocket cannot accept skip (exit 77)',
                        argv=[str(ws)])
        status = 'passed' if not self.failures else 'failed'
        return self.finish(status, 0 if status == 'passed' else 1)

    def enforce_test_floor(self, ran: Completed, *, registered: int | None = None,
                           selected: int | None = None) -> None:
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
        if self.cfg.exclude_label:
            minimum = max(EXCLUDED_REGISTERED_MIN.get(self.cfg.profile.name, 0),
                          self.cfg.min_tests or 0)
            if unreadable or rows is None or registered is None or selected is None:
                self.fail_stage('ctest-exclusion',
                                'CTest discovery or run count was unreadable; cannot prove '
                                'registered - labelled = ran'
                                + (f' ({unreadable})' if unreadable else ''))
            elif registered < minimum:
                self.fail_stage('ctest-exclusion',
                                f'CTest registered {registered} rows, below the PR '
                                f'registration floor of {minimum}')
            elif not 0 < selected < registered:
                self.fail_stage('ctest-exclusion',
                                f'CTest registered {registered} rows and selected {selected}; '
                                f'expected a nonempty {self.cfg.exclude_label} exclusion')
            elif rows.ran != selected:
                self.fail_stage('ctest-exclusion',
                                f'CTest registered {registered}, labelled '
                                f'{registered - selected}, selected {selected}, but ran '
                                f'{rows.ran}{rows.not_counted()}')
            else:
                self.pass_stage('ctest-exclusion',
                                f'ctest registered {registered}, labelled '
                                f'{registered - selected}, ran {rows.ran} '
                                f'(registered - labelled){rows.not_counted()}')
            return
        floor = self.cfg.min_tests
        if floor is None:
            self.write_summary()
            return
        if unreadable:
            self.fail_stage(
                'ctest-floor',
                f'ctest printed row lists the floor cannot read ({unreadable}); '
                f'the floor of {floor} rows cannot be verified')
        elif rows is None:
            self.fail_stage(
                'ctest-floor',
                f'ctest printed no row count; the floor of {floor} rows cannot be verified')
        elif rows.ran < floor:
            self.fail_stage(
                'ctest-floor',
                f'ctest ran {rows.ran} rows, below the floor of {floor} for the '
                f'{self.cfg.profile.name} profile{rows.not_counted()}; a registered row left '
                'the suite or stopped running, or lower the floor with --min-tests on purpose')
        else:
            self.pass_stage('ctest-floor',
                            f'ctest ran {rows.ran} rows (floor {floor}){rows.not_counted()}')

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
