#!/usr/bin/env python3
"""Run a strategy through the pineforge-release container and return its JSON
report. Shared helper for the docker-runner paths of the corpus harness
(scripts/run_strategy.py --runner docker) and the speed/throughput benchmark
drivers.

The image is a PURE RUNNER: it transpiles/compiles/runs and emits raw JSON.
ALL analysis (tv emit-window, trade trimming, CSV formatting, parity verdict,
speed median/ratio) stays host-side in the caller. This helper only:
  - mounts the pre-transpiled generated.cpp (NEVER re-transpiles .pine — the
    bundled codegen may differ from the one that produced the committed cpp),
  - mounts the OHLCV (caller pre-trims for ohlcv_start_ms),
  - maps already-resolved/normalized run knobs to the container's env contract
    (docker/entrypoint.sh), incl. the validation-parity knobs,
  - `docker run --rm`, parses stdout JSON, surfaces errors.

The caller is responsible for normalization the image does NOT do (e.g. mapping
a 'VOLUME_WEIGHTED' magnifier distribution to its real geometric distribution +
the volume_weighted flag — pass dist already normalized).
"""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

DEFAULT_IMAGE = os.environ.get(
    "PINEFORGE_RELEASE_IMAGE", "ghcr.io/pineforge-4pass/pineforge-release:latest")


class ReleaseRunError(RuntimeError):
    pass


def run_release(
    generated_cpp: Path,
    ohlcv: Path,
    *,
    image: str = DEFAULT_IMAGE,
    inputs: dict | None = None,
    overrides: dict | None = None,
    input_tf: str = "",
    script_tf: str = "",
    bar_magnifier: bool = False,
    magnifier_samples: int = 4,
    magnifier_dist: str = "endpoints",
    magnifier_volume_weighted: bool = False,
    trade_start_ms: int | None = None,
    chart_tz: str = "",
    syminfo: dict | None = None,
    bench: bool = False,
    warmup: int = 3,
    repeats: int = 20,
    network: bool = False,
    timeout: int = 600,
) -> dict:
    """Run one strategy in the container; return the parsed JSON report dict.

    ``generated_cpp`` is mounted as /in/strategy.cpp (compile + run, no
    transpile). ``ohlcv`` is mounted as /in/ohlcv.csv. ``syminfo`` (a dict) is
    written to a temp file and mounted. Knob values must already be resolved by
    the caller (e.g. magnifier_dist lowercased + not 'VOLUME_WEIGHTED')."""
    generated_cpp = Path(generated_cpp).resolve()
    ohlcv = Path(ohlcv).resolve()
    if not generated_cpp.exists():
        raise ReleaseRunError(f"generated.cpp not found: {generated_cpp}")
    if not ohlcv.exists():
        raise ReleaseRunError(f"ohlcv not found: {ohlcv}")

    cmd = ["docker", "run", "--rm"]
    if not network:
        cmd += ["--network=none"]
    cmd += [
        "-v", f"{generated_cpp}:/in/strategy.cpp:ro",
        "-v", f"{ohlcv}:/in/ohlcv.csv:ro",
    ]

    env = {
        "PINEFORGE_INPUTS": json.dumps(inputs or {}),
        "PINEFORGE_OVERRIDES": json.dumps(overrides or {}),
        "PINEFORGE_INPUT_TF": input_tf or "",
        "PINEFORGE_SCRIPT_TF": script_tf or "",
        "PINEFORGE_BAR_MAGNIFIER": "true" if bar_magnifier else "false",
        "PINEFORGE_MAGNIFIER_SAMPLES": str(int(magnifier_samples or 4)),
        "PINEFORGE_MAGNIFIER_DIST": (magnifier_dist or "endpoints").lower(),
    }
    if trade_start_ms is not None:
        env["PINEFORGE_TRADE_START_MS"] = str(int(trade_start_ms))
    if chart_tz:
        env["PINEFORGE_CHART_TZ"] = str(chart_tz)
    if magnifier_volume_weighted and bar_magnifier:
        env["PINEFORGE_MAGNIFIER_VOLUME_WEIGHTED"] = "1"
    if bench:
        env["PINEFORGE_BENCH"] = "1"
        env["PINEFORGE_WARMUP"] = str(int(warmup))
        env["PINEFORGE_REPEATS"] = str(int(repeats))

    # syminfo dict -> temp JSON mounted into the container.
    tmp_syminfo: Path | None = None
    if syminfo:
        import tempfile
        fd, p = tempfile.mkstemp(suffix=".json", prefix="pf_syminfo_")
        tmp_syminfo = Path(p)
        with os.fdopen(fd, "w") as f:
            json.dump(syminfo, f)
        cmd += ["-v", f"{tmp_syminfo}:/in/syminfo.json:ro"]
        env["PINEFORGE_SYMINFO"] = "/in/syminfo.json"

    for k, v in env.items():
        cmd += ["-e", f"{k}={v}"]
    cmd += [image]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    finally:
        if tmp_syminfo is not None:
            tmp_syminfo.unlink(missing_ok=True)

    if proc.returncode != 0:
        raise ReleaseRunError(
            f"docker run failed (exit {proc.returncode}) for {generated_cpp.parent.name}\n"
            f"stderr tail:\n{proc.stderr[-2000:]}")
    out = proc.stdout.strip()
    if not out:
        raise ReleaseRunError(f"empty stdout from container for {generated_cpp.parent.name}")
    # The report is the last JSON line (engine logs go to stderr, but be defensive).
    try:
        report = json.loads(out.splitlines()[-1])
    except json.JSONDecodeError as e:
        raise ReleaseRunError(f"non-JSON container stdout: {e}\n{out[-1000:]}")
    if isinstance(report, dict) and report.get("error"):
        raise ReleaseRunError(f"engine error: {report['error']}")
    return report


def report_trades_to_runstrategy_shape(report: dict) -> list[dict]:
    """Map run_json.py trades[] -> the dict shape scripts/run_strategy.py's
    write_engine_trades_csv expects. Key diff: run_json emits ``side`` ('long'/
    'short'); the writer reads ``is_long``. max_drawdown stays POSITIVE (the
    writer negates it for the TV export). All other keys already match."""
    out = []
    for t in report.get("trades", []):
        out.append({
            "entry_time": int(t["entry_time"]),
            "exit_time": int(t["exit_time"]),
            "entry_price": float(t["entry_price"]),
            "exit_price": float(t["exit_price"]),
            "pnl": float(t["pnl"]),
            "pnl_pct": float(t["pnl_pct"]),
            "is_long": (t["side"] == "long"),
            "max_runup": float(t["max_runup"]),
            "max_drawdown": float(t["max_drawdown"]),
            "qty": float(t["qty"]),
            "commission": float(t.get("commission", 0.0)),
            "entry_bar_index": int(t.get("entry_bar_index", -1)),
            "exit_bar_index": int(t.get("exit_bar_index", -1)),
            "entry_incarnation": int(t.get("entry_incarnation", 0) or 0),
        })
    return out
