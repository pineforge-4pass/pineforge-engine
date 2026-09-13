#!/usr/bin/env python3
"""Mutation controls for native C++ ABI ownership; no compiler or engine runs."""
import hashlib
import json
import shutil
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

    def test_current_command_and_preview_have_one_authority(self):
        self.reject(FILES[8], 'native_order::RequestHandle target;',
                    'native_order::RequestHandle target; std::optional<execution::SelectedOpeningSet> selected_close;')
        self.reject(FILES[8], 'std::optional<execution::Status> settlement_readiness;', '')
        self.reject(FILES[6], 'CurrentExecution = 8', 'CurrentExecution = 7')
        self.reject(FILES[6], 'Calculation = 7', 'Calculation = 9')
        self.reject(FILES[6], 'native-driver/v4', 'native-driver/v3')

    def test_current_cause_and_selected_hash_coverage(self):
        for fold in ('f.u(bind->openings.size());', 'f.u(openings->openings.size());',
                     'f.u(selected->incarnations.size());', 'f.d(point.price);',
                     'f.u(point.quote_origin_ordinal);', 'f.u(current_frame_->acceptance_cutoff);',
                     'f.u(notification.ordinal);'):
            with self.subTest(fold=fold):
                self.reject(FILES[10], fold, '')

    def test_current(self):
        check_texts(DATA)

    def test_stale_wrapper(self):
        for path, namespace, stale in (
            (FILES[0], "native_order_v4", "native_order_v1"),
            (FILES[1], "native_order_v4", "native_order_v1"),
            (FILES[11], "native_order_v1", "native_order_v4"),
            (FILES[2], "native_calendar_v2", "native_calendar_v1"),
            (FILES[3], "native_calendar_v2", "native_calendar_v3"),
            (FILES[4], "native_run_spec_v1", "native_run_spec_v2"),
            (FILES[5], "native_run_spec_v1", "native_run_spec_v2"),
            (FILES[6], "native_driver_v4", "native_driver_v2"),
            (FILES[7], "native_driver_v4", "native_driver_v3"),
            (FILES[8], "engine_script_run_v15", "engine_script_run_v12"),
            (FILES[9], "engine_script_run_v15", "engine_script_run_v12"),
            (FILES[10], "engine_script_run_v15", "engine_script_run_v12"),
        ):
            with self.subTest(path=path, namespace=namespace):
                self.reject(path, namespace, stale)

    def test_duplicate_wrapper(self):
        for path, namespace in (
            (FILES[0], "native_order_v4"),
            (FILES[2], "native_calendar_v2"),
            (FILES[4], "native_run_spec_v1"),
            (FILES[6], "native_driver_v4"),
            (FILES[8], "engine_script_run_v15"),
            (FILES[11], "native_order_v1"),
        ):
            with self.subTest(path=path):
                opening = "inline namespace " + namespace + " {"
                self.reject(path, opening, opening + " } inline namespace " + namespace + " {")

    def test_empty_namespace_is_not_ownership(self):
        for path, namespace in (
            (FILES[0], "native_order_v4"),
            (FILES[2], "native_calendar_v2"),
            (FILES[4], "native_run_spec_v1"),
            (FILES[6], "native_driver_v4"),
            (FILES[8], "engine_script_run_v15"),
            (FILES[11], "native_order_v1"),
        ):
            with self.subTest(path=path):
                self.reject(path, "inline namespace " + namespace + " {",
                            "inline namespace " + namespace + " {} namespace misplaced {")

    def test_comment_only_namespace_is_not_ownership(self):
        for path, namespace, decoy in (
            (FILES[0], "native_order_v4", "struct WorkingRequestCore"),
            (FILES[11], "native_order_v1", "struct RunIdentity"),
            (FILES[2], "native_calendar_v2", "parse_timeframe NativeInterval"),
            (FILES[8], "engine_script_run_v15", "class NativeStrategyHost"),
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
            "}  // inline namespace native_driver_v4",
            "}  // inline namespace native_driver_v4\n" + func,
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
            "}  // inline namespace native_order_v4",
            "}  // inline namespace native_order_v4\nvoid WorkingRequestCore::reset(RunIdentity) {}\n",
            1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_driver_native_run_spec_forward_must_stay_outside(self):
        self.reject(FILES[6], DRIVER_FORWARD, "")
        self.reject(
            FILES[6],
            DRIVER_FORWARD + "\ninline namespace native_driver_v4 {",
            "inline namespace native_driver_v4 {\n" + DRIVER_FORWARD)
        self.reject(
            FILES[6],
            DRIVER_FORWARD,
            "inline namespace native_run_spec_v1 { struct NativeRunSpec {}; }")

    def test_host_public_values_cannot_leave_v15(self):
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
            'kNativeConsumerSemanticVersion = "native-consumer/v6"',
            'kNativeConsumerSemanticVersion = "native-consumer/v3"')

    def test_phase0_native_abi_templates_are_staged(self):
        from check_native_cpp_abi import (
            CURRENT_EXECUTION_V15_CALLER, NATIVE_FX_CURVE_CALLER,
            CURRENT_TERMS_SURFACE_READY, control_applicability,
        )
        self.assertFalse(CURRENT_TERMS_SURFACE_READY)
        self.assertIn('R4B_CURRENT_RESULT_ALTERNATIVES', CURRENT_EXECUTION_V15_CALLER)
        self.assertIn('configure_native_fx_curve', CURRENT_EXECUTION_V15_CALLER)
        self.assertIn('validate_native_fx_curve', NATIVE_FX_CURVE_CALLER)
        controls = {row['name']: row for row in control_applicability()}
        self.assertEqual(controls['v14_current_execution_shape_agnostic_compile']['status'], 'required')
        for name in ('v15_current_execution_surface_compile',
                     'v15_current_result_missing_cancelled_compile_reject',
                     'v15_native_fx_curve_surface_compile'):
            self.assertEqual(controls[name]['status'], 'pending_surface')

    def test_order_namespace_is_derived_not_literal(self):
        from check_native_cpp_abi import current_order_namespace
        self.assertEqual(current_order_namespace(
            'inline namespace native_order_v4 { struct X {}; }'), 'native_order_v4')
        with self.assertRaises(RuntimeError):
            current_order_namespace(
                'inline namespace native_order_v4 { }\n'
                'inline namespace native_order_v3 { }')

    def test_missing_cancelled_mutation_is_exactly_one(self):
        from check_native_cpp_abi import remove_current_result_cancelled
        source = ('using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal, '
                  'native_order::ExecutionAppliedEvent, native_order::NoEffectEvent, '
                  'native_order::MatchRejectedEvent, native_order::CancelledEvent>;\n')
        changed = remove_current_result_cancelled(source)
        prefix, marker, suffix = source.partition(', native_order::CancelledEvent')
        self.assertTrue(marker)
        self.assertEqual(changed, prefix + suffix)
        self.assertNotIn('CancelledEvent', changed)
        with self.assertRaises(RuntimeError):
            remove_current_result_cancelled(source.replace('CancelledEvent', 'NoEffectEvent'))
        with self.assertRaises(RuntimeError):
            remove_current_result_cancelled(source.replace(
                'native_order::CancelledEvent>;',
                'native_order::CancelledEvent, native_order::CancelledEvent>;'))

    def test_compile_rejection_requires_named_diagnostic(self):
        from check_native_cpp_abi import (
            CURRENT_EXECUTION_V15_CALLER, expect_compile_rejection,
            remove_current_result_cancelled,
        )
        self.assertIn('R4B_CURRENT_RESULT_ALTERNATIVES', CURRENT_EXECUTION_V15_CALLER)
        compiler = shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.fail('the named compile-rejection mirror requires a C++ compiler')
        with tempfile.TemporaryDirectory(prefix='pf-native-reject-mirror-') as temp:
            root = Path(temp)
            include = root / 'include'
            header = include / 'pineforge/native_host.hpp'
            header.parent.mkdir(parents=True)
            original = ('#include <variant>\nnamespace pineforge { struct NativeCurrentRefusal {}; '
                        'namespace native_order { struct ExecutionAppliedEvent {}; '
                        'struct NoEffectEvent {}; struct MatchRejectedEvent {}; struct CancelledEvent {}; }\n'
                        'using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal, '
                        'native_order::ExecutionAppliedEvent, native_order::NoEffectEvent, '
                        'native_order::MatchRejectedEvent, native_order::CancelledEvent>; }\n')
            header.write_text(original)
            mutated = remove_current_result_cancelled(original)
            header.write_text(mutated)
            source = ('#include <pineforge/native_host.hpp>\n#include <variant>\n'
                      'static_assert(std::variant_size_v<pineforge::NativeCurrentExecutionResult> == 5, '
                      '"R4B_CURRENT_RESULT_ALTERNATIVES");\n')
            receipt = expect_compile_rejection(
                'named-negative', source, include,
                compiler_flags=[compiler, '-std=c++17'], generated_include=str(root),
                scratch=root, original_header_sha256=hashlib.sha256(
                    original.encode()).hexdigest())
            self.assertEqual(receipt['outcome'], 'expected_compile_rejection')
            self.assertIn('R4B_CURRENT_RESULT_ALTERNATIVES', receipt['diagnostics'])
            self.assertEqual(receipt['source_sha256'], hashlib.sha256(source.encode()).hexdigest())
            self.assertEqual(receipt['original_header_sha256'], hashlib.sha256(
                original.encode()).hexdigest())
            self.assertEqual(receipt['header_sha256'], hashlib.sha256(
                mutated.encode()).hexdigest())
            header.write_text(original)
            with self.assertRaises(RuntimeError):
                expect_compile_rejection(
                    'unexpected-success', source, include,
                    compiler_flags=[compiler, '-std=c++17'], generated_include=str(root),
                    scratch=root, original_header_sha256='x')
            bad = source.replace('static_assert', 'static_assertion', 1)
            header.write_text(mutated)
            with self.assertRaises(RuntimeError):
                expect_compile_rejection(
                    'unnamed-negative', bad, include,
                    compiler_flags=[compiler, '-std=c++17'], generated_include=str(root),
                    scratch=root, original_header_sha256='x')

    def test_v14_tar_authentication_rejects_archive_and_manifest_tampering(self):
        from check_native_cpp_abi import FIXTURE, authenticate_v14_fixture
        fixture = FIXTURE / 'host-f736676'
        with tempfile.TemporaryDirectory(prefix='pf-native-v14-auth-') as temp:
            root = Path(temp)
            valid = root / 'valid'
            manifest = root / 'manifest-copy'
            shutil.copytree(fixture, manifest)
            authenticate_v14_fixture(manifest, valid)
            tampered = root / 'tampered'
            shutil.copytree(fixture, tampered)
            archive = bytearray((tampered / 'headers.tar').read_bytes())
            archive[-1] ^= 1
            (tampered / 'headers.tar').write_bytes(archive)
            with self.assertRaises(RuntimeError):
                authenticate_v14_fixture(tampered, root / 'bad-archive')
            mislabeled = root / 'mislabeled'
            shutil.copytree(fixture, mislabeled)
            data = json.loads((mislabeled / 'manifest.json').read_text())
            data['tree'] = '0' * 40
            (mislabeled / 'manifest.json').write_text(json.dumps(data))
            with self.assertRaises(RuntimeError):
                authenticate_v14_fixture(mislabeled, root / 'bad-manifest')

    def test_current_execution_caller_is_rendered_per_provider(self):
        from check_native_cpp_abi import render_current_execution_caller
        v14 = render_current_execution_caller('engine_script_run_v14')
        v15 = render_current_execution_caller('engine_script_run_v15')
        self.assertIn('engine_script_run_v14', v14)
        self.assertNotIn('engine_script_run_v15', v14)
        self.assertIn('engine_script_run_v15', v15)
        self.assertNotIn('engine_script_run_v14', v15)
        with self.assertRaises(RuntimeError):
            render_current_execution_caller('engine_script_run_v13')


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
