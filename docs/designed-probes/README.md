# Designed probes — input.source / input.color / syminfo (issues #9, #23, #19)

Probe **designs** for the features resolved by engine PR #25 + codegen PR #18.
They are staged here (not under `corpus/validation/`) because each needs a
**manual TradingView export** (`tv_trades.csv`) before it can join the
TV-parity sweep — dropping a probe without `tv_trades.csv` into
`corpus/validation/` would fail `run_corpus.sh` (counts as `missing`).

## How to promote a probe into the corpus

1. Open the probe's `strategy.pine` in TradingView with the **TV setup** from
   its README (symbol, timeframe, any input overrides).
2. Export **List of trades** → save as `tv_trades.csv` in the probe dir.
3. Move the probe dir into the corpus submodule:
   `corpus/validation/<slug>/`.
4. Regenerate the C++:
   `cd ../pineforge-codegen && python scripts/regen_corpus_cpp.py`
5. Build + verify: `./scripts/run_corpus.sh` (the new probe should land
   `excellent`).

## Probe index

| Slug | Feature | Data | TV capture |
|---|---|---|---|
| `input-source-runtime-override-high-01` | #23/#9 — runtime override of input.source | ETH-USDT 15m (existing) | run with **Source = high** |
| `input-source-subscript-hl2-01` | #23/#9 — `src[1]`/`src[2]` series binding on input.source | ETH-USDT 15m (existing) | defaults (Source = hl2) |
| `input-color-packed-defval-01` | #9 — packed-ARGB color defval | n/a (cosmetic) | none — compile/UI golden, no trade effect |
| `syminfo-metadata-injection-eng-only-01` | #19 — fundamental metadata injection gate | ETH-USDT 15m (existing) | **engine-only** — TV fundamentals differ; no parity expected |
| `us-equity-exchange-tz-intraday-cap-01` | #19/#26 — intraday-cap day rollover on a real US session | **NEEDS US-equity OHLCV** | run NYSE/NASDAQ RTH, chart tz = America/New_York |

The last probe is the deferred dependency tracked in engine issue #26 — it
cannot be built until a real US-equity intraday OHLCV feed is added to
`corpus/data/` and a matching TV run is captured.
