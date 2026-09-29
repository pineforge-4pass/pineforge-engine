#!/usr/bin/env python3
"""TradingView's session calendar for a chart's symbol, as a pinned run input.

TradingView's calendar for a symbol is not its feed. OANDA:XAUUSD's keeps
sessions its feed holds no bar in -- the 17:00-18:00 ET hour every day, US
holidays (Good Friday), the rest of an early-close day (July 4, the Friday
after Thanksgiving) -- and TradingView reads that calendar for the bar after
the current one: ``time("", "", -1)``, ``session.islastbar`` and
``session.islastbar_regular`` (lab tv te_session_calendar_xau15-*, lane
TAIL-E). NASDAQ:AAPL's calendar carries the exchange's holidays and early
closes, which its feed shows too.

A calendar is captured from TradingView by a synthetic script
(tests/fixtures/symbol_calendar/xc-cal-*/strategy.pine, ``lab tv --no-note``)
whose first chart bar of every session day spells that day's ``time("D")`` and
``time_close("D")`` and the next three days', so a day the chart holds no bar
of is read from the day before it. ``calendar_from_tape`` turns the tape into
the calendar document; run_strategy.load_session_calendar reads one back for
a run (``PINEFORGE_RUN_SESSION_CALENDAR``), whose Pine host then reads the
next bar from it (PineStrategyHost::set_symbol_calendar).

Document (``pineforge-symbol-calendar/v1``, JSON)::

    {"schema": "pineforge-symbol-calendar/v1", "symbol": "OANDA:XAUUSD",
     "source": {...the tape's provenance...},
     "sessions": [[open_ms, close_ms], ...]}   # ascending, one per session day

usage: symbol_calendar.py <tape-dir> <out.json>
"""

from __future__ import annotations

import csv
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from run_strategy import SESSION_CALENDAR_SCHEMA as SCHEMA  # noqa: E402
from run_strategy import load_session_calendar  # noqa: E402,F401 -- the reader


def _ms(word: str) -> int | None:
    """``yyMMdd-HHmm`` (UTC) as epoch ms; None for ``na``."""
    if word == "na":
        return None
    moment = datetime.strptime(word, "%y%m%d-%H%M").replace(tzinfo=timezone.utc)
    return int(moment.timestamp() * 1000)


def calendar_from_tape(tv_trades_csv: Path) -> list[tuple[int, int]]:
    """Every session day the tape's comments spell -- ``d|<bar>|<open>~<close>``
    for the bar's own day and the next three -- as ascending ``(open_ms,
    close_ms)``. Raises ValueError when two readings of one day disagree or
    the days overlap."""
    days: dict[int, int] = {}
    with tv_trades_csv.open(newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            signal = row.get("Signal") or ""
            if not signal.startswith("d|"):
                continue
            for field in signal.split("|")[2:]:
                open_word, close_word = field.split("~")
                open_ms, close_ms = _ms(open_word), _ms(close_word)
                if open_ms is None or close_ms is None:
                    continue
                if days.setdefault(open_ms, close_ms) != close_ms:
                    raise ValueError(f"day {open_word} read with two closes")
    sessions = sorted(days.items())
    for (o1, c1), (o2, _c2) in zip(sessions, sessions[1:]):
        if not o1 < c1 <= o2:
            raise ValueError(f"days at {o1} and {o2} overlap")
    return sessions


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def calendar_document(tape_dir: Path) -> dict:
    """The calendar document of a ``lab tv`` export directory."""
    metrics = json.loads((tape_dir / "metrics.json").read_text(encoding="utf-8"))
    provenance = metrics.get("wsProvenance") or {}
    sessions = calendar_from_tape(tape_dir / "tv_trades.csv")
    return {
        "schema": SCHEMA,
        "symbol": metrics.get("symbol") or provenance.get("symbol"),
        "source": {
            "tape": tape_dir.name,
            "interval": metrics.get("interval"),
            "channel": metrics.get("tapeChannel"),
            "returnedRange": provenance.get("returnedRange"),
            "tvTradesSha256": _sha256(tape_dir / "tv_trades.csv"),
            "pineSha256": _sha256(tape_dir / "strategy.pine"),
        },
        "sessions": [[o, c] for o, c in sessions],
    }


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    doc = calendar_document(Path(argv[1]))
    out = Path(argv[2])
    out.write_text(json.dumps(doc, separators=(",", ":")) + "\n", encoding="utf-8")
    symbol, sessions = load_session_calendar(out)
    print(f"{symbol}: {len(sessions)} session days")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
