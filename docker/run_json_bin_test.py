"""Binary preflight discovery entry for tests/test_run_json_bin.py.

CTest names the implementation under tests/ so the private-fixture controls
copy the actual driver into their source mirror. Preflight keeps its Docker
glob and collects the same test functions through this compatibility entry.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

from test_run_json_bin import *  # noqa: F401,F403,E402
