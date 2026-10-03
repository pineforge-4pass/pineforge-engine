"""Subprocess/HTTP integration of the C++ CLI on synthetic, hand-written strategy inputs.

Python is a test receiver/orchestrator only, never part of the production path.
No campaign data, compiler, backtest measurements or broker accounts are used.
"""
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import base64
import struct

runner, library, parser, batch_parser = sys.argv[1:]
received = []
responses = []
secret = "synthetic-loopback-test-key"
snapshot = b''
stream_messages = []


class Receiver(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def do_GET(self):
        if self.path == '/stream':
            accept = base64.b64encode(hashlib.sha1(
                (self.headers['Sec-WebSocket-Key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            self.send_response(101)
            self.send_header('Upgrade', 'websocket')
            self.send_header('Connection', 'Upgrade')
            self.send_header('Sec-WebSocket-Accept', accept)
            self.end_headers()
            for message in stream_messages:
                data = message.encode()
                header = bytes([0x81, len(data)]) if len(data) < 126 else bytes([0x81, 126]) + struct.pack('!H', len(data))
                self.wfile.write(header + data)
            self.wfile.flush()
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(snapshot)))
        self.end_headers()
        self.wfile.write(snapshot)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers['Content-Length']))
        event = json.loads(raw)
        assert self.headers['Idempotency-Key'] == event['event_id']
        expected = 'sha256=' + hmac.new(secret.encode(), raw, hashlib.sha256).hexdigest()
        assert self.headers['X-PineForge-Signature'] == expected
        received.append(event)
        self.send_response(responses.pop(0) if responses else 200)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()

try:
    with tempfile.TemporaryDirectory(prefix='pineforge-native-e2e-') as raw_root:
        root = Path(raw_root)
        warmup = root/'warmup.csv'
        warmup.write_text('timestamp,open,high,low,close,volume\n' +
                          ''.join(f'{i*60000},100,101,99,100,4\n' for i in range(3)))
        base = [runner, 'run', '--strategy', library, '--warmup', str(warmup),
                '--script-tf', '3', '--symbol', 'TEST:MOCK', '--allow-insecure-http',
                '--webhook-url', f'http://127.0.0.1:{server.server_port}/webhook',
                '--webhook-secret-env', 'PINEFORGE_TEST_HMAC']

        def invoke(extra, success=True):
            import os
            env = dict(os.environ, PINEFORGE_TEST_HMAC=secret)
            p = subprocess.run(base + extra, capture_output=True, text=True, env=env, timeout=25)
            assert (p.returncode == 0) is success, (p.returncode, p.stdout, p.stderr)
            assert secret not in p.stdout + p.stderr
            return json.loads(p.stdout) if success else p

        bar_events = []
        tick_events = []
        seq = 1
        for i in range(3, 27):
            bar_events.append({'type': 'bar', 'bar': {'ts_open': i*60000, 'o': 100+i,
                              'h': 102+i, 'l': 99+i, 'c': 101+i, 'v': 4}})
            for offset, price in zip((0, 15000, 30000, 45000), (100+i, 102+i, 99+i, 101+i)):
                tick_events.append({'type': 'tick', 'ts': i*60000+offset, 'seq': seq,
                                    'price': price, 'qty': 1})
                seq += 1
            tick_events.append({'type': 'time', 'ts': (i+1)*60000})

        for mode, events in [('bars', bar_events), ('ticks', tick_events)]:
            received.clear()
            feed = root/(mode+'.jsonl')
            feed.write_text(''.join(json.dumps(e)+'\n' for e in events))
            ledger = root/(mode+'.sqlite3')
            options = ['--mode', mode, '--feed', str(feed), '--ledger', str(ledger)]
            partial = invoke(options+['--max-events', '2'])
            assert partial['inputs_processed'] == 2
            result = invoke(options)
            assert result['inputs_committed'] == len(events)
            assert result['webhooks_pending'] == 0
            assert len(received) == 4, received
            assert [e['order']['leg'] for e in received] == ['entry', 'exit', 'entry', 'exit']
            assert [e['sequence'] for e in received] == [1, 2, 3, 4]
            assert len({e['event_id'] for e in received}) == 4
            prior = list(received)
            final = invoke(options)
            assert final['inputs_processed'] == final['webhooks_delivered'] == 0
            assert received == prior
            assert final['prefix_skipped'] == len(events)
            same_timezone = invoke(options+['--chart-timezone', 'UTC'])
            assert same_timezone['webhooks_delivered'] == 0 and received == prior
            with sqlite3.connect(ledger) as db:
                assert db.execute('SELECT count(*) FROM inputs').fetchone()[0] == len(events)
            # Changing a declared strategy input changes deployment identity.
            invoke(options+['--input', 'changed=1'], success=False)
            assert received == prior
            # A changed historical frame must refuse without another webhook.
            bad = root/(mode+'-changed.jsonl')
            changed = [dict(e) for e in events]
            if mode == 'ticks':
                changed[0]['price'] += 1
            else:
                changed[0]['bar'] = dict(changed[0]['bar'], v=5)
            bad.write_text(''.join(json.dumps(e)+'\n' for e in changed))
            invoke(['--mode', mode, '--feed', str(bad), '--ledger', str(ledger)], success=False)
            assert received == prior

        # Failed HTTP response leaves the same immutable event queued. Restart
        # replays input before retrying that event, with its identical id/body.
        received.clear(); responses[:] = [503]
        feed = root/'retry.jsonl'; feed.write_text(''.join(json.dumps(e)+'\n' for e in bar_events))
        ledger = root/'retry.sqlite3'
        options = ['--mode', 'bars', '--feed', str(feed), '--ledger', str(ledger)]
        invoke(options+['--max-attempts', '1'], success=False)
        assert len(received) == 1
        first = received[0]
        invoke(options)
        assert received[1] == first
        assert len({e['event_id'] for e in received}) == 4

        # Redirect refusal is permanent, with one durable attempt per invocation.
        received.clear(); responses[:] = [302]
        redirect_ledger = root/'redirect.sqlite3'
        invoke(['--mode','bars','--feed',str(feed),'--ledger',str(redirect_ledger)],success=False)
        assert len(received) == 1
        with sqlite3.connect(redirect_ledger) as db:
            assert db.execute('SELECT attempts FROM events WHERE acknowledged=0 ORDER BY ordinal LIMIT 1').fetchone()[0] == 1

        # A complete provider batch is one durable input, even when delivery
        # fails or max-events requests a stop. No remaining event is lost.
        received.clear(); responses[:] = [503]
        feed = root/'batch.jsonl'
        feed.write_text(json.dumps({'type':'batch','events':bar_events})+'\n')
        ledger = root/'batch.sqlite3'
        options = ['--mode','bars','--feed',str(feed),'--ledger',str(ledger),'--max-events','1']
        invoke(options+['--max-attempts','1'],success=False)
        with sqlite3.connect(ledger) as db:
            assert db.execute('SELECT COUNT(*) FROM inputs').fetchone()[0] == 1
            assert db.execute('SELECT COUNT(*) FROM events').fetchone()[0] == 4
        first = received[0]
        invoke(options)
        assert received[1] == first and len({e['event_id'] for e in received}) == 4

        # A later invalid event in a batch must not commit any of the message.
        received.clear()
        invalid = root/'batch-invalid.jsonl'
        invalid.write_text(json.dumps({'type':'batch','events':[tick_events[0],dict(tick_events[1],seq=99)]})+'\n')
        invalid_ledger = root/'batch-invalid.sqlite3'
        invoke(['--mode','ticks','--feed',str(invalid),'--ledger',str(invalid_ledger)],success=False)
        with sqlite3.connect(invalid_ledger) as db:
            assert db.execute('SELECT COUNT(*) FROM inputs').fetchone()[0] == 0
            assert db.execute('SELECT COUNT(*) FROM events').fetchone()[0] == 0
        assert not received

        # A plugin may emit multiple trades; its message is still one input.
        feed = root/'parser-batch.txt'; feed.write_text('two-trades\n')
        ledger = root/'parser-batch.sqlite3'
        options=['--mode','ticks','--feed',str(feed),'--ledger',str(ledger),'--parser',batch_parser,'--max-events','1']
        result=invoke(options)
        assert result['inputs_committed']==1 and result['last_tick_sequence']==2
        assert invoke(options)['inputs_processed']==0

        # A custom C++ parser accepts provider messages without a Python adapter.
        # The next-minute tick finalizes the previous minute in the native engine.
        received.clear()
        feed = root/'provider.txt'
        messages = ['heartbeat'] + [f"trade,{e['seq']},{e['ts']},{e['price']},{e['qty']}"
                                    for e in tick_events if e['type'] == 'tick']
        messages.append(f'trade,{seq},1620000,127,1')
        feed.write_text('\n'.join(messages)+'\n')
        options = ['--mode', 'ticks', '--feed', str(feed), '--ledger', str(root/'parser.sqlite3'),
                   '--parser', parser]
        invoke(options)
        assert len(received) == 4
        prior = list(received)
        invoke(options)
        assert received == prior
        # Polling snapshots are consumed by the native libcurl transport.
        received.clear()
        snapshot = ''.join(json.dumps(e)+'\n' for e in bar_events).encode()
        options = ['--mode', 'bars', '--feed-url', f'http://127.0.0.1:{server.server_port}/snapshot',
                   '--check', '--ledger', str(root/'http.sqlite3')]
        invoke(options)
        assert len(received) == 4
        prior = list(received)
        invoke(options)
        assert received == prior

        # WebSocket wire intake, custom native parser, engine and HTTP outbox
        # execute in the same C++ process. A system curl lacking WebSockets
        # refuses explicitly; separate native transport tests report a skip.
        received.clear()
        stream_messages = messages
        options = ['--mode', 'ticks', '--feed-url', f'ws://127.0.0.1:{server.server_port}/stream',
                   '--max-events', str(len(messages)-1), '--parser', parser,
                   '--ledger', str(root/'websocket.sqlite3')]
        import os
        result = subprocess.run(base+options, capture_output=True, text=True,
                                env=dict(os.environ, PINEFORGE_TEST_HMAC=secret), timeout=25)
        if result.returncode:
            assert ('libcurl build with WS/WSS support enabled' in result.stderr or
                    'libcurl 8.14.1 or newer' in result.stderr), (result.stdout,result.stderr)
            assert not received
            assert not (root/'websocket.sqlite3').exists()
            print('WebSocket CLI integration unavailable in this libcurl build')
        else:
            assert len(received) == 4
            assert json.loads(result.stdout)['inputs_committed'] == len(messages)-1
            received.clear()
            stream_messages = ['two-trades']
            result = invoke(['--mode','ticks','--feed-url',f'ws://127.0.0.1:{server.server_port}/stream',
                             '--max-events','1','--parser',batch_parser,
                             '--ledger',str(root/'websocket-batch.sqlite3')])
            assert result['inputs_committed']==1 and result['last_tick_sequence']==2
        print('native C++ runner: bars/ticks/parser, HMAC HTTP, partial restart, exact recovery and immutable retry passed')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
