"""A controllable selected-inventory binder process for stage-lifetime tests."""

import json
import os
from pathlib import Path
import resource
import signal
import sys

trace = Path(os.environ["PF_ENTRYPOINT_FIXTURE_TRACE"])
(trace / "binder.json").write_text(json.dumps({
    "pid": os.getpid(),
    "start_ticks": Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19],
    "as": list(resource.getrlimit(resource.RLIMIT_AS)),
    "stdout": os.readlink("/proc/self/fd/1"),
}), encoding="utf-8")
mode = os.environ["PF_ENTRYPOINT_FIXTURE_BINDER"]
if mode == "hang":
    signal.pause()
elif mode in ("flood", "success"):
    payload = Path(os.environ["PF_ENTRYPOINT_FIXTURE_BINDER_DIAGNOSTIC"]).read_bytes()
    offset = 0
    while offset < len(payload):
        offset += os.write(2, payload[offset:offset + 8192])
    if mode == "flood":
        sys.exit(23)
    Path(sys.argv[sys.argv.index("--out") + 1]).write_text(
        '{"fixture":"bound inventory"}\n', encoding="utf-8")
elif mode == "fail":
    os.write(2, b"fixture binder: requested inventory failure\n")
    sys.exit(19)
else:
    raise ValueError(f"unknown fixture binder mode: {mode}")
