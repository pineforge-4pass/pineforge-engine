"""Compiled execution declarations are checked before durable startup."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading

runner, old_library, *library_arguments = sys.argv[1:]
libraries = dict(argument.split('=', 1) for argument in library_arguments)
requests = []


class Receiver(BaseHTTPRequestHandler):
    def do_POST(self):
        requests.append(self.path)
        self.send_response(200)
        self.end_headers()

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='strategy-capabilities-e2e-') as directory:
        root = Path(directory)
        warmup = root / 'warmup.csv'
        warmup.write_text('timestamp,open,high,low,close,volume\n' +
                          ''.join(f'{index * 60000},100,101,99,100,5\n' for index in range(3)))
        feed = root / 'feed.jsonl'
        feed.write_text(''.join(json.dumps({'type': 'bar', 'bar': {'ts_open': index * 60000,
                                           'o': 100, 'h': 101, 'l': 99, 'c': 100, 'v': 5}}) + '\n'
                               for index in range(3, 35)))
        base = [runner, 'run', '--mode', 'bars', '--warmup', str(warmup), '--input-tf', '1',
                '--script-tf', '1', '--symbol', 'TEST:MOCK', '--feed', str(feed),
                '--allow-insecure-http', '--webhook-url', f'http://127.0.0.1:{server.server_port}/webhook']
        declarations = {'tick': 'calc_on_every_tick', 'fills': 'calc_on_order_fills',
                        'pooc': 'process_orders_on_close', 'magnifier': 'use_bar_magnifier',
                        'standard': 'fill_orders_on_standard_ohlc', 'limits': 'backtest_fill_limits_assumption',
                        'currency': 'currency', 'timeframe': 'timeframe', 'history_tick': 'calc_on_every_history_tick',
                        'unresolved': 'pyramiding', 'varip': 'intrabar_persistence', 'realtime': 'barstate.isrealtime',
                        'unresolved_tick': 'calc_on_every_tick', 'islast': 'barstate.islast',
                        'lastconfirmedhistory': 'barstate.islastconfirmedhistory',
                        'last_bar_index': 'last_bar_index', 'last_bar_time': 'last_bar_time',
                        'timenow': 'timenow', 'security': 'request.security', 'lower_tf': 'request.security_lower_tf',
                        'recorded': 'request.earnings', 'unpinned': 'request.security', 'footprint': 'request.footprint',
                        'positional': 'calc_on_every_tick', 'unknown': 'version mismatch'}
        assert set(libraries) == set(declarations) | {'close', 'clock_flags'}
        for index, (kind, declaration) in enumerate(declarations.items()):
            library = libraries[kind]
            ledger = root / f'refused-{index}.sqlite'
            controls = root / f'refused-{index}.control'
            health = root / f'refused-{index}.status.json'
            command = base + ['--strategy', library, '--ledger', str(ledger),
                              '--control-dir', str(controls), '--status-file', str(health)]
            if declaration == 'calc_on_order_fills':
                command += ['--override', 'calc_on_order_fills=false']
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            assert result.returncode == 1, result
            assert declaration in result.stderr, result.stderr
            if kind == 'unresolved_tick':
                assert 'calc_on_every_tick (unresolved at compilation)' in result.stderr, result.stderr
            if declaration.startswith('request.'):
                assert 'the native stream does not yet reproduce the batch for requested series' in result.stderr
            assert not ledger.exists() and not list(root.glob(ledger.name + '*')), result.stderr
            assert not controls.exists() and not health.exists(), result.stderr
            assert not requests
        for value in ('true', '1'):
            ledger = root / f'override-pooc-{value}.sqlite'
            result = subprocess.run(base + ['--strategy', libraries['close'], '--ledger', str(ledger),
                                           '--override', f'process_orders_on_close={value}'],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 1 and 'process_orders_on_close' in result.stderr, result.stderr
            assert not ledger.exists() and not list(root.glob(ledger.name + '*')) and not requests
        ledger = root / 'close.sqlite'
        command = base + ['--strategy', libraries['close'], '--ledger', str(ledger),
                          '--override', 'process_orders_on_close=false']
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stderr
        summary = json.loads(result.stdout)
        receipt = summary['execution_capabilities']
        assert receipt['version'] == 1 and receipt['declarations']['calc_on_every_tick'] is False
        assert not receipt['requests'] and not any(receipt['requirements'].values())
        assert summary['inputs_processed'] == 32
        assert requests
        assert 'lacks execution capabilities' not in result.stderr
        with sqlite3.connect(ledger) as database:
            assert database.execute('SELECT identity FROM metadata').fetchone()[0] == summary['deployment']
        replay = subprocess.run(command, capture_output=True, text=True, timeout=20)
        assert replay.returncode == 0, replay.stderr
        assert json.loads(replay.stdout)['execution_capabilities'] == receipt
        assert json.loads(replay.stdout)['deployment'] == summary['deployment']
        changed = subprocess.run(base + ['--strategy', libraries['clock_flags'], '--ledger', str(ledger)],
                                 capture_output=True, text=True, timeout=20)
        assert changed.returncode == 1 and 'deployment identity or schema mismatch' in changed.stderr
        admitted = subprocess.run(base + ['--strategy', libraries['clock_flags'], '--ledger', str(root / 'flags.sqlite')],
                                  capture_output=True, text=True, timeout=20)
        assert admitted.returncode == 0, admitted.stderr
        assert json.loads(admitted.stdout)['execution_capabilities'] != receipt
        feed.write_text('')
        old_ledger = root / 'old.sqlite'
        old = subprocess.run(base + ['--strategy', old_library, '--ledger', str(old_ledger)],
                             capture_output=True, text=True, timeout=20)
        assert old.returncode == 0, old.stderr
        assert 'warning: compiled strategy lacks execution capabilities' in old.stderr
        assert 'legacy behavior retained' in old.stderr
        assert old_ledger.exists() and json.loads(old.stdout)['execution_capabilities'] is None
    print('strategy capabilities E2E: 25 generated/ABI refusals and two POOC override refusals before ledger/HTTP, four bar-end builtins, unresolved boolean reason, two admitted classes, changed-receipt resume and legacy warning PASS')
finally:
    server.shutdown()
    server.server_close()
