"""Use the existing v1 reader on real-engine L22A fd/path captures."""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docker"))

from pf_sink import SinkFormatError, SinkReader  # noqa: E402


def read(path: Path):
    with SinkReader(path) as reader:
        rows = list(reader)
        assert reader.complete
        assert all(reader.counts[tag] > 0 for tag in "ETOVKXH")
        lines = path.read_bytes().splitlines(keepends=True)
        assert hashlib.sha256(b"".join(lines[:-1])).hexdigest() == reader.sha256
        return rows, reader.trailer


def main(directory: Path) -> None:
    reference = directory / "path.ndjson"
    expected = read(reference)
    for name in ("pipe", "fifo", "socket", "jailed"):
        path = directory / f"{name}.ndjson"
        assert path.read_bytes() == reference.read_bytes(), name
        assert read(path) == expected, name
    for name in ("pipe-closed", "socket-closed", "aborted", "callback-failed"):
        path = directory / f"{name}.ndjson"
        data = path.read_bytes()
        assert data and b'{"trailer":' not in data, name
        assert reference.read_bytes().startswith(data), name
        try:
            read(path)
        except SinkFormatError:
            pass
        else:
            raise AssertionError(f"{name}: incomplete stream accepted")
    print("L22A existing SinkReader verified all fd/path records and refused every failed stream")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
