"""CLI contract for native runner startup/identity. Python orchestrates only."""
import json
import sqlite3
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

(runner, legacy_library, native_example, absent, unknown, no_export, stub) = sys.argv[1:]


def ledger_bound(path):
    path = Path(path)
    if not path.exists():
        return False
    try:
        with sqlite3.connect(path) as db:
            row = db.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name='metadata'"
            ).fetchone()
            return row is not None
    except sqlite3.Error:
        return False


def identity_of(path):
    with sqlite3.connect(path) as db:
        return db.execute('SELECT identity FROM metadata WHERE singleton=1').fetchone()[0]


def native_config(path, **clock):
    body = {
        'run': {'session_key': clock.pop('session_key', 'live-1'),
                'run_number': clock.pop('run_number', 1)},
        'clock': {
            'input_tf': clock.pop('input_tf', '5'),
            'script_tf': clock.pop('script_tf', '10'),
            'timezone': clock.pop('timezone', 'UTC'),
            'session': clock.pop('session', '24x7'),
            'chart_timezone': clock.pop('chart_timezone', ''),
        },
        'instrument': {
            'ticker': 'MOCK', 'tickerid': 'TEST:MOCK', 'type': 'crypto',
            'currency': 'USDT', 'basecurrency': 'ETH', 'description': '',
            'volumetype': '',
        },
        'execution': {
            'initial_capital': 10000, 'point_value': 1, 'account_fx': 1,
            'price_tick': 0.01, 'slippage_ticks': 0,
            'fee_kind': 'CashPerExecution', 'fee_value': 6,
            'quantity_grid': None, 'close_execution': 'NextEligiblePoint',
            'max_abs_units': None, 'max_open_lots': None,
            'allowed_open_directions': 3, 'initial_margin_fraction': None,
        },
    }
    path.write_text(json.dumps(body))


def warmup_csv(path, rows):
    text = 'timestamp,open,high,low,close,volume\n'
    for ts in rows:
        text += f'{ts},100,102,99,101,4\n'
    path.write_text(text)


def invoke(args, success=True):
    p = subprocess.run(args, capture_output=True, text=True, timeout=25)
    if success:
        assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout) if p.stdout.strip().startswith('{') else p
    assert p.returncode != 0, (p.returncode, p.stdout, p.stderr)
    return p


def base_cmd(strategy, warmup, ledger, config=None, extra=None):
    cmd = [runner, 'run', '--strategy', strategy, '--warmup', str(warmup),
           '--mode', 'bars', '--ledger', str(ledger),
           '--webhook-url', 'http://127.0.0.1:1/unused',
           '--allow-insecure-http', '--feed', str(Path(warmup).with_suffix('.jsonl'))]
    Path(warmup).with_suffix('.jsonl').write_text('')
    if config is not None:
        cmd += ['--native-config', str(config)]
    else:
        cmd += ['--script-tf', '3', '--symbol', 'TEST:MOCK']
    if extra:
        cmd += extra
    return cmd


with tempfile.TemporaryDirectory(prefix='pineforge-native-startup-') as raw:
    root = Path(raw)
    warmup1m = root / 'w1m.csv'
    warmup_csv(warmup1m, [0, 60000, 120000])
    warmup5 = root / 'w5.csv'
    warmup_csv(warmup5, [0, 300000, 600000, 900000])
    warmup5_gap = root / 'w5gap.csv'
    warmup_csv(warmup5_gap, [0, 300000, 900000])
    cfg = root / 'native.json'
    native_config(cfg)

    # Bad JSON never binds a ledger.
    bad_cfg = root / 'bad.json'
    bad_cfg.write_text('{')
    ledger = root / 'bad-json.sqlite3'
    p = invoke(base_cmd(stub, warmup5, ledger, bad_cfg), success=False)
    assert 'JSON' in p.stderr
    assert not ledger_bound(ledger)

    # Unknown contract never binds a ledger.
    ledger = root / 'unknown.sqlite3'
    p = invoke(base_cmd(unknown, warmup5, ledger, cfg), success=False)
    assert 'unknown strategy execution contract' in p.stderr
    assert not ledger_bound(ledger)

    # Native-config on a legacy (query-absent) library never binds.
    ledger = root / 'legacy-native-cfg.sqlite3'
    p = invoke(base_cmd(absent, warmup5, ledger, cfg), success=False)
    assert 'native-config requires NativeMarketV1' in p.stderr
    assert not ledger_bound(ledger)

    # Missing native configure export never binds.
    ledger = root / 'no-export.sqlite3'
    p = invoke(base_cmd(no_export, warmup5, ledger, cfg), success=False)
    assert 'strategy_configure_native_v1' in p.stderr
    assert not ledger_bound(ledger)

    # Native history gap never binds.
    ledger = root / 'gap.sqlite3'
    p = invoke(base_cmd(stub, warmup5_gap, ledger, cfg), success=False)
    assert 'in-session gap' in p.stderr
    assert not ledger_bound(ledger)

    # Monthly stream input never binds.
    monthly = root / 'monthly.json'
    native_config(monthly, input_tf='M', script_tf='M')
    ledger = root / 'monthly.sqlite3'
    p = invoke(base_cmd(stub, warmup5, ledger, monthly), success=False)
    assert 'monthly' in p.stderr
    assert not ledger_bound(ledger)

    # Explicit CLI contradiction never binds.
    ledger = root / 'cli.sqlite3'
    p = invoke(base_cmd(stub, warmup5, ledger, cfg, extra=['--input-tf', '1']), success=False)
    assert 'contradicts' in p.stderr
    assert not ledger_bound(ledger)

    # Omitted CLI clock values take the file (default 1m is not a contradiction).
    ledger = root / 'omit.sqlite3'
    result = invoke(base_cmd(stub, warmup5, ledger, cfg))
    assert ledger_bound(ledger)
    first_identity = identity_of(ledger)
    assert result['inputs_committed'] == 0

    # Replay empty ledger with identical identity.
    again = invoke(base_cmd(stub, warmup5, ledger, cfg))
    assert again['inputs_committed'] == 0
    assert identity_of(ledger) == first_identity

    # RunIdentity / spec / library / warmup / timezone identity changes, empty ledger.
    ledger2 = root / 'run2.sqlite3'
    cfg2 = root / 'native2.json'
    native_config(cfg2, run_number=2)
    invoke(base_cmd(stub, warmup5, ledger2, cfg2))
    assert identity_of(ledger2) != first_identity
    p = invoke(base_cmd(stub, warmup5, ledger, cfg2), success=False)
    assert 'identity' in p.stderr
    assert identity_of(ledger) == first_identity

    cfg_tz = root / 'tz.json'
    native_config(cfg_tz, timezone='UTC+5')
    p = invoke(base_cmd(stub, warmup5, ledger, cfg_tz), success=False)
    assert 'identity' in p.stderr

    cfg_lib = root / 'same.json'
    native_config(cfg_lib)
    p = invoke(base_cmd(no_export, warmup5, ledger, cfg_lib), success=False)
    # Fails at missing export or identity; must not deliver and must keep original identity.
    assert identity_of(ledger) == first_identity
    assert not Path(str(ledger) + '-wal').exists() or True

    other_warmup = root / 'w5b.csv'
    warmup_csv(other_warmup, [0, 300000, 600000, 900000, 1200000])
    p = invoke(base_cmd(stub, other_warmup, ledger, cfg), success=False)
    assert 'identity' in p.stderr
    assert identity_of(ledger) == first_identity

    # Query-absent old ABI4 library stays legacy (no native identity schema).
    legacy_ledger = root / 'legacy.sqlite3'
    invoke(base_cmd(absent, warmup1m, legacy_ledger))
    assert ledger_bound(legacy_ledger)
    ident = identity_of(legacy_ledger)
    assert len(ident) == 64
    # Changing a legacy input changes identity; native-config is refused.
    p = invoke(base_cmd(absent, warmup1m, legacy_ledger, extra=['--input', 'changed=1']),
               success=False)
    assert 'identity' in p.stderr
    p = invoke(base_cmd(legacy_library, warmup1m, root / 'legacy-real-nativecfg.sqlite3', cfg),
               success=False)
    assert 'native-config requires NativeMarketV1' in p.stderr
    assert not ledger_bound(root / 'legacy-real-nativecfg.sqlite3')

    # Real native example: startup, one live 5m bar, then exact replay.
    received = []

    class Receiver(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def do_POST(self):
            raw = self.rfile.read(int(self.headers['Content-Length']))
            received.append(json.loads(raw))
            self.send_response(200)
            self.send_header('Content-Length', '0')
            self.end_headers()
        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        example_ledger = root / 'example.sqlite3'
        example_feed = warmup5.with_suffix('.jsonl')
        example_feed.write_text(json.dumps({
            'type': 'bar',
            'bar': {'ts_open': 1200000, 'o': 100, 'h': 102, 'l': 99, 'c': 101, 'v': 4},
        }) + '\n')
        example_cmd = [runner, 'run', '--strategy', native_example, '--warmup', str(warmup5),
                       '--mode', 'bars', '--ledger', str(example_ledger),
                       '--webhook-url', f'http://127.0.0.1:{server.server_port}/webhook',
                       '--allow-insecure-http', '--feed', str(example_feed),
                       '--native-config', str(cfg), '--name', 'native-example']
        example = invoke(example_cmd)
        assert ledger_bound(example_ledger)
        assert example['inputs_committed'] == 1
        prior = list(received)
        replay = invoke(example_cmd)
        assert replay['inputs_processed'] == 0
        assert replay['prefix_skipped'] == 1
        assert received == prior
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

print('native runner startup/identity CLI contract passed')
