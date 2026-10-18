# CR-150 P00 declaration contracts

DESIGN coordinate basis: `core-cr150-design/work/DESIGN.md`, revision 6 text
identified by the P00-7 gating read's SHA-256:
`68bda6e37931c096522a348603623c7f3cba2b1371a9c0a442edbf53d25ab0ad`.
DESIGN line citations below use that pinned text; the amendments below take
precedence. Dispatch is the external design lane's `PARALLEL-PLAN.md`,
lines 27–30 and 63. C++ declarations spell
the operations described by DESIGN; OPEN items below preserve its unspecified
policies.

CORE-TOP's rev 6 amendment (10-18 16:04) in the external design lane's
`core-cr150-design/work/REVISION.md`, lines 342–348, resolves S1 and T1.
References to amendment S1 or T1 below name that ruling, which binds L6b-1
and L8a respectively.

CORE-TOP's rev 6 amendment recorded at 10-18 19:24 in the same external
`core-cr150-design/work/REVISION.md`, lines 351–358, resolves V1 and V2. It
adopts `core-cr150-l15a/PROPOSAL-V1-V2.md`, SHA-256
`b17ff9f2dab20d876df2e3148ca1c2aba2b5d78f7ccf8c4f026643cfdb14674e`,
with required changes 1–9 of `core-cr150-read-v1v2/REVIEW.md`. That ruling
also approves eager provider/feed component digests at configure, superseding
the lazy timing in DESIGN 5:1243-1246. The review notes that the later 19:11
converter amendment shifts live DESIGN coordinates at/after 1173 by +3;
the pinned coordinate basis here remains explicit.

The freeze adds declarations, value-type defaults and the provider's approved
defaulted virtual destructor. Existing execution paths, feature probes,
versions and caller-visible layouts stay unchanged. Operation definitions
and lab proof belong to CORE-TOP and the implementation lanes. Nothing is
built, compiled or tested on the P00 machine.

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

| File / declaration | DESIGN / V1-V2 amendment authority | Implementing lane |
|---|---|---|
| `include/pineforge/native_bar_views.hpp`: `inline namespace native_bar_views_v1` | 2.2:339-347; 6:1328-1329 | L15a |
| `NativeBarView { const Bar* data; std::size_t size; }` | 2.2:348 | L15a |
| `NativeInstrumentFeedView { NativeBarView bars; const int64_t* close_ms; }` | 2.2:349 | L15a/L15b |
| `NativeInstrumentFeedReadView { NativeBarView bars; const int64_t* close_ms; size_t close_count; }` | 2.2:354-375; proposal V1 | L15a |
| `NativeBarViews` with nullable indexed feed slots, const provider and current segment | 2.2:352-375; 5:1250-1259; changes 1-4 | L15a |
| `native_instrument_feed_view(owned,borrowed) noexcept` (no default argument) | 2.2:355-375; change 3 | L15a |
| `normalize_native_run_spec(spec,views) noexcept` | 2.2:361-363; change 5 also covers projection | L15a |
| `validate_native_instrument_feeds(feeds,views) noexcept` | 2.2:361-363; proposal V1 | L15a/L15b |
| `validate_native_timeframe_subscriptions(...,views) noexcept` | 2.2:361-363; proposal V1 | L15a |
| `native_instrument_feeds_digest(feeds,views) noexcept` | 2.2:364-375; proposal V1 | L15a |
| `native_run_spec_digest(spec,views)` (may throw) | 2.2:364-375; eager-digest amendment / change 8 | L15a |
| `class NativeLowerPathSource` (forward declaration retained and class completed) | 2.2:350-351; 5:1237-1262; proposal V2 as amended | L15a/L16 |
| `NativeLowerPathSource::Visitor = bool (*)(void*,const Bar&) noexcept` | 5:1241-1243; proposal V2 | L16 |
| `virtual ~NativeLowerPathSource() noexcept = default` | Proposal V2: exclusive consumer ownership / base deletion | L15a/L16 |
| `virtual size_t NativeLowerPathSource::count() const = 0` | 5:1241-1247; change 7: lifetime count, O(1) after first call | L16 |
| `virtual bool NativeLowerPathSource::for_each(Visitor,void*) const = 0` | 5:1241-1247; proposal V2 | L16 |
| `virtual NativeBarView NativeLowerPathSource::segment(int64_t,int64_t) = 0` | 5:1250-1257; change 6: repeated/overlapping windows | L16 |
| `virtual const Bar* NativeLowerPathSource::next_after_segment() const noexcept = 0` | 5:1251-1257; proposal V2 | L16 |
| `native_intrabar_path_digest(path,source)` (may throw) | 5:1241-1247; proposal V2 / change 8 | L15a/L16 |

Staging and the active query are public members of the private
`NativeExecutionConsumer` in `src/native_execution_consumer.hpp` (change 1):

| Member declaration | Frozen contract | Implementing lane |
|---|---|---|
| `void stage_instrument_feed_views(const NativeInstrumentFeedView* const*,size_t)` | Copy pending descriptors; no lifecycle judgement or active mutation | L15a |
| `void stage_lower_path_source(unique_ptr<NativeLowerPathSource>) noexcept` | Transfer exclusive ownership to pending; null clears pending source | L15a |
| `NativeBarViews bar_views() const noexcept` | Allocation-free active binding only; pending is invisible | L15a |

L15a implements staging and the common accessor, with no caller. It also adds
the new header/source and namespace check to the native ownership guard
(2.2:352-378). Neither the existing v3 run-spec nor native-host declarations
change here. The borrowed-feed C export/probe belongs to L15b (6:1318-1329).

### V1/V2 binding and provider rules

The two original view structs stay byte-for-byte the same. A zero context
feed count selects all owned vectors. Otherwise the count matches metadata
entries by index; a null slot selects its owned feed, while a nonnull slot
always selects borrowed data, including a present empty feed. The read type
preserves the independent owned close count, so validation still detects
length mismatches and the feed digest still folds zero for a missing close.
Borrowed slots require empty owning bar/close vectors, with metadata and
columns retained. Descriptors/table are copied; immutable arrays are borrowed
through every accepted-binding use. Binding adds no pointer registry or
duplicate key/timeframe strings.

Staging changes pending only. Descriptor allocation throws `std::bad_alloc`
with the previous pending slot preserved; a nonzero count with a null table
throws coded `engine_invariant`. Only an applied configure binds and judges
the pending descriptors/provider against candidate metadata. Refused configure
or unwound preparation discards pending; no Ready restaging. Binding checks
keep the proposal's typed bar/close/metadata refusals and existing validation
order. `bar_views()` exposes active Ready/Running state, never pending, and
returns an empty context when unbound or detached. A detached Ready spec
cannot be admitted, and a provider bound at stream begin is `engine_invariant`.
Run exit, including error/abort, detaches borrowed pointers after retaining
scalar counts/content digests. The provider destructor never reads its input.

The visitor is synchronous, ordered and nonthrowing; false stops normally.
Whole walks do not overwrite the active segment or separate look-ahead.
Count is fixed for provider life: the first call may walk or use a pre-scan,
later calls return its cached scalar in O(1). Segment bounds are individually
non-decreasing; repeated or overlapping windows return every row in the
half-open interval, including overlap rows. Equal bounds give empty data;
reversed/decreasing bounds are coded `engine_invariant` / `std::logic_error`.
Repeats keep the same storage. A different segment replaces storage only
after its script bar finishes, preparing look-ahead before exposing it.
Look-ahead is the first row at/after the end, even for an empty segment; its
separate buffer stays fixed until a different segment and queries do not
advance. The const context prevents scheduler callbacks selecting segments.

DESIGN 5 eager-digest amendment: after normalization succeeds and before
configure's first member write, compute immutable feed/provider component
digests. Lower-walk exceptions propagate unchanged and nothing is published;
they are not mapped to validation refusals. Keep components outside combined
cache invalidation and through detachment so the noexcept continuation query
never walks freed input. Standalone context digest overloads may walk/throw.
Folded bytes/order stay the same: feed NaNs canonicalize as before, while the
lower-path digest preserves raw double bits. Null source uses the vector
digest; nonnull source is present even if empty. No failure becomes empty
data, a fallback or a sentinel hash. Unexpected visitor stop/count mismatch
is an invariant error. Runtime segment failures retain the existing intrabar
option rejection; allocation exceptions use the run-entry handler.

All nine required changes are frozen: (1) consumer staging/query, (2) pending
only and configure binding, (3) explicit accessor argument, (4) const source,
(5) projection validation routing below, (6) overlapping windows, (7) cached
count, (8) propagating eager-digest failures, and (9) RESOLVED V1/V2 text.
The implementations and source guards remain L15a/L15b/L16's work.

### L15a read inventory

This is DESIGN's required source-guard read set (2.2:357-371; 5:1221-1228),
including the indirect count/hash consumers. Each source range below names
a symbol in that window. The DESIGN column preserves the revision-6 inventory
references; L15a must maintain these anchors and mechanically pin every current
`.bars`/`.close_ms` read when it installs the accessor/guard.

| Reader to route through the view/source | Source anchors in this tree | DESIGN lines |
|---|---|---|
| Kernel instrument-feed pump: current bar, close, next open, count | `NativeExecutionConsumer::pump_instrument_subscription` `src/native_execution_consumer.cpp:10063-10118` | 357-358 |
| Host feed lookup/delivery and column reads | `installed_symbol_feed` `src/source/pine_security_eval.cpp:1059-1068`; `installed_symbol_feed` `src/source/pine_security_eval.cpp:1291-1301`; `installed_symbol_feed` `src/source/pine_security_eval.cpp:1320-1328` | 359-360 |
| Feed validation: bar values/order, close length/bounds, column length; configure and consumer entry points | `feed.bars` `src/native_run_spec.cpp:259-290`; `instrument_feed_shapes` `src/native_run_spec.cpp:743`; `validate_native_instrument_feeds` `src/native_run_spec.cpp:1019-1022`; `validate_native_timeframe_subscriptions` `src/native_execution_consumer.cpp:9599-9602`; `validate_native_timeframe_subscriptions` `src/native_execution_consumer.cpp:9624-9626` | 361-363 |
| Adapter projection validation: use the context normalizer; host stages after lending and before projection, using a context built from its store | `normalize_projected_spec` `src/source/pine_adapter.cpp:3004-3010` | 361-363; V1/V2 change 5 |
| Instrument-feed digest and cached/uncached continuation folds | `digest_instrument_feed_views` `src/native_run_spec.cpp:1162-1205`; `native_instrument_feeds_digest` `src/native_execution_consumer.cpp:322-324`; `native_instrument_feeds_digest` `src/native_execution_consumer.cpp:2775-2776`; `native_run_spec_digest` `src/native_execution_consumer.cpp:12870-12875` | 364-365 |
| Staged feed counts in host state and its hash | `installed_symbol_feed` `src/source/pine_strategy_host.cpp:234-242`; `staged_.instrument_feeds` `src/source/pine_state_hash.cpp:292-299` | 366-367 |
| Refresh of symbol-data digest and host hash fold | `refresh_symbol_data_digest` `src/source/pine_security_eval.cpp:1545-1553`; `symbol_data_digest_` `src/source/pine_state_hash.cpp:1135-1141` | 367-368 |
| Restore/store upkeep and column-setter length check | `held.bars` `src/source/pine_security_eval.cpp:1091-1101`; `data.bars` `src/source/pine_security_eval.cpp:1428-1431` | 369-370 |
| Lower-path preflight/validation | `NativeExecutionConsumer::preflight_intrabar_path` `src/native_execution_consumer.cpp:3217-3293` | 371,1221-1222 |
| Lower-path digest (count first) and continuation hash | `native_intrabar_path_digest` `src/native_run_spec.cpp:891-919`; `native_intrabar_path_digest` `src/native_execution_consumer.cpp:289` | 371,1223-1224 |
| Per-script-bar lower-path binary search/walk | `candidate` `src/native_execution_consumer.cpp:8674-8707` | 371,1225 |
| Scheduler sub-bar lookup and one-bar look-ahead | `PineScheduler::lower_path_bar_at` `src/source/pine_scheduler_native.cpp:263-285`; `next_open` `src/source/pine_scheduler_native.cpp:326-327` | 371,1226-1227 |
| Lower-path emptiness test | `inactive_sampler_path` `src/source/pine_adapter.cpp:3429-3433` | 371,1228 |

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
| V1 | RESOLVED | CORE-TOP 19:24 amendment: consumer pending-only staging, active query, indexed nullable slots, explicit accessor and context validation/digest overloads, as amended by required changes 1-5 and 8-9. | L15a/L15b |
| V2 | RESOLVED | CORE-TOP 19:24 amendment: four-operation virtual provider with nonthrowing visitor, const read context, cached count, overlapping/repeated segments, separate look-ahead and exclusive consumer ownership; eager configure component digests propagate failures (changes 1-2,4,6-9). | L15a/L16 |

S1's model is the observation query, `strategy_execution_observation_v1`
`include/pineforge/execution_observer.h:134-141`.

## Lab handoff

CORE-TOP runs the headers' compile proof, `scripts/check_c_abi_runtime.py`,
and `scripts/check_native_cpp_versions.py` on the lab queue. P00 supplies no
local test or guard verdict. Repository-wide unit/corpus and merge-gate proof
remain required before integration is declared verified.
