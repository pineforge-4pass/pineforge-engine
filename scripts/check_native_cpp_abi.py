#!/usr/bin/env python3
"""Compile/link-only checks for standalone native C++ ABI pairing.

Frozen header closures are tracked fixtures: no Git history or network is needed.
Every translation unit must compile before expected linker failures are tested.
No produced executable is run. C ABI checks are separate.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

from check_native_cpp_versions import FILES, check as check_native_versions

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/native_cpp_abi"

ORDER_COMMIT = "1acaf33e0340f6ae09d9529a3b6a0c12742dcb0d"
ORDER_TREE = "ced31532e86bd41bc8753330ea0abcf379fdc05f"
ORDER_HEADER_SHA = "ff32dd1b9a34bc6868b86c8dc14d27513e5ea2a710c7657c895662bcde65ee71"
DRAFT_HEADER_SHA = "a8e34c127b3d8d95f5556cf2e99fbc3d93c68c85f607a0e5e3e044b904193291"
DRIVER_COMMIT = "08b5c8868eadd3022a62b32e7e2cabe0640e75f4"
DRIVER_TREE = "3482d155df5fe91c5dfd4a05fdfb506641d0ee19"
SPEC_COMMIT = "262a28013ed277990349e845ba0554a63f6fbc79"
SPEC_TREE = "fb00ed9ba02cede2fe08ee8113ae32f71fd26b00"
CAL262_HEADER_SHA = "c47b3ee15d69587879cca8f491e73880cddae1ce3e7dfd0d1f4db16dfb8fcf4b"
SPEC_HEADER_SHA = "b263a8bf3a68f58201c968ffa9ec8b54633fe3c752332df164ea11cf4bce3c7c"
V12_COMMIT = "e7d023dbdff1c98229155ec5bcdd1e4ac534f5fb"
V12_TREE = "0201bf052429490fb453bbfd6037e5afd1669626"
HOST_V12_HEADER_SHA = "871865715f084a0054c9d8e220cb9b957318bfdc0a0e100765d1cda85d7944b3"
ORDER_V1_HEADER_SHA = "b13006e99554ba9caa3e5b444cca3e4f2b5ebd5e372d3dcbe44bb6a677192a3e"

CURRENT_ORDER_VARIANT = 16
CURRENT_CALENDAR_INTERVAL = 40
CURRENT_COORDINATE = 80

ORDER_CALLER = '''#include <pineforge/native_order.hpp>
int main() {
    using namespace pineforge::native_order;
    WorkingRequestCore core({"abi-witness", 1});
    uint64_t incarnation = 1, ordinal = 1;
    Request request{pineforge::execution::Flatten{}, "", ""};
    auto submitted = core.submit(request, 0, incarnation, ordinal);
    return int(core.history().size() + unsigned(submitted.status));
}
'''
CALENDAR_CALLER = '''#include <pineforge/native_calendar.hpp>
int main() {
    using namespace pineforge::native_calendar;
    auto timeframe = parse_timeframe("1");
    auto calendar = parse_session("24x7", "UTC");
    auto key = period_key(*calendar, *timeframe, 0);
    auto interval = interval_containing(*calendar, *timeframe, 0);
    (void) key;
    return interval ? 0 : 1;
}
'''
PARSE_TIMEFRAME_CALLER = '''#include <pineforge/native_calendar.hpp>
int main() {
    auto tf = pineforge::native_calendar::parse_timeframe("1");
    return tf ? 0 : 1;
}
'''
DESCRIPTOR_CALLER = '''#include <pineforge/native_calendar.hpp>
int main() {
    auto d = pineforge::native_calendar::timezone_identity_descriptor("UTC");
    return d && d->valid() ? 0 : 1;
}
'''
SPEC_CALLER = '''#include <pineforge/native_run_spec.hpp>
int main() {
    pineforge::NativeRunSpec spec;
    auto v = pineforge::validate_native_run_spec(spec);
    auto n = pineforge::normalize_native_run_spec(spec);
    return int(v.error) + int(n.error);
}
'''
BAR_CALLER = '''#include <pineforge/market_driver.hpp>
int main() {
    pineforge::Bar bar{};
    return pineforge::native_bar_structurally_valid(bar) ? 0 : 1;
}
'''
PREFLIGHT_CALLER = '''#include <pineforge/market_driver.hpp>
#include <pineforge/native_run_spec.hpp>
int main() {
    pineforge::NativeRunSpec spec;
    auto r = pineforge::preflight_native_inputs(spec, nullptr, 0,
        pineforge::NativeInputPolicy::Batch);
    return int(r.error);
}
'''
# Labeled consumer-defined type seam. Not a production export.
COORDINATE_PROVIDER = '''#include <pineforge/market_driver.hpp>
long long abi_accept_coordinate(const pineforge::NativeCoordinate& c) {
    return c.last_traded_close_ms;
}
long long abi_accept_decision(const pineforge::NativeDecisionContext& d) {
    return d.decision_floor_ms;
}
'''
COORDINATE_CALLER = '''#include <pineforge/market_driver.hpp>
long long abi_accept_coordinate(const pineforge::NativeCoordinate&);
long long abi_accept_decision(const pineforge::NativeDecisionContext&);
int main() {
    pineforge::NativeCoordinate c{};
    pineforge::NativeDecisionContext d{};
    return int(abi_accept_coordinate(c) + abi_accept_decision(d));
}
'''
HOST_CALLER = '''#include <pineforge/native_host.hpp>
#include <type_traits>
static_assert(std::is_same_v<pineforge::NativeStrategyHost,
    pineforge::engine_script_run_v13::NativeStrategyHost>);
static_assert(std::is_same_v<pineforge::NativeStateView,
    pineforge::engine_script_run_v13::NativeStateView>);
static_assert(std::is_same_v<pineforge::NativeFailure,
    pineforge::engine_script_run_v13::NativeFailure>);
static_assert(std::is_trivially_copyable_v<pineforge::NativeFailure>);
static_assert(std::is_trivially_copyable_v<pineforge::NativeFailureContext>);
int main(int argc, char** argv) {
    auto* host = reinterpret_cast<pineforge::NativeStrategyHost*>(argv);
    auto state = host->native_state();
    return int(state.kind);
}
'''
# Return-only observation: native_events() return layout is not in the symbol.
HOST_EVENTS_CALLER = '''#include <pineforge/native_host.hpp>
#include <type_traits>
static_assert(std::is_same_v<pineforge::NativeStrategyHost,
    pineforge::engine_script_run_v13::NativeStrategyHost>);
int main(int argc, char** argv) {
    auto* host = reinterpret_cast<pineforge::NativeStrategyHost*>(argv);
    auto events = host->native_events(0);
    return int(events.size());
}
'''
OLD_HOST_EVENTS_CALLER = '''#include <pineforge/native_host.hpp>
#include <type_traits>
static_assert(std::is_same_v<pineforge::NativeStrategyHost,
    pineforge::engine_script_run_v12::NativeStrategyHost>);
int main(int argc, char** argv) {
    auto* host = reinterpret_cast<pineforge::NativeStrategyHost*>(argv);
    auto events = host->native_events(0);
    return int(events.size());
}
'''
HOST_EVENTS_SYMBOL_CONTROL = '''#include <pineforge/native_host.hpp>
namespace pineforge {
std::vector<NativeMarketEvent> NativeStrategyHost::native_events(uint64_t) const { return {}; }
}
'''
# Labeled minimal symbol control. Not a historical NativeRunSpec runtime.
SPEC_SYMBOL_CONTROL = '''#include <pineforge/native_run_spec.hpp>
namespace pineforge {
NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec&) noexcept { return {}; }
NativeRunSpecValidation normalize_native_run_spec(NativeRunSpec&) noexcept { return {}; }
}
'''


def sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def git_blob(raw: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()


def frozen_contents(fixture: Path, manifest: dict) -> dict:
    archive = (fixture / manifest["archive"]).read_bytes()
    if sha256(archive) != manifest["archive_sha256"]:
        raise RuntimeError(str(fixture) + " archive digest mismatch")
    contents = json.loads(gzip.decompress(archive))
    if contents.keys() != manifest["files"].keys():
        raise RuntimeError(str(fixture) + " file set mismatch")
    return contents


def authenticate_fixture(fixture: Path, destination: Path, expected: dict | None = None) -> dict:
    manifest = json.loads((fixture / "manifest.json").read_text())
    if expected:
        for key, value in expected.items():
            if manifest.get(key) != value:
                raise RuntimeError(str(fixture) + " does not identify the pinned contract: " + key)
    contents = frozen_contents(fixture, manifest)
    for name, content in contents.items():
        relative = Path(name)
        if (relative.is_absolute() or ".." in relative.parts
                or relative.parts[0] not in ("pineforge", "src")):
            raise RuntimeError("invalid native ABI fixture path: " + name)
        raw = content.encode()
        info = manifest["files"][name]
        if (sha256(raw) != info["sha256"] or len(raw) != info["bytes"]
                or git_blob(raw) != info["git_blob"]):
            raise RuntimeError("native ABI fixture digest mismatch: " + name)
        if relative.parts[0] == "pineforge":
            path = destination / "include" / relative
        else:
            path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    return manifest


def diagnostic_text(completed: subprocess.CompletedProcess[str]) -> str:
    return (completed.stdout or "") + (completed.stderr or "")


def undefined_mentions(text: str, symbol: str) -> bool:
    if "undefined" not in text.lower():
        return False
    return symbol in text


def assembly_layout_values(text: str, expected: int) -> list[int]:
    """Read compiler-emitted uint64 constants without executing a binary."""
    active = False
    values: list[int] = []
    for line in text.splitlines():
        if re.match(r"^\s*_?abi_layout:\s*(?:[#;].*)?$", line):
            active = True
            continue
        if not active:
            continue
        match = re.match(r"^\s*\.(?:quad|xword|8byte)\s+([^#;]+)", line)
        if match:
            values.append(int(match.group(1).strip(), 0))
            if len(values) == expected:
                return values
        elif re.match(r"^\s*(?:[.\w]+:|\.(?:zero|space|ascii|asciz|byte)\b)", line):
            break
    raise RuntimeError("compiler assembly did not expose the complete abi_layout array")


def order_layout_source(variant_size: int) -> str:
    return f'''#include <pineforge/native_order.hpp>
#include <cstddef>
#include <variant>
using namespace pineforge::native_order;
static_assert(std::variant_size_v<CommandEvent> == {variant_size});
extern "C" const unsigned long long abi_layout[] = {{
    sizeof(WorkingRequestCore), sizeof(CommandEvent), std::variant_size_v<CommandEvent>,
    sizeof(Request), sizeof(SubmitResult), sizeof(LiveRequest)}};
'''


def calendar_layout_source(layout: dict) -> str:
    return f'''#include <pineforge/native_calendar.hpp>
#include <cstddef>
#include <type_traits>
#include <utility>
using namespace pineforge::native_calendar;
static_assert(sizeof(NativeInterval) == {layout["NativeInterval_sizeof"]});
static_assert(sizeof(std::optional<NativeInterval>) == {layout["optional_NativeInterval_sizeof"]});
static_assert(sizeof(decltype(period_key(std::declval<SessionCalendar>(), std::declval<Timeframe>(), 0)))
    == {layout["period_key_return_sizeof"]});
extern "C" const unsigned long long abi_layout[] = {{
    sizeof(Timeframe), sizeof(std::optional<Timeframe>), sizeof(SessionCalendar),
    sizeof(NativeInterval), sizeof(std::optional<NativeInterval>),
    offsetof(NativeInterval, last_traded_close_ms),
    sizeof(decltype(period_key(std::declval<SessionCalendar>(), std::declval<Timeframe>(), 0)))}};
'''


def driver_layout_source(layout: dict) -> str:
    return f'''#include <pineforge/market_driver.hpp>
#include <cstddef>
static_assert(sizeof(pineforge::NativeCoordinate) == {layout["NativeCoordinate_sizeof"]});
static_assert(sizeof(pineforge::NativeDriverPoint) == {layout["NativeDriverPoint_sizeof"]});
static_assert(sizeof(pineforge::NativeDecisionContext) == {layout["NativeDecisionContext_sizeof"]});
static_assert(offsetof(pineforge::NativeCoordinate, last_traded_close_ms)
    == {layout["NativeCoordinate_last_traded_close_ms_offset"]});
extern "C" const unsigned long long abi_layout[] = {{
    sizeof(pineforge::NativeCoordinate), sizeof(pineforge::NativeDriverPoint),
    sizeof(pineforge::NativeDecisionContext),
    offsetof(pineforge::NativeCoordinate, last_traded_close_ms)}};
'''


def current_order_layout() -> str:
    # std::string/variant object sizes differ between libc++ and libstdc++.
    # The alternative count is invariant; record actual target sizes below.
    return order_layout_source(CURRENT_ORDER_VARIANT)


def current_calendar_layout() -> str:
    return calendar_layout_source({
        "NativeInterval_sizeof": CURRENT_CALENDAR_INTERVAL,
        "optional_NativeInterval_sizeof": 48,
        "period_key_return_sizeof": 16,
        "Timeframe_sizeof": 40,
        "optional_Timeframe_sizeof": 48,
        "SessionCalendar_sizeof": 88,
        "NativeInterval_last_traded_close_ms_offset": 16,
    })


def current_driver_layout() -> str:
    return driver_layout_source({
        "NativeCoordinate_sizeof": CURRENT_COORDINATE,
        "NativeDriverPoint_sizeof": 112,
        "NativeDecisionContext_sizeof": 168,
        "NativeCoordinate_last_traded_close_ms_offset": 32,
    })


def nm_symbols(obj: Path) -> str:
    listed = subprocess.run(["nm", "-g", "-C", str(obj)], capture_output=True, text=True)
    if listed.returncode:
        return listed.stderr or listed.stdout
    return listed.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--generated-include", required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()

    include = Path(args.include).resolve()
    source_root = include.parent
    library = Path(args.library).resolve()
    if not library.is_file():
        raise RuntimeError("library is not a file: " + str(library))
    lib_mtime = library.stat().st_mtime
    stale = [name for name in FILES
             if (source_root / name).stat().st_mtime > lib_mtime]
    if stale:
        raise RuntimeError("library older than tracked source: " + ", ".join(stale))
    check_native_versions(source_root)

    compiler_id = subprocess.run(
        [args.compiler, "--version"], capture_output=True, text=True, timeout=30)
    if compiler_id.returncode:
        raise RuntimeError("compiler --version failed:\n" + diagnostic_text(compiler_id))
    clang = "clang" in (compiler_id.stdout + compiler_id.stderr).lower()

    # Literal diagnostic controls: old unversioned needles must not match versioned names.
    controls = (
        ("pineforge::native_calendar::parse_timeframe(",
         "pineforge::native_calendar::native_calendar_v2::parse_timeframe("),
        ("pineforge::native_calendar::timezone_identity_descriptor(",
         "pineforge::native_calendar::native_calendar_v2::timezone_identity_descriptor("),
        ("pineforge::native_order::WorkingRequestCore::submit(",
         "pineforge::native_order::native_order_v1::WorkingRequestCore::submit("),
        ("pineforge::native_order::native_order_v1::WorkingRequestCore::submit(",
         "pineforge::native_order::native_order_v2::WorkingRequestCore::submit("),
        ("pineforge::validate_native_run_spec(",
         "pineforge::native_run_spec_v1::validate_native_run_spec("),
        ("pineforge::native_bar_structurally_valid(",
         "pineforge::native_driver_v3::native_bar_structurally_valid("),
        ("abi_accept_coordinate(pineforge::NativeCoordinate",
         "abi_accept_coordinate(pineforge::native_driver_v3::NativeCoordinate"),
        ("pineforge::engine_script_run_v12::NativeStrategyHost::native_events(",
         "pineforge::engine_script_run_v13::NativeStrategyHost::native_events("),
    )
    for old, new in controls:
        if old in new:
            raise RuntimeError("old ABI needle is a substring of the versioned name: " + old)
        fake_old = "Undefined symbols\n  \"" + old + "int)\""
        fake_new = "undefined reference to `" + new + "int)'"
        if not undefined_mentions(fake_old, old) or undefined_mentions(fake_new, old):
            raise RuntimeError("link diagnostics confuse old and versioned symbols: " + old)
    print("qualified-symbol diagnostic controls passed")

    receipt = {
        "library": str(library),
        "library_sha256": sha256(library.read_bytes()),
        "library_mtime": lib_mtime,
        "compiler": (compiler_id.stdout or compiler_id.stderr).splitlines()[0],
        "current_namespaces": {
            "native_order": "pineforge::native_order::native_order_v1",
            "native_calendar": "pineforge::native_calendar::native_calendar_v2",
            "native_run_spec": "pineforge::native_run_spec_v1",
            "native_driver": "pineforge::native_driver_v3",
            "native_host": "pineforge::engine_script_run_v13",
        },
        "executable_runs": 0,
        "compiles": [],
        "links": [],
        "layouts": {},
        "symbols": {},
        "fixtures": {},
    }

    common = [args.compiler, "-std=c++17", "-O0", *args.extra_flag]

    with tempfile.TemporaryDirectory(prefix="pf-native-cpp-abi-") as temporary:
        root = Path(temporary)
        pins = {
            "order-1acaf33": {
                "source_commit": ORDER_COMMIT, "source_tree": ORDER_TREE,
            },
            "calendar-draft-a8e34c": {"content_sha256": DRAFT_HEADER_SHA},
            "calendar-262a280": {
                "source_commit": SPEC_COMMIT, "source_tree": SPEC_TREE,
            },
            "driver-08b5c88": {
                "source_commit": DRIVER_COMMIT, "source_tree": DRIVER_TREE,
            },
            "run-spec-262a280": {
                "source_commit": SPEC_COMMIT, "source_tree": SPEC_TREE,
            },
            "host-e7d023d": {
                "source_commit": V12_COMMIT, "source_tree": V12_TREE,
            },
            "order-e7d023d": {
                "source_commit": V12_COMMIT, "source_tree": V12_TREE,
            },
        }
        unpacked = {}
        for name, expected in pins.items():
            dest = root / name
            manifest = authenticate_fixture(FIXTURE / name, dest, expected)
            if name == "calendar-draft-a8e34c":
                header = dest / "include/pineforge/native_calendar.hpp"
                if sha256(header.read_bytes()) != DRAFT_HEADER_SHA:
                    raise RuntimeError("draft calendar header content SHA mismatch")
            if name == "calendar-262a280":
                header = dest / "include/pineforge/native_calendar.hpp"
                if sha256(header.read_bytes()) != CAL262_HEADER_SHA:
                    raise RuntimeError("262a280 calendar header content SHA mismatch")
            if name == "order-1acaf33":
                header = dest / "include/pineforge/native_order.hpp"
                if sha256(header.read_bytes()) != ORDER_HEADER_SHA:
                    raise RuntimeError("1acaf33 order header content SHA mismatch")
            if name == "run-spec-262a280":
                header = dest / "include/pineforge/native_run_spec.hpp"
                if sha256(header.read_bytes()) != SPEC_HEADER_SHA:
                    raise RuntimeError("262a280 run-spec header content SHA mismatch")
            if name == "host-e7d023d":
                header = dest / "include/pineforge/native_host.hpp"
                if sha256(header.read_bytes()) != HOST_V12_HEADER_SHA:
                    raise RuntimeError("e7d023d host header content SHA mismatch")
            if name == "order-e7d023d":
                header = dest / "include/pineforge/native_order.hpp"
                if sha256(header.read_bytes()) != ORDER_V1_HEADER_SHA:
                    raise RuntimeError("e7d023d order-v1 header content SHA mismatch")
            unpacked[name] = dest
            receipt["fixtures"][name] = {
                "source_commit": manifest.get("source_commit"),
                "source_tree": manifest.get("source_tree"),
                "content_sha256": manifest.get("content_sha256"),
                "archive_sha256": manifest["archive_sha256"],
            }

        def compile_object(name, source, include_path, extra_source_dir=None):
            path = root / (name + ".cpp")
            path.write_text(source)
            obj = root / (name + ".o")
            argv = [*common, "-I", str(include_path), "-I", args.generated_include]
            if extra_source_dir is not None:
                argv.extend(["-I", str(extra_source_dir)])
            argv.extend(["-c", str(path), "-o", str(obj)])
            compiled = subprocess.run(argv, capture_output=True, text=True, timeout=90)
            if compiled.returncode:
                raise RuntimeError(name + " failed to compile (not a pairing rejection):\n"
                                   + diagnostic_text(compiled))
            receipt["compiles"].append({
                "name": name, "exit": compiled.returncode,
                "source_sha256": sha256(source.encode()),
                "object_sha256": sha256(obj.read_bytes()),
            })
            return obj

        def compile_file(name, source_path, include_path):
            obj = root / (name + ".o")
            compiled = subprocess.run(
                [*common, "-I", str(include_path), "-I", args.generated_include,
                 "-c", str(source_path), "-o", str(obj)],
                capture_output=True, text=True, timeout=90)
            if compiled.returncode:
                raise RuntimeError(name + " failed to compile (not a pairing rejection):\n"
                                   + diagnostic_text(compiled))
            receipt["compiles"].append({
                "name": name, "exit": compiled.returncode,
                "source_sha256": sha256(Path(source_path).read_bytes()),
                "object_sha256": sha256(obj.read_bytes()),
            })
            return obj

        def record_layout(name, include_path):
            receipt["layouts"][name] = {"method": "static_assert"}
            if not clang:
                assembly = root / (name + ".s")
                src = root / (name + ".cpp")
                emitted = subprocess.run(
                    [*common, "-I", str(include_path), "-I", args.generated_include,
                     "-S", str(src), "-o", str(assembly)],
                    capture_output=True, text=True, timeout=90)
                if emitted.returncode:
                    raise RuntimeError(name + " could not emit layout assembly:\n" + diagnostic_text(emitted))
                words = 6 if "order" in name else 7 if "calendar" in name else 4
                receipt["layouts"][name] = {
                    "method": "assembly",
                    "abi_layout": assembly_layout_values(assembly.read_text(), words),
                    "assembly_sha256": sha256(assembly.read_bytes()),
                }
                return
            ll = root / (name + ".ll")
            src = root / (name + ".cpp")
            emitted = subprocess.run(
                [*common, "-I", str(include_path), "-I", args.generated_include,
                 "-S", "-emit-llvm", str(src), "-o", str(ll)],
                capture_output=True, text=True, timeout=90)
            if emitted.returncode:
                receipt["layouts"][name]["llvm"] = "unavailable"
                return
            receipt["layouts"][name] = {
                "method": "llvm",
                "abi_layout": [line for line in ll.read_text().splitlines()
                               if line.startswith("@abi_layout")],
            }

        def link(name, objects, runtime, missing=None):
            out = root / name
            argv = [*common, *[str(x) for x in objects], str(runtime), "-pthread", "-o", str(out)]
            linked = subprocess.run(argv, capture_output=True, text=True, timeout=120)
            text = diagnostic_text(linked)
            if missing is None:
                if linked.returncode:
                    raise RuntimeError(name + " positive control failed to link:\n" + text)
                print(name + ": linked (not executed)")
                receipt["links"].append({
                    "name": name, "outcome": "linked", "exit": linked.returncode,
                    "not_executed": True,
                })
                return
            if linked.returncode == 0:
                raise RuntimeError(name + " stale C++ pairing unexpectedly linked")
            needles = missing if isinstance(missing, (list, tuple)) else [missing]
            if any(not undefined_mentions(text, needle) for needle in needles):
                raise RuntimeError(name + " failed for an unexpected reason:\n" + text)
            print(name + ": rejected missing " + ", ".join(needles) + " (not executed)")
            receipt["links"].append({
                "name": name, "outcome": "expected_rejection",
                "exit": linked.returncode, "missing": list(needles),
                "not_executed": True,
            })

        # Phase 1: compile every caller, layout TU, and old object before any link.
        current_order = compile_object("current_order_caller", ORDER_CALLER, include)
        current_calendar = compile_object("current_calendar_caller", CALENDAR_CALLER, include)
        current_parse = compile_object("current_parse_timeframe_caller", PARSE_TIMEFRAME_CALLER, include)
        current_descriptor = compile_object("current_descriptor_caller", DESCRIPTOR_CALLER, include)
        current_spec = compile_object("current_spec_caller", SPEC_CALLER, include)
        current_bar = compile_object("current_bar_caller", BAR_CALLER, include)
        current_preflight = compile_object("current_preflight_caller", PREFLIGHT_CALLER, include)
        current_coordinate = compile_object("current_coordinate_caller", COORDINATE_CALLER, include)
        current_coordinate_provider = compile_object(
            "current_coordinate_provider", COORDINATE_PROVIDER, include)
        current_host = compile_object("current_host_caller", HOST_CALLER, include)
        current_host_events = compile_object("current_host_events_caller", HOST_EVENTS_CALLER, include)

        old_order_include = unpacked["order-1acaf33"] / "include"
        old_calendar_include = unpacked["calendar-draft-a8e34c"] / "include"
        old_desc_include = unpacked["calendar-262a280"] / "include"
        old_driver_include = unpacked["driver-08b5c88"] / "include"
        old_spec_include = unpacked["run-spec-262a280"] / "include"
        old_host_include = unpacked["host-e7d023d"] / "include"
        old_order_v1_include = unpacked["order-e7d023d"] / "include"

        old_order = compile_object("old_order_caller", ORDER_CALLER, old_order_include)
        old_calendar = compile_object("old_calendar_caller", CALENDAR_CALLER, old_calendar_include)
        old_parse = compile_object("old_parse_timeframe_caller", PARSE_TIMEFRAME_CALLER, old_calendar_include)
        old_descriptor = compile_object("old_descriptor_caller", DESCRIPTOR_CALLER, old_desc_include)
        old_spec = compile_object("old_spec_caller", SPEC_CALLER, old_spec_include)
        old_bar = compile_object("old_bar_caller", BAR_CALLER, old_driver_include)
        old_coordinate = compile_object("old_coordinate_caller", COORDINATE_CALLER, old_driver_include)
        old_coordinate_provider = compile_object(
            "old_coordinate_provider", COORDINATE_PROVIDER, old_driver_include)

        old_order_obj = compile_file(
            "old_order_object", unpacked["order-1acaf33"] / "src/native_order.cpp", old_order_include)
        old_calendar_obj = compile_file(
            "old_calendar_object",
            unpacked["calendar-draft-a8e34c"] / "src/native_calendar.cpp",
            old_calendar_include)
        old_timezone_obj = compile_file(
            "old_timezone_object",
            unpacked["calendar-draft-a8e34c"] / "src/timezone.cpp",
            old_calendar_include)
        old_bar_obj = compile_file(
            "old_bar_object", unpacked["driver-08b5c88"] / "src/market_driver.cpp", old_driver_include)
        old_spec_symbols = compile_object(
            "old_spec_symbol_control", SPEC_SYMBOL_CONTROL, old_spec_include)
        old_host_events = compile_object(
            "old_host_events_caller", OLD_HOST_EVENTS_CALLER, old_host_include)
        old_host_events_symbols = compile_object(
            "old_host_events_symbol_control", HOST_EVENTS_SYMBOL_CONTROL, old_host_include)
        old_order_v1 = compile_object("old_order_v1_caller", ORDER_CALLER, old_order_v1_include)
        old_order_v1_obj = compile_file(
            "old_order_v1_object",
            unpacked["order-e7d023d"] / "src/native_order.cpp",
            old_order_v1_include)

        old_order_layout = compile_object(
            "old_order_layout",
            order_layout_source(7),
            old_order_include)
        old_calendar_layout = compile_object(
            "old_calendar_layout",
            calendar_layout_source(json.loads((FIXTURE / "calendar-draft-a8e34c/manifest.json").read_text())["layout"]),
            old_calendar_include)
        old_driver_layout = compile_object(
            "old_driver_layout",
            driver_layout_source(json.loads((FIXTURE / "driver-08b5c88/manifest.json").read_text())["layout"]),
            old_driver_include)
        now_order_layout = compile_object("current_order_layout", current_order_layout(), include)
        now_calendar_layout = compile_object("current_calendar_layout", current_calendar_layout(), include)
        now_driver_layout = compile_object("current_driver_layout", current_driver_layout(), include)
        record_layout("old_order_layout", old_order_include)
        record_layout("old_calendar_layout", old_calendar_include)
        record_layout("old_driver_layout", old_driver_include)
        record_layout("current_order_layout", include)
        record_layout("current_calendar_layout", include)
        record_layout("current_driver_layout", include)

        for name, obj in (
            ("old_order_caller", old_order),
            ("old_calendar_caller", old_calendar),
            ("old_parse_timeframe_caller", old_parse),
            ("old_descriptor_caller", old_descriptor),
            ("old_bar_caller", old_bar),
            ("old_coordinate_caller", old_coordinate),
            ("current_order_caller", current_order),
            ("current_calendar_caller", current_calendar),
            ("current_parse_timeframe_caller", current_parse),
            ("current_descriptor_caller", current_descriptor),
            ("current_bar_caller", current_bar),
            ("old_order_object", old_order_obj),
            ("old_calendar_object", old_calendar_obj),
            ("old_bar_object", old_bar_obj),
            ("current_coordinate_provider", current_coordinate_provider),
        ):
            receipt["symbols"][name] = [
                line for line in nm_symbols(obj).splitlines()
                if re.search(r"\b[UT]\b", line) and "pineforge" in line
            ]

        # Phase 2: matching controls, then expected mismatch rejections.
        link("current_order_to_current_library", [current_order], library)
        link("current_calendar_to_current_library", [current_calendar], library)
        link("current_parse_timeframe_to_current_library", [current_parse], library)
        link("current_descriptor_to_current_library", [current_descriptor], library)
        link("current_spec_to_current_library", [current_spec], library)
        link("current_bar_to_current_library", [current_bar], library)
        link("current_preflight_to_current_library", [current_preflight], library)
        link("current_host_to_current_library", [current_host], library)
        link("current_host_events_to_current_library", [current_host_events], library)
        link("current_coordinate_to_current_provider",
             [current_coordinate], current_coordinate_provider)

        link("old_order_to_old_object", [old_order], old_order_obj)
        link("old_calendar_to_old_objects", [old_calendar, old_timezone_obj], old_calendar_obj)
        link("old_parse_timeframe_to_old_objects", [old_parse, old_timezone_obj], old_calendar_obj)
        link("old_spec_to_old_symbol_control", [old_spec], old_spec_symbols)
        link("old_host_events_to_old_symbols", [old_host_events], old_host_events_symbols)
        link("old_order_v1_to_old_object", [old_order_v1], old_order_v1_obj)
        link("old_bar_to_old_object", [old_bar], old_bar_obj)
        link("old_coordinate_to_old_provider", [old_coordinate], old_coordinate_provider)

        link("old_order_to_current_library", [old_order], library,
             "pineforge::native_order::WorkingRequestCore::submit(")
        link("old_calendar_to_current_library", [old_calendar], library,
             ["pineforge::native_calendar::parse_timeframe(",
              "pineforge::native_calendar::period_key("])
        link("old_parse_timeframe_to_current_library", [old_parse], library,
             "pineforge::native_calendar::parse_timeframe(")
        link("old_descriptor_to_current_library", [old_descriptor], library,
             "pineforge::native_calendar::timezone_identity_descriptor(")
        link("old_spec_to_current_library", [old_spec], library,
             "pineforge::validate_native_run_spec(")
        link("old_host_events_to_current_library", [old_host_events], library,
             "pineforge::engine_script_run_v12::NativeStrategyHost::native_events(")
        link("old_bar_to_current_library", [old_bar], library,
             "pineforge::native_bar_structurally_valid(")
        link("old_coordinate_to_current_provider", [old_coordinate], current_coordinate_provider,
             ["abi_accept_coordinate(pineforge::NativeCoordinate",
              "abi_accept_decision(pineforge::NativeDecisionContext"])

        order_text = (source_root / "include/pineforge/native_order.hpp").read_text()
        if "inline namespace native_order_v2" in order_text:
            current_order_submit = (
                "pineforge::native_order::native_order_v2::WorkingRequestCore::submit(")
            link("old_order_v1_to_current_library", [old_order_v1], library,
                 "pineforge::native_order::native_order_v1::WorkingRequestCore::submit(")
            link("current_order_to_old_v1_object", [current_order], old_order_v1_obj,
                 current_order_submit)
        else:
            current_order_submit = (
                "pineforge::native_order::native_order_v1::WorkingRequestCore::submit(")
        link("current_order_to_old_object", [current_order], old_order_obj, current_order_submit)
        link("current_calendar_to_old_objects", [current_calendar, old_timezone_obj], old_calendar_obj,
             "pineforge::native_calendar::native_calendar_v2::parse_timeframe(")
        link("current_parse_timeframe_to_old_objects", [current_parse, old_timezone_obj], old_calendar_obj,
             "pineforge::native_calendar::native_calendar_v2::parse_timeframe(")
        link("current_descriptor_to_old_calendar", [current_descriptor, old_timezone_obj], old_calendar_obj,
             "pineforge::native_calendar::native_calendar_v2::timezone_identity_descriptor(")
        link("current_spec_to_old_symbol_control", [current_spec], old_spec_symbols,
             "pineforge::native_run_spec_v1::validate_native_run_spec(")
        link("current_host_events_to_old_symbols", [current_host_events], old_host_events_symbols,
             "pineforge::engine_script_run_v13::NativeStrategyHost::native_events(")
        link("current_bar_to_old_object", [current_bar], old_bar_obj,
             "pineforge::native_driver_v3::native_bar_structurally_valid(")
        link("current_coordinate_to_old_provider", [current_coordinate], old_coordinate_provider,
             ["abi_accept_coordinate(pineforge::native_driver_v3::NativeCoordinate",
              "abi_accept_decision(pineforge::native_driver_v3::NativeDecisionContext"])

    if args.receipt:
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({
        "executable_runs": 0,
        "compiles": len(receipt["compiles"]),
        "links": len(receipt["links"]),
        "library_sha256": receipt["library_sha256"],
        "receipt": str(args.receipt) if args.receipt else None,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
