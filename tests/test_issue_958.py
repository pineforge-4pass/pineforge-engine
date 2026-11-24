"""Check the mounted-C++ gate without compiling or loading the fixture."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ENTRYPOINT = Path(__file__).resolve().parents[1] / "docker" / "entrypoint.sh"
# Use the inert inputs from codegen's include policy test.
ALLOWED = b"#include <pineforge/source/pine_strategy_host.hpp>\n"
REFUSED = b"#include <unistd.h>\n"
REPORT = b'{"engine":"pineforge","marker":"issue-958-control"}\n'
REFUSAL = {
    "engine": "pineforge",
    "error": "entrypoint.sh: error: mounted strategy.cpp did not pass the generated-C++ allowlist",
    "code": "run_request_invalid",
    "args": {"option": "generated_cpp"},
}


def write_tool(path, body):
    path.write_text("#!/usr/bin/env bash\nset -eu\n" + body, encoding="utf-8")
    path.chmod(0o700)


def invoke(root, bash, cp, source, local, *, replace_mount=False, vouched_bytes=None):
    for directory in ("bin", "in", "prefix/bin", "packages/pineforge_codegen"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    (root / "in/strategy.cpp").write_bytes(source)
    (root / "in/ohlcv.csv").write_text(
        "timestamp,open,high,low,close,volume\n1700000000000,1,1,1,1,1\n", encoding="ascii")
    package = root / "packages/pineforge_codegen"
    (package / "__init__.py").touch()
    (root / "bin/python3").symlink_to(sys.executable)
    # The double records its input and recognizes only the two fixture units.
    (package / "cpp_allowlist.py").write_text(
        "from pathlib import Path\n"
        "def check_generated_cpp(text):\n"
        "    Path('checked.cpp').write_bytes(text.encode('utf-8'))\n"
        f"    if text == {ALLOWED.decode()!r}:\n"
        "        return []\n"
        f"    if text == {REFUSED.decode()!r}:\n"
        "        return [object()]\n"
        "    raise ValueError('unexpected fixture input')\n", encoding="utf-8")
    write_tool(root / "bin/g++",
               ': > compiler-called\n'
               'for arg in "$@"; do\n'
               '    if [[ "$arg" == *.cpp ]]; then cat -- "$arg" > compiler-input.cpp; fi\n'
               '    if [[ "$arg" == *.so ]]; then echo "stub object" > "$arg"; fi\n'
               'done\n')
    (root / "prefix/bin/run_json.py").write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "library = Path(sys.argv[sys.argv.index('--so') + 1])\n"
        "assert library.read_bytes() == b'stub object\\n'\n"
        f"print({REPORT.decode().rstrip()!r})\n", encoding="ascii")
    if replace_mount:
        # Model an update to the mount once the original bytes have been copied.
        write_tool(root / "bin/cp", '"$ISSUE958_CP" "$@"\n'
                   'if [[ "$2" == */in/strategy.cpp ]]; then\n'
                   '    printf "%s\\n" "#include <pineforge/source/pine_strategy_host.hpp>" > "$2"\n'
                   'fi\n')
    env = {name: value for name, value in os.environ.items()
           if not name.startswith(("PINEFORGE_", "PYTHON", "BASH_FUNC_")) and name != "BASH_ENV"}
    env.update(PATH=str(root / "bin") + os.pathsep + os.defpath,
               PYTHONPATH=str(root / "packages"), PYTHONDONTWRITEBYTECODE="1",
               TMPDIR=str(root), PINEFORGE_PREFIX=str(root / "prefix"),
               PINEFORGE_IN_DIR=str(root / "in"), ISSUE958_CP=cp)
    if local:
        env["PINEFORGE_ALLOW_UNATTESTED_CPP"] = "1"
    else:
        env["PINEFORGE_STRATEGY_CPP_SHA256"] = hashlib.sha256(
            source if vouched_bytes is None else vouched_bytes).hexdigest()
    return subprocess.run([bash, str(ENTRYPOINT)], cwd=root, env=env,
                          stdin=subprocess.DEVNULL, capture_output=True, check=False)


def observed(path):
    return path.read_bytes() if path.is_file() else None


class Issue958(unittest.TestCase):
    def test_mounted_cpp_must_pass_allowlist_before_compilation(self):
        bash, cp = shutil.which("bash"), shutil.which("cp")
        self.assertTrue(bash and cp and ENTRYPOINT.is_file() and ENTRYPOINT.stat().st_size,
                        f"NOT COMPARED: expected bash, cp and a nonempty script at {ENTRYPOINT}; "
                        f"got bash={bash!r}, cp={cp!r}, "
                        f"script bytes={ENTRYPOINT.stat().st_size if ENTRYPOINT.is_file() else None}")
        with tempfile.TemporaryDirectory(prefix="pf-issue-958-") as temporary:
            for local in (False, True):
                root = Path(temporary) / ("local" if local else "vouched")
                with self.subTest(local_opt_out=local, case="allowlist"):
                    control = root / "allowed"
                    accepted = invoke(control, bash, cp, ALLOWED, local)
                    self.assertEqual(
                        (accepted.returncode, accepted.stdout, observed(control / "compiler-input.cpp")),
                        (0, REPORT, ALLOWED),
                        f"NOT COMPARED: expected compiler and runner to accept the known-good fixture; "
                        f"got stderr={accepted.stderr!r}")
                    refused = root / "refused"
                    result = invoke(refused, bash, cp, REFUSED, local, replace_mount=True)
                    compiled = observed(refused / "compiler-input.cpp")
                    if result.returncode == 0:
                        self.assertEqual((result.stdout, compiled), (REPORT, REFUSED),
                                         "NOT COMPARED: success did not use the refused fixture")
                    else:
                        self.assertEqual(result.returncode, 2,
                                         f"NOT COMPARED: expected status 0 or 2; "
                                         f"got {result.stdout!r}, {result.stderr!r}, g++={compiled!r}")
                    self.assertEqual(result.returncode, 2,  # red on the base: 0 and g++ entered
                                     f"FAIL: g++ received the refused bytes {compiled!r}")
                    self.assert_refusal(result, refused)
                    self.assertEqual(observed(refused / "in/strategy.cpp"), ALLOWED,
                                     "NOT COMPARED: expected the mount to change after the copy")
                    self.assertEqual(observed(refused / "checked.cpp"), REFUSED,
                                     "NOT COMPARED: expected the checker to receive the frozen bytes")
                    self.assertEqual(observed(control / "checked.cpp"), ALLOWED,
                                     "NOT COMPARED: expected the checker to receive the accepted bytes")

                with self.subTest(local_opt_out=local, case="empty"):
                    empty = root / "empty"
                    result = invoke(empty, bash, cp, b"", local)
                    self.assertEqual(observed(empty / "in/strategy.cpp"), b"",
                                     "NOT COMPARED: expected a present, empty mounted file")
                    self.assert_refusal(result, empty)
                    self.assertIsNone(observed(empty / "checked.cpp"))

                if not local:
                    with self.subTest(case="wrong-vouch"):
                        wrong = root / "wrong-vouch"
                        result = invoke(wrong, bash, cp, REFUSED, local, vouched_bytes=ALLOWED)
                        self.assert_refusal(result, wrong, {
                            **REFUSAL, "error": "entrypoint.sh: error: mounted strategy.cpp is "
                            "not vouched for by a matching PINEFORGE_STRATEGY_CPP_SHA256",
                        })
                        self.assertIsNone(observed(wrong / "checked.cpp"))

    def assert_refusal(self, result, root, expected=REFUSAL):
        self.assertEqual(result.returncode, 2, (result.stdout, result.stderr))
        self.assertFalse((root / "compiler-called").exists(), "g++ entered on refused input")
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1, (result.stdout, result.stderr))
        self.assertEqual(json.loads(lines[0]), expected)


if __name__ == "__main__":
    unittest.main()
