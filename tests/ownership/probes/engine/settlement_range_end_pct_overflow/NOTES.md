# settlement_range_end_pct_overflow

D6 row 22, AFTER-g134 follow-up expectation for append_open_position_report_rows. One unit enters at 1 and remains held to the finite 1e308 final close. The range-end row has finite PnL and overflowing pnl_pct. Forty five-minute bars, first four at 1 and the rest at 1e308, volume 1; mintick=1. This lane does not edit the range-end function; CHANGES.md names the follow-up location. Until g134's guard and its recode land this expectation is intentionally red. No native execution of this source probe has been performed.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
