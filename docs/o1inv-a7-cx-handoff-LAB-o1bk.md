LAB-o1bk edit-only handoff. HEAD remains
`28bb6b459b20f65ff79851e27e8272dfe4f51a40`.
No staging, commit, build, test, Python, Node, lab, or network command was run.
Implementation and test source are supplied; compilation, executed fail-before,
C++ verification, and corpus/parity verification remain unfinished on the remote
hosts under the release verification hosts.

The exact per-file change list is:

| File | Change |
| --- | --- |
| `src/command_scratch_production.cpp` (moved out of the source layer by `c6f7fb23` to share the factory with the independent kernel) | New production factory and exclusive POSIX file backing: `mkstemp`, immediate `unlink`, close-on-exec, checked positional I/O, sparse extension, real durability calls, errno status mapping, and observer registration. |
| `src/source/pine_command_scratch_production.hpp` | New factory declaration, observer registration hook, directory/default documentation, geometry/error and observer lifetime contracts. |
| `tests/test_o1bk_production_backing.cpp` | New executable with seven named cases, using the library factory without any fixture lease or replacement definition. |
| `CMakeLists.txt` | One source registration line in `PINEFORGE_SOURCE_LAYER_SOURCES`. |
| `tests/CMakeLists.txt` | Appended test-list and `pineforge_add_unit_test` registration, conditional on `PINEFORGE_BUILD_SOURCE_LAYER AND UNIX`. |
| `docs/o1inv-a7-cx-handoff-LAB-o1bk.md` | This handoff, including scope checks, must-fail arguments, and pending remote work. |

The selected directory is `PINEFORGE_COMMAND_SCRATCH_DIR`; unset or empty means
`/tmp`. A failure in the selected directory throws `std::system_error` with the
underlying errno. Each successful factory call owns a distinct descriptor and
starts at zero pages. `ftruncate` extends the file before positional writes and
zero-fills gaps; after a successful extension the recorded extent remains real
even if the subsequent write fails. Offsets are checked before multiplication
and conversion to `off_t`. Short transfers and early EOF return `io_error`.
ENOSPC, EDQUOT, and EFBIG map to the contract's `programming_error`; other syscall
errors map to `io_error`.

`buffered` sync is a no-op; `data` calls `fdatasync`; `full` calls `fsync`.
Interrupted I/O is retried. Zero, non-platform-page-multiple, and
unrepresentable page sizes throw `std::invalid_argument`.

The run owner calls
`pineforge::source::detail::set_command_scratch_io_observer(observer)` on the
thread creating command backings. It returns the previous registration, which
the owner can restore after nested scopes. Each thread starts with no observer.
Each backing snapshots the registration into the frozen `ScratchBacking` base,
so its public methods keep the existing exactly-once charge path, including
failed operations. Observer contexts must outlive the backings using them.
Changing registration only affects subsequently created backings.

The CTest row and executable are both exactly
`test_o1bk_production_backing`. Its named cases and predicted failure evidence
are:

| Case printed by the executable | Coverage and must-fail argument |
| --- | --- |
| `invalid_page_geometry` | Requests zero and platform-page-size minus/plus one. Fails `zero or nonmultiple page size must throw invalid_argument` if geometry is silently accepted. |
| `sparse_extent_and_durability` | Runs at one and three platform pages per scratch page. Writes pages 0, 7, and 4095; checks exact extent transitions, all bytes after every durability level, zero-filled gaps, absence at 4096, and an overwrite. A `FixedMemoryScratchBacking` fails `factory must select the production disk backing`; insufficient fixed storage also cannot satisfy the page-4095 write, and memory's data/full sync cannot satisfy `file sync succeeds`. Fake extension fails the extent-transition, byte-equality, gap-zero, or absent-at-4096 assertion depending on the defect. |
| `exclusive_backings` | Writes page 3 in A while B stays empty; writes different bytes to page 3 in B; destroys A and rereads B. Shared extent fails `extending A cannot extend B`; shared storage fails `A and B never share file contents`; shared descriptor ownership fails B's read after A is destroyed. |
| `anonymous_file_lifecycle` | Finds the one new descriptor through `/dev/fd`, checks regular-file type, zero link count, zero initial size, selected filesystem, and close-on-exec. Writes page 4095 and checks OS `st_size == page_size * 4096`, then verifies descriptor closure. Checks the selected directory is empty both while live and after destruction. A fileless fixture fails `production factory must create a real file descriptor`. A counter-only extension fails `reported extent agrees with the OS file size`; delayed unlink fails the link-count or empty-directory assertion. |
| `observer_registration_and_charges` | Checks exactly one record, operation, medium, page, requested byte count, and result for writes, reads, absent pages, malformed buffers, sentinel/overflow IDs, all sync modes, and invalid durability. Checks nested registration/restoration, existing backing snapshots, and zero extra records from backings created without an observer. Dropping the observer fails `exactly one charge per operation` at the first write; wrong routing or double charging fails subsequent count/field checks. |
| `selected_directory_failure` | Selects a nonexistent subdirectory and requires ENOENT. Ignoring the selected directory or swallowing setup failure fails `failed selected directory must surface errno instead of falling back`. |
| `short_reads_and_syscall_errors` | Truncates a live file below one page and then to zero, requiring `io_error` for short read and early EOF with matching charge records. Closes its descriptor to force failures from fdatasync, fsync, pread, pwrite, and ftruncate, while buffered remains a no-op. Error swallowing fails the corresponding status assertion. Failed extension must leave `page_count()` at one. The test opens no new descriptor between the close fault and backing destruction. |

These are predicted must-fail arguments, not executed results. The literal
frozen base lacks the new production header, so compiling this entire new test
against its unchanged header closure is expected to first diagnose the missing
`../src/source/pine_command_scratch_production.hpp`. With the new declaration
header made available but the provider TU/registration withheld, the test must
fail to link the factory and observer-registration functions. The remote lab
must record the actual first diagnostic.

The requested base carries `src/source/pine_command_scratch_production.hpp` but lacks the pre-rename `src/source/pine_command_scratch.hpp` <!-- verified HEAD --> and the
O1 fixture headers named in the brief. The new header declares the exact factory
signature directly, so the standalone patch does not depend on those later
lane additions. Other O1 lanes can retain their identical existing factory
declarations.

Frozen-file comparisons used
`/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1c/wt/`
as the reference. Before any edit, ten detail headers matched and three already
differed:

| `include/pineforge/detail/` file | `cmp` result against reference |
| --- | --- |
| `adapter_exposure_storage.hpp` | 0, identical |
| `disk_id_table.hpp` | 0, identical |
| `disk_resident_id_table.hpp` | 0, identical |
| `id_pair_store.hpp` | 0, identical |
| `scratch_backing.hpp` | 0, identical |
| `scratch_defaults.hpp` | 0, identical |
| `scratch_errors.hpp` | 0, identical |
| `scratch_file.hpp` | 0, identical |
| `scratch_readback.hpp` | 1, first difference char 430, line 13 |
| `scratch_slab.hpp` | 0, identical |
| `scratch_store.hpp` | 1, first difference char 4087, line 72 |
| `scratch_store_impl.hpp` | 1, first difference char 2833, line 75 |
| `scratch_tables.hpp` | 0, identical |

All thirteen working-tree detail headers were additionally compared byte for
byte with their own `HEAD` blobs using `git show HEAD:<path> | cmp - <path>`;
every comparison returned 0. No frozen header was edited. The literal
reference-tree identity condition therefore has three pre-existing exceptions
for the supervisor to reconcile.

The historical `cmp src/source/pine_command_scratch.hpp <reference>/src/source/pine_command_scratch.hpp` <!-- verified HEAD -->
returned 2 because the local file is absent. A read-only `rg --files` search for
that exact filename under both source directories returned no matches (exit 1):
it is absent in both trees, so no byte-comparison PASS is claimed for that file.
It was not created or changed.

`git diff --check` returned 0 with no whitespace diagnostics. The three new C++
files were also checked with `git diff --no-index --check /dev/null <path>`;
each produced no whitespace diagnostics (exit 1 denotes the new-file
differences). The handoff is checked the same way. Git emits a host
`confstr()` temporary-directory warning on these read-only commands.

The final `git status --porcelain --untracked-files=all` inventory is:

```text
 M CMakeLists.txt
 M tests/CMakeLists.txt
?? docs/o1inv-a7-cx-handoff-LAB-o1bk.md
?? src/source/pine_command_scratch_production.cpp
?? src/source/pine_command_scratch_production.hpp
?? tests/test_o1bk_production_backing.cpp
```

Remote execution belongs on the maintainers' x86-64 lab build hosts after the
supervisor applies this diff. With the source layer enabled on UNIX, the normal
test registration links the test against `pineforge`; its descriptor checks
require `/dev/fd` and its temporary directory is created under the CTest working
directory. The provider and row are intentionally absent from source-layer-OFF
kernel-only builds.

The remote owner should first capture the frozen-closure fail-before, then run
preflight, a complete release verification build (all test executables), and
`ctest --test-dir <build-dir> --output-on-failure -R '^test_o1bk_production_backing$'`
for focused diagnostics as needed. The full release profile includes this CTest
row; the additional source-enabled debug/sanitizer profiles exercise it as well.
The supervisor owns the full corpus/parity campaign and required
`pineforge/verify` and `pineforge/parity` statuses on the final combined tree.
No runtime PASS, parity verdict, or build/test log is available from this lane.
