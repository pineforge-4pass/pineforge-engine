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
    "scratch_capacity_exceeded": 67,
    "scratch_configuration_invalid": 68,
    "scratch_io_failed": 69,
}
V190_ADDITIONS = {
    "diagnostic_budget_exceeded": 70,
    "report_budget_exceeded": 71,
    "report_channel_failed": 72,
    "run_deadline_exceeded": 73,
    "run_isolation_unavailable": 74,
    "run_process_refused": 75,
    "run_producer_failed": 76,
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
    assert {name: current.get(name) for name in V190_ADDITIONS} == V190_ADDITIONS
    assert set(current.values()) == set(range(77))
    header = (ROOT / HEADER).read_text()
    assert re.search(r"kRunFailureCodeCount = ([0-9]+);", header).group(1) == "77"


def test_run_failure_generator_appends_after_released_numbers():
    released = released_numbers()
    catalogue, _ = registry.load_catalog(ROOT)
    generated = numbers(registry.render_codes_header(catalogue))
    assert {name: generated.get(name) for name in released} == released
    assert {name: generated.get(name) for name in V170_ADDITIONS} == V170_ADDITIONS
    assert {name: generated.get(name) for name in V190_ADDITIONS} == V190_ADDITIONS
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


def test_report_channel_catalog_contract():
    # Independent literals from the report-channel contract's catalog table.
    # The released 1.8.0 is already the baseline; these rows target 1.9.0.
    integer = {"kind": "integer"}
    optional_integer = {"kind": "integer", "optional": True}
    expected = {
        "report_budget_exceeded": (
            "resource",
            {
                "channel": {"kind": "vocab", "values": ["json", "aggregate"]},
                "cap_bytes": integer, "used_bytes": integer, "requested_bytes": integer,
            },
            "The report exceeds its admitted byte budget.",
        ),
        "diagnostic_budget_exceeded": (
            "resource",
            {"cap_bytes": integer, "used_bytes": integer, "requested_bytes": integer},
            "The diagnostics exceed their admitted byte budget.",
        ),
        "report_channel_failed": (
            "resource",
            {
                "channel": {"kind": "vocab", "values": [
                    "report", "sink", "candidate", "diagnostic", "status",
                ]},
                "reason": {"kind": "vocab", "values": [
                    "setup", "io", "peer_closed", "deadline", "protocol",
                ]},
            },
            "A report transport could not complete.",
        ),
        "run_isolation_unavailable": (
            "unsupported",
            {"reason": {"kind": "vocab", "values": ["platform", "policy", "descriptor"]}},
            "The requested report isolation is unavailable.",
        ),
        "run_process_refused": (
            "resource",
            {"operation": {"kind": "vocab", "values": ["fork", "vfork", "clone", "clone3"]}},
            "The native process attempted forbidden process creation.",
        ),
        "run_deadline_exceeded": (
            "resource",
            {"stage": {"kind": "vocab", "values": [
                "prepare", "compile", "run", "finalize", "publish",
            ]}},
            "The admitted channel lifetime expired.",
        ),
        "run_producer_failed": (
            "resource",
            {
                "reason": {"kind": "vocab", "values": ["exit", "signal", "claim"]},
                "exit_status": optional_integer, "signal": optional_integer,
            },
            "The native report producer did not complete successfully.",
        ),
    }
    catalogue, _ = registry.load_catalog(ROOT)
    assert len(catalogue["codes"]) == 76
    for name, (cls, args, english) in expected.items():
        entry = catalogue["codes"].get(name)
        assert entry is not None, name
        assert entry["class"] == cls, name
        assert entry["retryable"] is False, name
        assert entry["since"] == "1.9.0", name
        assert entry["args"] == args, name
        assert entry["english"] == [english], name
