#!/usr/bin/env python3
"""Give both entrypoints the same work path, without altering their source."""

import os
from pathlib import Path
import sys

if len(sys.argv) != 3 or sys.argv[1] != "-d":
    raise RuntimeError(f"unexpected mktemp invocation: {sys.argv[1:]!r}")
work = Path(os.environ["PF_ENTRYPOINT_FIXTURE_WORK"])
work.mkdir()
print(work)
