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
            pooc = name.startswith(("pooc_", "refused_pooc_")) or name == "htf15_sma_pooc"
            declaration = "process_orders_on_close" if pooc else "request.security"
            contexts = [("bars", "1", [])] if name.startswith("refused_") or pooc else [("ticks", "1", [])]
            if not name.startswith("refused_"):
                if pooc:
                    contexts.append(("ticks", "1", []))
                contexts.extend(("bars", "1", options) for options in (
                    ["--timezone", "Asia/Taipei"], ["--chart-timezone", "America/New_York"],
                    ["--session", "0930-1600"]))
            if pooc:
                contexts.extend(("bars", "1", ["--override", setting]) for setting in (
                    "slippage=1", "process_orders_on_close=false", "process_orders_on_close=true"))
            if name == "pooc_dual_stop":
                contexts.append(("bars", "5", []))
            if name == "varip":
                contexts.append(("bars", "5", []))
            if name == "htf60_close":
                contexts.append(("bars", "60", []))
            for context_index, (mode, timeframe, options) in enumerate(contexts):
                if name == "varip":
                    declaration = "intrabar_persistence"
                elif not name.startswith("refused_"):
                    declaration = "process_orders_on_close" if pooc else "request.security"
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
