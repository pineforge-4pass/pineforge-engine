# Opening partial close slippage

An independent, campaign-authored TradingView synthetic exported with
`lab tv --no-note` on October 3, 2026, NSE:NIFTY 1D, June 13–20, 2025.
The export has covered range proof and two closed trades. The June 17
opening long execution is 24977.90; its default, non-immediate 50% close
books 24977.80, one sell-slippage tick below the raw 24977.85 opening
print, not below the already-slipped 24977.90 execution.

This independently pins the single-cohort partial-close correction used
by the Random37 quiet-bar witness. Full non-immediate closes have the
existing coof_open_limit/td-fp17-coof-open LC and SD exports; immediate
closes have the parent fixture's ES export. Cross-cohort next-tick closes
are intentionally outside this correction. CSV timestamps are UTC+8.

Source SHA-256: fbcf41dd1045dc071e7c40380bb33d9a758036c0bf07e7766dad5187ac21ea35.
TV tape SHA-256: 5427c14f181877264ff2c3ad428971f264ac85c1dbaf483bf32806c6f2b9ee50.
The feed rows are the corresponding committed TradingView chart bars
from coof_open_limit/bars.inc; the focused test checks both trades.
