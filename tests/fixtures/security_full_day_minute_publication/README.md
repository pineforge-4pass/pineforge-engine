# Full-day minute request publication

This independently authored synthetic strategy compares 1440-minute and D
request contexts on NASDAQ:AAPL, 15 minutes, over 2025-04-01 through
2026-05-01. Both requests use the default gaps_off and lookahead_off.
Trade timestamps are UTC+8; the comment fields contain UTC milliseconds.

At Friday 2026-03-06 20:30 UTC the requested 1440 context still refers to
Thursday and RSI[1] is 49.1996039707. At Friday 20:45 UTC, the last chart
bar of the regular session, it publishes Friday's completed context and
RSI[1] becomes 47.1139497967. Monday 2026-03-09 13:30 UTC retains that
value. The requested context closes at 21:00 UTC Friday, before the DST
transition, rather than waiting for Monday's first input.

1440 and D are not interchangeable expressions: their timeframe.period
values are 1440 and 1D, and Friday's D RSI[1] is 47.1118594402. The
adapter change completes the minute-aggregated context on the session's
last input; it does not substitute native daily OHLC or normalize the
timeframe spelling.

The active test_full_day_minute_context_publishes_on_session_close unit
in tests/test_aux_security_feed_l4d.cpp supplies embedded synthetic
Friday and Monday bars with America/New_York regular-session metadata.
It asserts exactly one completed publication per session, on its final
chart bar, including the DST shift.

## Session-shape controls

The same independently authored clock control, with its sampling window
widened to March 6–9, 2026, was exported using `lab tv --no-note` on each
of these 15-minute charts. Every export has `rangeProof=covered`; each
directory retains the full synthetic source, trade tape and provenance.
All three publish the 1440 context on the final chart bar of the session,
not on the next session's first bar:

| Fixture | Symbol | Friday publication UTC | Monday publication UTC |
| --- | --- | --- | --- |
| `overnight_cfd` | OANDA:XAUUSD | March 6 21:45 | March 9 20:45 |
| `overnight_futures` | CME_MINI:NQ1! | March 6 21:45 | March 9 20:45 |
| `regular_index` | NSE:NIFTY | March 6 09:45 | March 9 09:45 |

The first two include the daily maintenance break and the March 8 DST
transition. This does not claim evidence for every multi-window session.
The requested `MC` equals the publishing chart bar's `TC` in all six cases.

The `early_close_cfd` and `early_close_futures` controls sample November
27 through December 1, 2025. CME_MINI:NQ1! publishes at November 27 17:45
UTC and November 28 18:00 UTC, its actual final chart bars. OANDA:XAUUSD
does not publish Friday's shortened context on its last Friday bar: it
first publishes that context at Sunday November 30 23:00 UTC, retaining
the nominal Friday close of November 28 22:00 UTC. Consequently the
adapter's next-session completion arm respects `early_close_completes()`;
the nominal-close arm still applies to both exchange and OTC streams.

`test_full_day_minute_split_feed_respects_early_close_policy` embeds a
shortened Friday auxiliary slice and a full Monday slice. It asserts that
exchange metadata publishes Friday immediately, CFD metadata waits for
Monday's first bar, and both publish Monday at its nominal close.
