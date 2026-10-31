#!/usr/bin/env python3
"""Trusted preparation and CSV conversion around a compute-owned jailed run.

The callable launcher receives argv, env, cwd, arena_fd and report_fd. It
owns descriptor installation and the jail, and returns status, stdout,
stderr, report_bytes and events. Events are observations made in the child:
one ``jail`` record (profile, limits, cwd/TMP and arena) and one ``command``
record (script, profile and the open report descriptor). They are launcher
receipts, never claims read from a strategy report.

The adapter drains its supplied report pipe concurrently. A launcher that
captures a pipe of its own returns those exact bytes as report_bytes instead.
Diagnostics must also be drained concurrently. No report path, stdout trade
array or adapter-generated receipt can substitute for the sink finalizer.

The harness directory is resolved in this order: the keyword-only
``run_cases(harness_dir=...)`` argument, ``PINEFORGE_PARITY_HARNESS_DIR``,
then the source-checkout ``REPO_ROOT / "docker"`` default only when no
``PARITY-MANIFEST.json`` is beside this module. An installed bundle with
neither explicit configuration nor the environment variable is refused.
"""
from __future__ import annotations

import errno
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
from collections.abc import Mapping
from types import SimpleNamespace as _Namespace

from parity_paths import resolve_paths as _resolve_paths
import run_strategy as _strategy
from pf_release_run import (
    report_trades_to_runstrategy_shape as _report_trades_to_runstrategy_shape,
)
from verify_corpus import load_strategy_metadata as _load_strategy_metadata

_INITIAL_PARITY_PATHS = _resolve_paths()
_INITIAL_HARNESS_DIR = _INITIAL_PARITY_PATHS.harness_dir
_RUN_JSON = (None if _INITIAL_HARNESS_DIR is None
             else _INITIAL_HARNESS_DIR / "run_json.py")
_FINALIZER = (None if _INITIAL_HARNESS_DIR is None
              else _INITIAL_HARNESS_DIR / "pineforge_sink_finalize.py")
_ARENA_SEALS = 0x0004 | 0x0002 | 0x0001  # GROW | SHRINK | SEAL, still writable
_STAGED_DERIVED_FILES = (
    "ohlcv_ETH-USDT-USDT_15m.csv",
    "ohlcv_ETH-USDT-USDT_15m_window.csv",
)
_DERIVATION_STAMP = b"v2:first-positive-volume-open\n"
_SINK_REQUIRED = "refusing jailed parity: sink mode must be fd-on"
_HARNESS_REQUIRED = (
    "parity harness directory is not configured: "
    "pass harness_dir or set PINEFORGE_PARITY_HARNESS_DIR"
)


class _Refusal(RuntimeError):
    def __init__(self, text, document=None, raw_document=None):
        super().__init__(text)
        self.document = document
        self.raw_document = raw_document


def _unsupported(name):
    raise _Refusal(f"unsupported case parameter: {name}")


def _corpus_root(value):
    if not isinstance(value, (str, os.PathLike)):
        _unsupported("corpus_root")
    try:
        path = Path(os.fspath(value)).expanduser()
    except (TypeError, ValueError):
        _unsupported("corpus_root")
    if not path.is_absolute():
        _unsupported("corpus_root")
    return path.resolve()


def _lower_hex(value, length):
    return (
        isinstance(value, str)
        and len(value) == length
        and value == value.lower()
        and all(character in "0123456789abcdef" for character in value)
    )


def _validate_corpus_parameters(corpus_root, corpus_commit, staged_derived,
                                require_read_only_corpus):
    root = None if corpus_root is None else _corpus_root(corpus_root)
    if corpus_commit is not None and not _lower_hex(corpus_commit, 40):
        _unsupported("corpus_commit")
    if staged_derived is None:
        return root, None
    if root is None:
        _unsupported("corpus_root")
    if not isinstance(staged_derived, Mapping):
        _unsupported("staged_derived")
    try:
        if set(staged_derived) != set(_STAGED_DERIVED_FILES):
            _unsupported("staged_derived")
    except (TypeError, ValueError):
        _unsupported("staged_derived")
    hashes = {}
    for name in _STAGED_DERIVED_FILES:
        digest = staged_derived[name]
        if not _lower_hex(digest, 64):
            _unsupported("staged_derived")
        hashes[name] = digest
    if type(require_read_only_corpus) is not bool:
        _unsupported("require_read_only_corpus")
    expected_root = _INITIAL_PARITY_PATHS.corpus_root.resolve()
    expected_derived = _INITIAL_PARITY_PATHS.derived_workspace.resolve()
    if (root != expected_root
            or (root / "data" / "derived").resolve() != expected_derived):
        raise _Refusal("corpus_root differs from the resolved parity paths")
    if require_read_only_corpus:
        try:
            read_only = bool(os.statvfs(root).f_flag & os.ST_RDONLY)
        except OSError:
            read_only = False
        if not read_only:
            raise _Refusal(f"corpus is not mounted read-only: {root}")
    derived = root / "data" / "derived"
    for name in _STAGED_DERIVED_FILES:
        path = derived / name
        if path.is_symlink() or not path.is_file():
            raise _Refusal(f"missing corpus input: {path}")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != hashes[name]:
            raise _Refusal(f"staged derived feed differs: {name}")
    stamp = derived / ".derive_corpus_feeds.version"
    try:
        stamp_matches = stamp.read_bytes() == _DERIVATION_STAMP
    except OSError:
        stamp_matches = False
    if not stamp_matches:
        raise _Refusal("staged derived stamp differs")
    return root, hashes


def _harness_paths(harness_dir=None):
    selected = _resolve_paths(harness_dir=harness_dir).harness_dir
    if selected is None:
        raise _Refusal(_HARNESS_REQUIRED)
    paths = _Namespace(
        directory=selected,
        run_json=selected / "run_json.py",
        finalizer=selected / "pineforge_sink_finalize.py",
    )
    global _RUN_JSON, _FINALIZER
    _RUN_JSON, _FINALIZER = paths.run_json, paths.finalizer
    return paths


_MISSING = object()


def _effective_report_cap(launcher, requested):
    if requested is not None:
        if type(requested) is not int or requested < 0:
            _unsupported("report_max_bytes")
        return requested
    cap = getattr(launcher, "report_max_bytes", _MISSING)
    if cap is _MISSING or cap is None:
        return None
    if type(cap) is not int or cap <= 0:
        _unsupported("launcher.report_max_bytes")
    return cap


def _json_bytes(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _nonfinite(text):
    raise ValueError(f"nonfinite JSON value: {text}")


def _document(raw):
    value = json.loads(raw, object_pairs_hook=_unique_pairs,
                       parse_constant=_nonfinite)
    if not isinstance(value, dict):
        raise ValueError("expected one JSON object")
    return value


def _path(directory, value, name):
    if not isinstance(value, (str, os.PathLike)) or not os.fspath(value):
        _unsupported(name)
    path = Path(value)
    return (path if path.is_absolute() else directory / path).resolve()


def _remember(hashes, path, *, prepared=False, optional=False):
    path = Path(path).resolve()
    if optional and not path.exists():
        if path in hashes and hashes[path] is not None:
            raise _Refusal(f"prepared input changed: {path}")
        hashes[path] = None  # An absent optional input must stay absent.
        return path
    digest = _strategy._sha256_file(path) if path.is_file() else None
    if digest is None:
        prefix = "prepared input changed" if prepared else "missing corpus input"
        raise _Refusal(f"{prefix}: {path}")
    if path in hashes and hashes[path] != digest:
        raise _Refusal(f"prepared input changed: {path}")
    hashes[path] = digest
    return path


def _recheck(hashes):
    for path, expected in hashes.items():
        actual = _strategy._sha256_file(path) if path.is_file() else None
        if actual != expected or (expected is None and path.exists()):
            raise _Refusal(f"prepared input changed: {path}")


def _text(value, name, limit):
    if not isinstance(value, str):
        _unsupported(name)
    try:
        length = len(value.encode("utf-8"))
    except UnicodeEncodeError:
        _unsupported(name)
    if not 1 <= length <= limit or "\0" in value:
        _unsupported(name)
    return value


def _detected_timeframe(stamps):
    # src/timeframe.cpp:20-70, as reached by absent Strategy.run timeframes.
    # Fewer than two bars preserve an undetected batch state, not a default.
    if len(stamps) < 2:
        _unsupported("input_tf")
    deltas = sorted(b - a for a, b in zip(stamps[:100], stamps[1:101])
                    if b > a)
    if not deltas:
        return "1"  # detect_timeframe's explicit empty-positive-gap outcome.
    seconds = deltas[len(deltas) // 2] // 1000
    standards = ((60, "1"), (180, "3"), (300, "5"), (900, "15"),
                 (1800, "30"), (3600, "60"), (7200, "120"),
                 (14400, "240"), (86400, "D"), (604800, "W"))
    return min(standards, key=lambda item: abs(seconds - item[0]))[1]


def _case_metadata(directory):
    hashes = {}
    _remember(hashes, directory / "inputs.json", optional=True)
    try:
        params = _load_strategy_metadata(directory)
    except ValueError:
        _unsupported("inputs.json")
    if not isinstance(params, dict):
        _unsupported("inputs.json")
    # Bind the metadata read to its preparation hash, including absence.
    _recheck(hashes)
    return _Namespace(directory=directory, case_id=directory.name,
                      params=params, hashes=hashes)


def _prepare(case, directory, harness_paths, corpus_commit=None):
    params, hashes = case.params, case.hashes
    source = _remember(hashes, case.directory / "generated.cpp", prepared=True)
    library = _remember(hashes, _strategy.find_strategy_lib(case.directory), prepared=True)
    chart = _remember(hashes, _path(
        case.directory, params.get("ohlcv_csv", _strategy.DEFAULT_OHLCV), "ohlcv_csv"))
    # Resolve dependencies before the shared helper, whose FileNotFoundError
    # messages describe the ctypes runner rather than the adapter contract.
    for key in ("aux_security_ohlcv_csv", "tv_trades_csv", "tv_metrics_json"):
        if params.get(key):
            _remember(hashes, _path(case.directory, params[key], key))
    feeds = params.get("native_security_feeds") or {}
    if not isinstance(feeds, dict):
        _unsupported("native_security_feeds")
    for value in feeds.values():
        _remember(hashes, _path(case.directory, value, "native_security_feeds"))
    runtime = params.get("runtime_overrides") or {}
    if not isinstance(runtime, dict):
        _unsupported("runtime_overrides")
    for key in ("account_currency_fx_series_json", "account_currency_fx_daily_close_json"):
        if runtime.get(key):
            _remember(hashes, _path(case.directory, runtime[key], "runtime_overrides." + key))
    chart, kwargs = _strategy.inputs_run_kwargs(
        params, case.directory, _strategy.DEFAULT_OHLCV, default_chart_tz="")
    if "ohlcv_end_ms" in params:
        kwargs["ohlcv_end_ms"] = int(params["ohlcv_end_ms"])
    # The current run_json route has no equivalent of these ctypes channels.
    # Refuse them explicitly instead of changing a case's engine settings.
    for name in ("aux_security_ohlcv_csv", "native_security_feeds",
                 "account_currency_fx_series"):
        if kwargs.get(name):
            _unsupported(name)
    if _strategy.load_probe_requests(case.directory) is not None:
        _unsupported("probe_requests")
    supported_runtime = {
        "timezone", "session", "mintick", "pointvalue", "syminfo_metadata",
        "bar_magnifier", "magnifier_samples", "magnifier_distribution",
        "magnifier_volume_weighted",
    }
    for name in runtime:
        if name not in supported_runtime:
            _unsupported(f"runtime_overrides.{name}")

    selected = params.get("selected_window")
    if selected is not None and not isinstance(selected, dict):
        _unsupported("selected_window")
    report_window, trade_start, signal_start = None, None, None
    if selected is None:
        tape = _path(case.directory, params.get("tv_trades_csv", "tv_trades.csv"), "tv_trades_csv")
        _remember(hashes, tape, optional="tv_trades_csv" not in params)
        if tape.is_file():
            metrics = _strategy._tv_metrics_path(case.directory, params, tape)
            _remember(hashes, metrics, optional="tv_metrics_json" not in params)
        span = _strategy._load_tv_entry_span(case.directory, params)
        if span is not None:
            _strategy._apply_range_end_regime(case.directory, params, kwargs)
            stamps = _strategy._feed_timestamps(
                chart, ohlcv_start_ms=kwargs.get("ohlcv_start_ms"),
                ohlcv_end_ms=kwargs.get("ohlcv_end_ms"))
            first = _strategy._tv_first_bar(case.directory, params)
            interval = _strategy._infer_bar_interval_ms(chart)
            window = _strategy._tv_entry_emit_window(
                stamps, span[0], span[1], interval,
                None if first is None else first[0])
            report_window = (window.report_start_ms, window.end_ms)
            trade_start = window.start_ms
            if window.tv_first_bar_ms is not None:
                signal = _strategy._tv_entry_emit_window(
                    stamps, span[0], span[1], interval).start_ms
                if stamps and signal - interval > stamps[0]:
                    signal_start = signal

    # This helper is the same pre-trimmer used by the release runner. It
    # operates in trusted preparation, before the child has RLIMIT_FSIZE=0.
    trimmed = _strategy._trim_ohlcv_csv(
        chart, kwargs.get("ohlcv_start_ms"), kwargs.get("ohlcv_end_ms"))
    if trimmed is not None:
        staged_chart = directory / "ohlcv.csv"
        try:
            # The helper's temporary directory may be on another filesystem.
            shutil.copyfile(trimmed, staged_chart)
        finally:
            trimmed.unlink()
        chart_for_run = _remember(hashes, staged_chart)
        chart_for_run.chmod(0o444)
    else:
        chart_for_run = chart
    stamps = _strategy._feed_timestamps(chart_for_run)
    if not stamps:
        _unsupported("ohlcv_csv")
    timezone = _text(kwargs["chart_timezone"] or "UTC", "chart_timezone", 128)
    # Resolve the values the engine would otherwise infer from this feed.
    if selected is None:
        input_tf = kwargs.get("input_tf") or _detected_timeframe(stamps)
        script_tf = kwargs.get("script_tf") or input_tf
        # The source adapter retains an explicit equal-duration script
        # spelling when input_tf was detected (D versus 1D, for example).
        # See src/source/pine_adapter.cpp:2948-2955.
        if (not kwargs.get("input_tf") and kwargs.get("script_tf")
                and _strategy._tf_seconds(input_tf) > 0
                and _strategy._tf_seconds(input_tf) == _strategy._tf_seconds(script_tf)):
            input_tf = script_tf
    else:
        input_tf = _text(selected.get("input_tf"), "selected_window.input_tf", 32)
        script_tf = _text(selected.get("script_tf"), "selected_window.script_tf", 32)
    input_tf = _text(input_tf, "input_tf", 32)
    script_tf = _text(script_tf, "script_tf", 32)
    inputs = {}
    metadata_keys = _strategy._VALIDATION_META_KEYS | {
        "selected_window", "ohlcv_end_ms",
    }
    for name, value in params.items():
        if name in metadata_keys or name.startswith("tv_"):
            continue
        if isinstance(value, (dict, list)) or value is None:
            _unsupported(name)
        inputs[str(name)] = str(value)
    overrides = {str(k): str(v) for k, v in
                 (kwargs.get("strategy_overrides") or {}).items()}
    argv = [
        sys.executable, str(harness_paths.run_json), "--so", str(library),
        "--ohlcv", str(chart_for_run), "--generated-cpp", str(source),
        "--profile", "jailed", "--report-sink-format", "1",
        "--run-id", case.case_id, "--chart-tz", timezone,
        "--input-tf", input_tf, "--script-tf", script_tf,
        "--inputs", _json_bytes(inputs).decode("ascii"),
        "--overrides", _json_bytes(overrides).decode("ascii"),
    ]
    if selected is not None:
        required = {
            "input_tf", "script_tf", "window_start_ms", "window_end_ms",
            "preroll_bars", "fed_start_ms", "request_feed_inventory",
        }
        for name in sorted(required):
            if name not in selected:
                _unsupported(f"selected_window.{name}")
        for name in selected:
            if name not in required:
                _unsupported(f"selected_window.{name}")
        inventory = _remember(hashes, _path(
            case.directory, selected["request_feed_inventory"], "selected_window.request_feed_inventory"))
        argv += ["--report-policy", "selected-window/v1",
                 "--request-feed-inventory", str(inventory)]
        for name in ("window_start_ms", "window_end_ms", "preroll_bars", "fed_start_ms"):
            value = selected[name]
            if type(value) is not int:
                _unsupported(f"selected_window.{name}")
            argv += ["--" + name.replace("_", "-"), str(value)]
        report_window = (selected["window_start_ms"], selected["window_end_ms"])
    elif trade_start is not None:
        argv += ["--trade-start-ms", str(trade_start)]

    syminfo = {}
    for key in ("timezone", "session", "mintick", "pointvalue"):
        value = kwargs.get("syminfo_" + key)
        if value is not None:
            syminfo[key] = value
    for key, value in (kwargs.get("syminfo_metadata") or {}).items():
        if key != "mincontract":
            _unsupported("runtime_overrides.syminfo_metadata." + key)
        syminfo[key] = value
    if syminfo:
        syminfo_path = directory / "syminfo.json"
        syminfo_path.write_bytes(_json_bytes(syminfo))
        syminfo_path.chmod(0o444)
        _remember(hashes, syminfo_path)
        argv += ["--syminfo", str(syminfo_path)]
    dist = str(kwargs["magnifier_distribution"]).upper()
    if dist not in {"UNIFORM", "COSINE", "TRIANGLE", "ENDPOINTS",
                    "FRONT_LOADED", "BACK_LOADED", "VOLUME_WEIGHTED"}:
        _unsupported("runtime_overrides.magnifier_distribution")
    argv += ["--bar-magnifier", str(bool(kwargs["bar_magnifier"])).lower(),
             "--magnifier-samples", str(kwargs["magnifier_samples"]),
             "--magnifier-dist", "endpoints" if dist == "VOLUME_WEIGHTED" else dist.lower()]
    if kwargs["magnifier_volume_weighted"] or dist == "VOLUME_WEIGHTED":
        argv += ["--magnifier-volume-weighted"]
    temporary = directory / "tmp"
    temporary.mkdir(mode=0o555)
    manifest = {
        "case_id": case.case_id, "case_dir": str(case.directory),
        "hashes": {str(p): digest for p, digest in hashes.items()},
        "argv": argv, "report_window": report_window,
        "trade_start_ms": trade_start, "signal_start_ms": signal_start,
    }
    if corpus_commit is not None:
        manifest["corpus_commit"] = corpus_commit
    manifest_path = directory / "preparation.json"
    manifest_path.write_bytes(_json_bytes(manifest))
    manifest_path.chmod(0o444)
    _remember(hashes, manifest_path)
    directory.chmod(0o555)
    _recheck(hashes)
    return _Namespace(
        case_id=case.case_id, directory=case.directory, cwd=directory,
        temporary=temporary, argv=argv, hashes=hashes, manifest=manifest,
        manifest_sha256=hashes[manifest_path], stamps=stamps,
        chart=chart_for_run, selected=selected, report_window=report_window,
        signal_start=signal_start, inputs=inputs, overrides=overrides)


def _arena_fd(size):
    fd = os.memfd_create("pineforge-parity-arena",
                         os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        os.ftruncate(fd, size)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, _ARENA_SEALS)
        # A source descriptor is not the child's fd-5 destination.
        return fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 64)
    finally:
        os.close(fd)


def _close_fd(fd):
    try:
        os.close(fd)
    except OSError as error:
        # A synchronous launcher may take ownership of the handed endpoint.
        if error.errno != errno.EBADF:
            raise


def _event(launched, kind):
    events = [event for event in launched.events if event.get("kind") == kind]
    if len(events) != 1:
        _unsupported("launcher." + kind)
    return events[0]


def _validate_arena(jail, expected_bytes):
    """Check the child's fd-5 backing, not the parent's original memfd."""
    arena = jail.get("arena", {})
    return (
        arena.get("fd_open") is True
        and arena.get("is_regular") is True
        and arena.get("is_memfd") is True
        and isinstance(arena.get("target"), str)
        and arena["target"].startswith("/memfd:")
        and type(arena.get("bytes")) is int
        and arena["bytes"] == expected_bytes > 0
        and type(arena.get("seals")) is int
        and arena["seals"] & _ARENA_SEALS == _ARENA_SEALS
        and jail.get("arena_env_bytes") == expected_bytes
    )


def _child_document(launched):
    try:
        return _document(launched.stdout)
    except (ValueError, UnicodeError):
        return None


def _child_refusal(launched, document, fallback):
    if document is not None and document.get("error"):
        raise _Refusal(str(document["error"]), document, launched.stdout)
    raise _Refusal(fallback)


def _check_launch(launched, arena_bytes, run_json):
    document = _child_document(launched)
    jail = _event(launched, "jail")
    if not _validate_arena(jail, arena_bytes):  # MUTATION: jailed-arena-admission
        _child_refusal(launched, document,
                       "the jailed profile requires an attached scratch arena")
    command = _event(launched, "command")
    report = command.get("report", {})
    fd = report.get("fd")
    if fd is None:
        raise _Refusal(_SINK_REQUIRED, document if document and document.get("error") else None)
    message = None
    if fd == 5:
        message = "error: --report-sink-fd 5 collides with PF_SCRATCH_ARENA_FD"
    elif fd == 1:
        message = "report fd must differ from stdout (fd 1)"
    elif type(fd) is not int or fd < 7:
        message = "report fd must be >= 7"
    elif not (report.get("open") is True and report.get("writable") is True
              and report.get("fifo_or_socket") is True):
        message = "report fd is not a writable stream"
    if message is not None:
        raise _Refusal(message, document if document and document.get("error") else None,
                       launched.stdout)
    if (jail.get("profile") != "jailed" or command.get("profile") != "jailed"
            or jail.get("fsize") != [0, 0] or jail.get("sigxfsz_ignored") is not True
            or jail.get("work_read_only") is not True or jail.get("tmp_read_only") is not True
            or command.get("script") != str(run_json)):
        _unsupported("launcher.jail")
    if launched.status != 0 or (document is not None and document.get("error")):
        _child_refusal(launched, document, f"jailed harness exited {launched.status}")
    if document is None or document.get("engine") != "pineforge":
        raise _Refusal("unexpected stdout from jailed harness")
    if "trades" in document or "equity_curve" in document or "report_sink" in document:
        raise _Refusal(_SINK_REQUIRED)
    return document


class _PipeCapture:
    def __init__(self, fd, cap):
        self.fd, self.cap = fd, cap
        self.chunks, self.length, self.error = [], 0, None
        self.digest = hashlib.sha256()
        self.thread = threading.Thread(target=self._drain)

    def _drain(self):
        try:
            while chunk := os.read(self.fd, 64 * 1024):
                self.digest.update(chunk)
                if self.cap is not None:
                    chunk = chunk[:max(0, self.cap + 1 - self.length)]
                if chunk:
                    self.chunks.append(chunk)
                    self.length += len(chunk)
        except OSError as error:
            self.error = error


def _report_pipe():
    reader, writer = os.pipe()
    high_reader = None
    try:
        # Keep supplied resources away from the launcher's low control slots.
        # These are real pipe endpoints; the child writer is always >= 7.
        high_reader = fcntl.fcntl(reader, fcntl.F_DUPFD_CLOEXEC, 64)
        high_writer = fcntl.fcntl(writer, fcntl.F_DUPFD_CLOEXEC, 64)
    except OSError:
        if high_reader is not None:
            os.close(high_reader)
        raise
    finally:
        os.close(reader)
        os.close(writer)
    return high_reader, high_writer


def _launch(prepared, launcher, arena_bytes, result, harness_paths,
            report_cap):
    cap = report_cap
    window_bytes = getattr(launcher, "scratch_window_bytes", arena_bytes)
    if type(window_bytes) is not int or not 0 < window_bytes <= arena_bytes:
        _unsupported("launcher.scratch_window_bytes")
    arena = _arena_fd(arena_bytes)
    reader, report_fd, capture = None, None, None
    try:
        reader, report_fd = _report_pipe()
        capture = _PipeCapture(reader, None if cap in (None, 0) else cap)
        capture.thread.start()
        environment = dict(os.environ)
        environment.update(
            PINEFORGE_RUN_JSON_PROFILE="jailed", PF_SCRATCH_ARENA_FD="5",
            PF_SCRATCH_ARENA_BYTES=str(arena_bytes),
            PF_SCRATCH_WINDOW_BYTES=str(window_bytes), PF_SCRATCH_ARENA_JAILED="1",
            PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(prepared.temporary),
            TMP=str(prepared.temporary), TEMP=str(prepared.temporary))
        _recheck(prepared.hashes)
        launched = launcher(
            argv=[*prepared.argv, "--report-sink-fd", str(report_fd)],
            env=environment, cwd=prepared.cwd, arena_fd=arena, report_fd=report_fd)
        result.diagnostics[prepared.case_id] = launched
    finally:
        if report_fd is not None:
            _close_fd(report_fd)
        if capture is not None and capture.thread.ident is not None:
            capture.thread.join()
        if reader is not None:
            _close_fd(reader)
        _close_fd(arena)
    if capture.error is not None:
        raise capture.error
    captured = b"".join(capture.chunks)
    transported = getattr(launched, "report_bytes", None)
    if transported is not None and not isinstance(transported, bytes):
        _unsupported("launcher.report_bytes")
    if (captured and transported is not None
            and capture.digest.hexdigest() != hashlib.sha256(transported).hexdigest()):
        raise _Refusal("launcher report differs from the inherited stream")
    if transported is None:
        launched = _Namespace(**{**vars(launched), "report_bytes": captured})
        result.diagnostics[prepared.case_id] = launched
    return launched, _check_launch(launched, arena_bytes, harness_paths.run_json)


def _finalize(launched, prepared, launcher, result, harness_paths,
              report_cap):
    raw = launched.report_bytes
    if not isinstance(raw, bytes):
        _unsupported("launcher.report_bytes")
    # A zero or absent cap means the finalizer is bounded by the bytes this
    # launcher actually delivered. The stdout finalizer still requires a
    # positive bound, so an empty input uses the smallest valid bound and is
    # then refused by the sink grammar.
    cap = max(len(raw), 1) if report_cap in (None, 0) else report_cap
    finalized = subprocess.run(
        [sys.executable, "-B", str(harness_paths.finalizer), "--in-fd", "0", "--out", "-",
         "--max-bytes", str(cap), "--run-id", prepared.case_id, "--receipt-fd", "2"],
        input=raw, capture_output=True, check=False,
        env=dict(os.environ, PINEFORGE_RUN_JSON_PROFILE="jailed",
                 PYTHONDONTWRITEBYTECODE="1"))
    result.finalizer_outputs[prepared.case_id] = finalized
    try:
        receipt = _document(finalized.stderr)
    except (ValueError, UnicodeError):
        raise _Refusal("sink finalizer returned no complete receipt") from None
    result.receipts[prepared.case_id] = receipt
    if finalized.returncode != 0 or receipt.get("complete") is not True:
        raise _Refusal("sink finalizer refused the report", receipt, finalized.stderr)
    if receipt.get("sink_bytes") != len(raw) or finalized.stdout != raw:
        raise _Refusal("sink finalizer returned an inconsistent receipt", receipt,
                       finalized.stderr)
    # Decode only bytes delivered by an exited-successfully finalizer. The
    # shared parser supplies the wire schema; it is not an alternate gate.
    harness_dir = str(harness_paths.directory)
    if harness_dir not in sys.path:
        sys.path.insert(0, harness_dir)
    from pf_sink import SinkReader

    trades = []
    with SinkReader(stream=io.BytesIO(finalized.stdout)) as sink:
        for record in sink:
            if record.tag == "T":
                trades.append(record.as_dict())
        if (not sink.complete or sink.header["run_id"] != prepared.case_id
                or sink.sha256 != receipt.get("sha256")
                or any(receipt.get("count_" + tag.lower(), 0) != sink.counts.get(tag, 0)
                       for tag in "ETOVKXH")):
            raise _Refusal("sink finalizer returned an inconsistent receipt", receipt,
                           finalized.stderr)
    return trades


def _check_report(report, raw_trades, prepared):
    _recheck(prepared.hashes)
    runtime = report.get("applied_runtime", {})
    for option, key in (("--input-tf", "input_tf"), ("--script-tf", "script_tf"),
                        ("--chart-tz", "chart_tz")):
        if runtime.get(key) != prepared.argv[prepared.argv.index(option) + 1]:
            raise _Refusal(f"case report differs from prepared inputs: {prepared.case_id}")
    trade_start = (int(prepared.argv[prepared.argv.index("--trade-start-ms") + 1])
                   if "--trade-start-ms" in prepared.argv else None)
    if runtime.get("trade_start_ms") != trade_start:
        raise _Refusal(f"case report differs from prepared inputs: {prepared.case_id}")
    if (report.get("applied_inputs") != prepared.inputs
            or report.get("applied_overrides") != prepared.overrides
            or report.get("summary", {}).get("total_trades") != len(raw_trades)):
        raise _Refusal(f"case report differs from prepared inputs: {prepared.case_id}")
    actual = report.get("input", {})
    if actual.get("ohlcv") != str(prepared.chart):
        raise _Refusal(f"case report differs from prepared inputs: {prepared.case_id}")
    if prepared.selected is None:
        if (actual.get("bars"), actual.get("first_ts"), actual.get("last_ts")) != (
                len(prepared.stamps), prepared.stamps[0], prepared.stamps[-1]):
            raise _Refusal(f"case report differs from prepared inputs: {prepared.case_id}")
    else:
        window = report.get("report_window", {})
        requested = window.get("window", {})
        if (window.get("policy") != "selected-window/v1" or window.get("applied") is not True
                or (requested.get("start_ms"), requested.get("end_ms")) != prepared.report_window
                or window.get("counts", {}).get("supplied_input_bars") != len(prepared.stamps)
                or window.get("feed", {}).get("source_bytes_sha256") !=
                prepared.hashes[prepared.chart]):
            raise _Refusal(f"case report differs from prepared inputs: {prepared.case_id}")


def _convert(raw_trades, window):
    shaped = _report_trades_to_runstrategy_shape({"trades": raw_trades})
    for converted, original in zip(shaped, raw_trades):
        # The release helper handles the numeric/side conversion but predates
        # the four identity fields. Carry these from the verified T record.
        for name in ("entry_id", "exit_id", "exit_comment", "close_cause"):
            converted[name] = original[name]
    return _strategy._filter_trades_to_window(shaped, window)


def run_cases(cases, output_dir, *, launcher, arena_bytes, report_mode="fd",
              harness_dir=None, report_max_bytes=None, corpus_root=None,
              corpus_commit=None, staged_derived=None,
              require_read_only_corpus=True):
    """Run a nonempty population; atomically publish only a complete new set."""
    result = _Namespace(
        ok=False, failure_code=None, failure_document=None, failure_text=None,
        failure_document_bytes=None, publication_failure_text=None,
        csv_paths={}, trades={}, reports={}, receipts={}, preparations={},
        diagnostics={}, finalizer_outputs={}, corpus_commit=None)
    output = Path(output_dir).absolute()
    current_id = None
    try:
        _, staged_hashes = _validate_corpus_parameters(
            corpus_root, corpus_commit, staged_derived,
            require_read_only_corpus)
        result.corpus_commit = corpus_commit
        harness_paths = _harness_paths(harness_dir)
        report_cap = _effective_report_cap(launcher, report_max_bytes)
        population = [Path(case).resolve() for case in cases]
        if not population:
            raise _Refusal("refusing to run parity: zero cases")
        if report_mode != "fd":
            raise _Refusal(_SINK_REQUIRED)
        if type(arena_bytes) is not int or arena_bytes <= 0:
            _unsupported("arena_bytes")
        if len({case.name for case in population}) != len(population):
            _unsupported("case_id")
        for case in population:
            _text(case.name, "case_id", 512)
        metadata = [_case_metadata(case) for case in population]
        # Materialize once before any consumer can run over these feeds.
        if staged_hashes is None and any(_strategy._reads_derived_feed(
                case.params, case.directory,
                _path(case.directory, case.params.get("ohlcv_csv", _strategy.DEFAULT_OHLCV),
                      "ohlcv_csv"),
                None)
                for case in metadata):
            _strategy.ensure_derived()
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".pineforge-parity-", dir=output.parent) as work:
            work = Path(work)
            prepared_cases = []
            for index, case in enumerate(metadata):
                current_id = case.case_id
                directory = work / str(index)
                directory.mkdir()
                prepared = _prepare(
                    case, directory, harness_paths,
                    corpus_commit=corpus_commit)
                prepared_cases.append(prepared)
                result.preparations[current_id] = {
                    "manifest": prepared.manifest, "sha256": prepared.manifest_sha256}
            for prepared in prepared_cases:
                current_id = prepared.case_id
                launched, report = _launch(
                    prepared, launcher, arena_bytes, result, harness_paths,
                    report_cap)
                raw_trades = _finalize(
                    launched, prepared, launcher, result, harness_paths,
                    report_cap)
                _check_report(report, raw_trades, prepared)
                # Match the existing driver's retry when its first-bar gate
                # admitted a fill TradingView never had.
                if (prepared.signal_start is not None
                        and _strategy._entered_before_tv(
                            _convert(raw_trades, None), prepared.report_window[0]) is not None):
                    argv = list(prepared.argv)
                    argv[argv.index("--trade-start-ms") + 1] = str(prepared.signal_start)
                    retry = _Namespace(**{**vars(prepared), "argv": argv})
                    launched, report = _launch(
                        retry, launcher, arena_bytes, result, harness_paths,
                        report_cap)
                    raw_trades = _finalize(
                        launched, retry, launcher, result, harness_paths,
                        report_cap)
                    _check_report(report, raw_trades, retry)
                trades = _convert(raw_trades, prepared.report_window)
                if not trades:
                    raise _Refusal(f"refusing to grade case with zero trades: {current_id}")
                result.reports[current_id] = report
                result.trades[current_id] = trades
            for prepared in prepared_cases:
                _recheck(prepared.hashes)
        # Preparation cleanup must also succeed before a CSV is published.
        # Stage the whole population outside the graded directory, then
        # publish it with one directory rename after all cases have passed.
        with tempfile.TemporaryDirectory(
                prefix=".pineforge-parity-publish-", dir=output.parent) as staging:
            publication = Path(staging)
            for case_id, trades in result.trades.items():
                _strategy.write_engine_trades_csv(trades, publication / f"{case_id}.csv")
            if output.is_symlink() or (output.exists() and
                                       (not output.is_dir() or any(output.iterdir()))):
                raise _Refusal(
                    f"refusing to publish stale CSV after case refusal: {current_id}")
            # rename refuses a nonempty destination, including an output
            # populated after the preceding check; it never replaces a CSV.
            os.rename(publication, output)
        result.csv_paths = {case_id: output / f"{case_id}.csv" for case_id in result.trades}
        result.ok = True
    except _Refusal as error:
        result.failure_text = str(error)
        result.failure_document = error.document
        result.failure_document_bytes = error.raw_document
        result.failure_code = error.document.get("code") if error.document is not None else None
    except (OSError, ValueError, TypeError, KeyError) as error:
        result.failure_text = str(error)
    if not result.ok:
        result.csv_paths = {}
        result.trades = {}
        if current_id is not None and output.exists() and any(output.rglob("*.csv")):
            result.publication_failure_text = (
                f"refusing to publish stale CSV after case refusal: {current_id}")
    return result
