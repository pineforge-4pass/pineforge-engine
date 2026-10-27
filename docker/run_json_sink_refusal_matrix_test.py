"""CR-397 strict-only source preflight; the built matrix runs both routes."""
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

from test_run_json_sink_refusal_matrix import *  # noqa: F401,F403,E402


@pytest.fixture(params=("strict",), ids=lambda name: "route-" + name)
def route(request, monkeypatch):
    """Reuse every matrix row without requiring a module before the build."""
    return select_route(request.param, monkeypatch, request)
