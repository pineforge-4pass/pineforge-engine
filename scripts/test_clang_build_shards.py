#!/usr/bin/env python3
"""Synthetic parser/coverage tests; these do not qualify a hosted build.

Run only off-master. Fixtures and CLI outputs are kept in .lane-notes/scratch;
the real archive parse and genuine mutation controls belong to the parent.
"""
from __future__ import annotations

import contextlib
import copy
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
from pathlib import Path
import unittest
import uuid
import warnings
import zipfile

import clang_build_shards as shards


OLD_HEAD = '1' * 40
OLD_TREE = '2' * 40
HEAD = '3' * 40
TREE = '4' * 40
RUN = '12345'
JOB = 'clang-build'
ARTIFACT_ID = '67890'
SCRATCH = Path(__file__).resolve().parents[1] / '.lane-notes' / 'scratch'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def stamp(seconds):
    value = datetime(2026, 11, 8, tzinfo=timezone.utc) + timedelta(seconds=seconds)
    return value.strftime('%Y-%m-%dT%H:%M:%S.%f') + '0Z'


def log(records):
    return ''.join(f'{stamp(seconds)} {message}\n'
                   for seconds, message in records).encode('utf-8')


def phase(wall=60, exit_status=0):
    return (f'wall_seconds={wall}\nuser_seconds=2.50\n'
            f'system_seconds=0.25\nexit_status={exit_status}\n').encode()


def archive(members):
    stream = io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', UserWarning)
        with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED) as output:
            for name, data in members:
                output.writestr(name, data)
    return stream.getvalue()


def fixture(weights=None):
    """Build both real ZIP formats, with interleaved overlapping target windows."""
    if weights is None:
        weights = {'a': 8, 'b': 7, 'c': 6, 'd': 5}
    events = [(1, f'[  0%] Building CXX object tests/CMakeFiles/{name}.dir/{name}.cpp.o')
              for name in sorted(weights)]
    events += [(1 + seconds, f'[100%] Built target {name}')
               for name, seconds in weights.items()]
    events.sort(key=lambda row: (row[0], row[1]))
    configure_output = [
        (0.25, '-- Configuring done (0.1s)'),
        (0.5, '-- Generating done (0.1s)'),
        (0.75, '-- Build files have been written to: /build'),
    ]
    artifact = {
        'identity-before.log': f'{OLD_HEAD}\n{OLD_TREE}\n'.encode(),
        'identity-after.log': f'{OLD_HEAD}\n{OLD_TREE}\n'.encode(),
        'configure.time': phase(1),
        'build.time': phase(),
        'configure.log': ''.join(f'{message}\n' for _, message in configure_output).encode(),
        'build.log': ''.join(f'{message}\n' for _, message in events).encode(),
        'started-at.log': b'2026-11-08T00:00:00+00:00\n',
        'finished-at.log': b'2026-11-08T00:01:02+00:00\n',
    }
    logs = {
        f'{JOB}/1_Set up job.txt': log([(0, f'Complete job name: {JOB}')]),
        f'{JOB}/4_Configure Release.txt': log([
            (0, '##[group]Run configure'), (0, '##[endgroup]'), *configure_output]),
        f'{JOB}/5_Build all targets.txt': log([
            (1, '##[group]Run build'), (1, '##[endgroup]'), *events]),
        f'{JOB}/6_Record final identity and evidence.txt': log([
            (62, OLD_HEAD), (62, OLD_TREE)]),
        f'{JOB}/15_Complete job.txt': log([(64, 'Cleaning up orphan processes')]),
    }
    return logs, artifact


def pack(logs, artifacts):
    artifact_bytes = archive(list(artifacts.items()))
    logs = dict(logs)
    logs.setdefault(f'{JOB}/7_Upload build logs.txt', log([
        (63, f'SHA256 digest of uploaded artifact zip is {digest(artifact_bytes)}'),
        (63, f'Artifact synthetic.zip successfully finalized. Artifact ID {ARTIFACT_ID}'),
        (63, 'Artifact download URL: https://github.com/example/engine/actions/'
             f'runs/{RUN}/artifacts/{ARTIFACT_ID}'),
    ]))
    return archive(list(logs.items())), artifact_bytes


def observe_pair(pair, **overrides):
    logs, artifacts = pair
    arguments = dict(logs_sha256=digest(logs), artifacts_sha256=digest(artifacts),
                     run_id=RUN, job=JOB, head=OLD_HEAD, tree=OLD_TREE)
    arguments.update(overrides)
    return shards.observe_archives(logs, artifacts, **arguments)


def measured(weights=None):
    return observe_pair(pack(*fixture(weights)))


def inventory(targets=('a', 'b', 'c', 'd')):
    return {
        'schema': 'clang-build-inventory-v1',
        'head': HEAD, 'tree': TREE, 'targets': list(targets),
        'provenance': {'method': 'synthetic generated-graph fixture'},
    }


def source_text(member, text, archive_sha256='5' * 64):
    return {'archive_sha256': archive_sha256, 'member': member,
            'sha256': digest(text.encode()), 'text': text}


def result_manifest(shard, targets, prerequisites=()):
    built = list(targets) + list(prerequisites)
    text = log([(10 + index, f'[100%] Built target {name}')
                for index, name in enumerate(built)]).decode()
    return {
        'schema': 'clang-build-shard-result-v1',
        'head': HEAD, 'tree': TREE, 'shard': shard,
        'targets': list(targets), 'prerequisites': list(prerequisites),
        'result': {
            'head': HEAD, 'tree': TREE,
            'status': 'success', 'exit_status': 0,
            'run_id': '24680', 'job': f'clang-shard-{shard}',
            'build_log': source_text(f'clang-shard-{shard}/build.txt', text),
            'build_time': source_text('build.time', phase().decode()),
        },
    }


class ObserveTests(unittest.TestCase):
    def test_overlapping_windows_keep_exact_source_and_authoritative_totals(self):
        logs, artifact = fixture({'a': 8, 'b': 5})
        pair = pack(logs, artifact)
        observed = observe_pair(pair)
        rows = {row['target']: row for row in observed['observations']}
        self.assertEqual((rows['a']['seconds'], rows['b']['seconds']), (8, 5))
        self.assertEqual(rows['a']['first_line'], 3)
        self.assertEqual(rows['a']['last_line'], 6)
        self.assertEqual(rows['a']['first_timestamp'], stamp(1))
        self.assertEqual(rows['a']['last_timestamp'], stamp(9))
        provenance = rows['a']['provenance']
        self.assertEqual(provenance['head'], OLD_HEAD)
        self.assertEqual(provenance['tree'], OLD_TREE)
        self.assertEqual(provenance['run_id'], RUN)
        self.assertEqual(provenance['job'], JOB)
        self.assertEqual(provenance['logs_sha256'], digest(pair[0]))
        self.assertEqual(provenance['artifacts_sha256'], digest(pair[1]))
        self.assertEqual(provenance['member'], f'{JOB}/5_Build all targets.txt')
        self.assertEqual(provenance['member_sha256'],
                         digest(logs[provenance['member']]))
        self.assertIn('overlapping', observed['weight_semantics'])
        self.assertEqual(observed['phase_totals']['build']['wall_seconds'], 60)
        self.assertEqual(observed['phase_totals']['configure']['wall_seconds'], 1)
        self.assertNotEqual(sum(row['seconds'] for row in rows.values()), 60)

    def test_first_own_object_excludes_dependency_and_later_objects(self):
        logs, artifacts = fixture({'a': 8, 'dep': 3})
        records = [
            (1, '[  0%] Building CXX object CMakeFiles/dep.dir/dep.cpp.o'),
            (2, '[  0%] Building CXX object tests/CMakeFiles/a.dir/a.cpp.o'),
            (3, '[  0%] Building CXX object tests/CMakeFiles/a.dir/another.cpp.o'),
            (4, '[ 50%] Built target dep'), (9, '[100%] Built target a'),
        ]
        logs[f'{JOB}/5_Build all targets.txt'] = log([
            (1, '##[group]Run build'), (1, '##[endgroup]'), *records])
        artifacts['build.log'] = ''.join(f'{message}\n' for _, message in records).encode()
        observed = observe_pair(pack(logs, artifacts))
        row = next(row for row in observed['observations'] if row['target'] == 'a')
        self.assertEqual(row['seconds'], 7)
        self.assertEqual(row['first_line'], 4)

    def test_unmeasured_ambiguous_and_zero_windows_remain_explicit(self):
        records = [
            (1, '[  0%] Building CXX object CMakeFiles/zero.dir/a.cpp.o'),
            (1, '[100%] Built target zero'),
            (2, '[100%] Built target link_only'),
            (3, '[  0%] Building CXX object CMakeFiles/unfinished.dir/a.cpp.o'),
            (4, '[  0%] Building CXX object CMakeFiles/repeated.dir/a.cpp.o'),
            (5, '[100%] Built target repeated'),
            (6, '[100%] Built target repeated'),
            (7, '[  0%] Building CXX object one/CMakeFiles/clash.dir/a.cpp.o'),
            (8, '[  0%] Building CXX object two/CMakeFiles/clash.dir/b.cpp.o'),
            (9, '[100%] Built target clash'),
        ]
        logs, artifacts = fixture()
        logs[f'{JOB}/5_Build all targets.txt'] = log([
            (1, '##[group]Run build'), (1, '##[endgroup]'), *records])
        artifacts['build.log'] = ''.join(f'{message}\n' for _, message in records).encode()
        observed = observe_pair(pack(logs, artifacts))
        rows = {row['target']: row for row in observed['observations']}
        self.assertEqual(set(rows), {'zero', 'link_only', 'unfinished', 'repeated', 'clash'})
        self.assertEqual(rows['zero']['status'], 'unmeasured')
        self.assertEqual(rows['repeated']['status'], 'ambiguous')
        self.assertEqual(rows['clash']['status'], 'ambiguous')
        for row in rows.values():
            self.assertNotIn('seconds', row)
            self.assertTrue(row['reasons'])

    def test_archive_digests_and_run_job_head_tree_are_bound(self):
        pair = pack(*fixture())
        for override in ({'logs_sha256': '0' * 64},
                         {'artifacts_sha256': '0' * 64},
                         {'run_id': '99999'}, {'job': 'other-job'},
                         {'head': HEAD}, {'tree': TREE}):
            with self.subTest(override=override), self.assertRaises(shards.EvidenceError):
                observe_pair(pair, **override)

    def test_before_after_identity_and_companion_payload_must_match(self):
        for member, replacement in (
                ('identity-after.log', f'{HEAD}\n{OLD_TREE}\n'.encode()),
                ('build.log', b'[100%] Built target unrelated\n'),
                ('configure.log', b'-- Configuring done\n')):
            logs, artifacts = fixture()
            artifacts[member] = replacement
            with self.subTest(member=member), self.assertRaises(shards.EvidenceError):
                observe_pair(pack(logs, artifacts))

    def test_logged_upload_digest_must_match_companion(self):
        logs, artifacts = fixture()
        logs[f'{JOB}/7_Upload build logs.txt'] = log([
            (63, f'SHA256 digest of uploaded artifact zip is {"0" * 64}'),
            (63, f'Artifact x.zip successfully finalized. Artifact ID {ARTIFACT_ID}'),
            (63, 'Artifact download URL: https://github.com/example/engine/actions/'
                 f'runs/{RUN}/artifacts/{ARTIFACT_ID}')])
        with self.assertRaises(shards.EvidenceError):
            observe_pair(pack(logs, artifacts))

    def test_success_substring_cannot_rescue_actual_nonzero_timed_work(self):
        for phase_name in ('configure', 'build'):
            logs, artifacts = fixture()
            artifacts[f'{phase_name}.time'] = phase(exit_status=9)
            member = f'{JOB}/15_Complete job.txt'
            logs[member] += log([(64, 'wrapper exit_status=0; build successful')])
            with self.subTest(phase=phase_name), self.assertRaises(shards.EvidenceError):
                observe_pair(pack(logs, artifacts))

    def test_malformed_duplicate_and_nonfinite_timing_records_fail(self):
        for timing in (phase(wall='NaN'), phase(wall='Infinity'), phase(wall=-1),
                       phase(wall=0), phase() + b'exit_status=0\n',
                       b'wall_seconds=10\nexit_status=0\n'):
            logs, artifacts = fixture()
            artifacts['build.time'] = timing
            with self.subTest(timing=timing), self.assertRaises(shards.EvidenceError):
                observe_pair(pack(logs, artifacts))

    def test_missing_completion_and_error_annotation_fail(self):
        for mutation in ('missing', 'empty', 'error'):
            logs, artifacts = fixture()
            member = f'{JOB}/15_Complete job.txt'
            if mutation == 'missing':
                del logs[member]
            elif mutation == 'empty':
                logs[member] = b''
            else:
                logs[member] += log([(64, '##[error]Process completed with exit code 1.')])
            with self.subTest(mutation=mutation), self.assertRaises(shards.EvidenceError):
                observe_pair(pack(logs, artifacts))

    def test_malformed_utf8_timestamps_and_reversed_time_fail(self):
        for replacement in (
                b'\xff',
                b'NaN [100%] Built target a\n',
                b'2026-11-08T00:00:NaNZ [100%] Built target a\n',
                b'2026-02-31T00:00:00Z [100%] Built target a\n',
                log([(9, 'later'), (8, 'earlier')])):
            logs, artifacts = fixture()
            logs[f'{JOB}/5_Build all targets.txt'] = replacement
            with self.subTest(replacement=replacement), self.assertRaises(shards.EvidenceError):
                observe_pair(pack(logs, artifacts))

    def test_seven_digit_timestamp_precision_is_preserved(self):
        logs, artifacts = fixture({'a': 1})
        member = f'{JOB}/5_Build all targets.txt'
        logs[member] = logs[member].replace(stamp(1).encode(),
                                          b'2026-11-08T00:00:01.0000001Z')
        row = observe_pair(pack(logs, artifacts))['observations'][0]
        self.assertEqual(row['seconds'], 0.9999999)
        self.assertEqual(row['first_timestamp'], '2026-11-08T00:00:01.0000001Z')

    def test_malformed_zip_unsafe_and_duplicate_members_fail_without_extraction(self):
        logs, artifacts = fixture()
        good_logs, good_artifacts = pack(logs, artifacts)
        for which in ('logs', 'artifacts'):
            source = logs if which == 'logs' else artifacts
            member = next(iter(source))
            variants = [
                b'not a zip',
                archive([*source.items(), (member, source[member])]),
            ]
            for unsafe in ('../escape', '/absolute', 'C:/drive', 'dir\\escape',
                           'dir//alias', 'dir/./alias', '.ssh/key', 'secret.env'):
                variants.append(archive([*source.items(), (unsafe, b'unused')]))
            link = zipfile.ZipInfo('link')
            link.create_system = 3
            link.external_attr = 0o120777 << 16
            variants.append(archive([*source.items(), (link, b'elsewhere')]))
            for broken in variants:
                pair = (broken, good_artifacts) if which == 'logs' else (good_logs, broken)
                with self.subTest(which=which, digest=digest(broken)), \
                        self.assertRaises(shards.EvidenceError):
                    observe_pair(pair)


class PlanTests(unittest.TestCase):
    def test_measured_variation_changes_lpt_partition(self):
        first = shards.plan_shards(inventory(), [measured()])
        changed = shards.plan_shards(inventory(), [measured({'a': 8, 'b': 11, 'c': 6, 'd': 5})])
        self.assertEqual(first['shards'][0]['targets'], ['a', 'd'])
        self.assertEqual(first['shards'][1]['targets'], ['b', 'c'])
        self.assertEqual(first['shards'][0]['scheduling_sum_seconds'], 13)
        self.assertEqual(changed['shards'][0]['targets'], ['b', 'd'])
        self.assertNotEqual(first['shards'], changed['shards'])
        self.assertIn('not predicted', first['weight_semantics'])

    def test_stable_name_and_shard_one_ties_ignore_input_order(self):
        current = inventory(('d', 'c', 'b', 'a'))
        weights = measured(dict.fromkeys(('d', 'a', 'c', 'b'), 2))
        one = shards.plan_shards(current, [weights])
        current['targets'].reverse()
        weights['observations'].reverse()
        two = shards.plan_shards(current, [weights])
        self.assertEqual(one['shards'], two['shards'])
        self.assertEqual(one['shards'][0]['targets'], ['a', 'c'])
        self.assertEqual(one['shards'][1]['targets'], ['b', 'd'])

    def test_current_inventory_and_historical_measurement_identity_stay_distinct(self):
        current = inventory(('a', 'b', 'new'))
        with self.assertRaises(shards.EvidenceError) as failure:
            shards.plan_shards(current, [measured()])
        self.assertEqual(failure.exception.details['missing_targets'], ['new'])
        plan = shards.plan_shards(inventory(('a', 'b')), [measured()])
        self.assertEqual(plan['inventory']['head'], HEAD)
        self.assertEqual(plan['inventory']['tree'], TREE)
        self.assertEqual(plan['inventory']['provenance'], inventory()['provenance'])
        self.assertEqual({row['target'] for row in plan['obsolete_observations']}, {'c', 'd'})
        for row in plan['weights']:
            self.assertEqual(row['provenance']['head'], OLD_HEAD)
            self.assertEqual(row['provenance']['tree'], OLD_TREE)
        self.assertEqual(set().union(*(set(row['targets']) for row in plan['shards'])),
                         {'a', 'b'})

    def test_all_missing_names_are_reported_without_default_weights(self):
        with self.assertRaises(shards.EvidenceError) as failure:
            shards.plan_shards(inventory(('a', 'missing_z', 'missing_a')), [measured()])
        self.assertEqual(failure.exception.details['missing_targets'], ['missing_a', 'missing_z'])
        with self.assertRaises(shards.EvidenceError) as failure:
            shards.plan_shards(inventory(), [])
        self.assertEqual(failure.exception.details['missing_targets'], ['a', 'b', 'c', 'd'])

    def test_visible_but_unmeasured_target_is_still_missing_a_weight(self):
        logs, artifacts = fixture({'a': 8, 'b': 7})
        member = f'{JOB}/5_Build all targets.txt'
        output = artifacts['build.log'].decode().splitlines()
        output = [line for line in output if 'CMakeFiles/b.dir/' not in line]
        artifacts['build.log'] = ''.join(line + '\n' for line in output).encode()
        records = [(1, '##[group]Run build'), (1, '##[endgroup]'),
                   (1, output[0]), (8, output[1]), (9, output[2])]
        logs[member] = log(records)
        observed = observe_pair(pack(logs, artifacts))
        with self.assertRaises(shards.EvidenceError) as failure:
            shards.plan_shards(inventory(('a', 'b')), [observed])
        self.assertEqual(failure.exception.details['missing_targets'], ['b'])
        self.assertEqual(failure.exception.details['unusable_observations'][0]['target'], 'b')

    def test_missing_wrong_nonpositive_and_ambiguous_measurements_fail(self):
        mutations = [
            lambda doc: doc['observations'][0].pop('provenance'),
            lambda doc: doc['observations'][0]['provenance'].update(head=HEAD),
            lambda doc: doc['observations'][0]['provenance'].update(member_sha256='bad'),
            lambda doc: doc['observations'][0].update(first_line=0),
            lambda doc: doc['observations'][0].update(last_timestamp=stamp(1)),
            lambda doc: doc['observations'][0].update(seconds=0),
            lambda doc: doc['observations'][0].update(seconds=-1),
            lambda doc: doc['observations'][0].update(seconds=float('nan')),
            lambda doc: doc['observations'][0].update(seconds=float('inf')),
            lambda doc: doc['observations'][0].update(seconds=True),
            lambda doc: doc['observations'][0].update(seconds=999),
            lambda doc: doc['observations'].append(copy.deepcopy(doc['observations'][0])),
            lambda doc: doc['phase_totals']['build'].update(exit_status=7),
        ]
        for index, mutate in enumerate(mutations):
            doc = measured()
            mutate(doc)
            with self.subTest(mutation=index), self.assertRaises(shards.EvidenceError):
                shards.plan_shards(inventory(), [doc])
        with self.assertRaises(shards.EvidenceError) as failure:
            shards.plan_shards(inventory(), [measured(), measured()])
        self.assertEqual(failure.exception.details['ambiguous_targets'], ['a', 'b', 'c', 'd'])

    def test_inventory_empty_duplicate_malformed_and_singleton_fail(self):
        for targets in ([], ['a'], ['a', 'a'], ['a', ''], ['a', 3], ['a', '../b']):
            with self.subTest(targets=targets), self.assertRaises(shards.EvidenceError):
                shards.plan_shards(inventory(targets), [measured()])
        for field in ('head', 'tree'):
            current = inventory()
            current[field] = 'bad'
            with self.subTest(field=field), self.assertRaises(shards.EvidenceError):
                shards.plan_shards(current, [measured()])


class CheckTests(unittest.TestCase):
    def pair(self):
        return [result_manifest(1, ['a', 'd'], ['common']),
                result_manifest(2, ['b', 'c'], ['common', 'a'])]

    def test_exact_current_union_with_separate_shared_prerequisite_evidence(self):
        checked = shards.check_shards(inventory(), self.pair())
        self.assertEqual(checked['status'], 'coverage_checked')
        self.assertEqual(checked['covered_targets'], ['a', 'b', 'c', 'd'])
        self.assertEqual(checked['inventory']['head'], HEAD)
        self.assertEqual(checked['shards'][1]['prerequisites'], ['a', 'common'])

    def test_equal_counts_wrong_union_empty_and_partial_coverage_fail(self):
        cases = [
            [result_manifest(1, ['a', 'd']), result_manifest(2, ['b', 'wrong'])],
            [result_manifest(1, ['a', 'd']), result_manifest(2, ['b'])],
            [result_manifest(1, []), result_manifest(2, ['b', 'c'])],
        ]
        for pair in cases:
            with self.subTest(pair=pair), self.assertRaises(shards.EvidenceError):
                shards.check_shards(inventory(), pair)
        with self.assertRaises(shards.EvidenceError):
            shards.check_shards(inventory([]), self.pair())

    def test_missing_and_duplicate_shards_and_duplicate_ownership_fail(self):
        for pair in (self.pair()[:1], [self.pair()[0], self.pair()[0]],
                     [result_manifest(1, ['a', 'd']),
                      result_manifest(2, ['b', 'c', 'a'])],
                     [result_manifest(1, ['a', 'a', 'd']),
                      result_manifest(2, ['b', 'c'])]):
            with self.subTest(pair=pair), self.assertRaises(shards.EvidenceError):
                shards.check_shards(inventory(), pair)

    def test_current_inventory_is_authoritative_not_old_observation_population(self):
        old = inventory()
        old.update(head=OLD_HEAD, tree=OLD_TREE)
        with self.assertRaises(shards.EvidenceError):
            shards.check_shards(old, self.pair())
        for location in ('manifest', 'result'):
            for field in ('head', 'tree'):
                pair = self.pair()
                record = pair[0] if location == 'manifest' else pair[0]['result']
                record[field] = OLD_HEAD
                with self.subTest(location=location, field=field), \
                        self.assertRaises(shards.EvidenceError):
                    shards.check_shards(inventory(), pair)

    def test_missing_failed_cancelled_skipped_or_nonzero_results_fail(self):
        for status in (None, 'failure', 'failed', 'cancelled', 'skipped', 'pending', ''):
            pair = self.pair()
            if status is None:
                del pair[0]['result']
            else:
                pair[0]['result']['status'] = status
            with self.subTest(status=status), self.assertRaises(shards.EvidenceError):
                shards.check_shards(inventory(), pair)
        for key, value in (('exit_status', 3), ('exit_status', False)):
            pair = self.pair()
            pair[0]['result'][key] = value
            with self.subTest(value=value), self.assertRaises(shards.EvidenceError):
                shards.check_shards(inventory(), pair)
        pair = self.pair()
        pair[0]['result']['build_time'] = source_text('build.time', phase(exit_status=7).decode())
        with self.assertRaises(shards.EvidenceError):
            shards.check_shards(inventory(), pair)

    def test_success_without_actual_complete_built_target_records_fails(self):
        for text in ('', log([(1, 'echo "[100%] Built target a"')]).decode(),
                     log([(1, '[100%] Built target a')]).decode(),
                     log([(1, 'wrapper success exit_status=0')]).decode()):
            pair = self.pair()
            pair[0]['result']['build_log'] = source_text('clang-shard-1/build.txt', text)
            with self.subTest(text=text), self.assertRaises(shards.EvidenceError):
                shards.check_shards(inventory(), pair)
        pair = self.pair()
        pair[0]['result']['build_log']['text'] += 'tampered'
        with self.assertRaises(shards.EvidenceError):
            shards.check_shards(inventory(), pair)

    def test_prerequisite_cannot_hide_unowned_all_target_or_undeclared_build(self):
        pair = [result_manifest(1, ['a', 'd']), result_manifest(2, ['b'], ['c'])]
        with self.assertRaises(shards.EvidenceError):
            shards.check_shards(inventory(), pair)
        pair = self.pair()
        pair[0]['prerequisites'] = []
        with self.assertRaises(shards.EvidenceError):
            shards.check_shards(inventory(), pair)

    def test_partial_or_reversed_target_completion_evidence_fails(self):
        for extra in (
                '[  0%] Building CXX object CMakeFiles/undeclared.dir/a.cpp.o',
                '[  0%] Building CXX object CMakeFiles/a.dir/late.cpp.o'):
            pair = self.pair()
            evidence = pair[0]['result']['build_log']
            text = evidence['text'] + log([(20, extra)]).decode()
            pair[0]['result']['build_log'] = source_text(evidence['member'], text)
            with self.subTest(extra=extra), self.assertRaises(shards.EvidenceError):
                shards.check_shards(inventory(), pair)


class CliTests(unittest.TestCase):
    def setUp(self):
        self.directory = SCRATCH / ('clang-build-shards-' + uuid.uuid4().hex)
        self.directory.mkdir(parents=True)

    def write_json(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value), encoding='utf-8')
        return path

    def invoke(self, command, *arguments):
        output = self.directory / (uuid.uuid4().hex + '.json')
        report = self.directory / (uuid.uuid4().hex + '.report.json')
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = shards.main([command, *map(str, arguments),
                                '--scratch-dir', str(self.directory),
                                '--output', str(output), '--report', str(report)])
        return code, output, report, stderr.getvalue()

    def test_cli_observe_plan_check_use_real_judged_paths(self):
        pair = pack(*fixture())
        logs = self.directory / 'logs.zip'
        artifacts = self.directory / 'artifacts.zip'
        logs.write_bytes(pair[0])
        artifacts.write_bytes(pair[1])
        before = (logs.read_bytes(), artifacts.read_bytes())
        code, observed, report, audit = self.invoke(
            'observe', '--logs-zip', logs, '--artifacts-zip', artifacts,
            '--logs-sha256', digest(pair[0]), '--artifacts-sha256', digest(pair[1]),
            '--run-id', RUN, '--job', JOB, '--head', OLD_HEAD, '--tree', OLD_TREE)
        self.assertEqual(code, 0, audit)
        self.assertTrue(report.is_file())
        self.assertIn(str(logs), audit)
        self.assertIn(str(artifacts), audit)
        self.assertEqual(before, (logs.read_bytes(), artifacts.read_bytes()))
        current = self.write_json('inventory.json', inventory())
        code, planned, _, audit = self.invoke(
            'plan', '--inventory', current, '--observations', observed)
        self.assertEqual(code, 0, audit)
        plan = json.loads(planned.read_text())
        manifests = [self.write_json(f'shard-{row["shard"]}.json',
                                    result_manifest(row['shard'], row['targets']))
                     for row in plan['shards']]
        code, checked, _, audit = self.invoke(
            'check', '--inventory', current, '--shard', manifests[0], '--shard', manifests[1])
        self.assertEqual(code, 0, audit)
        self.assertEqual(json.loads(checked.read_text())['status'], 'coverage_checked')

    def test_cli_missing_weights_and_failed_shard_publish_error_report_only(self):
        current = self.write_json('inventory.json', inventory(('a', 'new_z', 'new_a')))
        weights = self.write_json('observations.json', measured())
        code, output, report, _ = self.invoke(
            'plan', '--inventory', current, '--observations', weights)
        self.assertNotEqual(code, 0)
        self.assertFalse(output.exists())
        error = json.loads(report.read_text())
        self.assertEqual(error['status'], 'error')
        self.assertEqual(error['details']['missing_targets'], ['new_a', 'new_z'])
        pair = [result_manifest(1, ['a']), result_manifest(2, ['new_z', 'new_a'])]
        pair[1]['result']['status'] = 'skipped'
        paths = [self.write_json(f'result-{i}.json', value) for i, value in enumerate(pair)]
        code, output, report, _ = self.invoke(
            'check', '--inventory', current, '--shard', paths[0], '--shard', paths[1])
        self.assertNotEqual(code, 0)
        self.assertFalse(output.exists())
        self.assertEqual(json.loads(report.read_text())['status'], 'error')

    def test_duplicate_json_keys_and_nonfinite_json_fail_closed(self):
        observations = self.write_json('observations.json', measured())
        for text in ('{"schema":"clang-build-inventory-v1","head":"x","head":"y"}',
                     '{"targets":[NaN]}'):
            current = self.directory / (uuid.uuid4().hex + '.json')
            current.write_text(text, encoding='utf-8')
            code, output, _, _ = self.invoke(
                'plan', '--inventory', current, '--observations', observations)
            self.assertNotEqual(code, 0)
            self.assertFalse(output.exists())

    def test_output_must_be_fresh_in_scratch_and_cannot_alias_input(self):
        current = self.write_json('inventory.json', inventory())
        observations = self.write_json('observations.json', measured())
        report = self.directory / 'report.json'
        before = current.read_bytes()
        for output in (current, self.directory.parent / 'outside.json'):
            with contextlib.redirect_stderr(io.StringIO()):
                code = shards.main([
                    'plan', '--inventory', str(current), '--observations', str(observations),
                    '--scratch-dir', str(self.directory), '--output', str(output),
                    '--report', str(report)])
            self.assertNotEqual(code, 0)
            self.assertEqual(current.read_bytes(), before)

    def test_sensitive_input_name_is_refused_before_any_open(self):
        observations = self.write_json('observations.json', measured())
        for name in ('.ssh/id', '.aws/config', '.codex/auth.json', 'secret.env',
                     '.config/zsh/keys/key', '.config/pineforge/state',
                     '.config/other/../zsh/keys/key'):
            code, output, _, audit = self.invoke(
                'plan', '--inventory', self.directory / name, '--observations', observations)
            self.assertNotEqual(code, 0)
            self.assertFalse(output.exists())
            self.assertIn('refused path', audit)


if __name__ == '__main__':
    unittest.main()
