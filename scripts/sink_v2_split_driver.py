"""Lab driver: serial oracle or --tee with concurrent A/C and owner publication.

Both sink arms use the current fd route and the same stage-A verifier. Timing
ends after publication/receipt. This is not a jail launcher; L6's uid/network/
fsize proof is a separate hosting proof. Temporary objects belong to this run.

Tee defaults to structure verification and the tool's 4 late threads.
--tee-late-verify full opts into whole-stream verification and inverse.
Serial and the transcoder CLI retain full verification. At 1M bars, a
one-strategy probe on four clean builds measured wall/v1-STDOUT ratios of
1.073x (structure/4 threads), 1.118x (structure/1) and 1.337x (full, about
1.34x). The 1.10 limit held at the default 4; a 1-core host misses it.
These are probe measurements, to be replaced by the gate box's numbers.
"""
from __future__ import annotations

import contextlib
import errno
import fcntl
import json
import os
from pathlib import Path
import resource
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import run_json

_new_report_sink = run_json._new_report_sink


class EncodeFailure(ValueError):
    def __init__(self, status):
        super().__init__(f"stage C exited {status}")
        self.status = status


def pop_option(argv, flag, default=None):
    if flag not in argv:
        return default
    index = argv.index(flag)
    if index + 1 >= len(argv) or argv.count(flag) != 1:
        raise ValueError("missing/repeated " + flag)
    value = argv[index + 1]
    del argv[index:index + 2]
    return value


def _release_pipe(write_fd, engine_fd, pipe_status, evidence=None):
    """Deliver EOF to the verifier: close our end, and the engine's duplicate only
    if run_json has not already (a closed number may have been reused)."""
    os.close(write_fd)
    if evidence is not None:
        evidence["driver_sink_fd_closed_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    try:
        now = os.fstat(engine_fd)
    except OSError as error:
        if evidence is not None:
            if error.errno == errno.EBADF:
                evidence.setdefault("engine_sink_fd_closed_observed_ns", time.clock_gettime_ns(time.CLOCK_MONOTONIC))
            else:
                evidence["engine_sink_fd_observation_error"] = str(error)
        return
    if (now.st_dev, now.st_ino) == (pipe_status.st_dev, pipe_status.st_ino):
        os.close(engine_fd)
        if evidence is not None:
            evidence["engine_sink_fd_closed_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            evidence["engine_sink_fd_close_actor"] = "driver fallback, not run_json"
            evidence.setdefault("engine_sink_fd_closed_observed_ns", time.clock_gettime_ns(time.CLOCK_MONOTONIC))
    elif evidence is not None:
        evidence.setdefault("engine_sink_fd_closed_observed_ns", time.clock_gettime_ns(time.CLOCK_MONOTONIC))


def _descriptor(receipt, version):
    """The receipt as the speed gate reads it: format version and per-type counts
    next to the receipt's own fields (stage A has no format key, stage C has none named version)."""
    counts = {tag: receipt["count_" + tag.lower()] for tag in "ETOVKXH"}
    return {**receipt, "version": version, "counts": counts}


def _wait4(child, started, role, evidence):
    """Reap exactly this child. Never substitute cumulative CHILDREN usage."""
    while True:
        try:
            _, status, usage = os.wait4(child.pid, 0)
            break
        except InterruptedError:
            continue
    finished = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    child.returncode = os.waitstatus_to_exitcode(status)
    evidence[role + "_child"] = {
        "pid": child.pid, "start_monotonic_ns": started, "reaped_monotonic_ns": finished,
        "wall_seconds": (finished - started) / 1e9, "exit_code": child.returncode,
        "cpu_user_seconds": usage.ru_utime, "cpu_sys_seconds": usage.ru_stime,
        "max_rss_kb": usage.ru_maxrss / 1024 if sys.platform == "darwin" else usage.ru_maxrss,
        "accounting": "os.wait4 for this pid; includes exec launcher/wrapper",
    }
    return child.returncode


def _tee_json(raw):
    """One strict compact document. Reject duplicates and JSON's NaN extension."""
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate control JSON member")
            result[key] = value
        return result
    def constant(_):
        raise ValueError("nonfinite control JSON")
    result = json.loads(raw, object_pairs_hook=pairs, parse_constant=constant)
    if type(result) is not dict:
        raise ValueError("control JSON must be an object")
    return result


def _tee_binding(a, c):
    if a.get("complete") is not True or c.get("complete") is not True or c.get("ready") is not True:
        raise ValueError("incomplete A receipt or C readiness")
    for left, right in [("sink_bytes", "input_bytes")] + [
            ("count_" + tag.lower(), "count_" + tag.lower()) for tag in "ETOVKXH"]:
        if (type(a.get(left)) is not int or type(c.get(right)) is not int
                or not 0 <= a[left] <= (1 << 53) - 1 or a[left] != c[right]):
            raise ValueError("A/C input byte/count disagreement: " + left)
    digest = a.get("sha256")
    if (type(digest) is not str or len(digest) != 64
            or any(ch not in "0123456789abcdef" for ch in digest) or digest != c.get("input_sha256")):
        raise ValueError("A/C input digest disagreement")


def _tee_peer_closed(raw):
    # run_json's own failure document, never an arbitrary stderr substring.
    try:
        failure = _tee_json(raw)
    except ValueError:
        return False
    args = failure.get("args")
    return (failure.get("code") == "report_sink_failed" and type(args) is dict
            and args.get("reason") == "sink_peer_closed")


def tee_transaction(runner_command, output, tool, relay, evidence, *, level=3,
                    max_bytes=(1 << 53) - 1, block_records=None, phase_log=False,
                    state_out=None, progress_out=None, runner_pass_fds=(), late_verify):
    """Owner only. runner_command receives the sole engine pipe writer.

    Every child is reaped with wait4 before evaluating commit. Only the owner
    drains runner stdout, with nonblocking reads and a hard 2 MiB memory cap.
    No process-wide fsize limit is installed in the engine, whose independent
    curve/spill files have their own admission.
    """
    output = Path(output).absolute()
    with contextlib.ExitStack() as retained_directories:
        parent_fd = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        retained_directories.callback(os.close, parent_fd)
        info = os.fstat(parent_fd)
        if info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ValueError("publication directory must be owned by this uid and not group/world writable")
        try:
            os.stat(output.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise ValueError("output already exists")
        committed = False

        @contextlib.contextmanager
        def publication_guard():
            nonlocal committed
            try:
                yield
            except BaseException:
                if committed:
                    try:
                        os.unlink(output.name, dir_fd=parent_fd)
                    except OSError as error:
                        evidence["publication_cleanup_error"] = str(error)
                    else:
                        committed = False
                        evidence["published"] = False
                raise

        # Keep rollback outside run-directory cleanup, with parent_fd still live.
        retained_directories.enter_context(publication_guard())
        temporary = retained_directories.enter_context(
            tempfile.TemporaryDirectory(prefix=".pf-tee-run-", dir=output.parent))
        directory = Path(temporary)
        os.chmod(directory, 0o700)
        run_dir_fd = os.open(directory.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent_fd)
        retained_directories.callback(os.close, run_dir_fd)
        staging_fd, staging_name = tempfile.mkstemp(prefix=".pf-transcode-v2-", dir=directory)
        staging = Path(staging_name)
        children, open_fds, captures = {}, {staging_fd}, {}
        summary_write = control_write = None
        abort_signal = None
        primary = None
        cancel_at = None
        term_at = None
        notified = False
        terminated_by_owner = set()
        report = None
        stdout_limit = 2 << 20
        runner_stdout = bytearray()
        evidence.update(v2_mode="tee", stage_a_mode="stdout", run_directory=str(directory),
                        engine_start_scope="runner child spawn; elapsed_seconds remains native run time",
                        engine_last_sink_write_ns=None,
                        runner_stdout={"transport": "owner-drained nonblocking pipe",
                                       "limit_bytes": stdout_limit, "captured_bytes": 0,
                                       "overflow": False, "eof": False})

        def snapshot():
            if state_out:
                Path(state_out).write_text(json.dumps(evidence, separators=(",", ":")) + "\n", encoding="ascii")

        def close(fd):
            if fd in open_fds:
                open_fds.remove(fd)
                os.close(fd)

        def pipe():
            pair = os.pipe()
            open_fds.update(pair)
            return pair

        def capture(name):
            value = (directory / name).open("w+b", buffering=0)
            captures[name] = value
            return value

        def read_capture(name, limit):
            value = captures[name]
            value.seek(0)
            raw = value.read(limit + 1)
            if len(raw) > limit:
                raise ValueError(name + " exceeds control/capture bound")
            return raw

        def spawn(role, command, *, inherited=(), stdout=None):
            started = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            child = subprocess.Popen(command, pass_fds=inherited, stdin=subprocess.DEVNULL,
                                     stdout=stdout, stderr=capture(role + ".stderr"),
                                     env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            children[role] = (child, started)
            evidence[role + "_child"] = {"pid": child.pid, "start_monotonic_ns": started}
            if role == "runner":
                evidence.update(engine_pid=child.pid, engine_start_monotonic_ns=started)
            snapshot()
            return child

        def reap(role, blocking=False):
            child, started = children[role]
            if child.returncode is not None:
                return False
            while True:
                try:
                    pid, status, usage = os.wait4(child.pid, 0 if blocking else os.WNOHANG)
                    break
                except InterruptedError:
                    continue
            if not pid:
                return False
            finished = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            child.returncode = os.waitstatus_to_exitcode(status)
            evidence[role + "_child"].update(
                reaped_monotonic_ns=finished, exit_code=child.returncode,
                wall_seconds=(finished - started) / 1e9, cpu_user_seconds=usage.ru_utime,
                cpu_sys_seconds=usage.ru_stime,
                max_rss_kb=usage.ru_maxrss / 1024 if sys.platform == "darwin" else usage.ru_maxrss,
                accounting="os.wait4 for this pid; HWM is not per-process memory evidence")
            if role == "runner":
                evidence["engine_finished_observed_ns"] = finished
            snapshot()
            return True

        def cancel(role, status, detail):
            nonlocal primary, cancel_at
            if primary is None:
                primary = (role, status, detail)
                cancel_at = time.monotonic()
                evidence["cancel"] = {"child": role, "status": status, "detail": detail}
                snapshot()

        def drain_stdout():
            while stdout_read in open_fds:
                remaining = stdout_limit - len(runner_stdout)
                try:
                    raw = os.read(stdout_read, min(65536, remaining + 1))
                except BlockingIOError:
                    break
                except InterruptedError:
                    continue
                if not raw:
                    evidence["runner_stdout"]["eof"] = True
                    close(stdout_read)
                    break
                if len(raw) > remaining:
                    # Close first: no drainer or regular capture remains, and
                    # the runner's next stdout write gets EPIPE.
                    close(stdout_read)
                    evidence["runner_stdout"]["overflow"] = True
                    cancel("owner", 5, "runner stdout exceeds 2 MiB bound")
                    break
                runner_stdout.extend(raw)
                evidence["runner_stdout"]["captured_bytes"] = len(runner_stdout)

        def interrupted(signum, _frame):
            nonlocal abort_signal
            abort_signal = signum

        def send(child, signum):
            # Popen.send_signal/terminate/kill may internally poll with waitpid,
            # stealing the wait4 receipt and its per-child resource accounting.
            try:
                os.kill(child.pid, signum)
            except ProcessLookupError:
                pass

        previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGTERM, signal.SIGINT)}
        try:
            engine_read, engine_write = pipe()
            # fd 5 is reserved for the engine arena. No second writer survives spawn.
            engine_fd = fcntl.fcntl(engine_write, fcntl.F_DUPFD_CLOEXEC, 10)
            open_fds.add(engine_fd)
            close(engine_write)
            a_read, a_write = pipe()
            c_read, c_write = pipe()
            summary_read, summary_write = pipe()
            control_read, control_write = pipe()
            stdout_read, stdout_write = pipe()
            os.set_blocking(summary_write, False)
            os.set_blocking(control_write, False)
            os.set_blocking(stdout_read, False)
            evidence["runner_stdout"]["pipe_capacity_bytes"] = (
                fcntl.fcntl(stdout_read, fcntl.F_GETPIPE_SZ) if sys.platform.startswith("linux") else None)
            a_receipt, c_receipt = capture("stage-a.json"), capture("stage-c.json")
            relay_receipt = capture("relay.json")
            with open(os.devnull, "wb") as null:
                spawn("stage_a", [
                    sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
                    "--in-fd", str(a_read), "--out", "-", "--max-bytes", str(max_bytes),
                    "--receipt-fd", str(a_receipt.fileno())],
                    inherited=(a_read, a_receipt.fileno()), stdout=null)
            close(a_read)
            c_command = [str(tool), "--in-fd", str(c_read), "--summary-fd", str(summary_read),
                         "--staging-fd", str(staging_fd), "--publish", "none",
                         "--spool-dir", str(directory), "--spool-max-bytes", str((1 << 53) - 1),
                         "--spool-max-files", "1", "--max-bytes", str((1 << 53) - 1),
                         "--level", str(level or 3)]
            if block_records:
                c_command += ["--block-records", block_records]
            if late_verify == "structure":
                c_command += ["--late-verify", "structure"]
            c_fds = (c_read, summary_read, staging_fd)
            if phase_log:
                phases = capture("transcoder-phases.jsonl")
                c_command += ["--phase-log", str(phases.fileno())]
                c_fds += (phases.fileno(),)
            # Same scrubbed C environment as the serial wrapper.
            c_started = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            child = subprocess.Popen(c_command, pass_fds=c_fds, stdin=subprocess.DEVNULL,
                                     stdout=c_receipt, stderr=capture("stage_c.stderr"),
                                     env={"PATH": os.defpath, "LANG": "C", "LC_ALL": "C", "TZ": "UTC"})
            children["stage_c"] = (child, c_started)
            evidence["stage_c_child"] = {"pid": child.pid, "start_monotonic_ns": c_started}
            snapshot()
            close(c_read); close(summary_read)
            relay_command = [str(relay), "--in-fd", str(engine_read), "--a-fd", str(a_write),
                             "--c-fd", str(c_write), "--status-fd", str(relay_receipt.fileno()),
                             "--control-fd", str(control_read)]
            relay_fds = (engine_read, a_write, c_write, relay_receipt.fileno(), control_read)
            if progress_out:
                progress = open(progress_out, "w+b", buffering=0)
                captures["relay-progress"] = progress
                relay_command += ["--progress-fd", str(progress.fileno())]
                relay_fds += (progress.fileno(),)
            spawn("relay", relay_command, inherited=relay_fds, stdout=subprocess.DEVNULL)
            for fd in (engine_read, a_write, c_write, control_read):
                close(fd)
            spawn("runner", runner_command(engine_fd), inherited=(engine_fd, *runner_pass_fds), stdout=stdout_write)
            close(stdout_write)
            close(engine_fd)
            snapshot()
            summary_raw, summary_at, summary_started = None, 0, False
            # Poll all children, including C while runner is blocked in a sink write.
            while any(child.returncode is None for child, _ in children.values()):
                for role in ("stage_a", "stage_c", "relay", "runner"):
                    reaped = reap(role)
                    if abort_signal:
                        cancel("owner", 128 + abort_signal, "owner cancelled")
                    if not reaped:
                        continue
                    child, _ = children[role]
                    if role in ("stage_a", "stage_c") and child.returncode:
                        if not notified and control_write in open_fds:
                            observed = evidence[role + "_child"]["reaped_monotonic_ns"]
                            message = f"{'A' if role == 'stage_a' else 'C'} {child.returncode} {observed}\n".encode("ascii")
                            try:
                                os.write(control_write, message)
                            except BrokenPipeError:
                                pass  # Relay already recorded its earlier EPIPE/EOF.
                            notified = True
                        cancel(role, child.returncode, "consumer exited")
                    elif child.returncode:
                        cancel(role, child.returncode, "child exited")
                drain_stdout()
                runner = children["runner"][0]
                if runner.returncode is not None and stdout_read not in open_fds and not summary_started:
                    summary_started = True
                    if runner.returncode == 0 and primary is None:
                        try:
                            summary_raw = runner_stdout
                            report = _tee_json(summary_raw)
                            if ("error" in report or report.get("ok") is False
                                    or type(report.get("summary")) is not dict
                                    or type(report.get("metrics")) is not dict):
                                raise ValueError("runner stdout is not compact success JSON")
                        except ValueError as error:
                            cancel("runner", 3, str(error))
                    # SUMMARY is never supplied after a failed runner.
                    if summary_raw is None or primary is not None:
                        close(summary_write)
                if primary is None and summary_raw is not None and summary_write in open_fds:
                    while summary_write in open_fds:
                        try:
                            sent = os.write(summary_write, summary_raw[summary_at:summary_at + 65536])
                            summary_at += sent
                            if summary_at == len(summary_raw):
                                close(summary_write)
                        except BlockingIOError:
                            break
                        except InterruptedError:
                            continue
                        except BrokenPipeError:
                            close(summary_write)
                            break  # C's actual wait4/receipt is authoritative.
                if abort_signal:
                    cancel("owner", 128 + abort_signal, "owner cancelled")
                for name, bound in (("stage-a.json", 4096), ("stage-c.json", 4096), ("relay.json", 4096)):
                    if os.fstat(captures[name].fileno()).st_size > bound:
                        cancel("owner", 5, name + " exceeds bound")
                if primary is not None:
                    close(summary_write)
                    # Only child-originated failures get a natural truncated-
                    # stream/runner receipt drain. Owner cancellation stops now.
                    if primary[0] == "owner" or time.monotonic() - cancel_at >= 2:
                        for fd in tuple(open_fds):
                            close(fd)
                        if term_at is None:
                            term_at = time.monotonic()
                            for live, _ in children.values():
                                if live.returncode is None:
                                    terminated_by_owner.add(live.pid)
                                    send(live, signal.SIGCONT)
                                    send(live, signal.SIGTERM)
                        elif time.monotonic() - term_at >= 1:
                            for live, _ in children.values():
                                if live.returncode is None:
                                    send(live, signal.SIGKILL)
                select.select([], [], [], 0.01)

            # No publish decision precedes this point: all four are reaped.
            for role in ("stage_a", "stage_c", "relay"):
                try:
                    evidence[role + "_receipt"] = _tee_json(read_capture(
                        {"stage_a": "stage-a.json", "stage_c": "stage-c.json", "relay": "relay.json"}[role], 4096))
                except ValueError as error:
                    cancel(role, 4, "missing/invalid receipt: " + str(error))
            runner_code = children["runner"][0].returncode
            relay_status = evidence.get("relay_receipt", {})
            first = relay_status.get("first_failure", {})
            runner_raw = runner_stdout
            if abort_signal:
                cancel("owner", 128 + abort_signal, "owner cancelled")
            # Independent engine failures win. Only a later peer-closed failure
            # yields to the relay's FIRST consumer failure.
            first_role = first.get("child")
            first_time = first.get("monotonic_ns", 0)
            engine_epipe_after = (first_time and first_time < evidence["runner_child"]["reaped_monotonic_ns"]
                                  and _tee_peer_closed(runner_raw))
            evidence["runner_peer_closed_after_consumer"] = bool(engine_epipe_after)
            owner_primary = primary is not None and primary[0] == "owner"
            if (not owner_primary and first_role in ("stage_a", "stage_c") and first_time
                    and (runner_code == 0 or engine_epipe_after)):
                consumer_code = children[first_role][0].returncode
                primary = (first_role, consumer_code or 5,
                           "first consumer failure before engine EPIPE" if engine_epipe_after else "first consumer failure")
            elif not owner_primary and runner_code and children["runner"][0].pid not in terminated_by_owner:
                primary = ("runner", runner_code, "engine failure")
            if primary is not None:
                role, status, detail = primary
                evidence["primary_failure"] = {"child": role, "status": status, "detail": detail}
                for name in ("runner.stderr", "stage_a.stderr", "stage_c.stderr", "relay.stderr"):
                    captures[name].seek(0)
                    sys.stderr.buffer.write(captures[name].read(65536))
                if role == "runner":
                    try:
                        failure = _tee_json(runner_raw)
                    except ValueError:
                        failure = {}
                else:
                    failure = dict(evidence.get(role + "_receipt", {}))
                if role == "owner" and evidence["runner_stdout"]["overflow"]:
                    failure.update(code="report_sink_failed", args={"reason": "v2_resource"})
                failure.update(complete=False, child=role, status=status, detail=detail)
                print(json.dumps(failure, separators=(",", ":")))
                return status if status > 0 else 128 - status, None
            a, c = evidence["stage_a_receipt"], evidence["stage_c_receipt"]
            try:
                if abort_signal:
                    raise ValueError("owner cancelled before commit")
                _tee_binding(a, c)
                if c.get("late_verify", "full") != late_verify:
                    raise ValueError("C receipt disagrees with requested late verification")
                if (relay_status.get("complete") is not True
                        or any(relay_status.get(key) != c["input_bytes"]
                               for key in ("input_bytes", "a_bytes", "c_bytes"))):
                    raise ValueError("relay did not deliver the same complete stream")
            except (ValueError, KeyError) as error:
                evidence["primary_failure"] = {"child": "owner", "status": 4, "detail": str(error)}
                print(json.dumps({"complete": False, "child": "owner", "status": 4,
                                  "detail": str(error)}, separators=(",", ":")))
                return 4, None
            close(staging_fd)
            # Exactly one no-replace publication. EEXIST is a failure.
            os.link(staging.name, output.name, src_dir_fd=run_dir_fd, dst_dir_fd=parent_fd)
            committed = True
            os.unlink(staging.name, dir_fd=run_dir_fd)
            evidence["commit_monotonic_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            receipt = {key: value for key, value in c.items() if key not in ("ready", "input_sha256")}
            runner_usage, c_usage = evidence["runner_child"], evidence["stage_c_child"]
            timing = {
                # Legacy gate columns: runner user CPU and C user+system CPU.
                "engine_thread_cpu_seconds": runner_usage["cpu_user_seconds"],
                "writer_thread_cpu_seconds": c_usage["cpu_user_seconds"] + c_usage["cpu_sys_seconds"],
                "stage_a_cpu_seconds": sum(evidence["stage_a_child"][k] for k in ("cpu_user_seconds", "cpu_sys_seconds")),
                "relay_cpu_seconds": sum(evidence["relay_child"][k] for k in ("cpu_user_seconds", "cpu_sys_seconds")),
                "finalization_seconds": (evidence["commit_monotonic_ns"] - evidence["engine_finished_observed_ns"]) / 1e9,
                "stage_a_wall_seconds": evidence["stage_a_child"]["wall_seconds"],
                "v2_mode": "tee",
            }
            if phase_log:
                _collect_phases(directory / "transcoder-phases.jsonl", evidence)
                timing.update(evidence)
            report["sink_stage_c_timing"] = timing
            report["report_sink"] = _descriptor(receipt, 2)
            snapshot()
            return 0, report
        except (OSError, ValueError) as error:
            if committed:
                raise  # publication_guard rolls back every exception class.
            evidence["primary_failure"] = {"child": "owner", "status": 5, "detail": str(error)}
            print(json.dumps({"complete": False, "child": "owner", "status": 5,
                              "detail": str(error)}, separators=(",", ":")))
            return 5, None
        finally:
            for fd in tuple(open_fds):
                close(fd)
            for live, _ in children.values():
                if live.returncode is None:
                    send(live, signal.SIGCONT)
                    send(live, signal.SIGTERM)
            deadline = time.monotonic() + 1
            while any(live.returncode is None for live, _ in children.values()) and time.monotonic() < deadline:
                for role in children:
                    reap(role)
                select.select([], [], [], 0.01)
            for role, (live, _) in children.items():
                if live.returncode is None:
                    send(live, signal.SIGKILL)
                    reap(role, blocking=True)
            if phase_log and "transcoder-phases.jsonl" in captures and "transcoder_phases" not in evidence:
                _collect_phases(directory / "transcoder-phases.jsonl", evidence)
            for value in captures.values():
                value.close()
            try:
                os.unlink(staging.name, dir_fd=run_dir_fd)
            except FileNotFoundError:
                pass
            evidence["published"] = committed
            snapshot()
            for sig, handler in previous.items():
                signal.signal(sig, handler)


def _run_accounted(command, role, evidence, *, pass_fds=(), stdin=None, stdout_fd=None):
    if not hasattr(os, "wait4"):
        raise ValueError("split driver measurements require os.wait4; no cumulative fallback")
    with tempfile.TemporaryFile() as capture, tempfile.TemporaryFile() as diagnostic:
        started = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        child = subprocess.Popen(command, stdin=stdin, stdout=capture if stdout_fd is None else stdout_fd,
                                 stderr=diagnostic, pass_fds=pass_fds)
        try:
            status = _wait4(child, started, role, evidence)
        except BaseException:
            child.kill()
            _wait4(child, started, role, evidence)
            raise
        capture.seek(0)
        diagnostic.seek(0)
        return subprocess.CompletedProcess(command, status, capture.read(), diagnostic.read())


def _stage_a(source, output, directory, evidence, *, mode="file", stdout_fd=None):
    """Standalone A probe, also used before standalone C. No engine timing."""
    with contextlib.ExitStack() as stack:
        stream = stack.enter_context(Path(source).open("rb"))
        receipt = stack.enter_context(tempfile.TemporaryFile())
        command = [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
                   "--in-fd", "0", "--out", "-" if mode == "stdout" else str(output),
                   "--spool-dir", str(directory), "--max-bytes", str(max(1, os.fstat(stream.fileno()).st_size))]
        if mode == "stdout":
            if stdout_fd is None:
                stdout_fd = stack.enter_context(open(os.devnull, "wb")).fileno()
            command += ["--receipt-fd", str(receipt.fileno())]
        child = _run_accounted(command, "stage_a", evidence, stdin=stream,
                               pass_fds=(receipt.fileno(),) if mode == "stdout" else (),
                               stdout_fd=stdout_fd if mode == "stdout" else None)
        if mode == "stdout":
            receipt.seek(0)
            child.stdout = receipt.read()
        evidence["stage_a_mode"] = mode
        evidence["stage_a_standalone_seconds"] = evidence["stage_a_child"]["wall_seconds"]
        evidence["stage_a_input_bytes"] = os.fstat(stream.fileno()).st_size
        return child


def _collect_phases(path, evidence):
    rows = []
    try:
        with path.open("rb") as source:
            raw = source.read(32769)
        if len(raw) > 32768:
            raise ValueError("transcoder phase log exceeds 32 KiB bound")
        for line in raw.splitlines():
            if len(rows) == 32:
                raise ValueError("transcoder phase log exceeds 32-event bound")
            rows.append(json.loads(line))
    except (OSError, ValueError) as error:
        # Keep reached phases and the primary child result even after a partial
        # diagnostic write. Missing events/error remain visible to the reviewer.
        evidence["transcoder_phase_log_error"] = str(error)
    evidence["transcoder_phases"] = rows


def encode(source, summary, output, directory, tool, *, level=3, receipt=None, block_records=None,
           evidence=None, phase_log=False):
    """The same verified-only wrapper used by the vector generator."""
    source, summary, output, directory = map(Path, (source, summary, output, directory))
    evidence = {} if evidence is None else evidence
    if receipt is None:
        verified = directory / "verified.v1"
        child = _stage_a(source, verified, directory, evidence)
        if child.returncode:
            raise ValueError(child.stderr.decode(errors="replace"))
        receipt = directory / "stage-a.json"
        receipt.write_bytes(child.stdout)
        source = verified
    command = [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_encode_v2.py"),
               "--transcoder", str(tool), "--verified-input", str(source),
               "--stage-a-receipt", str(receipt), "--summary", str(summary),
               "--out", str(output), "--spool-dir", str(directory),
               "--spool-max-bytes", str((1 << 53) - 1), "--spool-max-files", "1",
               "--level", str(level or 3)]
    if block_records:
        command += ["--block-records", block_records]
    with contextlib.ExitStack() as stack:
        pass_fds = ()
        if phase_log:
            phases = directory / "transcoder-phases.jsonl"
            phase = stack.enter_context(phases.open("wb"))
            # The frozen wrapper accepts only a tool path. This opt-in, private
            # exec shim preserves its receipt checks and four-key environment.
            launcher = directory / "phase-transcoder"
            launcher.write_text("#!/bin/sh\nexec " + shlex.quote(str(Path(tool).resolve()))
                                + " --phase-log " + str(phase.fileno()) + ' "$@"\n', encoding="utf-8")
            launcher.chmod(0o700)
            command[command.index("--transcoder") + 1] = str(launcher)
            pass_fds = (phase.fileno(),)
        try:
            child = _run_accounted(command, "stage_c", evidence, pass_fds=pass_fds)
        finally:
            if phase_log:
                _collect_phases(phases, evidence)
    evidence["stage_c_standalone_seconds"] = evidence["stage_c_child"]["wall_seconds"]
    if child.returncode:
        sys.stderr.buffer.write(child.stderr)
        sys.stderr.buffer.write(child.stdout)
        raise EncodeFailure(child.returncode)
    result = json.loads(child.stdout)
    if not result.get("complete"):
        raise ValueError("stage C did not publish a complete receipt")
    return result


def _main(argv, evidence, phase_log):
    if argv in (["--help"], ["-h"]):
        print(__doc__)
        print("Usage: from-v1 SOURCE OUTPUT --summary-json JSON [--tee] "
              "[--tee-late-verify full|structure]\n"
              "Or: --so MODULE --ohlcv CSV --report-sink OUTPUT --report-sink-format 2 [--tee]")
        return 0
    tool = os.environ.get("PF_SINK_V2_TRANSCODER")
    tee = "--tee" in argv
    if tee:
        if argv.count("--tee") != 1:
            raise ValueError("repeated --tee")
        argv.remove("--tee")
    state_out = pop_option(argv, "--tee-state-out")
    progress_out = pop_option(argv, "--tee-progress-out")
    late_verify = pop_option(argv, "--tee-late-verify")
    if late_verify is not None and (not tee or late_verify not in ("full", "structure")):
        raise ValueError("--tee-late-verify requires --tee and full or structure")
    late_verify = late_verify or ("structure" if tee else "full")
    if (state_out or progress_out) and not tee:
        raise ValueError("tee observations require --tee")
    relay = Path(tool).with_name("pf_sink_tee") if tool else None
    if argv and argv[0] == "to-v1":
        if tee:
            raise ValueError("--tee selects v2 generation, not to-v1")
        import pf_sink2
        return pf_sink2.main(argv)
    if argv and argv[0] in ("from-v1", "stage-a"):
        import argparse
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("command", choices=("from-v1", "stage-a"))
        parser.add_argument("source", type=Path)
        parser.add_argument("output", type=Path)
        parser.add_argument("--summary-json", type=Path)
        parser.add_argument("--level", type=int, default=3)
        parser.add_argument("--block-records")
        parser.add_argument("--v1-a-mode", choices=("file", "stdout"), default="file")
        parser.add_argument("--v1-a-stdout-fd", type=int)
        parser.epilog = ("--tee selects concurrent A/C with structure verification and 4 late threads; "
                         "--tee-late-verify full|structure requires --tee (default structure).")
        args = parser.parse_args(argv)
        if args.command == "stage-a":
            if tee:
                raise ValueError("--tee requires from-v1 or a v2 backtest")
            with tempfile.TemporaryDirectory(prefix="sink-v2-stage-a-") as temporary:
                child = _stage_a(args.source, args.output, temporary, evidence,
                                 mode=args.v1_a_mode, stdout_fd=args.v1_a_stdout_fd)
            sys.stdout.buffer.write(child.stdout)
            sys.stderr.buffer.write(child.stderr)
            return child.returncode
        if args.summary_json is None:
            raise ValueError("from-v1 requires --summary-json")
        if not tool:
            raise ValueError("PF_SINK_V2_TRANSCODER must bind this build's real encoder")
        if tee:
            status, report = tee_transaction(
                lambda fd: [str(relay), "--replay", "--input", str(args.source.resolve()),
                            "--summary", str(args.summary_json.resolve()), "--sink-fd", str(fd)],
                args.output, tool, relay, evidence, level=args.level, block_records=args.block_records,
                phase_log=phase_log, state_out=state_out, progress_out=progress_out,
                late_verify=late_verify)
            if status == 0:
                result = {key: value for key, value in evidence["stage_c_receipt"].items()
                          if key not in ("ready", "input_sha256")}
                try:
                    print(json.dumps(result, separators=(",", ":"), allow_nan=False), flush=True)
                except BaseException:
                    args.output.unlink()
                    raise
            return status
        with tempfile.TemporaryDirectory(prefix="sink-v2-codec-") as temporary:
            try:
                result = encode(args.source, args.summary_json, args.output, temporary, tool,
                                level=args.level, block_records=args.block_records,
                                evidence=evidence, phase_log=phase_log)
            except EncodeFailure as error:
                return error.status
        print(json.dumps(result, separators=(",", ":")))
        return 0
    output = pop_option(argv, "--report-sink")
    format_ = pop_option(argv, "--report-sink-format", "1")
    level = int(pop_option(argv, "--report-sink-level", "3"))
    a_mode = pop_option(argv, "--v1-a-mode", "file")
    a_stdout_fd = pop_option(argv, "--v1-a-stdout-fd")
    # Safe-integer whole-stream ceiling for this lab route, not a gate limit.
    a_max_bytes = int(pop_option(argv, "--v1-a-max-bytes", str((1 << 53) - 1)))
    if a_mode not in ("file", "stdout") or a_max_bytes <= 0:
        raise ValueError("invalid stage-A mode or nonpositive stdout stream bound")
    if output is None:
        if tee:
            raise ValueError("--tee requires a v2 output")
        # The harness's off arm replaces this module's factory with None.
        run_json._new_report_sink = _new_report_sink
        return run_json.main(argv)
    if format_ not in ("1", "2") or (format_ == "2" and not tool):
        raise ValueError("split driver needs format 1/2 and a bound real transcoder")
    if format_ == "2" and a_mode != "file":
        raise ValueError("--v1-a-mode applies only to the v1 arm")
    if tee:
        if format_ != "2":
            raise ValueError("--tee applies only to v2")
        # Retain an explicitly supplied arena across the new exec boundary.
        # Resolve it BEFORE opening any private files/pipes, so an absent fd 5
        # cannot accidentally become one of our new descriptors. Invalid arena
        # settings still reach run_json's existing typed admission refusal.
        runner_fds = ()
        supplied = os.environ.get("PF_SCRATCH_ARENA_FD", "")
        if supplied.isascii() and supplied.isdecimal() and len(supplied) <= 10:
            arena_fd = int(supplied)
            if 3 <= arena_fd <= (1 << 31) - 1:
                try:
                    os.fstat(arena_fd)
                    runner_fds = (arena_fd,)
                except OSError:
                    pass
        status, report = tee_transaction(
            lambda fd: [sys.executable, "-I", "-B", str(ROOT / "docker/run_json.py"),
                        *argv, "--report-sink-fd", str(fd)],
            output, tool, relay, evidence, level=level, max_bytes=a_max_bytes,
            phase_log=phase_log, state_out=state_out, progress_out=progress_out,
            runner_pass_fds=runner_fds, late_verify=late_verify)
        if status == 0:
            try:
                print(json.dumps(report, separators=(",", ":"), allow_nan=False), flush=True)
            except BaseException:
                Path(output).unlink()
                raise
        return status
    output = Path(output).resolve()
    with tempfile.TemporaryDirectory(prefix="sink-v2-split-") as temporary, contextlib.ExitStack() as stack:
        directory = Path(temporary)
        staged = output if format_ == "1" else directory / "verified.v1"
        summary, receipt = directory / "stdout.json", directory / "stage-a.json"
        read_fd, write_fd = os.pipe()
        # run_json takes ownership of the descriptor it is given and closes it, so
        # it gets a duplicate; this driver closes its own end separately. Number 5 is
        # reserved for the arena (run_json refuses it), so the duplicate starts at 10.
        engine_fd = fcntl.fcntl(write_fd, fcntl.F_DUPFD_CLOEXEC, 10)
        pipe_status = os.fstat(write_fd)
        sealed_file = stack.enter_context(tempfile.TemporaryFile())
        diagnostic_file = stack.enter_context(tempfile.TemporaryFile())
        command = [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
                   "--in-fd", "0" if a_mode == "stdout" else str(read_fd),
                   "--out", "-" if a_mode == "stdout" else str(staged), "--spool-dir", str(directory)]
        if a_mode == "stdout":
            null_fd = (int(a_stdout_fd) if a_stdout_fd is not None
                       else stack.enter_context(open(os.devnull, "wb")).fileno())
            command += ["--max-bytes", str(a_max_bytes), "--receipt-fd", str(sealed_file.fileno())]
        a_start = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        evidence["stage_a_mode"] = a_mode
        verifier = subprocess.Popen(command, stdin=read_fd if a_mode == "stdout" else None,
                                    pass_fds=(read_fd, sealed_file.fileno()),
                                    stdout=null_fd if a_mode == "stdout" else sealed_file,
                                    stderr=diagnostic_file)
        os.close(read_fd)
        stop = threading.Event()
        observer = None
        if phase_log:
            def observe_close():
                while not stop.is_set():
                    try:
                        now = os.fstat(engine_fd)
                        closed = (now.st_dev, now.st_ino) != (pipe_status.st_dev, pipe_status.st_ino)
                    except OSError as error:
                        if error.errno != errno.EBADF:
                            evidence["engine_sink_fd_observation_error"] = str(error)
                            return
                        closed = True
                    if closed:
                        evidence.setdefault("engine_sink_fd_closed_observed_ns", time.clock_gettime_ns(time.CLOCK_MONOTONIC))
                        return
                    stop.wait(0.001)
            evidence.update(engine_last_sink_write_ns=None, engine_sink_fd_closed_ns=None,
                            fd_close_observation_interval_seconds=0.001,
                            fd_close_scope="first fstat observation, not exact close; last write invisible")
            observer = threading.Thread(target=observe_close, daemon=True)
            observer.start()
        start_cpu = resource.getrusage(resource.RUSAGE_SELF).ru_utime
        try:
            with summary.open("w", encoding="ascii") as capture, contextlib.redirect_stdout(capture):
                if phase_log:
                    evidence["engine_pid"] = os.getpid()
                    evidence["engine_start_monotonic_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                status = run_json.main([*argv, "--report-sink-fd", str(engine_fd)])
                if phase_log:
                    evidence["engine_finished_observed_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        except BaseException:
            _release_pipe(write_fd, engine_fd, pipe_status, evidence if phase_log else None)
            _wait4(verifier, a_start, "stage_a", evidence)
            raise
        else:
            _release_pipe(write_fd, engine_fd, pipe_status, evidence if phase_log else None)
        finally:
            if observer is not None:
                stop.set()
                observer.join()
        engine_cpu = resource.getrusage(resource.RUSAGE_SELF).ru_utime - start_cpu
        _wait4(verifier, a_start, "stage_a", evidence)
        sealed_file.seek(0)
        diagnostic_file.seek(0)
        sealed, diagnostic = sealed_file.read(), diagnostic_file.read()
        evidence.update(stage_a_mode=a_mode, stage_a_wall_seconds=evidence["stage_a_child"]["wall_seconds"],
                        stage_a_standalone_seconds=None)
        if status or verifier.returncode:
            sys.stderr.buffer.write(diagnostic)
            return status or verifier.returncode
        if json.loads(sealed).get("complete") is not True:
            raise ValueError("stage A did not publish a complete receipt")
        receipt.write_bytes(sealed)
        with summary.open("rb") as captured:
            small = captured.read((2 << 20) + 1)
        if len(small) > 2 << 20:
            raise ValueError("fd route did not emit bounded compact stdout")
        report = json.loads(small)
        if format_ == "2":
            begin = time.monotonic()
            cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
            try:
                sink = encode(staged, summary, output, directory, tool, level=level, receipt=receipt,
                              evidence=evidence, phase_log=phase_log)
            except EncodeFailure as error:
                return error.status
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            report["sink_stage_c_timing"] = {
                "finalization_seconds": time.monotonic() - begin,
                "engine_thread_cpu_seconds": engine_cpu,
                # Inline transcoder process CPU, including its re-verification.
                "writer_thread_cpu_seconds": after.ru_utime + after.ru_stime - cpu.ru_utime - cpu.ru_stime,
            }
            if phase_log:
                report["sink_stage_c_timing"].update(evidence)
            # Metadata was captured before this lab-only timing annotation.
            # Do not pretend the annotation is embedded in the binary SUMMARY.
            report["report_sink"] = _descriptor(sink, 2)
        else:
            sink = json.loads(sealed)
            report["report_sink"] = _descriptor(sink, 1)
            if phase_log:
                report["sink_stage_c_timing"] = evidence
        print(json.dumps(report, separators=(",", ":"), allow_nan=False))
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    phase_out = pop_option(argv, "--phase-log-out")
    if not hasattr(os, "wait4"):
        raise ValueError("split driver measurements require os.wait4; no cumulative fallback")
    evidence = {"clock": "CLOCK_MONOTONIC", "driver_pid": os.getpid(), "engine_pid": None,
                "engine_start_scope": "in-process run_json.main entry; elapsed_seconds remains native run time"}
    try:
        return _main(argv, evidence, phase_out is not None)
    finally:
        if phase_out is not None:
            evidence["driver_exit_observed_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            Path(phase_out).write_text(json.dumps(evidence, separators=(",", ":"), allow_nan=False) + "\n",
                                       encoding="ascii")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"sink-v2 split driver: {error}", file=sys.stderr)
        raise SystemExit(1)
