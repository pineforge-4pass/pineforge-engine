# Realtime contract three-session corpus report

Run date: 2026-07-11

This report compares a bar-only run with same-instance OHLCV warmup plus one
contiguous normalized ETHUSDT-perpetual trade tape. Every session ends
exclusively at `2025-05-01T00:00:00Z`; no artificial chunks or short sessions
were used.

## Inputs

- Corpus: 252 compiled validation probes, three starts (756 runs).
- Starts: `2025-02-07T03:17Z`, `2025-03-11T14:23Z`,
  `2025-03-29T22:41Z`.
- Tape: 424,801,937 records, 13,593,661,984 bytes, 32-byte
  `pf_trade_tick_t` records.
- Runtime: 293.59 seconds including warmup, all stream runs, bar baselines,
  scoring, and report generation (tape construction excluded).
- Branch base at experiment time: `6d6e369`; the final implementation commit is
  recorded by draft PR #90.

## Scores

| Handoff | Runtime | Input bars | Script bars | Trade count exact | Fully structural | Weighted ordered match |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 2025-02-07 03:17 | 252/252 | 252/252 | 252/252 | 241/252 | 228/252 | 99.0363% |
| 2025-03-11 14:23 | 252/252 | 252/252 | 252/252 | 242/252 | 234/252 | 99.3784% |
| 2025-03-29 22:41 | 252/252 | 252/252 | 252/252 | 243/252 | 235/252 | 99.5667% |

`Fully structural` requires exact trade count and exact ordered structural
signature. Weighted ordered match uses direction, entry/exit minute, entry/exit
bar index, and quantity, with the denominator equal to the larger trade list.

Across probes, the median p50/p90/p99 entry and exit price delta is zero for
all sessions. At the 90th percentile probe, the per-probe p99 deltas were:

| Handoff | Entry p99 (bps) | Exit p99 (bps) |
| --- | ---: | ---: |
| 2025-02-07 03:17 | 0.0935 | 0.1595 |
| 2025-03-11 14:23 | 0.0612 | 0.0633 |
| 2025-03-29 22:41 | 0.0559 | 0.0601 |

Absolute summed P&L differences across all 252 probes were $2,339.34,
$1,500.51, and $1,051.71 respectively. These are reporting aggregates, not a
portfolio P&L, because every probe has its own independent account.

## Determinism and diagnostics

All 252 runs in every session emitted a canonical lifecycle hash. Total
lifecycle transitions were 4,984,478 / 3,112,033 / 1,966,731. Retained event
capacity overflowed for active probes (1,042,018 / 428,753 / 94,124 dropped
records), but the rolling count/hash includes dropped records by contract.

The machine-readable report is generated at
`build/realtime_contract_3_sessions.json`. It includes per-probe price/P&L
distributions, first divergence, and retained lifecycle context.

## Interpretation and remaining gate items

The hard runtime and bar-continuity gates pass. Tick execution is expected to
differ slightly from inferred bar paths; the high ordered-match percentages
show that divergence is concentrated. This run is not a claim of exact
TradingView realtime parity.

The revised design's full completion gate remains open: a frozen pre-change
run with the same scorer, TradingView probes P1-P9, per-lot exit fan-out, the
whole-engine handoff digest, and a deterministic second execution of all 756
sessions are not supplied by this post-change report.
