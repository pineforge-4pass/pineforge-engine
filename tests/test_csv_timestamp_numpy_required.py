"""A requested NumPy comparison must fail if the import stops working."""

from pathlib import Path
import subprocess
import sys


def test_numpy_required():
    driver = Path(__file__).with_name("test_issue_932_csv_timestamps.py")
    # Exercise the driver's real command line without uninstalling a package.
    child = subprocess.run(
        [sys.executable, "-B", "-c",
         "import runpy, sys; sys.modules['numpy'] = None; del sys.argv[0]; "
         "runpy.run_path(sys.argv[0], run_name='__main__')", str(driver), "numpy"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if (child.returncode != 1 or
            "NOT COMPARED: NumPy is required for the NumPy CSV timestamp test"
            not in child.stdout.splitlines() or "PASS" in child.stdout):
        raise SystemExit(
            f"FAIL: requested NumPy without NumPy: expected exit 1 and no pass; "
            f"got exit {child.returncode}\n{child.stdout}")


if __name__ == "__main__":
    test_numpy_required()
    print("PASS: requested NumPy without NumPy fails")
