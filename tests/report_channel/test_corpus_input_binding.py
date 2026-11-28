"""Ordinary unit checks for corpus metadata/input binding; no native runs."""
from __future__ import annotations

import ctypes
import contextlib
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import run_strategy as binding
import pf_release_run as release

if __package__:
    from .test_off_identity import load_module_bytes, load_pinned_module
else:
    from test_off_identity import load_module_bytes, load_pinned_module


def receipt_library(rows):
    content = json.dumps({"inputs": rows}).encode() + b"\0"

    def get_settings(_state, output, capacity, required, _error, _error_capacity):
        ctypes.cast(required, ctypes.POINTER(ctypes.c_size_t))[0] = len(content)
        if output is None and capacity == 0:
            return 4  # PF_SETTINGS_BUFFER_TOO_SMALL, pineforge.h
        if capacity < len(content):
            return 4
        ctypes.memmove(output, content, len(content))
        return 0  # PF_SETTINGS_OK

    return SimpleNamespace(
        strategy_settings_api_version=mock.Mock(return_value=1),
        strategy_get_effective_settings=mock.Mock(side_effect=get_settings),
    )


class CorpusInputBinding(unittest.TestCase):
    def jailed_request(self):
        """Prepare ordinary file fixtures; the placeholder module is never loaded."""
        import run_parity_jailed as jailed

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        case = root / "case"
        case.mkdir()
        (case / "generated.cpp").write_text("// request-builder fixture\n")
        (case / "strategy.so").write_bytes(b"request-builder fixture, never loaded\n")
        # _prepare does not read Pine declarations from this ordinary file fixture.
        (case / "strategy.pine").write_text(
            '//@version=6\nstrategy("request")\n')
        feed = case / "chart.csv"
        feed.write_text(
            "timestamp,open,high,low,close,volume\n"
            "1700000000000,10,12,9,11,1\n1700000060000,11,13,10,12,1\n")
        params = {
            "ohlcv_csv": str(feed), "notes": "literal or unknown value",
            "expected_tier": "literal or unknown tier", "_comment": "literal or unknown comment",
        }
        (case / "inputs.json").write_text(json.dumps(params))
        metadata = jailed._case_metadata(case)
        prepared_dir = root / "prepared"
        prepared_dir.mkdir()
        with mock.patch.object(binding.ctypes, "CDLL", side_effect=AssertionError("no native load")):
            prepared = jailed._prepare(
                metadata, prepared_dir, jailed._harness_paths(harness_dir=ROOT / "docker"))
        return prepared

    def test_jailed_preparation_forwards_raw_metadata_spelled_keys(self):
        """Real jailed preparation forwards raw keys without declaration lookup."""
        prepared = self.jailed_request()
        self.assertEqual(prepared.inputs["notes"], "literal or unknown value")
        self.assertEqual(
            json.loads(prepared.argv[prepared.argv.index("--inputs") + 1]),
            {key: value for key, value in prepared.inputs.items()})
        self.assertEqual(set(prepared.inputs), {"notes", "expected_tier", "_comment"})

    def test_jailed_checked_setter_call_path_with_ordinary_fixture(self):
        """Call real apply_settings with an ordinary rejecting setter double."""
        prepared = self.jailed_request()
        self.assertEqual(set(prepared.inputs), {"notes", "expected_tier", "_comment"})
        # Use the selected harness through the existing resolver, as the adapter does.
        import run_parity_jailed as jailed
        path = jailed._harness_paths(harness_dir=ROOT / "docker").run_json
        previous = sys.path[:]
        try:
            sys.path.insert(0, str(path.parent))
            runner = load_module_bytes("_binding_jailed_setter", path, path.read_bytes())
        finally:
            sys.path[:] = previous
        self.addCleanup(sys.modules.pop, "_binding_jailed_setter", None)

        def reject(_state, key, _value, error, _capacity):
            self.assertEqual(key, b"notes")
            error.value = b"unknown input key"
            return 1

        library = SimpleNamespace(
            strategy_set_input_checked=mock.Mock(side_effect=reject),
            strategy_set_override_checked=mock.Mock(),
        )
        with self.assertRaises(runner.RunFailure) as raised:
            runner.apply_settings(library, 17, prepared.inputs, {}, True)
        self.assertEqual(raised.exception.code, "setting_rejected")
        self.assertEqual(raised.exception.code_args, {
            "entrypoint": "strategy_set_input", "reason": "unknown_key"})
        library.strategy_set_input_checked.assert_called_once()
        library.strategy_set_override_checked.assert_not_called()

    def test_artifact_metadata_twins_preserve_unrecognized_settings(self):
        plain = {"Length": 8, "undeclared_input": "reject me", "_private": "keep"}
        annotated = {
            **plain, "expected_tier": "anomaly", "notes": "corpus annotation",
            "_comment": "feed annotation", "ohlcv_csv": "feed.csv",
            "tv_trades_csv_tz": "utc",
        }
        expected = {"Length": "8", "undeclared_input": "reject me", "_private": "keep"}
        self.assertEqual(binding.corpus_input_values(plain), expected)
        self.assertEqual(
            binding.corpus_input_values(annotated, declared_input_names={"Length"}),
            expected,
        )
        self.assertEqual(plain["Length"], 8)
        self.assertIn("notes", annotated)

    def test_literal_receipt_names_win_over_metadata_spelling(self):
        keys = ("expected_tier", "notes", "_comment", "input_tf", "tv_user_input")
        library = receipt_library([{"name": key} for key in (*keys, "Literal Title")])
        names = binding.declared_pine_input_names(library, 17)
        params = {key: f"value for {key}" for key in keys}
        params.update({"Literal Title": 5, "pine_variable_alias": 9, "Notes": "case matters"})
        self.assertEqual(
            binding.corpus_input_values(params, declared_input_names=names),
            {key: str(value) for key, value in params.items()},
        )
        self.assertNotIn("pine_variable_alias", names)
        self.assertEqual(library.strategy_get_effective_settings.call_count, 2)

    def test_missing_binding_refuses_to_discard_metadata_spelled_input(self):
        with self.assertRaisesRegex(RuntimeError, "NEEDS.*declared Pine input"):
            binding.corpus_input_values({"notes": "possibly a real input"})
        with self.assertRaisesRegex(RuntimeError, "NEEDS.*strategy_get_effective_settings"):
            binding.declared_pine_input_names(SimpleNamespace(), 17)

    def test_invalid_receipt_is_not_an_empty_descriptor_table(self):
        for rows in (None, [{"title": "notes"}], [{"name": 3}]):
            with self.subTest(rows=rows):
                with self.assertRaisesRegex(RuntimeError, "NEEDS.*input"):
                    binding.declared_pine_input_names(receipt_library(rows), 17)
        library = receipt_library([])
        library.strategy_get_effective_settings.return_value = 3
        library.strategy_get_effective_settings.side_effect = None
        with self.assertRaisesRegex(RuntimeError, "NEEDS.*input"):
            binding.declared_pine_input_names(library, 17)

    def test_receipt_buffer_protocol_never_accepts_partial_or_unterminated_json(self):
        content = b'{"inputs":[]}\0'
        for defect in ("size-status", "zero-size", "read-status", "grew", "short", "no-nul"):
            with self.subTest(defect=defect):
                calls = []

                def getter(_state, output, capacity, required, _error, _error_capacity):
                    calls.append((output is None, capacity))
                    length = ctypes.cast(required, ctypes.POINTER(ctypes.c_size_t))
                    if output is None:
                        length[0] = 0 if defect == "zero-size" else len(content)
                        return 0 if defect == "size-status" else 4
                    length[0] = (len(content) + 1 if defect == "grew" else
                                 len(content) - 1 if defect == "short" else len(content))
                    payload = content[:-1] + b"X" if defect == "no-nul" else content
                    ctypes.memmove(output, payload, len(payload))
                    return 4 if defect == "read-status" else 0

                library = SimpleNamespace(
                    strategy_settings_api_version=mock.Mock(return_value=1),
                    strategy_get_effective_settings=mock.Mock(side_effect=getter))
                with self.assertRaisesRegex(RuntimeError, "NEEDS.*input"):
                    binding.declared_pine_input_names(library, 17)
                self.assertEqual(calls, [(True, 0)] if defect in ("size-status", "zero-size")
                                 else [(True, 0), (False, len(content))])

    def test_runner_binds_its_module_and_releases_the_receipt_handle(self):
        library = receipt_library([{"name": "notes"}])
        library.strategy_create = mock.Mock(return_value=17)
        library.strategy_free = mock.Mock()
        strategy = binding.Strategy.__new__(binding.Strategy)
        strategy.lib = library
        params = {"notes": "Pine value", "_comment": "artifact", "unknown": 8}
        self.assertEqual(strategy.corpus_inputs(params), {
            "notes": "Pine value", "unknown": "8"})
        library.strategy_create.assert_called_once_with(b"{}")
        library.strategy_free.assert_called_once_with(17)
        library.strategy_get_effective_settings.side_effect = None
        library.strategy_get_effective_settings.return_value = 3
        with self.assertRaisesRegex(RuntimeError, "NEEDS.*input"):
            strategy.corpus_inputs(params)
        self.assertEqual(library.strategy_free.call_count, 2)

    def test_pinned_helper_guard_covers_bytes_executed(self):
        original = b"identity = 'immutable reference helper'\n"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "helper.py"
            path.write_bytes(original)
            loaded = load_pinned_module("_corpus_binding_original", path, original)
            self.addCleanup(sys.modules.pop, "_corpus_binding_original", None)
            self.assertEqual(loaded.identity, "immutable reference helper")
            self.assertEqual(Path(loaded.__file__).resolve(), path.resolve())
            path.write_bytes(b"raise AssertionError('changed bytes were executed')\n")
            with self.assertRaisesRegex(AssertionError, "BASE69 helper bytes differ"):
                load_pinned_module("_corpus_binding_changed", path, original)
            self.assertNotIn("_corpus_binding_changed", sys.modules)

    def test_docker_adapter_defers_metadata_but_keeps_ordinary_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "generated.cpp").write_text("// source\n")
            feed = directory / "bars.csv"
            feed.write_text("timestamp,open,high,low,close,volume\n")
            for params in ({"Length": 8}, {"Length": 8, "notes": "possibly Pine"}):
                with self.subTest(params=params):
                    with mock.patch.object(release, "run_release", side_effect=LookupError) as run:
                        with self.assertRaises(LookupError):
                            binding._run_via_docker(directory, feed, params, {}, None, "selected")
                    sent = run.call_args.kwargs
                    if "notes" in params:
                        self.assertEqual(sent["corpus_params"], params)
                        self.assertNotIn("inputs", sent)
                    else:
                        self.assertEqual(sent["inputs"], {"Length": "8"})
                        self.assertNotIn("corpus_params", sent)

    def test_selected_module_receipt_handle_is_freed_without_running_bars(self):
        library = receipt_library([{"name": "notes"}, {"name": "Literal Title"}])

        def create(_params, out, _error, _capacity):
            ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0] = 17
            return 0

        library.strategy_create_checked = mock.Mock(side_effect=create)
        library.strategy_free = mock.Mock()
        library.strategy_set_input_checked = mock.Mock(side_effect=AssertionError("no setters"))
        library.strategy_set_override_checked = mock.Mock(side_effect=AssertionError("no setters"))
        library.run_backtest_full = mock.Mock(side_effect=AssertionError("no bars"))
        with mock.patch.object(release.ctypes, "CDLL", return_value=library) as load:
            self.assertEqual(release._module_input_names(Path("/selected/module.so")),
                             frozenset({"notes", "Literal Title"}))
            load.assert_called_once_with("/selected/module.so")
            self.assertIsNone(library.strategy_create_checked.call_args.args[0])
            library.strategy_free.assert_called_once_with(17)
            library.strategy_set_input_checked.assert_not_called()
            library.strategy_set_override_checked.assert_not_called()
            library.run_backtest_full.assert_not_called()
            library.strategy_get_effective_settings.side_effect = None
            library.strategy_get_effective_settings.return_value = 3
            with self.assertRaisesRegex(RuntimeError, "NEEDS.*input"):
                release._module_input_names(Path("/selected/module.so"))
            self.assertEqual(library.strategy_free.call_count, 2)
            library.strategy_settings_api_version.return_value = 2
            with self.assertRaisesRegex(release.ReleaseRunError, "NEEDS.*settings API v1"):
                release._module_input_names(Path("/selected/module.so"))
            self.assertEqual(library.strategy_create_checked.call_count, 2)
            library.strategy_set_input_checked = None
            with self.assertRaisesRegex(release.ReleaseRunError, "strategy_set_input_checked"):
                release._module_input_names(Path("/selected/module.so"))
            self.assertEqual(library.strategy_create_checked.call_count, 2)

    def test_checked_creation_error_releases_any_returned_handle(self):
        library = receipt_library([])

        def refuse(_params, out, error, _capacity):
            ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0] = 17
            error.value = b"creation refused"
            return 3

        library.strategy_create_checked = mock.Mock(side_effect=refuse)
        library.strategy_free = mock.Mock()
        library.strategy_set_input_checked = mock.Mock()
        library.strategy_set_override_checked = mock.Mock()
        with mock.patch.object(release.ctypes, "CDLL", return_value=library):
            with self.assertRaisesRegex(release.ReleaseRunError, "creation refused"):
                release._module_input_names(Path("/selected/module.so"))
        library.strategy_free.assert_called_once_with(17)
        library.strategy_get_effective_settings.assert_not_called()

    def test_build_receipt_launch_order_and_metadata_twins(self):
        # Both transport and native query are doubles: this checks orchestration,
        # never Docker, a compiler, a native strategy, or Pine semantics.
        cases = (
            ({"Length": 8}, {"Length"}, {"Length": "8"}, 0),
            ({"Length": 8, "notes": "annotation", "_comment": "annotation",
              "expected_tier": "annotation"}, {"Length"}, {"Length": "8"}, 0),
            ({"Length": 8, "notes": "literal Pine", "_comment": "annotation"},
             {"Length", "notes"}, {"Length": "8", "notes": "literal Pine"}, 0),
            ({"notes": "annotation", "unrecognized": "reject"},
             set(), {"unrecognized": "reject"}, 1),
        )
        for params, names, expected, status in cases:
            with self.subTest(params=params), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                events = []
                document = {}

                def process(argv, **kwargs):
                    if not events:
                        events.append("build")
                        self.assertEqual(argv, [str(ROOT / "docker/entrypoint.sh")])
                        self.assertEqual(kwargs["env"]["PINEFORGE_BUILD_ONLY"], "1")
                        self.assertEqual(kwargs["env"]["PINEFORGE_WORK_DIR"], str(work))
                        document.update(write_build_manifest(work))
                        return SimpleNamespace(returncode=0)
                    events.append("engine")
                    self.assertEqual(events, ["build", "receipt", "engine"])
                    expected_argv = list(document["argv"])
                    expected_argv[expected_argv.index("--inputs") + 1] = json.dumps(expected)
                    self.assertEqual(argv, expected_argv)
                    self.assertEqual(kwargs["cwd"], document["cwd"])
                    self.assertEqual(kwargs["env"]["PINEFORGE_WORK_DIR"], str(work))
                    return SimpleNamespace(returncode=status)

                def query(path):
                    events.append("receipt")
                    self.assertEqual(str(path), document["artifacts"]["so"]["path"])
                    return frozenset(names)

                with mock.patch.dict(release.os.environ, {
                    "PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(b"source\n").hexdigest(),
                }, clear=True), mock.patch.object(release.subprocess, "run", side_effect=process), \
                        mock.patch.object(release, "_module_input_names", side_effect=query):
                    result = release._run_corpus_build(
                        [str(ROOT / "docker/entrypoint.sh")], params, work)
                self.assertEqual(result, 4 if status else 0)
                self.assertEqual(events, ["build", "receipt", "engine"])

    def test_changed_build_artifact_stops_before_query_or_engine(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)

            def build(_argv, **_kwargs):
                document = write_build_manifest(work)
                Path(document["artifacts"]["so"]["path"]).write_bytes(b"changed module\n")
                return SimpleNamespace(returncode=0)

            with mock.patch.dict(release.os.environ, {
                "PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(b"source\n").hexdigest(),
            }, clear=True), mock.patch.object(release.subprocess, "run", side_effect=build) as run, \
                    mock.patch.object(release, "_module_input_names") as query:
                with self.assertRaisesRegex(release.ReleaseRunError, "artifact"):
                    release._run_corpus_build(
                        [str(ROOT / "docker/entrypoint.sh")], {"notes": "annotation"}, work)
                query.assert_not_called()
                self.assertEqual(run.call_count, 1)

    def test_docker_transport_keeps_native_unknown_key_failure(self):
        failure = {
            "engine": "pineforge", "error": "strategy_set_input: unknown input key",
            "code": "setting_rejected",
            "args": {"entrypoint": "strategy_set_input", "reason": "unknown_key"},
        }
        with tempfile.TemporaryDirectory() as temporary:
            source, feed = Path(temporary) / "generated.cpp", Path(temporary) / "feed.csv"
            source.write_bytes(b"source\n")
            feed.write_bytes(b"feed\n")

            image_id, container = "sha256:" + "a" * 64, "b" * 64
            config = {"Entrypoint": ["/image/own-entrypoint.sh"], "Cmd": ["image-arg"]}
            responses = [
                SimpleNamespace(returncode=0, stdout=json.dumps([{"Id": image_id, "Config": config}]),
                                stderr=""),
                SimpleNamespace(returncode=0, stdout=container + "\n", stderr=""),
                SimpleNamespace(returncode=0, stdout=b"", stderr=b""),
                SimpleNamespace(returncode=4, stdout=json.dumps(failure), stderr="failed\n"),
                SimpleNamespace(returncode=0, stdout="", stderr=""),
            ]
            with mock.patch.object(release.subprocess, "run", side_effect=responses) as run:
                with self.assertRaisesRegex(release.ReleaseRunError, "unknown input key") as raised:
                    release.run_release(
                        source, feed, image="selected-image",
                        corpus_params={"notes": "annotation", "unrecognized": "reject"})
            self.assertEqual(raised.exception.report, failure)
            self.assertEqual(run.call_count, 5)
            inspect, create, copy, start, remove = run.call_args_list
            self.assertEqual(inspect.args[0], ["docker", "image", "inspect", "selected-image"])
            argv = create.args[0]
            self.assertEqual(argv[:2], ["docker", "create"])
            self.assertIn("--entrypoint", argv)
            self.assertEqual(argv[argv.index("--entrypoint") + 1], "python3")
            self.assertEqual(argv[-4], image_id)
            helper, request = (PurePosixPath(path).relative_to("/") for path in (argv[-3], argv[-1]))
            self.assertEqual(json.loads(argv[-2]), ["/image/own-entrypoint.sh", "image-arg"])
            self.assertEqual(copy.args[0], ["docker", "cp", "-a", "-", f"{container}:/"])
            with tarfile.open(fileobj=io.BytesIO(copy.kwargs["input"]), mode="r:") as archive:
                with archive.extractfile(str(helper)) as staged_helper:
                    self.assertEqual(staged_helper.read(),
                                     (ROOT / "scripts/pf_release_run.py").read_bytes())
                self.assertNotIn(str(helper.parent / "entrypoint.sh"), archive.getnames())
                with archive.extractfile(str(request)) as staged_request:
                    self.assertEqual(json.load(staged_request), {
                        "notes": "annotation", "unrecognized": "reject"})
            self.assertEqual(start.args[0], ["docker", "start", "-a", container])
            self.assertEqual(remove.args[0], ["docker", "rm", "-f", "-v", container])

    def test_installed_entrypoint_mismatch_stops_before_build_query_or_bars(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            installed = work / "image-entrypoint.sh"
            installed.write_bytes(b"#!/bin/sh\n# older image ignores BUILD_ONLY\n")
            with mock.patch.object(release.subprocess, "run") as run, \
                    mock.patch.object(release, "_module_input_names") as query:
                with self.assertRaisesRegex(release.ReleaseRunError, "installed entrypoint bytes"):
                    release._run_corpus_build(
                        [str(installed)], {"notes": "annotation"}, work)
                run.assert_not_called()
                query.assert_not_called()
            # Even the checkout script must match the reviewed source pin.
            with mock.patch.object(release.subprocess, "run") as run:
                with self.assertRaisesRegex(release.ReleaseRunError, "reviewed build-only"):
                    with mock.patch.object(release, "_BUILD_ONLY_ENTRYPOINT_SHA256", "0" * 64):
                        release._run_corpus_build(
                            [str(ROOT / "docker/entrypoint.sh")], {}, work)
                run.assert_not_called()
            installed.write_bytes((ROOT / "docker/entrypoint.sh").read_bytes())
            self.assertEqual(hashlib.sha256(installed.read_bytes()).hexdigest(),
                             release._BUILD_ONLY_ENTRYPOINT_SHA256)
            # Installed bytes and inspect-derived arguments are sufficient;
            # there is no comparison file or host-harness prerequisite.
            release._require_installed_entrypoint([str(installed), "image-argument"])
            release._require_installed_entrypoint(["bash", str(installed)])

    def test_image_config_requires_one_immutable_image_and_preserves_default_arguments(self):
        for entrypoint, cmd, expected in (
                (["/installed/entrypoint.sh"], ["--ordinary-argument"],
                 ["/installed/entrypoint.sh", "--ordinary-argument"]),
                (None, ["/installed/entrypoint.sh"], ["/installed/entrypoint.sh"])):
            with self.subTest(entrypoint=entrypoint):
                config = {"Entrypoint": entrypoint, "Cmd": cmd}
                response = SimpleNamespace(returncode=0, stderr="", stdout=json.dumps([{
                    "Id": "sha256:" + "b" * 64,
                    "Config": config,
                }]))
                with mock.patch.object(release.subprocess, "run", return_value=response) as run:
                    self.assertEqual(release._image_command("chosen"),
                                     ("sha256:" + "b" * 64, expected, config))
                run.assert_called_once_with(
                    ["docker", "image", "inspect", "chosen"],
                    capture_output=True, text=True, check=False)
        for document in ([], [{}, {}], [{"Id": "mutable-tag", "Config": {}}],
                         [{"Id": "sha256:" + "b" * 64, "Config": {
                             "Entrypoint": "/not-an-array", "Cmd": None}}],
                         [{"Id": "sha256:" + "b" * 64, "Config": {
                             "Entrypoint": [], "Cmd": None}}]):
            with self.subTest(document=document):
                response = SimpleNamespace(returncode=0, stderr="", stdout=json.dumps(document))
                with mock.patch.object(release.subprocess, "run", return_value=response):
                    with self.assertRaisesRegex(release.ReleaseRunError, "NEEDS selected image"):
                        release._image_command("chosen")

    def test_query_errors_keep_their_cause_and_never_launch_engine(self):
        for error in (OSError("selected.so: undefined symbol: real_symbol"),
                      RuntimeError("NEEDS receipt: size query returned 3"),
                      ImportError("missing staged dependency")):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)

                def build(_argv, **_kwargs):
                    write_build_manifest(work)
                    return SimpleNamespace(returncode=0)

                with mock.patch.dict(release.os.environ, {
                    "PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(b"source\n").hexdigest(),
                }, clear=True), mock.patch.object(release.subprocess, "run", side_effect=build) as run, \
                        mock.patch.object(release, "_module_input_names", side_effect=error):
                    with self.assertRaises(type(error)) as raised:
                        release._run_corpus_build(
                            [str(ROOT / "docker/entrypoint.sh")], {"notes": "annotation"}, work)
                self.assertIs(raised.exception, error)
                self.assertNotIn("manifest", release._cause_text(raised.exception))
                self.assertIn(str(error), release._cause_text(raised.exception))
                self.assertEqual(run.call_count, 1)

    def fingerprint_case(self, *, applied=True, failure=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        feed = directory / "bars.csv"
        feed.write_text(
            "timestamp,open,high,low,close,volume\n"
            "1700000000000,10,12,9,11,1\n1700000060000,11,13,10,12,1\n")
        (directory / "inputs.json").write_text(json.dumps({
            "notes": "raw request", "_comment": "annotation"}))
        (directory / "generated.cpp").write_text("// ordinary provenance fixture\n")
        report = {
            "trades": [], "net_profit": 0.0, "input_bars_processed": 2,
            "source_feed_sha256": "c" * 64,
        }
        if applied:
            report["applied_inputs"] = {"notes": "actually applied"}
        strategy = SimpleNamespace(
            lib=SimpleNamespace(), declares_bar_magnifier=False,
            run=mock.Mock(return_value=report),
            corpus_inputs=mock.Mock(side_effect=AssertionError("no fresh-handle classification")))
        output = directory / "fingerprint.json"
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(binding, "Strategy", return_value=strategy))
            stack.enter_context(mock.patch.dict(binding.os.environ, {}, clear=True))
            stack.enter_context(mock.patch.object(binding.sys, "argv", [
                "run_strategy.py", str(directory), "--ohlcv", str(feed),
                "--no-trim-output", "--fingerprint-json", str(output)]))
            stack.enter_context(contextlib.redirect_stdout(stdout))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            if failure is not None:
                stack.enter_context(mock.patch.object(binding, "build_fingerprint", side_effect=failure))
            try:
                status = binding.main()
            except RuntimeError as error:
                status = error
        strategy.run.assert_called_once()
        strategy.corpus_inputs.assert_not_called()
        self.assertNotIn("fingerprint: skipped", stdout.getvalue() + stderr.getvalue())
        return output, status

    def test_fingerprint_uses_the_values_returned_by_the_run(self):
        output, status = self.fingerprint_case()
        self.assertEqual(status, 0)
        document = json.loads(output.read_bytes())
        self.assertEqual(document["provenance"]["applied"]["inputs"], {"notes": "actually applied"})

    def test_requested_fingerprint_needs_is_a_failure(self):
        for applied, failure in ((False, None), (True, RuntimeError("NEEDS fingerprint dependency"))):
            with self.subTest(applied=applied):
                output, status = self.fingerprint_case(applied=applied, failure=failure)
                self.assertIsInstance(status, RuntimeError)
                self.assertIn("NEEDS", str(status))
                self.assertFalse(output.exists())
                if failure is not None:
                    self.assertIs(status.__cause__, failure)

    def test_invalid_build_manifests_stop_before_receipt_or_engine(self):
        def damage(document, defect):
            if defect == "schema":
                document["schema"] = "old"
            elif defect == "work":
                document["work_dir"] = "/other-work"
            elif defect == "inheritance":
                document["inherits"]["descriptors"] = ["7"]
            elif defect == "cwd":
                document["cwd"] = "relative"
            elif defect == "duplicate-slot":
                document["argv"] += ["--so", document["artifacts"]["so"]["path"]]
            elif defect == "module-slot":
                document["argv"][document["argv"].index("--so") + 1] = "/different.so"

        for defect in ("json", "schema", "work", "inheritance", "cwd", "duplicate-slot",
                       "module-slot", "source-vouch"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)

                def build(_argv, **_kwargs):
                    document = write_build_manifest(work)
                    damage(document, defect)
                    (work / "engine-step.json").write_text(
                        "{" if defect == "json" else json.dumps(document))
                    return SimpleNamespace(returncode=0)

                with mock.patch.dict(release.os.environ, {
                    "PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(
                        b"different vouched source\n" if defect == "source-vouch" else b"source\n"
                    ).hexdigest(),
                }, clear=True), mock.patch.object(release.subprocess, "run", side_effect=build) as run, \
                        mock.patch.object(release, "_module_input_names") as query:
                    with self.assertRaises(release.ReleaseRunError) as raised:
                        release._run_corpus_build(
                            [str(ROOT / "docker/entrypoint.sh")], {"notes": "annotation"}, work)
                self.assertIn("NEEDS", str(raised.exception))
                if defect == "json":
                    self.assertIn("JSONDecodeError", release._cause_text(raised.exception))
                if defect == "source-vouch":
                    self.assertIn("vouched source", str(raised.exception))
                query.assert_not_called()
                self.assertEqual(run.call_count, 1)

    def test_artifact_change_during_receipt_prevents_engine_launch(self):
        for name in ("so", "cpp"):
            with self.subTest(artifact=name), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                document = {}

                def build(_argv, **_kwargs):
                    document.update(write_build_manifest(work))
                    return SimpleNamespace(returncode=0)

                def query(_path):
                    Path(document["artifacts"][name]["path"]).write_bytes(b"changed after query\n")
                    return frozenset()

                with mock.patch.dict(release.os.environ, {
                    "PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(b"source\n").hexdigest(),
                }, clear=True), mock.patch.object(release.subprocess, "run", side_effect=build) as run, \
                        mock.patch.object(release, "_module_input_names", side_effect=query):
                    with self.assertRaisesRegex(release.ReleaseRunError, "artifact"):
                        release._run_corpus_build(
                            [str(ROOT / "docker/entrypoint.sh")], {"notes": "annotation"}, work)
                self.assertEqual(run.call_count, 1)


def write_build_manifest(work):
    # Ordinary fixture bytes are deliberately not a native library.
    source, module = work / "compiled.cpp", work / "selected.so"
    source.write_bytes(b"source\n")
    module.write_bytes(b"unit fixture module\n")
    document = {
        "schema": "pineforge-engine-step/v1", "work_dir": str(work), "cwd": str(work),
        "argv": [sys.executable, "/installed/run_json.py", "--so", str(module),
                 "--inputs", "{}", "--generated-cpp", str(source), "--magnifier-samples", "3"],
        "artifacts": {
            "cpp": {"path": str(source), "sha256": hashlib.sha256(source.read_bytes()).hexdigest()},
            "so": {"path": str(module), "sha256": hashlib.sha256(module.read_bytes()).hexdigest()},
        },
        "inherits": {"environment": "unchanged", "descriptors": []},
    }
    (work / "engine-step.json").write_text(json.dumps(document))
    return document


if __name__ == "__main__":
    unittest.main(verbosity=2)
