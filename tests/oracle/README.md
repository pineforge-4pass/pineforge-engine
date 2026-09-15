# R4-D L0 legacy-route oracle carriers

Every `test_oracle_*.cpp` here is the immutable literal capture from engine
`ab9714beccb62b796c122cf68986ec9e7dbf4a67`. They are source files, not
standalone CTest targets: each is included verbatim by the switched-route twin
listed below. The CTest aliases named `test_oracle_*` execute those twins.
`ORACLE_TEST_SOURCES` is intentionally absent from `tests/CMakeLists.txt` so a
dead source list cannot be mistaken for executable coverage.

| Frozen carrier | Executing native-route twin |
|---|---|
| `coof` | `test_native_oracle_coof_l2` |
| `day_key` | `test_native_oracle_day_key_l2` |
| `deferred_any_witnesses` | `test_native_oracle_deferred_any_witnesses_l2` |
| `deferred_birth` | `test_native_oracle_deferred_birth_l2` |
| `frozen_size` | `test_native_oracle_frozen_size_full_l2` |
| `fx` | `test_native_oracle_fx_l2` |
| `magnifier_barstate` | `test_native_oracle_magnifier_barstate_l2` |
| `magnifier_distribution` | `test_native_oracle_magnifier_distribution_l2` |
| `more_than_64_fills` | `test_native_oracle_more_than_64_fills_l2` |
| `pooc_freeze` | `test_native_oracle_pooc_freeze_l2` |
| `pooc_immediate` | `test_native_oracle_pooc_immediate_l2` |
| `relative_exit` | `test_native_oracle_relative_exit_l2` |
| `reversal_close_only` | `test_native_oracle_reversal_close_only_l2` |
| `reversal_later_tick` | `test_native_oracle_reversal_later_tick_l2` |
| `reversal_replaced_percent` | `test_native_oracle_reversal_replaced_percent_l2` |
| `reversal_same_bar_tx` | `test_native_oracle_reversal_same_bar_tx_l2` |
| `short_seed` | `test_native_oracle_short_seed_full_l2` |
| `short_seed_percent` | `test_native_oracle_short_seed_percent_full_l2` |
| `stop_snapshot` | `test_native_oracle_stop_snapshot_full_l2` |

`scripts/check_oracle_twin_census.py` uses Python `re` to require one direct
include per carrier and to pin the explicit `CHECK` census. It rejects a
hand-copied twin body. `scripts/check_oracle_sha256.py` authenticates the
complete `tests/oracle/` tree against `tests/oracle.sha256`; changing a frozen
literal therefore requires an intentional pin update and review.

The four owner-internal carriers deleted with the old book (`deferred_any`,
`fifo_cohort`, `reversal`, and `coof_first_open`) have per-file coverage rows
in the R4-D deletion ledger. Their public observable literals are carried by
the executing twins above; no legacy owner is retained just to compile them.
