import ast
import inspect
import unittest
import json
from pathlib import Path
import tempfile
import textwrap

from native_live_equivalence_e2e import (
    Strategy, first_difference, live_action, ordered_delivery_effects,
    require_replay_after_restart, synthetic_prints,
)
from native_live_tick_tape import (
    TIMESTAMP_CONTRACT, bar_differences, file_digest, load_tick_tape,
    message_groups, message_hashes, parse_cost, run_genuine_tape,
)
from native_live_tick_oracle import classify_first_divergence, first_fill, modeled_points, script_bars
from native_live_genuine_retry_e2e import check_invocation_order


class HarnessContract(unittest.TestCase):
    def test_timestamp_contract_matches_chart_batch_and_separate_reconstruction(self):
        calls = [node for node in ast.walk(ast.parse(inspect.getsource(run_genuine_tape)))
                 if isinstance(node, ast.Call)]
        batch_calls = [node for node in calls if isinstance(node.func, ast.Attribute)
                       and node.func.attr == "batch"]
        self.assertEqual(len(batch_calls), 1)
        batch = batch_calls[0]
        keywords = {entry.arg: ast.literal_eval(entry.value) for entry in batch.keywords}
        native_calls = [node for node in ast.walk(ast.parse(textwrap.dedent(inspect.getsource(Strategy.batch))))
                        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                        and node.func.attr == "run_backtest_full"]
        self.assertEqual(len(native_calls), 1)
        self.assertEqual(TIMESTAMP_CONTRACT["chart_input_batch"], {
            "input_tf": keywords["input_tf"], "script_tf": ast.literal_eval(batch.args[1]),
            "magnifier": ast.literal_eval(native_calls[0].args[5]),
            "tick_samples": ast.literal_eval(native_calls[0].args[6]),
            "distribution": keywords["distribution"], "distribution_name": "ENDPOINTS"})
        self.assertEqual(TIMESTAMP_CONTRACT["chart_input_batch"]["distribution"], 3)
        reconstruction = [node for node in calls if isinstance(node.func, ast.Name)
                          and node.func.id == "direct_tape" and any(
                              entry.arg == "observe_bars" and ast.literal_eval(entry.value)
                              for entry in node.keywords)]
        self.assertEqual(len(reconstruction), 1)
        timeframe = next(ast.literal_eval(entry.value) for entry in reconstruction[0].keywords
                         if entry.arg == "timeframe")
        self.assertEqual(timeframe, 1)
        self.assertEqual(TIMESTAMP_CONTRACT["one_minute_reconstruction"]["input_tf"], timeframe)
        self.assertEqual(TIMESTAMP_CONTRACT["one_minute_reconstruction"]["script_tf"], timeframe)

    def delivery_fixture(self, order=(1, 2, 3)):
        effects = [{"event_id": str(sequence), "delivery_id": f"key-{sequence}",
                    "sequence": sequence} for sequence in order]
        attempts = [{"event_id": str(sequence), "event_header": str(sequence),
                     "idempotency_header": f"key-{sequence}", "status": 503,
                     "body": json.dumps({"event_id": str(sequence),
                         "delivery_id": f"key-{sequence}", "sequence": sequence})}
                    for sequence in (1, 2, 3)]
        attempts += [{**attempt, "status": 200} for attempt in attempts]
        return attempts, effects

    def redelivery_fixture(self, event_ids, effects_before=0):
        return {"event_ids": event_ids, "attempts_before": 3, "effects_before": effects_before,
                "invocations": [{"attempts_before": 3, "effects_before": effects_before}]}

    def genuine_invocation_fixture(self):
        attempts, effects = self.delivery_fixture((2, 1, 3))
        attempts[1]["status"] = 200
        attempts = attempts[:4] + attempts[5:]
        invocations = [
            {"command": ["runner", "run"], "attempts_before": 0, "effects_before": 0},
            {"command": ["runner", "run"], "attempts_before": 1, "effects_before": 0},
            {"command": ["runner", "redeliver"], "attempts_before": 3, "effects_before": 1},
        ]
        return attempts, effects, invocations

    def test_genuine_invocations_allow_logical_order_across_explicit_redelivery(self):
        attempts, effects, invocations = self.genuine_invocation_fixture()
        redelivery = self.redelivery_fixture(["1", "3"], effects_before=1)
        self.assertEqual([effect["sequence"] for effect in ordered_delivery_effects(
            attempts, effects, redelivery)], [1, 2, 3])
        check_invocation_order(attempts, effects, invocations)

    def test_genuine_invocations_refuse_missing_overlapping_and_misattributed_boundaries(self):
        for index, key, value, diagnostic in (
                (0, "attempts_before", 1, "initial invocation"),
                (1, "attempts_before", 4, "invocation boundary"),
                (1, "effects_before", 1, "no successful attempt")):
            with self.subTest(index=index, key=key, value=value):
                attempts, effects, invocations = self.genuine_invocation_fixture()
                invocations[index][key] = value
                with self.assertRaisesRegex(AssertionError, diagnostic):
                    check_invocation_order(attempts, effects, invocations)

    def test_genuine_invocations_refuse_redelivery_attempt_and_effect_reordering(self):
        for reorder_effects in (False, True):
            with self.subTest(reorder_effects=reorder_effects):
                attempts, effects, invocations = self.genuine_invocation_fixture()
                if reorder_effects:
                    effects[1], effects[2] = effects[2], effects[1]
                else:
                    attempts[3], attempts[4] = attempts[4], attempts[3]
                with self.assertRaisesRegex(AssertionError, "reordered"):
                    check_invocation_order(attempts, effects, invocations)

    def test_genuine_invocations_keep_first_pass_order_strict_across_restart(self):
        attempts, effects = self.delivery_fixture((2, 1, 3))
        attempts = [attempts[4], attempts[3], attempts[5]]
        invocations = [
            {"command": ["runner", "run"], "attempts_before": 0, "effects_before": 0},
            {"command": ["runner", "run"], "attempts_before": 1, "effects_before": 1},
        ]
        with self.assertRaisesRegex(AssertionError, "first-pass effects"):
            check_invocation_order(attempts, effects, invocations)

    def test_explicit_redelivery_allows_an_earlier_failed_effect_after_later_effects(self):
        attempts, effects = self.delivery_fixture((2, 1, 3))
        redelivery = self.redelivery_fixture(["1", "3"], effects_before=1)
        self.assertEqual([effect["sequence"] for effect in ordered_delivery_effects(
            attempts, effects, redelivery)], [1, 2, 3])
        with self.assertRaisesRegex(RuntimeError, "first-pass effects"):
            ordered_delivery_effects(attempts, effects)

    def test_redelivery_does_not_allow_initial_effects_or_first_attempts_to_reorder(self):
        attempts, effects = self.delivery_fixture((2, 1, 3))
        redelivery = self.redelivery_fixture(["1", "2", "3"], effects_before=2)
        with self.assertRaisesRegex(RuntimeError, "first-pass effects"):
            ordered_delivery_effects(attempts, effects, redelivery)
        attempts[0], attempts[1] = attempts[1], attempts[0]
        with self.assertRaisesRegex(RuntimeError, "first-attempt"):
            ordered_delivery_effects(attempts, effects, redelivery)

    def test_redelivery_does_not_allow_unselected_effects_to_reorder(self):
        attempts, effects = self.delivery_fixture((2, 1, 3))
        with self.assertRaisesRegex(RuntimeError, "non-redelivered"):
            ordered_delivery_effects(attempts, effects,
                self.redelivery_fixture(["3"]))

    def test_redelivery_still_requires_exact_bodies_keys_and_complete_unique_effects(self):
        attempts, effects = self.delivery_fixture()
        for field, replacement in (("body", "{}"), ("idempotency_header", "wrong")):
            altered = [dict(attempt) for attempt in attempts]
            altered[-1][field] = replacement
            with self.assertRaises((RuntimeError, KeyError)):
                ordered_delivery_effects(altered, effects)
        for altered in (effects[:-1], effects + effects[:1]):
            with self.assertRaises(RuntimeError):
                ordered_delivery_effects(attempts, altered)

    def test_redelivery_selection_keeps_attempt_and_effect_order(self):
        attempts, effects = self.delivery_fixture((2, 3, 1))
        redelivery = self.redelivery_fixture(["1", "3"], effects_before=1)
        with self.assertRaisesRegex(RuntimeError, "redelivery effects"):
            ordered_delivery_effects(attempts, effects, redelivery)
        attempts[3], attempts[5] = attempts[5], attempts[3]
        with self.assertRaisesRegex(RuntimeError, "redelivery attempts"):
            ordered_delivery_effects(attempts, effects, redelivery)

    def test_redelivery_restart_reselects_acknowledged_actions_in_order(self):
        attempts, effects = self.delivery_fixture()
        attempts += [dict(attempt) for attempt in attempts[3:]]
        redelivery = self.redelivery_fixture(["1", "2", "3"])
        redelivery["invocations"].append({"attempts_before": 6, "effects_before": 3})
        self.assertEqual(ordered_delivery_effects(attempts, effects, redelivery), effects)
        for start in (3, 6):
            altered = list(attempts)
            altered[start + 1], altered[start + 2] = altered[start + 2], altered[start + 1]
            with self.subTest(invocation_start=start):
                with self.assertRaisesRegex(RuntimeError, "redelivery attempts"):
                    ordered_delivery_effects(altered, effects, redelivery)

    def test_redelivery_restart_preserves_exact_bytes_keys_and_unique_effects(self):
        attempts, effects = self.delivery_fixture()
        attempts += [dict(attempt) for attempt in attempts[3:]]
        redelivery = self.redelivery_fixture(["1", "2", "3"])
        redelivery["invocations"].append({"attempts_before": 6, "effects_before": 3})
        for field, replacement in (("body", attempts[-1]["body"] + " "),
                                   ("idempotency_header", "wrong"), ("event_header", "wrong")):
            altered = [dict(attempt) for attempt in attempts]
            altered[-1][field] = replacement
            with self.subTest(field=field):
                with self.assertRaises(RuntimeError):
                    ordered_delivery_effects(altered, effects, redelivery)
        for altered in (effects[:-1], effects + effects[:1]):
            with self.assertRaises(RuntimeError):
                ordered_delivery_effects(attempts, altered, redelivery)

    def test_redelivery_invocation_boundaries_cannot_omit_or_overlap_attempts(self):
        attempts, effects = self.delivery_fixture()
        for boundaries in ([], [{"attempts_before": 4, "effects_before": 0}],
                           [{"attempts_before": 3, "effects_before": 0},
                            {"attempts_before": 2, "effects_before": 3}]):
            redelivery = self.redelivery_fixture(["1", "2", "3"])
            redelivery["invocations"] = boundaries
            with self.assertRaisesRegex(RuntimeError, "invocation boundary"):
                ordered_delivery_effects(attempts, effects, redelivery)

    def test_acknowledgement_loss_requires_success_after_restart_boundary(self):
        attempts, _ = self.delivery_fixture()
        restart = {"attempts_before": 6, "effects_before": 3}
        # The original 503 and the accepted pre-crash request are not replay.
        with self.assertRaisesRegex(RuntimeError, "not replayed after restart"):
            require_replay_after_restart(attempts, "3", restart)
        with self.assertRaisesRegex(RuntimeError, "not replayed after restart"):
            require_replay_after_restart(attempts + attempts[3:5], "3", restart)
        require_replay_after_restart(attempts + attempts[3:], "3", restart)

    def test_live_filter_uses_the_sealed_script_bucket(self):
        self.assertFalse(live_action(1439, 15, 1447))
        self.assertFalse(live_action(1441, 15, 1455))
        self.assertTrue(live_action(1440, 15, 1447))
        self.assertTrue(live_action(1454, 15, 1447))
        self.assertFalse(live_action(1446, 1, 1447))
        self.assertTrue(live_action(1447, 1, 1447))

    def test_synthetic_prints_preserve_ohlcv_and_sequence(self):
        rows = [{"timestamp": "120000", "open": "10", "high": "12", "low": "9",
                 "close": "11", "volume": "0.1"},
                {"timestamp": "180000", "open": "11", "high": "13", "low": "10",
                 "close": "12", "volume": "0.125"}]
        packets = synthetic_prints(rows, 0)
        ticks = [packet for packet in packets if packet["type"] == "tick"]
        self.assertEqual([packet["seq"] for packet in ticks], list(range(1, 9)))
        for index, row in enumerate(rows):
            group = ticks[index * 4:index * 4 + 4]
            self.assertTrue(all(packet["qty"] > 0 for packet in group))
            actual = [group[0]["price"], max(packet["price"] for packet in group),
                      min(packet["price"] for packet in group), group[-1]["price"],
                      sum(packet["qty"] for packet in group)]
            expected = [float(row[field]) for field in ("open", "high", "low", "close", "volume")]
            self.assertIsNone(first_difference(expected, actual))
            self.assertEqual(packets[index * 5 + 4], {"type": "time", "ts": int(row["timestamp"]) + 60000})

    def test_zero_volume_cannot_be_a_positive_print_tape(self):
        with self.assertRaisesRegex(ValueError, "positive corpus volume"):
            synthetic_prints([{"timestamp": "120000", "volume": "0"}], 0)

    def test_binary64_comparison_never_adds_tolerance(self):
        self.assertIsNotNone(first_difference([0.0], [-0.0]))
        self.assertIsNotNone(first_difference([1.0], [1.0000000000000002]))
        self.assertIsNone(first_difference([1.0], [1.0]))

    def test_genuine_tape_checks_digest_ids_and_exact_decimal_volume(self):
        with tempfile.TemporaryDirectory() as directory:
            tape = Path(directory) / "ticks.csv"
            tape.write_text("agg_trade_id,price,quantity,transact_time\n7,10.01,0.1,1\n8,10.02,0.2,1\n")
            manifest = {"source": "local test fixture", "sha256": file_digest(tape),
                "first_id": 7, "last_id": 8, "count": 2, "start_ms": 0, "end_ms": 60000}
            manifest_path = Path(str(tape) + ".manifest.json")
            manifest_path.write_text(json.dumps(manifest))
            _, packets, bars = load_tick_tape(tape)
            self.assertEqual(bars[0]["volume"], "0.3")
            self.assertEqual([event["seq"] for event in packets[:-1]], [7, 8])
            self.assertEqual(packets[-1], {"type": "time", "ts": 60000})
            tape.write_text(tape.read_text().replace("8,10.02", "9,10.02"))
            with self.assertRaisesRegex(ValueError, "SHA256"):
                load_tick_tape(tape)
            manifest["sha256"] = file_digest(tape)
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "id hole"):
                load_tick_tape(tape)

    def test_message_boundaries_do_not_claim_per_tick_hashes_for_batches(self):
        events = [{"type": "time", "ts": index} for index in range(2050)]
        groups = list(message_groups(events, 1024))
        self.assertEqual([len(group["events"]) for group in groups], [1024, 1024, 2])
        self.assertEqual(message_hashes([{"hash": str(index)} for index in range(2050)], 2050, 1024),
                         ["1023", "2047", "2049"])
        self.assertEqual(message_hashes([{"event_index": index, "hash": str(index)}
            for index in (599, 1023, 1799, 2047, 2049)], 2050, 1024), ["1023", "2047", "2049"])
        with self.assertRaisesRegex(ValueError, "1..1024"):
            list(message_groups(events, 1025))

    def test_bar_volume_comparator_does_not_hide_binary64_drift(self):
        expected = [{"timestamp": "0", "open": "10", "high": "10", "low": "10", "close": "10", "volume": "0.3"}]
        actual = {0: {"open": 10, "high": 10, "low": 10, "close": 10, "volume": 0.1 + 0.2}}
        self.assertEqual(bar_differences(expected, actual),
            [{"minute": 0, "field": "volume", "expected": "0.3", "actual": "0.30000000000000004"}])

    def test_cost_receipt_preserves_cpu_and_peak_rss(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / "time.txt"
            receipt.write_text("User time (seconds): 1.25\nSystem time (seconds): 0.50\nMaximum resident set size (kbytes): 4096\n")
            self.assertEqual(parse_cost(receipt)["cpu_seconds"], 1.75)
            self.assertEqual(parse_cost(receipt)["max_rss_kib"], 4096)

    def test_independent_stop_oracle_gaps_at_print_and_applies_slippage(self):
        request = {"type": "stop", "levels": {"stop": 10.1}, "not_before": 100}
        points = [{"ts": 99, "seq": 7, "price": 11}, {"ts": 101, "seq": 8, "price": 10.09},
            {"ts": 101, "seq": 9, "price": 10.13}]
        fill = first_fill(request, points, True, slippage=2)
        self.assertEqual((fill["timestamp"], fill["sequence"], fill["raw_price"]), (101, 9, 10.13))
        self.assertIsNone(first_difference(fill["resolved_price"], 10.15))

    def test_independent_limit_oracle_distinguishes_open_gap_from_print(self):
        request = {"type": "limit", "levels": {"limit": 10.005}, "not_before": 0}
        tick = first_fill(request, [{"ts": 1, "seq": 7, "price": 10.2}], False, slippage=5)
        bar = script_bars([{"timestamp": 0, "open": "10.2", "high": "10.4", "low": "10", "close": "10.3"}])
        batch = first_fill(request, modeled_points(bar, 0), False, modeled=True, slippage=5)
        self.assertIsNone(first_difference(tick["resolved_price"], 10.01))
        self.assertIsNone(first_difference(batch["resolved_price"], 10.200000000000001))

    def test_independent_stop_limit_oracle_is_unslipped(self):
        request = {"type": "stop_limit", "levels": {"stop": 10.015, "limit": 10.05}, "not_before": 0}
        tick = first_fill(request, [{"ts": 1, "seq": 7, "price": 10},
            {"ts": 2, "seq": 8, "price": 10.03}], True, slippage=3)
        bar = script_bars([{"timestamp": 0, "open": "10", "high": "10.1", "low": "9", "close": "10"}])
        batch = first_fill(request, modeled_points(bar, 0), True, modeled=True, slippage=3)
        self.assertEqual(tick["sequence"], 8)
        self.assertIsNone(first_difference(tick["resolved_price"], 10.03))
        self.assertIsNone(first_difference(batch["resolved_price"], 10.01))

    def test_independent_trail_oracle_tracks_running_best_and_first_print(self):
        request = {"type": "trail", "levels": {"arm": 10.595, "best_seed": 10.6, "offset": 0.4}, "not_before": 0}
        points = [{"ts": index, "seq": index + 7, "price": price}
            for index, price in enumerate((10.5, 10.6, 10.7, 10.31, 10.29))]
        fill = first_fill(request, points, False)
        self.assertEqual((fill["sequence"], fill["running_best"], fill["activation"]["sequence"]), (11, "10.7", 8))
        self.assertIsNone(first_difference(fill["resolved_price"], 10.290000000000001))
        with self.assertRaisesRegex(ValueError, "positive-offset"):
            first_fill({"type": "trail", "levels": {"offset": 0}, "not_before": 0}, points, False)

    def test_genuine_tape_rejects_nonpositive_quantity_before_window(self):
        with tempfile.TemporaryDirectory() as directory:
            tape = Path(directory) / "ticks.csv"
            tape.write_text("agg_trade_id,price,quantity,transact_time\n7,10,0,-1\n8,10,1,1\n")
            manifest = {"source": "local fixture", "sha256": file_digest(tape), "first_id": 8,
                "last_id": 8, "count": 1, "start_ms": 0, "end_ms": 60000}
            Path(str(tape) + ".manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "nonpositive"):
                load_tick_tape(tape)

    def test_report_only_difference_is_not_explained_as_a_priced_fill(self):
        classification = classify_first_divergence([], [], [], [], [], [],
            {"path": ".totals.net_profit", "expected": 1, "actual": 2})
        self.assertEqual(classification["status"], "UNEXPLAINED")
        self.assertIn("report-only", classification["proof_gap"])


if __name__ == "__main__":
    unittest.main()
