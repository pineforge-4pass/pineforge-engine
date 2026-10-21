#!/usr/bin/env python3
"""Mutation self-tests for scripts/cr150_source_gate.py (CR-150 / C4).

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
PIN = '52101f9b7b540fece4493ddc7495451b6d85fc07'
MANIFEST_REL = 'ci/cr150-source-bounds.yaml'
# What the gate scans on the real tree, plus the files that bind it.
REAL_TREE_DIRS = ('src', 'include', 'tutorial', 'ci')
REAL_TREE_FILES = ('scripts/regen_corpus_cpp.sh',)

LOOP_CPP = '''\
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
             pin=PIN, fixed=True, templates=True, extra_fixed='') -> str:
    lines = ['schema: cr150-source-bounds/v1', 'codegen:', f'  pin: {q(pin)}',
             '  pin_source: scripts/regen_corpus_cpp.sh', '  tree: absent',
             '  emitter_templates:']
    lines.append('  ' + flow(id='T1', emits='std::vector<T> arrays', owner='generated',
                           phase='unclassified', state='unreviewed') if templates else '    []')
    lines += ['scan:', '  files:'] + [f'    - {q(g)}' for g in files]
    lines += ['  generated_files:'] + ([f'    - {q(g)}' for g in generated] or ['    []'])
    lines += ['ceilings:', f'  unreviewed: {ceilings[0]}',
              f'  pending-flat-memory: {ceilings[1]}',
              'owners:', flow(name='engine-core', globs=['src/**', 'include/**']),
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

    def gate(self, *extra):
        before = digest(self.root)
        proc = run_gate(self.root, *extra)
        self.assertEqual(before, digest(self.root), 'the gate changed the tree it scans')
        return proc

    def clean_entries(self):
        return [entry('E1', state='bounded')]


class PositiveControls(Tree):
    def test_fixed_constant_capacity_exits_0_and_counts_as_pass(self):
        self.build(self.clean_entries())
        proc = self.gate()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        c = counts(proc)
        self.assertEqual((c['files_scanned'], c['findings'], c['OPEN']), (2, 0, 0))  # loop.cpp + fixed.hpp
        self.assertGreaterEqual(c['sites_found'], 3)  # decl + reserve + push_back
        self.assertEqual(c['PASS(bounded)'], 1)

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
    def test_bound_naming_a_growth_axis_exits_2(self):
        bad = ['bars', 'n_bars', 'trades', 'trade_count', 'entries', 'decisions',
               'history_len', 'bar_index + 1', 'slots_', 'slots_.size()', 'kRingSlots + bars']
        for expr in bad:
            with self.subTest(expr):
                self.build([entry('E1', state='bounded', bound=expr,
                                  bound_inputs=['kRingSlots'])])
                proc = self.gate()
                self.assertEqual(proc.returncode, 2, f'{expr}: {proc.stdout}{proc.stderr}')
                self.assertRegex(proc.stdout + proc.stderr, r'E1')

    def test_bound_input_outside_the_fixed_input_list_exits_2(self):
        self.build([entry('E1', state='bounded', bound='kOther', bound_inputs=['kOther'])])
        proc = self.gate()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn('fixed input', proc.stdout + proc.stderr)

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


class FailClosed(Tree):
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
        # Every matched entry is exactly one of PASS(bounded) / OPEN.
        self.assertEqual(c['PASS(bounded)'] + c['OPEN'], c['entries_matched'])
        strict = run_gate(ROOT, '--strict')
        self.assertEqual(strict.returncode, 1 if c['OPEN'] else 0, strict.stdout)

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
            # 4. an OPEN entry rewritten to claim a bound that names bars
            real = text.splitlines()
            at = next(i for i, l in enumerate(real) if l.startswith('  - {')
                      and ('state: "pending-flat-memory"' in l or 'state: "unreviewed"' in l))
            real[at] = re.sub(r'state: "(pending-flat-memory|unreviewed)"',
                              'state: "bounded", bound: "bars", bound_inputs: ["bars"], '
                              'lifetime: "x", rollback: "x", hash_reader: "x", tests: "x"', real[at])
            manifest_path.write_text('\n'.join(real) + '\n')
            proc = run_gate(tree)
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertTrue(self.manifest_present)

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
                             MANIFEST_REL: manifest([])})
            argv = [sys.executable, '-B', str(GATE), '--root', str(bad)]
            with contextlib.redirect_stdout(io.StringIO()) as printed:
                code = run_checks([('source-guard-cr150-source-bounds', argv)], Path(tmp) / 'out')
            self.assertEqual(code, 1)
            self.assertIn('FINDING', printed.getvalue())


if __name__ == '__main__':
    unittest.main()
