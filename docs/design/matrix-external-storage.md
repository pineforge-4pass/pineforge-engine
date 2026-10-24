# Matrix storage and mapping on frozen2

NumericMatrix and mapped GenericMatrix can use a run-owned external arena.
Construct `matrix_store::Runtime` from the owning object layer's Store, install
`ScopedRuntime` before constructing the script/host and keep the Store, Runtime
and scope alive through all matrix handles and borrows. The runtime neither
chooses a medium nor creates a process-global backing.

Without attachment, existing constructors retain native compatibility. Unknown
UDT types also retain their native representation. `is_external_storage()`
reports the selected representation; a native fallback is never counted as
external. The production codegen/run-owner wiring is a separate integration
obligation, not a completed property of all shipped scripts.

## Layout and bounded operations

A matrix identity is a graph node whose 32-byte payload stores rows, columns,
record width, owning-slot count and cells per tile. Its ordered edges form the
tile directory. Each tile has

```
cells_per_tile = max(1, 4096 / record_bytes)
payload_bytes  = cells_per_tile * record_bytes
edge_slots     = cells_per_tile * owned_slots
tile_index     = flat_cell / cells_per_tile
byte_offset    = (flat_cell % cells_per_tile) * record_bytes
flat_cell      = row * columns + column
```

Rows can cross arbitrarily many tiles; there is no row-width or dense-window
capacity refusal. A fixed schema record wider than 4096 bytes occupies one tile
and is moved in 4096-byte chunks. No tile directory or row buffer grows in the
native heap.

Cell reads/writes, row/column insertion/removal and swaps, row-major reshape,
reverse, transpose, copy, submatrix and external concat use scalar/record moves.
Growth adds tiles and directory edges; shrink removes edges and clears every
removed owning slot in the surviving boundary tile. Numeric reverse reverses
both axes; generic reverse reverses rows only, preserving frozen2's distinction.
Numeric 0xN dimensions retain N; generic zero-row matrices report zero columns
and adopt the first added row's width, as before.

Snapshots copy records into separately rooted tiles in the same backing and
retain the original matrix handle. Restore writes the saved state into that
identity, so aliases observe rollback after receiver rebinding or na assignment.
Several roots can coexist and each can restore repeatedly. This uses fixed
native control blocks and fixed record-copy scratch, not a cell-sized heap copy.
The graph contract describes the additional transaction policy for mutable
reference children.

`external_identity()` exposes the graph header id only for an external handle.
Generated nested collections store that id in an owning edge and use
`TileMatrix::borrow(runtime, id)` for allocation-free temporary access. The
borrow owns no root and must not cross an unrooted collection safe point.

## Generated element mapping

Specialize `detail::matrix_store::codec<T, void>` before the first matrix<T>
instantiation. The surface is:

```
mapped                              // true only for an explicit complete mapping
record_bytes                        // fixed, nonzero record width
owned_slots                         // fixed schema arity; not a payload-size cap
encode(Encoder&, const T&, unsigned char* record)
decode(const Decoder&, const unsigned char* record) -> T
owned(const unsigned char* record, uint32_t slot) -> Id
```

Every record byte must be deterministic, including padding. Every owning slot
must name a child graph node or zero. There is no fixed-capacity root scan that
can silently omit later fields. Native pointers and allocator-owned bytes must
never be serialized as ownership.

Arithmetic/enum elements are raw fixed records; bool uses one byte rather than
vector<bool> proxy storage. std::string is a length and immutable graph payload
id. Encoder::put creates an immutable value leaf; decoder copies its exact bytes.
The nested-value fixture explicitly maps the shipped `vector<double> history`
shape to a fixed descriptor plus immutable string/history leaves. Its test
histories have four elements and labels have 48 bytes.

A generated reference UDT instead stores its stable graph id in the record and
lists that id as an owning slot. Decoder::graph supplies the borrowed arena for
an id-based view. This preserves shared children and supports arbitrarily large
nested vectors/strings through scalar/chunk access. The reference matrix fixture
demonstrates this mapping and whole-graph rollback after deleting the UDT and its
vector during speculation.

A native-value decoder returning a whole unbounded std::string/vector is not a
bounded operator. Codegen must use id-based views or prove a complete transitive
native width before choosing that compatibility surface. A value codec does not
substitute for reference identity semantics.

## Workspaces still NOT DONE

This lane does not complete the requirement that every algorithm have fixed,
preallocated native workspace. `dense()` gathers into a cell-sized Eigen matrix
so the original Eigen expressions and packet/fold choices remain available.
Products, powers, reductions, approximate properties, LU/COD/inverse and both
eigensolvers still have operand-sized dense/internal temporaries. Numeric mode
still uses std::map; both sorts retain their native algorithm/storage.

The reference-returning `data()` and `row_ref()` preserve persistent native
addresses by materializing native storage, and report that escape. The
vector-returning row()/col()/eigenvalues() APIs also materialize their outputs.
Streaming `for_each_row`/`for_each_col` avoid whole-row/column result allocation;
their visitors must also obey the compiler's native-width certificate.

No fixed workspace limit, refusal or silent truncation was added in place of
those missing algorithms. The numeric oracle compares result bytes with the
same frozen2 Eigen expressions, including cancellation-heavy reductions and
both eigensolver branches. It is an authored regression oracle, not an executed
equivalence receipt or a workspace proof.

Fixed-size handle/control allocations and native compatibility T copies also
remain. The graph and borrowed tile view avoid a growing native directory; this
does not establish the stricter requirement of zero allocation on every bar for
all compatibility APIs. The complete U05/U06 finding remains partial.

## Resident bound and validation

The fixture shape uses 64 resident cache pages of 4096 bytes, a 1024-byte object
chunk and fixed store staging/workspace metadata. The test's conservative bound
is derived as

```
(64 + 6 + 1 + 4) * 4096 + 64 * 32 + 128 * 4 + 1024 + 16
    + sizeof(family)
= 310800 + sizeof(family) bytes
```

The six work pages come from ScratchStore's layout; one page bounds fixed
metadata, four cover fixed graph/matrix chunks, slots are at most 32 bytes each
and 128 four-byte buckets index 64 cache slots. No term depends on bars or cells.
Graph records are 8/40 bytes; matrix headers are 32 bytes. The owning store
remains responsible for its full residency and physical backing proof.

Linux growth rows sum signed mallinfo2 live-heap changes around actual family
operations in the public host callbacks; this includes Eigen malloc/mmap bytes
and excludes OHLCV and unrelated host state. They reach 100k/1M bars before
checking the bound and read every retained old value after collection.
Calc-on-fill rows separately observe C++ allocation requests, retained libc heap
and exact values/aliases. These measure userspace heap, not OS file-cache charge.

All builds, RED/GREEN measurements, mutation executions and parity checks are
NOT RUN in this edit-only lane. The supervisor must compile the new rows first
against frozen2's header closure, record their real first diagnostics/behaviour,
then verify the integrated object-layer tree. The INTERIM declaration/source
binding is listed in REPORT-a5matudt.md and is not a second production store.
