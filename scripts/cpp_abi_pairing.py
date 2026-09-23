#!/usr/bin/env python3
"""Shared, compiler-backed controls for the frozen v18 / live v19 C++ boundary.

The ABI checks deliberately compile callers against the headers that name each
epoch, then link them only.  A caller binary is never executed: a successful
link proves an accepted pair and a failed link must name the other epoch's
``BacktestEngine::broker_state_hash`` symbol.  This keeps the test tied to the
real archives instead of a manifest that merely describes an intended pair.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Iterable

from prepare_settlement_cpp_abi_base import (
    PROVIDERS,
    authenticate_headers,
    extract_tar,
)


ROOT = Path(__file__).resolve().parents[1]
V16_EPOCH = "engine_script_run_v16"
V18_EPOCH = "engine_script_run_v18"
V19_EPOCH = "engine_script_run_v19"
# The live epoch, and the frozen provider every receipt-gated pair links
# against in both directions (tests/fixtures/native_cpp_abi/host-fc7aad6).
CURRENT_EPOCH = V19_EPOCH
ACTIVE_FROZEN_ROLE = "v18-frozen"


class PairingError(RuntimeError):
    """The ABI evidence is absent, unauthenticated, or has the wrong result."""


def enforce_receipt_mode(receipts: Iterable[Path | None], *, skip: bool,
                         require: bool, label: str) -> int | None:
    """Apply the manual-skip/CI-required contract to receipt inputs."""
    missing = [Path(value) for value in receipts
               if value is not None and not Path(value).exists()]
    if not missing:
        return None
    if skip:
        print(f"SKIP: receipt missing: {missing[0]} (prepared by scripts/ci_verify.py)")
        return 77
    if require:
        raise PairingError(f"{label} required receipt missing: {missing[0]}")
    return None


@dataclass(frozen=True)
class FrozenProvider:
    archive: Path
    headers_tar: Path
    receipt: Path
    data: dict


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _resolve(receipt: Path, value: str) -> Path:
    candidate = Path(value)
    return candidate if candidate.is_absolute() else receipt.parent / candidate


def epoch_from_headers(include: Path) -> str:
    engine = include / "pineforge" / "engine.hpp"
    if not engine.is_file():
        raise PairingError("engine header is missing from " + str(include))
    epochs = re.findall(r"\binline\s+namespace\s+(engine_script_run_v\d+)\s*\{",
                        engine.read_text())
    if len(epochs) != 1:
        raise PairingError("engine header must declare exactly one epoch")
    return epochs[0]


def _archive_symbols(archive: Path) -> str:
    result = subprocess.run(["nm", "-g", "-C", str(archive)], text=True,
                            capture_output=True, timeout=90)
    if result.returncode:
        raise PairingError("cannot inspect ABI archive " + str(archive) + ":\n" + result.stderr)
    return result.stdout


def load_frozen_provider(role: str, receipt_path: Path, destination: Path) -> FrozenProvider:
    """Authenticate and unpack the actual frozen archive `role` names by its receipt."""
    provider = PROVIDERS[role]
    epoch = provider["engine_epoch"]
    label = "frozen " + epoch.rsplit("_", 1)[-1]
    fixture = provider["manifest"].parent.name
    if not receipt_path.is_file():
        raise PairingError(label + " ABI receipt is missing: " + str(receipt_path))
    try:
        data = json.loads(receipt_path.read_text())
    except json.JSONDecodeError as error:
        raise PairingError("invalid " + label + " ABI receipt: " + str(error)) from error
    if data.get("commit") != provider["commit"] or data.get("tree") != provider["tree"]:
        raise PairingError("frozen ABI receipt does not identify " + fixture + " "
                           + epoch.rsplit("_", 1)[-1])
    for key in ("archive", "archiveSha256", "headers", "headersSha256"):
        if not data.get(key):
            raise PairingError(label + " ABI receipt omits " + key)
    archive = _resolve(receipt_path, data["archive"])
    headers_tar = _resolve(receipt_path, data["headers"])
    if not archive.is_file() or not headers_tar.is_file():
        raise PairingError(label + " ABI receipt names missing archive/header artifacts")
    if sha256(archive) != data["archiveSha256"] or sha256(headers_tar) != data["headersSha256"]:
        raise PairingError(label + " ABI artifact bytes do not match its receipt")
    if not archive.read_bytes().startswith(b"!<arch>\n"):
        raise PairingError(label + " provider is not a static archive")
    extract_tar(headers_tar.read_bytes(), destination)
    authenticate_headers(destination, provider["manifest"], commit=provider["commit"],
                         tree=provider["tree"])
    include = destination / "include"
    if epoch_from_headers(include) != epoch:
        raise PairingError("authenticated frozen header closure is not " + epoch)
    if epoch + "::BacktestEngine::broker_state_hash" not in _archive_symbols(archive):
        raise PairingError(label + " archive does not export its broker-state ABI witness")
    return FrozenProvider(archive=archive, headers_tar=headers_tar,
                          receipt=receipt_path, data=data)


def load_frozen_v16(receipt_path: Path, destination: Path) -> FrozenProvider:
    """The L0 v16 archive: the runtime-budget baseline (scripts/check_runtime_budget.py)."""
    return load_frozen_provider("v16-frozen", receipt_path, destination)


def audit_prepared_receipt(receipt_path: Path, label: str) -> dict:
    """Consume every historical CMake receipt with artifact-byte evidence.

    Only host-fc7aad6 is the active v18/v19 pairing provider.  The older
    receipts remain historical input evidence, so accepting a CMake argument
    without reading its archive and header bytes would make the CTest command
    line misleading again.
    """
    if not receipt_path.is_file():
        raise PairingError(label + " ABI receipt is missing: " + str(receipt_path))
    try:
        data = json.loads(receipt_path.read_text())
    except json.JSONDecodeError as error:
        raise PairingError(label + " ABI receipt is invalid: " + str(error)) from error
    for key in ("archive", "archiveSha256", "headers", "headersSha256"):
        if not data.get(key):
            raise PairingError(label + " ABI receipt omits " + key)
    archive = _resolve(receipt_path, data["archive"])
    headers = _resolve(receipt_path, data["headers"])
    if not archive.is_file() or not headers.is_file():
        raise PairingError(label + " ABI receipt names missing artifacts")
    if sha256(archive) != data["archiveSha256"] or sha256(headers) != data["headersSha256"]:
        raise PairingError(label + " ABI receipt artifact digest mismatch")
    if not archive.read_bytes().startswith(b"!<arch>\n"):
        raise PairingError(label + " ABI receipt does not name a static archive")
    return {
        "label": label,
        "receipt": str(receipt_path),
        "receiptSha256": sha256(receipt_path),
        "archive": str(archive),
        "archiveSha256": sha256(archive),
        "headersSha256": sha256(headers),
    }


def _source(epoch: str, kind: str) -> str:
    if kind == "script":
        return '''#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdint>
class AbiScript final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const pineforge::Bar&) override {}
};
int main(int argc, char** argv) {
    auto* strategy = reinterpret_cast<AbiScript*>(argv);
    return static_cast<int>(strategy->broker_state_hash() ^ static_cast<std::uint64_t>(argc));
}
'''
    if kind != "native":
        raise PairingError("unknown ABI caller kind: " + kind)
    return f'''#include <pineforge/engine.hpp>
#include <cstdint>
#include <type_traits>
static_assert(std::is_same<pineforge::BacktestEngine,
              pineforge::{epoch}::BacktestEngine>::value,
              "caller was compiled against the wrong engine epoch");
int main(int argc, char** argv) {{
    auto* engine = reinterpret_cast<pineforge::BacktestEngine*>(argv);
    return static_cast<int>(engine->broker_state_hash() ^ static_cast<std::uint64_t>(argc));
}}
'''


def _compile(compiler: str, flags: Iterable[str], name: str, text: str, include: Path,
             generated_include: Path, root: Path) -> tuple[Path, dict]:
    source = root / (name + ".cpp")
    object_file = root / (name + ".o")
    source.write_text(text)
    argv = [compiler, "-std=c++17", "-O0", "-ffp-contract=off", *flags,
            "-I", str(include), "-I", str(generated_include), "-c", str(source),
            "-o", str(object_file)]
    result = subprocess.run(argv, text=True, capture_output=True, timeout=120)
    (root / (name + ".compile.log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise PairingError(name + " failed to compile:\n" + result.stdout + result.stderr)
    return object_file, {
        "name": name,
        "argv": argv,
        "sourceSha256": hashlib.sha256(text.encode()).hexdigest(),
        "objectSha256": sha256(object_file),
    }


def _link(compiler: str, flags: Iterable[str], name: str, object_file: Path, archive: Path,
          expected: str, caller_epoch: str, provider_epoch: str, root: Path) -> dict:
    output = root / name
    argv = [compiler, "-std=c++17", *flags, str(object_file), str(archive), "-pthread",
            "-o", str(output)]
    result = subprocess.run(argv, text=True, capture_output=True, timeout=120)
    diagnostic = result.stdout + result.stderr
    (root / (name + ".link.log")).write_text(diagnostic)
    if expected == "accept":
        if result.returncode:
            raise PairingError(name + " rejected an ABI pair that must link:\n" + diagnostic)
    elif expected == "reject":
        if result.returncode == 0:
            raise PairingError(name + " unexpectedly linked a stale ABI pair")
        needle = caller_epoch + "::BacktestEngine::broker_state_hash"
        if needle not in diagnostic:
            raise PairingError(name + " rejected for the wrong reason; missing " + needle + ":\n"
                               + diagnostic)
    else:
        raise PairingError("unknown link expectation: " + expected)
    return {
        "name": name,
        "argv": argv,
        "outcome": "linked" if expected == "accept" else "expected-rejection",
        "exitCode": result.returncode,
        "callerEpoch": caller_epoch,
        "providerEpoch": provider_epoch,
        "diagnosticSha256": hashlib.sha256(diagnostic.encode()).hexdigest(),
        "diagnostic": diagnostic[-6000:],
        "executed": False,
    }


def execute_frozen_pair(*, compiler: str, extra_flags: Iterable[str], current_library: Path,
                        current_include: Path, generated_include: Path, frozen_receipt: Path,
                        kind: str, frozen_role: str = ACTIVE_FROZEN_ROLE,
                        artifact_directory: Path | None = None) -> dict:
    """Compile and link the two acceptance and two rejection pairings.

    The live archive and the frozen one each accept a caller compiled against
    their own headers and refuse the other's, in both directions.
    """
    frozen_epoch = PROVIDERS[frozen_role]["engine_epoch"]
    frozen_tag = frozen_epoch.rsplit("_", 1)[-1]
    current_tag = CURRENT_EPOCH.rsplit("_", 1)[-1]
    current_library = current_library.resolve()
    current_include = current_include.resolve()
    generated_include = generated_include.resolve()
    if not current_library.is_file():
        raise PairingError("current ABI library is missing: " + str(current_library))
    if epoch_from_headers(current_include) != CURRENT_EPOCH:
        raise PairingError("current headers are not " + CURRENT_EPOCH)
    if CURRENT_EPOCH + "::BacktestEngine::broker_state_hash" not in _archive_symbols(current_library):
        raise PairingError("current " + current_tag
                           + " archive does not export its broker-state ABI witness")
    flags = list(extra_flags)
    root_parent = artifact_directory if artifact_directory is not None else None
    if root_parent is not None:
        root_parent.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix=current_tag + "-" + frozen_tag + "-" + kind
                                     + ".artifacts-", dir=root_parent))
        cleanup = None
    else:
        cleanup = tempfile.TemporaryDirectory(
            prefix="pineforge-" + frozen_tag + "-" + current_tag + "-")
        root = Path(cleanup.name)
    try:
        frozen_root = root / ("frozen-" + frozen_tag)
        frozen = load_frozen_provider(frozen_role, frozen_receipt.resolve(), frozen_root)
        frozen_include = frozen_root / "include"
        frozen_object, frozen_compile = _compile(
            compiler, flags, kind + "_" + frozen_tag, _source(frozen_epoch, kind),
            frozen_include, generated_include, root)
        current_object, current_compile = _compile(
            compiler, flags, kind + "_" + current_tag, _source(CURRENT_EPOCH, kind),
            current_include, generated_include, root)
        links = [
            _link(compiler, flags, kind + "_" + frozen_tag + "_to_" + frozen_tag, frozen_object,
                  frozen.archive, "accept", frozen_epoch, frozen_epoch, root),
            _link(compiler, flags, kind + "_" + current_tag + "_to_" + current_tag, current_object,
                  current_library, "accept", CURRENT_EPOCH, CURRENT_EPOCH, root),
            _link(compiler, flags, kind + "_" + frozen_tag + "_to_" + current_tag + "_reject",
                  frozen_object, current_library, "reject", frozen_epoch, CURRENT_EPOCH, root),
            _link(compiler, flags, kind + "_" + current_tag + "_to_" + frozen_tag + "_reject",
                  current_object, frozen.archive, "reject", CURRENT_EPOCH, frozen_epoch, root),
        ]
    finally:
        if cleanup is not None:
            cleanup.cleanup()
    return {
        "kind": kind,
        "artifactsDirectory": str(root),
        "frozenProvider": {
            "role": frozen_role,
            "receipt": str(frozen.receipt),
            "archive": str(frozen.archive),
            "archiveSha256": sha256(frozen.archive),
            "headersSha256": sha256(frozen.headers_tar),
        },
        "compiles": [frozen_compile, current_compile],
        "links": links,
        "summary": {"accepted": 2, "rejected": 2, "executedBinaries": 0},
    }


def run_synthetic_pair(compiler: str) -> list[str]:
    """Small real-link mutation control used by each ABI checker test suite."""
    ar = shutil.which("ar")
    if not ar:
        raise PairingError("ar is required for ABI mutation controls")
    with tempfile.TemporaryDirectory(prefix="pineforge-abi-mutation-") as temporary:
        root = Path(temporary)
        (root / "v16.hpp").write_text(
            "namespace pairing { inline namespace v16 { int witness(); } }\n")
        (root / "v17.hpp").write_text(
            "namespace pairing { inline namespace v17 { int witness(); } }\n")
        provider = root / "provider.cpp"
        provider.write_text('#include "v16.hpp"\nint pairing::v16::witness() { return 7; }\n')
        provider_object = root / "provider.o"
        compiled = subprocess.run([compiler, "-std=c++17", "-c", str(provider), "-o",
                                   str(provider_object)], text=True, capture_output=True, timeout=60)
        if compiled.returncode:
            raise PairingError("synthetic v16 provider did not compile: " + compiled.stderr)
        archive = root / "libpair.a"
        packed = subprocess.run([ar, "rcs", str(archive), str(provider_object)], text=True,
                                capture_output=True, timeout=60)
        if packed.returncode:
            raise PairingError("synthetic v16 provider did not archive: " + packed.stderr)

        def link_header(header: str, expectation: str) -> str:
            source = root / (header + ".cpp")
            output = root / (header + ".out")
            source.write_text('#include "' + header + '.hpp"\nint main() { return pairing::witness(); }\n')
            result = subprocess.run([compiler, "-std=c++17", str(source), str(archive), "-o",
                                     str(output)], text=True, capture_output=True, timeout=60)
            if expectation == "accept" and result.returncode:
                raise PairingError("synthetic accepted pair rejected: " + result.stderr)
            if expectation == "reject" and result.returncode == 0:
                raise PairingError("synthetic stale pair unexpectedly linked")
            if expectation == "reject" and "pairing::v17::witness" not in (result.stdout + result.stderr):
                raise PairingError("synthetic stale pair had the wrong diagnostic")
            return expectation

        return [link_header("v16", "accept"), link_header("v17", "reject")]
