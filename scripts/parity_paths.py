#!/usr/bin/env python3
"""Resolve the filesystem roots used by the parity scripts.

The installed parity bundle is flat, so its scripts cannot infer a corpus or
the harness from a repository checkout.  For ``harness_dir`` the resolution
order is an explicit argument, ``PINEFORGE_PARITY_HARNESS_DIR``, then the
source-checkout ``REPO_ROOT / "docker"`` default only when no
``PARITY-MANIFEST.json`` is beside this module.  An installed bundle with no
configured harness returns ``None`` so its adapter can refuse the run without
guessing a sibling directory.
"""
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_ROOT.parent

CORPUS_ENV = "PINEFORGE_PARITY_CORPUS_ROOT"
DERIVED_ENV = "PINEFORGE_PARITY_DERIVED_WORKSPACE"
HARNESS_ENV = "PINEFORGE_PARITY_HARNESS_DIR"


@dataclass(frozen=True)
class ParityPaths:
    """The three externally selected roots and their source checkout."""

    corpus_root: Path
    derived_workspace: Path
    harness_dir: Path | None
    repo_root: Path = REPO_ROOT


def _resolve(value, environment_name: str, default: Path) -> Path:
    if value is None:
        value = os.environ.get(environment_name)
    if value is None or str(value) == "":
        value = default
    return Path(value).expanduser().resolve()


def _resolve_harness_dir(value) -> Path | None:
    configured = value if value is not None else os.environ.get(HARNESS_ENV)
    if configured is not None and str(configured) != "":
        return Path(configured).expanduser().resolve()
    if (SCRIPT_ROOT / "PARITY-MANIFEST.json").is_file():
        return None
    return (REPO_ROOT / "docker").resolve()


def resolve_paths(*, corpus_root=None, derived_workspace=None,
                  harness_dir=None) -> ParityPaths:
    """Resolve parity roots in explicit, environment, checkout order."""
    return ParityPaths(
        corpus_root=_resolve(corpus_root, CORPUS_ENV, REPO_ROOT / "corpus"),
        derived_workspace=_resolve(
            derived_workspace, DERIVED_ENV, REPO_ROOT / "corpus" / "data" / "derived"),
        harness_dir=_resolve_harness_dir(harness_dir),
    )


__all__ = [
    "CORPUS_ENV",
    "DERIVED_ENV",
    "HARNESS_ENV",
    "ParityPaths",
    "resolve_paths",
]
