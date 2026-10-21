# TNX-CX c18_3: buy_hold_return overflow

Pinned before the producer edits from selected-window wire v1.3 revision 3,
Appendix A lines 74, 83 and 93: a computed equity metric must be finite or
have an existing defined absence. Overflow requires R(nonfinite), whose
catalog entry is revision 2 section 8, line 149. No alternate buy/hold or
CAGR calculation is defined by those clauses.

One fixed long entry, qty 0.5, at 1, with pointvalue 1e300 and margin_long=100.
Initial capital 1e301 funds the entry without leverage. mincontract=0.5
admits the exact fractional quantity. Every price and input is finite.
The 40 synthetic minute bars start at 1704067200000: the first two have all
OHLC prices 1, and the remaining 38 have all prices 2.5e8. Volume is 1.
The entry submitted on bar 0 fills at bar 1's open; the position remains
open until the one range-end report row.

Expected base behavior (to be verified by the lab): completed, exactly one
long trade/open-at-end row, finite settlement and final equity, finite
buy_hold_return_pct = 24999999900, and null buy_hold_return and cagr. The
account profit is 0.5 * 1e300 * (2.5e8 - 1), below DBL_MAX; the buy/hold
metric uses all capital, 1e301 * (2.5e8 / 1 - 1), above DBL_MAX. The CAGR
pow also overflows over this short span. The first failing metric in the
existing computation order is buy_hold_return.

The expectation is failed/report_result_invalid with field
metrics.equity.buy_hold_return and reason nonfinite. min_trades is 0 because
driver.py requires it on failed rows, which have no success report/trade
array. The base receipt must separately show one trade; this is not a
zero-trade control. tests/test_metrics_overflow.cpp isolates both buy/hold
and CAGR failures and also preserves finite and defined-absence controls.

Authored only: no compiler, executable, probe, checker or test was run here.
The supervisor must retain both base-red and fixed-green receipts with the
same probe bytes, toolchain and engine build freshness evidence. A source
construction refusal or an earlier execution failure is not the intended red.
