"""Released C++ enum numbers are immutable; execute on the lab box."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = "include/pineforge/run_failure_codes.hpp"
sys.path.insert(0, str(ROOT / "scripts"))

import check_run_failure_codes as registry


def numbers(text):
    body = text.split("enum class RunFailureCode : std::uint16_t {", 1)[1].split("};", 1)[0]
    rows = re.findall(r"^\s+([a-z][a-z0-9_]*) = ([0-9]+),$", body, re.MULTILINE)
    assert rows and len(rows) == len({name for name, _ in rows})
    return {name: int(number) for name, number in rows}


def released_numbers():
    # Only this fixed, repository-owned path is followed, never an argv path.
    print(f"release enum input: v1.6.3:{HEADER}", flush=True)
    result = subprocess.run(
        ["git", "-C", str(ROOT), "show", f"v1.6.3:{HEADER}"],
        capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    released = numbers(result.stdout)
    assert released["none"] == 0 and max(released.values()) == 64
    return released


def test_run_failure_code_numbers_stable():
    released = released_numbers()
    current = numbers((ROOT / HEADER).read_text())
    assert {name: current.get(name) for name in released} == released
    assert len(set(current.values())) == len(current)
    assert all(value > 64 for name, value in current.items() if name not in released)


def test_run_failure_generator_appends_after_released_numbers():
    released = released_numbers()
    catalogue, _ = registry.load_catalog(ROOT)
    generated = numbers(registry.render_codes_header(catalogue))
    assert {name: generated.get(name) for name in released} == released
    for path, rendered in registry.generated(catalogue).items():
        assert (ROOT / path).read_text(encoding="utf-8") == rendered, str(path)
    # A future alphabetically earlier addition cannot shift any existing code.
    future = {**catalogue, "codes": {
        **catalogue["codes"],
        "aaa_future_code": {
            "class": "unsupported", "retryable": False, "args": {},
            "english": ["Future mode."], "description": "A future mode.",
            "since": "99.0.0",
        },
    }}
    appended = numbers(registry.render_codes_header(future))
    assert {name: appended.get(name) for name in generated} == generated
    assert appended["aaa_future_code"] == max(generated.values()) + 1


def test_run_failure_generator_orders_later_release_groups_numerically():
    catalogue, _ = registry.load_catalog(ROOT)
    current = numbers(registry.render_codes_header(catalogue))
    future = {**catalogue, "codes": dict(catalogue["codes"])}
    for name, since in (("aaa_tenth_release", "10.0.0"),
                        ("zzz_second_release", "2.0.0"),
                        ("aaa_second_release", "2.0.0")):
        future["codes"][name] = {
            "class": "unsupported", "retryable": False, "args": {},
            "english": ["Future mode."], "description": "A future mode.",
            "since": since,
        }
    appended = numbers(registry.render_codes_header(future))
    assert {name: appended.get(name) for name in current} == current
    first = max(current.values()) + 1
    assert [appended[name] for name in (
        "aaa_second_release", "zzz_second_release", "aaa_tenth_release",
    )] == [first, first + 1, first + 2]
