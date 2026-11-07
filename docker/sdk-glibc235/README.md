# Ubuntu 22.04 SDK toolchain

This source recipe builds GCC 13.3.0 C/C++ on glibc 2.35 for x86_64.
It installs Eigen 3.4.0 and the tools for a disconnected SDK build during image
creation. It has not been built or validated.

Both stages pin the linux/amd64 Ubuntu 22.04 manifest
`sha256:8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f`.
The Ubuntu index digest
`sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11`
is a separate identity. Neither is the resulting toolchain image digest.

APT reads only `ubuntu-snapshot.list`: the authenticated
`https://snapshot.ubuntu.com/ubuntu/20261101T000000Z` snapshot, jammy,
jammy-updates and jammy-security, main and universe. Only those dated sources
relax Valid-Until. Package versions are recorded from `dpkg-query`, without
assuming exact package revisions. The pinned base must already provide its
Ubuntu signing keyring and a working CA bundle for HTTPS bootstrap. Their
availability is **NEEDS inspection**; the recipe stops if the CA bundle is
missing. A missing trust input needs a separately reviewed bootstrap source.
Authentication and TLS verification remain enabled.

GCC comes from
`https://gcc.gnu.org/pub/gcc/releases/gcc-13.3.0/gcc-13.3.0.tar.xz`.
Before extraction, `sha512sum --check --strict` verifies the pinned record
in `gcc-13.3.0.sha512`:

```text
ed5f2f4c6ed2c796fcf2c93707159e9dbd3ddb1ba063d549804dd68cdabbb6d550985ae1c8465ae9a336cfe29274a6eb0f42e21924360574ebd8e5d5c7c9a801
```

GCC bootstraps with snapshot prerequisites, installs to `/opt/gcc-13.3.0`, and
uses at most eight make jobs. The final image copies that complete prefix,
including its runtime libraries. `CC`, `CXX`, `PATH` and `LD_LIBRARY_PATH` select
this build and its libraries. No compiler binary comes from a newer distribution.

On an amd64 image builder, from the repository root:

```sh
docker build --platform linux/amd64 --progress=plain \
  --file docker/sdk-glibc235/Dockerfile --tag sdk-glibc235:local .
docker image inspect sdk-glibc235:local
```

These commands are unexecuted usage examples. Read the build's actual output;
an image tag or image ID does not supply the immutable repository manifest pin
required by the release helper.

During a real build, checks execute `gcc`, `g++`, `getconf`, `dpkg-query` and
`readelf`. They require GCC 13.3.0, an x86_64 Linux target, glibc 2.35 and Eigen
3.4.0 macros from the installed header. Compiler/front-end/runtime ELF records
and loader resolution are saved; this limited inventory does not establish the
SDK's complete runtime compatibility.

Provenance lives under `/usr/local/share/sdk-toolchain/`:

- `inputs/`: exact recipe, README, snapshot list and expected source checksum.
- `gcc-build/`: actual bootstrap versions, package inventory, checksum output
  and configure/build/install output.
- `image/`: actual tool versions, package inventory, Eigen macros and file
  hashes, compiler targets, ELF and loader observations.
- `source/gcc-13.3.0.tar.xz`: the verified corresponding source, including
  copyright notices; `licenses/gcc-13.3.0/`: its `COPYING*` material, including
  the GCC Runtime Library Exception.

For later inspection of the locally built image, without network access:

```sh
docker run --pull=never --platform linux/amd64 --network none --read-only \
  sdk-glibc235:local /bin/bash -euo pipefail -c \
  'gcc -dumpfullversion; g++ -dumpfullversion; gcc -dumpmachine; getconf GNU_LIBC_VERSION; cat /usr/local/share/sdk-toolchain/image/installed-packages.tsv'
```

Negative checks are also unexecuted: in separate retained source copies, change
one checksum digit and require failure before extraction; separately change the
compiler version comparison to `13.3.1` and require failure on the actual
`-dumpfullversion` output. Stop on each unexpected result and retain the real
diagnostic. Do not use those modified copies for producing an image pin.

The release helper still needs the real resulting image pin and updated
toolchain admission, absolute compiler paths and provenance inputs. Then build
the SDK and packaged dependencies in this image, inspect every shipped ELF and
archive member for numeric GLIBC requirements at most 2.35 and no `__isoc23_*`,
and prove actual strategy loading on Ubuntu 22.04 and Debian 12, including the
observed failure of the previous release. This recipe supplies no such proof.
