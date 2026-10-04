"""New metadata never authorizes an unproven stream before durable startup."""
import subprocess
from pathlib import Path
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
        for index, argument in enumerate(libraries):
            name, library = argument.split("=", 1)
            declaration = "process_orders_on_close" if name.startswith("refused_pooc_") else "request.security"
            contexts = [("bars", "1")] if name.startswith("refused_") else [("ticks", "1")]
            if name == "pooc_dual_stop":
                contexts.append(("bars", "5"))
            for mode, timeframe in contexts:
                if name == "varip":
                    declaration = "intrabar_persistence"
                elif not name.startswith("refused_"):
                    declaration = "process_orders_on_close" if name.startswith("pooc_") else "request.security"
                ledger = root / f"{index}-{mode}-{timeframe}.sqlite"
                controls = root / f"{index}-{mode}-{timeframe}.control"
                health = root / f"{index}-{mode}-{timeframe}.status"
                result = subprocess.run([runner, "run", "--strategy", library, "--warmup", str(warmup),
                    "--feed", str(feed), "--ledger", str(ledger), "--mode", mode,
                    "--script-tf", timeframe, "--symbol", "BINANCE:ETHUSDT.P",
                    "--control-dir", str(controls), "--status-file", str(health)],
                    capture_output=True, text=True, timeout=30)
                assert result.returncode == 1 and declaration in result.stderr, (name, result)
                assert not ledger.exists() and not list(root.glob(ledger.name + "*"))
                assert not controls.exists() and not health.exists()
                print(f"confirmed refusal {name} mode={mode} script={timeframe}: {declaration} before ledger/control/status PASS")


if __name__ == "__main__":
    main()
