# CR-150 P00 declaration contracts

Authority: `core-cr150-design/work/DESIGN.md`, revision 6 (2026-10-18).
All DESIGN line numbers below refer to that revision. Dispatch:
`PARALLEL-PLAN.md:27-30,63`. C++ declarations spell the operations described
by DESIGN; OPEN items below preserve its unspecified policies.

These are declarations only. No existing file, execution path, feature probe,
version or caller-visible layout is changed. Definitions and lab proof belong
to CORE-TOP and the implementation lanes. Nothing is built, compiled or tested
on the P00 machine.

## Sink ABI and line writer

| File / declaration | DESIGN section and lines | Implementing lane |
|---|---|---|
| `include/pineforge/report_sink.h`: `pf_report_sink_v1` | 6:1255-1266,1284-1286 | L6b-1 |
| `pf_report_sink_receipt_v1` and its layout assertions | 6:1268-1283 | L6b-1 |
| `pf_report_sink_version(void)` | 6:1277 | L6b-1 |
| `strategy_set_report_sink_v1(pf_strategy_t,const pf_report_sink_v1*)` | 6:1278,1284-1294 | L6b-1 |
| `strategy_report_sink_receipt_v1(pf_strategy_t,pf_report_sink_receipt_v1*)` | 6:1279; 3.7:788-790 | L6b-1 |
| `include/pineforge/report_sink_writer.hpp`: `report_sink::RecordSet` | 3.7:792-806,820-828; 6:1287-1288 | L6a/L6b-3 |
| `report_sink::Trailer` | 3.7:830-834; 6:1268-1275 | L6a |
| `report_sink::f64(double)` | 3.7:847-850,855-872 | L6a |
| `report_sink::escaped_string(std::string_view)` | 3.7:875-876 | L6a |
| `report_sink::LineWriter` constructor/destructor and opaque `State` | 3.7:878-885; 6:1284 | L6a |
| `LineWriter::write_header(const RecordSet&)` | 3.7:783-806,878-880 | L6a/L6b-1 |
| `LineWriter::write_e(const pf_equity_point_t&)` | 3.7:822; 3.2:487-506; 3.6:753-770 | L6a/L6b-1/2 |
| `LineWriter::write_t(...)` | 3.7:823; 3.3:544-548; 3.4:636-649 | L6a/L6b-1/L8c |
| `LineWriter::write_trailer()` | 3.7:830-834,878-885 | L6a/L6b-1 |

`report_sink.h` follows the additive runtime-header pattern used by
`execution_observer.h`. `scripts/check_c_abi_runtime.py` currently names
`pineforge.h`, `execution_observer.h`, `selected_window.h` in `RUNTIME_HEADERS`.
P00 adds no definitions, so it leaves that inventory and its counts intact.
L6b-1 must enroll `report_sink.h`, the three definitions and their counts
together, and add `PINEFORGE_HAS_REPORT_SINK_V1` only with runtime support
(DESIGN 6:1245-1248,1310-1314). There is no transitive include from an existing
header in P00. O/V/K/X/H operations remain L6b-3's extension.

## TradeLedger

| File / declaration | DESIGN section and lines | Implementing lane |
|---|---|---|
| `include/pineforge/trade_ledger.hpp`: `engine_script_run_v19::TradeLedger`, type aliases, lifetime/copy/move declarations, opaque `State` | 3.4:609-617; 6:1321-1324 | L8a |
| `TradeLedger::size() const`, `base() const` | 3.4:610-615 | L8a |
| `TradeLedger::operator[](size_type)` (mutable/const), `back()` (mutable/const) | 3.4:610-613,661-667 | L8a/L8d |
| `TradeLedger::push_back(const Trade&)`, `push_back(Trade&&)` | 3.4:610-613 | L8a |
| `TradeLedger::resize(size_type)` | 3.4:611,661-662 | L8a |
| `TradeLedger::begin()/end()` (mutable/const) | 3.4:612 | L8a |
| `TradeLedger::seal(size_type)` and its caller's five-condition proof / fold-write-spill-before-drop contract | 3.4:619-685 | L8b/L8c/L18 |

The total-count versus resident-window distinction and closedtrades.*
retention exception are binding doc contracts (3.4:610-617,687-708).
`set_trade_history_streamable` and its feature probe remain the engine/codegen
lanes' work; P00 neither advertises them nor changes `trades_`.

## Metric accumulator and point sources

| File / declaration | DESIGN section and lines | Implementing lane |
|---|---|---|
| `include/pineforge/metrics_stream.hpp`: 24-byte `pf_equity_point_t` layout assertions | 3.3:538-540; 3.8:903-904 | L6b-4/L7c |
| `metrics::TradeStatsAccumulator` constructor/destructor, copy declarations, opaque `State` | 3.3:515-526,544-550 | L7a |
| `TradeStatsAccumulator::fold(const TradeC&)`, `finish() const` | 3.3:526,544-548 | L7a |
| `metrics::ArrayCurve` constructor, `size() const`, `operator[](int64_t) const` | 3.3:532-540 | L7b |
| `metrics::ScratchCurveRegion` | 3.8:899-906; 3.3:534,538-540 | L6b-4 |
| `metrics::SpillCurve` constructor/destructor, `size() const`, `operator[](int64_t) const`, opaque `State` | 3.3:532-543; 3.8:901-906 | L6b-4/L7c |
| `metrics::compute_equity_stats<PointSource>(...)` (ordinary denominator) | 3.3:528-543 | L7b/L7c |
| `metrics::compute_equity_stats<PointSource>(...,observation_count)` | 3.3:528-543; 3.6:742-746,766-770 | L7b/L7c/L17b |

Only the named accumulator/point-source operations are frozen here. The
month/per-bar generators are the metric implementation's internal walks;
their unprescribed helper names and storage are not made a second API.
Scratch curve/position-tail placement remains either regions or separate
private files, as DESIGN permits. No mapped scratch or text-based metrics.

## OPEN items

| ID | Unspecified by DESIGN; no policy inferred | Owner |
|---|---|---|
| S1 | Receipt query return codes, required caller initialization, refusal output, and query behavior without a configured sink (6:1279). | CORE-TOP / L6b-1 |
| S2 | Internal writer error transport, oversized-line/unsafe-integer failure details, and what a repeated fill/capture does after a trailer; DESIGN requires repeatable metric reads but gives no writer reset/idempotence contract (3.3:547-548; 3.7:787,878-885). | CORE-TOP / L6a/L6b-1 |
| T1 | Ledger reset/capacity API, reference/iterator invalidation, and empty-back/above-end error policy are not specified. Below-base internal access MUST throw engine_invariant; C accessors MUST use existing sentinels (3.4:610-613,661-667). | CORE-TOP / L8a/L8d |
| M1 | Point-source malformed-region/bounds/short-read error transport and concurrent-reader rules are not specified; the 24-byte binary points, 64 KiB pread window and repeated forward walks ARE fixed (3.3:532-543; 3.8:899-906). | CORE-TOP / L6b-4/L7c |

## Lab handoff

CORE-TOP runs the headers' compile proof, `scripts/check_c_abi_runtime.py`,
and `scripts/check_native_cpp_versions.py` on the lab queue. P00 supplies no
local test or guard verdict. Repository-wide unit/corpus and merge-gate proof
remain required before integration is declared verified.
