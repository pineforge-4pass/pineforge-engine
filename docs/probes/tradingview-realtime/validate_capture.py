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
import re


EVENT_SCHEMA = "pf-tv-probe-event-v2"
COMMAND_SCHEMA = "pf-tv-probe-command-v2"
RUN_ID_RE = re.compile(r"^[A-Za-z0-9._-]+$")


def reject_json_constant(value: str) -> None:
    raise ValueError(f"non-standard JSON number {value}")


def strict_json_loads(raw: str) -> object:
    return json.loads(raw, parse_constant=reject_json_constant)


def is_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


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
    try:
        manifest = strict_json_loads(manifest_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, ValueError) as exc:
        fail(f"invalid manifest JSON: {exc}")
    if not isinstance(manifest, dict):
        fail("manifest root must be an object")
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
    mode = manifest.get("mode")
    if not isinstance(probe, str) or not probe.startswith("P"):
        errors.append("manifest probe must be a P-number string")
    if not isinstance(run_id, str) or not run_id:
        errors.append("manifest run_id must be non-empty")
    elif not RUN_ID_RE.fullmatch(run_id):
        errors.append("manifest run_id must match [A-Za-z0-9._-]+")
    if mode is None or mode == "" or mode == "replace-me":
        errors.append("manifest mode is not populated")
    if manifest.get("alert", {}).get("alert_id") in {None, "", "replace-me"}:
        errors.append("manifest alert.alert_id is not populated")
    if manifest.get("alert", {}).get("event_schema") != EVENT_SCHEMA:
        errors.append(f"manifest alert.event_schema must be {EVENT_SCHEMA}")
    if manifest.get("alert", {}).get("command_schema") != COMMAND_SCHEMA:
        errors.append(f"manifest alert.command_schema must be {COMMAND_SCHEMA}")

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
            event = strict_json_loads(raw)
        except (json.JSONDecodeError, ValueError) as exc:
            errors.append(f"webhook line {line_number}: invalid JSON: {exc}")
            continue
        if not isinstance(event, dict):
            errors.append(f"webhook line {line_number}: root must be an object")
            continue
        schema = event.get("schema")
        if schema != EVENT_SCHEMA:
            errors.append(f"webhook line {line_number}: unknown schema {schema!r}")
            continue
        if event.get("source") != "tradingview":
            errors.append(f"webhook line {line_number}: source must be tradingview")
        if not isinstance(event.get("event_key_hint"), str) or not event.get(
                "event_key_hint"):
            errors.append(f"webhook line {line_number}: event_key_hint missing")
        event_type = event.get("event_type")
        if event_type == "trace":
            identity = event.get("probe")
            if not isinstance(identity, dict):
                errors.append(f"webhook line {line_number}: trace probe is not an object")
                continue
            event_probe = identity.get("id")
            event_run_id = identity.get("run_id")
            event_mode = identity.get("mode")
            if not isinstance(event_mode, str):
                errors.append(f"webhook line {line_number}: trace mode missing")
            trace_event = event.get("event")
            if not isinstance(trace_event, dict) or not isinstance(
                    trace_event.get("name"), str):
                errors.append(f"webhook line {line_number}: trace event.name missing")
            market = event.get("market")
            if not isinstance(market, dict):
                errors.append(f"webhook line {line_number}: trace market missing")
            else:
                for field in ("tickerid", "interval"):
                    if not isinstance(market.get(field), str) or not market.get(field):
                        errors.append(
                            f"webhook line {line_number}: market.{field} missing")
                for field in ("bar_index", "bar_time_ms", "bar_close_ms",
                              "server_time_ms"):
                    if not isinstance(market.get(field), int) or isinstance(
                            market.get(field), bool):
                        errors.append(
                            f"webhook line {line_number}: market.{field} must be int")
                if not is_number(market.get("price")):
                    errors.append(
                        f"webhook line {line_number}: market.price must be numeric")
            strategy = event.get("strategy")
            if not isinstance(strategy, dict) or not is_number(
                    strategy.get("position_size")):
                errors.append(
                    f"webhook line {line_number}: strategy.position_size missing")
        elif event_type == "order_fill":
            payload = event.get("command_context")
            if not isinstance(payload, dict):
                errors.append(
                    f"webhook line {line_number}: command_context is not an object")
                continue
            if payload.get("schema") != COMMAND_SCHEMA:
                errors.append(
                    f"webhook line {line_number}: command_context schema invalid")
            identity = payload.get("probe")
            if not isinstance(identity, dict):
                errors.append(
                    f"webhook line {line_number}: command_context.probe is not an object")
                continue
            event_probe = identity.get("id")
            event_run_id = identity.get("run_id")
            event_mode = identity.get("mode")
            if not isinstance(event_mode, str):
                errors.append(f"webhook line {line_number}: command mode missing")
            command = payload.get("command")
            required_string_fields = (
                "tag", "api", "action", "order_id", "side", "order_type",
                "debug_intent")
            if not isinstance(command, dict):
                errors.append(f"webhook line {line_number}: command missing")
            else:
                for field in required_string_fields:
                    if not isinstance(command.get(field), str) or not command.get(field):
                        errors.append(
                            f"webhook line {line_number}: command.{field} must be nonempty string")
                for field in ("from_entry", "oca_name", "oca_type"):
                    value = command.get(field)
                    if field not in command or (
                            value is not None and not isinstance(value, str)):
                        errors.append(
                            f"webhook line {line_number}: command.{field} invalid")
                if not isinstance(command.get("source_order"), int) or isinstance(
                        command.get("source_order"), bool) or command.get(
                        "source_order") < 1:
                    errors.append(
                        f"webhook line {line_number}: command.source_order must be positive int")
                numeric_fields = ("qty", "stop_price", "limit_price",
                                  "profit_ticks", "loss_ticks", "trail_points",
                                  "trail_offset")
                for field in numeric_fields:
                    value = command.get(field)
                    if field not in command or (
                            value is not None and not is_number(value)):
                        errors.append(
                            f"webhook line {line_number}: command.{field} invalid")
            evaluation = payload.get("message_evaluation")
            if not isinstance(evaluation, dict):
                errors.append(
                    f"webhook line {line_number}: message_evaluation missing")
            else:
                for field in ("bar_index", "bar_time_ms", "server_time_ms"):
                    if not isinstance(evaluation.get(field), int) or isinstance(
                            evaluation.get(field), bool):
                        errors.append(
                            f"webhook line {line_number}: message_evaluation.{field} must be int")
                if not is_number(evaluation.get("position_size")):
                    errors.append(
                        f"webhook line {line_number}: message_evaluation.position_size invalid")
            market = event.get("market")
            if not isinstance(market, dict):
                errors.append(f"webhook line {line_number}: fill market missing")
            else:
                for field in ("ticker", "exchange", "interval",
                              "fill_bar_time", "server_time"):
                    if not isinstance(market.get(field), str) or not market.get(field):
                        errors.append(
                            f"webhook line {line_number}: market.{field} missing")
            fill = event.get("fill")
            if not isinstance(fill, dict):
                errors.append(f"webhook line {line_number}: fill object missing")
            else:
                for field in ("order_id", "action", "market_position",
                              "previous_market_position"):
                    if not isinstance(fill.get(field), str) or not fill.get(field):
                        errors.append(
                            f"webhook line {line_number}: fill.{field} missing")
                for field in ("contracts", "price", "position_size",
                              "market_position_size",
                              "previous_market_position_size"):
                    if not is_number(fill.get(field)):
                        errors.append(
                            f"webhook line {line_number}: fill.{field} must be numeric")
        else:
            errors.append(
                f"webhook line {line_number}: unknown event_type {event_type!r}")
            continue
        if event_probe != probe:
            errors.append(
                f"webhook line {line_number}: probe {event_probe!r} != {probe!r}")
        if event_run_id != run_id:
            errors.append(
                f"webhook line {line_number}: run_id {event_run_id!r} != {run_id!r}")
        if event_mode != mode:
            errors.append(
                f"webhook line {line_number}: mode {event_mode!r} != {mode!r}")
        events.append(event)

    armed_events = [event for event in events
                    if event.get("event_type") == "trace"
                    and event.get("event", {}).get("name") == "armed"]
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
            record = strict_json_loads(raw)
        except (json.JSONDecodeError, ValueError) as exc:
            errors.append(f"receipt line {line_number}: invalid JSON: {exc}")
            continue
        if not isinstance(record, dict):
            errors.append(f"receipt line {line_number}: root must be an object")
            continue
        receipts.append(record)
    if len(receipts) != len(raw_lines):
        errors.append(
            f"receipt count {len(receipts)} != raw webhook body count {len(raw_lines)}")
    sequences = [record.get("sequence") for record in receipts]
    if not sequences:
        errors.append("receipt.jsonl is empty")
    elif not all(isinstance(value, int) and not isinstance(value, bool)
                 for value in sequences):
        errors.append("every receipt sequence must be an integer")
    elif sequences != list(range(sequences[0], sequences[0] + len(sequences))):
        errors.append("receipt sequences must be contiguous and arrival-ordered")
    for index, (record, raw) in enumerate(zip(receipts, raw_lines), 1):
        if not isinstance(record.get("receipt_id"), int) or isinstance(
                record.get("receipt_id"), bool):
            errors.append(f"receipt {index}: receipt_id must be an integer")
        if record.get("body_sha256") != line_sha256(raw):
            errors.append(f"receipt {index}: body_sha256 mismatch")
        parse_utc(record.get("received_at_utc"),
                  f"receipt {index}.received_at_utc", errors)
    receipt_ids = [record.get("receipt_id") for record in receipts]
    if receipt_ids and all(isinstance(value, int) and not isinstance(value, bool)
                           for value in receipt_ids):
        if any(right <= left for left, right in zip(receipt_ids, receipt_ids[1:])):
            errors.append("receipt_id values must increase in arrival order")
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
            event.get("event_type") == "trace" for event in events),
        "fill_events": sum(
            event.get("event_type") == "order_fill" for event in events),
        "artifact_hashes": actual_hashes,
        "semantic_review_required": True,
        "errors": errors,
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
