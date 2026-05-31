# syminfo-metadata-injection-eng-only-01

## Feature under test
Engine #19 — **fundamental metadata injection** reaching strategy behavior:
`syminfo.shares_outstanding_total` resolves through `get_syminfo_metadata`,
which returns `na` unless a feed injected a value
(`strategy_set_syminfo_metadata`, here via
`inputs.json::runtime_overrides.syminfo_metadata`).

## Why ENGINE-ONLY (no TV parity)
TV uses the real fundamental for the charted symbol; an injected synthetic
value won't match. So this has no `tv_trades.csv`. Validate via two **engine**
runs:

1. **With injection** (the provided `inputs.json`): `shares_outstanding_total`
   = 1.6e9 > threshold → `bigCap` true → the dual-MA crossover produces a
   non-empty trade list.
2. **Without injection** (delete the `runtime_overrides.syminfo_metadata`
   block): the field reads `na` → `bigCap` false → **zero** trades.

The difference between the two runs proves the injection path is wired end to
end (harness → `strategy_set_syminfo_metadata` → `get_syminfo_metadata` →
strategy gate).

## Notes
- If/when a real fundamentals data source is added (engine #26), this can be
  promoted to a TV-parity probe on a symbol whose `shares_outstanding_total`
  the feed supplies.
- Belongs in the `engine_only` corpus tier, not `excellent` — do not expect a
  TV match.
