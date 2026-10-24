"""Q7: real child signals, real ctypes engine and a concurrent strict fd-4 reader.

The socket gates synchronize on constructor/bar/stdout events; no sleeps or
deadlines select a lucky bar. The fixture reports its actual callback/fill
counts independently of the runtime's failure args and sink trace records.
"""
from __future__ import annotations

import errno
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import pf_sink
import run_json

MODULE = None
T0 = 1735776000000


def csv_text():
    return "timestamp,open,high,low,close,volume\n" + "".join(
        f"{T0 + i * 60000},{100 + i / 8},{101 + i / 8},"
        f"{99 + i / 8},{100 + i / 8},1\n" for i in range(60))


class RecordingStream:
    def __init__(self, fd):
        self.file = os.fdopen(fd, "rb")
        self.raw = bytearray()

    def readline(self, limit):
        data = self.file.readline(limit)
        self.raw.extend(data)
        return data

    def read(self, count):
        data = self.file.read(count)
        self.raw.extend(data)
        return data

    def close(self):
        self.file.close()


class Child:
    def __init__(self, chart, *, script_tf="1", stop=3, before_start=False,
                 input_stop=-1, before_capture=False, profile=None):
        sink_read, sink_write = os.pipe()
        self.control, child_control = socket.socketpair()
        # Keep the control channel clear of the invocation's actual fd 4.
        control_fd = fcntl.fcntl(child_control.fileno(), fcntl.F_DUPFD_CLOEXEC, 20)
        child_control.close()
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1",
                           PINEFORGE_Q7_CONTROL_FD=str(control_fd))
        environment.pop("PINEFORGE_Q7_BEFORE_START", None)
        environment.pop("PINEFORGE_Q7_BEFORE_CAPTURE", None)
        if before_start:
            environment["PINEFORGE_Q7_BEFORE_START"] = "1"
        if before_capture:
            environment["PINEFORGE_Q7_BEFORE_CAPTURE"] = "1"
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--child", str(sink_write), str(control_fd), "--",
                   "--so", str(MODULE), "--ohlcv", str(chart),
                   "--input-tf", "1", "--script-tf", script_tf,
                   "--inputs", json.dumps({"gate_bar": str(stop), "gate_input": str(input_stop)}),
                   "--report-sink-fd", "4", "--run-id", "q7-python"]
        if profile is not None:
            command.extend(["--profile", profile])
        try:
            self.process = subprocess.Popen(command, pass_fds=(sink_write, control_fd),
                                            env=environment, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE)
        finally:
            os.close(sink_write)
            os.close(control_fd)
        self.events = self.control.makefile("rb", buffering=0)
        self.stream = RecordingStream(sink_read)
        self.records, self.error, self.unexpected = [], None, None
        self.complete, self.trailer = False, None
        self.reader = threading.Thread(target=self.consume)
        self.reader.start()

    def consume(self):
        reader = None
        try:
            reader = pf_sink.SinkReader(stream=self.stream)
            for record in reader:
                self.records.append(record)
        except pf_sink.SinkFormatError as error:
            self.error = error
        except Exception as error:
            self.unexpected = error
        finally:
            if reader is not None:
                self.complete, self.trailer = reader.complete, reader.trailer
                reader.close()
            self.stream.close()

    def event(self):
        line = self.events.readline()
        if not line:
            raise AssertionError("child control EOF before the expected event")
        return line.decode().rstrip("\n")

    def signal_at_gate(self, count):
        for _ in range(count):
            os.kill(self.process.pid, signal.SIGTERM)
            self.control.sendall(b"?")
            if self.event() != "flag 1":
                raise AssertionError("the native SIGTERM handler did not latch its flag")
        self.control.sendall(b"R")

    def finish(self, *, late_term=False):
        observed = []
        while True:
            event = self.event()
            if event.startswith("stdout "):
                if event != "stdout 1":
                    raise AssertionError("stdout publication preceded fd-4 close: " + event)
                if late_term:
                    os.kill(self.process.pid, signal.SIGTERM)
                self.control.sendall(b"R")
                break
            observed.append(event)
        stdout, stderr = self.process.communicate()
        self.reader.join()
        return self.process.returncode, stdout.decode(), stderr.decode(), observed

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.kill()
            self.process.communicate()
        self.reader.join()
        self.events.close()
        self.control.close()


class SigtermTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pineforge-q7-")
        self.addCleanup(self.directory.cleanup)
        self.chart = Path(self.directory.name) / "bars.csv"
        self.chart.write_text(csv_text(), encoding="utf-8")

    def check_abort(self, child, result, completed, started, script_bars=0):
        status, text, errors, observed = result
        self.assertEqual((status, errors), (1, ""), result)
        self.assertEqual(len(text.splitlines()), 1, text)
        document = json.loads(text)
        self.assertEqual(document["engine"], "pineforge")
        self.assertEqual(document["code"], "run_aborted")
        self.assertEqual(document["args"],
                         {"bars_count": completed, "strategy_started": started})
        self.assertIs(type(document["args"]["bars_count"]), int)
        self.assertIs(type(document["args"]["strategy_started"]), bool)
        self.assertEqual(document["bars"], {"count": completed})
        self.assertIs(document["strategy_started"], started)
        self.assertNotIn("retryable", document)
        self.assertNotIn("summary", document)
        self.assertIsNone(child.unexpected)
        self.assertIsInstance(child.error, pf_sink.SinkFormatError)
        self.assertIn("missing header or trailer", str(child.error))
        self.assertFalse(child.complete)
        self.assertIsNone(child.trailer)
        raw = bytes(child.stream.raw)
        self.assertNotIn(b'"trailer":', raw)
        self.assertNotIn(b"run_aborted", raw)
        if raw:
            self.assertTrue(raw.endswith(b"\n"))
            for line in raw.splitlines():
                json.loads(line)  # every retained record, including the 70 KiB X, is whole
        traces = [record.as_dict() for record in child.records if record.tag == "X"]
        tails = [row for row in traces if row["name"] == "tail"]
        applied = [row for row in traces if row["name"] == "applied"]
        identities = [row for row in traces if row["name"] == "i" * 70000]
        for rows in (tails, applied, identities):
            self.assertEqual([row["bar_index"] for row in rows], list(range(script_bars)))
        # The fixture's report counter is independent of the failure args.
        for event in observed:
            if event.startswith("done "):
                self.assertEqual(int(event.split()[-1]), document["bars"]["count"])
        return observed

    def test_signal_during_real_bar_and_repeated_signal(self):
        for script_tf in ("1", "5"):
            for stop, signals in ((0, 1), (3, 2), (9, 2)):
                with self.subTest(script_tf=script_tf, stop=stop, signals=signals):
                    with Child(self.chart, script_tf=script_tf, stop=stop) as child:
                        self.assertEqual(child.event(), f"bar {stop}")
                        child.signal_at_gate(signals)
                        result = child.finish(late_term=signals == 2)
                    completed = (stop + 1) * int(script_tf)
                    observed = self.check_abort(child, result, completed, True, stop + 1)
                    self.assertEqual(observed, [f"done {stop + 1} {stop + 1} 1 {completed}"])

    def test_signal_before_strategy_start(self):
        with Child(self.chart, before_start=True) as child:
            self.assertEqual(child.event(), "create")
            child.signal_at_gate(2)
            result = child.finish(late_term=True)
        self.assertEqual(self.check_abort(child, result, 0, False), ["done 0 0 0 0"])

    def test_input_boundary_with_a_pending_script_aggregate(self):
        with Child(self.chart, script_tf="5", input_stop=2) as child:
            self.assertEqual(child.event(), "input 2")
            child.signal_at_gate(2)
            result = child.finish(late_term=True)
        self.assertEqual(self.check_abort(child, result, 3, True), ["done 0 0 1 3"])

    def test_signal_before_library_load_is_pending_through_startup(self):
        fifo = Path(self.directory.name) / "input.fifo"
        os.mkfifo(fifo)
        with Child(fifo) as child:
            # open returns only once the real loader has opened its read end.
            # SIGTERM is still blocked; no native strategy has been created.
            with fifo.open("w", encoding="utf-8") as feed:
                os.kill(child.process.pid, signal.SIGTERM)
                os.kill(child.process.pid, signal.SIGTERM)
                feed.write(csv_text())
            result = child.finish(late_term=True)
        self.assertEqual(self.check_abort(child, result, 0, False), [])

    def test_jailed_signal_before_library_load_closes_the_inherited_sink(self):
        fifo = Path(self.directory.name) / "jailed-input.fifo"
        os.mkfifo(fifo)
        with Child(fifo, profile="jailed") as child:
            with fifo.open("w", encoding="utf-8") as feed:
                os.kill(child.process.pid, signal.SIGTERM)
                os.kill(child.process.pid, signal.SIGTERM)
                feed.write(csv_text())
            result = child.finish(late_term=True)
        self.assertEqual(self.check_abort(child, result, 0, False), [])

    def test_normal_completion_keeps_the_verified_trailer(self):
        for script_tf, completed in (("1", 60), ("5", 12)):
            with self.subTest(script_tf=script_tf):
                with Child(self.chart, script_tf=script_tf, stop=3) as child:
                    self.assertEqual(child.event(), "bar 3")
                    child.control.sendall(b"R")
                    result = child.finish()
                status, text, errors, observed = result
                self.assertEqual((status, errors), (0, ""), result)
                self.assertEqual(len(text.splitlines()), 1)
                document = json.loads(text)
                self.assertNotIn("code", document)
                self.assertNotIn("run_aborted", text)
                self.assertEqual(document["summary"]["bars_processed"], 60)
                self.assertEqual(document["diagnostics"]["script_bars_processed"], completed)
                self.assertEqual(observed, [f"done {completed} {completed} 1 60"])
                self.assertIsNone(child.unexpected)
                self.assertIsNone(child.error)
                self.assertTrue(child.complete)
                self.assertEqual(child.trailer["counts"]["E"], completed)
                self.assertEqual(child.trailer["counts"]["T"], completed // 2)
                self.assertEqual(bytes(child.stream.raw).count(b'"trailer":'), 1)

    def test_signal_after_last_bar_still_prevents_a_trailer(self):
        with Child(self.chart, stop=-1, before_capture=True) as child:
            self.assertEqual(child.event(), "capture")
            child.signal_at_gate(2)
            result = child.finish(late_term=True)
        self.assertEqual(self.check_abort(child, result, 60, True, 60), ["done 60 60 1 60"])

    def test_mirrors_keep_native_integer_and_boolean_values(self):
        for count in (0, 1, (1 << 32) + 7):
            for started in (False, True):
                args = {"bars_count": count, "strategy_started": started}
                document = json.loads(run_json.failure_line("native run aborted", "run_aborted", args))
                self.assertEqual(document["args"], args)
                self.assertEqual(document["bars"], {"count": count})
                self.assertIs(document["strategy_started"], started)


def child_main(tokens):
    separator = tokens.index("--")
    sink_fd, control_fd = map(int, tokens[:separator])
    os.dup2(sink_fd, 4)
    if sink_fd != 4:
        os.close(sink_fd)
    control = socket.socket(fileno=control_fd)
    original = sys.stdout

    class StdoutProbe:
        def write(self, text):
            try:
                os.fstat(4)
                closed = False
            except OSError as error:
                if error.errno != errno.EBADF:
                    raise
                closed = True
            control.sendall(f"stdout {int(closed)}\n".encode())
            if control.recv(1) != b"R":
                raise AssertionError("stdout gate was not released")
            return original.write(text)

        def flush(self):
            return original.flush()

        def fileno(self):
            return original.fileno()

    sys.stdout = StdoutProbe()
    try:
        return run_json.main(tokens[separator + 1:])
    finally:
        sys.stdout = original
        control.close()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        raise SystemExit(child_main(sys.argv[2:]))
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", required=True, type=Path)
    own, remaining = parser.parse_known_args()
    MODULE = own.module.resolve()
    if not MODULE.is_file():
        parser.error("the CMake-built native module is missing")
    unittest.main(argv=[sys.argv[0], *remaining])
