# Complete scratch facts contract (scope A) {#scratch_facts}

This is the internal F contract for later lanes.
It adds no public descriptor or export and does not activate a new execution path.
The existing P0 policy and refusal behavior stay in place until F, V and H land
together, as described by the unchanged `default_scratch_manifest` include/pineforge/detail/scratch_defaults.hpp:15

`ScratchVocabularyFacts` owns five immutable binary-string rosters in S,F,E,H,J
order: possible source IDs, from_entry strings, exit IDs, cohort arguments, and
eligible entry/order-fill IDs. Each roster has its own completeness flag and any
unproved roster makes the certificate incomplete. Include the empty from_entry
whenever reachable, all finite alternatives and reachable engine-created labels.
An empty roster differs from one containing the empty string. Construction sorts
by unsigned bytes and deduplicates through `canonicalize` include/pineforge/detail/scratch_facts.hpp:100

Let a=|S|, f=|F|, s=sum of S byte lengths, t=sum of F byte lengths, and x=sum of
E byte lengths. These are effective-program facts, independent of bars and total
replacements. The table-only result has the existing `ScratchManifest` fields
include/pineforge/detail/scratch_tables.hpp:15

| Domain | Required capacity calculation | Implementation anchor |
| --- | --- | --- |
| q | K_q=B_q=0 | `K_q` include/pineforge/detail/scratch_tables.hpp:17 |
| Cohort | U=\|H\|, B_U=sum of H byte lengths | `cohort` include/pineforge/detail/scratch_facts.hpp:183 |
| e | K_e=a(1+f), B_e=(1+f)s+a*t, with single X distinct from pair (X,empty) | `erased_capacity` include/pineforge/detail/scratch_facts.hpp:154 |
| c | Count and sum distinct exit_id + 0x1f + from_entry byte strings, so K_c<=\|E\|f and B_c<=f*x+\|E\|t+\|E\|f. Separator-containing component pairs that join identically count once. | `joined` include/pineforge/detail/scratch_facts.hpp:209 |
| Rank | V=min(27,\|J\|), B_V=sum of the V largest distinct J string lengths | `largest` include/pineforge/detail/scratch_facts.hpp:218 |

`LiveBounds` supplies proved Lw working definitions, Ll filled opening incarnations
with live lots, M cohort root memberships, P other atomic handle leases, and W
additional preparation leases. Each number may be absent and proved zero is
present. A proof requires a closed future-query surface and includes every handle
inside pending/authority/source worksets. An unrestricted native observer or a
bar-dependent estimate supplies no such proof, per the contract comment for
`LiveBounds` include/pineforge/detail/scratch_facts.hpp:247-255

Before semantic begin, after effective inputs/overrides, `decide_scratch_backend`
returns external_history with no finite plan if any roster or live proof is
incomplete. Otherwise it derives root_slots=Lw+Ll+M+P+W and
alias_slots=Lw+Ll+P+W, plus the table manifest. Arithmetic overflow and allocation
failure are typed errors with no partial plan and never authorize bounded use,
as returned by `decide_scratch_backend` include/pineforge/detail/scratch_facts.hpp:268

The legacy R still means all rooted successors. The finite plan leaves R=0 and
carries root/alias slots separately, so it must not be passed to the public v1
setter to activate the old historical-root backend. H must bind the new internal
root pools and check their actual compiled byte layouts via `ScratchFinitePlan`
include/pineforge/detail/scratch_facts.hpp:254

The wire-format comment specifies version byte 1 followed by S,F,E,H,J sets.
Each set is a complete byte (exactly 0 or 1), an unsigned 64-bit little-endian
count, then count entries of unsigned 64-bit little-endian length and raw bytes.
Entries are strictly sorted by unsigned bytes, with no duplicate, padding or
trailer. NUL, empty strings and 0x1f remain ordinary bytes. LiveBounds is separate
from this certificate and its version is independent of the public descriptor's
version, as declared by `kScratchFactsWireVersion` include/pineforge/detail/scratch_facts.hpp:292

`decode_scratch_facts` validates the entire wire without allocating before copying
owned bytes, rejects unsupported versions, truncation, bad flags/counts/lengths,
overflow, duplicates, disorder and trailing bytes, and returns a typed error
without throwing or publishing a partial certificate
include/pineforge/detail/scratch_facts.hpp:385

A worked vector for producers in other languages, typed by hand from this format
and asserted by `golden_wire_vector` tests/test_scratch_facts.cpp:309

```text
S={"ab"} F={""} E={"x"} H={} J={"q"}, every set complete, 82 bytes in hex:
01
01 0100000000000000 0200000000000000 6162
01 0100000000000000 0000000000000000
01 0100000000000000 0100000000000000 78
01 0000000000000000
01 0100000000000000 0100000000000000 71
```

Hash and parity statement for scope A. This contract changes no state-hash leaf,
no parity digest and no pin, and no byte of any existing hash stream moves. Facts,
plans and the wire form are immutable pre-begin values outside the engine's durable
state, so every later backend must reproduce identical trades, receipts, reports and
hash observations for the same program, inputs and feed. A lane that finds it must
move a leaf has left scope A: it stops and reports instead of re-harvesting a pin.

Later lanes must satisfy these obligations before the joined target is claimed:

| Lane | Delivery against this contract | Boundary |
| --- | --- | --- |
| V | Prove literal/finite-alternative/effective-input/internal-label closure for every roster, mark dynamic/unproved images incomplete, emit canonical v1 bytes and producer fixtures. | `canonicalize` include/pineforge/detail/scratch_facts.hpp:100 |
| H | Read V's wire, fix effective trial settings before semantic begin, derive one plan and allocate/bind once for every run/trial. Reset all mutable presence/minimum/cycle/pin state per trial, sharing only immutable facts and OHLCV. Do not map the new plan through the legacy all-zero P0 sentinel. | `ScratchEffectivePlan` include/pineforge/detail/scratch_facts.hpp:260 |
| R1 | Allocate reusable root/alias pools, generation-checked leases, deletion indexes and free lists from slot counts using actual compiled record sizes and checked arithmetic, with exact reset/move/error behavior. | `ScratchFinitePlan` include/pineforge/detail/scratch_facts.hpp:254 |
| R2 | Acquire/transfer/release every definition, lot, membership, command, authority/cache, applied-notification, journal and source lease, preserving issued/receipt/alias answers. Supply proved live bounds on a closed surface. | `LiveBounds` include/pineforge/detail/scratch_facts.hpp:247 |
| R3 | Keep sparse askable origins with original positions, multiplicity, last-origin facts and append folds, proving all readers so a pinned prefix cannot retain replacement history. | `LiveBounds` include/pineforge/detail/scratch_facts.hpp:247 |
| C | Preallocate historical cohort records and nested live workspace, retaining logical presence, first-use handle order, resident order and pins. Unused slots fold nothing. | `U` include/pineforge/detail/scratch_facts.hpp:183 |
| E | Preserve exact single/pair identity, absence versus UINT64_MAX, minima fallback and existing erasure folds across recurrence/reset. | `erased_capacity` include/pineforge/detail/scratch_facts.hpp:154 |
| P | Preserve joined-key aliases, presence and last signed cycles, preallocating lexical hash traversal with all ever-present keys retained. | `joined` include/pineforge/detail/scratch_facts.hpp:209 |
| K | Bind finite rank bytes/workspace, preserve first-fill age, Java ordering/digest/peaks and the existing 28th-key ignore rule. | `largest` include/pineforge/detail/scratch_facts.hpp:218 |
| X | Route incomplete proofs to exact external history before execution on file-capable hosts, adding disk-ordered c traversal and chunked byte comparison/folds with bounded charged residency. Existing disk selection alone is not the completed memory proof. | `ScratchBackendDecision` include/pineforge/detail/scratch_facts.hpp:252 |
| G | Gate charged per-run/per-trial growth across bar/history scales after shared OHLCV load, with growing-allocation/hash-vector mutants that fail and a fixed-pool control that passes. Account for certificate setup, nested pools and all workspaces. | `ScratchEffectivePlan` include/pineforge/detail/scratch_facts.hpp:260 |
| B | Audit identical hash byte streams at existing observation points and all new representation metadata. Scope A changes no state-hash leaf, parity digest or pin, and permits zero trade/receipt/report differences. Keep every genuine root-pair fold despite physical slot recycling. | `ScratchFinitePlan` include/pineforge/detail/scratch_facts.hpp:254 |

The concrete-key capacity oracle is `enumerate_domain` tests/test_scratch_facts.cpp:52

The independent wire producer is `fixture_wire` tests/test_scratch_facts.cpp:227

Strict malformed-input branches are exercised by `hostile_wire` tests/test_scratch_facts.cpp:333
