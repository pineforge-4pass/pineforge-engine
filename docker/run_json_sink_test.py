"""CR-150 L9A preflight entry for tests/test_run_json_sink.py.

CTest names the implementation under tests/ so the private-fixture controls
copy the actual driver into their source mirror. Preflight retains its Docker
glob and collects the same tests through this compatibility entry.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

from test_run_json_sink import *  # noqa: F401,F403,E402
