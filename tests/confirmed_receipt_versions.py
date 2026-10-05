"""Unknown optional receipts preserve ordinary eligibility, never new admission."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    runner, *libraries = sys.argv[1:]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        warmup = root / "warmup.csv"
        warmup.write_text("timestamp,open,high,low,close,volume\n1577836800000,100,101,99,100,10\n")
        feed = root / "feed.jsonl"
        feed.write_text("")
        for name, library in zip(("ordinary", "request", "pooc", "missing"), libraries):
            ledger = root / (name + ".sqlite")
            result = subprocess.run([runner, "run", "--strategy", library, "--warmup", str(warmup),
                                     "--feed", str(feed), "--ledger", str(ledger), "--mode", "bars"],
                                    capture_output=True, text=True, timeout=30)
            if name == "ordinary":
                assert result.returncode == 0, (name, result)
                assert ledger.exists()
            else:
                declaration = {"request": "request.security", "pooc": "process_orders_on_close",
                               "missing": "confirmed-bar capabilities extension lacks version"}[name]
                assert result.returncode == 1 and declaration in result.stderr, (name, result)
                assert not list(root.glob(ledger.name + "*"))
            print(f"confirmed receipt version {name}: PASS", flush=True)


if __name__ == "__main__":
    main()
