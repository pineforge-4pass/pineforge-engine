"""The Python assembly half of DESIGN acceptance (g), on real selected runs.

Tracing starts at run_backtest_full, AFTER the CSV's allowed OHLCV allocation,
and ends once the complete JSON temporary is prepared, before stdout's fixed
1 MiB copier. Native allocations are outside tracemalloc: CORE-TOP must also
run the whole-process (g) RSS proof after L17b/L8/L19 integration. This row
specifically catches the prior per-point dictionaries and StringIO growth.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

from test_run_json_selected_sink import NATIVE_DRIVER, ROOT, native_argv


def test_selected_assembly_python_memory_is_flat(tmp_path):
    module = os.environ.get("PINEFORGE_L9S_STRATEGY_SO")
    if not module:
        pytest.skip("SKIP lab module not supplied: PINEFORGE_L9S_STRATEGY_SO; CTest wires it")
    assert Path(module).is_file(), module
    peaks = []
    for count in (1_000, 100_000, 1_000_000):
        argv = native_argv(tmp_path, module, count=count, empty=True)
        with open(os.devnull, "w") as destination:
            result = subprocess.run(
                [sys.executable, "-B", "-c", NATIVE_DRIVER, str(ROOT / "docker"),
                 str(ROOT / "docker/run_json.py"), "memory", "--so", str(module), *argv],
                cwd=ROOT, stdout=destination, stderr=subprocess.PIPE, text=True, timeout=1800)
        assert result.returncode == 0, (count, result.returncode, result.stderr)
        sample = json.loads(result.stderr)
        peak = sample["python_peak_bytes"]
        assert isinstance(peak, int) and peak > 0, sample
        peaks.append(peak)
    assert max(peaks) - min(peaks) <= 1024 * 1024, dict(zip((1_000, 100_000, 1_000_000), peaks))
