"""L0B: the census must count live bytes once and never hide an unknown slope.

Written before heap_census.py. The lab also runs the real battery on the base:
its red result must be retained engine allocations, not a missing sink flag.
These controls test the evidence reader, not the engine's memory contract.
"""
import hashlib
import json
from pathlib import Path
import tempfile
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
        subscription = (
            "std::vector<long, std::allocator<long> >::_M_fill_assign(unsigned long, long const&) "
            "(vector.tcc:278) <- pineforge::engine_script_run_v19::NativeExecutionConsumer::"
            "begin_timeframe_subscriptions(pineforge::engine_script_run_v19::BacktestEngine&, "
            "pineforge::native_run_spec_v3::NativeRunSpec const&, pineforge::Bar const*, int, bool) "
            "(src/native_execution_consumer.cpp:9313)")
        self.assertEqual(census.owner(subscription), ("L19i (or L3/L4)", False))
        self.assertEqual(census.owner("something_else (pine_adapter.cpp)"),
                         ("L19j", False))

    def test_demangled_push_functions_and_element_types_name_known_lanes(self):
        frames = (
            ("L19a", "pineforge::native_order_v7::WorkingRequestCore::index_committed("
             "std::variant<pineforge::native_order_v7::AcceptedEvent> const&) (src/native_order.cpp:1206)"),
            ("L19b", "std::vector<pineforge::native_order_v7::WorkingRequestCore::ReceiptKey, "
             "std::allocator<pineforge::native_order_v7::WorkingRequestCore::ReceiptKey> >::"
             "_M_realloc_insert<>(__gnu_cxx::__normal_iterator<"
             "pineforge::native_order_v7::WorkingRequestCore::ReceiptKey*, "
             "std::vector<pineforge::native_order_v7::WorkingRequestCore::ReceiptKey> >) (vector.tcc:453)"),
            ("L19c", "std::vector<pineforge::native_order_v7::CohortReceipt, "
             "std::allocator<pineforge::native_order_v7::CohortReceipt> >::"
             "_M_default_append(unsigned long) (vector.tcc:627)"),
            ("L19d", "pineforge::source::OriginRoster::push_back("
             "pineforge::native_order_v7::RequestHandle const&) (include/pineforge/source/pine_adapter.hpp:945)"),
            ("L19e", "pineforge::admission::market_admission_v2::Journal::append("
             "std::variant<pineforge::admission::market_admission_v2::CommandEvent, "
             "pineforge::admission::market_admission_v2::ReviewEvent, "
             "pineforge::admission::market_admission_v2::SizingEvent>) (src/source/market_admission.cpp:151)"),
            ("L19e", "pineforge::admission::market_admission_v2::Journal::reserve() "
             "(src/source/market_admission.cpp:125)"),
            ("L19h", "pineforge::source::BracketRoster::put(std::vector<unsigned char>&, "
             "unsigned long&, unsigned long) (include/pineforge/source/pine_adapter.hpp:1027)"),
            ("L19l", "pineforge::source::PineExecutionAdapter::note_exit_rank_key("
             "std::__cxx11::basic_string<char> const&) (src/source/pine_adapter.cpp:6339)"),
            ("L19l", "pineforge::source::PlacementTable::fold_sequence("
             "std::unordered_map<std::__cxx11::basic_string<char>, "
             "pineforge::source::PlacementTable::SequenceMinima>&, "
             "pineforge::source::PlacementSnapshot const&) (include/pineforge/source/pine_adapter.hpp:864)"),
            ("L19k", "pineforge::source::tradingview_multi_day_stamps("
             "pineforge::Bar const*, int, int, std::__cxx11::basic_string<char> const&, "
             "std::__cxx11::basic_string<char> const&) (src/source/pine_strategy_host.cpp:1601)"),
            ("L3", "void pineforge::native_module::run_batch<CensusHost>(void*, pf_bar*, int, "
             "char const*, char const*, bool, pf_report*) (include/pineforge/native_module.hpp:238)"),
            ("L4", "pineforge::source::PineScheduler::run_begin(pineforge::source::PineStrategyHost&) "
             "(src/source/pine_scheduler_native.cpp:187)"),
            ("L15a/L15b", "pineforge::source::PineStrategyHost::set_symbol_feed("
             "std::__cxx11::basic_string<char> const&, std::__cxx11::basic_string<char> const&, "
             "pineforge::Bar const*, long const*, int) (src/source/pine_security_eval.cpp:1369)"),
            ("L16", "pineforge::source::tradingview_magnifier_bars(pineforge::Bar const*, int, "
             "std::__cxx11::basic_string<char> const&, std::__cxx11::basic_string<char> const&, "
             "std::__cxx11::basic_string<char> const&, std::__cxx11::basic_string<char> const&) "
             "(src/source/magnifier_intrabars.cpp:77)"),
            ("L7b/L17b", "pineforge::metrics::compute_equity_stats(pf_equity_point_s const*, long, "
             "double, char const*, long, long) (src/engine_metrics.cpp:301)"),
            ("L8a/L8c/L18", "pineforge::engine_script_run_v19::NativeExecutionConsumer::closed_rows_digest("
             "std::vector<pineforge::Trade, std::allocator<pineforge::Trade> > const&) const "
             "(src/native_execution_consumer.cpp:11789)"),
            ("L8a/L8c/L18", "pineforge::source::PineExecutionAdapter::set_exit_phase(unsigned long, "
             "unsigned char) (src/source/pine_adapter.cpp:6437)"),
            ("L6b-1/L6b-2/L17a", "pineforge::engine_script_run_v19::BacktestEngine::record_equity_point(long) "
             "(include/pineforge/engine.hpp:1386)"),
            ("L6b-3", "pineforge::engine_script_run_v19::NativeExecutionConsumer::record_script_report_point("
             "pineforge::engine_script_run_v19::BacktestEngine&, long) const "
             "(src/native_execution_consumer.cpp:8852)"),
            ("L6b-3", "pineforge::source::PineStrategyHost::scheduler_record_broker_hash() "
             "(src/source/pine_strategy_host.cpp:2794)"),
            ("L6b-3", "pineforge::engine_script_run_v19::BacktestEngine::output_bar(long, long) "
             "(src/engine_report.cpp:445)"),
            ("L6b-3", "pineforge::engine_script_run_v19::BacktestEngine::record_output_event(int, double, "
             "std::__cxx11::basic_string<char> const*) (src/engine_report.cpp:517)"),
            ("L6b-3", "pineforge::engine_script_run_v19::BacktestEngine::trace("
             "std::__cxx11::basic_string<char> const&, double) (src/engine_report.cpp:37)"),
            ("L9a/L9s/L17a", "pineforge::engine_script_run_v19::BacktestEngine::fill_metrics_section("
             "pineforge::ReportC*) const (src/engine_report.cpp:196)"),
        )
        for lane, frame in frames:
            with self.subTest(frame=frame):
                low = census.parse_massif(massif(f"n1: 8 (heap allocation functions)\n n0: 8 0x123: {frame}\n", 8))
                high = census.parse_massif(massif(f"n1: 32 (heap allocation functions)\n n0: 32 0x456: {frame}\n", 32))
                row, = census.growth(low, high, 1, 4)
                self.assertEqual(row["lane"], lane)
                self.assertFalse(row["admitted"])
                self.assertEqual(row["bytes_per_unit"], 8.0)

    def test_unfolded_vector_is_distinguished_from_placement_nodes_and_id_minima(self):
        push = ("pineforge::source::PlacementTable::try_emplace<"
                "pineforge::source::PlacementSnapshot const&>(unsigned long, "
                "pineforge::source::PlacementSnapshot const&) (include/pineforge/source/pine_adapter.hpp:772)")
        vector = "std::vector<unsigned long, std::allocator<unsigned long> >::_M_realloc_insert<unsigned long const&>()"
        node = "std::make_unique<pineforge::source::PlacementSnapshot, pineforge::source::PlacementSnapshot const&>()"
        self.assertEqual(census.owner(vector + " <- " + push), ("L19h", False))
        self.assertEqual(census.owner(node + " <- " + push), ("L19j", False))
        self.assertEqual(census.owner("pineforge::source::PlacementTable::fold_sequence("
                         "std::unordered_map<std::__cxx11::basic_string<char>, "
                         "pineforge::source::PlacementTable::SequenceMinima>&, "
                         "pineforge::source::PlacementSnapshot const&)"), ("L19l", False))

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

    def test_q9_keeps_negative_sites_and_does_not_sum_only_the_positive_costs(self):
        empty = census.Snapshot(0, {})
        old = census.Snapshot(100, {"reuse_capacity": 100})
        new = census.Snapshot(160, {"ids": 160})
        result = census.id_cost(empty, empty, old, new, 10, 20)
        self.assertEqual(result["bytes_per_additional_id"], 6.0)
        self.assertEqual(sorted(row["bytes_per_additional_id"] for row in result["sites"]), [-10.0, 16.0])

    def test_streamed_q9_checks_identity_even_when_the_digest_is_valid(self):
        def sink(second):
            documents = [
                {"schema": "pf-report-sink", "version": 1, "records": [
                    {"type": "T", "fields": ["n", "entry_id", "qty", "open_at_end"]}]},
                ["T", 1, "E00000000000000", 1.0, False],
                ["T", 2, second, 1.0, False],
            ]
            raw = b"".join((json.dumps(item, separators=(",", ":")) + "\n").encode() for item in documents)
            trailer = dict(trailer="pf-report-sink", counts={"T": 2}, sha256=hashlib.sha256(raw).hexdigest())
            return raw + (json.dumps(trailer) + "\n").encode()

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sink.jsonl"
            path.write_bytes(sink("E00000000000001"))
            census.validate_sink_ids(path, 2, 0, False)
            path.write_bytes(sink("E00000000000000"))
            with self.assertRaisesRegex(ValueError, "identity"):
                census.validate_sink_ids(path, 2, 0, False)

    def test_nonincreasing_populations_are_not_a_flat_verdict(self):
        with self.assertRaises(ValueError):
            census.growth(census.Snapshot(100, {}), census.Snapshot(100, {}), 10, 10)


if __name__ == "__main__":
    unittest.main()
