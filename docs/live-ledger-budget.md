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
bound the pages that COMMIT can write, counting shared pages once. A refusal
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

A refusal cancels delivery and stops the advanced strategy instance. It is
latched under the mutex so another writer cannot add rows during shutdown.
Restart replays the committed prefix. Finite budgets disable the implicit
checkpoint on close, retaining the durable WAL for recovery. Checkpoints
during admitted commits are included in admission. Initialization can fail
before status publication starts. A running service's
`storage_below_budget` readiness field becomes false on a storage stop.

The physical bound uses these quantities:

| Quantity | Meaning |
|---|---|
| `P` | SQLite page size. |
| `C0`, `C1` | Logical page counts before BEGIN and after the cached SQL changes. |
| `M` | SQLite's 64-bit outstanding allocation total, from `sqlite3_memory_used()`, sampled after all SQL changes. |
| `D` | `min(C1, ceil(M/P))` when allocation tracking is enabled and auto-vacuum is off; otherwise `C1`. |
| `E` | Optional `extra_payload_bytes` margin supplied by another writer; zero for the current writers. |
| `A` | Zero when `E=0`, otherwise `floor(8*E/(P-64)) + 4` additional pages. |
| `K` | `ceil(65536/(P+24))` extra WAL frames for sector padding. |
| `F` | `D + A + K` accounted frames. |
| `main`, `wal`, `shm` | Current physical file sizes; `used = main + wal + shm`. |

Every dirty page has a resident full-page buffer when spilling is disabled.
SQLite's outstanding allocation total includes these buffers as well as clean
pages, statement allocations and pager overhead. The standalone runner does
not install an external page-cache allocator. Thus both `C1` and `ceil(M/P)`
bound the dirty page count. This includes all tables, indexes, parent paths,
splits, freelist work, overflow/text pages and trigger writes at their actual
page sizes. There is no guessed tree height or per-mutation path multiplier.
Auto-vacuum may dirty additional pages at COMMIT, so it uses all logical pages.
Disabled allocation tracking uses that same conservative fallback.

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
clean cached pages are included. The optional extra-payload margin is
additive; a writer must still put its SQL changes in the admitted transaction.

For sizing, assume no earlier checkpoint debt, no extra payload, an existing
32 KiB SHM file, and a WAL-index bound of 64 KiB:

| Page size | Accounted page bound `D` | Sector frames `K` | Required headroom `requested` |
|---|---:|---:|---:|
| 4 KiB | 32 pages | 16 | 428,320 bytes (0.408 MiB) |
| 64 KiB | 32 pages | 1 | 4,359,760 bytes (4.158 MiB) |
| 4 KiB | 512 pages, a 2 MiB allocation bound | 16 | 4,383,520 bytes (4.180 MiB) |
| 64 KiB | 32 pages, a 2 MiB allocation bound | 1 | 4,359,760 bytes (4.158 MiB) |

The exact minimum workable limit for a particular write is `used + requested`.
For example, with `used=1 MiB`, the first two rows require limits of
1,476,896 and 5,408,336 bytes. **64 MB (64,000,000 bytes)** is a workable
fresh-ledger budget for short ordinary inputs at both page sizes; the test
covers initialization, reports, 1/10/100-action inputs and audited delivery.
There is no fixed minimum covering arbitrary payloads or history. Add the
earlier checkpoint debt from the formula, and more WAL-index regions when
needed. An allocation total near 2 MiB means headroom of roughly 4.2 MiB
under the stated conditions. With auto-vacuum or disabled allocation tracking,
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
multiplier is removed: a finite-budget upgrade uses the same page/allocation
formula as other writes. Checkpoint debt from an existing WAL still counts.
If allocation tracking is disabled or auto-vacuum is enabled, the whole-page
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
unchanged byte for byte; a limit just above admission commits within its
reservation. The independent slack ceiling bounds absurd over-estimation
only; it is not a tightness check. The separate action-batch row fixes the
limit at `used + 4*request(1 action)`, commits 1, 10 and 100 ordinary actions,
and prints request, actual growth, limit and admission result. Migration rows
cover a ledger produced by the previous runner under a finite budget and a
fixture with 4 MB of historical action payload under 8 MiB of upgrade headroom,
with brute-force recounts and source-row comparisons.
