"""Behavioral HPO host checks; run on the lab, including against the BASE host.

The double records actual FFI arguments. It supports the legacy report call so
the BASE fails assertions about allocations/mode/manifest, not missing symbols.
Engine execution and numeric parity have a separate real-library test.
"""
from __future__ import annotations

import contextlib
import csv
import ctypes
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "benchmarks/throughput/grid_search_repro.py"
spec = importlib.util.spec_from_file_location("cr150_hpo_host", MODULE)
hpo = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = hpo
spec.loader.exec_module(hpo)


class Function:
    def __init__(self, function):
        self.function = function

    def __call__(self, *args):
        return self.function(*args)


class Library:
    """FFI call recorder, with a legacy path deliberately usable by BASE."""

    def __init__(self):
        self.events = []
        self.manifests = []
        self.runs = []
        self.freed = []
        self.next_handle = 0
        self.states = {}
        self.mode_status = 0
        self.run_error = b""
        functions = {
            "pf_abi_version": lambda: 4,
            "pf_report_output_version_v1": lambda: 1,
            "strategy_create": self.create,
            "strategy_free": self.free,
            "strategy_set_input": self.set_input,
            "strategy_set_report_output_mode_v1": self.set_mode,
            "strategy_set_hpo_scratch_manifest_v1": self.install,
            "strategy_hpo_scratch_manifest_v1": self.manifest_receipt,
            "strategy_report_summary_v1": self.summary,
            "strategy_run_hpo_v1": self.run_summary,
            "strategy_get_last_error": lambda handle: self.run_error,
            "strategy_get_last_error_code": lambda handle: b"strategy_runtime_error",
            "strategy_last_run_status": lambda handle: 1 if self.run_error else 0,
            "run_backtest": self.run,
            "report_free": lambda out: self.events.append(("report_free",)),
        }
        for name, function in functions.items():
            setattr(self, name, Function(function))

    def create(self, unused):
        self.next_handle += 1
        self.states[self.next_handle] = {}
        self.events.append(("create", self.next_handle))
        return self.next_handle

    def free(self, handle):
        self.freed.append(handle)
        self.events.append(("free", handle))

    def set_input(self, handle, name, value):
        self.states[handle][name] = value
        self.events.append(("input", handle))

    def set_mode(self, handle, mode):
        self.events.append(("mode", handle, mode))
        return self.mode_status

    def install(self, handle, manifest):
        self.manifests.append(bytes(manifest._obj))
        self.states[handle]["manifest"] = bytes(manifest._obj)
        self.events.append(("manifest", handle))
        return 0

    def manifest_receipt(self, handle, out):
        ctypes.memmove(out, self.states[handle]["manifest"], ctypes.sizeof(out._obj))
        return 0

    def run(self, handle, bars, count, out):
        # Deliberately consume every row; neither count alone nor the last row
        # is a substitute for observing what the host actually passed.
        witness = 0
        for index in range(count):
            witness += bars[index].timestamp + int(bars[index].volume)
        self.states[handle]["witness"] = witness
        self.states[handle]["count"] = count
        self.runs.append((handle, count, out, ctypes.addressof(bars)))
        self.events.append(("run", handle))
        if out is not None:
            # A real nonempty legacy shape, not a stub that makes BASE pass.
            report = out._obj
            curve = (hpo.pf_equity_point_t * count)()
            self.states[handle]["curve"] = curve
            report.equity_curve = curve
            report.equity_curve_len = count
            report.net_profit = float(witness % 101)
            report.total_trades = count // 4

    def run_summary(self, handle, bars, count):
        self.run(handle, bars, count, None)
        return 1 if self.run_error else 0

    def summary(self, handle, out):
        result = out._obj
        result.output_mode = 1
        result.complete = 1
        result.total_trades = self.states[handle]["count"] // 4
        result.equity_point_count = self.states[handle]["count"]
        result.input_bars_processed = self.states[handle]["count"]
        result.script_bars_processed = self.states[handle]["count"]
        result.net_profit = float(self.states[handle]["witness"] % 101)
        result.metrics.all.num_trades = result.total_trades
        # A payload the host must copy without recomputing it.
        result.metrics.equity.open_pl = -0.0
        self.events.append(("summary", handle))
        return 0


def write_csv(path, count):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("timestamp", "open", "high", "low", "close", "volume"))
        for index in range(count):
            writer.writerow((1704067200000 + index * 60000, 100 + index % 7,
                             103 + index % 7, 98 + index % 7,
                             101 + index % 7, index + 1))


class HpoHostTests(unittest.TestCase):
    def run_main(self, count, library=None, bar_type=None):
        library = library or Library()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module_path = root / "benchmarks/throughput/grid_search_repro.py"
            strategy = root / "benchmarks/assets/strategies" / hpo.SLUG / "strategy.so"
            strategy.parent.mkdir(parents=True)
            strategy.touch()
            write_csv(root / "benchmarks/assets/data/ETHUSDT_15.csv", count)
            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(hpo, "__file__", str(module_path)))
                stack.enter_context(mock.patch.object(hpo.ctypes, "CDLL", return_value=library))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                if bar_type is not None:
                    stack.enter_context(mock.patch.object(hpo, "pf_bar_t", bar_type))
                hpo.main()
        return library

    def test_main_requests_summary_for_every_trial_and_never_allocates_a_report(self):
        library = self.run_main(513)
        self.assertEqual(len(library.runs), 27)
        self.assertEqual(len({address for _, _, _, address in library.runs}), 1)
        self.assertTrue(all(out is None for _, _, out, _ in library.runs),
                        "HPO still supplies a full report per trial")
        for handle, count, _, _ in library.runs:
            self.assertEqual(count, 513)
            mode = library.events.index(("mode", handle, 1))
            manifest = library.events.index(("manifest", handle))
            run = library.events.index(("run", handle))
            summary = library.events.index(("summary", handle))
            self.assertLess(mode, run)
            self.assertLess(manifest, run)
            self.assertLess(run, summary)
        self.assertEqual(library.freed, list(range(1, 28)))
        self.assertNotIn(("report_free",), library.events)

    def test_installed_manifest_is_identical_at_two_actual_input_counts(self):
        small = self.run_main(31)
        large = self.run_main(513)
        self.assertEqual(len(small.manifests), 27, "no manifest installed on BASE")
        self.assertEqual(len(large.manifests), 27)
        self.assertEqual(small.manifests, large.manifests,
                         "installed scratch bytes still depend on bars")
        self.assertEqual({count for _, count, _, _ in small.runs}, {31})
        self.assertEqual({count for _, count, _, _ in large.runs}, {513})

    def test_csv_loader_does_not_construct_a_list_of_ctypes_rows(self):
        class TrackedBar(ctypes.Structure):
            _fields_ = hpo.pf_bar_t._fields_
            constructions = 0

            def __init__(self, *args, **kwargs):
                type(self).constructions += 1
                super().__init__(*args, **kwargs)

        library = self.run_main(513, bar_type=TrackedBar)
        self.assertEqual(len(library.runs), 27)
        self.assertEqual(TrackedBar.constructions, 0,
                         "standalone ctypes rows are built before the input array")

    def test_failed_mode_installation_frees_handle_and_does_not_run(self):
        library = Library()
        library.mode_status = -1
        with self.assertRaises(RuntimeError):
            self.run_main(31, library)
        self.assertEqual(library.runs, [])
        self.assertEqual(library.freed, [1])

    def test_failed_execution_cannot_enter_the_ranking(self):
        library = Library()
        library.run_error = b"fixture execution failed"
        with self.assertRaisesRegex(RuntimeError, "fixture execution failed"):
            self.run_main(31, library)
        self.assertEqual(library.freed, [1])
        self.assertFalse(any(event[0] == "summary" for event in library.events))

    def test_trial_keeps_logical_counts_and_exact_metric_bytes(self):
        library = Library()
        runtime = hpo.HpoRuntime(library)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bars.csv"
            write_csv(path, 513)
            bars = hpo.load_csv_bars(path)
            result = runtime.trial(bars, ((b"Fast EMA", 5),))
        expected = hpo.pf_metrics_t()
        expected.all.num_trades = 128
        expected.equity.open_pl = -0.0
        self.assertEqual(result.total_trades, 128)
        self.assertEqual(result.equity_point_count, 513)
        self.assertEqual(result.input_bars_processed, 513)
        self.assertEqual(result.script_bars_processed, 513)
        self.assertEqual(result.metrics, bytes(expected))
        self.assertEqual(library.freed, [1])

    def test_manifest_derivation_uses_the_fixed_spec_and_checks_arithmetic(self):
        spec = hpo.HpoScratchSpec(page_bytes=8192, root_pages=3, entry_pages=5,
                                  cohort_pages=7, transaction_pages=11)
        value = hpo.derive_hpo_scratch_manifest(spec)
        self.assertEqual((value.root_bytes, value.entry_bytes, value.cohort_bytes,
                          value.transaction_bytes, value.resident_bytes),
                         (24576, 40960, 57344, 90112, 212992))
        with self.assertRaises(ValueError):
            hpo.derive_hpo_scratch_manifest(hpo.HpoScratchSpec(root_pages=0))
        with self.assertRaises(OverflowError):
            hpo.derive_hpo_scratch_manifest(hpo.HpoScratchSpec(root_pages=1 << 64))

    def test_csv_values_are_preserved_in_the_only_input_array(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bars.csv"
            write_csv(path, 513)
            bars = hpo.load_csv_bars(path)
            self.assertEqual(len(bars), 513)
            for index in (0, 256, 512):
                self.assertEqual(tuple(getattr(bars[index], field)
                                       for field, _ in hpo.pf_bar_t._fields_),
                                 (100 + index % 7, 103 + index % 7, 98 + index % 7,
                                  101 + index % 7, index + 1,
                                  1704067200000 + index * 60000))

    def test_bar_sized_installed_window_mutant_is_rejected(self):
        class BarSizedWindows(Library):
            def manifest_receipt(self, handle, out):
                status = super().manifest_receipt(handle, out)
                if "count" in self.states[handle]:
                    # C07's former 16*R with R=2*B, deliberately injected as
                    # an installed-receipt mutant after execution starts.
                    previous = out._obj.root_bytes
                    out._obj.root_bytes = 32 * self.states[handle]["count"]
                    out._obj.resident_bytes += out._obj.root_bytes - previous
                return status

        library = BarSizedWindows()
        with self.assertRaisesRegex(RuntimeError, "installed resident windows differ"):
            self.run_main(513, library)
        self.assertEqual(len(library.runs), 1)
        self.assertEqual(library.freed, [1])
        self.assertFalse(any(event[0] == "summary" for event in library.events))


if __name__ == "__main__":
    unittest.main()
