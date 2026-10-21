"""CR-220 host checks: streamed TrialRecord retention and pf_bar_t/v1 input.

Every expectation is computed from data the test itself writes (the trial log
read back through its own reader, an independent sort of the logged records,
the CSV rows the binary file was generated from). No expected sweep value is a
hard-coded constant. The FFI double keeps no per-trial state after
strategy_free, and the memory probe is shown to fail a growing-result mutant.
"""
from __future__ import annotations

import contextlib
import csv
import ctypes
import gc
import hashlib
import importlib.util
import io
import itertools
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "benchmarks/throughput/grid_search_repro.py"
spec = importlib.util.spec_from_file_location("cr150_hpo_cr220", MODULE)
hpo = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = hpo
spec.loader.exec_module(hpo)


# 5 x 4 x 2 = 40 trials for the ranking/cap fixtures.
GRID_40 = tuple(itertools.product(
    (2, 4, 6, 8, 10), (10, 12, 14, 16), (10, 15)))
assert len(GRID_40) == 40

# 10 x 10 x 10 = 1000 trials for the retained-state probe: hundreds to
# thousands, with both marks late, so a per-trial leak cannot hide in the
# startup transients.
GRID_1000 = tuple(itertools.product(
    range(2, 12), range(10, 20), range(10, 20)))
assert len(GRID_1000) == 1000
PROBE_MARKS = (499, 999)

# Retained-state tolerance between two late marks with k=3: the heap is
# already full at the first mark, so neither probe may grow structurally. The
# bytes bound is a few KiB; each mutant below is run through the same probe and
# must exceed it (the expectation is measured, never typed).
RETAINED_OBJECT_TOLERANCE = 16
RETAINED_BYTES_TOLERANCE = 4 * 1024


def profit_for(inputs):
    """Deterministic profit from the installed inputs (collisions are legal).

    The engine never sees this function; the FFI double calls it, so the test
    can recompute nothing it did not ask for and every ranking is derivable
    from the parameters the host itself set.
    """
    fast = int(float(inputs[b"Fast EMA"]))
    slow = int(float(inputs[b"Slow EMA"]))
    tp = int(float(inputs[b"Take profit (ticks)"]))
    return float((fast * 37 + slow * 11 + tp * 3) % 23)


class ProfitSequence:
    """Return a pre-built net profit per trial, in trial order.

    The double asks for one profit per trial; a call past the sequence is an
    IndexError, so the test also pins the number of trials actually run.
    """

    def __init__(self, values):
        self.values = tuple(values)
        self.calls = 0

    def __call__(self, _inputs):
        value = self.values[self.calls]
        self.calls += 1
        return value


def old_best_scan(trials):
    """The pre-change best-1 scan, copied verbatim.

    Source: benchmarks/throughput/grid_search_repro.py lines 222-248 at commit
    2754739d33 (the CR-220 base, before the streamed top-k heap replaced it).
    The brief cites "base grid_search_repro.py lines 385-400"; at that commit
    the file has 258 lines and the scan is at 222-248, so it was copied from
    there. The scan lines are verbatim; only the loop header differs: the old
    loop was `for fast, slow, tp in itertools.product(...)` with `profit =
    report.net_profit`, here each trial supplies (index, fast, slow, tp,
    profit), and `best_index` records which trial the old `best_params`
    assignment last accepted so the winning index can be compared too.
    """
    best_profit = -float("inf")
    best_params = None
    best_index = None
    for index, fast, slow, tp, profit in trials:
        if profit > best_profit:
            best_profit = profit
            best_params = (fast, slow, tp)
            best_index = index
    return best_index, best_params, best_profit


def old_best_k(trials, k):
    """The k best by applying the verbatim old best-1 scan k times.

    The pre-change host only had a best-1 scan, so this is the reference for
    k>1: each round takes the old scan's winner out of the remaining trials.
    A NaN or -inf never wins a round, exactly as in the old scan.
    """
    remaining = list(trials)
    winners = []
    while remaining and len(winners) < k:
        index, params, profit = old_best_scan(remaining)
        if index is None:
            break
        winners.append((index, params, profit))
        remaining = [trial for trial in remaining if trial[0] != index]
    return winners


def make_bars(count):
    bars = (hpo.pf_bar_t * count)()
    for index in range(count):
        bar = bars[index]
        bar.timestamp = 1704067200000 + index * 60000
        bar.open = 100.0 + index
        bar.high = 103.0 + index
        bar.low = 98.0 + index
        bar.close = 101.0 + index
        bar.volume = float(index + 1)
    return bars


def write_csv(path, count):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("timestamp", "open", "high", "low", "close", "volume"))
        for index in range(count):
            writer.writerow((1704067200000 + index * 60000, 100 + index % 7,
                             103 + index % 7, 98 + index % 7,
                             101 + index % 7, index + 1))


def write_bin_from_csv(csv_path, bin_path):
    """Generate pf_bar_t/v1 records from the CSV text rows, not from ctypes."""
    written = 0
    with csv_path.open("r", newline="") as stream, bin_path.open("wb") as out:
        reader = csv.reader(stream)
        next(reader, None)
        for row in reader:
            if len(row) < 6:
                continue
            out.write(struct.pack(
                "<5dq", float(row[1]), float(row[2]), float(row[3]),
                float(row[4]), float(row[5]), int(row[0])))
            written += 1
    return written


class Function:
    def __init__(self, function):
        self.function = function

    def __call__(self, *args):
        return self.function(*args)


class StreamingLibrary:
    """FFI double that retains nothing once a handle is freed.

    net_profit is a pure function of the trial's installed inputs. The bar
    array is not folded into profit: the CSV/bin parity check is the byte
    equality of the two arrays plus the identical records each sweep writes,
    not a second witness inside this double.
    """

    def __init__(self, profit, fail_at=None):
        self._profit = profit
        self._fail_at = fail_at
        self._next_handle = 0
        self._states = {}
        self.trials_run = 0
        self._error = b""
        functions = {
            "pf_abi_version": lambda: 4,
            "pf_report_output_version_v1": lambda: 1,
            "strategy_create": self._create,
            "strategy_free": self._free,
            "strategy_set_input": self._set_input,
            "strategy_set_report_output_mode_v1": lambda handle, mode: 0,
            "strategy_set_hpo_scratch_manifest_v1": self._install,
            "strategy_hpo_scratch_manifest_v1": self._receipt,
            "strategy_report_summary_v1": self._summary,
            "strategy_run_hpo_v1": self._run,
            "strategy_get_last_error": lambda handle: self._error,
            "strategy_get_last_error_code": lambda handle: b"fixture_error",
            "strategy_last_run_status": lambda handle: 1 if self._error else 0,
        }
        for name, function in functions.items():
            setattr(self, name, Function(function))

    def _create(self, unused):
        self._next_handle += 1
        self._states[self._next_handle] = {"inputs": {}, "manifest": b""}
        return self._next_handle

    def _free(self, handle):
        # The host must free every trial handle; dropping the state here is
        # what keeps this double itself from growing with trial count.
        del self._states[handle]

    def _set_input(self, handle, name, value):
        self._states[handle]["inputs"][name] = value

    def _install(self, handle, manifest):
        self._states[handle]["manifest"] = bytes(manifest._obj)
        return 0

    def _receipt(self, handle, out):
        ctypes.memmove(out, self._states[handle]["manifest"],
                       ctypes.sizeof(out._obj))
        return 0

    def _run(self, handle, bars, count):
        state = self._states[handle]
        state["count"] = count
        self.trials_run += 1
        if self._fail_at is not None and self.trials_run == self._fail_at:
            self._error = b"fixture trial failed"
            return -1
        return 0

    def _summary(self, handle, out):
        state = self._states[handle]
        result = out._obj
        result.output_mode = 1
        result.complete = 1
        result.total_trades = state["count"] // 4
        result.equity_point_count = state["count"]
        result.input_bars_processed = state["count"]
        result.script_bars_processed = state["count"]
        result.net_profit = self._profit(state["inputs"])
        result.metrics.all.num_trades = result.total_trades
        result.metrics.equity.open_pl = -0.0
        return 0


class MetricsRetainingLibrary(StreamingLibrary):
    """Mutant row: retain every trial's raw 768-byte metrics block.

    This is the leak shape a host could have if it kept `result.metrics`
    instead of streaming only the digest; the retained-state probe must catch
    it by bytes even though a tuple/bytes object is not GC-tracked.
    """

    def __init__(self, profit, fail_at=None):
        super().__init__(profit, fail_at)
        self.retained_metrics = []

    def _summary(self, handle, out):
        status = super()._summary(handle, out)
        self.retained_metrics.append(bytes(out._obj.metrics))
        return status


def expected_metrics(count):
    """The metrics block the double publishes for a run over `count` bars."""
    value = hpo.pf_metrics_t()
    value.all.num_trades = count // 4
    value.equity.open_pl = -0.0
    return bytes(value)


def run_main(root, library, *, csv_path=None, bin_path=None, trial_log=None,
             top_k=None, max_bytes=None):
    """Call main the way the CLI does; return (stdout, stderr, exit code)."""
    strategy = root / "strategy.so"
    strategy.touch()
    with mock.patch.object(hpo.ctypes, "CDLL", return_value=library), \
            contextlib.redirect_stdout(io.StringIO()) as output, \
            contextlib.redirect_stderr(io.StringIO()) as errors:
        code = hpo.main(
            strategy, csv_path, bin_path, trial_log,
            hpo.DEFAULT_TOP_K if top_k is None else top_k,
            hpo.DEFAULT_TRIAL_LOG_MAX_BYTES if max_bytes is None else max_bytes)
    return output.getvalue(), errors.getvalue(), code


def run_cli(root, library, **kwargs):
    return run_main(root, library, **kwargs)[0]


def best_lines(text):
    return tuple(line for line in text.splitlines()
                 if line.startswith(("Best Profit:", "Best Parameters:")))


class TrialRetentionTests(unittest.TestCase):
    def sweep(self, root, grid=GRID_40, top_k=3, library=None, observer=None):
        library = library or StreamingLibrary(profit_for)
        runtime = hpo.HpoRuntime(library)
        log_path = Path(root) / "trials.bin"
        result = hpo.run_sweep(runtime, make_bars(37), grid, trial_log=log_path,
                               top_k=top_k, observer=observer)
        return log_path, result

    def test_top_k_equals_an_independent_sort_of_the_logged_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path, result = self.sweep(temporary)
            logged = list(hpo.iter_trial_log(log_path))
            log_size = log_path.stat().st_size

        self.assertEqual(len(logged), 40, "one record per finished trial")
        self.assertEqual([record.trial_index for record in logged],
                         list(range(40)), "records are in trial order")
        self.assertEqual({record.status for record in logged},
                         {hpo.TRIAL_STATUS_OK})
        self.assertEqual([tuple(record.params) for record in logged],
                         [tuple(float(value) for value in combo)
                          for combo in GRID_40])
        digest = hashlib.sha256(expected_metrics(37)).digest()
        self.assertEqual({record.metrics_sha256 for record in logged}, {digest},
                         "the logged digest is the engine's 768-byte block")
        self.assertEqual(log_size,
                         hpo.TRIAL_LOG_HEADER_SIZE + 40 * hpo.TRIAL_LOG_RECORD_SIZE)

        self.assertGreaterEqual(len({record.net_profit for record in logged}), 2,
                                "the fixture must produce a nontrivial ranking")
        expected = sorted(logged, key=lambda record: (-record.net_profit,
                                                      record.trial_index))[:3]
        ranked = result.ranked
        self.assertEqual(len(ranked), 3, "the heap keeps exactly k=3 records")
        self.assertEqual([record.trial_index for record in ranked],
                         [record.trial_index for record in expected])
        self.assertEqual([record.net_profit for record in ranked],
                         [record.net_profit for record in expected])

    def test_doctored_tie_keeps_the_lower_trial_index(self):
        def every_trial_ties(inputs):
            return 1.0

        with tempfile.TemporaryDirectory() as temporary:
            _log_path, one = self.sweep(
                temporary, grid=GRID_40[:6], top_k=1,
                library=StreamingLibrary(every_trial_ties))
        # Every logged profit is identical; only the tie-break can order them.
        self.assertEqual([record.trial_index for record in one.ranked], [0])
        self.assertEqual({record.net_profit for record in one.ranked}, {1.0})

        with tempfile.TemporaryDirectory() as temporary:
            _log_path, three = self.sweep(
                temporary, grid=GRID_40[:6], top_k=3,
                library=StreamingLibrary(every_trial_ties))
        self.assertEqual([record.trial_index for record in three.ranked],
                         [0, 1, 2])

    def test_a_failed_trial_is_logged_with_status_and_never_ranked(self):
        library = StreamingLibrary(profit_for, fail_at=6)
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "trials.bin"
            runtime = hpo.HpoRuntime(library)
            with self.assertRaisesRegex(RuntimeError, "fixture trial failed"):
                hpo.run_sweep(runtime, make_bars(11), GRID_40[:8],
                              trial_log=log_path, top_k=3)
            logged = list(hpo.iter_trial_log(log_path))

        self.assertEqual([record.status for record in logged],
                         [hpo.TRIAL_STATUS_OK] * 5 + [hpo.TRIAL_STATUS_FAILED])
        failed = logged[-1]
        self.assertTrue(math.isnan(failed.net_profit))
        self.assertEqual(failed.total_trades, -1)
        self.assertEqual(failed.metrics_sha256, bytes(32))
        self.assertEqual(failed.params, tuple(float(value) for value in GRID_40[5]))

        ranking = hpo.TopK(3)
        self.assertFalse(ranking.offer(failed))
        self.assertEqual(ranking.ranked(), ())

    def _retained_growth(self, retain=None):
        """Objects/bytes retained between two late marks over 1000 trials.

        `retain` selects the mutant row: None is the host under test,
        "record" keeps every TrialRecord, "tuple" keeps (net_profit, index)
        tuples, and "metrics" makes the FFI double keep every raw 768-byte
        result.metrics block. The mutant rows are the negative controls: each
        one is measured by this same probe and must exceed the bound.
        """
        marks = {}
        kept = []

        def observe(index, record):
            if index in PROBE_MARKS:
                gc.collect()
                marks[index] = (len(gc.get_objects()),
                                tracemalloc.get_traced_memory()[0])
            if retain == "record":
                kept.append(record)
            elif retain == "tuple":
                kept.append((record.net_profit, record.trial_index))

        library = (MetricsRetainingLibrary(profit_for) if retain == "metrics"
                   else StreamingLibrary(profit_for))
        tracemalloc.start()
        try:
            with tempfile.TemporaryDirectory() as temporary:
                self.sweep(temporary, grid=GRID_1000, top_k=3,
                           library=library, observer=observe)
        finally:
            tracemalloc.stop()
        self.assertEqual(tuple(marks), PROBE_MARKS,
                         "both late marks must be sampled")
        return (marks[PROBE_MARKS[1]][0] - marks[PROBE_MARKS[0]][0],
                marks[PROBE_MARKS[1]][1] - marks[PROBE_MARKS[0]][1])

    def test_retained_state_does_not_grow_between_two_late_marks(self):
        objects, traced_bytes = self._retained_growth()
        self.assertLessEqual(
            objects, RETAINED_OBJECT_TOLERANCE,
            f"host retained {objects} more objects after trial "
            f"{PROBE_MARKS[1]} than after trial {PROBE_MARKS[0]}")
        self.assertLessEqual(
            traced_bytes, RETAINED_BYTES_TOLERANCE,
            f"host retained {traced_bytes} more traced bytes after trial "
            f"{PROBE_MARKS[1]} than after trial {PROBE_MARKS[0]}")

    def test_a_growing_result_list_mutant_violates_the_same_probe(self):
        # Mutant: a host that appends every TrialRecord to a list would have
        # to retain hundreds of records between the marks.
        objects, traced_bytes = self._retained_growth("record")
        self.assertGreater(
            objects, RETAINED_OBJECT_TOLERANCE,
            "the retained-state probe does not catch a growing record list")
        self.assertGreater(
            traced_bytes, RETAINED_BYTES_TOLERANCE,
            "a growing record list did not exceed the bytes bound")

    def test_a_retained_tuple_mutant_violates_the_bytes_bound(self):
        # Mutant: retaining (net_profit, index) tuples is the natural ranking
        # mistake. Atomic tuples are not GC-tracked, so only the bytes bound
        # can catch it; the expectation is this measured growth, not a typed
        # value.
        _objects, traced_bytes = self._retained_growth("tuple")
        self.assertGreater(
            traced_bytes, RETAINED_BYTES_TOLERANCE,
            "retained per-trial tuples did not exceed the bytes bound")

    def test_a_retained_metrics_mutant_violates_the_bytes_bound(self):
        # Mutant: retaining each trial's raw result.metrics (768 bytes) rather
        # than streaming its digest. bytes objects are untracked by the GC,
        # so the bytes bound is the only detector.
        _objects, traced_bytes = self._retained_growth("metrics")
        self.assertGreater(
            traced_bytes, RETAINED_BYTES_TOLERANCE,
            "retained result.metrics blocks did not exceed the bytes bound")


class TrialLogCapTests(unittest.TestCase):
    """--trial-log-max-bytes: typed refusal before the crossing record."""

    def test_a_capped_sweep_keeps_exactly_the_whole_records_it_wrote(self):
        cap = hpo.TRIAL_LOG_HEADER_SIZE + 3 * hpo.TRIAL_LOG_RECORD_SIZE  # 448
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "trials.bin"
            runtime = hpo.HpoRuntime(StreamingLibrary(profit_for))
            with self.assertRaises(hpo.TrialLogCapError) as caught:
                hpo.run_sweep(runtime, make_bars(11), GRID_40[:10],
                              trial_log=log_path, top_k=3,
                              trial_log_max_bytes=cap)
            error = caught.exception
            logged = list(hpo.iter_trial_log(log_path))
            size = log_path.stat().st_size

        self.assertEqual(len(logged), 3)
        self.assertEqual([record.trial_index for record in logged], [0, 1, 2])
        self.assertEqual(size, cap, "no truncated or partial fourth record")
        self.assertEqual(error.written_bytes, cap)
        self.assertEqual(error.cap_bytes, cap)
        self.assertEqual(error.trial_index, 3)
        # The host's trial-log I/O error family carries the refusal.
        self.assertIsInstance(error, hpo.TrialLogError)

    def test_a_capped_cli_sweep_exits_non_zero_and_keeps_the_log(self):
        cap = hpo.TRIAL_LOG_HEADER_SIZE + 3 * hpo.TRIAL_LOG_RECORD_SIZE
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            csv_path = root / "bars.csv"
            write_csv(csv_path, 9)
            log_path = root / "trials.bin"
            _out, errors, code = run_main(
                root, StreamingLibrary(profit_for), csv_path=csv_path,
                trial_log=log_path, top_k=3, max_bytes=cap)
            size = log_path.stat().st_size
            logged = list(hpo.iter_trial_log(log_path))
        self.assertEqual(code, 1, "a capped sweep aborts with a non-zero exit")
        self.assertIn("past the", errors)
        self.assertEqual(size, cap)
        self.assertEqual(len(logged), 3)

    def test_the_exact_cap_completes_and_one_byte_less_fails_at_that_trial(self):
        trials = GRID_40[:4]
        exact = (hpo.TRIAL_LOG_HEADER_SIZE
                 + len(trials) * hpo.TRIAL_LOG_RECORD_SIZE)
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "exact.bin"
            result = hpo.run_sweep(
                hpo.HpoRuntime(StreamingLibrary(profit_for)), make_bars(11),
                trials, trial_log=log_path, top_k=1,
                trial_log_max_bytes=exact)
            logged = list(hpo.iter_trial_log(log_path))
            size = log_path.stat().st_size
        self.assertEqual(result.trials, len(trials))
        self.assertEqual(len(logged), len(trials))
        self.assertEqual(size, exact)

        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "short.bin"
            with self.assertRaises(hpo.TrialLogCapError) as caught:
                hpo.run_sweep(
                    hpo.HpoRuntime(StreamingLibrary(profit_for)), make_bars(11),
                    trials, trial_log=log_path, top_k=1,
                    trial_log_max_bytes=exact - 1)
            error = caught.exception
            logged = list(hpo.iter_trial_log(log_path))
            size = log_path.stat().st_size
        # One byte short: the 4th trial (index 3) cannot be written.
        self.assertEqual(error.trial_index, len(trials) - 1)
        self.assertEqual(error.written_bytes,
                         exact - hpo.TRIAL_LOG_RECORD_SIZE)
        self.assertEqual(error.cap_bytes, exact - 1)
        self.assertEqual(len(logged), len(trials) - 1)
        self.assertEqual(size, exact - hpo.TRIAL_LOG_RECORD_SIZE)

    def test_a_cap_on_the_failed_trials_record_still_names_the_trial_failure(self):
        # The failed trial's own record is the one that crosses the cap. The
        # cap refusal must not hide the real trial failure: the printed cap
        # message names it and the trial exception is chained as __cause__.
        # Mutant: re-raising the cap error bare (no chaining, no trial text)
        # makes both assertions below fail.
        cap = hpo.TRIAL_LOG_HEADER_SIZE + 2 * hpo.TRIAL_LOG_RECORD_SIZE
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "failed-capped.bin"
            runtime = hpo.HpoRuntime(StreamingLibrary(profit_for, fail_at=3))
            with self.assertRaises(hpo.TrialLogCapError) as caught:
                hpo.run_sweep(runtime, make_bars(7), GRID_40[:6],
                              trial_log=log_path, top_k=1,
                              trial_log_max_bytes=cap)
            error = caught.exception
            size = log_path.stat().st_size
            logged = list(hpo.iter_trial_log(log_path))
        self.assertIn("fixture trial failed", str(error),
                      "the real trial failure must survive in the message")
        self.assertIsInstance(error.__cause__, RuntimeError,
                              "the trial exception must be chained")
        self.assertIn("fixture trial failed", str(error.__cause__))
        self.assertEqual(error.trial_index, 2)
        self.assertEqual(error.cap_bytes, cap)
        self.assertEqual(error.written_bytes, cap)
        self.assertEqual(size, cap)
        self.assertEqual([record.trial_index for record in logged], [0, 1])

    def test_the_host_never_deletes_the_log(self):
        # A failed sweep leaves the whole written prefix, including the
        # status-1 record, on disk.
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "failed.bin"
            runtime = hpo.HpoRuntime(StreamingLibrary(profit_for, fail_at=3))
            with self.assertRaisesRegex(RuntimeError, "fixture trial failed"):
                hpo.run_sweep(runtime, make_bars(7), GRID_40[:6],
                              trial_log=log_path, top_k=1)
            self.assertTrue(log_path.exists(),
                            "the caller-owned log survives a failed sweep")
            failed_logged = list(hpo.iter_trial_log(log_path))
        self.assertEqual(len(failed_logged), 3)
        self.assertEqual(failed_logged[-1].status, hpo.TRIAL_STATUS_FAILED)

        # A capped sweep leaves the whole prefix and nothing partial.
        cap = hpo.TRIAL_LOG_HEADER_SIZE + 2 * hpo.TRIAL_LOG_RECORD_SIZE
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "capped.bin"
            with self.assertRaises(hpo.TrialLogCapError):
                hpo.run_sweep(hpo.HpoRuntime(StreamingLibrary(profit_for)),
                              make_bars(7), GRID_40[:6], trial_log=log_path,
                              top_k=1, trial_log_max_bytes=cap)
            self.assertTrue(log_path.exists(),
                            "the caller-owned log survives a capped sweep")
            self.assertEqual(log_path.stat().st_size, cap)
            self.assertEqual(len(list(hpo.iter_trial_log(log_path))), 2)

        # Without --trial-log the host creates no file at all. Run from inside
        # the temp directory so a stray file the host opened by default would
        # land here and be seen; the previous-directory restore is a finally.
        # Mutant: run_sweep opening a default TrialLog when trial_log is None
        # (e.g. "trials.bin" in the cwd) adds an entry and fails this.
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as temporary:
            os.chdir(temporary)
            try:
                before = set(Path(temporary).iterdir())
                hpo.run_sweep(hpo.HpoRuntime(StreamingLibrary(profit_for)),
                              make_bars(7), GRID_40[:3], top_k=1)
                self.assertEqual(set(Path(temporary).iterdir()), before,
                                 "a sweep without --trial-log created a file")
            finally:
                os.chdir(previous)


class TrialLogFileContractTests(unittest.TestCase):
    """Claims the host and README make that nothing else pins."""

    def test_exclusive_create_leaves_an_existing_file_byte_identical(self):
        # Mutant: opening "wb" instead of "xb" truncates the caller's file and
        # raises nothing, so both assertions fail.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "existing.bin"
            original = b"caller-owned bytes that must survive"
            path.write_bytes(original)
            with self.assertRaises(FileExistsError):
                hpo.TrialLog(path)
            self.assertEqual(path.read_bytes(), original,
                             "exclusive create must never touch an existing file")

    def test_a_cap_below_the_header_creates_no_file(self):
        # Mutant: opening the file before the cap check leaves a zero-byte
        # (or header-only) log behind; the existence assertion fails.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "too-small.bin"
            with self.assertRaises(hpo.TrialLogCapError) as caught:
                hpo.TrialLog(path, max_bytes=hpo.TRIAL_LOG_HEADER_SIZE - 1)
            self.assertFalse(path.exists(),
                             "the cap refusal must precede file creation")
            error = caught.exception
            self.assertEqual((error.written_bytes, error.cap_bytes,
                              error.trial_index),
                             (0, hpo.TRIAL_LOG_HEADER_SIZE - 1, None))

    def test_non_positive_bool_and_float_caps_raise_plain_value_error(self):
        # Mutant: isinstance(max_bytes, int) instead of type(...) is int
        # accepts True (a bool is an int subclass), which then falls through
        # to the header check and raises TrialLogCapError; the not-instance
        # assertion is what catches that.
        with tempfile.TemporaryDirectory() as temporary:
            for value in (0, True, 1.5):
                with self.subTest(max_bytes=value):
                    path = Path(temporary) / f"invalid-{type(value).__name__}.bin"
                    with self.assertRaises(ValueError) as caught:
                        hpo.TrialLog(path, max_bytes=value)
                    self.assertIs(type(caught.exception), ValueError)
                    self.assertNotIsInstance(caught.exception,
                                             hpo.TrialLogCapError)
                    self.assertFalse(path.exists())

    def test_the_header_and_a_record_decode_at_explicit_offsets(self):
        # Mutant: a log without the 64-byte header (records at offset 0), a
        # different header length, or padded records shift every explicit
        # offset below and fail the typed decode.
        trials = GRID_40[:2]
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "offsets.bin"
            hpo.run_sweep(hpo.HpoRuntime(StreamingLibrary(profit_for)),
                          make_bars(5), trials, trial_log=log_path, top_k=1)
            data = log_path.read_bytes()

        self.assertEqual(hpo.TRIAL_LOG_HEADER_SIZE, 64)
        self.assertEqual(hpo.TRIAL_LOG_RECORD_SIZE, 128)
        self.assertEqual(len(data), 64 + 2 * 128)

        magic, version, record_size, slots, reserved = struct.unpack_from(
            "<8sIIII", data, 0)
        self.assertEqual((magic, version, record_size, slots, reserved),
                         (hpo.TRIAL_LOG_MAGIC, 1, 128, 8, 0))
        self.assertEqual(data[24:64], b"\0" * 40, "header padding is zero")

        fast, slow, tp = (float(value) for value in trials[0])
        (index, status, profit, trades, count, pad, digest,
         *param_slots) = struct.unpack_from("<IIdqII32s8d", data, 64)
        self.assertEqual((index, status, trades, count, pad),
                         (0, hpo.TRIAL_STATUS_OK, 5 // 4, 3, 0))
        self.assertEqual(profit, profit_for({
            b"Fast EMA": fast, b"Slow EMA": slow,
            b"Take profit (ticks)": tp}))
        self.assertEqual(digest, hashlib.sha256(expected_metrics(5)).digest())
        self.assertEqual(tuple(param_slots), (fast, slow, tp, 0.0, 0.0, 0.0,
                                              0.0, 0.0))
        # The second record starts exactly one typed record later.
        self.assertEqual(struct.unpack_from("<I", data, 64 + 128)[0], 1)

    def test_each_record_is_flushed_before_the_observer_runs(self):
        # Pins per-trial flush directly: the file size the observer sees is
        # 64 + (index + 1) * 128 with no later write needed. Mutant: dropping
        # the flush in TrialLog.write leaves the record in Python's buffer,
        # so the observed size stays at the 64-byte header.
        observed = []

        def observe(index, record):
            observed.append(index)
            self.assertEqual(
                log_path.stat().st_size,
                64 + (index + 1) * 128,
                f"trial {index} was not flushed before its observer ran")

        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "flush.bin"
            hpo.run_sweep(hpo.HpoRuntime(StreamingLibrary(profit_for)),
                          make_bars(5), GRID_40[:4], trial_log=log_path,
                          top_k=1, observer=observe)
        self.assertEqual(observed, [0, 1, 2, 3])

    def test_cli_wires_argv_to_main_and_returns_its_code(self):
        # Mutant: cli dropping the --trial-log argument (or returning None
        # instead of main's code) leaves no log and a code of None; both
        # assertions fail.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            strategy = root / "strategy.so"
            strategy.touch()
            csv_path = root / "bars.csv"
            write_csv(csv_path, 11)
            log_path = root / "cli.bin"
            library = StreamingLibrary(profit_for)
            argv = ["--library", str(strategy),
                    "--ohlcv-csv", str(csv_path),
                    "--trial-log", str(log_path), "--top-k", "1"]
            with mock.patch.object(hpo.ctypes, "CDLL", return_value=library), \
                    contextlib.redirect_stdout(io.StringIO()) as output, \
                    contextlib.redirect_stderr(io.StringIO()):
                code = hpo.cli(argv)
            logged = list(hpo.iter_trial_log(log_path))

        self.assertEqual(code, 0)
        self.assertEqual(library.trials_run, 27, "the CLI ran the default grid")
        self.assertEqual(len(logged), 27)
        reference = old_best_scan(
            (record.trial_index, record.params[0], record.params[1],
             record.params[2], record.net_profit) for record in logged)
        self.assertIsNotNone(reference[0])
        self.assertEqual(best_lines(output.getvalue()), (
            f"Best Profit: {reference[2]:.2f}",
            "Best Parameters: Fast EMA = "
            f"{reference[1][0]:g}, Slow EMA = {reference[1][1]:g}, "
            f"Take profit (ticks) = {reference[1][2]:g}"))

    def test_csv_and_bin_are_mutually_exclusive_with_exit_two(self):
        # Mutant: replacing the mutually exclusive group with two independent
        # add_argument calls parses both paths and never exits 2.
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                hpo.cli(["--ohlcv-csv", "bars.csv", "--ohlcv-bin", "bars.bin"])
        self.assertEqual(caught.exception.code, 2)


class TrialLogCapArgumentTests(unittest.TestCase):
    def test_zero_negative_and_non_integer_caps_are_refused(self):
        for text in ("0", "-1", "-5", "1.5", "1e9", "abc", ""):
            for argv in (["--trial-log-max-bytes", text],
                         [f"--trial-log-max-bytes={text}"]):
                with self.subTest(argv=argv):
                    with self.assertRaises(SystemExit) as caught:
                        hpo.parse_arguments(argv)
                    self.assertEqual(caught.exception.code, 2)

    def test_a_positive_cap_is_parsed_and_the_default_counts_the_header(self):
        arguments = hpo.parse_arguments(["--trial-log-max-bytes", "448"])
        self.assertEqual(arguments.trial_log_max_bytes, 448)
        default = hpo.parse_arguments([]).trial_log_max_bytes
        self.assertEqual(default, hpo.DEFAULT_TRIAL_LOG_MAX_BYTES)
        self.assertEqual(default, 1 << 30)
        # 1 GiB / 128 bytes per record = 8,388,608; the 64-byte header takes
        # half a record, so 8,388,607 whole records fit after it.
        self.assertEqual(
            (default - hpo.TRIAL_LOG_HEADER_SIZE) // hpo.TRIAL_LOG_RECORD_SIZE,
            8_388_607)

    def test_zero_negative_and_non_integer_top_k_are_refused(self):
        # --top-k uses positive_int, so these exit 2 from argparse before the
        # library or the bars are touched. Mutant: dropping type=positive_int
        # leaves the default str, which parses all three and defers the 0 to a
        # ValueError inside TopK, after the library and the whole bar array
        # have loaded; no SystemExit(2) happens.
        for text in ("0", "-1", "abc"):
            for argv in (["--top-k", text], [f"--top-k={text}"]):
                with self.subTest(argv=argv):
                    with self.assertRaises(SystemExit) as caught:
                        hpo.parse_arguments(argv)
                    self.assertEqual(caught.exception.code, 2)


class MissingInputExitTests(unittest.TestCase):
    """A missing or empty input is a typed error, never a pass.

    Mutant: reverting any of the three pre-checks to a bare `return` yields
    None, which cli() refuses with a TypeError instead of letting
    `sys.exit(None)` exit 0; the code assertion and the no-log assertion both
    fail. Mutant: `sys.exit(cli())` -> bare `cli()` at the bottom of the
    module leaves the process status 0; the subprocess row fails.
    """

    def _run_cli(self, argv, library):
        with mock.patch.object(hpo.ctypes, "CDLL", return_value=library) as cdll, \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            code = hpo.cli(argv)
        return code, errors.getvalue(), cdll

    def test_missing_library_exits_non_zero_without_a_trial_or_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log_path = root / "trials.bin"
            library = StreamingLibrary(profit_for)
            code, errors, cdll = self._run_cli([
                "--library", str(root / "missing.dylib"),
                "--ohlcv-csv", str(root / "bars.csv"),
                "--trial-log", str(log_path)], library)
            self.assertFalse(cdll.called, "the library must not be loaded")
            self.assertEqual(code, 1)
            self.assertIn("Error:", errors)
            # main rewrites a missing .dylib to the .so suffix before failing.
            self.assertIn("compiled strategy library not found", errors)
            self.assertIn("missing.so", errors)
            self.assertEqual(library.trials_run, 0)
            self.assertFalse(log_path.exists())

    def test_missing_bin_exits_non_zero_without_a_trial_or_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            strategy = root / "strategy.so"
            strategy.touch()
            log_path = root / "trials.bin"
            library = StreamingLibrary(profit_for)
            code, errors, cdll = self._run_cli([
                "--library", str(strategy),
                "--ohlcv-bin", str(root / "missing.bin"),
                "--trial-log", str(log_path)], library)
            self.assertFalse(cdll.called, "the library must not be loaded")
            self.assertEqual(code, 1)
            self.assertIn("Error:", errors)
            self.assertIn("missing.bin", errors)
            self.assertEqual(library.trials_run, 0)
            self.assertFalse(log_path.exists())

    def test_missing_csv_exits_non_zero_without_a_trial_or_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            strategy = root / "strategy.so"
            strategy.touch()
            log_path = root / "trials.bin"
            library = StreamingLibrary(profit_for)
            code, errors, cdll = self._run_cli([
                "--library", str(strategy),
                "--ohlcv-csv", str(root / "missing.csv"),
                "--trial-log", str(log_path)], library)
            self.assertFalse(cdll.called, "the library must not be loaded")
            self.assertEqual(code, 1)
            self.assertIn("Error:", errors)
            self.assertIn("missing.csv", errors)
            self.assertEqual(library.trials_run, 0)
            self.assertFalse(log_path.exists())

    def test_the_module_process_exits_one_on_a_missing_library(self):
        # Pins the process boundary above cli(): the module is run as a real
        # process, so only `sys.exit(cli())` at the bottom can set the status.
        # Mutant: `sys.exit(cli())` -> bare `cli()` leaves the process status
        # 0 even though main returned 1, so returncode is 0 here.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = subprocess.run(
                [sys.executable, "-B", str(MODULE),
                 "--library", str(root / "missing.dylib"),
                 "--ohlcv-csv", str(root / "bars.csv")],
                cwd=temporary, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 1,
                         f"stderr: {completed.stderr!r}")
        self.assertIn("compiled strategy library not found", completed.stderr)
        self.assertNotIn("Starting parameter sweep", completed.stdout,
                         "no trial may run without the library")

    def test_a_header_only_csv_is_refused_before_any_trial(self):
        # A header-only CSV used to load zero bars, then run all 27 trials
        # over them, print a Best line and exit 0. Mutant: dropping the
        # zero-row check in load_csv_bars runs the sweep and returns code 0.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            strategy = root / "strategy.so"
            strategy.touch()
            csv_path = root / "header-only.csv"
            write_csv(csv_path, 0)
            log_path = root / "trials.bin"
            library = StreamingLibrary(profit_for)
            code, errors, _cdll = self._run_cli([
                "--library", str(strategy),
                "--ohlcv-csv", str(csv_path),
                "--trial-log", str(log_path)], library)
            self.assertEqual(code, 1)
            self.assertIn("Error:", errors)
            self.assertIn("header-only.csv", errors)
            self.assertIn("no data rows", errors)
            self.assertEqual(library.trials_run, 0,
                             "a zero-bar input must run no trial")
            self.assertFalse(log_path.exists())

    def test_an_empty_or_truncated_bin_is_refused_before_any_trial(self):
        # The `except BarsBinError` branch of cli() (:944-947) had no test
        # through cli: changing its `return 1` to `return 0`, or to a bare
        # `return` (None, which `sys.exit` turns into 0), left the suite
        # green, so a 0-byte or truncated --ohlcv-bin exited 0 with only an
        # `Error:` line and no trial-log owner to notice. Both mutants go red
        # here: the code assertion sees 0 (or a TypeError) instead of 1.
        cases = (
            ("empty", b"", "empty"),
            ("size_not_multiple_of_48", b"\0" * 97, "not a multiple of 48"),
        )
        for name, payload, reason in cases:
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    strategy = root / "strategy.so"
                    strategy.touch()
                    bin_path = root / "bars.bin"
                    bin_path.write_bytes(payload)
                    log_path = root / "trials.bin"
                    library = StreamingLibrary(profit_for)
                    code, errors, _cdll = self._run_cli([
                        "--library", str(strategy),
                        "--ohlcv-bin", str(bin_path),
                        "--trial-log", str(log_path)], library)
                    self.assertEqual(code, 1)
                    self.assertIn("Error:", errors)
                    self.assertIn(reason, errors)
                    self.assertEqual(library.trials_run, 0,
                                     "a malformed bar file must run no trial")
                    self.assertFalse(log_path.exists())


class KOneEquivalenceTests(unittest.TestCase):
    """k=1 must reproduce the pre-change scan, not a replay of the new log."""

    GRID = GRID_40[:7]

    def _sweep(self, profits):
        runtime = hpo.HpoRuntime(StreamingLibrary(ProfitSequence(profits)))
        # No trial log at all: the expected values can only come from the
        # verbatim reference scan, never from records read back from the log.
        return hpo.run_sweep(runtime, make_bars(5), self.GRID, top_k=1)

    def test_k_one_matches_the_old_scan_on_every_sequence(self):
        nan = float("nan")
        inf = float("inf")
        cases = {
            "all_equal": [4.0] * 7,
            "strictly_increasing": [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
            "strictly_decreasing": [6.0, 5.0, 4.0, 3.0, 2.0, 1.0, 0.0],
            "nan_in_the_middle": [1.0, 2.0, nan, 4.0, 3.0, 0.5, 2.5],
            "minus_infinity_throughout": [-inf] * 7,
            "best_first": [9.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
            "best_middle": [1.0, 2.0, 3.0, 9.0, 4.0, 5.0, 6.0],
            "best_last": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 9.0],
            "duplicate_maxima": [1.0, 9.0, 9.0, 2.0, 0.0, 3.0, 1.0],
            # NaN/inf placement. These are the sequences that fail a heap
            # with no `profit > -inf` guard: a first NaN sticks as the k=1
            # best (every tuple comparison against it is False), while the
            # old scan keeps its -inf floor and the later finite value wins.
            "nan_first": [nan, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
            "all_nan": [nan] * 7,
            "nan_then_minus_inf": [nan] + [-inf] * 6,
            "minus_inf_then_finite": [-inf, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        }
        for name, profits in cases.items():
            with self.subTest(sequence=name):
                reference = old_best_scan(
                    (index, float(fast), float(slow), float(tp), profit)
                    for index, ((fast, slow, tp), profit)
                    in enumerate(zip(self.GRID, profits)))
                result = self._sweep(profits)
                if reference[0] is None:
                    # Nothing strictly beat the old -inf sentinel.
                    self.assertEqual(result.ranked, ())
                    continue
                self.assertEqual(len(result.ranked), 1)
                record = result.ranked[0]
                self.assertEqual(record.trial_index, reference[0])
                self.assertEqual(record.net_profit, reference[2])
                self.assertEqual(tuple(record.params), reference[1])

    def _run_main_allow_error(self, root, library, csv_path, top_k=1):
        strategy = root / "strategy.so"
        strategy.touch()
        with mock.patch.object(hpo.ctypes, "CDLL", return_value=library), \
                contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(RuntimeError):
                hpo.main(strategy, csv_path, None, None, top_k)
        return output.getvalue()

    def test_k_one_prints_the_old_best_lines_for_every_sequence(self):
        grid = tuple(hpo.default_grid())  # 27 combinations, the CLI's grid
        nan = float("nan")
        inf = float("inf")
        nan_middle = [1.0] * 27
        nan_middle[13] = nan
        best_middle = [1.0] * 27
        best_middle[13] = 9.0
        duplicate = [1.0] * 27
        duplicate[5] = 9.0
        duplicate[19] = 9.0
        cases = {
            "all_equal": [4.0] * 27,
            "strictly_increasing": [float(index) for index in range(27)],
            "strictly_decreasing": [float(26 - index) for index in range(27)],
            "nan_in_the_middle": nan_middle,
            "minus_infinity_throughout": [-inf] * 27,
            "best_first": [9.0] + [1.0] * 26,
            "best_middle": best_middle,
            "best_last": [1.0] * 26 + [9.0],
            "duplicate_maxima": duplicate,
            # Same NaN/inf placement as the k=1 table above.
            "nan_first": [nan] + [1.0] * 26,
            "all_nan": [nan] * 27,
            "nan_then_minus_inf": [nan] + [-inf] * 26,
            "minus_inf_then_finite": [-inf, 1.0] + [0.0] * 25,
        }
        for name, profits in cases.items():
            with self.subTest(sequence=name):
                reference = old_best_scan(
                    (index, float(fast), float(slow), float(tp), profit)
                    for index, ((fast, slow, tp), profit)
                    in enumerate(zip(grid, profits)))
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    csv_path = root / "bars.csv"
                    write_csv(csv_path, 9)
                    library = StreamingLibrary(ProfitSequence(profits))
                    if reference[0] is None:
                        # all-NaN / all--inf: the old code printed
                        # "Best Profit: -inf" (its untouched sentinel) and
                        # then raised TypeError when it indexed the absent
                        # best_params. The new code prints the same Best line
                        # and raises RuntimeError. Both are non-zero exits;
                        # this asserts the printed line, and the helper's
                        # assertRaises(RuntimeError) is the non-zero proof
                        # (an uncaught exception exits non-zero, never 0).
                        output = self._run_main_allow_error(
                            root, library, csv_path)
                        expected = (f"Best Profit: {reference[2]:.2f}",)
                    else:
                        output = run_cli(root, library, csv_path=csv_path,
                                         top_k=1)
                        expected = (
                            f"Best Profit: {reference[2]:.2f}",
                            "Best Parameters: Fast EMA = "
                            f"{reference[1][0]:g}, Slow EMA = "
                            f"{reference[1][1]:g}, "
                            f"Take profit (ticks) = {reference[1][2]:g}")
                self.assertEqual(best_lines(output), expected)

    def test_k_three_with_nan_first_matches_the_old_scan_repeated(self):
        # One k>1 run with NaN first. The expectation is the verbatim old
        # best-1 scan applied three times (old_best_k), so a heap that admits
        # the first NaN differs from it.
        nan = float("nan")
        profits = [nan, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
        trials = [(index, float(fast), float(slow), float(tp), profit)
                  for index, ((fast, slow, tp), profit)
                  in enumerate(zip(self.GRID, profits))]
        # old_best_k removes the current best each round, so its winners are
        # already best first, the same order ranked() returns.
        expected = old_best_k(trials, 3)
        result = hpo.run_sweep(
            hpo.HpoRuntime(StreamingLibrary(ProfitSequence(profits))),
            make_bars(5), self.GRID, top_k=3)
        self.assertEqual([record.trial_index for record in result.ranked],
                         [winner[0] for winner in expected])
        self.assertEqual([record.net_profit for record in result.ranked],
                         [winner[2] for winner in expected])
        self.assertEqual([tuple(record.params) for record in result.ranked],
                         [winner[1] for winner in expected])
        self.assertNotIn(0, [record.trial_index for record in result.ranked],
                         "the first NaN must never enter the ranking")


class BinaryBarInputTests(unittest.TestCase):
    def test_bin_from_the_same_rows_yields_identical_bars_and_sweep(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            csv_path = root / "bars.csv"
            bin_path = root / "bars.bin"
            write_csv(csv_path, 57)
            written = write_bin_from_csv(csv_path, bin_path)

            self.assertEqual(written, 57)
            self.assertEqual(bin_path.stat().st_size, 57 * hpo.BAR_RECORD_SIZE)
            from_csv = hpo.load_csv_bars(csv_path)
            from_bin = hpo.load_bars_bin(bin_path)
            self.assertEqual(len(from_csv), len(from_bin))
            self.assertEqual(bytes(from_csv), bytes(from_bin),
                             "the binary decoder must not perturb one field")
            # The same first/middle/final row checks the CSV path is held to.
            for index in (0, 28, 56):
                self.assertEqual(
                    tuple(getattr(from_bin[index], field)
                          for field, _ in hpo.pf_bar_t._fields_),
                    tuple(getattr(from_csv[index], field)
                          for field, _ in hpo.pf_bar_t._fields_))

            csv_log = root / "csv.trials"
            bin_log = root / "bin.trials"
            csv_out = run_cli(root, StreamingLibrary(profit_for),
                              csv_path=csv_path, trial_log=csv_log)
            bin_out = run_cli(root, StreamingLibrary(profit_for),
                              bin_path=bin_path, trial_log=bin_log)

            self.assertEqual(csv_log.read_bytes(), bin_log.read_bytes(),
                             "both input options must log identical records")
            self.assertEqual(best_lines(csv_out), best_lines(bin_out))
            # The printed best is computed from the log, not compared to a
            # constant: the same records rank the same way through both paths.
            logged = list(hpo.iter_trial_log(bin_log))
            expected = sorted(logged, key=lambda record: (-record.net_profit,
                                                          record.trial_index))[0]
            self.assertEqual(
                best_lines(bin_out),
                (f"Best Profit: {expected.net_profit:.2f}",
                 "Best Parameters: Fast EMA = "
                 f"{expected.params[0]:g}, Slow EMA = "
                 f"{expected.params[1]:g}, Take profit (ticks) = "
                 f"{expected.params[2]:g}"))

    def test_size_that_is_not_a_multiple_of_48_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.bin"
            path.write_bytes(b"\0" * (hpo.BAR_RECORD_SIZE * 2 + 1))
            with self.assertRaises(hpo.BarsBinError) as caught:
                hpo.load_bars_bin(path)
        self.assertEqual(caught.exception.reason, "size_not_multiple_of_48")

    def test_empty_file_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "empty.bin"
            path.write_bytes(b"")
            with self.assertRaises(hpo.BarsBinError) as caught:
                hpo.load_bars_bin(path)
        self.assertEqual(caught.exception.reason, "empty")

    def test_missing_or_non_regular_file_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "missing.bin"
            with self.assertRaises(hpo.BarsBinError) as caught:
                hpo.load_bars_bin(path)
            directory = Path(temporary) / "bars.bin"
            directory.mkdir()
            with self.assertRaises(hpo.BarsBinError) as second:
                hpo.load_bars_bin(directory)
        self.assertEqual(caught.exception.reason, "not_a_file")
        self.assertEqual(second.exception.reason, "not_a_file")

    def test_a_pf_bar_t_that_is_not_the_48_byte_record_is_refused(self):
        class WideBar(ctypes.Structure):
            _fields_ = hpo.pf_bar_t._fields_ + [("padding", ctypes.c_double)]

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bars.bin"
            path.write_bytes(b"\0" * hpo.BAR_RECORD_SIZE)
            with mock.patch.object(hpo, "pf_bar_t", WideBar):
                with self.assertRaises(hpo.BarsBinError) as caught:
                    hpo.load_bars_bin(path)
        self.assertEqual(caught.exception.reason, "host_layout")


if __name__ == "__main__":
    unittest.main()
