#!/usr/bin/env python3
"""Mutation controls for native C++ ABI ownership; no compiler or engine runs."""
import json
import tempfile
import unittest
from pathlib import Path

from check_native_cpp_versions import DRIVER_FORWARD, FILES, check_texts, load

DATA = load()


class NativeVersions(unittest.TestCase):
    def reject(self, path, before, after):
        self.assertIn(before, DATA[path])
        changed = dict(DATA)
        changed[path] = changed[path].replace(before, after, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_current(self):
        check_texts(DATA)

    def test_stale_wrapper(self):
        for path, namespace, stale in (
            (FILES[0], "native_order_v2", "native_order_v1"),
            (FILES[1], "native_order_v2", "native_order_v1"),
            (FILES[11], "native_order_v1", "native_order_v2"),
            (FILES[2], "native_calendar_v2", "native_calendar_v1"),
            (FILES[3], "native_calendar_v2", "native_calendar_v3"),
            (FILES[4], "native_run_spec_v1", "native_run_spec_v2"),
            (FILES[5], "native_run_spec_v1", "native_run_spec_v2"),
            (FILES[6], "native_driver_v3", "native_driver_v2"),
            (FILES[7], "native_driver_v3", "native_driver_v4"),
            (FILES[8], "engine_script_run_v13", "engine_script_run_v12"),
            (FILES[9], "engine_script_run_v13", "engine_script_run_v12"),
            (FILES[10], "engine_script_run_v13", "engine_script_run_v12"),
        ):
            with self.subTest(path=path, namespace=namespace):
                self.reject(path, namespace, stale)

    def test_duplicate_wrapper(self):
        for path, namespace in (
            (FILES[0], "native_order_v2"),
            (FILES[2], "native_calendar_v2"),
            (FILES[4], "native_run_spec_v1"),
            (FILES[6], "native_driver_v3"),
            (FILES[8], "engine_script_run_v13"),
            (FILES[11], "native_order_v1"),
        ):
            with self.subTest(path=path):
                opening = "inline namespace " + namespace + " {"
                self.reject(path, opening, opening + " } inline namespace " + namespace + " {")

    def test_empty_namespace_is_not_ownership(self):
        for path, namespace in (
            (FILES[0], "native_order_v2"),
            (FILES[2], "native_calendar_v2"),
            (FILES[4], "native_run_spec_v1"),
            (FILES[6], "native_driver_v3"),
            (FILES[8], "engine_script_run_v13"),
            (FILES[11], "native_order_v1"),
        ):
            with self.subTest(path=path):
                self.reject(path, "inline namespace " + namespace + " {",
                            "inline namespace " + namespace + " {} namespace misplaced {")

    def test_comment_only_namespace_is_not_ownership(self):
        for path, namespace, decoy in (
            (FILES[0], "native_order_v2", "struct WorkingRequestCore"),
            (FILES[11], "native_order_v1", "struct RunIdentity"),
            (FILES[2], "native_calendar_v2", "parse_timeframe NativeInterval"),
            (FILES[8], "engine_script_run_v13", "class NativeStrategyHost"),
        ):
            with self.subTest(path=path):
                self.reject(
                    path,
                    "inline namespace " + namespace + " {",
                    "inline namespace " + namespace + " { /* " + decoy + " */ } namespace misplaced {")

    def test_type_moved_outside_owner(self):
        self.reject(FILES[0], "struct Request {",
                    "} struct Request {")
        self.reject(FILES[11], "struct RunIdentity {",
                    "} struct RunIdentity {")
        self.reject(FILES[2], "struct NativeInterval {",
                    "} struct NativeInterval {")
        self.reject(FILES[4], "struct NativeRunSpec {",
                    "} struct NativeRunSpec {")
        self.reject(FILES[6], "struct NativeCoordinate {",
                    "} struct NativeCoordinate {")
        self.reject(FILES[8], "class NativeStrategyHost : public BacktestEngine {",
                    "} class NativeStrategyHost : public BacktestEngine {")

    def test_command_event_alias_moved_outside_owner(self):
        self.reject(
            FILES[0],
            "using CommandEvent = std::variant<AcceptedEvent,",
            "} using CommandEvent = std::variant<AcceptedEvent,")

    def test_return_only_function_escape(self):
        calendar = FILES[2]
        self.reject(
            calendar,
            "std::optional<Timeframe> parse_timeframe(std::string_view text);",
            "")
        changed = dict(DATA)
        decl = "std::optional<Timeframe> parse_timeframe(std::string_view text);"
        text = changed[calendar].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_calendar_v2",
            "}  // inline namespace native_calendar_v2\n" + decl,
            1)
        changed[calendar] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        decl = "std::optional<TimezoneIdentityDescriptor>\ntimezone_identity_descriptor(std::string_view timezone);"
        self.assertIn(decl, changed[calendar])
        text = changed[calendar].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_calendar_v2",
            "}  // inline namespace native_calendar_v2\n" + decl,
            1)
        changed[calendar] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        decl = "std::optional<int64_t> period_key(const SessionCalendar& calendar, const Timeframe& tf, int64_t ms);"
        self.assertIn(decl, changed[calendar])
        text = changed[calendar].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_calendar_v2",
            "}  // inline namespace native_calendar_v2\n" + decl,
            1)
        changed[calendar] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        spec = FILES[4]
        changed = dict(DATA)
        decl = "NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept;"
        text = changed[spec].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_run_spec_v1",
            "}  // inline namespace native_run_spec_v1\n" + decl,
            1)
        changed[spec] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_out_of_line_function_escape(self):
        changed = dict(DATA)
        src = FILES[7]
        start = changed[src].index("bool native_bar_structurally_valid")
        end = changed[src].index("NativeInputPreflightResult preflight_native_inputs")
        func = changed[src][start:end]
        text = changed[src][:start] + changed[src][end:]
        text = text.replace(
            "}  // inline namespace native_driver_v3",
            "}  // inline namespace native_driver_v3\n" + func,
            1)
        changed[src] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        src = FILES[3]
        needle = "std::optional<Timeframe> parse_timeframe(std::string_view text) {"
        self.assertIn(needle, changed[src])
        changed[src] = changed[src].replace(
            needle, "/* escaped parse_timeframe */ int parse_timeframe_unused() {", 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        src = FILES[1]
        needle = "WorkingRequestCore::reset("
        self.assertIn(needle, changed[src])
        changed[src] = changed[src].replace(
            "}  // inline namespace native_order_v2",
            "}  // inline namespace native_order_v2\nvoid WorkingRequestCore::reset(RunIdentity) {}\n",
            1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_driver_native_run_spec_forward_must_stay_outside(self):
        self.reject(FILES[6], DRIVER_FORWARD, "")
        self.reject(
            FILES[6],
            DRIVER_FORWARD + "\ninline namespace native_driver_v3 {",
            "inline namespace native_driver_v3 {\n" + DRIVER_FORWARD)
        self.reject(
            FILES[6],
            DRIVER_FORWARD,
            "inline namespace native_run_spec_v1 { struct NativeRunSpec {}; }")

    def test_host_public_values_cannot_leave_v13(self):
        self.reject(FILES[8], "struct NativeStateView {", "} struct NativeStateView {")
        self.reject(FILES[8], "struct NativeFailure {", "} struct NativeFailure {")
        self.reject(FILES[8], "struct NativeFailureContext {", "} struct NativeFailureContext {")
        self.reject(FILES[9], "class NativeExecutionConsumer final : public IExecutionConsumer {",
                    "} class NativeExecutionConsumer final : public IExecutionConsumer {")
        self.reject(
            FILES[8],
            "native_order::SubmitResult submit(const native_order::Request& request);",
            "")
        self.reject(
            FILES[6],
            'kNativeConsumerSemanticVersion = "native-consumer/v4"',
            'kNativeConsumerSemanticVersion = "native-consumer/v3"')


class FixtureAuthentication(unittest.TestCase):
    def test_authentic_fixtures_unpack(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture
        for name in ("order-1acaf33", "calendar-draft-a8e34c", "calendar-262a280",
                     "driver-08b5c88", "run-spec-262a280", "host-e7d023d",
                     "order-e7d023d"):
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory(prefix="pf-native-abi-ok-") as temp:
                    authenticate_fixture(FIXTURE / name, Path(temp) / name)

    def test_rejects_tampered_hash(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture
        fixture = FIXTURE / "calendar-draft-a8e34c"
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-tamper-") as temp:
            dest = Path(temp)
            (dest / "headers.json.gz").write_bytes((fixture / "headers.json.gz").read_bytes())
            manifest = json.loads((fixture / "manifest.json").read_text())
            header = "pineforge/native_calendar.hpp"
            manifest["files"][header]["sha256"] = "0" * 64
            (dest / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")

    def test_rejects_tampered_archive_digest(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture
        fixture = FIXTURE / "order-1acaf33"
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-archive-") as temp:
            dest = Path(temp)
            (dest / "headers.json.gz").write_bytes((fixture / "headers.json.gz").read_bytes())
            manifest = json.loads((fixture / "manifest.json").read_text())
            manifest["archive_sha256"] = "0" * 64
            (dest / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")

    def test_rejects_path_set_mismatch_and_escape(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture, frozen_contents, git_blob
        import gzip
        import hashlib
        import io
        fixture = FIXTURE / "driver-08b5c88"
        manifest = json.loads((fixture / "manifest.json").read_text())
        contents = frozen_contents(fixture, manifest)
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-set-") as temp:
            dest = Path(temp)
            extra = dict(contents)
            extra["pineforge/not-in-manifest.hpp"] = "x"
            buf = io.BytesIO()
            with gzip.GzipFile(filename="", fileobj=buf, mode="wb", mtime=0) as z:
                z.write((json.dumps(extra, indent=2, sort_keys=True) + "\n").encode())
            archive = buf.getvalue()
            (dest / "headers.json.gz").write_bytes(archive)
            mutated = dict(manifest)
            mutated["archive_sha256"] = hashlib.sha256(archive).hexdigest()
            (dest / "manifest.json").write_text(json.dumps(mutated))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")

        raw = b"not a header"
        contents["../escape.hpp"] = raw.decode()
        buf = io.BytesIO()
        with gzip.GzipFile(filename="", fileobj=buf, mode="wb", mtime=0) as z:
            z.write((json.dumps(contents, indent=2, sort_keys=True) + "\n").encode())
        archive = buf.getvalue()
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-path-") as temp:
            dest = Path(temp)
            (dest / "headers.json.gz").write_bytes(archive)
            mutated = dict(manifest)
            mutated["archive_sha256"] = hashlib.sha256(archive).hexdigest()
            mutated["files"] = dict(manifest["files"])
            mutated["files"]["../escape.hpp"] = {
                "sha256": hashlib.sha256(raw).hexdigest(),
                "git_blob": git_blob(raw),
                "bytes": len(raw),
            }
            (dest / "manifest.json").write_text(json.dumps(mutated))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")


class NativeLayoutAssembly(unittest.TestCase):
    def test_exact_symbol_and_target_constants(self):
        from check_native_cpp_abi import assembly_layout_values
        emitted = "other:\n.quad 999\nabi_layout_extra:\n.quad 888\nabi_layout:\n.quad 88\n.quad 0x130 # libstdc++ size\n.quad 7\n.zero 16\nnext_symbol:\n.quad 999\n"
        self.assertEqual(assembly_layout_values(emitted, 3), [88, 304, 7])
        self.assertEqual(assembly_layout_values("_abi_layout:\n.xword 80\n.8byte 240\n", 2), [80, 240])

    def test_partial_array_is_not_layout_evidence(self):
        from check_native_cpp_abi import assembly_layout_values
        for emitted in ("abi_layout:\n.quad 80\n.zero 8\nnext:\n.quad 7\n",
                        "abi_layout_extra:\n.quad 80\n.quad 240\n"):
            with self.assertRaises(RuntimeError):
                assembly_layout_values(emitted, 2)


if __name__ == "__main__":
    unittest.main()
