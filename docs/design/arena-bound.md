# Pre-start arena and invocation bounds

This is the CR-150 sizing interface for COMPUTE and APP. Its implementation is
`scripts/arena_bound.py`; its hand-derived cross-language vectors are
`tests/data/arena_bound_vectors.json`. All sizes are bytes, all arithmetic is
integer arithmetic, and no completed-run count is an input.

**Status:** arithmetic, facts interface, source inventory and lab harness
authored; not executed or certified on the corpus. A trusted compiler/lowering
FACTS producer is still required for general Pine strategies. The engine does
not currently emit that record. The local corpus checkout is empty. See
[the census](arena-bound-corpus-census.md) for the exact known population and
the distinction between unbounded and unclassified.
Today no run has the facts needed for certified tightening; the bound
tightens nothing yet.

## One function and its exact result

The latest A5-WIDER interface supersedes the earlier arena-only return:

```text
arena_bound(facts) -> {
  terms: {
    arena: {
      arenaBytes: integer | null,
      bytes: integer | null,             # identical alias of arenaBytes
      inputs: {
        T: {value: integer | null, derivation: {formula, sources}},
        D: {value: integer | null, derivation: {formula, sources}},
        Q: {value: integer | null, derivation: {formula, sources}},
        L: {value: integer | null, derivation: {formula, sources}},
        R: {value: integer | null, derivation: {formula, sources}},
        E: {value: integer | null, derivation: {formula, sources}}
      },
      unbounded: boolean,
      derivation: {formula, sources},
      reason: string                    # present only when arena is unbounded
    },
    ohlcv: {
      residentBytes: integer | null, asBytes: integer | null,
      unbounded: boolean, derivation: {formula, sources}
    },
    finalizerStream: {
      bytes: integer | null, unbounded: boolean,
      derivation: {formula, sources, recordCounts?, maxLineBytes?}
    },
    childPrivate: {
      rssBytes: integer | null, asBytes: integer | null,
      measured: boolean, unbounded: boolean,
      derivation: {formula, sources, rssCertified?, asCertified?}
    }
  },
  unbounded: boolean,
  unboundedTerms: ["arena"?, "ohlcv"?, "finalizerStream"?],
  reason: string
}
```

`unboundedTerms` uses the displayed order and contains only the sizing terms
arena, ohlcv and finalizerStream whose own flags are true. The top-level
`unbounded` is true exactly when that list is nonempty; `reason` is empty
when all three sizing terms have certified finite values.
`terms.childPrivate.unbounded` is computed and reported independently as an
informational flag. It never enters `unboundedTerms`, the top-level verdict
or its reason, even when private RSS/AS is unknown.
The arena retains A5's `arenaBytes` and `inputs` names inside `terms.arena`;
`bytes` is an equal alias to accommodate the wider interface's spelling.
Each derivation names the formula and the pre-start evidence supplied with
its facts. `per_user_bytes(result["terms"]["arena"]["inputs"])` exposes the
eleven logical contributions without adding a second sizing result.

The flag means **no certified finite value available for this term**. Reasons
distinguish `UNBOUNDED` (a proved unbounded emitting computation),
`UNCERTIFIED` (an unsupported or absent proof), and `PLATFORM_RANGE` (a finite
mathematical value beyond this implementation). An unknown value is `null`,
never zero, a prior observed count, a percentage allowance or the largest
machine's memory. A source with a computed ID is not automatically
mathematically unbounded: finite bars and a proved formatting/loop bound can
make both its population and byte lengths finite.

For 1.7.0, COMPUTE starts attempt 0 with its default arena. A certified bound
only tightens attempt 0 where the required facts exist. A true top-level
`unbounded` means **no tightening available**: retain the named unresolved
sizing terms and use the default attempt-0 arena and the escalation ladder.
This applies equally to missing proofs, proved unbounded growth and finite
bounds outside the representation range.

A typed `arena_exhausted` re-places the run on the next rung of COMPUTE's
trusted ladder, with escalation cap 2. Arena exhaustion is never charged to
the user and raises an alert. The child's private allocations are capped by
its `RLIMIT_AS`; exceeding that cap is the strategy's own typed
`memory_limit`, charged to the user. The calculator reports facts and
verdicts; COMPUTE owns the default, ladder and re-placement policy.

## Facts and the producer's proof obligations

The root schema is `pineforge-arena-facts/v1`. `data` comes from the resolved
request/data manifest. `strategy` comes from a trusted compiler plus the
source-adapter lowering proof. Each group has a nonempty `source` string
naming its source span, compiler artifact or data-plan receipt.

| Input group / fields | Meaning and producer |
| --- | --- |
| data.scriptBars | Upper bound S for all script calculations' distinct script bars, including pre-roll. Data timestamps plus timeframe/session/calendar planner. Only use input count N when grouping/passthrough proves S <= N. |
| data.selected, data.windowBars | Integer flag 0/1 and W, 0 <= W <= S. Selected request and pre-start script-label bucketing in its half-open window. If bucketing is unavailable, use W=S. |
| strategy.controlFlow.status | `bounded`, `unbounded`, or `unknown`; a reason is required for the latter two. Typed compiler call graph and loop/string dataflow proof. |
| strategy.fills.perBar, fixed | F and F0: **applied fill executions** <= S*F+F0 for the entire run. Include old resting requests, bracket fan-out, every fill recalculation, margin/risk reductions and source follow-up commands. This is not a syntactic call-site count. |
| strategy.fills.rowsPerFill | H: maximum durable/published closed slices emitted by one execution, including source-side row expansion. A proved physical lot count suffices only when the lowering proves no extra row multiplicity. Pyramiding alone is unsafe for arbitrary Pine order calls. |
| strategy.fills.fixedRows | Z: additional closed/report-side rows not already charged, if a source lowering can emit them. Zero requires an explicit proof. Counting range-end rows here as well is conservative, although stream sizing separately charges open-at-end rows. |
| strategy.ids.literals | The complete literal component population, including generated source labels when fixed. The calculator unions the empty ID into it and deduplicates strings. Compiler output and adapter-generated ID closure. |
| strategy.ids.freshPerBar, fixedFresh | I and I0: extra component values <= S*I+I0, across **all** cohort/single/pair component domains, including IDs of unfilled/erased placements. A finite array of IDs, finite loop induction and decimal bar-index strings are eligible with proofs. |
| strategy.ids.maxBytes | L: maximum UTF-8 **byte** length of every source/generated ID component in those stores. Include prefixes, suffixes, separators internal to components and bounded decimal tokens. The calculator checks literal lengths but trusts the compiler for computed strings. |
| strategy.pairKeys.mode | `cartesian` uses D*D. `opportunities` also supplies perBar/fixed, proving K <= S*Kbar+K0 for the distinct-ever keys of **each** pair domain. Count attempts that populate consumed-cycle keys even without an accepted placement. |
| strategy.successors.perBar, fixed | Rbar/R0: successor-root rows issued <= S*Rbar+R0. Includes replacements that never fill; kept-handle repricing issues no row. Compiler/lowering command proof, not trades or bars alone. |

The exact arena input formulas are:

```text
T = (S * fills.perBar + fills.fixed) * fills.rowsPerFill + fills.fixedRows
D = |unique(ids.literals union {empty})| + S * ids.freshPerBar + ids.fixedFresh
L = ids.maxBytes
Q = D * D                                           # cartesian mode
Q = min(D * D, S * pairKeys.perBar + pairKeys.fixed) # opportunities mode
R = S * successors.perBar + successors.fixed
E = S                    if selected == 0
E = W + 1                if selected == 1
```

| Symbol | Unit / domain |
| --- | --- |
| N | Loaded primary input bars, including pre-roll; host data manifest. |
| S, W | Script bars in the complete run / selected window; data planner. |
| F, F0 | Per-script-bar coefficient / fixed contribution in the whole-run fill bound. |
| H, Z | Closed slices per fill / additional closed rows from source lowering. |
| I, I0 | Fresh component values per script bar / fixed fresh values. |
| D, Q | Common distinct-ever component / pair populations for the stores. |
| L | Bytes per ID component, including generated components. |
| T, R, E | Closed rows / issued successor-root rows / stored equity points. |
| Kbar, K0 | Pair-key opportunities per script bar / fixed contribution. |
| C, J | Arena chunk bytes / entry-rank key count min(D,27). |
| U_bound | Complete finalizer input/spool bytes, including open-at-end rows. |
| P_rss, P_as | Informational child private resident / address-space terms; certifying either requires its own proof. Neither affects sizing routing. |

The fixed terms can encode a tighter whole-run proof, for example a strategy
which permanently stops issuing after 500 closed slices. They cannot encode
the last observed run, or a constant chosen because it covered a corpus tape.
Where a proof is evaluated for particular data, timeframe, magnifier, inputs,
overrides or imported libraries, its receipt must bind those inputs too.

### Declaration facts do not replace compiled program facts

The carried declaration fields are visible in `PineStrategyConfig` at
`include/pineforge/source/pine_adapter.hpp:66`. Their `project` projection starts at
`src/source/pine_adapter.cpp:2734`. The relevant rules are:

| Declaration / native fact | Sizing consequence and evidence |
| --- | --- |
| pyramiding | Source entry admission policy, including pending/same-point rules. The optional generic cap is `max_open_lots` at `include/pineforge/native_run_spec.hpp:664`; the Pine projection deliberately leaves it unset, as the `max_open_lots` comment and surrounding projection at `src/source/pine_adapter.cpp:2862-2866` state. Do not assume this caps strategy.order or use it as an unconditional H. |
| calc_on_order_fills | Extra source callbacks. The adapter passes its 1<<20 guard `kCoofLoopGuard` through the native cadence at `src/source/pine_adapter.cpp:2834`; the `kCoofLoopGuard` literal is declared at `include/pineforge/source/pine_adapter.hpp:64`. The native point budget is a callback bound, not a total fill bound. |
| max_recalculations_per_point | Further executions still apply after callbacks stop; see the cadence comment and `max_recalculations_per_point` declaration at `include/pineforge/native_run_spec.hpp:686-695`. The actual counter check is `src/native_execution_consumer.cpp:7723`. |
| first-open COOF chain | A second source-side guard exists at `src/source/pine_scheduler_native.cpp:888`. Count this source path; a native budget of 8 is not Pine's bound. |
| process_orders_on_close | Adds a matching opportunity after calculation; projection at `src/source/pine_adapter.cpp:2823`. It does not impose an order/trade count. |
| default_qty, margin, initial capital | Admission/sizing predicates can tighten a separately proved schedule. They provide no universal positive minimum close quantity or minimum loss, hence no universal T. Margin can create additional reductions. |
| max_bars_back | Series-history setting, not a run/trade limit. The native mapping explicitly rules it out at `docs/pages/pine-to-native.md:142`. |
| timeframe, input sessions, magnifier | Determine script buckets, path/sub-bar matching opportunities and callbacks. A magnifier with many sub-bars must enter the fill/root proof even if S is unchanged. |

The guard comments above are cited as comments deliberately. In particular,
an entry can be reissued and a surviving reduction can partially execute;
there is no general “one source order fills once per bar” rule from which
this interface derives a universal bound.

### What a pre-start pass can establish

The committed generated shape is ordinary C++:
`tutorial/macd/generated.cpp:175` emits the Long entry, and
`tutorial/macd/generated.cpp:178` emits the Short entry. Their constructor
configures the Pine host. The generated C entry points wrap run/fill/free.
The native migration map is `docs/pages/pine-to-native.md`; it describes
native equivalents, not an existing compiler FACTS manifest.

A lexer/AST pass can count entry/order/exit/close/close_all/cancel/cancel_all
sites, inventory literal ID/from_entry operands, mark computed operands,
find enclosing loops and build a typed call graph. A certification pass
must then:

1. Expand each reachable function/method invocation into its order/output
   effects. A call through an array/UDT method is still a call. Reject
   unsupported indirect calls as unknown.
2. Multiply nested constant trip counts; use proved value ranges for
   input-dependent loops and finite arrays. Sum branches for a conservative
   count, or take a maximum only with a branch exclusivity proof.
3. Prove a finite recursion depth, or mark its affected facts unknown.
   A syntactic recursion occurrence alone does not prove infinite execution.
4. Bound computed string ranges: literal concatenation adds byte maxima,
   a finite choice takes a maximum, bounded decimal conversion has a proved
   digit/sign count. Include array elements and function-returned IDs.
5. Multiply by the actual source callback bound, then lower source effects
   into all possible native placements, replacements and executions.
   Pending orders from earlier bars and implicit broker commands participate.
6. Emit the FACTS record with proof spans and the complete generated ID
   closure. A source-site count by itself is never F, H, Q or R.

`scripts/arena_bound_source.py` provides an inventory, not step 5's missing
general proof. It recognizes a top-level unconditional emitting `while true`
without a lexical exit as an unbounded computation; nonconstant loops,
computed IDs, function effects and unsupported proofs otherwise remain
unknown until the compiler supplies the record. It never marks ordinary
literal-only source bounded merely because a lexical count is small.

`scripts/arena_bound_manifest.py` is the common production/lab reader.
`arena-facts.json`, next to generated.cpp, carries schema, producer,
`sha256` entries for generated.cpp/strategy.pine/inputs.json (explicit null
for an absent optional file), engineSha256, scope, strategy and optional
wider-term facts. engineSha256 hashes the runtime source/header closure
and CMakeLists with the versioned engine_fingerprint function. Scope is
either kind=all-data (a universal compiler proof) or kind=plan with the
exact pre-start planSha256. The latter hashes immutable primary input
bytes and resolved run settings via plan_fingerprint; no report enters it.
The reader checks fingerprints before starting the strategy. Hashes bind a
record to input bytes; they do not prove that its numeric claims are true.
The producer must be trusted build tooling, never user-supplied assertions.
The compiler owner must additionally bind imported library closures, the
target/compiler profile and any external request/environment identity not
already in the resolved plan. **That producer and external provenance
integration remain RETURN TO CORE-TOP.**

## Compiler facts the bound needs

This is the precise sidecar handoff for the codegen lane. Emit
`arena-facts.json` beside the final `generated.cpp`, after typed effect
analysis and source-to-native lowering. The transpiler is outside this
engine checkout; the emitting stages below name responsibilities for that
owner, not existing transpiler file locations.

The root, `sha256`, `scope` and `strategy` are JSON objects. `uint64` below
means a JSON integer in [0,18446744073709551615], decoded losslessly;
booleans, floats and numeric strings are invalid for those fields.
`flag` means a JSON integer 0 or 1. A digest is a lowercase 64-character
SHA-256 string. `source` is a nonempty string identifying the pre-start
proof artifact/source spans. Each path starts at the sidecar root; `[]`
denotes every array element. Multiple paths in a cell are separate fields
of the stated type.

| Fact name | JSON type | Field path in arena-facts.json | Where codegen would emit it | Cost note per group |
| --- | --- | --- | --- | --- |
| Schema version | string, exactly `pineforge-arena-facts/v1` | `$.schema` | Final sidecar writer | Envelope: hash final artifacts and engine closure once; exact-plan scope also hashes input bytes/settings. |
| Trusted producer identity | nonempty string | `$.producer` | Trusted compiler build identity, at sidecar write | — |
| Generated artifact digest | digest | `$.sha256["generated.cpp"]` | After generated.cpp is finalized | — |
| Pine source digest or explicit absence | digest or null | `$.sha256["strategy.pine"]` | Resolved source-input closure; null only if absent | — |
| Input parameters digest or explicit absence | digest or null | `$.sha256["inputs.json"]` | Resolved parameter specialization; null only if absent | — |
| Runtime/header/CMake closure digest | digest | `$.engineSha256` | Lowering target receipt using `engine_fingerprint` | — |
| Proof scope | string, `all-data` or `plan` | `$.scope.kind` | After deciding which input/run settings the proof specializes on | — |
| Exact plan digest | digest; required only for `kind=plan` | `$.scope.sha256` | Host-assisted plan resolution using `plan_fingerprint` | — |
| Control-flow verdict | string, `bounded`, `unbounded` or `unknown` | `$.strategy.controlFlow.status` | Typed reachable call graph, loop ranges and recursion analysis | Control flow: analyze the typed effect graph; unsupported paths produce unknown without executing the strategy. |
| Control-flow evidence | nonempty string | `$.strategy.controlFlow.source` | Same proof pass, including source spans | — |
| Unknown/unbounded explanation | nonempty string; required unless bounded | `$.strategy.controlFlow.reason` | Failed proof obligation or proof of unbounded emitting growth | — |
| Applied-fill coefficients F, F0 | uint64 each | `$.strategy.fills.perBar`; `$.strategy.fills.fixed` | After callback/loop expansion and broker execution lowering, including old resting orders, COOF, magnifier and risk/margin fills | Fills: propagate whole-run effects and lot/row multiplicity; declaration or call-site counts alone cannot supply them. |
| Closed slices per applied execution H | uint64 | `$.strategy.fills.rowsPerFill` | Physical-lot plus source row-expansion proof | — |
| Additional closed rows Z | uint64 | `$.strategy.fills.fixedRows` | Lowering's rows outside the applied-fill contribution | — |
| Fill evidence | nonempty string | `$.strategy.fills.source` | Proof artifact for F/F0/H/Z, including explicit zero proofs | — |
| Literal ID components | array of strings | `$.strategy.ids.literals` | Complete source and adapter-generated ID closure; empty is added by the calculator | IDs: collect literals and propagate computed string populations/UTF-8 byte maxima through typed value ranges. |
| Fresh ID coefficients I, I0 | uint64 each | `$.strategy.ids.freshPerBar`; `$.strategy.ids.fixedFresh` | Bounded loops, arrays, method/function returns and generated-ID rules across every store domain | — |
| Component byte maximum L | uint64 | `$.strategy.ids.maxBytes` | UTF-8 string-range proof including generated prefixes/suffixes and decimal tokens | — |
| ID evidence | nonempty string | `$.strategy.ids.source` | Literal/computed operands and generated-component proof artifact | — |
| Pair-domain proof mode | string, `cartesian` or `opportunities` | `$.strategy.pairKeys.mode` | Pair-store effect analysis after lowering | Pair keys: Cartesian mode reuses D; a tighter opportunity proof must also count unsuccessful and erased attempts. |
| Key-opportunity coefficients Kbar, K0 | uint64 each; required in opportunities mode | `$.strategy.pairKeys.perBar`; `$.strategy.pairKeys.fixed` | Count opportunities for each sequence/erased/consumed domain, including attempts without an accepted order | — |
| Pair evidence | nonempty string | `$.strategy.pairKeys.source` | Complete-domain or opportunity proof artifact | — |
| Successor-root coefficients Rbar, R0 | uint64 each | `$.strategy.successors.perBar`; `$.strategy.successors.fixed` | Native placement/replacement effect pass; include unfilled issued definitions, exclude kept-handle repricing only with proof | Successors: count lowered issued-root effects independently of fills and trades. |
| Successor evidence | nonempty string | `$.strategy.successors.source` | Root-issuance proof artifact | — |
| Open-at-end rows | uint64 | `$.stream.openAtEndRows` | Report lowering's terminal open-row proof | Stream: reuse typed effect counts for the whole run; missing optional string maxima use the existing line ceiling. |
| Output, trace and hash flags | flag each | `$.stream.outputs`; `$.stream.trace`; `$.stream.hashes` | Resolved report configuration; specialize/bind request-dependent flags | — |
| O row coefficients | uint64 each; required when outputs=1 | `$.stream.outputRowsPerBar`; `$.stream.outputRowsFixed` | Output-writer schedule after callbacks and loops | — |
| V row coefficients | uint64 each; required when outputs=1 | `$.stream.eventsPerBar`; `$.stream.eventsFixed` | Event effects after function/loop/callback expansion | — |
| X row coefficients | uint64 each; required when trace=1 | `$.stream.tracePerBar`; `$.stream.traceFixed` | Trace effects after function/loop/callback expansion | — |
| H row coefficients | uint64 each; required when hashes=1 | `$.stream.hashesPerBar`; `$.stream.hashesFixed` | Complete hash-writer schedule | — |
| Series and constant slot maxima | uint64 each; required when outputs=1 | `$.stream.seriesSlots`; `$.stream.constantSlots` | Final output descriptor layout | — |
| Optional string byte maxima | uint64 or null each; may be omitted | `$.stream.commentBytes`; `$.stream.messageBytes`; `$.stream.traceNameBytes` | UTF-8 range proof for comments, event messages and trace names | — |
| Stream evidence | nonempty string | `$.stream.source` | Output/terminal-row proof artifact bound to run flags | — |
| Physical feed inventory | array of objects | `$.ohlcv.feeds` | Host-assisted data-plan emission for primary/script/magnifier/extra feeds | OHLCV: enumerate buffer capacities/copies and page-round each mapping; static codegen alone cannot know host allocations. |
| Host page bytes | positive uint64 | `$.ohlcv.pageBytes` | Host allocation/mapping plan receipt | — |
| Allocated bar capacity | uint64 | `$.ohlcv.feeds[].bars` | Physical allocation capacity, including conversion/retained copies | — |
| Separate value columns | uint64 | `$.ohlcv.feeds[].columns` | Per-feed physical column inventory | — |
| Separate close-time array | flag | `$.ohlcv.feeds[].closeTimes` | Per-feed physical close-array inventory | — |
| Bar-buffer layout overhead | uint64 | `$.ohlcv.feeds[].overheadBytes` | Bar-file header/padding layout | — |
| Resident and address-space copy counts | uint64 each | `$.ohlcv.feeds[].residentCopies`; `$.ohlcv.feeds[].asCopies` | Wrapper sharing/mapping proof; count each AS mapping | — |
| OHLCV evidence | nonempty string | `$.ohlcv.source` | Complete host feed/allocation receipt | — |
| Informational private RSS and AS | uint64 or null each | `$.childPrivate.rssBytes`; `$.childPrivate.asBytes` | Optional host/compiler private-state receipt; omit the group if unavailable | Private diagnostics: independent RSS/AS evidence is optional for sizing; child RLIMIT_AS enforces the strategy's memory limit. |
| RSS observation provenance | boolean | `$.childPrivate.measured` | RSS receipt only; false when no RSS measurement applies | — |
| Independent private certifications | boolean each | `$.childPrivate.rssCertified`; `$.childPrivate.asCertified` | Independent applicable proofs; an RSS observation cannot certify AS | — |
| Private evidence | nonempty string | `$.childPrivate.source` | Private runtime/strategy-state receipt | — |

For a bounded strategy, every listed strategy numeric field is required,
except pair opportunity fields in cartesian mode. A genuine proved zero
is emitted as zero. Missing/unsupported strategy proofs use
`controlFlow.status="unknown"` with source/reason, never invented numeric
values. `stream`, `ohlcv` and `childPrivate` are optional root objects;
omitting either of the first two leaves its sizing term unknown.
Omitting childPrivate leaves only its informational flag true. Within a
present private group, measured and both certification booleans are
required; missing/null RSS/AS stays unknown.

The proof trace named by each `.source` must cover the typed reachable
order sites, literal/computed ID operands, weighted nested trip bounds,
array/UDT dispatch, finite recursion depths, callback/magnifier schedules,
implicit broker effects and generated ID byte rules relevant to that
group. These are proof artifacts behind the table's fields, not additional
counts the calculator guesses. Imported libraries, compiler/target profile
and external request/environment identities must be bound by that trusted
proof where relevant. The current reader checks the three artifact hashes,
engine closure and plan scope; it does not validate a new external-identity
schema. Until the producer and that integration exist, such cases remain
unknown.

`data` is supplied by the resolved host plan at `bound_probe`, not read from
the sidecar: `scriptBars` and `windowBars` are uint64 with windowBars <=
scriptBars, `selected` is a flag, `source` is nonempty, and `planSha256` is
the digest required to match an exact-plan scope. Codegen emits the
coefficients; the host supplies S/W/selection and the applicable allocation
inventory. A census may check proof availability with synthetic dimensions,
but those dimensions never certify a real request size.

## Storage derivation

Let C=1,048,576 bytes. `chunkCeil(x)=C*(x//C + (x%C != 0))`.
Define J=min(D,27), and:

```text
slots(0) = 0
slots(k) = max(64, nextPowerOfTwo(2*k))              # k > 0
indexPeak(0) = 0
indexPeak(k) = 1024                                # 1 <= k <= 32
indexPeak(k) = 24 * slots(k)                       # k > 32
```

| Physical user | Logical peak bytes | Code evidence |
| --- | --- | --- |
| Equity writer / selected hold-back / SpillCurve | 24*E | `pf_equity_point_t` size assertion `include/pineforge/metrics_stream.hpp:15`; `src/report_scratch.cpp:232` |
| Position tail | (30+L)*T | L18 sibling footer evidence described below; absent from this base |
| Cohort AVL and five-word summary | (96+L)*D + 1[D>0] | `include/pineforge/detail/disk_id_table.hpp:109`; `include/pineforge/source/pine_adapter.hpp:1830` |
| Entry-rank AVL | (60+L)*J + 1[J>0] | `src/source/pine_adapter.cpp:6377`; `include/pineforge/detail/disk_id_table.hpp:109` |
| Sequence minima data | (40+L)*D + (40+2*L)*Q | `include/pineforge/detail/id_pair_store.hpp:140`; `sequence_index_` `include/pineforge/source/pine_adapter.hpp:934` |
| Sequence minima index | indexPeak(D+Q) | `include/pineforge/detail/id_pair_store.hpp:403` |
| Erased minima data | (40+L)*D + (40+2*L)*Q | `erased_minima_` `include/pineforge/source/pine_adapter.hpp:936`; `remember_erased` `src/source/pine_adapter.cpp:853` |
| Erased minima index | indexPeak(D+Q) | `include/pineforge/detail/id_pair_store.hpp:403` |
| Consumed partial-cycle data | (41+2*L)*Q | `consumed_partial_exit_cycles_` `include/pineforge/source/pine_adapter.hpp:2592`; `src/source/pine_adapter.cpp:24901` |
| Consumed partial-cycle index | indexPeak(Q) | `include/pineforge/detail/id_pair_store.hpp:403` |
| Successor-root rows | 16*R | `src/native_order.cpp:5913`; `src/native_order.cpp:5964` |

The AVL node is seven uint64 words = 56 bytes, then its value and exact key
bytes. CohortFacts::Record is handle, cycle, origin_count, origin_digest and
erased_sides = 40 bytes. Rank values are uint32 = 4 bytes, and the existing
size>26 cutoff retains at most 27 keys. An initially empty AVL begins at
offset one, hence each of the two prefixes.

An IdPairStore record is five eight-byte words = 40 bytes; its slot has two
uint64 fields = 16 bytes. Sequence and erased stores each have separate
single and pair domains. Erase clears a live flag, not the distinct-ever
`used_` count; reappearance reuses the data record. At a largest rehash the
old half-size index and new index coexist: 16*(slots+slots/2)=24*slots.
The first 64-slot index has no old index, so its peak is 1024 bytes.
The consumed-cycle single key is source_id + separator byte + from_entry,
thus 40+2*L+1, not 40+L.

The L18 sibling's `src/source/pine_position_tail.hpp`, line 20, specifies a
30-byte footer. Its `src/source/pine_position_tail.cpp`, line 96, packs
exit_time (8), qty (8), entry_incarnation (8), side (1), margin flag (1)
and ID length (4), after the ID bytes. This is not sizeof(Row). The exact
external file:line evidence is recorded in the lane report; these sources
must be merged and their local anchors refreshed before claiming coverage.

Successor rows are two uint64 values. The WorkingRequestCore method `index_committed`
defined at `src/native_order.cpp:1190` omits kept-handle replacements and appends a
root only when a newly issued definition has one. The 64-row RAM tail makes
charging every issued row conservative. Partial closes create one trade
row per affected lot: `src/engine_execution.cpp:1241`. Bracket definitions
have limit, stop and trailing families; per-entry expansion is visible at
`src/source/pine_adapter.cpp:23425`. A closed-trade count cannot bound
unfilled placement history or the replacement-root history.

Summing all eleven peaks gives:

```text
B = 24*E + (30+L)*T + (96+L)*D + 1[D>0]
    + (60+L)*J + 1[J>0]
    + 2*((40+L)*D + (40+2*L)*Q + indexPeak(D+Q))
    + (41+2*L)*Q + indexPeak(Q) + 16*R

arenaBytes = chunkCeil(B) + 14*C
```

There are eleven persistent regions and up to three extra replacement
indexes. Each independently rounded region wastes less than C. Summing
independent peaks and adding fourteen complete chunks is conservative even
when their real peak times differ. This covers chunk fragmentation, not
unlisted stores or a missing population proof.

At L=64 the position term is **94*T**; each placement data term is
**104*D+168*Q**; consumed-cycle data is **169*Q**; the root term is
**16*R**. The contract's eleven coefficients agree with the inspected code
and L19l-A/C reports. No numeric correction to those coefficients was
needed. “72..104 bytes/key” in the L19l-C steady-state explanation does
not include the rehash overlap and must not replace indexPeak in sizing.

These formulas apply to the EngineRecorded streamed invocation. The
selected recorder appends its anchor and selected observations in one
writer (`src/engine_report.cpp:833`), and capture transfers that writer as `metric_curve`
instead of copying its prefix (`src/native_execution_consumer.cpp:12675`).
HostRecorded array presentation and a second full curve are outside this
profile. Adding another persistent scratch user requires another term and
rounding region.

## OHLCV resident and address-space term

`include/pineforge/bar.hpp:6` holds five binary64 values and one int64
timestamp: 48 bytes/bar. Borrowed feed views retain pointers and counts,
not another history (`include/pineforge/native_bar_views.hpp:30`).
Instrument columns are double arrays
(`include/pineforge/native_run_spec.hpp:532`); instrument close timestamps
are int64 arrays (`include/pineforge/native_run_spec.hpp:584`).
Before page rounding the terms are 48*N for primary input, 48 times every
actually allocated script-bar or magnifier-bar capacity, and
(48+8*closeTimes+8*columns)*feedBars for each extra feed copy. Borrowing
the primary input as script or magnifier history adds zero copies; it does
not charge that input a second time.

The optional `ohlcv` fact group supplies pageBytes, source, and feeds. Each
feed describes bars, columns, closeTimes (0/1), overheadBytes,
residentCopies and asCopies, all integers. Enumerate primary input,
additional instrument/auxiliary feeds, and every actually owned script or
magnifier-bar buffer. A borrowed script view or lower-path segment adds
zero full-history copies; its fixed descriptors and current buffers belong
in childPrivate. Owned vector capacities, CSV conversion copies or retained
preloaded arrays must be included explicitly, not silently treated as a
borrowed binary mapping. `overheadBytes` is manifest-known bar-file
padding/header bytes (zero for the current headerless bar file).

For each physical feed bundle:

```text
g = pageCeil(48*bars + overheadBytes)
    + (columns + closeTimes)*pageCeil(8*bars)
residentBytes = sum(g * residentCopies)
asBytes       = sum(g * asCopies)
```

Each column/close buffer can be a separate mapping, so round each, not
their combined payload. Repeated mappings of the same inode still count
separately against address space; residentCopies can reflect physical
sharing only with a wrapper proof. The resident result is a conservative
fully resident page charge, not an assertion that disk pages remain resident.
Metadata/allocator overhead belongs in the private term. An incomplete
feed/copy inventory makes the entire OHLCV term unbounded/unknown.

## Finalizer stream term U_bound

The finalizer receives the sink, not the arena. Its spool bound includes
the header, all records, trailer and LF bytes. It is not its private heap
bound. The hard successful-line ceiling is 1,048,576 including LF:
`src/report_sink_writer.cpp:21` and `src/report_sink_writer.cpp:29`.
The ceiling is already enforced by the writer; this calculator adds no
new run-capacity gate.

The optional `stream` group supplies source and openAtEndRows. Its flags
outputs/trace/hashes are integer 0/1. An enabled output group supplies
seriesSlots, constantSlots, outputRowsPerBar/Fixed and eventsPerBar/Fixed.
Enabled trace/hashes supply tracePerBar/Fixed and hashesPerBar/Fixed.
Those counts must include loops, nested functions and COOF callbacks.
Disabled categories have count zero by the declared run flags.

```text
nT = T + openAtEndRows              # T alone omits range-end open rows
nE = E
nO = S*outputRowsPerBar + outputRowsFixed  if outputs else 0
nV = S*eventsPerBar + eventsFixed          if outputs else 0
nK = 1                                   if outputs else 0
nX = S*tracePerBar + traceFixed            if trace else 0
nH = S*hashesPerBar + hashesFixed          if hashes else 0
```

K is at most one by `src/report_sink_writer.cpp:517`. The compiler can
tighten other counts only with a proof; finite E/T does not bound event
or trace calls made inside an unknown loop.

Safe signed JSON integers use <=17 characters, hash64 is 18 quoted
characters, and f64 conservatively uses <=64 characters: the formatter
`f64` at `src/report_sink_writer.cpp:162` builds scientific output with a fixed
buffer then performs bounded exponent relayout. Each input string byte
becomes at most six JSON bytes, plus two quotes; see
`src/report_sink_writer.cpp:207`. Counting delimiters and the LF gives:

| Tag | Maximum line bytes, before taking the hard ceiling | Builder |
| --- | --- | --- |
| E | 154 | `src/report_sink_writer.cpp:379` |
| T | 675 + 12*L + 6*commentBytes | `src/report_sink_writer.cpp:398` |
| O | 60 + 65*seriesSlots | `src/report_sink_writer.cpp:486` |
| V | 239 + 6*messageBytes | `src/report_sink_writer.cpp:499` |
| K | 6 + 65*constantSlots | `src/report_sink_writer.cpp:513` |
| X | 110 + 6*traceNameBytes | `src/report_sink_writer.cpp:523` |
| H | 25 | `src/report_sink_writer.cpp:531` |

For variable-width rows use min(expression,1048576). Missing comment,
message or trace-name byte proofs use the existing full-line ceiling,
which is finite for a successful wire line. Missing **record counts**
cannot use that ceiling to invent a count and make this term unknown.
V's constant conservatively covers both quoted and null messages.

```text
U_bound = 2*1048576 + sum(nTag * maxLineBytesTag)
```

The two full line allowances cover header descriptors/run_id/version and
the trailer counts/digest. T includes eight floats, six signed integers,
an incarnation, a side, a bool and three strings. Charging the actual ID
component L twice and the independent comment maximum accounts for its
string fields. Output/spool storage is separate from the eleven scratch
regions.

## Child private RSS and address space

`childPrivate` supplies rssBytes/asBytes (integer or null), measured
(boolean), rssCertified/asCertified (booleans), and source. `measured`
describes the RSS observation only. It never claims an AS measurement.
The term is unbounded if either value is unknown or either certification
flag is false. This flag is informational: it does not affect the top-level
verdict, its reason or `unboundedTerms`. Private growth is capped by the
child's `RLIMIT_AS` and a breach is the strategy's typed `memory_limit`.

CORE-TOP's A5-WIDER brief reports a **44.4–46.2 MB pre-flat-port resident
constant**, referring to Q2 at 15:43. That is a historical measurement,
not a source-derived upper bound for this merged tree or arbitrary
compiled strategy state. No P_as measurement was supplied. The calculator
therefore installs neither value by default. A supplied historical RSS
can be displayed with measured=true, rssCertified=false and asBytes=null;
the informational term remains unbounded. The formula tests also use
4096/8192 private bytes to check arithmetic/flag transport under a
hypothetical trusted proof; those numbers are not real engine coefficients.
The all-terms hand vector omits that toy proof and has a bounded top-level
verdict with private RSS/AS still null.

An applicable private proof must account for the native/Python runtime,
strategy-owned state and allocation peaks, fixed sink/curve batches,
mapped-window reservation, arena bitmap and region-index capacities,
stacks, libraries and any separate AS mappings. RSS observations cannot
be substituted for that inventory or copied into P_as.
An applicable post-port RSS/AS proof is needed only to claim certified
finite private diagnostics. Its absence leaves the informational flag true
and does not prevent tightening when the three sizing terms are certified.

## Worked values and the hand vectors

These contract-shaped examples presume their population proofs have
actually been supplied. They do not assert these caps for arbitrary Pine.

| Input / contribution | LARGE | Small |
| --- | ---: | ---: |
| S; selected; W | 8,000,000; 1; 8,000,000 | 10,000; 1; 2,000 |
| T; D; Q; L; R | 400,000; 400,000; 400,000; 64; 8,000,000 | 500; 500; 500; 64; 10,000 |
| E | 8,000,001 | 2,001 |
| Equity | 192,000,024 | 48,024 |
| Position tail | 37,600,000 | 47,000 |
| Cohorts | 64,000,001 | 80,001 |
| Rank | 3,349 | 3,349 |
| Each of two placement data files | 108,800,000 | 136,000 |
| Each of two placement index peaks | 50,331,648 | 49,152 |
| Consumed data | 67,600,000 | 84,500 |
| Consumed index peak | 25,165,824 | 24,576 |
| Roots | 128,000,000 | 160,000 |
| B | 832,632,494 | 817,754 |
| chunkCeil(B) | 833,617,920 | 1,048,576 |
| Fourteen-chunk slack | 14,680,064 | 14,680,064 |
| **arenaBytes** | **848,297,984 (809 MiB)** | **15,728,640 (15 MiB)** |

One primary 48-byte feed contributes raw 384,000,000 / 480,000 bytes.
With 4096-byte pages its fully resident/AS charge is 384,000,000 /
483,328 bytes. The small page charge is 3,328 bytes above raw payload;
the contract's raw-data example did not include that page rounding.

If there is at most one open-at-end row, no O/V/K/X/H, and commentBytes=64,
T lines are <=1827 bytes. U_bound is then 1,964,899,133 bytes for LARGE
and 3,320,633 bytes for Small. These extra hypotheses must be facts, not
inferred from the trade population. Private RSS/AS remains a separate
unknown term in the absence of its proof.

The eight JSON vectors were calculated by hand:

| Vector | Hand calculation / distinguishing boundary |
| --- | --- |
| Empty run | D=1 for empty component; B=97+61+40+40+1024+1024=2286; arena=15 MiB. |
| Small / LARGE | Eleven contributions in the table above, including both two-file rows. |
| Rehash at 33 | D=32,Q=1,L=1,E=1; both 128-slot rehash peaks are 3072; B=14696; arena=15 MiB. |
| Unicode | D=2,Q=4,L=2,T=2,R=2,E=2; B=48+64+197+125+520+2048+180+1024+32=4238. |
| Million points | B=24*1048576+2286=25168110; ceil=25 MiB; arena=39 MiB=40894464. Half supplies 19 complete chunks, below the actual 24 MiB curve. |
| All terms | Same Unicode arena. Primary one-page RSS/two-page AS; extra bar+two column+close buffers consume four pages with RSS copies=2: RSS=36864, AS=24576. Counts T/E/O/V/K/X/H=3/2/2/5/1/3/2; line maxima 717/154/190/263/71/140/25. U=2097152+2151+308+380+1315+71+420+50=2101847. Private RSS/AS are null and its own flag is true; top-level unbounded=false, unboundedTerms=[], reason="". |
| Unbounded emitting loop | Null arena inputs except E=2; no finite guessed size. unboundedTerms=[arena,ohlcv,finalizerStream]; childPrivate remains informational. Default attempt-0 arena and escalation apply. |

All eight vectors were recomputed by hand for the three-term verdict.
The first six retain their arena/logical byte values and have
`unboundedTerms=["ohlcv","finalizerStream"]`; their absent private proof
continues to set only its own flag. In the all-terms vector, removing the
old toy private proof isolates unknown childPrivate: the former four-term
rule would flag that input, while the three-term verdict is bounded.

### TypeScript arithmetic

Use BigInt for all arithmetic and comparisons, with uint64 checked addition
and multiplication. Do not use bitwise Number operations, floating log2,
Math.pow, or round through Number while loading large JSON integers.
Iterative doubling from 64 yields slots. Compute ceiling by quotient and
remainder, not x+C-1 in a fixed-width integer. In opportunity mode evaluate
min(D*D,K) without requiring a representable D*D when K is the tighter
uint64 value. Deduplicate literal strings by value and count their UTF-8
bytes, not UTF-16 code units.

ScratchArena's current representation limit is
(2^32-1)*1048576 = 4503599626321920 bytes, from its uint32 chunk-table
check at `src/scratch_arena.cpp:131`. This is not a supported allocation
promise or a size for an unknown strategy. A bound outside it returns
null with PLATFORM_RANGE. It must not be clamped to the cap.

## Lab verification and remaining integration

The lab harness links the bridge and generated code into the same module
as the static engine, then explicitly attaches a preallocated regular-file
arena. It reads the existing high_water/bytes_used stats after each run.
No production header declaration is changed.

Linux linker wrappers reject write/pwrite/ftruncate on unlinked regular
scratch files, while allowing the named report sink. A raw-fd scratch
consumer therefore fails instead of producing an artificially low
high-water measurement. The current base still has those owning-port
dependencies. It must not be reported as corpus proof until they land.

Fixtures submit order bursts/brackets, hit the entry pyramid limit, vary
computed IDs, use 4096-byte prefixes, partially close entries, issue
512 unfilled successors per calculation, and run a selected window.
The million-point control must materialize at least 23 MiB of curve and
fail with typed report_sink_failed/scratch arena exhausted at bound/2.
The source-only infinite/unknown-loop fixtures never execute.

The generated-code fixture's w is 1, 4 or 8 from the selected static
schedule, and it never enables COOF. Its bounded input has prices 99..102,
entry quantity 16 and initial capital 1e12; even all w*S entry attempts
together have notional <=102*16*w*S, below capital for these fixture
dimensions. Thus no margin-generated fills are needed in that profile.
There are three named partial brackets per entry and at most three
candidate legs per bracket/origin. The deliberately loose fill coefficient
is 4*(w+9*w*w+1), covering the four modeled OHLC opportunities, entry
attempts and close-all. Its historical opening count w*S is used as H
instead of guessing a universal physical pyramiding cap. Z=w*S also
overcounts terminal/report rows. Fresh component count is overcharged at
8*w*S+8, component bytes at 64 or 4160, and key opportunities at
16*w*w*S; roots are overcharged at four times the fill coefficient.
The separate unfilled-root profile has exactly R=512*S.
These static schedule/lowering arguments still require the lab review and
actual runs; their coefficients were not fitted to observed output.

The activation guards require eight entry calls every script bar in the
burst case, an observed eight-lot peak in the pyramid case, more closed
slices than entry calls in the partial case, and the full issued root
count. A schedule that only submits rejected commands cannot pass those
guards as trade/partial/pyramid evidence.

The full mode enumerates every source-marked validation directory plus
every parity-baseline probe, uses the same manifest reader before each
run, and drives the canonical run_strategy CLI. Missing proofs are printed
and cause incomplete proof coverage to fail. The
`RETURN TO CORE-TOP: no corpus probe has a certified FACTS record` guard is
retained: this lab harness must not pass without any certified arena to
compare with actual usage. It is a certification failure, not a production
admission or sizing decision. Production still uses the default attempt-0
arena and escalation ladder without a bound. Every measured run prints actual,
bound and actual/bound; the minimum ratio identifies worst conservatism.
No measured worst ratios exist yet. A 15 MiB allowance for a one-chunk
curve can be 15x over actual; the fixture's deliberately loose fill/lot
product can be thousands of times over actual. There is no finite universal
worst ratio: an inactive strategy may use zero arena.

Exact Linux lab sequence, after the supervisor's proof commit and merged
owning ports (never execute this on the Mac):

```bash
python3 scripts/ci_preflight.py
cmake -S . -B build-arena -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build-arena -j4
ctest --test-dir build-arena -R '^test_arena_bound_formulas$' --output-on-failure
ctest --test-dir build-arena -R '^test_arena_bound_corpus$' -V
python3 scripts/ci_verify.py release --build-dir build-verify --jobs 4
./scripts/run_corpus.sh
```

Run the arena harness and run_corpus.sh sequentially. Derived feeds must
be fresh before independent consumers fan out. Runtime/corpus CTest rows
are labelled slow;arena-bound, with corpus added on the full row. The
runtime-only row is test_arena_bound_runtime. Both runtime modes explicitly
return skip code 77 on non-Linux hosts; unavailable corpus/proof inputs on
Linux fail. Logs and measured receipts live under build-arena/arena-bound.
