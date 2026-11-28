#!/usr/bin/env python3
"""Mutation self-tests for scripts/cr150_source_gate.py (source allocation bound).

Every test runs the real gate as a subprocess on a tree it has just built or
mutated, and judges the gate by its exit code and its printed counts. Nothing
here asserts a constant the test wrote for itself: a control that must pass is
shown passing on the unmutated tree first, then the one mutation is applied and
the same command must change its verdict for the intended reason.

Runs under both ``python3 scripts/test_cr150_source_gate.py`` (unittest, no
third-party import) and ``python3 -m pytest scripts/test_cr150_source_gate.py``.
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
GATE = SCRIPTS / 'cr150_source_gate.py'
sys.path.insert(0, str(SCRIPTS))
from cr150_source_gate import Scanner, SITE_INVENTORY_REL, SITE_SCHEMA  # noqa: E402

PIN = '52101f9b7b540fece4493ddc7495451b6d85fc07'
MANIFEST_REL = 'ci/cr150-source-bounds.yaml'
# What the gate scans on the real tree, plus the files that bind it. The notes
# of the manifest also cite lines under runner/, and one of its comments cites
# docs/coverage.md, which the gate reads.
REAL_TREE_DIRS = ('src', 'include', 'tutorial', 'ci', 'runner')
REAL_TREE_FILES = ('scripts/regen_corpus_cpp.sh', 'docs/coverage.md')

LOOP_CPP = '''\
#include <vector>
#include "pineforge/fixed.hpp"
void on_bar(double x) {
    static std::vector<double> slots_;
    if (slots_.size() < kRingSlots) slots_.push_back(1.0);
}
'''
UNCAPPED_RING_CPP = '''\
#include <vector>
#include "pineforge/fixed.hpp"
struct Ring {
    std::vector<double> slots_;
};
void ring_init(Ring& r) { r.slots_.reserve(kRingSlots); }
void on_bar(Ring& r, double x) { r.slots_.push_back(x); }
'''
FIXED_HPP = 'constexpr int kRingSlots = 64;\n'
PIN_SH = f'#!/bin/sh\nCODEGEN_COMMIT="{PIN}"\n'


def q(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def flow(**fields) -> str:
    """One entry in the manifest's flow-map form: ``- {k: v, ...}``."""
    parts = []
    for key, value in fields.items():
        if isinstance(value, bool):
            text = 'true' if value else 'false'
        elif isinstance(value, int):
            text = str(value)
        elif isinstance(value, (list, tuple)):
            text = '[' + ', '.join(q(str(v)) for v in value) + ']'
        else:
            text = q(str(value))
        parts.append(f'{key}: {text}')
    return '  - {' + ', '.join(parts) + '}'


def entry(eid='E1', file='src/loop.cpp', symbol='slots_', ops=('decl', 'grow'),
          state='pending-flat-memory', **extra) -> str:
    fields = dict(id=eid, file=file, symbol=symbol, ops=list(ops),
                  container='std::vector<double>', owner='engine-core',
                  phase='bar', state=state)
    if state == 'pending-flat-memory':
        fields.update(grows_with=['bars'], inventory='R01')
    elif state == 'bounded':
        fields.update(bound='kRingSlots', bound_inputs=['kRingSlots'],
                      lifetime='run', rollback='checkpoint copies the ring',
                      hash_reader='none: not hashed', tests='test_ring')
    elif state == 'unreviewed':
        fields.update(phase='unclassified')
    fields.update(extra)
    return flow(**fields)


def manifest(entries, *, files=('src/**/*.cpp', 'include/pineforge/**/*.hpp'), generated=(), ceilings=(5, 5),
             pin=PIN, fixed=True, templates=True, extra_fixed='', cited_unchecked=None,
             comment_unchecked=None) -> str:
    lines = ['schema: cr150-source-bounds/v1', 'codegen:', f'  pin: {q(pin)}',
             '  pin_source: scripts/regen_corpus_cpp.sh', '  tree: absent',
             '  emitter_templates:']
    lines.append('  ' + flow(id='T1', emits='std::vector<T> arrays', owner='generated',
                           phase='unclassified', state='unreviewed') if templates else '    []')
    lines += ['scan:', '  files:'] + [f'    - {q(g)}' for g in files]
    lines += ['  generated_files:'] + ([f'    - {q(g)}' for g in generated] or ['    []'])
    lines += ['ceilings:', f'  unreviewed: {ceilings[0]}',
              f'  pending-flat-memory: {ceilings[1]}']
    if cited_unchecked is not None:
        # The pinned number of UNCHECKED note citations; a manifest without it allows none.
        lines.append(f'  cited_unchecked: {cited_unchecked}')
    if comment_unchecked is not None:
        # The same pin for the UNCHECKED citations of the manifest's comments.
        lines.append(f'  comment_unchecked: {comment_unchecked}')
    lines += ['owners:', flow(name='engine-core', globs=['src/**', 'include/**']),
              flow(name='generated', globs=['tutorial/**'])]
    lines.append('fixed_inputs:')
    if fixed:
        lines.append(flow(name='kRingSlots', value=64,
                          definition='include/pineforge/fixed.hpp',
                          fixed_before_bar0=True))
    if extra_fixed:
        lines.append(extra_fixed)
    if not fixed and not extra_fixed:
        lines.append('  []')
    lines.append('entries:')
    lines += list(entries) or ['  []']
    return '\n'.join(lines) + '\n'


def write_tree(root: Path, files: dict[str, str]) -> None:
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


def inventory_text(texts: dict[str, str]) -> str:
    """Freeze a control's concrete sites BEFORE any mutation, without a verdict."""
    sites = Scanner(set()).scan(texts)
    return json.dumps({'schema': SITE_SCHEMA, 'sites': [
        dict(file=site.file, symbol=site.symbol, **site.identity()) for site in sites
    ]}, indent=1) + '\n'


def digest(root: Path) -> dict[str, str]:
    out = {}
    for path in sorted(root.rglob('*')):
        if path.is_file():
            out[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def run_gate(root: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, '-B', str(GATE), '--root', str(root), *extra],
                          capture_output=True, text=True)


def counts(proc: subprocess.CompletedProcess) -> dict[str, int]:
    """The gate's own printed ``key=value`` counts, parsed back."""
    found = {}
    for line in proc.stdout.splitlines():
        if line.startswith('cr150_source_gate: '):
            for key, value in re.findall(r'([A-Za-z_()\-]+)=(\d+)', line):
                found[key] = int(value)
    return found


class Tree(unittest.TestCase):
    """A synthetic one-file tree whose manifest is clean unless a test changes it."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def build(self, entries, loop=LOOP_CPP, **kw):
        write_tree(self.root, {
            'src/loop.cpp': loop,
            'include/pineforge/fixed.hpp': FIXED_HPP,
            'scripts/regen_corpus_cpp.sh': PIN_SH,
            MANIFEST_REL: manifest(entries, **kw)})
        write_tree(self.root, {SITE_INVENTORY_REL: inventory_text({
            'src/loop.cpp': loop, 'include/pineforge/fixed.hpp': FIXED_HPP})})

    def gate(self, *extra):
        before = digest(self.root)
        proc = run_gate(self.root, *extra)
        self.assertEqual(before, digest(self.root), 'the gate changed the tree it scans')
        return proc

    def clean_entries(self):
        return [entry('E1', state='bounded')]


class PositiveControls(Tree):
    def test_own_size_guard_exits_0_and_counts_as_pass(self):
        self.build(self.clean_entries())
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c['files_scanned'], c['findings'], c['OPEN']), (2, 0, 0))  # loop.cpp + fixed.hpp
        self.assertGreaterEqual(c['sites_found'], 2)  # declaration + guarded push_back
        self.assertEqual(c['PASS(bounded)'], 1)

    def test_single_push_in_a_braced_guard_is_admitted(self):
        loop = LOOP_CPP.replace(') slots_.push_back(1.0);', ') { slots_.push_back(1.0); }')
        self.build(self.clean_entries(), loop=loop)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['PASS(bounded)'], 1)

    def test_local_declaration_inside_recognized_blocks_is_admitted(self):
        loop = LOOP_CPP.replace('    static std::vector',
                                '    if (x > 0) {\n    {\n    static std::vector')
        loop = loop.replace('\n}', '\n    }\n    }\n}')
        self.build(self.clean_entries(), loop=loop)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['PASS(bounded)'], 1)

    def test_local_declaration_in_a_member_function_is_admitted(self):
        loop = LOOP_CPP.replace('void on_bar', 'struct Callbacks {\nvoid on_bar') + '};\n'
        self.build(self.clean_entries(), loop=loop)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['PASS(bounded)'], 1)

    def test_empty_brace_initializer_is_admitted(self):
        self.build(self.clean_entries(), loop=LOOP_CPP.replace('slots_;', 'slots_{};'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['PASS(bounded)'], 1)

    def test_placeholder_words_are_valid_inventory_symbols(self):
        for symbol in ('rows', 'none', 'unknown', 'todo'):
            with self.subTest(symbol):
                self.build([entry('E1', symbol=symbol, ops=['decl'], state='unreviewed')],
                           loop=f'void f() {{ std::vector<int> {symbol}; }}\n')
                proc = self.gate()
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertEqual(counts(proc)['findings'], 0)
                self.assertEqual(counts(proc)['OPEN'], 1)

    def test_pending_flat_memory_is_reported_open_never_pass(self):
        self.build([entry('E1', state='pending-flat-memory')])
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c['OPEN'], c['PASS(bounded)'], c['findings']), (1, 0, 0))
        self.assertIn('OPEN', proc.stdout)
        self.assertNotIn('result: PASS', proc.stdout)
        strict = self.gate('--strict')
        self.assertEqual(strict.returncode, 1, strict.stdout)
        self.assertIn('OPEN', strict.stdout)

    def test_comments_and_string_literals_are_not_sites(self):
        loop = LOOP_CPP + '// v.push_back(1); new int[3]; malloc(4)\n' \
                          'const char* s = "x.push_back(1) new int";\n'
        self.build(self.clean_entries(), loop=loop)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


class UnlistedSite(Tree):
    def test_u0549_spelling_in_another_function_is_a_new_site(self):
        control = '''\
void set_rates(int n) {
    std::vector<double> next_rates;
    next_rates.reserve(n);
}
'''
        self.build([entry('U0549', symbol='next_rates', state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['OPEN'], 1)
        extra = '''\
void cr150_extra(int n) {
    std::vector<int> next_rates;
    next_rates.resize(n);
}
'''
        (self.root / 'src/loop.cpp').write_text(control + extra)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site on \'next_rates\'.*entry U0549')
        self.assertIn('cr150_extra', proc.stdout)

    def test_same_scope_duplicate_declaration_preserves_multiplicity(self):
        control = 'void f() { std::vector<int> next_rates; }\n'
        self.build([entry('E1', symbol='next_rates', ops=['decl'], state='unreviewed')],
                   loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        # Deliberately ill-formed C++: this indexes source, so even an identical
        # second declaration in one lexical scope must not inherit the first.
        (self.root / 'src/loop.cpp').write_text(
            control.replace('next_rates;', 'next_rates; std::vector<int> next_rates;'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site.*entry E1.*ordinal=2')

    def test_identical_type_and_operations_in_a_new_scope_are_new_sites(self):
        control = 'void first(int n) { std::vector<int> rows; rows.resize(n); }\n'
        self.build([entry('E1', symbol='rows', state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        (self.root / 'src/loop.cpp').write_text(control + control.replace('first', 'second'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site.*entry E1.*second')
        self.assertRegex(proc.stdout, r'NEW grow site.*entry E1.*second')

    def test_a_scope_change_is_not_hidden_by_unchanged_site_counts(self):
        control = 'void first() { std::vector<int> rows; }\n'
        self.build([entry('E1', symbol='rows', ops=['decl'], state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        (self.root / 'src/loop.cpp').write_text(control.replace('first', 'second'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site.*entry E1.*second')
        self.assertIn('reviewed site identity', proc.stdout)
        self.assertIn('stale', proc.stdout)

    def test_identical_sibling_blocks_have_distinct_site_identities(self):
        statement = '{ std::vector<int> rows; rows.resize(n); }'
        control = f'void f(int n) {{\n    {statement}\n}}\n'
        self.build([entry('E1', symbol='rows', state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        (self.root / 'src/loop.cpp').write_text(
            control.replace('\n}\n', f'\n    {statement}\n}}\n'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site.*entry E1.*<block> #2')
        self.assertRegex(proc.stdout, r'NEW grow site.*entry E1.*<block> #2')

    def test_a_type_change_is_not_hidden_by_unchanged_scope_and_counts(self):
        control = 'void f() { std::vector<int> rows; }\n'
        self.build([entry('E1', symbol='rows', ops=['decl'], state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        (self.root / 'src/loop.cpp').write_text(control.replace('vector<int>', 'vector<double>'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site.*entry E1')
        self.assertIn('reviewed site identity', proc.stdout)
        self.assertIn('stale', proc.stdout)

    def test_second_identical_growth_statement_is_a_new_site(self):
        control = 'void f() { std::vector<int> rows; rows.push_back(1); }\n'
        self.build([entry('E1', symbol='rows', state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        (self.root / 'src/loop.cpp').write_text(
            control.replace('rows.push_back(1);', 'rows.push_back(1); rows.push_back(1);'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW grow site.*entry E1.*ordinal=2')

    def test_moving_lines_and_whitespace_keeps_the_site_identity(self):
        self.build(self.clean_entries())
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        moved = '\n// added diagnostic lines\n\n' + LOOP_CPP.replace(
            'std::vector<double>', 'std::vector< double >')
        (self.root / 'src/loop.cpp').write_text(moved)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['PASS(bounded)'], 1)

    def test_authoring_inventory_does_not_change_the_verification_baseline(self):
        control = 'void first() { std::vector<int> rows; }\n'
        self.build([entry('E1', symbol='rows', ops=['decl'], state='unreviewed')], loop=control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        frozen = json.loads((self.root / SITE_INVENTORY_REL).read_text())
        (self.root / 'src/loop.cpp').write_text(control + control.replace('first', 'second'))
        authored = self.gate('--emit-site-inventory')
        self.assertEqual(authored.returncode, 0, authored.stdout + authored.stderr)
        proposal = json.loads(authored.stdout)
        self.assertGreater(len(proposal['sites']), len(frozen['sites']))
        self.assertNotIn('result:', authored.stdout)
        self.assertEqual(json.loads((self.root / SITE_INVENTORY_REL).read_text()), frozen)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'NEW decl site.*entry E1.*second')

    def test_push_back_without_entry_exits_1_and_names_the_line(self):
        self.build(self.clean_entries())
        self.assertEqual(self.gate().returncode, 0)  # positive control, same tree
        mutated = LOOP_CPP + 'void leak(std::vector<int>& rows) { rows.push_back(1); }\n'
        (self.root / 'src/loop.cpp').write_text(mutated)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        line = mutated.count('\n')
        self.assertIn(f'src/loop.cpp:{line}', proc.stdout)
        self.assertIn('no manifest entry', proc.stdout)
        self.assertEqual(counts(proc)['findings'], 1)

    def test_every_growth_class_is_caught_when_unlisted(self):
        injected = {
            'push_back': 'void f(std::vector<int>& q) { q.push_back(1); }',
            'emplace_back': 'void f(std::vector<int>& q) { q.emplace_back(1); }',
            'insert': 'void f(std::vector<int>& q) { q.insert(q.begin(), 1); }',
            'resize': 'void f(std::vector<int>& q) { q.resize(8); }',
            'reserve': 'void f(std::vector<int>& q) { q.reserve(8); }',
            'assign': 'void f(std::vector<int>& q) { q.assign(8, 0); }',
            'append': 'void f(std::string& q) { q.append("x"); }',
            'new[]': 'double* f() { double* buf = new double[8]; return buf; }',
            'malloc': 'void* f() { void* buf = malloc(8); return buf; }',
            'calloc': 'void* f() { void* buf = calloc(8, 1); return buf; }',
            'realloc': 'void* f(void* p) { void* buf = realloc(p, 8); return buf; }',
            'make_unique': 'auto f() { auto p = std::make_unique<int>(1); return p; }',
            'make_shared': 'auto f() { auto p = std::make_shared<int>(1); return p; }',
            'by-value vector': 'void f() { std::vector<int> local_rows; (void)local_rows; }',
            'by-value string': 'void f() { std::string local_text; (void)local_text; }',
            'ostringstream': 'void f() { std::ostringstream local_out; (void)local_out; }',
            'to_string': 'void f(int n) { auto s = std::to_string(n); (void)s; }',
            'map subscript': 'void f() { std::map<int,int> by_key_; by_key_[3] = 4; }',
            'container assign': 'void f(Ring& r, std::vector<double> o) { r.slots_ = o; }',
            'string +=': 'void f() { std::string acc; acc += "x"; }',
        }
        self.build(self.clean_entries())
        self.assertEqual(self.gate().returncode, 0)
        for name, snippet in injected.items():
            with self.subTest(name):
                (self.root / 'src/loop.cpp').write_text(LOOP_CPP + snippet + '\n')
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, f'{name}: {proc.stdout}{proc.stderr}')
                self.assertIn('no manifest entry', proc.stdout, name)

    def test_new_op_class_on_a_listed_container_exits_1(self):
        self.build(self.clean_entries())
        (self.root / 'src/loop.cpp').write_text(
            LOOP_CPP + 'void g(Ring& r, std::vector<double> o) { r.slots_.insert(r.slots_.end(), o.begin(), o.end()); }\n'
                       'void h(Ring& r, std::vector<double> o) { r.slots_ = o; }\n')
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("op class 'assign'", proc.stdout)

    def test_subscript_append_and_assign_classes_are_each_detected(self):
        base = LOOP_CPP + 'struct Aux { std::map<int, int> by_key_; std::string acc_; };\n'
        entries = self.clean_entries() + [
            entry('E2', symbol='by_key_', ops=['decl'], container='std::map<int,int>'),
            entry('E3', symbol='acc_', ops=['decl'], container='std::string')]
        self.build(entries, loop=base)
        self.assertEqual(self.gate().returncode, 0, 'positive control')
        for cls, stmt in (('subscript', 'void f(Aux& a) { a.by_key_[3] = 4; }'),
                          ('append', 'void f(Aux& a) { a.acc_ += "x"; }'),
                          ('assign', 'void f(Aux& a, Aux& b) { a.acc_ = b.acc_; }')):
            with self.subTest(cls):
                (self.root / 'src/loop.cpp').write_text(base + stmt + '\n')
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, f'{cls}: {proc.stdout}{proc.stderr}')
                self.assertIn(f"op class '{cls}'", proc.stdout)

    def test_stale_entry_exits_1(self):
        self.build(self.clean_entries() + [entry('E2', symbol='gone_', ops=['grow'])])
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn('matches no site', proc.stdout)

    def test_bounded_entry_covering_a_computed_reserve_exits_1(self):
        loop = LOOP_CPP + 'void grow(Ring& r, int n_bars) { r.slots_.reserve(n_bars); }\n'
        self.build(self.clean_entries(), loop=loop)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn('computed', proc.stdout)

    def test_unreviewed_ceiling_exceeded_exits_1(self):
        self.build([entry('E1', state='unreviewed')], ceilings=(0, 5))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn('ceiling', proc.stdout)

    def test_codegen_pin_drift_exits_1(self):
        self.build(self.clean_entries(), pin='0' * 40)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn('codegen pin', proc.stdout)


class BadBounds(Tree):
    def test_bound_naming_a_growth_axis_is_a_finding_with_the_entry_id(self):
        for expr in ('bars', 'n_bars', 'trades', 'trade_count', 'entries', 'decisions',
                     'history_len', 'bar_index + 1', 'kRingSlots + bars'):
            with self.subTest(expr):
                self.build([entry('E1', state='bounded', bound=expr,
                                  bound_inputs=['kRingSlots'])])
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, f'{expr}: {proc.stdout}{proc.stderr}')
                self.assertRegex(proc.stdout, r'FINDING .*entry E1:.*bound .*forbidden growth axis')
                self.assertEqual(counts(proc)['PASS(bounded)'], 0)

    def test_tautological_bound_is_a_manifest_error(self):
        for expr in ('slots_', 'slots_.size()'):
            with self.subTest(expr):
                self.build([entry('E1', state='bounded', bound=expr)])
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn('E1', proc.stdout)
                self.assertIn('tautology', proc.stdout)

    def test_bound_input_outside_the_fixed_input_list_exits_2(self):
        self.build([entry('E1', state='bounded', bound='kOther', bound_inputs=['kOther'])])
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('fixed input', proc.stdout + proc.stderr)
        self.assertIn('entry E1', proc.stdout)

    def test_fixed_input_that_names_a_growth_axis_exits_2(self):
        extra = flow(name='kMaxBars', value=9, definition='include/pineforge/fixed.hpp',
                     fixed_before_bar0=True)
        self.build(self.clean_entries(), extra_fixed=extra)
        (self.root / 'include/pineforge/fixed.hpp').write_text(FIXED_HPP + 'constexpr int kMaxBars = 9;\n')
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)

    def test_fixed_input_value_must_match_its_definition(self):
        self.build(self.clean_entries())
        (self.root / 'include/pineforge/fixed.hpp').write_text('constexpr int kRingSlots = 65;\n')
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)

    def test_fixed_input_requires_an_uncommented_nonlocal_immutable_definition(self):
        bad = (
            'int kRingSlots = 64;\n',
            '// constexpr int kRingSlots = 64;\nint kRingSlots = 65;\n',
            '/* constexpr int kRingSlots = 64; */\n',
            'void f() { constexpr int kRingSlots = 64; }\n',
            'const char* text = "constexpr int kRingSlots = 64;";\n',
            'struct Limits { const int kRingSlots = 64; };\n',
            'struct Limits { const int kRingSlots = 64; '
            'Limits(int n) : kRingSlots(n) {} };\n',
        )
        for definition in bad:
            with self.subTest(definition):
                self.build(self.clean_entries())
                control = self.gate()
                self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
                (self.root / 'include/pineforge/fixed.hpp').write_text(definition)
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn('namespace/class-scope const/constexpr', proc.stdout)

    def test_fixed_input_value_is_required(self):
        self.build(self.clean_entries())
        path = self.root / MANIFEST_REL
        path.write_text(path.read_text().replace('value: 64, ', ''))
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('fixed input kRingSlots: missing required field(s) value', proc.stdout)

    def test_const_namespace_and_constexpr_class_definitions_are_supported(self):
        for definition in (
                'namespace limits { const int kRingSlots = 64; }\n',
                'struct Limits { static constexpr int kRingSlots = 64; };\n',
                'struct Limits { inline static const int kRingSlots = 64; };\n'):
            with self.subTest(definition):
                # OPEN rows exercise provenance without making an unsupported
                # qualified-name bounded claim.
                self.build([entry('E1', state='unreviewed')])
                (self.root / 'include/pineforge/fixed.hpp').write_text(definition)
                proc = self.gate()
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertEqual(counts(proc)['PASS(bounded)'], 0)

    def test_bounded_entry_missing_a_required_field_exits_2(self):
        for field in ('bound', 'lifetime', 'rollback', 'hash_reader', 'tests', 'owner'):
            with self.subTest(field):
                line = entry('E1', state='bounded')
                line = re.sub(rf'{field}: "[^"]*"(, )?', '', line)
                line = line.replace(', }', '}')
                self.build([line])
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, f'{field}: {proc.stdout}{proc.stderr}')

    def test_placeholder_text_in_a_bounded_entry_exits_2(self):
        for text in ('TODO', 'tbd', '?', ''):
            with self.subTest(text):
                self.build([entry('E1', state='bounded', tests=text)])
                self.assertEqual(self.gate().returncode, 2)

    def test_pending_entry_may_not_carry_a_bound(self):
        self.build([entry('E1', state='pending-flat-memory', bound='kRingSlots')])
        self.assertEqual(self.gate().returncode, 2)

    def test_duplicate_entry_key_exits_2(self):
        self.build([entry('E1', state='bounded'), entry('E2', state='bounded')])
        self.assertEqual(self.gate().returncode, 2)


class GuardedBounds(Tree):
    def rejected(self, loop: str, diagnostic: str, entries=None):
        self.build(self.clean_entries())
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertEqual(counts(control)['PASS(bounded)'], 1)
        # Record the negative shape's actual sites as well. Its rejection must
        # come from enforcement, independently of the NEW-site check.
        self.build(entries or self.clean_entries(), loop=loop)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, rf'FINDING .*bounded entry E1:.*(?:{diagnostic})')
        self.assertEqual(counts(proc)['PASS(bounded)'], 0)
        self.assertNotIn('NEW ', proc.stdout)
        return proc

    def proof_file_rejected(self, rel: str, control: str, hostile: str, diagnostic: str):
        self.build(self.clean_entries(), loop=control if rel == 'src/loop.cpp' else LOOP_CPP)
        path = self.root / rel
        path.write_text(control)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc)['PASS(bounded)'], 1)
        # Keep the control's inventory: these raw-text mutations add no lexed
        # sites. Neither a rebaseline nor a NEW/stale finding can explain refusal.
        path.write_text(hostile)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f'bounded entry E1: {rel}: {diagnostic}', proc.stdout)
        self.assertEqual(counts(proc)['PASS(bounded)'], 0)
        self.assertEqual(counts(proc)['findings'], 1)
        self.assertNotIn('NEW ', proc.stdout)
        self.assertNotIn('stale', proc.stdout)

    def test_original_ring_reserve_and_uncapped_push_is_not_bounded(self):
        self.rejected(UNCAPPED_RING_CPP, 'initially empty local')
        self.rejected(UNCAPPED_RING_CPP.replace('reserve(kRingSlots)', 'reserve(65)'),
                      'initially empty local')

    def test_decorated_class_members_are_not_local(self):
        members = '''\
    std::vector<double> slots_;
    void on_bar(double x) {
        if (slots_.size() < kRingSlots) slots_.push_back(1.0);
    }
'''
        for header, close in (
                ('template <typename T> struct X', '};'),
                ('class PF_API X', '};'),
                ('struct alignas(64) X', '};'),
                ('struct Outer { public: struct X', '}; };')):
            with self.subTest(header):
                loop = '#include <vector>\n#include "pineforge/fixed.hpp"\n' \
                       + header + ' {\n' + members + close + '\n'
                self.rejected(loop, 'initially empty local')

    def test_local_class_members_do_not_inherit_the_outer_function_scope(self):
        loop = LOOP_CPP.replace('void on_bar(double x) {\n    static std::vector<double> slots_;',
                                'void f() { struct X {\n    std::vector<double> slots_;\n'
                                '    void on_bar(double x) {') + '}; }\n'
        self.rejected(loop, 'initially empty local')

    def test_unknown_scopes_do_not_inherit_the_outer_function_scope(self):
        for header in ('auto invoke = []', 'if (x > 0) auto invoke = []()'):
            with self.subTest(header):
                loop = LOOP_CPP.replace('    static std::vector',
                                        f'    {header} {{\n    static std::vector')
                loop = loop.replace('\n}', '\n    };\n}')
                self.rejected(loop, 'initially empty local')

    def test_extern_linkage_block_is_not_a_local_scope(self):
        loop = LOOP_CPP.replace('void on_bar(double x) {\n    static std::vector<double> slots_;',
                                'extern "C" {\nstd::vector<double> slots_;\n'
                                'void on_bar(double x) {') + '}\n'
        self.rejected(loop, 'initially empty local')

    def test_block_scope_extern_is_not_an_empty_local_definition(self):
        self.rejected(LOOP_CPP.replace('static std::vector', 'extern std::vector'),
                      'initially empty local')

    def test_nonempty_local_initializer_is_not_bounded(self):
        self.rejected(LOOP_CPP.replace('slots_;', 'slots_(100);'), 'initially empty local')

    def test_uncapped_repeated_push_is_not_bounded(self):
        self.rejected(LOOP_CPP.replace('if (slots_.size() < kRingSlots) ', ''),
                      'no direct own-size guard')

    def test_reserve_only_is_not_a_maximum_even_with_a_fixed_argument(self):
        for argument in ('kRingSlots', '65'):
            with self.subTest(argument):
                loop = LOOP_CPP.replace('if (slots_.size() < kRingSlots) slots_.push_back(1.0);',
                                        f'slots_.reserve({argument});')
                self.rejected(loop, 'reserve/resize/assign is not an enforced maximum')

    def test_literal_guard_does_not_match_a_named_bound(self):
        self.rejected(LOOP_CPP.replace('< kRingSlots', '< 65'),
                      "guard bound '65' does not match declared bound 'kRingSlots'")

    def test_guard_identifier_must_be_the_declared_fixed_input(self):
        loop = LOOP_CPP.replace('void on_bar', 'constexpr int kOther = 64;\nvoid on_bar')
        self.rejected(loop.replace('< kRingSlots', '< kOther'),
                      "guard bound 'kOther' does not match declared bound 'kRingSlots'")

    def test_guard_on_a_different_container_is_not_an_enforced_bound(self):
        loop = LOOP_CPP.replace('static std::vector<double> slots_;',
                                'static std::vector<double> slots_;\n'
                                '    static std::vector<double> other;')
        loop = loop.replace('if (slots_.size()', 'if (other.size()')
        self.rejected(loop, "this container's own size", entries=self.clean_entries() + [
            entry('E2', symbol='other', ops=['decl'], state='unreviewed')])

    def test_guard_cannot_cover_a_loop_or_two_pushes(self):
        for body in ('{ slots_.push_back(1.0); slots_.push_back(1.0); }',
                     'for (int i = 0; i < 100; ++i) slots_.push_back(1.0);'):
            with self.subTest(body):
                loop = LOOP_CPP.replace('slots_.push_back(1.0);', body)
                self.rejected(loop, 'guard block|no direct own-size guard')

    def test_a_guard_does_not_admit_other_growth_or_aliases(self):
        for statement in ('slots_.resize(kRingSlots);', 'slots_.assign(kRingSlots, 1);',
                          'slots_.reserve(kRingSlots);', 'slots_.push_back(1.0);',
                          'auto& alias = slots_; alias.push_back(1.0);'):
            with self.subTest(statement):
                loop = LOOP_CPP.replace('\n}', f'\n    {statement}\n}}')
                entries = self.clean_entries()
                if 'alias' in statement:
                    entries += [entry('E2', symbol='alias', ops=['grow'], state='unreviewed')]
                self.rejected(loop, 'unsupported|no direct own-size guard', entries=entries)

    def test_container_uses_in_other_scanned_files_are_not_admitted(self):
        self.build(self.clean_entries())
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertEqual(counts(control)['PASS(bounded)'], 1)
        write_tree(self.root, {'src/other.cpp': 'void f() { consume(slots_); }\n'})
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'FINDING src/other.cpp:1: bounded entry E1: '
                         r'container has an unsupported use or shadow')
        self.assertEqual(counts(proc)['PASS(bounded)'], 0)
        self.assertNotIn('NEW ', proc.stdout)
        self.assertNotIn('no manifest entry', proc.stdout)

    def test_shadowing_the_fixed_input_is_not_admitted(self):
        loop = LOOP_CPP.replace('static std::vector<double> slots_;',
                                'int kRingSlots = 100;\n    static std::vector<double> slots_;')
        self.rejected(loop, 'unsupported use or shadow declaration')

    def test_push_arguments_cannot_hide_calls_or_conversions(self):
        for argument in ('x', 'convert()', '(on_bar(x), 1.0)'):
            with self.subTest(argument):
                self.rejected(LOOP_CPP.replace('push_back(1.0)', f'push_back({argument})'),
                              'numeric literal push argument')

    def test_macro_expansion_is_not_treated_as_a_guard_proof(self):
        for directive in ('#define push_back(x) insert(end(), 65, x)',
                          '%:define push_back(x) resize(1u << 30)'):
            with self.subTest(directive):
                self.rejected(directive + '\n' + LOOP_CPP, 'macro/conditional preprocessing')

    def test_spliced_digraph_in_entry_is_not_a_guard_proof(self):
        # 28eac95877 sees neither physical line as a directive or a new site,
        # and admits the unchanged literal guard. The continuation must refuse it.
        loop = LOOP_CPP.replace('void on_bar(double x) {\n',
                                'void on_bar(double x) {\n'
                                '%\\\n:define push_back(x) resize(1u << 30)\n')
        self.rejected(loop, 'line continuation')

    def test_brace_digraphs_are_not_scope_proof(self):
        # 28eac95877 leaves the %>-closed opener on its stack, making the global
        # declaration "local". It also ignores the <% inner block in the second
        # case. Both pass there; both must refuse without changing site spelling.
        opener = LOOP_CPP.replace(
            'void on_bar(double x) {\n    static std::vector<double> slots_;',
            'void opener() { %>\nstd::vector<double> slots_;\nvoid on_bar(double x) {')
        inner = LOOP_CPP.replace('void on_bar(double x) {',
                                 'void on_bar(double x) { <%') + '}\n'
        for shape, loop in (('global-after-opener', opener), ('inner-block', inner)):
            with self.subTest(shape):
                self.rejected(loop, 'brace digraphs')

    def test_fixed_input_continuations_and_brace_digraphs_are_refused(self):
        self.build(self.clean_entries())
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertEqual(counts(control)['PASS(bounded)'], 1)
        # Each suffix is site-free and leaves the fixed declaration intact.
        # 28eac95877 therefore passes all three; the proof-file checks must refuse.
        for suffix, diagnostic in (('\\\n\n', 'line continuation'),
                                   ('void helper() <% }\n', 'brace digraphs'),
                                   ('void helper() { %>\n', 'brace digraphs')):
            with self.subTest(suffix):
                (self.root / 'include/pineforge/fixed.hpp').write_text(FIXED_HPP + suffix)
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertRegex(proc.stdout, r'FINDING .*bounded entry E1: '
                                 rf'include/pineforge/fixed.hpp: {diagnostic}')
                self.assertEqual(counts(proc)['PASS(bounded)'], 0)
                self.assertNotIn('NEW ', proc.stdout)
                self.assertNotIn('stale', proc.stdout)

    def test_splices_in_comments_and_literals_cannot_hide_an_unguarded_push(self):
        for rel in ('src/loop.cpp', 'include/pineforge/fixed.hpp'):
            action = 'slots_.push_back(1.0);' if rel == 'src/loop.cpp' else 'int ignored = 0;'
            for context, control, hostile in (
                    ('block comment', f'/* {action} */',
                     '/* *\\\n/ ' + action + ' /* */'),
                    ('string', '(void)""; (void)"";',
                     '(void)"\\\n"; ' + action + ' (void)"";'),
                    ('char', "(void)'n'; (void)'n';",
                     "(void)'\\\nn'; " + action + " (void)'n';")):
                with self.subTest(file=rel, context=context):
                    if rel == 'src/loop.cpp':
                        benign = LOOP_CPP.replace('\n}', f'\n    {control}\n}}')
                        mutation = LOOP_CPP.replace('\n}', f'\n    {hostile}\n}}')
                    else:
                        benign = FIXED_HPP + f'inline void cr150_note() {{\n    {control}\n}}\n'
                        mutation = FIXED_HPP + f'inline void cr150_note() {{\n    {hostile}\n}}\n'
                    # On 7331cbae29 cxxlex erases the splice and hidden action;
                    # the lexed continuation check and guarded proof both pass.
                    self.proof_file_rejected(rel, benign, mutation, 'line continuation')

    def test_whitespace_splices_in_line_comments_are_refused_in_both_proof_files(self):
        for rel in ('src/loop.cpp', 'include/pineforge/fixed.hpp'):
            for whitespace in (' ', '\t', '\f', '\v'):
                with self.subTest(file=rel, whitespace=repr(whitespace)):
                    if rel == 'src/loop.cpp':
                        benign = LOOP_CPP.replace('    if (', '    // cap\n    if (')
                        mutation = benign.replace('// cap\n', '// cap \\' + whitespace + '\n')
                    else:
                        benign = FIXED_HPP + '// cap\ninline void cr150_note() {}\n'
                        mutation = benign.replace('// cap\n', '// cap \\' + whitespace + '\n')
                    # The parent drops the comment and sees the guard. GCC and
                    # Clang splice its whitespace-terminated backslash instead.
                    self.proof_file_rejected(rel, benign, mutation, 'line continuation')

    def test_raw_string_splices_cannot_hide_defines_in_either_proof_file(self):
        for rel in ('src/loop.cpp', 'include/pineforge/fixed.hpp'):
            for prefix in ('R', 'u8R', 'LR', 'uR', 'UR'):
                with self.subTest(file=rel, prefix=prefix):
                    control = f'const auto* cr150_note = {prefix[:-1]}"a";\n'
                    hostile = (f'const auto* cr150_note = {prefix}"x(a)x\\\n'
                               '" /* )x";\n#define size() size() * 0\n// */\n')
                    if rel == 'src/loop.cpp':
                        benign = LOOP_CPP.replace('void on_bar', control + 'void on_bar')
                        mutation = LOOP_CPP.replace('void on_bar', hostile + 'void on_bar')
                    else:
                        benign, mutation = FIXED_HPP + control, FIXED_HPP + hostile
                    # The parent's joined-then-lexed view ends the raw literal
                    # early and hides the define inside the resulting comment.
                    self.proof_file_rejected(rel, benign, mutation, 'raw string literals')

    def test_proof_file_refusals_use_raw_text_even_inside_comments(self):
        cases = (
            ('// ordinary string note\n', '// R" note\n', 'raw string literals'),
            ('// ordinary brace note\n', '// <% note\n', 'brace digraphs'),
            ('// ordinary directive note\n', '// #define size() 0\n',
             'macro/conditional preprocessing'),
            ('// ASCII note\n', '// note \x00\n', 'NUL'),
            ('// ASCII note\n', '// note \ufeff\n', 'BOM'),
            ('// cafe\n', '// caf\u00e9\n', 'non-ASCII text'),
        )
        for rel in ('src/loop.cpp', 'include/pineforge/fixed.hpp'):
            for control, hostile, diagnostic in cases:
                with self.subTest(file=rel, diagnostic=diagnostic):
                    base = LOOP_CPP if rel == 'src/loop.cpp' else FIXED_HPP
                    # All these comments disappear in the parent's lexer.
                    self.proof_file_rejected(rel, base + control, base + hostile, diagnostic)

    def test_nul_before_a_define_in_the_entry_cannot_bypass_the_allowlist(self):
        control = LOOP_CPP.replace('    if (', '// size is a member function\n    if (')
        hostile = LOOP_CPP.replace('    if (', '\x00#define size() size() * 0\n    if (')
        # Inside the already-open function, the NUL adds no scope or site;
        # the parent's directive regex ignores the line and admits its guard.
        self.proof_file_rejected('src/loop.cpp', control, hostile, 'NUL')

    def test_bom_before_an_include_in_the_entry_cannot_bypass_the_allowlist(self):
        write_tree(self.root, {'tests/cr150_evil.hpp':
                              '#include <vector>\n#define size() size() * 0\n'})
        control = '#include <cstddef>\n;\n' + LOOP_CPP
        hostile = '\ufeff#include "../tests/cr150_evil.hpp"\n;\n' + LOOP_CPP
        # The empty declaration keeps the same function scope on the parent;
        # its directive regex skips BOM and never reads the unscanned header.
        self.proof_file_rejected('src/loop.cpp', control, hostile, 'BOM')

    def other_scanned_control(self, consumer='#include <vector>\n#define unrelated(x) x\n'):
        self.build(self.clean_entries())
        write_tree(self.root, {'src/other.cpp': consumer})
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertEqual(counts(control)['PASS(bounded)'], 1)
        return consumer

    def other_scanned_rejected(self, control: str, hostile: str, name: str):
        self.other_scanned_control(control)
        (self.root / 'src/other.cpp').write_text(hostile)
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f"bounded entry E1: scanned file src/other.cpp defines macro {name!r}",
                      proc.stdout)
        self.assertEqual(counts(proc)['PASS(bounded)'], 0)
        self.assertEqual(counts(proc)['findings'], 1)
        self.assertNotIn('header entries', proc.stdout)
        self.assertNotIn('NEW ', proc.stdout)
        self.assertNotIn('stale', proc.stdout)

    def test_raw_string_splices_cannot_hide_defines_in_other_scanned_files(self):
        for prefix in ('R', 'u8R', 'LR', 'uR', 'UR'):
            with self.subTest(prefix=prefix):
                note = (f'const auto* cr150_note = {prefix}"x(a)x\\\n'
                        '" /* )x";\n#define unrelated() unrelated() * 0\n// */\n')
                # The same raw literal and splice are permitted outside the
                # proof files until the hidden define names a proof identifier.
                self.other_scanned_rejected(note, note.replace('unrelated', 'size'), 'size')

    def test_obscured_defines_in_other_scanned_files_are_refused_without_lexing(self):
        directives = [
            '#/*\n*/define unrelated() unrelated() * 0',
            '\x00#define unrelated() unrelated() * 0',
            '\ufeff#define unrelated() unrelated() * 0',
            '#define/*\n*/unrelated() unrelated() * 0',
        ]
        for whitespace in ('\f', '\v', '\x00', ' \t\f\v\x00'):
            directives.extend((
                '#define \\' + whitespace + '\nunrelated() unrelated() * 0',
                '#de\\' + whitespace + '\nfine unrelated() unrelated() * 0'))
        for directive in directives:
            with self.subTest(directive=repr(directive)):
                # Each spelling is invisible to the parent's directive/name
                # parser; changing just the macro name triggers the raw rule.
                self.other_scanned_rejected(directive + '\n',
                                            directive.replace('unrelated', 'size') + '\n', 'size')

    def test_macros_in_other_scanned_files_cannot_rewrite_a_cpp_entry(self):
        consumer = self.other_scanned_control()
        for marker in ('#', '%:'):
            for definition in ('push_back(x) resize(1u << 30)',
                               'emplace_back(x) resize(1u << 30)', 'size() 0',
                               'slots_ replaced_slots', 'kRingSlots 100'):
                with self.subTest(marker=marker, definition=definition):
                    (self.root / 'src/other.cpp').write_text(
                        f'{marker}define {definition}\n' + consumer)
                    proc = self.gate()
                    self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                    self.assertRegex(proc.stdout, r'FINDING .*bounded entry E1: '
                                     r'scanned file src/other.cpp defines macro')
                    self.assertEqual(counts(proc)['PASS(bounded)'], 0)
                    self.assertNotIn('header entries', proc.stdout)
                    self.assertNotIn('NEW ', proc.stdout)

    def test_commented_and_spliced_defines_cannot_rewrite_a_cpp_entry(self):
        consumer = self.other_scanned_control()
        include = '#include <vector>'
        definitions = ['#define/**/push_back(x) resize(1u << 30)']
        for splice in ('\\\n', '\\ \t\r\n'):
            definitions.extend((
                f'#define {splice}push_back(x) resize(1u << 30)',
                f'#{splice}define push_back(x) resize(1u << 30)',
                f'%{splice}:define push_back(x) resize(1u << 30)'))
        # 28eac95877 drops /**/ without a space, and never joins any splice.
        # Its name regex therefore misses every definition, producing PASS=1.
        # The auxiliary source is outside the proof files: only define refusal can
        # close these cases, not the entry/fixed-file continuation refusal.
        for directive in definitions:
            with self.subTest(directive):
                (self.root / 'src/other.cpp').write_text(
                    consumer.replace(include, directive + '\n' + include))
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertRegex(proc.stdout, r'FINDING .*bounded entry E1: '
                                 r"scanned file src/other.cpp defines macro 'push_back'")
                self.assertEqual(counts(proc)['PASS(bounded)'], 0)
                self.assertNotIn('header entries', proc.stdout)
                self.assertNotIn('NEW ', proc.stdout)
                self.assertNotIn('stale', proc.stdout)

    def test_keyword_and_type_macros_cannot_rewrite_a_cpp_entry(self):
        consumer = self.other_scanned_control()
        include = '#include <vector>'
        # None of these names is in 28eac95877's five-name refusal list. Their
        # definitions add no sites or receiver/bound uses, so it passes each.
        # constexpr/int occur only in the fixed input, exercising both files.
        for name, definition in (('if', 'if(c) if (true)'),
                                 ('static', 'static extern'),
                                 ('std', 'std alternate_std'),
                                 ('vector', 'vector alternate_vector'),
                                 ('double', 'double ScalarWithSideEffects'),
                                 ('constexpr', 'constexpr const'),
                                 ('int', 'int long'),
                                 ('x', 'x ignored_argument')):
            with self.subTest(name):
                directive = '#define ' + definition
                (self.root / 'src/other.cpp').write_text(
                    consumer.replace(include, directive + '\n' + include))
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertRegex(proc.stdout, r'FINDING .*bounded entry E1: '
                                 rf'scanned file src/other.cpp defines macro {name!r}')
                self.assertEqual(counts(proc)['PASS(bounded)'], 0)
                self.assertNotIn('header entries', proc.stdout)
                self.assertNotIn('NEW ', proc.stdout)
                self.assertNotIn('stale', proc.stdout)

    def test_header_entries_are_refused_even_with_a_guarded_local_vector(self):
        header_file = 'include/pineforge/loop.hpp'
        header = '#pragma once\n' + LOOP_CPP
        self.build(self.clean_entries(), loop=header)
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertEqual(counts(control)['PASS(bounded)'], 1)
        # The identical guarded function moves to a public header. The parent
        # passes even when an unscanned header defines size() before inclusion.
        for prelude in ('', '#include "../tests/cr150_evil.hpp"\n'):
            with self.subTest(prelude=prelude):
                consumer = '#include <vector>\n' + prelude + '#include "pineforge/loop.hpp"\n'
                self.build([entry('E1', file=header_file, state='bounded')], loop=consumer)
                write_tree(self.root, {
                    header_file: header,
                    'tests/cr150_evil.hpp': '#define size() size() * 0\n',
                    SITE_INVENTORY_REL: inventory_text({
                        header_file: header, 'src/loop.cpp': consumer,
                        'include/pineforge/fixed.hpp': FIXED_HPP})})
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn('bounded entry E1: header entries under include/ are outside '
                              'guarded bounded admission: prior includes and build flags '
                              'are not proven', proc.stdout)
                self.assertEqual(counts(proc)['PASS(bounded)'], 0)
                self.assertEqual(counts(proc)['findings'], 1)
                self.assertNotIn('NEW ', proc.stdout)
                self.assertNotIn('stale', proc.stdout)


class NoteCitations(Tree):
    """Every line a note cites is read and compared with what the note names.

    src/loop.cpp (LOOP_CPP): 3 opens on_bar, 4 declares slots_ (a site of E1),
    5 guards and pushes (a site of E1), 6 closes the function.
    include/pineforge/fixed.hpp: 1 defines kRingSlots.
    docs/ring.md (RING_MD, written by the tests that cite it): 1 and 3 spell
    kRingSlots, 2 spells slots_ only, 4 spells neither.
    """

    RING_MD = ('kRingSlots caps the ring\n'
               'slots_ is the ring of the loop\n'
               'the guard compares its size with kRingSlots\n'
               'nothing about it here\n')
    # RING_MD with two more lines: 5 spells the qualified name ring::cap, 6
    # the growth operation reserve. Neither word has an underscore or an
    # inner capital.
    NAMES_MD = RING_MD + ('the reader asks ring::cap once\n'
                          'the ring calls reserve before the loop\n')
    # src/ring.cpp, written by the tests that cite it. It declares no
    # container, so it adds no site and needs no entry. 1 opens ring_cap, 2
    # opens the block inside it, 4 closes that block, 6 closes ring_cap; 7 is
    # ring_spare on one line; 8 and 9 are the header of ring_total, 10 and 11
    # one call of ring_sum written on two lines, 12 closes ring_total.
    RING_CPP = ('int ring_cap(int wanted) {\n'
                '    if (wanted < 1) {\n'
                '        return 1;\n'
                '    }\n'
                '    return wanted;\n'
                '}\n'
                'int ring_spare(int cap) { return cap / 2; }\n'
                'int ring_total(int cap,\n'
                '               int spare) {\n'
                '    return ring_sum(cap,\n'
                '                    spare);\n'
                '}\n')
    # What the gate says of a range that does not hold, before it names the line.
    RANGE_RULE = ('a range holds only when its first line does and its last line either does or '
                  'ends the block or statement that begins on the first, and ')

    def cite(self, note: str, ceiling=None, **files):
        """``ceiling`` is the number of UNCHECKED citations the manifest pins; None pins none."""
        self.build([entry('E1', note=note)], cited_unchecked=ceiling)
        write_tree(self.root, files)
        return self.gate()

    def held(self, note: str, citations: int, **files):
        proc = self.cite(note, **files)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (citations, citations, 0, 0), proc.stdout)
        self.assertNotIn('UNCHECKED entry', proc.stdout)
        return proc

    def tampered(self, old: str, new: str, finding: str, *, fails: int = 1):
        """Change one citation of the manifest the control just passed with."""
        path = self.root / MANIFEST_REL
        text = path.read_text()
        self.assertEqual(text.count(old), 1, f'{old!r} must occur once in the manifest')
        path.write_text(text.replace(old, new))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f'FINDING {finding}', proc.stdout)
        self.assertEqual(counts(proc).get('cited_fails'), fails, proc.stdout)
        self.assertIn('result: FINDINGS', proc.stdout)
        path.write_text(text)
        return proc

    def test_bare_citation_holds_on_the_entrys_site_and_fails_on_a_wrong_line(self):
        self.held('The guarded push_back is at :5.', 1)
        proc = self.tampered('is at :5.', 'is at :6.',
                             'src/loop.cpp:6: entry E1: note cites :6: src/loop.cpp:6 holds "}": '
                             "not a site of the entry ('slots_') and none of the constructs the "
                             'note names in that sentence (push_back)')
        self.assertEqual(counts(proc).get('cited_holds'), 0)
        self.assertEqual(counts(proc)['findings'], 1)

    def test_parenthesized_citation_is_the_same_bare_form(self):
        self.held('One guarded push_back (:5) and nothing else.', 1)
        self.tampered('(:5)', '(:3)', 'src/loop.cpp:3: entry E1: note cites :3: src/loop.cpp:3 '
                      'holds "void on_bar(double x) {"')

    def test_path_citation_resolves_in_that_path_not_in_the_entrys_file(self):
        self.held('The cap kRingSlots is defined at include/pineforge/fixed.hpp:1.', 1)
        self.tampered('include/pineforge/fixed.hpp:1.', 'src/loop.cpp:1.',
                      'src/loop.cpp:1: entry E1: note cites src/loop.cpp:1: src/loop.cpp:1 holds '
                      '"#include <vector>"')
        self.tampered('include/pineforge/fixed.hpp:1.', 'include/pineforge/fixed.hpp:2.',
                      'include/pineforge/fixed.hpp:2: entry E1: note cites '
                      'include/pineforge/fixed.hpp:2: include/pineforge/fixed.hpp has 1 line(s)')
        self.tampered('include/pineforge/fixed.hpp:1.', 'fixed.hpp:1.',
                      "fixed.hpp:1: entry E1: note cites fixed.hpp:1: 'fixed.hpp' is not a file "
                      'under the scanned root')

    def test_range_citation_holds_only_when_its_first_and_its_last_line_do(self):
        self.held('The declaration and its guarded push_back are at :4-5.', 1)
        self.tampered(':4-5.', ':1-3.', 'src/loop.cpp:1: entry E1: note cites :1-3: '
                      'src/loop.cpp:1-3 begins "#include <vector>"')
        # A site inside the range, or at one end of it, says nothing about
        # where the range begins or ends: each end is read. Line 6 closes the
        # function that line 3 opens, not the statement of line 5.
        push = 'if (slots_.size() < kRingSlots) slots_.push_back(1.0);'
        rule = self.RANGE_RULE
        tail = ("not a site of the entry ('slots_') and none of the constructs the note "
                'names in that sentence (push_back)')
        self.tampered(':4-5.', ':5-6.', 'src/loop.cpp:5: entry E1: note cites :5-6: '
                      f'src/loop.cpp:5-6 begins "{push}" and ends "}}": {rule}'
                      f'line 6 does not: {tail}')
        self.tampered(':4-5.', ':3-5.', 'src/loop.cpp:3: entry E1: note cites :3-5: '
                      f'src/loop.cpp:3-5 begins "void on_bar(double x) {{" and ends "{push}": '
                      f'{rule}line 3 does not: {tail}')
        proc = self.tampered(':4-5.', ':1-6.', 'src/loop.cpp:1: entry E1: note cites :1-6: '
                             'src/loop.cpp:1-6 begins "#include <vector>" and ends "}": '
                             f'{rule}neither line does: {tail}')
        self.assertEqual(counts(proc).get('cited_holds'), 0)

    def test_path_range_citation_is_judged_at_both_ends_in_that_path(self):
        self.held('The cap kRingSlots is described at docs/ring.md:1-3.', 1,
                  **{'docs/ring.md': self.RING_MD})
        self.tampered('docs/ring.md:1-3.', 'docs/ring.md:1-2.',
                      'docs/ring.md:1: entry E1: note cites docs/ring.md:1-2: docs/ring.md:1-2 '
                      'begins "kRingSlots caps the ring" and ends "slots_ is the ring of the '
                      f'loop": {self.RANGE_RULE}'
                      "line 2 does not: not in the entry's own file and none of the constructs "
                      'the note names in that sentence (kRingSlots)')
        self.tampered('docs/ring.md:1-3.', 'docs/ring.md:3-5.',
                      'docs/ring.md:3: entry E1: note cites docs/ring.md:3-5: docs/ring.md has '
                      '4 line(s)')

    def test_range_may_end_where_the_block_or_statement_of_its_first_line_ends(self):
        # Line 3 opens on_bar and line 6 is its closing brace. That line names
        # nothing, and it is where what the first line begins ends.
        self.held('The scope of on_bar is :3-6.', 1)
        rule = self.RANGE_RULE
        # The first line still has to hold: an end that is right does not
        # carry a beginning that is elsewhere.
        self.tampered('is :3-6.', 'is :2-6.', 'src/loop.cpp:2: entry E1: note cites :2-6: '
                      'src/loop.cpp:2-6 begins "#include "pineforge/fixed.hpp"" and ends "}": '
                      f"{rule}neither line does: not a site of the entry ('slots_') and none of "
                      'the constructs the note names in that sentence (on_bar)')
        ring = {'src/ring.cpp': self.RING_CPP}
        tail = ("not in the entry's own file and none of the constructs the note names in "
                'that sentence (ring_cap)')
        self.held('The function ring_cap is src/ring.cpp:1-6.', 1, **ring)
        # Line 4 closes the block inside ring_cap and line 7 is the next
        # function: neither is where what line 1 begins ends.
        self.tampered('src/ring.cpp:1-6.', 'src/ring.cpp:1-4.',
                      'src/ring.cpp:1: entry E1: note cites src/ring.cpp:1-4: src/ring.cpp:1-4 '
                      f'begins "int ring_cap(int wanted) {{" and ends "}}": {rule}'
                      f'line 4 does not: {tail}')
        self.tampered('src/ring.cpp:1-6.', 'src/ring.cpp:1-7.',
                      'src/ring.cpp:1: entry E1: note cites src/ring.cpp:1-7: src/ring.cpp:1-7 '
                      'begins "int ring_cap(int wanted) {" and ends "int ring_spare(int cap) { '
                      f'return cap / 2; }}": {rule}line 7 does not: {tail}')
        # A whole block whose first line does not hold is not held for being
        # a whole block: lines 2 to 4 open and close, and name nothing.
        proc = self.tampered('src/ring.cpp:1-6.', 'src/ring.cpp:2-4.',
                             'src/ring.cpp:2: entry E1: note cites src/ring.cpp:2-4: '
                             'src/ring.cpp:2-4 begins "if (wanted < 1) {" and ends "}": '
                             f'{rule}neither line does: {tail}')
        self.assertEqual(counts(proc).get('cited_holds'), 0)
        # A call written on two lines ends on its second line, and a header
        # written on two lines begins the block that line 12 closes.
        self.held('The call of ring_sum is at src/ring.cpp:10-11.', 1, **ring)
        self.tampered('src/ring.cpp:10-11.', 'src/ring.cpp:10-12.',
                      'src/ring.cpp:10: entry E1: note cites src/ring.cpp:10-12: '
                      'src/ring.cpp:10-12 begins "return ring_sum(cap," and ends "}": '
                      f"{rule}line 12 does not: not in the entry's own file and none of the "
                      'constructs the note names in that sentence (ring_sum)')
        self.held('The function ring_total is src/ring.cpp:8-12.', 1, **ring)
        # Only a C or C++ source is read for blocks: the same text in a file
        # of another kind holds at both its ends or not at all.
        self.held('The function ring_cap is src/ring.cpp:1-6.', 1, **ring,
                  **{'docs/ring.md': self.RING_CPP})
        self.tampered('src/ring.cpp:1-6.', 'docs/ring.md:1-6.',
                      'docs/ring.md:1: entry E1: note cites docs/ring.md:1-6: docs/ring.md:1-6 '
                      f'begins "int ring_cap(int wanted) {{" and ends "}}": {rule}'
                      f'line 6 does not: {tail}')

    def test_range_inside_a_list_is_judged_as_a_range(self):
        self.held('The scope of on_bar, its declaration and its push_back are at :3,4-5.', 2)
        proc = self.tampered(':3,4-5.', ':3,4-6.', 'src/loop.cpp:4: entry E1: note cites :4-6: '
                             'src/loop.cpp:4-6 begins "static std::vector<double> slots_;" and '
                             'ends "}": ')
        self.assertIn("line 6 does not: not a site of the entry ('slots_') and none of the "
                      'constructs the note names in that sentence (on_bar, push_back)',
                      proc.stdout)
        self.assertEqual(counts(proc).get('cited_holds'), 1)
        self.assertEqual(counts(proc).get('citations'), 2)

    def test_reversed_range_is_a_finding(self):
        self.held('The declaration and its guarded push_back are at :4-5.', 1)
        proc = self.tampered(':4-5.', ':5-4.', 'src/loop.cpp:5: entry E1: note cites :5-4: '
                             'src/loop.cpp has 6 line(s)')
        self.assertEqual(counts(proc).get('cited_holds'), 0)
        self.assertEqual(counts(proc).get('citations'), 1)

    def test_comma_list_checks_each_line_it_names(self):
        self.held('The declaration and the push_back are at :4,5.', 2)
        proc = self.tampered(':4,5.', ':4,6.', 'src/loop.cpp:6: entry E1: note cites :6: '
                             'src/loop.cpp:6 holds "}"')
        self.assertEqual(counts(proc).get('cited_holds'), 1)
        self.assertEqual(counts(proc).get('citations'), 2)

    def test_path_comma_list_checks_each_line_in_that_path(self):
        self.held('The cap kRingSlots is described at docs/ring.md:1,3.', 2,
                  **{'docs/ring.md': self.RING_MD})
        proc = self.tampered('docs/ring.md:1,3.', 'docs/ring.md:1,4.',
                             'docs/ring.md:4: entry E1: note cites docs/ring.md:4: '
                             'docs/ring.md:4 holds "nothing about it here"')
        self.assertEqual(counts(proc).get('cited_holds'), 1)
        self.assertEqual(counts(proc).get('citations'), 2)

    def test_spaced_list_reads_every_line_it_names(self):
        for join in (', ', ' and ', ' or ', ', and '):
            with self.subTest(join=join):
                self.held(f'The declaration and the push_back are at :4{join}5.', 2)
                proc = self.tampered(f':4{join}5.', f':4{join}6.',
                                     'src/loop.cpp:6: entry E1: note cites :6: '
                                     'src/loop.cpp:6 holds "}"')
                self.assertEqual(counts(proc).get('cited_holds'), 1)
                self.assertEqual(counts(proc).get('citations'), 2)

    def test_bare_citation_joined_to_a_path_is_a_line_of_that_path(self):
        for join in (' and ', ', ', ' or ', ', and '):
            with self.subTest(join=join):
                self.held(f'The cap kRingSlots is described at docs/ring.md:1{join}:3.', 2,
                          **{'docs/ring.md': self.RING_MD})
                # Line 5 of the entry's own file is a site of E1; docs/ring.md
                # has four lines. The joined form is read in the path it continues.
                proc = self.tampered(f'docs/ring.md:1{join}:3.', f'docs/ring.md:1{join}:5.',
                                     'docs/ring.md:5: entry E1: note cites :5: docs/ring.md '
                                     'has 4 line(s)')
                self.assertEqual(counts(proc).get('cited_holds'), 1)
        # After any other words a bare form is a line of the entry's own file.
        self.held('The cap kRingSlots is described at docs/ring.md:1 and slots_ is pushed '
                  'at :5.', 2, **{'docs/ring.md': self.RING_MD})

    def test_line_word_citation_is_a_line_of_the_entrys_file(self):
        self.held('Line 5 is the guarded push_back and lines 4-5 hold the declaration too.', 2)
        self.tampered('Line 5 is', 'Line 6 is', 'src/loop.cpp:6: entry E1: note cites Line 6: '
                      'src/loop.cpp:6 holds "}"')

    def test_construct_the_note_spells_holds_on_a_line_that_is_not_a_site(self):
        self.held('The scope of on_bar opens at :3.', 1)
        self.tampered('opens at :3.', 'opens at :2.',
                      'src/loop.cpp:2: entry E1: note cites :2: src/loop.cpp:2 holds '
                      '"#include "pineforge/fixed.hpp"": not a site of the entry (\'slots_\') '
                      'and none of the constructs the note names in that sentence (on_bar)')
        # Backticked text is spelled as code even when its words are plain.
        self.held('The include of `vector` is at :1.', 1)
        self.tampered('is at :1.', 'is at :6.', 'src/loop.cpp:6: entry E1: note cites :6: '
                      'src/loop.cpp:6 holds "}": not a site of the entry (\'slots_\') and none '
                      'of the constructs the note names in that sentence (vector)')

    def test_member_call_the_note_spells_is_a_construct_to_look_for(self):
        ring = self.RING_MD + 'the reader takes ring.back() once\n'
        self.held('The last value is read through ring.back() at docs/ring.md:5.', 1,
                  **{'docs/ring.md': ring})
        self.tampered('docs/ring.md:5.', 'docs/ring.md:4.',
                      'docs/ring.md:4: entry E1: note cites docs/ring.md:4: docs/ring.md:4 holds '
                      '"nothing about it here": not in the entry\'s own file and none of the '
                      'constructs the note names in that sentence (back, ring)')

    def test_parenthesized_clause_stays_in_the_sentence_that_names_the_construct(self):
        # The semicolon inside the parenthesis does not end the sentence, so
        # the second citation is read against on_bar too, not left as prose.
        self.held('The scope of on_bar is one function (:3; nothing else opens at :3).', 2)
        self.tampered('opens at :3).', 'opens at :6).',
                      'src/loop.cpp:6: entry E1: note cites :6: src/loop.cpp:6 holds "}": '
                      "not a site of the entry ('slots_') and none of the constructs the note "
                      'names in that sentence (on_bar)')

    def test_citation_the_note_calls_a_site_must_be_a_site(self):
        self.held('Site :5 pushes one value.', 1)
        self.tampered('Site :5', 'Site :6', 'src/loop.cpp:6: entry E1: note cites :6: '
                      'src/loop.cpp:6 holds "}": not a site of the entry (\'slots_\'), and the '
                      'note calls it a site')

    def test_every_citation_the_word_sites_introduces_must_be_a_site(self):
        called = ('src/loop.cpp:6: entry E1: note cites :6: src/loop.cpp:6 holds "}": not a '
                  "site of the entry ('slots_'), and the note calls it a site")
        # A list joined to the first citation.
        self.held('Sites :4 and :5 declare the ring and fill it.', 2)
        proc = self.tampered('and :5 declare', 'and :6 declare', called)
        self.assertEqual(counts(proc).get('cited_holds'), 1)
        self.held('The two sites (:4, :5) declare the ring and fill it.', 2)
        self.tampered(':4, :5)', ':4, :6)', called)
        # A later citation of the same sentence, after other words.
        self.held('Sites :4 for the ring and :5 for its one value.', 2)
        self.tampered('and :5 for', 'and :6 for', called)

    def test_site_claims_a_site_for_the_list_joined_to_it_and_for_nothing_else(self):
        # The singular: the citation it introduces and the list joined to it.
        self.held('Site :4 and :5 hold the ring.', 2)
        proc = self.tampered('and :5 hold', 'and :6 hold',
                             'src/loop.cpp:6: entry E1: note cites :6: src/loop.cpp:6 holds "}": '
                             "not a site of the entry ('slots_'), and the note calls it a site")
        self.assertEqual(counts(proc).get('cited_holds'), 1)
        # A citation that other words separate from it is not called a site:
        # in a sentence of prose it is UNCHECKED, not a finding.
        proc = self.cite('Site :4 declares the ring and the function ends at :6.', ceiling=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (2, 1, 0, 1), proc.stdout)
        self.assertIn('UNCHECKED entry E1: note cites :6: src/loop.cpp:6 holds "}"', proc.stdout)

    def test_site_claim_is_not_met_by_a_construct_the_sentence_spells(self):
        # Line 3 spells on_bar and is no site of E1. The sentence calls every
        # line it cites a site, so the name does not hold line 3.
        self.held('Sites :4 and :5 sit in on_bar.', 2)
        proc = self.tampered('and :5 sit', 'and :3 sit',
                             'src/loop.cpp:3: entry E1: note cites :3: src/loop.cpp:3 holds '
                             '"void on_bar(double x) {": not a site of the entry (\'slots_\'), '
                             'and the note calls it a site')
        self.assertEqual(counts(proc).get('cited_holds'), 1)
        self.assertEqual(counts(proc).get('citations'), 2)
        # The entry records no site in another file, whatever that line spells.
        self.held('Site :5 pushes into slots_.', 1, **{'docs/ring.md': self.RING_MD})
        self.tampered('Site :5 pushes', 'Site docs/ring.md:2 pushes',
                      'docs/ring.md:2: entry E1: note cites docs/ring.md:2: docs/ring.md:2 holds '
                      '"slots_ is the ring of the loop": not in the entry\'s own file, and the '
                      'note calls it a site')

    def test_unclosed_parenthesis_does_not_join_the_sentences_after_it(self):
        # The parenthesis never closes, so the first sentence still ends at
        # its period: it is prose, and on_bar of the next sentence is not a
        # name its citation can borrow. Line 3 spells on_bar.
        proc = self.cite('The function ends (at :3. The scope of on_bar is one function.',
                         ceiling=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (1, 0, 0, 1), proc.stdout)
        self.assertIn('UNCHECKED entry E1: note cites :3: src/loop.cpp:3 holds '
                      '"void on_bar(double x) {": not a site of the entry (\'slots_\'), and the '
                      'sentence spells no construct to look for', proc.stdout)
        # A parenthesis that closes still keeps its clause in the sentence.
        self.held('The function opens (at :3; the scope of on_bar is one function).', 1)

    def test_qualified_name_the_note_spells_is_a_construct_to_look_for(self):
        names = {'docs/ring.md': self.NAMES_MD}
        self.held('The limit ring::cap is read at docs/ring.md:5.', 1, **names)
        self.tampered('docs/ring.md:5.', 'docs/ring.md:4.',
                      'docs/ring.md:4: entry E1: note cites docs/ring.md:4: docs/ring.md:4 holds '
                      '"nothing about it here": not in the entry\'s own file and none of the '
                      'constructs the note names in that sentence (cap, ring)')

    def test_growth_operation_without_an_underscore_is_a_construct_to_look_for(self):
        names = {'docs/ring.md': self.NAMES_MD}
        self.held('The reserve before the loop is described at docs/ring.md:6.', 1, **names)
        self.tampered('docs/ring.md:6.', 'docs/ring.md:4.',
                      'docs/ring.md:4: entry E1: note cites docs/ring.md:4: docs/ring.md:4 holds '
                      '"nothing about it here": not in the entry\'s own file and none of the '
                      'constructs the note names in that sentence (reserve)')

    def test_keywords_and_type_words_are_never_the_construct_a_note_names(self):
        # Line 3 spells void and double. Backticked or not, such a word sits
        # on almost any line: it names nothing, so the citation is neither
        # held through it nor failed for it.
        proc = self.cite('The `void` return and the `double` argument open at :3.', ceiling=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (1, 0, 0, 1), proc.stdout)
        self.assertIn('UNCHECKED entry E1: note cites :3: src/loop.cpp:3 holds '
                      '"void on_bar(double x) {": not a site of the entry (\'slots_\'), and the '
                      'sentence spells no construct to look for', proc.stdout)
        # Beside a real name the excluded words are not listed either.
        self.held('The `void` return of on_bar opens at :3.', 1)
        self.tampered('opens at :3.', 'opens at :6.',
                      'src/loop.cpp:6: entry E1: note cites :6: src/loop.cpp:6 holds "}": not a '
                      "site of the entry ('slots_') and none of the constructs the note names "
                      'in that sentence (on_bar)')

    def test_template_and_fixed_input_notes_are_listed_unchecked_never_resolved(self):
        self.build([entry('E1')], cited_unchecked=2)
        path = self.root / MANIFEST_REL
        text = path.read_text()
        # Line 5 of the loop is a site of E1 and line 1 of the header defines
        # the fixed input: both citations would hold in an entry note.
        for old, note in (('state: "unreviewed"}', 'See :5.'),
                          ('fixed_before_bar0: true}',
                           'Defined at include/pineforge/fixed.hpp:1.')):
            self.assertEqual(text.count(old), 1, f'{old!r} must occur once in the manifest')
            text = text.replace(old, f'{old[:-1]}, note: {q(note)}}}')
        path.write_text(text)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (2, 0, 0, 2), proc.stdout)
        resolved = ': only an entry note is resolved against the tree'
        self.assertIn(f'UNCHECKED emitter template T1: note cites :5{resolved}', proc.stdout)
        self.assertIn('UNCHECKED fixed input kRingSlots: note cites '
                      f'include/pineforge/fixed.hpp:1{resolved}', proc.stdout)

    def test_json_result_carries_a_failing_citation_as_a_finding(self):
        note = 'The guarded push_back is at :5.'
        self.build([entry('E1', note=note)])
        with tempfile.TemporaryDirectory() as out:
            result = Path(out) / 'gate.json'
            proc = self.gate('--json', str(result))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            data = json.loads(result.read_text())
            self.assertEqual((data['verdict'], data['findings']), ('OPEN-ONLY', []))
            self.assertEqual(len(data['entries']), 1)
            row = data['entries'][0]
            self.assertEqual((row['id'], row['note'], row['matched'], row['live_lines']),
                             ('E1', note, True, [4, 5]))
            path = self.root / MANIFEST_REL
            text = path.read_text()
            self.assertEqual(text.count('is at :5.'), 1)
            path.write_text(text.replace('is at :5.', 'is at :6.'))
            proc = self.gate('--json', str(result))
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            data = json.loads(result.read_text())
            self.assertEqual(data['verdict'], 'FINDINGS')
            self.assertEqual(data['findings'], [{
                'file': 'src/loop.cpp', 'line': 6,
                'message': 'entry E1: note cites :6: src/loop.cpp:6 holds "}": not a site of '
                           "the entry ('slots_') and none of the constructs the note names in "
                           'that sentence (push_back)'}])

    def test_another_file_holds_only_a_construct_the_sentence_names(self):
        self.held('The ring slots_ is described at docs/ring.md:2.', 1,
                  **{'docs/ring.md': self.RING_MD})
        self.tampered('docs/ring.md:2.', 'docs/ring.md:4.',
                      'docs/ring.md:4: entry E1: note cites docs/ring.md:4: docs/ring.md:4 holds '
                      '"nothing about it here": not in the entry\'s own file and none of the '
                      'constructs the note names in that sentence (slots_)')
        # The same spelling as the entry's symbol on a line of another file is
        # not a site of the entry: a sentence of prose that cites it names
        # nothing a text check can look for. The gate says so with the entry,
        # the citation and the line, and holds nothing.
        proc = self.cite('The ring is described at docs/ring.md:2.', ceiling=1,
                         **{'docs/ring.md': self.RING_MD})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (1, 0, 0, 1), proc.stdout)
        self.assertIn('UNCHECKED entry E1: note cites docs/ring.md:2: docs/ring.md:2 holds '
                      '"slots_ is the ring of the loop": not in the entry\'s own file, and the '
                      'sentence spells no construct to look for', proc.stdout)

    def test_prose_citation_in_the_entrys_file_is_unchecked_never_holding(self):
        proc = self.cite('The function ends at :6.', ceiling=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (1, 0, 0, 1), proc.stdout)
        self.assertIn('UNCHECKED entry E1: note cites :6: src/loop.cpp:6 holds "}"', proc.stdout)
        self.assertEqual(sum(line.startswith('cr150_source_gate: UNCHECKED ')
                             for line in proc.stdout.splitlines()), 1)

    def test_clock_time_is_not_a_citation_and_an_unread_form_is_reported(self):
        proc = self.cite('Reviewed at 17:20 and nothing is cited.')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc).get('citations'), 0, proc.stdout)
        proc = self.cite('The push is at -:5 of the file.', ceiling=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc).get('cited_unchecked'), 1, proc.stdout)
        self.assertIn("UNCHECKED entry E1: note has ':5' in a form no citation rule reads",
                      proc.stdout)

    def test_line_number_in_a_form_no_rule_reads_is_not_a_citation(self):
        # What the manifest header states: the gate reads `:N`, `path:N` and
        # `line N`, and no other form. A line number written another way and
        # without `:N` is neither checked nor counted, although line 6 holds
        # no push_back.
        proc = self.cite('The guarded push_back is at LINE 6.')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (0, 0, 0, 0), proc.stdout)
        # The same sentence in a form the gate reads is checked.
        self.held('The guarded push_back is at line 5.', 1)
        self.tampered('at line 5.', 'at line 6.', 'src/loop.cpp:6: entry E1: note cites line 6: '
                      'src/loop.cpp:6 holds "}"')

    def test_a_note_without_a_citation_adds_nothing(self):
        proc = self.cite('One ring of fixed size. No line is named.')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_fails'), c.get('cited_unchecked')),
                         (0, 0, 0), proc.stdout)

    # Two sentences of prose, each citing a line that is not a site of E1:
    # two UNCHECKED citations, whatever the ceiling. The manifest pins their
    # number exactly: two pass with a ceiling of two and with no other.
    TWO_UNCHECKED = 'The function ends at :6. The includes end at :2.'
    OVER_CEILING = (f'FINDING {MANIFEST_REL}:0: UNCHECKED note citations 2 exceed the manifest '
                    'ceiling ')
    UNDER_CEILING = (f'FINDING {MANIFEST_REL}:0: UNCHECKED note citations 2 are below the manifest '
                     'ceiling ')

    def unchecked_run(self, ceiling):
        proc = self.cite(self.TWO_UNCHECKED, ceiling=ceiling)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked')), (2, 0, 0, 2), proc.stdout)
        self.assertEqual(sum(line.startswith('cr150_source_gate: UNCHECKED ')
                             for line in proc.stdout.splitlines()), 2)
        return proc, c

    def test_unchecked_citations_at_the_manifest_ceiling_pass(self):
        proc, c = self.unchecked_run(2)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(c.get('cited_unchecked_ceiling'), 2, proc.stdout)
        self.assertEqual(c.get('findings'), 0, proc.stdout)
        self.assertNotIn('cited_unchecked_slack', proc.stdout)

    def test_unchecked_citations_above_the_manifest_ceiling_fail(self):
        proc, c = self.unchecked_run(1)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(c.get('cited_unchecked_ceiling'), 1, proc.stdout)
        self.assertIn(self.OVER_CEILING + '1: ', proc.stdout)
        self.assertNotIn('pins no ceilings.cited_unchecked', proc.stdout)
        # The excess is one finding of the manifest, not a failed citation.
        self.assertEqual(c.get('findings'), 1, proc.stdout)
        self.assertIn('result: FINDINGS', proc.stdout)

    def test_unchecked_citations_below_the_manifest_ceiling_fail_until_it_is_lowered(self):
        # Room under the ceiling would take a later UNCHECKED citation unseen
        # (a checked sentence reworded into prose), so the gap itself fails.
        for ceiling in (3, 5):
            with self.subTest(ceiling=ceiling):
                proc, c = self.unchecked_run(ceiling)
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertEqual(c.get('cited_unchecked_ceiling'), ceiling, proc.stdout)
                self.assertIn(f'{self.UNDER_CEILING}{ceiling}: lower ceilings.cited_unchecked '
                              'to 2 in the same change', proc.stdout)
                self.assertNotIn('exceed the manifest ceiling', proc.stdout)
                self.assertNotIn('cited_unchecked_slack', proc.stdout)
                # The gap is one finding of the manifest, not a failed citation.
                self.assertEqual(c.get('findings'), 1, proc.stdout)
                self.assertIn('result: FINDINGS', proc.stdout)

    def test_manifest_without_an_unchecked_ceiling_allows_no_unchecked_citation(self):
        # The control: the citation holds, and no ceiling is needed for it.
        self.held('The guarded push_back is at :5.', 1)
        # Moving it to a wrong line fails (the test of the bare form). Taking
        # the name out of the sentence as well would make it prose, UNCHECKED
        # and unseen: without ceiling room that is a finding too.
        path = self.root / MANIFEST_REL
        text = path.read_text()
        self.assertEqual(text.count('The guarded push_back is at :5.'), 1)
        path.write_text(text.replace('The guarded push_back is at :5.',
                                     'The guarded push is at :6.'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds'), c.get('cited_fails'),
                          c.get('cited_unchecked'), c.get('cited_unchecked_ceiling')),
                         (1, 0, 0, 1, 0), proc.stdout)
        self.assertIn(f'FINDING {MANIFEST_REL}:0: UNCHECKED note citations 1 exceed the manifest '
                      'ceiling 0 (the manifest pins no ceilings.cited_unchecked, so none is '
                      'allowed): ', proc.stdout)
        self.assertIn('UNCHECKED entry E1: note cites :6: src/loop.cpp:6 holds "}"', proc.stdout)
        self.assertEqual(c.get('findings'), 1, proc.stdout)

    def test_unchecked_ceiling_must_be_a_non_negative_integer(self):
        for value in ('-1', '"many"', 'true'):
            with self.subTest(value=value):
                self.build([entry('E1', note=self.TWO_UNCHECKED)], cited_unchecked=value)
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn('ERROR: ceilings.cited_unchecked must be a non-negative integer',
                              proc.stdout)


class LineFields(Tree):
    """An entry's ``line:`` field is the line of its first live site.

    src/loop.cpp (LOOP_CPP): slots_ is declared on line 4 and pushed on line
    5, the two sites of an entry for slots_. Its first live site is line 4,
    the number ``--emit-seed`` writes and ``--json`` reports first.
    """

    def line_counts(self, proc):
        c = counts(proc)
        return (c.get('line_fields'), c.get('line_holds'), c.get('line_differs'),
                c.get('line_not_compared'))

    def test_line_field_on_the_first_live_site_passes_and_another_number_is_a_finding(self):
        self.build([entry('E1', line=4)])
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.line_counts(proc), (1, 1, 0, 0), proc.stdout)
        self.assertEqual(counts(proc)['findings'], 0, proc.stdout)
        path = self.root / MANIFEST_REL
        text = path.read_text()
        self.assertEqual(text.count('line: 4}'), 1, 'the entry must carry its line field once')
        # One off either way. Line 5 is a site of E1 too, and not its first;
        # line 3 opens the function and is no site.
        for wrong in (5, 3):
            with self.subTest(line=wrong):
                path.write_text(text.replace('line: 4}', f'line: {wrong}}}'))
                proc = self.gate()
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn(f'FINDING src/loop.cpp:4: entry E1: line: {wrong} is not the first '
                              "live site of the entry ('slots_'), which is line 4: write "
                              'line: 4, the number --emit-seed gives', proc.stdout)
                self.assertEqual(self.line_counts(proc), (1, 0, 1, 0), proc.stdout)
                self.assertEqual(counts(proc)['findings'], 1, proc.stdout)
                self.assertIn('result: FINDINGS', proc.stdout)

    def test_entry_without_a_line_field_has_nothing_to_compare(self):
        # The field is optional: an entry that carries none states no line,
        # and the gate counts no field for it.
        self.build([entry('E1')])
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.line_counts(proc), (0, 0, 0, 0), proc.stdout)

    def test_line_field_of_an_entry_with_no_live_site_is_not_compared_and_the_entry_is_stale(self):
        # gone_ is nowhere in the tree: E2 has no live site, so there is no
        # line to hold its field against. The entry fails as stale, the
        # field is counted as not compared and never as holding.
        self.build(self.clean_entries() + [entry('E2', symbol='gone_', ops=['grow'], line=5)])
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("FINDING src/loop.cpp:5: entry E2 ('gone_') matches no site: stale; remove "
                      'or update it; its line: 5 is not compared, the entry has no live site',
                      proc.stdout)
        self.assertNotIn('is not the first live site', proc.stdout)
        self.assertEqual(self.line_counts(proc), (1, 0, 0, 1), proc.stdout)
        self.assertEqual(counts(proc)['findings'], 1, proc.stdout)

    def test_line_field_that_emit_seed_writes_is_the_one_the_gate_accepts(self):
        # A tree whose manifest has no entry yet: --emit-seed prints the one
        # missing entry with its line field, and that row passes as written.
        self.build([])
        seed = self.gate('--emit-seed')
        self.assertEqual(seed.returncode, 0, seed.stdout + seed.stderr)
        rows = seed.stdout.splitlines()
        self.assertEqual(len(rows), 1, seed.stdout)
        self.assertRegex(rows[0], r'^  - \{id: "U0001", file: "src/loop\.cpp", symbol: "slots_", '
                         r'.*, line: 4\}$')
        self.build(rows)
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.line_counts(proc), (1, 1, 0, 0), proc.stdout)
        self.assertEqual(counts(proc)['findings'], 0, proc.stdout)


class CommentCitations(Tree):
    """A citation in a manifest comment is judged as one in a note of no entry.

    src/loop.cpp (LOOP_CPP): 3 opens on_bar, 4 declares slots_, 5 guards and
    pushes, 6 closes the function. A comment has no file of its own, so it
    writes the path of every line it cites, and it records no site, so a
    cited line holds only on a construct its sentence spells.
    """

    def comment(self, *rows, **kw):
        """Build the tree, write ``rows`` as comment lines at the end of its
        manifest and run the gate. Returns the result and the manifest line
        of the first row."""
        self.build([entry('E1')], **kw)
        path = self.root / MANIFEST_REL
        text = path.read_text()
        path.write_text(text + ''.join(f'# {row}\n' for row in rows))
        return self.gate(), text.count('\n') + 1

    def comment_counts(self, proc):
        c = counts(proc)
        return (c.get('comment_citations'), c.get('comment_holds'), c.get('comment_fails'),
                c.get('comment_unchecked'))

    def test_comment_citation_holds_on_a_construct_it_names_and_fails_on_a_wrong_line(self):
        proc, at = self.comment('The guarded push_back is at src/loop.cpp:5.')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.comment_counts(proc), (1, 1, 0, 0), proc.stdout)
        # Counted apart from the note citations: this manifest has none.
        c = counts(proc)
        self.assertEqual((c.get('citations'), c.get('cited_holds')), (0, 0), proc.stdout)
        proc, at = self.comment('The guarded push_back is at src/loop.cpp:6.')
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f'FINDING src/loop.cpp:6: manifest line {at}: comment cites '
                      'src/loop.cpp:6: src/loop.cpp:6 holds "}": a comment records no site and '
                      'none of the constructs the comment names in that sentence (push_back)',
                      proc.stdout)
        self.assertEqual(self.comment_counts(proc), (1, 0, 1, 0), proc.stdout)
        self.assertEqual(counts(proc)['findings'], 1, proc.stdout)
        self.assertIn('result: FINDINGS', proc.stdout)

    def test_comment_sentence_runs_over_its_lines_until_a_line_with_no_words(self):
        # The name is on the first comment line and the citation on the
        # second: one sentence, and the finding names the line that cites.
        first = 'The guarded push_back of the ring is'
        proc, at = self.comment(first, 'at src/loop.cpp:5.')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.comment_counts(proc), (1, 1, 0, 0), proc.stdout)
        proc, at = self.comment(first, 'at src/loop.cpp:6.')
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f'FINDING src/loop.cpp:6: manifest line {at + 1}: comment cites '
                      'src/loop.cpp:6: src/loop.cpp:6 holds "}": a comment records no site and '
                      'none of the constructs the comment names in that sentence (push_back)',
                      proc.stdout)
        # A comment line with no words ends the text before it: the citation
        # after it stands in a sentence of prose, which names nothing.
        proc, at = self.comment(first, '', 'at src/loop.cpp:6.', comment_unchecked=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.comment_counts(proc), (1, 0, 0, 1), proc.stdout)
        self.assertIn(f'UNCHECKED manifest line {at + 2}: comment cites src/loop.cpp:6: '
                      'src/loop.cpp:6 holds "}": a comment records no site, and the sentence '
                      'spells no construct to look for', proc.stdout)

    def test_comment_after_data_on_its_line_is_read(self):
        self.build([entry('E1')])
        path = self.root / MANIFEST_REL
        text = path.read_text()
        self.assertEqual(text.count('\nentries:\n'), 1, 'the manifest must open entries once')
        at = text[:text.index('\nentries:\n')].count('\n') + 2
        path.write_text(text.replace(
            '\nentries:\n', '\nentries:  # The guarded push_back is at src/loop.cpp:6.\n'))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f'FINDING src/loop.cpp:6: manifest line {at}: comment cites '
                      'src/loop.cpp:6: src/loop.cpp:6 holds "}"', proc.stdout)
        self.assertEqual(self.comment_counts(proc), (1, 0, 1, 0), proc.stdout)

    def test_comment_citation_without_a_path_is_a_finding(self):
        # In a note these are lines of the entry's own file. A comment has
        # no file of its own, although line 5 holds the push_back it names.
        for form in (':5', 'line 5'):
            with self.subTest(form=form):
                proc, at = self.comment(f'The guarded push_back is at {form}.')
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn(f'FINDING {MANIFEST_REL}:{at}: manifest line {at}: comment cites '
                              f'{form}: a comment has no file of its own, so a line it cites is '
                              'written with its path (path:N)', proc.stdout)
                self.assertEqual(self.comment_counts(proc), (1, 0, 1, 0), proc.stdout)
                self.assertEqual(counts(proc)['findings'], 1, proc.stdout)
        # A bare form joined to a path is a line of that path, as in a note.
        proc, at = self.comment('The declaration of slots_ and its push_back are at '
                                'src/loop.cpp:4 and :5.')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.comment_counts(proc), (2, 2, 0, 0), proc.stdout)

    def test_comment_calling_a_cited_line_a_site_fails_on_a_site_of_an_entry(self):
        # Line 5 is a site of E1. A comment records no site, so the claim
        # does not hold there, and no construct stands in for it.
        proc, at = self.comment('Site src/loop.cpp:5 pushes one value with push_back.')
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(f'FINDING src/loop.cpp:5: manifest line {at}: comment cites '
                      'src/loop.cpp:5: src/loop.cpp:5 holds "if (slots_.size() < kRingSlots) '
                      'slots_.push_back(1.0);": a comment records no site, and the comment '
                      'calls it a site', proc.stdout)
        self.assertEqual(self.comment_counts(proc), (1, 0, 1, 0), proc.stdout)

    PROSE = 'The function ends at src/loop.cpp:6.'

    def test_prose_comment_citation_is_unchecked_and_counted_apart_from_the_notes(self):
        proc, at = self.comment(self.PROSE, comment_unchecked=1)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.comment_counts(proc), (1, 0, 0, 1), proc.stdout)
        c = counts(proc)
        self.assertEqual(c.get('comment_unchecked_ceiling'), 1, proc.stdout)
        self.assertEqual((c.get('citations'), c.get('cited_unchecked')), (0, 0), proc.stdout)
        self.assertEqual(c.get('findings'), 0, proc.stdout)
        self.assertIn(f'UNCHECKED manifest line {at}: comment cites src/loop.cpp:6: '
                      'src/loop.cpp:6 holds "}": a comment records no site, and the sentence '
                      'spells no construct to look for', proc.stdout)
        self.assertEqual(sum(line.startswith('cr150_source_gate: UNCHECKED ')
                             for line in proc.stdout.splitlines()), 1)

    def test_unchecked_comment_citations_are_pinned_exactly(self):
        # No pin allows none, and the pin of the note citations is another
        # number: it does not cover a comment.
        for pins in ({}, {'cited_unchecked': 0}):
            with self.subTest(pins=pins):
                proc, at = self.comment(self.PROSE, **pins)
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertEqual(self.comment_counts(proc), (1, 0, 0, 1), proc.stdout)
                self.assertIn(f'FINDING {MANIFEST_REL}:0: UNCHECKED comment citations 1 exceed '
                              'the manifest ceiling 0 (the manifest pins no '
                              'ceilings.comment_unchecked, so none is allowed): ', proc.stdout)
                self.assertEqual(counts(proc)['findings'], 1, proc.stdout)
        # Room under the pin fails too, until the pin follows the count down.
        proc, at = self.comment(self.PROSE, comment_unchecked=2)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(counts(proc).get('comment_unchecked_ceiling'), 2, proc.stdout)
        self.assertIn(f'FINDING {MANIFEST_REL}:0: UNCHECKED comment citations 1 are below the '
                      'manifest ceiling 2: lower ceilings.comment_unchecked to 1 in the same '
                      'change', proc.stdout)
        self.assertEqual(counts(proc)['findings'], 1, proc.stdout)


class FailClosed(Tree):
    def test_blank_inventory_scope_members_are_malformed(self):
        self.build(self.clean_entries())
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertEqual(counts(control)['PASS(bounded)'], 1)
        path = self.root / SITE_INVENTORY_REL
        original = path.read_text()
        # 7331cbae29 accepts whitespace-only list members, then returns exit 1
        # for stale/NEW identities. This is an inventory error, before judging.
        for value in (' ', '\t', '\r\n', '\f\v', ' \t\r\n '):
            with self.subTest(scope=repr(value)):
                inventory = json.loads(original)
                inventory['sites'][0]['scope'] = [value]
                path.write_text(json.dumps(inventory))
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn('ERROR: site inventory record: field scope must be a list of strings',
                              proc.stdout)
                self.assertIn('result: ERROR (the gate could not judge; this is never a pass)',
                              proc.stdout)
                # Exit 2 emits no verdict counts at all, including no bounded
                # pass count; do not substitute an invented zero for missing data.
                self.assertNotIn('PASS(bounded)', proc.stdout)
                self.assertNotIn('NEW ', proc.stdout)
                self.assertNotIn('stale', proc.stdout)

    def test_blank_inventory_file_or_symbol_is_malformed(self):
        self.build(self.clean_entries())
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        path = self.root / SITE_INVENTORY_REL
        original = path.read_text()
        # 28eac95877 accepts these fields through placeholder_ok=True, then
        # reports stale/NEW identities with exit 1. They must be schema errors.
        for field in ('file', 'symbol'):
            for value in ('', ' \t '):
                with self.subTest(field=field, value=value):
                    inventory = json.loads(original)
                    inventory['sites'][0][field] = value
                    path.write_text(json.dumps(inventory))
                    proc = self.gate()
                    self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                    self.assertIn(f'site inventory record: field {field} is empty or a placeholder',
                                  proc.stdout)
                    self.assertNotIn('stale', proc.stdout)

    def test_inventory_spelling_cannot_be_empty_or_a_placeholder(self):
        self.build(self.clean_entries())
        control = self.gate()
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        path = self.root / SITE_INVENTORY_REL
        original = path.read_text()
        for spelling in ('', 'TODO', '?', 'none', 'unknown'):
            with self.subTest(spelling):
                inventory = json.loads(original)
                inventory['sites'][0]['spelling'] = spelling
                path.write_text(json.dumps(inventory))
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn('field spelling is empty or a placeholder', proc.stdout)

    def test_missing_site_inventory_exits_2_instead_of_rebaselining(self):
        self.build(self.clean_entries())
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        (self.root / SITE_INVENTORY_REL).unlink()
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('site inventory', proc.stdout)
        self.assertIn('missing', proc.stdout)

    def test_removing_a_reviewed_site_record_does_not_disable_matching(self):
        self.build(self.clean_entries())
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        path = self.root / SITE_INVENTORY_REL
        inventory = json.loads(path.read_text())
        inventory['sites'].pop()
        path.write_text(json.dumps(inventory))
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn('NEW ', proc.stdout)
        self.assertIn('entry E1', proc.stdout)
        self.assertEqual(counts(proc)['PASS(bounded)'], 0)

    def test_duplicate_reviewed_identity_is_not_silently_collapsed(self):
        self.build(self.clean_entries())
        path = self.root / SITE_INVENTORY_REL
        inventory = json.loads(path.read_text())
        inventory['sites'].append(inventory['sites'][0])
        path.write_text(json.dumps(inventory))
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('duplicate identity', proc.stdout)

    def test_missing_manifest_exits_2(self):
        self.build(self.clean_entries())
        (self.root / MANIFEST_REL).unlink()
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('manifest', proc.stdout + proc.stderr)

    def test_empty_manifest_exits_2(self):
        self.build(self.clean_entries())
        (self.root / MANIFEST_REL).write_text('')
        self.assertEqual(self.gate().returncode, 2)

    def test_manifest_with_no_entries_over_real_sites_is_a_finding_not_a_pass(self):
        self.build([])
        proc = self.gate()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn('no manifest entry', proc.stdout)

    def test_file_set_resolving_to_zero_files_exits_2_never_pass(self):
        self.build(self.clean_entries(), files=('src/**/*.nothing',))
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('zero files', proc.stdout + proc.stderr)
        self.assertNotIn('result: CLEAN', proc.stdout)

    def test_manifest_scope_narrower_than_the_engine_tree_exits_2(self):
        self.build(self.clean_entries(), files=('src/**/*.cpp',))  # omits include/pineforge/fixed.hpp
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('omits', proc.stdout)
        self.assertIn('include/pineforge/fixed.hpp', proc.stdout)

    def test_declared_generated_files_resolving_to_zero_exits_2(self):
        self.build(self.clean_entries(), generated=('tutorial/**/generated*.cpp',))
        self.assertEqual(self.gate().returncode, 2)

    def test_files_with_zero_sites_exits_2_never_pass(self):
        self.build(self.clean_entries(), loop='int add(int a, int b) { return a + b; }\n')
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('zero sites', proc.stdout + proc.stderr)

    def test_unsupported_codegen_tree_declaration_exits_2(self):
        self.build(self.clean_entries())
        path = self.root / MANIFEST_REL
        path.write_text(path.read_text().replace('tree: absent', 'tree: "../codegen"'))
        self.assertEqual(self.gate().returncode, 2)

    def test_absent_codegen_tree_requires_a_declared_template_list(self):
        self.build(self.clean_entries(), templates=False)
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('template', proc.stdout + proc.stderr)

    def test_unknown_manifest_key_exits_2(self):
        self.build(self.clean_entries())
        path = self.root / MANIFEST_REL
        path.write_text(path.read_text() + 'surprise: 1\n')
        self.assertEqual(self.gate().returncode, 2)

    def test_scanner_canary_covers_every_site_class(self):
        proc = run_gate(self.root, '--self-check')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'canary: \d+/\d+ site classes matched')


class RealTree(unittest.TestCase):
    """The gate on this repository's own tree, and on mutated copies of it."""

    @classmethod
    def setUpClass(cls):
        cls.manifest_present = (ROOT / MANIFEST_REL).is_file()

    def real_snapshot(self):
        out = {}
        for rel in REAL_TREE_DIRS:
            out.update({f'{rel}/{k}': v for k, v in digest(ROOT / rel).items()})
        for rel in REAL_TREE_FILES:
            out[rel] = hashlib.sha256((ROOT / rel).read_bytes()).hexdigest()
        return out

    def copy_tree(self, tmp: Path) -> Path:
        dest = tmp / 'tree'
        for rel in REAL_TREE_DIRS:
            shutil.copytree(ROOT / rel, dest / rel)
        for rel in REAL_TREE_FILES:
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, dest / rel)
        return dest

    def test_real_tree_scans_files_and_sites_and_is_not_a_pass_by_construction(self):
        before = self.real_snapshot()
        proc = run_gate(ROOT)
        self.assertEqual(before, self.real_snapshot(), 'the gate modified the real tree')
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertGreater(c['files_scanned'], 0)
        self.assertGreater(c['sites_found'], 0)
        self.assertGreater(c['entries_matched'], 0)
        self.assertEqual(c['findings'], 0)
        self.assertEqual(c['PASS(bounded)'], 0)
        # Exact manifest union: 1,344 original unreviewed groups, ten time-format
        # groups and two map groups, and 46 groups of the integrated tree (50
        # emitted, 4 stale entries removed), one group of the local list of
        # materialize_relative_exits and three groups of
        # SeriesPercentileNearestRank (its ring, the growth of the ring and its
        # window), less the two whole-state copies of the bounded drawing arena
        # removed when it began writing in place. expectation corrected:
        # 1588 -> 1589, because the scalar
        # field of the checked live-object limit class adds one key with two
        # assignment sites; that key stays OPEN.
        # Main adds five groups of the finished
        # trade text companion (the source's text accessor and the writer's
        # transfer buffer, transfer bytes, carried bytes and integer text
        # temporaries); all 184 pending-flat-memory groups remain.
        # expectation corrected: 1593 -> 1594, because main's 53 groups and the
        # retained scalar group form 54 distinct groups after the merge.
        # The selected-capacity Source stage adds three groups (its chunk
        # buffer, its record window and its stored message): 54 + 3 = 57.
        # The run-error bar prefix function adds two groups (its declaration in
        # the installed header and in its source): 57 + 2 = 59.
        # The setter refusal messages (three number-to-text temporaries in the
        # engine header) and the timezone refusal (its message string and that
        # string's temporary) add three groups: 59 + 3 = 62.
        # The timezone database lookup refusal (its message string in the
        # native calendar) adds one group: 62 + 1 = 63.
        # The refusal of a script timeframe the input timeframe cannot build
        # adds three groups (its failure arguments, its rule text and its
        # message text): 63 + 3 = 66.
        # The per-site run warning of a requested timeframe adds ten groups (the
        # native execution consumer's warning records and their strings, and the
        # host's argument text): 66 + 10 = 76.
        # expectation corrected: 1616 -> 1618, because U1854/U1855 add two
        # unreviewed SequenceIndex allocation groups. Both remain OPEN.
        # expectation corrected: 1618 -> 1616. #809 removes six stale
        # deque groups and adds four current groups; every new group is OPEN.
        # The combined tree adds two #858 cache-id site identities inside
        # existing U0307/U0871 OPEN groups. No new group: the union stays1616.
        # expectation corrected: 1616 -> 1618, because U1860/U1861 add two
        # unreviewed groups for the warning arrays the native execution
        # consumer renders after a run began (the list's declaration and its
        # one append). Both remain OPEN.
        # expectation corrected: 1618 -> 1626, because U1862-U1869 add eight
        # unreviewed groups: #871's request caller catalog, chart caller table
        # and request delivery stack (each a declaration and its insertions)
        # and the caller key's text, and #810's per-origin completed units of
        # one percentage exit's sizing query. All remain OPEN.
        # expectation corrected: 1626 -> 1625, because moving the #810/#716
        # partial-exit reservation rework to the follow-up PR retires U1866.
        # expectation corrected: 1625 -> 1629, because U1870-U1873 add four
        # unreviewed groups: the native calendar's kept session days of one
        # calendar set, the per-thread list of those sets and each set's zone
        # root, and the insertions of the request caller catalog's run map.
        # U1862 follows the catalog's storage into that run map. All remain OPEN.
        # expectation corrected: 1629 -> 1633, because U1874-U1877 add four
        # unreviewed groups in src/source/pine_adapter.cpp: the exits standing
        # on the book for an opening that fills, the market adds a cover fill
        # leaves without an order, and the comment and OCA name text those
        # lists and the file's order copies carry. All remain OPEN.
        # expectation corrected: 1633 -> 1636, because U1878-U1880 add three
        # unreviewed groups in src/live_report.cpp: the points a whole-report
        # read puts back in front of the engine's curve, the curve scratch
        # writer's allocation and its append. All remain OPEN.
        # KC and KCW constructor refusals add one OPEN group (U1930).
        # The checked adapters use the existing OPEN group U0439.
        # expectation corrected: 1636 -> 1637, because U1910 adds one unreviewed
        # group (#718): the input timeframe text each request.security evaluator
        # keeps for the fixed-period request clock. Five site identities also
        # enter existing OPEN groups without adding a group: the four insertions
        # of that clock's projection (U0652-U0655) and one parsed timeframe that
        # a call holds in a local named tf (U1048). All remain OPEN.
        # expectation corrected: 1638 -> 1639, because U1931 adds one unreviewed
        # group (#810) in src/source/pine_adapter.cpp: the local list of queued
        # exit legs that one full strategy.exit call gives a queue slot (its
        # declaration and its one insertion). It remains OPEN.
        # expectation corrected: 1639 -> 1641, because the timezone guard adds
        # two OPEN groups, U1900 for the copy from TZ at src/timezone.cpp:185
        # and U1901 for its owner at src/timezone.hpp:46. The copy follows the
        # input length; neither group is bounded here.
        # expectation corrected: 1641 -> 1644, because the exact window sum change adds
        # three OPEN groups, the Unix second time format change adds two, and the
        # pre-bar error prefix change retires two (U1102 and U1105); none is bounded.
        # expectation corrected: 1644 -> 1646, because the input rejection
        # string (U1937) and the account currency capabilities vector (U1954)
        # add one unreviewed group each. Both remain OPEN.
        # expectation corrected: 1646 -> 1647, because materialize_relative_exits
        # returns its list of consumed definitions by value (U1955). It remains OPEN.
        # expectation corrected: 1647 -> 1653, because eight groups are added and
        # two retired: the facts of an opening's exits (U1959, U1962) replace the
        # shadowed-opening set (U0268, U0811); a named tick exit stores its texts in
        # one string (U1957, U1958); three lists of one call (U1960, U1961, U1963)
        # and one list of a state fold (U1964) are new. All remain OPEN.
        # expectation corrected: 1653 -> 1654, because U1965 adds one OPEN group (1653 + 1).
        # expectation corrected: 1654 -> 1656, because U1966 and U1967 add two
        # unreviewed groups in include/pineforge/source/pine_ta_length.hpp: the
        # ring of the positional history of the Pine ta.mom / ta.roc objects
        # (PositionalLag's lag_slots_) and the larger vector one growth step
        # builds (grown). Two site identities also enter the existing OPEN group
        # U1108 without adding a group: the whole-state restores of
        # PineDmiReference::recompute and PineSar::recompute. All remain OPEN.
        # expectation corrected: 1656 -> 1658, because U1968 and U1969 add two
        # unreviewed groups in src/source/pine_host_reads.hpp: the allocation
        # of the state that keeps the N-day partitions of timeframe.change for
        # one run (TimeframeChangeState) and that state's rows, one for each
        # N-day timeframe the script names. Both remain OPEN.
        # expectation corrected: 1658 -> 1671, because U1970-U1982 add thirteen
        # unreviewed groups for the series-length classes of the Pine source
        # layer: three in include/pineforge/source/pine_ta_series_ma.hpp, two in
        # pine_ta_series_osc.hpp and eight in pine_ta_series_stats.hpp (the
        # fixed-length objects a call builds, the appends to what a site records
        # and the larger storage moved in for it, and the texts of the length
        # refusals). PineRci adds no group: its two counters are no longer named
        # values_ and saved_values_, the names of two container members of
        # include/pineforge/ta.hpp, so the index lists none of their four
        # integer statements. All remain OPEN.
        # expectation corrected: 1671 -> 1670, because the trading-day helper
        # no longer owns eff_tz (U0759). The wall-minute cache keeps its tz
        # group (U1184); moving session sites adds no group.
        expected_open = 1356 + 76 + 184 + 2 - 6 + 4 + 2 + 8 - 1 + 4 + 4 + 3 + 1 + 1 + 1 + 2 + 3 + 2 + 1 + 6 + 1 + 2 + 2 + 13 - 1
        self.assertEqual(c['OPEN'], expected_open)
        # Every matched entry is exactly one of PASS(bounded) / OPEN.
        self.assertEqual(c['PASS(bounded)'] + c['OPEN'], c['entries_matched'])
        strict = run_gate(ROOT, '--strict')
        self.assertEqual(strict.returncode, 1 if c['OPEN'] else 0, strict.stdout)

    def test_real_tree_note_citations_hold_or_are_listed_unchecked(self):
        proc = run_gate(ROOT)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual(c.get('cited_fails'), 0, proc.stdout)
        self.assertGreater(c.get('citations', 0), 0, proc.stdout)
        self.assertGreater(c.get('cited_holds', 0), 0, proc.stdout)
        # Every citation is exactly one of holding, failing or UNCHECKED, and
        # each UNCHECKED one is printed with the entry that cites it.
        self.assertEqual(c['cited_holds'] + c['cited_fails'] + c['cited_unchecked'],
                         c['citations'])
        listed = [line for line in proc.stdout.splitlines()
                  if line.startswith('cr150_source_gate: UNCHECKED ')]
        # The comments of the manifest list theirs in the same way, with the
        # manifest line in place of an entry, and count them apart.
        of_notes = [line for line in listed if ' UNCHECKED entry ' in line]
        self.assertEqual(len(of_notes), c['cited_unchecked'])
        self.assertEqual(len(listed), c['cited_unchecked'] + c.get('comment_unchecked', 0))
        for line in of_notes:
            self.assertRegex(line, r'UNCHECKED entry U\d{4}: note (cites|has) ')
        # The manifest pins how many are UNCHECKED, exactly, and the gate
        # prints the number it read beside the count.
        self.assertIn('cited_unchecked_ceiling', c, proc.stdout)
        self.assertEqual(c['cited_unchecked'], c['cited_unchecked_ceiling'])

    def test_real_tree_pin_above_the_unchecked_count_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            c = counts(control)
            self.assertIn('cited_unchecked_ceiling', c, control.stdout)
            self.assertEqual(c['cited_unchecked'], c['cited_unchecked_ceiling'], control.stdout)
            manifest_path = tree / MANIFEST_REL
            text = manifest_path.read_text()
            # One more than the notes have: the room a lowered count leaves
            # when the pin does not follow it down.
            old = f'\n  cited_unchecked: {c["cited_unchecked_ceiling"]}\n'
            self.assertEqual(text.count(old), 1, 'the manifest must pin cited_unchecked once')
            manifest_path.write_text(text.replace(
                old, f'\n  cited_unchecked: {c["cited_unchecked_ceiling"] + 1}\n'))
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            after = counts(proc)
            self.assertEqual(after['cited_unchecked'], c['cited_unchecked'])
            self.assertEqual(after['cited_unchecked_ceiling'], c['cited_unchecked_ceiling'] + 1)
            self.assertEqual(after.get('cited_fails'), 0, proc.stdout)
            self.assertIn(f'FINDING {MANIFEST_REL}:0: UNCHECKED note citations '
                          f'{c["cited_unchecked"]} are below the manifest ceiling '
                          f'{c["cited_unchecked_ceiling"] + 1}: lower ceilings.cited_unchecked '
                          f'to {c["cited_unchecked"]} in the same change', proc.stdout)
            self.assertEqual(after['findings'], 1, proc.stdout)

    def test_real_tree_block_range_holds_and_one_line_short_of_its_end_does_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            c = counts(control)
            # The note of U1545 names nothing in its last sentence and cites
            # the function its entry declares, from the declaration (a site of
            # the entry) to the closing brace. That range holds.
            cited = 'include/pineforge/detail/scratch_errors.hpp:47,118-126'
            self.assertNotIn('note cites include/pineforge/detail/scratch_errors.hpp:118-',
                             control.stdout)
            manifest_path = tree / MANIFEST_REL
            lines = manifest_path.read_text().splitlines()
            index = next(i for i, l in enumerate(lines) if l.startswith('  - {id: "U1545",'))
            self.assertEqual(lines[index].count(cited), 1)
            # One line short of the brace is the function's return statement:
            # not a site, and not where what line 118 begins ends.
            lines[index] = lines[index].replace(cited, cited[:-1] + '5')
            manifest_path.write_text('\n'.join(lines) + '\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            after = counts(proc)
            self.assertEqual(after['cited_holds'], c['cited_holds'] - 1)
            self.assertEqual(after['cited_unchecked'], c['cited_unchecked'] + 1)
            self.assertEqual(after.get('cited_fails'), 0, proc.stdout)
            self.assertRegex(proc.stdout,
                             r'UNCHECKED entry U1545: note cites '
                             r'include/pineforge/detail/scratch_errors\.hpp:118-125: '
                             r'include/pineforge/detail/scratch_errors\.hpp:118-125 begins '
                             r'".*scratch_object_limit_message.*" and ends "return message;": '
                             r'.*line 125 does not: not a site of the entry')
            self.assertIn(f'FINDING {MANIFEST_REL}:0: UNCHECKED note citations '
                          f'{after["cited_unchecked"]} exceed the manifest ceiling '
                          f'{c["cited_unchecked_ceiling"]}: ', proc.stdout)
            self.assertEqual(after['findings'], 1, proc.stdout)

    def test_real_tree_new_unchecked_citation_is_refused_by_the_ceiling(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            c = counts(control)
            self.assertIn('cited_unchecked_ceiling', c, control.stdout)
            room = c['cited_unchecked_ceiling'] - c['cited_unchecked']
            self.assertGreaterEqual(room, 0, control.stdout)
            manifest_path = tree / MANIFEST_REL
            lines = manifest_path.read_text().splitlines()
            index = next(i for i, l in enumerate(lines) if l.startswith('  - {id: "U0439",'))
            # A sentence of prose citing line 1 of that header, which holds no
            # site of the entry: one more UNCHECKED citation for each copy.
            # One more than the ceiling leaves room for is refused.
            old = 'one-object optional bound. OPEN.'
            self.assertEqual(lines[index].count(old), 1)
            lines[index] = lines[index].replace(old, old + ' See :1.' * (room + 1))
            manifest_path.write_text('\n'.join(lines) + '\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            after = counts(proc)
            self.assertEqual(after['cited_unchecked'], c['cited_unchecked'] + room + 1)
            self.assertEqual(after['cited_unchecked'], c['cited_unchecked_ceiling'] + 1)
            self.assertEqual(after.get('cited_fails'), 0, proc.stdout)
            self.assertIn(f'FINDING {MANIFEST_REL}:0: UNCHECKED note citations '
                          f'{after["cited_unchecked"]} exceed the manifest ceiling '
                          f'{c["cited_unchecked_ceiling"]}: ', proc.stdout)
            self.assertEqual(after['findings'], 1, proc.stdout)

    def test_real_tree_note_citing_a_wrong_line_is_caught(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            self.assertEqual(counts(control).get('cited_fails'), 0, control.stdout)
            manifest_path = tree / MANIFEST_REL
            lines = manifest_path.read_text().splitlines()
            index = next(i for i, l in enumerate(lines) if l.startswith('  - {id: "U0439",'))
            # The note names FirstCallBound and cites the line of its one
            # emplace. Line 1 of that header holds neither.
            self.assertEqual(lines[index].count('in an optional at :102.'), 1)
            lines[index] = lines[index].replace('in an optional at :102.',
                                                'in an optional at :1.')
            manifest_path.write_text('\n'.join(lines) + '\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertRegex(proc.stdout,
                             r'FINDING include/pineforge/source/pine_ta_length\.hpp:1: '
                             r'entry U0439: note cites :1: '
                             r'include/pineforge/source/pine_ta_length\.hpp:1 holds ".*": '
                             r"not a site of the entry \('object_'\) and none of the constructs "
                             r'the note names in that sentence \(FirstCallBound\)')
            self.assertEqual(counts(proc).get('cited_fails'), 1, proc.stdout)
            self.assertEqual(counts(proc)['cited_holds'], counts(control)['cited_holds'] - 1)

    def test_real_tree_line_fields_are_each_entrys_first_live_site(self):
        with tempfile.TemporaryDirectory() as out:
            result = Path(out) / 'gate.json'
            proc = run_gate(ROOT, '--json', str(result))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            c = counts(proc)
            self.assertGreater(c.get('line_fields', 0), 0, proc.stdout)
            # Every field the manifest carries is compared and holds.
            self.assertEqual((c.get('line_holds'), c.get('line_differs'),
                              c.get('line_not_compared')), (c['line_fields'], 0, 0), proc.stdout)
            # The same statement read from the gate's own result file: a
            # field is the first of the lines the scan found for its entry.
            rows = [row for row in json.loads(result.read_text())['entries'] if 'line' in row]
            self.assertEqual(len(rows), c['line_fields'])
            self.assertEqual([row['id'] for row in rows
                              if not row['live_lines'] or row['line'] != row['live_lines'][0]], [])

    def test_real_tree_line_field_one_off_is_caught(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            c = counts(control)
            self.assertEqual(c.get('line_differs'), 0, control.stdout)
            manifest_path = tree / MANIFEST_REL
            lines = manifest_path.read_text().splitlines()
            index = next(i for i, l in enumerate(lines) if l.startswith('  - {id: "U1930",'))
            field = re.search(r', line: (\d+)\}$', lines[index])
            self.assertIsNotNone(field, 'U1930 must end with its line field')
            live = int(field.group(1))
            # One line after the entry's first live site.
            lines[index] = lines[index][:field.start()] + f', line: {live + 1}}}'
            manifest_path.write_text('\n'.join(lines) + '\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn(f'FINDING src/ta_volatility_trend.cpp:{live}: entry U1930: line: '
                          f"{live + 1} is not the first live site of the entry ('<temp:string>'), "
                          f'which is line {live}: ', proc.stdout)
            after = counts(proc)
            self.assertEqual((after['line_fields'], after['line_holds'], after['line_differs']),
                             (c['line_fields'], c['line_holds'] - 1, 1), proc.stdout)
            self.assertEqual(after['findings'], 1, proc.stdout)

    def test_real_tree_comment_citations_hold_or_are_listed_unchecked(self):
        proc = run_gate(ROOT)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual(c.get('comment_fails'), 0, proc.stdout)
        self.assertGreater(c.get('comment_citations', 0), 0, proc.stdout)
        self.assertGreater(c.get('comment_holds', 0), 0, proc.stdout)
        # Every comment citation is exactly one of holding, failing or
        # UNCHECKED, and each UNCHECKED one is printed with the manifest line
        # that cites it.
        self.assertEqual(c['comment_holds'] + c['comment_fails'] + c['comment_unchecked'],
                         c['comment_citations'])
        listed = [line for line in proc.stdout.splitlines()
                  if line.startswith('cr150_source_gate: UNCHECKED manifest line ')]
        self.assertEqual(len(listed), c['comment_unchecked'])
        for line in listed:
            self.assertRegex(line, r'UNCHECKED manifest line \d+: comment (cites|has) ')
        # The manifest pins how many are UNCHECKED, exactly, apart from the
        # pin of the note citations.
        self.assertIn('comment_unchecked_ceiling', c, proc.stdout)
        self.assertEqual(c['comment_unchecked'], c['comment_unchecked_ceiling'])

    def test_real_tree_comment_citing_a_wrong_line_is_caught(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            self.assertEqual(counts(control).get('comment_fails'), 0, control.stdout)
            manifest_path = tree / MANIFEST_REL
            # The comment names evaluator_input_tf and cites the line of that
            # member in the engine header. Line 1 of the header does not hold it.
            text, changed = re.subn(
                r'evaluator keeps \(include/pineforge/engine\.hpp:\d+\)',
                'evaluator keeps (include/pineforge/engine.hpp:1)', manifest_path.read_text())
            self.assertEqual(changed, 1, 'the comment must cite the engine header once')
            manifest_path.write_text(text)
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertRegex(proc.stdout,
                             r'FINDING include/pineforge/engine\.hpp:1: manifest line \d+: '
                             r'comment cites include/pineforge/engine\.hpp:1: '
                             r'include/pineforge/engine\.hpp:1 holds ".*": a comment records no '
                             r'site and none of the constructs the comment names in that '
                             r'sentence \(evaluator_input_tf\)')
            after = counts(proc)
            self.assertEqual(after.get('comment_fails'), 1, proc.stdout)
            self.assertEqual(after['comment_holds'], counts(control)['comment_holds'] - 1)
            self.assertEqual(after['findings'], 1, proc.stdout)

    def test_real_tree_mutations_are_caught(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            self.assertEqual(run_gate(tree).returncode, 0, 'unmutated copy must be clean')
            target = tree / 'src/engine_run.cpp'
            original = target.read_text()
            # 1. a new growable on a per-bar file, no entry
            target.write_text(original + '\nvoid cr150_mutant(std::vector<double>& cr150_mutant_rows) { cr150_mutant_rows.push_back(1.0); }\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn('src/engine_run.cpp', proc.stdout)
            self.assertIn('no manifest entry', proc.stdout)
            target.write_text(original)
            self.assertEqual(run_gate(tree).returncode, 0)
            # The read's collision: same spelling/op classes, new scope/type.
            target.write_text(original + '''
void cr150_extra(int n) {
    std::vector<int> next_rates;
    next_rates.resize(n);
}
''')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertRegex(proc.stdout, r'NEW decl site on \'next_rates\'.*entry U0549')
            target.write_text(original)
            self.assertEqual(run_gate(tree).returncode, 0)
            # 2. a real entry deleted: its sites become unlisted
            manifest_path = tree / MANIFEST_REL
            text = manifest_path.read_text()
            lines = text.splitlines()
            index = next(i for i, l in enumerate(lines)
                         if l.startswith('  - {') and 'file: "src/engine_run.cpp"' in l)
            del lines[index]
            manifest_path.write_text('\n'.join(lines) + '\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            manifest_path.write_text(text)
            # 3. file set emptied
            manifest_path.write_text(re.sub(r'(?m)^  files:\n(    - .*\n)+',
                                            '  files:\n    - "src/**/*.nothing"\n', text))
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            manifest_path.write_text(text)
        self.assertTrue(self.manifest_present)

    def test_real_tree_bad_bound_changes_only_a_passing_controls_bound(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(Path(tmp))
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            baseline_open = counts(proc)['OPEN']
            source_file = 'src/cr150_bound_control.cpp'
            fixed_file = 'include/pineforge/cr150_bound_control.hpp'
            loop = LOOP_CPP.replace('pineforge/fixed.hpp', 'pineforge/cr150_bound_control.hpp')
            loop = loop.replace('slots_', 'fixture_slots').replace('kRingSlots', 'kFixtureLimit')
            loop = loop.replace('on_bar', 'fixture_push')
            fixed = FIXED_HPP.replace('kRingSlots', 'kFixtureLimit')
            write_tree(tree, {source_file: loop, fixed_file: fixed})
            manifest_path = tree / MANIFEST_REL
            text = manifest_path.read_text()
            marker = 'fixed_inputs:\n  []\nentries:\n'
            self.assertEqual(text.count(marker), 1, 'expected the real OPEN-only manifest')
            fixed_row = flow(name='kFixtureLimit', value=64, definition=fixed_file,
                             fixed_before_bar0=True)
            eid = 'CR150_BOUND_CONTROL'
            control_entry = entry(eid, file=source_file, symbol='fixture_slots', state='bounded',
                                  bound='kFixtureLimit', bound_inputs=['kFixtureLimit'])
            control_text = text.replace(marker, f'fixed_inputs:\n{fixed_row}\nentries:\n')
            control_text += control_entry + '\n'
            manifest_path.write_text(control_text)
            # Extend only with the deliberate fixture's sites. Existing source
            # inventory is kept intact; the copied real tree is not rebaselined.
            inventory_path = tree / SITE_INVENTORY_REL
            inventory = json.loads(inventory_path.read_text())
            fixture_inventory = json.loads(inventory_text({source_file: loop, fixed_file: fixed}))
            inventory['sites'].extend(fixture_inventory['sites'])
            inventory_path.write_text(json.dumps(inventory, indent=1) + '\n')
            control = run_gate(tree)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            self.assertEqual(counts(control)['PASS(bounded)'], 1)
            self.assertEqual(counts(control)['OPEN'], baseline_open)
            before = digest(tree)
            # Only the bound expression changes. Phase, state, bound_inputs,
            # source, fixed-input declaration and inventory remain the control's.
            mutated_entry = control_entry.replace('bound: "kFixtureLimit"',
                                                   'bound: "kFixtureLimit + bars"')
            self.assertNotEqual(mutated_entry, control_entry)
            manifest_path.write_text(control_text.replace(control_entry, mutated_entry, 1))
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertRegex(proc.stdout, rf'FINDING {source_file}:\d+: bounded entry {eid}: '
                             rf'entry {eid}: bound .*names \'bars\': forbidden growth axis \(bar\)')
            self.assertEqual(counts(proc)['PASS(bounded)'], 0)
            self.assertEqual(counts(proc)['OPEN'], baseline_open)
            after = digest(tree)
            self.assertEqual({p: h for p, h in before.items() if p != MANIFEST_REL},
                             {p: h for p, h in after.items() if p != MANIFEST_REL})

    def test_preflight_runs_the_gate_and_its_tests_unconditionally(self):
        sys.path.insert(0, str(SCRIPTS))
        from ci_preflight import check_commands, run_checks
        plan = {entry[0]: entry for entry in check_commands(Path('/nonexistent-pineforge-src'))}
        for name, script in (('source-guard-cr150-source-bounds', 'cr150_source_gate.py'),
                             ('cr150-source-gate-tests', 'test_cr150_source_gate.py')):
            self.assertTrue(name in plan, f'{name} is not a preflight stage')
            self.assertEqual(len(plan[name]), 2, f'{name} must be a binding stage')
            self.assertTrue(any(str(part).endswith(script) for part in plan[name][1]), name)
        self.assertNotIn('--strict', plan['source-guard-cr150-source-bounds'][1])
        # A failing gate fails the preflight run (fail closed, no advisory downgrade).
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / 'bad'
            write_tree(bad, {'src/loop.cpp': LOOP_CPP, 'include/pineforge/fixed.hpp': FIXED_HPP,
                             'scripts/regen_corpus_cpp.sh': PIN_SH,
                             SITE_INVENTORY_REL: inventory_text({
                                 'src/loop.cpp': LOOP_CPP,
                                 'include/pineforge/fixed.hpp': FIXED_HPP}),
                             MANIFEST_REL: manifest([])})
            argv = [sys.executable, '-B', str(GATE), '--root', str(bad)]
            with contextlib.redirect_stdout(io.StringIO()) as printed:
                code = run_checks([('source-guard-cr150-source-bounds', argv)], Path(tmp) / 'out')
            self.assertEqual(code, 1)
            self.assertIn('FINDING', printed.getvalue())


if __name__ == '__main__':
    unittest.main()
