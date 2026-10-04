# session_transition_clock: time(tf, session, tz) across a daylight-saving switch

TradingView tapes for the session clock rule, replayed by
`tests/test_session_transition_clock_tapes.cpp`.

## The rule

`time(tf, session, tz)` and `time_close(...)` with an intraday `tf` and a valid
session argument. In zone `tz`, a session occurrence is a day window
(start < end) on local date D, or an overnight window (start > end) that ends
on local date D.

1. An occurrence **deviates** when a switch falls on D and the window's start
   or end wall time equals the switch's pre-transition wall time: New York
   02:00 in both directions; London 02:00 in autumn and 01:00 in spring.
2. A deviating occurrence uses the post-transition offset throughout. It ends
   at D@end - post offset and owns the bars after the end of the D-1
   occurrence, up to and including its own end. Such a bar is in the session
   when its HH:MM on the post-transition offset lies in [start, end) (wrapped
   for an overnight window). This produces bars before the switch that read in
   session ("phantoms"): New York 06:15-06:45 UTC on 2025-11-01 and London
   01:15-01:45 UTC on 2025-10-25. New York's 1800-0200 on 2025-11-01 opens at
   18:00 EST (23:00 UTC), not 18:00 EDT.
3. Any bar no deviating occurrence owns:
   - day window: t in [I(D, start), I(D, end)). A repeated wall time takes the
     earlier instant. A skipped wall time takes the post-transition offset
     without moving (wall - post offset).
   - overnight window: the local wall time is in [(D-1)@start, D@end) and
     t < I(D, end). A repeated end takes the later instant, so both passes of
     the repeated hour count. A skipped end cuts without moving.

The value is the bar's tf bar, counted from the occurrence's first instant.
For a deviating occurrence that instant is D'@start - post offset, where D' is
the start's date. The engine implements the rule in
`session_argument_intraday_bar` (`src/session_time.cpp`) for a script's own
session argument; the chart's own bar close, which the Pine host reads through
the symbol's session (`detail::symbol_session_time_close`), keeps the plain
wall-clock windows until a tape pins a symbol session with an endpoint at its
zone's switch hour. The test-only switch
`pineforge::detail::session_clock_switches().transition_wall_clock`
(`include/pineforge/session_time.hpp`) turns it off to restore the plain
wall-clock windows the engine read before.

## Provenance

- Exported 2026-10-03 and 2026-10-04. The scripts are self-written synthetic controls. Each one
  prints the na-ness of a few `time(timeframe.period, "<session>", "<zone>")`
  sites into its order comment on every bar of a window around the 2025
  autumn and 2026 spring switches.
- Exported with `lab tv --no-note` on BINANCE:ETHUSDT.P 15, 2025-04-01 to
  2026-05-01. Nothing was published and no note was recorded. No third-party
  source was sent to TradingView.
- Each control was exported twice, and the two `tv_trades.csv` files are
  byte-identical. `ny-fallback-1800-0200-stamp` is an earlier single-export
  tape. Its comment prints the value itself (`SESSION=<ms>` or `NaN`).
- The rule was written into a predictor (sha256 a34016eb...bcc527) and frozen
  before the exports of `ny-round4` and `london-round4`. Those two are the
  independent confirmation round. Across all of them there are 0
  counterexamples.
- Each directory holds the script (`strategy.pine`, exact bytes), TradingView's
  export (`tv_trades.csv`, exact bytes; times are UTC+8) and
  `provenance.json` (symbol, interval, range, sha256 of both, repeats, original
  control name and export path, predictor sha256).

| Directory | Control | Zone | Sessions | Dates (UTC) |
|---|---|---|---|---|
| ny-fallback-round1 | n9-s1-fallback | America/New_York | 1800-0200, 0300-1200, 0800-1700, 0100-0400 | 2025-10-31..11-04 |
| ny-spring-round1 | n9-s2-springforward | America/New_York | same | 2026-03-06..03-10 |
| ny-fallback-round2 | n9-s3-ny-fallback | America/New_York | 2000-0300, 2300-0130, 0130-0230 | 2025-10-31..11-03 |
| ny-spring-round2 | n9-s4-ny-springforward | America/New_York | same | 2026-03-06..03-09 |
| london-autumn-round2 | n9-s5-london-fallback | Europe/London | 2200-0200, 1900-0130, 0030-0300 | 2025-10-24..10-27 |
| london-spring-round2 | n9-s6-london-springforward | Europe/London | same | 2026-03-27..03-30 |
| ny-round3 | n9-s7-ny-round3 | America/New_York | 2100-0200, 1800-0159, 0200-0300, 1730-0200 | both switches |
| london-round3 | n9-s8-london-round3 | Europe/London | 2300-0100, 2000-0200, 0100-0200, 2330-0030 | both switches |
| ny-round4 | n9-s9-ny-round4 | America/New_York | 0200-0400, 2200-0200, 0230-0330, 1900-0300 | both switches |
| london-round4 | n9-s10-london-round4 | Europe/London | 0100-0300, 2100-0100, 0200-0400, 2300-0200 | both switches |
| ny-fallback-1800-0200-stamp | px-rule-dst | America/New_York | 1800-0200 (value) | 2025-11-01 18:00..11-02 12:00 |

## What the test checks

For every Entry row of every tape, and for every session that the row's
Signal prints, the test calls exactly what codegen emits for the site:
`pine_time(bar, <tf>, "<session>", "<zone>", script_tf_, syminfo_.timezone,
syminfo_.session)`, with script_tf_ = "15", syminfo.timezone = "Etc/UTC" and
syminfo.session = "24x7". The bar is the row's time minus 8 hours. The engine
must agree with TradingView on whether the value is na, and on the value
itself where the tape prints one. The sites and the comment keys are read from
each `strategy.pine`. The test prints rows checked and mismatches per fixture
and fails on any mismatch.

It then turns the switch off and requires the same rows not to match every
time; the switch must be reverting real behaviour. It also requires two known
deviating bars to read the old answers when off and TradingView's when on:
New York 1800-0200 at 2025-11-01 22:00 UTC (old: in session from 18:00 EDT;
TradingView: na) and at 06:15 UTC (old: na; TradingView: in session).

## Not pinned by these tapes

Only New York and London were taped. Other zones follow the rule's general
form (an occurrence deviates when its start or end equals the switch's
pre-transition wall time), which TradingView has not confirmed. A window
spelled to end at `0000` is read as a day window ending at 24:00 on its start
date, so in a zone that switches at midnight (America/Santiago) it never
deviates, where the rule's overnight reading would. Day lists around a
deviating occurrence, several windows in one session string, a `tf` other than
the chart's, `time_close` and the stamp of a phantom bar are unverified too.
