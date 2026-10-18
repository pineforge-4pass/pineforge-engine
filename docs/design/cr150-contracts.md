# CR-150 P00 declaration contracts

Authority: `core-cr150-design/work/DESIGN.md`, final revision 6 (2026-10-18).
SHA-256 recorded by the P00-7 gating read:
`68bda6e37931c096522a348603623c7f3cba2b1371a9c0a442edbf53d25ab0ad`.
All DESIGN line citations below use that final text. Dispatch: the external
design lane's `PARALLEL-PLAN.md`, lines 27–30 and 63. C++ declarations spell
the operations described by DESIGN; OPEN items below preserve its unspecified
policies.

CORE-TOP's rev 6 amendment (10-18 16:04) in the external design lane's
`core-cr150-design/work/REVISION.md`, lines 342–348, resolves S1 and T1.
References to amendment S1 or T1 below name that ruling, which binds L6b-1
and L8a respectively.

These are declarations only. No existing file, execution path, feature probe,
version or caller-visible layout is changed. Definitions and lab proof belong
to CORE-TOP and the implementation lanes. Nothing is built, compiled or tested
on the P00 machine.

## Sink ABI and line writer

| File / declaration | DESIGN / amendment authority | Implementing lane |
|---|---|---|
| `include/pineforge/report_sink.h`: `pf_report_sink_v1` | 6:1283-1289,1307-1309 | L6b-1 |
| `pf_report_sink_receipt_v1` and its layout assertions | 6:1291-1298,1305-1306 | L6b-1 |
| `pf_report_sink_version(void)` | 6:1300 | L6b-1 |
| `strategy_set_report_sink_v1(pf_strategy_t,const pf_report_sink_v1*)` | 6:1301,1307-1317 | L6b-1 |
| `strategy_report_sink_receipt_v1(pf_strategy_t,pf_report_sink_receipt_v1*)` | 6:1302; 3.7:791-793; amendment S1 | L6b-1 |
| `include/pineforge/report_sink_writer.hpp`: `report_sink::RecordSet` | 3.7:795-809,823-831; 6:1310-1311 | L6a/L6b-3 |
| `report_sink::Trailer` | 3.7:833-837; 6:1291-1298 | L6a |
| `report_sink::f64(double)` | 3.7:850-853,858-875 | L6a |
| `report_sink::escaped_string(std::string_view)` | 3.7:878-879 | L6a |
| `report_sink::LineWriter` constructor/destructor and opaque `State` | 3.7:881-888; 6:1307 | L6a |
| `LineWriter::write_header(const RecordSet&)` | 3.7:786-809,881-883 | L6a/L6b-1 |
| `LineWriter::write_e(const pf_equity_point_t&)` | 3.7:825; 3.2:490-509; 3.6:756-773 | L6a/L6b-1/2 |
| `LineWriter::write_t(...)` | 3.7:826; 3.3:547-551; 3.4:639-652 | L6a/L6b-1/L8c |
| `LineWriter::write_trailer()` | 3.7:833-837,881-888 | L6a/L6b-1 |

`report_sink.h` follows the additive runtime-header pattern used by
`execution_observer.h`. `scripts/check_c_abi_runtime.py` currently names
`pineforge.h`, `execution_observer.h`, `selected_window.h` in `RUNTIME_HEADERS`.
P00 adds no definitions, so it leaves that inventory and its counts intact.
L6b-1 must enroll `report_sink.h`, the three definitions and their counts
together, and add `PINEFORGE_HAS_REPORT_SINK_V1` only with runtime support
(DESIGN 6:1268-1271,1333-1338). There is no transitive include from an existing
header in P00. O/V/K/X/H operations remain L6b-3's extension.

## TradeLedger

| File / declaration | DESIGN / amendment authority | Implementing lane |
|---|---|---|
| `include/pineforge/trade_ledger.hpp`: `engine_script_run_v19::TradeLedger`, type aliases, lifetime/copy/move declarations, opaque `State` | 3.4:612-620; 6:1344-1347 | L8a |
| `TradeLedger::clear()` | Amendment T1: size 0, base 0 | L8a |
| `TradeLedger::reserve(size_type)` | Amendment T1: a capacity hint | L8a |
| `TradeLedger::size() const`, `base() const` | 3.4:613-618 | L8a |
| `TradeLedger::empty() const` | Amendment T1: size() == 0 | L8a |
| `TradeLedger::operator[](size_type)` (mutable/const), `back()` (mutable/const) | 3.4:613-616,664-670 | L8a/L8d |
| `TradeLedger::at(size_type global)` | Amendment T1: engine_invariant below base() or at/after size() | L8a |
| `TradeLedger::at(size_type global) const` | Amendment T1: the same checked bounds and const result | L8a |
| `TradeLedger::push_back(const Trade&)`, `push_back(Trade&&)` | 3.4:613-616 | L8a |
| `TradeLedger::resize(size_type)` | 3.4:614,664-665 | L8a |
| `TradeLedger::begin()/end()` (mutable/const) | 3.4:615 | L8a |
| `TradeLedger::seal(size_type)` and its caller's five-condition proof / fold-write-spill-before-drop contract | 3.4:622-688 | L8b/L8c/L18 |

The total-count versus resident-window distinction and closedtrades.*
retention exception are binding doc contracts (3.4:613-620,690-711).
`set_trade_history_streamable` and its feature probe remain the engine/codegen
lanes' work; P00 neither advertises them nor changes `trades_`.

References and iterators invalidate as a `std::vector`'s (amendment T1).

## Metric accumulator and point sources

| File / declaration | DESIGN section and lines | Implementing lane |
|---|---|---|
| `include/pineforge/metrics_stream.hpp`: 24-byte `pf_equity_point_t` layout assertions | 3.3:541-543; 3.8:906-907 | L6b-4/L7c |
| `metrics::TradeStatsAccumulator` constructor/destructor, copy declarations, opaque `State` | 3.3:518-529,547-553 | L7a |
| `TradeStatsAccumulator::fold(const TradeC&)`, `finish() const` | 3.3:529,547-551 | L7a |
| `metrics::ArrayCurve` constructor, `size() const`, `operator[](int64_t) const` | 3.3:535-543 | L7b |
| `metrics::ScratchCurveRegion` | 3.8:902-909; 3.3:537,541-543 | L6b-4 |
| `metrics::SpillCurve` constructor/destructor, `size() const`, `operator[](int64_t) const`, opaque `State` | 3.3:535-546; 3.8:904-909 | L6b-4/L7c |
| `metrics::compute_equity_stats<PointSource>(...)` (ordinary denominator) | 3.3:531-546 | L7b/L7c |
| `metrics::compute_equity_stats<PointSource>(...,observation_count)` | 3.3:531-546; 3.6:745-749,769-773 | L7b/L7c/L17b |

Only the named accumulator/point-source operations are frozen here. The
month/per-bar generators are the metric implementation's internal walks;
their unprescribed helper names and storage are not made a second API.
Scratch curve/position-tail placement remains either regions or separate
private files, as DESIGN permits. No mapped scratch or text-based metrics.

## Bar and close views

| File / declaration | DESIGN section and lines | Implementing lane |
|---|---|---|
| `include/pineforge/native_bar_views.hpp`: `inline namespace native_bar_views_v1` | 2.2:339-347; 6:1328-1329 | L15a |
| `NativeBarView { const Bar* data; std::size_t size; }` | 2.2:348 | L15a |
| `NativeInstrumentFeedView { NativeBarView bars; const int64_t* close_ms; }` | 2.2:349 | L15a/L15b |
| `class NativeLowerPathSource;` (forward declaration only; OPEN V2) | 2.2:350-351; 5:1237-1262 | L15a/L16 |

L15a implements staging and the common accessor, with no caller. It also adds
the new header/source and namespace check to the native ownership guard
(2.2:352-378). Neither the existing v3 run-spec nor native-host declarations
change here. The borrowed-feed C export/probe belongs to L15b (6:1318-1329).

### L15a read inventory

This is DESIGN's required source-guard read set (2.2:357-371; 5:1221-1228),
including the indirect count/hash consumers. Each source range below names
a symbol in that window. The DESIGN column preserves the revision-6 inventory
references; L15a must maintain these anchors and mechanically pin every current
`.bars`/`.close_ms` read when it installs the accessor/guard.

| Reader to route through the view/source | Source anchors in this tree | DESIGN lines |
|---|---|---|
| Kernel instrument-feed pump: current bar, close, next open, count | `NativeExecutionConsumer::pump_instrument_subscription` `src/native_execution_consumer.cpp:9932-9983` | 357-358 |
| Host feed lookup/delivery and column reads | `installed_symbol_feed` `src/source/pine_security_eval.cpp:1059-1068`; `installed_symbol_feed` `src/source/pine_security_eval.cpp:1286-1293`; `installed_symbol_feed` `src/source/pine_security_eval.cpp:1312-1320` | 359-360 |
| Feed validation: bar values/order, close length/bounds, column length; configure and consumer entry points | `feed.bars` `src/native_run_spec.cpp:254-276`; `instrument_feed_shapes` `src/native_run_spec.cpp:730`; `validate_native_instrument_feeds` `src/native_run_spec.cpp:995-998`; `validate_native_timeframe_subscriptions` `src/native_execution_consumer.cpp:9468-9471`; `validate_native_timeframe_subscriptions` `src/native_execution_consumer.cpp:9493-9495` | 361-363 |
| Instrument-feed digest and cached/uncached continuation folds | `native_instrument_feeds_digest` `src/native_run_spec.cpp:1124-1160`; `native_instrument_feeds_digest` `src/native_execution_consumer.cpp:321-323`; `native_instrument_feeds_digest` `src/native_execution_consumer.cpp:1935-1936`; `native_run_spec_digest` `src/native_execution_consumer.cpp:12828-12833` | 364-365 |
| Staged feed counts in host state and its hash | `installed_symbol_feed` `src/source/pine_strategy_host.cpp:230-234`; `staged_.instrument_feeds` `src/source/pine_state_hash.cpp:292-299` | 366-367 |
| Refresh of symbol-data digest and host hash fold | `refresh_symbol_data_digest` `src/source/pine_security_eval.cpp:1536-1544`; `symbol_data_digest_` `src/source/pine_state_hash.cpp:1123-1129` | 367-368 |
| Restore/store upkeep and column-setter length check | `held.bars` `src/source/pine_security_eval.cpp:1091-1096`; `feed.bars` `src/source/pine_security_eval.cpp:1420-1422` | 369-370 |
| Lower-path preflight/validation | `NativeExecutionConsumer::preflight_intrabar_path` `src/native_execution_consumer.cpp:3148-3181` | 371,1221-1222 |
| Lower-path digest (count first) and continuation hash | `native_intrabar_path_digest` `src/native_run_spec.cpp:867-895`; `native_intrabar_path_digest` `src/native_execution_consumer.cpp:288` | 371,1223-1224 |
| Per-script-bar lower-path binary search/walk | `candidate` `src/native_execution_consumer.cpp:8562-8576` | 371,1225 |
| Scheduler sub-bar lookup and one-bar look-ahead | `PineScheduler::lower_path_bar_at` `src/source/pine_scheduler_native.cpp:252-264`; `next_open` `src/source/pine_scheduler_native.cpp:296-298` | 371,1226-1227 |
| Lower-path emptiness test | `inactive_sampler_path` `src/source/pine_adapter.cpp:3420-3422` | 371,1228 |

Every count/digest must fold the same bytes/order for copied and borrowed
feeds, including cached and uncached paths (2.2:372-375). Views replace storage;
metadata, validation and continuation identity remain the existing contract.

## Contract item status

| ID | Status | Contract / remaining gap | Owner |
|---|---|---|---|
| S1 | RESOLVED | Amendment S1: caller sets struct_size and version; 0 with the receipt filled; -1 with *out unchanged for a bad handle, struct_size or version. No sink installed: 0, counts 0, sha256_hex "", complete 0. | L6b-1 |
| S2 | OPEN | Internal writer error transport, oversized-line/unsafe-integer failure details, and what a repeated fill/capture does after a trailer; DESIGN requires repeatable metric reads but gives no writer reset/idempotence contract (3.3:550-551; 3.7:790,881-888). | CORE-TOP / L6a/L6b-1 |
| T1 | RESOLVED | Amendment T1: clear() resets size/base to 0; reserve(size_type) is a hint; both at(size_type global) forms throw engine_invariant below base() or at/after size(); empty() const means size() == 0. References and iterators invalidate as a std::vector's. | L8a |
| M1 | OPEN | Point-source malformed-region/bounds/short-read error transport and concurrent-reader rules are not specified; the 24-byte binary points, 64 KiB pread window and repeated forward walks ARE fixed (3.3:535-546; 3.8:902-909). | CORE-TOP / L6b-4/L7c |
| V1 | OPEN | Typed staging/accessor signatures, feed binding/lookup and absent-versus-present-empty view representation are not specified. DESIGN names stage_instrument_feed_views / stage_lower_path_source but no prototypes; none are invented (2.2:352-356). | CORE-TOP / L15a |
| V2 | OPEN | NativeLowerPathSource operation return/parameter types, visitor protocol and dispatch/error model are not specified. Only its forward declaration and required behavior are frozen; typed members await a ruling (2.2:350-351; 5:1237-1262). | CORE-TOP / L15a/L16 |

S1's model is the observation query, `strategy_execution_observation_v1`
`include/pineforge/execution_observer.h:134-141`.

## Lab handoff

CORE-TOP runs the headers' compile proof, `scripts/check_c_abi_runtime.py`,
and `scripts/check_native_cpp_versions.py` on the lab queue. P00 supplies no
local test or guard verdict. Repository-wide unit/corpus and merge-gate proof
remain required before integration is declared verified.
