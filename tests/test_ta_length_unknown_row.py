"""An unknown selector must be refused by the length-test executable."""
import subprocess
import sys


if __name__ == "__main__":
    try:
        control = subprocess.run([sys.argv[1], "pivothigh"], capture_output=True, text=True)
    except OSError as error:
        sys.exit(f"NOT COMPARED: could not launch the length test: {error}")
    if (control.returncode != 0 or control.stderr or control.stdout.splitlines() != [
            "row pivothigh", "PASS pivothigh first bar: na",
            "PASS pivothigh first bar recalculated: na"]):
        sys.exit(f"NOT COMPARED: pivothigh control: exit {control.returncode}, "
                 f"stdout={control.stdout!r}, stderr={control.stderr!r}")
    row = "pivot-high"
    child = subprocess.run([sys.argv[1], row], capture_output=True, text=True)
    if child.returncode != 2 or child.stderr.strip() != f"unknown row: {row}" or child.stdout:
        sys.exit(f"FAIL {row}: exit {child.returncode}, "
                 f"stdout={child.stdout!r}, stderr={child.stderr!r}")
    print(f"PASS {row}: refused as an unknown row")
