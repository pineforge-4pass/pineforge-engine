"""Finished trade text, opt-in parsing, and unchanged ordinary runner output."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
from tempfile import mkdtemp


def checked(command, *, success=True):
    result = subprocess.run(command, capture_output=True)
    assert (result.returncode == 0) == success, (
        command, result.returncode, result.stdout, result.stderr)
    return result


def rejected(command, message):
    result = checked(command, success=False)
    # A rejected argument exits 1 before emitting a report or operational summary.
    assert result.returncode == 1 and result.stdout == b""
    assert message in result.stderr


def default_bytes(executable, version):
    result = checked([executable, "--primary-wire"])
    # The existing E/T descriptors fix all names, types, units, ordering and LF framing.
    header = (
        '{"schema":"pf-report-sink","version":1,"run_id":"default","engine":'
        + json.dumps(version) + ',"records":['
        '{"type":"E","fields":["time_ms","equity","open_profit"],'
        '"types":["i64","f64?","f64?"],'
        '"units":["unix_ms","account_ccy","account_ccy"]},'
        '{"type":"T","fields":["n","side","entry_time","exit_time","entry_price",'
        '"exit_price","qty","pnl","pnl_pct","max_runup","max_drawdown","commission",'
        '"entry_bar_index","exit_bar_index","open_at_end","entry_incarnation",'
        '"close_cause","entry_id","exit_id","exit_comment"],'
        '"types":["i64","str","i64","i64","f64?","f64?","f64?","f64?","f64?",'
        '"f64?","f64?","f64?","i64","i64","bool","u64","i64","str","str","str"],'
        '"units":["","","unix_ms","unix_ms","quote_ccy","quote_ccy","qty",'
        '"account_ccy","percent","account_ccy","account_ccy","account_ccy",'
        '"script_bar","script_bar","","","","","",""]}]}\n')
    # Three flat-price bars preserve 10000 equity; entry/close fills are bars 1/2, qty 1.
    records = (
        '["E",1704067200000,10000.0,0.0]\n'
        '["E",1704067260000,10000.0,0.0]\n'
        '["E",1704067320000,10000.0,0.0]\n'
        '["T",1,"long",1704067260000,1704067320000,100.0,100.0,1.0,'
        '0.0,0.0,0.0,0.0,0.0,1,2,false,1,1,"L","__close__L",'
        '"digits 3678.7059999999997"]\n')
    prefix = (header + records).encode()
    # The frozen trailer counts three E rows and one T; its hash excludes itself.
    trailer = (
        '{"trailer":"pf-report-sink","version":1,"counts":{"E":3,"T":1},'
        '"sha256":"' + hashlib.sha256(prefix).hexdigest() + '"}\n').encode()
    assert result.stdout == prefix + trailer
    print("PASS default primary bytes: literal E/T schema, records and trailer framing")


def main():
    if sys.argv[1] == "--default-bytes":
        default_bytes(*sys.argv[2:])
        return
    runner, library, directory = sys.argv[1:]
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    # Keep each invocation's artifacts; an existing output is intentionally refused.
    root = Path(mkdtemp(prefix="trade-text-", dir=root))
    warmup = root / "warmup.csv"
    feed = root / "feed.jsonl"
    warmup.write_text(
        "timestamp,open,high,low,close,volume\n"
        "1704067200000,100,100,100,100,1\n", encoding="utf-8")
    feed.write_text(
        '{"type":"bar","bar":{"ts_open":1704067260000,"o":100,"h":100,"l":100,"c":100,"v":1}}\n'
        '{"type":"bar","bar":{"ts_open":1704067320000,"o":100,"h":100,"l":100,"c":100,"v":1}}\n',
        encoding="utf-8")

    def command(name, *options):
        return [
            runner, "run", "--strategy", library, "--warmup", str(warmup),
            "--feed", str(feed), "--ledger", str(root / name),
            "--mode", "bars", "--script-tf", "1", "--symbol", "TEST:TEXT",
            *options,
        ]

    ordinary = checked(command("ordinary.sqlite3"))
    # The only JSONL artifact after the default run is the input feed.
    assert sorted(path.name for path in root.glob("*.jsonl")) == ["feed.jsonl"]
    companion = root / "texts.jsonl"
    enabled = checked(command("enabled.sqlite3", "--trade-text", str(companion)))
    # The separate option adds no bytes to the existing summary stream.
    assert enabled.stdout == ordinary.stdout
    summary = json.loads(ordinary.stdout)
    # Two input bars follow warmup; no webhook target means no deliveries or failures.
    assert summary["inputs_committed"] == 2
    assert summary["inputs_processed"] == 2
    assert summary["webhooks_delivered"] == 0
    assert summary["webhook_failures"] == 0
    assert summary["webhooks_pending"] == 0
    run_id = summary["deployment"] + ":2"
    lines = companion.read_bytes().splitlines(keepends=True)
    # Header + one closed trade + trailer give three complete LF-terminated lines.
    assert len(lines) == 3 and all(line.endswith(b"\n") for line in lines)
    assert json.loads(lines[0]) == {
        "schema": "pf-trade-text", "version": 1, "run_id": run_id,
        "fields": ["run_id", "row_index", "entry_incarnation", "entry_id",
                   "exit_id", "entry_comment", "exit_comment"],
    }
    # The bar-0 L entry fills on bar 1; its L close fills on bar 2 with the exact digits.
    assert json.loads(lines[1]) == [
        "T", run_id, 0, 1, "L", "__close__L", "", "digits 3678.7059999999997"]
    assert json.loads(lines[1])[4].encode() == b"L"
    assert json.loads(lines[1])[7].encode() == b"digits 3678.7059999999997"
    trailer = json.loads(lines[2])
    assert trailer["trailer"] == "pf-trade-text"
    assert trailer["version"] == 1
    assert trailer["counts"] == {"T": 1}
    # SHA-256 independently covers the literal header/record framing plus both LFs.
    assert trailer["sha256"] == hashlib.sha256(b"".join(lines[:2])).hexdigest()

    # Full and delta modes must also retain every existing stdout byte.
    for mode in ("--report-jsonl=full", "--report-jsonl=delta"):
        name = mode.split("=")[1]
        off = checked(command(name + "-off.sqlite3", mode))
        on = checked(command(name + "-on.sqlite3", mode, "--trade-text",
                             str(root / (name + ".jsonl"))))
        assert on.stdout == off.stdout

    # Duplicate/missing/empty and colliding paths fail without replacing the finished file.
    before = companion.read_bytes()
    rejected(command("exists.sqlite3", "--trade-text", str(companion)),
             b"trade-text requires a new output path")
    rejected(command("empty.sqlite3", "--trade-text", ""),
             b"trade-text path must not be empty")
    rejected(command("missing.sqlite3", "--trade-text"),
             b"missing option value: --trade-text")
    rejected(command("duplicate.sqlite3", "--trade-text", str(root / "a"),
                     "--trade-text", str(root / "b")),
             b"duplicate option: --trade-text")
    rejected(command("overlap.sqlite3", "--trade-text", str(warmup)),
             b"trade-text must not replace a ledger or input artifact")
    assert companion.read_bytes() == before
    print("PASS trade text runner: digits, binding, digest and default/full/delta stdout")


if __name__ == "__main__":
    main()
