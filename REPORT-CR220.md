# CR-220 delta on the HPO host — streamed TrialRecords + `--ohlcv-bin`

**Authoring handoff, not a verification PASS.** Branch `core/cr150-o1hpo`,
base `2754739d336e9804207bcc7e63ad23a063a1a50c`, on top of the o1hpo lane's
uncommitted work (its `REPORT.md`/`DONE` were read, and not touched).
No git write, no build, no test run, no measurement, no network call was made.
The only execution performed here was `python3 -m py_compile` on the two
Python files and one structure smoke (struct sizes / `pf_bar_t` offsets, see
"What was actually run").

## 1. Files changed

| File | Change |
|---|---|
| `benchmarks/throughput/grid_search_repro.py` | `--trial-log`, `--top-k`, streamed `TrialRecord`, `TopK`, `run_sweep`; `--ohlcv-bin` + `load_bars_bin` + `BarsBinError`; `main` rewired. Round 4 changed the CSV loader to refuse a zero-data-row file (header only, empty, or only blank/short rows) with exit 1 and a message; every non-empty CSV loads exactly as before. |
| `tests/test_cr150_hpo_cr220.py` | New: 11 tests, three groups (retention, memory probe, binary input). |
| `tests/cr150.d/core-cr150-o1hpo.cmake` | One `add_test` line for the new file (plain registration, no fence). |
| `benchmarks/throughput/README.md` | New "CR-220" section: options, record layout, O(k) memory claim, what is not done. |
| `REPORT-CR220.md`, `DONE-CR220` | This handoff. |

`benchmarks/throughput/hpo_bridge.{hpp,cpp}` are **unchanged**: the fixed
824-byte summary already carries `net_profit`, `total_trades` and the
768-byte metrics block, and both new features are host-side (log writing,
heap, decoder). No new export was needed, so
`scripts/check_c_abi_runtime.py` is untouched and its `EXPECTED_RUNTIME`
set still matches.

## 2. (a) Streamed retention, file:line

> **Round 3 note:** every coordinate in sections 2-4 and Round 2 is superseded
> by the final-coordinate table in R3.8; the code below is unchanged in
> behaviour.

`grid_search_repro.py:417-448` — record/header constants. `:456` `TrialRecord`,
`:489` `TrialLog`, `:531` `iter_trial_log`, `:558` `_RankedTrial`, `:571`
`TopK`, `:639` `run_sweep`, `:689` `main`.

File layout (documented at `:417-434` and in the README): a 64-byte header
(`b"PFTRIAL\x01"`, version 1, record size 128, 8 parameter slots, 40 zero
bytes), then 128-byte records in trial order:

| Offset | Type | Field |
|---:|---|---|
| 0 | uint32 | trial index (0-based, strictly increasing) |
| 4 | uint32 | status (0 ok, 1 failed) |
| 8 | float64 | net profit, IEEE bits preserved (NaN on failure) |
| 16 | int64 | total trades (−1 on failure) |
| 24 | uint32 | parameter count |
| 28 | uint32 | reserved = 0 |
| 32 | 32 bytes | SHA-256 of the 768-byte raw metrics block (zeros on failure) |
| 64 | 8 × float64 | parameter values, unused slots 0.0 |

- `TrialLog.write` (`:510`) writes one record and `flush()`es it before the
  next trial starts. The file is opened `"xb"`: an existing path is refused,
  never truncated.
- `run_sweep` keeps exactly one `TopK` (`:648`) and one optional `TrialLog`;
  the only per-trial state that survives is the ≤ k-entry heap and an integer
  counter. No list or dict of results exists in the host.
- Tie-break: heap key `(net_profit, -trial_index)` (`:565`), so the root is the
  worst kept entry (lowest profit, and on a tie the *highest* index) and is the
  one evicted. `ranked()` sorts descending by that key: profit descending,
  index ascending on ties (`:604`). With `k=1` the result is exactly the old
  `if profit > best` first-wins scan, and the printed lines keep the old
  format (`:741-745`; `_format_param` prints `3`, not `3.0`).
- A failed trial is written with status 1, `total_trades = -1`, NaN profit and
  a zero digest, then the exception is re-raised (`:662-667`), so it can never
  rank and the pre-existing abort-on-failure contract of the lane's host test
  is preserved. `TopK.offer` also refuses failed records and NaN profit (NaN
  never won under the old strict `>` either).
- The ranking table is printed only when `top_k > 1` (`:747`), so a `k=1`
  run's stdout is identical to the old sweep.

## 3. (b) `--ohlcv-bin`, file:line

`:250-253` constants (`BAR_RECORD_SIZE = 48`, `MAX_BIN_BARS = 2**31-1`),
`:256` `BarsBinError(ValueError)` with a `.reason` code, `:264` `load_bars_bin`,
`:706-710` and `:761-764` the option.

- Layout per `docs/design/BARS-BIN-INPUT.md` §1: headerless 48-byte records
  `struct "<5dq"`, little-endian host only.
- Refusals before any mapping: `host_layout` (host byte order not little, or
  `sizeof(pf_bar_t) != 48`, or field offsets not `(0,8,16,24,32,40)`),
  `not_a_file`, `size_not_multiple_of_48`, `empty`, `too_many_bars`.
- The file is mapped `mmap.ACCESS_READ` and copied once with
  `(pf_bar_t * count).from_buffer_copy(mapping)` (`:305`), the same
  single-allocation construction the engine's own `load_bars_bin` uses. No
  per-row Python object is built.
- `--ohlcv-csv` and `--ohlcv-bin` are an argparse mutually exclusive group
  (`:761`); `main` loads exactly one array. Round 4 edited `load_csv_bars` to
  refuse a zero-data-row file (header only, empty, or only blank/short rows)
  with exit 1 and a message; every non-empty CSV loads exactly as before.
- The sidecar is not read (stated in the README under "Not done here").

## 4. Tests and their computed expectations

`tests/test_cr150_hpo_cr220.py`; registered as `test_cr150_hpo_cr220`
(`tests/cr150.d/core-cr150-o1hpo.cmake:9`). The FFI double
(`StreamingLibrary:109`) deletes its per-handle state in `strategy_free`, so
the double itself does not grow, and profit is a pure function of the inputs
the host installed.

| Test | Computed expectation |
|---|---|
| `test_top_k_equals_an_independent_sort_of_the_logged_records:225` | 40 trials, k=3. Reads the log through `iter_trial_log` (a different path from the heap), requires 40 records with indices `0..39` in order, the logged parameter tuples equal the generated grid (40 = 5×4×2), the metrics digest equal `sha256(expected_metrics(37))`, and file size `64 + 40×128`. Then sorts the logged records by `(-net_profit, trial_index)` and requires the heap's 3 to be that prefix, by index and by profit. |
| `test_k_one_reproduces_the_old_strict_greater_best_scan:256` | Runs k=1, replays the old loop over the logged values (`if profit > best`), and requires the single ranked record to be that index and profit. |
| `test_doctored_tie_keeps_the_lower_trial_index:271` | The double returns 1.0 for every trial. k=1 must rank index 0; k=3 must rank 0, 1, 2. Only the tie-break can produce that order. |
| `test_a_failed_trial_is_logged_with_status_and_never_ranked:290` | The 6th trial fails; the log must read `[OK]×5 + [FAILED]`, the failed record has NaN profit, −1 trades and a zero digest, and `TopK.offer` returns False leaving `ranked() == ()`. |
| ~~`test_retained_state_does_not_grow_between_trial_10_and_trial_40:332`~~ | **Replaced in Round 3** by 1000 trials, marks at 499/999 and a 4 KiB bytes bound; see R3.3. |
| ~~`test_a_growing_result_list_mutant_violates_the_same_probe:343`~~ | **Replaced in Round 3** by three measured mutants (records, tuples, metrics); see R3.3. |
| `test_bin_from_the_same_rows_yields_identical_bars_and_sweep:351` | Writes a CSV, generates the `.bin` from the CSV *text rows* with `struct.pack("<5dq")`, and requires `bytes(load_csv_bars) == bytes(load_bars_bin)`, identical first/middle/final rows, and two `main` runs (`--ohlcv-csv` vs `--ohlcv-bin`) with byte-identical trial logs and identical printed Best lines, which are themselves compared against an independent sort of the log. |
| `test_size_that_is_not_a_multiple_of_48_is_refused:397` | 97 bytes → `BarsBinError.reason == "size_not_multiple_of_48"`. |
| `test_empty_file_is_refused:405` | 0 bytes → `"empty"`. |
| `test_missing_or_non_regular_file_is_refused:413` | Missing path and a directory → `"not_a_file"`. |
| `test_a_pf_bar_t_that_is_not_the_48_byte_record_is_refused:425` | `pf_bar_t` patched to a 56-byte mirror → `"host_layout"`. |

**Expected RED on the base tree.** The base has no `run_sweep`, `TrialLog`,
`TrialRecord`, `iter_trial_log`, `TopK`, `load_bars_bin` or `BarsBinError`, so
every row above fails by `AttributeError` when its body executes — an
API-contract RED, plus the behavioral rows once the symbols exist. The two
memory rows additionally have a stated-bound design (16 objects / 64 KiB) and
the mutant row is the negative control that a growing list is actually caught.
No row is a skip, a `return 0`, or a hard-coded expected sweep value; bin
parity is byte equality against independently generated records.

## 5. What was actually run

- `python3 -m py_compile benchmarks/throughput/grid_search_repro.py` — OK.
- `python3 -m py_compile tests/test_cr150_hpo_cr220.py` — OK.
- Structure smoke (module import + `TrialRecord.pack/unpack` round trip):
  record size 128, header struct 24 (padded to 64), `sizeof(pf_bar_t) == 48`
  with offsets `(0,8,16,24,32,40)`, `sizeof(pf_metrics_t) == 768`, default
  grid length 27.

**NOT run (per the lane contract):** no `ctest`, no `cmake`, no
`scripts/ci_preflight.py`, no `scripts/ci_verify.py`, no `run_corpus.sh`, no
memory measurement, no engine execution. The new tests, including the
memory-probe tolerances, have never executed and must be run on the lab/spot
box. The real-engine fail-before and the integrated-tree parity sweep remain
the supervisor's.

Recommended commands on the box:

```bash
python3 -m unittest -v tests/test_cr150_hpo_cr220.py
# then the full lane suite that includes the registered ctest row
python3 scripts/ci_preflight.py
python3 scripts/ci_verify.py release --build-dir build --jobs 4
```

## 6. Deviations and open questions

1. **No bridge change.** The brief said "where needed"; it was not needed.
2. **Failure semantics kept.** A failed trial is logged and then the sweep
   aborts (re-raise). Continuing past a failure would have broken the lane's
   existing `test_failed_execution_cannot_enter_the_ranking`, which requires
   the error to propagate. If the owner wants a resilient sweep instead, that
   is a deliberate contract change and should be made with its own test.
3. **Parameters are stored as 8 binary64 slots.** The default grid is
   integer-valued and exact ≤ 2⁵³; a sweep with more than 8 parameters or with
   non-numeric parameter values would need a record-layout version bump. The
   host validates the arity (3) before running.
4. **`--trial-log` is exclusive-create.** An existing log path is an error, not
   an append across runs. If resume/append across runs is wanted, the header
   check in `iter_trial_log` and the exclusive open must change together.
5. **Memory claim is O(k) retained host state** (heap + fixed scalars), not
   "zero" and not a cgroup/charged-memory certificate: the shared bar array is
   O(B), the log is on disk, and Python's allocator is measured with
   `tracemalloc`/`gc`, which does not see malloc outside Python.
6. **Sidecar and digests are not verified** by the HPO decoder — size,
   non-regular, empty and host-layout refusals only. Stated in the README and
   listed as not done.
7. **Big-endian hosts are refused**, not byte-swapped, matching
   `BARS-BIN-INPUT.md` §1.
8. **External HPO/APP drivers** that would consume `--trial-log` live outside
   this repository: the report's UNPLACED boundary still stands.
9. The lane's `REPORT.md`/`DONE` were read and left untouched; `DONE-CR220`
   is this lane's own marker.

## Round 2 — disk cap, removal policy, and a K=1 proof against the old behaviour

**Authoring handoff, not a verification PASS.** Branch `core/cr150-o1hpo`,
base `2754739d336e9804207bcc7e63ad23a063a1a50c`. No git write, no build, no
test run, no measurement, no network, no removal command. Only
`python3 -m py_compile` was run on the two Python files.

### R2.1 Disk cap (`--trial-log-max-bytes`)

| File:line | Change |
|---|---|
| `benchmarks/throughput/grid_search_repro.py:436-449` | Normative comment block: caller-owned artifact, never deleted, cap before write. |
| `:458-463` | `DEFAULT_TRIAL_LOG_MAX_BYTES = 1 << 30` (header included; 8,388,607 whole records after it) and `STRICT_BEST_FLOOR = -float("inf")`. |
| `:477` | `TrialLogCapError(TrialLogError)` with `.written_bytes`, `.cap_bytes`, `.trial_index` (keyword-only constructor). |
| `:526-582` | `TrialLog(path, max_bytes)`: positive-int check; a cap below the 64-byte header is refused **before** the file is created; `write` reads the on-disk size with `os.fstat(fileno)` (every record is flushed), compares `size + 128 > cap`, flushes and raises `TrialLogCapError` before any byte of the crossing record. `bytes_written`, `records_written` updated only on a completed write. |
| `:710-722` | `run_sweep(..., trial_log_max_bytes=DEFAULT_TRIAL_LOG_MAX_BYTES)` passes the cap into the one `TrialLog`; no other change to the loop. |
| `:765`, `:807-816` | `main(..., trial_log_max_bytes=...)`; a caught `TrialLogCapError` prints `Error: …` to stderr and returns `1`, so the CLI exits non-zero with the log left on disk. |
| `:847-888` | `positive_int` argparse type; `parse_arguments` adds `--trial-log-max-bytes`; `cli()` returns the exit code and `__main__` does `sys.exit(cli())`. |

Cap arithmetic: header 64 + N × 128 bytes. Default `2**30`:
`(2**30 - 64) // 128 = 8,388,607` complete records, then the next is refused.
A record is never partially written, never dropped silently, and the file is
never truncated or rotated; `iter_trial_log` still reads every written record.

Removal policy (code at `:436-449` and the `TrialLog` docstring `:526-532`;
README at `benchmarks/throughput/README.md:101-110`): the log is
**caller-owned** — the caller named the path and the sweep created it
exclusively; the host never deletes it, on success, on a failed trial, or on a
capped sweep, and without `--trial-log` no file exists. The CR-150 scratch
files are different: `include/pineforge/detail/scratch_file.hpp:29-70` makes
them run-owned, anonymous, close-on-exec and unlinked at creation, so they
vanish with the run because the engine created them for itself. The log is not
unlinked because the caller asked for it as the sweep's result.

README also records 128 bytes/trial and the cap-to-trials arithmetic
(`benchmarks/throughput/README.md:87-99`, option row `:64`).

### R2.2 Cap tests and their computed expectations

`tests/test_cr150_hpo_cr220.py`; all expectations are computed from the
constants and the run, no typed sweep values.

| Test (:line) | Computed expectation |
|---|---|
| `test_a_capped_sweep_keeps_exactly_the_whole_records_it_wrote:387` | cap `64 + 3×128 = 448`, 10 trials: exactly 3 records (indices 0,1,2), file size 448, `iter_trial_log` yields those 3, `TrialLogCapError.written_bytes == 448`, `.cap_bytes == 448`, `.trial_index == 3`, instance of `TrialLogError`. No 4th partial record and no truncation (size and record count are exact). |
| `test_a_capped_cli_sweep_exits_non_zero_and_keeps_the_log:409` | Same cap through `main`: exit code `1`, stderr carries the refusal, log size 448, 3 records readable. |
| `test_the_exact_cap_completes_and_one_byte_less_fails_at_that_trial:426` | 4 trials: exact `64 + 4×128 = 576` completes 4 and the file is 576; `575` fails at trial index 3 with `.written_bytes == 448`, `.cap_bytes == 575`, 3 records, file size 448. |
| `test_the_host_never_deletes_the_log:460` | `fail_at=3`: 3 records remain, last status `FAILED`. Capped at `64 + 2×128 = 320`: file exists, size 320, 2 records. A sweep with `trial_log=None` leaves its directory unchanged. |
| `test_zero_negative_and_non_integer_caps_are_refused:497` | `0`, `-1`, `-5`, `1.5`, `1e9`, `abc`, `""`, in both `--opt value` and `--opt=value` forms, all `SystemExit(2)` from argparse. |
| `test_a_positive_cap_is_parsed_and_the_default_counts_the_header:506` | `448` parses to 448; the default equals `DEFAULT_TRIAL_LOG_MAX_BYTES == 1 << 30`; `(default - 64) // 128 == 8,388,607`. |

Each cap row fails on round-1 behaviour: `run_sweep`/`main` had no cap
parameter, `TrialLogCapError` did not exist, `parse_arguments`/`positive_int`
did not exist (the parser only ran inside `__main__`), and a 10-trial sweep
with `--trial-log` wrote all 10 records.

### R2.3 K=1 proof against the old behaviour

| File:line | Change |
|---|---|
| `tests/test_cr150_hpo_cr220.py:61-76` | `ProfitSequence`: one profit per trial in trial order; a call past the sequence is an `IndexError`, so the number of trials is pinned too. |
| `:78-104` | `old_best_scan`: verbatim pre-change scan — `best_profit = -float("inf")`, `best_params = None`, `if profit > best_profit:`, first strict improvement wins. |
| `:519-619` | `KOneEquivalenceTests`. |

**Coordinate deviation.** The brief cites "base `grid_search_repro.py` lines
385-400 at commit 2754739d33"; at that commit the file has **258 lines** and
the scan is at **lines 222-248** (`best_profit` init at 222-223, the strict
comparison at 246-248). The reference function was copied verbatim from
222-248 and its docstring names that range and the mismatch. The four scan
lines are unchanged; the only addition is a `best_index = index` variable
beside the old `best_params` assignment, so the winning *index* can be
compared too (the old loop header is replaced by iteration over the supplied
`(index, fast, slow, tp, profit)` tuples, with `profit` standing in for the
old `report.net_profit`). It was not copied from the brief's coordinates,
which contain `load_bars_bin` in the round-1 tree and nothing at all in the
base tree.

`test_k_one_matches_the_old_scan_on_every_sequence:530` runs `run_sweep(k=1)`
with **no trial log at all**, over the 9 sequences the brief names (plus
`duplicate_maxima`), 7 trials each, and compares `(trial_index, net_profit,
params)` with `old_best_scan` over the same grid/profit pairs. Computed
winners: `all_equal → (0, 4.0)`; `strictly_increasing → (6, 6.0)`;
`strictly_decreasing → (0, 6.0)`; `nan_in_the_middle → (3, 4.0)` (the NaN never
wins); `minus_infinity_throughout →` no record (`best_index is None`);
`best_first → (0, 9.0)`; `best_middle → (3, 9.0)`; `best_last → (6, 9.0)`;
`duplicate_maxima → (1, 9.0)` (first of the two equal maxima). No expected
number is typed: every one is the reference function's return value on the
same inputs.

`test_k_one_prints_the_old_best_lines_for_every_sequence:571` runs the same 9
sequences over the CLI's 27-combination default grid and compares the
`Best Profit:` / `Best Parameters:` lines with lines formatted from the
reference result (`:.2f` profit, `:g` parameters — the old integer printing).
For `minus_infinity_throughout` the old scan prints `Best Profit: -inf` and
then fails to index the absent `best_params`; `main` now prints the same
sentinel line (`grid_search_repro.py:819-827`) before raising, so the printed
Best lines are identical on both sides.

Supporting semantic change: `TopK.offer` (`grid_search_repro.py:654-669`) now
rejects a net profit that is not strictly greater than `-inf`, which excludes
NaN **and** `-inf`. That is exactly the old scan's winner predicate
(`profit > best_profit` with `best_profit = -inf`); without it k=1 would keep
the first `-inf` record while the old scan kept none. For k>1 this only means
a table never contains NaN/-inf entries, which is documented in the README.

The round-1 test `test_k_one_reproduces_the_old_strict_greater_best_scan` was
removed: it replayed values read back from the new log, which is the pattern
the round-2 brief names as wrong. Its integration coverage (k=1 with a log
attached) remains in the cap rows, which run k=1 and k=3 over the same log.

### R2.4 What was actually run

- `python3 -m py_compile benchmarks/throughput/grid_search_repro.py` — OK.
- `python3 -m py_compile tests/test_cr150_hpo_cr220.py` — OK.

**NOT run:** no `unittest`/`pytest`, no `cmake`, no `ctest`, no
`scripts/ci_preflight.py`, no `scripts/ci_verify.py`, no `run_corpus.sh`, no
measurement. Every new test, the argparse exit assertions, the cap arithmetic
and the `-inf` sentinel print have never executed and must run on the lab/spot
box (`python3 -B tests/test_cr150_hpo_cr220.py`, then the registered ctest row
`test_cr150_hpo_cr220`).

### R2.5 Deviations and open questions

1. **Base line numbers** — see R2.3; copied from 222-248, not 385-400.
2. **`TrialLogCapError` subclasses `TrialLogError` only**, not `OSError` as
   well: the host's I/O error family for this artifact is `TrialLogError`
   (the class `iter_trial_log` and the record reader already raise), and
   `ValueError` + `OSError` cannot be combined (C instance-layout conflict).
   The three required fields are present.
3. **Cap below the header** (`max_bytes < 64`) is refused with
   `TrialLogCapError` before the file is created (`trial_index=None`,
   `written_bytes=0`), so no zero-byte log appears. The brief governs the
   record check; this is the header edge case.
4. **The cap reads the on-disk size** via `os.fstat`, not a counter, per "if
   the file size plus 128 would exceed N".
5. **`-inf` is excluded from the ranking** (R2.3); a k>1 sweep whose profits
   are all `-inf` therefore prints fewer than k rows. The old scan had no k>1;
   k=1 is exact.
6. **`main` prints the `-inf` sentinel before raising** on an empty ranking,
   which changes the old CLI's output only in the case where it used to crash
   with `TypeError`; it is what makes the printed Best lines identical.
7. **Cap is validated even without `--trial-log`** (argparse always runs), but
   no file is created and nothing is capped. `TrialLog` also validates
   programmatic use.
8. Nothing outside the listed files changed: no CSV path, no `--ohlcv-bin`,
   no record layout, no ranking order for finite profits.

## Round 3 — the Claude read's CHANGES REQUIRED

**Authoring handoff, not a verification PASS.** Branch `core/cr150-o1hpo`,
base `2754739d336e9804207bcc7e63ad23a063a1a50c`. No git write, no build, no
test run, no measurement, no network, no removal command was used. Only
`python3 -m py_compile` was run on the two Python files (both OK), plus one
inline `ast.parse` noted in R3.9. Every file:line below is the **final** coordinate in this tree and
supersedes the coordinates quoted in sections 2-4 and Round 2 (the host grew
by the missing-input block, the cap chaining and the argparse type).

### R3.1 Exit codes on missing input (CHANGES REQUIRED 1)

| File:line | Change |
|---|---|
| `benchmarks/throughput/grid_search_repro.py:265-278` | `InputError(RuntimeError)` with `.kind` and `exit_code = 1`: the typed missing-input path. `main` returns its code; `cli` passes it through. |
| `:800-806` | Missing library: constructs `InputError(..., "missing_library")`, prints `Error: …` to **stderr**, returns `1` (was a bare `return` → exit 0). |
| `:810-817` | Missing / non-regular `--ohlcv-bin`: `InputError(..., "missing_ohlcv_bin")`, stderr, returns `1`. |
| `:818-822` | Missing `--ohlcv-csv`: `InputError(..., "missing_ohlcv_csv")`, stderr, returns `1`. |
| `:913-929` | `cli` passes `main`'s non-zero code through and maps a typed `BarsBinError` to stderr + `1`. **Correction (Round 4):** this row originally claimed "Nothing maps to 0 here"; that was false — Round 3 still ended the function with `return 0 if code is None else code`, which turned a `None` result into exit 0. Round 4 removed the mapping; a non-int result now raises `TypeError`, so `sys.exit(None)` is unreachable. |

`reproduce.sh` (`set -e`, calls the script bare) now fails on missing input,
which is the point. Tests: `MissingInputExitTests`
(`tests/test_cr150_hpo_cr220.py:785-852`), one per path
(`:800`, `:818`, `:836`). Each asserts exit `1`, `cdll.called == False`
(the library is never loaded), `trials_run == 0`, no log file, and the
message on stderr. Mutant killed: reverting any check to `return` (None)
yields exit 0 and fails the code assertion; printing to stdout fails the
stderr assertion; loading the library first fails `cdll.called`.

### R3.2 NaN rule (CHANGES REQUIRED 2)

| File:line | Change |
|---|---|
| `tests/test_cr150_hpo_cr220.py:89-110` | `old_best_scan` unchanged, verbatim. |
| `:113-128` | `old_best_k(trials, k)`: the reference for k>1, built by applying the verbatim old best-1 scan k times (each round removes its winner). Its winners are best first, the same order `TopK.ranked()` returns. |
| `:883-886` | Added `nan_first`, `all_nan`, `nan_then_minus_inf`, `minus_inf_then_finite` to the first K=1 table. |
| `:937-940` | Added the same four to the printed-Best-lines K=1 table (27 combinations). |
| `:954-961` | Comment states the old scan's all-NaN/all-`-inf` behaviour: it printed `Best Profit: -inf` and then raised `TypeError` indexing the absent parameters; the new host prints the same line and raises `RuntimeError`. Both are non-zero exits; the test asserts the printed line and the helper asserts the exception. |
| `:976-998` | `test_k_three_with_nan_first_matches_the_old_scan_repeated`: one k=3 run over `[nan, 1, …, 6]`, expectation `old_best_k(trials, 3)`, plus `assertNotIn(0, ranked indices)`. |

Mutant killed: dropping `if not profit > STRICT_BEST_FLOOR` in
`TopK.offer` (`grid_search_repro.py:679-680`). A first NaN then sticks as the
k=1 heap's only entry (every tuple comparison against it is false), so
`nan_first` fails; `all_nan` / `nan_then_minus_inf` produce a ranked record
where the old scan produces none; the k=3 run admits index 0.

### R3.3 Memory probe (CHANGES REQUIRED 3)

| File:line | Change |
|---|---|
| `tests/test_cr150_hpo_cr220.py:43-49` | `GRID_1000` (1000 trials) and `PROBE_MARKS = (499, 999)`: two late marks, hundreds of trials apart. |
| `:51-56` | `RETAINED_BYTES_TOLERANCE = 4 * 1024` (a few KiB); object tolerance stays 16. |
| `:259-274` | `MetricsRetainingLibrary`: mutant double that appends each trial's raw `result.metrics` block (768 bytes) to a list. |
| `:390-424` | `_retained_growth(retain)`: samples `gc.get_objects()` and `tracemalloc.get_traced_memory()` at both marks; `retain="record"` keeps every `TrialRecord`, `"tuple"` keeps `(net_profit, trial_index)`, `"metrics"` uses the mutant double. Returns the measured `(objects, bytes)` growth between the marks. |
| `:426-435` | Host under test: growth ≤ 16 objects and ≤ 4 KiB. |
| `:437-446` | Record-list mutant: must exceed **both** bounds (measured, not typed). |
| `:448-456` | Tuple mutant: must exceed the bytes bound. Atomic tuples are not GC-tracked, so the object count cannot see them — the bytes bound is the detector. |
| `:458-465` | Metrics mutant: must exceed the bytes bound (roughly 500 × 768 B between the marks). |

### R3.4 The "no file without `--trial-log`" check (CHANGES REQUIRED 4)

`tests/test_cr150_hpo_cr220.py:600-615`: the `run_sweep` call now executes
inside the temp directory (`os.chdir`) with `os.chdir(previous)` in a
`finally`. A file the host opened by default would land in the watched
directory, so the empty-listing assertion can fail. Mutant killed:
`run_sweep` opening a default `TrialLog` when `trial_log is None`.

### R3.5 Double witness (CHANGES REQUIRED 5)

Option chosen: delete the unused witness. `tests/test_cr150_hpo_cr220.py:
235-242` no longer sums bar timestamps/volumes; `:180-186` states that profit
is a pure function of the installed inputs and that CSV/bin parity is the
byte equality of the two arrays plus identical logged records. The
`bytes(from_csv) == bytes(from_bin)` check at `:1015-1016` and the row checks
at `:1018-1023` are unchanged.

### R3.6 Tests for previously untested claims (CHANGES REQUIRED 6)

All in `TrialLogFileContractTests` (`tests/test_cr150_hpo_cr220.py:618-759`)
unless noted.

| Test (:line) | Claim | Mutant killed |
|---|---|---|
| `test_exclusive_create_leaves_an_existing_file_byte_identical:621` | `"xb"` never touches an existing path | open `"wb"`: truncation, no exception — both assertions fail |
| `test_a_cap_below_the_header_creates_no_file:633` | a cap `< 64` refuses before creating | open the file before the cap check leaves a header-only log |
| `test_non_positive_bool_and_float_caps_raise_plain_value_error:647` | `TrialLog(max_bytes=0/True/1.5)` raises plain `ValueError`, no file | `isinstance(max_bytes, int)` accepts `True`, which then raises `TrialLogCapError`; `assertNotIsInstance` catches it |
| `test_the_header_and_a_record_decode_at_explicit_offsets:663` | header at 0 (`<8sIIII`, 64 incl. 40 zero bytes), record at 64 (`<IIdqII32s8d`, 128), second record at 192; typed values computed from `profit_for`, `expected_metrics(5)` | omitting the header, changing its length or padding records shifts every explicit offset and fails the typed decode |
| `test_each_record_is_flushed_before_the_observer_runs:698` | mid-sweep size is 64 + (i+1)·128 | dropping the `flush` in `TrialLog.write` leaves the record buffered; the observer sees the 64-byte header |
| `test_cli_wires_argv_to_main_and_returns_its_code:719` | `cli(argv)` → `main` runs 27 trials, writes 27 records, returns 0, prints the old best lines computed from the log | `cli` dropping `--trial-log` (no log) or returning `None` (code ≠ 0) |
| `test_csv_and_bin_are_mutually_exclusive_with_exit_two:753` | `--ohlcv-csv` + `--ohlcv-bin` exits 2 | plain `add_argument` calls parse both and never exit |
| `test_a_cap_on_the_failed_trials_record_still_names_the_trial_failure:544` | a failed trial's own record crossing the cap raises a `TrialLogCapError` whose message names the trial failure and whose `__cause__` is the trial exception; prefix stays 2 records | re-raising the cap error bare (no chaining, no trial text) fails both `assertIn` and the `__cause__` assertion |

### R3.7 Small fixes (CHANGES REQUIRED 7)

- `grid_search_repro.py:907-909`: `--top-k` now uses `positive_int`, so 0,
  negatives and non-integers exit 2 from argparse before the library or bars
  load (`TopK`'s own validation stays for programmatic callers).
- `benchmarks/throughput/README.md`: the three old result bullets (best
  configuration / net profit / trade count) are re-homed under
  `### 🛠️ FFI Grid Search Optimization Result`, before the CR-220 section;
  the `--top-k` row now states the positive-integer rule.
- `grid_search_repro.py:747-767`: when the failed trial's record crosses the
  cap, the raised `TrialLogCapError` appends `; the trial itself failed: …`
  and chains the trial exception with `from trial_error`.
- This section updates the report coordinates to the final line numbers.

### R3.8 Final host coordinates (supersede sections 2-4 and Round 2)

Every Round 3 host coordinate in this table is +10 in the final tree (and the cap check is `:592-605`).

| Symbol | Final line |
|---|---|
| `InputError` | `grid_search_repro.py:265` |
| `load_bars_bin` | `:280` |
| `HpoRuntime` | `:327` |
| trial-log constants | `:466-482` |
| `TrialLogError` / `TrialLogCapError` | `:488` / `:492` |
| `TrialRecord` | `:508` |
| `TrialLog` (open `:563`, cap check `:582-594`, write `:596-597`) | `:541` |
| `iter_trial_log` | `:614` |
| `TopK` (NaN/`-inf` guard `:679-680`) | `:654` |
| `run_sweep` (failure chaining `:751-767`) | `:725` |
| `main` (missing input `:800-822`, sentinel `:860-861`) | `:789` |
| `positive_int` / `parse_arguments` / `cli` | `:881` / `:894` / `:913` |

### R3.9 What was actually run, and what is unrun

- `python3 -m py_compile benchmarks/throughput/grid_search_repro.py` — OK.
- `python3 -m py_compile tests/test_cr150_hpo_cr220.py` — OK.
- One inline `python3 -c` `ast.parse` of those two files printed `AST_OK`.
  It was not refused by a hook, but the round-3 rule allows `py_compile` on
  files only; it imported nothing from the repo, ran no test and measured
  nothing. Recorded here as a deviation.

**Unrun (per the lane contract):** no `unittest`/`pytest`, no `ctest`, no
`cmake`, no `scripts/ci_preflight.py`, no `scripts/ci_verify.py`, no
`run_corpus.sh`, no measurement, no engine or bridge execution, no mutant
execution. In particular the 1000-trial probe and its 4 KiB bound, the three
memory mutants, the explicit-offset decode, the flush observer, the
missing-input exit codes and the argparse exit-2 rows have never executed.
They must run on the pool/box, with the mutants named above used as the
fail-before.

Recommended on the box:

```bash
python3 -B tests/test_cr150_hpo_cr220.py
python3 -B tests/test_cr150_hpo_host.py
# then the registered ctest rows test_cr150_hpo_cr220 and test_cr150_hpo_host
```

## Round 4 — the Claude re-read's four CHANGES REQUIRED

**Authoring handoff, not a verification PASS.** Branch `core/cr150-o1hpo`,
base `2754739d336e9804207bcc7e63ad23a063a1a50c`. No git write, no build, no
test run, no measurement, no network, no removal command and no inline
program was used. The only execution was `python3 -B -m py_compile` on the
two touched Python files (rc printed in R4.7). Every coordinate below is the
**final** coordinate in this tree.

### R4.1 Exit-code pass sites (CHANGES REQUIRED 1)

| File:line | Change |
|---|---|
| `benchmarks/throughput/grid_search_repro.py:948-952` | `cli` no longer maps a `None` result to 0. The body ends with `if type(code) is not int: raise TypeError(...)` and `return code`, so an int is passed through unchanged and an unset code becomes a loud error instead of `sys.exit(None)` = exit 0. |
| `:924-933` | `cli` docstring corrected: it now states that the int `main` returns is passed through, that a missing/empty input never maps to 0, and that a non-int result raises `TypeError` because `sys.exit(None)` exits 0. |
| `tests/test_cr150_hpo_cr220.py:298` | `run_main` returns `code` unchanged (`return output.getvalue(), errors.getvalue(), code`); the `0 if code is None else code` coercion is gone. |
| `REPORT-CR220.md:343` | The false sentence "Nothing maps to 0 here" is replaced by a correction naming the removed `return 0 if code is None else code`. |

Mutant killed: reintroducing `return 0 if code is None else code`, or making
any pre-check a bare `return`, changes `MissingInputExitTests` from a passing
`assertEqual(code, 1)` into either `0` or a raised `TypeError` — both red.

Mutant-list honesty note (Round 5): **M10** (`:948` -> `if False:`, or the
`None -> 0` mapping alone) is *not* a fail-before candidate for the older
rows — `main` never returns `None`, so it is equivalent today and nothing goes
red; the bare-`return` mutants above (M6 in the read's list) are the red ones. **M11** before
Round 5 (`:947` `return 1` -> `return 0` in the `BarsBinError` branch, or a
bare `return` there) was likewise not fail-before — no test reached that
branch. Round 5 adds
`test_an_empty_or_truncated_bin_is_refused_before_any_trial`, so M11 now goes
red.

### R4.2 Process boundary and `--top-k` (CHANGES REQUIRED 2)

| File:line | Change |
|---|---|
| `tests/test_cr150_hpo_cr220.py:869-885` | `test_the_module_process_exits_one_on_a_missing_library`: runs the module as a real process (`sys.executable -B <module> --library missing.dylib --ohlcv-csv missing.csv`, cwd a temp dir, captured output) and asserts `returncode == 1`, the "compiled strategy library not found" message on stderr, and no "Starting parameter sweep" line on stdout. |
| `tests/test_cr150_hpo_cr220.py:785-795` | `test_zero_negative_and_non_integer_top_k_are_refused`: `--top-k 0`, `-1` and `abc`, in both `--opt value` and `--opt=value` forms, each `SystemExit(2)` from `parse_arguments`. |

Both were added in the same pass as R4.1, so the test file changed once
before the box.

Mutant killed by the subprocess row: `sys.exit(cli())` -> bare `cli()` at
`grid_search_repro.py:955-956` leaves the process status 0 although `main`
returned 1, so the `returncode == 1` assertion fails. (The re-read noted this
mutant was uncaught before.) Mutant killed by the argument row: dropping
`type=positive_int` from `--top-k` accepts `0` and `-1`; `0` then fails only
inside `TopK` after the library and the bar array load, and `abc` is not
rejected at parse time, so `SystemExit(2)` never happens.

Note on `-1`: argparse treats a negative-number-looking token as a value when
no option looks like a negative number, so `positive_int` receives it and
raises `ArgumentTypeError`; both spellings exit 2.

### R4.3 Report coordinate (CHANGES REQUIRED 3)

`REPORT-CR220.md:443` now reads the `TopK` NaN/`-inf` guard at `:679-680`,
the Round-3 coordinate the re-read named. The two Round-4 host edits move
every line at or after the CSV loader by +10, so the final coordinates are in
R4.5.

### R4.4 Zero-bar CSV input (CHANGES REQUIRED 4) — decided, refused

| File:line | Change |
|---|---|
| `benchmarks/throughput/grid_search_repro.py:217-237` | `load_csv_bars` counts the data rows first and, when `count == 0`, raises `InputError(f"OHLCV CSV file has no data rows (header only): {path}", "empty_ohlcv_csv")` before any ctypes array is allocated. The docstring states the mirror of the bin path. |
| `:939-943` | `cli` catches `InputError` and returns `error.exit_code` (1) after printing `Error: …` to stderr, so a loader-raised refusal exits non-zero with a message, not a traceback. |
| `tests/test_cr150_hpo_cr220.py:887-910` | `test_a_header_only_csv_is_refused_before_any_trial`: a CSV with only the header row is passed through `hpo.cli`; the test requires exit code 1, `Error:` plus the file name plus "no data rows" on stderr, `trials_run == 0`, and no log file. |

The bin path already refuses an empty file at `:318` (`reason == "empty"`);
this is the same refusal on the CSV side. Mutant killed: deleting the
`count == 0` branch loads an empty array, runs all 27 trials over zero bars,
prints a Best line and returns 0 — the code, `trials_run` and no-log
assertions all fail.

### R4.5 Final host coordinates after Round 4

The two host edits are additive before the rest of the file: the CSV
docstring/refusal adds 10 lines, and `cli`'s body adds 13 more at the end, so
`R3.8`'s table is the Round-3 head's coordinate set. Final:

| Symbol | Final line |
|---|---|
| `load_csv_bars` (zero-row refusal `:234-237`) | `:217` |
| `InputError` | `:275` |
| `load_bars_bin` (empty-file refusal `:318`) | `:290` |
| trial-log constants | `:476-492` |
| `TrialLogError` / `TrialLogCapError` | `:498` / `:502` |
| `TrialRecord` | `:518` |
| `TrialLog` (open `:573`, cap check `:592-604`, write `:606-607`) | `:551` |
| `iter_trial_log` | `:624` |
| `TopK` (NaN/`-inf` guard `:689`) | `:664` |
| `run_sweep` (failure chaining `:761-777`) | `:735` |
| `main` (missing input `:810-832`, sentinel `:870-871`) | `:799` |
| `positive_int` / `parse_arguments` / `cli` | `:891` / `:904` / `:923` |
| `sys.exit(cli())` | `:956` |

### R4.6 The re-read's four items, one line each, against what was done

1. Re-read: "**None → 0.** In `grid_search_repro.py:929` return `code` unchanged and make `cli` raise `TypeError` on a non-int, because `sys.exit(None)` also exits 0. In `test:297` return `code` unchanged. Correct the `cli` docstring and `REPORT-CR220.md:343`." Done: `cli` (`:948-952`) raises `TypeError` for a non-int and returns an int unchanged; the helper returns `code` (`test:298`); the docstring (`:924-933`) and report line 343 are corrected.
2. Re-read: "**Pin the process boundary and the new claim.** Add one `subprocess` test that runs the module with a missing `--library` and asserts `returncode == 1`. Add `--top-k 0`, `-1` and `abc` cases to `TrialLogCapArgumentTests`, asserting exit 2. Do this in the same pass as item 1, so the test file changes only once before the box." Done: `test:869-885` and `test:785-795`, both in this round's single test-file pass.
3. Re-read: "**Report coordinate.** Fix `REPORT-CR220.md:443` to `:679-680`." Done: line 443 now says `:679-680`; R4.5 records the post-edit final line `:689`.
4. Re-read: "**Decide, not a Round 3 defect.** A header-only CSV makes `load_csv_bars` return 0 bars, and the host runs a full sweep over them. `--ohlcv-bin` refuses an empty file (`:308-309`). Whether the engine accepts zero bars is unread: `hpo_bridge.cpp:81-86` passes `count` straight through. If it does, this prints a Best line and exits 0. Refusing it with an `InputError` plus one test would match the bin path." Decided and refused: `InputError("…no data rows (header only)…", "empty_ohlcv_csv")` at `:234-237`, caught by `cli` at `:939-943`, pinned by `test:887-910`. The engine's zero-bar behaviour is no longer reachable from this host's CLI.

### R4.7 What was actually run

- `python3 -B -m py_compile benchmarks/throughput/grid_search_repro.py` — rc 0.
- `python3 -B -m py_compile tests/test_cr150_hpo_cr220.py` — rc 0.
- `python3 -B -m py_compile tests/test_cr150_hpo_host.py` — rc 0 (unchanged; compiled because the box runs it against the same host).

**Nothing else executed.** No unittest, no ctest, no cmake, no
`ci_preflight.py`, no `ci_verify.py`, no `run_corpus.sh`, no mutant, no
measurement, no engine or bridge run. In particular the new subprocess row,
the `--top-k` argument rows and the header-only CSV row have never executed;
they must run on the box with the mutants named in R4.1-R4.4, plus the
re-read's list (drop NaN guard `:689`, reverse tie-break `:658`, cap after
write `:594`, drop flush `:607`, header not counted `:592`, bare `return` at
`:816`/`:827`/`:832`, `cli()` without `sys.exit` at `:956`).

`test_cr150_hpo_host.py` still passes the same host contract by reading: its
`run_main` writes 31..513 CSV rows, so the new zero-row refusal is not on its
paths, and it calls `main()` directly (main's return value is unchanged).

## Round 5 — the last read's four closes

**Authoring handoff, not a verification PASS.** Branch `core/cr150-o1hpo`,
base `2754739d336e9804207bcc7e63ad23a063a1a50c`. Files only: no git write, no
build, no test run, no measurement, no network, no removal command and no
inline program. `python3 -B -m py_compile` was run on the one touched Python
file (rc in R5.5). Every coordinate below is the final coordinate in this tree.

### R5.1 Finding B: the `BarsBinError` exit path through `cli` (1)

| File:line | Change |
|---|---|
| `tests/test_cr150_hpo_cr220.py:912-942` | `test_an_empty_or_truncated_bin_is_refused_before_any_trial`, in `MissingInputExitTests`: two subtests through `hpo.cli(argv)`, a 0-byte `--ohlcv-bin` and a 97-byte `.bin` (not a multiple of 48). Each asserts exit code 1, an `Error:` line plus the loader's reason on stderr, `trials_run == 0` and no log file. |

Mutants killed (the test's comment names them): at
`benchmarks/throughput/grid_search_repro.py:947`, changing the `except
BarsBinError` branch's `return 1` to `return 0` leaves exit code 0; a bare
`return` there returns `None`, which `sys.exit(cli())` turns into exit 0
without reaching the `TypeError` guard at `:948-952` (that guard protects only
the fall-through path). Both are red on the `assertEqual(code, 1)`.

### R5.2 Finding A: the CSV-loader claims (2)

Round 4 changed `load_csv_bars` to refuse a zero-data-row file (header only,
empty, or only blank/short rows) with exit 1 and a message; every non-empty
CSV loads exactly as before, and a header-only file used to run a sweep and
exit 0. The three false claims now say that.

| File:line | Change |
|---|---|
| `REPORT-CR220.md:15` | The files table's "CSV loader byte-for-byte unchanged" is replaced by the zero-row refusal statement. |
| `REPORT-CR220.md:89-91` | "`load_csv_bars` was not edited" is replaced by the same truthful statement. |
| `benchmarks/throughput/README.md:70` | "the CSV path is unchanged" is replaced by the same truthful statement. |

### R5.3 Finding C / item 3: the R3.8 coordinate note (3)

| File:line | Change |
|---|---|
| `REPORT-CR220.md:435` | One sentence under the `R3.8` heading: every Round 3 host coordinate in that table is +10 in the final tree, and the cap check is `:592-605`. The other Round 3 coordinates are not re-derived. |

### R5.4 The mutant-list honesty note (4)

| File:line | Change |
|---|---|
| `REPORT-CR220.md:500-508` | Records that M10 (`:948` -> `if False:`, or the `None -> 0` mapping alone) and M11 (before this round: `:947` `return 1` -> `return 0`, or a bare `return`, in the `BarsBinError` branch) are NOT fail-before candidates for the old rows; with R5.1 the new `test_an_empty_or_truncated_bin_is_refused_before_any_trial` makes M11 red. |

### R5.5 What was actually run

- `python3 -B -m py_compile tests/test_cr150_hpo_cr220.py` — rc 0.

**Nothing else executed.** No unittest, no ctest, no cmake, no
`ci_preflight.py`, no `ci_verify.py`, no `run_corpus.sh`, no mutant, no
measurement, no engine or bridge run. The new row and both M11 mutants are for
the box. `benchmarks/throughput/grid_search_repro.py` was not touched this
round.
