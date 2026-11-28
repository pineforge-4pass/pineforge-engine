#!/usr/bin/env python3
"""Run a strategy through the pineforge-release container and return its JSON
report. Shared helper for the docker-runner paths of the corpus harness
(scripts/run_strategy.py --runner docker) and the speed/throughput benchmark
drivers.

The image is a PURE RUNNER: it transpiles/compiles/runs and emits raw JSON.
ALL analysis (tv emit-window, trade trimming, CSV formatting, parity verdict,
speed median/ratio) stays host-side in the caller. This helper only:
  - hands over the pre-transpiled generated.cpp (NEVER re-transpiles .pine — the
    bundled codegen may differ from the one that produced the committed cpp),
  - hands over the OHLCV (caller pre-trims for ohlcv_start_ms),
  - maps already-resolved/normalized run knobs to the container's env contract
    (docker/entrypoint.sh), incl. the validation-parity knobs,
  - for raw corpus params, builds once and reads that module's settings receipt
    before binding input values and starting the engine command,
  - copies every input into the container (no host bind mount) as files only
    the image user, their owner, can read, whatever the host umask is,
  - starts the container, parses stdout JSON, surfaces errors.

The caller is responsible for normalization the image does NOT do (e.g. mapping
a 'VOLUME_WEIGHTED' magnifier distribution to its real geometric distribution +
the volume_weighted flag — pass dist already normalized).
"""
from __future__ import annotations

import ctypes
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

DEFAULT_IMAGE = os.environ.get(
    "PINEFORGE_RELEASE_IMAGE", "ghcr.io/pineforge-4pass/pineforge-release:latest")
# The reviewed engine-step/v1 producer from this tree: docker/entrypoint.sh.
# An older installed producer could ignore BUILD_ONLY and execute bars.
_BUILD_ONLY_ENTRYPOINT_SHA256 = "680160eae62aca428de3e4aab418aa59be8d9c46287034092f4dbb888c9517d6"
_CORPUS_DIRECTORY = PurePosixPath("/pf-corpus-request")
_STRATEGY_PATH = PurePosixPath("/in/strategy.cpp")
_OHLCV_PATH = PurePosixPath("/in/ohlcv.csv")
_SYMINFO_PATH = PurePosixPath("/in/syminfo.json")
# Fixed tar modes: never a host file's mode and never the host umask. Owner
# only: docker cp -a keeps the image user as owner; no group or other bit.
_STAGED_FILE_MODE = 0o400
_STAGED_DIRECTORY_MODE = 0o500
_IMAGE_ID = re.compile(r"sha256:[0-9a-f]{64}")


class ReleaseRunError(RuntimeError):
    def __init__(self, message, *, report=None):
        super().__init__(message)
        self.report = report


def _runner_syminfo_keys(syminfo: dict | None) -> tuple[str, ...]:
    if not isinstance(syminfo, dict):
        return ()
    runner = syminfo.get("_pineforge_runner_syminfo")
    if not isinstance(runner, dict):
        return ()
    keys = []
    if runner.get("type"):
        keys.append("type")
    strings = runner.get("strings")
    if isinstance(strings, dict):
        keys.extend(
            str(key) for key, value in strings.items()
            if value is not None and str(value) != "")
    return tuple(dict.fromkeys(keys))


def _module_input_names(shared_object: Path) -> frozenset[str]:
    """Query this compiled module's v1 receipt without settings or bar execution."""
    from run_strategy import declared_pine_input_names

    library = ctypes.CDLL(str(shared_object))
    required = (
        "strategy_settings_api_version", "strategy_create_checked",
        "strategy_get_effective_settings", "strategy_free",
        "strategy_set_input_checked", "strategy_set_override_checked",
    )
    missing = [name for name in required if not callable(getattr(library, name, None))]
    if missing:
        raise ReleaseRunError("NEEDS selected module settings API v1: " + ", ".join(missing))
    version = library.strategy_settings_api_version
    version.argtypes, version.restype = [], ctypes.c_uint32
    if version() != 1:
        raise ReleaseRunError("NEEDS selected module settings API v1")
    create, free = library.strategy_create_checked, library.strategy_free
    create.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_size_t,
    ]
    create.restype = ctypes.c_int
    free.argtypes, free.restype = [ctypes.c_void_p], None
    handle = ctypes.c_void_p()
    error = ctypes.create_string_buffer(1024)
    try:
        status = create(None, ctypes.byref(handle), error, len(error))
        if status != 0 or not handle.value:  # PF_SETTINGS_OK=0, pineforge.h
            message = error.value.decode("utf-8", "replace")
            raise ReleaseRunError(
                f"NEEDS selected module receipt handle: status {status}: {message}")
        return declared_pine_input_names(library, handle.value)
    finally:
        if handle.value:
            free(handle.value)


def _build_artifact(document, name, work):
    item = document["artifacts"][name]
    path = Path(item["path"])
    if (not path.is_absolute() or path.parent != work
            or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]):
        raise ReleaseRunError(f"NEEDS matching build artifact: {name}")
    return path


def _build_manifest(work: Path, environment: dict):
    """Validate the build receipt separately from loading/querying its module."""
    try:
        document = json.loads((work / "engine-step.json").read_bytes())
        if (document["schema"] != "pineforge-engine-step/v1"
                or document["work_dir"] != str(work)
                or document["inherits"] != {"environment": "unchanged", "descriptors": []}):
            raise ReleaseRunError("NEEDS the ordinary build-only engine-step/v1 contract")
        argv = document["argv"]
        if (not isinstance(argv, list) or not all(isinstance(value, str) for value in argv)
                or not argv or not Path(argv[0]).is_absolute()
                or not Path(document["cwd"]).is_absolute()):
            raise ReleaseRunError("NEEDS the build-only engine command and cwd")
        slots = {}
        for flag in ("--so", "--generated-cpp", "--inputs"):
            if argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv):
                raise ReleaseRunError(f"NEEDS one build-only argument: {flag}")
            slots[flag] = argv.index(flag) + 1
        shared_object = _build_artifact(document, "so", work)
        source = _build_artifact(document, "cpp", work)
        if (argv[slots["--so"]] != str(shared_object)
                or argv[slots["--generated-cpp"]] != str(source)
                or document["artifacts"]["cpp"]["sha256"]
                != environment.get("PINEFORGE_STRATEGY_CPP_SHA256")):
            raise ReleaseRunError("NEEDS the selected compiled module and vouched source")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ReleaseRunError(
            f"NEEDS a complete build-only engine-step manifest: {error}") from error
    return document, slots, shared_object


def _inspect_image(image: str) -> tuple[str, dict]:
    """Read the selected local image's config and bind it to an immutable ID."""
    inspected = subprocess.run(
        ["docker", "image", "inspect", image], capture_output=True, text=True, check=False)
    if inspected.returncode:
        raise ReleaseRunError(
            f"NEEDS selected image config for {image}: {inspected.stderr.strip()}")
    try:
        documents = json.loads(inspected.stdout)
        if not isinstance(documents, list) or len(documents) != 1:
            raise ValueError("expected one inspected image")
        document = documents[0]
        identity = document["Id"]
        if not isinstance(identity, str) or not _IMAGE_ID.fullmatch(identity):
            raise ValueError("image has no immutable sha256 ID")
        config = document["Config"]
        if not isinstance(config, dict):
            raise ValueError("invalid image Config")
    except (ValueError, KeyError, TypeError) as error:
        raise ReleaseRunError(f"NEEDS selected image config: {error}") from error
    return identity, config


def _image_command(image: str) -> tuple[str, list[str], dict]:
    """Recreate Docker's default command when overriding the entrypoint."""
    identity, config = _inspect_image(image)
    try:
        command = []
        for name in ("Entrypoint", "Cmd"):
            try:
                part = config[name]
            except KeyError:
                part = None  # Docker can omit an unset command field.
            if part is None:
                continue
            if (not isinstance(part, list)
                    or any(not isinstance(word, str) or "\0" in word for word in part)):
                raise ValueError(f"invalid image Config.{name}")
            command.extend(part)
        if not command or not command[0]:
            raise ValueError("image has no default command")
    except (ValueError, KeyError, TypeError) as error:
        raise ReleaseRunError(f"NEEDS selected image command: {error}") from error
    return identity, command, config


def _account_file(container: str, path: str) -> str:
    copied = subprocess.run(["docker", "cp", "-L", f"{container}:{path}", "-"],
                            capture_output=True, check=False)
    if copied.returncode:
        detail = copied.stderr.decode("utf-8", "replace").strip()
        if "could not find the file" in detail.lower():
            return ""
        raise ReleaseRunError(f"docker cp could not read image {path}: {detail}")
    try:
        with tarfile.open(fileobj=io.BytesIO(copied.stdout), mode="r:*") as archive:
            member = archive.next()
            if member is None or not member.isfile() or archive.next() is not None:
                raise ValueError("expected one regular file")
            with archive.extractfile(member) as stream:
                return stream.read().decode("utf-8", "surrogateescape")
    except (OSError, ValueError, tarfile.TarError) as error:
        raise ReleaseRunError(f"invalid docker cp archive for image {path}: {error}") from error


def _numeric_id(value: str) -> int | None:
    if not re.fullmatch(r"[+-]?[0-9]+", value):
        return None
    number = int(value)
    return number if number >= 0 else None


def _image_owner(container: str, config: dict) -> tuple[int, int]:
    """Resolve the process UID and primary GID using the container's accounts."""
    spec = config.get("User")
    if spec is None or spec == "":
        return 0, 0
    try:
        if not isinstance(spec, str):
            raise ValueError("expected a user string")
        # Docker treats an empty component as the default user or group.
        user, _, group = spec.partition(":")
        uid = _numeric_id(user or "0")
        gid = _numeric_id(group)
        if uid is None or not group:
            for line in _account_file(container, "/etc/passwd").splitlines():
                account = line.split(":")
                if len(account) < 4 or not account[0] or account[0].startswith("#"):
                    continue
                account_uid = _numeric_id(account[2])
                if (account[0] == user if uid is None else account_uid == uid):
                    uid = account_uid
                    if not group:
                        gid = _numeric_id(account[3])
                    break
        if uid is None:
            raise ValueError(f"no matching account in /etc/passwd for {user!r}")
        if group and gid is None:
            for line in _account_file(container, "/etc/group").splitlines():
                account = line.split(":")
                if len(account) >= 3 and account[0] == group:
                    gid = _numeric_id(account[2])
                    break
            if gid is None:
                raise ValueError(f"no matching entry in /etc/group for {group!r}")
        # A numeric UID without a passwd entry still runs under Docker (GID 0).
        return uid, 0 if gid is None else gid
    except (ValueError, ReleaseRunError) as error:
        raise ReleaseRunError(f"cannot resolve image Config.User {spec!r}: {error}") from error


def _require_installed_entrypoint(command: list[str]) -> None:
    # The config, not a guessed image path, identifies the installed script.
    # Refuse wrappers whose script cannot be bound by this contract.
    if (not isinstance(command, list) or not command
            or any(not isinstance(word, str) or "\0" in word for word in command)):
        raise ReleaseRunError("NEEDS the inspected image command")
    script = command[0]
    if Path(script).name in ("bash", "sh"):
        if len(command) < 2:
            raise ReleaseRunError("NEEDS an installed entrypoint script in the image command")
        script = command[1]
    installed = Path(script)
    if not installed.is_absolute():
        raise ReleaseRunError("NEEDS an absolute installed entrypoint path from image config")
    if hashlib.sha256(installed.read_bytes()).hexdigest() != _BUILD_ONLY_ENTRYPOINT_SHA256:
        raise ReleaseRunError(
            "NEEDS installed entrypoint bytes matching the reviewed build-only "
            f"engine-step/v1 source: {installed}")


def _run_corpus_build(command: list[str], params: dict, work: Path) -> int:
    """One image's own build-only command, query, then its unchanged engine argv."""
    from run_strategy import corpus_input_values

    # An older entrypoint may ignore BUILD_ONLY and start bars. Check its actual
    # installed bytes against the reviewed source pin BEFORE invoking it.
    _require_installed_entrypoint(command)
    environment = dict(os.environ, PINEFORGE_BUILD_ONLY="1", PINEFORGE_WORK_DIR=str(work))
    built = subprocess.run(command, env=environment,
                           stdin=subprocess.DEVNULL, check=False)
    if built.returncode != 0:
        return built.returncode
    document, slots, shared_object = _build_manifest(work, environment)
    # A dlopen/receipt/import failure is not a manifest failure.
    names = _module_input_names(shared_object)
    inputs = corpus_input_values(params, declared_input_names=names)
    argv = list(document["argv"])
    argv[slots["--inputs"]] = json.dumps(inputs)
    _build_artifact(document, "so", work)
    _build_artifact(document, "cpp", work)
    sys.stderr.write("[pineforge] running backtest ...\n")
    result = subprocess.run(argv, cwd=document["cwd"], env=environment,
                            stdin=subprocess.DEVNULL, check=False)
    if result.returncode:
        # entrypoint.sh's documented full-run mapping is ENGINE_FAILURE_STATUS=4.
        sys.stderr.write("[pineforge] backtest failed\n")
        return 4
    return 0


def _corpus_helper_files(params: dict) -> dict[PurePosixPath, bytes]:
    scripts = Path(__file__).resolve().parent
    sources = (
        scripts / "pf_release_run.py", scripts / "run_strategy.py",
        scripts / "derive_corpus_feeds.py", scripts / "parity_paths.py",
    )
    for source in sources:
        if not source.is_file():
            raise ReleaseRunError(f"NEEDS corpus pre-execution helper source: {source}")
    files = {_CORPUS_DIRECTORY / source.name: source.read_bytes() for source in sources}
    files[_CORPUS_DIRECTORY / "corpus-params.json"] = json.dumps(params).encode("utf-8")
    return files


def _staged_archive(files: dict[PurePosixPath, bytes], uid: int, gid: int) -> bytes:
    with io.BytesIO() as stream:
        with tarfile.open(fileobj=stream, mode="w") as archive:
            # Each directory, /in included, is an entry of its own, so the image
            # user owns it and can search it whatever the image gave it before.
            for parent in dict.fromkeys(path.parent for path in files):
                directory = tarfile.TarInfo(str(parent.relative_to("/")))
                directory.uid, directory.gid = uid, gid
                directory.mode = _STAGED_DIRECTORY_MODE
                directory.type = tarfile.DIRTYPE
                archive.addfile(directory)
            for path, data in files.items():
                member = tarfile.TarInfo(str(path.relative_to("/")))
                member.uid, member.gid, member.mode = uid, gid, _STAGED_FILE_MODE
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
        return stream.getvalue()


def _container_image_config(container: str) -> dict:
    inspected = subprocess.run(
        ["docker", "container", "inspect", "--format", "{{.Image}}", container],
        capture_output=True, text=True)
    if inspected.returncode:
        raise ReleaseRunError(
            f"NEEDS image ID for created container {container}: {inspected.stderr.strip()}")
    identity = inspected.stdout.strip()
    if not _IMAGE_ID.fullmatch(identity):
        raise ReleaseRunError("created container has no immutable sha256 image ID")
    _, config = _inspect_image(identity)
    return config


def _run_with_staged_inputs(cmd: list[str], config: dict | None,
                            files: dict[PurePosixPath, bytes]) -> subprocess.CompletedProcess:
    created = subprocess.run(cmd, capture_output=True, text=True)
    if created.returncode:
        raise ReleaseRunError(
            f"docker create failed (exit {created.returncode})\nstderr tail:\n{created.stderr[-2000:]}")
    container = created.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{64}", container):
        raise ReleaseRunError("docker create did not return a container ID")
    try:
        # A metadata-only run can pull the tag during create, like docker run.
        if config is None:
            config = _container_image_config(container)
        uid, gid = _image_owner(container, config)
        # Docker extracts as root; only -a keeps the archive's numeric owners,
        # and the owner-only modes let that owner alone read the entries.
        copied = subprocess.run(["docker", "cp", "-a", "-", f"{container}:/"],
                                input=_staged_archive(files, uid, gid), capture_output=True)
        if copied.returncode:
            detail = copied.stderr[-2000:].decode("utf-8", "replace")
            raise ReleaseRunError(
                f"docker cp failed (exit {copied.returncode})\nstderr tail:\n{detail}")
        proc = subprocess.run(["docker", "start", "-a", container], capture_output=True, text=True)
    finally:
        removed = subprocess.run(["docker", "rm", "-f", "-v", container],
                                 capture_output=True, text=True)
    if removed.returncode and proc.returncode == 0:
        raise ReleaseRunError(
            f"docker rm failed (exit {removed.returncode})\nstderr tail:\n{removed.stderr[-2000:]}")
    return proc


def run_release(
    generated_cpp: Path,
    ohlcv: Path,
    *,
    image: str = DEFAULT_IMAGE,
    inputs: dict | None = None,
    corpus_params: dict | None = None,
    overrides: dict | None = None,
    input_tf: str = "",
    script_tf: str = "",
    bar_magnifier: bool = False,
    magnifier_samples: int = 4,
    magnifier_dist: str = "endpoints",
    magnifier_volume_weighted: bool = False,
    trade_start_ms: int | None = None,
    chart_tz: str = "",
    syminfo: dict | None = None,
    strict_metadata: bool = False,
    metadata_warning_state: dict | None = None,
    bench: bool = False,
    warmup: int = 3,
    repeats: int = 20,
    network: bool = False,
) -> dict:
    """Run one strategy in the container; return the parsed JSON report dict.

    ``generated_cpp`` is copied into the container as /in/strategy.cpp (compile
    + run, no transpile), ``ohlcv`` as /in/ohlcv.csv and ``syminfo`` (a dict)
    as JSON. Nothing is bind-mounted from the host, so the host files' owner
    and mode never decide what the image user can read. Knob values must
    already be resolved by the caller (e.g. magnifier_dist lowercased + not
    'VOLUME_WEIGHTED').

    ``corpus_params`` requests metadata/input classification using the actual
    compiled module's pre-execution settings receipt. It is exclusive with
    already-resolved ``inputs``. Both phases use one container and one module.
    ``strict_metadata`` asks the image to refuse requested type/string metadata
    when its optional setter export is absent or its reader is too old.
    """
    if corpus_params is not None and (inputs is not None or not isinstance(corpus_params, dict)):
        raise ReleaseRunError("corpus_params must be an object, exclusive with resolved inputs")
    generated_cpp = Path(generated_cpp).resolve()
    ohlcv = Path(ohlcv).resolve()
    if not generated_cpp.exists():
        raise ReleaseRunError(f"generated.cpp not found: {generated_cpp}")
    if not ohlcv.exists():
        raise ReleaseRunError(f"ohlcv not found: {ohlcv}")

    cmd = ["docker", "create"]
    if not network:
        cmd += ["--network=none"]
    # A bind mount would show the image user the host file under the host's
    # owner and mode (0600 under umask 077). Copy the bytes in instead.
    source = generated_cpp.read_bytes()
    files = {_STRATEGY_PATH: source, _OHLCV_PATH: ohlcv.read_bytes()}

    env = {
        # The release harness generated this file itself; vouch for exactly these bytes.
        "PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(source).hexdigest(),
        "PINEFORGE_INPUTS": json.dumps(inputs or {}),
        "PINEFORGE_OVERRIDES": json.dumps(overrides or {}),
        "PINEFORGE_INPUT_TF": input_tf or "",
        "PINEFORGE_SCRIPT_TF": script_tf or "",
        "PINEFORGE_BAR_MAGNIFIER": "true" if bar_magnifier else "false",
        "PINEFORGE_MAGNIFIER_SAMPLES": str(int(magnifier_samples or 4)),
        "PINEFORGE_MAGNIFIER_DIST": (magnifier_dist or "endpoints").lower(),
    }
    if trade_start_ms is not None:
        env["PINEFORGE_TRADE_START_MS"] = str(int(trade_start_ms))
    if chart_tz:
        env["PINEFORGE_CHART_TZ"] = str(chart_tz)
    if magnifier_volume_weighted and bar_magnifier:
        env["PINEFORGE_MAGNIFIER_VOLUME_WEIGHTED"] = "1"
    if bench:
        env["PINEFORGE_BENCH"] = "1"
        env["PINEFORGE_WARMUP"] = str(int(warmup))
        env["PINEFORGE_REPEATS"] = str(int(repeats))

    if syminfo:
        env["PINEFORGE_SYMINFO"] = str(_SYMINFO_PATH)
    if strict_metadata:
        env["PINEFORGE_STRICT_METADATA"] = "1"

    for k, v in env.items():
        cmd += ["-e", f"{k}={v}"]

    config = None
    if syminfo:
        files[_SYMINFO_PATH] = json.dumps(syminfo).encode("utf-8")
    if corpus_params is not None:
        files.update(_corpus_helper_files(corpus_params))
        identity, command, config = _image_command(image)
        # Stage Python classification only. Inside the pinned image,
        # verify the installed script against the source pin before invoking
        # the exact Config.Entrypoint + Config.Cmd, with its ordinary cwd/env.
        cmd += ["--entrypoint", "python3", identity,
                str(_CORPUS_DIRECTORY / "pf_release_run.py"), json.dumps(command),
                str(_CORPUS_DIRECTORY / "corpus-params.json")]
    else:
        cmd += [image]
    proc = _run_with_staged_inputs(cmd, config, files)

    requested_keys = _runner_syminfo_keys(syminfo)
    reader_protocol = "pineforge: runner syminfo protocol=1"
    forwarded_warning = False
    for line in proc.stderr.splitlines():
        if line == reader_protocol:
            reader_protocol = ""
        elif (line.startswith("warning: --runner docker does not apply syminfo key(s):")
              and not forwarded_warning
              and (metadata_warning_state is None
                   or not metadata_warning_state.get("emitted"))):
            sys.stderr.write(line + "\n")
            sys.stderr.flush()
            forwarded_warning = True
            if metadata_warning_state is not None:
                metadata_warning_state["emitted"] = True

    if (proc.returncode == 0 and requested_keys and reader_protocol
            and not forwarded_warning):
        names = ", ".join(requested_keys)
        text = (
            "warning: --runner docker image reader does not apply syminfo key(s): "
            f"{names}; the results do not apply them "
            "(reader protocol runner-syminfo/v1 unavailable).")
        if strict_metadata:
            raise ReleaseRunError(
                "error: " + text + "; use --runner ctypes")
        if (metadata_warning_state is None
                or not metadata_warning_state.get("emitted")):
            sys.stderr.write(text + "\n")
            sys.stderr.flush()
            if metadata_warning_state is not None:
                metadata_warning_state["emitted"] = True

    if proc.returncode != 0:
        if strict_metadata and proc.stdout.strip():
            try:
                failure = json.loads(proc.stdout.strip().splitlines()[-1])
            except (TypeError, ValueError):
                failure = None
            if isinstance(failure, dict) and failure.get("error"):
                raise ReleaseRunError(
                    f"engine error: {failure['error']}", report=failure)
        if corpus_params is not None and proc.stdout.strip():
            try:
                failure = json.loads(proc.stdout.strip().splitlines()[-1])
            except ValueError:
                failure = None
            if isinstance(failure, dict) and failure.get("error"):
                raise ReleaseRunError(f"engine error: {failure['error']}", report=failure)
        raise ReleaseRunError(
            f"docker run failed (exit {proc.returncode}) for {generated_cpp.parent.name}\n"
            f"stderr tail:\n{proc.stderr[-2000:]}")
    out = proc.stdout.strip()
    if not out:
        raise ReleaseRunError(f"empty stdout from container for {generated_cpp.parent.name}")
    # The report is the last JSON line (engine logs go to stderr, but be defensive).
    try:
        report = json.loads(out.splitlines()[-1])
    except json.JSONDecodeError as e:
        raise ReleaseRunError(f"non-JSON container stdout: {e}\n{out[-1000:]}")
    if isinstance(report, dict) and report.get("error"):
        raise ReleaseRunError(f"engine error: {report['error']}", report=report)
    return report


def report_trades_to_runstrategy_shape(report: dict) -> list[dict]:
    """Map run_json.py trades[] -> the dict shape scripts/run_strategy.py's
    write_engine_trades_csv expects. Key diff: run_json emits ``side`` ('long'/
    'short'); the writer reads ``is_long``. max_drawdown stays POSITIVE (the
    writer negates it for the TV export). All other keys already match."""
    out = []
    for t in report.get("trades", []):
        out.append({
            "entry_time": int(t["entry_time"]),
            "exit_time": int(t["exit_time"]),
            "entry_price": float(t["entry_price"]),
            "exit_price": float(t["exit_price"]),
            "pnl": float(t["pnl"]),
            "pnl_pct": float(t["pnl_pct"]),
            "is_long": (t["side"] == "long"),
            "max_runup": float(t["max_runup"]),
            "max_drawdown": float(t["max_drawdown"]),
            "qty": float(t["qty"]),
            "commission": float(t.get("commission", 0.0)),
            "entry_bar_index": int(t.get("entry_bar_index", -1)),
            "exit_bar_index": int(t.get("exit_bar_index", -1)),
            "open_at_end": bool(t.get("open_at_end", False)),
            "entry_incarnation": int(t.get("entry_incarnation", 0) or 0),
        })
    return out


def _cause_text(error) -> str:
    parts = []
    seen = set()
    while error is not None and id(error) not in seen:
        seen.add(id(error))
        parts.append(f"{type(error).__name__}: {error}")
        error = error.__cause__
    return ": caused by: ".join(parts)


if __name__ == "__main__":
    try:
        if len(sys.argv) != 3:
            raise ReleaseRunError(
                "NEEDS inspected image command and corpus request path")
        command = json.loads(sys.argv[1])
        params = json.loads(Path(sys.argv[2]).read_bytes())
        if not isinstance(params, dict):
            raise ReleaseRunError("corpus request must be an object")
        with tempfile.TemporaryDirectory(prefix="pf-corpus-build-") as work:
            status = _run_corpus_build(command, params, Path(work).resolve())
    except (OSError, ValueError, RuntimeError, ImportError) as error:
        sys.stderr.write(f"pf_release_run: {_cause_text(error)}\n")
        status = 4
    sys.exit(status)
