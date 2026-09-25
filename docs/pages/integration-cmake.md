# CMake integration {#integration_cmake}

@tableofcontents

PineForge installs a standard CMake **package config**. Downstream
projects pull it in with one `find_package` call.

## Minimal CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_strategy_runner LANGUAGES C)

find_package(PineForge 1.0 REQUIRED)

add_executable(runner runner.c)
target_link_libraries(runner PRIVATE PineForge::pineforge)
```

That's it. `PineForge::pineforge` is an `IMPORTED INTERFACE` target that
carries:

- the include directory containing `<pineforge/pineforge.h>`
- the static library `libpineforge.a`
- the standard C++ runtime (since the static lib is C++)
- the compile option `-ffp-contract=off`, so a translation unit that links
  it (a strategy's `generated.cpp` above all) rounds every `*` and `+` on its
  own, as `libpineforge.a` and TradingView's runtime do, instead of fusing
  them into one FMA on ARM64 or FMA-enabled x86 (`PineForge::kernel`
  carries it too)

## Locating a non-default install

If you installed to a non-standard prefix:

```bash
cmake -B build -DPineForge_DIR=/opt/pineforge/lib/cmake/PineForge
```

Or set `CMAKE_PREFIX_PATH=/opt/pineforge`.

## Version selection

```cmake
find_package(PineForge 1.0 REQUIRED)         # 1.0.0 or any later 1.x.y
find_package(PineForge 1.0.0 EXACT REQUIRED) # exactly 1.0.0
```

The package config is `SameMajorVersion`: a minimum pins its major version, so
`find_package(PineForge 0.14 REQUIRED)` does not find a 1.x install, and
`1.0` does not find a 0.x one. Within a major version PineForge guarantees C
ABI back-compat — see [ABI stability](@ref abi_stability) — so a minimum `1.x`
(any compatible later 1.x.y) is the recommended pin.

`find_package` compares MAJOR.MINOR.PATCH only. A release candidate installs
as `PineForge_VERSION` `1.0.0` with `PineForge_VERSION_FULL` `1.0.0-rc.1`, so
`find_package(PineForge 1.0.0 EXACT)` accepts `1.0.0-rc.1` as well; a project
that must tell a candidate from its release compares `PineForge_VERSION_FULL`,
the value `pf_version_string()` returns. Generated strategy code pairs with
exactly that full version of the engine (see
[Public contract](@ref public_contract)).

## Linking from a hand-written Makefile

```make
CFLAGS  += -I$(PREFIX)/include -ffp-contract=off
LDFLAGS += -L$(PREFIX)/lib
LDLIBS  += -lpineforge -lstdc++ -lm

runner: runner.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
```

`-lstdc++` is required even from C TUs because the runtime is C++ inside.
`-lm` covers the `math.h` calls inside the runtime's TA classes.
`-ffp-contract=off` is the option the CMake package hands every consumer;
compile a strategy's `generated.cpp` with it too (`CXXFLAGS`).

## Linking from pkg-config

PineForge does **not** ship a `pkg-config` `.pc` file (the CMake config
is the canonical surface). If you need one, generate it from the
`PineForgeConfig.cmake` properties at install time, or hand-roll one:

```ini
# pineforge.pc
prefix=/usr/local
includedir=${prefix}/include
libdir=${prefix}/lib

Name: pineforge
Description: Deterministic PineScript v6 backtest runtime
Version: 0.14.0
Cflags: -I${includedir} -ffp-contract=off
Libs: -L${libdir} -lpineforge -lstdc++ -lm
```

## Loading a compiled strategy .so

A compiled PineForge strategy is a separate shared object that **also**
exports the public C ABI symbols (`strategy_create`, `run_backtest`,
…). Each strategy `.so` statically links `libpineforge.a` internally;
the runtime is not a separate runtime DSO.

You don't need PineForge installed on the *target* machine to run a
prebuilt strategy `.so` — you only need it to **build** new strategies.

```c
#include <pineforge/pineforge.h>
#include <dlfcn.h>

typedef pf_strategy_t (*strategy_create_fn)(const char*);
typedef void (*run_backtest_fn)(pf_strategy_t, pf_bar_t*, int, pf_report_t*);

void *h = dlopen("./my_strategy.so", RTLD_NOW);
strategy_create_fn create = dlsym(h, "strategy_create");
run_backtest_fn    run    = dlsym(h, "run_backtest");
/* ... */
```

See [Lifecycle](@ref lifecycle) for what to do with the handle once you
have it.

## Verifying the install picked up the right version

```cmake
find_package(PineForge REQUIRED)
message(STATUS "PineForge ${PineForge_VERSION_FULL} from ${PineForge_DIR}")
```

If `PineForge_VERSION_FULL` doesn't match what you installed, your
`CMAKE_PREFIX_PATH` or `PineForge_DIR` is pointing at a different copy.
