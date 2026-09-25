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
| `PINEFORGE_BUILD_TESTS` | `ON` | Build the 30-binary `ctest` suite. Disable in package builds. |
| `PINEFORGE_BUILD_TUTORIAL` | `ON` | Build `tutorial/macd/strategy.so`. |
| `PINEFORGE_BUILD_CORPUS_STRATEGIES` | `OFF` | Compile the private 168-strategy corpus (maintainers only). |
| `PINEFORGE_ENABLE_COVERAGE` | `OFF` | Instrument runtime + tests for source coverage (Clang/GCC). |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Install root. |

### Install layout

```
${prefix}/
├── lib/
│   ├── libpineforge.a
│   └── cmake/PineForge/
│       ├── PineForgeConfig.cmake
│       ├── PineForgeConfigVersion.cmake
│       └── PineForgeTargets.cmake
└── include/pineforge/
    ├── pineforge.h        # public C ABI
    ├── version.h          # generated version macros
    ├── bar.hpp            # internal C++ headers (no stability guarantee)
    ├── color.hpp
    ├── engine.hpp
    ├── log.hpp
    ├── magnifier.hpp
    ├── math.hpp
    ├── matrix.hpp
    ├── na.hpp
    ├── series.hpp
    ├── session_time.hpp
    ├── str_utils.hpp
    ├── ta.hpp
    └── timeframe.hpp
```

## Docker

The published image bundles the runtime plus a one-shot transpile + run
harness for ad-hoc strategy execution.

```bash
docker pull ghcr.io/pineforge-4pass/pineforge-engine:latest
```

Run the tutorial MACD strategy entirely inside the container:

```bash
docker run --rm \
  -v "$(pwd)/tutorial/macd/generated.cpp:/in/strategy.cpp:ro" \
  -v "$(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro" \
  ghcr.io/pineforge-4pass/pineforge-engine:latest > report.json
```

See [`docker/README.md`](https://github.com/pineforge-4pass/pineforge-engine/blob/main/docker/README.md)
for the full mount + JSON schema reference.

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
