#!/usr/bin/env python3
"""Generate and check the engine cap/refusal/default table.

The manifest describes the meaning and ownership of each row. Values and
ScratchResource vocabulary are read from source files every time, so the
generated page is a checked-in record of the current source.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path("scripts/cap_table_rows.json")
MANIFEST_HEADER = Path("include/pineforge/detail/scratch_tables.hpp")
PAGE = Path("docs/pages/caps-and-defaults.md")
BEGIN = "<!-- BEGIN generated cap table: python3 scripts/gen_cap_table.py --write -->"
END = "<!-- END generated cap table -->"

INTRO = """# Caps, refusals and defaults {#caps_and_defaults}

This table is generated from the engine source and the small manifest beside
the generator. It describes the limits and sizing defaults a host needs when
admitting a run. `0` means unlimited only where the row says so. `RLIMIT`
settings and `memory_limit` are host controls; the engine does not enforce
them.

"""

CONSTANT_RE = re.compile(
    r"(?ms)^[ \t]*(?:(?:inline|static)\s+)*constexpr\b"
    r"(?:(?!;).)*?\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"(?P<value>[^;]+);"
)
FIELD_RE = re.compile(
    r"(?m)^[ \t]*(?:[A-Za-z_][A-Za-z0-9_:<>]*[ \t]+)+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(?P<value>[^;]+);"
)
RESOURCE_RE = re.compile(r"enum\s+class\s+ScratchResource\b[^{}]*\{(?P<body>.*?)\};", re.S)
RESOURCE_ITEM_RE = re.compile(r"(?m)^[ \t]*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^,]*)?,?")


def _load_manifest(root: Path) -> dict:
    path = root / MANIFEST
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {MANIFEST}: {error}") from error
    if not isinstance(value, dict) or not isinstance(value.get("rows"), list):
        raise ValueError(f"{MANIFEST} must contain a rows array")
    return value


def _source_texts(root: Path, rows: list[dict]) -> dict[Path, str]:
    paths = {Path(row["header"]) for row in rows if row.get("header")}
    if any(isinstance(row.get("constant"), str)
           and row["constant"].startswith("default_scratch_manifest(bars).")
           for row in rows):
        paths.add(MANIFEST_HEADER)
    resource_header = _load_manifest(root).get("resource_header")
    if resource_header:
        paths.add(Path(resource_header))
    texts: dict[Path, str] = {}
    for path in paths:
        full = root / path
        try:
            texts[path] = full.read_text(encoding="utf-8")
        except OSError as error:
            raise ValueError(f"cannot read source {path}: {error}") from error
    return texts


def _constant_value(text: str, name: str) -> tuple[str, int] | None:
    matches = [match for match in CONSTANT_RE.finditer(text)
               if match.group("name") == name]
    if not matches:
        return None
    match = matches[0]
    line = text.count("\n", 0, match.start()) + 1
    return " ".join(match.group("value").split()), line


def _struct_fields(text: str, struct_name: str) -> dict[str, tuple[str, int]]:
    struct = re.search(
        rf"\bstruct\s+{re.escape(struct_name)}\s*\{{(?P<body>.*?)^[ \t]*\}};",
        text,
        re.S | re.M,
    )
    if not struct:
        return {}
    fields = {}
    for match in FIELD_RE.finditer(struct.group("body")):
        line = text.count("\n", 0, struct.start("body") + match.start()) + 1
        fields[match.group("name")] = " ".join(match.group("value").split()), line
    return fields


def _struct_field_value(text: str, struct_name: str, field: str) -> tuple[str, int] | None:
    return _struct_fields(text, struct_name).get(field)


def _manifest_function_values(text: str, manifest_text: str) -> dict[str, tuple[str, int]]:
    function = re.search(
        r"default_scratch_manifest\s*\(\s*std::uint64_t\s+bars\s*\)\s*\{"
        r"(?P<body>.*?)^[ \t]*\}[ \t]*$",
        text,
        re.S | re.M,
    )
    if not function:
        return {}
    body = function.group("body")
    returned = re.search(r"\breturn\s*\{(?P<values>.*?)\}\s*;", body, re.S)
    if not returned:
        return {}
    # Keep offsets intact when removing comments so each expression cites its
    # actual source line, rather than an offset relative to the function body.
    aggregate = re.sub(r"//[^\n]*", lambda match: " " * len(match.group()),
                       returned.group("values"))
    values: list[tuple[str, int]] = []
    start = 0
    depth = 0
    for index, char in enumerate(aggregate + ","):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if char == "," and depth == 0:
            fragment = aggregate[start:index]
            if fragment.strip():
                offset = (function.start("body") + returned.start("values") + start
                          + len(fragment) - len(fragment.lstrip()))
                line = text.count("\n", 0, offset) + 1
                values.append((" ".join(fragment.split()), line))
            elif index < len(aggregate):
                raise ValueError("default_scratch_manifest(bars) has an empty initializer")
            start = index + 1
    fields = _struct_fields(manifest_text, "ScratchManifest")
    if not fields:
        raise ValueError(f"ScratchManifest fields are missing from {MANIFEST_HEADER}")
    if depth != 0 or len(fields) != len(values):
        raise ValueError("default_scratch_manifest(bars) initializer does not match "
                         f"ScratchManifest: {len(values)} values for {len(fields)} fields")
    return dict(zip(fields, values))


def _manifest_noarg_r_value(text: str) -> tuple[str, int] | None:
    function = re.search(
        r"default_scratch_manifest\s*\(\s*\)\s*\{(?P<body>.*?)\n\s*\}", text, re.S
    )
    if not function:
        return None
    assignment = re.search(r"\bmanifest\.R\s*=\s*([^;]+);", function.group("body"))
    if not assignment:
        return None
    line = text.count("\n", 0, function.start("body") + assignment.start()) + 1
    return " ".join(assignment.group(1).split()), line


def _source_value(text: str, symbol: str, manifest_text: str = "") -> tuple[str, int] | None:
    """Resolve a manifest symbol without evaluating it.

    The two selector forms beyond a C++ constant are deliberately narrow:
    ``Shape::field`` reads a default member initializer and
    ``default_scratch_manifest(bars).FIELD`` reads the returned aggregate.
    They keep the source expression visible in the generated record.
    """
    if symbol.startswith("literal:"):
        needle = symbol.removeprefix("literal:")
        match = re.search(re.escape(needle), text)
        if match:
            return needle, text.count("\n", 0, match.start()) + 1
        return None
    if symbol.startswith("default_scratch_manifest(bars)."):
        return _manifest_function_values(text, manifest_text).get(symbol.rsplit(".", 1)[1])
    if symbol == "default_scratch_manifest().R":
        return _manifest_noarg_r_value(text)
    selector = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)", symbol)
    if selector:
        return _struct_field_value(text, selector.group(1), selector.group(2))
    return _constant_value(text, symbol)


def _resource_values(text: str) -> list[str]:
    match = RESOURCE_RE.search(text)
    if not match:
        raise ValueError("ScratchResource enum is missing")
    values = []
    for item in RESOURCE_ITEM_RE.finditer(match.group("body")):
        name = item.group("name")
        if name not in values:
            values.append(name)
    if not values:
        raise ValueError("ScratchResource enum has no values")
    return values


def _catalog(root: Path) -> dict:
    path = root / "docker/run_failure_codes.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read docker/run_failure_codes.json: {error}") from error


def _validate_refusal(row: dict, resources: set[str], catalog: dict) -> None:
    refusal = row.get("refusal", "none")
    if isinstance(refusal, str):
        return
    if not isinstance(refusal, dict) or not isinstance(refusal.get("code"), str):
        raise ValueError(f"row {row.get('name', '<unnamed>')} has an invalid refusal")
    code = refusal["code"]
    entry = catalog.get("codes", {}).get(code)
    if entry is None:
        raise ValueError(f"row {row['name']} names unknown refusal code {code}")
    resource = refusal.get("resource")
    if resource is not None:
        if resource not in resources:
            raise ValueError(f"row {row['name']} names unknown ScratchResource {resource}")
        values = entry.get("args", {}).get("resource", {}).get("values", [])
        if resource not in values:
            raise ValueError(f"row {row['name']} refusal {code} has no resource {resource}")


def _resolved_rows(root: Path) -> tuple[list[dict], list[str]]:
    manifest = _load_manifest(root)
    rows = manifest["rows"]
    texts = _source_texts(root, rows)
    resources = _resource_values(texts[Path(manifest["resource_header"])])
    catalog = _catalog(root)
    seen_names: set[str] = set()
    seen_resources: set[str] = set()
    resolved = []
    for row in rows:
        name = row.get("name")
        if not isinstance(name, str) or not name:
            raise ValueError("every cap-table row needs a name")
        if name in seen_names:
            raise ValueError(f"duplicate cap-table row {name}")
        seen_names.add(name)
        resource = row.get("resource")
        if not isinstance(resource, str) or resource not in resources:
            raise ValueError(f"row {name} has no valid ScratchResource")
        seen_resources.add(resource)
        _validate_refusal(row, set(resources), catalog)
        symbol = row.get("constant")
        value = None
        line = None
        if symbol is not None:
            if not isinstance(symbol, str) or not symbol:
                raise ValueError(f"row {name} has an invalid constant")
            source_path = Path(row["header"])
            value = _source_value(texts[source_path], symbol, texts.get(MANIFEST_HEADER, ""))
            if value is None:
                raise ValueError(f'row "{name}" names missing source constant "{symbol}"')
            value, line = value
        resolved.append({**row, "source_value": value, "source_line": line})
    missing = sorted(set(resources) - seen_resources)
    if missing:
        raise ValueError("ScratchResource values missing manifest rows: " + ", ".join(missing))
    return resolved, resources


def _cell(value: object) -> str:
    if value is None:
        return "none"
    if isinstance(value, dict):
        code = value["code"]
        resource = value.get("resource")
        return f"`{code}`" + (f" / `{resource}`" if resource else "")
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_block(rows: list[dict]) -> str:
    lines = [
        "| Row | Source value | Unit | Enforced where | Refusal | Unlimited | Who may change it |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        source = "not a source cap"
        if row["constant"] is not None:
            source = f"`{_cell(row['source_value'])}` ({row['header']}:{row['source_line']})"
        lines.append(
            f"| `{row['name']}` | {source} | {_cell(row['unit'])} | "
            f"{_cell(row['enforced'])} | {_cell(row.get('refusal', 'none'))} | "
            f"{_cell(row['unlimited'])} | {_cell(row['owner'])} |"
        )
    return "\n".join(lines) + "\n"


def _replace_block(page: str, block: str) -> str:
    before, begin, rest = page.partition(BEGIN)
    if not begin:
        raise ValueError(f"{PAGE} is missing the generated-block begin marker")
    old, end, after = rest.partition(END)
    if not end:
        raise ValueError(f"{PAGE} is missing the generated-block end marker")
    return before + BEGIN + "\n\n" + block + "\n" + END + after


def _page(root: Path) -> Path:
    return root / PAGE


def _generated_page(root: Path, block: str) -> str:
    path = _page(root)
    if path.exists():
        return _replace_block(path.read_text(encoding="utf-8"), block)
    return INTRO + BEGIN + "\n\n" + block + "\n" + END + "\n"


def _check_page(root: Path, expected: str) -> list[str]:
    path = _page(root)
    if not path.exists():
        return [f"{PAGE} is absent; run --write"]
    actual = path.read_text(encoding="utf-8")
    try:
        # The generated block sits between "\n\n" after BEGIN and "\n" before END
        # (see _replace_block); hand _replace_block back exactly the block it wrote.
        inner = expected.split(BEGIN, 1)[1].split(END, 1)[0]
        expected_page = _replace_block(actual, inner[len("\n\n"):-len("\n")])
    except ValueError as error:
        return [str(error)]
    if actual == expected_page:
        return []
    actual_block = actual.partition(BEGIN)[2].partition(END)[0].strip("\n")
    expected_block = expected.partition(BEGIN)[2].partition(END)[0].strip("\n")
    actual_rows = {line.split("|", 2)[1].strip(): line
                   for line in actual_block.splitlines() if line.startswith("| `")}
    for line in expected_block.splitlines():
        if not line.startswith("| `"):
            continue
        key = line.split("|", 2)[1].strip()
        if actual_rows.get(key) != line:
            return [f'row {key} differs: page="{actual_rows.get(key, "")}" '
                    f'generated="{line}"']
    return [f'generated cap table differs from {PAGE}']


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        rows, _ = _resolved_rows(root)
        block = render_block(rows)
        generated = INTRO + BEGIN + "\n\n" + block + "\n" + END + "\n"
        if args.write:
            path = _page(root)
            page = _generated_page(root, block)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(page, encoding="utf-8")
            return 0
        findings = _check_page(root, generated)
        if findings:
            for finding in findings:
                print("cap-table: " + finding, file=sys.stderr)
            return 1
        return 0
    except ValueError as error:
        print("cap-table: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
