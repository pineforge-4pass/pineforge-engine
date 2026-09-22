"""Paths for benchmark OHLCV + per-strategy fixtures.

Open-source checkouts use ``benchmarks/assets/``, a **public** git submodule
(pineforge-benchmarks-assets, Apache-2.0) whose tree is ``data/`` (pinned OHLCV)
and ``strategies/`` (the 100 public slots: ``.pine`` sources, ``tv_trades.csv``,
engine trade lists, cloud-compiled Pyne, ``_indicators/``). Only the 101 closed
slots are private: TradingView-scraped third-party scripts that maintainers
extract from the evidence store into ``benchmarks/assets-closed/`` (gitignored).

If ``benchmarks/assets/strategies`` is missing, we fall back to inline
``benchmarks/data`` and ``benchmarks/strategies`` for maintainer monorepos or
pre-migration trees only — that layout must not be published in public Git
history once the repo is open-sourced (see CONTRIBUTING.md).
"""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH = REPO_ROOT / "benchmarks"
_ASSETS = BENCH / "assets"
if (_ASSETS / "strategies").is_dir():
    ASSETS = _ASSETS
else:
    ASSETS = BENCH
STRATEGIES = ASSETS / "strategies"
DATA = ASSETS / "data"
# Maintainer-local closed slots (TradingView-scraped, never public); absent in
# public checkouts, where STRATEGY_ROOTS is the public root alone.
CLOSED_STRATEGIES = BENCH / "assets-closed" / "strategies"
STRATEGY_ROOTS = [STRATEGIES] + ([CLOSED_STRATEGIES] if CLOSED_STRATEGIES.is_dir() else [])
