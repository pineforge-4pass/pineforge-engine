# Raw timing files

These are the timing files behind the speed figures in [`../../README.md`](../../README.md) (the headline), [`../speed.md`](../speed.md) and [`../../throughput/README.md`](../../throughput/README.md), as the measuring windows wrote them. [`manifest.json`](manifest.json) records each file's sha256, what it holds, the engine it timed and where it came from. [`../../check_provenance.py`](../../check_provenance.py) checks the files against the manifest and derives every number of the README headline from them or from another committed file:

```bash
python3 benchmarks/check_provenance.py
```

Nothing here was re-timed. Each directory is one measuring window on the same Apple M4 Max host.

| Directory | Window | Engine | What it backs |
|---|---|---|---|
| `2026-09-22-063e4460/` | BENCH3 | `063e4460` | The gate reading before each PineForge sweep batch, the five throughput-package runs with their load traces, and the paired A/B of `063e4460`, `e9ad37dd` and the 2026-06-11 engine `ac011d84` |
| `2026-09-22-e9ad37dd/` | BENCH2 | `e9ad37dd` | The published PyneCore, PineTS and vectorbt timings, BENCH2's PineForge sweep (superseded, kept for the cross-window comparison in `speed.md`) and the gate readings |
| `2026-06-11-94596bf/` | the 2026-06-11 refresh | `94596bf` | The 2026-06-11 table: PineForge, PyneCore, PineTS and vectorbt timings, including the median 162× |

The 2026-06-11 table was published in commits `ac011d84` and `933fe583`. Both change documentation only, so their engine is `94596bf`'s. That is the "2026-06-11 engine `ac011d84`" the A/B rebuilt.

## Edits

Three edits were made. The first touched the five throughput runs and the 2026-06-11 Google Benchmark file: in each, `context.executable` was rewritten from the measuring host's absolute build path to `./build/bin/pineforge_bench`, as the BENCH2 window did for `throughput/benchmark_results.json`. The manifest keeps each original's sha256 (`origin_sha256`). After the edit, throughput run `r2.json` is byte-identical to `benchmarks/throughput/benchmark_results.json`, the run the package publishes. The second rewrote `context.host_name` from the measuring machine's name to `apple-m4-max` in every Google Benchmark file here and in `benchmarks/throughput/benchmark_results.json`; each manifest entry's `origin_sha256` is the sha256 of the file as it was measured. The third rewrote every closed slot's name, `NNN-<author>-<title>`, to `NNN-closed` in the BENCH2 PineForge and PyneCore files and the five throughput runs, as every report prints it: the directory name carries the TradingView author's handle. The manifest's `edit` field names each file's rewrites.

## What is not here

- **BENCH3's PineForge sweep at `063e4460`**, the Google Benchmark files behind the published PineForge column (89.4 ms, 603k bars/s). The files lived in the measuring checkout's gitignored `benchmarks/_workdir/`, which was deleted with the checkout. The finest record left is the per-strategy table in `speed.md`: its "PF median (ms)" column, to two decimals. `check_provenance.py` derives the headline PineForge figures from that column. Regenerating the raw files takes a re-timing on a quiet host.
- **BENCH2's June-engine run** (5.34, 5.84 and 8.40 ms in `speed.md`). It was printed and not kept.
- **BENCH2's five throughput runs at `e9ad37dd`.** The published run (median 0.614 M bars/s) is `benchmarks/throughput/benchmark_results.json` at commit `cd581082`.
- **BENCH3's two discarded throughput runs.** The run gated at 12:08 UTC saw the load reach 16.8 before its benchmark began. The run gated at 13:09 UTC hit the session's time limit. Neither was published.
- **The closed slots' artifacts.** They are never committed. The evidence-store object named in the manifest's `evidence` entry holds them.

`speed.md` quotes the load during each throughput run as a median and a maximum. Over each whole trace (`throughput/r<N>-load.tsv`) the maxima match and the medians differ by at most 0.07; which samples were used was not recorded.
