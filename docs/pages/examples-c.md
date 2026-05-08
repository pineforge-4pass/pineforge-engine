# Example — Pure C harness {#examples_c}

@tableofcontents

A complete, self-contained C harness that loads a compiled MACD
strategy `.so`, feeds it OHLCV from a CSV, runs the backtest, prints
the trade list, and frees everything cleanly.

No Python, no scripting, no extra deps beyond `libc`. Total: ~120 lines.

## What you'll build

```
$ ./macd_runner ./strategy.so btcusdt_15m_7d.csv
PineForge 0.1.1 (97c93d3) — 672 bars
trades: 49  net pnl: -190.85
  L 0.000000 -> 0.000000  pnl=+12.40  qty=10.0
  S 0.000000 -> 0.000000  pnl=-22.10  qty=10.0
  ...
```

## Source — `macd_runner.c`

```c
#include <pineforge/pineforge.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every PineForge .so statically links libpineforge.a, so it re-exports
 * the runtime helpers (pf_version_get) alongside the per-strategy ABI.
 * We resolve every symbol via dlsym so the harness binary links nothing
 * but libdl. */
typedef pf_strategy_t (*strategy_create_fn)(const char *);
typedef void          (*strategy_free_fn)(pf_strategy_t);
typedef void          (*run_backtest_full_fn)(pf_strategy_t,
                                              pf_bar_t *, int,
                                              const char *, const char *,
                                              int, int,
                                              pf_magnifier_distribution_t,
                                              pf_report_t *);
typedef void          (*report_free_fn)(pf_report_t *);
typedef pf_version_t  (*pf_version_get_fn)(void);

struct strategy_so {
    void                  *handle;
    strategy_create_fn     create;
    strategy_free_fn       free;
    run_backtest_full_fn   run_full;
    report_free_fn         report_free;
    pf_version_get_fn      version_get;
};

static int load_strategy(const char *path, struct strategy_so *out)
{
    out->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!out->handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return -1; }

    out->create      = (strategy_create_fn)    dlsym(out->handle, "strategy_create");
    out->free        = (strategy_free_fn)      dlsym(out->handle, "strategy_free");
    out->run_full    = (run_backtest_full_fn)  dlsym(out->handle, "run_backtest_full");
    out->report_free = (report_free_fn)        dlsym(out->handle, "report_free");
    out->version_get = (pf_version_get_fn)     dlsym(out->handle, "pf_version_get");

    if (!out->create || !out->free || !out->run_full || !out->report_free) {
        fprintf(stderr, "missing ABI symbol in %s\n", path);
        return -1;
    }
    return 0;
}

/* Minimal CSV reader: timestamp,open,high,low,close,volume per row.
 * Match this against your actual CSV column order — the tutorial's
 * btcusdt_15m_7d.csv puts timestamp first. */
static pf_bar_t *load_csv(const char *path, int *out_n)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }

    char line[1024];
    fgets(line, sizeof line, f);          /* skip header */

    size_t cap = 1024, n = 0;
    pf_bar_t *bars = malloc(cap * sizeof *bars);

    while (fgets(line, sizeof line, f)) {
        if (n == cap) {
            cap *= 2;
            bars = realloc(bars, cap * sizeof *bars);
        }
        sscanf(line, "%lld,%lf,%lf,%lf,%lf,%lf",
               (long long *)&bars[n].timestamp,
               &bars[n].open, &bars[n].high, &bars[n].low,
               &bars[n].close, &bars[n].volume);
        ++n;
    }
    fclose(f);
    *out_n = (int)n;
    return bars;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s STRATEGY.so OHLCV.csv\n", argv[0]); return 2; }

    struct strategy_so so;
    if (load_strategy(argv[1], &so) < 0) return 1;

    int n = 0;
    pf_bar_t *bars = load_csv(argv[2], &n);
    if (!bars) return 1;

    pf_version_t v = so.version_get ? so.version_get() : (pf_version_t){0,0,0,""};
    printf("PineForge %d.%d.%d (%s) — %d bars\n",
           v.major, v.minor, v.patch,
           v.commit_sha[0] ? v.commit_sha : "unknown",
           n);

    pf_strategy_t s = so.create(NULL);
    pf_report_t   r = {0};

    so.run_full(s, bars, n,
                /* input_tf  */ "",
                /* script_tf */ "",
                /* magnifier */ 0, /* samples */ 4,
                PF_MAGNIFIER_ENDPOINTS,
                &r);

    printf("trades: %d  net pnl: %.2f\n", r.trades_len, r.net_profit);
    for (int i = 0; i < r.trades_len; ++i) {
        pf_trade_t t = r.trades[i];
        printf("  %c %.4f -> %.4f  pnl=%+.2f  qty=%.4f\n",
               t.is_long ? 'L' : 'S',
               t.entry_price, t.exit_price, t.pnl, t.qty);
    }

    so.report_free(&r);
    so.free(s);
    free(bars);
    dlclose(so.handle);
    return 0;
}
```

## Build

```bash
cc -O2 -Wall -Wextra \
   -I/usr/local/include \
   macd_runner.c \
   -ldl -o macd_runner
```

(On macOS, drop `-ldl` — `dlopen` lives in `libSystem`.)

The runtime is statically linked **inside** `strategy.so` — the harness
binary itself only links `libdl` (and the `<pineforge/pineforge.h>`
header for the type declarations). Runtime helpers like
#pf_version_get are resolved via `dlsym` from the loaded `.so`, not
linked at compile time.

## Why `dlopen` and not `-lpineforge`?

The PineForge runtime is shipped as a **static library** (`libpineforge.a`)
that gets baked into each compiled strategy `.so`. You never link the
runtime directly into your harness; you link `dl` and pull the per-strategy
ABI symbols out at load time.

This means you can:

- Swap strategies at runtime — no harness rebuild
- Load multiple strategies simultaneously (different `.so` files,
  different handles, no symbol clashes — internal symbols are hidden)
- Ship a single harness binary that runs any future PineForge `.so`

## See also

- [Tutorial: MACD](@ref tutorial_macd) — the Python equivalent
- [CMake integration](@ref integration_cmake) — for harnesses that statically
  link `libpineforge.a` for the version-query symbols
- [Lifecycle](@ref lifecycle) — what to do with the handle once you have it
