#!/usr/bin/env python3
"""Installed shipped-helper binding, using an isolated second build tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile
import unittest


def sha(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("ascii") + b"\n"


class ManifestBinding(unittest.TestCase):
    def run_command(self, argv, expected=0):
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        self.assertEqual(result.returncode, expected, result.stdout.decode("utf-8", "replace"))
        return result.stdout

    def test_installed_shipped_binding(self):
        source = Path(__file__).resolve().parents[1]
        parent = Path(OPTIONS.parent_build)
        root = Path(tempfile.mkdtemp(prefix="selected-admit-install-", dir=OPTIONS.work_dir))
        build = root / "build"
        identity_file = root / "identity.cmake"
        configure_prefix = root / "configure-prefix"
        chosen_sha = secrets.token_hex(20)

        def read_cache(tree):
            entries = {}
            for line in (tree / "CMakeCache.txt").read_text().splitlines():
                match = re.fullmatch(r"([^:#][^:]*):[^=]+=(.*)", line)
                if match:
                    entries[match[1]] = match[2]
            return entries

        cache = read_cache(parent)
        parent_options = [
            "-D" + key + "=" + cache[key]
            for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "Eigen3_DIR")
            if key in cache
        ]

        def write_identity(value, dirty):
            identity_file.write_text(
                'set(PINEFORGE_VERSION_GIT_SHA "' + value + '")\n'
                + "set(PINEFORGE_VERSION_DIRTY " + ("ON" if dirty else "OFF") + ")\n",
                encoding="utf-8",
            )

        def configure(extra=()):
            argv = [
                "cmake", "-S", str(source), "-B", str(build), *parent_options,
                "-DCMAKE_BUILD_TYPE=Release", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                "-DPINEFORGE_BUILD_TESTS=OFF", "-DPINEFORGE_BUILD_TUTORIAL=OFF",
                "-DPINEFORGE_VERSION_SOURCE=FILE", "-DCMAKE_PROJECT_INCLUDE=" + str(identity_file),
                "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_PREFIX=" + str(configure_prefix),
                *extra,
            ]
            return self.run_command(argv)

        def build_all():
            self.run_command(["cmake", "--build", str(build), "--parallel", "4"])

        spec_bytes = (source / "tools/selected_admit/DESCRIPTOR-V1.md").read_bytes()
        schemas = re.search(
            r"Request `([^`]+)`; result `([^`]+)`; manifest `([^`]+)`",
            spec_bytes.decode("utf-8"),
        )
        self.assertIsNotNone(schemas)
        request_schema, result_schema, manifest_schema = schemas.groups()
        catalog_bytes = (source / "docker/run_failure_codes.json").read_bytes()
        catalog = json.loads(catalog_bytes)
        codes = [name for name in catalog["codes"]
                 if name.startswith("selected_") and name.endswith("_limit_exceeded")]
        self.assertEqual(len(codes), 5)
        version = (source / "VERSION").read_text().strip()
        version_mmp = re.match(r"[0-9]+\.[0-9]+\.[0-9]+", version).group(0)
        expected_stages = json.loads(OPTIONS.expected_stages)

        def installed_identity(header):
            text = header.read_text()
            return {
                name: re.search(r"^#define\s+" + name + r'\s+"([^"]*)"\s*$', text, re.M).group(1)
                for name in ("PINEFORGE_GIT_SHA", "PINEFORGE_VERSION_FULL", "PINEFORGE_VERSION_STRING")
            }

        def check_binary(binary, manifest):
            data = binary.read_bytes()
            self.assertEqual(manifest["helper"]["bytes"], str(len(data)))
            self.assertEqual(manifest["helper"]["sha256"], sha(data))

        def check_install(prefix, expected_sha, dirty):
            binary = prefix / "bin/pineforge-selected-admit"
            manifest_path = prefix / "share/pineforge/pineforge-selected-admit.manifest.json"
            self.assertTrue(binary.is_file())
            manifest_bytes = manifest_path.read_bytes()
            manifest = json.loads(manifest_bytes)
            self.assertEqual(manifest_bytes, canonical(manifest))
            self.assertEqual(manifest["helper"]["path"], "bin/pineforge-selected-admit")
            binary = prefix / manifest["helper"]["path"]
            check_binary(binary, manifest)
            projection = json.loads(manifest_bytes)
            del projection["helper"]["bytes"]
            del projection["helper"]["sha256"]
            description = self.run_command([str(binary), "--describe"])
            self.assertEqual(description, canonical(projection))
            installed = installed_identity(prefix / "include/pineforge/version.h")
            self.assertEqual(manifest["engine"]["git_sha"], installed["PINEFORGE_GIT_SHA"])
            self.assertEqual(manifest["engine"]["git_sha"], expected_sha)
            self.assertEqual(manifest["engine"]["version_full"], installed["PINEFORGE_VERSION_FULL"])
            self.assertEqual(manifest["engine"]["version_full"], version)
            self.assertEqual(manifest["engine"]["version"], installed["PINEFORGE_VERSION_STRING"])
            self.assertEqual(manifest["engine"]["version"], version_mmp)
            self.assertIs(manifest["engine"]["dirty"], dirty)
            self.assertEqual(manifest["helper"]["name"], "pineforge-selected-admit")
            self.assertNotIn(str(root), manifest["helper"]["path"])
            self.assertNotIn(b"test_identity", manifest_bytes)
            self.assertEqual(manifest["schema"], manifest_schema)
            self.assertEqual(manifest["descriptor"], {
                "request_schema": request_schema, "result_schema": result_schema,
                "spec_sha256": sha(spec_bytes),
            })
            self.assertEqual(manifest["failure_catalog"], {
                "codes": codes, "catalog_sha256": sha(catalog_bytes),
            })
            self.assertEqual(manifest["stages"], expected_stages)
            for decoy in decoy_data:
                self.assertNotIn(sha(decoy).encode("ascii"), manifest_bytes)
            probe = subprocess.run([
                str(binary), "--stage", "source", "--descriptor-fd", "2147483647",
                "--descriptor-sha256", "0" * 64,
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(probe.returncode, 41 if "source" in expected_stages else 2)
            original = binary.read_bytes()
            tampered = bytearray(original)
            self.assertTrue(tampered)
            tampered[len(tampered) // 2] ^= 1
            binary.write_bytes(tampered)
            with self.assertRaises(AssertionError):
                check_binary(binary, manifest)
            binary.write_bytes(original)
            return binary

        def descriptor_case(binary, expected_identity, expected_exit, reason):
            data = b"timestamp,open,high,low,close,volume\n"
            input_path = root / "identity-input.csv"
            input_path.write_bytes(data)
            input_fd = os.open(input_path, os.O_RDONLY)
            try:
                request = {
                    "schema": request_schema, "expect": {"git_sha": expected_identity},
                    "identity": {"run_id": "manifest-run", "attempt_id": "manifest-attempt"},
                    "bounds": {}, "limits": {"max_record_bytes": "4096"},
                    "streams": [{"id": "source", "role": "source", "kind": "file",
                                 "fd": str(input_fd), "bytes": str(len(data)), "sha256": sha(data)}],
                }
                raw = canonical(request)
                descriptor_path = root / "identity-request.json"
                descriptor_path.write_bytes(raw)
                descriptor_fd = os.open(descriptor_path, os.O_RDONLY)
                try:
                    result = subprocess.run([
                        str(binary), "--stage", "selected", "--descriptor-fd", str(descriptor_fd),
                        "--descriptor-sha256", sha(raw),
                    ], pass_fds=(input_fd, descriptor_fd), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, check=False)
                finally:
                    os.close(descriptor_fd)
                self.assertEqual(result.returncode, expected_exit, result.stderr)
                document = json.loads(result.stdout)
                self.assertEqual(document["exit"], expected_exit)
                self.assertEqual(document["reason"], reason)
                self.assertNotIn(result.returncode, (0, 41))
            finally:
                os.close(input_fd)

        configure_decoy = configure_prefix / "bin/pineforge-selected-admit"
        path_decoy = root / "decoy-bin/pineforge-selected-admit"
        configure_decoy.parent.mkdir(parents=True)
        path_decoy.parent.mkdir(parents=True)
        decoy_data = (b"configure prefix decoy\n", b"PATH decoy\n")
        configure_decoy.write_bytes(decoy_data[0])
        path_decoy.write_bytes(decoy_data[1])
        path_decoy.chmod(0o755)
        old_path = os.environ.get("PATH", "")
        old_destdir = os.environ.get("DESTDIR")
        os.environ["PATH"] = str(path_decoy.parent) + os.pathsep + old_path
        os.environ.pop("DESTDIR", None)
        try:
            write_identity(chosen_sha, True)
            configure()
            build_all()
            prefix = root / "prefix"
            self.run_command(["cmake", "--install", str(build), "--prefix", str(prefix)])
            check_install(prefix, chosen_sha, True)
            self.assertEqual(configure_decoy.read_bytes(), decoy_data[0])
            self.assertEqual(path_decoy.read_bytes(), decoy_data[1])
            os.environ["DESTDIR"] = str(root / "dest")
            self.run_command(["cmake", "--install", str(build), "--prefix", "/opt/pf-stage"])
            os.environ.pop("DESTDIR")
            self.assertFalse(Path("/opt/pf-stage").exists())
            check_install(root / "dest/opt/pf-stage", chosen_sha, True)
            self.assertEqual(configure_decoy.read_bytes(), decoy_data[0])
            self.assertEqual(path_decoy.read_bytes(), decoy_data[1])
            # Retain the decoy outside the install location; delete no file.
            configure_decoy.rename(root / "retained-configure-decoy")
            self.run_command(["cmake", "--install", str(build)])
            binary = check_install(configure_prefix, chosen_sha, True)
            write_identity(chosen_sha, False)
            configure()
            build_all()
            self.run_command(["cmake", "--install", str(build)])
            binary = check_install(configure_prefix, chosen_sha, False)
            for weak, expected in (("d5242fe", "d5242fe12345"), ("unknown", "abcdef012345")):
                write_identity(weak, False)
                configure()
                build_all()
                self.run_command(["cmake", "--install", str(build)])
                binary = check_install(configure_prefix, weak, False)
                descriptor_case(binary, expected, 40, "helper:helper_mismatch")
            write_identity(chosen_sha, False)
            configure()
            build_all()
            self.run_command(["cmake", "--install", str(build)])
            binary = check_install(configure_prefix, chosen_sha, False)
            wrong = ("0" if chosen_sha[0] != "0" else "1") + chosen_sha[1:12]
            descriptor_case(binary, wrong, 40, "helper:helper_mismatch")
            descriptor_case(binary, chosen_sha[:12], 10, "no_requested_fields")
            descriptor_case(binary, chosen_sha, 10, "no_requested_fields")

            def walk_helpers(directory):
                found = []
                if not directory.exists():
                    return found
                self.assertTrue(directory.is_dir())

                def walk_error(error):
                    raise error

                for current, directories, files in os.walk(directory, onerror=walk_error):
                    for name in files:
                        if name in ("pineforge-selected-admit",
                                    "pineforge-selected-admit.manifest.json"):
                            found.append(Path(current) / name)
                return found

            absolute_bindir = root / "pf-abs-bindir"
            absolute_datadir = root / "pf-abs-datadir"
            self.assertTrue(absolute_bindir.is_absolute())
            self.assertTrue(absolute_datadir.is_absolute())
            layouts = (
                ("sbin", "data"),
                (str(absolute_bindir), str(absolute_datadir)),
            )
            for number, (bindir, datadir) in enumerate(layouts, start=1):
                configure((
                    "-DCMAKE_INSTALL_BINDIR=" + bindir,
                    "-DCMAKE_INSTALL_DATADIR=" + datadir,
                ))
                installed_cache = read_cache(build)
                self.assertEqual(installed_cache.get("PINEFORGE_BUILD_LIVE_RUNNER"), "OFF")
                self.assertEqual(installed_cache.get("CMAKE_INSTALL_BINDIR"), bindir)
                self.assertEqual(installed_cache.get("CMAKE_INSTALL_DATADIR"), datadir)
                build_all()
                dest = root / ("dest-" + str(number))
                self.assertFalse(dest.exists())
                saved_destdir = os.environ.get("DESTDIR")
                os.environ["DESTDIR"] = str(dest)
                try:
                    self.run_command(["cmake", "--install", str(build),
                                      "--prefix", "/opt/pf-t23"])
                finally:
                    if saved_destdir is None:
                        os.environ.pop("DESTDIR", None)
                    else:
                        os.environ["DESTDIR"] = saved_destdir
                self.assertFalse(Path("/opt/pf-t23").exists())
                install_root = dest / "opt/pf-t23"
                self.assertTrue(install_root.is_dir())
                check_install(install_root, chosen_sha, False)
                self.assertCountEqual(walk_helpers(install_root), [
                    install_root / "bin/pineforge-selected-admit",
                    install_root / "share/pineforge/pineforge-selected-admit.manifest.json",
                ])
                if number == len(layouts):
                    for directory in (absolute_bindir, absolute_datadir):
                        self.assertEqual(walk_helpers(directory), [])
                        staged_directory = dest / directory.relative_to(directory.anchor)
                        self.assertEqual(walk_helpers(staged_directory), [])
        finally:
            os.environ["PATH"] = old_path
            if old_destdir is None:
                os.environ.pop("DESTDIR", None)
            else:
                os.environ["DESTDIR"] = old_destdir


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--parent-build", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--expected-stages", required=True)
    OPTIONS = parser.parse_args()
    unittest.main(argv=[__file__], failfast=True)
