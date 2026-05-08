# CMake integration {#integration_cmake}

@tableofcontents

PineForge installs a standard CMake **package config**. Downstream
projects pull it in with one `find_package` call.

## Minimal `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_strategy_runner LANGUAGES C)

find_package(PineForge 0.1 REQUIRED)

add_executable(runner runner.c)
target_link_libraries(runner PRIVATE PineForge::pineforge)
```

That's it. `PineForge::pineforge` is an `IMPORTED INTERFACE` target that
carries:

- the include directory containing `<pineforge/pineforge.h>`
- the static library `libpineforge.a`
- the standard C++ runtime (since the static lib is C++)

## Locating a non-default install

If you installed to a non-standard prefix:

```bash
cmake -B build -DPineForge_DIR=/opt/pineforge/lib/cmake/PineForge
```

Or set `CMAKE_PREFIX_PATH=/opt/pineforge`.

## Version selection

```cmake
find_package(PineForge 0.1 REQUIRED)        # any 0.1.x
find_package(PineForge 0.1.1 EXACT REQUIRED) # pinned
```

Within a major version PineForge guarantees C ABI back-compat — see
[ABI stability](@ref abi_stability) — so `0.1` (any compatible
0.x.y) is the recommended pin.

## Linking from a hand-written `Makefile`

```make
CFLAGS  += -I$(PREFIX)/include
LDFLAGS += -L$(PREFIX)/lib
LDLIBS  += -lpineforge -lstdc++ -lm

runner: runner.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
```

`-lstdc++` is required even from C TUs because the runtime is C++ inside.
`-lm` covers the `math.h` calls inside the runtime's TA classes.

## Linking from `pkg-config`

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
Version: 0.1.1
Cflags: -I${includedir}
Libs: -L${libdir} -lpineforge -lstdc++ -lm
```

## Loading a compiled strategy `.so`

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
message(STATUS "PineForge ${PineForge_VERSION} from ${PineForge_DIR}")
```

If `PineForge_VERSION` doesn't match what you installed, your
`CMAKE_PREFIX_PATH` or `PineForge_DIR` is pointing at a different copy.
