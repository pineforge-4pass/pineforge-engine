#!/usr/bin/env python3
"""Refuse duplicate methods, test functions, and class bindings.

Parses the Python test/driver inventory with ``ast``, never importing targets.
Control-flow suites share their enclosing module or class namespace. Exits 1
with ``file:line`` diagnostics for duplicates, absent inventory directories,
or an inspection with no files or no ``test_*`` names; exits 0 otherwise.
"""
from __future__ import annotations

import argparse
import ast
from pathlib import Path
import sys
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parent.parent
TEST_GLOBS = (
    # CMake registers test_*, *_self_test, --self-test drivers and source
    # guards in scripts; tests also has e2e/receipt/equivalence driver names.
    # Scan the whole directories so those naming variants cannot evade this.
    ('scripts', '*.py'),
    ('docker', '*_test.py'),
    ('tests', '*.py'),
    ('tests/ownership', 'test_*.py'),
    ('benchmarks', '*.py'),
    ('benchmarks/tests', 'test_*.py'),
)


def default_test_files(root: Path = ROOT) -> list[Path]:
    if not root.is_dir():
        raise ValueError(f'missing repository root: {root}')
    files: list[Path] = []
    for subdir, pattern in TEST_GLOBS:
        directory = root / subdir
        if not directory.is_dir():
            raise ValueError(f'missing expected test directory: {directory}')
        files.extend(sorted(directory.glob(pattern)))
    return files


def _scope_statements(nodes: Iterable[ast.AST]) -> Iterable[ast.stmt]:
    """Walk all control-flow suites, stopping at namespace boundaries."""
    for node in nodes:
        if isinstance(node, ast.stmt):
            yield node
        if not isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            yield from _scope_statements(ast.iter_child_nodes(node))


def _check_scope(
    path: Path,
    nodes: Iterable[ast.stmt],
    findings: list[tuple[Path, int, str]],
    class_name: str | None = None,
) -> int:
    seen_functions: dict[str, int] = {}
    seen_classes: dict[str, int] = {}
    test_names = 0
    for stmt in _scope_statements(nodes):
        if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef)):
            is_test = stmt.name.startswith('test_')
            test_names += int(is_test)
            if class_name is None and not is_test:
                continue
            kind = f'method {class_name}.{stmt.name}' if class_name else f'top-level test function {stmt.name}'
            if stmt.name in seen_functions:
                findings.append((
                    path,
                    stmt.lineno,
                    f'duplicate {kind} '
                    f'(first defined at line {seen_functions[stmt.name]})',
                ))
            else:
                seen_functions[stmt.name] = stmt.lineno
        elif isinstance(stmt, ast.ClassDef):
            qualified = f'{class_name}.{stmt.name}' if class_name else stmt.name
            test_names += _check_scope(path, stmt.body, findings, qualified)
            # An empty body can inherit tests; refuse the rebinding without
            # trying to execute imports or resolve the class's ancestry.
            if stmt.name in seen_classes:
                findings.append((
                    path,
                    stmt.lineno,
                    f'duplicate test class {qualified} '
                    f'(first defined at line {seen_classes[stmt.name]})',
                ))
            else:
                seen_classes[stmt.name] = stmt.lineno
    return test_names


def _inspect_file(path: Path) -> tuple[list[tuple[Path, int, str]], int]:
    tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
    findings: list[tuple[Path, int, str]] = []
    test_names = _check_scope(path, tree.body, findings)
    return findings, test_names


def check_file(path: Path) -> list[tuple[Path, int, str]]:
    return _inspect_file(path)[0]


def collect_files(root: Path, paths: Sequence[Path]) -> list[Path]:
    if not paths:
        return default_test_files(root)
    collected: list[Path] = []
    for item in paths:
        if item.is_dir():
            if any((item / subdir).exists() for subdir, _ in TEST_GLOBS):
                collected.extend(default_test_files(item))
            else:
                collected.extend(sorted(item.glob('*.py')))
        else:
            collected.append(item)
    return list(dict.fromkeys(collected))


def check_paths(paths: Sequence[Path]) -> list[tuple[Path, int, str]]:
    findings: list[tuple[Path, int, str]] = []
    test_names = 0
    for path in paths:
        file_findings, file_test_names = _inspect_file(path)
        findings.extend(file_findings)
        test_names += file_test_names
    if paths and test_names == 0:
        findings.append((paths[0], 1, 'no test names inspected'))
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
    try:
        files = collect_files(args.root, args.paths)
        if not files:
            print('no test files inspected', file=sys.stderr)
            return 1
        findings = check_paths(files)
    except (OSError, SyntaxError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    if findings:
        for path, lineno, detail in findings:
            print(f'{path}:{lineno}: {detail}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
