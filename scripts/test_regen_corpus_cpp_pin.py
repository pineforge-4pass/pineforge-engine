#!/usr/bin/env python3
"""Source control for the sweep regeneration's paired-codegen pin."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / "scripts/regen_corpus_cpp.sh").read_text()
commit = "f3285d79f476c029e9739599658b5de6296046bc"
if text.count(f'CODEGEN_COMMIT="{commit}"') != 1:
    raise SystemExit("regen_corpus_cpp: paired codegen commit is not pinned exactly once")
if "pineforge-release:latest" in text:
    raise SystemExit("regen_corpus_cpp: floating release image remains")
if not re.search(r"pineforge-release@sha256:[0-9a-f]{64}", text):
    raise SystemExit("regen_corpus_cpp: runtime image is not digest-pinned")
for required in (
        'actual_codegen="$(git -C "$codegen_checkout" rev-parse HEAD)"',
        '[[ "$actual_codegen" == "$CODEGEN_COMMIT" ]]',
        "--network=none", "-e PYTHONPATH=/codegen"):
    if required not in text:
        raise SystemExit("regen_corpus_cpp: missing pin enforcement: " + required)
print("regen_corpus_cpp: codegen f3285d7 and immutable runtime image pinned")
