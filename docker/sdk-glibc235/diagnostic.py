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
            cwd: Path | None = None, environment: dict | None = None) -> dict:
        require(re.fullmatch(r"[a-z0-9-]+", label) is not None, "unsafe command label")
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
        try:
            with out.open("xb") as stdout, err.open("xb") as stderr:
                child = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stdout,
                                       stderr=stderr, cwd=cwd,
                                       env=environment or self.environment,
                                       check=False)
            row["returncode"] = child.returncode
            put(Path(str(base) + ".exit"), f"{child.returncode}\n")
        except BaseException as error:
            row["spawn_or_wait_error"] = repr(error)
            raise
        finally:
            row.update(finished=now(), elapsed_seconds=time.monotonic() - started)
            put_json(Path(str(base) + ".json"), row)
            self.emit("command_end", **row)
        row["stdout_hash"] = self.digest(out)
        row["stderr_hash"] = self.digest(err)
        self.emit("command_output_hashes", label=label, stdout=row["stdout_hash"],
                  stderr=row["stderr_hash"])
        if child.returncode != 0 and not allow_nonzero:
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


def inspect_symbols(s: Session, checker: Path, root: Path, baseline: Path,
                    label: str) -> dict:
    require(s.digest(checker)["sha256"] == FLOOR_CHECKER_SHA,
            "floor checker differs from the unchanged reviewed implementation")
    files = s.files(root)
    libraries = [p for p in files if p.name.endswith(".a")
                 or re.search(r"\.so(?:\..*)?$", p.name)]
    require(bool(libraries), "refuse empty symbol inspection inventory")
    for path in files:
        magic = s.magic(path)
        if magic.startswith(b"\x7fELF") or magic in (b"!<arch>\n", b"!<thin>\n"):
            require(path in libraries, f"ELF/archive outside checker's named inventory: {path}")
    row = s.run(label, ["python3", "-I", checker, root, "--baseline-root", baseline],
                inputs=[checker, *files, *s.files(baseline)])
    summaries, targets = [], []
    s.follows([Path(row["stdout"])], "read actual symbol-inspector result stream")
    with os.fdopen(open_regular(Path(row["stdout"])), "r", encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            if event["event"] == "summary":
                summaries.append(event)
            if event["event"] == "target" and event.get("role") != "baseline":
                targets.append(event)
    require(len(summaries) == 1, "checker supplied no unique summary")
    summary = summaries[0]
    require(summary["verdict"] == "PASS" and summary["issues"] == 0
            and summary["discovered"] == summary["inspected"] == len(libraries)
            and summary["archive_members"] >= 0, "incomplete/failing actual symbol inspection")
    require({t["path"] for t in targets} == {str(p) for p in libraries}
            and all(t["inspected"] and not t["isoc23_references"] for t in targets),
            "checker target coverage differs from complete input inventory")
    for target in targets:
        if target["kind"] == "archive":
            require(bool(target["expected_members"])
                    and [m["name"] for m in target["members"]] == target["expected_members"]
                    and all(m["inspected"] for m in target["members"]),
                    "incomplete actual archive-member inspection")
    put_json(s.report / f"{label}-result.json",
             {"summary": summary, "command": row, "targets": [
                 {"path": t["path"], "sha256": t["sha256"], "size": t["size"],
                  "max_glibc": t["max_glibc"]} for t in targets]})
    return summary


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
        executable = generated / (kind + "-loader.so")
        s.follows([example, loader, prefix / "lib/libpineforge.a",
                   *s.files(prefix / "include")], f"{kind} complete consumer inputs")
        s.run("compile-" + kind + "-strategy", [
            CXX, "-std=c++17", "-O2", "-ffp-contract=off", "-fPIC", "-shared",
            "-I" + str(prefix / "include"), "-I/usr/include/eigen3", example,
            "-Wl,--whole-archive", prefix / "lib/libpineforge.a", "-Wl,--no-whole-archive",
            "-o", so,
        ], inputs=[example, prefix / "include", prefix / "lib/libpineforge.a",
                   Path("/usr/include/eigen3")])
        # PIE is an actual executable. The .so suffix makes this DYN ELF visible
        # to the unchanged all-member checker, which discovers .a/.so/.so.*.
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
    generated_files = s.files(generated)
    dependency_records(s, generated_files + s.files(old_module), "consumers")
    summary = inspect_symbols(s, source / "scripts/check_prebuilt_glibc.py",
                              generated, old_module, "consumer-symbols")
    put_json(s.report / "consumers.result.json",
             {"status": "COMPILED_AND_INSPECTED", "inputs": records,
              "symbol_summary": summary})


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


def actual_case(s: Session, os_name: str, image: str, image_id: str, kind: str,
                consumers: Path, dependencies: Path, abi: int, token: str) -> dict:
    name = f"sdkdiag-{token}-{os_name}-{kind}"
    loader = f"/inputs/generated/{kind}-loader.so"
    module = "/inputs/old/strategy.so" if kind == "old" else "/inputs/generated/new-strategy.so"
    input_receipt = {
        "loader": s.digest(consumers / Path(loader).relative_to("/inputs")),
        "strategy": s.digest(consumers / Path(module).relative_to("/inputs")),
        "gcc_dependencies": [s.digest(dependencies / "lib" / name)
                             for name in sorted(GCC_NEEDED)],
    }
    put_json(s.report / f"inputs-{os_name}-{kind}.json", input_receipt)
    argv = container_argv(image, [(consumers, "/inputs", True),
                                 (dependencies, "/dependencies", True)], name=name,
                          environment={"LD_LIBRARY_PATH": "/dependencies/lib"})
    raw = s.run("load-" + os_name + "-" + kind, argv + [loader, module],
                inputs=s.files(consumers) + s.files(dependencies), allow_nonzero=True)
    # Preserve the actual stopped-container state before interpreting a nonzero
    # loader result. Docker setup/exec/OOM/ABI errors never count as old RED.
    state_row = s.run("state-" + os_name + "-" + kind,
                      ["docker", "container", "inspect", name])
    state = json.loads(s.text(state_row))
    require(len(state) == 1, "missing actual container state")
    container = state[0]
    observed = container["State"]
    stderr, stdout = s.text(raw, "stderr"), s.text(raw)
    result = {"os": os_name, "sdk": kind, "raw": raw, "container": container,
              "inputs": input_receipt,
              "verdict": "UNCLASSIFIED"}
    # This file survives any later classification refusal.
    put_json(s.report / f"raw-{os_name}-{kind}.json", result)
    require(container["Image"] == image_id and observed["Status"] == "exited"
            and not observed["OOMKilled"] and not observed["Error"]
            and observed["ExitCode"] == raw["returncode"],
            "runtime setup/state mismatch; cannot classify as a strategy outcome")
    events = [json.loads(line) for line in stdout.splitlines()]
    start = {"event": "loader_start", "expected_abi": abi}
    if kind == "old":
        require(raw["returncode"] == 10 and events == [start]
                and stderr.startswith("dlopen failed: ")
                and re.search(r"undefined symbol: __isoc23_strtol(?:\W|$)", stderr) is not None,
                "old SDK did not produce the required actual __isoc23_strtol dlopen failure")
        result["verdict"] = "EXPECTED_LOADER_RED"
    else:
        if raw["returncode"] != 0:
            raise ChildFailure("new SDK actual loader", raw["returncode"])
        require(not stderr and events == [
            start, {"event": "abi", "expected": abi, "actual": abi},
            {"event": "create_free", "non_null": True}, {"event": "loader_complete"},
        ], "new SDK supplied no complete ABI/create/free loader evidence")
        result["verdict"] = "LOADER_ABI_CREATE_FREE_OK"
    put_json(s.report / f"case-{os_name}-{kind}.json", result)
    return result


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
    installed_by_path = {row["path"]: row for row in installed_manifest}
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
    compiled_manifest = s.manifest(consumers, "compiled-inputs.json")
    compiled_by_path = {row["path"]: row for row in compiled_manifest}
    for record in compiled["inputs"].values():
        for name in ("strategy", "loader"):
            path = str(Path(record[name]["path"]).relative_to("/output"))
            require(record[name]["sha256"] == compiled_by_path[path]["sha256"]
                    and record[name]["size"] == compiled_by_path[path]["size"],
                    "runtime input differs from actual compiler output")
    consumer_symbols = json.loads(s.read(consume_report / "consumer-symbols-result.json"))
    for target in consumer_symbols["targets"]:
        path = str(Path(target["path"]).relative_to("/output"))
        require(target["sha256"] == compiled_by_path[path]["sha256"]
                and target["size"] == compiled_by_path[path]["size"],
                "runtime input differs from actual inspected consumer ELF")
    # Exactly the old-glibc GCC libraries shipped with the new SDK are also
    # supplied separately to the old consumer. Published old SDK stays read-only.
    deps = output / "consumer-dependencies"
    for soname in GCC_NEEDED:
        require(s.digest(deps / "lib" / soname)["sha256"]
                == s.digest(packed / "lib" / soname)["sha256"],
                "old/new consumer GCC runtime bytes differ")
    cases = []
    for os_name, runtime in RUNTIMES.items():
        s.run("pull-" + os_name, ["docker", "pull", "--platform", "linux/amd64", runtime])
        runtime_id = image_identity(s, runtime, "image-" + os_name)["Id"]
        for kind in ("old", "new"):
            # New case uses the packed SDK's actual runtime libraries.
            dependency_root = deps if kind == "old" else packed
            cases.append(actual_case(s, os_name, runtime, runtime_id, kind, consumers,
                                     dependency_root, compiled["inputs"][kind]["abi"], token))
    required = {(os_name, kind) for os_name in RUNTIMES for kind in ("old", "new")}
    require({(case["os"], case["sdk"]) for case in cases} == required and len(cases) == 4,
            "four required actual runtime outcomes are incomplete")
    # Read the work's own classified files and checker summaries; an exit marker
    # or a Docker build/tag alone can never produce this verdict.
    outcomes = [json.loads(s.read(s.report / f"case-{os_name}-{kind}.json"))
                for os_name, kind in sorted(required)]
    require(all(row["verdict"] == ("EXPECTED_LOADER_RED" if row["sdk"] == "old"
                                  else "LOADER_ABI_CREATE_FREE_OK") for row in outcomes),
            "case result files do not establish all required outcomes")
    for directory, label in ((build_report, "installed-symbols"),
                             (consume_report, "consumer-symbols")):
        result = json.loads(s.read(directory / f"{label}-result.json"))
        require(result["summary"]["verdict"] == "PASS" and result["summary"]["issues"] == 0
                and result["summary"]["discovered"] == result["summary"]["inspected"] > 0,
                "missing complete actual symbol verdict")
    put_json(s.report / "RUN.COMPLETE", {
        "verdict": "PASS", "finished": now(), "cases": [
            {"os": c["os"], "sdk": c["sdk"], "verdict": c["verdict"]} for c in outcomes],
        "source_commit": new_identity["commit"], "source_tree": new_identity["tree"],
        "local_diagnostic_image_id": image, "release_admitted": False,
        "symbol_results": ["sdk-build/installed-symbols-result.json",
                           "consumers/consumer-symbols-result.json"],
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
