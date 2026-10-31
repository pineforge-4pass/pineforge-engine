# TA window cache capacity

The C++ window stores accept `WindowCacheCapacity`, measured in pages of 1024
numeric values. This is separate from a Series's logical history length, a TA
window length, and Pine's `max_bars_back`. Those semantic lengths and their
existing errors are unchanged. A missing cache page reads the backing; it never
means an `na` value, absent history, or an invalid length.

The default, `WindowCacheCapacity{}`, keeps the logical window in memory as
before. A finite capacity smaller than the window selects scratch backing and
a direct-mapped read cache. Zero selects scratch reads without a cache. A
capacity large enough to hold the logical window keeps the owning memory
representation. No setting introduces a maximum number of values or backing
bytes.

Scratch-backed numeric reads use `detail::ReadThroughCache`. The page-count
parameter converts to an inclusive optional allocation of
`pages * ReadThroughCache::line_bytes(1024 * sizeof(T))`, including every
payload and tag. Unrepresentable optional byte budgets reduce the number of
cache lines without limiting the retained data. Fixed owner control and
transfer workspace are separate.
There is no private numeric cache layered over the shared component.

```cpp
pineforge::Series<double> prices(5000, {1});  // 5000 history slots, one cache page
pineforge::source::SeriesHighest highest({1});
pineforge::ta::WMA average(2057, {1});

// Also applies to windows constructed by composite indicators in this scope.
pineforge::WindowCacheScope cache({1});
pineforge::ta::Highest conditional_high(2057);
```

The scope's default is thread-local and is latched by constructors. Existing
objects retain their original capacity after a scope enters or exits. This
initial C++ interface covers arithmetic Series/DynamicRingBuffer values,
HeldHistory, the fixed extrema ring and WMA, including composites using those
rings. Other TA deque and ranking buffers retain their existing representation.
String and object-handle Series retain owning typed storage; their object
identity and copy semantics require typed backing codecs before they can be
evicted. The numeric page setting is not a bound on those owning objects.
The integrated host policy's `PF_CACHE_KIND_TA_HISTORY_PAGES` entry supplies
an inclusive byte allowance to windows constructed inside a run. The host
scope converts it to complete 1024-double lines for the numeric TA windows;
the default `SIZE_MAX` retains the owning representation. The standalone
page-count constructors remain available.

Reads return copies. In-memory pages are shared between checkpoints until
written. Scratch-backed checkpoints share immutable backing until the next
write clones it through a fixed 1024-value transfer buffer. Cache lines and
statistics belong to an object and are discarded on copy construction or
copy assignment; they do not enter the logical series or continuation state. The history ring
still retires only complete 1024-value chunks according to the existing
retention rule. Each window/cache instance has one executing owner; distinct
checkpoint copies serialize shared ScratchFile access to protect its I/O state.
Moves reconstruct the cache lazily with the destination's callback context;
they retain cumulative owner statistics and discard warm tags. Cache callbacks
carry backing exceptions across the shared component's `noexcept` status port
and immediately rethrow the original exception, preserving its dynamic type,
errno and structured failure fields.

Scalar writes invalidate the affected shared-cache page before writing the
existing scalar span directly to backing. They do not fetch or rewrite an
entire page. A failed or partially completed write cannot leave a successful
cache tag. Cloning a checkpoint preserves other cached pages because those
bytes are copied unchanged.

Scratch backing uses the existing anonymous-file or attached-arena provider,
including its directory, confinement, operating-system cache and typed error
policies. Cache allocation failure bypasses an optional read cache. Backing
I/O, backing allocation and actual arena exhaustion still fail through that
provider. Default owning storage can still fail to allocate its authoritative
values. A page capacity is not a promise of unlimited physical backing.

`cache_stats()` exposes shared-cache hit, eviction and bypass counts, plus
independent inner-backing attempted and successful transfer counts. Attempts
increment at each ScratchFile read/write call, including calls that fail; clone
transfers count there too. They are separate from the cache's own statistics
and are not operating-system syscall counts when the provider uses an arena
or retries a partial transfer. The Linux fault fixture also observes actual
`pread`/`pwrite` attempts. Payload, known metadata and maximum transient transfer
workspace are reported separately. The known metadata figure includes inline
control state, page pointers, vectors, cache tags and the backing wrapper; it
excludes allocator and shared-ownership control-block overhead and any
provider-owned arena directory. It is not a whole-process heap measurement.

`WindowValues::read_cache_policy()` and `read_cache_stats()` expose the actual
shared policy and statistics for adapter verification.
`DynamicRingBuffer::read_cache_stats()` forwards those same shared statistics;
its current payload plus metadata counts only optional lines and must fit the
configured byte allowance. `cache_stats()` retains the broader owner totals
described above, including fixed control and the backing wrapper even in
bypass. The shared control object is inline even for an owning window and is
already counted in owner metadata. This increases fixed object overhead; no
default memory or throughput improvement is claimed.

Each scratch-backed read copies an exact full or partial-tail page through a
fixed 1024-value transfer buffer. Bypass reads also use that page span; the
previous adapter used a scalar span in bypass. Physical read bytes can
therefore change while the result and scalar write ordering stay the same.
No logical charged-I/O observer is added or removed by this adapter. All
backing transfers still use the existing ScratchFile/provider path and its
operating-system cache policy.

The TA acceptance row compares zero, one, two, default and 64-page settings.
Its workload exceeds 64 pages, so all finite settings exercise backing reads.
The default keeps owning memory and must perform no backing I/O. The row uses
the common acceptance transcript comparison, broker replay and allocation
census, with that explicit default-representation assertion. The existing TA
fixture sweeps continue comparing every numerical and cadence result bit.
