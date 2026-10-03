"""WebSocket finality through the real CLI, SQLite ledger and loopback webhook."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time


runner, library = sys.argv[1:3]
expect_refusal = sys.argv[3:] == ['--expect-refusal']
assert not sys.argv[3:] or expect_refusal
received = []
stream_frames = []
stream_idle = False
final_frame_sent = threading.Event()


def frame(opcode, payload, final=True):
    header = bytes([opcode | (0x80 if final else 0)])
    if len(payload) < 126:
        return header + bytes([len(payload)]) + payload
    return header + bytes([126]) + struct.pack('!H', len(payload)) + payload


class Receiver(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        frames = list(stream_frames)
        idle = stream_idle
        accept = base64.b64encode(hashlib.sha1(
            (self.headers['Sec-WebSocket-Key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()
        ).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        self.close_connection = True
        try:
            for data in frames:
                if data[0] & 0x80 and data[0] & 0x0f in (0, 1):
                    final_frame_sent.set()
                self.wfile.write(data)
                self.wfile.flush()
                time.sleep(0.02)
            if idle:
                self.connection.settimeout(20)
                while self.connection.recv(4096):
                    pass
        except OSError:
            pass

    def do_POST(self):
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        received.append({'payload': payload, 'before_final': not final_frame_sent.is_set()})
        self.send_response(200)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *args):
        pass


def ledger_counts(path):
    if not path.exists():
        return 0, 0
    with sqlite3.connect(path) as database:
        tables = {row[0] for row in database.execute('SELECT name FROM sqlite_master WHERE type="table"')}
        inputs = database.execute('SELECT count(*) FROM inputs').fetchone()[0] if 'inputs' in tables else 0
        events = database.execute('SELECT count(*) FROM events').fetchone()[0] if 'events' in tables else 0
    return inputs, events


server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
payload = json.dumps({'type': 'bar', 'bar': {
    'ts_open': 120000, 'o': 100, 'h': 102, 'l': 99, 'c': 101, 'v': 4
}}).encode()
cases = [
    ('fin0-eof', [frame(1, payload, False)], False, False, 'truncated text message'),
    ('fin0-close', [frame(1, payload, False), frame(8, b'')], False, False, 'closed during text message'),
    ('fin0-idle', [frame(1, payload, False)], True, False, 'idle or message timeout'),
    ('unfinished-fragments-ping', [frame(1, payload, False), frame(9, b'alive'),
                                   frame(0, b' ', False), frame(9, b'still alive')],
     False, False, 'truncated text message'),
    ('binary', [frame(2, payload)], False, False, 'ordered text frames'),
    ('new-text-before-final', [frame(1, payload, False), frame(1, b' ')],
     False, False, 'truncated text message'),
    ('partial-final-frame', [bytes([0x81, 126]) + struct.pack('!H', len(payload) + 10) + payload],
     False, False, 'truncated text message'),
    ('valid-fragments-ping', [frame(1, payload[:40], False), frame(9, b'alive'),
                              frame(0, payload[40:80], False), frame(10, b'pong'),
                              frame(0, payload[80:])], False, True, ''),
    ('valid-empty-final', [frame(1, payload, False), frame(9, b'alive'), frame(0, b'')],
     False, True, ''),
]
failures = []
unsupported = False
try:
    with tempfile.TemporaryDirectory(prefix='native-ws-finality-') as directory:
        root = Path(directory)
        warmup = root/'warmup.csv'
        warmup.write_text('timestamp,open,high,low,close,volume\n'
                          '0,100,102,99,101,4\n60000,100,102,99,101,4\n')
        base = [runner, 'run', '--strategy', library, '--warmup', str(warmup),
                '--script-tf', '1', '--mode', 'bars', '--symbol', 'TEST:ETH',
                '--webhook-url', f'http://127.0.0.1:{server.server_port}/actions',
                '--allow-insecure-http', '--max-events', '1']
        for name, stream_frames, stream_idle, success, diagnostic in cases:
            received.clear()
            final_frame_sent.clear()
            ledger = root/(name + '.sqlite3')
            command = base + ['--ledger', str(ledger), '--feed-url',
                              f'ws://127.0.0.1:{server.server_port}/stream']
            started = time.monotonic()
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            elapsed = time.monotonic() - started
            inputs, events = ledger_counts(ledger)
            no_support = 'libcurl build with WS/WSS support enabled' in result.stderr
            unsupported = unsupported or no_support
            refused = expect_refusal or no_support
            print(json.dumps({'case': name, 'returncode': result.returncode, 'inputs': inputs,
                              'events': events, 'webhooks': len(received), 'ledger_exists': ledger.exists(),
                              'seconds': round(elapsed, 2), 'stderr': result.stderr.strip()}), flush=True)
            try:
                if refused:
                    assert result.returncode != 0
                    assert ('libcurl 8.14.1 or newer' in result.stderr or no_support)
                    assert not ledger.exists()
                    assert inputs == events == len(received) == 0
                elif success:
                    assert result.returncode == 0, (result.stdout, result.stderr)
                    assert inputs == events == len(received) == 1
                    assert json.loads(result.stdout)['inputs_committed'] == 1
                    assert received[0]['payload']['order']['action'] == 'buy'
                    assert not received[0]['before_final']
                else:
                    assert result.returncode != 0
                    assert inputs == events == len(received) == 0
                    assert diagnostic in result.stderr
                    if stream_idle:
                        assert elapsed >= 14
            except AssertionError as error:
                failures.append(name)
                print(f'FAIL {name}: {error}', flush=True)

        if expect_refusal or unsupported:
            sentinel = root/'existing.sqlite3'
            sentinel.write_bytes(b'ledger must stay byte-identical')
            before = sentinel.read_bytes()
            for scheme in ('ws', 'wss'):
                result = subprocess.run(base + ['--ledger', str(sentinel), '--feed-url',
                                        f'{scheme}://127.0.0.1:{server.server_port}/stream'],
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode != 0
                assert 'libcurl' in result.stderr
                assert sentinel.read_bytes() == before
                assert not Path(str(sentinel) + '-wal').exists()
                assert not Path(str(sentinel) + '-shm').exists()
                assert not received
            print('WebSocket startup refuses ws/wss before touching an existing ledger', flush=True)
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)

if failures:
    raise SystemExit('WebSocket finality failures: ' + ', '.join(failures))
print('native WebSocket CLI finality: all negative inputs/outbox/webhooks empty; '
      + ('startup refusal verified' if expect_refusal or unsupported else 'both fragmented positives committed once'))
raise SystemExit(77 if unsupported and not expect_refusal else 0)
