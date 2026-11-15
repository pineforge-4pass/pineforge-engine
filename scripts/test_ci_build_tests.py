#!/usr/bin/env python3
"""Compile controls for the actual hosted Release build decision; stdlib only."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import ci_preflight

ROOT = Path(__file__).resolve().parents[1]
BASE = '5cf08f5c16d6477a91dea8a18c331cf4a6c57496'
FIXTURE = ROOT / 'scripts/fixtures/ci_build_tests'


def step_run(workflow, job, name):
    step = ci_preflight._step_named(ci_preflight._steps(
        ci_preflight._jobs(workflow)[job]), '      - name: ' + name)
    lines = step.splitlines()
    indexes = [index for index, line in enumerate(lines) if line == '        run: |']
    if len(indexes) != 1:
        raise AssertionError('control requires the real multiline workflow command')
    command = []
    for line in lines[indexes[0] + 1:]:
        if line.strip() and not line.startswith(' ' * 10):
            break
        command.append(line[10:])
    return '\n'.join(command) + '\n'


def mutate_workflow(workflow, job, step, before, after):
    """Change exactly one occurrence in its explicit owning job and step."""
    headers = list(ci_preflight._JOB_HEADER.finditer(workflow))
    selected = [index for index, header in enumerate(headers) if header[1] == job]
    if len(selected) != 1:
        raise AssertionError('mutation requires one owning job: ' + job)
    index, = selected
    start = headers[index].end()
    end = headers[index + 1].start() if index + 1 < len(headers) else len(workflow)
    body = workflow[start:end]
    scope = body
    if step is not None:
        matches = [block for block in ci_preflight._steps(body)
                   if ci_preflight._step_values(block, 'name') == [step]]
        if len(matches) != 1 or body.count(matches[0]) != 1:
            raise AssertionError('mutation requires one owning step: ' + step)
        scope, = matches
    if not before or scope.count(before) != 1 or before == after:
        raise AssertionError('mutation requires one changing occurrence in ' + job)
    offset = body.index(scope) if step is not None else 0
    offset += scope.index(before)
    start += offset
    return workflow[:start] + after + workflow[start + len(before):]


class WorkflowControl(unittest.TestCase):
    def test_broken_registered_header_changes_the_workflow_verdict(self):
        # The baseline is Git's exact old workflow, not a reimplementation of
        # smoke selection. Fetch-depth 0 in the hosted job retains this object.
        old = subprocess.run(
            ['git', 'show', BASE + ':.github/workflows/ci-post.yml'],
            cwd=ROOT, check=True, capture_output=True, text=True).stdout
        new = (ROOT / '.github/workflows/gke-pr.yml').read_text()
        old_run = step_run(old, 'smoke', 'Verify ${{ matrix.build_type }} smoke')
        new_run = step_run(new, 'build-tests', 'Build every Release test target')
        strategy = ci_preflight._job_block(
            ci_preflight._jobs(new)['build-tests'], 'strategy')
        shard_line, = [line for line in strategy if line.strip().startswith('shard:')]
        shards = json.loads(shard_line.split(':', 1)[1])
        self.assertTrue(shards)
        self.assertEqual(shards, list(range(1, len(shards) + 1)))

        with tempfile.TemporaryDirectory(prefix='registered-build-control-') as tmp:
            source = Path(tmp)
            shutil.copytree(FIXTURE, source, dirs_exist_ok=True)
            # Keep this fixture scalable with the workflow's parallelism, without
            # admitting an empty build shard or pinning a test population total.
            with (source / 'CMakeLists.txt').open('a') as cmake:
                for shard in shards:
                    cmake.write(f'\nadd_library(control_aux_{shard} STATIC support.cpp)\n')
            scripts = source / 'scripts'
            scripts.mkdir()
            for name in ('ci_build_tests.py', 'ci_verify.py',
                         'prepare_settlement_cpp_abi_base.py'):
                shutil.copyfile(ROOT / 'scripts' / name, scripts / name)
            environment = dict(os.environ, BUILD_TYPE='Release', PYTHONDONTWRITEBYTECODE='1')

            def execute(command, label):
                result = subprocess.run(
                    ['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', command],
                    cwd=source, env=environment, capture_output=True, text=True)
                # Leave the compiler's actual first diagnostic in the hosted log.
                print(f'\nCONTROL {label}: exit {result.returncode}\n'
                      + result.stdout + result.stderr, flush=True)
                return result

            header = source / 'fixture.hpp'
            original = header.read_bytes()
            header.write_text('#pragma once\ninline int control_fixture_value() {\n'
                              '    return absent_fixture_type::value;\n}\n')
            broken = header.read_bytes()
            admitted = execute(old_run, 'old workflow, broken registered header')
            self.assertEqual(admitted.returncode, 0, admitted.stdout + admitted.stderr)
            self.assertFalse((source / 'build/bin/control_deferred').exists())
            rejected = []
            for shard in shards:
                command = new_run.replace('${{ matrix.shard }}', str(shard))
                result = execute(command, f'new workflow shard {shard}, same broken header')
                self.assertEqual(header.read_bytes(), broken)
                rejected.append(result)
            failures = [result for result in rejected if result.returncode != 0]
            self.assertTrue(failures, 'all new workflow shards admitted the broken tree')
            self.assertTrue(any('absent_fixture_type' in result.stdout + result.stderr
                                for result in failures),
                            'failure must be the injected compile error')
            merge_run = step_run(new, 'build-tests-result', 'Require the complete Release build')
            result = execute(merge_run, 'broken workflow union')
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

            # Repair the same actual input. Every workflow shard must now build,
            # and the real union decision must accept its exact inventories.
            header.write_bytes(original)
            for shard in shards:
                result = execute(new_run.replace('${{ matrix.shard }}', str(shard)),
                                 f'new workflow shard {shard}, repaired header')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = execute(merge_run, 'repaired workflow union')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class PopulationContract(unittest.TestCase):
    def test_union_refuses_each_kind_of_population_loss_or_invention(self):
        import ci_build_tests as build_tests

        plan = {
            'registeredNames': ['alias', 'row', 'script'],
            'targetMap': {'alias': ['fixture'], 'row': ['fixture'], 'script': []},
            'rowsWithoutBuildTargets': ['script'],
            'targets': {'fixture': ['bin/fixture'], 'support': ['lib/support.a']},
        }
        records = [
            {'plan': plan, 'index': 1, 'count': 2, 'builtTargets': ['fixture'],
             'outputs': {'bin/fixture': 'a' * 64}},
            {'plan': plan, 'index': 2, 'count': 2, 'builtTargets': ['support'],
             'outputs': {'lib/support.a': 'b' * 64}},
        ]
        build_tests.check_union(records, 2)
        mutations = [
            records[:-1],
            records + [records[0]],
            [dict(records[0], builtTargets=[]), records[1]],
            [dict(records[0], builtTargets=['fixture', 'invented']), records[1]],
            [dict(records[0], builtTargets=['fixture', 'fixture']), records[1]],
            [dict(records[0], outputs={}), records[1]],
            [dict(records[0], outputs={'bin/fixture': 'unreadable'}), records[1]],
            [dict(records[0], count=3), records[1]],
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, TypeError)):
                build_tests.check_union(mutation, 2)
        for change in ('missing', 'extra', 'duplicate', 'unmapped', 'fake-built-script'):
            mutation = copy.deepcopy(records)
            changed = mutation[0]['plan']
            if change == 'missing':
                del changed['targetMap']['row']
            elif change == 'extra':
                changed['targetMap']['extra'] = ['fixture']
            elif change == 'duplicate':
                changed['registeredNames'].append('row')
            elif change == 'unmapped':
                changed['targetMap']['row'] = ['no_producer']
            else:
                changed['rowsWithoutBuildTargets'] = []
            with self.subTest(change=change), self.assertRaises(ValueError):
                build_tests.check_union(mutation, 2)

    def test_discovery_rejects_changed_or_unreadable_rows(self):
        import ci_build_tests as build_tests

        raw = b'Test project build\n  Test #1: one\n  Test #2: two\n\nTotal Tests: 2\n'
        self.assertEqual(build_tests.registered_names(raw), ('one', 'two'))
        for bad in (
                raw.replace(b'#2', b'#1'), raw.replace(b'two', b'one'),
                raw.replace(b'#2: two', b'#2 two'), raw.replace(b'Tests: 2', b'Tests: 3'),
                raw + b'Total Tests: 2\n', raw.replace(b'#2', b'#3'), b'\xff'):
            with self.subTest(raw=bad), self.assertRaises(ValueError):
                build_tests.registered_names(bad)
        for bad in ('{"tests":[],"tests":[]}', '{', 'null'):
            with self.subTest(json=bad), self.assertRaises(ValueError):
                build_tests.json_object(bad)

    def test_cmake_inventory_refuses_duplicate_missing_or_unreadable_producers(self):
        import ci_build_tests as build_tests

        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp).resolve()
            reply = build / '.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            index = {'reply': {'codemodel-v2': {
                'kind': 'codemodel', 'version': {'major': 2}, 'jsonFile': 'model.json'}}}
            ref = {'id': 'fixture::id', 'name': 'fixture', 'jsonFile': 'target.json'}
            model = {'configurations': [{'name': 'Release', 'targets': [ref]}]}
            target = {'id': 'fixture::id', 'name': 'fixture', 'type': 'EXECUTABLE',
                      'artifacts': [{'path': 'bin/fixture'}]}
            index_file = reply / 'index-control.json'
            model_file = reply / 'model.json'
            target_file = reply / 'target.json'
            index_file.write_text(json.dumps(index))
            model_file.write_text(json.dumps(model))
            target_file.write_text(json.dumps(target))
            self.assertEqual(build_tests.release_targets(build), {'fixture': ['bin/fixture']})
            for change in ('duplicate-row', 'no-rows', 'different-name', 'no-artifact',
                           'duplicate-artifact', 'escaping-artifact', 'unreadable'):
                changed_model, changed_target = copy.deepcopy(model), copy.deepcopy(target)
                if change == 'duplicate-row':
                    changed_model['configurations'][0]['targets'].append(ref)
                elif change == 'no-rows':
                    changed_model['configurations'][0]['targets'] = []
                elif change == 'different-name':
                    changed_target['name'] = 'invented'
                elif change == 'no-artifact':
                    changed_target['artifacts'] = []
                elif change == 'duplicate-artifact':
                    changed_target['artifacts'] *= 2
                elif change == 'escaping-artifact':
                    changed_target['artifacts'] = [{'path': '../outside'}]
                model_file.write_text(json.dumps(changed_model))
                target_file.write_text('{' if change == 'unreadable' else json.dumps(changed_target))
                with self.subTest(change=change), self.assertRaises(ValueError):
                    build_tests.release_targets(build)

    def test_workflow_requires_real_build_union_and_header_control(self):
        mutations = (
                ('build-tests', 'Build every Release test target',
                 '--shard ${{ matrix.shard }}/8', '--shard 1/8'),
                ('build-tests', 'Build every Release test target',
                 'python3 scripts/ci_build_tests.py build', 'echo build'),
                ('build-tests-result', 'Require the complete Release build',
                 'python3 scripts/ci_build_tests.py union', 'echo union'),
                ('build-tests', 'Exercise the broken registered header control',
                 'python3 scripts/test_ci_build_tests.py WorkflowControl', 'echo control'),
                ('build-tests', 'Build every Release test target',
                 '--jobs "$(getconf _NPROCESSORS_ONLN)"', '--jobs 1'),
                ('build-tests', 'Retain Release target inventories and build logs',
                 'if-no-files-found: error', 'if-no-files-found: warn'),
                ('build-tests-result', None,
                 'needs: [authz, build-tests]', 'needs: [authz]'),
                ('build-tests', None,
                 'shard: [1, 2, 3, 4, 5, 6, 7, 8]', 'shard: [1, 2, 3, 4, 5, 6, 7]'),
                ('build-tests', 'Build every Release test target',
                 '--source .', '--source scripts/fixtures/ci_build_tests'),
                ('build-tests-result', 'Require the complete Release build',
                 '--shards 8', '--shards 7'),
                ('build-tests-result', 'Require every Release build shard to succeed',
                 'test "$BUILD_TESTS_RESULT" = success', 'true'),
                ('build-tests-result', 'Require every Release build shard to succeed',
                 'BUILD_TESTS_RESULT: ${{ needs.build-tests.result }}',
                 'BUILD_TESTS_RESULT: success'),
                ('build-tests', 'Build every Release test target',
                 '        run: |', '        if: false\n        run: |'),
                ('build-tests', 'Set up Python',
                 '        with:', '        if: false\n        with:'),
        )
        for name, pr in (('ci-post.yml', False), ('gke-pr.yml', True)):
            workflow = (ROOT / '.github/workflows' / name).read_text()
            self.assertEqual(ci_preflight.release_build_workflow_findings(
                ci_preflight._jobs(workflow), pr=pr), [])
            if pr:
                self.assertEqual(ci_preflight.gke_admit_workflow_findings(workflow), [])
            for job, step, before, after in mutations:
                if pr and step is None:
                    before, after = before.replace('authz', 'admit'), after.replace('authz', 'admit')
                with self.subTest(workflow=name, job=job, mutation=after):
                    self.assertIn(before, workflow)
                    changed = mutate_workflow(workflow, job, step, before, after)
                    self.assertTrue(ci_preflight.release_build_workflow_findings(
                        ci_preflight._jobs(changed), pr=pr), (before, after))
                    if pr:
                        self.assertTrue(ci_preflight.gke_admit_workflow_findings(changed),
                                        (before, after))
            jobs = ci_preflight._jobs(workflow)
            for job in ('build-tests', 'build-tests-result'):
                with self.subTest(workflow=name, missing=job):
                    self.assertTrue(ci_preflight.release_build_workflow_findings(
                        {key: body for key, body in jobs.items() if key != job}, pr=pr))
                before = ci_preflight._job_values(jobs[job], 'if')[0]
                classifier = 'admit' if pr else 'changes'
                conditions = ['${{ false }}'] + [
                    before[:-3] + f" && needs.{classifier}.outputs.{field} != 'true' " + '}}'
                    for field in (('docs_only', 'github_only') if pr else ('docs_only',))]
                for after in conditions:
                    with self.subTest(workflow=name, job=job, condition=after):
                        changed = mutate_workflow(workflow, job, None, before, after)
                        self.assertTrue(ci_preflight.release_build_workflow_findings(
                            ci_preflight._jobs(changed), pr=pr))


if __name__ == '__main__':
    unittest.main()
