#!/usr/bin/env python3
"""Hosted native-x86 diagnostic, deliberately separate from release admission.

Only reports are uploaded. Downloads, sources, build contexts, installed trees,
archives, and stopped containers remain available on the ephemeral runner.
No shell, cleanup, credential discovery, backtests, or expected-result fixtures.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import tarfile
import time
import traceback
import types


RECIPE = Path("docker/sdk-glibc235")
DRIVER = RECIPE / "diagnostic.py"
EXAMPLE = Path("examples/native/native_market_strategy.cpp")
KEYRING = Path("/usr/share/keyrings/ubuntu-archive-keyring.gpg")
SNAPSHOT = "https://snapshot.ubuntu.com/ubuntu/20261101T000000Z"
CA_NAME = "ca-certificates_20260816ubuntu0.22.04.1_all.deb"
CA_VERSION = "20260816ubuntu0.22.04.1"
CA_RELATIVE = "pool/main/c/ca-certificates/" + CA_NAME
CA_SHA = "29ca037f49f7a4b41d67ad72946c2e541f3ae52d8f6ab1495d2af09aa391144b"
CA_SIZE = 139540
PACKAGES_SHA = "759dae046fad315078b955b817b17e0713d60879c66a63df57a12b0f31a2de13"
PACKAGES_SIZE = 3938856
OLD_NAME = "pineforge-v1.10.0-linux-x86_64.tar.gz"
OLD_URL = "https://github.com/pineforge-4pass/pineforge-engine/releases/download/v1.10.0/"
OLD_SHA = "cb412fbae9e8ba515de41dc31026c77d6c5e309693247bee26bb93c67d3c3543"
OLD_SIZE = 8403177
SIDECAR_SHA = "ca260267fc54e85684f0836aad3b8fc97458e70519eff30e18dd002195ea5bb0"
RUNTIMES = {
    "ubuntu2204": "docker.io/library/ubuntu:22.04@sha256:"
                  "8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f",
    "debian12": "docker.io/library/debian:12@sha256:"
                "366a6da5efcceffbaa19c560a101b97b87a1a9664eeeac8bc3d24395ddbf14f3",
}
GCC = Path("/opt/gcc-13.3.0")
CC = str(GCC / "bin/gcc")
CXX = str(GCC / "bin/g++")
SYSTEM_NEEDED = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "ld-linux-x86-64.so.2",
}
GCC_NEEDED = {"libstdc++.so.6", "libgcc_s.so.1"}
FLOOR_CHECKER_SHA = "a2200c07fd4e60c66d9a38834fda7817ecf5ab5fa9ee75264b42c4475a313a62"
PROTECTED = {
    ".git", ".ssh", ".aws", ".codex", ".agents", ".config", ".gnupg", ".kube",
    ".azure", ".netrc", "credentials", "keys", "secrets",
}
ENV = {
    "PATH": "/usr/bin:/bin", "LC_ALL": "C", "LANG": "C",
    "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_ATTR_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0", "GIT_OPTIONAL_LOCKS": "0",
}


class Refusal(Exception):
    pass


class ChildFailure(Refusal):
    def __init__(self, label: str, code: int):
        super().__init__(f"{label}: child exit {code}; see complete command logs")
        self.code = code if 0 < code < 256 else 128 + abs(code) if code < 0 else 1


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Refusal(message)


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def component(name: str) -> None:
    lower = name.lower()
    require(name not in ("", ".", "..") and not any(
        ord(c) < 32 or ord(c) == 127 for c in name), f"unsafe path component: {name!r}")
    require(lower not in PROTECTED and not lower.startswith(".env")
            and not lower.endswith((".env", ".pem", ".key"))
            and not lower.startswith(("id_rsa", "id_dsa", "id_ecdsa", "id_ed25519")),
            f"protected path refused by name: {name}")


def relative(name: str) -> Path:
    require(bool(name) and not name.startswith("/") and "\\" not in name
            and "," not in name and "//" not in name, f"unsafe relative path: {name!r}")
    for part in name.split("/"):
        component(part)
    return Path(name)


def absolute(value: str | Path) -> Path:
    text = str(value)
    require(text.startswith("/") and not text.endswith("/") and "//" not in text
            and "," not in text and "\\" not in text, f"unsafe absolute path: {text!r}")
    relative(text[1:])
    return Path(text)


def checked(path: Path, *, missing: bool = False) -> Path:
    """Metadata only: reject every symlink ancestor before any content read."""
    path = absolute(path)
    current = Path("/")
    for part in path.parts[1:]:
        current /= part
        if missing and not os.path.lexists(current):
            continue
        info = current.lstat()
        require(not stat.S_ISLNK(info.st_mode), f"symlink refused: {current}")
        require(stat.S_ISDIR(info.st_mode) or
                (current == path and stat.S_ISREG(info.st_mode)),
                f"special/non-directory path refused: {current}")
    return path


def beneath(path: Path, root: Path) -> bool:
    return path != root and root in path.parents


def open_regular(path: Path) -> int:
    """Open without following a link in any component, including during a race."""
    path = absolute(path)
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    parent = os.open("/", flags)
    result = None
    try:
        for part in path.parts[1:-1]:
            child = os.open(part, flags, dir_fd=parent)
            os.close(parent)
            parent = child
        result = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
                         | os.O_NONBLOCK, dir_fd=parent)
        require(stat.S_ISREG(os.fstat(result).st_mode), f"not a regular input: {path}")
        return result
    except BaseException:
        if result is not None:
            os.close(result)
        raise
    finally:
        os.close(parent)


def mkdir(path: Path) -> None:
    checked(path, missing=True)
    path.mkdir(parents=True, exist_ok=False)
    checked(path)


def put(path: Path, data: bytes | str) -> None:
    checked(path.parent)
    component(path.name)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    with os.fdopen(os.open(path, flags, 0o644), "wb") as stream:
        stream.write(data.encode("utf-8") if isinstance(data, str) else data)
        stream.flush()
        os.fsync(stream.fileno())


def put_json(path: Path, data) -> None:
    put(path, json.dumps(data, indent=2, ensure_ascii=True) + "\n")


class Session:
    def __init__(self, report: Path, mode: str, roots: list[Path]):
        self.report = checked(report)
        self.mode = mode
        self.roots = [absolute(root) for root in roots]
        self.sequence = 0
        self.events = report / "events.jsonl"
        put(self.events, b"")
        put_json(report / "INCOMPLETE.json", {"mode": mode, "started": now(),
                                             "status": "INCOMPLETE"})
        self.environment = dict(ENV)
        if mode == "host":
            temporary = report.parent / "tmp"
            docker_settings = report.parent / "docker-settings"
            mkdir(temporary)
            mkdir(docker_settings)
            self.environment.update(TMPDIR=str(temporary), DOCKER_CONFIG=str(docker_settings))
        else:
            self.environment.update(PATH=str(GCC / "bin") + ":/usr/bin:/bin",
                                    LD_LIBRARY_PATH=str(GCC / "lib64") + ":" + str(GCC / "lib"),
                                    TMPDIR="/output/tmp")

    def emit(self, event: str, **fields) -> None:
        checked(self.events)
        with os.fdopen(os.open(self.events, os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW),
                       "ab") as stream:
            stream.write((json.dumps({"event": event, "time": now(), **fields},
                                     ensure_ascii=True) + "\n").encode())
            stream.flush()
            os.fsync(stream.fileno())

    def follows(self, paths, purpose: str) -> None:
        names = [str(absolute(p)) for p in paths]
        # Record the complete declaration BEFORE checking/opening the inputs.
        self.emit("follow_inputs", purpose=purpose, paths=names)
        for name in names:
            path = Path(name)
            require(any(path == root or beneath(path, root) for root in self.roots),
                    f"out-of-root input refused: {path}")
            checked(path)

    def magic(self, path: Path) -> bytes:
        self.follows([path], "identify every file's ELF/archive magic")
        with os.fdopen(open_regular(path), "rb") as stream:
            return stream.read(8)

    def read(self, path: Path) -> bytes:
        self.follows([path], "read regular input")
        with os.fdopen(open_regular(path), "rb") as stream:
            before = os.fstat(stream.fileno())
            require(stat.S_ISREG(before.st_mode), f"not a regular input: {path}")
            data = stream.read()
            after = os.fstat(stream.fileno())
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
                and len(data) == before.st_size, f"input changed during read: {path}")
        return data

    def digest(self, path: Path) -> dict:
        self.follows([path], "hash regular input")
        digest = hashlib.sha256()
        with os.fdopen(open_regular(path), "rb") as stream:
            before = os.fstat(stream.fileno())
            require(stat.S_ISREG(before.st_mode), f"not a regular input: {path}")
            size = 0
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
            after = os.fstat(stream.fileno())
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
                and size == before.st_size, f"input changed during hash: {path}")
        return {"path": str(path), "size": size, "sha256": digest.hexdigest()}

    def copy(self, src: Path, dst: Path) -> None:
        put(dst, self.read(src))
        os.chmod(dst, stat.S_IMODE(src.lstat().st_mode) & 0o755)

    def files(self, root: Path) -> list[Path]:
        self.follows([root], "metadata inventory; no symlink traversal")
        pending, files = [root], []
        while pending:
            directory = pending.pop()
            checked(directory)
            with os.scandir(directory) as scan:
                entries = sorted(scan, key=lambda entry: entry.name)
            self.emit("directory_entries", path=str(directory),
                      names=[entry.name for entry in entries])
            for entry in entries:
                component(entry.name)
                path = directory / entry.name
                info = entry.stat(follow_symlinks=False)
                require(not stat.S_ISLNK(info.st_mode), f"symlink refused: {path}")
                if stat.S_ISDIR(info.st_mode):
                    pending.append(path)
                else:
                    require(stat.S_ISREG(info.st_mode), f"special file refused: {path}")
                    files.append(path)
        files.sort()
        self.follows(files, "complete file inventory before content reads or execution")
        return files

    def manifest(self, root: Path, name: str) -> list[dict]:
        rows = [{**self.digest(p), "path": p.relative_to(root).as_posix()}
                for p in self.files(root)]
        require(bool(rows), f"empty file inventory: {root}")
        put_json(self.report / name, rows)
        return rows

    def run(self, label: str, argv, *, inputs=(), allow_nonzero: bool = False,
            cwd: Path | None = None, environment: dict | None = None,
            allow_unavailable: bool = False) -> dict:
        require(re.fullmatch(r"[a-z0-9-]+", label) is not None, "unsafe command label")
        require(not allow_unavailable or allow_nonzero,
                "unavailable commands must remain explicit failed observations")
        self.follows(inputs, f"command {label}")
        if cwd is not None:
            self.follows([cwd], f"command working directory {label}")
        self.sequence += 1
        base = self.report / f"{self.sequence:03d}-{label}"
        out, err = Path(str(base) + ".stdout"), Path(str(base) + ".stderr")
        argv = [str(arg) for arg in argv]
        row = {"label": label, "argv": argv, "cwd": str(cwd) if cwd else None,
               "started": now(), "stdout": str(out), "stderr": str(err),
               "returncode": None}
        self.emit("command_start", **row)
        started = time.monotonic()
        child = None
        try:
            with out.open("xb") as stdout, err.open("xb") as stderr:
                child = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stdout,
                                       stderr=stderr, cwd=cwd,
                                       env=environment or self.environment,
                                       check=False)
            row["returncode"] = child.returncode
            put(Path(str(base) + ".exit"), f"{child.returncode}\n")
        except OSError as error:
            row["spawn_or_wait_error"] = repr(error)
            if not allow_unavailable:
                raise
        except BaseException as error:
            row["spawn_or_wait_error"] = repr(error)
            raise
        finally:
            row.update(finished=now(), elapsed_seconds=time.monotonic() - started)
            put_json(Path(str(base) + ".json"), row)
            self.emit("command_end", **row)
        row["stdout_hash"] = self.digest(out) if os.path.lexists(out) else None
        row["stderr_hash"] = self.digest(err) if os.path.lexists(err) else None
        self.emit("command_output_hashes", label=label, stdout=row["stdout_hash"],
                  stderr=row["stderr_hash"])
        if child is not None and child.returncode != 0 and not allow_nonzero:
            raise ChildFailure(label, child.returncode)
        return row

    def text(self, row: dict, stream: str = "stdout") -> str:
        return self.read(Path(row[stream])).decode("utf-8", "strict")


def verified_download(s: Session, label: str, url: str, target: Path,
                      digest: str | None = None, size: int | None = None) -> dict:
    require(url.startswith("https://"), "download requires HTTPS")
    require(not os.path.lexists(target), f"download destination reused: {target}")
    # --disable is first: never discover a user's curl configuration.
    row = s.run(label, ["curl", "--disable", "--fail", "--location", "--show-error",
                       "--silent", "--proto", "=https", "--proto-redir", "=https",
                       "--tlsv1.2", "--output", target, url], inputs=[target.parent])
    observed = s.digest(target)
    s.emit("download", url=url, command=row["label"], **observed)
    require(digest is None or observed["sha256"] == digest, f"download hash mismatch: {url}")
    require(size is None or observed["size"] == size, f"download size mismatch: {url}")
    return observed


def deb_fields(text: str) -> dict[str, str]:
    result = {}
    current = None
    for line in text.splitlines():
        if line.startswith((" ", "\t")):
            require(current is not None, "invalid Debian continuation")
            result[current] += "\n" + line
        else:
            key, separator, value = line.partition(":")
            require(bool(separator) and key not in result, "duplicate/invalid Debian field")
            result[key] = value.strip()
            current = key
    return result


def bootstrap(s: Session, source: Path, work: Path) -> Path:
    root = work / "bootstrap"
    mkdir(root)
    gnupg = root / "gpgv-home"
    mkdir(gnupg)
    os.chmod(gnupg, 0o700)
    signed = root / "InRelease"
    verified_download(s, "fetch-inrelease", SNAPSHOT + "/dists/jammy-updates/InRelease",
                      signed)
    key = s.digest(KEYRING)
    put_json(s.report / "ubuntu-keyring.json", key)
    release = root / "Release"
    signature = s.run("verify-inrelease",
                      ["gpgv", "--homedir", gnupg, "--keyring", KEYRING,
                       "--status-fd", "1", "--output", release, signed],
                      inputs=[KEYRING, signed, gnupg])
    status = s.text(signature)
    require("[GNUPG:] VALIDSIG " in status, "gpgv supplied no valid signature receipt")
    signed_text = s.read(release).decode("utf-8")
    fields = deb_fields(signed_text)
    require(fields.get("Origin") == "Ubuntu" and fields.get("Suite") == "jammy-updates"
            and fields.get("Codename") == "jammy", "wrong signed Ubuntu pocket")
    entries = [line.split() for line in fields.get("SHA256", "").splitlines() if line.strip()]
    matches = [entry for entry in entries if len(entry) == 3
               and entry[2] == "main/binary-amd64/Packages.xz"]
    require(matches == [[PACKAGES_SHA, str(PACKAGES_SIZE), "main/binary-amd64/Packages.xz"]],
            "signed Packages.xz entry differs from pinned metadata")
    packages = root / "Packages.xz"
    verified_download(s, "fetch-packages",
                      SNAPSHOT + "/dists/jammy-updates/main/binary-amd64/Packages.xz",
                      packages, PACKAGES_SHA, PACKAGES_SIZE)
    s.follows([packages], "decompress authenticated Packages.xz")
    with os.fdopen(open_regular(packages), "rb") as raw:
        with lzma.LZMAFile(raw, "rb") as stream:
            content = stream.read(64 * 1024 * 1024 + 1)
    require(len(content) <= 64 * 1024 * 1024, "Packages index exceeds bounded size")
    text = content.decode("utf-8")
    selected = [stanza for stanza in text.split("\n\n")
                if stanza.startswith("Package: ca-certificates\n")]
    require(len(selected) == 1, "CA package stanza is missing or ambiguous")
    stanza = deb_fields(selected[0])
    for key_name, value in {
        "Package": "ca-certificates", "Architecture": "all", "Version": CA_VERSION,
        "Filename": CA_RELATIVE, "Size": str(CA_SIZE), "SHA256": CA_SHA,
    }.items():
        require(stanza.get(key_name) == value, f"signed CA stanza mismatch: {key_name}")
    record = s.read(source / RECIPE / "bootstrap-ca.sha256")
    require(record == f"{CA_SHA}  {CA_NAME}\n".encode(), "unexpected static CA checksum record")
    deb = root / CA_NAME
    verified_download(s, "fetch-ca-package", SNAPSHOT + "/" + CA_RELATIVE,
                      deb, CA_SHA, CA_SIZE)
    provenance = s.report / "bootstrap"
    mkdir(provenance)
    for item in (signed, release):
        s.copy(item, provenance / item.name)
    put(provenance / "ca-certificates.stanza", selected[0] + "\n")
    put_json(provenance / "chain.json", {
        "snapshot": SNAPSHOT, "signed_metadata": s.digest(signed),
        "verified_release": s.digest(release), "packages": s.digest(packages),
        "ca": s.digest(deb), "gpgv": signature, "keyring": key,
    })
    context = work / "context"
    mkdir(context)
    recipe = context / RECIPE
    mkdir(recipe)
    for name in ("Dockerfile", "README.md", "ubuntu-snapshot.list",
                 "gcc-13.3.0.sha512", "bootstrap-ca.sha256"):
        s.copy(source / RECIPE / name, recipe / name)
    s.copy(deb, recipe / CA_NAME)
    inventory = s.manifest(context, "context.json")
    require(len(inventory) == 6, "minimal Docker context must contain exactly six inputs")
    return context


def export_source(s: Session, source: Path, destination: Path, role: str) -> dict:
    s.follows([source], f"{role} checkout and its Git metadata, read-only identity/export")
    # Git is the sole metadata reader. Hosted checkout must own a real .git
    # directory, never a worktree pointer or link to another metadata root.
    s.emit("git_metadata", checkout=str(source), path=str(source / ".git"),
           purpose="read-only Git commit/tree/blob operations; never exported")
    metadata = (source / ".git").lstat()
    require(stat.S_ISDIR(metadata.st_mode), "Git metadata link/worktree pointer refused")
    head_row = s.run(role + "-head", ["git", "-C", source, "rev-parse", "HEAD"])
    head = s.text(head_row).strip()
    require(re.fullmatch("[0-9a-f]{40}", head) is not None, "invalid actual commit identity")
    tree_row = s.run(role + "-tree", ["git", "-C", source, "rev-parse", "HEAD^{tree}"])
    tree = s.text(tree_row).strip()
    require(re.fullmatch("[0-9a-f]{40}", tree) is not None, "invalid actual tree identity")
    if role == "old":
        tag = s.run("old-tag", ["git", "-C", source, "rev-parse",
                              "refs/tags/v1.10.0^{commit}"])
        require(s.text(tag).strip() == head, "old checkout differs from actual v1.10.0 tag")
    else:
        require(head == os.environ.get("GITHUB_SHA"), "new checkout differs from pushed SHA")
    listing = s.run(role + "-paths", ["git", "-C", source, "ls-tree", "-rz",
                                     "--full-tree", head])
    records = []
    for entry in s.read(Path(listing["stdout"])).split(b"\0"):
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        mode, kind, oid = metadata.decode("ascii").split()
        path = relative(name.decode("utf-8"))
        require(re.fullmatch("[0-9a-f]{40}", oid) is not None, "invalid Git blob identity")
        require((mode in ("100644", "100755") and kind == "blob")
                or (mode == "160000" and kind == "commit"),
                f"symlink/unsupported Git entry refused: {path} {mode} {kind}")
        records.append((mode, kind, oid, path))
    require(bool(records), "empty source tree")
    s.follows([source / path for _, _, _, path in records],
              role + " complete tracked checkout paths before clean check/export")
    clean = s.run(role + "-clean", ["git", "-C", source, "-c", "core.fsmonitor=false",
                                  "status", "--porcelain=v1", "--untracked-files=normal"])
    require(not s.text(clean), f"{role} checkout is not clean")
    mkdir(destination)
    # cat-file reads immutable blobs, without checkout filters or export-subst.
    # Its stdout is retained directly as the exported file, not duplicated into
    # the upload artifact (which must not contain the complete source tree).
    export_rows = []
    for mode, kind, oid, path in records:
        target = destination / path
        if not target.parent.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
        checked(target.parent)
        if kind == "commit":
            mkdir(target)
            export_rows.append({"path": str(path), "gitlink": oid})
            continue
        s.emit("follow_git_blob", checkout=str(source), path=str(path), oid=oid)
        argv = ["git", "-C", str(source), "cat-file", "blob", oid]
        error_path = s.report / f"{role}-blob-{len(export_rows):05d}.stderr"
        started, wall_started = time.monotonic(), now()
        with target.open("xb") as output, error_path.open("xb") as error:
            child = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=output,
                                   stderr=error, env=s.environment, check=False)
        s.emit("git_blob_result", argv=argv, returncode=child.returncode,
               started=wall_started, finished=now(),
               elapsed_seconds=time.monotonic() - started, output=str(target),
               stdout_hash=s.digest(target), stderr_hash=s.digest(error_path),
               stderr=str(error_path))
        if child.returncode != 0:
            raise ChildFailure(f"Git blob export {path}", child.returncode)
        require(not s.read(error_path), f"Git blob export emitted stderr: {path}")
        os.chmod(target, int(mode[-3:], 8))
        export_rows.append({**s.digest(target), "path": str(path), "git_blob": oid})
    require(not os.path.lexists(destination / ".git"), "source export contains Git metadata")
    version = s.read(destination / "VERSION").decode().strip()
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[1-9][0-9]*)?", version)
            is not None, "invalid exported VERSION")
    if role == "old":
        require(version == "1.10.0", "old source VERSION is not 1.10.0")
    identity = {"commit": head, "tree": tree, "version": version,
                "export": str(destination), "files": export_rows}
    put_json(s.report / (role + "-source.json"), identity)
    return identity


def safe_unpack(s: Session, archive: Path, target: Path, top: str) -> Path:
    relative(top)
    require("/" not in top, "archive top must be one component")
    mkdir(target)
    s.follows([archive], "inventory entire tar before extracting any member")
    with os.fdopen(open_regular(archive), "rb") as raw, \
            tarfile.open(fileobj=raw, mode="r:gz") as packed:
        members = packed.getmembers()
        names = set()
        for member in members:
            name = member.name.rstrip("/") if member.isdir() else member.name
            require(member.name in (name, name + "/"),
                    f"non-normalized archive member: {member.name}")
            path = relative(name)
            require(path.parts[0] == top, f"archive member outside declared root: {name}")
            require(name not in names, f"duplicate archive member: {name}")
            names.add(name)
            require(member.isdir() or member.isreg(),
                    f"archive link/device/special member refused: {name}")
        require(bool(members), "empty archive")
        s.emit("tar_inventory", archive=str(archive),
               members=[{"path": m.name, "size": m.size, "type": m.type.decode("ascii")}
                        for m in members])
        for member in members:
            path = target / relative(member.name.rstrip("/") if member.isdir()
                                     else member.name)
            if member.isdir():
                if not path.exists():
                    path.mkdir(parents=True)
                checked(path)
            else:
                if not path.parent.exists():
                    path.parent.mkdir(parents=True)
                checked(path.parent)
                data = packed.extractfile(member)
                require(data is not None, f"unreadable archive member: {member.name}")
                with data, path.open("xb") as output:
                    size = 0
                    while chunk := data.read(1024 * 1024):
                        output.write(chunk)
                        size += len(chunk)
                require(size == member.size, f"short archive member: {member.name}")
                os.chmod(path, 0o755 if member.mode & 0o111 else 0o644)
    return checked(target / top)


def fetch_old(s: Session, work: Path) -> Path:
    root = work / "published"
    mkdir(root)
    archive, sidecar = root / OLD_NAME, root / (OLD_NAME + ".sha256")
    archive_receipt = verified_download(s, "fetch-old-sdk", OLD_URL + OLD_NAME,
                                       archive, OLD_SHA, OLD_SIZE)
    checksum_receipt = verified_download(s, "fetch-old-sidecar",
                                        OLD_URL + sidecar.name, sidecar, SIDECAR_SHA, 104)
    require(s.read(sidecar) == f"{OLD_SHA}  {OLD_NAME}\n".encode(),
            "published sidecar does not name the verified archive")
    put_json(s.report / "published-release.json", {
        "archive": archive_receipt, "sidecar": checksum_receipt,
        "archive_url": OLD_URL + OLD_NAME, "sidecar_url": OLD_URL + sidecar.name,
    })
    sdk = safe_unpack(s, archive, work / "old-unpacked", OLD_NAME.removesuffix(".tar.gz"))
    require(s.read(sdk / "VERSION").decode().strip() == "1.10.0", "wrong old SDK VERSION")
    s.manifest(sdk, "old-sdk.json")
    return sdk


def container_argv(image: str, mounts: list[tuple[Path, str, bool]], *, name: str,
                   environment: dict | None = None) -> list[str]:
    argv = ["docker", "run", "--name", name, "--pull=never", "--platform", "linux/amd64",
            "--network=none", "--read-only", "--cap-drop=ALL",
            "--security-opt=no-new-privileges",
            "--user", f"{os.getuid()}:{os.getgid()}"]
    for source, target, readonly in mounts:
        checked(source)
        require(target in ("/source", "/old-source", "/old-sdk", "/new-sdk",
                           "/output", "/reports", "/inputs", "/dependencies"),
                "unexpected container mount target")
        argv += ["--mount", f"type=bind,source={source},target={target}"
                 + (",readonly" if readonly else "")]
    for key, value in (environment or {}).items():
        argv += ["--env", f"{key}={value}"]
    return argv + [image]


def image_identity(s: Session, image: str, label: str) -> dict:
    inspected = s.run(label, ["docker", "image", "inspect", image])
    rows = json.loads(s.text(inspected))
    require(len(rows) == 1 and rows[0]["Architecture"] == "amd64"
            and rows[0]["Os"] == "linux", "image must be actual linux/amd64")
    require(re.fullmatch("sha256:[0-9a-f]{64}", rows[0]["Id"]) is not None,
            "invalid actual Docker image ID")
    return rows[0]


def inventory_provenance(s: Session) -> None:
    source = Path("/usr/local/share/sdk-toolchain")
    target = s.report / "toolchain"
    mkdir(target)
    for name in ("inputs", "gcc-build", "image", "licenses"):
        for path in s.files(source / name):
            destination = target / name / path.relative_to(source / name)
            if not destination.parent.exists():
                destination.parent.mkdir(parents=True)
            s.copy(path, destination)
    for name in ("bootstrap-ca.sha256", "final-ca-check.txt", "final-bundle.sha256"):
        s.copy(source / "bootstrap" / name, target / name)
    s.manifest(target, "toolchain-provenance.json")


def dependency_records(s: Session, paths: list[Path], name: str) -> list[dict]:
    records = []
    s.follows(paths, "complete ELF dependency inventory before readelf")
    for ordinal, path in enumerate(paths):
        row = s.run(f"needed-{name}-{ordinal}", ["readelf", "--wide", "--dynamic", path],
                    inputs=[path])
        text = s.text(row)
        needed = re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", text)
        unknown = set(needed) - SYSTEM_NEEDED - GCC_NEEDED
        require(not unknown, f"unpackaged runtime dependencies in {path}: {sorted(unknown)}")
        records.append({"file": s.digest(path), "needed": needed,
                        "system_supplied": sorted(set(needed) & SYSTEM_NEEDED),
                        "gcc_supplied": sorted(set(needed) & GCC_NEEDED)})
    put_json(s.report / f"dependencies-{name}.json", records)
    return records


def same_bytes(observed: dict, expected: dict) -> bool:
    return (observed["sha256"] == expected["sha256"]
            and observed["size"] == expected["size"]
            and observed["size"] > 0)


def load_floor_checker(s: Session, checker: Path):
    # Import the exact hash-verified bytes, without a second source read or a
    # bytecode-cache write. Only its existing low-level grammars are reused;
    # parse_elf's shared-library guard is never altered or bypassed.
    data = s.read(checker)
    require(hashlib.sha256(data).hexdigest() == FLOOR_CHECKER_SHA,
            "floor checker differs from the unchanged reviewed implementation")
    module = types.ModuleType("_sdk_diagnostic_floor")
    module.__file__ = str(checker)
    exec(compile(data, str(checker), "exec"), module.__dict__)
    return module


def elf_inventory(s: Session, root: Path, *, tool_receipts: dict | None = None):
    files = s.files(root)
    libraries = [p for p in files if p.name.endswith(".a")
                 or re.search(r"\.so(?:\..*)?$", p.name)]
    require(bool(libraries), "refuse empty symbol inspection inventory")
    tools = {}
    if tool_receipts is not None:
        require(set(tool_receipts) == {"old", "new"},
                "only the two exact compiled loader tools may be declared")
        for kind in ("old", "new"):
            path = root / (kind + "-loader")
            receipt = tool_receipts[kind]
            require(receipt["path"] == str(path) and path in files,
                    f"missing/substituted exact loader tool: {path}")
            require(same_bytes(s.digest(path), receipt),
                    f"compiled loader tool bytes changed: {path}")
            require(path.lstat().st_mode & 0o111, f"loader tool is not executable: {path}")
            tools[path] = receipt
    for path in files:
        magic = s.magic(path)
        if path in tools:
            require(magic.startswith(b"\x7fELF"), f"loader tool is not ELF: {path}")
            continue
        if magic.startswith(b"\x7fELF") or magic in (b"!<arch>\n", b"!<thin>\n"):
            require(path in libraries, f"ELF/archive outside checker's named inventory: {path}")
    s.emit("elf_inventory", root=str(root), libraries=[str(p) for p in libraries],
           tools=[str(p) for p in tools])
    return files, libraries


def check_target_coverage(s: Session, targets: list[dict], receipts: dict) -> int:
    require(len(targets) == len(receipts)
            and {t["path"] for t in targets} == set(receipts),
            "checker target coverage differs from complete input inventory")
    members = 0
    for target in targets:
        path = Path(target["path"])
        kind = "archive" if path.name.endswith(".a") else "shared"
        require(target["kind"] == kind and target["inspected"] is True
                and target["symbols"] > 0
                and same_bytes(target, receipts[str(path)])
                and same_bytes(s.digest(path), target),
                f"incomplete/changed actual inspected library: {path}")
        if kind == "archive":
            require(bool(target["expected_members"])
                    and [m["name"] for m in target["members"]] == target["expected_members"]
                    and [m["ordinal"] for m in target["members"]]
                    == list(range(1, len(target["expected_members"]) + 1))
                    and all(m["inspected"] is True for m in target["members"]),
                    "incomplete actual archive-member inspection")
            members += len(target["members"])
        else:
            require(not target["members"] and target["expected_members"] is None,
                    "shared ELF acquired an archive-member inventory")
    return members


def check_old_negative(floor, row: dict, summary: dict, targets: list[dict],
                       issues: list[dict]) -> None:
    require(row["returncode"] == 1 and summary["verdict"] == "FAIL"
            and summary["issues"] == len(issues) > 0 and len(targets) == 1,
            "old reference supplied no actual nonzero/FAIL policy result")
    target = targets[0]
    require(target["inspected"] is True and target["kind"] == "shared"
            and bool(target["isoc23_references"]),
            "old reference lacks inspected __isoc23_* symbol evidence")
    reported = [{key: value for key, value in issue.items()
                 if key not in ("event", "role")} for issue in issues]
    require(target["violations"] == reported,
            "old reference failed outside its inspected policy violations")
    isoc23_issues = 0
    for issue in reported:
        require(issue["path"] == target["path"] and issue.get("member") is None
                and issue.get("ordinal") == 1,
                "old reference has an unrelated setup/member error")
        reference = issue.get("reference")
        if reference is not None:
            require(floor.is_isoc23_reference(reference["symbol"])
                    and issue["cause"] == "forbidden __isoc23_* symbol reference: "
                    + reference["symbol"]
                    and any(all(observed[key] == value for key, value in reference.items())
                            for observed in target["isoc23_references"]),
                    "old reference refusal does not match its actual symbol data")
            isoc23_issues += 1
        else:
            match = re.fullmatch(
                r"(GLIBC_[0-9]+(?:\.[0-9]+)+) exceeds fixed GLIBC_2.35 ceiling",
                issue["cause"])
            require(match is not None and match[1] in target["versions"]
                    and floor.version_exceeds_ceiling(floor.numeric_version(match[1])),
                    "old reference failed for an unrelated cause")
    require(isoc23_issues > 0, "old reference supplied no actual __isoc23_* refusal")


def inspect_symbols(s: Session, checker: Path, root: Path, baseline: Path | None,
                    label: str, *, tool_receipts: dict | None = None,
                    expected_negative: bool = False) -> dict:
    require(s.digest(checker)["sha256"] == FLOOR_CHECKER_SHA,
            "floor checker differs from the unchanged reviewed implementation")
    require((baseline is None) == expected_negative,
            "positive inspections require the published baseline; old control is independent")
    files, libraries = elf_inventory(s, root, tool_receipts=tool_receipts)
    if expected_negative:
        require(libraries == [root / "strategy.so"] and tool_receipts is None,
                "old negative must inspect exactly the separately compiled strategy.so")
    receipts = {str(p): s.digest(p) for p in libraries}
    baseline_files, baseline_libraries = (elf_inventory(s, baseline)
                                          if baseline is not None else ([], []))
    baseline_receipts = {str(p): s.digest(p) for p in baseline_libraries}
    argv = ["python3", "-I", checker, root]
    if baseline is not None:
        argv += ["--baseline-root", baseline]
    # Even a failing positive inspection must retain the real candidate and
    # published tagsets. Only the separate old control can accept a FAIL.
    row = s.run(label, argv, inputs=[checker, *files, *baseline_files], allow_nonzero=True)
    result = {"classification": "REFUSED", "command": row, "summary": None,
              "targets": [], "baseline_targets": [], "baseline_comparisons": [], "issues": []}
    try:
        require(not s.text(row, "stderr"), "symbol checker emitted stderr")
        summaries = []
        for line in s.text(row).splitlines():
            event = json.loads(line)
            require(isinstance(event, dict) and isinstance(event.get("event"), str),
                    "malformed symbol-inspector event")
            role = event.get("role")
            require(role in (None, "baseline"), "unknown symbol-inspector role")
            if event["event"] == "summary":
                require(role is None, "baseline supplied a separate/ambiguous summary")
                summaries.append(event)
            elif event["event"] == "target":
                result["baseline_targets" if role == "baseline" else "targets"].append(event)
            elif event["event"] == "baseline_comparison":
                result["baseline_comparisons"].append(event)
            elif event["event"] == "issue":
                result["issues"].append(event)
        require(len(summaries) == 1, "checker supplied no unique summary")
        summary = result["summary"] = summaries[0]
        require(summary["discovered"] == summary["inspected"] == len(libraries)
                and summary["issues"] == len(result["issues"]),
                "incomplete actual symbol inspection or issue coverage")
        members = check_target_coverage(s, result["targets"], receipts)
        require(summary["archive_members"] == members,
                "checker archive-member count differs from complete input inventory")
        if baseline is not None:
            require(summary["baseline_discovered"] == summary["baseline_inspected"]
                    == len(baseline_libraries) and bool(baseline_libraries),
                    "published baseline was not completely inspected")
            check_target_coverage(s, result["baseline_targets"], baseline_receipts)
            require(len(result["baseline_comparisons"]) == 1,
                    "missing actual published-baseline comparison")
        else:
            require(not result["baseline_targets"] and not result["baseline_comparisons"],
                    "independent old negative unexpectedly used a baseline")
        require(s.files(root) == files
                and (baseline is None or s.files(baseline) == baseline_files),
                "inspection input inventory changed")
        if tool_receipts is not None:
            for kind, receipt in tool_receipts.items():
                require(same_bytes(s.digest(root / (kind + "-loader")), receipt),
                        "loader tool changed during shared-library inspection")
        if expected_negative:
            check_old_negative(load_floor_checker(s, checker), row, summary,
                               result["targets"], result["issues"])
            result["classification"] = "EXPECTED_ISOC23_REFUSAL"
        else:
            require(row["returncode"] == 0 and summary["verdict"] == "PASS"
                    and summary["issues"] == 0
                    and result["baseline_comparisons"][0]["exceeds"] is False
                    and all(not t["isoc23_references"] for t in result["targets"]),
                    "failing actual symbol/baseline admission; see real tagsets and issues")
            result["classification"] = "PASS"
        return summary
    except (Refusal, KeyError, TypeError, ValueError) as error:
        result["error"] = repr(error)
        raise
    finally:
        put_json(s.report / f"{label}-result.json", result)


def parse_executable_elf(floor, output: str, layout: str) -> dict:
    """Parse genuine PIE output, independently of the DSO-only parse_elf API."""
    lines = output.splitlines()
    require(lines.count("ELF Header:") == 1
            and sum(bool(re.match(r"\s*Magic:\s+7f 45 4c 46(?:\s|$)", line))
                    for line in lines) == 1, "missing/malformed executable ELF header")

    def field(name):
        values = [match[1] for line in lines
                  if (match := re.fullmatch(r"\s*" + name + r":\s+(.+)", line))]
        require(len(values) == 1, f"missing/duplicate executable ELF {name}")
        return values[0]

    elf_type, machine, elf_class, data = (
        field("Type"), field("Machine"), field("Class"), field("Data"))
    require(elf_type == "DYN (Position-Independent Executable file)",
            f"loader tool is not an actual PIE executable: {elf_type}")
    require(elf_class == "ELF64" and data == "2's complement, little endian",
            "loader tool has unsupported ELF class/encoding")
    interpreters = re.findall(
        r"^\s*\[Requesting program interpreter: ([^\]\r\n]+)\]\s*$", layout, re.M)
    flags = re.findall(r"^\s*0x[0-9a-fA-F]+\s+\(FLAGS_1\)\s+Flags:\s+(.+)$", layout, re.M)
    require(layout.splitlines().count("Program Headers:") == 1
            and len(re.findall(r"^\s*INTERP\s+", layout, re.M)) == 1
            and len(interpreters) == 1 and interpreters[0].startswith("/")
            and len(flags) == 1 and "PIE" in flags[0].split(),
            "missing/malformed executable interpreter or PIE dynamic flags")
    tables, table_symbols, versions = {}, {}, set()
    references, isoc23 = [], []
    meaningful, index = 0, 0
    while index < len(lines):
        line = lines[index]
        header = floor.SYMBOL_HEADER.fullmatch(line)
        if header is None:
            require(not line.startswith("Symbol table ")
                    and floor.SYMBOL_ROW.fullmatch(line) is None,
                    "unparsable/uncovered executable symbol table row")
            index += 1
            continue
        table, count_text = header.groups()
        count = int(count_text)
        require(table not in tables and count > 0, "duplicate/empty executable symbol table")
        tables[table], table_symbols[table] = count, 0
        index += 1
        require(index < len(lines) and lines[index].split() == [
            "Num:", "Value", "Size", "Type", "Bind", "Vis", "Ndx", "Name"],
            "unparsable executable symbol columns")
        index += 1
        for ordinal in range(count):
            require(index < len(lines), "incomplete executable symbol table")
            symbol_row = floor.SYMBOL_ROW.fullmatch(lines[index])
            require(symbol_row is not None and int(symbol_row[1]) == ordinal,
                    "incomplete/noncontiguous executable symbol row")
            symbol = re.sub(r"\s+\([0-9]+\)$", "", (symbol_row[8] or "").strip())
            index += 1
            if not symbol or symbol_row[4] in ("FILE", "SECTION"):
                continue
            meaningful += 1
            table_symbols[table] += 1
            version = None
            if "@" in symbol:
                suffix = re.fullmatch(r"([^@]+)@{1,2}([^@]+)", symbol)
                require(suffix is not None, "unparsable executable versioned symbol")
                version = suffix[2]
                versions.add(version)
            reference = {"symbol": symbol, "table": table, "index": ordinal,
                         "section": symbol_row[7]}
            if version and version.startswith("GLIBC_"):
                references.append(reference)
            if floor.is_isoc23_reference(symbol):
                isoc23.append(reference)
    require(table_symbols.get(".dynsym", 0) > 0,
            "no complete nonempty executable dynamic symbol table")
    try:
        versions.update(floor.version_sections(lines, tables.get(".dynsym")))
    except (floor.InspectionError, ValueError) as error:
        raise Refusal(f"uninspectable executable version metadata: {error}") from error
    glibc = sorted(name for name in versions if name.startswith("GLIBC_"))
    return {
        "kind": "pie-executable", "elf_type": "DYN", "type_description": elf_type,
        "machine": machine, "class": elf_class, "data": data,
        "interpreter": interpreters[0], "dynamic_flags": flags[0].split(),
        "symbol_tables": tables, "table_symbols": table_symbols, "symbols": meaningful,
        "versions": glibc, "other_versions": sorted(versions.difference(glibc)),
        "max_glibc": floor.maximum_version(glibc), "glibc_references": references,
        "isoc23_references": isoc23,
    }


def executable_violations(floor, facts: dict) -> list[str]:
    issues = []
    for name in facts["versions"]:
        version = floor.numeric_version(name)
        if version is None:
            issues.append(f"private/unknown GLIBC version refused: {name}")
        elif floor.version_exceeds_ceiling(version):
            issues.append(f"{name} exceeds fixed GLIBC_2.35 ceiling")
    issues.extend("forbidden __isoc23_* symbol reference: " + reference["symbol"]
                  for reference in facts["isoc23_references"])
    return issues


def inspect_tools(s: Session, checker: Path, root: Path, receipts: dict) -> dict:
    elf_inventory(s, root, tool_receipts=receipts)
    floor = load_floor_checker(s, checker)
    result = {"status": "INCOMPLETE", "tools": []}
    try:
        for kind in ("old", "new"):
            path = root / (kind + "-loader")
            record = {"file": s.digest(path), "elf": None, "inspected": False,
                      "violations": [], "command": None, "layout_command": None}
            result["tools"].append(record)
            require(same_bytes(record["file"], receipts[kind]), "compiled tool changed")
            row = record["command"] = s.run("tool-" + kind + "-symbols", [
                "readelf", "--wide", "--file-header", "--symbols", "--version-info",
                "--", path], inputs=[path])
            layout = record["layout_command"] = s.run("tool-" + kind + "-layout", [
                "readelf", "--wide", "--program-headers", "--dynamic", "--", path],
                inputs=[path])
            require(not s.text(row, "stderr") and not s.text(layout, "stderr"),
                    "executable readelf emitted a diagnostic")
            record["elf"] = parse_executable_elf(floor, s.text(row), s.text(layout))
            require(same_bytes(s.digest(path), record["file"]),
                    "loader tool changed during real readelf inspection")
            record["inspected"] = True
            record["violations"] = executable_violations(floor, record["elf"])
            require(not record["violations"],
                    f"loader tool violates fixed symbol policy: {path}")
        result["status"] = "INSPECTED"
        return result
    except (Refusal, OSError, KeyError, TypeError, ValueError) as error:
        result["error"] = repr(error)
        raise
    finally:
        put_json(s.report / "consumer-tools-result.json", result)


def consumer_inventory(s: Session, root: Path, records: dict) -> dict:
    """Bind the entire consumer tree to exactly two tools and two real DSOs."""
    require(set(records) == {"old", "new"}, "missing/unknown compiled consumer kind")
    expected = {}
    for kind in ("old", "new"):
        paths = {"loader": Path("generated") / (kind + "-loader"),
                 "strategy": (Path("old/strategy.so") if kind == "old"
                              else Path("generated/new-strategy.so"))}
        for role, relative_path in paths.items():
            receipt = records[kind][role]
            require(receipt["path"] == str(Path("/output") / relative_path),
                    "compiler receipt names a substituted consumer path")
            expected[root / relative_path] = receipt
    files = s.files(root)
    observed = {}
    for path in files:
        magic = s.magic(path)
        binary = (magic.startswith(b"\x7fELF")
                  or magic in (b"!<arch>\n", b"!<thin>\n")
                  or path.name.endswith(".a") or re.search(r"\.so(?:\..*)?$", path.name))
        if binary:
            require(path in expected and magic.startswith(b"\x7fELF"),
                    f"unexpected/malformed consumer ELF/archive: {path}")
            observed[path] = s.digest(path)
            require(same_bytes(observed[path], expected[path]),
                    f"consumer bytes differ from actual compile: {path}")
    require(set(observed) == set(expected), "missing exact compiled consumer ELF/tool")
    return {path.relative_to(root).as_posix(): receipt for path, receipt in observed.items()}


def require_positive_inspection(result: dict) -> None:
    summary = result["summary"]
    require(result["classification"] == "PASS" and result["command"]["returncode"] == 0
            and summary["verdict"] == "PASS" and summary["issues"] == 0
            and not result["issues"]
            and summary["discovered"] == summary["inspected"] == len(result["targets"]) > 0
            and summary["baseline_discovered"] == summary["baseline_inspected"]
            == len(result["baseline_targets"]) > 0
            and len(result["baseline_comparisons"]) == 1
            and result["baseline_comparisons"][0]["exceeds"] is False,
            "missing complete actual positive/published-baseline verdict")


def inside_build(s: Session, args) -> None:
    source, output, baseline = Path("/source"), Path("/output"), Path("/old-sdk")
    require(os.uname().machine == "x86_64", "native x86_64 builder required")
    require(not os.path.lexists(source / ".git"), "builder source must be an exported tree")
    require(re.fullmatch("[0-9a-f]{40}", args.source_sha or "") is not None,
            "missing exact build commit")
    s.files(source)
    require(s.read(source / "VERSION").decode().strip() == args.version, "FILE identity mismatch")
    inventory_provenance(s)
    for label, command, expected in (
        ("glibc", ["getconf", "GNU_LIBC_VERSION"], "glibc 2.35"),
        ("gcc", [CC, "-dumpfullversion"], "13.3.0"),
        ("gxx", [CXX, "-dumpfullversion"], "13.3.0"),
    ):
        require(s.text(s.run("build-" + label, command)).strip() == expected,
                f"actual {label} admission failed")
    identity = output / "identity.cmake"
    require(s.read(identity) == (
        f'set(PINEFORGE_VERSION_GIT_SHA "{args.source_sha[:7]}")\n'
        "set(PINEFORGE_VERSION_DIRTY OFF)\n").encode(), "identity include mismatch")
    build = output / "build"
    stage_parent = output / "stage"
    mkdir(stage_parent)
    stage = stage_parent / f"pineforge-v{args.version}-linux-x86_64"
    s.run("configure-sdk", [
        "cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_C_COMPILER={CC}", f"-DCMAKE_CXX_COMPILER={CXX}",
        "-DEigen3_DIR=/usr/share/eigen3/cmake", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DPINEFORGE_BUILD_TESTS=OFF", "-DPINEFORGE_BUILD_TUTORIAL=OFF",
        "-DPINEFORGE_BUILD_EXAMPLES=OFF", "-DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF",
        "-DPINEFORGE_VERSION_SOURCE=FILE", f"-DCMAKE_PROJECT_INCLUDE={identity}",
        "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={stage}",
    ], inputs=[source, identity, Path("/usr/share/eigen3/cmake"),
               Path("/usr/include/eigen3")])
    cache = s.read(build / "CMakeCache.txt").decode()
    for line in ("Eigen3_DIR:PATH=/usr/share/eigen3/cmake",
                 f"CMAKE_C_COMPILER:FILEPATH={CC}", f"CMAKE_CXX_COMPILER:FILEPATH={CXX}",
                 "PINEFORGE_VERSION_SOURCE:STRING=FILE"):
        require(line in cache.splitlines(), f"CMake resolution differs: {line}")
    s.run("build-sdk", ["cmake", "--build", build, "--parallel", "4"], inputs=[source, build])
    s.run("install-sdk", ["cmake", "--install", build], inputs=[build])
    s.run("installed-version", [
        "python3", "-I", source / "scripts/release_version.py", "check-install",
        "--prefix", stage, "--expect", args.version,
    ], inputs=[source / "scripts/release_version.py", stage])
    version_header = s.read(stage / "include/pineforge/version.h").decode()
    require(re.search(r'^#define PINEFORGE_GIT_SHA\s+"'
                      + args.source_sha[:7] + '"$', version_header, re.M) is not None,
            "installed header lost exact source identity")
    for name in ("libpineforge.a", "libpineforge_kernel.a", "libpineforge_window_plan.so"):
        require(s.digest(stage / "lib" / name)["size"] > 0, f"missing installed library: {name}")
    for name in ("LICENSE", "NOTICE", "VERSION"):
        s.copy(source / name, stage / name)
    deps = output / "consumer-dependencies"
    mkdir(deps)
    mkdir(deps / "lib")
    # Inspect directory metadata; never follow the compiler prefix's symlinks.
    library_dir = checked(GCC / "lib64")
    with os.scandir(library_dir) as scan:
        entries = sorted(scan, key=lambda e: e.name)
    s.emit("gcc_runtime_candidates", directory=str(library_dir),
           entries=[{"name": e.name, "mode": e.stat(follow_symlinks=False).st_mode}
                    for e in entries])
    selected = {}
    for soname, pattern in (
        ("libstdc++.so.6", r"libstdc\+\+\.so\.6(?:\.[0-9]+)+"),
        ("libgcc_s.so.1", r"libgcc_s\.so\.1"),
    ):
        candidates = [library_dir / e.name for e in entries if re.fullmatch(pattern, e.name)
                      and stat.S_ISREG(e.stat(follow_symlinks=False).st_mode)]
        require(len(candidates) == 1, f"ambiguous/missing regular GCC runtime: {soname}")
        chosen = candidates[0]
        dynamic = s.run("runtime-" + ("stdcpp" if "stdc" in soname else "gcc"),
                        ["readelf", "--wide", "--dynamic", chosen], inputs=[chosen])
        require(re.findall(r"\(SONAME\).*Library soname: \[([^\]]+)\]",
                           s.text(dynamic)) == [soname], "GCC runtime SONAME mismatch")
        selected[soname] = s.digest(chosen)
        s.copy(chosen, deps / "lib" / soname)
        s.copy(chosen, stage / "lib" / soname)
    put_json(s.report / "gcc-consumer-runtimes.json", selected)
    share = stage / "share/gcc-runtime"
    mkdir(share)
    mkdir(share / "source")
    mkdir(share / "licenses")
    s.copy(Path("/usr/local/share/sdk-toolchain/source/gcc-13.3.0.tar.xz"),
           share / "source/gcc-13.3.0.tar.xz")
    for path in s.files(Path("/usr/local/share/sdk-toolchain/licenses/gcc-13.3.0")):
        s.copy(path, share / "licenses" / path.name)
    s.copy(s.report / "gcc-consumer-runtimes.json", share / "runtime-files.json")
    s.copy(Path("/usr/local/share/sdk-toolchain/inputs/gcc-13.3.0.sha512"),
           share / "source/gcc-13.3.0.sha512")
    put_json(deps / "PROVENANCE.json", {
        "libraries": selected,
        "corresponding_source": s.digest(share / "source/gcc-13.3.0.tar.xz"),
        "licenses": [s.digest(p) for p in s.files(share / "licenses")],
        "packaged_with": str(stage),
        "note": "supplementary consumer dependencies; not part of published v1.10.0 SDK",
    })
    installed = s.manifest(stage, "installed-sdk.json")
    dynamic_files = [stage / r["path"] for r in installed
                     if re.search(r"\.so(?:\..*)?$", r["path"])]
    dependency_records(s, dynamic_files, "installed")
    summary = inspect_symbols(s, source / "scripts/check_prebuilt_glibc.py",
                              stage, baseline, "installed-symbols")
    put_json(s.report / "build.result.json", {
        "stage": str(stage), "version": args.version, "commit": args.source_sha,
        "symbol_summary": summary, "status": "BUILT_AND_INSPECTED",
    })


def abi_from_header(s: Session, prefix: Path) -> int:
    header = s.read(prefix / "include/pineforge/pineforge.h").decode()
    found = re.findall(r"^#define PF_ABI_VERSION\s+([0-9]+)\s*$", header, re.M)
    require(len(found) == 1, "matching installed header has no unique PF_ABI_VERSION")
    return int(found[0])


def inside_consume(s: Session, args) -> None:
    source, old_source = Path("/source"), Path("/old-source")
    output, old, new = Path("/output"), Path("/old-sdk"), Path("/new-sdk")
    require(os.uname().machine == "x86_64", "native x86_64 consumer compiler required")
    for label, command in (("gcc", [CC, "-dumpfullversion"]),
                           ("gxx", [CXX, "-dumpfullversion"])):
        require(s.text(s.run("consumer-" + label, command)).strip() == "13.3.0",
                "consumers require the same newly built GCC 13.3.0")
    require(s.text(s.run("consumer-glibc", ["getconf", "GNU_LIBC_VERSION"])).strip()
            == "glibc 2.35", "loader compiler must use old glibc")
    generated, old_module = output / "generated", output / "old"
    mkdir(generated)
    mkdir(old_module)
    records = {}
    for kind, prefix, matching_source in (("old", old, old_source), ("new", new, source)):
        example = matching_source / EXAMPLE
        loader = source / RECIPE / "load_strategy.c"
        so = old_module / "strategy.so" if kind == "old" else generated / "new-strategy.so"
        executable = generated / (kind + "-loader")
        s.follows([example, loader, prefix / "lib/libpineforge.a",
                   *s.files(prefix / "include")], f"{kind} complete consumer inputs")
        s.run("compile-" + kind + "-strategy", [
            CXX, "-std=c++17", "-O2", "-ffp-contract=off", "-fPIC", "-shared",
            "-I" + str(prefix / "include"), "-I/usr/include/eigen3", example,
            "-Wl,--whole-archive", prefix / "lib/libpineforge.a", "-Wl,--no-whole-archive",
            "-o", so,
        ], inputs=[example, prefix / "include", prefix / "lib/libpineforge.a",
                   Path("/usr/include/eigen3")])
        # PIE is an executable tool. It never enters the DSO-only checker;
        # its genuine executable metadata and symbols are inspected separately.
        s.run("compile-" + kind + "-loader", [
            CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie",
            "-I" + str(prefix / "include"), loader, "-ldl", "-o", executable,
        ], inputs=[loader, prefix / "include"])
        records[kind] = {
            "abi": abi_from_header(s, prefix), "source": s.digest(example),
            "header": s.digest(prefix / "include/pineforge/pineforge.h"),
            "archive": s.digest(prefix / "lib/libpineforge.a"),
            "strategy": s.digest(so), "loader": s.digest(executable),
        }
    consumer_inventory(s, output, records)
    dependency_records(s, s.files(generated) + s.files(old_module), "consumers")
    checker = source / "scripts/check_prebuilt_glibc.py"
    tool_receipts = {kind: record["loader"] for kind, record in records.items()}
    tools = inspect_tools(s, checker, generated, tool_receipts)
    require(all(record["elf"]["machine"] == "Advanced Micro Devices X86-64"
                and record["elf"]["interpreter"] == "/lib64/ld-linux-x86-64.so.2"
                for record in tools["tools"]),
            "actual compiled loader machine/interpreter differs from native x86_64")
    summary = inspect_symbols(s, checker, generated, old, "consumer-symbols",
                              tool_receipts=tool_receipts)
    old_summary = inspect_symbols(s, checker, old_module, None, "old-reference-symbols",
                                  expected_negative=True)
    consumer_inventory(s, output, records)
    put_json(s.report / "consumers.result.json",
             {"status": "COMPILED_AND_INSPECTED", "inputs": records,
              "symbol_summary": summary, "old_reference_summary": old_summary,
              "tool_status": tools["status"]})


def package_sdk(s: Session, stage: Path, work: Path, inspected: list[dict]) -> Path:
    installed = s.manifest(stage, "package-input.json")
    require(installed == inspected, "package input differs from inspected installed manifest")
    archive = work / (stage.name + ".tar.gz")
    files = s.files(stage)
    s.follows(files, "pack exact installed files, without symlinks or metadata")
    with tarfile.open(archive, "x:gz") as packed:
        for path in files:
            with os.fdopen(open_regular(path), "rb") as payload:
                info = os.fstat(payload.fileno())
                member = tarfile.TarInfo((Path(stage.name) / path.relative_to(stage)).as_posix())
                member.size = info.st_size
                member.mode = stat.S_IMODE(info.st_mode)
                member.mtime = int(info.st_mtime)
                packed.addfile(member, payload)
    receipt = s.digest(archive)
    sidecar = work / (archive.name + ".sha256")
    expected = f"{receipt['sha256']}  {archive.name}\n"
    put(sidecar, expected)
    require(s.read(sidecar) == expected.encode(), "new archive sidecar write/read mismatch")
    unpacked = safe_unpack(s, archive, work / "new-unpacked", stage.name)
    transported = s.manifest(unpacked, "packed-sdk.json")
    require(installed == transported, "packed SDK bytes differ from inspected installed inventory")
    put_json(s.report / "new-archive.json", {"archive": receipt, "sidecar": s.digest(sidecar)})
    return unpacked


def classify_case(raw: dict, container: dict, stdout: str, stderr: str,
                  image_id: str, kind: str, abi: int) -> str:
    observed = container["State"]
    require(container["Image"] == image_id and observed["Status"] == "exited"
            and not observed["OOMKilled"] and not observed["Error"]
            and observed["ExitCode"] == raw["returncode"],
            "runtime setup/state mismatch; cannot classify as a strategy outcome")
    events = [json.loads(line) for line in stdout.splitlines()]
    start = {"event": "loader_start", "expected_abi": abi}
    require(kind in ("old", "new"), "unknown runtime consumer kind")
    if kind == "old":
        require(raw["returncode"] == 10 and events == [start]
                and stderr.startswith("dlopen failed: ")
                and re.search(r"undefined symbol: __isoc23_strtol(?:\W|$)", stderr) is not None,
                "old SDK did not produce the required actual __isoc23_strtol dlopen failure")
        return "EXPECTED_LOADER_RED"
    if raw["returncode"] != 0:
        raise ChildFailure("new SDK actual loader", raw["returncode"])
    require(not stderr and events == [
        start, {"event": "abi", "expected": abi, "actual": abi},
        {"event": "create_free", "non_null": True}, {"event": "loader_complete"},
    ], "new SDK supplied no complete ABI/create/free loader evidence")
    return "LOADER_ABI_CREATE_FREE_OK"


def runtime_text(s: Session, row: dict, stream: str, errors: list[dict]) -> str | None:
    if row.get(stream + "_hash") is None:
        errors.append({"phase": row["label"], "stream": stream,
                       "error": "command supplied no retained stream file"})
        return None
    try:
        return s.text(row, stream)
    except (OSError, UnicodeError) as error:
        errors.append({"phase": row["label"], "stream": stream, "error": repr(error)})
        return None


def actual_case(s: Session, os_name: str, image: str, image_id: str, kind: str,
                consumers: Path, dependencies: Path, abi: int, token: str,
                expected: dict) -> dict:
    name = f"sdkdiag-{token}-{os_name}-{kind}"
    loader = f"/inputs/generated/{kind}-loader"
    module = "/inputs/old/strategy.so" if kind == "old" else "/inputs/generated/new-strategy.so"
    input_receipt = {
        "loader": s.digest(consumers / Path(loader).relative_to("/inputs")),
        "strategy": s.digest(consumers / Path(module).relative_to("/inputs")),
        "gcc_dependencies": [s.digest(dependencies / "lib" / soname)
                             for soname in sorted(GCC_NEEDED)],
    }
    # These are shared trust prerequisites, outside outcome collection. A
    # substituted input stops execution rather than becoming a runtime case.
    require(all(same_bytes(input_receipt[role], expected[role])
                for role in ("loader", "strategy"))
            and len(input_receipt["gcc_dependencies"]) == len(expected["gcc_dependencies"])
            and all(actual["path"] == prior["path"] and same_bytes(actual, prior)
                    for actual, prior in zip(input_receipt["gcc_dependencies"],
                                             expected["gcc_dependencies"])),
            "exact runtime files differ from compiled/inspected inputs")
    put_json(s.report / f"inputs-{os_name}-{kind}.json", input_receipt)
    argv = container_argv(image, [(consumers, "/inputs", True),
                                 (dependencies, "/dependencies", True)], name=name,
                          environment={"LD_LIBRARY_PATH": "/dependencies/lib"})
    raw = s.run("load-" + os_name + "-" + kind, argv + [loader, module],
                inputs=s.files(consumers) + s.files(dependencies),
                allow_nonzero=True, allow_unavailable=True)
    # The single launch attempt and the single state query retain their actual
    # return codes/exceptions, including when Docker never created a container.
    state_row = s.run("state-" + os_name + "-" + kind,
                      ["docker", "container", "inspect", name],
                      allow_nonzero=True, allow_unavailable=True)
    result = {"os": os_name, "sdk": kind, "image": image, "image_id": image_id,
              "docker_run_attempted": True, "raw": raw, "state_command": state_row,
              "container": None, "inputs": input_receipt, "errors": [],
              "verdict": "UNCLASSIFIED"}
    errors = result["errors"]
    stdout, stderr = (runtime_text(s, raw, stream, errors) for stream in ("stdout", "stderr"))
    state_stdout = runtime_text(s, state_row, "stdout", errors)
    state_stderr = runtime_text(s, state_row, "stderr", errors)
    result.update(stdout=stdout, stderr=stderr,
                  state_stdout=state_stdout, state_stderr=state_stderr)
    if raw["returncode"] is None or raw.get("spawn_or_wait_error"):
        errors.append({"phase": "launch", "error": raw.get("spawn_or_wait_error"),
                       "returncode": raw["returncode"]})
    elif raw["returncode"] in (125, 126, 127):
        errors.append({"phase": "launch", "returncode": raw["returncode"],
                       "error": "Docker run reported a setup/exec failure; see actual streams/state"})
    if state_row["returncode"] != 0 or state_row.get("spawn_or_wait_error"):
        errors.append({"phase": "state", "error": state_row.get("spawn_or_wait_error"),
                       "returncode": state_row["returncode"]})
    elif state_stdout is not None:
        try:
            state = json.loads(state_stdout)
            require(isinstance(state, list) and len(state) == 1
                    and isinstance(state[0], dict), "missing actual container state")
            result["container"] = state[0]
        except (Refusal, ValueError, TypeError) as error:
            errors.append({"phase": "state", "error": repr(error)})
    # Preserve actual streams/state before classifying any loader outcome.
    put_json(s.report / f"raw-{os_name}-{kind}.json", result)
    if errors:
        result["verdict"] = "RUNTIME_UNAVAILABLE"
    else:
        try:
            result["verdict"] = classify_case(raw, result["container"], stdout, stderr,
                                              image_id, kind, abi)
        except (Refusal, KeyError, TypeError, ValueError) as error:
            result["verdict"] = "UNEXPECTED_OUTCOME"
            errors.append({"phase": "classification", "error": repr(error),
                           "traceback": traceback.format_exc()})
    put_json(s.report / f"case-{os_name}-{kind}.json", result)
    return result


def collect_cases(s: Session, consumers: Path, dependencies: Path, packed: Path,
                  compiled: dict, token: str, expected_inputs: dict) -> list[dict]:
    cases = []
    for os_name, runtime in RUNTIMES.items():
        preparation = {"image": runtime, "pull": None, "inspection_label": "image-" + os_name}
        runtime_id = None
        try:
            pull = preparation["pull"] = s.run(
                "pull-" + os_name, ["docker", "pull", "--platform", "linux/amd64", runtime],
                allow_nonzero=True, allow_unavailable=True)
            require(pull["returncode"] == 0 and not pull.get("spawn_or_wait_error"),
                    "runtime pull failed; see actual status/exception and command streams")
            runtime_id = image_identity(s, runtime, preparation["inspection_label"])["Id"]
        except (Refusal, OSError, KeyError, TypeError, ValueError) as error:
            preparation["error"] = repr(error)
            preparation["traceback"] = traceback.format_exc()
        for kind in ("old", "new"):
            if runtime_id is None:
                # A failed pull/identity check is evidence of unavailability,
                # never invented loader bytes or an expected old/new verdict.
                result = {
                    "os": os_name, "sdk": kind, "verdict": "RUNTIME_UNAVAILABLE",
                    "docker_run_attempted": False, "runtime_preparation": preparation,
                    "raw": None, "container": None,
                    "errors": [{"phase": "runtime_preparation",
                                "error": preparation["error"]}],
                }
                put_json(s.report / f"raw-{os_name}-{kind}.json", result)
                put_json(s.report / f"case-{os_name}-{kind}.json", result)
            else:
                dependency_root = dependencies if kind == "old" else packed
                result = actual_case(
                    s, os_name, runtime, runtime_id, kind, consumers, dependency_root,
                    compiled["inputs"][kind]["abi"], token, expected_inputs[kind])
            cases.append(result)
    return cases


def require_complete_cases(s: Session, cases: list[dict]) -> list[dict]:
    required = {(os_name, kind) for os_name in RUNTIMES for kind in ("old", "new")}
    errors, outcomes = [], []
    if len(cases) != 4 or {(case["os"], case["sdk"]) for case in cases} != required:
        errors.append("four required actual runtime outcomes are incomplete/duplicated")
    for os_name, kind in sorted(required):
        try:
            row = json.loads(s.read(s.report / f"case-{os_name}-{kind}.json"))
            matches = [case for case in cases if (case["os"], case["sdk"]) == (os_name, kind)]
            require(len(matches) == 1 and row == matches[0],
                    "retained case differs from its actual collected result")
            outcomes.append(row)
            require(row["verdict"] == ("EXPECTED_LOADER_RED" if kind == "old"
                                       else "LOADER_ABI_CREATE_FREE_OK")
                    and not row["errors"] and row["docker_run_attempted"] is True,
                    f"missing/errored/unexpected actual outcome: {os_name}/{kind}")
        except (Refusal, OSError, KeyError, TypeError, ValueError) as error:
            errors.append({"os": os_name, "sdk": kind, "error": repr(error)})
    put_json(s.report / "cases.result.json", {
        "status": "INCOMPLETE" if errors else "ALL_CASES_CLASSIFIED",
        "cases": cases, "retained_cases": outcomes, "errors": errors,
    })
    require(not errors, "four-case aggregate refused; see cases.result.json and each case")
    return outcomes


def host(s: Session, args, work: Path) -> None:
    require(os.uname().machine == "x86_64", "native hosted x86_64 required; no emulation")
    source, old_source = checked(absolute(args.new_source)), checked(absolute(args.old_tag_source))
    workspace = checked(absolute(os.environ["GITHUB_WORKSPACE"]))
    require(beneath(source, workspace) and beneath(old_source, workspace)
            and source != old_source and not beneath(source, old_source)
            and not beneath(old_source, source), "source roots must be separate workspace checkouts")
    require(os.environ.get("GITHUB_ACTIONS") == "true"
            and os.environ.get("RUNNER_ARCH") == "X64"
            and os.environ.get("RUNNER_OS") == "Linux", "requires the native hosted Linux X64 job")
    require(os.environ.get("GITHUB_EVENT_NAME") == "push"
            and os.environ.get("GITHUB_REF") == "refs/heads/diag/sdk-glibc235-1110"
            and os.environ.get("GITHUB_REPOSITORY") == "pineforge-4pass/pineforge-engine",
            "driver requires the same literal diagnostic push event as the job")
    # A deliberate uncaught exception proves that command recording preserves
    # the work's nonzero exit and traceback, before expensive/network work.
    own = source / DRIVER
    crash = s.run("deliberate-crash", ["python3", "-I", own, "--mode", "crash"],
                  inputs=[own], allow_nonzero=True)
    require(crash["returncode"] == 1
            and "RuntimeError: sdk-diag deliberate crash control" in s.text(crash, "stderr"),
            "deliberate crash control did not preserve actual nonzero/traceback")
    exported = work / "new-source"
    new_identity = export_source(s, source, exported, "new")
    old_exported = work / "old-source"
    export_source(s, old_source, old_exported, "old")
    require(s.digest(own)["sha256"] == s.digest(exported / DRIVER)["sha256"],
            "running driver differs from exact clean source export")
    context = bootstrap(s, exported, work)
    if args.prepare_only:
        put_json(s.report / "PREPARED.json", {"context": str(context),
                 "status": "AUTHENTICATED_CONTEXT_ONLY", "runtime_proof": False})
        return
    baseline = fetch_old(s, work)
    info = s.run("docker-native", ["docker", "info", "--format", "{{json .}}"])
    daemon = json.loads(s.text(info))
    require(daemon["OSType"] == "linux" and daemon["Architecture"] in ("x86_64", "amd64"),
            "Docker daemon must be native Linux x86_64")
    iid = work / "toolchain.iid"
    s.run("build-toolchain", [
        "docker", "build", "--platform", "linux/amd64", "--progress=plain",
        "--iidfile", iid, "--file", context / RECIPE / "Dockerfile", context,
    ], inputs=s.files(context))
    image = s.read(iid).decode().strip()
    require(re.fullmatch("sha256:[0-9a-f]{64}", image) is not None, "missing actual build image ID")
    toolchain = image_identity(s, image, "toolchain-image")
    require(toolchain["Id"] == image, "build ID differs from inspected local image")
    put_json(s.report / "diagnostic-image.json", {
        "local_image_id": image, "repo_digests": toolchain.get("RepoDigests"),
        "release_admitted": False, "note": "local diagnostic ID is not a release manifest pin",
    })
    token = os.environ["GITHUB_RUN_ID"] + "-" + os.environ["GITHUB_RUN_ATTEMPT"]
    require(re.fullmatch("[0-9]+-[0-9]+", token) is not None, "invalid unique run identity")
    output, build_report = work / "sdk-build", s.report / "sdk-build"
    mkdir(output)
    mkdir(build_report)
    put(output / "identity.cmake",
        f'set(PINEFORGE_VERSION_GIT_SHA "{new_identity["commit"][:7]}")\n'
        "set(PINEFORGE_VERSION_DIRTY OFF)\n")
    argv = container_argv(image, [(exported, "/source", True), (output, "/output", False),
                                 (baseline, "/old-sdk", True), (build_report, "/reports", False)],
                          name="sdkdiag-" + token + "-build",
                          environment={"TMPDIR": "/output/tmp"})
    mkdir(output / "tmp")
    s.run("build-installed-sdk", argv + [
        "python3", "-I", "/source/" + str(DRIVER), "--mode", "build",
        "--report-root", "/reports", "--source-sha", new_identity["commit"],
        "--version", new_identity["version"],
    ], inputs=[exported, output, baseline])
    built = json.loads(s.read(build_report / "build.result.json"))
    require(built["status"] == "BUILT_AND_INSPECTED", "builder supplied no actual result")
    stage = output / "stage" / f"pineforge-v{new_identity['version']}-linux-x86_64"
    installed_manifest = json.loads(s.read(build_report / "installed-sdk.json"))
    symbols = json.loads(s.read(build_report / "installed-symbols-result.json"))
    require_positive_inspection(symbols)
    installed_by_path = {row["path"]: row for row in installed_manifest}
    require({str(Path(target["path"]).relative_to(Path(built["stage"])))
             for target in symbols["targets"]}
            == {row["path"] for row in installed_manifest
                if row["path"].endswith(".a")
                or re.search(r"\.so(?:\..*)?$", row["path"])},
            "installed symbol verdict does not cover every installed library")
    for target in symbols["targets"]:
        path = str(Path(target["path"]).relative_to(Path(built["stage"])))
        require(target["sha256"] == installed_by_path[path]["sha256"]
                and target["size"] == installed_by_path[path]["size"],
                "actual inspected library differs from installed manifest")
    packed = package_sdk(s, stage, work, installed_manifest)
    consumers, consume_report = work / "consumers", s.report / "consumers"
    mkdir(consumers)
    mkdir(consumers / "tmp")
    mkdir(consume_report)
    argv = container_argv(image, [
        (exported, "/source", True), (old_exported, "/old-source", True),
        (baseline, "/old-sdk", True), (packed, "/new-sdk", True),
        (consumers, "/output", False), (consume_report, "/reports", False),
    ], name="sdkdiag-" + token + "-compile", environment={"TMPDIR": "/output/tmp"})
    s.run("compile-real-consumers", argv + [
        "python3", "-I", "/source/" + str(DRIVER), "--mode", "consume",
        "--report-root", "/reports",
    ], inputs=[exported, old_exported, baseline, packed, consumers])
    compiled = json.loads(s.read(consume_report / "consumers.result.json"))
    require(compiled["status"] == "COMPILED_AND_INSPECTED", "consumer compile evidence missing")
    consumer_inventory(s, consumers, compiled["inputs"])
    compiled_manifest = s.manifest(consumers, "compiled-inputs.json")
    compiled_by_path = {row["path"]: row for row in compiled_manifest}
    for record in compiled["inputs"].values():
        for name in ("strategy", "loader"):
            path = str(Path(record[name]["path"]).relative_to("/output"))
            require(record[name]["sha256"] == compiled_by_path[path]["sha256"]
                    and record[name]["size"] == compiled_by_path[path]["size"],
                    "runtime input differs from actual compiler output")
    consumer_symbols = json.loads(s.read(consume_report / "consumer-symbols-result.json"))
    require_positive_inspection(consumer_symbols)
    old_reference = json.loads(s.read(consume_report / "old-reference-symbols-result.json"))
    tools = json.loads(s.read(consume_report / "consumer-tools-result.json"))
    require(consumer_symbols["classification"] == "PASS"
            and len(consumer_symbols["targets"]) == 1
            and consumer_symbols["targets"][0]["path"] == "/output/generated/new-strategy.so"
            and old_reference["classification"] == "EXPECTED_ISOC23_REFUSAL"
            and old_reference["command"]["returncode"] == 1
            and old_reference["summary"]["verdict"] == "FAIL"
            and old_reference["summary"]["discovered"]
            == old_reference["summary"]["inspected"] == 1
            and len(old_reference["targets"]) == 1
            and old_reference["targets"][0]["path"] == "/output/old/strategy.so"
            and bool(old_reference["targets"][0]["isoc23_references"]),
            "positive consumer or independent expected-negative evidence is incomplete")
    inspected_consumers = {}
    for target in consumer_symbols["targets"] + old_reference["targets"]:
        path = str(Path(target["path"]).relative_to("/output"))
        require(target["inspected"] is True and same_bytes(target, compiled_by_path[path]),
                "runtime input differs from actual inspected consumer ELF")
        inspected_consumers[path] = target
    require(tools["status"] == compiled["tool_status"] == "INSPECTED"
            and len(tools["tools"]) == 2
            and {row["file"]["path"] for row in tools["tools"]}
            == {f"/output/generated/{kind}-loader" for kind in ("old", "new")},
            "the two exact executable inspections are incomplete")
    inspected_tools = {}
    for record in tools["tools"]:
        path = str(Path(record["file"]["path"]).relative_to("/output"))
        require(record["inspected"] is True and not record["violations"]
                and record["elf"]["kind"] == "pie-executable"
                and record["elf"]["elf_type"] == "DYN"
                and record["elf"]["machine"] == "Advanced Micro Devices X86-64"
                and record["elf"]["interpreter"] == "/lib64/ld-linux-x86-64.so.2"
                and same_bytes(record["file"], compiled_by_path[path]),
                "runtime tool differs from actual executable inspection")
        inspected_tools[path] = record["file"]
    # Exactly the old-glibc GCC libraries shipped with the new SDK are also
    # supplied separately to the old consumer. Published old SDK stays read-only.
    deps = output / "consumer-dependencies"
    expected_inputs = {}
    for kind in ("old", "new"):
        dependency_root = deps if kind == "old" else packed
        gcc_receipts = []
        for soname in sorted(GCC_NEEDED):
            receipt = s.digest(dependency_root / "lib" / soname)
            require(same_bytes(receipt, installed_by_path["lib/" + soname]),
                    "consumer GCC runtime differs from the inspected/packed SDK bytes")
            gcc_receipts.append(receipt)
        expected_inputs[kind] = {
            "loader": inspected_tools[f"generated/{kind}-loader"],
            "strategy": inspected_consumers["old/strategy.so" if kind == "old"
                                           else "generated/new-strategy.so"],
            "gcc_dependencies": gcc_receipts,
        }
    cases = collect_cases(s, consumers, deps, packed, compiled, token, expected_inputs)
    # Read the work's own classified files and checker summaries; an exit marker
    # or a Docker build/tag alone can never produce this verdict.
    outcomes = require_complete_cases(s, cases)
    for directory, label in ((build_report, "installed-symbols"),
                             (consume_report, "consumer-symbols")):
        result = json.loads(s.read(directory / f"{label}-result.json"))
        require_positive_inspection(result)
    require(json.loads(s.read(consume_report / "old-reference-symbols-result.json"))
            == old_reference
            and json.loads(s.read(consume_report / "consumer-tools-result.json")) == tools,
            "independent old-negative or executable inspection evidence changed")
    put_json(s.report / "RUN.COMPLETE", {
        "verdict": "PASS", "finished": now(), "cases": [
            {"os": c["os"], "sdk": c["sdk"], "verdict": c["verdict"]} for c in outcomes],
        "source_commit": new_identity["commit"], "source_tree": new_identity["tree"],
        "local_diagnostic_image_id": image, "release_admitted": False,
        "symbol_results": ["sdk-build/installed-symbols-result.json",
                           "consumers/consumer-symbols-result.json"],
        "old_reference_result": "consumers/old-reference-symbols-result.json",
        "executable_result": "consumers/consumer-tools-result.json",
        "case_aggregate": "cases.result.json",
    })


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("host", "build", "consume", "crash"), default="host")
    parser.add_argument("--new-source")
    parser.add_argument("--old-tag-source")
    parser.add_argument("--work-root")
    parser.add_argument("--report-root")
    parser.add_argument("--source-sha")
    parser.add_argument("--version")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if args.mode == "crash":
        raise RuntimeError("sdk-diag deliberate crash control")
    session = None
    try:
        require(args.report_root is not None, "explicit report-root required")
        report = absolute(args.report_root)
        if args.mode == "host":
            require(args.new_source is not None and args.old_tag_source is not None
                    and args.work_root is not None, "explicit new/old/work roots required")
            runner_temp = checked(absolute(os.environ["RUNNER_TEMP"]))
            work = absolute(args.work_root)
            require(beneath(work, runner_temp) and work.parent == runner_temp
                    and work.name.startswith("sdk-glibc235-") and report == work / "reports",
                    "unique work-root must be directly below RUNNER_TEMP; reports below work-root")
            mkdir(work)
            mkdir(report)
        else:
            require(report == Path("/reports"), "internal reports must use the declared mount")
            checked(report)
        roots = ([work, absolute(args.new_source), absolute(args.old_tag_source), KEYRING]
                 if args.mode == "host" else [
                     Path("/source"), Path("/output"), Path("/reports"), Path("/old-sdk"),
                     Path("/usr/local/share/sdk-toolchain"), GCC,
                     Path("/usr/include/eigen3"), Path("/usr/share/eigen3/cmake"),
                 ] + ([Path("/old-source"), Path("/new-sdk")] if args.mode == "consume" else []))
        session = Session(report, args.mode, roots)
        if args.mode == "host":
            host(session, args, work)
        elif args.mode == "build":
            inside_build(session, args)
        else:
            inside_consume(session, args)
        put(report / "driver.exit", "0\n")
        return 0
    except BaseException as error:
        code = error.code if isinstance(error, ChildFailure) else 130 if isinstance(
            error, KeyboardInterrupt) else 1
        if session is not None:
            # No suppression: inability to preserve evidence itself fails.
            put_json(session.report / "FAILURE.json", {
                "verdict": "INCOMPLETE", "error": repr(error), "exitcode": code,
                "traceback": traceback.format_exc(), "time": now(),
            })
            put(session.report / "driver.exit", f"{code}\n")
        traceback.print_exc()
        return code


if __name__ == "__main__":
    def interrupted(signum, _frame):
        raise ChildFailure("driver interrupted", 128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    sys.exit(main())
