#!/usr/bin/env python3
"""Fail-closed contract tests for scripts/run_corpus.sh."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


RUN_CORPUS = Path(__file__).with_name("run_corpus.sh")


def fixture(*, generated: bool, library: bool) -> tuple[Path, Path]:
    root = Path(tempfile.mkdtemp(prefix="pf-run-corpus-self-test-"))
    scripts = root / "scripts"
    strategy = root / "corpus" / "validation" / "probe"
    scripts.mkdir(parents=True)
    strategy.mkdir(parents=True)
    shutil.copy2(RUN_CORPUS, scripts / "run_corpus.sh")
    (root / "corpus" / "CMakeLists.txt").write_text("# fixture\n")
    (strategy / "strategy.pine").write_text("//@version=6\nstrategy('x')\n")
    if generated:
        (strategy / "generated.cpp").write_text("// fixture\n")
    if library:
        (strategy / "strategy.so").write_bytes(b"fixture")

    fake_python = scripts / "fake_python.py"
    fake_python.write_text(
        "#!/usr/bin/env python3\n"
        "import os, sys\n"
        "if len(sys.argv) > 1 and sys.argv[1].endswith('run_strategy.py') "
        "and os.environ.get('PF_FAKE_RUN_FAIL') == '1':\n"
        "    print('synthetic runner failure', file=sys.stderr)\n"
        "    raise SystemExit(42)\n"
    )
    fake_python.chmod(0o755)
    return root, fake_python


def run_case(root: Path, fake_python: Path, **extra_env: str) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env.update({
        "PYTHON": str(fake_python),
        "SKIP_BUILD": "1",
        "SKIP_VERIFY": "1",
        **extra_env,
    })
    return subprocess.run(
        [str(root / "scripts" / "run_corpus.sh")],
        cwd=root,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def check_failure(result: subprocess.CompletedProcess, expected: str) -> None:
    assert result.returncode != 0, result.stdout
    assert expected in result.stdout, result.stdout
    assert "regenerating canonical validation report" not in result.stdout, result.stdout


def main() -> int:
    roots: list[Path] = []
    try:
        root, py = fixture(generated=False, library=False)
        roots.append(root)
        check_failure(run_case(root, py), "missing generated.cpp")

        root, py = fixture(generated=True, library=False)
        roots.append(root)
        check_failure(run_case(root, py), "missing compiled strategy library")

        root, py = fixture(generated=True, library=True)
        roots.append(root)
        check_failure(
            run_case(root, py, ONLY="does-not-match"),
            "no corpus strategies matched ONLY=does-not-match",
        )

        root, py = fixture(generated=True, library=True)
        roots.append(root)
        result = run_case(root, py, PF_FAKE_RUN_FAIL="1")
        check_failure(result, "synthetic runner failure")
        assert "refusing to verify stale engine_trades.csv" in result.stdout

        root, py = fixture(generated=True, library=True)
        roots.append(root)
        result = run_case(root, py)
        assert result.returncode == 0, result.stdout
        assert "ran 1 strategies" in result.stdout, result.stdout
        assert "done." in result.stdout, result.stdout
    finally:
        for root in roots:
            shutil.rmtree(root, ignore_errors=True)

    print("run_corpus_self_test: 5 passed, 0 failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
