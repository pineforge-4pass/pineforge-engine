"""Tests of docker/request_feed_inventory.py, the consumer half of the request-feed inventory.

These are unit controls on a standalone API. Fixtures write temporary inventory and artifact bytes
and call the helper; every refusal is compared with the exact code and detail dict of the
interface, and each negative is one defect injected into an inventory that a positive control in
the same test resolves. They do not cover run_json's placement of the admission (after request
validation, before the first feed or library operation), a real wrapper, or the pinned
transpiler's inventories. The poison sentinels are process-wide entry points (a library load, a
feed reader, a child process, a module import) patched to record and fail, and open() is
recorded: they show the helper starts none of them and opens only the two files it may read.
"""
import ast
import builtins
import contextlib
import csv
import ctypes
import hashlib
import importlib
import importlib.util
import inspect
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "docker" / "request_feed_inventory.py"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


inventory = load_module("request_feed_inventory", MODULE_PATH)
Error = inventory.InventoryAdmissionError

# What the interface says, written out here and not read back from the module under test.
SCHEMA = "pineforge-request-feed-inventory/v1"
CODE = "window_mode_unsupported"
INVENTORY_CAPABILITY = "selected_window_request_feed_inventory"
CHART_CAPABILITY = "selected_window_chart_timeframe"
FEED_CAPABILITY = "selected_window_request_feed_timeframe"
INCOMPLETE = {"capability": INVENTORY_CAPABILITY}  # and no timeframe member

ARTIFACT_BYTES = b"not a library: hashed by the helper, never loaded\n"
ARTIFACT_SHA = hashlib.sha256(ARTIFACT_BYTES).hexdigest()
OTHER_SHA = hashlib.sha256(b"the bytes of another artifact\n").hexdigest()
SOURCE_SHA = hashlib.sha256(b"the generated C++ bytes\n").hexdigest()
PRIMARY_LIST = ["1", "3", "5", "15", "30", "60", "120", "240"]  # the first build's eight
FEED_LIST = ["1", "D"]
DEFAULT = object()

CANONICAL = {"1": "1", "15": "15", "240": "240", "99999": "99999", "100000": "100000",
             "999999": "999999", "1000000": "1000000", "D": "D", "1D": "D",
             "W": "W", "1W": "W", "M": "1M", "1M": "1M", "S": "1S", "1S": "1S", "2D": "2D",
             "3W": "3W", "12M": "12M", "30S": "30S", "9999D": "9999D", "10000D": "10000D",
             "99999W": "99999W", "100000M": "100000M", "123456S": "123456S"}
BAD_TOKENS = ("", " ", " 15", "15 ", "15\n", "\t1D", "0", "00", "015", "-1", "+1", "1.5", "1e2",
              "4h", "1H", "d", "1d", "w", "m", "s", "DD", "D1", "1DD", "0D",
              "01D", "１５", "٣", "15m", "1 D", "None", "null")


def policy(primary=DEFAULT, feeds=DEFAULT, preroll="supported"):
    return {"primary_chart_timeframes": list(PRIMARY_LIST) if primary is DEFAULT else primary,
            "preroll": preroll,
            "request_feed_timeframes": list(FEED_LIST) if feeds is DEFAULT else feeds}


def token(timeframe):
    return {"kind": "token", "timeframe": timeframe}


def input_ref(key, default):
    return {"kind": "input", "key": key, "default": default}


def unknown():
    return {"kind": "unknown"}


def inv(entries=(), binding=None, artifact_sha=ARTIFACT_SHA):
    """An inventory object as the pinned transpiler emits it once bound to its artifact."""
    return {"schema": SCHEMA, "source_sha256": SOURCE_SHA, "artifact_sha256": artifact_sha,
            "primary_chart_timeframe": binding, "entries": list(entries)}


def raw(entries="[]", binding="null", artifact_sha=ARTIFACT_SHA):
    """An inventory written by hand, for what json.dumps cannot make (a duplicate key)."""
    return ('{"schema": "%s", "source_sha256": "%s", "artifact_sha256": "%s", '
            '"primary_chart_timeframe": %s, "entries": %s}'
            % (SCHEMA, SOURCE_SHA, artifact_sha, binding, entries))


class Poison:
    """A stand-in for work the helper must never start: calling it records the call and fails."""

    def __init__(self, name, calls):
        self.name = name
        self.calls = calls

    def __call__(self, *args, **kwargs):
        self.calls.append(self.name)
        raise AssertionError(f"poison sentinel called: {self.name}")


@contextlib.contextmanager
def watch():
    """Patch the process-wide entry points through which a helper could load a library, read a
    feed, start a child process or import a module, to record and fail, and record every
    open(). No strategy or optimizer exists at this level: the child process and the module
    import are the ways the helper could start a constructor or a trial. Yields (calls, opened):
    the sentinels called, and the (path, mode) of every open()."""
    calls, opened = [], []
    real_open = builtins.open

    def recording_open(file, *args, **kwargs):
        mode = args[0] if args else kwargs.get("mode", "r")
        opened.append((os.fspath(file) if isinstance(file, (str, os.PathLike)) else file, mode))
        return real_open(file, *args, **kwargs)

    sentinels = [(ctypes, "CDLL", "library"), (ctypes, "PyDLL", "library"),
                 (ctypes.cdll, "LoadLibrary", "library"), (ctypes.pydll, "LoadLibrary", "library"),
                 (csv, "reader", "feed"), (csv, "DictReader", "feed"),
                 (subprocess, "Popen", "trial"), (os, "system", "trial"),
                 (importlib, "import_module", "constructor")]
    with contextlib.ExitStack() as stack:
        for target, name, label in sentinels:
            stack.enter_context(mock.patch.object(target, name, Poison(f"{label}:{name}", calls)))
        stack.enter_context(mock.patch.object(builtins, "open", recording_open))
        yield calls, opened


class Case(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.dir = Path(scratch.name)
        self.inventory_path = self.dir / "inventory.json"
        self.artifact_path = self.dir / "strategy.bin"
        self.artifact_path.write_bytes(ARTIFACT_BYTES)

    def write(self, content):
        """content is a dict (written as JSON), a str (UTF-8) or bytes: the inventory file."""
        if isinstance(content, dict):
            content = json.dumps(content)
        if isinstance(content, str):
            content = content.encode("utf-8")
        self.inventory_path.write_bytes(content)

    def resolve(self, content=None, inputs=DEFAULT, primary="15"):
        if content is not None:
            self.write(content)
        return inventory.resolve_request_feed_inventory(
            self.inventory_path, self.artifact_path, {} if inputs is DEFAULT else inputs,
            primary_timeframe=primary)

    def admit(self, content=None, inputs=DEFAULT, primary="15", support=DEFAULT):
        if content is not None:
            self.write(content)
        return inventory.admit_selected_request(
            primary, self.inventory_path, self.artifact_path,
            {} if inputs is DEFAULT else inputs, policy() if support is DEFAULT else support)

    def refused(self, detail, call, *args, **kwargs):
        with self.assertRaises(Error) as caught:
            call(*args, **kwargs)
        self.assertEqual(caught.exception.code, CODE)
        self.assertEqual(caught.exception.detail, detail)
        return caught.exception

    def incomplete(self, call, *args, **kwargs):
        return self.refused(INCOMPLETE, call, *args, **kwargs)


class ErrorAndSurfaceTests(unittest.TestCase):
    def test_the_error_carries_the_code_and_a_copied_detail_dict(self):
        source = {"capability": CHART_CAPABILITY, "timeframe": "W"}
        error = Error("text", source)
        self.assertEqual(error.code, CODE)
        self.assertEqual(Error.code, CODE)
        self.assertEqual(error.detail, source)
        source["timeframe"] = "D"
        self.assertEqual(error.detail["timeframe"], "W", "the detail is copied")
        self.assertEqual(str(error), "text")
        self.assertEqual(error.args, ("text",), "BaseException.args stays the exception's own")
        self.assertTrue(issubclass(Error, Exception))
        self.assertFalse(issubclass(Error, (OSError, ValueError)))

    def test_public_surface_and_signatures(self):
        self.assertEqual(sorted(inventory.__all__),
                         ["InventoryAdmissionError", "admit_selected_request",
                          "resolve_request_feed_inventory"])
        resolve = inspect.signature(inventory.resolve_request_feed_inventory)
        self.assertEqual(list(resolve.parameters), ["inventory_path", "artifact_path",
                                                    "validated_inputs", "primary_timeframe"])
        self.assertIs(resolve.parameters["primary_timeframe"].kind,
                      inspect.Parameter.KEYWORD_ONLY)
        admit = inspect.signature(inventory.admit_selected_request)
        self.assertEqual(list(admit.parameters), ["primary_timeframe", "inventory_path",
                                                  "artifact_path", "validated_inputs",
                                                  "support_policy"])
        for parameter in admit.parameters.values():
            self.assertIs(parameter.kind, inspect.Parameter.POSITIONAL_OR_KEYWORD)


class SourceTests(unittest.TestCase):
    """The helper reads no feed and loads no library, by construction as well as by the
    sentinels below: its imports, its calls and the modes of its two open() calls."""

    def setUp(self):
        self.tree = ast.parse(MODULE_PATH.read_text(encoding="utf-8"))

    def test_imports_are_the_small_allowed_set(self):
        imported = set()
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                imported.add(node.module)
        self.assertEqual(imported, {"__future__", "hashlib", "json", "os", "re",
                                    "collections.abc", "typing"})

    def test_no_evaluation_coercion_or_library_names(self):
        called = {node.func.id for node in ast.walk(self.tree)
                  if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)}
        self.assertFalse(called & {"eval", "exec", "compile", "__import__", "int", "float"})
        names = ({node.id for node in ast.walk(self.tree) if isinstance(node, ast.Name)}
                 | {node.attr for node in ast.walk(self.tree) if isinstance(node, ast.Attribute)})
        self.assertFalse(names & {"ctypes", "CDLL", "cdll", "dlopen", "subprocess", "Popen",
                                  "csv", "importlib", "run_json", "system"})

    def test_the_two_open_calls_are_binary_reads(self):
        opens = [node for node in ast.walk(self.tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                 and node.func.id == "open"]
        self.assertEqual(len(opens), 2)
        for call in opens:
            self.assertEqual(ast.literal_eval(call.args[1]), "rb")


class ResolveTests(Case):
    def test_complete_empty_inventory_yields_the_empty_set(self):
        for binding in (None, "15"):
            with self.subTest(binding=binding):
                result = self.resolve(inv([], binding))
                self.assertEqual(result, [])
                self.assertIsInstance(result, list)

    def test_literal_tokens_sort_and_collapse(self):
        entries = [token("D"), token("1"), token("1"), token("1D"), token("60")]
        result = self.resolve(inv(entries))
        self.assertEqual(result, ["1", "60", "D"])
        self.assertEqual(self.resolve(inv(list(reversed(entries)))), result)
        self.assertIsNot(self.resolve(), result, "a fresh list each call")

    def test_wire_normalization(self):
        for spelled, canonical in CANONICAL.items():
            with self.subTest(spelled=spelled):
                self.assertEqual(self.resolve(inv([token(spelled)])), [canonical])

    def test_malformed_tokens_are_incomplete(self):
        self.assertEqual(self.resolve(inv([token("15")])), ["15"])  # the control
        for bad in BAD_TOKENS:
            with self.subTest(bad=bad):
                self.incomplete(self.resolve, inv([token(bad)]))

    def test_long_minutes_and_counts_are_tokens_of_any_length(self):
        self.assertEqual(self.resolve(inv([token("100000"), token("1"), token("999999")])),
                         ["1", "100000", "999999"])
        self.assertEqual(self.resolve(inv([token("1000000")])), ["1000000"])
        self.assertEqual(self.resolve(inv([token("10000D"), token("99999W")])),
                         ["10000D", "99999W"])
        self.assertEqual(self.resolve(inv([token("10000M"), token("10000S")])),
                         ["10000M", "10000S"])
        self.assertEqual(self.resolve(inv([token("100000D"), token("1D")])), ["100000D", "D"])
        # digits are compared as spelled: a six-digit binding matches a six-digit primary only
        self.assertEqual(self.resolve(inv([token("1")], binding="100000"), primary="100000"),
                         ["1"])
        self.incomplete(self.resolve, inv([token("1")], binding="100000"), primary="15")

    def test_leading_zero_and_whitespace_spellings_stay_malformed(self):
        # the grammar has no leading zero and no whitespace at any length
        self.assertEqual(self.resolve(inv([token("100000")])), ["100000"])  # the control
        for bad in ("0100000", "00100000", "0999999", "010D", "0010000D", "01W", "001M",
                    "0001S", " 100000", "100000 ", "100000\n", "\t10000D", "10000D\r",
                    "10000D\f", " 1D", "1D "):
            with self.subTest(bad=bad):
                self.incomplete(self.resolve, inv([token(bad)]))
                self.incomplete(self.resolve, inv([input_ref("tf", bad)]))
                self.incomplete(self.resolve, inv([input_ref("tf", "1")]), inputs={"tf": bad})

    def test_any_unknown_entry_is_incomplete(self):
        self.assertEqual(self.resolve(inv([token("1"), token("D")])), ["1", "D"])
        for entries in ([unknown()], [unknown(), token("1")], [token("1"), unknown()],
                        [token("1"), unknown(), token("D")]):
            with self.subTest(entries=entries):
                self.incomplete(self.resolve, inv(entries))


class InputTests(Case):
    def test_the_declared_default_is_used_without_a_validated_value(self):
        self.assertEqual(self.resolve(inv([input_ref("tf", "D")])), ["D"])
        self.assertEqual(self.resolve(inv([input_ref("tf", "1D")])), ["D"])
        # the key is exact: another key, another case and the empty key are not "tf"
        others = {"TF": "W", "tf2": "W", "": "W", "t": "W"}
        self.assertEqual(self.resolve(inv([input_ref("tf", "1")]), inputs=others), ["1"])

    def test_a_validated_value_overrides_the_default(self):
        self.assertEqual(self.resolve(inv([input_ref("tf", "W")]), inputs={"tf": "1"}), ["1"])
        self.assertEqual(self.resolve(inv([input_ref("tf", "W")]), inputs={"tf": "1W"}), ["W"])
        self.assertEqual(self.resolve(inv([input_ref("tf", "1")]), inputs={"tf": "D"}), ["D"])
        entries = [input_ref("a", "1"), input_ref("b", "D")]
        self.assertEqual(self.resolve(inv(entries), inputs={"a": "5"}), ["5", "D"])

    def test_the_empty_key_is_an_exact_key_with_its_own_override_and_default(self):
        entry = [input_ref("", "D")]
        self.assertEqual(self.resolve(inv(entry)), ["D"])  # the control: the default
        self.assertEqual(self.resolve(inv(entry), inputs={}), ["D"])
        self.assertEqual(self.resolve(inv(entry), inputs={"tf": "1"}), ["D"])  # another key
        self.assertEqual(self.resolve(inv(entry), inputs={"": "1"}), ["1"])  # the override
        self.assertEqual(self.resolve(inv(entry), inputs={"": "100000"}), ["100000"])
        self.assertEqual(self.resolve(inv(entry), inputs={"": "1", " ": "W"}), ["1"])
        # a present override never consults the default, and an invalid one never falls back
        self.assertEqual(self.resolve(inv([input_ref("", "")]), inputs={"": "1"}), ["1"])
        self.incomplete(self.resolve, inv([input_ref("", "")]))
        self.incomplete(self.resolve, inv(entry), inputs={"": ""})
        self.incomplete(self.resolve, inv(entry), inputs={"": 15})
        self.incomplete(self.resolve, inv(entry), inputs={"": "4h"})
        # the key is exact: a space is another key, and "" is not "tf"
        self.assertEqual(self.resolve(inv([input_ref(" ", "1")]), inputs={"": "W"}), ["1"])
        self.assertEqual(self.resolve(inv([input_ref(" ", "1")]), inputs={" ": "W"}), ["W"])

    def test_an_invalid_or_nonstring_value_is_incomplete_and_never_falls_back(self):
        entries = [input_ref("tf", "1")]
        self.assertEqual(self.resolve(inv(entries), inputs={"tf": "5"}), ["5"])  # the control
        for value in (15, 1.5, True, None, ["15"], {"a": "1"}, "", " 15", "15 ", "4h", "15\n",
                      "１５"):
            with self.subTest(value=value):
                self.incomplete(self.resolve, inv(entries), inputs={"tf": value})

    def test_an_invalid_default_with_no_validated_value_is_incomplete(self):
        self.assertEqual(self.resolve(inv([input_ref("tf", "1")])), ["1"])  # the control
        for default in ("", " 1", "4h", "d", "0", "1.0"):
            with self.subTest(default=default):
                self.incomplete(self.resolve, inv([input_ref("tf", default)]))
                self.incomplete(self.resolve, inv([input_ref("tf", default)]),
                                inputs={"other": "1"})

    def test_validated_inputs_must_be_a_mapping(self):
        self.assertEqual(self.resolve(inv([]), inputs={}), [])  # the control
        for inputs in (None, [], ["tf"], "tf", 5):
            with self.subTest(inputs=inputs):
                self.incomplete(self.resolve, inv([]), inputs=inputs)


class InventoryFileTests(Case):
    def test_missing_and_unreadable_inventory_is_incomplete(self):
        self.write(inv([token("1")]))
        self.assertEqual(self.resolve(), ["1"])  # the control
        absent = self.dir / "absent.json"
        directory = self.dir / "a-directory"
        directory.mkdir()
        nul = str(self.dir) + "/in\0v.json"
        for path in (absent, str(absent), directory, None, 3, True, b"/x", object(), nul):
            with self.subTest(path=path):
                self.incomplete(inventory.resolve_request_feed_inventory, path,
                                self.artifact_path, {}, primary_timeframe="15")

    def test_bytes_that_are_not_strict_json_are_incomplete(self):
        good = json.dumps(inv([token("1")]))
        self.assertEqual(self.resolve(good), ["1"])  # the control
        cases = {
            "empty": b"", "blank": b" \n", "not json": b"inventory",
            "truncated": good[:-1].encode(), "trailing data": (good + " {}").encode(),
            "bad utf-8": b'{"schema": "\xff"}', "bom": b"\xef\xbb\xbf" + good.encode(),
            "utf-16": good.encode("utf-16"), "single quotes": good.replace('"', "'"),
            "top-level array": "[]", "top-level null": "null", "top-level string": '"x"',
            "empty object": "{}", "deep nesting": "[" * 100000 + "]" * 100000,
        }
        for name, content in cases.items():
            with self.subTest(name=name):
                self.incomplete(self.resolve, content)

    def test_an_oversize_inventory_is_incomplete(self):
        content = json.dumps(inv([token("1")]))
        size = len(content.encode("utf-8"))
        self.write(content)
        with mock.patch.object(inventory, "_INVENTORY_MAX_BYTES", size):
            self.assertEqual(self.resolve(), ["1"])
        with mock.patch.object(inventory, "_INVENTORY_MAX_BYTES", size - 1):
            self.incomplete(self.resolve)

    def test_duplicate_keys_are_refused_even_when_the_values_agree(self):
        self.assertEqual(self.resolve(raw('[{"kind": "token", "timeframe": "1"}]')), ["1"])
        cases = {
            "top level, equal values": raw()[:-1] + ', "schema": "%s"}' % SCHEMA,
            "top level, other digest last": raw()[:-1] + ', "artifact_sha256": "%s"}' % OTHER_SHA,
            "entry, permitted value last":
                raw('[{"kind": "token", "timeframe": "W", "timeframe": "1"}]'),
            "entry, permitted value first":
                raw('[{"kind": "token", "timeframe": "1", "timeframe": "W"}]'),
            "input entry": raw('[{"kind": "input", "key": "a", "key": "b", "default": "1"}]'),
        }
        for name, content in cases.items():
            with self.subTest(name=name):
                self.incomplete(self.resolve, content)

    def test_extra_missing_and_mistyped_members_are_incomplete(self):
        good = inv([token("1")])
        self.assertEqual(self.resolve(good), ["1"])  # the control
        mutations = {"extra member": dict(good, extra=1)}
        for member in good:
            mutations[f"without {member}"] = {k: v for k, v in good.items() if k != member}
        mutations.update({
            "schema v2": dict(good, schema="pineforge-request-feed-inventory/v2"),
            "schema empty": dict(good, schema=""),
            "schema null": dict(good, schema=None),
            "schema number": dict(good, schema=1),
            "source digest null": dict(good, source_sha256=None),
            "source digest number": dict(good, source_sha256=5),
            "source digest short": dict(good, source_sha256="0" * 63),
            "source digest long": dict(good, source_sha256="0" * 65),
            "source digest not hex": dict(good, source_sha256="g" * 64),
            "source digest upper case": dict(good, source_sha256="A" * 64),
            "source digest padded": dict(good, source_sha256=" " + "0" * 63),
            "artifact digest number": dict(good, artifact_sha256=5),
            "artifact digest short": dict(good, artifact_sha256="0" * 63),
            "artifact digest upper case": dict(good, artifact_sha256="A" * 64),
            "binding number": dict(good, primary_chart_timeframe=15),
            "binding empty": dict(good, primary_chart_timeframe=""),
            "binding text": dict(good, primary_chart_timeframe="4h"),
            "entries null": dict(good, entries=None),
            "entries object": dict(good, entries={"0": token("1")}),
            "entries string": dict(good, entries="[]"),
        })
        entries = {
            "entry null": None, "entry string": "token", "entry list": [token("1")],
            "entry empty": {}, "kind absent": {"timeframe": "1"},
            "kind null": {"kind": None, "timeframe": "1"},
            "kind upper case": {"kind": "TOKEN", "timeframe": "1"},
            "kind other": {"kind": "other", "timeframe": "1"},
            "kind list": {"kind": ["token"], "timeframe": "1"},
            "token extra": dict(token("1"), note="x"), "token without timeframe": {"kind": "token"},
            "token timeframe number": {"kind": "token", "timeframe": 15},
            "token timeframe null": {"kind": "token", "timeframe": None},
            "token timeframe list": {"kind": "token", "timeframe": ["1"]},
            "input without default": {"kind": "input", "key": "tf"},
            "input without key": {"kind": "input", "default": "1"},
            "input extra": dict(input_ref("tf", "1"), note="x"),
            "input key number": input_ref(5, "1"),
            "input default null": input_ref("tf", None),
            "input default number": input_ref("tf", 1),
            "unknown extra": dict(unknown(), note="x"),
        }
        for name, entry in entries.items():
            mutations[name] = dict(good, entries=[token("1"), entry])
        for name, mutated in mutations.items():
            with self.subTest(name=name):
                self.incomplete(self.resolve, mutated)


class ArtifactBindingTests(Case):
    def test_unbound_wrong_and_changed_artifacts_are_incomplete(self):
        self.assertEqual(self.resolve(inv([token("1")])), ["1"])  # the control
        self.incomplete(self.resolve, inv([token("1")], artifact_sha=None))
        self.incomplete(self.resolve, inv([token("1")], artifact_sha=OTHER_SHA))
        self.write(inv([token("1")]))
        self.artifact_path.write_bytes(ARTIFACT_BYTES + b"\0")  # changed after it was bound
        self.incomplete(self.resolve)
        self.artifact_path.write_bytes(ARTIFACT_BYTES)
        self.assertEqual(self.resolve(), ["1"])

    def test_missing_and_unreadable_artifact_is_incomplete(self):
        self.write(inv([token("1")]))
        self.assertEqual(self.resolve(), ["1"])  # the control
        nul = str(self.dir) + "/a\0b"
        for path in (self.dir / "absent.bin", self.dir, None, 3, True, b"/x", object(), nul):
            with self.subTest(path=path):
                self.incomplete(inventory.resolve_request_feed_inventory, self.inventory_path,
                                path, {}, primary_timeframe="15")

    def test_the_artifact_is_hashed_in_chunks(self):
        self.write(inv([token("1")]))
        with mock.patch.object(inventory, "_READ_CHUNK", 7):
            self.assertEqual(self.resolve(), ["1"])


class ChartBindingTests(Case):
    def test_a_recorded_binding_must_equal_the_requests_primary(self):
        entries = [token("1")]
        self.assertEqual(self.resolve(inv(entries, binding="15"), primary="15"), ["1"])
        self.assertEqual(self.resolve(inv(entries, binding=None), primary="15"), ["1"])
        for binding, primary in (("5", "15"), ("15", "5"), ("D", "W"), ("W", "D"), ("D", "15"),
                                 ("15", "D")):
            with self.subTest(binding=binding, primary=primary):
                self.incomplete(self.resolve, inv(entries, binding=binding), primary=primary)
        # entries that do not depend on the chart do not excuse a mismatch
        self.incomplete(self.resolve, inv([], binding="5"), primary="15")

    def test_the_requests_primary_aliases_are_canonicalized_first(self):
        for binding, primary in (("D", "1D"), ("D", "D"), ("W", "1W"), ("W", "W")):
            with self.subTest(binding=binding, primary=primary):
                self.assertEqual(self.resolve(inv([token("1")], binding=binding),
                                              primary=primary), ["1"])
        self.incomplete(self.resolve, inv([token("1")], binding="D"), primary="1W")


class AdmitTests(Case):
    def test_permitted_tokens_are_admitted_and_returned(self):  # the same/other positives
        entries = [token("1"), token("1"), token("D")]  # same-ticker and other-symbol sites
        self.assertEqual(self.admit(inv(entries)), ["1", "D"])
        self.assertEqual(self.admit(), self.resolve())
        self.assertEqual(self.admit(inv([input_ref("tf", "W")]), inputs={"tf": "1D"}), ["D"])
        self.assertEqual(self.admit(inv([]), support=policy(feeds=[])), [])
        self.assertEqual(self.admit(inv([token("1")]), support=policy(preroll="zero_only")),
                         ["1"], "preroll is not this helper's to enforce")

    def test_a_weekly_token_is_excluded_with_the_exact_token(self):
        weekly = {"capability": FEED_CAPABILITY, "timeframe": "W"}
        for entries, inputs in (([token("W")], {}), ([token("1W")], {}),
                                ([token("1"), token("W")], {}),
                                ([input_ref("tf", "W")], {}),
                                ([input_ref("tf", "1")], {"tf": "1W"}),
                                ([input_ref("tf", "D")], {"tf": "W"})):
            with self.subTest(entries=entries, inputs=inputs):
                self.refused(weekly, self.admit, inv(entries), inputs=inputs)

    def test_long_tokens_outside_the_policy_name_their_exact_spelling(self):
        # a token of any length that the policy does not list is the feed or the chart capability,
        # never the inventory one
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "100000"}, self.admit,
                     inv([token("100000")]))
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "999999"}, self.admit,
                     inv([token("1"), token("999999")]))
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "10000D"}, self.admit,
                     inv([token("10000D")]))
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "99999W"}, self.admit,
                     inv([token("1"), token("99999W")]))
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "100000"}, self.admit,
                     inv([input_ref("tf", "100000")]))
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "100000"}, self.admit,
                     inv([input_ref("tf", "1")]), inputs={"tf": "100000"})
        self.refused({"capability": CHART_CAPABILITY, "timeframe": "100000"}, self.admit,
                     inv([]), primary="100000")
        self.refused({"capability": CHART_CAPABILITY, "timeframe": "10000D"}, self.admit,
                     inv([]), primary="10000D")
        # the same spellings are admitted when the policy lists them
        self.assertEqual(self.admit(inv([token("100000")]),
                                    support=policy(feeds=["100000", "10000D"])), ["100000"])
        self.assertEqual(self.admit(inv([token("10000D")]),
                                    support=policy(feeds=["10000D"])), ["10000D"])

    def test_the_empty_key_reaches_the_policy_like_any_key(self):
        self.assertEqual(self.admit(inv([input_ref("", "W")]), inputs={"": "1"}), ["1"])
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "W"}, self.admit,
                     inv([input_ref("", "1")]), inputs={"": "W"})
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "W"}, self.admit,
                     inv([input_ref("", "W")]))

    def test_other_excluded_tokens_are_named_canonically(self):
        for spelled, canonical in (("5", "5"), ("2D", "2D"), ("M", "1M"), ("S", "1S"),
                                   ("1440", "1440")):
            with self.subTest(spelled=spelled):
                self.refused({"capability": FEED_CAPABILITY, "timeframe": canonical},
                             self.admit, inv([token(spelled)]))

    def test_the_first_excluded_token_in_sorted_order_is_named(self):
        # the interface names one token; this helper names the first of the sorted set
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "1M"}, self.admit,
                     inv([token("W"), token("2D"), token("1M"), token("1")]))

    def test_an_incomplete_inventory_is_the_inventory_capability(self):
        self.assertEqual(self.admit(inv([token("1")])), ["1"])  # the control
        self.incomplete(self.admit, inv([token("1"), unknown()]))
        self.incomplete(self.admit, inv([token("1")], artifact_sha=OTHER_SHA))
        self.incomplete(self.admit, inv([token("1")], artifact_sha=None))
        self.incomplete(self.admit, inv([token("1")], binding="5"))
        self.incomplete(self.admit, b"")
        self.incomplete(inventory.admit_selected_request, "15", self.dir / "absent",
                        self.artifact_path, {}, policy())
        # the inventory resolves before its tokens are checked: an unknown entry hides no token
        self.incomplete(self.admit, inv([token("W"), unknown()]))

    def test_a_primary_outside_the_policy_names_the_canonical_primary(self):
        good = inv([token("1")])
        for spelled, canonical in (("W", "W"), ("1W", "W"), ("D", "D"), ("1D", "D"), ("2", "2"),
                                   ("1440", "1440"), ("M", "1M"), ("S", "1S"), ("2D", "2D")):
            with self.subTest(primary=spelled):
                self.refused({"capability": CHART_CAPABILITY, "timeframe": canonical},
                             self.admit, good, primary=spelled)
        for primary in PRIMARY_LIST:
            with self.subTest(primary=primary):
                self.assertEqual(self.admit(good, primary=primary), ["1"])

    def test_primary_aliases_match_a_policy_that_lists_the_canonical_token(self):
        support = policy(primary=["D", "W"], feeds=["1"])
        for primary in ("D", "1D", "W", "1W"):
            with self.subTest(primary=primary):
                self.assertEqual(self.admit(inv([token("1")]), primary=primary,
                                            support=support), ["1"])
        self.refused({"capability": CHART_CAPABILITY, "timeframe": "15"}, self.admit,
                     inv([token("1")]), primary="15", support=support)

    def test_a_primary_outside_the_policy_reads_no_file(self):
        absent = self.dir / "absent"
        for spelled, canonical in (("W", "W"), ("1D", "D")):
            with self.subTest(primary=spelled):
                with watch() as (calls, opened):
                    self.refused({"capability": CHART_CAPABILITY, "timeframe": canonical},
                                 inventory.admit_selected_request, spelled, absent, absent, {},
                                 policy())
                self.assertEqual((calls, opened), ([], []))
        # the same call with a permitted primary does reach the inventory, and finds it missing
        self.incomplete(inventory.admit_selected_request, "15", absent, absent, {}, policy())

    def test_a_missing_or_invalid_policy_supports_nothing_and_reads_no_file(self):
        # a policy that cannot be read exactly declares no support; no new
        # capability may be added, so the refusal is the primary one
        good = policy()
        self.write(inv([]))
        self.assertEqual(self.admit(support=good), [])  # the control
        bad = {"none": None, "empty": {}, "list": [], "word": "all", "number": 1,
               "extra member": dict(good, extra=[]), "preroll empty": dict(good, preroll=""),
               "preroll case": dict(good, preroll="SUPPORTED"),
               "preroll null": dict(good, preroll=None), "preroll number": dict(good, preroll=1)}
        for key in good:
            bad[f"without {key}"] = {k: v for k, v in good.items() if k != key}
        for member in ("primary_chart_timeframes", "request_feed_timeframes"):
            for name, value in (("word case", "ALL"), ("other word", "none"), ("null", None),
                                ("number", 5), ("object", {"1": 1}), ("set", {"1"}),
                                ("alias in list", ["1D"]), ("space in list", ["D "]),
                                ("number in list", [1]), ("null in list", [None]),
                                ("word in list", ["all"]), ("text in list", ["4h"]),
                                ("empty string in list", [""]), ("nested list", [["1"]])):
                bad[f"{member}: {name}"] = dict(good, **{member: value})
        for name, support in bad.items():
            with self.subTest(name=name):
                with watch() as (calls, opened):
                    self.refused({"capability": CHART_CAPABILITY, "timeframe": "15"},
                                 self.admit, support=support)
                self.assertEqual((calls, opened), ([], []))

    def test_policy_lists_may_be_tuples_repeat_tokens_or_be_empty(self):
        self.assertEqual(self.admit(inv([token("1")]),
                                    support=policy(primary=("15", "15"), feeds=("1", "1"))), ["1"])
        self.refused({"capability": CHART_CAPABILITY, "timeframe": "15"}, self.admit,
                     inv([]), support=policy(primary=[]))
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "1"}, self.admit,
                     inv([token("1")]), support=policy(feeds=[]))

    def test_all_for_primary_admits_any_primary_and_still_checks_feeds(self):
        support = policy(primary="all")
        self.assertEqual(self.admit(inv([token("1")]), primary="W", support=support), ["1"])
        self.assertEqual(self.admit(inv([token("1")]), primary="1D", support=support), ["1"])
        self.refused({"capability": FEED_CAPABILITY, "timeframe": "W"}, self.admit,
                     inv([token("W")]), primary="W", support=support)

    def test_all_for_request_feeds_does_not_read_the_inventory(self):
        # under all the third fact does not enter the decision. The interface does
        # not say what is returned then: None, never an empty set, for a set not resolved
        absent = self.dir / "absent"
        with watch() as (calls, opened):
            listed = inventory.admit_selected_request("15", absent, absent, {},
                                                      policy(feeds="all"))
            both = inventory.admit_selected_request("W", absent, absent, {},
                                                    policy(primary="all", feeds="all"))
        self.assertEqual((listed, both, calls, opened), (None, None, [], []))
        with watch() as (calls, opened):
            self.refused({"capability": CHART_CAPABILITY, "timeframe": "W"},
                         inventory.admit_selected_request, "W", absent, absent, {},
                         policy(feeds="all"))
        self.assertEqual((calls, opened), ([], []))

    def test_a_primary_that_is_not_a_timeframe_token_is_a_caller_error(self):
        # the interface is silent: the request was validated before this call, so this is not
        # a refusal of the request
        self.write(inv([]))
        for primary in (None, 15, "", " 15", "4h", "d", "0100000", "100000 ", "10000D\n"):
            with self.subTest(primary=primary):
                with self.assertRaises(ValueError):
                    self.admit(primary=primary)
                with self.assertRaises(ValueError):
                    self.resolve(primary=primary)

    def test_the_arguments_are_not_modified(self):
        inputs, support = {"tf": "1D"}, policy()
        before = (json.dumps(inputs), json.dumps(support))
        self.admit(inv([input_ref("tf", "1")]), inputs=inputs, support=support)
        self.assertEqual((json.dumps(inputs), json.dumps(support)), before)


class NoWorkTests(Case):
    """Every pre-feed negative of the interface's first proof, watched: no library load, feed
    read, child process or import, and no file opened but the inventory and the artifact. The
    wrapper below only stands in for the later run_json placement (admit first; the work of the
    run begins only when admit returns): it is not that integration and proves nothing about it."""

    COUNTERS = {"feed_opens": 0, "library_loads": 0, "constructors": 0, "trials": 0}

    def run_wrapper(self, content, inputs=None):
        self.write(content)
        counters = dict.fromkeys(self.COUNTERS, 0)
        outcome = None
        with watch() as (calls, opened):
            try:
                outcome = inventory.admit_selected_request(
                    "15", self.inventory_path, self.artifact_path,
                    {} if inputs is None else inputs, policy())
            except Error as error:
                outcome = error
            else:
                for name in counters:
                    counters[name] += 1
        return outcome, counters, calls, opened

    def test_every_pre_feed_negative_does_no_work(self):
        allowed = {(str(self.inventory_path), "rb"), (str(self.artifact_path), "rb")}
        negatives = {
            "known weekly exclusion": (inv([token("W")]), None),
            "unknown entry": (inv([unknown()]), None),
            "artifact mismatch": (inv([token("1")], artifact_sha=OTHER_SHA), None),
            "unbound": (inv([token("1")], artifact_sha=None), None),
            "chart binding mismatch": (inv([token("1")], binding="5"), None),
            "malformed inventory": (b"not json", None),
            "input resolves to weekly": (inv([input_ref("tf", "1")]), {"tf": "W"}),
            "input is not a string": (inv([input_ref("tf", "1")]), {"tf": 15}),
        }
        for name, (content, inputs) in negatives.items():
            with self.subTest(name=name):
                outcome, counters, calls, opened = self.run_wrapper(content, inputs)
                self.assertIsInstance(outcome, Error)
                self.assertEqual(counters, self.COUNTERS)
                self.assertEqual(calls, [])
                self.assertLessEqual(set(opened), allowed)

    def test_a_missing_inventory_does_no_work(self):
        absent = self.dir / "absent"
        with watch() as (calls, opened):
            with self.assertRaises(Error):
                inventory.admit_selected_request("15", absent, absent, {}, policy())
        self.assertEqual(calls, [])
        self.assertLessEqual(set(opened), {(str(absent), "rb")})

    def test_a_positive_opens_only_the_two_files_and_starts_nothing(self):
        outcome, counters, calls, opened = self.run_wrapper(inv([token("1"), token("D")]))
        self.assertEqual(outcome, ["1", "D"])
        self.assertEqual(calls, [])
        self.assertEqual(sorted(opened), sorted([(str(self.inventory_path), "rb"),
                                                 (str(self.artifact_path), "rb")]))
        # the wrapper's own counters moved: they are live, and only admit's return releases work
        self.assertEqual(counters, {name: 1 for name in self.COUNTERS})

    def test_the_sentinels_fire_when_work_is_attempted(self):
        # Each attempt runs inside watch(), where os.system and subprocess.Popen are the poison
        # sentinels: they record and raise before any shell or process could start.
        attempts = (lambda: ctypes.CDLL("x"), lambda: ctypes.cdll.LoadLibrary("x"),
                    lambda: csv.reader([]), lambda: subprocess.Popen(["x"]),
                    lambda: os.system("x"), lambda: importlib.import_module("x"))
        with watch() as (calls, opened):
            for attempt in attempts:
                with self.assertRaises(AssertionError):
                    attempt()
            open(self.artifact_path, "rb").close()
        self.assertEqual(calls, ["library:CDLL", "library:LoadLibrary", "feed:reader",
                                 "trial:Popen", "trial:system", "constructor:import_module"])
        self.assertEqual(opened, [(str(self.artifact_path), "rb")])


if __name__ == "__main__":
    unittest.main()
