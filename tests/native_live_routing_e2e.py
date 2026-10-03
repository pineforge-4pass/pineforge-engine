"""Local receivers exercise immutable routing, delivery isolation and ledger readers."""
import copy
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


runner, library = sys.argv[1:3]
secrets = {'MAIN_HMAC': 'main-loopback-secret', 'ENTRY_HMAC': 'entry-loopback-secret',
           'EXIT_HMAC': 'exit-loopback-secret'}
environment = dict(os.environ, **secrets)


class Receiver:
    def __init__(self, secret, port=0):
        self.secret = secret
        self.mode = 'ok'
        self.rows = []
        self.errors = []
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers['Content-Length']))
                    action = json.loads(body)
                    signature = 'sha256=' + hmac.new(owner.secret.encode(), body, hashlib.sha256).hexdigest()
                    assert self.headers['X-PineForge-Signature'] == signature
                    assert self.headers['X-PineForge-Event-Id'] == action['event_id']
                    assert self.headers['Idempotency-Key'] == action.get('delivery_id', action['event_id'])
                    with owner.lock:
                        owner.rows.append({'body': body, 'action': action, 'key': self.headers['Idempotency-Key'],
                                           'at': time.monotonic()})
                        mode = owner.mode
                    if mode == 'reset':
                        self.connection.shutdown(socket.SHUT_RDWR)
                        return
                    if mode == 'hang' or (mode == 'hang-first' and action['sequence'] == 1):
                        time.sleep(4)
                    status = 500 if mode == 'fail' or (mode == 'fail-first' and action['sequence'] == 2) else 204
                    self.send_response(status)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except Exception as error:
                    with owner.lock:
                        owner.errors.append(str(error))

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
        self.server.daemon_threads = True
        self.server.request_queue_size = 128
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server.server_port}/actions'

    def clear(self):
        with self.lock:
            self.rows.clear()
            self.errors.clear()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('timed out waiting for local test condition')


def query(ledger, sql):
    with sqlite3.connect(ledger, timeout=2) as database:
        return database.execute(sql).fetchall()


def committed_actions(ledger):
    if not ledger.exists():
        return False
    try:
        return query(ledger, 'SELECT count(*) FROM events')[0][0] == 4
    except sqlite3.OperationalError:
        return False


def invoke(arguments, expected=0, env=None, auto_deployment=True):
    if arguments[0] == 'redeliver' and '--deployment' not in arguments and auto_deployment:
        ledger = arguments[arguments.index('--ledger') + 1]
        arguments = arguments + ['--deployment', query(ledger, 'SELECT identity FROM metadata')[0][0]]
    process = subprocess.run([runner] + arguments, capture_output=True, text=True,
                             env=environment if env is None else env, timeout=25)
    assert process.returncode == expected, (arguments, process.returncode, process.stdout, process.stderr)
    for secret in secrets.values():
        assert secret not in process.stdout + process.stderr
    return process


with tempfile.TemporaryDirectory(prefix='pineforge-routing-e2e-') as directory:
    root = Path(directory)
    warmup = root / 'warmup.csv'
    warmup.write_text('timestamp,open,high,low,close,volume\n' +
                      ''.join(f'{index * 60000},100,101,99,100,4\n' for index in range(3)))
    events = [{'type': 'bar', 'bar': {'ts_open': index * 60000, 'o': 100 + index, 'h': 102 + index,
                                    'l': 99 + index, 'c': 101 + index, 'v': 4, 'trade_count': 4}}
              for index in range(3, 27)]
    feed = root / 'feed.jsonl'
    feed.write_text(''.join(json.dumps(event) + '\n' for event in events))
    base = ['run', '--strategy', library, '--warmup', str(warmup), '--script-tf', '3',
            '--symbol', 'TEST:MOCK', '--mode', 'bars', '--feed', str(feed), '--allow-insecure-http']
    receivers = [Receiver(secrets[name]) for name in ('MAIN_HMAC', 'ENTRY_HMAC', 'EXIT_HMAC')]
    main_receiver, entries, exits = receivers

    def run(ledger, routes=None, extra=None, expected=0):
        arguments = base + ['--ledger', str(ledger)]
        if routes is not None:
            arguments += ['--webhook-routes', str(routes)]
        return invoke(arguments + (extra or []), expected)

    def save_routes(name, document):
        path = root / (name + '.json')
        path.write_text(json.dumps(document, sort_keys=True, separators=(',', ':')))
        return path

    def stored(ledger):
        return (query(ledger, 'SELECT input_index,canonical_json,state_hash FROM inputs ORDER BY input_index'),
                query(ledger, 'SELECT ordinal,event_id,payload FROM events ORDER BY ordinal'))

    try:
        journal = root / 'journal.sqlite'
        partial = json.loads(run(journal, extra=['--max-events', '2']).stdout)
        assert partial['inputs_committed'] == 2
        follower = subprocess.Popen([runner, 'actions', '--ledger', str(journal), '--after', '0', '--follow'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        streamed = queue.Queue()
        reader = threading.Thread(target=lambda: [streamed.put(line.rstrip('\n')) for line in follower.stdout], daemon=True)
        reader.start()
        run(journal)
        observed = [streamed.get(timeout=5) for _ in range(4)]
        expected_payloads = [row[0] for row in query(journal, 'SELECT payload FROM events ORDER BY ordinal')]
        assert observed == expected_payloads
        follower.send_signal(signal.SIGTERM)
        follower.wait(timeout=5)
        reader.join(timeout=2)
        assert follower.returncode == 130 and not follower.stderr.read()
        exported = invoke(['actions', '--ledger', str(journal), '--after', '2'])
        assert exported.stdout.splitlines() == expected_payloads[2:]
        assert json.loads(invoke(['status', '--ledger', str(journal)]).stdout)['targets'] == {}
        assert not query(journal, 'SELECT * FROM delivery_log')
        assert all(json.loads(payload)['schema_version'].endswith('/v1') for payload in expected_payloads)
        print('PASS journal-only runner, optional webhook, actions export and concurrent actions --follow', flush=True)

        document = {'schema_version': 1, 'default_target': 'main',
                    'targets': {'main': {'url': main_receiver.url, 'secret_env': 'MAIN_HMAC'},
                                'entries': {'url': entries.url, 'secret_env': 'ENTRY_HMAC'},
                                'exits': {'url': exits.url, 'secret_env': 'EXIT_HMAC'}},
                    'rules': [{'match': {'order_id': 'Long', 'kind': 'entry', 'side': 'long'}, 'target': 'entries'},
                              {'match': {'kind': 'entry'}, 'target': None},
                              {'match': {'kind': 'exit', 'side': 'long'}, 'target': 'exits'}]}
        routes = save_routes('routes', document)
        ledger = root / 'routed.sqlite'
        result = json.loads(run(ledger, routes).stdout)
        assert result['webhooks_delivered'] == 4
        assert not main_receiver.rows and len(entries.rows) == len(exits.rows) == 2
        payloads = [json.loads(row[0]) for row in query(ledger, 'SELECT payload FROM events ORDER BY ordinal')]
        assert [action['target_id'] for action in payloads] == ['entries', 'exits', 'entries', 'exits']
        assert all(action['schema_version'] == 'pineforge-native-order-action/v2' for action in payloads)
        assert all(action['order']['side'] == 'long' for action in payloads)
        for action in payloads:
            canonical = json.dumps({'event_id': action['event_id'], 'target_id': action['target_id']},
                                   sort_keys=True, separators=(',', ':')).encode()
            assert action['delivery_id'] == hashlib.sha256(canonical).hexdigest()
            assert action['order']['kind'] == action['order']['leg']
        immutable = stored(ledger)
        run(ledger, routes, ['--webhook-url', main_receiver.url, '--webhook-secret-env', 'MAIN_HMAC'])
        assert stored(ledger) == immutable and len(entries.rows) == len(exits.rows) == 2
        changed = copy.deepcopy(document)
        changed['rules'].reverse()
        refusal = run(ledger, save_routes('changed', changed), expected=1)
        assert 'routing' in refusal.stderr and 'new ledger' in refusal.stderr
        assert stored(ledger) == immutable
        print('PASS multiple targets, first-match B1 selectors, v2 identity, HMAC and routing identity refusal', flush=True)

        null_document = copy.deepcopy(document)
        null_document['default_target'] = None
        null_document['rules'] = [{'match': {'kind': 'entry'}, 'target': None},
                                  {'match': {'kind': 'exit'}, 'target': 'exits'}]
        exits.clear()
        null_ledger = root / 'null.sqlite'
        run(null_ledger, save_routes('null', null_document))
        assert len(exits.rows) == 2
        assert query(null_ledger, 'SELECT target_id FROM event_routes ORDER BY ordinal') == [(None,), ('exits',), (None,), ('exits',)]
        assert len(query(null_ledger, "SELECT * FROM delivery_log WHERE phase='completed'")) == 2
        print('PASS null rule/default target journals without sending or losing actions', flush=True)

        invalid_documents = []
        for selector in ({'alert_message': 'x'}, {'kind': 'close'}, {'side': 'buy'}, {'order_id': 1}, {'venue': 'x'}):
            invalid = copy.deepcopy(document)
            invalid['rules'][0]['match'] = selector
            invalid_documents.append((invalid, 'strategy-metadata extension' if 'alert_message' in selector or selector.get('kind') == 'close' else None))
        for delivery in ({'max_in_flight': 0}, {'transport_retries': 3}, {'connect_timeout_ms': -1},
                         {'total_timeout_ms': 0}, {'retry_backoff_ms': [10]}, {'unknown': 1}):
            invalid = copy.deepcopy(document)
            invalid['delivery'] = delivery
            invalid_documents.append((invalid, None))
        invalid = copy.deepcopy(document); invalid['extra'] = True
        invalid_documents.append((invalid, 'unknown'))
        invalid = copy.deepcopy(document); invalid['rules'][0]['target'] = 'undefined'
        invalid_documents.append((invalid, 'undefined'))
        invalid = copy.deepcopy(document); invalid['targets']['entries']['url'] = 'https://user:secret@receiver.example/'
        invalid_documents.append((invalid, 'user information'))
        for index, (invalid, message) in enumerate(invalid_documents):
            path = root / f'invalid-{index}.sqlite'
            process = run(path, save_routes(f'invalid-{index}', invalid), expected=1)
            assert not path.exists()
            if message:
                assert message in process.stderr, process.stderr
        missing_secret = dict(environment); missing_secret.pop('ENTRY_HMAC')
        path = root / 'missing-secret.sqlite'
        process = invoke(base + ['--ledger', str(path), '--webhook-routes', str(routes)], expected=1, env=missing_secret)
        assert 'missing or empty' in process.stderr and not path.exists()
        path = root / 'cli-conflict.sqlite'
        assert 'agree' in run(path, routes, ['--webhook-url', entries.url], expected=1).stderr
        assert not path.exists()
        print('PASS strict startup validation, B2 refusal, missing secrets and CLI conflicts before input', flush=True)

        isolation_document = copy.deepcopy(document)
        isolation_document['delivery'] = {'max_in_flight': 8, 'connect_timeout_ms': 100,
                                          'total_timeout_ms': 500, 'transport_retries': 2,
                                          'retry_backoff_ms': [100, 200]}
        isolation = save_routes('isolation', isolation_document)
        entries.clear(); exits.clear()
        entries.mode = 'hang-first'; exits.mode = 'fail-first'
        isolation_ledger = root / 'isolation.sqlite'
        process = run(isolation_ledger, isolation)
        newer = [row for row in entries.rows if row['action']['sequence'] == 3]
        assert len(newer) == 1
        first_completed = query(isolation_ledger, "SELECT min(ended_at) FROM delivery_log WHERE target_id='entries' AND phase='completed' AND success=0")[0][0]
        initial_starts = query(isolation_ledger, "SELECT started_at FROM delivery_log WHERE phase='started' AND attempt=1")
        assert len(initial_starts) == 4 and all(row[0] < first_completed for row in initial_starts)
        assert len([row for row in exits.rows if row['action']['sequence'] == 2]) == 1
        assert len([row for row in entries.rows if row['action']['sequence'] == 1]) == 3
        assert query(isolation_ledger, 'SELECT count(*) FROM inputs')[0][0] == len(events)
        warning = ('pineforge-live: warning: compiled strategy lacks checked settings; '
                   'legacy settings may be ignored or defaulted')
        log_lines = process.stderr.splitlines()
        assert log_lines.count(warning) == 1, process.stderr
        errors = [json.loads(line) for line in log_lines if line != warning]
        assert len(errors) == 4 and all(row['event'] == 'webhook_delivery_error' for row in errors)
        status = json.loads(invoke(['status', '--ledger', str(isolation_ledger)]).stdout)['targets']
        assert status['entries']['failed'] == 3 and status['exits']['failed'] == 1
        assert status['entries']['last_error']['category'] == 'timeout'
        assert status['exits']['last_error']['http_status'] == 500
        assert status['entries']['last_success'] is not None and status['main']['sent'] == 0
        attempts = query(isolation_ledger, "SELECT event_id,attempt,started_at,ended_at,http_status,error_category FROM delivery_log WHERE phase='completed'")
        assert len(attempts) == 6 and all(row[3] >= row[2] for row in attempts)
        run(isolation_ledger, isolation)
        assert len(entries.rows) == 4 and len(exits.rows) == 2
        entries.mode = exits.mode = 'ok'
        result = json.loads(invoke(['redeliver', '--ledger', str(isolation_ledger), '--target', 'entries', '--failed-only']).stdout)
        assert result == {'selected': 1, 'delivered': 1, 'failed': 0, 'pending': 0}
        repeated = [row for row in entries.rows if row['action']['sequence'] == 1]
        assert len(repeated) == 4 and len({row['body'] for row in repeated}) == len({row['key'] for row in repeated}) == 1
        rotated_environment = dict(environment, ENTRY_HMAC='rotated-loopback-signing-key')
        entries.secret = rotated_environment['ENTRY_HMAC']
        invoke(['redeliver', '--ledger', str(isolation_ledger), '--target', 'entries', '--from', '1'],
               env=rotated_environment)
        assert all(row['body'] == next(original['body'] for original in repeated
                                      if original['action']['sequence'] == row['action']['sequence'])
                   for row in entries.rows if row['action']['sequence'] == 1)
        entries.secret = secrets['ENTRY_HMAC']
        result = json.loads(invoke(['redeliver', '--ledger', str(isolation_ledger), '--target', 'exits', '--from', '3', '--failed-only']).stdout)
        assert result['selected'] == 0
        invoke(['redeliver', '--ledger', str(isolation_ledger), '--target', 'exits', '--failed-only'])
        assert len(exits.rows) == 3
        with sqlite3.connect(isolation_ledger) as database:
            for statement in ('UPDATE delivery_log SET error_category=\'hidden\'', 'DELETE FROM delivery_log'):
                try:
                    database.execute(statement)
                except sqlite3.DatabaseError as error:
                    assert 'append-only' in str(error)
                else:
                    raise AssertionError('delivery log was mutable')
        print('PASS hanging/500 targets never delay later actions, bounded retries, append-only rows, status and stable redelivery', flush=True)

        disconnected = copy.deepcopy(isolation_document)
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            refused_port = reserved.getsockname()[1]
        disconnected['targets']['entries']['url'] = f'http://127.0.0.1:{refused_port}/actions'
        disconnected['delivery'].pop('retry_backoff_ms')
        disconnected_routes = save_routes('disconnected', disconnected)
        exits.clear()
        disconnected_ledger = root / 'disconnected.sqlite'
        run(disconnected_ledger, disconnected_routes)
        initial_starts = query(disconnected_ledger, "SELECT started_at FROM delivery_log WHERE phase='started' AND attempt=1")
        assert max(row[0] for row in initial_starts) - min(row[0] for row in initial_starts) < 900
        failed = query(disconnected_ledger, "SELECT event_id,attempt,started_at FROM delivery_log WHERE phase='completed' AND target_id='entries' ORDER BY log_id")
        for event_id in {row[0] for row in failed}:
            timeline = [row[2] for row in failed if row[0] == event_id]
            assert len(timeline) == 3 and timeline[1] - timeline[0] >= 900 and timeline[2] - timeline[1] >= 1900
        print('PASS refused connections capped at two retries with default 1s/2s backoff and isolated healthy targets', flush=True)

        deterministic = copy.deepcopy(isolation_document)
        deterministic['default_target'] = 'main'
        deterministic['rules'] = []
        determinism_routes = save_routes('determinism', deterministic)
        snapshots = []
        for mode in ('ok', 'hang', 'fail', 'reset'):
            main_receiver.clear(); main_receiver.mode = mode
            computation = root / f'compute-{mode}.sqlite'
            run(computation, determinism_routes)
            snapshots.append(stored(computation))
        main_port = main_receiver.server.server_port
        main_receiver.close()
        run(root / 'compute-absent.sqlite', determinism_routes)
        snapshots.append(stored(root / 'compute-absent.sqlite'))
        main_receiver = Receiver(secrets['MAIN_HMAC'], main_port)
        receivers[0] = main_receiver
        assert all(snapshot == snapshots[0] for snapshot in snapshots)
        print('PASS identical actions, state hashes, ledger inputs and exact payload bytes for fast/slow/failing/absent receivers', flush=True)

        crash_document = copy.deepcopy(isolation_document)
        crash_document['targets']['main']['url'] = main_receiver.url
        crash_document['rules'] = []
        crash_document['delivery']['max_in_flight'] = 1
        crash_document['delivery']['total_timeout_ms'] = 5000
        crash_document['delivery']['transport_retries'] = 0
        crash_routes = save_routes('crash', crash_document)
        crash_ledger = root / 'crash.sqlite'
        main_receiver.mode = 'hang'
        process = subprocess.Popen([runner] + base + ['--ledger', str(crash_ledger), '--webhook-routes', str(crash_routes)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            def committed():
                if not crash_ledger.exists():
                    return False
                try:
                    return query(crash_ledger, 'SELECT count(*) FROM events')[0][0] == 4 and bool(main_receiver.rows)
                except sqlite3.OperationalError:
                    return False
            wait_for(committed)
            assert query(crash_ledger, "SELECT count(*) FROM delivery_log WHERE phase='completed'")[0][0] == 0
            assert query(crash_ledger, "SELECT count(*) FROM delivery_log WHERE phase='started'")[0][0] == 1
            before_crash = stored(crash_ledger)
            process.kill()
            process.communicate(timeout=5)
            assert process.returncode == -signal.SIGKILL
        finally:
            if process.poll() is None:
                process.kill(); process.wait(timeout=5)
        main_receiver.clear(); main_receiver.mode = 'ok'
        run(crash_ledger, crash_routes)
        assert [row['action']['sequence'] for row in main_receiver.rows] == [1, 2, 3, 4]
        assert stored(crash_ledger) == before_crash
        run(crash_ledger, crash_routes)
        assert len(main_receiver.rows) == 4
        print('PASS SIGKILL between commit/send and mid-request resumes each unsent action once after replay', flush=True)

        deployment = query(crash_ledger, 'SELECT identity FROM metadata')[0][0]
        assert 'requires --deployment' in invoke(['redeliver', '--ledger', str(crash_ledger), '--target', 'main'],
                                                 expected=1, auto_deployment=False).stderr
        for command in ('redeliver', 'actions', 'status'):
            arguments = [command, '--ledger', str(crash_ledger), '--deployment', 'wrong']
            if command == 'redeliver':
                arguments += ['--target', 'main']
            assert 'identity mismatch' in invoke(arguments, expected=1).stderr
        entries.mode = 'fail'
        chosen_environment = dict(environment)
        chosen_environment.pop('MAIN_HMAC'); chosen_environment.pop('EXIT_HMAC')
        failed_redelivery = json.loads(invoke(['redeliver', '--ledger', str(isolation_ledger), '--target', 'entries'],
                                              expected=2, env=chosen_environment).stdout)
        assert failed_redelivery == {'selected': 2, 'delivered': 0, 'failed': 2, 'pending': 0}
        entries.mode = 'ok'
        print('PASS deployment checks, failed redelivery exit/counts, selected-target secrets and saturation commit order', flush=True)

        stdin_base = list(base)
        stdin_base[stdin_base.index('--feed') + 1] = '-'
        fatal_document = copy.deepcopy(isolation_document)
        fatal_document['delivery'].update(max_in_flight=1, total_timeout_ms=2000)
        fatal_routes = save_routes('fatal', fatal_document)
        for mode in ('ok', 'hang'):
            entries.clear(); exits.clear(); entries.mode = exits.mode = mode
            fatal_ledger = root / f'fatal-{mode}.sqlite'
            fatal = subprocess.Popen([runner] + stdin_base + ['--ledger', str(fatal_ledger), '--webhook-routes', str(fatal_routes)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, env=environment)
            try:
                fatal.stdin.write(feed.read_text()); fatal.stdin.flush()
                wait_for(lambda: committed_actions(fatal_ledger))
                started = time.monotonic()
                stdout, stderr = fatal.communicate(input='malformed\n', timeout=5)
                assert fatal.returncode == 1 and time.monotonic() - started < 2.4, stderr
                status = json.loads(invoke(['status', '--ledger', str(fatal_ledger)]).stdout)['targets']
                pending = sum(target['unsent'] for target in status.values())
                completed = query(fatal_ledger, "SELECT count(DISTINCT event_id) FROM delivery_log WHERE phase='completed'")[0][0]
                assert completed + pending == 4
                assert f'{pending} actions not sent' in stderr.splitlines()[-1]
                if pending:
                    assert 'pineforge-live redeliver --ledger' in stderr.splitlines()[-1]
                    assert '--deployment' in stderr.splitlines()[-1] and '--target' in stderr.splitlines()[-1]
                assert all(row[0] <= 1 for row in query(fatal_ledger, 'SELECT attempts FROM events'))
            finally:
                if fatal.poll() is None:
                    fatal.kill(); fatal.communicate(timeout=5)
        print('PASS fatal malformed feed drains within one global timeout without retries and reports every unsent action in status', flush=True)

        entries.mode = exits.mode = 'ok'
        main_receiver.clear(); main_receiver.mode = 'hang'
        signal_ledger = root / 'signal.sqlite'
        active = subprocess.Popen([runner] + base + ['--ledger', str(signal_ledger), '--webhook-routes', str(crash_routes)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            wait_for(lambda: committed_actions(signal_ledger) and bool(main_receiver.rows))
            refusal = invoke(['redeliver', '--ledger', str(signal_ledger), '--target', 'main'], expected=1)
            assert 'the runner is running: stop it first; it resumes from its ledger' in refusal.stderr
            started = time.monotonic(); active.send_signal(signal.SIGTERM)
            active.communicate(timeout=3)
            assert active.returncode == 130 and time.monotonic() - started < 0.75
        finally:
            if active.poll() is None:
                active.kill(); active.communicate(timeout=5)
        main_receiver.clear()
        redelivery = subprocess.Popen([runner, 'redeliver', '--ledger', str(signal_ledger), '--target', 'main',
                                      '--deployment', query(signal_ledger, 'SELECT identity FROM metadata')[0][0]],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            wait_for(lambda: bool(main_receiver.rows))
            started = time.monotonic(); redelivery.send_signal(signal.SIGTERM)
            stdout, stderr = redelivery.communicate(timeout=3)
            assert redelivery.returncode == 130 and time.monotonic() - started < 0.75
            assert json.loads(stdout)['pending'] == 4
        finally:
            if redelivery.poll() is None:
                redelivery.kill(); redelivery.communicate(timeout=5)
        print('PASS SIGTERM promptly cancels EOF drain and redelivery; live redelivery is explicitly refused', flush=True)

        for receiver in receivers:
            assert not receiver.errors, receiver.errors
        print('PASS native webhook routing E2E', flush=True)
    finally:
        for receiver in receivers:
            receiver.close()
