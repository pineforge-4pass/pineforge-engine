"""L0B: the census must count live bytes once and never hide an unknown slope.

Written before heap_census.py. The lab also runs the real battery on the base:
its red result must be retained engine allocations, not a missing sink flag.
These controls test the evidence reader, not the engine's memory contract.
"""
import unittest

import heap_census as census


def massif(rows, total):
    return ("desc: --tool=massif\ncmd: fixture\ntime_unit: B\n"
            "#-----------\nsnapshot=0\n#-----------\ntime=42\n"
            f"mem_heap_B={total}\nmem_heap_extra_B=16\nmem_stacks_B=0\n"
            "heap_tree=detailed\n" + rows)


class CensusEvidence(unittest.TestCase):
    def test_only_leaves_are_counted_and_addresses_do_not_name_sites(self):
        tree = ("n2: 96 (heap allocation functions) malloc/new/new[]\n"
                " n1: 64 0x123: operator new(unsigned long) (new_op.cc:95)\n"
                "  n0: 64 0x234: pineforge::source::OriginRoster::push_back() (a.hpp:950)\n"
                " n0: 32 0x345: unrecognized_cache() (cache.cpp:7)\n")
        snap = census.parse_massif(massif(tree, 96))
        self.assertEqual(sum(snap.sites.values()), 96)
        self.assertEqual(len(snap.sites), 2)
        changed = tree.replace("0x123", "0xFFF").replace("a.hpp:950", "a.hpp:999")
        self.assertEqual(snap.sites, census.parse_massif(massif(changed, 96)).sites)
        self.assertEqual(snap.heap_bytes, 96)

    def test_a_growing_unknown_is_L19j_and_is_never_admitted(self):
        low = census.Snapshot(10, {"new_cache()": 10})
        high = census.Snapshot(50, {"new_cache()": 50})
        rows = census.growth(low, high, 10, 30)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["lane"], "L19j")
        self.assertFalse(rows[0]["admitted"])
        self.assertEqual(rows[0]["bytes_per_unit"], 2.0)
        self.assertFalse(census.flat(rows))

    def test_only_the_callers_explicit_ohlcv_allocation_is_admitted(self):
        low = census.Snapshot(48, {"census_chart_ohlcv(int)": 48})
        high = census.Snapshot(192, {"census_chart_ohlcv(int)": 192})
        self.assertTrue(census.flat(census.growth(low, high, 1, 4)))
        copied = "pineforge::source::PineScheduler::capture_begin()"
        self.assertEqual(census.owner(copied), ("L3", False))
        self.assertEqual(census.owner("input_next_ms_"), ("L19i (or L3/L4)", False))
        self.assertEqual(census.owner("something_else (pine_adapter.cpp)"),
                         ("L19j", False))

    def test_no_stack_or_unexpanded_tree_cannot_pass(self):
        for text in ("", massif("", 40),
                     massif("n0: 39 (heap allocation functions)\n", 40),
                     massif("n1: 40 (heap allocation functions)\n", 40),
                     massif("n0: 40 (heap allocation functions)\n", 40)):
            with self.subTest(text=text), self.assertRaises(ValueError):
                census.parse_massif(text)
        hidden = census.parse_massif(massif(
            "n1: 40 (heap allocation functions)\n"
            " n0: 40 in 1 place, below massif's threshold\n", 40))
        self.assertFalse(census.flat(census.growth(census.Snapshot(0, {}), hidden, 1, 4)))

    def test_broken_tree_and_duplicate_snapshot_are_refused(self):
        broken = "n1: 40 (heap allocation functions)\n n0: 50 cache()\n"
        with self.assertRaises(ValueError):
            census.parse_massif(massif(broken, 40))
        text = massif("n1: 40 (heap allocation functions)\n n0: 40 cache()\n", 40)
        with self.assertRaises(ValueError):
            census.parse_massif(text + text)

    def test_q9_subtracts_the_same_trade_work_at_both_rungs(self):
        # Both cases pay 100 B/trade. Unique ids pay 20 B/additional id.
        reused_low = census.Snapshot(1000, {"trade": 1000})
        unique_low = census.Snapshot(1180, {"trade": 1000, "ids": 180})
        reused_high = census.Snapshot(1000000, {"trade": 1000000})
        unique_high = census.Snapshot(1199980, {"trade": 1000000, "ids": 199980})
        result = census.id_cost(reused_low, unique_low, reused_high, unique_high, 10, 10000)
        self.assertEqual(result["bytes_per_additional_id"], 20.0)
        self.assertEqual(result["denominator_ids"], 9990)
        self.assertEqual(result["sites"][0]["site"], "ids")
        self.assertEqual(result["sites"][0]["lane"], "L19j")
        with self.assertRaises(ValueError):
            census.id_cost(reused_low, unique_low, reused_high, unique_high, 10, 10)


if __name__ == "__main__":
    unittest.main()
