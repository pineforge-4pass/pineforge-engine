# Getting Started {#getting_started}

@tableofcontents

A minimal end-to-end build, install, and link in under a minute.

## Prerequisites

| Requirement | Minimum | Notes |
| --- | --- | --- |
| CMake | 3.16 | |
| C++ compiler | GCC 9, Clang 10, Apple Clang 12 | C++17 required. |
| Eigen | 3.3+ | Optional — fetched automatically via `FetchContent` if no system install is found. |

## Build + test

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git
cd pineforge-engine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expect 30 tests to pass. The largest (`test_integration`,
`test_request_security`) take a few hundred milliseconds; everything
else completes faster.

## Install

```bash
cmake --install build --prefix /usr/local
```

This installs:

- `lib/libpineforge.a` — the static runtime
- `include/pineforge/*.hpp` — internal headers
- `include/pineforge/pineforge.h` — **the public C ABI**
- `include/pineforge/version.h` — generated version macros
- `lib/cmake/PineForge/PineForge{Config,Targets,ConfigVersion}.cmake`

After install, downstream CMake projects can pick the runtime up with a
single `find_package(PineForge)` call — see
[CMake integration](@ref integration_cmake).

## A first program

```c
#include <pineforge/pineforge.h>
#include <stdio.h>

int main(void) {
    pf_version_t v = pf_version_get();
    printf("PineForge %d.%d.%d (%s)\n",
           v.major, v.minor, v.patch,
           v.commit_sha[0] ? v.commit_sha : "unknown");
    return 0;
}
```

Compile and run:

```bash
cc hello.c -lpineforge -lstdc++ -lm -o hello
./hello
# PineForge 0.1.1 (97c93d3)
```

That's it — you have a working install. Next steps:

- **[Lifecycle](@ref lifecycle)** — handle ownership and report freeing
- **[Tutorial: MACD](@ref tutorial_macd)** — full backtest walkthrough
- **[FFI from Python](@ref ffi_python)** — calling the runtime via ctypes
