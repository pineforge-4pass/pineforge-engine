#!/usr/bin/env python3
"""Collect default ALL, observe hosted windows, plan shards, and check coverage.

Standard library only; no build, extraction, command execution, or Git discovery.
Inventory uses only the explicitly supplied generated Makefile2 and codemodel
bytes. The caller asserts checkout identity and generation/compiler flags.
Historical observation identities remain historical.

Schemas (extra inventory provenance fields are preserved):
  clang-build-inventory-v1: head, tree, targets (nonempty unique name strings).
  clang-build-observations-v1: emitted by observe; individual measured rows carry
    original identity, ZIP/member digests, line bounds, timestamps, and seconds.
    record_bytes maps member SHA256 to base64 of the original selected bytes.
    plan verifies those bytes and reconstructs observations with the same parser;
    scalar-only documents are refused. This validates records, not external
    authentication of a caller-supplied document.
  clang-build-shard-result-v1: head, tree, shard (1 or 2), targets, prerequisites,
    result {head, tree, run_id, job, status: "success", exit_status: 0,
            build_log: SOURCE_TEXT, build_time: SOURCE_TEXT}.
  SOURCE_TEXT: {archive_sha256, member, sha256, text}. build_log is the exact
    timestamped GitHub build member; build_time is the actual timed-work record.
    These are embedded evidence, not paths to follow. Each assigned target must
    have an actual anchored CMake Built target record. Other built targets must
    be declared prerequisites. Declaring a prerequisite never grants ownership.

Only JSON outputs are published, to fresh paths inside --scratch-dir. Pending
files are retained. Read the result output, not the diagnostic report, for the
command's result. No cold-build, test-execution, or <=900-second proof is made.
"""
from __future__ import annotations

import argparse
import base64
import binascii
from collections import defaultdict
import copy
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import uuid
import zipfile
import zlib


INVENTORY_SCHEMA = 'clang-build-inventory-v1'
OBSERVATIONS_SCHEMA = 'clang-build-observations-v1'
RESULT_SCHEMA = 'clang-build-shard-result-v1'
WINDOW_SEMANTICS = (
    'overlapping observed target windows; not isolated target build times or CPU costs')
SUM_SEMANTICS = (
    'scheduling sums of overlapping observed target windows; not predicted shard wall time; '
    'common dependency rebuild cost is not predicted')
TARGET_PATTERN = r'[A-Za-z0-9_][A-Za-z0-9_.+-]*'
TARGET = re.compile(TARGET_PATTERN)
PROGRESS = r'\[\s*(?:100|[0-9]{1,2})%\]\s+'
BUILDING = re.compile(PROGRESS + r'Building .+? (?P<path>\S+)')
BUILT = re.compile(PROGRESS + rf'Built target (?P<target>{TARGET_PATTERN})')
OBJECT_OWNER = re.compile(
    rf'(?P<owner>(?:.*/)?CMakeFiles/(?P<target>{TARGET_PATTERN})\.dir)/.+')
STAMP = re.compile(
    r'(?P<base>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})'
    r'(?:\.(?P<fraction>\d{1,9}))?(?:Z|\+00:00)')
DECIMAL_TEXT = re.compile(r'[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?')
UPLOAD_URL = re.compile(
    r'https://github\.com/(?P<repository>[^/\s]+/[^/\s]+)/actions/runs/'
    r'(?P<run_id>[1-9][0-9]*)/artifacts/(?P<artifact_id>[1-9][0-9]*)')
LOG_ROLES = {
    'setup': '1_Set up job.txt',
    'configure': '4_Configure Release.txt',
    'build': '5_Build all targets.txt',
    'identity': '6_Record final identity and evidence.txt',
    'upload': '7_Upload build logs.txt',
    'complete': '15_Complete job.txt',
}
ARTIFACT_MEMBERS = (
    'identity-before.log', 'identity-after.log', 'configure.time', 'build.time',
    'configure.log', 'build.log', 'started-at.log', 'finished-at.log',
)
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


class EvidenceError(ValueError):
    def __init__(self, message, details=None):
        super().__init__(message)
        self.details = details if details is not None else {}


def _require(condition, message, details=None):
    if not condition:
        raise EvidenceError(message, details)


def _object(value, label):
    _require(isinstance(value, dict), f'{label}: expected an object')
    return value


def _sha(value, length, label):
    _require(isinstance(value, str) and
             re.fullmatch(r'[0-9a-f]{' + str(length) + '}', value) is not None,
             f'{label}: expected a lowercase {length}-digit SHA')
    return value


def _identity(value, label):
    value = _object(value, label)
    return {key: _sha(value.get(key), 40, f'{label}.{key}') for key in ('head', 'tree')}


def _run_job(value, label):
    value = _object(value, label)
    run_id = value.get('run_id')
    job = value.get('job')
    _require(isinstance(run_id, str) and re.fullmatch(r'[1-9][0-9]*', run_id) is not None,
             f'{label}.run_id: expected a positive decimal string')
    _require(isinstance(job, str) and TARGET.fullmatch(job) is not None,
             f'{label}.job: invalid job name')
    return {'run_id': run_id, 'job': job}


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _number(value, label, positive=False):
    _require(type(value) in (int, float), f'{label}: expected a finite number')
    number = Decimal(str(value))
    _require(number.is_finite() and (number > 0 if positive else number >= 0),
             f'{label}: expected a {"positive" if positive else "nonnegative"} finite number')
    return number


def _json_number(number):
    result = float(number)
    _require(math.isfinite(result), 'numeric result cannot be represented in JSON')
    _require(number == 0 or result > 0, 'positive seconds underflow in JSON representation')
    return result


def _text(data, label, allow_empty=False):
    try:
        text = data.decode('utf-8-sig')
    except UnicodeDecodeError as error:
        raise EvidenceError(f'{label}: invalid UTF-8') from error
    _require('\x00' not in text, f'{label}: NUL in text record')
    _require(allow_empty or bool(text), f'{label}: empty record')
    return text


def _timestamp(value, label):
    _require(isinstance(value, str), f'{label}: expected a UTC timestamp')
    match = STAMP.fullmatch(value)
    _require(match is not None, f'{label}: malformed UTC timestamp')
    try:
        base = datetime.fromisoformat(match['base']).replace(tzinfo=timezone.utc)
    except ValueError as error:
        raise EvidenceError(f'{label}: malformed UTC timestamp') from error
    delta = base - EPOCH
    fraction = match['fraction']
    return (Decimal(delta.days * 86400 + delta.seconds) +
            (Decimal('0.' + fraction) if fraction else Decimal(0)))


def _records(data, label):
    text = _text(data, label)
    records = []
    previous = None
    for line, raw in enumerate(text.splitlines(), 1):
        match = re.fullmatch(r'(\S+)[ \t](.*)', raw)
        _require(match is not None, f'{label}:{line}: malformed timestamped record')
        timestamp, message = match.groups()
        seconds = _timestamp(timestamp, f'{label}:{line}')
        _require(previous is None or seconds >= previous,
                 f'{label}:{line}: reversed time')
        _require(not message.startswith('##[error]'), f'{label}:{line}: job error annotation')
        records.append({'line': line, 'timestamp': timestamp,
                        'time': seconds, 'message': message})
        previous = seconds
    _require(bool(records), f'{label}: missing records')
    return records


def _phase(data, label):
    fields = {}
    for line in _text(data, label).splitlines():
        match = re.fullmatch(r'([a-z_]+)=(\S+)', line)
        _require(match is not None, f'{label}: malformed timed-work record')
        key, value = match.groups()
        _require(key not in fields, f'{label}: duplicate timed-work field {key}')
        fields[key] = value
    _require(set(fields) == {'wall_seconds', 'user_seconds', 'system_seconds', 'exit_status'},
             f'{label}: timed-work fields must be wall/user/system seconds and exit_status')
    _require(re.fullmatch(r'[0-9]+', fields['exit_status']) is not None,
             f'{label}: malformed timed-work exit_status')
    _require(int(fields['exit_status']) == 0, f'{label}: timed work exited nonzero',
             {'exit_status': int(fields['exit_status'])})
    result = {'exit_status': 0}
    for key in ('wall_seconds', 'user_seconds', 'system_seconds'):
        raw = fields[key]
        _require(DECIMAL_TEXT.fullmatch(raw) is not None, f'{label}.{key}: invalid seconds')
        try:
            seconds = Decimal(raw)
        except InvalidOperation as error:
            raise EvidenceError(f'{label}.{key}: invalid seconds') from error
        _require(seconds.is_finite() and
                 (seconds > 0 if key == 'wall_seconds' else seconds >= 0),
                 f'{label}.{key}: nonpositive or nonfinite seconds')
        result[key] = _json_number(seconds)
    return result


def _refuse_sensitive_name(name):
    parts = name.replace('\\', '/').split('/')
    forbidden = any(part in {'.ssh', '.aws', '.codex'} or part.endswith('.env')
                    for part in parts)
    for index, part in enumerate(parts):
        if part == '.config':
            forbidden |= parts[index + 1:index + 3] == ['zsh', 'keys']
            forbidden |= parts[index + 1:index + 2] == ['pineforge']
    _require(not forbidden, f'refused path: {name}')


def _member_name(name):
    _require(isinstance(name, str) and bool(name), 'invalid ZIP/source member name')
    _refuse_sensitive_name(name)
    _require('\x00' not in name and '\\' not in name and ':' not in name and
             not name.startswith('/'), f'unsafe ZIP/source member name: {name}')
    core = name[:-1] if name.endswith('/') else name
    _require(bool(core) and all(part not in {'', '.', '..'} for part in core.split('/')),
             f'unsafe ZIP/source member name: {name}')
    return name


def _archive(data, expected_sha256, required, label):
    _sha(expected_sha256, 64, f'{label}.expected_sha256')
    _require(_digest(data) == expected_sha256, f'{label}: archive SHA256 mismatch')
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            names = {}
            aliases = set()
            for info in archive.infolist():
                name = _member_name(info.filename)
                # orig_filename retains a NUL suffix that ZipInfo.filename discards.
                _require(info.orig_filename == name, f'{label}: ambiguous member name')
                canonical = name.rstrip('/')
                _require(name not in names and canonical not in aliases,
                         f'{label}: duplicate/ambiguous member {name}')
                _require(not stat.S_ISLNK(info.external_attr >> 16),
                         f'{label}: symbolic-link member {name}')
                _require(not (info.flag_bits & 1), f'{label}: encrypted member {name}')
                names[name] = info
                aliases.add(canonical)
            missing = sorted(set(required) - names.keys())
            _require(not missing, f'{label}: missing required members', {'missing_members': missing})
            result = {}
            for name in required:
                _require(not names[name].is_dir(), f'{label}: required member is a directory: {name}')
                result[name] = archive.read(names[name])
                _text(result[name], f'{label}:{name}')
            return result
    except (zipfile.BadZipFile, zipfile.LargeZipFile, NotImplementedError, RuntimeError,
            EOFError, OSError, UnicodeError, zlib.error) as error:
        raise EvidenceError(f'{label}: invalid or unreadable ZIP: {error}') from error


def _source_ref(archive_sha256, member, data):
    return {'archive_sha256': archive_sha256, 'member': member, 'sha256': _digest(data)}


def _validate_ref(value, label):
    value = _object(value, label)
    _sha(value.get('archive_sha256'), 64, f'{label}.archive_sha256')
    _sha(value.get('sha256'), 64, f'{label}.sha256')
    _member_name(value.get('member'))
    return value


def _record_bytes(contents, source, label):
    """Resolve only an in-document digest, never a filename or filesystem path."""
    member_sha256 = _sha(source.get('sha256'), 64, f'{label}.sha256')
    encoded = contents.get(member_sha256)
    _require(isinstance(encoded, str), f'{label}: missing original record bytes',
             {'member_sha256': member_sha256})
    try:
        data = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as error:
        raise EvidenceError(f'{label}: malformed base64 original record bytes',
                            {'member_sha256': member_sha256}) from error
    _require(_digest(data) == member_sha256, f'{label}: cited SHA256 does not match record bytes',
             {'member_sha256': member_sha256})
    return data


def _payload(records, artifact, label):
    """Match all actual tee output, after the command-echo group, to the artifact."""
    lines = _text(artifact, label).splitlines()
    start = len(records) - len(lines)
    _require(start > 0 and records[start - 1]['message'] == '##[endgroup]' and
             [record['message'] for record in records[start:]] == lines,
             f'{label}: companion payload differs from actual logged output')
    return records[start:]


def _one_match(records, expression, label):
    matches = [match for record in records
               if (match := re.fullmatch(expression, record['message'])) is not None]
    _require(len(matches) == 1, f'{label}: required unique record missing or ambiguous')
    return matches[0]


def _build_events(records):
    events = defaultdict(lambda: {'starts': [], 'ends': []})
    for record in records:
        message = record['message']
        built = BUILT.fullmatch(message)
        if built:
            events[built['target']]['ends'].append(record)
            continue
        building = BUILDING.fullmatch(message)
        if building:
            path = building['path']
            owner = OBJECT_OWNER.fullmatch(path)
            if owner:
                events[owner['target']]['starts'].append(
                    {**record, 'path': path, 'owner': owner['owner']})
            elif 'CMakeFiles/' in path:
                raise EvidenceError(f'line {record["line"]}: malformed own-target path')
        elif re.match(PROGRESS + r'Built target\b', message):
            raise EvidenceError(f'line {record["line"]}: malformed Built target record')
    return events


def _target_windows(records, provenance):
    events = _build_events(records)
    _require(bool(events), 'build log has no observed target events')
    observations = []
    for name, event in sorted(events.items()):
        starts, ends = event['starts'], event['ends']
        reasons = []
        ambiguous = (len(ends) > 1 or len({row['owner'] for row in starts}) > 1 or
                     len({row['path'] for row in starts}) != len(starts))
        if ambiguous:
            reasons.append('ambiguous repeated completion, object, or own-target directory')
        if not starts:
            reasons.append('missing own Building record')
        if not ends:
            reasons.append('missing Built target completion')
        if starts and ends and not ambiguous:
            first, last = starts[0], ends[0]
            if last['line'] <= starts[-1]['line'] or last['time'] <= first['time']:
                reasons.append('nonpositive or reversed observed target window')
        row = {'target': name, 'provenance': copy.deepcopy(provenance)}
        if reasons:
            row.update(status='ambiguous' if ambiguous else 'unmeasured', reasons=reasons,
                       building_lines=[record['line'] for record in starts],
                       completion_lines=[record['line'] for record in ends])
        else:
            row.update(status='measured', first_line=first['line'], last_line=last['line'],
                       first_timestamp=first['timestamp'], last_timestamp=last['timestamp'],
                       seconds=_json_number(last['time'] - first['time']))
        observations.append(row)
    return observations


def observe_archives(logs_data, artifacts_data, *, logs_sha256, artifacts_sha256,
                     run_id, job, head, tree, log_members=None):
    """Validate both supplied ZIP byte strings, without extracting any member."""
    identity = _identity({'head': head, 'tree': tree}, 'identity')
    identity.update(_run_job({'run_id': run_id, 'job': job}, 'identity'))
    selected = {role: f'{job}/{suffix}' for role, suffix in LOG_ROLES.items()}
    if log_members is not None:
        _require(isinstance(log_members, dict) and set(log_members) <= LOG_ROLES.keys(),
                 'unknown log-member role')
        selected.update(log_members)
    for name in selected.values():
        _member_name(name)
        _require(name.startswith(f'{job}/') and not name.endswith('/'),
                 'selected log member must belong to the exact supplied job')
    _require(len(set(selected.values())) == len(selected), 'duplicate selected log members')
    logs = _archive(logs_data, logs_sha256, selected.values(), 'logs')
    artifacts = _archive(artifacts_data, artifacts_sha256, ARTIFACT_MEMBERS, 'artifacts')
    return _observe_members(logs, artifacts, identity=identity, selected=selected,
                            logs_sha256=logs_sha256, artifacts_sha256=artifacts_sha256)


def _observe_members(logs, artifacts, *, identity, selected, logs_sha256, artifacts_sha256):
    """Use the original record/target parser for both ZIP observation and planning."""
    head, tree, run_id, job = (identity[key] for key in ('head', 'tree', 'run_id', 'job'))
    records = {role: _records(logs[name], name) for role, name in selected.items()}
    _one_match(records['setup'], re.escape(f'Complete job name: {job}'), 'job identity')
    before = artifacts['identity-before.log']
    after = artifacts['identity-after.log']
    _require(before == after, 'artifact before/after identity mismatch')
    _require(_text(before, 'identity-before.log').splitlines() == [head, tree],
             'artifact head/tree does not match expected identity')
    _require([row['message'] for row in records['identity'][-2:]] == [head, tree],
             'logged final head/tree does not match artifact and expected identity')
    _one_match(records['complete'], r'Cleaning up orphan processes', 'job completion')
    upload_digest = _one_match(
        records['upload'], r'SHA256 digest of uploaded artifact zip is ([0-9a-f]{64})',
        'uploaded artifact digest')[1]
    _require(upload_digest == artifacts_sha256, 'uploaded companion archive SHA256 mismatch')
    upload_url = _one_match(records['upload'], r'Artifact download URL: (\S+)',
                            'artifact download URL')[1]
    upload = UPLOAD_URL.fullmatch(upload_url)
    _require(upload is not None and upload['run_id'] == run_id,
             'uploaded artifact URL does not match exact run')
    finalized = _one_match(
        records['upload'], r'Artifact .+\.zip successfully finalized\. Artifact ID ([1-9][0-9]*)',
        'artifact finalization')
    _require(finalized[1] == upload['artifact_id'], 'uploaded artifact ID mismatch')
    for earlier, later in (('setup', 'configure'), ('configure', 'build'), ('build', 'identity'),
                           ('identity', 'upload'), ('upload', 'complete')):
        _require(records[earlier][-1]['time'] <= records[later][0]['time'],
                 f'reversed phase time: {earlier} -> {later}')
    started_text = _text(artifacts['started-at.log'], 'started-at.log').splitlines()
    finished_text = _text(artifacts['finished-at.log'], 'finished-at.log').splitlines()
    _require(len(started_text) == len(finished_text) == 1, 'malformed start/finish records')
    started = _timestamp(started_text[0], 'started-at.log')
    finished = _timestamp(finished_text[0], 'finished-at.log')
    _require(started <= finished, 'reversed artifact start/finish time')
    # date --iso-8601=seconds rounds down; the final logged output can be later
    # within that same second. Honor the record's precision, not a guessed time.
    finish_match = STAMP.fullmatch(finished_text[0])
    precision = len(finish_match['fraction'] or '')
    finish_limit = finished + Decimal(10) ** -precision
    _require(records['configure'][0]['time'] >= started and
             records['identity'][-1]['time'] < finish_limit,
             'logged phases fall outside artifact start/finish records')
    sources = {role: _source_ref(logs_sha256, name, logs[name])
               for role, name in selected.items()}
    sources.update({name: _source_ref(artifacts_sha256, name, artifacts[name])
                    for name in ARTIFACT_MEMBERS})
    totals = {name: {**_phase(artifacts[f'{name}.time'], f'{name}.time'),
                     'source': sources[f'{name}.time']}
              for name in ('configure', 'build')}
    configure = _payload(records['configure'], artifacts['configure.log'], 'configure.log')
    for expression in (r'-- Configuring done(?: \([^)]+\))?',
                       r'-- Generating done(?: \([^)]+\))?',
                       r'-- Build files have been written to: .+'):
        _one_match(configure, expression, 'configure completion')
    build = _payload(records['build'], artifacts['build.log'], 'build.log')
    provenance = {**identity, 'logs_sha256': logs_sha256,
                  'artifacts_sha256': artifacts_sha256, 'member': selected['build'],
                  'member_sha256': sources['build']['sha256']}
    observations = _target_windows(build, provenance)
    return {
        'schema': OBSERVATIONS_SCHEMA, 'status': 'observed',
        'weight_semantics': WINDOW_SEMANTICS, 'identity': identity,
        'archives': {'logs_sha256': logs_sha256, 'artifacts_sha256': artifacts_sha256},
        'upload': {'url': upload_url, 'artifact_id': upload['artifact_id']},
        'sources': sources, 'phase_totals': totals,
        'record_bytes': {_digest(data): base64.b64encode(data).decode('ascii')
                         for data in (*logs.values(), *artifacts.values())},
        'observations': observations,
    }


def _names(value, label, allow_empty=False):
    _require(isinstance(value, list) and (allow_empty or bool(value)),
             f'{label}: expected {"a" if allow_empty else "a nonempty"} target-name list')
    seen = set()
    duplicates = set()
    for name in value:
        _require(isinstance(name, str) and TARGET.fullmatch(name) is not None,
                 f'{label}: malformed target name')
        if name in seen:
            duplicates.add(name)
        seen.add(name)
    _require(not duplicates, f'{label}: duplicate target names',
             {'duplicate_targets': sorted(duplicates)})
    return seen


def _inventory(value):
    value = _object(value, 'inventory')
    _require(value.get('schema') == INVENTORY_SCHEMA, 'inventory: wrong schema')
    _identity(value, 'inventory')
    return _names(value.get('targets'), 'inventory.targets')


def _model_index(value, size, label):
    _require(type(value) is int and 0 <= value < size, f'{label}: invalid index')
    return value


def _model_indexes(value, size, label):
    _require(isinstance(value, list), f'{label}: expected an index list')
    result = [_model_index(index, size, label) for index in value]
    _require(len(result) == len(set(result)), f'{label}: duplicate indexes')
    return set(result)


def _model_string(value, label):
    _require(isinstance(value, str) and bool(value) and
             all(ord(char) >= 32 and ord(char) != 127 for char in value),
             f'{label}: expected a nonempty string without control characters')
    return value


def _build_directory(value, label):
    _model_string(value, label)
    _require(value == '.' or all(
        part not in {'', '.', '..'} and re.fullmatch(r'[A-Za-z0-9_.+-]+', part)
        for part in value.split('/')), f'{label}: unsupported build-directory path')
    return value


def _model_hierarchy(rows, root, label):
    """Check indexes in both directions, including disconnected parent cycles."""
    children = [_model_indexes(row.get('childIndexes', []), len(rows),
                               f'{label}[{index}].childIndexes')
                for index, row in enumerate(rows)]
    expected = [set() for _ in rows]
    for index, row in enumerate(rows):
        if index == root:
            _require('parentIndex' not in row, f'{label}: root has a parent')
        else:
            parent = _model_index(row.get('parentIndex'), len(rows),
                                  f'{label}[{index}].parentIndex')
            _require(parent != index, f'{label}: self-parent')
            expected[parent].add(index)
    _require(children == expected, f'{label}: parent/child indexes disagree')
    seen = set()
    pending = [root]
    while pending:
        index = pending.pop()
        _require(index not in seen, f'{label}: cyclic hierarchy')
        seen.add(index)
        pending.extend(children[index])
    _require(len(seen) == len(rows), f'{label}: disconnected or cyclic hierarchy')


def _codemodel_bindings(data, configuration):
    try:
        model = json.loads(_text(data, 'codemodel'), object_pairs_hook=_no_duplicate_keys,
                           parse_constant=_nonfinite_constant)
    except (json.JSONDecodeError, ValueError) as error:
        if isinstance(error, EvidenceError):
            raise
        raise EvidenceError(f'codemodel: malformed JSON: {error}') from error
    _finite_json(model)
    _object(model, 'codemodel')
    _require(model.get('kind') == 'codemodel', 'codemodel: wrong kind')
    version = _object(model.get('version'), 'codemodel.version')
    _require(type(version.get('major')) is int and version['major'] == 2 and
             type(version.get('minor')) is int and version['minor'] >= 0,
             'codemodel: expected version 2')
    paths = _object(model.get('paths'), 'codemodel.paths')
    for key in ('source', 'build'):
        _require(_model_string(paths.get(key), f'codemodel.paths.{key}').startswith('/'),
                 f'codemodel.paths.{key}: expected an absolute path (metadata only)')
    configurations = model.get('configurations')
    _require(isinstance(configurations, list) and bool(configurations),
             'codemodel: missing configurations')
    selected = None
    names = set()
    for row in configurations:
        _object(row, 'codemodel configuration')
        name = row.get('name')
        _require(isinstance(name, str) and name not in names,
                 'codemodel: malformed or duplicate configuration name')
        names.add(name)
        if name == configuration:
            selected = row
    _require(selected is not None, 'codemodel: requested configuration is absent')
    directories, targets, projects = (selected.get(key)
                                       for key in ('directories', 'targets', 'projects'))
    for rows, label in ((directories, 'directories'), (targets, 'targets'), (projects, 'projects')):
        _require(isinstance(rows, list) and bool(rows), f'codemodel: missing {label}')
        for row in rows:
            _object(row, f'codemodel.{label} row')
    builds = []
    aliases = set()
    directory_owners = [set() for _ in projects]
    target_owners = [set() for _ in projects]
    directory_targets = [set() for _ in directories]
    for index, directory in enumerate(directories):
        build = _build_directory(directory.get('build'), f'directories[{index}].build')
        _require(build not in builds, 'codemodel: duplicate build-directory alias')
        builds.append(build)
        aliases.add('all' if build == '.' else build + '/all')
        _model_string(directory.get('source'), f'directories[{index}].source')
        project = _model_index(directory.get('projectIndex'), len(projects),
                               f'directories[{index}].projectIndex')
        directory_owners[project].add(index)
    _require('.' in builds, 'codemodel: missing root build directory')
    _require(directories[builds.index('.')]['source'] == '.',
             'codemodel: root directory source must be .')
    _model_hierarchy(directories, builds.index('.'), 'directories')
    bindings = {}
    target_names, identities, references = set(), set(), set()
    for index, target in enumerate(targets):
        name = target.get('name')
        _names([name], f'targets[{index}].name')
        identity = _model_string(target.get('id'), f'targets[{index}].id')
        _require(not any(char.isspace() for char in identity), 'codemodel: whitespace in target id')
        reference = target.get('jsonFile')
        _require(isinstance(reference, str) and
                 re.fullmatch(r'[A-Za-z0-9_.+-]+\.json', reference) is not None,
                 'codemodel: malformed target jsonFile (never followed)')
        _require(name not in target_names and identity not in identities and
                 reference not in references, 'codemodel: duplicate target name, id or jsonFile')
        target_names.add(name)
        identities.add(identity)
        references.add(reference)
        directory = _model_index(target.get('directoryIndex'), len(directories),
                                 f'targets[{index}].directoryIndex')
        project = _model_index(target.get('projectIndex'), len(projects),
                               f'targets[{index}].projectIndex')
        _require(project == directories[directory]['projectIndex'],
                 'codemodel: target/directory project indexes disagree')
        directory_targets[directory].add(index)
        target_owners[project].add(index)
        build = builds[directory]
        node = ('' if build == '.' else build + '/') + f'CMakeFiles/{name}.dir/all'
        bindings[node] = {'name': name, 'id': identity, 'build': build,
                          'source': directories[directory]['source'], 'all_node': node}
    for index, directory in enumerate(directories):
        declared = _model_indexes(directory.get('targetIndexes', []), len(targets),
                                  f'directories[{index}].targetIndexes')
        _require(declared == directory_targets[index],
                 'codemodel: directory/target indexes disagree')
    for index, project in enumerate(projects):
        _model_string(project.get('name'), f'projects[{index}].name')
        for key, rows, expected in (('directoryIndexes', directories, directory_owners[index]),
                                     ('targetIndexes', targets, target_owners[index])):
            _require(_model_indexes(project.get(key, []), len(rows), f'projects[{index}].{key}')
                     == expected, 'codemodel: project membership indexes disagree')
    roots = [index for index, project in enumerate(projects) if 'parentIndex' not in project]
    _require(len(roots) == 1, 'codemodel: ambiguous project root')
    _model_hierarchy(projects, roots[0], 'projects')
    _require(not aliases & bindings.keys(), 'codemodel: directory/target ALL alias collision')
    return aliases, bindings, {key: paths[key] for key in ('source', 'build')}


def _makefile2_rules(data):
    """Parse an intentionally restricted generated grammar; never evaluate Make."""
    text = _text(data, 'Makefile2')
    text = text.replace('\r\n', '\n')
    _require(all(char in '\n\t' or (ord(char) >= 32 and char not in '\x7f\x85\u2028\u2029')
                 for char in text),
             'Makefile2: unsupported character or line separator')
    lines = text.splitlines()
    _require(lines.count('# CMAKE generated file: DO NOT EDIT!') == 1,
             'Makefile2: missing or duplicate generated-file header')
    generators = [match.groups() for line in lines if (match := re.fullmatch(
        r'# Generated by "([^"]+)" Generator, CMake Version ([0-9]+(?:\.[0-9]+)+)', line))]
    _require(len(generators) == 1 and generators[0][0] == 'Unix Makefiles',
             'Makefile2: expected a unique Unix Makefiles generator header')
    rules = defaultdict(list)
    index = 0
    while index < len(lines):
        first = index + 1
        logical = lines[index]
        recipe = logical.startswith('\t')
        index += 1
        while (len(logical) - len(logical.rstrip('\\'))) % 2:
            _require(index < len(lines), f'Makefile2:{first}: dangling continuation')
            logical = logical[:-1] + ' ' + lines[index].lstrip(' \t')
            index += 1
        if recipe:
            continue
        code = logical.split('#', 1)[0].strip(' \t')
        if not code:
            continue
        # Only CMake's non-graph boilerplate is accepted outside dependency rules.
        # Unknown assignments/directives could change how ALL rules are parsed.
        assignment = re.fullmatch(
            r'(?:SHELL|CMAKE_COMMAND|RM|EQUALS|CMAKE_SOURCE_DIR|CMAKE_BINARY_DIR)\s*=\s*(.*)', code)
        if assignment:
            _require('$' not in assignment[1] and '\\' not in assignment[1],
                     f'Makefile2:{first}: unresolved assignment syntax')
            continue
        if re.fullmatch(r'\$\(VERBOSE\)MAKESILENT\s*=\s*-s', code):
            continue
        if re.fullmatch(r'\$\(VERBOSE\)\.SILENT\s*:', code):
            continue
        left, colon, right = code.partition(':')
        left, right = left.strip(' \t'), right.strip(' \t')
        _require(bool(colon), f'Makefile2:{first}: unsupported grammar or directive')
        if left in {'.PHONY', '.SUFFIXES'}:
            _require(re.fullmatch(r'[A-Za-z0-9_./+ \t-]*', right) is not None,
                     f'Makefile2:{first}: unsupported special-target syntax')
            continue
        if left == '%' and right in {'%,v', 'RCS/%', 'RCS/%,v', 'SCCS/s.%', 's.%'}:
            continue  # CMake's disabled VCS implicit rules; never default ALL nodes.
        _require(re.fullmatch(r'[A-Za-z0-9_./+-]+', left) is not None,
                 f'Makefile2:{first}: unsupported rule target syntax')
        _require(not (left.startswith('.') and '/' not in left),
                 f'Makefile2:{first}: unsupported Make special target')
        rules[left].append((first, right))
    return rules, generators[0]


def _all_prerequisites(node, rules):
    _require(node in rules, f'Makefile2: missing reachable rule: {node}')
    prerequisites = set()
    for line, right in rules[node]:
        # A semicolon introduces an inline recipe. Neither it nor tab recipes
        # are interpreted, scanned for commands, executed, or followed as files.
        edges = right.split(';', 1)[0]
        _require(re.fullmatch(r'[A-Za-z0-9_./+ \t-]*', edges) is not None,
                 f'Makefile2:{line}: unsupported reachable prerequisite syntax: {node}')
        for edge in edges.split():
            _require(all(part not in {'', '.', '..'} for part in edge.split('/')),
                     f'Makefile2:{line}: noncanonical reachable prerequisite: {edge}')
            prerequisites.add(edge)
    return prerequisites


def collect_inventory(makefile2_data, codemodel_data, *, configuration, head, tree, architecture):
    """Collect root ALL from two byte strings; caller identity is not authenticated.

    This deliberately refuses unsupported generated syntax. Only explicit ALL
    prerequisite rules contribute targets; registered/help/object lists and
    recipe contents cannot expand coverage. No embedded filename is followed.
    """
    _require(isinstance(makefile2_data, bytes) and isinstance(codemodel_data, bytes),
             'inventory: expected Makefile2 and codemodel bytes')
    identity = _identity({'head': head, 'tree': tree}, 'inventory identity')
    for value, label in ((configuration, 'configuration'), (architecture, 'architecture')):
        _require(isinstance(value, str) and TARGET.fullmatch(value) is not None,
                 f'inventory: invalid {label}')
    aliases, bindings, paths = _codemodel_bindings(codemodel_data, configuration)
    rules, (generator, cmake_version) = _makefile2_rules(makefile2_data)
    state = {}
    pending = [('all', False)]
    while pending:
        node, leaving = pending.pop()
        if leaving:
            state[node] = 2
            continue
        _require(state.get(node) != 1, f'Makefile2: cycle at reachable ALL node: {node}')
        if state.get(node) == 2:
            continue
        _require(node in aliases or node in bindings,
                 f'Makefile2: unknown or unregistered reachable ALL node: {node}')
        prerequisites = _all_prerequisites(node, rules)
        state[node] = 1
        pending.append((node, True))
        pending.extend((edge, False) for edge in sorted(prerequisites, reverse=True))
    reached = sorted((bindings[node] for node in state if node in bindings),
                     key=lambda row: row['name'])
    targets = [row['name'] for row in reached]
    _names(targets, 'collected inventory.targets')
    excluded = sorted({row['name'] for row in bindings.values()} - set(targets))
    return {
        'schema': INVENTORY_SCHEMA, **identity, 'targets': targets,
        'provenance': {
            'method': 'Makefile2 root all prerequisite closure bound to codemodel-v2',
            'inputs': {'makefile2': {'sha256': _digest(makefile2_data)},
                       'codemodel': {'sha256': _digest(codemodel_data)}},
            'configuration': configuration, 'generator': generator, 'cmake_version': cmake_version,
            'architecture': architecture, 'paths': paths,
            'identity_semantics': ('caller-supplied head/tree and architecture; caller asserts '
                                   'real checkout identity and generator/compiler flags; '
                                   'no Git discovery or external authentication'),
            'reachable_all_nodes': sorted(state), 'target_bindings': reached,
            'excluded_registered_targets': excluded,
        },
    }


def _target_refusal(name, error):
    return EvidenceError(f'{name}: {error}', {**error.details, 'target': name})


def _validated_observations(document):
    doc = _object(document, 'observations')
    _require(doc.get('schema') == OBSERVATIONS_SCHEMA and doc.get('status') == 'observed',
             'observations: wrong schema or status')
    _require(doc.get('weight_semantics') == WINDOW_SEMANTICS,
             'observations: wrong measurement semantics')
    rows = doc.get('observations')
    _require(isinstance(rows, list) and bool(rows), 'observations: missing target records')
    first_name = _object(rows[0], 'observation').get('target')
    _names([first_name], 'observation.target')
    try:
        identity = _identity(doc.get('identity'), 'observations.identity')
        identity.update(_run_job(doc['identity'], 'observations.identity'))
        archives = _object(doc.get('archives'), 'observations.archives')
        for key in ('logs_sha256', 'artifacts_sha256'):
            _sha(archives.get(key), 64, f'observations.archives.{key}')
        sources = _object(doc.get('sources'), 'observations.sources')
        for role in (*LOG_ROLES, *ARTIFACT_MEMBERS):
            source = _validate_ref(sources.get(role), f'sources.{role}')
            expected_archive = archives['logs_sha256' if role in LOG_ROLES else 'artifacts_sha256']
            _require(source['archive_sha256'] == expected_archive,
                     f'sources.{role}: archive mismatch')
            if role in LOG_ROLES:
                _require(source['member'].startswith(identity['job'] + '/') and
                         not source['member'].endswith('/'), f'sources.{role}: wrong job member')
            else:
                _require(source['member'] == role, f'sources.{role}: wrong artifact member')
        _require(len({sources[role]['member'] for role in LOG_ROLES}) == len(LOG_ROLES),
                 'observations: duplicate log source members')
        _require(sources['identity-before.log']['sha256'] == sources['identity-after.log']['sha256'],
                 'observations: before/after identity source mismatch')
        contents = _object(doc.get('record_bytes'), 'observations.record_bytes')
        selected = {role: sources[role]['member'] for role in LOG_ROLES}
        logs = {member: _record_bytes(contents, sources[role], f'sources.{role}')
                for role, member in selected.items()}
        artifacts = {name: _record_bytes(contents, sources[name], f'sources.{name}')
                     for name in ARTIFACT_MEMBERS}
        # Re-run the original parser on the cited bytes, including companion
        # payload, actual identity, upload, completion, and timed-work checks.
        reconstructed = _observe_members(
            logs, artifacts, identity=identity, selected=selected,
            logs_sha256=archives['logs_sha256'], artifacts_sha256=archives['artifacts_sha256'])
        for role in (*LOG_ROLES, *ARTIFACT_MEMBERS):
            _require(sources[role] == reconstructed['sources'][role],
                     f'sources.{role}: declared reference differs from actual record bytes')
        upload = _object(doc.get('upload'), 'observations.upload')
        _require(upload == reconstructed['upload'],
                 'observations: declared upload identity differs from actual record bytes')
        totals = _object(doc.get('phase_totals'), 'observations.phase_totals')
        for name in ('configure', 'build'):
            total = _object(totals.get(name), f'phase_totals.{name}')
            _require(type(total.get('exit_status')) is int and total['exit_status'] == 0,
                     f'phase_totals.{name}: timed work did not exit zero')
            for key in ('wall_seconds', 'user_seconds', 'system_seconds'):
                _number(total.get(key), f'phase_totals.{name}.{key}', positive=key == 'wall_seconds')
            _require(total == reconstructed['phase_totals'][name],
                     f'phase_totals.{name}: declared total/source differs from actual timed-work bytes')
    except EvidenceError as error:
        # A shared source failure prevents validating the first cited target.
        raise _target_refusal(first_name, error) from error
    actual_rows = {row['target']: row for row in reconstructed['observations']}
    expected_provenance = {
        **identity, 'logs_sha256': archives['logs_sha256'],
        'artifacts_sha256': archives['artifacts_sha256'],
        'member': sources['build']['member'], 'member_sha256': sources['build']['sha256'],
    }
    seen = set()
    validated = []
    for row in rows:
        _object(row, 'observation')
        name = row.get('target')
        _names([name], 'observation.target')
        try:
            _require(name not in seen, 'duplicate measurement')
            seen.add(name)
            provenance = _object(row.get('provenance'), 'measurement provenance')
            _require(all(provenance.get(key) == value for key, value in expected_provenance.items()),
                     'wrong or missing measurement provenance')
            status = row.get('status')
            _require(status in ('measured', 'unmeasured', 'ambiguous'), 'invalid observation status')
            if status == 'measured':
                first, last = row.get('first_line'), row.get('last_line')
                _require(type(first) is int and type(last) is int and 0 < first < last,
                         'invalid measurement lines')
                duration = (_timestamp(row.get('last_timestamp'), 'last_timestamp') -
                            _timestamp(row.get('first_timestamp'), 'first_timestamp'))
                seconds = _number(row.get('seconds'), 'seconds', positive=True)
                _require(duration > 0 and seconds == duration,
                         'measurement seconds disagree with timestamp window')
            else:
                _require('seconds' not in row and isinstance(row.get('reasons'), list) and
                         bool(row['reasons']) and
                         all(isinstance(reason, str) and reason for reason in row['reasons']),
                         'unmeasured/ambiguous observation must not carry a weight')
                for field in ('building_lines', 'completion_lines'):
                    lines = row.get(field)
                    _require(isinstance(lines, list) and
                             all(type(line) is int and line > 0 for line in lines) and
                             lines == sorted(set(lines)), f'malformed {field}')
            actual = actual_rows.get(name)
            _require(actual is not None, 'target is absent from the cited build record bytes')
            if status == 'measured':
                _require(actual['status'] == 'measured',
                         'cited build record has an ambiguous or unmeasured target window')
            different = sorted(key for key in row.keys() | actual.keys()
                               if key not in row or key not in actual or row[key] != actual[key])
            _require(not different, 'declared observation differs from the cited build record bytes',
                     {'mismatched_fields': different})
            # The scheduling weights come from recomputation, never from the
            # supplied scalar row, even after all comparisons have succeeded.
            validated.append(copy.deepcopy(actual))
        except EvidenceError as error:
            raise _target_refusal(name, error) from error
    return validated


def plan_shards(inventory, observation_documents):
    """Deterministic descending-weight assignment; never synthesize a weight."""
    current = _inventory(inventory)
    _require(len(current) >= 2, 'two nonempty shards require at least two current ALL targets')
    _require(isinstance(observation_documents, list), 'expected a list of observation documents')
    measured = defaultdict(list)
    obsolete = []
    unusable = []
    for document in observation_documents:
        for row in _validated_observations(document):
            if row['target'] not in current:
                obsolete.append(copy.deepcopy(row))
            elif row['status'] == 'measured':
                measured[row['target']].append(row)
            else:
                unusable.append(copy.deepcopy(row))
    missing = sorted(name for name in current if not measured[name])
    ambiguous = sorted(name for name in current if len(measured[name]) > 1)
    _require(not missing and not ambiguous, 'current ALL targets lack unique usable measured weights',
             {'missing_targets': missing, 'ambiguous_targets': ambiguous,
              'obsolete_targets': sorted({row['target'] for row in obsolete}),
              'unusable_observations': unusable})
    weights = [measured[name][0] for name in current]
    weights.sort(key=lambda row: (-_number(row['seconds'], row['target'], positive=True),
                                  row['target']))
    assignments = [[], []]
    sums = [Decimal(0), Decimal(0)]
    for row in weights:
        index = 0 if sums[0] <= sums[1] else 1
        assignments[index].append(row['target'])
        sums[index] += _number(row['seconds'], row['target'], positive=True)
    _require(all(assignments), 'two nonempty shard assignments are required')
    return {
        'schema': 'clang-build-plan-v1', 'status': 'planned',
        'inventory': copy.deepcopy(inventory), 'weight_semantics': SUM_SEMANTICS,
        'algorithm': 'descending seconds, target-name tie; smaller sum, then shard 1',
        'shards': [{'shard': index + 1, 'targets': names,
                    'scheduling_sum_seconds': _json_number(sums[index])}
                   for index, names in enumerate(assignments)],
        'weights': copy.deepcopy(weights),
        'observation_sources': [{key: copy.deepcopy(value) for key, value in document.items()
                                 if key != 'observations'}
                                for document in observation_documents],
        'obsolete_observations': sorted(obsolete, key=lambda row: row['target']),
        'unusable_observations': sorted(unusable, key=lambda row: row['target']),
    }


def _embedded_text(value, label):
    value = _validate_ref(value, label)
    text = value.get('text')
    _require(isinstance(text, str), f'{label}: missing actual evidence text')
    try:
        data = text.encode('utf-8')
    except UnicodeEncodeError as error:
        raise EvidenceError(f'{label}: invalid UTF-8 text') from error
    _require(_digest(data) == value['sha256'], f'{label}: evidence SHA256 mismatch')
    return data


def _checked_result(manifest, inventory_identity, assigned, prerequisites):
    result = _object(manifest.get('result'), 'shard.result')
    _require(_identity(result, 'shard.result') == inventory_identity, 'shard result: wrong head/tree')
    run_job = _run_job(result, 'shard.result')
    _require(result.get('status') == 'success', 'shard result: missing or unsuccessful result')
    _require(type(result.get('exit_status')) is int and result['exit_status'] == 0,
             'shard result: actual work exit_status must be zero')
    build_log = result.get('build_log')
    build_time = result.get('build_time')
    log_data = _embedded_text(build_log, 'shard.result.build_log')
    time_data = _embedded_text(build_time, 'shard.result.build_time')
    _require(build_log['member'].startswith(run_job['job'] + '/') and
             not build_log['member'].endswith('/'), 'shard build evidence: wrong job member')
    _require(build_time['member'].rsplit('/', 1)[-1] == 'build.time',
             'shard timing evidence: expected build.time member')
    timing = _phase(time_data, 'shard build.time')
    records = _records(log_data, 'shard build log')
    events = _build_events(records)
    completed = {name: event['ends'] for name, event in events.items() if event['ends']}
    _require(bool(completed), 'shard result: no actual Built target evidence')
    duplicates = sorted(name for name, ends in completed.items() if len(ends) != 1)
    missing = sorted(assigned - completed.keys())
    undeclared = sorted(events.keys() - assigned - prerequisites)
    incomplete = sorted(name for name, event in events.items() if not event['ends'])
    reversed_targets = sorted(
        name for name, event in events.items()
        if event['starts'] and event['ends'] and
        event['ends'][0]['line'] <= event['starts'][-1]['line'])
    _require(not duplicates and not missing and not undeclared and
             not incomplete and not reversed_targets,
             'shard result: actual built-target evidence disagrees with assignment/prerequisites',
             {'duplicate_built_targets': duplicates, 'missing_built_targets': missing,
              'undeclared_built_targets': undeclared, 'incomplete_targets': incomplete,
              'reversed_target_completions': reversed_targets})
    return {
        **run_job, 'phase_total': timing,
        'sources': {'build_log': {key: value for key, value in build_log.items() if key != 'text'},
                    'build_time': {key: value for key, value in build_time.items() if key != 'text'}},
        'built_targets': [{'target': name, 'line': ends[0]['line'],
                           'timestamp': ends[0]['timestamp']}
                          for name, ends in sorted(completed.items())],
    }


def check_shards(inventory, manifests):
    """Independently compare the exact current ALL set and both real results."""
    current = _inventory(inventory)
    identity = _identity(inventory, 'inventory')
    _require(isinstance(manifests, list) and len(manifests) == 2,
             'exactly two per-shard assignment/result manifests are required')
    ownership = set()
    shard_ids = set()
    rows = []
    for manifest in manifests:
        manifest = _object(manifest, 'shard manifest')
        _require(manifest.get('schema') == RESULT_SCHEMA, 'shard manifest: wrong schema')
        _require(_identity(manifest, 'shard manifest') == identity, 'shard manifest: wrong head/tree')
        shard = manifest.get('shard')
        _require(type(shard) is int and shard in (1, 2) and shard not in shard_ids,
                 'missing, malformed, or duplicate shard')
        shard_ids.add(shard)
        assigned = _names(manifest.get('targets'), f'shard {shard}.targets')
        prerequisites = _names(manifest.get('prerequisites'),
                               f'shard {shard}.prerequisites', allow_empty=True)
        _require(not assigned & prerequisites, 'prerequisites must be separate from owned targets')
        _require(not ownership & assigned, 'duplicate target ownership',
                 {'duplicate_targets': sorted(ownership & assigned)})
        ownership.update(assigned)
        rows.append({'shard': shard, 'targets': sorted(assigned),
                     'prerequisites': sorted(prerequisites),
                     'result': _checked_result(manifest, identity, assigned, prerequisites)})
    missing, extra = sorted(current - ownership), sorted(ownership - current)
    _require(shard_ids == {1, 2} and not missing and not extra,
             'assignment union does not equal the current default ALL inventory',
             {'missing_targets': missing, 'extra_targets': extra})
    jobs = [(row['result']['run_id'], row['result']['job']) for row in rows]
    _require(len(set(jobs)) == 2, 'both shards reuse the same job result')
    return {
        'schema': 'clang-build-coverage-v1', 'status': 'coverage_checked',
        'scope': 'current ALL ownership and supplied build-result evidence',
        'inventory': copy.deepcopy(inventory), 'covered_targets': sorted(current),
        'shards': sorted(rows, key=lambda row: row['shard']),
    }


def _safe_path(value):
    raw = str(value)
    _refuse_sensitive_name(raw)
    _require('\x00' not in raw, 'refused path: NUL')
    _refuse_sensitive_name(os.path.normpath(raw))
    path = Path(raw).resolve()
    _refuse_sensitive_name(str(path))
    return path


def _output_path(value, scratch):
    path = _safe_path(value)
    _require(path != scratch and scratch in path.parents, 'output must be inside --scratch-dir')
    _require(path.parent.is_dir(), f'output parent directory does not exist: {path.parent}')
    _require(not path.exists() and not path.is_symlink(), f'output must be fresh: {path}')
    return path


def _no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, f'duplicate JSON key: {key}')
        result[key] = value
    return result


def _nonfinite_constant(value):
    raise EvidenceError(f'nonfinite JSON constant: {value}')


def _finite_json(value):
    if isinstance(value, float):
        _require(math.isfinite(value), 'nonfinite JSON number')
    elif isinstance(value, dict):
        for item in value.values():
            _finite_json(item)
    elif isinstance(value, list):
        for item in value:
            _finite_json(item)


def _read_json(path):
    try:
        value = json.loads(_text(path.read_bytes(), str(path)),
                           object_pairs_hook=_no_duplicate_keys,
                           parse_constant=_nonfinite_constant)
    except (json.JSONDecodeError, ValueError) as error:
        if isinstance(error, EvidenceError):
            raise
        raise EvidenceError(f'{path}: malformed JSON: {error}') from error
    _finite_json(value)
    return value


def _encode(value):
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + '\n').encode('utf-8')


def _stage(path, data):
    """Keep the pending file and publish only a fully written inode, exclusively."""
    pending = path.parent / ('.pending-' + uuid.uuid4().hex + '-' + path.name)
    print(json.dumps({'write_pending': str(pending), 'publish': str(path)}), file=sys.stderr)
    with pending.open('xb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    return pending


def _publish(path, data):
    pending = _stage(path, data)
    os.link(pending, path)


def _parser():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inventory = commands.add_parser('inventory', help='collect the supplied generated root ALL graph')
    inventory.add_argument('--makefile2', required=True)
    inventory.add_argument('--codemodel', required=True)
    inventory.add_argument('--configuration', required=True)
    inventory.add_argument('--head', required=True)
    inventory.add_argument('--tree', required=True)
    inventory.add_argument('--architecture', required=True)
    observe = commands.add_parser('observe', help='validate the held ZIPs and emit observed windows')
    observe.add_argument('--logs-zip', required=True)
    observe.add_argument('--artifacts-zip', required=True)
    observe.add_argument('--logs-sha256', required=True)
    observe.add_argument('--artifacts-sha256', required=True)
    observe.add_argument('--run-id', required=True)
    observe.add_argument('--job', required=True)
    observe.add_argument('--head', required=True)
    observe.add_argument('--tree', required=True)
    for role in LOG_ROLES:
        observe.add_argument(f'--{role}-member', help='exact member, overriding the documented default')
    plan = commands.add_parser('plan', help='partition the externally collected current ALL inventory')
    plan.add_argument('--inventory', required=True)
    plan.add_argument('--observations', action='append', required=True)
    check = commands.add_parser('check', help='check current ALL ownership and both shard results')
    check.add_argument('--inventory', required=True)
    check.add_argument('--shard', action='append', required=True,
                       help='one combined assignment/result manifest; supply exactly twice')
    for command in (inventory, observe, plan, check):
        command.add_argument('--scratch-dir', required=True,
                             help='existing caller scratch directory for all writes')
        command.add_argument('--output', required=True, help='fresh JSON result path inside scratch')
        command.add_argument('--report', required=True, help='fresh JSON diagnostic path inside scratch')
    return parser


def main(argv=None):
    args = _parser().parse_args(argv)
    raw_inputs = ([args.makefile2, args.codemodel] if args.command == 'inventory' else
                  [args.logs_zip, args.artifacts_zip] if args.command == 'observe' else
                  [args.inventory, *(args.observations if args.command == 'plan' else args.shard)])
    # List all command-selected files before following or opening any of them.
    print(json.dumps({'inputs': raw_inputs, 'output': args.output, 'report': args.report,
                      'scratch': args.scratch_dir}), file=sys.stderr)
    report_path = None
    try:
        scratch = _safe_path(args.scratch_dir)
        _require(scratch.is_dir(), '--scratch-dir must be an existing directory')
        output_path = _output_path(args.output, scratch)
        report_path = _output_path(args.report, scratch)
        _require(output_path != report_path, '--output and --report must differ')
        inputs = [_safe_path(value) for value in raw_inputs]
        _require(output_path not in inputs and report_path not in inputs,
                 'output/report must not alias any input')
        print(json.dumps({'resolved_inputs': [str(path) for path in inputs]}), file=sys.stderr)
        if args.command == 'inventory':
            result = collect_inventory(
                inputs[0].read_bytes(), inputs[1].read_bytes(),
                configuration=args.configuration, head=args.head, tree=args.tree,
                architecture=args.architecture)
        elif args.command == 'observe':
            overrides = {role: value for role in LOG_ROLES
                         if (value := getattr(args, f'{role}_member')) is not None}
            result = observe_archives(
                inputs[0].read_bytes(), inputs[1].read_bytes(),
                logs_sha256=args.logs_sha256, artifacts_sha256=args.artifacts_sha256,
                run_id=args.run_id, job=args.job, head=args.head, tree=args.tree,
                log_members=overrides)
        elif args.command == 'plan':
            result = plan_shards(_read_json(inputs[0]),
                                 [_read_json(path) for path in inputs[1:]])
        else:
            result = check_shards(_read_json(inputs[0]),
                                  [_read_json(path) for path in inputs[1:]])
        encoded = _encode(result)
        pending = _stage(output_path, encoded)
        _publish(report_path, _encode({
            'schema': 'clang-build-helper-report-v1', 'command': args.command,
            'status': 'validated', 'output': str(output_path), 'output_sha256': _digest(encoded),
            'meaning': 'Validation finished; the result is only the separately published output.',
        }))
        # No fallible work follows publication. A report alone is never a result.
        os.link(pending, output_path)
        return 0
    except (EvidenceError, OSError, ValueError, OverflowError, RecursionError) as error:
        failure = {'schema': 'clang-build-helper-report-v1', 'command': args.command,
                   'status': 'error', 'error': str(error),
                   'details': error.details if isinstance(error, EvidenceError) else {}}
        print(json.dumps(failure, allow_nan=False), file=sys.stderr)
        if report_path is not None and not report_path.exists():
            try:
                _publish(report_path, _encode(failure))
            except OSError as report_error:
                print(json.dumps({'status': 'error', 'report_error': str(report_error)}),
                      file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
