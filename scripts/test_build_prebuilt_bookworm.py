#!/usr/bin/env python3
"""Recording-Docker transport controls, never evidence of an engine build.

Run on Linux with --scratch-root /explicit/path. Fixtures are retained.
The private helper copy changes only the shared-path root and uses a recording
id/Docker protocol; no image, compiler, ELF inspection, or network is invoked.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

import ci_preflight


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts/build_prebuilt_bookworm.sh"
IMAGE_DIGEST = "sha256:72fb3f2a4d3409e6050b17530585992784a21a4955b368cb8f8b392981abc780"
IMAGE_CONFIG_ID = "sha256:9b1e6c557f68a8b0f1577293d95aae135ade1f9c5499d817f6df100d1cf616bd"
IMAGE = "example.invalid/toolchains/engine-bookworm@" + IMAGE_DIGEST
SCRATCH: Path | None = None


def recording_command() -> int:
    """Explicit test-only subprocess protocol; it cannot build an engine."""
    root = Path(os.environ["BOOKWORM_RECORDING_ROOT"])
    command, *args = sys.argv[2:]
    with (root / "commands.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps([command, *args]) + "\n")
    controls = json.loads((root / "controls.json").read_text())
    if command == "id":
        print({"-u": controls.get("host_uid", "1001"),
               "-g": controls.get("host_gid", "123")}[args[0]])
        return 0
    if args == ["images", "--digests", "--no-trunc"]:
        print("RECORDING FIXTURE: no images inspected")
    elif args == ["info", "--format", "{{.Driver}}"]:
        print("overlayfs")
    elif args == ["version", "--format", "{{.Client.Version}} {{.Server.Version}}"]:
        print(controls.get("versions", "29.9.2 29.8.1"))
        return controls.get("version_rc", 0)
    # Record the reference the real helper supplies, including an old helper's
    # hardcoded reference. Old-source RED must be a failed refusal assertion,
    # never an unknown recording protocol or fixture setup failure.
    elif len(args) == 3 and args[:2] == ["image", "inspect"]:
        if controls.get("inspect_rc"):
            print("RECORDING FIXTURE: digest absent", file=sys.stderr)
            return controls["inspect_rc"]
        print(json.dumps([{"Os": "linux", "Architecture": "amd64",
                           "RepoDigests": [args[-1]],
                           "Id": controls.get("id", IMAGE_CONFIG_ID)}]))
    elif len(args) == 5 and args[:3] == ["image", "inspect", "--format"]:
        if args[3] == "{{.Id}}":
            if controls.get("id_rc"):
                print("RECORDING FIXTURE: image Id inspect failed", file=sys.stderr)
                return controls["id_rc"]
            print(controls.get("id", IMAGE_CONFIG_ID))
        elif args[3] == "{{.Os}}/{{.Architecture}}":
            print(controls.get("arch", "linux/amd64"))
        elif args[3] == "{{range .RepoDigests}}{{println .}}{{end}}":
            print(controls.get("digest", args[-1]))
        else:
            raise AssertionError(f"unexpected inspect format: {args!r}")
    elif args and args[0] == "run":
        if controls.get("run_rc"):
            child = "consumer" if "--inside-consume" in args else "builder"
            print(f"RECORDING FIXTURE: {child} child failed", file=sys.stderr)
            return controls["run_rc"]
        mounts = [args[index + 1] for index, value in enumerate(args) if value == "--mount"]
        source = Path(next(value.split(",")[1].removeprefix("source=")
                           for value in mounts if "target=/source," in value))
        output = Path(next(value.split(",")[1].removeprefix("source=")
                           for value in mounts if value.endswith("target=/output")))
        if "--inside-consume" in args:
            (root / "consumer-inputs.json").write_text(json.dumps({
                str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in source.rglob("*") if path.is_file()
            }))
            print("RECORDING FIXTURE: consumer transport only; no compatibility verdict")
            return 0
        (root / "source-observed.json").write_text(json.dumps({
            "git_metadata": (source / ".git").exists(),
            "untracked": (source / "untracked.txt").exists(),
            "tracked": (source / "tracked.txt").read_text(),
            "version": (source / "VERSION").read_text(),
            "identity": (output / "identity.cmake").read_text(),
        }))
        # A directory satisfies the transport postcondition only. No fake ELF,
        # version-header, checker verdict or archive is produced.
        (output / f"stage/pineforge-{args[-1]}-linux-x86_64").mkdir(parents=True)
        print("RECORDING FIXTURE: transport only; no build or floor verdict")
    else:
        raise AssertionError(f"unexpected recording protocol: {args!r}")
    return 0


class BookwormTransport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SCRATCH is None:
            raise AssertionError("--scratch-root is required; no implicit /tmp fixtures")
        if not SCRATCH.is_absolute() or ".." in SCRATCH.parts:
            raise AssertionError("scratch root must be an absolute normalized path")
        current = Path(SCRATCH.anchor)
        for part in SCRATCH.parts[1:]:
            current /= part
            if current.is_symlink():
                raise AssertionError(f"scratch ancestor symlink: {current}")
        SCRATCH.mkdir(parents=True, exist_ok=True)
        if not HELPER.is_file() or HELPER.is_symlink():
            raise AssertionError(f"regular helper missing: {HELPER}")

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="bookworm-transport-", dir=SCRATCH))
        print(json.dumps({"fixture_root": str(self.root)}), flush=True)
        self.shared = self.root / "shared"
        self.workspace = self.shared / "engine/engine"
        self.temp = self.shared / "_temp"
        self.workspace.mkdir(parents=True)
        self.temp.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.env_file = self.temp / "github-env"
        self.env_file.touch()
        self.env = {**os.environ, "LC_ALL": "C", "LANG": "C",
                    "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
                    "GIT_AUTHOR_NAME": "Transport fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
                    "GIT_COMMITTER_NAME": "Transport fixture",
                    "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
                    "BOOKWORM_RECORDING_ROOT": str(self.root),
                    "GITHUB_ENV": str(self.env_file),
                    "PATH": str(self.bin) + os.pathsep + os.defpath}
        (self.root / "controls.json").write_text("{}")
        for name in ("docker", "id"):
            path = self.bin / name
            path.write_text("#!/bin/sh\nexec " + shlex.quote(sys.executable) + " "
                            + shlex.quote(str(Path(__file__).resolve()))
                            + " --recording-command " + name + ' "$@"\n')
            path.chmod(0o755)
        text = HELPER.read_text()
        roots = [line for line in text.splitlines() if line.startswith("readonly shared_root=")]
        self.assertEqual(len(roots), 1)
        needle = roots[0]
        self.assertIn(needle, ("readonly shared_root=/home/runner/work",
                               "readonly shared_root=/home/runner/_work"))
        self.assertEqual(text.count(needle), 1)
        self.helper = self.root / "transport-helper.sh"
        self.helper.write_text(text.replace(needle, "readonly shared_root=" + shlex.quote(str(self.shared))))
        self.git("init", "--quiet")
        files = {"VERSION": "1.10.0\n", "tracked.txt": "exact committed bytes\n",
                 "CMakeLists.txt": "# transport fixture only\n",
                 "cmake/PineForgeVersion.cmake": "# transport fixture only\n",
                 "scripts/build_prebuilt_bookworm.sh": "# transport fixture only; never executed\n",
                 "scripts/release_version.py": "# transport fixture only; never executed\n"}
        for name, content in files.items():
            (self.workspace / name).parent.mkdir(parents=True, exist_ok=True)
            (self.workspace / name).write_text(content)
        tree = self.make_tree(files)
        self.sha = self.git("commit-tree", tree, data="transport fixture\n").strip()
        self.tree = tree
        self.git("update-ref", "HEAD", self.sha)
        self.git("read-tree", self.sha)
        self.git("tag", "v1.10.0", self.sha)
        self.git("tag", "v1.11.0-rc.2", self.sha)
        (self.workspace / "VERSION").write_text("1.11.0-rc.2\n")
        (self.workspace / "untracked.txt").write_text("must never be exported\n")

    def git(self, *args, data=None):
        ran = subprocess.run(["git", "-C", str(self.workspace), *args], input=data,
                             text=True, capture_output=True, env=self.env)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        return ran.stdout

    def make_tree(self, files):
        rows = []
        directories = {}
        for name, content in sorted(files.items()):
            if "/" in name:
                directory, remaining = name.split("/", 1)
                directories.setdefault(directory, {})[remaining] = content
            else:
                oid = self.git("hash-object", "-w", "--stdin", data=content).strip()
                rows.append(f"100644 blob {oid}\t{name}\n")
        for name, children in sorted(directories.items()):
            rows.append(f"040000 tree {self.make_tree(children)}\t{name}\n")
        return self.git("mktree", data="".join(rows)).strip()

    def invoke(self, *, sha=None, version="1.11.0-rc.2", tag="v1.11.0-rc.2",
               dry="true", workspace=None, temp=None, controls=None, image=IMAGE):
        (self.root / "controls.json").write_text(json.dumps(controls or {}))
        return subprocess.run(
            ["bash", str(self.helper), str(workspace or self.workspace), str(temp or self.temp),
             sha or self.sha, version, tag, dry],
            env={**self.env, "ENGINE_TOOLCHAIN_IMAGE": image}, text=True, capture_output=True)

    def commands(self):
        path = self.root / "commands.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def assert_refused(self, ran, fragment):
        self.assertNotEqual(ran.returncode, 0, ran.stdout)
        self.assertIn(fragment, ran.stderr)
        self.assertFalse(any(row[1:2] == ["run"] for row in self.commands()))
        self.assertEqual(self.env_file.read_text(), "")

    def test_transport_exports_exact_source_without_checkout_metadata_or_untracked_files(self):
        ran = self.invoke()
        self.assertEqual(ran.returncode, 0, ran.stderr)
        observed = json.loads((self.root / "source-observed.json").read_text())
        self.assertEqual(observed, {
            "git_metadata": False, "untracked": False, "tracked": "exact committed bytes\n",
            "version": "1.11.0-rc.2\n",
            "identity": f'set(PINEFORGE_VERSION_GIT_SHA "{self.sha[:7]}")\n'
                        'set(PINEFORGE_VERSION_DIRTY ON)\n',
        })
        run = next(row[1:] for row in self.commands() if row[1:2] == ["run"])
        for pair in (["--pull=never"], ["--platform", "linux/amd64"],
                     ["--network", "none"], ["--user", "1001:123"], ["--read-only"],
                     ["--cap-drop", "ALL"], ["--security-opt", "no-new-privileges"]):
            start = run.index(pair[0])
            self.assertEqual(run[start:start + len(pair)], pair)
        self.assertEqual(run.count("--mount"), 2)
        mounts = [run[index + 1] for index, arg in enumerate(run) if arg == "--mount"]
        self.assertTrue(any("target=/source,readonly" in item for item in mounts))
        self.assertTrue(all(str(self.temp) in item for item in mounts))
        self.assertIn(IMAGE, run)
        self.assertFalse(any(arg in run for arg in ("--privileged", "-v", "--volume")))
        self.assertIn("STAGE_DIR=", self.env_file.read_text())
        self.assertIn(self.tree, (next(self.temp.glob("prebuilt-bookworm.*"))
                                 / "evidence/source-identity.txt").read_text())

    def test_absent_digest_has_named_canary_dependency_and_preserves_child_status(self):
        ran = self.invoke(controls={"inspect_rc": 17})
        self.assertEqual(ran.returncode, 17, ran.stderr)
        self.assert_refused(ran, "same-daemon preload canary")
        self.assertFalse(any("pull" in row or "login" in row for row in self.commands()))

    def test_empty_engine_toolchain_image_is_refused_before_docker(self):
        ran = self.invoke(image="")
        self.assertEqual(ran.returncode, 1, ran.stderr)
        self.assert_refused(ran, "ENGINE_TOOLCHAIN_IMAGE is required")
        self.assertFalse(any(row[0] == "docker" for row in self.commands()))

    def test_wrong_exact_digest_suffix_is_refused_before_docker(self):
        repository = IMAGE.split("@", 1)[0]
        for image in (repository + ":latest", repository + "@sha256:" + "0" * 64,
                      IMAGE + "suffix", IMAGE + "\n", "@" + IMAGE_DIGEST):
            with self.subTest(image=image):
                ran = self.invoke(image=image)
                self.assertEqual(ran.returncode, 1, ran.stderr)
                self.assert_refused(ran, "ENGINE_TOOLCHAIN_IMAGE must be a repository@")
                self.assertFalse(any(row[0] == "docker" for row in self.commands()))

    def test_actual_inspected_image_id_must_equal_pinned_digest(self):
        # The supplied build log records this config ID; the registry pin is
        # a distinct manifest. This remains a recording protocol, not a pull.
        self.assertNotEqual(IMAGE_CONFIG_ID, IMAGE_DIGEST)
        for consume in (False, True):
            if consume:
                archive, checksum = self.consumer_inputs()
            for image_id in ("sha256:" + "a" * 64, "", IMAGE_DIGEST + "suffix",
                             IMAGE_DIGEST, IMAGE_CONFIG_ID + "suffix"):
                with self.subTest(image_id=image_id, consume=consume):
                    previous = set(self.temp.iterdir())
                    before_commands = len(self.commands())
                    before_environment = self.env_file.read_text()
                    controls = {"id": image_id}
                    ran = (self.consume(archive, checksum, controls=controls) if consume
                           else self.invoke(controls=controls))
                    self.assertEqual(ran.returncode, 1, ran.stderr)
                    self.assertIn("preloaded image Id differs from pinned config ID", ran.stderr)
                    commands = self.commands()[before_commands:]
                    self.assertFalse(any(row[1:2] == ["run"] for row in commands))
                    self.assertEqual(self.env_file.read_text(), before_environment)
                    self.assertIn(["docker", "image", "inspect", "--format", "{{.Id}}", IMAGE],
                                  commands)
                    created = set(self.temp.iterdir()) - previous
                    self.assertEqual(len(created), 1)
                    evidence = created.pop() / "evidence"
                    self.assertEqual((evidence / "docker-image-id.txt").read_text(), image_id + "\n")
                    self.assertEqual((evidence / "docker-image-id.exitcode").read_text(), "0\n")
            with self.subTest(accepted_config_id=IMAGE_CONFIG_ID, consume=consume):
                previous = set(self.temp.iterdir())
                before_commands = len(self.commands())
                controls = {"id": IMAGE_CONFIG_ID, "digest": IMAGE}
                ran = (self.consume(archive, checksum, controls=controls) if consume
                       else self.invoke(controls=controls))
                self.assertEqual(ran.returncode, 0, ran.stderr)
                created = set(self.temp.iterdir()) - previous
                self.assertEqual(len(created), 1)
                evidence = created.pop() / "evidence"
                self.assertEqual((evidence / "docker-image-id.txt").read_text(), IMAGE_CONFIG_ID + "\n")
                self.assertEqual((evidence / "docker-image-id.stderr").read_text(), "")
                self.assertEqual((evidence / "docker-image-id.exitcode").read_text(), "0\n")
                inspected = json.loads((evidence / "docker-image-inspect.json").read_text())
                self.assertEqual(inspected[0]["Id"], IMAGE_CONFIG_ID)
                self.assertEqual(inspected[0]["RepoDigests"], [IMAGE])
                self.assertEqual((evidence / "docker-repo-digests.txt").read_text(), IMAGE + "\n")
                commands = self.commands()[before_commands:]
                inspects = [row for row in commands if row[1:3] == ["image", "inspect"]]
                self.assertEqual(len(inspects), 4)
                self.assertTrue(all(row[-1] == IMAGE for row in inspects), inspects)
                run = next(row for row in commands if row[1:2] == ["run"])
                self.assertIn(IMAGE, run)
                self.assertNotIn(IMAGE_CONFIG_ID, run)

    def test_image_id_inspection_failure_preserves_child_status_and_raw_output(self):
        ran = self.invoke(controls={"id_rc": 19})
        self.assertEqual(ran.returncode, 19, ran.stderr)
        self.assert_refused(ran, "RECORDING FIXTURE: image Id inspect failed")
        evidence = next(self.temp.glob("prebuilt-bookworm.*")) / "evidence"
        self.assertEqual((evidence / "docker-image-id.txt").read_text(), "")
        self.assertEqual((evidence / "docker-image-id.stderr").read_text(),
                         "RECORDING FIXTURE: image Id inspect failed\n")
        self.assertEqual((evidence / "docker-image-id.exitcode").read_text(), "19\n")

    def test_variable_reference_is_consumed_by_every_inspect_and_run(self):
        alternate = "example.invalid/alternate/engine-bookworm@" + IMAGE_DIGEST
        ran = self.invoke(image=alternate)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        inspects = [row for row in self.commands() if row[1:3] == ["image", "inspect"]]
        self.assertEqual(len(inspects), 4)
        self.assertTrue(all(row[-1] == alternate for row in inspects), inspects)
        run = next(row for row in self.commands() if row[1:2] == ["run"])
        self.assertIn(alternate, run)
        self.assertNotIn(IMAGE, run)
        self.assertNotIn("--env", run)
        evidence = next(self.temp.glob("prebuilt-bookworm.*")) / "evidence"
        self.assertIn("image=" + alternate + "\n", (evidence / "source-identity.txt").read_text())
        self.assertEqual((evidence / "docker-image-id.txt").read_text(), IMAGE_CONFIG_ID + "\n")

    def test_empty_or_wrong_repository_digest_and_architecture_are_refused(self):
        for consume in (False, True):
            if consume:
                archive, checksum = self.consumer_inputs()
            for controls in ({"digest": ""}, {"digest": "example:latest"},
                             {"digest": IMAGE.split("@", 1)[0] + "@" + IMAGE_CONFIG_ID},
                             {"arch": "linux/arm64"}):
                with self.subTest(controls=controls, consume=consume):
                    ran = (self.consume(archive, checksum, controls=controls) if consume
                           else self.invoke(controls=controls))
                    self.assert_refused(ran, "prebuilt-bookworm:")

    def test_observed_docker_versions_are_recorded_for_both_transports(self):
        for consume in (False, True):
            if consume:
                archive, checksum = self.consumer_inputs()
            for versions in ("28.0.4 27.5.1", "29.9.2 28.0.0"):
                with self.subTest(versions=versions, consume=consume):
                    before = set(self.temp.iterdir())
                    controls = {"versions": versions}
                    ran = (self.consume(archive, checksum, controls=controls) if consume
                           else self.invoke(controls=controls))
                    self.assertEqual(ran.returncode, 0, ran.stderr)
                    evidence = (set(self.temp.iterdir()) - before).pop() / "evidence"
                    self.assertEqual((evidence / "docker-versions.txt").read_bytes(),
                                     (versions + "\n").encode())

    def test_host_gid_is_recorded_without_restricting_it_in_both_transports(self):
        for consume in (False, True):
            if consume:
                archive, checksum = self.consumer_inputs()
            for gid in ("999", "456"):
                with self.subTest(gid=gid, consume=consume):
                    before = set(self.temp.iterdir())
                    controls = {"host_gid": gid}
                    ran = (self.consume(archive, checksum, controls=controls) if consume
                           else self.invoke(controls=controls))
                    self.assertEqual(ran.returncode, 0, ran.stderr)
                    evidence = (set(self.temp.iterdir()) - before).pop() / "evidence"
                    self.assertEqual((evidence / "host-identity.txt").read_text(),
                                     "uid=1001\ngid=" + gid + "\n")

    def test_wrong_host_uid_refuses_both_transports_before_docker(self):
        archive, checksum = self.consumer_inputs()
        for uid in ("0", "1000"):
            for consume in (False, True):
                with self.subTest(uid=uid, consume=consume):
                    controls = {"host_uid": uid}
                    ran = (self.consume(archive, checksum, controls=controls) if consume
                           else self.invoke(controls=controls))
                    self.assert_refused(ran, "UID must be 1001")
                    self.assertFalse(any(row[0] == "docker" for row in self.commands()))

    def test_docker_version_failure_preserves_status_and_observed_bytes(self):
        ran = self.invoke(controls={"versions": "client-only diagnostic", "version_rc": 31})
        self.assertEqual(ran.returncode, 31, ran.stderr)
        self.assertFalse(any(row[1:2] == ["run"] for row in self.commands()))
        evidence = next(self.temp.glob("prebuilt-bookworm.*")) / "evidence"
        self.assertEqual((evidence / "docker-versions.txt").read_bytes(),
                         b"client-only diagnostic\n")

    def test_builder_failure_preserves_status_and_raw_output_without_host_fallback(self):
        ran = self.invoke(controls={"run_rc": 23})
        self.assertEqual(ran.returncode, 23, ran.stderr)
        self.assertIn("RECORDING FIXTURE: builder child failed", ran.stderr)
        self.assertEqual(self.env_file.read_text(), "")
        self.assertEqual(sum(row[1:2] == ["run"] for row in self.commands()), 1)

    def test_wrong_head_and_invalid_version_tag_and_mode_are_refused_before_docker(self):
        for kwargs in ({"sha": "a" * 40}, {"sha": "bad"}, {"version": "../bad"},
                       {"tag": "v1.12.0"}, {"dry": "maybe"},
                       {"version": "1.11.0-rc.0", "tag": "v1.11.0-rc.0"}):
            with self.subTest(kwargs=kwargs):
                self.assert_refused(self.invoke(**kwargs), "prebuilt-bookworm:")
                self.assertFalse(any(row[0] == "docker" for row in self.commands()))

    def test_publish_rejects_a_version_not_in_the_emitted_commit(self):
        self.assert_refused(self.invoke(dry="false"), "committed VERSION")

    def test_publish_transport_uses_the_committed_version_and_emitted_identity(self):
        (self.workspace / "VERSION").write_text("1.10.0\n")
        ran = self.invoke(dry="false", version="1.10.0", tag="v1.10.0")
        self.assertEqual(ran.returncode, 0, ran.stderr)
        observed = json.loads((self.root / "source-observed.json").read_text())
        self.assertEqual(observed["version"], "1.10.0\n")
        self.assertIn(f'"{self.sha[:7]}"', observed["identity"])
        self.assertIn("PINEFORGE_VERSION_DIRTY OFF", observed["identity"])

    def test_dry_run_checkout_version_must_equal_the_validated_override(self):
        (self.workspace / "VERSION").write_text("9.9.9\n")
        self.assert_refused(self.invoke(), "validated release version")

    def add_gitlink(self, name):
        listing = self.git("ls-tree", self.tree)
        tree = self.git("mktree", data=listing + f"160000 commit {self.sha}\t{name}\n").strip()
        sha = self.git("commit-tree", tree, "-p", self.sha, data="gitlink transport fixture\n").strip()
        self.git("update-ref", "HEAD", sha)
        self.git("read-tree", sha)
        self.sha = sha

    def test_gitlink_is_recorded_as_commit_and_empty_directory_without_fetching(self):
        (self.workspace / "data").mkdir()
        linked_sha = self.sha
        self.add_gitlink("data")
        ran = self.invoke()
        self.assertEqual(ran.returncode, 0, ran.stderr)
        run_root = next(self.temp.glob("prebuilt-bookworm.*"))
        self.assertEqual((run_root / "evidence/gitlinks.txt").read_text(),
                         f"{linked_sha} data\n")
        self.assertTrue((run_root / "source/data").is_dir())
        self.assertEqual(list((run_root / "source/data").iterdir()), [])

    def test_gitlink_file_symlink_and_missing_directory_are_refused(self):
        (self.workspace / "data-file").write_text("not a directory\n")
        (self.workspace / "data-link").symlink_to(self.temp, target_is_directory=True)
        for name in ("data-file", "data-link", "data-missing"):
            with self.subTest(name=name):
                self.add_gitlink(name)
                self.assert_refused(self.invoke(), "prebuilt-bookworm:")

    def test_fresh_build_output_is_used_for_each_transport_invocation(self):
        first = self.invoke()
        second = self.invoke()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        mounts = [[row[index + 1] for index, arg in enumerate(row) if arg == "--mount"]
                  for row in self.commands() if row[1:2] == ["run"]]
        self.assertEqual(len(mounts), 2)
        self.assertNotEqual(mounts[0], mounts[1])

    def test_checkout_edits_besides_version_are_refused(self):
        (self.workspace / "tracked.txt").write_text("edited\n")
        self.assert_refused(self.invoke(), "tracked checkout differs")

    def test_relative_outside_missing_and_symlink_mount_inputs_are_refused(self):
        link = self.shared / "workspace-link"
        link.symlink_to(self.workspace, target_is_directory=True)
        for kwargs in ({"workspace": Path("relative")}, {"temp": self.root},
                       {"workspace": self.shared / "missing"}, {"workspace": link},
                       {"temp": self.temp / "../_temp"}):
            with self.subTest(kwargs=kwargs):
                self.assert_refused(self.invoke(**kwargs), "prebuilt-bookworm:")

    def test_export_refuses_symlink_blobs_and_protected_names(self):
        for mode, name in (("120000", "linked"), ("100644", ".env.fixture")):
            with self.subTest(mode=mode, name=name):
                oid = self.git("hash-object", "-w", "--stdin", data="/outside\n").strip()
                tree = self.git("mktree", data=f"{mode} blob {oid}\t{name}\n").strip()
                sha = self.git("commit-tree", tree, data="unsafe export fixture\n").strip()
                self.git("update-ref", "HEAD", sha)
                self.git("read-tree", sha)
                # This also differs from the committed checkout. Either boundary
                # must refuse before Docker; it must never follow the link.
                self.assert_refused(self.invoke(sha=sha), "prebuilt-bookworm:")

    def test_helper_pin_consumes_changed_source_and_refuses_unsafe_transport_mutations(self):
        source = HELPER.read_text()
        self.assertEqual(ci_preflight.release_prebuilt_helper_findings(source), [])
        for before, after in (
                ("readonly image=${ENGINE_TOOLCHAIN_IMAGE:-}",
                 "readonly image=example.invalid/toolchains/engine-bookworm:latest"),
                (IMAGE_DIGEST, IMAGE_DIGEST[:-1] + "1"),
                (IMAGE_CONFIG_ID, IMAGE_CONFIG_ID[:-1] + "1"),
                ('|| fail "preloaded image Id differs from pinned config ID"', '|| true'),
                ("{{.Id}}", "{{.RepoDigests}}"),
                ("--pull=never", "--pull=always"),
                ("--platform linux/amd64", "--platform linux/arm64"),
                ("--network none", "--network host"),
                ("--user 1001:123", "--user 0:0"),
                ("--user 1001:123", "--user 1001:999"),
                ("--cap-drop ALL", "--privileged"),
                ("target=/source,readonly", "target=/source"),
                ("target=/output", "target=/var/run/docker.sock"),
                ("source=$source_dir", "source=$workspace/.git"),
                ("CMAKE_PROJECT_INCLUDE", "CMAKE_PROJECT_INCLUDE_BEFORE"),
                ("Eigen3_DIR=/usr/share/eigen3/cmake", "Eigen3_DIR=/host/eigen"),
                ("PINEFORGE_VERSION_SOURCE=FILE", "PINEFORGE_VERSION_SOURCE=AUTO"),
                ("FETCHCONTENT_FULLY_DISCONNECTED=ON", "FETCHCONTENT_FULLY_DISCONNECTED=OFF"),
                ("EIGEN_MINOR_VERSION[[:space:]]+0", "EIGEN_MINOR_VERSION[[:space:]]+1"),
                ("cmake --install", "true # cmake --install"),
                ("exit \"$build_rc\"", "exit 0"),
        ):
            with self.subTest(before=before):
                self.assertIn(before, source)
                changed = source.replace(before, after, 1)
                self.assertNotEqual(changed, source)
                self.assertTrue(ci_preflight.release_prebuilt_helper_findings(changed))
        for addition in ("\ndocker pull example:latest\n", "\ndocker login example\n",
                         "\ncmake -B host-build\n", "\ntrue\n"):
            self.assertTrue(ci_preflight.release_prebuilt_helper_findings(source + addition))

    def test_transport_controls_are_registered_in_fast_and_full_preflight(self):
        with mock.patch.object(ci_preflight.sys, "platform", "linux"):
            for tier in ("fast", "all"):
                plan = {entry[0]: entry for entry in ci_preflight.check_commands(ROOT, tier=tier)}
                entry = plan["prebuilt-bookworm-tests"]
                self.assertEqual(entry[1], [sys.executable, str(HELPER.with_name(
                    "test_build_prebuilt_bookworm.py")), "--scratch-root", str(ROOT / "build-ci-preflight")])
                self.assertIs(entry.parallel, True)
            for tier in ("slow", "github-only"):
                self.assertNotIn("prebuilt-bookworm-tests",
                                 dict(ci_preflight.check_commands(ROOT, tier=tier)))
        for platform in ("darwin", "win32"):
            with mock.patch.object(ci_preflight.sys, "platform", platform):
                for tier in ("fast", "all"):
                    self.assertNotIn("prebuilt-bookworm-tests",
                                     dict(ci_preflight.check_commands(ROOT, tier=tier)))

    def consumer_inputs(self):
        # A real tar stream exercises byte transport only. It intentionally
        # contains no library and the recording Docker never runs a consumer.
        archive = self.workspace / "pineforge-v1.11.0-rc.2-linux-x86_64.tar.gz"
        with tarfile.open(archive, "w:gz") as packed:
            packed.add(self.workspace / "tracked.txt", arcname="transport-input.txt")
        checksum = archive.with_name(archive.name + ".sha256")
        checksum.write_text(hashlib.sha256(archive.read_bytes()).hexdigest()
                            + "  " + archive.name + "\n")
        for relative in ("scripts/build_prebuilt_bookworm.sh",
                         "scripts/check_shipped_archive_runtime.sh",
                         "examples/native/hello_kernel.cpp"):
            target = self.workspace / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / relative).read_bytes())
        return archive, checksum

    def consume(self, archive, checksum, *, controls=None):
        (self.root / "controls.json").write_text(json.dumps(controls or {}))
        return subprocess.run(
            ["bash", str(self.helper), "--consume", str(self.workspace), str(self.temp),
             archive.name, checksum.name, "v1.11.0-rc.2"],
            env={**self.env, "ENGINE_TOOLCHAIN_IMAGE": IMAGE}, text=True, capture_output=True)

    def test_archive_consumer_exports_exact_packed_inputs_without_checkout_or_build_tree(self):
        archive, checksum = self.consumer_inputs()
        ran = self.consume(archive, checksum)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        paths = (archive, checksum, self.workspace / "scripts/build_prebuilt_bookworm.sh",
                 self.workspace / "scripts/check_shipped_archive_runtime.sh",
                 self.workspace / "examples/native/hello_kernel.cpp")
        observed = json.loads((self.root / "consumer-inputs.json").read_text())
        self.assertEqual(observed, {
            str(path.relative_to(self.workspace)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in paths
        })
        run = next(row for row in self.commands() if row[1:2] == ["run"])
        self.assertEqual(run[-2:], ["--inside-consume", "v1.11.0-rc.2"])
        for token in ("--pull=never", "--read-only", IMAGE):
            self.assertIn(token, run)
        for option, value in (("--platform", "linux/amd64"), ("--network", "none"),
                              ("--user", "1001:123")):
            index = run.index(option)
            self.assertEqual(run[index + 1], value)
        mounts = [run[index + 1] for index, arg in enumerate(run) if arg == "--mount"]
        self.assertEqual(len(mounts), 2)
        self.assertTrue(all(str(self.temp) in mount for mount in mounts))
        self.assertEqual(self.env_file.read_text(), "")

    def test_archive_consumer_preserves_child_failure_status_and_stderr(self):
        archive, checksum = self.consumer_inputs()
        ran = self.consume(archive, checksum, controls={"run_rc": 23})
        self.assertEqual(ran.returncode, 23, ran.stderr)
        evidence = next(self.temp.glob("archive-runtime.*")) / "evidence"
        self.assertEqual((evidence / "consumer.stderr").read_text(),
                         "RECORDING FIXTURE: consumer child failed\n")
        self.assertEqual((evidence / "consumer.exitcode").read_text(), "23\n")
        self.assertIn((evidence / "consumer.stderr").read_text(), ran.stderr)
        self.assertNotIn("shipped archive consumer completed", ran.stdout)
        self.assertEqual(len([row for row in self.commands() if row[1:2] == ["run"]]), 1)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--recording-command"]:
        raise SystemExit(recording_command())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch-root", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    SCRATCH = args.scratch_root
    unittest.main(argv=[sys.argv[0], *remaining])
