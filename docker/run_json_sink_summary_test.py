"""CR-150 L9B preflight discovery for the named-sink summary contract tests."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

from test_run_json_sink_summary import *  # noqa: F401,F403,E402
