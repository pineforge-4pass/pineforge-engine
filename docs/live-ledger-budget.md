# Live ledger admission and delivery status

`pineforge-live run --max-ledger-bytes N` budgets the SQLite database, WAL and
SHM files together. The default remains 0 (unlimited) for ordinary runs.
`--status-file` enables service mode, which requires a positive budget or the
explicit `--ledger-unbounded` opt-out. A positive budget still applies when both
options are supplied. This deployment policy is controlled by the single
`service_requires_ledger_budget` constant in `runner/store.hpp`.

Admission runs under the ledger's writer mutex before the transaction begins.
It reads file sizes and SQLite page metadata, computes a conservative
reservation, and refuses if `used + requested > limit`. Saturating arithmetic
prevents overflow from admitting an oversized request. An already committed,
identical input, routing document, report or redelivery request needs no write
reservation. Initialization and upgrades reserve space before changing the
journal mode or schema. Opening also reserves missing WAL/SHM space, using the
existing WAL length and SQLite's minimum page size to bound index rebuilding
before any SQL reader can allocate it.

The refusal is `LedgerBudgetExceeded`, a `coded<std::runtime_error>` carrying
the existing `scratch_capacity_exceeded` code with `{"resource":"layout"}`.
Its separate integer fields `used`, `requested` and `limit` describe the
ledger admission; they also appear in the diagnostic JSON. They are not new
arguments in the engine's unchanged failure-code catalog. Startup policy
refusals use `run_request_invalid` with `{"option":"arguments"}` and exit 1;
storage admission refusals exit 3. An admission refusal cancels delivery and
stops the advanced strategy instance. The first refusal is latched under the
ledger mutex, so another writer thread cannot add rows while shutdown catches
up. Restart replays the committed prefix. Finite budgets disable SQLite's
implicit checkpoint on close; the durable WAL is retained for recovery, and
checkpoint work during admitted commits is included in their reservations.
If admission fails during initialization, status publication may not yet have
started. The existing readiness field `storage_below_budget` becomes false
when a running service stops for storage, including failed admission.

The reservation deliberately exceeds the expected file growth. SQLite may
reuse pages or WAL frames, split a B-tree, or checkpoint previous WAL pages into
the main file. Available disk space can therefore exceed the remaining ledger
budget when admission stops. Size the budget for this headroom as well as the
durable history; an estimate is never a promise of successful storage I/O.

The accounting uses these quantities:

| Quantity | Meaning |
|---|---|
| `B` | Sum of text byte lengths being inserted or rewritten. Input commits include input JSON, the decimal state hash, event IDs/payloads/routes/delivery IDs, the supplied report delta and its 64-byte digest. Event updates include the existing event ID, payload and error bytes. |
| `W` | Conservative number of table/index mutations, including summary triggers: 2 per input, 24 per event, 6 per report/digest pair; 4 for a legacy attempt/failure, 24 for acknowledgement, 32 for starting an audited attempt, 64 for completion, 2 for routing, 4 per redelivery request. |
| `P`, `C` | SQLite page size and logical page count. |
| `T` | `floor(8*B/(P-64)) + 1` pages for text, copies in indexes, overflow pages and rounding. |
| `H` | `floor(log2(C + T + 512*W)) + 1`, a binary-tree upper bound on height including space for growth. |
| `D` | `T + W*8*(H+2)` dirty pages. The per-row overhead covers headers, sibling balancing, parent paths and splits. |

For the physical reservation, count `2*D*(P+24) + 32` bytes for WAL frames
(24-byte frame headers), the WAL header and possible simultaneous checkpoint
growth. Add `max(C*P - main_file_bytes, 0)` for checkpointing earlier logical
pages. Reserve WAL-index regions up to
`(floor(8*(floor(wal_file_bytes/(P+24)) + D)/32768) + 2)*32768`,
less the existing SHM size when positive. Page-number and hash slots explain
the eight bytes per indexed frame; the extra regions cover allocation
rounding. Schema creation/migration additionally reserves the existing
logical database bytes and `256 + 32*events + 16*delivery_log_rows` mutations.
This is conservative per-row overhead, not measured SQLite packing density.

The ledger test exercises 512, 4096 and 65536-byte pages, short text and
overflow records up to 800000 bytes. It compares reservations against real
database/WAL/SHM growth, tries limits one byte below and above admission, and
compares all three files byte for byte after refusals. Its independent slack
ceiling for the two-event input fixture is
`32*B + 56*16*66*(P+24) + 3*32768`; it also requires actual growth to fit the
reservation and the admitted limit. The extra writer cases cover reports,
routing, legacy delivery, audited attempts/results and redelivery requests.

Delivery status uses transactional `delivery_totals`, `delivery_targets` and
`delivery_state` tables. Global pending/unsent counts require one singleton
lookup. A partial index finds the first pending or unsent ordinal, and another
finds the earliest known creation/attempt time per pending target. Unknown
legacy times remain null. Service metrics visit only pending targets;
`status` reads per-target totals and at most one indexed error row per target.
Work is independent of input, backlog and attempt history, apart from B-tree
lookups and the number of targets being returned.

Migration rebuilds these structures once from source rows and verifies counts
before committing. A failed migration rolls back its counters. The read-only
`status` command retains its old scan fallback for ledgers that have not yet
been upgraded by `run`. The pre-existing refusal for incompatible chart-feed
ledgers remains unchanged. Export/recovery report formats and event payloads
do not change. See the [runner guide](../runner/README.md#service-operation)
for lifecycle, delivery and shutdown behavior.
