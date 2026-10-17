#!/usr/bin/env python3
"""Refuse duplicate method names in test classes and duplicate top-level test functions.

Parses every ``scripts/test_*.py`` and ``docker/*_test.py`` with ``ast`` (never
importing the target modules) and exits 1 naming ``file:line`` for any class
with two methods of the same name or any module with two top-level ``test_*``
functions of the same name; exits 0 otherwise.
"""
from __future__ import annotations

import argparse
import ast
from pathlib import Path
import sys
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parent.parent
TEST_GLOBS = (
    ('scripts', 'test_*.py'),
    ('docker', '*_test.py'),
)


def default_test_files(root: Path = ROOT) -> list[Path]:
    files: list[Path] = []
    for subdir, pattern in TEST_GLOBS:
        directory = root / subdir
        if directory.is_dir():
            files.extend(sorted(directory.glob(pattern)))
    return files


def _iter_classes(nodes: Iterable[ast.stmt]) -> Iterable[ast.ClassDef]:
    for node in nodes:
        if isinstance(node, ast.ClassDef):
            yield node
            yield from _iter_classes(node.body)


def check_file(path: Path) -> list[tuple[Path, int, str]]:
    source = path.read_text(encoding='utf-8')
    tree = ast.parse(source, filename=str(path))
    findings: list[tuple[Path, int, str]] = []

    seen_top: dict[str, int] = {}
    for stmt in tree.body:
        if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef)) and stmt.name.startswith('test_'):
            if stmt.name in seen_top:
                findings.append((
                    path,
                    stmt.lineno,
                    f'duplicate top-level test function {stmt.name} '
                    f'(first defined at line {seen_top[stmt.name]})',
                ))
            else:
                seen_top[stmt.name] = stmt.lineno

    for cls in _iter_classes(tree.body):
        seen_methods: dict[str, int] = {}
        for stmt in cls.body:
            if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if stmt.name in seen_methods:
                    findings.append((
                        path,
                        stmt.lineno,
                        f'duplicate method {cls.name}.{stmt.name} '
                        f'(first defined at line {seen_methods[stmt.name]})',
                    ))
                else:
                    seen_methods[stmt.name] = stmt.lineno

    return findings


def collect_files(root: Path, paths: Sequence[Path]) -> list[Path]:
    if not paths:
        return default_test_files(root)
    collected: list[Path] = []
    for item in paths:
        if item.is_dir():
            nested = default_test_files(item)
            if nested:
                collected.extend(nested)
            else:
                collected.extend(sorted(item.glob('test_*.py')))
                collected.extend(sorted(item.glob('*_test.py')))
        else:
            collected.append(item)
    return collected


def check_paths(paths: Sequence[Path]) -> list[tuple[Path, int, str]]:
    findings: list[tuple[Path, int, str]] = []
    for path in paths:
        findings.extend(check_file(path))
    return findings


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--root',
        type=Path,
        default=ROOT,
        help='Repository root to scan when no explicit paths are given.',
    )
    parser.add_argument(
        'paths',
        nargs='*',
        type=Path,
        help='Optional explicit test files or directories to check.',
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    files = collect_files(args.root, args.paths)
    findings = check_paths(files)
    if findings:
        for path, lineno, detail in findings:
            print(f'{path}:{lineno}: {detail}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
