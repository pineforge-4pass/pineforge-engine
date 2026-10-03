"""Checked generated settings are refused before ledger or HTTP side effects."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading

runner, checked_library, legacy_library = sys.argv[1:]
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
    with tempfile.TemporaryDirectory(prefix='checked-settings-e2e-') as directory:
        root = Path(directory)
        warmup = root / 'warmup.csv'
        warmup.write_text('timestamp,open,high,low,close,volume\n' +
                          ''.join(f'{index * 60000},100,101,99,100,5\n' for index in range(3)))
        feed = root / 'feed.jsonl'
        feed.write_text('')
        base = [runner, 'run', '--mode', 'bars', '--warmup', str(warmup), '--input-tf', '1',
                '--script-tf', '1', '--symbol', 'TEST:MOCK', '--feed', str(feed),
                '--allow-insecure-http', '--webhook-url', f'http://127.0.0.1:{server.server_port}/webhook']
        for index, setting in enumerate([
                ['--input', 'Lenght=7'], ['--input', 'Enabled=yes'],
                ['--input', 'Length=3suffix'], ['--input', 'Threshold=NaN'],
                ['--override', 'default_qty_type=typo'], ['--override', 'slippage=Inf']]):
            ledger = root / f'bad-{index}.sqlite'
            result = subprocess.run(base + ['--strategy', checked_library, '--ledger', str(ledger)] + setting,
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 1, result
            assert 'refused' in result.stderr, result.stderr
            assert not ledger.exists(), result.stderr
            assert not requests
        ledger = root / 'good.sqlite'
        settings = ['--input', 'Length=4', '--input', 'Enabled=false',
                    '--override', 'default_qty_value=200', '--override', 'default_qty_type=cash']
        command = base + ['--strategy', checked_library, '--ledger', str(ledger)] + settings
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stderr
        summary = json.loads(result.stdout)
        receipt = summary['effective_settings']
        assert receipt['version'] == 1
        inputs = {entry['name']: entry for entry in receipt['inputs']}
        overrides = {entry['name']: entry for entry in receipt['overrides']}
        assert inputs['Length']['effective_value'] == '4'
        assert inputs['Length']['min'] == 1 and inputs['Length']['step'] == 2
        assert inputs['Enabled']['effective_value'] == 'false'
        assert overrides['default_qty_value']['effective_value'] == '200'
        assert overrides['default_qty_value']['default'] == '1'
        assert overrides['default_qty_type']['effective_value'] == 'cash'
        with sqlite3.connect(ledger) as database:
            assert database.execute('SELECT identity FROM metadata').fetchone()[0] == summary['deployment']
        replay = subprocess.run(command, capture_output=True, text=True, timeout=20)
        assert replay.returncode == 0 and json.loads(replay.stdout)['deployment'] == summary['deployment'], replay.stderr
        changed_ledger = root / 'changed.sqlite'
        changed = subprocess.run(base + ['--strategy', checked_library, '--ledger', str(changed_ledger)]
                                 + ['Length=5.0' if argument == 'Length=4' else argument for argument in settings],
                                 capture_output=True, text=True, timeout=20)
        assert changed.returncode == 0, changed.stderr
        changed_summary = json.loads(changed.stdout)
        assert changed_summary['deployment'] != summary['deployment']
        assert {entry['name']: entry for entry in changed_summary['effective_settings']['inputs']}['Length']['effective_value'] == '5'
        incompatible = subprocess.run(base + ['--strategy', checked_library, '--ledger', str(ledger)]
                                      + ['Length=5.0' if argument == 'Length=4' else argument for argument in settings],
                                      capture_output=True, text=True, timeout=20)
        assert incompatible.returncode == 1, incompatible.stderr
        old_ledger = root / 'legacy.sqlite'
        old = subprocess.run(base + ['--strategy', legacy_library, '--ledger', str(old_ledger),
                                    '--input', 'unknown=7'], capture_output=True, text=True, timeout=20)
        assert old.returncode == 0, old.stderr
        assert 'warning' in old.stderr and 'legacy settings' in old.stderr
        assert json.loads(old.stdout)['effective_settings'] is None
        assert not requests
    print('checked settings E2E: refusal before ledger/HTTP, receipt identity, legacy warning PASS')
finally:
    server.shutdown()
    server.server_close()
