import unittest

from native_live_equivalence_e2e import first_difference, live_action, synthetic_prints


class HarnessContract(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
