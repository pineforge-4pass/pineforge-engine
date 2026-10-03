import unittest
from types import SimpleNamespace

from native_confirmed_bar_stream_scan import (
    compare_saved, semantic_actions, semantic_state, stream_splits, SETTINGS,
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


if __name__ == "__main__":
    unittest.main()
