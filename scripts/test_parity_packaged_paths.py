#!/usr/bin/env python3
"""Exercise the installed parity closure in isolated child interpreters.

The children receive only the flat installation directory on sys.path, run
outside it, and record actual filesystem accesses and module origins. No
native strategy library, corpus checkout, compiler, or Docker daemon is used.
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
BUNDLE = ROOT / "scripts" / "parity-bundle.json"
ENV_NAMES = {
    "corpus_root": "PINEFORGE_PARITY_CORPUS_ROOT",
    "derived_workspace": "PINEFORGE_PARITY_DERIVED_WORKSPACE",
    "harness_dir": "PINEFORGE_PARITY_HARNESS_DIR",
}
HARNESS_FILES = (
    "run_json.py", "pf_bars.py", "pf_sink.py", "pineforge_sink_finalize.py",
    "pineforge_sink_to_json.py", "replay_rq_support.py",
    "request_feed_inventory.py", "bind_compiled_inventory.py",
    "selected_window_plan.py", "selected_window_report.py",
)

# -I ignores both PYTHONPATH and the script/cwd import shortcuts. Add only the
# selected installation, then run the real CLI with its real argument parser.
# The observation comes from the child, independently of the scripts' output.
CHILD = r"""
import importlib
import json
from pathlib import Path
import runpy
import sys

config = json.loads(sys.argv[1])
script = Path(config["script"]).resolve()
sys.dont_write_bytecode = True
sys.path.insert(0, str(script.parent))
observed = {"opened": [], "listed": [], "statted": []}

def audit(event, args):
    if event == "open" and isinstance(args[0], str):
        observed["opened"].append(str(Path(args[0]).absolute()))
    elif event in ("os.listdir", "os.scandir") and isinstance(args[0], str):
        observed["listed"].append(str(Path(args[0]).absolute()))

sys.addaudithook(audit)
original_stat = Path.stat

def stat(path, *args, **kwargs):
    observed["statted"].append(str(path.absolute()))
    if str(path.absolute()) == config.get("stop_at"):
        raise FileNotFoundError("stop before accessing the checkout feed")
    return original_stat(path, *args, **kwargs)

Path.stat = stat
sys.argv = [str(script), *config.get("args", [])]
try:
    probe = config.get("probe")
    if probe == "sink":
        runner = importlib.import_module("run_strategy")
        kwargs = config.get("parameters", {})
        try:
            runner._new_harness_report_sink(object(), None, **kwargs)
        except RuntimeError as error:
            # No native ABI is provided. Reaching this check proves the real
            # harness was imported; a missing harness must fail before it.
            if "requires the report sink ABI" not in str(error):
                raise
            observed["sink_abi_check"] = str(error)
        else:
            raise AssertionError("the ABI-less object was accepted")
        importlib.import_module("pf_sink")
    elif probe == "resolver":
        paths = importlib.import_module("parity_paths").resolve_paths(
            **config.get("parameters", {}))
        observed["roots"] = {
            name: str(getattr(paths, name))
            for name in ("corpus_root", "derived_workspace", "harness_dir")
        }
    elif probe == "adapter":
        adapter = importlib.import_module("run_parity_jailed")
        result = adapter.run_cases(
            [],
            script.parent / "unpublished",
            launcher=None,
            arena_bytes=1,
        )
        observed["failure"] = result.failure_text
        observed["csv_paths"] = result.csv_paths
        observed["output_exists"] = (script.parent / "unpublished").exists()
    else:
        runpy.run_path(str(script), run_name="__main__")
finally:
    observed["sys_path"] = sys.path[:]
    observed["cwd"] = str(Path.cwd())
    observed["modules"] = {
        name: str(Path(module.__file__).resolve())
        for name, module in list(sys.modules.items())
        if getattr(module, "__file__", None)
    }
    Path(config["observation"]).write_text(json.dumps(observed), encoding="utf-8")
"""


class BundleDeclarationTests(unittest.TestCase):
    def test_source_declaration_matches_fixture_and_repository(self) -> None:
        actual = (ROOT / "scripts" / "parity-bundle.json").read_bytes()
        self.assertEqual(
            hashlib.sha256(actual).hexdigest(),
            "7b5d35dd87a45dfa46cb66abb536efd905e18e755f7cc76c2618c20668703566",
        )
        declaration = json.loads(actual)
        expected_files = [
            ("scripts/derive_corpus_feeds.py", "derive_corpus_feeds.py"),
            ("scripts/parity_paths.py", "parity_paths.py"),
            ("scripts/pf_release_run.py", "pf_release_run.py"),
            ("scripts/run_parity_jailed.py", "run_parity_jailed.py"),
            ("scripts/run_strategy.py", "run_strategy.py"),
            ("scripts/verify_corpus.py", "verify_corpus.py"),
        ]
        self.assertEqual(
            [(row["source"], row["destination"]) for row in declaration["files"]],
            expected_files,
        )
        self.assertEqual(
            actual,
            (json.dumps(declaration, sort_keys=True, ensure_ascii=False,
                        separators=(",", ":")) + "\n").encode("utf-8"),
        )
        destinations = []
        for row in declaration["files"]:
            with self.subTest(source=row["source"]):
                source = ROOT / row["source"]
                self.assertTrue(source.is_file(), row["source"])
                self.assertTrue(source.resolve().is_relative_to(ROOT))
                self.assertRegex(row["destination"], r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
                self.assertNotIn(row["destination"], {".", ".."})
                self.assertEqual(Path(row["destination"]).name, row["destination"])
                destinations.append(row["destination"])
        self.assertEqual(len(destinations), len(set(destinations)))
        self.assertEqual(len(destinations), len(expected_files))


class PackagedPathsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="parity-packaged-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.base.chmod(0o755)
        cls.installed = cls.base / "opt" / "pineforge" / "parity"
        cls.harness = cls.base / "opt" / "pineforge" / "bin"
        cls.cwd = cls.base / "unrelated"
        cls.observations = cls.base / "observations"
        for directory in (cls.installed, cls.harness, cls.cwd, cls.observations):
            directory.mkdir(parents=True)
        cls.observations.chmod(0o777)
        for row in json.loads(BUNDLE.read_bytes())["files"]:
            source = ROOT / row["source"]
            if not source.is_file():
                raise AssertionError(f"packaged source is missing: {row['source']}")
            shutil.copyfile(source, cls.installed / row["destination"])
        (cls.installed / "PARITY-MANIFEST.json").write_text(
            "{}\n", encoding="ascii")
        for name in HARNESS_FILES:
            shutil.copyfile(ROOT / "docker" / name, cls.harness / name)
        shutil.copyfile(BUNDLE, cls.installed / "parity-bundle.json")
        cls.corpus = cls.base / "mounted-corpus"
        cls.case = cls.corpus / "basic" / "one-trade"
        cls.case.mkdir(parents=True)
        (cls.case / "strategy.pine").write_text(
            '//@version=6\nstrategy("packaged paths")\n', encoding="utf-8")
        (cls.case / "inputs.json").write_text(
            json.dumps({"tv_trades_csv_tz": "utc"}), encoding="utf-8")
        tape = (
            "Trade #,Type,Date and time,Price,Qty,Net PnL\n"
            "1,Entry long,2025-04-21 00:00,100,1,2\n"
            "1,Exit long,2025-04-21 00:15,102,1,2\n"
        )
        for name in ("tv_trades.csv", "engine_trades.csv"):
            (cls.case / name).write_text(tape, encoding="utf-8")
        data = cls.corpus / "data"
        data.mkdir()
        cls.source = data / "ohlcv_ETH-USDT-USDT_1m.csv"
        # Keep the production 1 MB source-size guard. Use real minute rows,
        # not a padded header, mocked stat, LFS pointer, or copied corpus.
        cls.rows = []
        start = 1745193600000
        with cls.source.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream, lineterminator="\n")
            writer.writerow(("timestamp", "open", "high", "low", "close", "volume"))
            for index in range(36_000):
                price = 100 + index % 7
                row = (start + index * 60_000, price, price + 2,
                       price - 1, price + 1, 1)
                cls.rows.append(row)
                writer.writerow(row)
        if cls.source.stat().st_size < 1_000_000:
            raise AssertionError("synthetic real CSV must exercise the size guard")
        cls.source_before = cls.source.read_bytes()
        for path in cls.corpus.rglob("*"):
            path.chmod(0o555 if path.is_dir() else 0o444)
        cls.corpus.chmod(0o555)
        cls.addClassCleanup(cls._restore_corpus_permissions)

    @classmethod
    def _restore_corpus_permissions(cls) -> None:
        cls.corpus.chmod(0o755)
        for path in cls.corpus.rglob("*"):
            path.chmod(0o755 if path.is_dir() else 0o644)

    def setUp(self) -> None:
        self.work = self.base / self._testMethodName
        self.work.mkdir()
        self.workspace = self.work / "derived"

    def child(self, name, *args, env=None, probe=None, parameters=None,
              in_place=False, stop_at=None, unprivileged=False):
        observation = self.observations / f"{self._testMethodName}-{os.urandom(6).hex()}.json"
        config = {
            "script": str((ROOT / "scripts" if in_place else self.installed) / name),
            "args": [str(arg) for arg in args],
            "probe": probe,
            "parameters": parameters or {},
            "observation": str(observation),
            "stop_at": str(stop_at) if stop_at else None,
        }
        child_env = {key: value for key, value in os.environ.items()
                     if not key.startswith("PINEFORGE_") and not key.startswith("PYTHON")}
        child_env.update(env or {})
        # Poison PYTHONPATH deliberately; -I must make it irrelevant.
        child_env["PYTHONPATH"] = str(ROOT / "scripts")
        credentials = {}
        if unprivileged and os.geteuid() == 0:
            import pwd
            nobody = pwd.getpwnam("nobody")
            credentials = {"user": nobody.pw_uid, "group": nobody.pw_gid,
                           "extra_groups": []}
        result = subprocess.run(
            [sys.executable, "-I", "-B", "-c", CHILD, json.dumps(config)],
            cwd=self.cwd, env=child_env, capture_output=True, text=True,
            timeout=60, **credentials,
        )
        self.assertTrue(observation.is_file(), result.stdout + result.stderr)
        observed = json.loads(observation.read_text(encoding="utf-8"))
        self.assertEqual(Path(observed["cwd"]), self.cwd)
        if not in_place:
            for path in observed["sys_path"]:
                self.assertFalse(Path(path).resolve().is_relative_to(ROOT), path)
            for path in observed["modules"].values():
                self.assertFalse(Path(path).is_relative_to(ROOT), path)
        return result, observed

    def path_args(self, *, corpus=None, workspace=None, harness=None):
        return (
            "--corpus-root", corpus or self.corpus,
            "--derived-workspace", workspace or self.workspace,
            "--harness-dir", harness or self.harness,
        )

    def assert_pass(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def derive(self, **kwargs):
        return self.child("derive_corpus_feeds.py", *self.path_args(**kwargs))

    def test_relocated_derivation_is_read_only_and_idempotent(self) -> None:
        result, observed = self.derive()
        self.assert_pass(result)
        self.assertIn(str(self.source), observed["opened"])
        full = self.workspace / "ohlcv_ETH-USDT-USDT_15m.csv"
        with full.open(newline="", encoding="utf-8") as stream:
            actual = list(csv.reader(stream))
        expected = []
        for offset in range(0, len(self.rows), 15):
            bucket = self.rows[offset:offset + 15]
            expected.append([
                str(bucket[0][0]), str(bucket[0][1]),
                str(max(row[2] for row in bucket)),
                str(min(row[3] for row in bucket)), str(bucket[-1][4]),
                str(sum(row[5] for row in bucket)),
            ])
        self.assertEqual(actual[1:], expected)
        before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns)
                  for path in self.workspace.iterdir()}
        result, _ = self.derive()
        self.assert_pass(result)
        after = {path.name: (path.read_bytes(), path.stat().st_mtime_ns)
                 for path in self.workspace.iterdir()}
        self.assertEqual(after, before)
        self.assertEqual(self.source.read_bytes(), self.source_before)
        self.assertFalse((self.corpus / "data" / "derived").exists())

    def test_relocated_grader_single_case_and_category(self) -> None:
        for selection in ((self.case,), ("--category", "basic"), ("--all",)):
            with self.subTest(selection=selection):
                result, observed = self.child(
                    "verify_corpus.py", *selection, *self.path_args())
                self.assert_pass(result)
                self.assertIn(str(self.case / "tv_trades.csv"), observed["opened"])
                self.assertIn(str(self.case / "engine_trades.csv"), observed["opened"])
                if selection != (self.case,):
                    self.assertIn("Verified 1 strategies", result.stdout)
                    self.assertIn("excellent=1", result.stdout)

    def test_relocated_runner_prepares_selected_feed_and_help(self) -> None:
        result, _ = self.child("run_strategy.py", "--help", *self.path_args())
        self.assert_pass(result)
        # Run the real preparation path up to its native-library boundary.
        # No generated library is supplied or simulated by this script test.
        result, observed = self.child(
            "run_strategy.py", self.case, "--no-trim-output",
            "--output", self.work / "new-engine-trades.csv", *self.path_args())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("strategy library not found", result.stdout + result.stderr)
        self.assertIn(str(self.source), observed["opened"])
        self.assertTrue((self.workspace / "ohlcv_ETH-USDT-USDT_15m.csv").is_file())
        result, _ = self.child(
            "run_strategy.py", self.case, "--no-overwrite", *self.path_args())
        self.assert_pass(result)

    def test_environment_and_explicit_path_precedence(self) -> None:
        env = {variable: str(self.work / name) for name, variable in ENV_NAMES.items()}
        parameters = {name: str(self.work / ("explicit-" + name)) for name in ENV_NAMES}
        result, observed = self.child(
            "parity_paths.py", probe="resolver", env=env)
        self.assert_pass(result)
        self.assertEqual(observed["roots"],
                         {name: env[variable] for name, variable in ENV_NAMES.items()})
        result, observed = self.child(
            "parity_paths.py", probe="resolver", env=env, parameters=parameters)
        self.assert_pass(result)
        self.assertEqual(observed["roots"], parameters)
        env = {
            ENV_NAMES["corpus_root"]: str(self.corpus),
            ENV_NAMES["derived_workspace"]: str(self.workspace),
            ENV_NAMES["harness_dir"]: str(self.harness),
        }
        result, observed = self.child("derive_corpus_feeds.py", env=env)
        self.assert_pass(result)
        self.assertIn(str(self.source), observed["opened"])
        result, observed = self.child("verify_corpus.py", "--all", env=env)
        self.assert_pass(result)
        self.assertIn(str(self.case / "tv_trades.csv"), observed["opened"])
        result, _ = self.child("run_strategy.py", "--help", env=env)
        self.assert_pass(result)
        override = self.work / "override"
        result, _ = self.child(
            "derive_corpus_feeds.py", *self.path_args(workspace=override), env=env)
        self.assert_pass(result)
        self.assertTrue((override / "ohlcv_ETH-USDT-USDT_15m.csv").is_file())

    def test_missing_corpus_root_refuses_with_positive_control(self) -> None:
        result, _ = self.child("verify_corpus.py", "--all", *self.path_args())
        self.assert_pass(result)
        missing = self.work / "absent-corpus"
        result, _ = self.child(
            "verify_corpus.py", "--all", *self.path_args(corpus=missing))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("corpus root", result.stderr.lower())
        self.assertIn(str(missing), result.stderr)
        result, _ = self.derive()
        self.assert_pass(result)
        result, _ = self.derive(corpus=missing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(missing), result.stderr)

    def test_empty_population_refuses_with_positive_control(self) -> None:
        empty = self.work / "empty-corpus"
        (empty / "basic").mkdir(parents=True)
        for selection in (("--all",), ("--category", "basic")):
            with self.subTest(selection=selection):
                result, _ = self.child(
                    "verify_corpus.py", *selection, *self.path_args())
                self.assert_pass(result)
                result, _ = self.child(
                    "verify_corpus.py", *selection, *self.path_args(corpus=empty))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("zero", result.stderr.lower())

    def test_missing_harness_refuses_with_positive_control(self) -> None:
        for harness, succeeds in ((self.harness, True),
                                  (self.work / "absent-harness", False)):
            with self.subTest(harness=harness):
                result, observed = self.child(
                    "run_strategy.py", probe="sink",
                    parameters={"harness_dir": str(harness)})
                if succeeds:
                    self.assert_pass(result)
                    self.assertIn("sink_abi_check", observed)
                    self.assertEqual(Path(observed["modules"]["run_json"]),
                                     harness / "run_json.py")
                    self.assertEqual(Path(observed["modules"]["pf_sink"]),
                                     harness / "pf_sink.py")
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("harness", result.stderr.lower())
                    self.assertIn(str(harness), result.stderr)

    def test_unwritable_workspace_refuses_with_positive_control(self) -> None:
        self.work.chmod(0o755)
        self.workspace.mkdir()
        self.workspace.chmod(0o555)
        try:
            result, _ = self.child(
                "derive_corpus_feeds.py", *self.path_args(), unprivileged=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Permission denied", result.stderr)
            self.assertEqual(list(self.workspace.iterdir()), [])
            self.workspace.chmod(0o777)
            result, observed = self.child(
                "derive_corpus_feeds.py", *self.path_args(), unprivileged=True)
            self.assert_pass(result)
            self.assertIn(str(self.source), observed["opened"])
            self.assertTrue((self.workspace / "ohlcv_ETH-USDT-USDT_15m.csv").is_file())
        finally:
            self.workspace.chmod(0o777)

    def test_lfs_pointer_refuses_with_positive_control(self) -> None:
        result, _ = self.derive()
        self.assert_pass(result)
        pointer_corpus = self.work / "pointer-corpus"
        (pointer_corpus / "data").mkdir(parents=True)
        pointer = pointer_corpus / "data" / self.source.name
        pointer.write_text(
            "version https://git-lfs.github.com/spec/v1\n"
            + "oid sha256:" + hashlib.sha256(self.source_before).hexdigest()
            + f"\nsize {len(self.source_before)}\n", encoding="utf-8")
        result, _ = self.derive(corpus=pointer_corpus)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Git LFS pointer", result.stderr)

    def test_checkout_defaults_observe_repository_paths(self) -> None:
        source = ROOT / "corpus" / "data" / self.source.name
        result, observed = self.child(
            "derive_corpus_feeds.py", in_place=True, stop_at=source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(source), observed["statted"])
        result, observed = self.child(
            "verify_corpus.py", "--all", in_place=True, stop_at=ROOT / "corpus")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(ROOT / "corpus"), observed["statted"])
        result, observed = self.child("run_strategy.py", probe="sink", in_place=True)
        self.assert_pass(result)
        self.assertEqual(Path(observed["modules"]["run_json"]),
                         ROOT / "docker" / "run_json.py")
        result, _ = self.child("run_strategy.py", "--help", in_place=True)
        self.assert_pass(result)


if __name__ == "__main__":
    unittest.main(verbosity=2)
