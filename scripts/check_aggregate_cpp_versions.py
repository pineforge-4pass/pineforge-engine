#!/usr/bin/env python3
"""Source-only C++ ownership guard for the v17 native/source boundary."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def clean(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def body(text: str, pattern: str, name: str) -> str:
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise ValueError(name + " requires exactly one definition")
    start = matches[0].end()
    depth = 1
    for at in range(start, len(text)):
        depth += (text[at] == "{") - (text[at] == "}")
        if depth == 0:
            return text[start:at]
    raise ValueError(name + " has an unclosed body")


def standalone_scope(text: str, outer: str, version: str) -> str:
    owner = body(clean(text), r"namespace\s+" + re.escape(outer) + r"\s*\{", outer)
    return body(owner, r"inline\s+namespace\s+" + re.escape(version) + r"\s*\{", version)


def check(root: Path = ROOT) -> None:
    engine = (root / "include/pineforge/engine.hpp").read_text()
    native = (root / "include/pineforge/native_host.hpp").read_text()
    adapter = (root / "include/pineforge/source/pine_adapter.hpp").read_text()
    source_hash = (root / "src/source/pine_state_hash.cpp").read_text()
    if re.findall(r"inline\s+namespace\s+(engine_script_run_v\d+)\s*\{", engine) != [
            "engine_script_run_v17"]:
        raise ValueError("BacktestEngine requires engine_script_run_v17")
    if "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V17 1" not in native:
        raise ValueError("native host capability must remain v17")
    if 'kSourceAdapterDomain[] = "pineforge-source-adapter/v2"' not in adapter:
        raise ValueError("source adapter domain must remain v2")
    if "hash_source_extension(BrokerStateHashSink& f) const" not in source_hash:
        raise ValueError("source extension hash is missing")
    retired = root / "include/pineforge/source/pine_pending_intent.hpp"
    if retired.exists():
        raise ValueError("retired source order header is still present")


if __name__ == "__main__":
    check()
    print("aggregate v17/native source boundary ownership verified")
