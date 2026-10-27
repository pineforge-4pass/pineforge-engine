#!/usr/bin/env python3
"""Host handoff and failure-wire rows; engine timing is tested in C++.

RED-FIRST: the frozen host lacks _install_run_budgets and
_configure_sink_stall. No native library or generated catalog is mocked into
an engine timing pass here.
"""
import ctypes
import importlib.util
import json
import os
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("run_json_g1", ROOT / "docker/run_json.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Install:
    def __init__(self, status=0):
        self.calls = []
        self.status = status

    def __call__(self, wall, cpu):
        self.calls.append((wall, cpu))
        return self.status


def options(wall=None, cpu=None, stall=None):
    return SimpleNamespace(run_wall_budget_ms=wall, run_cpu_budget_ms=cpu,
                           report_sink_stall_ms=stall)


class BudgetRows(unittest.TestCase):
    def test_typed_handoff_and_explicit_disable(self):
        function = Install()
        library = SimpleNamespace(pf_run_budget_install_v1=function)
        runner._install_run_budgets(library, options(wall=50, cpu=10))
        runner._install_run_budgets(library, options(wall=0, cpu=0))
        runner._install_run_budgets(library, options(cpu=600000))
        self.assertEqual(function.calls, [(50, 10), (0, 0), (0, 600000)])
        self.assertEqual(function.argtypes, [ctypes.c_uint64, ctypes.c_uint64])
        self.assertIs(function.restype, ctypes.c_int)

    def test_old_library_only_allowed_without_explicit_budget(self):
        runner._install_run_budgets(SimpleNamespace(), options())
        with self.assertRaises(runner.StrategyLibraryError) as caught:
            runner._install_run_budgets(SimpleNamespace(), options(wall=1))
        self.assertEqual(caught.exception.code, "strategy_library_incompatible")
        self.assertEqual(caught.exception.code_args, {"reason": "symbol_missing"})

    def test_install_failure_is_forwarded(self):
        with self.assertRaises(runner.RunFailure) as caught:
            runner._install_run_budgets(
                SimpleNamespace(pf_run_budget_install_v1=Install(-1)), options(cpu=1))
        self.assertEqual(caught.exception.code, "engine_invariant")

    def test_cli_limits_refuse_before_uint64_conversion(self):
        for args in (options(wall=-1), options(cpu=2**63), options(stall=-1),
                     options(stall=2**64)):
            with self.subTest(args=args), self.assertRaises(runner.RunFailure) as caught:
                runner._validate_budget_options(args)
            self.assertEqual(caught.exception.code, "run_request_invalid")
        runner._validate_budget_options(options(wall=0, cpu=2**63 - 1, stall=2**64 - 1))

    def test_real_cli_parses_and_refuses_negative_budget_before_io(self):
        with ExitStack() as cleanup, patch.object(
                runner, "load_bars", side_effect=AssertionError("must refuse before chart IO")):
            with self.assertRaises(runner.RunFailure) as caught:
                runner._main(["--so", "unloaded.so", "--ohlcv", "unread.csv",
                              "--run-wall-budget-ms", "-1",
                              "--run-cpu-budget-ms", "10",
                              "--report-sink-stall-ms", "50"], cleanup)
        self.assertEqual(caught.exception.code, "run_request_invalid")
        self.assertEqual(caught.exception.code_args, {"option": "arguments"})

    def test_stall_default_and_explicit_zero_environment(self):
        with patch.dict(os.environ, {}, clear=True):
            with ExitStack() as cleanup:
                runner._configure_sink_stall(options(), cleanup)
                self.assertNotIn("PF_REPORT_SINK_STALL_MS", os.environ)
            with ExitStack() as cleanup:
                runner._configure_sink_stall(options(stall=0), cleanup)
                self.assertEqual(os.environ["PF_REPORT_SINK_STALL_MS"], "0")
            self.assertNotIn("PF_REPORT_SINK_STALL_MS", os.environ)
            os.environ["PF_REPORT_SINK_STALL_MS"] = "71"
            with ExitStack() as cleanup:
                runner._configure_sink_stall(options(stall=50), cleanup)
                self.assertEqual(os.environ["PF_REPORT_SINK_STALL_MS"], "50")
            self.assertEqual(os.environ["PF_REPORT_SINK_STALL_MS"], "71")

    def test_existing_engine_failure_forwarder_preserves_new_outcomes(self):
        # Handwritten witnesses: the budget kind/limit/used are engine-owned
        # integer milliseconds; sink_stalled has only its closed reason.
        for code, args in (
                ("run_budget_exceeded", {"kind": "wall", "limit": 50, "used": 100}),
                ("run_budget_exceeded", {"kind": "cpu", "limit": 10, "used": 20}),
                ("report_sink_failed", {"reason": "sink_stalled"})):
            with self.subTest(code=code, args=args):
                library = SimpleNamespace(
                    strategy_get_last_error=lambda state: b"terminal failure",
                    strategy_get_last_error_code=lambda state: code.encode(),
                    strategy_get_last_error_args=lambda state: json.dumps(args).encode(),
                    strategy_last_run_status=lambda state: 1)
                failure = runner.run_failure(library, object())
                wire = json.loads(runner.failure_line(*failure))
                self.assertEqual(wire["code"], code)
                self.assertEqual(wire["args"], args)
                if code == "run_budget_exceeded":
                    self.assertIs(type(wire["args"]["used"]), int)


if __name__ == "__main__":
    unittest.main()
