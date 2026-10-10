#!/usr/bin/env python3
"""The run-failure code registry, held to its published catalog.

`docker/run_failure_codes.json` (schema pineforge-run-failure-catalog/v1) is
the one source of the run-failure vocabulary: every code's class, retryable
flag, arguments (kind, closed values, optional), English templates and the
release it appeared in. The engine compiles two files generated from it:

  * include/pineforge/run_failure_codes.hpp -- the closed RunFailureCode enum;
  * src/run_failure_registry.inc            -- the registry the engine
    validates every raised code and argument against.

This guard fails closed when:

  1. the catalog is not canonical (sorted keys, one-space indent, a final
     newline), names an unknown class or kind, a code outside
     ^[a-z][a-z0-9_]{2,47}$, a vocab value that is not lower snake_case
     identifiers optionally joined by dots (VALUE, at most 64 characters), a
     vocab arg without values, or an arg kind/value list a code cannot carry
     (only a vocab arg carries values; string, nullable_string and scalar
     take none);
  2. a generated file differs from what the catalog generates (run
     `--write-registry` and commit both);
  3. against the catalog of the newest release tag (`git show`), a code was
     removed or renamed, or its `since` changed (never allowed, deprecated or
     not), or an existing code's class, retryable flag, an arg's kind, or
     its value list lost a value -- unless the old code is marked
     `deprecated` with `replacedBy` naming codes that exist (a value added to
     a closed list, a new optional arg and new codes are additions). In a
     clone without tags (or when git cannot read the tag) the published
     catalog is the one the checked-in diff's `from` names, as
     scripts/gen_run_failure_catalog_diff.py --check reads it: with
     PINEFORGE_REQUIRE_RELEASE_TAGS set to any value but 0 or empty (every
     workflow that runs the guards sets it to 1) the check fails, exit 2;
     `from.tag` must be v<VERSION>; a null `from.catalogSha256` (a release
     older than 1.4.0, the first that published a catalog) leaves nothing to
     compare, and the note says so; otherwise the catalog is rebuilt by
     running the diff backwards and must hash to `from.catalogSha256`.
     Offline, neither `from.tag` nor `from.catalogSha256` is verified against
     git: only the rebuilt catalog against the sha the diff states;
  4. a `throw` in the Pine-library headers, src/source/ or the Pine-library
     kernel units -- `throw`, `std::throw_with_nested`,
     `std::rethrow_exception` or `std::rethrow_if_nested` -- is neither coded
     (`throw coded<...>(...)`,
     or `std::throw_with_nested(coded<...>(...))`) nor admitted by a row of
     scripts/run_failure_throw_allowlist.txt (path, the throw's text, how
     many such statements it admits, and the reason it is an engine
     invariant or never reaches a run).

The throw scan reads the statements written in the files it scans, with
comments and string literals (raw ones with any encoding prefix too) masked.
It does not see:

  * a throw the standard library raises for its caller -- `.at()`,
    `std::stoi`, `substr` past the end, a vector length -- nor one inside a
    helper function, or behind a macro defined in a file it does not scan;
  * a `coded<>` spelled through another namespace or an alias (flagged as
    uncoded, the safe side);
  * the kernel units (src/engine_*.cpp, src/native_*.cpp, src/c_abi.cpp,
    src/reservation_expansion.cpp, ...) and the kernel headers, among them
    include/pineforge/native_module.hpp, which it does not scan: a throw
    there is taken for an engine invariant the classifier reads by its type
    (engine_invariant for the std::logic_error family,
    engine_unclassified_error otherwise). That no script's own values reach
    one is not proven site by site.

`--self-test` drives every check against fixtures. Exit 0 on success, 1 on a
finding, 2 when an input cannot be read.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# The generator owns the diff and how it runs backwards; a guard run leaves no
# scripts/__pycache__ behind.
sys.dont_write_bytecode = True
import gen_run_failure_catalog_diff as catalog_diff  # noqa: E402  (this directory)

ROOT = Path(__file__).resolve().parents[1]
CATALOG = Path("docker/run_failure_codes.json")
CODES_HEADER = Path("include/pineforge/run_failure_codes.hpp")
REGISTRY_INC = Path("src/run_failure_registry.inc")
ALLOWLIST = Path("scripts/run_failure_throw_allowlist.txt")
DIFF = Path("docker/run_failure_codes_diff.json")
SCHEMA = "pineforge-run-failure-catalog/v1"
CLASSES = ("strategy", "strategy_limit", "no_data", "symbol_metadata", "symbol_feeds",
           "input", "unsupported", "resource", "engine_fault")
KINDS = ("identifier", "keyword", "vocab", "integer", "number", "pine_source", "symbol",
         "timeframe", "string", "nullable_string", "scalar")
NAME = re.compile(r"^[a-z][a-z0-9_]{2,47}$")
VALUE = re.compile(r"^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*$")
CODE_FIELDS = {"class", "retryable", "args", "english", "since", "description",
               "deprecated", "replacedBy"}
ARG_FIELDS = {"kind", "values", "optional"}
VERSION = re.compile(r"^\d+\.\d+\.\d+$")

# Where an uncoded throw would reach a run as a fallback code: the Pine-library
# headers generated strategies compile, the source layer, and the kernel units
# that implement Pine built-ins.
THROW_SCAN_GLOBS = (
    "include/pineforge/generic_matrix.hpp",
    "include/pineforge/map.hpp",
    "include/pineforge/drawing.hpp",
    "include/pineforge/matrix.hpp",
    "include/pineforge/ta.hpp",
    "include/pineforge/series.hpp",
    "include/pineforge/str_utils.hpp",
    "include/pineforge/session_time.hpp",
    "include/pineforge/timeframe.hpp",
    "include/pineforge/math.hpp",
    "include/pineforge/color.hpp",
    "include/pineforge/log.hpp",
    "include/pineforge/window_sum.hpp",
    "include/pineforge/ta_compare_band.hpp",
    "include/pineforge/source/*.hpp",
    "include/pineforge/compat/pine/*.hpp",
    "src/source/*.cpp",
    "src/source/*.hpp",
    "src/compat/pine/*.cpp",
    "src/matrix.cpp",
    "src/str_utils.cpp",
    "src/ta_*.cpp",
    "src/timeframe.cpp",
    "src/timezone.cpp",
    "src/session_time.cpp",
    "src/math.cpp",
)
# A throw statement, and the standard functions that throw for their caller.
THROW = re.compile(r"\b(?:throw|throw_with_nested|rethrow_exception|rethrow_if_nested)\b")
# The encoding prefixes a raw string literal may carry before its R.
RAW_PREFIXES = ("", "u8", "u", "U", "L")


class CheckError(Exception):
    pass


class InputError(Exception):
    pass


# --------------------------------------------------------------------------
# Catalog


def canonical(catalog: dict) -> str:
    return json.dumps(catalog, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def load_catalog(root: Path) -> tuple[dict, str]:
    path = root / CATALOG
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise InputError(f"{CATALOG}: {error}") from error
    try:
        return json.loads(text), text
    except json.JSONDecodeError as error:
        raise CheckError(f"{CATALOG}: not JSON: {error}") from error


def validate_catalog(catalog: dict, text: str | None = None) -> list[str]:
    errors: list[str] = []
    if text is not None and canonical(catalog) != text:
        errors.append(f"{CATALOG} is not canonical (json.dumps(indent=1, sort_keys=True, "
                      "ensure_ascii=False) + newline)")
    if catalog.get("schema") != SCHEMA:
        errors.append(f"schema must be {SCHEMA}")
    if set(catalog.get("classes", {})) != set(CLASSES):
        errors.append("classes must be exactly " + ", ".join(CLASSES))
    if set(catalog.get("kinds", {})) != set(KINDS):
        errors.append("kinds must be exactly " + ", ".join(KINDS))
    codes = catalog.get("codes")
    if not isinstance(codes, dict) or not codes:
        return errors + ["codes must be a non-empty object"]
    for name, entry in codes.items():
        where = f"code {name}"
        if not NAME.match(name):
            errors.append(f"{where}: name must match {NAME.pattern}")
        if not isinstance(entry, dict):
            errors.append(f"{where}: must be an object")
            continue
        extra = set(entry) - CODE_FIELDS
        if extra:
            errors.append(f"{where}: unknown fields {sorted(extra)}")
        if entry.get("class") not in CLASSES:
            errors.append(f"{where}: class {entry.get('class')!r} is not one of {CLASSES}")
        if not isinstance(entry.get("retryable"), bool):
            errors.append(f"{where}: retryable must be a boolean")
        if not isinstance(entry.get("since"), str) or not VERSION.match(entry["since"]):
            errors.append(f"{where}: since must be X.Y.Z")
        english = entry.get("english")
        if not isinstance(english, list) or not all(isinstance(t, str) for t in english):
            errors.append(f"{where}: english must be a list of strings")
        if not isinstance(entry.get("description"), str) or not entry["description"]:
            errors.append(f"{where}: description must be a non-empty string")
        if entry.get("deprecated") is not None:
            if entry["deprecated"] is not True:
                errors.append(f"{where}: deprecated must be true when present")
            replaced = entry.get("replacedBy")
            if (not isinstance(replaced, list) or not replaced
                    or any(r not in codes or r == name for r in replaced)):
                errors.append(f"{where}: replacedBy must name other existing codes")
        elif "replacedBy" in entry:
            errors.append(f"{where}: replacedBy needs deprecated: true")
        args = entry.get("args")
        if not isinstance(args, dict):
            errors.append(f"{where}: args must be an object")
            continue
        for arg_name, arg in args.items():
            at = f"{where} arg {arg_name}"
            if not NAME.match(arg_name) and not re.match(r"^[a-z][a-z0-9_]{1,47}$", arg_name):
                errors.append(f"{at}: name must be lower snake_case")
            if not isinstance(arg, dict):
                errors.append(f"{at}: must be an object")
                continue
            extra = set(arg) - ARG_FIELDS
            if extra:
                errors.append(f"{at}: unknown fields {sorted(extra)}")
            kind = arg.get("kind")
            if kind not in KINDS:
                errors.append(f"{at}: kind {kind!r} is not one of {KINDS}")
            values = arg.get("values")
            if kind == "vocab":
                if (not isinstance(values, list) or not values
                        or len(set(values)) != len(values)
                        or not all(isinstance(v, str) and VALUE.match(v) and len(v) <= 64
                                   for v in values)):
                    errors.append(f"{at}: a vocab arg needs a non-empty list of distinct "
                                  "lower snake_case values")
            elif values is not None:
                errors.append(f"{at}: only a vocab arg carries values")
            if "optional" in arg and arg["optional"] is not True:
                errors.append(f"{at}: optional must be true when present")
    return errors


# --------------------------------------------------------------------------
# Generated files


HEADER_BANNER = ("// Generated by scripts/check_run_failure_codes.py --write-registry from\n"
                 "// docker/run_failure_codes.json. Do not edit: edit the catalog and\n"
                 "// regenerate.\n")


def ordered_codes(catalog: dict) -> list[str]:
    return sorted(catalog["codes"])


def render_codes_header(catalog: dict) -> str:
    names = ordered_codes(catalog)
    lines = [HEADER_BANNER, "#pragma once\n", "#include <cstdint>\n",
             "namespace pineforge {\n",
             "// The closed run-failure vocabulary (docker/run_failure_codes.json),",
             "// in catalog order. `none` is no failure. The numbers are not on any",
             "// wire: the C getters answer with the names.",
             "enum class RunFailureCode : std::uint16_t {",
             "    none = 0,"]
    for number, name in enumerate(names, 1):
        lines.append(f"    {name} = {number},")
    lines += ["};", "",
              f"inline constexpr std::uint16_t kRunFailureCodeCount = {len(names) + 1};", "",
              "}  // namespace pineforge", ""]
    return "\n".join(lines)


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_registry(catalog: dict) -> str:
    names = ordered_codes(catalog)
    values: list[str] = []
    args_rows: list[str] = []
    code_rows: list[str] = []
    for name in names:
        entry = catalog["codes"][name]
        first_arg = len(args_rows)
        for arg_name in sorted(entry["args"]):
            arg = entry["args"][arg_name]
            first_value = len(values)
            for value in arg.get("values", []):
                values.append(value)
            args_rows.append(
                f"    {{{cpp_string(arg_name)}, RunFailureArgKind::{arg['kind']}, "
                f"{'true' if arg.get('optional') else 'false'}, {first_value}, "
                f"{len(values) - first_value}}},")
        code_rows.append(
            f"    {{{cpp_string(name)}, RunFailureClass::{entry['class']}, "
            f"{'true' if entry['retryable'] else 'false'}, {first_arg}, "
            f"{len(args_rows) - first_arg}}},")
    lines = [HEADER_BANNER,
             "// The registry src/run_failure.cpp validates every raised code against:",
             "// kRegistryCodes[n] describes RunFailureCode value n (row 0 is none).",
             "",
             "static constexpr const char* kRegistryValues[] = {"]
    lines += [f"    {cpp_string(value)}," for value in values] or ['    "",']
    lines += ["};", "", "static constexpr RegistryArg kRegistryArgs[] = {"]
    lines += args_rows or ['    {"", RunFailureArgKind::identifier, false, 0, 0},']
    lines += ["};", "", "static constexpr RegistryCode kRegistryCodes[] = {",
              '    {"", RunFailureClass::engine_fault, false, 0, 0},']
    lines += code_rows
    lines += ["};", "",
              f"static_assert(sizeof(kRegistryCodes) / sizeof(kRegistryCodes[0]) == {len(names) + 1},",
              '              "the registry must have one row per RunFailureCode");', ""]
    return "\n".join(lines)


def generated(catalog: dict) -> dict[Path, str]:
    return {CODES_HEADER: render_codes_header(catalog), REGISTRY_INC: render_registry(catalog)}


def check_generated(root: Path, catalog: dict) -> list[str]:
    errors = []
    for path, content in generated(catalog).items():
        try:
            current = (root / path).read_text(encoding="utf-8")
        except OSError:
            current = None
        if current != content:
            errors.append(f"{path} is stale: run scripts/check_run_failure_codes.py "
                          "--write-registry")
    return errors


# --------------------------------------------------------------------------
# Compatibility with the published catalog


def newest_release_tag(root: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*",
             "HEAD"], capture_output=True, text=True, check=False)
    except OSError:
        return None
    tag = result.stdout.strip()
    return tag if result.returncode == 0 and tag else None


def catalog_at(root: Path, tag: str) -> str | None:
    """The catalog's bytes at `tag`; None when the tag carries no catalog."""
    result = subprocess.run(["git", "-C", str(root), "show", f"{tag}:{CATALOG.as_posix()}"],
                            capture_output=True, check=False)
    if result.returncode != 0:
        exists = subprocess.run(["git", "-C", str(root), "rev-parse", "--verify", "--quiet",
                                 f"{tag}^{{commit}}"], capture_output=True, check=False)
        if exists.returncode != 0:
            raise InputError(f"release tag {tag} is not in this clone (fetch its history)")
        return None
    return result.stdout.decode("utf-8")


def published_catalog(root: Path, *,
                      require_tags: str | None = None) -> tuple[str | None, str, str | None]:
    """The newest release's catalog (None: that release has none), its label,
    and a note when it was not read from git.

    The newest `v*` tag reachable from HEAD decides. A clone without tags (a
    remote verifier's ref-less checkout), or one that cannot read the tag,
    takes the release the checked-in diff names as its `from`
    (published_without_git).
    """
    tag = newest_release_tag(root)
    reason = "no release tag v* is reachable from HEAD (a clone without tags)"
    if tag is not None:
        try:
            return catalog_at(root, tag), tag, None
        except InputError as error:
            reason = str(error)
    return published_without_git(root, reason, require_tags=require_tags)


def published_without_git(root: Path, reason: str, *,
                          require_tags: str | None = None) -> tuple[str | None, str, str | None]:
    """The published catalog when git cannot read its tag, by the generator's
    three cases: tags required (`require_tags` names what requires them:
    InputError), a null from of a release older than the first catalog
    release (nothing to compare), or the catalog rebuilt by running the
    checked-in diff backwards, held to its from.catalogSha256; from.tag must
    be v<VERSION> (CheckError when either is not)."""
    if require_tags:
        raise InputError(f"{reason}, and {require_tags} requires the release tags: fetch them "
                         "(git fetch --tags; actions/checkout with fetch-depth: 0)")
    try:
        diff = json.loads((root / DIFF).read_text(encoding="utf-8"))
        catalog = (root / CATALOG).read_bytes()
    except (OSError, ValueError) as error:
        raise InputError(f"{reason}, and {DIFF} cannot be read to name the published release "
                         f"({error})") from error
    if not isinstance(diff, dict):
        raise CheckError(f"{DIFF} is not a JSON object")
    try:
        rebuilt, tag = catalog_diff.published_offline(diff, catalog,
                                                      catalog_diff.default_config(root))
    except catalog_diff.Finding as error:
        raise CheckError(f"{reason}: {error}") from error
    except catalog_diff.InputError as error:
        raise InputError(f"{reason}: {error}") from error
    if rebuilt is None:
        return None, tag, (f"history not compared: {tag} published no catalog ({reason}; "
                           "from.tag, VERSION's, is not verified against git)")
    return (rebuilt.decode("utf-8"), f"{tag} (rebuilt from {DIFF})",
            f"history compared against the catalog {tag} published, rebuilt by running {DIFF} "
            f"backwards ({reason}; it hashes to the sha256 the diff states; neither from.tag, "
            "VERSION's, nor from.catalogSha256 is verified against git)")


def compatibility(old: dict, new: dict) -> list[str]:
    """Breaking changes from `old` (published) to `new` (this tree)."""
    errors = []
    old_codes, new_codes = old.get("codes", {}), new.get("codes", {})
    for name, before in old_codes.items():
        after = new_codes.get(name)
        if after is None:
            errors.append(f"code {name} was removed or renamed; a published code stays "
                          "(mark it deprecated with replacedBy)")
            continue
        if before.get("since") != after.get("since"):
            errors.append(f"code {name}: since changed {before.get('since')!r} -> "
                          f"{after.get('since')!r}: it names the release that first shipped "
                          "the code, which never changes")
        if after.get("deprecated"):
            continue
        for field in ("class", "retryable"):
            if before.get(field) != after.get(field):
                errors.append(f"code {name}: {field} changed {before.get(field)!r} -> "
                              f"{after.get(field)!r} without a deprecation")
        for arg_name, arg_before in before.get("args", {}).items():
            arg_after = after.get("args", {}).get(arg_name)
            if arg_after is None:
                errors.append(f"code {name}: arg {arg_name} was removed without a deprecation")
                continue
            if arg_before.get("kind") != arg_after.get("kind"):
                errors.append(f"code {name}: arg {arg_name} kind changed "
                              f"{arg_before.get('kind')!r} -> {arg_after.get('kind')!r} "
                              "without a deprecation")
            lost = set(arg_before.get("values", [])) - set(arg_after.get("values", []))
            if lost:
                errors.append(f"code {name}: arg {arg_name} lost values {sorted(lost)} "
                              "without a deprecation")
            if arg_before.get("optional") and not arg_after.get("optional"):
                errors.append(f"code {name}: arg {arg_name} became required without a "
                              "deprecation")
        for arg_name, arg_after in after.get("args", {}).items():
            if arg_name not in before.get("args", {}) and not arg_after.get("optional"):
                errors.append(f"code {name}: new arg {arg_name} must be optional (a reader "
                              "of the published catalog does not expect it)")
    return errors


# --------------------------------------------------------------------------
# Uncoded throws


def strip_comments(text: str) -> str:
    """Blank comments and string/char literals, keeping offsets and newlines."""
    out = list(text)
    i, n = 0, len(text)

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif text[i] == "'" and digit_separator(text, i):
            i += 1  # C++14 digit separator (50'000), not a character literal
        elif text.startswith('R"', i) and raw_string_prefix(text, i):
            open_paren = text.find("(", i + 2)
            close = text.find(")" + text[i + 2:open_paren] + '"', open_paren + 1)
            end = n if open_paren < 0 or close < 0 else close + len(text[i + 2:open_paren]) + 2
            blank(i + 2, min(end, n))
            i = end
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            blank(i + 1, min(j, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def digit_separator(text: str, at: int) -> bool:
    """Whether the apostrophe at `at` sits inside a numeric literal: the token
    it continues starts with a digit and a digit or letter follows it."""
    if at + 1 >= len(text) or not text[at + 1].isalnum():
        return False
    start = at
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] in "_.'"):
        start -= 1
    return start < at and text[start].isdigit()


def raw_string_prefix(text: str, at: int) -> bool:
    """Whether the `R"` at `at` opens a raw string literal: the identifier
    characters before its R are nothing, or one encoding prefix (u8R, uR, UR,
    LR); `fooR"` is an identifier and an ordinary string."""
    start = at
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
        start -= 1
    return text[start:at] in RAW_PREFIXES


def throw_statements(text: str) -> list[tuple[int, str]]:
    """Each throw -- `throw`, `std::throw_with_nested`, `std::rethrow_exception`,
    `std::rethrow_if_nested` -- and the statement text up to its `;`, by line."""
    masked = strip_comments(text)
    found = []
    for match in THROW.finditer(masked):
        end = masked.find(";", match.start())
        end = len(masked) if end < 0 else end
        line = masked.count("\n", 0, match.start()) + 1
        statement = " ".join(text[match.start():end + 1].split())
        found.append((line, statement))
    return found


def is_coded(statement: str) -> bool:
    """`throw coded<...>(...)`, or `std::throw_with_nested(coded<...>(...))`,
    whose exception derives from the coded one and so carries its code."""
    return bool(re.match(r"(?:throw\s+|throw_with_nested\s*\(\s*)(?:::)?(?:pineforge::)?"
                         r"coded\s*<", statement))


def scan_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for pattern in THROW_SCAN_GLOBS:
        files.extend(sorted(root.glob(pattern)))
    seen, unique = set(), []
    for path in files:
        if path not in seen and path.is_file():
            seen.add(path)
            unique.append(path)
    return unique


def load_allowlist(root: Path) -> list[tuple[str, str, int, str]]:
    rows = []
    path = root / ALLOWLIST
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise InputError(f"{ALLOWLIST}: {error}") from error
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in raw.split("|")]
        if len(parts) != 4 or not all(parts) or not re.fullmatch(r"[1-9][0-9]*", parts[2]):
            raise CheckError(f"{ALLOWLIST}:{number}: expected `path | throw text | count | "
                             "reason`, count the number of throw statements the row admits "
                             "(1 or more)")
        rows.append((parts[0], parts[1], int(parts[2]), parts[3]))
    return rows


def uncoded_throws(root: Path) -> list[str]:
    allow = load_allowlist(root)
    used = [0] * len(allow)
    findings = []
    for path in scan_files(root):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for line, statement in throw_statements(text):
            if is_coded(statement):
                continue
            hit = False
            for index, (allow_path, fragment, _count, _reason) in enumerate(allow):
                if allow_path == rel and fragment in statement:
                    used[index] += 1
                    hit = True
                    break
            if not hit:
                findings.append(f"{rel}:{line}: uncoded throw: {statement[:160]}")
    for (allow_path, fragment, count, _reason), matched in zip(allow, used):
        if matched == 0:
            findings.append(f"{ALLOWLIST}: stale row (no such throw): {allow_path} | {fragment}")
        elif matched != count:
            findings.append(f"{ALLOWLIST}: the row {allow_path} | {fragment} admits {count} "
                            f"throw statement(s), but {matched} match: read each one, then set "
                            "its count (a strategy-reachable throw is coded, never admitted)")
    return findings


# --------------------------------------------------------------------------


def run_checks(root: Path, *, history: bool = True,
               require_tags: str | None = None) -> tuple[list[str], list[str]]:
    """The findings, and the notes worth printing."""
    catalog, text = load_catalog(root)
    errors = validate_catalog(catalog, text)
    if errors:
        return errors, []
    errors += check_generated(root, catalog)
    notes = []
    if history:
        try:
            published, label, note = published_catalog(root, require_tags=require_tags)
        except CheckError as error:
            errors.append(str(error))
        else:
            notes += [note] if note else []
            if published is not None:
                errors += [f"against {label}: {e}"
                           for e in compatibility(json.loads(published), catalog)]
    errors += uncoded_throws(root)
    return errors, notes


def self_test() -> int:
    good = {
        "schema": SCHEMA,
        "classes": {name: "x" for name in CLASSES},
        "kinds": {name: "x" for name in KINDS},
        "codes": {
            "alpha_code": {"class": "strategy", "retryable": False, "since": "1.3.0",
                           "description": "d", "english": ["a"],
                           "args": {"reason": {"kind": "vocab", "values": ["one", "two"]}}},
            "beta_code": {"class": "input", "retryable": True, "since": "1.3.0",
                          "description": "d", "english": [], "args": {}},
        },
    }
    failures = []
    if validate_catalog(good, canonical(good)):
        failures.append("a valid catalog was refused: " + repr(validate_catalog(good)))
    bad = json.loads(json.dumps(good))
    bad["codes"]["Bad-Name"] = bad["codes"].pop("beta_code")
    if not validate_catalog(bad):
        failures.append("a bad code name passed")
    bad = json.loads(json.dumps(good))
    bad["codes"]["alpha_code"]["args"]["reason"]["values"] = []
    if not validate_catalog(bad):
        failures.append("an empty vocab passed")
    bad = json.loads(json.dumps(good))
    bad["codes"]["alpha_code"]["class"] = "nope"
    if not validate_catalog(bad):
        failures.append("an unknown class passed")
    if not validate_catalog(good, canonical(good).replace("\n", "\n ", 1)):
        failures.append("a non-canonical catalog passed")
    # compatibility
    newer = json.loads(json.dumps(good))
    newer["codes"]["alpha_code"]["args"]["reason"]["values"].append("three")
    newer["codes"]["alpha_code"]["args"]["extra"] = {"kind": "integer", "optional": True}
    newer["codes"]["gamma_code"] = newer["codes"]["beta_code"]
    if compatibility(good, newer):
        failures.append("additions were refused: " + repr(compatibility(good, newer)))
    removed = json.loads(json.dumps(good))
    del removed["codes"]["beta_code"]
    if not compatibility(good, removed):
        failures.append("a removed code passed")
    kind = json.loads(json.dumps(good))
    kind["codes"]["alpha_code"]["args"]["reason"] = {"kind": "identifier"}
    if not compatibility(good, kind):
        failures.append("a changed arg kind passed")
    lost = json.loads(json.dumps(good))
    lost["codes"]["alpha_code"]["args"]["reason"]["values"] = ["one"]
    if not compatibility(good, lost):
        failures.append("a lost vocab value passed")
    deprecated = json.loads(json.dumps(lost))
    deprecated["codes"]["alpha_code"].update({"deprecated": True, "replacedBy": ["beta_code"]})
    if compatibility(good, deprecated):
        failures.append("a deprecated change was refused")
    required = json.loads(json.dumps(good))
    required["codes"]["beta_code"]["args"]["new_arg"] = {"kind": "integer"}
    if not compatibility(good, required):
        failures.append("a new required arg passed")
    for retired in (False, True):
        moved = json.loads(json.dumps(good))
        moved["codes"]["beta_code"]["since"] = "1.9.0"
        if retired:
            moved["codes"]["beta_code"].update({"deprecated": True, "replacedBy": ["alpha_code"]})
        if not any("since changed" in error for error in compatibility(good, moved)):
            failures.append(f"a published code's since moved (deprecated: {retired})")
    # history without git: the checked-in diff run backwards, or nothing to compare
    with tempfile.TemporaryDirectory(prefix="pf-run-failure-history-") as temporary:
        root = Path(temporary)
        (root / "docker").mkdir()
        cfg = catalog_diff.default_config(root)
        published = canonical(good).encode("utf-8")

        def offline(tree: dict, diff: dict, *, require_tags: str | None = None,
                    version: str | None = None):
            (root / CATALOG).write_text(canonical(tree), encoding="utf-8")
            (root / DIFF).write_text(catalog_diff.render(diff), encoding="utf-8")
            (root / "VERSION").write_text((version or diff["from"]["version"]) + "\n",
                                          encoding="utf-8")
            try:
                return published_without_git(root, "offline", require_tags=require_tags)
            except (CheckError, InputError) as error:
                return error

        def diff_to(tree: dict, old: bytes | None, tag: str) -> dict:
            return catalog_diff.build_diff(old, canonical(tree).encode("utf-8"), tag, cfg)

        found = offline(newer, diff_to(newer, published, "v1.4.0"))
        if (not isinstance(found, tuple) or found[0] != published.decode("utf-8")
                or "rebuilt" not in found[2] or compatibility(json.loads(found[0]), newer)):
            failures.append("the published catalog was not rebuilt offline: " + repr(found))
        found = offline(removed, diff_to(removed, published, "v1.4.0"))
        if (not isinstance(found, tuple)
                or not any("beta_code was removed" in error
                           for error in compatibility(json.loads(found[0]), removed))):
            failures.append("a removal the diff states was not compared offline: " + repr(found))
        hidden = diff_to(removed, published, "v1.4.0")
        hidden["removed"] = []
        if not isinstance(offline(removed, hidden), CheckError):
            failures.append("a removal the diff hides passed offline")
        if not isinstance(offline(removed, diff_to(removed, None, "v1.4.0")), CheckError):
            failures.append("a null from of v1.4.0 passed offline")
        found = offline(newer, diff_to(newer, None, "v1.3.0"))
        if not isinstance(found, tuple) or found[0] is not None or "not compared" not in found[2]:
            failures.append("a null from of v1.3.0 was not taken offline: " + repr(found))
        if not isinstance(offline(removed, diff_to(removed, None, "v1.3.0"), version="1.4.0"),
                          CheckError):
            failures.append("a null from of v1.3.0 passed after VERSION reached 1.4.0")
        found = offline(newer, diff_to(newer, published, "v1.4.0"),
                        require_tags=f"{catalog_diff.REQUIRE_TAGS_ENV}='false'")
        if not isinstance(found, InputError) or "='false' requires" not in str(found):
            failures.append("required tags were not required: " + repr(found))
    # generated files
    header = render_codes_header(good)
    if "alpha_code = 1," not in header or "kRunFailureCodeCount = 3" not in header:
        failures.append("the codes header is wrong")
    registry = render_registry(good)
    if '"one",' not in registry or "RunFailureArgKind::vocab, false, 0, 2" not in registry:
        failures.append("the registry is wrong")
    # the generic text kinds and dotted vocab values
    kinds_ok = json.loads(json.dumps(good))
    kinds_ok["codes"]["alpha_code"]["args"].update({
        "label": {"kind": "string"},
        "session": {"kind": "nullable_string", "optional": True},
        "value": {"kind": "scalar"},
        "path": {"kind": "vocab", "values": ["counts.window_input_bars", "plain_value"]},
    })
    found = validate_catalog(kinds_ok, canonical(kinds_ok))
    if found:
        failures.append("the string, nullable_string and scalar kinds or a dotted vocab value "
                        "were refused: " + repr(found))
    for bad_value in ("Counts.window", "counts..window", "counts.", ".counts", "1counts", "a-b"):
        bad = json.loads(json.dumps(kinds_ok))
        bad["codes"]["alpha_code"]["args"]["path"]["values"] = [bad_value]
        if not validate_catalog(bad):
            failures.append(f"the vocab value {bad_value!r} passed")
    bad = json.loads(json.dumps(kinds_ok))
    bad["codes"]["alpha_code"]["args"]["label"]["values"] = ["x"]
    if not validate_catalog(bad):
        failures.append("values on a string arg passed")
    bad = json.loads(json.dumps(kinds_ok))
    bad["codes"]["alpha_code"]["args"]["label"]["kind"] = "text"
    if not validate_catalog(bad):
        failures.append("an unknown arg kind passed")
    registry = render_registry(kinds_ok)
    for needle in ("RunFailureArgKind::string, false", "RunFailureArgKind::nullable_string, true",
                   "RunFailureArgKind::scalar, false"):
        if needle not in registry:
            failures.append("the registry lacks " + needle)
    # throw scan
    with tempfile.TemporaryDirectory(prefix="pf-run-failure-codes-") as temporary:
        root = Path(temporary)
        (root / "src/source").mkdir(parents=True)
        (root / "scripts").mkdir()
        (root / "src/source/a.cpp").write_text(
            "void f() {\n"
            "  // throw std::runtime_error(\"comment\");\n"
            "  throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {}, \"x\");\n"
            "  throw std::runtime_error(\"uncoded\");\n"
            "  throw std::logic_error(\"allowed invariant\");\n"
            "  const char* s = \"throw inside a string\";\n"
            "  int cap = 50'000;\n"
            "  throw std::out_of_range(\"after a digit separator\");\n"
            "  // the scan's apostrophe\n"
            "  const char* r = R\"x(throw in a raw string)x\";\n"
            "  const char* p = u8R\"(a\"b)\";\n"
            "  throw std::out_of_range(\"after a prefixed raw string\");\n"
            "  const wchar_t* w = LR\"q(\"throw\" in a wide raw string)q\";\n"
            "  std::throw_with_nested(std::runtime_error(\"nested\"));\n"
            "  std::throw_with_nested(coded<std::runtime_error>(RunFailureCode::engine_invariant,"
            " {}, \"y\"));\n"
            "  std::rethrow_exception(pending);\n"
            "  std::rethrow_if_nested(error);\n"
            "}\n")
        (root / "src/source/b.cpp").write_text("void g() { try {} catch (...) { throw; }\n"
                                               "  try {} catch (...) { throw; } }\n")
        (root / "src/source/c.cpp").write_text("void h() { try {} catch (...) { throw; } }\n")
        (root / ALLOWLIST).write_text(
            "# comment\n"
            "src/source/a.cpp | allowed invariant | 1 | an invariant\n"
            "src/source/a.cpp | gone | 1 | stale row\n"
            "src/source/a.cpp | rethrow_exception | 1 | a rethrow\n"
            "src/source/b.cpp | throw; | 1 | one rethrow admitted, two written\n"
            "src/source/c.cpp | throw; | 3 | three rethrows admitted, one written\n")
        findings = uncoded_throws(root)
        expected = ("\"uncoded\"", "after a digit separator", "after a prefixed raw string",
                    "throw_with_nested(std::runtime_error(\"nested\"))",
                    "rethrow_if_nested(error);", "stale row",
                    "b.cpp | throw; admits 1 throw statement(s), but 2 match",
                    "c.cpp | throw; admits 3 throw statement(s), but 1 match")
        if len(findings) != len(expected) or not all(
                needle in finding for needle, finding in zip(expected, findings)):
            failures.append("the throw scan is wrong: " + repr(findings))
        for row in ("src/source/a.cpp | gone | stale row\n",
                    "src/source/a.cpp | gone | 0 | stale row\n"):
            (root / ALLOWLIST).write_text(row)
            try:
                uncoded_throws(root)
                failures.append(f"an allowlist row without a count passed: {row.strip()}")
            except CheckError:
                pass
    for failure in failures:
        print("check_run_failure_codes: self-test: " + failure, file=sys.stderr)
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--write-registry", action="store_true",
                        help="regenerate the C++ files from the catalog")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--no-history", action="store_true",
                        help="skip the comparison with the newest release tag's catalog")
    args = parser.parse_args(argv)
    notes: list[str] = []
    if args.self_test:
        return self_test()
    if self_test():
        return 1
    root = args.root.resolve()
    try:
        if args.write_registry:
            catalog, text = load_catalog(root)
            errors = validate_catalog(catalog, text)
            if errors:
                for error in errors:
                    print("check_run_failure_codes: " + error, file=sys.stderr)
                return 1
            for path, content in generated(catalog).items():
                (root / path).write_text(content, encoding="utf-8")
                print(f"check_run_failure_codes: wrote {path}")
            return 0
        errors, notes = run_checks(root, history=not args.no_history,
                                   require_tags=catalog_diff.tags_requirement())
    except InputError as error:
        print("check_run_failure_codes: " + str(error), file=sys.stderr)
        return 2
    except CheckError as error:
        print("check_run_failure_codes: " + str(error), file=sys.stderr)
        return 1
    for note in notes:
        print("check_run_failure_codes: " + note)
    if errors:
        for error in errors:
            print("check_run_failure_codes: " + error, file=sys.stderr)
        return 1
    catalog, _ = load_catalog(root)
    print(f"check_run_failure_codes: {len(catalog['codes'])} codes, registry in sync, "
          f"{len(scan_files(root))} files scanned for uncoded throws, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
