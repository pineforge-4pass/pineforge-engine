#!/usr/bin/env python3
"""Cold Clang build/CTest collection, measured planning and execution union.

The v1 archive parser remains in clang_build_shards.py for historical records.
Its overlapping target windows cannot enter this protocol. Every v2 cost is
reconstructed from retained command, trace and JUnit bytes for the exact source,
configuration and toolchain. Scheduling sums are never hosted wall times.

collect is an input measurement, not acceptance. plan configures but builds
nothing. run always finishes its assigned work, without a 900-second deadline.
check requires both actual execution artifacts and the completed hosted job API
records; only there is the strictly-less-than-900-second requirement evaluated.
No cache is restored or saved. All outputs are retained; none are removed.
"""
from __future__ import annotations

import argparse
import base64
import binascii
from collections import Counter
from datetime import datetime
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

import clang_build_shards as legacy


EvidenceError = legacy.EvidenceError
require = legacy._require
COST_SCHEMA = 'clang-cold-costs-v2'
PLAN_SCHEMA = 'clang-cold-plan-v2'
RESULT_SCHEMA = 'clang-cold-result-v2'
PROOF_SCHEMA = 'clang-cold-union-v2'
JOBS = 4
GCC = '--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13'
CONFIG = [
    '-G', 'Unix Makefiles', '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_CXX_STANDARD=17', '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
    '-DCMAKE_CXX_EXTENSIONS=OFF', '-DCMAKE_C_COMPILER=/usr/bin/clang-18',
    '-DCMAKE_CXX_COMPILER=/usr/bin/clang++-18', '-DCMAKE_CXX_FLAGS=' + GCC,
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DPINEFORGE_BUILD_TESTS=ON',
    '-DPINEFORGE_BUILD_SOURCE_LAYER=ON', '-DPINEFORGE_BUILD_TUTORIAL=ON',
    '-DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF', '-DPINEFORGE_BUILD_EXAMPLES=OFF',
    '-DPINEFORGE_ENABLE_SANITIZERS=OFF', '-DPINEFORGE_ENABLE_COVERAGE=OFF',
    '-DPython3_EXECUTABLE:FILEPATH=/usr/bin/python3',
    '-DEigen3_DIR:PATH=/usr/share/eigen3/cmake', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
]
REQUIRED_CACHE = {
    'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_CXX_STANDARD': '17',
    'CMAKE_CXX_STANDARD_REQUIRED': 'ON', 'CMAKE_CXX_EXTENSIONS': 'OFF',
    'CMAKE_C_COMPILER': '/usr/bin/clang-18',
    'CMAKE_CXX_COMPILER': '/usr/bin/clang++-18', 'CMAKE_CXX_FLAGS': GCC,
    'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON', 'PINEFORGE_BUILD_TESTS': 'ON',
    'PINEFORGE_BUILD_SOURCE_LAYER': 'ON', 'PINEFORGE_BUILD_TUTORIAL': 'ON',
    'PINEFORGE_BUILD_CORPUS_STRATEGIES': 'OFF', 'PINEFORGE_BUILD_EXAMPLES': 'OFF',
    'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
    'CMAKE_GENERATOR': 'Unix Makefiles',
    'Python3_EXECUTABLE': '/usr/bin/python3',
    'CMAKE_C_COMPILER_LAUNCHER': '', 'CMAKE_CXX_COMPILER_LAUNCHER': '',
}
NAME = re.compile(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*')
BUILT = re.compile(r'^(?:\[\s*[0-9]+%\]\s*)?Built target ([A-Za-z0-9_][A-Za-z0-9_.+-]*)$')


def encoded(value):
    return (json.dumps(value, sort_keys=True, allow_nan=False, separators=(',', ':')) + '\n').encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def loads(data):
    try:
        result = json.loads(data, object_pairs_hook=legacy._no_duplicate_keys,
                            parse_constant=legacy._nonfinite_constant)
        legacy._finite_json(result)
        return result
    except (ValueError, UnicodeError) as error:
        raise EvidenceError(f'invalid JSON: {error}') from error


def keys(value, expected, label):
    require(isinstance(value, dict) and set(value) == set(expected),
            f'{label}: missing or unknown fields')


def record(data):
    require(isinstance(data, bytes), 'record requires original bytes')
    return {'sha256': digest(data), 'base64': base64.b64encode(data).decode('ascii')}


def unrecord(value):
    keys(value, ('sha256', 'base64'), 'record')
    legacy._sha(value['sha256'], 64, 'record.sha256')
    try:
        data = base64.b64decode(value['base64'], validate=True)
    except (ValueError, TypeError, binascii.Error) as error:
        raise EvidenceError('invalid base64 record') from error
    require(digest(data) == value['sha256'], 'record digest disagrees with original bytes')
    return data


def decode(value):
    return loads(unrecord(value))


def names(values, label, empty=False):
    require(isinstance(values, list) and (empty or bool(values)) and
            all(isinstance(v, str) and NAME.fullmatch(v) for v in values) and
            len(set(values)) == len(values), f'{label}: expected unique real identifiers')
    return values


def identity_checked(value):
    keys(value, ('head', 'tree', 'files_sha256'), 'source identity')
    legacy._identity(value, 'source')
    legacy._sha(value['files_sha256'], 64, 'source.files_sha256')
    return value


def normal(value, source, build):
    if isinstance(value, str):
        return value.replace(build, '${BUILD}').replace(source, '${SOURCE}')
    if isinstance(value, list):
        return [normal(v, source, build) for v in value]
    if isinstance(value, dict):
        return {k: normal(v, source, build) for k, v in value.items()}
    return value


def required_cache_text():
    return ''.join(f'{key}:STRING={value}\n' for key, value in REQUIRED_CACHE.items())


def configured_inventory(records, identity):
    """Reconstruct ALL and CTest from actual generated bytes, without exclusions."""
    identity_checked(identity)
    phase_seconds(records['configure'], ['/usr/bin/cmake', '-S', '.', '-B', 'build', *CONFIG])
    model_bytes = unrecord(records['codemodel'])
    make_bytes = unrecord(records['makefile2'])
    model = loads(model_bytes)
    old = legacy.collect_inventory(make_bytes, model_bytes, configuration='Release',
                                   head=identity['head'], tree=identity['tree'],
                                   architecture='x86_64')
    paths = old['provenance']['paths']
    source, build = paths['source'], paths['build']
    bindings = {row['name']: row for row in old['provenance']['target_bindings']}
    by_node = {row['all_node']: row['name'] for row in bindings.values()}
    rules, _ = legacy._makefile2_rules(make_bytes)
    targets = {}
    for name, binding in bindings.items():
        detail = decode(records['target:' + name])
        require(detail.get('name') == name and detail.get('id') == binding['id'],
                f'{name}: target detail is not the codemodel target')
        for group in detail.get('compileGroups', []):
            for include in group.get('includes', []):
                path = include.get('path', '')
                require(path in (source, build) or
                        path.startswith(source + '/') or path.startswith(build + '/') or
                        path == '/usr/include/eigen3',
                        f'{name}: unbound external include dependency')
        seen, pending = set(), [binding['all_node']]
        while pending:
            node = pending.pop()
            if node in seen:
                continue
            seen.add(node)
            pending.extend(legacy._all_prerequisites(node, rules))
        require(all(node in old['provenance']['reachable_all_nodes'] for node in seen),
                f'{name}: build dependency outside root ALL')
        artifacts = []
        for artifact in detail.get('artifacts', []):
            path = artifact.get('path')
            require(isinstance(path, str) and path and '\x00' not in path,
                    f'{name}: malformed artifact')
            if not path.startswith('/'):
                path = build + '/' + path
            require(path.startswith(source + '/') or path.startswith(build + '/'),
                    f'{name}: artifact outside source and build')
            artifacts.append(normal(path, source, build))
        targets[name] = {
            'id': binding['id'], 'all_node': binding['all_node'], 'type': detail.get('type'),
            'dependencies': sorted({by_node[node] for node in seen if node in by_node} - {name}),
            'artifacts': sorted(artifacts), 'detail': normal(detail, source, build),
        }
    cache = {}
    for line in unrecord(records['cache']).decode().splitlines():
        match = re.fullmatch(r'([^:#/][^:]*):[^=]+=(.*)', line)
        if match:
            require(match[1] not in cache, 'duplicate CMake cache key')
            cache[match[1]] = match[2]
    for key, expected in REQUIRED_CACHE.items():
        require(cache.get(key, '') == expected, f'configuration drift: {key}')
    for key in ('CMAKE_C_FLAGS', 'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_SHARED_LINKER_FLAGS'):
        require(cache.get(key, '') == '', f'unexpected compiler/linker flags: {key}')
    toolchain = decode(records['toolchain'])
    require(toolchain.get('architecture') == 'x86_64', 'cost architecture is not x86_64')
    raw_tests = decode(records['ctest'])
    require(raw_tests.get('kind') == 'ctestInfo' and
            raw_tests.get('version', {}).get('major') == 1, 'unsupported CTest inventory')
    recipes = parse_ctest_recipes(decode(records['ctest-recipes']))
    tests = {}
    for index, row in enumerate(raw_tests.get('tests', []), 1):
        name = row.get('name')
        names([name], 'CTest name')
        require(name not in tests, 'duplicate configured CTest identifier')
        require(name in recipes, f'{name}: configured recipe is absent')
        command = recipes[name]
        require(isinstance(command, list) and command and
                all(isinstance(v, str) and v for v in command), f'{name}: no executable command')
        published_command = row.get('command')
        require(not published_command or published_command == ['NOT_AVAILABLE'] or
                published_command == command,
                f'{name}: CTest command differs from generated recipe')
        props = {}
        for prop in row.get('properties', []):
            key = prop.get('name')
            require(isinstance(key, str) and key not in props, f'{name}: duplicate property')
            props[key] = prop.get('value')
        require(props.get('DISABLED', False) is False, f'{name}: configured row is disabled')
        tests[name] = {'index': index, 'command': normal(command, source, build),
                       'properties': normal(props, source, build)}
    require(bool(tests), 'configured CTest inventory is empty')
    require(set(tests) == set(recipes), 'configured recipe and CTest population differ')
    result = {'targets': sorted(targets), 'target_details': targets, 'tests': tests,
              'configuration': CONFIG, 'toolchain': toolchain}
    test_groups(result)  # also refuses dangling fixture/dependency identities
    return result


def closure(inventory, selected):
    result = set()
    for name in selected:
        require(name in inventory['target_details'], f'unknown build target: {name}')
        result.add(name)
        result.update(inventory['target_details'][name]['dependencies'])
    return sorted(result)


def build_command(inventory, selected):
    names(list(selected), 'build selection')
    return ['/usr/bin/make', '-C', 'build', '-f', 'CMakeFiles/Makefile2', '-j4',
            *[inventory['target_details'][n]['all_node'] for n in sorted(selected)]]


def test_command(selected, junit, trace=None):
    names(list(selected), 'test selection')
    command = ['/usr/bin/ctest', '--test-dir', 'build', '--parallel', '4',
               '-R', '^(' + '|'.join(re.escape(n) for n in sorted(selected)) + ')$',
               '-FA', '.*', '--output-on-failure', '--no-tests=error', '--output-junit', junit]
    if trace is not None:
        command = ['/usr/bin/strace', '-f', '-qq', '-s', '65535', '-e', 'trace=file',
                   '-o', trace, *command]
    return command


def prepare_command():
    return ['/usr/bin/python3', 'scripts/check_abi_receipt_skips.py',
            '--build-dir', 'build', '--prepare']


def phase_seconds(value, command):
    phase = decode(value)
    keys(phase, ('command', 'start_ns', 'end_ns', 'exit_status'), 'timed command')
    require(phase['command'] == command, 'timed command/selection differs from actual recipe')
    require(type(phase['exit_status']) is int and phase['exit_status'] == 0,
            'actual command failed or did not finish')
    require(type(phase['start_ns']) is int and type(phase['end_ns']) is int and
            0 < phase['start_ns'] < phase['end_ns'], 'invalid monotonic timed-work record')
    return (phase['end_ns'] - phase['start_ns']) / 1000000000


def junit_rows(data, expected):
    names(list(expected), 'expected executions')
    require(b'<!DOCTYPE' not in data and b'<!ENTITY' not in data, 'unsupported XML declarations')
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise EvidenceError('malformed actual JUnit') from error
    require(root.tag == 'testsuite', 'expected CTest testsuite')
    cases = root.findall('testcase')
    require(root.get('tests') == str(len(cases)), 'JUnit measured count disagrees with cases')
    for field in ('failures', 'errors', 'skipped', 'disabled'):
        require(root.get(field, '0') == '0', f'JUnit {field} is nonzero')
    actual = []
    rows = {}
    for case in cases:
        name = case.get('name')
        require(name in expected and name not in rows, 'unexpected or duplicate executed test')
        require(case.get('status') == 'run' and
                not any(case.find(tag) is not None for tag in ('failure', 'error', 'skipped')),
                f'{name}: test did not pass, skipped or did not run')
        try:
            seconds = Decimal(case.get('time', ''))
        except ArithmeticError as error:
            raise EvidenceError(f'{name}: invalid measured CTest cost') from error
        require(seconds.is_finite() and seconds >= 0, f'{name}: invalid measured CTest cost')
        output = case.findtext('system-out', '')
        require(not re.search(r'(?m)^(?:OK \(skipped=[1-9][0-9]*\)|=+.*\b[1-9][0-9]* skipped\b.*=+)\s*$',
                              output), f'{name}: nested test runner skipped work')
        actual.append(name)
        rows[name] = float(seconds)
    require(Counter(actual) == Counter(expected), 'actual execution union differs from selection')
    return rows


def trace_targets(data, inventory, test):
    """Conservative artifact-use mapping, recomputed from original file syscalls.

    A relative basename may over-include a target; it never subtracts one.
    CMake dependency closures are included even when the loader does not open
    an archive at runtime. Unused default ALL members remain build-only parts.
    """
    text = data.decode('utf-8')
    require('execve("/usr/bin/ctest"' in text, f'{test}: trace lacks actual CTest execution')
    paths, executables = set(), set()
    for line in text.splitlines():
        # CTest's discovery stat/access calls are not binary use. Only exec and
        # content opens contribute; unfinished calls conservatively contribute
        # too, so interleaved trace records cannot subtract a dependency.
        call = re.search(r'\b(execve|execveat|open|openat|openat2)\(', line)
        if not call:
            continue
        quoted = []
        for match in re.finditer(r'"(?:\\.|[^"\\])*"', line):
            try:
                quoted.append(json.loads(match[0]))
            except ValueError as error:
                raise EvidenceError(f'{test}: unsupported trace string escaping') from error
        if quoted:
            paths.add(quoted[0])
            if call[1].startswith('exec'):
                executables.add(quoted[0])
    # Original paths are carried by the codemodel; normalized paths make runs
    # in distinct hosted checkout directories comparable.
    command = inventory['tests'][test]['command']
    require(any(Path(command[0]).name == Path(path).name for path in executables),
            f'{test}: trace lacks configured executable')
    targets = set()
    used_basenames = {Path(path).name for path in paths}
    configured = json.dumps(inventory['tests'][test])
    for name, detail in inventory['target_details'].items():
        if any(Path(artifact).name in used_basenames for artifact in detail['artifacts']):
            targets.add(name)
        # A configured command/REQUIRED_FILES/ENVIRONMENT reference is also a
        # dependency, even if the OS resolved the runtime path through a symlink.
        if any(artifact in configured for artifact in detail['artifacts']):
            targets.add(name)
    return closure(inventory, sorted(targets))


def _property_list(test, key):
    value = test['properties'].get(key, [])
    require(isinstance(value, list) and all(isinstance(v, str) and v for v in value),
            f'{key}: unsupported CTest property')
    return value


def test_edges(inventory):
    tests = inventory['tests']
    edges = {name: set() for name in tests}
    fixtures = {}
    for name, row in tests.items():
        for required in _property_list(row, 'DEPENDS'):
            require(required in tests, f'{name}: dangling CTest dependency {required}')
            edges[name].add(required)
        for role in ('SETUP', 'REQUIRED', 'CLEANUP'):
            for fixture in _property_list(row, 'FIXTURES_' + role):
                fixtures.setdefault(fixture, {}).setdefault(role, set()).add(name)
    for fixture, roles in fixtures.items():
        require(roles.get('SETUP'), f'fixture {fixture}: no configured setup')
        for name in roles.get('REQUIRED', set()):
            edges[name].update(roles['SETUP'] - {name})
        for name in roles.get('CLEANUP', set()):
            edges[name].update((roles.get('REQUIRED', set()) | roles['SETUP']) - {name})
    return edges


def ordered(edges):
    done, result = set(), []
    while len(done) < len(edges):
        ready = sorted(name for name, dependencies in edges.items()
                       if name not in done and dependencies <= done)
        require(bool(ready), 'dependency cycle or unresolved member')
        result.extend(ready)
        done.update(ready)
    return result


def test_groups(inventory):
    edges = test_edges(inventory)
    ordered(edges)
    neighbours = {name: set(dependencies) for name, dependencies in edges.items()}
    for name, dependencies in edges.items():
        for dependency in dependencies:
            neighbours[dependency].add(name)
    groups, seen = [], set()
    for name in sorted(neighbours):
        if name in seen:
            continue
        members, pending = set(), [name]
        while pending:
            member = pending.pop()
            if member in members:
                continue
            members.add(member)
            pending.extend(neighbours[member] - members)
        groups.append(sorted(members))
        seen.update(members)
    return groups


def validate_costs(document, inventory, identity):
    keys(document, ('schema', 'identity', 'inventory', 'records', 'build_rows',
                    'test_rows', 'context'), 'cost document')
    require(document['schema'] == COST_SCHEMA, 'not complete cold v2 costs')
    require(identity_checked(document['identity']) == identity_checked(identity),
            'measured source/head/tree is stale or different')
    records = document['records']
    require(decode(records['source-before']) == identity and
            decode(records['source-after']) == identity, 'measurement changed source identity')
    measured_inventory = configured_inventory(records, identity)
    require(document['inventory'] == measured_inventory == inventory,
            'measured population/configuration/toolchain differs from configured tree')
    expected_records = configured_record_keys(records) | {'source-after', 'prepare', 'prepare-log'}
    expected_records.update(prefix + name for name in inventory['targets']
                            for prefix in ('build:', 'build-log:'))
    expected_records.update(prefix + name for name in inventory['tests']
                            for prefix in ('test:', 'test-log:', 'junit:', 'trace:'))
    keys(records, expected_records, 'complete measurement records')
    for value in records.values():
        unrecord(value)
    keys(document['context'], runtime_context().keys(), 'collection context')
    target_weights, test_weights, dependencies = {}, {}, {}
    for row in document['build_rows']:
        keys(row, ('target', 'seconds'), 'build cost row')
        name = row['target']
        require(name in inventory['targets'] and name not in target_weights,
                'unknown or duplicate build cost')
        seconds = phase_seconds(records['build:' + name], build_command(inventory, [name]))
        legacy._number(row['seconds'], 'measured build seconds', positive=True)
        built = [match[1] for line in unrecord(records['build-log:' + name]).decode().splitlines()
                 if (match := BUILT.fullmatch(line))]
        require(name in built and set(built) <= set(closure(inventory, [name])),
                f'{name}: build log does not prove its configured target')
        require(row['seconds'] == seconds, f'{name}: forged measured build weight')
        target_weights[name] = seconds
    for row in document['test_rows']:
        keys(row, ('test', 'seconds', 'build_targets'), 'test cost row')
        name = row['test']
        require(name in inventory['tests'] and name not in test_weights,
                'unknown or duplicate test cost')
        command = test_command([name], 'build/clang-test-' + name + '.xml',
                               trace='build/clang-test-' + name + '.trace')
        phase_seconds(records['test:' + name], command)
        seconds = junit_rows(unrecord(records['junit:' + name]), [name])[name]
        legacy._number(row['seconds'], 'measured CTest seconds')
        actual_targets = trace_targets(unrecord(records['trace:' + name]), inventory, name)
        require(row['seconds'] == seconds, f'{name}: forged measured CTest weight')
        require(row['build_targets'] == actual_targets, f'{name}: forged build-target mapping')
        test_weights[name], dependencies[name] = seconds, actual_targets
    require(set(target_weights) == set(inventory['targets']) and
            set(test_weights) == set(inventory['tests']), 'missing measured cost rows', {
                'missing_targets': sorted(set(inventory['targets']) - target_weights.keys()),
                'missing_tests': sorted(set(inventory['tests']) - test_weights.keys())})
    preparation = phase_seconds(records['prepare'], prepare_command())
    return {'targets': target_weights, 'tests': test_weights,
            'dependencies': dependencies, 'preparation': preparation}


def cost_origin_checked(costs, origin):
    keys(origin, ('run', 'jobs', 'sha256'), 'measurement origin')
    require(digest(encoded(costs)) == origin['sha256'], 'cost original member digest mismatch')
    run = origin['run']
    require(run.get('head_sha') == costs['identity']['head'] and
            run.get('path') in ('.github/workflows/clang-cost-collection.yml',
                                '.github/workflows/clang-build.yml') and
            run.get('event') in ('push', 'workflow_dispatch') and
            run.get('status') in ('in_progress', 'completed') and
            run.get('repository', {}).get('full_name') == 'pineforge-4pass/pineforge-engine',
            'not an exact-source collection run')
    jobs = origin['jobs']
    require(isinstance(jobs, list), 'missing cost job API records')
    collectors = [job for job in jobs if job.get('name') == 'collect']
    require(len(collectors) == 1, 'missing or duplicate completed cost job')
    job = collectors[0]
    require(job.get('head_sha') == costs['identity']['head'] and
            job.get('run_id') == run.get('id') and job.get('run_attempt') == run.get('run_attempt') and
            job.get('status') == 'completed' and job.get('conclusion') == 'success' and
            'ubuntu-24.04' in job.get('labels', []) and
            'self-hosted' not in job.get('labels', []), 'cost job did not actually succeed on hosted x86')
    context = costs['context']
    for key, value in {
            'GITHUB_REPOSITORY': 'pineforge-4pass/pineforge-engine',
            'GITHUB_SHA': costs['identity']['head'], 'GITHUB_RUN_ID': str(run.get('id')),
            'GITHUB_RUN_ATTEMPT': str(run.get('run_attempt')), 'GITHUB_JOB': 'collect',
            'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
            'RUNNER_OS': 'Linux', 'RUNNER_ARCH': 'X64'}.items():
        require(context.get(key) == value, 'cost collection origin mismatch: ' + key)
    return origin


def make_plan(costs, inventory, identity, origin=None):
    measured = validate_costs(costs, inventory, identity)
    if origin is not None:
        cost_origin_checked(costs, origin)
    groups = test_groups(inventory)
    require(len(groups) >= 2, 'two shards require two independent nonempty test groups')
    shards = [{'shard': i, 'tests': [], 'build_targets': [], 'owned_targets': [],
               'preparation_seconds': measured['preparation']} for i in (1, 2)]
    def load(shard, extra_tests=(), extra_targets=()):
        tests = set(shard['tests']) | set(extra_tests)
        targets = set(shard['build_targets']) | set(extra_targets)
        return (sum(measured['tests'][n] for n in tests) +
                sum(measured['targets'][n] for n in targets) + measured['preparation'])
    def assign(tests, targets):
        targets = closure(inventory, targets)
        # Test/target names only break equal measured-cost ties; no name/modulo
        # partition and no guessed/default cost enters the schedule.
        chosen = min(range(2), key=lambda i: (
            max(load(shards[i], tests, targets), load(shards[1-i])),
            load(shards[i], tests, targets), i))
        shards[chosen]['tests'].extend(tests)
        shards[chosen]['build_targets'] = sorted(set(shards[chosen]['build_targets']) | set(targets))
    def group_targets(group):
        return sorted({target for test in group for target in measured['dependencies'][test]})
    groups.sort(key=lambda g: (-(sum(measured['tests'][n] for n in g) +
                               sum(measured['targets'][n] for n in group_targets(g))), g))
    for group in groups:
        assign(group, group_targets(group))
    covered = {n for shard in shards for n in shard['build_targets']}
    for name in sorted(set(inventory['targets']) - covered,
                       key=lambda n: (-measured['targets'][n], n)):
        if not any(name in shard['build_targets'] for shard in shards):
            assign([], [name])
    for name in inventory['targets']:
        owner = min((s for s in shards if name in s['build_targets']),
                    key=lambda s: (load(s), s['shard']))
        owner['owned_targets'].append(name)
    for shard in shards:
        shard['tests'].sort()
        shard['scheduling_seconds'] = load(shard)
    require(all(s['tests'] and s['build_targets'] for s in shards), 'empty shard')
    require(Counter(n for s in shards for n in s['tests']) == Counter(inventory['tests'].keys()),
            'planned test union is incomplete or duplicated')
    require(Counter(n for s in shards for n in s['owned_targets']) == Counter(inventory['targets']),
            'planned default ALL ownership is incomplete or duplicated')
    return {'schema': PLAN_SCHEMA, 'identity': identity, 'inventory': inventory,
            'cost_origin': origin,
            'costs': costs, 'shards': shards,
            'cost_semantics': 'measured scheduling costs, not hosted wall times; '
                              'common dependencies and ABI preparation charged in each shard'}


def hosted_durations(jobs, head, run_id, attempt):
    require(isinstance(jobs, list), 'missing hosted jobs API')
    result = []
    for index in (1, 2):
        matches = [j for j in jobs if j.get('name') == f'clang-shard-{index}']
        require(len(matches) == 1, 'missing or duplicate actual hosted shard job')
        job = matches[0]
        require(job.get('head_sha') == head and str(job.get('run_id')) == run_id and
                str(job.get('run_attempt')) == attempt, 'hosted job source/run/attempt mismatch')
        require(job.get('status') == 'completed' and job.get('conclusion') == 'success',
                'both hosted shards must complete successfully; skip/cancel/red refuses')
        labels = job.get('labels', [])
        require('ubuntu-24.04' in labels and 'self-hosted' not in labels and
                isinstance(job.get('runner_name'), str) and job['runner_name'],
                'not a standard hosted ubuntu-24.04 execution')
        try:
            start = datetime.fromisoformat(job['started_at'].replace('Z', '+00:00'))
            end = datetime.fromisoformat(job['completed_at'].replace('Z', '+00:00'))
            seconds = (end - start).total_seconds()
        except (KeyError, TypeError, AttributeError, ValueError) as error:
            raise EvidenceError('missing completed hosted timing') from error
        require(start.utcoffset() is not None and end.utcoffset() is not None and
                0 < seconds < 900, 'actual cold hosted shard did not finish under 900 seconds')
        result.append(seconds)
    return result


def safe_path(value):
    path = Path(value).absolute()
    legacy._refuse_sensitive_name(str(path))
    require('..' not in path.parts and path.resolve().is_relative_to(Path.cwd().resolve()),
            'path outside the checkout')
    require(not any(p.is_symlink() for p in (path, *path.parents)), 'symlink path refused')
    return path


def write_new(path, data):
    path = safe_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.write(data)


def read_input(path):
    path = safe_path(path)
    require(path.is_file(), 'missing regular input: ' + str(path))
    return path.read_bytes()


def capture(command):
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    require(completed.returncode == 0, 'identity/toolchain command failed: ' + command[0])
    return completed.stdout


def source_identity():
    # No status, environment or credential dump. Tracked bytes and actual Git
    # identity are hashed. An uninitialized gitlink is distinguished from its
    # populated checkout, so those worlds cannot share weights.
    capture(['git', 'diff', '--quiet'])
    capture(['git', 'diff', '--cached', '--quiet'])
    head, tree = capture(['git', 'rev-parse', 'HEAD', 'HEAD^{tree}']).decode().splitlines()
    sha = hashlib.sha256()
    suffixes = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inc',
                '.py', '.cmake', '.csv', '.json', '.pine'}
    def hash_file(path):
        require(path.is_file(), 'source input is absent')
        content = hashlib.sha256()
        with path.open('rb') as data:
            for chunk in iter(lambda: data.read(8 * 1024 * 1024), b''):
                content.update(chunk)
        sha.update(content.digest())
    def visit(prefix):
        git = ['git'] if prefix == '.' else ['git', '-C', prefix]
        for entry in capture([*git, 'ls-files', '--stage', '-z']).split(b'\0'):
            if not entry:
                continue
            metadata, filename = entry.split(b'\t', 1)
            mode, object_id, stage = metadata.split()
            require(stage == b'0', 'unmerged source')
            name = str(Path(prefix) / filename.decode())
            path = safe_path(name)
            sha.update(name.encode() + b'\0' + mode + b'\0')
            if mode == b'160000':
                sha.update(object_id)
                populated = path.is_dir() and (path / '.git').exists()
                sha.update(b'populated' if populated else b'absent')
                if populated:
                    actual = capture(['git', '-C', name, 'rev-parse', 'HEAD']).strip()
                    require(actual == object_id, 'gitlink checkout differs from recorded source')
                    capture(['git', '-C', name, 'diff', '--quiet'])
                    visit(name)
            else:
                hash_file(path)
        # Private overlays are source inputs too. This is part of the same
        # identity, not a separate acceptance layer. Compiled outputs and the
        # protocol's evidence directories never enter this source-input set.
        scope = ['tests', 'include', 'src', 'tutorial', 'cmake'] if prefix == '.' else ['.']
        extra = set()
        for flags in (['--others', '--exclude-standard'],
                      ['--others', '--ignored', '--exclude-standard']):
            extra.update(capture([*git, 'ls-files', *flags, '-z', '--', *scope]).split(b'\0'))
        for filename in sorted(extra - {b''}):
            name = str(Path(prefix) / filename.decode())
            if Path(name).suffix in suffixes:
                sha.update(name.encode() + b'\0overlay\0')
                hash_file(safe_path(name))
    visit('.')
    return identity_checked({'head': head, 'tree': tree, 'files_sha256': sha.hexdigest()})


def toolchain_identity():
    require(platform.system() == 'Linux' and platform.machine() == 'x86_64',
            'cold Clang costs require Linux x86_64')
    # Never silently sanitize a cache or custom compiler setting.
    for key in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS',
                'CMAKE_TOOLCHAIN_FILE', 'CMAKE_C_COMPILER_LAUNCHER',
                'CMAKE_CXX_COMPILER_LAUNCHER', 'CCACHE_DIR', 'SCCACHE_DIR'):
        require(not os.environ.get(key), 'unexpected compiler/cache override: ' + key)
    binaries = {}
    for tool in ('clang-18', 'clang++-18', 'g++-13', 'cmake', 'ctest', 'make', 'python3'):
        path = Path('/usr/bin') / tool
        require(path.is_file(), 'missing toolchain executable: ' + str(path))
        binaries[tool] = digest(path.read_bytes())
    macros = capture(['/usr/bin/clang++-18', GCC, '-std=c++17', '-dM', '-E', '-x', 'c++',
                      '-include', 'bits/c++config.h', '/dev/null']).decode()
    require(re.search(r'(?m)^#define __clang_major__ 18$', macros) and
            re.search(r'(?m)^#define _GLIBCXX_RELEASE 13$', macros) and
            re.search(r'(?m)^#define __cplusplus 201703L$', macros),
            'Clang18/GCC13 headers/C++17 selection failed')
    packages = capture(['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\n',
                        'clang-18', 'g++-13', 'libstdc++-13-dev', 'libstdc++6',
                        'libeigen3-dev', 'tzdata-legacy', 'python3-pytest']).decode()
    return {'architecture': platform.machine(), 'binaries': binaries, 'packages': packages,
            'header_macros_sha256': digest(macros.encode()),
            'os_release_sha256': digest(Path('/etc/os-release').read_bytes()),
            'eigen_config_sha256': digest(Path('/usr/share/eigen3/cmake/Eigen3Config.cmake').read_bytes())}


def execute(command, output, label):
    """Retain the actual status and monotonic clock record, including failures."""
    log_path = output / (label + '.log')
    phase_path = output / (label + '.phase.json')
    start = time.perf_counter_ns()
    with log_path.open('xb') as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for chunk in iter(lambda: process.stdout.read(65536), b''):
            log.write(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
        status = process.wait()
    phase = {'command': command, 'start_ns': start, 'end_ns': time.perf_counter_ns(),
             'exit_status': status}
    write_new(phase_path, encoded(phase))
    return record(encoded(phase)), record(log_path.read_bytes())


def parse_ctest_recipes(doc):
    """Read CMake's generated, single-config CTest recipe grammar literally.

    ctest -N can omit a command before its executable exists. Those commands
    come from these actual recipes; no placeholder executable is manufactured.
    Unsupported generated directives refuse rather than guessing.
    """
    require(isinstance(doc, dict) and 'CTestTestfile.cmake' in doc, 'missing root CTest recipe')
    tests, reached = {}, set()
    def statements(text):
        cursor = 0
        while cursor < len(text):
            whitespace = re.match(r'\s+', text[cursor:])
            if whitespace:
                cursor += whitespace.end()
                continue
            if text[cursor] == '#':
                end = text.find('\n', cursor)
                cursor = len(text) if end < 0 else end + 1
                continue
            header = re.match(r'(add_test|set_tests_properties|subdirs)\(', text[cursor:])
            require(header is not None, 'unsupported generated CTest directive')
            cursor += header.end()
            values = []
            while True:
                whitespace = re.match(r'\s*', text[cursor:])
                cursor += whitespace.end()
                require(cursor < len(text), 'unterminated CTest recipe')
                if text[cursor] == ')':
                    cursor += 1
                    break
                bracket = re.match(r'\[(=*)\[', text[cursor:])
                if bracket:
                    start = cursor + bracket.end()
                    terminator = ']' + bracket[1] + ']'
                    end = text.find(terminator, start)
                    require(end >= 0, 'unterminated bracket argument')
                    value, cursor = text[start:end], end + len(terminator)
                elif text[cursor] == '"':
                    quoted = re.match(r'"((?:\\.|[^"\\])*)"', text[cursor:], re.DOTALL)
                    require(quoted is not None, 'unterminated quoted CTest argument')
                    raw, decoded, at = quoted[1], [], 0
                    while at < len(raw):
                        char = raw[at]
                        at += 1
                        if char == '\\':
                            require(at < len(raw), 'dangling CTest escape')
                            char = raw[at]
                            at += 1
                            require(not char.isalnum() or char in 'nrt',
                                    'unsupported CTest recipe escape')
                            char = {'n': '\n', 'r': '\r', 't': '\t'}.get(char, char)
                        decoded.append(char)
                    value = ''.join(decoded)
                    cursor += quoted.end()
                else:
                    token = re.match(r'[^\s()"\\]+', text[cursor:])
                    require(token is not None, 'unsupported CTest argument')
                    value = token[0]
                    cursor += token.end()
                values.append(value)
            yield header[1], values
    def visit(filename):
        require(filename not in reached and filename in doc, 'missing or duplicate CTest subdirectory')
        reached.add(filename)
        text = doc[filename]
        require(isinstance(text, str), 'malformed CTest recipe bytes')
        for directive, values in statements(text):
            if directive == 'add_test':
                require(len(values) >= 2, 'empty generated CTest command')
                name, command = values[0], values[1:]
                names([name], 'generated test identifier')
                require(name not in tests, 'duplicate generated CTest recipe')
                tests[name] = command
            elif directive == 'subdirs':
                require(len(values) == 1 and not Path(values[0]).is_absolute() and
                        all(p not in ('', '.', '..') for p in values[0].split('/')),
                        'unsafe CTest subdirectory')
                visit(str(Path(filename).parent / values[0] / 'CTestTestfile.cmake'))
            else:
                require('PROPERTIES' in values, 'malformed generated test properties')
                at = values.index('PROPERTIES')
                require(all(name in tests for name in values[:at]) and
                        (len(values) - at - 1) % 2 == 0, 'unbound generated test properties')
    visit('CTestTestfile.cmake')
    require(reached == set(doc), 'unreachable supplied CTest recipe')
    return tests


def fresh_configure(output):
    require(not Path('build').exists(), 'cold run requires an absent build directory')
    identity = source_identity()
    expected = os.environ.get('GITHUB_SHA')
    require(not expected or expected == identity['head'], 'checkout is not the exact event HEAD')
    toolchain = toolchain_identity()
    query = safe_path('build/.cmake/api/v1/query')
    query.mkdir(parents=True)
    write_new(query / 'codemodel-v2', b'')
    phase, log = execute(['/usr/bin/cmake', '-S', '.', '-B', 'build', *CONFIG], output, 'configure')
    phase_seconds(phase, ['/usr/bin/cmake', '-S', '.', '-B', 'build', *CONFIG])
    reply = safe_path('build/.cmake/api/v1/reply')
    indexes = list(reply.glob('index-*.json'))
    require(len(indexes) == 1, 'ambiguous CMake file-api index')
    index = loads(indexes[0].read_bytes())
    model_ref = index['reply']['codemodel-v2']['jsonFile']
    require(re.fullmatch(r'[A-Za-z0-9_.+-]+\.json', model_ref), 'unsafe codemodel reference')
    model_bytes = read_input(reply / model_ref)
    model = loads(model_bytes)
    configs = [c for c in model['configurations'] if c['name'] == 'Release']
    require(len(configs) == 1, 'ambiguous configured Release model')
    records = {
        'source-before': record(encoded(identity)), 'toolchain': record(encoded(toolchain)),
        'configure': phase, 'configure-log': log, 'codemodel': record(model_bytes),
        'makefile2': record(read_input('build/CMakeFiles/Makefile2')),
        'cache': record(read_input('build/CMakeCache.txt')),
    }
    for target in configs[0]['targets']:
        ref = target['jsonFile']
        require(re.fullmatch(r'[A-Za-z0-9_.+-]+\.json', ref), 'unsafe target reference')
        records['target:' + target['name']] = record(read_input(reply / ref))
    records['ctest'] = record(capture(['/usr/bin/ctest', '--test-dir', 'build', '--show-only=json-v1']))
    recipes = {}
    for path in sorted(Path('build').rglob('CTestTestfile.cmake')):
        recipes[path.relative_to('build').as_posix()] = read_input(path).decode()
    records['ctest-recipes'] = record(encoded(recipes))
    inventory = configured_inventory(records, identity)
    # CMake is allowed to place tutorial DSOs in the source tree. They must
    # also be absent before the build; a fresh build dir alone is insufficient.
    for detail in inventory['target_details'].values():
        for artifact in detail['artifacts']:
            actual = artifact.replace('${BUILD}', str(Path('build').absolute())).replace(
                '${SOURCE}', str(Path.cwd()))
            require(not Path(actual).exists(), 'pre-existing compiled artifact in a cold run')
    write_new(output / 'inventory.json', encoded(inventory))
    return identity, records, inventory


def runtime_context():
    return {key: os.environ.get(key, '') for key in (
        'GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
        'GITHUB_JOB', 'GITHUB_ACTIONS', 'RUNNER_ENVIRONMENT', 'RUNNER_OS', 'RUNNER_ARCH')}


def configured_record_keys(records):
    model = decode(records['codemodel'])
    configs = [row for row in model['configurations'] if row['name'] == 'Release']
    require(len(configs) == 1, 'missing or duplicate configured Release')
    return {
        'source-before', 'toolchain', 'configure', 'configure-log',
        'codemodel', 'makefile2', 'cache', 'ctest', 'ctest-recipes',
        *['target:' + row['name'] for row in configs[0]['targets']],
    }


def collect(output):
    identity, records, inventory = fresh_configure(output)
    build_rows, test_rows, refusals = [], [], {}
    edges = {n: set(d['dependencies']) for n, d in inventory['target_details'].items()}
    for name in ordered(edges):
        phase, log = execute(build_command(inventory, [name]), output, 'build-' + name)
        records['build:' + name], records['build-log:' + name] = phase, log
        try:
            seconds = phase_seconds(phase, build_command(inventory, [name]))
            build_rows.append({'target': name, 'seconds': seconds})
        except EvidenceError as error:
            refusals['target:' + name] = str(error)
    records['prepare'], records['prepare-log'] = execute(prepare_command(), output, 'prepare')
    for name in ordered(test_edges(inventory)):
        junit = 'build/clang-test-' + name + '.xml'
        trace = 'build/clang-test-' + name + '.trace'
        command = test_command([name], junit, trace=trace)
        phase, log = execute(command, output, 'test-' + name)
        records['test:' + name], records['test-log:' + name] = phase, log
        try:
            phase_seconds(phase, command)
            records['junit:' + name] = record(read_input(junit))
            records['trace:' + name] = record(read_input(trace))
            seconds = junit_rows(unrecord(records['junit:' + name]), [name])[name]
            targets = trace_targets(unrecord(records['trace:' + name]), inventory, name)
            test_rows.append({'test': name, 'seconds': seconds, 'build_targets': targets})
        except (EvidenceError, OSError) as error:
            refusals['test:' + name] = str(error)
    records['source-after'] = record(encoded(source_identity()))
    doc = {'schema': COST_SCHEMA, 'identity': identity, 'inventory': inventory,
           'records': records, 'build_rows': build_rows, 'test_rows': test_rows,
           'context': runtime_context()}
    write_new(output / 'collection-records.json', encoded(doc))
    write_new(output / 'refusals.json', encoded(refusals))
    require(not refusals, 'cost collection has failed, skipped or unresolved rows', refusals)
    validate_costs(doc, inventory, identity)
    write_new(output / 'costs.json', encoded(doc))
    print('costs.json SHA256: ' + digest(encoded(doc)))


def verify_plan(plan):
    require(isinstance(plan, dict) and plan.get('schema') == PLAN_SCHEMA, 'missing complete v2 plan')
    cost_origin_checked(plan['costs'], plan['cost_origin'])
    actual = make_plan(plan['costs'], plan['inventory'], plan['identity'], plan['cost_origin'])
    require(plan == actual, 'plan differs from the actual measured records')
    return plan


def run_shard(plan_bytes, shard_id, output):
    plan = verify_plan(loads(plan_bytes))
    identity, records, inventory = fresh_configure(output)
    require(identity == plan['identity'] and inventory == plan['inventory'],
            'shard source/configured population/toolchain differs from plan')
    assigned = plan['shards'][shard_id - 1]
    command = build_command(inventory, assigned['build_targets'])
    records['build'], records['build-log'] = execute(command, output, 'build')
    phase_seconds(records['build'], command)
    records['prepare'], records['prepare-log'] = execute(prepare_command(), output, 'prepare')
    phase_seconds(records['prepare'], prepare_command())
    command = test_command(assigned['tests'], 'build/clang-shard.xml')
    records['test'], records['test-log'] = execute(command, output, 'ctest')
    # Retain failed JUnit as well; never produce a success result on failure.
    records['junit'] = record(read_input('build/clang-shard.xml'))
    write_new(output / 'execution-records.json', encoded(records))
    phase_seconds(records['test'], command)
    junit_rows(unrecord(records['junit']), assigned['tests'])
    records['source-after'] = record(encoded(source_identity()))
    require(decode(records['source-after']) == identity, 'shard modified tracked source')
    result = {'schema': RESULT_SCHEMA, 'identity': identity, 'shard': shard_id,
              'plan_sha256': digest(plan_bytes), 'inventory': inventory,
              'records': records, 'context': runtime_context()}
    validate_result(result, plan, digest(plan_bytes), None)
    write_new(output / 'result.json', encoded(result))


def validate_result(result, plan, plan_sha256, run):
    keys(result, ('schema', 'identity', 'shard', 'plan_sha256', 'inventory',
                  'records', 'context'), 'execution result')
    require(result['schema'] == RESULT_SCHEMA and type(result['shard']) is int and
            result['shard'] in (1, 2), 'invalid actual shard result')
    require(result['identity'] == plan['identity'] and result['plan_sha256'] == plan_sha256,
            'actual shard result is from a different source or plan')
    records = result['records']
    keys(records, configured_record_keys(records) | {
        'source-after', 'build', 'build-log', 'prepare', 'prepare-log',
        'test', 'test-log', 'junit'}, 'complete execution records')
    for value in records.values():
        unrecord(value)
    keys(result['context'], runtime_context().keys(), 'execution context')
    inventory = configured_inventory(records, result['identity'])
    require(result['inventory'] == inventory == plan['inventory'], 'actual configured set differs')
    require(decode(records['source-before']) == decode(records['source-after']) == plan['identity'],
            'actual source changed during execution')
    shard = plan['shards'][result['shard'] - 1]
    phase_seconds(records['build'], build_command(inventory, shard['build_targets']))
    built = [m[1] for line in unrecord(records['build-log']).decode().splitlines()
             if (m := BUILT.fullmatch(line))]
    require(Counter(built) == Counter(shard['build_targets']),
            'actual build log misses or duplicates a configured target')
    phase_seconds(records['prepare'], prepare_command())
    phase_seconds(records['test'], test_command(shard['tests'], 'build/clang-shard.xml'))
    actual = junit_rows(unrecord(records['junit']), shard['tests'])
    if run is not None:
        context = result['context']
        for key, value in {
                'GITHUB_REPOSITORY': 'pineforge-4pass/pineforge-engine',
                'GITHUB_SHA': plan['identity']['head'], 'GITHUB_RUN_ID': str(run['id']),
                'GITHUB_RUN_ATTEMPT': str(run['run_attempt']),
                'GITHUB_JOB': f"clang-shard-{result['shard']}",
                'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
                'RUNNER_OS': 'Linux', 'RUNNER_ARCH': 'X64'}.items():
            require(context.get(key) == value, 'actual shard hosted identity mismatch: ' + key)
    return actual, built


def check(plan_bytes, results, jobs, run, needs, identity):
    plan = verify_plan(loads(plan_bytes))
    require(identity == plan['identity'], 'aggregate is not on the exact planned source')
    require(run.get('head_sha') == identity['head'] and
            run.get('path') == '.github/workflows/clang-build.yml' and
            run.get('event') in ('push', 'workflow_dispatch') and
            run.get('repository', {}).get('full_name') == 'pineforge-4pass/pineforge-engine',
            'aggregate API run identity mismatch')
    require(set(needs) == {'authz', 'collect', 'plan', 'clang-shard-1', 'clang-shard-2'} and
            all(value == 'success' for value in needs.values()),
            'aggregate requires both shards and every prerequisite to succeed')
    require(len(results) == 2 and Counter(r.get('shard') for r in results) == Counter([1, 2]),
            'both different actual shard artifacts are mandatory')
    durations = hosted_durations(jobs, identity['head'], str(run['id']), str(run['run_attempt']))
    executed, built = [], set()
    for result in results:
        actual, actual_built = validate_result(result, plan, digest(plan_bytes), run)
        executed.extend(actual)
        built.update(actual_built)
    require(Counter(executed) == Counter(plan['inventory']['tests'].keys()),
            'two actual executions do not equal the full configured test set exactly once')
    require(built == set(plan['inventory']['targets']), 'actual default ALL build union differs')
    return {'schema': PROOF_SCHEMA, 'identity': identity, 'run_id': run['id'],
            'run_attempt': run['run_attempt'], 'plan_sha256': digest(plan_bytes),
            'executed_tests': sorted(executed), 'actual_test_count': len(executed),
            'built_targets': sorted(built), 'hosted_job_seconds': durations,
            'result_sha256': [digest(encoded(result)) for result in results]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    collection = sub.add_parser('collect')
    collection.add_argument('--output', required=True)
    planner = sub.add_parser('plan')
    planner.add_argument('--costs', required=True)
    planner.add_argument('--costs-sha256', required=True)
    planner.add_argument('--cost-run', required=True)
    planner.add_argument('--cost-jobs', required=True)
    planner.add_argument('--cost-run-id', required=True)
    planner.add_argument('--cost-run-attempt', required=True)
    planner.add_argument('--output', required=True)
    runner = sub.add_parser('run')
    runner.add_argument('--plan', required=True)
    runner.add_argument('--shard', type=int, choices=(1, 2), required=True)
    runner.add_argument('--output', required=True)
    checker = sub.add_parser('check')
    checker.add_argument('--plan', required=True)
    checker.add_argument('--result', action='append', required=True)
    checker.add_argument('--jobs', required=True)
    checker.add_argument('--run', required=True)
    checker.add_argument('--needs', required=True)
    checker.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    try:
        output = safe_path(args.output)
        if args.command != 'check':
            output.mkdir(parents=True, exist_ok=False)
        if args.command == 'collect':
            collect(output)
        elif args.command == 'plan':
            raw = read_input(args.costs)
            legacy._sha(args.costs_sha256, 64, 'expected cost artifact member digest')
            require(digest(raw) == args.costs_sha256, 'cost member is not the pinned original bytes')
            identity, records, inventory = fresh_configure(output)
            run = loads(read_input(args.cost_run))
            require(str(run.get('id')) == args.cost_run_id and
                    str(run.get('run_attempt')) == args.cost_run_attempt,
                    'collection run/attempt is not the selected artifact origin')
            plan = make_plan(loads(raw), inventory, identity,
                             {'run': run, 'sha256': args.costs_sha256,
                              'jobs': [job for page in loads(read_input(args.cost_jobs))
                                       for job in page['jobs']]})
            # The configured source is also verified by each shard. Planning
            # records are retained separately to keep the plan deterministic.
            write_new(output / 'planning-records.json', encoded(records))
            write_new(output / 'plan.json', encoded(plan))
        elif args.command == 'run':
            run_shard(read_input(args.plan), args.shard, output)
        else:
            jobs_pages = loads(read_input(args.jobs))
            require(isinstance(jobs_pages, list), 'expected paginated jobs API array')
            jobs = [job for page in jobs_pages for job in page['jobs']]
            raw_needs = loads(args.needs)
            needs = {key: value['result'] for key, value in raw_needs.items()}
            proof = check(read_input(args.plan), [loads(read_input(p)) for p in args.result],
                          jobs, loads(read_input(args.run)), needs, source_identity())
            write_new(output, encoded(proof))
    except (EvidenceError, OSError, KeyError, TypeError, ValueError) as error:
        print('clang cold shards: REFUSED: ' + str(error), file=sys.stderr)
        if isinstance(error, EvidenceError) and error.details:
            print(json.dumps(error.details, sort_keys=True), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
