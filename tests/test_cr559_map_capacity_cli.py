"""Data oracle for cr559_cli_pair.sh; never launches or imports the CLI.

The shell runs the ordinary CLI against one genuinely generated module.
Finite-policy refusals are controls, not evidence of a default-policy bug.
"""

import argparse
import hashlib
import json
from pathlib import Path


T0 = 1704067200000
CASES = {
    "finite_boundary": (50000, 7, 0),
    "finite_refusal": (50001, 7, 1),
    "omitted_a": (50003, 11, 0),
    "zero_a": (50003, 11, 0),
    "omitted_b": (50009, 23, 0),
    "zero_b": (50009, 23, 0),
    "negative_option": (50003, 11, 2),
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n",
                    encoding="utf-8")


def prepare(out):
    # Four ordinary bars; no corpus, feeds, mocks, or runtime stand-ins.
    (out / "feed.csv").write_text(
        "timestamp,open,high,low,close,volume\n"
        f"{T0},100,102,99,101,10\n"
        f"{T0 + 60000},101,103,100,102,11\n"
        f"{T0 + 120000},103,105,102,104,12\n"
        f"{T0 + 180000},107,109,106,108,13\n", encoding="ascii")
    write_json(out / "collection.json", {
        "case_count": len(CASES), "cases": list(CASES),
        "feed_sha256": digest(out / "feed.csv"),
        "historical_baseline_red": "UNRUN; exact c6 has unlimited policy in source",
        "finite_refusal_role": "CONTROL",
    })


def read_case(out, name, expected_exit):
    paths = [out / (name + suffix) for suffix in (".stdout", ".stderr", ".exit")]
    require(all(path.is_file() for path in paths), f"{name}: missing raw artifact")
    stdout, stderr, exit_file = paths
    require(int(exit_file.read_text(encoding="ascii").strip()) == expected_exit,
            f"{name}: unexpected real CLI exit; inspect {exit_file}")
    if name == "negative_option":
        require(b"expected a nonnegative int64 map-pair count" in stderr.read_bytes(),
                "negative option lost its real parser diagnostic")
    else:
        require(stderr.read_bytes() == b"", f"{name}: unexpected stderr")
    lines = stdout.read_text(encoding="utf-8").splitlines()
    require(len(lines) == 1 and bool(lines[0]), f"{name}: expected one JSON line")
    document = json.loads(lines[0], parse_constant=lambda value: reject(value))
    require(isinstance(document, dict) and document.get("engine") == "pineforge",
            f"{name}: not an engine document")
    return document


def reject(value):
    raise AssertionError(f"non-JSON numeric value: {value}")


def check_success(document, pairs, salt):
    require("error" not in document and "code" not in document,
            "success carries a failure")
    require(document["input"]["bars"] == 4, "wrong input bar count")
    require(document["summary"]["bars_processed"] == 4, "incomplete run")
    require(document["applied_inputs"] == {"Pairs": str(pairs), "Salt": str(salt)},
            "CLI did not apply both meaningful inputs")
    trades = document["trades"]
    require(len(trades) == document["summary"]["total_trades"] == 2,
            "map-dependent orders did not both finish")
    # Independent arithmetic: the map's final key was inserted on bar 1,
    # whose close is 102. Size and lookup drive separate actual quantities.
    quantities = sorted([pairs, (pairs - 1) * 3 + salt + 102])
    require(sorted(trade["qty"] for trade in trades) == quantities,
            "size or tail lookup differs; ignored inputs cannot pass")
    for trade in trades:
        require(trade["side"] == "long" and not trade["open_at_end"],
                "unexpected side or unfinished trade")
        require((trade["entry_bar_index"], trade["exit_bar_index"]) == (2, 3),
                "orders did not follow successful insertion on bar 1")
        require((trade["entry_time"], trade["exit_time"]) ==
                (T0 + 120000, T0 + 180000), "wrong trade times")
        require((trade["entry_price"], trade["exit_price"]) == (103, 107),
                "synthetic feed did not reach execution")
        require(trade["commission"] == 0 and trade["pnl"] == 4 * trade["qty"],
                "wrong real fill accounting")
    require(document["summary"]["net_pnl"] == 4 * sum(quantities),
            "summary did not incorporate both map results")
    return quantities


def check(out):
    collection = json.loads((out / "collection.json").read_text(encoding="utf-8"))
    require(collection["case_count"] == len(CASES) and
            collection["cases"] == list(CASES), "wrong collection")
    require(collection["feed_sha256"] == digest(out / "feed.csv"), "feed changed")
    documents = {}
    for name, (_, _, expected_exit) in CASES.items():
        documents[name] = read_case(out, name, expected_exit)
    refusal = documents["finite_refusal"]
    require(set(refusal) == {"engine", "error", "code", "args"},
            "refusal published a success/partial trade report")
    require(refusal["code"] == "pine_map_capacity_exceeded", "wrong finite refusal")
    require(refusal["args"] == {
        "code": "RE10070", "maxSize": 50000, "currSize": 50001, "bar_index": 1,
    }, "wrong actual boundary/current/bar context")
    require(refusal["error"].endswith(
        "Error on bar 1: A map cannot have more than 50000 key-value pairs. "
        "The current size is 50001."), "wrong canonical capacity message")
    negative = documents["negative_option"]
    require(negative["code"] == "run_request_invalid" and
            negative["args"] == {"option": "arguments"},
            "negative option did not fail at the CLI parser")
    quantities = {}
    for name in ("finite_boundary", "omitted_a", "zero_a", "omitted_b", "zero_b"):
        pairs, salt, _ = CASES[name]
        quantities[name] = check_success(documents[name], pairs, salt)
    require(quantities["omitted_a"] == quantities["zero_a"] and
            quantities["omitted_b"] == quantities["zero_b"], "zero differs from omission")
    require(quantities["omitted_a"] != quantities["omitted_b"],
            "meaningful inputs produced identical map observations")
    raw = {path.name: digest(path) for path in sorted(out.iterdir())
           if path.suffix in (".stdout", ".stderr", ".exit")}
    require(len(raw) == 3 * len(CASES), "unexpected or missing runtime raw artifacts")
    write_json(out / "outcome.json", {
        "checked_cases": len(documents), "success_cases": 5, "control_cases": 2,
        "raw_artifact_count": len(raw), "raw_sha256": raw,
        "quantities": quantities,
        "finite_refusal": "CONTROL: real 50001st pair; no success report",
        "historical_baseline_red": "NOT ESTABLISHED",
        "default_policy": "omitted and zero both exceeded 50000 on exact staged pair",
    })
    print(f"CR559 CLI: {len(documents)} cases checked; finite refusal is CONTROL")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "check"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "prepare":
        prepare(args.out)
    else:
        check(args.out)


if __name__ == "__main__":
    main()
