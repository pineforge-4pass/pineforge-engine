#!/usr/bin/env python3
"""Build the configured lab Release target population, without running CTest."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import ci_verify


def json_object(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate JSON key: ' + key)
            result[key] = value
        return result

    value = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError('inventory must be a JSON object')
    return value


def names(values, *, empty=False):
    if (not isinstance(values, list) or (not values and not empty)
            or any(not isinstance(value, str) or not value or value.strip() != value
                   or any(char in value for char in '\r\n\0') for value in values)
            or len(values) != len(set(values)) or values != sorted(values)):
        raise ValueError('inventory names must be sorted, unique, nonempty single lines')
    return values


def registered_names(raw):
    text = raw.decode('utf-8')
    registry = ci_verify.ctest_registry(raw)
    rows = list(ci_verify.CTEST_LIST_NAME.finditer(text))
    if (len(ci_verify.CTEST_LIST_COUNT.findall(text)) != 1
            or [int(row['number']) for row in rows] != list(range(1, len(rows) + 1))
            or len(re.findall(r'(?m)^\s*Test\s+#', text)) != len(rows)):
        raise ValueError('malformed CTest discovery rows or summary')
    names(sorted(registry))
    return registry


def relative_artifact(raw, build):
    if not isinstance(raw, str) or not raw or any(char in raw for char in '\r\n\0'):
        raise ValueError('malformed target artifact')
    path = Path(raw)
    path = (path if path.is_absolute() else build / path).resolve()
    if build not in path.parents:
        raise ValueError('target artifact escapes the configured build: ' + raw)
    return path.relative_to(build).as_posix()


def release_targets(build):
    """CMake file-api inventory, independent of CTest and generated link parsing."""
    reply = build / '.cmake/api/v1/reply'
    indexes = list(reply.glob('index-*.json'))
    if len(indexes) != 1:
        raise ValueError('expected exactly one readable CMake file-api index')

    def read_reply(filename):
        if not isinstance(filename, str) or Path(filename).name != filename:
            raise ValueError('malformed CMake reply filename')
        return json_object((reply / filename).read_bytes())

    index = json_object(indexes[0].read_bytes())
    model_ref = index['reply']['codemodel-v2']
    if model_ref['kind'] != 'codemodel' or model_ref['version']['major'] != 2:
        raise ValueError('unsupported CMake codemodel')
    model = read_reply(model_ref['jsonFile'])
    configurations = model['configurations']
    if not isinstance(configurations, list) or len(configurations) != 1:
        raise ValueError('expected the single configured Release inventory')
    configuration = configurations[0]
    if configuration['name'] != 'Release':
        raise ValueError('target inventory is not Release')
    refs = configuration['targets']
    if not isinstance(refs, list) or not refs:
        raise ValueError('empty CMake target inventory')
    seen_ids, seen_names, targets, outputs = set(), set(), {}, set()
    for ref in refs:
        target_id, name = ref['id'], ref['name']
        if (not isinstance(target_id, str) or not target_id
                or not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_.+-]+', name)
                or target_id in seen_ids or name in seen_names):
            raise ValueError('duplicate or malformed CMake target row')
        seen_ids.add(target_id)
        seen_names.add(name)
        target = read_reply(ref['jsonFile'])
        if target['id'] != target_id or target['name'] != name:
            raise ValueError('CMake target reference differs from its reply')
        if target['type'] in {'UTILITY', 'INTERFACE_LIBRARY'}:
            continue
        if target['type'] not in {'EXECUTABLE', 'STATIC_LIBRARY', 'SHARED_LIBRARY',
                                  'MODULE_LIBRARY', 'OBJECT_LIBRARY'}:
            raise ValueError('unsupported compiled target type: ' + target['type'])
        artifacts = target.get('artifacts')
        if not isinstance(artifacts, list) or not artifacts:
            raise ValueError('compiled target has no artifacts: ' + name)
        paths = [relative_artifact(artifact['path'], build) for artifact in artifacts]
        if len(paths) != len(set(paths)) or outputs.intersection(paths):
            raise ValueError('duplicate target artifact')
        outputs.update(paths)
        targets[name] = sorted(paths)
    names(sorted(targets))
    return dict(sorted(targets.items()))


def validate_plan(plan):
    if not isinstance(plan, dict):
        raise ValueError('missing build plan')
    registry = names(plan['registeredNames'])
    targets = plan['targets']
    if not isinstance(targets, dict):
        raise ValueError('malformed Release target inventory')
    names(sorted(targets))
    all_outputs = []
    for target, paths in targets.items():
        if not re.fullmatch(r'[A-Za-z0-9_.+-]+', target):
            raise ValueError('malformed build target')
        for path in names(paths):
            if Path(path).is_absolute() or '..' in Path(path).parts:
                raise ValueError('malformed build output')
            all_outputs.append(path)
    if len(all_outputs) != len(set(all_outputs)):
        raise ValueError('duplicate build output')
    mapping = plan['targetMap']
    if not isinstance(mapping, dict) or sorted(mapping) != registry:
        raise ValueError('CTest and mapped row populations differ')
    for row_targets in mapping.values():
        if not set(names(row_targets, empty=True)) <= set(targets):
            raise ValueError('registered row has an unknown target producer')
    if plan['rowsWithoutBuildTargets'] != sorted(name for name in registry if not mapping[name]):
        raise ValueError('a row without a compiled producer was counted as built')


def check_union(records, count):
    if (type(count) is not int or count < 1 or not isinstance(records, list)
            or len(records) != count):
        raise ValueError('missing or extra build shard')
    plan = records[0]['plan']
    validate_plan(plan)
    indexes, built = [], []
    for record in records:
        if (record['plan'] != plan or type(record['index']) is not int
                or type(record['count']) is not int or record['count'] != count):
            raise ValueError('build shards disagree on the Release population')
        index = record['index']
        indexes.append(index)
        selected = names(record['builtTargets'])
        if selected != sorted(plan['targets'])[index - 1::count]:
            raise ValueError('built target population differs from assigned targets')
        expected_outputs = sorted(path for target in selected for path in plan['targets'][target])
        outputs = record['outputs']
        if (not isinstance(outputs, dict) or sorted(outputs) != expected_outputs
                or any(not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value)
                       for value in outputs.values())):
            raise ValueError('missing, extra or unreadable built output')
        built.extend(selected)
    if sorted(indexes) != list(range(1, count + 1)):
        raise ValueError('duplicate or missing shard index')
    if Counter(built) != Counter(plan['targets'].keys()):
        raise ValueError('Release target inventory and actual built target union differ')
    return plan


def command(argv, log, *, cwd):
    # No shell, pipeline or successful later command can mask the build status.
    print('+ ' + repr(argv), flush=True)
    with log.open('wb') as output:
        result = subprocess.run(argv, cwd=cwd, stdout=output, stderr=subprocess.STDOUT)
    raw = log.read_bytes()
    print(raw.decode('utf-8', errors='replace'), end='', flush=True)
    result.check_returncode()
    return raw


def build_shard(args):
    source, build = args.source.resolve(), args.build_dir.resolve()
    index, count = map(int, args.shard.split('/'))
    if not 1 <= index <= count <= 8 or args.jobs < 1:
        raise ValueError('require 1 <= shard index <= count <= 8 and positive jobs')
    artifacts = args.artifact_dir.resolve() / f'shard-{index}'
    artifacts.mkdir(parents=True, exist_ok=True)
    receipt = artifacts / 'built.json'
    # An interrupted/failed second invocation must never leave a successful
    # prior invocation's record available to the union.
    receipt.write_text('{}\n')
    started = time.monotonic()
    query = build / '.cmake/api/v1/query/codemodel-v2'
    query.parent.mkdir(parents=True, exist_ok=True)
    query.touch()
    cfg = ci_verify.VerifyConfig(
        profile=ci_verify.PROFILE['release'], source=source, build_dir=build,
        jobs=args.jobs, generator='Ninja', curl_dir=None,
        ccache_path=shutil_ccache(), require_websocket=False, runner=None)
    command(ci_verify.cmake_configure_argv(cfg), artifacts / 'configure.log', cwd=source)

    def discover(label):
        raw = command(['ctest', '--test-dir', str(build), '-N'],
                      artifacts / f'ctest-{label}.txt', cwd=source)
        registry = registered_names(raw)
        document = command(['ctest', '--test-dir', str(build), '-N', '--show-only=json-v1'],
                           artifacts / f'ctest-{label}.json', cwd=source)
        parsed = json_object(document)
        # ctest -N omits executable paths before they exist. The generated
        # add_test commands supply those paths; they must name exactly the same rows.
        if set(ci_verify.ctest_configure_commands(build)) != set(registry):
            raise ValueError('generated CTest names differ from ctest -N')
        inventory = ci_verify.ctest_build_inventory(
            json.dumps(parsed).encode(), build, registry, registry)
        targets = release_targets(build)
        # Compare file-api products with independent generated link producers.
        link_outputs = inventory['configuredTargetOutputs']
        for path, target in link_outputs.items():
            if target not in targets or path not in targets[target]:
                raise ValueError('generated link inventory differs from CMake target inventory')
        for target, paths in targets.items():
            for path in paths:
                if not path.endswith(('.o', '.obj')) and link_outputs.get(path) != target:
                    raise ValueError('CMake target has no generated link producer: ' + target)
        plan = {
            'registeredNames': sorted(registry),
            'targetMap': inventory['targetMap'],
            'rowsWithoutBuildTargets': sorted(name for name in registry
                                             if not inventory['targetMap'][name]),
            'targets': targets,
        }
        validate_plan(plan)
        (artifacts / f'inventory-{label}.json').write_text(json.dumps(plan, indent=2) + '\n')
        return plan

    plan = discover('before')
    selected = sorted(plan['targets'])[index - 1::count]
    if not selected:
        raise ValueError('build shard has no targets; reduce the shard count')
    command(['cmake', '--build', str(build), '--parallel', str(args.jobs),
             '--target', *selected], artifacts / 'build.log', cwd=source)
    outputs = {}
    for target in selected:
        for path in plan['targets'][target]:
            product = build / path
            if not product.is_file() or product.stat().st_size == 0:
                raise ValueError('build did not produce ' + path)
            outputs[path] = hashlib.sha256(product.read_bytes()).hexdigest()
    if discover('after') != plan:
        raise ValueError('CTest or Release build target population changed during compilation')
    record = {'plan': plan, 'index': index, 'count': count,
              'builtTargets': selected, 'outputs': outputs,
              'elapsedSeconds': time.monotonic() - started}
    receipt.write_text(json.dumps(record, indent=2) + '\n')
    print(f'Built {len(selected)} assigned Release targets; CTest execution is separate.', flush=True)


def shutil_ccache():
    import shutil
    path = shutil.which('ccache')
    if path is None:
        raise ValueError('the existing CI compiler launcher ccache is required')
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest='command', required=True)
    build = subparsers.add_parser('build')
    build.add_argument('--source', type=Path, default=Path('.'))
    build.add_argument('--build-dir', type=Path, required=True)
    build.add_argument('--artifact-dir', type=Path, required=True)
    build.add_argument('--jobs', type=int, required=True)
    build.add_argument('--shard', required=True)
    union = subparsers.add_parser('union')
    union.add_argument('--artifact-dir', type=Path, required=True)
    union.add_argument('--shards', type=int, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'build':
            build_shard(args)
        else:
            paths = sorted(args.artifact_dir.glob('shard-*/built.json'))
            plan = check_union([json_object(path.read_bytes()) for path in paths], args.shards)
            print(f'Release build union: {len(plan["targets"])} exact targets; '
                  f'{len(plan["registeredNames"])} mapped CTest rows; '
                  f'{len(plan["rowsWithoutBuildTargets"])} rows have no compiled producer '
                  'and are not counted as built.')
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print('Release test build FAILED: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
