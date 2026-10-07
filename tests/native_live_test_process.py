"""Direct-child timing and isolated output for the native live witnesses.

Linux and macOS both provide wait4. Reaping the specific Popen child gives us
its exit status and resource usage without a timing subprocess, process-tree
guessing, or the cumulative RUSAGE_CHILDREN totals of unrelated invocations.
"""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def isolated_output(root):
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="run-", dir=root))


def peak_rss_kib(value, platform):
    # Darwin reports bytes; Linux reports KiB.
    return value / 1024 if platform == "darwin" else value


class TimedProcess:
    """Own and reap one direct child; callers must wait, including after kill.

    Only this object waits for the child. Popen handles spawning and exec
    errors; wait4 collects per-process usage and sets Popen's public returncode
    so its destructor cannot attempt a second reap.
    """

    def __init__(self, command, receipt, **kwargs):
        self.command = list(map(str, command))
        self.receipt = Path(receipt)
        self.started = time.monotonic()
        self._child = subprocess.Popen(self.command, **kwargs)
        self.pid = self._child.pid
        self.returncode = None

    def poll(self):
        if self.returncode is not None:
            return self.returncode
        pid, status, usage = os.wait4(self.pid, os.WNOHANG)
        if pid == 0:
            return None
        self.returncode = os.waitstatus_to_exitcode(status)
        self._child.returncode = self.returncode
        cost = {"backend": "wait4", "command": self.command, "pid": self.pid,
            "returncode": self.returncode, "wall_seconds": time.monotonic() - self.started,
            "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
            "cpu_seconds": usage.ru_utime + usage.ru_stime,
            "max_rss_kib": peak_rss_kib(usage.ru_maxrss, sys.platform)}
        with self.receipt.open("x") as destination:
            destination.write(json.dumps(cost, indent=2, allow_nan=False) + "\n")
        return self.returncode

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while self.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(self.command, timeout)
            time.sleep(min(0.01, remaining))
        return self.returncode

    def kill(self):
        if self.poll() is None:
            # An unreaped child retains its PID, even if it exits between poll
            # and kill. This cannot signal a recycled, unrelated PID.
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
