"""Run the real Eigen fallback with an inert local archive, offline."""

import hashlib
import io
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


URL = "https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz"
SHA256 = "8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72"
MARKER_NAME = "archive-witness.txt"
MARKER = b"Local archive for the Eigen 3.4.0 hash test.\n"


def not_compared(reason, expected, observed):
    print(f"NOT COMPARED: {reason}; expected {expected!r}, observed {observed!r}", file=sys.stderr)
    return 1


def check(cmake, cmakelists):
    text = cmakelists.read_text(encoding="utf-8")
    begin = "find_package(Eigen3 3.3 QUIET)"
    end = "# === pineforge library"
    _, start, remainder = text.partition(begin)
    stanza, stop, _ = remainder.partition(end)
    if not start or not stop or not stanza.strip():
        return not_compared("Eigen fallback stanza", f"{begin} through {end}",
                            "missing or incomplete stanza" if text.strip() else "empty CMakeLists")
    urls = re.findall(r"(?m)^[ \t]*URL[ \t]+([^\r\n]+)", stanza)
    if urls != [URL]:
        return not_compared("Eigen fallback URL", [URL], urls)

    with tempfile.TemporaryDirectory(prefix="pineforge-issue-959-") as tmp:
        root = Path(tmp)
        archive = root / "eigen-3.4.0.tar.gz"
        with tarfile.open(archive, "w:gz") as fixture:
            marker = tarfile.TarInfo(MARKER_NAME)
            marker.size = len(MARKER)
            fixture.addfile(marker, io.BytesIO(MARKER))
        archive_bytes = archive.read_bytes()
        if not archive_bytes:
            return not_compared("local fixture size", "nonzero bytes", 0)
        actual_hash = hashlib.sha256(archive_bytes).hexdigest()
        if actual_hash == SHA256:
            return not_compared("local fixture SHA-256 must differ", f"different from {SHA256}", actual_hash)

        # Keep the production find/declare/populate logic. Only the archive
        # location changes; the local path goes through CMake's URL verifier.
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(eigen_archive_witness LANGUAGES NONE)\n"
            "set(CMAKE_DISABLE_FIND_PACKAGE_Eigen3 ON)\n"
            + start + stanza.replace(URL, f'"{archive.as_posix()}"', 1),
            encoding="utf-8",
        )
        build = root / "build"
        result = subprocess.run([cmake, "-S", str(root), "-B", str(build)],
                                capture_output=True, text=True)
        log = result.stdout + result.stderr
        if "System Eigen not found, fetching via FetchContent" not in log:
            return not_compared("forced Eigen fallback", "FetchContent fallback message", log)

        extracted = build / "_deps/eigen-src" / MARKER_NAME
        if result.returncode == 0:
            contents = extracted.read_bytes() if extracted.is_file() else None
            if contents != MARKER:
                return not_compared("extraction after configure succeeded", MARKER, contents)
            print("FAIL: Eigen fallback accepted an archive with the wrong SHA-256.", file=sys.stderr)
            return 1

        mismatch = re.search(
            r"SHA256 hash of\b.*?does not match expected value\s*"
            r"expected:\s*'([0-9a-f]{64})'\s*actual:\s*'([0-9a-f]{64})'",
            log, re.DOTALL,
        )
        if mismatch is None or mismatch[2] != actual_hash:
            return not_compared(f"configure exited {result.returncode} without the fixture's hash mismatch",
                                f"expected {SHA256}, actual {actual_hash}", log)
        if mismatch[1] != SHA256:
            print(f"FAIL: wrong Eigen archive pin; expected {SHA256}, observed {mismatch[1]}.", file=sys.stderr)
            return 1
        if extracted.exists():
            print("FAIL: Eigen fallback extracted the archive despite its SHA-256 mismatch.", file=sys.stderr)
            return 1
        print("PASS: Eigen fallback rejected the archive before extraction (SHA-256 mismatch).")
        return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(not_compared("test arguments", "CMake executable and CMakeLists.txt", sys.argv[1:]))
    try:
        sys.exit(check(sys.argv[1], Path(sys.argv[2])))
    except (OSError, UnicodeError) as error:
        sys.exit(not_compared("test could not read/write input or run CMake",
                              "readable CMakeLists, writable temp dir and runnable CMake", str(error)))
