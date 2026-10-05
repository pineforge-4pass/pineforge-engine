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
        self.initial_barrier = None
        self.release = threading.Event()
        self.newer_arrived = threading.Event()
        self.newer_actions_arrived = threading.Event()
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
                    self.request_count = getattr(self, 'request_count', 0) + 1
                    with owner.lock:
                        owner.rows.append({'body': body, 'action': action, 'key': self.headers['Idempotency-Key'],
                                           'connection_request': self.request_count})
                        mode = owner.mode
                        initial_barrier = owner.initial_barrier
                        release = owner.release
                        newer_arrived = owner.newer_arrived
                        newer_actions_arrived = owner.newer_actions_arrived
                        sequence_receipts = sum(row['action']['sequence'] == action['sequence']
                                                for row in owner.rows)
                        if action['sequence'] == 3:
                            newer_arrived.set()
                        if {3, 4}.issubset({row['action']['sequence'] for row in owner.rows}):
                            newer_actions_arrived.set()
                    if initial_barrier is not None:
                        try:
                            initial_barrier.wait(timeout=30)
                        except threading.BrokenBarrierError:
                            self.send_error(503, 'delivery concurrency barrier broken')
                            return
                    if mode == 'reset-first' and action['sequence'] == 1:
                        if not newer_arrived.wait(timeout=20):
                            raise AssertionError('newer action did not reach the receiver')
                    if mode == 'retry-hold' and action['sequence'] == 1 and sequence_receipts > 1:
                        if not newer_actions_arrived.wait(timeout=20):
                            raise AssertionError('newer actions were parked behind a retry')
                    if (mode == 'reset' or (mode == 'keepalive-replay' and self.request_count > 1) or
                            (mode == 'reset-first' and action['sequence'] == 1) or
                            (mode in ('retry-hold', 'retry-backoff') and action['sequence'] == 1
                             and sequence_receipts == 1)):
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_RDWR)
                        return
                    if mode == 'hang' or (mode == 'hang-first' and action['sequence'] == 1):
                        release.wait()
                    status = 500 if mode == 'fail' or (mode == 'fail-first' and action['sequence'] == 2) else 204
                    self.send_response(status)
                    self.send_header('Content-Length', '0')
                    if mode != 'keepalive-replay':
                        self.send_header('Connection', 'close')
                    self.end_headers()
                    self.close_connection = mode != 'keepalive-replay'
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
            self.release.set()
            self.newer_arrived.set()
            self.newer_actions_arrived.set()
            self.release = threading.Event()
            self.newer_arrived = threading.Event()
            self.newer_actions_arrived = threading.Event()
            self.rows.clear()
            self.errors.clear()

    def close(self):
        self.release.set()
        self.newer_arrived.set()
        self.newer_actions_arrived.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=30)
        assert not self.thread.is_alive()


def wait_for(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('timed out waiting for local test condition')


def query(ledger, sql):
    with sqlite3.connect(ledger, timeout=30) as database:
        return database.execute(sql).fetchall()


def committed_actions(ledger):
    if not ledger.exists():
        return False
    try:
        return query(ledger, 'SELECT count(*) FROM events')[0][0] == 4
    except sqlite3.OperationalError:
        return False


def delivery_attempts(ledger, sequence, outcomes, backoffs=None):
    event_id = next(event_id for event_id, payload in query(ledger, 'SELECT event_id,payload FROM events')
                    if json.loads(payload)['sequence'] == sequence)
    history = query(ledger, "SELECT log_id,attempt,phase,success,http_status,error_category,started_at,ended_at "
                    f"FROM delivery_log WHERE event_id='{event_id}' ORDER BY log_id")
    assert [(row[1], row[2]) for row in history] == [
        (attempt, phase) for attempt in range(1, len(outcomes) + 1) for phase in ('started', 'completed')], history
    completed = [row for row in history if row[2] == 'completed']
    assert [(row[3], row[4], row[5]) for row in completed] == outcomes, completed
    if backoffs is not None:
        assert len(backoffs) == len(outcomes) - 1
        for index, backoff in enumerate(backoffs):
            gap = history[2 * (index + 1)][6] - history[2 * index + 1][7]
            assert gap >= backoff, (sequence, backoff, gap, history)
    return history


def invoke(arguments, expected=0, env=None, auto_deployment=True):
    if arguments[0] == 'redeliver' and '--deployment' not in arguments and auto_deployment:
        ledger = arguments[arguments.index('--ledger') + 1]
        arguments = arguments + ['--deployment', query(ledger, 'SELECT identity FROM metadata')[0][0]]
    process = subprocess.run([runner] + arguments, capture_output=True, text=True,
                             env=environment if env is None else env, timeout=120)
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
        observed = [streamed.get(timeout=30) for _ in range(4)]
        expected_payloads = [row[0] for row in query(journal, 'SELECT payload FROM events ORDER BY ordinal')]
        assert observed == expected_payloads
        follower.send_signal(signal.SIGTERM)
        follower.wait(timeout=30)
        reader.join(timeout=30)
        assert not reader.is_alive()
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

        concurrent_document = copy.deepcopy(document)
        concurrent_document['delivery'] = {'max_in_flight': 8, 'connect_timeout_ms': 10000,
                                           'total_timeout_ms': 60000, 'transport_retries': 0,
                                           'retry_backoff_ms': []}
        concurrent_routes = save_routes('concurrent', concurrent_document)
        entries.clear(); exits.clear()
        entries.mode = exits.mode = 'ok'
        initial_barrier = threading.Barrier(4)
        entries.initial_barrier = exits.initial_barrier = initial_barrier
        concurrent_ledger = root / 'concurrent.sqlite'
        try:
            result = json.loads(run(concurrent_ledger, concurrent_routes).stdout)
            assert not initial_barrier.broken
            assert result['webhooks_delivered'] == 4
            assert not entries.errors and not exits.errors
            assert len(entries.rows) == len(exits.rows) == 2
            assert query(concurrent_ledger, 'SELECT count(*) FROM inputs')[0][0] == len(events)
        finally:
            entries.initial_barrier = exits.initial_barrier = None
        print('PASS all initial deliveries in flight while computation produces later actions', flush=True)

        isolation_document = copy.deepcopy(document)
        isolation_document['delivery'] = {'max_in_flight': 8, 'connect_timeout_ms': 10000,
                                          'total_timeout_ms': 30000, 'transport_retries': 2,
                                          'retry_backoff_ms': [100, 200]}
        isolation = save_routes('isolation', isolation_document)
        entries.clear(); exits.clear()
        entries.mode = 'reset-first'; exits.mode = 'fail-first'
        isolation_ledger = root / 'isolation.sqlite'
        process = run(isolation_ledger, isolation)
        assert not entries.errors and not exits.errors, (entries.errors, exits.errors)
        newer = [row for row in entries.rows if row['action']['sequence'] == 3]
        assert len(newer) == 1
        initial_starts = query(isolation_ledger, "SELECT log_id FROM delivery_log WHERE phase='started' AND attempt=1")
        assert len(initial_starts) == 4
        assert len([row for row in exits.rows if row['action']['sequence'] == 2]) == 1
        assert len([row for row in entries.rows if row['action']['sequence'] == 1]) == 3
        assert query(isolation_ledger, 'SELECT count(*) FROM inputs')[0][0] == len(events)
        warning = ('pineforge-live: warning: compiled strategy lacks checked settings; '
                   'legacy settings may be ignored or defaulted')
        capability_warning = ('pineforge-live: warning: compiled strategy lacks execution capabilities; '
                              'close-only eligibility cannot be proved (legacy behavior retained)')
        log_lines = process.stderr.splitlines()
        assert log_lines.count(warning) == 1, process.stderr
        assert log_lines.count(capability_warning) == 1, process.stderr
        errors = [json.loads(line) for line in log_lines if line not in (warning, capability_warning)]
        assert len(errors) == 4 and all(row['event'] == 'webhook_delivery_error' for row in errors), process.stderr
        status = json.loads(invoke(['status', '--ledger', str(isolation_ledger)]).stdout)['targets']
        assert status['entries']['failed'] == 3 and status['exits']['failed'] == 1
        assert status['entries']['last_error']['category'] == 'network_error'
        assert status['exits']['last_error']['http_status'] == 500
        assert status['entries']['last_success'] is not None and status['main']['sent'] == 0
        older = delivery_attempts(isolation_ledger, 1, [(0, 0, 'network_error')] * 3, [100, 200])
        delivery_attempts(isolation_ledger, 2, [(0, 500, 'http_status')])
        newer = delivery_attempts(isolation_ledger, 3, [(1, 204, '')])
        delivery_attempts(isolation_ledger, 4, [(1, 204, '')])
        assert newer[0][0] < older[1][0]
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
        print('PASS newer action starts before held transport failure; observed retry backoff, final HTTP 500, append-only rows, status and stable redelivery', flush=True)

        stdin_base = list(base)
        stdin_base[stdin_base.index('--feed') + 1] = '-'
        keepalive_document = copy.deepcopy(isolation_document)
        keepalive_document['targets'] = {'main': document['targets']['main']}
        keepalive_document['rules'] = []
        keepalive_document['delivery'].update(max_in_flight=1, transport_retries=0)
        keepalive_routes = save_routes('keepalive-replay', keepalive_document)
        main_receiver.clear(); main_receiver.mode = 'keepalive-replay'
        keepalive_ledger = root / 'keepalive-replay.sqlite'
        result = json.loads(run(keepalive_ledger, keepalive_routes).stdout)
        assert not main_receiver.errors, main_receiver.errors
        assert result['webhooks_delivered'] == 4
        assert any(row['connection_request'] > 1 for row in main_receiver.rows), main_receiver.rows
        assert len(main_receiver.rows) > 4, main_receiver.rows
        unique_receipts = {}
        for row in main_receiver.rows:
            previous = unique_receipts.setdefault(row['key'], row['body'])
            assert row['body'] == previous, row
        assert len(unique_receipts) == 4
        for sequence in range(1, 5):
            delivery_attempts(keepalive_ledger, sequence, [(1, 204, '')])
        assert query(keepalive_ledger, "SELECT count(*) FROM delivery_log WHERE phase='completed'") == [(4,)]
        print('PASS reused keep-alive POST replay has stable deduplication keys and one completion per ledger attempt', flush=True)

        retry_document = copy.deepcopy(isolation_document)
        retry_document['targets'] = {'main': document['targets']['main']}
        retry_document['rules'] = []
        retry_routes = save_routes('retry-isolation', retry_document)
        main_receiver.clear(); main_receiver.mode = 'retry-hold'
        retry_ledger = root / 'retry-isolation.sqlite'
        retry_process = subprocess.Popen([runner] + stdin_base + ['--ledger', str(retry_ledger),
                                         '--webhook-routes', str(retry_routes)], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            prefix_count = query(journal, 'SELECT input_index FROM events WHERE ordinal=1')[0][0] + 1
            feed_lines = feed.read_text().splitlines(keepends=True)
            retry_process.stdin.write(''.join(feed_lines[:prefix_count])); retry_process.stdin.flush()

            def first_attempt_failed():
                if not retry_ledger.exists():
                    return False
                try:
                    return bool(query(retry_ledger, "SELECT log_id FROM delivery_log WHERE phase='completed' "
                                      "AND attempt=1 AND success=0 AND error_category='network_error'"))
                except sqlite3.OperationalError:
                    return False

            wait_for(first_attempt_failed)
            wait_for(lambda: sum(row['action']['sequence'] == 1 for row in main_receiver.rows) == 2)
            retry_process.stdin.write(''.join(feed_lines[prefix_count:])); retry_process.stdin.flush()
            stdout, stderr = retry_process.communicate(timeout=90)
            assert retry_process.returncode == 0, stderr
            assert not main_receiver.errors, main_receiver.errors
            assert json.loads(stdout)['webhooks_delivered'] == 4, stdout
            older = delivery_attempts(retry_ledger, 1, [(0, 0, 'network_error'), (1, 204, '')], [100])
            for sequence in (2, 3, 4):
                newer = delivery_attempts(retry_ledger, sequence, [(1, 204, '')])
                assert older[2][0] < newer[0][0] < older[3][0], (older, newer)
            assert query(retry_ledger, 'SELECT count(*) FROM inputs')[0][0] == len(events)
        finally:
            if retry_process.poll() is None:
                retry_process.kill(); retry_process.communicate(timeout=30)
        print('PASS newer actions start after a transport failure and after its retry starts, before that held retry completes', flush=True)

        backoff_document = copy.deepcopy(retry_document)
        backoff_document['delivery'].update(transport_retries=1, retry_backoff_ms=[10000])
        backoff_routes = save_routes('sleeping-backoff', backoff_document)
        main_receiver.clear(); main_receiver.mode = 'retry-backoff'
        backoff_ledger = root / 'sleeping-backoff.sqlite'
        backoff_process = subprocess.Popen([runner] + stdin_base + ['--ledger', str(backoff_ledger),
                                           '--webhook-routes', str(backoff_routes)], stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            backoff_process.stdin.write(''.join(feed_lines[:prefix_count])); backoff_process.stdin.flush()
            wait_for(lambda: bool(main_receiver.rows))
            wait_for(lambda: bool(query(backoff_ledger, "SELECT log_id FROM delivery_log WHERE phase='completed' "
                                        "AND attempt=1 AND success=0")))
            backoff_process.stdin.write(''.join(feed_lines[prefix_count:])); backoff_process.stdin.flush()
            stdout, stderr = backoff_process.communicate(timeout=90)
            assert backoff_process.returncode == 0, stderr
            assert not main_receiver.errors, main_receiver.errors
            assert json.loads(stdout)['webhooks_delivered'] == 4, stdout
            older = delivery_attempts(backoff_ledger, 1, [(0, 0, 'network_error'), (1, 204, '')], [10000])
            for sequence in (2, 3, 4):
                newer = delivery_attempts(backoff_ledger, sequence, [(1, 204, '')])
                assert older[1][0] < newer[0][0] < older[2][0], (older, newer)
        finally:
            if backoff_process.poll() is None:
                backoff_process.kill(); backoff_process.communicate(timeout=30)
        print('PASS newer actions start during a configured 10 s retry backoff, before its retry starts', flush=True)

        timeout_document = copy.deepcopy(isolation_document)
        timeout_document['delivery'].update(connect_timeout_ms=5000, total_timeout_ms=5000)
        timeout_routes = save_routes('timeouts', timeout_document)
        entries.clear(); exits.clear(); entries.mode = exits.mode = 'hang'
        timeout_ledger = root / 'timeouts.sqlite'
        result = json.loads(run(timeout_ledger, timeout_routes).stdout)
        assert not entries.errors and not exits.errors, (entries.errors, exits.errors)
        assert result['webhooks_delivered'] == 0
        assert len(entries.rows) == len(exits.rows) == 6
        for sequence in range(1, 5):
            delivery_attempts(timeout_ledger, sequence, [(0, 0, 'timeout')] * 3, [100, 200])
        status = json.loads(invoke(['status', '--ledger', str(timeout_ledger)]).stdout)['targets']
        assert status['entries']['failed'] == status['exits']['failed'] == 6
        assert status['entries']['last_error']['category'] == status['exits']['last_error']['category'] == 'timeout'
        assert query(timeout_ledger, 'SELECT count(*) FROM inputs')[0][0] == len(events)
        print('PASS event-held requests end as timeout errors and retry only after each recorded failure', flush=True)

        disconnected = copy.deepcopy(isolation_document)
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            refused_port = reserved.getsockname()[1]
            disconnected['targets']['entries']['url'] = f'http://127.0.0.1:{refused_port}/actions'
            disconnected['delivery'].pop('retry_backoff_ms')
            disconnected_routes = save_routes('disconnected', disconnected)
            exits.clear(); exits.mode = 'ok'
            disconnected_ledger = root / 'disconnected.sqlite'
            run(disconnected_ledger, disconnected_routes)
        initial_starts = query(disconnected_ledger, "SELECT log_id FROM delivery_log WHERE phase='started' AND attempt=1")
        assert len(initial_starts) == 4
        for sequence in (1, 3):
            delivery_attempts(disconnected_ledger, sequence, [(0, 0, 'network_error')] * 3, [1000, 2000])
        for sequence in (2, 4):
            delivery_attempts(disconnected_ledger, sequence, [(1, 204, '')])
        assert len(exits.rows) == 2
        stored_routes = json.loads(query(disconnected_ledger, 'SELECT document FROM routing_configuration')[0][0])
        assert 'retry_backoff_ms' not in stored_routes['configuration']['delivery']
        print('PASS refused connections observe default 1 s/2 s backoff, cap retries at two and preserve healthy deliveries', flush=True)

        deterministic = copy.deepcopy(isolation_document)
        deterministic['default_target'] = 'main'
        deterministic['rules'] = []
        deterministic['delivery'].update(connect_timeout_ms=2000, total_timeout_ms=2000)
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
        crash_document['delivery']['total_timeout_ms'] = 300000
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
            process.communicate(timeout=30)
            assert process.returncode == -signal.SIGKILL
        finally:
            if process.poll() is None:
                process.kill(); process.wait(timeout=30)
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

        fatal_document = copy.deepcopy(isolation_document)
        fatal_document['targets'] = {'main': document['targets']['main']}
        fatal_document['rules'] = []
        fatal_document['delivery'].update(max_in_flight=1, connect_timeout_ms=5000, total_timeout_ms=5000)
        fatal_routes = save_routes('fatal', fatal_document)
        for mode in ('ok', 'hang'):
            main_receiver.clear(); main_receiver.mode = mode
            fatal_ledger = root / f'fatal-{mode}.sqlite'
            fatal = subprocess.Popen([runner] + stdin_base + ['--ledger', str(fatal_ledger), '--webhook-routes', str(fatal_routes)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, env=environment)
            try:
                fatal.stdin.write(feed.read_text()); fatal.stdin.flush()
                wait_for(lambda: committed_actions(fatal_ledger))
                started = time.monotonic()
                stdout, stderr = fatal.communicate(input='malformed\n', timeout=60)
                drain_bound = 3 * fatal_document['delivery']['total_timeout_ms'] / 1000
                assert fatal.returncode == 1 and time.monotonic() - started < drain_bound, stderr
                status = json.loads(invoke(['status', '--ledger', str(fatal_ledger)]).stdout)['targets']
                pending = sum(target['unsent'] for target in status.values())
                completed = query(fatal_ledger, "SELECT count(DISTINCT event_id) FROM delivery_log WHERE phase='completed'")[0][0]
                if mode == 'hang':
                    assert completed <= 1, (completed, stderr)
                assert completed + pending == 4
                assert f'{pending} actions not sent' in stderr.splitlines()[-1]
                if pending:
                    assert 'pineforge-live redeliver --ledger' in stderr.splitlines()[-1]
                    assert '--deployment' in stderr.splitlines()[-1] and '--target' in stderr.splitlines()[-1]
                assert all(row[0] <= 1 for row in query(fatal_ledger, 'SELECT attempts FROM events'))
                assert not main_receiver.errors, main_receiver.errors
            finally:
                if fatal.poll() is None:
                    fatal.kill(); fatal.communicate(timeout=30)
        print('PASS fatal feed has at most one hung completion within a 3x global-timeout guard, without retries, and reports every unsent action', flush=True)

        entries.mode = exits.mode = 'ok'
        main_receiver.clear(); main_receiver.mode = 'hang'
        signal_ledger = root / 'signal.sqlite'
        active = subprocess.Popen([runner] + base + ['--ledger', str(signal_ledger), '--webhook-routes', str(crash_routes)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            wait_for(lambda: committed_actions(signal_ledger) and bool(main_receiver.rows))
            refusal = invoke(['redeliver', '--ledger', str(signal_ledger), '--target', 'main'], expected=1)
            assert 'the runner is running: stop it first; it resumes from its ledger' in refusal.stderr
            started = time.monotonic()
            active.send_signal(signal.SIGTERM)
            active.communicate(timeout=30)
            assert active.returncode == 0 and time.monotonic() - started < 10
            assert query(signal_ledger, "SELECT count(*) FROM delivery_log WHERE phase='completed'")[0][0] == 0
            status = json.loads(invoke(['status', '--ledger', str(signal_ledger)]).stdout)['targets']
            assert status['main']['unsent'] == 4
        finally:
            if active.poll() is None:
                active.kill(); active.communicate(timeout=30)
        main_receiver.clear()
        redelivery = subprocess.Popen([runner, 'redeliver', '--ledger', str(signal_ledger), '--target', 'main',
                                      '--deployment', query(signal_ledger, 'SELECT identity FROM metadata')[0][0]],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
        try:
            wait_for(lambda: bool(main_receiver.rows))
            started = time.monotonic()
            redelivery.send_signal(signal.SIGTERM)
            stdout, stderr = redelivery.communicate(timeout=30)
            assert redelivery.returncode == 0 and time.monotonic() - started < 10
            assert json.loads(stdout)['pending'] == 4
        finally:
            if redelivery.poll() is None:
                redelivery.kill(); redelivery.communicate(timeout=30)
        print('PASS SIGTERM cancels EOF drain and redelivery within 10 s; live redelivery is explicitly refused', flush=True)

        for receiver in receivers:
            assert not receiver.errors, receiver.errors
        print('PASS native webhook routing E2E', flush=True)
    finally:
        for receiver in receivers:
            receiver.close()
