# Live ledger admission and delivery status

`pineforge-live run --max-ledger-bytes N` budgets the SQLite database, WAL and
SHM files together. The default is 0 (unlimited) for both ordinary and service
runs in this release. `--status-file` enables service mode without requiring
a budget. Requiring a positive service budget remains a pending policy
decision, controlled by `service_requires_ledger_budget` in `runner/store.hpp`.
The `--ledger-unbounded` opt-out is implemented for that policy. A positive
budget applies even with the opt-out. The policy's refusal, when enabled, is
`run_request_invalid` with `{"option":"arguments"}` and exit 1.

Offline `redeliver --max-ledger-bytes N` applies the same budget to attempts
and results. Its default also remains unlimited. A live control submission
uses the running owner's budget.

Admission runs under the writer mutex immediately before COMMIT. A finite
budget disables SQLite cache spilling: the transaction, including triggers,
splits and overflow records, changes cached pages first. Admission can then
bound the pages that COMMIT can write, counting shared pages once. It first
releases unreferenced clean pages, then counts only this ledger's resident
pages. Dirty pages and referenced clean pages, including the transaction's
header page, remain resident. Rollback cache history and unrelated SQLite
allocations therefore do not change the request for the same write. A refusal
rolls back the cached changes before any database or WAL growth. Dirty pages
stay resident until admission and COMMIT, so a large transaction also needs
memory for its dirty pages. Unlimited ledgers retain SQLite's normal spilling.

An identical committed input, routing document, report or redelivery request
needs no write admission. Schema creation and migration use the same cached
transaction admission. Opening separately admits missing sidecars before any
SQL reader can allocate them; changing journal mode separately admits a
missing database header page and sidecars.

The refusal is `LedgerBudgetExceeded`, carrying the existing
`scratch_capacity_exceeded` code with `{"resource":"layout"}`. **Here `layout`
names this ledger byte budget.** It does not mean report scratch storage in
this diagnostic. Integer fields `used`, `requested` and `limit` describe
ledger admission and appear in the diagnostic JSON; they add no arguments to
the unchanged failure-code catalog. Admission refuses when
`used + requested > limit`, using saturating arithmetic, and exits 3.
When admission is re-evaluated against unchanged ledger files, a write refused
with `requested=R` is refused again at `limit=used+R-1`, including after another
refusal. At `used+R` it is admitted, and database/WAL/SHM growth is bounded by
`R`. The reservation remains an upper bound on physical growth.

A required-write refusal cancels delivery and stops the advanced strategy
instance. It is latched under the mutex so another writer cannot add rows
during shutdown.
Restart replays the committed prefix. Finite budgets disable the implicit
checkpoint on close, retaining the durable WAL for recovery. SQLite WAL
checkpoint copies during admitted commits are included in admission.
Initialization can fail before status publication starts. A running service's
`storage_below_budget` readiness field becomes false on a storage stop.

The physical bound uses these quantities:

| Quantity | Meaning |
|---|---|
| `P` | SQLite page size. |
| `C0`, `C1` | Logical page counts before BEGIN and after the cached SQL changes. |
| `Q` | This ledger's resident page count after all SQL changes and release of unreferenced clean pages. |
| `D` | `min(C1, Q)` when the cache observer is available and auto-vacuum is off; otherwise `C1`. |
| `E` | `extra_payload_bytes`: the serialized cumulative report checkpoint length for a scheduled boundary input; zero for other writes. |
| `A` | Zero when `E=0`, otherwise `floor(8*E/(P-64)) + 4` additional pages. |
| `K` | `ceil(65536/(P+24))` extra WAL frames for sector padding. |
| `F` | `D + A + K` accounted frames. |
| `main`, `wal`, `shm` | Current physical file sizes; `used = main + wal + shm`. |

Every dirty page has a resident full-page buffer when spilling is disabled.
`sqlite3_db_release_memory()` evicts unreferenced clean pages without writing
dirty pages or changing WAL reader marks. The runner observes cache creation
and destruction through SQLite's page-cache interface, delegating allocation,
fetching, eviction and spilling to the original SQLite implementation. Its
`xPagecount` counts pages directly, avoiding truncation of the older 32-bit
cache-byte statistic. Cache ownership is private to the ledger connection;
statement allocations, other connections and unrelated heap allocations do
not enter `Q`. Counts are summed with saturating 64-bit arithmetic.

Both `C1` and `Q` bound the pages COMMIT can write. Referenced clean pages
include the header page that COMMIT may modify. This includes all tables,
indexes, parent paths, splits, freelist work, overflow/text pages and trigger
writes at their actual page sizes. There is no guessed tree height or
per-mutation path multiplier. Auto-vacuum may dirty additional pages at COMMIT,
so it uses all logical pages. The observer is configured before SQLite
initialization. If configuration is unavailable, or a cache's page count is
out of range, the same whole-logical-page fallback applies. Disabling SQLite
allocation tracking does not disable page counting.

COMMIT writes each dirty page to the WAL once, apart from possible repeats of
the last frame to finish a sector under FULL synchronization. `K` allows a
whole 64 KiB sector, rounded to frames. Each frame has a 24-byte header. Allow
both WAL growth and a checkpoint copy with `2*F*(P+24) + 32`, including a
32-byte WAL header. Add `max(C0*P - main, 0)` for earlier logical pages that
may still be checkpointed into the main file. New transaction pages are
already covered by the frame allowance.

WAL-index page numbers and hash slots need at most eight bytes per indexed
frame. Bound its size, including region rounding, by
`32768 * (2 + floor(8*(floor(wal/(P+24)) + F)/32768))`.
Add its excess over the existing SHM size when positive. In full:

```text
requested = 2*F*(P+24) + 32
          + max(C0*P - main, 0)
          + max(32768*(2 + floor(8*(floor(wal/(P+24)) + F)/32768)) - shm, 0)
```

The frame term is `(2 + 48/P)` times `F*P`: **2.01171875 at 4 KiB**,
**2.000732421875 at 64 KiB**. This is a multiple of accounted page bytes,
with earlier checkpoint debt and SHM added separately. It is not a fixed
multiple of actual file growth: WAL/freelist reuse can make growth zero, and
referenced clean pages are included. The optional extra-payload margin is
additive. Scheduled report checkpoints are prepared from the input
transaction's report state before its admission. The input is refused before
growth if its cached pages plus that margin do not fit. A refused boundary
input leaves database, WAL and SHM byte-identical, including repeated refusals.
Charging the cumulative payload to the boundary input is intended
(`Ledger::commit_input`, `runner/store.cpp:1099`). A boundary input can therefore
hit a storage stop when the input alone would fit, up to the cumulative
payload's reservation earlier than strictly necessary. This cost buys the
guarantee that no input commits with a checkpoint admitted without room in its
reservation.

The report checkpoint uses its own transaction after the input commits.
The writer mutex stays held between these transactions. That second
transaction admits its actual cached pages with `E=0`, against the smaller
of the configured budget and the first admission's `used + requested`.
It cannot spend beyond the original reservation. The extra-payload formula
alone does not prove that every possible checkpoint tree update will fit
this remaining ceiling. If that admission refuses, the checkpoint rolls back,
the `write_failed` diagnostic and its `count` are published, and the run
continues without latching a required-write storage stop. The committed input
remains durable and full report reconstruction remains available.
The additive diagnostic `reason` is `budget` for a refused admission, `build`
for preparation or allocation failure, or `io` for database setup/write failure.
`reason` names the most recent failure, while `budget_count`, `build_count`
and `io_count` are cumulative since the ledger was opened and `count` is their
sum. The same fields appear in the status file and stderr diagnostic JSON.
Checkpoint verification failures have null `reason` and zero reason counts.

Startup and recovery checkpoints also admit their actual cached pages before
COMMIT, using the configured budget without an input reservation. They are
optional on refusal and publish the same diagnostic. This is intended for both
startup and post-replay checkpoints (`Ledger::checkpoint_recovered_report`,
`runner/store.cpp:1225`; the post-replay call is `checkpoint_recovered_report`,
`runner/main.cpp:1108`): with a large budget, either can spend most of the
remaining headroom on an O(report) payload before the first live input. The run
can stop sooner, never over budget. Opening files already
over a newly supplied finite budget refuses with `LedgerBudgetExceeded` before
opening SQLite or attempting a checkpoint. Preparation failures skip the
optional checkpoint; the input's admission then has no checkpoint payload.

For sizing, assume no earlier checkpoint debt, no extra payload, an existing
32 KiB SHM file, and a WAL-index bound of 64 KiB:

| Page size | Accounted page bound `D` | Sector frames `K` | Required headroom `requested` |
|---|---:|---:|---:|
| 4 KiB | 32 pages | 16 | 428,320 bytes (0.408 MiB) |
| 64 KiB | 32 pages | 1 | 4,359,760 bytes (4.158 MiB) |
| 4 KiB | 512 pages, 2 MiB of resident pages | 16 | 4,383,520 bytes (4.180 MiB) |
| 64 KiB | 32 pages, 2 MiB of resident pages | 1 | 4,359,760 bytes (4.158 MiB) |

The exact minimum workable limit for a particular write is `used + requested`.
For example, with `used=1 MiB`, the first two rows require limits of
1,476,896 and 5,408,336 bytes. **64 MB (64,000,000 bytes)** is a workable
fresh-ledger budget for short ordinary inputs at both page sizes; the test
covers initialization, reports, 1/10/100-action inputs and audited delivery.
There is no fixed minimum covering arbitrary payloads or history. Add the
earlier checkpoint debt from the formula, and more WAL-index regions when
needed. A resident-page total near 2 MiB means headroom of roughly 4.2 MiB
under the stated conditions. With auto-vacuum or an unavailable cache observer,
the fallback instead needs roughly twice the entire logical database size,
plus the stated debt and sidecars.

Delivery status uses transactional `delivery_totals`, `delivery_targets` and
`delivery_state` tables. Global pending/unsent counts require one singleton
lookup. Partial indexes select pending/unsent ordinals and the earliest known
creation/attempt time per pending target. Unknown legacy times remain null.
Metrics visit pending targets; `status` reads per-target totals and at most
one indexed error row per target. Work is independent of input, backlog and
attempt history, apart from B-tree lookups and the targets returned.

Permanent storage adds one `delivery_state` row for **every action**, including
delivered and journal-only actions, plus up to three partial-index entries
for pending, unsent and oldest-pending selection. Each index contains an entry
only while its predicate holds. Target text is copied into the state row and
eligible oldest index. `delivery_targets` adds a row per distinct target,
including targets seen in historical attempts, plus its text primary-key
index and an eligible pending-index entry. A target row is shared by all its
actions. Actual bytes depend on text lengths, integer encoding, page occupancy
and overflow; these structures persist alongside the original history.

Migration builds the summaries and missing indexes once, verifies counts,
and commits them atomically. It copies summary fields and target names, not
historical input/action/report payloads. The old whole-database payload
multiplier is removed: a finite-budget upgrade uses the same cached-page
formula as other writes. Checkpoint debt from an existing WAL still counts.
If cache observation is unavailable or auto-vacuum is enabled, the whole-page
fallback honestly requires roughly `2*S` headroom for a checkpointed logical
database of size `S`, plus sector/SHM allowances and new summary pages.
The upgrade is **one-way**; back up the ledger first, since older runners
cannot open the migrated database. Read-only `status` retains its scan
fallback until `run` upgrades it. Incompatible chart-feed ledgers retain their
existing refusal. Export/recovery formats and action payloads do not change.

Verification rows compare requested bytes with real database/WAL/SHM growth
at 512, 4096 and 65536-byte pages, with records up to 800000 bytes. They
persist each page size with `VACUUM`, select no auto-vacuum, and check the
reopened size. Limits one byte below admission refuse with all three files
unchanged byte for byte, including two consecutive retries after the first
refusal. Writers also commit at the exact admission limit within their
reservation. The production-default checkpoint row shows that the observed
request belongs to the contract's allowed set, is no larger than the
logical-page (FULL) request, and admits at `used + request` with growth at most
`request`; it does not pin the estimator to the resident-page count.
A separate row releases a 32 MiB unrelated SQLite allocation
and warms different clean pages between refusals against a ledger with
12.8 MB of historical payload. The small routing write's headroom, excluding
checkpoint debt, must stay below one historical record's size. The independent
slack ceiling bounds absurd over-estimation only; it is not a tightness check.
The separate action-batch row fixes the limit at `used + 4*request(1 action)`,
commits 1, 10 and 100 ordinary actions,
and prints request, actual growth, limit and admission result. Migration rows
cover a ledger produced by the previous runner under a finite budget and a
fixture with 4 MB of historical action payload under 8 MiB of upgrade headroom,
with brute-force recounts and source-row comparisons.
