# Generated object graphs on frozen2

This is the U07 runtime/compiler contract and a prototype, not a claim that
frozen2's generated scripts already use external state. Runtime declarations are
in `include/pineforge/detail/udt_graph.hpp`; the matrix integration is in
`include/pineforge/detail/matrix_store.hpp`. Lab compilation, baseline
measurements, mutation execution and parity verification have not run in this
edit-only lane.

The purpose is to retain arbitrarily old objects and arbitrarily large nested
collections with resident script state independent of bars. Losing a late value,
splitting an alias, retaining unreachable cycles, reusing a dead identity or
omitting a speculative deletion from rollback violates the contract.

## Representation

One run owner supplies one `o1::Store` over the shipped CommandScratchStore and
one explicitly chosen ScratchBacking. The graph owns no second store, cache,
backing, native node table, recursive traversal or growing undo container.

`UdtGraph` holds a Store reference and a stable control id. Its control payload is
8 bytes: the intrusive node-directory head. Each graph node is a padding-free
40-byte record: next node id, payload object id, edge-array object id, flags and
owning control id. Both the scalar payload and the ordered edge array are Store
objects. A root slot is a node flagged as a root, with one owning edge and no
scalar payload. Root slots live in the external directory as well.

Every reachable owned UDT, array/vector, string, map and matrix has a stable node
id. Arrays and strings use exact payload bytes or external edge slots; UDTs use
schema-encoded scalars plus ordered owning edges. Primitive elements can be
packed in their owning collection's payload. A matrix header and each tile are
graph nodes. Numeric matrices, bool matrices, a UDT in a map value, and a matrix
inside a UDT therefore share the same ownership rules.

Assigning a reference copies its id. Two fields that refer to the same child
must retain the same id. A value copy is a separate operation selected by the
compiler; neither serialization nor collection may turn one alias into two
objects. Zero is absent/na. A released id, including an id first allocated in an
aborted transaction, must never name a new object. Physical backing pages may be
reused; ids are not physical offsets. Reads through dead or foreign arena ids
raise an invariant error.

The runtime does not inspect a C++ struct for hidden ownership. In particular,
copying a native struct that contains `std::vector<double>` or `std::string`
does not externalize those children.

## Roots, safe points and reclamation

At a collection safe point every observable global, local, escaped temporary,
historical reference, map value and nested collection is reachable from a root.
The compiler creates and retires root slots according to liveness. An unrooted
id is a borrow that expires at the next safe point.

Collection initializes marks from root flags, scans the external directory to a
fixed point, then unlinks and releases every unmarked node and its private
payload/edge objects. A marked node is scanned once. Duplicate edges mark one
node; unreachable cycles receive no root mark and are reclaimed. No native
reference count, work queue or visited set grows with the graph. Successful
collection resets all traversal flags to their root-only state.

This is exact tracing, with time O(V * depth + E) in the worst case; it is not
an O(1) time claim. After collection no unreachable graph node remains live.
Backing free-space reuse, index reclamation and undo retention are Store
obligations, separately from the graph's exact logical release.

The compiler must bound allocation work between safe points independently of
bars, including inside allocation loops. Reaching a scheduling interval triggers
collection, never a capacity refusal. Root completeness is required before the
runtime can collect; collection inside an operator that still has unrooted
temporary children would be incorrect.

The run owner calls `close()` only after all handles, borrows and transactions
have expired. There is no hidden arena teardown while native views still borrow
it. Releasing a C++ matrix handle drops its temporary root; an external owning
edge can keep that matrix alive.

## Versions and calc-on-fill

An outer `Store::begin()` versions payload bytes, lengths, edges, roots,
directory links, allocations and deletions together. `UdtGraph::begin()`
delegates to it. Before-images and reclamation metadata stay in the same backing.
Rollback restores the exact pre-execution graph, including a node collected
during speculation and any physical pages reused afterward. New speculative ids
become dead. Native speculative id caches must be discarded.

The generated hooks commit the preceding accepted execution before beginning the
next checkpoint. Restore rolls back the open transaction, reloads native borrows
from restored roots and begins another transaction against the same checkpoint.
The ordinary close execution becomes the next committed checkpoint. Broker
orders and trades remain outside script-state rollback.

The object layer's joined transaction scopes are not savepoints: committing a
joined scope does not publish an independently restorable version. The prototype
requires one checkpoint owner per Store.

C++ handle variable assignment is not a Store write. Generated globals and
collection elements must store matrix/UDT ids in versioned root/edge slots and
reload borrowed views after restore. Merely keeping a native NumericMatrix or
shared_ptr member and rolling back the Store cannot undo rebinding of that
native member. `TileMatrix::borrow(runtime, id)` is the allocation-free view for
an identity reached through a restored owning slot. It reads current dimensions
from the external header and retains no speculative shape cache.

For compatibility, matrix `snapshot()` owns separately rooted copied tiles in
the backing and restores its original C++ handle identity. Those roots are
reusable in any order. They copy value records, while reference elements keep
their original child ids. Mutable reference children require the enclosing graph
transaction or generated recursive checkpoint policy. A matrix snapshot created
inside an outer transaction is itself speculative; it cannot survive rollback
of its allocation. Generated whole-graph checkpoints must use the Store
transaction protocol rather than treating these two APIs as nested versions.

Independent immutable historical graph versions are not supplied by the Store
transaction API. Old objects that remain reachable as current references are
supported. Independent version reads would need a separate pin/read/drop API.

The arena has one rollback policy. Splitting rollback-exempt state into another
Store is valid only for disjoint ownership domains. Mixed-policy aliases need
an alias-preserving field/version policy in the compiler; copying a shared child
to separate policy domains changes semantics and is forbidden.

## Compiler/runtime boundary

| Compiler supplies | Runtime supplies |
| --- | --- |
| Exact scalar encoding, field schema and ownership edges for every transitive child | Stable ids, exact bounded range reads/writes and external resizing |
| Reference assignment versus value-copy semantics, defaults, ordering and alias analysis | Shared edges preserve identity; tracing visits an alias once |
| Complete roots, local/escape liveness, retirement and safe points | Versioned root slots, fixed-space tracing and cycle collection |
| A bound on native activation, simultaneous borrows and operator workspace | Fixed graph records and 256-byte graph chunks, with no native graph directory |
| Streaming collection/string/matrix lowering; reload native caches on restore | External graph and matrix metadata restored by the Store |
| Snapshot hooks, graph digest integration and run-owner lifetime | Transaction delegation, exact logical release and explicit close |

Reachable width may be unbounded. That selects external payloads/edges, never a
cap, truncation or refusal. A native specialization is valid only with a
bar-independent bound on its entire transitive payload and simultaneous
checkpoints. Bounding only the UDT's outer fields is insufficient.

`hash_state()` folds the control id/head, each directory id and node record,
root/tracing flags, payload/edge sizes, ordered edges and every exact payload
byte. The compiler folds this digest into its script-state digest and hashes
native certified state separately. Store allocation/free-space/version metadata
belongs to the Store's own durable-state contract.

## Prototype and evidence limits

The growth fixtures run the public PineStrategyHost/kernel over deterministic
100,000 and 1,000,000 bar inputs. The vector case retains one new UDT and one
nested numeric vector on every bar, then reads every old value late. The string
case grows an owned UDT field by eight exact bytes per bar, then reads every byte
in 256-byte chunks. Collection precedes late reads.

On frozen2 the same growth fixtures select ordinary generated-struct analogues,
with real owning vectors/strings, and test the same derived memory predicates.
The alias negative control uses the shipped matrix<shared_ptr<Child>> shallow
snapshot and observes the mutated shared child after restore. This demonstrates
why a compiler needs graph checkpoints; it does not redefine matrix snapshots
to be deep copies of reference elements.

Separate fixtures exercise same-child aliases, cycles, dead reads, nonreuse of
aborted ids, exact logical live-byte accounting, ordered integer map values,
matrix references to UDTs, UDT ownership of a matrix after native handle
retirement, and deletion/rollback inside the real fill scheduler.

The source tests are handwritten generated-code analogues. Actual compiler
analysis/lowering, production run-owner attachment, mixed rollback policies,
physical backing reuse and full trade parity remain unverified. No baseline
measurement, prototype PASS or fail-before compiler diagnostic is claimed here.
