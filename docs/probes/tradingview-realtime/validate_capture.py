#!/usr/bin/env python3
"""Validate completeness and JSON structure of a TradingView probe capture.

Semantic agreement between the TradingView alert log, trade export, and the
pre-registered decision rule still requires manual review.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path


TRACE_SCHEMA = "pf-tv-probe-trace-v1"
FILL_SCHEMA = "pf-tv-probe-fill-v1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def line_sha256(raw: str) -> str:
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def fail(message: str) -> None:
    raise ValueError(message)


def parse_utc(value: object, label: str, errors: list[str]) -> datetime | None:
    if not isinstance(value, str) or value.startswith("YYYY-"):
        errors.append(f"{label} is not populated")
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        errors.append(f"{label} is not ISO-8601: {value!r}")
        return None
    if parsed.tzinfo is None:
        errors.append(f"{label} must include a timezone")
        return None
    return parsed


def verify_hash(path: Path, expected: object, label: str,
                errors: list[str]) -> str:
    actual = sha256(path)
    if not isinstance(expected, str) or expected in {"", "replace-me"}:
        errors.append(f"{label} hash is not populated")
    elif expected != actual:
        errors.append(f"{label} hash {expected} != actual {actual}")
    return actual


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()

    run_dir = args.run_dir.resolve()
    manifest_path = run_dir / "manifest.json"
    if not manifest_path.is_file():
        fail(f"missing {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "pf-tv-probe-manifest-v1":
        fail("manifest schema must be pf-tv-probe-manifest-v1")

    required_files = {
        "webhook": run_dir / "webhook.jsonl",
        "receipt": run_dir / "receipt.jsonl",
        "alert_message": run_dir / "alert-message.txt",
        "tv_alert_log": run_dir / "tv-alert-log.csv",
        "tv_trades": run_dir / "tv-trades.csv",
        "tv_bars": run_dir / "tv-bars.csv",
        "chart": run_dir / "chart.png",
        "source": run_dir / "deployed-source.pine",
        "notes": run_dir / "notes.md",
        "results": run_dir / "results.md",
    }
    missing = [str(path) for path in required_files.values() if not path.is_file()]
    if missing:
        fail("missing required artifacts: " + ", ".join(missing))

    errors: list[str] = []
    probe = manifest.get("probe")
    run_id = manifest.get("run_id")
    if not isinstance(probe, str) or not probe.startswith("P"):
        errors.append("manifest probe must be a P-number string")
    if not isinstance(run_id, str) or not run_id:
        errors.append("manifest run_id must be non-empty")
    if manifest.get("mode") in {None, "", "replace-me"}:
        errors.append("manifest mode is not populated")
    if manifest.get("alert", {}).get("alert_id") in {None, "", "replace-me"}:
        errors.append("manifest alert.alert_id is not populated")

    scheduled = parse_utc(
        manifest.get("scheduled_start_utc"), "scheduled_start_utc", errors)
    created = parse_utc(
        manifest.get("alert", {}).get("created_at_utc"),
        "alert.created_at_utc", errors)
    armed = parse_utc(
        manifest.get("observation", {}).get("armed_at_utc"),
        "observation.armed_at_utc", errors)
    if scheduled and created and created >= scheduled:
        errors.append("alert must be created before scheduled start")
    if scheduled and armed and armed < scheduled:
        errors.append("armed time precedes scheduled start")

    webhook_path = required_files["webhook"]
    raw_lines = [raw for raw in webhook_path.read_text(
        encoding="utf-8").splitlines() if raw.strip()]
    events: list[dict] = []
    for line_number, raw in enumerate(raw_lines, 1):
        try:
            event = json.loads(raw)
        except json.JSONDecodeError as exc:
            errors.append(f"webhook line {line_number}: invalid JSON: {exc}")
            continue
        schema = event.get("schema")
        if schema == TRACE_SCHEMA:
            event_probe = event.get("probe")
            event_run_id = event.get("run_id")
            if not isinstance(event.get("tag"), str):
                errors.append(f"webhook line {line_number}: trace tag missing")
        elif schema == FILL_SCHEMA:
            payload = event.get("probe_payload")
            if not isinstance(payload, dict):
                errors.append(
                    f"webhook line {line_number}: fill probe_payload is not an object")
                continue
            event_probe = payload.get("probe")
            event_run_id = payload.get("run_id")
            if not isinstance(payload.get("order_tag"), str):
                errors.append(f"webhook line {line_number}: fill order_tag missing")
            if not isinstance(event.get("fill_bar_time"), str):
                errors.append(f"webhook line {line_number}: fill_bar_time missing")
            if not isinstance(event.get("server_time"), str):
                errors.append(f"webhook line {line_number}: server_time missing")
        else:
            errors.append(f"webhook line {line_number}: unknown schema {schema!r}")
            continue
        if event_probe != probe:
            errors.append(
                f"webhook line {line_number}: probe {event_probe!r} != {probe!r}")
        if event_run_id != run_id:
            errors.append(
                f"webhook line {line_number}: run_id {event_run_id!r} != {run_id!r}")
        events.append(event)

    armed_events = [event for event in events
                    if event.get("schema") == TRACE_SCHEMA
                    and event.get("tag") == "armed"]
    if len(armed_events) != 1:
        errors.append(f"expected exactly one armed trace, got {len(armed_events)}")

    observation = manifest.get("observation", {})
    declared_count = observation.get("webhook_event_count")
    if not isinstance(declared_count, int) or declared_count != len(events):
        errors.append(
            f"manifest webhook_event_count {declared_count!r} != parsed {len(events)}")
    tv_alert_count = observation.get("tradingview_alert_event_count")
    if not isinstance(tv_alert_count, int) or tv_alert_count <= 0:
        errors.append("tradingview_alert_event_count must be positive")
    if isinstance(tv_alert_count, int) and tv_alert_count < len(events):
        errors.append("TradingView alert count cannot be smaller than webhook count")
    if observation.get("chart_reloaded_or_changed") is not False:
        errors.append("chart/script changed or reloaded during capture")

    receipts: list[dict] = []
    for line_number, raw in enumerate(required_files["receipt"].read_text(
            encoding="utf-8").splitlines(), 1):
        if not raw.strip():
            continue
        try:
            record = json.loads(raw)
        except json.JSONDecodeError as exc:
            errors.append(f"receipt line {line_number}: invalid JSON: {exc}")
            continue
        receipts.append(record)
    if len(receipts) != len(raw_lines):
        errors.append(
            f"receipt count {len(receipts)} != raw webhook body count {len(raw_lines)}")
    sequences = [record.get("sequence") for record in receipts]
    if not sequences:
        errors.append("receipt.jsonl is empty")
    elif not all(isinstance(value, int) for value in sequences):
        errors.append("every receipt sequence must be an integer")
    elif sequences != list(range(sequences[0], sequences[0] + len(sequences))):
        errors.append("receipt sequences must be contiguous and arrival-ordered")
    for index, (record, raw) in enumerate(zip(receipts, raw_lines), 1):
        if record.get("body_sha256") != line_sha256(raw):
            errors.append(f"receipt {index}: body_sha256 mismatch")
        parse_utc(record.get("received_at_utc"),
                  f"receipt {index}.received_at_utc", errors)
    if sequences:
        if observation.get("receiver_first_sequence") != sequences[0]:
            errors.append("receiver_first_sequence does not match receipts")
        if observation.get("receiver_last_sequence") != sequences[-1]:
            errors.append("receiver_last_sequence does not match receipts")

    artifacts = manifest.get("artifacts", {})
    actual_hashes = {
        "webhook_jsonl_sha256": verify_hash(
            required_files["webhook"], artifacts.get("webhook_jsonl_sha256"),
            "webhook.jsonl", errors),
        "receipt_jsonl_sha256": verify_hash(
            required_files["receipt"], artifacts.get("receipt_jsonl_sha256"),
            "receipt.jsonl", errors),
        "tv_alert_log_sha256": verify_hash(
            required_files["tv_alert_log"], artifacts.get("tv_alert_log_sha256"),
            "tv-alert-log.csv", errors),
        "tv_trades_csv_sha256": verify_hash(
            required_files["tv_trades"], artifacts.get("tv_trades_csv_sha256"),
            "tv-trades.csv", errors),
        "tv_bars_csv_sha256": verify_hash(
            required_files["tv_bars"], artifacts.get("tv_bars_csv_sha256"),
            "tv-bars.csv", errors),
        "chart_png_sha256": verify_hash(
            required_files["chart"], artifacts.get("chart_png_sha256"),
            "chart.png", errors),
        "notes_md_sha256": verify_hash(
            required_files["notes"], artifacts.get("notes_md_sha256"),
            "notes.md", errors),
        "results_md_sha256": verify_hash(
            required_files["results"], artifacts.get("results_md_sha256"),
            "results.md", errors),
    }
    verify_hash(
        required_files["alert_message"],
        manifest.get("alert", {}).get("message_template_sha256"),
        "alert-message.txt", errors)
    source_hash = verify_hash(
        required_files["source"], manifest.get("deployed_source_sha256"),
        "deployed-source.pine", errors)
    if manifest.get("script_sha256") != source_hash:
        errors.append("script_sha256 does not match deployed-source.pine")

    results_text = required_files["results"].read_text(encoding="utf-8")
    if "## Pre-registered decision rule" not in results_text:
        errors.append("results.md lacks the pre-registered decision rule")

    summary = {
        "run_dir": str(run_dir),
        "probe": probe,
        "run_id": run_id,
        "events": len(events),
        "trace_events": sum(
            event.get("schema") == TRACE_SCHEMA for event in events),
        "fill_events": sum(
            event.get("schema") == FILL_SCHEMA for event in events),
        "artifact_hashes": actual_hashes,
        "semantic_review_required": True,
        "errors": errors,
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
