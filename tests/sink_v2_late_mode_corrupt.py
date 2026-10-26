"""TEST ONLY: finish the real full tee encoder, then change its staged bytes.

No synthetic receipt or encoder is used. The owner must reap this executable
before publication, so the altered same-length inode reaches the gate's hash
comparison with the real successful receipt. Serial/structure are controls.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time


def main(argv):
    tool, *options = argv
    child = subprocess.run([tool, *options], close_fds=False, check=False)
    structure = "--late-verify" in options and options[options.index("--late-verify") + 1] == "structure"
    if child.returncode == 0 and "--staging-fd" in options and not structure:
        staging = int(options[options.index("--staging-fd") + 1])
        original = os.pread(staging, 1, 0)
        if len(original) != 1 or os.pwrite(staging, bytes((original[0] ^ 1,)), 0) != 1:
            raise RuntimeError("test-only byte mutation failed")
        os.fsync(staging)
        phase = int(options[options.index("--phase-log") + 1])
        os.write(phase, (json.dumps({"event": "test_output_byte_flipped",
                                    "monotonic_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC)})
                         + "\n").encode("ascii"))
    return child.returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
