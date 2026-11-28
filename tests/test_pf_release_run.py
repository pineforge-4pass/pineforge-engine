"""Tests for inspecting the release image and staging its Docker inputs."""

import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tarfile
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import pf_release_run


IMAGE = "pineforge-release:test"
IMAGE_ID = "sha256:" + "a" * 64
# What the real image wrote, with exit 3, when its user could not read the
# strategy on a Docker 29 host (docker/entrypoint.sh, the snapshot step).
BOX_FAILURE = (
    "cp: cannot open '/in/strategy.cpp' for reading: Permission denied\n"
    "[pineforge] cannot snapshot strategy.cpp into the work dir\n"
)


def _inspect_with_config(config):
    result = subprocess.CompletedProcess(
        ["docker", "image", "inspect", IMAGE], 0,
        stdout=json.dumps([{"Id": IMAGE_ID, "Config": config}]), stderr="",
    )
    return mock.patch.object(pf_release_run.subprocess, "run", return_value=result)


@pytest.mark.parametrize("field", ["Entrypoint", "Cmd"])
def test_image_command_accepts_one_command_field(field):
    command = ["/entrypoint.sh"]
    with _inspect_with_config({field: command}):
        assert pf_release_run._image_command(IMAGE)[:2] == (IMAGE_ID, command)


def test_image_command_refuses_a_config_without_command_fields():
    with _inspect_with_config({}):
        with pytest.raises(pf_release_run.ReleaseRunError, match="image has no default command"):
            pf_release_run._image_command(IMAGE)


class _Docker:
    """Stand in for Docker at subprocess.run and keep what Docker keeps.

    A bind mount (-v) shows the host file under the host's owner and mode.
    ``docker cp - CONTAINER:/`` extracts as root: an entry keeps its mode, keeps
    its uid and gid only with -a, and a parent directory the archive does not
    name is made 0755 for root. Starting the container opens each input as the
    image user and fails as the image does when that user may not read one.
    """

    container = "b" * 64
    accounts = {
        "/etc/passwd": b"pineforge:x:10001:20002::/tmp:/bin/sh\n",
        "/etc/group": b"research:x:30003:\n",
    }
    report = {"trades": []}
    host_owner = (1000, 1000)  # the launching account; never one of the image's IDs

    def __init__(self, user):
        self.user = user
        self.calls = []
        self.account_reads = []
        self.upload = None
        self.launch = None
        self.mounts = {}
        self.copied = {}

    def run(self, cmd, **options):
        self.calls.append(cmd)
        stdout = ""
        if cmd[:3] == ["docker", "image", "inspect"]:
            config = {"Cmd": ["/entrypoint.sh"], "User": self.user}
            stdout = json.dumps([{"Id": IMAGE_ID, "Config": config}])
        elif cmd[:3] == ["docker", "container", "inspect"]:
            stdout = IMAGE_ID + "\n"
        elif cmd[1] in ("create", "run"):
            self.launch = cmd
            for option, value in zip(cmd, cmd[1:]):
                if option == "-v":
                    source, target = value.split(":")[:2]
                    self.mounts[target] = source
            if cmd[1] == "run":
                return self._start(cmd)
            stdout = self.container + "\n"
        elif cmd[1] == "cp":
            if cmd[-2] == "-":
                self.upload = options["input"]
                self._extract(keep_owner="-a" in cmd)
            else:
                target, path = cmd[-2].split(":", 1)
                assert target == self.container
                self.account_reads.append(path)
                data = self.accounts.get(path)
                if data is None:
                    message = f"Could not find the file {path} in container {self.container}"
                    return subprocess.CompletedProcess(cmd, 1, stdout=b"", stderr=message.encode())
                with io.BytesIO() as stream:
                    with tarfile.open(fileobj=stream, mode="w") as archive:
                        member = tarfile.TarInfo(Path(path).name)
                        member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
                    stdout = stream.getvalue()
        elif cmd[1] == "start":
            return self._start(cmd)
        else:
            assert cmd == ["docker", "rm", "-f", "-v", self.container]
        stderr = b"" if isinstance(stdout, bytes) else ""
        return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=stderr)

    def _extract(self, keep_owner):
        """Record the owner and mode each archive entry ends with in the container."""
        with tarfile.open(fileobj=io.BytesIO(self.upload), mode="r:") as archive:
            for member in archive:
                path = PurePosixPath("/", member.name)
                for parent in path.parents:
                    if str(parent) != "/":
                        self.copied.setdefault(str(parent), (0, 0, 0o755))
                owner = (member.uid, member.gid) if keep_owner else (0, 0)
                self.copied[str(path)] = (*owner, member.mode)

    def _rows(self, path):
        return [line.split(":") for line in self.accounts.get(path, b"").decode().splitlines()]

    def _process_ids(self):
        """UID and GID of the image command: Config.User against the image's accounts."""
        if not self.user:
            return 0, 0
        user, _, group = self.user.partition(":")
        uid, gid = (int(user), 0) if user.isdigit() else (None, 0)
        for name, _, account_uid, account_gid, *_ in self._rows("/etc/passwd"):
            if (int(account_uid) == uid) if user.isdigit() else (name == user):
                uid, gid = int(account_uid), int(account_gid)
                break
        if group.isdigit():
            gid = int(group)
        elif group:
            gid = next(int(row[2]) for row in self._rows("/etc/group") if row[0] == group)
        return uid, gid

    def _node(self, path):
        """Owner, group and mode the container shows at ``path``; None when absent."""
        if path in self.mounts:
            return (*self.host_owner, stat.S_IMODE(os.stat(self.mounts[path]).st_mode))
        return self.copied.get(path)

    @staticmethod
    def _allows(uid, gid, node, bit):
        """POSIX class check: bit 4 reads a file, bit 1 searches a directory."""
        owner, group, mode = node
        if uid == 0:
            return True  # root reads whatever the mode bits say
        shift = 6 if uid == owner else 3 if gid == group else 0
        return bool((mode >> shift) & bit)

    def _inputs(self):
        """Container paths the image command opens, in its own order."""
        paths = []
        if "--entrypoint" in self.launch:
            paths += [self.launch[-3], self.launch[-1]]  # the staged helper, then its request
        paths += ["/in/strategy.cpp", "/in/ohlcv.csv"]
        paths += [word.partition("=")[2] for word in self.launch
                  if word.startswith("PINEFORGE_SYMINFO=")]
        return paths

    def _start(self, cmd):
        """Open each input as the image user; the first refusal ends the run."""
        uid, gid = self._process_ids()
        for path in self._inputs():
            node = self._node(path)
            folder = self.copied.get(str(PurePosixPath(path).parent), (0, 0, 0o755))
            if (node is not None and self._allows(uid, gid, folder, 1)
                    and self._allows(uid, gid, node, 4)):
                continue
            reason = "Permission denied" if node is not None else "No such file or directory"
            stderr = f"cp: cannot open '{path}' for reading: {reason}\n"
            if path == "/in/strategy.cpp":
                stderr += "[pineforge] cannot snapshot strategy.cpp into the work dir\n"
            return subprocess.CompletedProcess(cmd, 3, stdout="", stderr=stderr)
        stdout = "progress\n" + json.dumps(self.report) + "\n"
        return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr="")

    def staged(self):
        assert self.upload is not None, "expected a tar upload of staged inputs, got none"
        with tarfile.open(fileobj=io.BytesIO(self.upload), mode="r:") as archive:
            return {
                member.name: (member.uid, member.gid, member.mode,
                              archive.extractfile(member).read() if member.isfile() else None)
                for member in archive
            }


@pytest.fixture
def runner_inputs(tmp_path):
    cpp = tmp_path / "generated.cpp"
    cpp.write_text("// generated\n", encoding="utf-8")
    ohlcv = tmp_path / "ohlcv.csv"
    ohlcv.write_text("timestamp,open,high,low,close,volume\n", encoding="utf-8")
    # What umask 077 leaves on the launching host: only the owner may read.
    for path in (cpp, ohlcv):
        path.chmod(0o600)
    return cpp, ohlcv


def _run_with_docker(docker, runner_inputs, **kwargs):
    with mock.patch.object(pf_release_run.subprocess, "run", side_effect=docker.run):
        return pf_release_run.run_release(*runner_inputs, image=IMAGE, **kwargs)


def test_docker_stand_in_refuses_a_host_file_the_image_user_cannot_read(runner_inputs):
    cpp, ohlcv = runner_inputs
    launch = ["docker", "run", "--rm", "-v", f"{cpp}:/in/strategy.cpp:ro",
              "-v", f"{ohlcv}:/in/ohlcv.csv:ro", IMAGE]
    refused = _Docker("10001:10001").run(launch)
    assert (refused.returncode, refused.stderr) == (3, BOX_FAILURE)
    # Root reads through the same mount, so an image without a user still runs.
    assert _Docker("").run(launch).returncode == 0


@pytest.mark.parametrize("user", ["10001", "10001:10001", "pineforge", "pineforge:research"])
@pytest.mark.parametrize("launch", [
    {"corpus_params": {}, "syminfo": {"mincontract": 1}}, {"syminfo": {"mincontract": 1}}, {},
], ids=["corpus-and-metadata", "metadata", "plain"])
def test_release_inputs_are_readable_by_the_image_user(runner_inputs, user, launch):
    # The host files are 0600 and belong to the launching account, not the image's.
    docker = _Docker(user)
    assert _run_with_docker(docker, runner_inputs, **launch) == docker.report


@pytest.mark.parametrize("launch", [
    {"corpus_params": {}, "syminfo": {"mincontract": 1}}, {"syminfo": {"mincontract": 1}}, {},
], ids=["corpus-and-metadata", "metadata", "plain"])
def test_release_stages_every_entry_for_its_owner_only(runner_inputs, launch):
    docker = _Docker("10001:10001")
    # The image user owns each entry (docker cp -a), so it still reads them all.
    assert _run_with_docker(docker, runner_inputs, **launch) == docker.report
    staged = {name: values[2] for name, values in docker.staged().items()}
    assert {"in", "in/strategy.cpp", "in/ohlcv.csv"} <= set(staged)
    # No group or world bit (read, write or search) on any entry of the archive.
    assert {name: oct(mode) for name, mode in staged.items() if mode & ~stat.S_IRWXU} == {}
    # Nor does the container end with a parent directory Docker made for root.
    assert {path: oct(node[2]) for path, node in docker.copied.items()
            if node[2] & ~stat.S_IRWXU} == {}


@pytest.mark.parametrize("user,passwd,gid", [
    ("10001", True, 20002), ("10001", False, 0), ("10001:10001", True, 10001),
], ids=["numeric-passwd", "numeric-no-passwd", "numeric-explicit-group"])
def test_release_copies_inputs_with_image_ownership(runner_inputs, user, passwd, gid):
    docker = _Docker(user)
    if not passwd:
        docker.accounts = {}
    request, syminfo = {"inputs": {"Length": 8}}, {"mincontract": 0.01}
    assert _run_with_docker(docker, runner_inputs, corpus_params=request,
                            syminfo=syminfo) == docker.report
    files = docker.staged()
    # expectation corrected: 0o644 -> 0o400 and 0o755 -> 0o500, because the
    # image user owns every entry (docker cp -a keeps the archive's uid and
    # gid), so no other account is given a read or search bit; in,
    # in/strategy.cpp and in/ohlcv.csv are entries because the strategy and
    # the feed are copied in instead of bind-mounted.
    directory, regular = (10001, gid, 0o500), (10001, gid, 0o400)
    assert {name: values[:3] for name, values in files.items()} == {
        "in": directory,
        "in/strategy.cpp": regular,
        "in/ohlcv.csv": regular,
        "in/syminfo.json": regular,
        "pf-corpus-request": directory,
        "pf-corpus-request/pf_release_run.py": regular,
        "pf-corpus-request/run_strategy.py": regular,
        "pf-corpus-request/derive_corpus_feeds.py": regular,
        "pf-corpus-request/parity_paths.py": regular,
        "pf-corpus-request/corpus-params.json": regular,
    }
    # What the container ends with: -a kept each entry's owner beside its mode.
    assert docker.copied == {"/" + name: values[:3] for name, values in files.items()}
    cpp, ohlcv = runner_inputs
    assert files["in/strategy.cpp"][3] == cpp.read_bytes()
    assert files["in/ohlcv.csv"][3] == ohlcv.read_bytes()
    assert json.loads(files["pf-corpus-request/corpus-params.json"][3]) == request
    assert json.loads(files["in/syminfo.json"][3]) == syminfo
    assert ["docker", "cp", "-a", "-", f"{docker.container}:/"] in docker.calls
    create = next(cmd for cmd in docker.calls if cmd[1] == "create")
    assert "PINEFORGE_SYMINFO=/in/syminfo.json" in create
    assert create[-6:-2] == [
        "--entrypoint", "python3", IMAGE_ID, "/pf-corpus-request/pf_release_run.py",
    ]
    assert json.loads(create[-2]) == ["/entrypoint.sh"]
    assert create[-1] == "/pf-corpus-request/corpus-params.json"
    assert docker.calls[-2:] == [
        ["docker", "start", "-a", docker.container],
        ["docker", "rm", "-f", "-v", docker.container],
    ]


def test_release_keeps_staged_inputs_off_host_mounts(runner_inputs):
    docker = _Docker("10001:10001")
    with mock.patch.object(pf_release_run.os, "chmod") as chmod:
        _run_with_docker(docker, runner_inputs, corpus_params={}, syminfo={"mincontract": 1})
    (launch,) = [cmd for cmd in docker.calls if cmd[1] in ("run", "create")]
    mounts = [value for option, value in zip(launch, launch[1:])
              if option in ("-v", "--volume", "--mount")]
    # expectation corrected: the strategy and feed bind mounts -> no mount,
    # because a bind mount shows the image user the host's owner and mode.
    assert mounts == []
    assert not any(word.startswith(("--volume=", "--mount=")) for word in launch)
    assert not any(word in ("-u", "--user") or word.startswith("--user=") for word in launch)
    chmod.assert_not_called()


def test_release_accepts_empty_image_user_as_root(runner_inputs):
    docker = _Docker("")
    assert _run_with_docker(docker, runner_inputs, syminfo={"mincontract": 1}) == docker.report
    # expectation corrected: 0o644 -> 0o400 and 0o755 -> 0o500, for the reason
    # given in the ownership test; in, in/strategy.cpp and in/ohlcv.csv are
    # entries for the reason given there too.
    assert {name: values[:3] for name, values in docker.staged().items()} == {
        "in": (0, 0, 0o500),
        "in/strategy.cpp": (0, 0, 0o400),
        "in/ohlcv.csv": (0, 0, 0o400),
        "in/syminfo.json": (0, 0, 0o400),
    }
    assert docker.account_reads == []


@pytest.mark.parametrize("user,gid,accounts", [
    ("pineforge", 20002, ["/etc/passwd"]),
    ("pineforge:research", 30003, ["/etc/passwd", "/etc/group"]),
])
def test_release_resolves_named_image_user(runner_inputs, user, gid, accounts):
    docker = _Docker(user)
    assert _run_with_docker(docker, runner_inputs, corpus_params={}) == docker.report
    assert docker.account_reads == accounts
    assert {values[:2] for values in docker.staged().values()} == {(10001, gid)}


@pytest.mark.parametrize("user,accounts", [
    ("missing-user", ["/etc/passwd"]),
    ("pineforge:missing-group", ["/etc/passwd", "/etc/group"]),
])
def test_release_refuses_unknown_image_user(runner_inputs, user, accounts):
    docker = _Docker(user)
    with pytest.raises(pf_release_run.ReleaseRunError, match=rf"Config\.User.*{user}"):
        _run_with_docker(docker, runner_inputs, corpus_params={})
    assert docker.account_reads == accounts
    assert docker.upload is None
    assert docker.calls[-1] == ["docker", "rm", "-f", "-v", docker.container]
