# Tail and curve read windows

The internal C++ constructors accept per-instance payload capacities. Ordinary
callers keep the existing payload defaults: 64 KiB for position-tail pages,
64 successor-root rows, 64 group-effect receipt rows, 16 long-chain index
rows, and 2,730 complete 24-byte curve points. These read caches use
`detail::ReadThroughCache`, with direct mapping and a read-only backing port.
Owner writes retain their existing ordering and invalidate read tags before
changing backing. The retained-sequence backing keeps its separate 64 KiB
payload default for its shared resident-prefix/read-cache allocation.

These capacities change resident storage and backing traffic. They do not
change retained history, identifier widths, command order, logical record
counts, curve points, or report mode. `strategy_set_cache_policy_v1` and
`BacktestEngine::set_cache_policy` supply inclusive byte allowances to the
admitted run. The payload/row constructor overloads remain available for
standalone C++ owners.

`PositionTailSpill` streams both its 30-byte backwards footers and
arbitrary-length entry identifiers across pages of at most 4,096 bytes.
Allowances below one page select that smaller geometry; larger allowances
keep complete 4,096-byte pages. Its fixed transfer buffer is 4,096 bytes.
The final page reads only its existing prefix. Zero and optional-allocation
failure read backing through the shared cache's bypass path. Append and
clear invalidate tags before I/O. The footer codec and newest-first
traversal order are unchanged. The typed `CacheCapacity` overload uses fixed
4,096-byte pages; its default is 16 complete shared lines, preserving the
64 KiB payload. The host never converts that allowance using 64 KiB geometry.

`WorkingRequestCore` accepts `TailWindowConfig` at construction. The existing
one-argument constructor keeps the original policy. Move and reset preserve
the selected policy; the lazy stores receive it when they are created.
Its native stores retain their authoritative pending batches: replacing
these with evictable read lines would put writes into nonthrowing
installation or change failed-write retry ordering. Preparation still
flushes half of the receipt/chain batch or the full root batch. Zero and
pending-allocation failure retain one mandatory pending record per store.
Preparation spills that record before another nonthrowing installation.
Committed rows use shared read caches with the selected row capacities.
Pending storage and read-cache storage coexist and are reported separately.
The row setting is not an inclusive byte allowance for their sum.
The typed `WorkingRequestCore` overload instead accepts three independent
`CacheCapacity` values for committed root, receipt and chain-index reads.
Host policy uses this overload. The pending batches keep the internal
64/64/16-row defaults at every host cache size, with the same allocation
fallback and prepare-only flush rules. Zero disables the read cache without
changing pending workspace, retained rows or slab admission. Move and reset
preserve both policies in the opaque journal owner.

`TailLog` accepts absolute indices in `[base(), size())`. A read below
`base()` is a caller error following explicit history retirement. It keeps
the existing `out_of_range` error. A retained row that has left the resident
tail is still read from backing. Cache replacement never advances `base()`.
Backing I/O, corrupt records, slab capacity and arena capacity keep their
existing failures.

The retained sequence index is derived from retained placements. Its
constructor forwards one typed allowance to the command-backing factory,
preserving the 512-byte geometry and two mandatory transaction slots.
`PlacementTable` retains that typed value until lazy index construction.
The factory derives complete payload pages once, then uses the same slab
for both the initial disk prefix and the shared read cache after spill.
Zero bypasses both phases. The arena branch disables the prefix and uses
the selected read cache. The existing placement constructor with two byte
arguments still accepts payload budgets, adding the exact shared line charge once.
The first placement defers index creation; a later mutation or reader may
build it. Configuration does not alter rebuilding or query ordering.

`ScratchCurveWindow` rounds its payload allowance down to complete points.
Each point is a 24-byte shared-cache page. Zero, a sub-point allowance and
optional-allocation failure bypass. A returned point is a copy, so callers
can retain it across later reads. A failed read publishes no successful tag
and can be retried after backing repair; unrelated valid lines remain valid.
The caller still rebinds readers after replacing or clearing a region.
Its typed overload consumes the inclusive allowance directly; the ordinary
report-sink metric reader receives the admitted `equity_curve_pages` value
through that overload.

The curve writer's 2,730-point pending batch remains mandatory authoritative
write workspace. Its failed-flush retry and suffix-replacement contracts
remain independent of the read-cache allowance.

For position, curve and native read caches with page size P and payload
allowance C, optional shared-cache storage is
`floor(C/P) * ReadThroughCache::line_bytes(P)`. The line charge includes all
payload, keys, pins and replacement metadata. Fixed owner control and
transfer/commit workspace are additional. The position reader defaults to
16 lines of 4,096 bytes; the curve reader defaults to 2,730 lines of 24
bytes. The curve therefore adds one metadata charge per point, rather than
pretending that all 2,730 records share one tag.

Native pending storage additionally uses `rows * record_bytes` when the
optional batch exists, plus one inline fallback record per store. At the
64/64/16 defaults this is 3,456 optional pending bytes plus 72 inline bytes;
the three shared read caches add another 3,456 payload bytes and their line
metadata. These extra read caches preserve the pending-write contract but
increase default residency. The host allowances include every read-line
charge; the fixed pending batches are separate transaction workspace. They
must be added when accounting for the complete store.
The retained-sequence factory instead reuses one allocation for its initial
prefix and subsequent read cache.

Physical read observations are taken at `ExtFile::read`, successor-root
file/slab reads, the position-tail file facade, and `read_scratch_points`.
Raw-descriptor curve reads count actual `pread` attempts. Sequence-index
tests use the factory's independent physical observer at the inner file/arena
operation boundary; their cold-read proof counts completed backing reads.
Cache hit/miss counts and logical charged-I/O counts cannot establish that a
cold read reached backing.
Separate read-only cache statistics expose allocated payload and metadata;
focused position/curve tests compare these bytes with global allocation
hooks. These observations do not establish whole-engine memory flatness.
File/arena provider allocations and explicit output projections remain part
of the supervisor's complete allocation census.
