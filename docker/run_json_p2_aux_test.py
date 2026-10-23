"""P2 auxiliary red-first tests for sidecar and temporary-file admission.

The run_json_* name keeps this file in the preflight pytest collection.
"""
from __future__ import annotations

import importlib
import multiprocessing
import os

import pytest


HEADER = "timestamp,open,high,low,close,volume"
T0 = 1735776000000


def _sidecar_worker(path, result):
    pf_bars = importlib.import_module("pf_bars")
    try:
        pf_bars.read_sidecar(path)
    except pf_bars.BarsError as error:
        result.send(("bars", error.code_args))
    except BaseException as error:  # report uncaught RecursionError without hanging pytest
        result.send(("exception", type(error).__name__))
    else:
        result.send(("accepted", None))
    finally:
        result.close()


def _assert_sidecar_refusal(path):
    context = multiprocessing.get_context("fork")
    parent, child = context.Pipe(duplex=False)
    process = context.Process(target=_sidecar_worker, args=(str(path), child))
    process.start()
    child.close()
    process.join(1.0)
    if process.is_alive():
        process.terminate()
        process.join()
        pytest.fail(f"read_sidecar did not refuse {path} within one second")
    assert process.exitcode == 0
    assert parent.poll()
    assert parent.recv() == ("bars", {"reason": "sidecar"})
    parent.close()


def test_sidecar_nonregular_oversize_and_deep_are_sidecar_refusals(tmp_path):
    """E-06: every hostile sidecar is a bounded, typed refusal."""
    fifo = tmp_path / "fifo.bin.json"
    os.mkfifo(fifo)
    _assert_sidecar_refusal(fifo)

    oversized = tmp_path / "oversized.bin.json"
    oversized.write_bytes(b'{"padding":"' + b"x" * (4 * 1024 * 1024) + b'"}')
    _assert_sidecar_refusal(oversized)

    deep = tmp_path / "deep.bin.json"
    depth = 100_000
    deep.write_bytes(b"[" * depth + b"]" * depth)
    _assert_sidecar_refusal(deep)


def _csv_source(directory):
    path = directory / "input.csv"
    path.write_text(
        HEADER + "\n"
        + f"{T0},1,2,0.5,1.5,3\n",
        encoding="utf-8",
    )
    return path


@pytest.mark.parametrize(
    ("feed", "temporary_name"),
    [
        (False, "out.bin.tmp"),
        (False, "out.bin.json.tmp"),
        (True, "out.bin.tmp"),
        (True, "out.close.bin.tmp"),
        (True, "out.bin.json.tmp"),
    ],
    ids=["chart-bars", "chart-sidecar", "feed-bars", "feed-closes", "feed-sidecar"],
)
def test_csv2bin_refuses_symlinked_temporaries(tmp_path, feed, temporary_name):
    """E-07: fixed temporary names cannot follow a planted symlink."""
    pf_bars = importlib.import_module("pf_bars")
    work = tmp_path / temporary_name.replace(".", "-")
    work.mkdir()
    source = _csv_source(work)
    output = work / "out.bin"
    sentinel = work / "sentinel"
    original = b"sentinel must remain unchanged\n"
    sentinel.write_bytes(original)
    (work / temporary_name).symlink_to(sentinel)

    with pytest.raises(pf_bars.BarsError) as caught:
        pf_bars.convert_csv(source, output, feed=feed, symbol="ETHUSDT", tf="1")
    assert caught.value.code_args == {"reason": "io"}
    assert sentinel.read_bytes() == original
