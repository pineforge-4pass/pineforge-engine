# R4-D L0 legacy-route oracles

Every `test_oracle_*.cpp` in this directory is a self-contained capture from
engine `ab9714beccb62b796c122cf68986ec9e7dbf4a67`. Its header marks the
capture SHA and its expected rows/quantities/bit values are literal source
route facts. These are intentionally separate translation units: later
adapter work must make them pass, never change the captured expectations.

| Oracle | Legacy fact frozen |
|---|---|
| `deferred_any`, `deferred_any_witnesses`, `fifo_cohort`, `deferred_birth`, `relative_exit` | Deferred ANY P-DA1…P-DA9, replacement growth, re-entry cohorts, no target, FIFO distinction, birth timing. |
| `reversal`, `reversal_close_only`, `reversal_same_bar_tx`, `reversal_replaced_percent`, `reversal_later_tick` | Contract §3.3's ReverseTo, Flatten, Transact, Reduce, F7/F8 bit patterns, and reversal selector families. |
| `short_seed`, `short_seed_percent` | Contract §3.4 ShortSeed books, codes, C-mirror observations and percentages. |
| `fx` | G4–G6, 1x/4x/floor/leveraged FX/open-margin ordering. |
| `coof`, `coof_first_open`, `more_than_64_fills` | First-open versus later cascade behavior, `UINT64_MAX` fill budget and a literal 65-fill sweep. |
| `pooc_freeze`, `pooc_immediate`, `frozen_size`, `stop_snapshot` | POOC freeze versus `immediately=true`, frozen sizing and stop-placement predicates. |
| `magnifier_distribution`, `magnifier_barstate` | `bar_magnifier=1` endpoint and volume-weighted corpus lanes, terminal-sub-bar barstate/history cadence. |
| `day_key` | Script-bar/day-key and chart-timezone literal values. |

The native-only precommit-cycle-overflow witness has no legacy-route analogue:
the legacy route has no `NativePrecommitView`/candidate precommit boundary.
It is recorded as an L1 native witness in the L0 report rather than invented
as a source oracle.
