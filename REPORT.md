## Stage 2.3 — round-2 review fixes

Branch: `cx/o1-contracts-fix1`. Base and unchanged HEAD:
`7025116c8e3f8b328268220afc3261fd0ddbab3a`.
This is an edit-only handoff; the supervisor commits the diff.

This section is prepared for `../REPORT.md`, which does not exist and is outside
the writable workspace. The consumer update is prepared alongside it in
`CONTRACT-NOTE-for-TNX-and-LAB.md` for append to the existing parent-directory
note. No parent-directory file was edited.

### Changes per defect

- **D1 — status unification.**
  `include/pineforge/detail/scratch_store_impl.hpp:226-243` records backing
  `io_error` provenance in the read/write/sync wrappers. `:1341-1352` and
  `:1398-1415` reset that flag only after the mutation prechecks and return
  `io_error` for backing failure; other failed mutations remain
  `programming_error`. `:1724` is transient state, not serialized or hashed.
  Source/comparator errors therefore retain their existing classification,
  and nested refused mutations cannot clear the outer operation's provenance.
  Contract text agrees at `include/pineforge/detail/scratch_store.hpp:23-25`,
  `:118-136`, `include/pineforge/detail/scratch_backing.hpp:14-16`, and
  `docs/scratch-store.md:135-151`, `:257-274`, `:291-294`.
  The cap is defined at `scratch_backing.hpp:85-89` and
  `docs/scratch-store.md:200-208`: bytes of extent growth from run start,
  including zero-filled gaps, checked before any growth write changes bytes.
  Transactions do not reset it; overwrites and rollback writes below the
  existing extent remain serviceable.
- **D1 — fixture.** Kept the earlier edit to
  `tests/test_scratch_store.cpp:1490`, changing only the authorized pinned
  expectation to `io_error`. Kept the single new capped-backing fixture at
  `:2872-2968`, called at `:3087`. Its three public-surface cases assert exact
  `io_error` from put/erase/commit, exactly one cap refusal, one page of actual
  growth, unchanged failed-put output, rollback-only state, rollback under the
  still-armed cap, actual rollback writes for put/erase, and agreement with the
  independent committed model/digests after rollback and same-backing reopen.
  It cannot pass by merely throwing or by failing to reach the cap.
  Comments were aligned at `:2810-2813` and
  `tests/test_scratch_contract_guard.py:8-10`; the guard logic is unchanged.
  The fixture documentation row is `docs/scratch-store.md:325`.
- **D2 — text only.** `include/pineforge/detail/scratch_backing.hpp:90-96`,
  `docs/scratch-store.md:32-39` (Ownership) and `:210-216` (Backing) bind file
  lifetime to the backing object's life, specify unlink/close, forbid another
  object/process reopening the file, and define reopen on the same live
  backing with no journal replay. Consumers cannot assume persistence or crash
  recovery; discard the backing if a destroyed store's rollback was refused.
  Per-run commits use `buffered`. Related store wording at
  `scratch_store.hpp:69-72`, `:147-150` and
  `docs/scratch-store.md:102-109`, `:129-133`, `:295-297`, `:353-354` now carries
  those preconditions.
- **D3 — two definition lines.**
  `include/pineforge/detail/scratch_store_impl.hpp:1290-1293` wraps both source
  `size()` calls in the existing `busy_` callback guard.
- **D4 — text only.** `include/pineforge/detail/scratch_backing.hpp:70-71`
  and `docs/scratch-store.md:223-224` forbid an I/O observer from calling any
  store/backing member. No optional runtime reentrancy guard was added.
- **D5 — two definition lines including its comment.**
  `include/pineforge/detail/scratch_store_impl.hpp:1166-1167` clears the journal
  pool after loading a superblock rather than trusting potentially torn
  saved links. `docs/scratch-store.md:251-254` states the per-reopen pool loss.

All paths abbreviated above refer to the existing headers under
`include/pineforge/detail/`; the brief's non-detail header paths are stale.
No declaration, public field, enumerator, sink field, format version or
serialized superblock field changed. The only existing assertion edited is
the explicitly authorized `tests/test_scratch_store.cpp:1490`.

### Blob pins for TNX-CX and LAB-CX

These are content IDs read with `git hash-object -- <paths>` without `-w`.
No commit was created, so `git rev-parse HEAD:<path>` still names the old
content. After the supervisor commits these exact files, re-pin with that
command on the new HEAD and announce its SHA before landing.

| Path | Stage-2.3 content blob |
| --- | --- |
| `include/pineforge/detail/scratch_store.hpp` | `7c5cc0588d3bb0fa91ca6657ba1ef4adbd1b101f` |
| `include/pineforge/detail/scratch_backing.hpp` | `c1e07b3c0a3a4d17af9f0c6bb59136220efd2c7b` |
| `include/pineforge/detail/scratch_store_impl.hpp` | `a10f416bd3e071828ceba5bdeab3522031ed4ddc` |
| `include/pineforge/detail/scratch_readback.hpp` (unchanged) | `6d182f3d85ab332d54354d110ea579af812534bd` |
| `docs/scratch-store.md` | `f26ca82641e7b70fb177c4f66373aeaaa99a8a19` |
| `tests/test_scratch_store.cpp` | `04a328379d6716fee314289fb00bcabc89e77633` |
| `tests/test_scratch_contract_guard.py` | `3b8aba60b3c5cecfcb56280db563bf2f363a139a` |

### Diff and read-only review

`git diff --stat 7025116c8` (tracked files; no lane commits):

```text
 docs/scratch-store.md                           | 102 ++++++++++++++-------
 include/pineforge/detail/scratch_backing.hpp    |  17 +++-
 include/pineforge/detail/scratch_store.hpp      |  42 +++++----
 include/pineforge/detail/scratch_store_impl.hpp |  25 ++++--
 tests/test_scratch_contract_guard.py            |   3 +-
 tests/test_scratch_store.cpp                    | 112 ++++++++++++++++++++++--
 6 files changed, 237 insertions(+), 64 deletions(-)
```

Additional untracked handoff files: this report,
`CONTRACT-NOTE-for-TNX-and-LAB.md`, and `DONE-stage23`.
`git diff --check` reported no findings. Review consisted only of file reads,
`rg`/`sed`, Git inspection/content hashes, and file edits. The entire stage-2.3
brief and round-2 review were read. No measured engine/source file was opened
or edited. No test assertion was weakened, skipped or removed.

### UNVERIFIED

**Nothing was built or run in this continuation.** No compiler, syntax check,
CMake, CTest, test binary, Python/repository script or install was executed on
this Mac. The earlier stopped attempt's Mac guard refusal (exit 77,
LB-446/compiler) is not verification evidence.

No fail-before diagnostic, unit-test result, preflight result, release-profile
result, ABI-matrix result, corpus/parity result or remote-job result is claimed.
Verification summary: **UNVERIFIED — lab submission pending supervisor**.
Verification logs/job IDs: **none for this continuation**.

By reading the old definition, the new capped put should report
`programming_error` instead of its required `io_error` against stage 2.2; this
is a prediction, not an executed RED result. The supervisor must submit the
frozen-header fail-before check, C++ unit fixtures, preflight, full release
verification and full corpus/parity verification to the lab. D3's callback
guard and D5's torn-pool fix also have no executed proof from this lane.

### Follow-ups, deviations and open questions

- No D1-D5 implementation/text item was deferred. D3 and D5 stayed within the
  brief's few-line definition allowance; D4 used its permitted text-only fix.
  Dedicated new D3/D5 regression fixtures and any optional observer guard are
  outside this limited round, and remain follow-ups for lab-backed work.
- The supervisor must append the local report section and consumer update to
  the parent-directory handoff files. Those paths are outside the sole writable
  root; `../REPORT.md` is absent. This is the output-location deviation.
- No `git add`, `git commit` or push was run, as the continuation explicitly
  requires the supervisor to commit. Suggested message:
  `Unify D1 scratch I/O status and D2 lifetime; fix D3-D5`.
- New pins are uncommitted-content hashes rather than misleading old HEAD
  pins; the supervisor must confirm the committed IDs and supply the new HEAD.
- Real disk/memfd/tmpfs backings, OS unlink/durability behaviour and the
  ENOSPC/EDQUOT classification question from review Q6(b) remain LAB-CX work.
  The Q3 slot-count wording nit is outside D1-D5 and remains a documentation
  follow-up. No other contract deviation or open implementation question was
  identified by this read-only review.
