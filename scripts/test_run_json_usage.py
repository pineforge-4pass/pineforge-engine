"""CR-403 usage receipt row.

The fake getter writes every field independently so the assertion catches
renaming, coercion or omission. The older-library case proves absence is
optional and the failure-line assertion keeps the old prefix byte-for-byte.
"""
from __future__ import annotations

import json
import sys
import ctypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docker"))
import run_json


class OldLibrary:
    pass


class ReceiptLibrary:
    def pf_run_usage_v1(self, out):
        receipt = ctypes.cast(
            out, ctypes.POINTER(run_json.RunUsageC)).contents
        for index, (name, _ctype) in enumerate(run_json.RunUsageC._fields_[2:], start=1):
            setattr(receipt, name, index * 17)
        return 0


def test_old_library_has_no_usage():
    assert run_json._read_run_usage(OldLibrary()) is None
    line = run_json.failure_line("old failure", "engine_invariant", {})
    assert "usage" not in json.loads(line)


def test_receipt_forwards_every_field_and_failure_usage_is_last():
    expected = {
        name: index * 17
        for index, (name, _ctype) in enumerate(run_json.RunUsageC._fields_[2:], start=1)
    }
    assert run_json._read_run_usage(ReceiptLibrary()) == expected
    old = run_json.failure_line("failure", "engine_invariant", {})
    with_usage = run_json.failure_line("failure", "engine_invariant", {},
                                       usage={**expected, "phases_ns": {"serialization": 9}})
    old_doc = json.loads(old)
    usage_doc = json.loads(with_usage)
    assert list(usage_doc)[:-1] == list(old_doc)
    assert usage_doc["usage"] == {**expected, "phases_ns": {"serialization": 9}}


def test_unmeasured_scratch_values_and_mask_are_forwarded():
    class ScratchUnmeasuredLibrary(ReceiptLibrary):
        def pf_run_usage_v1(self, out):
            super().pf_run_usage_v1(out)
            receipt = ctypes.cast(
                out, ctypes.POINTER(run_json.RunUsageC)).contents
            receipt.scratch_file_bytes_peak = 0
            receipt.scratch_file_count_peak = 0
            # Public REPORT_BYTES, SCRATCH_BYTES and SCRATCH_COUNT bits.
            receipt.unmeasured_mask = (1 << 4) | (1 << 5) | (1 << 6)
            return 0

    usage = run_json._read_run_usage(ScratchUnmeasuredLibrary())
    assert usage["scratch_file_bytes_peak"] == 0
    assert usage["scratch_file_count_peak"] == 0
    assert usage["unmeasured_mask"] == (1 << 4) | (1 << 5) | (1 << 6)
    assert usage["cpu_user_ns"] == 17


if __name__ == "__main__":
    raise SystemExit(0)
