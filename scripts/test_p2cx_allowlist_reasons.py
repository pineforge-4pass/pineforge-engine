#!/usr/bin/env python3
"""L-F-05: waiver reasons may cite only declared ScratchStatus members.

Only the two fixed inputs below are read. Paths in allowlist rows are text,
not additional files to follow.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).absolute().parent.parent
INPUTS = (
    ROOT / "include/pineforge/detail/scratch_backing.hpp",
    ROOT / "scripts/run_failure_throw_allowlist.txt",
)


def read_inputs() -> tuple[str, str]:
    print("L-F-05 input files:\n" + "\n".join(str(path) for path in INPUTS),
          flush=True)
    forbidden_parts = {".ssh", ".aws", ".codex"}
    forbidden_sequences = ((".config", "zsh", "keys"), (".config", "pineforge"))
    # Refuse named private paths before inspecting symlinks or opening files.
    for path in INPUTS:
        parts = path.parts
        if (forbidden_parts.intersection(parts)
                or any(part.endswith(".env") for part in parts)
                or any(parts[index:index + len(sequence)] == sequence
                       for sequence in forbidden_sequences
                       for index in range(len(parts)))):
            raise ValueError(f"refused input path: {path}")
        # Fixed inputs must not redirect to an unlisted or private file.
        if any(candidate.is_symlink() for candidate in (path, *path.parents)):
            raise ValueError(f"refused symlink input path: {path}")
    header, allowlist = (path.read_text(encoding="utf-8") for path in INPUTS)
    return header, allowlist


def scratch_status_names(header: str) -> set[str]:
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", header, flags=re.DOTALL)
    match = re.search(r"\benum\s+class\s+ScratchStatus\b[^{}]*\{([^{}]*)\}", code)
    if match is None:
        raise ValueError("ScratchStatus enum declaration is missing")
    names = {item.split("=", 1)[0].strip()
             for item in match.group(1).split(",") if item.strip()}
    if not names or any(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None
                        for name in names):
        raise ValueError("ScratchStatus enum members could not be read")
    return names


def cited_status_names(reason: str) -> set[str]:
    names = set(re.findall(r"\bScratchStatus\s*::\s*([A-Za-z_][A-Za-z0-9_]*)",
                           reason))
    if re.search(r"\bScratchStatus\b", reason):
        # The original waiver used bare names in a parenthesized mapping.
        # Keep reading that spelling so restoring it cannot evade the test.
        for mapping in re.findall(r"\(([^()]*)\)", reason):
            names.update(re.findall(
                r"(?:^|,)\s*([A-Za-z_][A-Za-z0-9_]*)\s+is\b", mapping))
    return names


class AllowlistReasonsTest(unittest.TestCase):
    def test_legacy_bare_status_mapping_is_read(self) -> None:
        reason = (
            "the ScratchStatus it was given (io_error is scratch_io_failed, "
            "capacity_exceeded is scratch_capacity_exceeded with resource=layout, "
            "anything else engine_invariant)"
        )
        self.assertEqual(cited_status_names(reason),
                         {"io_error", "capacity_exceeded"})

    def test_qualified_status_names_are_read(self) -> None:
        self.assertEqual(
            cited_status_names("ScratchStatus::io_error and "
                               "pineforge::detail::ScratchStatus::programming_error"),
            {"io_error", "programming_error"})

    def test_waiver_scratch_status_names_exist(self) -> None:
        header, allowlist = read_inputs()
        declared = scratch_status_names(header)
        exit_scratch_rows = 0
        checked_reasons = 0
        for number, raw in enumerate(allowlist.splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            # Match check_run_failure_codes.load_allowlist's four-column form.
            parts = [part.strip() for part in raw.split("|")]
            self.assertEqual(len(parts), 4, f"allowlist:{number}: expected four columns")
            self.assertTrue(all(parts), f"allowlist:{number}: empty column")
            self.assertRegex(parts[2], r"^[1-9][0-9]*$",
                             f"allowlist:{number}: invalid throw count")
            path, fragment, _, reason = parts
            is_exit_scratch = (path == "src/source/pine_adapter.cpp"
                               and fragment == "throw ExitScratchError(status);")
            if is_exit_scratch:
                exit_scratch_rows += 1
                self.assertRegex(reason, r"\bScratchStatus\b",
                                 "ExitScratchError waiver must name its status mapping")
            if not re.search(r"\bScratchStatus\b", reason):
                continue
            checked_reasons += 1
            with self.subTest(allowlist_line=number, path=path, fragment=fragment):
                cited = cited_status_names(reason)
                self.assertTrue(cited, "ScratchStatus reason names no status members")
                self.assertEqual(
                    cited - declared, set(),
                    f"allowlist:{number}: unknown ScratchStatus members: "
                    + ", ".join(sorted(cited - declared)))
        self.assertEqual(exit_scratch_rows, 1, "ExitScratchError waiver row is missing or duplicated")
        self.assertGreater(checked_reasons, 0, "no ScratchStatus waiver reason was checked")


if __name__ == "__main__":
    unittest.main()
