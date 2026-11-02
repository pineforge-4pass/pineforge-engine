"""Compile-stage loader: exercise the linked module, without timing a backtest."""

import argparse
import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--so", required=True)
parser.add_argument("--ohlcv", required=True)
args, _ = parser.parse_known_args()
Path(os.environ["PF_ENTRYPOINT_FIXTURE_RUN_STARTED"]).write_text(str(os.getpid()))
Path(os.environ["PF_ENTRYPOINT_FIXTURE_TRACE"], "artifact.sha256").write_text(
    hashlib.sha256(Path(args.so).read_bytes()).hexdigest(), encoding="ascii")
module = ctypes.CDLL(args.so)
module.strategy_create.argtypes = [ctypes.c_char_p]
module.strategy_create.restype = ctypes.c_void_p
module.strategy_free.argtypes = [ctypes.c_void_p]
module.strategy_free.restype = None
strategy = module.strategy_create(b"{}")
if not strategy:
    raise RuntimeError("strategy_create failed")
module.strategy_free(strategy)
module.entrypoint_fixture_signal.argtypes = [ctypes.c_double, ctypes.c_double]
module.entrypoint_fixture_signal.restype = ctypes.c_double
with open(args.ohlcv, newline="", encoding="utf-8") as feed:
    signals = [module.entrypoint_fixture_signal(float(bar["open"]), float(bar["close"]))
               for bar in csv.DictReader(feed)]
print(json.dumps({"signals": signals}, separators=(",", ":")))
