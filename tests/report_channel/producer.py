"""Input-driven real producer fixture; no supervisor implementation imports."""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import signal
import sys
import time

from host import ROOT, sink_wire


def write_all(fd, data, fragment):
    offset = 0
    while offset < len(data):
        count = os.write(fd, data[offset:offset + fragment])
        if count <= 0:
            raise OSError("fixture write made no progress")
        offset += count


def main():
    request = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    reference = "--reference" in sys.argv[2:]
    fragment = request.get("fragment_bytes", 65536)
    if "diagnostic_child_state" in request:
        leader = os.getpid()
        ready_read, ready_write = os.pipe()
        child = os.fork()
        if child == 0:
            os.close(ready_read)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            if request.get("detached_diagnostic_child"):
                os.setsid()
            for name in ("PINEFORGE_REPORT_CANDIDATE_FD", "PINEFORGE_REPORT_NATIVE_SINK_FD"):
                if name in os.environ:
                    os.close(int(os.environ[name]))
            Path(request["diagnostic_child_state"]).write_text(
                json.dumps({"leader": leader, "child": os.getpid()}))
            os.write(ready_write, b"1")
            os.close(ready_write)
            while True:
                try:
                    os.write(2, b"diagnostic child continues\n")
                except BrokenPipeError:
                    # Continue attempting diagnostics after S closes its reader.
                    pass
                if os.getppid() != leader:
                    Path(request["diagnostic_child_progress"]).touch()
                time.sleep(0.01)
        os.close(ready_write)
        if os.read(ready_read, 1) != b"1":
            raise AssertionError("diagnostic child did not become ready")
        os.close(ready_read)
        if not request.get("leader_completes"):
            time.sleep(120)
    for name, fd in (("stdout", 1), ("stderr", 2)):
        if name in request:
            write_all(fd, base64.b64decode(request[name]), fragment)
    if "candidate" in request:
        candidate = base64.b64decode(request["candidate"])
    else:
        closes = request.get("closes", [10, 12, 11])
        first_ts = request.get("first_ts", 1000)
        document = {
            "engine": "pineforge",
            "input": {"ohlcv": request.get("label", "fixed.csv"),
                      "bars": len(closes), "first_ts": first_ts,
                      "last_ts": first_ts + (len(closes) - 1) * 60000,
                      "first_time": "fixed", "last_time": "fixed"},
            "applied_inputs": {}, "applied_overrides": {}, "applied_runtime": {},
            "elapsed_seconds": 0.0,
            "summary": {"total_trades": 0, "wins": 0, "losses": 0, "win_rate_pct": 0,
                        "net_pnl": closes[-1] - closes[0], "avg_trade": 0,
                        "best_trade": 0, "worst_trade": 0, "max_drawdown": 0,
                        "bars_processed": len(closes)},
            "diagnostics": {}, "metrics": {"all": {}, "longs": {}, "shorts": {}, "equity": {}},
            "trades": [],
            "equity_curve": [],
        }
        if request.get("inherited_sink"):
            document.pop("trades")
            document.pop("equity_curve")
        candidate = json.dumps(document, separators=(",", ":"), allow_nan=False,
                               ensure_ascii=False).encode() + b"\n"
    sink_fd = None
    if request.get("inherited_sink"):
        name = ("PINEFORGE_REPORT_REFERENCE_SINK_FD" if reference
                else "PINEFORGE_REPORT_NATIVE_SINK_FD")
        sink_fd = int(os.environ[name])
        native = (base64.b64decode(request["native_sink"]) if "native_sink" in request else
                  sink_wire(request.get("closes", [10, 12, 11]), request.get("first_ts", 1000)))
        native += base64.b64decode(request.get("sink_suffix", ""))
        write_all(sink_fd, native, fragment)
        if not request.get("candidate_before_sink_close"):
            os.close(sink_fd)
            sink_fd = None
    fd = 1 if reference else int(os.environ["PINEFORGE_REPORT_CANDIDATE_FD"])
    if request.get("runner_writer") or "failure_writer" in request:
        sys.path.insert(0, str(ROOT / "docker"))
        import run_json
        owner = None if reference else run_json._CandidateOutput(str(fd))
        try:
            if "failure_writer" in request:
                failure = request["failure_writer"]
                run_json.write_failure(failure["text"], failure.get("code"),
                                       failure.get("args"), destination=owner)
            else:
                status = run_json._write_report(candidate.decode("utf-8"), destination=owner)
                if status:
                    return status
        finally:
            if owner is not None:
                owner.close()
    else:
        write_all(fd, candidate, fragment)
        if not reference:
            os.close(fd)
    if sink_fd is not None:
        time.sleep(request["hold_sink_open_ms"] / 1000)
        os.close(sink_fd)
    if request.get("delay_after_candidate_ms"):
        time.sleep(request["delay_after_candidate_ms"] / 1000)
    return request.get("exit_status", 0)


if __name__ == "__main__":
    sys.exit(main())
