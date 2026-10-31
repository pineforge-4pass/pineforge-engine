# Feed projection readers

Native daily calendar projections and recorded request tapes use random-index
copy-out readers. The three calendar columns are `trade_day`, `week_open` and
`month_open`. A recorded tape contains chart-open timestamps and their exact
binary64 values in increasing timestamp order.

The optional cache budget controls resident page payload and page tags. It
does not limit bars, tape rows or backing extent. The internal default is
256 KiB for each calendar column and each recorded tape. Payload pages hold
up to 4096 bytes of whole records; tags are additional bytes charged to the
same budget. `FeedProjection<Row>::page_bytes()` gives the inclusive size of
one slot for that record type. Allocated slots never exceed either the
budget or the number of pages in the feed.

Page storage and direct mapping use `ReadThroughCache` with a write-through
policy. The immutable reader supplies the backing; production finishes before
any read cache is published. Cache lines are allocated lazily on the first
read. Each line includes the shared cache's complete identity, generation,
prefix-length, pin and replacement metadata.

Zero bytes, a budget smaller than one slot, or failure to allocate optional
cache storage selects provider reads through the shared cache's bypass path.
A cold valid index reads an aligned page of complete records, shortened to
the exact final-page extent. Provider errors propagate with their original
exception type; a cache miss does not mean a missing bar. Every access copies
into caller storage, so subsequent reads, eviction and cache resizing cannot
invalidate an earlier returned value.
Readers are single-thread-owned; immutable providers may be shared when
their implementations support the callers' access pattern.

The C++ entry points are:

- `FeedReader<Row>` supplies the immutable row count and
  `read(first, destination, count)`, addressed by logical bar index. `at(index)`
  copies one row. A different feed snapshot requires a new reader.
- `BorrowedBarFeedReader` bridges existing caller-owned `Bar` and exclusive
  close-time arrays. It takes no ownership and makes no retained copy. The
  caller keeps both arrays unchanged and alive for the reader's lifetime.
- The seven-argument `build_native_day_partition` overload accepts optional
  cache bytes for **each** calendar column. The existing overload selects
  the internal default. Native engine preparation forwards the admitted
  `feed_calendar_pages` allowance unchanged; it is already inclusive.
- The five-argument `PineStrategyHost::set_recorded_series` overload accepts
  inclusive optional cache bytes for one tape. Like the existing four-argument
  setter, it copies the caller's timestamp and value arrays before returning.
- `PineStrategyHost::set_recorded_series_reader` retains a shared immutable
  `FeedReader<RecordedSeriesRow>` snapshot, validates increasing timestamps
  and folds the same logical bytes as the copying setter. A replacement is
  published only after validation and the complete candidate digest succeed.
  Its three-argument overload accepts the same inclusive per-tape budget.

An explicit C++ tape capacity takes precedence over `recorded_series_pages`
at every run preparation, including explicit zero and an explicit 256 KiB.
The four-argument copying setter and two-argument reader setter instead
follow the admitted run policy; before preparation they use the internal
default. Replacing a tape without a capacity restores that inheritance.
A rejected replacement preserves both the installed rows and their capacity
selection. An idle policy change takes effect on inheriting tapes at the next
preparation without resizing their previous caches. No policy bytes are added
to a per-tape budget, and page tags are charged exactly once.

The default producer writes complete rows sequentially to an unlinked
`ScratchFile`, with one mandatory transfer page per producer. Publication
explicitly finishes the final partial page. Destruction performs no flush.
Ordinary file failures retain `scratch_io_failed` and errno. An attached
arena retains its existing typed backing and exhaustion failures; arena
backing is still memory. Selecting a smaller cache cannot increase the
logical data that fit in a fixed arena.

Cache statistics report reads, hits, misses, evictions, bypasses, allocation
bypasses, provider bytes and errors, and current/peak optional resident
bytes. The positional scratch provider separately counts physical transfers.
`cache_stats()` exposes payload and metadata separately for the current cache
instance. Projection statistics retain totals and peaks across capacity
changes. Logical read calls include hits; backing counters count only actual
provider operations.

The fixed reader/control objects, the producer's transfer page and one
`page_payload_bytes()` transfer page per active read are separate from the
optional budget. `transfer_workspace_bytes()` reports the reader transfer
page. Copies share the immutable provider and start with a cold cache. Moves
also reconstruct a cold cache so borrowed callback context always names the
current owner. Cache contents, addresses, sizes and statistics are excluded
from logical hashes.

This change bounds the three derived calendar columns and each recorded
tape's optional pages. Native stamp and late-open vectors, symbol-calendar
metadata/session arrays, mutable security evaluator buffers, and owned bar,
close-time and column arrays still have their existing storage. Raw-array
entry points retain their existing ownership. The integrated
`strategy_set_cache_policy_v1` uses `PF_CACHE_KIND_FEED_CALENDAR_PAGES` for
each calendar column and `PF_CACHE_KIND_RECORDED_SERIES_PAGES` for each tape.
The recorded-series kind supplies the run default for tapes installed without
an explicit C++ capacity. Tape settings are applied at run preparation, before
evaluation. This is not a bound on total strategy or feed memory.
