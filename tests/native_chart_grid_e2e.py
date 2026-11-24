"""Real finer-input chart writers. Run with --help for individual raw receipts."""
import argparse
from datetime import date, datetime, time, timedelta
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
from zoneinfo import ZoneInfo


MIN = 60_000
ENGINE = ("batch", "api-bars", "api-ticks")
CLI = ("cli-bars", "cli-ticks")
PINE = ("pine-ticks", "pine-bars-refusal")
ROUTES = ENGINE + CLI + PINE

# Handwritten local opens and traded minute counts. The input is generated
# separately from its own opening and closing wall times. The list of traded
# bar opens omits the wholly closed cell before the first NY 45/60 bar.
NY45 = ("1745 1830 1915 2000 2045 2130 2215 2300 2345 0030 0115 0200 0245 "
        "0330 0415 0500 0545 0630 0715 0800 0845 0930 1015 1100 1145 1230 "
        "1315 1400 1445 1530 1615", [30] + [45] * 30)
NY240 = ("1700 2100 0100 0500 0900 1300", [180] + [240] * 5)
NY60 = ("1800 1900 2000 2100 2200 2300 0000 0100 0200 0300 0400 0500 0600 "
        "0700 0800 0900 1000 1100 1200 1300 1400 1500 1600", [60] * 23)
CHI45 = ("1700 1745 1830 1915 2000 2045 2130 2215 2300 2345 0030 0115 0200 "
         "0245 0330 0415 0500 0545 0630 0715 0800 0845 0930 1015 1100 1145 "
         "1230 1315 1400 1445 1530", [45] * 30 + [30])
CHI240 = ("1700 2100 0100 0500 0900 1300", [240] * 5 + [180])
CHI60 = ("1700 1800 1900 2000 2100 2200 2300 0000 0100 0200 0300 0400 0500 "
         "0600 0700 0800 0900 1000 1100 1200 1300 1400 1500", [60] * 23)

# The date is the civil date of the first traded minute. Masked NY includes
# Thursday EST, skips the closed weekend, and reopens Sunday EDT for Monday.
CASES = {
    "ny45": ("New_York", "1800-1700", 45, NY45, ("2025-03-10",)),
    "ny240": ("New_York", "1800-1700", 240, NY240, ("2025-03-10",)),
    "ny60": ("New_York", "1800-1700", 60, NY60, ("2025-03-10",)),
    "ny45-mask": ("New_York", "1800-1700:23456", 45, NY45, ("2025-03-06", "2025-03-09")),
    "ny240-mask": ("New_York", "1800-1700:23456", 240, NY240, ("2025-03-06", "2025-03-09")),
    "chi45": ("Chicago", "1700-1600:23456", 45, CHI45, ("2025-03-06", "2025-03-09")),
    "chi240": ("Chicago", "1700-1600:23456", 240, CHI240, ("2025-03-06", "2025-03-09")),
    "chi60": ("Chicago", "1700-1600:23456", 60, CHI60, ("2025-03-06", "2025-03-09")),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def digest_file(path):
    return digest(path.read_bytes())


class NotCompared(Exception):
    def __init__(self, reason, actual, expected):
        self.reason, self.actual, self.expected = reason, actual, expected


def gate(reason, actual, expected):
    if actual != expected:
        raise NotCompared(reason, actual, expected)


def at(day, hhmm, zone):
    return int(datetime.combine(day, time(int(hhmm[:2]), int(hhmm[2:])), zone).timestamp() * 1000)


def hhmm(stamp, case):
    return datetime.fromtimestamp(stamp / 1000, ZoneInfo("America/" + CASES[case][0])).isoformat()


def input_bar(stamp):
    p = 100 + (stamp // MIN) % 4096
    return stamp, p, p + 7, p - 3, p + 2, 4 * (1 + (stamp // MIN) % 11)


def fold(label, rows):
    return (label, rows[0][1], max(row[2] for row in rows), min(row[3] for row in rows),
            rows[-1][4], sum(row[5] for row in rows))


def fixture(case, stale=False):
    zone, session, width, (opens, counts), dates = CASES[case]
    timezone = "America/" + zone
    zone = ZoneInfo(timezone)
    days = [date.fromisoformat(value) for value in dates]
    if stale:
        days = [day - timedelta(days=7) for day in days]
    one_day = timedelta(days=1)
    warm_day = days[0] - one_day
    witness_day = days[-1] + one_day
    ny = session.startswith("1800")
    start, close = ("1800", "1700") if ny else ("1700", "1600")
    history_start, history_end = at(warm_day, start, zone), at(days[0], close, zone)
    warm = [input_bar(ts) for ts in range(history_start, history_end, MIN)]
    live, expected = [], []
    for day in days:
        first, stop = at(day, start, zone), at(day + one_day, close, zone)
        source = [input_bar(ts) for ts in range(first, stop, MIN)]
        gate("fixture in-session minutes", len(source), 1380)
        gate("oracle covers all input minutes", sum(counts), len(source))
        gate("oracle label count", len(opens.split()), len(counts))
        live.extend(source)
        pos = 0
        for hhmm, size in zip(opens.split(), counts):
            label = at(day + (one_day if hhmm < "1700" else timedelta()), hhmm, zone)
            expected.append(fold(label, source[pos:pos + size]))
            pos += size
    # A single minute of the next session makes the prior tail observable on
    # a writer that waits for the successor. It cannot complete its own chart
    # bar at any of the tested widths.
    live.append(input_bar(at(witness_day, start, zone)))
    span = (at(days[0], "1700", zone), at(witness_day, "1700", zone))
    gate("fixture start/end/count", (live[0][0], live[-2][0] + MIN, len(live)),
         (at(days[0], start, zone), at(witness_day, close, zone), len(days) * 1380 + 1))
    return timezone, session, width, warm, live, span, expected


def warm_csv(rows):
    return "timestamp,open,high,low,close,volume\n" + "".join(
        ",".join(map(str, bar)) + "\n" for bar in rows)


def frames_for(rows, mode):
    if mode == "bars":
        events = [dict(type="bar", bar=dict(zip(("ts_open", "o", "h", "l", "c", "v"), b)))
                  for b in rows]
    else:
        events, seq = [], 0
        for stamp, o, h, l, c, volume in rows:
            for index, price in enumerate((o, h, l, c)):
                seq += 1
                events.append(dict(type="tick", ts=stamp + 1000 * index, seq=seq,
                                   price=price, qty=volume // 4))
            events.append(dict(type="time", ts=stamp + MIN))
    return [dict(type="batch", events=events[i:i + 120]) for i in range(0, len(events), 120)]


def native_config(session, tz, width, key):
    return dict(run=dict(session_key=key, run_number=1),
                clock=dict(input_tf="1", script_tf=str(width), timezone=tz,
                           session=session, chart_timezone="UTC"),
                instrument=dict(ticker="GRID", tickerid="TEST:GRID", type="cfd",
                                currency="USD", basecurrency="", description="", volumetype=""),
                execution=dict(initial_capital=100000, point_value=1, account_fx=1, price_tick=0.01,
                               slippage_ticks=0, fee_kind="CashPerExecution", fee_value=0,
                               quantity_grid=None, close_execution="NextEligiblePoint",
                               max_abs_units=None, max_open_lots=None, allowed_open_directions=3,
                               initial_margin_fraction=None))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def run_child(root, command, raw_files):
    write_json(root / "command.json", {"argv": list(map(str, command))})
    for path, wanted in raw_files.items():
        gate("input file before launch " + path.name, digest_file(path), wanted)
    with (root / "stdout").open("wb") as out, (root / "stderr").open("wb") as err:
        # stdin belongs to the engine-API probe only. The runner opens its own
        # CSV and JSONL; never substitute a probe for the runner executable.
        if root.joinpath("stdin").is_file():
            with root.joinpath("stdin").open("rb") as stdin:
                result = subprocess.run(command, stdin=stdin, stdout=out, stderr=err, check=False)
        else:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=out,
                                    stderr=err, check=False)
    write_json(root / "process.json", {"rc": result.returncode})
    for path, wanted in raw_files.items():
        gate("input file after launch " + path.name, digest_file(path), wanted)
    return result.returncode, root.joinpath("stdout").read_text(), root.joinpath("stderr").read_text()


def read_bars(stderr, raw_phase):
    bars = []
    for line in stderr.splitlines():
        words = line.split()
        if words[:2] == ["GRID", "BEFORE"]:
            break
        if words[:2] != ["GRID", "BAR"]:
            continue
        gate("callback row columns", len(words), 9)
        if words[2] not in ("history", "live"):
            raise NotCompared("callback phase field", words[2], "history or live")
        if words[2] == raw_phase:
            bars.append((int(words[3]), *map(float, words[4:])))
    return bars


def check_create(stderr, tag, pine):
    gate("loaded observer / compiled build / actual engine contract",
         [line for line in stderr.splitlines() if line.startswith("GRID CREATE ")],
         [f"GRID CREATE {tag} {'pine' if pine else 'native'} 2"])


def artifacts(args, route):
    if route in ENGINE:
        paths = {"probe": args.probe}
    else:
        paths = {"runner": args.runner, "pine" if route in PINE else "native":
                 args.pine if route in PINE else args.native}
    for name, path in paths.items():
        gate(f"{name} artifact", str(path) if path and path.is_file() else None,
             str(path) if path else f"<{name} file>")
    digests = {key: digest_file(path) for key, path in paths.items()}
    if args.receipt:
        receipt = json.loads(args.receipt.read_text())
        gate("build receipt arm", receipt.get("tag"), args.tag)
        for name, path in paths.items():
            gate(f"{name} binary receipt", [str(path), digests[name]], receipt["artifacts"].get(name))
        gate("test driver source receipt", digest_file(Path(__file__)), receipt.get("driver_sha256"))
    return paths, digests


def observe(args, root, route, case, stale=False):
    paths, build = artifacts(args, route)
    tz, session, width, warm, live, span, oracle = fixture(case, stale)
    pending = {}

    def stage(name, content):
        path = root / name
        pending[path] = digest(content.encode())
        path.write_text(content)
        return path

    if route in ENGINE:
        mode = route.replace("api-", "")
        rows = live if route == "batch" else warm + live
        warm_count = len(rows) - len(live)
        stage("stdin", "".join(" ".join(map(str, b)) + "\n" for b in rows))
        command = [paths["probe"], mode, tz, session, str(width), str(warm_count)]
    else:
        mode = "bars" if route.endswith("bars") or route == "pine-bars-refusal" else "ticks"
        pine = route in PINE
        # The Pine CLI requires chart-width warmup; the first eligible open
        # is a legal label on both grids. It lies a whole session before the
        # comparison. Quiet bars synthesized there stay in the raw log.
        first_count = CASES[case][3][1][0]
        warm_path = stage("warmup.csv", warm_csv(
            [fold(warm[0][0], warm[:first_count])] if pine else warm))
        frames = frames_for(live, mode)
        feed = stage("feed.jsonl", "".join(json.dumps(v) + "\n" for v in frames))
        ledger = root / "ledger.sqlite"
        gate("fresh runner ledger", [p.name for p in root.glob("ledger.sqlite*")], [])
        command = [paths["runner"], "run", "--strategy", paths["pine" if pine else "native"],
                   "--mode", mode, "--warmup", warm_path, "--feed", feed, "--ledger", ledger]
        if pine:
            command += ["--input-tf", str(width), "--script-tf", str(width),
                        "--symbol", "TEST:GRID", "--timezone", tz, "--session", session,
                        "--chart-timezone", "UTC", "--syminfo", "type=cfd"]
        else:
            config = stage("config.json", json.dumps(native_config(session, tz, width, "grid-" + case)))
            command += ["--native-config", config, "--input-tf", "1", "--script-tf", str(width)]

    rc, stdout, stderr = run_child(root, command, pending)
    artifact_after = {name: digest_file(path) for name, path in paths.items()}
    gate("artifacts after process exit", artifact_after, build)
    if route != "pine-bars-refusal":
        gate("process exit", rc, 0)
    check_create(stderr, args.tag, route in PINE)
    if route == "pine-bars-refusal":
        errors = [line for line in stderr.splitlines() if line.startswith("pineforge-live: ")
                  and not line.startswith("pineforge-live: warning: ")]
        wanted = (f"pineforge-live: chart delivery for a {width} chart on this session calendar "
                  "is not supported yet: its bars do not tile the calendar's trading days.")
        gate("Pine bars startup rc/diagnostic/artifact",
             (rc, [line.split(". Keep", 1)[0] for line in errors], [p.name for p in root.glob("ledger.sqlite*")],
              read_bars(stderr, "history") + read_bars(stderr, "live")),
             (1, [wanted.rstrip(".")], [], []))
        return {"refusal": errors[0], "rc": rc}

    input_tf = width if route in PINE else 1
    gate("engine run input/script clocks and session",
         [line for line in stderr.splitlines() if line.startswith("GRID CLOCK ")],
         [f"GRID CLOCK {input_tf} {width} {tz} {session} 2"])
    phase = "history" if route == "batch" else "live"
    bars = read_bars(stderr, phase)
    if route in ENGINE:
        want = (f"GRID INPUT {mode} {tz} {session} {width} {warm_count} {len(rows)} "
                f"{rows[0][0]} {rows[-1][0]}")
        gate("processed API input count/span", stdout.strip(), want)
        if route != "batch":
            gate("API warmup and pre-end receipt",
                 (bool(read_bars(stderr, "history")), stderr.count("GRID BEFORE END\n")), (True, 1))
    else:
        summary = json.loads(stdout)
        ledger_gate = (len(frames), len(frames), 0, len(live) * 4 if mode == "ticks" else None)
        gate("runner committed/processed/replayed/sequence receipt",
             tuple(summary.get(key) for key in ("inputs_committed", "inputs_processed", "prefix_skipped",
                                                 "last_tick_sequence")), ledger_gate)
        gate("runner ledger created", ledger.is_file(), True)
        with sqlite3.connect(ledger.as_uri() + "?mode=ro", uri=True) as db:
            deployment = db.execute("SELECT identity FROM metadata WHERE singleton=1").fetchone()
            records = db.execute("SELECT input_index,canonical_json FROM inputs ORDER BY input_index").fetchall()
        gate("runner deployment digest shape", bool(re.fullmatch(r"[0-9a-f]{64}",
              summary.get("deployment") or "")), True)
        gate("runner deployment ledger identity", deployment, (summary["deployment"],))
        received = [(i, json.loads(body)) for i, body in records]
        sha = lambda entries: digest(json.dumps(entries, sort_keys=True).encode())
        gate("runner ledger feed count and ordered SHA", (len(received), sha(received)),
             (len(frames), sha(list(enumerate(frames)))))
        gate("runner warmup callback reached", bool(read_bars(stderr, "history")), True)

    # The measured window starts at 17:00 local even though the NY input
    # starts at 18:00. At the other end the witness is still forming.
    # Keep everything after the window start to catch extra bars. A lazy
    # warmup tail can be closed by the first live input; Pine ticks can also
    # synthesize earlier quiet warmup-day bars.
    if route != "batch":
        bars = [bar for bar in bars if bar[0] >= span[0]]
    gate("fixture nonempty oracle", bool(oracle), True)
    return {"route": route, "case": case, "span": span, "count": len(live), "bars": bars,
            "inputs": {path.name: sha for path, sha in pending.items()}, "build": build}


def compare(name, actual, expected_identity, expected, prefix=None):
    got = actual["bars"]
    identity = {k: actual.get(k) for k in expected_identity}
    # A prior day can have exactly the expected bar count. Reject its
    # provenance before any comparison of the product's labels or OHLCV.
    if identity != expected_identity:
        raise NotCompared("run/build/feed/window identity",
                          {"identity": identity, "bars": len(got)},
                          {"identity": expected_identity, "bars": len(expected)})
    if prefix is None:
        gate("nonempty callback count over full window", len(got), len(expected))
    else:
        if len(got) < prefix:
            raise NotCompared("confirmed prefix count", len(got), f">={prefix}")
        got, expected = got[:prefix], expected[:prefix]
    if any(bar[0] < expected_identity["span"][0] for bar in got):
        raise NotCompared("callback window start/count", (got[0][0], got[-1][0], len(got)),
                          {"min_label": expected_identity["span"][0], "bars": len(expected)})
    mismatches = [dict(index=i, actual=a, expected=e) for i, (a, e) in
                  enumerate(zip(got, expected)) if a != e]
    if mismatches:
        for diff in mismatches:
            diff["actual_open"] = hhmm(diff["actual"][0], expected_identity["case"])
            diff["expected_open"] = hhmm(diff["expected"][0], expected_identity["case"])
        print(f"{name} FAIL compared={len(got)} mismatches={json.dumps(mismatches)}", flush=True)
        return 1
    case = expected_identity["case"]
    print(f"{name} PASS compared={len(got)} first={hhmm(got[0][0], case)} {got[0]} "
          f"last={hhmm(got[-1][0], case)} {got[-1]}", flush=True)
    return 0


def exercise(args, root, route, case, variant):
    name = f"{route}:{case}:{variant}"
    target = root / name.replace(":", ".")
    print(f"{name} raw={target}", flush=True)
    try:
        target.mkdir(exist_ok=False)
        observed = observe(args, target, route, case)
        write_json(target / "observed.json", observed)
        if route == "pine-bars-refusal":
            print(f"{name} PASS rc={observed['rc']} {observed['refusal']}", flush=True)
            return 0
        *_, live, span, expected = fixture(case)
        write_json(target / "expected.json", expected)
        identity = dict(route=route, case=case, count=len(live), span=span,
                        inputs=observed["inputs"], build=observed["build"])
        if variant == "empty":
            observed = {**observed, "bars": []}
        if variant == "stale":
            prior = target / "prior"
            prior.mkdir()
            observed = observe(args, prior, route, case, stale=True)
            write_json(prior / "observed.json", observed)
            gate("stale control bar count before comparison", len(observed["bars"]), len(expected))
        write_json(target / "comparison.json", {"identity": identity,
                   "observed_bars": len(observed["bars"]), "expected_bars": len(expected)})
        return compare(name, observed, identity, expected, 3 if variant == "prefix" else None)
    except NotCompared as error:
        print(f"{name} NOT COMPARED {error.reason} actual={error.actual!r} "
              f"expected={error.expected!r}", flush=True)
        return 2
    except (OSError, ValueError, OverflowError, sqlite3.Error, KeyError, IndexError, TypeError) as error:
        print(f"{name} NOT COMPARED setup/receipt actual={error!r} expected=readable complete evidence",
              flush=True)
        return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("probe", "runner", "native", "pine", "receipt", "raw"):
        parser.add_argument("--" + option, type=lambda value: Path(value).resolve())
    parser.add_argument("--tag", default="ctest")
    parser.add_argument("--route", choices=("all",) + ROUTES, default="all")
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--variant", choices=("full", "prefix", "empty", "stale"))
    args = parser.parse_args()
    if args.variant and not (args.route != "all" and args.case):
        parser.error("--variant requires an individual --route and --case")
    if args.variant in ("empty", "stale") and args.route == "pine-bars-refusal":
        parser.error("negative controls require a chart-producing row")
    if args.tag != "ctest" and not args.receipt:
        parser.error("a fixed build tag requires --receipt")
    if args.raw is None:
        args.raw = Path(tempfile.mkdtemp(prefix="pineforge-chart-grid-"))
    else:
        args.raw.mkdir(parents=True, exist_ok=True)
    rows = []
    for route in ROUTES if args.route == "all" else (args.route,):
        cases = args.case and (args.case,) or (("ny45", "ny240") if route in PINE else CASES)
        for case in cases:
            rows.append((route, case, args.variant or "full"))
    results = [exercise(args, args.raw, *row) for row in rows]
    return max(results)


if __name__ == "__main__":
    raise SystemExit(main())
