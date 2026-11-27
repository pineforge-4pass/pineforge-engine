"""Requires a paired release image built with this tree's harness.

Set PINEFORGE_METADATA_TEST_IMAGE to its immutable local image ID. The image
builds the fixture from Pine once; ctypes loads that module and the Docker
runner compiles the very same generated.cpp. No corpus or TV data is needed.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "scripts"), str(ROOT / "docker")]
import pf_release_run
import run_json
import run_strategy


def test_image_and_ctypes_symbol_metadata_trades_and_summary(tmp_path):
    image = os.environ.get("PINEFORGE_METADATA_TEST_IMAGE")
    if not image:
        pytest.skip("NOT COMPARED: expected PINEFORGE_METADATA_TEST_IMAGE=sha256:<image ID>, got unset")
    if shutil.which("docker") is None:
        pytest.skip("NOT COMPARED: docker executable is unavailable")
    assert re.fullmatch(r"sha256:[0-9a-f]{64}", image), (
        f"FAIL: expected an immutable local image ID, got {image!r}")

    local = subprocess.run(["docker", "image", "inspect", "--format", "{{.Id}}", image],
                           text=True, capture_output=True)
    if local.returncode:
        error = local.stderr.strip()
        if "no such image" in error.lower() or "no such object" in error.lower():
            pytest.skip(f"NOT COMPARED: local image {image} is missing: {error}")
        if "cannot connect to the docker daemon" in error.lower():
            pytest.skip(f"NOT COMPARED: Docker daemon unavailable: {error}")
        pytest.fail(f"FAIL: cannot inspect {image}: {error}")
    assert local.stdout.strip() == image, f"FAIL: expected image {image}, got {local.stdout!r}"
    docker = ["docker", "run", "--rm", "--pull=never", "--network=none", "--user",
              f"{os.getuid()}:{os.getgid()}"]
    # Corpus admission already checks the installed entrypoint. Also bind
    # the vendored reader, or an old image could look like a forwarding RED.
    installed = "/opt/pineforge/bin/run_json.py"
    expected_digest = hashlib.sha256((ROOT / "docker/run_json.py").read_bytes()).hexdigest()
    result = subprocess.run([*docker, "--entrypoint", "sha256sum", image, installed],
                            text=True, capture_output=True)
    assert result.returncode == 0, f"FAIL: expected {installed}, got {result.stderr}"
    actual_digest = result.stdout.split(maxsplit=1)[0] if result.stdout.strip() else "missing"
    assert actual_digest == expected_digest, (
        f"FAIL: run_json.py image {actual_digest}, checkout {expected_digest}")

    pine = tmp_path / "strategy.pine"
    pine.write_text('''//@version=6
strategy("Runner symbol metadata", initial_capital=10000, currency=currency.EUR)
ready = not na(ta.ema(close, 3))
if bar_index == 0 and ready
    strategy.entry("L", strategy.long, qty=1.37)
if bar_index == 2
    strategy.close("L")
''', encoding="utf-8")
    chart = tmp_path / "bars.csv"
    start = 1704067200000
    prices = (100, 100, 104, 104, 104)
    chart.write_text("timestamp,open,high,low,close,volume\n" + "".join(
        f"{start + i * 60000},{p},{p},{p},{p},10\n" for i, p in enumerate(prices)),
        encoding="utf-8")
    build = tmp_path / "module"
    build.mkdir()
    result = subprocess.run([*docker, "-e", "PINEFORGE_BUILD_ONLY=1",
                             "-e", "PINEFORGE_WORK_DIR=/build", "-v", f"{build}:/build",
                             "-v", f"{pine}:/in/strategy.pine:ro",
                             "-v", f"{chart}:/in/ohlcv.csv:ro", image],
                            text=True, capture_output=True)
    assert result.returncode == 0, (
        f"FAIL: expected fixture build exit 0, got {result.returncode}: {result.stderr}")
    shutil.copyfile(build / "strategy.cpp", tmp_path / "generated.cpp")
    params = {"input_tf": "1", "script_tf": "1", "runtime_overrides": {
        "currency": "USD", "qty_step": 0.25, "syminfo_metadata": {
            "account_currency_fx": 1.25, "series_origin_ms": start, "chart_ema_na_warmup": 0}}}
    feed, kwargs = run_strategy.inputs_run_kwargs(params, tmp_path, chart, env={})
    host_summary = {}

    def read_summary(report):
        document = run_json.build_report_dict(report, chart, len(prices), start,
                                              start + 240000, 0.0, {}, {})
        host_summary.update(document["summary"])

    native = run_strategy.Strategy(build / "strategy.so").run(
        feed, params=params, on_report=read_summary, **kwargs)
    witness = [(row["entry_time"], row["exit_time"], row["qty"], row["pnl"])
               for row in native["trades"]]
    # The bar-zero EMA must be ready, quantity floors to 1.25, and the four
    # quote units per contract become five in the EUR account.
    expected = [(start + 60000, start + 180000, 1.25, 6.25)]
    assert witness == expected, f"FAIL: ctypes fixture {witness}, expected {expected}"

    raw = {}
    run_release = pf_release_run.run_release

    def observe(*args, **settings):
        raw.update(run_release(*args, **settings))
        return raw

    with mock.patch.object(pf_release_run, "run_release", observe):
        container = run_strategy._run_via_docker(tmp_path, feed, params, kwargs, None, image)
    # Compare all fields the image exports, including the raw summary (the
    # corpus driver retains only net_profit and the processed bar count).
    assert container["input_bars_processed"] == native["input_bars_processed"], (
        f"FAIL: input bars docker={container['input_bars_processed']}, "
        f"ctypes={native['input_bars_processed']}")
    assert len(container["trades"]) == len(native["trades"]), (
        "FAIL: trade details need matching counts; "
        f"docker={len(container['trades'])}, ctypes={len(native['trades'])}; "
        f"summaries: docker={raw['summary']}, ctypes={host_summary}")
    assert raw["summary"] == host_summary, (
        f"FAIL: summaries differ; docker={raw['summary']}, ctypes={host_summary}")
    for index, (actual, host) in enumerate(zip(container["trades"], native["trades"])):
        expected_trade = {k: v for k, v in host.items()
                          if k not in ("entry_id", "exit_id", "exit_comment", "close_cause")}
        assert actual == expected_trade, (
            f"FAIL: trade {index} differs; docker={actual}, ctypes={expected_trade}")
