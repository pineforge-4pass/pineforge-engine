"""The container entrypoint must not compile a mounted /in/strategy.cpp unless the host vouches for it.

The entrypoint compiles `/in/strategy.cpp` unchanged when no `/in/strategy.pine` is mounted. A caller that
can choose those bytes therefore chooses native code that runs in the engine's process. The host vouches
for exact bytes by exporting their sha256 as PINEFORGE_STRATEGY_CPP_SHA256 (environment is container
configuration; the user only controls the mounted bytes). Without a matching digest the entrypoint
refuses before g++ runs: exit 2 and one stdout line {"engine":"pineforge","error":...,"code":
"run_request_invalid","args":{"option":"generated_cpp"}}. PINEFORGE_ALLOW_UNATTESTED_CPP=1 is the
documented local-only opt-out. The .pine route is not touched.

The toolchain is stubbed: g++ records that it was called, so "refused before g++" is observable.
"""
import hashlib
import json
import os
import stat
import subprocess
from pathlib import Path

import pytest

ENTRYPOINT = Path(__file__).resolve().parents[1] / "docker" / "entrypoint.sh"
CPP_BYTES = b"// user supplied translation unit\nint user_main_marker = 1;\n"
REPORT = '{"ok": true, "marker": "ran"}'


class Box:
    def __init__(self, root):
        self.root = root
        self.indir = root / "in"
        self.prefix = root / "prefix"
        self.bin = root / "bin"
        self.called = root / "gxx-called"
        for d in (self.indir, self.prefix / "bin", self.prefix / "lib", self.prefix / "include", self.bin):
            d.mkdir(parents=True)
        (self.prefix / "lib" / "libpineforge.a").write_bytes(b"")
        gxx = self.bin / "g++"
        gxx.write_text(
            '#!/usr/bin/env bash\n'
            f': > "{self.called}"\n'
            'out=""; prev=""\n'
            'for a in "$@"; do [ "$prev" = "-o" ] && out="$a"; prev="$a"; done\n'
            ': > "$out"\n', encoding="utf-8")
        runner = self.prefix / "bin" / "run_json.py"
        runner.write_text(f"#!/usr/bin/env python3\nprint('{REPORT}')\n", encoding="utf-8")
        for f in (gxx, runner):
            f.chmod(f.stat().st_mode | stat.S_IXUSR)
        (self.indir / "ohlcv.csv").write_text("timestamp,open,high,low,close,volume\n", encoding="utf-8")

    def put_cpp(self):
        (self.indir / "strategy.cpp").write_bytes(CPP_BYTES)

    def run(self, extra=None):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("PINEFORGE_")}
        env.update({"PATH": f"{self.bin}:{os.environ['PATH']}", "PINEFORGE_PREFIX": str(self.prefix),
                    "PINEFORGE_IN_DIR": str(self.indir), "TMPDIR": str(self.root)})
        env.update(extra or {})
        return subprocess.run(["bash", str(ENTRYPOINT)], env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, encoding="utf-8", errors="replace", check=False)


@pytest.fixture
def box(tmp_path):
    return Box(tmp_path)


def assert_typed_refusal(result, box):
    assert result.returncode == 2, (result.returncode, result.stderr)
    lines = result.stdout.splitlines()
    assert len(lines) == 1, result.stdout
    document = json.loads(lines[0])
    assert document["engine"] == "pineforge"
    assert document["code"] == "run_request_invalid"
    assert document["args"] == {"option": "generated_cpp"}
    assert isinstance(document["error"], str) and "strategy.cpp" in document["error"]
    assert not box.called.exists(), "g++ ran on bytes nobody vouched for"


def test_plain_cpp_without_a_digest_is_refused_before_g_plus_plus(box):
    box.put_cpp()
    assert_typed_refusal(box.run(), box)


def test_cpp_with_a_wrong_digest_is_refused(box):
    box.put_cpp()
    wrong = hashlib.sha256(CPP_BYTES + b"x").hexdigest()
    assert_typed_refusal(box.run({"PINEFORGE_STRATEGY_CPP_SHA256": wrong}), box)


def test_cpp_with_a_malformed_digest_is_refused(box):
    box.put_cpp()
    assert_typed_refusal(box.run({"PINEFORGE_STRATEGY_CPP_SHA256": "not-a-digest"}), box)


def test_cpp_with_the_matching_digest_compiles_and_runs(box):
    box.put_cpp()
    digest = hashlib.sha256(CPP_BYTES).hexdigest()
    result = box.run({"PINEFORGE_STRATEGY_CPP_SHA256": digest})
    assert result.returncode == 0, result.stderr
    assert '"marker": "ran"' in result.stdout
    assert box.called.exists()


def test_a_changed_byte_after_the_host_hashed_it_is_refused(box):
    box.put_cpp()
    digest = hashlib.sha256(CPP_BYTES).hexdigest()
    (box.indir / "strategy.cpp").write_bytes(CPP_BYTES + b"// appended by the user\n")
    assert_typed_refusal(box.run({"PINEFORGE_STRATEGY_CPP_SHA256": digest}), box)


def test_local_dev_opt_out_compiles_without_a_digest(box):
    box.put_cpp()
    result = box.run({"PINEFORGE_ALLOW_UNATTESTED_CPP": "1"})
    assert result.returncode == 0, result.stderr
    assert box.called.exists()


def test_the_opt_out_flag_needs_the_exact_value(box):
    box.put_cpp()
    assert_typed_refusal(box.run({"PINEFORGE_ALLOW_UNATTESTED_CPP": "yes"}), box)
