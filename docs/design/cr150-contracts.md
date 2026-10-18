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

## Bar and close views

| File / declaration | DESIGN section and lines | Implementing lane |
|---|---|---|
| `include/pineforge/native_bar_views.hpp`: `inline namespace native_bar_views_v1` | 2.2:336-344; 6:1305-1306 | L15a |
| `NativeBarView { const Bar* data; std::size_t size; }` | 2.2:345 | L15a |
| `NativeInstrumentFeedView { NativeBarView bars; const int64_t* close_ms; }` | 2.2:346 | L15a/L15b |
| `class NativeLowerPathSource;` (forward declaration only; OPEN V2) | 2.2:347-348; 5:1214-1239 | L15a/L16 |

L15a implements staging and the common accessor, with no caller. It also adds
the new header/source and namespace check to the native ownership guard
(2.2:349-375). Neither the existing v3 run-spec nor native-host declarations
change here. The borrowed-feed C export/probe belongs to L15b (6:1295-1306).

### L15a read inventory

This is DESIGN's required source-guard read set (2.2:354-368; 5:1198-1205),
including the indirect count/hash consumers. Source line references are the
revision-6 design's snapshot; L15a must refresh offsets and mechanically pin
every current `.bars`/`.close_ms` read when it installs the accessor/guard.
P00 does not open the measured consumer or source-layer units.

| Reader to route through the view/source | Existing source references from DESIGN | DESIGN lines |
|---|---|---|
| Kernel instrument-feed pump: current bar, close, next open, count | `src/native_execution_consumer.cpp:9932-9983` | 354-355 |
| Host feed lookup/delivery and column reads | `src/source/pine_security_eval.cpp:1059-1068,1286-1293,1312-1320` | 356-357 |
| Feed validation: bar values/order, close length/bounds, column length; configure and consumer entry points | `src/native_run_spec.cpp:254-276,730,995-998`; `src/native_execution_consumer.cpp:9470-9471,9494-9495` | 358-360 |
| Instrument-feed digest and cached/uncached continuation folds | `src/native_run_spec.cpp:1124-1160`; `src/native_execution_consumer.cpp:321-323,1935-1936,12730-12735` | 361-362 |
| Staged feed counts in host state and its hash | `src/source/pine_strategy_host.cpp:230-234`; `src/source/pine_state_hash.cpp:292-299` | 363-364 |
| Refresh of symbol-data digest and host hash fold | `src/source/pine_security_eval.cpp:1536-1544`; `src/source/pine_state_hash.cpp:1123-1129` | 364-365 |
| Restore/store upkeep and column-setter length check | `src/source/pine_security_eval.cpp:1091-1096,1420-1422` | 366-367 |
| Lower-path preflight/validation | `src/native_execution_consumer.cpp:3148-3181` | 368,1198-1199 |
| Lower-path digest (count first) and continuation hash | `src/native_run_spec.cpp:867-895`; `src/native_execution_consumer.cpp:288` | 368,1200-1201 |
| Per-script-bar lower-path binary search/walk | `src/native_execution_consumer.cpp:8562-8576` | 368,1202 |
| Scheduler sub-bar lookup and one-bar look-ahead | `src/source/pine_scheduler_native.cpp:252-264,296-298` | 368,1203-1204 |
| Lower-path emptiness test | `src/source/pine_adapter.cpp:3420-3422` | 368,1205 |

Every count/digest must fold the same bytes/order for copied and borrowed
feeds, including cached and uncached paths (2.2:369-372). Views replace storage;
metadata, validation and continuation identity remain the existing contract.

## OPEN items

| ID | Unspecified by DESIGN; no policy inferred | Owner |
|---|---|---|
| S1 | Receipt query return codes, required caller initialization, refusal output, and query behavior without a configured sink (6:1279). | CORE-TOP / L6b-1 |
| S2 | Internal writer error transport, oversized-line/unsafe-integer failure details, and what a repeated fill/capture does after a trailer; DESIGN requires repeatable metric reads but gives no writer reset/idempotence contract (3.3:547-548; 3.7:787,878-885). | CORE-TOP / L6a/L6b-1 |
| T1 | Ledger reset/capacity API, reference/iterator invalidation, and empty-back/above-end error policy are not specified. Below-base internal access MUST throw engine_invariant; C accessors MUST use existing sentinels (3.4:610-613,661-667). | CORE-TOP / L8a/L8d |
| M1 | Point-source malformed-region/bounds/short-read error transport and concurrent-reader rules are not specified; the 24-byte binary points, 64 KiB pread window and repeated forward walks ARE fixed (3.3:532-543; 3.8:899-906). | CORE-TOP / L6b-4/L7c |
| V1 | Typed staging/accessor signatures, feed binding/lookup and absent-versus-present-empty view representation are not specified. DESIGN names stage_instrument_feed_views / stage_lower_path_source but no prototypes; none are invented (2.2:349-353). | CORE-TOP / L15a |
| V2 | NativeLowerPathSource operation return/parameter types, visitor protocol and dispatch/error model are not specified. Only its forward declaration and required behavior are frozen; typed members await a ruling (2.2:347-348; 5:1214-1239). | CORE-TOP / L15a/L16 |

## Lab handoff

CORE-TOP runs the headers' compile proof, `scripts/check_c_abi_runtime.py`,
and `scripts/check_native_cpp_versions.py` on the lab queue. P00 supplies no
local test or guard verdict. Repository-wide unit/corpus and merge-gate proof
remain required before integration is declared verified.
