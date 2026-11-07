"""Series-origin admission and provenance without a strategy library."""
from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import run_json


FIRST = 1_800_000_000_000


def apply(tmp_path, document, lib=None):
    path = tmp_path / "syminfo.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    calls = []
    if lib is None:
        lib = SimpleNamespace(
            strategy_set_syminfo_metadata=lambda *args: calls.append(args))
    return run_json.apply_syminfo(lib, 7, path), calls


@pytest.mark.parametrize("offset", [0, 17_000, 900_000 * 10_000, -17_000])
@pytest.mark.parametrize("wrapped", [False, True])
def test_applied_origin_and_first_bar(tmp_path, offset, wrapped):
    origin = FIRST - offset
    si = {"series_origin_ms": origin}
    applied, calls = apply(tmp_path, {"syminfo": si} if wrapped else si)
    assert calls == [(7, b"series_origin_ms", origin)]
    assert applied == si
    expected = {**si, "series_origin_first_bar_ms": FIRST,
                "series_origin_offset_ms": offset}
    if offset < 0:
        expected["series_origin_warning"] = "origin_after_first_bar"
    assert run_json.series_origin_echo(applied, FIRST) == expected
    assert applied == si  # pure helper does not modify the validated snapshot
    assert run_json.series_origin_echo(applied, None) == si


@pytest.mark.parametrize("value", [
    0, -1, 1.5, 1.0, "1", True, False,
    float("nan"), float("inf"), -float("inf"), [], {},
])
def test_malformed_origin_is_typed_and_atomic(tmp_path, value):
    calls = []
    lib = SimpleNamespace(
        strategy_set_syminfo_metadata=lambda *args: calls.append(args))
    with pytest.raises(run_json.SyminfoError) as caught:
        apply(tmp_path, {"mincontract": 0.25, "series_origin_ms": value}, lib)
    assert caught.value.code == "syminfo_unreadable"
    assert caught.value.code_args == {"reason": "value_type"}
    assert "series_origin_ms" in str(caught.value)
    assert calls == []  # even the preceding valid lot setter must not run


@pytest.mark.parametrize("si", [{}, {"series_origin_ms": None}])
def test_absent_or_null_is_noop(tmp_path, si):
    applied, calls = apply(tmp_path, si, SimpleNamespace())
    assert applied == {}
    assert calls == []
    assert run_json.series_origin_echo(applied, FIRST) == {}
    grid = {"qty_step": 0.25, "mincontract": 0.25}
    before = json.dumps(grid, separators=(",", ":"))
    assert json.dumps(run_json.series_origin_echo(grid, FIRST),
                      separators=(",", ":")) == before


def test_origin_and_grid_echo_together(tmp_path):
    applied, calls = apply(tmp_path, {"mincontract": 0.25, "series_origin_ms": FIRST})
    assert calls == [(7, b"qty_step", 0.25), (7, b"mincontract", 0.25),
                     (7, b"series_origin_ms", FIRST)]
    assert run_json.series_origin_echo(applied, FIRST) == {
        "qty_step": 0.25, "mincontract": 0.25, "series_origin_ms": FIRST,
        "series_origin_first_bar_ms": FIRST, "series_origin_offset_ms": 0}


def test_missing_metadata_setter_is_typed(tmp_path):
    with pytest.raises(run_json.SyminfoError) as caught:
        apply(tmp_path, {"series_origin_ms": FIRST}, SimpleNamespace())
    assert caught.value.code == "strategy_library_incompatible"
    assert caught.value.code_args == {
        "reason": "setter_missing", "missing": "strategy_set_syminfo_metadata"}
    assert "series_origin_ms" in str(caught.value)
