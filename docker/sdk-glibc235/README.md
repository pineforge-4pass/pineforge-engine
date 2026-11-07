# Ubuntu 22.04 SDK toolchain and hosted diagnostic

This recipe builds GCC 13.3.0 C/C++ on glibc 2.35 for native x86_64, with Eigen
3.4.0 and tools for a disconnected SDK build. The diagnostic source has not
been executed or accepted as runtime evidence.

## Authenticated CA bootstrap

The native hosted runner must already have `curl`, `gpgv`, Python 3 and
`/usr/share/keyrings/ubuntu-archive-keyring.gpg`. The driver downloads
`https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/dists/jammy-updates/InRelease`
over verified HTTPS, then verifies its actual signature with that existing
Ubuntu archive keyring. Missing trust or signature failure stops the run.
There is no key download or insecure transport fallback.

The verified Release must name `main/binary-amd64/Packages.xz` with size
3938856 and SHA256
`759dae046fad315078b955b817b17e0713d60879c66a63df57a12b0f31a2de13`.
After checking those downloaded bytes, the driver selects the unique
`ca-certificates` stanza, version `20260816ubuntu0.22.04.1`, architecture `all`,
size 139540, and URL beneath that snapshot:

```text
pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.22.04.1_all.deb
```

The package must match the signed stanza and `bootstrap-ca.sha256`:
`29ca037f49f7a4b41d67ad72946c2e541f3ae52d8f6ab1495d2af09aa391144b`.
The package is a generated CI input and is never committed.

The isolated Docker context contains exactly six files beneath
`docker/sdk-glibc235/`: `Dockerfile`, `README.md`, `ubuntu-snapshot.list`,
`gcc-13.3.0.sha512`, `bootstrap-ca.sha256`, and the verified CA `.deb`.
Both stages independently check the package hash before `dpkg-deb --extract`.
They concatenate the extracted Mozilla `.crt` payload into the HTTPS trust
bundle before APT runs. Package maintainer scripts are not used for bootstrap;
no host CA bundle is copied into either stage.

APT reads only the dated signed HTTPS sources in `ubuntu-snapshot.list`
(jammy, jammy-updates and jammy-security, main and universe). Only these
historical sources relax Valid-Until; TLS, repository signatures and
authentication remain enabled. Actual package versions are recorded.

## Prepare and build

The workflow `.github/workflows/sdk-glibc235-diag.yml` has one `ubuntu-24.04`
job, `sdk-glibc235-diag`. It runs only on a push to the literal branch
`diag/sdk-glibc235-1110` in `pineforge-4pass/pineforge-engine`. CORE owns that
push. There are no PR, manual dispatch, release or registry publication steps.
The job has read-only contents permission and uses pinned checkout/artifact
actions. Both checkouts disable persisted credentials.

The job invokes this command in its native hosted runner:

```sh
python3 -I "$GITHUB_WORKSPACE/new-source/docker/sdk-glibc235/diagnostic.py" \
  --new-source "$GITHUB_WORKSPACE/new-source" \
  --old-tag-source "$GITHUB_WORKSPACE/old-tag-source" \
  --work-root "$RUNNER_TEMP/sdk-glibc235-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT" \
  --report-root "$RUNNER_TEMP/sdk-glibc235-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT/reports"
```

For a bootstrap-only inspection in that same authorized hosted event, use
`--prepare-only` with a distinct unused work root and its `reports` child.
This still checks source identity and the deliberate crash control. It writes
`PREPARED.json` describing an authenticated context; it cannot write
`RUN.COMPLETE`. After inspecting the signature/hash receipts and only after
successful preparation, the standalone image build procedure is:

```sh
docker build --platform linux/amd64 --progress=plain \
  --iidfile "$RUNNER_TEMP/sdk-glibc235-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT-prepare/toolchain.iid" \
  --file "$RUNNER_TEMP/sdk-glibc235-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT-prepare/context/docker/sdk-glibc235/Dockerfile" \
  "$RUNNER_TEMP/sdk-glibc235-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT-prepare/context"
```

This assumes the bootstrap-only work root ends in `-prepare`. These are usage
instructions, not executed results. The full diagnostic builds its context and
image itself. Contexts, containers and build trees are retained without cleanup.

Both stages retain the Ubuntu 22.04 linux/amd64 manifest pin
`sha256:8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f`.
Its distinct index digest is
`sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11`.
GCC is downloaded from
`https://gcc.gnu.org/pub/gcc/releases/gcc-13.3.0/gcc-13.3.0.tar.xz`, checked
against the unchanged official SHA512 record before extraction, bootstrapped
with at most eight workers, and installed to `/opt/gcc-13.3.0`.
The image checks actual compiler versions/targets, glibc, Eigen macros, ELF
identities and compiler-runtime resolution.

The resulting local image ID in `diagnostic-image.json` identifies this
diagnostic's build. It is **not a release repository manifest pin**.
`scripts/build_prebuilt_bookworm.sh` retains its different immutable pin,
distribution/compiler admission and release contract. This diagnostic never
calls or changes that helper and grants no release admission.

## SDK and actual strategy loads

The driver records the actual commit and tree for the diagnostic checkout and
the separately checked-out `v1.10.0` tag. It refuses dirty inputs and exports
immutable Git blobs without Git metadata, filters or initialized submodule
contents. It declares followed paths before reading or executing them and
refuses protected names, symlinks, out-of-root paths and unsafe tar members.
Child environments exclude user configuration and credentials.

The new exported source is built and installed with explicit GCC 13.3.0
paths, Release CMake, installed Eigen, disconnected FetchContent, tests and
tutorial disabled, `PINEFORGE_VERSION_SOURCE=FILE`, and an exact source SHA
identity include. SDK builds use four workers. No host compiler or Eigen
tree is mounted. Version checks consume the actual installed headers and CMake
package files.

The installed package includes the old-glibc build's regular `libstdc++.so.6`
and `libgcc_s.so.1` bytes, selected by actual SONAME and recorded by hash.
`share/gcc-runtime/` preserves the verified corresponding GCC source archive,
official source checksum, `COPYING*` licenses and runtime exception.
Other dynamic dependencies must belong to the explicit OS-provided libc
family or the two supplied GCC libraries. No libc or ELF interpreter is copied.

The unchanged `scripts/check_prebuilt_glibc.py` inspects every installed
archive/shared object, including all ordered archive members, with the actual
released archive as comparison baseline. Every installed file's magic is
checked so an ELF hidden under an unrecognized filename fails. The fixed
GLIBC <= 2.35 and no-`__isoc23_*` rules remain unchanged. Complete nonempty
inventory and the checker's own PASS summary are required. The packed SDK is
safely unpacked and its complete file/hash manifest must equal the inspected
installed inventory before it is consumed.

The old archive and sidecar are the real public release downloads:

| File | Bytes | SHA256 |
| --- | --- | --- |
| `pineforge-v1.10.0-linux-x86_64.tar.gz` | 8403177 | `cb412fbae9e8ba515de41dc31026c77d6c5e309693247bee26bb93c67d3c3543` |
| `pineforge-v1.10.0-linux-x86_64.tar.gz.sha256` | 104 | `ca260267fc54e85684f0836aad3b8fc97458e70519eff30e18dd002195ea5bb0` |

Their URLs are beneath
`https://github.com/pineforge-4pass/pineforge-engine/releases/download/v1.10.0/`.
The sidecar must name the same verified archive. Published SDK bytes are never
patched. Supplementary consumer GCC dependencies are recorded separately.

For each SDK, its matching source tree's existing
`examples/native/native_market_strategy.cpp` is compiled using that SDK's
installed headers and `libpineforge.a`. Both use the same newly built GCC,
with the actual entrypoint flags: C++17, O2, `-ffp-contract=off`, PIC/shared,
installed SDK/Eigen include paths, and whole-archive linking. The example's
standalone `main` is never called.

`load_strategy.c` is compiled twice in the old-glibc image against each SDK's
matching C header. The executable is PIE and named `*-loader.so` so the
unchanged inspector discovers its DYN ELF alongside the new module. The old
module is inspected as baseline; its known incompatible symbols are never
allowed in the candidate. The loader uses `dlopen(RTLD_NOW)`, checks the real
`pf_abi_version` against that header's `PF_ABI_VERSION`, resolves the required
exports from `docker/run_json.py`, and calls real `strategy_create("{}")`
and `strategy_free`, requiring a non-null instance. It runs no backtest.

The four actual native-x86 container cases use these amd64 manifest pins:

| Runtime | Manifest SHA256 |
| --- | --- |
| Ubuntu 22.04 | `8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f` |
| Debian 12 | `366a6da5efcceffbaa19c560a101b97b87a1a9664eeeac8bc3d24395ddbf14f3` |

Each container has no network, a read-only root and input mounts, dropped
capabilities and no new privileges. There is no emulator or package install.
Each OS must show an actual old module `dlopen` failure, exit 10 from the
loader, with `undefined symbol: __isoc23_strtol`; each new module must finish
the ABI/create/free path with exit 0. Raw output and stopped-container state
are retained before classification. Compile errors, missing dependencies,
wrong ABI, setup failures and unexpected old success fail the proof.

## Evidence and unexecuted failure controls

Reports retain complete child argv, stdout, stderr, native return codes,
timings, input inventories, real hashes, signed bootstrap metadata,
toolchain build/provenance logs, symbol-inspector JSON lines, raw container
states and classified case files. Each new run first executes an uncaught
deliberate exception and requires its real nonzero exit and traceback. A
failed control stops the sequence. `.exit` files record actual child/driver
codes; no verdict is inferred from one.

`INCOMPLETE.json` is an immutable initial record. Exceptions and refusals
write `FAILURE.json` and a nonzero driver exit when the report destination
remains writable. Abrupt loss leaves the initial incomplete record.
Only all required classified case files and both complete inspector verdicts
permit `RUN.COMPLETE`. Upload uses `always()` with missing files treated as
an error. Only the reports subtree is uploaded; no checkout, Git metadata,
source archive, `.deb`, SDK archive or build tree is uploaded.

Further independent failure controls remain unexecuted. In separate retained
contexts, corrupt the downloaded CA package and require hash rejection before
extraction; corrupt signed metadata and require `gpgv` rejection; alter the
compiler-version expectation and require rejection of the actual version.
In separate retained consumer inputs, a missing dependency, ABI mismatch,
null create or wrong old-loader diagnostic must leave the proof incomplete.
An empty/thin/non-ELF or partly inspected archive must fail the unchanged
inspector. Each control needs its own actual failure at the intended stage;
an earlier setup failure is not evidence for it. Stop after an unexpected
control result. These instructions supply no current runtime results.
