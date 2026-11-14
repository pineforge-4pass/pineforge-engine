#!/usr/bin/env python3
"""Controls for the cold-shard protocol; synthetic records are not hosted proof."""
from __future__ import annotations

import copy
import json
import unittest

import clang_cold_shards as cold
from test_clang_build_inventory import HEAD, TREE, fixture


def encoded(value):
    return json.dumps(value, sort_keys=True).encode()


def record(value):
    return cold.record(value if isinstance(value, bytes) else encoded(value))


def xml(names, **attributes):
    fields = {'tests': str(len(names)), 'failures': '0', 'skipped': '0', **attributes}
    return ('<testsuite ' + ' '.join(f'{k}="{v}"' for k, v in fields.items()) + '>' +
            ''.join(f'<testcase name="{name}" status="run" time="0.125"/>'
                    for name in names) + '</testsuite>').encode()


def phase(command, seconds=2):
    return record({'command': command, 'start_ns': 1000000000,
                   'end_ns': 1000000000 + int(seconds * 1000000000), 'exit_status': 0})


def inventory_records():
    makefile, model = fixture()
    records = {'makefile2': record(makefile.encode()), 'codemodel': record(model)}
    targets = model['configurations'][0]['targets']
    for target in targets:
        name = target['name']
        records['target:' + name] = record({
            'id': target['id'], 'name': name, 'type': 'EXECUTABLE',
            'artifacts': [{'path': 'bin/' + name}], 'sources': [{'path': name + '.cpp'}]})
    records['ctest'] = record({
        'kind': 'ctestInfo', 'version': {'major': 1, 'minor': 0},
        'tests': [{'name': name, 'command': ['/synthetic/build/bin/' + target],
                   'properties': []}
                  for name, target in (('alias_left', 'runner'), ('alias_right', 'leaf'))]})
    records['ctest-recipes'] = record({'CTestTestfile.cmake': (
        'add_test([=[alias_left]=] "/synthetic/build/bin/runner")\n'
        'add_test([=[alias_right]=] "/synthetic/build/bin/leaf")\n')})
    records['cache'] = record(cold.required_cache_text().encode())
    records['toolchain'] = record({'architecture': 'x86_64', 'compiler': 'fixture'})
    records['source-before'] = record({'head': HEAD, 'tree': TREE, 'files_sha256': 'a' * 64})
    records['configure'] = phase(['/usr/bin/cmake', '-S', '.', '-B', 'build', *cold.CONFIG])
    records['configure-log'] = record(b'-- Generating done\n')
    return records


def costs():
    records = inventory_records()
    identity = cold.decode(records['source-before'])
    inv = cold.configured_inventory(records, identity)
    builds, tests = [], []
    for index, name in enumerate(inv['targets']):
        seconds = (8, 2, 5, 3)[index]
        records['build:' + name] = phase(cold.build_command(inv, [name]), seconds)
        records['build-log:' + name] = record(f'Built target {name}\n'.encode())
        builds.append({'target': name, 'seconds': seconds})
    for index, name in enumerate(inv['tests']):
        target = 'runner' if name == 'alias_left' else 'leaf'
        records['test:' + name] = phase(cold.test_command(
            [name], 'build/clang-test-' + name + '.xml',
            trace='build/clang-test-' + name + '.trace'))
        records['test-log:' + name] = record(b'CTest synthetic control\n')
        records['junit:' + name] = record(xml([name]))
        records['trace:' + name] = record((
            '10 execve("/usr/bin/ctest", ["ctest"], 0x0) = 0\n'
            f'11 execve("/synthetic/build/bin/{target}", ["{target}"], 0x0) = 0\n').encode())
        tests.append({'test': name, 'seconds': 0.125,
                      'build_targets': cold.trace_targets(
                          cold.unrecord(records['trace:' + name]), inv, name)})
    records['prepare'] = phase(cold.prepare_command(), 4)
    records['prepare-log'] = record(b'ABI preparation synthetic control\n')
    records['source-after'] = records['source-before']
    return {'schema': cold.COST_SCHEMA, 'identity': identity, 'inventory': inv,
            'records': records, 'build_rows': builds, 'test_rows': tests,
            'context': context('collect', 11)}


def context(job, run_id):
    return {'GITHUB_REPOSITORY': 'pineforge-4pass/pineforge-engine', 'GITHUB_SHA': HEAD,
            'GITHUB_RUN_ID': str(run_id), 'GITHUB_RUN_ATTEMPT': '1', 'GITHUB_JOB': job,
            'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
            'RUNNER_OS': 'Linux', 'RUNNER_ARCH': 'X64'}


def synthetic_execution_pair():
    """Protocol bytes only; these fixtures are never performance evidence."""
    doc = costs()
    doc['context'] = context('collect', 11)
    origin = {'sha256': cold.digest(cold.encoded(doc)), 'run': {
        'id': 11, 'run_attempt': 1, 'head_sha': HEAD, 'status': 'completed',
        'conclusion': 'success', 'event': 'workflow_dispatch',
        'path': '.github/workflows/clang-cost-collection.yml',
        'repository': {'full_name': 'pineforge-4pass/pineforge-engine'}},
        'jobs': [{'id': 10, 'name': 'collect', 'head_sha': HEAD, 'run_id': 11,
                  'run_attempt': 1, 'status': 'completed', 'conclusion': 'success',
                  'labels': ['ubuntu-24.04']}]}
    plan = cold.make_plan(doc, doc['inventory'], doc['identity'], origin)
    raw_plan, results, jobs = cold.encoded(plan), [], []
    for shard in plan['shards']:
        index = shard['shard']
        records = inventory_records()
        records['source-after'] = records['source-before']
        records['prepare'] = phase(cold.prepare_command(), 4)
        records['prepare-log'] = record(b'ABI preparation synthetic control\n')
        records['build'] = phase(cold.build_command(plan['inventory'], shard['build_targets']))
        records['build-log'] = record(''.join(
            f'Built target {name}\n' for name in shard['build_targets']).encode())
        records['test'] = phase(cold.test_command(shard['tests'], 'build/clang-shard.xml'))
        records['test-log'] = record(b'CTest synthetic control\n')
        records['junit'] = record(xml(shard['tests']))
        results.append({'schema': cold.RESULT_SCHEMA, 'identity': doc['identity'],
                        'shard': index, 'plan_sha256': cold.digest(raw_plan),
                        'inventory': plan['inventory'], 'records': records,
                        'context': context(f'clang-shard-{index}', 15)})
        jobs.append({'id': index, 'name': f'clang-shard-{index}', 'head_sha': HEAD,
                     'run_id': 15, 'run_attempt': 1, 'status': 'completed',
                     'conclusion': 'success', 'labels': ['ubuntu-24.04'],
                     'runner_name': 'GitHub Actions 1', 'started_at': '2026-11-14T10:00:00Z',
                     'completed_at': '2026-11-14T10:14:59Z'})
    run = {'id': 15, 'run_attempt': 1, 'head_sha': HEAD, 'event': 'workflow_dispatch',
           'path': '.github/workflows/clang-build.yml',
           'repository': {'full_name': 'pineforge-4pass/pineforge-engine'}}
    needs = dict.fromkeys(('authz', 'collect', 'plan', 'clang-shard-1', 'clang-shard-2'), 'success')
    return raw_plan, results, jobs, run, needs, doc['identity']


class CostAndPlanControls(unittest.TestCase):
    def test_actual_records_reconstruct_inventory_aliases_and_dependency_costs(self):
        doc = costs()
        checked = cold.validate_costs(doc, doc['inventory'], doc['identity'])
        plan = cold.make_plan(doc, doc['inventory'], doc['identity'])
        self.assertEqual(set(checked['tests']), {'alias_left', 'alias_right'})
        self.assertEqual(sorted(t for shard in plan['shards'] for t in shard['tests']),
                         ['alias_left', 'alias_right'])
        self.assertEqual(sorted(t for shard in plan['shards'] for t in shard['owned_targets']),
                         doc['inventory']['targets'])
        self.assertIn('needed', checked['dependencies']['alias_left'])
        self.assertEqual([s['preparation_seconds'] for s in plan['shards']], [4, 4])
        self.assertIn('not hosted wall', plan['cost_semantics'])

    def test_forged_scalar_and_different_valid_record_are_both_refused(self):
        original = costs()
        for mutate in ('scalar', 'record'):
            changed = copy.deepcopy(original)
            name = changed['build_rows'][0]['target']
            if mutate == 'scalar':
                changed['build_rows'][0]['seconds'] += 1
            else:
                changed['records']['build:' + name] = phase(
                    cold.build_command(changed['inventory'], [name]), 99)
            with self.subTest(mutate=mutate), self.assertRaises(cold.EvidenceError):
                cold.make_plan(changed, original['inventory'], original['identity'])

    def test_missing_duplicate_stale_unknown_and_unbound_costs_refuse(self):
        original = costs()
        mutations = []
        for field in ('build_rows', 'test_rows'):
            missing = copy.deepcopy(original)
            missing[field].pop()
            mutations.append(missing)
            duplicate = copy.deepcopy(original)
            duplicate[field].append(copy.deepcopy(duplicate[field][0]))
            mutations.append(duplicate)
        for field in ('head', 'tree', 'files_sha256'):
            stale = copy.deepcopy(original)
            stale['identity'][field] = '0' * len(stale['identity'][field])
            mutations.append(stale)
        forged = copy.deepcopy(original)
        forged['records']['trace:alias_left'] = record(b'11 openat(AT_FDCWD, "/other", 0) = 3\n')
        mutations.append(forged)
        unknown = copy.deepcopy(original)
        unknown['weights'] = {'alias_left': 1}
        mutations.append(unknown)
        for index, changed in enumerate(mutations):
            with self.subTest(index=index), self.assertRaises(cold.EvidenceError):
                cold.make_plan(changed, original['inventory'], original['identity'])

    def test_selection_does_not_follow_name_order_and_keeps_build_only_target(self):
        doc = costs()
        plan = cold.make_plan(doc, doc['inventory'], doc['identity'])
        self.assertEqual(len(plan['shards']), 2)
        self.assertTrue(all(s['tests'] for s in plan['shards']))
        self.assertIn('engine', {t for s in plan['shards'] for t in s['owned_targets']})
        changed = copy.deepcopy(doc)
        next(row for row in changed['build_rows'] if row['target'] == 'leaf')['seconds'] = 30
        changed['records']['build:leaf'] = phase(
            cold.build_command(doc['inventory'], ['leaf']), 30)
        changed_plan = cold.make_plan(changed, doc['inventory'], doc['identity'])
        self.assertNotEqual([s['tests'] for s in plan['shards']],
                            [s['tests'] for s in changed_plan['shards']])
        renamed = copy.deepcopy(doc)
        renamed['inventory']['tests']['alias_left']['command'] = ['ignored']
        with self.assertRaises(cold.EvidenceError):
            cold.make_plan(renamed, doc['inventory'], doc['identity'])

    def test_fixtures_and_explicit_test_dependencies_stay_together(self):
        inv = {'tests': {
            'setup': {'properties': {'FIXTURES_SETUP': ['shared']}},
            'work': {'properties': {'FIXTURES_REQUIRED': ['shared']}},
            'cleanup': {'properties': {'FIXTURES_CLEANUP': ['shared']}},
            'other': {'properties': {}}}}
        self.assertEqual(cold.test_groups(inv), [['cleanup', 'setup', 'work'], ['other']])
        inv['tests']['other']['properties']['DEPENDS'] = ['absent']
        with self.assertRaises(cold.EvidenceError):
            cold.test_groups(inv)


class ExecutionAndHostedControls(unittest.TestCase):
    def test_two_actual_junit_unions_and_build_logs_are_mandatory(self):
        args = synthetic_execution_pair()
        proof = cold.check(*args)
        self.assertEqual(proof['executed_tests'], ['alias_left', 'alias_right'])
        self.assertEqual(proof['actual_test_count'], 2)
        self.assertEqual(proof['built_targets'], ['engine', 'leaf', 'needed', 'runner'])
        mutations = []
        changed = copy.deepcopy(args)
        changed[1].pop()
        mutations.append(changed)
        changed = copy.deepcopy(args)
        changed[1][1] = copy.deepcopy(changed[1][0])
        mutations.append(changed)
        for index in (0, 1):
            for key in ('source-after', 'build-log', 'junit'):
                changed = copy.deepcopy(args)
                changed[1][index]['records'].pop(key)
                mutations.append(changed)
            changed = copy.deepcopy(args)
            changed[1][index]['plan_sha256'] = 'f' * 64
            mutations.append(changed)
            changed = copy.deepcopy(args)
            changed[1][index]['context']['RUNNER_ENVIRONMENT'] = 'self-hosted'
            mutations.append(changed)
            for verdict in ('skipped', 'failure', 'cancelled', ''):
                changed = copy.deepcopy(args)
                changed[4][f'clang-shard-{index + 1}'] = verdict
                mutations.append(changed)
        changed = copy.deepcopy(args)
        changed[4].pop('clang-shard-2')
        mutations.append(changed)
        for index, changed in enumerate(mutations):
            with self.subTest(index=index), self.assertRaises((cold.EvidenceError, KeyError)):
                cold.check(*changed)

    def test_original_measurement_origin_cannot_be_relabelled_or_replaced(self):
        args = synthetic_execution_pair()
        plan = cold.loads(args[0])
        for field, value in (
                ('head_sha', '9' * 40), ('status', 'queued'),
                ('event', 'pull_request'), ('path', '.github/workflows/ci-post.yml'),
                ('id', 22), ('run_attempt', 2)):
            changed = copy.deepcopy(plan)
            changed['cost_origin']['run'][field] = value
            with self.subTest(field=field), self.assertRaises(cold.EvidenceError):
                cold.verify_plan(changed)
        changed = copy.deepcopy(plan)
        changed['cost_origin'] = None
        with self.assertRaises(cold.EvidenceError):
            cold.verify_plan(changed)
        origin_mutations = []
        changed = copy.deepcopy(plan)
        changed['cost_origin']['jobs'] = []
        origin_mutations.append(changed)
        changed = copy.deepcopy(plan)
        changed['cost_origin']['jobs'].append(copy.deepcopy(changed['cost_origin']['jobs'][0]))
        origin_mutations.append(changed)
        for field, value in (
                ('conclusion', 'failure'), ('conclusion', 'skipped'),
                ('conclusion', 'cancelled'), ('head_sha', '9' * 40),
                ('labels', ['self-hosted', 'ubuntu-24.04']),
                ('run_id', 22), ('run_attempt', 2)):
            changed = copy.deepcopy(plan)
            changed['cost_origin']['jobs'][0][field] = value
            origin_mutations.append(changed)
        changed = copy.deepcopy(plan)
        changed['cost_origin']['sha256'] = 'f' * 64
        origin_mutations.append(changed)
        for index, changed in enumerate(origin_mutations):
            with self.subTest(origin=index), self.assertRaises(cold.EvidenceError):
                cold.verify_plan(changed)

    def test_cold_recipe_commands_are_bound_without_fake_executables(self):
        records = inventory_records()
        doc = cold.decode(records['ctest'])
        doc['tests'][0].pop('command')
        records['ctest'] = record(doc)
        inv = cold.configured_inventory(records, cold.decode(records['source-before']))
        self.assertEqual(inv['tests']['alias_left']['command'], ['${BUILD}/bin/runner'])
        for recipes in (
                {'CTestTestfile.cmake': 'add_test(alias_left "/different")\n'},
                {'CTestTestfile.cmake': 'subdirs("../outside")\n'},
                {'CTestTestfile.cmake': 'include("/outside")\n'}):
            changed = copy.deepcopy(records)
            changed['ctest-recipes'] = record(recipes)
            with self.assertRaises(cold.EvidenceError):
                cold.configured_inventory(changed, cold.decode(records['source-before']))

    def test_actual_junit_requires_exact_unique_passed_execution(self):
        self.assertEqual(cold.junit_rows(xml(['one', 'two']), ['one', 'two']),
                         {'one': 0.125, 'two': 0.125})
        mutations = [
            xml(['one']), xml(['one', 'one']), xml(['one', 'two', 'extra']),
            xml(['one', 'two'], skipped='1'),
            xml(['one', 'two']).replace(b'status="run"', b'status="notrun"', 1),
            xml(['one', 'two']).replace(b'/>', b'><skipped/></testcase>', 1),
            xml(['one', 'two']).replace(b'/>', b'><failure/></testcase>', 1),
            xml(['one', 'two']).replace(b'time="0.125"', b'time="NaN"', 1)]
        for index, data in enumerate(mutations):
            with self.subTest(index=index), self.assertRaises(cold.EvidenceError):
                cold.junit_rows(data, ['one', 'two'])

    def test_api_requires_both_successful_hosted_jobs_same_attempt_below_900(self):
        jobs = []
        for index in (1, 2):
            jobs.append({'id': index, 'name': f'clang-shard-{index}', 'head_sha': HEAD,
                         'run_id': 15, 'run_attempt': 1, 'status': 'completed',
                         'conclusion': 'success', 'labels': ['ubuntu-24.04'],
                         'runner_name': 'GitHub Actions 1',
                         'started_at': '2026-11-14T10:00:00Z',
                         'completed_at': '2026-11-14T10:14:59Z'})
        self.assertEqual(cold.hosted_durations(jobs, HEAD, '15', '1'), [899, 899])
        mutations = [jobs[:1], [*jobs, copy.deepcopy(jobs[0])]]
        for index in (0, 1):
            for field, values in {
                    'conclusion': ['skipped', 'cancelled', 'failure', None],
                    'head_sha': ['9' * 40], 'run_id': [16], 'run_attempt': [2],
                    'labels': [['self-hosted', 'ubuntu-24.04']],
                    'completed_at': [None, '2026-11-14T10:15:00Z']}.items():
                for value in values:
                    changed = copy.deepcopy(jobs)
                    changed[index][field] = value
                    mutations.append(changed)
        for index, changed in enumerate(mutations):
            with self.subTest(index=index), self.assertRaises(cold.EvidenceError):
                cold.hosted_durations(changed, HEAD, '15', '1')


if __name__ == '__main__':
    unittest.main()
