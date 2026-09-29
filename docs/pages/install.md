# Install {#install}

@tableofcontents

## From source

The canonical install path. Works on Linux and macOS.

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git
cd pineforge-engine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build --prefix /usr/local
```

### Useful CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | Set to `Debug` for assertions and unstripped symbols. |
| `PINEFORGE_BUILD_TESTS` | `ON` | Build the `ctest` suite (several hundred test binaries). Disable in package builds. |
| `PINEFORGE_BUILD_TUTORIAL` | `ON` | Build `tutorial/macd/strategy.so`. |
| `PINEFORGE_BUILD_CORPUS_STRATEGIES` | `OFF` | Build one `strategy.so` per probe of the parity corpus submodule (`git submodule update --init corpus`; 312 probes at its pinned commit). |
| `PINEFORGE_ENABLE_COVERAGE` | `OFF` | Instrument runtime + tests for source coverage (Clang/GCC). |
| `PINEFORGE_BUILD_EXAMPLES` | `OFF` | Build the Pine-free hosts of `examples/native/` and their `example_*` CTest rows. |
| `PINEFORGE_BUILD_SOURCE_LAYER` | `ON` | `OFF` builds the kernel alone; the Pine adapter headers are not installed. |
| `PINEFORGE_BUILD_LIVE_RUNNER` | `OFF` | Build the native `pineforge-live` runner (SQLite3, libcurl, OpenSSL); see `runner/README.md`. |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Install root. |

### Install layout

```
${prefix}/
├── lib/
│   ├── libpineforge.a          # kernel + Pine adapter (PineForge::pineforge)
│   ├── libpineforge_kernel.a   # the kernel alone (PineForge::kernel)
│   └── cmake/PineForge/
│       ├── PineForgeConfig.cmake
│       ├── PineForgeConfigVersion.cmake
│       └── PineForgeTargets*.cmake
└── include/pineforge/
    ├── pineforge.h        # public C ABI
    ├── native_c_api.h     # C native-host API (included by pineforge.h)
    ├── live_parser.h      # parser-plugin ABI of the native live runner
    ├── version.h          # generated version macros
    ├── native_*.hpp       # kernel / native C++ API (see Public contract)
    ├── *.hpp              # the other C++ headers (no stability guarantee)
    ├── source/            # Pine adapter headers (internal; not in a kernel-only build)
    └── compat/pine/       # Pine adapter headers (internal; not in a kernel-only build)
```

## Docker

This repository publishes no container image: a release attaches prebuilt
static-lib tarballs, `pineforge-vX.Y.Z-linux-x86_64.tar.gz`,
`pineforge-vX.Y.Z-linux-aarch64.tar.gz` and `pineforge-vX.Y.Z-macos-universal.tar.gz`
(each with a `.sha256`; `lib/`, `include/`, the CMake package, `LICENSE`,
`NOTICE`, `VERSION`), to its GitHub release and notifies the release hub,
pineforge-release. v0.13.1 is the last tagged release; main is not yet
released. The hub publishes the image,
`ghcr.io/pineforge-4pass/pineforge-release`: this runtime, a pinned
`pineforge-codegen` transpiler and the one-shot transpile + run harness of
`docker/`, built from the release's static-lib tarball. The image is tagged
with the hub's own version (`X.Y.Z`, `X.Y`, and `latest` for the newest stable
one), with `engine<E>-codegen<C>` naming the pair it carries, and with
`sha-<short>`; a release candidate gets no `latest`. Pin the
`engine<E>-codegen<C>` tag of the engine release you build against.

```bash
docker pull ghcr.io/pineforge-4pass/pineforge-release:latest
```

Run the tutorial MACD strategy entirely inside the container; the image
transpiles the `.pine` with its own codegen:

```bash
docker run --rm \
  -v "$(pwd)/tutorial/macd/strategy.pine:/in/strategy.pine:ro" \
  -v "$(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro" \
  ghcr.io/pineforge-4pass/pineforge-release:latest > report.json
```

See [`docker/README.md`](https://github.com/pineforge-4pass/pineforge-engine/blob/main/docker/README.md)
for the full mount + JSON schema reference. This tree keeps that harness
(`docker/entrypoint.sh`, `docker/run_json.py`) but no Dockerfile; the image's
Dockerfile is pineforge-release's.

## Verifying the install

A self-contained smoke test ships under `cmake/smoke_consumer/`:

```bash
cmake -S cmake/smoke_consumer -B build-smoke
cmake --build build-smoke
./build-smoke/smoke_version     # prints the runtime version string
```

If this prints the version you installed you're done: `pf_version_string()`,
which is the `VERSION` file exactly (a release candidate's `-rc.N` included)
for a tarball build or one configured with `-DPINEFORGE_VERSION_SOURCE=FILE`,
and `git describe`'s descriptor for a build from a git checkout. The program
exits 1 instead if the CMake package's `PineForge_VERSION_FULL`, the installed
`pineforge/version.h` and the linked library name different versions, or if
its own multiply-add was fused; the configure step also checks that the
package hands its consumers `-ffp-contract=off`.
