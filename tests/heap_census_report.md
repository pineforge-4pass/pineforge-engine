# CR-150 L0B heap census handoff

**OPEN LAB — source inventory and measurement harness checked in; no measured
census or bytes-per-id verdict is claimed.** CORE-TOP forbids building, compiling
or executing tests/profilers on this Mac. The numeric Q9 cells below remain OPEN
until CORE-TOP attaches the Linux lab artifacts. This document is not the
completeness proof required by acceptance (k).

Runtime/source basis: `ee3083d4a23fdae23d2cfa6ac254a8066de1b780`, branch
`core/cr150-l0b`. DESIGN revision 6 describes `3fc010a8b`; its quoted measurements
come from F39 box 6 at 1.6.0-rc.1, not from a run of this lane. The dispatch
authority is the L0b row of PARALLEL-PLAN section 3, DESIGN sections 2.1, 3.9,
3.10 and 7(k), Q9, and REVISION's amendments through 10-18 19:50. L19l remains
unconditional. The 18:21 scope ruling assigns the L0 acceptance harnesses to
integration; this lane does not modify them.

## Allocation-site ownership inventory

Every row is a source-identified term or an explicitly attributed prior
measurement, not a newly measured allocation site. The lab driver reports
every positive leaf-stack delta, its before/after bytes, B/bar or B/trade, and
an owner. Unknown, ambiguous and unsymbolized sites go to **L19j**, remain
non-admitted and must be resolved before acceptance. A known owner is an
assignment, never a waiver of growth.

| Site/container | Size/unit or existing evidence | Lane | Fixture/path |
|---|---|---|---|
| Caller chart OHLCV | 48 B/input bar; the only admitted native chart allocation | Caller; L2 maps it in run_json | All native cases; corpus caller tapes are mapped |
| Caller foreign close times | 8 B/feed bar, part of the admitted feed tape | Caller; L2 | `foreign_security`; shared chart/feed OHLCV bytes |
| Scheduler `RetainedBegin::bars`, native module input copy, stream warmup temporary | 48 B/input bar per copy, DESIGN 2.1 | L3 | Native battery; generated corpus modules |
| Scheduler completion and source-boundary arrays | 1 + 1 B/input bar | L4 | Bar ladder, both input/script splits |
| `equity_curve_`, final C report curve | 24 B/script point per copy; F39's combined 63.5 B/bar is separately flagged in DESIGN | L6b-1/L6b-2 | All bar ladders; C report copy is present in corpus baseline only |
| Month-end/monthly-return/per-bar-return metric scratch | About 16 B/month plus 8 B/script bar; report-time transient | L7b/L7c | Corpus metric phase; transient peak remains OPEN/L0 |
| `trades_` | 240 B/row nominal, 314.6 B/trade at F39's 1.31072 capacity ratio | L8a/L8c | Fixed 40,000 bars, 10 vs 10,000 trades |
| `closed_row_prefix_`, `trade_exit_phase_` | 8 + 1 B/row nominal; 10.5 + 1.3 at that capacity ratio | L8a/L8c | Same trade ladder |
| C report `TradeC` and presentation copies | 104 B/row plus selected/observed curve and trade copies | L9a/L9s/L17a | Corpus callback before report destruction; selected acceptance is L0 at integration |
| Rows closed inside a still-open position | Trade rows and percentage-exit reader tail | L18 | `partial_exits`; long scale-out acceptance belongs to L0 |
| `successor_roots_` | 16 B/replace successor before capacity slack | L19a | Exit repricing every bar |
| Group-effect `receipts_` | One `ReceiptKey` per group effect; platform size must be measured | L19b | OCA bracket pairs |
| `cohort_receipts_` | 40 B/receipt nominal; 2 x 40 x 1.31072 = 104.9 B/round trip in F39's inventory | L19c | Trade ladder, pyramiding |
| `OriginRoster::items_`, `positions_` and their inner vectors/buckets | 24 B/handle plus maps/vectors; about 119.5 B/trade in the prior structural inventory | L19d | Reused id vs unique id, recurring-id unit control |
| Admission `Journal::events_`, shared `CommandObservation`, review and sizing payloads (F0a) | Prior structural estimate ~480 B observation + 288 B event slot x capacity = ~857 B/admitted entry | L19e | All entry/order cases, recording OFF; review/margin variants also require L19e's own tests |
| Placement rows pinned by K5 (F0b) | About 1.3 KB/placed entry | L19f | `split_magnifier`, `split_no_magnifier` |
| Placement rows pinned by K1 (F0c) | About 1.3 KB/reissued leg while its origin stays open | L19g | `moved_exits`; margin-call parity fixture remains L19g's proof |
| `PlacementTable::unfolded_` (F0d) | 8 B/request nominal; 21.0 B/round trip at the prior capacity ratio | L19h | Market trade ladder, exit repricing |
| `BracketRoster::settled_` (F0d) | 1–2 B/settled leg/family plus capacity | L19h | OCA pairs and partial exits |
| `input_next_ms_` (F1) | 8 B/input bar with a subscription | L19i, unless L3/L4 takes it | `routed_security`, `foreign_security` |
| N-day stamps and native-group opens (F2) | 16 B/N trading days/site, not per trading day; prepare also has a per-day map transient | L19k if the measured slope exists | `nday_security`, 3D over daily bars; prepare transient remains OPEN |
| Adapter `cohorts_by_id_`, `cohort_order_` (F3) | One map node plus one string-vector slot per distinct id; bytes/id OPEN | L19l | Short/long unique-entry populations and reused controls |
| Entry-id rank table `exit_rank_keys_` (F3) | Source read below: at most 27 recorded keys at this base | L19l contract; bound reported for CORE-TOP | 10 vs 10,000 distinct ids, recurring 31-id unit control |
| Kernel `cohorts_`, each roster's retained vector capacity (F3) | One 32 B roster slot nominal per distinct id plus member capacity; bytes/id OPEN | L19l | Unique-id control; empty roster lifetime is not the same as empty members |
| `PlacementTable::sequence_index_`, `erased_minima_`, nested `by_from_entry` (F3) | Per (id, from_entry), maps/string storage; bytes/id OPEN | L19l | Unique entry ids and derived `strategy.close` labels |
| `consumed_partial_exit_cycles_` and per-exit-id auxiliaries (F3) | Per (exit id, from_entry); population/bytes require the profiler | L19l | Partial exits and corpus; per-trade exit-id coverage remains OPEN if absent from corpus |
| Foreign symbol feed bars and close-time copies | 56 B/feed bar nominal; F39 engine copy ~57 B/feed bar | L15a/L15b | `foreign_security`; borrowed setter discovered when implemented |
| Finer-input magnifier path and preparation temporaries | Up to 48 B/path bar; preparation has additional vectors | L16 | `split_magnifier`; transient peak is L0's acceptance |
| Recorded outputs, trace and hashes when enabled | Per output value/record; hash array 8 B/script bar | L6b-3 | Corpus declarations; dedicated enable/trace/hash acceptance is L0/integration |
| run_json CSV tuples, arrays and report dictionaries | F39: 298.6 B/chart CSV row, 395.5 B/feed row, 288.8 B/equity dict, 922.2 B/trade dict | L2/L9a/L9s | L0 RSS harness at integration; this census samples retained native/report state |
| Any additional growing allocation stack | No size assumed | L19j | Full battery and every source-marked corpus validation probe |

Bounded and non-production terms are not silently admitted. `range_end_trades_`
and pyramid lots are bounded by current open lots; issued ranges, repriced rows
and the Window command journal have existing bounds. Native/auxiliary security
copies, opt-in historical projections and Full driver logs are recorded as
non-production in DESIGN 2.4/3.9. If any appears as a growing stack in the
production battery/corpus, the driver reports it; it does not exempt a whole
file or class name.

Relevant declarations at this base: `trades_` in include/pineforge/engine.hpp:546;
`equity_curve_` in include/pineforge/engine.hpp:586; `closed_row_prefix_` in
src/native_execution_consumer.hpp:1914; `events_` in
include/pineforge/source/market_admission.hpp:213; `OriginRoster` in
include/pineforge/source/pine_adapter.hpp:935; the per-id cohort members in
include/pineforge/source/pine_adapter.hpp:2518 and :2519; `trade_exit_phase_` in
include/pineforge/source/pine_adapter.hpp:2734; `exit_rank_keys_` in
include/pineforge/source/pine_adapter.hpp:2740; `cohorts_` in
include/pineforge/native_order.hpp:2318; N-day stamp arrays in
include/pineforge/timeframe.hpp:555 and :556.

## Q9 bytes per additional distinct entry id

The measured result is **OPEN**, not 0 and not the prior 1,323.4 B/trade. That
F39 number is an engine trade slope for a different population. It cannot be
relabelled as bytes/id. L19l is required regardless of the result.

The fixture uses the same 40,000 one-minute bars and the same trade schedule in
each pair. Each trade opens one unit and closes it on the next script bar.
`unique_ids` uses a new entry id on every opening; `reused_id` uses one id.
Both use 14 zero-padded digits, so decimal transitions do not change id length.
The short population has 15-byte ids; the long population has 27-byte ids.
No trade-history accessor runs in the strategy body. The ordinary unit also
revisits ids after 31 openings, testing recurrence rather than assuming that an
old id can be forgotten. The Pine fixture expresses the same unique/reused rule;
the CMake-wired native fixture is what the profiler executes.

| ID length | Low pair | High pair | Incremental distinct ids | Measured B/additional id |
|---:|---|---|---:|---|
| 15 B | 10 trades, 1 vs 10 ids | 10,000 trades, 1 vs 10,000 ids | 9,990 | OPEN LAB |
| 27 B | 10 trades, 1 vs 10 ids | 10,000 trades, 1 vs 10,000 ids | 9,990 | OPEN LAB |

For every leaf stack s, use the signed difference of differences:

```text
q9(s) = ((unique_10000(s) - reused_10000(s))
       - (unique_10(s)    - reused_10(s))) / 9990
```

The aggregate uses total live heap bytes with the same formula. Negative
site costs remain in the output because distributing openings among ids can
change vector capacity and bucket allocation. Ordinary trade-history work is
controlled by equal trades, bars and id lengths, rather than charged to Q9.
The driver reports both aggregate and per-site B/id in `census.json`, with the
four raw snapshots and population receipts. Massif requested bytes do not
include allocator chunk overhead; they are not RSS measurements.

Source-only lower-level sizes are useful for orientation but not a measured
total: a libstdc++ `std::string` slot is 32 B, a kernel `CohortRoster` is an
8-byte handle plus a 24-byte vector, and a `RequestHandle` is 24 B. Map nodes,
bucket arrays, nested vectors, capacity slack and long-string allocations
still have to be counted on the lab. The table inventory deliberately does
not add a fabricated B/id total.

One source-level qualification: `note_exit_rank_key` in
src/source/pine_adapter.cpp:6331 returns once the table exceeds 26 keys. The
check is at :6334 and the append at :6339, so this particular table saturates
at 27 at the brief's base. DESIGN's F3 inventory groups it with per-distinct-id
tables. This lane changes neither the bound nor the contract; CORE-TOP/L19l
should retain the observation when attributing the early-rung cost.

## Lab procedure and artifacts

Do not overlap the census with `scripts/run_corpus.sh` or another producer of
the derived feeds/build library. CORE-TOP first prepares derived feeds once
to freshness and completes an all-target, symbolized Release/RelWithDebInfo
build, including corpus modules. The driver performs no build or derivation.
It refuses a library older than any working-tree source/header or root CMake
file, and refuses test/module binaries older than that static library.

Run the unit/guard/full verification commands prescribed by AGENTS.md on the
lab, including the previous-epoch compile diagnostic before trusting the new
C++ row. Then profile this lane's base runtime with the harness added and
the integrated runtime separately:

```bash
python3 tests/heap_census.py --build-dir build \
  --battery tests/fixtures/flat_memory --corpus corpus \
  --output build/heap-census-l0b-baseline
```

The output directory must be new. It preserves `command.json`, live checkpoint
`checkpoint.massif`, automatic Massif output, profiler/monitor/stdout/stderr
logs, sink and caller-tape files, `measurement.json`, `census.json`, and the
generated `CENSUS.md`. Its source HEAD, module/library hashes, measured bars,
trades, ids, id byte lengths, sink and trade-policy state bind the evidence.
Exit 1 means observed non-admitted growth with complete requested coverage;
exit 2 means OPEN/incomplete proof (including a battery-only run). Exit 0
requires the battery and nonempty corpus, increasing measured populations,
no non-admitted growing site and implemented sink/history-policy evidence.

The native checkpoint precedes both host destruction and report allocation.
The corpus checkpoint wraps `Strategy.run`'s live `on_report` seam before
report/state destruction and before Python report conversion. Its baseline
includes the C report copy, which is recorded. Caller OHLCV arrays are mapped
from byte-identical packed records in the output directory so the malloc
census does not misclassify the one permitted input tape. Engine-owned copies
still show up as allocations. The real corpus CLI supplies settings, requests
and magnifier routing, with N/4N chart prefixes and feed end bounds. Short
prefixes and missing modules fail explicitly rather than skipping probes.

P00 declares the sink but does not implement it. The native fixture discovers
implemented exports at runtime and uses the old retaining path when absent;
there is no red result caused solely by requesting a missing sink flag.
The expected base red reason is the live growth in the inventory, especially
the admission journal, origin/cohort state, trades and chart/curve copies.
After integration, the fixture detects the streamable policy method without
a preprocessor feature branch and validates streamed ids from T records.

## Remaining proof

- Execute the new C++ row, Python controls, previous-epoch compile, preflight,
  full release verification and full corpus sweep on the lab. No local test or
  guard summary/log exists; only a source/whitespace review was performed here.
- Execute the profiler on the base and integrated exact trees, attach the raw
  allocation stacks, fill both Q9 numeric cells, and check in the measured
  census/JSON evidence. No fresh growing allocation site has been measured by
  this lane; completeness is still OPEN.
- Confirm the lab's Valgrind monitor handshake and the optional sink/policy
  discovery against the actual implementing lanes. A failure is OPEN, not a
  memory PASS. Symbols need debug information for useful attribution.
- L0/integration must still cover selected windows, long scale-out, enabled
  outputs/trace/hashes, binary/CSV loading and transient prepare/report peaks.
  This retained-heap driver does not replace those acceptance harnesses.
- The base has a per-ID rank-table bound as noted above; CORE-TOP owns its
  reconciliation with the F3 wording. L19l is not gated on this observation.
- OPEN FROZEN: none encountered. No frozen file, P00 declaration, measured
  runtime source, source/header ABI list or existing fixture/helper was edited.
