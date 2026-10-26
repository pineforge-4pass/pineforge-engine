"""TEST ONLY: change the first timed tee encode's valid block packing.

The fixture's timed encodes omit phase logging; every equality replay requests
it. Only that first timed output uses the smaller real-tool block caps. All
replays and the next timed replicate run the unchanged encoder policy.
"""
from __future__ import annotations

from pathlib import Path
import subprocess
import sys


def main(argv):
    tool, marker, *options = argv
    marker = Path(marker)
    inject = "--staging-fd" in options and "--phase-log" not in options and not marker.exists()
    if inject:
        if "--block-records" in options:
            raise RuntimeError("test-only timed policy needs the default block caps")
        options += ["--block-records", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1"]
    child = subprocess.run([tool, *options], close_fds=False, check=False)
    if child.returncode == 0 and inject:
        marker.write_text("first timed output encoded with one-record block caps\n", encoding="utf-8")
    return child.returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
