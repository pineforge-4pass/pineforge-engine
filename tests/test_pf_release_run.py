"""Tests for inspecting the release image and staging its Docker inputs."""

import io
import json
from pathlib import Path
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
    """Stand in for Docker at subprocess.run, including its tar streams."""

    container = "b" * 64
    accounts = {
        "/etc/passwd": b"pineforge:x:10001:20002::/tmp:/bin/sh\n",
        "/etc/group": b"research:x:30003:\n",
    }
    report = {"trades": []}

    def __init__(self, user):
        self.user = user
        self.calls = []
        self.account_reads = []
        self.upload = None

    def run(self, cmd, **options):
        self.calls.append(cmd)
        stdout = ""
        if cmd[:3] == ["docker", "image", "inspect"]:
            config = {"Cmd": ["/entrypoint.sh"], "User": self.user}
            stdout = json.dumps([{"Id": IMAGE_ID, "Config": config}])
        elif cmd[:3] == ["docker", "container", "inspect"]:
            stdout = IMAGE_ID + "\n"
        elif cmd[1] == "create":
            stdout = self.container + "\n"
        elif cmd[1] == "cp":
            if cmd[-2] == "-":
                self.upload = options["input"]
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
        elif cmd[1] in ("start", "run"):
            stdout = "progress\n" + json.dumps(self.report) + "\n"
        else:
            assert cmd == ["docker", "rm", "-f", "-v", self.container]
        stderr = b"" if isinstance(stdout, bytes) else ""
        return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=stderr)

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
    return cpp, ohlcv


def _run_with_docker(docker, runner_inputs, **kwargs):
    with mock.patch.object(pf_release_run.subprocess, "run", side_effect=docker.run):
        return pf_release_run.run_release(*runner_inputs, image=IMAGE, **kwargs)


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
    assert {name: values[:3] for name, values in files.items()} == {
        "pf-corpus-request": (10001, gid, 0o500),
        "pf-corpus-request/pf_release_run.py": (10001, gid, 0o400),
        "pf-corpus-request/run_strategy.py": (10001, gid, 0o400),
        "pf-corpus-request/derive_corpus_feeds.py": (10001, gid, 0o400),
        "pf-corpus-request/parity_paths.py": (10001, gid, 0o400),
        "pf-corpus-request/corpus-params.json": (10001, gid, 0o400),
        "in/syminfo.json": (10001, gid, 0o400),
    }
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
    cpp, ohlcv = runner_inputs
    mounts = [value for option, value in zip(launch, launch[1:]) if option == "-v"]
    assert mounts == [f"{cpp.resolve()}:/in/strategy.cpp:ro", f"{ohlcv.resolve()}:/in/ohlcv.csv:ro"]
    assert not any(word in ("-u", "--user") or word.startswith("--user=") for word in launch)
    chmod.assert_not_called()


def test_release_accepts_empty_image_user_as_root(runner_inputs):
    docker = _Docker("")
    assert _run_with_docker(docker, runner_inputs, syminfo={"mincontract": 1}) == docker.report
    assert {name: values[:3] for name, values in docker.staged().items()} == {
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
