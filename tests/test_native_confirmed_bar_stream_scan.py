import unittest
import json
import io
import tarfile
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace

from native_confirmed_bar_stream_scan import (
    compare_saved, semantic_actions, semantic_state, stream_splits, SETTINGS,
    is_stream_refusal,
    recompare,
    read_saved_mode,
)


def action(timestamp, incarnation=1, price=123.0):
    return {"timestamp": timestamp, "bar_index": 0, "origin_input_index": 0,
            "order": {"action": "buy", "leg": "entry", "contracts": 2.0,
                      "price": price, "id": "long", "entry_incarnation": incarnation,
                      "reduce_only": False}}


def state():
    return {"closed_trades": [], "terminal_trades": [], "totals": {"net_profit": 0.0},
            "equity_curve": [], "metrics": {}}


class ConfirmedScanTests(unittest.TestCase):
    def test_recompare_reads_archived_modes_without_extracting_them(self):
        with TemporaryDirectory() as temporary:
            output = Path(temporary)
            case = output / "case"
            case.mkdir()
            archives = output / ".archives"
            archives.mkdir()
            with tarfile.open(archives / "case.tar.gz", "w:gz") as archive:
                for mode, price in (("batch", 0.0), ("stream-1", -0.0)):
                    for filename, content in (("state.json", json.dumps(state())),
                                              ("actions.jsonl", json.dumps(action(20, price=price)) + "\n")):
                        data = content.encode()
                        member = tarfile.TarInfo("case/" + mode + "/" + filename)
                        member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
            batch = read_saved_mode(case, "batch")
            stream = read_saved_mode(case, "stream-1")
            self.assertIsNotNone(compare_saved(batch, stream, 1, 20))
            self.assertFalse((case / "batch").exists())
            self.assertFalse((case / "stream-1").exists())

    def test_aligned_warmup_excludes_prior_actions(self):
        self.assertEqual(semantic_actions([action(10), action(20)], 20),
                         semantic_actions([action(20)], 20))

    def test_incarnation_is_not_the_pine_order_id(self):
        self.assertEqual(semantic_actions([action(20, 1)], 0),
                         semantic_actions([action(20, 3)], 0))

    def test_report_keeps_warmup_closed_trades(self):
        report = state()
        report["closed_trades"] = [{"exit_time": 10, "qty": 2.0, "entry_incarnation": 3}]
        self.assertEqual(semantic_state(report)["closed_trades"], [{"exit_time": 10, "qty": 2.0}])

    def test_price_difference_has_a_timestamp(self):
        batch = {"actions": [action(20)], "state": state()}
        stream = {"actions": [action(20, price=124.0)], "state": state()}
        difference = compare_saved(batch, stream, 1, 20)
        self.assertEqual(difference["path"], "actions[0].order.price")
        self.assertEqual(difference["time_ms"], 20)

    def test_signed_zero_is_not_rounded_away(self):
        batch = {"actions": [action(20, price=0.0)], "state": state()}
        stream = {"actions": [action(20, price=-0.0)], "state": state()}
        self.assertIsNotNone(compare_saved(batch, stream, 1, 20))

    def test_missing_action_has_the_next_timestamp(self):
        batch = {"actions": [action(20)], "state": state()}
        stream = {"actions": [], "state": state()}
        self.assertEqual(compare_saved(batch, stream, 1, 20)["time_ms"], 20)

    def test_session_chart_opens_align_without_utc_midnight(self):
        bars = [SimpleNamespace(timestamp=1000 + index * 60000) for index in range(100)]
        SETTINGS["splits"] = [0.8, 0.9]
        opens = [bars[index].timestamp for index in (0, 40, 82, 93)]
        self.assertEqual(stream_splits(bars, len(bars), "1D", opens), [82, 93])

    def test_chart_handoff_must_exist(self):
        bars = [SimpleNamespace(timestamp=1000 + index * 60000) for index in range(100)]
        SETTINGS["splits"] = [0.8, 0.9]
        with self.assertRaisesRegex(RuntimeError, "no chart boundary"):
            stream_splits(bars, len(bars), "1D", [bars[40].timestamp])

    def test_pushed_bar_preflight_is_a_refusal_not_trade_divergence(self):
        self.assertTrue(is_stream_refusal(
            "strategy_stream_push_bar[5]: rc=-1: native stream has an in-session gap"))
        self.assertFalse(is_stream_refusal("equivalence_export_actions: rc=-1"))

    def test_cumulative_report_subnormal_difference_is_not_rounded(self):
        batch = {"actions": [], "state": state()}
        stream = {"actions": [], "state": state()}
        stream["state"]["totals"]["net_profit"] = float.fromhex("0x0.0000000000001p-1022")
        difference = compare_saved(batch, stream, 1, 20)
        self.assertEqual(difference["path"], "state.totals.net_profit")

    def test_recompare_preserves_refusal_after_completed_handoff(self):
        with TemporaryDirectory() as temporary:
            output = Path(temporary)
            case = output / "case"
            case.mkdir()
            result = {"probe": "case", "result": "FAIL", "proof_gap": True,
                      "error": "strategy_stream_push_bar[5]: rc=-1: native stream has an in-session gap",
                      "splits": [{"index": 1, "time_ms": 20, "result": "PASS"}]}
            (case / "result.json").write_text(json.dumps(result))
            recompare(output)
            saved = json.loads((case / "result.json").read_text())
            self.assertEqual(saved["result"], "STREAM-UNSUPPORTED")
            self.assertEqual(saved["splits"], result["splits"])
            self.assertNotIn("proof_gap", saved)


if __name__ == "__main__":
    unittest.main()
