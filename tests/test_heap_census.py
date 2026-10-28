"""L0B: the census must count live bytes once and never hide an unknown slope.

Written before heap_census.py. The lab also runs the real battery on the base:
its red result must be retained engine allocations, not a missing sink flag.
These controls test the evidence reader, not the engine's memory contract.
"""
import contextlib
import ctypes
import hashlib
import importlib
import io
import json
import mmap
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

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
        command_event = "std::variant<" + ", ".join(
            "pineforge::native_order_v8::" + name for name in (
                "AcceptedEvent", "RejectedEvent", "ReplacedEvent", "ReplaceRejectedEvent",
                "CancelledEvent", "NotWorkingEvent", "InvalidHandleEvent", "NoEffectEvent",
                "MatchRejectedEvent", "ExecutionAppliedEvent", "CloseBoundEvent", "ActivatedEvent",
                "ReservationReducedEvent", "DeferredGroupAdjustmentEvent", "QuantityBoundEvent",
                "ArmedEvent", "TermsResolvedEvent", "MarginCallEvent", "NativeRiskEvent")) + ">"
        frames = (
            ("L19a", "pineforge::native_order_v8::WorkingRequestCore::index_committed("
             + command_event + " const&) (src/native_order.cpp:1206)"),
            ("L19b", "std::vector<pineforge::native_order_v8::WorkingRequestCore::ReceiptKey, "
             "std::allocator<pineforge::native_order_v8::WorkingRequestCore::ReceiptKey> >::"
             "_M_realloc_insert<>(__gnu_cxx::__normal_iterator<"
             "pineforge::native_order_v8::WorkingRequestCore::ReceiptKey*, "
             "std::vector<pineforge::native_order_v8::WorkingRequestCore::ReceiptKey> >) (vector.tcc:453)"),
            ("L19c", "std::vector<pineforge::native_order_v8::CohortReceipt, "
             "std::allocator<pineforge::native_order_v8::CohortReceipt> >::"
             "_M_default_append(unsigned long) (vector.tcc:627)"),
            ("L19d", "pineforge::source::OriginRoster::push_back("
             "pineforge::native_order_v8::RequestHandle const&) (include/pineforge/source/pine_adapter.hpp:945)"),
            ("L19e", "pineforge::admission::market_admission_v2::Journal::append("
             "std::variant<pineforge::admission::market_admission_v2::CommandEvent, "
             "pineforge::admission::market_admission_v2::ReviewEvent, "
             "pineforge::admission::market_admission_v2::SizingEvent>) (src/source/market_admission.cpp:151)"),
            ("L19e", "pineforge::admission::market_admission_v2::Journal::reserve() "
             "(src/source/market_admission.cpp:125)"),
            ("L19h", "pineforge::source::BracketRoster::put(std::vector<unsigned char, "
             "std::allocator<unsigned char> >&, unsigned long&, unsigned long) "
             "(include/pineforge/source/pine_adapter.hpp:1025)"),
            ("L19l", "pineforge::source::PineExecutionAdapter::note_exit_rank_key("
             "std::__cxx11::basic_string<char> const&) (src/source/pine_adapter.cpp:6339)"),
            ("L19l", "pineforge::source::PlacementTable::fold_sequence("
             "pineforge::detail::IdPairStore<unsigned long>&, "
             "pineforge::source::PlacementSnapshot const&) (include/pineforge/source/pine_adapter.hpp:864)"),
            ("L19k", "pineforge::source::tradingview_multi_day_stamps("
             "int, pineforge::Bar const*, int, bool, std::vector<pineforge::Bar> const&, "
             "std::__cxx11::basic_string<char> const&, std::__cxx11::basic_string<char> const&, "
             "bool) (src/source/pine_strategy_host.cpp:1601)"),
            ("L3", "void pineforge::native_module::run_batch<CensusHost>(void*, pf_bar_s*, int, "
             "char const*, char const*, bool, pf_report_s*) (include/pineforge/native_module.hpp:238)"),
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
             "double, std::__cxx11::basic_string<char> const&, double, double, long, double, long) "
             "(src/engine_metrics.cpp:301)"),
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
                         "pineforge::detail::IdPairStore<unsigned long>&, "
                         "pineforge::source::PlacementSnapshot const&)"), ("L19l", False))
        applied = ("pineforge::source::PineExecutionAdapter::on_applied("
                   "pineforge::native_order_v8::ExecutionAppliedEvent const&, "
                   "pineforge::native_driver_v5::NativeDecisionContext const&)")
        id_node = ("std::__detail::_Hash_node<std::pair<std::__cxx11::basic_string<char> const, "
                   "long>, true>::_Hash_node() <- " + applied)
        self.assertEqual(census.owner(id_node), ("L19l", False))
        self.assertEqual(census.owner(id_node.replace(", long>", ", unsigned long>")), ("L19j", False))

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


class CorpusWorkerMapping(unittest.TestCase):
    def runner(self, load, run):
        # Use the actual request parser and installation loop. Only native
        # execution and CSV loading are replaced; the worker's wrapping,
        # chart selection, mmap copies and checkpoint are all real.
        with mock.patch.object(sys, "path", [str(census.ROOT / "scripts"), *sys.path]):
            real = importlib.import_module("run_strategy")
        runner = types.ModuleType("run_strategy")
        runner.BarC = real.BarC
        runner._load_bars = load
        runner._parse_requests_feed = real._parse_requests_feed
        runner.ensure_derived = lambda: self.fail("worker must replace derivation")

        class Strategy:
            _install_probe_requests = real.Strategy._install_probe_requests

            def __init__(self, library):
                self.lib = types.SimpleNamespace()

        Strategy.run = run
        runner.Strategy = Strategy
        return runner

    def worker(self, runner, output, bars_limit=4):
        capture = io.StringIO()
        with mock.patch.dict(sys.modules, {"run_strategy": runner}), \
                mock.patch.object(sys, "argv", list(sys.argv)), \
                mock.patch.object(sys, "path", list(sys.path)), \
                mock.patch.object(sys, "stdin", types.SimpleNamespace(fileno=lambda: 0)), \
                mock.patch.object(census.os, "read", return_value=b"\n") as acknowledgement, \
                contextlib.redirect_stdout(capture):
            result = census.corpus_worker(output / "probe", bars_limit, output)
        self.assertEqual(result, 0)
        acknowledgement.assert_called_once_with(0, 1)
        markers = [line[len(census.MARKER):] for line in capture.getvalue().splitlines()
                   if line.startswith(census.MARKER)]
        self.assertEqual(len(markers), 1)
        self.assertEqual(json.loads(markers[0])["bars"], bars_limit)

    def assert_mapped(self, array):
        references = (array._objects or {}).values()
        self.assertTrue(any(isinstance(value, memoryview) and isinstance(value.obj, mmap.mmap)
                            for value in references), "caller tape must be backed by mmap, not a ctypes heap allocation")

    def test_quote_load_before_strategy_run_is_not_selected_as_the_chart(self):
        calls, inputs = [], {}
        runner = None

        def load(path, **kwargs):
            calls.append((Path(path), dict(kwargs)))
            # The SAME file is used by planning and execution. A pathname
            # match alone is insufficient: the wrapper must delimit the run.
            bars = (runner.BarC * 8)()
            for index in range(8):
                bars[index].timestamp = 1000 + index * 60000
                bars[index].close = 100.0 + index
            selected = [bar for bar in bars if bar.timestamp <= kwargs.get("ohlcv_end_ms", 10**12)]
            effective = (runner.BarC * len(selected))(*selected)
            return effective, len(selected), "source-identity"

        def run(strategy, path, *, on_report):
            bars, count, identity = runner._load_bars(path, ohlcv_start_ms=1000)
            self.assert_mapped(bars)
            self.assertEqual(identity, "source-identity")
            inputs.update(count=count, end=bars[count - 1].timestamp)
            feed, feed_count, _ = runner._load_bars(Path("feed.csv"), ohlcv_end_ms=10**12)
            self.assertEqual(feed_count, count)
            self.assert_mapped(feed)
            on_report(types.SimpleNamespace(input_bars_processed=count, total_trades=2))
            return {"trades": []}

        runner = self.runner(load, run)

        def main():
            strategy = runner.Strategy(Path("strategy.so"))
            quote, count, _ = runner._load_bars(Path("chart.csv"), ohlcv_start_ms=1000)
            self.assertEqual(count, 8, "the magnifier planner must see its full quote feed")
            self.assertEqual(quote[count - 1].close, 107.0)
            strategy.run(Path("chart.csv"))
            return 0

        runner.main = main
        with tempfile.TemporaryDirectory() as directory:
            self.worker(runner, Path(directory))
        self.assertEqual(inputs, {"count": 4, "end": 181000})
        self.assertEqual(calls[0][1], {"ohlcv_start_ms": 1000})
        self.assertEqual(calls[1][1], {"ohlcv_start_ms": 1000})
        self.assertEqual(calls[2][1], {"ohlcv_end_ms": 181000})

    def test_probe_symbol_feed_parser_maps_bars_closes_and_columns_before_install(self):
        runner = None
        installed = []

        def load(path, **kwargs):
            bars = (runner.BarC * 4)()
            for index in range(4):
                bars[index].timestamp = 1000 + index * 60000
            return bars, 4, "chart-identity"

        def install_feed(state, key, timeframe, bars, closes, count):
            self.assert_mapped(bars)
            self.assert_mapped(closes)
            self.assertEqual(count, 2)
            self.assertEqual(ctypes.sizeof(bars), 2 * ctypes.sizeof(runner.BarC))
            self.assertEqual(ctypes.sizeof(closes), 2 * ctypes.sizeof(ctypes.c_int64))
            self.assertEqual((key, timeframe), (b"TEST:OTHER", b"1"))
            self.assertEqual([bar.timestamp for bar in bars], [1000, 61000])
            self.assertEqual(list(closes), [61000, 121000])
            self.assertEqual([bar.close for bar in bars], [1.5, 2.5])
            self.assertNotEqual(bars[0].volume, bars[0].volume)  # NaN survives the byte copy.
            installed.append("feed")
            return 0

        def install_column(state, key, timeframe, name, values, count):
            self.assert_mapped(values)
            self.assertEqual((key, timeframe, name, count), (b"TEST:OTHER", b"1", b"extra", 2))
            self.assertEqual(ctypes.sizeof(values), 2 * ctypes.sizeof(ctypes.c_double))
            self.assertEqual(values[0], -0.0)
            self.assertEqual(bytes(values)[:8], bytes(ctypes.c_double(-0.0)))
            self.assertNotEqual(values[1], values[1])
            installed.append("column")
            return 0

        def run(strategy, path, *, probe_requests, on_report):
            bars, count, _ = runner._load_bars(path)
            strategy.lib.strategy_set_symbol_feed = install_feed
            strategy.lib.strategy_set_symbol_feed_column = install_column
            strategy._install_probe_requests(None, probe_requests)
            # The request dictionary itself must hold mapped arrays, not the
            # original allocations alongside mapped setter arguments.
            feed = probe_requests["feeds"][0]
            for array in (feed["bar_array"], feed["close_ms"], feed["extras"][0][1]):
                self.assert_mapped(array)
            on_report(types.SimpleNamespace(input_bars_processed=count, total_trades=0))
            return {"trades": []}

        runner = self.runner(load, run)

        def main():
            columns = ["timestamp", "time_close", "open", "high", "low", "close", "volume", "extra"]
            data = (",".join(columns) + "\n1000,61000,1,2,0.5,1.5,NaN,-0\n"
                    "61000,121000,2,3,1.5,2.5,10,NaN\n").encode()
            bars, closes, count, extras = runner._parse_requests_feed(data, columns, "control", "TEST:OTHER@1")
            requests = {"symbols": [], "recorded": [], "feeds": [dict(symbol="TEST:OTHER", timeframe="1",
                        bar_array=bars, close_ms=closes, bars=count, extras=extras)]}
            runner.Strategy(Path("strategy.so")).run(Path("chart.csv"), probe_requests=requests)
            return 0

        runner.main = main
        with tempfile.TemporaryDirectory() as directory:
            self.worker(runner, Path(directory))
        self.assertEqual(installed, ["feed", "column"])


if __name__ == "__main__":
    unittest.main()
