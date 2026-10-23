"""Released C++ enum numbers are immutable; execute on the lab box."""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = "include/pineforge/run_failure_codes.hpp"
RELEASED_FIXTURE = Path(__file__).with_name("run_failure_code_numbers_v1_6_3.json")
V170_ADDITIONS = {
    "report_mode_unsupported": 65,
    "report_sink_failed": 66,
    "report_sink_value_too_large": 67,
    "scratch_capacity_exceeded": 68,
    "scratch_configuration_invalid": 69,
    "scratch_io_failed": 70,
}
sys.path.insert(0, str(ROOT / "scripts"))

import check_run_failure_codes as registry


def numbers(text):
    body = text.split("enum class RunFailureCode : std::uint16_t {", 1)[1].split("};", 1)[0]
    rows = re.findall(r"^\s+([a-z][a-z0-9_]*) = ([0-9]+),$", body, re.MULTILINE)
    assert rows and len(rows) == len({name for name, _ in rows})
    return {name: int(number) for name, number in rows}


def released_numbers():
    # Copied from the released tag, independently of the generator and HEAD.
    # Only fixed, repository-owned paths are followed, never an argv path.
    print(f"release enum fixture input: {RELEASED_FIXTURE}", flush=True)
    released = json.loads(RELEASED_FIXTURE.read_text(encoding="utf-8"))
    assert len(released) == 65
    assert released["none"] == 0 and max(released.values()) == 64
    assert set(released.values()) == set(range(65))

    # Snapshots always use the fixture; available history also checks its source.
    try:
        tag = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--verify", "v1.6.3"],
            capture_output=True, text=True, check=False)
    except FileNotFoundError:
        tag = None
    if tag is not None and tag.returncode == 0:
        print(f"release enum tag input: v1.6.3:{HEADER}", flush=True)
        result = subprocess.run(
            ["git", "-C", str(ROOT), "show", f"v1.6.3:{HEADER}"],
            capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
        assert released == numbers(result.stdout), "fixture differs from v1.6.3 header"
    return released


def test_run_failure_code_numbers_stable():
    released = released_numbers()
    current = numbers((ROOT / HEADER).read_text())
    assert {name: current.get(name) for name in released} == released
    assert len(set(current.values())) == len(current)
    assert all(value > 64 for name, value in current.items() if name not in released)
    assert {name: current.get(name) for name in V170_ADDITIONS} == V170_ADDITIONS


def test_run_failure_generator_appends_after_released_numbers():
    released = released_numbers()
    catalogue, _ = registry.load_catalog(ROOT)
    generated = numbers(registry.render_codes_header(catalogue))
    assert {name: generated.get(name) for name in released} == released
    assert {name: generated.get(name) for name in V170_ADDITIONS} == V170_ADDITIONS
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
