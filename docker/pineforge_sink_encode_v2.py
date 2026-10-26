"""Trusted stage-C handoff. The hosting jail supplies uid 10002 and no network.

Accept only stage A's completed receipt and immutable staged v1 object.
Delegate binding to the consuming C++ pass, scrub its environment, and exec it.
This wrapper does not grant receipt authenticity: the run owner must own both
inputs in its private run directory and never accept client receipts.
"""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True
import argparse
import json
import os
import stat
from pathlib import Path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transcoder", type=Path, required=True)
    parser.add_argument("--verified-input", type=Path, required=True)
    parser.add_argument("--stage-a-receipt", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--spool-dir", type=Path, required=True)
    parser.add_argument("--spool-max-bytes", type=int, required=True)
    parser.add_argument("--spool-max-files", type=int, required=True)
    parser.add_argument("--level", type=int, default=3)
    parser.add_argument("--block-records")
    args = parser.parse_args(argv)
    try:
        with args.stage_a_receipt.open("rb") as receipt_file:
            raw = receipt_file.read(65537)
        if len(raw) > 65536:
            raise ValueError("stage-A receipt exceeds bound")
        receipt = json.loads(raw)
        if type(receipt) is not dict or receipt.get("complete") is not True:
            raise ValueError("stage A did not verify a complete stream")
        digest = receipt.get("sha256")
        if (type(digest) is not str or len(digest) != 64
                or any(c not in "0123456789abcdef" for c in digest)):
            raise ValueError("stage-A digest must be lowercase SHA-256 hex")
        counts = [receipt.get("count_" + tag.lower()) for tag in "ETOVKXH"]
        sink_bytes = receipt.get("sink_bytes")
        if any(type(value) is not int or not 0 <= value <= (1 << 53) - 1
               for value in [sink_bytes, *counts]):
            raise ValueError("stage-A counts and size must be safe nonnegative integers")
        # Keep the verified descriptor open across exec; the run owner makes
        # this object immutable. Only C++ reads its contents, from this fd.
        with args.verified_input.open("rb") as source, args.summary.open("rb") as summary:
            original = os.fstat(source.fileno())
            if not stat.S_ISREG(original.st_mode) or original.st_size != sink_bytes:
                raise ValueError("stage-A input is not a regular file of the receipted size")
            os.set_inheritable(source.fileno(), True)
            os.set_inheritable(summary.fileno(), True)
            command = [str(args.transcoder.resolve()), "--in-fd", str(source.fileno()),
                       "--summary-fd", str(summary.fileno()), "--out", str(args.out),
                       "--spool-dir", str(args.spool_dir), "--max-bytes", str(sink_bytes),
                       "--expect-v1-sha256", digest, "--expect-v1-bytes", str(sink_bytes),
                       "--expect-v1-counts", ",".join(map(str, counts)),
                       "--spool-max-bytes", str(args.spool_max_bytes),
                       "--spool-max-files", str(args.spool_max_files), "--level", str(args.level)]
            if args.block_records:
                command += ["--block-records", args.block_records]
            environment = {"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C", "TZ": "UTC"}
            os.execve(command[0], command, environment)
    except OSError as error:
        print(json.dumps({"complete": False, "code": "report_sink_failed",
                          "reason": "v2_resource", "diagnostic": str(error)},
                         separators=(",", ":")), flush=True)
        return 5
    except ValueError as error:
        print(json.dumps({"complete": False, "code": "report_sink_failed",
                          "reason": "v2_input_mismatch", "diagnostic": str(error)},
                         separators=(",", ":")), flush=True)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
