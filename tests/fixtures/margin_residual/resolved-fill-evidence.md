# PX-F2B synthetic evidence

`resolved-fill-evidence.json` preserves the exact source and CSV bytes (as JSON
strings), metrics and export provenance for independently authored controls.
All exports use `lab tv --no-note`, `ws-report-v1`, 15-minute charts. No
population source or modified population source was submitted to TradingView.

The five EURUSD raw-floor controls pin the existing famR sizing, admission
and whole-drop gates. They already pass on wave a747ad5b; no sizing change is
made. Their UTC signal is 2025-04-02 19:45, with closure at 20:15.

The five ETHUSDT.P capital controls and the slippage-0/1/3 controls pin the
repeated short margin execution price. A call that rechecks its resolved fill
uses that same fill again, without another exit-side slippage application.
Slippage 0/1 and funded capital controls preserve adverse-extreme calls.
The UTC opening signal is 2025-04-08 13:45, with closure at 14:30.

The extra slippage-5 export is a **remaining initial-checkpoint divergence**,
not a passing full-cascade pin: TV takes 0.0012 at 1567.72 initially; the wave
starts with 0.0004. It is retained to prevent claiming the margin schedule is
fully solved. The price-only repair does not change that first quantity.

The independent forward predictor in the lane scratch reproduces all 13
in-scope tapes, including dropped EURUSD entries, at quantity/price/time
precision and PnL to the cent. Sub-cent PnL display differences are not an
engine grader or tolerance change. The four fail-before assertions on the
wave are the two repeated-call prices and their PnL in the capital controls.

The active regression unit is `test_margin_residual_resolved_fill`. Historical
`test_tv_money_precision_l4b` and `test_carried_pooc_short_margin_state_l4a`
twins stay byte-identical to the wave, preserving their literal-parity pins.
The new unit uses source hosts and real exported chart bars.
