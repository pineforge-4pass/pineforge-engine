"""Paths for benchmark OHLCV + per-strategy fixtures.

Open-source checkouts should use ``benchmarks/assets/`` — a **private** git
submodule whose tree is ``data/`` (pinned OHLCV) and ``strategies/`` (per-strategy
``.pine`` copies, ``tv_trades.csv``, engine trade lists, cloud-compiled Pyne,
``_indicators/``). Those artefacts are TradingView-linked validation data and
are not redistributed inside the public engine repo.

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
