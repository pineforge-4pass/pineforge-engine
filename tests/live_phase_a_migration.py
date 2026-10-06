"""Run with an actual phase-A executable, the new executable and the same strategy library."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading


previous, current, library = sys.argv[1:]
received = []


class Receiver(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        event = json.loads(body)
        assert self.headers['Idempotency-Key'] == self.headers['X-PineForge-Event-Id'] == event['event_id']
        received.append((event['sequence'], event['event_id'], body))
        self.send_response(204 if event['sequence'] == 1 or allow_success else 500)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *args):
        pass


allow_success = False
server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix='pineforge-phase-a-migration-') as directory:
        root = Path(directory)
        warmup = root / 'warmup.csv'
        warmup.write_text('timestamp,open,high,low,close,volume\n' +
                          ''.join(f'{index*60000},100,101,99,100,4\n' for index in range(1)))
        feed = root / 'feed.jsonl'
        feed.write_text(''.join(json.dumps({'type': 'bar', 'bar': {'ts_open': index*60000,
                        'o': 100+index, 'h': 102+index, 'l': 99+index, 'c': 101+index, 'v': 4}}) + '\n'
                        for index in range(1, 9)))
        ledger = root / 'phase-a.sqlite'
        arguments = ['run', '--strategy', library, '--warmup', str(warmup), '--script-tf', '1',
                     '--symbol', 'TEST:MOCK', '--mode', 'bars', '--feed', str(feed), '--ledger', str(ledger),
                     '--webhook-url', f'http://127.0.0.1:{server.server_port}/actions', '--allow-insecure-http',
                     '--max-attempts', '1']
        old = subprocess.run([previous] + arguments, capture_output=True, text=True, env=os.environ, timeout=25)
        assert old.returncode == 1, (old.stdout, old.stderr)
        with sqlite3.connect(ledger) as database:
            assert database.execute('SELECT schema_version FROM metadata').fetchone() == (1,)
            assert database.execute("SELECT count(*) FROM sqlite_master WHERE type='table'").fetchone() == (3,)
            immutable = database.execute('SELECT ordinal,event_id,payload,acknowledged FROM events ORDER BY ordinal').fetchall()
            assert [row[3] for row in immutable] == [1, 0]
        received.clear()
        allow_success = True
        new = subprocess.run([current] + arguments, capture_output=True, text=True, env=os.environ, timeout=25)
        assert new.returncode == 0, (new.stdout, new.stderr)
        assert sorted(row[0] for row in received) == [2, 3, 4]
        recovered = next(row for row in received if row[0] == 2)
        assert recovered[1] == immutable[1][1] and recovered[2].decode() == immutable[1][2]
        with sqlite3.connect(ledger) as database:
            migrated = database.execute('SELECT ordinal,event_id,payload FROM events ORDER BY ordinal').fetchall()
            assert migrated[:2] == [row[:3] for row in immutable]
            assert database.execute("SELECT count(*) FROM sqlite_master WHERE type='table'").fetchone() == (6,)
            assert database.execute("SELECT count(*) FROM delivery_log WHERE phase='completed'").fetchone() == (3,)
            assert database.execute('SELECT delivery_id=event_id FROM event_routes JOIN events USING(ordinal)').fetchall() == [(1,)] * 4
        resumed = subprocess.run([current] + arguments, capture_output=True, text=True, env=os.environ, timeout=25)
        assert resumed.returncode == 0 and len(received) == 3, resumed.stderr
        print('PASS actual phase-A schema-1 ledger migration: additive audit tables, unchanged payload/event IDs, acknowledged events never resent, unacknowledged events sent once')
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=2)
