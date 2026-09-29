# Raw timing files

These are the timing files behind the speed figures in [`../../README.md`](../../README.md) (the headline), [`../speed.md`](../speed.md) and [`../../throughput/README.md`](../../throughput/README.md), as the measuring windows wrote them. [`manifest.json`](manifest.json) records each file's sha256, what it holds, the engine it timed and where it came from. [`../../check_provenance.py`](../../check_provenance.py) checks the files against the manifest and derives every number of the README headline from them or from another committed file:

```bash
python3 benchmarks/check_provenance.py
```

No file here was re-timed after its window. Each directory is one measuring window. The 2026-06-11 and 2026-09-22 windows ran on the same Apple M4 Max host. The 2026-09-29 window, the one the headline publishes, ran on an AWS c7a.8xlarge (AMD EPYC 9R14, 32 cores, SMT off) running Ubuntu 24.04 (`2026-09-29-35db01c8/host.json`), so its times are compared only with times of the same window.

| Directory | Window | Engine | What it backs |
|---|---|---|---|
| `2026-09-29-35db01c8/` | BENCH-REFRESH | `35db01c8` | The published figures: the PineForge sweep (`pf_speed_a.json`, slots 001–100, and `pf_speed_b.json`, 101–201), PyneCore 6.10.3, PineTS and vectorbt. The same-window A/B of `35db01c8` against `063e4460` on all 201 slots and against the 2026-06-11 engine `ac011d84` on its three probes (`ab/`), each engine running its own `generated.cpp`; the PyneCore 6.10.3 against 6.10.2 A/B (`pc_ab/`); the five throughput-package runs (`throughput/`); the gate reading before every batch (`gate.tsv`), a 10 s load trace of every batch (`loads/`, `throughput/r<N>-load.tsv`) and the host's facts (`host.json`) |
| `2026-09-22-063e4460/` | BENCH3 | `063e4460` | The 2026-09-22 table: the gate reading before each PineForge sweep batch, the five throughput-package runs with their load traces, and the paired A/B of `063e4460`, `e9ad37dd` and the 2026-06-11 engine `ac011d84` |
| `2026-09-22-e9ad37dd/` | BENCH2 | `e9ad37dd` | The published PyneCore, PineTS and vectorbt timings, BENCH2's PineForge sweep (superseded, kept for the cross-window comparison in `speed.md`) and the gate readings |
| `2026-06-11-94596bf/` | the 2026-06-11 refresh | `94596bf` | The 2026-06-11 table: PineForge, PyneCore, PineTS and vectorbt timings, including the median 162× |

The 2026-06-11 table was published in commits `ac011d84` and `933fe583`. Both change documentation only, so their engine is `94596bf`'s. That is the "2026-06-11 engine `ac011d84`" the A/B rebuilt.

## Edits

In the 2026-09-29 window's Google Benchmark files (the sweep, the A/B passes and the throughput runs), `context.host_name` was rewritten from the timing host's name to `aws-c7a-8xlarge` and `context.executable` from its absolute build path to `./build/bin/pineforge_bench`. The median throughput run, `throughput/r<N>.json` as the manifest names it, is byte-identical to `benchmarks/throughput/benchmark_results.json` after that edit. `host.json` holds the host's facts from `lscpu`, `/etc/os-release` and the toolchain, without its paths. Every other file of that window is as measured.

In the older windows, two kinds of file were edited: the five 2026-09-22 throughput runs and the 2026-06-11 Google Benchmark file. In each, `context.executable` was rewritten from the measuring host's absolute build path to `./build/bin/pineforge_bench`, as the BENCH2 window did for the throughput package's JSON. In every Google Benchmark file of those windows, `context.host_name` was also rewritten from the measuring machine's name to `apple-m4-max`. In the BENCH2 PineForge and PyneCore files and the five 2026-09-22 throughput runs, every closed slot's name, `NNN-<author>-<title>`, was rewritten to `NNN-closed`, as every report prints it: the directory name carries the TradingView author's handle. Each edited file's manifest entry keeps the sha256 of the file as it was measured (`origin_sha256`) and names its edits (`edit`).

## What is not here

- **BENCH3's PineForge sweep at `063e4460`**, the Google Benchmark files behind the 2026-09-22 table's PineForge column (89.4 ms, 603k bars/s on the Apple M4 Max). The files lived in the measuring checkout's gitignored `benchmarks/_workdir/`, which was deleted with the checkout. The finest record left is the per-strategy table of `speed.md` at commit `35db01c8`: its "PF median (ms)" column, to two decimals. The 2026-09-29 window timed `063e4460` again, on the AWS host (`ab/old1.json`, `ab/old2.json`); those files are that engine on another host, not the lost ones.
- **BENCH2's June-engine run** (5.34, 5.84 and 8.40 ms in `speed.md` at commit `35db01c8`). It was printed and not kept.
- **BENCH2's five throughput runs at `e9ad37dd`.** The published run (median 0.614 M bars/s) is `benchmarks/throughput/benchmark_results.json` at commit `cd581082`.
- **BENCH3's two discarded throughput runs.** The run gated at 12:08 UTC saw the load reach 16.8 before its benchmark began. The run gated at 13:09 UTC hit the session's time limit. Neither was published.
- **The closed slots' artifacts.** They are never committed. The evidence-store object named in the manifest's `evidence` entry holds them.

The 2026-09-22 `speed.md` (at commit `35db01c8`) quotes the load during each of its throughput runs as a median and a maximum. Over each whole trace (`2026-09-22-063e4460/throughput/r<N>-load.tsv`) the maxima match and the medians differ by at most 0.07; which samples were used was not recorded.
