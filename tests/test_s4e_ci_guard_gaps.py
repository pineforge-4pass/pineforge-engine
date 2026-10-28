"""Late PR-fast rewrites and foreign lane properties."""
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_cr150_cmake_d as checker
import ci_preflight
from test_ci_preflight import CI_SOURCES

FAST_FINDING = "PR fast rows must keep one pinned CMake label list and executable build target"
INCLUDE_LOOP = (
    'file(GLOB CR150_LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake")\n'
    'list(SORT CR150_LANE_CMAKE)\n'
    'foreach(cr150_lane_file IN LISTS CR150_LANE_CMAKE)\n'
    '    include("${cr150_lane_file}")\n'
    'endforeach()\n'
)


@pytest.mark.parametrize("property_", ["SKIP_RETURN_CODE 1", 'LABELS "slow"'])
def test_fast_rows_cannot_be_rewritten_after_validation(property_):
    print("workflow guard inputs:\n" + "\n".join(str(ROOT / path) for path in CI_SOURCES),
          flush=True)
    sources = [(ROOT / path).read_text() for path in CI_SOURCES]
    block = ci_preflight.FAST_TEST_REGISTRATION
    assert sources[3].count(block) == 1
    # A terminal block is the positive control, regardless of where BASE
    # currently places it. No CMake configuration or repository copy runs.
    sources[3] = sources[3].replace(block, "", 1).rstrip() + "\n" + block
    assert FAST_FINDING not in ci_preflight.ci_workflow_findings(*sources)
    sources[3] += f"set_tests_properties(test_ta PROPERTIES {property_})\n"
    assert FAST_FINDING in ci_preflight.ci_workflow_findings(*sources)


@pytest.fixture
def lane_tree(tmp_path, monkeypatch):
    tests = tmp_path / "tests"
    lanes = tests / "cr150.d"
    lanes.mkdir(parents=True)
    cmake = tests / "CMakeLists.txt"
    cmake.write_text(
        'add_test(NAME test_ta COMMAND ${Python3_EXECUTABLE} main.py)\n' + INCLUDE_LOOP)
    (lanes / "other.cmake").write_text(
        'add_test(NAME test_other COMMAND ${Python3_EXECUTABLE} other.py)\n')
    lane = lanes / "owner.cmake"
    lane.write_text('add_test(NAME test_owner COMMAND ${Python3_EXECUTABLE} owner.py)\n')
    monkeypatch.setattr(checker, "ROOT", tmp_path)
    monkeypatch.setattr(checker, "CMAKE", cmake)
    monkeypatch.setattr(checker, "LANE_DIR", lanes)
    # Reuse the existing policy's presence witness; do not create instructions.
    monkeypatch.setattr(checker, "README", ROOT / "tests/cr150.d/README.md")
    assert checker.main() == 0
    return lane


def test_lane_checker_accepts_the_pinned_terminal_fast_validation(lane_tree):
    cmake = checker.CMAKE
    cmake.write_text(cmake.read_text() + ci_preflight.FAST_TEST_REGISTRATION)
    assert checker.main() == 0


@pytest.mark.parametrize("name", ["test_ta", "test_other"])
@pytest.mark.parametrize("property_", ["TIMEOUT 30", "SKIP_RETURN_CODE 1", 'LABELS "slow"'])
def test_foreign_test_properties_are_refused_in_a_lane_file(lane_tree, capsys, name, property_):
    with lane_tree.open("a") as file:
        file.write(f"set_tests_properties({name} PROPERTIES {property_})\n")
    assert checker.main() == 1
    output = capsys.readouterr()
    assert "is not owned by" in output.err
    assert str(lane_tree) in output.err


@pytest.mark.parametrize("value", ["0", "1", "-1", '"${skip_code}"'])
def test_lane_skip_value_must_be_the_receipt_sentinel(lane_tree, capsys, value):
    original = lane_tree.read_text()
    lane_tree.write_text(original + "set_tests_properties(test_owner PROPERTIES SKIP_RETURN_CODE 77)\n")
    assert checker.main() == 0
    lane_tree.write_text(original + f"set_tests_properties(test_owner PROPERTIES SKIP_RETURN_CODE {value})\n")
    assert checker.main() == 1
    assert "SKIP_RETURN_CODE must be 77" in capsys.readouterr().err
