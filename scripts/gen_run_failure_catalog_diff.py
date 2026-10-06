#!/usr/bin/env python3
"""A published catalog's per-release diff: generated, checked, stamped, reset.

docker/run_failure_codes.json (schema pineforge-run-failure-catalog/v1) is the
run-failure vocabulary every release publishes beside its tarballs. Its diff,
docker/run_failure_codes_diff.json (schema
pineforge-run-failure-catalog-diff/v1), states exactly what the catalog
changed against the catalog of the previous release tag, so a consumer that
pins or localizes the codes reads one release's changes instead of
re-deriving them. The structure is the codegen diagnostics catalog diff's
(pineforge-diagnostics-catalog-diff/v1), and nothing below is specific to the
run-failure catalog: the paths, the schema names and the collections are
flags (--catalog, --catalog-path-in-git, --diff, --schema, --diff-schema,
--collection).

    {"schema": "pineforge-run-failure-catalog-diff/v1",
     "from": {"version": "1.2.0", "tag": "v1.2.0", "catalogSha256": null},
     "to": {"version": null, "unreleased": true, "catalogSha256": "<hex>"},
     "catalogSchema": {"before": null, "after": "pineforge-run-failure-catalog/v1"},
     "metadataChanges": [<change>...],
     "added": [{"code", "collection", "entry"}...],
     "changed": [{"code", "collection", "fields": [<change>...]}...],
     "removed": [{"code", "collection", "entry"}...],
     "deprecated": [{"code", "replacedBy"}...]}

  * `from` is the newest release tag reachable from HEAD (`git describe
    --tags --abbrev=0 --match 'v[0-9]*'`) and the SHA-256 of the raw bytes of
    the catalog at that tag: null when that tag published no catalog, and
    then every entry is `added`. `to` hashes the raw bytes of this tree's
    catalog; the checked-in diff is always `unreleased` (version null), and
    a release stamps a copy with its version (`--stamp`).
  * A <change> is {"path", "beforePresent", "afterPresent", "before",
    "after"}. The path walks every field recursively, dot-joined from the
    entry (`class`, `retryable`, `since`, `description`, `english`,
    `deprecated`, `replacedBy`, `args.<name>.kind`, `args.<name>.values`,
    `args.<name>.optional`), and an object member added or removed as a
    whole -- an argument, say -- is one change at its own path (`args.<name>`)
    with the whole value on its side. The presence flags tell an absent field
    from an explicit null; an absent side's value is null. A list is one
    value: a vocabulary list that gains or loses a value is one change of
    `args.<name>.values` with both lists, in their declared order.
  * `metadataChanges` are the same changes over the root fields that are
    neither `schema` (which `catalogSchema` states) nor a collection.
  * `added` and `removed` carry the whole entry; `changed` lists the changed
    fields of an entry both catalogs hold; `deprecated` names each entry
    deprecated in this tree that was not deprecated before, with its
    `replacedBy` (its field changes are in `changed` too). A removal is
    described, never accepted: --write exits 1 after writing it, --check and
    --stamp refuse it (a published code is deprecated, never removed).
  * Deterministic bytes: entries sorted by (collection, code), changes in the
    sorted order of their paths, object keys sorted inside every catalog
    value and list order kept, two-space indent, one final newline, and no
    timestamp, path or host.

Modes:

  --write   regenerate the checked-in diff from this tree's catalog and the
            newest release tag (`to` unreleased).
  --check   the CI source guard: recompute and compare byte for byte.
            `to.catalogSha256` must hash this tree's catalog, `from.tag` must
            be the newest release tag reachable from HEAD and
            `from.catalogSha256` the catalog at that tag, `removed` must be
            empty. In a clone without tags git cannot name the tag: when
            `from.catalogSha256` is null the diff is still recomputed in full
            (every entry added; `from.tag` itself unverified, and the note
            says so); when it is not null the check fails closed.
  --stamp --to-version X.Y.Z[-rc.N] --previous-tag vA.B.C --output PATH
            the release: everything --check holds, plus `from.tag` must be
            the previous release tag, and every added entry that states a
            `since` must state X.Y.Z (a release candidate's own X.Y.Z). Writes
            the stamped copy (`to.version` set, `unreleased` dropped) to PATH,
            never over the checked-in diff.
  --reset --tag vX.Y.Z [--output PATH]
            the state main is in right after a release: `from` is that tag
            and this tree's catalog, nothing added. The release workflow
            resets before it tags, so the tag may not exist yet; when it does,
            its catalog must be byte-identical to this tree's.
  --self-test
            drive every rule against in-memory fixtures. Every other mode
            runs it first, so a broken generator never judges.

Exit status: 0 on success, 1 on a finding, 2 when an input cannot be read --
a missing file, or git cannot read a tag the mode needs.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROG = "gen_run_failure_catalog_diff"
CATALOG = "docker/run_failure_codes.json"
DIFF = "docker/run_failure_codes_diff.json"
SCHEMA = "pineforge-run-failure-catalog/v1"
DIFF_SCHEMA = "pineforge-run-failure-catalog-diff/v1"
COLLECTIONS = ("codes",)
TAG_MATCH = "v[0-9]*"
# scripts/release_version.py's spelling: X.Y.Z or X.Y.Z-rc.N, no leading zeros.
_NUMBER = r"(?:0|[1-9][0-9]*)"
VERSION = re.compile(_NUMBER + r"\." + _NUMBER + r"\." + _NUMBER + r"(?:-rc\.[1-9][0-9]*)?")
UNRELEASED_TO = ("version", "unreleased", "catalogSha256")


class InputError(Exception):
    """An input cannot be read: exit 2."""


class Unreadable(InputError):
    """git cannot read a tag's tree or its catalog."""


class Finding(Exception):
    """A rule the diff or the catalog breaks: exit 1."""


@dataclass(frozen=True)
class Config:
    root: Path
    catalog: Path
    catalog_in_git: str
    diff: Path
    schema: str
    diff_schema: str
    collections: tuple

    def show(self, path: Path) -> str:
        try:
            return path.resolve().relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return str(path)


@dataclass
class Outcome:
    notes: list = field(default_factory=list)
    errors: list = field(default_factory=list)
    status: int = 0

    def code(self) -> int:
        return self.status or (1 if self.errors else 0)


class Git:
    """The repository's newest release tag and a file's bytes at a tag."""

    def __init__(self, root: Path):
        self.root = root

    def _run(self, *argv: str, text: bool = True):
        try:
            return subprocess.run(["git", "-C", str(self.root), *argv], capture_output=True,
                                  text=text, check=False)
        except OSError:
            return None

    def newest_tag(self) -> str | None:
        result = self._run("describe", "--tags", "--abbrev=0", "--match", TAG_MATCH, "HEAD")
        if result is None or result.returncode != 0:
            return None
        return result.stdout.strip() or None

    def has_tag(self, tag: str) -> bool:
        result = self._run("rev-parse", "--verify", "--quiet", "refs/tags/" + tag)
        return result is not None and result.returncode == 0

    def file_at(self, tag: str, path: str) -> bytes | None:
        """`path`'s bytes at `tag`; None when the tag's tree has no such file."""
        tree = self._run("rev-parse", "--verify", "--quiet", tag + "^{tree}")
        if tree is None or tree.returncode != 0:
            raise Unreadable(f"git cannot read the tree of {tag}")
        listed = self._run("ls-tree", "--name-only", "-z", tag, "--", path)
        if listed is None or listed.returncode != 0:
            raise Unreadable(f"git cannot list {path} at {tag}")
        if path not in listed.stdout.split("\0"):
            return None
        blob = self._run("cat-file", "blob", f"{tag}:{path}", text=False)
        if blob is None or blob.returncode != 0:
            raise Unreadable(f"git cannot read {path} at {tag}")
        return blob.stdout


# --------------------------------------------------------------------------
# The diff


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_tag(tag: object) -> bool:
    return isinstance(tag, str) and tag.startswith("v") and bool(VERSION.fullmatch(tag[1:]))


def tag_version(tag: str) -> str:
    return tag[1:] if tag.startswith("v") else tag


def sorted_value(value):
    """Object keys sorted at every depth; list order is the catalog's own."""
    if isinstance(value, dict):
        return {key: sorted_value(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        return [sorted_value(item) for item in value]
    return value


def _same(before, after) -> bool:
    # JSON equality, not Python's: true is not 1, and 1 is not 1.0.
    return (json.dumps(before, sort_keys=True, ensure_ascii=False)
            == json.dumps(after, sort_keys=True, ensure_ascii=False))


def _change(path: str, before_present: bool, after_present: bool, before, after) -> dict:
    return {"path": path, "beforePresent": before_present, "afterPresent": after_present,
            "before": sorted_value(before) if before_present else None,
            "after": sorted_value(after) if after_present else None}


def field_changes(before, after, prefix: str = "") -> list:
    """Every changed field from `before` to `after`, recursively (see the
    module docstring): objects are walked member by member, anything else is
    compared whole."""
    if isinstance(before, dict) and isinstance(after, dict):
        changes = []
        for key in sorted(set(before) | set(after)):
            path = f"{prefix}.{key}" if prefix else key
            if key not in before:
                changes.append(_change(path, False, True, None, after[key]))
            elif key not in after:
                changes.append(_change(path, True, False, before[key], None))
            else:
                changes.extend(field_changes(before[key], after[key], path))
        return changes
    if _same(before, after):
        return []
    return [_change(prefix, True, True, before, after)]


def parse_catalog(data: bytes, where: str, cfg: Config) -> dict:
    try:
        catalog = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise Finding(f"{where} is not JSON: {error}") from error
    if not isinstance(catalog, dict):
        raise Finding(f"{where} is not a JSON object")
    for collection in cfg.collections:
        entries = catalog.get(collection, {})
        if not isinstance(entries, dict) or not all(isinstance(entry, dict)
                                                    for entry in entries.values()):
            raise Finding(f"{where}: {collection} must be an object of objects")
    return catalog


def deprecated(entry) -> bool:
    return isinstance(entry, dict) and entry.get("deprecated") is True


def build_diff(old_bytes: bytes | None, new_bytes: bytes, from_tag: str, cfg: Config) -> dict:
    """The diff from the catalog at `from_tag` (None: that tag has none) to
    this tree's."""
    new = parse_catalog(new_bytes, cfg.show(cfg.catalog), cfg)
    if new.get("schema") != cfg.schema:
        raise Finding(f"{cfg.show(cfg.catalog)}: schema is {new.get('schema')!r}, "
                      f"expected {cfg.schema}")
    old = (parse_catalog(old_bytes, f"{cfg.catalog_in_git} at {from_tag}", cfg)
           if old_bytes is not None else None)
    root_fields = {"schema", *cfg.collections}
    metadata_before = {key: value for key, value in (old or {}).items()
                       if key not in root_fields}
    metadata_after = {key: value for key, value in new.items() if key not in root_fields}
    added, changed, removed, newly_deprecated = [], [], [], []
    for collection in sorted(cfg.collections):
        before = (old or {}).get(collection, {})
        after = new.get(collection, {})
        for code in sorted(set(before) | set(after)):
            if code not in before:
                added.append({"code": code, "collection": collection,
                              "entry": sorted_value(after[code])})
            elif code not in after:
                removed.append({"code": code, "collection": collection,
                                "entry": sorted_value(before[code])})
            else:
                fields = field_changes(before[code], after[code])
                if fields:
                    changed.append({"code": code, "collection": collection, "fields": fields})
            if code in after and deprecated(after[code]) and not deprecated(before.get(code)):
                newly_deprecated.append({"code": code, "replacedBy": sorted_value(
                    after[code].get("replacedBy"))})
    return {
        "schema": cfg.diff_schema,
        "from": {"version": tag_version(from_tag), "tag": from_tag,
                 "catalogSha256": sha256(old_bytes) if old_bytes is not None else None},
        "to": {"version": None, "unreleased": True, "catalogSha256": sha256(new_bytes)},
        "catalogSchema": {"before": old.get("schema") if old is not None else None,
                          "after": new.get("schema")},
        "metadataChanges": field_changes(metadata_before, metadata_after),
        "added": added,
        "changed": changed,
        "removed": removed,
        "deprecated": newly_deprecated,
    }


def render(diff: dict) -> str:
    return json.dumps(diff, indent=2, ensure_ascii=False) + "\n"


def summary(diff: dict) -> str:
    def count(key: str) -> int:
        value = diff.get(key)
        return len(value) if isinstance(value, list) else 0
    return (f"{count('added')} added, {count('changed')} changed, {count('removed')} removed, "
            f"{count('deprecated')} deprecated")


def read_bytes(path: Path, cfg: Config) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise InputError(f"{cfg.show(path)}: {error.strerror or error}") from error


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def removal_error(diff: dict) -> str:
    names = ", ".join(f"{item['collection']}.{item['code']}" for item in diff["removed"])
    return (f"removed is not empty ({names}): a published entry is never removed or "
            "renamed; keep it and mark it deprecated with replacedBy")


# --------------------------------------------------------------------------
# Modes


def do_write(cfg: Config, git) -> Outcome:
    outcome = Outcome()
    new_bytes = read_bytes(cfg.catalog, cfg)
    tag = git.newest_tag()
    if tag is None:
        raise InputError("no release tag v* is reachable from HEAD: fetch the tags "
                         "(git fetch --tags) or clone with history")
    diff = build_diff(git.file_at(tag, cfg.catalog_in_git), new_bytes, tag, cfg)
    write_text(cfg.diff, render(diff))
    outcome.notes.append(f"wrote {cfg.show(cfg.diff)}: {tag} -> unreleased: {summary(diff)}")
    if diff["removed"]:
        outcome.errors.append(removal_error(diff))
    return outcome


def do_check(cfg: Config, git) -> Outcome:
    outcome = Outcome()
    errors = outcome.errors
    new_bytes = read_bytes(cfg.catalog, cfg)
    shown = cfg.show(cfg.diff)
    try:
        text = cfg.diff.read_text(encoding="utf-8")
    except OSError as error:
        raise InputError(f"{shown}: {error.strerror or error}; run --write") from error
    try:
        diff = json.loads(text)
    except json.JSONDecodeError as error:
        errors.append(f"{shown} is not JSON ({error}); run --write")
        return outcome
    if not isinstance(diff, dict):
        errors.append(f"{shown} is not a JSON object; run --write")
        return outcome
    if diff.get("schema") != cfg.diff_schema:
        errors.append(f"{shown}: schema is {diff.get('schema')!r}, expected {cfg.diff_schema}")
    recorded_from = diff.get("from") if isinstance(diff.get("from"), dict) else {}
    recorded_to = diff.get("to") if isinstance(diff.get("to"), dict) else {}
    actual = sha256(new_bytes)
    if recorded_to.get("catalogSha256") != actual:
        errors.append(f"to.catalogSha256 is {recorded_to.get('catalogSha256')!r}, but "
                      f"{cfg.show(cfg.catalog)} hashes to {actual}: the catalog changed after "
                      "the diff was written; run --write")
    if (tuple(recorded_to) != UNRELEASED_TO or recorded_to.get("version") is not None
            or recorded_to.get("unreleased") is not True):
        errors.append("to must be {version: null, unreleased: true, catalogSha256}: the "
                      "checked-in diff is the unreleased one (a stamped diff is a release "
                      "asset, never a source file)")
    recorded_tag = recorded_from.get("tag")
    recorded_sha = recorded_from.get("catalogSha256")
    tag = git.newest_tag()
    old_bytes = None
    reason = "git names no release tag v* reachable from HEAD (a clone without tags)"
    if tag is not None:
        if recorded_tag != tag:
            errors.append(f"from.tag is {recorded_tag!r}, but the newest release tag reachable "
                          f"from HEAD is {tag}: run --write (a release's --reset moves it to "
                          "the new tag)")
        try:
            old_bytes = git.file_at(tag, cfg.catalog_in_git)
        except Unreadable as error:
            reason = f"{error} (a clone without its history)"
            tag = None
    if tag is None:
        if recorded_sha is not None:
            outcome.status = 2
            errors.append(f"{reason}, and from.catalogSha256 is {recorded_sha}: the catalog "
                          f"{recorded_tag} published cannot be read, so the diff cannot be "
                          "verified. Fetch the tags (git fetch --tags; actions/checkout with "
                          "fetch-depth: 0)")
            return outcome
        if not is_tag(recorded_tag):
            errors.append(f"from.tag {recorded_tag!r} is not a release tag vX.Y.Z[-rc.N]")
            return outcome
        tag = recorded_tag
        outcome.notes.append(
            f"{reason}: from.tag {recorded_tag} is not verified against git, but "
            "from.catalogSha256 is null (that tag published no catalog), so the diff was "
            "recomputed in full without git (every entry added)")
    expected = build_diff(old_bytes, new_bytes, tag, cfg)
    if recorded_sha != expected["from"]["catalogSha256"]:
        actual_from = expected["from"]["catalogSha256"] or "nothing (it has no catalog)"
        errors.append(f"from.catalogSha256 is {recorded_sha!r}, but {cfg.catalog_in_git} at "
                      f"{tag} hashes to {actual_from}; run --write")
    if expected["removed"]:
        errors.append(removal_error(expected))
    if render(expected) != text and not errors:
        errors.append(f"{shown} is stale: {cfg.show(cfg.catalog)} against {tag} gives "
                      f"{summary(expected)} and other bytes; run --write")
    if not errors:
        outcome.notes.append(f"{shown}: {tag} -> unreleased: {summary(expected)}; catalog "
                             f"sha256 {actual[:12]}, byte-identical to its regeneration ... OK")
    return outcome


def do_stamp(cfg: Config, git, to_version: str, previous_tag: str, output: Path) -> Outcome:
    if not VERSION.fullmatch(to_version):
        raise InputError(f"--to-version {to_version!r} is not X.Y.Z or X.Y.Z-rc.N")
    if not is_tag(previous_tag):
        raise InputError(f"--previous-tag {previous_tag!r} is not vX.Y.Z or vX.Y.Z-rc.N")
    if output.resolve() == cfg.diff.resolve():
        raise InputError("--output is the checked-in diff: the stamped diff is a release "
                         "asset, written beside it, never over it")
    outcome = do_check(cfg, git)
    if outcome.status or not cfg.diff.is_file():
        return outcome
    outcome.notes = [note for note in outcome.notes if not note.endswith("... OK")]
    errors = outcome.errors
    try:
        diff = json.loads(cfg.diff.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return outcome
    recorded_from = diff.get("from") if isinstance(diff.get("from"), dict) else {}
    if recorded_from.get("tag") != previous_tag:
        errors.append(f"from.tag is {recorded_from.get('tag')!r}, not the previous release tag "
                      f"{previous_tag}: the diff must start where the last release ended")
    if "v" + to_version == previous_tag:
        errors.append(f"--to-version {to_version} is the previous release itself")
    if diff.get("removed"):
        errors.append(removal_error(diff))
    release = to_version.split("-", 1)[0]
    for item in diff.get("added") or []:
        entry = item.get("entry") if isinstance(item, dict) else None
        if isinstance(entry, dict) and "since" in entry and entry["since"] != release:
            errors.append(f"added {item.get('collection')} entry {item.get('code')}: since is "
                          f"{entry['since']!r}, but this release is {release}")
    if errors:
        return outcome
    stamped = dict(diff)
    stamped["to"] = {"version": to_version, "catalogSha256": diff["to"]["catalogSha256"]}
    write_text(output, render(stamped))
    outcome.notes.append(f"wrote {cfg.show(output)}: {previous_tag} -> {to_version}: "
                         f"{summary(stamped)}")
    return outcome


def do_reset(cfg: Config, git, tag: str, output: Path | None = None) -> Outcome:
    if not is_tag(tag):
        raise InputError(f"--tag {tag!r} is not vX.Y.Z or vX.Y.Z-rc.N")
    outcome = Outcome()
    new_bytes = read_bytes(cfg.catalog, cfg)
    if git.has_tag(tag):
        if git.file_at(tag, cfg.catalog_in_git) != new_bytes:
            outcome.errors.append(f"{cfg.catalog_in_git} at {tag} is not this tree's catalog: "
                                  "--reset describes the tree a release tags, unchanged; use "
                                  "--write")
            return outcome
        outcome.notes.append(f"{tag} carries this tree's catalog")
    else:
        outcome.notes.append(f"{tag} does not exist yet: the release tags the commit that "
                             "carries this catalog")
    diff = build_diff(new_bytes, new_bytes, tag, cfg)
    target = output if output is not None else cfg.diff
    write_text(target, render(diff))
    outcome.notes.append(f"wrote {cfg.show(target)}: reset to {tag} (catalog sha256 "
                         f"{sha256(new_bytes)[:12]}), nothing added")
    return outcome


# --------------------------------------------------------------------------
# Self-test


class FakeGit:
    """`files` maps a tag to its catalog bytes (None: the tag has none);
    `unreadable` tags exist but cannot be read; `newest` is what describe says."""

    def __init__(self, newest: str | None = None, files: dict | None = None,
                 unreadable: tuple = ()):
        self.newest = newest
        self.files = dict(files or {})
        self.unreadable = set(unreadable)

    def newest_tag(self) -> str | None:
        return self.newest

    def has_tag(self, tag: str) -> bool:
        return tag in self.files or tag in self.unreadable

    def file_at(self, tag: str, path: str) -> bytes | None:
        if tag in self.unreadable or tag not in self.files:
            raise Unreadable(f"git cannot read the tree of {tag}")
        return self.files[tag]


def _fixture(codes: dict, *, schema: str = SCHEMA, classes: dict | None = None,
             extra: dict | None = None) -> bytes:
    catalog = {"schema": schema, "classes": classes or {"input": "i", "strategy": "s"},
               "kinds": {"integer": "n", "vocab": "v"}, "codes": codes}
    catalog.update(extra or {})
    return (json.dumps(catalog, indent=1, sort_keys=True) + "\n").encode()


def self_test() -> int:
    failures: list = []

    def expect(condition: bool, what: str) -> None:
        if not condition:
            failures.append(what)

    alpha = {"class": "strategy", "retryable": False, "since": "1.3.0", "description": "a",
             "english": ["a {reason}"],
             "args": {"reason": {"kind": "vocab", "values": ["one", "two"]}}}
    beta = {"class": "input", "retryable": True, "since": "1.3.0", "description": "b",
            "english": [], "args": {}}
    gamma = {"class": "input", "retryable": False, "since": "1.3.0", "description": "c",
             "english": ["c"], "args": {}}
    base_codes = {"alpha": alpha, "beta": beta}
    base = _fixture(base_codes)

    def variant(mutate) -> bytes:
        codes = copy.deepcopy(base_codes)
        mutate(codes)
        return _fixture(codes)

    def paths(diff: dict, code: str) -> list:
        return [change["path"] for item in diff["changed"] if item["code"] == code
                for change in item["fields"]]

    with tempfile.TemporaryDirectory(prefix="pf-catalog-diff-") as temporary:
        root = Path(temporary)
        cfg = Config(root, root / CATALOG, CATALOG, root / DIFF, SCHEMA, DIFF_SCHEMA,
                     COLLECTIONS)
        build = lambda old, new, tag="v1.2.0": build_diff(old, new, tag, cfg)

        # Null from: a tag that published no catalog, so every entry is added.
        diff = build(None, base)
        expect(diff["from"] == {"version": "1.2.0", "tag": "v1.2.0", "catalogSha256": None},
               "null from: wrong from " + repr(diff["from"]))
        expect(diff["to"] == {"version": None, "unreleased": True,
                              "catalogSha256": sha256(base)}, "null from: wrong to")
        expect(diff["catalogSchema"] == {"before": None, "after": SCHEMA},
               "null from: wrong catalogSchema")
        expect([item["code"] for item in diff["added"]] == ["alpha", "beta"]
               and diff["added"][0]["entry"] == alpha and not diff["changed"],
               "null from: every entry must be added whole")
        expect([change["path"] for change in diff["metadataChanges"]] == ["classes", "kinds"]
               and not diff["metadataChanges"][0]["beforePresent"],
               "null from: classes and kinds are metadata added whole")

        # Unchanged catalog: nothing to say.
        diff = build(base, base)
        expect(not any(diff[key] for key in ("metadataChanges", "added", "changed",
                                             "removed", "deprecated"))
               and diff["from"]["catalogSha256"] == diff["to"]["catalogSha256"],
               "an unchanged catalog produced changes")

        # Additions.
        diff = build(base, variant(lambda codes: codes.update(gamma=gamma)))
        expect([item["code"] for item in diff["added"]] == ["gamma"] and not diff["changed"],
               "an added code is not exactly one addition")

        # A kind change (the vocab list goes with it).
        diff = build(base, variant(lambda codes: codes["alpha"]["args"].update(
            reason={"kind": "identifier"})))
        fields = diff["changed"][0]["fields"] if diff["changed"] else []
        expect([f["path"] for f in fields] == ["args.reason.kind", "args.reason.values"]
               and fields[0]["before"] == "vocab" and fields[0]["after"] == "identifier"
               and fields[1]["beforePresent"] and not fields[1]["afterPresent"]
               and fields[1]["after"] is None,
               "a kind change was not described: " + repr(fields))

        # A value added, a value removed, both: one change of the list each.
        for values in (["one", "two", "three"], ["one"], ["two", "three"], ["two", "one"]):
            diff = build(base, variant(lambda codes, v=values: codes["alpha"]["args"]["reason"]
                                       .update(values=v)))
            fields = diff["changed"][0]["fields"] if diff["changed"] else []
            expect(len(fields) == 1 and fields[0]["path"] == "args.reason.values"
                   and fields[0]["before"] == ["one", "two"] and fields[0]["after"] == values,
                   f"values {values} were not one change of the list: {fields!r}")

        # An argument added, an argument removed: one change at args.<name>.
        diff = build(base, variant(lambda codes: codes["alpha"]["args"].update(
            extra={"kind": "integer", "optional": True})))
        expect(paths(diff, "alpha") == ["args.extra"]
               and not diff["changed"][0]["fields"][0]["beforePresent"]
               and diff["changed"][0]["fields"][0]["after"] == {"kind": "integer",
                                                                "optional": True},
               "an added argument was not one change: " + repr(diff["changed"]))
        diff = build(base, variant(lambda codes: codes["alpha"]["args"].pop("reason")))
        expect(paths(diff, "alpha") == ["args.reason"]
               and not diff["changed"][0]["fields"][0]["afterPresent"],
               "a removed argument was not one change")
        diff = build(base, variant(lambda codes: codes["alpha"]["args"]["reason"].update(
            optional=True)))
        expect(paths(diff, "alpha") == ["args.reason.optional"],
               "an argument turned optional was not described")

        # Scalars and templates.
        diff = build(base, variant(lambda codes: codes["beta"].update(
            retryable=False, english=["b"], description="b2")))
        expect(paths(diff, "beta") == ["description", "english", "retryable"],
               "scalar and template changes: " + repr(paths(diff, "beta")))
        diff = build(base, variant(lambda codes: codes["beta"].update(retryable=1)))
        expect(paths(diff, "beta") == ["retryable"], "true and 1 were taken as equal")

        # Deprecation with replacedBy: listed once, its fields changed too.
        deprecate = variant(lambda codes: codes["alpha"].update(deprecated=True,
                                                                replacedBy=["beta"]))
        diff = build(base, deprecate)
        expect(diff["deprecated"] == [{"code": "alpha", "replacedBy": ["beta"]}]
               and paths(diff, "alpha") == ["deprecated", "replacedBy"],
               "a deprecation was not described: " + repr(diff["deprecated"]))
        diff = build(deprecate, deprecate)
        expect(not diff["deprecated"], "an old deprecation was listed again")

        # Metadata and schema.
        diff = build(base, _fixture(base_codes, classes={"input": "i2", "strategy": "s"},
                                    extra={"note": "n"}))
        expect([change["path"] for change in diff["metadataChanges"]]
               == ["classes.input", "note"], "metadata changes: "
               + repr(diff["metadataChanges"]))
        diff = build(_fixture(base_codes, schema="pineforge-run-failure-catalog/v0"), base)
        expect(diff["catalogSchema"] == {"before": "pineforge-run-failure-catalog/v0",
                                         "after": SCHEMA}, "a schema change was not stated")

        # Removal is described.
        removed = variant(lambda codes: codes.pop("beta"))
        diff = build(base, removed)
        expect(diff["removed"] == [{"code": "beta", "collection": "codes", "entry": beta}],
               "a removal was not described")

        # Deterministic bytes, no path.
        once, twice = render(build(None, base)), render(build(None, base))
        expect(once == twice and once.endswith("}\n") and temporary not in once,
               "the rendering is not deterministic and path-free")

        # Generic collections and schema names.
        generic = Config(root, root / CATALOG, CATALOG, root / DIFF, "other/v2",
                         "other-diff/v1", ("codes", "fragments"))
        with_fragments = (json.dumps({"schema": "other/v2", "codes": {"E1": {"m": "x"}},
                                      "fragments": {"F1": {"m": "y"}}}) + "\n").encode()
        diff = build_diff(None, with_fragments, "v2.0.0", generic)
        expect(diff["schema"] == "other-diff/v1"
               and [(item["collection"], item["code"]) for item in diff["added"]]
               == [("codes", "E1"), ("fragments", "F1")] and not diff["metadataChanges"],
               "collections and schema names are not generic: " + repr(diff["added"]))

        # The modes, on files.
        def reset_files(catalog: bytes) -> None:
            (root / "docker").mkdir(exist_ok=True)
            cfg.catalog.write_bytes(catalog)
            if cfg.diff.exists():
                cfg.diff.unlink()

        def joined(outcome: Outcome) -> str:
            return " | ".join(outcome.errors + outcome.notes)

        reset_files(base)
        published = FakeGit("v1.2.0", {"v1.2.0": None})
        written = do_write(cfg, published)
        expect(written.code() == 0 and cfg.diff.is_file(), "--write failed: " + joined(written))
        checked = do_check(cfg, published)
        expect(checked.code() == 0, "--check refused a fresh diff: " + joined(checked))

        # A stale diff: the catalog moved after --write.
        cfg.catalog.write_bytes(variant(lambda codes: codes.update(gamma=gamma)))
        checked = do_check(cfg, published)
        expect(checked.code() == 1 and "to.catalogSha256" in joined(checked),
               "--check passed a catalog changed after the diff: " + joined(checked))
        do_write(cfg, published)
        expect(do_check(cfg, published).code() == 0, "--check refused a rewritten diff")

        # A bad sha in the checked-in diff, either side.
        good = cfg.diff.read_text(encoding="utf-8")
        tampered = json.loads(good)
        tampered["to"]["catalogSha256"] = "0" * 64
        cfg.diff.write_text(render(tampered), encoding="utf-8")
        expect("to.catalogSha256" in joined(do_check(cfg, published)), "a bad to sha passed")
        tampered = json.loads(good)
        tampered["from"]["catalogSha256"] = "0" * 64
        cfg.diff.write_text(render(tampered), encoding="utf-8")
        checked = do_check(cfg, published)
        expect(checked.code() == 1 and "from.catalogSha256" in joined(checked),
               "a bad from sha passed: " + joined(checked))
        tampered = json.loads(good)
        tampered["to"] = {"version": "1.3.0", "catalogSha256": tampered["to"]["catalogSha256"]}
        cfg.diff.write_text(render(tampered), encoding="utf-8")
        expect(do_check(cfg, published).code() == 1, "a stamped diff passed as the checked-in one")
        tampered = json.loads(good)
        tampered["added"] = tampered["added"][1:]
        cfg.diff.write_text(render(tampered), encoding="utf-8")
        checked = do_check(cfg, published)
        expect(checked.code() == 1 and "stale" in joined(checked),
               "a diff missing an addition passed: " + joined(checked))
        cfg.diff.write_text(good, encoding="utf-8")

        # The wrong from tag: a release happened and the diff was not reset.
        later = FakeGit("v1.3.0", {"v1.2.0": None, "v1.3.0": base})
        checked = do_check(cfg, later)
        expect(checked.code() == 1 and "from.tag" in joined(checked),
               "a diff from an old tag passed: " + joined(checked))

        # Offline: no tag reachable. Null from recomputes; a real from fails closed.
        offline = FakeGit(None)
        checked = do_check(cfg, offline)
        expect(checked.code() == 0 and "recomputed in full" in joined(checked),
               "the null-from offline path failed: " + joined(checked))
        checked = do_check(cfg, FakeGit("v1.2.0", unreadable=("v1.2.0",)))
        expect(checked.code() == 0 and "recomputed in full" in joined(checked),
               "an unreadable tag with a null from did not recompute: " + joined(checked))
        expect(do_write(cfg, published).code() == 0, "--write failed")

        # Removal: --write writes it and exits 1, --check and --stamp refuse it.
        reset_files(base)
        tagged = FakeGit("v1.2.0", {"v1.2.0": base})
        cfg.catalog.write_bytes(removed)
        written = do_write(cfg, tagged)
        expect(written.code() == 1 and "removed is not empty" in joined(written)
               and cfg.diff.is_file(), "--write accepted a removal: " + joined(written))
        expect("removed is not empty" in joined(do_check(cfg, tagged)),
               "--check accepted a removal")
        stamped = do_stamp(cfg, tagged, "1.3.0", "v1.2.0", root / "out" / "stamped.json")
        expect(stamped.code() == 1 and "removed is not empty" in joined(stamped)
               and not (root / "out" / "stamped.json").exists(),
               "--stamp accepted a removal: " + joined(stamped))

        # A real from that git cannot read: fail closed.
        cfg.catalog.write_bytes(base)
        do_write(cfg, tagged)
        for blind in (FakeGit(None), FakeGit("v1.2.0", unreadable=("v1.2.0",))):
            checked = do_check(cfg, blind)
            expect(checked.code() == 2 and "Fetch the tags" in joined(checked),
                   "an unverifiable published from passed: " + joined(checked))

        # --stamp: a release, a release candidate, and each refusal.
        reset_files(variant(lambda codes: codes.update(gamma=gamma)))
        do_write(cfg, tagged)
        before = cfg.diff.read_text(encoding="utf-8")
        output = root / "out" / "run_failure_codes_diff-v1.3.0.json"
        stamped = do_stamp(cfg, tagged, "1.3.0", "v1.2.0", output)
        result = json.loads(output.read_text(encoding="utf-8")) if output.is_file() else {}
        expect(stamped.code() == 0
               and result.get("to") == {"version": "1.3.0",
                                        "catalogSha256": json.loads(before)["to"]
                                        ["catalogSha256"]}
               and {key: value for key, value in result.items() if key != "to"}
               == {key: value for key, value in json.loads(before).items() if key != "to"}
               and cfg.diff.read_text(encoding="utf-8") == before,
               "--stamp did not write the stamped copy beside the diff: " + joined(stamped))
        expect(do_stamp(cfg, tagged, "1.3.0-rc.1", "v1.2.0", output).code() == 0,
               "a release candidate of the codes' release was refused")
        stamped = do_stamp(cfg, tagged, "1.4.0", "v1.2.0", output)
        expect(stamped.code() == 1 and "since" in joined(stamped),
               "a since mismatch passed: " + joined(stamped))
        stamped = do_stamp(cfg, tagged, "1.3.0", "v1.1.0", output)
        expect(stamped.code() == 1 and "previous release tag" in joined(stamped),
               "a wrong previous tag passed: " + joined(stamped))
        cfg.catalog.write_bytes(variant(lambda codes: codes.update(gamma=gamma, delta=gamma)))
        stamped = do_stamp(cfg, tagged, "1.3.0", "v1.2.0", output)
        expect(stamped.code() == 1 and "to.catalogSha256" in joined(stamped),
               "--stamp passed a catalog changed after the diff: " + joined(stamped))
        try:
            do_stamp(cfg, tagged, "1.3.0", "v1.2.0", cfg.diff)
            failures.append("--stamp wrote over the checked-in diff")
        except InputError:
            pass
        for bad in (("1.3", "v1.2.0"), ("1.3.0", "1.2.0")):
            try:
                do_stamp(cfg, tagged, bad[0], bad[1], output)
                failures.append(f"--stamp accepted {bad}")
            except InputError:
                pass

        # --reset before the tag exists, then main right after the release.
        released = variant(lambda codes: codes.update(gamma=gamma))
        reset_files(released)
        reset = do_reset(cfg, tagged, "v1.3.0")
        diff = json.loads(cfg.diff.read_text(encoding="utf-8")) if cfg.diff.is_file() else {}
        expect(reset.code() == 0 and diff.get("from") == {"version": "1.3.0", "tag": "v1.3.0",
                                                          "catalogSha256": sha256(released)}
               and diff.get("to", {}).get("catalogSha256") == sha256(released)
               and not any(diff.get(key) for key in ("metadataChanges", "added", "changed",
                                                      "removed", "deprecated")),
               "--reset did not describe the release: " + joined(reset))
        after_release = FakeGit("v1.3.0", {"v1.2.0": base, "v1.3.0": released})
        checked = do_check(cfg, after_release)
        expect(checked.code() == 0, "main fails --check right after a release: "
               + joined(checked))
        expect(cfg.diff.read_text(encoding="utf-8")
               == render(build_diff(released, released, "v1.3.0", cfg)),
               "--reset differs from what --write gives right after the release")
        reset = do_reset(cfg, after_release, "v1.3.0", root / "out" / "again.json")
        expect(reset.code() == 0 and "carries this tree's catalog" in joined(reset),
               "--reset of an existing tag with this catalog failed: " + joined(reset))
        cfg.catalog.write_bytes(base)
        expect(do_reset(cfg, after_release, "v1.3.0").code() == 1,
               "--reset accepted a tag whose catalog is not this tree's")
        try:
            do_reset(cfg, after_release, "1.3.0")
            failures.append("--reset accepted a tag without its v")
        except InputError:
            pass

    for failure in failures:
        print(f"{PROG}: self-test: {failure}", file=sys.stderr)
    return 1 if failures else 0


# --------------------------------------------------------------------------


def config_from(args: argparse.Namespace) -> Config:
    root = args.root.resolve()
    catalog = args.catalog if args.catalog.is_absolute() else root / args.catalog
    diff = args.diff if args.diff.is_absolute() else root / args.diff
    in_git = args.catalog_path_in_git
    if in_git is None:
        try:
            in_git = catalog.resolve().relative_to(root).as_posix()
        except ValueError:
            raise InputError("--catalog lies outside --root: name its repository path "
                             "with --catalog-path-in-git") from None
    collections = tuple(args.collection) if args.collection else COLLECTIONS
    return Config(root, catalog, in_git, diff, args.schema, args.diff_schema, collections)


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--write", action="store_true", help="regenerate the checked-in diff")
    modes.add_argument("--check", action="store_true",
                       help="recompute the diff and compare it byte for byte (CI)")
    modes.add_argument("--stamp", action="store_true",
                       help="validate and write the release's stamped copy (release)")
    modes.add_argument("--reset", action="store_true",
                       help="rewrite the diff to start at a release tag (release)")
    modes.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="the repository (default: this script's)")
    parser.add_argument("--catalog", type=Path, default=Path(CATALOG),
                        help="the catalog, relative to --root (default: %(default)s)")
    parser.add_argument("--catalog-path-in-git", default=None,
                        help="the catalog's path inside a release tag's tree (default: "
                             "--catalog's path under --root)")
    parser.add_argument("--diff", type=Path, default=Path(DIFF),
                        help="the checked-in diff, relative to --root (default: %(default)s)")
    parser.add_argument("--schema", default=SCHEMA,
                        help="the catalog's schema (default: %(default)s)")
    parser.add_argument("--diff-schema", default=DIFF_SCHEMA,
                        help="the diff's schema (default: %(default)s)")
    parser.add_argument("--collection", action="append", default=None,
                        help="a catalog member whose entries are diffed one by one; "
                             "repeatable (default: codes)")
    parser.add_argument("--to-version", help="--stamp: the release, X.Y.Z or X.Y.Z-rc.N")
    parser.add_argument("--previous-tag", help="--stamp: the release tag the diff starts at")
    parser.add_argument("--tag", help="--reset: the release tag, vX.Y.Z or vX.Y.Z-rc.N")
    parser.add_argument("--output", type=Path,
                        help="--stamp: where the stamped copy goes (required); --reset: "
                             "where the reset diff goes (default: the checked-in diff)")
    args = parser.parse_args(argv)
    if args.stamp and not (args.to_version and args.previous_tag and args.output):
        parser.error("--stamp needs --to-version, --previous-tag and --output")
    if args.reset and not args.tag:
        parser.error("--reset needs --tag")
    if args.self_test:
        code = self_test()
        if not code:
            print(f"{PROG}: self-test OK")
        return code
    if self_test():
        return 1
    try:
        cfg = config_from(args)
        git = Git(cfg.root)
        if args.write:
            outcome = do_write(cfg, git)
        elif args.check:
            outcome = do_check(cfg, git)
        elif args.stamp:
            output = args.output if args.output.is_absolute() else Path.cwd() / args.output
            outcome = do_stamp(cfg, git, args.to_version, args.previous_tag, output)
        else:
            output = None
            if args.output is not None:
                output = args.output if args.output.is_absolute() else Path.cwd() / args.output
            outcome = do_reset(cfg, git, args.tag, output)
    except InputError as error:
        print(f"{PROG}: {error}", file=sys.stderr)
        return 2
    except Finding as error:
        print(f"{PROG}: {error}", file=sys.stderr)
        return 1
    for note in outcome.notes:
        print(f"{PROG}: {note}")
    for error in outcome.errors:
        print(f"{PROG}: {error}", file=sys.stderr)
    return outcome.code()


if __name__ == "__main__":
    raise SystemExit(main())
