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
            contexts = [("bars", "1", [])] if name.startswith("refused_") else [("ticks", "1", [])]
            if not name.startswith("refused_"):
                contexts.extend(("bars", "1", options) for options in (
                    ["--timezone", "Asia/Taipei"], ["--chart-timezone", "America/New_York"],
                    ["--session", "0930-1600"]))
            if name.startswith("pooc_"):
                contexts.append(("bars", "1", ["--override", "slippage=1"]))
            if name == "pooc_dual_stop":
                contexts.append(("bars", "5", []))
            if name == "varip":
                contexts.append(("bars", "5", []))
            for context_index, (mode, timeframe, options) in enumerate(contexts):
                if name == "varip":
                    declaration = "intrabar_persistence"
                elif not name.startswith("refused_"):
                    declaration = "process_orders_on_close" if name.startswith("pooc_") else "request.security"
                ledger = root / f"{index}-{context_index}.sqlite"
                controls = root / f"{index}-{context_index}.control"
                health = root / f"{index}-{context_index}.status"
                result = subprocess.run([runner, "run", "--strategy", library, "--warmup", str(warmup),
                    "--feed", str(feed), "--ledger", str(ledger), "--mode", mode,
                    "--script-tf", timeframe, "--symbol", "BINANCE:ETHUSDT.P",
                    "--control-dir", str(controls), "--status-file", str(health)] + options,
                    capture_output=True, text=True, timeout=30)
                assert result.returncode == 1 and declaration in result.stderr, (name, result)
                assert not ledger.exists() and not list(root.glob(ledger.name + "*"))
                assert not controls.exists() and not health.exists()
                print(f"confirmed refusal {name} mode={mode} script={timeframe} options={options}: {declaration} before ledger/control/status PASS", flush=True)


if __name__ == "__main__":
    main()
