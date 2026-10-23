"""Read the trusted compiler FACTS sidecar before starting a strategy.

No execution/report/trade output is an input to this module.  The arithmetic
module remains pure.  A sidecar is a build product of a trusted compiler and
lowering proof pass; hashing binds it to inputs, it does not establish a proof.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from arena_bound import FACTS_SCHEMA, FactsError, arena_bound
from arena_bound_source import inspect_pine_source


MANIFEST_NAME = "arena-facts.json"
ENGINE_ROOT = Path(__file__).resolve().parents[1]


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def engine_fingerprint(root: Path = ENGINE_ROOT) -> str:
    """Bind a lowering/storage proof to the complete runtime header/source tree."""
    paths = [root / "CMakeLists.txt"]
    for directory in ("src", "include", "docker"):
        paths.extend(path for path in (root / directory).rglob("*")
                     if path.is_file() and path.suffix in (
                         ".h", ".hpp", ".cpp", ".c", ".inc", ".def", ".in"))
    digest = hashlib.sha256(b"pineforge-arena-engine/v1\0")
    for path in sorted(paths):
        data = path.read_bytes()
        digest.update(str(path.relative_to(root)).encode("utf-8") + b"\0")
        digest.update(str(len(data)).encode("ascii") + b"\0")
        digest.update(data)
    return digest.hexdigest()


def plan_fingerprint(primary_bytes, settings: dict) -> str:
    """Hash immutable input bytes and fully resolved, JSON-shaped run settings."""
    digest = hashlib.sha256(b"pineforge-arena-plan/v1\0")
    digest.update(primary_bytes)
    digest.update(b"\0")
    digest.update(json.dumps(settings, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=False, allow_nan=False,
                             default=str).encode("utf-8"))
    return digest.hexdigest()


def unknown_strategy(reason: str, source: str) -> dict:
    return {"controlFlow": {"status": "unknown", "reason": reason, "source": source}}


def read_compile_facts(probe: Path) -> tuple[dict, str]:
    """Return (all compiler facts, arena census classification).

    Missing sources, sidecars and unsupported proofs are UNCLASSIFIED.  They
    are never silently counted as bounded or as mathematically unbounded.
    The caller still passes this strategy object to arena_bound; its unknown
    status routes through the same public unbounded/null result.
    """
    probe = Path(probe)
    sidecar = probe / MANIFEST_NAME
    if not sidecar.is_file():
        pine = probe / "strategy.pine"
        if pine.is_file():
            inventory = inspect_pine_source(pine.read_text(encoding="utf-8"), str(pine))
            control = inventory["controlFlow"]
            if control["status"] == "unbounded":
                return {"strategy": {"controlFlow": control}}, "UNBOUNDED"
            return {"strategy": {"controlFlow": control}}, "UNCLASSIFIED"
        return {"strategy": unknown_strategy(
            "trusted compiler/lowering FACTS record is absent", str(sidecar))}, "UNCLASSIFIED"
    try:
        record = json.loads(sidecar.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise FactsError(f"{sidecar}: malformed FACTS JSON: {error}") from error
    if not isinstance(record, dict) or record.get("schema") != FACTS_SCHEMA:
        raise FactsError(f"{sidecar}: wrong FACTS schema")
    producer = record.get("producer")
    if not isinstance(producer, str) or not producer.strip():
        raise FactsError(f"{sidecar}: missing trusted producer identity")
    fingerprints = record.get("sha256")
    if not isinstance(fingerprints, dict):
        raise FactsError(f"{sidecar}: missing source fingerprints")
    for name in ("generated.cpp", "strategy.pine", "inputs.json"):
        path = probe / name
        expected = fingerprints.get(name)
        # Generated-code fixtures may have no Pine file.  Its absence must be
        # pinned explicitly, just like an absent optional inputs.json.
        actual = _sha256(path.read_bytes()) if path.is_file() else None
        if name == "generated.cpp" and actual is None:
            raise FactsError(f"{sidecar}: generated.cpp is missing")
        if name not in fingerprints or expected != actual:
            raise FactsError(f"{sidecar}: stale/missing fingerprint for {name}")
    if record.get("engineSha256") != engine_fingerprint():
        return {"strategy": unknown_strategy(
            "missing/stale runtime source/header closure fingerprint", str(sidecar))}, "UNCLASSIFIED"
    scope = record.get("scope")
    if not isinstance(scope, dict) or scope.get("kind") not in ("all-data", "plan"):
        return {"strategy": unknown_strategy(
            "missing compiler proof scope (all-data or exact plan)", str(sidecar))}, "UNCLASSIFIED"
    if scope["kind"] == "plan" and not re_full_sha256(scope.get("sha256")):
        raise FactsError(f"{sidecar}: exact-plan scope needs a sha256")
    strategy = record.get("strategy")
    if not isinstance(strategy, dict):
        raise FactsError(f"{sidecar}: missing strategy facts")
    control = strategy.get("controlFlow", {})
    if not isinstance(control, dict):
        raise FactsError(f"{sidecar}: strategy.controlFlow must be an object")
    status = control.get("status")
    if status == "unbounded":
        classification = "UNBOUNDED"
    elif status == "bounded":
        classification = "BOUNDED"
    elif status == "unknown":
        classification = "UNCLASSIFIED"
    else:
        raise FactsError(f"{sidecar}: missing control-flow verdict")
    result = {"strategy": strategy, "scope": scope}
    for name in ("ohlcv", "stream", "childPrivate"):
        if name in record:
            result[name] = record[name]
    return result, classification


def read_strategy_facts(probe: Path) -> tuple[dict, str]:
    record, classification = read_compile_facts(probe)
    return record["strategy"], classification


def re_full_sha256(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def bound_probe(probe: Path, data: dict) -> tuple[dict, str]:
    """Production/lab shared extraction: immutable build facts + data plan."""
    compiled, classification = read_compile_facts(probe)
    scope = compiled.get("scope", {})
    if scope.get("kind") == "plan" and scope["sha256"] != data.get("planSha256"):
        compiled = {"strategy": unknown_strategy(
            "FACTS record does not bind this resolved input/run plan",
            str(Path(probe) / MANIFEST_NAME))}
        classification = "UNCLASSIFIED"
    return arena_bound({**compiled, "schema": FACTS_SCHEMA, "data": data}), classification
