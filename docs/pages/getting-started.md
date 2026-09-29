# Getting Started {#getting_started}

@tableofcontents

A minimal end-to-end build, install, and link.

## Prerequisites

| Requirement | Minimum | Notes |
| --- | --- | --- |
| CMake | 3.16 | `ctest --test-dir` below needs 3.20. |
| C++ compiler | libstdc++ from GCC 11, or libc++ 14; on macOS a deployment target of 13.3 | C++17 with floating-point `std::to_chars`. |
| Eigen | 3.3+ | Optional — fetched automatically via `FetchContent` if no system install is found. |

## Build + test

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git
cd pineforge-engine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

All configured tests should pass. The suite includes integration, ABI,
timeframe, broker, and continuous-stream lifecycle coverage.

## Install

```bash
cmake --install build --prefix /usr/local
```

This installs:

- `lib/libpineforge.a` — the static runtime (kernel + Pine adapter)
- `lib/libpineforge_kernel.a` — the kernel alone (`PineForge::kernel`)
- `include/pineforge/*.hpp` — the C++ headers: the kernel / native API
  (`native_host.hpp`, `native_run_spec.hpp`, `native_order.hpp`,
  `native_toolkit.hpp`) and internal ones; `source/` and `compat/pine/` hold
  the Pine adapter's
- `include/pineforge/pineforge.h` — **the public C ABI**, with
  `native_c_api.h` (the C native-host API it includes) and `live_parser.h`
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
cc hello.c -lpineforge -lstdc++ -lm -o hello   # macOS: -lc++ for -lstdc++
./hello
# PineForge <version> (<sha>)
```

That's it — you have a working install. Next steps:

- **[Lifecycle](@ref lifecycle)** — handle ownership and report freeing
- **[Historical to realtime streaming](@ref streaming)** — preserve state while switching feeds
- **[Tutorial: MACD](@ref tutorial_macd)** — full backtest walkthrough
- **[FFI from Python](@ref ffi_python)** — calling the runtime via ctypes
